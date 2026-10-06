/*
 * manxtrust - manage the browser's trust material and random seed.
 *
 *   manxtrust status            what is in the data directory
 *   manxtrust roots FILE.pem    (re)build the root store from a PEM bundle
 *   manxtrust learn FILE.pem    verify the intermediates in a PEM bundle and
 *                             keep the good ones (preloading: a first visit
 *                             to a site then checks one signature, not two)
 *   manxtrust seed              seed the random pool from keystroke timing
 *   manxtrust seed -            seed it from 64 hex digits on stdin, made
 *                             on a machine with a real random source, e.g.
 *                             head -c 32 /dev/urandom | od -An -tx1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <termios.h>
#if defined(ICANNON) && !defined(ICANON)
#define ICANON ICANNON		/* (Helios's termios.h spells it so) */
#endif
#include "os.h"
#include "entropy.h"
#include "anchors.h"
#include "tls.h"

#ifdef MANX_SYSV4
int isatty(int);
int read(int, void *, unsigned);
#else
#include <unistd.h>
#endif

static void note(const char *msg)
{
	printf("%s\n", msg);
	fflush(stdout);
}

static int count_lines(const char *name)
{
	char path[600], line[700];
	FILE *f;
	int n = 0;

	if (os_datapath(path, sizeof path, name) == NULL || (f = fopen(path, "r")) == NULL)
		return 0;
	while (fgets(line, sizeof line, f))
		n++;
	fclose(f);
	return n;
}

static int status(void)
{
	char path[600], seed[600];
	struct anchors a;
	const char *dir = os_datadir();

	printf("data directory: %s\n", dir ? dir : "(none: set HOME or MANX_HOME)");
	if (dir == NULL)
		return 1;
	anchors_init(&a);
	if (anchors_load(&a, os_datapath(path, sizeof path, "roots.bin"), NULL, NULL) == 0)
		printf("roots:          %lu\n", (unsigned long)a.n);
	else
		printf("roots:          none (manxtrust roots FILE.pem)\n");
	anchors_free(&a);
	anchors_init(&a);
	anchors_load(&a, os_datapath(path, sizeof path, "inter.bin"), NULL, NULL);
	printf("intermediates:  %lu learned\n", (unsigned long)a.n);
	anchors_free(&a);
	printf("known leaves:   %d\n", count_lines("leaves"));
	printf("sessions:       %d\n", count_lines("sessions"));
	printf("full-profile hosts: %d\n", count_lines("hosts"));
	entropy_init(os_datapath(seed, sizeof seed, "seed"));
	printf("random pool:    %d bits%s\n", entropy_bits(),
		entropy_ready() ? "" : " - NOT READY (manxtrust seed)");
	return 0;
}

static int seed_hex(void)
{
	char line[256];
	unsigned char b[32];
	int n = 0, hi = -1;
	char *p;

	if (!fgets(line, sizeof line, stdin)) {
		fprintf(stderr, "manxtrust: no input\n");
		return 1;
	}
	for (p = line; *p && n < 32; p++) {
		int v;

		if (isdigit((unsigned char)*p)) v = *p - '0';
		else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
		else continue;
		if (hi < 0) hi = v;
		else { b[n++] = (unsigned char)(hi << 4 | v); hi = -1; }
	}
	if (n < 32) {
		fprintf(stderr, "manxtrust: need 64 hex digits, got %d\n", 2 * n);
		return 1;
	}
	entropy_add(b, sizeof b, 256);
	memset(b, 0, sizeof b);
	return 0;
}

static int seed_keys(void)
{
	struct termios old, raw;
	int keys = 0;

	if (!isatty(0)) {
		fprintf(stderr, "manxtrust: seeding needs a terminal (or: manxtrust seed -)\n");
		return 1;
	}
	printf("Type random keys, unevenly, until the bar is full.\n");
	tcgetattr(0, &old);
	raw = old;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);
	while (!entropy_ready()) {
		unsigned char c;
		unsigned long t = os_usec();
		int bar, i;

		if (read(0, (char *)&c, 1) != 1)
			break;
		t = os_usec() - t;
		/* the key and the gap before it; two bits each, conservatively
		 * (the clock may be coarse) */
		entropy_add(&c, 1, 0);
		entropy_add(&t, sizeof t, 2);
		keys++;
		bar = entropy_bits() * 40 / ENTROPY_NEEDED;
		printf("\r[");
		for (i = 0; i < 40; i++)
			putchar(i < bar ? '#' : '.');
		printf("] %d", keys);
		fflush(stdout);
	}
	tcsetattr(0, TCSANOW, &old);
	printf("\n");
	return entropy_ready() ? 0 : 1;
}

int main(int argc, char **argv)
{
	char path[600];

	if (argc < 2) {
		fprintf(stderr, "usage: manxtrust status | roots FILE.pem | learn FILE.pem"
			" | seed [-]\n");
		return 2;
	}
	if (strcmp(argv[1], "status") == 0)
		return status();
	if (strcmp(argv[1], "roots") == 0 && argc == 3) {
		unsigned long t0 = os_msec();
		int n = tls_build_roots(argv[2]);

		if (n < 0) {
			fprintf(stderr, "manxtrust: can't build roots from %s\n", argv[2]);
			return 1;
		}
		printf("%d roots stored (%lu s)\n", n, (os_msec() - t0 + 500) / 1000);
		return 0;
	}
	if (strcmp(argv[1], "learn") == 0 && argc == 3) {
		int n;

		if (tls_init(NULL, note) < 0) {
			fprintf(stderr, "manxtrust: no roots yet (manxtrust roots FILE.pem)\n");
			return 1;
		}
		n = tls_learn_pem(argv[2], note);
		if (n < 0) {
			fprintf(stderr, "manxtrust: can't read %s\n", argv[2]);
			return 1;
		}
		printf("%d intermediate(s) learned\n", n);
		return 0;
	}
	if (strcmp(argv[1], "seed") == 0) {
		int rc;

		if (os_datapath(path, sizeof path, "seed") == NULL) {
			fprintf(stderr, "manxtrust: no data directory\n");
			return 1;
		}
		entropy_init(path);
		rc = argc > 2 && strcmp(argv[2], "-") == 0 ? seed_hex() : seed_keys();
		if (rc == 0 && entropy_ready()) {
			entropy_save();
			printf("seeded: %d bits\n", entropy_bits());
		}
		return rc;
	}
	fprintf(stderr, "manxtrust: unknown command %s\n", argv[1]);
	return 2;
}
