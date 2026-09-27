/*
 * tpbench - the Phase 0 transputer spike: BearSSL's 15-bit public-key code
 * on a T800, checked against answers computed on the host (tpvec.h).
 *
 * Plain C89 without 64-bit integers, for the INMOS ANSI C compiler (icc),
 * and runnable on the host too. Timing uses clock(): 15625 ticks/s on the
 * transputer (its low-priority timer).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bearssl.h"
#include "tpvec.h"

static int fails;

static long ms_since(clock_t t0)
{
	clock_t d = clock() - t0;

	return (long)((double)d * 1000.0 / (double)CLOCKS_PER_SEC);
}

static void result(const char *what, int ok, long ms)
{
	printf("  %-36s %s %8ld ms\n", what, ok ? "ok  " : "FAIL", ms);
	if (!ok)
		fails++;
}

static void x25519(void)
{
	static const unsigned char k[32] = {
		0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d, 0x3b, 0x16, 0x15, 0x4b,
		0x82, 0x46, 0x5e, 0xdd, 0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18,
		0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4 };
	static const unsigned char u0[32] = {
		0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb, 0x35, 0x94, 0xc1, 0xa4,
		0x24, 0xb1, 0x5f, 0x7c, 0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
		0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c };
	static const unsigned char want[32] = {
		0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90, 0x8e, 0x94, 0xea, 0x4d,
		0xf2, 0x8d, 0x08, 0x4f, 0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7,
		0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52 };
	unsigned char u[32];
	clock_t t0;

	memcpy(u, u0, 32);
	t0 = clock();
	br_ec_c25519_m15.mul(u, 32, k, 32, BR_EC_curve25519);
	result("X25519 mul c25519_m15 (RFC 7748)", memcmp(u, want, 32) == 0,
		ms_since(t0));
}

static void p256(void)
{
	unsigned char pt[65];
	size_t glen;
	const unsigned char *g = br_ec_p256_m15.generator(BR_EC_secp256r1, &glen);
	clock_t t0;

	memcpy(pt, g, glen);
	t0 = clock();
	br_ec_p256_m15.mul(pt, glen, p256_scalar, 32, BR_EC_secp256r1);
	result("P-256 mul p256_m15", memcmp(pt, p256_expect, 65) == 0,
		ms_since(t0));
}

static void ecdsa(const char *what, const br_ec_impl *impl, int curve,
	const unsigned char *q, size_t qlen, const unsigned char *hash,
	size_t hlen, const unsigned char *sig, size_t siglen)
{
	br_ec_public_key pk;
	unsigned char h[64];
	clock_t t0;
	int good, bad;

	pk.curve = curve;
	pk.q = (unsigned char *)q;
	pk.qlen = qlen;
	memcpy(h, hash, hlen);
	t0 = clock();
	good = br_ecdsa_i15_vrfy_asn1(impl, h, hlen, &pk, sig, siglen) == 1;
	result(what, good, ms_since(t0));
	h[3] ^= 1;
	bad = br_ecdsa_i15_vrfy_asn1(impl, h, hlen, &pk, sig, siglen) == 0;
	result("  ...rejects a wrong hash", bad, 0);
}

static void rsa(void)
{
	unsigned char x[256];
	static const unsigned char e[3] = { 1, 0, 1 };
	br_rsa_public_key pk;
	clock_t t0;
	int ok;

	pk.n = (unsigned char *)rsa_n;
	pk.nlen = sizeof rsa_n;
	pk.e = (unsigned char *)e;
	pk.elen = 3;
	memcpy(x, rsa_x, sizeof x);
	t0 = clock();
	ok = br_rsa_i15_public(x, sizeof x, &pk) == 1;
	result("RSA-2048 public i15", ok && memcmp(x, rsa_expect, sizeof x) == 0,
		ms_since(t0));
}

int main(void)
{
	printf("tpbench: BearSSL i15/m15 (%ld clock ticks/s)\n",
		(long)CLOCKS_PER_SEC);
	x25519();
	p256();
	ecdsa("ECDSA P-256 verify i15/p256_m15", &br_ec_p256_m15,
		BR_EC_secp256r1, e256_q, sizeof e256_q, e256_hash,
		sizeof e256_hash, e256_sig, sizeof e256_sig);
	rsa();
	ecdsa("ECDSA P-384 verify i15/prime_i15", &br_ec_prime_i15,
		BR_EC_secp384r1, e384_q, sizeof e384_q, e384_hash,
		sizeof e384_hash, e384_sig, sizeof e384_sig);
	printf("%d failure(s)\n", fails);
	return fails != 0;
}
