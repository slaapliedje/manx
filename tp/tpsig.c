/*
 * tpsig.c - signature arithmetic for Manx, on the ATW800/2's T425
 * (tls/tpproto.h). Booted by Manx over /dev/link1 (tls/tpoff.c), which
 * also answers the C runtime's few startup requests in iserver's place.
 * Then it waits on all four links and serves the one spoken to: a
 * request in, its arithmetic (tls/tpjob.c, tls/sigmath.c), the answer
 * out. Built on the TT by tp/tp-build.sh, with icc.
 */
#include <stddef.h>
#include <channel.h>
#include <process.h>
#include "tpproto.h"

static unsigned char rq[TP_HDR + TP_MAX_PAYLOAD + 2];
static unsigned char rp[TP_HDR + 520 + 2];

int main(void)
{
	Channel *in[4], *out[4];
	int k;

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
		size_t len, outlen = 0;
		unsigned crc;
		int status;

		ChanIn(in[k], rq, TP_HDR);
		len = (size_t)rq[4] | (size_t)rq[5] << 8;
		if (rq[0] != TP_RQ_MAGIC || len > TP_MAX_PAYLOAD)
			status = TP_BAD;	/* out of step: Manx resets us */
		else {
			ChanIn(in[k], rq + TP_HDR, (int)len + 2);
			crc = tp_crc(rq, TP_HDR + len, 0xffff);
			if (rq[TP_HDR + len] != (crc & 0xff) || rq[TP_HDR + len + 1] != crc >> 8)
				status = TP_BAD;
			else
				status = tpjob_run(rq[1], rq + TP_HDR, len, rp + TP_HDR, &outlen);
		}
		rp[0] = TP_RP_MAGIC;
		rp[1] = (unsigned char)status;
		rp[2] = rq[2];
		rp[3] = 0;
		rp[4] = (unsigned char)(outlen & 0xff);
		rp[5] = (unsigned char)(outlen >> 8);
		crc = tp_crc(rp, TP_HDR + outlen, 0xffff);
		rp[TP_HDR + outlen] = (unsigned char)(crc & 0xff);
		rp[TP_HDR + outlen + 1] = (unsigned char)(crc >> 8);
		ChanOut(out[k], rp, (int)(TP_HDR + outlen + 2));
	}
	return 0;
}
