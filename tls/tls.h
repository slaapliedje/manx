/*
 * tls.h - TLS 1.2 client connections, shaped by what a 68030 can do (see
 * docs/phase0-results.md):
 *
 *   - validation is deferred: the handshake finishes first (servers give
 *     up after 10-20 s), then the chain and the server's signature are
 *     checked before any application data moves
 *   - intermediates this machine has verified are kept as trust anchors,
 *     and leaf certificates it has validated for a host are remembered
 *     until they expire: a repeat visit checks one signature, not three
 *   - sessions are resumed when the server allows: no public-key work
 *   - two profiles: FAST offers ECDHE-RSA with X25519 only (cheapest to
 *     verify and to compute); FULL offers everything, for servers that
 *     refuse FAST
 */
#ifndef UB_TLS_H
#define UB_TLS_H

#include <stddef.h>
#include "bearssl.h"
#include "anchors.h"
#include "xdefer.h"

enum { TLS_FAST = 0, TLS_FULL = 1 };

/* what happened, for the caller's status line and diagnostics */
struct tls_info {
	int profile;
	int resumed;			/* session resumption: no key exchange */
	int leaf_memo;			/* validated leaf remembered: chain skipped */
	int learned;			/* intermediates learned this time */
	unsigned version, suite;
	unsigned long t_handshake, t_verify;	/* ms */
};

struct tls_conn {
	br_ssl_client_context sc;
	br_x509_minimal_context xc;
	struct xdefer xd;
	br_sslio_context io;
	int fd;
	int open;
	char host[256];
	unsigned port;
	struct tls_info info;
	unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

/* errors beyond BearSSL's BR_ERR_* codes (which are < 1000) */
enum {
	TLS_ERR_NOT_SEEDED = 1001,	/* entropy pool not ready */
	TLS_ERR_NO_ANCHORS = 1002
};

/*
 * Load the trust material: roots.bin (rebuilt from pem_path when missing
 * or stale; that takes a while on a 68030, so progress is reported via
 * note(), which may be NULL), learned intermediates, validated leaves and
 * sessions from the data directory. 0, or -1 with no anchors at all.
 */
int tls_init(const char *pem_path, void (*note)(const char *msg));

/* Rebuild roots.bin from a PEM bundle now; returns the anchor count, or
 * -1. */
int tls_build_roots(const char *pem_path);

/* Verify every intermediate of a PEM bundle against the current anchors
 * and learn the good ones (for preloading). Returns the number learned. */
int tls_learn_pem(const char *pem_path, void (*note)(const char *msg));

/*
 * Handshake on a connected socket and validate the server. 0, or a
 * BR_ERR_* / TLS_ERR_* code; on failure nothing has been sent or accepted.
 * The connection object is large (~35 KB): don't put it on the stack.
 */
int tls_connect(struct tls_conn *c, int fd, const char *host, unsigned port,
	int profile);

int tls_read(struct tls_conn *c, void *buf, size_t len);	/* >0, 0 eof, -1 */
int tls_write(struct tls_conn *c, const void *buf, size_t len); /* 0 / -1 */
void tls_close(struct tls_conn *c);	/* sends close_notify; not the fd */

/* Should a failed FAST handshake be retried with FULL? */
int tls_retry_full(int err);

/* Remember that a host needs FULL (in memory and in the data directory) */
void tls_host_needs_full(const char *host);
int tls_host_profile(const char *host);

/* A description of a BearSSL/TLS error code */
const char *tls_strerror(int err);

#endif /* UB_TLS_H */
