-- mont.s - tls/mont.c for Helios on the T425, as Helios C 2.08 compiles it
-- (c -S -T5), with row() hand-written around lmul by
-- tools/helios/mont_lmul.py. Rebuild it that way if tls/mont.c changes.
	align
	module	-1
.ModStart:
	word	#60f160f1
	word	.ModEnd-.ModStart
	blkb	31,"mont.c" byte 0
	word	modnum
	word	1
	word	.MaxData
	init
	align
..4: -- 1 refs
	word #60f360f3,.row byte "row",0 align
.row:
	ldl	1
	ldnl	1
	ldlp	-76
	gt
	cj	..5
	ldc	..4-2
	ldpi
	ldl	1
	call	._stack_error
..5: -- 1 refs
	ajw	-12
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
	ret
	align
..15: -- 1 refs
	word #60f360f3,.mont_cmp byte "mont_cmp",0 align
.mont_cmp:
	ldl	1
	ldnl	1
	ldlp	-65
	gt
	cj	..16
	ldc	..15-2
	ldpi
	ldl	1
	call	._stack_error
..16: -- 1 refs
	ajw	-1
-- Line 125 (c/mont.c)
-- Line 128 (c/mont.c)
	ldl	4
	ldnl	129
	adc	-1
	stl	0
	align
..7: -- 2 refs
	ldc	0
	ldl	0
	gt
	eqc	0
	cj	..8
-- Line 129 (c/mont.c)
	ldl	0
	ldl	3
	wsub
	ldnl	0
	ldl	0
	ldl	4
	wsub
	ldnl	0
	diff
	cj	..10
-- Line 130 (c/mont.c)
	ldl	0
	ldl	3
	wsub
	ldnl	0
	ldl	0
	ldl	4
	wsub
	ldnl	0
	mint
	xor
	rev
	mint
	xor
	rev
	gt
	cj	..13
	ldc	1
	ldc	0
	cj	..12
	align
..13: -- 1 refs
	ldc	-1
	ldc	0
	align
..12: -- 2 refs
	diff
	ajw	1
	ret
	align
..10: -- 1 refs
	ldl	0
	adc	-1
	stl	0
	j	..7
	align
..8: -- 1 refs
-- Line 131 (c/mont.c)
	ldc	0
	ajw	1
	ret
	align
..21: -- 1 refs
	word #60f360f3,.mont_sub_n byte "mont_sub_n",0 align
.mont_sub_n:
	ldl	1
	ldnl	1
	ldlp	-69
	gt
	cj	..22
	ldc	..21-2
	ldpi
	ldl	1
	call	._stack_error
..22: -- 1 refs
	ajw	-5
-- Line 135 (c/mont.c)
-- Line 135 (c/mont.c)
	ldc	0
	stl	4
-- Line 139 (c/mont.c)
	ldc	0
	stl	3
	align
..18: -- 2 refs
	ldl	8
	ldnl	129
	ldl	3
	gt
	cj	..19
-- Line 139 (c/mont.c)
-- Line 139 (c/mont.c)
	ldl	3
	ldl	7
	wsub
	ldnl	0
	stl	1
-- Line 139 (c/mont.c)
	ldl	1
	ldl	3
	ldl	8
	wsub
	ldnl	0
	sub
	stl	0
-- Line 139 (c/mont.c)
	ldl	3
	ldl	8
	wsub
	ldnl	0
	ldl	1
	mint
	xor
	rev
	mint
	xor
	rev
	gt
	stl	2
-- Line 142 (c/mont.c)
	ldl	0
	ldl	4
	sub
	ldl	3
	ldl	7
	wsub
	stnl	0
-- Line 143 (c/mont.c)
	ldl	4
	ldl	0
	mint
	xor
	rev
	mint
	xor
	rev
	gt
	ldl	2
	or
	stl	4
	ldl	3
	adc	1
	stl	3
	j	..18
	align
..19: -- 1 refs
	ajw	5
	ret
	align
..31: -- 1 refs
	word #60f360f3,.mont_mul byte "mont_mul",0 align
.mont_mul:
	ldl	1
	ldnl	1
	ldnlp	258
	ldlp	-74
	gt
	cj	..32
	ldc	..31-2
	ldpi
	ldl	1
	call	._stack_error
..32: -- 1 refs
	ajw	-10
	ldl	11
	ldnl	0
	stl	8
	ldl	11
	ldnl	1
	ldnlp	258
	stl	9
