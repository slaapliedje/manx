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

#define CSS_RGB_SET	0x1000000UL	/* fg: this colour, 0xRRGGBB below */
#define CSS_RGB_DEFAULT	0x2000000UL	/* fg: back to the screen's own */

struct css_text {
	unsigned long fg;		/* 0: not said; CSS_RGB_* */
	unsigned char weight, style, deco, align;
};

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

/* How many rules the sheet kept (tests, the = page). */
unsigned long css_rules(const struct css_sheet *s);

#endif /* MANX_CSS_H */
