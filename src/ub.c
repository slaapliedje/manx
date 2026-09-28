/*
 * ub - the browser, text frontend.
 *
 *   ub [URL|FILE]
 *
 * Keys (as in Lynx, plus vi's):
 *   Up/Down      previous/next link (scrolling when it's off screen)
 *   Right, Enter follow the link          Left, u, Backspace  back
 *   Tab/Shift-Tab  next/previous link anywhere on the page
 *   Space, PgDn, b, PgUp  page down/up    j/k  line down/up
 *   Home/End, </>  top/bottom             m    the main content
 *   g  go to a URL (or search)  G  edit the current URL
 *   /  find  n/N  next/previous match     =    page and link information
 *   r  reload   ^L  redraw   z/Esc  stop loading   ?  help   q  quit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include "os.h"
#include "tags.h"
#include "doc.h"
#include "load.h"
#include "gophermap.h"
#include "layout.h"
#include "screen.h"
#include "entropy.h"
#include "tls.h"
#include "conn.h"
#include "url.h"
#include "fetch.h"
#include "utf8.h"
#include "forms.h"
#include "cookie.h"
#include "config.h"
#include "cache.h"
#include <time.h>

#define HIST_MAX	64
#define SEARCH_URL	"https://lite.duckduckgo.com/lite/?q="

/* the page on screen */
static struct doc g_doc;
static struct html_load g_load;
static struct page g_page;
static int g_have_page;
static char g_url[URL_MAX];		/* what the page is (final URL) */
static struct url g_base;		/* what its links are relative to */
static char g_ctype[128];
static long g_top;			/* first line shown */
static long g_sel = -1;			/* selected link, 0-based */
static char g_find[128];
static long g_find_line = -1, g_find_off;
static char g_msg[300];			/* the status line's message */
static int g_msg_sticky;		/* ... kept until the next key */
static char g_info[200];		/* how the page came (TLS etc.) */
static struct forms g_forms;		/* its form fields */
static int g_have_forms;
static const char *g_search = SEARCH_URL;

/* loading */
static int g_loading, g_started, g_gopher, g_aborted, g_unsupported;
static struct gophermap g_gmap;
static unsigned long g_bytes, g_last_draw;

struct hist {
	char *url;
	long top, sel;
	/* the answer to a form sent with POST: what was sent */
	char *post_body;
	size_t post_len;
	char post_type[80];
	char *post_key;			/* its answer in the cache */
};
static struct hist g_hist[HIST_MAX];
static int g_nhist, g_hpos = -1;

static int prompt(const char *label, char *buf, size_t n);
static int prompt_mask(const char *label, char *buf, size_t n, int mask);
static int confirm(const char *question);

static int view_rows(void)
{
	return scr_rows - 2;
}

/* --- text for the terminal ---------------------------------------------- */

/* UTF-8 to the terminal's character set, into buf */
static void to_term(const char *s, char *buf, size_t n)
{
	size_t o = 0;

	while (*s && o + 5 < n) {
		if ((unsigned char)*s < 0x80 || scr_cs == TCS_UTF8) {
			buf[o++] = *s++;
			continue;
		}
		{
			unsigned long cp = utf8_get(&s);

			o += (size_t)translit(cp, scr_cs == TCS_LATIN1, buf + o);
		}
	}
	buf[o] = '\0';
}

static void message(const char *fmt, const char *arg)
{
	snprintf(g_msg, sizeof g_msg, fmt, arg ? arg : "");
}

/* --- drawing ------------------------------------------------------------ */

static int cell_attr(int sa, int link)
{
	int a = 0;

	if (sa & SA_BOLD)
		a |= CA_BOLD;
	if (sa & (SA_UNDER | SA_FIELD))
		a |= CA_UNDER;
	if (link) {
		a |= CA_LINK;
		if (link - 1 == g_sel)
			a |= CA_REV;
	}
	return a;
}

static void draw_line(int row, long ln)
{
	const struct page *p = &g_page;
	const struct lline *l = &p->lines[ln];
	unsigned long off = l->off, end = l->off + l->len, s = l->span;
	int col = l->indent;

	while (off < end && col < scr_cols) {
		unsigned long next = end;
		const struct lspan *sp;

		s = layout_span_at(p, s, off);
		sp = &p->spans[s];
		if (s + 1 < p->nspans && p->spans[s + 1].off < end)
			next = p->spans[s + 1].off;
		col += scr_put(row, col, p->text + off, (int)(next - off),
			cell_attr(sp->attr, sp->link));
		off = next;
	}
	/* a find match on this line */
	if (ln == g_find_line && g_find[0]) {
		unsigned long m = (unsigned long)g_find_off;
		size_t k = strlen(g_find);
		int c = l->indent;
		const char *t = p->text + l->off;

		if (m >= l->off && m + k <= end) {
			/* its column: count the characters before it */
			if (scr_cs == TCS_UTF8) {
				const char *q = t;

				while (q < p->text + m) {
					unsigned long cp = utf8_get(&q);

					c += ucs_width(cp);
				}
			} else
				c += (int)(m - l->off);
			scr_put(row, c, p->text + m, (int)k, CA_MARK | CA_REV);
		}
	}
}

/* what the status line says about a selected form field */
static void describe_field(nodeid node, char *buf, size_t n)
{
	const struct field *f = g_have_forms ? forms_field(&g_forms, node) : NULL;
	const char *name = doc_attr(&g_doc, node, ATTR_NAME);

	if (f == NULL) {
		snprintf(buf, n, "A form field.");
		return;
	}
	switch (f->type) {
	case FT_TEXT: case FT_PASSWORD: case FT_TEXTAREA:
		snprintf(buf, n, "%s field%s%.40s: Enter to type in it",
			f->type == FT_PASSWORD ? "Password" : "Text",
			name ? " " : "", name ? name : "");
		break;
	case FT_CHECKBOX: case FT_RADIO:
		snprintf(buf, n, "%s: Enter to %s", f->type == FT_CHECKBOX ?
			"Checkbox" : "Option", f->type == FT_CHECKBOX ?
			(f->checked ? "clear it" : "tick it") : "choose it");
		break;
	case FT_SELECT:
		snprintf(buf, n, "A list: Enter to choose");
		break;
	case FT_SUBMIT: case FT_IMAGE: {
		const char *a = f->form ? doc_attr(&g_doc, f->form, ATTR_ACTION) : NULL;
		const char *m = f->form ? doc_attr(&g_doc, f->form, ATTR_METHOD) : NULL;

		snprintf(buf, n, "Button: Enter sends the form%s%.200s%s",
			a && *a ? " to " : "", a && *a ? a : "",
			m && (m[0] == 'p' || m[0] == 'P') ? " (POST)" : "");
		break;
	}
	case FT_RESET:
		snprintf(buf, n, "Button: Enter resets the form");
		break;
	default:
		snprintf(buf, n, "A button that needs JavaScript");
	}
}

