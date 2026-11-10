#!/usr/bin/env python3
"""A global setting is read from the store it is written to.

KiTTY++ keeps its global settings in two stores:

- param: the store in charge. ReadParameterDup / ReadParameterN /
  ReadParameter and WriteParameter / WriteParameterNoReport / DelParameter
  (kitty/kitty_params.c). With savemode=dir that is kitty.ini only. Otherwise
  it is the registry hive in use first, with kitty.ini as the read fallback.
  The hive is FLAT: WriteParameter drops the section, so [A] x and [B] x are
  one registry value.
- file: kitty.ini only, in every save mode. readINI / writeINI / delINI /
  kitty_read_ini_dup, kitty_inifile_read / kitty_inifile_read_dup /
  kitty_inifile_write, kitty_profile_string_dup, Get/WritePrivateProfileString.

A key written to one store and read from the other is a setting that does not
stick: the panel shows the new value, the next start the old one (or a stale
registry value shadows the file for ever). So for every (section, key) this
check collects the stores it is READ from and the stores it is WRITTEN to
(a delete counts as a write). It fails when:

- both sets exist and differ. A key only read, or only written, is fine.
- the key is read from both stores but not written to both.
- two param keys of different sections have the same name: the flat hive
  holds one value for both.
- a call whose key is a variable sits outside a known generic accessor and
  carries no marker: the check cannot place it.
- a line of tools/store-pairs.allow matches no finding (stale).
- a GENERIC entry below matches no call any more (stale).

Where the pairs come from:
- direct calls of the functions above with a macro or a string literal as
  section and key. INIT_SECTION and the literal "KiTTY" are [KiTTY]. A call
  spanning lines counts at its first line. Prototypes, definitions and
  commented-out calls do not count. Keys and sections compare without case.
- the wrappers in WRAPPERS below: functions that take a key and read or
  write one store, called with a literal key. Their own body is GENERIC.
- the INIP_KW / INIP_NUM rows of kitty/kitty_params.c, read only. The rdini
  column picks the store: 1 = file, 0 = param.
- the kset_keys[] rows of kitty/kitty_config_shared.c, read and written. The
  file_only column picks the store: true = file, false = param.
- the sc_actions[] rows of kitty/kitty_shortcuts.c (SC_ACT): [Shortcuts], read
  with readINI by InitShortcuts, written with writeINI by the Shortcuts
  panel. The kr_rules[] rows of kitty/kitty_reconnect.c: [KiTTY], read with
  ReadParameterN (the kset rows write them).
- markers, see below.

Markers. A comment of the form

    /* store-pairs: <read|write> <param|file> <SECTION> <KEYS> [!KEY ...] - <reason> */

records an access the parser cannot see: a call with a variable key, or a
hand-rolled registry or kitty.ini reader or writer. SECTION and each key are
a KI_ / KR_ macro or the stored name itself. KEYS is one key, several keys
joined by commas, or "*" = every key of the section. A "*" applies to every
key of that section the check knows from elsewhere, except the keys given
with "!" (each of them must appear in the marker's file, as a macro of that
value or as a literal). A marker may span lines. A marker block (markers on
consecutive lines) that ends on the line of a counted call, or on the line
directly above it, REPLACES what the check reads from that call. This is how
the kitty.ini fallback leg of a hand-rolled param reader is counted as param.
A marker block attached to no call stands on its own.

Not counted (not global settings of these two stores):
- a file call whose path argument is not KittyIniFile, GetKittyIniFile() or
  ini, and any file call in kitty/kitty_storemove.c or kitty/kitty_migrate.c
  whose path is ini. Those address another store's kitty.ini (a copy source
  or target, an old KiTTY's file), kitty_windowpos.ini (kitty_auxpos.c) or
  the resolver's own file (kitty_inifile.c, kitty_oldwin.c).
- the registry outside these functions unless a marker records it. That is
  kageant's own value names under KAGEANT_REG_BASE (its [Agent] keys in
  kitty.ini are counted), the launcher's per-session values (KR_FOLDER and
  the Launcher, Sessions, Commands, Proxies, WindowPos subkeys), state the
  hive keeps with no kitty.ini twin (kitty_secretstore.c, kitty_storage.c,
  kitty_updater.c, kitty_proxy.c, kitty_termenv_hint.c, kitty_winpos.c), the
  copy of the store in use that a store move or a backup reads (it reads
  every value and uses none), and the AutoText texts the one-time copy of
  kitty_startup.c reads from the hive and deletes there. A "*" marker on that
  copy would put every [Shortcuts] key in both stores. Its read of the list
  is marked and kept in the allow file.
- session settings (the Conf / session store).
- kitty/mini/ (the kitty.ini library), windows/test/, unix/, .git and build
  folders.

tools/store-pairs.allow, one line per kept finding:
    <SECTION> <KEY> | <reason>
A flat-name collision is listed as  FLAT <KEY> | <reason>.

Run:  python3 tools/check-store-pairs.py               (exit 1 on a finding)
      python3 tools/check-store-pairs.py --self-test   (the check on planted
                                                         sources)
      python3 tools/check-store-pairs.py --list        (every key and its
                                                         stores)
Options: --root DIR (source tree), --allow FILE.
"""
import io
import os
import re
import shutil
import sys
import tempfile
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)

R, W = "read", "write"
PARAM, FILE = "param", "file"
BOTH = frozenset((PARAM, FILE))

# name: (op, store, section arg, key arg, path arg or None)
FUNCS = {
    "ReadParameterDup":        (R, PARAM, 0, 1, None),
    "ReadParameterN":          (R, PARAM, 0, 1, None),
    "ReadParameter":           (R, PARAM, 0, 1, None),
    "WriteParameter":          (W, PARAM, 0, 1, None),
    "WriteParameterNoReport":  (W, PARAM, 0, 1, None),
    "DelParameter":            (W, PARAM, 0, 1, None),
    "readINI":                 (R, FILE, 1, 2, 0),
    "writeINI":                (W, FILE, 1, 2, 0),
    "delINI":                  (W, FILE, 1, 2, 0),
    "kitty_read_ini_dup":      (R, FILE, 1, 2, 0),
    "kitty_inifile_read":      (R, FILE, 0, 1, None),
    "kitty_inifile_read_dup":  (R, FILE, 0, 1, None),
    "kitty_inifile_write":     (W, FILE, 0, 1, None),
    "kitty_profile_string_dup": (R, FILE, 0, 1, 3),
    "GetPrivateProfileStringA": (R, FILE, 0, 1, 5),
    "GetPrivateProfileString": (R, FILE, 0, 1, 5),
    "WritePrivateProfileStringA": (W, FILE, 0, 1, 3),
    "WritePrivateProfileString": (W, FILE, 0, 1, 3),
}

