/*
 * image.c - the decoders' common part: sniffing, the memory budget, and
 * passing bytes to the format.
 */
#include <string.h>
#include "os.h"
#include "image_int.h"

#define DEFAULT_CAP	(512UL * 1024)

enum img_type img_sniff(const unsigned char *b, size_t n)
{
	if (n >= 6 && memcmp(b, "GIF8", 4) == 0 && (b[4] == '7' || b[4] == '9')
		&& b[5] == 'a')
		return IMG_GIF;
	if (n >= 8 && memcmp(b, "\211PNG\r\n\032\n", 8) == 0)
		return IMG_PNG;
	if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
		return IMG_JPEG;
	return IMG_NONE;
}

void *img_alloc(struct img_dec *d, size_t n)
{
	void *p;

	if (n > d->cap || d->used > d->cap - n)
		return NULL;
	if ((p = xmalloc(n)) == NULL)
		return NULL;
	d->used += n;
	return p;
}

void img_release(struct img_dec *d, void *p, size_t n)
{
	if (p == NULL)
		return;
	xfree(p);
	d->used -= n < d->used ? n : d->used;
}

const unsigned char *img_gather(struct img_dec *d, const unsigned char **b,
	size_t *len, size_t n)
{
	size_t k;

	if (d->want != n) {
		/* a new structure */
		d->want = n;
		d->held = 0;
	}
	if (d->held == 0 && *len >= n) {
		/* all there in the input: no copy */
		const unsigned char *p = *b;

		*b += n;
		*len -= n;
		d->want = 0;
		return p;
	}
	if (n > d->hold_cap) {
		unsigned char *q = img_alloc(d, n);

		if (q == NULL)
			return NULL;
		if (d->held)
			memcpy(q, d->hold, d->held);
		img_release(d, d->hold, d->hold_cap);
		d->hold = q;
		d->hold_cap = n;
	}
	k = n - d->held < *len ? n - d->held : *len;
	memcpy(d->hold + d->held, *b, k);
	d->held += k;
	*b += k;
	*len -= k;
	if (d->held < n)
		return NULL;
	d->want = 0;
	d->held = 0;
	return d->hold;
}

struct img_dec *img_new(enum img_type t, const struct img_sink *sink,
	size_t cap)
{
	struct img_dec *d;
	int r;

	if (t != IMG_GIF && t != IMG_PNG && t != IMG_JPEG)
		return NULL;
	if ((d = xmalloc(sizeof *d)) == NULL)
		return NULL;
	memset(d, 0, sizeof *d);
	d->type = t;
	d->sink = *sink;
	d->cap = cap ? cap : DEFAULT_CAP;
	r = t == IMG_GIF ? gif_new(d) : t == IMG_PNG ? png_new(d) : jpeg_new(d);
	if (r < 0) {
		img_free(d);
		return NULL;
	}
	return d;
}

int img_feed(struct img_dec *d, const unsigned char *b, size_t n)
{
	if (d->result != IMG_OK)
		return d->result;
	d->result = d->type == IMG_GIF ? gif_feed(d, b, n)
		: d->type == IMG_PNG ? png_feed(d, b, n) : jpeg_feed(d, b, n);
	return d->result;
}

int img_finish(struct img_dec *d)
{
	if (d->result != IMG_OK)
		return d->result == IMG_END ? IMG_END : d->result;
	d->result = d->type == IMG_GIF ? gif_finish(d)
		: d->type == IMG_PNG ? png_finish(d) : jpeg_finish(d);
	return d->result;
}

void img_free(struct img_dec *d)
{
	if (d == NULL)
		return;
	if (d->fmt)
		switch (d->type) {
		case IMG_GIF: gif_free(d); break;
		case IMG_PNG: png_free(d); break;
		case IMG_JPEG: jpeg_free(d); break;
		default: break;
		}
	img_release(d, d->hold, d->hold_cap);
	xfree(d);
}
