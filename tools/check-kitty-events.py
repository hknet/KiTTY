#!/usr/bin/env python3
"""Check windows/kitty_events.mc, the manual's event ID appendix
(doc/eventids.but) and its Markdown copy (docs/EVENT-IDS.md) against the
event ID table in kitty/kitty_eventids.h.

kitty.exe's message table (kitty_events.mc) is the text the Event Viewer
shows for every event of the suite; the table is what the programs report.
An ID reported without a message shows "The description for Event ID ...
cannot be found", and a message whose fields are in another order than the
event data labels every value wrongly - neither breaks the build. The
appendix is what a SIEM rule is written from, so its rows must be the
table's too. So:

1. The MessageIds of the .mc are exactly the IDs of the table.
2. Message 0 is "%1%0" (the text-only events of earlier versions).
3. Every other message is "%1", then one "%nfield: %N" per field of the
   table in its order (%2 onwards), then "%0".
4. The lines between the two generated-rows markers of doc/eventids.but are
   exactly the rows this script makes from the table (--write-appendix
   rewrites them).
5. The field glossary (kitty_eventfields[] in the same header) has one
   entry per field an ID uses, no unused entry, in name order, and the
   appendix's glossary lines are exactly those entries.
6. docs/EVENT-IDS.md is exactly the file this script makes from the table,
   the field glossary, the block list of the header and the appendix's
   introduction (--write-appendix rewrites it).

Every record written has an event ID of its own with its fields; ID 0 (the
text only) stays in the table and the .mc for the records of earlier
versions, and nothing writes it. The sources (every .c and .h of the tree,
comments left out) are held to that:

7. No ReportEvent / ReportEventA / ReportEventW call passes a literal 0 as
   its event ID (the fourth argument).
8. Every kind written to the agent log by name - the first argument of
   kitty_audit, kageant_audit_use, kageant_audit_use_dest,
   kageant_audit_keyset and kageant_audit_load, each string literal in it -
   is in audit_kinds[] (kitty/kitty_auditlog.c); a kind missing there would
   reach the file and never the Event Log. Each audit_kinds[] entry is a
   table row of the agent block with the same kind, and each such row has
   its entry.
9. Every kitty_wev_suite call gives an ID (a KITTY_EV_* constant of the
   header, or a number) that has a row in the table. Any other first
   argument is refused, as the check cannot read it.

The hand-written summaries of the IDs are held to the table too, so a new
ID cannot go without its mention and a count cannot drift:

10. The ID table of docs/WINDOWS-EVENT-LOG.md (its Records column) covers
    every ID of the table but 0; the ID list of doc/kitty-config.but covers
    every ID outside 0 and 100-199 (that block points to kageant's
    manual); the ID list of doc/pageant.but covers every ID of 100-199. A
    number covers its ID, a range a-b every table ID in it, "X as Y" counts
    X only. A number those lists give that is not in the table fails too.
11. A span a-b in the docs (doc/*.but, docs/*.md, FEATURES.md, README.md,
    the two generated appendices left out) that starts a block (a = 100,
    200, ...) and ends inside it (b not x99) gives exactly the block's first
    and last table ID, so "IDs 200-230" fails once the block ends at 241.
    "X as Y" counts neither side.

Every writer of the Event Log is held to the table, field by field, so a
value cannot land under another field's label:

12. kageant's lists (af_* in kitty/kitty_auditlog.c) are, for each kind of
    audit_kinds[], exactly the field list of its ID in the table, in the
    same order. Every call of the suite's writers - wev_write,
    kitty_wev_report and kitty_wev_suite (kitty/kitty_winevent.c) - names
    its values with a string literal ("host port user ..."), and that list
    is the table's list of its ID without pid (the writer fills in pid
    itself), in the same order; the number of values passed, when given
    as a number, is the number of names. An ID written as g_block +
    KITTY_EVO_x stands for both 2xx and 3xx, and both rows must match.
13. Append only: docs/event-fields-baseline.txt holds the IDs of the last
    release with their fields. Every one of them is still in the table
    with the same kind, and its field list starts with the baseline's
    list: a field is never removed, renamed or moved, a new field only
    takes a new position at the end. A new ID is free. --write-baseline
    <release> [--header <kitty_eventids.h of the release>] writes the file;
    release.ps1 -NextBaseline refreshes it from the release tag.
14. The field tables of docs/WINDOWS-EVENT-LOG.md (between the generated
    field tables markers: every ID, each field with its position, its
    label and its meaning from the glossary) are exactly the ones this
    script makes from the table (--write-appendix rewrites them). Where
    no message table is registered (the Application log) they are the
    mapping of the insertion strings.

15. Every agent row (100-199) ends with seq sig ts prev, and none of the
    four comes before them: kageant -verifylog and a SOC rebuild the signed
    line of the agent log file from an event by that rule
    (kitty_auditchain_event_line, docs/WINDOWS-EVENT-LOG.md section 7).

A self-test plants a violation of each of 7-13 and 15 in text held in
memory, and
a clean twin of each, before the tree is checked; a rule that no longer
fires fails the run.

Run:  python3 tools/check-kitty-events.py     (exit 1 on any violation)
      python3 tools/check-kitty-events.py --write-appendix
      python3 tools/check-kitty-events.py --self-test   (rules 7-13 alone)
      python3 tools/check-kitty-events.py --write-baseline 0.85.1.20 \
              [--header <path>]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AUDITLOG = ROOT / "kitty" / "kitty_auditlog.c"
# The source folders the build reads (CMakeLists.txt, the Conf accessor
# check's list); build folders are not among them.
SRC_DIRS = ("charset", "contrib", "crypto", "keygen", "kitty",
            "otherbackends", "proxy", "ssh", "stubs", "terminal", "test",
            "unicode", "unix", "utils", "windows")
AUDIT_CALLS = ("kitty_audit", "kageant_audit_use", "kageant_audit_use_dest",
               "kageant_audit_keyset", "kageant_audit_load")
# A file without one of these is skipped before it is parsed.
WATCHED = ("ReportEvent", "kitty_audit", "kageant_audit", "kitty_wev_suite")
HEADER = ROOT / "kitty" / "kitty_eventids.h"
MC = ROOT / "windows" / "kitty_events.mc"
APPENDIX = ROOT / "doc" / "eventids.but"
MARKDOWN = ROOT / "docs" / "EVENT-IDS.md"
BEGIN = "\\# BEGIN rows generated by tools/check-kitty-events.py --write-appendix"
END = "\\# END generated rows"
WINEVLOG = ROOT / "docs" / "WINDOWS-EVENT-LOG.md"
CFGDOC = ROOT / "doc" / "kitty-config.but"
PAGEANTDOC = ROOT / "doc" / "pageant.but"
GBEGIN = "\\# BEGIN glossary generated by tools/check-kitty-events.py --write-appendix"
GEND = "\\# END generated glossary"
BASELINE = ROOT / "docs" / "event-fields-baseline.txt"
FBEGIN = "<!-- BEGIN field tables generated by tools/check-kitty-events.py --write-appendix -->"
FEND = "<!-- END generated field tables -->"
# the suite's Event Log writers (kitty/kitty_winevent.c); the index of the
# field names in each one's arguments
WEV_CALLS = {"wev_write": 2, "kitty_wev_report": 2, "kitty_wev_suite": 2}


def read_table(path=None):
    text = (path or HEADER).read_text(encoding="ascii")
    defines = dict(re.findall(r'^#define\s+(KITTY_EV\w+)\s+("[^"]*"|\d+)',
                              text, re.M))

    def value(tok):
        tok = tok.strip()
        if tok in defines:
            tok = defines[tok]
        if tok.startswith('"'):
            return tok[1:-1]
        if tok == "NULL":
            return None
        return int(tok)

    body = text.split("kitty_eventids[] = {", 1)
    if len(body) != 2:
        sys.exit("check-kitty-events: no kitty_eventids[] table in %s" % HEADER)
    body = body[1].split("};", 1)[0]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    table = {}
    for m in re.finditer(r"\{([^{}]*)\}", body):
        # adjacent string literals ("a " "b") are one string
        raw = re.sub(r'"\s+"', "", m.group(1))
        # a quoted cell may hold a comma: whitespace before a cell is skipped
        # so that its opening quote starts the quoted alternative
        parts = [p.strip() for p in re.findall(r'\s*("[^"]*"|[^,]+)', raw)]
        parts = [p for p in parts if p]
        if len(parts) != 6:
            sys.exit("check-kitty-events: cannot read table row {%s}" % m.group(1))
        eid, block, source, kind, fields, level = (value(p) for p in parts)
        if eid in table:
            sys.exit("check-kitty-events: ID %d listed twice in the table" % eid)
        table[eid] = (block, source, kind, fields.split() if fields else [], level)
    return table


def read_fields():
    """The field glossary, kitty_eventfields[] in the same header, in order."""
    text = HEADER.read_text(encoding="ascii")
    body = text.split("kitty_eventfields[] = {", 1)
    if len(body) != 2:
        sys.exit("check-kitty-events: no kitty_eventfields[] table in %s" % HEADER)
    body = body[1].split("};", 1)[0]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    fields = []
    for m in re.finditer(r'\{\s*"([a-z]+)",\s*((?:"[^"\\]*"\s*)+)\}', body):
        fields.append((m.group(1), "".join(re.findall(r'"([^"\\]*)"', m.group(2)))))
    return fields


def glossary_lines(fields):
    return ["\\b \\c{%s} - %s." % (name, text) for name, text in fields]


def read_mc():
    text = MC.read_text(encoding="ascii")
    msgs = {}
    for m in re.finditer(r"^MessageId=(\d+)\s*$(.*?)^Language=English\s*$\n(.*?)\n^\.\s*$",
                         text, re.M | re.S):
        eid = int(m.group(1))
        if eid in msgs:
            sys.exit("check-kitty-events: MessageId=%d twice in the .mc" % eid)
        msgs[eid] = m.group(3).strip()
    declared = [int(x) for x in re.findall(r"^MessageId=(\d+)", text, re.M)]
    for eid in declared:
        if eid not in msgs:
            sys.exit("check-kitty-events: MessageId=%d has no English text block" % eid)
    return msgs


def appendix_rows(table):
    """One Halibut bullet per ID: the ID, its source, its level, then the
    kind (%1) and the fields (%2 onwards) in order."""
    rows = []
    for eid in sorted(table):
        _, source, kind, fields, level = table[eid]
        if eid == 0:
            # the level column holds why it is listed: no longer written
            rows.append("\\b \\c{0} \\c{%s}: %s. \\c{%%1} holds the text in full."
                        % (source, level))
            continue
        flist = ", ".join("\\c{%s}" % f for f in fields)
        rows.append("\\b \\c{%d} \\c{%s} (%s): \\c{%s}, then %s."
                    % (eid, source, level, kind, flist))
    return rows


def appendix_parts(begin=BEGIN, end=END):
    text = APPENDIX.read_text(encoding="ascii")
    if begin not in text or end not in text:
        sys.exit("check-kitty-events: %s has no %s markers" % (APPENDIX, begin))
    head, rest = text.split(begin, 1)
    body, tail = rest.split(end, 1)
    return head, body, tail


def write_block(begin, end, lines):
    head, _, tail = appendix_parts(begin, end)
    with open(APPENDIX, "w", encoding="ascii", newline="") as f:
        f.write(head + begin + "\n\n" + "\n\n".join(lines) + "\n\n" + end + tail)


def md_text(s, where):
    """Text for a Markdown table cell or list item. A | would end the cell
    and a backtick would open code; neither is escaped, the run stops."""
    for ch in "|`":
        if ch in s:
            sys.exit("check-kitty-events: %s has a %s, which docs/EVENT-IDS.md "
                     "cannot carry as plain text: %s" % (where, ch, s))
    return s


def but_to_md(s):
    """The appendix's introduction, Halibut to Markdown: \\c{x} is `x`;
    any other markup stops the run."""
    s = re.sub(r"\\c\{([^{}]*)\}", r"`\1`", s)
    if "\\" in s or "|" in s:
        sys.exit("check-kitty-events: %s: markup docs/EVENT-IDS.md cannot "
                 "carry in the introduction: %s" % (APPENDIX, s))
    return s


def appendix_intro():
    """The appendix's paragraphs between its title and the glossary."""
    text = APPENDIX.read_text(encoding="ascii")
    head = text.split("\nThe fields, by name:", 1)
    if len(head) != 2:
        sys.exit("check-kitty-events: %s has no 'The fields, by name:' line" % APPENDIX)
    lines = head[0].split("\n")
    if not lines[0].startswith("\\A{"):
        sys.exit("check-kitty-events: %s does not start with its \\A title" % APPENDIX)
    title = re.sub(r"^\\A\{[^}]*\}\s*", "", lines[0])
    paras = [" ".join(p.split()) for p in "\n".join(lines[1:]).split("\n\n")]
    # The appendix gives the level in brackets; the Markdown file gives it
    # in a column, so that sentence is left out there.
    paras = [p.replace(" The level of each ID is given in brackets.", "")
             for p in paras]
    return title, [but_to_md(p) for p in paras if p]


