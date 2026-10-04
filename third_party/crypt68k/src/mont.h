/*
 * mont.h - crypt68k's Montgomery multiplication (internal): little-endian
 * arrays of k 32-bit limbs, R = 2^(32k), the inner loop in 68020/030/040
 * assembly around mulu.l, and in C with uint64_t elsewhere. C89 (but for
 * the uint64_t). Variable time: for public data only.
 */
#ifndef C68K_MONT_H
#define C68K_MONT_H

#include <stddef.h>
#include <stdint.h>

#define MONT_MAXK	(4096 / 32)

struct mont {
	uint32_t n[MONT_MAXK];	/* the odd modulus */
	uint32_t m0i;		/* -1/n mod 2^32 */
	int k;			/* limbs */
};

/* m->n and m->k set (n odd, > 1): the rest */
void c68k_mont_setup(struct mont *m);

/* d = x y / R mod n, for x, y < n; d may be x or y */
void c68k_mont_mul(uint32_t *d, const uint32_t *x, const uint32_t *y, const struct mont *m);

/* t (k limbs) against n: -1, 0, 1 */
int c68k_mont_cmp(const uint32_t *t, const struct mont *m);

/* t -= n, mod 2^(32k) */
void c68k_mont_sub_n(uint32_t *t, const struct mont *m);

/* v = 2v mod n, for v < n */
void c68k_mont_dbl(uint32_t *v, const struct mont *m);

/* R^2 mod n into r2 (k limbs) */
void c68k_mont_r2(uint32_t *r2, const struct mont *m);

/* big-endian bytes into k limbs (len <= 4k), and back */
void c68k_mont_decode(uint32_t *w, int k, const unsigned char *b, size_t len);
void c68k_mont_encode(unsigned char *b, size_t len, const uint32_t *w);

#endif /* C68K_MONT_H */
