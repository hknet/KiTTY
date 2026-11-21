#!/usr/bin/env python3
"""The settings files and the store's decisions stay with the settings module.

tools/check-raw-registry.py keeps the registry and the profile calls
(Get/WritePrivateProfile*) in kitty/kitty_globalsettings.c. This check keeps
three more things there, each failing the build outside the module when the
function is not listed in tools/settings-access.allow:

FILE  a file call - fopen, _wfopen, _fsopen, CreateFile, kitty_write_file_whole,
      DeleteFile, MoveFile(Ex), rename, unlink, _unlink, remove - whose
      arguments name a settings file: KittyIniFile, GetKittyIniFile(),
      kitty_inifile_file(), "kitty.ini", KiTTYState, kitty_windowpos.ini,
      kitty_inifile_state_file_dup. A writer past the module would pass by
      readonly=yes and conf=no.
INI   kitty.exe's kitty.ini wrappers (readINI, writeINI, delINI,
      kitty_read_ini_dup) on a section of the global settings ([KiTTY],
      [ConfigBox], [Launcher], [Print], [FontFallback]). They read kitty.ini
      in every save mode, past the store in charge (ReadParameter /
      WriteParameter): how the kitty.ini-only settings came about.
FLAG  a call of a function that decides what may be written or what holds:
      GetReadOnlyFlag, kitty_store_readonly, kitty_inifile_readonly,
      kitty_inifile_noconf(_blocks), kitty_inifile_write_refusal,
      kitty_inifile_write_blocked, kitty_store_ro_by_policy,
      kitty_settings_policy_source(_in), kitty_settings_policy_text_dup,
      kitty_settings_hardening_dup, kitty_settings_recommended_text_dup. The
      writers refuse on their own; a decision taken again elsewhere drifts
      (conf=no had 22 of them, 2026-11-21).

The module (kitty/kitty_globalsettings.c, kitty/kitty_inifile.c,
kitty/mini/mini.c) is not checked. A listed line that matches nothing fails
(the list cannot go stale).

tools/settings-access.allow, one line per function that keeps its calls:
    KIND | file | function | reason
    KIND | file | *        | reason        every function of the file

Run:  python3 tools/check-settings-access.py               (exit 1 on a finding)
      python3 tools/check-settings-access.py --self-test   (planted sources)
      python3 tools/check-settings-access.py --list        (every call found)
"""
import os
import re
import sys
import shutil
import tempfile
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)
DEFAULT_ALLOW = os.path.join(HERE, "settings-access.allow")
MODULES = ("kitty/kitty_globalsettings.c", "kitty/kitty_inifile.c",
           "kitty/mini/mini.c")

_spec = importlib.util.spec_from_file_location(
    "crr", os.path.join(HERE, "check-raw-registry.py"))
crr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(crr)

FILE_CALL = re.compile(
    r"\b(fopen|_wfopen|_fsopen|CreateFile[AW]?|kitty_write_file_whole|"
    r"DeleteFile[AW]?|MoveFile(Ex)?[AW]?|rename|unlink|_unlink|remove)\s*\(")
FILE_TOK = re.compile(
    r"KittyIniFile|GetKittyIniFile\s*\(|kitty_inifile_file\s*\(|"
    r"\"kitty\.ini\"|KiTTYState|kitty_windowpos\.ini|"
    r"kitty_inifile_state_file_dup")
INI_CALL = re.compile(r"\b(readINI|writeINI|delINI|kitty_read_ini_dup)\s*\(")
GLOBAL_SEC = re.compile(
    r"^\s*(INIT_SECTION|KI_SECTION_KITTY|KI_SECTION_CONFIGBOX|"
    r"KI_SECTION_LAUNCHER|KI_SECTION_PRINT|KI_SECTION_FONTFALLBACK|"
    r"\"KiTTY\"|\"ConfigBox\"|\"Launcher\"|\"Print\"|\"FontFallback\")\s*$")
