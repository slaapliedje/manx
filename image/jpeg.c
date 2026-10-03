/*
 * jpeg.c - baseline (and extended sequential Huffman) JPEG: greyscale or
 * YCbCr, sampling up to 2x2, restart markers. The file is kept (within
 * the budget) and decoded when it ends, one MCU row at a time, each row of
 * pixels going to the sink as soon as it is whole.
 *
 * The IDCT is libjpeg's accurate integer one ("islow"), so pixels match
 * the reference decoder; chroma is upsampled by replication. At 1/8 size
 * only each block's DC term is needed: no IDCT at all; at 1/2 and 1/4 a
 * reduced IDCT of the low frequencies, a quarter of the work or less.
 *
 * Progressive files are read scan by scan into every block's
 * coefficients, kept until the end: only the S x S corner a block is
 * shown at (with, when that is less than the whole block, a bit for each
 * other position that is nonzero, which later scans need to know), and a
 * scan with nothing in that corner is skipped; at 1/8 size only the DC
 * scans are read. If that doesn't fit the memory allowed, a smaller corner
 * is kept and its pixels enlarged: a big picture shows, if softer. A
 * progressive file cut short shows what had come.
 *
 * Arithmetic-coded, 12-bit and CMYK files are refused (IMG_UNSUPPORTED),
 * as is a sequential file whose components come in separate scans.
 */
#include <string.h>
#include "image_int.h"

struct huff {
	unsigned char fast_len[512];	/* codes of up to 9 bits: length, */
	unsigned char fast_val[512];	/* and value (len 0: longer code) */
	long maxcode[18];		/* the longest code of each length */
	int valptr[17], mincode[17];
	unsigned char vals[256];
	int set;
};

struct comp {
	int id, h, v, tq, td, ta;
	int bw;				/* blocks across, in an MCU row */
	int pw, ph;			/* the MCU row's samples (scaled) */
	int pred;			/* DC predictor */
	unsigned char *plane;		/* pw * ph samples */
	/* progressive: every block's coefficients, kept until the end */
	int cbw, cbh;			/* blocks a scan of it alone covers */
	short *coef;			/* bw x (mcus_y * v) blocks x K */
	unsigned int *nz;		/* 2 a block: the zigzag positions not
					 * kept that are nonzero (1 < S2 < 8) */
};

struct jpeg {
	unsigned char *buf;
	size_t len, cap;
	unsigned short qt[4][64];
	int qset;
	struct huff dc[4], ac[4];
	int w, h, ncomp, hmax, vmax;
	int mcus_x, mcus_y;
	struct comp c[3];
	int ri;				/* restart interval (MCUs) */
	int shift, S;			/* scaling: block side 8 >> shift */
	/* progressive */
	int prog, S2, K, maxk;		/* coefficients kept a block: the S2 x S2
					 * corner (S2 = S, or less if they
					 * wouldn't fit); its highest zigzag
					 * position */
	signed char kidx[64];		/* zigzag position -> where kept, -1 */
	int ss, se, ah, al;		/* the scan's band and bit position */
	int scomp[3], nscomp;		/* its components */
	unsigned eobrun;
	int scans;			/* scans read */
	int ow, oh;			/* the size sent */
	unsigned char *rgba;		/* ow * 4 */
	long co[64];			/* a block's coefficients: all zero */
					/* between blocks (cleared as used) */
	/* the bit reader */
	size_t pos;
	unsigned int bits;		/* (32 bits on every target here) */
	int nbits, marker;
	int pad;			/* zero bytes read past the data */
	int done;
};

static const unsigned char zz[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* YCbCr to RGB, as libjpeg's jdcolor.c */
static int cr_r[256], cb_b[256];
static long cr_g[256], cb_g[256];
static int tables_ready;

static void make_tables(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		long x = i - 128;

		cr_r[i] = (int)((91881L * x + 32768L) >> 16);	/* 1.40200 */
		cb_b[i] = (int)((116130L * x + 32768L) >> 16);	/* 1.77200 */
		cr_g[i] = -46802L * x;				/* 0.71414 */
		cb_g[i] = -22554L * x + 32768L;			/* 0.34414 */
	}
	tables_ready = 1;
}

static unsigned char clamp(long v)
{
	/* (one comparison when, as nearly always, it is in range) */
	return (unsigned char)((unsigned long)v <= 255 ? v : v < 0 ? 0 : 255);
}

/*
 * A dequantized coefficient, kept to 11 bits: a valid 8-bit JPEG's never
 * go past 1024 or so, and with them the IDCTs' products stay within a
 * 32-bit long, below 2^30 (a corrupt file's would overflow it).
 */
static long coef(long v)
{
	return v < -2047 ? -2047 : v > 2047 ? 2047 : v;
}

int jpeg_new(struct img_dec *d)
{
	struct jpeg *j = img_alloc(d, sizeof *j);

	if (j == NULL)
		return -1;
	memset(j, 0, sizeof *j);
	if (!tables_ready)
		make_tables();
	d->fmt = j;
	return 0;
}

void jpeg_free(struct img_dec *d)
{
	struct jpeg *j = d->fmt;
	int i;

	for (i = 0; i < 3; i++) {
		struct comp *c = &j->c[i];
		size_t nb = (size_t)c->bw * (size_t)j->mcus_y * (size_t)c->v;

		if (c->coef)
			img_release(d, c->coef, nb * (size_t)j->K * sizeof *c->coef);
		if (c->nz)
			img_release(d, c->nz, nb * 2 * sizeof *c->nz);
	}
	img_release(d, j->buf, j->cap);
	img_release(d, j->rgba, (size_t)j->ow * 4);
	for (i = 0; i < 3; i++)
		img_release(d, j->c[i].plane, (size_t)j->c[i].pw * (size_t)j->c[i].ph);
	img_release(d, j, sizeof *j);
	d->fmt = NULL;
}

