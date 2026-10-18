/*
 * tree.c - see tree.h.
 */
#include <string.h>
#include "os.h"
#include "tags.h"
#include "css.h"
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

/* --- formatting elements --------------------------------------------- */

static int is_fmt(int tag)
{
	switch (tag) {
	case TAG_A: case TAG_B: case TAG_BIG: case TAG_CODE: case TAG_EM:
	case TAG_FONT: case TAG_I: case TAG_NOBR: case TAG_S: case TAG_SMALL:
	case TAG_STRIKE: case TAG_STRONG: case TAG_TT: case TAG_U:
		return 1;
	}
	return 0;
}

/* elements whose content formatting from outside doesn't reach */
static int is_fmt_marker(int tag)
{
	return tag == TAG_TD || tag == TAG_TH || tag == TAG_CAPTION
		|| tag == TAG_APPLET || tag == TAG_OBJECT || tag == TAG_MARQUEE
		|| tag == TAG_TEMPLATE;
}

static void fmt_remove(struct tree *b, int i)
{
	if (b->fmt[i].tag && !b->fmt[i].open)
		b->fmt_closed--;
	memmove(&b->fmt[i], &b->fmt[i + 1], (b->nfmt - i - 1) * sizeof b->fmt[0]);
	b->nfmt--;
}

static void fmt_add(struct tree *b, nodeid id, int tag)
{
	if (b->nfmt == TREE_FMT_MAX) {
		int i;

		for (i = 0; i < b->nfmt && b->fmt[i].tag == 0; i++)
			;
		if (i == b->nfmt)
			return;
		fmt_remove(b, i);	/* the oldest is forgotten */
	}
	b->fmt[b->nfmt].node = id;
	b->fmt[b->nfmt].tag = (unsigned char)tag;
	b->fmt[b->nfmt].open = 1;
	b->nfmt++;
}

/* the entry of tag after the last marker (-1: none) */
static int fmt_find(struct tree *b, int tag)
{
	int i;

	for (i = b->nfmt - 1; i >= 0 && b->fmt[i].tag; i--)
		if (b->fmt[i].tag == tag)
			return i;
	return -1;
}

