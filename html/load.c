/*
 * load.c - see load.h.
 *
 * Charset, in order: the HTTP header, a byte order mark, a <meta> in the
 * first LOAD_SNIFF bytes; failing those, UTF-8 if those bytes are valid
 * UTF-8 with some non-ASCII in them, else windows-1252 - and UTF-8 again
 * when they are plain ASCII (undeclared pages today are mostly UTF-8; the
 * HTML5 default of windows-1252 is from another era).
 */
#include <string.h>
#include "tags.h"
#include "load.h"

#define CHUNK	512

static int ci_eq(const unsigned char *s, const char *lit, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		int c = s[i];

		if (c >= 'A' && c <= 'Z')
			c += 32;
		if (c != lit[i])
			return 0;
	}
	return 1;
}

/* <meta charset=x> or <meta http-equiv content="...; charset=x"> */
static enum charset meta_prescan(const unsigned char *s, size_t n)
{
	size_t i;

	for (i = 0; i + 5 < n; i++) {
		size_t j, end;

		if (s[i] != '<' || !ci_eq(s + i + 1, "meta", 4))
			continue;
		for (end = i + 5; end < n && s[end] != '>'; end++)
			;
		for (j = i + 5; j + 8 <= end; j++) {
			if (ci_eq(s + j, "charset", 7)) {
				char label[40];
				size_t k = j + 7, m = 0;

				while (k < end && (s[k] == ' ' || s[k] == '='))
					k++;
				if (k < end && (s[k] == '"' || s[k] == '\''))
					k++;
				while (k < end && m < sizeof label - 1 && s[k] != '"'
					&& s[k] != '\'' && s[k] != ';' && s[k] != ' '
					&& s[k] != '/' && s[k] != '>')
					label[m++] = (char)s[k++];
				label[m] = '\0';
				if (m) {
					enum charset cs = charset_from_label(label);

					if (cs != CS_UNKNOWN)
						return cs;
				}
			}
		}
	}
	return CS_UNKNOWN;
}

/* valid UTF-8? A sequence cut off at the end is fine when the bytes are
 * only the start of the document (!complete). *high: any non-ASCII */
static int looks_utf8(const unsigned char *s, size_t n, int complete, int *high)
{
	size_t i = 0;

	*high = 0;
	while (i < n) {
		unsigned char c = s[i];
		int need;
		size_t k;

		if (c < 0x80) {
			i++;
			continue;
		}
		*high = 1;
		need = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2
			: c >= 0xF0 && c <= 0xF4 ? 3 : -1;
		if (need < 0)
			return 0;
		for (k = 1; k <= (size_t)need; k++) {
			if (i + k >= n)
				return !complete;
			if ((s[i + k] & 0xC0) != 0x80)
				return 0;
		}
		i += (size_t)need + 1;
	}
	return 1;
}

static enum charset sniff(const unsigned char *s, size_t n, int complete,
	int *from, size_t *bom_len)
{
	enum charset cs;
	int high;

	*bom_len = 0;
	if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {
		*from = CS_FROM_BOM;
		*bom_len = 3;
		return CS_UTF8;
	}
	if ((cs = meta_prescan(s, n)) != CS_UNKNOWN) {
		*from = CS_FROM_META;
		return cs;
	}
	*from = CS_FROM_GUESS;
	if (!looks_utf8(s, n, complete, &high))
		return CS_WIN1252;
	return CS_UTF8;
}

enum charset html_sniff(const unsigned char *s, size_t n, int *from,
	size_t *bom_len)
{
	return sniff(s, n, 1, from, bom_len);
}

static void run(struct html_load *l, const unsigned char *s, size_t n)
{
	static char out[3 * CHUNK + 8];

	while (n) {
		size_t k = n < CHUNK ? n : CHUNK, o;

		o = decoder_run(&l->dec, s, k, out);
		tok_feed(&l->tok, out, o);
		s += k;
		n -= k;
	}
}

void html_load_begin(struct html_load *l, struct doc *d,
	const char *http_charset, int plain_text)
{
	struct tok_sink sink;
	enum charset cs = http_charset && *http_charset
		? charset_from_label(http_charset) : CS_UNKNOWN;

	memset(l, 0, sizeof *l);
	l->d = d;
	tree_init(&l->tree, d);
	sink.ctx = &l->tree;
	sink.tag = tree_tag;
	sink.text = tree_text;
	tok_init(&l->tok, &sink);
	l->plain = plain_text;
	if (cs != CS_UNKNOWN) {
		l->cs = cs;
		l->cs_from = CS_FROM_HTTP;
		decoder_init(&l->dec, cs);
	} else
		l->sniffing = 1;
	if (plain_text) {
		/* text/plain: everything after <plaintext> is text, kept as is */
		static const char pre[] = "<plaintext>";

		tok_feed(&l->tok, pre, sizeof pre - 1);
	}
}

static void sniff_done(struct html_load *l, int complete)
{
	size_t bom;

	l->cs = sniff(l->sniff, l->sniff_len, complete, &l->cs_from, &bom);
	decoder_init(&l->dec, l->cs);
	l->sniffing = 0;
	run(l, l->sniff + bom, l->sniff_len - bom);
}

void html_load_feed(struct html_load *l, const unsigned char *s, size_t n)
{
	l->bytes_in += n;
	if (l->sniffing) {
		size_t k = sizeof l->sniff - l->sniff_len;

		if (k > n)
			k = n;
		memcpy(l->sniff + l->sniff_len, s, k);
		l->sniff_len += k;
		s += k;
		n -= k;
		if (l->sniff_len < sizeof l->sniff)
			return;
		sniff_done(l, 0);
	}
	run(l, s, n);
}

void html_load_end(struct html_load *l)
{
	char out[8];
	size_t o;

	if (l->sniffing)
		sniff_done(l, 1);
	o = decoder_end(&l->dec, out);
	tok_feed(&l->tok, out, o);
	tok_end(&l->tok);
	tree_end(&l->tree);
}
