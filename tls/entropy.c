/*
 * entropy.c - see entropy.h.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/times.h>
#include "bearssl.h"
#include "os.h"
#include "entropy.h"

#ifdef UB_SYSV4
pid_t getpid(void);
#else
#include <unistd.h>
#endif

#define JITTER_SAMPLES	512

static br_sha256_context s_pool;
static int s_bits, s_inited;
static unsigned long s_counter;
static char s_seed_path[512];
static struct entropy_report s_rep;

static int read_exact(const char *path, unsigned char *buf, size_t len)
{
	FILE *f = fopen(path, "rb");
	size_t got;

	if (f == NULL)
		return 0;
	got = fread(buf, 1, len, f);
	fclose(f);
	return got == len;
}

void entropy_add(const void *data, size_t len, int bits)
{
	if (!s_inited) {
		br_sha256_init(&s_pool);
		s_inited = 1;
	}
	br_sha256_update(&s_pool, data, len);
	s_bits += bits;
	if (s_bits > 256)
		s_bits = 256;
}

void entropy_event(void)
{
	unsigned long t = os_usec();

	entropy_add(&t, sizeof t, 1);
}

static void jitter(void)
{
	static unsigned long d[JITTER_SAMPLES];
	unsigned long prev = os_usec(), v;
	volatile unsigned long sink = 0;
	int i, j, distinct = 0;

	for (i = 0; i < JITTER_SAMPLES; i++) {
		unsigned long n = 50 + (prev & 0x3F);

		for (j = 0; (unsigned long)j < n; j++)
			sink += (unsigned long)j * prev;
		v = os_usec();
		d[i] = v - prev;
		prev = v;
	}
	for (i = 0; i < JITTER_SAMPLES; i++) {
		for (j = 0; j < i; j++)
			if (d[j] == d[i])
				break;
		if (j == i)
			distinct++;
	}
	s_rep.jitter_distinct = distinct;
	/* credit only a clock that really wobbles: one bit per 16 distinct
	 * deltas, at most 32 (the TT's 3-9 distinct values earn nothing) */
	entropy_add(d, sizeof d, distinct >= 64 ? (distinct / 16 > 32 ? 32 : distinct / 16) : 0);
	entropy_add((const void *)&sink, sizeof sink, 0);
}

void entropy_init(const char *seed_path)
{
	unsigned char buf[32];
	struct tms tm;
	long v;

	entropy_add("ub entropy", 10, 0);
	if (read_exact("/dev/urandom", buf, sizeof buf)) {
		s_rep.urandom = 1;
		entropy_add(buf, sizeof buf, 256);
	}
	if (seed_path && strlen(seed_path) < sizeof s_seed_path) {
		strcpy(s_seed_path, seed_path);
		if (read_exact(seed_path, buf, sizeof buf)) {
			s_rep.seedfile = 1;
			entropy_add(buf, sizeof buf, 256);
		}
	}
	v = (long)time(NULL);
	entropy_add(&v, sizeof v, 0);
	v = (long)getpid();
	entropy_add(&v, sizeof v, 0);
	v = (long)times(&tm);
	entropy_add(&v, sizeof v, 0);
	entropy_add(&tm, sizeof tm, 0);
	jitter();
	memset(buf, 0, sizeof buf);
}

int entropy_bits(void) { return s_bits; }
int entropy_ready(void) { return s_bits >= ENTROPY_NEEDED; }
const struct entropy_report *entropy_report(void) { return &s_rep; }

/* H(pool state || tag || counter), leaving the pool itself unchanged */
static void derive(const char *tag, unsigned char out[32])
{
	br_sha256_context c = s_pool;

	s_counter++;
	br_sha256_update(&c, tag, strlen(tag));
	br_sha256_update(&c, &s_counter, sizeof s_counter);
	br_sha256_out(&c, out);
}

void entropy_save(void)
{
	unsigned char seed[32];

	if (!s_seed_path[0] || !entropy_ready())
		return;
	derive("next seed", seed);
	os_write_file(s_seed_path, seed, sizeof seed, 0600);
	memset(seed, 0, sizeof seed);
}

int entropy_extract(unsigned char out[32])
{
	unsigned long t;

	if (!entropy_ready())
		return -1;
	t = os_usec();
	entropy_add(&t, sizeof t, 0);
	derive("tls seed", out);
	/* ratchet: the next seed file never lets anyone replay this one */
	entropy_save();
	return 0;
}
