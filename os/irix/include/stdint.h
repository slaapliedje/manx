/* stdint.h - the C99 fixed-width types. IRIX 6.5 has them in <inttypes.h>
 * but no <stdint.h>; the Makefile's TARGET=irix puts this directory first. */
#ifndef MANX_IRIX_STDINT_H
#define MANX_IRIX_STDINT_H
#include <inttypes.h>

#ifndef INT8_MAX
#define INT8_MIN	(-128)
#define INT8_MAX	127
#define UINT8_MAX	255
#define INT16_MIN	(-32767-1)
#define INT16_MAX	32767
#define UINT16_MAX	65535
#endif
#ifndef INT32_MIN
#define INT32_MIN	(-2147483647-1)
#endif
#ifndef INT64_MAX
#define INT64_MIN	(-9223372036854775807LL-1)
#define INT64_MAX	9223372036854775807LL
#define UINT64_MAX	18446744073709551615ULL
#endif
#ifndef SIZE_MAX
#define SIZE_MAX	4294967295U	/* n32: 32-bit size_t */
#endif
#ifndef INTPTR_MAX
#define INTPTR_MIN	(-2147483647L-1)
#define INTPTR_MAX	2147483647L
#define UINTPTR_MAX	4294967295UL
#endif
#ifndef INTMAX_MAX
#define INTMAX_MIN	INT64_MIN
#define INTMAX_MAX	INT64_MAX
#define UINTMAX_MAX	UINT64_MAX
#endif
#endif
