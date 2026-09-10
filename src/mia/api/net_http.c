/*
 * Copyright (c) 2026 bmarty
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Copie greffée de `extensions/net-device-B7/src/net_http.c` (source de vérité,
 * tests natifs 27/27).
 *
 * net_http.c — voir net_http.h. Parseur incrémental de la réponse `ATGET`.
 *
 * Le flux réel (mesuré, spec §1.1) :
 *
 *     \r\nCONNECT 9600\r\n  HTTP/1.1 200 OK\r\n  <headers>\r\n  \r\n
 *     <corps : Content-Length brut  OU  chunks « taille-hexa\r\n data \r\n » … « 0\r\n\r\n »>
 *     \r\nNO CARRIER (hh:mm:ss)\r\n
 */
#include "api/net_http.h"

/* ── petits utilitaires (pas de <string.h>/<ctype.h> : cible firmware) ── */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Comparaison INSENSIBLE À LA CASSE du début de `s` avec `pfx`. Nécessaire : le
 * dongle relaie les en-têtes tels que le serveur les envoie, et on a observé sur
 * le même serveur « Transfer-Encoding » et « last-modified ». */
static int starts_ci(const char *s, const char *pfx)
{
    while (*pfx) {
        if (lower(*s) != lower(*pfx)) return 0;
        s++; pfx++;
    }
    return 1;
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static uint32_t parse_dec(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10u + (uint32_t)(*s - '0'); s++; }
    return v;
}

/* Taille de chunk : hexadécimal, éventuellement suivi d'extensions « ;… » que la
 * RFC autorise et qu'on ignore. `ok` = au moins un chiffre hexa vu. */
static uint32_t parse_hex(const char *s, int *ok)
{
    uint32_t v = 0;
    int n = 0;
    s = skip_ws(s);
    for (;;) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (uint32_t)d;
        n++;
        s++;
    }
    *ok = n > 0;
    return v;
}

/* ── accumulation de ligne ──
 * Renvoie 1 quand une ligne complète est disponible dans p->line (CRLF ou LF
 * seul consommé, terminateur exclu). Une ligne plus longue que NH_LINE_MAX est
 * tronquée pour l'analyse mais on continue de chercher son terminateur : le flux
 * ne se désynchronise pas. */
static int line_push(nh_parser *p, uint8_t c)
{
    if (c == '\n') {
        if (p->line_len && p->line[p->line_len - 1] == '\r') p->line_len--;
        p->line[p->line_len] = '\0';
        return 1;
    }
    if (p->line_len < NH_LINE_MAX - 1) p->line[p->line_len++] = (char)c;
    else                               p->line_over = 1;
    return 0;
}

static void line_reset(nh_parser *p) { p->line_len = 0; p->line_over = 0; p->line[0] = '\0'; }

void nh_init(nh_parser *p)
{
    p->state = NH_ST_CONNECT;
    p->error = NH_ERR_NONE;
    p->status = 0;
    p->content_length = 0;
    p->has_length = 0;
    p->chunked = 0;
    p->body_total = 0;
    p->chunk_left = 0;
    p->saw_chunk_zero = 0;
    p->nc_match = 0;
    line_reset(p);
}

int nh_done(const nh_parser *p) { return p->state == NH_ST_DONE; }

/* Détection de « \r\nNO CARRIER » pour le SEUL cas où le corps n'a pas de
 * cadrage (ni Content-Length ni chunked) : la fermeture de connexion est alors
 * la seule fin possible. Ailleurs on ne s'en sert pas — un corps binaire peut
 * contenir ces octets (spec §1.1 point 4).
 *
 * Les octets déjà émis qui se révèlent appartenir à la sentinelle ne peuvent pas
 * être « reprisr » ; on retient donc la correspondance AVANT de produire, et on
 * n'émet un octet que lorsqu'il ne peut plus faire partie du motif. */
static const char NC_PAT[] = "\r\nNO CARRIER";

typedef struct { uint8_t emit[sizeof NC_PAT]; uint8_t n; uint8_t hit; } nc_out;

static nc_out nc_step(nh_parser *p, uint8_t c)
{
    nc_out r;
    r.n = 0;
    r.hit = 0;

    for (;;) {
        if ((char)c == NC_PAT[p->nc_match]) {
            p->nc_match++;
            if (NC_PAT[p->nc_match] == '\0') { p->nc_match = 0; r.hit = 1; }
            return r;                       /* octet retenu (peut-être la sentinelle) */
        }
        if (p->nc_match == 0) {
            r.emit[r.n++] = c;              /* aucun préfixe en cours : on émet */
            return r;
        }
        /* Faux départ : le préfixe retenu n'était pas la sentinelle. On émet son
         * premier octet et on retente la correspondance sur le reste (motif sans
         * auto-recouvrement non trivial, donc un décalage de 1 suffit). */
        r.emit[r.n++] = (uint8_t)NC_PAT[0];
        uint8_t keep = (uint8_t)(p->nc_match - 1);
        for (uint8_t i = 0; i < keep; i++) r.emit[r.n++] = (uint8_t)NC_PAT[i + 1];
        p->nc_match = 0;
    }
}

