#!/usr/bin/env python3
"""Every command of the terminal window's menus has a shortcut action, and
every action's menu command is on a menu.

The shortcut editor (Application > KiTTY++ Settings > Keys & Mouse >
Shortcuts) lists the rows of sc_actions[] in kitty/kitty_shortcuts.c. Each
row is

    SC_ACT( field, key, dflt, name, idm, run, oldkey )

and idm is the menu command the action runs (0 = none). A menu item of that
command shows the action's key after a tab (MenuShortcut). run SC_MENU:
ManageShortcuts sends WM_COMMAND idm from the table, so idm must not be 0. A menu command
added without an action is a command no key can reach. An action whose idm
is on no menu shows its key nowhere and may run a command nothing else
offers. Both used to be found only by reading the menu.

Where the menu commands come from: the AppendMenu, InsertMenu and ModifyMenu
calls (A and W forms too) in windows/window.c and the kitty menu builders it
calls (MENU_SOURCES below). Not counted:
- items whose flags hold MF_SEPARATOR or MF_POPUP (a separator, a submenu),
- commented-out calls,
- ids inside a reserved range: the RANGES of tools/check-menu-ids.py and the
  window.c-local spans in LOCAL_RANGES (saved sessions, special commands).
  An id expression that starts from a range base is that range.
- an id that is a variable, only in the files of VARIABLE_IDS (with the
  reason there). A variable id anywhere else fails: the check cannot place it.

The check fails when:
- a menu command has no action and no line in the allow file,
- an action's idm is on no menu and has no line in the allow file,
- two actions share one idm (MenuShortcut takes the first),
- an SC_MENU action has idm 0,
- an allow line is stale: its command has an action AND a menu item, or
  neither, or the line is there twice.

tools/menu-shortcuts.allow, one line per command without a partner:
    IDM_NAME | reason

Run:  python3 tools/check-menu-shortcuts.py               (exit 1 on a finding)
      python3 tools/check-menu-shortcuts.py --self-test   (planted mistakes)
      python3 tools/check-menu-shortcuts.py --list        (every menu command,
                                                          its text and action)
Options: --root DIR (source tree), --allow FILE.
"""
import importlib.util
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)


