/*
 * tpoff.c - the ATW800/2's T425 as a second processor (tpoff.h).
 *
 * The link is sp1's driver-tlk: read() and write() move bytes, ioctl()
 * resets the transputer, sets how long a read or write may wait, and
 * says whether a byte is waiting. Booting is iserver's: reset, then the
 * bootable file (icollect -t) as it is. Its C runtime then asks the host
 * for IBOARDSIZE, its id and its command line, in iserver's protocol (a
 * 2-byte length, the tag, the rest): answered here, then tpsig's own
 * protocol (tpproto.h) takes over.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "os.h"
#include "config.h"
#include "tpproto.h"
#include "tpoff.h"

#define TLK_IOC		('L' << 8)	/* sp1 driver-tlk/tlk.h */
#define TLK_RESET	(TLK_IOC | 1)
#define TLK_STATUS	(TLK_IOC | 3)
#define TLK_TIMEOUT	(TLK_IOC | 4)
#define TLK_ST_IN	1

enum { T_UNTRIED, T_UP, T_DOWN, T_OFF };

static int g_state = T_UNTRIED, g_configured, g_sim;
static int g_fd = -1;
static int g_out;			/* a job is out */
static unsigned char g_seq;
static char g_dev[256] = "/dev/link1", g_btl[512], g_why[96];
static unsigned long g_jobs, g_waited;

void tpoff_config(const char *dev, const char *btl, int off)
{
	g_configured = 1;
	if (dev)
		snprintf(g_dev, sizeof g_dev, "%s", dev);
	if (btl)
		snprintf(g_btl, sizeof g_btl, "%s", btl);
	if (off)
		g_state = T_OFF;
}

static int down(const char *why)
{
	if (g_fd >= 0) {
		ioctl(g_fd, TLK_RESET, 0);	/* stop whatever it was doing */
		close(g_fd);
		g_fd = -1;
	}
	g_state = T_DOWN;
	g_out = 0;
	snprintf(g_why, sizeof g_why, "%s", why);
	return -1;
}

/* move all n bytes, waiting at most ms for each piece */
static int move(unsigned char *b, size_t n, int out, int ms)
{
	size_t done = 0;

	ioctl(g_fd, TLK_TIMEOUT, ms);
	while (done < n) {
		int r = out ? (int)write(g_fd, b + done, n - done)
			: (int)read(g_fd, b + done, n - done);

		if (r <= 0)
			return -1;
		done += (size_t)r;
	}
	return 0;
}

/* the C runtime's startup requests, until it goes quiet: 0, or -1 */
static int serve_startup(void)
{
	unsigned char rq[64], rp[16];
	int served = 0;

	for (;;) {
		size_t len, out = 2;

		if (move(rq, 8, 0, served ? 300 : 2000) < 0)
			return served ? 0 : -1;		/* quiet: started */
		len = (size_t)rq[0] | (size_t)rq[1] << 8;
		if (len < 6 || len > sizeof rq - 2
			|| (len > 6 && move(rq + 8, len - 6, 0, 1000) < 0))
			return -1;
		if (rq[2] == 32) {			/* SP.GETENV */
			size_t n = (size_t)rq[3] | (size_t)rq[4] << 8;

			if (n == 10 && memcmp(rq + 5, "IBOARDSIZE", 10) == 0) {
				rp[out++] = 0;
				rp[out++] = 7;
				rp[out++] = 0;
				memcpy(rp + out, "#100000", 7);	/* the T425: 1 MB */
				out += 7;
			} else
				rp[out++] = 129;	/* SP.ERROR */
		} else if (rq[2] == 40) {		/* SP.COMMAND: none */
			rp[out++] = 0;
			rp[out++] = 0;
			rp[out++] = 0;
		} else
			rp[out++] = 1;			/* SP.UNIMPLEMENTED */
		rp[0] = (unsigned char)(out - 2);
		rp[1] = 0;
		if (move(rp, out, 1, 1000) < 0)
			return -1;
		if (++served > 16)
			return -1;			/* not the program we know */
	}
}

/* the settings: transputer = off, on, a device, or sim (the T425's work
 * done in-process, for the tests); transputer_program = its file */
static void from_config(void)
{
	const char *t = config_str("transputer", "on");

	g_configured = 1;
	if (strcmp(t, "off") == 0 || strcmp(t, "no") == 0 || strcmp(t, "0") == 0)
		g_state = T_OFF;
	else if (strcmp(t, "on") != 0 && strcmp(t, "yes") != 0 && strcmp(t, "1") != 0)
		snprintf(g_dev, sizeof g_dev, "%s", t);
	tpoff_config(NULL, config_str("transputer_program", NULL), 0);
}