def header_blocks():
    """The block list of the comment at the top of the header: one
    '<IDs>  <text>' line per block, after ' * The blocks:'."""
    text = HEADER.read_text(encoding="ascii")
    rest = text.split(" * The blocks:\n", 1)
    if len(rest) != 2:
        sys.exit("check-kitty-events: %s has no ' * The blocks:' comment" % HEADER)
    blocks = []
    for ln in rest[1].split("\n"):
        m = re.match(r"^ \*   (\d+(?:-\d+)?)\s+(\S.*)$", ln)
        if not m:
            break
        blocks.append((m.group(1), m.group(2)))
    if not blocks:
        sys.exit("check-kitty-events: %s: the block list is empty" % HEADER)
    return blocks


def markdown(table, fields):
    """docs/EVENT-IDS.md: the appendix's content for a reader of the
    repository - the same rows, the same glossary, the same introduction."""
    title, intro = appendix_intro()
    out = ["# " + title, "",
           "<!-- GENERATED by tools/check-kitty-events.py --write-appendix from "
           "kitty/kitty_eventids.h - do not edit by hand. The build fails while "
           "this file differs from the source. -->", "",
           "Every Windows Event Log event ID of the KiTTY++ suite, with its "
           "source, its kind, its level and its fields in order. Generated from "
           "the table in `kitty/kitty_eventids.h`: edit the table there and run "
           "`python3 tools/check-kitty-events.py --write-appendix`. The manual's "
           "appendix \"" + title + "\" (`doc/eventids.but`) has the same list.",
           "",
           "The blocks, their sources and logs:", ""]
    for ids, text in header_blocks():
        out.append("- `%s` %s" % (ids, md_text(text, "block " + ids)))
    out.append("")
    for p in intro:
        out += [p, ""]
    out += ["## Event IDs", "",
            "| ID | Source | Kind | Level | Fields |",
            "|---:|---|---|---|---|"]
    for eid in sorted(table):
        _, source, kind, idfields, level = table[eid]
        where = "ID %d" % eid
        cells = ["`%d`" % eid, "`%s`" % md_text(source, where)]
        if eid == 0:
            cells += ["-", md_text(level, where), "`%1` holds the text in full"]
        else:
            cells += ["`%s`" % md_text(kind, where), md_text(level, where),
                      ", ".join("`%s`" % md_text(f, where) for f in idfields)]
        out.append("| " + " | ".join(cells) + " |")
    out += ["", "## Fields", ""]
    for name, text in fields:
        out.append("- `%s` - %s." % (name, md_text(text, "field " + name)))
    return "\n".join(out) + "\n"


