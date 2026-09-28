/*
 * test_cache - store, look up, read back, freshness, collisions, the
 * size limit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "os.h"
#include "cache.h"

static int fails, runs;
static char got[70000];
static size_t got_len;

static int sink(void *ctx, const unsigned char *d, size_t n)
{
	(void)ctx;
	memcpy(got + got_len, d, n);
	got_len += n;
	return 0;
}

static void check(int cond, const char *what)
{
	runs++;
	if (!cond) {
		fails++;
		printf("FAIL %s\n", what);
	}
}

static void put(const char *url, const char *body, size_t n, long fresh)
{
	struct cache_meta m;

	memset(&m, 0, sizeof m);
	snprintf(m.url, sizeof m.url, "%s", url);
	strcpy(m.type, "text/html");
	strcpy(m.charset, "utf-8");
	m.stored = 1000;
	m.fresh_until = fresh;
	strcpy(m.etag, "\"abc\"");
	cache_begin(url);
	cache_write((const unsigned char *)body, n / 2);
	cache_write((const unsigned char *)body + n / 2, n - n / 2);
	cache_commit(&m);
}

int main(void)
{
	char dir[100], big[30000];
	struct cache_meta m;
	int ns, i;
	long f;

	sprintf(dir, "/tmp/ubcache.%d", (int)getpid());
	cache_init(dir, 100000);
	put("http://a.test/", "<p>hello", 8, 5000);
	check(cache_lookup("http://a.test/", &m) && m.fresh_until == 5000
		&& strcmp(m.etag, "\"abc\"") == 0 && m.size == 8, "lookup");
	got_len = 0;
	check(cache_read("http://a.test/", &m, sink, NULL) == 0 && got_len == 8
		&& memcmp(got, "<p>hello", 8) == 0, "read back");
	check(!cache_lookup("http://b.test/", &m), "miss");
	/* the limit: 100000 bytes; files of 30000 push the oldest out */
	memset(big, 'x', sizeof big);
	for (i = 0; i < 6; i++) {
		char u[40];

		sprintf(u, "http://big.test/%d", i);
		put(u, big, sizeof big, 0);
		sleep(1);	/* (file times have 1 s resolution) */
	}
	check(!cache_lookup("http://a.test/", &m), "oldest evicted");
	check(cache_lookup("http://big.test/5", &m), "newest kept");
	/* more than half the cache: not kept */
	{
		static char huge[60000];

		put("http://huge.test/", huge, sizeof huge, 0);
		check(!cache_lookup("http://huge.test/", &m), "too big skipped");
	}
	cache_remove("http://big.test/5");
	check(!cache_lookup("http://big.test/5", &m), "removed");
	/* freshness */
	f = cache_freshness("max-age=60", NULL, NULL, 1000, &ns);
	check(f == 1060 && !ns, "max-age");
	f = cache_freshness("no-store", NULL, NULL, 1000, &ns);
	check(f == 0 && ns, "no-store");
	f = cache_freshness("private, max-age=0", NULL, NULL, 1000, &ns);
	check(f == 0 && !ns, "max-age=0");
	f = cache_freshness(NULL, "Thu, 01 Jan 1970 01:00:00 GMT",
		"Thu, 01 Jan 1970 00:30:00 GMT", 5000, &ns);
	check(f == 5000 + 1800, "expires relative to date");
	f = cache_freshness("no-cache, max-age=600", NULL, NULL, 1000, &ns);
	check(f == 0, "no-cache");
	{
		char cmd[140];

		sprintf(cmd, "rm -rf %s", dir);
		(void)!system(cmd);
	}
	printf("cache: %d/%d passed\n", runs - fails, runs);
	return fails != 0;
}
