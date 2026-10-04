/*
 * sigpre.c - a chain's signature arithmetic done ahead, on the T425 and
 * the 68030 together (sigpre.h).
 *
 * A job is a request of tpproto.h's (TP_RSA, TP_EC), its answer kept
 * under the SHA-256 of its type and payload; rsavrfy.c and ecvrfy.c look
 * an answer up by making the same payload from what BearSSL hands them.
 * The jobs run biggest first: one out on the T425 while the 68030 does
 * the next, the T425 given another whenever it has finished.
 */
#include <string.h>
#include "sigmath.h"
#include "tpproto.h"
#include "tpoff.h"
#include "sigpre.h"

#define MAXJOBS	10

struct job {
	int type;
	unsigned char in[TP_MAX_PAYLOAD];
	size_t inlen;
	unsigned char key[32];		/* SHA-256 of type and payload */
	unsigned char out[520];
	size_t outlen;
	int status;			/* -1: not done; TP_DONE, TP_REFUSED */
	long cost;
};

static struct job g_job[MAXJOBS];
static int g_n, g_on_t425, g_used;

static void job_key(unsigned char *key, int type, const unsigned char *in, size_t len)
{
	br_sha256_context h;
	unsigned char t = (unsigned char)type;

	br_sha256_init(&h);
	br_sha256_update(&h, &t, 1);
	br_sha256_update(&h, in, len);
	br_sha256_out(&h, key);
}

static const unsigned char *strip(const unsigned char *b, size_t *len)
{
	while (*len > 0 && *b == 0) {
		b++;
		(*len)--;
	}
	return b;
}

/* TP_RSA's payload: nlen, n, elen, e, x (n and e without leading zeros) */
static size_t rsa_payload(unsigned char *p, const unsigned char *x, size_t xlen,
	const unsigned char *n, size_t nlen, const unsigned char *e, size_t elen)
{
	size_t o = 0;

	n = strip(n, &nlen);
	e = strip(e, &elen);
	if (nlen == 0 || nlen > 512 || elen > 8 || xlen != nlen)
		return 0;
	p[o++] = (unsigned char)(nlen & 0xff);
	p[o++] = (unsigned char)(nlen >> 8);
	memcpy(p + o, n, nlen);
	o += nlen;
	p[o++] = (unsigned char)elen;
	p[o++] = 0;
	memcpy(p + o, e, elen);
	o += elen;
	memcpy(p + o, x, xlen);
	return o + xlen;
}

/* TP_EC's: curve, Q, u1, u2 */
static size_t ec_payload(unsigned char *p, int curve, const unsigned char *q,
	const unsigned char *u1, const unsigned char *u2)
{
	size_t len = sig_ec_len(curve);

	if (len == 0)
		return 0;
	p[0] = (unsigned char)curve;
	memcpy(p + 1, q, 1 + 2 * len);
	memcpy(p + 2 + 2 * len, u1, len);
	memcpy(p + 2 + 3 * len, u2, len);
	return 2 + 4 * len;
}

static struct job *find(int type, const unsigned char *in, size_t len)
{
	unsigned char key[32];
	int i;

	job_key(key, type, in, len);
	for (i = 0; i < g_n; i++)
		if (g_job[i].type == type && g_job[i].status >= 0
			&& memcmp(g_job[i].key, key, 32) == 0)
			return &g_job[i];
	return NULL;
}

static void add(int type, const unsigned char *in, size_t len, long cost)
{
	struct job *j;

	if (len == 0 || g_n == MAXJOBS || find(type, in, len) != NULL)
		return;
	j = &g_job[g_n++];
	j->type = type;
	memcpy(j->in, in, len);
	j->inlen = len;
	job_key(j->key, type, in, len);
	j->status = -1;
	j->cost = cost;
}

/* a signature to check: sig (RSA PKCS#1, or ECDSA in ASN.1) over hash, by
 * key: its arithmetic as a job */
static void add_sig(const br_x509_pkey *key, int rsa, const unsigned char *hash,
	size_t hlen, const unsigned char *sig, size_t siglen)
{
	static unsigned char p[TP_MAX_PAYLOAD];

	if (rsa && key->key_type == BR_KEYTYPE_RSA) {
		size_t len = rsa_payload(p, sig, siglen, key->key.rsa.n, key->key.rsa.nlen,
			key->key.rsa.e, key->key.rsa.elen);

		add(TP_RSA, p, len, (long)siglen * (long)siglen / 1024);
	} else if (!rsa && key->key_type == BR_KEYTYPE_EC) {
		int curve = key->key.ec.curve;
		size_t len = sig_ec_len(curve), rawlen;
		unsigned char raw[(66 << 2) + 24], u1[48], u2[48];

		if (len == 0 || key->key.ec.qlen != 1 + 2 * len || siglen > sizeof raw / 2)
			return;
		memcpy(raw, sig, siglen);
		rawlen = br_ecdsa_asn1_to_raw(raw, siglen);
		if (rawlen == 0 || !sig_ec_scalars(curve, u1, u2, raw, raw + rawlen / 2,
			rawlen / 2, hash, hlen))
			return;
		add(TP_EC, p, ec_payload(p, curve, key->key.ec.q, u1, u2),
			len == 32 ? 220 : 560);
	}
}

