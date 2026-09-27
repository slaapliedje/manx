/*
 * os_time.c - clock for AMIX / Atari System V.
 *
 * gettimeofday is an hrtcntl system call there: if it fails, the timeval
 * holds garbage, so ask again, and never let the clock run backwards.
 */
#include <sys/time.h>
#include "os.h"

extern int gettimeofday(struct timeval *, void *);

static struct timeval s_t0;

static void now(long *sec, long *usec)
{
	struct timeval t;
	int tries;

	for (tries = 0; gettimeofday(&t, 0) < 0; tries++)
		if (tries > 100) {
			t = s_t0;
			break;
		}
	if (s_t0.tv_sec == 0)
		s_t0 = t;
	*sec = t.tv_sec - s_t0.tv_sec;
	*usec = t.tv_usec - s_t0.tv_usec;
}

unsigned long os_usec(void)
{
	static unsigned long last;
	unsigned long v;
	long s, us;

	now(&s, &us);
	v = (unsigned long)s * 1000000UL + (unsigned long)us;
	if ((long)(v - last) < 0)
		v = last;
	last = v;
	return v;
}

unsigned long os_msec(void)
{
	static unsigned long last;
	unsigned long v;
	long s, us;

	now(&s, &us);
	/* whole seconds and the microsecond parts separately: a microsecond
	 * total overflows 32 bits after 71 minutes */
	v = (unsigned long)s * 1000UL + (unsigned long)(us / 1000);
	if ((long)(v - last) < 0)
		v = last;
	last = v;
	return v;
}
