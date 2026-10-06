/*
 * tls.c - see tls.h. Persistent state in the data directory:
 *
 *   roots.bin   anchors from the PEM bundle (anchors.c format)
 *   inter.bin   intermediates this machine verified
 *   leaves      "sha256(leaf) days host" per line: leaves validated for
 *               a host, until they expire
 *   sessions    TLS session parameters per host:port (mode 600: they hold
 *               master secrets), dropped after SESSION_MAX_AGE
 *   hosts       "host profile" for hosts that refused FAST
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "os.h"
#include "sock.h"
#include "entropy.h"
#include "rsavrfy.h"
#include "ecvrfy.h"
#include "tls.h"

/* The key exchange's curves. X25519 by crypt68k (third_party/crypt68k,
 * its 68030 multiply; ~3x BearSSL's m31 there), the others by BearSSL's
 * m31. With no 64-bit integer type (Helios C: BR_NO_U64), BearSSL's m15
 * code instead: crypt68k is 68030 assembly. */
#if BR_NO_U64
#define EC_X25519	br_ec_c25519_m15
#define EC_ALL		br_ec_all_m15
#else
#include "crypt68k_bearssl.h"
#define EC_X25519	c68k_br_ec_c25519
#define EC_ALL		br_ec_all_m31
#endif

#define READ_TIMEOUT_MS		30000
#define MAX_SESSIONS		16
#define MAX_LEAVES		64
#define MAX_HOSTPREFS		32
#define SESSION_MAX_AGE		(12L * 3600)	/* seconds */

static struct anchors s_roots, s_inter, s_all;
static int s_all_dirty = 1;

/* --- small persistent tables ------------------------------------------------ */

struct session {
	char host[256];
	unsigned port;
	long when;
	br_ssl_session_parameters p;
};
static struct session s_sess[MAX_SESSIONS];

struct leaf {
	unsigned char hash[32];
	unsigned long notafter;
	char host[256];
};
static struct leaf s_leaves[MAX_LEAVES];
static int s_nleaves;

struct hostpref {
	char host[256];
	int profile;
};
static struct hostpref s_prefs[MAX_HOSTPREFS];
static int s_nprefs;

static char *datapath(const char *name)
{
	static char buf[4][600];
	static int k;

	k = (k + 1) & 3;
	return os_datapath(buf[k], sizeof buf[k], name);
}

static void hex(char *out, const unsigned char *p, size_t n)
{
	static const char d[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = d[p[i] >> 4];
		out[2 * i + 1] = d[p[i] & 15];
	}
	out[2 * n] = '\0';
}

static int unhex(unsigned char *out, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < 2 * n; i++) {
		int c = s[i], v;

		if (c >= '0' && c <= '9') v = c - '0';
		else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
		else return -1;
		if (i & 1) out[i / 2] |= (unsigned char)v;
		else out[i / 2] = (unsigned char)(v << 4);
	}
	return 0;
}

static void load_leaves(void)
{
	char *path = datapath("leaves"), line[400];
	FILE *f = path ? fopen(path, "r") : NULL;
	unsigned long today, sec;

	s_nleaves = 0;
	if (f == NULL)
		return;
	os_x509_now(&today, &sec);
	while (s_nleaves < MAX_LEAVES && fgets(line, sizeof line, f)) {
		struct leaf *l = &s_leaves[s_nleaves];
		char *h = strtok(line, " \n"), *d = strtok(NULL, " \n"),
			*host = strtok(NULL, " \n");

		if (!h || !d || !host || strlen(h) != 64 || strlen(host) >= sizeof l->host
			|| unhex(l->hash, h, 32) < 0)
			continue;
		l->notafter = strtoul(d, 0, 10);
		if (l->notafter < today)
			continue;
		strcpy(l->host, host);
		s_nleaves++;
	}
	fclose(f);
}

