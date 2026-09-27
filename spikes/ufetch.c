/*
 * ufetch - Phase 0 spike: fetch one URL over HTTP or HTTPS and report where
 * the time went.
 *
 *   ufetch [-v] [-o file] [-k ca.pem] [-c chacha|gcm|rsa|any] [-m 15|31] [-d] URL
 *
 *   -v   print the response headers and TLS details on stderr
 *   -o   write the body to a file (default: count it only)
 *   -k   PEM trust bundle, may be repeated (default $UB_CAFILE, else ./ca.pem)
 *   -c   cipher suites offered: ChaCha20-Poly1305 only, AES-GCM only,
 *        ECDHE-RSA only, or all with ChaCha20 first (default)
 *   -m   big-integer code for the handshake: i15/m15 or i31/m31 (default 31)
 *   -d   deferred validation: finish the handshake first, then check the
 *        certificate chain and the server's signature before sending
 *        anything (tls/xdefer.h)
 *
 * The summary line goes to stderr:
 *   dns 120 ms, connect 80 ms, trust 900 ms (143 anchors),
 *   handshake 6100 ms, first byte 400 ms, body 51234 B in 2100 ms (24 KB/s)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "bearssl.h"
#include "os.h"
#include "dns.h"
#include "tcp.h"
#include "trust.h"
#include "entropy.h"
#include "xdefer.h"

#define READ_TIMEOUT_MS	30000

static int g_verbose;

struct url {
	int tls;
	char host[256];
	unsigned port;
	char path[1024];
};

static int parse_url(const char *s, struct url *u)
{
	const char *h, *e, *colon;
	size_t hl;

	if (strncmp(s, "https://", 8) == 0) {
		u->tls = 1;
		u->port = 443;
		h = s + 8;
	} else if (strncmp(s, "http://", 7) == 0) {
		u->tls = 0;
		u->port = 80;
		h = s + 7;
	} else
		return -1;
	e = h + strcspn(h, "/?#");
	colon = memchr(h, ':', (size_t)(e - h));
	hl = (size_t)((colon ? colon : e) - h);
	if (hl == 0 || hl >= sizeof u->host)
		return -1;
	memcpy(u->host, h, hl);
	u->host[hl] = '\0';
	if (colon)
		u->port = (unsigned)strtoul(colon + 1, 0, 10);
	if (*e == '/' || *e == '?') {
		if (*e == '?')
			snprintf(u->path, sizeof u->path, "/%s", e);
		else
			snprintf(u->path, sizeof u->path, "%s", e);
		u->path[strcspn(u->path, "#")] = '\0';
	} else
		strcpy(u->path, "/");
	return 0;
}

/* --- BearSSL low-level I/O ----------------------------------------------- */

/* -v traces the handshake's traffic with timestamps: the gaps between a
 * read and the next write are the 68030 computing */
static unsigned long g_t_start;
static int g_trace;

static int low_read(void *ctx, unsigned char *buf, size_t len)
{
	int n = tcp_read(*(int *)ctx, buf, len, READ_TIMEOUT_MS);

	if (g_trace)
		fprintf(stderr, "  %6lu ms  read %d\n", os_msec() - g_t_start, n);
	return n > 0 ? n : -1;
}

static int low_write(void *ctx, const unsigned char *buf, size_t len)
{
	int rc = tcp_write_all(*(int *)ctx, buf, len);

	if (g_trace)
		fprintf(stderr, "  %6lu ms  write %lu%s\n", os_msec() - g_t_start,
			(unsigned long)len, rc == 0 ? "" : " FAILED");
	return rc == 0 ? (int)len : -1;
}

static const char *suite_name(unsigned s)
{
	switch (s) {
	case BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256: return "ECDHE-ECDSA-CHACHA20-POLY1305";
	case BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256: return "ECDHE-RSA-CHACHA20-POLY1305";
	case BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256: return "ECDHE-ECDSA-AES128-GCM-SHA256";
	case BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256: return "ECDHE-RSA-AES128-GCM-SHA256";
	case BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384: return "ECDHE-ECDSA-AES256-GCM-SHA384";
	case BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384: return "ECDHE-RSA-AES256-GCM-SHA384";
	case BR_TLS_RSA_WITH_AES_128_GCM_SHA256: return "RSA-AES128-GCM-SHA256";
	}
	return "other";
}

