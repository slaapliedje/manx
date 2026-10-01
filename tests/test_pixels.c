/*
 * test_pixels - image/pixels: packing for each kind of screen, the dither
 * (an 8x8 tile of a flat colour averages to that colour), the box filter,
 * growing, the size rules, JPEG scale choice, rows out of order, masks,
 * stopping, and that every target row is sent exactly once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "pixels.h"

static int fails, checks;

#define CHECK(c, ...) do {						\
	checks++;							\
	if (!(c)) {							\
		fails++;						\
		printf("  FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

/* what the sink sent */
struct rec {
	int w, h, masked, size_calls;
	size_t rb;
	unsigned char *px, *mask;
	int *sent;		/* times each row came */
	int stop_after;		/* rows, then -1 (0: never) */
	int rows;
	const struct px_format *f;
};

static int rec_size(void *ctx, int w, int h, int masked)
{
	struct rec *r = ctx;

	r->size_calls++;
	r->w = w;
	r->h = h;
	r->masked = masked;
	r->rb = px_row_bytes(r->f, w);
	r->px = calloc((size_t)h, r->rb);
	r->mask = calloc((size_t)h, ((size_t)w + 7) / 8);
	r->sent = calloc((size_t)h, sizeof *r->sent);
	return 0;
}

static int rec_row(void *ctx, int y, const unsigned char *px, const unsigned char *mask)
{
	struct rec *r = ctx;

	if (y < 0 || y >= r->h) {
		printf("  FAIL: row %d outside 0-%d\n", y, r->h - 1);
		fails++;
		return -1;
	}
	r->sent[y]++;
	memcpy(r->px + (size_t)y * r->rb, px, r->rb);
	if (mask)
		memcpy(r->mask + (size_t)y * ((r->w + 7) / 8), mask, ((size_t)r->w + 7) / 8);
	else if (r->masked) {
		printf("  FAIL: masked, but row %d came without one\n", y);
		fails++;
	}
	r->rows++;
	return r->stop_after && r->rows >= r->stop_after ? -1 : 0;
}

static void rec_free(struct rec *r)
{
	free(r->px);
	free(r->mask);
	free(r->sent);
	memset(r, 0, sizeof *r);
}

/*
 * Run an image of sw x sh (pixel (x, y) from src, RGBA) through the
 * sink, rows in the order given (NULL: in order), as a decoder would.
 * Returns the last callback result.
 */
static int run(struct px_out *o, struct rec *r, const unsigned char *src, int sw, int sh,
	int alpha, int interlaced, const int *order, int *shift_out)
{
	struct img_sink s;
	struct img_info in;
	int shift = 0, i, res;

	r->f = o->fmt;
	o->size = rec_size;
	o->row = rec_row;
	o->ctx = r;
	px_sink(o, &s);
	memset(&in, 0, sizeof in);
	in.w = in.full_w = sw;
	in.h = in.full_h = sh;
	in.alpha = alpha;
	in.interlaced = interlaced;
	res = s.size(s.ctx, &in, &shift);
	if (shift_out)
		*shift_out = shift;
	for (i = 0; i < sh && res >= 0; i++) {
		int y = order ? order[i] : i;

		res = s.row(s.ctx, y, src + (size_t)y * sw * 4);
	}
	if (res >= 0)
		res = px_finish(o);
	px_free(o);
	return res;
}

static unsigned char *flat(int w, int h, int r, int g, int b, int a)
{
	unsigned char *p = malloc((size_t)w * h * 4);
	int i;

	for (i = 0; i < w * h; i++) {
		p[i * 4] = (unsigned char)r;
		p[i * 4 + 1] = (unsigned char)g;
		p[i * 4 + 2] = (unsigned char)b;
		p[i * 4 + 3] = (unsigned char)a;
	}
	return p;
}

/* a TrueColor format of 8-bit channels, for reading pixels back */
static void fmt_true(struct px_format *f, int bpp, int msb, unsigned long rm,
	unsigned long gm, unsigned long bm)
{
	memset(f, 0, sizeof *f);
	f->kind = PX_TRUE;
	f->bpp = bpp;
	f->byte_msb = msb;
	f->bit_msb = 1;
	f->mask[0] = rm;
	f->mask[1] = gm;
	f->mask[2] = bm;
}

