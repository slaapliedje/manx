/*
 * test_image - the image decoders against tests/gen_images.py's images.
 * Each is decoded whole and in random pieces: both must give the expected
 * pixels (within the image's tolerance; a fully transparent pixel's
 * colour doesn't count) or the expected error. Cut and corrupted copies
 * must fail cleanly (or decode: never crash).
 *
 *   test_image DIR      (DIR/manifest, from gen_images.py)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "image.h"

struct out {
	int w, h, shift;
	unsigned char *px;
};

static int on_size(void *ctx, struct img_info *in, int *shift)
{
	struct out *o = ctx;

	*shift = o->shift;
	o->w = in->full_w;
	o->h = in->full_h;
	if (o->shift && in->scalable) {
		o->w = (in->full_w + (1 << o->shift) - 1) >> o->shift;
		o->h = (in->full_h + (1 << o->shift) - 1) >> o->shift;
	}
	free(o->px);
	o->px = calloc((size_t)o->w * (size_t)o->h, 4);
	return o->px ? 0 : -1;
}

static int on_row(void *ctx, int y, const unsigned char *rgba)
{
	struct out *o = ctx;

	if (y < 0 || y >= o->h)
		abort();		/* a row outside the image */
	memcpy(o->px + (size_t)y * (size_t)o->w * 4, rgba, (size_t)o->w * 4);
	return 0;
}

static unsigned char *slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b;
	long len;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);
	b = malloc((size_t)len + 1);
	*n = fread(b, 1, (size_t)len, f);
	fclose(f);
	return b;
}

static unsigned long seed = 1;

static unsigned long rnd(unsigned long n)
{
	seed = seed * 1103515245UL + 12345UL;
	return (seed >> 16) % n;
}

/* decode b (n bytes) in pieces of up to piece bytes (0: whole) */
static int decode(const unsigned char *b, size_t n, size_t piece, struct out *o)
{
	struct img_sink s;
	struct img_dec *d;
	size_t off = 0;
	int r = IMG_OK;

	s.size = on_size;
	s.row = on_row;
	s.ctx = o;
	if ((d = img_new(img_sniff(b, n), &s, 0)) == NULL)
		return -100;
	while (off < n && r == IMG_OK) {
		size_t k = piece ? 1 + rnd(piece) : n;

		if (k > n - off)
			k = n - off;
		r = img_feed(d, b + off, k);
		off += k;
	}
	if (r == IMG_OK)
		r = img_finish(d);
	img_free(d);
	return r;
}

static const char *what(int r)
{
	switch (r) {
	case IMG_END: return "ok";
	case IMG_BAD: return "bad";
	case IMG_STOP: return "stop";
	case IMG_UNSUPPORTED: return "unsupported";
	case IMG_TOOBIG: return "toobig";
	default: return "?";
	}
}

int main(int argc, char **argv)
{
	char path[512], line[512];
	FILE *m;
	int fails = 0, n = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: test_image DIR\n");
		return 2;
	}
	snprintf(path, sizeof path, "%s/manifest", argv[1]);
	if ((m = fopen(path, "r")) == NULL) {
		printf("test_image: no %s (no Pillow?): nothing tested\n", path);
		return 0;
	}
	while (fgets(line, sizeof line, m)) {
		char name[100], file[120], expect[20];
		int w, h, shift, tol, k, worst = 0, over = 0, ok = 1;
		size_t len, rlen, i;
		unsigned char *b, *ref;
		struct out whole, parts;
		int r1, r2;

		if (sscanf(line, "%99s %119s %d %d %d %d %19s", name, file, &w, &h,
			&shift, &tol, expect) != 7)
			continue;
		n++;
		snprintf(path, sizeof path, "%s/%s", argv[1], file);
		b = slurp(path, &len);
		snprintf(path, sizeof path, "%s/%s.rgba", argv[1], name);
		ref = slurp(path, &rlen);
		if (b == NULL || ref == NULL) {
			printf("  %-20s missing\n", name);
			fails++;
			continue;
		}
		if (shift == 0 && strcmp(expect, "ok") == 0) {
			int pw = 0, ph = 0;

			if (!img_probe(b, len, &pw, &ph) || pw != w || ph != h) {
				printf("  %-20s probe says %dx%d, want %dx%d\n", name, pw, ph, w, h);
				fails++;
				ok = 0;
			}
		}
		memset(&whole, 0, sizeof whole);
		memset(&parts, 0, sizeof parts);
		whole.shift = parts.shift = shift;
		r1 = decode(b, len, 0, &whole);
		r2 = decode(b, len, 1 + rnd(300), &parts);
		if (strcmp(what(r1), expect) != 0 || r1 != r2) {
			printf("  %-20s %s whole, %s in pieces; want %s\n", name,
				what(r1), what(r2), expect);
			fails++;
			ok = 0;
		} else if (r1 == IMG_END) {
			if (whole.w != w || whole.h != h || rlen != (size_t)w * h * 4) {
				printf("  %-20s %dx%d, want %dx%d\n", name, whole.w,
					whole.h, w, h);
				fails++;
				ok = 0;
			} else if (memcmp(whole.px, parts.px, rlen) != 0) {
				printf("  %-20s pieces decode differently\n", name);
				fails++;
				ok = 0;
			} else {
				for (i = 0; i < rlen; i += 4) {
					int d, c;

					if (whole.px[i + 3] == 0 && ref[i + 3] == 0)
						continue;	/* both transparent */
					for (c = 0; c < 4; c++) {
						d = abs(whole.px[i + c] - ref[i + c]);
						if (d > worst)
							worst = d;
						if (d > tol)
							over++;
					}
				}
				if (over) {
					printf("  %-20s %d channel(s) off by more than %d "
						"(worst %d)\n", name, over, tol, worst);
					fails++;
					ok = 0;
				}
			}
		}
		/* cut and corrupted copies: no crash, no stray rows */
		for (k = 1; k < 8; k++) {
			struct out o;

			memset(&o, 0, sizeof o);
			decode(b, len * (size_t)k / 8, 0, &o);
			free(o.px);
		}
		for (k = 0; k < 40; k++) {
			unsigned char *c = malloc(len);
			struct out o;
			int f, flips = 1 + (int)rnd(4);

			memcpy(c, b, len);
			for (f = 0; f < flips; f++)
				c[rnd(len)] ^= (unsigned char)(1 << rnd(8));
			memset(&o, 0, sizeof o);
			decode(c, len, rnd(4) ? 0 : 1 + rnd(64), &o);
			free(o.px);
			free(c);
		}
		if (ok)
			printf("  %-20s %s%s", name, what(r1),
				r1 == IMG_END && tol ? "" : "\n");
		if (ok && r1 == IMG_END && tol)
			printf(" (worst %d, within %d)\n", worst, tol);
		free(whole.px);
		free(parts.px);
		free(b);
		free(ref);
	}
	fclose(m);
	printf("%d image(s), %d failed\n", n, fails);
	return fails ? 1 : 0;
}
