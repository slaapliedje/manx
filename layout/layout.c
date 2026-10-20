/*
 * layout.c - block/inline flow onto a text grid, or onto a screen with
 * proportional fonts.
 *
 * One walk over the tree. Text goes straight into page.text in the
 * screen's character set; a line is a range of that text plus an
 * indent, so when a word doesn't fit, only the line boundary moves (the
 * word's bytes stay where they are). Spans record where the attributes
 * or the link (and, with metrics, the face) change.
 *
 * Widths are measured through struct lmetrics when there is one, and
 * otherwise in terminal columns: every width below starts as a column
 * count, which text_w() keeps or replaces by what the metrics say.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "tags.h"
#include "utf8.h"
#include "css.h"
#include "layout.h"

#define MAX_DEPTH	200
#define MAX_LISTS	16

struct saved {
	unsigned char attr, pre, tag, list;
	unsigned char display, margin, face, nomarker;
	unsigned char fg, align;
	short fsz;
	struct css_len lh;
	unsigned long para_mark, para_line;	/* <p>: where it began */
	unsigned short link;
	short indent;
	unsigned long link_mark;	/* text_len when the link began */
	unsigned char cell;
	unsigned long cell_mark;
	/* a CSS box (with metrics): its reserved lboxes (index + 1; 0: not
	 * a box), its first line, the width it narrowed, what's owed below */
	unsigned long box;
	unsigned char boxk;		/* 0: no box; 1: one drawn; 2: margins only */
	unsigned long box_line;
	int box_width;
	int box_in_b, box_m_b;		/* border and padding, margin (-1: the
					 * block's own blank lines) below */
	long box_minh;			/* height: at least (0: none) */
	int box_rin;			/* L->rin before it */
	int box_cut;			/* the float cut from the width then */
};

struct list {
	unsigned char ordered, type;	/* type: '1', 'a', 'A', 'i', 'I' */
	long counter;
};

struct lay {
	struct page *p;
	const struct doc *d;
	const struct forms *fs;
	const struct lmetrics *m;	/* NULL: terminal columns */
	int em;				/* an indent column, in units */
	int base_w;			/* the width it was laid out at */
	int rin;			/* the open boxes' right sides (padding,
					 * border, margin) beside the text */
	struct css_len lh;		/* the line height (with metrics; u
					 * CSS_U_NONE: the font's own) */
	int fsz;			/* the font's size, tenths of a unit (with
					 * metrics): CSS's em */
	int face;			/* LF_* */
	int width;
	int vw;				/* the window's width for @media: pixels,
					 * or a terminal's columns at 8 pixels
					 * (a grid cell's layout has the page's) */
	unsigned long max_lines;
	int stop;
	/* the line being built */
	int line_open;
	unsigned long line_off, line_span;
	int line_indent;
	int line_a, line_d;		/* its tallest ascent and descent */
	int col;			/* columns used, indent included */
	/* flow state */
	int indent;
	int pend_space;			/* spaces owed before the next word */
	int pend_lines;			/* blank lines owed before the next line */
	int blank_run;			/* blank lines just emitted */
	int in_word;
	unsigned long word_off, word_span;
	int word_col;
	int attr, pre;
	int fg;				/* the text's colour: page palette, 0: none */
	int align;			/* CSS_TA_*, 0: left */
	unsigned short link;
	unsigned long link_mark;
	/* inside a table cell: blocks break the line only between
	 * contents of the cell, so that a row stays one line */
	int cell;
	unsigned long cell_mark;	/* text_len when the cell began */
	int cell_break;			/* 1 + margin of a break owed */
	long heading_line;		/* the last h1-h3 */
	char marker[16];		/* a list item's marker, not yet shown */
	int marker_w;
	int nomarker;			/* list-style: none here */
	struct list lists[MAX_LISTS];
	int nlists;
	int oom;
	int small;			/* a grid cell's page: start its arrays small */
	unsigned short asc;		/* a grid cell's line in a band: its own
					 * baseline (struct lspan) */
	int at_width;			/* a cell at its column's width: lines centre */
	int aligned;			/* a cell had lines to centre or put right */
	int gdepth;			/* the grids this is in a cell of */
	int in_box;			/* CSS boxes open: their indents count */
	/* a float the text flows beside (with metrics): its lines go one
	 * at the start of each line of text, at their own height */
	struct fitem *fl;		/* NULL: none */
	int fl_right;			/* on the right */
	int fl_cut;			/* L->width is this much narrower for it */
	int fl_edge;			/* a left one: where text may start */
	int fl_cx;			/* its content's left (units) */
	unsigned long fl_ln;		/* its next line */
	long fl_next;			/* ... where that goes, below its top */
	long fl_y;			/* the next line of text's top, likewise */
	long fl_bottom;			/* its margin box's bottom, likewise */
	unsigned long fl_seen;		/* page lines counted into fl_y */
	long fl_over;			/* the open line's float line reaches so far
					 * below the line's top */
	int fl_inline;			/* the open line holds a float's line */
	int fl_line_x;			/* ... which is where the line starts */
	int fl_level;			/* the boxes open when it started */
	unsigned long fl_box;		/* its boxes: p->boxes[fl_box...] */
	long fl_top;			/* its content's top, below its own */
	int html_bg;			/* the root had a background */
	struct saved *top_pend[8];	/* boxes open with nothing above their
					 * content (no border, no padding) and
					 * nothing in them yet: a margin next
					 * moves their top down (it collapses
					 * through) */
	int ntop_pend;
	long gap;			/* (with metrics) the margins' height just
					 * above: blank lines and margin spacers
					 * since the last line of anything else */
	struct saved st[MAX_DEPTH];	/* (last: written before read, never
					 * cleared) */
};

/* --- storage --------------------------------------------------------- */

static size_t used(const struct page *p)
{
	return p->text_cap + p->lines_cap * sizeof(struct lline)
		+ p->heights_cap * sizeof(struct lheight)
		+ p->spans_cap * sizeof(struct lspan)
		+ p->links_cap * sizeof(struct llink)
		+ p->anchors_cap * sizeof(struct lanchor)
		+ p->images_cap * sizeof(struct limage)
		+ p->boxes_cap * sizeof(struct lbox);
}

/* grow *arr (of *cap elements of size sz) to hold need; 0 or -1 */
static int grow(struct lay *L, void **arr, unsigned long *cap,
	unsigned long need, size_t sz, unsigned long first)
{
	unsigned long n;
	void *q;

	if (need <= *cap)
		return 0;
	if (L->oom)
		return -1;
	if (L->small && first > 16)
		first = sz == 1 ? 256 : 16;	/* (a cell: little in it) */
	n = *cap ? *cap : first;
	while (n < need)
		n += n / 2 + 16;
	if (used(L->p) + (n - *cap) * sz > L->p->byte_cap) {
		/* one more try at exactly what's needed */
		n = need + 64;
		if (used(L->p) + (n - *cap) * sz > L->p->byte_cap) {
			L->oom = 1;
			L->p->truncated = 1;
			L->stop = 1;
			return -1;
		}
	}
	q = xrealloc(*arr, n * sz);
	if (q == NULL) {
		L->oom = 1;
		L->p->truncated = 1;
		L->stop = 1;
		return -1;
	}
	*arr = q;
	*cap = n;
	return 0;
}

#define GROW(L, arr, cap, need, first) \
	grow(L, (void **)&(L)->p->arr, &(L)->p->cap, need, \
		sizeof(*(L)->p->arr), first)

/* --- measuring ---------------------------------------------------------- */

/* how wide n bytes of text are in the current look: cols on a terminal */
static int text_w(const struct lay *L, const char *s, int n, int cols)
{
	return L->m ? L->m->width(L->m->ctx, L->attr, L->face, s, n) : cols;
}

static int space_w(const struct lay *L)
{
	return text_w(L, " ", 1, 1);
}

/* the open line is as tall as the current look, at least */
static void note_height(struct lay *L)
{
	int h, a;

	if (L->m == NULL)
		return;
	h = L->m->height(L->m->ctx, L->attr, L->face, &a);
	if (L->lh.u == CSS_U_EM || L->lh.u == CSS_U_PX) {
		/* CSS's line height: the difference half above, half below */
		long lh = L->lh.u == CSS_U_EM ? (long)L->lh.v * L->fsz / 100
			: L->lh.v * 3 / 40;

		if (lh < 1)
			lh = 1;
		a += (int)(lh - h) / 2;
		h = (int)lh;
		if (a < 0)
			a = 0;
	}
	if (a > L->line_a)
		L->line_a = a;
	if (h - a > L->line_d)
		L->line_d = h - a;
}

/* a line's height and ascent: its own, or (blank) the plain look's */
static int line_height(const struct lay *L, int content, int *ascent)
{
	int h;

	if (L->m == NULL) {
		*ascent = 0;
		return 1;
	}
	if (content && L->line_a + L->line_d > 0) {
		*ascent = L->line_a;
		return L->line_a + L->line_d;
	}
	h = L->m->height(L->m->ctx, 0, 0, ascent);
	return h;
}

static int put_bytes(struct lay *L, const char *s, size_t n)
{
	struct page *p = L->p;

	if (p->text_len + n > p->text_cap
		&& GROW(L, text, text_cap, p->text_len + n, 4096) < 0)
		return -1;
	if (n <= 16) {
		/* short copies inline: a libc call costs more on a 68030 */
		char *d = p->text + p->text_len;
		size_t i;

		for (i = 0; i < n; i++)
			d[i] = s[i];
	} else
		memcpy(p->text + p->text_len, s, n);
	p->text_len += n;
	return 0;
}

/* attr/link/colour (and with metrics, the face) from here on */
static void set_span(struct lay *L)
{
	struct page *p = L->p;
	struct lspan *s;
	unsigned char face = (unsigned char)(L->m ? L->face : 0);
	unsigned short asc = L->m ? L->asc : 0;

	if (p->nspans) {
		s = &p->spans[p->nspans - 1];
		if (s->attr == L->attr && s->link == L->link && s->face == face
			&& s->color == L->fg && s->asc == asc)
			return;
		if (s->off == p->text_len) {
			/* nothing shown with the last one: replace it */
			s->attr = (unsigned char)L->attr;
			s->link = L->link;
			s->face = face;
			s->color = (unsigned char)L->fg;
			s->asc = asc;
			if (p->nspans >= 2 && s[-1].attr == s->attr
				&& s[-1].link == s->link && s[-1].face == s->face
				&& s[-1].color == s->color && s[-1].asc == s->asc) {
				unsigned long last = --p->nspans - 1, k;

				/* what was taken as the gone one's number is the
				 * one before's: the open line's, the word's, blank
				 * lines' (else a later span takes the number, and
				 * a line seems to start with it) */
				if (L->line_span > last)
					L->line_span = last;
				if (L->word_span > last)
					L->word_span = last;
				for (k = p->nlines; k > 0 && p->lines[k - 1].span > last; k--)
					p->lines[k - 1].span = last;
			}
			return;
		}
	}
	if (GROW(L, spans, spans_cap, p->nspans + 1, 256) < 0)
		return;
	s = &p->spans[p->nspans++];
	s->off = p->text_len;
	s->attr = (unsigned char)L->attr;
	s->link = L->link;
	s->face = face;
	s->color = (unsigned char)L->fg;
	s->asc = asc;
}

/* the page's palette index of colour rgb (added if new); 0 when there is
 * no room: the screen's own colour */
static int page_color(struct page *p, unsigned long rgb)
{
	int i;

	for (i = p->npalette - 1; i >= 0; i--)
		if (p->palette[i] == rgb)
			return i + 1;
	if (p->npalette == LAYOUT_MAX_COLORS)
		return 0;
	if (p->palette == NULL
		&& (p->palette = xmalloc(LAYOUT_MAX_COLORS * sizeof *p->palette)) == NULL)
		return 0;
	p->palette[p->npalette++] = rgb;
	return p->npalette;
}

static unsigned long cur_span(const struct lay *L)
{
	return L->p->nspans ? L->p->nspans - 1 : 0;
}

/* --- lines ----------------------------------------------------------- */

/* the line that content placed now lands on: the open one, or the next,
 * after the blank lines a margin still owes */
static unsigned long here_line(const struct lay *L)
{
	const struct page *p = L->p;

	if (L->line_open || p->nlines == 0)
		return p->nlines;
	return p->nlines + (unsigned long)(L->pend_lines > L->blank_run ?
		L->pend_lines - L->blank_run : 0);
}

static void push_line(struct lay *L, unsigned long off, unsigned long len,
	int indent, unsigned long span, int content)
{
	struct page *p = L->p;
	struct lline *ln;
	int a, h;

	if (GROW(L, lines, lines_cap, p->nlines + 1, 256) < 0)
		return;
	if (L->m && GROW(L, heights, heights_cap, p->nlines + 1, 256) < 0)
		return;
	if (L->m) {
		h = line_height(L, content, &a);
		p->heights[p->nlines].height = (unsigned short)h;
		p->heights[p->nlines].ascent = (unsigned short)a;
		p->heights[p->nlines].over = 0;
		if (L->fl_over > h) {
			/* a float's line on it reaches below */
			long o = L->fl_over - h;

			if (o > 0xFFFF)
				o = 0xFFFF;
			p->heights[p->nlines].over = (unsigned short)o;
			if (o > p->max_over)
				p->max_over = (int)o;
		}
		L->fl_over = 0;
		/* (margins next to each other collapse: see margin()) */
		if (content) {
			L->gap = 0;
			L->ntop_pend = 0;
		} else
			L->gap += h;
	}
	ln = &p->lines[p->nlines++];
	ln->off = off;
	ln->len = (unsigned short)(len > 0xFFFF ? 0xFFFF : len);
	ln->indent = (unsigned short)(indent < 0 ? 0 : indent);
	ln->span = span;
	if (L->max_lines && p->nlines >= L->max_lines) {
		L->stop = 1;
		p->partial = 1;
	}
}

/* end the open line at byte end (trailing spaces dropped): where the text
 * ends (end_line) or where the word that moved down begins (make_room) */
static void close_line_at(struct lay *L, unsigned long end)
{
	struct page *p = L->p;
	const char *t = p->text;
	int endcol = end == p->text_len ? L->col : L->word_col;
	int indent = L->fl_inline ? L->fl_line_x : L->line_indent;

	if (!L->pre)
		while (end > L->line_off && t[end - 1] == ' ') {
			end--;
			endcol -= space_w(L);
		}
	/* centred or to the right (not a table row's line of cells; a grid
	 * cell's only at its column's width: measured, it would seem as wide
	 * as it was let be - it says it has some, to be laid again) */
	if (L->align >= CSS_TA_CENTER && !L->pre && !L->cell && L->small
		&& !L->at_width && end > L->line_off)
		L->aligned = 1;
	if (L->align >= CSS_TA_CENTER && !L->pre && !L->cell && !L->fl_inline
		&& (!L->small || L->at_width)
		&& end > L->line_off && endcol < L->width) {
		int shift = L->width - endcol;
		unsigned long k;

		if (L->align == CSS_TA_CENTER)
			shift /= 2;
		indent += shift;
		/* (its links start further along too) */
		for (k = p->nlinks; k > 0 && p->links[k - 1].line >= p->nlines; k--)
			if (p->links[k - 1].line == p->nlines)
				p->links[k - 1].col = (unsigned short)(p->links[k - 1].col + shift);
	}
	push_line(L, L->line_off, end - L->line_off, indent, L->line_span, 1);
	/* (how wide this needs: its text, and the right sides of the boxes
	 * it is in) */
	if (L->m && end > L->line_off) {
		int need = endcol + L->rin;

		if (need > p->need_w)
			p->need_w = need;
	}
	L->line_open = 0;
	L->blank_run = 0;
	L->fl_inline = 0;
}

static void end_line(struct lay *L)
{
	if (L->line_open)
		close_line_at(L, L->p->text_len);
	L->in_word = 0;
	L->pend_space = 0;
}

static int fl_place(struct lay *L, int *ind);
static void go_to(struct lay *L, int x);

/* a spacer line h units high (with metrics) */
static void push_spacer(struct lay *L, long h)
{
	struct page *p = L->p;

	push_line(L, p->text_len, 0, 0, cur_span(L), 0);
	if (p->heights && p->nlines) {
		p->heights[p->nlines - 1].height = (unsigned short)(h > 0xFFFF ? 0xFFFF : h);
		p->heights[p->nlines - 1].ascent = 0;
	}
}

/*
 * The blank lines owed (never at the top). With metrics they collapse
 * with the margin just above, as CSS's margins do: only as much as they
 * add to it.
 */
static void blank_lines(struct lay *L)
{
	if (L->p->nlines == 0 || L->blank_run >= L->pend_lines)
		return;
	if (L->m && L->gap > 0) {
		int a;
		long want = (long)L->pend_lines * L->m->height(L->m->ctx, 0, 0, &a);
		long gap = L->gap;

		if (want > gap) {
			push_spacer(L, want - gap);
			L->gap = want;
		}
		L->blank_run = L->pend_lines;
		return;
	}
	while (L->blank_run < L->pend_lines) {
		push_line(L, L->p->text_len, 0, 0, cur_span(L), 0);
		L->blank_run++;
	}
}

/* the boxes waiting for their content: their top is below this line */
static void move_tops(struct lay *L)
{
	struct page *p = L->p;
	int i, k;

	for (i = 0; i < L->ntop_pend; i++) {
		struct saved *sv = L->top_pend[i];

		sv->box_line = p->nlines;
		for (k = 0; k < 5 && sv->box && sv->box - 1 + k < p->nboxes; k++)
			p->boxes[sv->box - 1 + k].line = p->nlines;
	}
}

/* a margin h units high: as CSS's, collapsing with those next to it (and
 * the blank lines owed) - the gap is the tallest of them, not their sum */
static void margin(struct lay *L, long h)
{
	long want, gap;

	if (L->m == NULL)
		return;
	end_line(L);
	want = h > 0 ? h : 0;
	if (L->p->nlines && L->pend_lines > L->blank_run) {
		int a;
		long b = (long)(L->pend_lines - L->blank_run)
			* L->m->height(L->m->ctx, 0, 0, &a) + L->gap;

		if (b > want)
			want = b;
	}
	L->pend_lines = 0;
	gap = L->gap;
	if (want > gap) {
		push_spacer(L, want - gap);
		L->gap = want;
		move_tops(L);
	}
}

