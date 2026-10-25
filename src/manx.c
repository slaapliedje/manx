/*
 * manx - a web browser for 68030 System V Unix: the text frontend.
 *
 *   manx [URL|FILE]
 *
 * Keys (as in Lynx, plus vi's):
 *   Up/Down      previous/next link (scrolling when it's off screen)
 *   Right, Enter follow the link          Left, u, Backspace  back
 *   Tab/Shift-Tab  next/previous link anywhere on the page
 *   Space, PgDn, b, PgUp  page down/up    j/k  line down/up
 *   Home/End, </>  top/bottom             m    the main content
 *   g  go to a URL (or search)  G  edit the current URL
 *   /  find  n/N  next/previous match     =    page and link information
 *   r  reload   ^L  redraw   z/Esc  stop loading (or a refresh)
 *   F  the page's feed   ?  help   q  quit
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
#include "feed.h"
#include "layout.h"
#include "screen.h"
#include "entropy.h"
#include "tls.h"
#include "conn.h"
#include "dns.h"
#include "url.h"
#include "fetch.h"
#include "http.h"
#include "utf8.h"
#include "forms.h"
#include "cookie.h"
#include "sites.h"
#include "config.h"
#include "cache.h"
#include "pageimg.h"
#include "pagecss.h"
#include "frames.h"
#include "css.h"
#include "update.h"
#include <time.h>

#define HIST_MAX	64
#define PANE_OUT	8		/* the page's background goes this far
					 * out to the pane's sides (its inset) */
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
static int g_sel_shown = 1;		/* ... marked: always in a terminal;
					 * in a window (with a mouse) only once
					 * keys have moved among the links */
static char g_find[128];
static long g_find_line = -1, g_find_off;
static char g_msg[300];			/* the status line's message */
static int g_msg_sticky;		/* ... kept until the next key */
static char g_info[200];		/* how the page came (TLS etc.) */
static struct forms g_forms;		/* its form fields */
static int g_have_forms;
static const char *g_search = SEARCH_URL;
static int g_images = 1;		/* config images: show them (X11) */
static int g_img_busy;			/* the page's images aren't all in */
static int g_css_busy;			/* nor its linked style sheets */
static int g_img_relayout;		/* sizes came in: lay out again */
static unsigned long g_img_drawn, g_img_relaid;	/* when last drawn, laid out */
static unsigned long g_img_flushed;	/* when the status (and an image's new
					 * rows) last went out */
static int g_img_framed;		/* the frame has the image being decoded */
static int g_keyq[16];			/* keys read while loading */
static int g_nkeyq;
/* the #fragment the page was opened at: gone back to as images come in
 * and the lines above it grow, until the user moves */
static char g_frag[128];

/* loading */
static int g_loading, g_started, g_gopher, g_aborted, g_unsupported;
static struct gophermap g_gmap;
static struct feed g_feedc;		/* an RSS or Atom feed, made a page */
static int g_feed;
static unsigned long g_bytes, g_last_draw;

/* where the page asked to go on to (a Refresh header, <meta
 * http-equiv=refresh>), and when (os_msec) */
#define REFRESH_HOPS_MAX	5	/* refreshes followed in a row */
static char g_refresh_url[URL_MAX];
static unsigned long g_refresh_at;
static int g_refresh_hops, g_refreshing;

/* a page of frames (html/frames.h): which document each frame shows,
 * "name\turl" lines; and what the next load is to show in them */
static char *g_frames;
static const char *g_fchosen;

struct hist {
	char *url;
	long top, sel;
	/* the answer to a form sent with POST: what was sent */
	char *post_body;
	size_t post_len;
	char post_type[80];
	char *post_key;			/* its answer in the cache */
	char *frames;			/* a page of frames: what each showed */
};
static struct hist g_hist[HIST_MAX];
static int g_nhist, g_hpos = -1;

/* prompt_mask's flags */
#define PM_MASK		1	/* show * for each character */
#define PM_URL		2	/* in the window's URL field, if there is one */

static int prompt(const char *label, char *buf, size_t n);
static int prompt_mask(const char *label, char *buf, size_t n, int flags);
static int confirm(const char *question);
static void refresh_plan(const char *content);
static void feed_hint(int status);

/* a plain line's height, in the screen's units */
static int plain_h(void)
{
	const struct lmetrics *m = scr_metrics();
	int a;

	return m ? m->height(m->ctx, 0, 0, &a) : 1;
}

/* the page's lines that fit on the screen from line top (past the end,
 * as many plain lines as would) */
static int lines_fit(long top)
{
	int h = scr_pane_h(), y = 0, n = 0;
	long ln;

	if (scr_metrics() == NULL)
		return h;
	if (g_have_page && g_page.heights)
		for (ln = top < 0 ? 0 : top; ln < (long)g_page.nlines; ln++) {
			y += g_page.heights[ln].height;
			if (y > h)
				return n > 0 ? n : 1;
			n++;
		}
	n += (h - y) / plain_h();
	return n > 0 ? n : 1;
}

