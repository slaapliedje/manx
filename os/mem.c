/*
 * mem.c - allocation with a cap. The browser must live in a few MB on
 * the 68030 machines: everything it allocates goes through here, so a
 * page that would take too much fails cleanly (with a message) instead
 * of paging the machine to death. Each block carries its size in a
 * header word (8 bytes, keeping doubles aligned).
 */
#include <stdlib.h>
#include <string.h>
#include "os.h"

#define HDR	8

static size_t s_cap, s_used, s_peak;

void mem_set_cap(size_t bytes) { s_cap = bytes; }
size_t mem_in_use(void) { return s_used; }
size_t mem_peak(void) { return s_peak; }

void *xmalloc(size_t n)
{
	unsigned char *p;

	if (s_cap && s_used + n + HDR > s_cap)
		return NULL;
	p = malloc(n + HDR);
	if (p == NULL)
		return NULL;
	*(size_t *)p = n;
	s_used += n + HDR;
	if (s_used > s_peak)
		s_peak = s_used;
	return p + HDR;
}

void xfree(void *q)
{
	unsigned char *p = q;

	if (p == NULL)
		return;
	p -= HDR;
	s_used -= *(size_t *)p + HDR;
	free(p);
}

void *xrealloc(void *q, size_t n)
{
	unsigned char *p = q, *np;
	size_t old;

	if (p == NULL)
		return xmalloc(n);
	p -= HDR;
	old = *(size_t *)p;
	if (s_cap && n > old && s_used + (n - old) > s_cap)
		return NULL;
	np = realloc(p, n + HDR);
	if (np == NULL)
		return NULL;
	*(size_t *)np = n;
	s_used = s_used - old + n;
	if (s_used > s_peak)
		s_peak = s_used;
	return np + HDR;
}

char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = xmalloc(n);

	if (d)
		memcpy(d, s, n);
	return d;
}
