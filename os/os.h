/*
 * os.h - what the browser needs from the operating system, with one
 * implementation per target (os/host, os/sysv4).
 */
#ifndef MANX_OS_H
#define MANX_OS_H

#include <stddef.h>

/* Monotonic-enough wall clock in microseconds since the first call.
 * 32 bits of microseconds wrap after 71 minutes: callers take
 * differences of nearby readings only. */
unsigned long os_usec(void);

/* Milliseconds since the first call (wraps after 49 days). */
unsigned long os_msec(void);

/* --- files (os/os_files.c, all targets) --- */

/* The browser's own directory: $MANX_HOME, else $HOME/.manx (created, mode
 * 700). NULL when neither can be used. */
const char *os_datadir(void);

/* datadir/name into buf; NULL when it doesn't fit or there is no
 * datadir. */
char *os_datapath(char *buf, size_t n, const char *name);

/* Size and modification time of a file: 0, or -1 if it can't be read. */
int os_file_info(const char *path, long *size, long *mtime);

/* Write a whole file safely: to path.tmp, then renamed over path.
 * mode as for chmod. 0 or -1. */
int os_write_file(const char *path, const void *data, size_t len, int mode);

/* Read a whole file into a malloc'd buffer (xmalloc); NULL on error. */
unsigned char *os_read_file(const char *path, size_t *len);

/* Days since 1 January of year 0 (proleptic Gregorian) and seconds of
 * the day, now (UTC): BearSSL's X.509 time scale. */
void os_x509_now(unsigned long *days, unsigned long *seconds);

/* --- memory (os/mem.c, all targets) --- */

/* malloc/realloc/free with a global cap (mem_set_cap) and statistics:
 * a request that would pass the cap fails (returns NULL). */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void xfree(void *p);
char *xstrdup(const char *s);
void mem_set_cap(size_t bytes);		/* 0: no cap */
size_t mem_in_use(void);
size_t mem_peak(void);

#endif /* MANX_OS_H */
