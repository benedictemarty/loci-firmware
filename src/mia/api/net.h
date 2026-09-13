/*
 * Copyright (c) 2026 bmarty
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * net.h — device réseau `N:` (URL-as-file), opcode `$B7`.
 *
 * Donne à l'Oric un accès réseau SOUS FORME DE FICHIER : le 6502 fait
 * `open("N:https://host/path")` puis `read_xram`/`close`, et LOCI pilote le
 * modem AT (PicoWiFiModemUSB) pour son compte. Spec :
 * `extensions/net-device-B7/spec-net-device.md`.
 *
 * TRANSPORT : `ATGET` — le dongle termine le TLS mais **ne parse pas le HTTP**.
 * Il relaie « CONNECT », la status line et les en-têtes bruts, le corps
 * (éventuellement en `Transfer-Encoding: chunked`), puis « NO CARRIER ». Le
 * décodage est fait par `net_http.c`, validé par 27 tests natifs sur des
 * captures réelles du dongle. (La commande `ATDISKRD` utilisée par
 * `oric/dsk_web.c` n'est PAS reconnue par le firmware du dongle — vérifié le
 * 2026-09-10 sur v0.3.3 et absent de ses sources — elle n'est donc pas une
 * option ici.)
 *
 * MODÈLE : flux, pas bufferisation complète. Un corps peut peser plus que la
 * RAM disponible (une image disque !), donc `net_task()` remplit un anneau de
 * quelques Ko que `read_xram` vide. Le 6502 voit un fichier séquentiel.
 *
 * ARBITRAGE avec le mode passe-plat (ACIA `$0380` transparente, cf. OricTel) :
 * le canal AT est UNIQUE. Tant qu'une transaction `N:` est ouverte,
 * `net_owns_modem()` est vrai et `acia_task()` NE consomme PAS les octets du
 * modem — sinon les deux se voleraient le flux. Les deux modes restent donc
 * mutuellement exclusifs, sans qu'aucun ait à connaître l'autre.
 */

#ifndef _NET_H_
#define _NET_H_

#include <stdbool.h>
#include <stdint.h>

/* Nombre de descripteurs réseau. Le canal AT du modem est unique : une seule
 * transaction à la fois (spec §7 QO 1). */
#define NET_FD_MAX 1

/* Kernel events */
void net_task(void);
void net_stop(void);

/* Vrai tant qu'une transaction `N:` détient le lien modem : `acia_task()` doit
 * alors laisser le flux tranquille. */
bool net_owns_modem(void);

/* Vrai si le chemin désigne le device réseau (« N: », insensible à la casse). */
bool net_is_path(const uint8_t *path);

/* Ouvre une URL. `path` pointe le chemin complet (« N:https://… »), `flags` = les
 * flags cc65 de `open`. Renvoie 0 si l'ouverture est armée, sinon un errno API
 * (`API_ENODEV` pas de modem, `API_EMFILE` déjà ouvert, `API_EINVAL` URL vide).
 * N'ATTEND PAS la réponse : `net_read` s'en charge (machine à états). */
int net_open(const uint8_t *path, uint8_t flags);

/* Lit jusqu'à `count` octets de corps dans `dst`.
 *  >  0 : octets livrés
 *  == 0 : fin de corps atteinte (EOF)
 *  == -1 : RIEN DE PRÊT — l'appelant doit rendre la main sans répondre au 6502
 *          (BUSY reste posé) et rappeler au tour suivant
 *  == -2 : erreur ; `net_errno()` en donne la raison */
int32_t net_read(uint8_t *dst, uint16_t count);

/* Écriture (lot 2, fd ouvert O_WRONLY) : ajoute count octets au corps du PUT.
 * Renvoie count, -2 si le fd n'est pas en écriture, -3 si le corps dépasse le
 * tampon (2 Ko). Le PUT est émis au close() (ATDISKWR<url>?len=N + corps). */
int32_t net_write(const uint8_t *src, uint16_t count);
/* Sur un flux tcp:// (lot 3) : envoie directement (renvoie le nombre accepté, 0 si
 * le modem n'a pas de place — réessayer). */

/* Ferme la transaction et rend le lien modem au mode passe-plat. */
void net_close(void);

/* Dernier errno API produit par le backend réseau. */
uint8_t net_errno(void);

/* Opcode `$B7` — net_control. `API_A` = sous-fonction (0 = status). */
void net_api_control(void);
/* $B7 A=4 : chemin sur le xstack → valeur poussée sur le xstack ; longueur ou -errno. */
int net_json_query(void);

#endif /* _NET_H_ */
