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
 */
#include <string.h>
#include "os.h"
#include "tags.h"
#include "css.h"

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

/* what a rule (or a style="") says of text: 0 where nothing */
struct tdecl {
	unsigned long fvar;		/* fg FG_VAR: the custom property's hash */
	unsigned char fg;		/* palette index, FG_VAR or FG_DEFAULT */
	unsigned char ffb;		/* FG_VAR: the fallback's (0: none) */
	unsigned char fw, fs, td, ta;	/* CSS_FW_*, CSS_FS_*, CSS_TD_*, CSS_TA_* */
	unsigned char imp;		/* !important: T_FG, T_FW... bits */
};

#define T_FG	1
#define T_FW	2
#define T_FS	4
#define T_TD	8
#define T_TA	16

struct rule {
	unsigned long spec;		/* ids << 16 | classes << 8 | types */
	unsigned long order;
	unsigned short cmp, ncmp;	/* s->cmps[cmp...], the subject first */
	unsigned short media;		/* 0: all; else s->media[media - 1] */
	unsigned char disp, vis, ls;	/* CSS_SHOW / CSS_HIDE / CSS_UNSET */
	unsigned char disp_imp, vis_imp, ls_imp;	/* !important */
	struct tdecl t;			/* the look of text */
	int next;			/* the next rule in its bucket, -1 */
};

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

struct css_sheet {
	struct rule *rules;
	int nrules, rules_cap;
	int ntext;			/* rules about text only */
	unsigned long pal[PAL_MAX];	/* the colours: index i is pal[i - 1] */
	int npal;
	short pslot[NPSLOT];		/* colour -> index, 0: empty */
	struct cvar *vars;		/* NVSLOT of them, once there is one */
	int nvars;
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
	/* what was said of each node, M_* below; 0: not asked */
	unsigned long *memo;
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
	xfree(s->vars);
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
		if (c == '+' || c == '~' || c == '|' || c == ',')
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
			/* :root, :link; any other state, or a pseudo-element,
			 * isn't followed */
			int k;

