#!/usr/bin/env python3
"""Fixed character arrays of 256 bytes or more, or sized with MAX_PATH, need
a reason.

A fixed buffer that holds a path, a setting or text from outside is cut, or
overflowed, by the first value longer than its size. The cut is silent: a
shorter path opens another file, a log names the wrong one, a value is lost.
So every char-like array whose size is MAX_PATH (or an expression with it),
or 256 bytes or more, must either go - the value held in memory sized to it
(dupstr, dupprintf, dupcat, strbuf, snewn) - or be listed in
tools/fixed-arrays.allow with the reason its size is safe. A listed line
whose array no longer exists fails too, so the list cannot go stale.

What counts:
- every .c and .h file of the source tree, upstream PuTTY files included;
  not unix/, .git or build directories (a top-level folder whose name starts
  with "build", or any folder holding a CMakeCache.txt);
- element types char, unsigned/signed char, wchar_t, WCHAR, TCHAR, CHAR,
  BYTE, uint8_t and the like, and every typedef of one of them;
- locals, statics, globals, extern declarations, struct and union members,
  array typedefs, several declarators on one line, and arrays written inside
  #define bodies; a multi-dimensional array by its LAST dimension;
- the size after expanding #defines and enum constants of the tree (a
  #define in the same file wins). A size that does not evaluate (sizeof, an
  unknown name) counts as large: the check cannot prove it small.
Pointers, arrays of pointers, function parameters and arrays sized by their
initialiser (char x[] = "...") are not fixed buffers and are not counted.

tools/fixed-arrays.allow, one line per kept array:
    file | function or struct | name | size | reason
- function: "name()"; a struct or union member: "struct name" / "union
  name"; an array inside a #define: "#define NAME"; file scope: "(file)";
- size as written in the source (spaces do not matter);
- a line matches by these four fields, never by line number. Two arrays with
  the same four fields need two lines.

Run:  python3 tools/check-fixed-arrays.py               (exit 1 on a finding)
      python3 tools/check-fixed-arrays.py --self-test   (the check on planted
                                                          sources)
      python3 tools/check-fixed-arrays.py --list        (every counted array
                                                          as an allow line)
Options: --root DIR (source tree), --allow FILE.
"""
import os
import re
import sys
import tempfile
import shutil
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)
DEFAULT_ALLOW = os.path.join(HERE, "fixed-arrays.allow")

LIMIT = 256

# Windows SDK / C library constants the tree uses without defining them.
KNOWN = {
    "MAX_PATH": "260", "_MAX_PATH": "260", "FILENAME_MAX": "260",
    "MAX_COMPUTERNAME_LENGTH": "15", "UNLEN": "256", "LF_FACESIZE": "32",
    "BUFSIZ": "512", "_MAX_FNAME": "256", "_MAX_EXT": "256", "_MAX_DIR": "256",
    "_MAX_DRIVE": "3", "NI_MAXHOST": "1025", "NI_MAXSERV": "32",
    "INET6_ADDRSTRLEN": "65", "INET_ADDRSTRLEN": "22",
    "INTERNET_MAX_URL_LENGTH": "2083", "MAX_MODULE_NAME32": "255",
    "CHAR_BIT": "8", "UNCLEN": "17", "DNLEN": "15", "PWLEN": "256",
    "MAXIMUM_REPARSE_DATA_BUFFER_SIZE": "16384", "CCHDEVICENAME": "32",
    "MAX_ADAPTER_NAME_LENGTH": "256", "UINT8_MAX": "255", "MB_LEN_MAX": "5",
    "UCHAR_MAX": "255", "SHRT_MAX": "32767",
}
PATH_NAMES = ("MAX_PATH", "_MAX_PATH")
BASE_TYPES = {
    "char", "wchar_t", "WCHAR", "TCHAR", "CHAR", "BYTE", "uint8_t", "int8_t",
    "UCHAR", "UINT8", "INT8", "u_char", "TBYTE", "CCHAR", "BOOLEAN",
    "char16_t", "BCHAR", "u8",
}
TYPEDEF_RE = re.compile(
    r"\btypedef\s+((?:unsigned|signed|const)\s+)*([A-Za-z_]\w*)\s+"
    r"([A-Za-z_]\w*)\s*;")
