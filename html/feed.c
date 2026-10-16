/*
 * feed.c - see feed.h.
 *
 * The XML is read by a tokenizer of its own, as the bytes come, any
 * split: the HTML tokenizer would take <link> for a void element and a
 * CDATA section for a bogus comment. Element names are matched by their
 * local part (content:encoded is "encoded"), which is all the namespaces
 * feeds use need. Entities: the five of XML are decoded; numeric ones,
 * and HTML's that feeds use anyway, go on as they are, for the HTML engine
 * to decode - so no character is written in an encoding the page isn't in.
 */
#include <stdio.h>
#include <string.h>
#include "feed.h"

enum { T_TEXT, T_ENT, T_MARKUP, T_COMMENT, T_CDATA, T_PI, T_DOCTYPE, T_PASS };

/* --- output -------------------------------------------------------------- */

static void out_n(struct feed *f, const char *s, size_t n)
{
	if (n)
		f->out(f->ctx, s, n);
}

static void out(struct feed *f, const char *s)
{
	out_n(f, s, strlen(s));
}

/* text, with < > & " escaped */
static void out_esc(struct feed *f, const char *s, size_t n)
{
	size_t i, k = 0;

	for (i = 0; i < n; i++) {
		const char *e = s[i] == '<' ? "&lt;" : s[i] == '>' ? "&gt;"
			: s[i] == '&' ? "&amp;" : s[i] == '"' ? "&quot;" : NULL;

		if (e) {
			out_n(f, s + k, i - k);
			out(f, e);
			k = i + 1;
		}
	}
	out_n(f, s + k, n - k);
}

/* what the HTML engine reads the bytes as */
static void ensure_meta(struct feed *f)
{
	if (f->meta_done)
		return;
	f->meta_done = 1;
	out(f, "<meta charset=\"");
	out_esc(f, f->encoding[0] ? f->encoding : "utf-8",
		strlen(f->encoding[0] ? f->encoding : "utf-8"));
	out(f, "\">");
}

/* --- buffers ------------------------------------------------------------- */

static void put(struct feed *f, char *buf, size_t max, size_t *n, char c)
{
	if (*n + 1 < max)
		buf[(*n)++] = c;
	else if (buf == f->text)
		f->text_cut = 1;
	buf[*n] = '\0';
}

static void put_s(struct feed *f, char *buf, size_t max, size_t *n,
	const char *s)
{
	while (*s)
		put(f, buf, max, n, *s++);
}

/* a character into the capture: as is, or escaped */
static void cap_char(struct feed *f, char c)
{
	const char *e;

	if (f->cap == NULL)
		return;
	e = f->cap_raw ? NULL : c == '<' ? "&lt;" : c == '>' ? "&gt;"
		: c == '&' ? "&amp;" : c == '"' ? "&quot;" : NULL;
	if (e)
		put_s(f, f->cap, f->cap_max, f->cap_n, e);
	else
		put(f, f->cap, f->cap_max, f->cap_n, c);
}

/* an entity the HTML engine will decode: as it is, either way */
static void cap_ref(struct feed *f, const char *s)
{
	if (f->cap)
		put_s(f, f->cap, f->cap_max, f->cap_n, s);
}

static void capture(struct feed *f, char *buf, size_t max, size_t *n,
	int raw, int xhtml)
{
	f->cap = buf;
	f->cap_max = max;
	f->cap_n = n;
	*n = 0;
	buf[0] = '\0';
	f->cap_depth = f->depth;
	f->cap_raw = raw;
	f->cap_xhtml = xhtml;
}

/* trim white space in place; the new length */
static size_t trim(char *s, size_t n)
{
	size_t a = 0;

	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'
		|| s[n - 1] == '\n'))
		n--;
	while (a < n && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'
		|| s[a] == '\n'))
		a++;
	memmove(s, s + a, n - a);
	s[n - a] = '\0';
	return n - a;
}