/* pixel (x, y) of a 32-bit 0x00RRGGBB MSB-first recording */
static void rgb_at(const struct rec *r, int x, int y, int *c)
{
	const unsigned char *p = r->px + (size_t)y * r->rb + (size_t)x * 4;

	c[0] = p[1];
	c[1] = p[2];
	c[2] = p[3];
}

static int mask_at(const struct rec *r, int x, int y)
{
	return r->mask[(size_t)y * ((r->w + 7) / 8) + x / 8] >> (7 - x % 8) & 1;
}

static void test_packing(void)
{
	struct px_format f;
	struct px_out o;
	struct rec r;
	unsigned char src[2 * 4] = { 10, 20, 30, 255, 200, 100, 50, 255 };
	static const unsigned char tt[8] = { 10, 20, 30, 0, 200, 100, 50, 0 };
	static const unsigned char x24[6] = { 30, 20, 10, 50, 100, 200 };

	/* the TT's Xatw -depth 32: 0xRRGGBBxx, most significant byte first */
	fmt_true(&f, 32, 1, 0xff000000UL, 0xff0000UL, 0xff00UL);
	memset(&o, 0, sizeof o);
	memset(&r, 0, sizeof r);
	o.fmt = &f;
	CHECK(run(&o, &r, src, 2, 1, 0, 0, NULL, NULL) == 0, "32 bpp ran");
	CHECK(memcmp(r.px, tt, 8) == 0, "32 bpp RRGGBBxx: %02x %02x %02x %02x", r.px[0], r.px[1], r.px[2], r.px[3]);
	rec_free(&r);

	/* a PC's 24-bit, packed 3 bytes, least significant first */
	fmt_true(&f, 24, 0, 0xff0000UL, 0xff00UL, 0xffUL);
	memset(&o, 0, sizeof o);
	o.fmt = &f;
	run(&o, &r, src, 2, 1, 0, 0, NULL, NULL);
	CHECK(r.rb == 6 && memcmp(r.px, x24, 6) == 0, "24 bpp BGR");
	rec_free(&r);
}

/* the mean of a flat colour's 8x8 tile, per channel, read back by rd */
static void tile_mean(const struct px_format *f, int cr, int cg, int cb, double *m,
	void (*rd)(const struct rec *, int, int, double *))
{
	struct px_out o;
	struct rec r;
	unsigned char *src = flat(8, 8, cr, cg, cb, 255);
	int x, y, k;

	memset(&o, 0, sizeof o);
	memset(&r, 0, sizeof r);
	o.fmt = f;
	run(&o, &r, src, 8, 8, 0, 0, NULL, NULL);
	m[0] = m[1] = m[2] = 0;
	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x++) {
			double c[3];

			rd(&r, x, y, c);
			for (k = 0; k < 3; k++)
				m[k] += c[k] / 64;
		}
	rec_free(&r);
	free(src);
}

static void rd565(const struct rec *r, int x, int y, double *c)
{
	const unsigned char *p = r->px + (size_t)y * r->rb + (size_t)x * 2;
	unsigned v = (unsigned)p[0] | (unsigned)p[1] << 8;

	c[0] = (v >> 11 & 31) * 255.0 / 31;
	c[1] = (v >> 5 & 63) * 255.0 / 63;
	c[2] = (v & 31) * 255.0 / 31;
}

static int cube_l[3];

static void rdcube(const struct rec *r, int x, int y, double *c)
{
	int i = r->px[(size_t)y * r->rb + (size_t)x];

	c[0] = (i / (cube_l[1] * cube_l[2])) * 255.0 / (cube_l[0] - 1);
	c[1] = (i / cube_l[2] % cube_l[1]) * 255.0 / (cube_l[1] - 1);
	c[2] = (i % cube_l[2]) * 255.0 / (cube_l[2] - 1);
}

