#!/usr/bin/env python3
"""mkrelease.py - a release's files for Manx's updater (net/update.h).

    tools/mkrelease.py VERSION OUTDIR PLATFORM=BUILDDIR... [options]

For each PLATFORM=BUILDDIR (irix-mips=build/irix, sysv4-m68k=build/sysv4,
linux-x86_64=build/host), each program found there (manx, xmanx, manxmail,
manxtrust, t425wipe) is gzipped into OUTDIR as
manx-VERSION-PLATFORM-PROGRAM.gz, and listed in OUTDIR/manx-update.txt,
which is then signed into OUTDIR/manx-update.sig with the release key.

    --base URL     where the files will be (default: the GitHub release
                   vVERSION's downloads)
    --notes FILE   what's new, a line each, into the manifest's notes
    --key PEM      the release key (default ~/.config/manx-release/
                   release-key.pem); the signature is checked with its
                   public half before anything is written out
    --date DATE    (default today, UTC)
"""
import argparse
import datetime
import gzip
import hashlib
import os
import subprocess
import sys
import tempfile

PROGRAMS = ("manx", "xmanx", "manxmail", "manxtrust", "t425wipe")
REPO = "https://github.com/slaapliedje/manx"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version")
    ap.add_argument("outdir")
    ap.add_argument("builds", nargs="+", metavar="PLATFORM=BUILDDIR")
    ap.add_argument("--base")
    ap.add_argument("--notes")
    ap.add_argument("--key", default=os.path.expanduser(
        "~/.config/manx-release/release-key.pem"))
    ap.add_argument("--date", default=datetime.datetime.now(
        datetime.timezone.utc).strftime("%Y-%m-%d"))
    a = ap.parse_args()

    base = a.base or "%s/releases/download/v%s/" % (REPO, a.version)
    if not base.endswith("/"):
        base += "/"
    os.makedirs(a.outdir, exist_ok=True)
    lines = ["manx-update 1", "version " + a.version, "date " + a.date]
    for b in a.builds:
        plat, _, d = b.partition("=")
        if not d or not os.path.isdir(d):
            sys.exit("mkrelease: %s: not PLATFORM=BUILDDIR" % b)
        found = 0
        for prog in PROGRAMS:
            src = os.path.join(d, prog)
            if not os.path.isfile(src):
                continue
            data = open(src, "rb").read()
            name = "manx-%s-%s-%s.gz" % (a.version, plat, prog)
            # (mtime 0: the same build gives the same file)
            with open(os.path.join(a.outdir, name), "wb") as f:
                with gzip.GzipFile(filename=prog, mode="wb", fileobj=f,
                                   compresslevel=9, mtime=0) as g:
                    g.write(data)
            lines.append("file %s %s %d %s %s%s" % (
                plat, prog, len(data), hashlib.sha256(data).hexdigest(),
                base, name))
            found += 1
        if not found:
            sys.exit("mkrelease: no programs in %s" % d)
    if a.notes:
        for n in open(a.notes):
            n = n.rstrip()
            if n:
                lines.append("note " + n)
    text = ("\n".join(lines) + "\n").encode()

    man = os.path.join(a.outdir, "manx-update.txt")
    sig = os.path.join(a.outdir, "manx-update.sig")
    with tempfile.TemporaryDirectory() as t:
        tm, ts, tp = (os.path.join(t, x) for x in ("m", "s", "p"))
        open(tm, "wb").write(text)
        subprocess.run(["openssl", "dgst", "-sha256", "-sign", a.key,
                        "-out", ts, tm], check=True)
        subprocess.run(["openssl", "ec", "-in", a.key, "-pubout", "-out",
                        tp], check=True, stderr=subprocess.DEVNULL)
        subprocess.run(["openssl", "dgst", "-sha256", "-verify", tp,
                        "-signature", ts, tm], check=True,
                       stdout=subprocess.DEVNULL)
        open(man, "wb").write(text)
        open(sig, "wb").write(open(ts, "rb").read())
    sys.stdout.write(text.decode())


if __name__ == "__main__":
    main()
