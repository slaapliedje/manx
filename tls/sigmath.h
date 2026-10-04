/*
 * sigmath.h - the arithmetic of checking signatures, apart from BearSSL
 * (rsavrfy and ecvrfy are the BearSSL side) and in C89, so that the same
 * code runs on the 68030 and, built by icc, on the ATW800/2's T425
 * (tp/tpsig.c): RSA's public operation, and P-256/P-384's u1 G + u2 Q.
 * On mont.h's Montgomery multiplication. Big-endian byte strings.
 *
 * Variable time: for public inputs only (signatures, public keys).
 */
#ifndef MANX_SIGMATH_H
#define MANX_SIGMATH_H

#include <stddef.h>

/* x = x^e mod n, in place. xlen must be n's length without its leading
 * zeros. 1 if done; 0 if n is even, 1 or over 4096 bits, x >= n, or the
 * lengths don't agree. */
int sig_rsa(unsigned char *x, size_t xlen, const unsigned char *n, size_t nlen,
	const unsigned char *e, size_t elen);

/* the curves: BearSSL's numbers (BR_EC_secp256r1, BR_EC_secp384r1) */
#define SIG_P256	23
#define SIG_P384	24

/* The bytes of a coordinate or scalar on curve: 32, 48; 0 for others. */
size_t sig_ec_len(int curve);

/* Is q (0x04, x, y: 1 + 2 len bytes) a point of the curve? */
int sig_ec_point(int curve, const unsigned char *q, size_t qlen);

/* From a signature's r and s (rlen bytes each) and the hash: u1 = h/s and
 * u2 = r/s mod n (len bytes each). 0 if r or s is not in 1..n-1. */
int sig_ec_scalars(int curve, unsigned char *u1, unsigned char *u2,
	const unsigned char *r, const unsigned char *s, size_t rlen,
	const unsigned char *hash, size_t hash_len);

/* x (len bytes) = the x of u1 G + u2 Q, q a point of the curve (checked
 * again here) and u1, u2 below n. 0 if the sum is the point at infinity
 * or q is not on the curve. */
int sig_ec_muladd(int curve, unsigned char *x, const unsigned char *q,
	const unsigned char *u1, const unsigned char *u2);

/* x mod n = r? (x: len bytes, below p; r: rlen bytes) */
int sig_ec_x_is_r(int curve, const unsigned char *x, const unsigned char *r, size_t rlen);

#endif /* MANX_SIGMATH_H */
