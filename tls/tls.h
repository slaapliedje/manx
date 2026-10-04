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
 *   - a ladder of offers, cheapest first: FAST is ECDHE-RSA with X25519
 *     only (cheapest to verify and to compute); FULL_X adds ECDSA
 *     certificates, still X25519 only; FULL also offers P-256/P-384 (a
 *     P-256 key exchange costs ~5 s on the TT, which some servers won't
 *     wait for mid-handshake). A host that refused a rung is remembered.
 */
#ifndef MANX_TLS_H
#define MANX_TLS_H

#include <stddef.h>
#include "bearssl.h"
#include "anchors.h"
#include "xdefer.h"

enum { TLS_FAST = 0, TLS_FULL_X = 1, TLS_FULL = 2 };

/* what happened, for the caller's status line and diagnostics */
struct tls_info {
	int profile;
	int resumed;			/* session resumption: no key exchange */
	int leaf_memo;			/* validated leaf remembered: chain skipped */
	int learned;			/* intermediates learned this time */
	unsigned version, suite;
	unsigned long t_handshake, t_verify;	/* ms */
	int pre_jobs, pre_t425, pre_used;	/* signature work done ahead (sigpre.h) */
};

struct tls_conn {
	br_ssl_client_context sc;
	br_x509_minimal_context xc;
	struct xdefer xd;
	br_sslio_context io;
	int fd;
	int open;
	int verify_pending;		/* early request: validate at first read */
	int verify_err;			/* why validation failed */
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
 *
 * early: return right after the handshake, validation still pending.
 * The caller may then send a request that carries nothing private (no
 * cookies, credentials or form data), so the server gets it while the
 * 68030 validates; the first tls_read validates before it returns any
 * byte, and fails (verify_err) if the server isn't trusted. Only the
 * request line and ordinary headers can reach a man in the middle.
 */
int tls_connect(struct tls_conn *c, int fd, const char *host, unsigned port,
	int profile, int early);

/* Run a pending validation now: 0 or a BR_ERR_* code. */
int tls_verify(struct tls_conn *c);

int tls_read(struct tls_conn *c, void *buf, size_t len);	/* >0, 0 eof, -1 */
int tls_write(struct tls_conn *c, const void *buf, size_t len); /* 0 / -1 */
void tls_close(struct tls_conn *c);	/* sends close_notify; not the fd */

/* Should a handshake that failed with err be retried one rung up? */
int tls_retry_full(int err);

/* The rung to start at for a host, and remembering one that worked
 * (in memory and in the data directory) */
int tls_host_profile(const char *host);
void tls_host_set_profile(const char *host, int profile);

/* "fast", "full-x25519", "full" */
const char *tls_profile_name(int profile);

/* A description of a BearSSL/TLS error code */
const char *tls_strerror(int err);

#endif /* MANX_TLS_H */