/* element id leaves the stack */
static void fmt_popped(struct tree *b, nodeid id, int tag)
{
	int i;

	if (is_fmt(tag)) {
		for (i = b->nfmt - 1; i >= 0; i--)
			if (b->fmt[i].node == id && b->fmt[i].open) {
				b->fmt[i].open = 0;
				b->fmt_closed++;
				break;
			}
	} else if (is_fmt_marker(tag)) {
		for (i = b->nfmt - 1; i >= 0; i--)
			if (b->fmt[i].tag == 0 && b->fmt[i].node == id) {
				while (b->nfmt > i)
					fmt_remove(b, b->nfmt - 1);
				break;
			}
	}
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
		if (b->nfmt)
			fmt_popped(b, b->stack[b->depth], t);
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

/* --- tables --------------------------------------------------------- */

/* where rows and cells go: what's not a cell's content is fostered */
static int table_ctx(int tag)
{
	return tag == TAG_TABLE || tag == TAG_TBODY || tag == TAG_THEAD
		|| tag == TAG_TFOOT || tag == TAG_TR;
}

static int table_part(int tag)
{
	return tag == TAG_CAPTION || tag == TAG_COLGROUP || tag == TAG_COL
		|| tag == TAG_TBODY || tag == TAG_THEAD || tag == TAG_TFOOT
		|| tag == TAG_TR || tag == TAG_TD || tag == TAG_TH;
}

/* a part of the table: what was fostered out of it and is still open
 * closes (unless this is in a cell or caption: their rules then) */
static void clear_to_table(struct tree *b)
{
	int i;

	for (i = b->depth - 1; i >= 1; i--) {
		int t = b->stag[i];

		if (t == TAG_TD || t == TAG_TH || t == TAG_CAPTION
			|| t == TAG_BODY || t == TAG_HTML)
			return;
		if (table_ctx(t)) {
			close_to(b, i + 1);
			return;
		}
	}
}

/* foster parenting: content in a table but in no cell goes just before
 * the table (returned in *before), in the table's parent */
static nodeid foster_parent(struct tree *b, nodeid *before)
{
	int i;

	for (i = b->depth - 1; i >= 2; i--)
		if (b->stag[i] == TAG_TABLE) {
			*before = b->stack[i];
			return b->stack[i - 1];
		}
	*before = 0;
	return current(b);
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

static void push_new(struct tree *b, int tag)
{
	nodeid id = add_elem(b, current(b), tag, NULL);

	if (id)
		push(b, id, tag);
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

/* an attribute's value is word, in any case */
static int value_is(const char *v, const char *word)
{
	for (; *word; v++, word++)
		if ((*v >= 'A' && *v <= 'Z' ? *v + 32 : *v) != *word)
			return 0;
	return *v == '\0';
}

static void record_head_info(struct tree *b, const struct tok_tag *t)
{
	const char *equiv = NULL, *content = NULL;
	int i;

	for (i = 0; i < t->nattr; i++) {
		if (t->attr[i] == ATTR_HTTP_EQUIV)
			equiv = t->value[i];
		if (t->attr[i] == ATTR_CONTENT)
			content = t->value[i];
	}
	if (t->tag == TAG_META && equiv && content && !b->meta_refresh[0]
		&& value_is(equiv, "refresh")
		&& strlen(content) < sizeof b->meta_refresh)
		strcpy(b->meta_refresh, content);
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

/* a copy of element old (its attributes too) as parent's last child */
static nodeid copy_elem(struct tree *b, nodeid parent, nodeid old)
{
	struct doc *d = b->d;
	nodeid id = doc_add_elem(d, parent, d->nodes[old].tag);
	unsigned long off = d->nodes[old].data;

	if (id == 0)
		return 0;
	d->nodes[id].flags = NF_COPY;
	while (off < d->attr_len && d->attr[off]) {
		int a = d->attr[off];
		size_t n = strlen((const char *)d->attr + off + 1);
		char *v = xmalloc(n + 1);

		/* (copied out: adding may move the pool) */
		if (v) {
			memcpy(v, d->attr + off + 1, n + 1);
			doc_attr_add(d, a, v);
			xfree(v);
		}
		off += n + 2;
	}
	doc_attr_end(d);
	return id;
}

/* HTML5's reconstruction of the active formatting elements: those a
 * block closed open again, in order, where content goes next */
static void fmt_reopen(struct tree *b)
{
	int i, k;

	if (!b->fmt_closed || table_ctx(cur_tag(b)))
		return;
	for (k = b->nfmt - 1; k >= 0 && b->fmt[k].tag && !b->fmt[k].open; k--)
		;
	for (i = k + 1; i < b->nfmt; i++) {
		nodeid id = copy_elem(b, insertion_point(b), b->fmt[i].node);

		if (id == 0)
			return;
		push(b, id, b->fmt[i].tag);
		b->fmt[i].node = id;
		b->fmt[i].open = 1;
		b->fmt_closed--;
	}
}

/* --- the adoption agency (HTML5): a formatting element's end tag after
 * a block began in it (<a>1<p>2</a>3</p>: the block moves out, and gets
 * a copy of the element around what it had) ------------------------ */

static void stack_remove(struct tree *b, int i)
{
	int t = b->stag[i];

	memmove(&b->stack[i], &b->stack[i + 1], (b->depth - i - 1) * sizeof b->stack[0]);
	memmove(&b->stag[i], &b->stag[i + 1], (size_t)(b->depth - i - 1));
	b->depth--;
	b->nopen[t]--;
	if (tag_flags(t) & TF_PRE)
		recompute_pre(b);
}

static void stack_insert(struct tree *b, int i, nodeid id, int tag)
{
	if (b->depth == TREE_MAX_DEPTH)
		return;
	memmove(&b->stack[i + 1], &b->stack[i], (b->depth - i) * sizeof b->stack[0]);
	memmove(&b->stag[i + 1], &b->stag[i], (size_t)(b->depth - i));
	b->stack[i] = id;
	b->stag[i] = (unsigned char)tag;
	b->depth++;
	b->nopen[tag]++;
	if (tag_flags(tag) & TF_PRE)
		b->in_pre = 1;
}

static int fmt_index(struct tree *b, nodeid id)
{
	int i;

	for (i = b->nfmt - 1; i >= 0; i--)
		if (b->fmt[i].tag && b->fmt[i].node == id)
			return i;
	return -1;
}

/* 1 if the end tag was dealt with, 0: by the usual rules */
static int adopt(struct tree *b, int tag)
{
	int outer;

	for (outer = 0; outer < 8; outer++) {
		int fe = fmt_find(b, tag), fi, fbi, i, bm, inner = 0;
		nodeid f, fb, ca, last, nf;

		if (fe < 0)
			return outer > 0;
		f = b->fmt[fe].node;
		for (fi = b->depth - 1; fi >= 1 && b->stack[fi] != f; fi--)
			;
		if (fi < 1) {
			fmt_remove(b, fe);	/* not open: forgotten */
			return 1;
		}
		for (i = fi + 1; i < b->depth; i++)
			if (tag_flags(b->stag[i]) & TF_SCOPE)
				return 1;	/* not in scope: ignored */
		for (fbi = fi + 1; fbi < b->depth
			&& !(tag_flags(b->stag[fbi]) & TF_SPECIAL); fbi++)
			;
		if (fbi == b->depth) {
			/* no block in it: it simply closes */
			close_to(b, fi);
			if ((i = fmt_index(b, f)) >= 0)
				fmt_remove(b, i);
			return 1;
		}
		fb = b->stack[fbi];
		ca = b->stack[fi - 1];
		bm = fe;
		last = fb;
		/* the formatting elements between it and the block: copied
		 * around the block (others leave the stack) */
		for (i = fbi; ; ) {
			int ne;

			inner++;
			i--;
			if (b->stack[i] == f)
				break;
			ne = fmt_index(b, b->stack[i]);
			if (inner > 3 && ne >= 0) {
				fmt_remove(b, ne);
				if (ne < bm)
					bm--;
				ne = -1;
			}
			if (ne < 0) {
				stack_remove(b, i);
				fbi--;
				continue;
			}
			if ((nf = copy_elem(b, ca, b->stack[i])) == 0)
				return 1;
			b->fmt[ne].node = nf;
			b->stack[i] = nf;
			if (last == fb)
				bm = ne + 1;
			doc_reparent(b->d, last, nf);
			last = nf;
		}
		doc_reparent(b->d, last, ca);
		/* a copy of the element takes the block's content */
		if ((nf = copy_elem(b, ca, f)) == 0)
			return 1;
		doc_move_children(b->d, fb, nf);
		doc_reparent(b->d, nf, fb);
		if ((fe = fmt_index(b, f)) >= 0) {
			fmt_remove(b, fe);
			if (fe < bm)
				bm--;
		}
		if (b->nfmt < TREE_FMT_MAX) {
			memmove(&b->fmt[bm + 1], &b->fmt[bm], (b->nfmt - bm) * sizeof b->fmt[0]);
			b->fmt[bm].node = nf;
			b->fmt[bm].tag = (unsigned char)tag;
			b->fmt[bm].open = 1;
			b->nfmt++;
		}
		for (fi = b->depth - 1; fi >= 1 && b->stack[fi] != f; fi--)
			;
		if (fi >= 1) {
			stack_remove(b, fi);
			if (fi < fbi)
				fbi--;
		}
		/* (just above the block: the spec's stack is upside down) */
		stack_insert(b, fbi + 1, nf, tag);
	}
	return 1;
}

/* In a page of frames only frames count: <frameset>, <frame>, and
 * <noframes> (its text is dropped); the rest is ignored. */
static void frames_tag(struct tree *b, const struct tok_tag *t)
{
	nodeid id;

	if (t->end) {
		if (t->tag == TAG_FRAMESET && !b->frameset_done
			&& cur_tag(b) == TAG_FRAMESET) {
			pop_n(b, 1);
			if (cur_tag(b) != TAG_FRAMESET)
				b->frameset_done = 1;
		} else if (t->tag == TAG_NOFRAMES && cur_tag(b) == TAG_NOFRAMES)
			pop_n(b, 1);
		return;
	}
	if (cur_tag(b) == TAG_NOFRAMES
		|| (b->frameset_done && t->tag != TAG_NOFRAMES))
		return;
	switch (t->tag) {
	case TAG_FRAMESET:
	case TAG_FRAME:
	case TAG_NOFRAMES:
		id = add_elem(b, current(b), t->tag, t);
		if (id && t->tag != TAG_FRAME)
			push(b, id, t->tag);
		break;
	}
}

/* content in the body that makes it too late for a <frameset> */
static int ends_frameset_ok(const struct tok_tag *t)
{
	int i;

	switch (t->tag) {
	case TAG_PRE: case TAG_LISTING: case TAG_LI: case TAG_DD: case TAG_DT:
	case TAG_BUTTON: case TAG_APPLET: case TAG_MARQUEE: case TAG_OBJECT:
	case TAG_TABLE: case TAG_AREA: case TAG_BR: case TAG_EMBED:
	case TAG_IMG: case TAG_IMAGE: case TAG_WBR: case TAG_HR:
	case TAG_TEXTAREA: case TAG_XMP: case TAG_IFRAME: case TAG_SELECT:
		return 1;
	case TAG_INPUT:
		for (i = 0; i < t->nattr; i++)
			if (t->attr[i] == ATTR_TYPE && value_is(t->value[i], "hidden"))
				return 0;
		return 1;
	}
	return 0;
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
	if (b->frameset) {
		frames_tag(b, t);
		return;
	}
	if (tag == TAG_FRAMESET) {
		/* a page of frames: the <frameset> takes the body's place,
		 * even an empty body's */
		if (b->body && (b->no_frameset || b->depth < 3
			|| b->stack[2] != b->body))
			return;
		ensure_head(b);
		b->head_done = 1;
		close_to(b, in_scope(b, TAG_HTML, 0, NULL) + 1);
		if (b->body) {
			doc_unlink(b->d, b->body);
			b->body = 0;
		}
		b->frameset = add_elem(b, b->html, TAG_FRAMESET, t);
		if (b->frameset)
			push(b, b->frameset, TAG_FRAMESET);
		return;
	}
	if (tag == TAG_FRAME)
		return;			/* only in a <frameset> */
	if (tag == TAG_BODY) {
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

	if (ends_frameset_ok(t))
		b->no_frameset = 1;

	/* tables: their parts in order, a table in a table's rows ends it */
	if (table_part(tag) && b->nopen[TAG_TABLE])
		clear_to_table(b);
	if (tag == TAG_TABLE && table_ctx(cur_tag(b)))
		close_to(b, in_scope(b, TAG_TABLE, 0, NULL));
	if ((tag == TAG_CAPTION || tag == TAG_COLGROUP) && table_ctx(cur_tag(b)))
		close_to(b, in_scope(b, TAG_TABLE, 0, NULL) + 1);
	if (tag == TAG_COL && cur_tag(b) == TAG_TABLE)
		push_new(b, TAG_COLGROUP);

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
	case TAG_NOBR:
		/* no nested links: close the open one (simplified adoption) */
		close_to(b, in_scope(b, tag, TF_SCOPE, NULL));
		if ((idx = fmt_find(b, tag)) >= 0)
			fmt_remove(b, idx);
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
		if (cur_tag(b) == TAG_TABLE)
			push_new(b, TAG_TBODY);
		break;
	}
	case TAG_TD:
	case TAG_TH: {
		static const int row[] = { TAG_TR, TAG_TABLE, 0 };
		int c1 = in_scope(b, TAG_TD, 0, row), c2 = in_scope(b, TAG_TH, 0, row);

		close_to(b, c1 > c2 ? c1 : c2);
		/* a cell straight in a table: an implied body and row */
		if (cur_tag(b) == TAG_TABLE)
			push_new(b, TAG_TBODY);
		if (cur_tag(b) == TAG_TBODY || cur_tag(b) == TAG_THEAD
			|| cur_tag(b) == TAG_TFOOT)
			push_new(b, TAG_TR);
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

	if (!(f & (TF_BLOCK | TF_HEAD)) && !table_part(tag) && tag != TAG_LI
		&& tag != TAG_DD && tag != TAG_DT && tag != TAG_OPTION
		&& tag != TAG_OPTGROUP && tag != TAG_BASEFONT)
		fmt_reopen(b);
	parent = insertion_point(b);
	if (table_ctx(cur_tag(b)) && !table_part(tag) && tag != TAG_TABLE
		&& tag != TAG_SCRIPT && tag != TAG_STYLE && tag != TAG_TEMPLATE
		&& tag != TAG_FORM && !(tag == TAG_INPUT && !ends_frameset_ok(t))) {
		nodeid before;

		parent = foster_parent(b, &before);
		id = add_elem(b, parent, tag, t);
		if (id && before)
			doc_move_before(b->d, id, before);
	} else
		id = add_elem(b, parent, tag, t);
	if (id == 0)
		return;
	if (f & TF_DROP) {
		/* svg, math, template: keep the element, drop what's inside
		 * (<svg/> and <math/> have nothing inside: foreign elements
		 * close themselves; <template/> doesn't, as in browsers) */
		if (t->self_closing && tag != TAG_TEMPLATE)
			return;
		b->skip_tag = tag;
		b->skip_html = 0;
		b->skip_nest = 1;
		return;
	}
	if (f & TF_VOID)
		return;
	push(b, id, tag);
	if (b->depth > 0 && b->stack[b->depth - 1] == id) {
		if (is_fmt(tag))
			fmt_add(b, id, tag);
		else if (is_fmt_marker(tag))
			fmt_add(b, id, 0);
	}
	if (tag == TAG_PRE || tag == TAG_LISTING || tag == TAG_TEXTAREA)
		b->pre_newline = 1;
}

/* --- end tags -------------------------------------------------------- */

static void end_tag(struct tree *b, const struct tok_tag *t)
{
	int tag = t->tag, idx, i;
	unsigned f = tag_flags(tag);

	if (b->frameset) {
		frames_tag(b, t);
		return;
	}
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
	if (is_fmt(tag) && b->nopen[TAG_SELECT])
		return;			/* (a list's options have no formatting) */
	if (is_fmt(tag) && b->nfmt && adopt(b, tag))
		return;
	/* formatting and phrasing elements: the innermost one of that name,
	 * unless a special element is in the way (then the end tag is
	 * ignored, as in HTML5's "any other end tag") */
	for (i = b->depth - 1; i >= 1; i--) {
		if (b->stag[i] == tag) {
			/* (formatting closed with it stays active: <b>1<i>2</b>3
			 * has 3 in an <i> again) */
			nodeid id = b->stack[i];

			close_to(b, i);
			for (i = b->nfmt - 1; i >= 0; i--)
				if (b->fmt[i].node == id) {
					fmt_remove(b, i);
					break;
				}
			return;
		}
		if (tag_flags(b->stag[i]) & TF_SPECIAL)
			break;
	}
	/* not open here: its entry goes (<p><b>x</p>y</b>: y isn't bold) */
	if (is_fmt(tag) && (i = fmt_find(b, tag)) >= 0 && !b->fmt[i].open)
		fmt_remove(b, i);
}

/* <style> and </style>: its text goes to the page's style sheet */
static void style_tag(struct tree *b, const struct tok_tag *t)
{
	const char *media = NULL;
	int i;

	if (t->end) {
		if (b->in_style)
			css_end(b->d->sheet);
		b->in_style = 0;
		return;
	}
	if (b->d->sheet == NULL && (b->d->sheet = css_new()) == NULL)
		return;
	for (i = 0; i < t->nattr; i++)
		if (t->attr[i] == ATTR_MEDIA)
			media = t->value[i];
	/* (its node will be the next one: in the page's order) */
	css_begin(b->d->sheet, media, (nodeid)b->d->nnodes);
	b->in_style = 1;
}

void tree_style(void *ctx, const char *s, size_t n)
{
	struct tree *b = ctx;

	if (b->in_style)
		css_feed(b->d->sheet, s, n);
}

/* Inside <svg> or <math>, HTML that ends them (HTML5's breakout from
 * foreign content): an end tag of an element open outside, or one of the
 * start tags no SVG or MathML has. */
/* SVG's and MathML's elements whose content is HTML (HTML5's
 * integration points; not annotation-xml, whose encoding isn't kept) */
static int html_inside(const struct tok_tag *t)
{
	static const char *const names[] = { "foreignobject", "desc", "title",
		"mi", "mo", "mn", "ms", "mtext", NULL };
	int i;

	for (i = 0; names[i]; i++)
		if (t->name && strcmp(t->name, names[i]) == 0)
			return 1;
	return 0;
}

static int breaks_out(struct tree *b, const struct tok_tag *t)
{
	int i;

	if ((b->skip_tag != TAG_SVG && b->skip_tag != TAG_MATH) || b->skip_html)
		return 0;
	if (t->end)
		return t->tag != TAG_UNKNOWN && (b->nopen[t->tag] || t->tag == TAG_P
			|| t->tag == TAG_BR);
	switch (t->tag) {
	case TAG_B: case TAG_BIG: case TAG_BLOCKQUOTE: case TAG_BODY:
	case TAG_BR: case TAG_CENTER: case TAG_CODE: case TAG_DD: case TAG_DIV:
	case TAG_DL: case TAG_DT: case TAG_EM: case TAG_EMBED: case TAG_H1:
	case TAG_H2: case TAG_H3: case TAG_H4: case TAG_H5: case TAG_H6:
	case TAG_HEAD: case TAG_HR: case TAG_I: case TAG_IMG: case TAG_LI:
	case TAG_LISTING: case TAG_MENU: case TAG_META: case TAG_NOBR:
	case TAG_OL: case TAG_P: case TAG_PRE: case TAG_RUBY: case TAG_S:
	case TAG_SMALL: case TAG_SPAN: case TAG_STRONG: case TAG_STRIKE:
	case TAG_SUB: case TAG_SUP: case TAG_TABLE: case TAG_TT: case TAG_U:
	case TAG_UL: case TAG_VAR:
		return 1;
	case TAG_FONT:
		for (i = 0; i < t->nattr; i++)
			if (t->attr[i] == ATTR_COLOR || t->attr[i] == ATTR_FACE
				|| t->attr[i] == ATTR_SIZE)
				return 1;
		return 0;
	}
	return 0;
}

void tree_tag(void *ctx, const struct tok_tag *t)
{
	struct tree *b = ctx;

	if (b->skip_tag) {
		if (t->tag == b->skip_tag) {
			if (t->end) {
				if (--b->skip_nest == 0)
					b->skip_tag = 0;
			} else if (!t->self_closing)
				b->skip_nest++;
			return;
		}
		if (html_inside(t) && !t->self_closing) {
			if (!t->end)
				b->skip_html++;
			else if (b->skip_html)
				b->skip_html--;
			return;
		}
		if (!breaks_out(b, t))
			return;
		b->skip_tag = 0;	/* an <svg> or <math> left open ends */
		b->skip_html = 0;
	}
	if (breaks_line(t->tag))
		b->line_start = 1;
	else if (shows_content(t->tag))
		b->line_start = 0;	/* like text: a space after it counts */
	b->pre_newline = 0;
	if (t->tag == TAG_STYLE)
		style_tag(b, t);
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
	nodeid where, before = 0;
	size_t i;

	if (b->skip_tag || n == 0 || b->frameset)
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
	if (b->fmt_closed && !all_space(s, n))
		fmt_reopen(b);
	where = insertion_point(b);
	if (!b->no_frameset && !all_space(s, n))
		b->no_frameset = 1;
	if (table_ctx(cur_tag(b)) && !all_space(s, n))
		where = foster_parent(b, &before);
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
		if (before && b->d->nodes[where].last != before)
			doc_move_before(b->d, b->d->nodes[where].last, before);
	}
}

void tree_end(struct tree *b)
{
	if (b->in_style) {		/* (a <style> never closed) */
		css_end(b->d->sheet);
		b->in_style = 0;
	}
	if (!b->frameset)
		ensure_body(b);
}