static void draw_status(void)
{
	char buf[512];
	int r = scr_rows - 1;

	scr_fill(r, 0, scr_cols, ' ', 0);
	if (g_msg[0])
		to_term(g_msg, buf, sizeof buf);
	else if (g_sel >= 0 && (unsigned long)g_sel < g_page.nlinks) {
		/* the selected link's URL */
		const struct llink *k = &g_page.links[g_sel];
		const char *h = k->kind == LK_HREF ?
			doc_attr(&g_doc, k->node, k->node
				&& g_doc.nodes[k->node].tag == TAG_FRAME ?
				ATTR_SRC : ATTR_HREF) : NULL;
		struct url *u = xmalloc(sizeof *u);

		buf[0] = '\0';
		if (h && u && url_resolve(&g_base, h, u) == URL_OK)
			url_format(u, buf, sizeof buf, 1);
		else if (k->kind == LK_FIELD)
			describe_field(k->node, buf, sizeof buf);
		xfree(u);
	} else
		snprintf(buf, sizeof buf, "%s", g_info);
	scr_put(r, 0, buf, (int)strlen(buf), 0);
}

static void draw(int full)
{
	char title[300], pos[40];
	const char *t;
	int i, n, rows = view_rows();

	scr_erase();
	/* the title bar: title, and where we are */
	t = g_have_page ? doc_title(&g_doc) : "";
	to_term(*t ? t : g_url, title, sizeof title);
	if (!g_have_page || g_page.nlines <= (unsigned long)rows)
		pos[0] = '\0';
	else
		sprintf(pos, " %ld%%%s", (g_top + rows >= (long)g_page.nlines ?
			100L : (g_top + rows) * 100 / (long)g_page.nlines),
			g_page.truncated || g_doc.truncated ? " (truncated)" : "");
	if (g_loading)
		snprintf(pos, sizeof pos, " loading %lu KB", g_bytes / 1024);
	scr_fill(0, 0, scr_cols, ' ', CA_REV);
	n = (int)strlen(pos);
	scr_put(0, 0, title, (int)strlen(title), CA_REV | CA_BOLD);
	scr_put(0, scr_cols - n, pos, n, CA_REV);
	if (g_have_page)
		for (i = 0; i < rows; i++) {
			long ln = g_top + i;

			if (ln >= (long)g_page.nlines)
				break;
			draw_line(1 + i, ln);
		}
	draw_status();
	scr_cursor(-1, -1);
	scr_flush(full);
}

/* --- the page ------------------------------------------------------------ */

static void relayout(long max_lines)
{
	if (g_have_page)
		layout_free(&g_page);
	if (layout_run(&g_page, &g_doc, scr_cols, scr_cs,
		(unsigned long)max_lines, 0, g_have_forms ? &g_forms : NULL) < 0) {
		g_have_page = 0;
		message("out of memory for the layout", NULL);
		return;
	}
	g_have_page = 1;
}

static long max_top(void)
{
	long m = (long)g_page.nlines - view_rows();

	return m < 0 ? 0 : m;
}

static int link_visible(long k)
{
	long ln;

	if (k < 0 || (unsigned long)k >= g_page.nlinks)
		return 0;
	ln = (long)g_page.links[k].line;
	return ln >= g_top && ln < g_top + view_rows();
}

/* the first link on screen (or -1) */
static long first_visible(void)
{
	unsigned long k;

	for (k = 0; k < g_page.nlinks; k++)
		if ((long)g_page.links[k].line >= g_top)
			return link_visible((long)k) ? (long)k : -1;
	return -1;
}

static void scroll_to(long top)
{
	if (top > max_top())
		top = max_top();
	if (top < 0)
		top = 0;
	g_top = top;
	if (!link_visible(g_sel))
		g_sel = first_visible();
}

/* --- loading ------------------------------------------------------------- */

static void on_status(void *ctx, const char *msg)
{
	(void)ctx;
	message("%s", msg);
	draw(0);
}

static void feed_html(void *ctx, const char *s, size_t n)
{
	(void)ctx;
	html_load_feed(&g_load, (const unsigned char *)s, n);
}

static int is_html(const char *ct)
{
	return !*ct || strcmp(ct, "text/html") == 0
		|| strcmp(ct, "application/xhtml+xml") == 0;
}

/* the response's cache headers (of the final response) */
static char h_cc[200], h_expires[64], h_date[64], h_etag[128], h_lastmod[64];

static void on_header(void *ctx, const char *name, const char *value)
{
	char *dst = NULL;
	size_t n = 0, i;
	char low[32];

	(void)ctx;
	for (i = 0; name[i] && i < sizeof low - 1; i++)
		low[i] = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + 32)
			: name[i];
	low[i] = '\0';
	if (strcmp(low, "cache-control") == 0)
		dst = h_cc, n = sizeof h_cc;
	else if (strcmp(low, "expires") == 0)
		dst = h_expires, n = sizeof h_expires;
	else if (strcmp(low, "date") == 0)
		dst = h_date, n = sizeof h_date;
	else if (strcmp(low, "etag") == 0)
		dst = h_etag, n = sizeof h_etag;
	else if (strcmp(low, "last-modified") == 0)
		dst = h_lastmod, n = sizeof h_lastmod;
	if (dst)
		snprintf(dst, n, "%s", value);
}

/* the cache entry this load writes (g_caching), or revalidates */
static char g_ckey[URL_MAX + 40];
static int g_caching, g_status;
static struct cache_meta g_cmeta;	/* the cached copy being checked */
static int g_revalidating;

static int cacheable_type(const char *ct)
{
	return is_html(ct) || strncmp(ct, "text/", 5) == 0;
}

static void on_head(void *ctx, int status, const char *ctype,
	const char *charset, const char *url)
{
	(void)ctx;
	if (g_started)
		return;
	g_status = status;
	if (status == 304 && g_revalidating)
		return;			/* the cached copy is still good */
	snprintf(g_ctype, sizeof g_ctype, "%s", ctype);
	if (url && *url)
		snprintf(g_url, sizeof g_url, "%s", url);
	g_gopher = strcmp(ctype, "text/x-gopher-menu") == 0;
	g_unsupported = !g_gopher && !is_html(ctype)
		&& strncmp(ctype, "text/", 5) != 0;
	if (g_unsupported)
		return;
	html_load_begin(&g_load, &g_doc, charset,
		!g_gopher && !is_html(ctype));
	if (g_gopher)
		gophermap_begin(&g_gmap, feed_html, NULL);
	g_started = 1;
	/* keep it on disk, if it's a page worth keeping */
	g_caching = g_ckey[0] && status == 200 && cacheable_type(ctype)
		&& cache_begin(g_ckey) == 0;
}

/* a key while loading: z, Esc, ^C, ^G stop it */
static int stop_asked(void)
{
	int k;

	while ((k = scr_getkey(0)) >= 0)
		if (k == 'z' || k == 27 || k == 3 || k == 7 || k == 'q')
			return 1;
	return 0;
}

static void feed_body(const unsigned char *d, size_t n)
{
	if (g_gopher)
		gophermap_feed(&g_gmap, d, n);
	else
		html_load_feed(&g_load, d, n);
}

static int on_body(void *ctx, const unsigned char *d, size_t n)
{
	unsigned long now;

	if (!g_started)
		on_head(ctx, 200, "text/html", "", NULL);
	if (g_unsupported)
		return -1;
	if (g_caching)
		cache_write(d, n);
	feed_body(d, n);
	g_bytes += n;
	if (stop_asked()) {
		g_aborted = 1;
		return -1;
	}
	/* show the top of the page as it arrives (only the lines on
	 * screen are laid out, so this stays cheap) */
	now = os_msec();
	if (now - g_last_draw > 800) {
		g_last_draw = now;
		relayout(view_rows());
		g_top = 0;
		g_sel = -1;
		snprintf(g_msg, sizeof g_msg, "Loading %lu KB  (z: stop)",
			g_bytes / 1024);
		draw(0);
	}
	return 0;
}

