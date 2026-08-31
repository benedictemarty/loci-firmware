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

/* Handler ABI (firmware) : lit API_A + xstack, calcule, retourne. */
void math_api(void);

#endif /* _MATH_API_H_ */
