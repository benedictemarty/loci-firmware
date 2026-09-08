# Changelog — loci-firmware

Format inspiré de [Keep a Changelog](https://keepachangelog.com/).

## [non publié]

### 2026-09-08 — `copy_to_ram` : corrige le boot sur MATÉRIEL RÉEL (1ʳᵉ extension validée sur silicium)

**Validé sur la vraie cartouche LOCI (sans Oric).** Les builds en « stratégie C » (binaire
**FLASH/XIP** via `pico_set_binary_type(... default)` + linker `memmap_xram_mia_flash.ld`) **bootaient
dans l'émulateur mais PAS sur silicium** (LED éteinte = `led_init` jamais atteint), alors que le
firmware LOCI officiel (`copy_to_ram`) bootait (LED rouge). L'émulateur charge l'ELF sans modéliser le
XIP/boot2 → le XIP custom passait en émul mais échouait sur la flash réelle. *(Hypothèse implicit-int/
gcc-14 écartée : 0 déclaration implicite dans tout le build.)*

**Correctif** (`src/CMakeLists.txt`) : `feature/stream-bank-A8` repasse en **`copy_to_ram`** (comme
l'officiel). Linke à **RAM ~80 %** (210/256 Ko) — tient largement. **Procédure de test HW** (flash via
`RPI-RP2` en BOOTSEL) : XIP → LED éteinte ; `copy_to_ram` → **LED ROUGE = boote**. ⟹ **1ʳᵉ firmware
custom LOCI (`$A8`) qui tourne sur matériel réel.**

> ⚠️ La stratégie C (XIP) sert à libérer de la RAM pour empiler beaucoup de features. Elle **casse le
> boot HW en l'état** : ne la réactiver qu'après avoir fait fonctionner le XIP/boot2/linker sur
> silicium. Impacte aussi les autres builds XIP (`full-A7`, `coproc-A9`) → à repasser en `copy_to_ram`
> (s'ils tiennent) pour tourner sur matériel.

### 2026-09-01 — Opcode `$A8` MIA_OP_STREAM_BANK (branche `feature/stream-bank-A8`)

Base : branche `feature/stream-bank-A8`, créée sur `fix/full-A7` (qui apporte
l'opcode banking `$A7` / `map_api_set_bank` / `mia_set_bank`).

#### Added
- `src/mia/api/std.c` : handler **`std_api_stream_bank()`** (opcode `$A8`) —
  streamer read-only fichier → banque 16 Ko en un seul fastcall
  (`lseek` SEEK_SET + `read` dans `xram[(SEL<<14)+dst]`, puis mapping optionnel
  `$C000-$FFFF` si bit MAP). Calqué sur `std_api_lseek` / `std_api_read_xram`.
  Argument A = `MAP|SEL` ; xstack `fd,off,dst,len`. Retour AX = octets lus.
- `src/mia/main.c` : dispatch `case 0xA8`. `src/mia/api/std.h` : prototype.
- `tests/stream_bank_model_test.c` (+ `tests/Makefile`) : tests natifs de la
  logique pure (bornage `len`, adressage, rejets EINVAL, garde débordement,
  mapping conditionnel). 6 cas OK.

#### Notes
- Mapping `map=1` via **`mia_set_bank(sel,true)`**, même chemin (cœur 0) que
  `$A7` — pas de routage cœur 1 réinventé (question ouverte commune `$A7`/`$A8`,
  spec §4.1-5). Lecture flash/LFS synchrone = seul blocage cœur 0 toléré.
  `__dmb()` après lecture avant mapping.
- Strictement additif : `default: return false` préservé → `$A8` détectable côté
  6502, aucune fonctionnalité existante modifiée.
- **Compilé et lié** (`ninja` → `loci-firmware.elf`, RAM 26,35 %). ⚠️ **Non
  validé en runtime** : validation Phosphoric `--loci` puis matériel restante.
- Réf : `extensions/streamer-A8/spec-streamer-assets.md` (niveau 1, §4).

### 2026-08-17 — Stratégie C portée : binaire FLASH/XIP, RAM libérée, STD_FIL_MAX restauré à 16

Portage sur la branche `webdisk` de la **Stratégie C** conçue dans `loci-fw`
(`refactor/strategie-c`) : passage du binaire `copy_to_ram` (tout le code en RAM,
saturée par l'ajout du backend web) à un binaire **FLASH/XIP** où seul le code
temps-réel reste en RAM. **Objectif atteint** : annuler le débordement RAM qui
avait forcé le workaround `STD_FIL_MAX 16→10` (commit `a4f6e93`) et autoriser un
build `Release`.

Changements (identiques à la Stratégie C amont) :
- `src/memmap_xram_mia_flash.ld` : nouveau linker script FLASH/XIP (copié tel
  quel — la base `memmap_xram_mia.ld` est identique entre les deux dépôts).
- `src/CMakeLists.txt` : `pico_set_binary_type(... default)` + nouveau `.ld`.
- `src/mia/sys/mia.c` : `act_loop()` en `__not_in_flash()` (`.time_critical`, RAM)
  + `-fno-jump-tables` / `-fno-tree-switch-conversion` (sinon la jump table du
  switch resterait en `.rodata`/FLASH → lecture flash à chaque transaction bus).
- `src/mia/api/api.h` : `always_inline` sur `api_set_ax`/`api_return_ax`/
  `api_return_blocked`/`api_return_released` (pas de copie hors-ligne en flash).
- `src/mia/api/std.c` : **`STD_FIL_MAX` restauré à 16** (workaround devenu inutile).
- `CMakeLists.txt` (racine) : `-Wno-implicit-function-declaration -Wno-implicit-int
  -Wno-error` (gcc 14.2 vs pico-sdk épinglé).

Résultat mesuré (build **Release `-O3`**, toolchain arm-none-eabi-gcc 14.2,
`locirom` embarquée) : le firmware complet **compile et lie**, `loci-firmware.uf2`
(392 Ko) produit.

| Région | copy_to_ram (avant, -Os, FIL_MAX 10) | **Stratégie C (Release, FIL_MAX 16)** |
|---|---:|---:|
| RAM | 98,67 % (débordait de ~11 Ko en Release) | **25,48 % (50 096 B)** |
| FLASH | ~199 Ko | 196 120 B (1,17 % de 16 Mo) |

**Preuve statique (critère §4 du doc amont)** : `act_loop` @ `0x200100c0` (RAM),
**aucune** référence de donnée flash (`.word 0x10xxxxxx`) dans son désassemblage,
et ses 16 appels `bl` pointent tous vers la RAM (`0x2001_xxxx` : `acia_*`,
`tap_act`, act_loop). Aucun flash sur le chemin bus → pas de latence XIP ni de
risque de coupure XIP par une écriture lfs.

⚠️ **Validation runtime NON faite** (pas de matériel LOCI ici) : flasher le
`.uf2`, vérifier le boot (le `.ld` change la structure de démarrage — récupérable
par BOOTSEL), confirmer `mia_act_rxstall_cnt` stable à 0, et non-régression
ACIA/modem + disque Microdisc/web. Rollback : `git checkout` du fichier
`src/CMakeLists.txt` (le `memmap_xram_mia.ld` d'origine est conservé).

### 2026-06-21 — Note de contexte (aucun changement de firmware)

Le travail TLS/SSL de la session (terminaison TLS, vérification de certificat,
correctifs USB/WiFi) a été réalisé **entièrement dans le dongle**
`PicoWiFiModemUSB` (release v0.2.0).

**LOCI reste transparent vis-à-vis du TLS** : il n'échange que des données série
déjà déchiffrées et n'a donc requis **aucune modification de code**. Cette entrée
documente uniquement la décision ; le firmware LOCI est inchangé.
