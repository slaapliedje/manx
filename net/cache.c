/*
 * cache.c - see cache.h.
 *
 * A file is named by a 32-bit FNV-1a hash of its key; its head repeats
 * the key, so a collision is just a miss. Writes go to a temporary file
 * renamed into place on commit. When the files pass the limit, the
 * oldest go.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include "os.h"
#include "cookie.h"
#include "cache.h"

#define MAGIC	"UBC1"

static char s_dir[512];
static long s_max;
static FILE *s_w;
static char s_wkey_path[600], s_wtmp[620];
static long s_wsize;
static int s_wbad;

void cache_init(const char *dir, long max_bytes)
{
	s_dir[0] = '\0';
	if (dir == NULL || strlen(dir) + 20 >= sizeof s_dir || max_bytes <= 0)
		return;
	mkdir(dir, 0700);
	strcpy(s_dir, dir);
	s_max = max_bytes;
}

static void path_of(const char *key, char *out, size_t n)
{
	unsigned long h = 2166136261UL;
	const unsigned char *p;

	for (p = (const unsigned char *)key; *p; p++) {
		h ^= *p;
		h = (h * 16777619UL) & 0xFFFFFFFFUL;
	}
	snprintf(out, n, "%s/%08lx", s_dir, h);
}

/* the head: "MAGIC\n" then "name value" lines, then an empty line */
static int read_head(FILE *f, const char *key, struct cache_meta *m)
{
	char line[2200];

	memset(m, 0, sizeof *m);
	if (fgets(line, sizeof line, f) == NULL || strncmp(line, MAGIC, 4))
		return -1;
	while (fgets(line, sizeof line, f)) {
		char *v;

		line[strcspn(line, "\n")] = '\0';
		if (line[0] == '\0')
			break;
		v = strchr(line, ' ');
		v = v ? v + 1 : line + strlen(line);
		if (strncmp(line, "url ", 4) == 0)
			snprintf(m->url, sizeof m->url, "%s", v);
		else if (strncmp(line, "loc ", 4) == 0)
			snprintf(m->location, sizeof m->location, "%s", v);
		else if (strncmp(line, "type ", 5) == 0)
			snprintf(m->type, sizeof m->type, "%s", v);
		else if (strncmp(line, "charset ", 8) == 0)
			snprintf(m->charset, sizeof m->charset, "%s", v);
		else if (strncmp(line, "stored ", 7) == 0)
			m->stored = atol(v);
		else if (strncmp(line, "fresh ", 6) == 0)
			m->fresh_until = atol(v);
		else if (strncmp(line, "etag ", 5) == 0)
			snprintf(m->etag, sizeof m->etag, "%s", v);
		else if (strncmp(line, "lastmod ", 8) == 0)
			snprintf(m->last_modified, sizeof m->last_modified, "%s", v);
		else if (strncmp(line, "size ", 5) == 0)
			m->size = atol(v);
	}
	return strcmp(m->url, key) == 0 ? 0 : -1;
}

int cache_lookup(const char *key, struct cache_meta *m)
{
	char path[600];
	FILE *f;
	int rc;

	if (!s_dir[0])
		return 0;
	path_of(key, path, sizeof path);
	if ((f = fopen(path, "rb")) == NULL)
		return 0;
	rc = read_head(f, key, m);
	fclose(f);
	return rc == 0;
}

int cache_read(const char *key, struct cache_meta *m,
	int (*body)(void *ctx, const unsigned char *d, size_t n), void *ctx)
{
	static unsigned char buf[4096];
	char path[600];
	FILE *f;
	size_t n;
	long got = 0;

	if (!s_dir[0])
		return -1;
	path_of(key, path, sizeof path);
	if ((f = fopen(path, "rb")) == NULL)
		return -1;
	if (read_head(f, key, m) < 0) {
		fclose(f);
		return -1;
	}
	/* all of the body there? (checked before any of it is handed on) */
	{
		long at = ftell(f);

		if (at < 0 || fseek(f, 0L, SEEK_END) != 0
			|| ftell(f) - at != m->size || fseek(f, at, SEEK_SET) != 0) {
			fclose(f);
			return -1;
		}
	}
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
		got += (long)n;
		if (body(ctx, buf, n) < 0)
			break;
	}
	fclose(f);
	return 0;
}

int cache_begin(const char *key)
{
	if (!s_dir[0])
		return -1;
	if (s_w)
		cache_abort();
	path_of(key, s_wkey_path, sizeof s_wkey_path);
	snprintf(s_wtmp, sizeof s_wtmp, "%s.tmp", s_wkey_path);
	s_w = fopen(s_wtmp, "wb");
	s_wsize = 0;
	s_wbad = 0;
	return s_w ? 0 : -1;
}

