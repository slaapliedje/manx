/*
 * css.h - the little of CSS Manx uses: which elements a page's style
 * sheets hide, and how text looks (colour, weight, italics, underline,
 * alignment). Rules are read as the <style> text streams in, and only
 * those that say something about these are kept (not text "visually
 * hidden" for screen readers: without icons, Manx's readers need it).
 * Selectors: tag, *, #id, .class, [attr], [attr=v], [attr~=v] for the
 * attributes Manx keeps, :root and :link, combined, with descendant and
 * child combinators. A rule with anything else (other pseudo-classes,
 * sibling combinators, unknown tags) is dropped: in doubt, content shows
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
 * Boxes: margin, border, padding, width, height, float, clear, display
 * and background-color, for a screen that draws them (css_boxes: 1, the
 * default; a terminal sets it 0 before reading sheets, and then their
 * rules about boxes take no memory and no time).
 */
extern int css_boxes;

/* which of struct css_box's fields were said */
#define CSS_B_BG	0x000001UL
#define CSS_B_BCOLOR	0x000002UL
#define CSS_B_BSTYLE	0x000004UL
#define CSS_B_M(i)	(0x000008UL << (i))	/* margin: top right bottom left */
#define CSS_B_P(i)	(0x000080UL << (i))	/* padding */
#define CSS_B_BW(i)	(0x000800UL << (i))	/* border width */
#define CSS_B_W		0x008000UL
#define CSS_B_H		0x010000UL
#define CSS_B_FLOAT	0x020000UL
#define CSS_B_CLEAR	0x040000UL
#define CSS_B_DISP	0x080000UL
#define CSS_B_NBITS	20

enum { CSS_FL_NONE = 1, CSS_FL_LEFT, CSS_FL_RIGHT };
enum { CSS_CL_NONE = 1, CSS_CL_LEFT, CSS_CL_RIGHT, CSS_CL_BOTH };
enum { CSS_DI_BLOCK = 1, CSS_DI_INLINE, CSS_DI_LIST_ITEM, CSS_DI_INLINE_BLOCK };

#define CSS_BG_NONE	0xFFFFFFFFUL	/* transparent */

struct css_box {
	unsigned long set;		/* CSS_B_* */
	unsigned long bg;		/* 0xRRGGBB, or CSS_BG_NONE */
	unsigned long bcolor;		/* the border's colour, 0xRRGGBB */
	unsigned char bstyle;		/* the border is drawn (solid...): 1 */
	unsigned char fl, clr, disp;	/* CSS_FL_*, CSS_CL_*, CSS_DI_* */
	struct css_len m[4], p[4], bw[4], w, h;
};

/*
 * What the sheet says of element node's box (asked after css_style, for
 * the same element), over *b: 1 if anything.
 */
int css_box(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw, struct css_box *b);

/* What an inline style="" says of the box, over *b. */
void css_inline_box(const char *style, struct css_box *b);

/* How many rules the sheet kept (tests, the = page). */
unsigned long css_rules(const struct css_sheet *s);

#endif /* MANX_CSS_H */