def check_markdown(table, fields, bad):
    """docs/EVENT-IDS.md byte for byte against what --write-appendix writes."""
    fix = " (python3 tools/check-kitty-events.py --write-appendix)"
    if not MARKDOWN.exists():
        bad.append("docs/EVENT-IDS.md is missing" + fix)
        return
    have = MARKDOWN.read_bytes().decode("ascii", "replace")
    want = markdown(table, fields)
    if have == want:
        return
    if have.replace("\r\n", "\n") == want:
        bad.append("docs/EVENT-IDS.md has CRLF line ends; the repository "
                   "stores it with LF" + fix)
        return
    hl, wl = have.split("\n"), want.split("\n")
    n = next((i for i in range(min(len(hl), len(wl))) if hl[i] != wl[i]),
             min(len(hl), len(wl)))
    bad.append("docs/EVENT-IDS.md differs from the table, first at line %d:"
               "\n      %s\n    the table wants\n      %s%s"
               % (n + 1, hl[n] if n < len(hl) else "(end of file)",
                  wl[n] if n < len(wl) else "(end of file)", fix))


def check_glossary(table, fields, bad):
    """Every field an ID uses has one entry, every entry is used, the
    entries are in name order and the appendix has exactly their lines."""
    used = set()
    for eid in table:
        used.update(table[eid][3])
    names = [n for n, _ in fields]
    for n in sorted(used - set(names)):
        bad.append("kitty_eventfields[]: field %s is used by an ID but has no entry" % n)
    for n in sorted(set(names) - used):
        bad.append("kitty_eventfields[]: entry %s is used by no ID" % n)
    for n in sorted(set(n for n in names if names.count(n) > 1)):
        bad.append("kitty_eventfields[]: entry %s is listed twice" % n)
    if names != sorted(names):
        bad.append("kitty_eventfields[]: the entries are not in name order")
    _, body, _ = appendix_parts(GBEGIN, GEND)
    have = {}
    for ln in [ln for ln in body.split("\n") if ln.strip()]:
        m = re.match(r"\\b \\c\{([a-z]+)\} - ", ln)
        if not m:
            bad.append("doc/eventids.but: a glossary line that is no field entry: " + ln)
            continue
        if m.group(1) in have:
            bad.append("doc/eventids.but: field %s has two glossary lines" % m.group(1))
        have[m.group(1)] = ln
    want = dict(zip(names, glossary_lines(fields)))
    fix = " (python3 tools/check-kitty-events.py --write-appendix)"
    for n in sorted(set(want) - set(have)):
        bad.append("doc/eventids.but: field %s has no glossary line%s" % (n, fix))
    for n in sorted(set(have) - set(want)):
        bad.append("doc/eventids.but: glossary line for %s, which is no entry of the table" % n)
    for n in sorted(set(have) & set(want)):
        if have[n] != want[n]:
            bad.append("doc/eventids.but: glossary line for %s is\n      %s\n    the table wants\n      %s%s"
                       % (n, have[n], want[n], fix))
    if [n for n in have] != sorted(have):
        bad.append("doc/eventids.but: the glossary is not in name order" + fix)


# A string or character literal, a block comment (to its end, or to the end
# of the file) or a line comment, whichever starts first.
C_TOKEN = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\''
                     r'|/\*.*?(?:\*/|\Z)|//[^\n]*', re.S)


def strip_comments(text, blank_literals=False):
    """C source with its comments blanked (newlines kept, every offset
    the same); string and character literals left as they are, or with
    blank_literals their insides blanked too."""
    def blank(s):
        return re.sub(r"[^\n]", " ", s)

    def repl(m):
        s = m.group(0)
        if s[0] in "\"'":
            return s[0] + blank(s[1:-1]) + s[-1] if blank_literals else s
        return blank(s)
    return C_TOKEN.sub(repl, text)


def call_args(text, open_paren):
    """The arguments of the call whose '(' is at open_paren, as written,
    split at the commas of its own level; None when it does not close."""
    args, depth, start, i, n = [], 0, open_paren + 1, open_paren, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(text[start:i])
                return [a.strip() for a in args]
        elif c == "," and depth == 1:
            args.append(text[start:i])
            start = i + 1
        i += 1
    return None


def calls(text, names, blanked):
    """(name, line, args) for every call of one of names in text (comments
    already blanked); blanked is the same text with the insides of its
    literals blanked too, where the calls are looked for - a name inside a
    string literal is no call."""
    pat = re.compile(r"\b(%s)\s*\(" % "|".join(re.escape(x) for x in names))
    for m in pat.finditer(blanked):
        args = call_args(text, m.end() - 1)
        if args is not None:
            yield m.group(1), text.count("\n", 0, m.start()) + 1, args


def header_defines():
    text = HEADER.read_text(encoding="ascii")
    return {k: int(v) for k, v in
            re.findall(r"^#define\s+(KITTY_EV\w+)\s+(\d+)\s*(?:/\*.*)?$",
                       text, re.M)}


def read_audit_kinds(text=None):
    """audit_kinds[] of kitty/kitty_auditlog.c: kind -> event ID."""
    if text is None:
        text = AUDITLOG.read_text(encoding="ascii", errors="replace")
    body = strip_comments(text).split("audit_kinds[] = {", 1)
    if len(body) != 2:
        sys.exit("check-kitty-events: no audit_kinds[] table in %s" % AUDITLOG)
    body = body[1].split("};", 1)[0]
    kinds = {}
    for m in re.finditer(r'\{\s*"([a-z0-9-]+)"\s*,\s*(\d+)\s*,\s*\w+\s*\}', body):
        kinds[m.group(1)] = int(m.group(2))
    return kinds


