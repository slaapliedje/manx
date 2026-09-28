/*
 * entropy.h - the random seed pool for TLS, on systems without a random
 * device.
 *
 * Every source is hashed into a SHA-256 pool, with a conservative
 * estimate of the bits it adds. TLS gets seed material only once the
 * estimate reaches ENTROPY_NEEDED. On the 68030 machines the real sources
 * are a seed file saved by earlier runs (first made by `ubtrust seed`,
 * from keystroke timing or imported from another machine), keystroke
 * timing, and network arrival timing; the TT's clock jitter is nearly
 * worthless (3-9 distinct values in 512 samples).
 */
#ifndef UB_ENTROPY_H
#define UB_ENTROPY_H

#include <stddef.h>

#define ENTROPY_NEEDED	128

/* Gather what's available at start: the seed file (if seed_path isn't
 * NULL), /dev/urandom, clock jitter, process state. */
void entropy_init(const char *seed_path);

/* Mix in data, credited with `bits` of entropy. */
void entropy_add(const void *data, size_t len, int bits);

/* An unpredictable event happened now (a key press, a packet): mixes in
 * the time, credited with one bit. */
void entropy_event(void);

int entropy_bits(void);
int entropy_ready(void);

/* 32 bytes of seed material: 0, or -1 while the pool isn't ready. */
int entropy_extract(unsigned char out[32]);

/* Replace the seed file with fresh material for the next run (never the
 * extracted output itself). Called by entropy_extract, and at exit. */
void entropy_save(void);

/* What entropy_init found, for diagnostics. */
struct entropy_report {
	int urandom, seedfile, jitter_distinct;
};
const struct entropy_report *entropy_report(void);

#endif /* UB_ENTROPY_H */
