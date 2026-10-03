/*
 * fuzz_html - mutation fuzzing of the HTML engine, for a sanitizer build
 * (make fuzz): pages from the corpus are mutated (byte flips, markup
 * fragments spliced in, cuts, pages spliced together) and parsed twice,
 * whole and in random chunks. Both must finish without a sanitizer
 * report, and give the same tree.
 *
 *   fuzz_html ITERATIONS SEED FILE...
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "doc.h"
#include "load.h"
#include "layout.h"
#include "forms.h"

static int fake_width(void *ctx, int attr, int face, const char *s, int n);
static int fake_height(void *ctx, int attr, int face, int *ascent);

static unsigned long s_rng;

static unsigned long rnd(unsigned long n)
{
	s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL;
	return n ? (unsigned long)((s_rng >> 33) % n) : 0;
}

static const char *const frags[] = {
	"<", ">", "</", "<!--", "-->", "--!>", "<!", "<?", "&", "&amp", "&#",
	"&#x", "&#1234567890;", "&notin", ";", "\"", "'", "=", "/", "<script>",
	"</script>", "<style>", "</style", "<svg>", "</svg>", "<math>",
	"<template>", "<table>", "<td>", "<tr>", "</table>", "<p>", "</p>",
	"<li>", "<pre>\n", "<textarea>", "</textarea>", "<plaintext>", "<title>",
	"<a href=", "<a href='x'>", "</a>", "<select><option>", "<br/>", "\r\n",
	"\xC3", "\xE2\x82", "\xF0\x9F\x98", "\xFF", "\x00", "<html>", "<body>",
	"</body></html>", "<head>", "<div", "<xmp>", "</xmp>", "<noscript>",
	"<iframe>", "</iframe>", "<!DOCTYPE html>", "<![CDATA[", "]]>",
	"<img src=x width=50 height=20>", "<img src=y alt=why>", "<img width=100%>",
	"<a href=z><img src=q height=3000></a>", "<img src=w width=9999>",
	"<style>", "</style>", ".a{display:none}", "#b .c>p{visibility:hidden}",
	"@media (max-width:", "40em){", "@media print{", "}}", "{", "}", "/*", "*/",
	"\\", "\\31 0", "[title~=x]", "!important", "@font-face{", "a,b,c{", ";",
	"<td colspan=3>", "<td rowspan=9>", "<th>", "<caption>", "<thead>",
	"<td colspan=40 rowspan=0>",
};

/* the fake font's images: some of known size (by node), some not */
static int fake_image(void *ctx, nodeid node, int *w, int *h)
{
	(void)ctx;
	if (node % 3 == 0)
		return 0;
	*w = (int)(node * 37 % 900) + (node % 5 == 0 ? 0 : 1);
	*h = (int)(node * 11 % 500) + 1;
	return 1;
}

