/*
 * bench_parse FILE [N] - where parsing time goes, stage by stage, on
 * whatever machine it runs on: decoding only; decoding + tokenizing into
 * an empty sink; the whole parse. N rounds each (default 1).
 */
#include <stdio.h>
#include <stdlib.h>
#include "os.h"
#include "utf8.h"
#include "tokenizer.h"
#include "doc.h"
#include "load.h"

static unsigned long s_tags, s_text;

static void null_tag(void *ctx, const struct tok_tag *t)
{
	(void)ctx;
	(void)t;
	s_tags++;
}

static void null_text(void *ctx, const char *s, size_t n)
{
	(void)ctx;
	(void)s;
	s_text += n;
}

int main(int argc, char **argv)
{
	static struct html_load l;
	static struct tokenizer tok;
	static char out[3 * 4096 + 8];
	struct tok_sink sink = { 0, null_tag, null_text };
	struct decoder dec;
	size_t len, off;
	unsigned char *s = argc > 1 ? os_read_file(argv[1], &len) : NULL;
	int i, n = argc > 2 ? atoi(argv[2]) : 1;
	unsigned long t0, t_dec, t_tok, t_all;

	if (s == NULL)
		return 2;
	t0 = os_msec();
	for (i = 0; i < n; i++) {
		decoder_init(&dec, CS_UTF8);
		for (off = 0; off < len; off += 4096)
			decoder_run(&dec, s + off, len - off < 4096 ? len - off : 4096, out);
	}
	t_dec = os_msec() - t0;

	t0 = os_msec();
	for (i = 0; i < n; i++) {
		decoder_init(&dec, CS_UTF8);
		tok_init(&tok, &sink);
		for (off = 0; off < len; off += 4096) {
			size_t o = decoder_run(&dec, s + off,
				len - off < 4096 ? len - off : 4096, out);

			tok_feed(&tok, out, o);
		}
		tok_end(&tok);
	}
	t_tok = os_msec() - t0;

	t0 = os_msec();
	for (i = 0; i < n; i++) {
		struct doc d;

		doc_init(&d, 0);
		html_load_begin(&l, &d, "utf-8", 0);
		html_load_feed(&l, s, len);
		html_load_end(&l);
		doc_free(&d);
	}
	t_all = os_msec() - t0;
	printf("%lu bytes x %d: decode %lu ms, +tokenize %lu ms, +tree %lu ms "
		"(%lu tags, %lu text bytes per round)\n", (unsigned long)len, n,
		t_dec, t_tok, t_all, s_tags / n, s_text / n);
	return 0;
}
