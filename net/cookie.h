/*
 * cookie.h - the cookie jar (RFC 6265, as much as a browser without
 * scripts needs). Persistent cookies live in a file (mode 600); session
 * cookies only as long as the browser runs.
 */
#ifndef UB_COOKIE_H
#define UB_COOKIE_H

#include <stddef.h>
#include "url.h"

#define COOKIE_MAX		300	/* cookies kept */
#define COOKIE_BYTES_MAX	(96UL * 1024)	/* their names and values */

/* 0: no cookies are stored or sent */
extern int cookie_enabled;

/* Load persistent cookies from path (kept for cookie_save). */
void cookie_init(const char *path);

/* Write the persistent cookies back (when changed). */
void cookie_save(void);

/* A Set-Cookie header from a response to u. now: seconds since 1970. */
void cookie_set(const struct url *u, const char *header, long now);

/* The value of a Cookie header for a request to u (length; 0 and "" when
 * there are none). */
size_t cookie_header(const struct url *u, long now, char *buf, size_t n);

/* How many are stored; forget them all. */
int cookie_count(void);
void cookie_clear(void);

/* For tests: a cookie date to seconds since 1970, or -1. */
long cookie_parse_date(const char *s);

#endif /* UB_COOKIE_H */
