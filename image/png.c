/*
 * png.c - PNG: every colour type and bit depth, tRNS transparency, and
 * Adam7 interlacing. The IDAT stream goes through Manx's own inflate as
 * it arrives; scanlines are unfiltered and sent as they complete. An
 * interlaced image needs all of itself before any row is whole, so it is
 * assembled in a buffer first (if it fits the budget).
 */
#include <string.h>
#include "inflate.h"
#include "image_int.h"

enum { P_SIG, P_CHUNK, P_DATA, P_IDAT, P_SKIP, P_CRC, P_DONE };

struct png {
	int state;
	unsigned long clen;		/* the chunk's length, or what's left */
	char ctype[5];
	int have_ihdr, started, ended;
	int w, h, depth, ctype_n, interlace;
	int channels, bpp;		/* samples per pixel; bytes per pixel (>= 1) */
	int npal;
	unsigned char pal[256 * 4];	/* RGBA */
	int has_trns;
	unsigned short trns_g, trns_r, trns_gr, trns_b;	/* key colour */
	struct inflate *z;
	/* scanlines */
	int pass;			/* Adam7: 0-6; else 0 */
	int pw, ph;			/* this pass's size */
	int y;				/* rows of this pass done */
	size_t bpl;			/* bytes in a scanline (not the filter) */
	unsigned char *cur, *prev;	/* 1 + bpl each */
	size_t line_alloc;		/* ... as allocated (the widest pass) */
	size_t have;			/* bytes of cur filled */
	unsigned char *rgba;		/* a row (w * 4) */
	unsigned char *full;		/* Adam7: the whole image (w * h * 4) */
	int rows_done;
};

static const int a7x0[7] = { 0, 4, 0, 2, 0, 1, 0 }, a7y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const int a7dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, a7dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

static unsigned long be32(const unsigned char *p)
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16
		| (unsigned long)p[2] << 8 | p[3];
}

int png_new(struct img_dec *d)
{
	struct png *p = img_alloc(d, sizeof *p);

	if (p == NULL)
		return -1;
	memset(p, 0, sizeof *p);
	d->fmt = p;
	return 0;
}

void png_free(struct img_dec *d)
{
	struct png *p = d->fmt;

	if (p->z)
		inflate_free(p->z);
	img_release(d, p->cur, p->line_alloc);
	img_release(d, p->prev, p->line_alloc);
	img_release(d, p->rgba, (size_t)p->w * 4);
	img_release(d, p->full, (size_t)p->w * (size_t)p->h * 4);
	img_release(d, p, sizeof *p);
	d->fmt = NULL;
}

/* the size of Adam7 pass k (or of the image) */
static void pass_size(const struct png *p, int k, int *pw, int *ph)
{
	if (!p->interlace) {
		*pw = p->w;
		*ph = p->h;
		return;
	}
	*pw = p->w > a7x0[k] ? (p->w - a7x0[k] + a7dx[k] - 1) / a7dx[k] : 0;
	*ph = p->h > a7y0[k] ? (p->h - a7y0[k] + a7dy[k] - 1) / a7dy[k] : 0;
}

/* start pass k (skipping empty ones); 0, or 1 when there are no more */
static int begin_pass(struct png *p, int k)
{
	for (; k < 7; k++) {
		pass_size(p, k, &p->pw, &p->ph);
		if (p->pw > 0 && p->ph > 0)
			break;
		if (!p->interlace)
			return 1;
	}
	if (k >= 7)
		return 1;
	p->pass = k;
	p->y = 0;
	p->bpl = ((size_t)p->pw * (size_t)p->channels * (size_t)p->depth + 7) / 8;
	memset(p->prev, 0, p->bpl + 1);
	p->have = 0;
	return 0;
}

static int paeth(int a, int b, int c)
{
	int pp = a + b - c, pa = pp > a ? pp - a : a - pp;
	int pb = pp > b ? pp - b : b - pp, pc = pp > c ? pp - c : c - pp;

	if (pa <= pb && pa <= pc)
		return a;
	return pb <= pc ? b : c;
}

