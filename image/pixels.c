/*
 * pixels.c - the sink between a decoder and the screen (pixels.h).
 *
 * Scaling is a box filter. Target pixel x averages source pixels xs[x]
 * up to xs[x + 1] (at least one), and target row t the source rows from
 * ys(t) = t * sh / th up to ys(t + 1); growing, a source pixel is
 * repeated. Rows normally come in order, and a target row is averaged as
 * its source rows arrive. An interlaced GIF's come out of order: then each
 * target row is the one source row ys(t), sent as soon as that comes.
 *
 * Transparency is all or nothing (it becomes an X mask): a target pixel
 * is shown when at least half of the source pixels it covers are, in the
 * average colour of those.
 *
 * Colours with fewer than 8 bits a channel (a colour cube, greys, 5- or
 * 6-bit TrueColor) are dithered with an 8x8 ordered (Bayer) matrix. That
 * needs nothing carried from row to row, so rows may come in any order,
 * and costs a table lookup and a comparison a channel.
 */
#include <string.h>
#include "os.h"
#include "pixels.h"

#define PX_MAX_SIDE	8192

/* thresholds 0-63, each once in every 8x8 tile */
static const unsigned char bayer[8][8] = {
	{ 0, 32, 8, 40, 2, 34, 10, 42 },
	{ 48, 16, 56, 24, 50, 18, 58, 26 },
	{ 12, 44, 4, 36, 14, 46, 6, 38 },
	{ 60, 28, 52, 20, 62, 30, 54, 22 },
	{ 3, 35, 11, 43, 1, 33, 9, 41 },
	{ 51, 19, 59, 27, 49, 17, 57, 25 },
	{ 15, 47, 7, 39, 13, 45, 5, 37 },
	{ 63, 31, 55, 23, 61, 29, 53, 21 }
};

/*
 * One channel (or the grey): v adds base[c], and step more when frac[c],
 * how far c is above that level in 64ths, beats the pixel's threshold.
 * A channel of 8 bits or more has frac 0: no dither.
 */
struct chan {
	unsigned long base[256];
	unsigned char frac[256];
	unsigned long step;
};

struct px_state {
	int sw, sh, tw, th;	/* source (as decoded) and target sizes */
	int masked;
	int nearest;		/* each target row from one source row */
	int ty;			/* in order: the target row being gathered */
	int rows;		/* and how many source rows are in acc */
	int next_y;		/* the source row expected next */
	long *xs;		/* tw + 1 */
	unsigned long *acc;	/* tw * 4: r, g, b of the pixels shown, and
				 * how many were */
	unsigned char *line;	/* a target row, RGBA */
	unsigned long *pv;	/* its pixel values */
	unsigned char *px, *mask;
	struct chan ch[3];	/* PX_GRAY: ch[0] */
	unsigned short lum[3][256];	/* PX_GRAY: r, g, b's share of grey */
};

static unsigned long recip[257];	/* 65536 / n, rounded */

/* s / n, rounded (n >= 1; s at most 255 n) */
static unsigned avg(unsigned long s, unsigned long n)
{
	if (n == 1)
		return (unsigned)s;
	if (n <= 256)
		return (unsigned)((s * recip[n] + 32768UL) >> 16);
	return (unsigned)((s + n / 2) / n);
}

size_t px_row_bytes(const struct px_format *f, int w)
{
	return f->bpp == 1 ? ((size_t)w + 7) / 8 : (size_t)w * (size_t)(f->bpp / 8);
}

/* levels steps (2-256) of value step each: the dithered quantizer */
static void quantizer(struct chan *c, int levels, unsigned long step)
{
	int i;

	c->step = step;
	for (i = 0; i < 256; i++) {
		long v = (long)i * (levels - 1);

		c->base[i] = (unsigned long)(v / 255) * step;
		c->frac[i] = (unsigned char)(v % 255 * 64 / 255);
	}
}

/* a TrueColor channel in mask m */
static void true_chan(struct chan *c, unsigned long m)
{
	int shift = 0, bits = 0, i;

	if (m == 0) {
		memset(c, 0, sizeof *c);
		return;
	}
	while (!(m >> shift & 1))
		shift++;
	while (shift + bits < 32 && (m >> (shift + bits) & 1))
		bits++;
	if (bits < 8) {
		quantizer(c, 1 << bits, 1UL << shift);
		return;
	}
	c->step = 0;
	for (i = 0; i < 256; i++) {
		unsigned long v = (unsigned long)i << (bits - 8);

		/* (more bits: repeat the top ones below, 0xff -> all ones) */
		if (bits > 8)
			v |= (unsigned long)i >> (16 - bits > 0 ? 16 - bits : 0);
		c->base[i] = (v << shift) & m;
		c->frac[i] = 0;
	}
}

