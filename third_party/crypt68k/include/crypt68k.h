/*
 * crypt68k - fast signature checks for 68k machines: RSA's public
 * operation and ECDSA on P-256 and P-384, with Montgomery multiplication
 * on 32-bit limbs whose inner loop is 68020/030/040 assembly around
 * mulu.l (32x32 -> 64). Portable C89 everywhere else.
 *
 * On a 32 MHz 68030 (an Atari TT): RSA-2048 0.62 s, RSA-4096 2.3 s,
 * P-256 1.9 s, P-384 5.6 s; BearSSL's fastest code there takes 2.3 s,
 * 9.6 s, 5.5 s and 42 s.
 *
 * And X25519 (RFC 7748), the key exchange most TLS servers pick.
 *
 * TIME. The signature checks take a time that depends on their inputs.
 * That is fine for checking signatures, whose inputs (the signature, the
 * public key, the hash) are all public; it is NOT fine for anything
 * secret: never use them to sign, to decrypt, or to encrypt a secret
 * with RSA. X25519 is built the other way: no branch or memory access
 * in it depends on the scalar or the point. But it can only be as
 * constant-time as the CPU's multiply: on a 68030, mulu takes about 2
 * cycles longer when its source operand's low bit is 1 (measured; so
 * does BearSSL's X25519 there). Use it for ephemeral keys, as a TLS
 * client does, not long-lived ones. See README.
 *
 * Numbers are big-endian byte strings. MIT licence (LICENSE).
 */
#ifndef CRYPT68K_H
#define CRYPT68K_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- RSA ------------------------------------------------------------------ */

/*
 * x = x^e mod n, in place: the public operation that checking an RSA
 * signature starts with (then the PKCS#1 padding is checked against the
 * hash). xlen must be n's length without its leading zeros. 1 if done;
 * 0 if n is even, 1 or over 4096 bits, x >= n, or the lengths don't agree.
 */
int c68k_rsa_public(unsigned char *x, size_t xlen, const unsigned char *n, size_t nlen,
	const unsigned char *e, size_t elen);

/* --- ECDSA ---------------------------------------------------------------- */

/* the curves, by TLS's (and BearSSL's) numbers */
#define C68K_P256	23	/* secp256r1 */
#define C68K_P384	24	/* secp384r1 */

/*
 * Is sig a valid signature of hash by the public key q on curve? 1 or 0.
 * q: uncompressed (0x04, x, y). hash: as long as it is (its leftmost
 * bits are used, as ECDSA says). raw: r and s, each half of sig_len;
 * asn1: the DER SEQUENCE of two INTEGERs that X.509 and TLS use.
 */
int c68k_ecdsa_verify_raw(int curve, const unsigned char *q, size_t qlen,
	const void *hash, size_t hash_len, const void *sig, size_t sig_len);
int c68k_ecdsa_verify_asn1(int curve, const unsigned char *q, size_t qlen,
	const void *hash, size_t hash_len, const void *sig, size_t sig_len);

/*
 * The same in steps, for doing the heavy one (c68k_ec_muladd) somewhere
 * else, or ahead of time: Manx, the browser crypt68k comes from, hands it
 * to the transputer on an Atari TT's ATW800/2 card.
 */

/* The bytes of a coordinate or scalar on curve (32, 48); 0 for others. */
size_t c68k_ec_len(int curve);

/* Is q (0x04, x, y) a point of the curve? */
int c68k_ec_point(int curve, const unsigned char *q, size_t qlen);

/* u1 = h/s, u2 = r/s mod n (len bytes each) from r and s (rlen bytes
 * each) and the hash. 0 if r or s is not in 1..n-1. */
int c68k_ecdsa_scalars(int curve, unsigned char *u1, unsigned char *u2,
	const unsigned char *r, const unsigned char *s, size_t rlen,
	const unsigned char *hash, size_t hash_len);

/* x (len bytes) = the x coordinate of u1 G + u2 Q (q checked again
 * here, u1 and u2 below n). 0 if the sum is the point at infinity. */
int c68k_ec_muladd(int curve, unsigned char *x, const unsigned char *q,
	const unsigned char *u1, const unsigned char *u2);

/* x mod n = r? */
int c68k_ec_x_is_r(int curve, const unsigned char *x, const unsigned char *r, size_t rlen);

/* --- X25519 (constant time) ------------------------------------------------ */

/*
 * out = X25519(scalar, u) of RFC 7748: everything little-endian, 32
 * bytes; the scalar clamped and u's top bit ignored, as the RFC says. An
 * all-zero out means u was a point of small order; TLS's key exchange
 * must refuse it (RFC 7748, section 6.1).
 */
void c68k_x25519(unsigned char *out, const unsigned char *scalar, const unsigned char *u);

/* out = X25519(scalar, 9): the public key of a secret scalar */
void c68k_x25519_base(unsigned char *out, const unsigned char *scalar);

#ifdef __cplusplus
}
#endif

#endif /* CRYPT68K_H */
