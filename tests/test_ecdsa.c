/*
 * test_ecdsa - ecvrfy against BearSSL's br_ecdsa_i31 (with br_ec_all_m31):
 * keys BearSSL generates on P-256 and P-384, its deterministic signatures
 * over hashes of 20 to 64 bytes, checked by both; the same signatures
 * with s replaced by n - s (also valid); then the same answer from both
 * for broken signatures, other hashes, points off the curve, r or s
 * zero or not below n, odd lengths. P-521 goes through to BearSSL.
 *
 *   test_ecdsa [keys]     (default 40 a curve; on the TT: test_ecdsa 2,
 *                         BearSSL's P-384 check takes it 46 s)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bearssl.h"
#include "ecvrfy.h"

static int fails, runs;
static uint32_t g_rng = 88172645u;

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

/* both must say want (-1: both the same, whatever it is) */
static void both(const char *what, int want, const void *hash, size_t hlen,
	const br_ec_public_key *pk, const void *sig, size_t slen, int raw)
{
	uint32_t a, b;

	if (raw) {
		a = ecvrfy_raw(&br_ec_all_m31, hash, hlen, pk, sig, slen);
		b = br_ecdsa_i31_vrfy_raw(&br_ec_all_m31, hash, hlen, pk, sig, slen);
	} else {
		a = ecvrfy_asn1(&br_ec_all_m31, hash, hlen, pk, sig, slen);
		b = br_ecdsa_i31_vrfy_asn1(&br_ec_all_m31, hash, hlen, pk, sig, slen);
	}
	runs++;
	if (a != b || (want >= 0 && a != (uint32_t)want)) {
		fails++;
		if (fails < 12)
			printf("FAIL %s (curve %d, hash %lu): ecvrfy %lu, i31 %lu, want %d\n",
				what, pk->curve, (unsigned long)hlen, (unsigned long)a,
				(unsigned long)b, want);
	}
}

/* big-endian x = n - x (len bytes) */
static void negate(unsigned char *x, const unsigned char *n, size_t len)
{
	int bw = 0;
	size_t i;

	for (i = len; i-- > 0;) {
		int d = n[i] - x[i] - bw;

		bw = d < 0;
		x[i] = (unsigned char)(d & 0xff);
	}
}

static const unsigned char *order(int curve, size_t *len)
{
	/* the orders (FIPS 186-4) */
	static const unsigned char n256[32] = {
		0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
		0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51 };
	static const unsigned char n384[48] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xc7, 0x63, 0x4d, 0x81, 0xf4, 0x37, 0x2d, 0xdf, 0x58, 0x1a, 0x0d, 0xb2,
		0x48, 0xb0, 0xa7, 0x7a, 0xec, 0xec, 0x19, 0x6a, 0xcc, 0xc5, 0x29, 0x73 };

	*len = curve == BR_EC_secp256r1 ? 32 : 48;
	return curve == BR_EC_secp256r1 ? n256 : n384;
}

