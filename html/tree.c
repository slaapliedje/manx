/*
 * tree.c - see tree.h.
 */
#include <string.h>
#include "tags.h"
#include "tree.h"

static int breaks_line(int tag);
static int shows_content(int tag);

void tree_init(struct tree *b, struct doc *d)
{
	memset(b, 0, sizeof *b);
	b->d = d;
	b->stack[0] = 1;		/* the root */
	b->line_start = 1;
	b->stag[0] = TAG_UNKNOWN;
	b->depth = 1;
}

static nodeid current(struct tree *b)
{
	return b->stack[b->depth - 1];
}

static int cur_tag(struct tree *b)
{
	return b->depth > 1 ? b->stag[b->depth - 1] : TAG_UNKNOWN;
}

static void recompute_pre(struct tree *b)
{
	int i;

	b->in_pre = 0;
	for (i = 1; i < b->depth; i++)
		if (tag_flags(b->stag[i]) & TF_PRE)
			b->in_pre = 1;
}

static void push(struct tree *b, nodeid id, int tag)
{
	if (b->depth == TREE_MAX_DEPTH) {
		b->overflow++;		/* its content goes to the current node */
		return;
	}
	b->stack[b->depth] = id;
	b->stag[b->depth] = (unsigned char)tag;
	b->depth++;
	b->nopen[tag]++;
	if (tag_flags(tag) & TF_PRE)
		b->in_pre = 1;
}

/* pop the top n entries */
static void pop_n(struct tree *b, int n)
{
	int pre = 0;

	if (n > b->depth - 1)
		n = b->depth - 1;
	while (n-- > 0) {
		int t = b->stag[--b->depth];

		b->nopen[t]--;
		pre |= tag_flags(t) & TF_PRE;
	}
	if (pre)
		recompute_pre(b);
}

/* the stack index of the innermost `tag`, not looking past a scope
 * boundary (or anywhere if boundary is 0): -1 if not in scope */
static int in_scope(struct tree *b, int tag, unsigned boundary, const int *extra)
{
	int i;

	if (b->nopen[tag] == 0)
		return -1;		/* the usual case, and cheap */

	for (i = b->depth - 1; i >= 1; i--) {
		int t = b->stag[i];

		if (t == tag)
			return i;
		if (boundary && (tag_flags(t) & boundary))
			return -1;
		if (extra) {
			const int *e;

			for (e = extra; *e; e++)
				if (t == *e)
					return -1;
		}
	}
	return -1;
}

static void close_to(struct tree *b, int idx)
{
	if (idx >= 1)
		pop_n(b, b->depth - idx);
}

/* close an open <p> (in "button scope") */
static void close_p(struct tree *b)
{
	static const int button[] = { TAG_BUTTON, 0 };

	close_to(b, in_scope(b, TAG_P, TF_SCOPE, button));
}

/* --- structure ------------------------------------------------------- */

static nodeid add_elem(struct tree *b, nodeid parent, int tag,
	const struct tok_tag *t)
{
	nodeid id = doc_add_elem(b->d, parent, tag);
	int i;

	if (id == 0)
		return 0;
	if (t)
		for (i = 0; i < t->nattr; i++)
			doc_attr_add(b->d, t->attr[i], t->value[i]);
	doc_attr_end(b->d);
	return id;
}

static void ensure_html(struct tree *b)
{
	if (b->html)
		return;
	b->html = add_elem(b, 1, TAG_HTML, NULL);
	push(b, b->html, TAG_HTML);
}

static void ensure_head(struct tree *b)
{
	ensure_html(b);
	if (b->head)
		return;
	b->head = add_elem(b, b->html, TAG_HEAD, NULL);
}

static void ensure_body(struct tree *b)
{
	ensure_head(b);
	if (b->body)
		return;
	b->head_done = 1;
	/* nothing below <html> stays open once the body starts */
	close_to(b, in_scope(b, TAG_HTML, 0, NULL) + 1);
	b->body = add_elem(b, b->html, TAG_BODY, NULL);
	push(b, b->body, TAG_BODY);
}

/* text (or an element) arrives: where does it go? */
static nodeid insertion_point(struct tree *b)
{
	nodeid cur = current(b);

	if (cur == 1 || cur == b->html || (cur == b->head && b->head_done)) {
		ensure_body(b);
		return current(b);
	}
	return cur;
}

