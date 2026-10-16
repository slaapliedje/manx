/* test_sites - a sites file: its hosts, User-Agents and cookies. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "url.h"
#include "cookie.h"
#include "sites.h"

static int fails;

static void expect(const char *what, int ok)
{
	if (!ok) {
		printf("FAIL %s\n", what);
		fails++;
	}
}

static const char *jar_for(const char *url)
{
	static char buf[1024];
	struct url u;

	url_parse(url, &u);
	cookie_header(&u, (long)time(NULL), buf, sizeof buf);
	return buf;
}

int main(void)
{
	static const char path[] = "build/test/sites.txt";
	const char *a;
	FILE *f;

	expect("exact", sites_match("forums.example.com", "forums.example.com"));
	expect("exact, not another", !sites_match("forums.example.com", "example.com"));
	expect(".domain itself", sites_match(".example.com", "example.com"));
	expect(".domain under it", sites_match(".example.com", "a.b.example.com"));
	expect(".domain not a lookalike", !sites_match(".example.com", "badexample.com"));

	if ((f = fopen(path, "w")) == NULL) {
		printf("FAIL can't write %s\n", path);
		return 1;
	}
	fputs("# a browser's pass through the wall\n"
		"Forums.Example.com  agent  Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0  \n"
		".example.com cookie cf_clearance=abc.def-123\n"
		"\n"
		"other.org\tcookie\tsid=1\r\n"
		"broken-line-without-value agent\n"
		"nocookie.org cookie novalue\n", f);
	fclose(f);
	cookie_clear();
	sites_load(path);
	remove(path);

	a = sites_agent("forums.example.com");
	expect("agent", a && strcmp(a, "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) "
		"Gecko/20100101 Firefox/128.0") == 0);
	expect("no agent elsewhere", sites_agent("example.com") == NULL
		&& sites_agent("broken-line-without-value") == NULL);
	expect("cookie under the domain",
		strcmp(jar_for("https://forums.example.com/topic/1/"), "cf_clearance=abc.def-123") == 0);
	expect("cookie at the domain",
		strcmp(jar_for("https://example.com/"), "cf_clearance=abc.def-123") == 0);
	expect("cookie: https only", jar_for("http://example.com/")[0] == '\0');
	expect("cookie, host only", strcmp(jar_for("https://other.org/x"), "sid=1") == 0
		&& jar_for("https://www.other.org/")[0] == '\0');
	expect("no cookie without =", jar_for("https://nocookie.org/")[0] == '\0');
	printf("sites: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
