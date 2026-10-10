/*
 * cookie.c - see cookie.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "os.h"
#include "cookie.h"

int cookie_enabled = 1;

struct cookie {
	char *name, *value, *domain, *path;
	long expires;			/* 0: a session cookie */
	unsigned long order;		/* creation, for the header's order */
	unsigned char host_only, secure, http_only;
};

static struct cookie s_jar[COOKIE_MAX];
static int s_n;
static unsigned long s_bytes, s_order;
static char s_path[512];
static int s_dirty;

/* --- dates ------------------------------------------------------------ */

/* days from 1970-01-01 to y-m-d (proleptic Gregorian) */
static long days_from_civil(long y, int m, int d)
{
	long era, yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

/* RFC 6265 5.1.1: tokens anywhere, in any order */
long cookie_parse_date(const char *s)
{
	static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
	int have_time = 0, have_day = 0, have_month = 0, have_year = 0;
	int hh = 0, mm = 0, ss = 0, day = 0, month = 0;
	long year = 0;

	while (*s) {
		char tok[32];
		size_t n = 0;

		/* delimiters: everything but digits, letters and : */
		while (*s && !isalnum((unsigned char)*s) && *s != ':')
			s++;
		while (*s && (isalnum((unsigned char)*s) || *s == ':')) {
			if (n < sizeof tok - 1)
				tok[n++] = *s;
			s++;
		}
		tok[n] = '\0';
		if (n == 0)
			break;
		if (!have_time && sscanf(tok, "%d:%d:%d", &hh, &mm, &ss) == 3) {
			have_time = 1;
			continue;
		}
		if (!have_day && isdigit((unsigned char)tok[0]) && n <= 2
			&& !have_month) {
			day = atoi(tok);
			have_day = 1;
			continue;
		}
		if (!have_month && n >= 3 && isalpha((unsigned char)tok[0])) {
			char m3[4];
			const char *p;
			int i;

			for (i = 0; i < 3; i++)
				m3[i] = (char)tolower((unsigned char)tok[i]);
			m3[3] = '\0';
			if ((p = strstr(months, m3)) != NULL && (p - months) % 3 == 0) {
				month = (int)((p - months) / 3) + 1;
				have_month = 1;
			}
			continue;
		}
		if (!have_day && isdigit((unsigned char)tok[0]) && n <= 2) {
			day = atoi(tok);
			have_day = 1;
			continue;
		}
		if (!have_year && isdigit((unsigned char)tok[0]) && n >= 2
			&& n <= 4) {
			year = atol(tok);
			have_year = 1;
			continue;
		}
	}
	if (!have_time || !have_day || !have_month || !have_year)
		return -1;
	if (year >= 70 && year <= 99)
		year += 1900;
	else if (year < 70)
		year += 2000;
	if (day < 1 || day > 31 || year < 1601 || hh > 23 || mm > 59 || ss > 59)
		return -1;
	/* (a 32-bit long holds 1970 to January 2038: before, the past;
	 * after, the latest it can say) */
	if (year < 1970)
		return 0;
	if (year > 2037)
		return COOKIE_TIME_MAX;
	return days_from_civil(year, month, day) * 86400L + hh * 3600L
		+ mm * 60L + ss;
}

long cookie_time_add(long t, long secs)
{
	if (secs > 0 && t > COOKIE_TIME_MAX - secs)
		return COOKIE_TIME_MAX;
	return t + secs;
}

/* --- matching --------------------------------------------------------- */

/* host is domain, or ends with "." domain (and isn't an IP address) */
static int domain_match(const char *host, const char *domain)
{
	size_t h = strlen(host), d = strlen(domain);
	const char *p;

	if (strcmp(host, domain) == 0)
		return 1;
	if (h <= d || host[h - d - 1] != '.' || strcmp(host + h - d, domain))
		return 0;
	for (p = host; *p; p++)
		if (!isdigit((unsigned char)*p) && *p != '.')
			return 1;
	return 0;			/* an IP address */
}

static int path_match(const char *req, const char *cp)
{
	size_t n = strlen(cp);

	if (strcmp(req, cp) == 0)
		return 1;
	if (strncmp(req, cp, n) != 0)
		return 0;
	return cp[n - 1] == '/' || req[n] == '/';
}

/* too broad to set a cookie for: a top-level domain or a registry's
 * second level. (Not the whole public suffix list, which is 200 KB:
 * the common ones.) */
static int public_suffix(const char *d)
{
	static const char *const list[] = {
		"ac.uk", "co.uk", "gov.uk", "ltd.uk", "me.uk", "net.uk",
		"org.uk", "plc.uk", "sch.uk", "com.au", "net.au", "org.au",
		"edu.au", "gov.au", "co.nz", "net.nz", "org.nz", "co.jp",
		"ne.jp", "or.jp", "ac.jp", "go.jp", "com.br", "net.br",
		"org.br", "com.cn", "net.cn", "org.cn", "co.in", "net.in",
		"org.in", "co.za", "org.za", "com.mx", "com.tr", "com.ar",
		"com.sg", "com.hk", "com.tw", "co.kr", "or.kr", "co.il",
		"com.pl", "co.at", "or.at", "com.ua", "com.ru", "blogspot.com",
		"github.io", "gitlab.io", "herokuapp.com", "appspot.com",
		"neocities.org", "netlify.app", "pages.dev", "vercel.app",
	};
	size_t i;

	if (strchr(d, '.') == NULL)
		return 1;
	for (i = 0; i < sizeof list / sizeof list[0]; i++)
		if (strcmp(d, list[i]) == 0)
			return 1;
	return 0;
}

/* --- the jar ---------------------------------------------------------- */

static void drop(int i)
{
	struct cookie *c = &s_jar[i];

	s_bytes -= strlen(c->name) + strlen(c->value);
	if (c->expires)
		s_dirty = 1;
	xfree(c->name);
	xfree(c->value);
	xfree(c->domain);
	xfree(c->path);
	s_jar[i] = s_jar[--s_n];
}

static void expire(long now)
{
	int i;

	for (i = 0; i < s_n; )
		if (s_jar[i].expires && s_jar[i].expires <= now)
			drop(i);
		else
			i++;
}

/* room for one more of the given size: expired first, then the oldest */
static int make_room(long now, size_t bytes)
{
	expire(now);
	while (s_n && (s_n == COOKIE_MAX || s_bytes + bytes > COOKIE_BYTES_MAX)) {
		int i, old = 0;

		for (i = 1; i < s_n; i++)
			if (s_jar[i].order < s_jar[old].order)
				old = i;
		drop(old);
	}
	return s_n < COOKIE_MAX && s_bytes + bytes <= COOKIE_BYTES_MAX;
}

static void store(const char *name, const char *value, const char *domain,
	const char *path, long expires, int host_only, int secure,
	int http_only, long now)
{
	struct cookie *c;
	unsigned long order = 0;
	int i;

	/* the same name, domain and path replaces the old one (which keeps
	 * its place in the order, RFC 6265 5.3 step 11) */
	for (i = 0; i < s_n; i++) {
		c = &s_jar[i];
		if (strcmp(c->name, name) == 0 && strcmp(c->domain, domain) == 0
			&& strcmp(c->path, path) == 0) {
			order = c->order;
			drop(i);
			break;
		}
	}
	if (expires && expires <= now)
		return;			/* a deletion */
	if (!make_room(now, strlen(name) + strlen(value)))
		return;
	c = &s_jar[s_n];
	c->name = xstrdup(name);
	c->value = xstrdup(value);
	c->domain = xstrdup(domain);
	c->path = xstrdup(path);
	if (!c->name || !c->value || !c->domain || !c->path) {
		xfree(c->name);
		xfree(c->value);
		xfree(c->domain);
		xfree(c->path);
		return;
	}
	c->expires = expires;
	c->order = order ? order : ++s_order;
	c->host_only = (unsigned char)host_only;
	c->secure = (unsigned char)secure;
	c->http_only = (unsigned char)http_only;
	s_bytes += strlen(name) + strlen(value);
	s_n++;
	if (expires)
		s_dirty = 1;
}

static char *strip(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
	return s;
}

void cookie_set(const struct url *u, const char *header, long now)
{
	static char buf[8192], domain[URL_HOST_MAX], path[1024];
	char *p, *attr, *name, *value, *eq;
	long expires = 0, max_age = 0;
	int have_max_age = 0, secure = 0, http_only = 0, host_only = 1, i;

	if (!cookie_enabled || strlen(header) >= sizeof buf)
		return;
	strcpy(buf, header);
	/* name=value up to the first ; */
	p = strchr(buf, ';');
	if (p)
		*p++ = '\0';
	if ((eq = strchr(buf, '=')) == NULL)
		return;
	*eq = '\0';
	name = strip(buf);
	value = strip(eq + 1);
	if (!*name || strlen(name) + strlen(value) > 4096)
		return;
	snprintf(domain, sizeof domain, "%s", u->host);
	/* the default path: the request's, up to its last / */
	{
		const char *rp = u->path[0] == '/' ? u->path : "/";
		const char *slash = strrchr(rp, '/');
		size_t n = (size_t)(slash - rp);

		if (n == 0 || n >= sizeof path)
			strcpy(path, "/");
		else {
			memcpy(path, rp, n);
			path[n] = '\0';
		}
	}
	/* the attributes */
	while (p && *p) {
		char *next = strchr(p, ';');

		if (next)
			*next++ = '\0';
		attr = strip(p);
		p = next;
		eq = strchr(attr, '=');
		if (eq)
			*eq = '\0';
		{
			char *v = eq ? strip(eq + 1) : "";
			char *k = strip(attr);

			for (i = 0; k[i]; i++)
				k[i] = (char)tolower((unsigned char)k[i]);
			if (strcmp(k, "expires") == 0) {
				long t = cookie_parse_date(v);

				if (t >= 0)
					expires = t > 0 ? t : 1;
			} else if (strcmp(k, "max-age") == 0) {
				char *end;
				long a = strtol(v, &end, 10);

				if (*v && !*end) {
					max_age = a;
					have_max_age = 1;
				}
			} else if (strcmp(k, "domain") == 0 && *v) {
				if (*v == '.')
					v++;
				for (i = 0; v[i]; i++)
					v[i] = (char)tolower((unsigned char)v[i]);
				if (!domain_match(u->host, v))
					return;		/* not for this host */
				if (public_suffix(v)) {
					if (strcmp(v, u->host) != 0)
						return;
				} else {
					snprintf(domain, sizeof domain, "%s", v);
					host_only = 0;
				}
			} else if (strcmp(k, "path") == 0 && *v == '/') {
				snprintf(path, sizeof path, "%s", v);
			} else if (strcmp(k, "secure") == 0)
				secure = 1;
			else if (strcmp(k, "httponly") == 0)
				http_only = 1;
		}
	}
	/* a Secure cookie only from https */
	if (secure && strcmp(u->scheme, "https") != 0)
		return;
	if (have_max_age)
		expires = max_age <= 0 ? 1 : cookie_time_add(now, max_age);
	store(name, value, domain, path, expires, host_only, secure, http_only,
		now);
}

size_t cookie_header(const struct url *u, long now, char *buf, size_t n)
{
	static int pick[COOKIE_MAX];
	const char *path = u->path[0] ? u->path : "/";
	int https = strcmp(u->scheme, "https") == 0, np = 0, i, j;
	size_t len = 0;

	buf[0] = '\0';
	if (!cookie_enabled || n == 0)
		return 0;
	expire(now);
	for (i = 0; i < s_n; i++) {
		const struct cookie *c = &s_jar[i];

		if ((c->host_only ? strcmp(u->host, c->domain) == 0
			: domain_match(u->host, c->domain))
			&& path_match(path, c->path) && (!c->secure || https))
			pick[np++] = i;
	}
	/* longer paths first, then older (RFC 6265 5.4) */
	for (i = 1; i < np; i++)
		for (j = i; j > 0; j--) {
			const struct cookie *a = &s_jar[pick[j - 1]], *b = &s_jar[pick[j]];
			size_t la = strlen(a->path), lb = strlen(b->path);

			if (la > lb || (la == lb && a->order < b->order))
				break;
			{
				int t = pick[j];

				pick[j] = pick[j - 1];
				pick[j - 1] = t;
			}
		}
	for (i = 0; i < np; i++) {
		const struct cookie *c = &s_jar[pick[i]];
		size_t k = strlen(c->name) + 1 + strlen(c->value) + (len ? 2 : 0);

		if (len + k + 1 > n)
			break;
		len += (size_t)sprintf(buf + len, "%s%s=%s", len ? "; " : "",
			c->name, c->value);
	}
	return len;
}

int cookie_count(void)
{
	return s_n;
}

void cookie_clear(void)
{
	while (s_n)
		drop(s_n - 1);
	s_dirty = 1;
}

/* --- the file ----------------------------------------------------------- */

/* one cookie a line: domain host-only path secure http-only expires name
 * value, tab-separated */
void cookie_init(const char *path)
{
	char line[8400];
	FILE *f;

	if (path == NULL || strlen(path) >= sizeof s_path)
		return;
	strcpy(s_path, path);
	if ((f = fopen(path, "r")) == NULL)
		return;
	while (fgets(line, sizeof line, f)) {
		char *fld[8], *p = line;
		int k;

		line[strcspn(line, "\r\n")] = '\0';
		for (k = 0; k < 8 && p; k++) {
			fld[k] = p;
			p = strchr(p, '\t');
			if (p)
				*p++ = '\0';
		}
		if (k < 8 || line[0] == '#')
			continue;
		store(fld[6], fld[7], fld[0], fld[2], atol(fld[5]),
			fld[1][0] == '1', fld[3][0] == '1', fld[4][0] == '1', 0);
	}
	fclose(f);
	s_dirty = 0;
}

void cookie_save(void)
{
	/* (allocated for the moment: 280 KB, too much to keep on the T425
	 * or a 4 MB 68030) */
	size_t cap = COOKIE_BYTES_MAX + COOKIE_MAX * 600, len = 0;
	char *buf;
	int i;

	if (!s_dirty || !s_path[0] || (buf = xmalloc(cap)) == NULL)
		return;
	len += (size_t)sprintf(buf, "# manx cookies: domain host-only path secure "
		"http-only expires name value\n");
	for (i = 0; i < s_n; i++) {
		const struct cookie *c = &s_jar[i];

		if (!c->expires)
			continue;	/* session cookies aren't kept */
		if (len + strlen(c->domain) + strlen(c->path) + strlen(c->name)
			+ strlen(c->value) + 40 > cap)
			break;
		len += (size_t)sprintf(buf + len, "%s\t%d\t%s\t%d\t%d\t%ld\t%s\t%s\n",
			c->domain, c->host_only, c->path, c->secure, c->http_only,
			c->expires, c->name, c->value);
	}
	if (os_write_file(s_path, buf, len, 0600) == 0)
		s_dirty = 0;
	xfree(buf);
}