def read_audit_lists(text=None):
    """Rule 12: audit_kinds[] as kind -> (ID, list name), and every af_*
    list of kitty/kitty_auditlog.c as name -> fields."""
    if text is None:
        text = AUDITLOG.read_text(encoding="ascii", errors="replace")
    code = strip_comments(text)
    lists = {}
    for m in re.finditer(r"static\s+const\s+char\s*\*\s*const\s+(af_\w+)\[\]\s*=\s*\{(.*?)\};",
                         code, re.S):
        lists[m.group(1)] = re.findall(r'"([^"]*)"', m.group(2))
    kinds = {}
    body = code.split("audit_kinds[] = {", 1)
    if len(body) == 2:
        for m in re.finditer(r'\{\s*"([a-z0-9-]+)"\s*,\s*(\d+)\s*,\s*(\w+)\s*\}',
                             body[1].split("};", 1)[0]):
            kinds[m.group(1)] = (int(m.group(2)), m.group(3))
    return kinds, lists


def check_agent_lists(table, kinds, lists, bad):
    """Rule 12, kageant: each kind's af_* list is its ID's table list."""
    for kind, (eid, lname) in sorted(kinds.items()):
        if lname not in lists:
            bad.append("kitty/kitty_auditlog.c: audit_kinds[] gives %s the "
                       "list %s, which this check cannot read" % (kind, lname))
            continue
        row = table.get(eid)
        if row is not None and lists[lname] != row[3]:
            bad.append("kitty/kitty_auditlog.c: %s (kind %s, ID %d) is\n"
                       "      %s\n    the table of kitty/kitty_eventids.h has\n"
                       "      %s\n    - the Event Log would label every value "
                       "from the first difference on wrongly"
                       % (lname, kind, eid, " ".join(lists[lname]),
                          " ".join(row[3])))


def wev_ids(arg, fn, defines, table):
    """Rule 12: the IDs a writer call reports, None when unreadable."""
    a = re.sub(r"\s+", " ", arg.strip())
    if fn == "kitty_wev_suite":
        if a in defines:
            return [defines[a]]
        return [int(a)] if re.fullmatch(r"\d+", a) else None
    if fn == "kitty_wev_report":           # the kittygen block
        return [400 + defines[a]] if a in defines else None
    m = re.fullmatch(r"(g_block|KITTY_EVB_\w+) \+ (KITTY_EVO_\w+)", a)
    if not m or m.group(2) not in defines:
        return None
    off = defines[m.group(2)]
    if m.group(1) == "g_block":            # the shared offsets: 2xx and 3xx
        return [b + off for b in (200, 300) if b + off in table] or None
    return [defines[m.group(1)] + off] if m.group(1) in defines else None


def check_writers(sources, table, defines, bad):
    """Rule 12, the suite: every writer call names its values with a
    literal, the table's list of its ID without pid, in order."""
    for name, text in sources:
        if not any(k in text for k in WEV_CALLS):
            continue
        code = strip_comments(text)
        blanked = strip_comments(text, blank_literals=True)
        for fn, line, args in calls(code, tuple(WEV_CALLS), blanked):
            fi = WEV_CALLS[fn]
            if len(args) < fi + 3:
                continue
            if re.match(r"(?:unsigned|int|const|WORD|DWORD)\b", args[0]):
                continue                    # a definition or a declaration
            farg = args[fi]
            if farg == "fields":
                continue                    # passed on inside the writers
            lits = re.findall(r'"([^"\\]*)"', farg)
            if not lits or re.sub(r'"[^"\\]*"|\s', "", farg):
                bad.append("%s:%d: %s: give the names of the values as a "
                           "string literal, so this check can hold them to "
                           "the table" % (name, line, fn))
                continue
            names = "".join(lits).split()
            ids = wev_ids(args[0], fn, defines, table)
            if not ids:
                bad.append("%s:%d: %s(%s): an ID this check cannot read"
                           % (name, line, fn, args[0]))
                continue
            for eid in ids:
                if eid not in table:
                    bad.append("%s:%d: %s: ID %d has no table row"
                               % (name, line, fn, eid))
                    continue
                want = [f for f in table[eid][3] if f != "pid"]
                if names != want:
                    bad.append("%s:%d: %s for ID %d names\n      %s\n    the "
                               "table of kitty/kitty_eventids.h wants (without "
                               "pid)\n      %s" % (name, line, fn, eid,
                                                   " ".join(names),
                                                   " ".join(want)))
            n = args[fi + 2]
            if re.fullmatch(r"\d+", n) and int(n) != len(names):
                bad.append("%s:%d: %s names %d values and passes %s"
                           % (name, line, fn, len(names), n))


def read_baseline(path=None):
    """Rule 13: (release, {ID: (kind, fields)}); (None, {}) when missing."""
    path = path or BASELINE
    if not path.exists():
        return None, {}
    rel, rows = None, {}
    for ln in path.read_text(encoding="ascii").replace("\r\n", "\n").split("\n"):
        if not ln.strip() or ln.startswith("#"):
            continue
        p = ln.split()
        if p[0] == "release" and len(p) == 2:
            rel = p[1]
        elif re.fullmatch(r"\d+", p[0]) and len(p) >= 2:
            rows[int(p[0])] = (p[1], p[2:])
        else:
            sys.exit("check-kitty-events: %s: cannot read the line %r" % (path, ln))
    return rel, rows


def check_baseline(table, base, bad):
    """Rule 13: every ID of the last release still there, its list
    starting with the release's list."""
    rel, rows = base
    where = "docs/event-fields-baseline.txt"
    if rel is None:
        bad.append("%s is missing or names no release (python3 tools/"
                   "check-kitty-events.py --write-baseline <release>)" % where)
        return
    for eid, (kind, fields) in sorted(rows.items()):
        row = table.get(eid)
        if row is None:
            bad.append("ID %d (%s) of release %s is no longer in the table: "
                       "an ID is never removed (%s)" % (eid, kind, rel, where))
            continue
        if row[2] != kind:
            bad.append("ID %d was kind %s in release %s and is %s now (%s)"
                       % (eid, kind, rel, row[2], where))
        if row[3][:len(fields)] != fields:
            bad.append("ID %d: the fields of release %s were\n      %s\n    the "
                       "table has\n      %s\n    - a field only takes a new "
                       "position at the end, never one of the release (%s)"
                       % (eid, rel, " ".join(fields), " ".join(row[3]), where))


def baseline_text(table, release):
    out = ["# The event IDs of release %s with their fields in order. Every "
           "field list of" % release,
           "# kitty/kitty_eventids.h must start with the list here: fields "
           "are only appended",
           "# (tools/check-kitty-events.py, rule 13). Written by "
           "check-kitty-events.py",
           "# --write-baseline from the release tag; release.ps1 "
           "-NextBaseline refreshes it.",
           "release %s" % release]
    for eid in sorted(table):
        if eid == 0:
            continue                       # text only, no fields
        out.append(" ".join([str(eid), table[eid][2]] + table[eid][3]))
    return "\n".join(out) + "\n"


