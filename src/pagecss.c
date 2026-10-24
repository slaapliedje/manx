/*
 * pagecss.c - the page's linked style sheets (pagecss.h).
 *
 * A sheet is kept whole (up to PCSS_MAX_FILE) as it comes, so that one
 * cut short by a key is dropped and fetched again later rather than read
 * in part, then put in the disk cache and read into the page's sheet at
 * the place of its <link> in the page. Sites share their sheets between
 * pages: from the cache, a sheet costs only its reading.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "os.h"
#include "fetch.h"
#include "cache.h"
#include "tags.h"
#include "css.h"
#include "pagecss.h"

#define PCSS_MAX_SHEETS	16
#define PCSS_MAX_FILE	(384UL * 1024)

enum { S_NEW, S_DONE };

struct psheet {
	nodeid node;			/* the <link> */
	const char *href, *media;	/* the doc's */
	int state;
	int tries;			/* fetches that got no answer at all */
};

static struct psheet g_s[PCSS_MAX_SHEETS];
static int g_n;
static struct doc *g_d;
static struct url g_base;
static int g_local;		/* the page is a file: subresources may be */
static char g_referer[URL_MAX];

/* does rel name a style sheet (and not an alternate one)? */
static int is_sheet(const char *rel)
{
	const char *p = rel;
	int sheet = 0, alt = 0;

	while (p && *p) {
		const char *w;
		size_t n, i;
		char low[16];

		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		w = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
		n = (size_t)(p - w);
		if (n == 0 || n >= sizeof low)
			continue;
		for (i = 0; i < n; i++)
			low[i] = (char)(w[i] >= 'A' && w[i] <= 'Z' ? w[i] + 32 : w[i]);
		low[n] = '\0';
		if (strcmp(low, "stylesheet") == 0)
			sheet = 1;
		else if (strcmp(low, "alternate") == 0)
			alt = 1;
	}
	return sheet && !alt;
}

void pcss_end(void)
{
	g_n = 0;
	g_d = NULL;
}

void pcss_begin(struct doc *d, const struct url *base, const char *page_url)
{
	unsigned long id;

	pcss_end();
	g_d = d;
	g_base = *base;
	snprintf(g_referer, sizeof g_referer, "%s", page_url ? page_url : "");
	/* (a page from the network gets no local files: a fifo or a tty
	 * would hang the reading - <base href> can't change that) */
	g_local = strncmp(g_referer, "file:", 5) == 0;
	for (id = 2; id < d->nnodes && g_n < PCSS_MAX_SHEETS; id++) {
		const char *rel, *href, *media;

		if (d->nodes[id].type != NODE_ELEM || d->nodes[id].tag != TAG_LINK)
			continue;
		rel = doc_attr(d, (nodeid)id, ATTR_REL);
		href = doc_attr(d, (nodeid)id, ATTR_HREF);
		media = doc_attr(d, (nodeid)id, ATTR_MEDIA);
		if (rel == NULL || href == NULL || *href == '\0' || !is_sheet(rel))
			continue;
		g_s[g_n].node = (nodeid)id;
		g_s[g_n].href = href;
		g_s[g_n].media = media;
		g_s[g_n].state = S_NEW;
		g_s[g_n].tries = 0;
		g_n++;
	}
}

void pcss_count(int *total, int *done)
{
	int i;

	*total = g_n;
	*done = 0;
	for (i = 0; i < g_n; i++)
		if (g_s[i].state == S_DONE)
			(*done)++;
}

void pcss_cancel(void)
{
	int i;

	for (i = 0; i < g_n; i++)
		g_s[i].state = S_DONE;
}

/* --- one sheet ---------------------------------------------------------------- */

struct fetching {
	unsigned char *buf;
	size_t len, cap;
	int status, too_big, stopped, answered;
	char cc[200], expires[64], date[64], etag[128], lastmod[64], ctype[128];
	int (*poll)(void *ctx, int shown);
	void *ctx;
};

static void on_head(void *ctx, int status, const char *ctype, const char *charset,
	const char *url)
{
	struct fetching *f = ctx;

	(void)charset;
	(void)url;
	f->status = status;
	f->answered = 1;
	snprintf(f->ctype, sizeof f->ctype, "%s", ctype ? ctype : "");
}