static void on_reset(void *ctx)
{
	(void)ctx;
	doc_free(&g_doc);
	doc_init(&g_doc, 0);
	g_started = 0;
	g_bytes = 0;
	if (g_caching)
		cache_abort();
	g_caching = 0;
}

static void drop_forms(void)
{
	if (g_have_forms)
		forms_free(&g_forms);
	g_have_forms = 0;
}

static void make_forms(void)
{
	drop_forms();
	g_have_forms = forms_init(&g_forms, &g_doc) == 0;
}

/* show a page made here (errors, help) */
static void load_string(const char *html, const char *url)
{
	drop_forms();
	if (g_have_page) {
		layout_free(&g_page);
		g_have_page = 0;
	}
	doc_free(&g_doc);
	doc_init(&g_doc, 0);
	html_load_begin(&g_load, &g_doc, "utf-8", 0);
	html_load_feed(&g_load, (const unsigned char *)html, strlen(html));
	html_load_end(&g_load);
	snprintf(g_url, sizeof g_url, "%s", url);
	url_parse(url, &g_base);
	g_ctype[0] = '\0';
	make_forms();
	relayout(0);
	g_top = 0;
	g_sel = first_visible();
}

static const char help_html[] =
	"<title>ub help</title><h1>Keys</h1><pre>"
	"Up/Down        previous/next link\n"
	"Right, Enter   follow the link\n"
	"Left, u, BkSp  back\n"
	"Tab/Shift-Tab  next/previous link anywhere\n"
	"Space, PgDn    page down        b, PgUp   page up\n"
	"j/k            line down/up     Home/End  top/bottom\n"
	"m              the main content (skips site navigation)\n"
	"g              go to a URL, or search for words\n"
	"G              edit the current URL\n"
	"/              find             n/N       next/previous match\n"
	"=              page and link information\n"
	"r              reload           ^L        redraw\n"
	"a              bookmark this page  v      the bookmarks\n"
	"z, Esc         stop loading     q         quit\n"
	"</pre><h1>Forms</h1><p>Select a field (Up/Down, Tab) and press Enter: "
	"type in a text field (Enter moves to the next; in the last one it "
	"sends the form), tick a checkbox, choose from a list, or press a "
	"button. A text area opens in $EDITOR (vi).</p>"
	"<h1>Settings</h1><p>In ~/.ub/config, one <i>key = value</i> a line "
	"(or as UB_KEY in the environment):</p><pre>"
	"home = URL             the start page\n"
	"search = URL           where g sends words (DuckDuckGo Lite)\n"
	"charset = utf-8        the terminal's: utf-8, latin1, ascii\n"
	"color = off            no colours\n"
	"cookies = off          no cookies\n"
	"cache_kb = 2048        the disk cache's size (0: none)\n"
	"cafile = FILE          the PEM bundle of trusted roots\n"
	"early_requests = off   send nothing before the certificate is "
	"checked\n"
	"</pre><p>Start pages: <a href=\"about:start\">about:start</a>";

static const char start_html[] =
	"<title>ub</title><h1>ub - a web browser for 68030 Unix</h1>"
	"<p>Press <b>g</b> and type a URL, or words to search for. "
	"<b>?</b> lists the keys.</p><ul>"
	"<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a>"
	"<li><a href=\"https://en.m.wikipedia.org/\">Wikipedia</a>"
	"<li><a href=\"https://news.ycombinator.com/\">Hacker News</a>"
	"<li><a href=\"https://text.npr.org/\">NPR text</a>"
	"<li><a href=\"gopher://gopher.floodgap.com/\">Floodgap gopher</a>"
	"<li><a href=\"http://example.com/\">example.com</a></ul>";

static void html_escape(const char *s, char *out, size_t n)
{
	size_t o = 0;

	for (; *s && o + 6 < n; s++) {
		const char *e = *s == '<' ? "&lt;" : *s == '&' ? "&amp;"
			: *s == '>' ? "&gt;" : NULL;

		if (e) {
			strcpy(out + o, e);
			o += strlen(e);
		} else
			out[o++] = *s;
	}
	out[o] = '\0';
}

static void error_page(const char *url, const char *why)
{
	static char html[URL_MAX * 2 + 600];
	static char eu[URL_MAX + 200], ew[400];

	html_escape(url, eu, sizeof eu);
	html_escape(why, ew, sizeof ew);
	snprintf(html, sizeof html, "<title>Can't load the page</title>"
		"<h1>Can't load the page</h1><p>%.1000s</p><p><b>%.300s</b></p>"
		"<p>r tries again, Left goes back.</p>", eu, ew);
	load_string(html, url);
	g_msg[0] = '\0';
}

/* how load() may use the cache */
enum {
	L_NORMAL,		/* a fresh copy, else ask the server if it changed */
	L_HISTORY,		/* any copy (going back) */
	L_RELOAD		/* the server's, always */
};

static void doc_reset(void)
{
	drop_forms();
	if (g_have_page) {
		layout_free(&g_page);
		g_have_page = 0;
	}
	doc_free(&g_doc);
	doc_init(&g_doc, 0);
	g_started = g_gopher = g_aborted = g_unsupported = 0;
	g_bytes = 0;
	g_top = 0;
	g_sel = -1;
	g_info[0] = '\0';
}

/* the document is complete: its base URL, forms, layout, the fragment */
static void finish_doc(const char *frag)
{
	if (g_gopher)
		gophermap_end(&g_gmap);
	html_load_end(&g_load);
	/* a fragment asked for survives redirects */
	if (frag[0] && !strchr(g_url, '#')
		&& strlen(g_url) + strlen(frag) + 2 < sizeof g_url) {
		strcat(g_url, "#");
		strcat(g_url, frag);
	}
	/* links are relative to <base href>, else to the page */
	url_parse(g_url, &g_base);
	if (g_load.tree.base_href[0]) {
		static struct url b;

		if (url_resolve(&g_base, g_load.tree.base_href, &b) == URL_OK)
			g_base = b;
	}
	make_forms();
	relayout(0);
	g_top = 0;
	if (frag[0]) {
		long ln = layout_anchor(&g_page, frag);

		if (ln >= 0)
			g_top = ln;
	}
	scroll_to(g_top);
	g_sel = first_visible();
}

static int cached_body(void *ctx, const unsigned char *d, size_t n)
{
	(void)ctx;
	feed_body(d, n);
	g_bytes += n;
	return 0;
}

/* the page from the cache: 0, or -1 (then nothing has changed) */
static int load_cached(const char *key, const struct cache_meta *m,
	const char *frag)
{
	static struct cache_meta mm;
	long age;

	if (!cache_lookup(key, &mm))
		return -1;
	doc_reset();
	snprintf(g_ctype, sizeof g_ctype, "%s", m->type);
	g_gopher = strcmp(m->type, "text/x-gopher-menu") == 0;
	html_load_begin(&g_load, &g_doc, m->charset,
		!g_gopher && !is_html(m->type));
	if (g_gopher)
		gophermap_begin(&g_gmap, feed_html, NULL);
	g_started = 1;
	if (cache_read(key, &mm, cached_body, NULL) < 0) {
		html_load_end(&g_load);
		return -1;
	}
	snprintf(g_url, sizeof g_url, "%s", mm.location[0] ? mm.location : mm.url);
	finish_doc(frag);
	age = ((long)time(NULL) - mm.stored) / 60;
	snprintf(g_info, sizeof g_info, "from the cache (%ld min old)  %lu KB",
		age < 0 ? 0L : age, g_bytes / 1024);
	g_msg[0] = '\0';
	return 0;
}

