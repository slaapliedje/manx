/*
 * ecvrfy.c - ECDSA signatures checked on P-256 and P-384 (ecvrfy.h).
 *
 * Field elements are k limbs (8 or 12) in Montgomery form mod p; points
 * are Jacobian (x = X/Z^2, y = Y/Z^3) or affine, the curves' a = -3.
 * Scalars are plain integers mod n, recoded to width-4 NAF (digits 0,
 * +-1, +-3, +-5, +-7), with tables of P, 3P, 5P, 7P in affine form for G
 * and Q.
 */
#include <string.h>
#include "mont.h"
#include "ecvrfy.h"
#include "ecvrfy_curves.h"

#define FK	12		/* limbs of the largest field (P-384) */
#define NAFMAX	(384 + 2)

typedef uint32_t fe[FK];

struct curve {
	struct mont p, n;
	int k, len;		/* limbs, bytes */
	fe one, b;		/* 1 and b, Montgomery form mod p */
	fe r2p, r2n;		/* R^2 mod p, R^2 mod n */
	fe gx, gy;		/* G, Montgomery form */
	int ready;
};

struct jac {
	fe x, y, z;
	int inf;		/* the point at infinity */
};

struct aff {
	fe x, y;
};

/* --- multi-limb integers ---------------------------------------------- */

/* d = a + b, the carry out */
static uint32_t add_k(uint32_t *d, const uint32_t *a, const uint32_t *b, int k)
{
	uint32_t cy = 0;
	int i;

	for (i = 0; i < k; i++) {
		uint32_t s = a[i] + cy, t;

		cy = s < cy;
		t = s + b[i];
		cy += t < s;
		d[i] = t;
	}
	return cy;
}

/* d = a - b, the borrow out */
static uint32_t sub_k(uint32_t *d, const uint32_t *a, const uint32_t *b, int k)
{
	uint32_t bw = 0;
	int i;

	for (i = 0; i < k; i++) {
		uint32_t x = a[i], y = b[i], t = x - y, b1 = x < y;

		d[i] = t - bw;
		bw = b1 | (t < bw);
	}
	return bw;
}

static int cmp_k(const uint32_t *a, const uint32_t *b, int k)
{
	int i;

	for (i = k - 1; i >= 0; i--)
		if (a[i] != b[i])
			return a[i] > b[i] ? 1 : -1;
	return 0;
}

static int zero_k(const uint32_t *a, int k)
{
	int i;

	for (i = 0; i < k; i++)
		if (a[i])
			return 0;
	return 1;
}

static int one_k(const uint32_t *a, int k)
{
	return a[0] == 1 && (k == 1 || zero_k(a + 1, k - 1));
}

/* a = (top:a) >> 1, top 0 or 1 */
static void shr1(uint32_t *a, uint32_t top, int k)
{
	int i;

	for (i = 0; i < k - 1; i++)
		a[i] = a[i] >> 1 | a[i + 1] << 31;
	a[k - 1] = a[k - 1] >> 1 | top << 31;
}

/* a += v or a -= v, for a small v (no carry out of k limbs) */
static void addsmall(uint32_t *a, uint32_t v, int k)
{
	int i;

	for (i = 0; i < k && v; i++) {
		a[i] += v;
		v = a[i] < v;
	}
}

static void subsmall(uint32_t *a, uint32_t v, int k)
{
	int i;

	for (i = 0; i < k && v; i++) {
		uint32_t x = a[i];

		a[i] = x - v;
		v = x < v;
	}
}

/* --- modulo m ------------------------------------------------------------- */

/* d = a + b, a - b mod m, for a, b < m */
static void add_m(uint32_t *d, const uint32_t *a, const uint32_t *b, const struct mont *m)
{
	if (add_k(d, a, b, m->k) || mont_cmp(d, m) >= 0)
		mont_sub_n(d, m);
}

static void sub_m(uint32_t *d, const uint32_t *a, const uint32_t *b, const struct mont *m)
{
	if (sub_k(d, a, b, m->k))
		add_k(d, d, m->n, m->k);
}

/* u /= 2 (u even); x = x / 2 mod m */
static void halve(uint32_t *u, uint32_t *x, const struct mont *m)
{
	uint32_t cy = 0;

	shr1(u, 0, m->k);
	if (x[0] & 1)
		cy = add_k(x, x, m->n, m->k);
	shr1(x, cy, m->k);
}

/* d = 1/a mod m, 0 < a < m, plain (not Montgomery): binary GCD, keeping
 * x1 a = u and x2 a = v (mod m) */
