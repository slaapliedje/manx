/*
 * update.c - see update.h.
 */
#if !defined(MANX_SYSV4) && !defined(__sgi) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 600	/* (lstat, readlink, nice under -std=c99) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef MANX_SYSV4
pid_t fork(void);
pid_t getpid(void);
pid_t waitpid(pid_t, int *, int);
int link(const char *, const char *);
int unlink(const char *);
int rename(const char *, const char *);
int access(const char *, int);
int readlink(const char *, char *, int);
int lstat(const char *, struct stat *);
int nice(int);
void _exit(int);
int chmod(const char *, mode_t);
#define WNOHANG	0100
#define W_OK	2
#define X_OK	1
#else
#include <unistd.h>
#ifndef MANX_HELIOS
#include <sys/wait.h>
#endif
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)
#endif
#include "bearssl.h"
#include "os.h"
#include "config.h"
#include "fetch.h"
#include "inflate.h"
#include "ecvrfy.h"
#include "entropy.h"
#include "tpoff.h"
#include "update.h"
#include "update_key.h"

/* (from the Makefile: VERSION, the target, PACKAGED=) */
#ifndef MANX_VERSION
#define MANX_VERSION	"0"
#endif
#ifndef MANX_PLATFORM
#define MANX_PLATFORM	"unknown"
#endif
#ifndef MANX_PACKAGED
#define MANX_PACKAGED	""
#endif

#define MANIFEST_MAX	16384
#define SIG_MAX		160

const char *update_version(void)
{
	return MANX_VERSION;
}

const char *update_platform(void)
{
	return MANX_PLATFORM;
}

const char *update_packaged(void)
{
	return MANX_PACKAGED;
}

/* one dotted part (what follows its digits, "-rc1", is ignored) */
static long vpart(const char **s)
{
	long n = 0;

	while (**s >= '0' && **s <= '9')
		n = n * 10 + *(*s)++ - '0';
	while (**s && **s != '.')
		(*s)++;
	if (**s == '.')
		(*s)++;
	return n;
}

int update_vercmp(const char *a, const char *b)
{
	while (*a || *b) {
		long x = vpart(&a), y = vpart(&b);

		if (x != y)
			return x < y ? -1 : 1;
	}
	return 0;
}

int update_verify(const void *text, size_t len, const void *sig,
	size_t siglen, const unsigned char *pub)
{
	br_sha256_context h;
	unsigned char hash[32];
	br_ec_public_key pk;

	br_sha256_init(&h);
	br_sha256_update(&h, text, len);
	br_sha256_out(&h, hash);
	pk.curve = BR_EC_secp256r1;
	pk.q = (unsigned char *)(pub ? pub : update_key);
	pk.qlen = 65;
	return ecvrfy_asn1(&br_ec_prime_i31, hash, sizeof hash, &pk, sig,
		siglen) == 1;
}

