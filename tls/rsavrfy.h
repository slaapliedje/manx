/*
 * rsavrfy.h - RSA for checking signatures, about five times BearSSL's
 * br_rsa_i32 on a 68030: a public exponent's square-and-multiply (18
 * Montgomery multiplications for e = 65537, where br_i32_modpow does 48)
 * over 32-bit limbs, the inner loop in 68030 assembly around mulu.l.
 *
 * Variable time: for public inputs only (signatures, public keys), never
 * for encrypting a secret with RSA. Manx offers only ECDHE suites, so RSA
 * is only ever used to check signatures.
 */
#ifndef MANX_RSAVRFY_H
#define MANX_RSAVRFY_H

#include "bearssl.h"

/* A br_rsa_public: x (xlen bytes, the modulus's length) = x^e mod n.
 * 1 if done, 0 if the key or x can't be used (x >= n, an even modulus,
 * one over 4096 bits, xlen not the modulus's length). */
uint32_t rsavrfy_public(unsigned char *x, size_t xlen, const br_rsa_public_key *pk);

/* A br_rsa_pkcs1_vrfy, as br_rsa_i32_pkcs1_vrfy but on rsavrfy_public. */
uint32_t rsavrfy_pkcs1(const unsigned char *x, size_t xlen,
	const unsigned char *hash_oid, size_t hash_len,
	const br_rsa_public_key *pk, unsigned char *hash_out);

#endif /* MANX_RSAVRFY_H */