static void inv_m(uint32_t *d, const uint32_t *a, const struct mont *m)
{
	fe u, v, x1, x2;
	int k = m->k;

	memcpy(u, a, (size_t)k * sizeof *u);
	memcpy(v, m->n, (size_t)k * sizeof *v);
	memset(x1, 0, sizeof x1);
	memset(x2, 0, sizeof x2);
	x1[0] = 1;
	for (;;) {
		if (one_k(u, k)) {
			memcpy(d, x1, (size_t)k * sizeof *d);
			return;
		}
		if (one_k(v, k)) {
			memcpy(d, x2, (size_t)k * sizeof *d);
			return;
		}
		while (!(u[0] & 1))
			halve(u, x1, m);
		while (!(v[0] & 1))
			halve(v, x2, m);
		if (cmp_k(u, v, k) >= 0) {
			sub_k(u, u, v, k);
			sub_m(x1, x1, x2, m);
		} else {
			sub_k(v, v, u, k);
			sub_m(x2, x2, x1, m);
		}
	}
}

/* --- the field ------------------------------------------------------------ */

#define FADD(d, a, b)	add_m(d, a, b, &c->p)
#define FSUB(d, a, b)	sub_m(d, a, b, &c->p)
#define FMUL(d, a, b)	mont_mul(d, a, b, &c->p)
#define FCOPY(d, a)	memcpy(d, a, (size_t)c->k * sizeof (d)[0])
#define FZERO(a)	zero_k(a, c->k)

/* d = 1/a, both Montgomery form, a != 0 */
static void finv(uint32_t *d, const uint32_t *a, const struct curve *c)
{
	fe t, one;

	memset(one, 0, sizeof one);
	one[0] = 1;
	FMUL(t, a, one);
	inv_m(t, t, &c->p);
	FMUL(d, t, c->r2p);
}

/* y^2 = x^3 - 3x + b? */
static int on_curve(const uint32_t *x, const uint32_t *y, const struct curve *c)
{
	fe l, r, t;

	FMUL(l, y, y);
	FMUL(r, x, x);
	FMUL(r, r, x);
	FADD(t, x, x);
	FADD(t, t, x);
	FSUB(r, r, t);
	FADD(r, r, c->b);
	return memcmp(l, r, (size_t)c->k * sizeof l[0]) == 0;
}

/* --- the curves ----------------------------------------------------------- */

static void setup(struct curve *c, const unsigned char *p, const unsigned char *n,
	const unsigned char *b, const unsigned char *gx, const unsigned char *gy, int len)
{
	fe t;

	c->len = len;
	c->k = len / 4;
	c->p.k = c->n.k = c->k;
	mont_decode(c->p.n, c->k, p, (size_t)len);
	mont_setup(&c->p);
	mont_decode(c->n.n, c->k, n, (size_t)len);
	mont_setup(&c->n);
	mont_r2(c->r2p, &c->p);
	mont_r2(c->r2n, &c->n);
	memset(t, 0, sizeof t);
	t[0] = 1;
	FMUL(c->one, t, c->r2p);
	mont_decode(t, c->k, b, (size_t)len);
	FMUL(c->b, t, c->r2p);
	mont_decode(t, c->k, gx, (size_t)len);
	FMUL(c->gx, t, c->r2p);
	mont_decode(t, c->k, gy, (size_t)len);
	FMUL(c->gy, t, c->r2p);
	c->ready = 1;
}

static struct curve *curve(int id)
{
	static struct curve p256, p384;

	if (id == BR_EC_secp256r1) {
		if (!p256.ready)
			setup(&p256, P256_P, P256_N, P256_B, P256_GX, P256_GY, 32);
		return &p256;
	}
	if (id == BR_EC_secp384r1) {
		if (!p384.ready)
			setup(&p384, P384_P, P384_N, P384_B, P384_GX, P384_GY, 48);
		return &p384;
	}
	return NULL;
}

/* --- points --------------------------------------------------------------- */

