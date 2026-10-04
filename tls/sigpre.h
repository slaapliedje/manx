/*
 * sigpre.h - a certificate chain's signature arithmetic done ahead, on
 * two processors. Before BearSSL replays a recorded chain (xdefer.c),
 * the checks it is going to make are worked out from the certificates
 * (each one's signature against the next one's key, or a trust anchor's
 * of the same name; the key exchange's against the leaf's), and their
 * arithmetic (RSA's x^e mod n, ECDSA's u1 G + u2 Q) is shared between
 * the ATW800/2's T425 (tpoff.h) and the 68030. BearSSL's own checks
 * then find the answers waiting (rsavrfy.c and ecvrfy.c look here
 * first) and do every check themselves with them. A wrong guess costs
 * only its work: what isn't here is worked out as before.
 */
#ifndef MANX_SIGPRE_H
#define MANX_SIGPRE_H

#include "bearssl.h"

/* Work out and do the checks of a chain (certs[0] the leaf) and of the
 * key exchange's signature (ske_rsa: RSA PKCS#1, else ECDSA in ASN.1;
 * ske_hv the hash signed): only when the T425 is up, as the 68030 alone
 * gains nothing by it. leaf: the leaf's key (when ncert is 0, a known
 * leaf, only the key exchange is checked). */
void sigpre_chain(const unsigned char *const *certs, const size_t *lens, int ncert,
	const br_x509_trust_anchor *tas, size_t ntas, const br_x509_pkey *leaf,
	int ske_rsa, const unsigned char *ske_hv, size_t ske_hvlen,
	const unsigned char *ske_sig, size_t ske_siglen);

/* Forget them (after the checks). */
void sigpre_clear(void);

/* An answer done ahead: 1 (x replaced by x^e mod n; or x, len bytes, the
 * x of u1 G + u2 Q), 0 if it was refused, -1 if it isn't here. */
int sigpre_rsa(unsigned char *x, size_t xlen, const unsigned char *n, size_t nlen,
	const unsigned char *e, size_t elen);
int sigpre_ec(int curve, unsigned char *x, const unsigned char *q,
	const unsigned char *u1, const unsigned char *u2);

/* For ufetch -v: the last chain's jobs, those the T425 did, and those
 * BearSSL's checks used. */
void sigpre_stats(int *jobs, int *on_t425, int *used);

#endif /* MANX_SIGPRE_H */
