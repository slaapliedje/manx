/*
 * tpboot BTL [LINK] - reset the transputer at LINK (/dev/link1: the
 * ATW800/2's FPGA T425), boot it with BTL (made by icollect -t), then time
 * echoes through tpecho: empty, 256-byte and 1 KB messages.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <sys/param.h>

#define TLK_IOC		('L' << 8)	/* sp1 driver-tlk/tlk.h */
#define TLK_RESET	(TLK_IOC | 1)
#define TLK_STATUS	(TLK_IOC | 3)
#define TLK_TIMEOUT	(TLK_IOC | 4)

static long ms(void)
{
	struct tms t;

	return (long)times(&t) * 1000 / HZ;
}

static int all(int fd, unsigned char *b, int n, int out)
{
	int done = 0;

	while (done < n) {
		int r = out ? write(fd, b + done, n - done) : read(fd, b + done, n - done);

		if (r <= 0)
			return done;
		done += r;
	}
	return done;
}

/* answer the C runtime's startup requests (iserver's SP protocol: a
 * 2-byte length, then the tag) until it goes quiet: the count served */
static int serve_startup(int fd)
{
	unsigned char rq[512], rp[64];
	int served = 0;

	for (;;) {
		int n, len, k, out;

		ioctl(fd, TLK_TIMEOUT, served ? 300 : 2000);
		if (all(fd, rq, 8, 0) != 8)
			return served;
		len = rq[0] | rq[1] << 8;
		if (len < 6 || len > (int)sizeof rq - 2)
			return -1;
		if (len > 6 && all(fd, rq + 8, len - 6, 0) != len - 6)
			return -1;
		printf("  request tag %d, %d bytes:", rq[2], len);
		for (k = 0; k < len + 2 && k < 24; k++)
			printf(" %02x", rq[k]);
		printf("\n");
		out = 2;
		if (rq[2] == 32) {		/* SP.GETENV */
			n = rq[3] | rq[4] << 8;
			if (n == 10 && memcmp(rq + 5, "IBOARDSIZE", 10) == 0) {
				rp[out++] = 0;
				rp[out++] = 7;
				rp[out++] = 0;
				memcpy(rp + out, "#100000", 7);
				out += 7;
			} else
				rp[out++] = 129;
		} else if (rq[2] == 40) {	/* SP.COMMAND: none */
			rp[out++] = 0;
			rp[out++] = 0;
			rp[out++] = 0;
		} else if (rq[2] == 35) {	/* SP.EXIT */
			rp[out++] = 0;
		} else
			rp[out++] = 1;		/* SP.UNIMPLEMENTED */
		rp[0] = (unsigned char)(out - 2);
		rp[1] = 0;
		ioctl(fd, TLK_TIMEOUT, 2000);
		if (all(fd, rp, out, 1) != out)
			return -1;
		served++;
	}
}

int main(int argc, char **argv)
{
	static unsigned char btl[65536], msg[1028], back[1028];
	const char *link = argc > 2 ? argv[2] : "/dev/link1";
	FILE *f;
	int fd, len, i, sizes[3] = { 0, 256, 1024 }, s;
	long t0;

	if (argc < 2 || (f = fopen(argv[1], "rb")) == NULL) {
		fprintf(stderr, "usage: tpboot BTL [LINK]\n");
		return 2;
	}
	len = (int)fread(btl, 1, sizeof btl, f);
	fclose(f);
	if ((fd = open(link, O_RDWR)) < 0) {
		perror(link);
		return 1;
	}
	t0 = ms();
	if (ioctl(fd, TLK_RESET, 0) < 0)
		perror("TLK_RESET");
	ioctl(fd, TLK_TIMEOUT, 3000);
	if (all(fd, btl, len, 1) != len) {
		fprintf(stderr, "boot: short write\n");
		return 1;
	}
	printf("booted %d bytes in %ld ms\n", len, ms() - t0);
	if (argc > 3 && strcmp(argv[3], "serve") == 0) {
		t0 = ms();
		printf("startup: %d request(s) served, %ld ms\n", serve_startup(fd),
			ms() - t0);
		ioctl(fd, TLK_TIMEOUT, 3000);
	}
	if (argc > 3 && strcmp(argv[3], "dump") == 0) {
		/* what the program says first, for two seconds */
		unsigned char b[256];
		int n = 0, k;

		ioctl(fd, TLK_TIMEOUT, 2000);
		while (n < (int)sizeof b && read(fd, b + n, 1) == 1)
			n++;
		printf("%d byte(s) after boot:", n);
		for (k = 0; k < n; k++)
			printf(" %02x", b[k]);
		printf("\n");
		return 0;
	}
	for (s = 0; s < 3; s++) {
		int n = sizes[s], bad = 0;

		t0 = ms();
		for (i = 0; i < 20; i++) {
			int j;

			msg[0] = (unsigned char)n;
			msg[1] = (unsigned char)(n >> 8);
			msg[2] = msg[3] = 0;
			for (j = 0; j < n; j++)
				msg[4 + j] = (unsigned char)(j * 7 + i);
			if (all(fd, msg, 4 + n, 1) != 4 + n || all(fd, back, 4 + n, 0) != 4 + n) {
				printf("echo %d: link timed out\n", n);
				return 1;
			}
			if (memcmp(back + 4, msg + 4, n) != 0 || back[0] != msg[0] || back[1] != msg[1])
				bad++;
		}
		printf("echo %4d bytes: %ld ms each (link %d, count %d)%s\n", n,
			(ms() - t0) / 20, back[2], back[3], bad ? ", WRONG DATA" : "");
	}
	return 0;
}
