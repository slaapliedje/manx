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

/*
 * The page laid out at 40 columns with each run's look before it, as
 * [b u #rrggbb] (bold, underline, colour; [] when plain), lines joined
 * with "|"; an indent as spaces.
 */
static void looks(const char *html, char *out, size_t cap)
{
	static struct html_load l;
	struct doc d;
	struct page pg;
	unsigned long ln, i;
	size_t o = 0;

	doc_init(&d, 0);
	html_load_begin(&l, &d, "utf-8", 0);
	html_load_feed(&l, (const unsigned char *)html, strlen(html));
	html_load_end(&l);
	layout_run(&pg, &d, 40, TCS_ASCII, 0, 0, NULL);
	out[0] = '\0';
	for (ln = 0; ln < pg.nlines && o + 64 < cap; ln++) {
		const struct lline *li = &pg.lines[ln];
		unsigned long s = li->span, prev = (unsigned long)-1;

		for (i = 0; i < li->indent && o + 1 < cap; i++)
			out[o++] = ' ';
		for (i = li->off; i < li->off + li->len && o + 32 < cap; i++) {
			s = layout_span_at(&pg, s, i);
			if (s != prev) {
				const struct lspan *sp = &pg.spans[s];

				o += (size_t)sprintf(out + o, "[%s%s", sp->attr & SA_BOLD ? "b" : "",
					sp->attr & SA_UNDER ? "u" : "");
				if (sp->color)
					o += (size_t)sprintf(out + o, "#%06lx", pg.palette[sp->color - 1]);
				out[o++] = ']';
				prev = s;
			}
			out[o++] = pg.text[i];
		}
		out[o++] = '|';
		out[o] = '\0';
	}
	layout_free(&pg);
	doc_free(&d);
}

/* a sheet that arrives after the page, as a linked one does */
static void look_late(const char *name, const char *css, const char *body, const char *want)
{
	static struct html_load l;
	char page[2048], got[4096];
	struct doc d;
	struct page pg;
	unsigned long ln, i;
	size_t o = 0;

	snprintf(page, sizeof page, "<html><head><link rel=stylesheet href=x.css></head>"
		"<body>%s</body></html>", body);
	doc_init(&d, 0);
	html_load_begin(&l, &d, "utf-8", 0);
	html_load_feed(&l, (const unsigned char *)page, strlen(page));
	html_load_end(&l);
	layout_run(&pg, &d, 40, TCS_ASCII, 0, 0, NULL);	/* (before the sheet) */
	layout_free(&pg);
	if (d.sheet == NULL)
		d.sheet = css_new();
	css_begin(d.sheet, NULL, 3);
	css_feed(d.sheet, css, strlen(css));
	css_end(d.sheet);
	layout_run(&pg, &d, 40, TCS_ASCII, 0, 0, NULL);
	got[0] = '\0';
	for (ln = 0; ln < pg.nlines && o + 64 < sizeof got; ln++) {
		const struct lline *li = &pg.lines[ln];
		unsigned long sp = li->span, prev = (unsigned long)-1;

		for (i = li->off; i < li->off + li->len && o + 32 < sizeof got; i++) {
			sp = layout_span_at(&pg, sp, i);
			if (sp != prev) {
				o += (size_t)sprintf(got + o, "[%s", pg.spans[sp].attr & SA_BOLD ? "b" : "");
				if (pg.spans[sp].color)
					o += (size_t)sprintf(got + o, "#%06lx", pg.palette[pg.spans[sp].color - 1]);
				got[o++] = ']';
				prev = sp;
			}
			got[o++] = pg.text[i];
		}
		got[o++] = '|';
		got[o] = '\0';
	}
	layout_free(&pg);
	doc_free(&d);
	runs++;
	if (strcmp(got, want) != 0) {
		fails++;
		printf("FAIL %s\n  want %s\n  got  %s\n", name, want, got);
	}
}

static void look(const char *name, const char *css, const char *body, const char *want)
{
	char page[2048], got[4096];

	snprintf(page, sizeof page, "<html><head><style>%s</style></head><body>%s</body></html>",
		css, body);
	looks(page, got, sizeof got);
	runs++;
	if (strcmp(got, want) != 0) {
		fails++;
		printf("FAIL %s\n  want %s\n  got  %s\n", name, want, got);
	}
}