/* --- attributes ---------------------------------------------------------- */

/* the value of attribute name in a tag's text, entities as text_char
 * would see them: the five decoded, others kept. "" if there's none. */
static void attr(const char *tag, const char *name, char *v, size_t max)
{
	size_t nl = strlen(name), k = 0;
	const char *p = tag;

	v[0] = '\0';
	/* past the element's name */
	while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
		p++;
	while (*p) {
		const char *a, *e;
		char q;

		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		a = p;
		while (*p && *p != '=' && *p != ' ' && *p != '\t' && *p != '\r'
			&& *p != '\n' && *p != '/')
			p++;
		e = p;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (*p != '=') {
			if (*p)
				p++;
			continue;
		}
		p++;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (*p != '"' && *p != '\'')
			continue;
		q = *p++;
		if ((size_t)(e - a) == nl && strncmp(a, name, nl) == 0) {
			while (*p && *p != q && k + 1 < max) {
				if (*p == '&') {
					static const char *const ents[] = { "lt;<", "gt;>",
						"amp;&", "quot;\"", "apos;'" };
					size_t i;

					for (i = 0; i < 5; i++) {
						size_t el = strlen(ents[i]) - 1;

						if (strncmp(p + 1, ents[i], el) == 0) {
							v[k++] = ents[i][el];
							p += el + 1;
							break;
						}
					}
					if (i < 5)
						continue;
				}
				v[k++] = *p++;
			}
			v[k] = '\0';
			return;
		}
		while (*p && *p != q)
			p++;
		if (*p)
			p++;
	}
}

/* --- the feed ------------------------------------------------------------ */

static const char *parent(struct feed *f, int up)
{
	return f->depth - 1 - up >= 0 && f->depth - 1 - up < FEED_DEPTH
		? f->path[f->depth - 1 - up] : "";
}

static void write_head(struct feed *f)
{
	if (f->head_done)
		return;
	f->head_done = 1;
	ensure_meta(f);
	f->title_n = trim(f->title, f->title_n);
	f->link_n = trim(f->link, f->link_n);
	f->sub_n = trim(f->sub, f->sub_n);
	out(f, "<title>");
	out(f, f->title_n ? f->title : "A feed");
	out(f, "</title><h1>");
	out(f, f->title_n ? f->title : "A feed");
	out(f, "</h1>");
	if (f->sub_n && strcmp(f->sub, f->title)) {
		out(f, "<p>");
		out(f, f->sub);
		out(f, "</p>");
	}
	if (f->link_n) {
		out(f, "<p>The site: <a href=\"");
		out(f, f->link);
		out(f, "\">");
		out(f, f->link);
		out(f, "</a></p>");
	}
}

/* "2026-10-16T00:03:16Z" as "2026-10-16 00:03" */
static void tidy_date(char *d)
{
	if (strlen(d) >= 16 && d[4] == '-' && d[7] == '-' && d[10] == 'T'
		&& d[13] == ':') {
		d[10] = ' ';
		d[16] = '\0';
	}
}

/* a title that came as HTML: its text */
static void out_untagged(struct feed *f, const char *s)
{
	const char *p = s;

	while (*p) {
		const char *lt = strchr(p, '<'), *gt;

		if (lt == NULL || (gt = strchr(lt, '>')) == NULL) {
			out(f, p);
			return;
		}
		out_n(f, p, (size_t)(lt - p));
		p = gt + 1;
	}
}

