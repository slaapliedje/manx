/*
 * css.c - which elements a page's style sheets hide (css.h).
 *
 * The text is read a character at a time (comments, strings and escapes
 * followed across pieces) into a rule's prelude and its declarations;
 * when the rule ends, its declarations are looked at, and only a rule
 * about display or visibility is kept, compiled: each selector a few
 * compound selectors, the subject first, with class and id names hashed.
 * Rules are filed by their subject's id, else a class, else its tag, so an
 * element is tried against a handful of rules, not all of them. What the
 * sheet says about each element is remembered until the rules or the
 * window's width change.
 */
#include <string.h>
#include "os.h"
#include "tags.h"
#include "css.h"

#define MAX_RULES	2048		/* rules kept, at most */
#define MAX_CMPS	6144		/* compound selectors, all rules */
#define MAX_MEDIA	256		/* distinct @media conditions */
#define PRELUDE_MAX	1024		/* a longer selector list is dropped */
#define DECLS_MAX	2048		/* declaration bytes looked at */
#define MAX_GROUPS	16		/* @media/@supports nesting */
#define SEL_CMPS	6		/* compounds in one selector */
#define NBUCKET		1024
#define MEDIA_ALTS	4

struct cmp {
	unsigned long id;		/* #id's hash; 0: none */
	unsigned long cls[3];		/* .class hashes */
	unsigned long aval;		/* [attr=v] / [attr~=v]: v's hash */
	short tag;			/* TAG_*; 0: any */
	unsigned char ncls;
	unsigned char attr;		/* ATTR_*; 0: no attribute test */
	unsigned char aop;		/* 0: present; '=', '~' */
	unsigned char comb;		/* to the compound on its left: ' ' or '>' */
};

struct rule {
	unsigned long spec;		/* ids << 16 | classes << 8 | types */
	unsigned long order;
	unsigned short cmp, ncmp;	/* s->cmps[cmp...], the subject first */
	unsigned short media;		/* 0: all; else s->media[media - 1] */
	unsigned char disp, vis;	/* CSS_SHOW / CSS_HIDE / CSS_UNSET */
	unsigned char disp_imp, vis_imp;	/* !important */
	int next;			/* the next rule in its bucket, -1 */
};

/* width ranges in px (max 0: no upper bound); none: never */
struct media {
	unsigned char n;
	struct {
		unsigned short min, max;
	} alt[MEDIA_ALTS];
};

enum { P_PRELUDE, P_DECLS, P_SKIP };

#define EL_CLASSES	8		/* classes of an element looked at */
#define PATH_DEPTH	200		/* ancestors kept (as the tree's depth) */

/* an element as selectors see it, hashed once */
struct elinfo {
	nodeid node;
	short tag;
	unsigned char ncls;
	unsigned long id;
	unsigned long cls[EL_CLASSES];
};

struct css_sheet {
	struct rule *rules;
	int nrules, rules_cap;
	struct cmp *cmps;
	int ncmps, cmps_cap;
	struct media media[MAX_MEDIA];
	int nmedia;
	int bucket[NBUCKET];
	unsigned long order;		/* pos << 16 | the rule's number in it */
	unsigned gen;			/* bumped as rules are added */
	/* the reader */
	int on;				/* inside css_begin/css_end */
	int state, depth;
	int quote, esc, slash, star, comment;
	char pre[PRELUDE_MAX];
	int plen, plong;
	char decl[DECLS_MAX];
	int dlen;
	unsigned short group[MAX_GROUPS];	/* media of each open group */
	int ngroups, base;		/* base: the <style media>'s */
	/* the last element matched and its ancestors, root first: the layout
	 * goes through the tree in order, so the next one's are mostly here */
	struct elinfo path[PATH_DEPTH];
	int npath;
	/* what was said of each node: 0 not asked, 1 nothing, 2 hidden */
	unsigned char *memo;
	unsigned long memo_n;
	unsigned memo_gen;
	int memo_vw;
};

/* --- small things -------------------------------------------------------- */

