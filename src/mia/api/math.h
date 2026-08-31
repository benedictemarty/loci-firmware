#ifndef _MATH_API_H_
#define _MATH_API_H_

/* Coprocesseur arithmétique LOCI — opcode MIA $A9 (MIA_OP_MATH).
 * Le sous-code d'opération est dans API_A ($03B4), les opérandes sur le xstack.
 * Voir extensions/coprocessor-A9/spec-coprocesseur-math.md.
 *
 * La logique de calcul est isolée en fonctions PURES (ci-dessous) pour être
 * testable en natif hors dépendances RP2040 ; math_api() ne fait que l'ABI
 * (dépiler / calculer / repousser). */

#include <stdint.h>
#include <string.h>
#include <math.h>

/* ---- Sous-codes (API_A) — figés depuis la spec §3 ---- */
/* 3.1 Entiers */
#define MATH_MUL_U16     0x00
#define MATH_MUL_I16     0x01
#define MATH_DIVMOD_U16  0x02
#define MATH_DIVMOD_I16  0x03
#define MATH_MUL_U32     0x04
#define MATH_DIVMOD_U32  0x05
#define MATH_DIVMOD_I32  0x06
/* 3.2 Flottant IEEE754 32 bits */
#define MATH_FADD        0x10
#define MATH_FSUB        0x11
#define MATH_FMUL        0x12
#define MATH_FDIV        0x13
#define MATH_FCMP        0x14
#define MATH_ITOF        0x15
#define MATH_FTOI        0x16
/* 3.3 Transcendantes */
#define MATH_FSQRT       0x20
#define MATH_FSIN        0x21
#define MATH_FCOS        0x22
#define MATH_FTAN        0x23
#define MATH_FATAN2      0x24
#define MATH_FLOG        0x25
#define MATH_FEXP        0x26
#define MATH_FPOW        0x27
/* 3.4 Pont MBF (BASIC Oric, style MS/CBM) <-> IEEE754 */
#define MATH_MBF_TO_IEEE 0x30
#define MATH_IEEE_TO_MBF 0x31
/* 3.5 Opérations par bloc sur vecteurs XRAM (f32, little-endian) */
#define MATH_VEC_DOT     0x40  /* dot(A,B,count)               -> f32           */
#define MATH_VEC_SCALE   0x41  /* x[i] *= a (in-place)         (précise §3.5)    */
#define MATH_POLY_EVAL   0x42  /* Horner sum c[i]*x^i, i=0..deg -> f32          */
#define MATH_BLOCK_MAX   256   /* borne par appel (synchrone ; cf. §4bis)        */

/* ---- bit-cast f32 <-> u32 (aucun UB, memcpy optimisé en no-op) ---- */
static inline uint32_t math_f2b(float f)  { uint32_t b; memcpy(&b, &f, 4); return b; }
static inline float    math_b2f(uint32_t b){ float f;    memcpy(&f, &b, 4); return f; }

/* ---- Fonctions de calcul PURES (déterministes, testables en natif) ---- */
static inline uint32_t math_mul_u16(uint16_t a, uint16_t b) { return (uint32_t)a * (uint32_t)b; }
static inline int32_t  math_mul_i16(int16_t a, int16_t b)   { return (int32_t)a * (int32_t)b; }
static inline uint32_t math_mul_u32(uint32_t a, uint32_t b) { return a * b; } /* tronqué 32 bits */

/* divmod : renvoie quotient, écrit le reste via *rem. divisor==0 => renvoie 0
 * et pose *rem=0 (l'appelant ABI convertit en errno avant d'arriver ici). */
static inline uint16_t math_divmod_u16(uint16_t a, uint16_t b, uint16_t *rem) {
    if (b == 0) { *rem = 0; return 0; }
    *rem = a % b; return a / b;
}
static inline int16_t  math_divmod_i16(int16_t a, int16_t b, int16_t *rem) {
    if (b == 0) { *rem = 0; return 0; }
    *rem = a % b; return a / b;
}
static inline uint32_t math_divmod_u32(uint32_t a, uint32_t b, uint32_t *rem) {
    if (b == 0) { *rem = 0; return 0; }
    *rem = a % b; return a / b;
}
static inline int32_t  math_divmod_i32(int32_t a, int32_t b, int32_t *rem) {
    if (b == 0) { *rem = 0; return 0; }
    *rem = a % b; return a / b;
}

