/*
 * ecvrfy.h - ECDSA signatures checked on P-256 and P-384 with mont.h's
 * Montgomery multiplication (the 68030's mulu.l loop): Jacobian points,
 * u1 G + u2 Q by interleaved width-4 NAF, inverses by binary GCD, and the
 * final x = r check as r Z^2 = X, with no inversion. Other curves go to
 * BearSSL's br_ecdsa_i31.
 *
 * Variable time: for public inputs only (signatures, public keys), which
 * is all ECDSA checking ever sees. Never use it to sign, or for ECDH.
 */
#ifndef MANX_ECVRFY_H
#define MANX_ECVRFY_H

#include "bearssl.h"

/* A br_ecdsa_vrfy for ASN.1 (DER) signatures, as br_ecdsa_i31_vrfy_asn1;
 * impl is used only for the curves this doesn't do itself. */
uint32_t ecvrfy_asn1(const br_ec_impl *impl, const void *hash, size_t hash_len,
	const br_ec_public_key *pk, const void *sig, size_t sig_len);

/* The same for raw signatures (r and s, each half of sig_len). */
uint32_t ecvrfy_raw(const br_ec_impl *impl, const void *hash, size_t hash_len,
	const br_ec_public_key *pk, const void *sig, size_t sig_len);

#endif /* MANX_ECVRFY_H */
