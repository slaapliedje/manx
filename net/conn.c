/*
 * conn.c - see conn.h.
 */
#include <stdio.h>
#include <string.h>
#include "os.h"
#include "sock.h"
#include "dns.h"
#include "tcp.h"
#include "conn.h"

#define POOL_MAX	3		/* idle connections kept */
#define IDLE_MAX_MS	60000		/* servers drop idle ones long before */
#define READ_TIMEOUT_MS	30000

static struct conn *s_pool[POOL_MAX];

static void conn_free(struct conn *c)
{
	if (c->is_tls && c->tls) {
		tls_close(c->tls);
		xfree(c->tls);
	}
	if (c->fd >= 0)
		tcp_close(c->fd);
	xfree(c);
}

/* an idle connection the server hasn't closed (nothing to read on it) */
static int still_alive(struct conn *c)
{
	struct pollfd pfd;

	if (os_msec() - c->idle_since > IDLE_MAX_MS)
		return 0;
	pfd.fd = c->fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	return poll(&pfd, 1, 0) == 0;
}

static struct conn *from_pool(const char *host, unsigned port, int is_tls)
{
	int i;

	for (i = 0; i < POOL_MAX; i++) {
		struct conn *c = s_pool[i];

		if (c == NULL)
			continue;
		if (!still_alive(c)) {
			s_pool[i] = NULL;
			conn_free(c);
			continue;
		}
		if (c->port == port && c->is_tls == is_tls
			&& strcmp(c->host, host) == 0) {
			s_pool[i] = NULL;
			c->in_pool = 0;
			c->reused = 1;
			return c;
		}
	}
	return NULL;
}

static int tcp_open(struct conn *c, const unsigned char ip[4], char *err,
	size_t errlen)
{
	unsigned long t0 = os_msec();

	c->fd = tcp_connect(ip, c->port);
	c->t_connect = os_msec() - t0;
	if (c->fd < 0) {
		snprintf(err, errlen, "can't connect to %s:%u", c->host, c->port);
		return -1;
	}
	return 0;
}

struct conn *conn_open(const char *host, unsigned port, int is_tls,
	char *err, size_t errlen)
{
	struct conn *c;
	unsigned char ip[4];
	unsigned long t0;
	int rc, profile;

	if ((c = from_pool(host, port, is_tls)) != NULL)
		return c;
	if (strlen(host) >= sizeof c->host) {
		snprintf(err, errlen, "host name too long");
		return NULL;
	}
	c = xmalloc(sizeof *c);
	if (c == NULL) {
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	memset(c, 0, sizeof *c);
	c->fd = -1;
	strcpy(c->host, host);
	c->port = port;
	c->is_tls = is_tls;

	t0 = os_msec();
	rc = dns_resolve(host, ip);
	c->t_dns = os_msec() - t0;
	if (rc != DNS_OK) {
		snprintf(err, errlen, "%s: %s", host, dns_strerror(rc));
		conn_free(c);
		return NULL;
	}
	if (tcp_open(c, ip, err, errlen) < 0) {
		conn_free(c);
		return NULL;
	}
	if (!is_tls)
		return c;

	c->tls = xmalloc(sizeof *c->tls);
	if (c->tls == NULL) {
		snprintf(err, errlen, "out of memory for TLS");
		conn_free(c);
		return NULL;
	}
	profile = tls_host_profile(host);
	rc = tls_connect(c->tls, c->fd, host, port, profile);
	if (rc != 0 && profile == TLS_FAST && tls_retry_full(rc)) {
		/* the server refused the cheap offer: once more with everything */
		tcp_close(c->fd);
		if (tcp_open(c, ip, err, errlen) < 0) {
			conn_free(c);
			return NULL;
		}
		rc = tls_connect(c->tls, c->fd, host, port, TLS_FULL);
		if (rc == 0)
			tls_host_needs_full(host);
	}
	if (rc != 0) {
		snprintf(err, errlen, "%s: %s", host, tls_strerror(rc));
		conn_free(c);
		return NULL;
	}
	return c;
}

int conn_read(struct conn *c, void *buf, size_t len)
{
	if (c->pend_len > 0) {
		size_t n = len < c->pend_len ? len : c->pend_len;

		memcpy(buf, c->pend, n);
		memmove(c->pend, c->pend + n, c->pend_len - n);
		c->pend_len -= n;
		return (int)n;
	}
	if (c->is_tls)
		return tls_read(c->tls, buf, len);
	return tcp_read(c->fd, buf, len, READ_TIMEOUT_MS);
}

void conn_unread(struct conn *c, const void *buf, size_t len)
{
	if (len > sizeof c->pend - c->pend_len)
		len = sizeof c->pend - c->pend_len;
	memmove(c->pend + len, c->pend, c->pend_len);
	memcpy(c->pend, buf, len);
	c->pend_len += len;
}

int conn_write(struct conn *c, const void *buf, size_t len)
{
	if (c->is_tls)
		return tls_write(c->tls, buf, len);
	return tcp_write_all(c->fd, buf, len);
}

void conn_release(struct conn *c, int reusable)
{
	int i, oldest = 0;

	if (c == NULL)
		return;
	if (!reusable || c->pend_len > 0) {
		conn_free(c);
		return;
	}
	c->idle_since = os_msec();
	c->in_pool = 1;
	for (i = 0; i < POOL_MAX; i++) {
		if (s_pool[i] == NULL) {
			s_pool[i] = c;
			return;
		}
		if (s_pool[i]->idle_since < s_pool[oldest]->idle_since)
			oldest = i;
	}
	conn_free(s_pool[oldest]);
	s_pool[oldest] = c;
}

void conn_close_all(void)
{
	int i;

	for (i = 0; i < POOL_MAX; i++)
		if (s_pool[i]) {
			conn_free(s_pool[i]);
			s_pool[i] = NULL;
		}
}
