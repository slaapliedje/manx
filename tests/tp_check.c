/*
 * tp_check - on the TT: boot tp/tpsig.c on the ATW800/2's T425 through
 * tls/tpoff.c and check it against the 68030. RSA-2048/4096 and
 * P-256/P-384 jobs done on both, the answers compared and each side
 * timed; one that must be refused (the point at infinity); then two jobs
 * at once, one on each processor.
 *
 *   tp_check [BTL [DEV]]
 */
#include <stdio.h>
#include <string.h>
#include "os.h"
#include "sigmath.h"
#include "tpproto.h"
#include "tpoff.h"

static int fails, runs;
static unsigned g_rng = 777u;

static unsigned rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

static void fill(unsigned char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		b[i] = (unsigned char)rnd();
}

/* the generators, as Q: u1 G + u2 G */
static const unsigned char g256[65] = { 0x04,
	0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5,
	0x63, 0xa4, 0x40, 0xf2, 0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0,
	0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
	0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a,
	0x7c, 0x0f, 0x9e, 0x16, 0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce,
	0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5 };
static const unsigned char g384[97] = { 0x04,
	0xaa, 0x87, 0xca, 0x22, 0xbe, 0x8b, 0x05, 0x37, 0x8e, 0xb1, 0xc7, 0x1e,
	0xf3, 0x20, 0xad, 0x74, 0x6e, 0x1d, 0x3b, 0x62, 0x8b, 0xa7, 0x9b, 0x98,
	0x59, 0xf7, 0x41, 0xe0, 0x82, 0x54, 0x2a, 0x38, 0x55, 0x02, 0xf2, 0x5d,
	0xbf, 0x55, 0x29, 0x6c, 0x3a, 0x54, 0x5e, 0x38, 0x72, 0x76, 0x0a, 0xb7,
	0x36, 0x17, 0xde, 0x4a, 0x96, 0x26, 0x2c, 0x6f, 0x5d, 0x9e, 0x98, 0xbf,
	0x92, 0x92, 0xdc, 0x29, 0xf8, 0xf4, 0x1d, 0xbd, 0x28, 0x9a, 0x14, 0x7c,
	0xe9, 0xda, 0x31, 0x13, 0xb5, 0xf0, 0xb8, 0xc0, 0x0a, 0x60, 0xb1, 0xce,
	0x1d, 0x7e, 0x81, 0x9d, 0x7a, 0x43, 0x1d, 0x7c, 0x90, 0xea, 0x0e, 0x5f };

struct job {
	char name[32];
	int type;
	unsigned char in[TP_MAX_PAYLOAD];
	size_t inlen;
};

static void rsa_job(struct job *j, size_t bytes)
{
	size_t p = 0;

	sprintf(j->name, "RSA-%lu", (unsigned long)bytes * 8);
	j->type = TP_RSA;
	j->in[p++] = (unsigned char)(bytes & 0xff);
	j->in[p++] = (unsigned char)(bytes >> 8);
	fill(j->in + p, bytes);
	j->in[p] |= 0x80;
	j->in[p + bytes - 1] |= 1;
	p += bytes;
	j->in[p++] = 3;
	j->in[p++] = 0;
	j->in[p++] = 1;
	j->in[p++] = 0;
	j->in[p++] = 1;
	fill(j->in + p, bytes);
	j->in[p] &= 0x7f;
	p += bytes;
	j->inlen = p;
}

static void ec_job(struct job *j, int curve, int infinity)
{
	size_t len = curve == SIG_P256 ? 32 : 48, p = 0, i;

	sprintf(j->name, "%s%s", curve == SIG_P256 ? "P-256" : "P-384",
		infinity ? " (infinity)" : "");
	j->type = TP_EC;
	j->in[p++] = (unsigned char)curve;
	memcpy(j->in + p, curve == SIG_P256 ? g256 : g384, 1 + 2 * len);
	p += 1 + 2 * len;
	fill(j->in + p, 2 * len);
	j->in[p] &= 0x7f;
	j->in[p + len] &= 0x7f;
	if (infinity) {
		/* u2 = n - u1: u1 G + u2 G = n G, the point at infinity */
		static const unsigned char n256[32] = {
			0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
			0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51 };
		int bw = 0;

		for (i = len; i-- > 0;) {
			int d = n256[i] - j->in[p + i] - bw;

			bw = d < 0;
			j->in[p + len + i] = (unsigned char)(d & 0xff);
		}
	}
	p += 2 * len;
	j->inlen = p;
}

/* on the 68030 */
static int local(const struct job *j, unsigned char *out, size_t *outlen)
{
	return tpjob_run(j->type, j->in, j->inlen, out, outlen);
}

static void one(struct job *j)
{
	unsigned char a[600], b[600];
	size_t alen, blen;
	unsigned long t0, t68, t425;
	int sa, sb;

	t0 = os_msec();
	sa = local(j, a, &alen);
	t68 = os_msec() - t0;
	t0 = os_msec();
	if (tpoff_send(j->type, j->in, j->inlen) < 0)
		sb = -1;
	else
		sb = tpoff_recv(b, &blen, 1);
	t425 = os_msec() - t0;
	runs++;
	if (sa != sb || (sa == TP_DONE && (alen != blen || memcmp(a, b, alen) != 0))) {
		fails++;
		printf("FAIL %s: 68030 %d, T425 %d (%s)\n", j->name, sa, sb, tpoff_state());
		return;
	}
	printf("  %-18s %s  68030 %6lu ms   T425 %6lu ms\n", j->name,
		sa == TP_DONE ? "same" : "refused by both", t68, t425);
}

int main(int argc, char **argv)
{
	static struct job jobs[8];
	unsigned long t0, jobsdone, waited;
	int i;

	tpoff_config(argc > 2 ? argv[2] : NULL, argc > 1 ? argv[1] : NULL, 0);
	t0 = os_msec();
	if (!tpoff_up()) {
		printf("T425: %s\n", tpoff_state());
		return 1;
	}
	printf("T425: %s (%lu ms)\n", tpoff_state(), os_msec() - t0);
	rsa_job(&jobs[0], 256);
	rsa_job(&jobs[1], 512);
	ec_job(&jobs[2], SIG_P256, 0);
	ec_job(&jobs[3], SIG_P384, 0);
	ec_job(&jobs[4], SIG_P256, 1);
	rsa_job(&jobs[5], 253);		/* not a multiple of 4 limbs */
	for (i = 0; i < 6; i++)
		one(&jobs[i]);

	/* two P-384 jobs: one after the other on the 68030, then one on each */
	{
		unsigned char a[600], b[600];
		size_t alen, blen;
		unsigned long tseq, tpar;

		ec_job(&jobs[6], SIG_P384, 0);
		t0 = os_msec();
		local(&jobs[3], a, &alen);
		local(&jobs[6], b, &blen);
		tseq = os_msec() - t0;
		t0 = os_msec();
		tpoff_send(jobs[3].type, jobs[3].in, jobs[3].inlen);
		local(&jobs[6], b, &blen);
		tpoff_recv(a, &alen, 1);
		tpar = os_msec() - t0;
		printf("  two P-384 jobs: 68030 alone %lu ms, 68030 + T425 %lu ms\n", tseq, tpar);
	}
	tpoff_stats(&jobsdone, &waited);
	printf("T425: %lu job(s), %lu ms waited for; %d/%d checks passed\n", jobsdone,
		waited, runs - fails, runs);
	return fails ? 1 : 0;
}
