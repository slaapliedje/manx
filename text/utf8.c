/*
 * utf8.c - see utf8.h.
 */
#include <string.h>
#include <ctype.h>
#include "utf8.h"

/* windows-1252 0x80..0x9F (the rest of the high half is Latin-1) */
static const unsigned short cp1252[32] = {
	0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
	0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

static int label_is(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != *b)
			return 0;
	return *a == '\0' && *b == '\0';
}

enum charset charset_from_label(const char *label)
{
	static const char *const utf8[] = { "utf-8", "utf8", "unicode-1-1-utf-8", 0 };
	static const char *const w1252[] = { "windows-1252", "iso-8859-1",
		"iso8859-1", "latin1", "l1", "us-ascii", "ascii", "cp1252",
		"iso_8859-1", "x-cp1252", "cp819", "ibm819", "iso-ir-100", 0 };
	char buf[32];
	size_t i, n;

	/* trim white space */
	while (*label == ' ' || *label == '\t' || *label == '\n' || *label == '\r')
		label++;
	for (n = 0; label[n] && label[n] != ' ' && label[n] != '\t'
		&& label[n] != '\n' && label[n] != '\r' && label[n] != ';'
		&& label[n] != '"' && label[n] != '\'' && n < sizeof buf - 1; n++)
		buf[n] = label[n];
	buf[n] = '\0';
	for (i = 0; utf8[i]; i++)
		if (label_is(buf, utf8[i]))
			return CS_UTF8;
	for (i = 0; w1252[i]; i++)
		if (label_is(buf, w1252[i]))
			return CS_WIN1252;
	return CS_UNKNOWN;
}

