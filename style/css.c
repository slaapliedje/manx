/*
 * css.c - which elements a page's style sheets hide, and how their text
 * looks (css.h).
 *
 * The text is read a character at a time (comments, strings and escapes
 * followed across pieces) into a rule's prelude and its declarations;
 * when the rule ends, its declarations are looked at, and only a rule
 * about display, visibility, list-style or the look of text is kept,
 * compiled: each selector a few compound selectors, the subject first,
 * with class and id names hashed. Colours go into a palette of the
 * sheet's, a rule keeping an index. Rules are filed by their subject's
 * id, else a class, else its tag, so an element is tried against a
 * handful of rules, not all of them. What the sheet says about each
 * element is remembered until the rules or the window's width change.
 * What browsers drop is dropped here too: a declaration with a '!' not
 * of !important, a name that isn't one, a value with a word too many or
 * of the wrong kind ("red pink", "200" for a length), and a rule whose
 * selector a stray ';' spoils.
 */
#include <string.h>
#include "os.h"
#include "tags.h"
#include "css.h"
#include "utf8.h"

#define MAX_RULES	3072		/* rules kept, at most */
#define MAX_TEXT_RULES	1536		/* ... of them about text only: a big
					 * sheet's colours mustn't crowd out
					 * what hides content */
#define MAX_CMPS	9216		/* compound selectors, all rules */
#define MAX_VARS	768		/* custom properties (a colour each) */
#define NVSLOT		1024		/* their table (a power of 2) */
#define PAL_MAX		253		/* distinct colours: index 1..253 */
#define NPSLOT		512		/* the palette's lookup table */
#define FG_VAR		254		/* a rule's colour is a custom property's */
#define FG_DEFAULT	255		/* back to the screen's own colour */
#define MAX_MEDIA	256		/* distinct @media conditions */
#define PRELUDE_MAX	1024		/* a longer selector list is dropped */
#define DECLS_MAX	2048		/* declaration bytes looked at */
#define MAX_GROUPS	16		/* @media/@supports nesting */
#define SEL_CMPS	6		/* compounds in one selector */
#define NBUCKET		1024
#define MEDIA_ALTS	4
#define MAX_BOX_DECLS	1536		/* rules' declarations about boxes */
#define CPOOL_MAX	131072		/* content: values, and url()s, all rules */
#define MAX_URLS	32		/* sheets' URLs (url()s are relative to) */

struct cmp {
	unsigned long id;		/* #id's hash; 0: none */
	unsigned long cls[3];		/* .class hashes */
	unsigned long aval[2];		/* [attr=v] / [attr~=v]: v's hash */
	short tag;			/* TAG_*; 0: any */
	unsigned char ncls, nattr;
	unsigned char attr[2];		/* attribute tests: ATTR_* */
	unsigned char aop[2];		/* 0: present; '=', '~' */
	unsigned char comb;		/* to the compound on its left: ' ', '>',
					 * '+' or '~' */
	unsigned char pseudo;		/* (the subject's) ::before 1, ::after 2 */
};

/* what a rule (or a style="") says of text: 0 where nothing */
struct tdecl {
	unsigned long fvar;		/* fg FG_VAR: the custom property's hash */
	unsigned char fg;		/* palette index, FG_VAR or FG_DEFAULT */
	unsigned char ffb;		/* FG_VAR: the fallback's (0: none) */
	unsigned char fw, fs, td, ta;	/* CSS_FW_*, CSS_FS_*, CSS_TD_*, CSS_TA_* */
	unsigned char fz;		/* font-size: s->fsz[fz - 1]; 0: none */
	unsigned char imp;		/* !important: T_FG, T_FW... bits */
};

#define T_FG	1
#define T_FW	2
#define T_FS	4
#define T_TD	8
#define T_TA	16
#define T_FZ	32
#define FSZ_MAX	255			/* distinct font sizes in a sheet */

struct rule {
	unsigned long spec;		/* ids << 16 | classes << 8 | types */
	unsigned long order;
	unsigned short cmp, ncmp;	/* s->cmps[cmp...], the subject first */
	unsigned short media;		/* 0: all; else s->media[media - 1] */
	unsigned char disp, vis, ls;	/* CSS_SHOW / CSS_HIDE / CSS_UNSET */
	unsigned char disp_imp, vis_imp, ls_imp;	/* !important */
	struct tdecl t;			/* the look of text */
	int box;			/* its box: s->boxes[box], -1: none */
	int next;			/* the next rule in its bucket, -1 */
	unsigned long cont;		/* content: s->cpool + cont - 1; 0: none */
	unsigned char cont_imp;
	unsigned char pseudo;		/* about its ::before (1), ::after (2) */
};

/* what a rule says of the box, and which of it is !important */
struct bdecl {
	struct css_box b;
	unsigned long imp, imp2;	/* CSS_B_*, CSS_B2_* */
};

/* the box's properties, CSS_B_* then CSS_B2_*, numbered */
#define BOX_NBITS	(CSS_B_NBITS + CSS_B2_NBITS)

/* is property k among the bits w (CSS_B_*) and w2 (CSS_B2_*)? */
static int box_bit(unsigned long w, unsigned long w2, int k)
{
	return k < CSS_B_NBITS ? (w >> k & 1) != 0
		: (w2 >> (k - CSS_B_NBITS) & 1) != 0;
}