/* store what just loaded (or not) */
static void cache_finish(int ok, const char *final_url)
{
	static struct cache_meta m;
	int no_store;
	long now = (long)time(NULL);

	if (!g_caching)
		return;
	g_caching = 0;
	memset(&m, 0, sizeof m);
	m.fresh_until = cache_freshness(h_cc[0] ? h_cc : NULL,
		h_expires[0] ? h_expires : NULL, h_date[0] ? h_date : NULL, now,
		&no_store);
	if (!ok || no_store) {
		cache_abort();
		if (no_store)
			cache_remove(g_ckey);
		return;
	}
	snprintf(m.url, sizeof m.url, "%s", g_ckey);
	snprintf(m.location, sizeof m.location, "%.*s",
		(int)strcspn(final_url, "#"), final_url);
	snprintf(m.type, sizeof m.type, "%s", g_ctype);
	snprintf(m.charset, sizeof m.charset, "%s", g_load.cs == CS_UTF8 ?
		"utf-8" : "windows-1252");
	m.stored = now;
	snprintf(m.etag, sizeof m.etag, "%s", h_etag);
	snprintf(m.last_modified, sizeof m.last_modified, "%s", h_lastmod);
	cache_commit(&m);
}

/*
 * Get url into the page: with opts, a form's POST (key: where its answer
 * is kept, or NULL). mode: how the cache may be used. 0, or -1 (the old
 * page is gone either way).
 */
static int load(const char *url_in, const struct fetch_opts *opts, int mode,
	const char *key)
{
	static struct fetch_result res;
	static char url[URL_MAX], cond[400];
	static struct fetch_opts co;
	struct fetch_cb cb;
	char frag[256];
	const char *h;
	int rc;

	/* (a copy: the caller may pass g_url, which this rewrites) */
	snprintf(url, sizeof url, "%s", url_in);
	g_msg[0] = '\0';
	frag[0] = '\0';
	if ((h = strchr(url, '#')) != NULL)
		snprintf(frag, sizeof frag, "%.250s", h + 1);
	if (strcmp(url, "about:help") == 0) {
		load_string(help_html, url);
		return 0;
	}
	if (strcmp(url, "about:start") == 0 || strcmp(url, "about:") == 0
		|| strcmp(url, "about:blank") == 0) {
		load_string(start_html, url);
		return 0;
	}

	/* the cache: a GET is kept under its URL (no fragment), a POST's
	 * answer under the key it's given; local files aren't */
	g_ckey[0] = '\0';
	g_revalidating = 0;
	g_caching = 0;
	if (key)
		snprintf(g_ckey, sizeof g_ckey, "%s", key);
	else if (!opts && strncmp(url, "file:", 5) != 0)
		snprintf(g_ckey, sizeof g_ckey, "%.*s", (int)strcspn(url, "#"), url);
	if (g_ckey[0] && !opts && mode != L_RELOAD
		&& cache_lookup(g_ckey, &g_cmeta)) {
		if ((mode == L_HISTORY || g_cmeta.fresh_until > (long)time(NULL))
			&& load_cached(g_ckey, &g_cmeta, frag) == 0)
			return 0;
		/* stale: ask the server whether it changed */
		cond[0] = '\0';
		if (g_cmeta.etag[0])
			snprintf(cond, sizeof cond, "If-None-Match: %s\r\n",
				g_cmeta.etag);
		else if (g_cmeta.last_modified[0])
			snprintf(cond, sizeof cond, "If-Modified-Since: %s\r\n",
				g_cmeta.last_modified);
		if (cond[0]) {
			memset(&co, 0, sizeof co);
			co.extra = cond;
			g_revalidating = 1;
		}
	}

	doc_reset();
	snprintf(g_url, sizeof g_url, "%s", url);
	g_loading = 1;
	g_last_draw = os_msec();
	g_status = 0;
	h_cc[0] = h_expires[0] = h_date[0] = h_etag[0] = h_lastmod[0] = '\0';
	memset(&cb, 0, sizeof cb);
	cb.status = on_status;
	cb.head = on_head;
	cb.body = on_body;
	cb.reset = on_reset;
	cb.header = on_header;
	message("Loading %.250s", url);
	draw(0);
	rc = fetch_ex(url, opts ? "POST" : "GET", opts ? opts
		: g_revalidating ? &co : NULL, &cb, &res);
	g_loading = 0;
	cookie_save();
	if (rc == 0 && g_status == 304 && g_revalidating
		&& load_cached(g_ckey, &g_cmeta, frag) == 0) {
		snprintf(g_info + strlen(g_info), sizeof g_info - strlen(g_info),
			", unchanged");
		return 0;
	}
	cache_finish(rc == 0 && !g_aborted && !g_unsupported,
		res.url[0] ? res.url : url);
	if (g_unsupported) {
		static char why[300];

		snprintf(why, sizeof why, "It is %s, which ub can't show yet.",
			g_ctype);
		error_page(res.url[0] ? res.url : url, why);
		return -1;
	}
	if (rc < 0 && !g_aborted && !g_started) {
		error_page(url, res.error);
		return -1;
	}
	if (!g_started)
		on_head(NULL, 200, "text/html", "", NULL);
	if (res.url[0])
		snprintf(g_url, sizeof g_url, "%s", res.url);
	finish_doc(frag);
	g_msg[0] = '\0';
	snprintf(g_info, sizeof g_info, "%s%s  %lu KB%s%s",
		res.tls ? "TLS" : strncmp(g_url, "gopher:", 7) == 0 ? "gopher"
		: "HTTP (not encrypted)", res.tls_resumed ? " (resumed)" : "",
		(unsigned long)res.body_bytes / 1024, "",
		g_doc.truncated || g_page.truncated ? "  truncated" : "");
	if (res.wire_bytes && res.wire_bytes < res.body_bytes)
		snprintf(g_info + strlen(g_info), sizeof g_info - strlen(g_info),
			"  (%lu KB compressed)", (unsigned long)res.wire_bytes / 1024);
	if (g_aborted)
		message("Stopped.", NULL);
	else if (rc < 0)
		message("%s", res.error);
	else if (res.status >= 400)
		snprintf(g_msg, sizeof g_msg, "HTTP %d", res.status);
	return 0;
}

/* --- history -------------------------------------------------------------- */

static void hist_save_pos(void)
{
	if (g_hpos >= 0) {
		g_hist[g_hpos].top = g_top;
		g_hist[g_hpos].sel = g_sel;
	}
}

static void hist_push(const char *url)
{
	int i;

	/* forward history is dropped when a new page is visited */
	for (i = g_hpos + 1; i < g_nhist; i++) {
		xfree(g_hist[i].url);
		xfree(g_hist[i].post_body);
		xfree(g_hist[i].post_key);
	}
	g_nhist = g_hpos + 1;
	if (g_nhist == HIST_MAX) {
		xfree(g_hist[0].url);
		xfree(g_hist[0].post_body);
		xfree(g_hist[0].post_key);
		memmove(g_hist, g_hist + 1, (HIST_MAX - 1) * sizeof g_hist[0]);
		g_nhist--;
	}
	memset(&g_hist[g_nhist], 0, sizeof g_hist[0]);
	g_hist[g_nhist].url = xstrdup(url);
	g_hist[g_nhist].sel = -1;
	g_hpos = g_nhist++;
}