# Functions that take a key and read or write one store of one section:
# name: (op, store, section, key arg, reason)
WRAPPERS = {
    "console_read_switch": (R, PARAM, "KiTTY", 1,
        "the console tools' [KiTTY] switch: the hive first, kitty.ini as fallback"),
    "kitty_wev_switch_on": (R, PARAM, "KiTTY", 0,
        "the Event Log switches of the satellites: the hive first, kitty.ini as fallback"),
    "kitty_switchnotice_seconds": (R, PARAM, "KiTTY", 0,
        "a notice time in seconds, ReadParameterN"),
    "kt_global_int": (R, PARAM, "KiTTY", 0,
        "a transfer limit's global default, ReadParameterN"),
    "kx_xfer_dir_dup": (R, PARAM, "KiTTY", 2,
        "a transfer folder's global default, ReadParameterDup"),
    "kx_try_stored": (R, PARAM, "KiTTY", 0,
        "a stored helper path, ReadParameterDup"),
    "adopt_tool_path_if_exists": (W, PARAM, "KiTTY", 2,
        "a found helper path written back, WriteParameter"),
    "kitty_kset_backupcount": (R, PARAM, "KiTTY", 0,
        "a backup count, ReadParameterN"),
    "kstore_ini_dup": (R, FILE, "KiTTY", 1,
        "a folder-store layout key, kitty_read_ini_dup"),
    "kitty_store_ending_applied": (R, FILE, "KiTTY", 0,
        "the file ending last applied, readINI"),
    "kitty_store_ending_mark": (W, FILE, "KiTTY", 0,
        "the file ending applied, writeINI / delINI"),
    "retire_reg_value_present": (R, PARAM, "KiTTY", 0,
        "a retired value, looked for in the hive"),
    "retire_ini_value_present": (R, FILE, "KiTTY", 0,
        "a retired value, looked for in kitty.ini"),
    "kageant_setting_write": (W, FILE, "Agent", 0,
        "kageant's [Agent] writer (its registry side is KAGEANT_REG_BASE)"),
    "kageant_ini_read_dup": (R, FILE, "Agent", 0,
        "kageant's [Agent] reader"),
    "kageant_setting_str_dup": (R, FILE, "Agent", 0,
        "kageant's [Agent] text reader"),
    "kageant_setting_str_set": (W, FILE, "Agent", 0,
        "kageant's [Agent] text writer"),
    "kageant_bool_get": (R, FILE, "Agent", 0,
        "kageant's [Agent] yes/no reader"),
    "kageant_int_setting": (R, FILE, "Agent", 0,
        "kageant's [Agent] number reader"),
    "kageant_policy_read": (R, FILE, "Agent", 0,
        "kageant's [Agent] policy reader"),
    "kageant_policy_get": (R, FILE, "Agent", 0,
        "kageant's [Agent] policy reader, cached"),
    "kageant_policy_get_def": (R, FILE, "Agent", 0,
        "kageant's [Agent] policy reader, cached"),
    "kageant_policy_set": (W, FILE, "Agent", 0,
        "kageant's [Agent] policy writer"),
    "kageant_geometry_carry_over": ((R, W), FILE, "Agent", 0,
        "an old [Agent] window geometry, read and then emptied"),
    "ShortcutsIniDup": (R, FILE, "Shortcuts", 0,
        "the AutoText list or text, kitty.ini only"),
}

