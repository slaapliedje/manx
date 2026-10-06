/*
 * tcp.c - TCP connections for the fetcher.
 *
 * Plain blocking sockets: the connect waits for the kernel's own timeout,
 * reads wait in poll() so a stalled server can't hang the browser.
 */
#include <string.h>
#include "sock.h"
#include "tcp.h"

int tcp_connect(const unsigned char ip[4], unsigned port)
{
	struct sockaddr_in sa;
	int fd, rc;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	memcpy(&sa.sin_addr, ip, 4);
	do
		rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
	while (rc < 0 && (errno == EINTR || errno == ERESTART));
	if (rc < 0) {
		int e = errno;

		close(fd);
		errno = e;
		return -1;
	}
	return fd;
}

int tcp_read(int fd, void *buf, size_t len, int timeout_ms)
{
	for (;;) {
		struct pollfd pfd;
		int n;

		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		n = poll(&pfd, 1, timeout_ms);
		if (n < 0 && SOCK_RETRY(errno))
			continue;
		if (n <= 0)
			return -1;
		n = read(fd, buf, (unsigned)len);
		if (n < 0 && SOCK_RETRY(errno))
			continue;
		return n;
	}
}

int tcp_write_all(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;

	while (len > 0) {
		int n = write(fd, (const char *)p, (unsigned)len);

		if (n < 0) {
			if (SOCK_RETRY(errno))
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

void tcp_close(int fd)
{
	close(fd);
}
