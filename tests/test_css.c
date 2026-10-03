/*
 * test_css - style sheets hiding elements: pages with <style> laid out at
 * 40 columns (so @media sees a window 320 pixels wide), and what shows.
 * Each page is also loaded a byte at a time: the same must show.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "doc.h"
#include "load.h"
#include "layout.h"
#include "css.h"

static int fails, runs;

static char *render(const char *html, size_t piece)
{
	static struct html_load l;
	static char out[4096];
	struct doc d;
	struct page pg;
	size_t off = 0, n = strlen(html), k;
	FILE *f;

	doc_init(&d, 0);
	html_load_begin(&l, &d, "utf-8", 0);
	while (off < n) {
		k = piece && piece < n - off ? piece : n - off;
		html_load_feed(&l, (const unsigned char *)html + off, k);
		off += k;
	}
	html_load_end(&l);
	layout_run(&pg, &d, 40, TCS_ASCII, 0, 0, NULL);
	f = tmpfile();
	layout_print(&pg, f);
	rewind(f);
	k = fread(out, 1, sizeof out - 1, f);
	out[k] = '\0';
	fclose(f);
	layout_free(&pg);
	doc_free(&d);
	return out;
}

/* the page's lines, joined with "|" */
static void check(const char *name, const char *css, const char *body, const char *want)
{
	char page[2048], got[4096], got1[4096];
	char *p;

	snprintf(page, sizeof page, "<html><head><style>%s</style></head><body>%s</body></html>",
		css, body);
	snprintf(got, sizeof got, "%s", render(page, 0));
	snprintf(got1, sizeof got1, "%s", render(page, 1));
	for (p = got; *p; p++)
		if (*p == '\n')
			*p = '|';
	for (p = got1; *p; p++)
		if (*p == '\n')
			*p = '|';
	runs++;
	if (strcmp(got, want) != 0) {
		fails++;
		printf("FAIL %s\n  want %s\n  got  %s\n", name, want, got);
	} else if (strcmp(got, got1) != 0) {
		fails++;
		printf("FAIL %s: a byte at a time\n  whole %s\n  bytes %s\n", name, got, got1);
	}
}

