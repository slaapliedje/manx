#!/usr/bin/env python3
# mkvec.py - writes tpmontvec.h: a 2048-bit odd modulus n, x < n,
# R^2 mod n (R = 2^2048), -1/n mod 2^32 and x^65537 mod n, as 32-bit limbs
# (least significant first), for tpmont.c
import random
random.seed(20261003)
NL = 64
def limbs(v):
    return [(v >> (32 * i)) & 0xffffffff for i in range(NL)]
def arr(name, v):
    ls = limbs(v)
    s = "static const u32 %s[NL] = {\n" % name
    for i in range(0, NL, 4):
        s += "\t" + ", ".join("0x%08xU" % w for w in ls[i:i + 4]) + ",\n"
    return s + "};\n"
n = random.getrandbits(32 * NL) | (1 << (32 * NL - 1)) | 1
x = random.getrandbits(32 * NL) % n
r2 = (1 << (64 * NL)) % n
m0i = (-pow(n, -1, 1 << 32)) % (1 << 32)
out = "/* tpmontvec.h - made by mkvec.py */\n#define M0I 0x%08xU\n" % m0i
for name, v in (("vec_n", n), ("vec_x", x), ("vec_r2", r2), ("vec_y", pow(x, 65537, n))):
    out += arr(name, v)
open("tpmontvec.h", "w").write(out)