/* a custom property set on :root, html or body (only colours are kept) */
struct cvar {
	unsigned long name;		/* its hash; 0: a free slot */
	unsigned long ref;		/* fg FG_VAR: var(--ref) */
	unsigned char fg, ffb;		/* as in struct tdecl */
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

/* a box css_box worked out, kept for the nodes it holds for */
struct bxent {
	struct css_box b;
	int next;			/* 1 + the next of its hash's chain, 0: last */
};
#define BX_HASH		512

struct css_sheet {
	struct rule *rules;
	int nrules, rules_cap;
	int ntext;			/* rules about text only */
	unsigned long pal[PAL_MAX];	/* the colours: index i is pal[i - 1] */
	int npal;
	short pslot[NPSLOT];		/* colour -> index, 0: empty */
	struct css_len fsz[FSZ_MAX];	/* the font sizes rules say */
	int nfsz;
	struct cvar *vars;		/* NVSLOT of them, once there is one */
	int nvars;
	struct cmp *cmps;
	int ncmps, cmps_cap;
	struct bdecl *boxes;
	int nboxes, boxes_cap;
	char *cpool;			/* content: values, NUL-ended; and
					 * url()s, after their urls[] index + 1 */
	unsigned long cpool_n, cpool_cap;
	char *urls[MAX_URLS];		/* the sheets' URLs */
	int nurls;
	int url_at;			/* the text being read: urls[url_at - 1]'s
					 * (0: the page's) */
	int want;			/* matching for a ::before (1), ::after (2) */
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
	/* the last element sibling found before one (sib_at, a child of
	 * sib_par): sib_prev, 0 for none */
	nodeid sib_par, sib_at, sib_prev;
	/* what was said of each node, M_* below; 0: not asked */
	unsigned long *memo;
	unsigned long memo_n;
	unsigned memo_gen;
	int memo_vw;
	/* ... and of its box, as the rules put it over nothing: bmemo[node]
	 * is 1 + its index in bx[] (0: not asked, or nothing said); nodes
	 * whose boxes come out the same share one, and most do. Reset with
	 * memo. */
	unsigned short *bmemo;
	struct bxent *bx;
	int nbx, bx_cap;
	int bx_head[BX_HASH];		/* 1 + the first of each hash's chain */
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

/* is s (n bytes) the keyword w, case aside? */
static int is_kw(const char *s, size_t n, const char *w)
{
	return n == strlen(w) && starts(s, n, w);
}

/* is a display value (not none) one there is? its first word, as
 * "block flow", "inline flex" put two */
static int display_ok(const char *v, size_t vn)
{
	static const char *const kw[] = { "block", "inline", "inline-block",
		"list-item", "flex", "inline-flex", "grid", "inline-grid", "table",
		"inline-table", "table-row", "table-cell", "table-row-group",
		"table-header-group", "table-footer-group", "table-column",
		"table-column-group", "table-caption", "contents", "flow-root",
		"flow", "run-in", "ruby", "ruby-text", "ruby-base", "math",
		"-webkit-box", "-webkit-inline-box", "-ms-flexbox", "initial",
		"inherit", "unset", "revert", "revert-layer", NULL };
	size_t n;
	int i;

	for (n = 0; n < vn && !is_space((unsigned char)v[n]); n++)
		;
	for (i = 0; kw[i]; i++)
		if (is_kw(v, n, kw[i]))
			return 1;
	return 0;
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

int css_boxes = 1;

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
	xfree(s->boxes);
	xfree(s->cpool);
	while (s->nurls > 0)
		xfree(s->urls[--s->nurls]);
	xfree(s->vars);
	xfree(s->memo);
	xfree(s->bmemo);
	xfree(s->bx);
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

/* one query of a list, q[i..e): whether it can hold (print and the like
 * can't), its width's range into *min, *max (0: none) */
static int one_query(const char *q, size_t i, size_t e, long *min, long *max)
{
	int ok = 1, neg = 0, type = 0;

	*min = *max = 0;
	while (i < e) {
		int c = (unsigned char)q[i];

		if (is_space(c)) {
			i++;
		} else if (c == '(') {
			size_t j = i + 1;
			int r;

			while (j < e && q[j] != ')')
				j++;
			r = feature(q + i + 1, j - i - 1, min, max);
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
		ok = type < 0 && *max == 0 && *min == 0;	/* "not print" */
	return ok;
}

int css_media_matches(const char *q, int vw)
{
	size_t n = strlen(q), i = 0;

	while (i <= n) {
		size_t e = i;
		long min, max;

		while (e < n && q[e] != ',')
			e++;
		if (one_query(q, i, e, &min, &max) && (max == 0 || min <= max)
			&& vw >= min && (max == 0 || vw <= max))
			return 1;
		i = e + 1;
	}
	return 0;
}

/* a media query list: 0 for "all", else 1 + its entry in s->media */
static unsigned short parse_media(struct css_sheet *s, const char *q, size_t n)
{
	struct media m;
	size_t i = 0;

	memset(&m, 0, sizeof m);
	while (i <= n) {
		size_t e = i;
		long min, max;
		int ok;

		while (e < n && q[e] != ',')
			e++;
		ok = one_query(q, i, e, &min, &max);
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

/* --- colours ---------------------------------------------------------------- */

/* CSS's named colours, sorted */
static const struct { const char *name; unsigned long rgb; } named[] = {
	{ "aliceblue", 0xF0F8FFUL }, { "antiquewhite", 0xFAEBD7UL },
	{ "aqua", 0x00FFFFUL }, { "aquamarine", 0x7FFFD4UL },
	{ "azure", 0xF0FFFFUL }, { "beige", 0xF5F5DCUL },
	{ "bisque", 0xFFE4C4UL }, { "black", 0x000000UL },
	{ "blanchedalmond", 0xFFEBCDUL }, { "blue", 0x0000FFUL },
	{ "blueviolet", 0x8A2BE2UL }, { "brown", 0xA52A2AUL },
	{ "burlywood", 0xDEB887UL }, { "cadetblue", 0x5F9EA0UL },
	{ "chartreuse", 0x7FFF00UL }, { "chocolate", 0xD2691EUL },
	{ "coral", 0xFF7F50UL }, { "cornflowerblue", 0x6495EDUL },
	{ "cornsilk", 0xFFF8DCUL }, { "crimson", 0xDC143CUL },
	{ "cyan", 0x00FFFFUL }, { "darkblue", 0x00008BUL },
	{ "darkcyan", 0x008B8BUL }, { "darkgoldenrod", 0xB8860BUL },
	{ "darkgray", 0xA9A9A9UL }, { "darkgreen", 0x006400UL },
	{ "darkgrey", 0xA9A9A9UL }, { "darkkhaki", 0xBDB76BUL },
	{ "darkmagenta", 0x8B008BUL }, { "darkolivegreen", 0x556B2FUL },
	{ "darkorange", 0xFF8C00UL }, { "darkorchid", 0x9932CCUL },
	{ "darkred", 0x8B0000UL }, { "darksalmon", 0xE9967AUL },
	{ "darkseagreen", 0x8FBC8FUL }, { "darkslateblue", 0x483D8BUL },
	{ "darkslategray", 0x2F4F4FUL }, { "darkslategrey", 0x2F4F4FUL },
	{ "darkturquoise", 0x00CED1UL }, { "darkviolet", 0x9400D3UL },
	{ "deeppink", 0xFF1493UL }, { "deepskyblue", 0x00BFFFUL },
	{ "dimgray", 0x696969UL }, { "dimgrey", 0x696969UL },
	{ "dodgerblue", 0x1E90FFUL }, { "firebrick", 0xB22222UL },
	{ "floralwhite", 0xFFFAF0UL }, { "forestgreen", 0x228B22UL },
	{ "fuchsia", 0xFF00FFUL }, { "gainsboro", 0xDCDCDCUL },
	{ "ghostwhite", 0xF8F8FFUL }, { "gold", 0xFFD700UL },
	{ "goldenrod", 0xDAA520UL }, { "gray", 0x808080UL },
	{ "green", 0x008000UL }, { "greenyellow", 0xADFF2FUL },
	{ "grey", 0x808080UL }, { "honeydew", 0xF0FFF0UL },
	{ "hotpink", 0xFF69B4UL }, { "indianred", 0xCD5C5CUL },
	{ "indigo", 0x4B0082UL }, { "ivory", 0xFFFFF0UL },
	{ "khaki", 0xF0E68CUL }, { "lavender", 0xE6E6FAUL },
	{ "lavenderblush", 0xFFF0F5UL }, { "lawngreen", 0x7CFC00UL },
	{ "lemonchiffon", 0xFFFACDUL }, { "lightblue", 0xADD8E6UL },
	{ "lightcoral", 0xF08080UL }, { "lightcyan", 0xE0FFFFUL },
	{ "lightgoldenrodyellow", 0xFAFAD2UL }, { "lightgray", 0xD3D3D3UL },
	{ "lightgreen", 0x90EE90UL }, { "lightgrey", 0xD3D3D3UL },
	{ "lightpink", 0xFFB6C1UL }, { "lightsalmon", 0xFFA07AUL },
	{ "lightseagreen", 0x20B2AAUL }, { "lightskyblue", 0x87CEFAUL },
	{ "lightslategray", 0x778899UL }, { "lightslategrey", 0x778899UL },
	{ "lightsteelblue", 0xB0C4DEUL }, { "lightyellow", 0xFFFFE0UL },
	{ "lime", 0x00FF00UL }, { "limegreen", 0x32CD32UL },
	{ "linen", 0xFAF0E6UL }, { "magenta", 0xFF00FFUL },
	{ "maroon", 0x800000UL }, { "mediumaquamarine", 0x66CDAAUL },
	{ "mediumblue", 0x0000CDUL }, { "mediumorchid", 0xBA55D3UL },
	{ "mediumpurple", 0x9370DBUL }, { "mediumseagreen", 0x3CB371UL },
	{ "mediumslateblue", 0x7B68EEUL }, { "mediumspringgreen", 0x00FA9AUL },
	{ "mediumturquoise", 0x48D1CCUL }, { "mediumvioletred", 0xC71585UL },
	{ "midnightblue", 0x191970UL }, { "mintcream", 0xF5FFFAUL },
	{ "mistyrose", 0xFFE4E1UL }, { "moccasin", 0xFFE4B5UL },
	{ "navajowhite", 0xFFDEADUL }, { "navy", 0x000080UL },
	{ "oldlace", 0xFDF5E6UL }, { "olive", 0x808000UL },
	{ "olivedrab", 0x6B8E23UL }, { "orange", 0xFFA500UL },
	{ "orangered", 0xFF4500UL }, { "orchid", 0xDA70D6UL },
	{ "palegoldenrod", 0xEEE8AAUL }, { "palegreen", 0x98FB98UL },
	{ "paleturquoise", 0xAFEEEEUL }, { "palevioletred", 0xDB7093UL },
	{ "papayawhip", 0xFFEFD5UL }, { "peachpuff", 0xFFDAB9UL },
	{ "peru", 0xCD853FUL }, { "pink", 0xFFC0CBUL },
	{ "plum", 0xDDA0DDUL }, { "powderblue", 0xB0E0E6UL },
	{ "purple", 0x800080UL }, { "rebeccapurple", 0x663399UL },
	{ "red", 0xFF0000UL }, { "rosybrown", 0xBC8F8FUL },
	{ "royalblue", 0x4169E1UL }, { "saddlebrown", 0x8B4513UL },
	{ "salmon", 0xFA8072UL }, { "sandybrown", 0xF4A460UL },
	{ "seagreen", 0x2E8B57UL }, { "seashell", 0xFFF5EEUL },
	{ "sienna", 0xA0522DUL }, { "silver", 0xC0C0C0UL },
	{ "skyblue", 0x87CEEBUL }, { "slateblue", 0x6A5ACDUL },
	{ "slategray", 0x708090UL }, { "slategrey", 0x708090UL },
	{ "snow", 0xFFFAFAUL }, { "springgreen", 0x00FF7FUL },
	{ "steelblue", 0x4682B4UL }, { "tan", 0xD2B48CUL },
	{ "teal", 0x008080UL }, { "thistle", 0xD8BFD8UL },
	{ "tomato", 0xFF6347UL }, { "turquoise", 0x40E0D0UL },
	{ "violet", 0xEE82EEUL }, { "wheat", 0xF5DEB3UL },
	{ "white", 0xFFFFFFUL }, { "whitesmoke", 0xF5F5F5UL },
	{ "yellow", 0xFFFF00UL }, { "yellowgreen", 0x9ACD32UL }
};
#define NNAMED	(sizeof named / sizeof named[0])

/* what a colour value is */
enum { CV_NONE, CV_RGB, CV_VAR, CV_DEFAULT };

struct cval {
	int kind;			/* CV_* */
	unsigned long rgb;		/* CV_RGB */
	unsigned long var;		/* CV_VAR: the custom property's hash */
	int fbk;			/* ... and its fallback: CV_RGB, CV_DEFAULT
					 * or CV_NONE */
	unsigned long fbrgb;
};

/* a number at p (before e): its value in thousandths into *v, and
 * whether a % followed; the end, or NULL if there is none */
static const char *number(const char *p, const char *e, long *v, int *pct)
{
	long ip = 0, fr = 0, scale = 1000;
	int neg = 0, any = 0;

	while (p < e && (is_space((unsigned char)*p) || *p == ','))
		p++;
	if (p < e && (*p == '-' || *p == '+'))
		neg = *p++ == '-';
	for (; p < e && *p >= '0' && *p <= '9'; p++, any = 1)
		if (ip < 1000000)
			ip = ip * 10 + (*p - '0');
	if (p < e && *p == '.')
		for (p++; p < e && *p >= '0' && *p <= '9'; p++, any = 1)
			if (scale > 1) {
				scale /= 10;
				fr += (*p - '0') * scale;
			}
	if (!any)
		return NULL;
	*v = (ip * 1000 + fr) * (neg ? -1 : 1);
	*pct = p < e && *p == '%';
	if (*pct)
		p++;
	/* (units: deg, turn... only a hue has one, taken as degrees) */
	while (p < e && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
		p++;
	return p;
}

static long clamp(long v, long lo, long hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* the alpha after a colour's components at p, thousandths (1000: none) */
static long alpha_at(const char *p, const char *e)
{
	long a;
	int pct;

	while (p < e && (is_space((unsigned char)*p) || *p == ',' || *p == '/'))
		p++;
	if (number(p, e, &a, &pct) == NULL)
		return 1000;
	return clamp(pct ? a / 100 : a, 0, 1000);
}

/* r, g, b (0-255) at alpha a (thousandths), over a white page */
static unsigned long blend(long r, long g, long b, long a)
{
	if (a < 1000) {
		r = (r * a + 255 * (1000 - a)) / 1000;
		g = (g * a + 255 * (1000 - a)) / 1000;
		b = (b * a + 255 * (1000 - a)) / 1000;
	}
	return (unsigned long)clamp(r, 0, 255) << 16 | (unsigned long)clamp(g, 0, 255) << 8
		| (unsigned long)clamp(b, 0, 255);
}

static int hexval(int c)
{
	c = lower(c);
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* where the argument list from p (just past its '(') ends: its ')' */
static const char *close_paren(const char *p, const char *e)
{
	int depth = 0;

	for (; p < e; p++) {
		if (*p == '(')
			depth++;
		else if (*p == ')' && depth-- == 0)
			return p;
	}
	return e;
}

/* where the argument from p ends: a ',' or ')' at its level */
static const char *arg_end(const char *p, const char *e)
{
	int depth = 0;

	for (; p < e; p++) {
		if (*p == '(')
			depth++;
		else if (*p == ')') {
			if (depth-- == 0)
				return p;
		} else if (*p == ',' && depth == 0)
			return p;
	}
	return e;
}

static void parse_color(const char *v, size_t vn, struct cval *c, int depth);

/* rgb(), rgba(): comma or space separated, numbers or percentages */
static int color_rgb(const char *p, const char *e, unsigned long *rgb)
{
	long c[3], a;
	int i, pct;

	for (i = 0; i < 3; i++) {
		if ((p = number(p, e, &a, &pct)) == NULL)
			return 0;
		c[i] = pct ? a * 255 / 100000 : a / 1000;
	}
	a = alpha_at(p, e);
	if (a == 0)
		return 0;		/* transparent: no colour said */
	*rgb = blend(c[0], c[1], c[2], a);
	return 1;
}

/* hsl(), hsla() */
static int color_hsl(const char *p, const char *e, unsigned long *rgb)
{
	long h, sat, l, a, c, x, m, hp, r1 = 0, g1 = 0, b1 = 0;
	int pct;

	if ((p = number(p, e, &h, &pct)) == NULL
		|| (p = number(p, e, &sat, &pct)) == NULL
		|| (p = number(p, e, &l, &pct)) == NULL)
		return 0;
	a = alpha_at(p, e);
	if (a == 0)
		return 0;
	h = ((h % 360000) + 360000) % 360000;
	sat = clamp(sat / 100, 0, 1000);	/* (percent, as thousandths) */
	l = clamp(l / 100, 0, 1000);
	c = (1000 - (2 * l - 1000 < 0 ? 1000 - 2 * l : 2 * l - 1000)) * sat / 1000;
	hp = h / 60;				/* 0..5999 */
	x = c * (1000 - ((hp % 2000) - 1000 < 0 ? 1000 - hp % 2000 : hp % 2000 - 1000)) / 1000;
	switch (hp / 1000) {
	case 0: r1 = c; g1 = x; break;
	case 1: r1 = x; g1 = c; break;
	case 2: g1 = c; b1 = x; break;
	case 3: g1 = x; b1 = c; break;
	case 4: r1 = x; b1 = c; break;
	default: r1 = c; b1 = x; break;
	}
	m = l - c / 2;
	*rgb = blend((r1 + m) * 255 / 1000, (g1 + m) * 255 / 1000,
		(b1 + m) * 255 / 1000, a);
	return 1;
}

/* a colour value (spaces around it and !important already cut) */
static void parse_color(const char *v, size_t vn, struct cval *c, int depth)
{
	const char *e = v + vn;
	size_t n;

	memset(c, 0, sizeof *c);
	while (v < e && is_space((unsigned char)*v))
		v++;
	while (e > v && is_space((unsigned char)e[-1]))
		e--;
	n = (size_t)(e - v);
	if (n == 0 || depth > 3)
		return;
	if (*v == '#') {
		int d[8], k;

		for (k = 0; k < 8 && v + 1 + k < e; k++)
			if ((d[k] = hexval((unsigned char)v[1 + k])) < 0)
				return;
		if (v + 1 + k != e)
			return;
		if (k == 3 || k == 4) {
			long a = k == 4 ? d[3] * 17 * 1000 / 255 : 1000;

			if (a == 0)
				return;
			c->rgb = blend(d[0] * 17, d[1] * 17, d[2] * 17, a);
		} else if (k == 6 || k == 8) {
			long a = k == 8 ? (d[6] * 16 + d[7]) * 1000 / 255 : 1000;

			if (a == 0)
				return;
			c->rgb = blend(d[0] * 16 + d[1], d[2] * 16 + d[3], d[4] * 16 + d[5], a);
		} else
			return;
		c->kind = CV_RGB;
		return;
	}
	if (starts(v, n, "var(")) {
		const char *p = v + 4, *ne, *ae;
		struct cval fb;

		while (p < e && is_space((unsigned char)*p))
			p++;
		ae = arg_end(p, e);
		for (ne = ae; ne > p && is_space((unsigned char)ne[-1]); ne--)
			;
		if (ne - p < 3 || p[0] != '-' || p[1] != '-')
			return;
		c->kind = CV_VAR;
		c->var = hash_bytes(p, (size_t)(ne - p));
		if (ae < e && *ae == ',') {
			parse_color(ae + 1, (size_t)(close_paren(ae + 1, e) - ae - 1), &fb, depth + 1);
			if (fb.kind == CV_RGB || fb.kind == CV_DEFAULT) {
				c->fbk = fb.kind;
				c->fbrgb = fb.rgb;
			}
		}
		return;
	}
	if (starts(v, n, "light-dark(")) {
		/* a light page: the first */
		const char *p = v + 11;

		parse_color(p, (size_t)(arg_end(p, e) - p), c, depth + 1);
		return;
	}
	if (starts(v, n, "rgb(") || starts(v, n, "rgba(")) {
		const char *p = v + (v[3] == '(' ? 4 : 5);

		if (color_rgb(p, close_paren(p, e), &c->rgb))
			c->kind = CV_RGB;
		return;
	}
	if (starts(v, n, "hsl(") || starts(v, n, "hsla(")) {
		const char *p = v + (v[3] == '(' ? 4 : 5);

		if (color_hsl(p, close_paren(p, e), &c->rgb))
			c->kind = CV_RGB;
		return;
	}
	if (starts(v, n, "initial") || starts(v, n, "revert") || starts(v, n, "canvastext")) {
		c->kind = CV_DEFAULT;
		return;
	}
	{
		/* a name: binary search, case aside */
		int lo = 0, hi = (int)NNAMED - 1;

		if (n > 24)
			return;
		while (lo <= hi) {
			int mid = (lo + hi) / 2, cmp = 0;
			const char *nm = named[mid].name;
			size_t i;

			for (i = 0; i < n && nm[i]; i++)
				if ((cmp = lower((unsigned char)v[i]) - nm[i]) != 0)
					break;
			if (cmp == 0)
				cmp = i < n ? 1 : nm[i] ? -1 : 0;
			if (cmp == 0) {
				c->kind = CV_RGB;
				c->rgb = named[mid].rgb;
				return;
			}
			if (cmp < 0)
				hi = mid - 1;
			else
				lo = mid + 1;
		}
	}
	/* inherit, unset, currentcolor, transparent, color-mix()...: nothing */
}

/* what is_color says a word is */
enum { COL_NO, COL_RGB, COL_CLEAR, COL_OTHER };

/* Is word w a colour? COL_RGB (its value in *c), COL_CLEAR (transparent),
 * COL_OTHER (one, but not known here: currentcolor, var(), lab()...), or
 * COL_NO: in a shorthand, it's some other part, or a mistake. */
static int is_color(const char *w, size_t wn, struct cval *c)
{
	static const char *const fn[] = { "rgb(", "rgba(", "hsl(", "hsla(", "hwb(",
		"lab(", "lch(", "oklab(", "oklch(", "color(", "color-mix(",
		"light-dark(", NULL };
	int i;

	parse_color(w, wn, c, 0);
	if (c->kind == CV_RGB)
		return COL_RGB;
	if (c->kind != CV_NONE)
		return COL_OTHER;
	if (is_kw(w, wn, "transparent"))
		return COL_CLEAR;
	if (is_kw(w, wn, "currentcolor"))
		return COL_OTHER;
	if (*w == '#') {
		/* (well formed, so its alpha is 0: #0000) */
		for (i = 1; (size_t)i < wn && hexval((unsigned char)w[i]) >= 0; i++)
			;
		return (size_t)i == wn && (wn == 5 || wn == 9) ? COL_CLEAR : COL_NO;
	}
	if (wn > 4 && w[wn - 1] == ')')
		for (i = 0; fn[i]; i++)
			if (starts(w, wn, fn[i]))
				/* (rgb() and hsl() that parse_color refuses have
				 * alpha 0: rgba(0,0,0,0)) */
				return i < 4 ? COL_CLEAR : COL_OTHER;
	return COL_NO;
}

/* a border's colour word: 1 and *rgb (0xRRGGBB, CSS_BG_NONE or
 * CSS_BG_CURRENT), 0 for one not known here (var(), lab()...), -1 for no
 * colour */
static int border_rgb(const char *w, size_t wn, unsigned long *rgb)
{
	struct cval c;
	int ck = is_color(w, wn, &c);

	if (ck == COL_RGB)
		*rgb = c.rgb;
	else if (ck == COL_CLEAR)
		*rgb = CSS_BG_NONE;
	else if (is_kw(w, wn, "currentcolor"))
		*rgb = CSS_BG_CURRENT;
	else
		return ck == COL_OTHER ? 0 : -1;
	return 1;
}

/* var(--x, fallback): the fallback (a word: *f, *fn), when there is one */
static int var_fallback(const char *w, size_t wn, const char **f, size_t *fn)
{
	const char *e = w + wn, *a;

	if (!starts(w, wn, "var(") || w[wn - 1] != ')')
		return 0;
	a = arg_end(w + 4, e - 1);
	if (a >= e - 1 || *a != ',')
		return 0;
	for (a++; a < e - 1 && is_space((unsigned char)*a); a++)
		;
	for (e--; e > a && is_space((unsigned char)e[-1]); e--)
		;
	*f = a;
	*fn = (size_t)(e - a);
	return *fn > 0;
}

/* is word w one whose value is only known later (var(), calc()...)? In a
 * shorthand it may be any of the parts */
static int is_wild(const char *w, size_t wn)
{
	static const char *const fn[] = { "var(", "calc(", "min(", "max(", "clamp(",
		"env(", "attr(", NULL };
	int i;

	for (i = 0; fn[i]; i++)
		if (starts(w, wn, fn[i]))
			return 1;
	return 0;
}

/* the palette index of colour rgb, added if new; 0: no room */
static int pal_index(struct css_sheet *s, unsigned long rgb)
{
	unsigned h = (unsigned)((rgb * 2654435761UL) >> 7) & (NPSLOT - 1);

	while (s->pslot[h]) {
		if (s->pal[s->pslot[h] - 1] == rgb)
			return s->pslot[h];
		h = (h + 1) & (NPSLOT - 1);
	}
	if (s->npal == PAL_MAX)
		return 0;
	s->pal[s->npal++] = rgb;
	s->pslot[h] = (short)s->npal;
	return s->npal;
}

/* a colour value as a rule keeps it: into t's fg (and fvar, ffb) */
static void color_decl(struct css_sheet *s, const struct cval *c, struct tdecl *t)
{
	int k;

	switch (c->kind) {
	case CV_RGB:
		if ((k = pal_index(s, c->rgb)) != 0)
			t->fg = (unsigned char)k;
		break;
	case CV_DEFAULT:
		t->fg = FG_DEFAULT;
		break;
	case CV_VAR:
		t->fg = FG_VAR;
		t->fvar = c->var;
		t->ffb = c->fbk == CV_DEFAULT ? FG_DEFAULT
			: c->fbk == CV_RGB ? (unsigned char)pal_index(s, c->fbrgb) : 0;
		break;
	}
}

/* the slot of custom property name in s->vars (NULL: no table) */
static struct cvar *var_slot(const struct css_sheet *s, unsigned long name)
{
	unsigned h = (unsigned)(name ^ name >> 11) & (NVSLOT - 1);

	if (s->vars == NULL)
		return NULL;
	while (s->vars[h].name && s->vars[h].name != name)
		h = (h + 1) & (NVSLOT - 1);
	return &s->vars[h];
}

/* custom property name set to a colour (later ones replace it) */
static void set_var(struct css_sheet *s, unsigned long name, const struct tdecl *t)
{
	struct cvar *v;

	if (s->vars == NULL) {
		if ((s->vars = xmalloc(NVSLOT * sizeof *s->vars)) == NULL)
			return;
		memset(s->vars, 0, NVSLOT * sizeof *s->vars);
	}
	v = var_slot(s, name);
	if (v->name == 0) {
		if (s->nvars == MAX_VARS)
			return;
		s->nvars++;
		v->name = name;
	}
	v->fg = t->fg;
	v->ref = t->fvar;
	v->ffb = t->ffb;
	s->gen++;
}

/* what custom property name comes to: a palette index, FG_DEFAULT or 0 */
static int var_fg(const struct css_sheet *s, unsigned long name, int depth)
{
	const struct cvar *v = var_slot(s, name);

	if (v == NULL || v->name == 0 || depth > 6)
		return 0;
	if (v->fg == FG_VAR) {
		int k = var_fg(s, v->ref, depth + 1);

		return k ? k : v->ffb;
	}
	return v->fg;
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

		/* (a pseudo-element ends the selector) */
		if (cur.pseudo && !is_space(c))
			return 0;
		if (is_space(c)) {
			if (have && !comb)
				comb = ' ';
			p++;
			continue;
		}
		if (c == '>' || c == '+' || c == '~') {
			if (!have || (comb && comb != ' '))
				return 0;
			comb = c;
			p++;
			continue;
		}
		if (c == '|' || c == ',')
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
		} else if (c == ':') {
			/* :root, :link, ::before and ::after; any other state,
			 * or pseudo-element, isn't followed */
			int k, two;

			p++;
			if ((two = p < e && *p == ':') != 0)
				p++;
			if ((k = read_name(&p, e, name, sizeof name)) < 0)
				return 0;
			for (i = 0; i < k; i++)
				name[i] = (char)lower((unsigned char)name[i]);
			if (strcmp(name, "before") == 0 || strcmp(name, "after") == 0) {
				cur.pseudo = name[0] == 'b' ? 1 : 2;
				*spec += 1;
				have = 1;
				continue;
			}
			if (two)
				return 0;
			if (strcmp(name, "root") == 0) {
				if (cur.tag && cur.tag != TAG_HTML)
					return 0;
				cur.tag = TAG_HTML;
			} else if (strcmp(name, "link") == 0 || strcmp(name, "any-link") == 0) {
				if (cur.nattr == 2 || (cur.tag && cur.tag != TAG_A))
					return 0;
				cur.tag = TAG_A;
				cur.attr[cur.nattr] = ATTR_HREF;
				cur.aop[cur.nattr++] = 0;
			} else
				return 0;
			*spec += 1UL << 8;
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
			if (cur.nattr == 2 || (k = read_name(&p, e, name, sizeof name)) < 0)
				return 0;
			for (i = 0; i < k; i++)
				name[i] = (char)lower((unsigned char)name[i]);
			if ((a = attr_lookup(name)) == ATTR_NONE)
				return 0;	/* (not kept: can't be tested) */
			cur.attr[cur.nattr] = (unsigned char)a;
			while (p < e && is_space((unsigned char)*p))
				p++;
			if (p < e && *p == ']') {
				p++;
			} else {
				char q;
				const char *v;

				if (p < e && *p == '~' && p + 1 < e && p[1] == '=') {
					cur.aop[cur.nattr] = '~';
					p += 2;
				} else if (p < e && *p == '=') {
					cur.aop[cur.nattr] = '=';
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
					cur.aval[cur.nattr] = hash_bytes(v, (size_t)(p - v));
					p++;
				} else {
					if ((k = read_name(&p, e, name, sizeof name)) < 0)
						return 0;
					cur.aval[cur.nattr] = hash_bytes(name, (size_t)k);
				}
				while (p < e && is_space((unsigned char)*p))
					p++;
				if (p < e && (*p == 'i' || *p == 's')) /* (flags) */
					return 0;
				if (p == e || *p != ']')
					return 0;
				p++;
			}
			cur.nattr++;
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
	if (!have || (comb && comb != ' '))
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

/* do declarations d (n bytes) mention display, visibility, list-style,
 * color, font, text- or a custom property at all? (rules that don't are
 * dropped without reading them; a false yes only costs a reading) */
static int mentions(const char *d, int n)
{
	int i;

	if (memchr(d, '\\', (size_t)n))
		return 1;
	for (i = 1; i + 4 < n; i++)
		switch (d[i]) {
		case 'i':
			if (d[i + 1] == 's'
				&& ((d[i + 2] == 'p' && d[i + 3] == 'l' && d[i - 1] == 'd')
				|| (d[i + 2] == 'i' && d[i + 3] == 'b' && d[i - 1] == 'v')
				|| (d[i + 2] == 't' && d[i + 3] == '-' && d[i - 1] == 'l')))
				return 1;
			break;
		case 'o':
			if ((d[i - 1] == 'c' && d[i + 1] == 'l' && d[i + 2] == 'o'
				&& d[i + 3] == 'r')
				|| (d[i - 1] == 'c' && d[i + 1] == 'n' && d[i + 2] == 't'
				&& d[i + 3] == 'e')
				|| (d[i - 1] == 'f' && d[i + 1] == 'n' && d[i + 2] == 't'))
				return 1;
			break;
		case 'x':
			if (d[i - 1] == 'e' && d[i + 1] == 't' && d[i + 2] == '-')
				return 1;
			break;
		case '-':
			if (d[i - 1] == '-')
				return 1;
			break;
		}
	return 0;
}

/* one word of v (vn bytes) equal to w, case aside? */
static int has_word(const char *v, size_t vn, const char *w)
{
	size_t i = 0, k = strlen(w);

	while (i < vn) {
		size_t j;

		while (i < vn && (is_space((unsigned char)v[i]) || v[i] == ','))
			i++;
		for (j = i; j < vn && !is_space((unsigned char)v[j]) && v[j] != ','
			&& v[j] != '/'; j++)
			;
		if (j - i == k && starts(v + i, k, w))
			return 1;
		i = j + 1;
	}
	return 0;
}

/* a font-weight: CSS_FW_*, 0 for nothing (inherit...) */
static int weight(const char *v, size_t vn)
{
	long n = 0;
	size_t i;

	if (starts(v, vn, "bold"))		/* (bolder too) */
		return CSS_FW_BOLD;
	if (starts(v, vn, "normal") || starts(v, vn, "lighter") || starts(v, vn, "initial"))
		return CSS_FW_NORMAL;
	for (i = 0; i < vn && v[i] >= '0' && v[i] <= '9'; i++)
		n = n * 10 + (v[i] - '0');
	if (i == 0 || n > 1000)
		return 0;
	return n >= 600 ? CSS_FW_BOLD : CSS_FW_NORMAL;
}

/* a font-size value (a length, a % or a keyword) into *l: 1, or 0 */
static int font_size(const char *v, size_t vn, struct css_len *l)
{
	static const struct {
		const char *k;
		short px;
	} kw[] = { { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 },
		{ "medium", 16 }, { "large", 18 }, { "x-large", 24 },
		{ "xx-large", 32 }, { "xxx-large", 48 } };
	const char *e = v + vn, *q;
	long t;
	int pct;
	size_t i;

	for (i = 0; i < sizeof kw / sizeof kw[0]; i++)
		if (vn == strlen(kw[i].k) && starts(v, vn, kw[i].k)) {
			l->v = (short)(kw[i].px * 10);
			l->u = CSS_U_PX;
			return 1;
		}
	if (vn == 7 && starts(v, vn, "smaller")) {
		l->v = 8;		/* 0.8em */
		l->u = CSS_U_EM;
		return 1;
	}
	if (vn == 6 && starts(v, vn, "larger")) {
		l->v = 12;
		l->u = CSS_U_EM;
		return 1;
	}
	if ((q = number(v, e, &t, &pct)) == NULL || t <= 0)
		return 0;
	while (q > v && ((q[-1] | 32) >= 'a' && (q[-1] | 32) <= 'z'))
		q--;
	t /= 100;			/* tenths */
	if (pct) {
		l->u = CSS_U_EM;
		t /= 100;		/* (120% is 1.2em) */
		if (t == 0)
			return 0;
	} else if (starts(q, (size_t)(e - q), "px"))
		l->u = CSS_U_PX;
	else if (starts(q, (size_t)(e - q), "pt")) {
		l->u = CSS_U_PX;
		t = t * 4 / 3;
	} else if (starts(q, (size_t)(e - q), "rem")) {
		l->u = CSS_U_PX;
		t *= 16;
	} else if (starts(q, (size_t)(e - q), "em"))
		l->u = CSS_U_EM;
	else if (starts(q, (size_t)(e - q), "ex")) {
		l->u = CSS_U_EM;
		t /= 2;
	} else
		return 0;
	l->v = (short)clamp(t, 1, 32000);
	return 1;
}

/* the font shorthand's size: the word with one ("bold 1.2em/1.5 x") */
static int font_short_size(const char *v, size_t vn, struct css_len *l)
{
	size_t i = 0;

	while (i < vn) {
		size_t j, k;

		while (i < vn && is_space((unsigned char)v[i]))
			i++;
		for (j = i; j < vn && !is_space((unsigned char)v[j]) && v[j] != ','; j++)
			;
		for (k = i; k < j && v[k] != '/'; k++)
			;
		if (k > i && font_size(v + i, k - i, l))
			return 1;
		if (j < vn && v[j] == ',')
			return 0;	/* (the families: no size after) */
		i = j + 1;
	}
	return 0;
}

/* a font size's index in the sheet's table (0: no room) */
static int fsz_index(struct css_sheet *s, const struct css_len *l)
{
	int i;

	for (i = 0; i < s->nfsz; i++)
		if (s->fsz[i].v == l->v && s->fsz[i].u == l->u)
			return i + 1;
	if (s->nfsz == FSZ_MAX)
		return 0;
	s->fsz[s->nfsz++] = *l;
	return s->nfsz;
}

/*
 * One declaration about the look of text, name p (pn bytes), value v (vn
 * bytes, !important cut, imp says if it was there), into *t: 1 if it was
 * one. s: the sheet, for a colour's palette index.
 */
static int text_decl(struct css_sheet *s, const char *p, size_t pn,
	const char *v, size_t vn, int imp, struct tdecl *t)
{
	int k = 0, bit = 0;

	if (pn == 5 && starts(p, pn, "color")) {
		struct cval c;

		parse_color(v, vn, &c, 0);
		if (c.kind == CV_NONE)
			return 0;
		color_decl(s, &c, t);
		if (t->fg == 0)
			return 0;
		bit = T_FG;
	} else if (pn == 11 && starts(p, pn, "font-weight")) {
		if ((k = weight(v, vn)) == 0)
			return 0;
		t->fw = (unsigned char)k;
		bit = T_FW;
	} else if (pn == 10 && starts(p, pn, "font-style")) {
		if (starts(v, vn, "italic") || starts(v, vn, "oblique"))
			t->fs = CSS_FS_ITALIC;
		else if (starts(v, vn, "normal") || starts(v, vn, "initial"))
			t->fs = CSS_FS_NORMAL;
		else
			return 0;
		bit = T_FS;
	} else if (pn == 9 && starts(p, pn, "font-size")) {
		struct css_len l;

		if (s == NULL || !font_size(v, vn, &l) || (k = fsz_index(s, &l)) == 0)
			return 0;
		t->fz = (unsigned char)k;
		bit = T_FZ;
	} else if (pn == 4 && starts(p, pn, "font")) {
		/* the shorthand: what it doesn't say is normal */
		size_t i;
		int b = 0;
		struct css_len l;

		if (starts(v, vn, "inherit") || starts(v, vn, "unset")
			|| starts(v, vn, "var(") || starts(v, vn, "revert"))
			return 0;
		for (i = 0; i < vn; i++)
			if (v[i] >= '6' && v[i] <= '9' && i + 2 < vn && v[i + 1] == '0'
				&& v[i + 2] == '0' && (i == 0 || is_space((unsigned char)v[i - 1]))
				&& (i + 3 == vn || is_space((unsigned char)v[i + 3])))
				b = 1;
		t->fw = (unsigned char)(b || has_word(v, vn, "bold") || has_word(v, vn, "bolder") ?
			CSS_FW_BOLD : CSS_FW_NORMAL);
		t->fs = (unsigned char)(has_word(v, vn, "italic") || has_word(v, vn, "oblique") ?
			CSS_FS_ITALIC : CSS_FS_NORMAL);
		if (imp)
			t->imp |= T_FW | T_FS;
		if (s && font_short_size(v, vn, &l) && (k = fsz_index(s, &l)) != 0) {
			t->fz = (unsigned char)k;
			if (imp)
				t->imp |= T_FZ;
		}
		return 1;
	} else if ((pn == 15 && starts(p, pn, "text-decoration"))
		|| (pn == 20 && starts(p, pn, "text-decoration-line"))) {
		if (starts(v, vn, "inherit") || starts(v, vn, "unset") || starts(v, vn, "var("))
			return 0;
		t->td = (unsigned char)(has_word(v, vn, "underline") ? CSS_TD_UNDER : CSS_TD_NONE);
		bit = T_TD;
	} else if (pn == 10 && starts(p, pn, "text-align")) {
		if (starts(v, vn, "center") || starts(v, vn, "-webkit-center")
			|| starts(v, vn, "-moz-center"))
			t->ta = CSS_TA_CENTER;
		else if (starts(v, vn, "right") || starts(v, vn, "end"))
			t->ta = CSS_TA_RIGHT;
		else if (starts(v, vn, "left") || starts(v, vn, "start")
			|| starts(v, vn, "justify") || starts(v, vn, "initial"))
			t->ta = CSS_TA_LEFT;
		else
			return 0;
		bit = T_TA;
	} else
		return 0;
	if (imp)
		t->imp |= (unsigned char)bit;
	return 1;
}

/* is selector p..e one that custom properties are set on for the whole
 * page: :root, html, body, * */
static int global_sel(const char *p, const char *e)
{
	size_t n;

	while (p < e && is_space((unsigned char)*p))
		p++;
	while (e > p && is_space((unsigned char)e[-1]))
		e--;
	n = (size_t)(e - p);
	return (n == 5 && starts(p, n, ":root")) || (n == 4 && starts(p, n, "html"))
		|| (n == 4 && starts(p, n, "body")) || (n == 1 && *p == '*')
		|| (n == 5 && starts(p, n, ":host"));
}

/* a list-style value: CSS_HIDE for no marker, else CSS_SHOW */
static int list_style(const char *v, size_t vn, int type_only)
{
	size_t i = 0;

	if (type_only)
		return starts(v, vn, "none") ? CSS_HIDE : CSS_SHOW;
	/* the shorthand: none among its words (url(...) none, none inside) */
	while (i < vn) {
		size_t j;

		while (i < vn && is_space((unsigned char)v[i]))
			i++;
		for (j = i; j < vn && !is_space((unsigned char)v[j]) && v[j] != '!'; j++)
			;
		if (j - i == 4 && starts(v + i, j - i, "none"))
			return CSS_HIDE;
		i = j + 1;
	}
	return CSS_SHOW;
}

/* --- boxes ------------------------------------------------------------ */

/* what a background says of its image (bg_value), the shorthand's
 * initial values where it says nothing */
struct bgsay {
	const char *src;		/* url()'s, n bytes; NULL: none */
	size_t n;
	unsigned char rep, fixed;	/* CSS_BR_*, attachment fixed */
	struct css_len pos[2];
};

static int bg_value(const char *v, size_t vn, int shorthand, unsigned long *rgb,
	struct bgsay *o);

/* does a rule's text say anything about boxes? (cheap, before parsing) */
static int box_mentions(const char *d, int n)
{
	int i;

	if (memchr(d, '\\', (size_t)n))
		return 1;		/* (a name may be escaped: m\61rgin) */
	for (i = 0; i + 4 < n; i++)
		switch (d[i]) {
		case 'm': case 'p': case 'b': case 'w': case 'h': case 'f':
		case 'c': case 't': case 'l': case 'r': case 'z': case 'd': case 'o':
			if (starts(d + i, (size_t)(n - i), "margin")
				|| starts(d + i, (size_t)(n - i), "padding")
				|| starts(d + i, (size_t)(n - i), "border")
				|| starts(d + i, (size_t)(n - i), "background")
				|| starts(d + i, (size_t)(n - i), "width")
				|| starts(d + i, (size_t)(n - i), "height")
				|| starts(d + i, (size_t)(n - i), "float")
				|| starts(d + i, (size_t)(n - i), "font")
				|| starts(d + i, (size_t)(n - i), "clear")
				|| starts(d + i, (size_t)(n - i), "position")
				|| starts(d + i, (size_t)(n - i), "top")
				|| starts(d + i, (size_t)(n - i), "left")
				|| starts(d + i, (size_t)(n - i), "right")
				|| starts(d + i, (size_t)(n - i), "bottom")
				|| starts(d + i, (size_t)(n - i), "z-index")
				|| starts(d + i, (size_t)(n - i), "display")
				|| starts(d + i, (size_t)(n - i), "overflow"))
				return 1;
			break;
		}
	return 0;
}

/* a length (or auto) at v (vn bytes) into *l: 1, or 0 if not one */
static int parse_len(const char *v, size_t vn, struct css_len *l)
{
	const char *e = v + vn, *q, *u;
	long t;
	int pct;
	size_t un;

	while (v < e && is_space((unsigned char)*v))
		v++;
	while (e > v && is_space((unsigned char)e[-1]))
		e--;
	if (is_kw(v, (size_t)(e - v), "auto")) {
		l->v = 0;
		l->u = CSS_U_AUTO;
		return 1;
	}
	/* (one number and its unit, nothing more: "200", "2em x" aren't) */
	if (v == e || *v == ',' || (q = number(v, e, &t, &pct)) != e)
		return 0;
	for (u = q; u > v && ((u[-1] | 32) >= 'a' && (u[-1] | 32) <= 'z'); u--)
		;
	un = (size_t)(q - u);
	t /= 100;			/* thousandths to tenths */
	if (pct) {
		if (un)
			return 0;
		l->u = CSS_U_PCT;
	} else if (is_kw(u, un, "px"))
		l->u = CSS_U_PX;
	else if (is_kw(u, un, "em") || is_kw(u, un, "rem"))
		l->u = CSS_U_EM;
	else if (is_kw(u, un, "ex") || is_kw(u, un, "ch")) {
		l->u = CSS_U_EM;
		t /= 2;
	} else if (is_kw(u, un, "pt")) {
		l->u = CSS_U_PX;
		t = t * 4 / 3;
	} else if (is_kw(u, un, "pc")) {
		l->u = CSS_U_PX;
		t *= 16;
	} else if (is_kw(u, un, "in")) {
		l->u = CSS_U_PX;
		t = clamp(t, -1000, 1000) * 96;
	} else if (is_kw(u, un, "cm")) {
		l->u = CSS_U_PX;
		t = clamp(t, -10000, 10000) * 378 / 10;
	} else if (is_kw(u, un, "mm")) {
		l->u = CSS_U_PX;
		t = clamp(t, -100000, 100000) * 378 / 100;
	} else if (t == 0 && un == 0)
		l->u = CSS_U_PX;	/* (a bare 0) */
	else
		return 0;
	l->v = (short)clamp(t, -32000, 32000);
	return 1;
}

/* the words of v, as (start, length) pairs: how many (at most max) */
static int words(const char *v, size_t vn, const char **w, size_t *wn, int max)
{
	size_t i = 0;
	int n = 0;

	while (i < vn && n < max) {
		size_t j;
		int depth = 0;

		while (i < vn && is_space((unsigned char)v[i]))
			i++;
		if (i == vn)
			break;
		for (j = i; j < vn && (depth || !is_space((unsigned char)v[j])); j++)
			if (v[j] == '(')
				depth++;
			else if (v[j] == ')' && depth)
				depth--;
		w[n] = v + i;
		wn[n++] = j - i;
		i = j;
	}
	return n;
}

/* 1 to 4 lengths, as margin and padding have them, into l[4]: 1 or 0 */
static int four_lens(const char *v, size_t vn, struct css_len *l)
{
	const char *w[5];
	size_t wn[5];
	int n = words(v, vn, w, wn, 5), i;
	struct css_len t[4];

	if (n == 0 || n > 4)
		return 0;
	for (i = 0; i < n; i++)
		if (!parse_len(w[i], wn[i], &t[i]))
			return 0;
	l[0] = t[0];
	l[1] = n > 1 ? t[1] : t[0];
	l[2] = n > 2 ? t[2] : t[0];
	l[3] = n > 3 ? t[3] : l[1];
	return 1;
}

static int border_width(const char *v, size_t vn, struct css_len *l)
{
	l->u = CSS_U_PX;
	if (is_kw(v, vn, "thin"))
		l->v = 10;
	else if (is_kw(v, vn, "medium"))
		l->v = 30;
	else if (is_kw(v, vn, "thick"))
		l->v = 50;
	else
		return parse_len(v, vn, l) && l->u != CSS_U_AUTO && l->u != CSS_U_PCT
			&& l->v >= 0;
	return 1;
}

static int border_style(const char *v, size_t vn, unsigned char *st)
{
	static const char *const drawn[] = { "solid", "dotted", "dashed", "double",
		"groove", "ridge", "inset", "outset", NULL };
	int i;

	if (is_kw(v, vn, "none") || is_kw(v, vn, "hidden")) {
		*st = 0;
		return 1;
	}
	for (i = 0; drawn[i]; i++)
		if (vn == strlen(drawn[i]) && starts(v, vn, drawn[i])) {
			*st = 1;
			return 1;
		}
	return 0;
}

/* border, border-top...: width, style, colour in any order, each once
 * (else none of it is taken) */
static unsigned long border_short(const char *v, size_t vn, int side,
	struct css_box *b, unsigned long *set2)
{
	const char *w[4];
	size_t wn[4];
	int n = words(v, vn, w, wn, 4), i, k, got = 0, wild = 0;
	unsigned long set = 0, rgb = 0;
	unsigned char st = 0;
	struct css_len wl;

	if (n == 0 || n > 3)
		return 0;
	for (i = 0; i < n; i++) {
		int ck;

		/* (var(--w, 1px): taken as its fallback) */
		(void)var_fallback(w[i], wn[i], &w[i], &wn[i]);
		if (is_wild(w[i], wn[i])) {
			wild = 1;		/* (any part: none reset) */
			continue;
		}
		if (!(got & 1) && border_width(w[i], wn[i], &wl))
			got |= 1;
		else if (!(got & 2) && border_style(w[i], wn[i], &st))
			got |= 2;
		else if (!(got & 4) && (ck = border_rgb(w[i], wn[i], &rgb)) >= 0)
			got |= ck ? 4 | 8 : 4;
		else
			return 0;
	}
	/* (what the shorthand leaves out is reset: no style, medium,
	 * currentcolor) */
	if (!wild) {
		if (!(got & 2))
			st = 0;
		if (!(got & 1)) {
			wl.v = 30;
			wl.u = CSS_U_PX;
		}
		if (!(got & 4))
			rgb = CSS_BG_CURRENT;
		got |= 1 | 2 | 8;
	}
	for (k = 0; k < 4; k++) {
		if (side >= 0 && side != k)
			continue;
		if (got & 1) {
			b->bw[k] = wl;
			set |= CSS_B_BW(k);
		}
		if (got & 2) {
			b->bstyle[k] = st;
			*set2 |= CSS_B2_BS(k);
		}
		if (got & 8) {
			b->bcolor[k] = rgb;
			*set2 |= CSS_B2_BC(k);
		}
	}
	return set;
}

/* a line-height value into *l: 1, or 0 */
static int line_height(const char *v, size_t vn, struct css_len *l)
{
	const char *e = v + vn, *q;
	long t;
	int pct;

	if (vn == 6 && starts(v, vn, "normal")) {
		l->v = 0;
		l->u = CSS_U_AUTO;
		return 1;
	}
	if ((q = number(v, e, &t, &pct)) == NULL || t < 0)
		return 0;
	while (q > v && ((q[-1] | 32) >= 'a' && (q[-1] | 32) <= 'z'))
		q--;
	t /= 100;			/* tenths */
	if (pct) {
		l->u = CSS_U_PCT;	/* (of the font's size: as "em") */
		t /= 100;
	} else if (q == e)
		l->u = CSS_U_EM;	/* (a number: times the font's size) */
	else if (starts(q, (size_t)(e - q), "em"))
		l->u = CSS_U_PCT;
	else if (starts(q, (size_t)(e - q), "px"))
		l->u = CSS_U_PX;
	else if (starts(q, (size_t)(e - q), "pt")) {
		l->u = CSS_U_PX;
		t = t * 4 / 3;
	} else
		return 0;
	l->v = (short)clamp(t, 0, 32000);
	return 1;
}

/* the 1 to 4 words of a per-side value ("none solid"), each as one()
 * takes it, into the sides of *b (style: 1, else colour): the CSS_B2_*
 * bits set, or 0 */
static unsigned long side_words(const char *v, size_t vn, int style,
	struct css_box *b)
{
	const char *w[5];
	size_t wn[5];
	int n = words(v, vn, w, wn, 5), i;
	unsigned char st[4];
	unsigned long rgb[4];
	static const int from[4][4] = {
		{ 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };

	if (n == 0 || n > 4)
		return 0;
	for (i = 0; i < n; i++)
		if (style ? !border_style(w[i], wn[i], &st[i])
			: border_rgb(w[i], wn[i], &rgb[i]) <= 0)
			return 0;
	for (i = 0; i < 4; i++)
		if (style)
			b->bstyle[i] = st[from[n - 1][i]];
		else
			b->bcolor[i] = rgb[from[n - 1][i]];
	return style ? CSS_B2_BS(0) | CSS_B2_BS(1) | CSS_B2_BS(2) | CSS_B2_BS(3)
		: CSS_B2_BC(0) | CSS_B2_BC(1) | CSS_B2_BC(2) | CSS_B2_BC(3);
}

static void bg_initial(struct bgsay *o)
{
	memset(o, 0, sizeof *o);
	o->rep = CSS_BR_REPEAT;
	o->pos[0].u = o->pos[1].u = CSS_U_PCT;
}

/* is a picture at this URL (n bytes) one Manx can show? (data: of
 * PNG, GIF or JPEG; not SVG) */
static int pic_shown(const char *u, size_t n)
{
	size_t k;

	if (n >= 5 && starts(u, 5, "data:"))
		return (n > 14 && (starts(u, 14, "data:image/png") || starts(u, 14, "data:image/gif")))
			|| (n > 15 && starts(u, 15, "data:image/jpeg"))
			|| (n > 14 && starts(u, 14, "data:image/jpg"));
	for (k = 0; k < n && u[k] != '?' && u[k] != '#'; k++)
		;
	return !(k >= 4 && starts(u + k - 4, 4, ".svg"));
}

/* a url(...)'s URL (quoted or not): 1 and *src, *n, or 0 */
static int url_of(const char *w, size_t wn, const char **src, size_t *n)
{
	const char *e = w + wn, *a;

	if (wn < 5 || !starts(w, 4, "url(") || e[-1] != ')')
		return 0;
	w += 4;
	e--;
	while (w < e && is_space((unsigned char)*w))
		w++;
	while (e > w && is_space((unsigned char)e[-1]))
		e--;
	if (w < e && (*w == '"' || *w == '\'')) {
		for (a = w + 1; a < e && *a != *w; a++)
			;
		w++;
		e = a;
	}
	if (w == e || e - w > 60000)
		return 0;
	if (!pic_shown(w, (size_t)(e - w)))
		return 0;		/* (as none) */
	*src = w;
	*n = (size_t)(e - w);
	return 1;
}

/* one background-repeat value (n words): CSS_BR_* */
static int bg_repeat(const char *const *w, const size_t *wn, int n)
{
	int k[2], i;

	for (i = 0; i < n; i++) {
		if (is_kw(w[i], wn[i], "repeat-x"))
			return CSS_BR_X;
		if (is_kw(w[i], wn[i], "repeat-y"))
			return CSS_BR_Y;
		k[i] = is_kw(w[i], wn[i], "no-repeat") ? 0 : 1;	/* (space, round: repeat) */
	}
	if (n == 1)
		k[1] = k[0];
	return k[0] && k[1] ? CSS_BR_REPEAT : k[0] ? CSS_BR_X : k[1] ? CSS_BR_Y
		: CSS_BR_NONE;
}

/* a keyword of background-position as a %: 0 (left, top), 500 (center),
 * 1000 (right, bottom) in tenths, which axis it is of (1: x, 2: y, 3:
 * either) in *ax; -1 when it isn't one */
static int pos_kw(const char *w, size_t wn, int *ax)
{
	*ax = 1;
	if (is_kw(w, wn, "left"))
		return 0;
	if (is_kw(w, wn, "right"))
		return 1000;
	*ax = 2;
	if (is_kw(w, wn, "top"))
		return 0;
	if (is_kw(w, wn, "bottom"))
		return 1000;
	*ax = 3;
	return is_kw(w, wn, "center") ? 500 : -1;
}

/* a background-position (n words) into pos[2], x then y: keywords and
 * lengths (of four values, with offsets from an edge, the edges only) */
static void bg_pos(const char *const *w, const size_t *wn, int n, struct css_len *pos)
{
	struct css_len l[2];
	int ax[2], i, k = 0;

	pos[0].v = pos[1].v = 0;
	pos[0].u = pos[1].u = CSS_U_PCT;
	for (i = 0; i < n && k < 2; i++) {
		int v = pos_kw(w[i], wn[i], &ax[k]);

		if (v >= 0) {
			l[k].v = (short)v;
			l[k++].u = CSS_U_PCT;
		} else if (n <= 2 && parse_len(w[i], wn[i], &l[k]) && l[k].u != CSS_U_AUTO)
			ax[k++] = 3;
		else if (n <= 2)
			return;			/* (calc(), var(): not known) */
	}
	if (k == 0)
		return;
	if (k == 1) {
		l[1].v = 500;
		l[1].u = CSS_U_PCT;
		ax[1] = 3;
	}
	if (ax[0] == 2 || ax[1] == 1) {
		pos[0] = l[1];
		pos[1] = l[0];
	} else {
		pos[0] = l[0];
		pos[1] = l[1];
	}
}

/* background-image, -repeat, -attachment, -position (p: after the
 * "background-"), of the first layer, into *b: 0 (the bits in *set2) */
static unsigned long bg_long(const char *p, size_t pn, const char *v, size_t vn,
	struct css_box *b, unsigned long *set2)
{
	const char *w[4], *q;
	size_t wn[4], n;
	int depth = 0;

	if (pn == 5 && starts(p, pn, "image")) {
		/* (of several, the first picture: a gradient over it is the
		 * one that isn't drawn) */
		const char *e = v + vn, *a;
		int url = 0;

		b->bgsrc = NULL;
		b->bgsrc_n = 0;
		b->bgbase = NULL;
		for (a = v; a + 4 < e; a++)
			if (starts(a, (size_t)(e - a), "url(")) {
				for (q = a; q < e && *q != ')'; q++)
					;
				/* (one not shown here, SVG: as none) */
				url = q < e;
				if (url && url_of(a, (size_t)(q + 1 - a), &w[0], &n)) {
					b->bgsrc = w[0];
					b->bgsrc_n = (unsigned short)n;
				}
				break;
			}
		if (!url && !is_kw(v, vn, "none") && !has(v, vn, "gradient("))
			return 0;
		*set2 |= CSS_B2_BGI;
		return 0;
	}
	/* (the first layer) */
	for (q = v; q < v + vn && (depth || *q != ','); q++)
		if (*q == '(')
			depth++;
		else if (*q == ')' && depth)
			depth--;
	vn = (size_t)(q - v);
	if (pn == 6 && starts(p, pn, "repeat")) {
		int k = words(v, vn, w, wn, 3);

		if (k < 1 || k > 2)
			return 0;
		b->bgrep = (unsigned char)bg_repeat(w, wn, k);
		*set2 |= CSS_B2_BGR;
	} else if (pn == 10 && starts(p, pn, "attachment")) {
		b->bgfixed = is_kw(v, vn, "fixed");
		*set2 |= CSS_B2_BGA;
	} else if (pn == 8 && starts(p, pn, "position")) {
		int k = words(v, vn, w, wn, 4);

		if (k < 1)
			return 0;
		bg_pos(w, wn, k, b->bgpos);
		*set2 |= CSS_B2_BGP;
	}
	return 0;
}

/* one declaration about the box into *b: the CSS_B_* bits it set (0: none),
 * and the CSS_B2_* bits into *set2 */
static unsigned long box_decl(const char *p, size_t pn, const char *v,
	size_t vn, struct css_box *b, unsigned long *set2)
{
	static const char *const sides[] = { "top", "right", "bottom", "left" };
	struct css_len l[4];
	int i;

	if ((starts(v, vn, "inherit") && !(pn == 5 && starts(p, pn, "float")))
		|| starts(v, vn, "var(") || starts(v, vn, "unset") || starts(v, vn, "revert"))
		return 0;			/* (float: inherit, the layout's) */
	if ((pn == 6 && starts(p, pn, "margin")) || (pn == 7 && starts(p, pn, "padding"))) {
		int m = p[0] == 'm' || p[0] == 'M';

		if (!four_lens(v, vn, l))
			return 0;
		for (i = 0; i < 4; i++)
			if (!m && l[i].v < 0)
				return 0;	/* (padding can't be negative) */
		for (i = 0; i < 4; i++)
			if (m)
				b->m[i] = l[i];
			else
				b->p[i] = l[i];
		return m ? CSS_B_M(0) | CSS_B_M(1) | CSS_B_M(2) | CSS_B_M(3)
			: CSS_B_P(0) | CSS_B_P(1) | CSS_B_P(2) | CSS_B_P(3);
	}
	for (i = 0; i < 4; i++) {
		size_t k = strlen(sides[i]);

		if (pn == 7 + k && starts(p, 7, "margin-") && starts(p + 7, k, sides[i])) {
			if (!parse_len(v, vn, &l[0]))
				return 0;
			b->m[i] = l[0];
			return CSS_B_M(i);
		}
		if (pn == 8 + k && starts(p, 8, "padding-") && starts(p + 8, k, sides[i])) {
			if (!parse_len(v, vn, &l[0]) || l[0].v < 0)
				return 0;
			b->p[i] = l[0];
			return CSS_B_P(i);
		}
		if (pn == 7 + k && starts(p, 7, "border-") && starts(p + 7, k, sides[i]))
			return border_short(v, vn, i, b, set2);
		if (pn == 13 + k && starts(p, 7, "border-") && starts(p + 7, k, sides[i])
			&& starts(p + 7 + k, 6, "-width")) {
			if (!border_width(v, vn, &l[0]))
				return 0;
			b->bw[i] = l[0];
			return CSS_B_BW(i);
		}
		if (pn == 13 + k && starts(p, 7, "border-") && starts(p + 7, k, sides[i])
			&& starts(p + 7 + k, 6, "-style")) {
			const char *w[1];
			size_t wn[1];

			if (words(v, vn, w, wn, 1) == 1
				&& border_style(w[0], wn[0], &b->bstyle[i]))
				*set2 |= CSS_B2_BS(i);
			return 0;
		}
		if (pn == 13 + k && starts(p, 7, "border-") && starts(p + 7, k, sides[i])
			&& starts(p + 7 + k, 6, "-color")) {
			const char *w[1];
			size_t wn[1];

			if (words(v, vn, w, wn, 1) == 1
				&& border_rgb(w[0], wn[0], &b->bcolor[i]) > 0)
				*set2 |= CSS_B2_BC(i);
			return 0;
		}
	}
	if (pn == 6 && starts(p, pn, "border"))
		return border_short(v, vn, -1, b, set2);
	if (pn == 12 && starts(p, pn, "border-width")) {
		const char *w[5];
		size_t wn[5];
		int n = words(v, vn, w, wn, 5);
		struct css_len t[4];

		if (n == 0 || n > 4)
			return 0;
		for (i = 0; i < n; i++)
			if (!border_width(w[i], wn[i], &t[i]))
				return 0;
		b->bw[0] = t[0];
		b->bw[1] = n > 1 ? t[1] : t[0];
		b->bw[2] = n > 2 ? t[2] : t[0];
		b->bw[3] = n > 3 ? t[3] : b->bw[1];
		return CSS_B_BW(0) | CSS_B_BW(1) | CSS_B_BW(2) | CSS_B_BW(3);
	}
	if (pn == 12 && (starts(p, pn, "border-style") || starts(p, pn, "border-color"))) {
		*set2 |= side_words(v, vn, p[7] == 's' || p[7] == 'S', b);
		return 0;
	}
	if ((pn == 16 && starts(p, pn, "background-color"))
		|| (pn == 10 && starts(p, pn, "background"))) {
		unsigned long rgb;
		struct bgsay o;

		if (bg_value(v, vn, pn == 10, &rgb, &o) <= 0)
			return 0;
		b->bg = rgb;
		if (pn == 10) {
			/* (the shorthand: all of it, what isn't said as it starts) */
			b->bgsrc = o.src;
			b->bgsrc_n = (unsigned short)o.n;
			b->bgbase = NULL;
			b->bgrep = o.rep;
			b->bgfixed = o.fixed;
			memcpy(b->bgpos, o.pos, sizeof b->bgpos);
			*set2 |= CSS_B2_BGI | CSS_B2_BGR | CSS_B2_BGA | CSS_B2_BGP;
		}
		return CSS_B_BG;
	}
	if (pn > 11 && starts(p, 11, "background-"))
		return bg_long(p + 11, pn - 11, v, vn, b, set2);
	if (pn == 11 && starts(p, pn, "line-height"))
		return line_height(v, vn, &b->lh) ? CSS_B_LH : 0;
	if (pn == 4 && starts(p, pn, "font")) {
		/* the shorthand's line height ("10px/1.2"), else normal */
		size_t k;

		for (k = 0; k < vn && v[k] != '/' && v[k] != ','; k++)
			;
		if (k < vn && v[k] == '/') {
			size_t j = ++k;

			while (j < vn && !is_space((unsigned char)v[j]) && v[j] != ',')
				j++;
			if (!line_height(v + k, j - k, &b->lh))
				return 0;
		} else {
			b->lh.v = 0;
			b->lh.u = CSS_U_AUTO;
		}
		return CSS_B_LH;
	}
	if (pn == 5 && starts(p, pn, "width")) {
		if (!parse_len(v, vn, &l[0]) || l[0].v < 0)
			return 0;
		b->w = l[0];
		return CSS_B_W;
	}
	if (pn == 6 && starts(p, pn, "height")) {
		if (!parse_len(v, vn, &l[0]) || l[0].v < 0)
			return 0;
		b->h = l[0];
		return CSS_B_H;
	}
	if ((pn == 9 || pn == 10) && (starts(p, pn, "min-") || starts(p, pn, "max-"))
		&& (starts(p + 4, pn - 4, "width") || starts(p + 4, pn - 4, "height"))) {
		int mx = p[1] == 'a', wd = p[4] == 'w';
		struct css_len *l = wd ? (mx ? &b->maxw : &b->minw)
			: (mx ? &b->maxh : &b->minh);
		struct css_len t;

		/* (none, and auto: no limit) */
		if (is_kw(v, vn, "none")) {
			t.v = 0;
			t.u = CSS_U_AUTO;
		} else if (!parse_len(v, vn, &t) || t.v < 0)
			return 0;
		*l = t;
		return wd ? (mx ? CSS_B_MAXW : CSS_B_MINW) : (mx ? CSS_B_MAXH : CSS_B_MINH);
	}
	if (pn == 5 && starts(p, pn, "float")) {
		if (is_kw(v, vn, "left"))
			b->fl = CSS_FL_LEFT;
		else if (is_kw(v, vn, "right"))
			b->fl = CSS_FL_RIGHT;
		else if (is_kw(v, vn, "none"))
			b->fl = CSS_FL_NONE;
		else if (is_kw(v, vn, "inherit"))
			b->fl = CSS_FL_INHERIT;	/* (the parent's: the layout's) */
		else
			return 0;
		return CSS_B_FLOAT;
	}
	if (pn == 5 && starts(p, pn, "clear")) {
		if (is_kw(v, vn, "left"))
			b->clr = CSS_CL_LEFT;
		else if (is_kw(v, vn, "right"))
			b->clr = CSS_CL_RIGHT;
		else if (is_kw(v, vn, "both"))
			b->clr = CSS_CL_BOTH;
		else if (is_kw(v, vn, "none"))
			b->clr = CSS_CL_NONE;
		else
			return 0;
		return CSS_B_CLEAR;
	}
	if (pn == 8 && starts(p, pn, "overflow")) {
		/* (hidden, clip: what's outside isn't shown; the others show
		 * it, or scroll to it - as here, where nothing scrolls) */
		b->clip = is_kw(v, vn, "hidden") || is_kw(v, vn, "clip");
		*set2 |= CSS_B2_OVF;
		return 0;
	}
	if (pn == 8 && starts(p, pn, "position")) {
		if (is_kw(v, vn, "static"))
			b->pos = CSS_PO_STATIC;
		else if (is_kw(v, vn, "relative"))
			b->pos = CSS_PO_RELATIVE;
		else if (is_kw(v, vn, "sticky") || is_kw(v, vn, "-webkit-sticky"))
			b->pos = CSS_PO_STICKY;
		else if (is_kw(v, vn, "absolute"))
			b->pos = CSS_PO_ABSOLUTE;
		else if (is_kw(v, vn, "fixed"))
			b->pos = CSS_PO_FIXED;
		else
			return 0;
		return CSS_B_POS;
	}
	for (i = 0; i < 4; i++)
		if (pn == strlen(sides[i]) && starts(p, pn, sides[i])) {
			if (!parse_len(v, vn, &l[0]))
				return 0;
			b->off[i] = l[0];
			return CSS_B_OFF(i);
		}
	if (pn == 7 && starts(p, pn, "z-index")) {
		long z = 0;
		size_t k = 0;
		int neg = 0;

		if (is_kw(v, vn, "auto")) {
			b->z = 0;
			return CSS_B_Z;
		}
		if (k < vn && (v[k] == '-' || v[k] == '+'))
			neg = v[k++] == '-';
		if (k == vn)
			return 0;
		for (; k < vn; k++) {
			if (v[k] < '0' || v[k] > '9')
				return 0;
			if (z < 30000)
				z = z * 10 + (v[k] - '0');
		}
		b->z = (short)(neg ? -z : z);
		return CSS_B_Z;
	}
	if (pn == 7 && starts(p, pn, "display")) {
		if (vn == 5 && starts(v, vn, "block"))
			b->disp = CSS_DI_BLOCK;
		else if (vn == 6 && starts(v, vn, "inline"))
			b->disp = CSS_DI_INLINE;
		else if (is_kw(v, vn, "list-item"))
			b->disp = CSS_DI_LIST_ITEM;
		else if (is_kw(v, vn, "inline-block"))
			b->disp = CSS_DI_INLINE_BLOCK;
		else if (is_kw(v, vn, "table"))
			b->disp = CSS_DI_TABLE;
		else if (is_kw(v, vn, "table-row"))
			b->disp = CSS_DI_TABLE_ROW;
		else if (is_kw(v, vn, "table-cell"))
			b->disp = CSS_DI_TABLE_CELL;
		else
			return 0;
		return CSS_B_DISP;
	}
	return 0;
}

/* where the declaration from p ends: its ';' (not one in a string, in
 * brackets or escaped), or e */
static const char *decl_end(const char *p, const char *e)
{
	int paren = 0, quote = 0;

	for (; p < e; p++) {
		if (*p == '\\' && p + 1 < e)
			p++;
		else if (quote) {
			if (*p == quote)
				quote = 0;
		} else if (*p == '"' || *p == '\'')
			quote = *p;
		else if (*p == '(' || *p == '[')
			paren++;
		else if ((*p == ')' || *p == ']') && paren > 0)
			paren--;
		else if (*p == ';' && paren == 0)
			break;
	}
	return p;
}

/*
 * One declaration, p to e: its name (escapes undone, into nbuf when it
 * has any) and value, the value's !important cut off and said in *imp.
 * 0 when browsers drop it: no name, a name that isn't an identifier
 * ("*zoom", "m\argin" is "m", a newline, "rgin"), no value, or a '!'
 * not of !important ("red ! error").
 */
static int split_decl(const char *p, const char *e, const char **name, size_t *pn,
	const char **val, size_t *vn, int *imp, char *nbuf, int nbn)
{
	const char *colon, *v, *x;
	int paren = 0, quote = 0;
	size_t i, n;

	while (p < e && is_space((unsigned char)*p))
		p++;
	for (colon = p; colon < e && *colon != ':'; colon++)
		;
	if (colon == e)
		return 0;
	n = (size_t)(colon - p);
	while (n && is_space((unsigned char)p[n - 1]))
		n--;
	if (n == 0 || (*p >= '0' && *p <= '9'))
		return 0;
	for (i = 0; i < n && is_name((unsigned char)p[i]); i++)
		;
	if (i < n) {
		const char *q = p;
		int k = read_name(&q, p + n, nbuf, nbn);

		if (k < 0 || q != p + n)
			return 0;
		p = nbuf;
		n = (size_t)k;
	}
	*name = p;
	*pn = n;
	v = colon + 1;
	while (v < e && is_space((unsigned char)*v))
		v++;
	for (x = v; x < e; x++) {
		if (*x == '\\' && x + 1 < e)
			x++;
		else if (quote) {
			if (*x == quote)
				quote = 0;
		} else if (*x == '"' || *x == '\'')
			quote = *x;
		else if (*x == '(')
			paren++;
		else if (*x == ')' && paren > 0)
			paren--;
		else if (*x == '!' && paren == 0)
			break;
	}
	*imp = 0;
	if (x < e) {
		const char *k = x + 1, *ke = e;

		while (k < ke && is_space((unsigned char)*k))
			k++;
		while (ke > k && is_space((unsigned char)ke[-1]))
			ke--;
		if (ke - k != 9 || !starts(k, 9, "important"))
			return 0;
		*imp = 1;
	}
	n = (size_t)(x - v);
	while (n && is_space((unsigned char)v[n - 1]))
		n--;
	*val = v;
	*vn = n;
	/* (a custom property may be empty) */
	return n > 0 || (*pn > 2 && (*name)[0] == '-' && (*name)[1] == '-');
}

/* n bytes at v into the sheet's pool, after the byte url + 1 when url >=
 * 0: where (its offset + 1), or 0 when there's no room */
static unsigned long cpool_add(struct css_sheet *s, int url, const char *v, size_t n)
{
	unsigned long need = s->cpool_n + n + 1 + (url >= 0), at;

	if (need > CPOOL_MAX)
		return 0;
	if (need > s->cpool_cap) {
		unsigned long c = s->cpool_cap ? s->cpool_cap * 2 : 1024;
		char *q;

		while (c < need)
			c *= 2;
		if ((q = xrealloc(s->cpool, c)) == NULL)
			return 0;
		s->cpool = q;
		s->cpool_cap = c;
	}
	at = s->cpool_n + 1;
	if (url >= 0)
		s->cpool[s->cpool_n++] = (char)(url + 1);
	memcpy(s->cpool + s->cpool_n, v, n);
	s->cpool[s->cpool_n + n] = '\0';
	s->cpool_n += n + 1;
	return at;
}

/* the rule just read: if it says something about display, visibility,
 * list-style or text, file each of its selectors; custom properties set
 * for the whole page are kept */
static void end_rule(struct css_sheet *s)
{
	const char *d = s->decl, *p, *e;
	int disp = 0, vis = 0, ls = 0, disp_imp = 0, vis_imp = 0, ls_imp = 0;
	int global = -1;		/* a :root/html/body rule: not known yet */
	unsigned short media = cur_media(s);
	struct tdecl t;
	struct bdecl bd;
	int boxes, box = -1, cont_imp = 0;
	const char *cv = NULL;
	size_t cvn = 0;
	unsigned long cont = 0;

	if (s->plong || s->plen == 0)
		return;
	boxes = css_boxes && box_mentions(d, s->dlen);
	if (!boxes && !mentions(d, s->dlen))
		return;
	memset(&t, 0, sizeof t);
	memset(&bd, 0, sizeof bd);
	/* the declarations */
	for (p = d; p < d + s->dlen; p = e + 1) {
		const char *v;
		size_t pn, vn;
		int imp;
		char nbuf[64];

		e = decl_end(p, d + s->dlen);
		if (!split_decl(p, e, &p, &pn, &v, &vn, &imp, nbuf, sizeof nbuf))
			continue;
		if (pn > 2 && p[0] == '-' && p[1] == '-') {
			/* a custom property: kept if it's a colour set for the
			 * whole page, on every screen */
			struct cval c;
			struct tdecl vt;
			const char *q, *qe;

			if (media != 0)
				continue;
			if (global < 0) {
				global = 0;
				for (q = s->pre; q < s->pre + s->plen && !global; q = qe + 1) {
					for (qe = q; qe < s->pre + s->plen && *qe != ','; qe++)
						;
					global = global_sel(q, qe);
				}
			}
			if (!global)
				continue;
			parse_color(v, vn, &c, 0);
			if (c.kind == CV_NONE)
				continue;
			memset(&vt, 0, sizeof vt);
			color_decl(s, &c, &vt);
			if (vt.fg)
				set_var(s, hash_bytes(p, pn), &vt);
			continue;
		}
		if (boxes) {
			/* (first: the font shorthand is the text's too) */
			unsigned long bits2 = 0;
			unsigned long bits = box_decl(p, pn, v, vn, &bd.b, &bits2);

			if ((bits2 & CSS_B2_BGI) && bd.b.bgsrc) {
				/* (the url, kept: the text goes) */
				bd.b.bgi = cpool_add(s, s->url_at, bd.b.bgsrc, bd.b.bgsrc_n);
				bd.b.bgsrc = NULL;
				if (bd.b.bgi == 0)
					bits2 &= ~CSS_B2_BGI;
			} else if (bits2 & CSS_B2_BGI)
				bd.b.bgi = 0;
			bd.b.set |= bits;
			bd.b.set2 |= bits2;
			if (imp) {
				bd.imp |= bits;
				bd.imp2 |= bits2;
			} else {
				bd.imp &= ~bits;
				bd.imp2 &= ~bits2;
			}
		}
		if (text_decl(s, p, pn, v, vn, imp, &t))
			continue;
		if (pn == 7 && starts(p, pn, "content")) {
			/* (kept as it is: attr() is the element's) */
			if (imp || !cont_imp) {
				cv = v;
				cvn = vn;
				cont_imp = imp;
			}
		} else if (pn == 7 && starts(p, pn, "display")) {
			if (is_kw(v, vn, "none"))
				disp = CSS_HIDE;
			else if (display_ok(v, vn) || starts(v, vn, "var("))
				disp = CSS_SHOW;	/* (var(): in doubt, shown) */
			else
				continue;
			disp_imp = imp;
		} else if (pn == 10 && starts(p, pn, "visibility")) {
			if (is_kw(v, vn, "hidden") || is_kw(v, vn, "collapse"))
				vis = CSS_HIDE;
			else if (is_kw(v, vn, "visible"))
				vis = CSS_SHOW;
			else
				continue;
			vis_imp = imp;
		} else if (pn == 10 && starts(p, pn, "list-style")) {
			ls = list_style(v, vn, 0);
			ls_imp = imp;
		} else if (pn == 15 && starts(p, pn, "list-style-type")) {
			ls = list_style(v, vn, 1);
			ls_imp = imp;
		}
		/*
		 * (Text "visually hidden", clipped to nothing, stays: it is
		 * there for screen readers because sighted readers get the same
		 * from an icon or a picture, which Manx doesn't draw: "Posted 9
		 * minutes ago", "Search", a logo's name.)
		 */
	}
	if ((bd.b.set || bd.b.set2) && s->nboxes < MAX_BOX_DECLS) {
		if (s->nboxes == s->boxes_cap) {
			int c = s->boxes_cap ? s->boxes_cap * 2 : 32;
			struct bdecl *q = xrealloc(s->boxes, (size_t)c * sizeof *q);

			if (q) {
				s->boxes = q;
				s->boxes_cap = c;
			}
		}
		if (s->nboxes < s->boxes_cap) {
			s->boxes[s->nboxes] = bd;
			box = s->nboxes++;
		}
	}
	if (cv)
		cont = cpool_add(s, -1, cv, cvn);
	if (!disp && !vis && !ls && !t.fg && !t.fw && !t.fs && !t.td && !t.ta
		&& !t.fz && box < 0 && !cont)
		return;
	if (!disp && !vis && !ls && box < 0 && !cont && s->ntext >= MAX_TEXT_RULES)
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
		r->ls = (unsigned char)ls;
		r->ls_imp = (unsigned char)ls_imp;
		r->t = t;
		r->box = box;
		r->cont = cont;
		r->cont_imp = (unsigned char)cont_imp;
		r->pseudo = sel[0].pseudo;
		b = (int)bucket_of(&sel[0]);
		r->next = s->bucket[b];
		s->bucket[b] = s->nrules++;
		if (!disp && !vis && !ls && box < 0 && !cont)
			s->ntext++;
		s->ncmps += n;
		s->gen++;
	}
}

/* how much of a prelude's start is space, and the marks of an HTML
 * comment (<!-- -->) a sheet in <style> may still be wrapped in */
static int cdo_skip(const char *p, int n)
{
	int i = 0;

	for (;;) {
		while (i < n && is_space((unsigned char)p[i]))
			i++;
		if (i + 4 <= n && memcmp(p + i, "<!--", 4) == 0)
			i += 4;
		else if (i + 3 <= n && memcmp(p + i, "-->", 3) == 0)
			i += 3;
		else
			return i;
	}
}

/* a { after a prelude: a rule's declarations, or a group, or a block
 * to skip */
static void open_block(struct css_sheet *s)
{
	const char *p;
	int n, k = cdo_skip(s->pre, s->plen);

	if (k) {
		memmove(s->pre, s->pre + k, (size_t)(s->plen - k));
		s->plen -= k;
	}
	p = s->pre;
	n = s->plen;
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
		if (c == ';') {
			/* @import ...; @charset...: done. Any other ';' is the
			 * selector's, which then fails: "a{};b{...}" drops b */
			int k = cdo_skip(s->pre, s->plen);

			if (k < s->plen && s->pre[k] == '@') {
				s->plen = 0;
				s->plong = 0;
				return;
			}
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

void css_base(struct css_sheet *s, const char *url)
{
	size_t n;
	int i;

	if (s == NULL || url == NULL)
		return;
	for (i = 0; i < s->nurls && strcmp(s->urls[i], url) != 0; i++)
		;
	if (i == s->nurls) {
		if (i == MAX_URLS || (s->urls[i] = xmalloc((n = strlen(url)) + 1)) == NULL) {
			s->url_at = 0;	/* (the page's: often the same server) */
			return;
		}
		memcpy(s->urls[i], url, n + 1);
		s->nurls++;
	}
	s->url_at = i + 1;
}

void css_begin(struct css_sheet *s, const char *media, nodeid pos)
{
	unsigned short m = 0;

	if (s == NULL)
		return;
	s->on = 1;
	s->url_at = 0;
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
	for (i = 0; i < c->nattr; i++) {
		const char *v = doc_attr(d, e->node, c->attr[i]);

		if (v == NULL)
			return 0;
		if (c->aop[i] == '=' && hash_bytes(v, strlen(v)) != c->aval[i])
			return 0;
		if (c->aop[i] == '~' && !word_in(v, c->aval[i]))
			return 0;
	}
	return 1;
}

/* the element before n among its parent's children, or 0 (the layout
 * asks in order, so mostly the one after the last asked about) */
static nodeid prev_elem(struct css_sheet *s, const struct doc *d, nodeid n)
{
	nodeid p = DOC_NODE(d, n)->parent, c, last;
	int again = 0;

	if (s->sib_par == p && s->sib_at) {
		c = s->sib_at;
		last = s->sib_prev;
	} else {
		c = DOC_NODE(d, p)->first;
		last = 0;
		again = 1;
	}
	for (;;) {
		while (c && c != n) {
			if (DOC_NODE(d, c)->type == NODE_ELEM)
				last = c;
			c = DOC_NODE(d, c)->next;
		}
		if (c == n)
			break;
		if (again)
			return 0;
		c = DOC_NODE(d, p)->first;
		last = 0;
		again = 1;
	}
	s->sib_par = p;
	s->sib_at = n;
	s->sib_prev = last;
	return last;
}

/* the compounds from k on, c[k - 1] having matched element el, whose
 * ancestors are s->path[0..top) */
static int match_from(struct css_sheet *s, const struct cmp *c, int k,
	int n_cmp, const struct doc *d, nodeid el, int top)
{
	int i;

	if (k == n_cmp)
		return 1;
	if (c[k - 1].comb == '+') {
		struct elinfo e;
		nodeid b = prev_elem(s, d, el);

		if (b == 0)
			return 0;
		el_info(d, b, doc_attr(d, b, ATTR_ID), doc_attr(d, b, ATTR_CLASS), &e);
		return cmp_match(&c[k], d, &e) && match_from(s, c, k + 1, n_cmp, d, b, top);
	}
	if (c[k - 1].comb == '~') {
		/* (from the first: walking back would cost a walk a step) */
		nodeid b;

		for (b = DOC_NODE(d, DOC_NODE(d, el)->parent)->first; b && b != el;
			b = DOC_NODE(d, b)->next) {
			struct elinfo e;

			if (DOC_NODE(d, b)->type != NODE_ELEM)
				continue;
			el_info(d, b, doc_attr(d, b, ATTR_ID), doc_attr(d, b, ATTR_CLASS), &e);
			if (cmp_match(&c[k], d, &e) && match_from(s, c, k + 1, n_cmp, d, b, top))
				return 1;
		}
		return 0;
	}
	for (i = top - 1; i >= 0; i--) {
		if (cmp_match(&c[k], d, &s->path[i])
			&& match_from(s, c, k + 1, n_cmp, d, s->path[i].node, i))
			return 1;
		if (c[k - 1].comb == '>')
			break;		/* (the parent only) */
	}
	return 0;
}

/* does rule r hold for element e, whose ancestors are s->path[0..top)? */
static int rule_match(struct css_sheet *s, const struct rule *r,
	const struct doc *d, const struct elinfo *e, int top)
{
	const struct cmp *c = &s->cmps[r->cmp];

	return cmp_match(&c[0], d, e) && match_from(s, c, 1, r->ncmp, d, e->node, top);
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

/* the rule that wins each property for an element */
struct winners {
	const struct rule *disp, *vis, *ls, *fg, *fw, *fs, *td, *ta, *fz;
	const struct rule *box[BOX_NBITS];	/* each box property's */
	int boxed;			/* some rule said something of the box */
	const struct rule *cont;	/* content's */
	int pseudo;			/* rules about its ::before (1), ::after
					 * (2) hold */
};

/* does r's say on the text property with bit beat w's? */
#define TBEATS(r, w, bit) beats(((r)->t.imp & (bit)) != 0, r, \
	(w) ? ((w)->t.imp & (bit)) != 0 : 0, w)

/* the rules filed under key bucket b that hold, into the winners */
static void try_bucket(struct css_sheet *s, unsigned b, const struct doc *d,
	const struct elinfo *e, int top, int vw, struct winners *w)
{
	int i;

	for (i = s->bucket[b]; i >= 0; i = s->rules[i].next) {
		const struct rule *r = &s->rules[i];

		if (r->pseudo != s->want) {
			/* (another's: only whether one giving content holds -
			 * without, there's none) */
			if (r->pseudo && r->cont && !s->want && !(w->pseudo & r->pseudo)
				&& media_ok(s, r->media, vw) && rule_match(s, r, d, e, top))
				w->pseudo |= r->pseudo;
			continue;
		}
		if (!media_ok(s, r->media, vw) || !rule_match(s, r, d, e, top))
			continue;
		if (r->cont && beats(r->cont_imp, r, w->cont ? w->cont->cont_imp : 0, w->cont))
			w->cont = r;
		if (r->disp && beats(r->disp_imp, r, w->disp ? w->disp->disp_imp : 0, w->disp))
			w->disp = r;
		if (r->vis && beats(r->vis_imp, r, w->vis ? w->vis->vis_imp : 0, w->vis))
			w->vis = r;
		if (r->ls && beats(r->ls_imp, r, w->ls ? w->ls->ls_imp : 0, w->ls))
			w->ls = r;
		if (r->t.fg && TBEATS(r, w->fg, T_FG))
			w->fg = r;
		if (r->t.fw && TBEATS(r, w->fw, T_FW))
			w->fw = r;
		if (r->t.fs && TBEATS(r, w->fs, T_FS))
			w->fs = r;
		if (r->t.td && TBEATS(r, w->td, T_TD))
			w->td = r;
		if (r->t.ta && TBEATS(r, w->ta, T_TA))
			w->ta = r;
		if (r->t.fz && TBEATS(r, w->fz, T_FZ))
			w->fz = r;
		if (r->box >= 0) {
			const struct bdecl *bd = &s->boxes[r->box];
			int k;

			w->boxed = 1;
			for (k = 0; k < BOX_NBITS; k++) {
				const struct rule *o = w->box[k];
				const struct bdecl *ob = o ? &s->boxes[o->box] : NULL;

				if (box_bit(bd->b.set, bd->b.set2, k)
					&& beats(box_bit(bd->imp, bd->imp2, k), r,
					ob ? box_bit(ob->imp, ob->imp2, k) : 0, o))
					w->box[k] = r;
			}
		}
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

/* a node's memo: what the sheet said of it */
#define M_DISP(m)	((int)((m) & 15) - 1)	/* css_display's answer */
#define M_FW(m)		((int)((m) >> 4 & 3))
#define M_FS(m)		((int)((m) >> 6 & 3))
#define M_TD(m)		((int)((m) >> 8 & 3))
#define M_TA(m)		((int)((m) >> 10 & 3))
#define M_FG(m)		((int)((m) >> 16 & 255))	/* palette index, FG_DEFAULT */
#define M_BOXED		0x1000UL	/* rules about its box hold */
#define M_FZ(m)		((int)((m) >> 24 & 255))	/* font size: fsz[k - 1] */
#define M_BEFORE	0x2000UL	/* rules about its ::before hold */
#define M_AFTER		0x4000UL	/* ... its ::after */

/* a memo as the caller wants it */
static int memo_out(const struct css_sheet *s, unsigned long m, struct css_text *t)
{
	int fg = M_FG(m);

	if (t) {
		t->weight = (unsigned char)M_FW(m);
		t->style = (unsigned char)M_FS(m);
		t->deco = (unsigned char)M_TD(m);
		t->align = (unsigned char)M_TA(m);
		t->fg = fg == FG_DEFAULT ? CSS_RGB_DEFAULT
			: fg ? CSS_RGB_SET | s->pal[fg - 1] : 0;
		if (M_FZ(m))
			t->fsize = s->fsz[M_FZ(m) - 1];
	}
	return M_DISP(m) | (m & M_BEFORE ? CSS_BEFORE : 0) | (m & M_AFTER ? CSS_AFTER : 0);
}

/* the winners' say on the text, as a memo */
static unsigned long win_memo(const struct css_sheet *s, const struct winners *w,
	int k)
{
	int fg = 0;

	if (w->fg) {
		fg = w->fg->t.fg;
		if (fg == FG_VAR && (fg = var_fg(s, w->fg->t.fvar, 0)) == 0)
			fg = w->fg->t.ffb;
	}
	return (unsigned long)(k + 1)
		| (unsigned long)(w->fw ? w->fw->t.fw : 0) << 4
		| (unsigned long)(w->fs ? w->fs->t.fs : 0) << 6
		| (unsigned long)(w->td ? w->td->t.td : 0) << 8
		| (unsigned long)(w->ta ? w->ta->t.ta : 0) << 10
		| (unsigned long)(w->boxed ? M_BOXED : 0)
		| (unsigned long)(w->pseudo & 1 ? M_BEFORE : 0)
		| (unsigned long)(w->pseudo & 2 ? M_AFTER : 0)
		| (unsigned long)fg << 16
		| (unsigned long)(w->fz ? w->fz->t.fz : 0) << 24;
}

/* every rule that holds for node, into *w (its elinfo into *me) */
static void match_all(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw, struct elinfo *me,
	struct winners *w)
{
	struct cmp probe;
	unsigned done[EL_CLASSES + 3];
	int ndone = 0, top, k, i;

	top = set_path(s, d, node);
	el_info(d, node, id, cls, me);
	memset(w, 0, sizeof *w);
	/* the buckets it can be filed under: its id, its classes, its tag,
	 * none; each once */
	memset(&probe, 0, sizeof probe);
	probe.id = me->id;
	if (me->id)
		done[ndone++] = bucket_of(&probe);
	probe.id = 0;
	probe.ncls = 1;
	for (i = 0; i < me->ncls; i++) {
		probe.cls[0] = me->cls[i];
		done[ndone++] = bucket_of(&probe);
	}
	probe.ncls = 0;
	probe.tag = me->tag;
	done[ndone++] = bucket_of(&probe);
	probe.tag = 0;
	done[ndone++] = bucket_of(&probe);
	for (i = 0; i < ndone; i++) {
		for (k = 0; k < i && done[k] != done[i]; k++)
			;
		if (k == i)
			try_bucket(s, done[i], d, me, top, vw, w);
	}
}

int css_style(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw, struct css_text *t)
{
	struct winners w;
	struct elinfo me;
	unsigned long m;
	int k;

	if (t)
		memset(t, 0, sizeof *t);
	if (s == NULL || s->nrules == 0 || node == 0 || node >= d->nnodes)
		return CSS_UNSET;
	if (s->memo_n < d->nnodes) {
		unsigned long c = d->nnodes + 1024;
		unsigned long *q = xrealloc(s->memo, c * sizeof *q);

		if (q) {
			unsigned short *bq = xrealloc(s->bmemo, c * sizeof *bq);
			unsigned long had = s->bmemo ? s->memo_n : 0;

			memset(q + s->memo_n, 0, (c - s->memo_n) * sizeof *q);
			s->memo = q;
			s->memo_n = c;
			/* (the boxes' memo goes with it, or without: not
			 * remembered then) */
			if (bq) {
				memset(bq + had, 0, (c - had) * sizeof *bq);
				s->bmemo = bq;
			} else {
				xfree(s->bmemo);
				s->bmemo = NULL;
			}
		}
	}
	if (s->memo_gen != s->gen || s->memo_vw != vw) {
		if (s->memo)
			memset(s->memo, 0, s->memo_n * sizeof *s->memo);
		if (s->bmemo)
			memset(s->bmemo, 0, s->memo_n * sizeof *s->bmemo);
		s->nbx = 0;
		memset(s->bx_head, 0, sizeof s->bx_head);
		s->memo_gen = s->gen;
		s->memo_vw = vw;
		s->sib_at = 0;
	}
	if (s->memo && node < s->memo_n && s->memo[node])
		return memo_out(s, s->memo[node], t);

	match_all(s, d, node, id, cls, vw, &me, &w);
	/* (never the whole page hidden: some hide it until their script
	 * runs; its text's look counts) */
	k = 0;
	if (me.tag != TAG_HTML && me.tag != TAG_BODY) {
		k = (w.disp && w.disp->disp == CSS_HIDE) || (w.vis && w.vis->vis == CSS_HIDE) ?
			CSS_HIDDEN : 0;
		if (w.ls)
			k |= w.ls->ls == CSS_HIDE ? CSS_NO_MARKER : CSS_MARKER;
	}
	m = win_memo(s, &w, k);
	if (s->memo && node < s->memo_n)
		s->memo[node] = m;
	/* its children's turn next: it is their parent */
	if (s->npath < PATH_DEPTH)
		s->path[s->npath++] = me;
	return memo_out(s, m, t);
}

int css_display(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw)
{
	return css_style(s, d, node, id, cls, vw, NULL);
}

/* the field of struct css_box that box bit k is about, copied from a to b */
static void box_take(struct css_box *b, const struct css_box *a, int k)
{
	unsigned long bit = 1UL << (k % CSS_B_NBITS);

	if (k >= CSS_B_NBITS) {
		k -= CSS_B_NBITS;
		if (k < 4)
			b->bstyle[k] = a->bstyle[k];
		else if (k < 8)
			b->bcolor[k - 4] = a->bcolor[k - 4];
		else if (k == 8) {
			b->bgsrc = a->bgsrc;
			b->bgbase = a->bgbase;
			b->bgsrc_n = a->bgsrc_n;
			b->bgi = a->bgi;
		} else if (k == 9)
			b->bgrep = a->bgrep;
		else if (k == 10)
			b->bgfixed = a->bgfixed;
		else if (k == 11)
			memcpy(b->bgpos, a->bgpos, sizeof b->bgpos);
		else
			b->clip = a->clip;
		b->set2 |= 1UL << k;
		return;
	}
	if (bit == CSS_B_BG)
		b->bg = a->bg;
	else if (bit >= CSS_B_M(0) && bit <= CSS_B_M(3))
		b->m[k - 3] = a->m[k - 3];
	else if (bit >= CSS_B_P(0) && bit <= CSS_B_P(3))
		b->p[k - 7] = a->p[k - 7];
	else if (bit >= CSS_B_BW(0) && bit <= CSS_B_BW(3))
		b->bw[k - 11] = a->bw[k - 11];
	else if (bit == CSS_B_W)
		b->w = a->w;
	else if (bit == CSS_B_H)
		b->h = a->h;
	else if (bit == CSS_B_FLOAT)
		b->fl = a->fl;
	else if (bit == CSS_B_CLEAR)
		b->clr = a->clr;
	else if (bit == CSS_B_DISP)
		b->disp = a->disp;
	else if (bit == CSS_B_LH)
		b->lh = a->lh;
	else if (bit == CSS_B_MINW)
		b->minw = a->minw;
	else if (bit == CSS_B_MAXW)
		b->maxw = a->maxw;
	else if (bit == CSS_B_MINH)
		b->minh = a->minh;
	else if (bit == CSS_B_MAXH)
		b->maxh = a->maxh;
	else if (bit == CSS_B_POS)
		b->pos = a->pos;
	else if (bit >= CSS_B_OFF(0) && bit <= CSS_B_OFF(3))
		b->off[k - 26] = a->off[k - 26];
	else if (bit == CSS_B_Z)
		b->z = a->z;
	b->set |= bit;
}

/* the background-image the sheet's rules gave b, where it is kept */
static void bg_url(const struct css_sheet *s, struct css_box *b)
{
	const char *u;

	if (!(b->set2 & CSS_B2_BGI) || b->bgi == 0)
		return;
	u = s->cpool + b->bgi - 1;
	b->bgbase = u[0] > 1 && u[0] - 2 < s->nurls ? s->urls[u[0] - 2] : NULL;
	b->bgsrc = u + 1;
	b->bgsrc_n = (unsigned short)strlen(u + 1);
	b->bgi = 0;
}

/* what box t says, over *b */
static void box_over(struct css_box *b, const struct css_box *t)
{
	int k;

	for (k = 0; k < BOX_NBITS; k++)
		if (box_bit(t->set, t->set2, k))
			box_take(b, t, k);
}

/* box t among s->bx[] (one the same already there, or added): 1 + its
 * index, 0 when there is no room for it */
static int bx_keep(struct css_sheet *s, const struct css_box *t)
{
	unsigned h = (unsigned)hash_bytes((const char *)t, sizeof *t) & (BX_HASH - 1);
	int i;

	for (i = s->bx_head[h]; i > 0; i = s->bx[i - 1].next)
		if (memcmp(&s->bx[i - 1].b, t, sizeof *t) == 0)
			return i;
	if (s->nbx >= 65535)
		return 0;
	if (s->nbx == s->bx_cap) {
		int c = s->bx_cap ? s->bx_cap * 2 : 64;
		struct bxent *q = xrealloc(s->bx, (size_t)c * sizeof *q);

		if (q == NULL)
			return 0;
		s->bx = q;
		s->bx_cap = c;
	}
	s->bx[s->nbx].b = *t;
	s->bx[s->nbx].next = s->bx_head[h];
	s->bx_head[h] = ++s->nbx;
	return s->nbx;
}

int css_box(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw, struct css_box *b)
{
	struct winners w;
	struct elinfo me;
	struct css_box t;
	int k, memo = 0;

	if (s == NULL || s->nboxes == 0 || node == 0 || node >= d->nnodes)
		return 0;
	/* (css_style remembered whether any rule about boxes holds; the
	 * layout asks again at each of its runs, so what they said is
	 * remembered too: matching is most of a run's style work) */
	if (s->memo && node < s->memo_n && s->memo_gen == s->gen && s->memo_vw == vw) {
		if (s->memo[node] && !(s->memo[node] & M_BOXED))
			return 0;
		if (s->bmemo && s->bmemo[node]) {
			box_over(b, &s->bx[s->bmemo[node] - 1].b);
			return 1;
		}
		memo = s->bmemo != NULL;
	}
	match_all(s, d, node, id, cls, vw, &me, &w);
	if (!w.boxed)
		return 0;
	memset(&t, 0, sizeof t);
	for (k = 0; k < BOX_NBITS; k++)
		if (w.box[k])
			box_take(&t, &s->boxes[w.box[k]->box].b, k);
	bg_url(s, &t);
	if (memo)
		s->bmemo[node] = (unsigned short)bx_keep(s, &t);
	box_over(b, &t);
	return 1;
}

/* a content value for element node into out (n bytes, NUL-ended): its
 * length, or -1 for none (none, normal, or nothing known) */
static int content_text(const char *v, const struct doc *d, nodeid node,
	char *out, size_t n)
{
	const char *e = v + strlen(v);
	size_t o = 0;
	int any = 0;

	if (n == 0)
		return -1;
	while (v < e) {
		const char *w = v;
		char buf[8];
		size_t k;

		if (is_space((unsigned char)*v)) {
			v++;
			continue;
		}
		if (*v == '"' || *v == '\'') {
			char q = *v++;

			while (v < e && *v != q) {
				unsigned long cp = (unsigned char)*v++;

				if (cp == '\\' && v < e) {
					int h, hk = 0;

					cp = 0;
					while (hk < 6 && v < e && (h = hexval((unsigned char)*v)) >= 0) {
						cp = cp * 16 + (unsigned long)h;
						v++;
						hk++;
					}
					if (hk) {
						if (v < e && is_space((unsigned char)*v))
							v++;
					} else if (*v == '\n') {
						v++;
						continue;
					} else
						cp = (unsigned char)*v++;
				}
				if (cp >= 0x80 && (unsigned char)v[-1] >= 0x80 && cp < 0x100) {
					/* (raw UTF-8 bytes: as they are) */
					buf[0] = (char)cp;
					k = 1;
				} else
					k = (size_t)utf8_put(buf, cp ? cp : 0xFFFD);
				if (o + k < n) {
					memcpy(out + o, buf, k);
					o += k;
				}
			}
			if (v < e)
				v++;
			any = 1;
			continue;
		}
		while (v < e && !is_space((unsigned char)*v)) {
			if (*v == '(') {
				v = arg_end(v + 1, e);
				if (v < e)
					v++;
				break;
			}
			v++;
		}
		k = (size_t)(v - w);
		if (starts(w, k, "attr(") && w[k - 1] == ')') {
			char name[32];
			size_t i, j = 0;
			const char *a;

			for (i = 5; i < k - 1 && j + 1 < sizeof name; i++)
				if (!is_space((unsigned char)w[i]))
					name[j++] = (char)lower((unsigned char)w[i]);
			name[j] = '\0';
			a = doc_attr(d, node, attr_lookup(name));
			if (a && o + strlen(a) < n) {
				memcpy(out + o, a, strlen(a));
				o += strlen(a);
			}
			any = 1;
		} else if (is_kw(w, k, "open-quote") || is_kw(w, k, "close-quote")) {
			k = (size_t)utf8_put(buf, w[0] == 'o' || w[0] == 'O' ? 0x201C : 0x201D);
			if (o + k < n) {
				memcpy(out + o, buf, k);
				o += k;
			}
			any = 1;
		} else if (is_kw(w, k, "no-open-quote") || is_kw(w, k, "no-close-quote")
			|| starts(w, k, "counter(") || starts(w, k, "counters(")
			|| starts(w, k, "url(") || starts(w, k, "image-set("))
			any = 1;		/* (nothing shown for it here) */
		else if (k == 1 && *w == '/')
			break;			/* (the alt text after: not shown) */
		else
			return -1;		/* none, normal, inherit, var()... */
	}
	out[o] = '\0';
	return any ? (int)o : -1;
}

int css_pseudo(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw, int which, struct css_text *t,
	struct css_box *b, char *out, size_t n)
{
	struct winners w;
	struct elinfo me;
	int k, len;

	if (t)
		memset(t, 0, sizeof *t);
	if (s == NULL || s->nrules == 0 || node == 0 || node >= d->nnodes
		|| (which != 1 && which != 2))
		return 0;
	s->want = which;
	match_all(s, d, node, id, cls, vw, &me, &w);
	s->want = 0;
	/* (its children's ancestors are as css_style left them) */
	if (s->npath < PATH_DEPTH)
		s->path[s->npath++] = me;
	if (w.cont == NULL || (w.disp && w.disp->disp == CSS_HIDE)
		|| (len = content_text(s->cpool + w.cont->cont - 1, d, node, out, n)) < 0)
		return 0;
	(void)memo_out(s, win_memo(s, &w, 0), t);
	for (k = 0; b && k < BOX_NBITS; k++)
		if (w.box[k])
			box_take(b, &s->boxes[w.box[k]->box].b, k);
	if (b)
		bg_url(s, b);
	return 1;
}

void css_inline_box(const char *style, struct css_box *b)
{
	const char *p = style, *e, *end = style ? style + strlen(style) : NULL;

	while (p && p < end) {
		const char *n, *v;
		size_t pn, vn;
		int imp;
		char nbuf[64];

		e = decl_end(p, end);
		if (split_decl(p, e, &n, &pn, &v, &vn, &imp, nbuf, sizeof nbuf)) {
			struct css_box t;
			unsigned long bits, bits2 = 0;
			int k;

			p = n;
			memset(&t, 0, sizeof t);
			bits = box_decl(p, pn, v, vn, &t, &bits2);
			for (k = 0; k < BOX_NBITS; k++)
				if (box_bit(bits, bits2, k))
					box_take(b, &t, k);
		}
		p = e + 1;
	}
}

/* a gradient's colour, for a screen that doesn't draw them: its colour
 * stops' average. w: "linear-gradient(...)". 1 and *rgb, or 0 */
static int grad_color(const char *w, size_t wn, unsigned long *rgb)
{
	const char *p, *e = w + wn;
	long r = 0, g = 0, b = 0, n = 0;

	for (p = w; p < e && *p != '('; p++)
		;
	while (p < e && *p != ')') {
		const char *a, *q;
		struct cval c;

		for (a = ++p; a < e && is_space((unsigned char)*a); a++)
			;
		p = arg_end(a, e);
		for (q = a; q < p && !is_space((unsigned char)*q); q++)
			;
		if (is_color(a, (size_t)(q - a), &c) == COL_RGB) {
			r += (long)(c.rgb >> 16 & 0xFF);
			g += (long)(c.rgb >> 8 & 0xFF);
			b += (long)(c.rgb & 0xFF);
			n++;
		}
	}
	if (n == 0)
		return 0;
	*rgb = (unsigned long)(r / n) << 16 | (unsigned long)(g / n) << 8
		| (unsigned long)(b / n);
	return 1;
}

/*
 * A background-color value, or (shorthand 1) a background: 1 when
 * browsers take it, its colour in *rgb (CSS_BG_NONE: transparent, as the
 * shorthand is when it names no colour); -1 when they do, but its colour
 * isn't known here (currentcolor, var()); 0 when they drop it ("red
 * pink", "url(x) blah"). Of several layers, only the last has a colour.
 */
static int bg_value(const char *v, size_t vn, int shorthand, unsigned long *rgb,
	struct bgsay *o)
{
	static const char *const rep[] = { "repeat", "no-repeat", "space", "round", NULL };
	static const char *const pos[] = { "left", "right", "top", "bottom", "center", NULL };
	static const char *const boxk[] = { "border-box", "padding-box", "content-box",
		"text", NULL };
	const char *e = v + vn, *p;
	const char *rw[2], *pw[4] = { NULL, NULL, NULL, NULL };
	size_t rwn[2], pwn[4] = { 0, 0, 0, 0 };
	int ncol = 0, nimg = 0, nrep = 0, natt = 0, npos = 0, nsize = 0, nbox = 0;
	int slash = 0, known = 1, ck = COL_NO, depth = 0, i, grad = 0;
	unsigned long grgb = 0;
	struct cval c;
	struct css_len l;

	if (o)
		bg_initial(o);
	if (!shorthand) {
		if ((ck = is_color(v, vn, &c)) == COL_NO)
			return 0;
		goto done;
	}
	for (p = v; p < e; p++)
		if (*p == '(')
			depth++;
		else if (*p == ')' && depth)
			depth--;
		else if (*p == ',' && depth == 0)
			v = p + 1;		/* (the last layer) */
	for (p = v; p < e; ) {
		const char *w;
		size_t wn;

		if (is_space((unsigned char)*p)) {
			p++;
			continue;
		}
		if (*p == '/') {		/* position / size */
			if (slash || npos == 0)
				return 0;
			slash = 1;
			p++;
			continue;
		}
		for (w = p, depth = 0; p < e && (depth || (!is_space((unsigned char)*p)
			&& *p != '/')); p++)
			if (*p == '(')
				depth++;
			else if (*p == ')' && depth)
				depth--;
		wn = (size_t)(p - w);
		if (slash == 1) {
			if (nsize < 2 && (is_kw(w, wn, "cover") || is_kw(w, wn, "contain"))) {
				if (nsize)
					return 0;
				nsize = 2;
				continue;
			}
			if (nsize < 2 && (parse_len(w, wn, &l) || (is_wild(w, wn)
				&& !starts(w, wn, "var(")))) {
				nsize++;
				continue;
			}
			if (nsize == 0)
				return 0;
			slash = 2;		/* (the size is done) */
		}
		if (starts(w, wn, "var(")) {
			known = 0;		/* (could be any part) */
			continue;
		}
		if (is_kw(w, wn, "none") || starts(w, wn, "url(") || has(w, wn, "gradient(")
			|| starts(w, wn, "image-set(") || starts(w, wn, "-webkit-image-set(")) {
			if (nimg++)
				return 0;
			if (has(w, wn, "gradient("))
				grad = grad_color(w, wn, &grgb);
			if (o)
				url_of(w, wn, &o->src, &o->n);
			continue;
		}
		if (is_kw(w, wn, "repeat-x") || is_kw(w, wn, "repeat-y")) {
			if (nrep)
				return 0;
			rw[0] = w;
			rwn[0] = wn;
			rwn[1] = 0;
			nrep = 2;
			continue;
		}
		for (i = 0; rep[i] && !is_kw(w, wn, rep[i]); i++)
			;
		if (rep[i]) {
			if (nrep == 2)
				return 0;
			rw[nrep] = w;
			rwn[nrep++] = wn;
			continue;
		}
		if (is_kw(w, wn, "scroll") || is_kw(w, wn, "fixed") || is_kw(w, wn, "local")) {
			if (natt++)
				return 0;
			if (o)
				o->fixed = is_kw(w, wn, "fixed");
			continue;
		}
		for (i = 0; boxk[i] && !is_kw(w, wn, boxk[i]); i++)
			;
		if (boxk[i]) {
			if (nbox++ == 2)
				return 0;
			continue;
		}
		for (i = 0; pos[i] && !is_kw(w, wn, pos[i]); i++)
			;
		if (pos[i] || (parse_len(w, wn, &l) && l.u != CSS_U_AUTO) || is_wild(w, wn)) {
			if (slash || npos == 4)
				return 0;
			pw[npos] = w;
			pwn[npos++] = wn;
			continue;
		}
		if (!ncol && (ck = is_color(w, wn, &c)) != COL_NO) {
			ncol = 1;
			continue;
		}
		return 0;
	}
	if (slash == 1 && nsize == 0)
		return 0;
	if (!known)
		return -1;
	if (o) {
		if (nrep)
			o->rep = (unsigned char)bg_repeat(rw, rwn, nrep == 2 && rwn[1] ? 2 : 1);
		bg_pos(pw, pwn, npos, o->pos);
	}
done:
	if (ck == COL_RGB)
		*rgb = c.rgb;
	else if (ck == COL_OTHER)
		return -1;
	else if (ck == COL_NO && grad)
		*rgb = grgb;		/* (the gradient, roughly) */
	else
		*rgb = CSS_BG_NONE;	/* (transparent, or no colour) */
	return 1;
}

int css_attr_color(const char *v, unsigned long *rgb)
{
	struct cval c;
	size_t n, i;
	char b[8];

	if (v == NULL)
		return 0;
	n = strlen(v);
	parse_color(v, n, &c, 0);
	if (c.kind == CV_RGB) {
		*rgb = c.rgb;
		return 1;
	}
	/* bgcolor=ffff99: hex, the '#' left out */
	while (n && is_space((unsigned char)v[n - 1]))
		n--;
	while (n && is_space((unsigned char)*v)) {
		v++;
		n--;
	}
	if (n != 3 && n != 6)
		return 0;
	for (i = 0; i < n; i++)
		if (!((v[i] >= '0' && v[i] <= '9') || ((v[i] | 32) >= 'a' && (v[i] | 32) <= 'f')))
			return 0;
	b[0] = '#';
	memcpy(b + 1, v, n);
	parse_color(b, n + 1, &c, 0);
	if (c.kind != CV_RGB)
		return 0;
	*rgb = c.rgb;
	return 1;
}

void css_inline_text(struct css_sheet *s, const char *style, struct css_text *t)
{
	const char *p = style, *e, *end = style ? style + strlen(style) : NULL;

	while (p && p < end) {
		const char *n, *v;
		size_t pn, vn;
		int imp;
		char nbuf[64];

		e = decl_end(p, end);
		if (split_decl(p, e, &n, &pn, &v, &vn, &imp, nbuf, sizeof nbuf)) {
			struct tdecl td;

			p = n;
			memset(&td, 0, sizeof td);
			if (pn == 5 && starts(p, pn, "color")) {
				/* (as RGB: no palette needed) */
				struct cval c;
				int k = 0;

				parse_color(v, vn, &c, 0);
				if (c.kind == CV_VAR && s) {
					if ((k = var_fg(s, c.var, 0)) != 0)
						t->fg = k == FG_DEFAULT ? CSS_RGB_DEFAULT
							: CSS_RGB_SET | s->pal[k - 1];
				}
				if (c.kind == CV_VAR && k == 0) {
					c.kind = c.fbk;
					c.rgb = c.fbrgb;
				}
				if (c.kind == CV_RGB)
					t->fg = CSS_RGB_SET | c.rgb;
				else if (c.kind == CV_DEFAULT)
					t->fg = CSS_RGB_DEFAULT;
			} else if ((pn == 16 && starts(p, pn, "background-color"))
				|| (pn == 10 && starts(p, pn, "background"))) {
				unsigned long rgb;

				if (bg_value(v, vn, pn == 10, &rgb, NULL) > 0 && rgb != CSS_BG_NONE)
					t->bg = CSS_RGB_SET | rgb;
			} else if (pn == 9 && starts(p, pn, "font-size")) {
				struct css_len l;

				if (font_size(v, vn, &l))
					t->fsize = l;
			} else if (text_decl(NULL, p, pn, v, vn, imp, &td)) {
				/* (s is only looked at for a colour) */
				if (pn == 4 && starts(p, pn, "font")) {
					struct css_len l;

					if (font_short_size(v, vn, &l))
						t->fsize = l;
				}
				if (td.fw)
					t->weight = td.fw;
				if (td.fs)
					t->style = td.fs;
				if (td.td)
					t->deco = td.td;
				if (td.ta)
					t->align = td.ta;
			}
		}
		p = e + 1;
	}
}

int css_inline_list(const char *css)
{
	const char *p, *v;

	for (p = css; p && *p; p++) {
		int type;

		if ((*p != 'l' && *p != 'L') || !starts(p, strlen(p), "list-style"))
			continue;
		v = p + 10;
		type = starts(v, strlen(v), "-type");
		if (type)
			v += 5;
		while (is_space((unsigned char)*v))
			v++;
		if (*v != ':')
			continue;
		v++;
		while (is_space((unsigned char)*v))
			v++;
		return list_style(v, strcspn(v, ";"), type) == CSS_HIDE ?
			CSS_NO_MARKER : CSS_MARKER;
	}
	return 0;
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
