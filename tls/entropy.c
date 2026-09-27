/*
 * entropy.c - seed material for TLS on a system with no random device.
 *
 * SVR4.0 has no /dev/random. Sources, all hashed together with SHA-256:
 *   - /dev/urandom when there is one (the host build)
 *   - a seed file saved by the previous run (then replaced)
 *   - the clock, process id and CPU time
 *   - jitter: the clock's microsecond reading after busy loops, whose
 *     length depends on the previous reading
 *
 * PHASE 0 GRADE: good enough for a spike. Phase 1 adds keystroke and
 * network-arrival timing and refuses TLS until the pool is judged seeded.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/times.h>
#include <sys/stat.h>
#include "bearssl.h"
#include "os.h"
#include "entropy.h"

#ifdef UB_SYSV4
pid_t getpid(void);
int chmod(const char *, mode_t);
#else
#include <unistd.h>
#endif

#define JITTER_SAMPLES	512

static int read_file(const char *path, unsigned char *buf, size_t len)
{
	FILE *f = fopen(path, "rb");
	size_t got;

	if (f == NULL)
		return 0;
	got = fread(buf, 1, len, f);
	fclose(f);
	return got == len;
}

void entropy_gather(unsigned char out[32], const char *seed_path,
	struct entropy_report *rep)
{
	br_sha256_context h;
	unsigned char buf[32];
	unsigned long deltas[JITTER_SAMPLES];
	unsigned long prev, v;
	struct tms tm;
	long t;
	int i, j, distinct;
	volatile unsigned long sink = 0;

	memset(rep, 0, sizeof *rep);
	br_sha256_init(&h);

	if (read_file("/dev/urandom", buf, sizeof buf)) {
		br_sha256_update(&h, buf, sizeof buf);
		rep->urandom = 1;
	}
	if (seed_path && read_file(seed_path, buf, sizeof buf)) {
		br_sha256_update(&h, buf, sizeof buf);
		rep->seedfile = 1;
	}

	t = (long)time(NULL);
	br_sha256_update(&h, &t, sizeof t);
	t = (long)getpid();
	br_sha256_update(&h, &t, sizeof t);
	t = (long)times(&tm);
	br_sha256_update(&h, &t, sizeof t);
	br_sha256_update(&h, &tm, sizeof tm);

	prev = os_usec();
	for (i = 0; i < JITTER_SAMPLES; i++) {
		unsigned long n = 50 + (prev & 0x3F);

		for (j = 0; (unsigned long)j < n; j++)
			sink += (unsigned long)j * prev;
		v = os_usec();
		deltas[i] = v - prev;
		prev = v;
	}
	br_sha256_update(&h, deltas, sizeof deltas);
	br_sha256_update(&h, (const void *)&sink, sizeof sink);

	/* how much did the clock actually wobble? (a coarse clock gives
	 * few distinct deltas) */
	distinct = 0;
	for (i = 0; i < JITTER_SAMPLES; i++) {
		for (j = 0; j < i; j++)
			if (deltas[j] == deltas[i])
				break;
		if (j == i)
			distinct++;
	}
	rep->jitter_distinct = distinct;

	br_sha256_out(&h, out);

	/* the next run's seed: derived, never the output itself */
	if (seed_path) {
		FILE *f;

		br_sha256_update(&h, "next seed", 9);
		br_sha256_out(&h, buf);
		f = fopen(seed_path, "wb");
		if (f) {
			fwrite(buf, 1, sizeof buf, f);
			fclose(f);
			chmod(seed_path, 0600);
		}
	}
}
