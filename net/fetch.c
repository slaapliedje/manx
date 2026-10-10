/*
 * fetch.c - see fetch.h.
 *
 * gopher: the selector is the URL path after the item type (RFC 4266):
 * gopher://host/1/dir gives type '1' and selector "/dir". Menus (type 1
 * and 7) are delivered as "text/x-gopher-menu" for the renderer; type 0
 * as text/plain; the rest as application/octet-stream, with the type
 * character appended so a viewer can tell images from binaries.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "os.h"
#include "conn.h"
#include "http.h"
#include <time.h>
#include "fetch.h"
#include "inflate.h"
#include "cookie.h"

int fetch_early_requests = 1;
int fetch_keep_alive = 1;

static void status(const struct fetch_cb *cb, const char *fmt, const char *arg)
{
	char msg[URL_MAX + 64];

	if (cb->status == NULL)
		return;
	snprintf(msg, sizeof msg, fmt, arg);
	cb->status(cb->ctx, msg);
}

static int fail(struct fetch_result *res, const char *msg)
{
	snprintf(res->error, sizeof res->error, "%s", msg);
	return -1;
}

/* --- http and https ---------------------------------------------------- */

/* one response's parser callbacks: straight through to cb (through a
 * gzip/deflate decoder when the body is encoded), noting whether the
 * receiver asked to stop */
struct hctx {
	const struct fetch_cb *cb;
	const struct url *u;
	struct http_resp *r;
	struct inflate *z;
	int decided;			/* whether to decode, looked at */
	int corrupt;
	int stopped;
	unsigned long t0;
};

static int h_decoded(void *ctx, const unsigned char *d, size_t n)
{
	struct hctx *h = ctx;

	if (h->cb->body && h->cb->body(h->cb->ctx, d, n) < 0) {
		h->stopped = 1;
		return -1;
	}
	return 0;
}

/* ASCII case-insensitive equality */
static int same_word(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
	return *a == *b;
}

static void h_header(void *ctx, const char *name, const char *value)
{
	struct hctx *h = ctx;

	/* cookies from every response, redirects too (logins set them on
	 * the redirect that follows the form) */
	if (cookie_enabled && same_word(name, "set-cookie"))
		cookie_set(h->u, value, (long)time(NULL));
	if (h->cb->header)
		h->cb->header(h->cb->ctx, name, value);
}

static int h_body(void *ctx, const unsigned char *d, size_t n)
{
	struct hctx *h = ctx;
	int rc;

	if (!h->decided) {
		/* the head is complete by the first body byte */
		const char *ce = h->r->content_encoding;
		int fmt = strcmp(ce, "gzip") == 0 || strcmp(ce, "x-gzip") == 0 ?
			INF_GZIP : strcmp(ce, "deflate") == 0 ? INF_DEFLATE : -1;

		h->decided = 1;
		if (fmt >= 0 && (h->z = inflate_new(fmt, h_decoded, h)) == NULL) {
			h->corrupt = 1;
			return -1;
		}
	}
	if (h->z == NULL)
		return h_decoded(h, d, n);
	rc = inflate_feed(h->z, d, n);
	if (rc == INF_BAD) {
		h->corrupt = 1;
		return -1;
	}
	return rc == INF_STOP ? -1 : 0;
}

/* the response ended: the encoded body must have ended too */
static int h_finish(struct hctx *h)
{
	int rc;

	if (h->z == NULL)
		return 0;
	rc = inflate_finish(h->z);
	inflate_free(h->z);
	h->z = NULL;
	if (rc == INF_END)
		return 0;
	if (rc == INF_BAD)
		h->corrupt = 1;
	return -1;
}