/* --- start tags ------------------------------------------------------ */

static void record_head_info(struct tree *b, const struct tok_tag *t)
{
	int i;

	for (i = 0; i < t->nattr; i++) {
		if (t->tag == TAG_BASE && t->attr[i] == ATTR_HREF && !b->base_href[0]
			&& strlen(t->value[i]) < sizeof b->base_href)
			strcpy(b->base_href, t->value[i]);
		if (t->tag == TAG_META && t->attr[i] == ATTR_CHARSET
			&& strlen(t->value[i]) < sizeof b->meta_charset)
			strcpy(b->meta_charset, t->value[i]);
		if (t->tag == TAG_META && t->attr[i] == ATTR_CONTENT) {
			const char *cs = strstr(t->value[i], "charset=");

			if (cs == NULL)
				cs = strstr(t->value[i], "CHARSET=");
			if (cs && !b->meta_charset[0]
				&& strlen(cs + 8) < sizeof b->meta_charset)
				strcpy(b->meta_charset, cs + 8);
		}
	}
}

static void start_tag(struct tree *b, const struct tok_tag *t)
{
	int tag = t->tag, idx;
	unsigned f = tag_flags(tag);
	nodeid parent, id;

	if (tag == TAG_HTML) {
		ensure_html(b);
		return;
	}
	if (tag == TAG_HEAD) {
		if (!b->head && !b->body) {
			ensure_html(b);
			b->head = add_elem(b, b->html, TAG_HEAD, t);
			push(b, b->head, TAG_HEAD);
		}
		return;
	}
	if (tag == TAG_BODY || tag == TAG_FRAMESET) {
		if (!b->body) {
			ensure_head(b);
			b->head_done = 1;
			close_to(b, in_scope(b, TAG_HTML, 0, NULL) + 1);
			b->body = add_elem(b, b->html, TAG_BODY, t);
			push(b, b->body, TAG_BODY);
		}
		return;
	}

	/* head material before the body goes into <head> */
	if ((f & TF_HEAD) && !b->body) {
		if (tag == TAG_BASE || tag == TAG_META)
			record_head_info(b, t);
		ensure_head(b);
		parent = b->head;
		id = add_elem(b, parent, tag, t);
		if (!(f & TF_VOID) && id) {
			/* title/style/script: their text comes next */
			push(b, id, tag);
		}
		return;
	}
	if (tag == TAG_META || tag == TAG_BASE)
		record_head_info(b, t);

	if (!b->body) {
		/* anything else starts the body; an open <head> closes */
		idx = in_scope(b, TAG_HEAD, 0, NULL);
		if (idx >= 1)
			close_to(b, idx);
		ensure_body(b);
	}

	/* implied end tags */
	if (f & TF_BLOCK)
		close_p(b);
	if ((f & TF_HEADING) && (tag_flags(cur_tag(b)) & TF_HEADING))
		pop_n(b, 1);
	switch (tag) {
	case TAG_LI: {
		static const int list[] = { TAG_UL, TAG_OL, TAG_MENU, TAG_DIR, 0 };

		close_to(b, in_scope(b, TAG_LI, TF_SCOPE, list));
		close_p(b);
		break;
	}
	case TAG_DD:
	case TAG_DT: {
		static const int dl[] = { TAG_DL, 0 };
		int i1 = in_scope(b, TAG_DD, TF_SCOPE, dl), i2 = in_scope(b, TAG_DT, TF_SCOPE, dl);

		close_to(b, i1 > i2 ? i1 : i2);
		close_p(b);
		break;
	}
	case TAG_OPTION:
		if (cur_tag(b) == TAG_OPTION)
			pop_n(b, 1);
		break;
	case TAG_OPTGROUP:
		if (cur_tag(b) == TAG_OPTION)
			pop_n(b, 1);
		if (cur_tag(b) == TAG_OPTGROUP)
			pop_n(b, 1);
		break;
	case TAG_A:
		/* no nested links: close the open one (simplified adoption) */
		close_to(b, in_scope(b, TAG_A, TF_SCOPE, NULL));
		break;
	case TAG_NOBR:
		close_to(b, in_scope(b, TAG_NOBR, TF_SCOPE, NULL));
		break;
	case TAG_BUTTON:
		close_to(b, in_scope(b, TAG_BUTTON, TF_SCOPE, NULL));
		break;
	case TAG_SELECT:
		close_to(b, in_scope(b, TAG_SELECT, TF_SCOPE, NULL));
		break;
	case TAG_FORM:
		break;
	case TAG_TR: {
		static const int tbl[] = { TAG_TABLE, 0 };
		int tr = in_scope(b, TAG_TR, 0, tbl);

		if (tr >= 1)
			close_to(b, tr);
		break;
	}
	case TAG_TD:
	case TAG_TH: {
		static const int row[] = { TAG_TR, TAG_TABLE, 0 };
		int c1 = in_scope(b, TAG_TD, 0, row), c2 = in_scope(b, TAG_TH, 0, row);

		close_to(b, c1 > c2 ? c1 : c2);
		/* a cell straight in a table: an implied row */
		if (cur_tag(b) == TAG_TABLE || cur_tag(b) == TAG_TBODY
			|| cur_tag(b) == TAG_THEAD || cur_tag(b) == TAG_TFOOT) {
			id = add_elem(b, current(b), TAG_TR, NULL);
			if (id)
				push(b, id, TAG_TR);
		}
		break;
	}
	case TAG_TBODY:
	case TAG_THEAD:
	case TAG_TFOOT: {
		static const int tbl[] = { TAG_TABLE, 0 };
		int s1 = in_scope(b, TAG_TBODY, 0, tbl), s2 = in_scope(b, TAG_THEAD, 0, tbl),
			s3 = in_scope(b, TAG_TFOOT, 0, tbl), s = s1;

		if (s2 > s) s = s2;
		if (s3 > s) s = s3;
		close_to(b, s);
		break;
	}
	}

	parent = insertion_point(b);
	id = add_elem(b, parent, tag, t);
	if (id == 0)
		return;
	if (f & TF_DROP) {
		/* svg, math, template: keep the element, drop what's inside */
		b->skip_tag = tag;
		b->skip_nest = 1;
		return;
	}
	if (f & TF_VOID)
		return;
	push(b, id, tag);
	if (tag == TAG_PRE || tag == TAG_LISTING || tag == TAG_TEXTAREA)
		b->pre_newline = 1;
}