static void save_leaves(void)
{
	char *path = datapath("leaves"), buf[MAX_LEAVES * 340], h[65];
	size_t n = 0;
	int i;

	if (path == NULL)
		return;
	for (i = 0; i < s_nleaves; i++) {
		hex(h, s_leaves[i].hash, 32);
		n += (size_t)sprintf(buf + n, "%s %lu %s\n", h, s_leaves[i].notafter,
			s_leaves[i].host);
	}
	os_write_file(path, buf, n, 0644);
}

static int leaf_known(const unsigned char hash[32], const char *host)
{
	int i;

	for (i = 0; i < s_nleaves; i++)
		if (memcmp(s_leaves[i].hash, hash, 32) == 0
			&& strcmp(s_leaves[i].host, host) == 0)
			return 1;
	return 0;
}

static void leaf_remember(const unsigned char hash[32], unsigned long notafter,
	const char *host)
{
	struct leaf *l;

	if (leaf_known(hash, host) || strlen(host) >= sizeof l->host)
		return;
	if (s_nleaves == MAX_LEAVES) {		/* drop the oldest */
		memmove(s_leaves, s_leaves + 1, (MAX_LEAVES - 1) * sizeof *s_leaves);
		s_nleaves--;
	}
	l = &s_leaves[s_nleaves++];
	memcpy(l->hash, hash, 32);
	l->notafter = notafter;
	strcpy(l->host, host);
	save_leaves();
}

/*
 * sessions file: "host port when" + hex parameters per line. It holds
 * master secrets: mode 600, and entries expire.
 */
static void load_sessions(void)
{
	char *path = datapath("sessions"), line[600];
	FILE *f = path ? fopen(path, "r") : NULL;
	int n = 0;
	long now = (long)time(NULL);

	memset(s_sess, 0, sizeof s_sess);
	if (f == NULL)
		return;
	while (n < MAX_SESSIONS && fgets(line, sizeof line, f)) {
		struct session *s = &s_sess[n];
		char *host = strtok(line, " \n"), *port = strtok(NULL, " \n"),
			*when = strtok(NULL, " \n"), *p = strtok(NULL, " \n");

		if (!host || !port || !when || !p || strlen(host) >= sizeof s->host
			|| strlen(p) != 2 * sizeof s->p
			|| unhex((unsigned char *)&s->p, p, sizeof s->p) < 0)
			continue;
		s->when = strtol(when, 0, 10);
		if (now - s->when > SESSION_MAX_AGE || s->when > now + 300)
			continue;
		strcpy(s->host, host);
		s->port = (unsigned)strtoul(port, 0, 10);
		n++;
	}
	fclose(f);
}

static void save_sessions(void)
{
	char *path = datapath("sessions");
	static char buf[MAX_SESSIONS * 600];
	char h[2 * sizeof(br_ssl_session_parameters) + 1];
	size_t n = 0;
	int i;

	if (path == NULL)
		return;
	for (i = 0; i < MAX_SESSIONS; i++)
		if (s_sess[i].host[0]) {
			hex(h, (unsigned char *)&s_sess[i].p, sizeof s_sess[i].p);
			n += (size_t)sprintf(buf + n, "%s %u %ld %s\n", s_sess[i].host,
				s_sess[i].port, s_sess[i].when, h);
		}
	os_write_file(path, buf, n, 0600);
	memset(buf, 0, sizeof buf);
}

static struct session *session_find(const char *host, unsigned port)
{
	int i;

	for (i = 0; i < MAX_SESSIONS; i++)
		if (s_sess[i].host[0] && s_sess[i].port == port
			&& strcmp(s_sess[i].host, host) == 0)
			return &s_sess[i];
	return NULL;
}

static void session_store(const char *host, unsigned port,
	const br_ssl_session_parameters *p)
{
	struct session *s = session_find(host, port);
	int i;

	if (p->session_id_len == 0 || strlen(host) >= sizeof s->host)
		return;
	if (s == NULL) {		/* a free slot, else the oldest */
		s = &s_sess[0];
		for (i = 0; i < MAX_SESSIONS; i++) {
			if (!s_sess[i].host[0]) {
				s = &s_sess[i];
				break;
			}
			if (s_sess[i].when < s->when)
				s = &s_sess[i];
		}
	}
	strcpy(s->host, host);
	s->port = port;
	s->when = (long)time(NULL);
	s->p = *p;
	save_sessions();
}

