/*
 * pageimg.h - the page's images, on a screen that shows them (X11):
 * fetched after the page, a piece at a time between keys, and decoded
 * straight to the screen at the size the layout shows each at.
 */
#ifndef MANX_PAGEIMG_H
#define MANX_PAGEIMG_H

#include "doc.h"
#include "layout.h"
#include "url.h"

/* what pimg_step did */
enum {
	PIMG_IDLE,		/* nothing left to do for this page */
	PIMG_SHOWN,		/* an image is on the screen (whole or not) */
	PIMG_SIZED,		/* an image's own size came: lay out again */
	PIMG_WORKED,		/* something else (a fetch): call again */
	PIMG_STOPPED		/* poll said stop; call again later */
};

/* A new page: d, its links relative to base, from page_url (the Referer
 * images are fetched with). The last page's images are forgotten. */
void pimg_begin(const struct doc *d, const struct url *base, const char *page_url);

/* No page any more: free everything. */
void pimg_end(void);

/* <img> node's own size, if known: 1, else 0. */
int pimg_size(nodeid node, int *w, int *h);

/* p->images[k] as the screen has it at its size there, or NULL. */
void *pimg_screen(const struct page *p, long k);

/*
 * The next piece of work for page p, whose lines from top on (rows of
 * them) are shown: an image fetched, or decoded to the screen, those
 * shown first. poll(ctx) is called often meanwhile (shown: more of an
 * image is on the screen since the last call): 0 to go on, -1 to stop.
 */
int pimg_step(const struct page *p, long top, long rows,
	int (*poll)(void *ctx, int shown), void *ctx);

/* Fetch no more of this page's images (the user said stop). */
void pimg_cancel(void);

/* How many images the page has, and how many are fetched (or failed). */
void pimg_count(int *total, int *done);

#endif /* MANX_PAGEIMG_H */