int utf8_put(char *o, unsigned long cp)
{
	if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
		cp = UCS_REPLACEMENT;
	if (cp < 0x80) {
		o[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		o[0] = (char)(0xC0 | (cp >> 6));
		o[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = (char)(0xE0 | (cp >> 12));
		o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		o[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	o[0] = (char)(0xF0 | (cp >> 18));
	o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	o[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

void decoder_init(struct decoder *d, enum charset cs)
{
	memset(d, 0, sizeof *d);
	d->cs = cs == CS_UNKNOWN ? CS_WIN1252 : cs;
}

/* expected length of a UTF-8 sequence from its lead byte (0: invalid) */
static int seq_len(unsigned char c)
{
	if (c < 0x80) return 1;
	if (c >= 0xC2 && c <= 0xDF) return 2;
	if (c >= 0xE0 && c <= 0xEF) return 3;
	if (c >= 0xF0 && c <= 0xF4) return 4;
	return 0;
}

size_t decoder_run(struct decoder *d, const unsigned char *in, size_t n,
	char *out)
{
	size_t o = 0, i = 0;

	if (d->cs == CS_WIN1252) {
		for (; i < n; i++) {
			unsigned char c = in[i];

			if (c == 0)
				o += (size_t)utf8_put(out + o, UCS_REPLACEMENT);
			else if (c < 0x80)
				out[o++] = (char)c;
			else if (c < 0xA0)
				o += (size_t)utf8_put(out + o, cp1252[c - 0x80]);
			else
				o += (size_t)utf8_put(out + o, c);
		}
		return o;
	}

	/*
	 * The WHATWG UTF-8 decoder, a byte at a time, so where the input
	 * is split can't change the result: each maximal invalid subpart
	 * becomes one U+FFFD, and the byte that broke a sequence is read
	 * again as the start of the next.
	 */
	while (i < n) {
		unsigned char c = in[i];

		if (d->need == 0 && c < 0x80 && c != 0) {
			/* a run of ASCII: copied as is */
			size_t j = i + 1;

			while (j < n && in[j] < 0x80 && in[j] != 0)
				j++;
			memcpy(out + o, in + i, j - i);
			o += j - i;
			i = j;
			continue;
		}
		if (d->need == 0) {
			i++;
			if (c < 0x80) {
				if (c == 0)
					o += (size_t)utf8_put(out + o, UCS_REPLACEMENT);
				else
					out[o++] = (char)c;
			} else if (c >= 0xC2 && c <= 0xDF) {
				d->need = 1;
				d->cp = c & 0x1F;
			} else if (c >= 0xE0 && c <= 0xEF) {
				if (c == 0xE0) d->lo = 0xA0;	/* no overlongs */
				if (c == 0xED) d->hi = 0x9F;	/* no surrogates */
				d->need = 2;
				d->cp = c & 0x0F;
			} else if (c >= 0xF0 && c <= 0xF4) {
				if (c == 0xF0) d->lo = 0x90;
				if (c == 0xF4) d->hi = 0x8F;	/* <= U+10FFFF */
				d->need = 3;
				d->cp = c & 0x07;
			} else
				o += (size_t)utf8_put(out + o, UCS_REPLACEMENT);
			continue;
		}
		if (c < (d->lo ? d->lo : 0x80) || c > (d->hi ? d->hi : 0xBF)) {
			/* broken: one replacement, then c again (not consumed) */
			d->need = d->seen = 0;
			d->lo = d->hi = 0;
			o += (size_t)utf8_put(out + o, UCS_REPLACEMENT);
			continue;
		}
		i++;
		d->lo = d->hi = 0;
		d->cp = d->cp << 6 | (c & 0x3F);
		if (++d->seen == d->need) {
			o += (size_t)utf8_put(out + o, d->cp);
			d->need = d->seen = 0;
		}
	}
	return o;
}

size_t decoder_end(struct decoder *d, char *out)
{
	if (d->need) {
		d->need = d->seen = 0;
		d->lo = d->hi = 0;
		return (size_t)utf8_put(out, UCS_REPLACEMENT);
	}
	return 0;
}

unsigned long utf8_get(const char **pp)
{
	const unsigned char *p = (const unsigned char *)*pp;
	unsigned long cp;
	int n = seq_len(p[0]), i;

	if (n <= 1) {
		*pp += 1;
		return p[0] < 0x80 ? p[0] : UCS_REPLACEMENT;
	}
	cp = p[0] & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
	for (i = 1; i < n; i++) {
		if ((p[i] & 0xC0) != 0x80) {
			*pp += i;
			return UCS_REPLACEMENT;
		}
		cp = cp << 6 | (p[i] & 0x3F);
	}
	*pp += n;
	return cp;
}

/* --- transliteration ------------------------------------------------- */

/* Latin-1 letters U+00C0..U+00FF as plain ASCII */
static const char *const latin1_ascii[64] = {
	"A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
	"D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
	"a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
	"d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"
};

/* U+00A0..U+00BF as plain ASCII */
static const char *const latin1_sym[32] = {
	" ", "!", "c", "L", "*", "Y", "|", "S", "\"", "(c)", "a", "<<", "-", "", "(R)", "-",
	"o", "+-", "2", "3", "'", "u", "P", ".", ",", "1", "o", ">>", "1/4", "1/2", "3/4", "?"
};

struct tl {
	unsigned short cp;
	char s[5];
};

/* punctuation and symbols that web pages use a lot, sorted */
static const struct tl others[] = {
	{ 0x0131, "i" }, { 0x0152, "OE" }, { 0x0153, "oe" }, { 0x0160, "S" },
	{ 0x0161, "s" }, { 0x0178, "Y" }, { 0x017D, "Z" }, { 0x017E, "z" },
	{ 0x0192, "f" }, { 0x02C6, "^" }, { 0x02DC, "~" }, { 0x0391, "A" },
	{ 0x03B1, "a" }, { 0x03B2, "b" }, { 0x03BC, "u" }, { 0x03C0, "pi" },
	{ 0x2002, " " }, { 0x2003, " " }, { 0x2009, " " }, { 0x200A, " " },
	{ 0x2010, "-" }, { 0x2011, "-" }, { 0x2012, "-" }, { 0x2013, "-" },
	{ 0x2014, "--" }, { 0x2015, "--" }, { 0x2018, "'" }, { 0x2019, "'" },
	{ 0x201A, "," }, { 0x201C, "\"" }, { 0x201D, "\"" }, { 0x201E, "\"" },
	{ 0x2020, "+" }, { 0x2021, "++" }, { 0x2022, "*" }, { 0x2026, "..." },
	{ 0x202F, " " }, { 0x2030, "%o" }, { 0x2032, "'" }, { 0x2033, "\"" },
	{ 0x2039, "<" }, { 0x203A, ">" }, { 0x2044, "/" }, { 0x20AC, "EUR" },
	{ 0x2122, "TM" }, { 0x2190, "<-" }, { 0x2191, "^" }, { 0x2192, "->" },
	{ 0x2193, "v" }, { 0x2194, "<->" }, { 0x21D2, "=>" }, { 0x2212, "-" },
	{ 0x2248, "~" }, { 0x2260, "!=" }, { 0x2264, "<=" }, { 0x2265, ">=" },
	{ 0x22C5, "." }, { 0x25AA, "*" }, { 0x25B2, "^" }, { 0x25B6, ">" },
	{ 0x25BA, ">" }, { 0x25BC, "v" }, { 0x25C0, "<" }, { 0x25CF, "*" },
	{ 0x2605, "*" }, { 0x2606, "*" }, { 0x2713, "v" }, { 0x2714, "v" },
	{ 0x2715, "x" }, { 0x2716, "x" }, { 0x2717, "x" }, { 0x2718, "x" },
	{ 0x3000, " " }, { 0xFEFF, "" }, { 0xFFFD, "?" },
};

int translit(unsigned long cp, int latin1, char *out)
{
	const char *s = NULL;
	size_t lo, hi;

	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp >= 0xA0 && cp <= 0xFF) {
		if (latin1) {
			out[0] = (char)cp;
			return 1;
		}
		s = cp < 0xC0 ? latin1_sym[cp - 0xA0] : latin1_ascii[cp - 0xC0];
	} else if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060
		|| cp == 0x00AD || cp == 0xFEFF || (cp >= 0xFE00 && cp <= 0xFE0F))
		return 0;			/* zero width */
	else {
		lo = 0;
		hi = sizeof others / sizeof others[0];
		while (lo < hi) {
			size_t mid = (lo + hi) / 2;

			if (others[mid].cp == cp) {
				s = others[mid].s;
				break;
			}
			if (others[mid].cp < cp)
				lo = mid + 1;
			else
				hi = mid;
		}
		/* accented Latin Extended-A letters: their base letter */
		if (s == NULL && cp >= 0x100 && cp <= 0x17F) {
			/* generated from Unicode NFD base letters */
			static const char base[] =
				"AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlL"
				"lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

			if (cp - 0x100 < sizeof base - 1) {
				out[0] = base[cp - 0x100];
				return 1;
			}
		}
	}
	if (s == NULL) {
		out[0] = '?';
		return 1;
	}
	{
		size_t n = strlen(s);

		memcpy(out, s, n);
		return (int)n;
	}
}

/* --- display width ----------------------------------------------------- */

int ucs_width(unsigned long cp)
{
	if (cp < 0x300)
		return cp == 0xAD ? 0 : 1;
	/* combining marks and zero-width characters */
	if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x483 && cp <= 0x489)
		|| (cp >= 0x591 && cp <= 0x5BD) || (cp >= 0x610 && cp <= 0x61A)
		|| (cp >= 0x64B && cp <= 0x65F) || (cp >= 0x1AB0 && cp <= 0x1AFF)
		|| (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x200B && cp <= 0x200F)
		|| (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F)
		|| (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0x2060 || cp == 0xFEFF)
		return 0;
	/* East Asian wide and fullwidth, emoji */
	if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF
		&& cp != 0x303F) || (cp >= 0xAC00 && cp <= 0xD7A3)
		|| (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F)
		|| (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6)
		|| (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF)
		|| (cp >= 0x20000 && cp <= 0x3FFFD))
		return 2;
	return 1;
}
