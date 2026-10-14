/*
 * compose.c - a message to send, put together (compose.h).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "os.h"
#include "mime.h"
#include "smtp.h"
#include "compose.h"

void sb_put(struct sbuf *b, const char *s, size_t n)
{
	if (b->oom)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t c = b->cap ? b->cap * 2 : 1024;
		char *q;

		while (c < b->len + n + 1)
			c *= 2;
		if ((q = xrealloc(b->s, c)) == NULL) {
			b->oom = 1;
			return;
		}
		b->s = q;
		b->cap = c;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

void sb_str(struct sbuf *b, const char *s)
{
	sb_put(b, s, strlen(s));
}

/* --- addresses ------------------------------------------------------------ */

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return p;
}

/* s..e, its spaces trimmed and its quotes taken off, into out (n) */
static void unquote(const char *s, const char *e, char *out, size_t n)
{
	size_t o = 0;

	s = skip_ws(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		e--;
	for (; s < e && o + 1 < n; s++) {
		if (*s == '"')
			continue;
		if (*s == '\\' && s + 1 < e)
			s++;
		out[o++] = (unsigned char)*s < ' ' ? ' ' : *s;
	}
	out[o] = '\0';
}

int addr_parse(const char *list, struct addr *out, int max, char *bad, size_t badn)
{
	const char *p = list;
	int n = 0;

	if (bad && badn)
		bad[0] = '\0';
	while (*p) {
		const char *s = p, *lt = NULL, *gt = NULL;
		int q = 0, ang = 0;

		/* one: up to a comma outside quotes and <> */
		for (; *p; p++) {
			if (*p == '\\' && q && p[1]) {
				p++;
				continue;
			}
			if (*p == '"')
				q = !q;
			else if (!q && *p == '<' && !ang) {
				ang = 1;
				lt = p;
			} else if (!q && *p == '>' && ang) {
				ang = 0;
				gt = p;
			} else if (!q && !ang && *p == ',')
				break;
		}
		if (*skip_ws(s) != '\0' && skip_ws(s) != p) {
			struct addr a;

			memset(&a, 0, sizeof a);
			if (lt && gt && gt > lt) {
				unquote(s, lt, a.name, sizeof a.name);
				unquote(lt + 1, gt, a.mbox, sizeof a.mbox);
			} else
				unquote(s, p, a.mbox, sizeof a.mbox);
			if (!smtp_addr_ok(a.mbox)) {
				if (bad && badn)
					unquote(s, p, bad, badn);
				return -1;
			}
			if (n < max)
				out[n++] = a;
		}
		if (*p == ',')
			p++;
	}
	return n;
}

/* --- headers -------------------------------------------------------------- */

static int ascii(const char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s >= 127 || (unsigned char)*s < ' ')
			return 0;
	return 1;
}

/* u as RFC 2047 words (UTF-8, base64), each at most 75 characters, folded
 * onto lines of their own when the line would be long; *col: where the
 * line is */
static void put_words(struct sbuf *b, const char *u, size_t *col)
{
	size_t n = strlen(u);

	while (n > 0) {
		size_t k = n > 45 ? 45 : n;
		char enc[64];

		/* (not through a character) */
		while (k < n && k > 0 && ((unsigned char)u[k] & 0xC0) == 0x80)
			k--;
		if (k == 0)
			k = n > 45 ? 45 : n;
		b64_encode((const unsigned char *)u, k, enc);
		if (*col + 12 + strlen(enc) > 76 && *col > 1) {
			sb_str(b, "\r\n ");
			*col = 1;
		} else if (*col > 0 && b->len && b->s[b->len - 1] != ' ') {
			sb_str(b, " ");
			(*col)++;
		}
		sb_str(b, "=?UTF-8?B?");
		sb_str(b, enc);
		sb_str(b, "?=");
		*col += 12 + strlen(enc);
		u += k;
		n -= k;
	}
}

