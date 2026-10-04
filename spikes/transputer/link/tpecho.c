/*
 * tpecho - a transputer program that talks over its links with no
 * iserver (the reduced library): it waits on all four link inputs, and
 * on whichever is spoken to echoes messages (a 4-byte header: length
 * low, high, then two bytes it returns as which link and a count).
 */
#include <stddef.h>
#include <channel.h>
#include <process.h>

static unsigned char buf[4096];

int main(void)
{
	Channel *in[4], *out[4];
	unsigned char hdr[4];
	int k, n, count = 0;

	in[0] = LINK0IN;
	in[1] = LINK1IN;
	in[2] = LINK2IN;
	in[3] = LINK3IN;
	out[0] = LINK0OUT;
	out[1] = LINK1OUT;
	out[2] = LINK2OUT;
	out[3] = LINK3OUT;
	k = ProcAlt(in[0], in[1], in[2], in[3], NULL);
	for (;;) {
		ChanIn(in[k], hdr, 4);
		n = hdr[0] | hdr[1] << 8;
		if (n > (int)sizeof buf)
			n = sizeof buf;
		if (n > 0)
			ChanIn(in[k], buf, n);
		hdr[2] = (unsigned char)k;
		hdr[3] = (unsigned char)++count;
		ChanOut(out[k], hdr, 4);
		if (n > 0)
			ChanOut(out[k], buf, n);
	}
	return 0;
}
