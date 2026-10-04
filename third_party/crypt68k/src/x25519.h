/*
 * x25519.h - X25519's field multiplication (internal): the 512-bit
 * product of two 256-bit numbers (8 limbs of 32 bits, little-endian), in
 * the same time whatever their values. 68k assembly where mulu.l has
 * 32x32 -> 64 (cpu.h), C elsewhere.
 */
#ifndef C68K_X25519_H
#define C68K_X25519_H

#include <stdint.h>
#include "cpu.h"

/* the assembly versions are in fe_m68k.c */
#define C68K_FE_ASM	C68K_MULU64

void c68k_fe_mul512(uint32_t *t, const uint32_t *a, const uint32_t *b);
void c68k_fe_sqr512(uint32_t *t, const uint32_t *a);

#endif /* C68K_X25519_H */
