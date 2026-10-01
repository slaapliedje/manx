/*
 * pixels.h - the sink between an image decoder and the screen: each row
 * of RGBA scaled to the size shown, dithered to the colours the screen
 * has, and packed the way the screen's server packs pixels (so that X
 * takes it as it is), a row at a time as the decoder sends them.
 *
 * The caller fills in a px_out, gets an img_sink for it from px_sink(),
 * gives that to img_new(), and after the decoder is done calls
 * px_finish() (for a row still being gathered when an image ends early)
 * and px_free().
 */
#ifndef MANX_PIXELS_H
#define MANX_PIXELS_H

#include <stddef.h>
#include "image.h"

enum px_kind {
	PX_TRUE,		/* TrueColor: each channel in its mask */
	PX_CUBE,		/* a colour cube, levels[] steps of r, g, b */
	PX_GRAY			/* levels[0] greys; 2: black and white */
};

struct px_format {
	enum px_kind kind;
	int bpp;		/* bits a packed pixel takes: 1, 8, 16, 24 or 32 */
	int byte_msb;		/* 16-32 bpp: the most significant byte first */
	int bit_msb;		/* 1 bpp, and the mask: the leftmost pixel in
				 * a byte's top bit */
	unsigned long mask[3];	/* PX_TRUE: red, green, blue */
	int levels[3];		/* PX_CUBE: r, g, b (each 2 or more, 256 or
				 * fewer colours); PX_GRAY: [0], 2-256 */
	/* PX_CUBE: the pixel of colour (r * levels[1] + g) * levels[2] + b;
	 * PX_GRAY: the pixel of grey i, black first */
	unsigned long pixel[256];
};

struct px_state;

struct px_out {
	const struct px_format *fmt;
	int want_w, want_h;	/* the size to show it at: 0, 0 its own; one
				 * of them 0, in proportion to the other */
	int max_w, max_h;	/* and no bigger (0: no limit), in proportion */
	/* the size it will be, and whether rows come with a mask (it has
	 * transparent pixels): 0, or -1 to stop */
	int (*size)(void *ctx, int w, int h, int masked);
	/* row y: w pixels packed, and with a mask w bits (1: shown);
	 * 0, or -1 to stop */
	int (*row)(void *ctx, int y, const unsigned char *px,
		const unsigned char *mask);
	void *ctx;
	struct px_state *st;	/* (its own) */
};

/* Set s up to feed o. */
void px_sink(struct px_out *o, struct img_sink *s);

/* The image ended: send a row still being gathered (one cut short).
 * 0, or -1 if the row callback said to stop. */
int px_finish(struct px_out *o);

void px_free(struct px_out *o);

/* Bytes in a packed row of w pixels (and (w + 7) / 8 in a mask row). */
size_t px_row_bytes(const struct px_format *f, int w);

#endif /* MANX_PIXELS_H */
