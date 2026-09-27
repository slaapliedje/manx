/*
 * stdio.h - AMIX's, plus snprintf/vsnprintf, which SVR4.0 predates
 * (os/sysv4/snprintf.c). toolchain/sysv4-cc puts this directory ahead of
 * the system headers.
 */
#ifndef UB_SYSV4_STDIO_H
#define UB_SYSV4_STDIO_H

#include_next <stdio.h>
#include <stdarg.h>
#include <stddef.h>

int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

#endif /* UB_SYSV4_STDIO_H */
