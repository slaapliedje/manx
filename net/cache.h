/*
 * cache.h - pages kept on disk ($UB_HOME/cache), so that going back
 * needs no network (on a 68030 a TLS handshake costs seconds) and a page
 * still fresh isn't fetched again. One file per URL: a small text head,
 * then the body as it was shown (decoded).
 */
#ifndef UB_CACHE_H
#define UB_CACHE_H

#include <stddef.h>

struct cache_meta {
	char url[2048];			/* the key */
	char location[2048];		/* where the page is (after redirects) */
	char type[128];			/* content type */
	char charset[32];
	long stored;			/* when (seconds since 1970) */
	long fresh_until;		/* 0: must be checked with the server */
	char etag[128];			/* validators for a conditional request */
	char last_modified[64];
	long size;			/* body bytes */
};

/* dir: where (created); max_bytes: the most the files may take. */
void cache_init(const char *dir, long max_bytes);

/* Is key (a URL, or any other name) there? Fills m. */
int cache_lookup(const char *key, struct cache_meta *m);

/* Read key's body, in pieces, to body() (which returns -1 to stop).
 * 0, or -1 (missing, unreadable). */
int cache_read(const char *key, struct cache_meta *m,
	int (*body)(void *ctx, const unsigned char *d, size_t n), void *ctx);

/* Writing: begin, write the body as it comes, then commit with the meta
 * (or abort). Only one write at a time. */
int cache_begin(const char *key);
void cache_write(const unsigned char *d, size_t n);
void cache_commit(const struct cache_meta *m);
void cache_abort(void);

/* Forget key. */
void cache_remove(const char *key);

/* From a response's Cache-Control / Expires / Date headers: until when
 * it's fresh (0: not at all), and whether it may be stored. now: the
 * time here. */
long cache_freshness(const char *cache_control, const char *expires,
	const char *date, long now, int *no_store);

#endif /* UB_CACHE_H */