static int unfilter(struct png *p)
{
	unsigned char *x = p->cur + 1, *u = p->prev + 1;
	size_t i, n = p->bpl;
	int bpp = p->bpp;

	switch (p->cur[0]) {
	case 0:
		break;
	case 1:
		for (i = (size_t)bpp; i < n; i++)
			x[i] = (unsigned char)(x[i] + x[i - bpp]);
		break;
	case 2:
		for (i = 0; i < n; i++)
			x[i] = (unsigned char)(x[i] + u[i]);
		break;
	case 3:
		for (i = 0; i < n; i++)
			x[i] = (unsigned char)(x[i] + ((i >= (size_t)bpp ? x[i - bpp]
				: 0) + u[i]) / 2);
		break;
	case 4:
		for (i = 0; i < n; i++)
			x[i] = (unsigned char)(x[i] + paeth(i >= (size_t)bpp ?
				x[i - bpp] : 0, u[i], i >= (size_t)bpp ? u[i - bpp] : 0));
		break;
	default:
		return -1;
	}
	return 0;
}

/* sample s (0-based) of the scanline, at the image's bit depth */
static unsigned sample(const struct png *p, const unsigned char *x, size_t s)
{
	switch (p->depth) {
	case 1: return (x[s >> 3] >> (7 - (s & 7))) & 1;
	case 2: return (x[s >> 2] >> (6 - 2 * (s & 3))) & 3;
	case 4: return (x[s >> 1] >> (4 - 4 * (s & 1))) & 15;
	case 8: return x[s];
	default: return (unsigned)x[2 * s] << 8 | x[2 * s + 1];
	}
}

/* a sample scaled to 8 bits */
static unsigned char to8(const struct png *p, unsigned v)
{
	switch (p->depth) {
	case 1: return (unsigned char)(v * 255);
	case 2: return (unsigned char)(v * 85);
	case 4: return (unsigned char)(v * 17);
	case 8: return (unsigned char)v;
	default: return (unsigned char)(v >> 8);
	}
}

/* pixel i of the scanline as RGBA */
static void pixel(const struct png *p, const unsigned char *x, int i,
	unsigned char *o)
{
	size_t s = (size_t)i * (size_t)p->channels;
	unsigned v, r, g, b;

	switch (p->ctype_n) {
	case 0:			/* grey */
		v = sample(p, x, s);
		o[0] = o[1] = o[2] = to8(p, v);
		o[3] = p->has_trns && v == p->trns_g ? 0 : 255;
		break;
	case 2:			/* RGB */
		r = sample(p, x, s);
		g = sample(p, x, s + 1);
		b = sample(p, x, s + 2);
		o[0] = to8(p, r);
		o[1] = to8(p, g);
		o[2] = to8(p, b);
		o[3] = p->has_trns && r == p->trns_r && g == p->trns_gr
			&& b == p->trns_b ? 0 : 255;
		break;
	case 3:			/* palette */
		v = sample(p, x, s);
		if ((int)v < p->npal)
			memcpy(o, p->pal + v * 4, 4);
		else
			o[0] = o[1] = o[2] = o[3] = 0;
		break;
	case 4:			/* grey and alpha */
		o[0] = o[1] = o[2] = to8(p, sample(p, x, s));
		o[3] = to8(p, sample(p, x, s + 1));
		break;
	default:		/* RGBA */
		o[0] = to8(p, sample(p, x, s));
		o[1] = to8(p, sample(p, x, s + 1));
		o[2] = to8(p, sample(p, x, s + 2));
		o[3] = to8(p, sample(p, x, s + 3));
	}
}

/* a scanline is whole: unfilter it and send it (or place its pixels) */
static int scanline(struct img_dec *d, struct png *p)
{
	unsigned char *t;
	int i;

	if (unfilter(p) < 0)
		return IMG_BAD;
	if (!p->interlace) {
		for (i = 0; i < p->w; i++)
			pixel(p, p->cur + 1, i, p->rgba + i * 4);
		if (d->sink.row(d->sink.ctx, p->y, p->rgba) < 0)
			return IMG_STOP;
		p->rows_done++;
	} else {
		int yy = a7y0[p->pass] + p->y * a7dy[p->pass];
		unsigned char *row = p->full + (size_t)yy * (size_t)p->w * 4;

		for (i = 0; i < p->pw; i++)
			pixel(p, p->cur + 1, i,
				row + (size_t)(a7x0[p->pass] + i * a7dx[p->pass]) * 4);
	}
	t = p->prev;
	p->prev = p->cur;
	p->cur = t;
	p->have = 0;
	if (++p->y < p->ph)
		return IMG_OK;
	if (begin_pass(p, p->interlace ? p->pass + 1 : 7)) {
		/* all the image is here */
		p->ended = 1;
		if (p->interlace)
			for (i = 0; i < p->h; i++) {
				if (d->sink.row(d->sink.ctx, i,
					p->full + (size_t)i * (size_t)p->w * 4) < 0)
					return IMG_STOP;
				p->rows_done++;
			}
	}
	return IMG_OK;
}

