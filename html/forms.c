/*
 * forms.c - see forms.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "tags.h"
#include "utf8.h"
#include "forms.h"

/* --- the fields ----------------------------------------------------------- */

static int lower_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;

		if (x != *b)
			return 0;
	}
	return *a == *b;
}

static int field_type(const struct doc *d, nodeid id)
{
	const char *t;

	switch (d->nodes[id].tag) {
	case TAG_SELECT:
		return FT_SELECT;
	case TAG_TEXTAREA:
		return FT_TEXTAREA;
	case TAG_BUTTON:
		t = doc_attr(d, id, ATTR_TYPE);
		if (t && lower_eq(t, "reset"))
			return FT_RESET;
		if (t && lower_eq(t, "button"))
			return FT_BUTTON;
		return FT_SUBMIT;
	}
	t = doc_attr(d, id, ATTR_TYPE);
	if (t == NULL)
		return FT_TEXT;
	if (lower_eq(t, "password")) return FT_PASSWORD;
	if (lower_eq(t, "hidden")) return FT_HIDDEN;
	if (lower_eq(t, "checkbox")) return FT_CHECKBOX;
	if (lower_eq(t, "radio")) return FT_RADIO;
	if (lower_eq(t, "submit")) return FT_SUBMIT;
	if (lower_eq(t, "reset")) return FT_RESET;
	if (lower_eq(t, "button")) return FT_BUTTON;
	if (lower_eq(t, "image")) return FT_IMAGE;
	if (lower_eq(t, "file")) return FT_FILE;
	return FT_TEXT;			/* text, search, email, url, ... */
}