static void write_item(struct feed *f)
{
	const char *link;
	int i;

	write_head(f);
	f->nitems++;
	f->ititle_n = trim(f->ititle, f->ititle_n);
	f->ilink_n = trim(f->ilink, f->ilink_n);
	f->iguid_n = trim(f->iguid, f->iguid_n);
	f->idate_n = trim(f->idate, f->idate_n);
	f->iauthor_n = trim(f->iauthor, f->iauthor_n);
	f->img_n = trim(f->img, f->img_n);
	tidy_date(f->idate);
	link = f->ilink_n ? f->ilink
		: f->iguid_link && strncmp(f->iguid, "http", 4) == 0 ? f->iguid
		: f->content_src[0] ? f->content_src : "";
	out(f, "<hr><h2>");
	if (*link) {
		out(f, "<a href=\"");
		out(f, link);
		out(f, "\">");
	}
	if (!f->ititle_n)	/* (Mastodon's posts have none) */
		out(f, f->idate_n ? f->idate : "(no title)");
	else if (f->ititle_raw)
		out_untagged(f, f->ititle);
	else
		out(f, f->ititle);
	out(f, *link ? "</a></h2>" : "</h2>");
	if (!f->ititle_n)
		f->idate_n = 0;		/* (it was the title) */
	if (f->idate_n || f->iauthor_n) {
		out(f, "<p><i>");
		if (f->idate_n)
			out(f, f->idate);
		if (f->idate_n && f->iauthor_n)
			out(f, " &middot; ");
		out(f, f->iauthor);
		out(f, "</i></p>");
	}
	if (f->img_n) {
		out(f, "<p><img src=\"");
		out(f, f->img);
		out(f, "\" alt=\"picture\"></p>");
	}
	if (f->text_n) {
		if (f->text_plain) {
			const char *p = f->text, *nl;

			/* plain text: its line breaks kept */
			out(f, "<p>");
			while ((nl = strchr(p, '\n')) != NULL) {
				out_n(f, p, (size_t)(nl - p));
				out(f, "<br>");
				p = nl + 1;
			}
			out(f, p);
			out(f, "</p>");
		} else {
			out(f, "<div>");
			out_n(f, f->text, f->text_n);
			out(f, "</div>");
		}
		if (f->text_cut)
			out(f, "<p><i>(cut short here)</i></p>");
	}
	for (i = 0; i < f->nencl; i++) {
		char len[40];

		out(f, "<p>Enclosure: <a href=\"");
		out(f, f->encl[i].url);
		out(f, "\">");
		out(f, f->encl[i].type[0] ? f->encl[i].type : "a file");
		if (f->encl[i].length > 0) {
			sprintf(len, ", %ld KB", (f->encl[i].length + 1023) / 1024);
			out(f, len);
		}
		out(f, "</a></p>");
	}
}

static void item_begin(struct feed *f)
{
	write_head(f);
	f->item = f->depth;
	f->ititle_n = f->ilink_n = f->iguid_n = f->idate_n = f->iauthor_n = 0;
	f->ititle[0] = f->ilink[0] = f->iguid[0] = f->idate[0] = '\0';
	f->iauthor[0] = '\0';
	f->ititle_raw = f->iguid_link = 0;
	f->img_n = 0;
	f->img[0] = '\0';
	f->text_n = 0;
	f->text[0] = '\0';
	f->text_kind = f->text_plain = f->text_cut = 0;
	f->content_src_n = 0;
	f->content_src[0] = '\0';
	f->nencl = 0;
}

/* an attribute's value, escaped for an HTML attribute, into buf */
static void attr_esc(struct feed *f, const char *tag, const char *name,
	char *buf, size_t max, size_t *n)
{
	static char v[1024];
	size_t i;

	attr(tag, name, v, sizeof v);
	*n = 0;
	buf[0] = '\0';
	for (i = 0; v[i]; i++) {
		const char *e = v[i] == '<' ? "&lt;" : v[i] == '>' ? "&gt;"
			: v[i] == '&' ? "&amp;" : v[i] == '"' ? "&quot;" : NULL;

		if (e)
			put_s(f, buf, max, n, e);
		else
			put(f, buf, max, n, v[i]);
	}
}