static void test_dither(void)
{
	static const int col[][3] = {
		{ 0, 0, 0 }, { 255, 255, 255 }, { 100, 150, 200 }, { 1, 254, 128 },
		{ 37, 73, 219 }, { 128, 128, 128 }
	};
	struct px_format f;
	double m[3];
	int i, k, n, l;

	/* 16-bit 565: the channels dithered between their 5/6-bit levels */
	fmt_true(&f, 16, 0, 0xf800UL, 0x7e0UL, 0x1fUL);
	for (i = 0; i < (int)(sizeof col / sizeof col[0]); i++) {
		tile_mean(&f, col[i][0], col[i][1], col[i][2], m, rd565);
		for (k = 0; k < 3; k++)
			CHECK(m[k] > col[i][k] - 1.5 && m[k] < col[i][k] + 1.5,
				"565 colour %d channel %d: mean %.2f", i, k, m[k]);
	}
	/* colour cubes, pixel = index */
	for (l = 0; l < 3; l++) {
		static const int lv[3][3] = { { 6, 6, 6 }, { 8, 8, 4 }, { 2, 2, 2 } };

		memset(&f, 0, sizeof f);
		f.kind = PX_CUBE;
		f.bpp = 8;
		for (k = 0; k < 3; k++)
			f.levels[k] = cube_l[k] = lv[l][k];
		for (n = 0; n < 256; n++)
			f.pixel[n] = (unsigned long)n;
		for (i = 0; i < (int)(sizeof col / sizeof col[0]); i++) {
			tile_mean(&f, col[i][0], col[i][1], col[i][2], m, rdcube);
			for (k = 0; k < 3; k++) {
				double step = 255.0 / (lv[l][k] - 1);

				/* within a 64th of a level (the frac rounding) */
				CHECK(m[k] > col[i][k] - step / 64 - 0.01 && m[k] < col[i][k] + 0.01,
					"cube %dx%dx%d colour %d channel %d: mean %.2f",
					lv[l][0], lv[l][1], lv[l][2], i, k, m[k]);
			}
		}
	}
}

static void test_mono(void)
{
	struct px_format f;
	struct px_out o;
	struct rec r;
	unsigned char *src;
	int msb, ones, x, y;

	for (msb = 0; msb < 2; msb++) {
		memset(&f, 0, sizeof f);
		f.kind = PX_GRAY;
		f.bpp = 1;
		f.bit_msb = msb;
		f.levels[0] = 2;
		f.pixel[0] = 1;		/* a server whose black is 1 */
		f.pixel[1] = 0;
		/* 50% grey: half of each tile white */
		src = flat(8, 8, 128, 128, 128, 255);
		memset(&o, 0, sizeof o);
		memset(&r, 0, sizeof r);
		o.fmt = &f;
		run(&o, &r, src, 8, 8, 0, 0, NULL, NULL);
		ones = 0;
		for (y = 0; y < 8; y++)
			for (x = 0; x < 8; x++)
				ones += r.px[y] >> x & 1;
		CHECK(r.rb == 1 && ones == 32, "mono 50%%: %d of 64 black", ones);
		rec_free(&r);
		free(src);
		/* 9 wide, only the first pixel black: bit order */
		src = flat(9, 1, 255, 255, 255, 255);
		src[0] = src[1] = src[2] = 0;
		memset(&o, 0, sizeof o);
		o.fmt = &f;
		run(&o, &r, src, 9, 1, 0, 0, NULL, NULL);
		CHECK(r.rb == 2 && r.px[0] == (msb ? 0x80 : 0x01) && r.px[1] == 0,
			"mono bit order %s: %02x %02x", msb ? "msb" : "lsb", r.px[0], r.px[1]);
		rec_free(&r);
		free(src);
	}
}

