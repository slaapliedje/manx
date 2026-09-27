/* entropy.h - seeding the TLS random generator on systems without one. */
#ifndef UB_ENTROPY_H
#define UB_ENTROPY_H

struct entropy_report {
	int urandom;		/* /dev/urandom contributed (not on SVR4) */
	int seedfile;		/* a saved seed contributed */
	int jitter_distinct;	/* distinct clock-jitter deltas observed */
};

/* Fill out[32] from every source available and refresh the seed file
 * (seed_path may be NULL). */
void entropy_gather(unsigned char out[32], const char *seed_path,
	struct entropy_report *rep);

#endif /* UB_ENTROPY_H */
