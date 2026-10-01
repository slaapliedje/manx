# Phase 6 results: images (in progress)

## 6a: the decoders

| Module | What it does |
|---|---|
| `image/image` | the API: `img_sniff` by magic bytes; `img_new(type, sink, budget)`, `img_feed` in pieces of any size, `img_finish`, `img_free`. The sink learns the size first (and, for a JPEG, may ask for 1/2, 1/4 or 1/8 of it), then takes the picture a row at a time as RGBA. All memory comes from a per-image budget (512 KB by default); past it, `IMG_TOOBIG`. Sides over 8192 are refused |
| `image/gif` | GIF87a/89a, the first frame. LZW is decoded as the bytes come, and rows go out as they fill, in the order an interlaced image sends them. Transparency from the graphic control extension; a frame smaller than the screen is placed on a transparent one |
| `image/png` | every colour type and bit depth (1-16), palette and key transparency (tRNS), all five filters, Adam7 interlacing (into a whole-image buffer, within the budget). IDAT goes through `net/inflate` as it arrives, and rows go out as they unfilter. CRCs aren't checked: the zlib stream's Adler-32 is |
| `image/jpeg` | baseline and extended sequential Huffman JPEG: greyscale or YCbCr, sampling up to 2x2, restart markers. The file is kept and decoded at its end, one MCU row at a time. libjpeg's exact integer IDCT (pixels match its decoder); at 1/2 and 1/4 size a reduced IDCT of the low frequencies, at 1/8 the DC terms alone. Chroma is upsampled by replication. Progressive, arithmetic-coded, 12-bit, CMYK and multi-scan files are `IMG_UNSUPPORTED` |

The decoders are plain C89 with no 64-bit types, so INMOS icc can
build them for the transputers as they are.

## Tests

| Test | Cases |
|---|---|
| `test_image` | 34 images from `tests/gen_images.py`: 19 PNGs from its own encoder (every colour type and depth, Adam7, every filter, tRNS), 5 GIFs and 10 JPEGs from Pillow, against the pixels Pillow (libjpeg) decodes. Each is decoded whole and in random pieces, then cut short and corrupted (no crash, no row outside the image). Exact except JPEG: 4:2:2/4:2:0 within 12 of libjpeg's triangle-filtered chroma and 1/2 and 1/4 size within 12 of its reduced IDCTs (both on photo-like pictures; on colour changing every pixel they differ by up to ~90), 1/8 within 2 |
| `fuzz_image` | ASan/UBSan: the test images mutated (bit flips, bytes changed, inserted, deleted, cut short), fed in random pieces, random scales, random budgets, sinks that stop early; no crash, no stray row, none over 2 s. 1.9 million iterations clean. `make fuzz` runs it after the HTML fuzzer |
| `bench_image` | ms per decode of a file, at a given scale |

The fuzzer found two problems, both fixed: negative values shifted left
in the IDCT (undefined; now multiplied), and a corrupt file's
coefficients overflowing the IDCT's 32-bit arithmetic (now clamped to
11 bits, as a valid 8-bit JPEG's always are).

## Speed on the TT

320x240 pictures (a blurred noisy photo; JPEG 4:2:0 at quality 75), ms
per decode:

| File | Host | TT030 |
|---|---|---|
| JPEG, 22.9 KB, full size | 3 | 2900-4600 |
| JPEG at 1/2, 1/4, 1/8 | | 1900, 1700, 890 |
| JPEG 640x480, 67 KB, full / 1/4 | | 16200 / 4900 |
| PNG RGB, 98 KB | 7 | 9200-12200 |
| GIF, 128 colours, 23.7 KB | 1 | 1300-1550 |

The JPEG decoder started at 7100 ms (full) and 1390 ms (1/8). What
helped on the 68030:
- colour conversion without a division per pixel (half the time);
- the Huffman loop kept small enough for the 68030's 256-byte
  instruction cache: the bit buffer in locals, the fast paths as macros,
  only long codes, 0xFF bytes and markers out of line;