static int is_redirect(int s)
{
	return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

/*
 * One request/response on one connection. Returns 0 with the response
 * parsed, -1 on failure; *retry is set when a pooled connection turned
 * out dead before any byte of the response came (worth one retry).
 */
/* the request's extra header lines: cookies, referer, the body's type
 * and length. 1 when they carry something private (then the request
 * must wait for the certificate check). */
static int extra_headers(const struct url *u, const struct fetch_opts *o,
	char *buf, size_t n)
{
	static char cookies[4096];
	static struct url ref;
	size_t len = 0;
	int private = 0;

	buf[0] = '\0';
	if (cookie_header(u, (long)time(NULL), cookies, sizeof cookies) > 0
		&& strlen(cookies) + 12 < n) {
		len += (size_t)sprintf(buf, "Cookie: %s\r\n", cookies);
		private = 1;
	}
	if (o && o->referer && url_parse(o->referer, &ref) == URL_OK
		&& strcmp(ref.scheme, u->scheme) == 0
		&& strcmp(ref.host, u->host) == 0 && url_port(&ref) == url_port(u)) {
		char r[URL_MAX];

		/* the same origin only: other sites don't learn where from */
		if (url_format(&ref, r, sizeof r, 0) == URL_OK
			&& len + strlen(r) + 14 < n)
			len += (size_t)sprintf(buf + len, "Referer: %s\r\n", r);
	}
	if (o && o->extra && len + strlen(o->extra) + 1 < n)
		len += (size_t)sprintf(buf + len, "%s", o->extra);
	if (o && o->body) {
		if (len + 100 < n)
			len += (size_t)sprintf(buf + len, "Content-Type: %.60s\r\n"
				"Content-Length: %lu\r\n", o->body_type ? o->body_type
				: "application/x-www-form-urlencoded",
				(unsigned long)o->body_len);
		private = 1;
	}
	return private;
}

static int http_once(const struct url *u, const char *method,
	const struct fetch_opts *opts, const struct fetch_cb *cb,
	struct fetch_result *res, struct http_resp *r, int *retry)
{
	static char req[URL_MAX + 1024 + 4096 + URL_MAX];
	static char extra[4096 + URL_MAX + 200];
	int private;
	static unsigned char buf[4096];
	struct hctx h;
	struct http_sink sink;
	struct conn *c;
	char err[200];
	int is_tls = strcmp(u->scheme, "https") == 0, n, rc, got_any = 0;
	size_t used;
	unsigned port = url_port(u);

	*retry = 0;
	status(cb, "Connecting to %s...", u->host);
	/* a request with nothing private (no cookies, no form data) may go
	 * before the certificate check finishes (conn.h); others wait */
	private = extra_headers(u, opts, extra, sizeof extra);
	c = conn_open(u->host, port, is_tls, fetch_early_requests && !private,
		err, sizeof err);
	if (c == NULL)
		return fail(res, err);
	res->reused = c->reused;
	res->reconnected = c->reconnected;
	if (!c->reused) {
		res->t_dns = c->t_dns;
		res->t_connect = c->t_connect;
		res->tls = is_tls;
	}

	n = http_request(req, sizeof req, method, u, extra);
	if (n < 0) {
		conn_release(c, 0);
		return fail(res, "URL too long for a request");
	}
	if (conn_write(c, req, (size_t)n) < 0
		|| (opts && opts->body && opts->body_len
		&& conn_write(c, opts->body, opts->body_len) < 0)) {
		*retry = c->reused;
		conn_release(c, 0);
		return fail(res, "sending the request failed");
	}

	memset(&h, 0, sizeof h);
	h.cb = cb;
	h.u = u;
	h.r = r;
	h.t0 = os_msec();
	sink.ctx = &h;
	sink.header = h_header;
	sink.body = h_body;
	http_resp_init(r, &sink, strcmp(method, "HEAD") == 0);
	status(cb, "Waiting for %s...", u->host);
	rc = HTTP_MORE;
	while (rc == HTTP_MORE) {
		n = conn_read(c, buf, sizeof buf);
		if (n <= 0) {
			rc = n == 0 ? http_resp_eof(r) : HTTP_ERR;
			if (!got_any)
				*retry = c->reused;
			break;
		}
		if (!got_any) {
			res->t_first = os_msec() - h.t0;
			got_any = 1;
		}
		rc = http_resp_feed(r, buf, (size_t)n, &used);
		if (rc == HTTP_DONE && used < (size_t)n)
			conn_unread(c, buf + used, (size_t)n - used);
	}
	res->t_body = os_msec() - h.t0;
	/* the TLS figures are final now: with an early request the
	 * validation ran at the first read */
	if (is_tls && !c->reused) {
		const struct tls_info *ti = &c->tls->info;
		char msg[200];

		char pre[64];

		pre[0] = '\0';	/* (Helios C cannot initialise an auto array) */
		if (ti->pre_jobs)
			snprintf(pre, sizeof pre, " (%d signature%s ahead, %d on the T425, %d used)",
				ti->pre_jobs, ti->pre_jobs == 1 ? "" : "s", ti->pre_t425,
				ti->pre_used);
		snprintf(msg, sizeof msg, "TLS %s%s%s%s: handshake %lu ms, "
			"validation %lu ms%s, suite 0x%04x",
			tls_profile_name(ti->profile), ti->resumed ? ", resumed" : "",
			ti->leaf_memo ? ", known leaf" : "",
			c->reconnected ? ", reconnected" : "", ti->t_handshake,
			ti->t_verify, pre, ti->suite);
		status(cb, "%s", msg);
		if (!ti->resumed) {
			snprintf(msg, sizeof msg, "TLS time: handshake %lu ms waiting "
				"for the network (%d reads), %lu ms key exchange, %lu ms "
				"signatures; validation %lu ms signatures", ti->t_hs_net,
				ti->hs_reads, ti->t_hs_ec, ti->t_hs_sig, ti->t_v_sig);
			status(cb, "%s", msg);
		}
		res->tls_resumed = ti->resumed;
		res->tls_leaf_memo = ti->leaf_memo;
		res->tls_learned = ti->learned;
		res->tls_profile = ti->profile;
		res->tls_suite = ti->suite;
		res->t_handshake = ti->t_handshake;
		res->t_verify = ti->t_verify;
	}
	if (rc == HTTP_DONE && h_finish(&h) < 0 && !h.stopped) {
		conn_release(c, 0);
		return fail(res, "the compressed body is corrupt or cut short");
	}
	if (h.z) {
		inflate_free(h.z);
		h.z = NULL;
	}
	if (h.corrupt && !h.stopped) {
		conn_release(c, 0);
		return fail(res, "the compressed body is corrupt");
	}
	if (rc != HTTP_DONE) {
		const char *why = conn_error(c);

		if (why) {
			snprintf(err, sizeof err, "%.60s: server not trusted: %s",
				u->host, why);
			conn_release(c, 0);
			*retry = 0;
			return fail(res, err);
		}
		/* a long validation on a new connection: the server has likely
		 * given up meanwhile (or dropped a response nobody read). Once
		 * more is cheap now: the session is stored, the leaf known. */
		if (is_tls && !c->reused && !h.stopped
			&& c->tls->info.t_verify > 8000
			&& (res->body_bytes == 0 || cb->reset))
			*retry = 1;
		conn_release(c, 0);
		if (h.stopped)
			return fail(res, "stopped");
		return fail(res, got_any ? "malformed or truncated response"
			: "no response from the server");
	}
	conn_release(c, r->keep_alive && fetch_keep_alive);
	return 0;
}

/*
 * Which response is the one to deliver? A redirect's headers and body
 * are dropped. The status line is always parsed before any header or
 * body byte reaches the callbacks, so the shim decides at its first call.
 */
struct final_shim {
	struct fetch_cb inner;
	const struct fetch_cb *outer;
	struct http_resp *r;
	struct fetch_result *res;
	struct url *u;
	int decided, final;
};

static void decide(struct final_shim *s)
{
	if (s->decided || s->r->status == 0)
		return;
	s->decided = 1;
	s->final = !is_redirect(s->r->status) || s->r->location[0] == '\0';
}

static void shim_header(void *ctx, const char *name, const char *value)
{
	struct final_shim *s = ctx;

	decide(s);
	if (s->final && s->outer->header)
		s->outer->header(s->outer->ctx, name, value);
}

static int shim_body(void *ctx, const unsigned char *d, size_t n)
{
	struct final_shim *s = ctx;

	decide(s);
	if (!s->final)
		return 0;
	if (!s->res->body_bytes && s->outer->head) {
		char url[URL_MAX];

		url_format(s->u, url, sizeof url, 0);
		s->outer->head(s->outer->ctx, s->r->status, s->r->content_type,
			s->r->charset, url);
	}
	s->res->body_bytes += (long)n;
	return s->outer->body ? s->outer->body(s->outer->ctx, d, n) : 0;
}

static void shim_reset(void *ctx)
{
	struct final_shim *s = ctx;

	s->res->body_bytes = 0;
	s->decided = 0;
	s->outer->reset(s->outer->ctx);
}

static int http_fetch(struct url *u, const char *method,
	const struct fetch_opts *opts, const struct fetch_cb *cb,
	struct fetch_result *res)
{
	static struct http_resp r;
	static struct url next;
	struct final_shim shim;
	int hops, retry, rc;

	for (hops = 0; hops <= FETCH_MAX_REDIRECTS; hops++) {
		memset(&shim, 0, sizeof shim);
		shim.outer = cb;
		shim.r = &r;
		shim.res = res;
		shim.u = u;
		shim.inner.ctx = &shim;
		shim.inner.status = cb->status;
		shim.inner.header = shim_header;
		shim.inner.body = shim_body;
		shim.inner.reset = cb->reset ? shim_reset : NULL;
		rc = http_once(u, method, opts, &shim.inner, res, &r, &retry);
		if (rc < 0 && retry) {
			status(cb, "Reconnecting to %s...", u->host);
			res->error[0] = '\0';
			if (res->body_bytes && shim.inner.reset)
				shim.inner.reset(shim.inner.ctx);
			res->body_bytes = 0;
			rc = http_once(u, method, opts, &shim.inner, res, &r, &retry);
		}
		if (rc < 0)
			return -1;
		decide(&shim);
		if (!is_redirect(r.status) || r.location[0] == '\0')
			break;
		/* follow the redirect */
		if (url_resolve(u, r.location, &next) != URL_OK)
			return fail(res, "bad redirect location");
		if (strcmp(next.scheme, "http") && strcmp(next.scheme, "https"))
			return fail(res, "redirect to an unsupported scheme");
		/* (a fragment the target doesn't give is the one asked for) */
		if (!next.has_fragment && u->has_fragment) {
			strcpy(next.fragment, u->fragment);
			next.has_fragment = 1;
		}
		*u = next;
		res->redirects++;
		/* after a POST: 301/302/303 go on with a GET (as browsers do),
		 * 307/308 repeat the POST */
		if (opts && opts->body && r.status != 307 && r.status != 308) {
			method = "GET";
			opts = NULL;
		}
		{
			char url[URL_MAX];

			url_format(u, url, sizeof url, 0);
			status(cb, "Redirected to %s", url);
		}
	}
	if (hops > FETCH_MAX_REDIRECTS)
		return fail(res, "too many redirects");
	res->status = r.status;
	res->wire_bytes = r.body_bytes;
	snprintf(res->content_type, sizeof res->content_type, "%s", r.content_type);
	snprintf(res->charset, sizeof res->charset, "%s", r.charset);
	/* a final response with an empty body still reports its head */
	if (res->body_bytes == 0 && cb->head) {
		char url[URL_MAX];

		url_format(u, url, sizeof url, 0);
		cb->head(cb->ctx, r.status, r.content_type, r.charset, url);
	}
	return 0;
}

/* --- gopher ------------------------------------------------------------ */

static int gopher_fetch(const struct url *u, const struct fetch_cb *cb,
	struct fetch_result *res)
{
	static char sel[URL_MAX + 4];
	static unsigned char buf[4096];
	const char *p = u->path;
	char type = '1', err[200];
	const char *ctype;
	struct conn *c;
	unsigned long t0;
	size_t n;
	int got;

	if (p[0] == '/' && p[1]) {
		type = p[1];
		p += 2;
	}
	/* the selector is sent percent-decoded */
	for (n = 0; *p && n < sizeof sel - 3; p++) {
		if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
			char hx[3];

			hx[0] = p[1];
			hx[1] = p[2];
			hx[2] = '\0';
			sel[n++] = (char)strtol(hx, 0, 16);
			p += 2;
		} else
			sel[n++] = *p;
	}
	if (u->has_query && type == '7') {	/* search: selector TAB query */
		const char *q = u->query;

		sel[n++] = '\t';
		/* decoded too: "a%20b" is sent as "a b" */
		for (; *q && n < sizeof sel - 3; q++) {
			if (*q == '%' && isxdigit((unsigned char)q[1])
				&& isxdigit((unsigned char)q[2])) {
				char hx[3];

				hx[0] = q[1];
				hx[1] = q[2];
				hx[2] = '\0';
				sel[n++] = (char)strtol(hx, 0, 16);
				q += 2;
			} else
				sel[n++] = *q == '+' ? ' ' : *q;
		}
		if (*q)
			return fail(res, "gopher search too long");
	}
	sel[n++] = '\r';
	sel[n++] = '\n';

	status(cb, "Connecting to %s...", u->host);
	c = conn_open(u->host, url_port(u), 0, 0, err, sizeof err);
	if (c == NULL)
		return fail(res, err);
	res->t_dns = c->t_dns;
	res->t_connect = c->t_connect;
	if (conn_write(c, sel, n) < 0) {
		conn_release(c, 0);
		return fail(res, "sending the selector failed");
	}
	ctype = type == '1' || type == '7' ? "text/x-gopher-menu"
		: type == '0' ? "text/plain"
		: type == 'h' ? "text/html"
		: type == 'g' ? "image/gif"
		: type == 'I' ? "image/unknown" : "application/octet-stream";
	res->status = 200;
	snprintf(res->content_type, sizeof res->content_type, "%s", ctype);
	if (cb->head) {
		char url[URL_MAX];

		url_format(u, url, sizeof url, 0);
		cb->head(cb->ctx, 200, ctype, "", url);
	}
	t0 = os_msec();
	while ((got = conn_read(c, buf, sizeof buf)) > 0) {
		if (res->body_bytes == 0)
			res->t_first = os_msec() - t0;
		res->body_bytes += got;
		if (cb->body && cb->body(cb->ctx, buf, (size_t)got) < 0)
			break;
	}
	res->t_body = os_msec() - t0;
	conn_release(c, 0);
	return got < 0 ? fail(res, "gopher transfer failed") : 0;
}