static void tables(struct px_state *st, const struct px_format *f)
{
	int i;

	if (recip[1] == 0)
		for (i = 1; i <= 256; i++)
			recip[i] = (65536UL + (unsigned long)i / 2) / (unsigned long)i;
	switch (f->kind) {
	case PX_TRUE:
		for (i = 0; i < 3; i++)
			true_chan(&st->ch[i], f->mask[i]);
		break;
	case PX_CUBE:
		quantizer(&st->ch[0], f->levels[0], (unsigned long)(f->levels[1] * f->levels[2]));
		quantizer(&st->ch[1], f->levels[1], (unsigned long)f->levels[2]);
		quantizer(&st->ch[2], f->levels[2], 1);
		break;
	case PX_GRAY:
		quantizer(&st->ch[0], f->levels[0], 1);
		for (i = 0; i < 256; i++) {
			/* ITU-R 601 weights, in 256ths (they sum to 256) */
			st->lum[0][i] = (unsigned short)(i * 77);
			st->lum[1][i] = (unsigned short)(i * 150);
			st->lum[2][i] = (unsigned short)(i * 29);
		}
		break;
	}
}

/* --- out to the caller --------------------------------------------------- */

/* line (target row t, RGBA) to pixels, packed, and sent */
static int send(struct px_out *o, struct px_state *st, int t, const unsigned char *line)
{
	const struct px_format *f = o->fmt;
	const unsigned char *d = bayer[t & 7];
	const struct chan *r = &st->ch[0], *g = &st->ch[1], *b = &st->ch[2];
	unsigned long *pv = st->pv;
	unsigned char *q;
	int x, w = st->tw;

	if (st->masked) {
		memset(st->mask, 0, ((size_t)w + 7) / 8);
		for (x = 0; x < w; x++)
			if (line[x * 4 + 3] >= 128)
				st->mask[x >> 3] |= (unsigned char)(f->bit_msb ? 0x80 >> (x & 7) : 1 << (x & 7));
	}
	switch (f->kind) {
	case PX_TRUE:
		for (x = 0; x < w; x++, line += 4) {
			int k = d[x & 7];

			pv[x] = r->base[line[0]] + (r->frac[line[0]] > k ? r->step : 0)
				+ g->base[line[1]] + (g->frac[line[1]] > k ? g->step : 0)
				+ b->base[line[2]] + (b->frac[line[2]] > k ? b->step : 0);
		}
		break;
	case PX_CUBE:
		for (x = 0; x < w; x++, line += 4) {
			int k = d[x & 7];

			pv[x] = f->pixel[r->base[line[0]] + (r->frac[line[0]] > k ? r->step : 0)
				+ g->base[line[1]] + (g->frac[line[1]] > k ? g->step : 0)
				+ b->base[line[2]] + (b->frac[line[2]] > k ? b->step : 0)];
		}
		break;
	case PX_GRAY:
		for (x = 0; x < w; x++, line += 4) {
			int k = d[x & 7];
			unsigned y = (unsigned)(st->lum[0][line[0]] + st->lum[1][line[1]]
				+ st->lum[2][line[2]] + 128) >> 8;

			pv[x] = f->pixel[r->base[y] + (r->frac[y] > k ? r->step : 0)];
		}
		break;
	}
	q = st->px;
	switch (f->bpp) {
	case 1:
		memset(q, 0, ((size_t)w + 7) / 8);
		for (x = 0; x < w; x++)
			if (pv[x] & 1)
				q[x >> 3] |= (unsigned char)(f->bit_msb ? 0x80 >> (x & 7) : 1 << (x & 7));
		break;
	case 8:
		for (x = 0; x < w; x++)
			q[x] = (unsigned char)pv[x];
		break;
	case 16:
		for (x = 0; x < w; x++, q += 2) {
			q[!f->byte_msb] = (unsigned char)(pv[x] >> 8);
			q[f->byte_msb] = (unsigned char)pv[x];
		}
		break;
	case 24:
		for (x = 0; x < w; x++, q += 3) {
			q[f->byte_msb ? 0 : 2] = (unsigned char)(pv[x] >> 16);
			q[1] = (unsigned char)(pv[x] >> 8);
			q[f->byte_msb ? 2 : 0] = (unsigned char)pv[x];
		}
		break;
	default:
		for (x = 0; x < w; x++, q += 4)
			if (f->byte_msb) {
				q[0] = (unsigned char)(pv[x] >> 24);
				q[1] = (unsigned char)(pv[x] >> 16);
				q[2] = (unsigned char)(pv[x] >> 8);
				q[3] = (unsigned char)pv[x];
			} else {
				q[3] = (unsigned char)(pv[x] >> 24);
				q[2] = (unsigned char)(pv[x] >> 16);
				q[1] = (unsigned char)(pv[x] >> 8);
				q[0] = (unsigned char)pv[x];
			}
	}
	return o->row(o->ctx, t, st->px, st->masked ? st->mask : NULL);
}

