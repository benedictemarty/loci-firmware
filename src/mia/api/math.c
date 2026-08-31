/* Coprocesseur arithmétique LOCI — opcode MIA $A9 (MIA_OP_MATH).
 * Lot M2 : entiers (§3.1) + flottant IEEE754 (§3.2) + transcendantes (§3.3).
 * MBF (§3.4) et ops par bloc (§3.5) : lots ultérieurs (dépendent du §7.1 /
 * de l'adressage XRAM). Voir extensions/coprocessor-A9/spec-coprocesseur-math.md.
 *
 * Convention xstack (alignée sur Phosphoric loci_math.c, validée 22 vecteurs) :
 * pour op(a,b), le 6502 pousse a PUIS b (b au sommet) → on dépile b d'abord
 * (api_pop_*), a ensuite (api_pop_*_end). Sortie scalaire (dont 1 float) via
 * api_return_axsreg ; sortie multi-valeurs via api_push_* + api_sync_xstack +
 * api_return_released ; erreur via api_return_errno. */

#include "api/api.h"
#include "api/math.h"
#include "sys/mem.h"   /* xram[] pour les ops par bloc */
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
        if (!api_pop_uint16(&b) || !api_pop_uint16_end(&a)) return;
        api_return_axsreg(math_mul_u16(a, b));
        break;
    }
    case MATH_MUL_I16: {
        int16_t a, b;
        if (!api_pop_int16(&b) || !api_pop_int16_end(&a)) return;
        api_return_axsreg((uint32_t)math_mul_i16(a, b));
        break;
    }
    case MATH_MUL_U32: {
        uint32_t a, b;
        if (!api_pop_uint32(&b) || !api_pop_uint32_end(&a)) return;
        api_return_axsreg(math_mul_u32(a, b));
        break;
    }
    case MATH_DIVMOD_U16: {
        uint16_t a, b, r;
        if (!api_pop_uint16(&b) || !api_pop_uint16_end(&a)) return;
        if (b == 0) { api_return_errno(API_EINVAL); break; }
        uint16_t q = math_divmod_u16(a, b, &r);
        math_return_divmod_u16(q, r);
        break;
    }
    case MATH_DIVMOD_I16: {
        int16_t a, b, r;
        if (!api_pop_int16(&b) || !api_pop_int16_end(&a)) return;
        if (b == 0) { api_return_errno(API_EINVAL); break; }
        int16_t q = math_divmod_i16(a, b, &r);
        math_return_divmod_u16((uint16_t)q, (uint16_t)r);
        break;
    }
    case MATH_DIVMOD_U32: {
        uint32_t a, b, r;
        if (!api_pop_uint32(&b) || !api_pop_uint32_end(&a)) return;
        if (b == 0) { api_return_errno(API_EINVAL); break; }
        uint32_t q = math_divmod_u32(a, b, &r);
        math_return_divmod_u32(q, r);
        break;
    }
    case MATH_DIVMOD_I32: {
        int32_t a, b, r;
        if (!api_pop_int32(&b) || !api_pop_int32_end(&a)) return;
        if (b == 0) { api_return_errno(API_EINVAL); break; }
        int32_t q = math_divmod_i32(a, b, &r);
        math_return_divmod_u32((uint32_t)q, (uint32_t)r);
        break;
    }

    /* ---------- 3.2 Flottant IEEE754 32 bits ---------- */
    case MATH_FADD: {
        uint32_t a, b;
        if (!api_pop_uint32(&b) || !api_pop_uint32_end(&a)) return;
        math_return_f32(math_b2f(a) + math_b2f(b));
        break;
    }
    case MATH_FSUB: {
        uint32_t a, b;
        if (!api_pop_uint32(&b) || !api_pop_uint32_end(&a)) return;
        math_return_f32(math_b2f(a) - math_b2f(b));
        break;
    }
    case MATH_FMUL: {
        uint32_t a, b;
        if (!api_pop_uint32(&b) || !api_pop_uint32_end(&a)) return;
        math_return_f32(math_b2f(a) * math_b2f(b));
        break;
    }
    case MATH_FDIV: {
        uint32_t a, b;
        if (!api_pop_uint32(&b) || !api_pop_uint32_end(&a)) return;
        math_return_f32(math_b2f(a) / math_b2f(b));  /* IEEE : /0 -> Inf/NaN */
        break;
    }
    case MATH_FCMP: {
        uint32_t a, b;
        if (!api_pop_uint32(&b) || !api_pop_uint32_end(&a)) return;
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
        if (!api_pop_uint32(&b) || !api_pop_uint32_end(&a)) return;
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
        if (!api_pop_uint32(&b) || !api_pop_uint32_end(&a)) return;
        math_return_f32(powf(math_b2f(a), math_b2f(b))); break;
    }

    /* ---------- 3.4 Pont MBF <-> IEEE754 ---------- */
    case MATH_MBF_TO_IEEE: {
        uint8_t m[5];   /* m[0]=exposant au sommet ; m[4]=fond (dépilé en dernier) */
        if (!api_pop_uint8(&m[0]) || !api_pop_uint8(&m[1]) || !api_pop_uint8(&m[2])
            || !api_pop_uint8(&m[3]) || !api_pop_uint8_end(&m[4])) return;
        math_return_f32(math_mbf5_to_f32(m));
        break;
    }
    case MATH_IEEE_TO_MBF: {
        uint32_t a; uint8_t m[5];
        if (!api_pop_uint32_end(&a)) return;
        if (!math_f32_to_mbf5(math_b2f(a), m)) { api_return_errno(API_ERANGE); break; }
        api_push_n(m, 5);          /* m[0] (exposant) au sommet, comme Phosphoric */
        api_sync_xstack();
        api_return_released();
        break;
    }

    /* ---------- 3.5 Ops par bloc sur vecteurs XRAM (synchrones, bornées) ---------- */
    case MATH_VEC_DOT: {   /* dot(ptrA, ptrB, count) -> f32 */
        uint16_t ptrA, ptrB, count;
        if (!api_pop_uint16(&count) || !api_pop_uint16(&ptrB) || !api_pop_uint16_end(&ptrA)) return;
        if (count > MATH_BLOCK_MAX) { api_return_errno(API_ERANGE); break; }
        math_return_f32(math_vec_dot((const uint8_t *)xram, ptrA, ptrB, count));
        break;
    }
    case MATH_VEC_SCALE: {  /* x[i] *= a (in-place) : ptr, count, a(f32) */
        uint16_t ptr, count; uint32_t a;
        if (!api_pop_uint32(&a) || !api_pop_uint16(&count) || !api_pop_uint16_end(&ptr)) return;
        if (count > MATH_BLOCK_MAX) { api_return_errno(API_ERANGE); break; }
        math_vec_scale((uint8_t *)xram, ptr, count, math_b2f(a));
        api_return_axsreg(0);   /* succès (in-place, pas de valeur) */
        break;
    }
    case MATH_POLY_EVAL: {  /* Horner : ptr coeffs, degree, x(f32) -> f32 */
        uint16_t ptr, degree; uint32_t x;
        if (!api_pop_uint32(&x) || !api_pop_uint16(&degree) || !api_pop_uint16_end(&ptr)) return;
        if (degree > MATH_BLOCK_MAX) { api_return_errno(API_ERANGE); break; }
        math_return_f32(math_poly_eval((const uint8_t *)xram, ptr, degree, math_b2f(x)));
        break;
    }

    default:
        api_return_errno(API_EINVAL);
        break;
    }
}