DEFINE_RE = re.compile(r"\s*#\s*define\s+([A-Za-z_]\w*)(\([^)]*\))?\s*(.*)$")
TOK = re.compile(r"[A-Za-z_]\w*|\d[\w.]*|##|#|->|\.\.\.|[^\s\w]", re.S)


# ---------------------------------------------------------------- sources

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


def logical_lines(text):
    """(line number, logical line) with backslash continuations joined."""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        start = i
        line = lines[i]
        while line.endswith("\\") and i + 1 < len(lines):
            i += 1
            line = line[:-1] + " " + lines[i]
        yield start + 1, line
        i += 1


# ---------------------------------------------------------------- the tree

class Tree:
    """The macro, enum and typedef tables of one source tree."""

    def __init__(self, root, files):
        self.defs = {}
        self.enums = {}
        self.stripped = {}
        typedef_char = set()
        for f in files:
            try:
                with open(os.path.join(root, f), encoding="utf-8",
                          errors="replace") as fh:
                    self.stripped[f] = strip(fh.read())
            except OSError:
                pass
        for f, s in self.stripped.items():
            for _ln, line in logical_lines(s):
                m = DEFINE_RE.match(line)
                if m and not m.group(2):
                    self.defs.setdefault(m.group(1), []).append(
                        (f, m.group(3).strip()))
            for m in re.finditer(r"\benum\b[^{;]*\{([^}]*)\}", s):
                val = -1
                for part in m.group(1).split(","):
                    part = part.strip()
                    if not part:
                        continue
                    mm = re.match(r"([A-Za-z_]\w*)\s*(?:=\s*(.+))?$", part,
                                  re.S)
                    if not mm:
                        continue
                    if mm.group(2) is not None:
                        self.enums.setdefault(mm.group(1), []).append(
                            (f, mm.group(2).strip()))
                        val = ("expr", mm.group(2).strip(), 0)
                    elif isinstance(val, tuple):
                        val = (val[0], val[1], val[2] + 1)
                        self.enums.setdefault(mm.group(1), []).append(
                            (f, "(%s)+%d" % (val[1], val[2])))
                    else:
                        val += 1
                        self.enums.setdefault(mm.group(1), []).append(
                            (f, str(val)))
            for m in TYPEDEF_RE.finditer(s):
                if m.group(2) in BASE_TYPES:
                    typedef_char.add(m.group(3))
        changed = True
        while changed:
            changed = False
            for s in self.stripped.values():
                for m in TYPEDEF_RE.finditer(s):
                    if (m.group(2) in typedef_char
                            and m.group(3) not in typedef_char
                            and m.group(3) not in BASE_TYPES):
                        typedef_char.add(m.group(3))
                        changed = True
        self.elem = BASE_TYPES | typedef_char

    def lookup(self, name, f):
        cands = self.defs.get(name, []) + self.enums.get(name, [])
        if not cands:
            return (KNOWN[name], False) if name in KNOWN else (None, False)
        same = [b for (ff, b) in cands if ff == f]
        if same:
            return same[0], False
        bodies = sorted(set(b for (_ff, b) in cands))
        if len(bodies) == 1:
            return bodies[0], False
        return bodies, True

    def expand(self, expr, f, depth=0, seen=frozenset()):
        """(expanded text, mentions MAX_PATH)"""
        mp = [False]
        if depth > 20:
            return expr, False

        def rep(m):
            name = m.group(0)
            if name in PATH_NAMES:
                mp[0] = True
            if name == "sizeof" or name in seen:
                return name
            body, several = self.lookup(name, f)
            if body is None:
                return name
            if several:
                vals = []
                for b in body:
                    t, m2 = self.expand(b, f, depth + 1, seen | {name})
                    v = evaluate(t)
                    if v is not None:
                        vals.append(v)
                    if m2:
                        mp[0] = True
                return "(%d)" % max(vals) if vals else name
            t, m2 = self.expand(body, f, depth + 1, seen | {name})
            if m2:
                mp[0] = True
            return "(%s)" % t if t else name

        out = re.sub(r"\b[A-Za-z_]\w*\b", rep, expr)
        return out, mp[0]


