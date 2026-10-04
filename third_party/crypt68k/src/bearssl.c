/*
 * bearssl.c - crypt68k as BearSSL's signature checkers
 * (crypt68k_bearssl.h). Part of crypt68k, MIT licence (LICENSE).
 */
#include <string.h>
#include "crypt68k.h"
#include "crypt68k_bearssl.h"

/* in BearSSL (src/rsa/rsa_pkcs1_sig_unpad.c), not in its public headers */
uint32_t br_rsa_pkcs1_sig_unpad(const unsigned char *sig, size_t sig_len,
	const unsigned char *hash_oid, size_t hash_len, unsigned char *hash_out);

uint32_t c68k_br_rsa_public(unsigned char *x, size_t xlen, const br_rsa_public_key *pk)
{
	return (uint32_t)c68k_rsa_public(x, xlen, pk->n, pk->nlen, pk->e, pk->elen);
}

uint32_t c68k_br_rsa_pkcs1_vrfy(const unsigned char *x, size_t xlen,
	const unsigned char *hash_oid, size_t hash_len,
	const br_rsa_public_key *pk, unsigned char *hash_out)
{
	unsigned char sig[512];

	if (xlen > sizeof sig)
		return 0;
	memcpy(sig, x, xlen);
	if (!c68k_rsa_public(sig, xlen, pk->n, pk->nlen, pk->e, pk->elen))
		return 0;
	return br_rsa_pkcs1_sig_unpad(sig, xlen, hash_oid, hash_len, hash_out);
}

uint32_t c68k_br_ecdsa_vrfy_raw(const br_ec_impl *impl, const void *hash, size_t hash_len,
	const br_ec_public_key *pk, const void *sig, size_t sig_len)
{
	if (c68k_ec_len(pk->curve) == 0)
		return br_ecdsa_i31_vrfy_raw(impl, hash, hash_len, pk, sig, sig_len);
	return (uint32_t)c68k_ecdsa_verify_raw(pk->curve, pk->q, pk->qlen,
		hash, hash_len, sig, sig_len);
}

uint32_t c68k_br_ecdsa_vrfy_asn1(const br_ec_impl *impl, const void *hash, size_t hash_len,
	const br_ec_public_key *pk, const void *sig, size_t sig_len)
{
	/* BearSSL's reading of the DER, so both accept the same */
	unsigned char rsig[(66 << 2) + 24];

	if (sig_len > sizeof rsig / 2)
		return 0;
	memcpy(rsig, sig, sig_len);
	sig_len = br_ecdsa_asn1_to_raw(rsig, sig_len);
	return c68k_br_ecdsa_vrfy_raw(impl, hash, hash_len, pk, rsig, sig_len);
}
