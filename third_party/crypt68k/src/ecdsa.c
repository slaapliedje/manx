/*
 * ecdsa.c - ECDSA signatures checked (crypt68k.h): the steps of
 * sigmath.c in order, and the DER form of a signature read. Part of
 * crypt68k, MIT licence (LICENSE).
 */
#include <string.h>
#include "crypt68k.h"

int c68k_ecdsa_verify_raw(int curve, const unsigned char *q, size_t qlen,
	const void *hash, size_t hash_len, const void *sig, size_t sig_len)
{
	const unsigned char *s = sig;
	unsigned char u1[48], u2[48], x[48];
	size_t len = c68k_ec_len(curve), rlen;

	if (len == 0 || sig_len == 0 || (sig_len & 1) || qlen != 1 + 2 * len)
		return 0;
	rlen = sig_len / 2;
	return c68k_ec_point(curve, q, qlen)
		&& c68k_ecdsa_scalars(curve, u1, u2, s, s + rlen, rlen, hash, hash_len)
		&& c68k_ec_muladd(curve, x, q, u1, u2)
		&& c68k_ec_x_is_r(curve, x, s, rlen);
}

/* an INTEGER at *p, right-aligned into out (len bytes): 0 if it isn't
 * one, is negative, or doesn't fit */
static int der_int(const unsigned char **p, const unsigned char *end,
	unsigned char *out, size_t len)
{
	const unsigned char *b = *p;
	size_t n;

	if (end - b < 2 || b[0] != 0x02 || b[1] > 0x7f || (size_t)(end - b) < 2 + (size_t)b[1])
		return 0;
	n = b[1];
	b += 2;
	*p = b + n;
	if (n == 0 || (b[0] & 0x80))
		return 0;
	while (n > 0 && *b == 0) {
		b++;
		n--;
	}
	if (n > len)
		return 0;
	memset(out, 0, len - n);
	memcpy(out + len - n, b, n);
	return 1;
}

int c68k_ecdsa_verify_asn1(int curve, const unsigned char *q, size_t qlen,
	const void *hash, size_t hash_len, const void *sig, size_t sig_len)
{
	const unsigned char *p = sig, *end = p + sig_len;
	unsigned char raw[96];
	size_t len = c68k_ec_len(curve), n;

	/* SEQUENCE { INTEGER r, INTEGER s }, nothing after it */
	if (len == 0 || sig_len < 2 || p[0] != 0x30)
		return 0;
	if (p[1] < 0x80) {
		n = p[1];
		p += 2;
	} else if (p[1] == 0x81 && sig_len >= 3 && p[2] >= 0x80) {
		n = p[2];
		p += 3;
	} else
		return 0;
	if ((size_t)(end - p) != n || !der_int(&p, end, raw, len)
		|| !der_int(&p, end, raw + len, len) || p != end)
		return 0;
	return c68k_ecdsa_verify_raw(curve, q, qlen, hash, hash_len, raw, 2 * len);
}
