# Changelog — loci-firmware

Format inspiré de [Keep a Changelog](https://keepachangelog.com/).

## [non publié]

### 2026-06-21 — Note de contexte (aucun changement de firmware)

Le travail TLS/SSL de la session (terminaison TLS, vérification de certificat,
correctifs USB/WiFi) a été réalisé **entièrement dans le dongle**
`PicoWiFiModemUSB` (release v0.2.0).

**LOCI reste transparent vis-à-vis du TLS** : il n'échange que des données série
déjà déchiffrées et n'a donc requis **aucune modification de code**. Cette entrée
documente uniquement la décision ; le firmware LOCI est inchangé.
