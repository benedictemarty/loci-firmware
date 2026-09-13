#ifndef _BEXT_H_
#define _BEXT_H_
/* Extension BASIC `!` (opcode $AB) — voir bext.c. */
void bext_install_11(void);   /* patche la copie de ROM BASIC 1.1b servie (zone morte cassette) */
void bext_install_10(void);   /* idem BASIC 1.0 */
void bext_api_prim(void);     /* $AB */
#endif