static void session_drop(const char *host, unsigned port)
{
	struct session *s = session_find(host, port);

	if (s) {
		memset(s, 0, sizeof *s);
		save_sessions();
	}
}

static void load_hostprefs(void)
{
	char *path = datapath("hosts"), line[300];
	FILE *f = path ? fopen(path, "r") : NULL;

	s_nprefs = 0;
	if (f == NULL)
		return;
	while (s_nprefs < MAX_HOSTPREFS && fgets(line, sizeof line, f)) {
		char *h = strtok(line, " \n"), *what = strtok(NULL, " \n");
		int p;

		if (!h || !what || strlen(h) >= 256)
			continue;
		p = strcmp(what, "full") == 0 ? TLS_FULL
			: strcmp(what, "full-x25519") == 0 ? TLS_FULL_X : TLS_FAST;
		strcpy(s_prefs[s_nprefs].host, h);
		s_prefs[s_nprefs++].profile = p;
	}
	fclose(f);
}

const char *tls_profile_name(int profile)
{
	return profile == TLS_FULL ? "full" : profile == TLS_FULL_X
		? "full-x25519" : "fast";
}

int tls_host_profile(const char *host)
{
	int i;

	for (i = 0; i < s_nprefs; i++)
		if (strcmp(s_prefs[i].host, host) == 0)
			return s_prefs[i].profile;
	return TLS_FAST;
}

void tls_host_set_profile(const char *host, int profile)
{
	char *path, buf[MAX_HOSTPREFS * 280];
	size_t n = 0;
	int i;

	if (tls_host_profile(host) == profile || strlen(host) >= 256)
		return;
	for (i = 0; i < s_nprefs; i++)
		if (strcmp(s_prefs[i].host, host) == 0)
			break;
	if (i == s_nprefs) {
		if (s_nprefs == MAX_HOSTPREFS) {
			memmove(s_prefs, s_prefs + 1, (MAX_HOSTPREFS - 1) * sizeof *s_prefs);
			s_nprefs--;
		}
		i = s_nprefs++;
		strcpy(s_prefs[i].host, host);
	}
	s_prefs[i].profile = profile;
	if ((path = datapath("hosts")) == NULL)
		return;
	for (i = 0; i < s_nprefs; i++)
		if (s_prefs[i].profile != TLS_FAST)
			n += (size_t)sprintf(buf + n, "%s %s\n", s_prefs[i].host,
				tls_profile_name(s_prefs[i].profile));
	os_write_file(path, buf, n, 0644);
}

/* --- anchors --------------------------------------------------------------- */

static int rebuild_all(void)
{
	size_t i;

	anchors_free(&s_all);
	anchors_init(&s_all);
	for (i = 0; i < s_roots.n; i++)
		if (anchors_add_copy(&s_all, &s_roots, i) < 0)
			return -1;
	for (i = 0; i < s_inter.n; i++)
		if (anchors_add_copy(&s_all, &s_inter, i) < 0)
			return -1;
	s_all_dirty = 0;
	return 0;
}

int tls_build_roots(const char *pem_path)
{
	struct anchors a;
	long size, mtime;
	char *path = datapath("roots.bin");
	int n;

	if (os_file_info(pem_path, &size, &mtime) < 0)
		return -1;
	anchors_init(&a);
	if (anchors_load_pem(&a, pem_path) < 0 || a.n == 0
		|| path == NULL || anchors_save(&a, path, size, mtime) < 0) {
		anchors_free(&a);
		return -1;
	}
	n = (int)a.n;
	anchors_free(&s_roots);
	s_roots = a;
	s_all_dirty = 1;
	return n;
}

