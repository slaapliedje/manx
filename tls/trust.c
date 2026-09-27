/*
 * trust.c - trust anchors from a PEM bundle (the ca-certificates.crt
 * format: concatenated "-----BEGIN CERTIFICATE-----" blocks).
 *
 * The file is streamed: PEM decoding feeds the DER straight into BearSSL's
 * X.509 decoder, so no certificate is ever held whole. Each anchor keeps
 * only its subject name and public key.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bearssl.h"
#include "trust.h"

struct growbuf {
	unsigned char *p;
	size_t len, cap;
	int oom;
};

static void grow_append(void *ctx, const void *data, size_t len)
{
	struct growbuf *b = ctx;

	if (b->oom)
		return;
	if (b->len + len > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 256;
		unsigned char *np;

		while (cap < b->len + len)
			cap *= 2;
		np = realloc(b->p, cap);
		if (np == NULL) {
			b->oom = 1;
			return;
		}
		b->p = np;
		b->cap = cap;
	}
	memcpy(b->p + b->len, data, len);
	b->len += len;
}

static unsigned char *dup(const void *p, size_t n)
{
	unsigned char *d = malloc(n ? n : 1);

	if (d)
		memcpy(d, p, n);
	return d;
}

struct loader {
	br_x509_decoder_context xd;
	struct growbuf dn;
	int in_cert;
	struct trust_store *ts;
	size_t skipped;
};

static void der_to_decoder(void *ctx, const void *data, size_t len)
{
	struct loader *l = ctx;

	br_x509_decoder_push(&l->xd, data, len);
}

static int add_anchor(struct loader *l)
{
	br_x509_pkey *pk = br_x509_decoder_get_pkey(&l->xd);
	br_x509_trust_anchor *ta;

	if (pk == NULL || l->dn.oom) {
		l->skipped++;
		return 0;
	}
	if (l->ts->n == l->ts->cap) {
		size_t cap = l->ts->cap ? l->ts->cap * 2 : 64;
		br_x509_trust_anchor *na = realloc(l->ts->ta, cap * sizeof *na);

		if (na == NULL)
			return -1;
		l->ts->ta = na;
		l->ts->cap = cap;
	}
	ta = &l->ts->ta[l->ts->n];
	memset(ta, 0, sizeof *ta);
	ta->dn.data = dup(l->dn.p, l->dn.len);
	ta->dn.len = l->dn.len;
	ta->flags = br_x509_decoder_isCA(&l->xd) ? BR_X509_TA_CA : 0;
	switch (pk->key_type) {
	case BR_KEYTYPE_RSA:
		ta->pkey.key_type = BR_KEYTYPE_RSA;
		ta->pkey.key.rsa.n = dup(pk->key.rsa.n, pk->key.rsa.nlen);
		ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
		ta->pkey.key.rsa.e = dup(pk->key.rsa.e, pk->key.rsa.elen);
		ta->pkey.key.rsa.elen = pk->key.rsa.elen;
		if (!ta->pkey.key.rsa.n || !ta->pkey.key.rsa.e)
			return -1;
		break;
	case BR_KEYTYPE_EC:
		ta->pkey.key_type = BR_KEYTYPE_EC;
		ta->pkey.key.ec.curve = pk->key.ec.curve;
		ta->pkey.key.ec.q = dup(pk->key.ec.q, pk->key.ec.qlen);
		ta->pkey.key.ec.qlen = pk->key.ec.qlen;
		if (!ta->pkey.key.ec.q)
			return -1;
		break;
	default:
		free(ta->dn.data);
		l->skipped++;
		return 0;
	}
	if (!ta->dn.data)
		return -1;
	l->ts->n++;
	return 0;
}

int trust_load_pem(struct trust_store *ts, const char *path)
{
	static unsigned char buf[2048];
	br_pem_decoder_context pc;
	struct loader l;
	FILE *f;
	size_t got;
	int err = 0;

	memset(&l, 0, sizeof l);
	l.ts = ts;
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	br_pem_decoder_init(&pc);
	while (!err && (got = fread(buf, 1, sizeof buf, f)) > 0) {
		unsigned char *p = buf;

		while (got > 0 && !err) {
			size_t used = br_pem_decoder_push(&pc, p, got);

			p += used;
			got -= used;
			switch (br_pem_decoder_event(&pc)) {
			case BR_PEM_BEGIN_OBJ:
				l.in_cert = strcmp(br_pem_decoder_name(&pc),
					"CERTIFICATE") == 0
					|| strcmp(br_pem_decoder_name(&pc),
					"X509 CERTIFICATE") == 0;
				if (l.in_cert) {
					l.dn.len = 0;
					l.dn.oom = 0;
					br_x509_decoder_init(&l.xd, grow_append, &l.dn);
					br_pem_decoder_setdest(&pc, der_to_decoder, &l);
				} else
					br_pem_decoder_setdest(&pc, 0, 0);
				break;
			case BR_PEM_END_OBJ:
				if (l.in_cert && add_anchor(&l) < 0)
					err = -1;
				l.in_cert = 0;
				break;
			case BR_PEM_ERROR:
				err = -1;
				break;
			}
		}
	}
	fclose(f);
	free(l.dn.p);
	ts->skipped += l.skipped;
	return err;
}

void trust_free(struct trust_store *ts)
{
	size_t i;

	for (i = 0; i < ts->n; i++) {
		br_x509_trust_anchor *ta = &ts->ta[i];

		free(ta->dn.data);
		if (ta->pkey.key_type == BR_KEYTYPE_RSA) {
			free(ta->pkey.key.rsa.n);
			free(ta->pkey.key.rsa.e);
		} else
			free(ta->pkey.key.ec.q);
	}
	free(ts->ta);
	memset(ts, 0, sizeof *ts);
}
