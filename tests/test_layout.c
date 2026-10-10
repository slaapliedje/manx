/*
 * test_layout - HTML snippets laid out at a given width, compared with the
 * text expected.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "doc.h"
#include "load.h"
#include "layout.h"

static int fails, runs;

static char *render(const char *html, int width, enum term_cs cs,
	struct page *pg, struct doc *d)
{
	static struct html_load l;
	static char out[8192];
	FILE *f;
	size_t n;

	doc_init(d, 0);
	html_load_begin(&l, d, "utf-8", 0);
	html_load_feed(&l, (const unsigned char *)html, strlen(html));
	html_load_end(&l);
	layout_run(pg, d, width, cs, 0, 0, NULL);
	f = tmpfile();
	layout_print(pg, f);
	rewind(f);
	n = fread(out, 1, sizeof out - 1, f);
	out[n] = '\0';
	fclose(f);
	return out;
}

static void check(const char *name, const char *html, int width,
	enum term_cs cs, const char *want)
{
	struct page pg;
	struct doc d;
	const char *got = render(html, width, cs, &pg, &d);

	runs++;
	if (strcmp(got, want) != 0) {
		fails++;
		printf("FAIL %s\n--- want\n%s--- got\n%s---\n", name, want, got);
	}
	layout_free(&pg);
	doc_free(&d);
}

/* line n of the page (0: the first) is want */
static void check_line(const char *name, const char *html, int width, int n,
	const char *want)
{
	struct page pg;
	struct doc d;
	const char *got = render(html, width, TCS_ASCII, &pg, &d), *e;

	runs++;
	while (n-- > 0 && got && (got = strchr(got, '\n')) != NULL)
		got++;
	e = got ? strchr(got, '\n') : NULL;
	if (got == NULL || e == NULL || (size_t)(e - got) != strlen(want)
		|| strncmp(got, want, strlen(want)) != 0) {
		fails++;
		printf("FAIL %s\n--- want\n%s\n--- got\n%.*s\n---\n", name, want,
			got && e ? (int)(e - got) : 0, got ? got : "");
	}
	layout_free(&pg);
	doc_free(&d);
}

/* a table framing a page: a column of 21 links beside some 1300 bytes of
 * text */
static char *frame_page(int nested)
{
	static char b[4096];
	char *p = b;
	int i;

	p += sprintf(p, "<table><tr><td>");
	if (nested)
		p += sprintf(p, "<table><tr><td>a<td>b</table>");
	for (i = 0; i < 21; i++)
		p += sprintf(p, "link %d<br>", i);
	p += sprintf(p, "<td>");
	for (i = 0; i < 180; i++)
		p += sprintf(p, "word%d ", i);
	sprintf(p, "</table>");
	return b;
}

/* the links' first lines and the anchors */
static void check_links(void)
{
	struct page pg;
	struct doc d;

	render("<p>one <a href=a>two</a><p id=x>three <a href=b>four "
		"five six seven</a>", 12, TCS_ASCII, &pg, &d);
	runs++;
	if (pg.nlinks != 2 || pg.links[0].line != 0 || pg.links[1].line != 2
		|| layout_anchor(&pg, "x") != 2 || layout_anchor(&pg, "y") != -1) {
		fails++;
		printf("FAIL links: %lu links, lines %lu %lu, anchor %ld\n",
			pg.nlinks, pg.nlinks > 0 ? pg.links[0].line : 99,
			pg.nlinks > 1 ? pg.links[1].line : 99,
			layout_anchor(&pg, "x"));
	}
	layout_free(&pg);
	doc_free(&d);
}

/* tables as grids: links and anchors in cells, where they show */
static void check_grid_links(void)
{
	struct page pg;
	struct doc d;

	render("<table><tr><td><a href=a>one</a><td><a href=b>two</a></tr>"
		"<tr><td>x<td id=k><a href=c>three</a></table>", 30, TCS_ASCII, &pg, &d);
	runs++;
	if (pg.nlinks != 3 || pg.links[0].line != 0 || pg.links[1].line != 0
		|| pg.links[2].line != 1 || pg.links[0].col != 0 || pg.links[1].col != 5
		|| pg.links[2].col != 5 || layout_anchor(&pg, "k") != 1) {
		fails++;
		printf("FAIL grid links: %lu links, lines %lu %lu %lu, cols %u %u %u, anchor %ld\n",
			pg.nlinks, pg.nlinks > 0 ? pg.links[0].line : 99,
			pg.nlinks > 1 ? pg.links[1].line : 99, pg.nlinks > 2 ? pg.links[2].line : 99,
			pg.nlinks > 0 ? pg.links[0].col : 99, pg.nlinks > 1 ? pg.links[1].col : 99,
			pg.nlinks > 2 ? pg.links[2].col : 99, layout_anchor(&pg, "k"));
	}
	layout_free(&pg);
	doc_free(&d);
}