/* ---- Pont MBF 5 octets <-> IEEE754 f32 (fonctions PURES, cf. Phosphoric) ----
 * MBF : m[0]=exposant biaisé de 128 (0 => zéro) ; m[1..4]=mantisse 32 bits
 * big-endian, bit7 de m[1]=SIGNE (le 1 de tête implicite à cette position).
 * Valeur = (-1)^signe · mantisse · 2^(exp-160). MBF 1.0 = {81 00 00 00 00}. */
static inline float math_mbf5_to_f32(const uint8_t m[5]) {
    if (m[0] == 0) return 0.0f;                              /* exposant 0 = zéro */
    uint8_t sign = m[1] & 0x80u;
    uint32_t mant = ((uint32_t)(m[1] | 0x80u) << 24) | ((uint32_t)m[2] << 16) |
                    ((uint32_t)m[3] << 8) | (uint32_t)m[4]; /* 1 implicite restauré */
    double val = ldexp((double)mant, (int)m[0] - 160);
    return (float)(sign ? -val : val);
}
/* Renvoie false si non représentable en MBF (Inf/NaN, overflow d'exposant) ;
 * underflow -> 0 (MBF n'a pas de dénormaux). */
static inline int math_f32_to_mbf5(float f, uint8_t out[5]) {
    if (isnan(f) || isinf(f)) return 0;
    if (f == 0.0f) { memset(out, 0, 5); return 1; }
    int sign = signbit(f) ? 0x80 : 0x00;
    double a = fabs((double)f);
    int e; double frac = frexp(a, &e);                      /* a = frac·2^e, frac∈[0.5,1) */
    uint64_t mant = (uint64_t)llround(frac * 4294967296.0); /* frac·2^32 */
    if (mant >= 0x100000000ULL) { mant >>= 1; e += 1; }     /* arrondi -> 2^32 : renormaliser */
    int exp = e + 128;
    if (exp > 255) return 0;                                /* overflow */
    if (exp < 1) { memset(out, 0, 5); return 1; }           /* underflow -> 0 */
    out[0] = (uint8_t)exp;
    out[1] = (uint8_t)(((mant >> 24) & 0x7Fu) | (uint32_t)sign);
    out[2] = (uint8_t)((mant >> 16) & 0xFFu);
    out[3] = (uint8_t)((mant >> 8) & 0xFFu);
    out[4] = (uint8_t)(mant & 0xFFu);
    return 1;
}

/* ---- Ops par bloc (fonctions PURES : opèrent sur un buffer mémoire quelconque
 *      -> testables en natif ; math_api() passe (uint8_t*)xram). f32 LE. ---- */
static inline float math_xram_getf32(const uint8_t *mem, uint16_t off) {
    uint32_t b = (uint32_t)mem[off] | ((uint32_t)mem[(uint16_t)(off+1)] << 8) |
                 ((uint32_t)mem[(uint16_t)(off+2)] << 16) | ((uint32_t)mem[(uint16_t)(off+3)] << 24);
    return math_b2f(b);
}
static inline void math_xram_setf32(uint8_t *mem, uint16_t off, float v) {
    uint32_t b = math_f2b(v);
    mem[off] = (uint8_t)b; mem[(uint16_t)(off+1)] = (uint8_t)(b >> 8);
    mem[(uint16_t)(off+2)] = (uint8_t)(b >> 16); mem[(uint16_t)(off+3)] = (uint8_t)(b >> 24);
}
static inline float math_vec_dot(const uint8_t *mem, uint16_t a, uint16_t b, uint16_t count) {
    float acc = 0.0f;
    for (uint16_t i = 0; i < count; ++i)
        acc += math_xram_getf32(mem, (uint16_t)(a + 4*i)) * math_xram_getf32(mem, (uint16_t)(b + 4*i));
    return acc;
}
static inline void math_vec_scale(uint8_t *mem, uint16_t ptr, uint16_t count, float a) {
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t o = (uint16_t)(ptr + 4*i);
        math_xram_setf32(mem, o, a * math_xram_getf32(mem, o));
    }
}
static inline float math_poly_eval(const uint8_t *mem, uint16_t ptr, uint16_t degree, float x) {
    /* Horner : c[deg]..c[0], résultat = ((c[deg]*x + c[deg-1])*x + ...) + c[0] */
    float acc = math_xram_getf32(mem, (uint16_t)(ptr + 4*degree));
    for (int i = (int)degree - 1; i >= 0; --i)
        acc = acc * x + math_xram_getf32(mem, (uint16_t)(ptr + 4*(uint16_t)i));
    return acc;
}

/* Handler ABI (firmware) : lit API_A + xstack, calcule, retourne. */
void math_api(void);

#endif /* _MATH_API_H_ */
