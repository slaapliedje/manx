/*
 * gif.c - GIF87a/89a, the first frame: LZW decoded as the bytes come,
 * rows out as they fill, in the order an interlaced image sends them.
 * A frame smaller than the screen is placed on a transparent one.
 */
#include <string.h>
#include "image_int.h"

enum {
	G_HEADER, G_GCT, G_BLOCK, G_EXT_LABEL, G_SUBLEN, G_SUBDATA,
	G_IMGDESC, G_LCT, G_LZWMIN, G_DATALEN, G_DATA, G_DONE
};

struct gif {
	int state;
	int sw, sh;			/* the logical screen */
	int gct_n, lct_n;		/* colours in the tables (0: none) */
	unsigned char gct[768], lct[768];
	int ext_label, gce_seen, transp;	/* transp: index, or -1 */
	int sub_len;			/* bytes left in this sub-block */
	/* the frame */
	int fx, fy, fw, fh, interlaced;
	int w, h;			/* what the sink gets */
	int fy_out;			/* frame rows done (in sending order) */
	int col;			/* pixels in the current row */
	unsigned char *idx;		/* the row's colour indexes (fw) */
	unsigned char *rgba;		/* the row as sent (w * 4) */
	/* LZW */
	int min, size, clear, eoi, next, old, first;
	unsigned long acc;
	int nbits;
	unsigned short prefix[4096];
	unsigned char suffix[4096];
	unsigned char stack[4097];
	int lzw_end;			/* the end code came */
};

int gif_new(struct img_dec *d)
{
	struct gif *g = img_alloc(d, sizeof *g);

	if (g == NULL)
		return -1;
	memset(g, 0, sizeof *g);
	g->transp = -1;
	d->fmt = g;
	return 0;
}

void gif_free(struct img_dec *d)
{
	struct gif *g = d->fmt;

	if (g->idx)
		img_release(d, g->idx, (size_t)g->fw);
	if (g->rgba)
		img_release(d, g->rgba, (size_t)g->w * 4);
	img_release(d, g, sizeof *g);
	d->fmt = NULL;
}

/* the frame row the n-th row sent is (interlaced: passes of 8, 8, 4, 2) */
static int frame_row(const struct gif *g, int n)
{
	static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
	int p, c;

	if (!g->interlaced)
		return n;
	for (p = 0; p < 4; p++) {
		c = (g->fh - start[p] + step[p] - 1) / step[p];
		if (c < 0)
			c = 0;
		if (n < c)
			return start[p] + n * step[p];
		n -= c;
	}
	return -1;
}

/* the row of indexes is full: send it */
static int emit_row(struct img_dec *d, struct gif *g)
{
	const unsigned char *pal = g->lct_n ? g->lct : g->gct;
	int npal = g->lct_n ? g->lct_n : g->gct_n, x, y;

	y = g->fy + frame_row(g, g->fy_out++);
	g->col = 0;
	if (y < g->fy || y >= g->h)
		return 0;
	memset(g->rgba, 0, (size_t)g->w * 4);
	for (x = 0; x < g->fw && g->fx + x < g->w; x++) {
		int i = g->idx[x];
		unsigned char *o = g->rgba + (g->fx + x) * 4;

		if (i == g->transp || i >= npal)
			continue;	/* transparent (alpha 0) */
		o[0] = pal[i * 3];
		o[1] = pal[i * 3 + 1];
		o[2] = pal[i * 3 + 2];
		o[3] = 255;
	}
	return d->sink.row(d->sink.ctx, y, g->rgba) < 0 ? -1 : 0;
}

/* one decoded pixel */
static int put_px(struct img_dec *d, struct gif *g, int v)
{
	if (g->fy_out >= g->fh)
		return 0;		/* past the frame: ignored */
	g->idx[g->col++] = (unsigned char)v;
	if (g->col == g->fw)
		return emit_row(d, g);
	return 0;
}

static void lzw_reset(struct gif *g)
{
	g->size = g->min + 1;
	g->next = g->eoi + 1;
	g->old = -1;
}