/* P = 2P: dbl-2001-b (a = -3) */
static void pdbl(struct jac *P, const struct curve *c)
{
	fe delta, gamma, beta, alpha, t1, t2;

	if (P->inf)
		return;
	if (FZERO(P->y)) {
		P->inf = 1;
		return;
	}
	FMUL(delta, P->z, P->z);
	FMUL(gamma, P->y, P->y);
	FMUL(beta, P->x, gamma);
	FSUB(t1, P->x, delta);
	FADD(t2, P->x, delta);
	FMUL(alpha, t1, t2);
	FADD(t1, alpha, alpha);
	FADD(alpha, t1, alpha);
	/* Z3 = (Y + Z)^2 - gamma - delta */
	FADD(t1, P->y, P->z);
	FMUL(t1, t1, t1);
	FSUB(t1, t1, gamma);
	FSUB(P->z, t1, delta);
	/* X3 = alpha^2 - 8 beta */
	FADD(beta, beta, beta);
	FADD(beta, beta, beta);
	FADD(t2, beta, beta);
	FMUL(t1, alpha, alpha);
	FSUB(P->x, t1, t2);
	/* Y3 = alpha (4 beta - X3) - 8 gamma^2 */
	FSUB(t1, beta, P->x);
	FMUL(t1, alpha, t1);
	FMUL(gamma, gamma, gamma);
	FADD(gamma, gamma, gamma);
	FADD(gamma, gamma, gamma);
	FADD(gamma, gamma, gamma);
	FSUB(P->y, t1, gamma);
}

/* P += (qx, qy) affine: madd-2007-bl */
static void padd(struct jac *P, const uint32_t *qx, const uint32_t *qy,
	const struct curve *c)
{
	fe z1z1, u2, s2, h, hh, i4, j, r, v, t;

	if (P->inf) {
		FCOPY(P->x, qx);
		FCOPY(P->y, qy);
		FCOPY(P->z, c->one);
		P->inf = 0;
		return;
	}
	FMUL(z1z1, P->z, P->z);
	FMUL(u2, qx, z1z1);
	FMUL(s2, qy, P->z);
	FMUL(s2, s2, z1z1);
	FSUB(h, u2, P->x);
	FSUB(r, s2, P->y);
	FADD(r, r, r);
	if (FZERO(h)) {
		if (FZERO(r))
			pdbl(P, c);	/* P = Q */
		else
			P->inf = 1;	/* P = -Q */
		return;
	}
	FMUL(hh, h, h);
	FADD(i4, hh, hh);
	FADD(i4, i4, i4);
	FMUL(j, h, i4);
	FMUL(v, P->x, i4);
	/* X3 = r^2 - J - 2V */
	FMUL(t, r, r);
	FSUB(t, t, j);
	FSUB(t, t, v);
	FSUB(t, t, v);
	/* Z3 = (Z1 + H)^2 - Z1Z1 - HH */
	FADD(P->z, P->z, h);
	FMUL(P->z, P->z, P->z);
	FSUB(P->z, P->z, z1z1);
	FSUB(P->z, P->z, hh);
	/* Y3 = r (V - X3) - 2 Y1 J */
	FSUB(v, v, t);
	FMUL(v, r, v);
	FMUL(j, P->y, j);
	FADD(j, j, j);
	FSUB(P->y, v, j);
	FCOPY(P->x, t);
}

static void to_aff(struct aff *A, const struct jac *P, const struct curve *c)
{
	fe zi, zi2;

	finv(zi, P->z, c);
	FMUL(zi2, zi, zi);
	FMUL(A->x, P->x, zi2);
	FMUL(zi2, zi2, zi);
	FMUL(A->y, P->y, zi2);
}

/* T = P, 3P, 5P, 7P (P of the curve's prime order: none is infinity) */
static void table(struct aff *T, const uint32_t *x, const uint32_t *y,
	const struct curve *c)
{
	struct jac J;
	struct aff two;
	int i;

	FCOPY(T[0].x, x);
	FCOPY(T[0].y, y);
	FCOPY(J.x, x);
	FCOPY(J.y, y);
	FCOPY(J.z, c->one);
	J.inf = 0;
	pdbl(&J, c);
	to_aff(&two, &J, c);
	for (i = 1; i < 4; i++) {
		FCOPY(J.x, T[i - 1].x);
		FCOPY(J.y, T[i - 1].y);
		FCOPY(J.z, c->one);
		J.inf = 0;
		padd(&J, two.x, two.y, c);
		to_aff(&T[i], &J, c);
	}
}

/* P += d T (d odd, -7..7) */
static void add_digit(struct jac *P, const struct aff *T, int d, const struct curve *c)
{
	if (d > 0)
		padd(P, T[(d - 1) / 2].x, T[(d - 1) / 2].y, c);
	else {
		fe ny, zero;

		memset(zero, 0, sizeof zero);
		FSUB(ny, zero, T[(-d - 1) / 2].y);
		padd(P, T[(-d - 1) / 2].x, ny, c);
	}
}

