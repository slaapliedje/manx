# Phase 0 results (2026-09-27)

Measured on the real TT030 (32 MHz 68030, Atari System V UE12, 100 MB),
with a static AMIX binary: GCC 15 (mint) `-m68030 -msoft-float -O2`, AMIX
libs, BearSSL 0.6 run through ASV's `amx` module. All numbers are from
`build/sysv4/tlsbench` and `build/sysv4/ufetch`.

## Toolchain

- The OpenUA route works unchanged for us: `toolchain/sysv4-cc` (GCC 15 →
  SVR4 `as`) + `sysv4-ld` (static link against AMIX `libc.a` /
  `libsocket.a`). BearSSL builds as-is. One binary serves ASV and AMIX.
- Correctness on the target: the SHA-1/256/384 and X25519 (RFC 7748)
  known-answer tests pass. Every implementation agrees with the others, and the
  fingerprints of all outputs match the host build: `sym d4ccbfd7171a8275`,
  `ec 838095863525da53`, `ecdsa 8fadffca2b8c9bdd`, `rsa e02307053e3b72e2`.
- `-Os` is no faster than `-O2` (the 256-byte caches aren't the limit):
  keep `-O2`.
- Our own `snprintf` (SVR4.0 has none) is tested against glibc: `make test`.

## Crypto speed on the TT (best implementation of each)

| Operation | Best impl | Time |
|---|---|---|
| X25519 scalar mult | c25519_m31 | 1.0 s (m15 2.4 s, i15/i31 4-4.5 s) |
| P-256 mulgen / mul | p256_m31 | 2.1 s / 3.1 s |
| P-384 mul | prime_i31 | 18.8 s |
| ECDSA P-256 verify | i31 + p256_m31 | 5.8 s |
| ECDSA P-384 verify | i31 + prime_i31 | 45.6 s |
| RSA-2048 public (verify) | i31 / i32 | 2.3-2.5 s |
| RSA-4096 public | i32 | 9.7 s |
| ChaCha20-Poly1305 | ctmul | 67 KB/s |
| AES-128-GCM | big + ctmul | 17 KB/s (constant-time ct: 11 KB/s) |
| SHA-1 / SHA-256 / SHA-384 | | 162 / 67 / 28 KB/s |

**Choices:** m31/i31 everywhere (i32 for RSA-4096). ChaCha20-Poly1305 is the
only bulk cipher faster than the network (~35 KB/s), so offer it first.
Prefer X25519: P-256 key exchange costs about 5 s, against about 2 s.

## Real HTTPS from the TT (`ufetch`)

The handshake trace, example.com over RSA with validation inline (BearSSL's default):

| t | event |
|---|---|
| 0.2 s | certificate chain received |
| 31.6 s | chain validated (the RSA-4096 root signature dominates) |
| 36.5 s | ServerKeyExchange signature checked |
| 38.8 s | our key exchange sent: **the server had already hung up** |

Servers give up on a silent client after roughly 10-20 s (Google about 20 s).

**Deferred validation** (`tls/xdefer.c` + a small BearSSL hook,
`third_party/patches/bearssl-ske-defer.patch`) records the chain and the
ServerKeyExchange signature and finishes the handshake at once. The real
checks then run before anything is sent. It rejects expired, wrong-host,
self-signed and untrusted-root certificates (badssl.com, host build).

| Site | Handshake | Validation | Result |
|---|---|---|---|
| example.com (RSA), full chain | 3.4 s | 31.7 s | handshake OK, server closed before the request |
| example.com (RSA), intermediates as anchors | 3.9 s | 4.7 s | **200 OK** |
| www.google.com (RSA), intermediates as anchors | 3.0 s | 4.7 s | **200 OK, 84 KB, 10 KB/s** |
| example.com (ECDSA), full chain | 2.9 s | 104 s | server closed |
| example.com (ECDSA), intermediates as anchors | 2.9 s | 15.2 s | server closed (idle timeout < 15 s) |
| letsencrypt.org (ECDSA, P-256 key exchange) | fails at 6.2 s | – | server closes right after our Finished: to be investigated |

So after the handshake a server waits about 10-15 s for the request. The
whole validation has to fit inside that.

## Other findings

- **DNS:** our resolver works on the TT (60-240 ms). The system resolver is
  unusable from a static binary.
- **Trust store:** decoding PEM on the 68030 costs about 0.4-0.7 s per
  certificate. The full 121-root bundle takes **52 s**. A precompiled binary
  anchor file is required.
- **Entropy:** the TT's `gettimeofday` jitter gives only 3-9 distinct deltas
  in 512 samples, so timing jitter is nearly worthless. The seed file,
  keystroke and network timing have to carry the pool (and possibly the
  transputers' independent clocks).
- **The TT's load varies:** the same X25519 exchange took 2 s in one run
  and 5 s in another.

## What this changes in the plan

1. Deferred validation is the default.
2. **Cache verified intermediates** as anchors, persisted after the TT has
   verified them once. Ship a preload list of common intermediates to
   verify at install time. This turns a 32-104 s validation into 5-15 s.
3. **RSA certificate first, ECDSA as fallback:** RSA leaf plus RSA
   ServerKeyExchange verification takes about 5 s, against about 12 s for
   P-256 and far more for P-384 issuers.
4. Faster verification: variable-time 68030 assembly bignum for the public
   operations (verification involves no secrets); offload to the transputers,
   verifying signatures in parallel with the 68030's key exchange.
5. Session resumption, so a repeat connection needs no public-key work.
6. A binary trust-anchor cache (the 52 s PEM load).
7. Look into letsencrypt.org: P-256 was chosen over X25519, and the server
   closes after our Finished.
8. Policy for requests while the server waits: validate first always for
   cookies and POST. Sending a plain GET early while holding the response
   until validation finishes is a possible, explicitly weaker option.

## Not done in Phase 0

- The transputer spike. The tools exist: sp1 `iserver` and `icc` run on the
  slot-1 T800, and `/dev/link0`/`link1` work. Next step: compile BearSSL's
  `ec_prime_i31`/`rsa_i31_pub` with `icc -t800` and time a P-384 verify.
- AMIX itself: everything so far ran on ASV through `amx`, not on an Amiga.