/* go to a new URL */
static void visit(const char *url)
{
	hist_save_pos();
	load(url, NULL, L_NORMAL, NULL);
	hist_push(g_url);
}

/* send a form with POST: the answer is a new page, which remembers what
 * was sent (going back to it asks before sending it again) */
static void visit_post(const char *url, char *body, size_t len,
	const char *type)
{
	static unsigned long seq;
	struct fetch_opts o;
	char ref[URL_MAX], key[URL_MAX + 40];

	snprintf(ref, sizeof ref, "%s", g_url);
	/* the answer is kept in the cache under a name of its own */
	snprintf(key, sizeof key, "post:%lu.%lu:%.2000s", (unsigned long)time(NULL),
		++seq, url);
	memset(&o, 0, sizeof o);
	o.body = body;
	o.body_len = len;
	o.body_type = type;
	o.referer = ref;
	hist_save_pos();
	load(url, &o, L_NORMAL, key);
	hist_push(g_url);
	g_hist[g_hpos].post_key = xstrdup(key);
	g_hist[g_hpos].post_body = body;	/* the history keeps it */
	g_hist[g_hpos].post_len = len;
	snprintf(g_hist[g_hpos].post_type, sizeof g_hist[g_hpos].post_type,
		"%s", type);
}

/* load history entry h again (mode: L_HISTORY going back, L_RELOAD for
 * r): a form's answer from the cache, or sent again only when the user
 * says so */
static void reload_entry(struct hist *h, int mode)
{
	struct fetch_opts o;
	static struct cache_meta m;

	if (h->post_body == NULL) {
		load(h->url, NULL, mode, NULL);
		return;
	}
	if (mode == L_HISTORY && h->post_key && cache_lookup(h->post_key, &m)
		&& load_cached(h->post_key, &m, "") == 0)
		return;
	if (!confirm("This page was the answer to a form. Send the form "
		"again? (y/n) ")) {
		static const char html[] = "<title>Form answer</title>"
			"<h1>The answer to a form</h1><p>This page came from sending "
			"a form, which wasn't sent again. <b>r</b> sends it again.";

		load_string(html, h->url);
		return;
	}
	memset(&o, 0, sizeof o);
	o.body = h->post_body;
	o.body_len = h->post_len;
	o.body_type = h->post_type;
	load(h->url, &o, L_NORMAL, h->post_key);
}

static void back(void)
{
	struct hist *h;

	if (g_hpos <= 0) {
		message("Already at the first page.", NULL);
		return;
	}
	hist_save_pos();
	g_hpos--;
	h = &g_hist[g_hpos];
	reload_entry(h, L_HISTORY);
	scroll_to(h->top);
	if (link_visible(h->sel))
		g_sel = h->sel;
}

/* --- links ----------------------------------------------------------- */

/* the same document, apart from the fragment? */
static int same_doc(const char *a, const char *b)
{
	size_t la = strcspn(a, "#"), lb = strcspn(b, "#");

	return la == lb && strncmp(a, b, la) == 0;
}

static void go_url(const char *target)
{
	static char buf[URL_MAX];
	const char *f;

	/* a jump within the page */
	if (g_have_page && same_doc(target, g_url)
		&& (f = strchr(target, '#')) != NULL) {
		long ln = layout_anchor(&g_page, f + 1);

		hist_save_pos();
		hist_push(target);
		snprintf(g_url, sizeof g_url, "%s", target);
		if (ln >= 0) {
			g_top = ln;
			scroll_to(g_top);
			g_sel = first_visible();
		} else
			message("No anchor \"%s\" on this page.", f + 1);
		return;
	}
	/* a gopher search item: ask for the words */
	if (strncmp(target, "gopher://", 9) == 0) {
		const char *p = strchr(target + 9, '/');

		if (p && p[1] == '7' && !strchr(p, '?')) {
			char words[200], *o;
			size_t i;

			words[0] = '\0';
			if (!prompt("Search: ", words, sizeof words) || !words[0])
				return;
			snprintf(buf, sizeof buf, "%s?", target);
			o = buf + strlen(buf);
			for (i = 0; words[i] && o < buf + sizeof buf - 4; i++) {
				unsigned char c = (unsigned char)words[i];

				if (c == ' ')
					*o++ = '+';
				else if (c < 0x21 || c > 0x7E || c == '#'
					|| c == '%' || c == '+' || c == '&') {
					sprintf(o, "%%%02X", c);
					o += 3;
				} else
					*o++ = (char)c;
			}
			*o = '\0';
			target = buf;
		}
	}
	if (strncmp(target, "telnet:", 7) == 0 || strncmp(target, "mailto:", 7) == 0
		|| strncmp(target, "javascript:", 11) == 0) {
		message("ub can't follow %s links.", target);
		return;
	}
	visit(target);
}

/* --- form fields -------------------------------------------------------- */

/* keep the place when a field's value changes the layout */
static void relayout_keep(void)
{
	long t = g_top, k = g_sel;

	relayout(0);
	scroll_to(t);
	if (link_visible(k))
		g_sel = k;
}

/* the link index of a field (or -1) */
static long link_of(nodeid node)
{
	unsigned long k;

	for (k = 0; k < g_page.nlinks; k++)
		if (g_page.links[k].node == node)
			return (long)k;
	return -1;
}

static void select_link(long k)
{
	if (k < 0)
		return;
	g_sel = k;
	if (!link_visible(k)) {
		scroll_to((long)g_page.links[k].line - view_rows() / 3);
		g_sel = k;
	}
}

/* a menu over the page: the chosen item, or -1 */
static int menu(const char *title, char **items, int n, int cur)
{
	int rows = view_rows() - 2, w = (int)strlen(title), i, top = 0, k;

	if (rows > n)
		rows = n;
	for (i = 0; i < n; i++)
		if ((int)strlen(items[i]) > w)
			w = (int)strlen(items[i]);
	if (w > scr_cols - 6)
		w = scr_cols - 6;
	if (cur < 0 || cur >= n)
		cur = 0;
	for (;;) {
		int r0 = 1 + (view_rows() - rows - 2) / 2, c0 = (scr_cols - w - 4) / 2;

		if (cur < top)
			top = cur;
		if (cur >= top + rows)
			top = cur - rows + 1;
		draw(0);
		scr_fill(r0, c0, w + 4, ' ', CA_REV);
		scr_put(r0, c0 + 2, title, (int)strlen(title) > w ? w
			: (int)strlen(title), CA_REV | CA_BOLD);
		for (i = 0; i < rows; i++) {
			char buf[400];
			int a = top + i == cur ? CA_REV : 0;

			to_term(items[top + i], buf, sizeof buf);
			scr_fill(r0 + 1 + i, c0, w + 4, ' ', CA_REV);
			scr_fill(r0 + 1 + i, c0 + 1, w + 2, ' ', a ? 0 : 0);
			scr_put(r0 + 1 + i, c0 + 2, buf, (int)strlen(buf) > w ? w
				: (int)strlen(buf), a ? CA_REV : 0);
		}
		scr_fill(r0 + 1 + rows, c0, w + 4, ' ', CA_REV);
		scr_flush(0);
		k = scr_getkey(-1);
		switch (k) {
		case K_UP: case 'k':
			if (cur > 0)
				cur--;
			break;
		case K_DOWN: case 'j':
			if (cur + 1 < n)
				cur++;
			break;
		case K_PGUP: case 'b':
			cur = cur > rows ? cur - rows : 0;
			break;
		case K_PGDN: case ' ':
			cur = cur + rows < n ? cur + rows : n - 1;
			break;
		case K_HOME:
			cur = 0;
			break;
		case K_END:
			cur = n - 1;
			break;
		case '\r': case K_RIGHT:
			return cur;
		case 27: case 7: case 3: case K_LEFT: case 'q':
			return -1;
		}
	}
}

