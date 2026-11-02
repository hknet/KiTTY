#!/usr/bin/env python3
"""A buffer wiped and then freed must be wiped with smemclr.

`memset(p, 0, n); free(p);` looks like a wipe, but the compiler may drop it:
nothing reads p after the free, so the store is dead and GCC at -O2 removes
it. The secret then stays in freed memory. smemclr
(utils/smemclr.c) cannot be dropped: it hands the buffer to a function
called through a volatile pointer, which the compiler must assume reads it.

What fails the build:
1. a memset(P, ...) - with ANY fill value - whose statement is followed
   directly (only braces between) by free(P) or sfree(P) of the same
   pointer expression;
2. any SecureZeroMemory call: the tree uses smemclr only, one name to look
   for.
Comments and string literals are ignored.

tools/secure-wipe.allow, one line per kept memset-before-free of a buffer
that holds no secret:
    file | pointer | reason
A listed line that matches nothing fails too, so the list cannot go stale.

What counts: every .c and .h file of the source tree, upstream PuTTY files
included; not unix/, .git or build directories (a top-level folder whose
name starts with "build", or any folder holding a CMakeCache.txt).

Run:  python3 tools/check-secure-wipe.py               (exit 1 on a finding)
      python3 tools/check-secure-wipe.py --self-test   (the check on planted
                                                         sources)
Options: --root DIR (source tree), --allow FILE.
"""
import os
import re
import sys
import shutil
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)
DEFAULT_ALLOW = os.path.join(HERE, "secure-wipe.allow")


def source_files(root):
    """Every .c / .h below root, as '/'-separated relative paths, sorted."""
    out = []
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        if rel == ".":
            dirs[:] = [x for x in dirs
                       if not (x.startswith("build") or x in (".git", "unix"))]
        dirs[:] = [x for x in dirs if x != ".git" and not os.path.exists(
            os.path.join(d, x, "CMakeCache.txt"))]
        for f in files:
            if f.endswith((".c", ".h")):
                p = os.path.normpath(os.path.join(rel, f))
                out.append(p.replace(os.sep, "/"))
    return sorted(out)