/* the longest run without a space */
static size_t longest_word(const char *s)
{
	size_t best = 0, n = 0;

	for (; *s; s++)
		if (*s == ' ') {
			n = 0;
		} else if (++n > best)
			best = n;
	return best;
}

/* "Field: value", folded at its spaces onto lines of 76 or so; the value
 * in words where it isn't ASCII (or has a word too long to fold) */
static void put_text_header(struct sbuf *b, const char *field, const char *v)
{
	size_t col = strlen(field) + 2;

	sb_str(b, field);
	sb_str(b, ":");
	if (ascii(v) && longest_word(v) < 900) {
		while (*v) {
			const char *w;

			while (*v == ' ')
				v++;
			if (*v == '\0')
				break;
			for (w = v; *w && *w != ' '; w++)
				;
			if (col + (size_t)(w - v) + 1 > 76 && col > strlen(field) + 2) {
				sb_str(b, "\r\n");
				col = 0;
			}
			sb_str(b, " ");
			sb_put(b, v, (size_t)(w - v));
			col += (size_t)(w - v) + 1;
			v = w;
		}
	} else {
		sb_str(b, " ");
		put_words(b, v, &col);
	}
	sb_str(b, "\r\n");
}

/* does a display name need quotes (RFC 5322's specials)? */
static int needs_quotes(const char *s)
{
	for (; *s; s++)
		if (strchr("()<>[]:;@\\,.\"", *s))
			return 1;
	return 0;
}

/* "Field: Name <a@b>, ..." from a list as typed; -1 if it isn't one */
static int put_addr_header(struct sbuf *b, const char *field, const char *list)
{
	struct addr a[64];
	int n = addr_parse(list, a, 64, NULL, 0), i;
	size_t col = strlen(field) + 2;

	if (n < 0)
		return -1;
	if (n == 0)
		return 0;
	sb_str(b, field);
	sb_str(b, ": ");
	for (i = 0; i < n; i++) {
		size_t w = strlen(a[i].name) + strlen(a[i].mbox) + 4;

		if (i > 0) {
			sb_str(b, ",");
			if (col + w > 76) {
				sb_str(b, "\r\n ");
				col = 1;
			} else {
				sb_str(b, " ");
				col += 2;
			}
		}
		if (a[i].name[0] && ascii(a[i].name)) {
			if (needs_quotes(a[i].name)) {
				const char *s;

				sb_str(b, "\"");
				for (s = a[i].name; *s; s++) {
					if (*s == '"' || *s == '\\')
						sb_str(b, "\\");
					sb_put(b, s, 1);
				}
				sb_str(b, "\"");
			} else
				sb_str(b, a[i].name);
			sb_str(b, " <");
		} else if (a[i].name[0]) {
			put_words(b, a[i].name, &col);
			sb_str(b, " <");
		}
		sb_str(b, a[i].mbox);
		if (a[i].name[0])
			sb_str(b, ">");
		col += w;
	}
	sb_str(b, "\r\n");
	return 0;
}

/* --- the text ------------------------------------------------------------- */

/* does the text need quoted-printable: 8-bit, control characters, or a
 * line longer than SMTP takes? */
static int needs_qp(const char *s)
{
	size_t col = 0;

	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '\n') {
			col = 0;
			continue;
		}
		if (c == '\r' && s[1] == '\n')
			continue;
		/* (a CR anywhere else would go out bare: =0D) */
		if (c >= 127 || (c < ' ' && c != '\t') || ++col > 900)
			return 1;
	}
	return 0;
}

