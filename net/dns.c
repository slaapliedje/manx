/*
 * dns.c - a small stub resolver: IPv4 addresses for host names.
 *
 * A static AMIX program cannot use the system's resolver (it lives in
 * shared objects only), so the browser asks the name servers itself:
 * dotted quads are parsed, /etc/hosts is searched, then each nameserver of
 * /etc/resolv.conf gets a recursive A query over UDP. A CNAME without an
 * address in the same answer is followed with a new query.
 *
 * Answers are remembered for their time to live (at least TTL_MIN: a
 * lookup over a slow link costs more than a stale address risks), in
 * memory and, with dns_cache_init, in a file that the next program run
 * starts from. When no server answers, an expired answer still serves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "sock.h"
#include "os.h"
#include "dns.h"

#ifdef MANX_HELIOS
#define HOSTS_FILE	"/helios/etc/hosts"
#define RESOLV_FILE	"/helios/etc/resolv.conf"
#else
#define HOSTS_FILE	"/etc/hosts"
#define RESOLV_FILE	"/etc/resolv.conf"
#endif

#define MAXNS		3
#define TRY_MS		1000	/* the first wait for an answer, then doubled */
#define SENDS		2	/* sends of a query per server and round */
#define ROUNDS		2
#define MAXCNAME	6

#define CACHE_MAX	64		/* names remembered */
#define NAME_MAX_LEN	96		/* (longer names aren't) */
#define TTL_MIN		600L		/* seconds, whatever the answer said */
#define TTL_MAX		86400L
#define STALE_MAX	(7L * 86400)	/* past expiry, kept for when asking fails */

struct dns_entry {
	char name[NAME_MAX_LEN];
	unsigned char ip[4];
	long expires;			/* time(), seconds */
};

/* the system's files (dns_set_files: a test's own) */
static const char *s_hosts = HOSTS_FILE, *s_resolv = RESOLV_FILE;

static struct dns_entry s_cache[CACHE_MAX];
static int s_ncache;
static char s_cache_path[600];		/* "": memory only */

static int parse_quad(const char *s, unsigned char ip[4])
{
	int i;

	for (i = 0; i < 4; i++) {
		unsigned v = 0;
		int digits = 0;

		while (*s >= '0' && *s <= '9' && digits < 4) {
			v = v * 10 + (unsigned)(*s++ - '0');
			digits++;
		}
		if (digits == 0 || v > 255)
			return 0;
		ip[i] = (unsigned char)v;
		if (i < 3 && *s++ != '.')
			return 0;
	}
	return *s == '\0';
}

static int name_eq(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);

	/* a trailing dot marks a fully qualified name; ignore it */
	if (la && a[la - 1] == '.') la--;
	if (lb && b[lb - 1] == '.') lb--;
	if (la != lb)
		return 0;
	while (la--) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
	return 1;
}

/* /etc/hosts: "address name aliases..." with # comments */
static int from_hosts(const char *name, unsigned char ip[4])
{
	FILE *f = fopen(s_hosts, "r");
	char line[256];
	int found = 0;

	if (f == NULL)
		return 0;
	while (!found && fgets(line, sizeof line, f)) {
		char *p = strchr(line, '#'), *tok, *addr;

		if (p)
			*p = '\0';
		addr = strtok(line, " \t\r\n");
		if (addr == NULL || !parse_quad(addr, ip))
			continue;
		while ((tok = strtok(NULL, " \t\r\n")) != NULL)
			if (name_eq(tok, name)) {
				found = 1;
				break;
			}
	}
	fclose(f);
	return found;
}

static int nameservers(unsigned char ns[MAXNS][4])
{
	FILE *f = fopen(s_resolv, "r");
	char line[256];
	int n = 0;

	if (f == NULL)
		return 0;
	while (n < MAXNS && fgets(line, sizeof line, f)) {
		char *k = strtok(line, " \t\r\n"), *v = strtok(NULL, " \t\r\n");

		if (k && v && strcmp(k, "nameserver") == 0 && parse_quad(v, ns[n]))
			n++;
	}
	fclose(f);
	return n;
}

/* query id: needs to be unguessable enough to resist blind spoofing */
static unsigned short next_id(void)
{
	static unsigned long x;

	x = x * 1103515245UL + 12345UL + os_usec();
	return (unsigned short)(x >> 8);
}

static size_t build_query(unsigned char *q, size_t cap, const char *name,
	unsigned short id)
{
	size_t n = 12;
	const char *p = name;

	memset(q, 0, 12);
	q[0] = (unsigned char)(id >> 8);
	q[1] = (unsigned char)id;
	q[2] = 0x01;			/* recursion desired */
	q[5] = 1;			/* one question */
	while (*p) {
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);

		if (len == 0 || len > 63 || n + len + 1 + 5 > cap)
			return 0;
		q[n++] = (unsigned char)len;
		memcpy(q + n, p, len);
		n += len;
		p += len;
		if (*p == '.')
			p++;
	}
	q[n++] = 0;
	q[n++] = 0; q[n++] = 1;		/* type A */
	q[n++] = 0; q[n++] = 1;		/* class IN */
	return n;
}

