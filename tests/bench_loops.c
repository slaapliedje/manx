/* loops - is the 68030 slow at instructions (cache off?) or at memory? */
#include <stdio.h>
#include <stdlib.h>
#include "os.h"

static long s_arr[1024];		/* 4 KB: fits no 68030 cache, but hot in any */

int main(void)
{
	unsigned long t0, ms;
	register long i, x = 0;
	long *heap = calloc(4096, sizeof(long));

	/* registers only: a few instructions per round, 8 rounds unrolled */
	t0 = os_msec();
	for (i = 0; i < 4000000L; i += 8) {
		__asm__ volatile ("" : "+d"(x));
		x += i; x ^= i; x += i; x ^= i; x += i; x ^= i; x += i; x ^= i;
	}
	ms = os_msec() - t0;
	printf("register loop: 4000000 ops in %lu ms (%lu K ops/s)\n", ms,
		ms ? 4000000UL / ms : 0UL);

	/* the same work reading a small array (256 bytes: in the data cache) */
	t0 = os_msec();
	for (i = 0; i < 2000000L; i++)
		x += s_arr[i & 63];
	ms = os_msec() - t0;
	printf("256-byte array reads (bss): 2000000 in %lu ms\n", ms);

	t0 = os_msec();
	for (i = 0; i < 2000000L; i++)
		x += heap[i & 63];
	ms = os_msec() - t0;
	printf("256-byte array reads (heap): 2000000 in %lu ms\n", ms);

	/* a 16 KB walk: past the 256-byte cache, memory speed */
	t0 = os_msec();
	for (i = 0; i < 2000000L; i++)
		x += heap[(i * 17) & 4095];
	ms = os_msec() - t0;
	printf("16 KB scattered reads (heap): 2000000 in %lu ms\n", ms);
	return (int)(x & 1);
}
