/* memwhere - are heap, bss and stack reads equally fast on the TT? The
 * same 256-byte read loop over each, pointers made opaque to the compiler.
 * Prints each region's address too (ST-RAM is below 0x01000000 on a TT). */
#include <stdio.h>
#include <stdlib.h>
#include "os.h"

static long s_bss[64];

static unsigned long run(volatile long *a, const char *what)
{
	unsigned long t0 = os_msec(), ms;
	long i, x = 0;

	for (i = 0; i < 64; i++)
		a[i] = i;
	for (i = 0; i < 2000000L; i++)
		x += a[i & 63];
	ms = os_msec() - t0;
	printf("%-6s at %08lx: 2000000 reads in %lu ms (%ld)\n", what,
		(unsigned long)a, ms, x & 1);
	return ms;
}

int main(void)
{
	long stack[64];
	long *heap = malloc(64 * sizeof(long));
	long *big = malloc(4L << 20);	/* further up the heap */

	run(s_bss, "bss");
	run(heap, "heap");
	run(stack, "stack");
	run(big + (2L << 18), "big");
	return 0;
}
