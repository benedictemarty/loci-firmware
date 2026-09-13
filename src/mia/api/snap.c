/*
 * snap — save-state / snapshot ($B0, extensions/save-state-B0).
 *
 * Le gel et la reprise existent déjà : bouton court → trap IRQ (sys/ext.c) → menu LOCI
 * dont restore.s (rom) range ZP, $0100-$1FFF, VRAM, VIA, registres et mode vidéo en
 * XRAM [0, SNAP_RSTR_SIZE), puis MIA_OP_BOOT|RESUME les rejoue. Il manque au fichier :
 * la RAM $2000-$9FFF (que seul le 6502 peut lire : la ROM la recopie par tranches de
 * 16 Ko dans XRAM [SNAP_BUF, +SNAP_CHUNK) via $03A4) et les map_flags figés au gel
 * (ROM BASIC ou device ROM servie), tenus ici par mia_save_map_flags().
 * L'E/S fichier est faite ici (littlefs 0:/LOCI.SNP) : la ROM 16 Ko est pleine et
 * read()/write() côté 6502 lui coûteraient ~600 o de bibliothèque.
 *
 *   A = 0        : AX = saved_map_flags
 *   A = 1, X = f : saved_map_flags = f
 *   A = 2, X = 0 : SNAP_BEGIN sauvegarde — crée le fichier, écrit en-tête + XRAM [0, RSTR)
 *   A = 2, X = 1 : SNAP_BEGIN chargement — ouvre, vérifie l'en-tête, relit XRAM [0, RSTR),
 *                  pose saved_map_flags depuis l'en-tête
 *   A = 3        : SNAP_CHUNK — écrit (ou relit) XRAM [SNAP_BUF, +SNAP_CHUNK)
 *   A = 4        : SNAP_END — ferme le fichier
 * Retour AX = 0, sinon errno (EINVAL si séquence incohérente, ENOEXEC si en-tête invalide).
 *
 * Fichier : en-tête 16 o { "LOCISNP\0", version 1, map_flags, 6 réservés } | XRAM [0, RSTR)
 * | RAM $2000-$5FFF | RAM $6000-$9FFF.
 */
#include <string.h>
#include "api/api.h"
#include "api/snap.h"
#include "sys/lfs.h"
#include "sys/mem.h"
#include "sys/mia.h"

#define SNAP_PATH       "LOCI.SNP"
#define SNAP_VERSION    1
#define SNAP_RSTR_SIZE  0x3F0F      /* = rom restore.s : $1F00 + $2000 + 1 + 13 + 1 */
#define SNAP_BUF        0x8000      /* tampon XRAM libre pendant le menu (cf. filemanager) */
#define SNAP_CHUNK      0x4000

typedef struct {
    char    magic[8];
    uint8_t version;
    uint8_t map_flags;
    uint8_t reserved[6];
} snap_hdr_t;
static_assert(sizeof(snap_hdr_t) == 16);

static const char snap_magic[8] = "LOCISNP";
static lfs_file_t snap_file;
static bool snap_open;
static bool snap_loading;

static int snap_close(void)
{
    if (!snap_open)
        return 0;
    snap_open = false;
    lfs_free_file_config(&snap_file);
    return lfs_file_close(&lfs_volume, &snap_file);
}

/* Transfère n octets de xram[off] vers/depuis le fichier ; 0 si complet, sinon errno. */
static int snap_xfer(uint16_t off, uint32_t n)
{
    lfs_ssize_t r = snap_loading
        ? lfs_file_read(&lfs_volume, &snap_file, (void *)&xram[off], n)
        : lfs_file_write(&lfs_volume, &snap_file, (void *)&xram[off], n);
    if (r < 0)
        return API_ELFSFS(r);
    return (uint32_t)r == n ? 0 : API_EIO;
}

static int snap_begin(bool loading)
{
    snap_hdr_t hdr;
    lfs_ssize_t r;
    int e;
    snap_close();
    snap_loading = loading;
    r = lfs_file_opencfg(&lfs_volume, &snap_file, SNAP_PATH,
                         loading ? LFS_O_RDONLY : (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC),
                         lfs_alloc_file_config());
    if (r < 0)
        return API_ELFSFS(r);
    snap_open = true;
    if (loading) {
        r = lfs_file_read(&lfs_volume, &snap_file, &hdr, sizeof hdr);
        if (r != sizeof hdr || memcmp(hdr.magic, snap_magic, 8) || hdr.version != SNAP_VERSION) {
            snap_close();
            return API_ENOEXEC;
        }
        mia_set_saved_map_flags(hdr.map_flags);
    } else {
        memset(&hdr, 0, sizeof hdr);
        memcpy(hdr.magic, snap_magic, 8);
        hdr.version = SNAP_VERSION;
        hdr.map_flags = mia_get_saved_map_flags();
        r = lfs_file_write(&lfs_volume, &snap_file, &hdr, sizeof hdr);
        if (r != sizeof hdr) {
            snap_close();
            return r < 0 ? API_ELFSFS(r) : API_EIO;
        }
    }
    e = snap_xfer(0, SNAP_RSTR_SIZE);
    if (e)
        snap_close();
    return e;
}

void snap_api(void)
{
    int e;
    switch (API_A) {
    case 0:
        return api_return_ax(mia_get_saved_map_flags());
    case 1:
        mia_set_saved_map_flags(API_X);
        return api_return_ax(0);
    case 2:
        e = snap_begin(API_X != 0);
        break;
    case 3:
        if (!snap_open)
            return api_return_errno(API_EINVAL);
        e = snap_xfer(SNAP_BUF, SNAP_CHUNK);
        if (e)
            snap_close();
        break;
    case 4:
        e = snap_close();
        if (e < 0)
            e = API_ELFSFS(e);
        break;
    default:
        return api_return_errno(API_EINVAL);
    }
    if (e)
        return api_return_errno(e);
    return api_return_ax(0);
}
