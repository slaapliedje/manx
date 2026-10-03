/*
 * tpmont - does hand-written transputer code beat the 68030 at the bignum
 * arithmetic of a signature check? RSA-2048's public operation
 * (x^65537 mod n: 19 Montgomery multiplications, 155,648 32x32->64
 * multiply-adds) with 32-bit limbs, the inner loop (r += a * b, one row)
 * done three ways:
 *   - asm:   T800/T425 assembly around lmul (32x32+32 -> 64), unrolled 4x
 *   - c16:   plain C89 from 16-bit halves, as icc compiles it (the check)
 *   - ll:    C with unsigned long long, which GCC makes mulu.l on a 68030
 *   - m68k:  68030 assembly around mulu.l (32x32 -> 64), 9 instructions
 * Every answer is checked against Python's (tpmontvec.h, from mkvec.py).
 * tpcfg.h names the machine and says which loops it has (TP_LMUL, TP_LL,
 * TP_M68K): tp-lmul.sh writes it for a transputer; for the 68030 it is
 *   #define TP_NAME "68030"
 *   #define TP_LL 1
 *   #define TP_M68K 1
 * built with toolchain/sysv4-cc -m68030 -std=gnu89 -O2 and linked with
 * sysv4-ld and build/sysv4/os/sysv4/sysv_rt.o. Results: docs/phase0-results.md.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "tpcfg.h"

typedef unsigned int u32;
#define NL 64
#include "tpmontvec.h"

typedef u32 (*row_fn)(u32 *r, const u32 *a, u32 b, int n);

/* (integers only: AMIX's libc has no soft-float routines) */
static long ms_since(clock_t t0)
{
	long d = (long)(clock() - t0), cps = (long)CLOCKS_PER_SEC;

	return d / cps * 1000 + d % cps * 1000 / cps;
}

/* r[0..n-1] += a[0..n-1] * b; the carry out */
static u32 row_c16(u32 *r, const u32 *a, u32 b, int n)
{
	u32 c = 0, bl = b & 0xffff, bh = b >> 16;
	int i;

	for (i = 0; i < n; i++) {
		u32 al = a[i] & 0xffff, ah = a[i] >> 16;
		u32 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
		u32 mid = lh + hl, lo, hi, s;

		hi = hh + (mid >> 16) + (mid < lh ? 0x10000 : 0);
		lo = ll + (mid << 16);
		hi += lo < ll;
		s = lo + r[i];
		hi += s < lo;
		lo = s + c;
		hi += lo < s;
		r[i] = lo;
		c = hi;
	}
	return c;
}

#ifdef TP_LL
static u32 row_ll(u32 *r, const u32 *a, u32 b, int n)
{
	u32 c = 0;
	int i;

	for (i = 0; i < n; i++) {
		unsigned long long z = (unsigned long long)a[i] * b + r[i] + c;

		r[i] = (u32)z;
		c = (u32)(z >> 32);
	}
	return c;
}
#endif

#ifdef TP_M68K
/* 0 < n <= 65536. add.l to memory leaves the carry in X for the high
 * word. (No immediates: the SVR4 toolchain's wrapper takes '#' for a
 * comment.) */
static u32 row_m68k(u32 *r, const u32 *a, u32 b, int n)
{
	u32 c = 0, z = 0;
	int k = n - 1;

	__asm__ volatile(
		"1:\n\t"
		"move.l (%[a])+,%%d1\n\t"
		"mulu.l %[b],%%d2:%%d1\n\t"
		"add.l %[c],%%d1\n\t"
		"addx.l %[z],%%d2\n\t"
		"add.l %%d1,(%[r])+\n\t"
		"addx.l %[z],%%d2\n\t"
		"move.l %%d2,%[c]\n\t"
		"dbra %[k],1b"
		: [a] "+a" (a), [r] "+a" (r), [c] "+d" (c), [k] "+d" (k), [z] "+d" (z)
		: [b] "d" (b)
		: "d1", "d2", "cc", "memory");
	return c;
}
#endif

#ifdef TP_LMUL
/* n a multiple of 4. The carry stays in Areg from one limb to the next:
 * lmul (Breg*Areg + Creg) takes it as Creg, lsum (Breg + Areg + Creg
 * bit 0) adds the old r[i] to the low word, sum adds its carry to the
 * high word. Backwards with cj, which (unlike j) never deschedules, the
 * carry in the workspace across it: eqc 0 makes the count's "not done"
 * a 0, which cj jumps on. (A label can't end an __asm.) */
static u32 row_asm(u32 *r, const u32 *a, u32 b, int n)
{
	u32 *pr = r;
	const u32 *pa = a;
	u32 bb = b, carry = 0, wlo, whi;
	int cnt = n / 4;

	__asm {
	again:
		ldl carry;
		ldl pa; ldnl 0; ldl bb; lmul; stl wlo; stl whi;
		ldc 0; ldl wlo; ldl pr; ldnl 0; lsum; ldl pr; stnl 0; ldl whi; sum;
		ldl pa; ldnl 1; ldl bb; lmul; stl wlo; stl whi;
		ldc 0; ldl wlo; ldl pr; ldnl 1; lsum; ldl pr; stnl 1; ldl whi; sum;
		ldl pa; ldnl 2; ldl bb; lmul; stl wlo; stl whi;
		ldc 0; ldl wlo; ldl pr; ldnl 2; lsum; ldl pr; stnl 2; ldl whi; sum;
		ldl pa; ldnl 3; ldl bb; lmul; stl wlo; stl whi;
		ldc 0; ldl wlo; ldl pr; ldnl 3; lsum; ldl pr; stnl 3; ldl whi; sum;
		stl carry;
		ldl pa; adc 16; stl pa;
		ldl pr; adc 16; stl pr;
		ldl cnt; adc -1; stl cnt;
		ldl cnt; eqc 0; cj again;
	}
	return carry;
}

