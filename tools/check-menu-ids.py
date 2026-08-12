#!/usr/bin/env python3
"""Check the menu command ids (IDM_*) for the mistakes that do not show up as
build errors - they show up as a menu item or a keyboard shortcut that silently
does nothing.

Four rules, each of which has already been broken at least once here:

1. ONE DEFINITION PER NAME. kitty/kitty.h used to carry its own #ifndef-guarded
   copies of 25 ids that windows/kitty_rc_additions.h also defined. Whichever
   header a translation unit included first won, and three of them disagreed
   (IDM_XYZSTART/UPLOAD/ABORT: 0xA810/20/30 vs 0xB150/60/70). Nothing outside
   window.c used those three, so it was consistent by luck.

2. ONE NAME PER VALUE. IDM_DUPKITTY sat on 0xB130, which is IDM_LAUNCHER.

3. MULTIPLE OF 0x10 - but only for ids used as `case` labels in window.c.
   windows/window.c dispatches WM_COMMAND and WM_SYSCOMMAND through the SAME
   switch, on `wParam & ~0xF`, because Windows reserves the low four bits of a
   WM_SYSCOMMAND wParam. An id like 0xB101 therefore arrives as 0xB100 and runs
   the wrong command. It does NOT apply to ids that are only decoded from the
   raw wParam (see rule 4) or handled by another window procedure.

4. RANGES ARE RESERVED. Some ids are bases, not commands: IDM_USERCMD+n for the
   predefined user commands, IDM_LAUNCHER+n for the launcher's tray menu. They
   are read back with LOWORD(wParam) - base, from the UNMASKED wParam, so
   consecutive ids are correct there - but nothing else may sit inside the span.

Run:  python3 tools/check-menu-ids.py     (exit 1 on any violation)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADERS = [ROOT / "kitty" / "kitty.h", ROOT / "windows" / "kitty_rc_additions.h"]
WINDOW_C = ROOT / "windows" / "window.c"

# base -> (how many ids it covers, why)
RANGES = {
    "IDM_USERCMD": (1024, "predefined user commands, NB_MENU_MAX"),
    "IDM_LAUNCHER": (16, "launcher tray menu, IDM_LAUNCHER+1..+9"),
}

DEFINE = re.compile(r"^\s*#\s*define\s+(IDM_[A-Z_0-9]+)\s+(0x[0-9a-fA-F]+|\d+)")


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="surrogateescape")


def defines():
    """name -> list of (value, file) - a list so redefinitions are visible."""
    found: dict[str, list[tuple[int, str]]] = {}
    for h in HEADERS:
        for line in read(h).splitlines():
            m = DEFINE.match(line)
            if m:
                val = int(m.group(2), 0)
                found.setdefault(m.group(1), []).append((val, h.name))
    return found


def case_labels() -> set[str]:
    """IDM_* used as `case` labels in window.c - the ones the masked switch has
    to match exactly."""
    return set(re.findall(r"^\s*case\s+(IDM_[A-Z_0-9]+)\s*:", read(WINDOW_C), re.M))


def main() -> int:
    ids = defines()
    cases = case_labels()
    problems: list[str] = []

    # 1. one definition per name (a name defined twice with the SAME value is
    #    still worth reporting - it is how the disagreeing ones started)
    for name, occurrences in sorted(ids.items()):
        if len(occurrences) > 1:
            where = ", ".join(f"{f}=0x{v:04X}" for v, f in occurrences)
            values = {v for v, _ in occurrences}
            severity = "DISAGREE" if len(values) > 1 else "duplicated"
            problems.append(f"{name}: {severity} ({where})")

    # 2. one name per value
    by_value: dict[int, list[str]] = {}
    for name, occurrences in ids.items():
        by_value.setdefault(occurrences[0][0], []).append(name)
    for val, names in sorted(by_value.items()):
        if len(names) > 1:
            problems.append(f"0x{val:04X}: shared by {', '.join(sorted(names))}")

    # 3. case labels must survive `wParam & ~0xF`
    for name in sorted(cases):
        if name not in ids:
            continue                    # defined elsewhere (putty's own ids)
        val = ids[name][0][0]
        if val & 0xF:
            problems.append(
                f"{name} = 0x{val:04X} is a `case` label in window.c but is not a "
                f"multiple of 0x10 - the switch matches on (wParam & ~0xF), so it "
                f"would arrive as 0x{val & ~0xF:04X}")

    # 4. nothing inside a reserved range
    for base_name, (count, why) in RANGES.items():
        if base_name not in ids:
            continue
        base = ids[base_name][0][0]
        for name, occurrences in sorted(ids.items()):
            if name == base_name:
                continue
            val = occurrences[0][0]
            if base < val < base + count:
                problems.append(
                    f"{name} = 0x{val:04X} lies inside {base_name}'s range "
                    f"0x{base:04X}-0x{base + count - 1:04X} ({why})")

    if problems:
        print("Menu id check FAILED:\n", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print("\nSee the comment at the top of tools/check-menu-ids.py.",
              file=sys.stderr)
        return 1

    print(f"OK: {len(ids)} menu ids, each defined once, no shared values; "
          f"{len(cases & set(ids))} used as case labels and all multiples of 0x10; "
          f"{len(RANGES)} reserved ranges clear.")
    return 0


sys.exit(main())