/* skip a (possibly compressed) name at off; returns the offset after it */
static size_t skip_name(const unsigned char *m, size_t len, size_t off)
{
	while (off < len) {
		unsigned c = m[off];

		if (c == 0)
			return off + 1;
		if ((c & 0xC0) == 0xC0)
			return off + 2;
		off += c + 1;
	}
	return len + 1;
}

/* read a (possibly compressed) name at off into out as dotted text */
static int read_name(const unsigned char *m, size_t len, size_t off,
	char *out, size_t cap)
{
	size_t n = 0;
	int hops = 0;

	while (off < len) {
		unsigned c = m[off];

		if (c == 0) {
			out[n ? n - 1 : 0] = '\0';
			return 1;
		}
		if ((c & 0xC0) == 0xC0) {
			if (off + 1 >= len || ++hops > 16)
				return 0;
			off = ((c & 0x3F) << 8) | m[off + 1];
			continue;
		}
		if (off + 1 + c > len || n + c + 1 >= cap)
			return 0;
		memcpy(out + n, m + off + 1, c);
		n += c;
		out[n++] = '.';
		off += c + 1;
	}
	return 0;
}

/*
 * Parse a response: 1 = address found, 2 = only a CNAME (in cname),
 * 0 = no such name / no answer, -1 = malformed or not ours.
 */
static int parse_answer(const unsigned char *m, size_t len, unsigned short id,
	unsigned char ip[4], char *cname, size_t cap, long *ttl)
{
	unsigned qd, an, i;
	size_t off = 12;
	int have_cname = 0;

	if (len < 12 || m[0] != (id >> 8) || m[1] != (id & 0xFF) || !(m[2] & 0x80))
		return -1;
	if ((m[3] & 0x0F) != 0)		/* rcode: NXDOMAIN, SERVFAIL ... */
		return 0;
	qd = (unsigned)m[4] << 8 | m[5];
	an = (unsigned)m[6] << 8 | m[7];
	for (i = 0; i < qd; i++)
		off = skip_name(m, len, off) + 4;
	for (i = 0; i < an && off < len; i++) {
		unsigned type, rdlen;
		long rr_ttl;

		off = skip_name(m, len, off);
		if (off + 10 > len)
			return -1;
		type = (unsigned)m[off] << 8 | m[off + 1];
		rr_ttl = (long)((unsigned long)m[off + 4] << 24
			| (unsigned long)m[off + 5] << 16
			| (unsigned long)m[off + 6] << 8 | m[off + 7]);
		rdlen = (unsigned)m[off + 8] << 8 | m[off + 9];
		off += 10;
		if (off + rdlen > len)
			return -1;
		if (type == 1 && rdlen == 4) {
			memcpy(ip, m + off, 4);
			if (rr_ttl < *ttl)
				*ttl = rr_ttl;
			return 1;
		}
		if (type == 5 && !have_cname) {
			have_cname = read_name(m, len, off, cname, cap);
			if (have_cname && rr_ttl < *ttl)
				*ttl = rr_ttl;
		}
		off += rdlen;
	}
	return have_cname ? 2 : 0;
}

static int query(const unsigned char ns[4], const char *name,
	unsigned char ip[4], char *cname, size_t cap, long *ttl)
{
	unsigned char q[300], r[1024];
	struct sockaddr_in sa;
	unsigned short id = next_id();
	size_t qlen = build_query(q, sizeof q, name, id);
	unsigned long t0;
	long wait;
	int fd, sends, rc = -1;

	if (qlen == 0)
		return 0;
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(53);
	memcpy(&sa.sin_addr, ns, 4);
	/* a lost query (or a slow server, itself asking upstream) is asked
	 * again soon, on the same socket under the same id: whichever answer
	 * comes first will do */
	for (sends = 0, wait = TRY_MS; sends < SENDS && rc < 0; sends++, wait *= 2) {
		if (sendto(fd, (char *)q, (int)qlen, 0, (struct sockaddr *)&sa,
				sizeof sa) < 0)
			break;
		t0 = os_msec();
		for (;;) {
			struct pollfd pfd;
			struct sockaddr_in from;
			int n, fromlen = sizeof from;
			long left = wait - (long)(os_msec() - t0);

			if (left <= 0)
				break;
			pfd.fd = fd;
			pfd.events = POLLIN;
			pfd.revents = 0;
			n = poll(&pfd, 1, (int)left);
			if (n < 0 && SOCK_RETRY(errno))
				continue;
			if (n == 0)
				break;		/* (send again) */
			if (n < 0)
				goto out;
			n = recvfrom(fd, (char *)r, sizeof r, 0, (struct sockaddr *)&from,
				(void *)&fromlen);
			if (n < 0) {
				if (SOCK_RETRY(errno))
					continue;
				goto out;
			}
			/* only the server we asked, and only our id */
			if (memcmp(&from.sin_addr, ns, 4) != 0)
				continue;
			rc = parse_answer(r, (size_t)n, id, ip, cname, cap, ttl);
			if (rc >= 0)
				break;
		}
	}
out:
	close(fd);
	return rc;
}

