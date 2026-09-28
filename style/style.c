/*
 * style.c - the built-in style table.
 */
#include <string.h>
#include "tags.h"
#include "style.h"

struct rule {
	unsigned char tag, display, margin, indent, attr, pre;
};

/* anything not listed is inline and plain */
static const struct rule rules[] = {
	{ TAG_ADDRESS,	D_BLOCK, 1, 0, 0, 0 },
	{ TAG_AREA,	D_NONE, 0, 0, 0, 0 },
	{ TAG_ARTICLE,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_ASIDE,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_AUDIO,	D_NONE, 0, 0, 0, 0 },
	{ TAG_B,	D_INLINE, 0, 0, SA_BOLD, 0 },
	{ TAG_BASE,	D_NONE, 0, 0, 0, 0 },
	{ TAG_BLOCKQUOTE, D_BLOCK, 1, 4, 0, 0 },
	{ TAG_BODY,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_CANVAS,	D_NONE, 0, 0, 0, 0 },
	{ TAG_CAPTION,	D_BLOCK, 0, 0, SA_BOLD, 0 },
	{ TAG_CENTER,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_CITE,	D_INLINE, 0, 0, SA_UNDER, 0 },
	{ TAG_COL,	D_NONE, 0, 0, 0, 0 },
	{ TAG_COLGROUP,	D_NONE, 0, 0, 0, 0 },
	{ TAG_DATALIST,	D_NONE, 0, 0, 0, 0 },
	{ TAG_DD,	D_BLOCK, 0, 4, 0, 0 },
	{ TAG_DETAILS,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_DFN,	D_INLINE, 0, 0, SA_UNDER, 0 },
	{ TAG_DIALOG,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_DIR,	D_BLOCK, 1, 4, 0, 0 },
	{ TAG_DIV,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_DL,	D_BLOCK, 1, 0, 0, 0 },
	{ TAG_DT,	D_BLOCK, 0, 0, SA_BOLD, 0 },
	{ TAG_EM,	D_INLINE, 0, 0, SA_UNDER, 0 },
	{ TAG_EMBED,	D_NONE, 0, 0, 0, 0 },
	{ TAG_FIELDSET,	D_BLOCK, 1, 0, 0, 0 },
	{ TAG_FIGCAPTION, D_BLOCK, 0, 0, 0, 0 },
	{ TAG_FIGURE,	D_BLOCK, 1, 4, 0, 0 },
	{ TAG_FOOTER,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_FORM,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_FRAMESET,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_H1,	D_BLOCK, 1, 0, SA_BOLD | SA_UNDER, 0 },
	{ TAG_H2,	D_BLOCK, 1, 0, SA_BOLD, 0 },
	{ TAG_H3,	D_BLOCK, 1, 0, SA_BOLD, 0 },
	{ TAG_H4,	D_BLOCK, 1, 0, SA_BOLD, 0 },
	{ TAG_H5,	D_BLOCK, 1, 0, SA_BOLD, 0 },
	{ TAG_H6,	D_BLOCK, 1, 0, SA_BOLD, 0 },
	{ TAG_HEAD,	D_NONE, 0, 0, 0, 0 },
	{ TAG_HEADER,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_HGROUP,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_HR,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_HTML,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_I,	D_INLINE, 0, 0, SA_UNDER, 0 },
	{ TAG_IFRAME,	D_NONE, 0, 0, 0, 0 },
	{ TAG_INS,	D_INLINE, 0, 0, SA_UNDER, 0 },
	{ TAG_LEGEND,	D_BLOCK, 0, 0, SA_BOLD, 0 },
	{ TAG_LI,	D_LIST_ITEM, 0, 0, 0, 0 },
	{ TAG_LINK,	D_NONE, 0, 0, 0, 0 },
	{ TAG_LISTING,	D_BLOCK, 1, 0, 0, 1 },
	{ TAG_MAIN,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_MAP,	D_NONE, 0, 0, 0, 0 },
	{ TAG_MENU,	D_BLOCK, 1, 4, 0, 0 },
	{ TAG_META,	D_NONE, 0, 0, 0, 0 },
	{ TAG_NAV,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_NOEMBED,	D_INLINE, 0, 0, 0, 0 },
	{ TAG_NOFRAMES,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_NOSCRIPT,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_OBJECT,	D_INLINE, 0, 0, 0, 0 },
	{ TAG_OL,	D_BLOCK, 1, 4, 0, 0 },
	{ TAG_OPTGROUP,	D_NONE, 0, 0, 0, 0 },
	{ TAG_OPTION,	D_NONE, 0, 0, 0, 0 },
	{ TAG_P,	D_BLOCK, 1, 0, 0, 0 },
	{ TAG_PARAM,	D_NONE, 0, 0, 0, 0 },
	{ TAG_PLAINTEXT, D_BLOCK, 1, 0, 0, 1 },
	{ TAG_PRE,	D_BLOCK, 1, 0, 0, 1 },
	{ TAG_RP,	D_NONE, 0, 0, 0, 0 },
	{ TAG_SCRIPT,	D_NONE, 0, 0, 0, 0 },
	{ TAG_SEARCH,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_SECTION,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_SOURCE,	D_NONE, 0, 0, 0, 0 },
	{ TAG_STRONG,	D_INLINE, 0, 0, SA_BOLD, 0 },
	{ TAG_STYLE,	D_NONE, 0, 0, 0, 0 },
	{ TAG_SUMMARY,	D_BLOCK, 0, 0, SA_BOLD, 0 },
	{ TAG_TABLE,	D_BLOCK, 1, 0, 0, 0 },
	{ TAG_TBODY,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_TD,	D_TABLE_CELL, 0, 0, 0, 0 },
	{ TAG_TEMPLATE,	D_NONE, 0, 0, 0, 0 },
	{ TAG_TFOOT,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_TH,	D_TABLE_CELL, 0, 0, SA_BOLD, 0 },
	{ TAG_THEAD,	D_BLOCK, 0, 0, 0, 0 },
	{ TAG_TITLE,	D_NONE, 0, 0, 0, 0 },
	{ TAG_TR,	D_TABLE_ROW, 0, 0, 0, 0 },
	{ TAG_TRACK,	D_NONE, 0, 0, 0, 0 },
	{ TAG_U,	D_INLINE, 0, 0, SA_UNDER, 0 },
	{ TAG_UL,	D_BLOCK, 1, 4, 0, 0 },
	{ TAG_VAR,	D_INLINE, 0, 0, SA_UNDER, 0 },
	{ TAG_VIDEO,	D_NONE, 0, 0, 0, 0 },
	{ TAG_XMP,	D_BLOCK, 1, 0, 0, 1 },
};