int main(void)
{
	check("class", ".x{display:none}", "<p>a</p><p class=x>b</p><p>c</p>", "a||c|");
	check("id", "#k { display : none }", "<p id=k>a</p><p id=j>b</p>", "b|");
	check("tag", "aside{display:none}", "<aside>a</aside><p>b</p>", "b|");
	check("several classes", ".x.y{display:none}",
		"<p class='x'>a</p><p class='y x'>b</p><p class='y'>c</p>", "a||c|");
	check("tag and class", "p.x{display:none}", "<div class=x>a</div><p class=x>b</p>", "a|");
	check("descendant", ".nav a{display:none}",
		"<div class=nav><p><a href=u>in</a> nav</p></div><p><a href=u>out</a></p>",
		"nav||out|");
	check("child", ".nav>p{display:none}",
		"<div class=nav><p>child</p><div><p>grandchild</p></div></div>", "grandchild|");
	check("selector list", "h1, .x , #y{display:none}",
		"<h1>a</h1><p class=x>b</p><p id=y>c</p><p>d</p>", "d|");
	check("later wins", ".x{display:none} .x{display:block}", "<p class=x>a</p>", "a|");
	check("later hides", ".x{display:block} .x{display:none}", "<p class=x>a</p>", "");
	check("specificity", "#a{display:block} .x{display:none}",
		"<p id=a class=x>a</p>", "a|");
	check("important", ".x{display:none !important} #a{display:block}",
		"<p id=a class=x>a</p>", "");
	check("inline beats it", ".x{display:none}", "<p class=x style='display:block'>a</p>",
		"a|");
	check("visibility", ".v{visibility:hidden}", "<p class=v>a</p><p>b</p>", "b|");
	check("display and visibility apart", ".x{display:none} .x.y{visibility:visible}",
		"<p class='x y'>a</p><p>b</p>", "b|");
	check("visually hidden: shown", ".sr-only{position:absolute;width:1px;height:1px;"
		"overflow:hidden;clip:rect(0,0,0,0)}", "<span class=sr-only>Posted</span> today",
		"Posted today|");
	check("many @media blocks", "@media (min-width:1px){.a{color:red}}"
		"@media (min-width:2px){.a{color:red}}@media (min-width:3px){.a{color:red}}"
		"@media (min-width:1px){.b{display:none}}@media (min-width:2px){.c{display:none}}",
		"<p class=b>a</p><p class=c>b</p><p>c</p>", "c|");
	check("pseudo-classes: not followed", ".x:hover{display:none} .y::before{display:none}",
		"<p class=x>a</p><p class=y>b</p>", "a||b|");
	check("siblings: not followed", ".a+.x{display:none} .a~.y{display:none}",
		"<p class=a>a</p><p class=x>b</p><p class=y>c</p>", "a||b||c|");
	check("unknown tags: not followed", "my-thing{display:none}",
		"<my-thing>a</my-thing> <other-thing>b</other-thing>", "a b|");
	check("attribute", "[title]{display:none} p[lang=fr]{display:none}",
		"<p title=t>a</p><p lang=fr>b</p><p lang=de>c</p>", "c|");
	check("attribute word", "[rel~=nofollow]{display:none}",
		"<p><a rel='external nofollow' href=u>a</a> b</p>", "b|");
	check("unknown attribute: not followed", "[data-x]{display:none} [x-cloak]{display:none}",
		"<p data-x=1>a</p>", "a|");
	check("escapes", ".md\\:hide{display:none} .\\31 0{display:none}",
		"<p class='md:hide'>a</p><p class='10'>b</p><p>c</p>", "c|");
	check("comments", "/* .y{display:none} */ .z{display:/* x */none}",
		"<p class=y>a</p><p class=z>b</p>", "a|");
	check("strings", ".s{content:\"}{\";display:none} .t{display:none}",
		"<p class=s>a</p><p class=t>b</p><p>c</p>", "c|");
	check("media width", "@media (max-width: 400px){.m{display:none}}"
		"@media (min-width: 740px){.d{display:none}}",
		"<p class=m>a</p><p class=d>b</p>", "b|");
	check("media rem and screen", "@media only screen and (max-width: 37.5rem){.m{display:none}}",
		"<p class=m>a</p><p>b</p>", "b|");
	check("media print", "@media print{.p{display:none}}", "<p class=p>a</p>", "a|");
	check("media not print", "@media not print{.p{display:none}}", "<p class=p>a</p><p>b</p>", "b|");
	check("media unknown", "@media (prefers-color-scheme: dark){.p{display:none}}",
		"<p class=p>a</p>", "a|");
	check("media nested", "@media screen{@media (min-width:1000px){.p{display:none}}"
		".q{display:none}}", "<p class=p>a</p><p class=q>b</p>", "a|");
	check("supports and layer", "@supports (display:grid){.p{display:none}}"
		"@layer base{.q{display:none}}", "<p class=p>a</p><p class=q>b</p><p>c</p>", "c|");
	check("blocks skipped", "@keyframes k{from{display:none}to{opacity:1}}"
		"@font-face{font-family:x;src:url(a;b)}.k{display:none}",
		"<p class=k>a</p><p>b</p>", "b|");
	check("import skipped", "@import url(x.css);@charset 'x';.k{display:none}",
		"<p class=k>a</p><p>b</p>", "b|");
	check("nested rule not followed", ".a{color:red; .b{display:none}}",
		"<p class=a>a</p><div class=a><p class=b>b</p></div>", "a||b|");
	check("never the body", "body{display:none} html{visibility:hidden}", "<p>a</p>", "a|");
	check("unclosed rule", ".k{display:none", "<p class=k>a</p><p>b</p>", "b|");
	check("style media print", "", "<style media=print>.p{display:none}</style><p class=p>a</p>",
		"a|");
	check("style in the body", "", "<p>a</p><style>.p{display:none}</style><p class=p>b</p>",
		"a|");
	check("no style: as before", "", "<p class=x>a</p><p hidden>b</p>", "a|");
	printf("css: %d/%d passed\n", runs - fails, runs);
	return fails ? 1 : 0;
}