static void on_header(void *ctx, const char *name, const char *value)
{
	struct fetching *f = ctx;
	char low[32];
	size_t i;

	for (i = 0; name[i] && i < sizeof low - 1; i++)
		low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
	low[i] = '\0';
	if (strcmp(low, "cache-control") == 0)
		snprintf(f->cc, sizeof f->cc, "%s", value);
	else if (strcmp(low, "expires") == 0)
		snprintf(f->expires, sizeof f->expires, "%s", value);
	else if (strcmp(low, "date") == 0)
		snprintf(f->date, sizeof f->date, "%s", value);
	else if (strcmp(low, "etag") == 0)
		snprintf(f->etag, sizeof f->etag, "%s", value);
	else if (strcmp(low, "last-modified") == 0)
		snprintf(f->lastmod, sizeof f->lastmod, "%s", value);
}

static int on_body(void *ctx, const unsigned char *b, size_t n)
{
	struct fetching *f = ctx;

	if (f->len + n > PCSS_MAX_FILE) {
		f->too_big = 1;
		return -1;
	}
	if (f->len + n > f->cap) {
		size_t c = f->cap ? f->cap * 2 : 16384;
		unsigned char *q;

		while (c < f->len + n)
			c *= 2;
		if (c > PCSS_MAX_FILE)
			c = PCSS_MAX_FILE;
		if ((q = xrealloc(f->buf, c)) == NULL) {
			f->too_big = 1;
			return -1;
		}
		f->buf = q;
		f->cap = c;
	}
	memcpy(f->buf + f->len, b, n);
	f->len += n;
	if (f->poll(f->ctx, 0) < 0) {
		f->stopped = 1;
		return -1;
	}
	return 0;
}

static void on_reset(void *ctx)
{
	struct fetching *f = ctx;

	f->len = 0;
}

/* a cached copy's bytes, into the page's sheet */
static int from_cache(void *ctx, const unsigned char *b, size_t n)
{
	(void)ctx;
	css_feed(g_d->sheet, (const char *)b, n);
	return 0;
}

static void read_into_sheet(const struct psheet *p, const unsigned char *b, size_t n,
	const char *cache_key)
{
	static struct cache_meta m;

	if (g_d->sheet == NULL && (g_d->sheet = css_new()) == NULL)
		return;
	css_begin(g_d->sheet, p->media, p->node);
	if (cache_key)
		cache_read(cache_key, &m, from_cache, NULL);
	else if (b)		/* (NULL: an empty body) */
		css_feed(g_d->sheet, (const char *)b, n);
	css_end(g_d->sheet);
}

static void keep(const char *key, const char *final_url, struct fetching *f)
{
	static struct cache_meta m;
	int no_store;
	long now = (long)time(NULL);

	memset(&m, 0, sizeof m);
	m.fresh_until = cache_freshness(f->cc[0] ? f->cc : NULL,
		f->expires[0] ? f->expires : NULL, f->date[0] ? f->date : NULL, now,
		&no_store);
	if (no_store || cache_begin(key) < 0)
		return;
	cache_write(f->buf, f->len);
	snprintf(m.url, sizeof m.url, "%s", key);
	snprintf(m.location, sizeof m.location, "%s", final_url);
	snprintf(m.type, sizeof m.type, "%s", f->ctype);
	m.stored = now;
	snprintf(m.etag, sizeof m.etag, "%s", f->etag);
	snprintf(m.last_modified, sizeof m.last_modified, "%s", f->lastmod);
	m.size = (long)f->len;
	cache_commit(&m);
}

static int hexd(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
		: c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int b64d(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	return c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}

/* is s (n bytes) w, case aside? */
static int same(const char *s, size_t n, const char *w)
{
	size_t i;

	for (i = 0; i < n && w[i]; i++)
		if ((s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]) != w[i])
			return 0;
	return i == n && w[i] == '\0';
}

/* a data: URL's sheet ("data:text/css,a%20%7B...", or ;base64), read at
 * once: there is nothing to fetch */
