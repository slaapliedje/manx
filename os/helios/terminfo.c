/*
 * terminfo.c - Helios has no terminfo: setupterm() says so, and
 * frontend/screen.c then speaks ANSI, as Helios's console window does
 * (its termcap calls it "helios ansi window"), and so do the VT100s a
 * telnet client brings.
 */
#include <stddef.h>

int setupterm(char *term, int fd, int *err)
{
	(void)term;
	(void)fd;
	if (err)
		*err = 0;
	return -1;
}

char *tigetstr(char *cap)
{
	(void)cap;
	return (char *)-1;
}

int tigetnum(char *cap)
{
	(void)cap;
	return -2;
}

char *tparm(const char *s, ...)
{
	(void)s;
	return NULL;
}