static void test_scale(void)
{
	struct px_format f;
	struct px_out o;
	struct rec r;
	unsigned char src[16 * 4], row10[10 * 4];
	int c[3], x, y, shift;

	fmt_true(&f, 32, 1, 0xff0000UL, 0xff00UL, 0xffUL);
	/* 4x4 -> 2x2: each the mean of 2x2 */
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++) {
			unsigned char *p = src + (y * 4 + x) * 4;

			p[0] = (unsigned char)(x * 10 + y);
			p[1] = (unsigned char)(x * 50);
			p[2] = (unsigned char)(y * 60);
			p[3] = 255;
		}
	memset(&o, 0, sizeof o);
	memset(&r, 0, sizeof r);
	o.fmt = &f;
	o.want_w = 2;
	run(&o, &r, src, 4, 4, 0, 0, NULL, NULL);
	CHECK(r.w == 2 && r.h == 2, "4x4 to width 2: %dx%d", r.w, r.h);
	rgb_at(&r, 1, 1, c);
	/* x 2,3 y 2,3: r (22+32+23+33)/4 = 27.5 -> 28, g 125, b 150 */
	CHECK(c[0] == 28 && c[1] == 125 && c[2] == 150, "box mean: %d %d %d", c[0], c[1], c[2]);
	CHECK(r.sent[0] == 1 && r.sent[1] == 1, "each row once");
	rec_free(&r);

	/* 10 -> 4: pixels [0,2) [2,5) [5,7) [7,10) */
	for (x = 0; x < 10; x++) {
		row10[x * 4] = (unsigned char)(x * 20);
		row10[x * 4 + 1] = row10[x * 4 + 2] = 0;
		row10[x * 4 + 3] = 255;
	}
	memset(&o, 0, sizeof o);
	o.fmt = &f;
	o.want_w = 4;
	o.want_h = 1;
	run(&o, &r, row10, 10, 1, 0, 0, NULL, NULL);
	{
		static const int want[4] = { 10, 60, 110, 160 };

		for (x = 0; x < 4; x++) {
			rgb_at(&r, x, 0, c);
			CHECK(c[0] == want[x], "10 to 4, pixel %d: %d, want %d", x, c[0], want[x]);
		}
	}
	rec_free(&r);

	/* 2x2 -> 4x4: repeated */
	memset(&o, 0, sizeof o);
	o.fmt = &f;
	o.want_w = 4;
	o.want_h = 4;
	run(&o, &r, src, 2, 2, 0, 0, NULL, NULL);
	rgb_at(&r, 3, 3, c);
	CHECK(r.w == 4 && r.h == 4 && c[0] == src[(1 * 2 + 1) * 4], "grown 2x: %d", c[0]);
	for (y = 0; y < 4; y++)
		CHECK(r.sent[y] == 1, "grown: row %d sent %d times", y, r.sent[y]);
	rec_free(&r);

	/* sizes: in proportion, and limits */
	{
		static const struct { int fw, fh, ww, wh, mw, mh, w, h; } sz[] = {
			{ 100, 50, 0, 0, 0, 0, 100, 50 },
			{ 100, 50, 50, 0, 0, 0, 50, 25 },
			{ 100, 50, 0, 10, 0, 0, 20, 10 },
			{ 100, 50, 30, 30, 0, 0, 30, 30 },
			{ 200, 100, 0, 0, 50, 0, 50, 25 },
			{ 200, 100, 0, 0, 0, 20, 40, 20 },
			{ 200, 100, 400, 0, 300, 0, 300, 150 },
			{ 1000, 1, 0, 0, 10, 0, 10, 1 },
			{ 1, 1000, 0, 0, 0, 10, 1, 10 },
		};
		unsigned char *big;
		int i;

		for (i = 0; i < (int)(sizeof sz / sizeof sz[0]); i++) {
			big = flat(sz[i].fw, sz[i].fh, 1, 2, 3, 255);
			memset(&o, 0, sizeof o);
			o.fmt = &f;
			o.want_w = sz[i].ww;
			o.want_h = sz[i].wh;
			o.max_w = sz[i].mw;
			o.max_h = sz[i].mh;
			run(&o, &r, big, sz[i].fw, sz[i].fh, 0, 0, NULL, NULL);
			CHECK(r.w == sz[i].w && r.h == sz[i].h, "size case %d: %dx%d, want %dx%d",
				i, r.w, r.h, sz[i].w, sz[i].h);
			rec_free(&r);
			free(big);
		}
	}

	/* a JPEG's scale: 640x480 shown 100 wide decodes at 1/4 (160x120) */
	{
		struct img_sink s;
		struct img_info in;

		memset(&o, 0, sizeof o);
		memset(&r, 0, sizeof r);
		r.f = &f;
		o.fmt = &f;
		o.want_w = 100;
		o.size = rec_size;
		o.row = rec_row;
		o.ctx = &r;
		px_sink(&o, &s);
		memset(&in, 0, sizeof in);
		in.w = in.full_w = 640;
		in.h = in.full_h = 480;
		in.scalable = 1;
		shift = -1;
		s.size(s.ctx, &in, &shift);
		CHECK(shift == 2 && r.w == 100 && r.h == 75, "JPEG scale: shift %d, %dx%d", shift, r.w, r.h);
		px_free(&o);
		rec_free(&r);
		/* shown at full size: no scaling; at 80 wide: 1/8 */
		r.f = &f;
		o.want_w = 0;
		px_sink(&o, &s);
		s.size(s.ctx, &in, &shift);
		CHECK(shift == 0, "JPEG at full size: shift %d", shift);
		px_free(&o);
		rec_free(&r);
		r.f = &f;
		o.want_w = 80;
		px_sink(&o, &s);
		s.size(s.ctx, &in, &shift);
		CHECK(shift == 3, "JPEG at 80 wide: shift %d", shift);
		px_free(&o);
		rec_free(&r);
	}
}