static void put_qp(struct sbuf *b, const char *s)
{
	static const char hx[] = "0123456789ABCDEF";

	while (*s) {
		const char *e = strchr(s, '\n');
		size_t n = e ? (size_t)(e - s) : strlen(s), i, col = 0;

		if (n && s[n - 1] == '\r')
			n--;
		for (i = 0; i < n; i++) {
			unsigned char c = (unsigned char)s[i];
			char t[4];
			size_t k = 1;

			if (c >= 33 && c <= 126 && c != '=')
				t[0] = (char)c;
			else if ((c == ' ' || c == '\t') && i + 1 < n)
				t[0] = (char)c;
			else {
				t[0] = '=';
				t[1] = hx[c >> 4];
				t[2] = hx[c & 15];
				k = 3;
			}
			if (col + k > 75) {
				sb_str(b, "=\r\n");
				col = 0;
			}
			sb_put(b, t, k);
			col += k;
		}
		sb_str(b, "\r\n");
		s = e ? e + 1 : s + strlen(s);
	}
}

static void put_7bit(struct sbuf *b, const char *s)
{
	while (*s) {
		const char *e = strchr(s, '\n');
		size_t n = e ? (size_t)(e - s) : strlen(s);

		if (n && s[n - 1] == '\r')
			n--;
		sb_put(b, s, n);
		sb_str(b, "\r\n");
		s = e ? e + 1 : s + strlen(s);
	}
}

char *mail_build(const struct outmsg *o, size_t *len)
{
	struct sbuf b;
	int qp = needs_qp(o->body);

	memset(&b, 0, sizeof b);
	sb_str(&b, "Date: ");
	sb_str(&b, o->date);
	sb_str(&b, "\r\n");
	if (put_addr_header(&b, "From", o->from) < 0 || put_addr_header(&b, "To", o->to) < 0
		|| (o->cc && put_addr_header(&b, "Cc", o->cc) < 0)) {
		xfree(b.s);
		return NULL;
	}
	put_text_header(&b, "Subject", o->subject);
	sb_str(&b, "Message-ID: ");
	sb_str(&b, o->msgid);
	sb_str(&b, "\r\n");
	if (o->in_reply_to && *o->in_reply_to)
		put_text_header(&b, "In-Reply-To", o->in_reply_to);
	if (o->references && *o->references)
		put_text_header(&b, "References", o->references);
	sb_str(&b, "MIME-Version: 1.0\r\n");
	sb_str(&b, qp ? "Content-Type: text/plain; charset=utf-8\r\n"
		"Content-Transfer-Encoding: quoted-printable\r\n"
		: "Content-Type: text/plain; charset=us-ascii\r\n"
		"Content-Transfer-Encoding: 7bit\r\n");
	sb_str(&b, "User-Agent: manxmail\r\n\r\n");
	if (qp)
		put_qp(&b, o->body);
	else
		put_7bit(&b, o->body);
	if (b.oom) {
		xfree(b.s);
		return NULL;
	}
	*len = b.len;
	return b.s;
}

/* days since 1970-01-01 of a date (month 1-12) */
static long days(long y, int m, int d)
{
	long era, yoe, doy;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

void mail_date(long t, char *out)
{
	static const char wd[] = "SunMonTueWedThuFriSat", mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	time_t tt = (time_t)t;
	struct tm l, g, *p;
	long off;

	if ((p = localtime(&tt)) == NULL) {
		strcpy(out, "Thu, 01 Jan 1970 00:00:00 +0000");
		return;
	}
	l = *p;
	g = (p = gmtime(&tt)) != NULL ? *p : l;
	/* (local time less UTC, in minutes) */
	off = ((days(l.tm_year + 1900L, l.tm_mon + 1, l.tm_mday)
		- days(g.tm_year + 1900L, g.tm_mon + 1, g.tm_mday)) * 1440
		+ (l.tm_hour - g.tm_hour) * 60 + (l.tm_min - g.tm_min));
	sprintf(out, "%.3s, %02d %.3s %d %02d:%02d:%02d %c%02ld%02ld", wd + 3 * l.tm_wday,
		l.tm_mday, mon + 3 * l.tm_mon, l.tm_year + 1900, l.tm_hour, l.tm_min, l.tm_sec,
		off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60);
}
