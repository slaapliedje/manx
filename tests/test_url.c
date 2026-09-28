/* test_url - URL parsing and RFC 3986 section 5.4 resolution examples. */
#include <stdio.h>
#include <string.h>
#include "url.h"

static int fails;

static void resolve(const char *base, const char *ref, const char *want)
{
	struct url b, t;
	char got[URL_MAX];

	if (url_parse(base, &b) != URL_OK || url_resolve(&b, ref, &t) != URL_OK
		|| url_format(&t, got, sizeof got, 1) != URL_OK)
		strcpy(got, "(error)");
	if (strcmp(got, want) != 0) {
		printf("FAIL resolve(%s, %s): got %s, want %s\n", base, ref, got, want);
		fails++;
	}
}

static void parse(const char *s, const char *host, unsigned port,
	const char *target)
{
	struct url u;
	char t[URL_MAX];

	if (url_parse(s, &u) != URL_OK || url_target(&u, t, sizeof t) != URL_OK) {
		printf("FAIL parse(%s): error\n", s);
		fails++;
		return;
	}
	if (strcmp(u.host, host) || url_port(&u) != port || strcmp(t, target)) {
		printf("FAIL parse(%s): host %s port %u target %s\n", s, u.host,
			url_port(&u), t);
		fails++;
	}
}

int main(void)
{
	static const char *normal[][2] = {
		{ "g:h", "g:h" }, { "g", "http://a/b/c/g" }, { "./g", "http://a/b/c/g" },
		{ "g/", "http://a/b/c/g/" }, { "/g", "http://a/g" }, { "//g", "http://g" },
		{ "?y", "http://a/b/c/d;p?y" }, { "g?y", "http://a/b/c/g?y" },
		{ "#s", "http://a/b/c/d;p?q#s" }, { "g#s", "http://a/b/c/g#s" },
		{ "g?y#s", "http://a/b/c/g?y#s" }, { ";x", "http://a/b/c/;x" },
		{ "g;x", "http://a/b/c/g;x" }, { "g;x?y#s", "http://a/b/c/g;x?y#s" },
		{ "", "http://a/b/c/d;p?q" }, { ".", "http://a/b/c/" },
		{ "./", "http://a/b/c/" }, { "..", "http://a/b/" }, { "../", "http://a/b/" },
		{ "../g", "http://a/b/g" }, { "../..", "http://a/" }, { "../../", "http://a/" },
		{ "../../g", "http://a/g" },
		/* abnormal */
		{ "../../../g", "http://a/g" }, { "../../../../g", "http://a/g" },
		{ "/./g", "http://a/g" }, { "/../g", "http://a/g" }, { "g.", "http://a/b/c/g." },
		{ ".g", "http://a/b/c/.g" }, { "g..", "http://a/b/c/g.." },
		{ "..g", "http://a/b/c/..g" }, { "./../g", "http://a/b/g" },
		{ "./g/.", "http://a/b/c/g/" }, { "g/./h", "http://a/b/c/g/h" },
		{ "g/../h", "http://a/b/c/h" }, { "g;x=1/./y", "http://a/b/c/g;x=1/y" },
		{ "g;x=1/../y", "http://a/b/c/y" }, { "g?y/./x", "http://a/b/c/g?y/./x" },
		{ "g?y/../x", "http://a/b/c/g?y/../x" }, { "g#s/./x", "http://a/b/c/g#s/./x" },
		{ "g#s/../x", "http://a/b/c/g#s/../x" }, { "http:g", "http:g" },
	};
	size_t i;

	for (i = 0; i < sizeof normal / sizeof normal[0]; i++)
		resolve("http://a/b/c/d;p?q", normal[i][0], normal[i][1]);

	/* browser-style clean-up and everyday cases */
	resolve("http://a/b/c", "  \tpage 2.html\n", "http://a/b/page%202.html");
	resolve("https://Example.COM:8443/x/", "y?z=1", "https://example.com:8443/x/y?z=1");
	resolve("http://a/", "https://b/c", "https://b/c");
	resolve("http://a", "x", "http://a/x");

	parse("http://example.com", "example.com", 80, "/");
	parse("https://user:pw@Example.com:444/p?q=1#f", "example.com", 444, "/p?q=1");
	parse("gopher://gopher.floodgap.com/1/world", "gopher.floodgap.com", 70, "/1/world");
	parse("HTTP://[::1]:8080/x", "[::1]", 8080, "/x");
	{
		struct url u;

		if (url_parse("http://a:99999/", &u) != URL_BAD) {
			printf("FAIL: port 99999 accepted\n");
			fails++;
		}
	}
	printf("url: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
