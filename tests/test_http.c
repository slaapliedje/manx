/* test_http - the response parser, fed whole and one byte at a time. */
#include <stdio.h>
#include <string.h>
#include "http.h"

static int fails;
static char body[4096];
static size_t blen;
static int nheaders;

static int on_body(void *ctx, const unsigned char *d, size_t n)
{
	(void)ctx;
	if (blen + n < sizeof body) {
		memcpy(body + blen, d, n);
		blen += n;
	}
	return 0;
}

static void on_header(void *ctx, const char *name, const char *value)
{
	(void)ctx; (void)name; (void)value;
	nheaders++;
}

/*
 * Parse msg in pieces of `step` bytes (0: all at once), then eof if
 * `close`. Check the result, the body and the bytes left over.
 */
static void run(const char *what, const char *msg, size_t step, int close,
	int head_only, int want_rc, int want_status, const char *want_body,
	size_t want_left)
{
	struct http_resp r;
	struct http_sink s = { 0, on_header, on_body };
	size_t len = strlen(msg), off = 0, used;
	int rc = HTTP_MORE;

	blen = 0;
	nheaders = 0;
	http_resp_init(&r, &s, head_only);
	while (off < len && rc == HTTP_MORE) {
		size_t n = step ? (len - off < step ? len - off : step) : len - off;

		rc = http_resp_feed(&r, (const unsigned char *)msg + off, n, &used);
		off += used;
	}
	if (rc == HTTP_MORE && close)
		rc = http_resp_eof(&r);
	body[blen] = '\0';
	if (rc != want_rc || (want_rc >= 0 && r.status != want_status)
		|| (want_body && strcmp(body, want_body) != 0)
		|| (want_rc == HTTP_DONE && len - off != want_left)) {
		printf("FAIL %s (step %lu): rc %d status %d body [%s] left %lu\n",
			what, (unsigned long)step, rc, r.status, body,
			(unsigned long)(len - off));
		fails++;
	}
}

static void both(const char *what, const char *msg, int close, int head_only,
	int want_rc, int want_status, const char *want_body, size_t want_left)
{
	run(what, msg, 0, close, head_only, want_rc, want_status, want_body, want_left);
	run(what, msg, 1, close, head_only, want_rc, want_status, want_body, want_left);
	run(what, msg, 7, close, head_only, want_rc, want_status, want_body, want_left);
}

int main(void)
{
	struct http_resp r;
	struct url u;
	char req[512];

	both("content-length",
		"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloNEXT", 0, 0,
		HTTP_DONE, 200, "hello", 4);
	both("chunked",
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
		"5\r\nhello\r\n7;ext=1\r\n, world\r\n0\r\nX-Trailer: 1\r\n\r\nNEXT", 0, 0,
		HTTP_DONE, 200, "hello, world", 4);
	both("until close",
		"HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<p>bye", 1, 0,
		HTTP_DONE, 200, "<p>bye", 0);
	both("head", "HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\n", 0, 1,
		HTTP_DONE, 200, "", 0);
	both("304", "HTTP/1.1 304 Not Modified\r\nETag: x\r\n\r\n", 0, 0,
		HTTP_DONE, 304, "", 0);
	both("100 continue",
		"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
		0, 0, HTTP_DONE, 200, "ok", 0);
	both("bare LF", "HTTP/1.1 200 OK\nContent-Length: 2\n\nok", 0, 0,
		HTTP_DONE, 200, "ok", 0);
	both("truncated length",
		"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", 1, 0,
		HTTP_ERR, 0, NULL, 0);
	both("conflicting lengths",
		"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab", 0, 0,
		HTTP_ERR, 0, NULL, 0);
	both("not http", "SSH-2.0-OpenSSH\r\n", 0, 0, HTTP_ERR, 0, NULL, 0);
	both("bad chunk", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
		0, 0, HTTP_ERR, 0, NULL, 0);

	/* header fields reach the parsed response */
	{
		const char *m = "HTTP/1.1 301 Moved\r\nLocation: /new?x=1\r\n"
			"Content-Type: text/HTML; charset=\"ISO-8859-1\"\r\n"
			"Connection: close\r\nContent-Length: 0\r\n\r\n";
		size_t used;

		http_resp_init(&r, NULL, 0);
		if (http_resp_feed(&r, (const unsigned char *)m, strlen(m), &used) != HTTP_DONE
			|| r.status != 301 || strcmp(r.location, "/new?x=1")
			|| strcmp(r.content_type, "text/html") || strcmp(r.charset, "iso-8859-1")
			|| r.keep_alive) {
			printf("FAIL fields: %d [%s] [%s] [%s] %d\n", r.status, r.location,
				r.content_type, r.charset, r.keep_alive);
			fails++;
		}
	}

	/* requests */
	url_parse("http://example.com:8080/a b?q", &u);
	if (http_request(req, sizeof req, "GET", &u, "Cookie: a=b\r\n") < 0
		|| strncmp(req, "GET /a%20b?q HTTP/1.1\r\nHost: example.com:8080\r\n", 46)
		|| !strstr(req, "Cookie: a=b\r\n\r\n")) {
		printf("FAIL request:\n%s", req);
		fails++;
	}
	url_parse("https://example.com:443", &u);
	http_request(req, sizeof req, "HEAD", &u, NULL);
	if (strncmp(req, "HEAD / HTTP/1.1\r\nHost: example.com\r\n", 36)) {
		printf("FAIL request default port:\n%s", req);
		fails++;
	}
	printf("http: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
