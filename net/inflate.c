/*
 * inflate.c - see inflate.h.
 *
 * Streaming without coroutines: each step (a block header, a symbol with
 * its extra bits, a trailer) runs only when the buffered input holds all
 * the bits it can need, or when the input has ended. What a step didn't
 * use stays buffered for the next feed. Huffman codes are decoded with a
 * 9-bit lookup table, longer ones bit by bit (as in zlib's puff).
 */
#include <stddef.h>
#include <string.h>
#include "os.h"
#include "inflate.h"

#define WSIZE		32768UL
#define WMASK		(WSIZE - 1)
#define IBUF		8192
#define LUTBITS		9
#define MAXBITS		15

/* the most bits a step can need */
#define NEED_SYMBOL	48		/* length code + extra + distance + extra */
#define NEED_DYNAMIC	4600		/* a dynamic block's code lengths */

struct huff {
	unsigned short count[MAXBITS + 1];
	unsigned short symbol[320];
	unsigned short lut[1 << LUTBITS];	/* sym | len << 9; 0: longer */
};

enum {
	S_WRAPPER, S_GZ_EXTRA, S_GZ_NAME, S_GZ_COMMENT, S_GZ_HCRC, S_BLOCK,
	S_STORED, S_HUFF, S_TRAILER, S_END, S_BAD
};

struct inflate {
	int (*out)(void *ctx, const unsigned char *data, size_t n);
	void *ctx;
	int format, zlib, state, last, finishing, stopped;
	int gz_flags;
	unsigned long bitbuf;
	int bitcnt;
	size_t ilen, ipos;
	unsigned long wpos, flushed;	/* bytes written / handed out */
	unsigned long stored_left;
	unsigned long crc, adler;
	const struct huff *lit, *dist;
	struct huff dlit, ddist;
	unsigned char in[IBUF];
	unsigned char win[WSIZE];
};

/* --- checksums -------------------------------------------------------- */

static unsigned long crc_table[256];

