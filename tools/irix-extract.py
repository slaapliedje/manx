#!/usr/bin/env python3
"""irix-extract.py - take one file out of an IRIX inst distribution.

    tools/irix-extract.py PRODUCT.idb PRODUCT.sw PATH OUT

PATH as the idb names it (usr/sbin/gendist). In the .sw image each file
is its name (a 2-byte length, then the name) and its data, at the idb's
off(), compress(1)ed when the idb gives a cmpsize(); gzip reads that.
For gendist on a machine without inst_dev (whose overlay, on the 6.5.22
overlays' disc 2, won't install without the 6.5 base):
    tools/irix-extract.py inst_dev_6522m.idb inst_dev_6522m.sw \\
        usr/sbin/gendist gendist
"""
import re
import subprocess
import sys

idb, sw, path, out = sys.argv[1:5]
for line in open(idb, encoding="latin-1"):
    f = line.split()
    if len(f) < 6 or f[0] != "f" or f[4] != path:
        continue
    off = int(re.search(r"\boff\((\d+)\)", line).group(1))
    size = int(re.search(r"\bsize\((\d+)\)", line).group(1))
    m = re.search(r"\bcmpsize\((\d+)\)", line)
    cmp = int(m.group(1)) if m else 0
    with open(sw, "rb") as s:
        s.seek(off)
        n = int.from_bytes(s.read(2), "big")
        if s.read(n).decode("latin-1") != path:
            sys.exit("irix-extract: %s isn't at off(%d)" % (path, off))
        data = s.read(cmp or size)
    if cmp:
        data = subprocess.run(["gzip", "-dc"], input=data, check=True,
                              stdout=subprocess.PIPE).stdout
    if len(data) != size:
        sys.exit("irix-extract: %s: %d bytes, not %d" % (path, len(data), size))
    with open(out, "wb") as o:
        o.write(data)
    sys.exit(0)
sys.exit("irix-extract: no %s in %s" % (path, idb))
