/* trust.h - trust anchors for certificate validation. */
#ifndef UB_TRUST_H
#define UB_TRUST_H

#include <stddef.h>
#include "bearssl.h"

struct trust_store {
	br_x509_trust_anchor *ta;
	size_t n, cap;
	size_t skipped;		/* certificates that could not be used */
};

/* Add every certificate of a PEM bundle; 0, or -1 on a read/parse or
 * memory error (anchors loaded before the error are kept). */
int trust_load_pem(struct trust_store *ts, const char *path);

void trust_free(struct trust_store *ts);

#endif /* UB_TRUST_H */