FLAG_CALL = re.compile(
    r"\b(GetReadOnlyFlag|kitty_store_readonly|kitty_inifile_readonly|"
    r"kitty_inifile_noconf_blocks|kitty_inifile_noconf|"
    r"kitty_inifile_write_refusal|kitty_inifile_write_blocked|"
    r"kitty_store_ro_by_policy|kitty_settings_policy_source_in|"
    r"kitty_settings_policy_source|kitty_settings_policy_text_dup|"
    r"kitty_settings_hardening_dup|kitty_settings_recommended_text_dup)\s*\(")
NAME_BEFORE_PAREN = re.compile(r"([A-Za-z_]\w*)\s*\(")
KINDS = ("FILE", "INI", "FLAG")


def args_of(raw, open_paren):
    """The raw text between the call's parentheses, with its top-level args."""
    depth, i, n = 0, open_paren, len(raw)
    start = open_paren + 1
    parts, cur = [], start
    while i < n:
        c = raw[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                parts.append(raw[cur:i])
                return raw[start:i], parts
        elif c == "," and depth == 1:
            parts.append(raw[cur:i])
            cur = i + 1
        elif c in "\"'":
            j = i + 1
            while j < n and raw[j] != c and raw[j] != "\n":
                j += 2 if raw[j] == "\\" else 1
            i = j
        i += 1
    return raw[start:], parts


def findings_in(raw):
    """[(kind, function, line)] in one file's source."""
    text = crr.no_preprocessor(crr.strip(raw))
    out = []
    depth, func, last_top = 0, None, 0
    pat = re.compile(r"[{};]|" + FILE_CALL.pattern + "|" + INI_CALL.pattern +
                     "|" + FLAG_CALL.pattern)
    for m in pat.finditer(text):
        tok = m.group(0)
        if tok == "{":
            if depth == 0:
                names = NAME_BEFORE_PAREN.findall(text[last_top:m.start()])
                func = names[0] if names else None
            depth += 1
            continue
        if tok == "}":
            depth = max(depth - 1, 0)
            if depth == 0:
                func, last_top = None, m.end()
            continue
        if tok == ";":
            if depth == 0:
                last_top = m.end()
            continue
        if depth == 0:
            continue                    # a prototype or a definition
        line = text.count("\n", 0, m.start()) + 1
        paren = text.index("(", m.start())
        whole, parts = args_of(raw, paren)
        if FILE_CALL.match(tok):
            if FILE_TOK.search(whole):
                out.append(("FILE", func or "<file scope>", line))
        elif INI_CALL.match(tok):
            if len(parts) >= 2 and GLOBAL_SEC.match(parts[1]):
                out.append(("INI", func or "<file scope>", line))
        else:
            out.append(("FLAG", func or "<file scope>", line))
    return out


def scan(root):
    found = []
    for rel, path in crr.sources(root):
        if rel in MODULES:
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            raw = f.read()
        for kind, func, line in findings_in(raw):
            found.append((kind, rel, func, line))
    return found


def read_allow(path):
    rules, bad = [], []
    if not os.path.exists(path):
        return rules, bad
    with open(path, encoding="utf-8") as f:
        for n, l in enumerate(f, 1):
            s = l.strip()
            if not s or s.startswith("#"):
                continue
            p = [x.strip() for x in s.split("|")]
            if len(p) != 4 or p[0] not in KINDS or not p[1] or not p[2] or not p[3]:
                bad.append("%s:%d: %s" % (os.path.basename(path), n, s))
                continue
            rules.append((p[0], p[1], p[2], p[3], n))
    return rules, bad


def verdict(found, rules, bad):
    used = set()
    missing = []
    for kind, rel, func, line in found:
        hit = None
        for r in rules:
            if r[0] == kind and r[1] == rel and r[2] in (func, "*"):
                hit = r
                break
        if hit:
            used.add(hit[4])
        else:
            missing.append((kind, rel, func, line))
    stale = [r for r in rules if r[4] not in used]
    return missing, stale


def run(root, allow, quiet=False):
    rules, bad = read_allow(allow)
    found = scan(root)
    missing, stale = verdict(found, rules, bad)
    if missing or stale or bad:
        if not quiet:
            print("check-settings-access FAILED: the settings files and the store's "
                  "decisions stay with kitty/kitty_globalsettings.c and kitty/kitty_inifile.c")
            seen = set()
            for kind, rel, func, line in missing:
                key = (kind, rel, func)
                if key in seen:
                    continue
                seen.add(key)
                print("  %s:%d: %s in %s() outside the settings module, not in "
                      "settings-access.allow" % (rel, line, kind, func))
            for r in stale:
                print("  stale line %d: %s | %s | %s matches nothing" % (r[4], r[0], r[1], r[2]))
            for b in bad:
                print("  malformed: " + b)
            print("\nMove it into the module, or add one line with its reason:\n"
                  "    KIND | file | function | <reason>")
        return 1
    if not quiet:
        print("check-settings-access: %d calls outside the module, each listed in "
              "tools/settings-access.allow (%d lines)" % (len(found), len(rules)))
    return 0


def self_test():
    tmp = tempfile.mkdtemp(prefix="ksa_")
    ok = True
    try:
        os.makedirs(os.path.join(tmp, "kitty"))
        def put(name, text):
            with open(os.path.join(tmp, name), "w") as f:
                f.write(text)
        put("kitty/a.c",
            "int f(void){ FILE *x = fopen(KittyIniFile, \"r\");return 0;}\n"
            "int g(void){ readINI(KittyIniFile, INIT_SECTION, \"k\", b, 4);return 0;}\n"
            "int h(void){ readINI(KittyIniFile, \"Shortcuts\", \"k\", b, 4);return 0;}\n"
            "int k(void){ if (GetReadOnlyFlag()) return 1;return 0;}\n"
            "int GetReadOnlyFlag(void);\n"
            "int m(void){ FILE *y = fopen(other, \"r\");/* GetReadOnlyFlag() */ return 0;}\n")
        put("kitty/kitty_inifile.c", "int z(void){ return GetReadOnlyFlag(); }\n")
        found = scan(tmp)
        kinds = sorted((k, fn) for k, _, fn, _ in found)
        want = [("FILE", "f"), ("FLAG", "k"), ("INI", "g")]
        def ok_(cond, what):
            nonlocal ok
            print(("ok    " if cond else "FAIL  ") + what)
            ok = ok and cond
        ok_(kinds == want, "a settings-file fopen, a global-section readINI and a flag "
                           "call are found; [Shortcuts], another file, a prototype, a "
                           "comment and the module are not (%s)" % kinds)
        allow = os.path.join(tmp, "a.allow")
        with open(allow, "w") as f:
            f.write("FILE | kitty/a.c | f | r\nINI | kitty/a.c | g | r\nFLAG | kitty/a.c | * | r\n")
        ok_(run(tmp, allow, quiet=True) == 0, "every finding listed: pass")
        with open(allow, "a") as f:
            f.write("FLAG | kitty/a.c | nothing | r\n")
        ok_(run(tmp, allow, quiet=True) == 1, "a stale line fails")
        with open(allow, "w") as f:
            f.write("FILE | kitty/a.c | f\n")
        ok_(run(tmp, allow, quiet=True) == 1, "a line without a reason fails")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("check-settings-access self-test: %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv):
    root, allow = DEFAULT_ROOT, DEFAULT_ALLOW
    if "--root" in argv:
        root = argv[argv.index("--root") + 1]
    if "--allow" in argv:
        allow = argv[argv.index("--allow") + 1]
    if "--self-test" in argv:
        return self_test()
    if "--list" in argv:
        for kind, rel, func, line in scan(root):
            print("%s | %s | %s | line %d" % (kind, rel, func, line))
        return 0
    return run(root, allow)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
