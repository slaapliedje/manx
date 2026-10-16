/*
 * fuzz_feed - the feed converter under ASan/UBSan: RSS and Atom feeds
 * (built in, and any FILEs given) mutated - bytes flipped, inserted,
 * deleted, ranges repeated, feed-shaped pieces put in (CDATA ends,
 * entities, items, self-closing tags), cut short - and fed in random
 * pieces; what comes out goes through the HTML engine too. Nothing may
 * crash or read or write out of bounds.
 *
 *   fuzz_feed ITERATIONS SEED [FILE...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "feed.h"
#include "load.h"

static unsigned long s_rng = 1;

static unsigned long rnd(unsigned long n)
{
	s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL;
	return n ? (unsigned long)(s_rng >> 33) % n : 0;
}

static const char *const seeds[] = {
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?><!-- c --><!DOCTYPE rss [<!ENTITY x \"y\">]>"
	"<rss xmlns:content=\"c\" xmlns:dc=\"d\" xmlns:media=\"m\"><channel><title>T &amp; t</title>"
	"<link>https://e.com/</link><description>D</description><item><title><![CDATA[A <b>]]></title>"
	"<link>https://e.com/1?a=1&amp;b=2</link><description>&lt;p&gt;S&lt;/p&gt;</description>"
	"<content:encoded><![CDATA[<p>W]] x</p>]]></content:encoded><dc:creator>Ann</dc:creator>"
	"<pubDate>Thu, 15 Oct 2026 22:03:10 +0000</pubDate><enclosure url=\"https://e.com/a.mp3\" "
	"type=\"audio/mpeg\" length=\"2048000\"/><media:thumbnail url=\"https://e.com/t.jpg\"/></item>"
	"<item><guid isPermaLink='true'>https://e.com/2</guid><description>x &amp;amp; &#8217;</description>"
	"</item></channel></rss>",
	"<?xml version='1.0' encoding='iso-8859-1'?><feed xmlns=\"http://www.w3.org/2005/Atom\" "
	"xmlns:media=\"m\"><title type=\"text\">A</title><subtitle>S</subtitle><link rel=\"self\" "
	"href=\"https://o.org/a\"/><link href=\"https://o.org/\"/><entry><title type=\"html\">"
	"&lt;i&gt;F&lt;/i&gt;</title><link rel=\"alternate\" href=\"https://o.org/e\"/><link "
	"rel=\"enclosure\" type=\"video/mp4\" href=\"https://o.org/v\" length=\"9\"/><id>tag:x</id>"
	"<updated>2026-10-16T00:03:16Z</updated><author><name>B</name></author><summary type=\"html\">"
	"&lt;p&gt;S&lt;/p&gt;</summary><content type=\"xhtml\"><div xmlns=\"x\"><p>P<br/>g &amp; m</p>"
	"</div></content><media:group><media:thumbnail url=\"https://i/x.jpg\"/><media:description>L1\nL2"
	"</media:description></media:group></entry><entry><content src=\"https://o.org/s\"/></entry></feed>",
	"<rdf:RDF xmlns:rdf=\"r\" xmlns=\"p\"><channel><title>R</title><link>https://r/</link></channel>"
	"<item><title>O</title><link>https://r/1</link><dc:date>2026-10-01T10:00:00+02:00</dc:date></item>"
	"</rdf:RDF>",
	"<?xml version=\"1.0\"?><urlset><url><loc>a&amp;b</loc></url></urlset>",
};

static const char *const bits[] = { "<", ">", "/>", "</", "]]>", "<![CDATA[", "&", "&amp;",
	"&#", "&#x1F600;", "&nbsp;", ";", "\"", "'", "<!--", "-->", "<?", "?>", "<!DOCTYPE [",
	"]", "<item>", "</item>", "<entry>", "</entry>", "<title>", "</title>", "<link href=\"",
	"<content type=\"xhtml\">", "</content>", "<media:group>", "<author><name>",
	"<enclosure url=\"x\" type=\"image/png\"/>", "<rss>", "<feed>", "\n", " ", ":", "\xff",
	"\xe2\x80\x99", "<description>", "</description>", "<content:encoded>", NULL };

static size_t nbits(void)
{
	size_t n = 0;

	while (bits[n])
		n++;
	return n;
}

static char *mutate(const char *in, size_t len, size_t *out_len)
{
	size_t cap = len * 2 + 70000, n = len, k, edits = 1 + rnd(12);
	char *b = malloc(cap);

	memcpy(b, in, len);
	for (k = 0; k < edits && n; k++) {
		size_t at = rnd(n + 1), l;
		const char *s;

		switch (rnd(6)) {
		case 0:		/* a flipped bit */
			if (at < n)
				b[at] ^= (char)(1 << rnd(8));
			break;
		case 1:		/* a feed-shaped piece in */
			s = bits[rnd(nbits())];
			l = strlen(s);
			if (n + l < cap) {
				memmove(b + at + l, b + at, n - at);
				memcpy(b + at, s, l);
				n += l;
			}
			break;
		case 2:		/* a range out */
			l = rnd(n - at + 1);
			memmove(b + at, b + at + l, n - at - l);
			n -= l;
			break;
		case 3:		/* a range repeated */
			l = rnd(n - at + 1);
			if (l > 4000)
				l = 4000;
			if (n + l < cap) {
				memmove(b + at + l, b + at, n - at);
				memcpy(b + at + l, b + at, l);
				n += l;
			}
			break;
		case 4:		/* a long run (past every buffer) */
			l = 1 + rnd(66000);
			if (n + l < cap) {
				memmove(b + at + l, b + at, n - at);
				memset(b + at, "a<&]x\""[rnd(6)], l);
				n += l;
			}
			break;
		case 5:		/* cut short */
			n = at;
			break;
		}
	}
	*out_len = n;
	return b;
}