int jpeg_feed(struct img_dec *d, const unsigned char *b, size_t n)
{
	struct jpeg *j = d->fmt;

	if (j->len + n > j->cap) {
		size_t c = j->cap ? j->cap * 2 : 16384;
		unsigned char *q;

		while (c < j->len + n)
			c *= 2;
		if ((q = img_alloc(d, c)) == NULL) {
			/* the budget, exactly */
			c = j->len + n;
			if ((q = img_alloc(d, c)) == NULL)
				return IMG_TOOBIG;
		}
		if (j->len)
			memcpy(q, j->buf, j->len);
		img_release(d, j->buf, j->cap);
		j->buf = q;
		j->cap = c;
	}
	memcpy(j->buf + j->len, b, n);
	j->len += n;
	return IMG_OK;
}

/* --- markers ------------------------------------------------------------ */

static int u16(const struct jpeg *j, size_t at)
{
	return at + 1 < j->len ? j->buf[at] << 8 | j->buf[at + 1] : -1;
}

static int read_dqt(struct jpeg *j, size_t at, size_t end)
{
	while (at < end) {
		int pq = j->buf[at] >> 4, tq = j->buf[at] & 15, i;

		at++;
		if (tq > 3 || at + (size_t)(pq ? 128 : 64) > end)
			return IMG_BAD;
		for (i = 0; i < 64; i++) {
			j->qt[tq][i] = (unsigned short)(pq ? j->buf[at] << 8
				| j->buf[at + 1] : j->buf[at]);
			at += (size_t)(pq ? 2 : 1);
		}
		j->qset |= 1 << tq;
	}
	return IMG_OK;
}

static int build_huff(struct huff *t, const unsigned char *counts,
	const unsigned char *vals, int nvals)
{
	int len, i, k = 0;
	long code = 0;

	memset(t, 0, sizeof *t);
	memcpy(t->vals, vals, (size_t)nvals);
	for (len = 1; len <= 16; len++) {
		t->valptr[len] = k;
		t->mincode[len] = (int)code;
		for (i = 0; i < counts[len - 1]; i++, k++, code++) {
			if (len <= 9) {
				int shift = 9 - len, n = 1 << shift, f;
				long base = code << shift;

				if (base + n > 512)
					return -1;
				for (f = 0; f < n; f++) {
					t->fast_len[base + f] = (unsigned char)len;
					t->fast_val[base + f] = vals[k];
				}
			}
		}
		t->maxcode[len] = counts[len - 1] ? code - 1 : -1;
		if (code > (1L << len))
			return -1;		/* more codes than fit */
		code <<= 1;
	}
	t->maxcode[17] = 0x7FFFFFFFL;
	t->set = 1;
	return 0;
}

static int read_dht(struct jpeg *j, size_t at, size_t end)
{
	while (at + 17 <= end) {
		int tc = j->buf[at] >> 4, th = j->buf[at] & 15, n = 0, i;
		const unsigned char *counts = j->buf + at + 1;

		for (i = 0; i < 16; i++)
			n += counts[i];
		at += 17;
		if (tc > 1 || th > 3 || n > 256 || at + (size_t)n > end)
			return IMG_BAD;
		if (build_huff(tc ? &j->ac[th] : &j->dc[th], counts, j->buf + at, n) < 0)
			return IMG_BAD;
		at += (size_t)n;
	}
	return IMG_OK;
}

