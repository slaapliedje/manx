/*
 * compose.h - a message to send, put together: its headers (names and
 * subjects outside ASCII as RFC 2047 words), its text (quoted-printable
 * when it isn't 7-bit), address lists as people type them.
 */
#ifndef MANX_COMPOSE_H
#define MANX_COMPOSE_H

#include <stddef.h>

/* growing text */
struct sbuf {
	char *s;
	size_t len, cap;
	int oom;			/* (something didn't fit in memory) */
};

void sb_put(struct sbuf *b, const char *s, size_t n);
void sb_str(struct sbuf *b, const char *s);

#define ADDR_MAX	256

/* One address of a list as typed ("Ann Lee <ann@x.org>, \"Doe, J\" <j@y>,
 * bob@z"): its display name (UTF-8, unquoted; "" for none) and mailbox. */
struct addr {
	char name[160];
	char mbox[ADDR_MAX];
};

/* The addresses of list into out (max): how many, or -1 when one isn't an
 * address (it is copied into bad, badn bytes). */
int addr_parse(const char *list, struct addr *out, int max, char *bad, size_t badn);

struct outmsg {
	const char *from;		/* "Name <a@b>" or a@b */
	const char *to, *cc;		/* lists as typed (cc may be NULL) */
	const char *subject;		/* UTF-8 */
	const char *date;		/* "Wed, 14 Oct 2026 11:30:00 -0600" */
	const char *msgid;		/* "<...>" */
	const char *in_reply_to;	/* "<...>", or NULL */
	const char *references;		/* "<...> <...>", or NULL */
	const char *body;		/* UTF-8, lines ending in \n */
};

/* The message, CRLF lines (not dot-stuffed), *len bytes: NULL when out of
 * memory or an address list is bad (o's lists should be checked with
 * addr_parse first). xfree it. */
char *mail_build(const struct outmsg *o, size_t *len);

/* time t as an RFC 5322 date, in local time with its offset, into out
 * (40 bytes) */
void mail_date(long t, char *out);

#endif /* MANX_COMPOSE_H */