static int unhex(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* the next space-separated word of *s into w (cut to n) */
static int word(const char **s, char *w, size_t n)
{
	size_t k = 0;

	while (**s == ' ' || **s == '\t')
		(*s)++;
	while (**s && **s != ' ' && **s != '\t') {
		if (k + 1 < n)
			w[k++] = **s;
		(*s)++;
	}
	w[k] = '\0';
	return k > 0;
}

/* "file PLATFORM NAME SIZE SHA256 URL", the rest after "file" */
static int file_line(const char *s, const char *platform,
	struct update_info *u, char *err, size_t errn)
{
	char plat[32], name[32], size[16], hex[80], url[URL_MAX];
	struct update_file *f;
	int i;

	if (!word(&s, plat, sizeof plat) || !word(&s, name, sizeof name)
		|| !word(&s, size, sizeof size) || !word(&s, hex, sizeof hex)
		|| !word(&s, url, sizeof url) || strlen(hex) != 64) {
		snprintf(err, errn, "a file line of the manifest isn't right");
		return -1;
	}
	if (strcmp(plat, platform) != 0)
		return 0;
	if (u->nfiles == UPDATE_FILES || strchr(name, '/') || name[0] == '.') {
		snprintf(err, errn, "the manifest's files for %s aren't right",
			platform);
		return -1;
	}
	f = &u->files[u->nfiles++];
	snprintf(f->name, sizeof f->name, "%s", name);
	f->size = atol(size);
	snprintf(f->url, sizeof f->url, "%s", url);
	for (i = 0; i < 32; i++) {
		int hi = unhex(hex[2 * i]), lo = unhex(hex[2 * i + 1]);

		if (hi < 0 || lo < 0) {
			snprintf(err, errn, "a checksum in the manifest isn't hex");
			return -1;
		}
		f->sha256[i] = (unsigned char)(hi << 4 | lo);
	}
	return 0;
}

int update_parse(const char *text, size_t len, const char *platform,
	struct update_info *u, char *err, size_t errn)
{
	char line[URL_MAX + 200];
	size_t at = 0, k;
	int first = 1;

	memset(u, 0, sizeof *u);
	while (at < len) {
		const char *s = line;

		for (k = 0; at < len && text[at] != '\n'; at++)
			if (k + 1 < sizeof line)
				line[k++] = text[at];
		at++;
		while (k > 0 && (line[k - 1] == '\r' || line[k - 1] == ' '))
			k--;
		line[k] = '\0';
		if (first) {
			if (strcmp(line, "manx-update 1") != 0) {
				snprintf(err, errn, "not a manifest Manx knows");
				return -1;
			}
			first = 0;
		} else if (strncmp(line, "version ", 8) == 0) {
			s += 8;
			word(&s, u->version, sizeof u->version);
		} else if (strncmp(line, "date ", 5) == 0) {
			s += 5;
			word(&s, u->date, sizeof u->date);
		} else if (strncmp(line, "file ", 5) == 0) {
			if (file_line(line + 5, platform, u, err, errn) < 0)
				return -1;
		} else if (strncmp(line, "note ", 5) == 0) {
			size_t n = strlen(u->notes);

			snprintf(u->notes + n, sizeof u->notes - n, "%s\n",
				line + 5);
		}
		/* (anything else: a later manifest's, ignored) */
	}
	if (first || !u->version[0]) {
		snprintf(err, errn, "the manifest names no version");
		return -1;
	}
	return 0;
}

/* --- fetching --- */

struct buf {
	unsigned char *p;
	size_t n, cap;
};

static int buf_body(void *ctx, const unsigned char *d, size_t n)
{
	struct buf *b = ctx;

	if (n > b->cap - b->n)
		return -1;
	memcpy(b->p + b->n, d, n);
	b->n += n;
	return 0;
}

static void buf_reset(void *ctx)
{
	((struct buf *)ctx)->n = 0;
}

/* a small file whole into out: 0, or -1 with err */
static int get_small(const char *url, unsigned char *out, size_t cap,
	size_t *len, char *err, size_t errn)
{
	static struct fetch_result res;
	struct fetch_cb cb;
	struct buf b;

	b.p = out;
	b.n = 0;
	b.cap = cap;
	memset(&cb, 0, sizeof cb);
	cb.ctx = &b;
	cb.body = buf_body;
	cb.reset = buf_reset;
	if (fetch(url, "GET", &cb, &res) < 0) {
		snprintf(err, errn, "%s", res.error[0] ? res.error
			: "the download stopped");
		return -1;
	}
	if (res.status != 200) {
		snprintf(err, errn, "%.300s: HTTP %d", url, res.status);
		return -1;
	}
	*len = b.n;
	return 0;
}

int update_check(const char *url, struct update_info *u, char *err,
	size_t errn, void (*status)(void *ctx, const char *msg), void *ctx)
{
	static unsigned char text[MANIFEST_MAX];
	unsigned char sig[SIG_MAX];
	char sigurl[URL_MAX];
	size_t tlen, slen;
	const char *slash;

	if (url == NULL)
		url = config_str("update_url", UPDATE_URL);
	/* the signature beside it: .../manx-update.sig */
	slash = strrchr(url, '/');
	snprintf(sigurl, sizeof sigurl, "%.*smanx-update.sig",
		slash ? (int)(slash + 1 - url) : 0, url);
	if (status)
		status(ctx, "Asking what the newest Manx is");
	if (get_small(url, text, sizeof text, &tlen, err, errn) < 0
		|| get_small(sigurl, sig, sizeof sig, &slen, err, errn) < 0)
		return -1;
	if (!update_verify(text, tlen, sig, slen, NULL)) {
		snprintf(err, errn, "the newest release's manifest isn't signed "
			"by the release key: not trusted");
		return -1;
	}
	if (update_parse((const char *)text, tlen, update_platform(), u, err,
			errn) < 0)
		return -1;
	return update_vercmp(u->version, update_version()) > 0;
}

/* --- where it is --- */

static char g_self[600];

void update_init(const char *argv0)
{
	char p[600];

	if (argv0 == NULL || !argv0[0])
		return;
	snprintf(p, sizeof p, "%s", argv0);
#ifndef MANX_HELIOS
	if (!strchr(argv0, '/')) {
		/* a name found on $PATH, as the shell did */
		const char *path = getenv("PATH"), *d = path;
		struct stat st;

		p[0] = '\0';
		while (d && *d) {
			size_t n = strcspn(d, ":");

			snprintf(p, sizeof p, "%.*s/%s", n ? (int)n : 1,
				n ? d : ".", argv0);
			if (stat(p, &st) == 0 && S_ISREG(st.st_mode)
				&& access(p, X_OK) == 0)
				break;
			p[0] = '\0';
			d += n;
			if (*d == ':')
				d++;
		}
		if (!p[0])
			return;
	}
	{
		/* links followed to the program itself (what's replaced) */
		char l[600], q[1200];
		struct stat st;
		int i, n;

		for (i = 0; i < 8; i++) {
			const char *slash;

			if (lstat(p, &st) < 0 || !S_ISLNK(st.st_mode))
				break;
			if ((n = (int)readlink(p, l, sizeof l - 1)) <= 0)
				break;
			l[n] = '\0';
			slash = strrchr(p, '/');
			if (l[0] == '/' || slash == NULL)
				snprintf(q, sizeof q, "%s", l);
			else
				snprintf(q, sizeof q, "%.*s%s",
					(int)(slash + 1 - p), p, l);
			snprintf(p, sizeof p, "%.599s", q);
		}
	}
#endif
	snprintf(g_self, sizeof g_self, "%s", p);
}

const char *update_self(void)
{
	return g_self;
}

/* the directory of path, "." for none */
static void dir_of(const char *path, char *out, size_t n)
{
	const char *slash = strrchr(path, '/');

	if (slash == NULL)
		snprintf(out, n, ".");
	else if (slash == path)
		snprintf(out, n, "/");
	else
		snprintf(out, n, "%.*s", (int)(slash - path), path);
}

static const char *base_of(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

int update_can_install(char *why, size_t n)
{
	char dir[600];

	if (strcmp(update_packaged(), "inst") == 0) {
		snprintf(why, n, "This Manx came in an inst package: install the "
			"new one's (from %s) with inst or the Software Manager.",
			UPDATE_PAGE);
		return 0;
	}
	if (strcmp(update_packaged(), "tdnf") == 0) {
		snprintf(why, n, "This Manx came from mogrix: \"tdnf update "
			"manx\", as root, updates it once mogrix has the new one.");
		return 0;
	}
	if (update_packaged()[0]) {
		snprintf(why, n, "This Manx came in a %s package: update it "
			"with that.", update_packaged());
		return 0;
	}
#ifdef MANX_HELIOS
	snprintf(why, n, "Manx can't replace itself on Helios.");
	return 0;
#else
	if (!g_self[0]) {
		snprintf(why, n, "Manx can't tell where it was started from.");
		return 0;
	}
	dir_of(g_self, dir, sizeof dir);
	if (access(dir, W_OK) != 0 || access(g_self, W_OK) != 0) {
		snprintf(why, n, "Manx is in %.300s, which you can't change: "
			"whoever installed it can, or get the new one from %s.",
			dir, UPDATE_PAGE);
		return 0;
	}
	return 1;
#endif
}

#ifndef MANX_HELIOS

/* --- installing --- */

struct dl {
	const struct update_file *file;
	const char *path;
	FILE *f;
	struct inflate *z;
	br_sha256_context h;
	long n, told;
	int bad;
	void (*status)(void *ctx, const char *msg);
	void *ctx;
};

static int dl_out(void *ctx, const unsigned char *d, size_t n)
{
	struct dl *x = ctx;

	if ((long)n > x->file->size - x->n || fwrite(d, 1, n, x->f) != n) {
		x->bad = 1;
		return -1;
	}
	br_sha256_update(&x->h, d, n);
	x->n += (long)n;
	if (x->status && x->n - x->told >= 32768) {
		char msg[120];

		x->told = x->n;
		snprintf(msg, sizeof msg, "Getting %s: %ld of %ld KB",
			x->file->name, x->n / 1024, x->file->size / 1024);
		x->status(x->ctx, msg);
	}
	return 0;
}

static int dl_body(void *ctx, const unsigned char *d, size_t n)
{
	struct dl *x = ctx;
	int r;

	if (x->bad)
		return -1;
	r = inflate_feed(x->z, d, n);
	if (r == INF_BAD || r == INF_STOP) {
		x->bad = 1;
		return -1;
	}
	return 0;
}

/* a broken transfer is retried from the start */
static void dl_reset(void *ctx)
{
	struct dl *x = ctx;

	if (x->f)
		x->f = freopen(x->path, "wb", x->f);
	inflate_free(x->z);
	x->z = inflate_new(INF_GZIP, dl_out, x);
	br_sha256_init(&x->h);
	x->n = x->told = 0;
	x->bad = x->f == NULL || x->z == NULL;
}

static void dl_status(void *ctx, const char *msg)
{
	struct dl *x = ctx;

	if (x->status)
		x->status(x->ctx, msg);
}

/* f into path, checked: 0, or -1 with msg (and path removed) */
static int download(const struct update_file *f, const char *path,
	char *msg, size_t n, void (*status)(void *ctx, const char *msg),
	void *ctx)
{
	static struct fetch_result res;
	struct fetch_cb cb;
	struct dl x;
	unsigned char got[32];
	int rc, ok;

	memset(&x, 0, sizeof x);
	x.file = f;
	x.path = path;
	x.status = status;
	x.ctx = ctx;
	if ((x.f = fopen(path, "wb")) == NULL) {
		snprintf(msg, n, "Can't write %.300s.", path);
		return -1;
	}
	if ((x.z = inflate_new(INF_GZIP, dl_out, &x)) == NULL) {
		fclose(x.f);
		unlink(path);
		snprintf(msg, n, "Out of memory.");
		return -1;
	}
	br_sha256_init(&x.h);
	memset(&cb, 0, sizeof cb);
	cb.ctx = &x;
	cb.status = dl_status;
	cb.body = dl_body;
	cb.reset = dl_reset;
	rc = fetch(f->url, "GET", &cb, &res);
	ok = rc == 0 && res.status == 200 && !x.bad && x.z
		&& inflate_finish(x.z) == INF_END;
	inflate_free(x.z);
	if (x.f == NULL || fclose(x.f) != 0)
		ok = 0;
	br_sha256_out(&x.h, got);
	if (!ok || x.n != f->size || memcmp(got, f->sha256, 32) != 0) {
		unlink(path);
		if (rc < 0)
			snprintf(msg, n, "Getting %s: %.200s", f->name, res.error);
		else if (res.status != 200)
			snprintf(msg, n, "Getting %s: HTTP %d", f->name,
				res.status);
		else
			snprintf(msg, n, "%s didn't arrive as it should (its size "
				"or checksum is wrong): nothing was replaced.",
				f->name);
		return -1;
	}
	return 0;
}

int update_install(const struct update_info *u, char *msg, size_t n,
	void (*status)(void *ctx, const char *msg), void *ctx)
{
	static char dst[UPDATE_FILES][640], tmp[UPDATE_FILES][660];
	char dir[600], line[200];
	struct stat st[UPDATE_FILES];
	int want[UPDATE_FILES], i, mine = 0, kept = 1;

	if (!update_can_install(msg, n))
		return -1;
	dir_of(g_self, dir, sizeof dir);
	for (i = 0; i < u->nfiles; i++) {
		snprintf(dst[i], sizeof dst[i], "%s/%s", dir, u->files[i].name);
		snprintf(tmp[i], sizeof tmp[i], "%s/.%s.new", dir,
			u->files[i].name);
		/* (only the programs this copy has: a manx-only install
		 * doesn't gain an xmanx) */
		want[i] = stat(dst[i], &st[i]) == 0 && S_ISREG(st[i].st_mode);
		if (strcmp(u->files[i].name, base_of(g_self)) == 0)
			mine = want[i];
	}
	if (!mine) {
		snprintf(msg, n, "Manx %s has no %s for %s: get it from %s.",
			u->version, base_of(g_self), update_platform(),
			UPDATE_PAGE);
		return -1;
	}
	/* everything first, checked; only then is anything replaced */
	for (i = 0; i < u->nfiles; i++) {
		if (!want[i])
			continue;
		snprintf(line, sizeof line, "Getting %s %s", u->files[i].name,
			u->version);
		if (status)
			status(ctx, line);
		if (download(&u->files[i], tmp[i], msg, n, status, ctx) < 0) {
			while (--i >= 0)
				if (want[i])
					unlink(tmp[i]);
			return -1;
		}
		chmod(tmp[i], st[i].st_mode & 07777);
	}
	for (i = 0; i < u->nfiles; i++) {
		char old[700];

		if (!want[i])
			continue;
		snprintf(old, sizeof old, "%.640s.old", dst[i]);
		unlink(old);
		if (link(dst[i], old) < 0)
			kept = 0;
		if (rename(tmp[i], dst[i]) < 0) {
			snprintf(msg, n, "Can't put the new %s in place in %.300s.",
				u->files[i].name, dir);
			for (; i < u->nfiles; i++)
				if (want[i])
					unlink(tmp[i]);
			return -1;
		}
	}
	snprintf(msg, n, "Manx %s is installed in %.300s%s: quit, and start "
		"it again to use it.", u->version, dir,
		kept ? " (each old program kept as NAME.old)" : "");
	return 0;
}

/* --- the daily check --- */

static pid_t g_bg_pid;
static char g_known[32];
static int g_known_read;

/* $MANX_HOME/update: "checked TIME" and "version V" (the newest found) */
static void read_state(long *checked, char *ver, size_t vn)
{
	char path[600], line[100];
	FILE *f;

	*checked = 0;
	ver[0] = '\0';
	if (!os_datapath(path, sizeof path, "update")
		|| (f = fopen(path, "r")) == NULL)
		return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strncmp(line, "checked ", 8) == 0)
			*checked = atol(line + 8);
		else if (strncmp(line, "version ", 8) == 0)
			snprintf(ver, vn, "%.30s", line + 8);
	}
	fclose(f);
}