/* one code: IMG_OK, IMG_BAD or IMG_STOP */
static int lzw_code(struct img_dec *d, struct gif *g, int code)
{
	int sp = 0, in;

	if (code == g->clear) {
		lzw_reset(g);
		return IMG_OK;
	}
	if (code == g->eoi) {
		g->lzw_end = 1;
		return IMG_OK;
	}
	if (g->old < 0) {
		if (code >= g->clear)
			return IMG_BAD;
		g->old = g->first = code;
		return put_px(d, g, code) < 0 ? IMG_STOP : IMG_OK;
	}
	in = code;
	if (code > g->next)
		return IMG_BAD;
	if (code == g->next) {
		/* the string not yet in the table: old's, and its first */
		g->stack[sp++] = (unsigned char)g->first;
		code = g->old;
	}
	while (code >= g->clear) {
		if (sp >= 4096 || code >= g->next)
			return IMG_BAD;
		g->stack[sp++] = g->suffix[code];
		code = g->prefix[code];
	}
	g->first = code;
	g->stack[sp++] = (unsigned char)code;
	if (g->next < 4096) {
		g->prefix[g->next] = (unsigned short)g->old;
		g->suffix[g->next] = (unsigned char)g->first;
		g->next++;
		if (g->next == (1 << g->size) && g->size < 12)
			g->size++;
	}
	g->old = in;
	while (sp > 0)
		if (put_px(d, g, g->stack[--sp]) < 0)
			return IMG_STOP;
	return IMG_OK;
}

static int lzw_bytes(struct img_dec *d, struct gif *g, const unsigned char *b,
	size_t n)
{
	size_t i;

	for (i = 0; i < n && !g->lzw_end; i++) {
		g->acc |= (unsigned long)b[i] << g->nbits;
		g->nbits += 8;
		while (g->nbits >= g->size && !g->lzw_end) {
			int code = (int)(g->acc & ((1UL << g->size) - 1)), r;

			g->acc >>= g->size;
			g->nbits -= g->size;
			if ((r = lzw_code(d, g, code)) != IMG_OK)
				return r;
		}
	}
	return IMG_OK;
}

/* the image descriptor came: the sink learns the size */
static int frame_begin(struct img_dec *d, struct gif *g)
{
	struct img_info in;
	int shift = 0;

	g->w = g->sw > g->fx + g->fw ? g->sw : g->fx + g->fw;
	g->h = g->sh > g->fy + g->fh ? g->sh : g->fy + g->fh;
	if (g->fw <= 0 || g->fh <= 0 || g->w > IMG_MAX_SIDE || g->h > IMG_MAX_SIDE)
		return g->fw <= 0 || g->fh <= 0 ? IMG_BAD : IMG_TOOBIG;
	if ((g->idx = img_alloc(d, (size_t)g->fw)) == NULL
		|| (g->rgba = img_alloc(d, (size_t)g->w * 4)) == NULL) {
		if (g->idx && !g->rgba) {
			img_release(d, g->idx, (size_t)g->fw);
			g->idx = NULL;
		}
		return IMG_TOOBIG;
	}
	memset(&in, 0, sizeof in);
	in.w = in.full_w = g->w;
	in.h = in.full_h = g->h;
	in.alpha = g->transp >= 0 || g->fx || g->fy || g->fw < g->w || g->fh < g->h;
	in.interlaced = g->interlaced;
	return d->sink.size(d->sink.ctx, &in, &shift) < 0 ? IMG_STOP : IMG_OK;
}

