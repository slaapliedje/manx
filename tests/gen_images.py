#!/usr/bin/env python3
"""gen_images.py OUTDIR - test images for test_image, and what each must
decode to.

Writes OUTDIR/NAME.{gif,png,jpg}, OUTDIR/NAME.rgba (the expected pixels,
R G B A for every pixel of the canvas) and prints a manifest on stdout,
one image a line:

    NAME FILE W H SHIFT TOLERANCE EXPECT

TOLERANCE is the largest difference allowed in any channel of a pixel
(0: exact), EXPECT is "ok" or an error the decoder must give
("unsupported"). PNGs come from a small encoder here, which can do what
Pillow can't (2/4/16-bit, Adam7, every filter type); GIFs and JPEGs from
Pillow, whose libjpeg decodes the reference pixels.

Needs Pillow; with none, prints nothing (test_image then has nothing to
test, and says so)."""
import os, struct, sys, zlib

try:
    from PIL import Image
except ImportError:
    sys.exit(0)

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
lines = []


def pattern(w, h, seed=0):
    """an RGBA picture with edges, gradients and some transparency"""
    px = []
    for y in range(h):
        for x in range(w):
            r = (x * 255 // max(1, w - 1) + seed) & 255
            g = (y * 255 // max(1, h - 1)) & 255
            b = ((x ^ y) * 9 + seed * 7) & 255
            a = 255 if (x + y) % 7 else 0
            px.append((r, g, b, a))
    return px


def rgba_bytes(px):
    return bytes(c for p in px for c in p)


def emit(name, ext, data, w, h, ref, shift=0, tol=0, expect="ok"):
    with open(os.path.join(OUT, name + "." + ext), "wb") as f:
        f.write(data)
    with open(os.path.join(OUT, name + ".rgba"), "wb") as f:
        f.write(ref)
    lines.append("%s %s.%s %d %d %d %d %s" % (name, name, ext, w, h, shift, tol, expect))


# --- PNG, by hand -------------------------------------------------------------

def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


def filt(kind, line, prev, bpp):
    out = bytearray()
    for i, x in enumerate(line):
        a = line[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if kind == 0:
            p = 0
        elif kind == 1:
            p = a
        elif kind == 2:
            p = b
        elif kind == 3:
            p = (a + b) // 2
        else:
            pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
            p = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
        out.append((x - p) & 255)
    return bytes([kind]) + bytes(out)


def pack_row(samples, depth):
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    out, acc, n = bytearray(), 0, 0
    for s in samples:
        acc = (acc << depth) | s
        n += depth
        if n == 8:
            out.append(acc)
            acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


A7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def png(w, h, ctype, depth, pixels, plte=None, trns=None, interlace=False):
    """pixels: rows of sample tuples, at the image's depth"""
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, chans * depth // 8)
    raw, k = bytearray(), 0
    passes = A7 if interlace else [(0, 0, 1, 1)]
    for x0, y0, dx, dy in passes:
        pw = (w - x0 + dx - 1) // dx if w > x0 else 0
        ph = (h - y0 + dy - 1) // dy if h > y0 else 0
        if pw == 0 or ph == 0:
            continue
        prev = bytes(len(pack_row([0] * (pw * chans), depth)))
        for j in range(ph):
            y = y0 + j * dy
            samples = []
            for i in range(pw):
                samples.extend(pixels[y][x0 + i * dx])
            line = pack_row(samples, depth)
            raw += filt(k % 5, line, prev, bpp)
            k += 1
            prev = line
    ihdr = struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 1 if interlace else 0)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
    if plte:
        data += chunk(b"PLTE", plte)
    if trns is not None:
        data += chunk(b"tRNS", trns)
    z = zlib.compress(bytes(raw), 9)
    # IDAT in several chunks, as encoders send big images
    for i in range(0, len(z), 97):
        data += chunk(b"IDAT", z[i:i + 97])
    return data + chunk(b"IEND", b"")


def to8(v, depth):
    return {1: 255, 2: 85, 4: 17, 8: 1}[depth] * v if depth <= 8 else v >> 8


def png_case(name, w, h, ctype, depth, interlace=False):
    src = pattern(w, h, depth + ctype)
    mx = (1 << depth) - 1
    rows, ref = [], []
    plte = trns = None
    if ctype == 3:
        n = 1 << depth
        pal = [((i * 53) & 255, (i * 101) & 255, (i * 197) & 255) for i in range(n)]
        alphas = [255 if i % 3 else 128 for i in range(n)]
        plte = bytes(c for p in pal for c in p)
        trns = bytes(alphas)
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = src[y * w + x]
            if ctype == 0:
                v = ((r + g + b) // 3) * mx // 255
                row.append((v,))
                gv = to8(v, depth)
                key = mx // 3 if depth > 1 else 1
                ref.append((gv, gv, gv, 0 if v == key else 255))
            elif ctype == 2:
                s = tuple(c * mx // 255 for c in (r, g, b))
                row.append(s)
                key = (mx // 2, mx // 3, mx // 5)
                ref.append(tuple(to8(c, depth) for c in s) + ((0,) if s == key else (255,)))
            elif ctype == 3:
                i = (r + g * 3 + b * 5) % (1 << depth)
                row.append((i,))
                ref.append(pal[i] + (alphas[i],))
            elif ctype == 4:
                v, al = ((r + g + b) // 3) * mx // 255, a * mx // 255
                row.append((v, al))
                ref.append((to8(v, depth),) * 3 + (to8(al, depth),))
            else:
                s = tuple(c * mx // 255 for c in (r, g, b, a))
                row.append(s)
                ref.append(tuple(to8(c, depth) for c in s))
        rows.append(row)
    if ctype == 0:
        key = mx // 3 if depth > 1 else 1
        trns = struct.pack(">H", key)
    elif ctype == 2:
        trns = struct.pack(">HHH", mx // 2, mx // 3, mx // 5)
    data = png(w, h, ctype, depth, rows, plte, trns, interlace)
    emit(name, "png", data, w, h, rgba_bytes(ref))


for ctype, depths in ((0, (1, 2, 4, 8, 16)), (2, (8, 16)), (3, (1, 2, 4, 8)), (4, (8, 16)), (6, (8, 16))):
    for depth in depths:
        png_case("png_c%d_d%d" % (ctype, depth), 37, 23, ctype, depth)
png_case("png_a7_rgb", 37, 23, 2, 8, interlace=True)
png_case("png_a7_pal4", 13, 11, 3, 4, interlace=True)
png_case("png_a7_tiny", 3, 2, 6, 8, interlace=True)
png_case("png_one", 1, 1, 6, 8)

# --- GIF, by Pillow ---------------------------------------------------------------

import io


def gif_case(name, w, h, colors, interlace=False, transparent=False):
    im = Image.new("RGB", (w, h))
    im.putdata([p[:3] for p in pattern(w, h, colors)])
    q = im.quantize(colors=colors)
    opts = {"interlace": interlace}
    if transparent:
        opts["transparency"] = 0
    b = io.BytesIO()
    q.save(b, "GIF", **opts)
    back = Image.open(io.BytesIO(b.getvalue())).convert("RGBA")
    emit(name, "gif", b.getvalue(), w, h, back.tobytes())


gif_case("gif_2", 37, 23, 2)
gif_case("gif_16", 37, 23, 16)
gif_case("gif_256", 64, 48, 256)
gif_case("gif_interlaced", 64, 48, 64, interlace=True)
gif_case("gif_transparent", 37, 23, 16, transparent=True)

# --- JPEG, by Pillow --------------------------------------------------------------


def smooth(w, h):
    """a photo-like picture: colour changing gently"""
    return [((x * 255 // max(1, w - 1)), (y * 255 // max(1, h - 1)),
             ((x + y) * 255 // max(1, w + h - 2))) for y in range(h) for x in range(w)]


def jpeg_case(name, w, h, mode="RGB", subsampling=0, shift=0, tol=1, expect="ok",
              gentle=False, **opts):
    im = Image.new("RGB", (w, h))
    im.putdata(smooth(w, h) if gentle else [p[:3] for p in pattern(w, h, 5)])
    if mode == "L":
        im = im.convert("L")
    b = io.BytesIO()
    im.save(b, "JPEG", quality=85, subsampling=subsampling, **opts)
    back = Image.open(io.BytesIO(b.getvalue()))
    if shift:
        back.draft(back.mode, ((w + (1 << shift) - 1) >> shift, (h + (1 << shift) - 1) >> shift))
    back = back.convert("RGBA")
    emit(name, "jpg", b.getvalue(), back.size[0], back.size[1], back.tobytes(), shift, tol, expect)


jpeg_case("jpeg_gray", 37, 23, mode="L")
jpeg_case("jpeg_444", 37, 23)
jpeg_case("jpeg_444_restart", 64, 48, restart_marker_blocks=3)
# chroma upsampled by replication here, by a triangle filter in libjpeg:
# alike on a photograph's gentle colour (on colour changing every pixel,
# as the other cases have, they differ by up to ~90 at the edges)
jpeg_case("jpeg_422", 37, 23, subsampling=1, tol=12, gentle=True)
jpeg_case("jpeg_420", 37, 23, subsampling=2, tol=12, gentle=True)
jpeg_case("jpeg_420_restart", 64, 48, subsampling=2, tol=12, gentle=True,
          restart_marker_rows=1)
# scaled: the low frequencies only here, libjpeg's reduced IDCTs fold in
# more of the rest; alike on photographs (on colour changing every pixel
# they differ by up to ~40)
jpeg_case("jpeg_half", 64, 48, shift=1, tol=12, gentle=True)
jpeg_case("jpeg_quarter", 64, 48, shift=2, tol=12, gentle=True)
jpeg_case("jpeg_eighth", 64, 48, shift=3, tol=2)
# progressive: scan by scan, the same coefficients in the end as a
# sequential file's, so as exact
jpeg_case("jpeg_prog_444", 37, 23, progressive=True)
jpeg_case("jpeg_prog_gray", 37, 23, mode="L", progressive=True)
jpeg_case("jpeg_prog_restart", 64, 48, progressive=True, restart_marker_blocks=3)
jpeg_case("jpeg_prog_420", 37, 23, subsampling=2, tol=12, gentle=True, progressive=True)
jpeg_case("jpeg_prog_half", 64, 48, shift=1, tol=12, gentle=True, progressive=True)
jpeg_case("jpeg_prog_quarter", 64, 48, shift=2, tol=12, gentle=True, progressive=True)
jpeg_case("jpeg_prog_eighth", 64, 48, shift=3, tol=2, progressive=True)
# too big to keep every coefficient in the default budget (512 KB): kept
# from a smaller corner and enlarged, so softer, but whole
jpeg_case("jpeg_prog_big", 640, 480, subsampling=0, tol=24, gentle=True, progressive=True)

print("\n".join(lines))