static void add_encl(struct feed *f, const char *tag, const char *url_attr)
{
	struct feed_encl *e;
	size_t n;
	char len[24];

	if (f->nencl == FEED_ENCL_MAX)
		return;
	e = &f->encl[f->nencl];
	attr_esc(f, tag, url_attr, e->url, sizeof e->url, &n);
	if (!n)
		return;
	attr_esc(f, tag, "type", e->type, sizeof e->type, &n);
	attr(tag, "length", len, sizeof len);
	e->length = 0;
	sscanf(len, "%ld", &e->length);
	f->nencl++;
	if (!f->img_n && strncmp(e->type, "image/", 6) == 0) {
		snprintf(f->img, sizeof f->img, "%s", e->url);
		f->img_n = strlen(f->img);
		f->nencl--;		/* (shown as the picture) */
	}
}

/* the text of an item: a fuller kind replaces a summary */
static void text_capture(struct feed *f, int kind, const char *type)
{
	int xhtml = strcmp(type, "xhtml") == 0;
	int html = xhtml || strcmp(type, "html") == 0
		|| strcmp(type, "text/html") == 0;

	if (kind <= f->text_kind)
		return;
	f->text_kind = kind;
	f->text_plain = !html;
	f->text_cut = 0;
	/* (XHTML: its tags are copied as they are, its text escaped) */
	capture(f, f->text, sizeof f->text, &f->text_n, html && !xhtml, xhtml);
}

static int is(const char *a, const char *b)
{
	return strcmp(a, b) == 0;
}

static void item_child(struct feed *f, const char *qname, const char *local,
	const char *tag)
{
	char type[40], rel[40], v[40];

	attr(tag, "type", type, sizeof type);
	if (strncmp(qname, "media:", 6) == 0) {
		/* Media RSS: a picture (YouTube: in a group, with the text) */
		if (is(local, "thumbnail") && !f->img_n)
			attr_esc(f, tag, "url", f->img, sizeof f->img, &f->img_n);
		else if (is(local, "content") && !f->img_n) {
			attr(tag, "medium", v, sizeof v);
			if (is(v, "image") || strncmp(type, "image/", 6) == 0)
				attr_esc(f, tag, "url", f->img, sizeof f->img, &f->img_n);
		} else if (is(local, "description"))
			text_capture(f, 1, type[0] ? type : "plain");
		return;
	}
	if (f->depth - f->item == 2) {
		/* Atom's <author><name> */
		if (is(local, "name") && is(parent(f, 1), "author"))
			capture(f, f->iauthor, sizeof f->iauthor, &f->iauthor_n, 0, 0);
		return;
	}
	if (f->depth - f->item != 1)
		return;
	if (is(local, "title")) {
		f->ititle_raw = is(type, "html") || is(type, "xhtml");
		capture(f, f->ititle, sizeof f->ititle, &f->ititle_n,
			is(type, "html"), is(type, "xhtml"));
	} else if (is(local, "link")) {
		attr(tag, "rel", rel, sizeof rel);
		attr(tag, "href", v, sizeof v);
		if (!v[0]) {
			if (!f->ilink_n)	/* RSS: the URL as text */
				capture(f, f->ilink, sizeof f->ilink, &f->ilink_n, 0, 0);
		} else if (is(rel, "enclosure"))
			add_encl(f, tag, "href");
		else if ((!rel[0] || is(rel, "alternate")) && !f->ilink_n)
			attr_esc(f, tag, "href", f->ilink, sizeof f->ilink, &f->ilink_n);
	} else if (is(local, "guid") || is(local, "id")) {
		attr(tag, "isPermaLink", v, sizeof v);
		f->iguid_link = !is(v, "false");
		capture(f, f->iguid, sizeof f->iguid, &f->iguid_n, 0, 0);
	} else if (is(local, "description"))
		text_capture(f, 1, "html");
	else if (is(local, "encoded"))
		text_capture(f, 2, "html");
	else if (is(local, "summary"))
		text_capture(f, 1, type[0] ? type : "text");
	else if (is(local, "content")) {
		attr(tag, "src", v, sizeof v);
		if (v[0])
			attr_esc(f, tag, "src", f->content_src, sizeof f->content_src,
				&f->content_src_n);
		else
			text_capture(f, 2, type[0] ? type : "text");
	} else if (is(local, "pubDate") || is(local, "published")
		|| is(local, "date") || (is(local, "updated") && !f->idate_n)) {
		capture(f, f->idate, sizeof f->idate, &f->idate_n, 0, 0);
	} else if (is(local, "author") && !f->iauthor_n)
		capture(f, f->iauthor, sizeof f->iauthor, &f->iauthor_n, 0, 0);
	else if (is(local, "creator"))
		capture(f, f->iauthor, sizeof f->iauthor, &f->iauthor_n, 0, 0);
	else if (is(local, "enclosure"))
		add_encl(f, tag, "url");
}