static struct html_load g_load;
static struct doc g_doc;

static void sink(void *ctx, const char *s, size_t n)
{
	(void)ctx;
	html_load_feed(&g_load, (const unsigned char *)s, n);
}

int main(int argc, char **argv)
{
	static struct feed f;
	unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000, i;
	char **files = NULL;
	size_t *flen = NULL;
	int nfiles = 0, a;

	s_rng = argc > 2 ? strtoul(argv[2], NULL, 10) : 1;
	for (a = 3; a < argc; a++) {
		FILE *fp = fopen(argv[a], "rb");
		long sz;

		if (!fp)
			continue;
		fseek(fp, 0, SEEK_END);
		sz = ftell(fp);
		rewind(fp);
		files = realloc(files, (nfiles + 1) * sizeof *files);
		flen = realloc(flen, (nfiles + 1) * sizeof *flen);
		files[nfiles] = malloc((size_t)sz + 1);
		flen[nfiles] = fread(files[nfiles], 1, (size_t)sz, fp);
		fclose(fp);
		nfiles++;
	}
	for (i = 0; i < iters; i++) {
		size_t ns = sizeof seeds / sizeof seeds[0], len, off = 0;
		size_t pick = rnd(ns + (size_t)nfiles);
		const char *in = pick < ns ? seeds[pick] : files[pick - ns];
		size_t inlen = pick < ns ? strlen(in) : flen[pick - ns];
		char *b = mutate(in, inlen, &len);

		doc_init(&g_doc, 0);
		html_load_begin(&g_load, &g_doc, rnd(2) ? "" : "utf-8", 0);
		feed_begin(&f, sink, NULL);
		while (off < len) {
			size_t n = 1 + rnd(rnd(2) ? 16 : 4096);

			if (n > len - off)
				n = len - off;
			feed_feed(&f, (const unsigned char *)b + off, n);
			off += n;
		}
		feed_end(&f);
		html_load_end(&g_load);
		doc_free(&g_doc);
		free(b);
		if ((i + 1) % 1000 == 0)
			printf("%lu iterations\n", i + 1);
	}
	while (nfiles > 0)
		free(files[--nfiles]);
	free(files);
	free(flen);
	printf("fuzz_feed: %lu iterations ok\n", iters);
	return 0;
}
