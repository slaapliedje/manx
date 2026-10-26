/*
 * chapol_m68k.c - ChaCha20 and Poly1305 for the 68020, 68030 and 68040,
 * as BearSSL's br_chacha20_run and br_poly1305_run (crypt68k_bearssl.h):
 * the record cipher and MAC of TLS's ChaCha20-Poly1305 suites, which are
 * the fastest a 68030 has. Part of crypt68k, MIT licence (LICENSE).
 *
 * ChaCha20: the 20 rounds run with the state in registers (see
 * CHACHA_ROUNDS); the rotates are the 68020's barrel shifter's (constant
 * time), 16 by swap, 12 by swap and 4 back. The key stream is added,
 * byte-swapped to little-endian and exclusive-ored into the data a word
 * at a time (the 68020 and up take misaligned words).
 *
 * Poly1305: BearSSL's "ctmul" arrangement (five 26-bit limbs, the high
 * limbs' multiples of 5 precomputed, a partial carry pass per block;
 * after poly1305-donna), with the 25 limb products on mulu.l's 64-bit
 * result and the sums in register pairs with addx. The carry out of a
 * limb's 58-bit sum fits a register (a_i < 2^27, r_j < 2^26, 5 r_j <
 * 2^29: the sum is under 2^58, the carry under 2^32), as the C relies
 * on too. Constant time: no branch depends on the data.
 *
 * Measured on a 32 MHz 68030 (Atari TT030), one 4096-byte record
 * encrypted and tagged: BearSSL's ctmul 56 KB/s; see README.md for
 * this code's figure.
 */
/*
 * (The immediates are written &n, not #n: inside inline assembly GNU as
 * scrubs the text, and there a # starts a comment.)
 */
#include <string.h>
#include "cpu.h"
#include "crypt68k_bearssl.h"

#if C68K_MULU64