/* the feed's own title, link and description */
static void feed_child(struct feed *f, const char *qname, const char *local,
	const char *tag)
{
	const char *up = parent(f, 1);
	char rel[40], v[8];

	if (!(f->kind == FEED_K_ATOM ? f->depth == 2 : is(up, "channel")))
		return;
	if (strchr(qname, ':') && strncmp(qname, "atom:", 5) != 0)
		return;
	if (is(local, "title") && !f->title_n)
		capture(f, f->title, sizeof f->title, &f->title_n, 0, 0);
	else if (is(local, "link") && !f->link_n) {
		attr(tag, "rel", rel, sizeof rel);
		attr(tag, "href", v, sizeof v);
		if (!v[0] && !strchr(qname, ':'))
			capture(f, f->link, sizeof f->link, &f->link_n, 0, 0);
		else if (v[0] && (!rel[0] || is(rel, "alternate")))
			attr_esc(f, tag, "href", f->link, sizeof f->link, &f->link_n);
	} else if ((is(local, "description") || is(local, "subtitle"))
		&& !f->sub_n)
		capture(f, f->sub, sizeof f->sub, &f->sub_n, 0, 0);
}

static void element_end(struct feed *f);

/* a start tag (tag: its text, without < >) */
static void element_start(struct feed *f, const char *tag, int self)
{
	char qname[FEED_NAME_MAX * 2], *local;
	size_t n = 0;

	while (tag[n] && tag[n] != ' ' && tag[n] != '\t' && tag[n] != '\r'
		&& tag[n] != '\n' && tag[n] != '/' && n + 1 < sizeof qname) {
		qname[n] = tag[n];
		n++;
	}
	qname[n] = '\0';
	local = strrchr(qname, ':') ? strrchr(qname, ':') + 1 : qname;

	if (f->kind == FEED_K_UNKNOWN) {
		f->kind = is(local, "rss") ? FEED_K_RSS : is(local, "feed")
			? FEED_K_ATOM : is(local, "RDF") ? FEED_K_RDF : FEED_K_XML;
		if (f->kind == FEED_K_XML) {
			/* not a feed: the XML itself, from here */
			ensure_meta(f);
			out(f, "<pre>&lt;");
			out_esc(f, tag, strlen(tag));
			out(f, "&gt;");
			f->state = T_PASS;
			return;
		}
	}
	/* XHTML content: its markup is the item's HTML */
	if (f->cap && f->cap_xhtml && f->depth >= f->cap_depth) {
		put(f, f->cap, f->cap_max, f->cap_n, '<');
		put_s(f, f->cap, f->cap_max, f->cap_n, tag);
		put(f, f->cap, f->cap_max, f->cap_n, '>');
	}
	if (f->depth < FEED_DEPTH) {
		char *d = f->path[f->depth];

		for (n = 0; local[n] && n < FEED_NAME_MAX - 1; n++)
			d[n] = local[n];
		d[n] = '\0';
	}
	f->depth++;
	if (f->cap && f->cap_xhtml)
		;	/* (copied above) */
	else if (!f->item && (is(local, "item") || is(local, "entry")))
		item_begin(f);
	else if (f->item)
		item_child(f, qname, local, tag);
	else
		feed_child(f, qname, local, tag);
	if (self)
		element_end(f);
}