/* --- file -------------------------------------------------------------- */

static int file_fetch(const struct url *u, const struct fetch_cb *cb,
	struct fetch_result *res)
{
	static unsigned char buf[4096];
	static char path[URL_MAX];
	const char *p = u->path, *dot;
	const char *ctype = "text/plain";
	FILE *f;
	size_t n = 0, got;

	for (; *p && n < sizeof path - 1; p++) {
		if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
			char hx[3];

			hx[0] = p[1];
			hx[1] = p[2];
			hx[2] = '\0';
			path[n++] = (char)strtol(hx, 0, 16);
			p += 2;
		} else
			path[n++] = *p;
	}
	path[n] = '\0';
	if ((f = fopen(path, "rb")) == NULL)
		return fail(res, "can't open the file");
	dot = strrchr(path, '.');
	if (dot && (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0))
		ctype = "text/html";
	else if (dot && strcmp(dot, ".gif") == 0)
		ctype = "image/gif";
	else if (dot && strcmp(dot, ".png") == 0)
		ctype = "image/png";
	else if (dot && (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0))
		ctype = "image/jpeg";
	res->status = 200;
	snprintf(res->content_type, sizeof res->content_type, "%s", ctype);
	if (cb->head) {
		char url[URL_MAX];

		url_format(u, url, sizeof url, 0);
		cb->head(cb->ctx, 200, ctype, "", url);
	}
	while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
		res->body_bytes += (long)got;
		if (cb->body && cb->body(cb->ctx, buf, got) < 0)
			break;
	}
	fclose(f);
	return 0;
}