/* width-4 NAF of d (plain, destroyed), least significant digit first */
static int naf(signed char *out, uint32_t *d, int k)
{
	int n = 0;

	while (!zero_k(d, k) && n < NAFMAX) {
		int dig = 0;

		if (d[0] & 1) {
			dig = (int)(d[0] & 15);
			if (dig > 8)
				dig -= 16;
			if (dig > 0)
				subsmall(d, (uint32_t)dig, k);
			else
				addsmall(d, (uint32_t)-dig, k);
		}
		out[n++] = (signed char)dig;
		shr1(d, 0, k);
	}
	return n;
}

/* --- ECDSA ---------------------------------------------------------------- */

/* a signature half: blen big-endian bytes, 0 < value < n */
static int scalar(uint32_t *d, const unsigned char *b, size_t blen, const struct curve *c)
{
	while (blen > (size_t)c->len) {
		if (*b)
			return 0;
		b++;
		blen--;
	}
	mont_decode(d, c->k, b, blen);
	return !zero_k(d, c->k) && mont_cmp(d, &c->n) < 0;
}

uint32_t ecvrfy_raw(const br_ec_impl *impl, const void *hash, size_t hash_len,
	const br_ec_public_key *pk, const void *sig, size_t sig_len)
{
	const unsigned char *q = pk->q, *sg = sig;
	struct curve *c = curve(pk->curve);
	fe r, s, h, w, u1, u2, qx, qy, t, t2;
	struct aff tg[4], tq[4];
	struct jac R;
	signed char n1[NAFMAX], n2[NAFMAX];
	size_t rlen, len;
	int l1, l2, i;

	if (c == NULL)
		return br_ecdsa_i31_vrfy_raw(impl, hash, hash_len, pk, sig, sig_len);
	len = (size_t)c->len;
	if (sig_len == 0 || (sig_len & 1) || pk->qlen != 1 + 2 * len || q[0] != 0x04)
		return 0;
	rlen = sig_len / 2;
	if (!scalar(r, sg, rlen, c) || !scalar(s, sg + rlen, rlen, c))
		return 0;

	/* Q: below p, on the curve */
	mont_decode(qx, c->k, q + 1, len);
	mont_decode(qy, c->k, q + 1 + len, len);
	if (mont_cmp(qx, &c->p) >= 0 || mont_cmp(qy, &c->p) >= 0)
		return 0;
	FMUL(qx, qx, c->r2p);
	FMUL(qy, qy, c->r2p);
	if (!on_curve(qx, qy, c))
		return 0;

	/* h: the hash's leftmost bits (the orders here are whole bytes), mod n */
	mont_decode(h, c->k, hash, hash_len < len ? hash_len : len);
	if (mont_cmp(h, &c->n) >= 0)
		mont_sub_n(h, &c->n);

	/* u1 = h/s, u2 = r/s mod n */
	inv_m(w, s, &c->n);
	mont_mul(t, h, w, &c->n);
	mont_mul(u1, t, c->r2n, &c->n);
	mont_mul(t, r, w, &c->n);
	mont_mul(u2, t, c->r2n, &c->n);

	/* u1 G + u2 Q */
	l1 = naf(n1, u1, c->k);
	l2 = naf(n2, u2, c->k);
	table(tg, c->gx, c->gy, c);
	table(tq, qx, qy, c);
	R.inf = 1;
	for (i = (l1 > l2 ? l1 : l2) - 1; i >= 0; i--) {
		pdbl(&R, c);
		if (i < l1 && n1[i])
			add_digit(&R, tg, n1[i], c);
		if (i < l2 && n2[i])
			add_digit(&R, tq, n2[i], c);
	}
	if (R.inf)
		return 0;

	/* x mod n = r: X = r Z^2, or (r + n) Z^2 when r + n < p */
	FMUL(t, R.z, R.z);
	FMUL(t2, r, c->r2p);
	FMUL(t2, t2, t);
	if (memcmp(t2, R.x, len) == 0)
		return 1;
	if (!add_k(t2, r, c->n.n, c->k) && cmp_k(t2, c->p.n, c->k) < 0) {
		FMUL(t2, t2, c->r2p);
		FMUL(t2, t2, t);
		if (memcmp(t2, R.x, len) == 0)
			return 1;
	}
	return 0;
}

uint32_t ecvrfy_asn1(const br_ec_impl *impl, const void *hash, size_t hash_len,
	const br_ec_public_key *pk, const void *sig, size_t sig_len)
{
	/* twice the largest raw signature: a malformed ASN.1 one may grow */
	unsigned char rsig[(66 << 2) + 24];

	if (sig_len > sizeof rsig / 2)
		return 0;
	memcpy(rsig, sig, sig_len);
	sig_len = br_ecdsa_asn1_to_raw(rsig, sig_len);
	return ecvrfy_raw(impl, hash, hash_len, pk, rsig, sig_len);
}