/* an interlaced GIF's row order: passes from 0 by 8, 4 by 8, 2 by 4, 1 by 2 */
static int gif_order(int h, int *order)
{
	static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
	int p, y, n = 0;

	for (p = 0; p < 4; p++)
		for (y = start[p]; y < h; y += step[p])
			order[n++] = y;
	return n;
}

static void test_order_and_mask(void)
{
	struct px_format f;
	struct px_out o;
	struct rec r, ref;
	unsigned char *src = malloc(8 * 16 * 4), m4[4 * 4];
	int order[16], c[3], d[3], x, y;

	fmt_true(&f, 32, 1, 0xff0000UL, 0xff00UL, 0xffUL);
	for (y = 0; y < 16; y++)
		for (x = 0; x < 8; x++) {
			unsigned char *p = src + (y * 8 + x) * 4;

			p[0] = (unsigned char)(y * 16);
			p[1] = (unsigned char)(x * 30);
			p[2] = 7;
			p[3] = 255;
		}
	/* interlaced, halved: target row t is source row 2t, each sent once */
	gif_order(16, order);
	memset(&o, 0, sizeof o);
	memset(&r, 0, sizeof r);
	o.fmt = &f;
	o.want_h = 8;
	o.want_w = 8;
	run(&o, &r, src, 8, 16, 0, 1, order, NULL);
	for (y = 0; y < 8; y++) {
		rgb_at(&r, 3, y, c);
		CHECK(r.sent[y] == 1 && c[0] == y * 32, "interlaced row %d: sent %d, r %d", y, r.sent[y], c[0]);
	}
	rec_free(&r);
	/* interlaced at full size: the same as in order */
	memset(&o, 0, sizeof o);
	o.fmt = &f;
	run(&o, &r, src, 8, 16, 0, 1, order, NULL);
	memset(&o, 0, sizeof o);
	memset(&ref, 0, sizeof ref);
	o.fmt = &f;
	run(&o, &ref, src, 8, 16, 0, 0, NULL, NULL);
	CHECK(memcmp(r.px, ref.px, ref.rb * 16) == 0, "interlaced, full size, as in order");
	rec_free(&r);
	rec_free(&ref);
	/* out of order without the flag: no crash, no row twice */
	memset(&o, 0, sizeof o);
	o.fmt = &f;
	o.want_h = 8;
	run(&o, &r, src, 8, 16, 0, 0, order, NULL);
	for (y = 0; y < 8; y++)
		CHECK(r.sent[y] <= 1, "unflagged out of order: row %d sent %d times", y, r.sent[y]);
	rec_free(&r);

	/* masks: 4x1, pixels opaque, clear, clear, clear -> 2x1: the first
	 * shown (half), in the opaque pixel's colour; the second not */
	memset(m4, 0, sizeof m4);
	m4[0] = 90;
	m4[1] = 80;
	m4[2] = 70;
	m4[3] = 255;
	m4[4] = 255;			/* (a clear pixel's colour: ignored) */
	memset(&o, 0, sizeof o);
	o.fmt = &f;
	o.want_w = 2;
	o.want_h = 1;
	run(&o, &r, m4, 4, 1, 1, 0, NULL, NULL);
	rgb_at(&r, 0, 0, c);
	CHECK(r.masked && mask_at(&r, 0, 0) == 1 && mask_at(&r, 1, 0) == 0,
		"mask after scaling: %d %d", mask_at(&r, 0, 0), mask_at(&r, 1, 0));
	CHECK(c[0] == 90 && c[1] == 80 && c[2] == 70, "shown pixel's colour: %d %d %d", c[0], c[1], c[2]);
	rec_free(&r);
	/* unscaled: alpha 127 hidden, 128 shown */
	m4[3] = 127;
	m4[7] = 128;
	memset(&o, 0, sizeof o);
	o.fmt = &f;
	run(&o, &r, m4, 2, 1, 1, 0, NULL, NULL);
	CHECK(mask_at(&r, 0, 0) == 0 && mask_at(&r, 1, 0) == 1, "alpha threshold");
	rgb_at(&r, 1, 0, d);
	rec_free(&r);
	free(src);
}

