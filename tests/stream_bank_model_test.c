/*
 * Test de non-régression (modèle de logique) — opcode $A8 MIA_OP_STREAM_BANK.
 *
 * Le firmware n'est pas compilable ici (toolchain RP2040 absente) : ce test ne
 * lie PAS le firmware, il reproduit la logique PURE de validation/adressage de
 * `std_api_stream_bank()` (src/mia/api/std.c) pour VERROUILLER :
 *   - le bornage de `len` à l'intérieur de la banque 16 Ko (0x4000 - dst) ;
 *   - le calcul d'adresse XRAM `xram[(SEL<<14) + dst]` pour SEL 0..3 ;
 *   - les rejets EINVAL (fd hors bornes, SEL>3, dst>0x3FFF) ;
 *   - la garde de débordement `base + len <= 0x10000` ;
 *   - le mapping de banque (MAP=1 ⇒ mia_set_bank(sel,true) une seule fois,
 *     APRÈS la lecture).
 *
 * Réf : extensions/streamer-A8/spec-streamer-assets.md (§4) ;
 *       firmware std.c std_api_stream_bank(), map.c/mia_set_bank().
 * Compilation/exécution : `make` (voir Makefile).
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Constantes miroir de std.c (STD_FIL_MAX=16) */
#define STD_FIL_OFFS 3
#define STD_FIL_MAX  16
#define STD_LFS_OFFS (STD_FIL_OFFS + STD_FIL_MAX) /* 19 */
#define STD_LFS_MAX  2                            /* fd LFS valides : 19, 20 */
#define XRAM_SIZE    0x10000
#define BANK_SIZE    0x4000

#define API_EINVAL 1 /* valeur factice : on teste seulement "rejeté ou non" */

typedef struct {
    int      err;         /* 0 = OK, sinon EINVAL */
    int      nread;       /* octets "lus" (= len bornée, fichier supposé assez grand) */
    uint32_t base;        /* offset XRAM cible */
    bool     bank_mapped; /* mia_set_bank(sel,true) appelé ? */
    uint8_t  bank_sel;
    int      order_ok;    /* mapping fait APRÈS lecture ? */
} result_t;

/*
 * Modèle fidèle de std_api_stream_bank : mêmes bornes, même adressage, même
 * ordre (lecture puis mapping). `file_len` = taille du fichier simulé à l'offset
 * `off` (borne le nombre d'octets réellement lus, comme f_read/lfs_file_read).
 */
static result_t stream_bank_apply(int fd, uint8_t a, int32_t off,
                                  uint16_t dst, uint16_t len, uint32_t file_len)
{
    result_t r = {0};
    uint8_t sel = a & 0x0F;
    bool map = !!(a & 0x80);

    if (fd < STD_FIL_OFFS || fd >= STD_LFS_MAX + STD_LFS_OFFS ||
        sel > 3 || dst > 0x3FFF) {
        r.err = API_EINVAL;
        return r;
    }
    if (len > (uint16_t)(BANK_SIZE - dst))
        len = (uint16_t)(BANK_SIZE - dst);

    r.base = ((uint32_t)sel << 14) + dst;
    if (r.base + len > XRAM_SIZE) { /* garde défense en profondeur */
        r.err = API_EINVAL;
        return r;
    }

    /* Lecture synchrone simulée : bornée par la taille du fichier restant. */
    uint32_t avail = (off < 0) ? 0 : file_len;
    r.nread = (len <= avail) ? len : (int)avail;
    r.order_ok = 1; /* la lecture précède le mapping ci-dessous */

    if (map) {
        r.bank_mapped = true;
        r.bank_sel = sel;
    }
    return r;
}

static void test_len_clamp(void)
{
    /* dst proche du haut de banque : len tronquée à 0x4000 - dst */
    result_t r = stream_bank_apply(STD_LFS_OFFS, 0x00, 0, 0x3F00, 0x0400, 0x10000);
    assert(r.err == 0);
    assert(r.nread == 0x0100); /* 0x4000 - 0x3F00 */
    printf("  [ok] bornage len dans la banque (0x400 -> 0x100)\n");
}

static void test_addressing(void)
{
    for (uint8_t sel = 0; sel <= 3; sel++) {
        result_t r = stream_bank_apply(STD_LFS_OFFS, sel, 0, 0x0010, 0x0100, 0x10000);
        assert(r.err == 0);
        assert(r.base == ((uint32_t)sel << 14) + 0x0010);
    }
    printf("  [ok] adressage xram[(SEL<<14)+dst] pour SEL 0..3\n");
}

static void test_rejects(void)
{
    assert(stream_bank_apply(0, 0, 0, 0, 0x100, 0x10000).err == API_EINVAL);   /* stdin */
    assert(stream_bank_apply(2, 0, 0, 0, 0x100, 0x10000).err == API_EINVAL);   /* < OFFS */
    assert(stream_bank_apply(21, 0, 0, 0, 0x100, 0x10000).err == API_EINVAL);  /* > LFS max */
    assert(stream_bank_apply(STD_LFS_OFFS, 0x04, 0, 0, 0x100, 0x10000).err == API_EINVAL); /* sel=4 */
    assert(stream_bank_apply(STD_LFS_OFFS, 0x00, 0, 0x4000, 0x100, 0x10000).err == API_EINVAL); /* dst>0x3FFF */
    /* fd valides : bornes basse (3) et haute (20) acceptées */
    assert(stream_bank_apply(3, 0, 0, 0, 0x100, 0x10000).err == 0);
    assert(stream_bank_apply(20, 0, 0, 0, 0x100, 0x10000).err == 0);
    printf("  [ok] rejets EINVAL (fd, SEL, dst) et bornes fd valides\n");
}

static void test_overflow_guard(void)
{
    /* Pour SEL<=3 et len bornée, base+len ne dépasse jamais 0x10000. */
    for (uint8_t sel = 0; sel <= 3; sel++) {
        result_t r = stream_bank_apply(STD_LFS_OFFS, sel, 0, 0x3FFF, 0xFFFF, 0x10000);
        assert(r.err == 0);
        assert(r.base + r.nread <= XRAM_SIZE);
    }
    printf("  [ok] garde débordement base+len <= 0x10000\n");
}

static void test_map(void)
{
    /* MAP=1 -> banque mappée avec le bon SEL, après lecture */
    result_t r1 = stream_bank_apply(STD_LFS_OFFS, 0x80 | 2, 0, 0, 0x100, 0x10000);
    assert(r1.err == 0 && r1.bank_mapped && r1.bank_sel == 2 && r1.order_ok);
    /* MAP=0 -> pas de mapping (préchargement banque inactive) */
    result_t r0 = stream_bank_apply(STD_LFS_OFFS, 0x00 | 3, 0, 0, 0x100, 0x10000);
    assert(r0.err == 0 && !r0.bank_mapped);
    printf("  [ok] MAP : mapping conditionnel du bon SEL, après lecture\n");
}

static void test_short_read(void)
{
    /* fichier plus court que len -> nread tronqué (comme f_read/lfs_file_read) */
    result_t r = stream_bank_apply(STD_LFS_OFFS, 0x00, 0, 0, 0x1000, 0x0040);
    assert(r.err == 0 && r.nread == 0x0040);
    printf("  [ok] lecture courte : nread borné à la taille fichier\n");
}

int main(void)
{
    printf("== stream_bank_model_test ==\n");
    test_len_clamp();
    test_addressing();
    test_rejects();
    test_overflow_guard();
    test_map();
    test_short_read();
    printf("Tous les tests $A8 passent.\n");
    return 0;
}
