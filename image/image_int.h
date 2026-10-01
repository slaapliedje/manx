/*
 * image_int.h - what the decoders share: the decoder's common part, a
 * memory budget, and a buffer that gathers a structure of n bytes from
 * input that may come in pieces.
 */
#ifndef MANX_IMAGE_INT_H
#define MANX_IMAGE_INT_H

#include "image.h"

#define IMG_MAX_SIDE	8192		/* no image is wider or taller */

struct img_dec {
	enum img_type type;
	struct img_sink sink;
	size_t cap, used;		/* memory: allowed, taken */
	int result;			/* IMG_OK until the end or an error */
	void *fmt;			/* the format's own state */
	/* gathering: hold[0..held) of want bytes */
	unsigned char *hold;
	size_t held, want, hold_cap;
};

/* memory from the budget (NULL when over it, or out of memory) */
void *img_alloc(struct img_dec *d, size_t n);
void img_release(struct img_dec *d, void *p, size_t n);

/*
 * Gather n bytes from *b (advancing it and *len): the whole structure
 * when they are all there, else NULL (more input needed). n is at most
 * the hold buffer, which grows within the budget.
 */
const unsigned char *img_gather(struct img_dec *d, const unsigned char **b,
	size_t *len, size_t n);

/* the formats */
int gif_new(struct img_dec *d);
int gif_feed(struct img_dec *d, const unsigned char *b, size_t n);
int gif_finish(struct img_dec *d);
void gif_free(struct img_dec *d);

int png_new(struct img_dec *d);
int png_feed(struct img_dec *d, const unsigned char *b, size_t n);
int png_finish(struct img_dec *d);
void png_free(struct img_dec *d);

int jpeg_new(struct img_dec *d);
int jpeg_feed(struct img_dec *d, const unsigned char *b, size_t n);
int jpeg_finish(struct img_dec *d);
void jpeg_free(struct img_dec *d);

#endif /* MANX_IMAGE_INT_H */
