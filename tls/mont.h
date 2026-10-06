/*
 * mont.h - Montgomery multiplication for checking signatures (rsavrfy,
 * ecvrfy): little-endian arrays of k 32-bit limbs, R = 2^(32k), the inner
 * loop in 68030 assembly around mulu.l, in transputer assembly around lmul
 * (icc, for the ATW800/2's T425), and in C with uint64_t elsewhere, or on
 * 16-bit halves where there is no 64-bit type (BR_NO_U64: Helios C). C89.
 * Variable time: for public data only.
 */
#ifndef MANX_MONT_H
#define MANX_MONT_H

#include <stddef.h>
#ifdef _ICC
typedef unsigned int uint32_t;	/* icc (1990) has no stdint.h; int is 32 bits */
#else
#include <stdint.h>
#endif

#define MONT_MAXK	(4096 / 32)

struct mont {
	uint32_t n[MONT_MAXK];	/* the odd modulus */
	uint32_t m0i;		/* -1/n mod 2^32 */
	int k;			/* limbs */
};

/* m->n and m->k set (n odd, > 1): the rest */
void mont_setup(struct mont *m);

/* d = x y / R mod n, for x, y < n; d may be x or y */
void mont_mul(uint32_t *d, const uint32_t *x, const uint32_t *y, const struct mont *m);

/* t (k limbs) against n: -1, 0, 1 */
int mont_cmp(const uint32_t *t, const struct mont *m);

/* t -= n, mod 2^(32k) */
void mont_sub_n(uint32_t *t, const struct mont *m);

/* v = 2v mod n, for v < n */
void mont_dbl(uint32_t *v, const struct mont *m);

/* R^2 mod n into r2 (k limbs) */
void mont_r2(uint32_t *r2, const struct mont *m);

/* big-endian bytes into k limbs (len <= 4k), and back */
void mont_decode(uint32_t *w, int k, const unsigned char *b, size_t len);
void mont_encode(unsigned char *b, size_t len, const uint32_t *w);

#endif /* MANX_MONT_H */