def evaluate(t):
    t2 = re.sub(r"\b(\d+)[uUlL]+\b", r"\1", t)
    t2 = re.sub(r"\(\s*(?:size_t|int|unsigned|DWORD|long)\s*\)", "", t2)
    t2 = re.sub(r"\b0[xX]([0-9a-fA-F]+)\b",
                lambda m: str(int(m.group(1), 16)), t2)
    t2 = re.sub(r"\b(\d+)[uUlL]+\b", r"\1", t2)
    if re.search(r"[A-Za-z_]", t2):
        return None
    if not re.fullmatch(r"[\d\s()+\-*/%<>&|^~?:.=!]*", t2):
        return None
    t2 = t2.replace("/", "//")
    try:
        return int(eval(t2, {"__builtins__": {}}, {}))
    except Exception:
        return None


# ---------------------------------------------------------------- scanning

QUALS = ("static", "const", "volatile", "extern", "unsigned", "signed",
         "register", "typedef", "THREAD_LOCAL", "__thread", "inline", "auto")


def line_of(starts, pos):
    import bisect
    return bisect.bisect_right(starts, pos)


def scan_file(tree, f):
    """Every array declaration of a char-like type in one file."""
    s = tree.stripped[f]
    elem = tree.elem
    hits = []
    lines = s.split("\n")
    pp_line = [False] * (len(lines) + 2)
    i = 0
    while i < len(lines):
        j = i
        while lines[j].rstrip().endswith("\\") and j + 1 < len(lines):
            j += 1
        logical = " ".join(x.rstrip().rstrip("\\") for x in lines[i:j + 1])
        if re.match(r"\s*#\s*(\w+)", logical):
            for k in range(i, j + 1):
                pp_line[k + 1] = True
        i = j + 1
    code = "\n".join("" if pp_line[k + 1] else lines[k]
                     for k in range(len(lines)))
    starts = [0] + [m.end() for m in re.finditer("\n", code)]
    toks = [(m.group(0), m.start()) for m in TOK.finditer(code)]
    n = len(toks)

    def block_label(idx):
        j = idx - 1
        depth = 0
        seg = []
        while j >= 0:
            t = toks[j][0]
            if t == ")":
                depth += 1
            elif t == "(":
                depth -= 1
            if depth == 0 and t in (";", "{", "}"):
                break
            seg.append(t)
            j -= 1
        seg.reverse()
        if "=" in seg and not ("(" in seg and seg.index("=") > seg.index("(")):
            return ("init", "")
        for kw in ("struct", "union", "enum"):
            if kw in seg and not (seg and seg[-1] == ")"):
                p = seg.index(kw)
                name = (seg[p + 1] if p + 1 < len(seg)
                        and re.match(r"[A-Za-z_]", seg[p + 1]) else "")
                return (kw, name)
        if seg and seg[-1] == ")":
            d = 0
            for q, t in enumerate(seg):
                if t == "(":
                    if d == 0 and q > 0 and re.match(r"[A-Za-z_]\w*$",
                                                     seg[q - 1]):
                        if seg[q - 1] in ("if", "while", "for", "switch"):
                            return ("ctl", seg[q - 1])
                        return ("func", seg[q - 1])
                    d += 1
                elif t == ")":
                    d -= 1
        if seg and seg[-1] in ("else", "do"):
            return ("ctl", seg[-1])
        if not seg:
            return ("block", "")
        return ("other", "")

    def after_close_name(idx):
        j = idx + 1
        while j < n and toks[j][0] == "*":
            j += 1
        if j < n and re.match(r"[A-Za-z_]\w*$", toks[j][0]):
            return toks[j][0]
        return ""

    match = {}
    st = []
    for q, (t, _pos) in enumerate(toks):
        if t == "{":
            st.append(q)
        elif t == "}" and st:
            match[st.pop()] = q

    blocks = []
    paren = 0
    q = 0
    while q < n:
        t = toks[q][0]
        if t == "(":
            paren += 1
        elif t == ")":
            paren = max(0, paren - 1)
        elif t == "{":
            kind, label = block_label(q)
            if kind in ("struct", "union") and not label and q in match:
                label = after_close_name(match[q]) or "(anonymous)"
            blocks.append((kind, label, paren))
            paren = 0
        elif t == "}":
            if blocks:
                _k, _l, paren = blocks.pop()
        elif t in elem and paren == 0:
            prevt = toks[q - 1][0] if q > 0 else ";"
            if prevt in ("sizeof", ".", "->"):
                q += 1
                continue
            j = q - 1
            quals = []
            while j >= 0 and toks[j][0] in QUALS:
                quals.append(toks[j][0])
                j -= 1
            if j >= 0 and toks[j][0] not in (";", "{", "}", ")", ":", ","):
                if (not re.match(r"[A-Za-z_]", toks[j][0])
                        or toks[j][0] in ("return", "case", "goto")):
                    q += 1
                    continue
                if toks[j][0] not in ("static", "const", "PRINTF_LIKE",
                                      "NORETURN", "WINAPI", "CALLBACK"):
                    q += 1
                    continue
            r = q + 1
            while r < n and toks[r][0] in ("const", "volatile", "char", "int",
                                           "short", "long", "unsigned",
                                           "signed"):
                r += 1
            while True:
                ptr = False
                parenwrap = False
                while r < n and toks[r][0] in ("*", "const", "volatile", "far",
                                               "near", "WINAPI", "__restrict",
                                               "restrict"):
                    if toks[r][0] == "*":
                        ptr = True
                    r += 1
                if r < n and toks[r][0] == "(":
                    parenwrap = True
                    d = 0
                    while r < n:
                        if toks[r][0] == "(":
                            d += 1
                        elif toks[r][0] == ")":
                            d -= 1
                            if d == 0:
                                r += 1
                                break
                        r += 1
                    name = None
                    name_pos = None
                else:
                    name = (toks[r][0] if r < n
                            and re.match(r"[A-Za-z_]\w*$", toks[r][0])
                            else None)
                    if name is None:
                        break
                    name_pos = toks[r][1]
                    r += 1
                dims = []
                while r < n and toks[r][0] == "[":
                    d = 0
                    startpos = toks[r][1]
                    while r < n:
                        if toks[r][0] == "[":
                            d += 1
                        elif toks[r][0] == "]":
                            d -= 1
                            if d == 0:
                                break
                        r += 1
                    endpos = toks[r][1] if r < n else len(code)
                    dims.append(code[startpos + 1:endpos].strip())
                    r += 1
                if r < n and toks[r][0] == "(":
                    break                        # a function declaration
                if name and dims and not ptr and not parenwrap:
                    hits.append({
                        "file": f, "line": line_of(starts, name_pos),
                        "name": name,
                        "type": (" ".join(reversed(quals)) + " " + t).strip(),
                        "dims": dims, "blocks": [(b[0], b[1]) for b in blocks],
                        "macro": None})
                if r < n and toks[r][0] == "=":
                    d = 0
                    while r < n:
                        tt = toks[r][0]
                        if tt in ("{", "(", "["):
                            d += 1
                        elif tt in ("}", ")", "]"):
                            d -= 1
                        elif d == 0 and tt in (",", ";"):
                            break
                        r += 1
                if r < n and toks[r][0] == ",":
                    r += 1
                    continue
                break
        q += 1

    elem_alt = "|".join(sorted(elem, key=len, reverse=True))
    decl_re = re.compile(r"\b(" + elem_alt +
                         r")\b\s+((?:[A-Za-z_]\w*\s*\[[^\]]+\]\s*)+)")
    for ln, line in logical_lines(s):
        m = DEFINE_RE.match(line)
        if not m:
            continue
        for mm in decl_re.finditer(m.group(3)):
            for dm in re.finditer(r"([A-Za-z_]\w*)\s*((?:\[[^\]]+\]\s*)+)",
                                  mm.group(2)):
                hits.append({
                    "file": f, "line": ln, "name": dm.group(1),
                    "type": mm.group(1),
                    "dims": re.findall(r"\[([^\]]+)\]", dm.group(2)),
                    "blocks": [], "macro": m.group(1)})
    return hits


