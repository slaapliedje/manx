/*
 * xdefer.c - deferred certificate and ServerKeyExchange validation (see
 * xdefer.h). Needs the br_ssl_client_ske_defer hook patched into BearSSL
 * (third_party/patches/bearssl-ske-defer.patch).
 */
#include <string.h>
#include "sigpre.h"
#include "xdefer.h"

/* the handshake in progress; one connection handshakes at a time */
static struct xdefer *g_active;

static void xd_start_chain(const br_x509_class **ctx, const char *server_name)
{
	struct xdefer *xd = (struct xdefer *)ctx;

	xd->len = 0;
	xd->ncert = 0;
	xd->err = 0;
	xd->ske_seen = 0;
	memset(&xd->leaf, 0, sizeof xd->leaf);
	xd->server_name[0] = '\0';
	if (server_name) {
		strncpy(xd->server_name, server_name, sizeof xd->server_name - 1);
		xd->server_name[sizeof xd->server_name - 1] = '\0';
	}
}

static void xd_start_cert(const br_x509_class **ctx, uint32_t length)
{
	struct xdefer *xd = (struct xdefer *)ctx;

	if (xd->err)
		return;
	if (xd->ncert == XDEFER_CERTS_MAX || xd->len + length > sizeof xd->chain) {
		xd->err = BR_ERR_X509_LIMIT_EXCEEDED;
		return;
	}
	xd->cert_off[xd->ncert] = xd->len;
	xd->cert_len[xd->ncert] = length;
	if (xd->ncert == 0)
		br_x509_decoder_init(&xd->dec, 0, 0);
}

static void xd_append(const br_x509_class **ctx, const unsigned char *buf,
	size_t len)
{
	struct xdefer *xd = (struct xdefer *)ctx;

	if (xd->err)
		return;
	if (xd->len + len > sizeof xd->chain) {
		xd->err = BR_ERR_X509_LIMIT_EXCEEDED;
		return;
	}
	memcpy(xd->chain + xd->len, buf, len);
	xd->len += len;
	if (xd->ncert == 0)
		br_x509_decoder_push(&xd->dec, buf, len);
}

static void xd_end_cert(const br_x509_class **ctx)
{
	struct xdefer *xd = (struct xdefer *)ctx;
	br_x509_pkey *pk;

	if (xd->err)
		return;
	if (xd->len - xd->cert_off[xd->ncert] != xd->cert_len[xd->ncert]) {
		xd->err = BR_ERR_X509_INVALID_VALUE;
		return;
	}
	if (xd->ncert == 0) {
		pk = br_x509_decoder_get_pkey(&xd->dec);
		if (pk == NULL) {
			xd->err = br_x509_decoder_last_error(&xd->dec);
			if (xd->err == 0)
				xd->err = BR_ERR_X509_INVALID_VALUE;
			return;
		}
		xd->leaf = *pk;
		if (pk->key_type == BR_KEYTYPE_RSA) {
			if (pk->key.rsa.nlen + pk->key.rsa.elen > sizeof xd->key) {
				xd->err = BR_ERR_X509_LIMIT_EXCEEDED;
				return;
			}
			memcpy(xd->key, pk->key.rsa.n, pk->key.rsa.nlen);
			memcpy(xd->key + pk->key.rsa.nlen, pk->key.rsa.e, pk->key.rsa.elen);
			xd->leaf.key.rsa.n = xd->key;
			xd->leaf.key.rsa.e = xd->key + pk->key.rsa.nlen;
		} else if (pk->key_type == BR_KEYTYPE_EC) {
			if (pk->key.ec.qlen > sizeof xd->key) {
				xd->err = BR_ERR_X509_LIMIT_EXCEEDED;
				return;
			}
			memcpy(xd->key, pk->key.ec.q, pk->key.ec.qlen);
			xd->leaf.key.ec.q = xd->key;
		} else {
			xd->err = BR_ERR_X509_UNSUPPORTED;
			return;
		}
	}
	xd->ncert++;
}

static unsigned xd_end_chain(const br_x509_class **ctx)
{
	struct xdefer *xd = (struct xdefer *)ctx;

	if (xd->err)
		return (unsigned)xd->err;
	if (xd->ncert == 0)
		return BR_ERR_X509_EMPTY_CHAIN;
	return 0;
}

static const br_x509_pkey *xd_get_pkey(const br_x509_class *const *ctx,
	unsigned *usages)
{
	const struct xdefer *xd = (const struct xdefer *)ctx;

	/* the real key usage is checked by xdefer_verify */
	if (usages)
		*usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
	return xd->ncert ? &xd->leaf : NULL;
}

static const br_x509_class xd_vtable = {
	sizeof(struct xdefer),
	xd_start_chain,
	xd_start_cert,
	xd_append,
	xd_end_cert,
	xd_end_chain,
	xd_get_pkey
};

/* the ServerKeyExchange hook: record, check later */
static int xd_ske(br_ssl_client_context *sc, int use_rsa,
	const unsigned char *hash_oid, const unsigned char *hv, size_t hv_len,
	const unsigned char *sig, size_t sig_len)
{
	struct xdefer *xd = g_active;

	(void)sc;
	if (xd == NULL || hv_len > sizeof xd->ske_hv || sig_len > sizeof xd->ske_sig)
		return 0;
	xd->ske_seen = 1;
	xd->ske_rsa = use_rsa;
	xd->ske_oid = hash_oid;		/* points into BearSSL's constant table */
	memcpy(xd->ske_hv, hv, hv_len);
	xd->ske_hvlen = hv_len;
	memcpy(xd->ske_sig, sig, sig_len);
	xd->ske_siglen = sig_len;
	return 1;
}

