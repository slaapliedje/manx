/*
 * mime.c - the pieces of mail (mime.h).
 */
#include <stdio.h>
#include <string.h>
#include "os.h"
#include "utf8.h"
#include "imap.h"
#include "mime.h"

static const char b64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const unsigned char *in, size_t n, char *out)
{
	size_t i, o = 0;

	for (i = 0; i + 2 < n; i += 3) {
		unsigned long v = (unsigned long)in[i] << 16 | (unsigned long)in[i + 1] << 8 | in[i + 2];

		out[o++] = b64[v >> 18 & 63];
		out[o++] = b64[v >> 12 & 63];
		out[o++] = b64[v >> 6 & 63];
		out[o++] = b64[v & 63];
	}
	if (i < n) {
		unsigned long v = (unsigned long)in[i] << 16 | (i + 1 < n ? (unsigned long)in[i + 1] << 8 : 0);

		out[o++] = b64[v >> 18 & 63];
		out[o++] = b64[v >> 12 & 63];
		out[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
		out[o++] = '=';
	}
	out[o] = '\0';
	return o;
}

/* a base64 digit's value (modified UTF-7's ',' for '/' too), or -1 */
static int b64_val(int c, int comma)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == (comma ? ',' : '/'))
		return 63;
	return -1;
}

static size_t b64_run(const char *in, size_t n, unsigned char *out, int comma)
{
	unsigned long v = 0;
	int bits = 0;
	size_t i, o = 0;

	for (i = 0; i < n; i++) {
		int d = b64_val((unsigned char)in[i], comma);

		if (in[i] == '=')
			break;
		if (d < 0)
			continue;
		v = (v << 6 | (unsigned long)d) & 0xFFFFFF;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (unsigned char)(v >> bits & 255);
		}
	}
	return o;
}

size_t b64_decode(const char *in, size_t n, unsigned char *out)
{
	return b64_run(in, n, out, 0);
}

