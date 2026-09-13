/*
 * ramx — expansion RAM paginée façon GeoRAM (opcode $AF, extensions/ram-expansion-AF).
 *
 * Fenêtre de 32 octets dans la page I/O : $03C0-$03DF (lue par le read-serve,
 * écrite par le cas par défaut de l'act_loop = io-page). Registres :
 *   $03E0  PAGE_LO  (écriture : sélectionne la page, bascule immédiate)
 *   $03E1  PAGE_HI
 *   $03E2/$03E3  NPAGES (lecture seule, 16 bits) — 0 = expansion absente
 *   $03E4  WSIZE   (lecture seule) = 32
 * Page n = octets [n*32, n*32+32) de la RAM d'expansion. Une écriture de PAGE_LO
 * ou PAGE_HI recopie la fenêtre courante vers sa page puis charge la nouvelle
 * (8 mots de 32 bits chacun, dans l'act_loop, avant le cycle 6502 suivant).
 * Taille : LOCI_RAMX_KB (CMake ; 128 en build XIP, 0 = désactivé sinon).
 */
#ifndef _RAMX_H_
#define _RAMX_H_
#include <stdint.h>
#include <stdbool.h>
#include "sys/mem.h"

#define RAMX_WINDOW   0x03C0
#define RAMX_WSIZE    32
#define RAMX_PAGE_LO  0x03E0
#define RAMX_PAGE_HI  0x03E1
#define RAMX_NPAGES   0x03E2
#define RAMX_WSIZE_R  0x03E4

#ifndef LOCI_RAMX_KB
#define LOCI_RAMX_KB 0
#endif
#define RAMX_NPAGES_VAL ((LOCI_RAMX_KB * 1024u) / RAMX_WSIZE)

#if LOCI_RAMX_KB > 0
extern uint8_t ramx_mem[LOCI_RAMX_KB * 1024u] __attribute__((aligned(4)));
extern volatile uint16_t ramx_cur;     /* page actuellement dans la fenêtre */
#endif

void ramx_init(void);
void ramx_api(void);                   /* $AF : A=0 info, A=1 set_page(xstack uint16) */

/* Bascule de page — appelée depuis l'act_loop (core 1, .time_critical) : tout en
 * RAM, 8+8 mots. Sans expansion : no-op. */
static inline __attribute__((always_inline)) void ramx_select(uint16_t page)
{
#if LOCI_RAMX_KB > 0
    page %= RAMX_NPAGES_VAL;
    uint32_t *win = (uint32_t *)&iopage[RAMX_WINDOW & 0xFF];
    uint32_t *old = (uint32_t *)&ramx_mem[(uint32_t)ramx_cur * RAMX_WSIZE];
    uint32_t *nw  = (uint32_t *)&ramx_mem[(uint32_t)page * RAMX_WSIZE];
    for (int i = 0; i < RAMX_WSIZE / 4; i++) old[i] = win[i];
    for (int i = 0; i < RAMX_WSIZE / 4; i++) win[i] = nw[i];
    ramx_cur = page;
    IOREGS(RAMX_PAGE_LO) = page & 0xFF;
    IOREGS(RAMX_PAGE_HI) = page >> 8;
#else
    (void)page;
#endif
}
#endif