void cache_write(const unsigned char *d, size_t n)
{
	if (s_w == NULL || s_wbad)
		return;
	if ((long)n + s_wsize > s_max / 2 || fwrite(d, 1, n, s_w) != n)
		s_wbad = 1;		/* too big to be worth keeping */
	s_wsize += (long)n;
}

void cache_abort(void)
{
	if (s_w) {
		fclose(s_w);
		remove(s_wtmp);
		s_w = NULL;
	}
}

/* over the limit: the oldest files go */
static void trim(void)
{
	DIR *d = opendir(s_dir);
	struct dirent *e;
	long total = 0;
	char path[600], oldest[600];
	long otime;
	int rounds;

	if (d == NULL)
		return;
	for (rounds = 0; rounds < 1000; rounds++) {
		struct stat st;

		total = 0;
		otime = 0;
		oldest[0] = '\0';
		rewinddir(d);
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.' || strlen(e->d_name) != 8)
				continue;
			snprintf(path, sizeof path, "%s/%s", s_dir, e->d_name);
			if (stat(path, &st) < 0)
				continue;
			total += (long)st.st_size;
			if (!oldest[0] || (long)st.st_mtime < otime) {
				otime = (long)st.st_mtime;
				strcpy(oldest, path);
			}
		}
		if (total <= s_max || !oldest[0])
			break;
		remove(oldest);
	}
	closedir(d);
}

void cache_commit(const struct cache_meta *m)
{
	FILE *f;
	char head[5200];
	int n;

	if (s_w == NULL)
		return;
	if (s_wbad) {
		cache_abort();
		return;
	}
	if (fclose(s_w) != 0) {
		s_w = NULL;
		remove(s_wtmp);
		return;
	}
	s_w = NULL;
	/* the head first, then the body copied after it */
	n = snprintf(head, sizeof head, MAGIC "\nurl %s\nloc %s\ntype %s\n"
		"charset %s\nstored %ld\nfresh %ld\netag %s\nlastmod %s\n"
		"size %ld\n\n", m->url, m->location[0] ? m->location : m->url,
		m->type, m->charset, m->stored, m->fresh_until, m->etag,
		m->last_modified, s_wsize);
	if (n < 0 || n >= (int)sizeof head) {
		remove(s_wtmp);
		return;
	}
	{
		char final_tmp[620];
		static unsigned char buf[4096];
		FILE *in = fopen(s_wtmp, "rb");
		size_t k;
		int ok = 1;

		snprintf(final_tmp, sizeof final_tmp, "%s.new", s_wkey_path);
		f = fopen(final_tmp, "wb");
		if (in == NULL || f == NULL) {
			if (in)
				fclose(in);
			if (f)
				fclose(f);
			remove(s_wtmp);
			return;
		}
		chmod(final_tmp, 0600);
		if (fwrite(head, 1, (size_t)n, f) != (size_t)n)
			ok = 0;
		while (ok && (k = fread(buf, 1, sizeof buf, in)) > 0)
			if (fwrite(buf, 1, k, f) != k)
				ok = 0;
		fclose(in);
		if (fclose(f) != 0)
			ok = 0;
		remove(s_wtmp);
		if (!ok || rename(final_tmp, s_wkey_path) != 0) {
			remove(final_tmp);
			return;
		}
	}
	trim();
}

void cache_remove(const char *key)
{
	char path[600];

	if (!s_dir[0])
		return;
	path_of(key, path, sizeof path);
	remove(path);
}

/* does Cache-Control have this directive? (value in *v if given) */
static int directive(const char *cc, const char *name, long *v)
{
	size_t n = strlen(name);

	while (cc && *cc) {
		while (*cc == ' ' || *cc == ',')
			cc++;
		if (strncmp(cc, name, n) == 0 && (cc[n] == '\0' || cc[n] == ','
			|| cc[n] == ' ' || cc[n] == '=')) {
			if (v && cc[n] == '=')
				*v = atol(cc + n + 1 + (cc[n + 1] == '"'));
			return 1;
		}
		cc = strchr(cc, ',');
	}
	return 0;
}

long cache_freshness(const char *cache_control, const char *expires,
	const char *date, long now, int *no_store)
{
	long age = -1, t, d;

	*no_store = cache_control && directive(cache_control, "no-store", NULL);
	/* (private is fine: this cache has one user) */
	if (cache_control && directive(cache_control, "no-cache", NULL))
		return 0;
	if (cache_control && directive(cache_control, "max-age", &age))
		return age > 0 ? now + age : 0;
	if (expires && (t = cookie_parse_date(expires)) > 0) {
		/* relative to the server's clock, if it says what it is */
		d = date ? cookie_parse_date(date) : -1;
		if (d > 0)
			t = now + (t - d);
		return t > now ? t : 0;
	}
	return 0;
}