static unsigned long hash_bytes(const char *s, size_t n)
{
	unsigned long h = 2166136261UL;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= (unsigned char)s[i];
		h = (h * 16777619UL) & 0xFFFFFFFFUL;
	}
	return h | 1;			/* (0 means none) */
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int is_space(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int is_name(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		|| (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80;
}

/* does s (n bytes) start with word w, case aside? */
static int starts(const char *s, size_t n, const char *w)
{
	size_t i;

	for (i = 0; w[i]; i++)
		if (i >= n || lower((unsigned char)s[i]) != w[i])
			return 0;
	return 1;
}

/* the word w somewhere in s (n bytes), case aside */
static int has(const char *s, size_t n, const char *w)
{
	size_t i, k = strlen(w);

	for (i = 0; i + k <= n; i++)
		if (starts(s + i, n - i, w))
			return 1;
	return 0;
}

/* --- the sheet -------------------------------------------------------------- */

struct css_sheet *css_new(void)
{
	struct css_sheet *s = xmalloc(sizeof *s);
	int i;

	if (s == NULL)
		return NULL;
	memset(s, 0, sizeof *s);
	for (i = 0; i < NBUCKET; i++)
		s->bucket[i] = -1;
	return s;
}

void css_free(struct css_sheet *s)
{
	if (s == NULL)
		return;
	xfree(s->rules);
	xfree(s->cmps);
	xfree(s->memo);
	xfree(s);
}

unsigned long css_rules(const struct css_sheet *s)
{
	return s ? (unsigned long)s->nrules : 0;
}

/* --- @media --------------------------------------------------------------- */

/* a length in px: "740px", "37.5rem" (16 px), "48em"; -1 if not one */
static long length_px(const char *s, size_t n)
{
	long v = 0, tenths = 0;
	size_t i = 0;
	int frac = 0;

	while (i < n && is_space((unsigned char)s[i]))
		i++;
	if (i == n || (!(s[i] >= '0' && s[i] <= '9') && s[i] != '.'))
		return -1;
	for (; i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.'); i++) {
		if (s[i] == '.')
			frac = 1;
		else if (!frac)
			v = v * 10 + (s[i] - '0');
		else if (frac++ == 1)
			tenths = s[i] - '0';
		if (v > 100000)
			return -1;
	}
	if (starts(s + i, n - i, "px"))
		return v;
	if (starts(s + i, n - i, "rem") || starts(s + i, n - i, "em"))
		return v * 16 + tenths * 16 / 10;
	return -1;
}

/* one media feature "(name: value)": does a screen like Manx's meet it?
 * Widths go to *min, *max instead. 1, 0, or -1: not known */
static int feature(const char *f, size_t n, long *min, long *max)
{
	size_t c;
	const char *v;
	size_t vn;
	long px;

	while (n && is_space((unsigned char)*f)) {
		f++;
		n--;
	}
	for (c = 0; c < n && f[c] != ':'; c++)
		;
	if (c == n)
		return starts(f, n, "color") || starts(f, n, "hover")
			|| starts(f, n, "pointer") ? 1 : -1;
	v = f + c + 1;
	vn = n - c - 1;
	while (vn && is_space((unsigned char)*v)) {
		v++;
		vn--;
	}
	if (starts(f, n, "min-width") || starts(f, n, "max-width")
		|| starts(f, n, "width")) {
		if ((px = length_px(v, vn)) < 0)
			return -1;
		if (starts(f, n, "min-")) {
			if (px > *min)
				*min = px;
		} else if (starts(f, n, "max-")) {
			if (*max == 0 || px < *max)
				*max = px ? px : 1;
		} else {
			*min = px;
			*max = px ? px : 1;
		}
		return 1;
	}
	if (starts(f, n, "orientation"))
		return starts(v, vn, "landscape");
	if (starts(f, n, "prefers-color-scheme"))
		return starts(v, vn, "light");
	if (starts(f, n, "prefers-reduced-motion") || starts(f, n, "prefers-contrast")
		|| starts(f, n, "forced-colors") || starts(f, n, "prefers-reduced"))
		return starts(v, vn, "no-preference") || starts(v, vn, "none");
	if (starts(f, n, "hover") || starts(f, n, "any-hover"))
		return starts(v, vn, "hover");
	if (starts(f, n, "pointer") || starts(f, n, "any-pointer"))
		return starts(v, vn, "fine");
	if (has(f, n, "resolution") || has(f, n, "pixel-ratio"))
		return !has(f, n, "min-");	/* (a 1x screen) */
	return -1;
}

/* 1 + the entry in s->media holding m (added if new) */
static unsigned short media_id(struct css_sheet *s, const struct media *m)
{
	int i;

	for (i = 0; i < s->nmedia; i++)
		if (memcmp(&s->media[i], m, sizeof *m) == 0)
			return (unsigned short)(i + 1);
	if (s->nmedia == MAX_MEDIA)
		return 0xFFFF;		/* (no room: never, so content shows) */
	s->media[s->nmedia] = *m;
	return (unsigned short)++s->nmedia;
}

/* a media query list: 0 for "all", else 1 + its entry in s->media */
static unsigned short parse_media(struct css_sheet *s, const char *q, size_t n)
{
	struct media m;
	size_t i = 0;

	memset(&m, 0, sizeof m);
	while (i <= n) {
		size_t e = i;
		long min = 0, max = 0;
		int ok = 1, neg = 0, type = 0;

		while (e < n && q[e] != ',')
			e++;
		while (i < e) {
			int c = (unsigned char)q[i];

			if (is_space(c)) {
				i++;
			} else if (c == '(') {
				size_t j = i + 1;
				int r;

				while (j < e && q[j] != ')')
					j++;
				r = feature(q + i + 1, j - i - 1, &min, &max);
				if (r <= 0)
					ok = 0;
				i = j + 1;
			} else {
				size_t j = i;

				while (j < e && !is_space((unsigned char)q[j]) && q[j] != '(')
					j++;
				if (starts(q + i, j - i, "not"))
					neg = 1;
				else if (starts(q + i, j - i, "only") || starts(q + i, j - i, "and"))
					;
				else if (starts(q + i, j - i, "all") || starts(q + i, j - i, "screen"))
					type = 1;
				else
					type = -1;	/* print, speech... */
				i = j;
			}
		}
		if (type < 0)
			ok = 0;
		if (neg)
			ok = type < 0 && max == 0 && min == 0;	/* "not print" */
		if (ok && (max == 0 || min <= max) && m.n < MEDIA_ALTS) {
			m.alt[m.n].min = (unsigned short)(min > 65535 ? 65535 : min);
			m.alt[m.n].max = (unsigned short)(max > 65535 ? 65535 : max);
			m.n++;
		}
		i = e + 1;
	}
	if (m.n == 1 && m.alt[0].min == 0 && m.alt[0].max == 0)
		return 0;
	return media_id(s, &m);
}

/* a group inside another: both must hold */
static unsigned short both_media(struct css_sheet *s, unsigned short a,
	unsigned short b)
{
	struct media m;
	int i, j;

	if (a == 0)
		return b;
	if (b == 0 || a == 0xFFFF)
		return a;
	if (b == 0xFFFF)
		return b;
	memset(&m, 0, sizeof m);
	for (i = 0; i < s->media[a - 1].n; i++)
		for (j = 0; j < s->media[b - 1].n; j++) {
			unsigned short x0 = s->media[a - 1].alt[i].min, x1 = s->media[a - 1].alt[i].max;
			unsigned short y0 = s->media[b - 1].alt[j].min, y1 = s->media[b - 1].alt[j].max;
			unsigned short lo = x0 > y0 ? x0 : y0;
			unsigned short hi = !x1 ? y1 : !y1 ? x1 : x1 < y1 ? x1 : y1;

			if ((hi == 0 || lo <= hi) && m.n < MEDIA_ALTS) {
				m.alt[m.n].min = lo;
				m.alt[m.n].max = hi;
				m.n++;
			}
		}
	return media_id(s, &m);
}

static int media_ok(const struct css_sheet *s, unsigned short id, int vw)
{
	const struct media *m;
	int i;

	if (id == 0)
		return 1;
	if (id == 0xFFFF)
		return 0;
	m = &s->media[id - 1];
	for (i = 0; i < m->n; i++)
		if (vw >= m->alt[i].min && (m->alt[i].max == 0 || vw <= m->alt[i].max))
			return 1;
	return 0;
}

static unsigned short cur_media(const struct css_sheet *s)
{
	return s->ngroups ? s->group[s->ngroups - 1] : 0;
}

/* --- selectors ------------------------------------------------------------- */

/* a name at *p (an identifier, escapes undone) into out (cap bytes,
 * NUL-ended): its length, or -1 (none, or too long, or unusable) */
static int read_name(const char **pp, const char *e, char *out, int cap)
{
	const char *p = *pp;
	int n = 0;

	while (p < e) {
		int c = (unsigned char)*p;

		if (c == '\\') {
			unsigned long cp = 0;
			int k = 0;

			if (++p == e)
				return -1;
			while (p < e && k < 6 && ((*p >= '0' && *p <= '9')
				|| (lower((unsigned char)*p) >= 'a' && lower((unsigned char)*p) <= 'f'))) {
				int d = lower((unsigned char)*p);

				cp = cp * 16 + (unsigned long)(d <= '9' ? d - '0' : d - 'a' + 10);
				p++;
				k++;
			}
			if (k == 0)
				cp = (unsigned char)*p++;
			else if (p < e && is_space((unsigned char)*p))
				p++;
			/* (as UTF-8, as the page's attributes are) */
			if (cp < 0x80) {
				if (n + 1 >= cap)
					return -1;
				out[n++] = (char)cp;
			} else if (cp < 0x800) {
				if (n + 2 >= cap)
					return -1;
				out[n++] = (char)(0xC0 | cp >> 6);
				out[n++] = (char)(0x80 | (cp & 0x3F));
			} else if (cp < 0x10000) {
				if (n + 3 >= cap)
					return -1;
				out[n++] = (char)(0xE0 | cp >> 12);
				out[n++] = (char)(0x80 | (cp >> 6 & 0x3F));
				out[n++] = (char)(0x80 | (cp & 0x3F));
			} else
				return -1;
			continue;
		}
		if (!is_name(c))
			break;
		if (n + 1 >= cap)
			return -1;
		out[n++] = (char)c;
		p++;
	}
	out[n] = '\0';
	*pp = p;
	return n > 0 ? n : -1;
}

/*
 * One selector, p to e, compiled into out[], the subject first: the
 * count, or 0 when it can't be used (anything Manx doesn't follow).
 */
static int compile(const char *p, const char *e, struct cmp *out, unsigned long *spec)
{
	struct cmp left[SEL_CMPS];
	struct cmp cur;
	int n = 0, have = 0, comb = 0, i;
	char name[128];

	memset(&cur, 0, sizeof cur);
	*spec = 0;
	while (p < e) {
		int c = (unsigned char)*p;

		if (is_space(c)) {
			if (have && !comb)
				comb = ' ';
			p++;
			continue;
		}
		if (c == '>') {
			if (!have)
				return 0;
			comb = '>';
			p++;
			continue;
		}
		if (c == '+' || c == '~' || c == ':' || c == '|' || c == ',')
			return 0;
		if (comb) {
			if (n == SEL_CMPS - 1)
				return 0;
			left[n++] = cur;
			memset(&cur, 0, sizeof cur);
			cur.comb = (unsigned char)comb;
			comb = 0;
			have = 0;
		}
		if (c == '*') {
			p++;
		} else if (c == '#') {
			p++;
			if (cur.id || read_name(&p, e, name, sizeof name) < 0)
				return 0;
			cur.id = hash_bytes(name, strlen(name));
			*spec += 1UL << 16;
		} else if (c == '.') {
			int k;

			p++;
			if (cur.ncls == 3 || (k = read_name(&p, e, name, sizeof name)) < 0)
				return 0;
			cur.cls[cur.ncls++] = hash_bytes(name, (size_t)k);
			*spec += 1UL << 8;
		} else if (c == '[') {
			int a, k;

			p++;
			while (p < e && is_space((unsigned char)*p))
				p++;
			if (cur.attr || (k = read_name(&p, e, name, sizeof name)) < 0)
				return 0;
			for (i = 0; i < k; i++)
				name[i] = (char)lower((unsigned char)name[i]);
			if ((a = attr_lookup(name)) == ATTR_NONE)
				return 0;	/* (not kept: can't be tested) */
			cur.attr = (unsigned char)a;
			while (p < e && is_space((unsigned char)*p))
				p++;
			if (p < e && *p == ']') {
				p++;
			} else {
				char q;
				const char *v;

				if (p < e && *p == '~' && p + 1 < e && p[1] == '=') {
					cur.aop = '~';
					p += 2;
				} else if (p < e && *p == '=') {
					cur.aop = '=';
					p++;
				} else
					return 0;	/* ^= $= *= |= */
				while (p < e && is_space((unsigned char)*p))
					p++;
				if (p < e && (*p == '"' || *p == '\'')) {
					q = *p++;
					v = p;
					while (p < e && *p != q)
						p++;
					if (p == e)
						return 0;
					cur.aval = hash_bytes(v, (size_t)(p - v));
					p++;
				} else {
					if ((k = read_name(&p, e, name, sizeof name)) < 0)
						return 0;
					cur.aval = hash_bytes(name, (size_t)k);
				}
				while (p < e && is_space((unsigned char)*p))
					p++;
				if (p < e && (*p == 'i' || *p == 's')) /* (flags) */
					return 0;
				if (p == e || *p != ']')
					return 0;
				p++;
			}
			*spec += 1UL << 8;
		} else if (is_name(c) || c == '\\') {
			int k = read_name(&p, e, name, sizeof name), t;

			if (k < 0 || cur.tag)
				return 0;
			for (i = 0; i < k; i++)
				name[i] = (char)lower((unsigned char)name[i]);
			if ((t = tag_lookup(name)) == TAG_UNKNOWN)
				return 0;	/* (custom elements all look alike) */
			cur.tag = (short)t;
			*spec += 1;
		} else
			return 0;
		have = 1;
	}
	if (!have)
		return 0;
	left[n++] = cur;
	/* the subject first: out[k].comb joins out[k] to out[k + 1] */
	for (i = 0; i < n; i++) {
		out[i] = left[n - 1 - i];
		out[i].comb = i + 1 < n ? left[n - 1 - i].comb : 0;
	}
	return n;
}

/* the bucket a rule is filed in, by its subject */
static unsigned bucket_of(const struct cmp *c)
{
	unsigned long k;

	if (c->id)
		k = c->id ^ 0x9E3779B9UL;
	else if (c->ncls)
		k = c->cls[0];
	else if (c->tag)
		k = (unsigned long)c->tag * 2654435761UL;
	else
		k = 0;
	return (unsigned)((k ^ k >> 13) & (NBUCKET - 1));
}

/* --- rules --------------------------------------------------------------------- */

/* do declarations d (n bytes) mention display or visibility at all?
 * (most rules don't, and are dropped without reading them) */
static int mentions(const char *d, int n)
{
	int i;

	for (i = 1; i + 4 < n; i++)
		if (d[i] == 'i' && d[i + 1] == 's'
			&& ((d[i + 2] == 'p' && d[i + 3] == 'l' && d[i - 1] == 'd')
			|| (d[i + 2] == 'i' && d[i + 3] == 'b' && d[i - 1] == 'v')))
			return 1;
	return 0;
}

/* the rule just read: if it says something about display or visibility,
 * file each of its selectors */
static void end_rule(struct css_sheet *s)
{
	const char *d = s->decl, *p, *e;
	int disp = 0, vis = 0, disp_imp = 0, vis_imp = 0;
	unsigned short media = cur_media(s);

	if (s->plong || s->plen == 0 || !mentions(d, s->dlen))
		return;
	/* the declarations */
	for (p = d; p < d + s->dlen; p = e + 1) {
		const char *colon, *v;
		size_t pn, vn;
		int paren = 0, quote = 0, imp;

		for (e = p; e < d + s->dlen; e++) {
			if (quote) {
				if (*e == quote)
					quote = 0;
			} else if (*e == '"' || *e == '\'')
				quote = *e;
			else if (*e == '(')
				paren++;
			else if (*e == ')')
				paren--;
			else if (*e == ';' && paren <= 0)
				break;
		}
		while (p < e && is_space((unsigned char)*p))
			p++;
		for (colon = p; colon < e && *colon != ':'; colon++)
			;
		if (colon == e)
			continue;
		pn = (size_t)(colon - p);
		while (pn && is_space((unsigned char)p[pn - 1]))
			pn--;
		v = colon + 1;
		while (v < e && is_space((unsigned char)*v))
			v++;
		vn = (size_t)(e - v);
		imp = has(v, vn, "!important");
		if (pn == 7 && starts(p, pn, "display")) {
			disp = starts(v, vn, "none") ? CSS_HIDE : CSS_SHOW;
			disp_imp = imp;
		} else if (pn == 10 && starts(p, pn, "visibility")) {
			if (starts(v, vn, "hidden") || starts(v, vn, "collapse"))
				vis = CSS_HIDE;
			else if (starts(v, vn, "visible"))
				vis = CSS_SHOW;
			vis_imp = imp;
		}
		/*
		 * (Text "visually hidden", clipped to nothing, stays: it is
		 * there for screen readers because sighted readers get the same
		 * from an icon or a picture, which Manx doesn't draw: "Posted 9
		 * minutes ago", "Search", a logo's name.)
		 */
	}
	if (!disp && !vis)
		return;
	/* the selectors */
	for (p = s->pre; p < s->pre + s->plen; p = e + 1) {
		struct cmp sel[SEL_CMPS];
		unsigned long spec;
		struct rule *r;
		int n, b, paren = 0;

		for (e = p; e < s->pre + s->plen; e++) {
			if (*e == '(')
				paren++;
			else if (*e == ')')
				paren--;
			else if (*e == ',' && paren <= 0)
				break;
		}
		if ((n = compile(p, e, sel, &spec)) == 0)
			continue;
		if (s->nrules == MAX_RULES || s->ncmps + n > MAX_CMPS)
			return;
		if (s->nrules == s->rules_cap) {
			int c = s->rules_cap ? s->rules_cap * 2 : 64;
			struct rule *q = xrealloc(s->rules, (size_t)c * sizeof *q);

			if (q == NULL)
				return;
			s->rules = q;
			s->rules_cap = c;
		}
		if (s->ncmps + n > s->cmps_cap) {
			int c = s->cmps_cap ? s->cmps_cap * 2 : 128;
			struct cmp *q;

			while (c < s->ncmps + n)
				c *= 2;
			if ((q = xrealloc(s->cmps, (size_t)c * sizeof *q)) == NULL)
				return;
			s->cmps = q;
			s->cmps_cap = c;
		}
		memcpy(s->cmps + s->ncmps, sel, (size_t)n * sizeof *sel);
		r = &s->rules[s->nrules];
		r->spec = spec;
		r->order = s->order;
		if ((s->order & 0xFFFF) != 0xFFFF)
			s->order++;
		r->cmp = (unsigned short)s->ncmps;
		r->ncmp = (unsigned short)n;
		r->media = media;
		r->disp = (unsigned char)disp;
		r->vis = (unsigned char)vis;
		r->disp_imp = (unsigned char)disp_imp;
		r->vis_imp = (unsigned char)vis_imp;
		b = (int)bucket_of(&sel[0]);
		r->next = s->bucket[b];
		s->bucket[b] = s->nrules++;
		s->ncmps += n;
		s->gen++;
	}
}

/* a { after a prelude: a rule's declarations, or a group, or a block
 * to skip */
static void open_block(struct css_sheet *s)
{
	const char *p = s->pre;
	int n = s->plen;

	while (n && is_space((unsigned char)*p)) {
		p++;
		n--;
	}
	s->depth = 0;
	if (n && *p == '@') {
		unsigned short m = cur_media(s);
		int keep = 1;

		if (starts(p + 1, (size_t)(n - 1), "media"))
			m = both_media(s, m, parse_media(s, p + 6, (size_t)(n - 6)));
		else if (!starts(p + 1, (size_t)(n - 1), "supports")
			&& !starts(p + 1, (size_t)(n - 1), "layer")
			&& !starts(p + 1, (size_t)(n - 1), "document")
			&& !starts(p + 1, (size_t)(n - 1), "-moz-document"))
			keep = 0;	/* @font-face, @keyframes, @page... */
		if (keep && s->ngroups < MAX_GROUPS) {
			s->group[s->ngroups++] = m;
			s->state = P_PRELUDE;
		} else
			s->state = P_SKIP;	/* (its } ends the skip) */
		s->plen = 0;
		s->plong = 0;
		return;
	}
	s->state = P_DECLS;
	s->dlen = 0;
}

/* a character, past comments */
static void put(struct css_sheet *s, int c)
{
	if (s->state == P_SKIP) {
		if (s->esc)
			s->esc = 0;
		else if (c == '\\')
			s->esc = 1;
		else if (s->quote) {
			if (c == s->quote || c == '\n')
				s->quote = 0;
		} else if (c == '"' || c == '\'')
			s->quote = c;
		else if (c == '{')
			s->depth++;
		else if (c == '}' && s->depth-- == 0) {
			s->state = P_PRELUDE;
			s->plen = 0;
			s->plong = 0;
		}
		return;
	}
	if (s->esc || s->quote || c == '\\' || c == '"' || c == '\'') {
		/* (inside strings and escapes, braces don't count) */
		if (s->esc)
			s->esc = 0;
		else if (c == '\\')
			s->esc = 1;
		else if (s->quote) {
			if (c == s->quote || c == '\n')
				s->quote = 0;
		} else
			s->quote = c;
		goto keep;
	}
	if (s->state == P_PRELUDE) {
		if (c == '{') {
			open_block(s);
			return;
		}
		if (c == ';') {			/* @import ...; @charset... */
			s->plen = 0;
			s->plong = 0;
			return;
		}
		if (c == '}') {			/* a group ends */
			if (s->ngroups > s->base)
				s->ngroups--;
			s->plen = 0;
			s->plong = 0;
			return;
		}
	} else {			/* P_DECLS */
		if (c == '{') {			/* a nested rule: not followed */
			s->depth++;
			return;
		}
		if (c == '}') {
			if (s->depth > 0) {
				s->depth--;
				return;
			}
			end_rule(s);
			s->state = P_PRELUDE;
			s->plen = 0;
			s->plong = 0;
			return;
		}
	}
keep:
	if (s->state == P_PRELUDE) {
		if (s->plen < PRELUDE_MAX)
			s->pre[s->plen++] = (char)c;
		else
			s->plong = 1;
	} else if (s->state == P_DECLS && s->depth == 0 && s->dlen < DECLS_MAX)
		s->decl[s->dlen++] = (char)c;
}

void css_begin(struct css_sheet *s, const char *media, nodeid pos)
{
	unsigned short m = 0;

	if (s == NULL)
		return;
	s->on = 1;
	s->order = (unsigned long)pos << 16;
	s->state = P_PRELUDE;
	s->depth = s->quote = s->esc = s->slash = s->star = s->comment = 0;
	s->plen = s->plong = s->dlen = 0;
	s->ngroups = s->base = 0;
	if (media) {
		m = parse_media(s, media, strlen(media));
		if (m == 0xFFFF || (m && s->media[m - 1].n == 0)) {
			s->on = 0;		/* print only */
			return;
		}
	}
	if (m) {
		s->group[s->ngroups++] = m;
		s->base = 1;
	}
}

/* characters put() must see one at a time; the rest go in runs */
static unsigned char special[256];

static void make_special(void)
{
	const char *p;

	for (p = "{};\"'\\/"; *p; p++)
		special[(unsigned char)*p] = 1;
}

/* n ordinary characters */
static void run(struct css_sheet *s, const unsigned char *p, size_t n)
{
	size_t k;

	if (s->state == P_PRELUDE) {
		k = PRELUDE_MAX - (size_t)s->plen;
		if (n > k) {
			n = k;
			s->plong = 1;
		}
		memcpy(s->pre + s->plen, p, n);
		s->plen += (int)n;
	} else if (s->state == P_DECLS && s->depth == 0) {
		k = DECLS_MAX - (size_t)s->dlen;
		if (n > k)
			n = k;
		memcpy(s->decl + s->dlen, p, n);
		s->dlen += (int)n;
	}
}

void css_feed(struct css_sheet *s, const char *text, size_t n)
{
	const unsigned char *p = (const unsigned char *)text, *e = p + n, *q;

	if (s == NULL || !s->on)
		return;
	if (!special['{'])
		make_special();
	while (p < e) {
		if (s->comment) {
			while (p < e) {
				int c = *p++;

				if (s->star && c == '/') {
					s->comment = 0;
					break;
				}
				s->star = c == '*';
			}
			continue;
		}
		if (s->slash) {
			s->slash = 0;
			if (*p == '*') {
				s->comment = 1;
				s->star = 0;
				p++;
				continue;
			}
			put(s, '/');
		}
		if (!s->quote && !s->esc) {
			for (q = p; q < e && !special[*q]; q++)
				;
			if (q > p) {
				run(s, p, (size_t)(q - p));
				p = q;
				continue;
			}
			if (*p == '/') {
				s->slash = 1;
				p++;
				continue;
			}
		}
		put(s, *p++);
	}
}

void css_end(struct css_sheet *s)
{
	if (s == NULL)
		return;
	if (s->slash && !s->comment)
		put(s, '/');
	/* a rule left open at the end counts, as in browsers */
	if (s->on && s->state == P_DECLS && s->depth == 0)
		end_rule(s);
	s->on = 0;
}

/* --- matching ---------------------------------------------------------------- */

/* is a word with hash h one of the space-separated words of list? */
static int word_in(const char *list, unsigned long h)
{
	const char *p = list;

	while (*p) {
		const char *w;

		while (*p && is_space((unsigned char)*p))
			p++;
		w = p;
		while (*p && !is_space((unsigned char)*p))
			p++;
		if (p > w && hash_bytes(w, (size_t)(p - w)) == h)
			return 1;
	}
	return 0;
}

/* an element, hashed: its tag, id and classes */
static void el_info(const struct doc *d, nodeid n, const char *id, const char *cls,
	struct elinfo *e)
{
	const char *p = cls;

	e->node = n;
	e->tag = (short)DOC_NODE(d, n)->tag;
	e->id = id && *id ? hash_bytes(id, strlen(id)) : 0;
	e->ncls = 0;
	while (p && *p && e->ncls < EL_CLASSES) {
		const char *w;

		while (*p && is_space((unsigned char)*p))
			p++;
		w = p;
		while (*p && !is_space((unsigned char)*p))
			p++;
		if (p > w)
			e->cls[e->ncls++] = hash_bytes(w, (size_t)(p - w));
	}
}

/* compound c against element e */
static int cmp_match(const struct cmp *c, const struct doc *d, const struct elinfo *e)
{
	int i, j;

	if (c->tag && e->tag != c->tag)
		return 0;
	if (c->id && e->id != c->id)
		return 0;
	for (i = 0; i < c->ncls; i++) {
		for (j = 0; j < e->ncls && e->cls[j] != c->cls[i]; j++)
			;
		if (j == e->ncls)
			return 0;
	}
	if (c->attr) {
		const char *v = doc_attr(d, e->node, c->attr);

		if (v == NULL)
			return 0;
		if (c->aop == '=' && hash_bytes(v, strlen(v)) != c->aval)
			return 0;
		if (c->aop == '~' && !word_in(v, c->aval))
			return 0;
	}
	return 1;
}

/* the compounds from k on, against the ancestors s->path[0..top) */
static int match_from(const struct css_sheet *s, const struct cmp *c, int k,
	int n_cmp, const struct doc *d, int top)
{
	int i;

	if (k == n_cmp)
		return 1;
	for (i = top - 1; i >= 0; i--) {
		if (cmp_match(&c[k], d, &s->path[i])
			&& match_from(s, c, k + 1, n_cmp, d, i))
			return 1;
		if (c[k - 1].comb == '>')
			break;		/* (the parent only) */
	}
	return 0;
}

/* does rule r hold for element e, whose ancestors are s->path[0..top)? */
static int rule_match(const struct css_sheet *s, const struct rule *r,
	const struct doc *d, const struct elinfo *e, int top)
{
	const struct cmp *c = &s->cmps[r->cmp];

	return cmp_match(&c[0], d, e) && match_from(s, c, 1, r->ncmp, d, top);
}

/* is (imp, spec, order) of a above b's? */
static int beats(int imp_a, const struct rule *a, int imp_b, const struct rule *b)
{
	if (b == NULL)
		return 1;
	if (imp_a != imp_b)
		return imp_a > imp_b;
	if (a->spec != b->spec)
		return a->spec > b->spec;
	return a->order > b->order;
}

/* the rules filed under key bucket b that hold, into the winners */
static void try_bucket(const struct css_sheet *s, unsigned b, const struct doc *d,
	const struct elinfo *e, int top, int vw,
	const struct rule **dw, const struct rule **vwin)
{
	int i;

	for (i = s->bucket[b]; i >= 0; i = s->rules[i].next) {
		const struct rule *r = &s->rules[i];

		if (!media_ok(s, r->media, vw) || !rule_match(s, r, d, e, top))
			continue;
		if (r->disp && beats(r->disp_imp, r, *dw ? (*dw)->disp_imp : 0, *dw))
			*dw = r;
		if (r->vis && beats(r->vis_imp, r, *vwin ? (*vwin)->vis_imp : 0, *vwin))
			*vwin = r;
	}
}

/* make s->path the ancestors of node, root first: those of the last
 * element asked about, cut back to node's parent, or found again */
static int set_path(struct css_sheet *s, const struct doc *d, nodeid node)
{
	nodeid p = DOC_NODE(d, node)->parent, chain[PATH_DEPTH];
	int k, n = 0;

	for (k = s->npath - 1; k >= 0 && s->path[k].node != p; k--)
		;
	if (k >= 0)
		return s->npath = k + 1;
	/* not there: up the tree from the parent */
	for (; p > 1 && n < PATH_DEPTH; p = DOC_NODE(d, p)->parent)
		chain[n++] = p;
	for (k = 0; k < n; k++) {
		nodeid a = chain[n - 1 - k];

		el_info(d, a, doc_attr(d, a, ATTR_ID), doc_attr(d, a, ATTR_CLASS),
			&s->path[k]);
	}
	return s->npath = n;
}

int css_display(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw)
{
	const struct rule *dw = NULL, *vwin = NULL;
	struct elinfo me;
	struct cmp probe;
	unsigned done[EL_CLASSES + 3];
	int ndone = 0, top, k, i;

	if (s == NULL || s->nrules == 0 || node == 0 || node >= d->nnodes)
		return CSS_UNSET;
	if (s->memo_n < d->nnodes) {
		unsigned long c = d->nnodes + 1024;
		unsigned char *q = xrealloc(s->memo, c);

		if (q) {
			memset(q + s->memo_n, 0, c - s->memo_n);
			s->memo = q;
			s->memo_n = c;
		}
	}
	if (s->memo_gen != s->gen || s->memo_vw != vw) {
		if (s->memo)
			memset(s->memo, 0, s->memo_n);
		s->memo_gen = s->gen;
		s->memo_vw = vw;
	}
	if (s->memo && node < s->memo_n && s->memo[node])
		return s->memo[node] == 2 ? CSS_HIDE : CSS_UNSET;

	top = set_path(s, d, node);
	el_info(d, node, id, cls, &me);
	/* (never the whole page: some hide it until their script runs) */
	if (me.tag != TAG_HTML && me.tag != TAG_BODY) {
		/* the buckets it can be filed under: its id, its classes, its
		 * tag, none; each once */
		memset(&probe, 0, sizeof probe);
		probe.id = me.id;
		if (me.id)
			done[ndone++] = bucket_of(&probe);
		probe.id = 0;
		probe.ncls = 1;
		for (i = 0; i < me.ncls; i++) {
			probe.cls[0] = me.cls[i];
			done[ndone++] = bucket_of(&probe);
		}
		probe.ncls = 0;
		probe.tag = me.tag;
		done[ndone++] = bucket_of(&probe);
		probe.tag = 0;
		done[ndone++] = bucket_of(&probe);
		for (i = 0; i < ndone; i++) {
			for (k = 0; k < i && done[k] != done[i]; k++)
				;
			if (k == i)
				try_bucket(s, done[i], d, &me, top, vw, &dw, &vwin);
		}
	}
	k = (dw && dw->disp == CSS_HIDE) || (vwin && vwin->vis == CSS_HIDE);
	if (s->memo && node < s->memo_n)
		s->memo[node] = (unsigned char)(k ? 2 : 1);
	/* its children's turn next: it is their parent */
	if (s->npath < PATH_DEPTH)
		s->path[s->npath++] = me;
	return k ? CSS_HIDE : CSS_UNSET;
}

int css_inline_shows(const char *css)
{
	const char *p;

	for (p = css; p && *p; p++) {
		const char *v;

		if (*p != 'd' && *p != 'D')
			continue;
		if (!starts(p, strlen(p), "display"))
			continue;
		v = p + 7;
		while (is_space((unsigned char)*v))
			v++;
		if (*v != ':')
			continue;
		v++;
		while (is_space((unsigned char)*v))
			v++;
		return *v && *v != ';' && !starts(v, strlen(v), "none");
	}
	return 0;
}