static void open_line(struct lay *L)
{
	int ind = L->indent;

	/* the blank lines a block's margin asked for (never at the top);
	 * beside a float, each with its line */
	if (L->p->nlines && !L->fl)
		blank_lines(L);
	if (L->p->nlines) {
		while (L->blank_run < L->pend_lines) {
			if (L->fl) {
				int pend = L->pend_lines, br = L->blank_run;

				L->pend_lines = 0;
				open_line(L);
				end_line(L);
				L->pend_lines = pend;
				L->blank_run = br + 1;
				continue;
			}
			push_line(L, L->p->text_len, 0, 0, cur_span(L), 0);
			L->blank_run++;
		}
	}
	L->pend_lines = 0;
	if (ind > L->width / 2 && !L->in_box && !L->fl)
		ind = L->width / 2;
	L->line_open = 1;
	L->line_off = L->p->text_len;
	L->line_span = cur_span(L);
	L->line_a = L->line_d = 0;
	L->in_word = 0;
	L->pend_space = 0;
	if (L->fl && fl_place(L, &ind)) {
		/* (the float's line is on it: the text starts at ind) */
		if (L->marker_w) {
			int a = L->attr, f = L->face, mw;
			unsigned short k = L->link;

			L->attr = 0;
			L->face = 0;
			L->link = 0;
			mw = text_w(L, L->marker, L->marker_w, L->marker_w);
			go_to(L, ind - mw > 0 ? ind - mw : 0);
			L->attr = 0;
			L->face = 0;
			L->link = 0;
			set_span(L);
			put_bytes(L, L->marker, (size_t)L->marker_w);
			note_height(L);
			L->col += mw;
			L->attr = a;
			L->face = f;
			L->link = k;
			L->marker_w = 0;
		}
		go_to(L, ind);
		L->line_indent = ind;	/* (the text's start; the line's is
					 * the float's) */
		return;
	}
	if (L->marker_w) {
		int a = L->attr, f = L->face, mw;
		unsigned short k = L->link;

		/* the marker hangs left of the item's text, plain */
		L->attr = 0;
		L->face = 0;
		L->link = 0;
		mw = text_w(L, L->marker, L->marker_w, L->marker_w);
		L->line_indent = ind - mw;
		if (L->line_indent < 0)
			L->line_indent = 0;
		set_span(L);
		L->line_span = cur_span(L);
		put_bytes(L, L->marker, (size_t)L->marker_w);
		note_height(L);
		L->attr = a;
		L->face = f;
		L->link = k;
		set_span(L);
		L->col = L->line_indent + mw;
		L->marker_w = 0;
	} else {
		L->line_indent = ind;
		L->col = ind;
	}
}

/* a block starts or ends: margin blank lines before what follows */
static void block_break(struct lay *L, int margin)
{
	if (L->cell && L->line_open) {
		/* nothing of this cell yet: stay on the row's line; else
		 * break if more of the cell follows */
		if (L->p->text_len > L->cell_mark && margin + 1 > L->cell_break)
			L->cell_break = margin + 1;
		return;
	}
	if (L->marker_w && !L->line_open) {
		/* a list item that starts with a block: its marker alone */
		open_line(L);
	}
	end_line(L);
	if (margin > L->pend_lines)
		L->pend_lines = margin;
}

/* --- text ------------------------------------------------------------ */

/* is span a plainer (no link, fewer attributes) than span b? */
static int plainer(const struct lspan *a, const struct lspan *b)
{
	int na = 0, nb = 0, i;

	if (!a->link != !b->link)
		return !a->link;
	for (i = 0; i < 8; i++) {
		na += (a->attr >> i) & 1;
		nb += (b->attr >> i) & 1;
	}
	return na < nb;
}

/* before content of w columns: a cell's owed break, the line, the space
 * owed before it (or a line break instead, when it won't fit) */
static void settle(struct lay *L, int w)
{
	if (L->cell_break) {
		int m = L->cell_break - 1;

		L->cell_break = 0;
		end_line(L);
		if (m > L->pend_lines)
			L->pend_lines = m;
	}
	if (!L->line_open)
		open_line(L);
	if (L->pend_space) {
		int sw = space_w(L);

		if (L->col + L->pend_space * sw + w > L->width
			&& L->col > L->line_indent) {
			end_line(L);
			open_line(L);
		} else {
			static const char sp[] = "        ";
			int k = L->pend_space > 8 ? 8 : L->pend_space;
			struct page *p = L->p;

			put_bytes(L, sp, (size_t)k);
			L->col += k * sw;
			/* between two spans the space takes the plainer one's
			 * look: no underline running into a link or out of it */
			if (p->nspans >= 2
				&& p->spans[p->nspans - 1].off == p->text_len - (unsigned long)k
				&& p->spans[p->nspans - 1].off > L->line_off
				&& !(p->spans[p->nspans - 2].face & LF_IMAGE)
				&& plainer(&p->spans[p->nspans - 2],
				&p->spans[p->nspans - 1]))
				p->spans[p->nspans - 1].off += (unsigned long)k;
		}
		L->pend_space = 0;
	}
	if (!L->in_word) {
		L->in_word = 1;
		L->word_off = L->p->text_len;
		L->word_col = L->col;
		L->word_span = cur_span(L);
	}
}

/* w more columns don't fit: move the word begun so far to a new line,
 * or break it there (a word longer than a line, preformatted text) */
/* beside a float: the word moves down written again, after the float's
 * line on the next line (its bytes can't stay where they are) */
static void refit_word(struct lay *L)
{
	struct page *p = L->p;
	unsigned long woff = L->word_off, n = p->text_len - woff, k0, j;
	int wcols = L->col - L->word_col, ns, a = L->attr, f = L->face, fg = L->fg;
	unsigned short link = L->link, asc = L->asc;
	char *bytes;
	struct lspan *sv;

	k0 = layout_span_at(p, L->word_span, woff);
	ns = (int)(p->nspans - k0);
	bytes = xmalloc(n + 1);
	sv = xmalloc((size_t)(ns > 0 ? ns : 1) * sizeof *sv);
	if (bytes == NULL || sv == NULL) {
		xfree(bytes);
		xfree(sv);
		return;
	}
	memcpy(bytes, p->text + woff, n);
	for (j = 0; j < (unsigned long)ns; j++) {
		sv[j] = p->spans[k0 + j];
		sv[j].off = sv[j].off > woff ? sv[j].off - woff : 0;
	}
	/* the line ends before the word; the word's bytes and spans go */
	p->text_len = woff;
	p->nspans = p->spans[k0].off < woff ? k0 + 1 : k0;
	if (p->nspans == 0)
		p->nspans = 1;		/* (the first span stays, empty) */
	L->col = L->word_col;
	end_line(L);
	open_line(L);
	L->word_off = p->text_len;
	L->word_col = L->col;
	for (j = 0; j < (unsigned long)ns; j++) {
		unsigned long e = j + 1 < (unsigned long)ns ? sv[j + 1].off : n;

		L->attr = sv[j].attr;
		L->link = sv[j].link;
		L->face = sv[j].face;
		L->fg = sv[j].color;
		L->asc = sv[j].asc;
		set_span(L);
		if (j == 0)
			L->word_span = cur_span(L);
		if (e > sv[j].off)
			put_bytes(L, bytes + sv[j].off, e - sv[j].off);
	}
	L->attr = a;
	L->face = f;
	L->fg = fg;
	L->link = link;
	L->asc = asc;
	set_span(L);
	L->col += wcols;
	L->in_word = 1;
	note_height(L);
	xfree(bytes);
	xfree(sv);
}

static void make_room(struct lay *L, int w)
{
	if (!L->pre && L->word_col > L->line_indent && L->fl) {
		refit_word(L);
		return;
	}
	if (!L->pre && L->word_col > L->line_indent) {
		/* move the word down: the line ends where it began */
		int wcols = L->col - L->word_col;

		close_line_at(L, L->word_off);
		L->line_open = 1;
		L->line_off = L->word_off;
		L->line_span = L->word_span;
		L->line_a = L->line_d = 0;
		note_height(L);
		L->line_indent = L->indent > L->width / 2 ?
			L->width / 2 : L->indent;
		L->col = L->line_indent + wcols;
		L->word_col = L->line_indent;
	}
	if (L->col + w > L->width && L->col > L->line_indent) {
		end_line(L);
		open_line(L);
		L->in_word = 1;
		L->word_off = L->p->text_len;
		L->word_col = L->col;
		L->word_span = cur_span(L);
	}
}

/* one character of w columns (units), n bytes */
static void put_char(struct lay *L, const char *b, int n, int w)
{
	settle(L, w);
	if (L->col + w > L->width)
		make_room(L, w);
	put_bytes(L, b, (size_t)n);
	note_height(L);
	L->col += w;
}

/*
 * n bytes of w columns with no break allowed inside (a word, or a piece
 * of one): placed as a unit. Only a piece longer than a line (or
 * preformatted text that overflows) goes character by character.
 */
static void put_run(struct lay *L, const char *b, int n, int w)
{
	if (L->cell_break || !L->line_open || L->pend_space || !L->in_word)
		settle(L, w);
	if (L->col + w > L->width) {
		if (L->pre || w > L->width - L->line_indent) {
			const char *e = b + n;

			while (b < e && !L->stop) {
				int k = 1, cw = 1;

				if (L->p->cs == TCS_UTF8 && (unsigned char)*b >= 0x80) {
					const char *q = b;
					unsigned long cp = utf8_get(&q);

					k = (int)(q - b);
					cw = ucs_width(cp);
				}
				put_char(L, b, k, text_w(L, b, k, cw));
				b += k;
			}
			return;
		}
		make_room(L, w);
	}
	put_bytes(L, b, (size_t)n);
	note_height(L);
	L->col += w;
}

static void put_text(struct lay *L, const char *s)
{
	enum term_cs cs = L->p->cs;

	while (*s && !L->stop) {
		unsigned char c = (unsigned char)*s;
		char b[8];
		int n, w;

		if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
			s++;
			if (!L->pre) {
				if (L->line_open)
					L->pend_space = 1;
				L->in_word = 0;
				continue;
			}
			if (c == '\n') {
				if (!L->line_open)
					open_line(L);
				end_line(L);
			} else if (c == '\t') {
				int to, sw = space_w(L), tab = 8 * sw;

				if (!L->line_open)
					open_line(L);
				to = L->line_indent
					+ ((L->col - L->line_indent) / tab + 1) * tab;
				/* (only spaces that fit: one that wrapped would
				 * start the line again, short of the stop) */
				while (L->col < to && L->col + sw <= L->width)
					put_char(L, " ", 1, sw);
			} else if (c == ' ')
				put_char(L, " ", 1, space_w(L));
			continue;
		}
		if (c < 0x80) {
			const char *r = s;

			/* a run of plain characters: one piece of a word */
			while ((unsigned char)*s > ' ' && (unsigned char)*s < 0x7F)
				s++;
			if (s == r) {
				s++;		/* a control character */
				continue;
			}
			put_run(L, r, (int)(s - r), text_w(L, r, (int)(s - r),
				(int)(s - r)));
			continue;
		}
		{
			unsigned long cp = utf8_get(&s);

			if (cs == TCS_UTF8) {
				w = ucs_width(cp);
				if (w == 0 && !L->line_open)
					continue;
				n = utf8_put(b, cp);
			} else {
				n = translit(cp, cs == TCS_LATIN1, b);
				w = n;
				if (n == 0)
					continue;
			}
			if (cp >= 0x80 && cp <= 0x9F)
				continue;
			put_run(L, b, n, text_w(L, b, n, w));
		}
	}
}

/* text with its own attributes, e.g. a form field's rendering */
static void put_marked(struct lay *L, const char *s, int attr)
{
	int a = L->attr;

	L->attr |= attr;
	set_span(L);
	put_text(L, s);
	L->attr = a;
	set_span(L);
}

/* --- lists ----------------------------------------------------------- */

/* an attribute's number: leading white space, sign, digits */
static long atol_safe(const char *s)
{
	long n = 0;
	int neg = 0;

	while (*s == ' ')
		s++;
	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	while (*s >= '0' && *s <= '9' && n < 100000000L)
		n = n * 10 + (*s++ - '0');
	return neg ? -n : n;
}

static void roman(long n, int upper, char *out)
{
	static const char *const lo[] = { "m", "cm", "d", "cd", "c", "xc",
		"l", "xl", "x", "ix", "v", "iv", "i" };
	static const int val[] = { 1000, 900, 500, 400, 100, 90, 50, 40,
		10, 9, 5, 4, 1 };
	int i;

	*out = '\0';
	if (n <= 0 || n >= 4000) {
		sprintf(out, "%ld", n);
		return;
	}
	for (i = 0; i < 13; i++)
		while (n >= val[i]) {
			const char *q;

			for (q = lo[i]; *q; q++)
				*out++ = upper ? (char)(*q - 32) : *q;
			n -= val[i];
		}
	*out = '\0';
}

static void make_marker(struct lay *L, nodeid li)
{
	struct list *ls = L->nlists ? &L->lists[L->nlists - 1] : NULL;
	const char *v;
	char num[24];

	if (ls == NULL || !ls->ordered) {
		static const char bullets[] = "*+o-";
		int depth = ls ? L->nlists - 1 : 0;

		L->marker[0] = bullets[depth % 4];
		L->marker[1] = ' ';
		L->marker[2] = '\0';
		L->marker_w = L->nomarker ? 0 : 2;	/* (list-style: none) */
		return;
	}
	if ((v = doc_attr(L->d, li, ATTR_VALUE)) != NULL)
		ls->counter = atol_safe(v);
	switch (ls->type) {
	case 'a':
	case 'A':
		if (ls->counter >= 1 && ls->counter <= 26) {
			num[0] = (char)((ls->type == 'a' ? 'a' : 'A')
				+ ls->counter - 1);
			num[1] = '\0';
		} else
			sprintf(num, "%ld", ls->counter);
		break;
	case 'i':
	case 'I':
		roman(ls->counter, ls->type == 'I', num);
		break;
	default:
		sprintf(num, "%ld", ls->counter);
	}
	ls->counter++;
	if (strlen(num) > 12)
		num[12] = '\0';
	sprintf(L->marker, "%s. ", num);
	L->marker_w = (int)strlen(L->marker);
	if (L->nomarker)
		L->marker_w = 0;	/* (list-style: none; still counted) */
}

static void list_begin(struct lay *L, nodeid id, int tag)
{
	struct list *ls;
	const char *v;

	if (L->nlists == MAX_LISTS)
		return;
	ls = &L->lists[L->nlists++];
	ls->ordered = tag == TAG_OL;
	ls->type = '1';
	ls->counter = 1;
	if (ls->ordered) {
		if ((v = doc_attr(L->d, id, ATTR_TYPE)) != NULL
			&& strchr("aAiI", v[0]) && v[0])
			ls->type = (unsigned char)v[0];
		if ((v = doc_attr(L->d, id, ATTR_START)) != NULL)
			ls->counter = atol_safe(v);
	}
}

/* --- elements with their own rendering -------------------------------- */

/* the first text inside id, white space trimmed, at most max bytes */
static void inner_text(const struct doc *d, nodeid id, char *out, size_t max)
{
	nodeid c;
	size_t n = 0;

	out[0] = '\0';
	for (c = d->nodes[id].first; c; c = d->nodes[c].next)
		if (d->nodes[c].type == NODE_TEXT) {
			const char *t = doc_text(d, c);

			while (*t == ' ' || *t == '\n' || *t == '\t')
				t++;
			while (*t && n + 1 < max)
				out[n++] = *t++;
			break;
		}
	while (n && (out[n - 1] == ' ' || out[n - 1] == '\n'))
		n--;
	out[n] = '\0';
}

/* a link for a form field, so that it can be selected (Phase 4 edits) */
static void field_begin(struct lay *L, nodeid id)
{
	struct page *p = L->p;
	struct llink *k;

	if (GROW(L, links, links_cap, p->nlinks + 1, 64) < 0)
		return;
	k = &p->links[p->nlinks++];
	k->node = id;
	k->kind = LK_FIELD;
	k->pad = 0;
	k->line = here_line(L);
	k->col = (unsigned short)L->col;
	L->link = (unsigned short)p->nlinks;
}

static void render_field(struct lay *L, nodeid id, const char *s)
{
	unsigned short k = L->link;

	/* a field inside a link stays part of that link */
	if (!k)
		field_begin(L, id);
	put_marked(L, s, SA_FIELD);
	L->link = k;
	set_span(L);
}

static void render_input(struct lay *L, nodeid id)
{
	const struct doc *d = L->d;
	const struct field *f = L->fs ? forms_field(L->fs, id) : NULL;
	const char *type = doc_attr(d, id, ATTR_TYPE);
	const char *val = doc_attr(d, id, ATTR_VALUE);
	char buf[80];
	int size, i, n, checked = doc_attr(d, id, ATTR_CHECKED) != NULL;

	if (f) {
		checked = f->checked;
		if (f->type == FT_TEXT || f->type == FT_PASSWORD)
			val = f->value;
	}
	if (type == NULL)
		type = "text";
	if (strcmp(type, "hidden") == 0)
		return;
	/* (with fonts one word, which the screen draws as a box) */
	if (strcmp(type, "checkbox") == 0) {
		render_field(L, id, checked ? "[x]" : L->m ? "[_]" : "[ ]");
		return;
	}
	if (strcmp(type, "radio") == 0) {
		render_field(L, id, checked ? "(*)" : L->m ? "(_)" : "( )");
		return;
	}
	if (strcmp(type, "submit") == 0 || strcmp(type, "button") == 0
		|| strcmp(type, "reset") == 0 || strcmp(type, "image") == 0) {
		if (val == NULL || !*val)
			val = strcmp(type, "reset") == 0 ? "Reset"
				: strcmp(type, "image") == 0
				&& doc_attr(d, id, ATTR_ALT) ?
				doc_attr(d, id, ATTR_ALT) : "Submit";
		n = snprintf(buf, sizeof buf, "[%.60s]", val);
		(void)n;
		render_field(L, id, buf);
		return;
	}
	if (strcmp(type, "file") == 0) {
		render_field(L, id, "[file...]");
		return;
	}
	/* text-like: a box of size= columns showing the value */
	size = doc_attr(d, id, ATTR_SIZE) ?
		(int)atol_safe(doc_attr(d, id, ATTR_SIZE)) : 20;
	if (size < 4)
		size = 4;
	if (size > 30)
		size = 30;
	if (size > L->width / L->em - 4)
		size = L->width / L->em - 4;
	if ((val == NULL || !*val) && !f) {
		val = doc_attr(d, id, ATTR_PLACEHOLDER);
		if (val == NULL)
			val = "";
	} else if (val == NULL)
		val = "";
	buf[0] = '[';
	n = 1;
	if (strcmp(type, "password") == 0) {
		int cols = 0;

		for (i = 0; val[i] && cols < size; i++)
			if (((unsigned char)val[i] & 0xC0) != 0x80) {
				buf[n++] = '*';	/* one per character */
				cols++;
			}
		size -= cols;
		size += 1;
	} else {
		/* whole characters only */
		const char *v = val;
		int cols = 0;

		while (*v && cols < size && n < (int)sizeof buf - 8) {
			const char *c0 = v;

			(void)utf8_get(&v);
			memcpy(buf + n, c0, (size_t)(v - c0));
			n += (int)(v - c0);
			cols++;
		}
		size -= cols;
		size += 1;
	}
	while (size-- > 1 && n < (int)sizeof buf - 2)
		buf[n++] = '_';
	buf[n++] = ']';
	buf[n] = '\0';
	/* the box itself mustn't wrap at its underscores: no spaces in it */
	render_field(L, id, buf);
}