/* --------------------------------------------------------------------- */

int fetch(const char *url, const char *method, const struct fetch_cb *cb,
	struct fetch_result *res)
{
	return fetch_ex(url, method, NULL, cb, res);
}

int fetch_ex(const char *url, const char *method, const struct fetch_opts *opts,
	const struct fetch_cb *cb, struct fetch_result *res)
{
	static struct url u;
	static const struct fetch_cb none;
	int rc;

	if (cb == NULL)
		cb = &none;
	memset(res, 0, sizeof *res);
	if (url_parse(url, &u) != URL_OK || u.scheme[0] == '\0')
		return fail(res, "not a valid URL");
	if (strcmp(u.scheme, "http") == 0 || strcmp(u.scheme, "https") == 0) {
		if (u.host[0] == '\0')
			return fail(res, "URL without a host");
		rc = http_fetch(&u, method, opts, cb, res);
	} else if (strcmp(u.scheme, "gopher") == 0) {
		if (u.host[0] == '\0')
			return fail(res, "URL without a host");
		rc = gopher_fetch(&u, cb, res);
	} else if (strcmp(u.scheme, "file") == 0)
		rc = file_fetch(&u, cb, res);
	else
		return fail(res, "unsupported URL scheme");
	url_format(&u, res->url, sizeof res->url, 1);
	return rc;
}