def strip(src):
    """Comments and the inside of literals blanked, line structure kept."""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j]))
            i = j
        elif c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            q = c
            j = i + 1
            while j < n and src[j] != q:
                if src[j] == "\\":
                    j += 1
                if j < n and src[j] == "\n":
                    break
                j += 1
            j = min(j + 1, n)
            body = src[i:j]
            out.append(q + "".join("\n" if ch == "\n" else " "
                                   for ch in body[1:-1])
                       + (q if len(body) > 1 else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def call_args(text, open_pos):
    """The arguments of the call whose '(' is at open_pos, split at the top
    level commas, and the position after its ')'; None when unbalanced."""
    depth = 0
    args = []
    start = open_pos + 1
    i = open_pos
    while i < len(text):
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[start:i])
                return args, i + 1
        elif c == "," and depth == 1:
            args.append(text[start:i])
            start = i + 1
        i += 1
    return None


def norm(expr):
    return re.sub(r"\s+", "", expr)


MEMSET = re.compile(r"\bmemset\s*\(")
FREE = re.compile(r"(?:s?free)\s*\(")
SZM = re.compile(r"\bSecureZeroMemory\s*\(")


def scan_text(text):
    """(kind, line, pointer) for every finding in one file's text."""
    t = strip(text)
    found = []

    def line_at(pos):
        return t.count("\n", 0, pos) + 1

    for m in SZM.finditer(t):
        found.append(("szm", line_at(m.start()), ""))
    for m in MEMSET.finditer(t):
        r = call_args(t, m.end() - 1)
        if not r or not r[0]:
            continue
        args, after = r
        ptr = norm(args[0])
        semi = t.find(";", after)
        if semi < 0 or t[after:semi].strip():
            continue                       # the memset is not a statement of its own
        k = semi + 1
        while k < len(t) and (t[k].isspace() or t[k] in "{}"):
            k += 1
        f = FREE.match(t, k)
        if not f or (k > 0 and (t[k - 1].isalnum() or t[k - 1] == "_")):
            continue
        fr = call_args(t, f.end() - 1)
        if fr and len(fr[0]) == 1 and norm(fr[0][0]) == ptr:
            found.append(("memfree", line_at(m.start()), ptr))
    return found


def read_allow(path):
    items = []
    if not os.path.exists(path):
        return items
    with open(path, encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|")]
            if len(parts) != 3 or not all(parts):
                raise ValueError("%s:%d: want 'file | pointer | reason'" % (path, n))
            items.append((parts[0], norm(parts[1]), n))
    return items


def check(root, allow_path):
    allow = read_allow(allow_path)
    used = set()
    findings = []
    for f in source_files(root):
        try:
            with open(os.path.join(root, f), encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        for kind, line, ptr in scan_text(text):
            if kind == "memfree":
                hit = [a for a in allow if a[0] == f and a[1] == ptr]
                if hit:
                    used.update(a[2] for a in hit)
                    continue
                findings.append("%s:%d: memset(%s, ...) then free(%s): the wipe "
                                "may be dropped - use smemclr" % (f, line, ptr, ptr))
            else:
                findings.append("%s:%d: SecureZeroMemory - use smemclr" % (f, line))
    stale = ["%s:%d: allow line matches nothing: %s | %s"
             % (os.path.basename(allow_path), a[2], a[0], a[1])
             for a in allow if a[2] not in used]
    return findings, stale


def self_test():
    planted = {
        "a.c": (
            "void f(char *p, char *q) {\n"
            "    memset(p, 0, strlen(p)); free(p);\n"            # 2: finding
            "    memset( q , 1 , 4 ) ;\n"                         # 3: finding (fill 1)
            "    sfree( q );\n"
            "    if (p) { memset(p->buf, 0, 8); }\n"              # 5: finding (brace)
            "    sfree(p->buf);\n"
            "    SecureZeroMemory(p, 4);\n"                       # 7: finding
            "    smemclr(p, 4); free(p);\n"                       # fine
            "    memset(p, 0, 4); n = 1; free(p);\n"              # fine: a statement between
            "    memset(p, 0, 4); free(q);\n"                     # fine: another pointer
            "    /* SecureZeroMemory(p, 4); */\n"                 # fine: comment
            "    // memset(p, 0, 4); free(p);\n"                  # fine: comment
            "    puts(\"memset(p,0,4); free(p);\");\n"           # fine: literal
            "    memset(ub->buf, 0, 8); free(ub->buf);\n"         # allowed
            "}\n"),
    }
    tmp = tempfile.mkdtemp(prefix="secwipe_")
    try:
        for name, body in planted.items():
            with open(os.path.join(tmp, name), "w", encoding="utf-8") as fh:
                fh.write(body)
        allow = os.path.join(tmp, "allow")
        with open(allow, "w", encoding="utf-8") as fh:
            fh.write("a.c | ub->buf | not a secret\n"
                     "a.c | gone | stale on purpose\n")
        findings, stale = check(tmp, allow)
        want = {("a.c", 2), ("a.c", 3), ("a.c", 5), ("a.c", 7)}
        got = set()
        for line in findings:
            f, n = line.split(":")[0], int(line.split(":")[1])
            got.add((f, n))
        ok = got == want and len(stale) == 1 and "gone" in stale[0]
        if not ok:
            print("SELF-TEST FAILED")
            print("  want findings at", sorted(want), "got", sorted(got))
            print("  stale:", stale)
            return 1
        print("check-secure-wipe self-test: OK (%d planted findings, 1 stale allow line)"
              % len(want))
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv):
    root, allow = DEFAULT_ROOT, DEFAULT_ALLOW
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--self-test":
            return self_test()
        if a == "--root" and i + 1 < len(argv):
            root = argv[i + 1]; i += 2; continue
        if a == "--allow" and i + 1 < len(argv):
            allow = argv[i + 1]; i += 2; continue
        print("unknown argument: " + a)
        return 2
    findings, stale = check(root, allow)
    for line in findings + stale:
        print(line)
    if findings or stale:
        print("check-secure-wipe: %d finding(s), %d stale allow line(s)"
              % (len(findings), len(stale)))
        return 1
    print("check-secure-wipe: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
