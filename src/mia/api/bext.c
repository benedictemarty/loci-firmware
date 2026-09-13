/*
 * bext — extension BASIC par le hook `!` (opcode $AB, extensions/basic-ext-AB).
 *
 * Le handler `!` des deux ROMs est JMP ($02F5) : un VECTEUR RAM que le démarrage
 * à froid initialise à l'erreur ILLEGAL QUANTITY (1.1b : $CD13 / init $ECF3 ;
 * 1.0 : $CC89 / init $EA80) — c'est le crochet officiel des DOS. Quand la
 * cassette passe par LOCI (MIA_BOOTSET_TAP, qui rend mortes les routines
 * cassette de la copie de ROM servie), LOCI y loge un trampoline 6502 + une
 * table de mots-clés, et retouche les 6 octets STA/STY de l'init en
 * JSR install ; NOP ×3 pour poser le vecteur au démarrage à froid.
 *
 *   !VER   -> primitive 0 : chaîne de version
 *   !MEM   -> primitive 1 : espace libre du FS interne 0:
 *   !XSET a,v -> primitive 2 (signature 2 : arguments évalués par les routines de
 *              POKE de la ROM, poussés octet puis adresse) : xram[a] = v, confirme
 *   inconnu -> comportement `!` d'origine (JMP défaut)
 *
 * Le firmware pousse une chaîne terminée par 0 ; le trampoline la dépile et
 * l'affiche. Source/assemblage : extensions/basic-ext-AB/src (make → bext_tramp.h).
 */
#include <stdio.h>
#include <string.h>
#include "pico/platform.h"    /* __in_flash */
#include "api/api.h"
#include "api/bext.h"
#include "sys/mem.h"
#include "sys/lfs.h"
#include "locifw_version.h"
#include "bext_tramp.h"

static void bext_install(uint16_t code_addr, const uint8_t *code, size_t ncode,
                         uint16_t table_addr, const uint8_t *table, size_t ntable,
                         uint16_t install_addr, uint16_t cold_addr)
{
    for (size_t i = 0; i < ncode; i++)  xram[code_addr + i]  = code[i];
    for (size_t i = 0; i < ntable; i++) xram[table_addr + i] = table[i];
    /* STA $02F5 ; STY $02F6 -> JSR install ; NOP ; NOP ; NOP (A/Y = défaut, déjà
     * posés dans $22/$23 et $02FC/$02FD par les instructions précédentes). */
    xram[cold_addr + 0] = 0x20;
    xram[cold_addr + 1] = install_addr & 0xFF;
    xram[cold_addr + 2] = install_addr >> 8;
    xram[cold_addr + 3] = 0xEA;
    xram[cold_addr + 4] = 0xEA;
    xram[cold_addr + 5] = 0xEA;
}

void bext_install_11(void)
{
    bext_install(BEXT11_CODE_ADDR, bext11_code, sizeof bext11_code,
                 BEXT11_TABLE_ADDR, bext11_table, sizeof bext11_table,
                 BEXT11_INSTALL_ADDR, BEXT11_COLD_ADDR);
}

void bext_install_10(void)
{
    bext_install(BEXT10_CODE_ADDR, bext10_code, sizeof bext10_code,
                 BEXT10_TABLE_ADDR, bext10_table, sizeof bext10_table,
                 BEXT10_INSTALL_ADDR, BEXT10_COLD_ADDR);
}

/* Pousse une chaîne pour un dépilement dans l'ordre (terminateur 0 poussé en premier). */
static void bext_push_str(const char *s)
{
    uint8_t zero = 0;
    size_t n = strlen(s);
    api_push_uint8(&zero);
    while (n)
        api_push_uint8((const uint8_t *)&s[--n]);
}

/* $AB — A = primitive. Rend 0 + chaîne poussée ; EINVAL sinon. */
void bext_api_prim(void)
{
    char buf[48];
    switch (API_A) {
    case 0:                                       /* VER */
        bext_push_str("LOCI FW " LOCIFW_VERSION "\r\n");
        break;
    case 1: {                                     /* MEM : espace libre du FS interne 0: */
        lfs_ssize_t used = lfs_fs_size(&lfs_volume);
        if (used < 0)
            return api_return_errno(API_ELFSFS(used));
        uint32_t total = lfs_volume.cfg->block_count;
        uint32_t kb = (total - (uint32_t)used) * (lfs_volume.cfg->block_size / 1024);
        snprintf(buf, sizeof buf, "0: %lu KB FREE\r\n", (unsigned long)kb);
        bext_push_str(buf);
        break; }
    case 2: {                                     /* XSET adresse,valeur : octet de la XRAM LOCI */
        uint16_t addr; uint8_t val;
        if (!api_pop_uint16(&addr) || !api_pop_uint8(&val))
            return api_return_errno(API_EINVAL);
        xram[addr] = val;
        snprintf(buf, sizeof buf, "XRAM $%04X = $%02X\r\n", addr, val);
        bext_push_str(buf);
        break; }
    default:
        return api_return_errno(API_EINVAL);
    }
    api_sync_xstack();
    return api_return_ax(0);
}
