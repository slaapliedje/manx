/*
 * sites.c - see sites.h.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "os.h"
#include "url.h"
#include "cookie.h"
#include "sites.h"

static struct {
	char *host, *value;
} s_agent[SITES_MAX];
static int s_n;

int sites_match(const char *pattern, const char *host)
{
	size_t p, h;

	if (pattern[0] != '.')
		return strcmp(pattern, host) == 0;
	/* .example.com: example.com and all under it */
	if (strcmp(pattern + 1, host) == 0)
		return 1;
	p = strlen(pattern);
	h = strlen(host);
	return h > p && strcmp(host + h - p, pattern) == 0;
}

/* a cookie for host (or .domain), as the site would have set it */
static void add_cookie(const char *host, const char *value)
{
	static struct url u;
	static char url[URL_HOST_MAX + 16], hdr[2048];

	snprintf(url, sizeof url, "https://%.255s/",
		host[0] == '.' ? host + 1 : host);
	if (url_parse(url, &u) != URL_OK || !strchr(value, '='))
		return;
	snprintf(hdr, sizeof hdr, "%s; Path=/; Secure%s%s", value,
		host[0] == '.' ? "; Domain=" : "", host[0] == '.' ? host + 1 : "");
	cookie_set(&u, hdr, (long)time(NULL));
}

void sites_load(const char *path)
{
	char line[2048];
	FILE *f;

	if (path == NULL || (f = fopen(path, "r")) == NULL)
		return;
	while (fgets(line, sizeof line, f)) {
		char *host = line, *key, *v, *e;

		while (*host == ' ' || *host == '\t')
			host++;
		if (*host == '#' || *host == '\0' || *host == '\n')
			continue;
		key = host + strcspn(host, " \t\r\n");
		if (*key == '\0')
			continue;
		*key++ = '\0';
		key += strspn(key, " \t");
		v = key + strcspn(key, " \t\r\n");
		if (*v == '\0')
			continue;
		*v++ = '\0';
		v += strspn(v, " \t");
		e = v + strlen(v);
		while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '
			|| e[-1] == '\t'))
			*--e = '\0';
		for (e = host; *e; e++)
			if (*e >= 'A' && *e <= 'Z')
				*e = (char)(*e + 32);
		if (!*v)
			continue;
		if (strcmp(key, "agent") == 0 && s_n < SITES_MAX) {
			s_agent[s_n].host = xstrdup(host);
			s_agent[s_n].value = xstrdup(v);
			if (s_agent[s_n].host && s_agent[s_n].value)
				s_n++;
		} else if (strcmp(key, "cookie") == 0)
			add_cookie(host, v);
	}
	fclose(f);
}

const char *sites_agent(const char *host)
{
	int i;

	for (i = 0; i < s_n; i++)
		if (sites_match(s_agent[i].host, host))
			return s_agent[i].value;
	return NULL;
}
