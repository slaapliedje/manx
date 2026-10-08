#!/usr/bin/env python3
"""
tpwipe.py - assemble tpwipe, the T425 program that zeroes its memory
(tls/tpwipe.c), and print it as the C array there.

The program is booted straight after a reset (a length byte, then the
code, loaded at MemStart in the chip's own RAM). It reads ranges from the
link it was booted on, 8 bytes each (start, end: little-endian, 32-byte
multiples), zeroes each, then reads it back and answers with the OR of
its words (4 bytes): 0 when it is clean. Then it waits for the next; a
reset ends it. Nothing outside the ranges is touched: its own code and
workspace stay below 0x80000120 (tpwipe.h's TPW_CHIP_FREE).

    python3 tp/tpwipe.py        # the C array, with the listing

(Tested on Hatari's T425 core and on a real ATW800/2.)
"""

# direct functions
J, LDLP, PFIX, LDNL, LDC, LDNLP, NFIX, LDL, ADC, CALL, CJ, AJW, EQC, STL, \
    STNL, OPR = range(16)
# operations
OPS = {'in': 0x07, 'gt': 0x09, 'out': 0x0B, 'or': 0x4B}
NAMES = {'j': J, 'ldlp': LDLP, 'ldnl': LDNL, 'ldc': LDC, 'ldl': LDL,
         'adc': ADC, 'cj': CJ, 'ajw': AJW, 'stl': STL, 'stnl': STNL}

UNROLL = 8          # words zeroed and read per turn: ranges are 32-byte multiples

SRC = [
    '; booted: A = old Iptr, B = old Wptr, C = the boot link\'s input channel',
    'ajw 8',            # room below the workspace: a waiting process's words
    'stl 0', 'stl 0',   # (old Iptr, old Wptr)
    'stl 1',            # W1 = the input channel
    'ldl 1', 'adc -16', 'stl 2',    # W2 = its output channel
    'next:',
    'ldlp 3', 'ldl 1', 'ldc 8', 'in',   # W3 = start, W4 = end
    'ldl 3', 'stl 5',   # W5 = p
    'zero:',
    'ldl 4', 'ldl 5', 'gt', 'cj check',     # while end > p
] + [x for k in range(UNROLL) for x in ('ldc 0', 'ldl 5', 'stnl %d' % k)] + [
    'ldl 5', 'adc %d' % (4 * UNROLL), 'stl 5', 'j zero',
    'check:',
    'ldl 3', 'stl 5', 'ldc 0', 'stl 6',     # p = start, W6 = 0
    'or_:',
    'ldl 4', 'ldl 5', 'gt', 'cj send',
    'ldl 6',
] + [x for k in range(UNROLL) for x in ('ldl 5', 'ldnl %d' % k, 'or')] + [
    'stl 6', 'ldl 5', 'adc %d' % (4 * UNROLL), 'stl 5', 'j or_',
    'send:',
    'ldlp 6', 'ldl 2', 'ldc 4', 'out',      # the OR
    'j next',
]


def code(f, v):
    """f's instruction with operand v, with its prefixes"""
    if 0 <= v < 16:
        return [f << 4 | v]
    if v >= 16:
        return code(PFIX, v >> 4) + [f << 4 | (v & 15)]
    return code(NFIX, (~v) >> 4) + [f << 4 | (v & 15)]


def assemble(src):
    lines = [s for s in src if not s.startswith(';')]
    labels, sizes = {}, {}
    for _ in range(10):         # until the jumps' prefixes settle
        addr, out, listing = 0, [], []
        for i, s in enumerate(lines):
            if s.endswith(':'):
                labels[s[:-1]] = addr
                continue
            op, *arg = s.split()
            if op in OPS:
                b = code(OPR, OPS[op])
            else:
                a = arg[0]
                if a in labels or not a.lstrip('-').isdigit():
                    # relative to the next instruction: size from last pass
                    v = labels.get(a, 0) - (addr + sizes.get(i, 1))
                else:
                    v = int(a)
                b = code(NAMES[op], v)
            sizes[i] = len(b)
            listing.append((addr, b, s))
            out += b
            addr += len(b)
        again = False
        for addr, b, s in listing:
            op, *arg = s.split()
            if arg and arg[0] in labels:
                if code(NAMES[op], labels[arg[0]] - (addr + len(b))) != b:
                    again = True
        if not again:
            return out, listing
    raise SystemExit('tpwipe: jumps did not settle')


def main():
    out, listing = assemble(SRC)
    assert len(out) < 256, len(out)
    print('/* tp/tpwipe.py: %d bytes */' % len(out))
    print('static const unsigned char wiper[] = {')
    for addr, b, s in listing:
        print('\t%s/* %3d  %s */' % (''.join('0x%02x, ' % x for x in b).ljust(24),
                                     addr, s))
    print('};')


if __name__ == '__main__':
    main()
