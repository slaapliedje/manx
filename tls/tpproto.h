/*
 * tpproto.h - the link protocol between Manx on the 68030 (tls/tpoff.c)
 * and tp/tpsig.c on the ATW800/2's T425, which does signature arithmetic
 * (tls/sigmath.c) for it. C89, for icc too.
 *
 * A request:   0xA5, type, seq, 0, length (2, low first), payload, crc (2)
 * The answer:  0x5A, status, seq, 0, length (2), payload, crc (2)
 * crc: CRC-16/CCITT (from 0xffff) over everything before it.
 *
 * The answer to a request is arithmetic, never a verdict: Manx checks
 * every signature itself with what comes back, so a garbled answer can
 * only make a good signature fail, not a bad one pass.
 */
#ifndef MANX_TPPROTO_H
#define MANX_TPPROTO_H

#include <stddef.h>

#define TP_RQ_MAGIC	0xa5
#define TP_RP_MAGIC	0x5a
#define TP_HDR		6
#define TP_MAX_PAYLOAD	1600	/* RSA-4096: 2 + 512 + 2 + e + 512 */

/* types; their payloads, and what comes back */
#define TP_PING		1	/* up to 512 bytes: the same */
#define TP_RSA		2	/* nlen (2), n, elen (2), e, x (n's length): x^e mod n */
#define TP_EC		3	/* curve (1), Q (1 + 2 len), u1 (len), u2 (len): x of u1 G + u2 Q */

/* status */
#define TP_DONE		0
#define TP_REFUSED	1	/* the arithmetic said no (x >= n, Q not on the curve, ...) */
#define TP_BAD		2	/* a bad request: crc, length, type */

/* CRC-16/CCITT of n bytes, from crc (0xffff to start) */
unsigned tp_crc(const unsigned char *b, size_t n, unsigned crc);

/* Run a request's payload of type type: the answer's payload into out
 * (*outlen bytes); the status. (tls/tpjob.c) */
int tpjob_run(int type, const unsigned char *in, size_t inlen,
	unsigned char *out, size_t *outlen);

#endif /* MANX_TPPROTO_H */