			p++;
			if ((k = read_name(&p, e, name, sizeof name)) < 0)
				return 0;
			for (i = 0; i < k; i++)
				name[i] = (char)lower((unsigned char)name[i]);
			if (strcmp(name, "root") == 0) {
				if (cur.tag && cur.tag != TAG_HTML)
					return 0;
				cur.tag = TAG_HTML;
			} else if (strcmp(name, "link") == 0 || strcmp(name, "any-link") == 0) {
				if (cur.attr || (cur.tag && cur.tag != TAG_A))
					return 0;
				cur.tag = TAG_A;
				cur.attr = ATTR_HREF;
				cur.aop = 0;
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

/* do declarations d (n bytes) mention display, visibility, list-style,
 * color, font, text- or a custom property at all? (rules that don't are
 * dropped without reading them; a false yes only costs a reading) */
static int mentions(const char *d, int n)
{
	int i;

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
	} else if (pn == 4 && starts(p, pn, "font")) {
		/* the shorthand: what it doesn't say is normal */
		size_t i;
		int b = 0;

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

	if (s->plong || s->plen == 0 || !mentions(d, s->dlen))
		return;
	memset(&t, 0, sizeof t);
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
		if (imp) {
			size_t k = 0;

			while (k < vn && v[k] != '!')
				k++;
			vn = k;
		}
		while (vn && is_space((unsigned char)v[vn - 1]))
			vn--;
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
		if (text_decl(s, p, pn, v, vn, imp, &t))
			continue;
		if (pn == 7 && starts(p, pn, "display")) {
			disp = starts(v, vn, "none") ? CSS_HIDE : CSS_SHOW;
			disp_imp = imp;
		} else if (pn == 10 && starts(p, pn, "visibility")) {
			if (starts(v, vn, "hidden") || starts(v, vn, "collapse"))
				vis = CSS_HIDE;
			else if (starts(v, vn, "visible"))
				vis = CSS_SHOW;
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
	if (!disp && !vis && !ls && !t.fg && !t.fw && !t.fs && !t.td && !t.ta)
		return;
	if (!disp && !vis && !ls && s->ntext >= MAX_TEXT_RULES)
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
		b = (int)bucket_of(&sel[0]);
		r->next = s->bucket[b];
		s->bucket[b] = s->nrules++;
		if (!disp && !vis && !ls)
			s->ntext++;
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

/* the rule that wins each property for an element */
struct winners {
	const struct rule *disp, *vis, *ls, *fg, *fw, *fs, *td, *ta;
};

/* does r's say on the text property with bit beat w's? */
#define TBEATS(r, w, bit) beats(((r)->t.imp & (bit)) != 0, r, \
	(w) ? ((w)->t.imp & (bit)) != 0 : 0, w)

/* the rules filed under key bucket b that hold, into the winners */
static void try_bucket(const struct css_sheet *s, unsigned b, const struct doc *d,
	const struct elinfo *e, int top, int vw, struct winners *w)
{
	int i;

	for (i = s->bucket[b]; i >= 0; i = s->rules[i].next) {
		const struct rule *r = &s->rules[i];

		if (!media_ok(s, r->media, vw) || !rule_match(s, r, d, e, top))
			continue;
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
	}
	return M_DISP(m);
}

int css_style(struct css_sheet *s, const struct doc *d, nodeid node,
	const char *id, const char *cls, int vw, struct css_text *t)
{
	struct winners w;
	struct elinfo me;
	struct cmp probe;
	unsigned done[EL_CLASSES + 3];
	unsigned long m;
	int ndone = 0, top, k, i, fg = 0;

	if (t)
		memset(t, 0, sizeof *t);
	if (s == NULL || s->nrules == 0 || node == 0 || node >= d->nnodes)
		return CSS_UNSET;
	if (s->memo_n < d->nnodes) {
		unsigned long c = d->nnodes + 1024;
		unsigned long *q = xrealloc(s->memo, c * sizeof *q);

		if (q) {
			memset(q + s->memo_n, 0, (c - s->memo_n) * sizeof *q);
			s->memo = q;
			s->memo_n = c;
		}
	}
	if (s->memo_gen != s->gen || s->memo_vw != vw) {
		if (s->memo)
			memset(s->memo, 0, s->memo_n * sizeof *s->memo);
		s->memo_gen = s->gen;
		s->memo_vw = vw;
	}
	if (s->memo && node < s->memo_n && s->memo[node])
		return memo_out(s, s->memo[node], t);

	top = set_path(s, d, node);
	el_info(d, node, id, cls, &me);
	memset(&w, 0, sizeof w);
	/* the buckets it can be filed under: its id, its classes, its tag,
	 * none; each once */
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
			try_bucket(s, done[i], d, &me, top, vw, &w);
	}
	/* (never the whole page hidden: some hide it until their script
	 * runs; its text's look counts) */
	k = 0;
	if (me.tag != TAG_HTML && me.tag != TAG_BODY) {
		k = (w.disp && w.disp->disp == CSS_HIDE) || (w.vis && w.vis->vis == CSS_HIDE) ?
			CSS_HIDDEN : 0;
		if (w.ls)
			k |= w.ls->ls == CSS_HIDE ? CSS_NO_MARKER : CSS_MARKER;
	}
	if (w.fg) {
		fg = w.fg->t.fg;
		if (fg == FG_VAR && (fg = var_fg(s, w.fg->t.fvar, 0)) == 0)
			fg = w.fg->t.ffb;
	}
	m = (unsigned long)(k + 1)
		| (unsigned long)(w.fw ? w.fw->t.fw : 0) << 4
		| (unsigned long)(w.fs ? w.fs->t.fs : 0) << 6
		| (unsigned long)(w.td ? w.td->t.td : 0) << 8
		| (unsigned long)(w.ta ? w.ta->t.ta : 0) << 10
		| (unsigned long)fg << 16;
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

void css_inline_text(struct css_sheet *s, const char *style, struct css_text *t)
{
	const char *p = style, *e;

	while (p && *p) {
		const char *colon, *v, *x;
		size_t pn, vn;
		int imp;

		for (e = p; *e && *e != ';'; e++)
			;
		while (p < e && is_space((unsigned char)*p))
			p++;
		for (colon = p; colon < e && *colon != ':'; colon++)
			;
		if (colon < e) {
			struct tdecl td;

			pn = (size_t)(colon - p);
			while (pn && is_space((unsigned char)p[pn - 1]))
				pn--;
			v = colon + 1;
			while (v < e && is_space((unsigned char)*v))
				v++;
			for (x = v; x < e && *x != '!'; x++)
				;
			imp = x < e;
			vn = (size_t)(x - v);
			while (vn && is_space((unsigned char)v[vn - 1]))
				vn--;
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
			} else if (text_decl(s, p, pn, v, vn, imp, &td)) {
				/* (s is only looked at for a colour) */
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
		p = *e ? e + 1 : e;
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
