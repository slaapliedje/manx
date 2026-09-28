# Phase 3 results: the text browser

`ub` is the first usable browser: it loads a page, lays it out for the
terminal, and lets you read it and follow links, over HTTP, HTTPS, gopher
and file:. Static binary for the TT (and AMIX): 329 KB.

## What was built

| Module | What it does |
|---|---|
| `style/` | the built-in style table (display, margins, indent, bold/underline, pre) per tag; `hidden` and `style="display:none"` hide; React's streamed `<div hidden id="S:n">` is shown |
| `layout/` | one walk over the tree into lines of terminal bytes: word wrap (a word that no longer fits moves down by moving the line boundary, not the bytes), margins that collapse, lists (bullets by depth, `ol start/type`, roman), `<pre>` with tabs, blockquote/dd indent, `hr`, `br`, tables row by row (a row stays one line; blocks inside a cell break only between contents), form fields shown as `[____]` `[Go]` `[x]` `[B v]`, image alt text, a label for links with no text, anchors (`id`, `a name`) and `<main>` |
| `html/gophermap` | gopher menus to HTML as they stream, `(DIR)`/`(TXT)`/`(?)` tags |
| `frontend/screen` | the terminal through terminfo (ANSI when unknown): raw keys and escape sequences, a cell buffer, flushes that send only the changed part of each row. Not SVR4 curses: it counts bytes as columns, which breaks UTF-8 |
| `src/ub.c` | the browser: progressive display while loading (only the screenful is laid out), `z`/Esc stops, Lynx keys (arrows move between links and follow/back), Tab, find, history with positions, `g` takes a URL or search words (DuckDuckGo Lite), gopher search prompts, `m` jumps past site navigation to `<main>`, `=` page info, error pages |

Character sets: the terminal's is taken from `UB_CHARSET`, the locale, or
by asking the terminal (print é, ask where the cursor went: 1 column is
UTF-8, 2 is an 8-bit set, no answer is ASCII). Text is transliterated at
layout time (U+2014 becomes `--` on ASCII) and wide characters take two
cells on UTF-8 terminals.

## Tests

- `tests/test_layout.c`: 23 snippets laid out and compared with the text
  expected (wrapping, margins, lists, pre, tables, fields, charsets,
  anchors).
- All 162 corpus pages lay out at 80, 40 and 20 columns; the output was
  byte-identical across the refactors below.
- `make fuzz` now lays out every mutated page too, at random widths,
  charsets, caps and line limits: 30,000 iterations clean under ASan+UBSan.
- The UI was driven in a pseudo-terminal with a terminal emulator (pyte),
  locally and over telnet on the TT: start page, link selection, follow,
  back (position restored), find/next, help, Wikipedia, Hacker News,
  gopher.

## On the TT

| | time |
|---|---|
| Hacker News, resumed TLS session: first screen / whole page (33 KB) | 6.5 s / 13.6 s |
| Wikipedia article (127 KB), new TLS session | 104 s (mostly the handshake and certificate check) |
| layout, Hacker News (81 lines, 230 links) | 0.17 s |
| layout, Spiegel (1.9 MB page, 1728 lines) | 1.9 s |

## The compiler was making every call slow

Layout first ran 3000-4000x slower on the TT than on the development
machine (the 68030 is normally 100-200x slower). The disassembly showed
every 32-bit argument pushed and read back as two 16-bit halves. The mint
GCC defaults to `-mstrict-align`, and together with `-malign-int` (needed
for SVR4 structure layout) it can't assume a 4-byte-aligned stack.
`toolchain/sysv4-cc` now passes `-mno-strict-align`: both targets are
68030s, which handle misaligned accesses in hardware. The binaries got
20% smaller, and on the TT:

| | before | after |
|---|---|---|
| parse Hacker News | 1.57 s | 0.91 s |
| parse Spiegel (1.9 MB) | 26.7 s | 17.6 s |
| layout Hacker News | 848 ms | 174 ms (with the changes below) |

BearSSL barely changed (ChaCha20 58-67 KB/s): its hot loops make few
calls.

Layout also changed for the 68030:
- words are placed as runs, not characters
- short copies are done inline, because AMIX `memcpy` costs ~180 cycles a call
- one pass over an element's attributes replaces a `doc_attr` call per
  attribute, each of which also cost a libc `strlen`
- each element's style is saved when entering it, not looked up again
  when leaving

`-O2`, `-O2 -fno-inline` and `-Os` came out within the TT's noise, so the
default stays `-O2`.

Profiling on the target works now: `SYSV4_PROF=1` links through
`mcrt1.o`, and the TT's `prof -t` reads the `mon.out` it writes. `prof`
only sees global symbols, so compile the module being studied with
`-Dstatic= -fno-inline`.

## Left for later

- Tables laid out as grids (columns aligned); now rows run linearly.
- The parser is the slowest stage on big pages (17.6 s for 1.9 MB).
- Two empty links in a row get no space between them. The parser drops
  the space because it saw nothing worth separating.
- Forms (Phase 4): fields are shown and selectable, not yet editable.