/* the text inside a node (an option's label, a textarea's value) */
static void inner_text(const struct doc *d, nodeid id, char *out, size_t n,
	int trim)
{
	nodeid c;
	size_t o = 0;

	for (c = d->nodes[id].first; c && o + 1 < n; c = d->nodes[c].next)
		if (d->nodes[c].type == NODE_TEXT) {
			const char *t = doc_text(d, c);

			while (*t && o + 1 < n)
				out[o++] = *t++;
		}
	out[o] = '\0';
	if (trim) {
		char *s = out, *e;

		while (*s == ' ' || *s == '\n' || *s == '\t')
			s++;
		e = s + strlen(s);
		while (e > s && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\t'))
			*--e = '\0';
		memmove(out, s, strlen(s) + 1);
	}
}

/* the text inside id, all of it, in a new string (NULL: no memory) */
static char *inner_dup(const struct doc *d, nodeid id)
{
	size_t n = 1;
	nodeid c;
	char *s;

	for (c = d->nodes[id].first; c; c = d->nodes[c].next)
		if (d->nodes[c].type == NODE_TEXT)
			n += strlen(doc_text(d, c));
	if ((s = xmalloc(n)) != NULL)
		inner_text(d, id, s, n, 0);
	return s;
}

/* the options of a select (optgroups looked into) */
int forms_options(const struct forms *fs, const struct field *f,
	nodeid *opts, int max)
{
	const struct doc *d = fs->d;
	nodeid c, g;
	int n = 0;

	for (c = d->nodes[f->node].first; c; c = d->nodes[c].next) {
		if (d->nodes[c].type != NODE_ELEM)
			continue;
		if (d->nodes[c].tag == TAG_OPTION) {
			if (n < max)
				opts[n] = c;
			n++;
		} else if (d->nodes[c].tag == TAG_OPTGROUP)
			for (g = d->nodes[c].first; g; g = d->nodes[g].next)
				if (d->nodes[g].type == NODE_ELEM
					&& d->nodes[g].tag == TAG_OPTION) {
					if (n < max)
						opts[n] = g;
					n++;
				}
	}
	return n < max ? n : max;
}

static void initial(struct forms *fs, struct field *f)
{
	const struct doc *d = fs->d;

	xfree(f->value);
	f->value = NULL;
	f->checked = 0;
	f->selected = 0;
	switch (f->type) {
	case FT_CHECKBOX:
	case FT_RADIO:
		f->checked = doc_attr(d, f->node, ATTR_CHECKED) != NULL;
		break;
	case FT_SELECT: {
		static nodeid o[FORMS_MAX_OPTIONS];
		int n = forms_options(fs, f, o, FORMS_MAX_OPTIONS), i;

		for (i = 0; i < n && !f->selected; i++)
			if (doc_attr(d, o[i], ATTR_SELECTED))
				f->selected = o[i];
		if (!f->selected && n)
			f->selected = o[0];
		break;
	}
	case FT_TEXTAREA:
		/* (whole: what's sent back is what was there) */
		f->value = inner_dup(d, f->node);
		break;
	default: {
		const char *v = doc_attr(d, f->node, ATTR_VALUE);

		f->value = xstrdup(v ? v : "");
	}
	}
}

int forms_init(struct forms *fs, const struct doc *d)
{
	unsigned long i;
	nodeid last_form = 0;
	int cap = 0;

	memset(fs, 0, sizeof *fs);
	fs->d = d;
	for (i = 2; i < d->nnodes; i++) {
		const struct node *n = &d->nodes[i];
		struct field *f;
		nodeid a;

		if (n->type != NODE_ELEM)
			continue;
		if (n->tag == TAG_FORM) {
			last_form = (nodeid)i;
			continue;
		}
		if (n->tag != TAG_INPUT && n->tag != TAG_SELECT
			&& n->tag != TAG_TEXTAREA && n->tag != TAG_BUTTON)
			continue;
		if (fs->n == cap) {
			struct field *g;

			cap = cap ? cap * 2 : 16;
			g = xrealloc(fs->f, (size_t)cap * sizeof *g);
			if (g == NULL)
				return -1;
			fs->f = g;
		}
		f = &fs->f[fs->n++];
		memset(f, 0, sizeof *f);
		f->node = (nodeid)i;
		f->type = (unsigned char)field_type(d, (nodeid)i);
		/* its form: the one it's in; failing that (a form cut short by
		 * a table, in sloppy HTML) the last one before it */
		for (a = n->parent; a > 1; a = d->nodes[a].parent)
			if (d->nodes[a].tag == TAG_FORM)
				break;
		f->form = a > 1 ? a : last_form;
		initial(fs, f);
	}
	return 0;
}

void forms_free(struct forms *fs)
{
	int i;

	for (i = 0; i < fs->n; i++)
		xfree(fs->f[i].value);
	xfree(fs->f);
	memset(fs, 0, sizeof *fs);
}

struct field *forms_field(const struct forms *fs, nodeid node)
{
	int lo = 0, hi = fs->n;

	/* in document order, which is node order */
	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (fs->f[mid].node == node)
			return &fs->f[mid];
		if (fs->f[mid].node < node)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

const char *forms_text(const struct forms *fs, const struct field *f)
{
	static char buf[256];

	if (f->type == FT_SELECT) {
		buf[0] = '\0';
		if (f->selected) {
			inner_text(fs->d, f->selected, buf, sizeof buf, 1);
			if (!buf[0] && doc_attr(fs->d, f->selected, ATTR_LABEL))
				snprintf(buf, sizeof buf, "%s",
					doc_attr(fs->d, f->selected, ATTR_LABEL));
		}
		return buf;
	}
	return f->value ? f->value : "";
}

void forms_set_text(struct field *f, const char *v)
{
	char *n = xstrdup(v);

	if (n) {
		xfree(f->value);
		f->value = n;
	}
}

void forms_click(struct forms *fs, struct field *f)
{
	int i;

	if (f->type == FT_CHECKBOX) {
		f->checked = !f->checked;
		return;
	}
	if (f->type != FT_RADIO)
		return;
	{
		const char *name = doc_attr(fs->d, f->node, ATTR_NAME);

		for (i = 0; i < fs->n; i++) {
			struct field *g = &fs->f[i];
			const char *gn;

			if (g->type != FT_RADIO || g->form != f->form || g == f)
				continue;
			gn = doc_attr(fs->d, g->node, ATTR_NAME);
			if (name && gn && strcmp(name, gn) == 0)
				g->checked = 0;
		}
		f->checked = 1;
	}
}

void forms_choose(struct field *f, nodeid option)
{
	f->selected = option;
}

void forms_reset(struct forms *fs, nodeid form)
{
	int i;

	for (i = 0; i < fs->n; i++)
		if (fs->f[i].form == form)
			initial(fs, &fs->f[i]);
}

struct field *forms_next_text(const struct forms *fs, const struct field *f)
{
	int i;

	for (i = (int)(f - fs->f) + 1; i < fs->n; i++)
		if (fs->f[i].form == f->form && (fs->f[i].type == FT_TEXT
			|| fs->f[i].type == FT_PASSWORD)
			&& !doc_attr(fs->d, fs->f[i].node, ATTR_DISABLED)
			&& !doc_attr(fs->d, fs->f[i].node, ATTR_READONLY))
			return &fs->f[i];
	return NULL;
}

int forms_has_submit(const struct forms *fs, nodeid form)
{
	int i;

	for (i = 0; i < fs->n; i++)
		if (fs->f[i].form == form && (fs->f[i].type == FT_SUBMIT
			|| fs->f[i].type == FT_IMAGE))
			return 1;
	return 0;
}

/* --- encoding ------------------------------------------------------------- */

struct buf {
	char *p;
	size_t len, cap;
	int oom;
};

static void put(struct buf *b, const char *s, size_t n)
{
	if (b->oom)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t c = (b->len + n + 1) * 2;
		char *q = xrealloc(b->p, c);

		if (q == NULL) {
			b->oom = 1;
			return;
		}
		b->p = q;
		b->cap = c;
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = '\0';
}

static void puts_(struct buf *b, const char *s)
{
	put(b, s, strlen(s));
}

/* the bytes a value is sent as: UTF-8, or windows-1252 with &#N; for
 * what it lacks (as browsers do) */
static void charset_bytes(struct buf *b, const char *s, int win1252)
{
	while (*s) {
		unsigned long cp;
		int w;

		if ((unsigned char)*s < 0x80 || !win1252) {
			put(b, s, 1);
			s++;
			continue;
		}
		cp = utf8_get(&s);
		if ((w = win1252_byte(cp)) >= 0) {
			char c = (char)w;

			put(b, &c, 1);
		} else {
			char e[16];

			sprintf(e, "&#%lu;", cp);
			puts_(b, e);
		}
	}
}

/* application/x-www-form-urlencoded, of bytes */
static void urlencode(struct buf *b, const char *s, size_t n)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '*' || c == '-'
			|| c == '.' || c == '_')
			put(b, (char *)&c, 1);
		else if (c == ' ')
			put(b, "+", 1);
		else {
			char e[3];

			e[0] = '%';
			e[1] = hex[c >> 4];
			e[2] = hex[c & 15];
			put(b, e, 3);
		}
	}
}