/* --- certificates ---------------------------------------------------------- */

/* the DER element at *p (before end): its tag, content and length; *p
 * moved past it. 0 if it doesn't fit. */
static int der(const unsigned char **p, const unsigned char *end, int *tag,
	const unsigned char **c, size_t *clen)
{
	const unsigned char *q = *p;
	size_t len;

	if (end - q < 2)
		return 0;
	*tag = *q++;
	len = *q++;
	if (len & 0x80) {
		int nb = (int)(len & 0x7f);

		if (nb < 1 || nb > 3 || end - q < nb)
			return 0;
		for (len = 0; nb > 0; nb--)
			len = len << 8 | *q++;
	}
	if ((size_t)(end - q) < len)
		return 0;
	*c = q;
	*clen = len;
	*p = q + len;
	return 1;
}

struct cert {
	const unsigned char *tbs, *issuer, *sig;
	size_t tbslen, issuerlen, siglen;
	int rsa;			/* the signature: RSA, else ECDSA */
	const br_hash_class *hash;
};

/* the signature algorithms BearSSL checks */
static const struct {
	unsigned char oid[9];
	size_t len;
	int rsa;
	const br_hash_class *hash;
} g_algs[] = {
	{ { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b }, 9, 1, &br_sha256_vtable },
	{ { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c }, 9, 1, &br_sha384_vtable },
	{ { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d }, 9, 1, &br_sha512_vtable },
	{ { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0e }, 9, 1, &br_sha224_vtable },
	{ { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x05 }, 9, 1, &br_sha1_vtable },
	{ { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 }, 8, 0, &br_sha256_vtable },
	{ { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 }, 8, 0, &br_sha384_vtable },
	{ { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04 }, 8, 0, &br_sha512_vtable },
	{ { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x01 }, 8, 0, &br_sha224_vtable },
	{ { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x01 }, 7, 0, &br_sha1_vtable },
};

static int parse_cert(const unsigned char *b, size_t len, struct cert *ct)
{
	const unsigned char *p = b, *end = b + len, *c, *q, *qend, *t, *tend, *start, *a;
	size_t clen, alen, i;
	int tag;

	if (!der(&p, end, &tag, &c, &clen) || tag != 0x30)
		return 0;
	q = c;
	qend = c + clen;
	/* tbsCertificate: kept whole, for its hash */
	start = q;
	if (!der(&q, qend, &tag, &t, &clen) || tag != 0x30)
		return 0;
	ct->tbs = start;
	ct->tbslen = (size_t)(q - start);
	tend = t + clen;
	/* in it: [0] version, serial, signature, issuer */
	if (!der(&t, tend, &tag, &c, &clen))
		return 0;
	if (tag == 0xa0 && !der(&t, tend, &tag, &c, &clen))	/* the serial */
		return 0;
	if (!der(&t, tend, &tag, &c, &clen))		/* the signature algorithm */
		return 0;
	start = t;
	if (!der(&t, tend, &tag, &c, &clen) || tag != 0x30)
		return 0;
	ct->issuer = start;
	ct->issuerlen = (size_t)(t - start);
	/* signatureAlgorithm */
	if (!der(&q, qend, &tag, &c, &clen) || tag != 0x30)
		return 0;
	if (!der(&c, c + clen, &tag, &a, &alen) || tag != 0x06)
		return 0;
	ct->hash = NULL;
	for (i = 0; i < sizeof g_algs / sizeof g_algs[0]; i++)
		if (alen == g_algs[i].len && memcmp(a, g_algs[i].oid, alen) == 0) {
			ct->rsa = g_algs[i].rsa;
			ct->hash = g_algs[i].hash;
		}
	if (ct->hash == NULL)
		return 0;
	/* signatureValue: a BIT STRING, no unused bits */
	if (!der(&q, qend, &tag, &c, &clen) || tag != 0x03 || clen < 2 || c[0] != 0)
		return 0;
	ct->sig = c + 1;
	ct->siglen = clen - 1;
	return 1;
}

static const br_x509_trust_anchor *find_ta(const br_x509_trust_anchor *tas, size_t ntas,
	const unsigned char *dn, size_t dnlen)
{
	size_t i;

	for (i = 0; i < ntas; i++)
		if (tas[i].dn.len == dnlen && memcmp(tas[i].dn.data, dn, dnlen) == 0)
			return &tas[i];
	return NULL;
}

