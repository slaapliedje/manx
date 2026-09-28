/*
 * url.c - URL parsing and relative resolution, RFC 3986.
 *
 * The parser is the RFC's appendix B regular expression written out by
 * hand; resolution is section 5.2.2 with remove_dot_segments (5.2.4).
 * Components are kept percent-encoded, as they appear on the wire.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "url.h"

static int copy(char *dst, size_t cap, const char *src, size_t len)
{
	if (len >= cap)
		return URL_TOOLONG;
	memcpy(dst, src, len);
	dst[len] = '\0';
	return URL_OK;
}

/*
 * Clean the input the way browsers do: trim white space at both ends,
 * drop tabs and newlines, and encode spaces. The result goes to out.
 */
static int clean(const char *s, char *out, size_t cap)
{
	const char *e;
	size_t n = 0;

	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n'
		|| e[-1] == '\r' || e[-1] == '\f'))
		e--;
	for (; s < e; s++) {
		if (*s == '\t' || *s == '\n' || *s == '\r')
			continue;
		if (*s == ' ') {
			if (n + 3 >= cap)
				return URL_TOOLONG;
			out[n++] = '%';
			out[n++] = '2';
			out[n++] = '0';
			continue;
		}
		if (n + 1 >= cap)
			return URL_TOOLONG;
		out[n++] = *s;
	}
	out[n] = '\0';
	return URL_OK;
}

static void lower(char *s)
{
	for (; *s; s++)
		*s = (char)tolower((unsigned char)*s);
}

int url_parse(const char *in, struct url *u)
{
	static char buf[URL_MAX];
	const char *s, *p;
	int rc;

	memset(u, 0, sizeof *u);
	u->port = -1;
	if ((rc = clean(in, buf, sizeof buf)) != URL_OK)
		return rc;
	s = buf;

	/* scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" */
	if (isalpha((unsigned char)*s)) {
		p = s + 1;
		while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')
			p++;
		if (*p == ':') {
			if ((rc = copy(u->scheme, sizeof u->scheme, s, (size_t)(p - s))) != URL_OK)
				return rc;
			lower(u->scheme);
			s = p + 1;
		}
	}

	/* authority: "//" [ userinfo "@" ] host [ ":" port ] */
	if (s[0] == '/' && s[1] == '/') {
		const char *a = s + 2, *ae = a + strcspn(a, "/?#"), *at, *h, *colon;

		u->has_authority = 1;
		/* userinfo is dropped: never sent, and a phishing aid */
		at = NULL;
		for (p = a; p < ae; p++)
			if (*p == '@')
				at = p;
		h = at ? at + 1 : a;
		colon = NULL;
		if (*h == '[') {		/* IPv6 literal: keep as given */
			const char *rb = memchr(h, ']', (size_t)(ae - h));

			if (rb == NULL)
				return URL_BAD;
			if (rb + 1 < ae && rb[1] == ':')
				colon = rb + 1;
			else if (rb + 1 != ae)
				return URL_BAD;
		} else
			colon = memchr(h, ':', (size_t)(ae - h));
		if ((rc = copy(u->host, sizeof u->host, h,
			(size_t)((colon ? colon : ae) - h))) != URL_OK)
			return rc;
		lower(u->host);
		if (colon && colon + 1 < ae) {
			long port = 0;

			for (p = colon + 1; p < ae; p++) {
				if (!isdigit((unsigned char)*p))
					return URL_BAD;
				port = port * 10 + (*p - '0');
				if (port > 65535)
					return URL_BAD;
			}
			u->port = (int)port;
		}
		s = ae;
	}

	/* path, ?query, #fragment */
	p = s + strcspn(s, "?#");
	if ((rc = copy(u->path, sizeof u->path, s, (size_t)(p - s))) != URL_OK)
		return rc;
	s = p;
	if (*s == '?') {
		p = s + 1 + strcspn(s + 1, "#");
		u->has_query = 1;
		if ((rc = copy(u->query, sizeof u->query, s + 1, (size_t)(p - s - 1))) != URL_OK)
			return rc;
		s = p;
	}
	if (*s == '#') {
		u->has_fragment = 1;
		if ((rc = copy(u->fragment, sizeof u->fragment, s + 1, strlen(s + 1))) != URL_OK)
			return rc;
	}
	return URL_OK;
}

/* RFC 3986 5.2.4, in place */
static void remove_dot_segments(char *path)
{
	char *in = path, *out = path;

	while (*in) {
		if (strncmp(in, "../", 3) == 0)
			in += 3;
		else if (strncmp(in, "./", 2) == 0)
			in += 2;
		else if (strncmp(in, "/./", 3) == 0)
			in += 2;
		else if (strcmp(in, "/.") == 0)
			in[1] = '\0';		/* becomes "/" */
		else if (strncmp(in, "/../", 4) == 0 || strcmp(in, "/..") == 0) {
			if (in[3] == '/')
				in += 3;
			else {
				in += 2;
				*in = '/';	/* "/.." becomes "/" */
			}
			/* drop the last segment of the output */
			while (out > path && *--out != '/')
				;
		} else if (strcmp(in, ".") == 0 || strcmp(in, "..") == 0)
			in += strlen(in);
		else {
			/* move the first segment, with its leading "/" */
			do
				*out++ = *in++;
			while (*in && *in != '/');
		}
	}
	*out = '\0';
}