static int read_sof(struct img_dec *d, struct jpeg *j, size_t at, size_t end)
{
	struct img_info in;
	int i, shift = 0;

	if (end - at < 6)
		return IMG_BAD;
	if (j->buf[at] != 8)
		return IMG_UNSUPPORTED;		/* 12-bit */
	j->h = u16(j, at + 1);
	j->w = u16(j, at + 3);
	j->ncomp = j->buf[at + 5];
	if (j->ncomp != 1 && j->ncomp != 3)
		return IMG_UNSUPPORTED;
	if (end - at < 6 + 3 * (size_t)j->ncomp || j->w <= 0 || j->h <= 0)
		return IMG_BAD;
	if (j->w > IMG_MAX_SIDE || j->h > IMG_MAX_SIDE)
		return IMG_TOOBIG;
	j->hmax = j->vmax = 1;
	for (i = 0; i < j->ncomp; i++) {
		struct comp *c = &j->c[i];
		const unsigned char *p = j->buf + at + 6 + 3 * i;

		c->id = p[0];
		c->h = p[1] >> 4;
		c->v = p[1] & 15;
		c->tq = p[2];
		if (j->ncomp == 1)
			c->h = c->v = 1;	/* a lone component's MCU is a block */
		if (c->h < 1 || c->h > 2 || c->v < 1 || c->v > 2 || c->tq > 3)
			return c->tq > 3 ? IMG_BAD : IMG_UNSUPPORTED;
		if (c->h > j->hmax)
			j->hmax = c->h;
		if (c->v > j->vmax)
			j->vmax = c->v;
	}
	memset(&in, 0, sizeof in);
	in.w = in.full_w = j->w;
	in.h = in.full_h = j->h;
	in.scalable = 1;
	if (d->sink.size(d->sink.ctx, &in, &shift) < 0)
		return IMG_STOP;
	j->shift = shift >= 1 && shift <= 3 ? shift : 0;
	j->S = 8 >> j->shift;
	j->ow = (j->w + (1 << j->shift) - 1) >> j->shift;
	j->oh = (j->h + (1 << j->shift) - 1) >> j->shift;
	j->mcus_x = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
	j->mcus_y = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
	for (i = 0; i < j->ncomp; i++) {
		struct comp *c = &j->c[i];

		c->bw = j->mcus_x * c->h;
		c->pw = c->bw * j->S;
		c->ph = c->v * j->S;
		if ((c->plane = img_alloc(d, (size_t)c->pw * (size_t)c->ph)) == NULL)
			return IMG_TOOBIG;
	}
	if (j->prog) {
		/* every block's coefficients: the corner shown (S x S), or a
		 * smaller one if that won't fit what is left of the budget
		 * (the rows of pixels still to come out of it too) */
		size_t blocks = 0, room = d->cap - d->used, need;
		int k;

		for (i = 0; i < j->ncomp; i++)
			blocks += (size_t)j->c[i].bw * (size_t)j->mcus_y * (size_t)j->c[i].v;
		room = room > (size_t)j->ow * 4 + 4096 ? room - (size_t)j->ow * 4 - 4096 : 0;
		for (j->S2 = j->S; ; j->S2 /= 2) {
			need = blocks * ((size_t)j->S2 * (size_t)j->S2 * sizeof(short)
				+ (j->S2 > 1 && j->S2 < 8 ? 2 * sizeof(unsigned int) : 0));
			if (need <= room || j->S2 == 1)
				break;
		}
		j->K = j->S2 * j->S2;
		j->maxk = 0;
		for (k = 0; k < 64; k++) {
			int z = zz[k], row = z / 8, col = z % 8;

			j->kidx[k] = (signed char)(row < j->S2 && col < j->S2 ?
				row * j->S2 + col : -1);
			if (j->kidx[k] >= 0)
				j->maxk = k;
		}
		for (i = 0; i < j->ncomp; i++) {
			struct comp *c = &j->c[i];
			size_t nb = (size_t)c->bw * (size_t)j->mcus_y * (size_t)c->v;

			c->cbw = ((j->w * c->h + j->hmax - 1) / j->hmax + 7) / 8;
			c->cbh = ((j->h * c->v + j->vmax - 1) / j->vmax + 7) / 8;
			if ((c->coef = img_alloc(d, nb * (size_t)j->K * sizeof *c->coef)) == NULL)
				return IMG_TOOBIG;
			memset(c->coef, 0, nb * (size_t)j->K * sizeof *c->coef);
			/* (at 1/8 no AC scan is read: no mask) */
			if (j->S2 > 1 && j->S2 < 8) {
				if ((c->nz = img_alloc(d, nb * 2 * sizeof *c->nz)) == NULL)
					return IMG_TOOBIG;
				memset(c->nz, 0, nb * 2 * sizeof *c->nz);
			}
		}
	}
	if ((j->rgba = img_alloc(d, (size_t)j->ow * 4)) == NULL)
		return IMG_TOOBIG;
	/* opaque, once: emit() never writes alpha */
	for (i = 0; i < j->ow; i++)
		j->rgba[i * 4 + 3] = 255;
	return IMG_OK;
}

/* --- entropy decoding ------------------------------------------------------ */

/*
 * The bit buffer, left-aligned in 32 bits. On a 68030 the Huffman loop
 * must fit the 256-byte instruction cache, or every symbol misses it: so
 * block() keeps the buffer in locals and decodes through the macros below,
 * with only the rare cases (long codes, 0xFF bytes, markers, the end) out
 * of line.
 */

/* the next byte for the buffer, when it is 0xFF or there is none: a
 * stuffed 0xFF, or zeros from a marker (which stays put) or the end */
static unsigned special(struct jpeg *j)
{
	unsigned m;

	if (j->marker || j->pos >= j->len) {
		j->pad++;
		return 0;
	}
	m = j->pos + 1 < j->len ? j->buf[j->pos + 1] : 0xD9;
	if (m == 0) {
		j->pos += 2;
		return 0xFF;
	}
	j->marker = (int)m;
	return 0;
}

/* top the buffer up to more than 24 bits */
static void refill(struct jpeg *j)
{
	while (j->nbits <= 24) {
		unsigned b;

		if (j->pos < j->len && (b = j->buf[j->pos]) != 0xFF)
			j->pos++;
		else
			b = special(j);
		j->bits |= (unsigned int)b << (24 - j->nbits);
		j->nbits += 8;
	}
}

/* a symbol whose code is longer than the fast table's 9 bits, or -1 */
static int decode_long(struct jpeg *j, const struct huff *t)
{
	int len;

	for (len = 10; len <= 16; len++) {
		long code = (long)(j->bits >> (32 - len));

		if (code <= t->maxcode[len]) {
			int k = t->valptr[len] + (int)(code - t->mincode[len]);

			j->bits <<= len;
			j->nbits -= len;
			return k >= 0 && k < 256 ? t->vals[k] : -1;
		}
	}
	return -1;
}

/* in a function with locals j, bits and nbits: */
#define SYNC_OUT()	(j->bits = bits, j->nbits = nbits)
#define SYNC_IN()	(bits = j->bits, nbits = j->nbits)
#define NEED16()	do { if (nbits < 16) { SYNC_OUT(); refill(j); SYNC_IN(); } } while (0)

/* sym = the next symbol of table t (-1: none) */
#define HUFF(t, sym) do {						\
	int l_;								\
									\
	NEED16();							\
	if ((l_ = (t)->fast_len[bits >> 23]) != 0) {			\
		(sym) = (t)->fast_val[bits >> 23];			\
		bits <<= l_;						\
		nbits -= l_;						\
	} else {							\
		SYNC_OUT();						\
		(sym) = decode_long(j, (t));				\
		SYNC_IN();						\
	}								\
} while (0)

/* v = the next n (1-16) bits, unsigned */
#define GETBITS(n, v) do {						\
	NEED16();							\
	(v) = (int)(bits >> (32 - (n)));				\
	bits <<= (n);							\
	nbits -= (n);							\
} while (0)