/* --- doing them ------------------------------------------------------------ */

static void local(struct job *j)
{
	j->status = tpjob_run(j->type, j->in, j->inlen, j->out, &j->outlen);
}

/* the T425's answer to j: wait, or only if it's there; 1 if j is done */
static int collect(struct job *j, int wait)
{
	int st = tpoff_recv(j->out, &j->outlen, wait);

	if (st == -2)
		return 0;
	if (st < 0) {
		local(j);		/* the link failed: here, then */
		return 1;
	}
	j->status = st;
	g_on_t425++;
	return 1;
}

static void run(void)
{
	struct job *out = NULL;
	int next = 0, i, k;

	/* biggest first */
	for (i = 1; i < g_n; i++)
		for (k = i; k > 0 && g_job[k].cost > g_job[k - 1].cost; k--) {
			struct job t = g_job[k];

			g_job[k] = g_job[k - 1];
			g_job[k - 1] = t;
		}
	for (;;) {
		if (out == NULL && next < g_n && tpoff_up()
			&& tpoff_send(g_job[next].type, g_job[next].in, g_job[next].inlen) == 0)
			out = &g_job[next++];
		if (next < g_n)
			local(&g_job[next++]);
		else if (out != NULL) {
			collect(out, 1);
			out = NULL;
		} else
			break;
		if (out != NULL && collect(out, 0))
			out = NULL;
	}
}

void sigpre_chain(const unsigned char *const *certs, const size_t *lens, int ncert,
	const br_x509_trust_anchor *tas, size_t ntas, const br_x509_pkey *leaf,
	int ske_rsa, const unsigned char *ske_hv, size_t ske_hvlen,
	const unsigned char *ske_sig, size_t ske_siglen)
{
	static br_x509_decoder_context dc;
	int i;

	sigpre_clear();
	if (!tpoff_up())
		return;
	add_sig(leaf, ske_rsa, ske_hv, ske_hvlen, ske_sig, ske_siglen);
	for (i = 0; i < ncert; i++) {
		const br_x509_trust_anchor *ta;
		const br_x509_pkey *signer = NULL;
		unsigned char hash[64];
		size_t hlen = 0;
		struct cert ct;

		if (!parse_cert(certs[i], lens[i], &ct))
			break;
		ta = find_ta(tas, ntas, ct.issuer, ct.issuerlen);
		if (ta != NULL)
			signer = &ta->pkey;
		else if (i + 1 < ncert) {
			br_x509_decoder_init(&dc, 0, 0);
			br_x509_decoder_push(&dc, certs[i + 1], lens[i + 1]);
			signer = br_x509_decoder_get_pkey(&dc);
		}
		if (signer == NULL)
			break;
		if (!ct.rsa) {
			br_hash_compat_context h;

			ct.hash->init(&h.vtable);
			ct.hash->update(&h.vtable, ct.tbs, ct.tbslen);
			ct.hash->out(&h.vtable, hash);
			hlen = (ct.hash->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
		}
		add_sig(signer, ct.rsa, hash, hlen, ct.sig, ct.siglen);
		if (ta != NULL)
			break;		/* the anchor: BearSSL stops there too */
	}
	run();
}

void sigpre_clear(void)
{
	g_n = g_on_t425 = g_used = 0;
}

int sigpre_rsa(unsigned char *x, size_t xlen, const unsigned char *n, size_t nlen,
	const unsigned char *e, size_t elen)
{
	static unsigned char p[TP_MAX_PAYLOAD];
	struct job *j;
	size_t len;

	if (g_n == 0 || (len = rsa_payload(p, x, xlen, n, nlen, e, elen)) == 0
		|| (j = find(TP_RSA, p, len)) == NULL)
		return -1;
	g_used++;
	if (j->status != TP_DONE || j->outlen != xlen)
		return 0;
	memcpy(x, j->out, xlen);
	return 1;
}

int sigpre_ec(int curve, unsigned char *x, const unsigned char *q,
	const unsigned char *u1, const unsigned char *u2)
{
	unsigned char p[2 + 4 * 48];
	struct job *j;
	size_t len;

	if (g_n == 0 || (len = ec_payload(p, curve, q, u1, u2)) == 0
		|| (j = find(TP_EC, p, len)) == NULL)
		return -1;
	g_used++;
	if (j->status != TP_DONE || j->outlen != sig_ec_len(curve))
		return 0;
	memcpy(x, j->out, j->outlen);
	return 1;
}

void sigpre_stats(int *jobs, int *on_t425, int *used)
{
	*jobs = g_n;
	*on_t425 = g_on_t425;
	*used = g_used;
}