/* a made-up proportional font: 10 pixels a byte, lines 12 high */
static int fake_w(void *ctx, int attr, int face, const char *s, int n)
{
	(void)ctx;
	(void)attr;
	(void)face;
	(void)s;
	return n * 10;
}

static int fake_h(void *ctx, int attr, int face, int *ascent)
{
	(void)ctx;
	(void)attr;
	(void)face;
	*ascent = 9;
	return 12;
}

/* with proportional fonts the columns line up to the pixel: spacers */
static void check_grid_pixels(void)
{
	static struct html_load l;
	static const struct lmetrics m = { fake_w, fake_h, NULL, 10, NULL, NULL };
	const char *html = "<table><tr><td>a<td>b</tr><tr><td>abc<td>d</table>";
	struct page pg;
	struct doc d;
	unsigned long ln;
	int bad = 0;

	doc_init(&d, 0);
	html_load_begin(&l, &d, "utf-8", 0);
	html_load_feed(&l, (const unsigned char *)html, strlen(html));
	html_load_end(&l);
	layout_run_m(&pg, &d, 300, TCS_UTF8, 0, 0, NULL, &m);
	/* each line: a cell's text, a gap to 40 px, the next cell's: 50 px */
	for (ln = 0; ln < pg.nlines; ln++) {
		const struct lline *li = &pg.lines[ln];
		unsigned long off = li->off, end = off + li->len, sp = li->span;
		int w = li->indent;

		while (off < end) {
			unsigned long next = end;

			sp = layout_span_at(&pg, sp, off);
			if (sp + 1 < pg.nspans && pg.spans[sp + 1].off < end)
				next = pg.spans[sp + 1].off;
			w += pg.spans[sp].face & LF_IMAGE ?
				layout_images_w(&pg, pg.text + off, (int)(next - off))
				: (int)(next - off) * 10;
			off = next;
		}
		if (w != 50)
			bad++;
	}
	runs++;
	if (pg.nlines != 2 || bad) {
		fails++;
		printf("FAIL grid pixels: %lu lines, %d not 50 px wide\n", pg.nlines, bad);
	}
	layout_free(&pg);
	doc_free(&d);
}

