/*
 * stdio.h - AMIX's, plus snprintf/vsnprintf, which SVR4.0 predates
 * (os/sysv4/snprintf.c). toolchain/sysv4-cc puts this directory ahead of
 * the system headers.
 */
#ifndef MANX_SYSV4_STDIO_H
#define MANX_SYSV4_STDIO_H

#include_next <stdio.h>
#include <stdarg.h>
#include <stddef.h>

int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

#endif /* MANX_SYSV4_STDIO_H */