int main(void)
{
	/* the look of text */
	look("color by class", ".r{color:red}", "<p class=r>a</p><p>b</p>", "[#ff0000]a||[]b|");
	look("inherited", "div{color:#00f}", "<div><p>a <b>b</b></p></div>",
		"[#0000ff]a [b#0000ff]b|");
	look("short hex, rgb()", "p{color:#f80} i{color:rgb(0, 128, 255)}",
		"<p>a<i>b</i></p>", "[#ff8800]a[u#0080ff]b|");
	look("rgba over white", "p{color:rgba(0,0,0,.5)}", "<p>a</p>", "[#7f7f7f]a|");
	look("space syntax", "p{color:rgb(255 0 0 / 50%)}", "<p>a</p>", "[#ff7f7f]a|");
	look("hsl", "p{color:hsl(120, 100%, 25%)}", "<p>a</p>", "[#007f00]a|");
	look("var", ":root{--c:#123456} p{color:var(--c)}", "<p>a</p>", "[#123456]a|");
	look("var set later", "p{color:var(--c)} html{--c:#654321}", "<p>a</p>", "[#654321]a|");
	look("var fallback", "p{color:var(--none, green)}", "<p>a</p>", "[#008000]a|");
	look("var of a var", ":root{--a:#abcdef;--b:var(--a)} p{color:var(--b)}",
		"<p>a</p>", "[#abcdef]a|");
	look("light-dark", "p{color:light-dark(#111, #eee)}", "<p>a</p>", "[#111111]a|");
	look("transparent ignored", "p{color:transparent}", "<p>a</p>", "[]a|");
	look("initial resets", "div{color:red} p{color:initial}", "<div><p>a</p>b</div>",
		"[]a||[#ff0000]b|");
	look("inline color", "", "<p style='color: #0f0'>a</p>", "[#00ff00]a|");
	look("important", "p.x{color:red} p{color:blue !important}", "<p class=x>a</p>",
		"[#0000ff]a|");
	look("specificity", "p{color:blue} .x{color:red}", "<p class=x>a</p>", "[#ff0000]a|");
	look(":link", "a:link{color:green}", "<p><a href=x>l</a> <a name=n>m</a></p>",
		"[#008000]l[] m|");
	look(":root", ":root{color:#333}", "<p>a</p>", "[#333333]a|");
	look("body color", "body{color:#444}", "<p>a</p>", "[#444444]a|");
	look("media not met", "@media (max-width:100px){p{color:red}}", "<p>a</p>", "[]a|");
	look("weight normal", "h2{font-weight:normal}", "<h2>a</h2>", "[]a|");
	look("weight 700", "span{font-weight:700}", "<p><span>a</span>b</p>", "[b]a[]b|");
	look("font shorthand", "p{font:italic bold 12px/1.5 sans-serif}", "<p>a</p>", "[bu]a|");
	look("shorthand resets", "b{font:12px serif}", "<p><b>a</b></p>", "[]a|");
	look("no underline", "a{text-decoration:none}", "<p><a href=x>l</a></p>", "[]l|");
	look("underline", ".u{text-decoration:underline}", "<p class=u>a</p>", "[u]a|");
	look("centre", "p{text-align:center}", "<p>abcd</p>",
		"                  []abcd|");
	look("right", "", "<p align=right>abcd</p>",
		"                                    []abcd|");
	look("center tag", "", "<center>ab</center>", "                   []ab|");
	look("sheet over align=", "p{text-align:left}", "<p align=center>ab</p>", "[]ab|");
	look("centred block, left after", "div{text-align:center}",
		"<div>ab</div><p>cd</p>", "                   []ab||[]cd|");
	look("inline align ignored", "span{text-align:center}", "<p><span>ab</span></p>",
		"[]ab|");
	look("pseudo-classes still dropped", "a:hover{color:red} p::before{color:red}",
		"<p><a href=x>l</a></p>", "[]l|");

	look_late("a sheet after the page", "td{color:#828282} a:link{color:#000000}",
		"<table><tr><td><a href=x>Title</a></td></tr><tr><td>63 points</td></tr></table>",
		"[#000000]Title|[#828282]63 points|");
	look("a table in <center> starts left", "",
		"<center><table><tr><td>row</td></tr></table></center>", "[]row|");
	look("a table's align= isn't its text's", "",
		"<table align=center><tr><td>row</td></tr></table>", "[]row|");
	look("table cells' colours (Hacker News)",
		"body{color:#828282} td{font-family:Verdana; color:#828282}"
		" a:link{color:#000000; text-decoration:none}",
		"<table><tr><td class=title><a href=x>Title</a></td></tr>"
		"<tr><td class=subtext>63 points</td></tr></table>",
		"[#000000]Title|[#828282]63 points|");

	/* what hides */
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
	/* list-style: markers or none, inherited */
	check("list-style none", "ul{list-style:none}", "<ul><li>a<li>b</ul>", "    a|    b|");
	check("list-style-type none on ol", ".n{list-style-type:none}",
		"<ol class=n><li>a<li>b</ol><ol><li>c</ol>", "    a|    b|| 1. c|");
	check("list-style inherited", ".a{list-style:none}",
		"<ul class=a><li>x<ul><li>y</ul></ul>", "    x|        y|");
	check("list-style back", ".a{list-style:none} ul ul{list-style:square}",
		"<ul class=a><li>x<ul><li>y</ul></ul>", "    x|      + y|");
	check("list-style on an item", "li.k{list-style:none}",
		"<ul><li class=k>a<li>b</ul>", "    a|  * b|");
	check("list-style inline", "", "<ul style='list-style: none inside'><li>a</ul>"
		"<ul style='list-style:square'><li>b</ul>", "    a||  * b|");
	printf("css: %d/%d passed\n", runs - fails, runs);
	return fails ? 1 : 0;
}
