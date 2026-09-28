/*
 * test_cookie - the cookie jar: setting, matching, ordering, expiry,
 * limits, the file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "os.h"
#include "url.h"
#include "cookie.h"

static int fails, runs;
static long now = 1790000000L;		/* 2026 */

static void set(const char *url, const char *h)
{
	static struct url u;

	url_parse(url, &u);
	cookie_set(&u, h, now);
}

static void want(const char *url, const char *expect)
{
	static struct url u;
	char buf[2048];

	url_parse(url, &u);
	cookie_header(&u, now, buf, sizeof buf);
	runs++;
	if (strcmp(buf, expect) != 0) {
		fails++;
		printf("FAIL %s: want \"%s\", got \"%s\"\n", url, expect, buf);
	}
}

static void date(const char *s, long expect)
{
	long got = cookie_parse_date(s);

	runs++;
	if (got != expect) {
		fails++;
		printf("FAIL date \"%s\": want %ld, got %ld\n", s, expect, got);
	}
}

int main(void)
{
	char path[256];
	int i;

	date("Wed, 21 Oct 2015 07:28:00 GMT", 1445412480L);
	date("Wednesday, 21-Oct-15 07:28:00 GMT", 1445412480L);
	date("Wed Oct 21 07:28:00 2015", 1445412480L);
	date("Thu, 01 Jan 1970 00:00:00 GMT", 0L);
	date("21 Oct 2015", -1L);		/* no time */
	date("nonsense", -1L);

	/* host-only by default, sent to that host only */
	set("http://example.com/", "a=1");
	want("http://example.com/x", "a=1");
	want("http://www.example.com/", "");
	/* a Domain cookie goes to subdomains */
	set("http://www.example.org/", "b=2; Domain=.example.org");
	want("http://example.org/", "b=2");
	want("http://news.example.org/", "b=2");
	want("http://badexample.org/", "");
	/* not for someone else's domain, nor a public suffix */
	set("http://www.example.org/", "c=3; Domain=other.org");
	set("http://www.example.co.uk/", "d=4; Domain=co.uk");
	set("http://www.example.co.uk/", "e=5; Domain=com");
	want("http://other.org/", "");
	want("http://foo.co.uk/", "");
	/* paths: the default is the request's directory; longer paths first */
	set("http://p.test/dir/page.html", "p1=x");
	set("http://p.test/", "p0=y; Path=/");
	set("http://p.test/", "p2=z; Path=/dir/sub");
	want("http://p.test/dir/sub/x", "p2=z; p1=x; p0=y");
	want("http://p.test/dir", "p1=x; p0=y");
	want("http://p.test/directory", "p0=y");
	want("http://p.test/", "p0=y");
	/* Secure: only from and to https */
	set("http://s.test/", "s1=1; Secure");
	set("https://s.test/", "s2=2; Secure; HttpOnly");
	want("http://s.test/", "");
	want("https://s.test/", "s2=2");
	/* replacing keeps the order; expiry deletes */
	set("http://r.test/", "x=1");
	set("http://r.test/", "y=2");
	set("http://r.test/", "x=3");
	want("http://r.test/", "x=3; y=2");
	set("http://r.test/", "x=; Max-Age=0");
	want("http://r.test/", "y=2");
	set("http://r.test/", "y=; Expires=Thu, 01 Jan 1970 00:00:01 GMT");
	want("http://r.test/", "");
	set("http://r.test/", "t=1; Max-Age=60");
	want("http://r.test/", "t=1");
	now += 61;
	want("http://r.test/", "");
	/* the limit: the oldest go */
	for (i = 0; i < COOKIE_MAX + 20; i++) {
		char h[40];

		sprintf(h, "n%d=v", i);
		set("http://many.test/", h);
	}
	runs++;
	if (cookie_count() > COOKIE_MAX) {
		fails++;
		printf("FAIL %d cookies kept\n", cookie_count());
	}
	/* the file: persistent ones survive, session ones don't */
	cookie_clear();
	sprintf(path, "/tmp/ubcookies.%d", (int)getpid());
	cookie_init(path);
	set("https://f.test/", "keep=1; Max-Age=3600; Secure");
	set("https://f.test/", "session=1");
	cookie_save();
	cookie_clear();
	cookie_init(path);
	want("https://f.test/", "keep=1");
	remove(path);
	cookie_enabled = 0;
	want("https://f.test/", "");
	printf("cookie: %d/%d passed\n", runs - fails, runs);
	return fails != 0;
}
