/*
 * poll.h - poll() for Helios, whose BSD library has select() only
 * (poll.c). Enough for Manx: POLLIN/POLLOUT on sockets and the X
 * connection.
 */
#ifndef MANX_HELIOS_POLL_H
#define MANX_HELIOS_POLL_H

#define POLLIN		0x0001
#define POLLPRI		0x0002
#define POLLOUT		0x0004
#define POLLERR		0x0008
#define POLLHUP		0x0010
#define POLLNVAL	0x0020

struct pollfd {
	int fd;
	short events;
	short revents;
};

int poll(struct pollfd *fds, unsigned long nfds, int timeout_ms);

#endif /* MANX_HELIOS_POLL_H */
