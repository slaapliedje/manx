/*
 * mul16.h - a multiply of two values that fit in 16 bits, for the
 * decoders' inner loops. On a 68030 muls.w (16 x 16 -> 32) takes 28
 * cycles and muls.l 44 or so at best, 78 at worst, and gcc only picks
 * the short form when it can see both operands are shorts, which it
 * can't through a long's arithmetic: so on the 68020-40 it is written
 * out. Elsewhere it is the ordinary multiply, of the same low 16 bits,
 * so every build computes the same thing; what a value past 16 bits (a
 * corrupt file's) comes out as is wrong but defined, the same
 * everywhere. (No immediate operands: the SVR4 toolchain's assembler
 * wrapper takes '#' for a comment, see tls/mont.c.)
 */
#ifndef MANX_MUL16_H
#define MANX_MUL16_H

#if defined(__GNUC__) && (defined(__mc68020__) || defined(__mc68030__) \
	|| defined(__mc68040__))
static inline long mul16(long a, long b)
{
	long r = a;

	__asm__("muls.w %[b],%[r]" : [r] "+d" (r) : [b] "d" (b) : "cc");
	return r;
}
static inline unsigned long mulu16(unsigned long a, unsigned long b)
{
	unsigned long r = a;

	__asm__("mulu.w %[b],%[r]" : [r] "+d" (r) : [b] "d" (b) : "cc");
	return r;
}
#else
#define mul16(a, b)	((long)(short)(a) * (long)(short)(b))
#define mulu16(a, b)	((unsigned long)(unsigned short)(a) * (unsigned long)(unsigned short)(b))
#endif

#endif /* MANX_MUL16_H */
