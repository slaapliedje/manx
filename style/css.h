/*
 * css.h - the little of CSS Manx uses: which elements a page's style
 * sheets hide, and how text looks (colour, weight, italics, underline,
 * alignment). Rules are read as the <style> text streams in, and only
 * those that say something about these are kept (not text "visually
 * hidden" for screen readers: without icons, Manx's readers need it).
 * Selectors: tag, *, #id, .class, [attr], [attr=v], [attr~=v] for the
 * attributes Manx keeps, :root and :link, combined, with descendant,
 * child and sibling (+, ~) combinators, and ::before and ::after at the
 * end. A rule with anything else (other pseudo-classes and -elements,
 * unknown tags) is dropped: in doubt, content shows
 * as it would without the sheet. @media width conditions are checked
 * against the window's width at each layout. The cascade is followed for
 * each property: !important, then specificity, then order. Colours may
 * come from custom properties (var()) set on :root, html or body.
 */
#ifndef MANX_CSS_H
#define MANX_CSS_H

#include <stddef.h>
#include "doc.h"

/* a rule's say about a property */
enum { CSS_UNSET = 0, CSS_SHOW, CSS_HIDE };

/* what css_display says of an element (bits) */
#define CSS_HIDDEN	1	/* display:none or visibility:hidden */
#define CSS_NO_MARKER	2	/* list-style: none (inherited) */
#define CSS_MARKER	4	/* list-style: a marker again */
#define CSS_BEFORE	8	/* rules about its ::before hold (css_pseudo) */
#define CSS_AFTER	16	/* ... its ::after */

struct css_sheet;

/* A new, empty sheet (NULL: no memory). */
struct css_sheet *css_new(void);
void css_free(struct css_sheet *s);

/* A style sheet's text, in pieces of any size, between css_begin and
 * css_end. media: the <style media=""> (NULL: all); a sheet for print
 * only is read and dropped. pos: where the sheet is in the page (its
 * element's node), which orders it in the cascade: a sheet linked
 * early ranks below a later <style> whenever it arrives. */
void css_begin(struct css_sheet *s, const char *media, nodeid pos);
void css_feed(struct css_sheet *s, const char *text, size_t n);
void css_end(struct css_sheet *s);

/*
 * What the sheet does to element node of d (its tag, id and class
 * attributes given, NULL when absent), for a window vw pixels wide:
 * CSS_* bits, 0 when nothing.
 */
int css_display(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw);

/* how text looks: what the sheet (or a style="") says, 0 where nothing */
enum { CSS_FW_BOLD = 1, CSS_FW_NORMAL };	/* font-weight */
enum { CSS_FS_ITALIC = 1, CSS_FS_NORMAL };	/* font-style */
enum { CSS_TD_UNDER = 1, CSS_TD_NONE };		/* text-decoration */
enum { CSS_TA_LEFT = 1, CSS_TA_CENTER, CSS_TA_RIGHT };	/* text-align */

/* a length: px, em (a font size: the parent's), % (of what it's of) */
enum { CSS_U_NONE, CSS_U_PX, CSS_U_EM, CSS_U_PCT, CSS_U_AUTO };

struct css_len {
	short v;			/* tenths of the unit */
	unsigned char u;		/* CSS_U_* */
};

#define CSS_RGB_SET	0x1000000UL	/* fg: this colour, 0xRRGGBB below */
#define CSS_RGB_DEFAULT	0x2000000UL	/* fg: back to the screen's own */

struct css_text {
	unsigned long fg;		/* 0: not said; CSS_RGB_* */
	unsigned char weight, style, deco, align;
	unsigned long bg;		/* background (style="" only): 0, or
					 * CSS_RGB_SET | 0xRRGGBB */
	struct css_len fsize;		/* font-size (u CSS_U_NONE: not said);
					 * keywords as px, rem as 16px */
};

/* Does a media query list (<source media="">) hold on a screen vw units
 * wide? */
int css_media_matches(const char *q, int vw);

/* An HTML attribute's colour (bgcolor=, bordercolor=): a CSS colour, or
 * hex without its '#' as browsers take it. 1 and *rgb, or 0. */
int css_attr_color(const char *v, unsigned long *rgb);

/*
 * css_display, and how the element's text looks into *t (only what the
 * sheet says of this element itself: inheriting is the caller's).
 */
int css_style(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw, struct css_text *t);

/* What an inline style="" says of the same, over *t (s: for var(); may be
 * NULL). */
void css_inline_text(struct css_sheet *s, const char *style, struct css_text *t);

/* Does an inline style="" set display to something other than none
 * (which beats the sheet's display:none)? */
int css_inline_shows(const char *style);

/* What an inline style="" says of list-style: CSS_NO_MARKER, CSS_MARKER
 * or 0. */
int css_inline_list(const char *style);

/*
 * Boxes: margin, border, padding, width, height (and their min- and
 * max-), float, clear, display, background (colour, image, repeat,
 * attachment, position), position (with top,
 * right, bottom, left and z-index), for a screen that draws them (css_boxes: 1, the
 * default; a terminal sets it 0 before reading sheets, and then their
 * rules about boxes take no memory and no time).
 */
extern int css_boxes;

