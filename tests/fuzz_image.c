/*
 * fuzz_image - the image decoders under ASan/UBSan: tests/gen_images.py's
 * images mutated (bit flips, bytes inserted, deleted or overwritten, cut
 * short), fed in random pieces, with random scale requests and sinks
 * that sometimes stop; half of them through image/pixels to a random
 * screen format and size. Nothing may crash, read or write out of
 * bounds, send a row outside the image, or take long.
 *
 *   fuzz_image ITERATIONS SEED DIR
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "os.h"
#include "image.h"
#include "pixels.h"

static unsigned long s_rng = 1;

static unsigned long rnd(unsigned long n)
{
	s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL;
	return n ? (unsigned long)(s_rng >> 33) % n : 0;
}

struct out {
	int w, h, stop_at, rows;
};

static int on_size(void *ctx, struct img_info *in, int *shift)
{
	struct out *o = ctx;

	*shift = (int)rnd(5);		/* (4: out of range, ignored) */
	o->w = in->full_w;
	o->h = in->full_h;
	if (in->scalable && *shift >= 1 && *shift <= 3) {
		o->w = (in->full_w + (1 << *shift) - 1) >> *shift;
		o->h = (in->full_h + (1 << *shift) - 1) >> *shift;
	}
	if (in->full_w <= 0 || in->full_h <= 0)
		abort();
	return rnd(50) == 0 ? -1 : 0;
}

static int on_row(void *ctx, int y, const unsigned char *rgba)
{
	struct out *o = ctx;
	volatile unsigned char sink = 0;
	int i;

	if (y < 0 || y >= o->h)
		abort();
	for (i = 0; i < o->w * 4; i++)
		sink ^= rgba[i];	/* (ASan: the whole row is readable) */
	(void)sink;
	return o->stop_at && ++o->rows >= o->stop_at ? -1 : 0;
}

/* through image/pixels */
struct pxo {
	int w, h, masked;
	const struct px_format *f;
};

static int px_size(void *ctx, int w, int h, int masked)
{
	struct pxo *o = ctx;

	if (w <= 0 || h <= 0)
		abort();
	o->w = w;
	o->h = h;
	o->masked = masked;
	return rnd(50) == 0 ? -1 : 0;
}

static int px_row(void *ctx, int y, const unsigned char *px, const unsigned char *mask)
{
	struct pxo *o = ctx;
	volatile unsigned char sink = 0;
	size_t i, n = px_row_bytes(o->f, o->w);

	if (y < 0 || y >= o->h || (o->masked && mask == NULL))
		abort();
	for (i = 0; i < n; i++)
		sink ^= px[i];
	for (i = 0; mask && i < ((size_t)o->w + 7) / 8; i++)
		sink ^= mask[i];
	(void)sink;
	return rnd(200) == 0 ? -1 : 0;
}

