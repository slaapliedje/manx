/*
 * bench_mem - is memory the problem on this machine, or the C library?
 * Each pair runs the same instructions except for the thing measured:
 *   - a loop with a load that hits the cache, vs. the same loop without
 *   - the same load pattern within 256 bytes (cache hits) vs. over 1 MB
 *   - the C library's memset/memcpy vs. plain longword loops
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

#define N	2000000L

static long run_loop(const long *a, long mask, int load)
{
	register long i, x = 0;

	if (load)
		for (i = 0; i < N; i++)
			x += a[(i * 17) & mask];
	else
		for (i = 0; i < N; i++)
			x += (i * 17) & mask;
	return x;
}

static void lset(void *p, int c, size_t n)
{
	long *q = p, v = c & 0xFF;

	v |= v << 8;
	v |= v << 16;
	for (n /= 4; n; n--)
		*q++ = v;
}

static void lcpy(void *d, const void *s, size_t n)
{
	long *q = d;
	const long *r = s;

	for (n /= 4; n; n--)
		*q++ = *r++;
}

#define TIME(label, stmt) do { \
	unsigned long t0 = os_msec(); stmt; \
	printf("%-44s %6lu ms\n", label, os_msec() - t0); } while (0)

int main(void)
{
	long *big = calloc(1024 * 1024 / 4, 4);	/* 1 MB */
	char *a = malloc(256 * 1024), *b = malloc(256 * 1024);
	volatile long sink;
	int i;

	TIME("loop, no load", sink = run_loop(big, 63, 0));
	TIME("loop, load within 256 B (cache hits)", sink = run_loop(big, 63, 1));
	TIME("loop, load within 1 MB (misses)", sink = run_loop(big, 262143, 1));
	TIME("libc memset 256 KB x 16", for (i = 0; i < 16; i++) memset(a, i, 256 * 1024));
	TIME("longword loop memset 256 KB x 16", for (i = 0; i < 16; i++) lset(a, i, 256 * 1024));
	TIME("libc memcpy 256 KB x 16", for (i = 0; i < 16; i++) memcpy(b, a, 256 * 1024));
	TIME("longword loop copy 256 KB x 16", for (i = 0; i < 16; i++) lcpy(b, a, 256 * 1024));
	TIME("libc strcmp x 200000", for (i = 0; i < 200000; i++) sink = strcmp(a + (i & 7), "div"));
	(void)sink;
	return 0;
}
