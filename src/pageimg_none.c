/*
 * pageimg_none.c - the page's images where the screen shows none (the
 * terminal manx): pageimg.h's calls, doing nothing, so that the decoders
 * aren't linked in for nothing.
 */
#include <stddef.h>
#include "pageimg.h"

void pimg_begin(const struct doc *d, const struct url *base, const char *page_url)
{
	(void)d;
	(void)base;
	(void)page_url;
}

void pimg_end(void)
{
}

int pimg_size(nodeid node, int *w, int *h)
{
	(void)node;
	(void)w;
	(void)h;
	return 0;
}

void *pimg_screen(const struct page *p, long k)
{
	(void)p;
	(void)k;
	return NULL;
}

int pimg_step(const struct page *p, long top, long rows,
	int (*poll)(void *ctx, int shown), void *ctx)
{
	(void)p;
	(void)top;
	(void)rows;
	(void)poll;
	(void)ctx;
	return PIMG_IDLE;
}

void pimg_cancel(void)
{
}

void pimg_count(int *total, int *done)
{
	*total = *done = 0;
}
