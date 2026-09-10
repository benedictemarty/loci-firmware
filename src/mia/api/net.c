/*
 * Copyright (c) 2026 bmarty
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * net.c — device réseau `N:` (voir net.h). Transport `ATGET`, décodage par
 *         net_http.c, flux en anneau.
 *
 * Vie d'une transaction :
 *
 *   open("N:https://h/p")   -> ST_SEND     (ATGET écrit vers le modem)
 *                           -> ST_RECV     (net_task pompe : CDC -> parseur -> anneau)
 *   read_xram(fd, n)        -> sert l'anneau ; -1 « rien de prêt » tant qu'il est
 *                              vide et le corps non terminé (le 6502 reste en BUSY)
 *                           -> 0 à l'EOF
 *   close(fd)               -> ST_IDLE, le lien modem retourne au passe-plat
 *
 * Ce qui est délibérément ABSENT de ce lot : POST (`ATPOST`), `tcp://`, prefix,
 * json_query, multi-connexions. Le lot 1 fait le GET, proprement.
 */

#include "api/api.h"
#include "api/net.h"
#include "api/net_http.h"
#include "usb/cdc.h"
#include "tusb.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#ifndef CFG_TUH_CDC
#define CFG_TUH_CDC 4
#endif

/* Anneau de flux. 2 Ko : assez pour absorber une rafale USB entre deux appels du
 * 6502, assez petit pour la RAM du RP2040 partagée avec le reste du firmware. */
#define NET_RING_SIZE 2048u
#define NET_RING_MASK (NET_RING_SIZE - 1u)
static_assert((NET_RING_SIZE & NET_RING_MASK) == 0, "NET_RING_SIZE doit etre une puissance de 2");

/* Le premier octet d'une réponse HTTPS tarde de plusieurs secondes (handshake
 * TLS terminé par le dongle) : mesuré le 2026-09-10, un timeout de 3 s concluait
 * à tort à un échec. D'où deux temporisations distinctes. */
#define NET_FIRST_TIMEOUT_US (20ull * 1000 * 1000)
#define NET_IDLE_TIMEOUT_US  (6ull * 1000 * 1000)
#define NET_SEND_TIMEOUT_US  (2ull * 1000 * 1000)

/* Taille max de la commande AT (URL comprise). */
#define NET_CMD_MAX 224

enum net_state {
    ST_IDLE = 0,   /* aucun fd réseau ouvert */
    ST_SEND,       /* commande AT en cours d'émission */
    ST_RECV,       /* réponse en cours de réception */
    ST_EOF,        /* corps complet ; l'anneau peut encore contenir des octets */
    ST_ERR         /* transaction échouée (net_errno) */
};

static struct {
    enum net_state state;
    int      dev;              /* interface CDC du modem, -1 si aucune */
    uint8_t  errno_api;

    char     cmd[NET_CMD_MAX];
    uint16_t cmd_len, cmd_sent;

    nh_parser hp;

    uint8_t  ring[NET_RING_SIZE];
    uint16_t head, tail;       /* head = écriture (net_task), tail = lecture (6502) */

    uint64_t deadline;
    bool     got_first;        /* au moins un octet reçu du modem */
} net;

/* ── utilitaires ── */

static int net_modem_dev(void)
{
    for (uint8_t i = 0; i < CFG_TUH_CDC; i++)
        if (tuh_cdc_mounted(i) && cdc_is_modem(i))
            return i;
    return -1;
}

static inline uint16_t ring_used(void) { return (uint16_t)((net.head - net.tail) & NET_RING_MASK); }
static inline uint16_t ring_free(void) { return (uint16_t)(NET_RING_MASK - ring_used()); }

static void net_reset(void)
{
    net.state = ST_IDLE;
    net.dev = -1;
    net.errno_api = 0;
    net.cmd_len = net.cmd_sent = 0;
    net.head = net.tail = 0;
    net.deadline = 0;
    net.got_first = false;
    nh_init(&net.hp);
}

static void net_fail(uint8_t e)
{
    net.errno_api = e;
    net.state = ST_ERR;
}

