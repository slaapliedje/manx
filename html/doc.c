/*
 * doc.c - see doc.h.
 */
#include <stdio.h>
#include <string.h>
#include "os.h"
#include "tags.h"
#include "css.h"
#include "doc.h"

int doc_init(struct doc *d, size_t byte_cap)
{
	memset(d, 0, sizeof *d);
	d->byte_cap = byte_cap ? byte_cap : DOC_DEFAULT_CAP;
	/* small to start with: pages grow them, and a cap must hold them */
	d->cap_nodes = 64;
	d->nodes = xmalloc(d->cap_nodes * sizeof *d->nodes);
	d->text_cap = 1024;
	d->text = xmalloc(d->text_cap);
	d->attr_cap = 256;
	d->attr = xmalloc(d->attr_cap);
	if (!d->nodes || !d->text || !d->attr) {
		doc_free(d);
		return -1;
	}
	memset(&d->nodes[0], 0, 2 * sizeof *d->nodes);
	d->nodes[1].type = NODE_ROOT;
	d->nnodes = 2;
	d->text[0] = '\0';
	d->text_len = 1;		/* offset 0 = "" */
	d->attr[0] = 0;
	d->attr_len = 1;		/* offset 0 = no attributes */
	return 0;
}

void doc_free(struct doc *d)
{
	css_free(d->sheet);
	xfree(d->nodes);
	xfree(d->text);
	xfree(d->attr);
	memset(d, 0, sizeof *d);
}

size_t doc_bytes(const struct doc *d)
{
	return d->cap_nodes * sizeof *d->nodes + d->text_cap + d->attr_cap;
}

/* grow one of the pools to hold `need` units, within the byte cap */
static int grow(struct doc *d, void **p, unsigned long *cap, unsigned long need,
	size_t unit)
{
	unsigned long ncap = *cap;
	void *np;
	size_t other;

	if (need <= *cap)
		return 0;
	while (ncap < need)
		ncap *= 2;
	other = doc_bytes(d) - *cap * unit;
	if (other + ncap * unit > d->byte_cap) {
		/* the doubling doesn't fit: take exactly what's left */
		if (other + need * unit > d->byte_cap) {
			d->truncated = 1;
			return -1;
		}
		ncap = (unsigned long)((d->byte_cap - other) / unit);
	}
	np = xrealloc(*p, ncap * unit);
	if (np == NULL) {
		d->truncated = 1;
		return -1;
	}
	*p = np;
	*cap = ncap;
	return 0;
}

static nodeid new_node(struct doc *d, nodeid parent, int type)
{
	struct node *n, *p;
	nodeid id;

	if (d->truncated || d->nnodes > DOC_MAX_NODES - 1) {
		d->truncated = 1;
		return 0;
	}
	if (grow(d, (void **)&d->nodes, &d->cap_nodes, d->nnodes + 1,
		sizeof *d->nodes) < 0)
		return 0;
	id = (nodeid)d->nnodes++;
	n = &d->nodes[id];
	memset(n, 0, sizeof *n);
	n->type = (unsigned char)type;
	n->parent = parent;
	p = &d->nodes[parent];
	if (p->last)
		d->nodes[p->last].next = id;
	else
		p->first = id;
	p->last = id;
	return id;
}

nodeid doc_add_elem(struct doc *d, nodeid parent, int tag)
{
	nodeid id = new_node(d, parent, NODE_ELEM);

	if (id == 0)
		return 0;
	d->nodes[id].tag = (unsigned char)tag;
	/* the attribute list starts here; doc_attr_end closes it */
	if (grow(d, (void **)&d->attr, &d->attr_cap, d->attr_len + 1, 1) < 0) {
		d->nodes[id].data = 0;
		return id;
	}
	d->nodes[id].data = d->attr_len;
	return id;
}

void doc_unlink(struct doc *d, nodeid id)
{
	struct node *n = &d->nodes[id];
	struct node *p = &d->nodes[n->parent];
	nodeid k, prev = 0;

	for (k = p->first; k && k != id; k = d->nodes[k].next)
		prev = k;
	if (k == 0)
		return;
	if (prev)
		d->nodes[prev].next = n->next;
	else
		p->first = n->next;
	if (p->last == id)
		p->last = prev;
	n->next = 0;
	d->last_text = 0;		/* text may not join what's gone */
}

