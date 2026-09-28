/*
 * http.c - HTTP/1.1 response parsing (RFC 9112), push style: the caller
 * feeds whatever arrived, the parser keeps its place. Body framing, in
 * RFC order: no body for HEAD/1xx/204/304; chunked; Content-Length; else
 * the body runs to the close of the connection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "http.h"

enum {
	S_STATUS, S_HEADER, S_BODY_LEN, S_BODY_CLOSE,
	S_CHUNK_SIZE, S_CHUNK_DATA, S_CHUNK_CRLF, S_TRAILER, S_DONE
};

#define USER_AGENT	"ub/0.1 (68030; System V)"

void http_resp_init(struct http_resp *r, const struct http_sink *sink,
	int head_only)
{
	memset(r, 0, sizeof *r);
	r->content_length = -1;
	r->head_only = head_only;
	r->state = S_STATUS;
	if (sink)
		r->sink = *sink;
}

static int ieq(const char *a, const char *b)
{
	while (*a && *b)
		if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
			return 0;
	return *a == *b;
}

/* does a comma-separated header value contain token t? */
static int has_token(const char *v, const char *t)
{
	size_t tl = strlen(t);

	while (*v) {
		size_t n;

		while (*v == ' ' || *v == '\t' || *v == ',')
			v++;
		n = strcspn(v, ", \t;");
		if (n == tl) {
			size_t i;

			for (i = 0; i < n; i++)
				if (tolower((unsigned char)v[i]) != t[i])
					break;
			if (i == n)
				return 1;
		}
		v += n;
		while (*v && *v != ',')
			v++;
	}
	return 0;
}

static void copy_lower(char *dst, size_t cap, const char *src, size_t n)
{
	size_t i;

	if (n >= cap)
		n = cap - 1;
	for (i = 0; i < n; i++)
		dst[i] = (char)tolower((unsigned char)src[i]);
	dst[n] = '\0';
}

static void content_type(struct http_resp *r, const char *v)
{
	const char *cs;

	copy_lower(r->content_type, sizeof r->content_type, v, strcspn(v, "; \t"));
	for (cs = v; (cs = strchr(cs, ';')) != NULL; ) {
		cs++;
		while (*cs == ' ' || *cs == '\t')
			cs++;
		if (strncmp(cs, "charset=", 8) == 0 || strncmp(cs, "CHARSET=", 8) == 0
			|| strncmp(cs, "Charset=", 8) == 0) {
			cs += 8;
			if (*cs == '"')
				cs++;
			copy_lower(r->charset, sizeof r->charset, cs, strcspn(cs, "\"; \t"));
		}
	}
}

static int status_line(struct http_resp *r, char *line)
{
	char *p;

	if (strncmp(line, "HTTP/1.", 7) != 0 || !isdigit((unsigned char)line[7]))
		return HTTP_ERR;
	r->minor = line[7] - '0';
	p = line + 8;
	if (*p != ' ')
		return HTTP_ERR;
	p++;
	if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1])
		|| !isdigit((unsigned char)p[2]))
		return HTTP_ERR;
	r->status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
	/* HTTP/1.1 keeps the connection unless told; 1.0 closes unless told */
	r->keep_alive = r->minor >= 1;
	return HTTP_MORE;
}

