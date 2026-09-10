/*
 * Copyright (c) 2026 bmarty
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * SOURCE DE VÉRITÉ : `extensions/net-device-B7/src/net_http.{c,h}`, où vivent
 * les 27 tests natifs (`make test`) et les captures réelles du dongle. Ce fichier
 * en est la copie greffée ; toute correction se fait là-bas puis se recopie ici.
 *
 * net_http.h — parseur de réponse `ATGET` du PicoWiFiModemUSB, pour le device
 *              réseau `N:` (opcode `$B7`). Voir spec-net-device.md §1.1.
 *
 * POURQUOI CE MODULE : le dongle termine le TLS mais **ne parse pas le HTTP**.
 * Il relaie `CONNECT 9600`, puis la status line et les headers BRUTS, puis le
 * corps — éventuellement en `Transfer-Encoding: chunked` —, puis `NO CARRIER`.
 * Livrer ce flux tel quel au 6502 lui donnerait des en-têtes HTTP et des tailles
 * de blocs en hexadécimal mélangées à ses données. Ce parseur extrait le **code
 * de statut** et le **corps dé-chunké**, et rien d'autre.
 *
 * CONTRAINTES DE LA CIBLE (firmware RP2040, core 0) :
 *  - aucune allocation : tout l'état tient dans `nh_parser` (fourni par l'appelant) ;
 *  - INCRÉMENTAL : le réseau arrive par fragments arbitraires, une ligne d'en-tête
 *    ou même une taille de chunk peut être coupée en deux lectures ;
 *  - SORTIE BORNÉE : `nh_feed` s'arrête quand le buffer de sortie est plein et
 *    signale ce qu'il a consommé, pour que l'appelant reprenne où il en était
 *    (patron de la machine à états `std_api_read_xram`, socle §5.3) ;
 *  - pas de `printf`, pas de `malloc`, pas de récursion.
 *
 * Ce fichier est l'implémentation de RÉFÉRENCE, validée par des tests natifs sur
 * des captures RÉELLES du dongle (`tests/fixtures`, fichiers `.raw`). Elle est destinée à
 * être greffée dans `firmware/src/mia/api/` — d'où l'absence de dépendance autre
 * que <stdint.h>/<stddef.h>.
 */
#ifndef NET_HTTP_H
#define NET_HTTP_H

#include <stdint.h>
#include <stddef.h>

/* Longueur max d'une ligne de statut ou d'en-tête retenue. Au-delà, la ligne est
 * TRONQUÉE pour l'analyse mais le flux reste synchronisé (on continue de chercher
 * son CRLF) : un serveur bavard ne doit pas désynchroniser le parseur. */
#define NH_LINE_MAX 128

typedef enum {
    NH_ST_CONNECT = 0,  /* avant/pendant la ligne « CONNECT … » du modem */
    NH_ST_STATUS,       /* ligne « HTTP/1.1 200 OK » */
    NH_ST_HEADERS,      /* en-têtes, jusqu'à la ligne vide */
    NH_ST_BODY_LEN,     /* corps de longueur connue (Content-Length) */
    NH_ST_BODY_EOF,     /* corps jusqu'à fermeture (ni CL ni chunked) */
    NH_ST_CHUNK_SIZE,   /* ligne de taille hexa d'un chunk */
    NH_ST_CHUNK_DATA,   /* données d'un chunk */
    NH_ST_CHUNK_CRLF,   /* CRLF de fin de chunk */
    NH_ST_TRAILER,      /* en-têtes de fin après le chunk nul */
    NH_ST_DONE,         /* corps complet — fin d'autorité atteinte */
    NH_ST_ERROR         /* flux inexploitable (voir nh_error) */
} nh_state;

typedef enum {
    NH_ERR_NONE = 0,
    NH_ERR_NO_STATUS,   /* pas de status line HTTP reconnaissable */
    NH_ERR_BAD_CHUNK,   /* taille de chunk illisible */
    NH_ERR_MODEM        /* le modem a répondu ERROR / NO CARRIER avant le HTTP */
} nh_error;

typedef struct {
    nh_state state;
    nh_error error;

    uint16_t status;          /* code HTTP (0 tant qu'inconnu) */
    uint32_t content_length;  /* valide si has_length */
    uint8_t  has_length;
    uint8_t  chunked;

    uint32_t body_total;      /* octets de CORPS produits depuis nh_init */

    char     line[NH_LINE_MAX];
    uint16_t line_len;
    uint8_t  line_over;       /* ligne tronquée (dépassement de NH_LINE_MAX) */

    uint32_t chunk_left;      /* octets restants du chunk courant */
    uint8_t  saw_chunk_zero;  /* le chunk nul a été vu */

    uint8_t  nc_match;        /* avancement de la détection de « \r\nNO CARRIER » */
} nh_parser;

/* Résultat d'un appel à nh_feed. */
typedef struct {
    size_t consumed;   /* octets d'entrée traités */
    size_t produced;   /* octets de CORPS écrits dans out */
} nh_result;

/* Remet le parseur à zéro. À appeler avant chaque transaction (chaque `open`). */
void nh_init(nh_parser *p);

/*
 * Consomme jusqu'à `in_len` octets et écrit jusqu'à `out_cap` octets de corps.
 *
 * S'arrête (consumed < in_len) quand `out` est plein : rappeler avec le reste de
 * l'entrée. S'arrête aussi une fois le corps terminé (`NH_ST_DONE`) : les octets
 * suivants — `NO CARRIER` du modem — ne sont PAS consommés, l'appelant décide
 * quoi en faire.
 *
 * `out` peut être NULL avec `out_cap` = 0 : le parseur avance alors dans les
 * en-têtes sans produire de corps (utile pour attendre le code de statut).
 */
nh_result nh_feed(nh_parser *p, const uint8_t *in, size_t in_len,
                  uint8_t *out, size_t out_cap);

/* 1 si le corps est complet (fin d'autorité : Content-Length atteint, chunk nul,
 * ou fermeture pour un corps sans cadrage). */
int nh_done(const nh_parser *p);

#endif /* NET_HTTP_H */