/* newlines in a textarea go as CRLF */
static char *crlf(const char *s)
{
	size_t n = 0, i;
	char *o, *p;

	for (i = 0; s[i]; i++)
		n += s[i] == '\n' ? 2 : 1;
	o = p = xmalloc(n + 1);
	if (o == NULL)
		return NULL;
	for (i = 0; s[i]; i++) {
		if (s[i] == '\r')
			continue;
		if (s[i] == '\n')
			*p++ = '\r';
		*p++ = s[i];
	}
	*p = '\0';
	return o;
}

enum { ENC_URL, ENC_MULTIPART, ENC_PLAIN };

static void add_pair(struct buf *b, int enc, const char *boundary,
	const char *name, const char *value, int win1252)
{
	struct buf nb, vb;

	memset(&nb, 0, sizeof nb);
	memset(&vb, 0, sizeof vb);
	charset_bytes(&nb, name, win1252);
	charset_bytes(&vb, value, win1252);
	switch (enc) {
	case ENC_URL:
		if (b->len)
			put(b, "&", 1);
		urlencode(b, nb.p ? nb.p : "", nb.len);
		put(b, "=", 1);
		urlencode(b, vb.p ? vb.p : "", vb.len);
		break;
	case ENC_MULTIPART:
		puts_(b, "--");
		puts_(b, boundary);
		puts_(b, "\r\nContent-Disposition: form-data; name=\"");
		{
			/* quotes and newlines in the name, escaped as browsers do */
			size_t i;

			for (i = 0; i < nb.len; i++)
				if (nb.p[i] == '"')
					puts_(b, "%22");
				else if (nb.p[i] == '\r')
					puts_(b, "%0D");
				else if (nb.p[i] == '\n')
					puts_(b, "%0A");
				else
					put(b, nb.p + i, 1);
		}
		puts_(b, "\"\r\n\r\n");
		put(b, vb.p ? vb.p : "", vb.len);
		puts_(b, "\r\n");
		break;
	default:
		put(b, nb.p ? nb.p : "", nb.len);
		put(b, "=", 1);
		put(b, vb.p ? vb.p : "", vb.len);
		puts_(b, "\r\n");
	}
	if (nb.oom || vb.oom)
		b->oom = 1;
	xfree(nb.p);
	xfree(vb.p);
}