/* v = the next n (1-16) bits as a signed value of size n (JPEG's
 * "receive" and "extend") */
#define RECEIVE(n, v) do {						\
	unsigned int u_;						\
									\
	NEED16();							\
	u_ = bits >> (32 - (n));					\
	bits <<= (n);							\
	nbits -= (n);							\
	(v) = u_ < (1U << ((n) - 1)) ? (int)u_ - (1 << (n)) + 1 : (int)u_; \
} while (0)

/* --- the IDCTs ---------------------------------------------------------- */

/*
 * The full-size IDCT is libjpeg's exact one (jidctint.c). Its fast one
 * (jidctfst.c, AAN: 5 multiplies a pass instead of 12, the rest folded
 * into dequantization) measured slower on the 68030, 3.9 s against 2.9 s
 * for a 320x240 photo, as well as less exact.
 */
#define CONST_BITS	13
#define PASS1_BITS	2
#define DESCALE(x, n)	(((x) + (1L << ((n) - 1))) >> (n))
/* (a negative value is multiplied up, not shifted: that is undefined) */

#define FIX_0_298631336	2446L
#define FIX_0_390180644	3196L
#define FIX_0_541196100	4433L
#define FIX_0_765366865	6270L
#define FIX_0_899976223	7373L
#define FIX_1_175875602	9633L
#define FIX_1_501321110	12299L
#define FIX_1_847759065	15137L
#define FIX_1_961570560	16069L
#define FIX_2_053119869	16819L
#define FIX_2_562915447	20995L
#define FIX_3_072711026	25172L
/* the 1-D transform of src[0], src[d], ... src[7d] into dst[0], dst[ds],
 * ... dst[7ds], each result through OUTX (both passes are this) */
#define ISLOW_1D(src, d, dst, ds, OUTX) do {				\
	long z1, z2, z3, z4, z5, t0, t1, t2, t3, t10, t11, t12, t13;	\
									\
	/* even part */							\
	z2 = src[2 * (d)];						\
	z3 = src[6 * (d)];						\
	z1 = (z2 + z3) * FIX_0_541196100;				\
	t2 = z1 + z3 * -FIX_1_847759065;				\
	t3 = z1 + z2 * FIX_0_765366865;					\
	t0 = (src[0] + src[4 * (d)]) * (1L << CONST_BITS);		\
	t1 = (src[0] - src[4 * (d)]) * (1L << CONST_BITS);		\
	t10 = t0 + t3;							\
	t13 = t0 - t3;							\
	t11 = t1 + t2;							\
	t12 = t1 - t2;							\
	/* odd part */							\
	t0 = src[7 * (d)];						\
	t1 = src[5 * (d)];						\
	t2 = src[3 * (d)];						\
	t3 = src[(d)];							\
	z1 = t0 + t3;							\
	z2 = t1 + t2;							\
	z3 = t0 + t2;							\
	z4 = t1 + t3;							\
	z5 = (z3 + z4) * FIX_1_175875602;				\
	t0 *= FIX_0_298631336;						\
	t1 *= FIX_2_053119869;						\
	t2 *= FIX_3_072711026;						\
	t3 *= FIX_1_501321110;						\
	z1 *= -FIX_0_899976223;						\
	z2 *= -FIX_2_562915447;						\
	z3 = z3 * -FIX_1_961570560 + z5;				\
	z4 = z4 * -FIX_0_390180644 + z5;				\
	t0 += z1 + z3;							\
	t1 += z2 + z4;							\
	t2 += z2 + z3;							\
	t3 += z1 + z4;							\
	dst[0] = OUTX(t10 + t3);					\
	dst[7 * (ds)] = OUTX(t10 - t3);					\
	dst[(ds)] = OUTX(t11 + t2);					\
	dst[6 * (ds)] = OUTX(t11 - t2);					\
	dst[2 * (ds)] = OUTX(t12 + t1);					\
	dst[5 * (ds)] = OUTX(t12 - t1);					\
	dst[3 * (ds)] = OUTX(t13 + t0);					\
	dst[4 * (ds)] = OUTX(t13 - t0);					\
} while (0)
#define P1(v)	DESCALE(v, CONST_BITS - PASS1_BITS)
#define P2(v)	clamp(DESCALE(v, CONST_BITS + PASS1_BITS + 3) + 128)

static void idct(const long *in, unsigned char *out, int stride)
{
	long ws[64];
	int i;

	for (i = 0; i < 8; i++) {
		const long *p = in + i;
		long *w = ws + i;

		if (!p[8] && !p[16] && !p[24] && !p[32] && !p[40] && !p[48] && !p[56]) {
			long dc = p[0] * (1L << PASS1_BITS);

			w[0] = w[8] = w[16] = w[24] = w[32] = w[40] = w[48] = w[56] = dc;
			continue;
		}
		ISLOW_1D(p, 8, w, 8, P1);
	}
	for (i = 0; i < 8; i++) {
		const long *w = ws + i * 8;
		unsigned char *o = out + i * stride;

		if (!w[1] && !w[2] && !w[3] && !w[4] && !w[5] && !w[6] && !w[7]) {
			unsigned char v = clamp(DESCALE(w[0], PASS1_BITS + 3) + 128);

			o[0] = o[1] = o[2] = o[3] = o[4] = o[5] = o[6] = o[7] = v;
			continue;
		}
		ISLOW_1D(w, 1, o, 1, P2);
	}
}

