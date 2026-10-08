/*
 * t425wipe - zero the ATW800/2's T425's memory (tls/tpwipe.h): what a
 * Helios session, or a Manx that was killed, left there. A reset clears
 * nothing, and anyone who can open the link can read it back.
 *
 *   t425wipe [-p] [DEVICE]
 *
 * All 5 MB by default: for the boot, before X starts (Atari System V's
 * /etc/rc2.d/S04t425wipe). The T425's first 2 MB are the card's memory
 * past 2 MB, which a desktop at 32 bits per pixel shows and draws in: -p
 * leaves them, zeroing only what the T425 alone sees. DEVICE: /dev/link1.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "os.h"
#include "tpwipe.h"

int main(int argc, char **argv)
{
	static const unsigned long all[][2] = {
		{ TPW_CHIP_FREE, TPW_EXT }, { TPW_EXT, TPW_REGS },
		{ TPW_SHARED_END, TPW_END }
	};
	static const unsigned long own[][2] = {
		{ TPW_CHIP_FREE, TPW_EXT }, { TPW_SHARED_END, TPW_END }
	};
	const char *dev = "/dev/link1";
	int a = 1, only_own = 0, fd, rc;
	unsigned long t0;

	if (a < argc && strcmp(argv[a], "-p") == 0) {
		only_own = 1;
		a++;
	}
	if (a < argc && argv[a][0] == '-') {
		fprintf(stderr, "usage: t425wipe [-p] [DEVICE]\n");
		return 2;
	}
	if (a < argc)
		dev = argv[a];
	if ((fd = open(dev, O_RDWR)) < 0) {
		perror(dev);
		return 1;
	}
	t0 = os_msec();
	rc = only_own ? tpwipe(fd, own, 2) : tpwipe(fd, all, 3);
	close(fd);
	if (rc < 0) {
		fprintf(stderr, "t425wipe: %s: the T425 didn't answer, or its "
			"memory didn't come back zeroed\n", dev);
		return 1;
	}
	printf("t425wipe: the T425's memory zeroed (%s), %lu ms\n",
		only_own ? "all but what the card shares" : "all of it",
		os_msec() - t0);
	return 0;
}
