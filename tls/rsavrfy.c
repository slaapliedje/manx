/*
 * rsavrfy.c - RSA for checking signatures (rsavrfy.h), on mont.h's
 * Montgomery multiplication. Measured on the TT (docs/phase0-results.md,
 * "Hand-written lmul"): RSA-2048 0.52 s against br_rsa_i32's 2.30 s.
 */
#include <string.h>
#include "mont.h"
#include "rsavrfy.h"

/* in BearSSL (src/rsa/rsa_pkcs1_sig_unpad.c), not in its public headers */
uint32_t br_rsa_pkcs1_sig_unpad(const unsigned char *sig, size_t sig_len,
	const unsigned char *hash_oid, size_t hash_len, unsigned char *hash_out);

uint32_t rsavrfy_public(unsigned char *x, size_t xlen, const br_rsa_public_key *pk)
{
	struct mont m;
	uint32_t r2[MONT_MAXK], xm[MONT_MAXK], acc[MONT_MAXK];
	const unsigned char *n = pk->n, *e = pk->e;
	size_t nlen = pk->nlen, elen = pk->elen;
	int k, i, ebit;

	while (nlen > 0 && *n == 0) {
		n++;
		nlen--;
	}
	if (nlen == 0 || nlen > 4 * MONT_MAXK || xlen != nlen || !(n[nlen - 1] & 1))
		return 0;
	k = (int)((nlen + 3) / 4);
	m.k = k;
	mont_decode(m.n, k, n, nlen);
	mont_decode(xm, k, x, xlen);
	if (mont_cmp(xm, &m) >= 0)
		return 0;
	if (k == 1 && m.n[0] == 1)
		return 0;		/* n = 1: no Montgomery form */

	mont_setup(&m);
	mont_r2(r2, &m);

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
	mont_mul(xm, xm, r2, &m);
	memcpy(acc, xm, (size_t)k * sizeof acc[0]);
	for (ebit = 7; !(e[0] >> ebit & 1); ebit--)
		;
	for (i = 0; i < (int)elen; i++) {
		int b = i == 0 ? ebit - 1 : 7;

		for (; b >= 0; b--) {
			mont_mul(acc, acc, acc, &m);
			if (e[i] >> b & 1)
				mont_mul(acc, acc, xm, &m);
		}
	}
	memset(r2, 0, (size_t)k * sizeof r2[0]);
	r2[0] = 1;
	mont_mul(acc, acc, r2, &m);
	mont_encode(x, xlen, acc);
	return 1;
}

uint32_t rsavrfy_pkcs1(const unsigned char *x, size_t xlen,
	const unsigned char *hash_oid, size_t hash_len,
	const br_rsa_public_key *pk, unsigned char *hash_out)
{
	unsigned char sig[4 * MONT_MAXK];

	if (xlen > sizeof sig)
		return 0;
	memcpy(sig, x, xlen);
	if (!rsavrfy_public(sig, xlen, pk))
		return 0;
	return br_rsa_pkcs1_sig_unpad(sig, xlen, hash_oid, hash_len, hash_out);
}
