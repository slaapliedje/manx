/* bench_inflate FILE.gz [reps] - decode speed (output KB/s) */
#include <stdio.h>
#include <stdlib.h>
#include "os.h"
#include "inflate.h"

static unsigned long total;

static int sink(void *ctx, const unsigned char *d, size_t n)
{
	(void)ctx;
	(void)d;
	total += n;
	return 0;
}

int main(int argc, char **argv)
{
	static unsigned char buf[2 << 20];
	FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
	int reps = argc > 2 ? atoi(argv[2]) : 3, i, rc = 0;
	size_t n, off;
	unsigned long t0, ms;

	if (f == NULL)
		return 2;
	n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	t0 = os_msec();
	for (i = 0; i < reps; i++) {
		struct inflate *z = inflate_new(INF_GZIP, sink, NULL);

		/* in 4 KB pieces, as the network delivers */
		for (off = 0; off < n; off += 4096)
			inflate_feed(z, buf + off, n - off < 4096 ? n - off : 4096);
		rc = inflate_finish(z);
		inflate_free(z);
	}
	ms = os_msec() - t0;
	printf("%s: %lu bytes -> %lu, rc %d, %lu ms each, %lu KB/s out\n",
		argv[1], (unsigned long)n, total / (unsigned long)reps, rc,
		ms / (unsigned long)reps, ms ? total / ms : 0);
	return 0;
}