/* --- the cache ------------------------------------------------------------ */

static struct dns_entry *cache_find(const char *name)
{
	int i;

	for (i = 0; i < s_ncache; i++)
		if (strcmp(s_cache[i].name, name) == 0)
			return &s_cache[i];
	return NULL;
}

/* a new answer: in place of the old one for the name, else of the one
 * that expires first */
static void cache_put(const char *name, const unsigned char ip[4], long expires)
{
	struct dns_entry *e = cache_find(name);
	int i;

	if (strlen(name) >= NAME_MAX_LEN)
		return;
	if (e == NULL) {
		if (s_ncache < CACHE_MAX)
			e = &s_cache[s_ncache++];
		else
			for (e = &s_cache[0], i = 1; i < s_ncache; i++)
				if (s_cache[i].expires < e->expires)
					e = &s_cache[i];
		strcpy(e->name, name);
	}
	memcpy(e->ip, ip, 4);
	e->expires = expires;
}

static void cache_save(void)
{
	char *buf, *p;
	int i;

	if (!s_cache_path[0])
		return;
	buf = xmalloc((size_t)s_ncache * (NAME_MAX_LEN + 32) + 1);
	if (buf == NULL)
		return;
	p = buf;
	for (i = 0; i < s_ncache; i++) {
		const struct dns_entry *e = &s_cache[i];

		sprintf(p, "%ld %u.%u.%u.%u %s\n", e->expires, e->ip[0], e->ip[1],
			e->ip[2], e->ip[3], e->name);
		p += strlen(p);
	}
	os_write_file(s_cache_path, buf, (size_t)(p - buf), 0600);
	xfree(buf);
}

void dns_set_files(const char *hosts, const char *resolv)
{
	s_hosts = hosts;
	s_resolv = resolv;
}

void dns_cache_init(const char *path)
{
	FILE *f;
	char line[NAME_MAX_LEN + 40];
	long now = (long)time(NULL);

	s_cache_path[0] = '\0';
	if (path == NULL || strlen(path) >= sizeof s_cache_path)
		return;
	strcpy(s_cache_path, path);
	if ((f = fopen(path, "r")) == NULL)
		return;
	while (fgets(line, sizeof line, f)) {
		char *t = strtok(line, " \t\r\n"), *q = strtok(NULL, " \t\r\n"),
			*n = strtok(NULL, " \t\r\n");
		unsigned char ip[4];
		long expires;

		if (t == NULL || q == NULL || n == NULL || !parse_quad(q, ip))
			continue;
		expires = atol(t);
		if (expires + STALE_MAX > now)
			cache_put(n, ip, expires);
	}
	fclose(f);
}

/* --- lookups --------------------------------------------------------------- */

int dns_resolve(const char *host, unsigned char ip[4])
{
	unsigned char ns[MAXNS][4];
	char name[256], cname[256];
	struct dns_entry *e;
	long now = (long)time(NULL), ttl = TTL_MAX;
	int nns, hop, rc = -1;

	if (parse_quad(host, ip))
		return DNS_OK;
	if (from_hosts(host, ip))
		return DNS_OK;
	e = cache_find(host);
	if (e && e->expires > now) {
		memcpy(ip, e->ip, 4);
		return DNS_OK;
	}
	nns = nameservers(ns);
	if (strlen(host) >= sizeof name)
		return DNS_NOTFOUND;
	strcpy(name, host);
	for (hop = 0; hop < MAXCNAME && nns > 0; hop++) {
		int round, s;

		rc = -1;
		for (round = 0; round < ROUNDS && rc < 0; round++)
			for (s = 0; s < nns && rc < 0; s++)
				rc = query(ns[s], name, ip, cname, sizeof cname, &ttl);
		if (rc != 2)
			break;
		strcpy(name, cname);	/* CNAME only: ask again */
	}
	if (rc == 1) {
		if (ttl < TTL_MIN)
			ttl = TTL_MIN;
		cache_put(host, ip, now + ttl);
		cache_save();
		return DNS_OK;
	}
	/* no answer (or no server to ask): an old one is better than none */
	if (rc < 0 && e) {
		memcpy(ip, e->ip, 4);
		return DNS_OK;
	}
	if (nns == 0)
		return DNS_NOSERVER;
	return rc < 0 ? DNS_TIMEOUT : DNS_NOTFOUND;
}

const char *dns_strerror(int err)
{
	switch (err) {
	case DNS_OK: return "ok";
	case DNS_NOTFOUND: return "host not found";
	case DNS_TIMEOUT: return "name server not responding";
	case DNS_NOSERVER: return "no nameserver in " RESOLV_FILE;
	}
	return "resolver error";
}
