/*
 * conin.c - keys from Helios's console, with a timeout. Helios's select()
 * is for sockets: on its console it neither times out nor wakes for a
 * key. So frontend/screen.c reads here, as Helios's own programs do, with
 * Read() on the stream under file 0 and its timeout (microseconds).
 */
#include <helios.h>
#include <syslib.h>

extern Stream *fdstream(int fd);

/* up to n bytes, waiting at most ms (forever if < 0) for the first, then
 * taking any that are already there (the rest of an escape sequence): the
 * number read, 0 if none */
int os_con_read(char *buf, int n, int ms)
{
	Stream *s = fdstream(0);
	int got = 0;
	word r;

	if (s == NULL || n <= 0)
		return 0;
	r = Read(s, (byte *)buf, 1, ms < 0 ? -1 : (word)ms * 1000);
	if (r <= 0)
		return 0;
	got = 1;
	while (got < n && Read(s, (byte *)buf + got, 1, 0) == 1)
		got++;
	return got;
}
