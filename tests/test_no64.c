/*
 * test_no64.c - BearSSL as a compiler without 64-bit integers builds it
 * (BR_NO_U64: Helios C on the transputer). `make TARGET=no64 test-no64`
 * builds this with uint64_t 32 bits wide (tests/no_u64.h).
 *
 * BearSSL's own test functions, from its test_crypto.c, for the code
 * such a build has: the hashes (SHA-384/512 on 32-bit halves), HMAC, the
 * DRBG, the TLS PRF, AES ct, DES, ChaCha20, Poly1305 ctmul32, GHASH
 * ctmul32, RSA i15, X25519 m15, and ECDSA i15 over br_ec_all_m15 (P-256
 * on m15, P-384 and P-521 on i15). Its EC_* tests are left out: they
 * compute their expected values with the 64-bit i31 code. Instead, P-256
 * m15 must agree with prime_i15 on random scalars.
 */
#define main bearssl_test_crypto_main
#include "../third_party/bearssl/test/test_crypto.c"
#undef main

static void test_P256_m15_vs_i15(void)
{
	const br_ec_impl *m15 = &br_ec_p256_m15, *i15 = &br_ec_prime_i15;
	const int curve = BR_EC_secp256r1;
	br_hmac_drbg_context rng;
	size_t glen;
	const unsigned char *g = i15->generator(curve, &glen);
	int i;

	printf("Test P-256 m15 against prime_i15: ");
	fflush(stdout);
	br_hmac_drbg_init(&rng, &br_sha256_vtable, "test_no64", 9);
	for (i = 0; i < 40; i++) {
		unsigned char k1[32], k2[32], p1[65], p2[65], q1[65], q2[65];

		/* random scalars below the order (top bit clear) */
		br_hmac_drbg_generate(&rng, k1, sizeof k1);
		br_hmac_drbg_generate(&rng, k2, sizeof k2);
		k1[0] &= 0x7F;
		k2[0] &= 0x7F;
		k1[31] |= 1;
		k2[31] |= 1;
		if (m15->mulgen(p1, k1, 32, curve) != 65
			|| i15->mulgen(p2, k1, 32, curve) != 65
			|| memcmp(p1, p2, 65) != 0) {
			fprintf(stderr, "mulgen differs (%d)\n", i);
			exit(EXIT_FAILURE);
		}
		memcpy(q1, p1, 65);
		memcpy(q2, p1, 65);
		if (m15->mul(q1, 65, k2, 32, curve) != 1
			|| i15->mul(q2, 65, k2, 32, curve) != 1
			|| memcmp(q1, q2, 65) != 0) {
			fprintf(stderr, "mul differs (%d)\n", i);
			exit(EXIT_FAILURE);
		}
		memcpy(q1, p1, 65);
		memcpy(q2, p1, 65);
		if (m15->muladd(q1, g, 65, k1, 32, k2, 32, curve) != 1
			|| i15->muladd(q2, g, 65, k1, 32, k2, 32, curve) != 1
			|| memcmp(q1, q2, 65) != 0) {
			fprintf(stderr, "muladd differs (%d)\n", i);
			exit(EXIT_FAILURE);
		}
		printf(".");
		fflush(stdout);
	}
	printf(" done.\n");
}

int main(void)
{
	test_MD5();
	test_SHA1();
	test_SHA224();
	test_SHA256();
	test_SHA384();
	test_SHA512();
	test_multihash();
	test_HMAC();
	test_HMAC_DRBG();
	test_PRF();
	test_AES_ct();
	test_AES_CTRCBC_ct();
	test_DES_tab();
	test_ChaCha20_ct();
	test_Poly1305_ctmul32();
	test_GHASH_ctmul32();
	test_RSA_i15();
	test_EC_c25519_m15();
	printf("Test ECDSA i15 over all_m15: [raw]");
	fflush(stdout);
	test_ECDSA_KAT(&br_ec_all_m15, &br_ecdsa_i15_sign_raw,
		&br_ecdsa_i15_vrfy_raw, 0);
	printf(" [asn1]");
	fflush(stdout);
	test_ECDSA_KAT(&br_ec_all_m15, &br_ecdsa_i15_sign_asn1,
		&br_ecdsa_i15_vrfy_asn1, 1);
	printf(" done.\n");
	test_P256_m15_vs_i15();
	printf("All tests OK.\n");
	return 0;
}