static char *dump_of(const unsigned char *s, size_t n, int chunked, size_t cap)
{
	static struct html_load l;
	struct doc d;
	char *out = NULL;
	size_t len = 0, off = 0;
	FILE *f;

	if (doc_init(&d, cap) < 0)
		return strdup("oom");
	html_load_begin(&l, &d, rnd(3) == 0 ? "windows-1252" : NULL, rnd(20) == 0);
	while (off < n) {
		size_t k = chunked ? 1 + rnd(97) : n - off;

		if (k > n - off)
			k = n - off;
		html_load_feed(&l, s + off, k);
		off += k;
	}
	html_load_end(&l);
	f = open_memstream(&out, &len);
	doc_dump(&d, f);
	fclose(f);
	if (!chunked) {
		/* and lay it out: any width, charset, cap, line limit; half
		 * the time in a made-up proportional font (pixels) */
		static const enum term_cs cs[] = { TCS_ASCII, TCS_LATIN1, TCS_UTF8 };
		static const struct lmetrics fake = { fake_width, fake_height, NULL, 7,
			fake_image, NULL };
		const struct lmetrics *m = rnd(2) ? &fake : NULL;
		struct page pg;
		int lw = m ? 1 + (int)rnd(2000) : 1 + (int)rnd(200);
		enum term_cs lcs = cs[rnd(3)];
		unsigned long lmax = rnd(4) == 0 ? 1 + rnd(50) : 0;
		size_t lcap = rnd(4) == 0 ? 1 + rnd(20000) : 0;

		if (layout_run_m(&pg, &d, lw, lcs, lmax, lcap, NULL, m) == 0) {
			unsigned long i;

			/* with metrics, every line has a height */
			if (m && pg.nlines && pg.heights == NULL)
				abort();
			for (i = 0; m && i < pg.nlines; i++)
				if (pg.heights[i].height == 0
					|| pg.heights[i].ascent > pg.heights[i].height)
					abort();

			/* every line and span within the text */
			for (i = 0; i < pg.nlines; i++)
				if (pg.lines[i].off + pg.lines[i].len > pg.text_len)
					abort();
			for (i = 1; i < pg.nspans; i++)
				if (pg.spans[i].off < pg.spans[i - 1].off
					|| pg.spans[i].link > pg.nlinks)
					abort();
			/* images: only with metrics, whole, each a real one */
			for (i = 0; i < pg.nspans; i++) {
				unsigned long e = i + 1 < pg.nspans ? pg.spans[i + 1].off : pg.text_len, o;

				if (!(pg.spans[i].face & LF_IMAGE))
					continue;
				if (!m || (e - pg.spans[i].off) % LAYOUT_IMG_BYTES)
					abort();
				for (o = pg.spans[i].off; o < e; o += LAYOUT_IMG_BYTES)
					if (layout_image(&pg, pg.text + o) < 0
						&& layout_spacer_w(pg.text + o) < 0)
						abort();
			}
			/* each on a line (or the one still open when a line
			 * limit stopped the layout) as tall as it */
			for (i = 0; i < pg.nimages; i++) {
				unsigned long ln = pg.images[i].line;

				if (pg.images[i].w == 0 || pg.images[i].h == 0
					|| ln > pg.nlines
					|| (ln < pg.nlines && pg.heights[ln].ascent < pg.images[i].h)) {
					FILE *ff = fopen("fuzz-fail.html", "wb");

					fprintf(stderr, "image %lu: %ux%u on line %lu of %lu (ascent %d);"
						" width %d cs %d max_lines %lu cap %lu: fuzz-fail.html\n",
						i, pg.images[i].w, pg.images[i].h, ln, pg.nlines,
						ln < pg.nlines ? pg.heights[ln].ascent : -1,
						lw, (int)lcs, lmax, (unsigned long)lcap);
					if (ff) {
						fwrite(s, 1, n, ff);
						fclose(ff);
					}
					abort();
				}
			}
			layout_free(&pg);
		}
		/* the forms: collect, lay out with them, change, submit */
		{
			struct forms fs;
			struct url base;
			int i;

			url_parse("http://fuzz.test/a/b?c", &base);
			if (forms_init(&fs, &d) == 0) {
				for (i = 0; i < fs.n; i++) {
					struct field *f = &fs.f[i];
					nodeid o[4];
					int no = forms_options(&fs, f, o, 4);

					if (rnd(2))
						forms_click(&fs, f);
					if (f->type == FT_TEXT && rnd(2))
						forms_set_text(f, "fu\xc3\x9fz & =");
					if (no && rnd(2))
						forms_choose(f, o[rnd(no)]);
					(void)forms_text(&fs, f);
				}
				if (fs.n) {
					struct submission sub;
					const char *why;
					struct field *f = &fs.f[rnd(fs.n)];

					if (forms_submit(&fs, f->form, f->node, &base,
						(int)rnd(2), &sub, &why) == 0)
						xfree(sub.body);
					forms_reset(&fs, f->form);
				}
				if (layout_run(&pg, &d, 40, TCS_UTF8, 0, 0, &fs) == 0)
					layout_free(&pg);
				forms_free(&fs);
			}
		}
	}
	doc_free(&d);
	return out;
}

/* a made-up proportional font: widths by byte, bold wider, headings
 * bigger, monospace even */
static int fake_width(void *ctx, int attr, int face, const char *s, int n)
{
	int w = 0, i;

	(void)ctx;
	for (i = 0; i < n; i++)
		w += (face & LF_MONO) ? 7 : 4 + ((unsigned char)s[i] % 6);
	if (attr & SA_BOLD)
		w += n;
	if (face & LF_HMASK)
		w += w / (face & LF_HMASK);
	return w;
}

