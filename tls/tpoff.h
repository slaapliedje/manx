/*
 * tpoff.h - the ATW800/2's T425 as a second processor for signature
 * arithmetic (tp/tpsig.c, tls/tpproto.h). It is booted over /dev/link1
 * the first time it's wanted (the C runtime's startup requests answered
 * here, in iserver's place), then takes one job at a time: sent, and
 * collected later, so the 68030 works on another meanwhile. On any
 * trouble (no device, no program file, a timeout, a bad crc, a reply out
 * of step) it is reset and left alone for the rest of the run, and the
 * 68030 does the work, as it always does without the card (AMIX, a TT
 * without an ATW800/2).
 */
#ifndef MANX_TPOFF_H
#define MANX_TPOFF_H

#include <stddef.h>

/* Where: the link device and tp/tpsig.c's bootable file (NULL for the
 * defaults: /dev/link1, and tpsig.btl in the Manx directory). off: never. */
void tpoff_config(const char *dev, const char *btl, int off);

/* Booted and answering (booting it the first time it's asked)? */
int tpoff_up(void);

/* Send a job (TP_RSA, TP_EC): 0 sent, -1 not (down, or one is out). */
int tpoff_send(int type, const unsigned char *payload, size_t len);

/* The job's answer: its status (TP_DONE, TP_REFUSED), the payload into
 * out (*outlen bytes, out holding at least 520). wait 0: -2 if not there
 * yet. -1: the link failed (and the T425 is now down). */
int tpoff_recv(unsigned char *out, size_t *outlen, int wait);

/* What happened, for ufetch -v: "up (booted in 140 ms)", "no device", ... */
const char *tpoff_state(void);

/* Jobs it did, and the ms spent waiting for it after the 68030 was done. */
void tpoff_stats(unsigned long *jobs, unsigned long *waited_ms);

#endif /* MANX_TPOFF_H */
