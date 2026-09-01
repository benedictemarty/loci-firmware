# Changelog — loci-firmware

Format inspiré de [Keep a Changelog](https://keepachangelog.com/).

## [non publié]

### 2026-09-01 — ACIA RX : handshake `acia_stat_checked` (branche `feature/acia-rx-lossless`)

Base : branche `feature/acia-rx-lossless`, créée par-dessus le hotfix 17
(`17-please-help-track-down-my-acia-failings` : mapping ACIA configurable
`$0380`↔`$0340` + correction de l'enable-map R/W).

#### Added
- `src/mia/oric/acia.c` : drapeau de handshake RX **`acia_stat_checked`**, repris
  du design initial de Sodium (branche `feature/acia`, où il n'existait qu'à
  l'état de brouillon commenté). Il est posé à `true` quand le 6502 lit le
  registre STATUS (`acia_clr_irq`), remis à `false` quand le cœur 0 vient de
  stager un nouvel octet RX.
- `tests/acia_rx_model_test.c` + `tests/Makefile` : premiers tests natifs RX
  (modèle de logique, sans matériel). Couvrent : intégrité FIFO de bout en bout,
  slot non lu jamais écrasé (pas de perte au handoff), buffer vide, handshake
  IRQ, distance de sécurité du ring.

#### Changed
- Le rappel IRQ RX périodique (compteur `cnt`) ne se déclenche plus **que tant
  que le CPU n'a pas lu le STATUS** (`!acia_stat_checked`). Une fois le statut
  acquitté, on cesse de ré-asserter l'IRQ inutilement.

#### Notes
- Staging RX **inchangé et mono-cœur** (cœur 0) : `acia_read()` (cœur 1) ne fait
  que libérer le slot. Aucune écriture concurrente du slot/ring entre cœurs.
- **Compilé et lié** (`ninja` → `loci-firmware.elf`). ⚠️ **Non validé en
  runtime** : validation Phosphoric (`--loci`) puis matériel restant à faire.
- Réf : forum defence-force t=2926 (réponses Sodium 31/08–01/09/2026) ;
  branche de référence `origin/feature/acia` (`acia_stat_checked`).

### 2026-06-21 — Note de contexte (aucun changement de firmware)

Le travail TLS/SSL de la session (terminaison TLS, vérification de certificat,
correctifs USB/WiFi) a été réalisé **entièrement dans le dongle**
`PicoWiFiModemUSB` (release v0.2.0).

**LOCI reste transparent vis-à-vis du TLS** : il n'échange que des données série
déjà déchiffrées et n'a donc requis **aucune modification de code**. Cette entrée
documente uniquement la décision ; le firmware LOCI est inchangé.