/*
 * Reduced IDCTs, for 1/2 and 1/4 size: the block's low 4x4 (2x2)
 * frequencies, evaluated at the centres of the pixel pairs (quads). Per
 * dimension, with c_k = cos(k pi / 8):
 *   4-point: g0,3 = e0 +- o0, g1,2 = e1 +- o1, where
 *     e0,1 = F0/(2 sqrt 2) +- F2 c2/2,  o0 = (F1 c1 + F3 c3)/2,
 *     o1 = (F1 c3 - F3 c1)/2  (6 multiplies, against the 8x8's 16 or so)
 *   2-point: g0,1 = F0/(2 sqrt 2) +- F1 c2/2
 * A flat block comes out flat at its value, as at full size.
 */
#define K_A	2896L		/* 1/(2 sqrt 2), and c2/2 */
#define K_C1	3784L		/* c1/2 */
#define K_C3	1567L		/* c3/2 */

static void idct4(const long *in, unsigned char *out, int stride)
{
	long t[16], e0, e1, o0, o1;
	int i;

	for (i = 0; i < 4; i++) {
		const long *p = in + i;

		e0 = p[0] * K_A + p[16] * K_A;
		e1 = p[0] * K_A - p[16] * K_A;
		o0 = p[8] * K_C1 + p[24] * K_C3;
		o1 = p[8] * K_C3 - p[24] * K_C1;
		t[i] = DESCALE(e0 + o0, CONST_BITS - PASS1_BITS);
		t[12 + i] = DESCALE(e0 - o0, CONST_BITS - PASS1_BITS);
		t[4 + i] = DESCALE(e1 + o1, CONST_BITS - PASS1_BITS);
		t[8 + i] = DESCALE(e1 - o1, CONST_BITS - PASS1_BITS);
	}
	for (i = 0; i < 4; i++) {
		const long *p = t + i * 4;
		unsigned char *o = out + i * stride;

		e0 = p[0] * K_A + p[2] * K_A;
		e1 = p[0] * K_A - p[2] * K_A;
		o0 = p[1] * K_C1 + p[3] * K_C3;
		o1 = p[1] * K_C3 - p[3] * K_C1;
		o[0] = clamp(DESCALE(e0 + o0, CONST_BITS + PASS1_BITS) + 128);
		o[3] = clamp(DESCALE(e0 - o0, CONST_BITS + PASS1_BITS) + 128);
		o[1] = clamp(DESCALE(e1 + o1, CONST_BITS + PASS1_BITS) + 128);
		o[2] = clamp(DESCALE(e1 - o1, CONST_BITS + PASS1_BITS) + 128);
	}
}

static void idct2(const long *in, unsigned char *out, int stride)
{
	long a0 = in[0] * K_A + in[8] * K_A, a1 = in[0] * K_A - in[8] * K_A;
	long b0 = in[1] * K_A + in[9] * K_A, b1 = in[1] * K_A - in[9] * K_A;

	/* (columns, then the rows of the 2x2) */
	a0 = DESCALE(a0, CONST_BITS - PASS1_BITS);
	a1 = DESCALE(a1, CONST_BITS - PASS1_BITS);
	b0 = DESCALE(b0, CONST_BITS - PASS1_BITS);
	b1 = DESCALE(b1, CONST_BITS - PASS1_BITS);
	out[0] = clamp(DESCALE(a0 * K_A + b0 * K_A, CONST_BITS + PASS1_BITS) + 128);
	out[1] = clamp(DESCALE(a0 * K_A - b0 * K_A, CONST_BITS + PASS1_BITS) + 128);
	out[stride] = clamp(DESCALE(a1 * K_A + b1 * K_A, CONST_BITS + PASS1_BITS) + 128);
	out[stride + 1] = clamp(DESCALE(a1 * K_A - b1 * K_A, CONST_BITS + PASS1_BITS) + 128);
}

/*
 * One block of component c into its plane at block bx, by. j->co is all
 * zeros on the way in and out: the coefficients set are cleared after.
 */
static int block(struct jpeg *j, struct comp *c, int bx, int by)
{
	long *co = j->co, dc;
	const unsigned short *q = j->qt[c->tq];
	const struct huff *ac = &j->ac[c->ta];
	unsigned char set[64];		/* natural positions written */
	int s, k, r, sym, n = 0, i, S = j->S;
	unsigned int bits = j->bits;
	int nbits = j->nbits;
	unsigned char *o = c->plane + (size_t)by * S * (size_t)c->pw + (size_t)bx * S;

	/* well past the end of the data: a truncated file, not a huge one
	 * to decode out of zeros (the rows sent so far stay) */
	if (j->pad > 64)
		return IMG_BAD;
	HUFF(&j->dc[c->td], sym);
	if (sym < 0 || sym > 11)
		return IMG_BAD;
	if (sym) {
		RECEIVE(sym, r);
		c->pred += r;
	}
	dc = coef((long)c->pred * q[0]);
	for (k = 1; k < 64; k++) {
		HUFF(ac, sym);
		if (sym < 0)
			return IMG_BAD;
		r = sym >> 4;
		s = sym & 15;
		if (s == 0) {
			if (r != 15)
				break;		/* end of block */
			k += 15;
			continue;
		}
		k += r;
		if (k > 63)
			return IMG_BAD;
		RECEIVE(s, r);
		if (S > 1) {
			int z = zz[k];

			co[z] = coef((long)r * q[k]);
			set[n++] = (unsigned char)z;
		}
	}
	SYNC_OUT();
	if (n == 0) {
		/* the DC term alone (common): a flat block, at its mean */
		unsigned char v = clamp(DESCALE(dc, 3) + 128);

		for (i = 0; i < j->S; i++, o += c->pw) {
			int x;

			for (x = 0; x < j->S; x++)
				o[x] = v;
		}
		return IMG_OK;
	}
	co[0] = dc;
	if (j->S == 8)
		idct(co, o, c->pw);
	else if (j->S == 4)
		idct4(co, o, c->pw);
	else
		idct2(co, o, c->pw);
	co[0] = 0;
	while (n > 0)
		co[set[--n]] = 0;
	return IMG_OK;
}

