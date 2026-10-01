/*
 * imgconv - an image through the decoders and image/pixels as a screen
 * would get it, written back out as a PPM to look at (the screen's pixels
 * mapped back to colours; transparent pixels as a grey check):
 *
 *   imgconv [-w W] [-h H] [-f FORMAT] IN OUT.ppm
 *
 * FORMAT: true (24-bit), 565 (16-bit), cube6 (6x6x6, 216 colours), cube4
 * (64), gray16, mono.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "image.h"
#include "pixels.h"
#include "pxfmt.h"

static struct px_format g_f;
static int g_w, g_h, g_masked;
static unsigned char *g_rgb;

static int on_size(void *ctx, int w, int h, int masked)
{
	(void)ctx;
	g_w = w;
	g_h = h;
	g_masked = masked;
	g_rgb = calloc((size_t)w * h, 3);
	return g_rgb ? 0 : -1;
}

/* a pixel value back to its colour */
static void colour(unsigned long p, unsigned char *c)
{
	int k;

	switch (g_f.kind) {
	case PX_TRUE:
		for (k = 0; k < 3; k++) {
			unsigned long m = g_f.mask[k], v = p & m;
			int bits = 0;

			while (m && !(m & 1)) {
				m >>= 1;
				v >>= 1;
			}
			while (m >> bits & 1)
				bits++;
			c[k] = (unsigned char)(v * 255 / ((1UL << bits) - 1));
		}
		break;
	case PX_CUBE:
		c[0] = (unsigned char)(p / (g_f.levels[1] * g_f.levels[2]) * 255 / (g_f.levels[0] - 1));
		c[1] = (unsigned char)(p / g_f.levels[2] % g_f.levels[1] * 255 / (g_f.levels[1] - 1));
		c[2] = (unsigned char)(p % g_f.levels[2] * 255 / (g_f.levels[2] - 1));
		break;
	case PX_GRAY:
		c[0] = c[1] = c[2] = (unsigned char)(p * 255 / (g_f.levels[0] - 1));
		break;
	}
}

static int on_row(void *ctx, int y, const unsigned char *px, const unsigned char *mask)
{
	int x;

	(void)ctx;
	for (x = 0; x < g_w; x++) {
		unsigned char *o = g_rgb + ((size_t)y * g_w + x) * 3;
		unsigned long p;

		if (mask && !(mask[x >> 3] & (0x80 >> (x & 7)))) {
			o[0] = o[1] = o[2] = ((x >> 3) + (y >> 3)) & 1 ? 200 : 150;
			continue;
		}
		switch (g_f.bpp) {
		case 1:
			p = px[x >> 3] >> (7 - (x & 7)) & 1;
			break;
		case 8:
			p = px[x];
			break;
		case 16:
			p = (unsigned long)px[x * 2] << 8 | px[x * 2 + 1];
			break;
		case 24:
			p = (unsigned long)px[x * 3] << 16 | (unsigned long)px[x * 3 + 1] << 8 | px[x * 3 + 2];
			break;
		default:
			p = (unsigned long)px[x * 4] << 24 | (unsigned long)px[x * 4 + 1] << 16
				| (unsigned long)px[x * 4 + 2] << 8 | px[x * 4 + 3];
		}
		colour(p, o);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *fmt = "true";
	unsigned char *b;
	size_t n;
	struct px_out o;
	struct img_sink s;
	struct img_dec *d;
	int i, r;
	FILE *out;

	memset(&o, 0, sizeof o);
	for (i = 1; i + 1 < argc && argv[i][0] == '-'; i += 2)
		if (strcmp(argv[i], "-w") == 0)
			o.want_w = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "-h") == 0)
			o.want_h = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "-f") == 0)
			fmt = argv[i + 1];
	if (argc - i != 2 || (b = os_read_file(argv[i], &n)) == NULL) {
		fprintf(stderr, "usage: imgconv [-w W] [-h H] [-f true|565|cube6|cube4|gray16|mono] IN OUT.ppm\n");
		return 2;
	}
	if (px_format_named(&g_f, fmt) < 0) {
		fprintf(stderr, "imgconv: no format %s\n", fmt);
		return 2;
	}
	o.fmt = &g_f;
	o.size = on_size;
	o.row = on_row;
	px_sink(&o, &s);
	if ((d = img_new(img_sniff(b, n), &s, 4UL << 20)) == NULL) {
		fprintf(stderr, "imgconv: not an image\n");
		return 1;
	}
	r = img_feed(d, b, n);
	if (r == IMG_OK)
		r = img_finish(d);
	img_free(d);
	px_finish(&o);
	px_free(&o);
	if (g_rgb == NULL) {
		fprintf(stderr, "imgconv: decoding failed (%d)\n", r);
		return 1;
	}
	if ((out = fopen(argv[argc - 1], "wb")) == NULL)
		return 1;
	fprintf(out, "P6\n%d %d\n255\n", g_w, g_h);
	fwrite(g_rgb, 3, (size_t)g_w * g_h, out);
	fclose(out);
	printf("%s: %dx%d%s, %s\n", argv[argc - 1], g_w, g_h, g_masked ? " masked" : "",
		r == IMG_END ? "complete" : "INCOMPLETE");
	return 0;
}
