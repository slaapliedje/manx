/*
 * image.h - image decoders: GIF (the first frame), PNG and baseline JPEG.
 *
 * Bytes go in as they arrive, in pieces of any size; rows of RGBA pixels
 * come out to a sink as they are decoded, so that no whole image need be
 * held in full colour (a 640x480 one would be 1.2 MB, more than the
 * browser's budget for all its images). The sink scales and dithers each
 * row to the screen as it comes.
 *
 * GIF and PNG decode as the bytes come. JPEG keeps the file (up to the
 * cap) and decodes at the end, at full size or, if the sink asks, at 1/2,
 * 1/4 or 1/8 of it, which saves a 68030 most of the work when the image
 * is to be shown smaller.
 */
#ifndef MANX_IMAGE_H
#define MANX_IMAGE_H

#include <stddef.h>

enum img_type { IMG_NONE, IMG_GIF, IMG_PNG, IMG_JPEG };

enum {
	IMG_OK = 0,		/* feed more */
	IMG_END = 1,		/* the image is complete (later bytes ignored) */
	IMG_BAD = -1,		/* corrupt or truncated */
	IMG_STOP = -2,		/* the sink asked to stop */
	IMG_UNSUPPORTED = -3,	/* e.g. a progressive JPEG */
	IMG_TOOBIG = -4		/* over the memory cap */
};

struct img_info {
	int w, h;		/* in pixels, as decoded (JPEG: after scaling) */
	int full_w, full_h;	/* the image's own size */
	int alpha;		/* it has transparent pixels */
	int scalable;		/* the decoder honours *shift (a JPEG) */
};

struct img_sink {
	/*
	 * The size is known. For a JPEG the sink may set *shift to 1, 2 or
	 * 3 to have it decoded at 1/2, 1/4 or 1/8 of full_w/full_h: it is
	 * then (full_w + (1 << shift) - 1) >> shift pixels wide, and as
	 * much smaller in height. Formats that can't (info->scalable 0)
	 * ignore *shift. 0, or -1 to stop.
	 */
	int (*size)(void *ctx, struct img_info *info, int *shift);
	/*
	 * Row y (0 to h-1): w pixels of R, G, B, A (A 0: transparent, 255:
	 * opaque). Rows may come in any order (an interlaced GIF's passes);
	 * a row never sent stays transparent. 0, or -1 to stop.
	 */
	int (*row)(void *ctx, int y, const unsigned char *rgba);
	void *ctx;
};

struct img_dec;

/* What the first bytes are (8 are enough). */
enum img_type img_sniff(const unsigned char *b, size_t n);

/* A decoder of type t into sink, using no more than cap bytes of memory
 * for its own state and buffers (0: a default of 512 KB); NULL when out
 * of memory. */
struct img_dec *img_new(enum img_type t, const struct img_sink *sink,
	size_t cap);

/* More bytes: IMG_OK, IMG_END, or an error (negative). */
int img_feed(struct img_dec *d, const unsigned char *b, size_t n);

/* The bytes ended: IMG_END when the image was complete, else an error. */
int img_finish(struct img_dec *d);

void img_free(struct img_dec *d);

#endif /* MANX_IMAGE_H */