-- Line 150 (c/mont.c)
	ldl	9
	ldnlp	-258
	stl	7
-- Line 150 (c/mont.c)
	ldl	7
	stl	4
-- Line 150 (c/mont.c)
	ldl	15
	ldnl	129
	stl	5
-- Line 155 (c/mont.c)
	ldl	5
	ldc	2
	prod
	adc	2
	ldc	4
	prod
	stl	0
	ldc	0
	ldl	7
	ldlp	8
	call	.memset
-- Line 156 (c/mont.c)
	ldc	0
	stl	6
	align
..24: -- 2 refs
	ldl	5
	ldl	6
	gt
	cj	..25
-- Line 156 (c/mont.c)
-- Line 160 (c/mont.c)
	ldl	7
	ldl	6
	ldc	4
	prod
	add
	stl	4
-- Line 161 (c/mont.c)
	ldl	5
	stl	1
	ldl	6
	ldl	14
	wsub
	ldnl	0
	stl	0
	ldl	13
	ldl	4
	ldlp	8
	call	.row
	stl	3
-- Line 162 (c/mont.c)
	ldl	5
	ldl	4
	wsub
	ldnl	0
	ldl	3
	add
	stl	2
-- Line 163 (c/mont.c)
	ldl	4
	ldl	5
	adc	1
	ldc	4
	prod
	add
	stl	1
	ldl	3
	ldl	2
	mint
	xor
	rev
	mint
	xor
	rev
	gt
	ldl	1
	ldnl	0
	add
	ldl	1
	stnl	0
-- Line 164 (c/mont.c)
	ldl	2
	ldl	5
	ldl	4
	wsub
	stnl	0
-- Line 165 (c/mont.c)
	ldl	5
	stl	1
	ldl	4
	ldnl	0
	ldl	15
	ldnl	128
	prod
	stl	0
	ldl	15
	ldl	4
	ldlp	8
	call	.row
	stl	3
-- Line 166 (c/mont.c)
	ldl	5
	ldl	4
	wsub
	ldnl	0
	ldl	3
	add
	stl	2
-- Line 167 (c/mont.c)
	ldl	4
	ldl	5
	adc	1
	ldc	4
	prod
	add
	stl	1
	ldl	3
	ldl	2
	mint
	xor
	rev
	mint
	xor
	rev
	gt
	ldl	1
	ldnl	0
	add
	ldl	1
	stnl	0
-- Line 168 (c/mont.c)
	ldl	2
	ldl	5
	ldl	4
	wsub
	stnl	0
	ldl	6
	adc	1
	stl	6
	j	..24
	align
..25: -- 1 refs
-- Line 170 (c/mont.c)
	ldl	7
	ldl	5
	ldc	4
	prod
	add
	stl	4
-- Line 171 (c/mont.c)
	ldl	5
	ldl	4
	wsub
	ldnl	0
	eqc	0
	cj	..28
	ldl	15
	ldl	4
	ldlp	8
	call	.mont_cmp
	ldc	0
	rev
	gt
	eqc	0
	cj	..27
..28: -- 3 refs
-- Line 172 (c/mont.c)
	ldl	15
	ldl	4
	ldlp	8
	call	.mont_sub_n
..27: -- 2 refs
-- Line 173 (c/mont.c)
	ldl	5
	ldc	4
	prod
	stl	0
	ldl	4
	ldl	12
	ldlp	8
	call	.memcpy
	ajw	10
	ret
	align
..41: -- 1 refs
	word #60f360f3,.mont_dbl byte "mont_dbl",0 align
.mont_dbl:
	ldl	1
	ldnl	1
	ldlp	-67
	gt
	cj	..42
	ldc	..41-2
	ldpi
	ldl	1
	call	._stack_error
..42: -- 1 refs
	ajw	-3
-- Line 177 (c/mont.c)
-- Line 177 (c/mont.c)
	ldc	0
	stl	2
-- Line 181 (c/mont.c)
	ldc	0
	stl	1
	align
..34: -- 2 refs
	ldl	6
	ldnl	129
	ldl	1
	gt
	cj	..35
-- Line 181 (c/mont.c)
-- Line 181 (c/mont.c)
	ldl	1
	ldl	5
	wsub
	ldnl	0
	stl	0
-- Line 184 (c/mont.c)
	ldl	0
	ldc	1
	shl
	ldl	2
	or
	ldl	1
	ldl	5
	wsub
	stnl	0