def field_meaning(text, eid):
    """Rule 14: the parts of a glossary text that concern one ID: a part
    led by IDs ("100-170:", "211, 311 refused:") belongs to those IDs, a
    part without them to the IDs of the part before (or to all)."""
    out, cur = [], True
    for seg in text.split("; "):
        m = re.match(r"^((?:\d{3}(?:-\d{3})?)(?:,\s*\d{3}(?:-\d{3})?)*)"
                     r"((?:\s[^:;]*)?):\s*(.*)$", seg, re.S)
        if m:
            cov = False
            for lo, hi in re.findall(r"(\d{3})(?:-(\d{3}))?", m.group(1)):
                if int(lo) <= eid <= int(hi or lo):
                    cov = True
            exc = re.match(r"\s+except\s+(.*)$", m.group(2))
            if exc and str(eid) in re.findall(r"\d{3}", exc.group(1)):
                cov = False
            cur = cov
            if cov:
                out.append(m.group(3) if not m.group(2).strip() or exc
                           else m.group(2).strip() + ": " + m.group(3))
        elif cur:
            out.append(seg)
    return "; ".join(out) if out else text


def field_tables(table, fields):
    """Rule 14: one table per ID for docs/WINDOWS-EVENT-LOG.md."""
    gl = dict(fields)
    out = []
    for eid in sorted(table):
        if eid == 0:
            continue
        _, source, kind, idf, _ = table[eid]
        out += ["### %d %s" % (eid, kind), "",
                "Source `%s`, `%%1` = `%s`." % (source, kind), "",
                "| Position | Label | Meaning |", "|---:|---|---|"]
        for i, f in enumerate(idf):
            out.append("| `%%%d` | `%s` | %s |"
                       % (i + 2, f, md_text(field_meaning(gl.get(f, ""), eid),
                                            "field " + f)))
        out.append("")
    return out


def winevlog_parts():
    text = WINEVLOG.read_text(encoding="utf-8")
    if FBEGIN not in text or FEND not in text:
        sys.exit("check-kitty-events: %s has no field table markers" % WINEVLOG)
    head, rest = text.split(FBEGIN, 1)
    body, tail = rest.split(FEND, 1)
    return head, body, tail


def check_field_tables(table, fields, bad):
    _, body, _ = winevlog_parts()
    want = "\n" + "\n".join(field_tables(table, fields)) + "\n"
    if body != want:
        hl, wl = body.split("\n"), want.split("\n")
        n = next((i for i in range(min(len(hl), len(wl))) if hl[i] != wl[i]),
                 min(len(hl), len(wl)))
        bad.append("docs/WINDOWS-EVENT-LOG.md: the field tables differ from "
                   "the table, first at their line %d:\n      %s\n    the "
                   "table wants\n      %s (python3 tools/check-kitty-events.py "
                   "--write-appendix)" % (n + 1, hl[n] if n < len(hl) else "",
                                         wl[n] if n < len(wl) else ""))


CHAIN_TAIL = ["seq", "sig", "ts", "prev"]


def check_chain_tail(table, bad):
    """Rule 15: the chain fields end every agent row, once each."""
    for eid in sorted(table):
        block, _, kind, idf, _ = table[eid]
        if block != 100 or kind is None:
            continue
        if idf[-4:] != CHAIN_TAIL or any(f in CHAIN_TAIL for f in idf[:-4]):
            bad.append("ID %d (%s): an agent row ends with %s, once each - the "
                       "signed line is rebuilt from the event by that rule; it "
                       "has %s" % (eid, kind, " ".join(CHAIN_TAIL), " ".join(idf)))


def zero_literal(arg):
    """1 for a literal 0 (0, 0L, 0x0, (WORD)0, (0) ...)."""
    a = re.sub(r"\(\s*(?:unsigned\s+)?[A-Za-z_]\w*\s*\)", "", arg)   # casts
    a = re.sub(r"[\s()]", "", a)
    return re.fullmatch(r"(?:0+|0[xX]0+)[uUlL]*", a) is not None


def check_sources(sources, table, defines, kinds, bad):
    """Rules 7-9 over sources, a list of (name, text). A file that holds
    none of the names is not read further (the build runs this)."""
    for name, text in sources:
        if not any(k in text for k in WATCHED):
            continue
        code = strip_comments(text)
        blanked = strip_comments(text, blank_literals=True)
        for fn, line, args in calls(code, ("ReportEvent", "ReportEventA",
                                           "ReportEventW"), blanked):
            if len(args) >= 4 and zero_literal(args[3]):
                bad.append("%s:%d: %s with the literal event ID 0 - every "
                           "record has an ID of its own (kitty/kitty_eventids.h)"
                           % (name, line, fn))
        for fn, line, args in calls(code, AUDIT_CALLS, blanked):
            if not args:
                continue
            for kind in re.findall(r'"([^"\\]*)"', args[0]):
                if kind not in kinds:
                    bad.append("%s:%d: %s(\"%s\"): the kind is not in "
                               "audit_kinds[] (kitty/kitty_auditlog.c), so it "
                               "never reaches the Event Log" % (name, line, fn, kind))
        for fn, line, args in calls(code, ("kitty_wev_suite",), blanked):
            if not args:
                continue
            a = args[0]
            if re.fullmatch(r"KITTY_EV\w*", a):
                if a not in defines:
                    bad.append("%s:%d: kitty_wev_suite(%s): no such #define in "
                               "kitty/kitty_eventids.h" % (name, line, a))
                elif defines[a] not in table:
                    bad.append("%s:%d: kitty_wev_suite(%s): ID %d has no table "
                               "row" % (name, line, a, defines[a]))
            elif re.fullmatch(r"\d+", a):
                if int(a) not in table:
                    bad.append("%s:%d: kitty_wev_suite(%s): no table row"
                               % (name, line, a))
            elif not re.fullmatch(r"(?:unsigned|int|long|DWORD|WORD)\s+\w+", a):
                # not the declaration: an ID this check cannot read
                bad.append("%s:%d: kitty_wev_suite(%s): give the ID as a "
                           "KITTY_EV_* constant (kitty/kitty_eventids.h)"
                           % (name, line, a))


def check_kinds(table, kinds, bad):
    """audit_kinds[] against the agent block of the table, both ways."""
    for kind, eid in sorted(kinds.items()):
        row = table.get(eid)
        if row is None:
            bad.append("audit_kinds[]: %s is ID %d, which has no table row"
                       % (kind, eid))
        elif row[0] != 100 or row[2] != kind:
            bad.append("audit_kinds[]: %s is ID %d, whose table row is the "
                       "kind %s of block %d" % (kind, eid, row[2], row[0]))
    for eid in sorted(table):
        if table[eid][0] == 100 and kinds.get(table[eid][2]) != eid:
            bad.append("ID %d (%s) is in the table but not in audit_kinds[]"
                       % (eid, table[eid][2]))


