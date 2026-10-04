/*
 * x25519.c - X25519 (RFC 7748) in constant time (crypt68k.h). Part of
 * crypt68k, MIT licence (LICENSE).
 *
 * Field elements are 8 limbs of 32 bits, little-endian, kept below 2^256
 * (not always below p = 2^255 - 19): what overflows 2^256 is folded back
 * as 38 (2^256 = 2p + 38), with arithmetic carries, never a branch. No
 * branch or memory address here depends on the scalar or on a field
 * element's value; the ladder's swaps are masks. Fully reduced only on
 * the way out.
 */
#include <string.h>
#include "crypt68k.h"
#include "x25519.h"

/* --- the field ------------------------------------------------------------ */

static void fe_add(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
	uint64_t c = 0;
	int i;

	for (i = 0; i < 8; i++) {
		c += (uint64_t)a[i] + b[i];
		r[i] = (uint32_t)c;
		c >>= 32;
	}
	/* 2^256 = 38: fold the carry (0 or 1), then whatever that carries */
	c *= 38;
	for (i = 0; i < 8; i++) {
		c += r[i];
		r[i] = (uint32_t)c;
		c >>= 32;
	}
	r[0] += (uint32_t)c * 38;	/* r was below 38 if c is 1: no carry */
}

static void fe_sub(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
	uint64_t t;
	uint32_t bw = 0;
	int i;

	for (i = 0; i < 8; i++) {
		t = (uint64_t)a[i] - b[i] - bw;
		r[i] = (uint32_t)t;
		bw = (uint32_t)(t >> 32) & 1;
	}
	/* a borrow took 2^256 too many: give back 38 less (2^256 = 38) */
	bw *= 38;
	for (i = 0; i < 8; i++) {
		t = (uint64_t)r[i] - bw;
		r[i] = (uint32_t)t;
		bw = (uint32_t)(t >> 32) & 1;
	}
	r[0] -= bw * 38;		/* r is near 2^256 if bw is 1: no borrow */
}

/* r = t[0..7] + 38 t[8..15], below 2^256 */
static void fe_reduce512(uint32_t *r, const uint32_t *t)
{
	uint64_t c = 0;
	int i;

	for (i = 0; i < 8; i++) {
		c += (uint64_t)t[i + 8] * 38 + t[i];
		r[i] = (uint32_t)c;
		c >>= 32;
	}
	c *= 38;			/* c was at most 38 */
	for (i = 0; i < 8; i++) {
		c += r[i];
		r[i] = (uint32_t)c;
		c >>= 32;
	}
	r[0] += (uint32_t)c * 38;
}

#if !C68K_FE_ASM
void c68k_fe_mul512(uint32_t *t, const uint32_t *a, const uint32_t *b)
{
	int i, j;

	for (i = 0; i < 16; i++)
		t[i] = 0;
	for (i = 0; i < 8; i++) {
		uint64_t c = 0;

		for (j = 0; j < 8; j++) {
			c += (uint64_t)a[i] * b[j] + t[i + j];
			t[i + j] = (uint32_t)c;
			c >>= 32;
		}
		t[i + 8] = (uint32_t)c;
	}
}

void c68k_fe_sqr512(uint32_t *t, const uint32_t *a)
{
	c68k_fe_mul512(t, a, a);
}
#endif

static void fe_mul(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
	uint32_t t[16];

	c68k_fe_mul512(t, a, b);
	fe_reduce512(r, t);
}

static void fe_sqr(uint32_t *r, const uint32_t *a)
{
	uint32_t t[16];

	c68k_fe_sqr512(t, a);
	fe_reduce512(r, t);
}

/* r = a * 121665, (A - 2) / 4 for curve25519 */
static void fe_mul_a24(uint32_t *r, const uint32_t *a)
{
	uint64_t c = 0;
	int i;

	for (i = 0; i < 8; i++) {
		c += (uint64_t)a[i] * 121665;
		r[i] = (uint32_t)c;
		c >>= 32;
	}
	c *= 38;
	for (i = 0; i < 8; i++) {
		c += r[i];
		r[i] = (uint32_t)c;
		c >>= 32;
	}
	r[0] += (uint32_t)c * 38;
}

static void fe_sqr_n(uint32_t *r, const uint32_t *a, int n)
{
	fe_sqr(r, a);
	while (--n > 0)
		fe_sqr(r, r);
}

/* r = z^(p - 2) = 1/z: the addition chain of ref10, 254 squarings and
 * 11 multiplications, the same whatever z is */