/* which of struct css_box's fields were said */
#define CSS_B_BG	0x000001UL
/* (0x000002, 0x000004: free) */
#define CSS_B_M(i)	(0x000008UL << (i))	/* margin: top right bottom left */
#define CSS_B_P(i)	(0x000080UL << (i))	/* padding */
#define CSS_B_BW(i)	(0x000800UL << (i))	/* border width */
#define CSS_B_W		0x008000UL
#define CSS_B_H		0x010000UL
#define CSS_B_FLOAT	0x020000UL
#define CSS_B_CLEAR	0x040000UL
#define CSS_B_DISP	0x080000UL
#define CSS_B_LH	0x100000UL	/* line-height (inherited: the layout's) */
#define CSS_B_MINW	0x200000UL	/* min-width (auto: none) */
#define CSS_B_MAXW	0x400000UL	/* max-width (auto: none) */
#define CSS_B_MINH	0x800000UL	/* min-height */
#define CSS_B_MAXH	0x1000000UL	/* max-height */
#define CSS_B_POS	0x2000000UL	/* position */
#define CSS_B_OFF(i)	(0x4000000UL << (i))	/* top right bottom left */
#define CSS_B_Z		0x40000000UL	/* z-index (auto: 0) */
#define CSS_B_NBITS	31
/* ... and of set2: */
#define CSS_B2_BS(i)	(0x01UL << (i))	/* border style: top right bottom left */
#define CSS_B2_BC(i)	(0x10UL << (i))	/* border colour */
#define CSS_B2_BGI	0x100UL		/* background-image */
#define CSS_B2_BGR	0x200UL		/* background-repeat */
#define CSS_B2_BGA	0x400UL		/* background-attachment */
#define CSS_B2_BGP	0x800UL		/* background-position */
#define CSS_B2_NBITS	12

enum { CSS_FL_NONE = 1, CSS_FL_LEFT, CSS_FL_RIGHT, CSS_FL_INHERIT };
enum { CSS_CL_NONE = 1, CSS_CL_LEFT, CSS_CL_RIGHT, CSS_CL_BOTH };
enum { CSS_DI_BLOCK = 1, CSS_DI_INLINE, CSS_DI_LIST_ITEM, CSS_DI_INLINE_BLOCK };
enum { CSS_PO_STATIC = 1, CSS_PO_RELATIVE, CSS_PO_ABSOLUTE, CSS_PO_FIXED,
	CSS_PO_STICKY };

enum { CSS_BR_REPEAT, CSS_BR_X, CSS_BR_Y, CSS_BR_NONE };	/* background-repeat */

#define CSS_BG_NONE	0xFFFFFFFFUL	/* transparent */
#define CSS_BG_CURRENT	0xFFFFFFFEUL	/* currentcolor (a border's) */

struct css_box {
	unsigned long set;		/* CSS_B_* */
	unsigned long set2;		/* CSS_B2_* */
	unsigned long bg;		/* 0xRRGGBB, or CSS_BG_NONE */
	unsigned long bcolor[4];	/* each side's border colour, 0xRRGGBB,
					 * CSS_BG_NONE: transparent, or
					 * CSS_BG_CURRENT (unset: currentcolor) */
	unsigned char bstyle[4];	/* each side's border is drawn
					 * (solid...): 1 */
	unsigned char fl, clr, disp;	/* CSS_FL_*, CSS_CL_*, CSS_DI_* */
	unsigned char pos;		/* CSS_PO_* */
	short z;			/* z-index */
	struct css_len m[4], p[4], bw[4], w, h;
	struct css_len minw, maxw, minh, maxh;	/* (max: none is auto) */
	struct css_len lh;		/* line-height: em (a number: times the
					 * font's size), % (an em or a %, in
					 * tenths of the font's size: a length),
					 * px, or auto: normal */
	struct css_len off[4];		/* top, right, bottom, left (auto) */
	/* background-image: the url()'s URL, bgsrc_n bytes (NULL: none),
	 * good until the sheet is read further; relative to bgbase (its
	 * sheet's URL; NULL: the page's) */
	const char *bgsrc, *bgbase;
	unsigned short bgsrc_n;
	unsigned char bgrep;		/* CSS_BR_* */
	unsigned char bgfixed;		/* background-attachment: fixed */
	struct css_len bgpos[2];	/* background-position: x, y (px, em
					 * or %) */
	unsigned long bgi;		/* (the sheet's: where bgsrc is kept) */
};

/*
 * What the sheet says of element node's box (asked after css_style, for
 * the same element), over *b: 1 if anything.
 */
int css_box(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw, struct css_box *b);

/* The URL the sheet's text from now on (until css_begin) came from:
 * its url()s are relative to it. */
void css_base(struct css_sheet *s, const char *url);

/* What an inline style="" says of the box, over *b. */
void css_inline_box(const char *style, struct css_box *b);

/*
 * Element node's ::before (which: 1) or ::after (2), asked when css_style
 * said CSS_BEFORE or CSS_AFTER of it: 1 when there is one, its content
 * (UTF-8, its strings and attr()s) in out (n bytes, NUL-ended), its text's
 * look in *t (what isn't said: the element's) and what is said of its box
 * over *b (with css_boxes); 0 when there is none (no content, or none,
 * or display: none).
 */
int css_pseudo(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw, int which, struct css_text *t,
	struct css_box *b, char *out, size_t n);

/* How many rules the sheet kept (tests, the = page). */
unsigned long css_rules(const struct css_sheet *s);

#endif /* MANX_CSS_H */
