/*
 * anchors.c - trust anchor sets: from PEM, from single certificates, and
 * to and from the binary cache format:
 *
 *   "UBTA1\n"  u32 count  u32 src_size  u32 src_mtime
 *   per anchor:
 *     u8 flags (BR_X509_TA_CA)  u8 key type  u32 notafter (days)
 *     u16 dn_len  dn
 *     RSA: u16 n_len n  u16 e_len e     EC: u8 curve  u16 q_len q
 *
 * all big-endian. Each anchor's DN and key bytes live in one allocation,
 * pointed to by ta.dn.data.
 */
#include <stdio.h>
#include <string.h>
#include "os.h"
#include "anchors.h"

#define MAGIC	"UBTA1\n"

void anchors_init(struct anchors *a)
{
	memset(a, 0, sizeof *a);
}

void anchors_free(struct anchors *a)
{
	size_t i;

	for (i = 0; i < a->n; i++)
		xfree(a->ta[i].dn.data);
	xfree(a->ta);
	xfree(a->notafter);
	memset(a, 0, sizeof *a);
}

static size_t key_len(const br_x509_pkey *pk)
{
	return pk->key_type == BR_KEYTYPE_RSA
		? pk->key.rsa.nlen + pk->key.rsa.elen : pk->key.ec.qlen;
}

static int same_key(const br_x509_pkey *a, const br_x509_pkey *b)
{
	if (a->key_type != b->key_type)
		return 0;
	if (a->key_type == BR_KEYTYPE_RSA)
		return a->key.rsa.nlen == b->key.rsa.nlen
			&& a->key.rsa.elen == b->key.rsa.elen
			&& memcmp(a->key.rsa.n, b->key.rsa.n, a->key.rsa.nlen) == 0
			&& memcmp(a->key.rsa.e, b->key.rsa.e, a->key.rsa.elen) == 0;
	return a->key.ec.curve == b->key.ec.curve
		&& a->key.ec.qlen == b->key.ec.qlen
		&& memcmp(a->key.ec.q, b->key.ec.q, a->key.ec.qlen) == 0;
}

/* add an anchor, copying dn and key into one block: 1, 0 dup, -1 oom */
static int add(struct anchors *a, const unsigned char *dn, size_t dnlen,
	const br_x509_pkey *pk, unsigned flags, unsigned long notafter)
{
	br_x509_trust_anchor *ta;
	unsigned char *blk;
	size_t i;

	if (pk->key_type != BR_KEYTYPE_RSA && pk->key_type != BR_KEYTYPE_EC) {
		a->skipped++;
		return 0;
	}
	for (i = 0; i < a->n; i++)
		if (a->ta[i].dn.len == dnlen
			&& memcmp(a->ta[i].dn.data, dn, dnlen) == 0
			&& same_key(&a->ta[i].pkey, pk))
			return 0;
	if (a->n == a->cap) {
		size_t cap = a->cap ? a->cap * 2 : 32;
		br_x509_trust_anchor *nt = xrealloc(a->ta, cap * sizeof *nt);
		unsigned long *nn;

		if (nt == NULL)
			return -1;
		a->ta = nt;
		nn = xrealloc(a->notafter, cap * sizeof *nn);
		if (nn == NULL)
			return -1;
		a->notafter = nn;
		a->cap = cap;
	}
	blk = xmalloc(dnlen + key_len(pk) + 1);
	if (blk == NULL)
		return -1;
	ta = &a->ta[a->n];
	memset(ta, 0, sizeof *ta);
	memcpy(blk, dn, dnlen);
	ta->dn.data = blk;
	ta->dn.len = dnlen;
	ta->flags = flags;
	ta->pkey.key_type = pk->key_type;
	if (pk->key_type == BR_KEYTYPE_RSA) {
		ta->pkey.key.rsa.n = blk + dnlen;
		ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
		memcpy(ta->pkey.key.rsa.n, pk->key.rsa.n, pk->key.rsa.nlen);
		ta->pkey.key.rsa.e = ta->pkey.key.rsa.n + pk->key.rsa.nlen;
		ta->pkey.key.rsa.elen = pk->key.rsa.elen;
		memcpy(ta->pkey.key.rsa.e, pk->key.rsa.e, pk->key.rsa.elen);
	} else {
		ta->pkey.key.ec.curve = pk->key.ec.curve;
		ta->pkey.key.ec.q = blk + dnlen;
		ta->pkey.key.ec.qlen = pk->key.ec.qlen;
		memcpy(ta->pkey.key.ec.q, pk->key.ec.q, pk->key.ec.qlen);
	}
	a->notafter[a->n] = notafter;
	a->n++;
	return 1;
}

