/*
 * cpu.h - which of crypt68k's 68k loops a build gets. The 68020, 68030
 * and 68040 have mulu.l's 32x32 -> 64; the 68060 traps it (and gcc's
 * -m68020-60 names the 68060 too): C there, and everywhere else.
 */
#ifndef C68K_CPU_H
#define C68K_CPU_H

#if defined(__GNUC__) && !defined(__mc68060__) && (defined(__mc68020__) \
	|| defined(__mc68030__) || defined(__mc68040__))
#define C68K_MULU64	1
#else
#define C68K_MULU64	0
#endif

#endif /* C68K_CPU_H */