def context_of(h):
    if h["macro"]:
        return "#define " + h["macro"]
    blocks = h["blocks"]
    if blocks and blocks[-1][0] in ("struct", "union"):
        return "%s %s" % blocks[-1]
    func = ""
    for kind, label in blocks:
        if kind == "func":
            func = label
    if func:
        return func + "()"
    return "(file)"


def norm_size(s):
    return re.sub(r"\s+", "", s)


def scan(root):
    """Every counted array: list of dicts (file, line, ctx, name, size,
    value, maxpath, type)."""
    files = source_files(root)
    tree = Tree(root, files)
    out = []
    for f in files:
        if f not in tree.stripped:
            continue
        for h in scan_file(tree, f):
            last = h["dims"][-1]
            if not last.strip():
                continue
            txt, mp = tree.expand(last, f)
            v = evaluate(txt)
            if not (mp or v is None or v >= LIMIT):
                continue
            out.append({"file": f, "line": h["line"], "ctx": context_of(h),
                        "name": h["name"], "size": " ".join(last.split()),
                        "value": v, "maxpath": mp, "type": h["type"]})
    return out, len(files)


# ---------------------------------------------------------------- the list

def read_allow(path):
    """[(line number, file, ctx, name, size, reason)] and format errors."""
    entries = []
    errors = []
    try:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as e:
        return entries, ["%s: cannot read it: %s" % (path, e.strerror)]
    for no, raw in enumerate(text.split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split("|", 4)]
        if len(parts) != 5 or not all(parts):
            errors.append("%s:%d: not \"file | function or struct | name | "
                          "size | reason\": %s" % (path, no, line))
            continue
        entries.append((no, parts[0], parts[1], parts[2], parts[3], parts[4]))
    return entries, errors


