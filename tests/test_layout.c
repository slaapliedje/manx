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

int main(void)
{
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
	check_links();
	printf("layout: %d/%d passed\n", runs - fails, runs);
	return fails != 0;
}
