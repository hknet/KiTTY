#!/usr/bin/env python3
"""Check the two kitty.ini templates against the global options the source reads.

There are two copies of "the template" and they drift apart independently:

  docs/examples/kitty.ini.example   an inert sample file, shipped alongside the
                                    binaries and linked from the docs.
  kitty/kitty_ini.h                 the compiled-in copy, and the one that
                                    matters: it is what KiTTY WRITES when it
                                    creates a kitty.ini for the first time, so a
                                    key misspelt here is a key no user can set.

Three checks, two of them fatal:

  1. FATAL  every global option the source reads is mentioned in the example
            file (the original check).
  2. FATAL  every key the compiled-in template writes is actually read by the
            source. This is the direction that catches a typo: `winrol=yes`
            sat in the template for years matching nothing, because a key
            nobody reads cannot fail check 1.
  3. FATAL  keys the source reads that the compiled-in template never mentions.
            The compiled-in copy is generated from the sample by
            tools/gen-kitty-ini-template.py, so the two agree by construction -
            this catches a sample that was never regenerated into the header.

  5. FATAL  a NEW key that its section's row in KITTY-INI.md's "Sections at
            a glance" table does not name. That table is what a reader scans
            first, and it went stale silently: the [Agent] row still listed
            the pre-Hello settings a release after hellocacheseconds shipped.
  6. FATAL  a NEW key mentioned in NEITHER docs/KITTY-INI.md NOR FEATURES.md.
            The per-key reference is the annotated example itself (that is
            what KITTY-INI.md says it is), so this does not ask for a second
            copy of it - only that a knob is described in prose SOMEWHERE.
            0.85.1.3-beta shipped hellocacheseconds with neither.

  Both are measured against tools/kitty-ini-doc-baseline.txt, the keys that
  predate the checks. That file MAY ONLY SHRINK - a new key cannot be added to
  it, and deleting a line is how a key stops being an exception.

  4. FATAL  an ini_params[] key whose value nothing outside its own plumbing
            ever reads - a knob that turns nothing. WEAK on purpose: it sees
            references, not reachability, so a flag read only from dead code
            still passes (that is how `icon` survived). It catches the blatant
            cases and must not be trusted further than that.

It intentionally focuses on string-literal keys in the global sections used by
kitty.ini, not on per-session registry/portable settings.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "docs" / "examples" / "kitty.ini.example"
GUIDE = ROOT / "docs" / "KITTY-INI.md"
FEATURES = ROOT / "FEATURES.md"
DOC_BASELINE = ROOT / "tools" / "kitty-ini-doc-baseline.txt"
TEMPLATE = ROOT / "kitty" / "kitty_ini.h"
# Every section kitty.ini has. It used to be four of these, which meant the
# [Print], [Launcher] and [FontFallback] keys - 14 of them - were documented
# but never checked either way.
SECTIONS = {"KiTTY", "ConfigBox", "Shortcuts", "Agent",
            "Print", "Launcher", "FontFallback"}
ALLOW_UNDOCUMENTED = {
    # Internal state / sensitive legacy values that should not be advertised as knobs.
    ("KiTTY", "KiTTYPath"),
    ("KiTTY", "KiCount"),
    ("KiTTY", "KiLastUp"),
    ("KiTTY", "KiLic"),
    ("KiTTY", "KiPP"),
    ("KiTTY", "password"),
    # Read only to migrate it to [Agent] loadkeysonstartup; not a knob.
    ("Agent", "loadonstartup"),
}
# Keys whose variable is referenced only from its own plumbing but which are
# not dead - none known; entries here need a reason.
ALLOW_DEAD_KNOBS: set[str] = set()

ALLOW_TEMPLATE_UNREAD = {
    # The template documents cygterm, but nothing in the built sources reads the
    # key: the cygterm support under kitty/cthelper/ is not wired into this
    # port's CMake build, so the option has no effect wherever it is set.
    ("KiTTY", "cygterm"),
    # Real keys the literal scan cannot see, because the name is built at
    # runtime or hidden behind a macro rather than written out at the call:
    #   kageant_policy_get("lockdownmode", ...)   kitty/kitty_pageant.c
    #   snprintf(key, ..., "startupkey%d", i)     kitty/kitty_pageant.c
    #   KL_GEOM_INIKEY / KL_COLS_INIKEY           windows/pageant.c
    ("Agent", "lockdownmode"),
    ("Agent", "blockipcadd"),
    ("Agent", "blockipcremove"),
    ("Agent", "keylistgeometry"),
    ("Agent", "keylistcolumns"),
    ("Agent", "agentloggeometry"),
    ("Agent", "agentlogcolumns"),
    ("Agent", "startupkey1"),
    #   kageant_int_setting("agentlogmaxkb", ...)  kitty/kitty_pageant.c
    #   kageant_setting_str_get("agentlogpath", ...)      - " -
    ("Agent", "agentlogpath"),
    ("Agent", "agentlogmaxkb"),
    ("Agent", "agentlogkeep"),
    ("Agent", "agentlogexpiredays"),
}


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def ini_options(lines: list[str]) -> set[tuple[str, str]]:
    """Section/key pairs from plain ini lines; commented-out keys count."""
    opts: set[tuple[str, str]] = set()
    section = None
    for line in lines:
        m = re.match(r"\s*\[([^\]]+)\]", line)
        if m:
            section = m.group(1)
            continue
        # `key=` or `;key=` hard against the left margin. Indented text after a
        # ';' is prose - "Default 0 = off", "auto (default) = ...", the
        # namedproxy value list - and must not be mistaken for a key.
        m = re.match(r";?([A-Za-z0-9_.-]+)\s*=", line)
        if m and section in SECTIONS:
            opts.add((section, m.group(1)))
    return opts


def documented_options() -> set[tuple[str, str]]:
    return ini_options(read_text(EXAMPLE).splitlines())


def template_options() -> set[tuple[str, str]]:
    """The compiled-in template is one big C string literal: every line is an
    ini line wrapped as  <text>\\n\\  , with the first line carrying the
    declaration. Strip the wrapping and it parses as ordinary ini."""
    lines = []
    for line in read_text(TEMPLATE).splitlines():
        line = re.sub(r"\\n\\\s*$", "", line)
        line = line.replace('char default_init_file_content[] = "', "")
        lines.append(line)
    return ini_options(lines)


def source_options() -> set[tuple[str, str]]:
    opts: set[tuple[str, str]] = set()
    files = list((ROOT / "kitty").glob("*.c")) + list((ROOT / "windows").glob("*.c"))
    for path in files:
        text = read_text(path)
        # ReadParameterN is the size-checked variant of the same call.
        for key in re.findall(r'ReadParameterN?\s*\(\s*INIT_SECTION\s*,\s*"([^"]+)"', text):
            opts.add(("KiTTY", key))
        for m in re.finditer(r'readINI\s*\([^;\n]*?"([^"]+)"\s*,\s*"([^"]+)"', text):
            section, key = m.groups()
            if section in SECTIONS:
                opts.add((section, key))
        # ReadParameterN("Launcher", "key", ...) - a named section rather than
        # the INIT_SECTION macro. [Launcher] is read this way throughout.
        for m in re.finditer(r'ReadParameterN?\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)"', text):
            section, key = m.groups()
            if section in SECTIONS:
                opts.add((section, key))
        # kitty_config.c mirrors INIT_SECTION without including kitty.h.
        for m in re.finditer(r'readINI\s*\([^;\n]*?KITTY_INI_SECTION\s*,\s*"([^"]+)"', text):
            opts.add(("KiTTY", m.group(1)))
        # satellite binaries read the ini through kitty_inilight.
        for m in re.finditer(r'kitty_inilight_(?:read|write)\s*\(\s*"([^"]+)"\s*,\s*"([^"]+)"', text):
            section, key = m.groups()
            if section in SECTIONS:
                opts.add((section, key))
        # readINI with the INIT_SECTION macro (kitty.h) also targets [KiTTY].
        for m in re.finditer(r'readINI\s*\([^;\n]*?\bINIT_SECTION\s*,\s*"([^"]+)"', text):
            opts.add(("KiTTY", m.group(1)))
        # Most plain keyword/number keys no longer have a literal ReadParameter
        # call site: they are rows in kitty.c's declarative ini_params[] table,
        # applied in one pass by load_ini_params(). Without this the whole table
        # was invisible to the check.
        #   INIP_KW (sec, use_readini, "key", yes, no, other, &var, setter)
        #   INIP_NUM(sec, use_readini, "key", intmin,          &var, setter)
        for m in re.finditer(
            r'INIP_(?:KW|NUM)\s*\(\s*(INIT_SECTION|"[^"]+")\s*,\s*\d+\s*,\s*"([^"]+)"', text
        ):
            section, key = m.groups()
            section = "KiTTY" if section == "INIT_SECTION" else section.strip('"')
            if section in SECTIONS:
                opts.add((section, key))
    return opts


def unread_knobs() -> list[tuple[str, str]]:
    """ini_params[] rows whose value nothing consults.

    A flag is usually read through its accessor - GetCtrlTabFlag() rather than
    CtrlTabFlag - so both the variable AND its getter have to be unreferenced
    outside their own plumbing before the key is called dead. Returns
    (key, variable).
    """
    kitty_c = (ROOT / "kitty" / "kitty.c").read_text(encoding="utf-8", errors="ignore")
    rows = re.findall(
        r'INIP_(?:KW|NUM)\s*\(\s*(?:INIT_SECTION|"[^"]+")\s*,\s*\d+\s*,\s*'
        r'"([^"]+)"[^)]*?,\s*(&\w+|NULL)\s*,\s*(\w+)\s*\)', kitty_c)

    # terminal/ too: the terminal core consults a [KiTTY] key through its
    # getter (framepace, terminal.c) - a knob read there is not dead.
    sources = (list((ROOT / "kitty").glob("*.c")) + list((ROOT / "windows").glob("*.c"))
               + list((ROOT / "terminal").glob("*.c")))
    texts = {p: read_text(p) for p in sources}
    all_text = "\n".join(texts.values())

    def getters_for(var: str) -> set[str]:
        """Functions whose body is just `return var`."""
        return set(re.findall(
            r"\b(\w+)\s*\([^)]*\)\s*\{\s*return\s+" + re.escape(var) + r"\s*;",
            all_text))

    def consulted(name: str, is_func: bool) -> bool:
        pat = re.compile(r"\b" + re.escape(name) + (r"\s*\(" if is_func else r"\b"))
        for text in texts.values():
            for line in text.split("\n"):
                if not pat.search(line):
                    continue
                st = line.strip()
                if (st.startswith("//") or st.startswith("*") or
                        st.startswith("/*") or st.startswith("extern ")):
                    continue
                if "INIP_KW(" in st or "INIP_NUM(" in st:
                    continue
                if re.match(r"(static\s+)?(int|BOOL|bool)\s+" + re.escape(name)
                            + r"\s*(=|;)", st):
                    continue
                # the accessor pair itself, and a bare prototype
                if re.search(r"\breturn\s+\w+\s*;", st) and "{" in st:
                    continue
                if re.match(r"void\s+\w+\s*\([^)]*\)\s*\{[^}]*=", st):
                    continue
                if re.match(r"(int|void|bool|BOOL)\s+" + re.escape(name)
                            + r"\s*\([^)]*\)\s*;", st):
                    continue
                return True
        return False

    dead = []
    for key, var, setter in rows:
        name = var[1:] if var.startswith("&") else None
        if not name:
            m = re.search(r"void\s+" + re.escape(setter) +
                          r"\s*\([^)]*\)\s*\{[^}]*?(\w+)\s*=", all_text)
            if not m:
                continue
            name = m.group(1)
        if consulted(name, False):
            continue
        if any(consulted(g, True) for g in getters_for(name)):
            continue
        dead.append((key, name))
    return dead


def doc_baseline() -> dict[str, set[tuple[str, str]]]:
    """The keys each documentation check is allowed to skip, by check name."""
    out: dict[str, set[tuple[str, str]]] = {"row": set(), "text": set()}
    if not DOC_BASELINE.exists():
        return out
    for line in read_text(DOC_BASELINE).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) == 3 and parts[0] in out:
            out[parts[0]].add((parts[1], parts[2]))
    return out


def section_rows() -> dict[str, str]:
    """The 'Sections at a glance' table: section -> the text of its row."""
    rows: dict[str, str] = {}
    for line in read_text(GUIDE).splitlines():
        m = re.match(r"\|\s*`\[([A-Za-z]+)\]`\s*\|(.*)\|", line)
        if m:
            rows[m.group(1)] = m.group(2).lower()
    return rows


def prose_text() -> str:
    """Everything a reader could learn a setting's meaning from, in prose."""
    return (read_text(GUIDE) + "\n" + read_text(FEATURES)).lower()


