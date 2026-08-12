#!/usr/bin/env python3
"""Find duplicate keyboard accelerators inside a single dialog template.

Two controls in one dialog sharing an accelerator letter is invisible in the
source and invisible on screen. One of them silently takes the key: in
kageant's key list, "&Add Key" and "Stop &agent" both claimed A, and Alt+A
pressed STOP AGENT (measured 2026-08-12) - the destructive one. It was found by
a user, after shipping that way for a long time, which is the whole argument
for checking it here.

Scans every DIALOG/DIALOGEX template in the .rc files given (or the repo's
windows/*.rc by default) and exits 1 if any dialog uses a letter twice.

Not checked, deliberately: labels built at run time. windows/pageant.c swaps
"Re-e&ncrypt" for "&Decrypt" on the same button, so a clash can be introduced
in C as well - the KNOWN_RUNTIME list below records those so a reader knows
they were considered rather than missed.
"""
import glob
import os
import re
import sys

# Labels that C code substitutes into an existing control at run time, as
# (dialog, letter, text). Checked against each dialog's static letters too.
KNOWN_RUNTIME = [
    ("IDD_KEYLIST", "D", "&Decrypt (replaces Re-e&ncrypt at run time)"),
]

DIALOG_RE = re.compile(r'^\s*(\w+)\s+DIALOG(EX)?\b')
STRING_RE = re.compile(r'"((?:[^"]|"")*)"')
ACCEL_RE = re.compile(r'&(.)')


def accelerators(path):
    """Yield (dialog, letter, text, lineno) for every accelerator in a file."""
    dialog = None
    with open(path, encoding="utf-8", errors="replace") as fp:
        for lineno, line in enumerate(fp, 1):
            if dialog is None:
                m = DIALOG_RE.match(line)
                if m:
                    dialog = m.group(1)
                continue
            if line.startswith("END"):
                dialog = None
                continue
            # Skip comment lines: they quote control labels when explaining
            # them, and those are not real controls.
            stripped = line.strip()
            if stripped.startswith("/*") or stripped.startswith("*"):
                continue
            for text in STRING_RE.findall(line):
                m = ACCEL_RE.search(text)
                if not m:
                    continue
                ch = m.group(1)
                if ch == "&":          # "&&" is a literal ampersand
                    continue
                yield dialog, ch.upper(), text, lineno


def main(argv):
    files = argv[1:]
    if not files:
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        files = sorted(glob.glob(os.path.join(here, "windows", "*.rc")))
    if not files:
        print("no .rc files to check", file=sys.stderr)
        return 2

    seen = {}          # (dialog, letter) -> (text, lineno, path)
    clashes = []
    total = 0
    for path in files:
        for dialog, letter, text, lineno in accelerators(path):
            total += 1
            key = (dialog, letter)
            if key in seen:
                first = seen[key]
                clashes.append((path, lineno, dialog, letter, text, first))
            else:
                seen[key] = (text, lineno, path)

    for dialog, letter, text in KNOWN_RUNTIME:
        key = (dialog, letter)
        if key in seen:
            first = seen[key]
            clashes.append(("(run time)", 0, dialog, letter, text, first))

    if clashes:
        for path, lineno, dialog, letter, text, first in clashes:
            print("%s:%d: %s: Alt+%s is already \"%s\" (%s:%d) - also \"%s\"" %
                  (path, lineno, dialog, letter,
                   first[0], os.path.basename(first[2]), first[1], text),
                  file=sys.stderr)
        print("FAIL: %d duplicate accelerator(s)" % len(clashes), file=sys.stderr)
        return 1

    print("OK: %d accelerators across %d dialog(s), none duplicated" %
          (total, len({d for d, _ in seen})))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
