# Phase 4 results: forms, cookies, cache, settings, bookmarks, gzip

## What was built

| Module | What it does |
|---|---|
| `net/inflate` | streaming DEFLATE with gzip and zlib wrappers (and raw deflate, which some servers send as "deflate"). Input can be cut anywhere: each step waits until the input it can need is buffered (48 bits for a symbol, about 560 bytes for a dynamic block header). 9-bit lookup table, CRC-32/Adler-32 checked, 33 KB of state |
| `net/fetch` | `Accept-Encoding: gzip, deflate`; bodies decoded as they arrive; a corrupt or cut-short stream is an error. `fetch_ex`: POST bodies, extra header lines, a same-origin `Referer`. After a POST, 301/302/303 continue with a GET, 307/308 with the POST |
| `net/cookie` | the cookie jar (RFC 6265): host-only and domain cookies, path matching, Secure only over https (and only from https), Max-Age/Expires with the RFC's date parser, replacement keeping the creation order, the longest path first. It rejects public suffixes: single labels plus a list of common second levels, not the full 200 KB list. Persistent cookies go in `~/.manx/cookies` (mode 600); session cookies stay in memory. Limits: 300 cookies, 96 KB. Set-Cookie is taken from every response, redirects included |
| `net/cache` | pages on disk in `~/.manx/cache`: one file per URL (FNV hash; the head repeats the URL, so a collision is a miss), written through a temporary file and renamed into place. Oldest out past the limit (2 MB by default); a page over half the limit isn't kept. Freshness from Cache-Control and Expires |
| `html/forms` | the fields of a page with their owning form. When sloppy HTML ends a form early inside a table, a field falls back to the last form before it. Radio groups, select options, reset. The submission follows HTML's algorithm: urlencoded, multipart or text/plain; only the button pressed counts; image buttons send x/y; disabled fields are left out; textarea newlines go as CRLF; a windows-1252 page is answered in windows-1252, with `&#N;` for what it lacks. `javascript:` actions are refused |
| `os/config` | `~/.manx/config`, `key = value`; `MANX_KEY` in the environment overrides |
| `src/manx.c` | fields are edited with Enter:<br>• Enter in a text field moves to the next one; in the last one it sends the form, as browsers do<br>• checkboxes and radios toggle<br>• a select opens a menu<br>• a textarea opens `$VISUAL`/`$EDITOR`/`vi`<br>• password entry shows `*`<br>• sending a password over plain http asks first<br>Back and history use the cache; a stale page is checked with If-None-Match or If-Modified-Since, and a 304 means use the cached copy; `r` always fetches again. A POST's answer is cached under its own key, so Back shows it without sending anything, and `r` there asks before resending. Bookmarks (`a`, `v`) are an HTML file. The status line describes the selected field |

Security rules kept from Phase 1: a request carrying cookies or a form
body is never sent before the certificate check (no early request).
Cache and cookie files are mode 600 in a 700 directory.

## Tests

| Test | Cases |
|---|---|
| `test_inflate` | 1840 checks: corpus pages at levels 1/6/9, gzip and zlib, fed whole and in pieces of 1 to 4096 bytes; stored and empty streams; truncated streams never reported complete; 200 corrupted streams per page. Also clean under ASan/UBSan |
| `test_cookie` | 27: dates, host-only/domain, public suffixes, paths and their order, Secure, replacement, expiry, limits, the file |
| `test_forms` | 9: GET and POST, every field type, radio groups, reset, windows-1252, a form cut by a table, multipart, `javascript:`, image buttons |
| `test_cache` | 12: store, read back, miss, eviction, oversized pages skipped, freshness rules |
| `test_net.sh` | 24 cases against `tests/netserver.py`, now with gzip (whole, chunked, raw deflate, cut short), cookies across a redirect and a second request, POST, and POST followed by 303 and by 307. All pass on the host and on the TT |
| fuzzer | now also collects, edits and submits the forms of every mutated page, and lays them out with the edited values |
| UI, locally with `tools/uidrive.py` | a GET form, a POST login with every field type, the password warning, the option menu, Back/reload after a POST, the cache (Back from it, `r` past it, ETag → 304), bookmarks |
| UI, on real sites | a DuckDuckGo Lite search through its POST form |

## On the TT

| | |
|---|---|
| gzip decoding | 80-100 KB/s of output |
| Hacker News | 34.6 KB of HTML arrives as 5.6 KB: 10.2 s with a full TLS handshake and certificate check, the body 0.4 s of it |
| Wikipedia article | 131 KB arrives as 26 KB: 13.0 s total with a full handshake |
| Back to a cached page | about 1.2 s: parse and layout, no network |
| Hacker News, DuckDuckGo, Wikipedia, NPR, BBC | all send gzip |

The TT's network does about 35 KB/s, so decoding is now the slower step,
and it's still a clear gain: fewer bytes to wait for and fewer to
decrypt.

## Left for later

- Tables as grids.
- `<select multiple>`: treated as a single choice.
- A 304 doesn't extend the cached copy's freshness (the next visit asks
  again).
- File upload.
