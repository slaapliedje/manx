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

/* loading */
static int g_loading, g_started, g_gopher, g_aborted, g_unsupported;
static struct gophermap g_gmap;
static unsigned long g_bytes, g_last_draw;

struct hist {
	char *url;
	long top, sel;
};
static struct hist g_hist[HIST_MAX];
static int g_nhist, g_hpos = -1;

static int prompt(const char *label, char *buf, size_t n);

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
			snprintf(buf, sizeof buf, "(form field: Phase 4)");
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
		(unsigned long)max_lines, 0) < 0) {
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

static void on_head(void *ctx, int status, const char *ctype,
	const char *charset, const char *url)
{
	(void)ctx;
	(void)status;
	if (g_started)
		return;
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

static int on_body(void *ctx, const unsigned char *d, size_t n)
{
	unsigned long now;

	if (!g_started)
		on_head(ctx, 200, "text/html", "", NULL);
	if (g_unsupported)
		return -1;
	if (g_gopher)
		gophermap_feed(&g_gmap, d, n);
	else
		html_load_feed(&g_load, d, n);
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
}

/* show a page made here (errors, help) */
static void load_string(const char *html, const char *url)
{
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
	"z, Esc         stop loading     q         quit\n"
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
		"<h1>Can't load the page</h1><p>%s</p><p><b>%s</b></p>"
		"<p>r tries again, Left goes back.</p>", eu, ew);
	load_string(html, url);
	g_msg[0] = '\0';
}

/* get url into the page; 0, or -1 (the old page is gone either way) */
static int load(const char *url)
{
	static struct fetch_result res;
	struct fetch_cb cb;
	char frag[256];
	const char *h;
	int rc;

	g_msg[0] = '\0';
	frag[0] = '\0';
	if ((h = strchr(url, '#')) != NULL)
		snprintf(frag, sizeof frag, "%s", h + 1);
	if (strcmp(url, "about:help") == 0) {
		load_string(help_html, url);
		return 0;
	}
	if (strcmp(url, "about:start") == 0 || strcmp(url, "about:") == 0
		|| strcmp(url, "about:blank") == 0) {
		load_string(start_html, url);
		return 0;
	}
	if (g_have_page) {
		layout_free(&g_page);
		g_have_page = 0;
	}
	doc_free(&g_doc);
	if (doc_init(&g_doc, 0) < 0) {
		message("out of memory", NULL);
		return -1;
	}
	snprintf(g_url, sizeof g_url, "%s", url);
	g_loading = 1;
	g_started = g_gopher = g_aborted = g_unsupported = 0;
	g_bytes = 0;
	g_last_draw = os_msec();
	g_top = 0;
	g_sel = -1;
	g_info[0] = '\0';
	memset(&cb, 0, sizeof cb);
	cb.status = on_status;
	cb.head = on_head;
	cb.body = on_body;
	cb.reset = on_reset;
	message("Loading %s", url);
	draw(0);
	rc = fetch(url, "GET", &cb, &res);
	g_loading = 0;
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
	if (g_gopher)
		gophermap_end(&g_gmap);
	html_load_end(&g_load);
	if (res.url[0])
		snprintf(g_url, sizeof g_url, "%s", res.url);
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
	relayout(0);
	g_msg[0] = '\0';
	snprintf(g_info, sizeof g_info, "%s%s  %lu KB%s",
		res.tls ? "TLS" : strncmp(g_url, "gopher:", 7) == 0 ? "gopher"
		: "HTTP (not encrypted)", res.tls_resumed ? " (resumed)" : "",
		(unsigned long)res.body_bytes / 1024,
		g_doc.truncated || g_page.truncated ? "  truncated" : "");
	if (g_aborted)
		message("Stopped.", NULL);
	else if (rc < 0)
		message("%s", res.error);
	else if (res.status >= 400) {
		snprintf(g_msg, sizeof g_msg, "HTTP %d", res.status);
	}
	g_top = 0;
	if (frag[0]) {
		long ln = layout_anchor(&g_page, frag);

		if (ln >= 0)
			g_top = ln;
	}
	scroll_to(g_top);
	g_sel = first_visible();
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
	for (i = g_hpos + 1; i < g_nhist; i++)
		xfree(g_hist[i].url);
	g_nhist = g_hpos + 1;
	if (g_nhist == HIST_MAX) {
		xfree(g_hist[0].url);
		memmove(g_hist, g_hist + 1, (HIST_MAX - 1) * sizeof g_hist[0]);
		g_nhist--;
	}
	g_hist[g_nhist].url = xstrdup(url);
	g_hist[g_nhist].top = 0;
	g_hist[g_nhist].sel = -1;
	g_hpos = g_nhist++;
}

/* go to a new URL */
static void visit(const char *url)
{
	hist_save_pos();
	load(url);
	hist_push(g_url);
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
	load(h->url);
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
		message("Forms come in Phase 4.", NULL);
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

/* A line editor on the status line: 1 when Enter was pressed. */
static int prompt(const char *label, char *buf, size_t n)
{
	size_t len = strlen(buf), pos = len;
	int r = scr_rows - 1, lw = (int)strlen(label);

	for (;;) {
		int k, avail = scr_cols - lw - 1, start = 0;

		/* show the part around the cursor */
		if ((int)pos > avail)
			start = (int)pos - avail;
		draw_status();
		scr_fill(r, 0, scr_cols, ' ', 0);
		scr_put(r, 0, label, lw, CA_BOLD);
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

		snprintf(out, n, "%s", SEARCH_URL);
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
	char seed[600], in[URL_MAX], go[URL_MAX];
	const char *start = argc > 1 ? argv[1] : "about:start";
	int quit = 0;

	signal(SIGPIPE, SIG_IGN);
	entropy_init(os_datapath(seed, sizeof seed, "seed"));
	/* the first run builds the trust store: say so, it takes a minute */
	tls_init(getenv("UB_CAFILE"), tls_note);
	if (doc_init(&g_doc, 0) < 0 || scr_open(getenv("UB_CHARSET")) < 0) {
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

				load(g_url);
				scroll_to(t);
			}
			break;
		case 12:
			draw(1);
			break;
		case '=':
			show_info();
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
