#!/usr/bin/env python3
"""Check the switches of docs/AGENTS-GUIDE.md against the tools' usage text.

The page is read by AI agents, which run its command lines as written. A
switch that a tool does not know ends the run with "unknown option", or
worse, is taken as a host name. So every switch a command line of the page
passes to klink, kscp, ksftp, kitty.exe or kageant must be in that tool's
usage text in the source:

  klink      usage() in windows/plink.c
  kscp       usage() in pscp.c
  ksftp      usage() in psftp.c
  kitty.exe  default_help_file_content in kitty/kitty_help.h (it lists only
             the KiTTY++ additions: a command line with PuTTY's own
             switches, -ssh, -P, -l, fails here on purpose)
  kageant    KT_KA_CLI_HELP in kitty/kitty_text.h

A KT_* macro printed by a usage() (KT_CLI_USAGE_AGENT, ...) is read from
kitty/kitty_text.h.

What is checked:

- only lines inside fenced code blocks (``` ... ```). The "never" lines
  are prose: a switch listed there as forbidden (-pw, -unsafe, -be) is not
  a use.
- a command line is split at "|". A part is an invocation when it starts
  with the tool's name, optionally as a quoted path ending in the .exe and
  optionally after "& ". An assignment such as $env:GIT_SSH = "...klink.exe"
  is no invocation. A PowerShell variable assigned the output of an
  unquoted tool call ($out = klink ...) is one: the "$name = " in front is
  dropped.
- quoted text in an invocation ("<command>", '...') is the remote command
  or a value, not a switch, and is skipped.
- a switch is a token that starts with "-" or "--" and a letter or digit.

The run fails when a usage text cannot be found, when the page has no
invocation at all (the parser no longer matches the page) or on any switch
missing from its tool's usage text.

A self-test plants a miss and a clean twin of each rule, in text held in
memory, before the page is checked. A rule that no longer fires fails the
run.

Run:  python3 tools/check-agents-guide.py              (exit 1 on any miss)
      python3 tools/check-agents-guide.py --self-test  (the self-test alone)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGE = ROOT / "docs" / "AGENTS-GUIDE.md"
TEXT_H = ROOT / "kitty" / "kitty_text.h"
HELP_H = ROOT / "kitty" / "kitty_help.h"
USAGE_FILES = {
    "klink": ROOT / "windows" / "plink.c",
    "kscp": ROOT / "pscp.c",
    "ksftp": ROOT / "psftp.c",
}
TOOLS = ("klink", "kscp", "ksftp", "kitty", "kageant")

SWITCH_IN_TEXT = re.compile(r"(?<![\w/-])(--?[A-Za-z0-9][A-Za-z0-9_-]*)")
SWITCH_TOKEN = re.compile(r"^--?[A-Za-z0-9][A-Za-z0-9_-]*$")
INVOCATION = re.compile(
    r'^(?:&\s+)?(?:"[^"]*[\\/])?(%s)(?:\.exe)?"?(?=\s|$)' % "|".join(TOOLS),
    re.IGNORECASE)
C_STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
# "$out = " in front of an unquoted call: the call's output goes to a
# variable. A quoted right-hand side is a value ($env:GIT_SSH = "...").
PS_ASSIGN = re.compile(r'^\$[\w:]+\s*=\s*(?=[^\s"\'])')


def c_unescape(s):
    return (s.replace("\\n", "\n").replace("\\r", "\r").replace("\\t", "\t")
             .replace('\\"', '"').replace("\\\\", "\\"))


def strip_c_comments(src):
    return re.sub(r"/\*.*?\*/", " ", src, flags=re.S)


def read_macros(src):
    """#define NAME "..." "..." (backslash continuation lines too): the
    concatenated string of each macro whose body is string literals."""
    macros = {}
    lines = strip_c_comments(src).split("\n")
    i = 0
    while i < len(lines):
        m = re.match(r"\s*#\s*define\s+(KT_\w+)\b(.*)$", lines[i])
        if not m:
            i += 1
            continue
        name, body = m.group(1), m.group(2)
        while body.rstrip().endswith("\\") and i + 1 < len(lines):
            body = body.rstrip()[:-1] + " " + lines[i + 1]
            i += 1
        lits = C_STRING.findall(body)
        if lits:
            macros[name] = "".join(c_unescape(x) for x in lits)
        i += 1
    return macros


def usage_body(src):
    """The body of 'static void usage(void)', or None."""
    m = re.search(r"^static void usage\(void\)\s*\n\{\n(.*?)^\}", src,
                  re.S | re.M)
    return m.group(1) if m else None


def usage_text(body, macros, missing):
    """The text a usage() prints: its string literals plus the KT_* macros
    it passes. An unknown macro goes to `missing`."""
    body = strip_c_comments(body)
    parts = [c_unescape(x) for x in C_STRING.findall(body)]
    for name in re.findall(r"\b(KT_\w+)\b", body):
        if name in macros:
            parts.append(macros[name])
        else:
            missing.append(name)
    return "\n".join(parts)


def switches_of(text):
    return set(SWITCH_IN_TEXT.findall(text))


def read_usage(found):
    """{tool: set of switches} from the sources. Problems go to `found`."""
    text_h = TEXT_H.read_text(encoding="utf-8", errors="replace")
    macros = read_macros(text_h)
    allowed = {}
    for tool, path in USAGE_FILES.items():
        body = usage_body(path.read_text(encoding="utf-8", errors="replace"))
        if body is None:
            found.append("%s: no 'static void usage(void)' found" %
                         path.relative_to(ROOT))
            continue
        missing = []
        allowed[tool] = switches_of(usage_text(body, macros, missing))
        for name in missing:
            found.append("%s: usage() prints %s, not found in %s" % (
                path.relative_to(ROOT), name, TEXT_H.relative_to(ROOT)))
    help_src = HELP_H.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'default_help_file_content\s*=\s*"(.*?)"\s*;', help_src,
                  re.S)
    if m:
        allowed["kitty"] = switches_of(m.group(1).replace("\\\n", "\n"))
    else:
        found.append("%s: default_help_file_content not found" %
                     HELP_H.relative_to(ROOT))
    if "KT_KA_CLI_HELP" in macros:
        allowed["kageant"] = switches_of(macros["KT_KA_CLI_HELP"])
    else:
        found.append("%s: KT_KA_CLI_HELP not found" % TEXT_H.relative_to(ROOT))
    return allowed


def fenced_lines(page):
    """(line number, text) of every line inside a ``` fence."""
    out = []
    inside = False
    for n, line in enumerate(page.split("\n"), 1):
        if line.lstrip().startswith("```"):
            inside = not inside
            continue
        if inside:
            out.append((n, line))
    return out


