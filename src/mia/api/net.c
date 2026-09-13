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
 * Lot 2 — écriture (PUT) :
 *
 *   open("N:http://h/p", W)  -> ST_WBUF    (le corps s'accumule dans l'anneau, linéaire)
 *   write_xram(fd, n)        -> ajoute n octets (ENOSPC au-delà de NET_RING_SIZE)
 *   close(fd)                -> ST_SEND    « ATDISKWR<url>?len=N\r » + N octets bruts
 *                            -> ST_RECV    réponse HTTP du serveur (statut via $B7 status)
 *                            -> ST_EOF     ; le prochain open libère
 * `ATDISKWR` est la commande binaire ajoutée au dongle pour le disque web (PUT,
 * octets bruts cadrés par `len=`) : `ATPOST` corrompt le binaire (spec §1.2).
 *
 * Lot 3 — flux TCP brut (`N:tcp://host:port`, `N:telnet://host:port`) :
 *
 *   open (R, W ou RDWR)      -> ST_SEND     « ATDT-host:port\r » ('-' = pas de
 *                                            négociation telnet ; '=' pour telnet://)
 *                            -> ST_DIAL     attente de la ligne CONNECT (ou NO CARRIER…)
 *                            -> ST_STREAM   octets bruts : read sert l'anneau, write part
 *                                            directement ; « \r\nNO CARRIER » en bande = EOF
 *   close                    -> ST_HANGUP   +++ (garde 1,1 s avant/après) puis ATH,
 *                                            puis retour au passe-plat ; immédiat si la
 *                                            distante a déjà raccroché
 * Pas de DCD : la fin distante ne se voit que par la trame « NO CARRIER » du
 * modem, comme pour tout programme terminal. Lecture NON bloquante conseillée :
 * interroger `$B7 status` (avail) avant read_xram, qui bloque l'anneau vide.
 *
 * Lot 4 — `$B7` A=4 json_query(chemin) : extrait un champ du corps JSON encore
 * dans l'anneau (≤ 2 Ko, non lu, transaction en ST_EOF de préférence) sans que
 * le 6502 parse quoi que ce soit. Chemin pointé « a.b[2].c » ; valeur renvoyée
 * sur le xstack (chaîne sans guillemets, nombre/true/false/null tels quels,
 * objet/tableau = texte brut), AX = longueur ; ENOENT si absent, EINVAL si
 * l'anneau n'est pas un JSON exploitable. Non destructif (le corps reste lisible).
 *
 * Ce qui reste ABSENT : prefix, multi-connexions.
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
    ST_SEND,       /* commande AT (+ corps en écriture) en cours d'émission */
    ST_RECV,       /* réponse en cours de réception */
    ST_EOF,        /* corps complet ; l'anneau peut encore contenir des octets */
    ST_ERR,        /* transaction échouée (net_errno) */
    ST_WBUF,       /* ouvert en écriture : le corps s'accumule (lot 2) */
    ST_DIAL,       /* tcp : numérotation, attente de CONNECT (lot 3) */
    ST_STREAM,     /* tcp : connecté, flux brut dans les deux sens */
    ST_HANGUP      /* tcp : raccrochage en cours (+++ / ATH), canal encore tenu */
};

#define NET_ESC_GUARD_US (1100ull * 1000)   /* garde autour de +++ (modem : 1 s) */
#define NET_HANGUP_TAIL_US (300ull * 1000)  /* laisser le modem répondre à ATH */
#define NET_DIAL_TIMEOUT_US (30ull * 1000 * 1000)

static struct {
    enum net_state state;
    int      dev;              /* interface CDC du modem, -1 si aucune */
    uint8_t  errno_api;

    char     cmd[NET_CMD_MAX];
    uint16_t cmd_len, cmd_sent;
    uint16_t body_len, body_sent;   /* écriture : corps linéaire dans ring[0..body_len) */
    bool     writing;
    bool     stream;                /* lot 3 : transaction tcp:// */
    uint8_t  hang_step;             /* 0 attente garde, 1 +++ envoyé, 2 ATH envoyé */
    char     line[48];              /* ST_DIAL : ligne de réponse en cours */
    uint8_t  line_len;
    uint8_t  nc_match;              /* ST_STREAM : avancement dans "\r\nNO CARRIER" */

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
    net.body_len = net.body_sent = 0;
    net.writing = false;
    net.stream = false;
    net.hang_step = 0;
    net.line_len = 0;
    net.nc_match = 0;
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
    return net.state == ST_SEND || net.state == ST_RECV || net.state == ST_EOF ||
           net.state == ST_WBUF || net.state == ST_DIAL || net.state == ST_STREAM ||
           net.state == ST_HANGUP;
}