def main(verbose: bool = False) -> int:

    for path in (EXAMPLE, TEMPLATE):
        if not path.exists():
            print(f"missing {path}", file=sys.stderr)
            return 2

    documented = documented_options()
    template = template_options()
    source = source_options()
    failed = False

    # 1. the example file must mention every option the source reads.
    missing = sorted(source - documented - ALLOW_UNDOCUMENTED)
    if missing:
        failed = True
        print(f"{EXAMPLE.relative_to(ROOT)} is missing documented options:", file=sys.stderr)
        for section, key in missing:
            print(f"  [{section}] {key}", file=sys.stderr)

    # 2. the compiled-in template must not write a key nothing reads. A key
    #    here that the source never asks for is dead on arrival: whatever the
    #    user sets under it is silently ignored.
    unread = sorted(template - source - ALLOW_TEMPLATE_UNREAD)
    if unread:
        failed = True
        print(f"{TEMPLATE.relative_to(ROOT)} writes keys that no source reads "
              f"(misspelt, or the feature is gone):", file=sys.stderr)
        for section, key in unread:
            print(f"  [{section}] {key}", file=sys.stderr)

    # 3. the compiled-in template must mention every option the source reads.
    #    It is generated from the sample, so a failure here means the sample
    #    gained a key and nobody re-ran tools/gen-kitty-ini-template.py.
    behind = sorted(source - template - ALLOW_UNDOCUMENTED)
    if behind:
        failed = True
        print(f"{TEMPLATE.relative_to(ROOT)} never mentions "
              f"{len(behind)} option(s) the source reads "
              f"(re-run tools/gen-kitty-ini-template.py):", file=sys.stderr)
        for section, key in behind:
            print(f"  [{section}] {key}", file=sys.stderr)

    # 4. a key that turns nothing. See the caveat at the top of this file.
    dead = unread_knobs()
    dead = [d for d in dead if (SECTIONS and d[0] not in ALLOW_DEAD_KNOBS)]
    if dead:
        failed = True
        print("ini keys whose value nothing reads (a knob that turns nothing):",
              file=sys.stderr)
        for key, var in dead:
            print(f"  {key}  ->  {var}", file=sys.stderr)

    # 5 + 6. documentation of the KEYS THEMSELVES. Only new keys are judged:
    # everything already undocumented when the checks were added is listed in
    # tools/kitty-ini-doc-baseline.txt, which may only shrink.
    baseline = doc_baseline()
    rows = section_rows()
    prose = prose_text()

    row_missing = sorted(
        (s, k) for (s, k) in documented
        if s in rows and k.lower() not in rows[s] and (s, k) not in baseline["row"])
    if row_missing:
        failed = True
        print(f"{GUIDE.relative_to(ROOT)}: the 'Sections at a glance' row for "
              f"these sections does not name {len(row_missing)} new key(s):",
              file=sys.stderr)
        for section, key in row_missing:
            print(f"  [{section}] {key}", file=sys.stderr)
        print("  -> name it in that row, or (only for a key that predates this "
              "check) add it to tools/kitty-ini-doc-baseline.txt", file=sys.stderr)

    text_missing = sorted(
        (s, k) for (s, k) in documented
        if k.lower() not in prose and (s, k) not in baseline["text"])
    if text_missing:
        failed = True
        print(f"{len(text_missing)} new key(s) appear in NEITHER "
              f"{GUIDE.relative_to(ROOT)} NOR {FEATURES.relative_to(ROOT)} - a "
              f"knob nobody describes in prose:", file=sys.stderr)
        for section, key in text_missing:
            print(f"  [{section}] {key}", file=sys.stderr)
        print("  -> describe it where it belongs (the guide for how the file "
              "works, FEATURES.md for what the feature does)", file=sys.stderr)

    if failed:
        return 1


    print(f"OK: {EXAMPLE.relative_to(ROOT)} documents {len(documented)} options; "
          f"{TEMPLATE.relative_to(ROOT)} writes {len(template)}; "
          f"source scan found {len(source)} literal global ini reads.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(verbose="-v" in sys.argv or "--verbose" in sys.argv))
