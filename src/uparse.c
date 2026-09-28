/*
 * uparse - parse an HTML document with the browser's engine: the Phase 2
 * test tool.
 *
 *   uparse [-d] [-l] [-t] [-s] [-c N] [-m cap_kb] [-C charset] FILE|URL|-
 *
 *   -d   dump the tree            -l   list the links (a href)
 *   -t   the text, one line per block-ish run
 *   -s   statistics (the default when nothing else is asked)
 *   -c   feed the parser N bytes at a time (default: as read)
 *   -m   cap for the document's memory, in KB (default 1200)
 *   -C   the charset, as an HTTP header would give it
 *   -w   lay the page out N columns wide and print it (-u: for a UTF-8
 *        terminal, -a: ASCII; default Latin-1); -r repeats the layout
 *        for timing
 * A URL is fetched with the network core, parsed as the bytes arrive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "os.h"
#include "tags.h"
#include "doc.h"
#include "load.h"
#include "entropy.h"
#include "tls.h"
#include "conn.h"
#include "fetch.h"
#include "layout.h"

static struct html_load g_load;
static struct doc g_doc;
static size_t g_chunk;
static int g_started;
static const char *g_charset;
static unsigned long g_parse_ms;

static void feed(const unsigned char *s, size_t n)
{
	unsigned long t0 = os_msec();

	while (n) {
		size_t k = g_chunk && g_chunk < n ? g_chunk : n;

		html_load_feed(&g_load, s, k);
		s += k;
		n -= k;
	}
	g_parse_ms += os_msec() - t0;
}

/* fetch callbacks: start the parse when the head says what it is */
static void on_head(void *ctx, int status, const char *ctype,
	const char *charset, const char *url)
{
	(void)ctx;
	(void)status;
	(void)url;
	if (!g_started) {
		html_load_begin(&g_load, &g_doc, g_charset ? g_charset : charset,
			strcmp(ctype, "text/plain") == 0);
		g_started = 1;
	}
}

static int on_body(void *ctx, const unsigned char *d, size_t n)
{
	(void)ctx;
	if (!g_started)
		on_head(ctx, 200, "text/html", "", "");
	feed(d, n);
	return 0;
}

static void on_reset(void *ctx)
{
	(void)ctx;
	/* the fetch starts over: so does the document */
	doc_free(&g_doc);
	doc_init(&g_doc, g_doc.byte_cap);
	g_started = 0;
}

static int count_links(const struct doc *d)
{
	unsigned long i;
	int n = 0;

	for (i = 2; i < d->nnodes; i++)
		if (d->nodes[i].type == NODE_ELEM && d->nodes[i].tag == TAG_A
			&& doc_attr(d, (nodeid)i, ATTR_HREF))
			n++;
	return n;
}

static void list_links(const struct doc *d)
{
	unsigned long i;

	for (i = 2; i < d->nnodes; i++)
		if (d->nodes[i].type == NODE_ELEM && d->nodes[i].tag == TAG_A) {
			const char *h = doc_attr(d, (nodeid)i, ATTR_HREF);

			if (h)
				printf("%s\n", h);
		}
}

/* text, with a line break at block-level boundaries */
static void text_out(const struct doc *d, nodeid id, int *col)
{
	for (; id; id = d->nodes[id].next) {
		const struct node *n = &d->nodes[id];

		if (n->type == NODE_TEXT) {
			const char *s = doc_text(d, id);

			fputs(s, stdout);
			*col += (int)strlen(s);
			continue;
		}
		if (n->tag == TAG_HEAD)
			continue;
		if (tag_flags(n->tag) & (TF_BLOCK | TF_SPECIAL)) {
			if (*col) {
				putchar('\n');
				*col = 0;
			}
		}
		text_out(d, n->first, col);
		if ((tag_flags(n->tag) & (TF_BLOCK | TF_SPECIAL)) && *col) {
			putchar('\n');
			*col = 0;
		}
	}
}