def key_of(f, ctx, name, size):
    return (f, " ".join(ctx.split()), name, norm_size(size))


def allow_line(a, reason):
    return "%s | %s | %s | %s | %s" % (a["file"], a["ctx"], a["name"],
                                        a["size"], reason)


def check(root, allow_path):
    """(findings, stale lines, format errors, counted arrays, files)"""
    arrays, nfiles = scan(root)
    entries, errors = read_allow(allow_path)
    code = OrderedDict()
    for a in arrays:
        code.setdefault(key_of(a["file"], a["ctx"], a["name"], a["size"]),
                        []).append(a)
    listed = OrderedDict()
    for e in entries:
        listed.setdefault(key_of(e[1], e[2], e[3], e[4]), []).append(e)
    findings = []
    stale = []
    for k, arr in code.items():
        have = len(listed.get(k, []))
        if len(arr) > have:
            findings.append((arr, have))
    for k, ents in listed.items():
        have = len(code.get(k, []))
        if len(ents) > have:
            stale.append((ents[have:], have))
    return findings, stale, errors, arrays, nfiles


def describe(a):
    why = ("sized with MAX_PATH" if a["maxpath"] else
           ("its size does not evaluate here, so it counts as large"
            if a["value"] is None else "%d bytes or more" % LIMIT))
    val = "" if a["value"] is None or a["size"] == str(a["value"]) else \
        " (= %d)" % a["value"]
    where = "" if a["ctx"] == "(file)" else " in " + a["ctx"]
    return "%s:%d: %s %s[%s]%s%s - %s" % (a["file"], a["line"], a["type"],
                                          a["name"], a["size"], val, where,
                                          why)


def report(findings, stale, errors, allow_rel, out):
    if errors:
        for e in errors:
            out.write("check-fixed-arrays: %s\n" % e)
    if findings:
        n = sum(len(arr) - have for arr, have in findings)
        out.write("check-fixed-arrays: %d fixed array%s not in %s:\n\n"
                  % (n, "" if n == 1 else "s", allow_rel))
        for arr, have in findings:
            for a in arr:
                out.write("  %s\n" % describe(a))
            if have:
                out.write("    (%d arrays with this function, name and size, "
                          "%d line%s in the list)\n"
                          % (len(arr), have, "" if have == 1 else "s"))
        out.write(
            "\nHold the value in memory sized to it instead (dupstr, "
            "dupprintf, dupcat,\nstrbuf, snewn of the length the call "
            "returns), free it on every way out, and\nlet a path too long "
            "for Windows fail with Windows' own error. An array that\nmust "
            "stay fixed (a protocol or API size, a name Windows bounds) gets "
            "one line\nin %s with the reason its size is safe:\n" % allow_rel)
        for arr, have in findings:
            for a in arr[have:]:
                out.write("    %s\n" % allow_line(a, "<why this size is safe>"))
        out.write("\n")
    if stale:
        n = sum(len(ents) for ents, _ in stale)
        out.write("check-fixed-arrays: %d line%s in %s match%s no array any "
                  "more:\n\n" % (n, "" if n == 1 else "s", allow_rel,
                                 "es" if n == 1 else ""))
        for ents, _have in stale:
            for e in ents:
                out.write("  %s:%d: %s | %s | %s | %s | %s\n"
                          % (allow_rel, e[0], e[1], e[2], e[3], e[4], e[5]))
        out.write("\nRemove the line, or correct its file, function or struct, "
                  "name or size if\nthe array moved or changed.\n\n")


def run(root, allow_path, out=sys.stdout, quiet=False):
    findings, stale, errors, arrays, nfiles = check(root, allow_path)
    allow_rel = os.path.relpath(allow_path, root).replace(os.sep, "/")
    if allow_rel.startswith(".."):
        allow_rel = allow_path
    report(findings, stale, errors, allow_rel, out)
    if findings or stale or errors:
        return 1
    if not quiet:
        out.write("check-fixed-arrays: %d files, %d fixed arrays of %d bytes "
                  "or more or sized with MAX_PATH, each listed in %s\n"
                  % (nfiles, len(arrays), LIMIT, allow_rel))
    return 0


# ---------------------------------------------------------------- self-test

PLANT_A = r"""
#define BIGBUF 4096
#define SMALLBUF 64
enum { ENUM_SMALL = 10, ENUM_BIG = 300 };
typedef unsigned char octet;
struct holder {
    char name[300];
    int x;
    char small[32];
};
#define DECLARE_SCRATCH char macrobuf[512]
static char listed_global[1024];
void work(char param[4096], const char *text)
{
    char path[MAX_PATH];
    char big[BIGBUF];
    char fine[SMALLBUF];
    char *ptrs[400];
    char lines[5][256];
    octet raw[ENUM_BIG];
    wchar_t wide[MAX_PATH + 1];
    char twice[1024], twice2[1024];
    char init[] = "a string sized by its initialiser";
    /* char commented[4096]; */
    const char *s = "char quoted[4096];";
    (void)param; (void)text; (void)s;
}
"""
PLANT_B = r"""
void other(void)
{
    char buf[8192];
    char buf2[sizeof(buf)];
}
"""


def self_test():
    """The check on planted sources: what it counts, what passes, what
    fails. Exit 0 = every case as expected."""
    failures = []
    passed = [0]

    def ok(cond, what):
        if cond:
            passed[0] += 1
            print("ok    %s" % what)
        else:
            failures.append(what)
            print("FAIL  %s" % what)

    root = tempfile.mkdtemp(prefix="kitty_fixarr_selftest_")
    try:
        os.makedirs(os.path.join(root, "src"))
        os.makedirs(os.path.join(root, "unix"))
        os.makedirs(os.path.join(root, "build-x"))
        os.makedirs(os.path.join(root, "out", "cmk"))
        with open(os.path.join(root, "src", "a.c"), "w") as fh:
            fh.write(PLANT_A)
        with open(os.path.join(root, "src", "b.h"), "w") as fh:
            fh.write(PLANT_B)
        for skipped in ("unix/u.c", "build-x/g.c", "out/cmk/g.c"):
            with open(os.path.join(root, skipped), "w") as fh:
                fh.write("char skipped[4096];\n")
        with open(os.path.join(root, "out", "cmk", "CMakeCache.txt"), "w"):
            pass

        arrays, _ = scan(root)
        got = {(a["file"], a["ctx"], a["name"], norm_size(a["size"]))
               for a in arrays}
        want = {
            ("src/a.c", "work()", "path", "MAX_PATH"),
            ("src/a.c", "work()", "big", "BIGBUF"),
            ("src/a.c", "work()", "lines", "256"),
            ("src/a.c", "work()", "raw", "ENUM_BIG"),
            ("src/a.c", "work()", "wide", "MAX_PATH+1"),
            ("src/a.c", "work()", "twice", "1024"),
            ("src/a.c", "work()", "twice2", "1024"),
            ("src/a.c", "struct holder", "name", "300"),
            ("src/a.c", "#define DECLARE_SCRATCH", "macrobuf", "512"),
            ("src/a.c", "(file)", "listed_global", "1024"),
            ("src/b.h", "other()", "buf", "8192"),
            ("src/b.h", "other()", "buf2", "sizeof(buf)"),
        }
        ok(got == want, "counts exactly the planted large arrays")
        for extra in sorted(got - want):
            print("      counted, not planted as large: %s" % (extra,))
        for miss in sorted(want - got):
            print("      planted, not counted:          %s" % (miss,))
        ok(not any(a["name"] in ("fine", "small", "ptrs", "param", "init",
                                 "commented", "quoted", "skipped")
                   for a in arrays),
           "small, pointer, parameter, initialiser-sized, commented-out and "
           "quoted arrays are not counted; unix/ and build folders are not "
           "read")
        ok(any(a["name"] == "name" and a["ctx"] == "struct holder"
               for a in arrays), "(e) an array inside a struct is found")
        ok(any(a["name"] == "macrobuf" and a["ctx"].startswith("#define")
               for a in arrays), "(e) an array inside a #define is found")
        big = [a for a in arrays if a["name"] == "big"]
        ok(len(big) == 1 and big[0]["value"] == 4096,
           "a #define size is resolved (BIGBUF = 4096)")

        full = [
            "src/a.c | work() | path | MAX_PATH | test",
            "src/a.c | work() | big | BIGBUF | test",
            "src/a.c | work() | lines | 256 | test",
            "src/a.c | work() | raw | ENUM_BIG | test",
            "src/a.c | work() | wide | MAX_PATH + 1 | test (spaces differ)",
            "src/a.c | work() | twice | 1024 | test",
            "src/a.c | work() | twice2 | 1024 | test",
            "src/a.c | struct holder | name | 300 | test",
            "src/a.c | #define DECLARE_SCRATCH | macrobuf | 512 | test",
            "src/a.c | (file) | listed_global | 1024 | test",
            "src/b.h | other() | buf | 8192 | test",
            "src/b.h | other() | buf2 | sizeof(buf) | test",
        ]

        def verdict(lines, label):
            path = os.path.join(root, "allow_%s.txt" % label)
            with open(path, "w") as fh:
                fh.write("# planted list\n\n" + "\n".join(lines) + "\n")
            import io
            buf = io.StringIO()
            rc = run(root, path, out=buf, quiet=True)
            return rc, buf.getvalue()

        rc, text = verdict(full, "full")
        ok(rc == 0, "(c) every array listed: the check passes")
        if rc:
            print(text)

        rc, text = verdict([x for x in full if "| path |" not in x], "nopath")
        ok(rc == 1 and "path[MAX_PATH]" in text and
           "src/a.c | work() | path | MAX_PATH |" in text,
           "(a) a new unlisted MAX_PATH array fails, with the line to add")

        rc, text = verdict([x for x in full if "| big |" not in x], "nobig")
        ok(rc == 1 and "big[BIGBUF] (= 4096)" in text,
           "(b) a new unlisted 4096 array sized by a #define fails")

        rc, text = verdict(full + ["src/a.c | work() | gone | 2048 | test"],
                           "stale")
        ok(rc == 1 and "matches no array" in text and "| gone |" in text,
           "(d) a list line whose array is gone fails")

        rc, text = verdict([x for x in full if "| name |" not in x],
                           "nostruct")
        ok(rc == 1 and "name[300]" in text and "struct holder" in text,
           "(e) an unlisted struct member fails")

        rc, text = verdict([x for x in full if "| macrobuf |" not in x],
                           "nomacro")
        ok(rc == 1 and "macrobuf[512]" in text,
           "(e) an unlisted array inside a #define fails")

        moved = [x.replace("work() | path", "other() | path") for x in full]
        rc, text = verdict(moved, "moved")
        ok(rc == 1 and "path[MAX_PATH]" in text and "matches no array" in text,
           "a line naming another function matches nothing: both reported")

        resized = [x.replace("| path | MAX_PATH |", "| path | 512 |")
                   for x in full]
        rc, text = verdict(resized, "resized")
        ok(rc == 1 and "path[MAX_PATH]" in text,
           "a line with another size does not cover the array")

        rc, text = verdict([x for x in full if "| twice2 |" not in x] +
                           ["src/a.c | work() | twice | 1024 | test"],
                           "same-key")
        ok(rc == 1 and "twice2[1024]" in text,
           "two arrays need two lines: one line per array")

        rc, text = verdict(full + ["src/a.c | work() | path | MAX_PATH | "
                                   "a second line"], "twolines")
        ok(rc == 1 and "matches no array" in text,
           "a second line for one array is stale")

        rc, text = verdict(full + ["src/a.c | work() | path"], "badline")
        ok(rc == 1 and "not \"file | function or struct" in text,
           "a malformed line fails")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("\ncheck-fixed-arrays self-test: %d checks, %d failed"
          % (passed[0] + len(failures), len(failures)))
    return 1 if failures else 0


# ---------------------------------------------------------------- main

def main(argv):
    root = DEFAULT_ROOT
    allow = None
    mode = "check"
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--self-test":
            mode = "self-test"
        elif a == "--list":
            mode = "list"
        elif a in ("--root", "--allow") and i + 1 < len(argv):
            if a == "--root":
                root = os.path.abspath(argv[i + 1])
            else:
                allow = os.path.abspath(argv[i + 1])
            i += 1
        else:
            sys.stderr.write(__doc__)
            return 2
        i += 1
    if allow is None:
        allow = os.path.join(root, "tools", "fixed-arrays.allow")
    if mode == "self-test":
        return self_test()
    if mode == "list":
        arrays, _ = scan(root)
        entries, _ = read_allow(allow)
        reasons = {}
        for e in entries:
            reasons.setdefault(key_of(e[1], e[2], e[3], e[4]), []).append(e[5])
        for a in arrays:
            k = key_of(a["file"], a["ctx"], a["name"], a["size"])
            have = reasons.get(k)
            print("%s\t%d" % (allow_line(a, have.pop(0) if have else "-"),
                              a["line"]))
        return 0
    return run(root, allow)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