int main(void)
{
	/* foreign elements close themselves: what follows is the page's */
	check("svg/ and math/", "<p>before</p><svg/><p>after</p><math/><p>end</p>",
		40, TCS_ASCII, "before\n\nafter\n\nend\n");
	/* style="" as CSS reads it: whole property names, the later
	 * declaration wins (unless the earlier is !important), any case */
	check("style= display", "<p style=\"--x-display:none\">a</p>"
		"<p style=\"display:none;display:block\">b</p>"
		"<p style=\"display:none !important;display:block\">c</p>"
		"<p style=\"DISPLAY : NONE\">d</p><p>e</p>",
		40, TCS_ASCII, "a\n\nb\n\ne\n");
	check("wrap", "<p>The quick brown fox jumps over the lazy dog.", 16,
		TCS_ASCII, "The quick brown\nfox jumps over\nthe lazy dog.\n");
	check("word across elements", "<p>aaaa bbbb c<b>cc</b>c dd", 12,
		TCS_ASCII, "aaaa bbbb\ncccc dd\n");
	check("long word", "<p>abcdefghijklmnopqrstuvwxyz", 10, TCS_ASCII,
		"abcdefghij\nklmnopqrst\nuvwxyz\n");
	check("margins collapse", "<p>a</p><p>b</p><div><p>c</p></div>", 20,
		TCS_ASCII, "a\n\nb\n\nc\n");
	check("headings", "<h1>Title</h1><p>text", 20, TCS_ASCII,
		"Title\n\ntext\n");
	check("br", "<p>a<br>b<br><br>c", 20, TCS_ASCII, "a\nb\n\nc\n");
	check("ul nested", "<ul><li>one<li>two<ul><li>x</ul></ul>", 20,
		TCS_ASCII, "  * one\n  * two\n      + x\n");
	check("ol start type", "<ol start=3><li>c<li>d</ol><ol type=i>"
		"<li>i<li>ii</ol>", 20, TCS_ASCII,
		" 3. c\n 4. d\n\n i. i\nii. ii\n");
	check("item wraps under its text", "<ul><li>one two three four</ul>",
		12, TCS_ASCII, "  * one two\n    three\n    four\n");
	check("pre", "<pre>\n a  b\n\n\tc</pre>", 20, TCS_ASCII,
		" a  b\n\n        c\n");
	check("blockquote", "<blockquote>quoted text here</blockquote>", 14,
		TCS_ASCII, "    quoted\n    text here\n");
	check("hr", "<p>a<hr>b", 10, TCS_ASCII, "a\n\n----------\nb\n");
	check("table row one line", "<table><tr><td>1.<td><center>"
		"<a href=v><div></div></a></center><td>Title</table>", 30,
		TCS_ASCII, "1.  [v]  Title\n");
	check("cell with paragraphs", "<table><tr><td><p>one</p><p>two</p>"
		"</table>", 30, TCS_ASCII, "one\n\ntwo\n");
	check("hidden", "<p>a<span hidden>b</span><span style='display:"
		" none'>c</span>d", 20, TCS_ASCII, "ad\n");
	check("react streamed", "<div hidden id=S:0><p>shown</p></div>", 20,
		TCS_ASCII, "shown\n");
	check("form fields", "<form><input name=q size=6> <input type=submit"
		" value=Go> <input type=checkbox checked> <select><option>A"
		"<option selected>B</select></form>", 40, TCS_ASCII,
		"[______] [Go] [x] [B v]\n");
	check("img alt and empty links", "<p><img alt=Logo> <a href="
		"'http://host/'></a> <a href='/x/page.html'></a>", 40,
		TCS_ASCII, "[Logo] [host][page.html]\n");
	/* (no space between the two: the parser saw two empty links and
	 * nothing between them worth a space) */
	check("translit ascii", "<p>&ldquo;caf&eacute;&rdquo; &mdash; "
		"&hellip;", 40, TCS_ASCII, "\"cafe\" -- ...\n");
	check("latin1", "<p>caf&eacute;", 40, TCS_LATIN1, "caf\351\n");
	/* (10 columns is the narrowest layout) */
	check("utf8 wide", "<p>\xe4\xb8\xad\xe6\x96\x87\xe4\xb8\xad"
		"\xe6\x96\x87 abc", 10, TCS_UTF8, "\xe4\xb8\xad\xe6\x96\x87"
		"\xe4\xb8\xad\xe6\x96\x87\nabc\n");
	check("nbsp keeps words", "<p>aaaa bbbb&nbsp;cc", 10, TCS_ASCII,
		"aaaa\nbbbb cc\n");
	/* a rule in a table cell laid out as rows breaks the row's line;
	 * what was before it stays (it was lost: the rule's line was opened
	 * over it) */
	check("hr in a cell", "<table><tr><td>before<hr>after<td>nextnextnextnex"
		"</table>", 20, TCS_ASCII,
		"before\n--------------------\nafter\nnextnextnextnex\n");
	/* in a grid's cell the rule is as wide as the cell (and no wider:
	 * it doesn't keep the cells from sitting side by side) */
	check("hr in a grid cell", "<table><tr><td>before<hr>after<td>next</table>",
		20, TCS_ASCII, "before  next\n------\nafter\n");
	/* a table framing a page: side by side where there is room for it,
	 * as rows where there isn't (FRAME_MIN_EM) */
	check_line("frame wide", frame_page(0), 100, 0, "link 0   word0 word1 word2 "
		"word3 word4 word5 word6 word7 word8 word9 word10 word11 word12 word13");
	check_line("frame narrow", frame_page(0), 80, 0, "link 0");
	check_line("frame with a table in it", frame_page(1), 100, 0, "a  b     word0 "
		"word1 word2 word3 word4 word5 word6 word7 word8 word9 word10 word11 word12 "
		"word13");
	/* a grid in a centred block is centred as a block, not a line at a
	 * time */
	check("centred grid", "<center><table><tr><td>a<td>bb</tr><tr><td>ccc<td>d"
		"</table></center>", 30, TCS_ASCII, "           a    bb\n           ccc  d\n");
	check_links();
	/* tables of data as grids; those framing a page as rows */
	check("grid", "<table><tr><th>Name<th>Size</tr><tr><td>apple<td>12</tr>"
		"<tr><td>fig<td>3</tr></table>", 30, TCS_ASCII,
		"Name   Size\napple  12\nfig    3\n");
	check("grid wraps in its column", "<table><tr><td>key<td>a value that "
		"wraps here</table>", 20, TCS_ASCII, "key  a value that\n     wraps here\n");
	check("grid spans", "<table><tr><td colspan=2>wide<td>z</tr><tr><td>a<td>b"
		"<td>c</table>", 30, TCS_ASCII, "wide  z\na  b  c\n");
	check("grid rowspan", "<table><tr><td rowspan=2>two rows<td>a</tr><tr><td>b"
		"</table>", 30, TCS_ASCII, "two rows  a\n          b\n");
	check("grid caption", "<table><caption>Cap</caption><tr><td>a<td>b</table>"
		"<p>after", 30, TCS_ASCII, "Cap\na  b\n\nafter\n");
	check("one column: rows", "<table><tr><td>one</tr><tr><td>two</table>", 30,
		TCS_ASCII, "one\ntwo\n");
	check("a table holding a table: rows, the inner one a grid",
		"<table><tr><td>a<td><table><tr><td>x<td>y</table></table>", 30,
		TCS_ASCII, "a\n\nx  y\n");
	check("a cell framing a page: rows", "<table><tr><td>side<td><p>p1<p>p2<p>p3"
		"<p>p4<p>p5<p>p6<p>p7<p>p8<p>p9<p>p10<p>p11</table>", 30, TCS_ASCII,
		"side  p1\n\np2\n\np3\n\np4\n\np5\n\np6\n\np7\n\np8\n\np9\n\np10\n\np11\n");
	check("too wide for a grid: rows", "<table><tr><td>abcdefghijklmnop"
		"<td>qrstuvwxyz0123456</table>", 20, TCS_ASCII,
		"abcdefghijklmnop\nqrstuvwxyz0123456\n");
	check_grid_links();
	check_grid_pixels();
	/* nor the space after it */
	{
		struct page pg;
		struct doc d;
		unsigned long i;
		int bad = 0;

		render("<p>by <a href=u>someone</a> today <b>bold</b> end", 40,
			TCS_ASCII, &pg, &d);
		runs++;
		for (i = 0; i < pg.nspans; i++) {
			unsigned long o = pg.spans[i].off;

			if ((pg.spans[i].link || pg.spans[i].attr)
				&& o < pg.text_len && pg.text[o] == ' ')
				bad = 1;
			if (!pg.spans[i].link && !pg.spans[i].attr && o > 0
				&& pg.text[o] != ' ' && o < pg.text_len
				&& pg.text[o - 1] == ' ' && i > 0
				&& (pg.spans[i - 1].link || pg.spans[i - 1].attr))
				bad = 1;
		}
		if (bad) {
			fails++;
			printf("FAIL a space is part of a link or bold run\n");
		}
		layout_free(&pg);
		doc_free(&d);
	}
	/* where reading starts */
	{
		struct page pg;
		struct doc d;

		render("<ul><li>menu<li>menu</ul><main><p>Toggle<ul><li>x<li>y"
			"</ul><h1>Title</h1><p>short<p>A first paragraph that is "
			"long enough to be the start of the article itself, surely."
			"</main>", 80, TCS_ASCII, &pg, &d);
		runs++;
		if (pg.content_line < 0 || strncmp(pg.text
			+ pg.lines[pg.content_line].off, "Title", 5) != 0) {
			fails++;
			printf("FAIL content line %ld\n", pg.content_line);
		}
		layout_free(&pg);
		doc_free(&d);
	}
	/* the space before a link isn't part of it */
	{
		struct page pg;
		struct doc d;
		unsigned long i, sp = 0;

		render("<p>by <a href=u>someone</a> today", 40, TCS_ASCII, &pg, &d);
		runs++;
		for (i = 0; i < pg.nspans; i++)
			if (pg.spans[i].link)
				sp = pg.spans[i].off;
		if (sp != 3 || pg.text[sp] != 's') {
			fails++;
			printf("FAIL link span starts at %lu ('%c')\n", sp, pg.text[sp]);
		}
		layout_free(&pg);
		doc_free(&d);
	}
	printf("layout: %d/%d passed\n", runs - fails, runs);
	return fails != 0;
}
