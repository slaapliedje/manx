/*
 * smtp.h - sending mail (RFC 5321) to the server that takes the user's
 * mail: TLS from the start (port 465, "implicit TLS"), validated before
 * anything is sent, unless the server is this machine; a login; one
 * message at a time.
 */
#ifndef MANX_SMTP_H
#define MANX_SMTP_H

#include <stddef.h>
#include "conn.h"

struct smtp {
	struct conn *c;
	unsigned char rbuf[1024];	/* read from the connection, not yet used */
	size_t rpos, rlen;
	char line[1024];		/* the reply line being read */
	char said[200];			/* the last reply's first line */
	char ext[1024];			/* what EHLO said it can do, a line each */
	char err[256];			/* why the last thing failed */
};

/* Connect to host:port (tls 0: only this machine), the greeting, EHLO.
 * 0, or -1 with s->err. */
int smtp_open(struct smtp *s, const char *host, unsigned port, int tls);

/* Log in: AUTH PLAIN, else AUTH LOGIN. 0, or -1 with s->err. */
int smtp_auth(struct smtp *s, const char *user, const char *pass);

/* Send msg (len bytes; CRLF lines, not dot-stuffed: that's done here) from
 * the address from to the nrcpt addresses rcpt (bare: a@b). 0, or -1 with
 * s->err. */
int smtp_send(struct smtp *s, const char *from, char **rcpt, int nrcpt,
	const char *msg, size_t len);

/* QUIT and close. */
void smtp_close(struct smtp *s);

/* Can addr go in MAIL FROM or RCPT TO: a@b, printable ASCII, no spaces
 * or angle brackets? */
int smtp_addr_ok(const char *addr);

#endif /* MANX_SMTP_H */