void doc_move_before(struct doc *d, nodeid id, nodeid ref)
{
	nodeid parent = d->nodes[ref].parent, k, prev = 0;
	nodeid keep = d->last_text;

	if (d->nodes[id].parent != parent || id == ref)
		return;
	doc_unlink(d, id);
	d->last_text = keep;
	for (k = d->nodes[parent].first; k && k != ref; k = d->nodes[k].next)
		prev = k;
	if (k == 0)
		return;
	d->nodes[id].next = ref;
	if (prev)
		d->nodes[prev].next = id;
	else
		d->nodes[parent].first = id;
}

static int is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* a picture this can show (not by its name WebP or AVIF) */
static int showable(const char *u, const char *ue)
{
	const char *q = u;

	while (q < ue && *q != '?' && *q != '#')
		q++;
	if (q - u >= 5) {
		char t[6];
		int i;

		for (i = 0; i < 5; i++)
			t[i] = (char)(q[i - 5] >= 'A' && q[i - 5] <= 'Z' ? q[i - 5] + 32 : q[i - 5]);
		t[5] = '\0';
		if (strcmp(t, ".webp") == 0 || strcmp(t, ".avif") == 0)
			return 0;
	}
	return 1;
}

const char *doc_srcset_pick(const char *ss, int target, size_t *len)
{
	const char *p = ss, *bw = NULL, *wide = NULL, *bx = NULL;
	size_t bw_n = 0, wide_n = 0, bx_n = 0;
	long bw_v = 0, wide_v = 0, bx_v = 0;

	while (*p) {
		const char *u, *ue, *q;
		long v = 0, frac = 0;
		int fd = 0;

		while (is_ws(*p) || *p == ',')
			p++;
		if (*p == '\0')
			break;
		u = p;
		while (*p && !is_ws(*p))
			p++;
		ue = p;
		if (ue[-1] == ',') {
			/* (commas right after the URL: no descriptor) */
			while (ue > u && ue[-1] == ',')
				ue--;
			q = ue;
		} else {
			while (is_ws(*p))
				p++;
			q = p;
			while (*p && *p != ',')
				p++;
		}
		if (ue == u || !showable(u, ue))
			continue;
		while (*q >= '0' && *q <= '9' && v < 100000)
			v = v * 10 + (*q++ - '0');
		if (*q == '.')
			for (q++; *q >= '0' && *q <= '9'; q++)
				if (fd < 3) {
					frac = frac * 10 + (*q - '0');
					fd++;
				}
		while (fd < 3) {
			frac *= 10;
			fd++;
		}
		if (*q == 'w' && v > 0) {
			if (v >= target && (bw == NULL || v < bw_v)) {
				bw = u;
				bw_n = (size_t)(ue - u);
				bw_v = v;
			}
			if (wide == NULL || v > wide_v) {
				wide = u;
				wide_n = (size_t)(ue - u);
				wide_v = v;
			}
		} else {
			long x = *q == 'x' ? v * 1000 + frac : 1000;

			if (bx == NULL || x < bx_v) {
				bx = u;
				bx_n = (size_t)(ue - u);
				bx_v = x;
			}
		}
	}
	if (bw) {
		*len = bw_n;
		return bw;
	}
	if (wide) {
		*len = wide_n;
		return wide;
	}
	*len = bx_n;
	return bx;
}

void doc_attr_add(struct doc *d, int attr, const char *value)
{
	size_t n = strlen(value);

	if (d->truncated)
		return;
	/* pictures' choices: none kept where none are shown; of a srcset
	 * the one candidate fetched (whole lists took big pages' memory) */
	if (attr == ATTR_SRCSET || attr == ATTR_DATA_SRCSET || attr == ATTR_DATA_SRC) {
		if (d->img_w <= 0)
			return;
		if (attr != ATTR_DATA_SRC && (value = doc_srcset_pick(value, d->img_w, &n)) == NULL)
			return;
	}
	if (grow(d, (void **)&d->attr, &d->attr_cap, d->attr_len + n + 3, 1) < 0)
		return;
	d->attr[d->attr_len++] = (unsigned char)attr;
	memcpy(d->attr + d->attr_len, value, n);
	d->attr[d->attr_len + n] = '\0';
	d->attr_len += n + 1;
}

void doc_attr_end(struct doc *d)
{
	if (grow(d, (void **)&d->attr, &d->attr_cap, d->attr_len + 1, 1) < 0) {
		/* keep the list terminated in the space already there */
		if (d->attr_len)
			d->attr[d->attr_len - 1] = 0;
		return;
	}
	d->attr[d->attr_len++] = 0;
}

