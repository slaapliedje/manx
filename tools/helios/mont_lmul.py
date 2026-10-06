#!/usr/bin/env python3
"""
mont_lmul.py IN.s OUT.s - os/helios/mont.s from Helios C's assembly for
tls/mont.c (on Helios, in /helios/local/src/manx after a bundle build:
c -S -T5 <build.csh's flags> c/mont.c, which writes c/mont.s): row(), the
Montgomery multiplication's inner loop, becomes a hand-written one around
the transputer's lmul (32 x 32 + 32 -> 64 bits) and lsum, in place of
Helios C's four 16 x 16-bit prods a limb. Certificate checks on the T425
take about a third of the time. Everything else is Helios C's own.
"""
import sys

ROW = '''	ajw	-12
-- (hand-written, tools/helios/mont_lmul.py: r[0..k-1] += a[0..k-1] * b,
--  the carry out, by lmul and lsum. Args r a b k at 14 15 16 17; locals
--  0 carry, 1 r's pointer, 2 a's pointer, 3 the count, 4 low, 5 high)
	ldc	0
	stl	0
	ldl	14
	stl	1
	ldl	15
	stl	2
	ldl	17
	stl	3
	ldl	3
	ldc	0
	gt
	cj	..9002
	align
..9001:
	ldl	0
	ldl	2
	ldnl	0
	ldl	16
	lmul
	stl	4
	stl	5
	ldc	0
	ldl	4
	ldl	1
	ldnl	0
	lsum
	ldl	1
	stnl	0
	ldl	5
	sum
	stl	0
	ldl	2
	ldnlp	1
	stl	2
	ldl	1
	ldnlp	1
	stl	1
	ldl	3
	adc	-1
	stl	3
	ldl	3
	eqc	0
	cj	..9001
..9002:
	ldl	0
	ajw	12
	ret'''

HEAD = '''-- mont.s - tls/mont.c for Helios on the T425, as Helios C 2.08 compiles it
-- (c -S -T5), with row() hand-written around lmul by
-- tools/helios/mont_lmul.py. Rebuild it that way if tls/mont.c changes.
'''


def main():
	if len(sys.argv) != 3:
		sys.exit('usage: mont_lmul.py IN.s OUT.s')
	lines = open(sys.argv[1], newline='').read().replace('\r\n', '\n').split('\n')
	start = lines.index('.row:')
	# row()'s args must be where ROW takes them: its frame is 12 words
	a = next(i for i in range(start, len(lines)) if lines[i].split() == ['ajw', '-12'])
	if a - start > 12:
		sys.exit('mont_lmul: row() does not start as expected')
	r = next(i for i in range(a, len(lines)) if lines[i].split() == ['ret'])
	if lines[r - 1].split() != ['ajw', '12']:
		sys.exit('mont_lmul: row() does not end as expected')
	out = lines[:a] + ROW.split('\n') + lines[r + 1:]
	with open(sys.argv[2], 'w') as f:
		f.write(HEAD + '\n'.join(out).rstrip('\n') + '\n')


main()
