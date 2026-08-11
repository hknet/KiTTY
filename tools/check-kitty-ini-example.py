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

It intentionally focuses on string-literal keys in the global sections used by
kitty.ini, not on per-session registry/portable settings.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "docs" / "examples" / "kitty.ini.example"
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
    ("Agent", "startupkey1"),
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

    if failed:
        return 1

    print(f"OK: {EXAMPLE.relative_to(ROOT)} documents {len(documented)} options; "
          f"{TEMPLATE.relative_to(ROOT)} writes {len(template)}; "
          f"source scan found {len(source)} literal global ini reads.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(verbose="-v" in sys.argv or "--verbose" in sys.argv))
