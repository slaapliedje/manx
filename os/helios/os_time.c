/*
 * os_time.c - clock for Helios (the ATW800/2's transputer): gettimeofday,
 * never running backwards.
 */
#include <sys/time.h>
#include "os.h"

static struct timeval s_t0;

static void now(long *sec, long *usec)
{
	struct timeval t;

	gettimeofday(&t, 0);
	if (s_t0.tv_sec == 0)
		s_t0 = t;
	*sec = (long)(t.tv_sec - s_t0.tv_sec);
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
	v = (unsigned long)s * 1000UL + (unsigned long)(us / 1000);
	if ((long)(v - last) < 0)
		v = last;
	last = v;
	return v;
}