static void random_format(struct px_format *f)
{
	static const int bpps[5] = { 1, 8, 16, 24, 32 };
	int i;

	memset(f, 0, sizeof *f);
	f->kind = (enum px_kind)rnd(3);
	f->bpp = bpps[rnd(5)];
	f->byte_msb = (int)rnd(2);
	f->bit_msb = (int)rnd(2);
	f->mask[0] = rnd(2) ? 0xff000000UL : 0xf800UL;
	f->mask[1] = rnd(2) ? 0xff0000UL : 0x7e0UL;
	f->mask[2] = rnd(2) ? 0xff00UL : 0x1fUL;
	for (i = 0; i < 3; i++)
		f->levels[i] = 2 + (int)rnd(6);
	if (f->kind == PX_GRAY)
		f->levels[0] = 2 + (int)rnd(255);
	for (i = 0; i < 256; i++)
		f->pixel[i] = rnd(1UL << 30);
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

int main(int argc, char **argv)
{
	static unsigned char *img[64];
	static size_t ilen[64];
	char path[512], line[512];
	long iters, it;
	int n = 0;
	FILE *m;

	if (argc != 4) {
		fprintf(stderr, "usage: fuzz_image ITERATIONS SEED DIR\n");
		return 2;
	}
	iters = atol(argv[1]);
	s_rng = (unsigned long)atol(argv[2]);
	snprintf(path, sizeof path, "%s/manifest", argv[3]);
	if ((m = fopen(path, "r")) == NULL) {
		printf("fuzz_image: no %s: nothing to fuzz\n", path);
		return 0;
	}
	while (n < 64 && fgets(line, sizeof line, m)) {
		char name[100], file[120];

		if (sscanf(line, "%99s %119s", name, file) != 2)
			continue;
		snprintf(path, sizeof path, "%s/%s", argv[3], file);
		if ((img[n] = slurp(path, &ilen[n])) != NULL)
			n++;
	}
	fclose(m);
	for (it = 0; it < iters; it++) {
		int k = (int)rnd((unsigned long)n), edits = 1 + (int)rnd(6), e, r = IMG_OK;
		size_t len = ilen[k], cap = len + 64, off = 0;
		unsigned char *b = malloc(cap);
		struct img_sink s;
		struct img_dec *d;
		struct out o;
		struct px_format pf;
		struct px_out po;
		struct pxo pxo;
		int use_px = (int)rnd(2);
		clock_t t0 = clock();

		memcpy(b, img[k], len);
		for (e = 0; e < edits && len > 0; e++) {
			size_t at = rnd(len);

			switch (rnd(5)) {
			case 0:		/* flip a bit */
				b[at] ^= (unsigned char)(1 << rnd(8));
				break;
			case 1:		/* a byte */
				b[at] = (unsigned char)rnd(256);
				break;
			case 2:		/* delete one */
				memmove(b + at, b + at + 1, len - at - 1);
				len--;
				break;
			case 3:		/* insert one */
				if (len < cap) {
					memmove(b + at + 1, b + at, len - at);
					b[at] = (unsigned char)rnd(256);
					len++;
				}
				break;
			default:	/* cut short */
				len = at;
			}
		}
		memset(&o, 0, sizeof o);
		o.stop_at = rnd(8) == 0 ? 1 + (int)rnd(20) : 0;
		s.size = on_size;
		s.row = on_row;
		s.ctx = &o;
		if (use_px) {
			random_format(&pf);
			memset(&po, 0, sizeof po);
			memset(&pxo, 0, sizeof pxo);
			pxo.f = &pf;
			po.fmt = &pf;
			po.want_w = rnd(3) ? 0 : (int)rnd(300);
			po.want_h = rnd(3) ? 0 : (int)rnd(300);
			po.max_w = rnd(2) ? 0 : 1 + (int)rnd(200);
			po.max_h = rnd(4) ? 0 : 1 + (int)rnd(200);
			po.size = px_size;
			po.row = px_row;
			po.ctx = &pxo;
			px_sink(&po, &s);
		}
		d = img_new(img_sniff(b, len) != IMG_NONE ? img_sniff(b, len)
			: (enum img_type)(1 + rnd(3)), &s, rnd(4) ? 0 : 4096 + rnd(65536));
		if (d) {
			while (off < len && r == IMG_OK) {
				size_t piece = rnd(3) ? len : 1 + rnd(200);

				if (piece > len - off)
					piece = len - off;
				r = img_feed(d, b + off, piece);
				off += piece;
			}
			if (r == IMG_OK)
				img_finish(d);
			img_free(d);
		}
		if (use_px) {
			px_finish(&po);
			px_free(&po);
		}
		free(b);
		if ((clock() - t0) / CLOCKS_PER_SEC > 2) {
			printf("iteration %ld: image %d took over 2 s\n", it, k);
			return 1;
		}
		if (it % 1000 == 999) {
			printf("%ld iterations\n", it + 1);
			fflush(stdout);
		}
	}
	printf("fuzz_image: %ld iterations ok\n", iters);
	return 0;
}