static int fake_height(void *ctx, int attr, int face, int *ascent)
{
	static const int h[4] = { 14, 22, 18, 15 };

	(void)ctx;
	(void)attr;
	*ascent = h[face & LF_HMASK] - 3;
	return h[face & LF_HMASK];
}

int main(int argc, char **argv)
{
	unsigned char *pages[128];
	size_t lens[128];
	int npages = 0, i, j;
	long iters, it;

	if (argc < 4) {
		fprintf(stderr, "usage: fuzz_html ITERATIONS SEED FILE...\n");
		return 2;
	}
	iters = atol(argv[1]);
	s_rng = strtoul(argv[2], 0, 10);
	for (i = 3; i < argc && npages < 128; i++) {
		FILE *f = fopen(argv[i], "rb");
		long n;

		if (!f)
			continue;
		fseek(f, 0, SEEK_END);
		n = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (n > 200000)
			n = 200000;	/* keep iterations quick */
		pages[npages] = malloc((size_t)n + 1);
		lens[npages] = fread(pages[npages], 1, (size_t)n, f);
		fclose(f);
		npages++;
	}
	if (npages == 0)
		return 2;

	for (it = 0; it < iters; it++) {
		int p = (int)rnd((unsigned long)npages);
		size_t cap = lens[p] + 70000, n = lens[p], m;
		unsigned char *buf = malloc(cap);
		char *a, *b;
		size_t mut = 1 + rnd(40);
		size_t doc_cap = rnd(4) == 0 ? 4096 + rnd(60000) : 0;

		memcpy(buf, pages[p], n);
		/* sometimes only a slice of the page */
		if (rnd(3) == 0 && n > 2000) {
			size_t start = rnd(n - 1000), len = 1000 + rnd(n - start - 1000 + 1);

			memmove(buf, buf + start, len);
			n = len;
		}
		for (m = 0; m < mut && n < cap - 64; m++) {
			switch (rnd(5)) {
			case 0:		/* flip a byte */
				if (n)
					buf[rnd(n)] = (unsigned char)rnd(256);
				break;
			case 1: {	/* insert a fragment */
				const char *f = frags[rnd(sizeof frags / sizeof frags[0])];
				size_t fl = strlen(f) + (f[0] == '\0'), at = rnd(n + 1);

				memmove(buf + at + fl, buf + at, n - at);
				memcpy(buf + at, f, fl);
				n += fl;
				break;
			}
			case 2:		/* cut */
				if (n > 10) {
					size_t at = rnd(n), len = rnd(n - at) / 4;

					memmove(buf + at, buf + at + len, n - at - len);
					n -= len;
				}
				break;
			case 3: {	/* splice in a piece of another page */
				int q = (int)rnd((unsigned long)npages);
				size_t len = lens[q] ? rnd(lens[q] < 400 ? lens[q] : 400) : 0,
					from = len < lens[q] ? rnd(lens[q] - len) : 0, at = rnd(n + 1);

				if (n + len >= cap)
					break;
				memmove(buf + at + len, buf + at, n - at);
				memcpy(buf + at, pages[q] + from, len);
				n += len;
				break;
			}
			case 4:		/* truncate */
				n = rnd(n + 1);
				break;
			}
		}
		/* the same randomness for the attributes of both parses */
		{
			unsigned long saved = s_rng;

			a = dump_of(buf, n, 0, doc_cap);
			s_rng = saved;
			b = dump_of(buf, n, 1, doc_cap);
		}
		if (strcmp(a, b) != 0) {
			char name[64];
			FILE *f;

			snprintf(name, sizeof name, "fuzz-fail-%ld.html", it);
			f = fopen(name, "wb");
			fwrite(buf, 1, n, f);
			fclose(f);
			printf("iteration %ld: chunked parse differs; input saved as %s\n",
				it, name);
			return 1;
		}
		free(a);
		free(b);
		free(buf);
		if (it % 1000 == 999) {
			printf("%ld iterations\n", it + 1);
			fflush(stdout);
		}
	}
	printf("fuzz: %ld iterations ok\n", iters);
	for (j = 0; j < npages; j++)
		free(pages[j]);
	return 0;
}
