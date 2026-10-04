/*
 * bearssl_x25519.c - crypt68k's X25519 as a BearSSL br_ec_impl
 * (crypt68k_bearssl.h), apart from the signature checkers so a program
 * can take one without the other. Part of crypt68k, MIT licence
 * (LICENSE).
 */
#include <string.h>
#include "crypt68k.h"
#include "crypt68k_bearssl.h"

static const unsigned char c25519_gen[32] = { 9 };
static const unsigned char c25519_order[32] = {
	0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static const unsigned char *c25519_generator(int curve, size_t *len)
{
	(void)curve;
	*len = 32;
	return c25519_gen;
}

static const unsigned char *c25519_orderf(int curve, size_t *len)
{
	(void)curve;
	*len = 32;
	return c25519_order;
}

static size_t c25519_xoff(int curve, size_t *len)
{
	(void)curve;
	*len = 32;
	return 0;
}

/* G = kb G: kb little-endian, kblen <= 32 (zero-padded), as BearSSL */
static uint32_t c25519_mul(unsigned char *G, size_t Glen, const unsigned char *kb,
	size_t kblen, int curve)
{
	unsigned char k[32];

	(void)curve;
	if (Glen != 32 || kblen > 32)
		return 0;
	memcpy(k, kb, kblen);
	memset(k + kblen, 0, 32 - kblen);
	c68k_x25519(G, k, G);
	memset(k, 0, sizeof k);
	return 1;
}

static size_t c25519_mulgen(unsigned char *R, const unsigned char *x, size_t xlen, int curve)
{
	memcpy(R, c25519_gen, 32);
	c25519_mul(R, 32, x, xlen, curve);
	return 32;
}

static uint32_t c25519_muladd(unsigned char *A, const unsigned char *B, size_t len,
	const unsigned char *x, size_t xlen, const unsigned char *y, size_t ylen, int curve)
{
	/* no ECDSA on curve25519 (BearSSL's has none either) */
	(void)A; (void)B; (void)len; (void)x; (void)xlen; (void)y; (void)ylen; (void)curve;
	return 0;
}

const br_ec_impl c68k_br_ec_c25519 = {
	(uint32_t)1 << BR_EC_curve25519,
	&c25519_generator,
	&c25519_orderf,
	&c25519_xoff,
	&c25519_mul,
	&c25519_mulgen,
	&c25519_muladd
};

