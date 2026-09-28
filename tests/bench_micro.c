/*
 * bench_micro - small timings that explain the parser's speed on a given
 * machine: call overhead, small appends into static vs heap memory, tag
 * name lookups. Each line: operations and milliseconds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "tags.h"

#define N	150000L

static char s_static[600000];
volatile int sink;

__attribute__((noinline)) static void empty(int x) { sink = x; }

__attribute__((noinline)) static void append(char *buf, size_t *len,
	const char *s, size_t n)
{
	memcpy(buf + *len, s, n);
	*len += n;
}

static void timeit(const char *what, long ops, unsigned long t0)
{
	unsigned long ms = os_msec() - t0;

	/* integers only: no floating point on the 68030 build */
	printf("%-34s %8ld ops %6lu ms  %8lu ns/op\n", what, ops, ms,
		ops ? (unsigned long)(ms * 1000000UL / (unsigned long)ops) : 0UL);
}

int main(void)
{
	char *heap = malloc(600000), *xheap = xmalloc(600000);
	size_t len;
	long i;
	unsigned long t0;
	static const char *const names[] = { "div", "a", "span", "td", "script", "li", "p", "xyz" };

	t0 = os_msec();
	for (i = 0; i < N * 4; i++)
		empty((int)i);
	timeit("empty call", N * 4, t0);

	t0 = os_msec();
	for (i = 0, len = 0; i < N; i++)
		append(s_static, &len, "abc", 3);
	timeit("3-byte append, static (bss)", N, t0);

	t0 = os_msec();
	for (i = 0, len = 0; i < N; i++)
		append(heap, &len, "abc", 3);
	timeit("3-byte append, malloc", N, t0);

	t0 = os_msec();
	for (i = 0, len = 0; i < N; i++)
		append(xheap, &len, "abc", 3);
	timeit("3-byte append, xmalloc", N, t0);

	t0 = os_msec();
	for (i = 0; i < N; i++)
		sink = tag_lookup(names[i & 7]);
	timeit("tag_lookup", N, t0);

	t0 = os_msec();
	for (i = 0; i < 20; i++)
		memset(heap, (int)i, 600000);
	timeit("memset 600 KB (heap), x20", 20, t0);
	t0 = os_msec();
	for (i = 0; i < 20; i++)
		memset(s_static, (int)i, 600000);
	timeit("memset 600 KB (bss), x20", 20, t0);
	printf("heap at %p, bss at %p, stack at %p\n", (void *)heap,
		(void *)s_static, (void *)&len);
	return 0;
}