/*
 * The MCU row my is in the planes: send its rows of pixels. Sampling
 * factors are 1 or 2, so a component's sample for pixel x, y is at x and
 * y shifted right by 0 or 1 (no division per pixel: on a 68030 that was
 * half the time of the whole decode).
 */
static int emit(struct img_dec *d, struct jpeg *j, int my)
{
	const struct comp *c0 = &j->c[0], *c1 = &j->c[1], *c2 = &j->c[2];
	int rows = j->vmax * j->S, ow = j->ow, y, x;
	int h0 = c0->h < j->hmax, v0 = c0->v < j->vmax;
	int h1 = c1->h < j->hmax, v1 = c1->v < j->vmax;
	int h2 = c2->h < j->hmax, v2 = c2->v < j->vmax;

	for (y = 0; y < rows; y++) {
		int oy = my * rows + y;
		const unsigned char *py = c0->plane + (size_t)(y >> v0) * (size_t)c0->pw;
		unsigned char *o = j->rgba;

		if (oy >= j->oh)
			break;
		if (j->ncomp == 1)
			for (x = 0; x < ow; x++, o += 4)
				o[0] = o[1] = o[2] = py[x >> h0];
		else {
			const unsigned char *pb = c1->plane + (size_t)(y >> v1) * (size_t)c1->pw;
			const unsigned char *pr = c2->plane + (size_t)(y >> v2) * (size_t)c2->pw;

			for (x = 0; x < ow; x++, o += 4) {
				int yy = py[x >> h0], cb = pb[x >> h1], cr = pr[x >> h2];

				o[0] = clamp(yy + cr_r[cr]);
				o[1] = clamp(yy + (int)((cb_g[cb] + cr_g[cr]) >> 16));
				o[2] = clamp(yy + cb_b[cb]);
			}
		}
		if (d->sink.row(d->sink.ctx, oy, j->rgba) < 0)
			return IMG_STOP;
	}
	return IMG_OK;
}

/* the restart marker that should come next */
static int restart(struct jpeg *j)
{
	int i;

	if (!j->marker) {
		/* not reached yet: the padding before it is still to come */
		while (j->pos + 1 < j->len && !(j->buf[j->pos] == 0xFF
			&& j->buf[j->pos + 1] >= 0xD0 && j->buf[j->pos + 1] <= 0xD7))
			j->pos++;
		if (j->pos + 1 < j->len)
			j->marker = j->buf[j->pos + 1];
	}
	if (j->marker < 0xD0 || j->marker > 0xD7)
		return IMG_BAD;
	j->pos += 2;
	j->marker = 0;
	j->bits = 0;
	j->nbits = 0;
	j->pad = 0;
	for (i = 0; i < j->ncomp; i++)
		j->c[i].pred = 0;
	return IMG_OK;
}

static int scan(struct img_dec *d, struct jpeg *j)
{
	int mx, my, i, r, todo = j->ri;

	j->bits = 0;
	j->nbits = 0;
	j->marker = 0;
	j->pad = 0;
	for (i = 0; i < j->ncomp; i++)
		j->c[i].pred = 0;
	for (my = 0; my < j->mcus_y; my++) {
		for (mx = 0; mx < j->mcus_x; mx++) {
			if (j->ri && todo-- == 0) {
				if ((r = restart(j)) != IMG_OK)
					return r;
				todo = j->ri - 1;
			}
			for (i = 0; i < j->ncomp; i++) {
				struct comp *c = &j->c[i];
				int bx, by;

				for (by = 0; by < c->v; by++)
					for (bx = 0; bx < c->h; bx++)
						if ((r = block(j, c, mx * c->h + bx, by)) != IMG_OK)
							return r;
			}
		}
		if ((r = emit(d, j, my)) != IMG_OK)
			return r;
	}
	return IMG_END;
}

static int read_sos(struct img_dec *d, struct jpeg *j, size_t at, size_t end)
{
	int n, i, k;

	if (j->w == 0)
		return IMG_BAD;		/* no frame yet */
	n = j->buf[at];
	if (n != j->ncomp)
		return IMG_UNSUPPORTED;	/* components in separate scans */
	if (end - at < 1 + 2 * (size_t)n + 3)
		return IMG_BAD;
	for (i = 0; i < n; i++) {
		int id = j->buf[at + 1 + 2 * i], t = j->buf[at + 2 + 2 * i];

		for (k = 0; k < j->ncomp && j->c[k].id != id; k++)
			;
		if (k == j->ncomp)
			return IMG_BAD;
		j->c[k].td = t >> 4;
		j->c[k].ta = t & 15;
		if (j->c[k].td > 3 || j->c[k].ta > 3 || !j->dc[j->c[k].td].set
			|| !j->ac[j->c[k].ta].set || !(j->qset & (1 << j->c[k].tq)))
			return IMG_BAD;
	}
	j->pos = end;
	return scan(d, j);
}

/* --- progressive ------------------------------------------------------------ */

/* zigzag position k of a block (its kept values v, its mask m): nonzero? */
#define NZ(k)	(j->kidx[k] >= 0 ? v[j->kidx[k]] != 0 : (int)(m[(k) >> 5] >> ((k) & 31) & 1))
#define SET(k, x) do {							\
	if (j->kidx[k] >= 0)						\
		v[j->kidx[k]] = (short)(x);				\
	else								\
		m[(k) >> 5] |= 1U << ((k) & 31);			\
} while (0)

/* a refinement bit for a nonzero coefficient (only kept ones change) */
#define REFINE(k) do {							\
	int b_;								\
									\
	GETBITS(1, b_);							\
	if (b_ && j->kidx[k] >= 0) {					\
		short *p_ = &v[j->kidx[k]];				\
									\
		if ((*p_ & bit) == 0)					\
			*p_ = (short)(*p_ >= 0 ? *p_ + bit : *p_ - bit);	\
	}								\
} while (0)