void doc_add_text(struct doc *d, nodeid parent, const char *s, size_t n)
{
	nodeid last = d->nodes[parent].last;

	if (n == 0 || d->truncated)
		return;
	if (grow(d, (void **)&d->text, &d->text_cap, d->text_len + n + 1, 1) < 0)
		return;
	if (last && last == d->last_text && d->nodes[last].type == NODE_TEXT) {
		/* extend: overwrite the NUL */
		memcpy(d->text + d->text_len - 1, s, n);
		d->text_len += n;
		d->text[d->text_len - 1] = '\0';
		return;
	}
	last = new_node(d, parent, NODE_TEXT);
	if (last == 0)
		return;
	d->nodes[last].data = d->text_len;
	memcpy(d->text + d->text_len, s, n);
	d->text_len += n + 1;
	d->text[d->text_len - 1] = '\0';
	d->last_text = last;
}

char *doc_text_reserve(struct doc *d, nodeid parent, size_t max)
{
	nodeid last = d->nodes[parent].last;

	if (d->truncated
		|| grow(d, (void **)&d->text, &d->text_cap, d->text_len + max + 1, 1) < 0)
		return NULL;
	d->res_parent = parent;
	d->res_extend = last && last == d->last_text
		&& d->nodes[last].type == NODE_TEXT;
	/* extending writes over the old NUL */
	return d->text + d->text_len - (d->res_extend ? 1 : 0);
}

void doc_text_commit(struct doc *d, size_t written)
{
	nodeid id;

	if (written == 0) {
		if (d->res_extend)
			d->text[d->text_len - 1] = '\0';
		return;
	}
	if (d->res_extend) {
		d->text_len += written;
		d->text[d->text_len - 1] = '\0';
		return;
	}
	id = new_node(d, d->res_parent, NODE_TEXT);
	if (id == 0)
		return;
	d->nodes[id].data = d->text_len;
	d->text_len += written + 1;
	d->text[d->text_len - 1] = '\0';
	d->last_text = id;
}

const char *doc_text(const struct doc *d, nodeid id)
{
	return d->nodes[id].type == NODE_TEXT ? d->text + d->nodes[id].data : "";
}

const char *doc_attr(const struct doc *d, nodeid id, int attr)
{
	const unsigned char *p;

	if (d->nodes[id].type != NODE_ELEM)
		return NULL;
	p = d->attr + d->nodes[id].data;
	while (p < d->attr + d->attr_len && *p) {
		if (*p == attr)
			return (const char *)p + 1;
		p += 1 + strlen((const char *)p + 1) + 1;
	}
	return NULL;
}

static nodeid find_tag(const struct doc *d, nodeid id, int tag)
{
	nodeid c;

	for (; id; id = d->nodes[id].next) {
		if (d->nodes[id].type == NODE_ELEM && d->nodes[id].tag == tag)
			return id;
		if ((c = find_tag(d, d->nodes[id].first, tag)) != 0)
			return c;
	}
	return 0;
}

const char *doc_title(const struct doc *d)
{
	static char buf[256];
	nodeid t = find_tag(d, d->nodes[1].first, TAG_TITLE), c;
	size_t n = 0;
	int space = 1;

	buf[0] = '\0';
	if (t == 0)
		return buf;
	for (c = d->nodes[t].first; c; c = d->nodes[c].next) {
		const char *s = doc_text(d, c);

		for (; *s && n < sizeof buf - 1; s++) {
			int ws = *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r';

			if (ws) {
				if (!space)
					buf[n++] = ' ';
				space = 1;
			} else {
				buf[n++] = *s;
				space = 0;
			}
		}
	}
	while (n && buf[n - 1] == ' ')
		n--;
	buf[n] = '\0';
	return buf;
}

static void dump(const struct doc *d, nodeid id, int depth, FILE *f)
{
	for (; id; id = d->nodes[id].next) {
		const struct node *n = &d->nodes[id];

		fprintf(f, "%*s", depth * 2, "");
		if (n->type == NODE_TEXT) {
			const char *s = doc_text(d, id);

			fputc('"', f);
			for (; *s; s++)
				if (*s == '\n')
					fputs("\\n", f);
				else if (*s == '"')
					fputs("\\\"", f);
				else
					fputc(*s, f);
			fputs("\"\n", f);
			continue;
		}
		fprintf(f, "<%s", tag_name(n->tag));
		{
			const unsigned char *p = d->attr + n->data;

			while (p < d->attr + d->attr_len && *p) {
				fprintf(f, " %s=\"%s\"", attr_name(*p), p + 1);
				p += 1 + strlen((const char *)p + 1) + 1;
			}
		}
		fputs(">\n", f);
		dump(d, n->first, depth + 1, f);
	}
}

void doc_dump(const struct doc *d, void *file)
{
	dump(d, d->nodes[1].first, 0, file);
	if (d->truncated)
		fputs("[truncated]\n", file);
}
