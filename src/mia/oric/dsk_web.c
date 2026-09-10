/*
 * Copyright (c) 2026 bmarty
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Web-backed disk transport — voir dsk_web.h.
 *
 * LECTURE : `ATGET<url>?offset=&len=` + décodage HTTP par `api/net_http.c`.
 *
 * Ce fichier émettait `ATDISKRD`, une commande AT propriétaire supposée ajoutée
 * au firmware du modem. Vérification du 2026-09-10 sur le dongle réel (v0.3.3) :
 * **elle n'existe pas** — `ATDISKRD…` est interprété comme `ATD` + « ISKRDhttp:0 »
 * (« DIALLING SKRDhttp:0 » puis `NO CARRIER`) — et elle est **absente des sources
 * du dongle**. Ce transport était donc inopérant.
 *
 * La bascule sur `ATGET` est possible sans toucher au dongle parce que le serveur
 * webdisk accepte déjà `?offset=<o>&len=<n>` en query (documenté comme « pratique
 * pour un client minimal qui ne gère pas l'en-tête Range ») : aucun en-tête à
 * envoyer, ce qu'`ATGET` ne permettrait pas. Il répond `206 Partial Content` +
 * `Content-Length` exact. Le dongle relaie le HTTP brut sans le parser, d'où
 * `net_http.c` (dé-chunkage, Content-Length, statut) — validé par 31 tests natifs
 * dont la réponse réelle de ce serveur. Bénéfice au passage : HTTPS marche, le
 * dongle terminant le TLS.
 *
 * La pile USB hôte est pompée (tuh_task) pendant l'attente ; tout est borné par
 * un timeout. Le modèle reste BLOQUANT (le 6502 attend sa piste de toute façon).
 */

#include "oric/dsk_web.h"
#include "usb/cdc.h"
#include "api/net_http.h"
#include "tusb.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

#ifndef CFG_TUH_CDC
#define CFG_TUH_CDC 4
#endif

/* Timeout global d'une requête (lecture d'une piste de 6400 o sur lien série).
 * Élargi par rapport aux 8 s d'origine : en HTTPS le premier octet n'arrive
 * qu'après le handshake TLS, mesuré à plusieurs secondes (spec $B7 §1.1). */
#define DSK_WEB_TIMEOUT_US (20ull * 1000 * 1000)

/* Trouve l'interface CDC du modem, ou -1. */
static int dsk_web_modem_dev(void)
{
    for (uint8_t i = 0; i < CFG_TUH_CDC; i++)
        if (tuh_cdc_mounted(i) && cdc_is_modem(i))
            return i;
    return -1;
}

bool dsk_web_available(void)
{
    return dsk_web_modem_dev() >= 0;
}

/* Vide les octets en attente côté modem (réponse précédente, bruit). */
static void web_drain(uint8_t dev)
{
    uint8_t tmp[64];
    for (int guard = 0; guard < 2000; guard++) {
        tuh_task();
        if (tuh_cdc_read_available(dev))
            tuh_cdc_read(dev, tmp, sizeof tmp);
        else
            break;
    }
}

/* Écrit n octets vers le modem, en pompant l'USB, borné par deadline. */
static bool web_write_all(uint8_t dev, const char *s, uint32_t n, uint64_t deadline)
{
    uint32_t sent = 0;
    while (sent < n) {
        if (time_us_64() > deadline)
            return false;
        if (tuh_cdc_write_available(dev)) {
            sent += tuh_cdc_write(dev, s + sent, n - sent);
            tuh_cdc_write_flush(dev);
        }
        tuh_task();
    }
    return true;
}

/* Lit un octet du modem (pompe l'USB), ou -1 sur timeout. */
static int web_getc(uint8_t dev, uint64_t deadline)
{
    uint8_t c;
    while (time_us_64() <= deadline) {
        if (tuh_cdc_read_available(dev) && tuh_cdc_read(dev, &c, 1) == 1)
            return c;
        tuh_task();
    }
    return -1;
}


/* ─── GET via ATGET + décodage HTTP ──────────────────────────────────────────
 * Émet `ATGET<url>\r`, alimente le parseur avec ce qui arrive du modem et range
 * le CORPS dans `buf` (au plus `cap` octets). Renvoie true si le statut est 2xx
 * et, quand `exact` est non nul, si exactement `exact` octets de corps ont été
 * reçus. `*outlen` reçoit la taille de corps réellement livrée (peut être NULL).
 *
 * Le corps est écrit au fil de l'eau : pas de tampon intermédiaire de la taille
 * de la piste. Au-delà de `cap` les octets sont consommés puis jetés, ce qui
 * garde le flux modem synchronisé pour l'appel suivant. */
static bool web_get(uint8_t dev, const char *cmd, uint32_t cmd_len,
                    void *buf, uint32_t cap, uint32_t exact, uint32_t *outlen)
{
    uint64_t deadline = time_us_64() + DSK_WEB_TIMEOUT_US;

    web_drain(dev);
    if (!web_write_all(dev, cmd, cmd_len, deadline))
        return false;

    nh_parser hp;
    nh_init(&hp);

    uint8_t *out = (uint8_t *)buf;
    uint32_t got = 0;

    while (!nh_done(&hp) && hp.state != NH_ST_ERROR) {
        if (time_us_64() > deadline)
            return false;

        uint8_t in[64];
        uint32_t avail = tuh_cdc_read_available(dev);
        if (!avail) {
            tuh_task();
            continue;
        }
        uint32_t want = (avail > sizeof in) ? (uint32_t)sizeof in : avail;
        uint32_t n = tuh_cdc_read(dev, in, want);
        if (!n)
            continue;

        uint32_t off = 0;
        while (off < n) {
            uint8_t body[64];
            nh_result r = nh_feed(&hp, in + off, n - off, body, sizeof body);
            for (uint32_t i = 0; i < r.produced; i++, got++)
                if (got < cap)
                    out[got] = body[i];
            off += r.consumed;
            if (r.consumed == 0 && r.produced == 0)
                break;
        }
    }

    if (hp.state == NH_ST_ERROR)
        return false;                       /* refus du modem, trame illisible */
    if (hp.status != 200 && hp.status != 206)
        return false;                       /* 404, 416, 5xx… : échec propre */
    if (exact && got != exact)
        return false;                       /* tranche incomplète : ne pas servir */
    if (outlen)
        *outlen = (got < cap) ? got : cap;
    return true;
}