static int hex(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

size_t qp_decode(const char *in, size_t n, unsigned char *out, int header)
{
	size_t i, o = 0;

	for (i = 0; i < n; i++) {
		if (in[i] == '=') {
			int h, l;

			/* a soft line break: =, maybe spaces, then the line's end */
			size_t j = i + 1;

			while (j < n && (in[j] == ' ' || in[j] == '\t'))
				j++;
			if (j < n && (in[j] == '\r' || in[j] == '\n')) {
				if (in[j] == '\r' && j + 1 < n && in[j + 1] == '\n')
					j++;
				i = j;
				continue;
			}
			if (j >= n && !header) {
				i = j;		/* (= at the very end) */
				continue;
			}
			if (i + 2 < n && (h = hex((unsigned char)in[i + 1])) >= 0
				&& (l = hex((unsigned char)in[i + 2])) >= 0) {
				out[o++] = (unsigned char)(h << 4 | l);
				i += 2;
				continue;
			}
		}
		out[o++] = header && in[i] == '_' ? ' ' : (unsigned char)in[i];
	}
	return o;
}

char *mime_utf8(const char *cs, const unsigned char *s, size_t n)
{
	enum charset c = cs && *cs ? charset_from_label(cs) : CS_WIN1252;
	struct decoder d;
	char *out = xmalloc(3 * n + 9);
	size_t o;

	if (out == NULL)
		return NULL;
	decoder_init(&d, c == CS_UNKNOWN ? CS_WIN1252 : c);
	o = decoder_run(&d, s, n, out);
	o += decoder_end(&d, out + o);
	out[o] = '\0';
	return out;
}

/* one =?cs?X?text?= word at in (before e): its end, its UTF-8 into out
 * (*o advanced, room n); NULL if it isn't one */
static const char *ew(const char *in, const char *e, char *out, size_t *o, size_t n)
{
	const char *cs = in + 2, *x, *t, *te;
	char label[40];
	unsigned char raw[400];
	size_t k;
	char *u;

	for (x = cs; x < e && *x != '?' && *x != ' '; x++)
		;
	if (x + 2 >= e || x[0] != '?' || x[2] != '?' || (size_t)(x - cs) >= sizeof label)
		return NULL;
	t = x + 3;
	for (te = t; te + 1 < e && !(te[0] == '?' && te[1] == '='); te++)
		if (*te == ' ')
			return NULL;
	if (te + 1 >= e || (size_t)(te - t) > sizeof raw)
		return NULL;
	memcpy(label, cs, (size_t)(x - cs));
	label[x - cs] = '\0';
	if (strchr(label, '*'))
		*strchr(label, '*') = '\0';	/* (RFC 2231's language) */
	if (x[1] == 'B' || x[1] == 'b')
		k = b64_decode(t, (size_t)(te - t), raw);
	else if (x[1] == 'Q' || x[1] == 'q')
		k = qp_decode(t, (size_t)(te - t), raw, 1);
	else
		return NULL;
	if ((u = mime_utf8(label, raw, k)) != NULL) {
		size_t ul = strlen(u);

		if (*o + ul >= n)
			ul = n - *o - 1;
		memcpy(out + *o, u, ul);
		*o += ul;
		xfree(u);
	}
	return te + 2;
}

void mime_header(const char *in, size_t n, char *out, size_t outn)
{
	const char *p = in, *e = in + n, *q;
	size_t o = 0;
	int after_word = 0;

	if (outn == 0)
		return;
	while (p < e && o + 1 < outn) {
		if (p + 1 < e && p[0] == '=' && p[1] == '?' && (q = ew(p, e, out, &o, outn)) != NULL) {
			p = q;
			after_word = 1;
			continue;
		}
		if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			const char *s = p;

			while (s < e && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
				s++;
			/* (between two encoded words, space is nothing) */
			if (!(after_word && s + 1 < e && s[0] == '=' && s[1] == '?')
				&& o > 0 && out[o - 1] != ' ')
				out[o++] = ' ';
			p = s;
			continue;
		}
		out[o++] = *p++;
		after_word = 0;
	}
	out[o] = '\0';
}

void mime_mutf7(const char *in, char *out, size_t n)
{
	size_t o = 0;

	if (n == 0)
		return;
	while (*in && o + 5 < n) {
		const char *e;
		unsigned char raw[256];
		size_t k, i;
		unsigned long hi = 0;

		if (*in != '&') {
			out[o++] = *in++;
			continue;
		}
		if (in[1] == '-') {
			out[o++] = '&';
			in += 2;
			continue;
		}
		for (e = in + 1; *e && *e != '-'; e++)
			;
		k = (size_t)(e - in - 1);
		if (k > 300)
			k = 300;
		k = b64_run(in + 1, k, raw, 1);
		/* UTF-16BE, surrogate pairs joined */
		for (i = 0; i + 1 < k && o + 5 < n; i += 2) {
			unsigned long u = (unsigned long)raw[i] << 8 | raw[i + 1];

			if (u >= 0xD800 && u < 0xDC00) {
				hi = u;
				continue;
			}
			if (u >= 0xDC00 && u < 0xE000 && hi) {
				u = 0x10000 + ((hi - 0xD800) << 10) + (u - 0xDC00);
				hi = 0;
			}
			o += (size_t)utf8_put(out + o, u);
		}
		in = *e ? e + 1 : e;
	}
	out[o] = '\0';
}

/* --- parts ---------------------------------------------------------------- */

static void lower_copy(char *dst, size_t n, const struct ival *v)
{
	size_t i;

	iv_str(v, dst, n);
	for (i = 0; dst[i]; i++)
		if (dst[i] >= 'A' && dst[i] <= 'Z')
			dst[i] = (char)(dst[i] + 32);
}

/* the value of parameter name in list v ("charset" "utf-8" "name" "x") */
static int param(struct imap *m, struct ival *v, const char *name, char *out, size_t n)
{
	struct ival *k;
	char key[40];

	for (k = iv_at(m, v, 0); k && iv_next(m, k); k = iv_next(m, iv_next(m, k))) {
		lower_copy(key, sizeof key, k);
		if (strcmp(key, name) == 0) {
			char raw[256];

			iv_str(iv_next(m, k), raw, sizeof raw);
			mime_header(raw, strlen(raw), out, n);
			return 1;
		}
	}
	return 0;
}

static int parts_of(struct imap *m, struct ival *b, const char *sec, int top,
	struct mpart *out, int n, int max)
{
	struct ival *f = iv_at(m, b, 0), *d;
	struct mpart *p;
	unsigned long i;

	if (f == NULL || n >= max)
		return n;
	if (f->type == IV_LIST) {
		/* multipart: its parts (lists), then its subtype */
		struct ival *k;

		for (k = f, i = 1; k && k->type == IV_LIST && n < max; k = iv_next(m, k), i++) {
			char s[24];

			if (top)
				sprintf(s, "%lu", i);
			else if (strlen(sec) + 12 < sizeof s)
				sprintf(s, "%s.%lu", sec, i);
			else
				break;
			n = parts_of(m, k, s, 0, out, n, max);
		}
		return n;
	}
	p = &out[n];
	memset(p, 0, sizeof *p);
	strcpy(p->section, top ? "1" : sec);
	lower_copy(p->type, sizeof p->type, f);
	lower_copy(p->subtype, sizeof p->subtype, iv_at(m, b, 1));
	param(m, iv_at(m, b, 2), "charset", p->charset, sizeof p->charset);
	param(m, iv_at(m, b, 2), "name", p->name, sizeof p->name);
	lower_copy(p->encoding, sizeof p->encoding, iv_at(m, b, 5));
	p->size = iv_num(iv_at(m, b, 6));
	/* the disposition: after the lines (text), the envelope, body and
	 * lines (message/rfc822), else the md5 */
	d = iv_at(m, b, strcmp(p->type, "text") == 0 ? 9
		: strcmp(p->type, "message") == 0 && strcmp(p->subtype, "rfc822") == 0 ? 11 : 8);
	if (d && d->type == IV_LIST) {
		char dt[24];

		lower_copy(dt, sizeof dt, iv_at(m, d, 0));
		if (strcmp(dt, "attachment") == 0)
			p->attachment = 1;
		param(m, iv_at(m, d, 1), "filename", p->name, sizeof p->name);
	}
	if (strcmp(p->type, "text") != 0)
		p->attachment = 1;
	return n + 1;
}

int mime_parts(struct imap *m, struct ival *bs, struct mpart *out, int max)
{
	return parts_of(m, bs, "", 1, out, 0, max);
}

int mime_pick(const struct mpart *p, int n, int html)
{
	const char *want[2];
	int k, i;

	want[0] = html ? "html" : "plain";
	want[1] = html ? "plain" : "html";
	for (k = 0; k < 2; k++)
		for (i = 0; i < n; i++)
			if (!p[i].attachment && strcmp(p[i].type, "text") == 0
				&& strcmp(p[i].subtype, want[k]) == 0)
				return i;
	return -1;
}

unsigned char *mime_decode(const struct mpart *p, const char *s, size_t n, size_t *len)
{
	unsigned char *out = xmalloc(n + 4);

	if (out == NULL)
		return NULL;
	if (strcmp(p->encoding, "base64") == 0)
		*len = b64_decode(s, n, out);
	else if (strcmp(p->encoding, "quoted-printable") == 0)
		*len = qp_decode(s, n, out, 0);
	else {
		memcpy(out, s, n);
		*len = n;
	}
	out[*len] = '\0';
	return out;
}
