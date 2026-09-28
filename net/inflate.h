/*
 * inflate.h - a streaming DEFLATE decoder (RFC 1951) with the gzip (RFC
 * 1952) and zlib (RFC 1950) wrappers, for Content-Encoding. Input may be
 * cut anywhere; output goes to a callback as it's produced. 33 KB of
 * state (the 32 KB window).
 */
#ifndef UB_INFLATE_H
#define UB_INFLATE_H

#include <stddef.h>

enum {
	INF_GZIP,		/* gzip, x-gzip */
	INF_DEFLATE		/* "deflate": zlib-wrapped, or raw as some send */
};

enum {
	INF_OK = 0,		/* feed more */
	INF_END = 1,		/* the stream is complete (later bytes ignored) */
	INF_BAD = -1,		/* corrupt, or a checksum is wrong */
	INF_STOP = -2		/* out() asked to stop */
};

struct inflate;

/* NULL when out of memory. out returns -1 to stop the decoding. */
struct inflate *inflate_new(int format,
	int (*out)(void *ctx, const unsigned char *data, size_t n), void *ctx);

/* Feed compressed bytes: INF_OK, INF_END, INF_BAD or INF_STOP. */
int inflate_feed(struct inflate *z, const unsigned char *in, size_t n);

/* The input ended: INF_END if the stream was complete, else INF_BAD
 * (truncated). */
int inflate_finish(struct inflate *z);

void inflate_free(struct inflate *z);

#endif /* UB_INFLATE_H */
