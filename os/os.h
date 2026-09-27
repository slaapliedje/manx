/*
 * os.h - what the browser needs from the operating system, with one
 * implementation per target (os/host, os/sysv4).
 */
#ifndef UB_OS_H
#define UB_OS_H

/* Monotonic-enough wall clock in microseconds since the first call.
 * 32 bits of microseconds wrap after 71 minutes: callers take
 * differences of nearby readings only. */
unsigned long os_usec(void);

/* Milliseconds since the first call (wraps after 49 days). */
unsigned long os_msec(void);

#endif /* UB_OS_H */
