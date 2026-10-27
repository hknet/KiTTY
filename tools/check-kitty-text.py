#!/usr/bin/env python3
"""The UI texts of kitty/kitty_text.h stay used and in house style,
and new UI text goes there, not into the code.

Four kinds of finding (no duplicate check: two names with the same English
text are allowed, a translation may need different words for them - the
user's decision 2026-10-27):

  unused     a KT_ define of kitty/kitty_text.h that no .c, .h or .rc file
             of the tree uses (a use inside another KT_ define counts;
             comments and string contents do not).
  word       a text of kitty_text.h that uses say/ask/tell or names/named as
             a verb: every whole-word hit, any case, of say says said saying
             ask asks asked asking tell tells told telling names named naming
             ("name" alone is left out: it is a noun too often). Menu
             accelerators are ignored (As&k matches), \\n \\r \\t separate
             words.
  product    a text of kitty_text.h with a bare "KiTTY" not followed by
             "++" (case-sensitive, a whole word: KiTTYgen is another
             name). The product is KiTTY++; a text that means the classic
             program is listed with that reason.
  hardcoded  a string literal handed directly to MessageBox*,
             SetWindowText*, SetDlgItemText*, AppendMenu*, InsertMenu* or a
             dialog.h control constructor (ctrl_settitle, ctrl_getset - not
             its internal set name -, ctrl_editbox, ctrl_combobox,
             ctrl_editbox_multiline, ctrl_radiobuttons(_fn),
             ctrl_pushbutton, ctrl_listbox, ctrl_droplist, ctrl_draglist,
             ctrl_filesel, ctrl_fontsel, ctrl_text, ctrl_checkbox) in a
             kitty/*.c file. TEXT("..") / _T("..") / L".." count as direct;
             a literal inside another call (dupprintf("..")) does not. A
             literal with no letter in it is not UI text. Adjacent literals
             in one argument are one text.

Every finding must be listed in tools/kitty-text.allow with a reason, or the
check fails. A listed line that matches no finding fails too, so the list
cannot go stale: a text that was reworded, a define that got used, a string
that moved into kitty_text.h - its line goes.

tools/kitty-text.allow, one line per finding:
    category | subject | detail | reason
  unused     | KT_NAME                 | -          | reason
  word       | KT_NAME                 | the word, lower case | reason
  product    | KT_NAME                 | KiTTY      | reason
  hardcoded  | kitty/file.c            | Call "literal as written" | reason
A "|" inside a field is written "\\|". Lines match by the first three
fields, never by line number; a hard-coded text that occurs twice in one
file with the same call needs two lines.

Run:  python3 tools/check-kitty-text.py               (exit 1 on a finding)
      python3 tools/check-kitty-text.py --self-test   (the check on planted
                                                        sources)
      python3 tools/check-kitty-text.py --list        (every finding as an
                                                        allow line)
Options: --root DIR (source tree), --allow FILE.
"""
import io
import os
import re
import shutil
import sys
import tempfile
from collections import Counter, OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)
TEXT_H = "kitty/kitty_text.h"

WORDS = ("say", "says", "said", "saying", "ask", "asks", "asked", "asking",
         "tell", "tells", "told", "telling", "names", "named", "naming")
WORD_RE = re.compile(r"(?<![A-Za-z0-9_])(" + "|".join(WORDS) +
                     r")(?![A-Za-z0-9_])", re.I)
PRODUCT_RE = re.compile(r"(?<![A-Za-z0-9_])KiTTY(?![A-Za-z0-9_]|\+\+)")

WIN32_CALL_RE = re.compile(
    r"\b(MessageBox\w*|SetWindowText\w*|SetDlgItemText\w*|AppendMenu\w*|"
    r"InsertMenu\w*|ctrl_settitle|ctrl_getset|ctrl_editbox_multiline|"
    r"ctrl_editbox|ctrl_combobox|ctrl_radiobuttons_fn|ctrl_radiobuttons|"
    r"ctrl_pushbutton|ctrl_listbox|ctrl_droplist|ctrl_draglist|ctrl_filesel|"
    r"ctrl_fontsel|ctrl_text|ctrl_checkbox)\s*\(")
# argument positions (0-based) that are not UI text
SKIP_ARGS = {"ctrl_getset": {2}}
WRAPPERS = ("TEXT", "_T", "_TEXT", "__TEXT")

CATEGORIES = ("unused", "word", "product", "hardcoded")


# ---------------------------------------------------------------- sources