bool net_owns_modem(void)
{
    /* ST_EOF garde le lien : le « NO CARRIER » de fin arrive après le corps et
     * ne doit pas être servi au mode passe-plat comme si c'était des données. */
    return net.state == ST_SEND || net.state == ST_RECV || net.state == ST_EOF;
}

uint8_t net_errno(void) { return net.errno_api; }

bool net_is_path(const uint8_t *path)
{
    return path && (path[0] == 'N' || path[0] == 'n') && path[1] == ':';
}

/* ── ouverture ── */

int net_open(const uint8_t *path, uint8_t flags)
{
    const unsigned char RDWR = 0x03;

    if (net.state != ST_IDLE)
        return API_EMFILE;          /* canal AT unique : une transaction à la fois */

    /* Lot 1 : lecture seule. Un open en écriture demanderait ATPOST (§4.3). */
    if ((flags & RDWR) != 0x01 && (flags & RDWR) != 0x00)
        return API_EINVAL;

    const char *url = (const char *)path + 2;   /* saute « N: » */
    if (!url[0])
        return API_EINVAL;

    int dev = net_modem_dev();
    if (dev < 0)
        return API_ENODEV;          /* pas de modem monté : échec propre, détectable */

    net_reset();
    net.dev = dev;

    int n = snprintf(net.cmd, sizeof net.cmd, "ATGET%s\r", url);
    if (n <= 0 || n >= (int)sizeof net.cmd)
        return API_EINVAL;          /* URL trop longue pour la commande AT */
    net.cmd_len = (uint16_t)n;
    net.cmd_sent = 0;

    /* Purge des octets en attente (réponse précédente, bruit) : sans ça le
     * parseur démarrerait au milieu d'une trame. Borné, non bloquant. */
    uint8_t tmp[64];
    for (int guard = 0; guard < 64 && tuh_cdc_read_available((uint8_t)dev); guard++)
        tuh_cdc_read((uint8_t)dev, tmp, sizeof tmp);

    net.state = ST_SEND;
    net.deadline = time_us_64() + NET_SEND_TIMEOUT_US;
    return 0;
}

/* ── pompe de fond ──
 * Appelée par le scheduler noyau. Ne bloque jamais : elle avance d'un cran et
 * rend la main (le firmware doit continuer à servir le bus 6502). */
void net_task(void)
{
    if (net.state == ST_IDLE || net.state == ST_ERR)
        return;

    if (net.dev < 0 || !tuh_cdc_mounted((uint8_t)net.dev)) {
        net_fail(API_ENODEV);       /* dongle débranché en vol */
        return;
    }

    if (net.state == ST_SEND) {
        if (time_us_64() > net.deadline) { net_fail(API_EIO); return; }
        if (tuh_cdc_write_available((uint8_t)net.dev)) {
            net.cmd_sent += (uint16_t)tuh_cdc_write((uint8_t)net.dev,
                                                    net.cmd + net.cmd_sent,
                                                    net.cmd_len - net.cmd_sent);
            tuh_cdc_write_flush((uint8_t)net.dev);
        }
        if (net.cmd_sent >= net.cmd_len) {
            net.state = ST_RECV;
            net.deadline = time_us_64() + NET_FIRST_TIMEOUT_US;
        }
        return;
    }

    if (net.state != ST_RECV)
        return;                     /* ST_EOF : plus rien à pomper */

    /* Place libre dans l'anneau : on ne lit le modem que si on peut ranger le
     * corps produit. Le parseur consomme plus qu'il ne produit (en-têtes, tailles
     * de chunk), donc `free` est une borne sûre pour la taille d'entrée. */
    uint16_t room = ring_free();
    if (!room) {
        /* Anneau plein : le 6502 n'a pas encore lu. On NE consomme pas le lien —
         * les octets restent dans le buffer USB, ce qui applique naturellement
         * une contre-pression. */
        return;
    }

    uint32_t avail = tuh_cdc_read_available((uint8_t)net.dev);
    if (!avail) {
        uint64_t now = time_us_64();
        if (now > net.deadline)
            net_fail(API_EIO);      /* silence trop long : timeout franc */
        return;
    }

    uint8_t in[128];
    uint32_t want = avail;
    if (want > sizeof in) want = sizeof in;
    if (want > room)      want = room;      /* jamais plus que ce qu'on peut ranger */
    uint32_t got = tuh_cdc_read((uint8_t)net.dev, in, want);
    if (!got)
        return;

    net.got_first = true;
    net.deadline = time_us_64() + NET_IDLE_TIMEOUT_US;

    /* Décodage : l'anneau est circulaire, on remplit en deux temps si besoin. */
    uint32_t off = 0;
    while (off < got) {
        uint16_t h = (uint16_t)(net.head & NET_RING_MASK);
        uint16_t lin = (uint16_t)(NET_RING_SIZE - h);        /* place linéaire */
        uint16_t cap = ring_free();
        if (!cap) break;
        if (lin > cap) lin = cap;

        nh_result r = nh_feed(&net.hp, in + off, got - off, &net.ring[h], lin);
        net.head = (uint16_t)((net.head + r.produced) & NET_RING_MASK);
        off += r.consumed;

        if (r.consumed == 0 && r.produced == 0)
            break;                  /* parseur terminé ou en erreur */
    }

    if (net.hp.state == NH_ST_ERROR) {
        /* Refus du modem avant tout HTTP (pas de WiFi, hôte injoignable) ou trame
         * illisible : erreur de TRANSPORT. Un statut HTTP d'erreur (404…) n'est
         * PAS un échec ici — il remonte via net_control status. */
        net_fail(API_EIO);
        return;
    }
    if (nh_done(&net.hp))
        net.state = ST_EOF;
}

