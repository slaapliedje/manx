/*
 * tls.h for Helios: Manx's TLS interface without BearSSL. Helios C has no
 * 64-bit integers, which BearSSL needs, so this build speaks plain HTTP
 * (and Gopher) and refuses https with a clear message (tls_none.c). It
 * stands in for tls/tls.h, which the Helios build leaves off the include
 * path.
 */
#ifndef MANX_TLS_H
#define MANX_TLS_H

#include <stddef.h>

enum { TLS_FAST = 0, TLS_FULL_X = 1, TLS_FULL = 2 };

struct tls_info {
	int profile;
	int resumed;
	int leaf_memo;
	int learned;
	unsigned version, suite;
	unsigned long t_handshake, t_verify;	/* ms */
	int pre_jobs, pre_t425, pre_used;
};

struct tls_conn {
	int fd;
	int open;
	int verify_pending;
	int verify_err;
	char host[256];
	unsigned port;
	struct tls_info info;
};

enum {
	TLS_ERR_NOT_SEEDED = 1001,
	TLS_ERR_NO_ANCHORS = 1002,
	TLS_ERR_NO_TLS = 1003		/* this build has no TLS */
};

int tls_init(const char *pem_path, void (*note)(const char *msg));
int tls_build_roots(const char *pem_path);
int tls_learn_pem(const char *pem_path, void (*note)(const char *msg));
int tls_connect(struct tls_conn *c, int fd, const char *host, unsigned port,
	int profile, int early);
int tls_verify(struct tls_conn *c);
int tls_read(struct tls_conn *c, void *buf, size_t len);
int tls_write(struct tls_conn *c, const void *buf, size_t len);
void tls_close(struct tls_conn *c);
int tls_retry_full(int err);
int tls_host_profile(const char *host);
void tls_host_set_profile(const char *host, int profile);
const char *tls_profile_name(int profile);
const char *tls_strerror(int err);

#endif /* MANX_TLS_H */
