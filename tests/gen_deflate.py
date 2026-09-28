#!/usr/bin/env python3
"""gen_deflate.py OUTDIR FILE... - FILE.gz and FILE.zz (zlib) of each, at
several compression levels, and a list of test_inflate arguments."""
import gzip, os, sys, zlib
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
args = []
for i, f in enumerate(sys.argv[2:]):
    data = open(f, "rb").read()
    level = [1, 6, 9][i % 3]
    base = os.path.join(out, "%02d" % i)
    open(base + ".raw", "wb").write(data)
    open(base + ".gz", "wb").write(gzip.compress(data, level, mtime=0))
    open(base + ".zz", "wb").write(zlib.compress(data, level))
    args += [base + ".raw", base + ".gz", base + ".zz"]
# stored blocks and an empty stream
for name, data, level in (("stored", os.urandom(70000), 0), ("empty", b"", 6)):
    base = os.path.join(out, name)
    open(base + ".raw", "wb").write(data)
    open(base + ".gz", "wb").write(gzip.compress(data, level, mtime=0))
    open(base + ".zz", "wb").write(zlib.compress(data, level))
    args += [base + ".raw", base + ".gz", base + ".zz"]
print(" ".join(args))