/* ── lecture ── */

int32_t net_read(uint8_t *dst, uint16_t count)
{
    if (net.state == ST_ERR)
        return -2;
    if (net.state == ST_IDLE)
        return -2;

    uint16_t used = ring_used();
    if (!used) {
        if (net.state == ST_EOF)
            return 0;               /* EOF franc : corps complet et anneau vidé */
        return -1;                  /* rien de prêt : rappeler plus tard */
    }

    if (count > used) count = used;
    uint16_t t = (uint16_t)(net.tail & NET_RING_MASK);
    uint16_t lin = (uint16_t)(NET_RING_SIZE - t);
    uint16_t first = (count < lin) ? count : lin;
    memcpy(dst, &net.ring[t], first);
    if (count > first)
        memcpy(dst + first, &net.ring[0], (size_t)(count - first));
    net.tail = (uint16_t)((net.tail + count) & NET_RING_MASK);
    return count;
}

void net_close(void)
{
    if (net.state == ST_IDLE)
        return;
    /* Le modem raccroche de lui-même en fin de transaction (« NO CARRIER »
     * observé après le corps) : aucun ATH à envoyer ici. Si l'appli ferme AVANT
     * la fin, les octets restants seront purgés au prochain net_open. */
    net_reset();
}

void net_stop(void)
{
    net_reset();
}

/* ── opcode $B7 : net_control ── */

#define NET_CTL_STATUS 0

void net_api_control(void)
{
    uint8_t fn = API_A;

    switch (fn) {
    case NET_CTL_STATUS: {
        /* Renvoie, sur le xstack : code HTTP (uint16), état (uint8), octets
         * disponibles (uint16). Permet à l'appli de distinguer « pas encore de
         * statut » (0), un 200, un 404, et de savoir s'il reste à lire.
         *
         * Un SEUL accès par registre côté 6502 : les valeurs sont poussées sur le
         * xstack (pas de registre à effet de bord), conformément au socle §5.9 —
         * un `LDA reg,X` indexé provoquerait un double accès. */
        uint16_t status = net.hp.status;
        uint16_t avail = ring_used();
        uint8_t  st = (uint8_t)net.state;
        if (!api_push_uint16(&avail) ||
            !api_push_uint8(&st) ||
            !api_push_uint16(&status))
            return api_return_errno(API_EINVAL);
        api_sync_xstack();
        return api_return_ax(0);
    }
    default:
        /* Sous-fonctions non implémentées dans ce lot (set_mode, wifi, prefix,
         * json_query, time) : erreur explicite plutôt que silence. */
        return api_return_errno(API_EINVAL);
    }
}
