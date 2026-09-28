#!/usr/bin/env python3
"""corpus_check.py UPARSE [DIR] - run the parser over the corpus
(tests/fetch_corpus.sh) and check it:

  - the title and the number of <a href> links agree with Python's
    html.parser (outside script/style/svg/math/template, as ours drops
    those; our cap may truncate the biggest pages: those are reported)
  - the tree is the same whether the bytes come all at once, 1 at a time,
    or 7 at a time (streaming must not change the result)
  - no crash, and within the memory cap

Prints one line per page and a summary; exit status 1 on any failure."""
import glob, html.parser, os, re, subprocess, sys

UPARSE = sys.argv[1]
DIR = sys.argv[2] if len(sys.argv) > 2 else "build/corpus"
DROP = {"script", "style", "svg", "math", "template", "iframe", "noembed",
        "noframes", "datalist"}


class Ref(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.skip = []
        self.title = None
        self.in_title = False
        self.tbuf = []
        self.links = 0

    def handle_starttag(self, tag, attrs):
        if self.skip:
            if tag == self.skip[-1]:
                self.skip.append(tag)
            return
        if tag in DROP:
            self.skip.append(tag)
            return
        if tag == "title" and self.title is None:
            self.in_title = True
        if tag == "a" and any(k == "href" for k, _ in attrs):
            self.links += 1

    def handle_startendtag(self, tag, attrs):
        if not self.skip and tag == "a" and any(k == "href" for k, _ in attrs):
            self.links += 1

    def handle_endtag(self, tag):
        if self.skip:
            if tag == self.skip[-1]:
                self.skip.pop()
            return
        if tag == "title" and self.in_title:
            self.in_title = False
            self.title = " ".join("".join(self.tbuf).split())

    def handle_data(self, d):
        if self.in_title and not self.skip:
            self.tbuf.append(d)


def charset(hdr):
    cs = ""
    try:
        for line in open(hdr, encoding="latin-1"):
            m = re.match(r"(?i)content-type:.*charset=\"?([\w-]+)", line)
            if m:
                cs = m.group(1)
    except OSError:
        pass
    return cs


def run(args):
    return subprocess.run([UPARSE] + args, capture_output=True, timeout=60)


fails = 0
rows = []
for f in sorted(glob.glob(os.path.join(DIR, "*.html"))):
    base = f[:-5]
    url = open(base + ".url").read().strip() if os.path.exists(base + ".url") else f
    cs = charset(base + ".hdr")
    opt = ["-C", cs] if cs else []
    problems = []

    r = run(opt + ["-s", f])
    if r.returncode != 0:
        problems.append("crashed (%d): %s" % (r.returncode, r.stderr.decode()[:200]))
        stats = ""
    else:
        stats = r.stdout.decode("utf-8", "replace")
    m = re.search(r'title "(.*)"  links (\d+)', stats)
    ours_title, ours_links = (m.group(1), int(m.group(2))) if m else (None, -1)
    truncated = "TRUNCATED" in stats

    # the reference, decoding the way we do
    raw = open(f, "rb").read()
    enc = "utf-8" if (cs or "").lower() in ("utf-8", "utf8", "") else "cp1252"
    if not cs:
        m2 = re.search(rb'(?i)<meta[^>]*charset=["\']?([\w-]+)', raw[:1024])
        if m2 and m2.group(1).lower() not in (b"utf-8", b"utf8"):
            enc = "cp1252"
    text = raw.decode(enc, "replace")
    if url.endswith(".txt"):
        ref = None
    else:
        ref = Ref()
        ref.feed(text)
        ref.close()

    if ref is not None and not truncated:
        want_title = (ref.title or "")[:255].rstrip()
        if ours_title is not None and ours_title != want_title:
            problems.append("title %r != python %r" % (ours_title[:60], want_title[:60]))
        if ours_links != ref.links:
            problems.append("links %d != python %d" % (ours_links, ref.links))

    # streaming invariance
    whole = run(opt + ["-d", f]).stdout
    for step in ("1", "7"):
        if run(opt + ["-d", "-c", step, f]).stdout != whole:
            problems.append("tree differs when fed %s byte(s) at a time" % step)

    m = re.search(r"nodes (\d+).*doc (\d+) KB.*\n.*parse (\d+) ms.*heap peak (\d+) KB",
                  stats, re.S)
    info = "nodes %6s doc %5s KB peak %5s KB" % (m.group(1), m.group(2), m.group(4)) \
        if m else ""
    flag = "FAIL" if problems else ("trunc" if truncated else "ok")
    print("%-5s %8d B %s  %s" % (flag, len(raw), info, url))
    for p in problems:
        print("        " + p)
    fails += bool(problems)

print("%d page(s) with problems" % fails)
sys.exit(1 if fails else 0)
