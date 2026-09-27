/*
 * tlsbench - Phase 0 spike: how fast is each BearSSL implementation on the
 * 68030, and does the target build compute the right answers?
 *
 *   tlsbench [min_ms] [group...]     groups: kat hash sym ec ecdsa rsa
 *
 * Every group first checks correctness: known-answer tests where there are
 * published vectors (SHA-2, X25519), and otherwise agreement between all
 * implementations plus a fingerprint (the first bytes of a SHA-256 over the
 * outputs) that must match the host build's. Then each operation is timed
 * for at least min_ms (default 1500).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bearssl.h"
#include "os.h"
#include "rsa_moduli.h"

static unsigned long g_min_ms = 1500;
static int g_fail;

/* --- helpers --------------------------------------------------------------- */

static size_t unhex(unsigned char *dst, const char *src)
{
	size_t n = 0;
	int hi = -1;

	for (; *src; src++) {
		int c = *src, v;

		if (c >= '0' && c <= '9') v = c - '0';
		else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
		else continue;
		if (hi < 0)
			hi = v;
		else {
			dst[n++] = (unsigned char)(hi << 4 | v);
			hi = -1;
		}
	}
	return n;
}

static void check(const char *what, int ok)
{
	printf("  %-34s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		g_fail++;
}

static void check_hex(const char *what, const unsigned char *got,
	size_t len, const char *want_hex)
{
	unsigned char want[128];
	size_t n = unhex(want, want_hex);

	check(what, n == len && memcmp(got, want, len) == 0);
}

/* fingerprint: running SHA-256 over a group's outputs */
static br_sha256_context g_fp;

static void fp_start(void) { br_sha256_init(&g_fp); }
static void fp_add(const void *p, size_t n) { br_sha256_update(&g_fp, p, n); }

static void fp_print(const char *group)
{
	unsigned char h[32];
	int i;

	br_sha256_out(&g_fp, h);
	printf("  %-34s ", "fingerprint");
	for (i = 0; i < 8; i++)
		printf("%02x", h[i]);
	printf("  (%s; must match the host build)\n", group);
}

/*
 * Run op(arg) until at least g_min_ms has passed (and at least once);
 * report per-operation time, or throughput when bytes > 0.
 */
static void bench(const char *name, void (*op)(void *), void *arg,
	unsigned long bytes)
{
	unsigned long t0, dt, n = 0;

	t0 = os_msec();
	do {
		op(arg);
		n++;
		dt = os_msec() - t0;
	} while (dt < g_min_ms);
	if (bytes)
		printf("  %-34s %8lu KB/s   (%lu x %lu B in %lu ms)\n", name,
			dt ? bytes / 64 * n * 1000 / 16 / dt : 0UL,
			n, bytes, dt);
	else
		printf("  %-34s %8lu ms/op  (%lu ops in %lu ms)\n", name,
			(dt + n / 2) / n, n, dt);
	fflush(stdout);
}

/* --- known answers ----------------------------------------------------------- */

static void group_kat(void)
{
	br_sha1_context s1;
	br_sha256_context s256;
	br_sha384_context s384;
	unsigned char h[64];
	unsigned char k[32], u[32];

	printf("kat\n");
	br_sha1_init(&s1);
	br_sha1_update(&s1, "abc", 3);
	br_sha1_out(&s1, h);
	check_hex("SHA-1(abc)", h, 20,
		"a9993e364706816aba3e25717850c26c9cd0d89d");
	br_sha256_init(&s256);
	br_sha256_update(&s256, "abc", 3);
	br_sha256_out(&s256, h);
	check_hex("SHA-256(abc)", h, 32,
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	br_sha384_init(&s384);
	br_sha384_update(&s384, "abc", 3);
	br_sha384_out(&s384, h);
	check_hex("SHA-384(abc) [64-bit arithmetic]", h, 48,
		"cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
		"8086072ba1e7cc2358baeca134c825a7");

	/* RFC 7748 section 5.2, first vector, on every Curve25519 impl */
	{
		static const struct { const char *name; const br_ec_impl *impl; } c[] = {
			{ "X25519 KAT c25519_i15", &br_ec_c25519_i15 },
			{ "X25519 KAT c25519_i31", &br_ec_c25519_i31 },
			{ "X25519 KAT c25519_m15", &br_ec_c25519_m15 },
			{ "X25519 KAT c25519_m31", &br_ec_c25519_m31 },
		};
		size_t i;

		for (i = 0; i < sizeof c / sizeof c[0]; i++) {
			unhex(k, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
			unhex(u, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
			c[i].impl->mul(u, 32, k, 32, BR_EC_curve25519);
			check_hex(c[i].name, u, 32,
				"c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
		}
	}
}

/* --- hashes ------------------------------------------------------------------ */

static unsigned char g_buf[4096];

static void op_sha1(void *a) { br_sha1_context c; (void)a; br_sha1_init(&c); br_sha1_update(&c, g_buf, sizeof g_buf); br_sha1_out(&c, g_buf); }
static void op_sha256(void *a) { br_sha256_context c; (void)a; br_sha256_init(&c); br_sha256_update(&c, g_buf, sizeof g_buf); br_sha256_out(&c, g_buf); }
static void op_sha384(void *a) { br_sha384_context c; (void)a; br_sha384_init(&c); br_sha384_update(&c, g_buf, sizeof g_buf); br_sha384_out(&c, g_buf); }

static void group_hash(void)
{
	printf("hash\n");
	bench("SHA-1", op_sha1, 0, sizeof g_buf);
	bench("SHA-256", op_sha256, 0, sizeof g_buf);
	bench("SHA-384", op_sha384, 0, sizeof g_buf);
}

/* --- symmetric: ChaCha20-Poly1305 and AES-GCM -------------------------------- */

static const unsigned char g_key[32] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
	17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32
};
static const unsigned char g_iv[12] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2 };

struct aead {
	const char *name;
	br_poly1305_run poly;			/* ChaCha20-Poly1305 */
	const br_block_ctr_class *aes;		/* or AES-GCM */
	br_ghash ghash;
};

static void aead_run(const struct aead *a, unsigned char *tag)
{
	if (a->poly)
		a->poly(g_key, g_iv, g_buf, sizeof g_buf, "aad", 3, tag,
			br_chacha20_ct_run, 1);
	else {
		br_aes_gen_ctr_keys keys;
		br_gcm_context gcm;

		a->aes->init(&keys.vtable, g_key, 16);
		br_gcm_init(&gcm, &keys.vtable, a->ghash);
		br_gcm_reset(&gcm, g_iv, sizeof g_iv);
		br_gcm_aad_inject(&gcm, "aad", 3);
		br_gcm_flip(&gcm);
		br_gcm_run(&gcm, 1, g_buf, sizeof g_buf);
		br_gcm_get_tag(&gcm, tag);
	}
}

static void op_aead(void *arg)
{
	unsigned char tag[16];

	aead_run(arg, tag);
}

static const struct aead g_aeads[] = {
	{ "ChaCha20-Poly1305 ctmul", br_poly1305_ctmul_run, 0, 0 },
	{ "ChaCha20-Poly1305 ctmul32", br_poly1305_ctmul32_run, 0, 0 },
	{ "ChaCha20-Poly1305 i15", br_poly1305_i15_run, 0, 0 },
	{ "AES128-GCM big/ctmul", 0, &br_aes_big_ctr_vtable, br_ghash_ctmul },
	{ "AES128-GCM big/ctmul32", 0, &br_aes_big_ctr_vtable, br_ghash_ctmul32 },
	{ "AES128-GCM small/ctmul32", 0, &br_aes_small_ctr_vtable, br_ghash_ctmul32 },
	{ "AES128-GCM ct/ctmul", 0, &br_aes_ct_ctr_vtable, br_ghash_ctmul },
	{ "AES128-GCM ct/ctmul32", 0, &br_aes_ct_ctr_vtable, br_ghash_ctmul32 },
};
#define NAEAD	(sizeof g_aeads / sizeof g_aeads[0])

static void group_sym(void)
{
	unsigned char ref_cc[4096], ref_gcm[4096], ref_cct[16], ref_gcmt[16];
	unsigned char tag[16];
	size_t i;

	printf("sym\n");
	fp_start();
	for (i = 0; i < NAEAD; i++) {
		const struct aead *a = &g_aeads[i];
		int first = i == 0 || (a->aes && !g_aeads[i - 1].aes);
		size_t j;

		for (j = 0; j < sizeof g_buf; j++)
			g_buf[j] = (unsigned char)(j * 7 + 3);
		aead_run(a, tag);
		if (first) {
			memcpy(a->poly ? ref_cc : ref_gcm, g_buf, sizeof g_buf);
			memcpy(a->poly ? ref_cct : ref_gcmt, tag, 16);
			fp_add(g_buf, sizeof g_buf);
			fp_add(tag, 16);
		} else {
			char what[64];

			snprintf(what, sizeof what, "%s agrees", a->name);
			check(what, memcmp(a->poly ? ref_cc : ref_gcm, g_buf, sizeof g_buf) == 0
				&& memcmp(a->poly ? ref_cct : ref_gcmt, tag, 16) == 0);
		}
	}
	fp_print("sym");
	for (i = 0; i < NAEAD; i++)
		bench(g_aeads[i].name, op_aead, (void *)&g_aeads[i], sizeof g_buf);
}

/* --- key exchange: X25519 and P-256 ----------------------------------------- */

struct ecop {
	const char *name;
	const br_ec_impl *impl;
	int curve;
	int gen;		/* mulgen (own public key) rather than mul (shared secret) */
};

static unsigned char g_scalar[48];

static void op_ec(void *arg)
{
	const struct ecop *e = arg;
	unsigned char pt[133];
	size_t glen, xlen = e->curve == BR_EC_secp384r1 ? 48 : 32;
	const unsigned char *g = e->impl->generator(e->curve, &glen);

	if (e->gen)
		e->impl->mulgen(pt, g_scalar, xlen, e->curve);
	else {
		memcpy(pt, g, glen);
		e->impl->mul(pt, glen, g_scalar, xlen, e->curve);
	}
}

static const struct ecop g_ecops[] = {
	{ "X25519 mul c25519_i15", &br_ec_c25519_i15, BR_EC_curve25519, 0 },
	{ "X25519 mul c25519_i31", &br_ec_c25519_i31, BR_EC_curve25519, 0 },
	{ "X25519 mul c25519_m15", &br_ec_c25519_m15, BR_EC_curve25519, 0 },
	{ "X25519 mul c25519_m31", &br_ec_c25519_m31, BR_EC_curve25519, 0 },
	{ "P-256 mulgen p256_m15", &br_ec_p256_m15, BR_EC_secp256r1, 1 },
	{ "P-256 mul p256_m15", &br_ec_p256_m15, BR_EC_secp256r1, 0 },
	{ "P-256 mulgen p256_m31", &br_ec_p256_m31, BR_EC_secp256r1, 1 },
	{ "P-256 mul p256_m31", &br_ec_p256_m31, BR_EC_secp256r1, 0 },
	{ "P-256 mul prime_i15", &br_ec_prime_i15, BR_EC_secp256r1, 0 },
	{ "P-256 mul prime_i31", &br_ec_prime_i31, BR_EC_secp256r1, 0 },
	{ "P-384 mul prime_i15", &br_ec_prime_i15, BR_EC_secp384r1, 0 },
	{ "P-384 mul prime_i31", &br_ec_prime_i31, BR_EC_secp384r1, 0 },
};
#define NECOP	(sizeof g_ecops / sizeof g_ecops[0])

static void group_ec(void)
{
	size_t i;

	printf("ec\n");
	for (i = 0; i < sizeof g_scalar; i++)
		g_scalar[i] = (unsigned char)(0x11 + i * 3);
	g_scalar[0] = 0x3f;		/* below every curve order */
	/* all implementations of the same curve must agree */
	fp_start();
	for (i = 0; i < NECOP; i++) {
		const struct ecop *e = &g_ecops[i];
		unsigned char pt[133];
		size_t glen, xlen = e->curve == BR_EC_secp384r1 ? 48 : 32;
		const unsigned char *g = e->impl->generator(e->curve, &glen);

		memcpy(pt, g, glen);
		e->impl->mul(pt, glen, g_scalar, xlen, e->curve);
		fp_add(pt, glen);
	}
	fp_print("ec: all impls' results in order");
	for (i = 0; i < NECOP; i++)
		bench(g_ecops[i].name, op_ec, (void *)&g_ecops[i], 0);
}

/* --- ECDSA verify (certificate chains) ------------------------------------- */

struct ecdsa {
	const char *name;
	br_ecdsa_vrfy vrfy;
	const br_ec_impl *impl;
	int curve;
	unsigned char q[97];		/* public key */
	size_t qlen;
	unsigned char sig[160];
	size_t siglen;
	unsigned char hash[48];
	size_t hlen;
};

static void op_ecdsa(void *arg)
{
	struct ecdsa *e = arg;
	br_ec_public_key pk;

	pk.curve = e->curve;
	pk.q = e->q;
	pk.qlen = e->qlen;
	e->vrfy(e->impl, e->hash, e->hlen, &pk, e->sig, e->siglen);
}

static struct ecdsa g_ecdsas[] = {
	{ "ECDSA P-256 verify i15/p256_m15", br_ecdsa_i15_vrfy_asn1, &br_ec_p256_m15, BR_EC_secp256r1 },
	{ "ECDSA P-256 verify i31/p256_m31", br_ecdsa_i31_vrfy_asn1, &br_ec_p256_m31, BR_EC_secp256r1 },
	{ "ECDSA P-256 verify i15/prime_i15", br_ecdsa_i15_vrfy_asn1, &br_ec_prime_i15, BR_EC_secp256r1 },
	{ "ECDSA P-384 verify i15/prime_i15", br_ecdsa_i15_vrfy_asn1, &br_ec_prime_i15, BR_EC_secp384r1 },
	{ "ECDSA P-384 verify i31/prime_i31", br_ecdsa_i31_vrfy_asn1, &br_ec_prime_i31, BR_EC_secp384r1 },
};
#define NECDSA	(sizeof g_ecdsas / sizeof g_ecdsas[0])

static void group_ecdsa(void)
{
	unsigned char skx[48];
	size_t i;

	printf("ecdsa\n");
	for (i = 0; i < sizeof skx; i++)
		skx[i] = (unsigned char)(0x5a ^ (i * 13));
	skx[0] = 0x21;
	fp_start();
	for (i = 0; i < NECDSA; i++) {
		struct ecdsa *e = &g_ecdsas[i];
		br_ec_private_key sk;
		br_ec_public_key pk;
		unsigned char kbuf[BR_EC_KBUF_PUB_MAX_SIZE];
		const br_hash_class *hf;
		char what[64];

		if (e->curve == BR_EC_secp384r1) {
			br_sha384_context h;
			hf = &br_sha384_vtable;
			br_sha384_init(&h);
			br_sha384_update(&h, "certificate", 11);
			br_sha384_out(&h, e->hash);
			e->hlen = 48;
		} else {
			br_sha256_context h;
			hf = &br_sha256_vtable;
			br_sha256_init(&h);
			br_sha256_update(&h, "certificate", 11);
			br_sha256_out(&h, e->hash);
			e->hlen = 32;
		}
		sk.curve = e->curve;
		sk.x = skx;
		sk.xlen = e->hlen;
		br_ec_compute_pub(e->impl, &pk, kbuf, &sk);
		memcpy(e->q, pk.q, pk.qlen);
		e->qlen = pk.qlen;
		/* RFC 6979: deterministic, so the fingerprint is stable */
		e->siglen = br_ecdsa_i15_sign_asn1(e->impl, hf, e->hash, &sk, e->sig);
		fp_add(e->sig, e->siglen);
		pk.q = e->q;
		snprintf(what, sizeof what, "%.28s good", e->name);
		check(what, e->vrfy(e->impl, e->hash, e->hlen, &pk, e->sig, e->siglen) == 1);
		e->hash[3] ^= 1;
		snprintf(what, sizeof what, "%.28s bad", e->name);
		check(what, e->vrfy(e->impl, e->hash, e->hlen, &pk, e->sig, e->siglen) == 0);
		e->hash[3] ^= 1;
	}
	fp_print("ecdsa signatures");
	for (i = 0; i < NECDSA; i++)
		bench(g_ecdsas[i].name, op_ecdsa, &g_ecdsas[i], 0);
}

/* --- RSA public operation (certificate chains, RSA key exchange) --------- */

struct rsaop {
	const char *name;
	br_rsa_public pub;
	int bits;
};

static unsigned char g_n2048[256], g_n4096[512];
static const unsigned char g_e[3] = { 1, 0, 1 };

static uint32_t rsa_run(const struct rsaop *r, unsigned char *x)
{
	br_rsa_public_key pk;

	pk.n = r->bits == 2048 ? g_n2048 : g_n4096;
	pk.nlen = r->bits / 8;
	pk.e = (unsigned char *)g_e;
	pk.elen = sizeof g_e;
	memset(x, 0x42, pk.nlen);
	x[0] = 0x01;		/* below the modulus */
	return r->pub(x, pk.nlen, &pk);
}

static void op_rsa(void *arg)
{
	unsigned char x[512];

	rsa_run(arg, x);
}

static const struct rsaop g_rsaops[] = {
	{ "RSA-2048 public i15", br_rsa_i15_public, 2048 },
	{ "RSA-2048 public i31", br_rsa_i31_public, 2048 },
	{ "RSA-2048 public i32", br_rsa_i32_public, 2048 },
	{ "RSA-4096 public i15", br_rsa_i15_public, 4096 },
	{ "RSA-4096 public i31", br_rsa_i31_public, 4096 },
	{ "RSA-4096 public i32", br_rsa_i32_public, 4096 },
};
#define NRSA	(sizeof g_rsaops / sizeof g_rsaops[0])

static void group_rsa(void)
{
	unsigned char ref[512], x[512];
	size_t i;

	printf("rsa\n");
	unhex(g_n2048, rsa2048_n_hex);
	unhex(g_n4096, rsa4096_n_hex);
	fp_start();
	for (i = 0; i < NRSA; i++) {
		const struct rsaop *r = &g_rsaops[i];
		char what[64];
		uint32_t ok = rsa_run(r, x);

		if (i % 3 == 0) {
			memcpy(ref, x, r->bits / 8);
			fp_add(x, r->bits / 8);
			snprintf(what, sizeof what, "%s", r->name);
			check(what, ok == 1);
		} else {
			snprintf(what, sizeof what, "%s agrees", r->name);
			check(what, ok == 1 && memcmp(ref, x, r->bits / 8) == 0);
		}
	}
	fp_print("rsa");
	for (i = 0; i < NRSA; i++)
		bench(g_rsaops[i].name, op_rsa, (void *)&g_rsaops[i], 0);
}

/* --------------------------------------------------------------------------- */

static const struct { const char *name; void (*run)(void); } g_groups[] = {
	{ "kat", group_kat },
	{ "hash", group_hash },
	{ "sym", group_sym },
	{ "ec", group_ec },
	{ "ecdsa", group_ecdsa },
	{ "rsa", group_rsa },
};
#define NGROUP	(sizeof g_groups / sizeof g_groups[0])

int main(int argc, char **argv)
{
	size_t i;
	int a, any = 0;
	unsigned long t0;

	if (argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9') {
		g_min_ms = strtoul(argv[1], 0, 10);
		argc--;
		argv++;
	}
	setvbuf(stdout, 0, _IOLBF, 0);
	t0 = os_msec();
	for (i = 0; i < NGROUP; i++) {
		int want = argc <= 1;

		for (a = 1; a < argc; a++)
			if (strcmp(argv[a], g_groups[i].name) == 0)
				want = 1;
		if (want) {
			g_groups[i].run();
			any = 1;
		}
	}
	if (!any) {
		fprintf(stderr, "usage: tlsbench [min_ms] [kat hash sym ec ecdsa rsa]\n");
		return 2;
	}
	printf("%d check(s) failed, %lu s total\n", g_fail,
		(os_msec() - t0 + 500) / 1000);
	return g_fail != 0;
}
