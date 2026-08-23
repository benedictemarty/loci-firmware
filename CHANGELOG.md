# Changelog — loci-firmware

Format inspiré de [Keep a Changelog](https://keepachangelog.com/).

## [non publié]

### 2026-08-23 — Trace de l'ACIA 6551 (debug)

**Ajouté** — instrumentation de trace de l'ACIA 6551 émulée (`src/mia/oric/acia.c`).

- Ring buffer lock-free `acia_trc` (256 entrées `{tag,val}`, single-producer
  core1 / single-consumer core0) : les accès registres du 6502
  (`$0380`-`$0383`), servis dans `act_loop()` sur **core1** (temps réel,
  `__not_in_flash`), ne peuvent pas appeler `printf`. Ils enregistrent donc un
  évènement via `acia_trace(tag,val)` ; `acia_task()` (core0) vide le buffer sur
  la console UART0 (GP0/GP1, 115200) via `acia_trace_flush()`.
- Points instrumentés : `W` data write (`$0380`), `R` data read (`$0380`),
  `C` cmd (`$0382`), `T` ctrl (`$0383`), `S` status reset (`$0381`),
  `K` clear IRQ (lecture `$0381`), `I` IRQ levée (val = registre status).
- `acia_trace()` forcée `always_inline` : garantit qu'elle réside en RAM avec
  ses appelants `__not_in_flash` (sinon risque de hard fault si core1 l'exécute
  depuis la flash pendant une écriture flash, XIP coupé).
- Réactivation du `printf("ACIA on %d\n", ...)` au montage du modem.
- Compilation conditionnelle via `#define ACIA_TRACE 1` en tête de `acia.c` :
  mettre à `0` pour retirer entièrement la trace du firmware de production.

Sortie type sur la console de debug : `6551 C 09` (DTR+IRQ off), `6551 T 1e`
(9600 8N1), `6551 W 41` (envoi 'A'), `6551 R 0d` (réception CR), `6551 I 88`.

### 2026-06-21 — Note de contexte (aucun changement de firmware)

Le travail TLS/SSL de la session (terminaison TLS, vérification de certificat,
correctifs USB/WiFi) a été réalisé **entièrement dans le dongle**
`PicoWiFiModemUSB` (release v0.2.0).

**LOCI reste transparent vis-à-vis du TLS** : il n'échange que des données série
déjà déchiffrées et n'a donc requis **aucune modification de code**. Cette entrée
documente uniquement la décision ; le firmware LOCI est inchangé.
