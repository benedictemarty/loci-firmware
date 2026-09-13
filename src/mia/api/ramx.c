/* ramx — voir ramx.h. */
#include <string.h>
#include "api/api.h"
#include "api/ramx.h"

#if LOCI_RAMX_KB > 0
uint8_t ramx_mem[LOCI_RAMX_KB * 1024u] __attribute__((aligned(4)));
volatile uint16_t ramx_cur;
#endif

void ramx_init(void)
{
    IOREGS(RAMX_NPAGES)     = RAMX_NPAGES_VAL & 0xFF;
    IOREGS(RAMX_NPAGES + 1) = RAMX_NPAGES_VAL >> 8;
    IOREGS(RAMX_WSIZE_R)    = RAMX_WSIZE;
    IOREGS(RAMX_PAGE_LO) = 0;
    IOREGS(RAMX_PAGE_HI) = 0;
#if LOCI_RAMX_KB > 0
    ramx_cur = 0;
    memset(ramx_mem, 0, sizeof ramx_mem);
    memset((void *)&iopage[RAMX_WINDOW & 0xFF], 0, RAMX_WSIZE);
#endif
}

/* $AF — A = 0 : info → AX = nombre de pages (0 = absente), pousse WSIZE (1 o) ;
 *       A = 1 : set_page(n) — n (uint16) sur la xstack → AX = page effective. */
void ramx_api(void)
{
    switch (API_A) {
    case 0: {
        uint8_t w = RAMX_WSIZE;
        api_push_uint8(&w);
        api_sync_xstack();
        return api_return_ax(RAMX_NPAGES_VAL);
    }
    case 1: {
        uint16_t page;
        if (!api_pop_uint16(&page))
            return api_return_errno(API_EINVAL);
#if LOCI_RAMX_KB > 0
        ramx_select(page);
        return api_return_ax(ramx_cur);
#else
        return api_return_errno(API_ENODEV);
#endif
    }
    default:
        return api_return_errno(API_EINVAL);
    }
}
