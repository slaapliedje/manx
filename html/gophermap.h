/*
 * gophermap.h - a gopher menu (RFC 1436 lines) turned into HTML as it
 * streams, for the HTML engine: text lines as text, items as links with
 * a (DIR)/(TXT)/... tag in front.
 */
#ifndef MANX_GOPHERMAP_H
#define MANX_GOPHERMAP_H

#include <stddef.h>

struct gophermap {
	void (*out)(void *ctx, const char *html, size_t n);
	void *ctx;
	char line[1024];
	size_t n;
	int done;			/* the "." line was seen */
};

void gophermap_begin(struct gophermap *g,
	void (*out)(void *ctx, const char *html, size_t n), void *ctx);
void gophermap_feed(struct gophermap *g, const unsigned char *s, size_t n);
void gophermap_end(struct gophermap *g);

#endif /* MANX_GOPHERMAP_H */
