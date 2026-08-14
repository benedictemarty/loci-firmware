/*
 * Copyright (c) 2026 bmarty
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Web-backed disk transport for LOCI (projet loci-webdisk, architecture B).
 *
 * LOCI parle au modem PicoWiFi (hôte USB-CDC) et lui fait récupérer des tranches
 * d'une image disque distante via la commande AT propriétaire ATDISKRD (ajoutée au
 * firmware du modem). Le modem retire l'habillage HTTP et renvoie les octets bruts,
 * encadrés par :
 *
 *     \r\n+DISK:<len>\r\n <len octets bruts>   puis  OK
 *
 * Ce protocole a été validé en runtime contre le serveur Python (voir
 * docs/FIRMWARE-INTEGRATION-B.md et CHANGELOG [0.10.0] du dépôt loci-webdisk).
 *
 * Prérequis runtime : le modem doit être monté en USB-CDC (cdc_is_modem) et déjà
 * connecté au WiFi (AT$SSID/AT$PASS/ATC1). La lecture est bloquante et pompe la
 * pile USB hôte pendant l'attente ; elle tourne sur le même cœur que tuh_task/
 * cdc_task (cf. main.c : main_task et task s'exécutent dans la même boucle core0).
 */

#ifndef _DSK_WEB_H_
#define _DSK_WEB_H_

#include <stdbool.h>
#include <stdint.h>

/* Un modem AT est-il monté en USB-CDC ? (backend web disponible) */
bool dsk_web_available(void);

/*
 * Lit `len` octets à l'offset `offset` de l'image dont l'URL de base est `url`
 * (ex. "http://192.168.88.252:8080/disk/game.dsk") vers `buf`. Émet
 * ATDISKRD<url>?offset=<offset>&len=<len> au modem et décode la trame +DISK.
 * Renvoie true si exactement `len` octets ont été reçus, false sinon (timeout,
 * pas de modem, statut d'erreur).
 */
bool dsk_web_read(const char *url, uint32_t offset, uint32_t len, void *buf);

#endif /* _DSK_WEB_H_ */