static void test_stop_and_rows(void)
{
	struct px_format f;
	struct px_out o;
	struct rec r;
	unsigned char *src;
	int i, y, bad = 0;
	unsigned long seed = 7;

	fmt_true(&f, 32, 1, 0xff0000UL, 0xff00UL, 0xffUL);
	src = flat(10, 10, 1, 2, 3, 255);
	memset(&o, 0, sizeof o);
	memset(&r, 0, sizeof r);
	o.fmt = &f;
	r.stop_after = 3;
	CHECK(run(&o, &r, src, 10, 10, 0, 0, NULL, NULL) == -1 && r.rows == 3, "stop after 3 rows: %d", r.rows);
	rec_free(&r);
	free(src);

	/* any sizes, in order or interlaced: every target row exactly once */
	for (i = 0; i < 3000; i++) {
		int sw, sh, il, order[64];

		seed = (seed * 1103515245UL + 12345UL) & 0xffffffffUL;
		sw = 1 + (int)((seed >> 8) % 40);
		sh = 1 + (int)((seed >> 16) % 60);
		il = (int)(seed >> 24) & 1;
		src = flat(sw, sh, 9, 8, 7, (seed >> 20 & 1) ? 255 : 100);
		memset(&o, 0, sizeof o);
		memset(&r, 0, sizeof r);
		o.fmt = &f;
		seed = (seed * 1103515245UL + 12345UL) & 0xffffffffUL;
		o.want_w = (int)((seed >> 8) % 70);
		o.want_h = (int)((seed >> 16) % 70);
		if (il)
			gif_order(sh, order);
		run(&o, &r, src, sw, sh, (int)(seed >> 26) & 1, il, il ? order : NULL, NULL);
		for (y = 0; y < r.h; y++)
			if (r.sent[y] != 1 && bad++ < 5)
				printf("  FAIL: %dx%d %s to %dx%d: row %d sent %d times\n", sw, sh,
					il ? "interlaced" : "in order", r.w, r.h, y, r.sent[y]);
		rec_free(&r);
		free(src);
	}
	checks++;
	if (bad)
		fails++;
}

int main(void)
{
	test_packing();
	test_dither();
	test_mono();
	test_scale();
	test_order_and_mask();
	test_stop_and_rows();
	printf("test_pixels: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
