/*
 * poll.c - poll() on top of select() for Helios. Its select() takes plain
 * int bit masks (one word: descriptors 0-31), as its sys/socket.h says.
 */
#include <sys/types.h>
#include <sys/time.h>
#include "poll.h"

extern int select(int nfds, int *readfds, int *writefds, int *exceptfds,
	struct timeval *tv);

int poll(struct pollfd *fds, unsigned long nfds, int timeout_ms)
{
	int rmask = 0, wmask = 0, emask = 0, maxfd = -1, n, ready = 0;
	unsigned long i;
	struct timeval tv, *tvp = 0;

	for (i = 0; i < nfds; i++) {
		int fd = fds[i].fd;

		fds[i].revents = 0;
		if (fd < 0)
			continue;
		if (fd > 31) {
			fds[i].revents = POLLNVAL;
			ready++;
			continue;
		}
		if (fds[i].events & (POLLIN | POLLPRI))
			rmask |= 1 << fd;
		if (fds[i].events & POLLOUT)
			wmask |= 1 << fd;
		emask |= 1 << fd;
		if (fd > maxfd)
			maxfd = fd;
	}
	if (ready)
		return ready;
	if (timeout_ms >= 0) {
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000L;
		tvp = &tv;
	}
	n = select(maxfd + 1, &rmask, &wmask, &emask, tvp);
	if (n <= 0)
		return n;
	for (i = 0; i < nfds; i++) {
		int fd = fds[i].fd;

		if (fd < 0 || fd > 31)
			continue;
		if (rmask & (1 << fd))
			fds[i].revents |= fds[i].events & (POLLIN | POLLPRI);
		if (wmask & (1 << fd))
			fds[i].revents |= fds[i].events & POLLOUT;
		if (emask & (1 << fd))
			fds[i].revents |= POLLERR;
		if (fds[i].revents)
			ready++;
	}
	return ready;
}