/* --- end tags -------------------------------------------------------- */

static void end_tag(struct tree *b, const struct tok_tag *t)
{
	int tag = t->tag, idx, i;
	unsigned f = tag_flags(tag);

	switch (tag) {
	case TAG_HEAD:
		idx = in_scope(b, TAG_HEAD, 0, NULL);
		if (idx >= 1)
			close_to(b, idx);
		b->head_done = 1;
		return;
	case TAG_BODY:
	case TAG_HTML:
		/* content after </body> still goes into the body */
		return;
	case TAG_BR: {
		/* </br> is a <br> (HTML5) */
		struct tok_tag br;

		memset(&br, 0, sizeof br);
		br.tag = TAG_BR;
		start_tag(b, &br);
		return;
	}
	case TAG_P:
		close_p(b);
		return;
	}

	if (tag == TAG_UNKNOWN) {
		/* unknown names are all TAG_UNKNOWN: close the innermost one,
		 * but never across a special element */
		for (i = b->depth - 1; i >= 1; i--) {
			if (b->stag[i] == TAG_UNKNOWN) {
				close_to(b, i);
				return;
			}
			if (tag_flags(b->stag[i]) & TF_SPECIAL)
				return;
		}
		return;
	}
	if (f & TF_SPECIAL) {
		/* structural elements: close it if it's in scope */
		idx = in_scope(b, tag, (tag == TAG_TD || tag == TAG_TH
			|| tag == TAG_TR || tag == TAG_TABLE || tag == TAG_CAPTION
			|| tag == TAG_TBODY || tag == TAG_THEAD || tag == TAG_TFOOT)
			? 0 : TF_SCOPE, NULL);
		if (tag == TAG_TR || tag == TAG_TBODY || tag == TAG_THEAD
			|| tag == TAG_TFOOT || tag == TAG_TD || tag == TAG_TH) {
			/* don't close past the table they belong to */
			static const int tbl[] = { TAG_TABLE, 0 };

			idx = in_scope(b, tag, 0, tbl);
		}
		if (idx >= 1)
			close_to(b, idx);
		return;
	}
	/* formatting and phrasing elements: the innermost one of that name,
	 * unless a special element is in the way (then the end tag is
	 * ignored, as in HTML5's "any other end tag") */
	for (i = b->depth - 1; i >= 1; i--) {
		if (b->stag[i] == tag) {
			close_to(b, i);
			return;
		}
		if (tag_flags(b->stag[i]) & TF_SPECIAL)
			return;
	}
}

