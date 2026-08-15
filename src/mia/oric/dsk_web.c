/*
 * Copyright (c) 2026 bmarty
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Web-backed disk transport — voir dsk_web.h.
 *
 * Émet ATDISKRD<url>?offset=&len= au modem PicoWiFi (hôte USB-CDC) et décode la
 * trame  \r\n+DISK:<len>\r\n <len octets>  . La pile USB hôte est pompée
 * (tuh_task) pendant l'attente ; tout est borné par un timeout.
 */

#include "oric/dsk_web.h"
#include "usb/cdc.h"
#include "tusb.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

#ifndef CFG_TUH_CDC
#define CFG_TUH_CDC 4
#endif

/* Timeout global d'une requête (lecture d'une piste de 6400 o sur lien série). */
#define DSK_WEB_TIMEOUT_US (8ull * 1000 * 1000)

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

/* Attend "+DISK:<len>\r\n" (robuste à l'écho) et renvoie <len> dans *rlen.
 * Partagé par dsk_web_read (taille exacte) et dsk_web_fetch (taille variable). */
static bool web_recv_header(uint8_t dev, uint64_t deadline, uint32_t *rlen)
{
    static const char marker[] = "+DISK:";
    int mi = 0, c;
    while (mi < (int)(sizeof marker - 1)) {
        c = web_getc(dev, deadline);
        if (c < 0)
            return false;
        mi = (c == marker[mi]) ? mi + 1 : (c == marker[0]);
    }
    uint32_t v = 0;
    bool any = false;
    for (;;) {
        c = web_getc(dev, deadline);
        if (c < 0)
            return false;
        if (c >= '0' && c <= '9') {
            v = v * 10 + (uint32_t)(c - '0');
            any = true;
        } else if (c == '\r') {
            break;
        } else if (any) {
            return false;
        }
    }
    web_getc(dev, deadline); /* consommer le LF */
    if (!any)
        return false;
    *rlen = v;
    return true;
}

bool dsk_web_read(const char *url, uint32_t offset, uint32_t len, void *buf)
{
    int dev = dsk_web_modem_dev();
    if (dev < 0 || url == NULL || buf == NULL || len == 0)
        return false;

    char cmd[192];
    int n = snprintf(cmd, sizeof cmd, "ATDISKRD%s?offset=%lu&len=%lu\r",
                     url, (unsigned long)offset, (unsigned long)len);
    if (n <= 0 || n >= (int)sizeof cmd)
        return false;

    uint64_t deadline = time_us_64() + DSK_WEB_TIMEOUT_US;

    web_drain((uint8_t)dev);
    if (!web_write_all((uint8_t)dev, cmd, (uint32_t)n, deadline))
        return false;

    uint32_t rlen = 0;
    if (!web_recv_header((uint8_t)dev, deadline, &rlen) || rlen != len)
        return false; /* autre taille annoncée → échec propre */

    /* Lire exactement len octets de corps dans buf. */
    uint8_t *out = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < len) {
        if (time_us_64() > deadline)
            return false;
        uint32_t avail = tuh_cdc_read_available((uint8_t)dev);
        if (avail) {
            uint32_t want = len - got;
            if (want > avail)
                want = avail;
            got += tuh_cdc_read((uint8_t)dev, out + got, want);
        } else {
            tuh_task();
        }
    }
    return got == len;
}

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

    char cmd[192];
    int n = snprintf(cmd, sizeof cmd, "ATDISKRD%s\r", url);
    if (n <= 0 || n >= (int)sizeof cmd)
        return false;

    uint64_t deadline = time_us_64() + DSK_WEB_TIMEOUT_US;
    web_drain((uint8_t)dev);
    if (!web_write_all((uint8_t)dev, cmd, (uint32_t)n, deadline))
        return false;

    uint32_t rlen = 0;
    if (!web_recv_header((uint8_t)dev, deadline, &rlen))
        return false;

    /* Lire les rlen octets (taille variable) ; n'en conserver que cap, jeter le
     * reste (garde le flux modem synchronisé pour l'appel suivant). */
    uint8_t *out = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < rlen) {
        if (time_us_64() > deadline)
            return false;
        int c = web_getc((uint8_t)dev, deadline);
        if (c < 0)
            return false;
        if (got < cap)
            out[got] = (uint8_t)c;
        got++;
    }
    *outlen = (rlen < cap) ? rlen : cap;
    return true;
}
