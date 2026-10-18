/*
 * test_frames - a page of frames made one page (html/frames.c): frames'
 * documents from a table here, links made absolute and aimed at their
 * frame, style sheets gathered, nested framesets, a chosen document.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "doc.h"
#include "load.h"
#include "frames.h"

static int fails;

static const struct {
	const char *url, *html;
} pages[] = {
	{ "http://h/d/menu.html",
		"<html><head><title>Menu</title>"
		"<link rel=stylesheet href=../s.css></head>"
		"<body bgcolor=#ffffff><a href=a.html target=page>A</a> "
		"<a href=/top.html target=_top>Top</a> <a href=self.html>Self</a>"
		"<img src=pics/x.gif srcset=\"pics/y.gif 2x\" alt=X></body>" },
	{ "http://h/d/a.html",
		"<style>p { color: red }</style><p>Page A &amp; &lt;b&gt;" },
	{ "http://h/d/b.html", "<p>Page B" },
	{ "http://h/d/inner.html",
		"<frameset cols=\"30%,*\"><frame src=b.html name=left>"
		"<frame name=right src=a.html></frameset>" },
};

static int get(void *ctx, const char *url, struct frame_bytes *out)
{
	size_t i;

	(void)ctx;
	for (i = 0; i < sizeof pages / sizeof pages[0]; i++)
		if (strcmp(url, pages[i].url) == 0) {
			out->len = strlen(pages[i].html);
			out->data = xmalloc(out->len);
			memcpy(out->data, pages[i].html, out->len);
			snprintf(out->url, sizeof out->url, "%s", url);
			return 0;
		}
	snprintf(out->error, sizeof out->error, "HTTP 404");
	return -1;
}

/* the frameset page html, from http://h/d/index.html, made one page */
static char *compose(const char *html, const char *chosen, char **state)
{
	static struct html_load l;
	static struct url base;
	struct doc d;
	char *out;

	doc_init(&d, 0);
	html_load_begin(&l, &d, "utf-8", 0);
	html_load_feed(&l, (const unsigned char *)html, strlen(html));
	html_load_end(&l);
	url_parse("http://h/d/index.html", &base);
	if (!l.tree.frameset) {
		printf("FAIL no frameset in %s\n", html);
		fails++;
		doc_free(&d);
		*state = NULL;
		return xstrdup("");
	}
	out = frames_compose(&d, l.tree.frameset, &base, chosen, get, NULL, state);
	doc_free(&d);
	return out;
}

static void has(const char *what, const char *s, const char *want)
{
	if (s == NULL || strstr(s, want) == NULL) {
		printf("FAIL %s: no\n  %s\nin\n  %s\n", what, want, s ? s : "(null)");
		fails++;
	}
}

static void hasnt(const char *what, const char *s, const char *bad)
{
	if (s && strstr(s, bad)) {
		printf("FAIL %s: has\n  %s\nin\n  %s\n", what, bad, s);
		fails++;
	}
}

int main(void)
{
	static const char sgi[] = "<html><frameset rows=\"100,*\" border=0>"
		"<frame src=menu.html name=menu scrolling=no>"
		"<frame src=b.html name=page></frameset></html>";
	char *out, *state, *ch;

	out = compose(sgi, "", &state);
	has("title from a frame", out, "<title>Menu</title>");
	has("sheet link absolute", out,
		"<link rel=\"stylesheet\" href=\"http://h/s.css\">");
	has("frame body colour", out, "<td valign=\"top\" bgcolor=\"#ffffff\">");
	has("targeted link", out,
		"<a href=\"http://h/d/a.html\" target=\"page\">A</a>");
	has("_top link", out, "<a href=\"http://h/top.html\" target=\"_top\">");
	has("link aimed at its frame", out,
		"<a href=\"http://h/d/self.html\" target=\"menu\">");
	has("image absolute, srcset gone", out,
		"<img src=\"http://h/d/pics/x.gif\" alt=\"X\">");
	has("second frame", out, "<p>Page B</p>");
	has("rows stacked", out, "</td></tr><tr><td");
	has("state", state, "menu\thttp://h/d/menu.html\npage\thttp://h/d/b.html\n");
	xfree(out);

	/* a link to frame page: the same page with a.html in it */
	ch = frames_choose(state, "page", "http://h/d/a.html");
	xfree(state);
	out = compose(sgi, ch, &state);
	has("chosen document", out, "<p>Page A &amp; &lt;b&gt;</p>");
	hasnt("not the old one", out, "Page B");
	has("its style sheet", out, "<style>p { color: red }</style>");
	has("chosen state", state, "page\thttp://h/d/a.html\n");
	if (!frames_has(state, "menu") || frames_has(state, "men")
		|| frames_has(state, "nothing")) {
		printf("FAIL frames_has\n");
		fails++;
	}
	xfree(ch);
	xfree(out);
	xfree(state);

	/* a frame that is frames, side by side; and one that can't be had */
	out = compose("<frameset rows=\"*,*\"><frame src=inner.html>"
		"<frame src=gone.html name=g></frameset>", "", &state);
	has("columns", out, "<tr><td valign=\"top\" width=\"30%\"><p>Page B</p>"
		"</td><td valign=\"top\"><p>Page A");
	has("unnamed frame", state, "manx-frame-1\thttp://h/d/inner.html\n");
	has("missing frame", out, "[frame g: HTTP 404] <a href=\"http://h/d/gone.html\">");
	xfree(out);
	xfree(state);

	if (fails) {
		printf("frames: %d FAILED\n", fails);
		return 1;
	}
	printf("frames: ok\n");
	return 0;
}
