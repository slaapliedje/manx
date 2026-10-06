/*
 * tls_none.c - the TLS interface for a build without TLS (Helios): every
 * https connection fails with TLS_ERR_NO_TLS, and the entropy pool (which
 * only TLS draws on) does nothing.
 */
#include <string.h>
#include "tls.h"
#include "entropy.h"

int tls_init(const char *pem_path, void (*note)(const char *msg))
{
	(void)pem_path; (void)note;
	return 0;
}

int tls_build_roots(const char *pem_path)
{
	(void)pem_path;
	return -1;
}

int tls_learn_pem(const char *pem_path, void (*note)(const char *msg))
{
	(void)pem_path; (void)note;
	return 0;
}

int tls_connect(struct tls_conn *c, int fd, const char *host, unsigned port,
	int profile, int early)
{
	(void)fd; (void)port; (void)profile; (void)early;
	memset(c, 0, sizeof *c);
	strncpy(c->host, host, sizeof c->host - 1);
	return TLS_ERR_NO_TLS;
}

int tls_verify(struct tls_conn *c) { (void)c; return TLS_ERR_NO_TLS; }
int tls_read(struct tls_conn *c, void *buf, size_t len) { (void)c; (void)buf; (void)len; return -1; }
int tls_write(struct tls_conn *c, const void *buf, size_t len) { (void)c; (void)buf; (void)len; return -1; }
void tls_close(struct tls_conn *c) { (void)c; }
int tls_retry_full(int err) { (void)err; return 0; }
int tls_host_profile(const char *host) { (void)host; return TLS_FAST; }
void tls_host_set_profile(const char *host, int profile) { (void)host; (void)profile; }
const char *tls_profile_name(int profile) { (void)profile; return "none"; }

const char *tls_strerror(int err)
{
	return err == TLS_ERR_NO_TLS ? "https needs TLS, which this build lacks"
		: "TLS error";
}

/* entropy.h */
static struct entropy_report no_report;

void entropy_init(const char *seed_path) { (void)seed_path; }
void entropy_add(const void *data, size_t len, int bits) { (void)data; (void)len; (void)bits; }
void entropy_event(void) { }
int entropy_bits(void) { return 0; }
int entropy_ready(void) { return 0; }
int entropy_extract(unsigned char out[32]) { memset(out, 0, 32); return -1; }
void entropy_save(void) { }
const struct entropy_report *entropy_report(void) { return &no_report; }
