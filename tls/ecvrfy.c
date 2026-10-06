/*
 * ecvrfy.c - ECDSA signatures checked on P-256 and P-384 (ecvrfy.h):
 * BearSSL's side of sigmath.c. The checks are all here: the key a point
 * of the curve, r and s in range, u1 = h/s and u2 = r/s, then the x of
 * u1 G + u2 Q against r.
 */
#include <string.h>
#include "sigmath.h"
#include "sigpre.h"
#include "ecvrfy.h"

uint32_t ecvrfy_raw(const br_ec_impl *impl, const void *hash, size_t hash_len,
	const br_ec_public_key *pk, const void *sig, size_t sig_len)
{
	const unsigned char *sg = sig;
	unsigned char u1[48], u2[48], x[48];
	size_t len = sig_ec_len(pk->curve), rlen;
	int ahead;

	if (len == 0)		/* (P-521) */
#if BR_NO_U64
		return br_ecdsa_i15_vrfy_raw(impl, hash, hash_len, pk, sig, sig_len);
#else
		return br_ecdsa_i31_vrfy_raw(impl, hash, hash_len, pk, sig, sig_len);
#endif
	if (sig_len == 0 || (sig_len & 1) || pk->qlen != 1 + 2 * len)
		return 0;
	rlen = sig_len / 2;
	if (!sig_ec_point(pk->curve, pk->q, pk->qlen)
		|| !sig_ec_scalars(pk->curve, u1, u2, sg, sg + rlen, rlen, hash, hash_len))
		return 0;
	/* u1 G + u2 Q done ahead (on the T425, say)? */
	ahead = sigpre_ec(pk->curve, x, pk->q, u1, u2);
	if (ahead == 0 || (ahead < 0 && !sig_ec_muladd(pk->curve, x, pk->q, u1, u2)))
		return 0;
	return (uint32_t)sig_ec_x_is_r(pk->curve, x, sg, rlen);
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