uint8_t net_errno(void) { return net.errno_api; }

bool net_is_path(const uint8_t *path)
{
    return path && (path[0] == 'N' || path[0] == 'n') && path[1] == ':';
}

/* ── ouverture ── */

static int net_purge_and_arm(int dev)
{
    /* Purge des octets en attente (réponse précédente, bruit) : sans ça le
     * parseur démarrerait au milieu d'une trame. Borné, non bloquant. */
    uint8_t tmp[64];
    for (int guard = 0; guard < 64 && tuh_cdc_read_available((uint8_t)dev); guard++)
        tuh_cdc_read((uint8_t)dev, tmp, sizeof tmp);
    net.state = ST_SEND;
    net.deadline = time_us_64() + NET_SEND_TIMEOUT_US;
    return 0;
}

int net_open(const uint8_t *path, uint8_t flags)
{
    const unsigned char RDWR = 0x03;
    uint8_t rw = flags & RDWR;

    /* Canal AT unique : une transaction à la fois. Un PUT dont la réponse est
     * arrivée (ST_EOF) ou a échoué (ST_ERR) ne bloque pas : on le libère ici. */
    if (net.state == ST_EOF || net.state == ST_ERR)
        net_reset();
    if (net.state != ST_IDLE)
        return API_EMFILE;

    /* Lecture (GET) ou écriture (PUT via ATDISKWR) ; RDWR réservé à tcp://. */
    bool writing = (rw == 0x02);

    const char *url = (const char *)path + 2;   /* saute « N: » */
    if (!url[0])
        return API_EINVAL;

    int dev = net_modem_dev();
    if (dev < 0)
        return API_ENODEV;          /* pas de modem monté : échec propre, détectable */

    net_reset();
    net.dev = dev;

    /* Lot 3 : flux TCP brut. Tous les modes sont admis (bidirectionnel). */
    bool telnet = !strncmp(url, "telnet://", 9);
    if (telnet || !strncmp(url, "tcp://", 6)) {
        const char *hp = url + (telnet ? 9 : 6);
        if (!hp[0] || !strchr(hp, ':'))
            return API_EINVAL;      /* host:port obligatoire */
        int n = snprintf(net.cmd, sizeof net.cmd, "ATDT%c%s\r", telnet ? '=' : '-', hp);
        if (n <= 0 || n >= (int)sizeof net.cmd)
            return API_EINVAL;
        net.cmd_len = (uint16_t)n;
        net.cmd_sent = 0;
        net.stream = true;
        return net_purge_and_arm(dev);
    }

    if (rw == 0x03)
        return API_EINVAL;          /* HTTP : pas de RDWR sur un flux GET/PUT */
    net.writing = writing;

    if (writing) {
        /* La commande n'est construite qu'au close (len= connu) : on garde l'URL. */
        int n = snprintf(net.cmd, sizeof net.cmd, "%s", url);
        if (n <= 0 || n >= (int)sizeof net.cmd - 32)   /* place pour ATDISKWR + ?len=NNNNN\r */
            return API_EINVAL;
        net.cmd_len = (uint16_t)n;
        net.state = ST_WBUF;
        return 0;
    }

    int n = snprintf(net.cmd, sizeof net.cmd, "ATGET%s\r", url);
    if (n <= 0 || n >= (int)sizeof net.cmd)
        return API_EINVAL;          /* URL trop longue pour la commande AT */
    net.cmd_len = (uint16_t)n;
    net.cmd_sent = 0;
    return net_purge_and_arm(dev);
}

/* ── écriture (lot 2) ── */

int32_t net_write(const uint8_t *src, uint16_t count)
{
    if (net.state == ST_STREAM) {
        /* Flux : part directement vers le modem (borné par la place USB). */
        if (!tuh_cdc_write_available((uint8_t)net.dev))
            return 0;               /* rien accepté : réessayer */
        uint32_t n = tuh_cdc_write((uint8_t)net.dev, src, count);
        tuh_cdc_write_flush((uint8_t)net.dev);
        return (int32_t)n;
    }
    if (net.state != ST_WBUF)
        return -2;                  /* pas ouvert en écriture (EINVAL) */
    if ((uint32_t)net.body_len + count > NET_RING_SIZE)
        return -3;                  /* corps plus grand que le tampon (ENOSPC) */
    memcpy(&net.ring[net.body_len], src, count);
    net.body_len = (uint16_t)(net.body_len + count);
    return count;
}

