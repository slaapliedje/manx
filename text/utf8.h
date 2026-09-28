/*
 * utf8.h - character encodings. Documents are held in UTF-8 whatever they
 * arrived in; displays get Latin-1 or ASCII at draw time.
 */
#ifndef UB_UTF8_H
#define UB_UTF8_H

#include <stddef.h>

#define UCS_REPLACEMENT	0xFFFDUL

enum charset { CS_UTF8, CS_WIN1252, CS_UNKNOWN };

/* A charset label ("utf-8", "ISO-8859-1", "latin1", ...) to what we
 * decode: the WHATWG rule makes ISO-8859-1 and US-ASCII mean
 * windows-1252. CS_UNKNOWN for anything else. */
enum charset charset_from_label(const char *label);

/* Encode one code point; returns the length (1-4). Surrogates and values
 * past U+10FFFF become U+FFFD. */
int utf8_put(char *out, unsigned long cp);

/*
 * A streaming decoder to UTF-8: input may break anywhere, even inside a
 * multibyte sequence. Invalid UTF-8 becomes U+FFFD, NUL becomes U+FFFD.
 */
struct decoder {
	enum charset cs;
	/* a UTF-8 sequence in progress (WHATWG decoder state) */
	int need, seen;
	unsigned long cp;
	unsigned char lo, hi;	/* bounds for the next byte (0: 80..BF) */
};

void decoder_init(struct decoder *d, enum charset cs);

/* Decode in[0..n) into out, which must hold 3*n + 8 bytes; returns the
 * bytes written. */
size_t decoder_run(struct decoder *d, const unsigned char *in, size_t n,
	char *out);

/* The input ended: flush an incomplete sequence (as U+FFFD). */
size_t decoder_end(struct decoder *d, char *out);

/* Read one code point from UTF-8 text at *p (advanced); well-formed input
 * assumed (as the decoder produces). */
unsigned long utf8_get(const char **p);

/*
 * The closest rendering of a code point in a limited character set:
 * latin1 != 0 allows U+00A0..U+00FF as themselves. Writes up to 4 bytes
 * (e.g. U+2026 -> "..."), returns the count; 0 means drop it (zero-width
 * characters). Unknown characters become '?'.
 */
int translit(unsigned long cp, int latin1, char *out);

/* The windows-1252 byte for a code point, or -1 when it has none. */
int win1252_byte(unsigned long cp);

/* Terminal cells a code point takes on a UTF-8 terminal: 0 (combining,
 * zero width), 2 (East Asian wide) or 1. */
int ucs_width(unsigned long cp);

#endif /* UB_UTF8_H */
