/*
 * layout.h - a document laid out for a screen of a given width: lines of
 * bytes (already in the screen's character set), attribute/link spans
 * over them, the links, and the fragment anchors.
 *
 * Widths are a terminal's columns, or, given struct lmetrics, whatever
 * units it measures in (an X screen: pixels, with proportional fonts).
 */
#ifndef MANX_LAYOUT_H
#define MANX_LAYOUT_H

#include <stddef.h>
#include "doc.h"
#include "style.h"
#include "forms.h"

/* the terminal's character set */
enum term_cs { TCS_ASCII, TCS_LATIN1, TCS_UTF8 };

struct lline {
	unsigned long off;		/* first byte in page.text */
	unsigned short len;		/* bytes */
	unsigned short indent;		/* blank columns (units) before them */
	unsigned long span;		/* the span in effect at off */
};

/* a line's height and baseline in units: only with metrics (a terminal's
 * lines are all 1 high, and don't spend memory saying so) */
struct lheight {
	unsigned short height, ascent;
};

/* attr/link from off up to the next span's off */
struct lspan {
	unsigned long off;
	unsigned short link;		/* 1-based index into links, 0: none */
	unsigned char attr;		/* SA_* */
	unsigned char face;		/* LF_* (only with metrics; else 0) */
	unsigned char color;		/* the page's style sheet's colour:
					 * palette[color - 1]; 0: the screen's */
};

/*
 * How a screen with proportional fonts measures: the width of n bytes of
 * text in the look of attr/face, and a line's height with its baseline
 * (ascent). em is the unit of indents, a terminal column's worth.
 */
struct lmetrics {
	int (*width)(void *ctx, int attr, int face, const char *s, int n);
	int (*height)(void *ctx, int attr, int face, int *ascent);
	void *ctx;
	int em;
	/* An <img>'s own size in units, if known yet: 1, else 0. NULL, or
	 * not known and no width and height given: its alt text instead. */
	int (*image)(void *ctx, nodeid node, int *w, int *h);
	void *image_ctx;
};

/*
 * With metrics, an image is shown in the text as LAYOUT_IMG_BYTES bytes
 * (control characters, which text never has) in a span of face LF_IMAGE,
 * standing for p->images[layout_image(p, s)]; a run of that face holds
 * one or more. It sits on the baseline, as wide as the image.
 */
#define LAYOUT_IMG_BYTES	4
#define LAYOUT_MAX_IMAGES	4096

struct limage {
	nodeid node;			/* the <img> */
	unsigned short w, h;		/* the size it is shown at, in units */
	unsigned long line;		/* the line it is on */
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
	struct lheight *heights;	/* NULL without metrics */
	unsigned long heights_cap;
	struct lspan *spans;
	unsigned long nspans, spans_cap;
	struct llink *links;
	unsigned long nlinks, links_cap;
	struct lanchor *anchors;
	unsigned long nanchors, anchors_cap;
	struct limage *images;		/* NULL without metrics */
	unsigned long nimages, images_cap;
	unsigned long *palette;		/* the spans' colours, 0xRRGGBB */
	int npalette;			/* (LAYOUT_MAX_COLORS at most) */
	size_t byte_cap;		/* all of the above together */
	long main_line;			/* where <main> begins, or -1 */
	long content_line;		/* the first real paragraph (or its
					 * heading), or -1 */
	int truncated;			/* the cap was hit: the rest is missing */
	int partial;			/* stopped at max_lines */
};

#define LAYOUT_DEFAULT_CAP	(600UL * 1024)
#define LAYOUT_MAX_COLORS	255	/* a page's distinct text colours */

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
/* the same, measured by m (NULL: a terminal's columns, as layout_run) */
int layout_run_m(struct page *p, const struct doc *d, int width,
	enum term_cs cs, unsigned long max_lines, size_t byte_cap,
	const struct forms *fs, const struct lmetrics *m);
void layout_free(struct page *p);

/* The span index in effect at text offset off, looking from span s. */
unsigned long layout_span_at(const struct page *p, unsigned long s,
	unsigned long off);

/* The image whose bytes begin at s (see LAYOUT_IMG_BYTES): its index in
 * p->images, or -1. */
long layout_image(const struct page *p, const char *s);

/* With metrics, a gap of an exact width (between a grid's columns) is
 * LAYOUT_IMG_BYTES bytes in the same kind of run, a spacer: its width,
 * or -1 if the bytes at s aren't one. */
int layout_spacer_w(const char *s);

/* How wide n bytes of a run of face LF_IMAGE are: its images' and
 * spacers' widths. */
int layout_images_w(const struct page *p, const char *s, int n);

/* The line of the anchor named name (id= or <a name=>), or -1. */
long layout_anchor(const struct page *p, const char *name);

/* The page as plain text, one line per line (tests, dumps). */
void layout_print(const struct page *p, void *file);

#endif /* MANX_LAYOUT_H */