static struct style table[TAG_COUNT];
static int table_ready;

static void make_table(void)
{
	size_t i;

	memset(table, 0, sizeof table);
	for (i = 0; i < sizeof rules / sizeof rules[0]; i++) {
		struct style *s = &table[rules[i].tag];

		s->display = rules[i].display;
		s->margin = rules[i].margin;
		s->indent = rules[i].indent;
		s->attr = rules[i].attr;
		s->pre = rules[i].pre;
	}
	table_ready = 1;
}

/* does a style="" attribute say display:none or visibility:hidden? */
static int css_hidden(const char *css)
{
	const char *p;

	for (p = css; *p; p++) {
		const char *v;

		if (*p != 'd' && *p != 'v')
			continue;
		if (strncmp(p, "display", 7) == 0)
			v = p + 7;
		else if (strncmp(p, "visibility", 10) == 0)
			v = p + 10;
		else
			continue;
		while (*v == ' ')
			v++;
		if (*v != ':')
			continue;
		v++;
		while (*v == ' ')
			v++;
		if (strncmp(v, "none", 4) == 0 || strncmp(v, "hidden", 6) == 0)
			return 1;
	}
	return 0;
}

void style_for(int tag, const char *hidden, const char *css, const char *id,
	struct style *s)
{
	if (!table_ready)
		make_table();
	*s = table[tag < TAG_COUNT ? tag : 0];
	if (s->display == D_NONE)
		return;
	if (hidden) {
		/* React's streamed content waits hidden in <div hidden
		 * id="S:0"> for a script to move it into place: show it */
		if (!(id && id[0] == 'S' && id[1] == ':'))
			s->display = D_NONE;
	} else if (css && css_hidden(css))
		s->display = D_NONE;
}

void style_of(const struct doc *d, nodeid id, struct style *s)
{
	const struct node *n = DOC_NODE(d, id);

	style_for(n->tag, doc_attr(d, id, ATTR_HIDDEN),
		doc_attr(d, id, ATTR_STYLE), doc_attr(d, id, ATTR_ID), s);
}