def _menu_id_ranges():
    """RANGES of tools/check-menu-ids.py: one list of reserved spans."""
    spec = importlib.util.spec_from_file_location(
        "check_menu_ids", os.path.join(HERE, "check-menu-ids.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return dict(mod.RANGES)


# Range bases defined in windows/window.c itself (check-menu-ids.py reads the
# shared headers only). base -> why
LOCAL_RANGES = {
    "IDM_SAVED_MIN": "saved sessions, IDM_SAVED_MIN + n * MENU_SAVED_STEP",
    "IDM_SPECIAL_MIN": "special commands, IDM_SPECIAL_MIN + 0x10 * i",
}

MENU_SOURCES = [
    "windows/window.c",
    "kitty/kitty_specialmenu.c",   # InitSpecialMenu: the User Command menu
    "kitty/kitty_win.c",           # kitty_sync_transparency_menu
]
# file -> why its variable ids are known
VARIABLE_IDS = {
    "kitty/kitty_win.c": "kitty_sync_transparency_menu re-inserts the ids "
                         "window.c passes (IDM_TRANSPARUP, IDM_TRANSPARDOWN)",
}
ACTIONS = "kitty/kitty_shortcuts.c"
ALLOW = os.path.join("tools", "menu-shortcuts.allow")

# name -> (flags arg, id arg)
CALLS = {}
for n in ("AppendMenu", "AppendMenuA", "AppendMenuW"):
    CALLS[n] = (1, 2)
for n in ("InsertMenu", "InsertMenuA", "InsertMenuW",
          "ModifyMenu", "ModifyMenuA", "ModifyMenuW"):
    CALLS[n] = (2, 3)


def strip_comments(s):
    """Comments out, strings and line numbers kept."""
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and s[j] != c:
                j += 2 if s[j] == "\\" else 1
            out.append(s[i:j + 1])
            i = j + 1
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", s[i:j]))
            i = j
        elif s.startswith("//", i):
            j = s.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def call_args(s, open_paren):
    """The top-level arguments of the call whose '(' is at open_paren."""
    args, depth, cur, i = [], 0, [], open_paren + 1
    while i < len(s):
        c = s[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < len(s) and s[j] != c:
                j += 2 if s[j] == "\\" else 1
            cur.append(s[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                args.append("".join(cur).strip())
                return args
            depth -= 1
        elif c == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    return args


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8", errors="surrogateescape") as f:
        return f.read()


def menu_commands(root, ranges, problems):
    """IDM name -> list of (file:line, text) of the menu items with that id."""
    found = {}
    names = "|".join(sorted(CALLS, key=len, reverse=True))
    for rel in MENU_SOURCES:
        s = strip_comments(read(root, rel))
        for m in re.finditer(r"\b(%s)\s*\(" % names, s):
            fa, ia = CALLS[m.group(1)]
            args = call_args(s, m.end() - 1)
            where = "%s:%d" % (rel, s.count("\n", 0, m.start()) + 1)
            if len(args) <= ia:
                continue                # a prototype or a wrapper's own text
            flags, idexpr = args[fa], args[ia]
            if re.search(r"\bMF_(SEPARATOR|POPUP)\b", flags):
                continue
            ids = re.findall(r"\bIDM_\w+\b", idexpr)
            if not ids:
                if rel in VARIABLE_IDS:
                    continue
                problems.append("%s: menu item id '%s' is no IDM_ name: the check "
                                "cannot place it" % (where, idexpr))
                continue
            if ids[0] in ranges:
                continue
            if not re.fullmatch(r"IDM_\w+", idexpr):
                problems.append("%s: menu item id '%s' is an expression on no "
                                "reserved range" % (where, idexpr))
                continue
            text = args[ia + 1] if len(args) > ia + 1 else ""
            found.setdefault(idexpr, []).append((where, text))
    return found


def action_idms(root, problems):
    """IDM name -> list of (file:line, action field) of sc_actions[] rows."""
    s = strip_comments(read(root, ACTIONS))
    found = {}
    rows = 0
    for m in re.finditer(r"^\s*SC_ACT\s*\(", s, re.M):
        args = call_args(s, m.end() - 1)
        where = "%s:%d" % (ACTIONS, s.count("\n", 0, m.start()) + 1)
        if len(args) != 7:
            problems.append("%s: an SC_ACT row with %d columns, 7 expected "
                            "(field, key, dflt, name, idm, run, oldkey)" % (where, len(args)))
            continue
        rows += 1
        idm = args[4]
        if args[5] not in ("SC_CODE", "SC_MENU"):
            problems.append("%s: action %s: run '%s' is neither SC_CODE nor SC_MENU"
                            % (where, args[0], args[5]))
        if idm == "0":
            if args[5] == "SC_MENU":
                problems.append("%s: action %s runs its menu command (SC_MENU) "
                                "but has none (idm 0)" % (where, args[0]))
            continue
        if not re.fullmatch(r"IDM_\w+", idm):
            problems.append("%s: action %s: idm '%s' is neither 0 nor an IDM_ name"
                            % (where, args[0], idm))
            continue
        found.setdefault(idm, []).append((where, args[0]))
    if rows == 0:
        problems.append("%s: no SC_ACT rows found" % ACTIONS)
    return found


def read_allow(path, problems):
    allow = {}
    if not os.path.exists(path):
        return allow
    with open(path, encoding="utf-8") as f:
        for no, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.fullmatch(r"(IDM_\w+)\s*\|\s*(\S.*)", line)
            if not m:
                problems.append("%s:%d: not 'IDM_NAME | reason': %s" % (path, no, line))
                continue
            if m.group(1) in allow:
                problems.append("%s:%d: %s is listed twice" % (path, no, m.group(1)))
                continue
            allow[m.group(1)] = (no, m.group(2))
    return allow


def check(root, allow_path):
    problems = []
    ranges = dict(_menu_id_ranges())
    ranges.update(LOCAL_RANGES)
    menu = menu_commands(root, ranges, problems)
    acts = action_idms(root, problems)
    allow = read_allow(allow_path, problems)
    for idm, rows in sorted(acts.items()):
        if len(rows) > 1:
            problems.append("%s is the menu command of %d actions (%s): the menu "
                            "shows the key of the first only"
                            % (idm, len(rows), ", ".join(r[1] for r in rows)))
    for idm in sorted(menu):
        if idm not in acts and idm not in allow:
            where, text = menu[idm][0]
            problems.append("%s: menu command %s %s has no shortcut action: give it an "
                            "SC_ACT row or a line in %s" % (where, idm, text, ALLOW))
    for idm in sorted(acts):
        if idm not in menu and idm not in allow:
            where, field = acts[idm][0]
            problems.append("%s: action %s runs %s, which no menu offers: a line in %s "
                            "with the reason, or idm 0" % (where, field, idm, ALLOW))
    for idm, (no, _reason) in sorted(allow.items()):
        if idm in menu and idm in acts:
            problems.append("%s:%d: stale: %s has an action and a menu item now"
                            % (allow_path, no, idm))
        elif idm not in menu and idm not in acts:
            problems.append("%s:%d: stale: %s is on no menu and no action runs it"
                            % (allow_path, no, idm))
    return problems, menu, acts, allow


def list_all(root, allow_path):
    problems, menu, acts, allow = check(root, allow_path)
    for idm in sorted(set(menu) | set(acts)):
        text = " ".join(menu[idm][0][1].split()) if idm in menu else "(no menu item)"
        act = ", ".join(r[1] for r in acts[idm]) if idm in acts else "-"
        why = allow[idm][1] if idm in allow else ""
        print("%-24s %-40s %-18s %s" % (idm, text, act, why))
    return 1 if problems else 0


# ---------------------------------------------------------------- self-test

SELF_WINDOW = r'''
#define IDM_SAVED_MIN 0x1000
#define IDM_SPECIAL_MIN 0x0400
void build(HMENU m, HMENU sub) {
    AppendMenu(m, MF_ENABLED, IDM_SHOWLOG, "&Event Log");
    AppendMenu(m, MF_SEPARATOR, 0, 0);
    /* AppendMenu(m, MF_ENABLED, IDM_NEWSESS, "Ne&w Session..."); */
    // AppendMenu(m, MF_ENABLED, IDM_HIDDEN, "Hidden");
    AppendMenu(m, MF_POPUP | MF_ENABLED, (UINT_PTR)sub, "Sa&ved Sessions");
    AppendMenu(m, MF_ENABLED, IDM_SAVED_MIN + (i-1)*MENU_SAVED_STEP, name);
    AppendMenu(m, MF_ENABLED, IDM_SPECIAL_MIN + 0x10 * i, name);
    InsertMenu(m, IDM_SHOWLOG, MF_BYCOMMAND | MF_ENABLED, IDM_RESTART, "&Restart Session");
    InsertMenu(m, IDM_SHOWLOG, MF_BYCOMMAND | MF_SEPARATOR, IDM_SPECIALSEP, 0);
    AppendMenu(m, MF_ENABLED, IDM_QUIT, "E&xit");
    AppendMenu(m, MF_ENABLED,
               IDM_PRINT, label("Print"));
%(extra_window)s
}
'''
SELF_SPECIAL = r'''
void sm(HMENU menu, int nb) { AppendMenu( menu, MF_ENABLED, IDM_USERCMD+nb, buffer ) ; }
'''
SELF_KWIN = r'''
void sync(HMENU menu, UINT id_up) { InsertMenu(menu, anchor, MF_BYPOSITION, id_up, "Up"); }
%(extra_kwin)s
'''
SELF_ACTIONS = r'''
static const struct sc_action sc_actions[] = {
	SC_ACT( eventlog, KI_SC_EVENTLOG, 0, KT_SC_ACT_EVENTLOG, IDM_SHOWLOG, SC_MENU, NULL ),
	SC_ACT( print,    KI_SC_PRINT,    SHIFTKEY+VK_F7, KT_SC_ACT_PRINT, IDM_PRINT, SC_CODE, NULL ),
	SC_ACT( duplicate, KI_SC_DUPLICATE, CONTROLKEY+ALTKEY+'T', KT_SC_ACT_DUPLICATE, 0, SC_CODE, NULL ),
	SC_ACT( opennew,  KI_SC_OPENNEW,  0, KT_SC_ACT_OPENNEW, IDM_NEWSESS, SC_CODE, "old" ),
%(extra_actions)s
} ;
'''
SELF_ALLOW = '''# test
IDM_QUIT | the program's end
IDM_RESTART | PuTTY's own, at connection close
IDM_NEWSESS | menu line hidden on purpose
%(extra_allow)s
'''


def self_test():
    cases = [
        ("clean", {}, None),
        ("a menu command without an action",
         {"extra_window": '    AppendMenu(m, MF_ENABLED, IDM_NEW, "New");'},
         "menu command IDM_NEW"),
        ("an action whose idm is on no menu",
         {"extra_actions": "\tSC_ACT( x, KI_X, 0, KT_X, IDM_NOWHERE, SC_CODE, NULL ),"},
         "runs IDM_NOWHERE, which no menu offers"),
        ("a stale allow line: the command got an action",
         {"extra_allow": "IDM_PRINT | none yet"},
         "stale: IDM_PRINT has an action and a menu item"),
        ("a stale allow line: the command is gone",
         {"extra_allow": "IDM_GONE | removed"},
         "stale: IDM_GONE is on no menu"),
        ("an allow line twice",
         {"extra_allow": "IDM_QUIT | again"},
         "IDM_QUIT is listed twice"),
        ("two actions on one idm",
         {"extra_actions": "\tSC_ACT( y, KI_Y, 0, KT_Y, IDM_PRINT, SC_CODE, NULL ),"},
         "IDM_PRINT is the menu command of 2 actions"),
        ("a variable id outside VARIABLE_IDS",
         {"extra_window": '    AppendMenu(m, MF_ENABLED, someid, "Var");'},
         "is no IDM_ name"),
        ("an id expression on no range",
         {"extra_window": '    AppendMenu(m, MF_ENABLED, IDM_PRINT + 1, "Plus");'},
         "an expression on no reserved range"),
        ("an SC_ACT row with the old six columns",
         {"extra_actions": "\tSC_ACT( z, KI_Z, 0, KT_Z, 0, NULL ),"},
         "7 expected"),
        ("an SC_MENU action without a menu command",
         {"extra_actions": "\tSC_ACT( w, KI_W, 0, KT_W, 0, SC_MENU, NULL ),"},
         "but has none (idm 0)"),
        ("a variable id inside VARIABLE_IDS passes",
         {"extra_kwin": 'void s2(HMENU menu, UINT id_down) { InsertMenu(menu, 1, MF_BYPOSITION, id_down, "Down"); }'},
         None),
    ]
    failed = 0
    for name, extra, want in cases:
        tmp = tempfile.mkdtemp(prefix="menu-shortcuts-")
        try:
            ex = {"extra_window": "", "extra_actions": "", "extra_allow": "", "extra_kwin": ""}
            ex.update(extra)
            for rel, text in (("windows/window.c", SELF_WINDOW),
                              ("kitty/kitty_specialmenu.c", SELF_SPECIAL),
                              ("kitty/kitty_win.c", SELF_KWIN),
                              (ACTIONS, SELF_ACTIONS),
                              (ALLOW, SELF_ALLOW)):
                p = os.path.join(tmp, rel)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w", encoding="utf-8") as f:
                    f.write(text % ex)
            problems = check(tmp, os.path.join(tmp, ALLOW))[0]
            if want is None:
                ok = not problems
            else:
                ok = any(want in p for p in problems)
            if not ok:
                failed += 1
                print("  FAIL %s: %s" % (name, problems or "no finding"), file=sys.stderr)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print("check-menu-shortcuts self-test: %d checks, %d failed" % (len(cases), failed))
    return 1 if failed else 0


def main(argv):
    root, allow, mode = DEFAULT_ROOT, None, "check"
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--self-test":
            mode = "self"
        elif a == "--list":
            mode = "list"
        elif a == "--root" and i + 1 < len(argv):
            i += 1
            root = argv[i]
        elif a == "--allow" and i + 1 < len(argv):
            i += 1
            allow = argv[i]
        else:
            print("usage: check-menu-shortcuts.py [--self-test|--list] [--root DIR] [--allow FILE]",
                  file=sys.stderr)
            return 2
        i += 1
    if mode == "self":
        return self_test()
    allow = allow or os.path.join(root, ALLOW)
    if mode == "list":
        return list_all(root, allow)
    problems, menu, acts, allow_lines = check(root, allow)
    if problems:
        print("check-menu-shortcuts FAILED:\n", file=sys.stderr)
        for p in problems:
            print("  - " + p, file=sys.stderr)
        print("\nSee the comment at the top of tools/check-menu-shortcuts.py.", file=sys.stderr)
        return 1
    print("check-menu-shortcuts: %d menu commands, %d with an action. %d actions with a "
          "menu command. %d kept in %s" % (len(menu), len(set(menu) & set(acts)),
                                           len(acts), len(allow_lines), ALLOW))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
