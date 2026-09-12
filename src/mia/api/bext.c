/*
 * bext — extension BASIC par le hook `!` (opcode $AB, extensions/basic-ext-AB).
 *
 * Prototype : quand la cassette passe par LOCI (MIA_BOOTSET_TAP) et que la ROM
 * servie est BASIC 1.1b, le vecteur RAM du `!` ($02F5, handler $CD13 = JMP ($02F5),
 * crochet officiel des DOS) est posé au démarrage à froid sur un trampoline 6502
 * logé dans la zone morte $E66E-$E6C8 (corps de « écrire un octet cassette »,
 * court-circuité par mia_write_byte_patch). Seule retouche de code ROM : les 6
 * octets STA/STY de $ECF3 qui initialisaient ce vecteur. `!VER` → primitive 0 :
 * le firmware pousse une chaîne terminée par 0, le trampoline l'affiche.
 * Mot inconnu → séquence `!` d'origine rejouée (compat DOS / `!expr`).
 * Source du trampoline : extensions/basic-ext-AB/src/tramp11.s (ca65).
 */
#include <string.h>
#include "api/api.h"
#include "api/bext.h"
#include "sys/mem.h"
#include "locifw_version.h"
#include "pico/platform.h"    /* __in_flash */

#define BEXT_TRAMP_11_ADDR   0xE66E
#define BEXT_INSTALL_11_ADDR 0xE6B2   /* `install` dans le trampoline (tramp11.lbl) */
#define BEXT_COLDSTART_11_ADDR 0xECF3 /* STA $02F5 ; STY $02F6 du démarrage à froid */

static const uint8_t __in_flash() bext_tramp_11[] = {
    0xA0, 0x00, 0xB1, 0xE9, 0xC9, 0x20, 0xD0, 0x03, 0xC8, 0xD0, 0xF7, 0xA2,
    0x00, 0xB1, 0xE9, 0xDD, 0xAF, 0xE6, 0xD0, 0x2A, 0xC8, 0xE8, 0xE0, 0x03,
    0xD0, 0xF3, 0x98, 0x18, 0x65, 0xE9, 0x85, 0xE9, 0x90, 0x02, 0xE6, 0xEA,
    0xA9, 0x00, 0x8D, 0xB4, 0x03, 0xA9, 0xAB, 0x8D, 0xAF, 0x03, 0x20, 0xB0,
    0x03, 0xAD, 0xAC, 0x03, 0xF0, 0x07, 0xAA, 0x20, 0x7C, 0xF7, 0x4C, 0x9F,
    0xE6, 0x60, 0x4C, 0x36, 0xD3, 0x56, 0x45, 0x52, 0xA9, 0x6E, 0x8D, 0xF5,
    0x02, 0xA9, 0xE6, 0x8D, 0xF6, 0x02, 0x60,
};

void bext_install_11(void)
{
    for (uint16_t i = 0; i < sizeof(bext_tramp_11); i++)
        xram[BEXT_TRAMP_11_ADDR + i] = bext_tramp_11[i];
    /* Démarrage à froid : STA $02F5 ; STY $02F6 (6 o) -> JSR install ; NOP x3.
     * install pose le vecteur `!` ($02F5) sur le trampoline ; A/Y (= $D336) restent
     * posés dans $22/$23 et $02FC/$02FD par les instructions précédentes. */
    xram[BEXT_COLDSTART_11_ADDR + 0] = 0x20;                       /* JSR */
    xram[BEXT_COLDSTART_11_ADDR + 1] = BEXT_INSTALL_11_ADDR & 0xFF;
    xram[BEXT_COLDSTART_11_ADDR + 2] = BEXT_INSTALL_11_ADDR >> 8;
    xram[BEXT_COLDSTART_11_ADDR + 3] = 0xEA;
    xram[BEXT_COLDSTART_11_ADDR + 4] = 0xEA;
    xram[BEXT_COLDSTART_11_ADDR + 5] = 0xEA;
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

/* $AB — A = primitive. 0 : VER (chaîne de version). Sinon EINVAL. */
void bext_api_prim(void)
{
    switch (API_A) {
    case 0:
        bext_push_str("LOCI FW " LOCIFW_VERSION "\r\n");
        api_sync_xstack();
        return api_return_ax(0);
    default:
        return api_return_errno(API_EINVAL);
    }
}