/* one block of a progressive scan into its coefficients */
static int prog_block(struct jpeg *j, struct comp *c, long bi)
{
	short *v = c->coef + bi * j->K;
	unsigned int *m = c->nz ? c->nz + bi * 2 : NULL;
	unsigned int bits = j->bits;
	int nbits = j->nbits, k, r, s, sym;

	if (j->pad > 64)
		return IMG_BAD;
	if (j->ss == 0) {		/* DC */
		if (j->ah == 0) {
			HUFF(&j->dc[c->td], sym);
			if (sym < 0 || sym > 11)
				return IMG_BAD;
			if (sym) {
				RECEIVE(sym, r);
				c->pred += r;
				/* (a corrupt file's: kept from overflowing) */
				if (c->pred > 65535 || c->pred < -65535)
					c->pred = c->pred > 0 ? 65535 : -65535;
			}
			v[0] = (short)((long)c->pred * (1L << j->al));
		} else {
			GETBITS(1, r);
			if (r)
				v[0] = (short)(v[0] | (1 << j->al));
		}
		SYNC_OUT();
		return IMG_OK;
	}
	if (m == NULL) {
		static unsigned int none[2];

		m = none;		/* (all kept: the mask unused) */
	}
	if (j->ah == 0) {		/* an AC band, first */
		if (j->eobrun) {
			j->eobrun--;
			return IMG_OK;
		}
		for (k = j->ss; k <= j->se; k++) {
			HUFF(&j->ac[c->ta], sym);
			if (sym < 0)
				return IMG_BAD;
			r = sym >> 4;
			s = sym & 15;
			if (s == 0) {
				if (r < 15) {
					j->eobrun = 1U << r;
					if (r) {
						int x;

						GETBITS(r, x);
						j->eobrun += (unsigned)x;
					}
					j->eobrun--;
					break;
				}
				k += 15;
				continue;
			}
			k += r;
			if (k > 63)
				return IMG_BAD;
			RECEIVE(s, r);
			SET(k, r * (1 << j->al));
		}
	} else {			/* an AC band, refined */
		int bit = 1 << j->al;

		k = j->ss;
		if (j->eobrun == 0) {
			while (k <= j->se) {
				int x = 0;

				HUFF(&j->ac[c->ta], sym);
				if (sym < 0)
					return IMG_BAD;
				r = sym >> 4;
				s = sym & 15;
				if (s == 0) {
					if (r < 15) {
						j->eobrun = 1U << r;
						if (r) {
							int y;

							GETBITS(r, y);
							j->eobrun += (unsigned)y;
						}
						break;	/* the rest: as an end of band */
					}
				} else {
					if (s != 1)
						return IMG_BAD;
					GETBITS(1, x);
					x = x ? bit : -bit;
				}
				/* r zero coefficients passed (the nonzero ones
				 * among them refined), then x into the next */
				while (k <= j->se) {
					if (NZ(k))
						REFINE(k);
					else {
						if (r == 0) {
							if (x)
								SET(k, x);
							k++;
							break;
						}
						r--;
					}
					k++;
				}
			}
		}
		if (j->eobrun) {
			/* the end of the band: its nonzero ones refined */
			for (; k <= j->se; k++)
				if (NZ(k))
					REFINE(k);
			j->eobrun--;
		}
	}
	SYNC_OUT();
	return IMG_OK;
}

/* where the entropy data of the scan ends: its marker's 0xFF, or len */
static size_t scan_end(const struct jpeg *j, size_t p)
{
	while (p + 1 < j->len) {
		if (j->buf[p] == 0xFF) {
			int m = j->buf[p + 1];

			if (m != 0 && m != 0xFF && !(m >= 0xD0 && m <= 0xD7))
				return p;
		}
		p++;
	}
	return j->len;
}

/* a progressive scan, its header from at to end: decoded, or skipped
 * when it has nothing kept. The position after it, or 0 on an error. */
static size_t prog_scan(struct jpeg *j, size_t at, size_t end)
{
	int n = j->buf[at], i, k, r, todo = j->ri;

	if (n < 1 || n > j->ncomp || end - at < 1 + 2 * (size_t)n + 3)
		return 0;
	for (i = 0; i < n; i++) {
		int id = j->buf[at + 1 + 2 * i], t = j->buf[at + 2 + 2 * i];

		for (k = 0; k < j->ncomp && j->c[k].id != id; k++)
			;
		if (k == j->ncomp)
			return 0;
		j->scomp[i] = k;
		j->c[k].td = t >> 4;
		j->c[k].ta = t & 15;
		if (j->c[k].td > 3 || j->c[k].ta > 3)
			return 0;
	}
	j->nscomp = n;
	j->ss = j->buf[at + 1 + 2 * n];
	j->se = j->buf[at + 2 + 2 * n];
	j->ah = j->buf[at + 3 + 2 * n] >> 4;
	j->al = j->buf[at + 3 + 2 * n] & 15;
	if (j->ss > 63 || j->se > 63 || j->ss > j->se || j->al > 13
		|| (j->ss == 0 && j->se != 0) || (j->ss > 0 && n != 1))
		return 0;
	for (i = 0; i < n; i++) {
		struct comp *c = &j->c[j->scomp[i]];

		if ((j->ss == 0 && j->ah == 0 && !j->dc[c->td].set)
			|| (j->ss > 0 && !j->ac[c->ta].set))
			return 0;
	}
	if (j->ss > j->maxk)
		return scan_end(j, end);	/* nothing in it is kept */
	j->pos = end;
	j->bits = 0;
	j->nbits = 0;
	j->marker = 0;
	j->pad = 0;
	j->eobrun = 0;
	for (i = 0; i < j->ncomp; i++)
		j->c[i].pred = 0;
	if (n == 1) {
		/* one component: its blocks in order, each an MCU */
		struct comp *c = &j->c[j->scomp[0]];
		int bx, by;

		for (by = 0; by < c->cbh; by++)
			for (bx = 0; bx < c->cbw; bx++) {
				if (j->ri && todo-- == 0) {
					if (restart(j) != IMG_OK)
						return 0;
					j->eobrun = 0;
					todo = j->ri - 1;
				}
				if (prog_block(j, c, (long)by * c->bw + bx) != IMG_OK)
					return 0;
			}
	} else {
		int mx, my;

		for (my = 0; my < j->mcus_y; my++)
			for (mx = 0; mx < j->mcus_x; mx++) {
				if (j->ri && todo-- == 0) {
					if (restart(j) != IMG_OK)
						return 0;
					todo = j->ri - 1;
				}
				for (i = 0; i < n; i++) {
					struct comp *c = &j->c[j->scomp[i]];
					int bx, by;

					for (by = 0; by < c->v; by++)
						for (bx = 0; bx < c->h; bx++)
							if ((r = prog_block(j, c,
								((long)my * c->v + by) * c->bw
								+ mx * c->h + bx)) != IMG_OK)
								return 0;
				}
			}
	}
	j->scans++;
	return scan_end(j, j->pos);
}

