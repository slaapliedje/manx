/*
 * xdefer.h - finish the TLS handshake first, validate afterwards.
 *
 * On a 68030 checking a certificate chain takes 15-100 s, and servers
 * abandon a handshake long before that. The deferred validator records the
 * server's chain and ServerKeyExchange signature during the handshake and
 * gives BearSSL the leaf key at once, so only the key exchange itself
 * (~2 s) happens while the server waits. xdefer_verify() then runs the
 * real checks. NOTHING may be sent on, or accepted from, the connection
 * until xdefer_verify() returns 0.
 */
#ifndef UB_XDEFER_H
#define UB_XDEFER_H

#include "bearssl.h"

#define XDEFER_CHAIN_MAX	16384
#define XDEFER_CERTS_MAX	8

struct xdefer {
	const br_x509_class *vtable;	/* first: BearSSL's object pointer */
	char server_name[256];
	unsigned char chain[XDEFER_CHAIN_MAX];
	size_t len;
	size_t cert_off[XDEFER_CERTS_MAX], cert_len[XDEFER_CERTS_MAX];
	int ncert;
	int err;			/* BR_ERR_* while recording */
	br_x509_decoder_context dec;	/* the leaf's key, no signature checks */
	br_x509_pkey leaf;
	unsigned char key[520];		/* the leaf key's bytes */
	/* the ServerKeyExchange signature, checked later */
	int ske_seen, ske_rsa;
	const unsigned char *ske_oid;
	unsigned char ske_hv[64];
	size_t ske_hvlen;
	unsigned char ske_sig[512];
	size_t ske_siglen;
};

/* Install on a client context set up by br_ssl_client_init_full(); call
 * before br_ssl_client_reset(). */
void xdefer_install(struct xdefer *xd, br_ssl_client_context *sc);

/* After the handshake: validate the recorded chain with the configured
 * x509_minimal context and check the recorded signature. 0 when the
 * server is who it claims to be, otherwise a BR_ERR_* code. */
int xdefer_verify(struct xdefer *xd, br_ssl_client_context *sc,
	br_x509_minimal_context *xc);

#endif /* UB_XDEFER_H */
