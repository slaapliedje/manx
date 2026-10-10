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
	/* the faces */
	table[TAG_H1].face = 1;
	table[TAG_H2].face = 2;
	table[TAG_H3].face = 3;
	table[TAG_PRE].face = table[TAG_XMP].face = table[TAG_LISTING].face
		= table[TAG_PLAINTEXT].face = table[TAG_CODE].face
		= table[TAG_TT].face = table[TAG_KBD].face
		= table[TAG_SAMP].face = LF_MONO;
	table[TAG_EM].face = table[TAG_I].face = table[TAG_CITE].face
		= table[TAG_VAR].face = table[TAG_DFN].face
		= table[TAG_ADDRESS].face = LF_ITALIC;
	table_ready = 1;
}

/* does [a, b) say word w, in any case? */
static int is_word(const char *a, const char *b, const char *w)
{
	for (; a < b && *w; a++, w++) {
		char c = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;

		if (c != *w)
			return 0;
	}
	return a == b && !*w;
}

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/*
 * Does a style="" attribute hide its element: display:none,
 * visibility:hidden or collapse, content-visibility:hidden (what's inside
 * isn't drawn)? Read as CSS reads it, a declaration at a time: the whole
 * property name, any case (not --x-display, a custom property); a later
 * declaration wins, unless the earlier was !important.
 */
static int css_hidden(const char *css)
{
	static const char *const prop[3] = { "display", "visibility", "content-visibility" };
	int hide[3] = { 0, 0, 0 }, imp[3] = { 0, 0, 0 };
	const char *p = css;

	while (*p) {
		const char *name, *ne, *v, *ve;
		char q = 0;
		int k, depth = 0, important = 0, h;

		while (is_space(*p) || *p == ';')
			p++;
		name = p;
		while (*p && *p != ':' && *p != ';')
			p++;
		for (ne = p; ne > name && is_space(ne[-1]); ne--)
			;
		if (*p != ':')
			continue;
		v = ++p;
		/* the value: to a ';' outside quotes and brackets */
		while (*p && (q || depth || *p != ';')) {
			if (q) {
				if (*p == q)
					q = 0;
			} else if (*p == '"' || *p == '\'')
				q = *p;
			else if (*p == '(')
				depth++;
			else if (*p == ')' && depth)
				depth--;
			p++;
		}
		for (k = 0; k < 3 && !is_word(name, ne, prop[k]); k++)
			;
		if (k == 3)
			continue;
		for (ve = p; ve > v && is_space(ve[-1]); ve--)
			;
		while (v < ve && is_space(*v))
			v++;
		if (ve - v >= 10 && is_word(ve - 10, ve, "!important")) {
			important = 1;
			for (ve -= 10; ve > v && is_space(ve[-1]); ve--)
				;
		}
		if (imp[k] && !important)
			continue;
		h = k == 0 ? is_word(v, ve, "none")
			: is_word(v, ve, "hidden") || (k == 1 && is_word(v, ve, "collapse"));
		hide[k] = h;
		imp[k] = important;
	}
	return hide[0] || hide[1] || hide[2];
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