/* RFC 3986 5.2.3 */
static int merge(const struct url *base, const char *ref_path, char *out,
	size_t cap)
{
	size_t n;

	if (base->has_authority && base->path[0] == '\0') {
		if (strlen(ref_path) + 2 > cap)
			return URL_TOOLONG;
		out[0] = '/';
		strcpy(out + 1, ref_path);
		return URL_OK;
	}
	{
		const char *slash = strrchr(base->path, '/');

		n = slash ? (size_t)(slash - base->path) + 1 : 0;
	}
	if (n + strlen(ref_path) + 1 > cap)
		return URL_TOOLONG;
	memcpy(out, base->path, n);
	strcpy(out + n, ref_path);
	return URL_OK;
}

int url_resolve(const struct url *base, const char *ref, struct url *t)
{
	static struct url r;
	static char tmp[URL_MAX];
	int rc;

	if ((rc = url_parse(ref, &r)) != URL_OK)
		return rc;
	memset(t, 0, sizeof *t);
	t->port = -1;
	/* a reference with the base's own scheme is not treated as relative
	 * (the RFC's strict parser) */
	if (r.scheme[0]) {
		*t = r;
		remove_dot_segments(t->path);
		return URL_OK;
	}
	strcpy(t->scheme, base->scheme);
	if (r.has_authority) {
		t->has_authority = 1;
		strcpy(t->host, r.host);
		t->port = r.port;
		strcpy(t->path, r.path);
		remove_dot_segments(t->path);
		t->has_query = r.has_query;
		strcpy(t->query, r.query);
	} else {
		t->has_authority = base->has_authority;
		strcpy(t->host, base->host);
		t->port = base->port;
		if (r.path[0] == '\0') {
			strcpy(t->path, base->path);
			if (r.has_query) {
				t->has_query = 1;
				strcpy(t->query, r.query);
			} else {
				t->has_query = base->has_query;
				strcpy(t->query, base->query);
			}
		} else {
			if (r.path[0] == '/')
				strcpy(tmp, r.path);
			else if ((rc = merge(base, r.path, tmp, sizeof tmp)) != URL_OK)
				return rc;
			remove_dot_segments(tmp);
			strcpy(t->path, tmp);
			t->has_query = r.has_query;
			strcpy(t->query, r.query);
		}
	}
	t->has_fragment = r.has_fragment;
	strcpy(t->fragment, r.fragment);
	return URL_OK;
}

static int append(char *buf, size_t n, size_t *len, const char *s)
{
	size_t l = strlen(s);

	if (*len + l >= n)
		return URL_TOOLONG;
	memcpy(buf + *len, s, l + 1);
	*len += l;
	return URL_OK;
}

int url_format(const struct url *u, char *buf, size_t n, int with_fragment)
{
	size_t len = 0;
	char port[8];

	if (n == 0)
		return URL_TOOLONG;
	buf[0] = '\0';
	if (u->scheme[0] && (append(buf, n, &len, u->scheme)
		|| append(buf, n, &len, ":")))
		return URL_TOOLONG;
	if (u->has_authority) {
		if (append(buf, n, &len, "//") || append(buf, n, &len, u->host))
			return URL_TOOLONG;
		if (u->port >= 0) {
			snprintf(port, sizeof port, ":%d", u->port);
			if (append(buf, n, &len, port))
				return URL_TOOLONG;
		}
	}
	if (append(buf, n, &len, u->path))
		return URL_TOOLONG;
	if (u->has_query && (append(buf, n, &len, "?")
		|| append(buf, n, &len, u->query)))
		return URL_TOOLONG;
	if (with_fragment && u->has_fragment && (append(buf, n, &len, "#")
		|| append(buf, n, &len, u->fragment)))
		return URL_TOOLONG;
	return URL_OK;
}

unsigned url_port(const struct url *u)
{
	if (u->port >= 0)
		return (unsigned)u->port;
	if (strcmp(u->scheme, "http") == 0)
		return 80;
	if (strcmp(u->scheme, "https") == 0)
		return 443;
	if (strcmp(u->scheme, "gopher") == 0)
		return 70;
	return 0;
}

int url_target(const struct url *u, char *buf, size_t n)
{
	size_t len = 0;

	if (n == 0)
		return URL_TOOLONG;
	buf[0] = '\0';
	if (append(buf, n, &len, u->path[0] ? u->path : "/"))
		return URL_TOOLONG;
	if (u->has_query && (append(buf, n, &len, "?")
		|| append(buf, n, &len, u->query)))
		return URL_TOOLONG;
	return URL_OK;
}
