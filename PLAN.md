# Manx — plan

A web browser for 68030 System V Unix: **Atari System V (ASV, UniSoft SVR4
UE12) on the TT030** with the ATW800/2 card, and **AMIX (Amiga UNIX, SVR4)**.
Text (curses) frontend first, then X11. HTTPS natively on the 68030.

## 1. Targets and the facts that shape the design

| Fact | Consequence |
|---|---|
| 68030 @ 32 MHz (TT) / 25 MHz (A3000), big-endian | Integer-only hot paths; endian bugs caught by testing on the target from day one. |
| Budget: **~4 MB process footprint** (the TT has 100 MB, but the target is the small machine) | Streaming parse into a per-page memory pool, hard caps, no JavaScript, tiny CSS subset. |
| **One static AMIX binary runs on both** (ASV runs AMIX programs through sp1's `amx` module) | Build once with the OpenUA route: modern m68k GCC → SVR4 `as`/`ld` from gcc-cross-amix, linked statically against AMIX's `libc.a`/`libsocket.a`/`libnsl.a`/`libX11.a`. |
| A static AMIX program **cannot use the system resolver** (shared-object only) | Ship our own small DNS stub resolver (UDP, reads `/etc/resolv.conf`, falls back to `/etc/hosts`). |
| SVR4.0 libc: no `snprintf`, no `stdint.h`, no `getaddrinfo`; `libsocket.a` needs a static netconfig shim | `os/sysv4/` carries these (adapted from OpenUA `platform/unix`). |
| Signals under `amx` are expensive and are only delivered at syscall boundaries; interrupted calls may leak `ERESTART` | No `SIGALRM`, no signal-driven I/O. Single-threaded event loop on `poll`/`select` with timeouts; retry on `EINTR`/91. |
| gcc 2.7.2 miscompiles `long long` comparisons at `-O` | Not used for our code: GCC 15 compiles, the 1997 binutils only assemble and link. |
| AMIX `libm` is soft-float style; don't call it from `-m68881` code | `-msoft-float`, and no floating point on hot paths. |
| Network: ~35 KB/s on the TT (DaynaPORT over the ZuluSCSI WiFi) | Stream everything; progressive display; gzip support is worth it later. |
| Clock is right (`ttntp` + cron) | Certificate date checks can be enforced. |
| X11R6.3 `Xatw` on the ATW800/2 (8-bit by default), X11R5 libs in AMIX | Raw Xlib only (no toolkit); X protocol makes R5 static libs fine against the R6.3 server. |
| Transputers: `/dev/link0` (TRAM slot 1, T800) and `/dev/link1` (FPGA T425) via sp1 `driver-tlk`, ~100 KB/s | Offload is optional, behind a job API with a 68030 fallback. |

## 2. Architecture

```
 frontend/   terminal (terminfo) |  x11 (Xlib)         <- input, drawing
 layout/     block/inline flow -> line boxes -> display list (cells or pixels)
 style/      built-in per-tag style table + tiny CSS subset (display:none, ...)
 html/       streaming tokenizer -> tolerant tree builder -> node pool
 text/       UTF-8 decode, entities, charset -> terminal/font transliteration
 net/        URL, DNS stub, HTTP/1.1, redirects, cookies, gopher, file:, cache
 tls/        BearSSL glue, PEM trust store, entropy pool, session cache
 offload/    job API {bignum, inflate, image decode} -> transputer | CPU
 os/         host | sysv4: sockets, poll, time, random, memory pools, shims
```

- **Memory:** per-page pool freed on navigation, plus a global cap. Oversized
  documents are truncated with a notice.
- **Parsing:** one tolerant tokenizer fed network chunks; the raw HTML is never
  held whole. `<script>`/`<style>` bodies are skipped as they stream.
- **Document store:** 16-bit node indexes and interned strings.
- **Layout:** written once against a font-metrics interface (1x1 cells for
  curses, pixels for X11). Simplified tables with a linear fallback.
- **TLS:** BearSSL 0.6 (no heap use, constant-time, portable C) with one
  local patch (deferred ServerKeyExchange check). TLS 1.2, ECDHE-X25519
  first, ChaCha20-Poly1305 first, m31/i31 code (measured fastest on the TT).
  The certificate chain is validated *after* the handshake and before
  anything is sent, since servers don't wait the 30-100 s a 68030 needs.
- **Trust anchors:** a standard **PEM bundle** (`ca-certificates.crt` format),
  path configurable; decoded once and cached in a compact binary form on disk
  (decoding the PEM takes 52 s on the TT). Intermediates the TT has verified
  are cached as extra anchors.
- **Entropy:** no `/dev/random` on SVR4. Mix high-resolution timing jitter,
  `times()`, keystroke timing and network arrival times into a SHA-256 pool;
  save a seed file (mode 600) between runs; refuse TLS until seeded.

## 3. Memory budget (4 MB)

| Area | Budget |
|---|---|
| Code incl. BearSSL subset (static binary) | ~700 KB |
| libc/libsocket/Xlib (static) | ~400 KB |
| TLS state + buffers (1-2 connections) | 40-80 KB |
| Network input / cache staging | 64 KB |
| Document pool | <= 1.2 MB, capped |
| Layout, display list, screen | <= 600 KB |
| Images (X11 only) | <= 800 KB, LRU |
| Headroom | rest |

Back/forward re-parses from the disk cache rather than keeping documents.

## 4. Build and test setup

- `make` (host, Linux): the same code for fast unit tests and fuzzing, with a
  memory-cap allocator. `make TARGET=sysv4`: the static AMIX binary via
  `toolchain/sysv4-cc` + `sysv4-ld` (from OpenUA; needs `AMIX_SYSROOT`).
- `tools/tt/`: push a binary to the TT (ftp as `dev`) and run it (telnet),
  using `ASV_HOST`; credentials stay outside the repo.
- Emulators when the real machine is busy: the Hatari fork (ASV) and
  Amiberry or WinUAE (AMIX).
- `tools/amix/`: the same for AMIX 2.1 in an emulated A3000 (sp1's
  `amix/emu/uaeamix.py`, on Amiberry by default): `amixsh`, `amixput`,
  `amixrun`, and `amixdrive` for the full-screen UI.

## 5. Phases

**Phase 0 — toolchain and risk spikes** (done 2026-09-27 except the
transputer spike; see `docs/phase0-results.md`)
- Build skeleton for host + sysv4; static binaries run on the TT. ✔
- `tlsbench` on the TT: m31/i31 fastest; X25519 1 s, RSA-2048 verify 2.4 s,
  ECDSA P-256 verify 5.8 s, P-384 46 s; ChaCha20-Poly1305 67 KB/s,
  AES-GCM 17 KB/s. ✔
- `ufetch`: own DNS + TCP + TLS 1.2 fetched google.com and example.com from
  the TT, using **deferred validation**. ✔
- Transputer spike: BearSSL i15 runs correctly on the T800 but 1.5-3.7x
  slower than the 68030 (icc has no optimiser, C has no `lmul`). ✔

**Phase 1 — network core** (done 2026-09-27; see `docs/phase1-results.md`)
- URL, HTTP/1.1, DNS, keep-alive, redirects, gopher, file:. ✔
- TLS: deferred validation, early requests for requests with nothing
  private, the fast/full-x25519/full offer ladder, a binary trust store
  (0.3 s load), learned intermediates, known leaves, session resumption. ✔
- Entropy pool with an estimate; `manxtrust seed`. ✔
- Every site in the campaign loads on the TT, cold and repeat; ~650 KB
  footprint. ✔
- Left for later: a preload bundle of common intermediates (`manxtrust
  learn`); 68030 assembly for RSA/ECDSA verification; testing on a real
  AMIX machine.

**Phase 2 — HTML engine** (done 2026-09-28; see `docs/phase2-results.md`)
- Streaming tokenizer, 285-entity table, tolerant tree builder, compact
  document store, charset detection and UTF-8 decoding. ✔
- 54 real pages agree with Python's parser (title, links) and are
  chunking-invariant. 100k+ sanitizer fuzz iterations are clean. ✔
- On the TT, parsing keeps up with the network: Wikipedia 1.9 s,
  Hacker News 1.0 s, 1.1 MB pages 15-20 s. ✔
- Checked (A/B of the cache register): the TT's caches are on and
  effective; the 68030 is simply this fast. AMIX libc memset/strcmp are
  slow and worth avoiding in hot paths.

**Phase 3 — text frontend (first usable browser)** (done 2026-09-28; see
`docs/phase3-results.md`)
- Style table, block/inline layout, lists, `<pre>`, tables row by row,
  form fields shown, anchors. ✔
- Own terminfo screen layer instead of SVR4 curses (which can't do UTF-8);
  scrolling, Lynx-style link selection, history, URL/search prompt, find,
  status line, progressive display, stop. ✔
- On the TT: Hacker News first screen in 6.5 s with a resumed session. ✔
- Found and fixed: the compiler split every 32-bit stack access
  (`-mno-strict-align`); parsing got 40% faster.
- Left for later: grid tables; the parser on very large pages.

**Phase 4 — forms, cookies, polish** (done 2026-09-28; see
`docs/phase4-results.md`)
- gzip/deflate: a streaming inflate; pages arrive 4-6x smaller (Wikipedia
  131 KB as 26 KB). ✔
- GET/POST forms, every field type, $EDITOR for textareas. ✔
- Cookie jar (RFC 6265), never sent on an early request. ✔
- Disk cache: Back without the network, conditional requests, POST
  answers kept. ✔
- Config file, bookmarks. ✔

**Phase 5 — X11 frontend:** Xlib window, proportional fonts through the metrics
interface, mouse, same features as text. Under way (2026-09-30): `xmanx`
has its window, OPEN LOOK controls (both targets' desktops are OPEN LOOK;
drawn on Xlib, as OLIT is AMIX's only and XView too big), the mouse, the
URL typed in its field, and pages in the server's Helvetica and Courier
through `struct lmetrics` (the terminal's layout is byte-identical to
before on the corpus). Runs on ASV's Xatw (X11R6.3, 8-bit) and AMIX's
Xdmi (X11R5, mono, in the emulator). Done: `xmanx` shares the text
browser's command handling, so forms, find, bookmarks and the rest work in
both. ✔

**Phase 6 — images and transputer offload:** GIF, PNG, baseline JPEG,
dithering to 8-bit; offload image decode / inflate / bignum to the T800s when
present. Done (see `docs/phase6-results.md`): GIF, PNG, baseline and
progressive JPEG, to 24-bit, 8-bit and black and white, through the disk
cache. ✔ The offload went to certificate checks instead: the T800s are
slower than the 68030 at bignum work, the ATW800/2's T425 is not (it
shares the checks, see `docs/phase0-results.md`). Images stay on the
68030.

**Phase 7 — pages that look right:** what style sheets hide
(`display:none`, `visibility:hidden`) from `<style>`, then from linked
sheets; tables laid out as grids. Done; see `docs/phase7-results.md`. ✔

## 6. Risks

| Risk | Mitigation |
|---|---|
| TLS handshake too slow (measured: servers wait ~10-20 s; full validation takes 30-100 s) | Deferred validation, intermediate cache, RSA-first, session resumption, asm bignum, transputer offload; last resort an optional stripping proxy. |
| Sites dropping TLS 1.2 (BearSSL has no 1.3) | Watch; most sites still offer 1.2. |
| Weak entropy | Several sources + seed file; refuse TLS until seeded. |
| Static-link gaps in AMIX libs (resolver, netconfig, etc.) | Own resolver; shims in `os/sysv4`. |
| Huge / JavaScript-only pages | Caps, `<noscript>`, reader-style fallback. |
| amx signal and `ERESTART` quirks | No signals; loop on `EINTR`/`ERESTART`. |

## 7. Open questions
- AMIX machine for testing: an emulator for now (`tools/amix/`); a real A3000UX
  if one turns up.
- Which sites matter most (for the Phase 2 corpus)?
