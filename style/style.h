/*
 * style.h - how each element is shown: a built-in table per tag, plus the
 * few attributes that change it (hidden, style="display:none").
 * No CSS cascade: on a 68030 the table is the stylesheet.
 */
#ifndef UB_STYLE_H
#define UB_STYLE_H

#include "doc.h"

enum display {
	D_INLINE = 0,
	D_BLOCK,
	D_LIST_ITEM,
	D_NONE,
	D_TABLE_ROW,		/* a line of its own, cells side by side */
	D_TABLE_CELL,		/* inline, set apart from its neighbours */
};

/* text attributes (bits) */
#define SA_BOLD		0x01
#define SA_UNDER	0x02
#define SA_LINK		0x04	/* set by the layout for a[href] */
#define SA_FIELD	0x08	/* a form field */
#define SA_MARK		0x10	/* drawn: a find match */

struct style {
	unsigned char display;
	unsigned char margin;	/* blank lines before and after (blocks) */
	unsigned char indent;	/* extra left indent of the content */
	unsigned char attr;	/* SA_* added to the content */
	unsigned char pre;	/* white space kept, lines as in the source */
};

/* The style of element id in d. */
void style_of(const struct doc *d, nodeid id, struct style *s);

/* The same from what the caller already has: the tag and its hidden,
 * style and id attributes (NULL when absent). */
void style_for(int tag, const char *hidden, const char *css, const char *id,
	struct style *s);

#endif /* UB_STYLE_H */
