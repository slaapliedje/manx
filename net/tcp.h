/* tcp.h - blocking TCP connections with timeouts on reads. */
#ifndef MANX_TCP_H
#define MANX_TCP_H

#include <stddef.h>

/* Connect to ip:port; returns a descriptor or -1. */
int tcp_connect(const unsigned char ip[4], unsigned port);

/* Read at most len bytes, waiting up to timeout_ms: > 0 bytes read,
 * 0 end of stream, -1 error or timeout. */
int tcp_read(int fd, void *buf, size_t len, int timeout_ms);

/* Write all of buf: 0 or -1. */
int tcp_write_all(int fd, const void *buf, size_t len);

void tcp_close(int fd);

#endif /* MANX_TCP_H */
