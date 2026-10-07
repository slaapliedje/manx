/* test_dns - the resolver's cache: fresh answers without asking, expired
 * ones when no server answers, too old ones forgotten, answers saved. */
#define _POSIX_C_SOURCE 200809L	/* (mkdtemp) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dns.h"

static int fails;

static void want(const char *host, int rc, const char *ip)
{
	unsigned char a[4];
	char got[20];
	int r = dns_resolve(host, a);

	sprintf(got, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
	if (r != rc || (rc == DNS_OK && strcmp(got, ip) != 0)) {
		printf("FAIL %s: got %d %s, want %d %s\n", host, r,
			r == DNS_OK ? got : "", rc, ip);
		fails++;
	}
}

int main(void)
{
	char dir[] = "/tmp/test_dnsXXXXXX", hosts[64], resolv[64], cache[64];
	FILE *f;
	long now = (long)time(NULL);
	char line[200];
	int saved = 0;

	if (mkdtemp(dir) == NULL)
		return 1;
	sprintf(hosts, "%s/hosts", dir);
	sprintf(resolv, "%s/resolv.conf", dir);
	sprintf(cache, "%s/dns", dir);
	f = fopen(hosts, "w");
	fprintf(f, "10.1.1.1 local.example\n");
	fclose(f);
	f = fopen(resolv, "w");		/* no server: nothing can be asked */
	fprintf(f, "# none\n");
	fclose(f);
	f = fopen(cache, "w");
	fprintf(f, "%ld 1.2.3.4 fresh.example\n", now + 1000);
	fprintf(f, "%ld 5.6.7.8 stale.example\n", now - 100);
	fprintf(f, "%ld 9.9.9.9 ancient.example\n", now - 8L * 86400);
	fprintf(f, "garbage line\n");
	fclose(f);

	dns_set_files(hosts, resolv);
	dns_cache_init(cache);
	want("fresh.example", DNS_OK, "1.2.3.4");
	want("stale.example", DNS_OK, "5.6.7.8");	/* (no server answered) */
	want("ancient.example", DNS_NOSERVER, "");
	want("unknown.example", DNS_NOSERVER, "");
	want("local.example", DNS_OK, "10.1.1.1");
	want("192.0.2.7", DNS_OK, "192.0.2.7");

	/* a second program run starts from the same file */
	dns_cache_init(cache);
	want("fresh.example", DNS_OK, "1.2.3.4");

	/* nothing new was learned: the file is as it was written */
	f = fopen(cache, "r");
	while (f && fgets(line, sizeof line, f))
		saved += strstr(line, "fresh.example") != NULL;
	if (f)
		fclose(f);
	if (saved != 1) {
		printf("FAIL the cache file lost fresh.example\n");
		fails++;
	}
	unlink(hosts);
	unlink(resolv);
	unlink(cache);
	rmdir(dir);
	printf("test_dns: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
