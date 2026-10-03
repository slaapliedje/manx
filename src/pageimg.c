/*
 * pageimg.c - the page's images (pageimg.h).
 *
 * Every <img> with a src gets an entry when the page arrives, in document
 * order (node order). An entry's file is fetched whole and kept, its size
 * read from the header (img_probe), and it is decoded through
 * image/pixels to the screen at the size the layout gives it, as many
 * times as that size changes. Kept files are dropped past PIMG_HOLD bytes
 * once their image is on the screen (fetched again only if the size
 * changes). Images up to PIMG_CACHE_MAX go in the disk cache, so that a
 * page seen again (Back) takes them from there: a fresh copy as it is, a
 * stale one checked with the server first (If-None-Match /
 * If-Modified-Since: a 304 renews it).
 *
 * Order: the images shown first (decoded if fetched, else fetched); then
 * those whose size isn't known yet, in document order (until it is, the
 * layout shows their alt text, and they may be anywhere); then the rest,
 * nearest the lines shown first.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "os.h"
#include "fetch.h"
#include "cache.h"
#include "tags.h"
#include "image.h"
#include "pixels.h"
#include "screen.h"
#include "pageimg.h"

#define PIMG_MAX_FILE	(1536UL * 1024)	/* no image file bigger */
#define PIMG_HOLD	(2048UL * 1024)	/* files kept, all together */
#define PIMG_ROWS_POLL	8		/* rows decoded between polls */
#define PIMG_CACHE_MAX	(256UL * 1024)	/* bigger ones: not kept on disk
					 * (the cache is pages' too) */

enum { P_NEW, P_HAVE, P_FAILED, P_SKIP };

struct pent {
	nodeid node;
	const char *src;		/* the doc's */
	unsigned char state;
	unsigned char bad;		/* decoding at bw x bh failed: not again */
	unsigned char whole;		/* scr has every row */
	unsigned char held;		/* data counted in g_held */
	unsigned char tries;		/* fetches that got no answer at all */
	int iw, ih;			/* its own size (0: not known yet) */
	unsigned char *data;		/* the file (P_HAVE) */
	size_t len, cap;
	void *scr;			/* on the screen, sw x sh */
	int sw, sh, bw, bh;
	unsigned gen;			/* g_gen: laid out as an image */
};

static struct pent *g_e;
static int g_n;
static const struct doc *g_d;
static struct url g_base;
static char g_referer[URL_MAX];
static size_t g_held;
static unsigned g_gen;

/* --- the entries ------------------------------------------------------------ */

static void drop_data(struct pent *e)
{
	if (e->held)
		g_held -= e->len < g_held ? e->len : g_held;
	xfree(e->data);
	e->data = NULL;
	e->len = e->cap = 0;
	e->held = 0;
}

static void drop_scr(struct pent *e)
{
	if (e->scr)
		scr_image_free(e->scr);
	e->scr = NULL;
	e->sw = e->sh = 0;
	e->whole = 0;
}

void pimg_end(void)
{
	int i;

	for (i = 0; i < g_n; i++) {
		drop_data(&g_e[i]);
		drop_scr(&g_e[i]);
	}
	xfree(g_e);
	g_e = NULL;
	g_n = 0;
	g_d = NULL;
	g_held = 0;
}

/* a width/height attribute of 1 or less: a tracking pixel */
static int tiny(const char *v)
{
	return v && (strcmp(v, "0") == 0 || strcmp(v, "1") == 0
		|| strcmp(v, "0px") == 0 || strcmp(v, "1px") == 0);
}

void pimg_begin(const struct doc *d, const struct url *base, const char *page_url)
{
	unsigned long id;
	int n = 0;

	pimg_end();
	if (scr_pixels() == NULL)
		return;
	g_d = d;
	g_base = *base;
	snprintf(g_referer, sizeof g_referer, "%s", page_url ? page_url : "");
	for (id = 2; id < d->nnodes; id++)
		if (d->nodes[id].type == NODE_ELEM
			&& (d->nodes[id].tag == TAG_IMG || d->nodes[id].tag == TAG_IMAGE))
			n++;
	if (n == 0)
		return;
	if (n > LAYOUT_MAX_IMAGES)
		n = LAYOUT_MAX_IMAGES;
	if ((g_e = xmalloc((size_t)n * sizeof *g_e)) == NULL)
		return;
	memset(g_e, 0, (size_t)n * sizeof *g_e);
	for (id = 2; id < d->nnodes && g_n < n; id++) {
		struct pent *e;
		const char *src;

		if (d->nodes[id].type != NODE_ELEM
			|| (d->nodes[id].tag != TAG_IMG && d->nodes[id].tag != TAG_IMAGE))
			continue;
		e = &g_e[g_n++];
		e->node = (nodeid)id;
		src = doc_attr(d, (nodeid)id, ATTR_SRC);
		e->src = src;
		if (src == NULL || *src == '\0'
			|| (tiny(doc_attr(d, (nodeid)id, ATTR_WIDTH))
			&& tiny(doc_attr(d, (nodeid)id, ATTR_HEIGHT))))
			e->state = P_SKIP;
	}
}

static struct pent *find(nodeid node)
{
	int lo = 0, hi = g_n - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (g_e[mid].node == node)
			return &g_e[mid];
		if (g_e[mid].node < node)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

int pimg_size(nodeid node, int *w, int *h)
{
	struct pent *e = find(node);

	if (e == NULL || e->iw <= 0 || e->ih <= 0 || e->state == P_FAILED)
		return 0;
	*w = e->iw;
	*h = e->ih;
	return 1;
}

void *pimg_screen(const struct page *p, long k)
{
	struct pent *e;

	if (k < 0 || k >= (long)p->nimages || (e = find(p->images[k].node)) == NULL)
		return NULL;
	if (e->scr && e->sw == p->images[k].w && e->sh == p->images[k].h)
		return e->scr;
	return NULL;
}

void pimg_cancel(void)
{
	int i;

	for (i = 0; i < g_n; i++)
		if (g_e[i].state == P_NEW && g_e[i].data == NULL && g_e[i].iw == 0)
			g_e[i].state = P_SKIP;
}

void pimg_count(int *total, int *done)
{
	int i;

	*total = *done = 0;
	for (i = 0; i < g_n; i++) {
		if (g_e[i].state == P_SKIP)
			continue;
		(*total)++;
		if (g_e[i].state != P_NEW || g_e[i].iw > 0)
			(*done)++;
	}
}

/* --- fetching ---------------------------------------------------------------- */

struct fetching {
	struct pent *e;
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

/* the headers the disk cache wants */
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

static int append(struct pent *e, const unsigned char *b, size_t n)
{
	if (e->len + n > PIMG_MAX_FILE)
		return -1;
	if (e->len + n > e->cap) {
		size_t c = e->cap ? e->cap * 2 : 16384;
		unsigned char *q;

		while (c < e->len + n)
			c *= 2;
		if (c > PIMG_MAX_FILE)
			c = PIMG_MAX_FILE;
		if ((q = xrealloc(e->data, c)) == NULL)
			return -1;
		e->data = q;
		e->cap = c;
	}
	memcpy(e->data + e->len, b, n);
	e->len += n;
	return 0;
}

static int on_body(void *ctx, const unsigned char *b, size_t n)
{
	struct fetching *f = ctx;

	if (append(f->e, b, n) < 0) {
		f->too_big = 1;
		return -1;
	}
	if (f->poll(f->ctx, 0) < 0) {
		f->stopped = 1;
		return -1;
	}
	return 0;
}

static void on_reset(void *ctx)
{
	struct fetching *f = ctx;

	f->e->len = 0;
}

static int b64(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+' || c == '-')
		return 62;
	if (c == '/' || c == '_')
		return 63;
	return -1;
}

/* a data: URL's bytes (base64 only: the images worth showing are) */
static int data_url(struct pent *e, const char *s)
{
	const char *comma = strchr(s, ',');
	unsigned long acc = 0;
	int bits = 0;

	if (comma == NULL || comma - s < 7 || strncmp(comma - 7, ";base64", 7) != 0)
		return -1;
	for (s = comma + 1; *s; s++) {
		int v = b64((unsigned char)*s);
		unsigned char c;

		if (v < 0)
			continue;	/* (white space, padding) */
		acc = acc << 6 | (unsigned long)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			c = (unsigned char)(acc >> bits);
			if (append(e, &c, 1) < 0)
				return -1;
		}
	}
	return 0;
}

/* a cached copy's bytes, into the entry */
static int cached_body(void *ctx, const unsigned char *b, size_t n)
{
	return append(ctx, b, n);
}

/* e's file into the disk cache under key, fresh as f's headers say (old:
 * the cached copy's, for validators a 304 didn't repeat) */
static void keep(const char *key, const char *final_url, const struct pent *e,
	const struct fetching *f, const struct cache_meta *old)
{
	static struct cache_meta m;
	int no_store;
	long now = (long)time(NULL);

	if (e->len == 0 || e->len > PIMG_CACHE_MAX)
		return;
	memset(&m, 0, sizeof m);
	m.fresh_until = cache_freshness(f->cc[0] ? f->cc : NULL,
		f->expires[0] ? f->expires : NULL, f->date[0] ? f->date : NULL, now,
		&no_store);
	if (no_store) {
		cache_remove(key);
		return;
	}
	if (cache_begin(key) < 0)
		return;
	cache_write(e->data, e->len);
	snprintf(m.url, sizeof m.url, "%s", key);
	snprintf(m.location, sizeof m.location, "%s", final_url);
	snprintf(m.type, sizeof m.type, "%s", f->ctype[0] || !old ? f->ctype : old->type);
	m.stored = now;
	snprintf(m.etag, sizeof m.etag, "%s", f->etag[0] || !old ? f->etag : old->etag);
	snprintf(m.last_modified, sizeof m.last_modified, "%s",
		f->lastmod[0] || !old ? f->lastmod : old->last_modified);
	m.size = (long)e->len;
	cache_commit(&m);
}

static int fetch_one(struct pent *e, int (*poll)(void *ctx, int shown), void *ctx)
{
	static struct cache_meta m;
	struct fetching f;
	struct fetch_cb cb;
	struct fetch_opts opts;
	struct fetch_result *res;
	struct url u;
	char url[URL_MAX], final[URL_MAX], cond[400];
	int rc, cached = 0;

	drop_data(e);
	if (strncmp(e->src, "data:", 5) == 0) {
		rc = data_url(e, e->src);
	} else {
		if (url_resolve(&g_base, e->src, &u) < 0
			|| url_format(&u, url, sizeof url, 0) < 0
			|| (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)
			&& strncmp(url, "file://", 7))) {
			e->state = P_FAILED;
			return PIMG_WORKED;
		}
		memset(&opts, 0, sizeof opts);
		/* the disk cache: a fresh copy is all it takes; a stale one
		 * may only need the server's word that it is still good */
		if (strncmp(url, "file:", 5) != 0 && cache_lookup(url, &m)) {
			cached = 1;
			if (m.fresh_until > (long)time(NULL)
				&& cache_read(url, &m, cached_body, e) == 0 && e->len > 0) {
				rc = 0;
				goto have;
			}
			drop_data(e);
			cond[0] = '\0';
			if (m.etag[0])
				snprintf(cond, sizeof cond, "If-None-Match: %s\r\n", m.etag);
			else if (m.last_modified[0])
				snprintf(cond, sizeof cond, "If-Modified-Since: %s\r\n",
					m.last_modified);
			if (cond[0])
				opts.extra = cond;
		}
		memset(&f, 0, sizeof f);
		f.e = e;
		f.status = 200;
		f.poll = poll;
		f.ctx = ctx;
		memset(&cb, 0, sizeof cb);
		cb.ctx = &f;
		cb.head = on_head;
		cb.header = on_header;
		cb.body = on_body;
		cb.reset = on_reset;
		opts.referer = g_referer[0] ? g_referer : NULL;
		/* (a fetch_result is big: not on the stack) */
		if ((res = xmalloc(sizeof *res)) == NULL)
			return PIMG_WORKED;
		rc = fetch_ex(url, "GET", &opts, &cb, res);
		snprintf(final, sizeof final, "%s", res->url[0] ? res->url : url);
		xfree(res);
		if (f.stopped) {
			drop_data(e);		/* (from the start next time) */
			return PIMG_STOPPED;
		}
		if (rc < 0 && !f.answered && ++e->tries < 3) {
			/* no answer at all (the network gone a moment, as a
			 * TT's WiFi can): again later, not given up */
			drop_data(e);
			return PIMG_WORKED;
		}
		if (rc == 0 && f.status == 304 && cached) {
			/* not changed: the cached copy, good for longer */
			drop_data(e);
			if (cache_read(url, &m, cached_body, e) == 0 && e->len > 0)
				keep(url, m.location[0] ? m.location : final, e, &f, &m);
			else
				rc = -1;
		} else if (f.too_big || f.status != 200)
			rc = -1;
		else if (rc == 0 && strncmp(url, "file:", 5) != 0)
			keep(url, final, e, &f, NULL);
	}
have:
	if (rc < 0 || !img_probe(e->data, e->len, &e->iw, &e->ih)) {
		drop_data(e);
		e->iw = e->ih = 0;
		e->state = P_FAILED;
		return PIMG_WORKED;
	}
	g_held += e->len;
	e->held = 1;
	e->state = P_HAVE;
	return PIMG_WORKED;
}

/* --- decoding ------------------------------------------------------------------ */

struct decoding {
	struct pent *e;
	int rows;
	int stopped;
	int (*poll)(void *ctx, int shown);
	void *ctx;
};

static int dec_size(void *ctx, int w, int h, int masked)
{
	struct decoding *g = ctx;

	if ((g->e->scr = scr_image_new(w, h, masked)) == NULL)
		return -1;
	g->e->sw = w;
	g->e->sh = h;
	return 0;
}

static int dec_row(void *ctx, int y, const unsigned char *px, const unsigned char *mask)
{
	struct decoding *g = ctx;

	scr_image_row(g->e->scr, y, px, mask);
	if (++g->rows % PIMG_ROWS_POLL == 0 && g->poll(g->ctx, 1) < 0) {
		g->stopped = 1;
		return -1;
	}
	return 0;
}

static int decode_one(const struct page *p, long k, struct pent *e,
	int (*poll)(void *ctx, int shown), void *ctx)
{
	struct decoding g;
	struct px_out o;
	struct img_sink s;
	struct img_dec *d;
	int r = IMG_OK;

	drop_scr(e);
	memset(&g, 0, sizeof g);
	g.e = e;
	g.poll = poll;
	g.ctx = ctx;
	memset(&o, 0, sizeof o);
	o.fmt = scr_pixels();
	o.want_w = p->images[k].w;
	o.want_h = p->images[k].h;
	o.size = dec_size;
	o.row = dec_row;
	o.ctx = &g;
	px_sink(&o, &s);
	/* (a JPEG keeps its whole file: room for that and its state) */
	d = img_new(img_sniff(e->data, e->len), &s, e->len + 512UL * 1024);
	if (d == NULL)
		r = IMG_BAD;
	else {
		r = img_feed(d, e->data, e->len);
		if (r == IMG_OK)
			r = img_finish(d);
		img_free(d);
	}
	if (!g.stopped && px_finish(&o) < 0)
		g.stopped = 1;
	px_free(&o);
	if (g.stopped) {
		/* the rows so far stay on the screen; decoded again later */
		e->whole = 0;
		return PIMG_STOPPED;
	}
	/* whole, or as much as there was of a bad one: not again */
	e->whole = 1;
	if (r != IMG_END) {
		e->bad = 1;
		e->bw = (int)p->images[k].w;
		e->bh = (int)p->images[k].h;
		if (e->scr == NULL) {
			/* nothing of it to show (a progressive JPEG): its
			 * alt text instead, so lay out again */
			drop_data(e);
			e->state = P_FAILED;
			return PIMG_SIZED;
		}
	}
	if (g_held > PIMG_HOLD) {
		/* (only fetched again if its size changes) */
		drop_data(e);
		e->state = P_NEW;
	}
	return e->scr ? PIMG_SHOWN : PIMG_WORKED;
}

/* --- the order -------------------------------------------------------------------- */

/* page image k needs: 1 decoding, 2 fetching, 0 nothing */
static int needs(const struct page *p, long k, struct pent **ep)
{
	struct pent *e = find(p->images[k].node);

	*ep = e;
	if (e == NULL || e->state == P_SKIP || e->state == P_FAILED)
		return 0;
	if (e->scr && e->sw == p->images[k].w && e->sh == p->images[k].h && e->whole)
		return 0;
	if (e->bad && e->bw == p->images[k].w && e->bh == p->images[k].h)
		return 0;
	if (e->state == P_HAVE)
		return 1;
	return 2;
}

static int work_on(const struct page *p, long k, int (*poll)(void *ctx, int shown),
	void *ctx)
{
	struct pent *e;

	switch (needs(p, k, &e)) {
	case 1:
		e->bad = 0;
		return decode_one(p, k, e, poll, ctx);
	case 2:
		return fetch_one(e, poll, ctx);
	}
	return -1;
}

int pimg_step(const struct page *p, long top, long rows,
	int (*poll)(void *ctx, int shown), void *ctx)
{
	long k, best = -1, bestd = 0;
	unsigned long ln;
	int i, r;

	if (g_n == 0 || p->images == NULL)
		return PIMG_IDLE;
	/* which are laid out as images (the others show alt text) */
	g_gen++;
	for (k = 0; k < (long)p->nimages; k++) {
		struct pent *e = find(p->images[k].node);

		if (e)
			e->gen = g_gen;
	}
	/* those shown */
	for (k = 0; k < (long)p->nimages; k++) {
		ln = p->images[k].line;
		if ((long)ln >= top && (long)ln < top + rows
			&& (r = work_on(p, k, poll, ctx)) >= 0)
			return r;
	}
	/* those of no known size yet, shown as alt text: once it is known,
	 * the page is laid out again */
	for (i = 0; i < g_n; i++)
		if (g_e[i].state == P_NEW && g_e[i].iw == 0 && g_e[i].gen != g_gen) {
			r = fetch_one(&g_e[i], poll, ctx);
			return r == PIMG_WORKED && g_e[i].state == P_HAVE ? PIMG_SIZED : r;
		}
	/* the rest, nearest first */
	for (k = 0; k < (long)p->nimages; k++) {
		struct pent *e;
		long d;

		if (needs(p, k, &e) == 0)
			continue;
		ln = p->images[k].line;
		d = (long)ln < top ? top - (long)ln : (long)ln - top;
		if (best < 0 || d < bestd) {
			best = k;
			bestd = d;
		}
	}
	if (best >= 0 && (r = work_on(p, best, poll, ctx)) >= 0)
		return r;
	return PIMG_IDLE;
}