static const uint16_t suites_chacha[] = {
	BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
};
static const uint16_t suites_gcm[] = {
	BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
};
/* RSA certificates only: a server holding both kinds then sends its RSA
 * chain, whose signatures are far cheaper to check on a 68030 than ECDSA
 * P-384 (see tlsbench) */
static const uint16_t suites_rsa[] = {
	BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
};
static const uint16_t suites_any[] = {
	BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
};

/* --- the fetch ------------------------------------------------------------ */

struct conn {
	int fd;
	br_ssl_client_context sc;
	br_x509_minimal_context xc;
	br_sslio_context io;
	int tls;
};

static unsigned char g_iobuf[BR_SSL_BUFSIZE_BIDI];

static int conn_write(struct conn *c, const void *p, size_t n)
{
	if (!c->tls)
		return tcp_write_all(c->fd, p, n);
	if (br_sslio_write_all(&c->io, p, n) < 0)
		return -1;
	return br_sslio_flush(&c->io);
}

static int conn_read(struct conn *c, void *p, size_t n)
{
	if (!c->tls)
		return tcp_read(c->fd, p, n, READ_TIMEOUT_MS);
	n = (size_t)br_sslio_read(&c->io, p, n);
	return (int)n;
}

static void usage(void)
{
	fprintf(stderr, "usage: ufetch [-v] [-o file] [-k ca.pem] "
		"[-c chacha|gcm|rsa|any] [-m 15|31] [-d] URL\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *out_path = NULL, *ca_list[8];
	int nca = 0, k;
	const char *suites = "any";
	int bits = 31, defer = 0;
	static struct xdefer xd;
	unsigned long t_verify = 0;
	struct url u;
	static struct conn c;
	struct trust_store ts;
	unsigned char ip[4], seed[32];
	struct entropy_report er;
	unsigned long t0, t_dns, t_conn, t_trust = 0, t_hs = 0, t_first = 0, t_body;
	unsigned long body = 0, total = 0;
	int a, rc, header_done = 0;
	char req[1400];
	static char buf[4096];
	char hdr[2048];
	size_t hlen = 0;
	FILE *out = NULL;

	for (a = 1; a < argc && argv[a][0] == '-'; a++) {
		if (strcmp(argv[a], "-v") == 0)
			g_verbose = 1;
		else if (strcmp(argv[a], "-o") == 0 && a + 1 < argc)
			out_path = argv[++a];
		else if (strcmp(argv[a], "-k") == 0 && a + 1 < argc && nca < 8)
			ca_list[nca++] = argv[++a];
		else if (strcmp(argv[a], "-c") == 0 && a + 1 < argc)
			suites = argv[++a];
		else if (strcmp(argv[a], "-d") == 0)
			defer = 1;
		else if (strcmp(argv[a], "-m") == 0 && a + 1 < argc)
			bits = atoi(argv[++a]);
		else
			usage();
	}
	if (a != argc - 1 || parse_url(argv[a], &u) < 0)
		usage();
	if (nca == 0) {
		ca_list[0] = getenv("UB_CAFILE") ? getenv("UB_CAFILE") : "ca.pem";
		nca = 1;
	}
	/* a server that gives up on us mid-handshake must not kill us */
	signal(SIGPIPE, SIG_IGN);
	memset(&ts, 0, sizeof ts);

	t0 = os_msec();
	rc = dns_resolve(u.host, ip);
	t_dns = os_msec() - t0;
	if (rc != DNS_OK) {
		fprintf(stderr, "ufetch: %s: %s\n", u.host, dns_strerror(rc));
		return 1;
	}
	if (g_verbose)
		fprintf(stderr, "%s is %u.%u.%u.%u\n", u.host, ip[0], ip[1], ip[2], ip[3]);

	if (u.tls) {
		unsigned long te = os_msec();

		entropy_gather(seed, ".ufetch-seed", &er);
		if (g_verbose)
			fprintf(stderr, "entropy: urandom %d, seed file %d, "
				"%d distinct jitter deltas, %lu ms\n", er.urandom,
				er.seedfile, er.jitter_distinct, os_msec() - te);
		te = os_msec();
		for (k = 0; k < nca; k++)
			if (trust_load_pem(&ts, ca_list[k]) < 0) {
				fprintf(stderr, "ufetch: can't load trust anchors "
					"from %s\n", ca_list[k]);
				return 1;
			}
		if (ts.n == 0) {
			fprintf(stderr, "ufetch: no trust anchors\n");
			return 1;
		}
		t_trust = os_msec() - te;
	}

	t0 = os_msec();
	c.fd = tcp_connect(ip, u.port);
	t_conn = os_msec() - t0;
	if (c.fd < 0) {
		perror("ufetch: connect");
		return 1;
	}

	if (u.tls) {
		c.tls = 1;
		br_ssl_client_init_full(&c.sc, &c.xc, ts.ta, ts.n);
		if (strcmp(suites, "chacha") == 0)
			br_ssl_engine_set_suites(&c.sc.eng, suites_chacha,
				sizeof suites_chacha / sizeof suites_chacha[0]);
		else if (strcmp(suites, "gcm") == 0)
			br_ssl_engine_set_suites(&c.sc.eng, suites_gcm,
				sizeof suites_gcm / sizeof suites_gcm[0]);
		else if (strcmp(suites, "rsa") == 0)
			br_ssl_engine_set_suites(&c.sc.eng, suites_rsa,
				sizeof suites_rsa / sizeof suites_rsa[0]);
		else
			br_ssl_engine_set_suites(&c.sc.eng, suites_any,
				sizeof suites_any / sizeof suites_any[0]);
		if (bits == 15) {
			br_ssl_engine_set_ec(&c.sc.eng, &br_ec_all_m15);
			br_ssl_engine_set_rsavrfy(&c.sc.eng, br_rsa_i15_pkcs1_vrfy);
			br_ssl_engine_set_ecdsa(&c.sc.eng, br_ecdsa_i15_vrfy_asn1);
			br_x509_minimal_set_rsa(&c.xc, br_rsa_i15_pkcs1_vrfy);
			br_x509_minimal_set_ecdsa(&c.xc, &br_ec_all_m15,
				br_ecdsa_i15_vrfy_asn1);
		} else {
			br_ssl_engine_set_ec(&c.sc.eng, &br_ec_all_m31);
			br_ssl_engine_set_rsavrfy(&c.sc.eng, br_rsa_i31_pkcs1_vrfy);
			br_ssl_engine_set_ecdsa(&c.sc.eng, br_ecdsa_i31_vrfy_asn1);
			br_x509_minimal_set_rsa(&c.xc, br_rsa_i31_pkcs1_vrfy);
			br_x509_minimal_set_ecdsa(&c.xc, &br_ec_all_m31,
				br_ecdsa_i31_vrfy_asn1);
		}
		if (defer)
			xdefer_install(&xd, &c.sc);
		br_ssl_engine_set_buffer(&c.sc.eng, g_iobuf, sizeof g_iobuf, 1);
		br_ssl_engine_inject_entropy(&c.sc.eng, seed, sizeof seed);
		if (!br_ssl_client_reset(&c.sc, u.host, 0)) {
			fprintf(stderr, "ufetch: TLS reset failed (%d)\n",
				br_ssl_engine_last_error(&c.sc.eng));
			return 1;
		}
		br_sslio_init(&c.io, &c.sc.eng, low_read, &c.fd, low_write, &c.fd);
	}

	snprintf(req, sizeof req,
		"GET %s HTTP/1.0\r\n"
		"Host: %s\r\n"
		"User-Agent: ufetch/0 (68030; System V)\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n\r\n", u.path, u.host);
	t0 = os_msec();
	g_t_start = t0;
	g_trace = g_verbose;
	if (u.tls) {
		/* a flush with nothing written runs the handshake to its end */
		if (br_sslio_flush(&c.io) < 0) {
			fprintf(stderr, "ufetch: TLS handshake failed after %lu ms: "
				"error %d\n", os_msec() - t0,
				br_ssl_engine_last_error(&c.sc.eng));
			return 1;
		}
		t_hs = os_msec() - t0;
		g_trace = 0;
		if (defer) {
			unsigned long tv = os_msec();
			int err = xdefer_verify(&xd, &c.sc, &c.xc);

			t_verify = os_msec() - tv;
			if (g_verbose)
				fprintf(stderr, "  %6lu ms  deferred validation: %d certs, "
					"error %d\n", os_msec() - t0, xd.ncert, err);
			if (err) {
				fprintf(stderr, "ufetch: server not trusted: error %d "
					"(after %lu ms)\n", err, t_verify);
				return 1;
			}
		}
	}
	g_trace = g_verbose;
	if (conn_write(&c, req, strlen(req)) < 0) {
		fprintf(stderr, "ufetch: sending the request failed after %lu ms "
			"(error %d)\n", os_msec() - t0,
			u.tls ? br_ssl_engine_last_error(&c.sc.eng) : 0);
		return 1;
	}
	g_trace = 0;
	if (!u.tls)
		t_hs = 0;

	if (out_path) {
		out = fopen(out_path, "wb");
		if (out == NULL) {
			perror(out_path);
			return 1;
		}
	}
	t0 = os_msec();
	for (;;) {
		int n = conn_read(&c, buf, sizeof buf);

		if (n <= 0)
			break;
		if (total == 0)
			t_first = os_msec() - t0;
		total += (unsigned long)n;
		if (!header_done) {
			/* collect the header, then pass the rest through */
			size_t take = (size_t)n < sizeof hdr - 1 - hlen ? (size_t)n : sizeof hdr - 1 - hlen;
			char *end;

			memcpy(hdr + hlen, buf, take);
			hlen += take;
			hdr[hlen] = '\0';
			end = strstr(hdr, "\r\n\r\n");
			if (end || hlen == sizeof hdr - 1) {
				size_t hl = end ? (size_t)(end - hdr) + 4 : hlen;
				size_t start = take - (hlen - hl);	/* body starts here in buf */

				header_done = 1;
				if (g_verbose)
					fprintf(stderr, "%.*s", (int)hl, hdr);
				else
					fprintf(stderr, "%.*s\n", (int)strcspn(hdr, "\r\n"), hdr);
				body += (unsigned long)((size_t)n - start);
				if (out)
					fwrite(buf + start, 1, (size_t)n - start, out);
			}
		} else {
			body += (unsigned long)n;
			if (out)
				fwrite(buf, 1, (size_t)n, out);
		}
	}
	t_body = os_msec() - t0;
	if (out)
		fclose(out);

	if (u.tls) {
		int err = br_ssl_engine_last_error(&c.sc.eng);

		if (g_verbose || err != BR_ERR_OK)
			fprintf(stderr, "TLS 1.%d, %s (0x%04x), close error %d\n",
				(br_ssl_engine_get_version(&c.sc.eng) & 0xFF) - 1,
				suite_name(c.sc.eng.session.cipher_suite),
				c.sc.eng.session.cipher_suite, err);
	}
	tcp_close(c.fd);

	fprintf(stderr, "dns %lu ms, connect %lu ms", t_dns, t_conn);
	if (u.tls)
		fprintf(stderr, ", trust %lu ms (%lu anchors), handshake %lu ms%s",
			t_trust, (unsigned long)ts.n, t_hs, defer ? "" : " (incl. validation)");
	if (defer)
		fprintf(stderr, ", validation %lu ms", t_verify);
	fprintf(stderr, ", first byte %lu ms, body %lu B in %lu ms (%lu KB/s)\n",
		t_first, body, t_body, t_body ? total / 1024 * 1000 / t_body : 0UL);
	trust_free(&ts);
	return total > 0 ? 0 : 1;
}