int tls_init(const char *pem_path, void (*note)(const char *msg))
{
	long psize = 0, pmtime = 0, csize = -1, cmtime = -1;
	char *roots = datapath("roots.bin"), *inter = datapath("inter.bin");

	anchors_free(&s_roots);
	anchors_free(&s_inter);
	anchors_init(&s_roots);
	anchors_init(&s_inter);
	if (roots)
		anchors_load(&s_roots, roots, &csize, &cmtime);
	if (pem_path && os_file_info(pem_path, &psize, &pmtime) == 0
		&& (s_roots.n == 0 || psize != csize || pmtime != cmtime)) {
		if (note)
			note("Building the certificate store (once; this takes a "
				"minute on a 68030)...");
		tls_build_roots(pem_path);
	}
	if (inter)
		anchors_load(&s_inter, inter, NULL, NULL);
	load_leaves();
	load_sessions();
	load_hostprefs();
	if (rebuild_all() < 0 || s_all.n == 0)
		return -1;
	return 0;
}

/* certificates 1..upto of a validated chain become anchors */
static int learn_chain(struct xdefer *xd, int upto)
{
	int i, learned = 0;
	char *path;

	for (i = 1; i <= upto && i < xd->ncert; i++)
		if (anchors_add_der(&s_inter, xd->chain + xd->cert_off[i],
			xd->cert_len[i]) > 0)
			learned++;
	if (learned) {
		if ((path = datapath("inter.bin")) != NULL)
			anchors_save(&s_inter, path, 0, 0);
		s_all_dirty = 1;
	}
	return learned;
}

/* --- connections ---------------------------------------------------------- */

static int low_read(void *ctx, unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;

	for (;;) {
		struct pollfd pfd;
		int n;

		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		n = poll(&pfd, 1, READ_TIMEOUT_MS);
		if (n < 0 && SOCK_RETRY(errno))
			continue;
		if (n <= 0)
			return -1;
		n = read(fd, buf, (unsigned)len);
		if (n < 0 && SOCK_RETRY(errno))
			continue;
		if (n > 0)
			entropy_event();	/* arrival times feed the pool */
		return n > 0 ? n : -1;
	}
}

static int low_write(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;

	for (;;) {
		int n = write(fd, buf, (unsigned)len);

		if (n < 0 && SOCK_RETRY(errno))
			continue;
		return n > 0 ? n : -1;
	}
}

static const uint16_t suites_fast[] = {
	BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
};
static const uint16_t suites_full[] = {
	BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
	BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
	BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
	BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
	BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
};

/* The key exchange's curves (EC_X25519, EC_ALL at the top). FAST and
 * FULL_X offer X25519 alone; FULL offers all of them, through these. */
static const br_ec_impl *ec_for(int curve)
{
	return curve == BR_EC_curve25519 ? &EC_X25519 : &EC_ALL;
}

static const unsigned char *ec_generator(int curve, size_t *len)
{
	return ec_for(curve)->generator(curve, len);
}

static const unsigned char *ec_order(int curve, size_t *len)
{
	return ec_for(curve)->order(curve, len);
}

static size_t ec_xoff(int curve, size_t *len)
{
	return ec_for(curve)->xoff(curve, len);
}

static uint32_t ec_mul(unsigned char *G, size_t Glen, const unsigned char *x, size_t xlen,
	int curve)
{
	return ec_for(curve)->mul(G, Glen, x, xlen, curve);
}

static size_t ec_mulgen(unsigned char *R, const unsigned char *x, size_t xlen, int curve)
{
	return ec_for(curve)->mulgen(R, x, xlen, curve);
}

static uint32_t ec_muladd(unsigned char *A, const unsigned char *B, size_t len,
	const unsigned char *x, size_t xlen, const unsigned char *y, size_t ylen, int curve)
{
	return ec_for(curve)->muladd(A, B, len, x, xlen, y, ylen, curve);
}