- reduced IDCTs for 1/2 and 1/4 size, a fill for blocks with only a DC
  term, and no memset per block (AMIX's memset is slow).

What didn't:
- libjpeg's fast (AAN) IDCT, with 5 multiplies a pass instead of 12,
  measured slower: 3.9 s against 2.9 s;
- `-Os` was slower than `-O2` (4.75 s against 3.35 s).

The ranges are real. The same source runs 2.9 s or 4.6 s depending only
on where the linker puts the code: each IDCT pass, with GCC's
shift-and-add multiplies, is bigger than the instruction cache, and
where it falls decides how badly it thrashes. The same goes for PNG,
whose code didn't change between its two timings.

So on the TT the levers are:
- decoding at the size shown (a JPEG twice the window's width costs a
  third as much at 1/2);
- showing rows as they come;
- the transputers.

## 6b: the pixel sink

| Module | What it does |
|---|---|
| `image/pixels` | sits between a decoder and the screen: each row scaled to the size shown, dithered to the screen's colours and packed as its server packs pixels (1, 8, 16, 24 or 32 bits a pixel, either byte and bit order), so X takes it as it is. Size: the image's own, or `width`/`height` (one alone keeps the proportions), within a maximum (the pane's width). A JPEG decodes at the smallest of 1/2, 1/4, 1/8 that is still no smaller than shown. Scaling: a box filter, averaging as rows arrive in order; an interlaced GIF's rows come out of order, so there each target row is the nearest source row. Transparency becomes an X mask: a pixel is shown when at least half of what it covers is opaque. Colours: TrueColor straight through (any masks, the TT's 0xRRGGBBxx included); a colour cube, greys or black and white, and TrueColor channels under 8 bits, through an 8x8 ordered (Bayer) dither. That needs no state between rows, so any row order works, and costs a lookup and a comparison per channel |

The decoders now say when rows come out of order (`img_info.interlaced`,
set by GIF).

| Test | Cases |
|---|---|
| `test_pixels` | 125 checks: the TT's 32-bit and a PC's 24-bit packing; 16-bit 565 and colour cubes of 6x6x6, 8x8x4 and 2x2x2, where an 8x8 tile of a flat colour must average to that colour (within a 64th of a level); black and white with a server whose black is 1, and both bit orders; box means, uneven groups (10 to 4), growing; nine size cases; JPEG scale choice; an interlaced GIF's order halved and at full size; masks with scaling and the alpha threshold; stopping; and 3000 random sizes, in order and interlaced, each target row sent exactly once |
| `fuzz_image` | now half its iterations go through `image/pixels`, with a random format and size: 600,000 more clean |
| `imgconv` | an image as a screen would get it, back out as a PPM, to look at |
| `bench_image` | `-f FORMAT -w WIDTH`: time through `image/pixels` too |

A real photo (1184x664, shown 320 wide) in 24-bit, a 6x6x6 cube, 4x4x4
and black and white came out as they should. On the PC that takes 7 ms
(the JPEG decodes at 1/2, then the box filter). Two of the three JPEG
photos on the PC were progressive, which the decoder refuses: progressive
support will be needed for real pages.

## 6c-6e: images on the page

| Module | What it does |
|---|---|
| `layout` | with metrics (X), an `<img>` whose size is known is laid out as itself: `LAYOUT_IMG_BYTES` control bytes in a span of the new face `LF_IMAGE`, standing for `page.images[k]` (node, the size shown, its line). Its size comes from `width` and `height`, or one of them and the image's own proportions, or its own size (a new `lmetrics.image` callback), no wider than the line; it sits on the baseline, and lines break on either side of it. Until its size is known it is its `[alt]` text, as on a terminal. The terminal layout is unchanged (216 corpus dumps byte-identical) |
| `src/pageimg` | the page's images: every `<img src>` fetched after the page (the page as Referer), its file kept, its size read from the header (`img_probe`, new: GIF, PNG IHDR, JPEG SOFn), then decoded through `image/pixels` straight to the screen at the size shown, showing rows as they come. Those on screen first, then those of no known size (once one is known, the page is laid out again, at most every 2 s), then the rest, nearest first. `data:` URLs (base64) need no fetch; 1x1 tracking pixels aren't fetched; one that can't be decoded (a progressive JPEG, a WebP) goes back to its alt text. Files kept past 2 MB in all are dropped once shown. The terminal manx links `pageimg_none.c` instead, and no decoders |
| `src/manx.c` | image work is done while no key waits. During a fetch or a decode, keys that only move the view (scrolling, the wheel, the scrollbar, moving between links) are done at once; any other stops the image, which goes on later. `z` stops fetching more; the status line counts them; `images = off` in the config shows alt text only. Clicks land on linked images; the selected one is framed |
| `xscreen` | `scr_pixels()`: the format from a test XImage (bits per pixel, byte and bit order as the server has them). TrueColor straight, PseudoColor through a colour cube allocated in the shared colormap (6x6x6, else 5, 4, 3, 2 levels), GrayScale a ramp of greys, StaticGray its own, depth 1 black and white. An image is a pixmap (and a mask pixmap when it has transparency), filled a row at a time with XPutImage, drawn with XCopyArea through the mask; one not here yet is a frame. X errors no longer end the program: an image the server has no room for is simply not shown |

Tested in Xvfb at 24-bit, 8-bit PseudoColor and 8-bit StaticGray with a
page of every case: a JPEG given only a width, a transparent PNG of no
given size (laid out again when it came), a `data:` URL, a linked GIF, a
missing file and a progressive JPEG (their alt text), a tracking pixel,
an interlaced GIF. `fuzz_html` now lays images out in its made-up
proportional font (60,000 iterations clean). On the way it found an older
bug, fixed separately: an `<hr>` in a table cell lost what came before it.

## The displays

| Server | Depth | Images |
|---|---|---|
| ASV Xatw (the TT's desktop, as xdm starts it now) | 8 bpp PseudoColor through the card's LUT | dither to a colour cube |
| ASV Xatw `-depth 32` (4 MB card, up to 1024x768) | 32 bpp TrueColor, pixel 0xRRGGBBxx | direct; pixels built from the visual's masks, which here aren't the usual ones |
| AMIX Xdmi | 1 bpp | dither to black and white |

The card's 2D engine moves pixels on screen at 38 MB/s (scrolling), but
the CPU writes to the card over the VME bus at 0.9 MB/s. A 320x240
image is 77 KB at 8 bpp (85 ms to draw) and 300 KB at 32 bpp (330 ms).

## The transputers

The T800 in TRAM slot 1 (`/dev/link0`) and the T425 in the card's FPGA
(`/dev/link1`) each take a stream of compressed bytes and return rows.
The link moves about 100 KB/s, so rows dithered to 8-bit indexes
(1 byte a pixel) are the form to bring back. Phase 0 measured compiled C
1.5-3.7x slower on the T800 than on the 68030, but that was bignum
arithmetic, which leans on the T800's slow multiply. Huffman decoding is
shifts and table lookups. Measuring the decoders there comes next. Even
at the 68030's speed, a transputer decoding while the 68030 lays out the
page, or two of them taking an image each, is time the 68030 doesn't
spend.

## Left for 6

- On the TT (it was off the network): images at 24 bits through AMIX's
  R5 Xlib, and the time each step takes there; AMIX's Xdmi in black and
  white.
- Progressive JPEG (two of the three photos on the PC were).
- Images in the disk cache, so that Back doesn't fetch them again.
- The transputer offload, behind the same sink.
