/*
 * test_tpjob - the T425's requests (tls/tpjob.c) on the host: RSA and EC
 * jobs give what sigmath gives directly; pings come back; bad lengths and
 * types are refused as bad, without reading past the payload.
 */
#include <stdio.h>
#include <string.h>
#include "sigmath.h"
#include "tpproto.h"

static int fails, runs;
static unsigned g_rng = 1234567u;

static unsigned rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

static void check(const char *what, int ok)
{
	runs++;
	if (!ok) {
		fails++;
		printf("FAIL %s\n", what);
	}
}

/* P-256's G, a point to use as Q */
static const unsigned char g256[65] = { 0x04,
	0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5,
	0x63, 0xa4, 0x40, 0xf2, 0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0,
	0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
	0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a,
	0x7c, 0x0f, 0x9e, 0x16, 0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce,
	0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5 };

int main(void)
{
	static unsigned char in[TP_MAX_PAYLOAD + 64], out[600], want[600];
	size_t outlen, i;
	int st;

	/* ping */
	for (i = 0; i < 100; i++)
		in[i] = (unsigned char)rnd();
	st = tpjob_run(TP_PING, in, 100, out, &outlen);
	check("ping", st == TP_DONE && outlen == 100 && memcmp(in, out, 100) == 0);
	check("ping too long", tpjob_run(TP_PING, in, 513, out, &outlen) == TP_BAD);

	/* RSA: a random odd 2048-bit n, e = 65537, x < n */
	{
		unsigned char n[256], x[256];
		static const unsigned char e[3] = { 1, 0, 1 };
		size_t p = 0;

		for (i = 0; i < 256; i++)
			n[i] = (unsigned char)rnd();
		n[0] |= 0x80;
		n[255] |= 1;
		for (i = 0; i < 256; i++)
			x[i] = (unsigned char)rnd();
		x[0] &= 0x7f;
		in[p++] = 0;
		in[p++] = 1;		/* 256 */
		memcpy(in + p, n, 256);
		p += 256;
		in[p++] = 3;
		in[p++] = 0;
		memcpy(in + p, e, 3);
		p += 3;
		memcpy(in + p, x, 256);
		p += 256;
		memcpy(want, x, 256);
		check("RSA directly", sig_rsa(want, 256, n, 256, e, 3) == 1);
		st = tpjob_run(TP_RSA, in, p, out, &outlen);
		check("RSA job", st == TP_DONE && outlen == 256 && memcmp(out, want, 256) == 0);
		/* x >= n: refused; lengths that don't add up: bad */
		memset(in + p - 256, 0xff, 256);
		check("RSA x >= n", tpjob_run(TP_RSA, in, p, out, &outlen) == TP_REFUSED);
		check("RSA short", tpjob_run(TP_RSA, in, 3, out, &outlen) == TP_BAD);
		in[258] = 0xff;
		in[259] = 0xff;		/* elen past the end */
		check("RSA elen", tpjob_run(TP_RSA, in, p, out, &outlen) == TP_BAD);
	}

	/* EC: u1 G + u2 G on P-256, against sigmath directly */
	{
		unsigned char u1[32], u2[32];
		size_t p = 0;

		for (i = 0; i < 32; i++) {
			u1[i] = (unsigned char)rnd();
			u2[i] = (unsigned char)rnd();
		}
		u1[0] &= 0x7f;
		u2[0] &= 0x7f;
		in[p++] = SIG_P256;
		memcpy(in + p, g256, 65);
		p += 65;
		memcpy(in + p, u1, 32);
		p += 32;
		memcpy(in + p, u2, 32);
		p += 32;
		check("EC directly", sig_ec_muladd(SIG_P256, want, g256, u1, u2) == 1);
		st = tpjob_run(TP_EC, in, p, out, &outlen);
		check("EC job", st == TP_DONE && outlen == 32 && memcmp(out, want, 32) == 0);
		check("EC wrong length", tpjob_run(TP_EC, in, p - 1, out, &outlen) == TP_BAD);
		in[0] = 99;
		check("EC unknown curve", tpjob_run(TP_EC, in, p, out, &outlen) == TP_BAD);
		in[0] = SIG_P256;
		in[1 + 64] ^= 1;	/* Q off the curve */
		check("EC Q off the curve", tpjob_run(TP_EC, in, p, out, &outlen) == TP_REFUSED);
	}
	check("unknown type", tpjob_run(77, in, 10, out, &outlen) == TP_BAD);

	/* random requests: no crash, no read past the payload (ASan builds) */
	for (i = 0; i < 2000; i++) {
		size_t len = rnd() % 600, j;

		for (j = 0; j < len; j++)
			in[j] = (unsigned char)rnd();
		if (rnd() & 1)
			in[0] = (unsigned char)(rnd() & 1 ? SIG_P256 : SIG_P384);
		tpjob_run(1 + (int)(rnd() % 3), in, len, out, &outlen);
	}
	printf("tpjob: %d/%d passed\n", runs - fails, runs);
	return fails ? 1 : 0;
}
