#!/usr/bin/env python3
# gen_fe_m68k.py - writes src/fe_m68k.c: X25519's 512-bit multiply and
# square (8 x 32-bit limbs) for the 68020/030/040, as gcc inline assembly.
# Product scanning, unrolled: a column's products added into a 96-bit
# accumulator of three registers whose roles rotate, so nothing is moved;
# a square's cross products doubled first, the doubling's carry into the
# top word. No branches, the same instructions whatever the values, and
# no '#' immediates (one SVR4 toolchain's wrapper takes '#' for a comment).

def body(square):
    lines = ["clr.l %%d2", "clr.l %%d3", "clr.l %%d4", "clr.l %%d5"]
    acc = ["%%d2", "%%d3", "%%d4"]		# low, middle, high
    for k in range(15):
        lo, mid, hi = acc
        for i in range(8):
            j = k - i
            if j < 0 or j > 7 or (square and i > j):
                continue
            lines.append("move.l %d(%%[a]),%%%%d0" % (4 * i))
            lines.append("mulu.l %d(%%[b]),%%%%d1:%%%%d0" % (4 * j))
            if square and i != j:
                lines += ["add.l %%d0,%%d0", "addx.l %%d1,%%d1", "addx.l %%d5," + hi]
            lines += ["add.l %%d0," + lo, "addx.l %%d1," + mid, "addx.l %%d5," + hi]
        lines.append("move.l %s,%d(%%[t])" % (lo, 4 * k))
        lines.append("clr.l " + lo)
        acc = [mid, hi, lo]
    lines.append("move.l %s,60(%%[t])" % acc[0])
    return "".join('\t\t"%s\\n\\t"\n' % l for l in lines)

out = '''/*
 * fe_m68k.c - X25519's 512-bit multiply and square for the 68020, 68030
 * and 68040 (x25519.h), made by tools/gen_fe_m68k.py: product scanning,
 * unrolled, 64 and 36 mulu.l. Constant time: no branches, and the same
 * instructions whatever the values. Part of crypt68k, MIT licence
 * (LICENSE).
 */
#include "x25519.h"

#if C68K_FE_ASM
void c68k_fe_mul512(uint32_t *t, const uint32_t *a, const uint32_t *b)
{
	__asm__ volatile(
%s		: : [t] "a" (t), [a] "a" (a), [b] "a" (b)
		: "d0", "d1", "d2", "d3", "d4", "d5", "cc", "memory");
}

void c68k_fe_sqr512(uint32_t *t, const uint32_t *a)
{
	__asm__ volatile(
%s		: : [t] "a" (t), [a] "a" (a), [b] "a" (a)
		: "d0", "d1", "d2", "d3", "d4", "d5", "cc", "memory");
}
#endif
''' % (body(False), body(True))
open("src/fe_m68k.c", "w").write(out)
print("src/fe_m68k.c: %d lines" % out.count("\n"))