def invocations(page):
    """(line number, tool, [switches]) of every invocation on the page."""
    out = []
    for n, line in fenced_lines(page):
        for part in line.split("|"):
            part = PS_ASSIGN.sub("", part.strip())
            m = INVOCATION.match(part)
            if not m:
                continue
            tool = m.group(1).lower()
            rest = re.sub(r'"[^"]*"|\'[^\']*\'', " ", part[m.end():])
            sw = [t for t in rest.split() if SWITCH_TOKEN.match(t)]
            out.append((n, tool, sw))
    return out


def check_page(page, allowed, found, where="docs/AGENTS-GUIDE.md"):
    """Every switch of every invocation in its tool's usage text. Returns
    the number of invocations."""
    calls = invocations(page)
    for n, tool, sw in calls:
        if tool not in allowed:
            continue
        for s in sw:
            if s not in allowed[tool]:
                found.append("%s:%d: %s %s is not in %s's usage text" % (
                    where, n, tool, s, tool))
    return len(calls)


def self_test():
    """Each planted miss found, each clean twin passes. Returns the list of
    what did not behave."""
    wrong = []
    macros = read_macros('#define KT_X_AGENT "  -agent    use it\\n"\n'
                         '#define KT_X_HELP \\\n'
                         '        "-unix PATH\\n" \\\n'
                         '        "-h, -help\\n"\n')
    body = usage_body('static void usage(void)\n{\n'
                      '    printf("  -batch    no prompts\\n");\n'
                      '    printf("  -hostkey keyid  (re-encrypt)\\n");\n'
                      '    printf(KT_X_AGENT);\n}\n')
    missing = []
    got = switches_of(usage_text(body, macros, missing)) if body else set()
    if got != {"-batch", "-hostkey", "-agent"} or missing:
        wrong.append("usage extraction: got %s, missing %s" % (
            sorted(got), missing))
    missing = []
    usage_text('printf(KT_X_NOPE);', macros, missing)
    if missing != ["KT_X_NOPE"]:
        wrong.append("an unknown macro: missing %s, want ['KT_X_NOPE']" % missing)
    if switches_of(macros.get("KT_X_HELP", "")) != {"-unix", "-h", "-help"}:
        wrong.append("a continued macro: got %s" %
                     sorted(switches_of(macros.get("KT_X_HELP", ""))))
    allowed = {"klink": {"-batch", "-hostkey", "-V"}, "kscp": {"-batch", "-q"},
               "ksftp": {"-batch", "-b", "-bc"}, "kageant": {"-unix"},
               "kitty": {"-load"}}
    cases = [
        ("a switch not in the usage", "klink -batch -nosuch <host>", 1),
        ("its clean twin", "klink -batch -hostkey <fp> <host>", 0),
        ("a quoted remote command", 'klink -batch <host> "ls -la"', 0),
        ("after a pipe, a miss", "Get-Content f | klink -batch -nosuch <host>", 1),
        ("after a pipe, clean", "Get-Content f | klink -batch <host>", 0),
        ("a quoted path, a miss", '& "<install-dir>\\klink.exe" -Q', 1),
        ("a quoted path, clean", '& "<install-dir>\\klink.exe" -V', 0),
        ("another tool's switch", "kscp -batch -bc <host>", 1),
        ("the tool's own switch", "ksftp -batch -bc <host>", 0),
        ("kageant, a miss", "kageant -noload", 1),
        ("kageant, clean", "kageant -unix <path>", 0),
        ("kitty.exe, a miss", "kitty.exe -ssh <host>", 1),
        ("an assignment", '$env:GIT_SSH = "<d>\\klink.exe"', 0),
        ("output to a variable, a miss", "$out = klink -batch -nosuch <host>", 1),
        ("output to a variable, clean", "$out = klink -batch <host> 2> <f>", 0),
        ("a comment line", "# klink -nosuch", 0),
        ("a tool name inside a word", "klinkx -nosuch", 0),
    ]
    for what, line, want in cases:
        found = []
        check_page("```\n%s\n```\n" % line, allowed, found, "planted")
        if len(found) != want:
            wrong.append("%s: %d finding(s), want %d%s" % (
                what, len(found), want, "".join("\n      " + f for f in found)))
    found = []
    check_page("never: `klink -pw <password>`\n", allowed, found, "planted")
    if found:
        wrong.append("a switch in prose: %d finding(s), want 0" % len(found))
    if check_page("text only\n", allowed, [], "planted") != 0:
        wrong.append("a page without invocations: counted some")
    return wrong


def main():
    wrong = self_test()
    if wrong:
        for w in wrong:
            print("check-agents-guide: self-test: " + w)
        print("check-agents-guide: the self-test failed - the check cannot be trusted")
        return 1
    if "--self-test" in sys.argv[1:]:
        print("check-agents-guide: self-test passed")
        return 0
    found = []
    allowed = read_usage(found)
    page = PAGE.read_text(encoding="utf-8")
    n = check_page(page, allowed, found)
    if n == 0:
        found.append("docs/AGENTS-GUIDE.md: no tool invocation found - "
                     "the page or this parser changed")
    if found:
        for f in found:
            print("check-agents-guide: " + f)
        return 1
    print("check-agents-guide: %d command lines, every switch is in its "
          "tool's usage text" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
