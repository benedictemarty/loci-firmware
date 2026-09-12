# Changelog — loci-firmware

Format inspiré de [Keep a Changelog](https://keepachangelog.com/).

## [non publié]

### 2026-09-13 — branche `feature/fs-posix` : `$1E SYNCFS`, `$1F STAT`, `$84 CHDIR`, `$85 GETFREE` ; `xstack_ptr` initialisé

Complément POSIX (spec `extensions/fs-posix`, strictement additif) : quatre handlers minces
(`std.c`, `dir.c`) + quatre `case` dans `main_api`. **STAT et SYNCFS ont le même opcode et la
même ABI que RP6502** (`f_stat_t` 282 octets dépilée champ par champ ; `syncfs(fd)`, `$FF` =
tous) ; GETFREE dépile `free`, `total` comme `f_getfree` amont et rend `csize` en AX ; CHDIR
fait aussi `f_chdrive` (sinon les chemins relatifs restaient sur l'ancien volume) et n'accepte
que la racine sur littlefs. Validé par le bus (`emul` `test_fsposix`, 21 suites) et par un
**vrai programme 6502** (`extensions/fs-posix/tests/oric/fstest`, façade `loci_fs` de
`loci.lib`) : `FSTEST OK`.

**Correctif trouvé en chemin — `xstack_ptr` non initialisé** (`sys/mem.c`) : il partait de 0
et n'était posé à `XSTACK_SIZE` que par `api_zxstack()` (boot par LOCI ou opcode 0). Un
programme lancé sans passer par LOCI (Oric transparent, cassette) perdait ses premiers
arguments — le push de `$03AC` est gardé par `if (xstack_ptr)` — et `open` recevait un chemin
vide (`FR_NOT_ENABLED`). Patch `extensions/upstream-fixes` 0008.

### 2026-09-12 — `dsk_act()` aligné sur le correctif STEP_OUT ; état des correctifs `upstream-fixes` sur cette branche

`dsk_cmd()` (chemin réel : FIFO → `dsk_task`) porte déjà le correctif STEP_OUT (piste−1,
direction « out ») ; `dsk_act()`, appel commenté dans `mia.c`, gardait l'ancien bug — aligné.
Vérifié par `~/loci/emul` `test_dsk` 3b (STEP_IN ×2, STEP_OUT, STEP à direction mémorisée,
lecture exacte ensuite). Inventaire : les patchs `extensions/upstream-fixes` 0001-0005 sont tous
présents sur cette branche ; 0006 (délai d'écriture FDC par octet) est sur
`fix/dsk-write-interbyte-timeout`, 0007 (mode ACIA persistant) concerne la base amont 17.

### 2026-09-12 — RECTIFICATIF : booter sur un disque EST possible en co-simulation

L'entrée du 10/09 ci-dessous (« BOOTER sur le disque distant est IMPOSSIBLE EN CO-SIMULATION »)
diagnostiquait le gel de la machine à états de boot après `Fast boot ON`. Le diagnostic
« core0 ne progresse qu'en sync avec les transactions bus » était incomplet : en boot rapide
le 6502 **poll `$03B1`** (`BVC *` posé par `api_return_boot`) et chaque poll fait avancer le
firmware — le chargement des ROMs aboutit bien. Les vrais manques étaient dans le banc :
1. la fenêtre **Microdisc `$0310-$0318` n'était pas co-simulée** (servie par le modèle interne
   de Phosphoric, qui ne voyait pas le `.dsk` monté par le firmware) ;
2. la **carte haute** servie par LOCI était figée sur `oric_bank3` : `microdis.rom` (chargée
   par `rom_load_raw("microdis.rom", 0xA000)` = `oric_bank2 + $2000`) n'était jamais servie
   sur `$E000-$FFFF`, ni la RAM overlay sous `$C000-$DFFF` (MAP).

Les deux sont corrigés côté `~/loci/emul` (`c788d3a`) et `~/Oric1` (`da0ae2b`) : **`SEDORIC
V3.0` boote depuis `1:/SEDORIC3.DSK` monté sur `A:` dans le vrai menu, menu autorun identique
à la référence**. Aucun changement firmware. **Scénario `W:` rejoué le même jour** (build
`build-xip-web/`, `-DLOCI_WEBDISK_BASE=http://127.0.0.1:8090`, `disk_server.py --writable`,
modem factice `emul/tools/mock_modem.py` sur PTY) : Sedoric boote depuis le disque web et
`SAVE"COSIM"` écrit 6 `PUT` (`dsk_flush_track` → `ATDISKWR`), `DIR` liste le fichier.

Deux comportements firmware **observés** au passage (pas de correctif, à évaluer sur
silicium) :
- `DSK_WRITE` n'attend l'octet suivant que `dsk_rw_countdown = 2 × data_len` passes de
  `dsk_task` (`oric/dsk.c`) ; en lecture c'est `200 ×`. Si une passe de la boucle noyau est
  rapide devant l'intervalle entre deux `STA $0313` du 6502, un secteur écrit est tronqué
  (les octets suivants tombent dans le registre DATA inerte, sans erreur). Le banc a dû geler
  core0 pendant la transaction pour l'éviter.
- `INTRQ` est **rejoué en impulsion à chaque passe** tant que `dsk_reg_irq == 0x00` (fin de
  `dsk_task`) : sur un 6502 câblé en niveau c'est neutre, mais tout consommateur en front
  (banc, analyseur) voit une rafale.

### 2026-09-10 — le device web `W:` devient atteignable : option `LOCI_WEBDISK_BASE`

Le pseudo-périphérique **`W: Web disks`** existait dans le firmware depuis longtemps — liste
racine (`api/dir.c:235`), `opendir("W:")` qui récupère `GET {base}/disks` et en décode le JSON
(`api/dir.c:85-95`), `mount("W:jeu.dsk")` qui attache `{base}/disk/jeu.dsk` (`api/mnt.c:47-56`)
— le tout conditionné à `dsk_web_base()`, c'est-à-dire au define `LOCI_WEBDISK_BASE`.

**Mais aucune option du build ne permettait de définir cette macro** : le device restait
invisible quoi qu'on fasse, et son code n'avait donc jamais pu tourner. D'où l'option :

    cmake -DLOCI_WEBDISK_BASE=http://192.168.1.10:8080 …
    -- LOCI: device web W: activé sur http://192.168.1.10:8080

Vide (défaut) = device masqué, aucun accès réseau au démarrage.

**Résultat, dans le menu LOCI et pour la première fois** (co-simulation + dongle physique +
serveur `disk_server.py` réel) :

    [0: Internal storage [15MB]        ^W:----------------[.dsk  ][x]
    [2: CDC modem mounted              |/..
    [W: Web disks                      | e2e.dsk
                                       | sedoric3.dsk

puis, après sélection, **`A: sedoric3.dsk` monté** avec « Microdisc on ». Le montage n'est pas
cosmétique : `dsk_mount_web` lit l'en-tête du disque (20 octets) **par HTTP** avant d'accepter
— le transport `ATGET` est donc exercé de bout en bout par le menu.

Le protocole est **HTTP/1.1 + JSON**, pas du FTP ni un autoindex web : `GET /disks` pour la
liste, `GET /disk/<nom>?offset=&len=6400` (réponse `206`) pour les pistes, `PUT` pour les
écritures. `W:` attend donc l'API de `disk_server.py` ; c'est `N:` (`$B7`) qui accepte n'importe
quelle URL.

**Les ROMs Oric dans le FS interne** — `!ROM` disparaît en déposant les ROMs dans
`src/roms/` avant compilation : le mécanisme `EMBEDDED_EXTRA_ROMS` (`sys/ext.c`,
`create_resources` du CMakeLists) les écrit dans le littlefs au premier boot, et elles
apparaissent dans `0:` (`basic10.rom`, `basic11b.rom`, `microdis.rom`). Le champ `rom:` du menu
les propose alors — raccourci `o` pour y aller, filtre `.rom` appliqué automatiquement — et
« Oric ROM » passe à `Custom`. (`src/roms` est dans le `.gitignore` : ces ROMs restent hors
dépôt.)

**BOOTER sur le disque distant est IMPOSSIBLE EN CO-SIMULATION — limite du banc, pas du
firmware.** Diagnostic établi par le journal du firmware :

    dsk web tr:80/1{MNT ok W:/sedoric3.dsk}DEV ROM loaded ok
    Fast boot ON

Le montage réussit, la géométrie est détectée, la ROM Microdisc est chargée — puis **plus
rien** : ni `BIOS loaded ok`, ni `!rom_load 11 failed`. La machine à états de boot
(`MIA_LOADING_DEVROM` → `MIA_LOADING_BIOS` → chargement du BASIC → pose de `nROMDIS`,
`sys/mia.c:405`) **ne progresse jamais**.

La raison est dans le co-sim, pas ici : `loci_emu_tick()` n'est **jamais appelée**
(`~/Oric1/src/main.c:1307` explique pourquoi — la faire tourner entre deux transactions
désynchronise le service de bus et figeait le menu). Le firmware n'avance donc **qu'en sync
avec les transactions bus** : dès que le 6502 cesse d'accéder à `$03xx`, core0 gèle. Or le boot
exige exactement le contraire — que core0 progresse **seul** pendant que le 6502 est en reset.

`nROMDIS` reste donc à 0, LOCI reste transparent, et l'Oric démarre sur sa **propre** ROM (écran
BASIC 1.1). `!ROM` est le retour de `mia_api_boot` que le menu affiche quand le boot ne prend
pas la main (`rom/src/main.c:413-439`).

Le **montage**, lui, est synchrone — déclenché par un accès 6502 — d'où sa réussite. À valider
sur matériel réel, où core0 tourne en continu. Vaut pour tout ce qui dépendra d'une progression
autonome du firmware.

**Navigation du menu au clavier** (pour les tests headless, m'a coûté plusieurs essais) :
`\d`/`\u` déplacent, **ESPACE** agit (pas RETURN), `\e` = boot, `?` remonte d'un niveau,
`o` saute au champ ROM, `f` édite le filtre, `w` monte l'URL du champ path en A:. Le popup
**mémorise le dernier chemin** : après une sélection dans `0:`, l'ouverture suivante y reste et
le filtre peut ne rien trouver — d'où le `?` avant de changer de device.

### 2026-09-10 — `$B7` : la lecture réseau ne survivait pas au rappel de `api_task` (trouvé par un vrai programme 6502)

Le device `N:` rend la main **sans répondre** tant que rien n'est prêt (BUSY maintenu,
patron socle §5.3) ; `api_task` rappelle alors `main_api` avec le **même opcode**. Mais les
paramètres avaient déjà été dépilés au premier passage : le second redépilait un **xstack
vide** et rendait `EINVAL`. Toute lecture réseau qui n'était pas servie du premier coup —
c'est-à-dire toute lecture réelle, le réseau n'étant jamais instantané — échouait donc.

**Correctif** : les paramètres sont mémorisés (`net_rd_active`, `net_rd_addr`,
`net_rd_count`) et l'état de reprise est testé **avant** tout dépilage, exactement comme
`std_api_read_xram` le fait pour stdin. `close` annule une lecture restée en attente.

**Pourquoi les tests ne l'avaient pas vu** : `emul/tests/test_net.c` appelle `net_read` en C
direct, sans passer par `api_task` — il ne pouvait structurellement pas rencontrer le rappel.
Il a fallu un **vrai programme 6502** (`extensions/net-device-B7/tests/oric/nettest.c`,
façade cc65, cible atmos) pour le déclencher : `open` réussissait (`fd=21`) puis `read`
rendait `EINVAL` alors que `$B7 status` montrait la transaction bien en réception
(`state=2`).

**Validé de bout en bout** (Oric → façade cc65 → API MIA → firmware réel → USB CDC → dongle
→ WiFi → serveur webdisk) :

    open OK, fd=21
    lu 32 octets (attendu 32)
    http=206 state=3 avail=0
    OK : tranche identique, statut 206

« tranche identique » = 32 octets comparés **un par un** à leur référence, `FF` et `00`
compris. Et `http=206 state=3 avail=0` valide au passage l'**ordre de dépilage** de
`$B7 status` entre le firmware et la façade cc65 — un décalage aurait mis 206 dans `avail`.
C'est le point que la note H de `test_net.c` laissait explicitement découvert.

### 2026-09-10 — webdisk : la lecture bascule d'`ATDISKRD` (inexistante) vers `ATGET`
`oric/dsk_web.c` émettait `ATDISKRD`, commande AT propriétaire supposée ajoutée au firmware du
modem. **Elle n'existe pas** : vérifié le 2026-09-10 sur le dongle réel (v0.3.3), où
`ATDISKRD…` est interprété comme `ATD` + « ISKRDhttp:0 » (« DIALLING SKRDhttp:0 » puis
`NO CARRIER`), et **absente des sources du dongle** (`~/picowifi`). Ce transport était donc
**inopérant depuis le début** — ce qui explique qu'il n'ait jamais pu être validé en runtime.

**Bascule de la LECTURE sur `ATGET`**, sans rien toucher au dongle :
- `dsk_web_read` émet `ATGET<url>?offset=<o>&len=<n>` — la **query** et non un en-tête `Range`,
  qu'`ATGET` ne peut pas porter. C'est possible parce que le serveur webdisk accepte déjà cette
  forme (documentée « pratique pour un client minimal qui ne gère pas l'en-tête `Range` ») et
  répond `206 Partial Content` + `Content-Length` exact ;
- `dsk_web_fetch` (liste JSON, taille variable) émet `ATGET<url>` et accepte un corps
  **chunked** ;
- le décodage est fait par `api/net_http.c` (statut, saut des en-têtes, dé-chunkage,
  `Content-Length`), déjà présent pour le device `N:` ;
- nouveau `web_get()` interne, commun aux deux, qui écrit le corps **au fil de l'eau** (pas de
  tampon intermédiaire de la taille d'une piste) et rejette : statut non-2xx, trame illisible,
  et **tranche plus courte que demandée** — servir une piste incomplète au FDC lui ferait lire
  les octets d'une piste précédente ;
- timeout porté de 8 s à **20 s** : en HTTPS le premier octet n'arrive qu'après le handshake
  TLS, mesuré à plusieurs secondes ;
- le décodeur `+DISK:` (`web_recv_header`) est supprimé, devenu mort.

**Bénéfice** : le disque web gagne **HTTPS**, le dongle terminant le TLS.

**VALIDÉ EN RUNTIME** (`emul/tests/test_dskweb.c`, **7/7**) : commande émise exacte, tranche
rendue octet pour octet, tranche courte et statut 404 refusés, `fetch` dé-chunké. Suite
complète de l'émulateur : **15 suites vertes**.

**L'ÉCRITURE redevient possible — par une commande ajoutée au dongle, pas par un contournement.**
`ATPOST` a été **mesuré inadapté au binaire** le 2026-09-10 contre `httpbin.org/post` : il
**supprime les `0x0D`** (6 octets `00 0D 0A 1A FF 41` arrivent en 5), **casse** (`ERROR`) sur un
corps contenant `\r\n.\r\n` qui est son terminateur, et **refuse dès ~3000 octets** quand une
piste en fait 6400. C'est un canal **ligne**, pas un canal d'octets. La conception d'origine
était donc juste : il faut une **commande AT binaire**. Elle a été **implémentée côté dongle**
(dépôt `picowifi`, commit `4af36e7`) : `ATDISKWR<url>?offset=&len=` lit les octets **bruts** et
les streame dans un `PUT`, en répondant `OK`/`ERROR`.

**`dsk_web_write` décode lui-même la réponse**, avec le **même parseur que la lecture**. Le
modem relaie le HTTP brut et ne juge pas (`picowifi` `846b05b`) : mettre un client HTTP dans son
firmware alors que LOCI en a déjà un — `net_http.c`, requis de toute façon pour `$B7` — aurait
fait **deux décodeurs pour un seul besoin**. Le modem transporte, LOCI parle HTTP, dans les deux
sens. `web_get()` devient donc `web_xfer()`, qui streame une charge binaire optionnelle avant de
décoder la réponse ; le scan `OK`/`ERROR` et `web_getc()` disparaissent, devenus morts.

**Corrigé au passage** : seuls les statuts 200 et 206 étaient acceptés. Or un `PUT` répond selon
le serveur **200** (celui du webdisk, avec un corps JSON), **201** ou **204** — tous rejetés.
**Tout 2xx** vaut désormais succès, ce qu'un test a mis au jour.

**Validé en runtime** : `emul/tests/test_dskweb.c` **12/12** — commande exacte, charge binaire
transmise octet pour octet (`0x0D`, `0x0A`, `0x00` et un `.` inclus), `204` sans corps accepté,
**`200` + corps JSON de la vraie réponse du serveur** accepté, `403` refusé sans faux succès.
⚠️ Le dongle doit être **reflashé** pour disposer de la commande ; sur un dongle antérieur,
l'écriture échoue proprement.

**Non testé de bout en bout** : le trajet dongle → serveur local. Le dongle « DIALLING » puis
échoue à ouvrir un TCP vers la machine de développement, alors que celle-ci **ping** le dongle
et que le serveur écoute sur `0.0.0.0` — blocage des connexions **entrantes** (pare-feu local
ou isolation AP). Cette limite prévalait déjà pour `ATDISKRD`.

### 2026-09-10 — device réseau `N:` (opcode `$B7`) : lot 1 « GET », validé en runtime
Donne à l'Oric un accès réseau **sous forme de fichier** : le 6502 fait
`open("N:https://host/path")` puis `read_xram`/`close`, et LOCI pilote le modem AT
(PicoWiFiModemUSB) pour son compte. Spec : `extensions/net-device-B7/spec-net-device.md`.

**Nouveaux fichiers** : `mia/api/net.{c,h}` (device, machine à états, arbitrage du lien
modem) et `mia/api/net_http.{c,h}` (décodage de la réponse `ATGET`).

**Pourquoi un décodeur HTTP dans LOCI** — le contraire de ce que la spec supposait. Mesure
du 2026-09-10 sur le dongle réel (v0.3.3) : il **termine le TLS mais ne parse pas le HTTP**.
Il relaie `CONNECT 9600`, la **status line et les en-têtes bruts**, le corps — souvent en
**`Transfer-Encoding: chunked`** —, puis `NO CARRIER`. Livrer ce flux tel quel au 6502 lui
donnerait des en-têtes et des tailles de blocs hexadécimales au milieu de ses données. D'où
`net_http.c` : status line, saut des en-têtes, **dé-chunkage**, `Content-Length`, et
distinction entre **erreur de transport** (le modem refuse avant tout HTTP → `errno`) et
**statut HTTP** (un 404 remonte tel quel, à l'appli de décider).

**Modèle = flux, pas bufferisation.** Un corps peut peser plus que la RAM (une image
disque) : `net_task()` remplit un anneau de 2 Ko que `read_xram` vide. Anneau plein ⇒ on ne
consomme pas le lien, la contre-pression se fait naturellement dans le buffer USB.

**`read_xram` sur un fd réseau** suit le patron socle §5.3 : tant que rien n'est prêt, la
fonction **retourne sans répondre** (BUSY reste posé) et le 6502 rappelle au tour suivant.
Deux temporisations distinctes, parce que le **premier octet d'une réponse HTTPS tarde de
plusieurs secondes** (handshake TLS) : 20 s pour le premier octet, 6 s entre octets.

**Arbitrage avec le passe-plat ACIA `$0380`** (terminaux type OricTel) : le canal AT est
**unique**. Tant qu'une transaction `N:` est ouverte, `net_owns_modem()` est vrai et
`acia_task()` ne consomme **ni** ne pousse d'octets vers le modem — sinon les deux modes se
voleraient le flux. Les deux restent mutuellement exclusifs sans qu'aucun connaisse l'autre
(spec §7 QO 2, tranchée ainsi). `net_stop()` libère le lien à l'arrêt du noyau.

**Descripteurs** : `STD_NET_OFFS` suit ceux du littlefs, un seul fd (canal AT unique,
spec §7 QO 1). `open` route sur le préfixe `N:` (insensible à la casse) avant le test `0:`.

**VALIDÉ EN RUNTIME**, pas seulement compilé : `emul/tests/test_net.c` (**12/12**) exécute ce
code dans l'émulateur RP2040 avec un modem mock — commande AT réellement émise
(`ATGET<url>\r` exact), corps dé-chunké livré au 6502 sans en-têtes, `-1` « rien de prêt »
distinct de l'EOF, EOF franc, corps **binaire** intact (`0x00`/`0x0D`/`0x1A`), verrou
`net_owns_modem()` posé puis relâché, réouverture après `close`, et refus du modem remonté en
erreur de transport sans faux succès. Suite complète de l'émulateur : **14 suites vertes**.

**Hors périmètre de ce lot** (spec §4.2) : `ATPOST`/écriture, `tcp://`/`telnet://`, `prefix`,
`json_query`, `time`, multi-connexions. `net_control` n'expose que `status` (code HTTP, état,
octets disponibles), poussé **sur le xstack** et non dans un registre à effet de bord — un
`LDA reg,X` indexé provoquerait un double accès (socle §5.9).

**⚠️ Constat annexe, non corrigé** : `mia/oric/dsk_web.c` (webdisk archi B) émet `ATDISKRD`,
commande que le dongle **ne reconnaît pas** — vérifié le 2026-09-10 sur v0.3.3 (interprétée
comme `ATD` + « ISKRDhttp:0 » → `NO CARRIER`) et **absente des sources du dongle**
(`~/picowifi`). Le webdisk archi B ne peut donc pas fonctionner en l'état : soit la commande
est ajoutée au dongle, soit `dsk_web.c` bascule sur `ATGET` + `net_http.c` (désormais
disponible). À trancher.

### 2026-09-09 — CAUSE RACINE du boot HW en build XIP + correctif candidat (masquage IRQ des ops flash)
Diagnostic via le co-sim `~/loci/emul` de **pourquoi la stratégie C (FLASH/XIP) bootait en émulateur mais
mourait avant `led_init` sur silicium** (cf. entrée 2026-09-08).
- **Cause racine** : `src/mia/sys/lfs.c` (`lfs_erase`/`lfs_prog`) appelle `flash_range_erase`/
  `flash_range_program` **sans masquer les interruptions** (ni `flash_safe_execute`). Sur RP2040 ces
  fonctions **coupent le XIP** le temps de l'op. Or `init()` lance le **format littlefs (`lfs_init`, ligne
  72) AVANT `led_init` (ligne 85)** ; en build XIP **63 handlers d'IRQ résident en flash** (`isr_irq0..31`,
  `isr_hardfault` @`0x100001cc`). Un IRQ pendant l'erase → fetch de l'ISR dans une flash déconnectée →
  hardfault (lui-même en flash → double faute) → **mort avant la LED**. En `copy_to_ram` tout le code (ISR
  compris) est en RAM → couper le XIP est inoffensif → boot OK. Explique exactement « XIP casse, copy_to_ram
  marche ».
- **Pourquoi invisible en émul** : le co-sim **HLE** `flash_range_erase/program` de façon atomique et traite
  `flash_exit_xip`/`enter_xip` en **no-op** → le XIP n'est jamais réellement coupé, aucun IRQ ne fetch du
  flash mort. Limite **structurelle** : le co-sim ne peut pas valider le chemin XIP/boot2 réel.
- **Correctif candidat** (`src/mia/sys/lfs.c`) : `save_and_disable_interrupts()`/`restore_interrupts()`
  autour des deux ops flash (`#include "hardware/sync.h"`). Inoffensif en `copy_to_ram`. **⚠ core1** : si
  l'act_loop (core1) prend des IRQ, un lockout (`flash_safe_execute`/`multicore_lockout`) serait aussi
  requis — le co-sim montre core1 en **boucle RAM pure** pendant tout le format (4000 pas/op, jamais en
  flash), donc le masquage core0 est le 1ᵉʳ correctif ; à confirmer sur MATÉRIEL.
- **Bénéfice double** : le build XIP **corrige aussi** le crash « entrer dans un périphérique » du menu
  (`oric_bank3=0x2000C000` chevauchait le code `.text`/littlefs en copy_to_ram) — en XIP le code est en flash
  et la zone `xram`/`oric_bank0-3` (0x20000000-0x10000) est réservée, sans code (RAM à 0x20010000).
- **Outillage** : `src/CMakeLists.txt` — choix mémoire conditionnel `-DLOCI_XIP=ON` (linker
  `memmap_xram_mia_flash.ld`), défaut inchangé (`copy_to_ram`). Build XIP dans `build-xip/` (link OK, RAM
  ~53 Ko / 192 Ko, pas de débordement).
- **✅ VALIDÉ SUR SILICIUM (2026-09-09)** : `build-xip/.../loci-firmware.uf2` flashé via
  `/media/bmarty/RPI-RP2` → **LED ROUGE = boote**. **1ᵉʳ build FLASH/XIP qui tourne sur la vraie cartouche.**
  Le masquage IRQ de core0 **suffit** (pas de lockout core1 nécessaire : l'act_loop reste en RAM). ⟹ **~130 Ko
  de RAM libérés** ET le crash menu « entrer dans un périphérique » réglé (code hors de `oric_bank3`). La
  « stratégie C » (XIP) est donc de nouveau utilisable ; à propager aux autres builds (`full-A7`, `coproc-A9`)
  s'ils tiennent, pour libérer la RAM tout en bootant sur matériel.

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