static void make_crc_table(void)
{
	unsigned long c;
	int n, k;

	for (n = 0; n < 256; n++) {
		c = (unsigned long)n;
		for (k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

static unsigned long crc32_update(unsigned long crc, const unsigned char *p,
	size_t n)
{
	crc = ~crc & 0xFFFFFFFFUL;
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc & 0xFFFFFFFFUL;
}

static unsigned long adler_update(unsigned long a, const unsigned char *p,
	size_t n)
{
	unsigned long s1 = a & 0xFFFF, s2 = a >> 16;

	while (n) {
		size_t k = n < 3800 ? n : 3800;	/* no overflow before % */

		n -= k;
		while (k--) {
			s1 += *p++;
			s2 += s1;
		}
		s1 %= 65521UL;
		s2 %= 65521UL;
	}
	return (s2 << 16) | s1;
}

/* --- output ----------------------------------------------------------- */

/* hand what's in the window and not yet out to the callback */
static int flush(struct inflate *z)
{
	while (z->flushed < z->wpos) {
		size_t at = (size_t)(z->flushed & WMASK);
		size_t n = (size_t)(z->wpos - z->flushed);

		if (n > WSIZE - at)
			n = WSIZE - at;
		if (z->format == INF_GZIP)
			z->crc = crc32_update(z->crc, z->win + at, n);
		else if (z->zlib)
			z->adler = adler_update(z->adler, z->win + at, n);
		z->flushed += n;
		if (!z->stopped && z->out(z->ctx, z->win + at, n) < 0)
			z->stopped = 1;
	}
	return z->stopped ? -1 : 0;
}

#define PUT(z, b) do { \
	if ((z)->wpos - (z)->flushed == WSIZE && flush(z) < 0) \
		return INF_STOP; \
	(z)->win[(z)->wpos++ & WMASK] = (unsigned char)(b); \
} while (0)

/* --- bits ------------------------------------------------------------- */

static unsigned long avail_bits(const struct inflate *z)
{
	return (unsigned long)z->bitcnt + 8UL * (z->ilen - z->ipos);
}

static void refill(struct inflate *z)
{
	while (z->bitcnt <= 24 && z->ipos < z->ilen) {
		z->bitbuf |= (unsigned long)z->in[z->ipos++] << z->bitcnt;
		z->bitcnt += 8;
	}
}

/* n bits (n <= 16), or -1 when the input has run out */
static long bits(struct inflate *z, int n)
{
	unsigned long v;

	if (z->bitcnt < n)
		refill(z);
	if (z->bitcnt < n)
		return -1;
	v = z->bitbuf & ((1UL << n) - 1);
	z->bitbuf >>= n;
	z->bitcnt -= n;
	return (long)v;
}

/* drop the bits up to the next byte boundary */
static void align(struct inflate *z)
{
	z->bitbuf >>= z->bitcnt & 7;
	z->bitcnt -= z->bitcnt & 7;
}

/* --- Huffman codes ---------------------------------------------------- */

/* build h from code lengths; 0, or -1 if they don't form a usable code */
static int build(struct huff *h, const unsigned char *len, int n)
{
	unsigned short offs[MAXBITS + 1];
	int sym, l, left;
	unsigned code, first, index;

	memset(h->count, 0, sizeof h->count);
	for (sym = 0; sym < n; sym++)
		h->count[len[sym]]++;
	if (h->count[0] == n) {
		/* no codes at all: only valid for an unused distance code */
		memset(h->lut, 0, sizeof h->lut);
		return 0;
	}
	left = 1;
	for (l = 1; l <= MAXBITS; l++) {
		left <<= 1;
		left -= h->count[l];
		if (left < 0)
			return -1;		/* over-subscribed */
	}
	offs[1] = 0;
	for (l = 1; l < MAXBITS; l++)
		offs[l + 1] = (unsigned short)(offs[l] + h->count[l]);
	for (sym = 0; sym < n; sym++)
		if (len[sym])
			h->symbol[offs[len[sym]]++] = (unsigned short)sym;

	/* the lookup table: every code up to LUTBITS long, bit-reversed as
	 * the stream sends it, repeated over the bits past it */
	memset(h->lut, 0, sizeof h->lut);
	code = first = index = 0;
	for (l = 1; l <= LUTBITS; l++) {
		int c;

		for (c = 0; c < h->count[l]; c++) {
			unsigned cd = first + (unsigned)c, rev = 0, k;
			unsigned short e = (unsigned short)(h->symbol[index + (unsigned)c]
				| (l << 9));

			for (k = 0; k < (unsigned)l; k++)
				rev |= ((cd >> k) & 1) << (l - 1 - k);
			for (k = rev; k < (1U << LUTBITS); k += 1U << l)
				h->lut[k] = e;
		}
		index += h->count[l];
		first = (first + h->count[l]) << 1;
	}
	(void)code;
	return 0;
}

/* one symbol, or -1 (out of input, or not a code) */
static int decode(struct inflate *z, const struct huff *h)
{
	unsigned e;
	int code, first, index, count, l;

	refill(z);
	e = h->lut[z->bitbuf & ((1U << LUTBITS) - 1)];
	if (e && (int)(e >> 9) <= z->bitcnt) {
		z->bitbuf >>= e >> 9;
		z->bitcnt -= (int)(e >> 9);
		return (int)(e & 0x1FF);
	}
	/* a longer code: bit by bit */
	code = first = index = 0;
	for (l = 1; l <= MAXBITS; l++) {
		long b = bits(z, 1);

		if (b < 0)
			return -1;
		code |= (int)b;
		count = h->count[l];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

static struct huff fixed_lit, fixed_dist;
static int fixed_ready;

static void make_fixed(void)
{
	unsigned char len[288];
	int i;

	for (i = 0; i < 144; i++)
		len[i] = 8;
	for (; i < 256; i++)
		len[i] = 9;
	for (; i < 280; i++)
		len[i] = 7;
	for (; i < 288; i++)
		len[i] = 8;
	build(&fixed_lit, len, 288);
	for (i = 0; i < 30; i++)
		len[i] = 5;
	build(&fixed_dist, len, 30);
	make_crc_table();
	fixed_ready = 1;
}

/* a dynamic block's code length codes, then its two codes */
static int dynamic(struct inflate *z)
{
	static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10,
		5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	unsigned char len[320];
	struct huff lencode;
	long nlen, ndist, ncode, v;
	int i;

	if ((nlen = bits(z, 5)) < 0 || (ndist = bits(z, 5)) < 0
		|| (ncode = bits(z, 4)) < 0)
		return -1;
	nlen += 257;
	ndist += 1;
	ncode += 4;
	if (nlen > 286 || ndist > 30)
		return -1;
	memset(len, 0, 19);
	for (i = 0; i < ncode; i++) {
		if ((v = bits(z, 3)) < 0)
			return -1;
		len[order[i]] = (unsigned char)v;
	}
	if (build(&lencode, len, 19) < 0)
		return -1;
	for (i = 0; i < nlen + ndist; ) {
		int sym = decode(z, &lencode), rep, prev = 0;

		if (sym < 0)
			return -1;
		if (sym < 16) {
			len[i++] = (unsigned char)sym;
			continue;
		}
		if (sym == 16) {
			if (i == 0)
				return -1;
			prev = len[i - 1];
			rep = (int)bits(z, 2) + 3;
		} else if (sym == 17)
			rep = (int)bits(z, 3) + 3;
		else
			rep = (int)bits(z, 7) + 11;
		if (rep < 3 || i + rep > nlen + ndist)
			return -1;
		while (rep--)
			len[i++] = (unsigned char)prev;
	}
	if (len[256] == 0)
		return -1;			/* no end-of-block code */
	if (build(&z->dlit, len, (int)nlen) < 0
		|| build(&z->ddist, len + nlen, (int)ndist) < 0)
		return -1;
	z->lit = &z->dlit;
	z->dist = &z->ddist;
	return 0;
}

/* --- the decoder -------------------------------------------------------- */

static const unsigned short len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13,
	15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195,
	227, 258 };
static const unsigned char len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
	1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17,
	25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
	3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3,
	4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* run the state machine as far as the buffered input allows */
static int run(struct inflate *z)
{
	for (;;) {
		long v;

		switch (z->state) {
		case S_WRAPPER:
			if (!z->finishing && avail_bits(z) < 80)
				return INF_OK;
			if (z->format == INF_GZIP) {
				long id1 = bits(z, 8), id2 = bits(z, 8),
					cm = bits(z, 8), flg = bits(z, 8);

				if (id1 != 0x1F || id2 != 0x8B || cm != 8 || flg < 0)
					return INF_BAD;
				z->gz_flags = (int)flg;
				for (v = 0; v < 6; v++)	/* mtime, xfl, os */
					if (bits(z, 8) < 0)
						return INF_BAD;
				z->state = S_GZ_EXTRA;
			} else {
				/* zlib: CMF FLG with (CMF*256+FLG) % 31 == 0 and
				 * method 8; else raw deflate (some servers) */
				unsigned long b = z->bitbuf;

				refill(z);
				b = z->bitbuf;
				if (z->bitcnt >= 16 && (b & 0x0F) == 8
					&& (((b & 0xFF) << 8) | ((b >> 8) & 0xFF)) % 31 == 0
					&& !(b & 0x2000)) {
					bits(z, 16);
					z->zlib = 1;
					z->adler = 1;
				}
				z->state = S_BLOCK;
			}
			break;
		case S_GZ_EXTRA:
			if (z->gz_flags & 4) {
				/* the extra field: its length, then skip it */
				if (!(z->gz_flags & 0x100)) {
					if (!z->finishing && avail_bits(z) < 16)
						return INF_OK;
					if ((v = bits(z, 16)) < 0)
						return INF_BAD;
					z->stored_left = (unsigned long)v;
					z->gz_flags |= 0x100;
				}
				while (z->stored_left) {
					if (bits(z, 8) < 0)
						return z->finishing ? INF_BAD : INF_OK;
					z->stored_left--;
				}
				z->gz_flags &= ~4;
			}
			z->state = S_GZ_NAME;
			break;
		case S_GZ_NAME:
		case S_GZ_COMMENT:
			if (z->gz_flags & (z->state == S_GZ_NAME ? 8 : 16)) {
				/* a zero-terminated string: skip it */
				while ((v = bits(z, 8)) > 0)
					;
				if (v < 0)
					return z->finishing ? INF_BAD : INF_OK;
			}
			z->state = z->state == S_GZ_NAME ? S_GZ_COMMENT : S_GZ_HCRC;
			break;
		case S_GZ_HCRC:
			if ((z->gz_flags & 2) && bits(z, 16) < 0)
				return z->finishing ? INF_BAD : INF_OK;
			z->crc = 0;
			z->state = S_BLOCK;
			break;
		case S_BLOCK:
			if (z->last) {
				z->state = S_TRAILER;
				break;
			}
			if (!z->finishing && avail_bits(z) < NEED_DYNAMIC)
				return INF_OK;
			if ((v = bits(z, 3)) < 0)
				return INF_BAD;
			z->last = (int)(v & 1);
			switch (v >> 1) {
			case 0: {
				long len, nlen;

				align(z);
				if ((len = bits(z, 16)) < 0 || (nlen = bits(z, 16)) < 0
					|| (len ^ 0xFFFF) != nlen)
					return INF_BAD;
				z->stored_left = (unsigned long)len;
				z->state = S_STORED;
				break;
			}
			case 1:
				z->lit = &fixed_lit;
				z->dist = &fixed_dist;
				z->state = S_HUFF;
				break;
			case 2:
				if (dynamic(z) < 0)
					return INF_BAD;
				z->state = S_HUFF;
				break;
			default:
				return INF_BAD;
			}
			break;
		case S_STORED:
			while (z->stored_left) {
				if ((v = bits(z, 8)) < 0)
					return z->finishing ? INF_BAD : INF_OK;
				PUT(z, v);
				z->stored_left--;
			}
			z->state = S_BLOCK;
			break;
		case S_HUFF:
			for (;;) {
				int sym;
				long len, d;

				if (!z->finishing && avail_bits(z) < NEED_SYMBOL)
					return INF_OK;
				if ((sym = decode(z, z->lit)) < 0)
					return INF_BAD;
				if (sym < 256) {
					PUT(z, sym);
					continue;
				}
				if (sym == 256)
					break;
				sym -= 257;
				if (sym >= 29)
					return INF_BAD;
				len = len_base[sym];
				if (len_extra[sym]) {
					if ((v = bits(z, len_extra[sym])) < 0)
						return INF_BAD;
					len += v;
				}
				if ((sym = decode(z, z->dist)) < 0 || sym >= 30)
					return INF_BAD;
				d = dist_base[sym];
				if (dist_extra[sym]) {
					if ((v = bits(z, dist_extra[sym])) < 0)
						return INF_BAD;
					d += v;
				}
				if ((unsigned long)d > z->wpos)
					return INF_BAD;	/* before the start */
				while (len--) {
					unsigned char b = z->win[(z->wpos - (unsigned long)d)
						& WMASK];

					PUT(z, b);
				}
			}
			z->state = S_BLOCK;
			break;
		case S_TRAILER: {
			unsigned long want = z->format == INF_GZIP ? 64 : z->zlib ? 32 : 0;

			if (!z->finishing && avail_bits(z) < want
				+ (unsigned long)(z->bitcnt & 7))
				return INF_OK;
			if (flush(z) < 0)
				return INF_STOP;
			align(z);
			if (z->format == INF_GZIP) {
				long c0 = bits(z, 16), c1 = bits(z, 16);
				long s0 = bits(z, 16), s1 = bits(z, 16);

				if (c0 < 0 || s1 < 0 || ((unsigned long)c1 << 16 | (unsigned long)c0)
					!= z->crc || ((unsigned long)s1 << 16 | (unsigned long)s0)
					!= (z->wpos & 0xFFFFFFFFUL))
					return INF_BAD;
			} else if (z->zlib) {
				unsigned long a = 0;
				int i;

				for (i = 0; i < 4; i++) {
					if ((v = bits(z, 8)) < 0)
						return INF_BAD;
					a = a << 8 | (unsigned long)v;
				}
				if (a != z->adler)
					return INF_BAD;
			}
			z->state = S_END;
			return INF_END;
		}
		case S_END:
			return INF_END;
		default:
			return INF_BAD;
		}
	}
}

struct inflate *inflate_new(int format,
	int (*out)(void *ctx, const unsigned char *data, size_t n), void *ctx)
{
	struct inflate *z;

	if (!fixed_ready)
		make_fixed();
	z = xmalloc(sizeof *z);
	if (z == NULL)
		return NULL;
	memset(z, 0, offsetof(struct inflate, in));
	z->out = out;
	z->ctx = ctx;
	z->format = format;
	z->state = S_WRAPPER;
	return z;
}

static int step(struct inflate *z)
{
	int rc = run(z);

	if (rc == INF_BAD)
		z->state = S_BAD;
	if (rc == INF_OK && flush(z) < 0)
		rc = INF_STOP;
	if (rc == INF_STOP)
		z->stopped = 1;
	/* keep what wasn't used */
	if (z->ipos) {
		memmove(z->in, z->in + z->ipos, z->ilen - z->ipos);
		z->ilen -= z->ipos;
		z->ipos = 0;
	}
	return rc;
}

int inflate_feed(struct inflate *z, const unsigned char *in, size_t n)
{
	int rc = INF_OK;

	if (z->state == S_END)
		return INF_END;
	if (z->state == S_BAD)
		return INF_BAD;
	if (z->stopped)
		return INF_STOP;
	while (n) {
		size_t k = IBUF - z->ilen;

		if (k > n)
			k = n;
		memcpy(z->in + z->ilen, in, k);
		z->ilen += k;
		in += k;
		n -= k;
		rc = step(z);
		if (rc != INF_OK)
			return rc;
	}
	return rc;
}

int inflate_finish(struct inflate *z)
{
	int rc;

	if (z->state == S_END)
		return INF_END;
	if (z->state == S_BAD || z->stopped)
		return z->stopped ? INF_STOP : INF_BAD;
	z->finishing = 1;
	rc = step(z);
	if (rc == INF_END)
		return INF_END;
	return rc == INF_STOP ? INF_STOP : INF_BAD;
}

void inflate_free(struct inflate *z)
{
	xfree(z);
}
