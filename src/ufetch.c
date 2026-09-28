/*
 * ufetch - fetch a URL with the browser's network core and report how it
 * went: the command-line face of Phase 1, and its test driver.
 *
 *   ufetch [-v] [-I] [-o file] [-k ca.pem] [-m cap_kb] URL...
 *
 *   -v   headers, TLS details and progress on stderr
 *   -I   HEAD instead of GET
 *   -o   write the body to a file (default: count it only; "-" = stdout)
 *   -k   the PEM root bundle (default $UB_CAFILE, else the store as built)
 *   -m   memory cap in KB for everything ufetch allocates
 *   -E   no early requests: validate the server before sending anything
 *   -n   no connection reuse (each URL gets a new connection)
 * Several URLs are fetched in turn, reusing connections.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "os.h"
#include "entropy.h"
#include "tls.h"
#include "conn.h"
#include "fetch.h"

static int g_verbose;
static FILE *g_out;

static void on_status(void *ctx, const char *msg)
{
	(void)ctx;
	if (g_verbose)
		fprintf(stderr, "  [%lu ms] %s\n", os_msec(), msg);
}

static void on_header(void *ctx, const char *name, const char *value)
{
	(void)ctx;
	if (g_verbose)
		fprintf(stderr, "  < %s: %s\n", name, value);
}

static int on_body(void *ctx, const unsigned char *d, size_t n)
{
	(void)ctx;
	if (g_out && fwrite(d, 1, n, g_out) != n)
		return -1;
	return 0;
}

static const char *g_out_path;

/* a broken transfer is being retried: start the output over */
static void on_reset(void *ctx)
{
	(void)ctx;
	if (g_out && g_out != stdout)
		g_out = freopen(g_out_path, "wb", g_out);
	if (g_verbose)
		fprintf(stderr, "  (starting the transfer over)\n");
}

static void note(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
}

static void usage(void)
{
	fprintf(stderr, "usage: ufetch [-v] [-I] [-o file] [-k ca.pem] "
		"[-m cap_kb] [-E] [-n] URL...\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *out_path = NULL, *pem = getenv("UB_CAFILE"), *method = "GET";
	char seed_path[600];
	struct fetch_cb cb;
	static struct fetch_result res;
	int a, fails = 0;

	for (a = 1; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
		if (strcmp(argv[a], "-v") == 0)
			g_verbose = 1;
		else if (strcmp(argv[a], "-n") == 0)
			fetch_keep_alive = 0;
		else if (strcmp(argv[a], "-E") == 0)
			fetch_early_requests = 0;
		else if (strcmp(argv[a], "-I") == 0)
			method = "HEAD";
		else if (strcmp(argv[a], "-o") == 0 && a + 1 < argc)
			out_path = argv[++a];
		else if (strcmp(argv[a], "-k") == 0 && a + 1 < argc)
			pem = argv[++a];
		else if (strcmp(argv[a], "-m") == 0 && a + 1 < argc)
			mem_set_cap((size_t)atol(argv[++a]) * 1024);
		else
			usage();
	}
	if (a >= argc)
		usage();
	signal(SIGPIPE, SIG_IGN);

	entropy_init(os_datapath(seed_path, sizeof seed_path, "seed"));
	if (g_verbose) {
		const struct entropy_report *er = entropy_report();

		fprintf(stderr, "entropy: %d bits (urandom %d, seed file %d, "
			"%d distinct jitter deltas)\n", entropy_bits(), er->urandom,
			er->seedfile, er->jitter_distinct);
	}
	{
		unsigned long t0 = os_msec();
		int rc = tls_init(pem, note);

		if (g_verbose)
			fprintf(stderr, "trust store: %s, %lu ms\n",
				rc == 0 ? "loaded" : "EMPTY (https won't work)",
				os_msec() - t0);
	}

	g_out_path = out_path;
	if (out_path)
		g_out = strcmp(out_path, "-") == 0 ? stdout : fopen(out_path, "wb");
	if (out_path && g_out == NULL) {
		perror(out_path);
		return 1;
	}
	memset(&cb, 0, sizeof cb);
	cb.status = on_status;
	cb.header = on_header;
	cb.body = on_body;
	if (!out_path || strcmp(out_path, "-") != 0)
		cb.reset = on_reset;	/* stdout can't be taken back */

	for (; a < argc; a++) {
		unsigned long t0 = os_msec();

		if (fetch(argv[a], method, &cb, &res) < 0) {
			fprintf(stderr, "ufetch: %s: %s\n", argv[a], res.error);
			fails++;
			continue;
		}
		fprintf(stderr, "%d %s %s%s%s, %ld B, %lu ms total", res.status,
			res.url, res.content_type, res.charset[0] ? "; " : "",
			res.charset, res.body_bytes, os_msec() - t0);
		if (res.redirects)
			fprintf(stderr, ", %d redirect(s)", res.redirects);
		fprintf(stderr, "\n");
		if (res.reused)
			fprintf(stderr, "  connection reused\n");
		else {
			fprintf(stderr, "  dns %lu ms, connect %lu ms", res.t_dns, res.t_connect);
			if (res.tls)
				fprintf(stderr, ", TLS %s%s%s handshake %lu ms, validation %lu ms"
					"%s, suite 0x%04x",
					tls_profile_name(res.tls_profile),
					res.tls_resumed ? " resumed" : "",
					res.tls_leaf_memo ? " known-leaf" : "",
					res.t_handshake, res.t_verify,
					res.tls_learned ? " (learned intermediates)" : "",
					res.tls_suite);
			if (res.reconnected)
				fprintf(stderr, ", reconnected after validating");
			fprintf(stderr, "\n");
		}
		fprintf(stderr, "  first byte %lu ms, body %lu ms", res.t_first, res.t_body);
		if (res.t_body)
			fprintf(stderr, " (%lu KB/s)", (unsigned long)res.body_bytes / 1024
				* 1000 / res.t_body);
		fprintf(stderr, ", memory peak %lu KB\n", (unsigned long)mem_peak() / 1024);
	}
	conn_close_all();
	entropy_save();
	if (g_out && g_out != stdout)
		fclose(g_out);
	return fails ? 1 : 0;
}
