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

    /* Attendre le marqueur "+DISK:" (robuste à l'écho de la commande). */
    static const char marker[] = "+DISK:";
    int mi = 0, c;
    while (mi < (int)(sizeof marker - 1)) {
        c = web_getc((uint8_t)dev, deadline);
        if (c < 0)
            return false;
        if (c == marker[mi])
            mi++;
        else
            mi = (c == marker[0]) ? 1 : 0;
    }

    /* Lire la longueur annoncée jusqu'au CR. */
    uint32_t rlen = 0;
    bool any = false;
    while (1) {
        c = web_getc((uint8_t)dev, deadline);
        if (c < 0)
            return false;
        if (c >= '0' && c <= '9') {
            rlen = rlen * 10 + (uint32_t)(c - '0');
            any = true;
        } else if (c == '\r') {
            break;
        } else if (any) {
            return false; /* format inattendu */
        }
    }
    web_getc((uint8_t)dev, deadline); /* consommer le LF */
    if (!any || rlen != len)
        return false; /* le serveur a renvoyé une autre taille → échec propre */

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