static void fe_invert(uint32_t *r, const uint32_t *z)
{
	uint32_t z2[8], z9[8], z11[8], z2_5[8], z2_10[8], z2_20[8], z2_50[8], z2_100[8], t[8];

	fe_sqr(z2, z);			/* 2 */
	fe_sqr_n(t, z2, 2);		/* 8 */
	fe_mul(z9, t, z);		/* 9 */
	fe_mul(z11, z9, z2);		/* 11 */
	fe_sqr(t, z11);			/* 22 */
	fe_mul(z2_5, t, z9);		/* 2^5 - 1 */
	fe_sqr_n(t, z2_5, 5);
	fe_mul(z2_10, t, z2_5);		/* 2^10 - 1 */
	fe_sqr_n(t, z2_10, 10);
	fe_mul(z2_20, t, z2_10);	/* 2^20 - 1 */
	fe_sqr_n(t, z2_20, 20);
	fe_mul(t, t, z2_20);		/* 2^40 - 1 */
	fe_sqr_n(t, t, 10);
	fe_mul(z2_50, t, z2_10);	/* 2^50 - 1 */
	fe_sqr_n(t, z2_50, 50);
	fe_mul(z2_100, t, z2_50);	/* 2^100 - 1 */
	fe_sqr_n(t, z2_100, 100);
	fe_mul(t, t, z2_100);		/* 2^200 - 1 */
	fe_sqr_n(t, t, 50);
	fe_mul(t, t, z2_50);		/* 2^250 - 1 */
	fe_sqr_n(t, t, 5);		/* 2^255 - 32 */
	fe_mul(r, t, z11);		/* 2^255 - 21 = p - 2 */
}

/* swap a and b if swap is 1 (0 or 1), in the same time either way */
static void fe_cswap(uint32_t *a, uint32_t *b, uint32_t swap)
{
	uint32_t mask = 0 - swap, x;
	int i;

	for (i = 0; i < 8; i++) {
		x = mask & (a[i] ^ b[i]);
		a[i] ^= x;
		b[i] ^= x;
	}
}

static void fe_frombytes(uint32_t *r, const unsigned char *b)
{
	int i;

	for (i = 0; i < 8; i++)
		r[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8
			| (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
	r[7] &= 0x7fffffff;		/* RFC 7748: the top bit is ignored */
}

/* fully reduced, little-endian */
static void fe_tobytes(unsigned char *b, const uint32_t *a)
{
	uint32_t v[8], t[8], top, mask;
	uint64_t c;
	int i;

	/* below 2^255 + 19: fold bit 255 as 19 */
	memcpy(v, a, sizeof v);
	top = v[7] >> 31;
	v[7] &= 0x7fffffff;
	c = (uint64_t)top * 19;
	for (i = 0; i < 8; i++) {
		c += v[i];
		v[i] = (uint32_t)c;
		c >>= 32;
	}
	/* v >= p exactly when v + 19 reaches 2^255: then v - p is that, less
	 * 2^255; chosen by a mask */
	c = 19;
	for (i = 0; i < 8; i++) {
		c += v[i];
		t[i] = (uint32_t)c;
		c >>= 32;
	}
	mask = 0 - (t[7] >> 31);
	t[7] &= 0x7fffffff;
	for (i = 0; i < 8; i++)
		v[i] = (t[i] & mask) | (v[i] & ~mask);
	for (i = 0; i < 8; i++) {
		b[4 * i] = (unsigned char)v[i];
		b[4 * i + 1] = (unsigned char)(v[i] >> 8);
		b[4 * i + 2] = (unsigned char)(v[i] >> 16);
		b[4 * i + 3] = (unsigned char)(v[i] >> 24);
	}
}

/* --- the ladder ------------------------------------------------------------ */

void c68k_x25519(unsigned char *out, const unsigned char *scalar, const unsigned char *u)
{
	uint32_t x1[8], x2[8], z2[8], x3[8], z3[8];
	uint32_t a[8], aa[8], b[8], bb[8], e[8], c[8], d[8], da[8], cb[8];
	unsigned char k[32];
	uint32_t swap = 0, kt;
	int t;

	memcpy(k, scalar, 32);
	k[0] &= 0xf8;
	k[31] &= 0x7f;
	k[31] |= 0x40;
	fe_frombytes(x1, u);
	memset(x2, 0, sizeof x2);
	x2[0] = 1;
	memset(z2, 0, sizeof z2);
	memcpy(x3, x1, sizeof x3);
	memset(z3, 0, sizeof z3);
	z3[0] = 1;
	for (t = 254; t >= 0; t--) {
		kt = (uint32_t)(k[t >> 3] >> (t & 7)) & 1;
		swap ^= kt;
		fe_cswap(x2, x3, swap);
		fe_cswap(z2, z3, swap);
		swap = kt;
		fe_add(a, x2, z2);
		fe_sqr(aa, a);
		fe_sub(b, x2, z2);
		fe_sqr(bb, b);
		fe_sub(e, aa, bb);
		fe_add(c, x3, z3);
		fe_sub(d, x3, z3);
		fe_mul(da, d, a);
		fe_mul(cb, c, b);
		fe_add(x3, da, cb);
		fe_sqr(x3, x3);
		fe_sub(z3, da, cb);
		fe_sqr(z3, z3);
		fe_mul(z3, z3, x1);
		fe_mul(x2, aa, bb);
		fe_mul_a24(z2, e);
		fe_add(z2, z2, aa);
		fe_mul(z2, e, z2);
	}
	fe_cswap(x2, x3, swap);
	fe_cswap(z2, z3, swap);
	fe_invert(z2, z2);
	fe_mul(x2, x2, z2);
	fe_tobytes(out, x2);
	memset(k, 0, sizeof k);
}

void c68k_x25519_base(unsigned char *out, const unsigned char *scalar)
{
	static const unsigned char nine[32] = { 9 };

	c68k_x25519(out, scalar, nine);
}
