/*
 * bench_image FILE [REPS [SHIFT]] - how long an image takes to decode on
 * this machine: ms per decode (the sink only looks at the rows), at full
 * size or, for a JPEG, at 1/2^SHIFT.
 */
#include <stdio.h>
#include <stdlib.h>
#include "os.h"
#include "image.h"

static int g_shift, g_w, g_h;
static unsigned long g_sum;

static int on_size(void *ctx, struct img_info *in, int *shift)
{
	(void)ctx;
	*shift = in->scalable ? g_shift : 0;
	g_w = in->full_w;
	g_h = in->full_h;
	if (*shift) {
		g_w = (g_w + (1 << *shift) - 1) >> *shift;
		g_h = (g_h + (1 << *shift) - 1) >> *shift;
	}
	return 0;
}

static int on_row(void *ctx, int y, const unsigned char *rgba)
{
	(void)ctx;
	g_sum += (unsigned long)y + rgba[0] + rgba[g_w * 4 - 2];
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char *b;
	size_t n;
	int reps = argc > 2 ? atoi(argv[2]) : 3, i, r = IMG_OK;
	unsigned long t0, ms;

	if (argc < 2 || (b = os_read_file(argv[1], &n)) == NULL) {
		fprintf(stderr, "usage: bench_image FILE [REPS [SHIFT]]\n");
		return 2;
	}
	g_shift = argc > 3 ? atoi(argv[3]) : 0;
	t0 = os_msec();
	for (i = 0; i < reps; i++) {
		struct img_sink s;
		struct img_dec *d;

		s.size = on_size;
		s.row = on_row;
		s.ctx = NULL;
		if ((d = img_new(img_sniff(b, n), &s, 1024UL * 1024)) == NULL) {
			fprintf(stderr, "bench_image: not an image (or no memory)\n");
			return 1;
		}
		r = img_feed(d, b, n);
		if (r == IMG_OK)
			r = img_finish(d);
		img_free(d);
	}
	ms = os_msec() - t0;
	printf("%s: %lu bytes -> %dx%d (1/%d), %s, %lu ms each\n", argv[1],
		(unsigned long)n, g_w, g_h, 1 << g_shift, r == IMG_END ? "ok" : "FAILED",
		ms / (unsigned long)(reps > 0 ? reps : 1));
	return r == IMG_END ? 0 : 1;
}
