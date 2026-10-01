/*
 * bench_image - how long an image takes to decode on this machine, in ms
 * per decode:
 *
 *   bench_image [-n REPS] [-s SHIFT] FILE
 *	the decoder alone (the sink only looks at the rows), at full size
 *	or, for a JPEG, at 1/2^SHIFT
 *   bench_image [-n REPS] -f FORMAT [-w WIDTH] FILE
 *	as a screen gets it: through image/pixels, scaled to WIDTH (a JPEG
 *	decoding smaller when it can) and dithered to FORMAT (true, 565,
 *	cube6, cube4, gray16, mono)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "image.h"
#include "pixels.h"
#include "pxfmt.h"

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

static int px_size(void *ctx, int w, int h, int masked)
{
	(void)ctx;
	(void)masked;
	g_w = w;
	g_h = h;
	return 0;
}

static int px_row(void *ctx, int y, const unsigned char *px, const unsigned char *mask)
{
	(void)ctx;
	(void)mask;
	g_sum += (unsigned long)y + px[0];
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char *b = NULL;
	size_t n;
	int reps = 3, width = 0, i, r = IMG_OK;
	const char *fmt = NULL;
	struct px_format pf;
	unsigned long t0, ms;

	for (i = 1; i + 1 < argc && argv[i][0] == '-'; i += 2)
		if (strcmp(argv[i], "-n") == 0)
			reps = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "-s") == 0)
			g_shift = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "-w") == 0)
			width = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "-f") == 0)
			fmt = argv[i + 1];
	if (argc - i != 1 || (b = os_read_file(argv[i], &n)) == NULL
		|| (fmt && px_format_named(&pf, fmt) < 0)) {
		fprintf(stderr, "usage: bench_image [-n REPS] [-s SHIFT | -f FORMAT [-w WIDTH]] FILE\n");
		return 2;
	}
	t0 = os_msec();
	for (i = 0; i < reps; i++) {
		struct img_sink s;
		struct img_dec *d;
		struct px_out o;

		s.size = on_size;
		s.row = on_row;
		s.ctx = NULL;
		if (fmt) {
			memset(&o, 0, sizeof o);
			o.fmt = &pf;
			o.want_w = width;
			o.size = px_size;
			o.row = px_row;
			px_sink(&o, &s);
		}
		if ((d = img_new(img_sniff(b, n), &s, 1024UL * 1024)) == NULL) {
			fprintf(stderr, "bench_image: not an image (or no memory)\n");
			return 1;
		}
		r = img_feed(d, b, n);
		if (r == IMG_OK)
			r = img_finish(d);
		img_free(d);
		if (fmt) {
			px_finish(&o);
			px_free(&o);
		}
	}
	ms = os_msec() - t0;
	if (fmt)
		printf("%s: %lu bytes -> %dx%d %s, %s, %lu ms each\n", argv[argc - 1],
			(unsigned long)n, g_w, g_h, fmt, r == IMG_END ? "ok" : "FAILED",
			ms / (unsigned long)(reps > 0 ? reps : 1));
	else
		printf("%s: %lu bytes -> %dx%d (1/%d), %s, %lu ms each\n", argv[argc - 1],
			(unsigned long)n, g_w, g_h, 1 << g_shift, r == IMG_END ? "ok" : "FAILED",
			ms / (unsigned long)(reps > 0 ? reps : 1));
	return r == IMG_END ? 0 : 1;
}
