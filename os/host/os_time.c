/* os_time.c - clock for the host (Linux) build. */
#define _POSIX_C_SOURCE 200112L
#include <time.h>
#include "os.h"

static struct timespec s_t0;

static void now(long *sec, long *nsec)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	if (s_t0.tv_sec == 0 && s_t0.tv_nsec == 0)
		s_t0 = t;
	*sec = (long)(t.tv_sec - s_t0.tv_sec);
	*nsec = t.tv_nsec - s_t0.tv_nsec;
}

unsigned long os_usec(void)
{
	long s, ns;

	now(&s, &ns);
	return (unsigned long)s * 1000000UL + (unsigned long)(ns / 1000);
}

unsigned long os_msec(void)
{
	long s, ns;

	now(&s, &ns);
	return (unsigned long)s * 1000UL + (unsigned long)(ns / 1000000);
}
