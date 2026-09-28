/*
 * config.c - see config.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "config.h"

#define MAX_KEYS	64

static struct {
	char *key, *value;
} s_kv[MAX_KEYS];
static int s_n;

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n'
		|| e[-1] == '\r'))
		*--e = '\0';
	return s;
}

int config_load(const char *path, const char **err)
{
	static char msg[160];
	char line[1024];
	FILE *f;
	int n = 0, rc = 0;

	if (path == NULL || (f = fopen(path, "r")) == NULL)
		return 0;
	while (fgets(line, sizeof line, f)) {
		char *k, *v, *eq;
		int i;

		n++;
		k = trim(line);
		if (*k == '\0' || *k == '#')
			continue;
		if ((eq = strchr(k, '=')) == NULL) {
			snprintf(msg, sizeof msg, "%s line %d: no '='", path, n);
			*err = msg;
			rc = -1;
			continue;
		}
		*eq = '\0';
		k = trim(k);
		v = trim(eq + 1);
		for (i = 0; k[i]; i++)
			if (k[i] >= 'A' && k[i] <= 'Z')
				k[i] = (char)(k[i] + 32);
		for (i = 0; i < s_n; i++)
			if (strcmp(s_kv[i].key, k) == 0)
				break;
		if (i == MAX_KEYS)
			continue;
		if (i == s_n) {
			s_kv[i].key = xstrdup(k);
			s_n++;
		} else
			xfree(s_kv[i].value);
		s_kv[i].value = xstrdup(v);
	}
	fclose(f);
	return rc;
}

const char *config_str(const char *key, const char *def)
{
	char env[64];
	const char *e;
	int i;

	/* UB_KEY in the environment first */
	if (strlen(key) + 4 < sizeof env) {
		strcpy(env, "UB_");
		for (i = 0; key[i]; i++)
			env[3 + i] = key[i] >= 'a' && key[i] <= 'z' ?
				(char)(key[i] - 32) : key[i];
		env[3 + i] = '\0';
		if ((e = getenv(env)) != NULL)
			return e;
	}
	for (i = 0; i < s_n; i++)
		if (strcmp(s_kv[i].key, key) == 0)
			return s_kv[i].value;
	return def;
}

int config_bool(const char *key, int def)
{
	const char *v = config_str(key, NULL);

	if (v == NULL || !*v)
		return def;
	if (strcmp(v, "on") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "1") == 0
		|| strcmp(v, "true") == 0)
		return 1;
	if (strcmp(v, "off") == 0 || strcmp(v, "no") == 0 || strcmp(v, "0") == 0
		|| strcmp(v, "false") == 0)
		return 0;
	return def;
}

long config_long(const char *key, long def)
{
	const char *v = config_str(key, NULL);
	char *end;
	long n;

	if (v == NULL || !*v)
		return def;
	n = strtol(v, &end, 10);
	return *end ? def : n;
}
