#!/usr/bin/env python3
"""The programs' own settings and state reach the registry and kitty.ini
through the settings module only (kitty/kitty_globalsettings.c).

What fails the build:
1. A call of a registry function (RegOpenKey, RegCreateKey, RegQueryValue,
   RegSetValue, RegDeleteValue, RegEnumKey, RegEnumValue, RegGetValue,
   RegDeleteKey, RegDeleteTree, RegQueryInfoKey, RegCopyTree, RegSetKeyValue,
   RegDeleteKeyValue, RegRenameKey, each with Ex, A and W) or of an .ini
   function (GetPrivateProfileString/Int/Section/SectionNames/Struct,
   WritePrivateProfileString/Section/Struct, A and W) in a function not
   listed in tools/raw-registry.allow. The settings module itself is the
   place for them and is not checked.
2. A listed line that matches no call (the list cannot go stale).

tools/raw-registry.allow, one line per function that keeps its own calls:
    file | function | reason
    file | * | reason            every function of the file
The reasons say which store it is: the session store and its tree code,
Windows' own keys, the Windows XP stand-ins, a one-time migration, the
store move's whole sections of kitty.ini.

What counts: every .c file of the source tree except test/, windows/test/,
unix/, contrib/, .git and build directories (a top-level folder whose name
starts with "build", or any folder holding a CMakeCache.txt). Comments,
string and character literals are ignored. A function is the last name
before "(" in the text that opens a top-level brace.

Run:  python3 tools/check-raw-registry.py               (exit 1 on a finding)
      python3 tools/check-raw-registry.py --self-test   (the check on planted
                                                          sources)
Options: --root DIR (source tree), --allow FILE, --list (every call found
as "file | function", allowed or not).
"""
import os
import re
import sys
import shutil
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)
DEFAULT_ALLOW = os.path.join(HERE, "raw-registry.allow")
MODULE = "kitty/kitty_globalsettings.c"
SKIP_TOP = ("test", "unix", "contrib", ".git")
CALL = re.compile(
    r"\bReg(OpenKey|CreateKey|QueryValue|SetValue|DeleteValue|EnumKey|"
    r"EnumValue|GetValue|DeleteKey|DeleteTree|QueryInfoKey|CopyTree|"
    r"SetKeyValue|DeleteKeyValue|RenameKey)(Ex)?[AW]?\s*\(|"
    r"\b(GetPrivateProfile(String|Int|Section|SectionNames|Struct)|"
    r"WritePrivateProfile(String|Section|Struct))[AW]?\s*\(")
NAME_BEFORE_PAREN = re.compile(r"([A-Za-z_]\w*)\s*\(")