static const br_ec_impl s_ec_all = {
	(uint32_t)1 << BR_EC_secp256r1 | (uint32_t)1 << BR_EC_secp384r1
		| (uint32_t)1 << BR_EC_secp521r1 | (uint32_t)1 << BR_EC_curve25519,
	ec_generator, ec_order, ec_xoff, ec_mul, ec_mulgen, ec_muladd
};

static unsigned long ms_since(unsigned long t0)
{
	return os_msec() - t0;
}

int tls_connect(struct tls_conn *c, int fd, const char *host, unsigned port,
	int profile, int early)
{
	unsigned char seed[32];
	struct session *sess;
	unsigned long t0;
	int err;

	memset(&c->info, 0, sizeof c->info);
	c->info.profile = profile;
	c->fd = fd;
	c->open = 0;
	c->verify_pending = 0;
	c->verify_err = 0;
	c->port = port;
	if (strlen(host) >= sizeof c->host)
		return BR_ERR_BAD_PARAM;
	strcpy(c->host, host);
	if (entropy_extract(seed) < 0)
		return TLS_ERR_NOT_SEEDED;
	if (s_all_dirty && rebuild_all() < 0)
		return TLS_ERR_NO_ANCHORS;
	if (s_all.n == 0)
		return TLS_ERR_NO_ANCHORS;

	br_ssl_client_init_full(&c->sc, &c->xc, s_all.ta, s_all.n);
	if (profile == TLS_FAST) {
		br_ssl_engine_set_suites(&c->sc.eng, suites_fast,
			sizeof suites_fast / sizeof suites_fast[0]);
		br_ssl_engine_set_ec(&c->sc.eng, &EC_X25519);
	} else {
		br_ssl_engine_set_suites(&c->sc.eng, suites_full,
			sizeof suites_full / sizeof suites_full[0]);
		if (profile == TLS_FULL_X)
			br_ssl_engine_set_ec(&c->sc.eng, &EC_X25519);
		else
			br_ssl_engine_set_ec(&c->sc.eng, &s_ec_all);
	}
	/* the fastest code measured on the TT: signatures checked by rsavrfy
	 * and ecvrfy (on the 68030's mulu.l; P-521 through to i31), the key
	 * exchange on m31 curves */
	br_ssl_engine_set_rsavrfy(&c->sc.eng, rsavrfy_pkcs1);
	br_ssl_engine_set_ecdsa(&c->sc.eng, ecvrfy_asn1);
	br_x509_minimal_set_rsa(&c->xc, rsavrfy_pkcs1);
	br_x509_minimal_set_ecdsa(&c->xc, &EC_ALL, ecvrfy_asn1);
	br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
	xdefer_install(&c->xd, &c->sc);
	c->xd.iec = &EC_ALL;	/* not the key exchange's subset */
	br_ssl_engine_inject_entropy(&c->sc.eng, seed, sizeof seed);
	memset(seed, 0, sizeof seed);

	sess = session_find(host, port);
	if (sess)
		br_ssl_engine_set_session_parameters(&c->sc.eng, &sess->p);
	if (!br_ssl_client_reset(&c->sc, host, sess != NULL))
		return br_ssl_engine_last_error(&c->sc.eng);
	br_sslio_init(&c->io, &c->sc.eng, low_read, &c->fd, low_write, &c->fd);

	/* the handshake: a flush with nothing written runs it to its end */
	t0 = os_msec();
	if (br_sslio_flush(&c->io) < 0) {
		err = br_ssl_engine_last_error(&c->sc.eng);
		if (sess)
			session_drop(host, port);
		return err ? err : BR_ERR_IO;
	}
	c->info.t_handshake = ms_since(t0);
	c->info.version = br_ssl_engine_get_version(&c->sc.eng);
	c->info.suite = c->sc.eng.session.cipher_suite;

	/* resumed: no certificate came, the session was validated before */
	if (sess && c->xd.ncert == 0
		&& c->sc.eng.session.session_id_len == sess->p.session_id_len
		&& memcmp(c->sc.eng.session.session_id, sess->p.session_id,
			sess->p.session_id_len) == 0) {
		c->info.resumed = 1;
		c->open = 1;
		return 0;
	}

	/* a full handshake: validate before anything moves, or, for an
	 * early request, before the first byte is read */
	c->open = 1;
	if (early) {
		c->verify_pending = 1;
		return 0;
	}
	return tls_verify(c);
}

