#!/usr/bin/env python3
"""Rewrite the table of contents of FEATURES.md from its headings.

Run from the repository root after adding, renaming or moving a section:
    python3 tools/gen-features-toc.py
The build runs tools/check-features-toc.py, which fails when the TOC and the
headings differ.
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
    toc = wanted_toc(lines, last + 1)
    if lines[first:last] == toc:
        print('FEATURES.md: table of contents already matches its headings')
        return 0
    lines[first:last] = toc
    open(FEATURES, 'w', newline='', encoding='utf-8').write(nl.join(lines))
    print('FEATURES.md: table of contents rewritten (%d entries)'
          % sum(1 for t in toc if t.startswith(('- [', '  - ['))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
