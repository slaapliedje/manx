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
	unsigned long para_mark, para_line;	/* <p>: where it began */
	unsigned short link;
	short indent;
	unsigned long link_mark;	/* text_len when the link began */
	unsigned char cell;
	unsigned long cell_mark;
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
	int gdepth;			/* the grids this is in a cell of */
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
		+ p->images_cap * sizeof(struct limage);
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

	if (p->nspans) {
		s = &p->spans[p->nspans - 1];
		if (s->attr == L->attr && s->link == L->link && s->face == face
			&& s->color == L->fg)
			return;
		if (s->off == p->text_len) {
			/* nothing shown with the last one: replace it */
			s->attr = (unsigned char)L->attr;
			s->link = L->link;
			s->face = face;
			s->color = (unsigned char)L->fg;
			if (p->nspans >= 2 && s[-1].attr == s->attr
				&& s[-1].link == s->link && s[-1].face == s->face
				&& s[-1].color == s->color) {
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
	int endcol = end == p->text_len ? L->col : L->word_col, indent = L->line_indent;

	if (!L->pre)
		while (end > L->line_off && t[end - 1] == ' ') {
			end--;
			endcol -= space_w(L);
		}
	/* centred or to the right (not a table row's line of cells, nor a
	 * grid cell's: its width is what its lines measure) */
	if (L->align >= CSS_TA_CENTER && !L->pre && !L->cell && !L->small
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
	L->line_open = 0;
	L->blank_run = 0;
}

static void end_line(struct lay *L)
{
	if (L->line_open)
		close_line_at(L, L->p->text_len);
	L->in_word = 0;
	L->pend_space = 0;
}

static void open_line(struct lay *L)
{
	int ind = L->indent;

	/* the blank lines a block's margin asked for (never at the top) */
	if (L->p->nlines) {
		while (L->blank_run < L->pend_lines) {
			push_line(L, L->p->text_len, 0, 0, cur_span(L), 0);
			L->blank_run++;
		}
	}
	L->pend_lines = 0;
	if (ind > L->width / 2)
		ind = L->width / 2;
	L->line_open = 1;
	L->line_off = L->p->text_len;
	L->line_span = cur_span(L);
	L->line_a = L->line_d = 0;
	L->in_word = 0;
	L->pend_space = 0;
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
static void make_room(struct lay *L, int w)
{
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
	if (strcmp(type, "checkbox") == 0) {
		render_field(L, id, checked ? "[x]" : "[ ]");
		return;
	}
	if (strcmp(type, "radio") == 0) {
		render_field(L, id, checked ? "(*)" : "( )");
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

/* enter element id at depth: 1 when its children are to be laid out */
static int enter(struct lay *L, nodeid id, int depth)
{
	const struct doc *d = L->d;
	const struct node *n = &d->nodes[id];
	struct style s;
	struct saved *sv;
	struct eattr ea;
	struct css_text ct;
	const char *v;
	int tag = n->tag, cssf = 0;

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
	/* list-style: inherited, the element's own style="" last */
	if (ea.style && css_inline_list(ea.style))
		cssf = (cssf & ~(CSS_NO_MARKER | CSS_MARKER)) | css_inline_list(ea.style);
	if (cssf & CSS_NO_MARKER)
		L->nomarker = 1;
	else if (cssf & CSS_MARKER)
		L->nomarker = 0;

	switch (s.display) {
	case D_BLOCK:
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
	if (s.face & LF_HMASK)
		L->face = (L->face & ~LF_HMASK) | (s.face & LF_HMASK);
	L->face |= s.face & (LF_MONO | LF_ITALIC);
	if (tag == TAG_UL || tag == TAG_OL || tag == TAG_MENU || tag == TAG_DIR) {
		list_begin(L, id, tag);
		sv->list = 1;
	}
	L->attr |= s.attr;
	text_look(L, tag, &s, &ct, ea.align);
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
		/* the link had no text: show something to select */
		if (L->p->text_len == L->link_mark) {
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
static void walk(struct lay *L, nodeid root, int depth)
{
	const struct doc *d = L->d;
	nodeid id = d->nodes[root].first;

	while (id && !L->stop) {
		const struct node *n = &d->nodes[id];
		int descend = 0;

		if (n->type == NODE_TEXT)
			put_text(L, doc_text(d, id));
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
	int laid_w;			/* the width pg was laid out at */
	int frame;			/* it frames a page (see FRAME_MIN_EM) */
	struct page pg;
	int *lmap;			/* its links' numbers in the page, -1 */
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
	if (g->ncols < 2 || g->nrows == 0)
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
	int run = 0;

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
}

/* lay cell gc out into its page, width wide, as a block's content, with
 * S (a struct lay of the grid's, reused for each cell); max_lines: stop
 * there (0: all of it) */
static int lay_cell(struct lay *L, struct lay *S, struct gcell *gc, int width,
	unsigned long max_lines)
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
	if (room < 4096)
		return -1;
	/* (all but the saved-state stack, written before it's read: on a
	 * 68030 clearing it for every cell costs) */
	memset(S, 0, offsetof(struct lay, st));
	S->small = 1;
	S->gdepth = L->gdepth + 1;
	S->max_lines = max_lines;
	S->p = pg;
	S->d = L->d;
	S->fs = L->fs;
	S->m = L->m;
	S->em = L->em;
	S->width = width;
	S->vw = L->vw;
	S->face = L->face;
	S->nomarker = L->nomarker;
	S->heading_line = -1;
	style_of(L->d, gc->node, &s);	/* (a <th>: bold) */
	S->attr = L->attr | s.attr;
	S->align = L->align;
	if (L->fg && L->p->palette)
		S->fg = page_color(pg, L->p->palette[L->fg - 1]);
	{
		/* the cell's own look: its sheet, style="", align= */
		struct css_text ct;
		const char *st = doc_attr(L->d, gc->node, ATTR_STYLE);

		memset(&ct, 0, sizeof ct);
		if (L->d->sheet)
			css_style(L->d->sheet, L->d, gc->node, doc_attr(L->d, gc->node, ATTR_ID),
				doc_attr(L->d, gc->node, ATTR_CLASS), L->vw, &ct);
		if (st)
			css_inline_text(L->d->sheet, st, &ct);
		s.display = D_BLOCK;		/* (its lines are its own) */
		text_look(S, L->d->nodes[gc->node].tag, &s, &ct,
			doc_attr(L->d, gc->node, ATTR_ALIGN));
	}
	set_span(S);
	if (doc_attr(L->d, gc->node, ATTR_ID))
		add_anchor(S, gc->node);	/* (the cell's own id) */
	walk(S, gc->node, 0);
	end_line(S);
	/* (no blank lines at its end) */
	while (pg->nlines && pg->lines[pg->nlines - 1].len == 0)
		pg->nlines--;
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
		if (lay_cell(L, S, gc, avail, gc->frame ? FRAME_MEASURE : 0) < 0)
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

/* line ln of cell gc's page onto the open line */
static void put_cell_line(struct lay *L, struct gcell *gc, unsigned long ln)
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
		L->attr = sp->attr;
		L->face = sp->face;
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
}

/* lay table id out as a grid: 1, or 0 when it is to be laid out as rows */
static int grid_table(struct lay *L, nodeid id)
{
	struct grid g;
	struct lay *S;
	int gap = L->m ? L->em : 2, avail, i, c, total, attr = L->attr, face = L->face;
	int cell = L->cell, was_align = L->align, align = L->align, shift = 0, gw;
	const char *ta;
	unsigned short link = L->link;
	unsigned long base = 0, ln;
	nodeid n;

	if (L->pre || L->marker_w || L->gdepth >= GRID_DEPTH)
		return 0;
	avail = L->width - (L->indent > L->width / 2 ? L->width / 2 : L->indent);
	if (grid_build(L, id, &g, avail) < 0)
		return 0;
	if ((S = xmalloc(sizeof *S)) == NULL) {
		grid_free(&g);
		return 0;
	}
	if (grid_widths(L, S, &g, avail, gap) < 0) {
		xfree(S);
		grid_free(&g);
		return 0;
	}
	/* each cell at its column's width (if narrower than it was laid) */
	for (i = 0; i < g.ncells && !L->stop; i++) {
		struct gcell *gc = &g.cell[i];
		int w = g.x[gc->c + gc->cs - 1] + g.w[gc->c + gc->cs - 1] - g.x[gc->c];

		if ((gc->maxw > w || (gc->rule && gc->laid_w != w) || gc->pg.partial)
			&& lay_cell(L, S, gc, w, 0) < 0) {
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
	gw = g.x[g.ncols - 1] + g.w[g.ncols - 1];
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
			put_cell_line(L, gc, cl);
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
	L = xmalloc(sizeof *L);
	if (L == NULL)
		return -1;
	memset(L, 0, sizeof *L);
	L->p = p;
	L->d = d;
	L->fs = fs;
	L->m = m;
	L->em = m && m->em > 0 ? m->em : 1;
	L->width = p->width;
	L->vw = m ? p->width : p->width * 8;
	L->max_lines = max_lines;
	L->heading_line = -1;
	set_span(L);
	if (d->nnodes > 1)
		walk(L, 1, 0);
	end_line(L);
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
	xfree(p->palette);
	memset(p, 0, sizeof *p);
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
	if (s[0] != '\002' || s[1] < 2 || s[2] < 2 || s[3] < 2)
		return -1;
	return (s[1] - 2) * 900 + (s[2] - 2) * 30 + (s[3] - 2);
}

int layout_images_w(const struct page *p, const char *s, int n)
{
	int w = 0;

	for (; n >= LAYOUT_IMG_BYTES; s += LAYOUT_IMG_BYTES, n -= LAYOUT_IMG_BYTES) {
		long k = layout_image(p, s);

		if (k >= 0)
			w += p->images[k].w;
		else if ((k = layout_spacer_w(s)) > 0)
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