/* the coefficients, all scans read: into pixels, an MCU row at a time
 * (from a corner smaller than shown: each pixel f x f) */
static int prog_output(struct img_dec *d, struct jpeg *j)
{
	int my, i, r, S = j->S, S2 = j->S2, f = j->S / j->S2;
	unsigned char tmp[64];

	for (my = 0; my < j->mcus_y; my++) {
		for (i = 0; i < j->ncomp; i++) {
			struct comp *c = &j->c[i];
			const unsigned short *q = j->qt[c->tq];
			int bx, by;

			for (by = 0; by < c->v; by++)
				for (bx = 0; bx < c->bw; bx++) {
					const short *v = c->coef
						+ (((long)my * c->v + by) * c->bw + bx) * j->K;
					unsigned char *o = c->plane + (size_t)by * S * (size_t)c->pw
						+ (size_t)bx * S;
					unsigned char *out = f > 1 ? tmp : o;
					int stride = f > 1 ? S2 : c->pw;
					long *co = j->co;
					int k, any = 0;

					for (k = 0; k <= j->maxk; k++)
						if (j->kidx[k] > 0 && v[j->kidx[k]]) {
							co[zz[k]] = coef((long)v[j->kidx[k]] * q[k]);
							any = 1;
						}
					co[0] = coef((long)v[0] * q[0]);
					if (!any) {
						/* the DC term alone: a flat block */
						unsigned char flat = clamp(DESCALE(co[0], 3) + 128);
						int y, x;

						for (y = 0; y < S; y++, o += c->pw)
							for (x = 0; x < S; x++)
								o[x] = flat;
					} else {
						int y, x;

						if (S2 == 8)
							idct(co, out, stride);
						else if (S2 == 4)
							idct4(co, out, stride);
						else
							idct2(co, out, stride);
						/* (each pixel f x f) */
						for (y = 0; f > 1 && y < S; y++, o += c->pw)
							for (x = 0; x < S; x++)
								o[x] = tmp[(y / f) * S2 + x / f];
					}
					for (k = 0; k <= j->maxk; k++)
						if (j->kidx[k] >= 0)
							co[zz[k]] = 0;
				}
		}
		if ((r = emit(d, j, my)) != IMG_OK)
			return r;
	}
	return IMG_END;
}

int jpeg_finish(struct img_dec *d)
{
	struct jpeg *j = d->fmt;
	size_t at = 2;

	if (j->len < 4 || img_sniff(j->buf, j->len) != IMG_JPEG)
		return IMG_BAD;
	for (;;) {
		int m, len, r = IMG_OK;
		size_t end;

		while (at < j->len && j->buf[at] != 0xFF)
			at++;			/* (junk between markers) */
		while (at < j->len && j->buf[at] == 0xFF)
			at++;
		if (at >= j->len)		/* (cut short: what came shows) */
			return j->prog && j->scans ? prog_output(d, j) : IMG_BAD;
		m = j->buf[at++];
		if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01)
			continue;
		if (m == 0xD9)			/* the end */
			return j->prog && j->scans ? prog_output(d, j) : IMG_BAD;
		if ((len = u16(j, at)) < 2 || at + (size_t)len > j->len)
			return IMG_BAD;
		end = at + (size_t)len;
		at += 2;
		switch (m) {
		case 0xC0: case 0xC1: case 0xC2:
			if (j->w)
				return IMG_BAD;	/* a second frame */
			j->prog = m == 0xC2;
			r = read_sof(d, j, at, end);
			break;
		case 0xC3: case 0xC5: case 0xC6: case 0xC7:
		case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
			return IMG_UNSUPPORTED;
		case 0xC4:
			r = read_dht(j, at, end);
			break;
		case 0xDB:
			r = read_dqt(j, at, end);
			break;
		case 0xDD:
			j->ri = u16(j, at);
			break;
		case 0xDA:
			if (!j->prog)
				return read_sos(d, j, at, end);
			if (j->w == 0 || !j->qset)
				return IMG_BAD;
			if ((at = prog_scan(j, at, end)) == 0)
				/* (a scan in error: what came before it shows) */
				return j->scans ? prog_output(d, j) : IMG_BAD;
			continue;
		default:
			break;			/* APPn, COM... */
		}
		if (r != IMG_OK)
			return r;
		at = end;
	}
}
