/*
 * gophermap.c - see gophermap.h.
 */
#include <stdio.h>
#include <string.h>
#include "gophermap.h"

static void out(struct gophermap *g, const char *s)
{
	g->out(g->ctx, s, strlen(s));
}

/* text, with < & > escaped */
static void out_text(struct gophermap *g, const char *s, size_t n)
{
	size_t i, k = 0;

	for (i = 0; i < n; i++) {
		const char *e = s[i] == '<' ? "&lt;" : s[i] == '>' ? "&gt;"
			: s[i] == '&' ? "&amp;" : s[i] == '"' ? "&quot;" : NULL;

		if (e) {
			if (i > k)
				g->out(g->ctx, s + k, i - k);
			out(g, e);
			k = i + 1;
		}
	}
	if (n > k)
		g->out(g->ctx, s + k, n - k);
}

/* a selector, percent-encoded for a URL path */
static void out_selector(struct gophermap *g, const char *s, size_t n)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c <= ' ' || c >= 0x7F || c == '%' || c == '?' || c == '#'
			|| c == '"' || c == '<' || c == '>') {
			char b[4];

			b[0] = '%';
			b[1] = hex[c >> 4];
			b[2] = hex[c & 15];
			b[3] = '\0';
			out(g, b);
		} else
			g->out(g->ctx, s + i, 1);
	}
}

static const char *tag_for(char type)
{
	switch (type) {
	case '0': return "(TXT) ";
	case '1': return "(DIR) ";
	case '7': return "(?) ";
	case 'h': return "(HTML) ";
	case 'g': case 'I': case 'p': return "(IMG) ";
	case '8': case 'T': return "(TEL) ";
	case 's': return "(SND) ";
	default: return "(BIN) ";
	}
}

static void one_line(struct gophermap *g, char *l, size_t n)
{
	char *f[4];
	size_t len[4];
	int nf = 0;
	char type, *p = l, *e = l + n;

	if (n && l[n - 1] == '\r')
		n--, e--;
	if (n == 1 && l[0] == '.') {
		g->done = 1;
		return;
	}
	if (n == 0) {
		out(g, "\n");
		return;
	}
	type = *p++;
	/* display TAB selector TAB host TAB port [TAB +] */
	while (nf < 4) {
		char *t = memchr(p, '\t', (size_t)(e - p));

		f[nf] = p;
		len[nf] = (size_t)((t ? t : e) - p);
		nf++;
		if (t == NULL)
			break;
		p = t + 1;
	}
	if (type == 'i' || type == '3' || nf < 3) {
		out_text(g, f[0], len[0]);
		out(g, "\n");
		return;
	}
	out(g, tag_for(type));
	out(g, "<a href=\"");
	if (type == 'h' && len[1] > 4 && strncmp(f[1], "URL:", 4) == 0)
		out_text(g, f[1] + 4, len[1] - 4);
	else if (type == '8' || type == 'T') {
		out(g, "telnet://");
		out_text(g, f[2], len[2]);
		if (nf > 3 && !(len[3] == 2 && strncmp(f[3], "23", 2) == 0)) {
			out(g, ":");
			out_text(g, f[3], len[3]);
		}
	} else {
		char t[2];

		out(g, "gopher://");
		out_text(g, f[2], len[2]);
		if (nf > 3 && len[3] && !(len[3] == 2
			&& strncmp(f[3], "70", 2) == 0)) {
			out(g, ":");
			out_text(g, f[3], len[3]);
		}
		t[0] = '/';
		t[1] = type;
		g->out(g->ctx, t, 2);
		out_selector(g, f[1], len[1]);
	}
	out(g, "\">");
	out_text(g, f[0], len[0]);
	out(g, "</a>\n");
}

void gophermap_begin(struct gophermap *g,
	void (*o)(void *ctx, const char *html, size_t n), void *ctx)
{
	memset(g, 0, sizeof *g);
	g->out = o;
	g->ctx = ctx;
	out(g, "<pre>");
}

void gophermap_feed(struct gophermap *g, const unsigned char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n && !g->done; i++) {
		if (s[i] == '\n') {
			one_line(g, g->line, g->n);
			g->n = 0;
		} else if (g->n < sizeof g->line - 1)
			g->line[g->n++] = (char)s[i];
	}
}

void gophermap_end(struct gophermap *g)
{
	if (g->n && !g->done)
		one_line(g, g->line, g->n);
	out(g, "</pre>");
}
