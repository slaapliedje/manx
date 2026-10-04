# crypt68k

Fast public-key arithmetic for 68k machines, for TLS clients: RSA's
public operation and ECDSA on P-256 and P-384 (checking certificate
chains and the server's key exchange), and X25519 (the key exchange
itself). Plain C89 with hand-written 68020/030/040 inner loops, MIT
licensed, and a drop-in for [BearSSL](https://bearssl.org).

On a 32 MHz 68030 (an Atari TT030), against BearSSL 0.6's fastest code
for each:

| | BearSSL | crypt68k |
|---|---|---|
| RSA-2048 public operation | 2.30 s (`i32`) | 0.62 s |
| RSA-4096 public operation | 9.61 s (`i32`) | 2.30 s |
| ECDSA P-256 check | 5.49 s (`p256_m31`) | 1.89 s |
| ECDSA P-384 check | 42.2 s (`prime_i31`) | 5.59 s |
| X25519 | 2.60 s (`c25519_m31`) | 1.22 s |

(crypt68k's `bench`; BearSSL's own `tlsbench` figures from Manx, the
same machine. The X25519 pair comes from one `tlsbench` run, several
operations each, on a busier TT than the rest: compare the ratio.)

In [Manx](https://github.com/slaapliedje/manx), the browser it comes
from, a TT's first visit to a site with a Let's Encrypt P-384 chain went
from 147 s to 27 s.

## Time

**The signature checks take a time that depends on their inputs.** That
is what makes them fast, and it is fine for checking signatures: the
signature, the public key and the hash are all public. It is not fine
for anything secret: never use them to sign, to decrypt, or to encrypt
a secret with RSA (RSA key exchange).

**X25519 is built the other way:** no branch and no memory address in
it depends on the scalar or the point; the ladder swaps with masks,
carries are arithmetic, the inversion is a fixed chain. But it can only
be as constant-time as the CPU's multiply instruction, and Motorola's
MC68030 manual marks `MULU`'s execution time "data dependent".

Measured on a 32 MHz 68030 with `tools/mul_timing.c`: `mulu.l` and
`mulu.w` take about 2 cycles longer when the **source** operand's lowest
bit is 1. The other operand makes no difference, and nothing else about
the value showed (zero, one, a top bit, alternating bits, all ones). So
each multiply leaks the low bit of one operand. BearSSL's own X25519
uses the same instruction there. On a 68020 or 68030, use X25519 for
ephemeral keys (a TLS client's, new for every handshake), not for
long-lived ones. (68020: likely the same; 68040: not measured.)

## Why it's faster

- **Montgomery multiplication on 32-bit limbs** (CIOS), its inner loop
  eight 68k instructions around `mulu.l`'s 32x32 -> 64 multiply.
- **RSA for public exponents:** left-to-right square-and-multiply, 18
  multiplications for e = 65537. BearSSL's general modpow, built to keep
  a private exponent secret, does two for every bit of the exponent's
  bytes: 48.
- **X25519:** 8 limbs of 32 bits kept below 2^256, what overflows
  folded back as 38 (2^256 = 2p + 38); the 512-bit multiply and square
  unrolled product scanning, 64 and 36 `mulu.l`
  (`tools/gen_fe_m68k.py`).
- **ECDSA:** Jacobian coordinates (a = -3), u1 G + u2 Q by interleaved
  width-4 NAF over affine tables of G, 3G, 5G, 7G (made once) and the
  same for Q, inverses by binary GCD. All of it variable time, which a
  check can afford.

## Using it

```c
#include "crypt68k.h"

/* RSA: x = x^e mod n (then check the PKCS#1 padding against the hash) */
ok = c68k_rsa_public(x, xlen, n, nlen, e, elen);

/* ECDSA: q uncompressed (0x04, x, y); sig raw (r, s) or DER */
ok = c68k_ecdsa_verify_asn1(C68K_P384, q, qlen, hash, hash_len, sig, sig_len);

/* X25519 (RFC 7748): little-endian, 32 bytes */
c68k_x25519_base(my_public, my_secret);
c68k_x25519(shared, my_secret, their_public);
```

`crypt68k.h` also has the ECDSA check in steps (`c68k_ecdsa_scalars`,
`c68k_ec_muladd`, `c68k_ec_x_is_r`), for doing the heavy step somewhere
else: Manx hands it to the transputer on an ATW800/2 card.

### With BearSSL

`crypt68k_bearssl.h` has BearSSL's signature checker types. In a client's
setup:

```c
br_ssl_engine_set_rsavrfy(&cc.eng, c68k_br_rsa_pkcs1_vrfy);
br_ssl_engine_set_ecdsa(&cc.eng, c68k_br_ecdsa_vrfy_asn1);
br_x509_minimal_set_rsa(&xc, c68k_br_rsa_pkcs1_vrfy);
br_x509_minimal_set_ecdsa(&xc, &br_ec_all_m31, c68k_br_ecdsa_vrfy_asn1);
```

Not `br_ssl_engine_set_rsapub`: that one encrypts the premaster secret in
RSA key exchange suites. Curves other than P-256 and P-384 go on to
BearSSL's `br_ecdsa_i31`.

`c68k_br_ec_c25519` is X25519 as a `br_ec_impl`, like BearSSL's
`br_ec_c25519_m31`, for `br_ssl_engine_set_ec` (see the Time section).

## Building

```sh
make                 # libcrypt68k.a, test_kat, bench (here)
make test            # the known answers
make BEARSSL=../BearSSL test    # also the drop-ins, against BearSSL itself
```

For a 68k machine, name its compiler and CPU, then run `test_kat` and
`bench` there:

```sh
make CC=m68k-atari-mint-gcc AR=m68k-atari-mint-ar CFLAGS="-O2 -m68030"
make CC=m68k-amigaos-gcc AR=m68k-amigaos-ar CFLAGS="-O2 -m68030 -noixemul"
```

`-m68020`, `-m68030` and `-m68040` get the `mulu.l` loop. A 68060 traps
the 64-bit `mulu.l`, so `-m68060` and `-m68020-60` builds use the C loop
(`uint64_t`), as does every other machine. The 68000's multiply is
16x16 -> 32, so it is slow there either way.

`tools/mul_timing.c` (68k only) times `mulu.l` and `mulu.w` with
operand patterns, to see what a CPU's multiply time depends on.

## Tests

- `test_kat` checks known answers made by `tests/gen_vectors.py` with
  Python's integers: RSA on moduli of 512 to 4096 bits; real ECDSA
  signatures on both curves over 20- to 64-byte hashes, through the raw,
  DER and step-by-step calls, with ones that must fail (another hash,
  r + 1, s = 0, r = n, a key off the curve, malformed DER); X25519
  against RFC 7748's answers (and its iterated test: `test_kat long` for
  the thousand rounds) and u = 0, 1, p, above p, with the top bit set. It
  needs nothing else, so it runs on the 68k machine itself.
- `test_bearssl` compares the drop-ins with BearSSL's own code: thousands
  of random RSA cases with the edges (x >= n, even moduli, leading zeros,
  wrong lengths, e = 0), PKCS#1 signatures from keys BearSSL generates,
  ECDSA keys and signatures BearSSL makes, broken every way that
  matters, with the same answer from both, and X25519 against
  `br_ec_c25519_m31` on random points and scalars.

## Licence

MIT: see `LICENSE`.
