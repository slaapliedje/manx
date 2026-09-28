# Phase 2 results: the HTML engine (2026-09-28)

## What exists

| Module | What it does |
|---|---|
| `text/utf8` | WHATWG UTF-8 decoder (byte at a time, so chunking can't change it), windows-1252, charset labels, transliteration to Latin-1/ASCII for display |
| `html/tokenizer` | streaming HTML tokenizer: tags, attributes (only the ~50 the browser uses are kept), comments, doctype, character references (285 named + numeric, legacy no-semicolon rules), RCDATA/RAWTEXT. `<script>`/`<style>`/`<iframe>` bodies are skipped as they stream, never stored |
| `html/tree` | tolerant tree building: implied html/head/body, implied end tags (p, li, dd/dt, option, cells, rows, headings, nested links), scoped end tags, void elements, dropped subtrees (svg, math, template), depth cap 160, whitespace collapsing outside pre, `<base href>` and `<meta>` charset capture |
| `html/doc` | the document: 16-byte nodes with 16-bit indexes, text and attribute pools, a byte cap (default 1.2 MB) that truncates instead of failing |
| `html/load` | bytes in, document out: charset from HTTP, BOM, `<meta>` prescan, or a UTF-8 validity guess; decode, tokenize, build as bytes arrive |
| `src/uparse` | parse a file or URL (streamed through the network core): `-s` stats, `-d` tree dump, `-t` text, `-l` links, `-c N` chunking |

Deliberately left out: the HTML5 adoption agency algorithm (misnested
formatting elements are closed by the simpler "innermost of that name"
rule), foster parenting of stray table text, and template contents.

## How it is checked

- `tests/test_html.c` (in `make test`): the tag/attribute tables, charset
  handling (UTF-8 split at every position, random bytes split everywhere,
  invalid sequences, windows-1252, sniffing, transliteration), and 28
  tokenizer/tree cases against expected trees. Every case is parsed whole
  and in chunks of 1, 2, 3, 7 and 64 bytes.
- **Corpus:** `tests/fetch_corpus.sh` downloads 55 real pages
  (`tests/corpus_urls.txt`: Wikipedia in 3 languages, news sites, forums,
  RFCs, GitHub, old-web pages; 559 B to 1.9 MB). They stay out of git.
  `tests/corpus_check.py` compares every page's title and link count with
  Python's `html.parser`, and checks that the tree is identical fed whole,
  1 byte or 7 bytes at a time. **All 54 fetched pages pass.** The only
  truncated one is RFC 9110 (1.2 MB of HTML, at the 1.2 MB cap).
- **Fuzzing:** `make fuzz` builds `tests/fuzz_html.c` with AddressSanitizer
  and UBSan. It mutates corpus pages (byte flips, markup fragments, cuts,
  splices) and parses each whole and in random chunks: **100,000+
  iterations, no sanitizer reports, no differences.**

Two bugs were found this way:
- The first UTF-8 decoder handled an invalid sequence differently depending
  on where the input was split. It was replaced by the WHATWG algorithm.
- The whitespace collapsing lost the space between inline elements. It was
  replaced by a single line-start flag.

## Speed and size on the TT030

| Page | Size | Parse | Document |
|---|---|---|---|
| Hacker News | 35 KB | 1.0 s | 56 KB, 1298 nodes |
| Wikipedia article | 131 KB | 1.9-2.0 s | 112 KB, 1935 nodes |
| rfc3986.html | 171 KB | 1.8 s | 336 KB |
| lite.cnn.com | 333 KB | 1.5 s | 32 KB (mostly script: skipped) |
| Frankenstein (Gutenberg) | 434 KB | 3.3 s | 548 KB |
| github.com | 576 KB | 4.1 s | 296 KB |
| The Guardian | 1.36 MB | 15 s | 416 KB |
| The Register | 1.13 MB | 20 s | 704 KB |

Streamed from the network (`uparse https://...`), Wikipedia's 131 KB
parsed in 3.5 s inside a 23 s fetch: parsing keeps up with the ~20-35 KB/s
link on every page type.

The first working version parsed at 10-20 KB/s. What made it faster:
- fast paths over runs of ordinary bytes: text, skipped script, attribute
  values, tag and attribute names, comments, white space, ASCII in the
  decoder;
- text written straight into the document pool;
- first-letter tag/attribute tables compared inline;
- counts of open elements, so scope searches usually cost nothing;
- no full memset of the tag record.

Frankenstein went from 16 s to 3.3 s, Wikipedia from 8.4 s to 1.9 s.

The binary grew to 289 KB of code (`uparse`, with the network core).

## A finding about the TT itself

Micro-benchmarks (`tests/bench_micro.c`, `tests/bench_loops.c`) show
register-only code running at about the expected 68030 speed. **Every data
load, however, costs ~35-50 cycles, even from a 256-byte array that should
sit in the 68030's data cache**: an empty call takes ~4 us, and memset
manages ~2.6 MB/s. So user data on this ASV seems to be uncached: the data
cache is off, the pages are cache-inhibited, or they live in ST-RAM. That
would explain why the crypto and the parser run 4-5x slower than a 32 MHz
68030 should. If ASV keeps the data cache off on purpose (DMA coherency
is a common reason), turning it on is a kernel question for sp1, not the
browser's. But everything here would gain from it.

## Next

Phase 3: styles, layout and the curses frontend.