static int boot(void)
{
	unsigned char *btl, ping[16], back[600];
	size_t len, n;
	unsigned long t0 = os_msec();
	char path[512];
	int st, i;

	if (strcmp(g_dev, "sim") == 0) {
		g_sim = 1;
		g_state = T_UP;
		snprintf(g_why, sizeof g_why, "simulated");
		return 0;
	}

	if (!g_btl[0] && os_datapath(path, sizeof path, "tpsig.btl"))
		snprintf(g_btl, sizeof g_btl, "%s", path);
	if ((g_fd = open(g_dev, O_RDWR)) < 0)
		return down("no device");
	if ((btl = os_read_file(g_btl, &len)) == NULL)
		return down("no tpsig.btl");
	if (ioctl(g_fd, TLK_RESET, 0) < 0) {
		xfree(btl);
		return down("can't reset it");
	}
	i = move(btl, len, 1, 3000);
	xfree(btl);
	if (i < 0)
		return down("boot timed out");
	if (serve_startup() < 0)
		return down("its startup went wrong");
	/* and a ping, to be sure it is tpsig */
	for (i = 0; i < 16; i++)
		ping[i] = (unsigned char)(i * 37 + 11);
	g_state = T_UP;
	if (tpoff_send(TP_PING, ping, sizeof ping) < 0)
		return -1;
	st = tpoff_recv(back, &n, 1);
	g_jobs = 0;
	if (st < 0)
		return -1;			/* down already, saying why */
	if (st != TP_DONE || n != sizeof ping || memcmp(back, ping, n) != 0)
		return down("a ping came back wrong");
	snprintf(g_why, sizeof g_why, "up (booted in %lu ms)", os_msec() - t0);
	return 0;
}

int tpoff_up(void)
{
	if (!g_configured)
		from_config();
	if (g_state == T_UNTRIED)
		boot();
	return g_state == T_UP;
}

/* sim: the answer, worked out at the send */
static unsigned char g_simout[520];
static size_t g_simlen;
static int g_simst;

int tpoff_send(int type, const unsigned char *payload, size_t len)
{
	unsigned char hdr[TP_HDR], crc2[2];
	unsigned crc;

	if (g_state != T_UP || g_out || len > TP_MAX_PAYLOAD)
		return -1;
	if (g_sim) {
		g_simst = tpjob_run(type, payload, len, g_simout, &g_simlen);
		g_out = 1;
		return 0;
	}
	hdr[0] = TP_RQ_MAGIC;
	hdr[1] = (unsigned char)type;
	hdr[2] = ++g_seq;
	hdr[3] = 0;
	hdr[4] = (unsigned char)(len & 0xff);
	hdr[5] = (unsigned char)(len >> 8);
	crc = tp_crc(payload, len, tp_crc(hdr, TP_HDR, 0xffff));
	crc2[0] = (unsigned char)(crc & 0xff);
	crc2[1] = (unsigned char)(crc >> 8);
	if (move(hdr, TP_HDR, 1, 2000) < 0 || move((unsigned char *)payload, len, 1, 2000) < 0
		|| move(crc2, 2, 1, 2000) < 0)
		return down("a send timed out");
	g_out = 1;
	return 0;
}

int tpoff_recv(unsigned char *out, size_t *outlen, int wait)
{
	unsigned char hdr[TP_HDR], crc2[2];
	unsigned long t0 = os_msec();
	size_t len;
	unsigned crc;

	*outlen = 0;
	if (g_state != T_UP || !g_out)
		return -1;
	if (g_sim) {
		g_out = 0;
		g_jobs++;
		memcpy(out, g_simout, g_simlen);
		*outlen = g_simlen;
		return g_simst;
	}
	if (!wait) {
		int st = ioctl(g_fd, TLK_STATUS, 0);

		if (st >= 0 && !(st & TLK_ST_IN))
			return -2;
	}
	/* RSA-4096 takes it ~1 s, P-384 a few: 30 s means it's gone */
	if (move(hdr, 1, 0, 30000) < 0)
		return down("no answer");
	if (wait)
		g_waited += os_msec() - t0;
	if (move(hdr + 1, TP_HDR - 1, 0, 1000) < 0)
		return down("an answer cut short");
	len = (size_t)hdr[4] | (size_t)hdr[5] << 8;
	if (hdr[0] != TP_RP_MAGIC || hdr[2] != g_seq || len > 520)
		return down("an answer out of step");
	if (move(out, len, 0, 1000) < 0 || move(crc2, 2, 0, 1000) < 0)
		return down("an answer cut short");
	crc = tp_crc(out, len, tp_crc(hdr, TP_HDR, 0xffff));
	if (crc2[0] != (crc & 0xff) || crc2[1] != crc >> 8)
		return down("an answer's crc was wrong");
	if (hdr[1] == TP_BAD)
		return down("it didn't take a request");
	g_out = 0;
	g_jobs++;
	*outlen = len;
	return hdr[1];
}

const char *tpoff_state(void)
{
	switch (g_state) {
	case T_OFF:
		return "off";
	case T_UNTRIED:
		return "not used yet";
	}
	return g_why;
}

void tpoff_stats(unsigned long *jobs, unsigned long *waited_ms)
{
	*jobs = g_jobs;
	*waited_ms = g_waited;
}
