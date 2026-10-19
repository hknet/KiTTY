#!/usr/bin/env python3
"""Fail the build when the table of contents of FEATURES.md and its headings
differ: a section nobody can find from the top, or a TOC link that leads
nowhere. The fix is always: python3 tools/gen-features-toc.py
"""
import re
import sys

FEATURES = 'FEATURES.md'
TOC_HEAD = '**Table of contents**'
# the two closing sections get a link of their own; every other ## is a
# group heading whose ### sections are listed under it
OWN_LINK = ('Differences vs', 'Credits')


def anchor(title):
    """GitHub's anchor for a heading: lower case, punctuation dropped (a
    hyphen and an underscore stay), each space a hyphen."""
    t = title.strip().lower()
    t = ''.join(c for c in t if c.isalnum() or c in ' -_')
    return t.replace(' ', '-')


def read(path=FEATURES):
    s = open(path, newline='', encoding='utf-8').read()
    nl = '\r\n' if '\r\n' in s else '\n'
    return s, nl, s.split(nl)


def toc_span(lines):
    """(first, last) line index of the TOC body: after the head, before ---"""
    start = lines.index(TOC_HEAD)
    end = start + 1
    while lines[end] != '---':
        end += 1
    return start + 1, end


def wanted_toc(lines, after):
    toc = ['']
    seen = {}
    for line in lines[after:]:
        m = re.match(r'^(##|###) (.+)$', line)
        if not m:
            continue
        lvl, title = m.group(1), m.group(2).strip()
        a = anchor(title)
        n = seen.get(a, 0)
        seen[a] = n + 1
        if n:
            a = '%s-%d' % (a, n)
        if lvl == '##':
            if title.startswith(OWN_LINK):
                toc.append('- [%s](#%s)' % (title, a))
            else:
                toc.append('- **%s**' % title)
        else:
            toc.append('  - [%s](#%s)' % (title, a))
    toc.append('')
    return toc


def main():
    s, nl, lines = read()
    first, last = toc_span(lines)
    have = lines[first:last]
    want = wanted_toc(lines, last + 1)
    if have == want:
        return 0
    have_set, want_set = set(have), set(want)
    missing = [t.strip() for t in want if t.strip() and t not in have_set]
    stale = [t.strip() for t in have if t.strip() and t not in want_set]
    print('FEATURES.md: the table of contents does not match the headings')
    for t in missing:
        print('  missing: ' + t)
    for t in stale:
        print('  stale:   ' + t)
    if not missing and not stale:
        print('  (the same entries, in another order)')
    print('  -> run: python3 tools/gen-features-toc.py')
    return 1


if __name__ == '__main__':
    sys.exit(main())