/* --- scaling ---------------------------------------------------------------- */

static long ys(const struct px_state *st, int t)
{
	return (long)t * st->sh / st->th;
}

/* add source row rgba into acc, each target pixel's share */
static void gather(struct px_state *st, const unsigned char *rgba)
{
	unsigned long *a = st->acc;
	int x;

	for (x = 0; x < st->tw; x++, a += 4) {
		long i = st->xs[x], e = st->xs[x + 1] > i ? st->xs[x + 1] : i + 1;
		const unsigned char *p = rgba + i * 4;

		for (; i < e; i++, p += 4)
			if (!st->masked || p[3] >= 128) {
				a[0] += p[0];
				a[1] += p[1];
				a[2] += p[2];
				a[3]++;
			}
	}
}

/* acc, over rows source rows, into line; acc cleared */
static void average(struct px_state *st, int rows)
{
	unsigned long *a = st->acc;
	unsigned char *l = st->line;
	int x;

	for (x = 0; x < st->tw; x++, a += 4, l += 4) {
		long nx = st->xs[x + 1] > st->xs[x] ? st->xs[x + 1] - st->xs[x] : 1;
		unsigned long n = a[3];

		if (n == 0 || n * 2 < (unsigned long)(nx * rows)) {
			l[0] = l[1] = l[2] = l[3] = 0;
		} else {
			l[0] = (unsigned char)avg(a[0], n);
			l[1] = (unsigned char)avg(a[1], n);
			l[2] = (unsigned char)avg(a[2], n);
			l[3] = 255;
		}
		a[0] = a[1] = a[2] = a[3] = 0;
	}
}

/* source row y alone: the target rows t with ys(t) == y */
static int nearest(struct px_out *o, struct px_state *st, int y, const unsigned char *rgba)
{
	int t = (int)(((long)y * st->th + st->sh - 1) / st->sh);
	const unsigned char *line = rgba;

	if (t >= st->th || ys(st, t) != y)
		return 0;		/* (shrinking: a row not used) */
	if (st->tw != st->sw) {
		gather(st, rgba);
		average(st, 1);
		line = st->line;
	}
	for (; t < st->th && ys(st, t) == y; t++)
		if (send(o, st, t, line) < 0)
			return -1;
	return 0;
}

static int on_row(void *ctx, int y, const unsigned char *rgba)
{
	struct px_out *o = ctx;
	struct px_state *st = o->st;

	if (st == NULL || y < 0 || y >= st->sh)
		return 0;
	if (!st->nearest && y != st->next_y) {
		/* rows out of order after all (or some missing): send what
		 * is gathered, then take each row as it comes */
		if (st->rows > 0) {
			average(st, st->rows);
			if (send(o, st, st->ty, st->line) < 0)
				return -1;
		}
		st->nearest = 1;
	}
	if (st->nearest)
		return nearest(o, st, y, rgba);
	st->next_y = y + 1;
	gather(st, rgba);
	st->rows++;
	if (st->next_y >= ys(st, st->ty + 1)) {
		int t = st->ty++, rows = st->rows;

		st->rows = 0;
		average(st, rows);
		return send(o, st, t, st->line);
	}
	return 0;
}

/* --- the size ---------------------------------------------------------------- */

