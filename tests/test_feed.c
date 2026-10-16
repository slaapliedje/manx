/*
 * test_feed - RSS and Atom feeds into HTML: what each kind of field
 * becomes, and the same HTML however the bytes are split.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "feed.h"

static int fails;
static char got[200000];
static size_t got_n;

static void sink(void *ctx, const char *s, size_t n)
{
	(void)ctx;
	if (got_n + n < sizeof got) {
		memcpy(got + got_n, s, n);
		got_n += n;
	}
	got[got_n] = '\0';
}

static char *convert(const char *xml, size_t step)
{
	static struct feed f;
	size_t len = strlen(xml), off = 0;

	got_n = 0;
	got[0] = '\0';
	feed_begin(&f, sink, NULL);
	while (off < len) {
		size_t n = step && len - off > step ? step : len - off;

		feed_feed(&f, (const unsigned char *)xml + off, n);
		off += n;
	}
	feed_end(&f);
	return got;
}

/* xml must give HTML containing each of want[] (NULL-ended), and lacking
 * each of not[], in pieces of any size */
static void check(const char *what, const char *xml, const char *const *want,
	const char *const *not)
{
	static const size_t steps[] = { 1, 2, 3, 7, 64 };
	static char whole[200000];
	size_t i;

	snprintf(whole, sizeof whole, "%s", convert(xml, 0));
	for (i = 0; want[i]; i++)
		if (!strstr(whole, want[i])) {
			printf("FAIL %s: no [%s] in\n%s\n", what, want[i], whole);
			fails++;
		}
	for (i = 0; not && not[i]; i++)
		if (strstr(whole, not[i])) {
			printf("FAIL %s: [%s] in\n%s\n", what, not[i], whole);
			fails++;
		}
	for (i = 0; i < sizeof steps / sizeof steps[0]; i++)
		if (strcmp(convert(xml, steps[i]), whole)) {
			printf("FAIL %s: in pieces of %lu:\n%s\nwhole:\n%s\n", what,
				(unsigned long)steps[i], got, whole);
			fails++;
		}
}

