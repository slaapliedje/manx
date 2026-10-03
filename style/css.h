/*
 * css.h - the little of CSS Manx uses: which elements a page's style
 * sheets hide. Rules are read as the <style> text streams in, and only
 * those that set display or visibility are kept (not text "visually
 * hidden" for screen readers: without icons, Manx's readers need it).
 * Selectors: tag, *, #id, .class, [attr], [attr=v], [attr~=v] for the
 * attributes Manx keeps, combined, with descendant and child combinators. A rule with anything else
 * (pseudo-classes, sibling combinators, unknown tags) is dropped: in
 * doubt, content shows. @media width conditions are checked against the
 * window's width at each layout. The cascade is followed for display and
 * for visibility: !important, then specificity, then order.
 */
#ifndef MANX_CSS_H
#define MANX_CSS_H

#include <stddef.h>
#include "doc.h"

enum { CSS_UNSET = 0, CSS_SHOW, CSS_HIDE };

struct css_sheet;

/* A new, empty sheet (NULL: no memory). */
struct css_sheet *css_new(void);
void css_free(struct css_sheet *s);

/* A style sheet's text, in pieces of any size, between css_begin and
 * css_end. media: the <style media=""> (NULL: all); a sheet for print
 * only is read and dropped. */
void css_begin(struct css_sheet *s, const char *media);
void css_feed(struct css_sheet *s, const char *text, size_t n);
void css_end(struct css_sheet *s);

/*
 * What the sheet does to element node of d (its tag, id and class
 * attributes given, NULL when absent), for a window vw pixels wide:
 * CSS_HIDE, or CSS_UNSET when it doesn't hide it.
 */
int css_display(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw);

/* Does an inline style="" set display to something other than none
 * (which beats the sheet's display:none)? */
int css_inline_shows(const char *style);

/* How many rules the sheet kept (tests, the = page). */
unsigned long css_rules(const struct css_sheet *s);

#endif /* MANX_CSS_H */