static void render_select(struct lay *L, nodeid id)
{
	const struct doc *d = L->d;
	const struct field *f = L->fs ? forms_field(L->fs, id) : NULL;
	nodeid c, g, first = 0, sel = 0;
	char txt[48], buf[64];

	if (f) {
		snprintf(buf, sizeof buf, "[%.44s v]", forms_text(L->fs, f));
		render_field(L, id, buf);
		return;
	}
	for (c = d->nodes[id].first; c && !sel; c = d->nodes[c].next) {
		const struct node *n = &d->nodes[c];

		if (n->type != NODE_ELEM)
			continue;
		if (n->tag == TAG_OPTION) {
			if (!first)
				first = c;
			if (doc_attr(d, c, ATTR_SELECTED))
				sel = c;
		} else if (n->tag == TAG_OPTGROUP)
			for (g = n->first; g && !sel; g = d->nodes[g].next)
				if (d->nodes[g].type == NODE_ELEM
					&& d->nodes[g].tag == TAG_OPTION) {
					if (!first)
						first = g;
					if (doc_attr(d, g, ATTR_SELECTED))
						sel = g;
				}
	}
	if (!sel)
		sel = first;
	txt[0] = '\0';
	if (sel) {
		inner_text(d, sel, txt, sizeof txt);
		if (!txt[0] && doc_attr(d, sel, ATTR_LABEL))
			snprintf(txt, sizeof txt, "%s",
				doc_attr(d, sel, ATTR_LABEL));
	}
	snprintf(buf, sizeof buf, "[%s v]", txt);
	render_field(L, id, buf);
}

static void render_textarea(struct lay *L, nodeid id)
{
	const struct field *f = L->fs ? forms_field(L->fs, id) : NULL;
	char txt[40], buf[56];
	size_t i;

	if (f) {
		/* whole characters of its start */
		const char *v = forms_text(L->fs, f), *q = v;

		while (*q && q - v < (long)sizeof txt - 5)
			(void)utf8_get(&q);
		snprintf(txt, sizeof txt, "%.*s", (int)(q - v), v);
	} else
		inner_text(L->d, id, txt, sizeof txt);
	for (i = 0; txt[i]; i++)
		if (txt[i] == '\n' || txt[i] == '\t')
			txt[i] = ' ';
	snprintf(buf, sizeof buf, "[%s___]", txt);
	render_field(L, id, buf);
}

/* the last path segment of a URL, for a link with nothing to show */
/* a width or height attribute in units ("120", "120px"; "50%" of pct),
 * or 0 */
static long dim(const struct lay *L, const char *v, long pct)
{
	long n = 0;

	if (v == NULL)
		return 0;
	while (*v == ' ')
		v++;
	while (*v >= '0' && *v <= '9' && n < 100000)
		n = n * 10 + (*v++ - '0');
	if (*v == '.')
		while (*++v >= '0' && *v <= '9')
			;
	if (*v == '%')
		return pct > 0 ? pct * n / 100 : 0;
	(void)L;
	return n;
}

/*
 * An <img> as itself, when its size is known: from width and height, or
 * from one of them and the image's own proportions, or its own size; no
 * wider than the line, in proportion. A break is allowed on either side,
 * as around any image. 0, or -1 when its size isn't known (yet).
 */
static int put_image(struct lay *L, nodeid id)
{
	struct page *p = L->p;
	long avail = L->width - L->indent, w, h;
	long aw = dim(L, doc_attr(L->d, id, ATTR_WIDTH), avail);
	long ah = dim(L, doc_attr(L->d, id, ATTR_HEIGHT), 0);
	int iw = 0, ih = 0, face = L->face;
	unsigned long k;
	char b[LAYOUT_IMG_BYTES];

	if (aw > 0 && ah > 0) {
		w = aw;
		h = ah;
	} else if (L->m->image(L->m->image_ctx, id, &iw, &ih) && iw > 0 && ih > 0) {
		if (aw > 0) {
			w = aw;
			h = ((long)ih * aw + iw / 2) / iw;
		} else if (ah > 0) {
			h = ah;
			w = ((long)iw * ah + ih / 2) / ih;
		} else {
			w = iw;
			h = ih;
		}
	} else
		return -1;
	if (avail < 1)
		avail = 1;
	if (w > avail) {
		h = (h * avail + w / 2) / w;
		w = avail;
	}
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (h > 8192)
		h = 8192;
	if (p->nimages >= LAYOUT_MAX_IMAGES
		|| GROW(L, images, images_cap, p->nimages + 1, 16) < 0)
		return -1;
	k = p->nimages++;
	p->images[k].node = id;
	p->images[k].w = (unsigned short)w;
	p->images[k].h = (unsigned short)h;
	b[0] = '\001';
	b[1] = (char)(2 + k / 900);
	b[2] = (char)(2 + k / 30 % 30);
	b[3] = (char)(2 + k % 30);

	L->in_word = 0;
	settle(L, (int)w);
	if (L->col + w > L->width)
		make_room(L, (int)w);
	p->images[k].line = p->nlines;	/* (the open line's number) */
	L->face = face | LF_IMAGE;
	set_span(L);
	put_bytes(L, b, sizeof b);
	L->face = face;
	set_span(L);
	if (h > L->line_a)
		L->line_a = (int)h;	/* (on the baseline: all above it) */
	L->col += (int)w;
	L->in_word = 0;
	return 0;
}

static void url_name(const char *href, char *out, size_t max)
{
	const char *e = href + strcspn(href, "?#"), *b = e;
	size_t n;

	while (b > href && b[-1] == '/')
		b--;
	e = b;
	while (b > href && b[-1] != '/')
		b--;
	n = (size_t)(e - b);
	if (n == 0 || (b > href && b[-1] == '/' && b - 2 >= href
		&& b[-2] == '/')) {
		/* "http://host/" : the host */
		snprintf(out, max, "%s", "link");
		if (n && (size_t)(e - b) < max) {
			memcpy(out, b, n);
			out[n] = '\0';
		}
		return;
	}
	if (n >= max)
		n = max - 1;
	memcpy(out, b, n);
	out[n] = '\0';
}

/* --- the walk --------------------------------------------------------- */

static void add_anchor(struct lay *L, nodeid id)
{
	struct page *p = L->p;

	if (GROW(L, anchors, anchors_cap, p->nanchors + 1, 64) < 0)
		return;
	p->anchors[p->nanchors].node = id;
	p->anchors[p->nanchors].line = here_line(L);
	p->nanchors++;
}

/* the attributes enter() looks at, in one pass (doc_attr per attribute
 * costs a scan and a libc strlen each) */
struct eattr {
	const char *hidden, *style, *id, *name, *href, *cls, *align;
};

static void scan_attrs(const struct doc *d, nodeid id, struct eattr *a)
{
	const unsigned char *p = d->attr + d->nodes[id].data;
	const unsigned char *e = d->attr + d->attr_len;

	a->hidden = a->style = a->id = a->name = a->href = a->cls = a->align = NULL;
	while (p < e && *p) {
		int k = *p++;
		const char *v = (const char *)p;

		switch (k) {
		case ATTR_HIDDEN: a->hidden = v; break;
		case ATTR_STYLE: a->style = v; break;
		case ATTR_ID: a->id = v; break;
		case ATTR_NAME: a->name = v; break;
		case ATTR_HREF: a->href = v; break;
		case ATTR_CLASS: a->cls = v; break;
		case ATTR_ALIGN: a->align = v; break;
		}
		while (*p)
			p++;
		p++;
	}
}

static void walk(struct lay *L, nodeid root, int depth);
static int grid_table(struct lay *L, nodeid id);

/* how an element's text looks, from its style sheet and style="" (ct)
 * over the built-in style s: weight, italics, underline, colour,
 * alignment (also <center> and align=, which the sheet overrides) */
/* the face's size class nearest a size in tenths of a unit */
static int size_class(int fsz)
{
	static const int px[8] = LF_SIZE_PX;
	int k, best = 3, d = 1 << 30;

	for (k = 1; k < 8; k++) {
		int e = px[k] * 10 - fsz;

		if (e < 0)
			e = -e;
		if (e < d) {
			d = e;
			best = k;
		}
	}
	return LF_SIZE(best);
}

static void text_look(struct lay *L, int tag, const struct style *s,
	const struct css_text *ct, const char *align)
{
	if (ct->weight == CSS_FW_BOLD)
		L->attr |= SA_BOLD;
	else if (ct->weight == CSS_FW_NORMAL)
		L->attr &= ~SA_BOLD;
	if (ct->style == CSS_FS_ITALIC) {
		L->face |= LF_ITALIC;
		if (L->m == NULL)
			L->attr |= SA_UNDER;	/* (a terminal's italics) */
	} else if (ct->style == CSS_FS_NORMAL) {
		L->face &= ~LF_ITALIC;
		if (L->m == NULL && (s->face & LF_ITALIC))
			L->attr &= ~SA_UNDER;
	}
	if (ct->deco == CSS_TD_UNDER)
		L->attr |= SA_UNDER;
	else if (ct->deco == CSS_TD_NONE)
		L->attr &= ~SA_UNDER;
	if (L->m && ct->fsize.u == CSS_U_PX) {
		/* a size in CSS pixels: the browsers' usual 16 is the
		 * screen's 12 */
		L->fsz = ct->fsize.v * 3 / 4;
		L->face = (L->face & ~LF_SMASK) | size_class(L->fsz);
	} else if (L->m && ct->fsize.u == CSS_U_EM) {
		L->fsz = (int)((long)L->fsz * ct->fsize.v / 10);
		L->face = (L->face & ~LF_SMASK) | size_class(L->fsz);
	}
	/* (no smaller than 10 pixels: less can't be read in the screen's
	 * fonts - browsers have a least size too) */
	if (L->fsz < 100) {
		L->fsz = 100;
		if (L->face & LF_SMASK)
			L->face = (L->face & ~LF_SMASK) | size_class(L->fsz);
	} else if (L->fsz > 480)
		L->fsz = 480;
	if (ct->fg & CSS_RGB_SET)
		L->fg = page_color(L->p, ct->fg & 0xFFFFFFUL);
	else if (ct->fg & CSS_RGB_DEFAULT)
		L->fg = 0;
	/* alignment is a block's; a table starts at the left again (as in
	 * browsers' quirks mode: pages built of tables inside <center> keep
	 * their cells' text left), and its align= places the table, not
	 * its text */
	if (s->display == D_INLINE)
		return;
	if (tag == TAG_TABLE) {
		L->align = 0;
		align = NULL;
	}
	if (tag == TAG_CENTER)
		L->align = CSS_TA_CENTER;
	if (align) {
		if (align[0] == 'c' || align[0] == 'C')
			L->align = CSS_TA_CENTER;
		else if (align[0] == 'r' || align[0] == 'R')
			L->align = CSS_TA_RIGHT;
		else if (align[0] == 'l' || align[0] == 'L' || align[0] == 'j' || align[0] == 'J')
			L->align = CSS_TA_LEFT;
	}
	if (ct->align)
		L->align = ct->align;
}

/* --- CSS boxes (with metrics) ------------------------------------------ */

static int dark(unsigned long rgb);
static void float_flush(struct lay *L);
struct fitem;

/* what the sheet and style="" say of element id's box (only where boxes
 * are drawn): 1 if anything */
static int el_box(struct lay *L, nodeid id, const struct eattr *ea,
	struct css_box *b)
{
	memset(b, 0, sizeof *b);
	if (L->m == NULL || !css_boxes)
		return 0;
	if (L->d->sheet) {
		/* (css_style first: it remembers whether to ask at all) */
		css_style(L->d->sheet, L->d, id, ea->id, ea->cls, L->vw, NULL);
		css_box(L->d->sheet, L->d, id, ea->id, ea->cls, L->vw, b);
	}
	if (ea->style)
		css_inline_box(ea->style, b);
	return b->set != 0;
}

