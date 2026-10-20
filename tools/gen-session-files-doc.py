#!/usr/bin/env python3
"""Generate docs/SESSION-FILES.md: every key a saved session holds.

For tools that write session files or registry sessions themselves. Read out
of the source, so the list cannot drift from what KiTTY++ loads and saves:

  conf.h          CONF_OPTION(name, VALUE_TYPE(t), DEFAULT_x(v),
                  SAVE_KEYWORD("K"), STORAGE_ENUM(e)) - key, type, default
  conf-enums.h    CONF_ENUM(e, VALUE(internal, stored)) - the stored numbers
  kitty/kitty_config_*.c, windows/config.c, config.c
                  the panel controls: I(CONF_x) gives the panel and the
                  HELPCTX, a radio group's buttons give the meaning of each
                  value
  windows/help.h  HELPCTX -> help topic
  doc/*.but       the topic; its first sentence is the key's description
  settings.c      the keys saved by custom code (SAVE_CUSTOM below)
  tools/session-key-descriptions.txt
                  Key<TAB>sentence, sorted, '#' comments: the description of
                  a key whose topic sentence does not describe it; it wins
                  over the topic. A line for a key that does not exist fails
                  the run, so the table cannot rot.

The "Changed since" section compares with docs/session-keys/<version>.json,
the key list of the previous release (exactly one such file).

    python3 tools/gen-session-files-doc.py              write the document
    python3 tools/gen-session-files-doc.py --check      exit 1 when it is stale
    python3 tools/gen-session-files-doc.py --selftest   the stale check fails
                                                        on a changed document
    python3 tools/gen-session-files-doc.py --root DIR --write-baseline VER
        write docs/session-keys/VER.json from the source tree DIR (a release
        tag extracted with git archive); the old baseline file is removed
        by hand
"""
from __future__ import annotations

import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DOC = os.path.join(REPO, 'docs', 'SESSION-FILES.md')
BASELINE_DIR = os.path.join(REPO, 'docs', 'session-keys')
DESCRIPTIONS = os.path.join('tools', 'session-key-descriptions.txt')

PANEL_SOURCES = ('kitty/kitty_config_session.c', 'kitty/kitty_config.c',
                 'kitty/kitty_config_upstream.c', 'kitty/kitty_config_shared.c',
                 'kitty/kitty_config_app.c', 'kitty/kitty_config_apps.c',
                 'windows/config.c', 'config.c')