def tree_files(root, exts):
    """Every file with one of exts below root, '/'-separated, sorted. Not
    .git, not a top-level build* folder, not a folder with CMakeCache.txt."""
    out = []
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root)
        if rel == ".":
            dirs[:] = [x for x in dirs if not x.startswith("build")]
        dirs[:] = [x for x in dirs if x != ".git" and not os.path.exists(
            os.path.join(d, x, "CMakeCache.txt"))]
        for f in files:
            if f.endswith(exts):
                p = os.path.normpath(os.path.join(rel, f))
                out.append(p.replace(os.sep, "/"))
    return sorted(out)


def read(path):
    with open(path, encoding="utf-8", errors="replace", newline="") as fh:
        return fh.read().replace("\r\n", "\n")


def strip(src, keep_strings):
    """Comments blanked, line structure kept. keep_strings=False blanks the
    inside of string and character literals too."""
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
            if keep_strings:
                out.append(body)
            else:
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
        while line.rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            line = line.rstrip()[:-1] + " " + lines[i]
        yield start + 1, line
        i += 1


BODY_TOK = re.compile(r'L?"(?:[^"\\\n]|\\.)*"|[A-Za-z_]\w*(?:\s*\([^()]*\))?'
                      r'|\S')
DEFINE_RE = re.compile(r"\s*#\s*define\s+(KT_\w+)(\([^)]*\))?\s*(.*)$")


def text_defines(root):
    """[(line, name, text or None, words-only text)] of kitty_text.h.
    text: the literals joined, other tokens kept as written in <..>; None
    for a body without a string literal (an alias, a number)."""
    out = []
    path = os.path.join(root, TEXT_H)
    if not os.path.exists(path):
        return out
    s = strip(read(path), keep_strings=True)
    for ln, line in logical_lines(s):
        m = DEFINE_RE.match(line)
        if not m:
            continue
        body = m.group(3).strip()
        toks = BODY_TOK.findall(body)
        if not any(t.startswith(('"', 'L"')) for t in toks):
            out.append((ln, m.group(1), None, ""))
            continue
        parts = []
        lits = []
        for t in toks:
            if t.startswith(('"', 'L"')):
                c = t[t.index('"') + 1:-1]
                parts.append(c)
                lits.append(c)
            else:
                parts.append("<%s>" % re.sub(r"\s+", "", t))
        out.append((ln, m.group(1), "".join(parts), " ".join(lits)))
    return out


def words_text(lits):
    """The literal contents as words: escapes as spaces, accelerators gone."""
    t = re.sub(r"\\[nrt]", " ", lits)
    t = t.replace("&&", "\0").replace("&", "").replace("\0", "&")
    return t


# ---------------------------------------------------------------- the rules

def find_unused(root, defs):
    names = [d[1] for d in defs]
    if not names:
        return []
    uses = Counter()
    for f in tree_files(root, (".c", ".h", ".rc")):
        try:
            s = strip(read(os.path.join(root, f)), keep_strings=False)
        except OSError:
            continue
        uses.update(re.findall(r"\bKT_\w+", s))
    heads = Counter(names)
    out = []
    seen = set()
    for ln, name, _t, _w in defs:
        if name in seen:
            continue
        seen.add(name)
        if uses[name] - heads[name] <= 0:
            out.append({"cat": "unused", "subject": name, "detail": "-",
                        "where": "%s:%d" % (TEXT_H, ln),
                        "msg": "%s is used nowhere" % name})
    return out


def find_words(defs):
    out = []
    for ln, name, text, lits in defs:
        if text is None:
            continue
        w = words_text(lits)
        hit = []
        for m in WORD_RE.finditer(w):
            k = m.group(1).lower()
            if k not in hit:
                hit.append(k)
        for k in hit:
            out.append({"cat": "word", "subject": name, "detail": k,
                        "where": "%s:%d" % (TEXT_H, ln),
                        "msg": "%s uses \"%s\": \"%s\"" % (name, k, text)})
        if PRODUCT_RE.search(lits):
            out.append({"cat": "product", "subject": name, "detail": "KiTTY",
                        "where": "%s:%d" % (TEXT_H, ln),
                        "msg": "%s has a bare KiTTY: \"%s\"" % (name, text)})
    return out


LIT_RE = re.compile(r'(?:L|u8|u|U)?"(?:[^"\\\n]|\\.)*"')