/* --- from certificates --------------------------------------------------- */

struct dnbuf {
	unsigned char data[1024];
	size_t len;
	int overflow;
};

static void dn_append(void *ctx, const void *buf, size_t len)
{
	struct dnbuf *d = ctx;

	if (d->len + len > sizeof d->data) {
		d->overflow = 1;
		return;
	}
	memcpy(d->data + d->len, buf, len);
	d->len += len;
}

static int from_decoder(struct anchors *a, br_x509_decoder_context *dc,
	struct dnbuf *dn)
{
	br_x509_pkey *pk = br_x509_decoder_get_pkey(dc);

	if (pk == NULL || dn->overflow) {
		a->skipped++;
		return 0;
	}
	return add(a, dn->data, dn->len, pk,
		br_x509_decoder_isCA(dc) ? BR_X509_TA_CA : 0, dc->notafter_days);
}

int anchors_add_der(struct anchors *a, const unsigned char *der, size_t len)
{
	static br_x509_decoder_context dc;
	static struct dnbuf dn;

	dn.len = 0;
	dn.overflow = 0;
	br_x509_decoder_init(&dc, dn_append, &dn);
	br_x509_decoder_push(&dc, der, len);
	return from_decoder(a, &dc, &dn);
}

struct pemload {
	br_x509_decoder_context dc;
	struct dnbuf dn;
	int in_cert;
};

static void to_decoder(void *ctx, const void *data, size_t len)
{
	struct pemload *l = ctx;

	br_x509_decoder_push(&l->dc, data, len);
}

