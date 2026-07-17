#!/usr/bin/env python3
"""Check that global kitty.ini options read by the source are mentioned in docs/examples/kitty.ini.example.

This is a lightweight drift guard for the inert sample file. It intentionally
focuses on string-literal keys in the global sections used by kitty.ini, not on
per-session registry/portable settings.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "docs" / "examples" / "kitty.ini.example"
SECTIONS = {"KiTTY", "ConfigBox", "Shortcuts", "Agent"}
ALLOW_UNDOCUMENTED = {
    # Internal state / sensitive legacy values that should not be advertised as knobs.
    ("KiTTY", "KiTTYPath"),
    ("KiTTY", "KiCount"),
    ("KiTTY", "KiLastUp"),
    ("KiTTY", "KiLic"),
    ("KiTTY", "KiPP"),
    ("KiTTY", "password"),
}


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def documented_options() -> set[tuple[str, str]]:
    opts: set[tuple[str, str]] = set()
    section = None
    for line in read_text(EXAMPLE).splitlines():
        m = re.match(r"\s*\[([^\]]+)\]", line)
        if m:
            section = m.group(1)
            continue
        m = re.match(r"\s*;?\s*([A-Za-z0-9_.-]+)\s*=", line)
        if m and section in SECTIONS:
            opts.add((section, m.group(1)))
    return opts


def source_options() -> set[tuple[str, str]]:
    opts: set[tuple[str, str]] = set()
    files = list((ROOT / "kitty").glob("*.c")) + list((ROOT / "windows").glob("*.c"))
    for path in files:
        text = read_text(path)
        for key in re.findall(r'ReadParameter\s*\(\s*INIT_SECTION\s*,\s*"([^"]+)"', text):
            opts.add(("KiTTY", key))
        for m in re.finditer(r'readINI\s*\([^;\n]*?"([^"]+)"\s*,\s*"([^"]+)"', text):
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
    return opts


def main() -> int:
    if not EXAMPLE.exists():
        print(f"missing {EXAMPLE}", file=sys.stderr)
        return 2
    documented = documented_options()
    source = source_options()
    missing = sorted(source - documented - ALLOW_UNDOCUMENTED)
    if missing:
        print("kitty.ini.example is missing documented options:", file=sys.stderr)
        for section, key in missing:
            print(f"  [{section}] {key}", file=sys.stderr)
        return 1
    print(f"OK: {EXAMPLE.relative_to(ROOT)} documents {len(documented)} options; source scan found {len(source)} literal global ini reads.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