void xdefer_install(struct xdefer *xd, br_ssl_client_context *sc)
{
	/* nothing may survive from an earlier handshake on this object: a
	 * resumed session sends no certificate, so start_chain never runs
	 * to reset these, and a stale chain would be taken for this one */
	memset(xd, 0, sizeof *xd);
	xd->vtable = &xd_vtable;
	g_active = xd;
	br_ssl_client_ske_defer = xd_ske;
	br_ssl_engine_set_x509(&sc->eng, &xd->vtable);
}

static int verify(struct xdefer *xd, br_ssl_client_context *sc,
	br_x509_minimal_context *xc, int leaf_known_good, int *anchor_at)
{
	const br_x509_class **v = &xc->vtable;
	const br_x509_pkey *pk;
	unsigned usages = 0;
	unsigned err;
	int i;

	if (anchor_at)
		*anchor_at = -1;
	if (xd->err)
		return xd->err;
	if (!xd->ske_seen)
		return BR_ERR_BAD_SIGNATURE;	/* ECDHE suites always sign */

	if (leaf_known_good) {
		/* the leaf was validated for this host before: its key is the
		 * one recorded from the same bytes */
		pk = &xd->leaf;
		goto signature;
	}

	/* 1. the chain, replayed into the real validator; note where it
	 * reached an anchor (x509_minimal sets err to OK there and ignores
	 * the certificates after) */
	(*v)->start_chain(v, xd->server_name[0] ? xd->server_name : NULL);
	for (i = 0; i < xd->ncert; i++) {
		(*v)->start_cert(v, (uint32_t)xd->cert_len[i]);
		(*v)->append(v, xd->chain + xd->cert_off[i], xd->cert_len[i]);
		(*v)->end_cert(v);
		if (anchor_at && *anchor_at < 0 && xc->err == BR_ERR_X509_OK)
			*anchor_at = i;
	}
	err = (*v)->end_chain(v);
	if (err)
		return (int)err;
	pk = (*v)->get_pkey(v, &usages);
	if (pk == NULL || pk->key_type != xd->leaf.key_type
		|| !(usages & BR_KEYTYPE_SIGN))
		return BR_ERR_WRONG_KEY_USAGE;
	/* the handshake ran on the key recorded from the same bytes; make
	 * sure it is the key that was just validated */
	if (pk->key_type == BR_KEYTYPE_RSA
		? pk->key.rsa.nlen != xd->leaf.key.rsa.nlen
			|| pk->key.rsa.elen != xd->leaf.key.rsa.elen
			|| memcmp(pk->key.rsa.n, xd->leaf.key.rsa.n, pk->key.rsa.nlen)
			|| memcmp(pk->key.rsa.e, xd->leaf.key.rsa.e, pk->key.rsa.elen)
		: pk->key.ec.curve != xd->leaf.key.ec.curve
			|| pk->key.ec.qlen != xd->leaf.key.ec.qlen
			|| memcmp(pk->key.ec.q, xd->leaf.key.ec.q, pk->key.ec.qlen))
		return BR_ERR_X509_NOT_TRUSTED;

signature:
	/* 2. the ServerKeyExchange signature, with the validated key */
	if (xd->ske_rsa) {
		unsigned char tmp[64];

		if (pk->key_type != BR_KEYTYPE_RSA
			|| !sc->eng.irsavrfy(xd->ske_sig, xd->ske_siglen,
				xd->ske_oid, xd->ske_hvlen, &pk->key.rsa, tmp)
			|| memcmp(tmp, xd->ske_hv, xd->ske_hvlen) != 0)
			return BR_ERR_BAD_SIGNATURE;
	} else {
		if (pk->key_type != BR_KEYTYPE_EC
			|| !sc->eng.iecdsa(xd->iec ? xd->iec : sc->eng.iec,
				xd->ske_hv, xd->ske_hvlen,
				&pk->key.ec, xd->ske_sig, xd->ske_siglen))
			return BR_ERR_BAD_SIGNATURE;
	}
	return 0;
}

int xdefer_verify(struct xdefer *xd, br_ssl_client_context *sc,
	br_x509_minimal_context *xc, int leaf_known_good, int *anchor_at)
{
	const unsigned char *certs[XDEFER_CERTS_MAX];
	int i, r;

	/* with the ATW800/2's T425 up, the arithmetic of the checks below is
	 * done ahead, on it and the 68030 together; the checks find it */
	if (!xd->err && xd->ske_seen) {
		for (i = 0; i < xd->ncert; i++)
			certs[i] = xd->chain + xd->cert_off[i];
		sigpre_chain(certs, xd->cert_len, leaf_known_good ? 0 : xd->ncert,
			xc->trust_anchors, xc->trust_anchors_num, &xd->leaf,
			xd->ske_rsa, xd->ske_hv, xd->ske_hvlen, xd->ske_sig, xd->ske_siglen);
	}
	r = verify(xd, sc, xc, leaf_known_good, anchor_at);
	sigpre_stats(&xd->pre_jobs, &xd->pre_t425, &xd->pre_used);
	sigpre_clear();
	return r;
}