void tree_tag(void *ctx, const struct tok_tag *t)
{
	struct tree *b = ctx;

	if (!b->skip_tag && breaks_line(t->tag))
		b->line_start = 1;
	else if (!b->skip_tag && shows_content(t->tag))
		b->line_start = 0;	/* like text: a space after it counts */

	if (b->skip_tag) {
		if (t->tag == b->skip_tag) {
			if (t->end) {
				if (--b->skip_nest == 0)
					b->skip_tag = 0;
			} else if (!t->self_closing)
				b->skip_nest++;
		}
		return;
	}
	b->pre_newline = 0;
	if (t->end) {
		/* an end tag with nothing open to close in this subtree */
		end_tag(b, t);
		return;
	}
	/* past TREE_MAX_DEPTH, push() declines and content flattens */
	start_tag(b, t);
}

/* --- text ------------------------------------------------------------ */

static int all_space(const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (s[i] != ' ' && s[i] != '\n' && s[i] != '\t' && s[i] != '\f')
			return 0;
	return 1;
}

/* does this element start or end a line? (then white space next to it
 * is not worth a space) */
static int breaks_line(int tag)
{
	if (tag_flags(tag) & TF_BLOCK)
		return 1;
	switch (tag) {
	case TAG_LI: case TAG_DD: case TAG_DT: case TAG_TD: case TAG_TH:
	case TAG_TR: case TAG_TBODY: case TAG_THEAD: case TAG_TFOOT:
	case TAG_CAPTION: case TAG_BR: case TAG_BODY: case TAG_HTML:
	case TAG_HEAD: case TAG_TITLE: case TAG_OPTION: case TAG_LEGEND:
		return 1;
	}
	return 0;
}

/* inline elements the layout shows something for (an image's alt, a form
 * field): white space next to them separates them like text does */
static int shows_content(int tag)
{
	switch (tag) {
	case TAG_IMG: case TAG_IMAGE: case TAG_INPUT: case TAG_SELECT:
	case TAG_TEXTAREA: case TAG_BUTTON:
		return 1;
	}
	return 0;
}

void tree_text(void *ctx, const char *s, size_t n)
{
	struct tree *b = ctx;
	nodeid where;
	size_t i;

	if (b->skip_tag || n == 0)
		return;
	if (b->pre_newline) {
		/* a newline right after <pre> is not content */
		b->pre_newline = 0;
		if (*s == '\n') {
			s++;
			if (--n == 0)
				return;
		}
	}
	if (!b->body) {
		/* white space in the head: nothing */
		if (all_space(s, n) || cur_tag(b) == TAG_TITLE
			|| cur_tag(b) == TAG_STYLE || cur_tag(b) == TAG_SCRIPT) {
			if (cur_tag(b) == TAG_TITLE)
				doc_add_text(b->d, current(b), s, n);
			return;
		}
	}
	where = insertion_point(b);
	if (b->in_pre) {
		doc_add_text(b->d, where, s, n);
		return;
	}
	/* white space inside element-only containers is layout noise */
	if ((tag_flags(cur_tag(b)) & TF_ELEMONLY) && all_space(s, n))
		return;

	/*
	 * Collapse each run of white space to one space, and drop it where
	 * it can't matter: after a line break (a block, a cell, <br>...) or
	 * after text that already ends in a space. line_start tracks that
	 * across elements. (Spaces before a line end are the layout's.)
	 */
	{
		/* straight into the document's text pool: collapsing only ever
		 * shrinks, so n bytes are enough */
		char *out = doc_text_reserve(b->d, where, n), *o;
		int ls = b->line_start;

		if (out == NULL)
			return;
		o = out;
		for (i = 0; i < n; i++) {
			char c = s[i];

			if (c == ' ' || c == '\n' || c == '\t' || c == '\f') {
				if (!ls) {
					*o++ = ' ';
					ls = 1;
				}
			} else {
				*o++ = c;
				ls = 0;
			}
		}
		b->line_start = ls;
		doc_text_commit(b->d, (size_t)(o - out));
	}
}

void tree_end(struct tree *b)
{
	ensure_body(b);
}