def strip(src):
    """Comments, string and character literals as blanks (newlines kept)."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", src[i + 1:j - 1]) + c)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def no_preprocessor(text):
    """Preprocessor lines (with their continuations) as blanks."""
    lines = text.split("\n")
    out, cont = [], False
    for l in lines:
        if cont or l.lstrip().startswith("#"):
            cont = l.rstrip().endswith("\\")
            out.append(" " * len(l))
        else:
            cont = False
            out.append(l)
    return "\n".join(out)


def calls_in(text):
    """(function, line) of every registry call in a stripped source."""
    found = []
    depth = 0
    func = None
    last_top = 0          # where the text of the next top-level item starts
    for m in re.finditer(r"[{};]|" + CALL.pattern, text):
        tok = m.group(0)
        if tok == "{":
            if depth == 0:
                head = text[last_top:m.start()]
                names = NAME_BEFORE_PAREN.findall(head)
                func = names[0] if names else None
                # "static int foo( ... )": the name before the parameters is
                # the first "name(" of the header (a cast or macro inside the
                # parameter list comes later)
            depth += 1
        elif tok == "}":
            depth = max(depth - 1, 0)
            if depth == 0:
                func = None
                last_top = m.end()
        elif tok == ";":
            if depth == 0:
                last_top = m.end()
        else:
            line = text.count("\n", 0, m.start()) + 1
            found.append((func or "<file scope>", line))
    return found


def sources(root):
    for top, dirs, files in os.walk(root):
        rel = os.path.relpath(top, root).replace(os.sep, "/")
        parts = [] if rel == "." else rel.split("/")
        if parts and (parts[0] in SKIP_TOP or parts[0].startswith("build")):
            dirs[:] = []
            continue
        if "CMakeCache.txt" in files or rel == "windows/test":
            dirs[:] = []
            continue
        for f in files:
            if f.endswith(".c"):
                yield ("/".join(parts + [f]) if parts else f), os.path.join(top, f)


def scan(root):
    out = []
    for rel, path in sorted(sources(root)):
        if rel == MODULE:
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = no_preprocessor(strip(fh.read()))
        for func, line in calls_in(text):
            out.append((rel, func, line))
    return out


def read_allow(path):
    allow = []
    with open(path, encoding="utf-8") as fh:
        for no, raw in enumerate(fh, 1):
            l = raw.strip()
            if not l or l.startswith("#"):
                continue
            parts = [p.strip() for p in l.split("|")]
            if len(parts) < 3 or not parts[2]:
                raise SystemExit(f"{path}:{no}: want 'file | function | reason': {l}")
            allow.append((parts[0], parts[1], no, l))
    return allow


def check(root, allow_path, quiet=False):
    allow = read_allow(allow_path)
    used = set()
    bad = []
    for rel, func, line in scan(root):
        hit = None
        for i, (f, fn, _, _) in enumerate(allow):
            if f == rel and (fn == "*" or fn == func):
                hit = i
                break
        if hit is None:
            bad.append(f"  {rel}:{line}: a registry or kitty.ini call in {func}() outside "
                       f"the settings module, not in {os.path.basename(allow_path)}")
        else:
            used.add(hit)
    for i, (f, fn, no, l) in enumerate(allow):
        if i not in used:
            bad.append(f"  {os.path.basename(allow_path)}:{no}: matches no call: {l}")
    if bad:
        if not quiet:
            print("raw registry check FAILED: the programs' settings go through "
                  "kitty/kitty_globalsettings.c")
            print("\n".join(bad))
        return 1
    if not quiet:
        print(f"raw registry check: OK ({len(allow)} listed functions)")
    return 0


def self_test():
    tmp = tempfile.mkdtemp(prefix="rawreg_")
    try:
        os.makedirs(os.path.join(tmp, "kitty"))
        os.makedirs(os.path.join(tmp, "test"))

        def w(rel, s):
            with open(os.path.join(tmp, rel), "w", encoding="utf-8") as fh:
                fh.write(s)
        w(MODULE, "int m(void)\n{\n  RegOpenKeyExA(0,0,0,0,0);\n  return 0;\n}\n")
        w("kitty/a.c", "/* RegOpenKeyEx( in a comment */\n"
                       "static const char *s = \"RegSetValueEx(\";\n"
                       "#define X RegQueryValueExA(\n"
                       "static int keep( int x ) {\n  if (x) { RegDeleteValueA(0, \"v\"); }\n  return 0;\n}\n"
                       "int other( void )\n{\n  return RegGetValueA(0,0,0,0,0,0,0);\n}\n")
        w("test/t.c", "int t(void)\n{\n  RegOpenKeyExA(0,0,0,0,0);\n  return 0;\n}\n")
        allow = os.path.join(tmp, "allow")
        cases = [
            ("kitty/a.c | keep | r\nkitty/a.c | other | r\n", 0),
            ("kitty/a.c | keep | r\n", 1),                        # other() unlisted
            ("kitty/a.c | * | r\n", 0),
            ("kitty/a.c | * | r\nkitty/b.c | f | r\n", 1),       # stale line
        ]
        for text, want in cases:
            with open(allow, "w", encoding="utf-8") as fh:
                fh.write(text)
            got = check(tmp, allow, quiet=True)
            if got != want:
                print(f"self-test FAILED: allow {text!r}: exit {got}, want {want}")
                return 1
        found = scan(tmp)
        if sorted((f, fn) for f, fn, _ in found) != [("kitty/a.c", "keep"), ("kitty/a.c", "other")]:
            print(f"self-test FAILED: found {found}")
            return 1
        print("raw registry check self-test: OK")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv):
    root, allow = DEFAULT_ROOT, DEFAULT_ALLOW
    if "--self-test" in argv:
        return self_test()
    if "--root" in argv:
        root = argv[argv.index("--root") + 1]
    if "--allow" in argv:
        allow = argv[argv.index("--allow") + 1]
    if "--list" in argv:
        seen = []
        for rel, func, line in scan(root):
            if (rel, func) not in seen:
                seen.append((rel, func))
                print(f"{rel} | {func}")
        return 0
    return check(root, allow)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
