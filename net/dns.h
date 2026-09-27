/* dns.h - IPv4 name resolution (hosts file, then DNS over UDP). */
#ifndef UB_DNS_H
#define UB_DNS_H

enum { DNS_OK = 0, DNS_NOTFOUND = -1, DNS_TIMEOUT = -2, DNS_NOSERVER = -3 };

/* Resolve host (a name or a dotted quad) to an IPv4 address in network
 * order. Blocks for at most a few seconds per name server. */
int dns_resolve(const char *host, unsigned char ip[4]);
const char *dns_strerror(int err);

#endif /* UB_DNS_H */
