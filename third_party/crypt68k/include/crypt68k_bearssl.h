/*
 * crypt68k_bearssl.h - crypt68k as BearSSL's signature checkers, in a
 * BearSSL client's setup:
 *
 *	br_ssl_engine_set_rsavrfy(&cc.eng, c68k_br_rsa_pkcs1_vrfy);
 *	br_ssl_engine_set_ecdsa(&cc.eng, c68k_br_ecdsa_vrfy_asn1);
 *	br_x509_minimal_set_rsa(&xc, c68k_br_rsa_pkcs1_vrfy);
 *	br_x509_minimal_set_ecdsa(&xc, &br_ec_all_m31, c68k_br_ecdsa_vrfy_asn1);
 *
 * The x509 ones check certificate chains, the engine's the
 * server's key exchange. NOT br_ssl_engine_set_rsapub: that one encrypts
 * the premaster secret in RSA key exchange suites, and crypt68k's
 * variable time would leak it. ECDSA on curves other than P-256 and
 * P-384 goes on to BearSSL's br_ecdsa_i31 with the impl given.
 *
 * Built only with BearSSL (src/bearssl.c; make BEARSSL=its directory).
 */
#ifndef CRYPT68K_BEARSSL_H
#define CRYPT68K_BEARSSL_H

#include "bearssl.h"

/* a br_rsa_public: for signatures only (see above) */
uint32_t c68k_br_rsa_public(unsigned char *x, size_t xlen, const br_rsa_public_key *pk);

/* a br_rsa_pkcs1_vrfy */
uint32_t c68k_br_rsa_pkcs1_vrfy(const unsigned char *x, size_t xlen,
	const unsigned char *hash_oid, size_t hash_len,
	const br_rsa_public_key *pk, unsigned char *hash_out);

/* X25519 as a br_ec_impl, like br_ec_c25519_m31 (constant time), for the
 * key exchange: br_ssl_engine_set_ec(&cc.eng, &c68k_br_ec_c25519) when
 * the client offers only X25519, or behind a br_ec_impl that hands
 * curve25519 to it and the other curves to br_ec_all_m31. */
extern const br_ec_impl c68k_br_ec_c25519;

/* br_ecdsa_vrfy's: ASN.1 (DER) and raw signatures */
uint32_t c68k_br_ecdsa_vrfy_asn1(const br_ec_impl *impl, const void *hash, size_t hash_len,
	const br_ec_public_key *pk, const void *sig, size_t sig_len);
uint32_t c68k_br_ecdsa_vrfy_raw(const br_ec_impl *impl, const void *hash, size_t hash_len,
	const br_ec_public_key *pk, const void *sig, size_t sig_len);

#endif /* CRYPT68K_BEARSSL_H */
