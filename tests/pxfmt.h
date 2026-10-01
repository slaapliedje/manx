/*
 * pxfmt.h - screen formats by name, for the image tools (imgconv,
 * bench_image): true (the TT's Xatw -depth 32), 565, cube6, cube4,
 * gray16, mono. Pixel values are the cube or grey index.
 */
#include <stdlib.h>
#include <string.h>
#include "pixels.h"

static int px_format_named(struct px_format *f, const char *name)
{
	int i;

	memset(f, 0, sizeof *f);
	f->byte_msb = f->bit_msb = 1;
	if (strcmp(name, "565") == 0) {
		f->kind = PX_TRUE;
		f->bpp = 16;
		f->mask[0] = 0xf800;
		f->mask[1] = 0x7e0;
		f->mask[2] = 0x1f;
	} else if (strcmp(name, "cube6") == 0 || strcmp(name, "cube4") == 0) {
		f->kind = PX_CUBE;
		f->bpp = 8;
		f->levels[0] = f->levels[1] = f->levels[2] = name[4] - '0';
	} else if (strcmp(name, "gray16") == 0 || strcmp(name, "mono") == 0) {
		f->kind = PX_GRAY;
		f->bpp = name[0] == 'm' ? 1 : 8;
		f->levels[0] = name[0] == 'm' ? 2 : 16;
	} else if (strcmp(name, "true") == 0) {
		f->kind = PX_TRUE;
		f->bpp = 32;
		f->mask[0] = 0xff000000UL;
		f->mask[1] = 0xff0000UL;
		f->mask[2] = 0xff00UL;
	} else
		return -1;
	for (i = 0; i < 256; i++)
		f->pixel[i] = (unsigned long)i;
	return 0;
}