int gif_feed(struct img_dec *d, const unsigned char *b, size_t n)
{
	struct gif *g = d->fmt;
	const unsigned char *p;
	int r;

	while (n > 0) {
		switch (g->state) {
		case G_HEADER:
			if ((p = img_gather(d, &b, &n, 13)) == NULL)
				break;
			if (img_sniff(p, 6) != IMG_GIF)
				return IMG_BAD;
			g->sw = p[6] | p[7] << 8;
			g->sh = p[8] | p[9] << 8;
			if (p[10] & 0x80) {
				g->gct_n = 2 << (p[10] & 7);
				g->state = G_GCT;
			} else
				g->state = G_BLOCK;
			break;
		case G_GCT:
			if ((p = img_gather(d, &b, &n, (size_t)g->gct_n * 3)) == NULL)
				break;
			memcpy(g->gct, p, (size_t)g->gct_n * 3);
			g->state = G_BLOCK;
			break;
		case G_BLOCK:
			switch (*b) {
			case 0x21:
				g->state = G_EXT_LABEL;
				break;
			case 0x2C:
				g->state = G_IMGDESC;
				break;
			case 0x3B:
				return IMG_BAD;	/* the trailer, and no image */
			default:
				return IMG_BAD;
			}
			b++;
			n--;
			break;
		case G_EXT_LABEL:
			g->ext_label = *b++;
			n--;
			g->state = G_SUBLEN;
			break;
		case G_SUBLEN:
			g->sub_len = *b++;
			n--;
			g->state = g->sub_len ? G_SUBDATA : G_BLOCK;
			break;
		case G_SUBDATA:
			if (g->ext_label == 0xF9 && !g->gce_seen && g->sub_len >= 4) {
				/* the graphic control extension: transparency */
				if ((p = img_gather(d, &b, &n, (size_t)g->sub_len)) == NULL)
					break;
				g->gce_seen = 1;
				if (p[0] & 1)
					g->transp = p[3];
				g->sub_len = 0;
			} else {
				size_t k = (size_t)g->sub_len < n ? (size_t)g->sub_len : n;

				b += k;
				n -= k;
				g->sub_len -= (int)k;
			}
			if (g->sub_len == 0)
				g->state = G_SUBLEN;
			break;
		case G_IMGDESC:
			if ((p = img_gather(d, &b, &n, 9)) == NULL)
				break;
			g->fx = p[0] | p[1] << 8;
			g->fy = p[2] | p[3] << 8;
			g->fw = p[4] | p[5] << 8;
			g->fh = p[6] | p[7] << 8;
			g->interlaced = (p[8] & 0x40) != 0;
			if (p[8] & 0x80) {
				g->lct_n = 2 << (p[8] & 7);
				g->state = G_LCT;
			} else
				g->state = G_LZWMIN;
			if ((r = frame_begin(d, g)) != IMG_OK)
				return r;
			break;
		case G_LCT:
			if ((p = img_gather(d, &b, &n, (size_t)g->lct_n * 3)) == NULL)
				break;
			memcpy(g->lct, p, (size_t)g->lct_n * 3);
			g->state = G_LZWMIN;
			break;
		case G_LZWMIN:
			g->min = *b++;
			n--;
			if (g->min < 2 || g->min > 11)
				return IMG_BAD;
			g->clear = 1 << g->min;
			g->eoi = g->clear + 1;
			lzw_reset(g);
			g->state = G_DATALEN;
			break;
		case G_DATALEN:
			g->sub_len = *b++;
			n--;
			if (g->sub_len == 0) {
				/* the frame's data ended: the first frame is all
				 * we show */
				g->state = G_DONE;
				return IMG_END;
			}
			g->state = G_DATA;
			break;
		case G_DATA: {
			size_t k = (size_t)g->sub_len < n ? (size_t)g->sub_len : n;

			if ((r = lzw_bytes(d, g, b, k)) != IMG_OK)
				return r;
			b += k;
			n -= k;
			g->sub_len -= (int)k;
			if (g->sub_len == 0)
				g->state = G_DATALEN;
			break;
		}
		case G_DONE:
			return IMG_END;
		}
		if (d->want && n == 0)
			break;		/* gathering: more input needed */
		if (d->want && d->hold_cap < d->want && n > 0)
			return IMG_TOOBIG;
	}
	return g->state == G_DONE ? IMG_END : IMG_OK;
}

int gif_finish(struct img_dec *d)
{
	struct gif *g = d->fmt;

	/* a stream cut after its rows are all there still shows */
	if (g->state == G_DONE || (g->fh > 0 && g->fy_out >= g->fh))
		return IMG_END;
	return IMG_BAD;
}