def read(root, path):
    try:
        with open(os.path.join(root, path), encoding='utf-8',
                  errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def balanced(src, start, open_ch='(', close_ch=')'):
    """The text inside the bracket that opens just before `start`."""
    depth, i = 1, start
    while depth and i < len(src):
        if src[i] == open_ch:
            depth += 1
        elif src[i] == close_ch:
            depth -= 1
        i += 1
    return src[start:i - 1]


def strip_comments(src):
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


# ------------------------------------------------------------ constants
def enum_constants(root):
    """Named integer constants: enum members and #define NAME number."""
    consts = {}
    headers = ['putty.h', 'ssh.h', 'terminal/terminal.h', 'conf.h', 'misc.h',
               'defs.h', 'ssh/gss.h', 'windows/putty-rc.h']
    for extra in ('kitty', 'windows'):
        d = os.path.join(root, extra)
        if os.path.isdir(d):
            headers += [extra + '/' + f for f in sorted(os.listdir(d))
                        if f.endswith('.h')]
    for h in headers:
        src = read(root, h)
        for name, val in re.findall(
                r'(?m)^\s*#\s*define\s+([A-Za-z_]\w*)\s+(-?\d+|true|false)\s*(?:/|$)',
                src):
            consts.setdefault(name, {'true': 1, 'false': 0}[val]
                              if val in ('true', 'false') else int(val))
        for m in re.finditer(r'\benum\s*(?:\w+\s*)?\{', src):
            body = strip_comments(balanced(src, m.end(), '{', '}'))
            body = re.sub(r'(?m)^\s*#.*$', '', body)
            if '(' in body and '=' not in body:
                continue            # a macro list, not a plain enumeration
            nxt = 0
            for item in body.split(','):
                item = item.strip()
                if not item:
                    continue
                if '=' in item:
                    name, val = (s.strip() for s in item.split('=', 1))
                    if re.match(r'^-?\d+$', val):
                        nxt = int(val)
                    elif val in consts:
                        nxt = consts[val]
                    else:
                        continue
                else:
                    name = item
                if not re.match(r'^[A-Za-z_]\w*$', name):
                    continue
                consts.setdefault(name, nxt)
                nxt += 1
    # #define NAME OTHER_NAME, e.g. CLIPUI_DEFAULT_MOUSE CLIPUI_EXPLICIT
    for h in headers:
        for name, val in re.findall(
                r'(?m)^\s*#\s*define\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(?:/|$)',
                read(root, h)):
            if val in consts:
                consts.setdefault(name, consts[val])
    return consts


def humane(cname):
    """FORCE_OFF -> off, SER_PAR_ODD -> par odd: a constant as a label."""
    return (cname.split('_', 1)[1] if '_' in cname else cname) \
        .replace('_', ' ').lower()


def resolve_int(expr, consts):
    """A default such as AUTO, 2*TICKSPERSEC or -1 as a number, else None."""
    expr = expr.strip()
    if expr in ('true', 'false'):
        return 1 if expr == 'true' else 0
    sub = re.sub(r'[A-Za-z_]\w*',
                 lambda m: str(consts[m.group(0)]) if m.group(0) in consts
                 else m.group(0), expr)
    if re.match(r'^[-+*/() \d]+$', sub):
        try:
            return int(eval(sub, {'__builtins__': {}}))  # digits and operators only
        except (ArithmeticError, SyntaxError):
            return None
    return None


def storage_enums(root, consts):
    """enum -> {internal value: stored value} (VALUE_OBSOLETE is load-only)."""
    src = read(root, 'conf-enums.h')
    out = {}
    for m in re.finditer(r'CONF_ENUM\s*\(\s*(\w+)\s*,', src):
        body = balanced(src, m.end() - 1)
        mapping = {}
        for internal, stored in re.findall(
                r'(?<!_)\bVALUE\s*\(\s*([^,]+?)\s*,\s*(-?\d+)\s*\)', body):
            key = resolve_int(internal, consts)
            if key is not None:
                mapping[key] = (int(stored), internal.strip())
        if mapping:
            out[m.group(1)] = mapping
    return out


def options(root):
    src = read(root, 'conf.h')
    out = {}
    for m in re.finditer(r'CONF_OPTION\s*\(\s*(\w+)\s*,', src):
        body = strip_comments(balanced(src, m.end() - 1))
        if 'NOT_SAVED' in body:
            continue
        kw = re.search(r'SAVE_KEYWORD\s*\(\s*"([^"]+)"', body)
        vt = re.search(r'VALUE_TYPE\s*\(\s*(\w+)', body)
        se = re.search(r'STORAGE_ENUM\s*\(\s*(\w+)', body)
        df = re.search(r'DEFAULT_(INT|BOOL|STR)\s*\(', body)
        default = balanced(body, df.end()).strip() if df else None
        out[m.group(1)] = {
            'key': kw.group(1) if kw else None,
            'type': vt.group(1) if vt else 'STR',
            'enum': se.group(1) if se else None,
            'custom': 'SAVE_CUSTOM' in body,
            'default': default,
        }
    return out


# ------------------------------------------------------- the panels
def text_macros(root):
    """KT_* user-visible strings (kitty/kitty_text.h)."""
    src = read(root, 'kitty/kitty_text.h').replace('\\\n', ' ')
    out = {}
    for name, val in re.findall(
            r'(?m)^\s*#\s*define\s+(KT_\w+)\s+((?:"(?:[^"\\]|\\.)*"\s*)+)', src):
        out[name] = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', val))
    return out


def c_string(tok, texts, label=True):
    """A C string literal or KT_* macro as text; a label loses its '&'."""
    tok = tok.strip()
    if tok.startswith('"'):
        s = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', tok))
    elif tok in texts:
        s = texts[tok]
    else:
        return None
    s = s.replace('\\"', '"').replace('\\\\', '\\')
    if not label:
        return s
    s = s.replace('&&', '\0').replace('&', '').replace('\0', '&')
    return s.rstrip(':').strip()


def controls(root, consts, texts):
    """option -> {'help': HELPCTX name, 'panel': path, 'labels': {v: text}}.

    The first control found wins, in PANEL_SOURCES order: the kitty.exe box
    is built by kitty/kitty_config_*.c, config.c only fills what they lack.
    """
    out = {}
    via_handler = []                    # (handler, HELPCTX, panel)
    sources = {f: strip_comments(read(root, f)) for f in PANEL_SOURCES}
    for f in PANEL_SOURCES:
        src = sources[f]
        panels = [(m.start(), c_string(m.group(1), texts, label=False))
                  for m in re.finditer(
                      r'ctrl_getset\s*\(\s*\w+\s*,\s*("[^"]*"|KT_\w+)', src)]
        for m in re.finditer(r'\bctrl_(\w+)\s*\(', src):
            if m.group(1) in ('getset', 'columns', 'text', 'separator'):
                continue
            body = balanced(src, m.end())
            opt = re.search(r'\bI\(\s*CONF_(\w+)', body)
            hc = re.search(r'HELPCTX\(\s*(\w+)\s*\)', body)
            if not hc:
                continue
            panel = None
            for pos, p in panels:
                if pos > m.start():
                    break
                panel = p
            if not opt:
                h = re.search(r'\b(\w+_handler)\b', body)
                if h:
                    via_handler.append((h.group(1), hc.group(1), panel))
                continue
            name = opt.group(1)
            ent = out.setdefault(name, {'help': hc.group(1), 'panel': panel,
                                        'labels': {}})
            if m.group(1) == 'radiobuttons' and not ent['labels']:
                tail = body[opt.end():]
                for lab, val in re.findall(
                        r'("(?:[^"\\]|\\.)*"|\w+)\s*,\s*'
                        r'(?:\'(?:[^\'\\]|\\.)\'\s*,\s*|NO_SHORTCUT\s*,\s*)?'
                        r'I\(\s*([^)]+?)\s*\)', tail):
                    v = resolve_int(val.replace('|', '+'), consts) \
                        if '|' in val else resolve_int(val, consts)
                    text = c_string(lab, texts)
                    if v is not None and text:
                        ent['labels'].setdefault(v, text)
    # A control with its own handler (the host name box, the protocol
    # buttons) carries no I(CONF_x): the settings its handler writes are its
    # own. Only a handler that writes one or two settings is taken - a larger
    # one serves a whole group, and its topic belongs to none of them alone.
    # "Main" is the root of a separate dialog (the host key scan), not a
    # configuration panel.
    for handler, helpctx, panel in via_handler:
        if not panel or panel == 'Main':
            continue
        for src in sources.values():
            d = re.search(r'\bvoid\s+' + handler + r'\s*\([^)]*\)\s*\{', src)
            if d:
                names = set(re.findall(
                    r'conf_set_\w+\s*\(\s*[\w>.-]+\s*,\s*CONF_(\w+)',
                    balanced(src, d.end(), '{', '}')))
                if len(names) <= 2:
                    for name in sorted(names):
                        out.setdefault(name, {'help': helpctx, 'panel': panel,
                                              'labels': {}})
                break
    return out


# ------------------------------------------------------------ the help
def help_topics(root):
    src = read(root, 'windows/help.h')
    out = dict(re.findall(r'#define\s+WINHELP_CTX_(\w+)\s+"([^"]+)"', src))
    for a, b in re.findall(r'#define\s+WINHELP_CTX_(\w+)\s+WINHELP_CTX_(\w+)', src):
        if b in out:
            out[a] = out[b]
    return out


def halibut_markup(text):
    """Halibut markup to plain text; braces nest, so this walks them."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c != '\\':
            out.append(c)
            i += 1
            continue
        if i + 1 < n and text[i + 1] in '\\{}-_':
            out.append({'_': ' '}.get(text[i + 1], text[i + 1]))
            i += 2
            continue
        m = re.match(r'\\(u[0-9A-Fa-f]{4}|[A-Za-z]+|#)', text[i:])
        if not m:
            i += 1
            continue
        cmd, j = m.group(1), i + len(m.group(0))
        args = []
        while j < n and text[j] == '{':
            inner = balanced(text, j + 1, '{', '}')
            args.append(inner)
            j += len(inner) + 2
        if cmd.startswith('u') and len(cmd) == 5:
            out.append(chr(int(cmd[1:], 16)))
        elif cmd in ('I', 'IM', '#', 'k', 'K', 'cfg', 'versionid'):
            pass                        # index terms, comments, cross-refs
        elif cmd == 'q' and args:
            out.append('"%s"' % halibut_markup(args[0]))
        elif cmd in ('c', 'cw') and args:
            out.append('`%s`' % halibut_markup(args[0]).replace('`', ''))
        elif args:
            out.append(halibut_markup(args[-1] if cmd == 'W' else args[0]))
        i = j
    return ''.join(out)


def halibut_plain(text):
    """Halibut markup to one plain line of a table."""
    text = halibut_markup(text)
    # what a removed cross-reference leaves behind
    text = re.sub(r'[;,]\s*(?:see|in)?\s*\)', ')', text)
    text = re.sub(r'\(\s*(?:see|in|and)?\s*\)', '', text)
    text = re.sub(r'\s+([,.;:])', r'\1', text)
    return re.sub(r'\s+', ' ', text).strip()


HEADING = r'(?m)^\\(?:C|H|S\d?|A|U)\{([^}]+)\}'


def heading_titles(root):
    """Heading id -> its title, in Halibut markup, across every doc/*.but:
    a cross-reference points into any file of the manual."""
    out = {}
    for path in sorted(glob.glob(os.path.join(root, 'doc', '*.but'))):
        with open(path, encoding='utf-8', errors='replace') as f:
            for m in re.finditer(HEADING + r'[ \t]*([^\n]*)', f.read()):
                if m.group(2).strip():
                    out.setdefault(m.group(1), m.group(2).strip())
    return out


def resolve_refs(text, titles):
    """\\k{id} and \\K{id} -> the target's title as a quotation, as the manual
    shows the link; a reference alone in brackets reads "(see ...)". A target
    that is no heading is dropped (halibut_plain removes what that leaves
    behind)."""
    def title(m):
        t = titles.get(m.group(1))
        if not t:
            return ''
        return t if t.startswith('\\q{') else '\\q{%s}' % t

    def alone(m):
        t = title(m)
        return '(see %s)' % t if t else ''
    text = re.sub(r'\(\s*\\[kK]\{([^}]*)\}\s*\)', alone, text)
    return re.sub(r'\\[kK]\{([^}]*)\}', title, text)


def help_sentences(root):
    """topic -> the first sentence of its first paragraph."""
    out = {}
    titles = heading_titles(root)
    for path in sorted(glob.glob(os.path.join(root, 'doc', '*.but'))):
        with open(path, encoding='utf-8', errors='replace') as f:
            src = f.read()
        heads = list(re.finditer(HEADING, src))
        for i, h in enumerate(heads):
            end = heads[i + 1].start() if i + 1 < len(heads) else len(src)
            lines = src[h.end():end].split('\n')
            # the title is a paragraph of its own: it ends at a blank line
            while lines and lines.pop(0).strip():
                pass
            para = []
            for line in lines:
                s = line.strip()
                if not s:
                    if para:
                        break
                    continue
                if s.startswith(('\\cfg', '\\#', '\\versionid', '\\c ',
                                 '\\IM', '\\define', '\\rule', '\\quote')):
                    if para:
                        break
                    continue
                m = re.match(r'\\(?:b|n|dt|dd)(?:\s+|$)', s)
                if m:
                    if para:
                        break
                    s = s[m.end():]
                para.append(s)
            text = halibut_plain(resolve_refs(' '.join(para), titles))
            # the first full stop outside parentheses ends the sentence
            end = len(text)
            for m in re.finditer(r'(?<!\be\.g)(?<!\bi\.e)(?<!\betc)(?<!\.)\.(?:\s|$)',
                                 text):
                if text.count('(', 0, m.start()) <= text.count(')', 0, m.start()):
                    end = m.start() + 1
                    break
            sentence = text[:end]
            if sentence:
                out.setdefault(h.group(1), sentence)
    return out


# ------------------------------------------------- keys saved by custom code
#
# conf.h marks these SAVE_CUSTOM: settings.c writes them by hand, so their
# key and format are listed here. The run fails when conf.h gains or loses a
# SAVE_CUSTOM option this table does not match.
#   option -> [(key, kind, format)]
SAVE_CUSTOM = {
    'protocol': [('Protocol', 'word', '@PROTOCOLS@')],
    'ping_interval': [('PingInterval', 'int', 'number, minutes'),
                      ('PingIntervalSecs', 'int',
                       'number, seconds; added to `PingInterval`')],
    'ssh_cipherlist': [('Cipher', 'list', '@ciphernames@')],
    'ssh_kexlist': [('KEX', 'list', '@kexnames@')],
    'ssh_hklist': [('HostKey', 'list', '@hknames@')],
    'ssh_gsslist': [('GSSLibs', 'list', '@gsslibkeywords@')],
    'ssh_manual_hostkeys': [('SSHManualHostKeys', 'map',
                             'comma-separated host keys or fingerprints')],
    'try_gssapi_auth': [('AuthGSSAPI', 'bool', '`0` / `1`')],
    'try_gssapi_kex': [('AuthGSSAPIKEX', 'bool', '`0` / `1`')],
    'gssapifwd': [('GssapiFwd', 'bool', '`0` / `1`')],
    'gssapirekey': [('GssapiRekey', 'int', 'number, minutes')],
    'ssh_gss_custom': [('GSSCustom', 'path', 'path')],
    'ttymodes': [('TerminalModes', 'map',
                  'comma-separated `MODE=A` (auto), `MODE=N` (not sent) or '
                  '`MODE=V<value>`; @MAPESC@')],
    'environmt': [('Environment', 'map',
                   'comma-separated `NAME=value`; @MAPESC@')],
    'osx_option_meta': [('OSXOptionMeta', 'bool', '`0` / `1` (not used on Windows)')],
    'osx_command_meta': [('OSXCommandMeta', 'bool', '`0` / `1` (not used on Windows)')],
    'bellovl_t': [('BellOverloadT', 'int', 'number, milliseconds')],
    'bellovl_s': [('BellOverloadS', 'int', 'number, milliseconds')],
    'colours': [('Colour0 ... Colour@NCOLOURS@', 'rgb',
                 '`R,G,B`, each 0-255; one key per colour')],
    'wordness': [('Wordness0, Wordness32 ... Wordness224', 'list',
                  '32 comma-separated character classes each, '
                  'for characters 0-31, 32-63 ...')],
    'mousepaste': [('MousePaste', 'word', '@CLIPWORDS@')],
    'ctrlshiftins': [('CtrlShiftIns', 'word', '@CLIPWORDS@')],
    'ctrlshiftcv': [('CtrlShiftCV', 'word', '@CLIPWORDS@')],
    'mousepaste_custom': [],            # the custom: part of MousePaste
    'ctrlshiftins_custom': [],
    'ctrlshiftcv_custom': [],
    'portfwd': [('PortForwardings', 'map',
                 'comma-separated `[4|6]Lport=host:port`, `[4|6]Rport=host:port` '
                 'or `[4|6]Dport=`; @MAPESC@')],
}


def load_defaults(root, consts):
    """Literal defaults of the custom-loaded keys, from load_open_settings."""
    src = read(root, 'settings.c')
    src = re.sub(r'#ifdef PUTTY_UNIX_PLATFORM_H.*?#endif', '', src, flags=re.S)
    out = {}
    # read_clip_setting(sesskey, "Key", DEFAULT, ...): the word of DEFAULT
    words = clip_word_values(root, consts)
    for key, expr in re.findall(
            r'read_clip_setting\s*\(\s*sesskey\s*,\s*"(\w+)"\s*,\s*(\w+)', src):
        if consts.get(expr) in words:
            out[key] = words[consts[expr]]
    for key, expr in re.findall(
            r'gpp[bi](?:_raw)?\s*\(\s*sesskey\s*,\s*"(\w+)"\s*,\s*([^,;]+?)\s*[,)]',
            src):
        v = resolve_int(expr, consts)
        if v is not None:
            out.setdefault(key, str(v))
    for key, val in re.findall(
            r'(?:gpps_raw|gprefs)\s*\(\s*sesskey\s*,\s*"(\w+)"\s*,\s*"([^"\\]+)"',
            src):
        out.setdefault(key, val)
    return out


def name_tables(root):
    """keyvalwhere tables: the names a preference list takes."""
    src = read(root, 'settings.c') + read(root, 'windows/gss.c')
    out = {}
    for m in re.finditer(r'keyvalwhere\s+(\w+)\[\]\s*=\s*\{', src):
        body = balanced(src, m.end(), '{', '}')
        out[m.group(1)] = re.findall(r'\{\s*"([^"]+)"', body)
    return out


def protocol_names(root):
    """The Protocol words: the id of every backend be_list.c builds in."""
    src = strip_comments(read(root, 'be_list.c'))
    m = re.search(r'backends\[\]\s*=\s*\{', src)
    if not m:
        return []
    sources = []
    for d in ('ssh', 'otherbackends', 'windows', 'kitty'):
        for path in sorted(glob.glob(os.path.join(root, d, '*.c'))):
            with open(path, encoding='utf-8', errors='replace') as f:
                sources.append(f.read())
    names = []
    for vt in re.findall(r'&(\w+)', balanced(src, m.end(), '{', '}')):
        for text in sources:
            d = re.search(r'BackendVtable\s+' + vt + r'\s*=\s*\{', text)
            if d:
                idm = re.search(r'\.id\s*=\s*"([^"]+)"',
                                balanced(text, d.end(), '{', '}'))
                if idm and idm.group(1) not in names:
                    names.append(idm.group(1))
                break
    return names


def default_protocol(root):
    m = re.search(r'be_default_protocol\s*=\s*PROT_(\w+)', read(root, 'be_list.c'))
    return m.group(1).lower() if m else None


def clip_words(root):
    src = read(root, 'settings.c')
    i = src.find('static void write_clip_setting')
    body = src[i:src.index('\n}', i)] if i >= 0 else ''
    words = []
    for w in re.findall(r'"([a-z]+:?)"', body):
        if w not in words:
            words.append(w)
    return words


def clip_word_values(root, consts):
    """write_clip_setting: CLIPUI_x value -> the word it is stored as."""
    src = read(root, 'settings.c')
    i = src.find('static void write_clip_setting')
    body = src[i:src.index('\n}', i)] if i >= 0 else ''
    out = {}
    for m in re.finditer(r'case\s+(\w+)\s*:', body):
        w = re.search(r'"([a-z]+):?"', body[m.end():])
        if w and m.group(1) in consts and w.group(1) != 'custom':
            out.setdefault(consts[m.group(1)], w.group(1))
    return out


# ------------------------------------------------- the description table
def parse_descriptions(text, source=DESCRIPTIONS):
    """Key<TAB>sentence lines -> ({key: sentence}, [problem, ...]).

    Blank lines and lines starting with '#' are skipped. The keys must be
    unique and sorted as the key list is (case-insensitive)."""
    table, problems, prev = {}, [], None
    for n, line in enumerate(text.split('\n'), 1):
        line = line.rstrip('\r')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        where = '%s:%d' % (source, n)
        if '\t' not in line:
            problems.append('%s: no TAB between key and sentence' % where)
            continue
        key, sentence = (s.strip() for s in line.split('\t', 1))
        if not key or not sentence:
            problems.append('%s: empty key or sentence' % where)
            continue
        if key in table:
            problems.append('%s: %s is listed twice' % (where, key))
            continue
        if prev is not None and (key.lower(), key) < (prev.lower(), prev):
            problems.append('%s: %s is out of order (after %s)'
                            % (where, key, prev))
        table[key] = sentence
        prev = key
    return table, problems


def check_descriptions(table, rows, source=DESCRIPTIONS):
    """A line for a key no session holds is an error: renamed or removed."""
    known = {r['key'] for r in rows}
    return ['%s names %s, which is no session key' % (source, k)
            for k in sorted(set(table) - known, key=str.lower)]


def descriptions(root, rows):
    path = os.path.join(root, DESCRIPTIONS)
    try:
        with open(path, encoding='utf-8') as f:
            text = f.read()
    except OSError as e:
        raise SystemExit('gen-session-files-doc: cannot read %s: %s'
                         % (DESCRIPTIONS, e))
    table, problems = parse_descriptions(text)
    problems += check_descriptions(table, rows)
    if problems:
        raise SystemExit('gen-session-files-doc: ' + '\n  '.join(problems))
    return table


# ------------------------------------------------------------ the key list
def md(text):
    return text.replace('|', '\\|')


def code_list(vals):
    return ', '.join('`%s`' % v for v in vals)


def keys(root, strict=True):
    """Every saved key: [{'key', 'kind', 'values', 'default', 'format',
    'option'}], sorted by key."""
    consts = enum_constants(root)
    senums = storage_enums(root, consts)
    opts = options(root)
    texts = text_macros(root)
    ctls = controls(root, consts, texts)
    rows = []

    custom = {n for n, o in opts.items() if o['custom']}
    problems = []
    for n in sorted(custom - set(SAVE_CUSTOM)):
        problems.append('conf.h option %s is SAVE_CUSTOM and missing from '
                        'SAVE_CUSTOM in this script' % n)
    for n in sorted(set(SAVE_CUSTOM) - custom):
        problems.append('SAVE_CUSTOM in this script lists %s, which conf.h '
                        'does not save by custom code' % n)
    if problems and strict:
        raise SystemExit('gen-session-files-doc: ' + '\n  '.join(problems))

    for name, o in opts.items():
        if o['custom'] or not o['key']:
            continue
        labels = ctls.get(name, {}).get('labels', {})
        default = o['default']
        values = None
        if o['type'] == 'BOOL':
            kind, fmt = 'bool', '`0` / `1`'
            d = resolve_int(default, consts) if default else None
            default = str(d) if d is not None else default
        elif o['type'] == 'INT' and o['enum'] in senums:
            kind = 'enum'
            mapping = senums[o['enum']]
            pairs = []
            for internal, (stored, cname) in sorted(mapping.items(),
                                                    key=lambda kv: kv[1][0]):
                pairs.append((stored, labels.get(internal) or humane(cname)))
            values = [p[0] for p in pairs]
            fmt = '; '.join('`%d` %s' % p for p in pairs)
            d = resolve_int(default, consts) if default else None
            default = str(mapping[d][0]) if d in mapping else default
        elif o['type'] == 'INT' and labels:
            kind = 'enum'
            values = sorted(labels)
            fmt = '; '.join('`%d` %s' % (v, labels[v]) for v in values)
            d = resolve_int(default, consts) if default else None
            default = str(d) if d is not None else default
        elif o['type'] == 'INT':
            kind, fmt = 'int', 'number'
            d = resolve_int(default, consts) if default else None
            default = str(d) if d is not None else default
        elif o['type'] == 'FILENAME':
            kind, fmt = 'path', 'path'
        elif o['type'] == 'FONT':
            base = o['key']
            for key, k, f in ((base, 'text', 'font name'),
                              (base + 'IsBold', 'bool', '`0` / `1`'),
                              (base + 'CharSet', 'int', 'number, Windows character set'),
                              (base + 'Height', 'int', 'number, points')):
                rows.append({'key': key, 'kind': k, 'values': None,
                             'default': None, 'format': f, 'option': name})
            continue
        else:
            kind, fmt = 'text', 'text'
            if default is not None:
                default = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', default)) \
                    if default.startswith('"') else default
        rows.append({'key': o['key'], 'kind': kind, 'values': values,
                     'default': default, 'format': fmt, 'option': name})

    ldef = load_defaults(root, consts)
    if default_protocol(root):
        ldef['Protocol'] = default_protocol(root)
    tables = name_tables(root)
    subst = {'@PROTOCOLS@': code_list(protocol_names(root)),
             '@CLIPWORDS@': code_list(clip_words(root)).replace(
                 '`custom:`', '`custom:<name>`'),
             '@NCOLOURS@': str(consts.get('CONF_NCOLOURS', 1) - 1),
             '@MAPESC@': '`\\` escapes `=`, `,` and `\\` in a name or value'}
    for name in sorted(custom & set(SAVE_CUSTOM)):
        for key, kind, fmt in SAVE_CUSTOM[name]:
            m = re.match(r'^@(\w+)@$', fmt)
            if m and m.group(1) in tables:
                fmt = ('comma-separated, in order of preference: ' +
                       code_list(tables[m.group(1)]) +
                       ('; `WARN` marks the warning threshold'
                        if m.group(1) != 'gsslibkeywords' else ''))
            for a, b in subst.items():
                fmt = fmt.replace(a, b)
                key = key.replace(a, b)
            rows.append({'key': key, 'kind': kind, 'values': None,
                         'default': ldef.get(key), 'format': fmt,
                         'option': name})
    rows.append({'key': 'Present', 'kind': 'int', 'values': [1],
                 'default': '1', 'format': '`1`, always written',
                 'option': None})
    rows.sort(key=lambda r: (r['key'].lower(), r['key']))
    return rows, ctls


def baseline_record(rows):
    return {r['key']: {'kind': r['kind'], 'values': r['values'],
                       'default': r['default']} for r in rows}


def show_default(d):
    if d is None:
        return '-'
    if d == '':
        return '*(empty)*'
    return '`%s`' % d


def show_kind(rec):
    if rec['values']:
        return '%s %s' % (rec['kind'], '/'.join(str(v) for v in rec['values']))
    return rec['kind']


def changes(old, new):
    out = []
    for k in sorted(set(new) - set(old), key=str.lower):
        out.append(('added', k, '-', show_kind(new[k])))
    for k in sorted(set(old) - set(new), key=str.lower):
        out.append(('removed', k, show_kind(old[k]), '-'))
    for k in sorted(set(old) & set(new), key=str.lower):
        o, n = old[k], new[k]
        if (o['kind'], o['values']) != (n['kind'], n['values']):
            out.append(('type changed', k, show_kind(o), show_kind(n)))
        if o['default'] != n['default']:
            out.append(('default changed', k, show_default(o['default']),
                        show_default(n['default'])))
    return out


def baseline_file():
    files = sorted(glob.glob(os.path.join(BASELINE_DIR, '*.json')))
    if len(files) != 1:
        raise SystemExit('gen-session-files-doc: docs/session-keys/ must hold '
                         'exactly one <version>.json (the previous release), '
                         'found %d' % len(files))
    return files[0]


def render(root):
    rows, ctls = keys(root)
    topics = help_topics(root)
    sentences = help_sentences(root)
    table = descriptions(root, rows)
    base = baseline_file()
    version = os.path.splitext(os.path.basename(base))[0]
    with open(base, encoding='utf-8') as f:
        old = json.load(f)

    out = []
    w = out.append
    w('# Session keys')
    w('')
    w('<!-- GENERATED by tools/gen-session-files-doc.py - do not edit by hand. '
      'The build fails while this file differs from the source. -->')
    w('')
    w('Every key a saved KiTTY++ session holds: for tools that write session '
      'files ([SESSION-FOLDERS.md](SESSION-FOLDERS.md)) or registry sessions. '
      'Generated from the source at build time.')
    w('')
    w('- **Values** in a session file are decimal text for numbers and '
      'switches; text and paths use the `%xx` escaping of '
      '[SESSION-FOLDERS.md](SESSION-FOLDERS.md#file-content) (`%25` for `%`, '
      '`%5C` for `\\`). In the registry, text is `REG_SZ`, numbers and '
      'switches `REG_DWORD`.')
    w('- **Enumerations** list each stored number with the button that '
      'selects it. A value outside the list is not legal.')
    w('- **Default** applies when the key is missing (`-`: computed at load, '
      'e.g. `PortNumber` from `Protocol`, fonts from the system). With '
      '`[KiTTY] inheritdefaultsettings=yes` a missing key comes from '
      '`Default Settings` instead.')
    w('- **What it does** is the line for the key in '
      '`tools/session-key-descriptions.txt` where it has one, else the '
      'first sentence of the help topic of the key\'s control; `-` where '
      'neither exists.')
    w('')
    w('## Changed since %s' % version)
    w('')
    diff = changes(old, baseline_record(rows))
    if diff:
        w('| Change | Key | Was | Now |')
        w('|---|---|---|---|')
        for c, k, a, b in diff:
            w('| %s | `%s` | %s | %s |' % (c, k, md(a), md(b)))
    else:
        w('No key added, removed or changed.')
    w('')
    w('## Keys')
    w('')
    w('| Key | Format | Default | Panel | What it does |')
    w('|---|---|---|---|---|')
    for r in rows:
        ctl = ctls.get(r['option'] or '', {})
        desc = table.get(r['key']) or \
            sentences.get(topics.get(ctl.get('help', ''), ''), '-')
        panel = ctl.get('panel') or '-'
        w('| `%s` | %s | %s | %s | %s |' % (
            r['key'], md(r['format']), md(show_default(r['default'])),
            md(panel), md(desc)))
    return '\n'.join(out) + '\n'


def compare(have, want):
    """Return the lines that differ (empty when equal)."""
    have = have.replace('\r\n', '\n')
    if have == want:
        return []
    a, b = have.split('\n'), want.split('\n')
    sa, sb = set(a), set(b)
    return (['- ' + l for l in a if l not in sb] +
            ['+ ' + l for l in b if l not in sa]) or ['(line order differs)']


def selftest(root):
    """--check must fail on a stale document and pass on a fresh one."""
    want = render(root)
    if compare(want, want):
        print('selftest FAILED: a fresh document reads as stale')
        return 1
    lines = want.split('\n')
    i = next(n for n, l in enumerate(lines) if l.startswith('| `HostName`'))
    stale = list(lines)
    stale[i] = stale[i].replace('| text |', '| number |', 1)
    gone = lines[:i] + lines[i + 1:]
    for name, doc in (('changed row', '\n'.join(stale)),
                      ('missing row', '\n'.join(gone))):
        if not compare(doc, want):
            print('selftest FAILED: a %s reads as up to date' % name)
            return 1
    old = {'HostName': {'kind': 'int', 'values': None, 'default': None},
           'NoSuchKey': {'kind': 'bool', 'values': None, 'default': '0'}}
    kinds = {c for c, k, a, b in changes(old, {
        'HostName': {'kind': 'text', 'values': None, 'default': ''},
        'NewKey': {'kind': 'bool', 'values': None, 'default': '1'}})}
    if kinds != {'added', 'removed', 'type changed', 'default changed'}:
        print('selftest FAILED: change kinds %s' % sorted(kinds))
        return 1
    # the description table: a key that does not exist, a duplicate and an
    # unsorted line each fail; the real table's lines reach their rows
    rows, _ = keys(root)
    for name, text in (('unknown key', 'NoSuchKey\tx\n'),
                       ('duplicate key', 'HostName\tx\nHostName\ty\n'),
                       ('unsorted keys', 'PortNumber\tx\nHostName\ty\n'),
                       ('line without a TAB', 'HostName x\n')):
        table, problems = parse_descriptions(text)
        if not problems + check_descriptions(table, rows):
            print('selftest FAILED: a description table with a %s passes'
                  % name)
            return 1
    table = descriptions(root, rows)
    cells = {}
    for l in lines:
        m = re.match(r'^\| `([^`]+)` \|.* \| (.*) \|$', l)
        if m:
            cells[m.group(1)] = m.group(2)
    for k, sentence in table.items():
        if cells.get(k) != md(sentence):
            print('selftest FAILED: the description of %s does not reach '
                  'its row' % k)
            return 1
    # what a cross-reference must not leave behind: "(see )", "; see )"
    for k, cell in cells.items():
        if re.search(r'\(\s*\)|\bsee\s*[).]|[;,]\s*\)', cell):
            print('selftest FAILED: a dangling reference in %s: %s'
                  % (k, cell))
            return 1
    print('selftest OK: a changed or missing row fails --check, a '
          'description for no key fails the run, no dangling reference')
    return 0


def main(argv):
    root = REPO
    if '--root' in argv:
        root = os.path.abspath(argv[argv.index('--root') + 1])
    if '--write-baseline' in argv:
        version = argv[argv.index('--write-baseline') + 1]
        rows, _ = keys(root, strict=False)
        os.makedirs(BASELINE_DIR, exist_ok=True)
        path = os.path.join(BASELINE_DIR, version + '.json')
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(baseline_record(rows), f, indent=1, sort_keys=True)
            f.write('\n')
        print('wrote %s (%d keys)' % (path, len(rows)))
        return 0
    if '--selftest' in argv:
        return selftest(root)
    text = render(root)
    if '--check' in argv:
        try:
            with open(DOC, encoding='utf-8', newline='') as f:
                have = f.read()
        except OSError:
            print('docs/SESSION-FILES.md is missing - run '
                  'python3 tools/gen-session-files-doc.py', file=sys.stderr)
            return 1
        diff = compare(have, text)
        if diff:
            print('docs/SESSION-FILES.md is stale - run '
                  'python3 tools/gen-session-files-doc.py and commit it:',
                  file=sys.stderr)
            for l in diff[:40]:
                print('  ' + l, file=sys.stderr)
            return 1
        print('docs/SESSION-FILES.md: up to date')
        return 0
    with open(DOC, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('wrote %s' % DOC)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