/* n/d to the nearest (d > 0) */
static long rdiv(long n, long d)
{
	return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

/* a CSS length in units (of: what a % is of) */
static int box_units(const struct lay *L, struct css_len l, int of)
{
	long v;

	switch (l.u) {
	case CSS_U_PX:
		v = rdiv(l.v, 10);
		break;
	case CSS_U_EM:
		v = rdiv((long)l.v * L->fsz, 100);
		break;
	case CSS_U_PCT:
		/* rounded: Acid1's 10.638% of 470 is 50, not 49 */
		v = rdiv((long)l.v * of, 1000);
		break;
	default:
		return 0;
	}
	return v < -4000 ? -4000 : v > 8000 ? 8000 : (int)v;
}

/* the box's sides, in units: margins, borders (0 when not drawn),
 * padding; of: the width a % is of */
static void box_sides(const struct lay *L, const struct css_box *b, int of,
	int *m, int *bw, int *pd)
{
	int i;

	for (i = 0; i < 4; i++) {
		m[i] = b->set & CSS_B_M(i) ? box_units(L, b->m[i], of) : 0;
		pd[i] = b->set & CSS_B_P(i) ? box_units(L, b->p[i], of) : 0;
		bw[i] = (b->set & CSS_B_BSTYLE) && b->bstyle && (b->set & CSS_B_BW(i))
			? box_units(L, b->bw[i], of) : 0;
		if (pd[i] < 0)
			pd[i] = 0;
		if (bw[i] < 0)
			bw[i] = 0;
	}
}

/* does the box show (a background, a border, padding, a size)? */
static int box_shows(const struct css_box *b)
{
	int i;

	if ((b->set & CSS_B_BG) && b->bg != CSS_BG_NONE)
		return 1;
	for (i = 0; i < 4; i++) {
		if ((b->set & CSS_B_BSTYLE) && b->bstyle && (b->set & CSS_B_BW(i))
			&& b->bw[i].v > 0)
			return 1;
		if ((b->set & CSS_B_P(i)) && b->p[i].v > 0)
			return 1;
	}
	return ((b->set & CSS_B_W) && b->w.u != CSS_U_AUTO)
		|| ((b->set & CSS_B_H) && b->h.u != CSS_U_AUTO);
}

/* the blank lines owed, made now (a box's edge must be where they end) */
static void flush_blank(struct lay *L)
{
	end_line(L);
	blank_lines(L);
	L->pend_lines = 0;
}

/* an empty line exactly h units high */
static void vspace(struct lay *L, int h)
{
	if (h <= 0 || L->m == NULL)
		return;
	flush_blank(L);
	push_spacer(L, h);
	L->gap = 0;			/* (a border, padding: margins don't
					 * collapse through it) */
	L->ntop_pend = 0;
}

/* how tall lines from..nlines are, in units */
static long lines_h(const struct lay *L, unsigned long from)
{
	long h = 0;
	unsigned long k;

	if (L->p->heights == NULL)
		return 0;
	for (k = from; k < L->p->nlines; k++)
		h += L->p->heights[k].height;
	return h;
}

/* a block with a CSS box starts: its margin above, its box (reserved now,
 * filled at the end: drawn before what's in it), its border and padding;
 * the lines inside go in by its left side, no wider than it */
static void box_open(struct lay *L, struct saved *sv, const struct css_box *b,
	int draw)
{
	struct page *p = L->p;
	int m[4], bw[4], pd[4], of = L->width - L->indent, x, w, i;
	unsigned long first;

	box_sides(L, b, of, m, bw, pd);
	for (i = 0; i < 4; i++)
		if (m[i] < 0)
			m[i] = 0;	/* (negative margins: not here) */
	if (b->set & CSS_B_M(0))
		margin(L, m[0]);
	flush_blank(L);
	x = L->indent + m[3];
	if ((b->set & CSS_B_W) && b->w.u != CSS_U_AUTO) {
		w = box_units(L, b->w, of) + pd[1] + pd[3] + bw[1] + bw[3];
		/* margin: auto either side: in the middle */
		if ((b->set & CSS_B_M(1)) && (b->set & CSS_B_M(3))
			&& b->m[1].u == CSS_U_AUTO && b->m[3].u == CSS_U_AUTO && w < of)
			x = L->indent + (of - w) / 2;
	} else
		w = L->width - x - m[1];
	if (w > L->width - x)
		w = L->width - x;
	if (w < 1)
		w = 1;
	sv->boxk = draw ? 1 : 2;
	sv->box = 0;
	sv->box_rin = L->rin;
	/* (a box as wide as it says: its page needs that) */
	if ((b->set & CSS_B_W) && b->w.u != CSS_U_AUTO && L->m
		&& x + w + m[1] > p->need_w)
		p->need_w = x + w + m[1];
	L->rin += m[1] + (draw ? bw[1] + pd[1] : 0);
	sv->box_width = L->width;
	sv->box_cut = L->fl_cut;
	sv->box_m_b = b->set & CSS_B_M(2) ? m[2] : -1;
	if (!draw) {
		/* margins only: the lines go in by them */
		sv->box_line = p->nlines;
		sv->box_in_b = 0;
		sv->box_minh = 0;
		L->indent = x;
		L->width = x + w;
		if (L->width < L->indent + L->em)
			L->width = L->indent + L->em;
		L->in_box++;
		return;
	}
	/* the background, then each side's border: filled in at the end */
	first = p->nboxes;
	if (GROW(L, boxes, boxes_cap, p->nboxes + 5, 16) < 0)
		return;
	for (i = 0; i < 5; i++) {
		struct lbox *lb = &p->boxes[p->nboxes++];

		memset(lb, 0, sizeof *lb);
		lb->line = p->nlines;
		lb->bg = LBOX_NONE;
		lb->border_rgb = LBOX_NONE;
	}
	p->boxes[first].x = x;
	p->boxes[first].w = w;
	if ((b->set & CSS_B_BG) && b->bg != CSS_BG_NONE) {
		p->boxes[first].bg = b->bg;
		if (dark(b->bg))
			L->attr |= SA_ONDARK;
	}
	/* (sides: top, right, bottom, left; their heights come later) */
	for (i = 0; i < 4; i++) {
		struct lbox *lb = &p->boxes[first + 1 + i];

		if (bw[i] == 0)
			continue;
		lb->bg = (b->set & CSS_B_BCOLOR) ? b->bcolor : 0;
		lb->x = i == 1 ? x + w - bw[1] : x;
		lb->w = i == 1 || i == 3 ? bw[i] : w;
		lb->h = i == 0 || i == 2 ? bw[i] : 0;
	}
	sv->box = first + 1;
	sv->box_line = p->nlines;
	sv->box_in_b = bw[2] + pd[2];
	sv->box_minh = (b->set & CSS_B_H) && b->h.u != CSS_U_AUTO
		? box_units(L, b->h, 0) : 0;
	vspace(L, bw[0] + pd[0]);
	sv->box_minh += bw[0] + pd[0];
	if (bw[0] + pd[0] == 0 && L->ntop_pend < 8)
		L->top_pend[L->ntop_pend++] = sv;
	L->indent = x + bw[3] + pd[3];
	L->width = x + w - bw[1] - pd[1];
	if (L->width < L->indent + L->em)
		L->width = L->indent + L->em;
	L->in_box++;
}

/* ... and ends: its height known, its boxes filled */
static void box_close(struct lay *L, const struct saved *sv)
{
	struct page *p = L->p;
	struct lbox *lb;
	long h;
	int i;

	if (L->fl && L->fl_level >= L->in_box)
		float_flush(L);		/* (a float in it stays in it) */
	if (sv->boxk == 2) {
		end_line(L);
		L->rin = sv->box_rin;
		L->width = sv->box_width + sv->box_cut - L->fl_cut;
		L->in_box--;
		if (sv->box_m_b >= 0)
			margin(L, sv->box_m_b);
		return;
	}
	for (i = 0; i < L->ntop_pend; i++)
		if (L->top_pend[i] == sv) {
			L->ntop_pend = i;	/* (it and those inside it) */
			break;
		}
	flush_blank(L);
	h = lines_h(L, sv->box_line);
	/* (margins at its end, no border or padding below them: they
	 * collapse through its bottom, outside it) */
	if (sv->box_in_b == 0 && L->gap > 0 && L->gap <= h)
		h -= L->gap;
	if (h < sv->box_minh) {
		vspace(L, (int)(sv->box_minh - h));
		h = sv->box_minh;
	}
	lb = &p->boxes[sv->box - 1];
	if (sv->box_in_b) {
		int in_b = sv->box_in_b;

		/* ending with floats, and nothing of its own to show: its
		 * bottom padding is beside them, not below (CSS1: it is
		 * no taller than its text; Acid1's dl) */
		if (p->heights && p->nlines && p->gap_line == p->nlines
			&& lb->bg == LBOX_NONE && lb[1].w == 0 && lb[2].w == 0
			&& lb[3].w == 0 && lb[4].w == 0)
			in_b = in_b > p->heights[p->nlines - 1].height
				? in_b - p->heights[p->nlines - 1].height : 0;
		if (in_b)
			vspace(L, in_b);
		h = lines_h(L, sv->box_line);
	}
	lb->h = h;
	for (i = 0; i < 4; i++) {
		struct lbox *sd = &lb[1 + i];

		if (sd->w == 0)
			continue;
		if (i == 1 || i == 3)
			sd->h = h;
		else if (i == 2)
			sd->dy = h - sd->h;
	}
	/* (a float beside it may have ended meanwhile) */
	L->width = sv->box_width + sv->box_cut - L->fl_cut;
	L->rin = sv->box_rin;
	L->in_box--;
	if (sv->box_m_b >= 0)
		margin(L, sv->box_m_b);
}

/* enter element id at depth: 1 when its children are to be laid out */
static int enter(struct lay *L, nodeid id, int depth)
{
	const struct doc *d = L->d;
	const struct node *n = &d->nodes[id];
	struct style s;
	struct saved *sv;
	struct eattr ea;
	struct css_text ct;
	struct css_box bx;
	const char *v;
	int tag = n->tag, cssf = 0, boxed, lh_new;

	if (n->data)
		scan_attrs(d, id, &ea);
	else
		ea.hidden = ea.style = ea.id = ea.name = ea.href = ea.cls = ea.align = NULL;
	style_for(tag, ea.hidden, ea.style, ea.id, &s);
	memset(&ct, 0, sizeof ct);
	/* the page's style sheet */
	if (s.display != D_NONE && L->d->sheet) {
		cssf = css_style(L->d->sheet, L->d, id, ea.id, ea.cls, L->vw, &ct);
		if ((cssf & CSS_HIDDEN) && !css_inline_shows(ea.style))
			s.display = D_NONE;
	}
	if (s.display == D_NONE)
		return 0;
	if (ea.style)
		css_inline_text(L->d->sheet, ea.style, &ct);
	boxed = 0;
	bx.set = 0;
	if (L->m && !L->small && (tag == TAG_HTML || tag == TAG_BODY)
		&& el_box(L, id, &ea, &bx) && (bx.set & CSS_B_BG) && bx.bg != CSS_BG_NONE
		&& (tag == TAG_HTML || L->p->page_bg == LBOX_NONE)) {
		/* the root's background (else the body's) is the page's */
		L->p->page_bg = bx.bg;
		if (tag == TAG_HTML)
			L->html_bg = 1;
		if (dark(bx.bg))
			L->attr |= SA_ONDARK;
		if (tag == TAG_BODY)
			bx.set &= ~CSS_B_BG;	/* (not its box's too) */
	}
	if (L->m && tag != TAG_HTML && tag != TAG_TABLE && n->type == NODE_ELEM
		&& el_box(L, id, &ea, &bx)) {
		if (tag == TAG_BODY && L->p->page_bg != LBOX_NONE && !L->html_bg)
			bx.set &= ~CSS_B_BG;
		if (bx.set & CSS_B_DISP)
			s.display = bx.disp == CSS_DI_BLOCK ? D_BLOCK
				: bx.disp == CSS_DI_LIST_ITEM ? D_LIST_ITEM : D_INLINE;
		boxed = (s.display == D_BLOCK || s.display == D_LIST_ITEM)
			&& !L->cell && !L->pre && box_shows(&bx);
		/* a block that only has margins: they say where it is */
		if (!boxed && (s.display == D_BLOCK || s.display == D_LIST_ITEM)
			&& !L->cell && !L->pre && tag != TAG_BODY
			&& (bx.set & (CSS_B_M(0) | CSS_B_M(1) | CSS_B_M(2) | CSS_B_M(3))))
			boxed = 2;
		if ((bx.set & CSS_B_CLEAR) && bx.clr != CSS_CL_NONE)
			float_flush(L);
		/* (its margins say where it is: no blank lines of its own) */
		if (boxed && (bx.set & (CSS_B_M(0) | CSS_B_M(2))))
			s.margin = 0;
	}
	lh_new = (bx.set & CSS_B_LH) != 0;

	/* leaves with a rendering of their own */
	switch (tag) {
	case TAG_TABLE:
		if (grid_table(L, id))
			return 0;		/* laid out as a grid */
		break;
	case TAG_BR:
		if (!L->line_open)
			open_line(L);
		end_line(L);
		return 0;
	case TAG_HR: {
		int i, n, dw;

		block_break(L, 0);
		/* in a table cell that's a break owed: make it (a line opened
		 * over the open one would lose what is on it) */
		if (L->cell_break) {
			int m = L->cell_break - 1;

			L->cell_break = 0;
			end_line(L);
			if (m > L->pend_lines)
				L->pend_lines = m;
		}
		if (!L->line_open)
			open_line(L);
		dw = text_w(L, "-", 1, 1);
		n = (L->width - L->col) / (dw > 0 ? dw : 1);
		L->attr |= SA_RULE;
		set_span(L);
		for (i = 0; i < n; i++)
			put_bytes(L, "-", 1);
		note_height(L);
		L->col += n * dw;
		L->attr &= ~SA_RULE;
		set_span(L);
		end_line(L);
		return 0;
	}
	case TAG_IMG:
	case TAG_IMAGE:
		if (L->m && L->m->image && put_image(L, id) == 0)
			return 0;
		v = doc_attr(d, id, ATTR_ALT);
		if (v && *v) {
			char buf[128];

			snprintf(buf, sizeof buf, "[%s]", v);
			put_text(L, buf);
		}
		return 0;
	case TAG_INPUT:
		render_input(L, id);
		return 0;
	case TAG_SELECT:
		render_select(L, id);
		return 0;
	case TAG_TEXTAREA:
		render_textarea(L, id);
		return 0;
	case TAG_FRAME:
		if ((v = doc_attr(d, id, ATTR_SRC)) != NULL) {
			block_break(L, 0);
			if (GROW(L, links, links_cap, L->p->nlinks + 1, 64) == 0) {
				struct llink *k = &L->p->links[L->p->nlinks++];

				k->node = id;
				k->kind = LK_HREF;
				k->pad = 0;
				k->line = L->p->nlines;
				k->col = (unsigned short)L->indent;
				L->link = (unsigned short)L->p->nlinks;
				set_span(L);
				put_text(L, "[frame: ");
				put_text(L, doc_attr(d, id, ATTR_NAME) ?
					doc_attr(d, id, ATTR_NAME) : v);
				put_text(L, "]");
				L->link = 0;
				set_span(L);
			}
			block_break(L, 0);
		}
		return 0;
	default:
		break;
	}
	if (n->type != NODE_ELEM || depth >= MAX_DEPTH)
		return 0;

	sv = &L->st[depth];
	sv->attr = (unsigned char)L->attr;
	sv->pre = (unsigned char)L->pre;
	sv->link = L->link;
	sv->indent = (short)L->indent;
	sv->tag = (unsigned char)tag;
	sv->list = 0;
	sv->link_mark = L->link_mark;
	sv->cell = (unsigned char)L->cell;
	sv->cell_mark = L->cell_mark;
	sv->display = s.display;
	sv->margin = s.margin;
	sv->face = (unsigned char)L->face;
	sv->nomarker = (unsigned char)L->nomarker;
	sv->fg = (unsigned char)L->fg;
	sv->align = (unsigned char)L->align;
	sv->fsz = (short)L->fsz;
	sv->lh = L->lh;
	sv->box = 0;
	sv->boxk = 0;
	/* list-style: inherited, the element's own style="" last */
	if (ea.style && css_inline_list(ea.style))
		cssf = (cssf & ~(CSS_NO_MARKER | CSS_MARKER)) | css_inline_list(ea.style);
	if (cssf & CSS_NO_MARKER)
		L->nomarker = 1;
	else if (cssf & CSS_MARKER)
		L->nomarker = 0;

	switch (s.display) {
	case D_BLOCK:
		/* a list item's first block (<li><p>): on the marker's line,
		 * no margin above; a list in it puts the marker alone */
		if (L->marker_w && !L->line_open && !L->cell
			&& tag != TAG_UL && tag != TAG_OL && tag != TAG_DL
			&& tag != TAG_MENU && tag != TAG_DIR)
			break;
		block_break(L, s.margin > 0 && L->nlists
			&& (tag == TAG_UL || tag == TAG_OL) ? 0 : s.margin);
		break;
	case D_LIST_ITEM:
		block_break(L, 0);
		make_marker(L, id);
		break;
	case D_TABLE_ROW:
		L->cell = 0;
		L->cell_break = 0;
		block_break(L, 0);
		break;
	case D_TABLE_CELL:
		if (L->line_open && L->col > L->line_indent)
			L->pend_space = 2;
		L->in_word = 0;
		L->cell = 1;
		L->cell_mark = L->p->text_len;
		L->cell_break = 0;
		break;
	default:
		break;
	}
	if (s.display == D_BLOCK && tag == TAG_TABLE)
		L->cell = 0;		/* a table in a cell: its own rows */
	if (boxed)
		box_open(L, sv, &bx, boxed == 1);
	/* (after the break: where the element's content will start) */
	if (ea.id || (tag == TAG_A && ea.name))
		add_anchor(L, id);
	if (tag == TAG_MAIN && L->p->main_line < 0)
		L->p->main_line = (long)here_line(L);
	if (tag == TAG_H1 || tag == TAG_H2 || tag == TAG_H3)
		L->heading_line = (long)here_line(L);
	if (tag == TAG_P) {
		sv->para_mark = L->p->text_len;
		sv->para_line = here_line(L);
	}
	L->indent += s.indent * L->em;
	if (s.pre)
		L->pre = 1;
	/* a heading's level replaces the one outside; the rest add up */
	if (s.face & LF_HMASK) {
		/* (as tall as the screen's heading fonts: 18, 14, 12) */
		static const short hsz[4] = { 120, 180, 140, 120 };
		int h = s.face & LF_HMASK;

		L->face = (L->face & ~LF_HMASK) | h;
		if (L->m && (L->face & LF_SMASK)) {
			/* in text the page sized: as much bigger than it */
			L->fsz = L->fsz * hsz[h] / 120;
			L->face = (L->face & ~LF_SMASK) | size_class(L->fsz);
		} else if (L->m)
			L->fsz = hsz[h];
	}
	L->face |= s.face & (LF_MONO | LF_ITALIC);
	if (tag == TAG_UL || tag == TAG_OL || tag == TAG_MENU || tag == TAG_DIR) {
		list_begin(L, id, tag);
		sv->list = 1;
	}
	L->attr |= s.attr;
	/* <font color=> (a sheet's colour, in text_look, beats it) */
	if (tag == TAG_FONT) {
		unsigned long rgb;

		if (css_attr_color(doc_attr(L->d, id, ATTR_COLOR), &rgb))
			L->fg = page_color(L->p, rgb);
	}
	text_look(L, tag, &s, &ct, ea.align);
	if (lh_new) {
		L->lh = bx.lh;
		if (bx.lh.u == CSS_U_AUTO)
			L->lh.u = CSS_U_NONE;	/* (normal: the font's) */
	}
	if (tag == TAG_A && ea.href
		&& GROW(L, links, links_cap, L->p->nlinks + 1, 64) == 0) {
		struct llink *k = &L->p->links[L->p->nlinks++];

		k->node = id;
		k->kind = LK_HREF;
		k->pad = 0;
		k->line = here_line(L);
		k->col = (unsigned short)L->col;
		L->link = (unsigned short)L->p->nlinks;
		L->link_mark = L->p->text_len;
	}
	if (tag == TAG_Q)
		put_text(L, "\"");
	if (tag == TAG_BUTTON) {
		if (!L->link)
			field_begin(L, id);
		L->attr |= SA_FIELD;
		set_span(L);
		put_text(L, "[");
	}
	set_span(L);
	return 1;
}

static void leave(struct lay *L, nodeid id, int depth)
{
	const struct doc *d = L->d;
	const struct saved *sv;
	struct style s;
	int tag = d->nodes[id].tag;

	if (depth >= MAX_DEPTH)
		return;
	sv = &L->st[depth];
	s.display = sv->display;
	s.margin = sv->margin;
	if (tag == TAG_Q)
		put_text(L, "\"");
	if (tag == TAG_BUTTON)
		put_text(L, "]");
	/* the first real paragraph (after <main>, if there is one): where
	 * the reading starts; its heading if that's just above */
	if (tag == TAG_P && L->p->content_line < 0
		&& L->p->text_len - sv->para_mark >= 80
		&& (L->p->main_line < 0 || (long)sv->para_line >= L->p->main_line)) {
		long ln = (long)sv->para_line;

		if (L->heading_line >= 0 && ln - L->heading_line <= 12
			&& L->heading_line >= L->p->main_line)
			ln = L->heading_line;
		L->p->content_line = ln;
	}
	if (tag == TAG_A && L->link && L->link != sv->link) {
		/* the link had no text: show something to select (not for
		 * the parser's empty copy of a link left open) */
		if (L->p->text_len == L->link_mark && (d->nodes[id].flags & NF_COPY)) {
			/* nothing to select either */
			if (L->p->nlinks == L->link)
				L->p->nlinks--;
		} else if (L->p->text_len == L->link_mark) {
			char name[40], buf[48];
			const char *h = doc_attr(d, id, ATTR_HREF);

			url_name(h ? h : "", name, sizeof name);
			snprintf(buf, sizeof buf, "[%s]", name);
			put_text(L, buf);
		}
	}
	/* a block's last line is its own: aligned as it was */
	if (L->align != sv->align && !L->cell && s.display != D_INLINE
		&& s.display != D_TABLE_CELL)
		end_line(L);
	L->attr = sv->attr;
	L->pre = sv->pre;
	L->face = sv->face;
	L->fsz = sv->fsz;
	L->lh = sv->lh;
	L->fg = sv->fg;
	L->align = sv->align;
	L->link = sv->link;
	L->link_mark = sv->link_mark;
	L->indent = sv->indent;
	L->cell = sv->cell;
	L->cell_mark = sv->cell_mark;
	L->nomarker = sv->nomarker;
	if (s.display == D_TABLE_CELL || tag == TAG_TABLE)
		L->cell_break = 0;
	if (sv->list && L->nlists)
		L->nlists--;
	if (sv->boxk)
		box_close(L, sv);
	switch (s.display) {
	case D_BLOCK:
		block_break(L, s.margin > 0 && L->nlists
			&& (tag == TAG_UL || tag == TAG_OL) ? 0 : s.margin);
		break;
	case D_LIST_ITEM:
	case D_TABLE_ROW:
		block_break(L, 0);
		break;
	case D_TABLE_CELL:
		L->in_word = 0;
		break;
	default:
		break;
	}
	if (s.display == D_LIST_ITEM)
		L->marker_w = 0;	/* an empty item: no marker left over */
	set_span(L);
}

/* the children of root (and theirs...), laid out from depth on */
static nodeid float_run(struct lay *L, nodeid id);

static void walk(struct lay *L, nodeid root, int depth)
{
	const struct doc *d = L->d;
	nodeid id = d->nodes[root].first, last;

	while (id && !L->stop) {
		const struct node *n = &d->nodes[id];
		int descend = 0;

		if (n->type == NODE_TEXT)
			put_text(L, doc_text(d, id));
		else if (n->type == NODE_ELEM && L->m && css_boxes
			&& (last = float_run(L, id)) != 0)
			id = last;		/* (floats: laid out as rows) */
		else if (n->type == NODE_ELEM)
			descend = enter(L, id, depth);
		if (L->stop)
			break;
		if (descend && n->first) {
			depth++;
			id = n->first;
			continue;
		}
		if (descend)
			leave(L, id, depth);
		while (id && !d->nodes[id].next) {
			id = d->nodes[id].parent;
			if (id == root || id <= 1) {
				id = 0;
				break;
			}
			depth--;
			leave(L, id, depth);
		}
		if (id)
			id = d->nodes[id].next;
	}
}

/* --- tables as grids --------------------------------------------------- */

/*
 * A table of data is laid out as a grid, its columns side by side; one
 * that frames a page (a sidebar beside the content, a table inside a
 * cell, big cells) stays linear, a row a line, as it reads better on a
 * narrow screen. Each cell is laid out on its own, as a block, into a
 * page of its own: once as wide as the table may be, which gives its
 * widest line and (from its words) its narrowest, then, if its column is
 * narrower, again at that width. The rows are then put together a line
 * at a time, each cell's line at its column; between columns, spaces on
 * a terminal, a spacer of the exact width with proportional fonts.
 */
#define GRID_MAX_COLS	32
#define GRID_MAX_CELLS	2048
#define GRID_CELL_TEXT	800		/* a cell with more text frames a page */
#define GRID_CELL_LINES	20		/* ... or more lines, or a table */
/* A table framing a page (a column of links beside the news, say) is laid
 * out as a grid only where there is room for it: at least FRAME_MIN_EM
 * wide (em: a terminal's column, or a "0" in a window), with its main
 * text at least FRAME_COL_EM and the other columns FRAME_SIDE_EM (or as
 * wide as they need). Narrower, its cells go one after another. */
#define FRAME_MIN_EM	96
#define FRAME_COL_EM	40
#define FRAME_SIDE_EM	12
#define FRAME_WORD_EM	30		/* a longer word (a URL) is broken there */
#define FRAME_MEASURE	40		/* lines a framing cell is measured by */
#define GRID_DEPTH	3		/* grids in grids' cells, so deep */

struct gcell {
	nodeid node;
	int r, c, rs, cs;		/* its row and column, and spans */
	int minw, maxw;
	int minw_cap;			/* minw, words no wider than FRAME_WORD_EM */
	int rule;			/* an <hr> in it: laid again at its width */
	int aligned;			/* lines to centre or put right: laid again */
	int at_width;			/* laid at its width (lines centred as asked) */
	int laid_w;			/* the width pg was laid out at */
	int frame;			/* it frames a page (see FRAME_MIN_EM) */
	struct page pg;
	int *lmap;			/* its links' numbers in the page, -1 */
	unsigned long bg;		/* its background (or its row's), LBOX_NONE */
	unsigned long box_i;		/* its page's boxes taken into the grid's */
};

struct grid {
	struct gcell *cell;
	int ncells, nrows, ncols;
	short *slot;			/* nrows x ncols: the cell there, -1 */
	int w[GRID_MAX_COLS], x[GRID_MAX_COLS];
	int *row_h, *row_at;		/* each row's lines, and its first */
	int frame;			/* a cell frames a page */
};

/* is element id not shown at all (hidden, display:none, a sheet)? */
static int el_hidden(struct lay *L, nodeid id)
{
	struct eattr ea;
	struct style s;

	scan_attrs(L->d, id, &ea);
	style_for(L->d->nodes[id].tag, ea.hidden, ea.style, ea.id, &s);
	if (s.display != D_NONE && L->d->sheet
		&& (css_display(L->d->sheet, L->d, id, ea.id, ea.cls, L->vw) & CSS_HIDDEN)
		&& !css_inline_shows(ea.style))
		s.display = D_NONE;
	return s.display == D_NONE;
}

/* the text bytes in the subtree of id; *table: 1 if a table is in it */
static long subtree_text(const struct doc *d, nodeid id, int *table)
{
	nodeid n = d->nodes[id].first;
	long total = 0;

	while (n) {
		const struct node *nd = &d->nodes[n];

		if (nd->type == NODE_TEXT)
			total += (long)strlen(doc_text(d, n));
		else if (nd->type == NODE_ELEM && nd->tag == TAG_TABLE)
			*table = 1;
		if (nd->type == NODE_ELEM && nd->first) {
			n = nd->first;
			continue;
		}
		while (n && !d->nodes[n].next) {
			n = d->nodes[n].parent;
			if (n == id)
				return total;
		}
		if (n)
			n = d->nodes[n].next;
	}
	return total;
}

static int attr_num(const struct doc *d, nodeid id, int attr, int lo, int hi)
{
	const char *v = doc_attr(d, id, attr);
	long n = 0;

	if (v == NULL)
		return lo;
	while (*v == ' ')
		v++;
	while (*v >= '0' && *v <= '9' && n < 100000)
		n = n * 10 + (*v++ - '0');
	return n < lo ? lo : n > hi ? hi : (int)n;
}

static void grid_free(struct grid *g);

/* room in *taken (rows of GRID_MAX_COLS: the cell there, -1) for need rows */
static int grow_taken(short **taken, int *rows_cap, int need)
{
	int nc, k;
	short *q;

	if (need <= *rows_cap)
		return 0;
	nc = need * 2 + 16;
	if ((q = xrealloc(*taken, (size_t)nc * GRID_MAX_COLS * sizeof *q)) == NULL)
		return -1;
	for (k = *rows_cap * GRID_MAX_COLS; k < nc * GRID_MAX_COLS; k++)
		q[k] = -1;
	*taken = q;
	*rows_cap = nc;
	return 0;
}

/* is there room here for a table framing a page (avail: its width)? */
static int frame_room(const struct lay *L, int avail)
{
	return avail >= FRAME_MIN_EM * L->em;
}

/* the table's cells, where they sit: 0, or -1 when it isn't a grid (avail:
 * the width it would have) */
/* does table id have a look of its own: a border, or a background on it,
 * a row or a cell (not those of a table inside)? In fonts such a table is
 * a grid even of one column: its box is drawn */
static int decorated(const struct doc *d, nodeid id)
{
	const char *b = doc_attr(d, id, ATTR_BORDER);
	nodeid n;

	if (b && (*b == '\0' || (*b >= '1' && *b <= '9')))
		return 1;
	for (n = id; n; ) {
		const struct node *nd = &d->nodes[n];

		if (nd->type == NODE_ELEM && (doc_attr(d, n, ATTR_BGCOLOR)
			|| (doc_attr(d, n, ATTR_STYLE)
			&& strstr(doc_attr(d, n, ATTR_STYLE), "background"))))
			return 1;
		/* on through the table, not into a table inside it */
		if (nd->first && (n == id || nd->tag != TAG_TABLE))
			n = nd->first;
		else {
			while (n != id && !d->nodes[n].next)
				n = d->nodes[n].parent;
			if (n == id)
				break;
			n = d->nodes[n].next;
		}
	}
	return 0;
}

static int grid_build(struct lay *L, nodeid table, struct grid *g, int avail)
{
	const struct doc *d = L->d;
	nodeid kids[256];
	int nkids = 0, i, r = 0, cap = 0, rows_cap = 0;
	nodeid n;
	short *taken = NULL;		/* rows x GRID_MAX_COLS while placing */

	memset(g, 0, sizeof *g);
	/* the rows: the table's own, and those of its sections */
	for (n = d->nodes[table].first; n && nkids < 256; n = d->nodes[n].next) {
		const struct node *nd = &d->nodes[n];

		if (nd->type != NODE_ELEM)
			continue;
		if (nd->tag == TAG_THEAD || nd->tag == TAG_TBODY || nd->tag == TAG_TFOOT) {
			nodeid m;

			if (el_hidden(L, n))
				continue;
			for (m = nd->first; m && nkids < 256; m = d->nodes[m].next)
				if (d->nodes[m].type == NODE_ELEM && d->nodes[m].tag == TAG_TR)
					kids[nkids++] = m;
		} else if (nd->tag == TAG_TR)
			kids[nkids++] = n;
	}
	if (nkids == 256 && n)
		return -1;			/* (a long table: as it was) */
	for (i = 0; i < nkids; i++) {
		nodeid tr = kids[i], c;
		int col = 0;

		if (el_hidden(L, tr))
			continue;
		if (grow_taken(&taken, &rows_cap, r + 1) < 0)
			goto linear;
		for (c = d->nodes[tr].first; c; c = d->nodes[c].next) {
			const struct node *cd = &d->nodes[c];
			struct gcell *gc;
			int rs, cs, k, j, nested = 0, frame = 0;
			long text;

			if (cd->type != NODE_ELEM || (cd->tag != TAG_TD && cd->tag != TAG_TH))
				continue;
			if (el_hidden(L, c))
				continue;
			/* a cell framing a page: a grid only where there is room */
			if ((text = subtree_text(d, c, &nested)) > GRID_CELL_TEXT || nested) {
				if (!frame_room(L, avail))
					goto linear;
				frame = 1;
			}
			cs = attr_num(d, c, ATTR_COLSPAN, 1, GRID_MAX_COLS);
			rs = attr_num(d, c, ATTR_ROWSPAN, 1, 64);
			if (grow_taken(&taken, &rows_cap, r + rs) < 0)
				goto linear;
			/* the next column free in this row (rows above may
			 * reach down into it) */
			while (col < GRID_MAX_COLS && taken[r * GRID_MAX_COLS + col] >= 0)
				col++;
			if (col + cs > GRID_MAX_COLS)
				goto linear;
			if (g->ncells == GRID_MAX_CELLS)
				goto linear;
			if (g->ncells == cap) {
				int nc = cap ? cap * 2 : 32;
				struct gcell *q = xrealloc(g->cell, (size_t)nc * sizeof *q);

				if (q == NULL)
					goto linear;
				g->cell = q;
				cap = nc;
			}
			gc = &g->cell[g->ncells];
			memset(gc, 0, sizeof *gc);
			gc->node = c;
			gc->frame = frame;
			if (frame)
				g->frame = 1;
			gc->r = r;
			gc->c = col;
			gc->rs = rs;
			gc->cs = cs;
			for (k = r; k < r + rs; k++)
				for (j = col; j < col + cs; j++)
					taken[k * GRID_MAX_COLS + j] = (short)g->ncells;
			g->ncells++;
			col += cs;
			if (col > g->ncols)
				g->ncols = col;
		}
		r++;
	}
	g->nrows = r;
	for (i = 0; i < g->ncells; i++)	/* (a rowspan past the last row) */
		if (g->cell[i].r + g->cell[i].rs > g->nrows)
			g->cell[i].rs = g->nrows - g->cell[i].r;
	if ((g->ncols < 2 && !(g->ncols == 1 && L->m && decorated(L->d, table)))
		|| g->nrows == 0)
		goto linear;
	g->slot = xmalloc((size_t)g->nrows * (size_t)g->ncols * sizeof *g->slot);
	g->row_h = xmalloc((size_t)g->nrows * sizeof *g->row_h);
	g->row_at = xmalloc(((size_t)g->nrows + 1) * sizeof *g->row_at);
	if (!g->slot || !g->row_h || !g->row_at)
		goto linear;
	for (r = 0; r < g->nrows; r++)
		for (i = 0; i < g->ncols; i++)
			g->slot[r * g->ncols + i] = taken[r * GRID_MAX_COLS + i];
	xfree(taken);
	return 0;
linear:
	xfree(taken);
	grid_free(g);
	return -1;
}

static void grid_free(struct grid *g)
{
	int i;

	for (i = 0; i < g->ncells; i++) {
		layout_free(&g->cell[i].pg);
		xfree(g->cell[i].lmap);
	}
	xfree(g->cell);
	xfree(g->slot);
	xfree(g->row_h);
	xfree(g->row_at);
	memset(g, 0, sizeof *g);
}

/* how wide n bytes of page tp are in a span's look */
static int seg_w(const struct lay *L, const struct page *tp, int attr, int face,
	const char *s, int n)
{
	int w = 0;

	if (face & LF_IMAGE)
		return layout_images_w(tp, s, n);
	if (L->m)
		return L->m->width(L->m->ctx, attr, face, s, n);
	if (tp->cs != TCS_UTF8)
		return n;
	{
		const char *e = s + n;

		while (s < e) {
			if ((unsigned char)*s < 0x80) {
				s++;
				w++;
			} else
				w += ucs_width(utf8_get(&s));
		}
	}
	return w;
}

/*
 * Line ln of page tp: its width (indent included) into *w, and its widest
 * piece that can't be broken (a word, a field, a picture) into *word.
 */
static void line_widths(const struct lay *L, const struct page *tp, unsigned long ln,
	int *w, int *word)
{
	const struct lline *l = &tp->lines[ln];
	unsigned long off = l->off, end = l->off + l->len, s = l->span;
	int run = 0, reach = 0;

	*w = l->indent;
	*word = 0;
	while (off < end) {
		unsigned long next = end, a;
		const struct lspan *sp;

		s = layout_span_at(tp, s, off);
		sp = &tp->spans[s];
		if (s + 1 < tp->nspans && tp->spans[s + 1].off < end)
			next = tp->spans[s + 1].off;
		if (sp->attr & SA_RULE) {	/* (as wide as it is let be) */
			off = next;
			continue;
		}
		*w += seg_w(L, tp, sp->attr, sp->face, tp->text + off, (int)(next - off));
		if (*w > reach)
			reach = *w;
		if ((sp->attr & SA_FIELD) || (sp->face & LF_IMAGE))
			run += seg_w(L, tp, sp->attr, sp->face, tp->text + off,
				(int)(next - off));
		else
			for (a = off; a < next; ) {
				unsigned long b = a;

				while (b < next && tp->text[b] != ' ')
					b++;
				run += seg_w(L, tp, sp->attr, sp->face, tp->text + a, (int)(b - a));
				if (b < next) {	/* a space: the word ends */
					if (l->indent + run > *word)
						*word = l->indent + run;
					run = 0;
					b++;
				}
				a = b;
			}
		off = next;
	}
	if (l->indent + run > *word)
		*word = l->indent + run;
	if (reach > *w)
		*w = reach;		/* (back from a float's line) */
}

/* lay cell gc out into its page, width wide, as a block's content, with
 * S (a struct lay of the grid's, reused for each cell); max_lines: stop
 * there (0: all of it) */
static int lay_cell(struct lay *L, struct lay *S, struct gcell *gc, int width,
	unsigned long max_lines, int at_width)
{
	struct page *pg = &gc->pg;
	size_t have = used(L->p), room = L->p->byte_cap > have ? L->p->byte_cap - have : 0;
	struct style s;

	layout_free(pg);
	pg->d = L->d;
	pg->width = width;
	pg->cs = L->p->cs;
	pg->byte_cap = room;
	pg->main_line = -1;
	pg->content_line = -1;
	gc->laid_w = width;
	gc->at_width = at_width;
	gc->aligned = 0;
	if (room < 4096)
		return -1;
	/* (all but the saved-state stack, written before it's read: on a
	 * 68030 clearing it for every cell costs) */
	memset(S, 0, offsetof(struct lay, st));
	S->small = 1;
	S->gdepth = L->gdepth + 1;
	S->max_lines = max_lines;
	S->at_width = at_width;
	S->p = pg;
	S->d = L->d;
	S->fs = L->fs;
	S->m = L->m;
	S->em = L->em;
	S->fsz = L->fsz;
	S->lh = L->lh;
	S->width = width;
	S->base_w = width;
	S->vw = L->vw;
	S->face = L->face;
	S->nomarker = L->nomarker;
	S->heading_line = -1;
	style_of(L->d, gc->node, &s);	/* (a <th>: bold) */
	S->attr = L->attr | s.attr;
	/* (a float's own look: a heading's face, italics...; in fonts a
	 * heading is its size, not an underline) */
	S->face |= s.face & (LF_MONO | LF_ITALIC);
	/* (quirks-style, as the pages this matters for are: a table doesn't
	 * take the alignment around it - a cell's is its own, or its row's) */
	S->align = 0;
	if (L->fg && L->p->palette)
		S->fg = page_color(pg, L->p->palette[L->fg - 1]);
	{
		/* the cell's own look: its sheet, style="", align= (or its row's) */
		struct css_text ct;
		const char *st = doc_attr(L->d, gc->node, ATTR_STYLE);
		const char *al = doc_attr(L->d, gc->node, ATTR_ALIGN);
		nodeid row = L->d->nodes[gc->node].parent;

		memset(&ct, 0, sizeof ct);
		if (L->d->sheet)
			css_style(L->d->sheet, L->d, gc->node, doc_attr(L->d, gc->node, ATTR_ID),
				doc_attr(L->d, gc->node, ATTR_CLASS), L->vw, &ct);
		if (st)
			css_inline_text(L->d->sheet, st, &ct);
		s.display = D_BLOCK;		/* (its lines are its own) */
		if (L->m && (s.face & LF_HMASK)) {
			/* a heading's size, unless the sheet says one (an em
			 * of its own is of the size around it) */
			static const short hsz[4] = { 120, 180, 140, 120 };

			S->face = (S->face & ~LF_HMASK) | (s.face & LF_HMASK);
			if (ct.fsize.u == CSS_U_NONE)
				S->fsz = hsz[s.face & LF_HMASK];
		}
		if (al == NULL && row && L->d->nodes[row].tag == TAG_TR)
			al = doc_attr(L->d, row, ATTR_ALIGN);
		text_look(S, L->d->nodes[gc->node].tag, &s, &ct, al);
	}
	set_span(S);
	if (doc_attr(L->d, gc->node, ATTR_ID))
		add_anchor(S, gc->node);	/* (the cell's own id) */
	walk(S, gc->node, 0);
	end_line(S);
	float_flush(S);
	/* (no blank lines at its end; a box's spacer stays: its height is
	 * the box's, and its ascent 0) */
	while (pg->nlines && pg->lines[pg->nlines - 1].len == 0
		&& !(pg->heights && pg->heights[pg->nlines - 1].ascent == 0))
		pg->nlines--;
	gc->aligned = S->aligned;
	return pg->truncated ? -1 : 0;
}

/* the cells' widths, the columns' (or -1: they don't fit) */
static int grid_widths(struct lay *L, struct lay *S, struct grid *g, int avail, int gap)
{
	int cmin[GRID_MAX_COLS], cmax[GRID_MAX_COLS], i, c, smin = 0, smax = 0;

	for (c = 0; c < g->ncols; c++)
		cmin[c] = cmax[c] = 0;
	/* single columns first, then what spans need beyond them */
	for (i = 0; i < g->ncells; i++) {
		struct gcell *gc = &g->cell[i];
		unsigned long ln;

		/* (a cell framing a page is measured by its first lines: the
		 * widest is all of avail, the longest word no wider than
		 * FRAME_WORD_EM; laying all of it out twice costs a 68030) */
		if (lay_cell(L, S, gc, avail, gc->frame ? FRAME_MEASURE : 0, 0) < 0)
			return -1;
		if (gc->pg.nlines > GRID_CELL_LINES) {
			if (!frame_room(L, avail))
				return -1;	/* (a page's frame, no room for it) */
			gc->frame = g->frame = 1;
		}
		gc->minw = gc->maxw = gc->minw_cap = gc->rule = 0;
		for (ln = 0; ln < gc->pg.nspans; ln++)
			if (gc->pg.spans[ln].attr & SA_RULE)
				gc->rule = 1;
		if (gc->pg.need_w > gc->maxw)
			gc->maxw = gc->pg.need_w;
		for (ln = 0; ln < gc->pg.nlines; ln++) {
			int w, word;

			line_widths(L, &gc->pg, ln, &w, &word);
			if (w > gc->maxw)
				gc->maxw = w;
			if (word > gc->minw)
				gc->minw = word;
			if (word > FRAME_WORD_EM * L->em)
				word = FRAME_WORD_EM * L->em;
			if (word > gc->minw_cap)
				gc->minw_cap = word;
		}
	}
	/* (a page's frame breaks its longest words rather than not fit:
	 * the layout breaks a word longer than its line) */
	for (i = 0; i < g->ncells; i++) {
		struct gcell *gc = &g->cell[i];

		if (g->frame)
			gc->minw = gc->minw_cap;
		if (gc->cs == 1) {
			if (gc->minw > cmin[gc->c])
				cmin[gc->c] = gc->minw;
			if (gc->maxw > cmax[gc->c])
				cmax[gc->c] = gc->maxw;
		}
	}
	for (i = 0; i < g->ncells; i++) {
		struct gcell *gc = &g->cell[i];
		int have_min = (gc->cs - 1) * gap, have_max = have_min, extra;

		if (gc->cs == 1)
			continue;
		for (c = gc->c; c < gc->c + gc->cs; c++) {
			have_min += cmin[c];
			have_max += cmax[c];
		}
		if ((extra = gc->minw - have_min) > 0)
			for (c = gc->c; c < gc->c + gc->cs; c++)
				cmin[c] += extra / gc->cs + (c - gc->c < extra % gc->cs);
		if ((extra = gc->maxw - have_max) > 0)
			for (c = gc->c; c < gc->c + gc->cs; c++)
				cmax[c] += extra / gc->cs + (c - gc->c < extra % gc->cs);
	}
	for (c = 0; c < g->ncols; c++) {
		if (cmax[c] < cmin[c])
			cmax[c] = cmin[c];
		smin += cmin[c];
		smax += cmax[c];
	}
	avail -= (g->ncols - 1) * gap;
	if (smin > avail)
		return -1;
	/* all at their widest if they fit; else each its narrowest and a
	 * share of what is left, as much as it would take more */
	for (c = 0; c < g->ncols; c++)
		g->w[c] = smax <= avail ? cmax[c] : cmin[c] + (smax > smin ?
			(int)((long)(cmax[c] - cmin[c]) * (avail - smin) / (smax - smin)) : 0);
	/* a page's frame: its narrow columns (a sidebar of links, gutters) at
	 * their widest, the others sharing the rest so */
	if (g->frame && smax > avail) {
		int side = FRAME_SIDE_EM * L->em, fixed = 0, rmin = 0, rmax = 0;

		for (c = 0; c < g->ncols; c++)
			if (cmax[c] <= side)
				fixed += cmax[c];
			else {
				rmin += cmin[c];
				rmax += cmax[c];
			}
		if (rmax > rmin && fixed + rmin <= avail)
			for (c = 0; c < g->ncols; c++)
				g->w[c] = cmax[c] <= side ? cmax[c] : cmin[c]
					+ (int)((long)(cmax[c] - cmin[c])
					* (avail - fixed - rmin) / (rmax - rmin));
	}
	for (c = 0; c < g->ncols; c++)
		g->x[c] = c ? g->x[c - 1] + g->w[c - 1]
			+ (g->w[c - 1] ? gap : 0) : 0;	/* (none after nothing) */
	/* a page's frame only if it reads well: the framing cell wanting
	 * the most room (the page's main text, beside its sidebars) wide
	 * enough or as wide as it would be, no column squeezed */
	if (g->frame) {
		const struct gcell *main = NULL;
		int w;

		for (i = 0; i < g->ncells; i++)
			if (g->cell[i].frame && (!main || g->cell[i].maxw > main->maxw))
				main = &g->cell[i];
		w = g->x[main->c + main->cs - 1] + g->w[main->c + main->cs - 1]
			- g->x[main->c];
		if (w < FRAME_COL_EM * L->em && w < main->maxw)
			return -1;
		for (c = 0; c < g->ncols; c++)	/* (wider ones than the side's) */
			if (g->w[c] < FRAME_SIDE_EM * L->em && cmax[c] > FRAME_SIDE_EM * L->em)
				return -1;
	}
	return 0;
}

/* the gap up to x: spaces, or with proportional fonts a spacer that wide */
static void pad_to(struct lay *L, int x)
{
	int n = x - L->col;

	if (n <= 0)
		return;
	L->attr = 0;
	L->link = 0;
	L->fg = 0;
	L->asc = 0;
	if (L->m) {
		char b[LAYOUT_IMG_BYTES];

		L->face = LF_IMAGE;
		set_span(L);
		b[0] = '\002';
		b[1] = (char)(2 + n / 900 % 30);
		b[2] = (char)(2 + n / 30 % 30);
		b[3] = (char)(2 + n % 30);
		put_bytes(L, b, sizeof b);
	} else {
		static const char sp[] = "                ";

		L->face = 0;
		set_span(L);
		while (n > 0) {
			int k = n > 16 ? 16 : n;

			put_bytes(L, sp, (size_t)k);
			n -= k;
		}
	}
	L->col = x;
}

/* to x on the open line, forward or back (with metrics: a spacer that
 * goes back, so that a float's line and the text beside it share a line) */
static void go_to(struct lay *L, int x)
{
	int n = L->col - x;
	int a = L->attr, f = L->face, fg = L->fg;
	unsigned short link = L->link;

	if (n <= 0 || L->m == NULL) {
		pad_to(L, x);
	} else {
		char b[LAYOUT_IMG_BYTES];

		L->attr = 0;
		L->link = 0;
		L->fg = 0;
		L->asc = 0;
		L->face = LF_IMAGE;
		set_span(L);
		b[0] = '\003';
		b[1] = (char)(2 + n / 900 % 30);
		b[2] = (char)(2 + n / 30 % 30);
		b[3] = (char)(2 + n % 30);
		put_bytes(L, b, sizeof b);
		L->col = x;
	}
	L->attr = a;
	L->face = f;
	L->fg = fg;
	L->link = link;
	set_span(L);
}

/* line ln of cell gc's page onto the open line; dy >= 0 (with metrics):
 * the line's top is that far below the open line's (a band of a grid),
 * so its text keeps its own baseline; ondark: on a dark background */
static void put_cell_line(struct lay *L, struct gcell *gc, unsigned long ln,
	long dy, int ondark)
{
	const struct page *tp = &gc->pg;
	const struct lline *l = &tp->lines[ln];
	unsigned long off = l->off, end = l->off + l->len, s = l->span;
	struct page *p = L->p;

	while (off < end && !L->stop) {
		unsigned long next = end;
		const struct lspan *sp;
		int w;

		s = layout_span_at(tp, s, off);
		sp = &tp->spans[s];
		if (s + 1 < tp->nspans && tp->spans[s + 1].off < end)
			next = tp->spans[s + 1].off;
		w = seg_w(L, tp, sp->attr, sp->face, tp->text + off, (int)(next - off));
		L->attr = sp->attr | (ondark ? SA_ONDARK : 0);
		L->face = sp->face;
		L->asc = 0;
		if (dy >= 0 && L->m && tp->heights) {
			long a = dy + (sp->asc ? sp->asc : tp->heights[ln].ascent);

			L->asc = (unsigned short)(a > 0xFFFF ? 0xFFFF : a);
		}
		L->fg = sp->color && tp->palette ? page_color(p, tp->palette[sp->color - 1]) : 0;
		L->link = 0;
		if (sp->link && gc->lmap) {
			int k = sp->link - 1;

			if (gc->lmap[k] < 0 && GROW(L, links, links_cap, p->nlinks + 1, 64) == 0) {
				struct llink *nk = &p->links[p->nlinks];

				*nk = tp->links[k];
				nk->line = p->nlines;
				nk->col = (unsigned short)L->col;
				gc->lmap[k] = (int)p->nlinks++;
			}
			if (gc->lmap[k] >= 0)
				L->link = (unsigned short)(gc->lmap[k] + 1);
		}
		set_span(L);
		if (sp->face & LF_IMAGE) {
			/* pictures get their number in the page; spacers as
			 * they are */
			unsigned long a;

			for (a = off; a + LAYOUT_IMG_BYTES <= next; a += LAYOUT_IMG_BYTES) {
				char b[LAYOUT_IMG_BYTES];
				long k = layout_image(tp, tp->text + a);

				memcpy(b, tp->text + a, sizeof b);
				if (k >= 0) {
					unsigned long ni;

					if (p->nimages >= LAYOUT_MAX_IMAGES
						|| GROW(L, images, images_cap, p->nimages + 1, 16) < 0)
						continue;
					ni = p->nimages++;
					p->images[ni] = tp->images[k];
					p->images[ni].line = p->nlines;
					b[1] = (char)(2 + ni / 900);
					b[2] = (char)(2 + ni / 30 % 30);
					b[3] = (char)(2 + ni % 30);
				}
				put_bytes(L, b, sizeof b);
			}
		} else
			put_bytes(L, tp->text + off, (size_t)(next - off));
		L->col += w;
		off = next;
	}
	L->asc = 0;
}

/* --- grids in fonts: bands across them, boxes under them ------------------ */

/* a table's look, with metrics only (a terminal shows none of it) */
struct gdecor {
	int border;			/* the table's border's width, 0: none */
	int spacing, padding;		/* cellspacing=, cellpadding= */
	int hin;			/* a cell's box reaches this far beside its text */
	int margin;			/* the cells' text this far in from the table's box */
	unsigned long bg, border_rgb;	/* the table's; LBOX_NONE: none */
};

#define BORDER_GREY	0x808080UL	/* (browsers' raised and sunken greys) */

/* attribute a of id in whole units (pixels): dflt if not said, or a % */
static int attr_units(const struct doc *d, nodeid id, int a, int dflt)
{
	const char *v = doc_attr(d, id, a);
	long n = 0;

	if (v == NULL)
		return dflt;
	while (*v == ' ')
		v++;
	if (*v < '0' || *v > '9')
		return dflt;
	while (*v >= '0' && *v <= '9' && n < 100000)
		n = n * 10 + (*v++ - '0');
	return *v == '%' ? dflt : (int)n;
}

/* a style=""'s background colour, or LBOX_NONE */
static unsigned long style_bg(const char *st)
{
	struct css_text ct;

	if (st == NULL)
		return LBOX_NONE;
	memset(&ct, 0, sizeof ct);
	css_inline_text(NULL, st, &ct);
	return ct.bg & CSS_RGB_SET ? ct.bg & 0xFFFFFFUL : LBOX_NONE;
}

/* a cell's background: its own (bgcolor=, style=""), else its row's */
static unsigned long cell_bg(const struct doc *d, nodeid cell)
{
	nodeid row = d->nodes[cell].parent;
	unsigned long rgb;

	if (css_attr_color(doc_attr(d, cell, ATTR_BGCOLOR), &rgb))
		return rgb;
	if ((rgb = style_bg(doc_attr(d, cell, ATTR_STYLE))) != LBOX_NONE)
		return rgb;
	if (row && d->nodes[row].tag == TAG_TR) {
		if (css_attr_color(doc_attr(d, row, ATTR_BGCOLOR), &rgb))
			return rgb;
		return style_bg(doc_attr(d, row, ATTR_STYLE));
	}
	return LBOX_NONE;
}

/* too dark for the screen's darkened text colours to be read on */
static int dark(unsigned long rgb)
{
	return rgb != LBOX_NONE && ((rgb >> 16 & 255) * 299 + (rgb >> 8 & 255) * 587
		+ (rgb & 255) * 114) / 1000 < 110;
}

/* table id's look into *dc, its cells' backgrounds into its cells */
static void grid_decor(struct lay *L, nodeid id, struct grid *g, int gap,
	struct gdecor *dc)
{
	const struct doc *d = L->d;
	const char *b = doc_attr(d, id, ATTR_BORDER);
	unsigned long rgb;
	int i, any = 0;

	memset(dc, 0, sizeof *dc);
	dc->bg = dc->border_rgb = LBOX_NONE;
	for (i = 0; i < g->ncells; i++)
		g->cell[i].bg = LBOX_NONE;
	if (L->m == NULL)
		return;
	dc->border = b == NULL ? 0 : *b == '\0' ? 1 : attr_units(d, id, ATTR_BORDER, 1);
	if (dc->border > 8)
		dc->border = 8;
	dc->spacing = attr_units(d, id, ATTR_CELLSPACING, 2);
	dc->padding = attr_units(d, id, ATTR_CELLPADDING, 1);
	if (dc->spacing > 40)
		dc->spacing = 40;
	if (dc->padding > 40)
		dc->padding = 40;
	if (css_attr_color(doc_attr(d, id, ATTR_BGCOLOR), &rgb))
		dc->bg = rgb;
	else
		dc->bg = style_bg(doc_attr(d, id, ATTR_STYLE));
	dc->border_rgb = css_attr_color(doc_attr(d, id, ATTR_BORDERCOLOR), &rgb)
		? rgb : BORDER_GREY;
	any = dc->bg != LBOX_NONE;
	for (i = 0; i < g->ncells; i++)
		if ((g->cell[i].bg = cell_bg(d, g->cell[i].node)) != LBOX_NONE)
			any = 1;
	/* (a box for each cell: room for it either side of the text) */
	if (dc->border || any) {
		dc->hin = gap > dc->spacing ? (gap - dc->spacing) / 2 : 0;
		dc->margin = dc->hin + dc->border;
	}
}

static void add_box(struct lay *L, unsigned long line, long dy, int x, int w,
	long h, unsigned long bg, int border, unsigned long border_rgb)
{
	struct page *p = L->p;
	struct lbox *b;

	if (w <= 0 || h <= 0 || (bg == LBOX_NONE && border == 0)
		|| GROW(L, boxes, boxes_cap, p->nboxes + 1, 16) < 0)
		return;
	b = &p->boxes[p->nboxes++];
	b->line = line;
	b->dy = dy;
	b->x = x;
	b->w = w;
	b->h = h;
	b->bg = bg;
	b->border = (unsigned char)border;
	b->border_rgb = border_rgb;
}

/* a cell's line in a grid in fonts: where it starts */
struct bev {
	long y;				/* its top, below the grid's */
	int c, cell;
	unsigned long ln;
};

static int bev_cmp(const void *a, const void *b)
{
	const struct bev *p = a, *q = b;

	if (p->y != q->y)
		return p->y < q->y ? -1 : 1;
	return p->c - q->c;
}

/* a cell's valign= (else its row's): 0 top, 1 middle (as browsers, when
 * neither says), 2 bottom */
static int cell_valign(const struct doc *d, nodeid cell)
{
	const char *v = doc_attr(d, cell, ATTR_VALIGN);
	nodeid row = d->nodes[cell].parent;

	if (v == NULL && row && d->nodes[row].tag == TAG_TR)
		v = doc_attr(d, row, ATTR_VALIGN);
	if (v == NULL)
		return 1;
	if ((v[0] | 32) == 't' || (v[0] | 32) == 'b') {
		/* top, baseline: at the top; bottom */
		return (v[0] | 32) == 'b' && (v[1] | 32) == 'o' ? 2 : 0;
	}
	return 1;
}

/*
 * With metrics: grid g as bands. Each cell's lines keep their own heights,
 * one under the other, whatever the cells beside hold (a tall picture in
 * one no longer opens gaps in the rest). A band - a line of the page - runs
 * from where some cell's line starts to where the next one does; each
 * cell's line goes in the band it starts in, at its own baseline (struct
 * lspan's asc), reaching down into the bands below where it is taller
 * (struct lheight's over). A row is as tall as its tallest cell, the
 * others' text at its top, middle (as browsers, unless valign= says) or
 * bottom; cellspacing and cellpadding between and around; the table's and
 * the cells' backgrounds and borders are boxes under it all. 0, or -1
 * (out of memory: nothing done).
 */
static int compose_bands(struct lay *L, struct grid *g, const struct gdecor *dc,
	int shift, int attr, int face, unsigned short link)
{
	struct page *p = L->p;
	int cb = dc->border ? 1 : 0, vpad = cb + dc->padding, i, r, nb = 0, x0 = 0;
	long *rh, *ra, *cy, *band, y, H, over;
	unsigned long *bl, *first, nev = 0, e, k, base = 0, j;
	struct bev *ev;
	int gw = g->x[g->ncols - 1] + g->w[g->ncols - 1];

	for (i = 0; i < g->ncells; i++)
		nev += g->cell[i].pg.heights ? g->cell[i].pg.nlines : 0;
	rh = xmalloc((size_t)g->nrows * sizeof *rh);
	ra = xmalloc(((size_t)g->nrows + 1) * sizeof *ra);
	cy = xmalloc((size_t)g->ncells * sizeof *cy);
	first = xmalloc(((size_t)g->ncells + 1) * sizeof *first);
	ev = xmalloc((nev + 1) * sizeof *ev);
	band = xmalloc((nev + 1) * sizeof *band);
	bl = xmalloc((nev + 1) * sizeof *bl);
	if (!rh || !ra || !cy || !first || !ev || !band || !bl) {
		xfree(rh);
		xfree(ra);
		xfree(cy);
		xfree(first);
		xfree(ev);
		xfree(band);
		xfree(bl);
		return -1;
	}

	/* the rows' heights: their tallest cells, padded (a cell spanning
	 * rows lengthens the last of them if it must) */
	for (r = 0; r < g->nrows; r++)
		rh[r] = 0;
	for (i = 0; i < g->ncells; i++) {
		const struct gcell *gc = &g->cell[i];
		long need = 2L * vpad, have;
		nodeid row = L->d->nodes[gc->node].parent;
		int ha = attr_units(L->d, gc->node, ATTR_HEIGHT, 0);

		if (row && L->d->nodes[row].tag == TAG_TR
			&& attr_units(L->d, row, ATTR_HEIGHT, 0) > ha)
			ha = attr_units(L->d, row, ATTR_HEIGHT, 0);
		if (gc->pg.heights)
			for (j = 0; j < gc->pg.nlines; j++)
				need += gc->pg.heights[j].height;
		if (ha > need && ha < 8192)
			need = ha;
		if (gc->rs == 1) {
			if (need > rh[gc->r])
				rh[gc->r] = need;
			continue;
		}
		for (have = 0, r = gc->r; r < gc->r + gc->rs; r++)
			have += rh[r] + (r > gc->r ? dc->spacing : 0);
		if (need > have)
			rh[gc->r + gc->rs - 1] += need - have;
	}
	y = dc->border + dc->spacing;
	for (r = 0; r < g->nrows; r++) {
		ra[r] = y;
		y += rh[r] + dc->spacing;
	}
	ra[g->nrows] = y;
	H = y + dc->border;

	/* each cell's text: where it starts; then its lines, all in order */
	for (i = 0, e = 0; i < g->ncells; i++) {
		const struct gcell *gc = &g->cell[i];
		int last = gc->r + gc->rs - 1;
		long box_h = ra[last] + rh[last] - ra[gc->r], ch = 0, room;

		first[i] = e;
		if (gc->pg.heights)
			for (j = 0; j < gc->pg.nlines; j++)
				ch += gc->pg.heights[j].height;
		room = box_h - 2L * vpad - ch;
		if (room < 0)
			room = 0;
		cy[i] = ra[gc->r] + vpad;
		switch (cell_valign(L->d, gc->node)) {
		case 1:
			cy[i] += room / 2;
			break;
		case 2:
			cy[i] += room;
			break;
		}
		y = cy[i];
		if (gc->pg.heights)
			for (j = 0; j < gc->pg.nlines; j++, e++) {
				ev[e].y = y;
				ev[e].c = gc->c;
				ev[e].cell = i;
				ev[e].ln = j;
				bl[e] = (unsigned long)-1;
				y += gc->pg.heights[j].height;
			}
	}
	first[g->ncells] = e;
	qsort(ev, (size_t)nev, sizeof *ev, bev_cmp);
	/* the bands: the first from the grid's top; the next wherever a
	 * line starts that the band before doesn't hold already */
	for (e = 0; e < nev; e++)
		if (nb == 0)
			band[nb++] = 0;
		else if (ev[e].y != ev[e - 1].y && ev[e].y > band[nb - 1])
			band[nb++] = ev[e].y;
	if (nb == 0 && H > 0)
		band[nb++] = 0;

	for (k = 0, e = 0; k < (unsigned long)nb && !L->stop; k++) {
		long bt = band[k], bb = k + 1 < (unsigned long)nb ? band[k + 1] : H;

		open_line(L);
		if (k == 0) {
			/* the boxes under the grid: the table's, the cells' */
			base = p->nlines;
			x0 = L->col + shift + dc->margin;
			add_box(L, base, 0, x0 - dc->margin, gw + 2 * dc->margin, H,
				dc->bg, dc->border, dc->border_rgb);
			for (i = 0; i < g->ncells; i++) {
				const struct gcell *gc = &g->cell[i];
				int last = gc->r + gc->rs - 1;
				int w = g->x[gc->c + gc->cs - 1] + g->w[gc->c + gc->cs - 1]
					- g->x[gc->c];

				add_box(L, base, ra[gc->r], x0 + g->x[gc->c] - dc->hin,
					w + 2 * dc->hin, ra[last] + rh[last] - ra[gc->r],
					gc->bg, cb, dc->border_rgb);
			}
		}
		over = 0;
		for (; e < nev && (k + 1 >= (unsigned long)nb || ev[e].y < band[k + 1]); e++) {
			struct gcell *gc = &g->cell[ev[e].cell];
			unsigned long ln = ev[e].ln;
			long dy = ev[e].y - bt, ext;

			pad_to(L, x0 + g->x[gc->c] + gc->pg.lines[ln].indent);
			bl[first[ev[e].cell] + ln] = p->nlines;
			put_cell_line(L, gc, ln, dy,
				gc->bg != LBOX_NONE ? dark(gc->bg) : dark(dc->bg));
			ext = dy + gc->pg.heights[ln].height + gc->pg.heights[ln].over
				- (bb - bt);
			if (ext > over)
				over = ext;
			/* boxes in the cell (a table in it): into the grid's */
			for (; gc->box_i < gc->pg.nboxes
				&& gc->pg.boxes[gc->box_i].line <= ln; gc->box_i++) {
				const struct lbox *b = &gc->pg.boxes[gc->box_i];

				if (b->line == ln)
					add_box(L, p->nlines, b->dy + dy,
						x0 + g->x[gc->c] + b->x, b->w, b->h,
						b->bg, b->border, b->border_rgb);
			}
		}
		L->attr = attr;
		L->face = face;
		L->link = link;
		L->asc = 0;
		set_span(L);
		L->line_a = (int)(bb - bt);
		L->line_d = 0;
		end_line(L);
		if (p->heights && p->nlines) {
			if (over > 0xFFFF)
				over = 0xFFFF;
			p->heights[p->nlines - 1].over = (unsigned short)over;
			if (over > p->max_over)
				p->max_over = (int)over;
		}
	}

	/* what the cells knew of the page: anchors, where content starts */
	for (i = 0; i < g->ncells && nb > 0; i++) {
		const struct page *tp = &g->cell[i].pg;
		unsigned long n = first[i + 1] - first[i];

		for (j = 0; j < tp->nanchors; j++)
			if (tp->anchors[j].line < n && bl[first[i] + tp->anchors[j].line]
				!= (unsigned long)-1
				&& GROW(L, anchors, anchors_cap, p->nanchors + 1, 64) == 0) {
				p->anchors[p->nanchors].node = tp->anchors[j].node;
				p->anchors[p->nanchors].line = bl[first[i] + tp->anchors[j].line];
				p->nanchors++;
			}
		if (p->main_line < 0 && tp->main_line >= 0 && (unsigned long)tp->main_line < n
			&& bl[first[i] + (unsigned long)tp->main_line] != (unsigned long)-1)
			p->main_line = (long)bl[first[i] + (unsigned long)tp->main_line];
		if (p->content_line < 0 && tp->content_line >= 0
			&& (unsigned long)tp->content_line < n
			&& bl[first[i] + (unsigned long)tp->content_line] != (unsigned long)-1)
			p->content_line = (long)bl[first[i] + (unsigned long)tp->content_line];
	}
	xfree(rh);
	xfree(ra);
	xfree(cy);
	xfree(first);
	xfree(ev);
	xfree(band);
	xfree(bl);
	return 0;
}

/* lay table id out as a grid: 1, or 0 when it is to be laid out as rows */
static int grid_table(struct lay *L, nodeid id)
{
	struct grid g;
	struct gdecor dc;
	struct lay *S;
	int gap = L->m ? L->em : 2, avail, i, c, total, attr = L->attr, face = L->face;
	int cell = L->cell, was_align = L->align, align = L->align, shift = 0, gw;
	const char *ta;
	unsigned short link = L->link;
	unsigned long base = 0, ln;
	nodeid n;

	if (L->pre || L->marker_w || L->gdepth >= GRID_DEPTH)
		return 0;
	float_flush(L);
	avail = L->width - (L->indent > L->width / 2 ? L->width / 2 : L->indent);
	if (grid_build(L, id, &g, avail) < 0)
		return 0;
	if ((S = xmalloc(sizeof *S)) == NULL) {
		grid_free(&g);
		return 0;
	}
	grid_decor(L, id, &g, gap, &dc);
	if (grid_widths(L, S, &g, avail - 2 * dc.margin, gap) < 0) {
		xfree(S);
		grid_free(&g);
		return 0;
	}
	/* each cell at its column's width (if narrower than it was laid) */
	for (i = 0; i < g.ncells && !L->stop; i++) {
		struct gcell *gc = &g.cell[i];
		int w = g.x[gc->c + gc->cs - 1] + g.w[gc->c + gc->cs - 1] - g.x[gc->c];

		if ((gc->maxw > w || (gc->rule && gc->laid_w != w) || gc->pg.partial
			|| (gc->aligned && !gc->at_width))
			&& lay_cell(L, S, gc, w, 0, 1) < 0) {
			xfree(S);
			grid_free(&g);
			return 0;
		}
		if (gc->pg.nlinks && (gc->lmap = xmalloc(gc->pg.nlinks * sizeof *gc->lmap)) != NULL)
			for (c = 0; c < (int)gc->pg.nlinks; c++)
				gc->lmap[c] = -1;
	}
	xfree(S);
	/* the rows' heights: their cells' lines (a cell spanning rows
	 * lengthens the last of them if it must) */
	for (i = 0; i < g.nrows; i++)
		g.row_h[i] = 0;
	for (i = 0; i < g.ncells; i++)
		if (g.cell[i].rs == 1 && (int)g.cell[i].pg.nlines > g.row_h[g.cell[i].r])
			g.row_h[g.cell[i].r] = (int)g.cell[i].pg.nlines;
	for (i = 0; i < g.ncells; i++) {
		struct gcell *gc = &g.cell[i];
		int have = 0, k;

		if (gc->rs == 1)
			continue;
		for (k = gc->r; k < gc->r + gc->rs; k++)
			have += g.row_h[k];
		if ((int)gc->pg.nlines > have)
			g.row_h[gc->r + gc->rs - 1] += (int)gc->pg.nlines - have;
	}
	for (i = 0, total = 0; i < g.nrows; i++) {
		g.row_at[i] = total;
		total += g.row_h[i];
	}
	g.row_at[g.nrows] = total;

	/* the grid is placed as a block - centred or to the right as a whole
	 * (its align=, or where it stands), not a line at a time - and its
	 * cells' text keeps the alignment the cells gave it */
	gw = g.x[g.ncols - 1] + g.w[g.ncols - 1] + 2 * dc.margin;
	ta = doc_attr(L->d, id, ATTR_ALIGN);
	if (ta && (ta[0] == 'c' || ta[0] == 'C'))
		align = CSS_TA_CENTER;
	else if (ta && (ta[0] == 'r' || ta[0] == 'R'))
		align = CSS_TA_RIGHT;
	else if (ta && (ta[0] == 'l' || ta[0] == 'L'))
		align = CSS_TA_LEFT;
	if (align >= CSS_TA_CENTER && !L->small && avail > gw)
		shift = align == CSS_TA_CENTER ? (avail - gw) / 2 : avail - gw;
	L->align = 0;

	/* the caption, then the rows a line at a time (in a cell of a table
	 * laid out as rows, the grid is a block of its own all the same) */
	L->cell = 0;
	L->cell_break = 0;
	block_break(L, 1);
	for (n = L->d->nodes[id].first; n; n = L->d->nodes[n].next)
		if (L->d->nodes[n].type == NODE_ELEM && L->d->nodes[n].tag == TAG_CAPTION
			&& !el_hidden(L, n)) {
			L->attr = attr | SA_BOLD;
			set_span(L);
			walk(L, n, 0);
			end_line(L);
			L->attr = attr;
			set_span(L);
		}
	if (L->m && compose_bands(L, &g, &dc, shift, attr, face, link) == 0)
		;	/* (in fonts: bands, boxes) */
	else {
		for (ln = 0, i = 0; ln < (unsigned long)total && !L->stop; ln++) {
			int x0, a = 0, h = 0;

			while (i + 1 < g.nrows && (int)ln >= g.row_at[i + 1])
				i++;
			open_line(L);
			if (ln == 0)
				base = L->p->nlines;
			x0 = L->col + shift;
			for (c = 0; c < g.ncols; c++) {
				int k = g.slot[i * g.ncols + c];
				struct gcell *gc;
				unsigned long cl;

				if (k < 0 || (gc = &g.cell[k])->c != c)
					continue;	/* (nothing, or a span's continuation) */
				cl = ln - (unsigned long)g.row_at[gc->r];
				if (cl >= gc->pg.nlines)
					continue;
				pad_to(L, x0 + g.x[c] + gc->pg.lines[cl].indent);
				put_cell_line(L, gc, cl, -1, 0);
				if (L->m && gc->pg.heights) {
					const struct lheight *lh = &gc->pg.heights[cl];

					if (lh->ascent > a)
						a = lh->ascent;
					if (lh->height - lh->ascent > h)
						h = lh->height - lh->ascent;
				}
			}
			if (a > L->line_a)
				L->line_a = a;
			if (h > L->line_d)
				L->line_d = h;
			L->attr = attr;
			L->face = face;
			L->link = link;
			set_span(L);
			end_line(L);
		}
		/* what the cells knew of the page: anchors, where content starts */
		for (i = 0; i < g.ncells && !L->stop && total > 0; i++) {
			const struct page *tp = &g.cell[i].pg;
			unsigned long at = base + (unsigned long)g.row_at[g.cell[i].r], k;

			for (k = 0; k < tp->nanchors; k++)
				if (GROW(L, anchors, anchors_cap, L->p->nanchors + 1, 64) == 0) {
					L->p->anchors[L->p->nanchors].node = tp->anchors[k].node;
					L->p->anchors[L->p->nanchors].line = at + tp->anchors[k].line;
					L->p->nanchors++;
				}
			if (L->p->main_line < 0 && tp->main_line >= 0)
				L->p->main_line = (long)(at + (unsigned long)tp->main_line);
			if (L->p->content_line < 0 && tp->content_line >= 0)
				L->p->content_line = (long)(at + (unsigned long)tp->content_line);
		}
	}
	L->attr = attr;
	L->face = face;
	L->link = link;
	set_span(L);
	block_break(L, 1);
	L->cell = cell;
	L->cell_break = 0;
	L->align = was_align;
	grid_free(&g);
	return 1;
}

/* --- floats (with metrics) ----------------------------------------------- */

/*
 * A run of floated elements (and blocks holding nothing but floats) is
 * laid out as boxes in rows: each box's content on a page of its own, as
 * wide as its width says (or shrunk to fit), left floats from the left,
 * right ones from the right, a new row when the next doesn't fit beside
 * them. The rows then go into the page as bands, as a grid's do. What
 * follows the run starts below it (text doesn't flow round floats).
 */
#define FLOAT_MAX	64

struct fitem {
	struct gcell gc;		/* its content, laid out */
	int fl;				/* CSS_FL_LEFT or CSS_FL_RIGHT */
	int m[4], bw[4], pd[4];
	unsigned long bg, bc;		/* LBOX_NONE: no background */
	int w;				/* its content's width */
	long h;				/* ... and height */
	int x;				/* its margin box's left, in the run */
	long y;				/* ... and top */
};

/* is element id floated (and shown)? its box into *b */
static int float_of(struct lay *L, nodeid id, struct css_box *b)
{
	struct eattr ea;

	scan_attrs(L->d, id, &ea);
	if (!el_box(L, id, &ea, b) || !(b->set & CSS_B_FLOAT)
		|| (b->fl != CSS_FL_LEFT && b->fl != CSS_FL_RIGHT))
		return 0;
	return !el_hidden(L, id);
}

static int all_blank(const char *s)
{
	for (; *s; s++)
		if (*s != ' ' && *s != '\n' && *s != '\t')
			return 0;
	return 1;
}

/* a block holding nothing but floats (and white space): its floats are
 * its parent's (it has no height of its own) */
static int only_floats(struct lay *L, nodeid id)
{
	const struct doc *d = L->d;
	struct css_box b;
	struct eattr ea;
	nodeid k;
	int any = 0;

	scan_attrs(d, id, &ea);
	if (el_box(L, id, &ea, &b) && box_shows(&b))
		return 0;
	for (k = d->nodes[id].first; k; k = d->nodes[k].next) {
		if (d->nodes[k].type == NODE_TEXT) {
			if (!all_blank(doc_text(d, k)))
				return 0;
		} else if (d->nodes[k].type == NODE_ELEM) {
			if (float_of(L, k, &b))
				any = 1;
			else if (!el_hidden(L, k))
				return 0;
		}
	}
	return any;
}

/* the floats from id on among its siblings, into it[]: how many; *last
 * the last sibling taken */
static int float_gather(struct lay *L, nodeid id, nodeid *items, int max,
	nodeid *last)
{
	const struct doc *d = L->d;
	struct css_box b;
	nodeid k;
	int n = 0;

	*last = id;
	for (k = id; k && n < max; k = d->nodes[k].next) {
		const struct node *nd = &d->nodes[k];

		if (nd->type == NODE_TEXT) {
			if (!all_blank(doc_text(d, k)))
				break;
		} else if (nd->type == NODE_ELEM) {
			if (float_of(L, k, &b))
				items[n++] = k;
			else if (only_floats(L, k)) {
				nodeid c;

				for (c = nd->first; c && n < max; c = d->nodes[c].next)
					if (d->nodes[c].type == NODE_ELEM && float_of(L, c, &b))
						items[n++] = c;
			} else if (!el_hidden(L, k))
				break;
		} else
			break;
		*last = k;
	}
	return n;
}

/* the rows' boxes and lines into the page, as bands (compose_bands' way) */
static void float_emit(struct lay *L, struct fitem *it, int n, long H)
{
	struct page *p = L->p;
	unsigned long nev = 0, e, k, base = 0, j;
	struct bev *ev;
	long *band, *cy, over, ink = 0;
	int *cx, i, nb = 0, x0 = 0;

	for (i = 0; i < n; i++)
		nev += it[i].gc.pg.heights ? it[i].gc.pg.nlines : 0;
	ev = xmalloc((nev + 1) * sizeof *ev);
	band = xmalloc((nev + 1) * sizeof *band);
	cy = xmalloc((size_t)n * sizeof *cy);
	cx = xmalloc((size_t)n * sizeof *cx);
	if (!ev || !band || !cy || !cx) {
		xfree(ev);
		xfree(band);
		xfree(cy);
		xfree(cx);
		return;
	}
	for (i = 0, e = 0; i < n; i++) {
		const struct fitem *f = &it[i];
		long y;

		cx[i] = f->x + f->m[3] + f->bw[3] + f->pd[3];
		cy[i] = f->y + f->m[0] + f->bw[0] + f->pd[0];
		y = cy[i];
		if (f->gc.pg.heights)
			for (j = 0; j < f->gc.pg.nlines; j++, e++) {
				ev[e].y = y;
				ev[e].c = cx[i];
				ev[e].cell = i;
				ev[e].ln = j;
				y += f->gc.pg.heights[j].height;
			}
		if (y > ink)
			ink = y;
		y = f->y + f->m[0] + f->bw[0] + f->pd[0] + f->h + f->pd[2] + f->bw[2];
		if (y > ink)
			ink = y;
	}
	qsort(ev, (size_t)nev, sizeof *ev, bev_cmp);
	for (e = 0; e < nev; e++)
		if (nb == 0)
			band[nb++] = 0;
		else if (ev[e].y != ev[e - 1].y && ev[e].y > band[nb - 1])
			band[nb++] = ev[e].y;
	if (nb == 0 && H > 0)
		band[nb++] = 0;
	for (k = 0, e = 0; k < (unsigned long)nb && !L->stop; k++) {
		long bt = band[k], bb = k + 1 < (unsigned long)nb ? band[k + 1] : H;

		open_line(L);
		if (k == 0) {
			base = p->nlines;
			x0 = L->col;
			for (i = 0; i < n; i++) {
				const struct fitem *f = &it[i];
				int bx = x0 + f->x + f->m[3];
				long by = f->y + f->m[0];
				int bwid = f->bw[3] + f->pd[3] + f->w + f->pd[1] + f->bw[1];
				long bht = f->bw[0] + f->pd[0] + f->h + f->pd[2] + f->bw[2];

				add_box(L, base, by, bx, bwid, bht, f->bg, 0, 0);
				add_box(L, base, by, bx, bwid, f->bw[0], f->bc, 0, 0);
				add_box(L, base, by + bht - f->bw[2], bx, bwid, f->bw[2], f->bc, 0, 0);
				add_box(L, base, by, bx, f->bw[3], bht, f->bc, 0, 0);
				add_box(L, base, by, bx + bwid - f->bw[1], f->bw[1], bht, f->bc, 0, 0);
			}
		}
		over = 0;
		for (; e < nev && (k + 1 >= (unsigned long)nb || ev[e].y < band[k + 1]); e++) {
			struct fitem *f = &it[ev[e].cell];
			struct gcell *gc = &f->gc;
			unsigned long ln = ev[e].ln;
			long dy = ev[e].y - bt, ext;

			pad_to(L, x0 + cx[ev[e].cell] + gc->pg.lines[ln].indent);
			put_cell_line(L, gc, ln, dy, f->bg != LBOX_NONE && dark(f->bg));
			ext = dy + gc->pg.heights[ln].height + gc->pg.heights[ln].over - (bb - bt);
			if (ext > over)
				over = ext;
			for (; gc->box_i < gc->pg.nboxes
				&& gc->pg.boxes[gc->box_i].line <= ln; gc->box_i++) {
				const struct lbox *b = &gc->pg.boxes[gc->box_i];

				if (b->line == ln)
					add_box(L, p->nlines, b->dy + dy, x0 + cx[ev[e].cell] + b->x,
						b->w, b->h, b->bg, b->border, b->border_rgb);
			}
		}
		L->asc = 0;
		set_span(L);
		L->line_a = (int)(bb - bt);
		L->line_d = 0;
		end_line(L);
		if (p->heights && p->nlines) {
			if (over > 0xFFFF)
				over = 0xFFFF;
			p->heights[p->nlines - 1].over = (unsigned short)over;
			if (over > p->max_over)
				p->max_over = (int)over;
		}
	}
	(void)base;
	if (nb > 0) {
		p->gap_line = p->nlines;
		p->gap = H > ink ? H - ink : 0;
	}
	xfree(ev);
	xfree(band);
	xfree(cy);
	xfree(cx);
}

/* a box at this place, kept even with nothing to draw (yet): fill, or
 * LBOX_NONE */
static void add_box_any(struct lay *L, unsigned long line, long dy, int x, int w,
	long h, unsigned long bg)
{
	struct page *p = L->p;
	struct lbox *b;

	if (GROW(L, boxes, boxes_cap, p->nboxes + 1, 16) < 0)
		return;
	b = &p->boxes[p->nboxes++];
	memset(b, 0, sizeof *b);
	b->line = line;
	b->dy = dy;
	b->x = x;
	b->w = w;
	b->h = h;
	b->bg = bg;
	b->border_rgb = LBOX_NONE;
}

/* the float beside the text is done with */
static void fl_end(struct lay *L)
{
	struct fitem *f = L->fl;

	if (f == NULL)
		return;
	/* (its lines may have gone lower than planned: its box reaches them) */
	if (L->fl_box + 5 <= L->p->nboxes && L->fl_ln > 0) {
		struct lbox *b = &L->p->boxes[L->fl_box];
		long h = L->fl_next - L->fl_top + f->pd[2] + f->bw[2]
			+ (L->fl_top - f->m[0]);

		if (h > b->h) {
			b[2].dy += h - b->h;
			b[3].h = b[4].h = h;
			b[0].h = h;
		}
	}
	L->width += L->fl_cut;
	L->fl_cut = 0;
	L->fl = NULL;
	layout_free(&f->gc.pg);
	xfree(f->gc.lmap);
	xfree(f);
}

/* a plain line's height */
static int plain_h(const struct lay *L)
{
	int a;

	return L->m ? L->m->height(L->m->ctx, 0, 0, &a) : 1;
}

/*
 * A line is opening beside the float: its next line goes at its start
 * (if its top is above this line's foot), and the text's start is put in
 * *ind (beside a left one). 1 when a line of it went on, 0 if not (and
 * when the float is past: it's done).
 */
static int fl_place(struct lay *L, int *ind)
{
	struct page *p = L->p;
	struct fitem *f = L->fl;
	int a = L->attr, fa = L->face, fg = L->fg;
	unsigned short link = L->link;
	unsigned long ln;
	long dy, h;

	while (L->fl_seen < p->nlines) {
		if (p->heights)
			L->fl_y += p->heights[L->fl_seen].height;
		L->fl_seen++;
	}
	if (L->fl_ln >= f->gc.pg.nlines && L->fl_y >= L->fl_bottom
		&& L->fl_y >= L->fl_next + f->pd[2] + f->bw[2] + f->m[2]) {
		fl_end(L);
		return 0;
	}
	if (!L->fl_right && *ind < L->fl_edge)
		*ind = L->fl_edge;
	if (L->fl_ln >= f->gc.pg.nlines || L->fl_next >= L->fl_y + plain_h(L)) {
		L->line_indent = L->col = *ind;
		return 0;
	}
	ln = L->fl_ln++;
	dy = L->fl_next > L->fl_y ? L->fl_next - L->fl_y : 0;
	h = f->gc.pg.heights ? f->gc.pg.heights[ln].height : 1;
	L->line_indent = L->col = L->fl_cx + f->gc.pg.lines[ln].indent;
	L->fl_line_x = L->col;
	put_cell_line(L, &f->gc, ln, dy, f->bg != LBOX_NONE && dark(f->bg));
	/* boxes in it (a table, a box): with the line */
	for (; f->gc.box_i < f->gc.pg.nboxes
		&& f->gc.pg.boxes[f->gc.box_i].line <= ln; f->gc.box_i++) {
		const struct lbox *b = &f->gc.pg.boxes[f->gc.box_i];

		if (b->line == ln)
			add_box(L, p->nlines, b->dy + dy, L->fl_cx + b->x, b->w, b->h,
				b->bg, b->border, b->border_rgb);
	}
	if (dy + h + (f->gc.pg.heights ? f->gc.pg.heights[ln].over : 0) > L->fl_over)
		L->fl_over = dy + h + (f->gc.pg.heights ? f->gc.pg.heights[ln].over : 0);
	L->fl_next = L->fl_y + dy + h;
	L->fl_inline = 1;
	L->attr = a;
	L->face = fa;
	L->fg = fg;
	L->link = link;
	set_span(L);
	return 1;
}

/* the float beside the text runs out here: its lines on lines of their
 * own, its foot reached */
static void float_flush(struct lay *L)
{
	if (L->fl == NULL)
		return;
	end_line(L);
	while (L->fl && !L->stop && L->fl_ln < L->fl->gc.pg.nlines) {
		open_line(L);
		end_line(L);
	}
	if (L->fl) {
		long y = L->fl_y;
		unsigned long k;

		for (k = L->fl_seen; k < L->p->nlines; k++)
			if (L->p->heights)
				y += L->p->heights[k].height;
		if (y < L->fl_bottom)
			vspace(L, (int)(L->fl_bottom - y));
		fl_end(L);
	}
}

/* float f (laid out, a single one) goes beside the text from here */
static void fl_start(struct lay *L, struct fitem *f)
{
	struct page *p = L->p;
	int ow = f->m[3] + f->bw[3] + f->pd[3] + f->w + f->pd[1] + f->bw[1] + f->m[1];
	int bx, bwid;
	long by, bht;

	L->fl = f;
	L->fl_right = f->fl == CSS_FL_RIGHT;
	f->x = L->fl_right ? L->width - ow : L->indent;
	if (f->x < L->indent)
		f->x = L->indent;
	bx = f->x + f->m[3];
	by = f->m[0];
	bwid = f->bw[3] + f->pd[3] + f->w + f->pd[1] + f->bw[1];
	bht = f->bw[0] + f->pd[0] + f->h + f->pd[2] + f->bw[2];
	/* its boxes: on the next line, reaching down (five, even those
	 * with nothing to draw: fl_end may stretch them) */
	L->fl_box = p->nboxes;
	add_box_any(L, p->nlines, by, bx, bwid, bht, f->bg);
	add_box_any(L, p->nlines, by, bx, bwid, f->bw[0], f->bc);
	add_box_any(L, p->nlines, by + bht - f->bw[2], bx, bwid, f->bw[2], f->bc);
	add_box_any(L, p->nlines, by, bx, f->bw[3], bht, f->bc);
	add_box_any(L, p->nlines, by, bx + bwid - f->bw[1], f->bw[1], bht, f->bc);
	L->fl_level = L->in_box;
	L->fl_cx = bx + f->bw[3] + f->pd[3];
	L->fl_cut = L->fl_right ? ow + L->em : 0;
	L->width -= L->fl_cut;
	L->fl_edge = L->fl_right ? 0 : f->x + ow + L->em;
	L->fl_ln = 0;
	L->fl_y = 0;
	L->fl_seen = p->nlines;
	L->fl_next = f->m[0] + f->bw[0] + f->pd[0];
	L->fl_top = L->fl_next;
	L->fl_bottom = f->m[0] + bht + f->m[2];
	f->gc.box_i = 0;
}

/* lay the floats from sibling id on: the last sibling taken, or 0 when
 * id isn't one to be laid so */
static nodeid float_run(struct lay *L, nodeid id)
{
	nodeid nodes[FLOAT_MAX], last;
	struct fitem *it;
	struct lay *S;
	int n, i, avail, lx, rx, attr = L->attr, face = L->face, fg = L->fg;
	long row_y = 0, row_h = 0;
	unsigned short link = L->link;

	if (L->m == NULL || L->pre || L->cell || L->gdepth >= GRID_DEPTH
		|| (n = float_gather(L, id, nodes, FLOAT_MAX, &last)) == 0)
		return 0;
	avail = L->width - L->indent;
	it = xmalloc((size_t)n * sizeof *it);
	S = xmalloc(sizeof *S);
	if (it == NULL || S == NULL) {
		xfree(it);
		xfree(S);
		return 0;
	}
	memset(it, 0, (size_t)n * sizeof *it);
	float_flush(L);
	block_break(L, 0);
	flush_blank(L);
	lx = 0;
	rx = avail;
	for (i = 0; i < n; i++) {
		struct fitem *f = &it[i];
		struct css_box b;
		int extra, ow, cw;
		unsigned long ln;

		float_of(L, nodes[i], &b);
		f->fl = b.fl;
		box_sides(L, &b, avail, f->m, f->bw, f->pd);
		f->bg = (b.set & CSS_B_BG) && b.bg != CSS_BG_NONE ? b.bg : LBOX_NONE;
		f->bc = (b.set & CSS_B_BCOLOR) ? b.bcolor : 0;
		f->gc.node = nodes[i];
		extra = f->m[1] + f->m[3] + f->bw[1] + f->bw[3] + f->pd[1] + f->pd[3];
		cw = avail - extra;
		if ((b.set & CSS_B_W) && b.w.u != CSS_U_AUTO)
			cw = box_units(L, b.w, avail);
		if (cw < L->em)
			cw = L->em;
		if (lay_cell(L, S, &f->gc, cw, 0, 1) < 0) {
			layout_free(&f->gc.pg);	/* (out of room: as far as here) */
			break;
		}
		if (!((b.set & CSS_B_W) && b.w.u != CSS_U_AUTO)) {
			/* (shrunk to fit its widest line) */
			int w = f->gc.pg.need_w, lw, word;

			for (ln = 0; ln < f->gc.pg.nlines; ln++) {
				line_widths(L, &f->gc.pg, ln, &lw, &word);
				if (lw > w)
					w = lw;
			}
			if (w < cw && w > 0) {
				cw = w;
				if (lay_cell(L, S, &f->gc, cw, 0, 1) < 0) {
					layout_free(&f->gc.pg);
					break;
				}
			}
		}
		f->w = cw;
		f->h = 0;
		if (f->gc.pg.heights)
			for (ln = 0; ln < f->gc.pg.nlines; ln++)
				f->h += f->gc.pg.heights[ln].height;
		if ((b.set & CSS_B_H) && b.h.u != CSS_U_AUTO) {
			/* as tall as it says: what's drawn may reach lower, its
			 * floats' bottom margins may not (Acid1's dd) */
			if (f->gc.pg.nlines && f->gc.pg.gap_line == f->gc.pg.nlines)
				f->h -= f->gc.pg.gap;
			if (box_units(L, b.h, 0) > f->h)
				f->h = box_units(L, b.h, 0);
		}
		if (f->gc.pg.nlinks
			&& (f->gc.lmap = xmalloc(f->gc.pg.nlinks * sizeof *f->gc.lmap)) != NULL) {
			unsigned long k;

			for (k = 0; k < f->gc.pg.nlinks; k++)
				f->gc.lmap[k] = -1;
		}
		/* its place: beside the others in the row, or a row below */
		ow = cw + extra;
		/* (2 units to spare: ems and percentages round) */
		if ((f->fl == CSS_FL_LEFT ? lx + ow > rx + 2 : rx - ow < lx - 2)
			&& (lx > 0 || rx < avail)) {
			row_y += row_h;
			row_h = 0;
			lx = 0;
			rx = avail;
		}
		if (f->fl == CSS_FL_LEFT) {
			f->x = lx;
			lx += ow;
		} else {
			f->x = rx - ow;
			if (f->x < 0)
				f->x = 0;
			rx = f->x;
		}
		f->y = row_y;
		if (f->m[0] + f->bw[0] + f->pd[0] + f->h + f->pd[2] + f->bw[2] + f->m[2] > row_h)
			row_h = f->m[0] + f->bw[0] + f->pd[0] + f->h + f->pd[2] + f->bw[2] + f->m[2];
	}
	n = i;
	xfree(S);
	/* one, not too wide: the text goes beside it */
	if (n == 1 && it[0].m[1] + it[0].m[3] + it[0].bw[1] + it[0].bw[3] + it[0].pd[1]
		+ it[0].pd[3] + it[0].w <= avail * 3 / 5) {
		fl_start(L, it);
		L->attr = attr;
		L->face = face;
		L->fg = fg;
		L->link = link;
		set_span(L);
		return last;		/* (it's L->fl's now) */
	}
	float_emit(L, it, n, row_y + row_h);
	for (i = 0; i < n; i++) {
		layout_free(&it[i].gc.pg);
		xfree(it[i].gc.lmap);
	}
	xfree(it);
	L->attr = attr;
	L->face = face;
	L->fg = fg;
	L->link = link;
	set_span(L);
	return last;
}

int layout_run(struct page *p, const struct doc *d, int width,
	enum term_cs cs, unsigned long max_lines, size_t byte_cap,
	const struct forms *fs)
{
	return layout_run_m(p, d, width, cs, max_lines, byte_cap, fs, NULL);
}

int layout_run_m(struct page *p, const struct doc *d, int width,
	enum term_cs cs, unsigned long max_lines, size_t byte_cap,
	const struct forms *fs, const struct lmetrics *m)
{
	struct lay *L;

	memset(p, 0, sizeof *p);
	p->d = d;
	p->width = width < 10 ? 10 : width;
	p->cs = cs;
	p->byte_cap = byte_cap ? byte_cap : LAYOUT_DEFAULT_CAP;
	p->main_line = -1;
	p->content_line = -1;
	p->page_bg = LBOX_NONE;
	L = xmalloc(sizeof *L);
	if (L == NULL)
		return -1;
	memset(L, 0, sizeof *L);
	L->p = p;
	L->d = d;
	L->fs = fs;
	L->m = m;
	L->em = m && m->em > 0 ? m->em : 1;
	L->fsz = 120;			/* (the screen's usual 12 pixels) */
	L->width = p->width;
	L->base_w = p->width;
	L->vw = m ? p->width : p->width * 8;
	L->max_lines = max_lines;
	L->heading_line = -1;
	set_span(L);
	if (d->nnodes > 1)
		walk(L, 1, 0);
	end_line(L);
	float_flush(L);
	fl_end(L);
	/* (boxes reserved that had nothing to draw in the end: gone) */
	{
		unsigned long i, k = 0;

		for (i = 0; i < p->nboxes; i++) {
			const struct lbox *b = &p->boxes[i];

			if (b->w > 0 && b->h > 0 && (b->bg != LBOX_NONE || b->border))
				p->boxes[k++] = *b;
		}
		p->nboxes = k;
	}
	xfree(L);
	return 0;
}

void layout_free(struct page *p)
{
	xfree(p->text);
	xfree(p->lines);
	xfree(p->heights);
	xfree(p->spans);
	xfree(p->links);
	xfree(p->anchors);
	xfree(p->images);
	xfree(p->boxes);
	xfree(p->tops);
	xfree(p->palette);
	memset(p, 0, sizeof *p);
}

const long *layout_tops(struct page *p)
{
	unsigned long i;

	if (p->tops || p->heights == NULL)
		return p->tops;
	if ((p->tops = xmalloc((p->nlines + 1) * sizeof *p->tops)) == NULL)
		return NULL;
	p->tops[0] = 0;
	for (i = 0; i < p->nlines; i++)
		p->tops[i + 1] = p->tops[i] + p->heights[i].height;
	return p->tops;
}

long layout_image(const struct page *p, const char *s)
{
	long k;

	if (s[0] != '\001' || s[1] < 2 || s[2] < 2 || s[3] < 2)
		return -1;
	k = (long)(s[1] - 2) * 900 + (s[2] - 2) * 30 + (s[3] - 2);
	return k < (long)p->nimages ? k : -1;
}

int layout_spacer_w(const char *s)
{
	int w;

	if ((s[0] != '\002' && s[0] != '\003') || s[1] < 2 || s[2] < 2 || s[3] < 2)
		return LAYOUT_NO_SPACER;
	w = (s[1] - 2) * 900 + (s[2] - 2) * 30 + (s[3] - 2);
	return s[0] == '\003' ? -w : w;	/* (back: a float's line and text) */
}

int layout_images_w(const struct page *p, const char *s, int n)
{
	int w = 0;

	for (; n >= LAYOUT_IMG_BYTES; s += LAYOUT_IMG_BYTES, n -= LAYOUT_IMG_BYTES) {
		long k = layout_image(p, s);

		if (k >= 0)
			w += p->images[k].w;
		else if ((k = layout_spacer_w(s)) != LAYOUT_NO_SPACER)
			w += (int)k;
	}
	return w;
}

unsigned long layout_span_at(const struct page *p, unsigned long s,
	unsigned long off)
{
	/* (s is where to start looking: back too, should it be past off) */
	if (s >= p->nspans)
		s = p->nspans ? p->nspans - 1 : 0;
	while (s > 0 && p->spans[s].off > off)
		s--;
	while (s + 1 < p->nspans && p->spans[s + 1].off <= off)
		s++;
	return s;
}

long layout_anchor(const struct page *p, const char *name)
{
	unsigned long i;

	for (i = 0; i < p->nanchors; i++) {
		const char *v = doc_attr(p->d, p->anchors[i].node, ATTR_ID);

		if (v && strcmp(v, name) == 0)
			return (long)p->anchors[i].line;
		v = doc_attr(p->d, p->anchors[i].node, ATTR_NAME);
		if (v && strcmp(v, name) == 0)
			return (long)p->anchors[i].line;
	}
	return -1;
}

void layout_print(const struct page *p, void *file)
{
	FILE *f = file;
	unsigned long i;

	for (i = 0; i < p->nlines; i++) {
		const struct lline *ln = &p->lines[i];

		if (ln->len)
			fprintf(f, "%*s%.*s\n", ln->indent, "", (int)ln->len,
				p->text + ln->off);
		else
			fputc('\n', f);
	}
}