static int view_rows(void)
{
	return lines_fit(g_top);
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
	if (sa & SA_ONDARK)
		a |= CA_ONDARK;
	if (sa & SA_FIELD)
		a |= CA_FIELD;
	if (link) {
		a |= CA_LINK;
		if (link - 1 == g_sel && g_sel_shown)
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
			cell_attr(sp->attr, sp->link) | CA_FG(sp->color));
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

/* how wide n bytes at s are in span sp's look: text in its font, or the
 * images the bytes stand for */
static int run_w(const struct page *p, const struct lspan *sp, const char *s, int n)
{
	const struct lmetrics *m = scr_metrics();

	if (sp->face & LF_IMAGE)
		return layout_images_w(p, s, n);
	return m->width(m->ctx, sp->attr, sp->face, s, n);
}

/* the images n bytes at s stand for, x across, sitting on the baseline
 * base */
static void draw_images(const struct page *p, int x, int base, const char *s, int n,
	int attr)
{
	for (; n >= LAYOUT_IMG_BYTES; s += LAYOUT_IMG_BYTES, n -= LAYOUT_IMG_BYTES) {
		long k = layout_image(p, s);

		if (k < 0) {
			int sw = layout_spacer_w(s);	/* (a grid's gap, a float's) */

			if (sw != LAYOUT_NO_SPACER)
				x += sw;
			continue;
		}
		scr_image_draw(pimg_screen(p, k), x, base - p->images[k].h,
			p->images[k].w, p->images[k].h, attr);
		x += p->images[k].w;
	}
}

/* how wide text of line ln is, from byte off for n bytes, in its spans'
 * looks (fonts) */
static int text_w_px(const struct lline *l, unsigned long s, unsigned long off,
	unsigned long n)
{
	const struct page *p = &g_page;
	unsigned long end = off + n;
	int w = 0;

	(void)l;
	while (off < end) {
		unsigned long next = end;
		const struct lspan *sp;

		s = layout_span_at(p, s, off);
		sp = &p->spans[s];
		if (s + 1 < p->nspans && p->spans[s + 1].off < end)
			next = p->spans[s + 1].off;
		w += run_w(p, sp, p->text + off, (int)(next - off));
		off = next;
	}
	return w;
}

/* line ln of page p (the page, or a layer of it) in fonts, its top at y
 * in the pane, x0 across */
static void draw_line_px(const struct page *p, int x0, int y, long ln)
{
	const struct lline *l = &p->lines[ln];
	const struct lheight *lh = &p->heights[ln];
	unsigned long off = l->off, end = l->off + l->len, s = l->span;
	int x = x0 + l->indent;

	while (off < end) {
		unsigned long next = end;
		const struct lspan *sp;
		int a;

		s = layout_span_at(p, s, off);
		sp = &p->spans[s];
		if (s + 1 < p->nspans && p->spans[s + 1].off < end)
			next = p->spans[s + 1].off;
		/* (in fonts, italics and headings look like what they are:
		 * no underline standing in for them, as on a terminal) */
		/* (a grid cell's line keeps its own baseline) */
		a = sp->asc ? sp->asc : lh->ascent;

		if (sp->face & LF_IMAGE)
			draw_images(p, x, y + a, p->text + off, (int)(next - off),
				cell_attr(sp->attr, sp->link));	/* (pictures: no colour) */
		else
			scr_text(x, y, a, p->text + off, (int)(next - off),
				cell_attr(sp->attr & (sp->face & (LF_ITALIC | LF_HMASK) ?
				~SA_UNDER : 0xFF), sp->link) | CA_FG(sp->color), sp->face);
		x += run_w(p, sp, p->text + off, (int)(next - off));
		off = next;
	}
	/* a find match on this line */
	if (p == &g_page && ln == g_find_line && g_find[0]) {
		unsigned long mo = (unsigned long)g_find_off;
		size_t k = strlen(g_find);

		if (mo >= l->off && mo + k <= end) {
			unsigned long ms = layout_span_at(p, l->span, mo);

			scr_text(l->indent + text_w_px(l, l->span, l->off, mo - l->off),
				y, p->spans[ms].asc ? p->spans[ms].asc : lh->ascent,
				p->text + mo, (int)k, CA_MARK | CA_REV, p->spans[ms].face);
		}
	}
}

/* the top of line ln of page p, below its first's (its foot, past the
 * last) */
static long line_top(struct page *p, unsigned long ln)
{
	const long *t = layout_tops(p);

	if (t == NULL)
		return 0;
	return t[ln < p->nlines ? ln : p->nlines];
}

/* page p's layers in the order they're drawn: by z, the document's among
 * equals (NULL: none, or no memory) */
static long *layer_order(const struct page *p)
{
	unsigned long i, k;
	long *ord;

	if (p->nlayers == 0 || (ord = xmalloc(p->nlayers * sizeof *ord)) == NULL)
		return NULL;
	for (i = 0; i < p->nlayers; i++) {
		/* (insertion: there are few) */
		for (k = i; k > 0 && p->layers[ord[k - 1]].z > p->layers[i].z; k--)
			ord[k] = ord[k - 1];
		ord[k] = (long)i;
	}
	return ord;
}

/* where layer ly of page p goes in the pane: its line 0's top (p's at y0,
 * x0 across), and across; h: the pane's height */
static long layer_y(struct page *p, const struct llayer *ly, long y0, int h)
{
	if (ly->fixed == 1)
		return ly->dy;
	if (ly->fixed == 2)
		return h + ly->dy;
	return y0 + line_top(p, ly->line) + ly->dy;
}

/* background picture k of the page (lbox.bgi) over a box whose border
 * box is at x, y of the pane, w x h, and out more to each side */
static void draw_bg_pic(unsigned k, int x, long y, int w, long h, int out)
{
	const struct lbgimg *g;
	void *img;
	int iw, ih;
	long ax, ay, aw, ah, tx, ty;

	if (k == 0 || k > g_page.nbgimgs || (img = pimg_bg_screen(&g_page, k, &iw, &ih)) == NULL)
		return;
	g = &g_page.bgimgs[k - 1];
	/* placed in its padding box, or the window (fixed) */
	if (g->fixed) {
		ax = ay = 0;
		aw = scr_pane_w();
		ah = scr_pane_h();
	} else {
		ax = x + g->in[3];
		ay = y + g->in[0];
		aw = w - g->in[1] - g->in[3];
		ah = h - g->in[0] - g->in[2];
	}
	tx = ax + (g->pct[0] ? g->pos[0] * (aw - iw) / 1000 : g->pos[0]);
	ty = ay + (g->pct[1] ? g->pos[1] * (ah - ih) / 1000 : g->pos[1]);
	scr_tile(x - out, (int)y, w + 2 * out, (int)h, img, (int)tx, (int)ty, g->rep);
}

/* page p's layers (positioned elements), its line 0's top at y0 in the
 * pane, x0 across, in z order, each one's boxes, then its lines, then its
 * own layers; h: the pane's height */
static void draw_layers(struct page *p, int x0, long y0, int h)
{
	unsigned long i, n = p->nlayers;
	long *ord = layer_order(p);

	if (ord == NULL)
		return;
	for (i = 0; i < n; i++) {
		struct llayer *ly = &p->layers[ord[i]];
		struct page *lp = &ly->pg;
		long ly0 = layer_y(p, ly, y0, h), top;
		int lx = (ly->fixed ? 0 : x0) + ly->x;
		unsigned long b, ln;

		for (b = 0; b < lp->nboxes * LBOX_KINDS; b++) {
			const struct lbox *bx = &lp->boxes[b % lp->nboxes];
			long by = ly0 + line_top(lp, bx->line) + bx->dy;

			if (bx->kind == b / lp->nboxes && by < h && by + bx->h > 0) {
				if (bx->side)
					scr_side(lx + bx->x, (int)by, bx->w, (int)bx->h, bx->bg,
						bx->side, bx->cut[0], bx->cut[1]);
				else
					scr_rect(lx + bx->x, (int)by, bx->w, (int)bx->h, bx->bg,
						bx->border, bx->border_rgb);
				draw_bg_pic(bx->bgi, lx + bx->x, by, bx->w, bx->h, 0);
			}
		}
		if (lp->heights)
			for (ln = 0; ln < lp->nlines; ln++) {
				top = ly0 + line_top(lp, ln);
				if (top >= h)
					break;
				if (top + lp->heights[ln].height + lp->heights[ln].over > 0)
					draw_line_px(lp, lx, (int)top, (long)ln);
			}
		draw_layers(lp, lx, ly0, h);
	}
	xfree(ord);
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
	else if (g_sel >= 0 && (unsigned long)g_sel < g_page.nlinks
		&& (g_sel_shown || !(g_css_busy || g_img_busy))) {
		/* the selected link's URL (in a window, while the page's
		 * pictures are coming and no key has moved among the links:
		 * how far they are instead) */
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
	} else if (g_css_busy || g_img_busy) {
		/* (and how much of the one being fetched is in: on a slow
		 * link a fetch is minutes, and this shows it moving) */
		unsigned long b = g_css_busy ? pcss_fetching() : pimg_fetching();
		char prog[40];
		int total, done;

		if (g_css_busy)
			pcss_count(&total, &done);
		else
			pimg_count(&total, &done);
		prog[0] = '\0';
		if (b)
			snprintf(prog, sizeof prog, "  %lu KB", b / 1024);
		snprintf(buf, sizeof buf, "%s%s%s %d of %d%s  (z: stop)", g_info,
			g_info[0] ? "  " : "", g_css_busy ? "style sheets" : "images",
			done, total, prog);
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
	/* the page's colours, as the screen shows them */
	scr_palette(g_page.palette, g_have_page ? g_page.npalette : 0);
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
	scr_title(title);
	if (g_have_page && scr_metrics() && g_page.heights) {
		/* in fonts: lines as tall as they are, down to the pane's
		 * bottom, the last one cut off there (a line with a tall
		 * picture in it would otherwise show nothing until the whole
		 * picture fits) */
		int y = 0, h = scr_pane_h();
		long ln;
		const long *tops = layout_tops(&g_page);

		if (tops) {
			long y0 = tops[g_top];
			unsigned long b;

			/* the page's background, then the boxes under the text:
			 * blocks', tables' and cells' backgrounds and borders */
			/* (the page's background: the page's, scrolled with
			 * it, so that only the strip a scroll uncovers is
			 * painted again; out to the pane's sides) */
			long ph = tops[g_page.nlines] + h;

			if (g_page.page_bg != LBOX_NONE)
				scr_rect(-PANE_OUT, (int)-y0, scr_pane_w() + 2 * PANE_OUT, (int)ph,
					g_page.page_bg, 0, 0);
			if (g_page.page_bgi)
				draw_bg_pic(g_page.page_bgi, 0, -y0, scr_pane_w(), ph, PANE_OUT);
			/* (blocks', then floats', then inline boxes') */
			for (b = 0; b < g_page.nboxes * LBOX_KINDS; b++) {
				const struct lbox *bx = &g_page.boxes[b % g_page.nboxes];
				long by;

				if (bx->line > g_page.nlines || bx->kind != b / g_page.nboxes)
					continue;
				by = tops[bx->line] - y0 + bx->dy;
				if (by < h && by + bx->h > 0) {
					if (bx->side)
						scr_side(bx->x, (int)by, bx->w, (int)bx->h, bx->bg,
							bx->side, bx->cut[0], bx->cut[1]);
					else
						scr_rect(bx->x, (int)by, bx->w, (int)bx->h, bx->bg,
							bx->border, bx->border_rgb);
					draw_bg_pic(bx->bgi, bx->x, by, bx->w, bx->h, 0);
				}
			}
			/* lines above whose tall content (a cell's picture)
			 * reaches down into view */
			for (ln = g_top; ln > 0 && y0 - tops[ln] < g_page.max_over; ln--)
				;
			for (; ln < g_top; ln++)
				if (tops[ln + 1] + g_page.heights[ln].over > y0)
					draw_line_px(&g_page, 0, (int)(tops[ln] - y0), ln);
		}
		for (ln = g_top; ln < (long)g_page.nlines && y < h; ln++) {
			draw_line_px(&g_page, 0, y, ln);
			y += g_page.heights[ln].height;
		}
		/* positioned elements, over it all */
		if (tops)
			draw_layers(&g_page, 0, -tops[g_top], h);
	} else if (g_have_page)
		for (i = 0; i < rows; i++) {
			long ln = g_top + i;

			if (ln >= (long)g_page.nlines)
				break;
			draw_line(1 + i, ln);
		}
	draw_status();
	/* the window's controls, where there are any */
	scr_url(g_url);
	scr_scroll(g_top, rows, g_have_page ? (long)g_page.nlines : 0);
	scr_state(g_hpos > 0, g_hpos + 1 < g_nhist, g_loading || g_img_busy
		|| g_css_busy);
	scr_cursor(-1, -1);
	scr_flush(full);
}

/* --- the page ------------------------------------------------------------ */

static struct lmetrics g_lm;		/* the screen's, with images' sizes */

static int image_size(void *ctx, nodeid node, int *w, int *h)
{
	(void)ctx;
	return pimg_size(node, w, h);
}

/* how the page is measured: the screen's metrics and, when it shows
 * them, images' own sizes; NULL on a terminal */
static const struct lmetrics *page_metrics(void)
{
	const struct lmetrics *m = scr_metrics();

	if (m == NULL)
		return NULL;
	g_lm = *m;
	g_lm.image = g_images && scr_pixels() ? image_size : NULL;
	g_lm.image_ctx = NULL;
	g_lm.view_h = scr_pane_h();
	return &g_lm;
}

static void relayout(long max_lines)
{
	if (g_have_page)
		layout_free(&g_page);
	if (layout_run_m(&g_page, &g_doc, scr_metrics() ? scr_pane_w() : scr_cols,
		scr_cs, (unsigned long)max_lines, 0, g_have_forms ? &g_forms : NULL,
		page_metrics()) < 0) {
		g_have_page = 0;
		message("out of memory for the layout", NULL);
		return;
	}
	g_have_page = 1;
	/* (sizes may have changed: images to decode again) */
	if (g_lm.image) {
		pimg_bgs(&g_page);
		g_img_busy = 1;
	}
}

static long max_top(void)
{
	long m;

	if (scr_metrics() && g_page.heights) {
		/* the top from which the last lines fill the pane */
		int y = 0, h = scr_pane_h();

		for (m = (long)g_page.nlines - 1; m >= 0; m--) {
			y += g_page.heights[m].height;
			if (y > h)
				return m + 1;
		}
		return 0;
	}
	m = (long)g_page.nlines - view_rows();
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
static char h_refresh[URL_MAX + 40];

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
	else if (strcmp(low, "refresh") == 0)
		dst = h_refresh, n = sizeof h_refresh;
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
	return is_html(ct) || strncmp(ct, "text/", 5) == 0 || feed_type(ct);
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
	g_feed = !g_gopher && feed_type(ctype);
	g_unsupported = !g_gopher && !g_feed && !is_html(ctype)
		&& strncmp(ctype, "text/", 5) != 0;
	if (g_unsupported)
		return;
	g_doc.img_w = scr_pixels() ? scr_pane_w() : 0;	/* (srcset) */
	html_load_begin(&g_load, &g_doc, charset,
		!g_gopher && !g_feed && !is_html(ctype));
	if (g_gopher)
		gophermap_begin(&g_gmap, feed_html, NULL);
	if (g_feed)
		feed_begin(&g_feedc, feed_html, NULL);
	g_started = 1;
	/* keep it on disk, if it's a page worth keeping */
	g_caching = g_ckey[0] && status == 200 && cacheable_type(ctype)
		&& cache_begin(g_ckey) == 0;
}

/* a key while loading: z, Esc, ^C, ^G stop it; others wait for the page
 * (not a click or the scrollbar: where they were may move) */
static int stop_asked(void)
{
	int k;

	while ((k = scr_getkey(0)) >= 0) {
		if (k == 'z' || k == 27 || k == 3 || k == 7 || k == 'q'
			|| k == K_CLOSE)
			return 1;
		if (k != K_MOUSE && k != K_SCROLL
			&& g_nkeyq < (int)(sizeof g_keyq / sizeof g_keyq[0]))
			g_keyq[g_nkeyq++] = k;
	}
	return 0;
}

static void feed_body(const unsigned char *d, size_t n)
{
	if (g_gopher)
		gophermap_feed(&g_gmap, d, n);
	else if (g_feed)
		feed_feed(&g_feedc, d, n);
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
	pimg_end();
	pcss_end();
	g_css_busy = 0;
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
	pimg_end();
	pcss_end();
	g_img_busy = g_css_busy = 0;
	g_refresh_url[0] = '\0';
	xfree(g_frames);
	g_frames = NULL;
	doc_free(&g_doc);
	doc_init(&g_doc, 0);
	g_doc.img_w = scr_pixels() ? scr_pane_w() : 0;	/* (srcset) */
	html_load_begin(&g_load, &g_doc, "utf-8", 0);
	html_load_feed(&g_load, (const unsigned char *)html, strlen(html));
	html_load_end(&g_load);
	snprintf(g_url, sizeof g_url, "%s", url);
	url_parse(url, &g_base);
	g_ctype[0] = '\0';
	/* (not what the page before came with) */
	snprintf(g_info, sizeof g_info, "%s",
		strncmp(url, "about:", 6) == 0 ? "Manx's own page" : "");
	make_forms();
	relayout(0);
	g_top = 0;
	g_sel = first_visible();
}

static const char help_html[] =
	"<title>Manx help</title><h1>Keys</h1><pre>"
	"Up/Down        previous/next link\n"
	"Right, Enter   follow the link\n"
	"Left, u, BkSp  back             f         forward\n"
	"Tab/Shift-Tab  next/previous link anywhere\n"
	"Space, PgDn    page down        b, PgUp   page up\n"
	"j/k            line down/up     Home/End  top/bottom\n"
	"m              where the reading starts (past the site's menus)\n"
	"g              go to a URL, or search for words\n"
	"G              edit the current URL\n"
	"/              find             n/N       next/previous match\n"
	"=              page and link information\n"
	"r              reload           ^L        redraw\n"
	"a              bookmark this page  v      the bookmarks\n"
	"F              the page's feed (or the site's, when its pages are\n"
	"               walled off or need JavaScript)\n"
	"z, Esc         stop loading, or stay when a page would go on\n"
	"q              quit\n"
	"Z              xmanx: the window fills the screen, or back\n"
	"</pre><h1>Forms</h1><p>Select a field (Up/Down, Tab) and press Enter: "
	"type in a text field (Enter moves to the next; in the last one it "
	"sends the form), tick a checkbox, choose from a list, or press a "
	"button. A text area opens in $EDITOR (vi).</p>"
	"<h1>Settings</h1><p>In ~/.manx/config, one <i>key = value</i> a line "
	"(or as MANX_KEY in the environment):</p><pre>"
	"start = URL            the start page\n"
	"search = URL           where g sends words (DuckDuckGo Lite)\n"
	"charset = utf-8        the terminal's: utf-8, latin1, ascii\n"
	"color = off            no colours\n"
	"link_color = blue      links' colour (cyan; blue suits white)\n"
	"proportional = off     xmanx: the page in a fixed font\n"
	"cookies = off          no cookies\n"
	"geometry = 120x40+0+0  xmanx: the window's columns, rows (and place;\n"
	"                       or: xmanx -geometry 120x40 URL)\n"
	"images = off           xmanx: no pictures, only their alt text\n"
	"stylesheets = off      ignore pages' &lt;style&gt; (what they hide shows)\n"
	"cache_kb = 2048        the disk cache's size (0: none)\n"
	"cafile = FILE          the PEM bundle of trusted roots\n"
	"early_requests = off   send nothing before the certificate is "
	"checked\n"
	"transputer = off       don't share certificate checks with the "
	"ATW800/2's T425\n"
	"transputer_program = FILE  its program (tpsig.btl in ~/.manx)\n"
	"update_check = off     never ask once a day whether there's a newer "
	"Manx\n"
	"</pre><p>Start pages: <a href=\"about:start\">about:start</a>. "
	"A newer Manx: <a href=\"about:update\">about:update</a>.";

static const char start_html[] =
	"<title>Manx</title><h1>Manx - a web browser for %s Unix</h1>"
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
	pimg_end();
	pcss_end();
	g_img_busy = g_css_busy = 0;
	g_img_relayout = 0;
	g_refresh_url[0] = '\0';
	xfree(g_frames);
	g_frames = NULL;
	doc_free(&g_doc);
	doc_init(&g_doc, 0);
	g_started = g_gopher = g_feed = g_aborted = g_unsupported = 0;
	g_bytes = 0;
	g_top = 0;
	g_sel = -1;
	g_sel_shown = scr_pixels() == NULL;
	g_info[0] = '\0';
}

/* frames_get: a frame's document, fetched whole */
struct fgot {
	struct frame_bytes *out;
	size_t cap;
};

static void fr_head(void *ctx, int status, const char *ctype,
	const char *charset, const char *url)
{
	struct fgot *g = ctx;

	(void)status;
	(void)ctype;
	snprintf(g->out->charset, sizeof g->out->charset, "%s", charset);
	snprintf(g->out->url, sizeof g->out->url, "%s", url ? url : "");
}

static int fr_body(void *ctx, const unsigned char *d, size_t n)
{
	struct fgot *g = ctx;
	struct frame_bytes *o = g->out;

	if (o->len + n > g->cap) {
		size_t cap = g->cap ? g->cap * 2 : 16384;
		unsigned char *p;

		while (cap < o->len + n)
			cap *= 2;
		if ((p = xrealloc(o->data, cap)) == NULL)
			return -1;
		o->data = p;
		g->cap = cap;
	}
	memcpy(o->data + o->len, d, n);
	o->len += n;
	return stop_asked() ? -1 : 0;
}

static void fr_reset(void *ctx)
{
	struct fgot *g = ctx;

	g->out->len = 0;
}

static int get_frame(void *ctx, const char *url, struct frame_bytes *out)
{
	static struct fetch_result res;
	struct fetch_opts o;
	struct fetch_cb cb;
	struct fgot g;

	(void)ctx;
	/* (a page from the network gets no local files, as for its
	 * pictures and style sheets) */
	if (strncmp(url, "file:", 5) == 0 && strncmp(g_url, "file:", 5) != 0) {
		snprintf(out->error, sizeof out->error, "a local file");
		return -1;
	}
	memset(&g, 0, sizeof g);
	g.out = out;
	memset(&cb, 0, sizeof cb);
	cb.ctx = &g;
	cb.status = on_status;
	cb.head = fr_head;
	cb.body = fr_body;
	cb.reset = fr_reset;
	memset(&o, 0, sizeof o);
	o.referer = g_url;
	message("Loading frame %.250s", url);
	draw(0);
	if (fetch_ex(url, "GET", &o, &cb, &res) < 0) {
		snprintf(out->error, sizeof out->error, "%s", res.error);
		return -1;
	}
	return 0;
}

/* the page is frames: make them one page, in its place */
static void frames_page(void)
{
	char *state, *html;

	html = frames_compose(&g_doc, g_load.tree.frameset, &g_base,
		g_fchosen ? g_fchosen : "", get_frame, NULL, &state);
	if (html == NULL)
		return;			/* (the frame links stay) */
	doc_free(&g_doc);
	doc_init(&g_doc, 0);
	g_doc.img_w = scr_pixels() ? scr_pane_w() : 0;	/* (srcset) */
	html_load_begin(&g_load, &g_doc, "utf-8", 0);
	html_load_feed(&g_load, (const unsigned char *)html, strlen(html));
	html_load_end(&g_load);
	xfree(html);
	xfree(g_frames);
	g_frames = state;
}

/* the document is complete: its base URL, forms, layout, the fragment */
static void finish_doc(const char *frag)
{
	if (g_gopher)
		gophermap_end(&g_gmap);
	if (g_feed)
		feed_end(&g_feedc);
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
	if (g_load.tree.frameset && !g_gopher && !g_feed)
		frames_page();
	make_forms();
	pimg_begin(&g_doc, &g_base, g_url);
	if (html_stylesheets) {
		pcss_begin(&g_doc, &g_base, g_url);
		g_css_busy = 1;
	}
	relayout(0);
	g_top = 0;
	snprintf(g_frag, sizeof g_frag, "%s", frag);
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
	g_feed = !g_gopher && feed_type(m->type);
	g_doc.img_w = scr_pixels() ? scr_pane_w() : 0;	/* (srcset) */
	html_load_begin(&g_load, &g_doc, m->charset,
		!g_gopher && !g_feed && !is_html(m->type));
	if (g_gopher)
		gophermap_begin(&g_gmap, feed_html, NULL);
	if (g_feed)
		feed_begin(&g_feedc, feed_html, NULL);
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
	refresh_plan(g_load.tree.meta_refresh);
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

/* about:update: the newest release, and (about:update?install, followed
 * from that page only) putting it in place of this one */
static void update_page(const char *url)
{
	static struct update_info u;
	static char html[UPDATE_NOTES * 2 + 2400];
	char err[400], why[600], e[UPDATE_NOTES * 2];
	int r, install = strcmp(url, "about:update?install") == 0
		&& strncmp(g_url, "about:update", 12) == 0;
	size_t n;

	message("Asking what the newest Manx is", NULL);
	draw(0);
	r = update_check(NULL, &u, err, sizeof err, on_status, NULL);
	snprintf(html, sizeof html, "<title>Updating Manx</title>"
		"<h1>Updating Manx</h1><p>This is Manx %s, for %s.</p>",
		update_version(), update_platform());
	n = strlen(html);
	if (r < 0) {
		html_escape(err, e, sizeof e);
		snprintf(html + n, sizeof html - n, "<p>What the newest is "
			"can't be told: <b>%s</b></p><p>r tries again.</p>", e);
	} else if (r == 0) {
		snprintf(html + n, sizeof html - n, "<p>That is the newest: the "
			"latest release is Manx %s.</p>", u.version);
	} else {
		const char *nl, *t;

		snprintf(html + n, sizeof html - n, "<p><b>Manx %s</b>%s%s%s is "
			"out.</p>", u.version, u.date[0] ? " (" : "", u.date,
			u.date[0] ? ")" : "");
		/* what's new, a line an item */
		for (t = u.notes; (nl = strchr(t, '\n')) != NULL; t = nl + 1) {
			char one[400];

			snprintf(one, sizeof one, "%.*s", (int)(nl - t), t);
			html_escape(one, e, sizeof e);
			n = strlen(html);
			snprintf(html + n, sizeof html - n, "%s<li>%s",
				t == u.notes ? "<ul>" : "", e);
		}
		n = strlen(html);
		if (u.notes[0])
			snprintf(html + n, sizeof html - n, "</ul>");
		n = strlen(html);
		if (install) {
			int ok = update_install(&u, why, sizeof why, on_status,
				NULL) == 0;

			html_escape(why, e, sizeof e);
			snprintf(html + n, sizeof html - n, "<p><b>%s</b></p>%s", e,
				ok ? "" : "<p><a href=\"about:update?install\">"
				"Try again</a></p>");
		} else if (update_can_install(why, sizeof why)) {
			html_escape(update_self(), e, sizeof e);
			snprintf(html + n, sizeof html - n, "<p><a href=\""
				"about:update?install\">Install it</a>, in place of %s "
				"(and of the other Manx programs beside it).</p>", e);
		} else {
			html_escape(why, e, sizeof e);
			snprintf(html + n, sizeof html - n, "<p>%s</p>", e);
		}
	}
	n = strlen(html);
	snprintf(html + n, sizeof html - n, "<p>Each release's list of files is "
		"signed, and Manx installs only what that list names. It asks "
		"once a day whether there is a newer one; update_check = off in "
		"~/.manx/config stops that.</p>");
	load_string(html, install ? "about:update" : url);
	g_msg[0] = '\0';
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
	h_refresh[0] = '\0';
	if (!g_refreshing)
		g_refresh_hops = 0;	/* (the user went somewhere) */
	if ((h = strchr(url, '#')) != NULL)
		snprintf(frag, sizeof frag, "%.250s", h + 1);
	if (strcmp(url, "about:help") == 0) {
		load_string(help_html, url);
		return 0;
	}
	if (strcmp(url, "about:start") == 0 || strcmp(url, "about:") == 0
		|| strcmp(url, "about:blank") == 0) {
		/* (the heading names the processor it runs on) */
		static char start[sizeof start_html + 200];

		snprintf(start, sizeof start, start_html, os_cpu());
		if (update_known()[0])
			snprintf(start + strlen(start), sizeof start - strlen(start),
				"<p><b>Manx %s is out:</b> <a href=\"about:update\">"
				"about:update</a></p>", update_known());
		load_string(start, url);
		return 0;
	}
	if (strncmp(url, "about:update", 12) == 0) {
		update_page(url);
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

		snprintf(why, sizeof why, "It is %s, which Manx can't show yet.",
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
		: strncmp(g_url, "file:", 5) == 0 ? "a local file"
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
	if (!g_aborted && rc == 0) {
		refresh_plan(h_refresh[0] ? h_refresh : g_load.tree.meta_refresh);
		feed_hint(res.status);
	}
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
		xfree(g_hist[i].frames);
	}
	g_nhist = g_hpos + 1;
	if (g_nhist == HIST_MAX) {
		xfree(g_hist[0].url);
		xfree(g_hist[0].post_body);
		xfree(g_hist[0].post_key);
		xfree(g_hist[0].frames);
		memmove(g_hist, g_hist + 1, (HIST_MAX - 1) * sizeof g_hist[0]);
		g_nhist--;
	}
	memset(&g_hist[g_nhist], 0, sizeof g_hist[0]);
	g_hist[g_nhist].url = xstrdup(url);
	g_hist[g_nhist].frames = g_frames ? xstrdup(g_frames) : NULL;
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

/* a link aimed at one of the page's frames: the page again, with url in
 * that frame */
static void visit_frame(const char *name, const char *url)
{
	char *chosen = frames_choose(g_frames, name, url);
	static char page[URL_MAX];

	if (chosen == NULL)
		return;
	snprintf(page, sizeof page, "%.*s", (int)strcspn(g_url, "#"), g_url);
	hist_save_pos();
	g_fchosen = chosen;
	load(page, NULL, L_NORMAL, NULL);
	g_fchosen = NULL;
	xfree(chosen);
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
		g_fchosen = h->frames;
		load(h->url, NULL, mode, NULL);
		g_fchosen = NULL;
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

static void forward(void)
{
	struct hist *h;

	if (g_hpos + 1 >= g_nhist) {
		message("No page to go forward to.", NULL);
		return;
	}
	hist_save_pos();
	g_hpos++;
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
		message("Manx can't follow %s links.", target);
		return;
	}
	visit(target);
}

/* --- refresh ----------------------------------------------------------- */

/*
 * The page asks to go on to another: a Refresh header or <meta
 * http-equiv=refresh> ("2; url=/next"). Done after the seconds it gives,
 * from the main loop, unless z or Esc says to stay (Anubis's check for
 * browsers without JavaScript is one: wait, then go on). A refresh of the
 * page itself isn't done: news pages ask for one every few minutes, and
 * on a 68030 each costs seconds and the reader's place.
 */
static void refresh_plan(const char *content)
{
	static struct url u;
	static char ref[URL_MAX];
	long secs;

	g_refresh_url[0] = '\0';
	if (!content[0] || http_refresh(content, &secs, ref, sizeof ref) < 0
		|| !ref[0] || url_resolve(&g_base, ref, &u) != URL_OK
		|| url_format(&u, g_refresh_url, sizeof g_refresh_url, 1) != URL_OK) {
		g_refresh_url[0] = '\0';
		return;
	}
	if ((strcmp(u.scheme, "http") && strcmp(u.scheme, "https")
			&& strcmp(u.scheme, "gopher")
			&& (strcmp(u.scheme, "file") || strncmp(g_url, "file:", 5)))
		|| same_doc(g_refresh_url, g_url)) {
		g_refresh_url[0] = '\0';
		return;
	}
	if (g_refresh_hops >= REFRESH_HOPS_MAX) {
		/* round and round (a check that never passes, say) */
		snprintf(g_msg, sizeof g_msg, "Not going on again (%d times in a "
			"row): %s", g_refresh_hops, g_refresh_url);
		g_refresh_url[0] = '\0';
		return;
	}
	g_refresh_at = os_msec() + (unsigned long)secs * 1000UL;
	if (secs > 0 && !g_msg[0])
		snprintf(g_msg, sizeof g_msg, "Going on in %ld s (z: stay here): %s",
			secs, g_refresh_url);
}

/* ms until the page's refresh is due (0: now), or -1: there's none */
static int refresh_left(void)
{
	long left;

	if (!g_refresh_url[0])
		return -1;
	left = (long)(g_refresh_at - os_msec());
	return left <= 0 ? 0 : left > 3600000L ? 3600000 : (int)left;
}

/* go on: the new page takes the place of the one that asked, as a
 * redirect would (Back doesn't land on it, to be sent on again) */
static void do_refresh(void)
{
	static char url[URL_MAX];
	struct hist *h;

	snprintf(url, sizeof url, "%s", g_refresh_url);
	g_refresh_url[0] = '\0';
	g_refresh_hops++;
	g_refreshing = 1;
	load(url, NULL, L_NORMAL, NULL);
	g_refreshing = 0;
	if (g_hpos < 0) {
		hist_push(g_url);
		return;
	}
	h = &g_hist[g_hpos];
	xfree(h->url);
	xfree(h->post_body);
	xfree(h->post_key);
	xfree(h->frames);
	memset(h, 0, sizeof *h);
	h->url = xstrdup(g_url);
	h->frames = g_frames ? xstrdup(g_frames) : NULL;
	h->sel = -1;
}

/* --- feeds ------------------------------------------------------------- */

static int ends_with(const char *s, const char *tail)
{
	size_t a = strlen(s), b = strlen(tail);

	return a >= b && strcmp(s + a - b, tail) == 0;
}

/* a space-separated list (rel="alternate") has word */
static int has_word(const char *s, const char *w)
{
	size_t n = strlen(w);

	while (*s) {
		while (*s == ' ')
			s++;
		if (strncmp(s, w, n) == 0 && (s[n] == ' ' || s[n] == '\0'))
			return 1;
		while (*s && *s != ' ')
			s++;
	}
	return 0;
}

/* where sites whose pages are walled off (Cloudflare) or need JavaScript
 * keep a feed that isn't: into url, 0; or -1 */
static int known_feed(const struct url *u, char *url, size_t n)
{
	const char *h = strncmp(u->host, "www.", 4) == 0 ? u->host + 4 : u->host;
	const char *p = u->path;
	unsigned long id;
	size_t k;

	/* Stack Exchange: a question, with its answers */
	if ((strcmp(h, "stackoverflow.com") == 0 || strcmp(h, "superuser.com") == 0
			|| strcmp(h, "serverfault.com") == 0
			|| strcmp(h, "askubuntu.com") == 0
			|| strcmp(h, "mathoverflow.net") == 0
			|| ends_with(h, ".stackexchange.com"))
		&& (sscanf(p, "/questions/%lu", &id) == 1
			|| sscanf(p, "/q/%lu", &id) == 1)) {
		snprintf(url, n, "https://%s/feeds/question/%lu", u->host, id);
		return 0;
	}
	/* Reddit: any listing or thread, with .rss on the end (what its login
	 * wall was sent to, dest=, if that's where this is) */
	if (strcmp(h, "reddit.com") == 0 || ends_with(h, ".reddit.com")) {
		static char dest[URL_MAX];
		static struct url du;
		const char *d = strstr(u->query, "dest=");

		if (strncmp(p, "/login", 6) == 0 && d) {
			size_t i, o = 0;
			unsigned c;

			for (i = 5; d[i] && d[i] != '&' && o + 1 < sizeof dest; i++)
				if (d[i] == '%' && sscanf(d + i + 1, "%2x", &c) == 1) {
					dest[o++] = (char)c;
					i += 2;
				} else
					dest[o++] = d[i];
			dest[o] = '\0';
			if (url_parse(dest, &du) == URL_OK)
				p = du.path;
		}
		k = strlen(p);
		if (k > 1900)
			return -1;
		snprintf(url, n, "https://www.reddit.com%.1900s%s.rss", k ? p : "/",
			k && p[k - 1] == '/' ? "" : "/");
		return 0;
	}
	/* Invision Community (AtariAge): a forum's newest topics, from
	 * /forum/12-name/ */
	if (strncmp(p, "/forum/", 7) == 0 && p[7] >= '0' && p[7] <= '9') {
		k = strcspn(p + 7, "/");
		if (strchr(p + 7, '-') && strchr(p + 7, '-') < p + 7 + k) {
			snprintf(url, n, "%s://%s/forum/%.*s.xml/", u->scheme, u->host,
				(int)k, p + 7);
			return 0;
		}
	}
	/* Medium: a writer's or a publication's */
	if (strcmp(h, "medium.com") == 0 && p[0] == '/' && p[1]) {
		k = strcspn(p + 1, "/");
		snprintf(url, n, "https://medium.com/feed/%.*s", (int)k, p + 1);
		return 0;
	}
	return -1;
}

/* This page's feed into url: the one it names (<link rel=alternate
 * type=application/rss+xml...>), else known_feed's. 0, or -1: none. */
static int page_feed(char *url, size_t n)
{
	static struct url u;
	unsigned long id;

	if (g_feed)
		return -1;
	for (id = 2; id < g_doc.nnodes; id++) {
		const char *rel, *type, *href;

		if (g_doc.nodes[id].type != NODE_ELEM
			|| g_doc.nodes[id].tag != TAG_LINK)
			continue;
		rel = doc_attr(&g_doc, (nodeid)id, ATTR_REL);
		type = doc_attr(&g_doc, (nodeid)id, ATTR_TYPE);
		href = doc_attr(&g_doc, (nodeid)id, ATTR_HREF);
		if (rel && type && href && *href && has_word(rel, "alternate")
			&& feed_type(type) && url_resolve(&g_base, href, &u) == URL_OK
			&& url_format(&u, url, n, 0) == URL_OK)
			return 0;
	}
	if (url_parse(g_url, &u) != URL_OK)
		return -1;
	return known_feed(&u, url, n);
}

/* a page that came walled off or all but empty, with a feed to offer:
 * say so */
static void feed_hint(int status)
{
	static char url[URL_MAX];
	size_t k = strlen(g_msg);

	if (g_feed || g_refresh_url[0]
		|| (status < 400 && g_have_page && g_page.nlines > 12)
		|| page_feed(url, sizeof url) < 0)
		return;
	snprintf(g_msg + k, sizeof g_msg - k, "%sF: its feed", k ? "  " : "");
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
	int rows = scr_rows - 4, w = (int)strlen(title), i, top = 0, k;

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
		int r0 = 1 + (scr_rows - 2 - rows - 2) / 2, c0 = (scr_cols - w - 4) / 2;

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
		case 27: case 7: case 3: case K_LEFT: case 'q': case K_CLOSE:
			return -1;
		}
	}
}

static void choose_option(struct field *f)
{
	static nodeid opts[FORMS_MAX_OPTIONS];
	static char *items[FORMS_MAX_OPTIONS];
	int n = forms_options(&g_forms, f, opts, FORMS_MAX_OPTIONS), i, cur = 0, pick;

	if (n == 0) {
		message("The list is empty.", NULL);
		return;
	}
	for (i = 0; i < n; i++) {
		struct field tmp = *f;

		tmp.selected = opts[i];
		if ((items[i] = xstrdup(forms_text(&g_forms, &tmp))) == NULL) {
			n = i;			/* (no memory: the ones so far) */
			break;
		}
		if (opts[i] == f->selected)
			cur = i;
	}
	if (n == 0)
		return;
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
	if (!prompt_mask(label, buf, sizeof buf,
		f->type == FT_PASSWORD ? PM_MASK : 0))
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
		message("Manx can't upload files.", NULL);
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
	if (g_frames && g_doc.nodes[k->node].tag == TAG_A
		&& (h = doc_attr(&g_doc, k->node, ATTR_TARGET)) != NULL
		&& frames_has(g_frames, h)) {
		visit_frame(h, buf);
		return;
	}
	go_url(buf);
}

/* the columns text of n bytes takes on the screen */
static int text_cols(const char *t, size_t n)
{
	const char *e = t + n;
	int c = 0;

	if (scr_cs != TCS_UTF8)
		return (int)n;
	while (t < e)
		c += ucs_width(utf8_get(&t));
	return c;
}

/* the link (0-based) under screen row/col, or -1 */
static long link_at(int row, int col)
{
	const struct page *p = &g_page;
	const struct lline *l;
	long ln = g_top + row - 1;
	unsigned long off, end, s;
	int c;

	if (!g_have_page || row < 1 || row > view_rows() || ln >= (long)p->nlines)
		return -1;
	l = &p->lines[ln];
	off = l->off;
	end = l->off + l->len;
	s = l->span;
	c = l->indent;
	while (off < end) {
		unsigned long next = end;
		const struct lspan *sp;
		int w;

		s = layout_span_at(p, s, off);
		sp = &p->spans[s];
		if (s + 1 < p->nspans && p->spans[s + 1].off < end)
			next = p->spans[s + 1].off;
		w = text_cols(p->text + off, (size_t)(next - off));
		if (col >= c && col < c + w)
			return sp->link ? (long)sp->link - 1 : -1;
		c += w;
		off = next;
	}
	return -1;
}

/* the link (0-based) of line ln of page p (the page, or a layer) at x
 * across it, yr below the line's top (a line above, its tall content
 * reaching down: only what does), or -1 */
static long link_on_line(const struct page *p, long ln, int x, long yr)
{
	const struct lline *l = &p->lines[ln];
	unsigned long off = l->off, end = l->off + l->len, s = l->span;
	int c = l->indent;

	while (off < end) {
		unsigned long next = end;
		const struct lspan *sp;
		int w;

		s = layout_span_at(p, s, off);
		sp = &p->spans[s];
		if (s + 1 < p->nspans && p->spans[s + 1].off < end)
			next = p->spans[s + 1].off;
		w = run_w(p, sp, p->text + off, (int)(next - off));
		if (x >= c && x < c + w) {
			/* (a descender's worth below its baseline) */
			if (yr >= p->heights[ln].height
				&& yr > (sp->asc ? sp->asc : p->heights[ln].ascent) + 4)
				return -1;
			return sp->link ? (long)sp->link - 1 : -1;
		}
		c += w;
		off = next;
	}
	return -1;
}

/* the link (0-based) under x, y of the pane in page p's layers, the top
 * one first (p's line 0's top at y0, x0 across; h: the pane's height),
 * or -1 */
static long layer_link_at(struct page *p, int x0, long y0, int x, int y, int h)
{
	long *ord = layer_order(p), k = -1, i;

	if (ord == NULL)
		return -1;
	for (i = (long)p->nlayers - 1; i >= 0 && k < 0; i--) {
		struct llayer *ly = &p->layers[ord[i]];
		struct page *lp = &ly->pg;
		long ly0 = layer_y(p, ly, y0, h), top;
		int lx = (ly->fixed ? 0 : x0) + ly->x;
		unsigned long ln;

		if ((k = layer_link_at(lp, lx, ly0, x, y, h)) >= 0 || lp->heights == NULL)
			continue;
		for (ln = 0; ln < lp->nlines && k < 0; ln++) {
			top = ly0 + line_top(lp, ln);
			if (y >= top && y < top + lp->heights[ln].height + lp->heights[ln].over)
				k = link_on_line(lp, (long)ln, x - lx, y - top);
		}
	}
	xfree(ord);
	return k;
}

/* the link (0-based) under x, y of the pane, in fonts; or -1 */
static long link_at_px(int x, int y)
{
	const struct page *p = &g_page;
	const long *tops;
	long ln, k;
	int ly = 0;

	if (!g_have_page || !p->heights || x < 0 || y < 0)
		return -1;
	/* (positioned elements: over the rest) */
	if (p->nlayers && (tops = layout_tops(&g_page)) != NULL
		&& (k = layer_link_at(&g_page, 0, -tops[g_top], x, y, scr_pane_h())) >= 0)
		return k;
	for (ln = g_top; ln < (long)p->nlines; ln++) {
		if (y < ly + p->heights[ln].height)
			break;
		ly += p->heights[ln].height;
	}
	if (ln >= (long)p->nlines)
		return -1;
	if ((k = link_on_line(p, ln, x, y - ly)) >= 0)
		return k;
	/* a tall cell's line (a picture) reaching down from a band above */
	if (p->max_over > 0 && (tops = layout_tops(&g_page)) != NULL) {
		long ya = tops[g_top] + y, l2;

		for (l2 = ln - 1; l2 >= 0 && ya - tops[l2 + 1] < p->max_over; l2--)
			if (ya < tops[l2 + 1] + p->heights[l2].over
				&& (k = link_on_line(p, l2, x, ya - tops[l2])) >= 0)
				return k;
	}
	return -1;
}

/* a click: on a link, follow it */
static void click(int row, int col)
{
	long k = scr_metrics() ? link_at_px(scr_mouse_x, scr_mouse_y)
		: link_at(row, col);

	if (k < 0)
		return;
	g_sel = k;
	g_sel_shown = 1;	/* (a field clicked into: marked as it's used) */
	draw(0);
	follow();
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

/* What's typed is in the terminal's charset: on a UTF-8 terminal a
 * character is several bytes, moved over and deleted whole, and a column
 * or two wide (none for a combining mark) */
static size_t ch_prev(const char *b, size_t pos)
{
	if (pos == 0)
		return 0;
	pos--;
	if (scr_cs == TCS_UTF8)
		while (pos > 0 && ((unsigned char)b[pos] & 0xC0) == 0x80)
			pos--;
	return pos;
}

static size_t ch_next(const char *b, size_t pos, size_t len)
{
	if (pos >= len)
		return len;
	pos++;
	if (scr_cs == TCS_UTF8)
		while (pos < len && ((unsigned char)b[pos] & 0xC0) == 0x80)
			pos++;
	return pos;
}

static int ch_cols(const char *b, size_t n)
{
	const char *s = b, *e = b + n;
	int w = 0;

	if (scr_cs != TCS_UTF8)
		return (int)n;
	while (s < e) {
		if ((unsigned char)*s < 0x80) {
			s++;
			w++;
		} else {
			int k = ucs_width(utf8_get(&s));

			w += k > 0 ? k : 0;
		}
	}
	return w;
}

/* A line editor on the status line (or, with PM_URL, in the window's URL
 * field): 1 when Enter was pressed. */
static int prompt_mask(const char *label, char *buf, size_t n, int flags)
{
	size_t len = strlen(buf), pos = len;
	size_t sa = 0, sb = 0;		/* xmanx: a selection, sa < sb */
	int r = scr_rows - 1, lw = (int)strlen(label), skip = 0;
	static char stars[1024];

	for (;;) {
		int k, avail = scr_cols - lw - 1, start = 0;

		/* show the part around the cursor */
		if (ch_cols(buf, pos) > avail) {
			size_t st = 0;

			while (st < pos && ch_cols(buf + st, pos - st) > avail)
				st = ch_next(buf, st, len);
			start = (int)st;
		}
		draw_status();
		if ((flags & PM_URL) && scr_url_edit(buf, (int)pos)) {
			scr_url_sel((int)sa, (int)sb);
			scr_cursor(-1, -1);
		}
		else {
			scr_fill(r, 0, scr_cols, ' ', 0);
			scr_put(r, 0, label, lw, CA_BOLD);
			if (flags & PM_MASK) {
				memset(stars, '*', len < sizeof stars ? len
					: sizeof stars - 1);
				scr_put(r, lw, stars + start, (int)len - start, 0);
			} else
				scr_put(r, lw, buf + start, (int)len - start, 0);
			scr_cursor(r, lw + ch_cols(buf + start, pos - (size_t)start));
		}
		scr_flush(0);
		k = scr_getkey(-1);
		/* a selection: typing, pasting or deleting replaces it; any
		 * other key lets it go */
		if (sa < sb && k >= 0) {
			int del = k == 8 || k == 127 || k == K_DEL || k == 4;

			if (del || k == K_PASTE || (k >= 32 && k < 256)) {
				memmove(buf + sa, buf + sb, len - sb + 1);
				len -= sb - sa;
				pos = sa;
			}
			sa = sb = 0;
			if (del)
				continue;
		}
		switch (k) {
		case K_FIELD:			/* xmanx: a click in the field */
			if (scr_field_clicks >= 2) {
				sa = 0;
				sb = pos = len;
			} else {
				size_t p = 0;

				while (p < len && p < (size_t)scr_field_pos)
					p = ch_next(buf, p, len);
				pos = p;
			}
			break;
		case K_PASTE: {			/* xmanx: a middle click */
			const unsigned char *s = (const unsigned char *)scr_paste;

			for (; *s && len + 1 < n; s++) {
				if (*s < 32 || *s == 127)
					continue;	/* (a newline, a tab) */
				/* (a UTF-8 character that won't fit whole stops it) */
				if (*s >= 0xC0 && len + 1 + (*s >= 0xF0 ? 3
					: *s >= 0xE0 ? 2 : 1) >= n)
					break;
				memmove(buf + pos + 1, buf + pos, len - pos + 1);
				buf[pos++] = (char)*s;
				len++;
			}
			break;
		}
		case '\r':
			scr_cursor(-1, -1);
			if (flags & PM_URL)
				scr_url_edit(NULL, 0);
			return 1;
		case 27: case 7: case 3: case K_CLOSE:
			scr_cursor(-1, -1);
			if (flags & PM_URL)
				scr_url_edit(NULL, 0);
			return 0;
		case 8: case 127:
			if (pos) {
				size_t p0 = ch_prev(buf, pos);

				memmove(buf + p0, buf + pos, len - pos + 1);
				len -= pos - p0;
				pos = p0;
			}
			break;
		case K_DEL: case 4:
			if (pos < len) {
				size_t p1 = ch_next(buf, pos, len);

				memmove(buf + pos, buf + p1, len - p1 + 1);
				len -= p1 - pos;
			}
			break;
		case K_LEFT: case 2:
			pos = ch_prev(buf, pos);
			break;
		case K_RIGHT: case 6:
			pos = ch_next(buf, pos, len);
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
			if (k < 32 || k >= 256 || k == 127)
				break;
			/* (a UTF-8 character that won't fit whole isn't begun) */
			if (scr_cs == TCS_UTF8 && (k & 0xC0) == 0x80 && skip) {
				skip--;
				break;
			}
			if (scr_cs == TCS_UTF8 && k >= 0xC0) {
				int more = k >= 0xF0 ? 3 : k >= 0xE0 ? 2 : 1;

				if (len + 1 + (size_t)more >= n) {
					skip = more;
					break;
				}
			}
			if (len + 1 < n) {
				memmove(buf + pos + 1, buf + pos, len - pos + 1);
				buf[pos++] = (char)k;
				len++;
			}
		}
	}
}

/* a path on this machine as a file: URL in out: 1, or 0 when in isn't
 * one (~ is $HOME; a relative path must exist, being otherwise a host) */
static int local_url(const char *in, char *out, size_t n)
{
	char cwd[URL_MAX / 2];
	const char *home;
	long sz, mt;

	if ((in[0] == '~' && (in[1] == '/' || in[1] == '\0'))
		&& (home = getenv("HOME")) != NULL) {
		snprintf(out, n, "file://%s%s", strcmp(home, "/") ? home : "", in + 1);
		return 1;
	}
	if (in[0] == '/') {
		snprintf(out, n, "file://%s", in);
		return 1;
	}
	if (strncmp(in, "./", 2) != 0 && os_file_info(in, &sz, &mt) != 0)
		return 0;
	if (getcwd(cwd, sizeof cwd) == NULL)
		return 0;
	if (strncmp(in, "./", 2) == 0)
		in += 2;
	snprintf(out, n, "file://%s/%s", strcmp(cwd, "/") ? cwd : "", in);
	return 1;
}

/* what was typed at g: a URL, a file, or words to search for */
static void typed(const char *in, char *out, size_t n)
{
	const char *p;

	while (*in == ' ')
		in++;
	if (strstr(in, "://") || strncmp(in, "about:", 6) == 0
		|| strncmp(in, "file:", 5) == 0)
		snprintf(out, n, "%s", in);
	else if (local_url(in, out, n))
		;
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

/* the bookmarks are a page: $MANX_HOME/bookmarks.html, one link a line */
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
		message("No place for bookmarks ($HOME or $MANX_HOME).", NULL);
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
				"<p>(Manx adds a line with a; the file is " "bookmarks.html in "
				"~/.manx: edit it to change or remove them.)\n<ul>\n", f);
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

/* lay out again, keeping the same text at the top (lines may have moved) */
static void relayout_same_text(void)
{
	unsigned long off = g_have_page && g_top < (long)g_page.nlines ?
		g_page.lines[g_top].off : 0;
	long i;

	relayout(0);
	for (i = 0; i + 1 < (long)g_page.nlines
		&& g_page.lines[i + 1].off <= off; i++)
		;
	g_top = i;
	scroll_to(g_top);
}

/* a key that only moves the view (scrolling, the selected link): done,
 * 1; else 0 */
static int view_key(int k)
{
	if (k >= 0)
		g_frag[0] = '\0';	/* (the user has moved: stay put) */
	switch (k) {
	case K_WHEELUP:
		scroll_to(g_top - 3);
		return 1;
	case K_WHEELDN:
		scroll_to(g_top + 3);
		return 1;
	case K_SCROLL:
		scroll_to(scr_scroll_target);
		return 1;
	case K_DOWN:
		move_link(1);
		return 1;
	case K_UP:
		move_link(-1);
		return 1;
	case '\t':
		jump_link(1);
		return 1;
	case K_BTAB:
		jump_link(-1);
		return 1;
	case ' ': case K_PGDN: case 6:
		scroll_to(g_top + view_rows() - 1);
		return 1;
	case 'b': case K_PGUP: case 2:
		scroll_to(g_top - (view_rows() - 1));
		return 1;
	case 'j': case 5:
		scroll_to(g_top + 1);
		return 1;
	case 'k': case 25:
		scroll_to(g_top - 1);
		return 1;
	case K_HOME: case '<':
		scroll_to(0);
		return 1;
	case K_END: case '>':
		scroll_to(max_top());
		return 1;
	case 'm':
		if (g_page.content_line >= 0)
			scroll_to(g_page.content_line);
		else if (g_page.main_line >= 0)
			scroll_to(g_page.main_line);
		else if (layout_anchor(&g_page, "content") >= 0)
			scroll_to(layout_anchor(&g_page, "content"));
		else
			message("This page doesn't mark its main content.", NULL);
		return 1;
	case 12:
		draw(1);
		return 1;
	}
	return 0;
}

static int next_key(int timeout)
{
	int k;

	if (g_nkeyq == 0)
		return scr_getkey(timeout);
	k = g_keyq[0];
	memmove(g_keyq, g_keyq + 1, (size_t)--g_nkeyq * sizeof g_keyq[0]);
	return k;
}

/*
 * While an image is fetched or decoded: keys that only move the view are
 * done there and then; any other stops the image (it goes on later) and
 * waits for the main loop. What has come of the image is shown now and
 * then.
 */
static int img_poll(void *ctx, int shown)
{
	unsigned long now;
	int k;

	(void)ctx;
	while ((k = scr_getkey(0)) >= 0) {
		if (view_key(k)) {
			draw(0);
			g_img_framed = 1;
			continue;
		}
		if (g_nkeyq < (int)(sizeof g_keyq / sizeof g_keyq[0]))
			g_keyq[g_nkeyq++] = k;
		return -1;
	}
	now = os_msec();
	if (shown) {
		/* the image's rows as they come: a frame once it is in one,
		 * then only the new rows go out (a frame is the whole page
		 * again: a slow machine's decoding would be mostly that) */
		if (!g_img_framed || now - g_img_drawn > 3000) {
			g_img_drawn = g_img_flushed = now;
			g_img_framed = 1;
			draw(0);
		} else if (now - g_img_flushed > 400) {
			g_img_flushed = now;
			draw_status();
			scr_flush(0);
		}
	} else if (now - g_img_flushed > 1000) {
		/* a fetch: how far it is (the page itself stays as it is) */
		g_img_flushed = now;
		draw_status();
		scr_flush(0);
	}
	return 0;
}

/* a piece of the page's image work, while no key waits */
static void img_work(void)
{
	unsigned long now;
	int r;

	/* (one piece of work: a fetch, or one image's decoding, which the
	 * frame doesn't hold until it is drawn with it) */
	g_img_framed = 0;
	if (g_css_busy) {
		/* the style sheets first: they say what is laid out at all,
		 * images included */
		r = pcss_step(img_poll, NULL);
		if (r == PCSS_READ)
			g_img_relayout = 1;
		if (r == PCSS_IDLE)
			g_css_busy = 0;
		now = os_msec();
		if (g_img_relayout && (r == PCSS_IDLE || now - g_img_relaid > 2000)) {
			g_img_relayout = 0;
			g_img_relaid = now;
			relayout_same_text();
			if (g_frag[0]) {
				long ln = layout_anchor(&g_page, g_frag);

				if (ln >= 0)
					scroll_to(ln);
			}
		}
		return;
	}
	/* (the line cut off at the bottom is shown too) */
	r = pimg_step(&g_page, g_top, view_rows() + 1, img_poll, NULL);
	now = os_msec();
	if (r == PIMG_SIZED)
		g_img_relayout = 1;
	if (r == PIMG_IDLE)
		g_img_busy = 0;
	/* new sizes: lay out again, but not more often than every 2 s (this
	 * makes g_img_busy again: what the new layout shows may need work) */
	if (g_img_relayout && (r == PIMG_IDLE || now - g_img_relaid > 2000)) {
		g_img_relayout = 0;
		g_img_relaid = now;
		relayout_same_text();
		if (g_frag[0]) {
			/* opened at a #fragment: the images above it have
			 * moved it */
			long ln = layout_anchor(&g_page, g_frag);

			if (ln >= 0)
				scroll_to(ln);
		}
	}
	if (!g_img_busy)
		g_frag[0] = '\0';
}

static int g_cli_over;		/* a progress line to write over */

static void cli_status(void *ctx, const char *msg)
{
	int over = g_cli_over, progress = strstr(msg, " KB") != NULL;

	(void)ctx;
	fflush(stdout);
	if (progress && !isatty(2))
		return;
	if (progress)
		fprintf(stderr, "\r%-60s", msg);
	else
		fprintf(stderr, "%s%s\n", over ? "\n" : "", msg);
	g_cli_over = progress;
}

/* manx --update: check, and install if there's newer (as about:update) */
static int update_cli(void)
{
	static struct update_info u;
	char err[600];
	int r;

	printf("Manx %s, for %s\n", update_version(), update_platform());
	r = update_check(NULL, &u, err, sizeof err, cli_status, NULL);
	if (r < 0) {
		fprintf(stderr, "manx: %s\n", err);
		return 1;
	}
	if (r == 0) {
		printf("That is the newest: the latest release is %s.\n",
			u.version);
		return 0;
	}
	printf("Manx %s%s%s%s is out.\n%s", u.version, u.date[0] ? " (" : "",
		u.date, u.date[0] ? ")" : "", u.notes);
	r = update_install(&u, err, sizeof err, cli_status, NULL);
	if (g_cli_over)
		fputc('\n', stderr);
	printf("%s\n", err);
	return r < 0;
}

int main(int argc, char **argv)
{
	char path[600], in[URL_MAX], go[URL_MAX];
	const char *start, *geometry = NULL, *err = NULL;
	int quit = 0, a = 1;

	signal(SIGPIPE, SIG_IGN);
	update_init(argv[0]);
	if (argc == 2 && (strcmp(argv[1], "--version") == 0
			|| strcmp(argv[1], "-version") == 0)) {
		printf("Manx %s (%s%s%s)\n", update_version(), update_platform(),
			update_packaged()[0] ? ", from " : "", update_packaged());
		return 0;
	}
	/* settings: ~/.manx/config, MANX_<KEY> overriding */
	if (config_load(os_datapath(path, sizeof path, "config"), &err) < 0)
		fprintf(stderr, "manx: %s\n", err);
	/* -geometry COLSxROWS[+X+Y], as X programs take it (xmanx) */
	while (a + 1 < argc && (strcmp(argv[a], "-geometry") == 0
			|| strcmp(argv[a], "-g") == 0)) {
		geometry = argv[a + 1];
		a += 2;
	}
	start = a < argc ? argv[a] : config_str("start", "about:start");
	g_search = config_str("search", SEARCH_URL);
	scr_color = config_bool("color", 1);
	scr_font = config_str("font", NULL);
	scr_geometry = geometry ? geometry : config_str("geometry", NULL);
	scr_proportional = config_bool("proportional", 1);
	g_images = config_bool("images", 1);
	html_stylesheets = config_bool("stylesheets", 1);
	{
		/* link_color: a colour name, or 0-7 (unset: the screen's own,
		 * cyan on a terminal, blue in a window) */
		static const char *const names[] = { "black", "red", "green",
			"yellow", "blue", "magenta", "cyan", "white" };
		const char *lc = config_str("link_color", NULL);
		int i;

		for (i = 0; lc && i < 8; i++)
			if (strcmp(lc, names[i]) == 0 || (lc[0] == '0' + i && !lc[1]))
				scr_link_color = i;
	}
	fetch_early_requests = config_bool("early_requests", 1);
	cookie_enabled = config_bool("cookies", 1);
	if (cookie_enabled)
		cookie_init(os_datapath(path, sizeof path, "cookies"));
	/* a site's own User-Agent, cookies (a browser's pass through a wall) */
	sites_load(os_datapath(path, sizeof path, "sites"));
	if (config_long("cache_kb", 2048) > 0)
		cache_init(os_datapath(path, sizeof path, "cache"),
			config_long("cache_kb", 2048) * 1024L);
	entropy_init(os_datapath(path, sizeof path, "seed"));
	dns_cache_init(os_datapath(path, sizeof path, "dns"));
	/* the first run builds the trust store: say so, it takes a minute */
	tls_init(config_str("cafile", NULL), tls_note);
	if (argc == 2 && strcmp(argv[1], "--update") == 0)
		return update_cli();
	/* (before the screen: the child has none of it) */
	update_bg_start();
	if (doc_init(&g_doc, 0) < 0 || scr_open(config_str("charset", NULL)) < 0) {
		fprintf(stderr, "manx: needs %s\n", scr_needs);
		return 1;
	}
	/* (boxes - backgrounds, borders, floats - only where they're drawn:
	 * a terminal's sheets keep none of that) */
	css_boxes = scr_metrics() != NULL;
	if (a < argc && !strstr(start, "://") && strncmp(start, "about:", 6)) {
		/* (a file here: file:// with its full path) */
		typed(start, go, sizeof go);
		start = go;
	}
	visit(start);
	while (!quit) {
		int k;

		if (refresh_left() == 0) {
			do_refresh();
			continue;
		}
		if (update_bg_poll()) {
			snprintf(g_msg, sizeof g_msg, "Manx %s is out: about:update "
				"has it.", update_known());
			g_msg_sticky = 1;
		}
		if (scr_check_size()) {
			relayout_same_text();
			draw(1);
		} else
			draw(0);
		if ((g_img_busy || g_css_busy) && g_nkeyq == 0) {
			if ((k = scr_getkey(0)) < 0) {
				/* nothing typed: on with the images */
				img_work();
				continue;
			}
		} else
			k = next_key(refresh_left());	/* (-1: no refresh, wait) */
		if (!g_msg_sticky)
			g_msg[0] = '\0';
		g_msg_sticky = 0;
		/* in a window, a page's selected link unmarked until keys
		 * move among the links: the first of them marks it */
		if (!g_sel_shown && g_sel >= 0 && (k == K_DOWN || k == K_UP || k == '\t'
			|| k == K_BTAB || k == K_RIGHT || k == '\r')) {
			g_sel_shown = 1;
			continue;
		}
		if (k == K_DOWN || k == K_UP || k == '\t' || k == K_BTAB)
			g_sel_shown = 1;
		if (view_key(k))	/* (any key: g_frag dropped there) */
			continue;
		switch (k) {
		case 'q': case 'Q': case K_CLOSE:
			quit = 1;
			break;
		case K_MOUSE:
			click(scr_mouse_row, scr_mouse_col);
			break;
		case K_RIGHT: case '\r':
			follow();
			break;
		case K_LEFT: case 'u': case 8: case 127:
			back();
			break;
		case 'f':
			forward();
			break;
		case 'g': case 'G':
			in[0] = '\0';
			if (k == 'G')
				snprintf(in, sizeof in, "%s", g_url);
			if (prompt_mask("URL or search: ", in, sizeof in, PM_URL)
				&& in[0]) {
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
		case '=':
			show_info();
			break;
		case 'a':
			add_bookmark();
			break;
		case 'F':
			if (g_feed)
				message("This is a feed.", NULL);
			else if (page_feed(go, sizeof go) < 0)
				message("No feed for this page.", NULL);
			else
				go_url(go);
			break;
		case 'v':
			show_bookmarks();
			break;
		case '?': case 'h': case K_F1:
			go_url("about:help");
			break;
		case 'Z':
			if (scr_zoom() < 0)
				message("Z zooms xmanx's window.", NULL);
			break;
		case 'z': case 27:
			if (g_refresh_url[0]) {
				g_refresh_url[0] = '\0';
				message("Staying on this page.", NULL);
			} else if (k == 'z' && (g_img_busy || g_css_busy)) {
				pcss_cancel();
				pimg_cancel();
				message("No more images for this page.", NULL);
			}
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