/* close() d'un fd ouvert en écriture : émet la commande puis le corps, et laisse
 * la réponse arriver (statut HTTP lisible par $B7 status jusqu'au prochain open). */
static int net_commit(void)
{
    char url[NET_CMD_MAX];
    memcpy(url, net.cmd, net.cmd_len);
    url[net.cmd_len] = 0;
    int n = snprintf(net.cmd, sizeof net.cmd, "ATDISKWR%s%clen=%u\r",
                     url, strchr(url, '?') ? '&' : '?', (unsigned)net.body_len);
    if (n <= 0 || n >= (int)sizeof net.cmd) {
        net_fail(API_EINVAL);
        return API_EINVAL;
    }
    net.cmd_len = (uint16_t)n;
    net.cmd_sent = 0;
    net.body_sent = 0;
    return net_purge_and_arm(net.dev);
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
        if (net.cmd_sent >= net.cmd_len && net.writing && net.body_sent < net.body_len) {
            /* Corps brut à la suite de la commande (cadré par len=). */
            if (tuh_cdc_write_available((uint8_t)net.dev)) {
                net.body_sent += (uint16_t)tuh_cdc_write((uint8_t)net.dev,
                                                         net.ring + net.body_sent,
                                                         net.body_len - net.body_sent);
                tuh_cdc_write_flush((uint8_t)net.dev);
                net.deadline = time_us_64() + NET_SEND_TIMEOUT_US;
            }
        }
        if (net.cmd_sent >= net.cmd_len && (!net.writing || net.body_sent >= net.body_len)) {
            net.state = net.stream ? ST_DIAL : ST_RECV;
            net.head = net.tail = 0;    /* l'anneau redevient le tampon de RÉPONSE */
            net.deadline = time_us_64() + (net.stream ? NET_DIAL_TIMEOUT_US : NET_FIRST_TIMEOUT_US);
        }
        return;
    }

    if (net.state == ST_HANGUP) {
        uint64_t now = time_us_64();
        if (now < net.deadline) return;
        if (net.hang_step == 0) {
            if (!tuh_cdc_write_available((uint8_t)net.dev)) return;
            tuh_cdc_write((uint8_t)net.dev, "+++", 3);
            tuh_cdc_write_flush((uint8_t)net.dev);
            net.hang_step = 1; net.deadline = now + NET_ESC_GUARD_US;
        } else if (net.hang_step == 1) {
            if (!tuh_cdc_write_available((uint8_t)net.dev)) return;
            tuh_cdc_write((uint8_t)net.dev, "ATH\r", 4);
            tuh_cdc_write_flush((uint8_t)net.dev);
            net.hang_step = 2; net.deadline = now + NET_HANGUP_TAIL_US;
        } else {
            uint8_t tmp[64];        /* purge OK / NO CARRIER puis libère le canal */
            for (int guard = 0; guard < 64 && tuh_cdc_read_available((uint8_t)net.dev); guard++)
                tuh_cdc_read((uint8_t)net.dev, tmp, sizeof tmp);
            net_reset();
        }
        return;
    }

    if (net.state == ST_DIAL) {
        /* Réponse ligne à ligne du modem : CONNECT xxxx → flux ; sinon refus. */
        uint64_t now = time_us_64();
        if (now > net.deadline) { net_fail(API_EIO); return; }
        uint8_t ch;
        while (tuh_cdc_read_available((uint8_t)net.dev) && tuh_cdc_read((uint8_t)net.dev, &ch, 1) == 1) {
            if (ch == '\n' || ch == '\r') {
                net.line[net.line_len] = 0;
                if (net.line_len) {
                    if (!strncmp(net.line, "CONNECT", 7)) {
                        net.state = ST_STREAM;
                        net.hp.status = 0;
                        /* « CONNECT 9600\r\n » : le \n qui suit le \r appartient à la
                         * réponse du modem, pas au flux. */
                        if (ch == '\r' && tuh_cdc_read_available((uint8_t)net.dev)) {
                            uint8_t lf;
                            tuh_cdc_read((uint8_t)net.dev, &lf, 1);
                            if (lf != '\n') { net.ring[net.head++ & NET_RING_MASK] = lf; net.head &= NET_RING_MASK; }
                        }
                        return;
                    }
                    if (!strncmp(net.line, "NO CARRIER", 10) || !strncmp(net.line, "NO ANSWER", 9) ||
                        !strncmp(net.line, "ERROR", 5) || !strncmp(net.line, "BUSY", 4) ||
                        !strncmp(net.line, "NO DIALTONE", 11)) {
                        net_fail(API_EIO); return;
                    }
                }
                net.line_len = 0;   /* DIALLING…, écho : ignorés */
            } else if (net.line_len < sizeof net.line - 1) {
                net.line[net.line_len++] = (char)ch;
            }
        }
        return;
    }

    if (net.state == ST_STREAM) {
        /* Flux brut → anneau, avec détection en bande de « \r\nNO CARRIER » (fin
         * distante) : les octets du motif ne sont livrés que s'il échoue. */
        static const char nc[] = "\r\nNO CARRIER";
        while (ring_free() > (uint16_t)sizeof nc && tuh_cdc_read_available((uint8_t)net.dev)) {
            uint8_t ch;
            if (tuh_cdc_read((uint8_t)net.dev, &ch, 1) != 1) break;
            if (ch == (uint8_t)nc[net.nc_match]) {
                if (++net.nc_match == (uint8_t)(sizeof nc - 1)) {
                    net.state = ST_EOF;     /* la distante a raccroché : EOF après l'anneau */
                    return;
                }
                continue;
            }
            /* motif rompu : livrer les octets retenus, puis celui-ci (ou repartir sur lui) */
            for (uint8_t i = 0; i < net.nc_match; i++)
                net.ring[net.head++ & NET_RING_MASK] = (uint8_t)nc[i];
            net.head &= NET_RING_MASK;
            net.nc_match = (ch == (uint8_t)nc[0]) ? 1 : 0;
            if (!net.nc_match)
                net.ring[net.head++ & NET_RING_MASK] = ch, net.head &= NET_RING_MASK;
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
    if (net.state == ST_IDLE || net.state == ST_WBUF || net.state == ST_HANGUP)
        return -2;                  /* pas ouvert, ouvert en écriture, ou raccrochage */
    if (net.state == ST_DIAL)
        return -1;                  /* pas encore connecté */

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
    if (net.state == ST_WBUF) {
        net_commit();               /* le PUT part maintenant ; net_task le pompe */
        return;
    }
    if (net.state == ST_DIAL || net.state == ST_STREAM) {
        /* Connexion vivante : raccrocher proprement (+++ / ATH), en fond. */
        net.state = ST_HANGUP;
        net.hang_step = 0;
        net.deadline = time_us_64() + NET_ESC_GUARD_US;
        return;
    }
    /* Le modem raccroche de lui-même en fin de transaction (« NO CARRIER »
     * observé après le corps) : aucun ATH à envoyer ici. Si l'appli ferme AVANT
     * la fin, les octets restants seront purgés au prochain net_open. */
    net_reset();
}

void net_stop(void)
{
    net_reset();
}

/* ── json_query (lot 4) : navigation minimale dans l'anneau, sans copie ── */

static inline int jb(uint16_t i)            /* octet i du corps non lu, -1 au-delà */
{
    return i < ring_used() ? net.ring[(net.tail + i) & NET_RING_MASK] : -1;
}
static uint16_t jskip_ws(uint16_t i) { int c; while ((c = jb(i)) == ' ' || c == '\t' || c == '\r' || c == '\n') i++; return i; }
/* Fin (exclusive) de la valeur commençant en i : chaîne, nombre/mot, objet/tableau (équilibré). */
static uint16_t jvalue_end(uint16_t i)
{
    int c = jb(i);
    if (c == '"') {
        for (i++; (c = jb(i)) >= 0; i++) { if (c == '\\') i++; else if (c == '"') return (uint16_t)(i + 1); }
        return i;
    }
    if (c == '{' || c == '[') {
        int depth = 0; bool instr = false;
        for (; (c = jb(i)) >= 0; i++) {
            if (instr) { if (c == '\\') i++; else if (c == '"') instr = false; continue; }
            if (c == '"') instr = true;
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') { if (--depth == 0) return (uint16_t)(i + 1); }
        }
        return i;
    }
    while ((c = jb(i)) >= 0 && c != ',' && c != '}' && c != ']' && c != ' ' && c != '\r' && c != '\n' && c != '\t') i++;
    return i;
}
/* Dans l'objet en i : position de la valeur de la clé (len octets), ou -1. */
static int jfind_key(uint16_t i, const char *key, uint8_t klen)
{
    if (jb(i) != '{') return -1;
    i = jskip_ws((uint16_t)(i + 1));
    while (jb(i) == '"') {
        uint16_t ks = (uint16_t)(i + 1), ke = ks;
        int c;
        while ((c = jb(ke)) >= 0 && c != '"') { if (c == '\\') ke++; ke++; }
        bool match = (ke - ks == klen);
        for (uint8_t k = 0; match && k < klen; k++) if (jb((uint16_t)(ks + k)) != (uint8_t)key[k]) match = false;
        i = jskip_ws((uint16_t)(ke + 1));
        if (jb(i) != ':') return -1;
        i = jskip_ws((uint16_t)(i + 1));
        if (match) return i;
        i = jskip_ws(jvalue_end(i));
        if (jb(i) != ',') return -1;
        i = jskip_ws((uint16_t)(i + 1));
    }
    return -1;
}
/* Dans le tableau en i : position de l'élément n, ou -1. */
static int jfind_index(uint16_t i, unsigned n)
{
    if (jb(i) != '[') return -1;
    i = jskip_ws((uint16_t)(i + 1));
    for (;;) {
        if (jb(i) == ']' || jb(i) < 0) return -1;
        if (n == 0) return i;
        i = jskip_ws(jvalue_end(i));
        if (jb(i) != ',') return -1;
        i = jskip_ws((uint16_t)(i + 1));
        n--;
    }
}

/* Chemin « a.b[2].c » depuis le xstack ; pousse la valeur, renvoie sa longueur ou -errno. */
int net_json_query(void)
{
    const char *path = (const char *)&xstack[xstack_ptr];
    uint16_t plen = (uint16_t)(XSTACK_SIZE - xstack_ptr);
    if (net.state == ST_IDLE || net.state == ST_WBUF || net.state == ST_ERR || !ring_used())
        return -API_EINVAL;
    int pos = (int)jskip_ws(0);
    uint16_t p = 0;
    while (p < plen && path[p]) {
        if (path[p] == '.') { p++; continue; }
        if (path[p] == '[') {
            unsigned n = 0; p++;
            while (p < plen && path[p] >= '0' && path[p] <= '9') n = n * 10 + (unsigned)(path[p++] - '0');
            if (p >= plen || path[p] != ']') return -API_EINVAL;
            p++;
            pos = jfind_index((uint16_t)pos, n);
        } else {
            uint16_t ks = p;
            while (p < plen && path[p] && path[p] != '.' && path[p] != '[') p++;
            pos = jfind_key((uint16_t)pos, path + ks, (uint8_t)(p - ks));
        }
        if (pos < 0) return -API_ENOENT;
    }
    /* Valeur : chaîne sans guillemets (échappements \" \\ \/ \n \t \r réduits), sinon texte brut. */
    uint8_t out[255]; uint16_t n = 0;
    uint16_t end = jvalue_end((uint16_t)pos);
    if (jb((uint16_t)pos) == '"') {
        for (uint16_t i = (uint16_t)(pos + 1); i + 1 < end && n < sizeof out; i++) {
            int c = jb(i);
            if (c == '\\') {
                int e = jb(++i);
                c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
            }
            out[n++] = (uint8_t)c;
        }
    } else {
        for (uint16_t i = (uint16_t)pos; i < end && n < sizeof out; i++) out[n++] = (uint8_t)jb(i);
    }
    api_zxstack();
    if (!api_push_n(out, n)) return -API_EINVAL;
    api_sync_xstack();
    return n;
}

/* ── opcode $B7 : net_control ── */

#define NET_CTL_STATUS 0
#define NET_CTL_JSON   4

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
    case NET_CTL_JSON: {
        int n = net_json_query();
        if (n < 0) return api_return_errno((uint8_t)-n);
        return api_return_ax((uint16_t)n);
    }
    default:
        /* Sous-fonctions non implémentées (set_mode, wifi, prefix, time) :
         * erreur explicite plutôt que silence. */
        return api_return_errno(API_EINVAL);
    }
}