static uint32_t dec32le(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
		| ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void enc32le(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

/* --- ChaCha20 ------------------------------------------------------------ */

/*
 * The 20 rounds on the block in st (a local of the caller, reached
 * through a pointer kept in memory and a few stack-relative operands,
 * since every address register is taken). On this machine a register add or exclusive-or is 5 cycles, a
 * rotate 12, a load 9 and a store 13, so the state stays in registers:
 * the eight words that are only ever added to (0-3 and 8-11) in address
 * registers, where adda does the sum, and the eight that are
 * exclusive-ored and rotated (4-7 and 12-15) in data registers. With a6
 * left to the compiler and d7 as the go-between, words 10 and 11 stay in
 * memory (added to there, read for the exclusive-or) and word 15 borrows
 * d0 for its two quarter rounds, word 4 taking its place in memory
 * meanwhile. A quarter round is then 18 instructions and no store.
 */
#define QR(A, B, C, D) \
	"add.l " B "," A "\n\t" \
	"move.l " A ",%%d7\n\t" \
	"eor.l %%d7," D "\n\t" \
	"swap " D "\n\t" \
	"add.l " D "," C "\n\t" \
	"move.l " C ",%%d7\n\t" \
	"eor.l %%d7," B "\n\t" \
	"swap " B "\n\t" \
	"ror.l &4," B "\n\t" \
	"add.l " B "," A "\n\t" \
	"move.l " A ",%%d7\n\t" \
	"eor.l %%d7," D "\n\t" \
	"rol.l &8," D "\n\t" \
	"add.l " D "," C "\n\t" \
	"move.l " C ",%%d7\n\t" \
	"eor.l %%d7," B "\n\t" \
	"rol.l &7," B "\n\t"
/* word 15 into d0 for a quarter round, word 4 kept in memory meanwhile */
#define W15_IN	"move.l %%d0,%[w4]\n\tmove.l %[w15],%%d0\n\t"
#define W15_OUT	"move.l %%d0,%[w15]\n\tmove.l %[w4],%%d0\n\t"

#define CHACHA_ROUNDS(st, stp, cnt) \
	__asm__ volatile( \
		"move.l %[p],%%a0\n\t" \
		"move.l 16(%%a0),%%d0\n\t" \
		"move.l 20(%%a0),%%d1\n\t" \
		"move.l 24(%%a0),%%d2\n\t" \
		"move.l 28(%%a0),%%d3\n\t" \
		"move.l 48(%%a0),%%d4\n\t" \
		"move.l 52(%%a0),%%d5\n\t" \
		"move.l 56(%%a0),%%d6\n\t" \
		"move.l 32(%%a0),%%a4\n\t" \
		"move.l 36(%%a0),%%a5\n\t" \
		"move.l 4(%%a0),%%a1\n\t" \
		"move.l 8(%%a0),%%a2\n\t" \
		"move.l 12(%%a0),%%a3\n\t" \
		"move.l (%%a0),%%a0\n\t" \
		"1:\n\t" \
		QR("%%a0", "%%d0", "%%a4", "%%d4")		/* 0 4 8 12 */ \
		QR("%%a1", "%%d1", "%%a5", "%%d5")		/* 1 5 9 13 */ \
		QR("%%a2", "%%d2", "%[w10]", "%%d6")		/* 2 6 10 14 */ \
		W15_IN \
		QR("%%a3", "%%d3", "%[w11]", "%%d0")		/* 3 7 11 15 */ \
		W15_OUT \
		W15_IN \
		QR("%%a0", "%%d1", "%[w10]", "%%d0")		/* 0 5 10 15 */ \
		W15_OUT \
		QR("%%a1", "%%d2", "%[w11]", "%%d4")		/* 1 6 11 12 */ \
		QR("%%a2", "%%d3", "%%a4", "%%d5")		/* 2 7 8 13 */ \
		QR("%%a3", "%%d0", "%%a5", "%%d6")		/* 3 4 9 14 */ \
		"subq.l &1,%[cnt]\n\t" \
		"bne 1b\n\t" \
		"move.l %%a0,%%d7\n\t" \
		"move.l %[p],%%a0\n\t" \
		"move.l %%d7,(%%a0)\n\t" \
		"move.l %%a1,4(%%a0)\n\t" \
		"move.l %%a2,8(%%a0)\n\t" \
		"move.l %%a3,12(%%a0)\n\t" \
		"move.l %%d0,16(%%a0)\n\t" \
		"move.l %%d1,20(%%a0)\n\t" \
		"move.l %%d2,24(%%a0)\n\t" \
		"move.l %%d3,28(%%a0)\n\t" \
		"move.l %%a4,32(%%a0)\n\t" \
		"move.l %%a5,36(%%a0)\n\t" \
		"move.l %%d4,48(%%a0)\n\t" \
		"move.l %%d5,52(%%a0)\n\t" \
		"move.l %%d6,56(%%a0)\n\t" \
		: [w4] "+m" (st[4]), [w10] "+m" (st[10]), [w11] "+m" (st[11]), \
		  [w15] "+m" (st[15]), [cnt] "+m" (cnt) \
		: [p] "m" (stp) \
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", \
		  "a0", "a1", "a2", "a3", "a4", "a5", "memory", "cc")

/* one key stream word, at byte offset o: rounds + input, little-endian,
 * exclusive-ored into the data */
#define XW(o) \
	"move.l " #o "(%[s]),%%d0\n\t" \
	"add.l " #o "(%[in]),%%d0\n\t" \
	"ror.w &8,%%d0\n\t" \
	"swap %%d0\n\t" \
	"ror.w &8,%%d0\n\t" \
	"eor.l %%d0,(%[b])+\n\t"

/* 64 bytes of data exclusive-ored with the block's key stream */
static void chacha_xor64(const uint32_t *s, const uint32_t *in, unsigned char *buf)
{
	__asm__ volatile(
		XW(0) XW(4) XW(8) XW(12) XW(16) XW(20) XW(24) XW(28)
		XW(32) XW(36) XW(40) XW(44) XW(48) XW(52) XW(56) XW(60)
		: [b] "+a" (buf)
		: [s] "a" (s), [in] "a" (in)
		: "d0", "memory", "cc");
}

uint32_t c68k_br_chacha20_run(const void *key, const void *iv, uint32_t cc,
	void *data, size_t len)
{
	unsigned char *buf = data;
	uint32_t in[16], st[16];
	size_t u;

	in[0] = 0x61707865;
	in[1] = 0x3320646e;
	in[2] = 0x79622d32;
	in[3] = 0x6b206574;
	for (u = 0; u < 8; u++)
		in[4 + u] = dec32le((const unsigned char *)key + (u << 2));
	in[12] = cc;
	for (u = 0; u < 3; u++)
		in[13 + u] = dec32le((const unsigned char *)iv + (u << 2));
	while (len > 0) {
		int cnt = 10;
		uint32_t *stp = st;

		memcpy(st, in, sizeof st);
		CHACHA_ROUNDS(st, stp, cnt);
		if (len >= 64) {
			chacha_xor64(st, in, buf);
			buf += 64;
			len -= 64;
		} else {
			unsigned char ks[64];

			memset(ks, 0, sizeof ks);
			chacha_xor64(st, in, ks);	/* (the key stream itself) */
			for (u = 0; u < len; u++)
				buf[u] ^= ks[u];
			len = 0;
		}
		in[12] = ++cc;
	}
	return cc;
}

/* --- Poly1305 ------------------------------------------------------------ */

/* the carry out of the pair d3:d2 into d4 (a shift by 26: rotate by 6 and
 * mask, the count being too big for an immediate), the limb into acc at k,
 * then the next pair loaded from w at nlo/nhi and the carry added */
#define CARRY(k, nlo, nhi) \
	"move.l %%d3,%%d4\n\t" \
	"lsl.l &6,%%d4\n\t" \
	"move.l %%d2,%%d5\n\t" \
	"rol.l &6,%%d5\n\t" \
	"and.l &63,%%d5\n\t" \
	"or.l %%d5,%%d4\n\t" \
	"and.l &67108863,%%d2\n\t" \
	"move.l %%d2," #k "(%[acc])\n\t" \
	"move.l " #nlo "(%[w]),%%d2\n\t" \
	"move.l " #nhi "(%[w]),%%d3\n\t" \
	"add.l %%d4,%%d2\n\t" \
	"moveq &0,%%d5\n\t" \
	"addx.l %%d5,%%d3\n\t"

/* a little-endian word of the block at offset o, shifted down by s,
 * masked to 26 bits, added to the limb at offset k */
#define LIMB(o, s, k) \
	"move.l " #o "(%[b]),%%d0\n\t" \
	"ror.w &8,%%d0\n\t" \
	"swap %%d0\n\t" \
	"ror.w &8,%%d0\n\t" \
	"lsr.l &" #s ",%%d0\n\t" \
	"and.l &67108863,%%d0\n\t" \
	"add.l %%d0," #k "(%[acc])\n\t"

/*
 * nblk whole 16-byte blocks into the accumulator (five 26-bit limbs).
 * t holds 5 r1, 5 r2, 5 r3, 5 r4, r0, r1, r2, r3, r4: limb sum k is
 * acc_i times t[4 + k - i], i = 0..4, so each sum walks t downwards
 * from t[4 + k] (mulu.l's predecrement) while it walks acc upwards.
 * Loops rather than 25 unrolled products: the block then fits the
 * 68030's instruction cache. w is scratch for the five 64-bit sums.
 */
static void poly_blocks(uint32_t *acc, const uint32_t *t, const unsigned char *buf,
	size_t nblk, uint32_t *w)
{
	__asm__ volatile(
		"1:\n\t"
		/* the block, with its high bit, into the limbs */
		"move.l 0(%[b]),%%d0\n\t"
		"ror.w &8,%%d0\n\t"
		"swap %%d0\n\t"
		"ror.w &8,%%d0\n\t"
		"and.l &67108863,%%d0\n\t"
		"add.l %%d0,0(%[acc])\n\t"
		LIMB(3, 2, 4)
		LIMB(6, 4, 8)
		LIMB(9, 6, 12)
		"move.l 12(%[b]),%%d0\n\t"
		"ror.w &8,%%d0\n\t"
		"swap %%d0\n\t"
		"ror.w &8,%%d0\n\t"
		"lsr.l &8,%%d0\n\t"
		"or.l &16777216,%%d0\n\t"
		"add.l %%d0,16(%[acc])\n\t"
		/* the five sums, into w, from the limbs before any is replaced:
		 * d4 = one past t[4 + k], d7 = k's countdown */
		"lea 20(%[t]),%%a1\n\t"
		"move.l %%a1,%%d4\n\t"
		"moveq &4,%%d7\n\t"
		"2:\n\t"
		"move.l %[acc],%%a0\n\t"
		"move.l %%d4,%%a1\n\t"
		"moveq &0,%%d2\n\t"
		"moveq &0,%%d3\n\t"
		"moveq &4,%%d5\n\t"
		"3:\n\t"
		"move.l (%%a0)+,%%d0\n\t"
		"mulu.l -(%%a1),%%d1:%%d0\n\t"
		"add.l %%d0,%%d2\n\t"
		"addx.l %%d1,%%d3\n\t"
		"dbra %%d5,3b\n\t"
		"move.l %%d2,(%[w])+\n\t"
		"move.l %%d3,(%[w])+\n\t"
		"addq.l &4,%%d4\n\t"
		"dbra %%d7,2b\n\t"
		"lea -40(%[w]),%[w]\n\t"
		/* the carries, upwards from w0 */
		"move.l 0(%[w]),%%d2\n\t"
		"move.l 4(%[w]),%%d3\n\t"
		CARRY(0, 8, 12)
		CARRY(4, 16, 20)
		CARRY(8, 24, 28)
		CARRY(12, 32, 36)
		/* the top limb: its carry, times 5, back into limb 0 */
		"move.l %%d3,%%d4\n\t"
		"lsl.l &6,%%d4\n\t"
		"move.l %%d2,%%d5\n\t"
		"rol.l &6,%%d5\n\t"
		"and.l &63,%%d5\n\t"
		"or.l %%d5,%%d4\n\t"
		"and.l &67108863,%%d2\n\t"
		"move.l %%d2,16(%[acc])\n\t"
		"move.l %%d4,%%d5\n\t"
		"lsl.l &2,%%d5\n\t"
		"add.l %%d4,%%d5\n\t"
		"add.l 0(%[acc]),%%d5\n\t"
		"move.l %%d5,%%d0\n\t"
		"rol.l &6,%%d0\n\t"
		"and.l &63,%%d0\n\t"
		"add.l %%d0,4(%[acc])\n\t"
		"and.l &67108863,%%d5\n\t"
		"move.l %%d5,0(%[acc])\n\t"
		"lea 16(%[b]),%[b]\n\t"
		"subq.l &1,%[n]\n\t"
		"bne 1b\n\t"
		: [b] "+a" (buf), [n] "+d" (nblk), [w] "+a" (w)
		: [acc] "a" (acc), [t] "a" (t)
		: "d0", "d1", "d2", "d3", "d4", "d5", "d7", "a0", "a1",
		  "memory", "cc");
}

static void poly_inner(uint32_t *acc, const uint32_t *t, const void *data, size_t len)
{
	const unsigned char *buf = data;
	size_t nb = len >> 4;
	uint32_t w[10];

	if (nb)
		poly_blocks(acc, t, buf, nb, w);
	if (len & 15) {
		unsigned char tmp[16];

		memset(tmp, 0, sizeof tmp);
		memcpy(tmp, buf + (nb << 4), len & 15);
		poly_blocks(acc, t, tmp, 1, w);
	}
}

/* constant-time comparisons and choice, as BearSSL's */
static uint32_t ct_eq(uint32_t x, uint32_t y)
{
	uint32_t q = x ^ y;

	return ((q | -q) >> 31) ^ 1;
}

static uint32_t ct_gt(uint32_t x, uint32_t y)
{
	uint32_t z = y - x;

	return (z ^ ((x ^ y) & (x ^ z))) >> 31;
}

static uint32_t ct_mux(uint32_t ctl, uint32_t x, uint32_t y)
{
	return y ^ (-ctl & (x ^ y));
}

void c68k_br_poly1305_run(const void *key, const void *iv, void *data, size_t len,
	const void *aad, size_t aad_len, void *tag, br_chacha20_run ichacha,
	int encrypt)
{
	unsigned char pkey[32], foot[16];
	uint32_t t[9], acc[5], cc, ctl, hi;
	uint32_t lo, carry;
	int i;

	/* the MAC key: the first 16 bytes of the cipher's block 0, as the
	 * five clamped 26-bit limbs of r, after 5 r1..5 r4 (poly_blocks) */
	memset(pkey, 0, sizeof pkey);
	ichacha(key, iv, 0, pkey, sizeof pkey);
	if (encrypt)
		ichacha(key, iv, 1, data, len);
	t[4] = dec32le(pkey) & 0x03FFFFFF;
	t[5] = (dec32le(pkey + 3) >> 2) & 0x03FFFF03;
	t[6] = (dec32le(pkey + 6) >> 4) & 0x03FFC0FF;
	t[7] = (dec32le(pkey + 9) >> 6) & 0x03F03FFF;
	t[8] = (dec32le(pkey + 12) >> 8) & 0x000FFFFF;
	for (i = 0; i < 4; i++)
		t[i] = t[5 + i] * 5;
	memset(acc, 0, sizeof acc);

	/* the AAD, the ciphertext and the footer with their lengths, each
	 * zero-padded to 16 bytes */
	enc32le(foot, (uint32_t)aad_len);
	enc32le(foot + 4, 0);
	enc32le(foot + 8, (uint32_t)len);
	enc32le(foot + 12, 0);
	poly_inner(acc, t, aad, aad_len);
	poly_inner(acc, t, data, len);
	poly_inner(acc, t, foot, sizeof foot);

	/* a full carry pass, then the value reduced below 2^130 - 5 if it
	 * is not already */
	cc = 0;
	for (i = 1; i <= 6; i++) {
		int j = i >= 5 ? i - 5 : i;

		acc[j] += cc;
		cc = acc[j] >> 26;
		acc[j] &= 0x03FFFFFF;
	}
	ctl = ct_gt(acc[0], 0x03FFFFFA);
	for (i = 1; i < 5; i++)
		ctl &= ct_eq(acc[i], 0x03FFFFFF);
	cc = 5;
	for (i = 0; i < 5; i++) {
		uint32_t v = acc[i] + cc;

		cc = v >> 26;
		v &= 0x03FFFFFF;
		acc[i] = ct_mux(ctl, v, acc[i]);
	}

	/* the tag: the limbs as 128 bits, plus s (the key's second half),
	 * in 32-bit pieces with the carry kept by hand (no 64-bit ints) */
	lo = acc[1] << 26;
	carry = acc[1] >> 6;			/* the bits of acc[1] << 26 past 32 */
	hi = lo;
	lo += acc[0];
	carry += lo < hi;
	hi = lo;
	lo += dec32le(pkey + 16);
	carry += lo < hi;
	enc32le((unsigned char *)tag, lo);

	hi = carry;
	lo = hi + (acc[2] << 20);
	carry = (lo < hi) + (acc[2] >> 12);
	hi = lo;
	lo += dec32le(pkey + 20);
	carry += lo < hi;
	enc32le((unsigned char *)tag + 4, lo);

	hi = carry;
	lo = hi + (acc[3] << 14);
	carry = (lo < hi) + (acc[3] >> 18);
	hi = lo;
	lo += dec32le(pkey + 24);
	carry += lo < hi;
	enc32le((unsigned char *)tag + 8, lo);

	hi = carry + (acc[4] << 8) + dec32le(pkey + 28);
	enc32le((unsigned char *)tag + 12, hi);

	if (!encrypt)
		ichacha(key, iv, 1, data, len);
}

#endif /* C68K_MULU64 */
