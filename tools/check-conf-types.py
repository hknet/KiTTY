#!/usr/bin/env python3
"""Check every Conf accessor call against the type of the key it names.

utils/conf.c asserts that each accessor is used on a key of the right type
(conf_get_str only on a CONF_TYPE_STR key, conf_get_bool only on a BOOL
key, ...). A mismatch compiles without a warning and only fails when the
call runs - an assertion that ends the program. The classic case is a key
declared STR_AMBI (CONF_username, CONF_remote_cmd, CONF_remote_cmd2): it
must be read with conf_get_str_ambi, and a conf_get_str on it builds fine
and aborts at run time.

This script reads each key's value and subkey type from conf.h
(CONF_OPTION / VALUE_TYPE / SUBKEY_TYPE) and checks every call of a
conf_get_* / conf_set_* / conf_try_set_* / conf_del_* accessor whose
second argument is a literal CONF_ key, in every .c and .h file of the
source tree (build directories and .git skipped; comments and string
literals ignored; calls spanning several lines included). Calls whose key
is a variable cannot be checked here and are skipped.

Run:  python3 tools/check-conf-types.py     (exit 1 on any mismatch)
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONF_H = ROOT / "conf.h"

# accessor -> (value types it accepts, subkey type it needs or None)
ANY_STR = {"STR", "STR_AMBI", "UTF8"}
ACCESSORS = {
    "conf_get_bool": ({"BOOL"}, None),
    "conf_set_bool": ({"BOOL"}, None),
    "conf_get_int": ({"INT"}, None),
    "conf_set_int": ({"INT"}, None),
    "conf_get_int_int": ({"INT"}, "INT"),
    "conf_set_int_int": ({"INT"}, "INT"),
    "conf_get_str": ({"STR"}, None),
    "conf_get_utf8": ({"UTF8"}, None),
    "conf_get_str_ambi": (ANY_STR, None),
    "conf_set_str": ({"STR", "STR_AMBI"}, None),
    "conf_try_set_str": (ANY_STR, None),
    "conf_set_utf8": ({"UTF8", "STR_AMBI"}, None),
    "conf_try_set_utf8": (ANY_STR, None),
    "conf_get_str_str": ({"STR"}, "STR"),
    "conf_get_str_str_opt": ({"STR"}, "STR"),
    "conf_get_str_strs": ({"STR"}, "STR"),
    "conf_get_str_nthstrkey": ({"STR"}, "STR"),
    "conf_set_str_str": ({"STR"}, "STR"),
    "conf_del_str_str": ({"STR"}, "STR"),
    "conf_get_filename": ({"FILENAME"}, None),
    "conf_set_filename": ({"FILENAME"}, None),
    "conf_get_fontspec": ({"FONT"}, None),
    "conf_set_fontspec": ({"FONT"}, None),
}

# the accessor to use for a key of this (value type, subkey type)
SUGGEST = {
    ("BOOL", None): "conf_get_bool / conf_set_bool",
    ("INT", None): "conf_get_int / conf_set_int",
    ("INT", "INT"): "conf_get_int_int / conf_set_int_int",
    ("STR", None): "conf_get_str / conf_set_str",
    ("STR_AMBI", None): "conf_get_str_ambi / conf_set_str or conf_set_utf8",
    ("UTF8", None): "conf_get_utf8 or conf_get_str_ambi / conf_set_utf8",
    ("STR", "STR"): "conf_get_str_str / conf_set_str_str and the other _str_str accessors",
    ("FILENAME", None): "conf_get_filename / conf_set_filename",
    ("FONT", None): "conf_get_fontspec / conf_set_fontspec",
}

CALL = re.compile(r"\b(conf_(?:get|set|try_set|del)_\w+)\s*\(")
KEY = re.compile(r"\s*CONF_(\w+)\b")


def key_types():
    text = CONF_H.read_text(encoding="utf-8", errors="replace")
    types = {}
    for m in re.finditer(r"CONF_OPTION\(\s*(\w+)\s*,(.*?)\n\)", text, re.S):
        body = m.group(2)
        vt = re.search(r"VALUE_TYPE\((\w+)\)", body)
        st = re.search(r"SUBKEY_TYPE\((\w+)\)", body)
        types[m.group(1)] = (vt.group(1) if vt else None,
                             st.group(1) if st else None)
    return types


def blank_comments_and_strings(text):
    """Comments and string/char literals replaced by spaces, newlines kept,
    so offsets and line numbers stay those of the file."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            for k in range(i + 1, j - 1):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def second_arg_start(text, pos):
    """Offset just after the first top-level comma of the call whose '('
    ends at pos, or -1."""
    depth = 0
    i, n = pos, len(text)
    while i < n:
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return -1
            depth -= 1
        elif c == "," and depth == 0:
            return i + 1
        elif c == ";":
            return -1
        i += 1
    return -1


def source_files():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames
                       if d != ".git" and not d.startswith("build")
                       and not os.path.exists(os.path.join(dirpath, d, "CMakeCache.txt"))]
        for f in filenames:
            if f.endswith((".c", ".h")):
                yield Path(dirpath) / f


def main():
    types = key_types()
    if not types:
        print("check-conf-types: no CONF_OPTION found in conf.h")
        return 1
    bad = []
    calls = 0
    for path in source_files():
        raw = path.read_text(encoding="utf-8", errors="replace")
        if "conf_" not in raw:
            continue
        text = blank_comments_and_strings(raw)
        for m in CALL.finditer(text):
            acc = m.group(1)
            if acc not in ACCESSORS:
                continue
            start = second_arg_start(text, m.end())
            if start < 0:
                continue
            km = KEY.match(text, start)
            if not km or km.group(1) not in types:
                continue
            calls += 1
            want, sub = ACCESSORS[acc]
            vt, st = types[km.group(1)]
            if vt in want and sub == st:
                continue
            line = text.count("\n", 0, m.start()) + 1
            rel = path.relative_to(ROOT).as_posix()
            bad.append("%s:%d: %s(CONF_%s): the key is %s%s - use %s" % (
                rel, line, acc, km.group(1), vt,
                (" with %s subkeys" % st) if st else "",
                SUGGEST.get((vt, st), "the accessor of that type")))
    for b in bad:
        print(b)
    if bad:
        print("check-conf-types: %d call(s) use an accessor of the wrong type "
              "(utils/conf.c asserts at run time)" % len(bad))
        return 1
    print("check-conf-types: %d calls match their keys' types" % calls)
    return 0


if __name__ == "__main__":
    sys.exit(main())
