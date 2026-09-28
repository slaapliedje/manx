/*
 * fetch.h - get a document by URL: http, https, gopher, file.
 */
#ifndef UB_FETCH_H
#define UB_FETCH_H

#include <stddef.h>
#include "url.h"

#define FETCH_MAX_REDIRECTS	10

/* what the fetch delivers, as it goes */
struct fetch_cb {
	void *ctx;
	/* progress, for a status line ("Connecting to ...", "Verifying ..."):
	 * may be NULL */
	void (*status)(void *ctx, const char *msg);
	/* the final response's head is known: may be NULL */
	void (*head)(void *ctx, int status, const char *content_type,
		const char *charset, const char *url);
	/* body bytes of the final response; return -1 to stop */
	int (*body)(void *ctx, const unsigned char *data, size_t len);
	/* each response header of the final response: may be NULL */
	void (*header)(void *ctx, const char *name, const char *value);
};

struct fetch_result {
	int status;			/* HTTP status; 200 for gopher/file */
	char url[URL_MAX];		/* the final URL, after redirects */
	char content_type[128];
	char charset[32];
	long body_bytes;
	int redirects;
	/* for the curious (ufetch -v): of the last connection */
	int tls, tls_resumed, tls_leaf_memo, tls_learned, tls_profile, reused;
	unsigned tls_suite;
	unsigned long t_dns, t_connect, t_handshake, t_verify, t_first, t_body;
	char error[200];		/* set when fetch fails */
};

/* 0, or -1 with res->error set. method: "GET" or "HEAD". */
int fetch(const char *url, const char *method, const struct fetch_cb *cb,
	struct fetch_result *res);

#endif /* UB_FETCH_H */
