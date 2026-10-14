#!/usr/bin/env python3
"""
tpreloc.py - assemble tpreloc, a boot stage put ahead of a T425 bootable
(icollect -t) so that it runs from LOAD instead of MemStart, and print it
as the C array tls/tpoff.c sends.

Why: on the ATW800/2 the T425's memory from 0x80000000 is the card's
memory past 2 MB - a desktop at 32 bits per pixel shows it. From LOAD
(0x80200070) up only the T425 sees it.

How: the INMOS bootstrap finds its place with ldpi, never ldmemstartval,
so it runs wherever it is put, and loads what follows next to itself.
tpreloc is booted first (in the chip's own RAM): it reads the bootable's
first stage (a length byte, then that many bytes) from the boot link to
LOAD, sets the workspace and the registers as a boot from a link would
(Wptr the first word after the code; A old Iptr, B old Wptr, C the link's
input channel) and jumps there. The bootable's first stage then reads the
rest from the link itself, as usual.

    python3 tp/tpreloc.py        # the C array, with the listing

(Tested on Hatari's T425 core with the ATW800/2's own hello.btl.)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tpwipe import code, LDC, J, NAMES, OPS  # noqa: E402

MEMSTART = 0x80000070
LOAD = 0x80200070

OPS = dict(OPS, add=0x05, gajw=0x3C, **{'and': 0x46})

SRC = [
    '; booted: A = old Iptr, B = old Wptr, C = the boot link\'s input channel',
    'ajw 8',
    'stl 1', 'stl 2', 'stl 3',          # W1 old Iptr, W2 old Wptr, W3 the link
    'ldc 0', 'stl 4',
    'ldlp 4', 'ldl 3', 'ldc 1', 'in',   # W4 = the first stage's length
    'ldc %d' % (LOAD - (1 << 32)), 'ldl 3', 'ldl 4', 'in',     # it, at LOAD
    'ldc %d' % (LOAD - (1 << 32)), 'ldl 4', 'add', 'adc 3', 'ldc -4', 'and',
    'gajw',                             # the workspace a boot would give it
    'stl 0',                            # (ours, to reach our words)
    'ldl 0', 'ldnl 3',                  # C: the link
    'ldl 0', 'ldnl 2',                  # B: old Wptr
    'ldl 0', 'ldnl 1',                  # A: old Iptr
    'jabs %d' % LOAD,                   # (no other process: no timeslice)
]


def assemble(src, base):
    lines = [s for s in src if not s.startswith(';')]
    sizes = {}
    for _ in range(10):
        addr, out, listing, again = 0, [], [], False
        for i, s in enumerate(lines):
            op, *arg = s.split()
            if op in OPS:
                b = code(15, OPS[op])
            elif op == 'jabs':
                target = int(arg[0])
                v = target - (base + addr + sizes.get(i, 1))
                b = code(J, v)
            else:
                b = code(NAMES[op], int(arg[0]))
            if sizes.get(i) != len(b):
                again = True
            sizes[i] = len(b)
            listing.append((addr, b, s))
            out += b
            addr += len(b)
        if not again:
            return out, listing
    raise SystemExit('tpreloc: did not settle')


def main():
    out, listing = assemble(SRC, MEMSTART)
    assert len(out) < 256, len(out)
    print('/* tp/tpreloc.py: %d bytes, the bootable run from %#x */' % (len(out), LOAD))
    print('static const unsigned char reloc[] = {')
    for addr, b, s in listing:
        print('\t%s/* %3d  %s */' % (''.join('0x%02x, ' % x for x in b).ljust(40),
                                     addr, s))
    print('};')


if __name__ == '__main__':
    main()
