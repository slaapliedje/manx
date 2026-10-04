/*
 * rsavrfy.c - RSA for checking signatures (rsavrfy.h): BearSSL's side of
 * sigmath.c's sig_rsa. Measured on the TT (docs/phase0-results.md,
 * "Hand-written lmul"): RSA-2048 0.52 s against br_rsa_i32's 2.30 s.
 */
#include <string.h>
#include "sigmath.h"
#include "sigpre.h"
#include "rsavrfy.h"

/* in BearSSL (src/rsa/rsa_pkcs1_sig_unpad.c), not in its public headers */
uint32_t br_rsa_pkcs1_sig_unpad(const unsigned char *sig, size_t sig_len,
	const unsigned char *hash_oid, size_t hash_len, unsigned char *hash_out);

uint32_t rsavrfy_public(unsigned char *x, size_t xlen, const br_rsa_public_key *pk)
{
	/* done ahead (on the T425, say)? */
	int r = sigpre_rsa(x, xlen, pk->n, pk->nlen, pk->e, pk->elen);

	if (r >= 0)
		return (uint32_t)r;
	return (uint32_t)sig_rsa(x, xlen, pk->n, pk->nlen, pk->e, pk->elen);
}

uint32_t rsavrfy_pkcs1(const unsigned char *x, size_t xlen,
	const unsigned char *hash_oid, size_t hash_len,
	const br_rsa_public_key *pk, unsigned char *hash_out)
{
	unsigned char sig[512];

	if (xlen > sizeof sig)
		return 0;
	memcpy(sig, x, xlen);
	if (!rsavrfy_public(sig, xlen, pk))
		return 0;
	return br_rsa_pkcs1_sig_unpad(sig, xlen, hash_oid, hash_len, hash_out);
}