/* inflate's output: scanline bytes */
static int on_inflated(void *ctx, const unsigned char *b, size_t n)
{
	struct img_dec *d = ctx;
	struct png *p = d->fmt;

	while (n > 0 && !p->ended) {
		size_t k = p->bpl + 1 - p->have;

		if (k > n)
			k = n;
		memcpy(p->cur + p->have, b, k);
		p->have += k;
		b += k;
		n -= k;
		if (p->have == p->bpl + 1) {
			int r = scanline(d, p);

			if (r != IMG_OK) {
				d->result = r;
				return -1;
			}
		}
	}
	return 0;
}

/* the first IDAT: everything about the image is known */
static int start_image(struct img_dec *d, struct png *p)
{
	struct img_info in;
	int shift = 0, i;
	size_t line;

	if (!p->have_ihdr || (p->ctype_n == 3 && p->npal == 0))
		return IMG_BAD;
	memset(&in, 0, sizeof in);
	in.w = in.full_w = p->w;
	in.h = in.full_h = p->h;
	in.alpha = p->ctype_n == 4 || p->ctype_n == 6 || p->has_trns;
	if (p->ctype_n == 3)
		for (i = 0; i < p->npal; i++)
			if (p->pal[i * 4 + 3] < 255)
				in.alpha = 1;
	line = ((size_t)p->w * (size_t)p->channels * (size_t)p->depth + 7) / 8 + 1;
	if ((p->cur = img_alloc(d, line)) == NULL
		|| (p->prev = img_alloc(d, line)) == NULL
		|| (p->rgba = img_alloc(d, (size_t)p->w * 4)) == NULL)
		return IMG_TOOBIG;
	p->bpl = line - 1;
	p->line_alloc = line;
	if (p->interlace) {
		if ((size_t)p->w * (size_t)p->h > (d->cap - d->used) / 4
			|| (p->full = img_alloc(d, (size_t)p->w * (size_t)p->h * 4)) == NULL)
			return IMG_TOOBIG;
		memset(p->full, 0, (size_t)p->w * (size_t)p->h * 4);
	}
	if ((p->z = inflate_new(INF_DEFLATE, on_inflated, d)) == NULL)
		return IMG_TOOBIG;
	if (begin_pass(p, 0)) {
		p->ended = 1;
		return IMG_OK;
	}
	/* (begin_pass sized bpl for the pass; the buffers fit the widest) */
	p->started = 1;
	return d->sink.size(d->sink.ctx, &in, &shift) < 0 ? IMG_STOP : IMG_OK;
}

static int read_ihdr(struct png *p, const unsigned char *c, unsigned long n)
{
	static const int chans[7] = { 1, 0, 3, 1, 2, 0, 4 };

	if (n != 13)
		return IMG_BAD;
	p->w = (int)(be32(c) > IMG_MAX_SIDE ? 0 : be32(c));
	p->h = (int)(be32(c + 4) > IMG_MAX_SIDE ? 0 : be32(c + 4));
	p->depth = c[8];
	p->ctype_n = c[9];
	p->interlace = c[12];
	if (be32(c) > IMG_MAX_SIDE || be32(c + 4) > IMG_MAX_SIDE)
		return IMG_TOOBIG;
	if (p->w <= 0 || p->h <= 0 || p->ctype_n > 6 || chans[p->ctype_n] == 0
		|| c[10] || c[11] || p->interlace > 1)
		return IMG_BAD;
	switch (p->depth) {
	case 1: case 2: case 4:
		if (p->ctype_n != 0 && p->ctype_n != 3)
			return IMG_BAD;
		break;
	case 8:
		break;
	case 16:
		if (p->ctype_n == 3)
			return IMG_BAD;
		break;
	default:
		return IMG_BAD;
	}
	p->channels = chans[p->ctype_n];
	p->bpp = (p->channels * p->depth + 7) / 8;
	p->have_ihdr = 1;
	return IMG_OK;
}

static void read_plte(struct png *p, const unsigned char *c, unsigned long n)
{
	int i;

	p->npal = (int)(n / 3 > 256 ? 256 : n / 3);
	for (i = 0; i < p->npal; i++) {
		p->pal[i * 4] = c[i * 3];
		p->pal[i * 4 + 1] = c[i * 3 + 1];
		p->pal[i * 4 + 2] = c[i * 3 + 2];
		p->pal[i * 4 + 3] = 255;
	}
}

