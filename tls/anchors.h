/*
 * anchors.h - trust anchors: root certificates from a PEM bundle, and
 * intermediates this machine has verified, kept in a compact binary file
 * so a 68030 doesn't decode X.509 at every start (the 121-root bundle
 * takes 52 s to decode on the TT; its binary form loads in well under a
 * second).
 */
#ifndef UB_ANCHORS_H
#define UB_ANCHORS_H

#include <stddef.h>
#include "bearssl.h"

struct anchors {
	br_x509_trust_anchor *ta;
	unsigned long *notafter;	/* per anchor: expiry in BearSSL days, 0 none */
	size_t n, cap;
	size_t skipped;			/* certificates that could not be used */
};

void anchors_init(struct anchors *a);
void anchors_free(struct anchors *a);

/*
 * Add one DER certificate as an anchor (subject DN, public key, CA flag,
 * expiry). Duplicates (same DN and key) are not added again.
 * 1 added, 0 duplicate/unusable (counted in skipped), -1 out of memory.
 */
int anchors_add_der(struct anchors *a, const unsigned char *der, size_t len);

/* Every certificate of a PEM bundle. 0, or -1 on a read/parse/memory
 * error (anchors added before it are kept). */
int anchors_load_pem(struct anchors *a, const char *path);

/*
 * The binary form. The header records the size and modification time of
 * the PEM file it was made from (0 for learned intermediates), so a stale
 * cache is noticed. Expired anchors are left out when loading.
 */
int anchors_save(const struct anchors *a, const char *path, long src_size,
	long src_mtime);
int anchors_load(struct anchors *a, const char *path, long *src_size,
	long *src_mtime);

/* Copy anchor i of src into dst (for merging sets): as anchors_add_der. */
int anchors_add_copy(struct anchors *dst, const struct anchors *src, size_t i);

#endif /* UB_ANCHORS_H */