static void write_state(long checked, const char *ver)
{
	char path[600], text[100];

	if (!os_datapath(path, sizeof path, "update"))
		return;
	snprintf(text, sizeof text, "checked %ld\nversion %.30s\n", checked,
		ver);
	os_write_file(path, text, strlen(text), 0644);
}

const char *update_known(void)
{
	if (!g_known_read) {
		long checked;
		char v[32];

		read_state(&checked, v, sizeof v);
		g_known[0] = '\0';
		if (v[0] && update_vercmp(v, update_version()) > 0)
			snprintf(g_known, sizeof g_known, "%s", v);
		g_known_read = 1;
	}
	return g_known;
}

void update_bg_start(void)
{
	static struct update_info u;
	long checked, now = (long)time(NULL);
	char v[32], err[300];
	pid_t pid;
	int r;

	/* (mogrix's copy: its repository has a release when it has it, so
	 * news of GitHub's would only puzzle) */
	if (!config_bool("update_check", strcmp(update_packaged(), "tdnf") != 0))
		return;
	read_state(&checked, v, sizeof v);
	if (checked && now >= checked && now - checked < 86400L)
		return;
	/* stamped now: a check that fails (no network) waits a day too,
	 * and two Manxes started together don't both ask */
	write_state(now, v);
	if ((pid = fork()) != 0) {
		g_bg_pid = pid > 0 ? pid : 0;
		return;
	}
	/* the child: its own random state, no T425 (the parent's), and
	 * behind whatever the user is doing */
	{
		unsigned long mix[2];

		mix[0] = (unsigned long)getpid();
		mix[1] = os_usec();
		entropy_add(mix, sizeof mix, 0);
	}
	tpoff_config(NULL, NULL, 1);
	nice(10);
	r = update_check(NULL, &u, err, sizeof err, NULL, NULL);
	if (r >= 0)
		write_state(now, r > 0 ? u.version : "");
	_exit(r > 0 ? 3 : 0);
}

int update_bg_poll(void)
{
	int st;

	if (g_bg_pid <= 0 || waitpid(g_bg_pid, &st, WNOHANG) != g_bg_pid)
		return 0;
	g_bg_pid = 0;
	g_known_read = 0;
	/* (exit status 3: a newer one) */
	return (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 3
		&& update_known()[0] != '\0';
}

#else	/* MANX_HELIOS: no processes to spare, no links */

int update_install(const struct update_info *u, char *msg, size_t n,
	void (*status)(void *ctx, const char *msg), void *ctx)
{
	update_can_install(msg, n);
	return -1;
}

void update_bg_start(void)
{
}

int update_bg_poll(void)
{
	return 0;
}

const char *update_known(void)
{
	return "";
}

#endif