-- Line 185 (c/mont.c)
	ldl	0
	ldc	31
	shr
	stl	2
	ldl	1
	adc	1
	stl	1
	j	..34
	align
..35: -- 1 refs
-- Line 187 (c/mont.c)
	ldl	2
	eqc	0
	cj	..38
	ldl	6
	ldl	5
	ldl	4
	call	.mont_cmp
	ldc	0
	rev
	gt
	eqc	0
	cj	..37
..38: -- 3 refs
-- Line 188 (c/mont.c)
	ldl	6
	ldl	5
	ldl	4
	call	.mont_sub_n
..37: -- 2 refs
	ajw	3
	ret
	align
..47: -- 1 refs
	word #60f360f3,.mont_decode byte "mont_decode",0 align
.mont_decode:
	ldl	1
	ldnl	1
	ldlp	-67
	gt
	cj	..48
	ldc	..47-2
	ldpi
	ldl	1
	call	._stack_error
..48: -- 1 refs
	ajw	-3
-- Line 192 (c/mont.c)
-- Line 195 (c/mont.c)
	ldl	6
	ldc	4
	prod
	stl	0
	ldc	0
	ldl	5
	ldl	4
	call	.memset
-- Line 196 (c/mont.c)
	ldc	0
	stl	2
	align
..44: -- 2 refs
	ldl	8
	ldl	2
	mint
	xor
	rev
	mint
	xor
	rev
	gt
	cj	..45
-- Line 196 (c/mont.c)
-- Line 196 (c/mont.c)
	ldl	8
	adc	-1
	ldl	2
	sub
	stl	1
-- Line 199 (c/mont.c)
	ldl	5
	ldl	1
	ldc	2
	shr
	ldc	4
	prod
	add
	stl	0
	ldl	2
	ldl	7
	bsub
	lb
	ldl	1
	ldc	3
	and
	ldc	3
	shl
	shl
	ldl	0
	ldnl	0
	or
	ldl	0
	stnl	0
	ldl	2
	adc	1
	stl	2
	j	..44
	align
..45: -- 1 refs
	ajw	3
	ret
	align
..53: -- 1 refs
	word #60f360f3,.mont_encode byte "mont_encode",0 align
.mont_encode:
	ldl	1
	ldnl	1
	ldlp	-66
	gt
	cj	..54
	ldc	..53-2
	ldpi
	ldl	1
	call	._stack_error
..54: -- 1 refs
	ajw	-2
-- Line 204 (c/mont.c)
-- Line 207 (c/mont.c)
	ldc	0
	stl	1
	align
..50: -- 2 refs
	ldl	5
	ldl	1
	mint
	xor
	rev
	mint
	xor
	rev
	gt
	cj	..51
-- Line 207 (c/mont.c)
-- Line 207 (c/mont.c)
	ldl	5
	adc	-1
	ldl	1
	sub
	stl	0
-- Line 210 (c/mont.c)
	ldl	0
	ldc	2
	shr
	ldl	6
	wsub
	ldnl	0
	ldl	0
	ldc	3
	and
	ldc	3
	shl
	shr
	ldc	255
	and
	ldl	1
	ldl	4
	bsub
	sb
	ldl	1
	adc	1
	stl	1
	j	..50
	align
..51: -- 1 refs
	ajw	2
	ret
	align
..59: -- 1 refs
	word #60f360f3,.mont_setup byte "mont_setup",0 align
.mont_setup:
	ldl	1
	ldnl	1
	ldlp	-66
	gt
	cj	..60
	ldc	..59-2
	ldpi
	ldl	1
	call	._stack_error
..60: -- 1 refs
	ajw	-2
-- Line 215 (c/mont.c)
-- Line 215 (c/mont.c)
	ldl	4
	ldnl	0
	stl	1
-- Line 220 (c/mont.c)
	ldc	0
	stl	0
	align
..56: -- 2 refs
	ldc	4
	ldl	0
	gt
	cj	..57
-- Line 221 (c/mont.c)
	ldl	1
	ldl	4
	ldnl	0
	ldl	1
	prod
	ldc	2
	rev
	sub
	prod
	stl	1
	ldl	0
	adc	1
	stl	0
	j	..56
	align
..57: -- 1 refs
-- Line 222 (c/mont.c)
	ldl	1
	not
	adc	1
	ldl	4
	stnl	128
	ajw	2
	ret
	align
..68: -- 1 refs
	word #60f360f3,.mont_r2 byte "mont_r2",0 align