int forms_submit(const struct forms *fs, nodeid form, nodeid submitter,
	const struct url *base, int win1252, struct submission *out,
	const char **why)
{
	const struct doc *d = fs->d;
	const char *action = form ? doc_attr(d, form, ATTR_ACTION) : NULL;
	const char *method = form ? doc_attr(d, form, ATTR_METHOD) : NULL;
	const char *enctype = form ? doc_attr(d, form, ATTR_ENCTYPE) : NULL;
	char boundary[48];
	struct buf b;
	static struct url u;
	int post = method && lower_eq(method, "post"), enc = ENC_URL, i;

	memset(out, 0, sizeof *out);
	memset(&b, 0, sizeof b);
	if (!form) {
		*why = "the field isn't in a form";
		return -1;
	}
	if (action == NULL || !*action)
		action = "";		/* the page itself */
	if (strncmp(action, "javascript:", 11) == 0) {
		*why = "the form needs JavaScript";
		return -1;
	}
	if (url_resolve(base, action, &u) != URL_OK) {
		*why = "the form's action isn't a usable URL";
		return -1;
	}
	if (strcmp(u.scheme, "http") && strcmp(u.scheme, "https")
		&& strcmp(u.scheme, "gopher") && strcmp(u.scheme, "file")) {
		*why = "the form goes to an unsupported kind of URL";
		return -1;
	}
	if (post && enctype && lower_eq(enctype, "multipart/form-data"))
		enc = ENC_MULTIPART;
	else if (post && enctype && lower_eq(enctype, "text/plain"))
		enc = ENC_PLAIN;
	sprintf(boundary, "----ubform%08lx%08lx", os_usec(),
		(unsigned long)rand());

	for (i = 0; i < fs->n; i++) {
		const struct field *f = &fs->f[i];
		const char *name = doc_attr(d, f->node, ATTR_NAME);
		const char *v;

		if (f->form != form || doc_attr(d, f->node, ATTR_DISABLED))
			continue;
		/* buttons: only the one pressed */
		if ((f->type == FT_SUBMIT || f->type == FT_IMAGE)
			&& f->node != submitter)
			continue;
		if (f->type == FT_IMAGE) {
			/* the click point; we have none, so 0,0 */
			char k[300];

			snprintf(k, sizeof k, "%s%sx", name ? name : "",
				name && *name ? "." : "");
			add_pair(&b, enc, boundary, k, "0", win1252);
			k[strlen(k) - 1] = 'y';
			add_pair(&b, enc, boundary, k, "0", win1252);
			continue;
		}
		if (name == NULL || !*name)
			continue;
		switch (f->type) {
		case FT_RESET:
		case FT_BUTTON:
			continue;
		case FT_CHECKBOX:
		case FT_RADIO:
			if (!f->checked)
				continue;
			v = doc_attr(d, f->node, ATTR_VALUE);
			add_pair(&b, enc, boundary, name, v ? v : "on", win1252);
			break;
		case FT_SELECT:
			if (!f->selected)
				continue;
			v = doc_attr(d, f->selected, ATTR_VALUE);
			if (v)
				add_pair(&b, enc, boundary, name, v, win1252);
			else {
				char t[1024];

				inner_text(d, f->selected, t, sizeof t, 1);
				add_pair(&b, enc, boundary, name, t, win1252);
			}
			break;
		case FT_TEXTAREA: {
			char *c = crlf(f->value ? f->value : "");

			add_pair(&b, enc, boundary, name, c ? c : "", win1252);
			xfree(c);
			break;
		}
		case FT_FILE:
			add_pair(&b, enc, boundary, name, "", win1252);
			break;
		default:
			add_pair(&b, enc, boundary, name,
				f->value ? f->value : "", win1252);
		}
	}
	if (enc == ENC_MULTIPART) {
		puts_(&b, "--");
		puts_(&b, boundary);
		puts_(&b, "--\r\n");
	}
	if (b.oom) {
		xfree(b.p);
		*why = "out of memory";
		return -1;
	}
	if (!post) {
		/* GET: the data replaces the action's query */
		u.has_fragment = 0;
		if (b.len >= sizeof u.query) {
			xfree(b.p);
			*why = "the form data is too long for a URL";
			return -1;
		}
		u.has_query = 1;
		memcpy(u.query, b.p ? b.p : "", b.len + 1);
		xfree(b.p);
		strcpy(out->method, "GET");
		if (url_format(&u, out->url, sizeof out->url, 0) != URL_OK) {
			*why = "the form data is too long for a URL";
			return -1;
		}
		return 0;
	}
	strcpy(out->method, "POST");
	u.has_fragment = 0;
	if (url_format(&u, out->url, sizeof out->url, 0) != URL_OK) {
		xfree(b.p);
		*why = "the form's URL is too long";
		return -1;
	}
	out->body = b.p ? b.p : xstrdup("");
	out->body_len = b.len;
	if (enc == ENC_MULTIPART)
		snprintf(out->type, sizeof out->type,
			"multipart/form-data; boundary=%s", boundary);
	else
		strcpy(out->type, enc == ENC_PLAIN ? "text/plain"
			: "application/x-www-form-urlencoded");
	return 0;
}