static void read_trns(struct png *p, const unsigned char *c, unsigned long n)
{
	unsigned long i;

	switch (p->ctype_n) {
	case 3:
		for (i = 0; i < n && i < 256; i++)
			p->pal[i * 4 + 3] = c[i];
		break;
	case 0:
		if (n >= 2) {
			p->trns_g = (unsigned short)(c[0] << 8 | c[1]);
			p->has_trns = 1;
		}
		break;
	case 2:
		if (n >= 6) {
			p->trns_r = (unsigned short)(c[0] << 8 | c[1]);
			p->trns_gr = (unsigned short)(c[2] << 8 | c[3]);
			p->trns_b = (unsigned short)(c[4] << 8 | c[5]);
			p->has_trns = 1;
		}
		break;
	}
}

/* the IDAT stream is over: inflate gives what it still holds (it sends
 * its output a window at a time, so a small image's rows come only now) */
static int flush(struct img_dec *d, struct png *p)
{
	if (p->z == NULL || p->ended)
		return IMG_OK;
	inflate_finish(p->z);
	inflate_free(p->z);
	p->z = NULL;
	return d->result != IMG_OK ? d->result : IMG_OK;
}

int png_feed(struct img_dec *d, const unsigned char *b, size_t n)
{
	struct png *p = d->fmt;
	const unsigned char *c;
	int r;

	while (n > 0) {
		switch (p->state) {
		case P_SIG:
			if ((c = img_gather(d, &b, &n, 8)) == NULL)
				break;
			if (img_sniff(c, 8) != IMG_PNG)
				return IMG_BAD;
			p->state = P_CHUNK;
			break;
		case P_CHUNK:
			if ((c = img_gather(d, &b, &n, 8)) == NULL)
				break;
			p->clen = be32(c);
			memcpy(p->ctype, c + 4, 4);
			p->ctype[4] = '\0';
			if (p->clen > 0x7FFFFFFFUL)
				return IMG_BAD;
			if (strcmp(p->ctype, "IDAT") == 0) {
				if (!p->started && !p->ended
					&& (r = start_image(d, p)) != IMG_OK)
					return r;
				p->state = P_IDAT;
			} else if (strcmp(p->ctype, "IEND") == 0) {
				p->state = P_DONE;
				if ((r = flush(d, p)) != IMG_OK)
					return r;
				return p->ended ? IMG_END : IMG_BAD;
			} else if (strcmp(p->ctype, "IHDR") == 0
				|| strcmp(p->ctype, "PLTE") == 0
				|| strcmp(p->ctype, "tRNS") == 0) {
				if (p->clen > 1024)
					return IMG_BAD;
				p->state = P_DATA;
			} else
				p->state = P_SKIP;
			if (p->clen == 0 && p->state != P_DATA)
				p->state = P_CRC;
			break;
		case P_DATA:
			if ((c = img_gather(d, &b, &n, (size_t)p->clen)) == NULL)
				break;
			if (strcmp(p->ctype, "IHDR") == 0) {
				if ((r = read_ihdr(p, c, p->clen)) != IMG_OK)
					return r;
			} else if (strcmp(p->ctype, "PLTE") == 0)
				read_plte(p, c, p->clen);
			else
				read_trns(p, c, p->clen);
			p->state = P_CRC;
			break;
		case P_IDAT: {
			size_t k = p->clen < n ? (size_t)p->clen : n;

			if (!p->ended) {
				r = inflate_feed(p->z, b, k);
				if (d->result != IMG_OK)
					return d->result;	/* set by a scanline */
				if (r == INF_BAD)
					return IMG_BAD;
			}
			b += k;
			n -= k;
			p->clen -= k;
			if (p->clen == 0)
				p->state = P_CRC;
			break;
		}
		case P_SKIP: {
			size_t k = p->clen < n ? (size_t)p->clen : n;

			b += k;
			n -= k;
			p->clen -= k;
			if (p->clen == 0)
				p->state = P_CRC;
			break;
		}
		case P_CRC:
			/* (not checked: zlib's Adler-32 checks the image) */
			if ((c = img_gather(d, &b, &n, 4)) == NULL)
				break;
			p->state = P_CHUNK;
			break;
		case P_DONE:
			return IMG_END;
		}
		if (d->want && n == 0)
			break;
		if (d->want && d->hold_cap < d->want && n > 0)
			return IMG_TOOBIG;
	}
	return IMG_OK;
}

int png_finish(struct img_dec *d)
{
	struct png *p = d->fmt;
	int r;

	/* every row came, even if IEND didn't */
	if ((r = flush(d, p)) != IMG_OK)
		return r;
	return p->ended ? IMG_END : IMG_BAD;
}