static void choose_option(struct field *f)
{
	nodeid opts[200];
	char *items[200];
	int n = forms_options(&g_forms, f, opts, 200), i, cur = 0, pick;

	if (n == 0) {
		message("The list is empty.", NULL);
		return;
	}
	for (i = 0; i < n; i++) {
		struct field tmp = *f;

		tmp.selected = opts[i];
		items[i] = xstrdup(forms_text(&g_forms, &tmp));
		if (opts[i] == f->selected)
			cur = i;
	}
	pick = menu("Choose:", items, n, cur);
	for (i = 0; i < n; i++)
		xfree(items[i]);
	if (pick >= 0) {
		forms_choose(f, opts[pick]);
		relayout_keep();
	}
}

/* does the form hold a password someone typed? */
static int has_password(nodeid form)
{
	int i;

	for (i = 0; i < g_forms.n; i++)
		if (g_forms.f[i].form == form && g_forms.f[i].type == FT_PASSWORD
			&& g_forms.f[i].value && g_forms.f[i].value[0])
			return 1;
	return 0;
}

static void submit(nodeid form, nodeid submitter)
{
	struct submission sub;
	const char *why = "";

	if (forms_submit(&g_forms, form, submitter, &g_base,
		g_load.cs == CS_WIN1252, &sub, &why) < 0) {
		message("Can't send the form: %s.", why);
		return;
	}
	if (strncmp(sub.url, "http:", 5) == 0 && has_password(form)
		&& !confirm("This sends a password unencrypted (http). Send it? "
		"(y/n) ")) {
		xfree(sub.body);
		return;
	}
	if (strcmp(sub.method, "POST") == 0)
		visit_post(sub.url, sub.body, sub.body_len, sub.type);
	else
		visit(sub.url);
}

/* Enter in a text field: the next one, or send the form (as browsers
 * do: with its first button, or alone when it's the only field) */
static void after_text(struct field *f)
{
	struct field *next = forms_next_text(&g_forms, f);
	int i, ntext = 0;

	if (next) {
		select_link(link_of(next->node));
		return;
	}
	if (!f->form)
		return;
	for (i = 0; i < g_forms.n; i++)
		if (g_forms.f[i].form == f->form && (g_forms.f[i].type == FT_SUBMIT
			|| g_forms.f[i].type == FT_IMAGE)) {
			submit(f->form, g_forms.f[i].node);
			return;
		}
	for (i = 0; i < g_forms.n; i++)
		if (g_forms.f[i].form == f->form && (g_forms.f[i].type == FT_TEXT
			|| g_forms.f[i].type == FT_PASSWORD))
			ntext++;
	if (ntext == 1)
		submit(f->form, 0);
}

static void edit_text(struct field *f)
{
	char buf[1024], label[64];
	const char *name = doc_attr(&g_doc, f->node, ATTR_PLACEHOLDER);

	if (name == NULL || !*name)
		name = doc_attr(&g_doc, f->node, ATTR_NAME);
	if (name == NULL || !*name)
		name = f->type == FT_PASSWORD ? "Password" : "Text";
	snprintf(label, sizeof label, "%.40s: ", name);
	snprintf(buf, sizeof buf, "%s", f->value ? f->value : "");
	if (!prompt_mask(label, buf, sizeof buf, f->type == FT_PASSWORD))
		return;
	forms_set_text(f, buf);
	relayout_keep();
	after_text(f);
}

/* a textarea: in $VISUAL/$EDITOR (vi), else on the status line */
static void edit_area(struct field *f)
{
	const char *ed = getenv("VISUAL");
	char path[600], cmd[800];
	unsigned char *text;
	size_t n;

	if (ed == NULL || !*ed)
		ed = getenv("EDITOR");
	if (ed == NULL || !*ed)
		ed = "vi";
	if (os_datapath(path, sizeof path, "textarea.txt") == NULL
		|| os_write_file(path, f->value ? f->value : "",
		f->value ? strlen(f->value) : 0, 0600) < 0) {
		edit_text(f);
		return;
	}
	snprintf(cmd, sizeof cmd, "%s %s", ed, path);
	scr_suspend();
	n = (size_t)system(cmd);
	scr_resume();
	(void)n;
	if ((text = os_read_file(path, &n)) != NULL) {
		char *t = (char *)text;

		t[n] = '\0';
		while (n && (t[n - 1] == '\n' || t[n - 1] == '\r'))
			t[--n] = '\0';
		forms_set_text(f, t);
		xfree(text);
		relayout_keep();
	}
	remove(path);
}

static void field_action(nodeid node)
{
	struct field *f = g_have_forms ? forms_field(&g_forms, node) : NULL;

	if (f == NULL) {
		message("That field can't be used.", NULL);
		return;
	}
	if (doc_attr(&g_doc, node, ATTR_DISABLED)) {
		message("That field is disabled.", NULL);
		return;
	}
	switch (f->type) {
	case FT_TEXT:
	case FT_PASSWORD:
		if (doc_attr(&g_doc, node, ATTR_READONLY))
			message("That field is read-only.", NULL);
		else
			edit_text(f);
		break;
	case FT_TEXTAREA:
		edit_area(f);
		break;
	case FT_CHECKBOX:
	case FT_RADIO:
		forms_click(&g_forms, f);
		relayout_keep();
		break;
	case FT_SELECT:
		choose_option(f);
		break;
	case FT_SUBMIT:
	case FT_IMAGE:
		submit(f->form, f->node);
		break;
	case FT_RESET:
		forms_reset(&g_forms, f->form);
		relayout_keep();
		message("The form is back as it was.", NULL);
		break;
	case FT_BUTTON:
		message("That button needs JavaScript.", NULL);
		break;
	case FT_FILE:
		message("ub can't upload files.", NULL);
		break;
	default:
		break;
	}
}

static void follow(void)
{
	const struct llink *k;
	const char *h;
	static struct url u;
	static char buf[URL_MAX];

	if (!link_visible(g_sel)) {
		message("No link selected (Up/Down or Tab select one).", NULL);
		return;
	}
	k = &g_page.links[g_sel];
	if (k->kind != LK_HREF) {
		field_action(k->node);
		return;
	}
	h = doc_attr(&g_doc, k->node, g_doc.nodes[k->node].tag == TAG_FRAME ?
		ATTR_SRC : ATTR_HREF);
	if (h == NULL || url_resolve(&g_base, h, &u) != URL_OK
		|| url_format(&u, buf, sizeof buf, 1) != URL_OK) {
		message("That link's URL is not usable.", NULL);
		return;
	}
	go_url(buf);
}

