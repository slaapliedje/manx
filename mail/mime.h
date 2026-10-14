/*
 * mime.h - the pieces of mail: base64 and quoted-printable, RFC 2047's
 * encoded words in headers, IMAP's modified UTF-7 mailbox names, charsets
 * into UTF-8, and a message's parts from its IMAP BODYSTRUCTURE (so only
 * the part shown is fetched).
 */
#ifndef MANX_MIME_H
#define MANX_MIME_H

#include <stddef.h>

/* base64 of n bytes into out (4 * ((n + 2) / 3) + 1 bytes); its length */
size_t b64_encode(const unsigned char *in, size_t n, char *out);

/* base64 text (whitespace and the like skipped) into out (n * 3 / 4 + 3
 * bytes); the bytes written */
size_t b64_decode(const char *in, size_t n, unsigned char *out);

/* quoted-printable into out (n bytes is enough); header: '_' is a space
 * (RFC 2047's Q); the bytes written */
size_t qp_decode(const char *in, size_t n, unsigned char *out, int header);

/* n bytes in charset cs (a label; NULL or unknown: windows-1252, which
 * ASCII is part of) as UTF-8, in a new string (NULL: no memory) */
char *mime_utf8(const char *cs, const unsigned char *s, size_t n);

/* A header's text, folded lines and =?charset?B|Q?...?= words undone, as
 * UTF-8 into out (outn bytes). */
void mime_header(const char *in, size_t n, char *out, size_t outn);

/* An IMAP mailbox name (modified UTF-7) as UTF-8 into out. */
void mime_mutf7(const char *in, char *out, size_t n);

/* One part of a message, from its BODYSTRUCTURE. */
struct mpart {
	char section[24];		/* for BODY[...]: "1", "2.1" */
	char type[24], subtype[32];	/* lower case */
	char charset[32];
	char encoding[24];		/* lower case: base64, quoted-printable... */
	char name[128];			/* its file name, if it says (UTF-8) */
	unsigned long size;		/* as sent (encoded) */
	int attachment;			/* not for showing in the text */
};

struct imap;
struct ival;

/* The parts of BODYSTRUCTURE bs, in order, into out (max): how many. */
int mime_parts(struct imap *m, struct ival *bs, struct mpart *out, int max);

/* The part to show: a text/plain one (html: text/html first), else the
 * other; -1 when there is neither. */
int mime_pick(const struct mpart *p, int n, int html);

/* A part's bytes as fetched, its transfer encoding undone, in a new
 * buffer (*len bytes, NUL after them); NULL: no memory. */
unsigned char *mime_decode(const struct mpart *p, const char *s, size_t n,
	size_t *len);

#endif /* MANX_MIME_H */
