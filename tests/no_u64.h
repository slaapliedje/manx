/*
 * no_u64.h - build on the host what Helios C builds for the transputer: a
 * compiler with no 64-bit integer type (BR_NO_U64 in BearSSL's config.h).
 * Forced in ahead of every file with -include, after <stdint.h>, whose
 * uint64_t it then hides: uint64_t is 32 bits wide from here on, as in
 * os/helios/stdint.h. `make test-no64` runs BearSSL's tests and Manx's
 * TLS this way.
 */
#ifndef MANX_NO_U64_H
#define MANX_NO_U64_H

#include <stdint.h>

typedef uint32_t no_u64_t;
#define uint64_t no_u64_t

#define BR_NO_U64		1
#define BR_64			0
#define BR_INT128		0
#define BR_UMUL128		0
#define BR_AES_X86NI		0
#define BR_SSE2			0
#define BR_POWER8		0
#define BR_LE_UNALIGNED		0
#define BR_BE_UNALIGNED		0

#endif /* MANX_NO_U64_H */