int anchors_load_pem(struct anchors *a, const char *path)
{
	static unsigned char buf[2048];
	static struct pemload l;
	br_pem_decoder_context pc;
	FILE *f;
	size_t got;
	int err = 0;

	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	memset(&l, 0, sizeof l);
	br_pem_decoder_init(&pc);
	while (!err && (got = fread(buf, 1, sizeof buf, f)) > 0) {
		unsigned char *p = buf;

		while (got > 0 && !err) {
			size_t used = br_pem_decoder_push(&pc, p, got);

			p += used;
			got -= used;
			switch (br_pem_decoder_event(&pc)) {
			case BR_PEM_BEGIN_OBJ:
				l.in_cert = strcmp(br_pem_decoder_name(&pc), "CERTIFICATE") == 0
					|| strcmp(br_pem_decoder_name(&pc), "X509 CERTIFICATE") == 0;
				if (l.in_cert) {
					l.dn.len = 0;
					l.dn.overflow = 0;
					br_x509_decoder_init(&l.dc, dn_append, &l.dn);
					br_pem_decoder_setdest(&pc, to_decoder, &l);
				} else
					br_pem_decoder_setdest(&pc, 0, 0);
				break;
			case BR_PEM_END_OBJ:
				if (l.in_cert && from_decoder(a, &l.dc, &l.dn) < 0)
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
	return err;
}

int anchors_add_copy(struct anchors *dst, const struct anchors *src, size_t i)
{
	const br_x509_trust_anchor *ta = &src->ta[i];

	return add(dst, ta->dn.data, ta->dn.len, &ta->pkey, ta->flags,
		src->notafter[i]);
}

/* --- binary form -------------------------------------------------------- */

struct wbuf {
	unsigned char *p;
	size_t len, cap;
	int oom;
};

static void put(struct wbuf *w, const void *d, size_t n)
{
	if (w->oom)
		return;
	if (w->len + n > w->cap) {
		size_t cap = w->cap ? w->cap * 2 : 4096;
		unsigned char *np;

		while (cap < w->len + n)
			cap *= 2;
		np = xrealloc(w->p, cap);
		if (np == NULL) {
			w->oom = 1;
			return;
		}
		w->p = np;
		w->cap = cap;
	}
	memcpy(w->p + w->len, d, n);
	w->len += n;
}

static void put8(struct wbuf *w, unsigned v)
{
	unsigned char b = (unsigned char)v;

	put(w, &b, 1);
}

static void put16(struct wbuf *w, unsigned v)
{
	unsigned char b[2];

	b[0] = (unsigned char)(v >> 8);
	b[1] = (unsigned char)v;
	put(w, b, 2);
}

static void put32(struct wbuf *w, unsigned long v)
{
	unsigned char b[4];

	b[0] = (unsigned char)(v >> 24);
	b[1] = (unsigned char)(v >> 16);
	b[2] = (unsigned char)(v >> 8);
	b[3] = (unsigned char)v;
	put(w, b, 4);
}

int anchors_save(const struct anchors *a, const char *path, long src_size,
	long src_mtime)
{
	struct wbuf w;
	size_t i;
	int rc;

	memset(&w, 0, sizeof w);
	put(&w, MAGIC, 6);
	put32(&w, a->n);
	put32(&w, (unsigned long)src_size);
	put32(&w, (unsigned long)src_mtime);
	for (i = 0; i < a->n; i++) {
		const br_x509_trust_anchor *ta = &a->ta[i];

		put8(&w, ta->flags);
		put8(&w, ta->pkey.key_type);
		put32(&w, a->notafter[i]);
		put16(&w, (unsigned)ta->dn.len);
		put(&w, ta->dn.data, ta->dn.len);
		if (ta->pkey.key_type == BR_KEYTYPE_RSA) {
			put16(&w, (unsigned)ta->pkey.key.rsa.nlen);
			put(&w, ta->pkey.key.rsa.n, ta->pkey.key.rsa.nlen);
			put16(&w, (unsigned)ta->pkey.key.rsa.elen);
			put(&w, ta->pkey.key.rsa.e, ta->pkey.key.rsa.elen);
		} else {
			put8(&w, (unsigned)ta->pkey.key.ec.curve);
			put16(&w, (unsigned)ta->pkey.key.ec.qlen);
			put(&w, ta->pkey.key.ec.q, ta->pkey.key.ec.qlen);
		}
	}
	rc = w.oom ? -1 : os_write_file(path, w.p, w.len, 0644);
	xfree(w.p);
	return rc;
}

struct rbuf {
	const unsigned char *p;
	size_t left;
	int bad;
};

static const unsigned char *get(struct rbuf *r, size_t n)
{
	const unsigned char *p = r->p;

	if (r->bad || n > r->left) {
		r->bad = 1;
		return NULL;
	}
	r->p += n;
	r->left -= n;
	return p;
}

static unsigned long getn(struct rbuf *r, int bytes)
{
	const unsigned char *p = get(r, (size_t)bytes);
	unsigned long v = 0;
	int i;

	if (p == NULL)
		return 0;
	for (i = 0; i < bytes; i++)
		v = v << 8 | p[i];
	return v;
}

int anchors_load(struct anchors *a, const char *path, long *src_size,
	long *src_mtime)
{
	unsigned char *data;
	size_t len;
	struct rbuf r;
	unsigned long count, i, today, sec;
	int err = 0;

	data = os_read_file(path, &len);
	if (data == NULL)
		return -1;
	r.p = data;
	r.left = len;
	r.bad = 0;
	if (len < 18 || memcmp(get(&r, 6), MAGIC, 6) != 0) {
		xfree(data);
		return -1;
	}
	count = getn(&r, 4);
	if (src_size)
		*src_size = (long)getn(&r, 4);
	else
		getn(&r, 4);
	if (src_mtime)
		*src_mtime = (long)getn(&r, 4);
	else
		getn(&r, 4);
	os_x509_now(&today, &sec);
	for (i = 0; i < count && !r.bad && !err; i++) {
		br_x509_pkey pk;
		unsigned flags = (unsigned)getn(&r, 1);
		unsigned long notafter;
		size_t dnlen;
		const unsigned char *dn;

		memset(&pk, 0, sizeof pk);
		pk.key_type = (unsigned char)getn(&r, 1);
		notafter = getn(&r, 4);
		dnlen = (size_t)getn(&r, 2);
		dn = get(&r, dnlen);
		if (pk.key_type == BR_KEYTYPE_RSA) {
			pk.key.rsa.nlen = (size_t)getn(&r, 2);
			pk.key.rsa.n = (unsigned char *)get(&r, pk.key.rsa.nlen);
			pk.key.rsa.elen = (size_t)getn(&r, 2);
			pk.key.rsa.e = (unsigned char *)get(&r, pk.key.rsa.elen);
		} else if (pk.key_type == BR_KEYTYPE_EC) {
			pk.key.ec.curve = (int)getn(&r, 1);
			pk.key.ec.qlen = (size_t)getn(&r, 2);
			pk.key.ec.q = (unsigned char *)get(&r, pk.key.ec.qlen);
		} else
			r.bad = 1;
		if (r.bad)
			break;
		if (notafter && notafter < today) {	/* expired: leave out */
			a->skipped++;
			continue;
		}
		if (add(a, dn, dnlen, &pk, flags, notafter) < 0)
			err = -1;
	}
	xfree(data);
	return r.bad || err ? -1 : 0;
}
