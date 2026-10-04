/*
 * tpjob.c - one request of the T425's protocol (tpproto.h) run through
 * sigmath.c: on the T425 (tp/tpsig.c), and in the tests. C89, for icc.
 */
#include <string.h>
#include "sigmath.h"
#include "tpproto.h"

unsigned tp_crc(const unsigned char *b, size_t n, unsigned crc)
{
	size_t i;
	int j;

	for (i = 0; i < n; i++) {
		crc ^= (unsigned)b[i] << 8;
		for (j = 0; j < 8; j++)
			crc = crc & 0x8000 ? (crc << 1 ^ 0x1021) & 0xffff : crc << 1 & 0xffff;
	}
	return crc;
}

static unsigned get16(const unsigned char *b)
{
	return (unsigned)b[0] | (unsigned)b[1] << 8;
}

int tpjob_run(int type, const unsigned char *in, size_t inlen,
	unsigned char *out, size_t *outlen)
{
	*outlen = 0;
	if (type == TP_PING) {
		if (inlen > 512)
			return TP_BAD;
		memcpy(out, in, inlen);
		*outlen = inlen;
		return TP_DONE;
	}
	if (type == TP_RSA) {
		const unsigned char *n, *e, *x;
		size_t nlen, elen;

		if (inlen < 4)
			return TP_BAD;
		nlen = get16(in);
		if (nlen > 512 || 2 + nlen + 2 > inlen)
			return TP_BAD;
		n = in + 2;
		elen = get16(n + nlen);
		e = n + nlen + 2;
		x = e + elen;
		if (elen > 512 || (size_t)(x - in) > inlen)
			return TP_BAD;
		/* x: the rest, n's length without its leading zeros */
		*outlen = inlen - (size_t)(x - in);
		memcpy(out, x, *outlen);
		if (!sig_rsa(out, *outlen, n, nlen, e, elen)) {
			*outlen = 0;
			return TP_REFUSED;
		}
		return TP_DONE;
	}
	if (type == TP_EC) {
		int curve;
		size_t len;

		if (inlen < 1)
			return TP_BAD;
		curve = in[0];
		len = sig_ec_len(curve);
		if (len == 0 || inlen != 1 + (1 + 2 * len) + 2 * len)
			return TP_BAD;
		if (!sig_ec_muladd(curve, out, in + 1, in + 2 + 2 * len, in + 2 + 3 * len))
			return TP_REFUSED;
		*outlen = len;
		return TP_DONE;
	}
	return TP_BAD;
}
