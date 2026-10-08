/*
 * tpwipe.h - zero the ATW800/2's T425's memory. A reset clears nothing:
 * what Manx's program for it worked on (certificate arithmetic) or a
 * whole Helios session stays there, and anyone who can open the link can
 * read it back with the boot protocol's peeks. So a 104-byte program
 * (tp/tpwipe.py) is booted over the link, and it zeroes the ranges it is
 * sent at the T425's speed (1 MB in about 0.1 s).
 *
 * The T425's memory (measured on a real card): 4 KB on the chip from
 * 0x80000000, the wiper itself below TPW_CHIP_FREE; then the external
 * memory, 5 MB, whose first 2 MB are the card's memory past 2 MB - which
 * a desktop at 32 bits per pixel shows and draws in. The card's registers
 * sit at the top of that view: the 4 KB beneath them is left alone.
 */
#ifndef MANX_TPWIPE_H
#define MANX_TPWIPE_H

#define TPW_CHIP_FREE	0x80000120UL	/* past the wiper's code and workspace */
#define TPW_EXT		0x80001000UL	/* the external memory */
#define TPW_REGS	0x801ff000UL	/* under the card's registers (4 KB) */
#define TPW_SHARED_END	0x80200000UL	/* from here only the T425 sees it */
#define TPW_END		0x80500000UL	/* 5 MB, as Helios and rspy find */

/*
 * fd: the link (sp1's driver-tlk), open. The T425 is reset, given the
 * wiper, and each [start, end) zeroed (32-byte multiples), then reset
 * again. 0 when every range came back clean; -1 when the link failed or
 * a range didn't. No stdio or malloc: it may run in a signal handler.
 */
int tpwipe(int fd, const unsigned long (*ranges)[2], int n);

#endif /* MANX_TPWIPE_H */