.mont_r2:
	ldl	1
	ldnl	1
	ldlp	-70
	gt
	cj	..69
	ldc	..68-2
	ldpi
	ldl	1
	call	._stack_error
..69: -- 1 refs
	ajw	-6
-- Line 228 (c/mont.c)
-- Line 228 (c/mont.c)
	ldl	9
	ldnl	129
	adc	-1
	ldl	9
	wsub
	ldnl	0
	stl	5
-- Line 228 (c/mont.c)
	ldl	9
	ldnl	129
	adc	-1
	ldc	32
	prod
	stl	4
-- Line 232 (c/mont.c)
	align
..62: -- 2 refs
	ldl	5
	cj	..63
-- Line 232 (c/mont.c)
-- Line 233 (c/mont.c)
	ldl	4
	adc	1
	stl	4
-- Line 234 (c/mont.c)
	ldl	5
	ldc	1
	shr
	stl	5
	j	..62
	align
..63: -- 1 refs
-- Line 236 (c/mont.c)
	ldl	9
	ldnl	129
	ldc	4
	prod
	stl	0
	ldc	0
	ldl	8
	ldl	7
	call	.memset
-- Line 237 (c/mont.c)
	ldc	1
	ldl	4
	adc	-1
	ldc	31
	and
	shl
	stl	2
	ldl	4
	adc	-1
	xdble
	ldc	5
	lshr
	ldl	8
	wsub
	ldl	2
	rev
	stnl	0
-- Line 238 (c/mont.c)
	ldl	9
	ldnl	129
	ldc	32
	prod
	ldl	4
	sub
	adc	1
	ldl	9
	ldnl	129
	ldc	8
	prod
	add
	stl	3
	align
..65: -- 2 refs
	ldl	3
	ldc	0
	gt
	cj	..66
-- Line 239 (c/mont.c)
	ldl	9
	ldl	8
	ldl	7
	call	.mont_dbl
	ldl	3
	adc	-1
	stl	3
	j	..65
	align
..66: -- 1 refs
-- Line 240 (c/mont.c)
	ldl	9
	stl	1
	ldl	8
	stl	0
	ldl	8
	ldl	8
	ldl	7
	call	.mont_mul
-- Line 241 (c/mont.c)
	ldl	9
	stl	1
	ldl	8
	stl	0
	ldl	8
	ldl	8
	ldl	7
	call	.mont_mul
	ajw	6
	ret
-- Stubs
	align
._stack_error:
	ldl	1
	ldnl	0
	ldnl	@__stack_error
	ldnl	__stack_error
	gcall
	align
.memset:
	ldl	1
	ldnl	0
	ldnl	@_memset
	ldnl	_memset
	gcall
	align
.memcpy:
	ldl	1
	ldnl	0
	ldnl	@_memcpy
	ldnl	_memcpy
	gcall
-- Data Initialization
	data	..dataseg 0
	global	_mont_cmp
	data	_mont_cmp 1
	global	_mont_sub_n
	data	_mont_sub_n 1
	global	_mont_mul
	data	_mont_mul 1
	global	_mont_dbl
	data	_mont_dbl 1
	global	_mont_decode
	data	_mont_decode 1
	global	_mont_encode
	data	_mont_encode 1
	global	_mont_setup
	data	_mont_setup 1
	global	_mont_r2
	data	_mont_r2 1
	align
	init
	ajw	-2
	ldl	3
	ldnl	0
	ldnl	modnum
	stl	1
	ldl	1
	ldnlp	..dataseg
	stl	0
	ldl	4
	cj	..71
	j	..72
..71: -- 1 refs
	ldc	.mont_r2-2
	ldpi
	ldl	0
	stnl	7
	ldc	.mont_setup-2
	ldpi
	ldl	0
	stnl	6
	ldc	.mont_encode-2
	ldpi
	ldl	0
	stnl	5
	ldc	.mont_decode-2
	ldpi
	ldl	0
	stnl	4
	ldc	.mont_dbl-2
	ldpi
	ldl	0
	stnl	3
	ldc	.mont_mul-2
	ldpi
	ldl	0
	stnl	2
	ldc	.mont_sub_n-2
	ldpi
	ldl	0
	stnl	1
	ldc	.mont_cmp-2
	ldpi
	ldl	0
	stnl	0
..72: -- 1 refs
	ajw	2
	ret
	data	.MaxData 0
	align
.ModEnd:
