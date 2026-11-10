#!/usr/bin/env python3
"""Trace switches and test doors read from the environment exist in test
builds only.

A test build is one configured with a non-empty KITTY_TEST_BUILD_LABEL. The
label reaches a source through the generated kitty_buildlabel.h, which
defines the macro in a test build only. Code under
#ifdef KITTY_TEST_BUILD_LABEL is therefore not in a release build at all.

What fails the build:
1. An environment read outside a KITTY_TEST_BUILD_LABEL conditional, of a
   name not listed in tools/test-doors.allow.
2. An environment read whose name is not a string literal outside such a
   conditional, in a function not listed in tools/test-doors.allow. A
   table of string literals walked as vars[i] or table[i].field is
   resolved: each literal counts as a read of that name.
3. A KITTY_TEST_BUILD_LABEL conditional in a file without
   #include "kitty_buildlabel.h". The macro is then never defined: the
   test build loses the code silently.
4. A file whose braces do not balance when only the first branch of each
   conditional is counted. The check cannot place its reads.

The readers: getenv, _wgetenv, _tgetenv, _dupenv_s, _wdupenv_s, getenv_s,
_wgetenv_s, GetEnvironmentVariable(A/W). Also ExpandEnvironmentStrings(A/W)
of a string literal: each %NAME% in it is a read. Text from a setting or a
user handed to ExpandEnvironmentStrings is not a read of a fixed name.
A function that hands one of its parameters to a reader as the name is a
reader too (kitty_getenv_dup, the probe helpers): its calls are checked.

Guarded: inside #ifdef KITTY_TEST_BUILD_LABEL, inside
#if defined(KITTY_TEST_BUILD_LABEL) [&& ...] without a top-level ||, or in
the #else of #ifndef KITTY_TEST_BUILD_LABEL. An #elif or #else of a guarded
branch is not guarded. Comments are ignored.

tools/test-doors.allow, one line per kept read:
    name | NAME | reason                    set by Windows, an admin or a user
    nonliteral | file | function | reason   a read of a non-literal name
A listed line that matches nothing fails too, so the list cannot go stale.

What counts: every .c and .h file of the source tree except test/,
windows/test/, unix/, contrib/, .git and build directories (a top-level
folder whose name starts with "build", or any folder holding a
CMakeCache.txt).

Run:  python3 tools/check-test-doors.py               (exit 1 on a finding)
      python3 tools/check-test-doors.py --self-test   (the check on planted
                                                        sources)
Options: --root DIR (source tree), --allow FILE, --list (every read found,
guarded or not).
"""
import os
import re
import sys
import shutil
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)
DEFAULT_ALLOW = os.path.join(HERE, "test-doors.allow")
LABEL = "KITTY_TEST_BUILD_LABEL"
SKIP_TOP = ("test", "unix", "contrib", ".git")

# reader -> index of the argument that holds the name
READERS = {
    "getenv": 0, "_wgetenv": 0, "_tgetenv": 0,
    "_dupenv_s": 2, "_wdupenv_s": 2, "getenv_s": 3, "_wgetenv_s": 3,
    "GetEnvironmentVariable": 0, "GetEnvironmentVariableA": 0,
    "GetEnvironmentVariableW": 0,
}
EXPANDERS = ("ExpandEnvironmentStrings", "ExpandEnvironmentStringsA",
             "ExpandEnvironmentStringsW")
KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "do",
            "else", "case", "defined", "__attribute__", "__declspec"}


def source_files(root):
    """Every .c / .h below root, as '/'-separated relative paths, sorted."""
    out = []
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        if rel == ".":
            dirs[:] = [x for x in dirs
                       if not (x.startswith("build") or x in SKIP_TOP)]
        dirs[:] = [x for x in dirs if x not in (".git", "test") and not
                   os.path.exists(os.path.join(d, x, "CMakeCache.txt"))]
        for f in files:
            if f.endswith((".c", ".h")):
                p = os.path.normpath(os.path.join(rel, f))
                out.append(p.replace(os.sep, "/"))
    return sorted(out)