nh_result nh_feed(nh_parser *p, const uint8_t *in, size_t in_len,
                  uint8_t *out, size_t out_cap)
{
    nh_result res;
    res.consumed = 0;
    res.produced = 0;

    while (res.consumed < in_len) {
        if (p->state == NH_ST_DONE || p->state == NH_ST_ERROR) break;

        uint8_t c = in[res.consumed];

        switch (p->state) {

        case NH_ST_CONNECT:
            if (line_push(p, c)) {
                if (p->line_len == 0) { /* ligne vide avant CONNECT : ignorer */ }
                else if (starts_ci(p->line, "CONNECT")) {
                    p->state = NH_ST_STATUS;
                }
                else if (starts_ci(p->line, "NO CARRIER") ||
                         starts_ci(p->line, "ERROR") ||
                         starts_ci(p->line, "NO ANSWER")) {
                    /* Le modem a refusé avant tout HTTP : erreur de transport,
                     * pas une erreur HTTP — l'appelant la remonte en errno. */
                    p->error = NH_ERR_MODEM;
                    p->state = NH_ST_ERROR;
                }
                else if (starts_ci(p->line, "HTTP/")) {
                    /* Pas de CONNECT (variante de firmware) : on accepte. */
                    goto status_line;
                }
                line_reset(p);
            }
            res.consumed++;
            break;

        case NH_ST_STATUS:
            if (line_push(p, c)) {
                res.consumed++;
            status_line:
                if (starts_ci(p->line, "HTTP/")) {
                    const char *s = p->line;
                    while (*s && *s != ' ') s++;      /* saute « HTTP/1.1 » */
                    p->status = (uint16_t)parse_dec(skip_ws(s));
                    p->state = NH_ST_HEADERS;
                } else if (p->line_len == 0) {
                    /* ligne vide : tolérée avant la status line */
                } else {
                    p->error = NH_ERR_NO_STATUS;
                    p->state = NH_ST_ERROR;
                }
                line_reset(p);
                break;
            }
            res.consumed++;
            break;

        case NH_ST_HEADERS:
        case NH_ST_TRAILER:
            if (line_push(p, c)) {
                int end = (p->line_len == 0);
                if (!end && p->state == NH_ST_HEADERS) {
                    if (starts_ci(p->line, "content-length:")) {
                        p->content_length = parse_dec(skip_ws(p->line + 15));
                        p->has_length = 1;
                    } else if (starts_ci(p->line, "transfer-encoding:")) {
                        const char *v = skip_ws(p->line + 18);
                        /* « chunked » peut être précédé d'autres codages ; on ne
                         * gère que le cas où il est présent (le seul observé). */
                        for (const char *q = v; *q; q++) {
                            if (starts_ci(q, "chunked")) { p->chunked = 1; break; }
                        }
                    }
                }
                line_reset(p);
                if (end) {
                    if (p->state == NH_ST_TRAILER)   p->state = NH_ST_DONE;
                    else if (p->chunked)             p->state = NH_ST_CHUNK_SIZE;
                    else if (p->has_length)          p->state = (p->content_length == 0)
                                                                ? NH_ST_DONE : NH_ST_BODY_LEN;
                    else                             p->state = NH_ST_BODY_EOF;
                }
            }
            res.consumed++;
            break;

        case NH_ST_BODY_LEN:
            if (res.produced >= out_cap) goto out_full;
            out[res.produced++] = c;
            p->body_total++;
            res.consumed++;
            if (p->body_total >= p->content_length) p->state = NH_ST_DONE;
            break;

        case NH_ST_BODY_EOF: {
            /* Pas de cadrage : la fin est la fermeture, annoncée par NO CARRIER.
             * On retarde l'émission des octets qui pourraient l'amorcer. */
            nc_out o = nc_step(p, c);
            if (o.n && res.produced + o.n > out_cap) goto out_full;
            for (uint8_t i = 0; i < o.n; i++) { out[res.produced++] = o.emit[i]; p->body_total++; }
            res.consumed++;
            if (o.hit) p->state = NH_ST_DONE;
            break;
        }

        case NH_ST_CHUNK_SIZE:
            if (line_push(p, c)) {
                res.consumed++;
                if (p->line_len == 0) { line_reset(p); break; }   /* CRLF résiduel */
                int ok = 0;
                uint32_t n = parse_hex(p->line, &ok);
                line_reset(p);
                if (!ok) { p->error = NH_ERR_BAD_CHUNK; p->state = NH_ST_ERROR; break; }
                if (n == 0) { p->saw_chunk_zero = 1; p->state = NH_ST_TRAILER; }
                else        { p->chunk_left = n;     p->state = NH_ST_CHUNK_DATA; }
                break;
            }
            res.consumed++;
            break;

        case NH_ST_CHUNK_DATA:
            if (res.produced >= out_cap) goto out_full;
            out[res.produced++] = c;
            p->body_total++;
            res.consumed++;
            if (--p->chunk_left == 0) p->state = NH_ST_CHUNK_CRLF;
            break;

        case NH_ST_CHUNK_CRLF:
            /* CRLF de fin de chunk : consommé sans être émis. */
            res.consumed++;
            if (c == '\n') { line_reset(p); p->state = NH_ST_CHUNK_SIZE; }
            break;

        default:
            res.consumed++;
            break;
        }
    }
    return res;

out_full:
    return res;
}
