/*
 * feed.h - an RSS (0.9x, 1.0, 2.0) or Atom feed turned into HTML as it
 * streams, for the HTML engine: the feed's title and description, then
 * each item as it completes - its title as a link, date and author,
 * picture, text and enclosures. XML that isn't a feed is shown as its
 * source.
 *
 * Bytes go out in the feed's own encoding, after a <meta charset> naming
 * it (from the XML declaration, else UTF-8), which the HTML engine reads
 * when HTTP named none.
 */
#ifndef MANX_FEED_H
#define MANX_FEED_H

#include <stddef.h>

#define FEED_TAG_MAX	2048	/* longest tag kept whole (longer: dropped) */
#define FEED_TEXT_MAX	65536	/* an item's text (more is cut short) */
#define FEED_DEPTH	32
#define FEED_NAME_MAX	24	/* element names kept for the path */
#define FEED_ENCL_MAX	4

struct feed_encl {
	char url[1024];
	char type[64];
	long length;
};

struct feed {
	void (*out)(void *ctx, const char *html, size_t n);
	void *ctx;

	/* the XML tokenizer */
	int state;
	char tag[FEED_TAG_MAX];		/* a tag, comment or PI being read */
	size_t tag_n;
	int tag_long;			/* longer than tag[]: dropped */
	char quote;			/* in a tag's quoted value */
	int dashes;			/* '-' or ']' just seen (comment/CDATA end) */
	char last;			/* a tag's last character that shows */
	int bracket;			/* <!DOCTYPE [ ... ]> */
	char ent[12];			/* an entity in text */
	size_t ent_n;

	/* where in the document */
	int kind;			/* FEED_K_* */
	int depth;
	char path[FEED_DEPTH][FEED_NAME_MAX];	/* local names, outermost first */
	int item;			/* depth of the open item/entry, or 0 */
	int head_done;			/* the feed's head written */
	int meta_done;			/* <meta charset> written */
	char encoding[40];		/* from <?xml encoding=?> */
	int nitems;

	/* text being captured: into which buffer, how */
	char *cap;
	size_t cap_max, *cap_n;
	int cap_depth;			/* the capturing element's depth */
	int cap_raw;			/* HTML kept as is (else escaped) */
	int cap_xhtml;			/* markup inside copied as HTML */

	/* the feed's own fields */
	char title[512], link[1024], sub[2048];
	size_t title_n, link_n, sub_n;

	/* the item being read */
	char ititle[1024], ilink[1024], iguid[1024], idate[80], iauthor[200];
	size_t ititle_n, ilink_n, iguid_n, idate_n, iauthor_n;
	int ititle_raw, iguid_link;
	char img[1024];
	size_t img_n;
	char text[FEED_TEXT_MAX];
	size_t text_n;
	int text_kind;			/* 0 none, 1 summary, 2 the full text */
	int text_plain;			/* plain text: line breaks kept */
	int text_cut;
	char content_src[1024];		/* Atom <content src=> */
	size_t content_src_n;
	struct feed_encl encl[FEED_ENCL_MAX];
	int nencl;
};

enum { FEED_K_UNKNOWN, FEED_K_RSS, FEED_K_ATOM, FEED_K_RDF, FEED_K_XML };

void feed_begin(struct feed *f,
	void (*out)(void *ctx, const char *html, size_t n), void *ctx);
void feed_feed(struct feed *f, const unsigned char *s, size_t n);
void feed_end(struct feed *f);

/* Is a Content-Type (lower case, no parameters) one of a feed's, or of
 * XML that may be one? */
int feed_type(const char *ctype);

#endif /* MANX_FEED_H */