static void data_sheet(const struct psheet *p)
{
	const char *s = p->href + 5, *comma = strchr(s, ','), *t, *q;
	unsigned char *b;
	unsigned long acc = 0;
	size_t n = 0;
	int bits = 0, base64;

	if (comma == NULL)
		return;
	for (t = s; t < comma && *t != ';'; t++)
		;
	/* (a type, if any, must be CSS's) */
	if (t > s && !same(s, (size_t)(t - s), "text/css"))
		return;
	base64 = comma - s >= 7 && same(comma - 7, 7, ";base64");
	if ((b = xmalloc(strlen(comma) + 1)) == NULL)
		return;
	for (q = comma + 1; *q; q++) {
		if (base64) {
			int v = b64d((unsigned char)*q);

			if (v < 0)
				continue;	/* (white space, padding) */
			acc = acc << 6 | (unsigned long)v;
			if ((bits += 6) >= 8) {
				bits -= 8;
				b[n++] = (unsigned char)(acc >> bits);
			}
		} else if (*q == '%' && hexd((unsigned char)q[1]) >= 0
			&& hexd((unsigned char)q[2]) >= 0) {
			b[n++] = (unsigned char)(hexd((unsigned char)q[1]) << 4
				| hexd((unsigned char)q[2]));
			q += 2;
		} else
			b[n++] = (unsigned char)*q;
	}
	read_into_sheet(p, b, n, NULL);
	xfree(b);
}

int pcss_step(int (*poll)(void *ctx, int shown), void *ctx)
{
	static struct cache_meta m;
	struct psheet *p = NULL;
	struct fetching f;
	struct fetch_cb cb;
	struct fetch_opts opts;
	struct fetch_result *res;
	struct url u;
	char url[URL_MAX];
	int i, rc;

	if (g_d == NULL)
		return PCSS_IDLE;
	for (i = 0; i < g_n && !p; i++)
		if (g_s[i].state == S_NEW)
			p = &g_s[i];
	if (p == NULL)
		return PCSS_IDLE;
	if (same(p->href, 5, "data:")) {
		data_sheet(p);
		p->state = S_DONE;
		return PCSS_READ;
	}
	if (url_resolve(&g_base, p->href, &u) < 0
		|| url_format(&u, url, sizeof url, 0) < 0
		|| (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)
		&& (strncmp(url, "file:", 5) || !g_local))) {
		p->state = S_DONE;
		return PCSS_WORKED;
	}
	/* a fresh copy in the cache: no fetch */
	if (strncmp(url, "file:", 5) != 0 && cache_lookup(url, &m)
		&& m.fresh_until > (long)time(NULL)) {
		read_into_sheet(p, NULL, 0, url);
		p->state = S_DONE;
		return PCSS_READ;
	}
	memset(&f, 0, sizeof f);
	f.status = 200;
	f.poll = poll;
	f.ctx = ctx;
	memset(&cb, 0, sizeof cb);
	cb.ctx = &f;
	cb.head = on_head;
	cb.header = on_header;
	cb.body = on_body;
	cb.reset = on_reset;
	memset(&opts, 0, sizeof opts);
	opts.referer = g_referer[0] ? g_referer : NULL;
	if ((res = xmalloc(sizeof *res)) == NULL)
		return PCSS_WORKED;
	rc = fetch_ex(url, "GET", &opts, &cb, res);
	if (f.stopped) {
		xfree(f.buf);
		xfree(res);
		return PCSS_STOPPED;	/* (from the start next time) */
	}
	if (rc < 0 && !f.answered && ++p->tries < 3) {
		/* no answer at all: again later, not given up */
		xfree(f.buf);
		xfree(res);
		return PCSS_WORKED;
	}
	p->state = S_DONE;
	/* (f.status is the head's, which a body-less answer never gives) */
	if (rc < 0 || f.too_big || f.status != 200 || res->status != 200) {
		xfree(f.buf);
		xfree(res);
		return PCSS_WORKED;
	}
	if (strncmp(url, "file:", 5) != 0)
		keep(url, res->url, &f);
	read_into_sheet(p, f.buf, f.len, NULL);
	xfree(f.buf);
	xfree(res);
	return PCSS_READ;
}