static int header_line(struct http_resp *r, char *line)
{
	char *colon = strchr(line, ':'), *v, *e;

	if (colon == NULL || colon == line)
		return HTTP_ERR;
	*colon = '\0';
	if (strpbrk(line, " \t"))	/* no white space before the colon */
		return HTTP_ERR;
	v = colon + 1;
	while (*v == ' ' || *v == '\t')
		v++;
	e = v + strlen(v);
	while (e > v && (e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';

	if (ieq(line, "Content-Length")) {
		char *end;
		long n = strtol(v, &end, 10);

		if (end == v || *end || n < 0)
			return HTTP_ERR;
		/* conflicting lengths are a smuggling signature */
		if (r->content_length >= 0 && r->content_length != n)
			return HTTP_ERR;
		r->content_length = n;
	} else if (ieq(line, "Transfer-Encoding")) {
		if (has_token(v, "chunked"))
			r->chunked = 1;
	} else if (ieq(line, "Connection")) {
		if (has_token(v, "close"))
			r->keep_alive = 0;
		else if (has_token(v, "keep-alive"))
			r->keep_alive = 1;
	} else if (ieq(line, "Location")) {
		r->location[0] = '\0';
		if (strlen(v) < sizeof r->location)
			strcpy(r->location, v);
	} else if (ieq(line, "Content-Type"))
		content_type(r, v);
	else if (ieq(line, "Content-Encoding"))
		copy_lower(r->content_encoding, sizeof r->content_encoding, v, strlen(v));
	if (r->sink.header)
		r->sink.header(r->sink.ctx, line, v);
	return HTTP_MORE;
}

/* the blank line after the headers: choose the body framing */
static int end_of_head(struct http_resp *r)
{
	if (r->status >= 100 && r->status < 200) {
		/* interim response (100 Continue ...): a real one follows */
		struct http_sink s = r->sink;
		int ho = r->head_only;

		http_resp_init(r, &s, ho);
		return HTTP_MORE;
	}
	if (r->head_only || r->status == 204 || r->status == 304) {
		r->state = S_DONE;
		return HTTP_DONE;
	}
	if (r->chunked) {
		r->content_length = -1;
		r->state = S_CHUNK_SIZE;
	} else if (r->content_length >= 0) {
		r->state = S_BODY_LEN;
		if (r->content_length == 0) {
			r->state = S_DONE;
			return HTTP_DONE;
		}
	} else {
		r->state = S_BODY_CLOSE;
		r->keep_alive = 0;	/* only the close ends it */
	}
	return HTTP_MORE;
}

static int chunk_size(struct http_resp *r, const char *line)
{
	long n = 0;
	const char *p = line;

	if (!isxdigit((unsigned char)*p))
		return HTTP_ERR;
	for (; isxdigit((unsigned char)*p); p++) {
		int d = isdigit((unsigned char)*p) ? *p - '0'
			: tolower((unsigned char)*p) - 'a' + 10;

		if (n > 0x7FFFFFFL / 16)
			return HTTP_ERR;
		n = n * 16 + d;
	}
	/* chunk extensions (";name=value") are ignored */
	r->chunk_left = n;
	r->state = n ? S_CHUNK_DATA : S_TRAILER;
	return HTTP_MORE;
}

static int deliver(struct http_resp *r, const unsigned char *d, size_t n)
{
	r->body_bytes += (long)n;
	if (r->sink.body && r->sink.body(r->sink.ctx, d, n) < 0)
		return HTTP_ABORT;
	return HTTP_MORE;
}

/* a complete line (without CRLF) in r->line */
static int line_done(struct http_resp *r)
{
	char *line = r->line;

	switch (r->state) {
	case S_STATUS:
		if (line[0] == '\0')		/* tolerate stray CRLFs before it */
			return HTTP_MORE;
		r->state = S_HEADER;
		return status_line(r, line);
	case S_HEADER:
		if (line[0] == '\0')
			return end_of_head(r);
		if (line[0] == ' ' || line[0] == '\t')
			return HTTP_MORE;	/* obsolete folding: ignored */
		return header_line(r, line);
	case S_CHUNK_SIZE:
		return chunk_size(r, line);
	case S_CHUNK_CRLF:
		if (line[0] != '\0')
			return HTTP_ERR;
		r->state = S_CHUNK_SIZE;
		return HTTP_MORE;
	case S_TRAILER:
		if (line[0] == '\0') {
			r->state = S_DONE;
			return HTTP_DONE;
		}
		return HTTP_MORE;		/* trailer fields are ignored */
	}
	return HTTP_ERR;
}

static int is_line_state(int s)
{
	return s == S_STATUS || s == S_HEADER || s == S_CHUNK_SIZE
		|| s == S_CHUNK_CRLF || s == S_TRAILER;
}

int http_resp_feed(struct http_resp *r, const unsigned char *data,
	size_t len, size_t *used)
{
	size_t i = 0;
	int rc = HTTP_MORE;

	while (i < len && rc == HTTP_MORE) {
		if (r->state == S_DONE) {
			rc = HTTP_DONE;
			break;
		}
		if (is_line_state(r->state)) {
			unsigned char c = data[i++];

			if (r->state == S_STATUS || r->state == S_HEADER)
				if (++r->head_total > HTTP_HEAD_MAX) {
					rc = HTTP_ERR;
					break;
				}
			if (c == '\n') {
				if (r->line_len && r->line[r->line_len - 1] == '\r')
					r->line_len--;
				r->line[r->line_len] = '\0';
				r->line_len = 0;
				rc = line_done(r);
			} else if (r->line_len + 1 >= sizeof r->line)
				rc = HTTP_ERR;
			else
				r->line[r->line_len++] = (char)c;
		} else if (r->state == S_BODY_CLOSE) {
			rc = deliver(r, data + i, len - i);
			i = len;
		} else {
			/* S_BODY_LEN or S_CHUNK_DATA: a counted run of bytes */
			long left = r->state == S_BODY_LEN
				? r->content_length - r->body_bytes : r->chunk_left;
			size_t n = len - i;

			if ((long)n > left)
				n = (size_t)left;
			rc = deliver(r, data + i, n);
			i += n;
			if (r->state == S_CHUNK_DATA) {
				r->chunk_left -= (long)n;
				if (r->chunk_left == 0)
					r->state = S_CHUNK_CRLF;
			} else if (r->body_bytes == r->content_length) {
				r->state = S_DONE;
				rc = rc == HTTP_MORE ? HTTP_DONE : rc;
			}
		}
	}
	if (rc == HTTP_MORE && r->state == S_DONE)
		rc = HTTP_DONE;
	if (used)
		*used = i;
	return rc;
}

int http_resp_eof(struct http_resp *r)
{
	if (r->state == S_BODY_CLOSE || r->state == S_DONE) {
		r->state = S_DONE;
		return HTTP_DONE;
	}
	return HTTP_ERR;
}

int http_request(char *buf, size_t n, const char *method,
	const struct url *u, const char *extra)
{
	char target[URL_MAX], host[URL_HOST_MAX + 12];
	int len;

	if (url_target(u, target, sizeof target) != URL_OK)
		return -1;
	/* the port goes in Host only when it isn't the scheme's default */
	if (u->port >= 0 && !((u->port == 80 && strcmp(u->scheme, "http") == 0)
		|| (u->port == 443 && strcmp(u->scheme, "https") == 0)))
		snprintf(host, sizeof host, "%s:%d", u->host, u->port);
	else
		snprintf(host, sizeof host, "%s", u->host);
	len = snprintf(buf, n,
		"%s %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"User-Agent: " USER_AGENT "\r\n"
		"Accept: text/html, text/plain;q=0.9, */*;q=0.5\r\n"
		"Accept-Encoding: identity\r\n"
		"%s"
		"\r\n", method, target, host, extra ? extra : "");
	return len < 0 || (size_t)len >= n ? -1 : len;
}