static void usage(void)
{
	fprintf(stderr, "usage: uparse [-d] [-l] [-t] [-s] [-c N] [-m cap_kb] "
		"[-C charset] [-w width [-u|-a] [-r N]] FILE|URL|-\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int dump = 0, links = 0, text = 0, stats = 0, a, width = 0, reps = 1;
	enum term_cs tcs = TCS_LATIN1;
	size_t cap = 0;
	const char *src;
	unsigned long t0;
	static const char *const csfrom[] = { "default", "http", "bom", "meta", "guess" };

	for (a = 1; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
		if (strcmp(argv[a], "-d") == 0) dump = 1;
		else if (strcmp(argv[a], "-l") == 0) links = 1;
		else if (strcmp(argv[a], "-t") == 0) text = 1;
		else if (strcmp(argv[a], "-s") == 0) stats = 1;
		else if (strcmp(argv[a], "-c") == 0 && a + 1 < argc)
			g_chunk = (size_t)atol(argv[++a]);
		else if (strcmp(argv[a], "-m") == 0 && a + 1 < argc)
			cap = (size_t)atol(argv[++a]) * 1024;
		else if (strcmp(argv[a], "-C") == 0 && a + 1 < argc)
			g_charset = argv[++a];
		else if (strcmp(argv[a], "-w") == 0 && a + 1 < argc)
			width = atoi(argv[++a]);
		else if (strcmp(argv[a], "-r") == 0 && a + 1 < argc)
			reps = atoi(argv[++a]);
		else if (strcmp(argv[a], "-u") == 0) tcs = TCS_UTF8;
		else if (strcmp(argv[a], "-a") == 0) tcs = TCS_ASCII;
		else
			usage();
	}
	if (a != argc - 1)
		usage();
	src = argv[a];
	if (!dump && !links && !text && !width)
		stats = 1;
	if (doc_init(&g_doc, cap) < 0) {
		fprintf(stderr, "uparse: out of memory\n");
		return 1;
	}
	t0 = os_msec();

	if (strstr(src, "://")) {
		struct fetch_cb cb;
		static struct fetch_result res;
		char seed[600];

		signal(SIGPIPE, SIG_IGN);
		entropy_init(os_datapath(seed, sizeof seed, "seed"));
		tls_init(getenv("UB_CAFILE"), NULL);
		memset(&cb, 0, sizeof cb);
		cb.head = on_head;
		cb.body = on_body;
		cb.reset = on_reset;
		if (fetch(src, "GET", &cb, &res) < 0) {
			fprintf(stderr, "uparse: %s: %s\n", src, res.error);
			return 1;
		}
		conn_close_all();
		entropy_save();
	} else {
		static unsigned char buf[4096];
		FILE *f = strcmp(src, "-") == 0 ? stdin : fopen(src, "rb");
		size_t n;

		if (f == NULL) {
			perror(src);
			return 1;
		}
		html_load_begin(&g_load, &g_doc, g_charset, 0);
		g_started = 1;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0)
			feed(buf, n);
		if (f != stdin)
			fclose(f);
	}
	if (!g_started)
		html_load_begin(&g_load, &g_doc, g_charset, 0);
	{
		unsigned long t1 = os_msec();

		html_load_end(&g_load);
		g_parse_ms += os_msec() - t1;
	}

	if (dump)
		doc_dump(&g_doc, stdout);
	if (links)
		list_links(&g_doc);
	if (text) {
		int col = 0;

		text_out(&g_doc, g_doc.nodes[1].first, &col);
		if (col)
			putchar('\n');
	}
	if (width) {
		static struct page pg;
		unsigned long t1 = os_msec();
		int i;

		for (i = 0; i < reps; i++) {
			if (i)
				layout_free(&pg);
			if (layout_run(&pg, &g_doc, width, tcs, 0, 0, NULL) < 0) {
				fprintf(stderr, "uparse: layout: out of memory\n");
				return 1;
			}
		}
		t1 = os_msec() - t1;
		if (reps == 1)
			layout_print(&pg, stdout);
		if (stats || reps > 1)
			printf("layout %lu lines  %lu spans  %lu links  %lu KB%s  "
				"%lu ms each\n", pg.nlines, pg.nspans, pg.nlinks,
				(pg.text_cap + pg.lines_cap * sizeof(struct lline)
				+ pg.spans_cap * sizeof(struct lspan)) / 1024,
				pg.truncated ? " TRUNCATED" : "",
				t1 / (unsigned long)(reps > 0 ? reps : 1));
		layout_free(&pg);
	}
	if (stats) {
		printf("bytes %lu  charset %s (%s)  nodes %lu  text %lu  attr %lu  "
			"doc %lu KB%s\n", g_load.bytes_in,
			g_load.cs == CS_UTF8 ? "utf-8" : "windows-1252",
			csfrom[g_load.cs_from], g_doc.nnodes, g_doc.text_len,
			g_doc.attr_len, (unsigned long)doc_bytes(&g_doc) / 1024,
			g_doc.truncated ? "  TRUNCATED" : "");
		printf("title \"%s\"  links %d  parse %lu ms  total %lu ms  "
			"heap peak %lu KB\n", doc_title(&g_doc), count_links(&g_doc),
			g_parse_ms, os_msec() - t0, (unsigned long)mem_peak() / 1024);
	}
	doc_free(&g_doc);
	return 0;
}
