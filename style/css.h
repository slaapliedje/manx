/*
 * css.h - the little of CSS Manx uses: which elements a page's style
 * sheets hide. Rules are read as the <style> text streams in, and only
 * those that set display or visibility are kept (not text "visually
 * hidden" for screen readers: without icons, Manx's readers need it).
 * Selectors: tag, *, #id, .class, [attr], [attr=v], [attr~=v] for the
 * attributes Manx keeps, combined, with descendant and child combinators. A rule with anything else
 * (pseudo-classes, sibling combinators, unknown tags) is dropped: in
 * doubt, content shows. @media width conditions are checked against the
 * window's width at each layout. The cascade is followed for display, for
 * visibility and for list-style (markers or none): !important, then
 * specificity, then order.
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

/* Does an inline style="" set display to something other than none
 * (which beats the sheet's display:none)? */
int css_inline_shows(const char *style);

/* What an inline style="" says of list-style: CSS_NO_MARKER, CSS_MARKER
 * or 0. */
int css_inline_list(const char *style);

/* How many rules the sheet kept (tests, the = page). */
unsigned long css_rules(const struct css_sheet *s);

#endif /* MANX_CSS_H */