/* next link on screen (dir 1) or previous (-1), scrolling when there is
 * none on screen */
static void move_link(int dir)
{
	long k = g_sel, rows = view_rows();

	if (k < 0)
		k = dir > 0 ? -1 : (long)g_page.nlinks;
	for (k += dir; k >= 0 && k < (long)g_page.nlinks; k += dir) {
		long ln = (long)g_page.links[k].line;

		if (link_visible(k)) {
			g_sel = k;
			return;
		}
		if ((dir > 0 && ln >= g_top + rows) || (dir < 0 && ln < g_top))
			break;
	}
	/* none on screen that way: scroll a page, taking the next link if
	 * it's there */
	if (dir > 0 && g_top >= max_top()) {
		scr_bell();
		return;
	}
	if (dir < 0 && g_top == 0) {
		scr_bell();
		return;
	}
	scroll_to(g_top + dir * (rows - 2));
	if (k >= 0 && k < (long)g_page.nlinks && link_visible(k))
		g_sel = k;
}

/* Tab: the next link anywhere */
static void jump_link(int dir)
{
	long k = g_sel + dir;

	if (g_page.nlinks == 0) {
		scr_bell();
		return;
	}
	if (k < 0)
		k = (long)g_page.nlinks - 1;
	if (k >= (long)g_page.nlinks)
		k = 0;
	g_sel = k;
	if (!link_visible(k)) {
		g_top = (long)g_page.links[k].line - view_rows() / 3;
		scroll_to(g_top);
		g_sel = k;
	}
}

/* --- prompt ----------------------------------------------------------- */

/* a yes/no question on the status line */
static int confirm(const char *question)
{
	int r = scr_rows - 1, k;

	draw(0);
	scr_fill(r, 0, scr_cols, ' ', 0);
	scr_put(r, 0, question, (int)strlen(question), CA_BOLD);
	scr_cursor(r, (int)strlen(question) < scr_cols ?
		(int)strlen(question) : scr_cols - 1);
	scr_flush(0);
	k = scr_getkey(-1);
	scr_cursor(-1, -1);
	return k == 'y' || k == 'Y';
}

static int prompt(const char *label, char *buf, size_t n)
{
	return prompt_mask(label, buf, n, 0);
}

/* A line editor on the status line: 1 when Enter was pressed. mask:
 * show * for each character (passwords). */
static int prompt_mask(const char *label, char *buf, size_t n, int mask)
{
	size_t len = strlen(buf), pos = len;
	int r = scr_rows - 1, lw = (int)strlen(label);
	static char stars[1024];

	for (;;) {
		int k, avail = scr_cols - lw - 1, start = 0;

		/* show the part around the cursor */
		if ((int)pos > avail)
			start = (int)pos - avail;
		draw_status();
		scr_fill(r, 0, scr_cols, ' ', 0);
		scr_put(r, 0, label, lw, CA_BOLD);
		if (mask) {
			memset(stars, '*', len < sizeof stars ? len : sizeof stars - 1);
			scr_put(r, lw, stars + start, (int)len - start, 0);
		} else
			scr_put(r, lw, buf + start, (int)len - start, 0);
		scr_cursor(r, lw + (int)pos - start);
		scr_flush(0);
		k = scr_getkey(-1);
		switch (k) {
		case '\r':
			scr_cursor(-1, -1);
			return 1;
		case 27: case 7: case 3:
			scr_cursor(-1, -1);
			return 0;
		case 8: case 127:
			if (pos) {
				memmove(buf + pos - 1, buf + pos, len - pos + 1);
				pos--;
				len--;
			}
			break;
		case K_DEL: case 4:
			if (pos < len) {
				memmove(buf + pos, buf + pos + 1, len - pos);
				len--;
			}
			break;
		case K_LEFT: case 2:
			if (pos)
				pos--;
			break;
		case K_RIGHT: case 6:
			if (pos < len)
				pos++;
			break;
		case K_HOME: case 1:
			pos = 0;
			break;
		case K_END: case 5:
			pos = len;
			break;
		case 21:			/* ^U */
			buf[0] = '\0';
			len = pos = 0;
			break;
		case 11:			/* ^K */
			buf[pos] = '\0';
			len = pos;
			break;
		default:
			if (k >= 32 && k < 256 && k != 127 && len + 1 < n) {
				memmove(buf + pos + 1, buf + pos, len - pos + 1);
				buf[pos++] = (char)k;
				len++;
			}
		}
	}
}

/* what was typed at g: a URL, a file, or words to search for */
static void typed(const char *in, char *out, size_t n)
{
	const char *p;

	while (*in == ' ')
		in++;
	if (strstr(in, "://") || strncmp(in, "about:", 6) == 0)
		snprintf(out, n, "%s", in);
	else if (in[0] == '/' || strncmp(in, "./", 2) == 0)
		snprintf(out, n, "file://%s", in);
	else if (!strchr(in, ' ') && strchr(in, '.'))
		snprintf(out, n, "https://%s", in);
	else {
		char *o;

		snprintf(out, n, "%s", g_search);
		o = out + strlen(out);
		for (p = in; *p && o < out + n - 4; p++) {
			unsigned char c = (unsigned char)*p;

			if (c == ' ')
				*o++ = '+';
			else if (c < 0x21 || c > 0x7E || strchr("#%&+?=", c)) {
				sprintf(o, "%%%02X", c);
				o += 3;
			} else
				*o++ = (char)c;
		}
		*o = '\0';
	}
}

/* --- bookmarks ------------------------------------------------------- */

/* the bookmarks are a page: $UB_HOME/bookmarks.html, one link a line */
static void add_bookmark(void)
{
	char path[600], title[300], line[URL_MAX * 2 + 800], eu[URL_MAX + 200],
		et[700];
	const char *t = g_have_page ? doc_title(&g_doc) : "";
	long size, mtime;
	FILE *f;

	if (strncmp(g_url, "about:", 6) == 0) {
		message("Nothing to keep here.", NULL);
		return;
	}
	snprintf(title, sizeof title, "%.250s", *t ? t : g_url);
	if (!prompt("Bookmark as: ", title, sizeof title) || !title[0])
		return;
	if (os_datapath(path, sizeof path, "bookmarks.html") == NULL) {
		message("No place for bookmarks ($HOME or $UB_HOME).", NULL);
		return;
	}
	html_escape(g_url, eu, sizeof eu);
	html_escape(title, et, sizeof et);
	{
		/* (quotes too, for the attribute) */
		char *q;

		while ((q = strchr(eu, '"')) != NULL)
			*q = '\'';
	}
	if (os_file_info(path, &size, &mtime) < 0) {
		f = fopen(path, "w");
		if (f)
			fputs("<title>Bookmarks</title>\n<h1>Bookmarks</h1>\n"
				"<p>(ub adds a line with a; the file is " "bookmarks.html in "
				"~/.ub: edit it to change or remove them.)\n<ul>\n", f);
	} else
		f = fopen(path, "a");
	if (f == NULL) {
		message("Can't write the bookmarks file.", NULL);
		return;
	}
	chmod(path, 0600);
	snprintf(line, sizeof line, "<li><a href=\"%s\">%s</a>\n", eu, et);
	fputs(line, f);
	fclose(f);
	message("Bookmarked: %.250s", title);
}

