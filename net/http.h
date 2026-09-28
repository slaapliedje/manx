/*
 * http.h - HTTP/1.1 client messages: building requests and parsing
 * responses incrementally, as bytes arrive.
 */
#ifndef UB_HTTP_H
#define UB_HTTP_H

#include <stddef.h>
#include "url.h"

#define HTTP_LINE_MAX	8192	/* longest status/header line */
#define HTTP_HEAD_MAX	32768	/* all headers of one response */

/* what the parser reports while it runs */
struct http_sink {
	void *ctx;
	/* each header field as it is parsed (name without the colon, value
	 * trimmed); may be NULL */
	void (*header)(void *ctx, const char *name, const char *value);
	/* decoded body bytes (de-chunked); may be NULL */
	int (*body)(void *ctx, const unsigned char *data, size_t len);
};

struct http_resp {
	/* filled in as the head is parsed */
	int status;			/* 200, 404, ... */
	int minor;			/* HTTP/1.minor */
	long content_length;		/* -1: not given */
	int chunked;
	int keep_alive;			/* connection may be reused afterwards */
	char location[URL_MAX];		/* for redirects */
	char content_type[128];		/* media type, lower case, no parameters */
	char charset[32];		/* from Content-Type, lower case */
	char content_encoding[32];
	long body_bytes;		/* body bytes delivered so far */

	/* parser state (private) */
	int state, head_only;
	size_t head_total;
	long chunk_left;
	char line[HTTP_LINE_MAX];
	size_t line_len;
	struct http_sink sink;
};

enum {
	HTTP_MORE = 0,		/* feed more bytes */
	HTTP_DONE = 1,		/* the response is complete */
	HTTP_ERR = -1,		/* malformed or over a limit */
	HTTP_ABORT = -2		/* the body sink asked to stop */
};

/* Start parsing a response. head_only: the request was HEAD (no body). */
void http_resp_init(struct http_resp *r, const struct http_sink *sink,
	int head_only);

/* Feed bytes; *used says how many were consumed (the rest belong to the
 * next response on a kept-alive connection). */
int http_resp_feed(struct http_resp *r, const unsigned char *data,
	size_t len, size_t *used);

/* The connection closed: completes a body delimited by the close, fails
 * any other unfinished response. */
int http_resp_eof(struct http_resp *r);

/* Build a GET (or HEAD) request for u. Returns its length, or -1 if it
 * doesn't fit. extra: more header lines, each ending in \r\n, or NULL. */
int http_request(char *buf, size_t n, const char *method,
	const struct url *u, const char *extra);

#endif /* UB_HTTP_H */
