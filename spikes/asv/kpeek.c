/*
 * kpeek ADDR... - print the 32-bit words at kernel addresses (hex) from
 * /dev/kmem. Read-only; root. For looking at ASV's cache settings
 * (softcacr, cacheconfig) without touching anything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

int open(const char *, int, ...);
int read(int, void *, unsigned);
long lseek(int, long, int);

int main(int argc, char **argv)
{
	int fd = open("/dev/kmem", O_RDONLY), i;

	if (fd < 0) {
		perror("/dev/kmem");
		return 1;
	}
	for (i = 1; i < argc; i++) {
		unsigned long a = strtoul(argv[i], 0, 16);
		unsigned char b[4];

		if (lseek(fd, (long)a, 0) < 0 || read(fd, b, 4) != 4) {
			printf("%08lx: unreadable\n", a);
			continue;
		}
		printf("%08lx: %02x%02x%02x%02x\n", a, b[0], b[1], b[2], b[3]);
	}
	return 0;
}