static void element_end(struct feed *f)
{
	if (f->depth == 0)
		return;
	if (f->cap && f->depth == f->cap_depth)
		f->cap = NULL;
	if (f->item && f->depth == f->item) {
		write_item(f);
		f->item = 0;
	}
	f->depth--;
}

/* a tag's text is complete (without < >) */
static void tag_done(struct feed *f)
{
	size_t n = f->tag_n;
	int self;

	f->tag[n] = '\0';
	if (f->tag[0] == '/') {
		if (f->cap && f->cap_xhtml && f->depth > f->cap_depth) {
			put(f, f->cap, f->cap_max, f->cap_n, '<');
			put_s(f, f->cap, f->cap_max, f->cap_n, f->tag);
			put(f, f->cap, f->cap_max, f->cap_n, '>');
		}
		element_end(f);
		return;
	}
	while (n && (f->tag[n - 1] == ' ' || f->tag[n - 1] == '\t'
		|| f->tag[n - 1] == '\r' || f->tag[n - 1] == '\n'))
		n--;
	/* (a tag too long for tag[] lost its end, but not where it ended) */
	self = f->tag_long ? f->last == '/' : n && f->tag[n - 1] == '/';
	if (self && !f->tag_long)
		f->tag[n - 1] = '\0';
	element_start(f, f->tag, self);
}

/* <?xml ... encoding="x"?> */
static void pi_done(struct feed *f)
{
	f->tag[f->tag_n] = '\0';
	if (strncmp(f->tag, "xml ", 4) == 0 && !f->meta_done)
		attr(f->tag, "encoding", f->encoding, sizeof f->encoding);
}

/* a character of text (decoded) */
static void text_char(struct feed *f, char c)
{
	if (f->cap && f->depth >= f->cap_depth)
		cap_char(f, c);
}

static void entity_done(struct feed *f)
{
	static const char *const ents[] = { "lt", "gt", "amp", "quot", "apos" };
	static const char chars[] = "<>&\"'";
	char ref[16];
	size_t i;

	f->ent[f->ent_n] = '\0';
	for (i = 0; i < 5; i++)
		if (strcmp(f->ent, ents[i]) == 0) {
			text_char(f, chars[i]);
			return;
		}
	sprintf(ref, "&%s;", f->ent);
	if (f->cap && f->depth >= f->cap_depth)
		cap_ref(f, ref);
}

