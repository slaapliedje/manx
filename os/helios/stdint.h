/*
 * stdint.h for Helios C (Norcroft, 1992), which has none. int and long are
 * 32 bits wide, and there is no 64-bit integer type: uint64_t is 32 bits,
 * as BearSSL's BR_NO_U64 asks (counters and lengths only, see its
 * config.h), and tests/no_u64.h makes it on the host.
 */
#ifndef MANX_HELIOS_STDINT_H
#define MANX_HELIOS_STDINT_H

typedef signed char		int8_t;
typedef short			int16_t;
typedef int			int32_t;
typedef unsigned char		uint8_t;
typedef unsigned short		uint16_t;
typedef unsigned int		uint32_t;
typedef unsigned int		uint64_t;	/* (32 bits: see above) */
typedef int			intptr_t;
typedef unsigned int		uintptr_t;

#define INT8_MAX	127
#define INT16_MAX	32767
#define INT32_MAX	2147483647
#define UINT8_MAX	255
#define UINT16_MAX	65535
#define UINT32_MAX	4294967295U

#endif /* MANX_HELIOS_STDINT_H */