def source_files():
    files = sorted(ROOT.glob("*.c")) + sorted(ROOT.glob("*.h"))
    for d in SRC_DIRS:
        for ext in ("*.c", "*.h"):
            files += sorted((ROOT / d).rglob(ext))
    for f in files:
        yield (str(f.relative_to(ROOT)).replace("\\", "/"),
               f.read_text(encoding="latin-1"))


def covered_ids(text, table):
    """Rule 10: the table IDs a hand-written list covers, and the numbers it
    gives. A three-digit number covers its ID, a range a-b every table ID
    from a to b; in "X as Y" only X counts."""
    text = re.sub(r"\bas\s+\d{3}-\d{3}\b", "", text)
    covered, named = set(), set()
    for m in re.finditer(r"\b(\d{3})(?:-(\d{3}))?\b", text):
        lo = int(m.group(1))
        hi = int(m.group(2)) if m.group(2) else lo
        named.update((lo, hi))
        covered.update(e for e in table if lo <= e <= hi)
    return covered, named


def check_lists(lists, table, bad):
    """Rule 10 on (where, text, wanted IDs) triples; text None = the list
    was not found."""
    for where, text, want in lists:
        if text is None:
            bad.append("%s: the hand-written ID list was not found" % where)
            continue
        covered, named = covered_ids(text, table)
        for eid in sorted(want - covered):
            bad.append("%s: ID %d (%s) is in the table but not in the "
                       "hand-written ID list" % (where, eid, table[eid][2]))
        for n in sorted(named - set(table)):
            bad.append("%s: the hand-written ID list gives %d, which is not "
                       "in the table" % (where, n))


def check_spans(docs, table, bad):
    """Rule 11 on (where, text) pairs: a span that starts an ID block and
    ends inside it gives that block's first and last table ID."""
    for where, text in docs:
        text = re.sub(r"\b\d{3}-\d{3}\s+as\s+\d{3}-\d{3}\b", "", text)
        for m in re.finditer(r"\b(\d00)-(\d{3})\b", text):
            lo, hi = int(m.group(1)), int(m.group(2))
            if hi // 100 != lo // 100 or hi % 100 == 99:
                continue
            block = sorted(e for e in table if lo <= e <= lo + 99)
            if not block:
                continue
            if (lo, hi) != (block[0], block[-1]):
                line = text.count("\n", 0, m.start()) + 1
                bad.append("%s:%d: the span %d-%d does not match the table, "
                           "whose block runs %d-%d" % (where, line, lo, hi,
                                                      block[0], block[-1]))


