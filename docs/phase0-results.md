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

- ~~The transputer spike~~: done, see "Transputer spike" below.
- AMIX itself: everything so far ran on ASV through `amx`, not on an Amiga.

## Transputer spike (2026-09-27)

BearSSL's 15-bit public-key code (`i15`/`m15`, the only variant with no
64-bit integers) compiled **on the slot-1 T800 itself** with INMOS icc
2.01 through sp1's `iserver`, then booted over `/dev/link0` and run against
answers computed on the host (`spikes/transputer/`: `mkbundle.sh` builds a
flat C89 bundle, `tp-build.sh` builds and runs it on the TT).

| Operation (same i15/m15 C code) | T800 | 68030 | 68030 best (m31/i31/i32) |
|---|---|---|---|
| X25519 mul | 3.5 s | 2.4 s | 1.0 s |
| P-256 mul | 11.3 s | 4.6 s | 3.1 s |
| ECDSA P-256 verify | 21.4 s | 8.5 s | 5.8 s |
| RSA-2048 public | 11.3 s | 3.0 s | 2.3 s |
| ECDSA P-384 verify | 161 s | 52.6 s | 45.6 s |

All results are correct, once `BR_NO_ARITH_SHIFT` is set: icc's signed
`>>` is logical unless `-FS` is passed, which silently broke the m15
curves on the first run.

**Verdict:** as compiled C, the T800 is 1.5-3.7× slower than the 68030 on
the same code, and 3-7× slower than the 68030's best. icc 2.01 has no
optimiser, and C can't express the transputer's 32×32→64 `lmul`. So
offloading compiled BearSSL is not worth it for speed. Its only value would
be parallelism: 2 TRAMs + the T425 verifying signatures while the 68030
does the key exchange. Even then, one P-256 verify at 21 s is longer than
servers wait.

The remaining transputer option is **hand-written assembly** for Montgomery
multiplication using `lmul`/`lsum` on 32-bit limbs (variable time is fine
for public-key verification). Whether that beats the 68030's `mulu.l` has to be
measured. Until then Phase 1 relies on the 68030 measures: intermediate
cache, RSA first, deferred validation, session resumption, and 68030
assembly bignum.

Practical findings:
- `iserver` link errors ("protocol error, timed out ...") hit about one in
  3-4 compiles, sometimes several in a row. A real offload protocol needs
  framing, checksums and retries.
- iserver's command line is short: link through an indirect file
  (`-f tpbench.lnk`).
- A killed script can leave an orphaned `iserver` holding `/dev/link0`
  ("Device busy").
- Scripts on the TT: AMIX bash died silently in the retry function
  (`unwind_frame_discard` warnings); plain ASV `sh` works, without `test -nt`.
- Compiling one BearSSL file on the T800 takes about 1.3 minutes; the whole
  bundle takes about 45 minutes.

## Hand-written `lmul` (2026-10-03)

The measurement the spike left open. `spikes/transputer/lmul/tpmont.c`:
RSA-2048's public operation (x^65537 mod n: 16 squarings, one multiply
and the two conversions, 19 Montgomery multiplications, 155,648 32x32->64
multiply-adds) with 32-bit limbs, CIOS, the inner row `r += a * b` written
several ways. Every run's answer is checked against Python's
(`mkvec.py`). `tp-lmul.sh` compiles it with icc on the slot-1 T800 and runs
it on a transputer.

| RSA-2048 public op | time | per multiply-add |
|---|---|---|
| 68030, BearSSL `br_rsa_i32` (Manx today, whole check) | ~2.4 s | |
| 68030, C from 16-bit halves | 1.26-1.41 s | 8.1-9.1 us |
| 68030, C with `unsigned long long` (GCC: `mulu.l`) | 0.58-0.71 s | 3.7-4.6 us |
| 68030, 8-instruction `mulu.l` loop | 0.42-0.48 s | 2.7-3.1 us |
| T800 (TRAM slot 1), C from 16-bit halves (icc) | 3.41 s | 21.9 us |
| T800 (TRAM slot 1), `lmul` loop unrolled 4x | 0.77 s | 4.9 us |
| T425 (the FPGA's, 40 MHz), C from 16-bit halves (icc) | 0.85 s | 5.4 us |
| T425 (the FPGA's, 40 MHz), `lmul` loop unrolled 4x | 0.28 s | 1.8 us |

A loop of `ldc; ldc; ldc; lmul` reads as 18.7 MHz on the T800 (so a
20 MHz part, if `lmul` takes the data sheet's 33 cycles) and as 49.8 MHz
on the T425: the FPGA's `lmul` is faster than a real T425's.

What it says:

- **The 68030 first.** BearSSL's `i32` code is general: its modpow does
  two multiplications per exponent bit (36 for e = 65537, where 18 do), and
  its 64-bit additions compile long. A verification-only path (public
  exponent, variable time is fine) with the `mulu.l` loop is ~5x faster,
  before the transputers: about 0.5 s for RSA-2048 with the key's setup,
  about 2 s for RSA-4096 (today ~10 s). AMIX gets it too.
- **The FPGA's T425 is the transputer worth using:** 1.6x the 68030's best,
  on `/dev/link1` directly, and it runs while the 68030 does something
  else. A signature check sends under 1 KB over the link.
- **The T800 TRAMs are slower than the 68030** at this (0.77 s against
  0.43 s), about 99 cycles a multiply-add against the loop's ~64 on paper
  (memory waits, probably). They add capacity for chains of several checks,
  not speed for one.
- Nothing here helps a resumed session (no public-key work) or a slow
  network: the TT's first visit to geekdot.com the same day took 6.5 min,
  ~75 s of it CPU, the page coming at 2 KB/s over the WiFi.

In Manx since: `tls/rsavrfy.c` checks every RSA signature (the server's
key exchange, certificates) with the `mulu.l` loop and a public exponent's
square-and-multiply, R^2 mod n by doubling. On the TT, setup included
(`tlsbench 1 rsa`): RSA-2048 2.30 s -> 0.52 s, RSA-4096 9.61 s -> 2.40 s,
the answers the same as BearSSL's (`tests/test_rsa.c`: 316 cases there,
3028 and four generated keys' signatures on the host).

And ECDSA (`tls/ecvrfy.c`, on the same multiplication, split out as
`tls/mont.c`): Jacobian points, u1 G + u2 Q by interleaved width-4 NAF,
inverses by binary GCD, x = r checked as r Z^2 = X. On the TT
(`tlsbench 1 ecdsa`): P-256 5.49 s (br_ec_p256_m31) -> 1.92 s, P-384
42.2 s (br_ec_prime_i31) -> 5.54 s. P-521 still goes to BearSSL.
`tests/test_ecdsa.c` checks it against br_ecdsa_i31 with keys and
signatures BearSSL makes (the TT agrees too).