def strip(src):
    """Comments blanked, line structure and length kept. String and char
    literals are skipped over intact: a name lives inside one."""
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
            j = i + 1
            while j < n and src[j] != c:
                if src[j] == "\\":
                    j += 1
                if j < n and src[j] == "\n":
                    break
                j += 1
            j = min(j + 1, n)
            out.append(src[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def blank_literals(t):
    """t with the inside of every literal blanked (for brace counting)."""
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                  lambda m: m.group(0)[0] + " " * (len(m.group(0)) - 2)
                  + m.group(0)[-1], t)


def call_args(text, open_pos):
    """The arguments of the call whose '(' is at open_pos, split at the top
    level commas. None when unbalanced."""
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
                return args
        elif c == "," and depth == 1:
            args.append(text[start:i])
            start = i + 1
        i += 1
    return None


LITERAL = re.compile(r'^(?:L|u8|u|U)?"((?:\\.|[^"\\])*)"$')


def literal(arg):
    m = LITERAL.match(arg.strip())
    return m.group(1) if m else None


def cond_kind(directive, expr):
    """(guarded branch, guarded #else) for an #if / #ifdef / #ifndef."""
    expr = re.sub(r"\s+", " ", expr).strip()
    if directive == "ifdef":
        return expr == LABEL, False
    if directive == "ifndef":
        return False, expr == LABEL
    if "||" in expr:
        return False, False
    terms = [x.strip() for x in expr.split("&&")]
    pos = re.compile(r"^defined\s*\(?\s*%s\s*\)?$" % LABEL)
    neg = re.compile(r"^!\s*defined\s*\(?\s*%s\s*\)?$" % LABEL)
    if any(pos.match(x) for x in terms):
        return True, False
    if len(terms) == 1 and neg.match(terms[0]):
        return False, True
    return False, False


class FileScan:
    """One file: the guard state per line, the enclosing function per
    position, the calls of a given name."""

    def __init__(self, text):
        self.t = strip(text)
        self.b = blank_literals(self.t)
        self.lines = self.t.split("\n")
        self.line_starts = [0]
        for ln in self.lines:
            self.line_starts.append(self.line_starts[-1] + len(ln) + 1)
        self.guard = []          # per line: guarded
        self.pp_line = []        # per line: a preprocessor line
        self.label_conds = []    # lines with a label conditional
        self.alt = []            # per line: in an #elif / #else branch
        stack = []               # [guarded now, guarded #else, alt]
        cont = False
        for no, ln in enumerate(self.lines, 1):
            s = ln.strip()
            is_pp = cont or s.startswith("#")
            self.pp_line.append(is_pp)
            if s.startswith("#") and not cont:
                m = re.match(r"#\s*(\w+)\s*(.*)$", s)
                d, e = (m.group(1), m.group(2).rstrip("\\")) if m else ("", "")
                if d in ("if", "ifdef", "ifndef", "elif") and LABEL in e:
                    self.label_conds.append(no)
                if d in ("if", "ifdef", "ifndef"):
                    stack.append(list(cond_kind(d, e)) + [False])
                elif d == "elif" and stack:
                    g, _ = cond_kind("if", e)
                    stack[-1] = [g, False, True]
                elif d == "else" and stack:
                    stack[-1] = [stack[-1][1], False, True]
                elif d == "endif" and stack:
                    stack.pop()
                self.guard.append(any(f[0] for f in stack) or
                                  d in ("if", "ifdef", "ifndef", "elif",
                                        "else", "endif"))
            else:
                self.guard.append(any(f[0] for f in stack))
            self.alt.append(any(f[2] for f in stack))
            cont = ln.rstrip().endswith("\\")
        self.funcs = self._functions()

    def line_of(self, pos):
        lo, hi = 0, len(self.line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.line_starts[mid] <= pos:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    def _functions(self):
        """(start, end, name, params) of every function body."""
        out = []
        b = self.b
        depth = 0
        seg_start = 0
        open_at = None
        cur = (None, [])
        self.balanced = True
        for i in self._code_positions():
            c = b[i]
            if c == "\n":
                if depth == 0:
                    seg_start = i
                continue
            if c == "{":
                if depth == 0:
                    open_at = i
                    head = b[seg_start:i]
                    name, params = None, []
                    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", head):
                        if m.group(1) in KEYWORDS:
                            continue
                        r = call_args(head, m.end() - 1)
                        if r is not None:
                            name = m.group(1)
                            params = [pname(p) for p in r]
                        break
                    cur = (name, params)
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0 and open_at is not None:
                    if cur[0]:
                        out.append((open_at, i, cur[0], cur[1]))
                    seg_start = i + 1
                    open_at = None
                if depth < 0:
                    depth = 0
                    self.balanced = False
            elif c == ";" and depth == 0:
                seg_start = i + 1
        if depth:
            self.balanced = False
        return out

    def _code_positions(self):
        """The positions of every brace and semicolon outside preprocessor
        lines. Also the newline ending each preprocessor line. Only the
        first branch of a conditional counts, so branches that each open
        a brace keep the depth right."""
        for no, ln in enumerate(self.lines):
            lo = self.line_starts[no]
            if self.pp_line[no]:
                yield lo + len(ln)
                continue
            if self.alt[no]:
                continue
            for m in re.finditer(r"[{};]", self.b[lo:lo + len(ln)]):
                yield lo + m.start()

    def func_at(self, pos):
        for s, e, name, params in self.funcs:
            if s < pos < e:
                return name, params
        return None, []

    def calls(self, name):
        """(pos, args, function, params) of each call of name."""
        out = []
        for m in re.finditer(r"(?<![\w.>])%s\s*\(" % re.escape(name), self.b):
            line = self.line_of(m.start())
            func, params = self.func_at(m.start())
            args = call_args(self.t, m.end() - 1)
            if args is None:
                continue
            if func is None and not self.pp_line[line - 1] and \
                    not any(literal(a) is not None for a in args):
                continue                  # a prototype or a definition
            out.append((m.start(), [a.strip() for a in args], func, params))
        return out


def pname(p):
    p = re.sub(r"\[[^\]]*\]", "", p).strip()
    ids = re.findall(r"[A-Za-z_]\w*", p)
    return ids[-1] if ids else ""


def table_names(scan, expr, pos):
    """The literals of the array a 'table[i]' or 'table[i].field' read
    walks, defined above pos in the same file. None when not found."""
    m = re.match(r"^([A-Za-z_]\w*)\s*\[[^\]]*\]\s*(?:\.\s*\w+)?$", expr)
    if not m:
        return None
    best = None
    for d in re.finditer(r"\b%s\s*\[\s*\w*\s*\]\s*=\s*\{" % m.group(1),
                         scan.b[:pos]):
        best = d
    if best is None:
        return None
    r = call_args(scan.t, best.end() - 1)
    if r is None:
        return None
    body = ",".join(r)
    return re.findall(r'"((?:\\.|[^"\\])*)"', body)


def scan_tree(root):
    """Every read as (file, line, name or None, guarded, function, expr).
    Also the files with a label conditional but no kitty_buildlabel.h."""
    scans = {}
    no_include = []
    for f in source_files(root):
        try:
            with open(os.path.join(root, f), encoding="utf-8",
                      errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        if LABEL not in text and not any(r in text for r in READERS) \
                and "ExpandEnvironmentStrings" not in text:
            continue
        s = FileScan(text)
        scans[f] = s
        if not s.balanced:
            no_include.append((f, 0))
        if s.label_conds and not re.search(
                r'#\s*include\s*[<"]kitty_buildlabel\.h[>"]', s.t):
            no_include.append((f, s.label_conds[0]))

    readers = dict((k, (v, "get")) for k, v in READERS.items())
    readers.update((k, (0, "expand")) for k in EXPANDERS)
    reads = []
    done = set()
    todo = list(readers)
    while todo:
        name = todo.pop(0)
        if name in done:
            continue
        done.add(name)
        idx, kind = readers[name]
        for f, s in scans.items():
            for pos, args, func, params in s.calls(name):
                if idx >= len(args):
                    continue
                arg = args[idx]
                line = s.line_of(pos)
                g = s.guard[line - 1]
                lit = literal(arg)
                if lit is not None:
                    names = (re.findall(r"%([^%\s\"]+)%", lit)
                             if kind == "expand" else [lit])
                    for n in names:
                        reads.append((f, line, n, g, func, arg))
                    continue
                if kind == "expand":
                    continue              # user text, not a fixed name
                if re.match(r"^[A-Za-z_]\w*$", arg) and arg in params:
                    if func not in readers:
                        readers[func] = (params.index(arg), kind)
                        todo.append(func)
                    continue
                names = table_names(s, arg, pos)
                if names:
                    for n in names:
                        reads.append((f, line, n, g, func, arg))
                    continue
                reads.append((f, line, None, g, func, arg))
    return reads, no_include


def read_allow(path):
    names, nonlit = [], []
    if not os.path.exists(path):
        return names, nonlit
    with open(path, encoding="utf-8") as fh:
        for n, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|")]
            if parts[0] == "name" and len(parts) == 3 and all(parts):
                names.append((parts[1], n))
            elif parts[0] == "nonliteral" and len(parts) == 4 and all(parts):
                nonlit.append((parts[1], parts[2], n))
            else:
                raise ValueError("%s:%d: want 'name | NAME | reason' or "
                                 "'nonliteral | file | function | reason'"
                                 % (path, n))
    return names, nonlit


def check(root, allow_path):
    names, nonlit = read_allow(allow_path)
    reads, no_include = scan_tree(root)
    used = set()
    findings = []
    for f, line in no_include:
        if line == 0:
            findings.append("%s: the braces do not balance in the first "
                            "branch of each conditional, so the check "
                            "cannot place the reads in their functions" % f)
            continue
        findings.append("%s:%d: %s conditional without #include "
                        "\"kitty_buildlabel.h\": never true" % (f, line, LABEL))
    for f, line, name, g, func, expr in sorted(
            reads, key=lambda r: (r[0], r[1], r[2] or "")):
        if name is None:
            hit = [a for a in nonlit if a[0] == f and a[1] == func]
            used.update(a[2] for a in hit)
            if not g and not hit:
                findings.append("%s:%d: environment read of %s in %s() "
                                "outside #ifdef %s" % (f, line, expr, func,
                                                       LABEL))
            continue
        hit = [a for a in names if a[0] == name]
        used.update(a[1] for a in hit)
        if not g and not hit:
            findings.append("%s:%d: %s read outside #ifdef %s"
                            % (f, line, name, LABEL))
    stale = ["%s:%d: allow line matches nothing" %
             (os.path.basename(allow_path), n)
             for n in sorted([a[1] for a in names] + [a[2] for a in nonlit])
             if n not in used]
    return findings, stale, reads


def self_test():
    planted = {
        "a.c": (
            '#include "kitty_buildlabel.h"\n'                      # 1
            'void f(void) {\n'                                      # 2
            '    const char *a = getenv("T_PLAIN");\n'              # 3: finding
            '#ifdef KITTY_TEST_BUILD_LABEL\n'                       # 4
            '    const char *b = getenv("T_GUARDED");\n'            # 5
            '#ifdef OTHER\n'                                        # 6
            '    GetEnvironmentVariableA("T_NESTED", 0, 0);\n'      # 7
            '#endif\n'                                              # 8
            '#else\n'                                               # 9
            '    const char *c = getenv("T_ELSE");\n'               # 10: finding
            '#endif\n'                                              # 11
            '#ifndef KITTY_TEST_BUILD_LABEL\n'                      # 12
            '    _wgetenv(L"T_IFNDEF");\n'                          # 13: finding
            '#else\n'                                               # 14
            '    _wgetenv(L"T_IFNDEF_ELSE");\n'                     # 15
            '#endif\n'                                              # 16
            '#if defined(KITTY_TEST_BUILD_LABEL) || defined(X)\n'   # 17
            '    getenv("T_OR");\n'                                 # 18: finding
            '#endif\n'                                              # 19
            '#if defined(MOD_PERSO) && defined(KITTY_TEST_BUILD_LABEL)\n'  # 20
            '    getenv("T_AND");\n'                                # 21
            '#endif\n'                                              # 22
            '    /* getenv("T_COMMENT"); */\n'                      # 23
            '    getenv("USERPROFILE");\n'                          # 24: allowed
            '    ExpandEnvironmentStringsA("%T_EXPAND%\\\\x", 0, 0);\n'  # 25: finding
            '    ExpandEnvironmentStringsA(user_text, 0, 0);\n'     # 26
            '}\n'                                                   # 27
            'static char *wrap(const char *name) {\n'               # 28
            '    return getenv(name);\n'                            # 29
            '}\n'                                                   # 30
            'static char *wrap2(int x, const char *nm) {\n'         # 31
            '    return wrap(nm);\n'                                # 32
            '}\n'                                                   # 33
            'void g(void) {\n'                                      # 34
            '    wrap2(1, "T_WRAPPED");\n'                          # 35: finding
            '    static const char *vars[] = { "T_TAB1", "APPDATA" };\n'  # 36
            '    for (i = 0; i < 2; i++) getenv(vars[i]);\n'        # 37: T_TAB1
            '    getenv(somewhere());\n'                            # 38: finding
            '}\n'                                                   # 39
            'void h(const char *p) { getenv(p + 1); }\n'            # 40: allowed
            '#define TRACE_ON (getenv("T_MACRO") != NULL)\n'        # 41: finding
            'void k(int a) {\n'                                     # 42
            '#ifdef X\n'                                            # 43
            '    if (a) {\n'                                        # 44
            '#else\n'                                               # 45
            '    if (!a) {\n'                                       # 46
            '#endif\n'                                              # 47
            '        a++;\n'                                        # 48
            '    }\n'                                               # 49
            '}\n'                                                   # 50
            'static char *wrap3(const char *n3) { return getenv(n3); }\n'  # 51
            'void m(void) { wrap3("T_AFTER_BRANCHES"); }\n'         # 52: finding
            ),
        "d.c": 'void f(void) { getenv("USERPROFILE");\n',          # unbalanced
        "b.c": (
            'void f(void) {\n'                                      # 1
            '#ifdef KITTY_TEST_BUILD_LABEL\n'                       # 2: finding
            '    getenv("T_NOINCLUDE");\n'                          # 3
            '#endif\n'
            '}\n'),
        "test/c.c": 'void f(void) { getenv("T_TESTDIR"); }\n',
    }
    tmp = tempfile.mkdtemp(prefix="testdoors_")
    try:
        for name, body in planted.items():
            p = os.path.join(tmp, name)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(body)
        allow = os.path.join(tmp, "allow")
        with open(allow, "w", encoding="utf-8") as fh:
            fh.write("name | USERPROFILE | the profile folder\n"
                     "name | APPDATA | the roaming folder\n"
                     "nonliteral | a.c | h | planted\n"
                     "name | T_GONE | stale on purpose\n"
                     "nonliteral | a.c | gone | stale on purpose\n")
        findings, stale, _ = check(tmp, allow)
        want = {("a.c", 3), ("a.c", 10), ("a.c", 13), ("a.c", 18),
                ("a.c", 25), ("a.c", 35), ("a.c", 37), ("a.c", 38),
                ("a.c", 41), ("a.c", 52), ("b.c", 2), ("d.c", 0)}
        got = set()
        for line in findings:
            f, n = line.split(":")[0], line.split(":")[1]
            got.add((f, int(n) if n.isdigit() else 0))
        names_ok = (any("T_WRAPPED" in x for x in findings) and
                    any("T_TAB1" in x for x in findings) and
                    not any("APPDATA" in x for x in findings) and
                    any("T_EXPAND" in x for x in findings))
        ok = (got == want and names_ok and len(stale) == 2 and
              len(findings) == len(want))
        if not ok:
            print("SELF-TEST FAILED")
            print("  want findings at", sorted(want), "got", sorted(got))
            for x in findings:
                print("  ", x)
            print("  stale:", stale)
            return 1
        print("check-test-doors self-test: OK (%d planted findings, "
              "2 stale allow lines)" % len(want))
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv):
    root, allow = DEFAULT_ROOT, DEFAULT_ALLOW
    show = False
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--self-test":
            return self_test()
        if a == "--list":
            show = True; i += 1; continue
        if a == "--root" and i + 1 < len(argv):
            root = argv[i + 1]; i += 2; continue
        if a == "--allow" and i + 1 < len(argv):
            allow = argv[i + 1]; i += 2; continue
        print("unknown argument: " + a)
        return 2
    findings, stale, reads = check(root, allow)
    if show:
        for f, line, name, g, func, expr in sorted(
                reads, key=lambda r: (r[0], r[1], r[2] or "")):
            print("%s:%d: %s %s in %s()" % (f, line, name or "<" + expr + ">",
                                           "guarded" if g else "OPEN", func))
    for line in findings + stale:
        print(line)
    if findings or stale:
        print("check-test-doors: %d finding(s), %d stale allow line(s)"
              % (len(findings), len(stale)))
        return 1
    print("check-test-doors: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
