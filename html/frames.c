/*
 * frames.c - see frames.h.
 */
#include <stdio.h>
#include <string.h>
#include "os.h"
#include "tags.h"
#include "load.h"
#include "frames.h"

struct sbuf {
	char *p;
	size_t n, cap;
	int oom;
};

static void put_n(struct sbuf *b, const char *s, size_t n)
{
	if (b->oom)
		return;
	if (b->n + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 4096;
		char *p;

		while (b->n + n + 1 > cap)
			cap *= 2;
		if ((p = xrealloc(b->p, cap)) == NULL) {
			b->oom = 1;
			return;
		}
		b->p = p;
		b->cap = cap;
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = '\0';
}

static void put(struct sbuf *b, const char *s)
{
	put_n(b, s, strlen(s));
}

/* s as HTML text, or (quote) in an attribute's double quotes */
static void put_esc(struct sbuf *b, const char *s, int quote)
{
	const char *run = s;

	for (; *s; s++) {
		const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;"
			: *s == '>' ? "&gt;" : quote && *s == '"' ? "&quot;" : NULL;

		if (e) {
			put_n(b, run, (size_t)(s - run));
			put(b, e);
			run = s + 1;
		}
	}
	put_n(b, run, (size_t)(s - run));
}

struct compose {
	const char *chosen;
	frames_get get;
	void *ctx;
	struct sbuf head, body, state;
	char title[200];
	int nframes, unnamed;
};

/* the document chosen for frame name, or NULL */
static const char *chosen_url(const char *chosen, const char *name, size_t *len)
{
	size_t nl = strlen(name);
	const char *p = chosen;

	while (p && *p) {
		const char *eol = strchr(p, '\n');

		if (eol == NULL)
			eol = p + strlen(p);
		if (strncmp(p, name, nl) == 0 && p[nl] == '\t') {
			*len = (size_t)(eol - p - nl - 1);
			return p + nl + 1;
		}
		p = *eol ? eol + 1 : eol;
	}
	return NULL;
}

/* ref resolved against base, into out (URL_MAX); 0 or -1 */
static int absolute(const struct url *base, const char *ref, char *out)
{
	struct url *u = xmalloc(sizeof *u);
	int rc = -1;

	if (u && url_resolve(base, ref, u) == URL_OK
		&& url_format(u, out, URL_MAX, 1) == URL_OK)
		rc = 0;
	xfree(u);
	return rc;
}

static int url_attr(int a)
{
	return a == ATTR_HREF || a == ATTR_SRC || a == ATTR_ACTION
		|| a == ATTR_DATA_SRC;
}

/* an element and what's in it, written out again */
static void write_node(struct sbuf *b, const struct doc *d, nodeid id,
	const struct url *base, const char *target, char *ubuf)
{
	const struct node *n = &d->nodes[id];
	const unsigned char *p;
	unsigned f;
	nodeid c;
	int has_target = 0;

	if (n->type == NODE_TEXT) {
		put_esc(b, doc_text(d, id), 0);
		return;
	}
	f = tag_flags(n->tag);
	switch (n->tag) {
	case TAG_SCRIPT: case TAG_STYLE: case TAG_TITLE: case TAG_META:
	case TAG_LINK: case TAG_BASE:
		return;
	}
	if (f & TF_DROP)
		return;
	if (n->tag != TAG_UNKNOWN) {
		put(b, "<");
		put(b, tag_name(n->tag));
		for (p = d->attr + n->data; p < d->attr + d->attr_len && *p;
			p += 2 + strlen((const char *)p + 1)) {
			const char *v = (const char *)p + 1;

			if (*p == ATTR_SRCSET || *p == ATTR_DATA_SRCSET)
				continue;	/* (its pick is in src) */
			if (*p == ATTR_TARGET)
				has_target = 1;
			if (url_attr(*p) && absolute(base, v, ubuf) == 0)
				v = ubuf;
			put(b, " ");
			put(b, attr_name(*p));
			put(b, "=\"");
			put_esc(b, v, 1);
			put(b, "\"");
		}
		if (n->tag == TAG_A && !has_target && doc_attr(d, id, ATTR_HREF)) {
			put(b, " target=\"");
			put_esc(b, target, 1);
			put(b, "\"");
		}
		put(b, ">");
		if (f & TF_VOID)
			return;
	}
	if (!(f & TF_SKIPTEXT) || n->tag == TAG_UNKNOWN)
		for (c = n->first; c; c = d->nodes[c].next)
			write_node(b, d, c, base, target, ubuf);
	if (n->tag != TAG_UNKNOWN) {
		put(b, "</");
		put(b, tag_name(n->tag));
		put(b, ">");
	}
}

/* s starts with lowercase word, in any case */
static int has_word(const unsigned char *s, const char *word)
{
	for (; *word; s++, word++)
		if ((*s >= 'A' && *s <= 'Z' ? *s + 32 : *s) != *word)
			return 0;
	return 1;
}

/* the frame's <style> sheets, as they are in its bytes */
static void copy_styles(struct sbuf *b, const unsigned char *s, size_t n)
{
	size_t i = 0;

	while (i + 7 < n) {
		size_t j, k;

		if (s[i] != '<' || !has_word(s + i + 1, "style")
			|| !(s[i + 6] == '>' || s[i + 6] == ' ' || s[i + 6] == '\t'
			|| s[i + 6] == '\n' || s[i + 6] == '\r')) {
			i++;
			continue;
		}
		for (j = i + 6; j + 7 < n; j++)
			if (s[j] == '<' && s[j + 1] == '/'
				&& has_word(s + j + 2, "style"))
				break;
		if (j + 7 >= n)
			return;
		for (k = j + 7; k < n && s[k] != '>'; k++)
			;
		put_n(b, (const char *)s + i, j - i);
		put(b, "</style>");
		i = k;
	}
}

/* the frame's <link rel=stylesheet>s, absolute */
static void copy_links(struct sbuf *b, const struct doc *d,
	const struct url *base, char *ubuf)
{
	unsigned long id;

	for (id = 2; id < d->nnodes; id++) {
		const char *rel, *href, *media;

		if (d->nodes[id].type != NODE_ELEM || d->nodes[id].tag != TAG_LINK)
			continue;
		rel = doc_attr(d, (nodeid)id, ATTR_REL);
		href = doc_attr(d, (nodeid)id, ATTR_HREF);
		media = doc_attr(d, (nodeid)id, ATTR_MEDIA);
		if (rel == NULL || href == NULL || absolute(base, href, ubuf) < 0)
			continue;
		put(b, "<link rel=\"");
		put_esc(b, rel, 1);
		put(b, "\" href=\"");
		put_esc(b, ubuf, 1);
		if (media) {
			put(b, "\" media=\"");
			put_esc(b, media, 1);
		}
		put(b, "\">");
	}
}

static void frameset(struct compose *c, const struct doc *d, nodeid fs,
	const struct url *base, int depth);

/* a <frame>: the end of its cell's <td ...>, then its document */
static void frame(struct compose *c, const struct doc *d, nodeid fr,
	const struct url *base, int depth)
{
	char name[64], *url = xmalloc(URL_MAX), *ubuf = xmalloc(URL_MAX);
	const char *src = doc_attr(d, fr, ATTR_SRC), *nm = doc_attr(d, fr, ATTR_NAME);
	const char *ch;
	struct frame_bytes *fb = NULL;
	struct html_load *l = NULL;
	struct url *fbase = NULL;
	struct doc fd;
	size_t len, i;
	int have_doc = 0;

	if (url == NULL || ubuf == NULL) {
		c->body.oom = 1;
		goto out;
	}
	if (nm && *nm)
		snprintf(name, sizeof name, "%s", nm);
	else
		snprintf(name, sizeof name, "manx-frame-%d", ++c->unnamed);
	for (i = 0; name[i]; i++)
		if (name[i] == '\t' || name[i] == '\n')
			name[i] = ' ';
	if ((ch = chosen_url(c->chosen, name, &len)) != NULL && len < URL_MAX) {
		memcpy(url, ch, len);
		url[len] = '\0';
	} else if (src == NULL || absolute(base, src, url) < 0) {
		put(&c->body, ">");
		goto out;
	}
	put(&c->state, name);
	put(&c->state, "\t");
	put(&c->state, url);
	put(&c->state, "\n");
	if (++c->nframes > FRAMES_MAX) {
		put(&c->body, "><a href=\"");
		put_esc(&c->body, url, 1);
		put(&c->body, "\">[frame: ");
		put_esc(&c->body, name, 0);
		put(&c->body, "]</a>");
		goto out;
	}
	fb = xmalloc(sizeof *fb);
	if (fb == NULL) {
		c->body.oom = 1;
		goto out;
	}
	memset(fb, 0, sizeof *fb);
	if (c->get(c->ctx, url, fb) < 0) {
		put(&c->body, "><p>[frame ");
		put_esc(&c->body, name, 0);
		put(&c->body, ": ");
		put_esc(&c->body, fb->error, 0);
		put(&c->body, "] <a href=\"");
		put_esc(&c->body, url, 1);
		put(&c->body, "\">");
		put_esc(&c->body, url, 0);
		put(&c->body, "</a>");
		goto out;
	}

	/* its document, relative to its URL or its <base href> */
	l = xmalloc(sizeof *l);
	fbase = xmalloc(sizeof *fbase);
	if (l == NULL || fbase == NULL || doc_init(&fd, 0) < 0) {
		c->body.oom = 1;
		goto out;
	}
	have_doc = 1;
	html_load_begin(l, &fd, fb->charset, 0);
	html_load_feed(l, fb->data, fb->len);
	html_load_end(l);
	if (url_parse(fb->url[0] ? fb->url : url, fbase) != URL_OK)
		*fbase = *base;
	if (l->tree.base_href[0] && absolute(fbase, l->tree.base_href, ubuf) == 0)
		url_parse(ubuf, fbase);

	if (!c->title[0])
		snprintf(c->title, sizeof c->title, "%s", doc_title(&fd));
	copy_links(&c->head, &fd, fbase, ubuf);
	copy_styles(&c->head, fb->data, fb->len);
	if (l->tree.frameset && depth < FRAMES_DEPTH) {
		put(&c->body, ">");
		frameset(c, &fd, l->tree.frameset, fbase, depth + 1);
	} else {
		nodeid body = l->tree.body, k;
		const char *bg = body ? doc_attr(&fd, body, ATTR_BGCOLOR) : NULL;

		if (bg) {
			put(&c->body, " bgcolor=\"");
			put_esc(&c->body, bg, 1);
			put(&c->body, "\"");
		}
		put(&c->body, ">");
		for (k = body ? fd.nodes[body].first : 0; k; k = fd.nodes[k].next)
			write_node(&c->body, &fd, k, fbase, name, ubuf);
	}
out:
	if (have_doc)
		doc_free(&fd);
	if (fb)
		xfree(fb->data);
	xfree(fb);
	xfree(l);
	xfree(fbase);
	xfree(url);
	xfree(ubuf);
}

/* the n'th size of a rows/cols list ("100,*,20%"), as a width, or "" */
static void nth_size(const char *list, int n, char *out, size_t cap)
{
	const char *p = list;
	size_t i = 0;

	out[0] = '\0';
	while (p && n-- > 0 && (p = strchr(p, ',')) != NULL)
		p++;
	if (p == NULL)
		return;
	while (*p == ' ')
		p++;
	while (i + 1 < cap && ((*p >= '0' && *p <= '9') || *p == '%'))
		out[i++] = *p++;
	out[i] = '\0';
	if (*p == '*')
		out[0] = '\0';		/* a share of what's left */
}

static void frameset(struct compose *c, const struct doc *d, nodeid fs,
	const struct url *base, int depth)
{
	const char *rows = doc_attr(d, fs, ATTR_ROWS);
	const char *cols = doc_attr(d, fs, ATTR_COLS);
	int across = cols && !rows, k = 0;
	nodeid ch;

	put(&c->body, "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"4\""
		" border=\"0\">");
	if (across)
		put(&c->body, "<tr>");
	for (ch = d->nodes[fs].first; ch; ch = d->nodes[ch].next) {
		int tag = d->nodes[ch].tag;
		char w[16];

		if (d->nodes[ch].type != NODE_ELEM
			|| (tag != TAG_FRAME && tag != TAG_FRAMESET))
			continue;
		if (!across)
			put(&c->body, "<tr>");
		put(&c->body, "<td valign=\"top\"");
		if (across) {
			nth_size(cols, k, w, sizeof w);
			if (w[0]) {
				put(&c->body, " width=\"");
				put(&c->body, w);
				put(&c->body, "\"");
			}
		}
		if (tag == TAG_FRAME)
			frame(c, d, ch, base, depth);
		else {
			put(&c->body, ">");
			if (depth < FRAMES_DEPTH)
				frameset(c, d, ch, base, depth + 1);
		}
		put(&c->body, "</td>");
		if (!across)
			put(&c->body, "</tr>");
		k++;
	}
	if (across)
		put(&c->body, "</tr>");
	put(&c->body, "</table>");
}

char *frames_compose(const struct doc *d, nodeid fs, const struct url *base,
	const char *chosen, frames_get get, void *ctx, char **state)
{
	struct compose c;
	struct sbuf out;

	memset(&c, 0, sizeof c);
	memset(&out, 0, sizeof out);
	c.chosen = chosen;
	c.get = get;
	c.ctx = ctx;
	snprintf(c.title, sizeof c.title, "%s", doc_title(d));
	frameset(&c, d, fs, base, 1);
	put(&out, "<html><head><title>");
	put_esc(&out, c.title, 0);
	put(&out, "</title>");
	if (c.head.p)
		put(&out, c.head.p);
	put(&out, "</head><body>");
	if (c.body.p)
		put(&out, c.body.p);
	put(&out, "</body></html>");
	xfree(c.head.p);
	xfree(c.body.p);
	if (out.oom || c.body.oom || c.head.oom || c.state.oom) {
		xfree(out.p);
		xfree(c.state.p);
		*state = NULL;
		return NULL;
	}
	*state = c.state.p ? c.state.p : xstrdup("");
	return out.p;
}

char *frames_choose(const char *chosen, const char *name, const char *url)
{
	struct sbuf b;
	const char *p = chosen;
	size_t nl = strlen(name);

	memset(&b, 0, sizeof b);
	while (p && *p) {
		const char *eol = strchr(p, '\n');

		eol = eol ? eol + 1 : p + strlen(p);
		if (!(strncmp(p, name, nl) == 0 && p[nl] == '\t'))
			put_n(&b, p, (size_t)(eol - p));
		p = eol;
	}
	put(&b, name);
	put(&b, "\t");
	put(&b, url);
	put(&b, "\n");
	if (b.oom) {
		xfree(b.p);
		return NULL;
	}
	return b.p;
}

int frames_has(const char *state, const char *name)
{
	size_t len;

	return chosen_url(state, name, &len) != NULL;
}
