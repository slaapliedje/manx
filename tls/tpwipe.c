/*
 * tpwipe.c - zero the ATW800/2's T425's memory (tpwipe.h).
 */
#include <stddef.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "tpwipe.h"

#define TLK_IOC		('L' << 8)	/* sp1 driver-tlk/tlk.h */
#define TLK_RESET	(TLK_IOC | 1)
#define TLK_TIMEOUT	(TLK_IOC | 4)

/* The program: a range comes in (start, end, little-endian), it is
 * zeroed, its words ORed and the OR sent back; then the next. */
/* tp/tpwipe.py: 104 bytes */
static const unsigned char wiper[] = {
	0xb8,                   /*   0  ajw 8 */
	0xd0,                   /*   1  stl 0 */
	0xd0,                   /*   2  stl 0 */
	0xd1,                   /*   3  stl 1 */
	0x71,                   /*   4  ldl 1 */
	0x60, 0x80,             /*   5  adc -16 */
	0xd2,                   /*   7  stl 2 */
	0x13,                   /*   8  ldlp 3 */
	0x71,                   /*   9  ldl 1 */
	0x48,                   /*  10  ldc 8 */
	0xf7,                   /*  11  in */
	0x73,                   /*  12  ldl 3 */
	0xd5,                   /*  13  stl 5 */
	0x74,                   /*  14  ldl 4 */
	0x75,                   /*  15  ldl 5 */
	0xf9,                   /*  16  gt */
	0x21, 0xae,             /*  17  cj check */
	0x40,                   /*  19  ldc 0 */
	0x75,                   /*  20  ldl 5 */
	0xe0,                   /*  21  stnl 0 */
	0x40,                   /*  22  ldc 0 */
	0x75,                   /*  23  ldl 5 */
	0xe1,                   /*  24  stnl 1 */
	0x40,                   /*  25  ldc 0 */
	0x75,                   /*  26  ldl 5 */
	0xe2,                   /*  27  stnl 2 */
	0x40,                   /*  28  ldc 0 */
	0x75,                   /*  29  ldl 5 */
	0xe3,                   /*  30  stnl 3 */
	0x40,                   /*  31  ldc 0 */
	0x75,                   /*  32  ldl 5 */
	0xe4,                   /*  33  stnl 4 */
	0x40,                   /*  34  ldc 0 */
	0x75,                   /*  35  ldl 5 */
	0xe5,                   /*  36  stnl 5 */
	0x40,                   /*  37  ldc 0 */
	0x75,                   /*  38  ldl 5 */
	0xe6,                   /*  39  stnl 6 */
	0x40,                   /*  40  ldc 0 */
	0x75,                   /*  41  ldl 5 */
	0xe7,                   /*  42  stnl 7 */
	0x75,                   /*  43  ldl 5 */
	0x22, 0x80,             /*  44  adc 32 */
	0xd5,                   /*  46  stl 5 */
	0x62, 0x0d,             /*  47  j zero */
	0x73,                   /*  49  ldl 3 */
	0xd5,                   /*  50  stl 5 */
	0x40,                   /*  51  ldc 0 */
	0xd6,                   /*  52  stl 6 */
	0x74,                   /*  53  ldl 4 */
	0x75,                   /*  54  ldl 5 */
	0xf9,                   /*  55  gt */
	0x22, 0xa8,             /*  56  cj send */
	0x76,                   /*  58  ldl 6 */
	0x75,                   /*  59  ldl 5 */
	0x30,                   /*  60  ldnl 0 */
	0x24, 0xfb,             /*  61  or */
	0x75,                   /*  63  ldl 5 */
	0x31,                   /*  64  ldnl 1 */
	0x24, 0xfb,             /*  65  or */
	0x75,                   /*  67  ldl 5 */
	0x32,                   /*  68  ldnl 2 */
	0x24, 0xfb,             /*  69  or */
	0x75,                   /*  71  ldl 5 */
	0x33,                   /*  72  ldnl 3 */
	0x24, 0xfb,             /*  73  or */
	0x75,                   /*  75  ldl 5 */
	0x34,                   /*  76  ldnl 4 */
	0x24, 0xfb,             /*  77  or */
	0x75,                   /*  79  ldl 5 */
	0x35,                   /*  80  ldnl 5 */
	0x24, 0xfb,             /*  81  or */
	0x75,                   /*  83  ldl 5 */
	0x36,                   /*  84  ldnl 6 */
	0x24, 0xfb,             /*  85  or */
	0x75,                   /*  87  ldl 5 */
	0x37,                   /*  88  ldnl 7 */
	0x24, 0xfb,             /*  89  or */
	0xd6,                   /*  91  stl 6 */
	0x75,                   /*  92  ldl 5 */
	0x22, 0x80,             /*  93  adc 32 */
	0xd5,                   /*  95  stl 5 */
	0x62, 0x03,             /*  96  j or_ */
	0x16,                   /*  98  ldlp 6 */
	0x72,                   /*  99  ldl 2 */
	0x44,                   /* 100  ldc 4 */
	0xfb,                   /* 101  out */
	0x65, 0x00,             /* 102  j next */
};

/* all n bytes in or out, each piece within the link's timeout: 0 or -1 */
static int xfer(int fd, unsigned char *b, size_t n, int out)
{
	while (n > 0) {
		int r = out ? (int)write(fd, (char *)b, n) : (int)read(fd, (char *)b, n);

		if (r <= 0)
			return -1;
		b += r;
		n -= (size_t)r;
	}
	return 0;
}

static void le32(unsigned char *b, unsigned long v)
{
	b[0] = (unsigned char)(v & 0xff);
	b[1] = (unsigned char)(v >> 8 & 0xff);
	b[2] = (unsigned char)(v >> 16 & 0xff);
	b[3] = (unsigned char)(v >> 24 & 0xff);
}

int tpwipe(int fd, const unsigned long (*ranges)[2], int n)
{
	unsigned char b[8];
	int i, bad = 0;

	if (ioctl(fd, TLK_RESET, 0) < 0)
		return -1;
	ioctl(fd, TLK_TIMEOUT, 2000);
	b[0] = sizeof wiper;
	if (xfer(fd, b, 1, 1) < 0 || xfer(fd, (unsigned char *)wiper, sizeof wiper, 1) < 0)
		bad = 1;
	for (i = 0; i < n && !bad; i++) {
		unsigned long s = ranges[i][0], e = ranges[i][1];

		if (s < TPW_CHIP_FREE || e <= s || (s | e) & 31) {
			bad = 1;
			break;
		}
		le32(b, s);
		le32(b + 4, e);
		ioctl(fd, TLK_TIMEOUT, 2000);
		if (xfer(fd, b, 8, 1) < 0) {
			bad = 1;
			break;
		}
		/* about 0.1 s a megabyte; 10 s a megabyte means it's gone */
		ioctl(fd, TLK_TIMEOUT, (int)(2000 + (e - s) / 100));
		if (xfer(fd, b, 4, 0) < 0 || (b[0] | b[1] | b[2] | b[3]))
			bad = 1;
	}
	ioctl(fd, TLK_RESET, 0);
	return bad ? -1 : 0;
}