bool dsk_web_read(const char *url, uint32_t offset, uint32_t len, void *buf)
{
    int dev = dsk_web_modem_dev();
    if (dev < 0 || url == NULL || buf == NULL || len == 0)
        return false;

    /* `?offset=&len=` plutôt qu'un en-tête `Range:` — ATGET n'envoie pas d'en-tête,
     * et le serveur webdisk accepte cette forme (réponse 206 + Content-Length). */
    char cmd[224];
    int n = snprintf(cmd, sizeof cmd, "ATGET%s?offset=%lu&len=%lu\r",
                     url, (unsigned long)offset, (unsigned long)len);
    if (n <= 0 || n >= (int)sizeof cmd)
        return false;

    return web_get((uint8_t)dev, cmd, (uint32_t)n, buf, len, len, NULL);
}

/* ⚠️ ÉCRITURE NON FONCTIONNELLE — et elle ne peut PAS être basculée comme la
 * lecture. `ATDISKWR` n'existe pas plus que `ATDISKRD` dans le dongle (même
 * vérification du 2026-09-10), et `ATPOST` — la seule alternative — a été mesuré
 * inadapté aux données binaires (spec `$B7` §1.2) :
 *
 *   - il SUPPRIME les `0x0D` : 6 octets `00 0D 0A 1A FF 41` arrivent en 5,
 *     `00 0A 1A FF 41` (vérifié contre httpbin.org/post) ;
 *   - un corps contenant `\r\n.\r\n` déclenche `ERROR` : c'est son terminateur ;
 *   - il refuse dès ~3000 octets, alors qu'une piste en fait 6400.
 *
 * C'est un canal LIGNE, pas un canal d'octets ; un encodage base64 réglerait la
 * corruption mais pas la taille (8534 o encodés par piste, cinq requêtes). Écrire
 * une piste exige donc une COMMANDE AT BINAIRE côté dongle — ce qui était
 * l'intention d'`ATDISKWR`. Le disque web reste en LECTURE SEULE jusque-là.
 *
 * La fonction est laissée en l'état plutôt que basculée à moitié, pour ne pas
 * faire croire à un chemin d'écriture opérationnel : elle échoue proprement
 * (le modem répond ERROR à `ATDISKWR`), et `dsk.c` remonte l'échec. */
bool dsk_web_write(const char *url, uint32_t offset, uint32_t len, const void *buf)
{
    int dev = dsk_web_modem_dev();
    if (dev < 0 || url == NULL || buf == NULL || len == 0)
        return false;

    char cmd[192];
    int n = snprintf(cmd, sizeof cmd, "ATDISKWR%s?offset=%lu&len=%lu\r",
                     url, (unsigned long)offset, (unsigned long)len);
    if (n <= 0 || n >= (int)sizeof cmd)
        return false;

    uint64_t deadline = time_us_64() + DSK_WEB_TIMEOUT_US;

    web_drain((uint8_t)dev);
    if (!web_write_all((uint8_t)dev, cmd, (uint32_t)n, deadline))
        return false;
    /* Corps : les len octets bruts (le modem les relaie en PUT). */
    if (!web_write_all((uint8_t)dev, (const char *)buf, len, deadline))
        return false;

    /* Réponse du modem : OK (2xx) ou ERROR. Scan robuste des deux jetons. */
    static const char tok_ok[] = "OK";
    static const char tok_er[] = "ERROR";
    int oi = 0, ei = 0, c;
    while ((c = web_getc((uint8_t)dev, deadline)) >= 0) {
        oi = (c == tok_ok[oi]) ? oi + 1 : (c == tok_ok[0] ? 1 : 0);
        if (oi == (int)(sizeof tok_ok - 1))
            return true;
        ei = (c == tok_er[ei]) ? ei + 1 : (c == tok_er[0] ? 1 : 0);
        if (ei == (int)(sizeof tok_er - 1))
            return false;
    }
    return false;
}

/* ─── Route B : base URL + GET généraliste (liste /disks) ─────────────────── */

const char *dsk_web_base(void)
{
#ifdef LOCI_WEBDISK_BASE
    return LOCI_WEBDISK_BASE;
#else
    return "";
#endif
}

bool dsk_web_fetch(const char *url, void *buf, uint32_t cap, uint32_t *outlen)
{
    int dev = dsk_web_modem_dev();
    if (dev < 0 || url == NULL || buf == NULL || cap == 0 || outlen == NULL)
        return false;

    char cmd[224];
    int n = snprintf(cmd, sizeof cmd, "ATGET%s\r", url);
    if (n <= 0 || n >= (int)sizeof cmd)
        return false;

    /* Taille variable (liste JSON /disks) : pas de longueur exacte exigée. Le
     * corps peut arriver en `Transfer-Encoding: chunked`, que le parseur
     * dé-chunke — c'est le cas observé sur les serveurs derrière Cloudflare. */
    return web_get((uint8_t)dev, cmd, (uint32_t)n, buf, cap, 0, outlen);
}
