/*
 * sock.h - BSD sockets and poll on every target.
 *
 * AMIX's headers define the socket structures and constants but declare
 * none of the functions (libsocket.a has them), so they are declared here.
 */
#ifndef UB_SOCK_H
#define UB_SOCK_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <errno.h>

#ifdef UB_SYSV4
int socket(int, int, int);
int connect(int, const struct sockaddr *, int);
int sendto(int, const void *, int, int, const struct sockaddr *, int);
int recvfrom(int, void *, int, int, struct sockaddr *, int *);
int setsockopt(int, int, int, const void *, int);
int shutdown(int, int);
int poll(struct pollfd *, unsigned long, int);
int close(int);
int read(int, void *, unsigned);
int write(int, const void *, unsigned);
#else
#include <unistd.h>
#endif

/* SVR4 system calls interrupted by a signal can leak ERESTART (and AMIX
 * programs under ASV's amx module see it): treat it like EINTR */
#ifndef ERESTART
#define ERESTART	91
#endif
#define SOCK_RETRY(e)	((e) == EINTR || (e) == ERESTART || (e) == EAGAIN)

#endif /* UB_SOCK_H */
