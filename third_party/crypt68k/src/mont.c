/*
 * mont.c - crypt68k's Montgomery multiplication (mont.h). Part of
 * crypt68k, MIT licence (LICENSE).
 */
#include <string.h>
#include "cpu.h"
#include "mont.h"

#if C68K_MULU64
/* r[0..k-1] += a[0..k-1] * b, 0 < k <= 65536; the carry out. add.l to
 * memory leaves its carry in X for the high word. (No immediates: one
 * SVR4 toolchain's wrapper takes '#' for a comment.) About 2.7 us a
 * multiply-add on a 32 MHz 68030, all told. */
static uint32_t row(uint32_t *r, const uint32_t *a, uint32_t b, int k)
{
	uint32_t c = 0, z = 0;
	int left = k - 1;

	__asm__ volatile(
		"1:\n\t"
		"move.l (%[a])+,%%d1\n\t"
		"mulu.l %[b],%%d2:%%d1\n\t"
		"add.l %[c],%%d1\n\t"
		"addx.l %[z],%%d2\n\t"
		"add.l %%d1,(%[r])+\n\t"
		"addx.l %[z],%%d2\n\t"
		"move.l %%d2,%[c]\n\t"
		"dbra %[left],1b"
		: [a] "+a" (a), [r] "+a" (r), [c] "+d" (c), [left] "+d" (left),
		  [z] "+d" (z)
		: [b] "d" (b)
		: "d1", "d2", "cc", "memory");
	return c;
}
#else
static uint32_t row(uint32_t *r, const uint32_t *a, uint32_t b, int k)
{
	uint32_t c = 0;
	int i;

	for (i = 0; i < k; i++) {
		uint64_t z = (uint64_t)a[i] * b + r[i] + c;

		r[i] = (uint32_t)z;
		c = (uint32_t)(z >> 32);
	}
	return c;
}
#endif

int c68k_mont_cmp(const uint32_t *t, const struct mont *m)
{
	int i;

	for (i = m->k - 1; i >= 0; i--)
		if (t[i] != m->n[i])
			return t[i] > m->n[i] ? 1 : -1;
	return 0;
}

void c68k_mont_sub_n(uint32_t *t, const struct mont *m)
{
	uint32_t bw = 0;
	int i;

	for (i = 0; i < m->k; i++) {
		uint32_t a = t[i], d = a - m->n[i], b1 = a < m->n[i];

		t[i] = d - bw;
		bw = b1 | (d < bw);
	}
}

/* CIOS, the running sum sliding up buf a limb a turn */
void c68k_mont_mul(uint32_t *d, const uint32_t *x, const uint32_t *y,
	const struct mont *m)
{
	uint32_t buf[2 * MONT_MAXK + 2];
	uint32_t *t = buf;
	int i, k = m->k;

	memset(buf, 0, (2 * (size_t)k + 2) * sizeof buf[0]);
	for (i = 0; i < k; i++) {
		uint32_t c, s;

		/* t < 2n: t[k] is 0 or 1, t[k + 1] is 0 */
		t = buf + i;
		c = row(t, x, y[i], k);
		s = t[k] + c;
		t[k + 1] += s < c;
		t[k] = s;
		c = row(t, m->n, t[0] * m->m0i, k);
		s = t[k] + c;
		t[k + 1] += s < c;
		t[k] = s;
	}
	t = buf + k;
	if (t[k] || c68k_mont_cmp(t, m) >= 0)
		c68k_mont_sub_n(t, m);
	memcpy(d, t, (size_t)k * sizeof *t);
}

void c68k_mont_dbl(uint32_t *v, const struct mont *m)
{
	uint32_t c = 0;
	int i;

	for (i = 0; i < m->k; i++) {
		uint32_t w = v[i];

		v[i] = w << 1 | c;
		c = w >> 31;
	}
	if (c || c68k_mont_cmp(v, m) >= 0)
		c68k_mont_sub_n(v, m);
}

void c68k_mont_decode(uint32_t *w, int k, const unsigned char *b, size_t len)
{
	size_t i;

	memset(w, 0, (size_t)k * sizeof *w);
	for (i = 0; i < len; i++) {
		size_t j = len - 1 - i;

		w[j >> 2] |= (uint32_t)b[i] << ((j & 3) << 3);
	}
}

void c68k_mont_encode(unsigned char *b, size_t len, const uint32_t *w)
{
	size_t i;

	for (i = 0; i < len; i++) {
		size_t j = len - 1 - i;

		b[i] = (unsigned char)(w[j >> 2] >> ((j & 3) << 3));
	}
}

void c68k_mont_setup(struct mont *m)
{
	uint32_t y = m->n[0];
	int i;

	/* -1/n mod 2^32 by Newton: correct to 3, 6, 12, 24, 48 bits */
	for (i = 0; i < 4; i++)
		y *= 2 - m->n[0] * y;
	m->m0i = -y;
}

/* R mod n from 2^(bits-1) by doubling, 2^(8k) R by 8k more, squared
 * twice: 2^(32k) R = R^2 */
void c68k_mont_r2(uint32_t *r2, const struct mont *m)
{
	uint32_t top = m->n[m->k - 1];
	int bits = 32 * (m->k - 1), i;

	while (top) {
		bits++;
		top >>= 1;
	}
	memset(r2, 0, (size_t)m->k * sizeof r2[0]);
	r2[(bits - 1) >> 5] = (uint32_t)1 << ((bits - 1) & 31);
	for (i = 32 * m->k - bits + 1 + 8 * m->k; i > 0; i--)
		c68k_mont_dbl(r2, m);
	c68k_mont_mul(r2, r2, r2, m);
	c68k_mont_mul(r2, r2, r2, m);
}
