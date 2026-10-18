#!/usr/bin/env python3
"""html5lib_tree.py UPARSE [DIR] [-v N] [-f FILE] - the HTML5 tree-construction
tests (html5lib's .dat files, kept in web-platform-tests under
html/syntax/parsing/resources; tests/fetch_html5lib.sh puts them in
build/wpt) against our tree builder.

Our tree is a reader's, not a DOM, so both trees are compared as we keep
them: no comments or doctype; elements we don't know are "?"; only the
attributes in html/tags.c, in name order; svg, math, template and the
other TF_DROP elements empty; script/style and the other TF_SKIPTEXT
elements without their text; white space collapsed and trimmed (layout
does that anyway). Fragment and scripting-on tests are skipped: we parse
whole documents and run no scripts.

Prints a pass count per .dat file and in all; -v N shows the first N
failures (or those of -f FILE only) with both trees. Exit status 0: this
is a score, not a gate."""
import glob, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def tables():
    """our element names (with their flags) and kept attribute names"""
    src = open(os.path.join(HERE, "..", "html", "tags.c")).read()
    tags = {}
    for name, flags in re.findall(r'\{ "([a-z0-9]+)", ([^}]*)\}', src):
        tags[name] = flags
    body = src[src.index("attrs[ATTR_COUNT]"):]
    body = body[body.index("{") + 1:body.index("};")]
    attrs = set(re.findall(r'"([a-z-]+)"', body))
    return tags, attrs


TAGS, ATTRS = tables()


def read_dat(path):
    """the tests of one .dat file: dicts of data, document, flags"""
    tests, cur, sect = [], None, None
    for line in open(path, encoding="utf-8", errors="surrogateescape"):
        line = line.rstrip("\n")
        if line == "#data":
            if cur:
                tests.append(cur)
            cur = {"data": [], "document": [], "flags": set()}
            sect = "data"
        elif cur is not None and line.startswith("#") and line[1:] in (
                "errors", "new-errors", "document", "document-fragment",
                "script-on", "script-off"):
            sect = line[1:]
            if sect in ("document-fragment", "script-on", "script-off"):
                cur["flags"].add(sect)
        elif cur is not None and sect in ("data", "document"):
            cur[sect].append(line)
    if cur:
        tests.append(cur)
    for t in tests:
        t["data"] = "\n".join(t["data"])
        while t["document"] and t["document"][-1] == "":
            t["document"].pop()
    return tests


class Node:
    def __init__(self, name, ns=None):
        self.name, self.ns = name, ns
        self.attrs, self.kids = {}, []


def parse_expected(lines):
    """html5lib's "| " tree into Nodes (None: the document)"""
    root = Node("#doc")
    stack = [(-1, root)]
    text = None
    for line in lines:
        if not line.startswith("| "):
            if text is not None:            # a text node's next line
                text[0].kids[text[1]] += "\n" + line
            continue
        body = line[2:]
        depth = (len(body) - len(body.lstrip(" "))) // 2
        body = body.strip(" ") if not body.lstrip(" ").startswith('"') \
            else body.lstrip(" ")
        while stack[-1][0] >= depth:
            stack.pop()
        parent = stack[-1][1]
        text = None
        if body.startswith('"'):
            parent.kids.append(body[1:])
            text = (parent, len(parent.kids) - 1)
        elif body.startswith("<!--") or body.startswith("<!DOCTYPE") \
                or body.startswith("<?"):
            parent.kids.append(None)        # a comment: splits text
            stack.append((depth, Node("#skip")))
        elif body.startswith("<"):
            parts = body[1:-1].split(" ", 1)
            n = Node(parts[1], parts[0]) if len(parts) > 1 else Node(parts[0])
            parent.kids.append(n)
            stack.append((depth, n))
        elif body == "content":             # a template's content
            stack.append((depth, parent))
        else:                               # an attribute
            m = re.match(r'(?:(\S+) )?([^=]+)="(.*)"$', body, re.S)
            if m and isinstance(parent, Node):
                parent.attrs[(m.group(1), m.group(2))] = m.group(3)
    # strip the closing quote of text nodes
    def fix(n):
        for i, k in enumerate(n.kids):
            if isinstance(k, str):
                n.kids[i] = k[:-1] if k.endswith('"') else k
            elif isinstance(k, Node):
                fix(k)
    fix(root)
    return root