static int px_format_ok(const struct px_format *f)
{
	if (f->bpp != 1 && f->bpp != 8 && f->bpp != 16 && f->bpp != 24 && f->bpp != 32)
		return 0;
	switch (f->kind) {
	case PX_TRUE:
		return 1;
	case PX_CUBE:
		return f->levels[0] >= 2 && f->levels[1] >= 2 && f->levels[2] >= 2
			&& f->levels[0] * f->levels[1] * f->levels[2] <= 256;
	case PX_GRAY:
		return f->levels[0] >= 2 && f->levels[0] <= 256;
	}
	return 0;
}

static int on_size(void *ctx, struct img_info *in, int *shift)
{
	struct px_out *o = ctx;
	struct px_state *st;
	long fw = in->full_w, fh = in->full_h, tw, th;
	int s = 0, x;

	if (fw <= 0 || fh <= 0 || o->st != NULL || !px_format_ok(o->fmt))
		return -1;
	tw = o->want_w > 0 ? o->want_w : 0;
	th = o->want_h > 0 ? o->want_h : 0;
	if (tw == 0 && th == 0) {
		tw = fw;
		th = fh;
	} else if (th == 0)
		th = (fh * tw + fw / 2) / fw;
	else if (tw == 0)
		tw = (fw * th + fh / 2) / fh;
	if (tw > PX_MAX_SIDE)
		tw = PX_MAX_SIDE;
	if (th > PX_MAX_SIDE)
		th = PX_MAX_SIDE;
	if (o->max_w > 0 && tw > o->max_w) {
		th = (th * o->max_w + tw / 2) / tw;
		tw = o->max_w;
	}
	if (o->max_h > 0 && th > o->max_h) {
		tw = (tw * o->max_h + th / 2) / th;
		th = o->max_h;
	}
	if (tw < 1)
		tw = 1;
	if (th < 1)
		th = 1;
	/* a JPEG decodes at the smallest of 1/2, 1/4, 1/8 still no smaller
	 * than what is shown (the box filter does the rest) */
	if (in->scalable)
		while (s < 3 && ((fw + (2L << s) - 1) >> (s + 1)) >= tw
			&& ((fh + (2L << s) - 1) >> (s + 1)) >= th)
			s++;
	*shift = s;

	if ((st = xmalloc(sizeof *st)) == NULL)
		return -1;
	memset(st, 0, sizeof *st);
	st->sw = (int)((fw + (1L << s) - 1) >> s);
	st->sh = (int)((fh + (1L << s) - 1) >> s);
	st->tw = (int)tw;
	st->th = (int)th;
	st->masked = in->alpha != 0;
	st->nearest = in->interlaced || st->th >= st->sh;
	st->xs = xmalloc(((size_t)tw + 1) * sizeof *st->xs);
	st->acc = xmalloc((size_t)tw * 4 * sizeof *st->acc);
	st->line = xmalloc((size_t)tw * 4);
	st->pv = xmalloc((size_t)tw * sizeof *st->pv);
	st->px = xmalloc(px_row_bytes(o->fmt, (int)tw) + 4);
	st->mask = xmalloc(((size_t)tw + 7) / 8);
	o->st = st;
	if (!st->xs || !st->acc || !st->line || !st->pv || !st->px || !st->mask)
		return -1;
	for (x = 0; x <= tw; x++)
		st->xs[x] = (long)x * st->sw / tw;
	memset(st->acc, 0, (size_t)tw * 4 * sizeof *st->acc);
	tables(st, o->fmt);
	return o->size(o->ctx, (int)tw, (int)th, st->masked);
}

void px_sink(struct px_out *o, struct img_sink *s)
{
	o->st = NULL;
	s->size = on_size;
	s->row = on_row;
	s->ctx = o;
}

int px_finish(struct px_out *o)
{
	struct px_state *st = o->st;
	int rows;

	if (st == NULL || st->nearest || st->rows == 0)
		return 0;
	rows = st->rows;
	st->rows = 0;
	average(st, rows);
	return send(o, st, st->ty++, st->line);
}

void px_free(struct px_out *o)
{
	struct px_state *st = o->st;

	if (st == NULL)
		return;
	xfree(st->xs);
	xfree(st->acc);
	xfree(st->line);
	xfree(st->pv);
	xfree(st->px);
	xfree(st->mask);
	xfree(st);
	o->st = NULL;
}
