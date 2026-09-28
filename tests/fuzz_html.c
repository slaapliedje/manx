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
};

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
	doc_free(&d);
	return out;
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