def parse_ours(dump):
    root = Node("#doc")
    stack = [(-1, root)]
    for line in dump.split("\n"):
        if not line or line == "[truncated]":
            continue
        body = line.lstrip(" ")
        depth = (len(line) - len(body)) // 2
        while stack[-1][0] >= depth:
            stack.pop()
        parent = stack[-1][1]
        if body.startswith('"'):
            s = body[1:-1].replace('\\"', '"').replace("\\n", "\n")
            parent.kids.append(s)
        else:
            m = re.match(r"<(\S+?)( .*)?>$", body)
            n = Node(m.group(1))
            for a, v in re.findall(r' ([a-z-]+)="(.*?)"(?= [a-z-]+="|$)',
                                   m.group(2) or ""):
                n.attrs[(None, a)] = v
            parent.kids.append(n)
            stack.append((depth, n))
    return root


def norm(n, ours):
    """the tree as we keep it, serialised"""
    out = []

    def walk(n, depth):
        name = n.name
        flags = ""
        if not ours:
            if n.ns in ("svg", "math"):
                name = n.ns if n.ns == n.name else None
            elif name not in TAGS:
                name = "?"
        if name is None:
            return
        flags = TAGS.get(name, "")
        attrs = sorted((k[1], v) for k, v in n.attrs.items()
                       if k[0] is None and k[1] in ATTRS)
        out.append("  " * depth + "<" + name
                   + "".join(' %s="%s"' % a for a in attrs) + ">")
        if "TF_DROP" in flags or n.ns in ("svg", "math"):
            return
        run = []

        def flush():
            s = re.sub(r"\s+", " ", "".join(run)).strip()
            if s:
                out.append("  " * (depth + 1) + '"' + s + '"')
            run.clear()
        for k in n.kids:
            if isinstance(k, str):
                if "TF_SKIPTEXT" not in flags:
                    run.append(k)
            elif k is None:
                pass                        # comments: text runs on
            else:
                # an element that leaves no trace doesn't split text
                if not ours and k.ns in ("svg", "math") and k.ns != k.name:
                    continue
                flush()
                walk(k, depth + 1)
        flush()

    for k in n.kids:
        if isinstance(k, Node):
            walk(k, 0)
    return "\n".join(out) + "\n"


def run_ours(uparse, data):
    p = subprocess.run([uparse, "-d", "-C", "utf-8", "-"],
                       input=data.encode("utf-8", "surrogateescape"),
                       capture_output=True, timeout=20)
    if p.returncode != 0:
        return None, "exit %d: %s" % (p.returncode, p.stderr[-300:])
    return p.stdout.decode("utf-8", "replace"), None


def main():
    args = sys.argv[1:]
    verbose, only = 0, None
    if "-v" in args:
        i = args.index("-v"); verbose = int(args[i + 1]); del args[i:i + 2]
    if "-f" in args:
        i = args.index("-f"); only = args[i + 1]; del args[i:i + 2]
    uparse = args[0]
    d = args[1] if len(args) > 1 else "build/wpt/html/syntax/parsing"
    files = sorted(glob.glob(os.path.join(d, "**", "*.dat"), recursive=True))
    if not files:
        sys.exit("html5lib_tree: no .dat files in %s "
                 "(tests/fetch_html5lib.sh)" % d)
    total = passed = skipped = shown = 0
    for f in files:
        name = os.path.basename(f)
        if only and name != only:
            continue
        fp = ft = 0
        for t in read_dat(f):
            if t["flags"] & {"document-fragment", "script-on"}:
                skipped += 1
                continue
            ft += 1
            want = norm(parse_expected(t["document"]), False)
            dump, err = run_ours(uparse, t["data"])
            got = norm(parse_ours(dump), True) if dump is not None else err
            if got == want:
                fp += 1
            elif shown < verbose:
                shown += 1
                print("=== %s: %r\n--- want\n%s--- got\n%s"
                      % (name, t["data"][:300], want, got))
        total += ft
        passed += fp
        if ft:
            print("%-40s %4d/%-4d" % (name, fp, ft))
    print("%-40s %4d/%-4d %.1f%% (%d fragment/scripted skipped)"
          % ("all", passed, total, 100.0 * passed / max(total, 1), skipped))


if __name__ == "__main__":
    main()