/* the clock: 16 x (ldc; ldc; ldc; lmul) a turn, 36 cycles each by the
 * data sheet (lmul 33) */
static long lmul_probe(int turns)
{
	int cnt = turns;
	clock_t t0 = clock();

	__asm {
	ptop:
		ldc 1; ldc 2; ldc 3; lmul; ldc 1; ldc 2; ldc 3; lmul;
		ldc 1; ldc 2; ldc 3; lmul; ldc 1; ldc 2; ldc 3; lmul;
		ldc 1; ldc 2; ldc 3; lmul; ldc 1; ldc 2; ldc 3; lmul;
		ldc 1; ldc 2; ldc 3; lmul; ldc 1; ldc 2; ldc 3; lmul;
		ldc 1; ldc 2; ldc 3; lmul; ldc 1; ldc 2; ldc 3; lmul;
		ldc 1; ldc 2; ldc 3; lmul; ldc 1; ldc 2; ldc 3; lmul;
		ldc 1; ldc 2; ldc 3; lmul; ldc 1; ldc 2; ldc 3; lmul;
		ldc 1; ldc 2; ldc 3; lmul; ldc 1; ldc 2; ldc 3; lmul;
		ldl cnt; adc -1; stl cnt;
		ldl cnt; eqc 0; cj ptop;
	}
	return ms_since(t0);
}
#endif

static int cmp_n(const u32 *t)
{
	int i;

	for (i = NL - 1; i >= 0; i--)
		if (t[i] != vec_n[i])
			return t[i] > vec_n[i] ? 1 : -1;
	return 0;
}

static void sub_n(u32 *t)
{
	u32 bw = 0;
	int i;

	for (i = 0; i < NL; i++) {
		u32 a = t[i], d = a - vec_n[i], b1 = a < vec_n[i];

		t[i] = d - bw;
		bw = b1 | (d < bw);
	}
}

/* d = x * y / R mod n (x, y < n; d may be x or y): CIOS, the running sum
 * sliding up buf a word a turn instead of being shifted down */
static void montmul(u32 *d, const u32 *x, const u32 *y, row_fn row)
{
	u32 buf[2 * NL + 2];
	u32 *t;
	int i;

	memset(buf, 0, sizeof buf);
	for (i = 0; i < NL; i++) {
		u32 c, s, f;

		t = buf + i;
		c = row(t, x, y[i], NL);
		s = t[NL] + c;
		t[NL + 1] += s < c;
		t[NL] = s;
		f = t[0] * (u32)M0I;
		c = row(t, vec_n, f, NL);
		s = t[NL] + c;
		t[NL + 1] += s < c;
		t[NL] = s;
	}
	t = buf + NL;
	if (t[NL] || cmp_n(t) >= 0)
		sub_n(t);
	memcpy(d, t, NL * sizeof *t);
}

static int fails;

static void rsa_pub(const char *what, row_fn row)
{
	u32 xm[NL], acc[NL], one[NL];
	clock_t t0 = clock();
	long ms;
	int i, ok;

	montmul(xm, vec_x, vec_r2, row);
	memcpy(acc, xm, sizeof acc);
	for (i = 0; i < 16; i++)
		montmul(acc, acc, acc, row);
	montmul(acc, acc, xm, row);
	memset(one, 0, sizeof one);
	one[0] = 1;
	montmul(acc, acc, one, row);
	ms = ms_since(t0);
	ok = memcmp(acc, vec_y, sizeof acc) == 0;
	if (!ok)
		fails++;
	/* hundredths of a microsecond: ms * 1000 * 100 / 155648 */
	printf("  RSA-2048 public op, %-5s %s %7ld ms  (%ld.%02ld us a multiply-add)\n",
		what, ok ? "ok  " : "FAIL", ms, ms * 100 / 156 / 100, ms * 100 / 156 % 100);
}

int main(void)
{
	printf("tpmont on %s\n", TP_NAME);
	rsa_pub("c16", row_c16);
#ifdef TP_LL
	rsa_pub("ll", row_ll);
#endif
#ifdef TP_M68K
	rsa_pub("m68k", row_m68k);
	rsa_pub("m68k", row_m68k);
#endif
#ifdef TP_LMUL
	rsa_pub("asm", row_asm);
	rsa_pub("asm", row_asm);
	{
		long ms = lmul_probe(20000);

		/* 20000 * (16 * 36 + ~12) cycles: 117600 tenths of MHz in a ms */
		long t = ms ? (117600L + ms / 2) / ms : 0;

		printf("  lmul probe: %ld ms, %ld.%ld MHz if lmul takes 33 cycles\n", ms,
			t / 10, t % 10);
	}
#endif
	printf("%s\n", fails ? "FAILED" : "all ok");
	return fails ? 1 : 0;
}