static void show_bookmarks(void)
{
	char path[600], url[700];
	long size, mtime;

	if (os_datapath(path, sizeof path, "bookmarks.html") == NULL
		|| os_file_info(path, &size, &mtime) < 0) {
		message("No bookmarks yet (a adds this page).", NULL);
		return;
	}
	snprintf(url, sizeof url, "file://%s", path);
	visit(url);
}

/* --- find ------------------------------------------------------------ */

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static void find(int dir)
{
	const struct page *p = &g_page;
	size_t k = strlen(g_find);
	long ln, start, count;
	unsigned long from;

	if (!k || !g_have_page || p->nlines == 0)
		return;
	/* from just after (or before) the last match, else the screen top */
	start = g_find_line >= 0 && g_find_line >= g_top
		&& g_find_line < g_top + view_rows() ? g_find_line : g_top;
	from = g_find_line == start ? (unsigned long)g_find_off : 0;
	for (count = 0, ln = start; count <= (long)p->nlines; count++) {
		const struct lline *l = &p->lines[ln];
		unsigned long o, end = l->off + l->len;
		unsigned long hit = 0;
		int found = 0;

		for (o = l->off; o + k <= end; o++) {
			size_t i;

			if (count == 0 && (dir > 0 ? o <= from && from
				&& g_find_line == start : o >= from
				&& g_find_line == start))
				continue;
			for (i = 0; i < k; i++)
				if (lower((unsigned char)p->text[o + i])
					!= lower((unsigned char)g_find[i]))
					break;
			if (i == k) {
				hit = o;
				found = 1;
				if (dir > 0)
					break;
			}
		}
		if (found) {
			g_find_line = ln;
			g_find_off = (long)hit;
			if (ln < g_top || ln >= g_top + view_rows())
				scroll_to(ln - view_rows() / 3);
			return;
		}
		ln += dir;
		if (ln >= (long)p->nlines)
			ln = 0;
		if (ln < 0)
			ln = (long)p->nlines - 1;
	}
	g_find_line = -1;
	message("\"%s\" not found.", g_find);
}

/* --- main ------------------------------------------------------------- */

static void show_info(void)
{
	snprintf(g_msg, sizeof g_msg, "%.160s  %.40s  %.60s  %lu lines, %lu links",
		g_url, g_ctype[0] ? g_ctype : "", g_info, g_page.nlines,
		g_page.nlinks);
	g_msg_sticky = 1;
}

static void tls_note(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
}

int main(int argc, char **argv)
{
	char path[600], in[URL_MAX], go[URL_MAX];
	const char *start, *err = NULL;
	int quit = 0;

	signal(SIGPIPE, SIG_IGN);
	/* settings: ~/.ub/config, UB_<KEY> overriding */
	if (config_load(os_datapath(path, sizeof path, "config"), &err) < 0)
		fprintf(stderr, "ub: %s\n", err);
	start = argc > 1 ? argv[1] : config_str("home", "about:start");
	g_search = config_str("search", SEARCH_URL);
	scr_color = config_bool("color", 1);
	fetch_early_requests = config_bool("early_requests", 1);
	cookie_enabled = config_bool("cookies", 1);
	if (cookie_enabled)
		cookie_init(os_datapath(path, sizeof path, "cookies"));
	if (config_long("cache_kb", 2048) > 0)
		cache_init(os_datapath(path, sizeof path, "cache"),
			config_long("cache_kb", 2048) * 1024L);
	entropy_init(os_datapath(path, sizeof path, "seed"));
	/* the first run builds the trust store: say so, it takes a minute */
	tls_init(config_str("cafile", NULL), tls_note);
	if (doc_init(&g_doc, 0) < 0 || scr_open(config_str("charset", NULL)) < 0) {
		fprintf(stderr, "ub: needs a terminal\n");
		return 1;
	}
	if (argc > 1 && !strstr(start, "://") && strncmp(start, "about:", 6)) {
		long sz, mt;

		if (os_file_info(start, &sz, &mt) == 0) {
			/* a file here: file:// with its full path */
			char cwd[URL_MAX / 2];

			if (start[0] == '/' || getcwd(cwd, sizeof cwd) == NULL)
				snprintf(go, sizeof go, "file://%s", start);
			else
				snprintf(go, sizeof go, "file://%s/%s", cwd, start);
		} else
			typed(start, go, sizeof go);
		start = go;
	}
	visit(start);
	while (!quit) {
		int k;

		if (scr_check_size()) {
			/* keep the same text at the top */
			unsigned long off = g_have_page && g_top < (long)g_page.nlines ?
				g_page.lines[g_top].off : 0;
			long i;

			relayout(0);
			for (i = 0; i + 1 < (long)g_page.nlines
				&& g_page.lines[i + 1].off <= off; i++)
				;
			g_top = i;
			scroll_to(g_top);
			draw(1);
		} else
			draw(0);
		k = scr_getkey(-1);
		if (!g_msg_sticky)
			g_msg[0] = '\0';
		g_msg_sticky = 0;
		switch (k) {
		case 'q': case 'Q':
			quit = 1;
			break;
		case K_DOWN:
			move_link(1);
			break;
		case K_UP:
			move_link(-1);
			break;
		case '\t':
			jump_link(1);
			break;
		case K_BTAB:
			jump_link(-1);
			break;
		case K_RIGHT: case '\r':
			follow();
			break;
		case K_LEFT: case 'u': case 8: case 127:
			back();
			break;
		case ' ': case K_PGDN: case 6:
			scroll_to(g_top + view_rows() - 1);
			break;
		case 'b': case K_PGUP: case 2:
			scroll_to(g_top - (view_rows() - 1));
			break;
		case 'j': case 5:
			scroll_to(g_top + 1);
			break;
		case 'k': case 25:
			scroll_to(g_top - 1);
			break;
		case K_HOME: case '<':
			scroll_to(0);
			break;
		case K_END: case '>':
			scroll_to(max_top());
			break;
		case 'm':
			if (g_page.main_line >= 0)
				scroll_to(g_page.main_line);
			else if (layout_anchor(&g_page, "content") >= 0)
				scroll_to(layout_anchor(&g_page, "content"));
			else
				message("This page doesn't mark its main content.", NULL);
			break;
		case 'g': case 'G':
			in[0] = '\0';
			if (k == 'G')
				snprintf(in, sizeof in, "%s", g_url);
			if (prompt("URL or search: ", in, sizeof in) && in[0]) {
				typed(in, go, sizeof go);
				go_url(go);
			}
			break;
		case '/':
			if (prompt("Find: ", g_find, sizeof g_find)) {
				g_find_line = -1;
				find(1);
			}
			break;
		case 'n':
			find(1);
			break;
		case 'N':
			find(-1);
			break;
		case 'r':
			hist_save_pos();
			{
				long t = g_top;

				if (g_hpos >= 0 && g_hist[g_hpos].post_body)
					reload_entry(&g_hist[g_hpos], L_RELOAD);
				else
					load(g_url, NULL, L_RELOAD, NULL);
				scroll_to(t);
			}
			break;
		case 12:
			draw(1);
			break;
		case '=':
			show_info();
			break;
		case 'a':
			add_bookmark();
			break;
		case 'v':
			show_bookmarks();
			break;
		case '?': case 'h': case K_F1:
			go_url("about:help");
			break;
		case -1:
			break;
		default:
			break;
		}
	}
	scr_close();
	conn_close_all();
	entropy_save();
	return 0;
}
