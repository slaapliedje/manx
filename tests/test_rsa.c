/*
 * test_rsa - rsavrfy against BearSSL's br_rsa_i32: the public operation on
 * random odd moduli of every size up to 4096 bits (any odd number will do
 * for x^e mod n), with random exponents and inputs and the edges (x = 0,
 * x = n - 1, x >= n, even moduli, leading zeros, a wrong length, e = 0);
 * then PKCS#1 signatures made with keys BearSSL generates: good ones
 * accepted with the same hash, broken ones refused.
 *
 *   test_rsa [cases [nosig]]    (on the TT: test_rsa 300 nosig; BearSSL's
 *                               4096-bit keys take a 68030 too long)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bearssl.h"
#include "rsavrfy.h"

static int fails, runs;
static uint32_t g_rng = 2463534242u;

static uint32_t rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

static void fill(unsigned char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		b[i] = (unsigned char)rnd();
}

/* the same answer from both, for n (nlen bytes, maybe with leading
 * zeros), e and x (xlen bytes) */
static void same(const char *what, const unsigned char *n, size_t nlen,
	const unsigned char *e, size_t elen, const unsigned char *x, size_t xlen)
{
	unsigned char a[512], b[512];
	br_rsa_public_key pk;
	uint32_t ra, rb;

	pk.n = (unsigned char *)n;
	pk.nlen = nlen;
	pk.e = (unsigned char *)e;
	pk.elen = elen;
	memcpy(a, x, xlen);
	memcpy(b, x, xlen);
	ra = rsavrfy_public(a, xlen, &pk);
	rb = br_rsa_i32_public(b, xlen, &pk);
	runs++;
	if (ra != rb || (ra && memcmp(a, b, xlen) != 0)) {
		fails++;
		if (fails < 10)
			printf("FAIL %s: nlen %lu elen %lu xlen %lu: rsavrfy %lu, i32 %lu%s\n",
				what, (unsigned long)nlen, (unsigned long)elen,
				(unsigned long)xlen, (unsigned long)ra, (unsigned long)rb,
				ra == rb ? ", different answers" : "");
	}
}

/* a random odd modulus of len bytes (top byte nonzero, not 1) */
static void modulus(unsigned char *n, size_t len)
{
	fill(n, len);
	n[0] |= (unsigned char)(len == 1 ? 2 : 1);
	n[len - 1] |= 1;
}

/* a random x below n (same length): the top byte kept below n's */
static void below(unsigned char *x, const unsigned char *n, size_t len)
{
	fill(x, len);
	x[0] = (unsigned char)(n[0] > 1 ? rnd() % n[0] : 0);
	if (len == 1)
		x[0] = (unsigned char)(rnd() % n[0]);
}

static void publics(int cases)
{
	static const unsigned char e65537[3] = { 1, 0, 1 }, e3[1] = { 3 },
		e0[2] = { 0, 0 }, e1[1] = { 1 }, elead[5] = { 0, 0, 1, 0, 1 };
	unsigned char n[514], x[512], e[4];
	int i;

	for (i = 0; i < cases; i++) {
		size_t len = i < cases * 2 / 3 ? 1 + rnd() % 64
			: i < cases * 29 / 30 ? 1 + rnd() % 256 : 1 + rnd() % 512;
		size_t elen = 1 + rnd() % 4;

		modulus(n, len);
		fill(e, elen);
		below(x, n, len);
		switch (rnd() % 4) {
		case 0:
			same("random e", n, len, e, elen, x, len);
			break;
		case 1:
			same("e = 65537", n, len, e65537, 3, x, len);
			break;
		case 2:
			same("e = 3", n, len, e3, 1, x, len);
			break;
		default:
			same("e leading zeros", n, len, elead, 5, x, len);
		}
	}
	/* the sizes certificates use, e = 65537 */
	for (i = 0; i < 6; i++) {
		static const size_t sizes[] = { 128, 256, 384, 512, 255, 257 };
		size_t len = sizes[i];

		modulus(n, len);
		n[0] |= 0x80;
		below(x, n, len);
		same("certificate sizes", n, len, e65537, 3, x, len);
	}
	/* the edges */
	modulus(n, 256);
	memset(x, 0, 256);
	same("x = 0", n, 256, e65537, 3, x, 256);
	memcpy(x, n, 256);
	x[255] -= 1;
	same("x = n - 1", n, 256, e65537, 3, x, 256);
	memcpy(x, n, 256);
	same("x = n", n, 256, e65537, 3, x, 256);
	memset(x, 0xff, 256);
	same("x > n", n, 256, e65537, 3, x, 256);
	below(x, n, 256);
	same("e = 0", n, 256, e0, 2, x, 256);
	same("e = 1", n, 256, e1, 1, x, 256);
	same("xlen short", n, 256, e65537, 3, x, 255);
	n[255] &= 0xfe;
	same("even modulus", n, 256, e65537, 3, x, 256);
	/* leading zeros on n: x has the length without them */
	n[0] = n[1] = 0;
	modulus(n + 2, 254);
	below(x, n + 2, 254);
	same("n leading zeros", n, 256, e65537, 3, x, 254);
	same("n leading zeros, xlen with them", n, 256, e65537, 3, x, 256);
}

static void signatures(void)
{
	const unsigned char *oid = BR_HASH_OID_SHA256;
	static const unsigned sizes[] = { 1024, 2048, 3072, 4096 };
	static unsigned char kpriv[BR_RSA_KBUF_PRIV_SIZE(4096)];
	static unsigned char kpub[BR_RSA_KBUF_PUB_SIZE(4096)];
	br_hmac_drbg_context rng;
	br_rsa_private_key sk;
	br_rsa_public_key pk;
	unsigned char hash[32], got[32], sig[512];
	size_t i;

	br_hmac_drbg_init(&rng, &br_sha256_vtable, "test_rsa", 8);
	for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		size_t len = sizes[i] / 8;
		char what[64];

		if (!br_rsa_i31_keygen(&rng.vtable, &sk, kpriv, &pk, kpub, sizes[i], 65537)) {
			printf("FAIL keygen %u\n", sizes[i]);
			fails++;
			continue;
		}
		fill(hash, sizeof hash);
		if (!br_rsa_i31_pkcs1_sign(oid, hash, sizeof hash, &sk, sig)) {
			printf("FAIL sign %u\n", sizes[i]);
			fails++;
			continue;
		}
		runs++;
		snprintf(what, sizeof what, "RSA-%u signature", sizes[i]);
		memset(got, 0, sizeof got);
		if (!rsavrfy_pkcs1(sig, len, oid, sizeof hash, &pk, got)
			|| memcmp(got, hash, sizeof hash) != 0) {
			printf("FAIL %s: refused, or a different hash\n", what);
			fails++;
		}
		runs++;
		sig[len / 2] ^= 0x10;
		if (rsavrfy_pkcs1(sig, len, oid, sizeof hash, &pk, got)) {
			printf("FAIL %s: a broken one accepted\n", what);
			fails++;
		}
		sig[len / 2] ^= 0x10;
		runs++;
		if (rsavrfy_pkcs1(sig, len - 1, oid, sizeof hash, &pk, got)) {
			printf("FAIL %s: a short one accepted\n", what);
			fails++;
		}
	}
}

int main(int argc, char **argv)
{
	publics(argc > 1 ? atoi(argv[1]) : 3000);
	if (argc <= 2 || strcmp(argv[2], "nosig") != 0)
		signatures();
	printf("rsa: %d/%d passed\n", runs - fails, runs);
	return fails ? 1 : 0;
}
