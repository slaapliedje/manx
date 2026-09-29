/*
 * url.h - URLs: parsing, resolving relative references (RFC 3986), and
 * formatting.
 */
#ifndef MANX_URL_H
#define MANX_URL_H

#include <stddef.h>

#define URL_MAX		2048	/* longest URL handled, as text */
#define URL_HOST_MAX	256

struct url {
	char scheme[16];		/* lower case; "" in a relative ref */
	int has_authority;		/* "//" present */
	char host[URL_HOST_MAX];	/* lower case, no userinfo */
	int port;			/* -1: none given */
	char path[URL_MAX];
	int has_query;
	char query[URL_MAX];		/* without the '?' */
	int has_fragment;
	char fragment[URL_MAX];		/* without the '#' */
};

enum { URL_OK = 0, URL_BAD = -1, URL_TOOLONG = -2 };

/* Parse an absolute URL or a relative reference. Leading and trailing
 * white space is dropped, tabs and newlines inside are removed, and
 * spaces become %20 (as browsers do with sloppy hrefs). */
int url_parse(const char *s, struct url *u);

/* Resolve ref against an absolute base URL (RFC 3986 section 5.2). */
int url_resolve(const struct url *base, const char *ref, struct url *out);

/* Write u as text; with_fragment 0 leaves out "#fragment" (for
 * requests). URL_TOOLONG when it doesn't fit. */
int url_format(const struct url *u, char *buf, size_t n, int with_fragment);

/* The port to connect to: the explicit one, else the scheme's default
 * (0 when the scheme has none). */
unsigned url_port(const struct url *u);

/* The request target: path (at least "/") and "?query". */
int url_target(const struct url *u, char *buf, size_t n);

#endif /* MANX_URL_H */