def span_docs():
    """Rule 11: the docs a span is read in."""
    paths = sorted((ROOT / "doc").glob("*.but")) + sorted((ROOT / "docs").glob("*.md"))
    paths += [ROOT / "FEATURES.md", ROOT / "README.md"]
    for path in paths:
        if path.name in ("eventids.but", "EVENT-IDS.md") or not path.exists():
            continue
        yield (str(path.relative_to(ROOT)),
               path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n"))


def hand_lists(table):
    """Rule 10: the three hand-written ID lists, each with what it must cover."""
    ids = set(table) - {0}
    agent = {e for e in ids if 100 <= e <= 199}
    lists = []
    text = WINEVLOG.read_text(encoding="utf-8")
    m = re.search(r"^## \d+\. Event IDs\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    rows = None
    if m:
        rows = []
        for ln in m.group(1).split("\n"):
            cols = ln.split("|")
            if len(cols) >= 5 and not re.match(r"\s*(Range|-+)\s*$", cols[1]):
                rows.append(cols[3])
        rows = "\n".join(rows) if rows else None
    lists.append(("docs/WINDOWS-EVENT-LOG.md", rows, ids))
    text = CFGDOC.read_text(encoding="utf-8").replace("\r\n", "\n")
    m = re.search(r"\\k\{kitty-eventids\}\.\n(.*?)\nRefusals, failures", text, re.S)
    body = None
    if m:
        # each paragraph without its "\b 200-299 - " block heading
        body = re.sub(r"^\\b \d+(?:-\d+)? - ", "", m.group(1), flags=re.M)
    lists.append(("doc/kitty-config.but", body, ids - agent))
    text = PAGEANTDOC.read_text(encoding="utf-8").replace("\r\n", "\n")
    m = re.search(r"^(100 sign, .*?)\n\n", text, re.M | re.S)
    lists.append(("doc/pageant.but", m.group(1) if m else None, agent))
    return lists


def self_test():
    """Rules 7-9 on planted sources: each violation found, each clean twin
    passes. Returns the list of what did not behave."""
    table = {0: (0, "KiTTY++", None, [], ""),
             100: (100, "KiTTY++_agent", "sign", ["fp"], ""),
             900: (900, "KiTTY++", "program-refused", ["check"], "")}
    defines = {"KITTY_EV_PROGRAM_REFUSED": 900, "KITTY_EV_NO_ROW": 999}
    kinds = read_audit_kinds('static const struct audit_kind {\n'
                             '} audit_kinds[] = {\n'
                             '    { "sign", 100, af_sign },\n};\n')
    cases = [
        ("literal 0", 'ReportEventA(h, t, 0, 0, sid, 1, 0, s, NULL);', 1),
        ("cast 0", 'ReportEventA(h, (WORD)t, 0, (DWORD)0, sid, 1, 0, s, NULL);', 1),
        ("0x0, W", 'ReportEventW(h, t, 0, 0x0, NULL, 1, 0, s, NULL);', 1),
        ("an ID", 'ReportEventA(h, t, 0, id, sid, ns, 0, s, NULL);', 0),
        ("a commented call", '/* ReportEventA(h, t, 0, 0, sid, 1, 0, s, NULL); */', 0),
        ("in a string", 'x = "ReportEventA(h, t, 0, 0, ...)";', 0),
        ("unknown kind", 'kitty_audit("nosuchkind", "result", r, NULL);', 1),
        ("known kind", 'kitty_audit("sign", "result", r, NULL);', 0),
        ("variable kind", 'kitty_audit(ev, "result", r, NULL);', 0),
        ("ternary, one unknown", 'kageant_audit_use(a ? "sign" : "bogus", f, c, r, x, 0);', 1),
        ("use_dest unknown", 'kageant_audit_use_dest("request", f, c, r, x, 0, d, w);', 1),
        ("suite ID undefined", 'kitty_wev_suite(KITTY_EV_NOPE, t, v, 1);', 1),
        ("suite ID without row", 'kitty_wev_suite(KITTY_EV_NO_ROW, t, v, 1);', 1),
        ("suite number without row", 'kitty_wev_suite(905, t, v, 1);', 1),
        ("suite ID with row", 'kitty_wev_suite(KITTY_EV_PROGRAM_REFUSED, t, v, 3);', 0),
        ("suite ID as an expression", 'kitty_wev_suite(KITTY_EV_PROGRAM_REFUSED + n, t, v, 3);', 1),
        ("the definition", 'void kitty_wev_suite(unsigned id, unsigned type,\n'
                           '    const char *const *vals, int n) { }', 0),
    ]
    wrong = []
    for what, src, want in cases:
        found = []
        check_sources([("planted.c", src)], table, defines, kinds, found)
        if len(found) != want:
            wrong.append("%s: %d finding(s), want %d%s" % (
                what, len(found), want,
                "".join("\n      " + f for f in found)))
    found = []
    check_kinds(table, {"sign": 100, "bogus": 100}, found)
    if len(found) != 1:
        wrong.append("audit_kinds against the table: %d finding(s), want 1" % len(found))
    found = []
    check_kinds(table, {}, found)
    if len(found) != 1:
        wrong.append("an agent row without audit_kinds: %d finding(s), want 1" % len(found))
    t10 = {0: None, 100: (0, "", "sign"), 101: (0, "", "confirm"),
           200: (0, "", "session-open"), 213: (0, "", "hostkey-update"),
           300: (0, "", "x"), 313: (0, "", "y")}
    t10 = {k: (v if v else (0, "", "text")) for k, v in t10.items()}
    lists10 = [
        ("every ID named", "sign (100), confirm (101)", {100, 101}, 0),
        ("an ID missing", "sign (100)", {100, 101}, 1),
        ("a number not in the table", "sign (100), confirm (101), bogus (199)", {100, 101}, 1),
        ("a range", "Windows Hello (100-101)", {100, 101}, 0),
        ("X as Y counts X only", "300-313 as 200-213", {200, 213, 300, 313}, 2),
        ("X as Y over a line break", "300-313 as\n200-213", {200, 213, 300, 313}, 2),
        ("the list not found", None, {100}, 1),
    ]
    for what, text, want, n in lists10:
        found = []
        check_lists([("planted", text, want)], t10, found)
        if len(found) != n:
            wrong.append("%s: %d finding(s), want %d%s" % (
                what, len(found), n, "".join("\n      " + f for f in found)))
    spans11 = [
        ("a span that drifted", "IDs 200-210", 1),
        ("the block's span", "IDs 200-213 and 300-313", 0),
        ("a span over a full block", "200-299, serial 100-199", 0),
        ("X as Y", "300-310 as 200-210", 0),
        ("a span not starting a block", "port 210-213", 0),
    ]
    for what, text, n in spans11:
        found = []
        check_spans([("planted", text)], t10, found)
        if len(found) != n:
            wrong.append("%s: %d finding(s), want %d%s" % (
                what, len(found), n, "".join("\n      " + f for f in found)))

    # rule 12, kageant: a list in another order than the table's
    t12 = {100: (100, "KiTTY++_agent", "sign", ["fp", "comment", "seq", "sig", "ts", "prev"], ""),
           171: (100, "KiTTY++_agent", "logchain", ["result", "seq", "sig", "ts", "prev"], "")}
    src12 = ('static const char *const af_sign[] = { "fp", "comment", "seq", "sig", "ts", "prev", NULL };\n'
             'static const char *const af_bad[] = { "comment", "fp", "seq", "sig", "ts", "prev", NULL };\n'
             'static const char *const af_lc[] = { "result", "seq", "sig", "ts", "prev", NULL };\n'
             'static const struct audit_kind {\n} audit_kinds[] = {\n'
             '    { "sign", 100, %s },\n    { "logchain", 171, af_lc },\n};\n')
    for what, lname, n in (("an agent list in the table's order", "af_sign", 0),
                           ("an agent list with two fields swapped", "af_bad", 1),
                           ("an agent list this check cannot find", "af_none", 1)):
        found = []
        kinds, lists = read_audit_lists(src12 % lname)
        check_agent_lists(t12, kinds, lists, found)
        if len(found) != n:
            wrong.append("%s: %d finding(s), want %d%s" % (
                what, len(found), n, "".join("\n      " + f for f in found)))
    # rule 12, the suite's writers
    tw = {210: (200, "", "hostkey-known", ["host", "port", "pid", "proxy"], ""),
          310: (300, "", "hostkey-known", ["host", "port", "pid", "proxy"], ""),
          401: (400, "", "private-saved", ["tool", "fp", "pid"], ""),
          900: (900, "", "program-refused", ["check", "found", "program"], "")}
    dw = {"KITTY_EV_PROGRAM_REFUSED": 900, "KITTY_EVO_HOSTKEY_KNOWN": 10,
          "KITTY_EVO_PRIVATE_SAVED": 1, "KITTY_EVB_KITTY": 200}
    writers12 = [
        ("a suite call in the table's order", 'kitty_wev_suite(KITTY_EV_PROGRAM_REFUSED, t, "check found program", v, 3);', 0),
        ("a suite call with two fields swapped", 'kitty_wev_suite(KITTY_EV_PROGRAM_REFUSED, t, "found check program", v, 3);', 1),
        ("a suite call without the names", 'kitty_wev_suite(KITTY_EV_PROGRAM_REFUSED, t, names, v, 3);', 1),
        ("a suite call passing another count", 'kitty_wev_suite(KITTY_EV_PROGRAM_REFUSED, t, "check found program", v, 2);', 1),
        ("a kittygen call, pid left out", 'kitty_wev_report(KITTY_EVO_PRIVATE_SAVED, 0, "tool fp", v, 2);', 0),
        ("a kittygen call naming pid", 'kitty_wev_report(KITTY_EVO_PRIVATE_SAVED, 0, "tool fp pid", v, 3);', 1),
        ("a shared offset: 2xx and 3xx both", 'wev_write(g_block + KITTY_EVO_HOSTKEY_KNOWN, t, "host port proxy", v, 3);', 0),
        ("a shared offset in another order", 'wev_write(g_block + KITTY_EVO_HOSTKEY_KNOWN, t, "port host proxy", v, 3);', 2),
        ("one block named", 'wev_write(KITTY_EVB_KITTY + KITTY_EVO_HOSTKEY_KNOWN, t,\n  "host port " "proxy", v, 3);', 0),
        ("the names passed on inside a writer", 'wev_write(id, (WORD)type, fields, vals, n);', 0),
        ("the definition", 'void kitty_wev_suite(unsigned id, unsigned type, const char *fields,\n    const char *const *vals, int n) { }', 0),
    ]
    for what, src, n in writers12:
        found = []
        check_writers([("planted.c", src)], tw, dw, found)
        if len(found) != n:
            wrong.append("%s: %d finding(s), want %d%s" % (
                what, len(found), n, "".join("\n      " + f for f in found)))
    # rule 13: append only against the release
    base = ("0.85.1.19", {100: ("sign", ["fp", "comment"]),
                          170: ("logrotate", ["result"])})
    def t13(f100, f170=("result",), kind100="sign", with170=True):
        t = {100: (100, "", kind100, list(f100), "")}
        if with170:
            t[170] = (100, "", "logrotate", list(f170), "")
        t[171] = (100, "", "logchain", ["result"], "")    # new: free
        t[172] = (100, "", "repeated", ["count", "of"], "")   # new: free
        return t
    cases13 = [
        ("fields appended, a new ID", t13(["fp", "comment", "seq", "sig", "ts", "prev"]), 0),
        ("a field reordered", t13(["comment", "fp", "seq"]), 1),
        ("a field removed", t13(["fp"]), 1),
        ("a field renamed", t13(["fp", "remark"]), 1),
        ("a kind renamed", t13(["fp", "comment"], kind100="signature"), 1),
        ("an ID removed", t13(["fp", "comment"], with170=False), 1),
    ]
    for what, table13, n in cases13:
        found = []
        check_baseline(table13, base, found)
        if len(found) != n:
            wrong.append("%s: %d finding(s), want %d%s" % (
                what, len(found), n, "".join("\n      " + f for f in found)))
    found = []
    check_baseline(t13(["fp", "comment"]), (None, {}), found)
    if len(found) != 1:
        wrong.append("no baseline file: %d finding(s), want 1" % len(found))
    # rule 15: the chain fields end every agent row
    for what, f, n in (("an agent row ending with seq sig ts prev", ["fp", "seq", "sig", "ts", "prev"], 0),
                       ("an agent row without ts and prev", ["fp", "seq", "sig"], 1),
                       ("the chain fields in another order", ["fp", "seq", "ts", "sig", "prev"], 1),
                       ("a chain field before the tail", ["ts", "fp", "seq", "sig", "ts", "prev"], 1)):
        found = []
        check_chain_tail({100: (100, "", "sign", f, ""),
                          200: (200, "", "session-open", ["session"], "")}, found)
        if len(found) != n:
            wrong.append("%s: %d finding(s), want %d" % (what, len(found), n))
    # rule 14: a glossary text's part per ID
    m14 = field_meaning("the key file; 100-170 except 103: a token; 104: "
                        "another; 211, 311 refused: none", 211)
    if m14 != "the key file; refused: none":
        wrong.append("the meaning of a field for one ID: %r" % m14)
    return wrong


def main():
    wrong = self_test()
    if wrong:
        for w in wrong:
            print("check-kitty-events: self-test: " + w)
        print("check-kitty-events: the self-test failed - rules 7-15 cannot be trusted")
        return 1
    if "--self-test" in sys.argv[1:]:
        print("check-kitty-events: self-test passed (rules 7-15)")
        return 0
    if "--write-baseline" in sys.argv[1:]:
        args = sys.argv[1:]
        i = args.index("--write-baseline")
        if i + 1 >= len(args):
            sys.exit("check-kitty-events: --write-baseline needs the release")
        header = None
        if "--header" in args:
            j = args.index("--header")
            if j + 1 >= len(args):
                sys.exit("check-kitty-events: --header needs a path")
            header = Path(args[j + 1])
        with open(BASELINE, "w", encoding="ascii", newline="") as f:
            f.write(baseline_text(read_table(header), args[i + 1]))
        print("check-kitty-events: wrote %s for release %s"
              % (BASELINE.relative_to(ROOT), args[i + 1]))
        return 0
    table = read_table()
    fields = read_fields()
    if "--write-appendix" in sys.argv[1:]:
        rows = appendix_rows(table)
        write_block(GBEGIN, GEND, glossary_lines(fields))
        write_block(BEGIN, END, rows)
        with open(MARKDOWN, "w", encoding="ascii", newline="") as f:
            f.write(markdown(table, fields))
        head, _, tail = winevlog_parts()
        with open(WINEVLOG, "w", encoding="utf-8", newline="") as f:
            f.write(head + FBEGIN + "\n" + "\n".join(field_tables(table, fields))
                    + "\n" + FEND + tail)
        print("check-kitty-events: wrote %d glossary lines and %d appendix rows, "
              "and docs/EVENT-IDS.md" % (len(fields), len(rows)))
        return 0
    msgs = read_mc()
    bad = []
    for eid in sorted(set(table) - set(msgs)):
        bad.append("ID %d is in the table but has no message in the .mc" % eid)
    for eid in sorted(set(msgs) - set(table)):
        bad.append("MessageId=%d is in the .mc but not in the table" % eid)
    for eid in sorted(set(table) & set(msgs)):
        _, _, kind, idfields, _ = table[eid]
        if eid == 0:
            want = "%1%0"
        else:
            want = "%1" + "".join("%%n%s: %%%d" % (f, i + 2)
                                  for i, f in enumerate(idfields)) + "%0"
        if msgs[eid] != want:
            bad.append("MessageId=%d text is\n      %s\n    the table wants\n      %s"
                       % (eid, msgs[eid], want))
    # The appendix, row by row: every ID once, each row exactly the table's
    # (source, level, kind, fields in order), in ID order.
    _, body, _ = appendix_parts()
    have = [ln for ln in body.split("\n") if ln.strip()]
    want_rows = dict(zip(sorted(table), appendix_rows(table)))
    seen = {}
    order = []
    fix = " (python3 tools/check-kitty-events.py --write-appendix)"
    for ln in have:
        m = re.match(r"\\b \\c\{(\d+)\}", ln)
        if not m:
            bad.append("doc/eventids.but: a generated line that is no ID row: " + ln)
            continue
        eid = int(m.group(1))
        if eid in seen:
            bad.append("doc/eventids.but: ID %d has two rows" % eid)
            continue
        seen[eid] = ln
        order.append(eid)
    for eid in sorted(set(want_rows) - set(seen)):
        bad.append("doc/eventids.but: ID %d is in the table but has no row%s" % (eid, fix))
    for eid in sorted(set(seen) - set(want_rows)):
        bad.append("doc/eventids.but: ID %d has a row but is not in the table" % eid)
    for eid in sorted(set(seen) & set(want_rows)):
        if seen[eid] != want_rows[eid]:
            bad.append("doc/eventids.but: ID %d row is\n      %s\n    the table wants\n      %s%s"
                       % (eid, seen[eid], want_rows[eid], fix))
    if order != sorted(order):
        bad.append("doc/eventids.but: the rows are not in ID order" + fix)
    check_glossary(table, fields, bad)
    check_markdown(table, fields, bad)
    kinds = read_audit_kinds()
    check_kinds(table, kinds, bad)
    akinds, alists = read_audit_lists()
    check_agent_lists(table, akinds, alists, bad)
    sources = list(source_files())
    check_sources(sources, table, header_defines(), kinds, bad)
    check_writers(sources, table, header_defines(), bad)
    check_baseline(table, read_baseline(), bad)
    check_chain_tail(table, bad)
    check_field_tables(table, fields, bad)
    check_lists(hand_lists(table), table, bad)
    docs = list(span_docs())
    check_spans(docs, table, bad)
    if bad:
        for b in bad:
            print("check-kitty-events: " + b)
        return 1
    print("check-kitty-events: %d messages, appendix rows and docs/EVENT-IDS.md "
          "rows match the table; %d source files: no event ID 0, every agent "
          "log kind and suite ID in the table, every writer's field list the "
          "table's and the last release's at its start; the hand-written ID lists of "
          "docs/WINDOWS-EVENT-LOG.md, doc/kitty-config.but and doc/pageant.but "
          "cover the table; the ID spans of %d docs match it"
          % (len(table), len(sources), len(docs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
