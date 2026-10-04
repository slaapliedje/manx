/*
 * rsavrfy.c - RSA for checking signatures (rsavrfy.h).
 *
 * Numbers are little-endian arrays of 32-bit limbs, k of them for a
 * modulus of up to 32k bits. Montgomery multiplication is CIOS with the
 * running sum sliding up a buffer a limb a turn; R = 2^(32k). R^2 mod n
 * comes from doubling: R mod n from 2^(bits-1), then 2^(8k) R by 8k more
 * doublings, squared twice. Measured on the TT (docs/phase0-results.md,
 * "Hand-written lmul"): RSA-2048's exponentiation 0.43 s against
 * br_rsa_i32's ~2.4 s.
 */
#include <string.h>
#include "rsavrfy.h"

#define MAXK	(4096 / 32)	/* BearSSL's BR_MAX_RSA_SIZE */

/* in BearSSL (src/rsa/rsa_pkcs1_sig_unpad.c), not in its public headers */
uint32_t br_rsa_pkcs1_sig_unpad(const unsigned char *sig, size_t sig_len,
	const unsigned char *hash_oid, size_t hash_len, unsigned char *hash_out);

struct mod {
	uint32_t n[MAXK];
	uint32_t m0i;		/* -1/n mod 2^32 */
	int k;
};

#if defined(__GNUC__) && (defined(__mc68020__) || defined(__mc68030__) \
	|| defined(__mc68040__))
/* r[0..k-1] += a[0..k-1] * b, 0 < k <= 65536; the carry out. add.l to
 * memory leaves its carry in X for the high word. (No immediates: the
 * SVR4 toolchain's wrapper takes '#' for a comment.) */
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

/* t (k limbs) against n */
static int cmp_n(const uint32_t *t, const struct mod *m)
{
	int i;

	for (i = m->k - 1; i >= 0; i--)
		if (t[i] != m->n[i])
			return t[i] > m->n[i] ? 1 : -1;
	return 0;
}

/* t -= n (mod 2^(32k)) */
static void sub_n(uint32_t *t, const struct mod *m)
{
	uint32_t bw = 0;
	int i;

	for (i = 0; i < m->k; i++) {
		uint32_t a = t[i], d = a - m->n[i], b1 = a < m->n[i];

		t[i] = d - bw;
		bw = b1 | (d < bw);
	}
}

/* d = x y / R mod n, for x, y < n; d may be x or y */
static void montmul(uint32_t *d, const uint32_t *x, const uint32_t *y,
	const struct mod *m)
{
	uint32_t buf[2 * MAXK + 2];
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
	if (t[k] || cmp_n(t, m) >= 0)
		sub_n(t, m);
	memcpy(d, t, (size_t)k * sizeof *t);
}

/* v = 2v mod n, for v < n */
static void dbl(uint32_t *v, const struct mod *m)
{
	uint32_t c = 0;
	int i;

	for (i = 0; i < m->k; i++) {
		uint32_t w = v[i];

		v[i] = w << 1 | c;
		c = w >> 31;
	}
	if (c || cmp_n(v, m) >= 0)
		sub_n(v, m);
}

/* big-endian bytes into k limbs (len <= 4k) */
static void decode(uint32_t *w, int k, const unsigned char *b, size_t len)
{
	size_t i;

	memset(w, 0, (size_t)k * sizeof *w);
	for (i = 0; i < len; i++) {
		size_t j = len - 1 - i;

		w[j >> 2] |= (uint32_t)b[i] << ((j & 3) << 3);
	}
}

static void encode(unsigned char *b, size_t len, const uint32_t *w)
{
	size_t i;

	for (i = 0; i < len; i++) {
		size_t j = len - 1 - i;

		b[i] = (unsigned char)(w[j >> 2] >> ((j & 3) << 3));
	}
}

uint32_t rsavrfy_public(unsigned char *x, size_t xlen, const br_rsa_public_key *pk)
{
	struct mod m;
	uint32_t r2[MAXK], xm[MAXK], acc[MAXK];
	const unsigned char *n = pk->n, *e = pk->e;
	size_t nlen = pk->nlen, elen = pk->elen;
	int k, bits, i, ebit;
	uint32_t y, top;

	while (nlen > 0 && *n == 0) {
		n++;
		nlen--;
	}
	if (nlen == 0 || nlen > 4 * MAXK || xlen != nlen || !(n[nlen - 1] & 1))
		return 0;
	k = (int)((nlen + 3) / 4);
	m.k = k;
	decode(m.n, k, n, nlen);
	decode(xm, k, x, xlen);
	if (cmp_n(xm, &m) >= 0)
		return 0;
	if (k == 1 && m.n[0] == 1)
		return 0;		/* n = 1: no Montgomery form */

	/* -1/n mod 2^32 by Newton: correct to 3, 6, 12, 24, 48 bits */
	y = m.n[0];
	for (i = 0; i < 4; i++)
		y *= 2 - m.n[0] * y;
	m.m0i = -y;

	/* R^2 mod n */
	top = m.n[k - 1];
	bits = 32 * (k - 1);
	while (top) {
		bits++;
		top >>= 1;
	}
	memset(r2, 0, (size_t)k * sizeof r2[0]);
	r2[(bits - 1) >> 5] = (uint32_t)1 << ((bits - 1) & 31);
	for (i = 32 * k - bits + 1 + 8 * k; i > 0; i--)
		dbl(r2, &m);
	montmul(r2, r2, r2, &m);
	montmul(r2, r2, r2, &m);

	/* left to right over e's bits, from the one below its top bit */
	while (elen > 0 && *e == 0) {
		e++;
		elen--;
	}
	if (elen == 0) {
		memset(x, 0, xlen);	/* x^0 = 1, as br_i32_modpow gives */
		x[xlen - 1] = 1;
		return 1;
	}
	montmul(xm, xm, r2, &m);
	memcpy(acc, xm, (size_t)k * sizeof acc[0]);
	for (ebit = 7; !(e[0] >> ebit & 1); ebit--)
		;
	for (i = 0; i < (int)elen; i++) {
		int b = i == 0 ? ebit - 1 : 7;

		for (; b >= 0; b--) {
			montmul(acc, acc, acc, &m);
			if (e[i] >> b & 1)
				montmul(acc, acc, xm, &m);
		}
	}
	memset(r2, 0, (size_t)k * sizeof r2[0]);
	r2[0] = 1;
	montmul(acc, acc, r2, &m);
	encode(x, xlen, acc);
	return 1;
}

uint32_t rsavrfy_pkcs1(const unsigned char *x, size_t xlen,
	const unsigned char *hash_oid, size_t hash_len,
	const br_rsa_public_key *pk, unsigned char *hash_out)
{
	unsigned char sig[4 * MAXK];

	if (xlen > sizeof sig)
		return 0;
	memcpy(sig, x, xlen);
	if (!rsavrfy_public(sig, xlen, pk))
		return 0;
	return br_rsa_pkcs1_sig_unpad(sig, xlen, hash_oid, hash_len, hash_out);
}
