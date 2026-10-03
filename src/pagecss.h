/*
 * pagecss.h - the page's linked style sheets (<link rel=stylesheet>):
 * fetched after the page, a sheet at a time between keys, through the
 * disk cache, and read into the page's sheet (style/css.h), after which
 * the page is laid out again.
 */
#ifndef MANX_PAGECSS_H
#define MANX_PAGECSS_H

#include "doc.h"
#include "url.h"

/* what pcss_step did */
enum {
	PCSS_IDLE,		/* nothing left to do for this page */
	PCSS_READ,		/* a sheet was read: lay the page out again */
	PCSS_WORKED,		/* something else (one that failed): call again */
	PCSS_STOPPED		/* poll said stop; call again later */
};

/* A new page: d, its links relative to base, from page_url (the
 * Referer). The last page's sheets are forgotten. */
void pcss_begin(struct doc *d, const struct url *base, const char *page_url);

/* No page any more. */
void pcss_end(void);

/* The next sheet. poll(ctx, 0) is called often meanwhile: 0 to go on,
 * -1 to stop. */
int pcss_step(int (*poll)(void *ctx, int shown), void *ctx);

/* How many sheets the page links, and how many are read (or failed). */
void pcss_count(int *total, int *done);

/* Fetch no more of them (the user said stop). */
void pcss_cancel(void);

#endif /* MANX_PAGECSS_H */