# Calls with a variable key the check accepts without a marker: the bodies of
# the accessors and wrappers themselves, and the loops over the tables read
# above. (file, function, callee): reason. An entry that matches no call fails.
GENERIC = {
    ("kitty/kitty_params.c", "write_parameter", "writeINI"):
        "WriteParameter itself",
    ("kitty/kitty_params.c", "ReadParameterDup", "kitty_read_ini_dup"):
        "ReadParameterDup itself",
    ("kitty/kitty_params.c", "ReadParameterN", "ReadParameterDup"):
        "ReadParameterN itself",
    ("kitty/kitty_params.c", "DelParameter", "delINI"):
        "DelParameter itself",
    ("kitty/kitty_params.c", "load_ini_params", "readINI"):
        "the INIP rows with rdini 1",
    ("kitty/kitty_params.c", "load_ini_params", "ReadParameterN"):
        "the INIP rows with rdini 0",
    ("kitty/kitty_config_shared.c", "kset_read_dup", "kitty_read_ini_dup"):
        "the kset rows with file_only true",
    ("kitty/kitty_config_shared.c", "kset_read_dup", "ReadParameterDup"):
        "the kset rows with file_only false",
    ("kitty/kitty_config_shared.c", "kset_write", "writeINI"):
        "the kset rows with file_only true",
    ("kitty/kitty_config_shared.c", "kset_write", "WriteParameterNoReport"):
        "the kset rows with file_only false",
    ("kitty/kitty_config_shared.c", "kitty_kset_backupcount", "ReadParameterN"):
        "the wrapper kitty_kset_backupcount itself",
    ("kitty/kitty_shortcuts.c", "InitShortcuts", "readINI"):
        "the sc_actions rows",
    ("kitty/kitty_config_app.c", "sc_save_action", "writeINI"):
        "the sc_actions rows (ShortcutActionKey)",
    ("kitty/kitty_shortcuts.c", "ShortcutsIniDup", "kitty_read_ini_dup"):
        "the wrapper ShortcutsIniDup itself",
    ("kitty/kitty_reconnect.c", "kr_action", "ReadParameterN"):
        "the kr_rules rows",
    ("kitty/kitty_console_store.c", "console_read_switch", "kitty_inifile_read"):
        "the wrapper console_read_switch itself",
    ("kitty/kitty_winevent_cfg.c", "kitty_wev_switch_on", "kitty_inifile_read"):
        "the wrapper kitty_wev_switch_on itself",
    ("kitty/kitty_switchnotice.c", "kitty_switchnotice_seconds", "ReadParameterN"):
        "the wrapper kitty_switchnotice_seconds itself",
    ("kitty/kitty_transfer.c", "kt_global_int", "ReadParameterN"):
        "the wrapper kt_global_int itself",
    ("kitty/kitty_xfer.c", "kx_xfer_dir_dup", "ReadParameterDup"):
        "the wrapper kx_xfer_dir_dup itself",
    ("kitty/kitty_xfer.c", "kx_try_stored", "ReadParameterDup"):
        "the wrapper kx_try_stored itself",
    ("kitty/kitty_xfer.c", "adopt_tool_path_if_exists", "WriteParameter"):
        "the wrapper adopt_tool_path_if_exists itself",
    ("kitty/kitty_store.c", "kstore_ini_dup", "kitty_read_ini_dup"):
        "the wrapper kstore_ini_dup itself",
    ("kitty/kitty_store.c", "kitty_store_ending_applied", "readINI"):
        "the wrapper kitty_store_ending_applied itself",
    ("kitty/kitty_store.c", "kitty_store_ending_mark", "writeINI"):
        "the wrapper kitty_store_ending_mark itself",
    ("kitty/kitty_store.c", "kitty_store_ending_mark", "delINI"):
        "the wrapper kitty_store_ending_mark itself",
    ("kitty/kitty_regbackup.c", "retire_ini_value_present", "kitty_read_ini_dup"):
        "the wrapper retire_ini_value_present itself",
    ("kitty/kitty_kageant_settings.c", "kageant_setting_write", "kitty_inifile_write"):
        "the wrapper kageant_setting_write itself",
    ("kitty/kitty_kageant_settings.c", "kageant_ini_read_dup", "kitty_inifile_read"):
        "the wrapper kageant_ini_read_dup itself",
    ("kitty/kitty_kageant_settings.c", "kageant_setting_str_dup", "kageant_ini_read_dup"):
        "the wrapper kageant_setting_str_dup itself",
    ("kitty/kitty_kageant_settings.c", "kageant_setting_str_set", "kitty_inifile_write"):
        "the wrapper kageant_setting_str_set itself",
    ("kitty/kitty_kageant_settings.c", "kageant_bool_get", "kitty_inifile_read"):
        "the wrapper kageant_bool_get itself",
    ("kitty/kitty_kageant_settings.c", "kageant_int_setting", "kitty_inifile_read"):
        "the wrapper kageant_int_setting itself",
    ("kitty/kitty_kageant_settings.c", "kageant_policy_read", "kitty_inifile_read"):
        "the wrapper kageant_policy_read itself",
    ("kitty/kitty_kageant_settings.c", "kageant_policy_get", "kageant_policy_get_def"):
        "the wrapper kageant_policy_get itself",
    ("kitty/kitty_kageant_settings.c", "kageant_policy_get_def", "kageant_policy_read"):
        "the wrapper kageant_policy_get_def itself",
    ("kitty/kitty_kageant_settings.c", "kageant_policy_set", "kageant_setting_write"):
        "the wrapper kageant_policy_set itself",
    ("windows/pageant.c", "kageant_geometry_carry_over", "kageant_setting_str_dup"):
        "the wrapper kageant_geometry_carry_over itself",
    ("windows/pageant.c", "kageant_geometry_carry_over", "kageant_setting_str_set"):
        "the wrapper kageant_geometry_carry_over itself",
}

# A file call's path argument that names the kitty.ini in use.
LIVE_PATHS = ("KittyIniFile", "GetKittyIniFile()", "ini")
# Files where "ini" is another store's kitty.ini.
OTHER_INI_FILES = ("kitty/kitty_storemove.c", "kitty/kitty_migrate.c")
# Sections the tree spells as a macro that is not a plain string.
SECTION_FIXED = {"INIT_SECTION": "KiTTY"}

SKIP_DIRS = ("kitty/mini", "windows/test")

MARKER_RE = re.compile(r"store-pairs:\s*(.*)$", re.S)
DEFINE_STR_RE = re.compile(r'^\s*#\s*define\s+([A-Za-z_]\w*)\s+"([^"\\]*)"',
                           re.M)