int tls_verify(struct tls_conn *c)
{
	unsigned char hash[32];
	unsigned long t0;
	int err, anchor_at, known;
	const char *host = c->host;
	unsigned port = c->port;

	c->verify_pending = 0;
	t0 = os_msec();
	known = 0;
	if (c->xd.ncert > 0) {
		br_sha256_context h;

		br_sha256_init(&h);
		br_sha256_update(&h, c->xd.chain + c->xd.cert_off[0], c->xd.cert_len[0]);
		br_sha256_out(&h, hash);
		known = leaf_known(hash, host);
	}
	err = xdefer_verify(&c->xd, &c->sc, &c->xc, known, &anchor_at);
	c->info.t_verify = ms_since(t0);
	c->info.pre_jobs = c->xd.pre_jobs;
	c->info.pre_t425 = c->xd.pre_t425;
	c->info.pre_used = c->xd.pre_used;
	c->info.leaf_memo = known;
	if (err) {
		session_drop(host, port);
		c->open = 0;
		c->verify_err = err;
		return err;
	}
	if (!known) {
		br_x509_decoder_context dc;

		br_x509_decoder_init(&dc, 0, 0);
		br_x509_decoder_push(&dc, c->xd.chain + c->xd.cert_off[0],
			c->xd.cert_len[0]);
		if (br_x509_decoder_get_pkey(&dc))
			leaf_remember(hash, dc.notafter_days, host);
		if (anchor_at > 0)
			c->info.learned = learn_chain(&c->xd, anchor_at);
	}
	session_store(host, port, &c->sc.eng.session);
	return 0;
}

int tls_read(struct tls_conn *c, void *buf, size_t len)
{
	int n;

	/* an early request: nothing from the server is taken before the
	 * server has been validated */
	if (c->verify_pending && tls_verify(c) != 0)
		return -1;
	if (!c->open)
		return -1;
	n = br_sslio_read(&c->io, buf, len);
	if (n < 0) {
		/* a clean close_notify is the end of the stream */
		int err = br_ssl_engine_last_error(&c->sc.eng);

		c->open = 0;
		return err == BR_ERR_OK ? 0 : -1;
	}
	return n;
}

int tls_write(struct tls_conn *c, const void *buf, size_t len)
{
	if (!c->open)
		return -1;
	if (br_sslio_write_all(&c->io, buf, len) < 0 || br_sslio_flush(&c->io) < 0) {
		c->open = 0;
		return -1;
	}
	return 0;
}

void tls_close(struct tls_conn *c)
{
	if (c->open)
		br_sslio_close(&c->io);
	c->open = 0;
}

int tls_retry_full(int err)
{
	/* the server refused what FAST offered (no RSA certificate, no
	 * X25519, ...) or hung up on it: an alert, or a close before any
	 * validation happened */
	return (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_RECV_FATAL_ALERT + 256)
		|| err == BR_ERR_IO || err == BR_ERR_UNEXPECTED
		|| err == BR_ERR_BAD_HANDSHAKE;
}

/* --- preloading intermediates ---------------------------------------------- */

struct learnctx {
	br_x509_minimal_context xc;
	br_x509_decoder_context dc;
	unsigned char der[8192];
	size_t len;
	int in_cert, overflow;
};

static void to_buf(void *ctx, const void *data, size_t len)
{
	struct learnctx *l = ctx;

	if (l->len + len > sizeof l->der) {
		l->overflow = 1;
		return;
	}
	memcpy(l->der + l->len, data, len);
	l->len += len;
}

