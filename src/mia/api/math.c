/* Coprocesseur arithmétique LOCI — opcode MIA $A9 (MIA_OP_MATH).
 * Lot M2 : entiers (§3.1) + flottant IEEE754 (§3.2) + transcendantes (§3.3).
 * MBF (§3.4) et ops par bloc (§3.5) : lots ultérieurs (dépendent du §7.1 /
 * de l'adressage XRAM). Voir extensions/coprocessor-A9/spec-coprocesseur-math.md.
 *
 * Convention xstack (calquée sur std_api_read_xram) : 1er opérande dépilé par
 * api_pop_*, dernier opérande par api_pop_*_end. Sortie scalaire (dont 1 float)
 * via api_return_axsreg ; sortie multi-valeurs via api_push_* + api_sync_xstack
 * + api_return_released ; erreur via api_return_errno. */

#include "api/api.h"
#include "api/math.h"
#include <math.h>

/* Retour d'un f32 en AX:sreg (bit-cast). */
static inline void math_return_f32(float r) { api_return_axsreg(math_f2b(r)); }

/* Retour quotient+reste (2 valeurs de N octets) sur le xstack. */
static void math_return_divmod_u16(uint16_t quo, uint16_t rem) {
    api_push_uint16(&rem);          /* poussé en 1er -> dépilé en dernier côté 6502 */
    api_push_uint16(&quo);
    api_sync_xstack();
    api_return_released();
}
static void math_return_divmod_u32(uint32_t quo, uint32_t rem) {
    api_push_uint32(&rem);
    api_push_uint32(&quo);
    api_sync_xstack();
    api_return_released();
}

void math_api(void)
{
    switch (API_A)
    {
    /* ---------- 3.1 Entiers ---------- */
    case MATH_MUL_U16: {
        uint16_t a, b;
        if (!api_pop_uint16(&a) || !api_pop_uint16_end(&b)) return;
        api_return_axsreg(math_mul_u16(a, b));
        break;
    }
    case MATH_MUL_I16: {
        int16_t a, b;
        if (!api_pop_int16(&a) || !api_pop_int16_end(&b)) return;
        api_return_axsreg((uint32_t)math_mul_i16(a, b));
        break;
    }
    case MATH_MUL_U32: {
        uint32_t a, b;
        if (!api_pop_uint32(&a) || !api_pop_uint32_end(&b)) return;
        api_return_axsreg(math_mul_u32(a, b));
        break;
    }
    case MATH_DIVMOD_U16: {
        uint16_t a, b, r;
        if (!api_pop_uint16(&a) || !api_pop_uint16_end(&b)) return;
        if (b == 0) { api_return_errno(API_EINVAL); break; }
        uint16_t q = math_divmod_u16(a, b, &r);
        math_return_divmod_u16(q, r);
        break;
    }
    case MATH_DIVMOD_I16: {
        int16_t a, b, r;
        if (!api_pop_int16(&a) || !api_pop_int16_end(&b)) return;
        if (b == 0) { api_return_errno(API_EINVAL); break; }
        int16_t q = math_divmod_i16(a, b, &r);
        math_return_divmod_u16((uint16_t)q, (uint16_t)r);
        break;
    }
    case MATH_DIVMOD_U32: {
        uint32_t a, b, r;
        if (!api_pop_uint32(&a) || !api_pop_uint32_end(&b)) return;
        if (b == 0) { api_return_errno(API_EINVAL); break; }
        uint32_t q = math_divmod_u32(a, b, &r);
        math_return_divmod_u32(q, r);
        break;
    }
    case MATH_DIVMOD_I32: {
        int32_t a, b, r;
        if (!api_pop_int32(&a) || !api_pop_int32_end(&b)) return;
        if (b == 0) { api_return_errno(API_EINVAL); break; }
        int32_t q = math_divmod_i32(a, b, &r);
        math_return_divmod_u32((uint32_t)q, (uint32_t)r);
        break;
    }

    /* ---------- 3.2 Flottant IEEE754 32 bits ---------- */
    case MATH_FADD: {
        uint32_t a, b;
        if (!api_pop_uint32(&a) || !api_pop_uint32_end(&b)) return;
        math_return_f32(math_b2f(a) + math_b2f(b));
        break;
    }
    case MATH_FSUB: {
        uint32_t a, b;
        if (!api_pop_uint32(&a) || !api_pop_uint32_end(&b)) return;
        math_return_f32(math_b2f(a) - math_b2f(b));
        break;
    }
    case MATH_FMUL: {
        uint32_t a, b;
        if (!api_pop_uint32(&a) || !api_pop_uint32_end(&b)) return;
        math_return_f32(math_b2f(a) * math_b2f(b));
        break;
    }
    case MATH_FDIV: {
        uint32_t a, b;
        if (!api_pop_uint32(&a) || !api_pop_uint32_end(&b)) return;
        math_return_f32(math_b2f(a) / math_b2f(b));  /* IEEE : /0 -> Inf/NaN */
        break;
    }
    case MATH_FCMP: {
        uint32_t a, b;
        if (!api_pop_uint32(&a) || !api_pop_uint32_end(&b)) return;
        float fa = math_b2f(a), fb = math_b2f(b);
        int8_t r = (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
        api_return_axsreg((uint32_t)(int32_t)r);
        break;
    }
    case MATH_ITOF: {
        int32_t a;
        if (!api_pop_int32_end(&a)) return;
        math_return_f32((float)a);
        break;
    }
    case MATH_FTOI: {
        uint32_t a;
        if (!api_pop_uint32_end(&a)) return;
        api_return_axsreg((uint32_t)(int32_t)math_b2f(a));
        break;
    }

    /* ---------- 3.3 Transcendantes (libm / bootrom) ---------- */
    case MATH_FSQRT: {
        uint32_t a; if (!api_pop_uint32_end(&a)) return;
        math_return_f32(sqrtf(math_b2f(a))); break;
    }
    case MATH_FSIN: {
        uint32_t a; if (!api_pop_uint32_end(&a)) return;
        math_return_f32(sinf(math_b2f(a))); break;
    }
    case MATH_FCOS: {
        uint32_t a; if (!api_pop_uint32_end(&a)) return;
        math_return_f32(cosf(math_b2f(a))); break;
    }
    case MATH_FTAN: {
        uint32_t a; if (!api_pop_uint32_end(&a)) return;
        math_return_f32(tanf(math_b2f(a))); break;
    }
    case MATH_FATAN2: {
        uint32_t a, b;
        if (!api_pop_uint32(&a) || !api_pop_uint32_end(&b)) return;
        math_return_f32(atan2f(math_b2f(a), math_b2f(b))); break;
    }
    case MATH_FLOG: {
        uint32_t a; if (!api_pop_uint32_end(&a)) return;
        math_return_f32(logf(math_b2f(a))); break;
    }
    case MATH_FEXP: {
        uint32_t a; if (!api_pop_uint32_end(&a)) return;
        math_return_f32(expf(math_b2f(a))); break;
    }
    case MATH_FPOW: {
        uint32_t a, b;
        if (!api_pop_uint32(&a) || !api_pop_uint32_end(&b)) return;
        math_return_f32(powf(math_b2f(a), math_b2f(b))); break;
    }

    default:
        api_return_errno(API_EINVAL);
        break;
    }
}