int main(void)
{
	{
		static const char rss[] =
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			"<!-- a comment > with -- dashes -->\n"
			"<!DOCTYPE rss [ <!ENTITY x \"y\"> ]>\n"
			"<rss version=\"2.0\" xmlns:content=\"http://purl.org/rss/1.0/modules/content/\""
			" xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
			" xmlns:media=\"http://search.yahoo.com/mrss/\">"
			"<channel><title>Tom &amp; Jerry&#8217;s</title>"
			"<link>https://example.com/</link>"
			"<atom:link href=\"https://example.com/feed\" rel=\"self\"/>"
			"<description>All the news</description>"
			"<item><title><![CDATA[First <post>]]></title>"
			"<link>https://example.com/1?a=1&amp;b=2</link>"
			"<description>&lt;p&gt;Short&lt;/p&gt;</description>"
			"<content:encoded><![CDATA[<p>The <b>whole</b> post]] here</p>]]></content:encoded>"
			"<dc:creator>Ann</dc:creator>"
			"<pubDate>Thu, 15 Oct 2026 22:03:10 +0000</pubDate>"
			"<enclosure url=\"https://example.com/a.mp3\" type=\"audio/mpeg\" length=\"2048000\"/>"
			"<media:thumbnail url=\"https://example.com/t.jpg\"/>"
			"</item>"
			"<item><title>Second</title>"
			"<guid isPermaLink=\"true\">https://example.com/2</guid>"
			"<description>Only a summary &amp;amp; more</description>"
			"</item></channel></rss>";
		static const char *const want[] = {
			"<meta charset=\"UTF-8\">",
			"<title>Tom &amp; Jerry&#8217;s</title>",
			"<p>All the news</p>",
			"The site: <a href=\"https://example.com/\">",
			"<h2><a href=\"https://example.com/1?a=1&amp;b=2\">First &lt;post&gt;</a></h2>",
			"<p><i>Thu, 15 Oct 2026 22:03:10 +0000 &middot; Ann</i></p>",
			"<img src=\"https://example.com/t.jpg\"",
			"<div><p>The <b>whole</b> post]] here</p></div>",
			"Enclosure: <a href=\"https://example.com/a.mp3\">audio/mpeg, 2000 KB</a>",
			"<h2><a href=\"https://example.com/2\">Second</a></h2>",
			"<div>Only a summary &amp; more</div>",
			NULL };
		static const char *const not[] = { "Short", "feed\"", "ENTITY", NULL };

		check("rss 2.0", rss, want, not);
	}
	{
		static const char atom[] =
			"<?xml version='1.0' encoding='iso-8859-1'?>"
			"<feed xmlns=\"http://www.w3.org/2005/Atom\">"
			"<title type=\"text\">Atom &lt;feed&gt;</title>"
			"<subtitle>Its subtitle</subtitle>"
			"<link rel=\"self\" href=\"https://example.org/atom\"/>"
			"<link href=\"https://example.org/\"/>"
			"<entry><title type=\"html\">&lt;i&gt;Fancy&lt;/i&gt; title</title>"
			"<link rel=\"alternate\" type=\"text/html\" href=\"https://example.org/e1\"/>"
			"<link rel=\"enclosure\" type=\"video/mp4\" href=\"https://example.org/v.mp4\"/>"
			"<id>tag:example.org,2026:1</id>"
			"<updated>2026-10-16T00:03:16Z</updated>"
			"<author><name>Bea</name><uri>https://example.org/bea</uri></author>"
			"<summary type=\"html\">&lt;p&gt;Summary&lt;/p&gt;</summary>"
			"<content type=\"xhtml\"><div xmlns=\"http://www.w3.org/1999/xhtml\">"
			"<p>Para<br/>graph &amp; more</p></div></content>"
			"<source><title>Not this</title></source>"
			"</entry>"
			"<entry><title>Text</title><id>https://example.org/e2</id>"
			"<content type=\"text\">a &lt; b\nnext line</content></entry>"
			"</feed>";
		static const char *const want[] = {
			"<meta charset=\"iso-8859-1\">",
			"<h1>Atom &lt;feed&gt;</h1>",
			"<p>Its subtitle</p>",
			"The site: <a href=\"https://example.org/\">",
			"<h2><a href=\"https://example.org/e1\">Fancy title</a></h2>",
			"<p><i>2026-10-16 00:03 &middot; Bea</i></p>",
			"<div><div xmlns=\"http://www.w3.org/1999/xhtml\"><p>Para<br>graph &amp; more</p></div></div>",
			"video/mp4",
			"<h2><a href=\"https://example.org/e2\">Text</a></h2>",
			"<p>a &lt; b<br>next line</p>",
			NULL };
		static const char *const not[] = { "Summary", "Not this",
			"https://example.org/atom", NULL };

		check("atom", atom, want, not);
	}
	{
		static const char rdf[] =
			"<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\""
			" xmlns=\"http://purl.org/rss/1.0/\">"
			"<channel rdf:about=\"x\"><title>RDF feed</title><link>https://r.example/</link></channel>"
			"<item rdf:about=\"https://r.example/1\"><title>One</title>"
			"<link>https://r.example/1</link><dc:date>2026-10-01T10:00:00+02:00</dc:date></item>"
			"</rdf:RDF>";
		static const char *const want[] = { "<h1>RDF feed</h1>",
			"<h2><a href=\"https://r.example/1\">One</a></h2>",
			"<i>2026-10-01 10:00</i>", NULL };

		check("rss 1.0", rdf, want, NULL);
	}
	{
		/* YouTube's: the text and picture in a media:group */
		static const char yt[] =
			"<feed xmlns:media=\"http://search.yahoo.com/mrss/\" xmlns=\"http://www.w3.org/2005/Atom\">"
			"<title>A channel</title><entry><title>A video</title>"
			"<link rel=\"alternate\" href=\"https://www.youtube.com/watch?v=x\"/>"
			"<media:group><media:title>A video</media:title>"
			"<media:thumbnail url=\"https://i.ytimg.com/vi/x/hqdefault.jpg\" width=\"480\"/>"
			"<media:description>Line one\nLine two &lt;3</media:description>"
			"</media:group></entry></feed>";
		static const char *const want[] = {
			"<img src=\"https://i.ytimg.com/vi/x/hqdefault.jpg\"",
			"<p>Line one<br>Line two &lt;3</p>", NULL };

		check("youtube", yt, want, NULL);
	}
	{
		/* Mastodon's: no titles */
		static const char m[] =
			"<rss><channel><title>Someone</title><item>"
			"<guid isPermaLink=\"true\">https://m.example/@a/1</guid>"
			"<link>https://m.example/@a/1</link>"
			"<pubDate>Wed, 14 Oct 2026 12:00:00 +0000</pubDate>"
			"<description>&lt;p&gt;A toot&lt;/p&gt;</description></item></channel></rss>";
		static const char *const want[] = {
			"<h2><a href=\"https://m.example/@a/1\">Wed, 14 Oct 2026 12:00:00 +0000</a></h2>",
			"<div><p>A toot</p></div>", NULL };
		static const char *const not[] = { "<i>Wed", NULL };

		check("no titles", m, want, not);
	}
	{
		static const char x[] = "<?xml version=\"1.0\"?><urlset><url><loc>a&amp;b</loc></url></urlset>";
		static const char *const want[] = {
			"<pre>&lt;urlset&gt;&lt;url&gt;&lt;loc&gt;a&amp;amp;b&lt;/loc&gt;", "</pre>", NULL };

		check("other xml", x, want, NULL);
	}
	{
		static const char *const want[] = { "Not a feed", NULL };

		check("no xml", "just some text", want, NULL);
	}
	{
		/* cut off inside an item: what came of it is shown */
		static const char cut[] = "<rss><channel><title>T</title><item><title>Half</title>"
			"<description>Some of it";
		static const char *const want[] = { "<h2>Half</h2>", "<div>Some of it</div>", NULL };

		check("cut short", cut, want, NULL);
	}
	{
		/* a CDATA section's ]s; an & that isn't an entity */
		static const char cd[] = "<rss><channel><item><title>A & B</title>"
			"<description><![CDATA[x]]]]]><![CDATA[>y]]></description></item></channel></rss>";
		static const char *const want[] = { "<h2>A &amp; B</h2>", "<div>x]]]>y</div>", NULL };

		check("cdata", cd, want, NULL);
	}
	if (!feed_type("application/rss+xml") || !feed_type("text/xml")
		|| feed_type("text/html")) {
		printf("FAIL feed_type\n");
		fails++;
	}
	printf("feed: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
