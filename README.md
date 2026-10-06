# Manx

A web browser for 68030 System V Unix: **Atari System V** on the TT030 and
**Amiga UNIX** (AMIX). In a terminal (`manx`) or an X11 window with
pictures (`xmanx`). It does HTTPS itself, and decodes its images, on the
68030, in a few megabytes.

Named after Lynx's tailless cousin, and after Manx Software Systems, whose
Aztec C built a good share of the Amiga's and the Atari ST's software.

![xmanx on the Atari TT030's own 24-bit display, showing Wikipedia's article about the TT030](docs/screenshots/11-xmanx-on-the-tt030.png)

![Hacker News in Manx on the TT030](docs/screenshots/03-hackernews.png)

![A Wikipedia article](docs/screenshots/05-wikipedia-article.png)

![Manx in colour in XFree86's xterm on the TT's own display](docs/screenshots/10-manx-in-colour-xterm.png)

(Captured from the real TT030: its X display, on an ATW800/2 card at
1024x768 in 24-bit colour, and over telnet. More in
[docs/screenshots](docs/screenshots).)

## What it does

- HTTP/1.1 and HTTPS: TLS 1.2 through [BearSSL](https://bearssl.org),
  ChaCha20-Poly1305 and X25519 first. Keep-alive and session resumption.
  Certificate checks and X25519 in 68030 assembly
  ([crypt68k](https://github.com/slaapliedje/crypt68k)), shared with an
  ATW800/2 card's T425 transputer when there is one.
  Its own DNS resolver, because a static SVR4 program can't use the system's.
- gzip/deflate content, decoded as it arrives. Pages come 4-6x smaller,
  which matters at the TT's 35 KB/s.
- HTML parsed as it streams, into a compact document (16-byte nodes, a
  1.2 MB cap). Lists, `<pre>`, tables of data as grids (those framing a
  page row by row), form fields, anchors. UTF-8, windows-1252 and Latin-1
  pages.
- What style sheets hide (`display:none`, `visibility:hidden`,
  `list-style:none`), and how they want text to look: colours (names,
  `#hex`, `rgb()`, `hsl()`, `var()` from `:root`), bold, italics,
  underline, centred and right-aligned text. From `<style>`, linked
  sheets and `style=""`, with `@media` widths and the cascade; never page
  layout. `xmanx` shows the colours (darkened if too pale to read on
  white); a terminal shows the clear ones in its own eight and keeps its
  own colour for greys, so text reads on any background.
- Forms (GET and POST, every field type, `$EDITOR` for text areas), a
  cookie jar, a disk cache (Back without the network), bookmarks, gopher,
  `file:`.
- Any terminal: its own screen layer on terminfo, not SVR4 curses, which
  can't do UTF-8. UTF-8, Latin-1 or plain ASCII (it asks the terminal which);
  colour where the terminal has it.
- Lynx's keys: Up/Down between links, Right follows, Left goes back.
- `xmanx`: the page in proportional fonts in an X11 window with OPEN LOOK
  controls. GIF, PNG and JPEG (baseline and progressive) decoded on the
  68030 as they arrive, scaled and dithered to the display (24-bit,
  8-bit colour cube, black and white). On the ATW800 card, scrolling moves
  the window's pixels with the card's 2D engine.

What it doesn't do: JavaScript, CSS layout (columns, positions,
backgrounds, font sizes), SVG and WebP pictures.

## On the TT030 (68030, 32 MHz)

| | |
|---|---|
| Hacker News fetched over a new TLS session (handshake and certificate check included) | 10 s |
| Wikipedia article fetched (131 KB, 26 KB gzipped), new TLS session | 13 s |
| Hacker News, resumed session: first screen / whole page | 6.5 s / 13.6 s |
| Back to a page in the cache | 1.2 s |
| The browser binary (static, with TLS): manx / xmanx | 400 KB / 530 KB |

Checking a certificate chain takes the 68030 5-10 s, and many servers
give up on an idle client well before that. So a request with nothing
private in it (no cookies, no form data) is sent right after the handshake,
and the chain is checked before any of the answer is used. A request with
cookies or form data always waits for the check. `early_requests = off` in
the settings makes every request wait.

## Building

Everything also builds and runs on Linux, for development and tests:

```sh
make                 # build/host/manx, xmanx, manxtrust, ufetch, uparse, benchmarks
make test            # unit tests, TLS known-answer tests
make fuzz            # the HTML engine and layout under ASan/UBSan
```

For the 68030: one static AMIX program, which also runs on Atari System V
through the `amx` module of [atari-sysv-sp1](https://github.com/slaapliedje/atari-sysv-sp1):

```sh
make TARGET=sysv4    # build/sysv4/manx, xmanx, manxtrust
```

`X11=0` leaves `xmanx` out (it needs Xlib: AMIX's static X11R5 `libX11.a`
for the 68030, `libX11` on Linux).

This needs:
- a modern m68k GCC (the mint cross compiler, `m68k-atari-mint-gcc` 15);
- the SVR4 assembler and linker of gcc-cross-amix (`~/opt/asv-cross`);
- an AMIX sysroot, which sp1's `amix/mksysroot.sh` makes from Commodore's
  AMIX 2.1 packages (not included here).

`toolchain/sysv4-cc` explains how the three fit together.

`xmanx` also builds with Helios 1.31's C compiler and runs on the
ATW800/2's T425 itself, under Helios's X server. That build speaks HTTP
and Gopher only: Helios C has no 64-bit integers, which BearSSL needs.
`tools/helios/bundle.sh DIR` lays the sources out in 8.3 names with a
`build.csh`; copy DIR to `/helios/local/src/manx` and `source build.csh`
there (about 14 minutes). Helios reaches the network through its own
TCP/IP; on the TT, through a DaynaPORT.

## Running

Copy `manx` and `manxtrust` to the machine. Then, once:

```sh
manxtrust roots cacert.pem   # trusted roots, from any PEM bundle (e.g. curl's cacert.pem)
manxtrust seed               # SVR4 has no /dev/random: type for a while to seed
manx                         # or: manx https://news.ycombinator.com/
```

`xmanx` is the same browser in an X11 window, with controls in the OPEN
LOOK manner of both systems' desktops (Back, Forward, Reload and Stop
buttons, a URL field, a scrollbar with an elevator) and the mouse: a click
follows a link, the wheel scrolls, the keys are the same. The page is set
in the server's Helvetica and Courier (the 75 dpi fonts of X11R5 and
R6.3), headings bigger, italics italic. On Atari System V
it opens `$DISPLAY` over TCP (`:0` becomes `thishost:0`), because AMIX's
X11R5 library and ASV's X11R6.3 server have no local transport in common.

Settings go in `~/.manx/config`, one `key = value` a line, or as
`MANX_KEY` in the environment:

| key | |
|---|---|
| `start` | the start page |
| `search` | where words typed at `g` go (DuckDuckGo Lite) |
| `charset` | the terminal's: `utf-8`, `latin1`, `ascii` (otherwise the terminal is asked) |
| `color`, `link_color` | `off` for none; the links' colour (cyan on a terminal, blue in a window) |
| `font` | `xmanx`'s font for its title and status lines, a fixed-width X font (`fixed`) |
| `proportional` | `off`: `xmanx` shows the page in that font too, as on a terminal |
| `cookies` | `off` for none |
| `cache_kb` | the disk cache's size (2048; 0 for none) |
| `cafile` | a PEM bundle to build the roots from |
| `early_requests` | `off`: send nothing before the certificate is checked |

`?` in the browser lists the keys.

## Status

The phases of [PLAN.md](PLAN.md) are done: toolchain, network core, HTML
engine, text browser, forms, cookies, cache and gzip, then `xmanx` (the
same browser in proportional fonts, with OPEN LOOK controls and the
mouse), its pictures, and what style sheets hide. Each has a write-up in
[docs](docs), with measurements from the TT. Since then the certificate
checks went into 68030 assembly (RSA 4x, ECDSA 3-8x faster) and onto the
ATW800/2's T425 (up to 2x more on a first visit).

Tested on an Atari TT030 with Atari System V, and on AMIX 2.1 in an
emulated Amiga 3000 (`tools/amix/`), where it loads Hacker News over HTTPS.
It also runs on [Ash Nazag](https://github.com/kdedon/ashnazag)'s AMIX for
the Quadra 800, in QEMU. It hasn't been tried on a real Amiga yet.

## Licence

GPL-2.0 (see [LICENSE](LICENSE)). BearSSL, in `third_party/bearssl`, is MIT
licensed; `third_party/patches` has the one change made to it. crypt68k,
in `third_party/crypt68k`, is MIT licensed.