def split_args(code, open_pos):
    """The argument texts of the call whose "(" is at open_pos, split at the
    top-level commas; (args, end position)."""
    depth = 0
    args = []
    cur = []
    i = open_pos
    n = len(code)
    while i < n:
        c = code[i]
        if c == '"':
            m = LIT_RE.match(code, i)
            if m:
                cur.append(m.group(0))
                i = m.end()
                continue
        if c == "'":
            j = i + 1
            while j < n and code[j] != "'":
                j += 2 if code[j] == "\\" else 1
            cur.append(code[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
            if depth == 1:
                i += 1
                continue
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(cur))
                return args, i
        elif c == "," and depth == 1:
            args.append("".join(cur))
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    return args, n


def direct_literals(arg):
    """The string texts handed directly in one argument: [raw text]."""
    unwrapped = re.sub(r"\b(?:%s)\s*\(\s*((?:L)?\"(?:[^\"\\\n]|\\.)*\")\s*\)"
                       % "|".join(WRAPPERS), r"\1",
                       re.sub(r"'(?:[^'\\\n]|\\.)+'", "0", arg))
    # blank everything inside nested parentheses: not passed directly
    flat = []
    depth = 0
    i = 0
    while i < len(unwrapped):
        c = unwrapped[i]
        if c == '"' or (c in "LuU" and unwrapped[i + 1:i + 2] == '"'):
            m = LIT_RE.match(unwrapped, i)
            if m:
                flat.append(m.group(0) if depth == 0 else " ")
                i = m.end()
                continue
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif depth == 0:
            flat.append(c)
        i += 1
    flat = "".join(flat)
    lits = list(LIT_RE.finditer(flat))
    if not lits:
        return []
    rest = LIT_RE.sub("", flat)
    if not rest.strip():
        joined = "".join(m.group(0)[m.group(0).index('"') + 1:-1]
                         for m in lits)
        return [joined]
    return [m.group(0)[m.group(0).index('"') + 1:-1] for m in lits]


def find_hardcoded(root):
    out = []
    kdir = os.path.join(root, "kitty")
    if not os.path.isdir(kdir):
        return out
    for f in sorted(os.listdir(kdir)):
        if not f.endswith(".c"):
            continue
        rel = "kitty/" + f
        code = strip(read(os.path.join(kdir, f)), keep_strings=True)
        starts = [m.start() for m in re.finditer(r"^", code, re.M)]
        import bisect
        for m in WIN32_CALL_RE.finditer(code):
            call = m.group(1)
            # a declaration or definition has no literal arguments; a
            # preprocessor line (#define ctrl_x ...) is not a call
            line_start = code.rfind("\n", 0, m.start()) + 1
            if code[line_start:m.start()].lstrip().startswith("#"):
                continue
            args, _end = split_args(code, m.end() - 1)
            for idx, a in enumerate(args):
                if idx in SKIP_ARGS.get(call, ()):
                    continue
                for lit in direct_literals(a):
                    bare = re.sub(r"%[-+ #0-9.*]*(?:hh|h|ll|l|L|z|j|t|I64|"
                                  r"I32|I)?[a-zA-Z%]|\\(?:x[0-9a-fA-F]+|.)",
                                  "", lit)
                    if not re.search(r"[A-Za-z]", bare):
                        continue
                    ln = bisect.bisect_right(starts, m.start())
                    out.append({"cat": "hardcoded", "subject": rel,
                                "detail": '%s "%s"' % (call, lit),
                                "where": "%s:%d" % (rel, ln),
                                "msg": '%s "%s"' % (call, lit)})
    return out


def scan(root):
    defs = text_defines(root)
    return (find_unused(root, defs) +
            find_words(defs) + find_hardcoded(root)), defs


# ---------------------------------------------------------------- the list

def esc(s):
    return s.replace("|", "\\|")


def split_fields(line):
    parts = re.split(r"(?<!\\)\|", line, maxsplit=3)
    return [p.strip().replace("\\|", "|") for p in parts]


def read_allow(path):
    entries = []
    errors = []
    try:
        text = read(path)
    except OSError as e:
        return entries, ["%s: cannot read it: %s" % (path, e.strerror)]
    for no, raw in enumerate(text.split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = split_fields(line)
        if len(parts) != 4 or not all(parts) or parts[0] not in CATEGORIES:
            errors.append("%s:%d: not \"category | subject | detail | "
                          "reason\": %s" % (path, no, line))
            continue
        entries.append((no, parts[0], parts[1], parts[2], parts[3]))
    return entries, errors


def key_of(cat, subject, detail):
    return (cat, " ".join(subject.split()), detail)


def allow_line(f, reason):
    return "%s | %s | %s | %s" % (f["cat"], esc(f["subject"]),
                                  esc(f["detail"]), reason)


def check(root, allow_path):
    findings, defs = scan(root)
    entries, errors = read_allow(allow_path)
    found = OrderedDict()
    for f in findings:
        found.setdefault(key_of(f["cat"], f["subject"], f["detail"]),
                         []).append(f)
    listed = OrderedDict()
    for e in entries:
        listed.setdefault(key_of(e[1], e[2], e[3]), []).append(e)
    new = []
    stale = []
    for k, fs in found.items():
        have = len(listed.get(k, []))
        new.extend(fs[have:])
    for k, es in listed.items():
        have = len(found.get(k, []))
        stale.extend(es[have:])
    return new, stale, errors, findings, defs


HINT = {
    "unused": "Remove the define, or use it.",
    "word": "Reword without say/ask/tell/names - the wording needs the "
            "user's OK first.",
    "product": "Write KiTTY++, or list it if it means classic KiTTY.",
    "hardcoded": "Put the text in kitty/kitty_text.h as a KT_ define and "
                 "use that.",
}


def run(root, allow_path, out=sys.stdout, quiet=False):
    new, stale, errors, findings, defs = check(root, allow_path)
    allow_rel = os.path.relpath(allow_path, root).replace(os.sep, "/")
    if allow_rel.startswith(".."):
        allow_rel = allow_path
    for e in errors:
        out.write("check-kitty-text: %s\n" % e)
    for f in new:
        out.write("%s: %s: %s\n" % (f["where"], f["cat"], f["msg"]))
    for e in stale:
        out.write("%s:%d: stale: %s | %s | %s matches no finding any more - "
                  "remove the line\n" % (allow_rel, e[0], e[1], e[2], e[3]))
    if new:
        out.write("\nAdd a line to %s only with a reason; otherwise fix it:\n"
                  % allow_rel)
        for cat in CATEGORIES:
            if any(f["cat"] == cat for f in new):
                out.write("  %s: %s\n" % (cat, HINT[cat]))
        for f in new:
            out.write("    %s\n" % allow_line(f, "<reason>"))
    counts = Counter(f["cat"] for f in findings)
    summary = ", ".join("%d %s" % (counts[c], c) for c in CATEGORIES)
    if new or stale or errors:
        out.write("check-kitty-text: FAILED - %d new finding%s, %d stale "
                  "line%s, %d bad line%s (all findings: %s)\n"
                  % (len(new), "" if len(new) == 1 else "s",
                     len(stale), "" if len(stale) == 1 else "s",
                     len(errors), "" if len(errors) == 1 else "s", summary))
        return 1
    if not quiet:
        out.write("check-kitty-text: %d KT_ defines, %d findings, each listed "
                  "in %s (%s)\n" % (len(defs), len(findings), allow_rel,
                                    summary))
    return 0


# ---------------------------------------------------------------- self-test

PLANT_TEXT = r'''/* header: KiTTY in a comment is not a text */
#define KT_USED_C     "Used in a.c"
#define KT_USED_RC    "Used in a.rc"
#define KT_USED_INNER "inner"
#define KT_OUTER      KT_USED_INNER
#define KT_UNUSED     "Never used"
#define KT_ONLY_COMMENT "Named in a comment only"
#define KT_DUP_A      "Same " \
                      "text"
#define KT_DUP_B      "Same text"
#define KT_WORD       "The host as&ks first\nand said so"
#define KT_NOUN_OK    "Session name"
#define KT_PROD_BARE  "Old KiTTY sessions"
#define KT_PROD_OK    "KiTTY++ settings, KiTTYgen"
#define KT_LIMIT      "Up to " KITTY_STR(KITTY_MAX) " chars"
'''
PLANT_C = r'''
#include "kitty_text.h"
void f(HWND h, struct controlset *s, struct controlbox *b)
{
    const char *x = "KT_UNUSED";       /* KT_ONLY_COMMENT */
    MessageBox(h, "Hard text", KT_USED_C, MB_OK);
    MessageBoxW(h, L"Wide | text", TEXT("Cap"), MB_OK);
    SetWindowText(h, dupprintf("%s nested", x));
    SetDlgItemText(h, 5, "%d");
    AppendMenu(m, MF_STRING, 1, "Two " "parts");
    s = ctrl_getset(b, "Panel/Path", "internalname", "Box title");
    ctrl_checkbox(s, KT_OUTER, 'x', HELPCTX(no_help), h, I(0));
    /* MessageBox(h, "commented out", "x", 0); */
    MessageBox(h, "Hard text", KT_USED_C, MB_OK);
}
'''
PLANT_RC = 'CAPTION KT_USED_RC\n'


def self_test():
    failures = []
    passed = [0]

    def ok(cond, what):
        if cond:
            passed[0] += 1
            print("ok    %s" % what)
        else:
            failures.append(what)
            print("FAIL  %s" % what)

    root = tempfile.mkdtemp(prefix="kitty_ktext_selftest_")
    try:
        os.makedirs(os.path.join(root, "kitty"))
        os.makedirs(os.path.join(root, "windows"))
        os.makedirs(os.path.join(root, "build-x", "kitty"))
        with open(os.path.join(root, TEXT_H), "w") as fh:
            fh.write(PLANT_TEXT)
        with open(os.path.join(root, "kitty", "a.c"), "w") as fh:
            fh.write(PLANT_C)
        with open(os.path.join(root, "windows", "a.rc"), "w") as fh:
            fh.write(PLANT_RC)
        with open(os.path.join(root, "build-x", "kitty", "copy.c"), "w") as fh:
            fh.write("int a = KT_UNUSED;\n")

        findings, _defs = scan(root)
        got = Counter((f["cat"], f["subject"], f["detail"]) for f in findings)
        want = Counter([
            ("unused", "KT_UNUSED", "-"),
            ("unused", "KT_ONLY_COMMENT", "-"),
            ("unused", "KT_DUP_A", "-"),
            ("unused", "KT_DUP_B", "-"),
            ("unused", "KT_WORD", "-"),
            ("unused", "KT_NOUN_OK", "-"),
            ("unused", "KT_PROD_BARE", "-"),
            ("unused", "KT_PROD_OK", "-"),
            ("unused", "KT_LIMIT", "-"),
            ("word", "KT_ONLY_COMMENT", "named"),
            ("word", "KT_WORD", "asks"),
            ("word", "KT_WORD", "said"),
            ("product", "KT_PROD_BARE", "KiTTY"),
            ("hardcoded", "kitty/a.c", 'MessageBox "Hard text"'),
            ("hardcoded", "kitty/a.c", 'MessageBox "Hard text"'),
            ("hardcoded", "kitty/a.c", 'MessageBoxW "Wide | text"'),
            ("hardcoded", "kitty/a.c", 'MessageBoxW "Cap"'),
            ("hardcoded", "kitty/a.c", 'AppendMenu "Two parts"'),
            ("hardcoded", "kitty/a.c", 'ctrl_getset "Panel/Path"'),
            ("hardcoded", "kitty/a.c", 'ctrl_getset "Box title"'),
        ])
        ok(got == want, "finds exactly the planted findings")
        for extra in sorted((got - want).elements()):
            print("      found, not planted: %s" % (extra,))
        for miss in sorted((want - got).elements()):
            print("      planted, not found: %s" % (miss,))
        ok(not any(f["subject"] in ("KT_USED_C", "KT_USED_RC",
                                    "KT_USED_INNER", "KT_OUTER")
                   for f in findings if f["cat"] == "unused"),
           "uses in .c, .rc and inside another KT_ define count; a build "
           "folder copy does not")

        full = [allow_line(f, "test") for f in findings]

        def verdict(lines, label):
            path = os.path.join(root, "allow_%s.txt" % label)
            with open(path, "w") as fh:
                fh.write("# planted list\n\n" + "\n".join(lines) + "\n")
            buf = io.StringIO()
            rc = run(root, path, out=buf, quiet=True)
            return rc, buf.getvalue()

        rc, text = verdict(full, "full")
        ok(rc == 0, "every finding listed (a \\| inside a field included): "
                    "the check passes")
        if rc:
            print(text)
        for cat in CATEGORIES:
            rc, text = verdict([x for x in full
                                if not x.startswith(cat + " |")], cat)
            ok(rc == 1 and (": %s: " % cat) in text,
               "an unlisted %s finding fails" % cat)
        rc, text = verdict(full + ["word | KT_GONE | asks | test"], "stale")
        ok(rc == 1 and "stale" in text, "a line matching nothing fails")
        one = list(full)
        one.remove('hardcoded | kitty/a.c | MessageBox "Hard text" | test')
        rc, text = verdict(one, "twice")
        ok(rc == 1, "the same hard-coded text twice needs two lines")
        rc, text = verdict(full + ["word | KT_WORD"], "bad")
        ok(rc == 1 and "not \"category" in text, "a malformed line fails")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("\ncheck-kitty-text self-test: %d checks, %d failed"
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
        allow = os.path.join(root, "tools", "kitty-text.allow")
    if mode == "self-test":
        return self_test()
    if mode == "list":
        findings, _defs = scan(root)
        for f in findings:
            print("%s\t%s\t%s" % (allow_line(f, "-"), f["where"], f["msg"]))
        return 0
    return run(root, allow)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