static void one_key(br_hmac_drbg_context *rng, int curve)
{
	static const br_hash_class *const hashes[] = {
		&br_sha1_vtable, &br_sha256_vtable, &br_sha384_vtable, &br_sha512_vtable };
	unsigned char kpriv[BR_EC_KBUF_PRIV_MAX_SIZE], kpub[BR_EC_KBUF_PUB_MAX_SIZE];
	unsigned char hash[64], sig[160], raw[160], q[BR_EC_KBUF_PUB_MAX_SIZE];
	br_ec_private_key sk;
	br_ec_public_key pk, bad;
	const br_hash_class *hf = hashes[rnd() % 4];
	size_t hlen = (hf->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
	size_t slen, rlen, nlen, i;
	const unsigned char *n = order(curve, &nlen);

	if (!br_ec_keygen(&rng->vtable, &br_ec_all_m31, &sk, kpriv, curve)
		|| !br_ec_compute_pub(&br_ec_all_m31, &pk, kpub, &sk)) {
		printf("FAIL keygen on curve %d\n", curve);
		fails++;
		return;
	}
	fill(hash, hlen);
	slen = br_ecdsa_i31_sign_asn1(&br_ec_all_m31, hf, hash, &sk, sig);
	if (slen == 0) {
		printf("FAIL sign on curve %d\n", curve);
		fails++;
		return;
	}
	both("good", 1, hash, hlen, &pk, sig, slen, 0);
	memcpy(raw, sig, slen);
	rlen = br_ecdsa_asn1_to_raw(raw, slen);
	both("good, raw", 1, hash, hlen, &pk, raw, rlen, 1);

	/* s -> n - s: as good */
	negate(raw + rlen / 2, n, nlen);
	both("n - s", 1, hash, hlen, &pk, raw, rlen, 1);
	negate(raw + rlen / 2, n, nlen);

	/* another hash (in the bytes used: those to the order's length), a
	 * broken signature */
	hash[rnd() % (hlen < nlen ? hlen : nlen)] ^= (unsigned char)(1 << rnd() % 8);
	both("other hash", 0, hash, hlen, &pk, raw, rlen, 1);
	for (i = 0; i < 3; i++) {
		unsigned char b[160];
		size_t at = rnd() % slen;

		memcpy(b, sig, slen);
		b[at] ^= (unsigned char)(1 << rnd() % 8);
		both("broken ASN.1", -1, hash, hlen, &pk, b, slen, 0);
	}

	/* r, s = 0 or n; odd length; random */
	{
		unsigned char b[160];
		size_t h = rlen / 2;

		memcpy(b, raw, rlen);
		memset(b, 0, h);
		both("r = 0", 0, hash, hlen, &pk, b, rlen, 1);
		memcpy(b, raw, rlen);
		memset(b + h, 0, h);
		both("s = 0", 0, hash, hlen, &pk, b, rlen, 1);
		if (h == nlen) {
			memcpy(b, raw, rlen);
			memcpy(b, n, nlen);
			both("r = n", 0, hash, hlen, &pk, b, rlen, 1);
			memcpy(b, raw, rlen);
			memcpy(b + h, n, nlen);
			both("s = n", 0, hash, hlen, &pk, b, rlen, 1);
		}
		both("odd length", 0, hash, hlen, &pk, raw, rlen - 1, 1);
		fill(b, rlen);
		b[0] &= 0x7f;
		b[h] &= 0x7f;
		both("random r, s", 0, hash, hlen, &pk, b, rlen, 1);
	}

	/* the key off the curve, the wrong length, compressed */
	memcpy(q, pk.q, pk.qlen);
	bad = pk;
	bad.q = q;
	q[pk.qlen - 1] ^= 1;
	both("Q off the curve", 0, hash, hlen, &bad, raw, rlen, 1);
	q[pk.qlen - 1] ^= 1;
	bad.qlen = pk.qlen - 1;
	both("Q short", 0, hash, hlen, &bad, raw, rlen, 1);
	bad.qlen = pk.qlen;
	q[0] = 0x02;
	both("Q compressed", 0, hash, hlen, &bad, raw, rlen, 1);
}

int main(int argc, char **argv)
{
	int keys = argc > 1 ? atoi(argv[1]) : 40, i;
	br_hmac_drbg_context rng;

	br_hmac_drbg_init(&rng, &br_sha256_vtable, "test_ecdsa", 10);
	for (i = 0; i < keys; i++) {
		one_key(&rng, BR_EC_secp256r1);
		one_key(&rng, BR_EC_secp384r1);
	}
	/* P-521: to BearSSL */
	{
		unsigned char kpriv[BR_EC_KBUF_PRIV_MAX_SIZE], kpub[BR_EC_KBUF_PUB_MAX_SIZE];
		unsigned char hash[64], sig[160];
		br_ec_private_key sk;
		br_ec_public_key pk;
		size_t slen;

		br_ec_keygen(&rng.vtable, &br_ec_all_m31, &sk, kpriv, BR_EC_secp521r1);
		br_ec_compute_pub(&br_ec_all_m31, &pk, kpub, &sk);
		fill(hash, 64);
		slen = br_ecdsa_i31_sign_asn1(&br_ec_all_m31, &br_sha512_vtable, hash, &sk, sig);
		both("P-521 good", 1, hash, 64, &pk, sig, slen, 0);
		hash[5] ^= 1;
		both("P-521 other hash", 0, hash, 64, &pk, sig, slen, 0);
	}
	printf("ecdsa: %d/%d passed\n", runs - fails, runs);
	return fails ? 1 : 0;
}
