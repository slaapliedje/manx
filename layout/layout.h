/*
 * layout.h - a document laid out for a text screen of a given width:
 * lines of terminal bytes (already in the terminal's character set),
 * attribute/link spans over them, the links, and the fragment anchors.
 */
#ifndef UB_LAYOUT_H
#define UB_LAYOUT_H

#include <stddef.h>
#include "doc.h"
#include "style.h"
#include "forms.h"

/* the terminal's character set */
enum term_cs { TCS_ASCII, TCS_LATIN1, TCS_UTF8 };

struct lline {
	unsigned long off;		/* first byte in page.text */
	unsigned short len;		/* bytes */
	unsigned short indent;		/* blank columns before them */
	unsigned long span;		/* the span in effect at off */
};

/* attr/link from off up to the next span's off */
struct lspan {
	unsigned long off;
	unsigned short link;		/* 1-based index into links, 0: none */
	unsigned char attr;		/* SA_* */
	unsigned char pad;
};

enum { LK_HREF = 1, LK_FIELD };

struct llink {
	nodeid node;			/* the <a> (or the form field) */
	unsigned char kind;
	unsigned char pad;
	unsigned long line;		/* where it starts */
	unsigned short col;
};

struct lanchor {
	nodeid node;			/* has id=, or is <a name=> */
	unsigned long line;
};

struct page {
	const struct doc *d;
	int width;
	enum term_cs cs;
	char *text;
	unsigned long text_len, text_cap;
	struct lline *lines;
	unsigned long nlines, lines_cap;
	struct lspan *spans;
	unsigned long nspans, spans_cap;
	struct llink *links;
	unsigned long nlinks, links_cap;
	struct lanchor *anchors;
	unsigned long nanchors, anchors_cap;
	size_t byte_cap;		/* all of the above together */
	long main_line;			/* where <main> begins, or -1 */
	int truncated;			/* the cap was hit: the rest is missing */
	int partial;			/* stopped at max_lines */
};

#define LAYOUT_DEFAULT_CAP	(600UL * 1024)

/*
 * Lay d out width columns wide for a terminal of character set cs.
 * max_lines: stop after that many lines (0: all), for showing the top of
 * a page that is still loading. byte_cap: 0 for the default. fs: what
 * the user has put in the form fields (NULL: the page's values). 0, or
 * -1 when out of memory before anything was laid out.
 */
int layout_run(struct page *p, const struct doc *d, int width,
	enum term_cs cs, unsigned long max_lines, size_t byte_cap,
	const struct forms *fs);
void layout_free(struct page *p);

/* The span index in effect at text offset off, searching from span s. */
unsigned long layout_span_at(const struct page *p, unsigned long s,
	unsigned long off);

/* The line of the anchor named name (id= or <a name=>), or -1. */
long layout_anchor(const struct page *p, const char *name);

/* The page as plain text, one line per line (tests, dumps). */
void layout_print(const struct page *p, void *file);

#endif /* UB_LAYOUT_H */
