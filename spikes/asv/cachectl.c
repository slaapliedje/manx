/*
 * cachectl - look at and set the 68030 cache control register on Atari
 * System V (a native ASV program, not AMIX: built with the gcc-cross-amix
 * compiler against ASV's own libc).
 *
 *   cachectl get          clock rate and memory size, as the kernel says
 *   cachectl set HEX      CACR := HEX via sysm68k(SM68KCACHE) (root)
 *   cachectl bench        loads that hit the data cache vs. loads that miss
 *
 * CACR bits (68030): 0x0001 EI instruction cache, 0x0010 IBE its burst,
 * 0x0100 ED data cache, 0x1000 DBE its burst, 0x2000 WA write allocate.
 * ASV boots with 0x3111 (everything on).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/times.h>

#define SM68KMEM	65
#define SM68KCACHE	68
#define SM68KGETCLKRT	73
#define N		2000000L

extern int sysm68k();

static long ticks(void)
{
	struct tms t;

	return (long)times(&t);
}

static long loop(long *a, long mask, int load)
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

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "get") == 0) {
		printf("clock rate %d, memory %d\n", sysm68k(SM68KGETCLKRT),
			sysm68k(SM68KMEM));
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "set") == 0) {
		long v = strtol(argv[2], 0, 16);
		int rc = sysm68k(SM68KCACHE, v);

		printf("CACR := 0x%lx (%d)\n", v, rc);
		return rc < 0;
	}
	if (argc >= 2 && strcmp(argv[1], "bench") == 0) {
		/*
		 * The same loop over three spans: 256 B (fits the 256-byte
		 * data cache), 16 KB (misses the cache, but only 4 pages: the
		 * 22-entry ATC still hits), 1 MB (misses both). The memory is
		 * written first (fresh pages may all map one zero page), and
		 * each case runs 3 times: the best is kept (the TT is noisy).
		 */
		static const long masks[3] = { 63, 4095, 262143 };
		static const char *const names[3] = { "256 B", "16 KB", "1 MB" };
		long *big = (long *)malloc(262144 * sizeof(long));
		long best[4], t0, t, i;
		int k, r;
		volatile long sink;

		for (i = 0; i < 262144; i++)
			big[i] = i;
		for (k = 0; k < 4; k++) {
			best[k] = 1L << 30;
			for (r = 0; r < 3; r++) {
				t0 = ticks();
				sink = k == 0 ? loop(big, 63, 0) : loop(big, masks[k - 1], 1);
				t = ticks() - t0;
				if (t < best[k])
					best[k] = t;
			}
		}
		printf("ticks (best of 3): no load %ld", best[0]);
		for (k = 0; k < 3; k++)
			printf(", loads in %s %ld", names[k], best[k + 1]);
		printf("\n");
		return 0;
	}
	fprintf(stderr, "usage: cachectl get | set HEX | bench\n");
	return 2;
}
