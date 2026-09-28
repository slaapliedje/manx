/*
 * conn.h - connections to servers: plain TCP or TLS behind one interface,
 * with a small pool of kept-alive connections (on a 68030 a new TLS
 * connection costs seconds, so reusing one matters more than anywhere).
 */
#ifndef UB_CONN_H
#define UB_CONN_H

#include <stddef.h>
#include "tls.h"

struct conn {
	int fd;
	int is_tls;
	struct tls_conn *tls;		/* xmalloc'd when is_tls */
	char host[256];
	unsigned port;
	int reused;			/* came from the pool */
	int reconnected;		/* redone after a long validation */
	int in_pool;
	unsigned long idle_since;
	/* bytes read past the end of a response, for the next one */
	unsigned char pend[4096];
	size_t pend_len;
	/* timings (ms) of the setup, for status lines */
	unsigned long t_dns, t_connect;
};

/*
 * A connection to host:port, from the pool when one is idle there, else
 * new: resolve, connect, and for TLS handshake and validate (climbing the
 * ladder of offers, tls.h). NULL on failure with a message in err.
 *
 * early (TLS only): validation waits for the first read, so a request
 * with nothing private in it can go out at once (see tls_connect). The
 * caller must never send cookies, credentials or form data on an early
 * connection before a read has succeeded.
 */
struct conn *conn_open(const char *host, unsigned port, int is_tls,
	int early, char *err, size_t errlen);

/* Why the connection failed validation (early connections), or NULL. */
const char *conn_error(struct conn *c);

/* >0 bytes, 0 end of stream, -1 error/timeout */
int conn_read(struct conn *c, void *buf, size_t len);

/* Put bytes back, to be read first next time (at most sizeof pend). */
void conn_unread(struct conn *c, const void *buf, size_t len);

int conn_write(struct conn *c, const void *buf, size_t len);	/* 0 / -1 */

/* Done with it: keep it for reuse (reusable) or close it. */
void conn_release(struct conn *c, int reusable);

/* Close every pooled connection. */
void conn_close_all(void);

#endif /* UB_CONN_H */