int tls_learn_pem(const char *pem_path, void (*note)(const char *msg))
{
	static struct learnctx l;
	static unsigned char buf[2048];
	br_pem_decoder_context pc;
	FILE *f = fopen(pem_path, "rb");
	size_t got;
	int learned = 0;
	char msg[160];

	if (f == NULL)
		return -1;
	if (s_all_dirty && rebuild_all() < 0) {
		fclose(f);
		return -1;
	}
	br_pem_decoder_init(&pc);
	while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
		unsigned char *p = buf;

		while (got > 0) {
			size_t used = br_pem_decoder_push(&pc, p, got);

			p += used;
			got -= used;
			switch (br_pem_decoder_event(&pc)) {
			case BR_PEM_BEGIN_OBJ:
				l.in_cert = strcmp(br_pem_decoder_name(&pc), "CERTIFICATE") == 0;
				l.len = 0;
				l.overflow = 0;
				br_pem_decoder_setdest(&pc, l.in_cert ? to_buf : 0, &l);
				break;
			case BR_PEM_END_OBJ:
				if (l.in_cert && !l.overflow) {
					/* validate it as a one-certificate chain,
					 * without a server name */
					const br_x509_class **v = &l.xc.vtable;
					unsigned long t0 = os_msec();
					unsigned err;

					br_x509_minimal_init_full(&l.xc, s_all.ta, s_all.n);
					br_x509_minimal_set_rsa(&l.xc, rsavrfy_pkcs1);
					br_x509_minimal_set_ecdsa(&l.xc, &EC_ALL,
						ecvrfy_asn1);
					(*v)->start_chain(v, NULL);
					(*v)->start_cert(v, (uint32_t)l.len);
					(*v)->append(v, l.der, l.len);
					(*v)->end_cert(v);
					err = (*v)->end_chain(v);
					br_x509_decoder_init(&l.dc, 0, 0);
					br_x509_decoder_push(&l.dc, l.der, l.len);
					if (err == 0 && br_x509_decoder_isCA(&l.dc)
						&& anchors_add_der(&s_inter, l.der, l.len) > 0) {
						learned++;
						s_all_dirty = 1;
						rebuild_all();
					}
					if (note) {
						snprintf(msg, sizeof msg, "  %s (%lu ms)",
							err ? tls_strerror((int)err)
							: br_x509_decoder_isCA(&l.dc) ? "verified" : "not a CA",
							ms_since(t0));
						note(msg);
					}
				}
				l.in_cert = 0;
				break;
			}
		}
	}
	fclose(f);
	if (learned) {
		char *path = datapath("inter.bin");

		if (path)
			anchors_save(&s_inter, path, 0, 0);
	}
	return learned;
}

const char *tls_strerror(int err)
{
	static char buf[64];

	switch (err) {
	case 0: return "ok";
	case TLS_ERR_NOT_SEEDED: return "random pool not seeded (run: manxtrust seed)";
	case TLS_ERR_NO_ANCHORS: return "no trusted certificates (run: manxtrust roots FILE)";
	case BR_ERR_IO: return "connection closed by the server";
	case BR_ERR_X509_EXPIRED: return "certificate expired or not yet valid";
	case BR_ERR_X509_BAD_SERVER_NAME: return "certificate is for another host";
	case BR_ERR_X509_NOT_TRUSTED: return "certificate not trusted";
	case BR_ERR_BAD_SIGNATURE: return "bad server signature";
	case BR_ERR_X509_BAD_SIGNATURE: return "bad certificate signature";
	case BR_ERR_X509_LIMIT_EXCEEDED: return "certificate chain too large";
	case BR_ERR_UNSUPPORTED_VERSION: return "server needs a TLS version we lack";
	}
	if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_RECV_FATAL_ALERT + 256) {
		int a = err - BR_ERR_RECV_FATAL_ALERT;

		if (a == 40)
			return "server refused the handshake (no common cipher/curve)";
		if (a == 70)
			return "server refused the TLS version";
		snprintf(buf, sizeof buf, "server sent alert %d", a);
		return buf;
	}
	snprintf(buf, sizeof buf, "TLS error %d", err);
	return buf;
}
