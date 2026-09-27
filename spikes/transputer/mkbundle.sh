#!/bin/sh
# mkbundle.sh OUT - a flat source directory for the INMOS C compiler (icc):
# tpbench plus the BearSSL i15/m15 sources it needs, with
#   - a stdint.h for a 32-bit machine with no 64-bit integers (uint64_t is
#     a struct, so declarations that mention it still compile)
#   - a slim bearssl.h (hash, block, rand, ec, rsa) with `inline` defined
#     away: 1990 C has no inline, and the functions become plain statics
#   - inner.h without its 64-bit helpers and its TLS section
#   - config.h fixing BearSSL's feature switches (no 64-bit paths, no OS
#     random source, no unaligned access)
# The result is checked with the host GCC as strict C89 without long long,
# then built on the transputer by tp-build.sh.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$here/../..
br=$top/third_party/bearssl
out=$1
[ -n "$out" ] || { echo "usage: mkbundle.sh OUT" >&2; exit 2; }
mkdir -p "$out"

SRC="codec/ccopy ec/ec_c25519_m15 ec/ecdsa_atr ec/ecdsa_i15_bits
ec/ecdsa_i15_vrfy_asn1 ec/ecdsa_i15_vrfy_raw ec/ec_p256_m15 ec/ec_prime_i15
ec/ec_secp256r1 ec/ec_secp384r1 ec/ec_secp521r1 int/i15_add int/i15_bitlen
int/i15_decmod int/i15_decode int/i15_encode int/i15_fmont int/i15_iszero
int/i15_modpow2 int/i15_modpow int/i15_montmul int/i15_muladd int/i15_ninv15
int/i15_rshift int/i15_sub int/i15_tmont int/i32_div32 rsa/rsa_i15_pub"

for s in $SRC; do
	cp "$br/src/$s.c" "$out/"
done
for h in bearssl_hash.h bearssl_block.h bearssl_rand.h bearssl_ec.h bearssl_rsa.h; do
	cp "$br/inc/$h" "$out/"
done
cp "$here/tpbench.c" "$out/"

# the expected answers, computed with the full BearSSL on the host
[ -f "$top/build/host/libbearssl.a" ] || make -C "$top" build/host/libbearssl.a
cc -std=c99 -O2 -I"$br/inc" "$here/gen_vectors.c" "$top/build/host/libbearssl.a" \
	-o "$out/gen_vectors.host"
"$out/gen_vectors.host" > "$out/tpvec.h"
rm "$out/gen_vectors.host"

cat > "$out/stdint.h" <<'EOF'
/* stdint.h for the transputer bundle: 32-bit int, no 64-bit integers */
#ifndef TP_STDINT_H
#define TP_STDINT_H
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef struct { uint32_t hi, lo; } uint64_t;	/* declarations only */
typedef unsigned long uintptr_t;
#endif
EOF

cat > "$out/bearssl.h" <<'EOF'
/* bearssl.h for the transputer bundle: only what public-key code needs */
#ifndef BR_BEARSSL_H__
#define BR_BEARSSL_H__
#define inline
#include <stddef.h>
#include <stdint.h>
#include "bearssl_hash.h"
#include "bearssl_block.h"
#include "bearssl_rand.h"
#include "bearssl_ec.h"
#include "bearssl_rsa.h"
/* named in inner.h's hash-support prototypes (from bearssl_prf.h) */
typedef struct {
	const void *data;
	size_t len;
} br_tls_prf_seed_chunk;
#endif
EOF

cat > "$out/config.h" <<'EOF'
/* config.h for the transputer bundle */
#ifndef CONFIG_H__
#define CONFIG_H__
#define BR_64 0
#define BR_INT128 0
#define BR_UMUL128 0
#define BR_LE_UNALIGNED 0
#define BR_BE_UNALIGNED 0
#define BR_USE_URANDOM 0
#define BR_USE_WIN32_RAND 0
#define BR_USE_UNIX_TIME 0
#define BR_USE_WIN32_TIME 0
#define BR_RDRAND 0
#define BR_AES_X86NI 0
#define BR_SSE2 0
#define BR_POWER8 0
#define BR_NO_UINT64 1
#define BR_CT_MUL31 0
#define BR_CT_MUL15 0
#define BR_NO_ARITH_SHIFT 1	/* icc: signed >> is not arithmetic */
#define BR_SLOW_MUL 0
#define BR_SLOW_MUL15 0
#define BR_LOMUL 0
#define BR_ARMEL_CORTEXM_GCC 0
#endif
EOF

# inner.h: fence off the 64-bit helpers and drop the TLS section and the
# compiler-intrinsics section (both need types the slim header lacks)
python3 - "$br/src/inner.h" "$out/inner.h" <<'EOF'
import sys
s = open(sys.argv[1]).read()
a = s.index("static inline void\nbr_enc64le(")
b = s.index("}\n", s.index("br_dec64be(const void *src)")) + 2
s = s[:a] + "#if !BR_NO_UINT64\n" + s[a:b] + "#endif\n" + s[b:]
t0 = s.index(" * SSL/TLS support functions.")
t0 = s.rindex("/* ====", 0, t0)
t2 = s.rindex("#endif")		# the include guard
s = s[:t0] + s[t2:]
open(sys.argv[2], "w").write(s)
EOF
# the toolset's <...> search path is its own library directory: point the
# sources at the bundle's stdint.h
for f in "$out"/*.[ch]; do
	sed 's/#include <stdint.h>/#include "stdint.h"/' "$f" > "$f.tmp" && mv "$f.tmp" "$f"
done
cp "$here/tp-build.sh" "$out/"
echo "bundle in $out: $(ls "$out" | wc -l) files"
