/* dns.h - IPv4 name resolution (hosts file, then DNS over UDP). */
#ifndef MANX_DNS_H
#define MANX_DNS_H

enum { DNS_OK = 0, DNS_NOTFOUND = -1, DNS_TIMEOUT = -2, DNS_NOSERVER = -3 };

/* Resolve host (a name or a dotted quad) to an IPv4 address in network
 * order. Blocks for at most a few seconds per name server. */
int dns_resolve(const char *host, unsigned char ip[4]);
const char *dns_strerror(int err);

/* Keep answers in path too (os_datapath "dns"), and start from what it
 * has; without this call they are kept in memory only. */
void dns_cache_init(const char *path);

/* Other hosts and resolv.conf files than the system's (tests). */
void dns_set_files(const char *hosts, const char *resolv);

#endif /* MANX_DNS_H */