DEFINE_ALIAS_RE = re.compile(
    r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(?:/[/*].*)?$", re.M)
DECL_ARG_RE = re.compile(
    r"^\s*(?:const\s+)?(?:unsigned\s+)?(?:char|int|void|size_t|DWORD|LPCTSTR|"
    r"LPTSTR|HKEY|BOOL|bool)\b[\s*]*(?:const\s+)?(?:[A-Za-z_]\w*)?\s*$")
CAST_RE = re.compile(r"^\(\s*(?:const\s+)?(?:char|LPTSTR|LPCTSTR)\s*\*?\s*\)\s*")


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
        relp = "" if rel == "." else rel.replace(os.sep, "/")
        dirs[:] = [x for x in dirs
                   if (relp + "/" + x if relp else x) not in SKIP_DIRS]
        for f in files:
            if f.endswith((".c", ".h")):
                p = os.path.normpath(os.path.join(rel, f))
                out.append(p.replace(os.sep, "/"))
    return sorted(out)


def split_comments(src):
    """(code with comments blanked, code with comments and the inside of
    literals blanked, [(start line, end line, comment text)]). Line
    structure and offsets are kept."""
    keep = []
    blank = []
    comments = []
    i = 0
    n = len(src)
    line = 1
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] in "*/":
            if src[i + 1] == "*":
                j = src.find("*/", i + 2)
                j = n if j < 0 else j + 2
                body = src[i + 2:j - 2]
            else:
                j = src.find("\n", i)
                j = n if j < 0 else j
                body = src[i + 2:j]
            text = src[i:j]
            nl = text.count("\n")
            comments.append((line, line + nl, body))
            b = "".join("\n" if ch == "\n" else " " for ch in text)
            keep.append(b)
            blank.append(b)
            line += nl
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
            keep.append(body)
            blank.append(q + "".join("\n" if ch == "\n" else " "
                                     for ch in body[1:-1])
                         + (q if len(body) > 1 else ""))
            line += body.count("\n")
            i = j
        else:
            keep.append(c)
            blank.append(c)
            if c == "\n":
                line += 1
            i += 1
    return "".join(keep), "".join(blank), comments


def call_args(text, pos):
    """The arguments of the call whose '(' is at pos, split at top-level
    commas; (args, end offset)."""
    depth = 0
    args = []
    cur = []
    i = pos
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            q = c
            j = i + 1
            while j < n and text[j] != q:
                if text[j] == "\\":
                    j += 1
                j += 1
            cur.append(text[i:j + 1])
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
                args.append("".join(cur).strip())
                return args, i
        elif c == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    return args, n


def function_spans(blank):
    """[(start offset, end offset, function name)] of the function bodies of
    one file (top-level braces preceded by a parameter list)."""
    spans = []
    depth = 0
    start = None
    name = None
    for m in re.finditer(r"[{}]", blank):
        if m.group(0) == "{":
            if depth == 0:
                name = None
                j = m.start() - 1
                while j >= 0 and blank[j] in " \t\r\n":
                    j -= 1
                if j >= 0 and blank[j] == ")":
                    d = 0
                    while j >= 0:
                        if blank[j] == ")":
                            d += 1
                        elif blank[j] == "(":
                            d -= 1
                            if d == 0:
                                break
                        j -= 1
                    k = j - 1
                    while k >= 0 and blank[k] in " \t\r\n":
                        k -= 1
                    e = k + 1
                    while k >= 0 and (blank[k].isalnum() or blank[k] == "_"):
                        k -= 1
                    cand = blank[k + 1:e]
                    if cand and cand not in ("if", "while", "for", "switch",
                                             "sizeof", "return"):
                        name = cand
                start = m.start()
            depth += 1
        else:
            depth -= 1
            if depth == 0:
                if name:
                    spans.append((start, m.end(), name))
                name = None
            if depth < 0:
                depth = 0
    return spans


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


# ---------------------------------------------------------------- the tree

class Tree:
    def __init__(self, root, files):
        self.root = root
        self.files = files
        self.keep = {}
        self.blank = {}
        self.comments = {}
        self.strings = {}
        aliases = {}
        for f in files:
            try:
                with open(os.path.join(root, f), encoding="utf-8",
                          errors="replace") as fh:
                    src = fh.read()
            except OSError:
                continue
            keep, blank, comments = split_comments(src)
            self.keep[f] = keep
            self.blank[f] = blank
            self.comments[f] = comments
            for m in DEFINE_STR_RE.finditer(keep):
                self.strings.setdefault(m.group(1), {})[f] = m.group(2)
            for m in DEFINE_ALIAS_RE.finditer(keep):
                aliases.setdefault(m.group(1), {})[f] = m.group(2)
        self.aliases = aliases

    def value(self, name, f):
        """The string a macro stands for, or None."""
        if name in SECTION_FIXED:
            return SECTION_FIXED[name]
        seen = set()
        while name not in seen:
            seen.add(name)
            if name in SECTION_FIXED:
                return SECTION_FIXED[name]
            vals = self.strings.get(name)
            if vals:
                if f in vals:
                    return vals[f]
                if "kitty/kitty_inikeys.h" in vals:
                    return vals["kitty/kitty_inikeys.h"]
                return sorted(vals.values())[0]
            al = self.aliases.get(name)
            if not al:
                return None
            name = al.get(f) or sorted(al.values())[0]
        return None

    def resolve(self, arg, f):
        """A section or key argument: (text, kind) with kind "name" (a known
        name), "null" or "var"."""
        a = CAST_RE.sub("", arg.strip())
        a = a.strip()
        if a in ("NULL", "0"):
            return None, "null"
        m = re.fullmatch(r'"([^"\\]*)"', a)
        if m:
            return m.group(1), "name"
        if re.fullmatch(r"[A-Za-z_]\w*", a):
            v = self.value(a, f)
            if v is not None:
                return v, "name"
        return a, "var"


# ---------------------------------------------------------------- scanning

class Rec:
    __slots__ = ("section", "key", "op", "store", "where", "how", "excl")

    def __init__(self, section, key, op, store, where, how, excl=()):
        self.section = section
        self.key = key
        self.op = op
        self.store = store
        self.where = where
        self.how = how
        self.excl = tuple(excl)


def parse_markers(tree, f):
    """[(start line, end line, [Rec], error or None)]"""
    out = []
    for (l0, l1, body) in tree.comments.get(f, []):
        m = MARKER_RE.search(body)
        if not m:
            continue
        # a marker over several lines: the " * " that starts each next line
        # of a block comment is not part of it
        mlines = m.group(1).split("\n")
        mlines = mlines[:1] + [re.sub(r"^\s*\*(?!/)", "", x) for x in mlines[1:]]
        text = " ".join(" ".join(mlines).split())
        where = "%s:%d" % (f, l0)
        if " - " not in text:
            out.append((l0, l1, [], "%s: marker without \" - <reason>\": %s"
                        % (where, text)))
            continue
        head, reason = text.split(" - ", 1)
        parts = head.split()
        if (len(parts) < 4 or parts[0] not in (R, W)
                or parts[1] not in (PARAM, FILE) or not reason.strip()):
            out.append((l0, l1, [], "%s: not \"store-pairs: <read|write> "
                        "<param|file> <SECTION> <KEY|*> [!KEY ...] - <reason>\""
                        ": %s" % (where, text)))
            continue
        op, store, sec = parts[0], parts[1], parts[2]
        secv = tree.value(sec, f) or sec
        keys = parts[3]
        excl = []
        err = None
        for p in parts[4:]:
            if not p.startswith("!"):
                err = "%s: unexpected \"%s\" in a marker" % (where, p)
                break
            kv = tree.value(p[1:], f) or p[1:]
            excl.append(kv)
        if err:
            out.append((l0, l1, [], err))
            continue
        if excl and keys != "*":
            out.append((l0, l1, [], "%s: \"!KEY\" goes with \"*\" only" % where))
            continue
        for kv in excl:
            src = tree.keep.get(f, "")
            names = [n for n, vals in tree.strings.items()
                     if any(v.lower() == kv.lower() for v in vals.values())]
            if not (re.search(r'"%s"' % re.escape(kv), src, re.I)
                    or any(re.search(r"\b%s\b" % re.escape(n), src)
                           for n in names)):
                err = ("%s: \"!%s\" does not appear in %s: the code no longer "
                       "skips it" % (where, kv, f))
                break
        if err:
            out.append((l0, l1, [], err))
            continue
        recs = []
        for k in keys.split(","):
            if not k:
                continue
            kv = "*" if k == "*" else (tree.value(k, f) or k)
            recs.append(Rec(secv, kv, op, store, where, "marker", excl))
        out.append((l0, l1, recs, None))
    return out


def scan(tree, funcs=FUNCS, wrappers=WRAPPERS, generic=GENERIC):
    """(records, errors, unmarked, generic entries used)"""
    recs = []
    errors = []
    unmarked = []
    used = set()
    names = dict(funcs)
    call_re = re.compile(r"\b(" + "|".join(
        sorted(list(funcs) + list(wrappers), key=len, reverse=True))
        + r")\s*\(")
    for f in tree.files:
        if f not in tree.blank:
            continue
        blank = tree.blank[f]
        keep = tree.keep[f]
        markers = parse_markers(tree, f)
        for (_l0, _l1, mrecs, err) in markers:
            if err:
                errors.append(err)
            recs.extend(mrecs)
        # marker blocks: consecutive markers; the block's last line
        blocks = []
        for (l0, l1, _mr, err) in sorted(markers, key=lambda x: x[0]):
            if err:
                continue
            if blocks and l0 <= blocks[-1][1] + 1:
                blocks[-1][1] = max(blocks[-1][1], l1)
            else:
                blocks.append([l0, l1])
        block_end = set(b[1] for b in blocks)
        spans = function_spans(blank)
        for m in call_re.finditer(blank):
            fn = m.group(1)
            # a member or another identifier ending in the name
            if m.start() > 0 and (blank[m.start() - 1].isalnum()
                                  or blank[m.start() - 1] in "_."):
                continue
            args, _end = call_args(keep, m.end() - 1)
            if any(DECL_ARG_RE.match(a) for a in args) or args == ["void"]:
                continue                       # a prototype or a definition
            ln = line_of(blank, m.start())
            where = "%s:%d" % (f, ln)
            func = ""
            for (s, e, nm) in spans:
                if s <= m.start() < e:
                    func = nm
                    break
            if ln in block_end or (ln - 1) in block_end:
                continue                       # the marker block counts
            if fn in wrappers:
                op, store, secname, ki, _why = wrappers[fn]
                if ki >= len(args):
                    continue
                key, kind = tree.resolve(args[ki], f)
                if kind == "null":
                    continue
                if kind == "var":
                    g = (f, func, fn)
                    if g in generic:
                        used.add(g)
                        continue
                    unmarked.append((where, func, fn, args))
                    continue
                for o in (op if isinstance(op, tuple) else (op,)):
                    recs.append(Rec(secname, key, o, store, where, fn))
                continue
            op, store, si, ki, pi = names[fn]
            if pi is not None:
                if pi >= len(args):
                    continue
                path = " ".join(args[pi].split())
                if path not in LIVE_PATHS:
                    continue
                if path == "ini" and f in OTHER_INI_FILES:
                    continue
            if max(si, ki) >= len(args):
                continue
            sec, skind = tree.resolve(args[si], f)
            key, kkind = tree.resolve(args[ki], f)
            if kkind == "null":
                continue
            if skind != "name" or kkind != "name":
                g = (f, func, fn)
                if g in generic:
                    used.add(g)
                    continue
                unmarked.append((where, func, fn, args))
                continue
            recs.append(Rec(sec, key, op, store, where, fn))
    recs.extend(table_records(tree))
    return recs, errors, unmarked, used


def table_records(tree):
    out = []
    f = "kitty/kitty_params.c"
    if f in tree.keep:
        s = tree.keep[f]
        for m in re.finditer(r"\bINIP_(?:KW|NUM)\s*\(\s*(\w+)\s*,\s*([01])\s*,"
                             r"\s*(\w+)", s):
            sec = tree.value(m.group(1), f)
            key = tree.value(m.group(3), f)
            if sec and key:
                out.append(Rec(sec, key, R,
                               FILE if m.group(2) == "1" else PARAM,
                               "%s:%d" % (f, line_of(s, m.start())),
                               "INIP row"))
    f = "kitty/kitty_config_shared.c"
    if f in tree.keep:
        s = tree.keep[f]
        m0 = re.search(r"\bkset_keys\s*\[\s*\]\s*=\s*\{", s)
        if m0:
            _args, end = call_args(s, m0.end() - 1)
            body = s[m0.end():end]
            for m in re.finditer(r"\{\s*(\w+)\s*,\s*(\w+)\s*,\s*KSET_\w+\s*,"
                                 r"\s*(true|false)\b", body):
                sec = tree.value(m.group(1), f)
                key = tree.value(m.group(2), f)
                if not (sec and key):
                    continue
                st = FILE if m.group(3) == "true" else PARAM
                where = "%s:%d" % (f, line_of(s, m0.end() + m.start()))
                out.append(Rec(sec, key, R, st, where, "kset row"))
                out.append(Rec(sec, key, W, st, where, "kset row"))
    f = "kitty/kitty_shortcuts.c"
    if f in tree.keep:
        s = tree.keep[f]
        for m in re.finditer(r"^\s*SC_ACT\s*\(\s*\w+\s*,\s*(\w+)\s*,", s, re.M):
            key = tree.value(m.group(1), f)
            if key:
                where = "%s:%d" % (f, line_of(s, m.start(1)))
                out.append(Rec("Shortcuts", key, R, FILE, where,
                               "sc_actions row (InitShortcuts, readINI)"))
                out.append(Rec("Shortcuts", key, W, FILE, where,
                               "sc_actions row (Shortcuts panel, writeINI)"))
    f = "kitty/kitty_reconnect.c"
    if f in tree.keep:
        s = tree.keep[f]
        m0 = re.search(r"\bkr_rules\s*\[[^\]]*\]\s*=\s*\{", s)
        if m0:
            _args, end = call_args(s, m0.end() - 1)
            body = s[m0.end():end]
            for m in re.finditer(r"=\s*\{\s*(\w+)\s*,", body):
                key = tree.value(m.group(1), f)
                if key:
                    out.append(Rec("KiTTY", key, R, PARAM, "%s:%d" % (
                        f, line_of(s, m0.end() + m.start())),
                        "kr_rules row (kr_action, ReadParameterN)"))
    return out


# ---------------------------------------------------------------- verdict

def collect(recs):
    """{(section, key) lower: {"name": (section, key), R: {store: [rec]},
    W: {...}}} with the "*" markers applied."""
    keys = OrderedDict()

    def slot(sec, key):
        k = (sec.lower(), key.lower())
        if k not in keys:
            keys[k] = {"name": (sec, key), R: {}, W: {}}
        return keys[k]

    stars = []
    for r in recs:
        if r.key == "*":
            stars.append(r)
        slot(r.section, r.key)[r.op].setdefault(r.store, []).append(r)
    for r in stars:
        excl = set(x.lower() for x in r.excl)
        for (s, k), e in keys.items():
            if s == r.section.lower() and k != "*" and k not in excl:
                e[r.op].setdefault(r.store, []).append(r)
    return keys


def findings_of(keys):
    """[(id, text lines)]: store mismatches and flat-name collisions."""
    out = []
    for (s, k), e in keys.items():
        rd = frozenset(e[R])
        wr = frozenset(e[W])
        bad = None
        if rd and wr and rd != wr:
            bad = "read from %s, written to %s" % (
                " and ".join(sorted(rd)), " and ".join(sorted(wr)))
        elif rd == BOTH and wr != BOTH:
            bad = "read from both stores, written to %s" % (
                " and ".join(sorted(wr)) if wr else "neither")
        if bad:
            sec, key = e["name"]
            lines = ["[%s] %s: %s" % (sec, key, bad)]
            for op in (R, W):
                for st in sorted(e[op]):
                    for r in e[op][st]:
                        lines.append("    %-5s %-5s %s (%s)" % (op, st, r.where,
                                                                r.how))
            out.append(((sec.lower(), key.lower()), lines))
    by_name = OrderedDict()
    for (s, k), e in keys.items():
        if k == "*":
            continue
        if PARAM in e[R] or PARAM in e[W]:
            by_name.setdefault(k, []).append(e)
    for k, es in by_name.items():
        secs = sorted(set(e["name"][0].lower() for e in es))
        if len(secs) < 2:
            continue
        lines = ["flat name \"%s\": a param key of %s - one registry value" % (
            es[0]["name"][1], " and ".join("[%s]" % e["name"][0] for e in es))]
        for e in es:
            for op in (R, W):
                for r in e[op].get(PARAM, []):
                    lines.append("    [%s] %-5s %s (%s)" % (
                        e["name"][0], op, r.where, r.how))
        out.append((("flat", k), lines))
    return out


def read_allow(path):
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
        if "|" not in line:
            errors.append("%s:%d: not \"<SECTION> <KEY> | <reason>\": %s"
                          % (path, no, line))
            continue
        head, reason = line.split("|", 1)
        parts = head.split()
        if len(parts) != 2 or not reason.strip():
            errors.append("%s:%d: not \"<SECTION> <KEY> | <reason>\": %s"
                          % (path, no, line))
            continue
        entries.append((no, parts[0].lower(), parts[1].lower(), line))
    return entries, errors


SECTION_TABLE_FILE = "kitty/kitty_storemove.c"
SECTION_TABLE_NAME = "ksm_section_keys"
SECTION_TABLE_SECTIONS = ("configbox", "launcher")


def section_table_errors(tree, keys):
    """ksm_section_keys[] of kitty_storemove.c holds exactly the param keys
    of [ConfigBox] and [Launcher]: a registry-to-folder copy puts a flat hive
    value into its section by that list."""
    src = tree.keep.get(SECTION_TABLE_FILE)
    if src is None:
        return []
    m = re.search(SECTION_TABLE_NAME + r"\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        return ["%s: the table %s[] is missing" % (SECTION_TABLE_FILE,
                                                   SECTION_TABLE_NAME)]
    have = set()
    errs = []
    for sec, key in re.findall(r"\{\s*(\w+)\s*,\s*(\w+)\s*\}", m.group(1)):
        s = tree.value(sec, SECTION_TABLE_FILE)
        k = tree.value(key, SECTION_TABLE_FILE)
        if s is None or k is None:
            errs.append("%s: %s[] row { %s, %s } is not a known name"
                        % (SECTION_TABLE_FILE, SECTION_TABLE_NAME, sec, key))
            continue
        have.add((s.lower(), k.lower()))
    want = set()
    for (s, k), e in keys.items():
        if s in SECTION_TABLE_SECTIONS and k != "*" and (
                PARAM in e[R] or PARAM in e[W]):
            want.add((s, k))
    for s, k in sorted(want - have):
        errs.append("%s: %s[] lacks [%s] %s (a registry setting: a copy into "
                    "a folder store would put it in [KiTTY])"
                    % (SECTION_TABLE_FILE, SECTION_TABLE_NAME, s, k))
    for s, k in sorted(have - want):
        errs.append("%s: %s[] lists [%s] %s, which is no registry setting"
                    % (SECTION_TABLE_FILE, SECTION_TABLE_NAME, s, k))
    return errs


def check(root, allow_path, funcs=FUNCS, wrappers=WRAPPERS, generic=GENERIC):
    files = source_files(root)
    tree = Tree(root, files)
    recs, errors, unmarked, used = scan(tree, funcs, wrappers, generic)
    keys = collect(recs)
    errors.extend(section_table_errors(tree, keys))
    found = findings_of(keys)
    entries, aerr = read_allow(allow_path)
    errors.extend(aerr)
    allowed = set((e[1], e[2]) for e in entries)
    open_f = [(i, t) for (i, t) in found if i not in allowed]
    ids = set(i for (i, _t) in found)
    stale = [e for e in entries if (e[1], e[2]) not in ids]
    stale_generic = sorted(g for g in generic
                           if g not in used and g[0] in tree.keep)
    return {"files": len(files), "keys": keys, "findings": open_f,
            "allowed": len(found) - len(open_f), "stale": stale,
            "errors": errors, "unmarked": unmarked,
            "stale_generic": stale_generic, "records": recs}


def report(res, allow_rel, out):
    for e in res["errors"]:
        out.write("check-store-pairs: %s\n" % e)
    if res["unmarked"]:
        out.write("check-store-pairs: %d call%s with a key the check cannot "
                  "place:\n\n" % (len(res["unmarked"]),
                                  "" if len(res["unmarked"]) == 1 else "s"))
        for (where, func, fn, args) in res["unmarked"]:
            out.write("  %s: %s(%s)%s\n" % (where, fn, ", ".join(args),
                                             " in " + func + "()" if func
                                             else ""))
        out.write("\nPut a marker on the line or the line above, with the "
                  "section and the keys that\nreach this call (\"*\" when the "
                  "set is open):\n    /* store-pairs: <read|write> "
                  "<param|file> <SECTION> <KEY|*> - <reason> */\n\n")
    if res["findings"]:
        out.write("check-store-pairs: %d global setting%s not read from the "
                  "store it is written to:\n\n" % (
                      len(res["findings"]),
                      "" if len(res["findings"]) == 1 else "s"))
        for (_i, lines) in res["findings"]:
            for ln in lines:
                out.write("  %s\n" % ln)
            out.write("\n")
        out.write("Read and write a key through the same store: param "
                  "(ReadParameter*/WriteParameter)\nor file (readINI/writeINI "
                  "on kitty.ini). Two param keys need two registry\nnames. A "
                  "kept exception is one line in %s:\n    <SECTION> <KEY> | "
                  "<reason>      (a flat name: FLAT <KEY> | <reason>)\n\n"
                  % allow_rel)
    if res["stale"]:
        out.write("check-store-pairs: %d line%s in %s match%s no finding any "
                  "more:\n\n" % (len(res["stale"]),
                                 "" if len(res["stale"]) == 1 else "s",
                                 allow_rel,
                                 "es" if len(res["stale"]) == 1 else ""))
        for e in res["stale"]:
            out.write("  %s:%d: %s\n" % (allow_rel, e[0], e[3]))
        out.write("\nRemove the line.\n\n")
    if res["stale_generic"]:
        out.write("check-store-pairs: %d GENERIC entr%s of the check match no "
                  "call any more:\n\n" % (
                      len(res["stale_generic"]),
                      "y" if len(res["stale_generic"]) == 1 else "ies"))
        for g in res["stale_generic"]:
            out.write("  %s: %s() calling %s\n" % g)
        out.write("\nCorrect or remove the entry in tools/check-store-pairs.py."
                  "\n\n")


def run(root, allow_path, out=sys.stdout, quiet=False, **kw):
    res = check(root, allow_path, **kw)
    allow_rel = os.path.relpath(allow_path, root).replace(os.sep, "/")
    if allow_rel.startswith(".."):
        allow_rel = allow_path
    report(res, allow_rel, out)
    bad = (res["findings"] or res["stale"] or res["errors"]
           or res["unmarked"] or res["stale_generic"])
    if bad:
        return 1
    if not quiet:
        n = sum(1 for k in res["keys"] if k[1] != "*")
        out.write("check-store-pairs: %d files, %d global settings, each read "
                  "from the store it is written to (%d kept in %s)\n"
                  % (res["files"], n, res["allowed"], allow_rel))
    return 0


# ---------------------------------------------------------------- self-test

PLANT_KEYS = r"""
#define KI_SECTION_KITTY "KiTTY"
#define KI_SECTION_CONFIGBOX "ConfigBox"
#define KI_SECTION_LAUNCHER "Launcher"
#define INIT_SECTION KI_SECTION_KITTY
#define KI_MISMATCH "mismatch"
#define KI_CLEAN "clean"
#define KI_FILEONLY "fileonly"
#define KI_CB_DBL "dblclick"
#define KI_LA_DBL "dblclick"
#define KI_MARKED "marked"
#define KI_BOTHREAD "bothread"
"""
PLANT_A = r"""
#include "keys.h"
int ReadParameterN(const char *key, const char *name, char *value, size_t size);
void a(void)
{
    char buf[16];
    WriteParameter(INIT_SECTION, KI_MISMATCH, "yes");
    readINI(KittyIniFile, INIT_SECTION, KI_MISMATCH, buf, sizeof(buf));
    WriteParameter(KI_SECTION_KITTY, KI_CLEAN, "yes");
    ReadParameterN("KiTTY", KI_CLEAN,
                   buf, sizeof(buf));
    writeINI(GetKittyIniFile(), INIT_SECTION, KI_FILEONLY, "x");
    readINI(KittyIniFile, INIT_SECTION, KI_FILEONLY, buf, sizeof(buf));
    WriteParameter(KI_SECTION_CONFIGBOX, KI_CB_DBL, "open");
    WriteParameter(KI_SECTION_LAUNCHER, KI_LA_DBL, "config");
    /* ReadParameterN(INIT_SECTION, KI_FILEONLY, buf, sizeof(buf)); */
    readINI(otherfile, INIT_SECTION, KI_CLEAN, buf, sizeof(buf));
    ReadParameterN(INIT_SECTION, KI_BOTHREAD, buf, sizeof(buf));
    kitty_inifile_read(INIT_SECTION, KI_BOTHREAD, buf, sizeof(buf));
}
"""
PLANT_B = r"""
#include "keys.h"
static void b(const char *key)
{
    char buf[16];
    ReadParameterN(INIT_SECTION, key, buf, sizeof(buf));
    /* store-pairs: write param KiTTY KI_MARKED - a planted marker */
    WriteParameter(INIT_SECTION, key, "1");
    /* store-pairs: read param KiTTY marked - its reading */
    kitty_inifile_read(INIT_SECTION, KI_MARKED, buf, sizeof(buf));
}
static void generic_one(const char *key)
{
    char buf[16];
    ReadParameterN(INIT_SECTION, key, buf, sizeof(buf));
}
"""
PLANT_C = r"""
#include "keys.h"
static void c(const char *key)
{
    if (!strcmp(key, KI_FILEONLY)) return;
    /* store-pairs: write file KiTTY *
     *   !fileonly - every [KiTTY] key but one, a marker over two lines */
    writeINI(ini, INIT_SECTION, key, "1");
}
"""


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

    root = tempfile.mkdtemp(prefix="kitty_storepairs_selftest_")
    gen = {("src/b.c", "generic_one", "ReadParameterN"): "planted"}
    try:
        os.makedirs(os.path.join(root, "src"))
        os.makedirs(os.path.join(root, "kitty", "mini"))
        for name, body in (("src/keys.h", PLANT_KEYS), ("src/a.c", PLANT_A),
                           ("src/b.c", PLANT_B),
                           ("kitty/mini/m.c",
                            "void m(void){ WriteParameter(INIT_SECTION, x, y); }")):
            with open(os.path.join(root, name), "w") as fh:
                fh.write(body)

        def verdict(lines, label, generic=gen, extra=None):
            path = os.path.join(root, "allow_%s.txt" % label)
            with open(path, "w") as fh:
                fh.write("# planted list\n\n" + "\n".join(lines) + "\n")
            cpath = os.path.join(root, "src", "c.c")
            if extra:
                with open(cpath, "w") as fh:
                    fh.write(extra)
            elif os.path.exists(cpath):
                os.remove(cpath)
            buf = io.StringIO()
            rc = run(root, path, out=buf, quiet=True, generic=generic)
            return rc, buf.getvalue(), check(root, path, generic=generic)

        full = ["KiTTY mismatch | planted", "FLAT dblclick | planted",
                "KiTTY bothread | planted"]
        rc, text, res = verdict(full, "full")
        unm = [u[0] for u in res["unmarked"]]
        ok(unm == ["src/b.c:6"],
           "an unmarked variable key fails, with file:line (and only it)")
        if unm != ["src/b.c:6"]:
            print(text)
        ok(not res["findings"] and not res["stale"] and not res["errors"],
           "with the planted allow lines nothing else is reported")
        k = res["keys"]
        ok(set(k[("kitty", "clean")][R]) == {PARAM} and
           set(k[("kitty", "clean")][W]) == {PARAM},
           "a clean pair: a call over two lines, a literal section, another "
           "file's path not counted")
        ok(set(k[("kitty", "fileonly")][R]) == {FILE},
           "a commented-out call does not count")
        ok(set(k[("kitty", "marked")][R]) == {PARAM} and
           set(k[("kitty", "marked")][W]) == {PARAM},
           "a marker replaces the call below it (a file call read as param)")
        ok(not any(r.where.startswith("kitty/mini/") for r in res["records"]),
           "kitty/mini/ is not read")
        ok(not any(r.where == "src/a.c:3" for r in res["records"]),
           "a prototype is not a call")

        rc, text, res = verdict([x for x in full if "mismatch" not in x],
                                "nomismatch")
        ok(rc == 1 and "[KiTTY] mismatch: read from file, written to param"
           in text and "src/a.c:7" in text and "src/a.c:8" in text,
           "a key written to param and read from file fails, with both sites")

        rc, text, res = verdict([x for x in full if "FLAT" not in x], "noflat")
        ok(rc == 1 and "flat name \"dblclick\"" in text and "[ConfigBox]" in text
           and "[Launcher]" in text,
           "two param keys of different sections with one name fail")

        rc, text, res = verdict([x for x in full if "bothread" not in x],
                                "noboth")
        ok(rc == 1 and "read from both stores, written to neither" in text,
           "a key read from both stores and written to neither fails")

        rc, text, res = verdict(full + ["KiTTY gone | planted"], "stale")
        ok(rc == 1 and "no finding any" in text and "KiTTY gone" in text,
           "an allow line that matches no finding fails (stale)")

        rc, text, res = verdict(full, "nogeneric", generic={})
        ok(rc == 1 and "src/b.c:15" in text,
           "without its GENERIC entry the accessor's variable key fails")

        rc, text, res = verdict(full, "stalegeneric", generic=dict(
            list(gen.items())
            + [(("src/b.c", "gone", "ReadParameterN"), "planted")]))
        ok(rc == 1 and "GENERIC entr" in text and "gone()" in text,
           "a GENERIC entry that matches no call fails")

        rc, text, res = verdict(full, "star", extra=PLANT_C)
        ok(rc == 1 and "[KiTTY] clean: read from param, written to file and "
           "param" in text and "[KiTTY] fileonly" not in text,
           "a \"*\" marker applies to every key of its section but the "
           "\"!\" ones")

        rc, text, res = verdict(full, "badexcl", extra=PLANT_C.replace(
            "!fileonly", "!nosuchkey"))
        ok(rc == 1 and "does not appear in" in text,
           "a \"!KEY\" the marker's file never uses fails")

        rc, text, res = verdict(full, "badmarker", extra=PLANT_C.replace(
            " - every", " every"))
        ok(rc == 1 and "marker without" in text, "a marker without a reason "
           "fails")

        rc, text, res = verdict(full + ["KiTTY"], "badline")
        ok(rc == 1 and "not \"<SECTION> <KEY> | <reason>\"" in text,
           "a malformed allow line fails")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("\ncheck-store-pairs self-test: %d checks, %d failed"
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
        allow = os.path.join(root, "tools", "store-pairs.allow")
    if mode == "self-test":
        return self_test()
    if mode == "list":
        res = check(root, allow)
        for (_s, _k), e in sorted(res["keys"].items()):
            print("[%s] %s  read: %s  write: %s" % (
                e["name"][0], e["name"][1],
                ",".join(sorted(e[R])) or "-", ",".join(sorted(e[W])) or "-"))
        return 0
    return run(root, allow)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
