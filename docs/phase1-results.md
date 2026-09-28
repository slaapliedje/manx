# Phase 1 results: the network core (2026-09-27)

Everything below was measured on the real TT030 (Atari System V) with the
static AMIX binaries `ufetch` and `ubtrust`, over the TT's WiFi-bridged
DaynaPORT link.

## What exists

| Module | What it does |
|---|---|
| `net/url` | RFC 3986 parsing and relative resolution (all section 5.4 examples), browser-style clean-up of sloppy hrefs |
| `net/http` | incremental HTTP/1.1 response parser: Content-Length, chunked (extensions, trailers), until-close, HEAD/1xx/204/304, header limits, conflicting-length rejection |
| `net/dns`, `net/tcp` | own resolver (the system one is shared-object only), TCP with read timeouts |
| `net/conn` | TCP or TLS behind one interface; keep-alive pool; the TLS offer ladder |
| `net/fetch` | http(s) with redirects, retries (stale pooled connection; transfer broken by a long validation), gopher (menus, text, search), `file:` |
| `tls/tls` | BearSSL client: offer ladder, deferred validation, early requests, learned intermediates, known leaves, session resumption |
| `tls/anchors` | trust anchors with expiry; PEM loader; compact binary store |
| `tls/entropy` | estimated pool; TLS refused until 128 bits; seed-file ratchet |
| `os/` | data directory (`$UB_HOME` or `~/.ub`), whole-file helpers, capped allocator |
| `src/ufetch` | fetch and report (`-v -I -o -k -m -E -n`) |
| `src/ubtrust` | `status`, `roots FILE.pem`, `learn FILE.pem`, `seed` / `seed -` |

Tests: `make test` (URL, HTTP, snprintf, crypto known answers);
`tests/test_net.sh` against `tests/netserver.py` (14 framing, redirect,
keep-alive and gopher cases, passing on the host and on the TT);
`tests/test_tls_live.sh` (badssl.com refusals with and without early
requests, in-process session resumption); `tests/sites.sh` (the campaign).

## How TLS copes with a 68030

The Phase 0 numbers set the design: a full certificate chain takes the TT
30-140 s to check, and servers drop a silent client after roughly 10-20 s.

1. **Trust store:** `ubtrust roots` decodes the PEM bundle once (55 s on
   the TT). After that, startup loads `roots.bin` in **0.3 s**.
2. **The offer ladder:**
   - **fast:** ECDHE-RSA, X25519 only.
   - **full-x25519:** ECDSA certificates too, still X25519 only.
   - **full:** all curves.

   A host that refuses a rung is remembered. letsencrypt.org needs
   full-x25519: it prefers P-256, and a P-256 key exchange (~5 s on the TT)
   is longer than it waits mid-handshake. That was the Phase 0 mystery.
3. **Deferred validation:** the handshake finishes first (~3 s), then the
   chain and the server's signature are checked.
4. **Early requests:** a request with nothing private in it (no cookies,
   credentials or form data) goes out right after the handshake. The first
   read validates before any byte is accepted. A man in the middle could see
   the request line, never the page. `ufetch -E` turns it off, and badssl.com
   refusals are tested both ways. This is what makes first visits work at all.
5. **Retry after a long validation:** if the server gave up meanwhile
   (Wikipedia: a response nobody read for 140 s), the fetch retries once.
   By then that is cheap, and the receiver is told to start over.
6. **Learning:** intermediates the TT has verified become anchors. Leaves
   validated for a host are remembered until they expire. Sessions are
   resumed (no public-key work at all). The result: a repeat visit costs a
   ~0.2 s handshake, or one signature check.

## The campaign (cold data directory, then a repeat visit)

| Site | Cold visit | Repeat | How the repeat connected |
|---|---|---|---|
| http://example.com | 0.2 s | 0.3 s | plain HTTP |
| example.com | 30 s | 0.7 s | resumed |
| www.google.com (85 KB) | 25 s | 13.6 s | known leaf (no resumption) |
| en.wikipedia.org article (131 KB) | 160 s (140 s validating; retried on the resumed session) | 8.9 s | resumed |
| news.ycombinator.com | 104 s | 5.5 s | resumed |
| lite.cnn.com (333 KB) | 147 s | 14 s | resumed |
| text.npr.org | 35 s | 6.9 s | known leaf |
| github.com (576 KB) | 62 s | 42 s | known leaf (RSA-4096: ~10 s) |
| letsencrypt.org (90 KB) | 59 s | 13 s | full-x25519, known leaf |
| www.gnu.org | 20 s | 5.8 s | resumed |
| gopher.floodgap.com | 0.7 s | 0.5 s | plain gopher |

The live TLS checks pass on the TT as well: the four badssl.com servers
are refused with and without early requests, and a second connection in one
process resumes.

Before early requests, the cold visits to example.com, Google, NPR,
Hacker News and GitHub all failed ("no response"), and letsencrypt.org
failed on every visit.

Bodies arrive at 13-22 KB/s with ChaCha20 (the link runs ~35 KB/s). Hacker
News only offers AES-GCM: 6 KB/s.

## Memory

The static binary is 252 KB of code plus 131 KB of data/bss. The heap
peaks at ~180-250 KB with a TLS connection or two (each needs a 33 KB
record buffer). The total is ~650 KB, inside the 1 MB target; `ufetch -m`
caps the heap for testing.

## Known limits / next

- AES-only servers are slow (AES-GCM 17 KB/s on the 68030). ChaCha20 is
  offered first.
- RSA-4096 leaves (GitHub) cost ~10 s per full handshake, and GitHub doesn't
  resume. 68030 assembly for the RSA public operation would help most here.
- Cold visits to sites with ECDSA P-384 chains still take 1.5-2.5 minutes
  once. `ubtrust learn` with a preload bundle of common intermediates cuts
  that to one signature.
- The early-request policy is a deliberate trade-off (see 4 above): the
  browser must keep cookies, forms and credentials off early connections.
- AMIX itself hasn't been tested; everything ran on ASV through `amx`.