static void feed_char(struct feed *f, char c)
{
	switch (f->state) {
	case T_PASS: {
		const char *e = c == '<' ? "&lt;" : c == '>' ? "&gt;"
			: c == '&' ? "&amp;" : NULL;

		if (e)
			out(f, e);
		else
			out_n(f, &c, 1);
		return;
	}
	case T_TEXT:
		if (c == '<') {
			f->state = T_MARKUP;
			f->tag_n = 0;
			f->tag_long = 0;
			f->quote = 0;
			f->dashes = 0;
		} else if (c == '&') {
			f->state = T_ENT;
			f->ent_n = 0;
		} else
			text_char(f, c);
		return;
	case T_ENT:
		if (c == ';') {
			entity_done(f);
			f->state = T_TEXT;
		} else if (f->ent_n < sizeof f->ent - 1 && (c == '#'
			|| (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9'))) {
			f->ent[f->ent_n++] = c;
		} else {
			/* not one: a lone & */
			size_t i;

			text_char(f, '&');
			for (i = 0; i < f->ent_n; i++)
				text_char(f, f->ent[i]);
			f->state = T_TEXT;
			feed_char(f, c);
		}
		return;
	case T_COMMENT:
		if (c == '>' && f->dashes >= 2)
			f->state = T_TEXT;
		f->dashes = c == '-' ? f->dashes + 1 : 0;
		return;
	case T_CDATA:
		if (c == ']') {
			f->dashes++;
			return;
		}
		if (c == '>' && f->dashes >= 2) {
			while (f->dashes-- > 2)
				text_char(f, ']');
			f->dashes = 0;
			f->state = T_TEXT;
			return;
		}
		while (f->dashes > 0) {
			text_char(f, ']');
			f->dashes--;
		}
		text_char(f, c);
		return;
	case T_PI:
		if (c == '>' && f->tag_n && f->tag[f->tag_n - 1] == '?') {
			f->tag_n--;
			pi_done(f);
			f->state = T_TEXT;
		} else if (f->tag_n < sizeof f->tag - 1)
			f->tag[f->tag_n++] = c;
		return;
	case T_DOCTYPE:
		if (c == '[')
			f->bracket++;
		else if (c == ']')
			f->bracket--;
		else if (c == '>' && f->bracket <= 0)
			f->state = T_TEXT;
		return;
	case T_MARKUP: {
		int bang = f->tag_n && f->tag[0] == '!';

		if (f->tag_n == 0 && c == '?') {
			f->state = T_PI;
			return;
		}
		if (f->quote) {
			if (c == f->quote)
				f->quote = 0;
		} else if ((c == '"' || c == '\'') && f->tag_n && !bang)
			f->quote = c;
		else if (c == '>' && !bang) {
			tag_done(f);
			if (f->state == T_MARKUP)
				f->state = T_TEXT;
			return;
		}
		if (f->tag_n < sizeof f->tag - 1)
			f->tag[f->tag_n++] = c;
		else
			f->tag_long = 1;
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			f->last = c;
		if (f->tag[0] == '!') {
			if (f->tag_n == 3 && strncmp(f->tag, "!--", 3) == 0) {
				f->state = T_COMMENT;
				f->dashes = 0;
			} else if (f->tag_n == 8 && strncmp(f->tag, "![CDATA[", 8) == 0) {
				f->state = T_CDATA;
				f->dashes = 0;
			} else if (f->tag_n >= 2 && strncmp(f->tag, "!--", f->tag_n)
				&& strncmp(f->tag, "![CDATA[", f->tag_n)) {
				f->state = T_DOCTYPE;
				f->bracket = 0;
				if (c == '>')
					f->state = T_TEXT;
			}
		}
		return;
	}
	}
}

void feed_begin(struct feed *f,
	void (*o)(void *ctx, const char *html, size_t n), void *ctx)
{
	memset(f, 0, sizeof *f);
	f->out = o;
	f->ctx = ctx;
	f->state = T_TEXT;
}

void feed_feed(struct feed *f, const unsigned char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		feed_char(f, (char)s[i]);
}

void feed_end(struct feed *f)
{
	if (f->state == T_PASS) {
		out(f, "</pre>");
		return;
	}
	if (f->item) {
		/* cut off inside an item: what came of it */
		f->depth = f->item;
		f->cap = NULL;
		write_item(f);
		f->item = 0;
	}
	if (f->kind == FEED_K_UNKNOWN) {
		ensure_meta(f);
		out(f, "<title>Not a feed</title><p>There was no XML in this.</p>");
		return;
	}
	write_head(f);
	if (!f->nitems)
		out(f, "<p>(No items in this feed.)</p>");
}

int feed_type(const char *ct)
{
	static const char *const types[] = { "application/rss+xml",
		"application/atom+xml", "application/rdf+xml",
		"application/x-rss+xml", "application/x-atom+xml",
		"application/xml", "text/xml", NULL };
	int i;

	for (i = 0; types[i]; i++)
		if (strcmp(ct, types[i]) == 0)
			return 1;
	return 0;
}
