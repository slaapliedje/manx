/*
 * imap.h - an IMAP4rev1 client (RFC 3501), enough to read mail: one
 * connection, TLS unless the server is this machine, a command at a time,
 * each response parsed into values.
 *
 * A response's values live in the client until the next response: atoms
 * (numbers, flags, BODY[1.2]), strings (quoted or literal: binary-safe,
 * not NUL-terminated), NIL, and lists, whose values are linked in order.
 */
#ifndef MANX_IMAP_H
#define MANX_IMAP_H

#include <stddef.h>
#include <stdarg.h>
#include "conn.h"

enum { IV_NIL, IV_ATOM, IV_STR, IV_LIST };

struct ival {
	int type;
	const char *s;			/* IV_ATOM, IV_STR: len bytes */
	unsigned long len;
	long first;			/* IV_LIST: its first value (index), -1 */
	long next;			/* the value after this one, -1 */
	unsigned long n;		/* IV_LIST: how many values */
};

struct imap {
	struct conn *c;
	unsigned tag;
	unsigned char rbuf[4096];	/* read from the connection, not yet used */
	size_t rpos, rlen;
	char *buf;			/* the response being read */
	size_t len, cap;
	struct ival *iv;		/* its values */
	unsigned long niv, ivcap;
	char caps[512];			/* what the server can do */
	char err[256];			/* why the last thing failed */
	/* the mailbox selected */
	unsigned long exists, uidvalidity, uidnext;
};

/* Connect to host:port (TLS, validated before anything is sent, unless
 * tls is 0, which only this machine is allowed) and read the greeting.
 * 0, or -1 with m->err. */
int imap_open(struct imap *m, const char *host, unsigned port, int tls);

/* Log in: AUTHENTICATE PLAIN (in the command where the server allows),
 * else LOGIN. The password is never written anywhere but the socket. */
int imap_auth(struct imap *m, const char *user, const char *pass);

/* LOGOUT and close. */
void imap_close(struct imap *m);

/* An untagged response: its values after the "*", count of them. */
typedef void (*imap_cb)(void *ctx, struct imap *m, struct ival *v, unsigned long n);

/* Run a command (printf-style, without tag or CRLF); each untagged
 * response through cb (may be NULL). 0 on OK, -1 (m->err) otherwise.
 * A connection that failed is closed: m->c is NULL then. */
int imap_cmd(struct imap *m, imap_cb cb, void *ctx, const char *fmt, ...);
int imap_vcmd(struct imap *m, imap_cb cb, void *ctx, const char *fmt, va_list ap);

/* SELECT (or EXAMINE, readonly) a mailbox: m->exists, uidvalidity, uidnext. */
int imap_select(struct imap *m, const char *mbox, int readonly);

/* A string as an IMAP quoted string, into out (n bytes): 0, or -1 when
 * it can't be one (CR, LF, NUL; too long). */
int imap_quote(char *out, size_t n, const char *s);

/* The values of s..e (a response's words; quoted strings are unescaped
 * where they lie), linked: the first's index into m->iv (-1: none), *n of
 * them. (For tests, and what reads responses.) */
long imap_parse(struct imap *m, char *s, char *e, unsigned long *n);

/* --- values ---------------------------------------------------------- */

/* The i-th value of list v (0-based), or NULL. */
struct ival *iv_at(struct imap *m, const struct ival *v, unsigned long i);

/* The value after v, or NULL. */
struct ival *iv_next(struct imap *m, const struct ival *v);

/* Is v the atom w (any case)? */
int iv_is(const struct ival *v, const char *w);

/* A number's value (0 if v isn't one). */
unsigned long iv_num(const struct ival *v);

/* v (atom or string) as a NUL-terminated copy in out (n bytes); "" for
 * NIL or a list. Returns out. */
char *iv_str(const struct ival *v, char *out, size_t n);

#endif /* MANX_IMAP_H */
