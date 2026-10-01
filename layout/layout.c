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
#include <string.h>
#include "os.h"
#include "tags.h"
#include "utf8.h"
#include "layout.h"

#define MAX_DEPTH	200
#define MAX_LISTS	16

struct saved {
	unsigned char attr, pre, tag, list;
	unsigned char display, margin, face;
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
	struct list lists[MAX_LISTS];
	int nlists;
	int oom;
	struct saved st[MAX_DEPTH];
};

/* --- storage --------------------------------------------------------- */

static size_t used(const struct page *p)
{
	return p->text_cap + p->lines_cap * sizeof(struct lline)
		+ p->heights_cap * sizeof(struct lheight)
		+ p->spans_cap * sizeof(struct lspan)
		+ p->links_cap * sizeof(struct llink)
		+ p->anchors_cap * sizeof(struct lanchor);
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

/* attr/link (and with metrics, the face) from here on */
static void set_span(struct lay *L)
{
	struct page *p = L->p;
	struct lspan *s;
	unsigned char face = (unsigned char)(L->m ? L->face : 0);

	if (p->nspans) {
		s = &p->spans[p->nspans - 1];
		if (s->attr == L->attr && s->link == L->link && s->face == face)
			return;
		if (s->off == p->text_len) {
			/* nothing shown with the last one: replace it */
			s->attr = (unsigned char)L->attr;
			s->link = L->link;
			s->face = face;
			if (p->nspans >= 2 && s[-1].attr == s->attr
				&& s[-1].link == s->link && s[-1].face == s->face)
				p->nspans--;
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

/* end the open line at byte end (trailing spaces dropped) */
static void close_line_at(struct lay *L, unsigned long end)
{
	const char *t = L->p->text;

	if (!L->pre)
		while (end > L->line_off && t[end - 1] == ' ')
			end--;
	push_line(L, L->line_off, end - L->line_off, L->line_indent,
		L->line_span, 1);
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
		L->marker_w = 2;
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
	const char *hidden, *style, *id, *name, *href;
};

static void scan_attrs(const struct doc *d, nodeid id, struct eattr *a)
{
	const unsigned char *p = d->attr + d->nodes[id].data;
	const unsigned char *e = d->attr + d->attr_len;

	a->hidden = a->style = a->id = a->name = a->href = NULL;
	while (p < e && *p) {
		int k = *p++;
		const char *v = (const char *)p;

		switch (k) {
		case ATTR_HIDDEN: a->hidden = v; break;
		case ATTR_STYLE: a->style = v; break;
		case ATTR_ID: a->id = v; break;
		case ATTR_NAME: a->name = v; break;
		case ATTR_HREF: a->href = v; break;
		}
		while (*p)
			p++;
		p++;
	}
}

/* enter element id at depth: 1 when its children are to be laid out */
static int enter(struct lay *L, nodeid id, int depth)
{
	const struct doc *d = L->d;
	const struct node *n = &d->nodes[id];
	struct style s;
	struct saved *sv;
	struct eattr ea;
	const char *v;
	int tag = n->tag;

	if (n->data)
		scan_attrs(d, id, &ea);
	else
		ea.hidden = ea.style = ea.id = ea.name = ea.href = NULL;
	style_for(tag, ea.hidden, ea.style, ea.id, &s);
	if (s.display == D_NONE)
		return 0;

	/* leaves with a rendering of their own */
	switch (tag) {
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
		for (i = 0; i < n; i++)
			put_bytes(L, "-", 1);
		note_height(L);
		L->col += n * dw;
		end_line(L);
		return 0;
	}
	case TAG_IMG:
	case TAG_IMAGE:
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
	L->attr = sv->attr;
	L->pre = sv->pre;
	L->face = sv->face;
	L->link = sv->link;
	L->link_mark = sv->link_mark;
	L->indent = sv->indent;
	L->cell = sv->cell;
	L->cell_mark = sv->cell_mark;
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
	nodeid id;
	int depth = 0;

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
	L->max_lines = max_lines;
	L->heading_line = -1;
	set_span(L);

	id = d->nnodes > 1 ? d->nodes[1].first : 0;
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
			if (id <= 1) {
				id = 0;
				break;
			}
			depth--;
			leave(L, id, depth);
		}
		if (id)
			id = d->nodes[id].next;
	}
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
	memset(p, 0, sizeof *p);
}

unsigned long layout_span_at(const struct page *p, unsigned long s,
	unsigned long off)
{
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
