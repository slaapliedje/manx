/*
 * imap.c - an IMAP4rev1 client (imap.h).
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "mime.h"
#include "imap.h"

#define RESP_MAX	(16UL * 1024 * 1024)	/* a response's most (attachments) */
#define DEPTH_MAX	32

static int fail(struct imap *m, const char *fmt, const char *arg)
{
	snprintf(m->err, sizeof m->err, fmt, arg ? arg : "");
	return -1;
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* does s begin with w, case aside? */
static int ci_prefix(const char *s, const char *w)
{
	while (*w)
		if (lower((unsigned char)*s++) != lower((unsigned char)*w++))
			return 0;
	return 1;
}

/* --- reading ------------------------------------------------------------ */

static int fill(struct imap *m)
{
	int r = conn_read(m->c, m->rbuf, sizeof m->rbuf);

	if (r <= 0)
		return -1;
	m->rpos = 0;
	m->rlen = (size_t)r;
	return 0;
}

static int put(struct imap *m, const void *s, size_t n)
{
	if (m->len + n + 1 > m->cap) {
		size_t c = m->cap ? m->cap : 1024;
		char *q;

		while (c < m->len + n + 1)
			c *= 2;
		if (c > RESP_MAX || (q = xrealloc(m->buf, c)) == NULL)
			return -1;
		m->buf = q;
		m->cap = c;
	}
	memcpy(m->buf + m->len, s, n);
	m->len += n;
	m->buf[m->len] = '\0';
	return 0;
}

/* the connection is no use any more (what's left of a response can't be
 * told from the next): closed, m->c NULL */
static void drop(struct imap *m)
{
	if (m->c) {
		conn_release(m->c, 0);
		m->c = NULL;
	}
}

/*
 * One response into m->buf: a line without its CRLF, and where it ends in
 * a literal ({n}), the n bytes right after the '}' and the line going on.
 */
static int read_resp1(struct imap *m)
{
	m->len = 0;
	if (put(m, "", 0) < 0)
		return fail(m, "out of memory", NULL);
	for (;;) {
		size_t start = m->len, i;
		unsigned long n;

		for (;;) {
			unsigned char c;

			if (m->rpos == m->rlen && fill(m) < 0)
				return fail(m, "the server went away", NULL);
			c = m->rbuf[m->rpos++];
			if (c == '\n')
				break;
			if (c != '\r' && put(m, &c, 1) < 0)
				return fail(m, "a response too long", NULL);
		}
		if (m->len == start || m->buf[m->len - 1] != '}')
			return 0;
		for (i = m->len - 1; i > start && m->buf[i - 1] >= '0' && m->buf[i - 1] <= '9'; i--)
			;
		if (i == start || m->buf[i - 1] != '{' || i == m->len - 1)
			return 0;
		n = strtoul(m->buf + i, NULL, 10);
		if (n > RESP_MAX)
			return fail(m, "a response too long", NULL);
		while (n > 0) {
			size_t k;

			if (m->rpos == m->rlen && fill(m) < 0)
				return fail(m, "the server went away", NULL);
			k = m->rlen - m->rpos;
			if (k > n)
				k = n;
			if (put(m, m->rbuf + m->rpos, k) < 0)
				return fail(m, "a response too long", NULL);
			m->rpos += k;
			n -= k;
		}
	}
}

static int read_resp(struct imap *m)
{
	if (read_resp1(m) < 0) {
		drop(m);
		return -1;
	}
	return 0;
}

/* --- parsing ------------------------------------------------------------ */

static long new_val(struct imap *m)
{
	if (m->niv == m->ivcap) {
		unsigned long c = m->ivcap ? m->ivcap * 2 : 256;
		struct ival *q = xrealloc(m->iv, c * sizeof *q);

		if (q == NULL)
			return -1;
		m->iv = q;
		m->ivcap = c;
	}
	memset(&m->iv[m->niv], 0, sizeof m->iv[m->niv]);
	m->iv[m->niv].first = m->iv[m->niv].next = -1;
	return (long)m->niv++;
}

/* one value at *pp (before e): its index, or -1 */
static long parse_val(struct imap *m, char **pp, char *e, int depth)
{
	char *p = *pp;
	long k;

	while (p < e && *p == ' ')
		p++;
	if (p >= e || (k = new_val(m)) < 0)
		return -1;
	if (*p == '(' || *p == '[') {
		/* a list (or a response code in brackets) */
		char close = *p == '(' ? ')' : ']';
		long prev = -1;

		p++;
		m->iv[k].type = IV_LIST;
		for (;;) {
			long c;

			while (p < e && *p == ' ')
				p++;
			if (p >= e)
				break;
			if (*p == close) {
				p++;
				break;
			}
			if (depth >= DEPTH_MAX || (c = parse_val(m, &p, e, depth + 1)) < 0)
				return -1;
			if (prev < 0)
				m->iv[k].first = c;
			else
				m->iv[prev].next = c;
			prev = c;
			m->iv[k].n++;
		}
	} else if (*p == '"') {
		/* a quoted string, unescaped where it lies */
		char *s = ++p, *o = s;

		while (p < e && *p != '"') {
			if (*p == '\\' && p + 1 < e)
				p++;
			*o++ = *p++;
		}
		if (p < e)
			p++;
		m->iv[k].type = IV_STR;
		m->iv[k].s = s;
		m->iv[k].len = (unsigned long)(o - s);
	} else if (*p == '{') {
		/* a literal: its bytes follow the '}' (read_resp) */
		unsigned long n = 0;

		for (p++; p < e && *p >= '0' && *p <= '9'; p++)
			n = n * 10 + (unsigned long)(*p - '0');
		if (p < e && *p == '+')
			p++;
		if (p < e && *p == '}')
			p++;
		if (n > (unsigned long)(e - p))
			n = (unsigned long)(e - p);
		m->iv[k].type = IV_STR;
		m->iv[k].s = p;
		m->iv[k].len = n;
		p += n;
	} else {
		/* an atom: a number, a flag, BODY[1.2]<0> */
		char *s = p;

		while (p < e && *p != ' ' && *p != '(' && *p != ')' && *p != ']') {
			if (*p == '[') {
				while (p < e && *p != ']')
					p++;
				if (p < e)
					p++;
				continue;
			}
			p++;
		}
		if (p == s)
			p++;		/* (a stray ')' or ']': skipped) */
		m->iv[k].type = p - s == 3 && ci_prefix(s, "NIL") ? IV_NIL : IV_ATOM;
		m->iv[k].s = s;
		m->iv[k].len = (unsigned long)(p - s);
	}
	*pp = p;
	return k;
}

long imap_parse(struct imap *m, char *s, char *e, unsigned long *n)
{
	long first = -1, prev = -1, k;

	m->niv = 0;
	*n = 0;
	while (s < e) {
		while (s < e && *s == ' ')
			s++;
		if (s >= e || (k = parse_val(m, &s, e, 0)) < 0)
			break;
		if (prev < 0)
			first = k;
		else
			m->iv[prev].next = k;
		prev = k;
		(*n)++;
	}
	return first;
}

/* --- values ---------------------------------------------------------------- */

struct ival *iv_at(struct imap *m, const struct ival *v, unsigned long i)
{
	long k;

	if (v == NULL || v->type != IV_LIST)
		return NULL;
	for (k = v->first; k >= 0 && i > 0; i--)
		k = m->iv[k].next;
	return k >= 0 ? &m->iv[k] : NULL;
}

struct ival *iv_next(struct imap *m, const struct ival *v)
{
	return v && v->next >= 0 ? &m->iv[v->next] : NULL;
}

int iv_is(const struct ival *v, const char *w)
{
	return v && v->type == IV_ATOM && v->len == strlen(w) && ci_prefix(v->s, w);
}

unsigned long iv_num(const struct ival *v)
{
	unsigned long n = 0, i;

	if (v == NULL || v->type != IV_ATOM)
		return 0;
	for (i = 0; i < v->len && v->s[i] >= '0' && v->s[i] <= '9'; i++)
		n = n * 10 + (unsigned long)(v->s[i] - '0');
	return n;
}

char *iv_str(const struct ival *v, char *out, size_t n)
{
	size_t k = 0;

	if (v && (v->type == IV_ATOM || v->type == IV_STR)) {
		k = v->len < n - 1 ? v->len : n - 1;
		memcpy(out, v->s, k);
	}
	out[k] = '\0';
	return out;
}

/* --- commands ------------------------------------------------------------- */

/* [CAPABILITY ...] at s (a response code), or the words of a CAPABILITY
 * response: kept */
static void take_caps(struct imap *m, const char *s)
{
	size_t n = 0;

	m->caps[0] = ' ';
	n = 1;
	while (*s && *s != ']' && n + 2 < sizeof m->caps)
		m->caps[n++] = *s++;
	m->caps[n++] = ' ';
	m->caps[n] = '\0';
}

static int has_cap(const struct imap *m, const char *c)
{
	const char *p = m->caps;
	size_t n = strlen(c);

	while ((p = strchr(p, ' ')) != NULL) {
		p++;
		if (ci_prefix(p, c) && (p[n] == ' ' || p[n] == '\0'))
			return 1;
	}
	return 0;
}

/* an untagged response: what the client keeps of it, then cb */
static void untagged(struct imap *m, imap_cb cb, void *ctx)
{
	char *s = m->buf + 2, *e = m->buf + m->len;
	unsigned long n;
	long first;
	struct ival *v, *w;

	if (ci_prefix(s, "CAPABILITY "))
		take_caps(m, s + 11);
	else if (ci_prefix(s, "OK [CAPABILITY "))
		take_caps(m, s + 15);
	first = imap_parse(m, s, e, &n);
	if (first < 0)
		return;
	v = &m->iv[first];
	w = iv_next(m, v);
	if (w && iv_is(w, "EXISTS"))
		m->exists = iv_num(v);
	if (iv_is(v, "OK") && w && w->type == IV_LIST) {
		struct ival *code = iv_at(m, w, 0), *val = iv_at(m, w, 1);

		if (iv_is(code, "UIDVALIDITY"))
			m->uidvalidity = iv_num(val);
		else if (iv_is(code, "UIDNEXT"))
			m->uidnext = iv_num(val);
	}
	if (cb)
		cb(ctx, m, v, n);
}

/* responses up to the one tagged t: 0 for its OK, -1 (m->err) */
static int wait_tagged(struct imap *m, const char *t, imap_cb cb, void *ctx)
{
	size_t tl = strlen(t);

	for (;;) {
		if (read_resp(m) < 0)
			return -1;
		if (m->buf[0] == '*' && m->buf[1] == ' ')
			untagged(m, cb, ctx);
		else if (strncmp(m->buf, t, tl) == 0 && m->buf[tl] == ' ') {
			const char *r = m->buf + tl + 1;

			if (ci_prefix(r, "OK")) {
				if (ci_prefix(r, "OK [CAPABILITY "))
					take_caps(m, r + 15);
				return 0;
			}
			/* (the server's words: NO [AUTHENTICATIONFAILED] ...) */
			snprintf(m->err, sizeof m->err, "%s", r);
			return -1;
		}
		/* (anything else - a continuation not asked for - is passed) */
	}
}

/* the command line: tag, words, CRLF, into line; its length or -1 */
static int build(struct imap *m, char *line, size_t n, char *t, const char *fmt,
	va_list ap)
{
	int k;

	sprintf(t, "A%04u", ++m->tag % 10000);
	k = snprintf(line, n, "%s ", t);
	k += vsnprintf(line + k, n - (size_t)k - 2, fmt, ap);
	if (k < 0 || (size_t)k >= n - 2)
		return -1;
	line[k++] = '\r';
	line[k++] = '\n';
	return k;
}

int imap_vcmd(struct imap *m, imap_cb cb, void *ctx, const char *fmt, va_list ap)
{
	char line[1200], t[12];
	int k, rc;

	if (m->c == NULL)
		return fail(m, "not connected", NULL);
	k = build(m, line, sizeof line, t, fmt, ap);
	if (k < 0)
		return fail(m, "a command too long", NULL);
	rc = conn_write(m->c, line, (size_t)k);
	/* (it may have held a login: not left in memory) */
	memset(line, 0, sizeof line);
	if (rc < 0) {
		drop(m);
		return fail(m, "can't write to the server", NULL);
	}
	return wait_tagged(m, t, cb, ctx);
}

int imap_cmd(struct imap *m, imap_cb cb, void *ctx, const char *fmt, ...)
{
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = imap_vcmd(m, cb, ctx, fmt, ap);
	va_end(ap);
	return rc;
}

int imap_quote(char *out, size_t n, const char *s)
{
	size_t o = 0;

	if (n < 3)
		return -1;
	out[o++] = '"';
	for (; *s; s++) {
		if (*s == '\r' || *s == '\n')
			return -1;
		if (*s == '"' || *s == '\\') {
			if (o + 3 >= n)
				return -1;
			out[o++] = '\\';
		}
		if (o + 2 >= n)
			return -1;
		out[o++] = *s;
	}
	out[o++] = '"';
	out[o] = '\0';
	return 0;
}

int imap_open(struct imap *m, const char *host, unsigned port, int tls)
{
	char err[256];

	memset(m, 0, sizeof *m);
	if (!tls && strcmp(host, "127.0.0.1") != 0 && strcmp(host, "localhost") != 0)
		return fail(m, "IMAP without TLS only to this machine: a password "
			"would cross the network as it is", NULL);
	if ((m->c = conn_open(host, port, tls, 0, err, sizeof err)) == NULL)
		return fail(m, "%s", err);
	if (read_resp(m) < 0)
		return -1;
	if (!ci_prefix(m->buf, "* OK") && !ci_prefix(m->buf, "* PREAUTH")) {
		snprintf(m->err, sizeof m->err, "the server said: %s", m->buf);
		return -1;
	}
	if (ci_prefix(m->buf, "* OK [CAPABILITY "))
		take_caps(m, m->buf + 17);
	if (!m->caps[0] && imap_cmd(m, NULL, NULL, "CAPABILITY") < 0)
		return -1;
	return 0;
}

int imap_auth(struct imap *m, const char *user, const char *pass)
{
	int rc;

	if (has_cap(m, "AUTH=PLAIN")) {
		/* SASL PLAIN: NUL user NUL password, in base64 */
		size_t ul = strlen(user), pl = strlen(pass), n = ul + pl + 2;
		unsigned char *raw = xmalloc(n);
		char *b = xmalloc(4 * (n / 3 + 1) + 1);

		if (raw == NULL || b == NULL) {
			xfree(raw);
			xfree(b);
			return fail(m, "out of memory", NULL);
		}
		raw[0] = '\0';
		memcpy(raw + 1, user, ul);
		raw[1 + ul] = '\0';
		memcpy(raw + 2 + ul, pass, pl);
		b64_encode(raw, n, b);
		memset(raw, 0, n);
		xfree(raw);
		if (has_cap(m, "SASL-IR"))
			rc = imap_cmd(m, NULL, NULL, "AUTHENTICATE PLAIN %s", b);
		else {
			/* the command, the server's "+", then the answer */
			char line[64], t[12], *ans;
			size_t bl = strlen(b);

			rc = -1;
			sprintf(t, "A%04u", ++m->tag % 10000);
			sprintf(line, "%s AUTHENTICATE PLAIN\r\n", t);
			if (conn_write(m->c, line, strlen(line)) == 0 && read_resp(m) == 0
				&& m->buf[0] == '+' && (ans = xmalloc(bl + 3)) != NULL) {
				memcpy(ans, b, bl);
				memcpy(ans + bl, "\r\n", 2);
				rc = conn_write(m->c, ans, bl + 2);
				memset(ans, 0, bl + 2);
				xfree(ans);
				if (rc < 0)
					drop(m);
				rc = rc == 0 ? wait_tagged(m, t, NULL, NULL) : fail(m,
					"can't write to the server", NULL);
			} else if (m->err[0] == '\0')
				fail(m, "the server didn't take AUTHENTICATE", NULL);
		}
		memset(b, 0, strlen(b));
		xfree(b);
		return rc;
	}
	if (!has_cap(m, "LOGINDISABLED")) {
		char qu[300], qp[300];

		if (imap_quote(qu, sizeof qu, user) < 0 || imap_quote(qp, sizeof qp, pass) < 0)
			return fail(m, "a user or password IMAP can't send", NULL);
		rc = imap_cmd(m, NULL, NULL, "LOGIN %s %s", qu, qp);
		memset(qp, 0, sizeof qp);
		return rc;
	}
	if (has_cap(m, "AUTH=XOAUTH2") || has_cap(m, "AUTH=OAUTHBEARER"))
		return fail(m, "the server takes only OAuth2 logins (as Outlook and Microsoft "
			"365 do), not a password: manxmail can't log in to it", NULL);
	return fail(m, "the server takes no login this client knows", NULL);
}

int imap_select(struct imap *m, const char *mbox, int readonly)
{
	char q[600];

	if (imap_quote(q, sizeof q, mbox) < 0)
		return fail(m, "a mailbox name IMAP can't send", NULL);
	m->exists = m->uidvalidity = m->uidnext = 0;
	return imap_cmd(m, NULL, NULL, readonly ? "EXAMINE %s" : "SELECT %s", q);
}

void imap_close(struct imap *m)
{
	if (m->c) {
		imap_cmd(m, NULL, NULL, "LOGOUT");
		conn_release(m->c, 0);
	}
	xfree(m->buf);
	xfree(m->iv);
	memset(m, 0, sizeof *m);
}
