# Generating a session folder (`Sessions\`)

For tools that write KiTTY++ sessions themselves (inventory, CMDB, Git
template). Applies to the **folder store** (`[KiTTY] savemode=dir`, the default
of `kitty_portable.exe`): one file per session, folders as directories. The
registry store is at the end.

Every session key with its format and default: [SESSION-FILES.md](SESSION-FILES.md).

## Where the folder is

| `kitty.ini` (`[KiTTY]`) | Session folder |
|---|---|
| `sessions=D:\ops\Sessions` | that folder, `%VAR%` expanded. Anything but `X:\...` (a leading `\` included) is below the exe folder, so no UNC path |
| no `sessions`, `configdir=...` | `<configdir>\Sessions` (configdir may be a UNC path) |
| neither | `Sessions\` beside the exe |

- `kitty.exe` uses the folder store only with `savemode=dir` in its
  `kitty.ini` (beside the exe, `%APPDATA%\KiTTY\`, or `%KITTY_INI_FILE%` - see
  [KITTY-INI.md](KITTY-INI.md)). `klink`, `kscp` and `ksftp` from the same
  folder read the same store.
- kitty.exe creates a missing session folder; the command-line tools do not.
- **The store's other files follow the session folder.** When its last
  component is not `Sessions` (e.g. `sessions=D:\ops\mysess`), `KiTTYState`,
  `Jumplist`, `PUTTY.RND`, `SshHostKeys\`, `SshHostCAs\`, `Security\`,
  `TermEnvHint\` and `WindowPos\` are inside it. They are never listed as
  sessions - a tool that regenerates the folder must not delete them.
  `Proxies\`, `Launcher\`, `Backups\`, `Commands\` and `Sessions_Commands\`
  stay in the configuration directory. Details: the help appendix "The folder
  store".

## Layout

```
Sessions\
├── Default Settings.ktx        optional, see "Missing keys"
├── bastion.ktx
├── Linux\
│   ├── web01.ktx
│   └── web02.ktx
└── Network\
    └── core%2Fsw01.ktx         the session "core/sw01"
```

- One file per session; each directory is a folder level, up to 24 deep.
  `Linux\web01` is the file `Linux\web01<suffix>`.
- **A folder exists through its sessions.** A directory without a session file
  is not shown. To survive deletion of its last session, it needs a `.gitkeep`.
- **Not listed:** names starting with a dot (`.git`, `.gitkeep`), directory
  junctions and symbolic links, and files that are not session files.
- **A session file** has a line starting with exactly `HostName` or `Protocol`
  directly followed by `=` or `\`: case-sensitive, no space before the key, no
  BOM.

## File names

**Suffix.** `[KiTTY] fileextension` (Storage & Backup, "Session File
Extension"; none by default, a dot put in front if missing) ends every session
file name, e.g. `.ktx`, and is not shown in the list. A file without it is
listed and gets it on its next save. Do not generate both `web01` and
`web01.ktx`: a save is then refused.

**Escaping, per path component:**

| In the session name | In the file or directory name |
|---|---|
| `% \ / : * ? " < > \|`, control characters, DEL | `%` + two UPPERCASE hex digits: `%25`, `%2F`, `%3A` ... |
| a leading dot | `%2E` |
| a trailing dot or space | `%2E` / `%20` |
| `CON` `PRN` `AUX` `NUL` `COM1`-`COM9` `LPT1`-`LPT9`, also with an extension | first character escaped: `%43ON`, `%63on.txt` |
| everything else (inner spaces, `=`, `#`, `&`, non-ASCII) | as it is |

- Names are in the ANSI code page, not UTF-8. A name that does not fit is
  shown as a red row requesting a rename; such a folder is not looked into.
- `%xx` is read in either case, other spellings too (`%20` for a space); such
  a file is listed under its decoded name and saved in place. Two files
  decoding to one session: the second is a red row. Generate exactly the form
  above.

## File content

```
Protocol=ssh
HostName=web01.example.com
PortNumber=22
UserName=admin
```

- One `Key=value` per line, key at the line start, no spaces around `=`
  (they become part of key and value). Keys are case-sensitive. A repeated
  key: the last value wins, a save keeps one line where the first was. Line
  order does not matter.
- LF or CRLF, kept per line; an added line takes the ending of the first line.
  No BOM.
- **Values** use the `%xx` set above without the dot and device-name rules:
  `\` is `%5C` (`D:%5Ckeys%5Cid.ppk`), a line break `%0A`; `=` and spaces stay.
  Non-ASCII in the ANSI code page.
- Numbers and switches are decimal: `PortNumber=2222`, `AgentFwd=1`.
- Old KiTTY's `Key\value\` lines are read too - do not generate them.
- **A save keeps what a tool wrote**: keys of its own, comments, blank lines
  and line order survive; unchanged lines stay as written (a Git diff shows
  only changed values); a changed value is rewritten as `Key=value` in place;
  keys KiTTY++ adds (every setting, `Folder=` among them) go to the end. An
  unchanged password keeps its stored line.

**The keys that matter:**

| Key | Value |
|---|---|
| `Protocol` | `ssh`, `telnet`, `raw`, `rlogin`, `serial`, `supdup`, `adb` - exactly these, lower case |
| `HostName` | host or address |
| `PortNumber` | port. **Read only with a valid `Protocol`**: without one, or misspelt, the session is SSH on port 22 |
| `UserName` | login name |
| `PublicKeyFile` | private key file, escaped (`%5C` for `\`) |
| `SerialLine`, `SerialSpeed` | for `Protocol=serial` (`COM3`, `115200`) |
| `Tags` | session tags, for Manage Sessions and the launcher |
| `SessionLockdown` | `1`: KiTTY++ refuses to save over this session until the lock is off (not for `Default Settings`) |

**`Folder=` is not needed.** The directory decides the folder; `Folder=` in a
file inside a directory is ignored and corrected on the next save. A top-level
file with `Folder=Linux` is shown in `Linux` by its bare name - Manage
Sessions > Arrange... moves such files into their directory.

**Missing keys** come from the built-in defaults, or with
`[KiTTY] inheritdefaultsettings=yes` (Storage & Backup, "Inherit Default
Settings") from the session `Default Settings` - the file
`Default Settings<suffix>`, with a real space. A generated session then holds
only what differs per host.

## The order of a folder (`.kittyorder`)

Every list sorts by the name shown (numbers by value, without case). A
folder can carry its own order in the file `.kittyorder` in its directory
(`Sessions\.kittyorder` for the top level). Manage Sessions writes it when
a folder or a session is dragged between two rows. A tool can write it too.

```
# sessions of this folder, in this order
s	web01
s	web02
f	databases
```

- Text, UTF-8 (a BOM is accepted), LF or CRLF line ends. Empty lines and
  lines starting with `#` are comments.
- One item per line: `s` (a session) or `f` (a folder), a TAB, the name as
  the list shows it: the session file name without the escaping and without
  the `sessionsuffix`, the folder's directory name unescaped. Names match
  without case.
- An optional third field, a TAB and the anchor: the name of the item (same
  kind) it comes after. Without it the item comes after the item of the line
  before of the same kind. The first such line puts its item first. A list
  of names in the wanted order is therefore a complete order.
- Items without a line keep their automatic place. An item comes directly
  after its anchor. An anchor that does not exist, a cycle or a line that
  cannot be read leaves the item in its automatic place. No message.
- A name that matches no item stays in the file: a session generated later
  finds its place.
- Dot-files are never sessions: `.kittyorder` is not listed.

What Manage Sessions changes. A drop, a rename, a delete and Reset Order
(the folder's right-click menu) EDIT the file: only the lines whose meaning
changes are touched. Comments, empty lines, the line ends, the BOM and the
lines of names not in the folder stay byte for byte. A moved item's line
moves right after its anchor's line. A line that would now follow another
item gets its anchor written out. No change in meaning, no write. The file
is written whole or not at all. Reset Order deletes the item lines of that
folder and deletes the file when nothing else is left in it. The next run of
a tool that owns the file replaces what Manage Sessions stored.

A row with a stored place carries a pin in Manage Sessions. The
configuration window, the terminal's Saved Sessions menu, the launcher and
kageant show the same order.

## When KiTTY++ sees the changes

- Nothing watches the folder. `kitty.exe -load Linux\web01` always reads the
  file; the configuration window reads the list when it opens; kageant each
  time its tray menu opens.
- The launcher's tray menu is a copy, rebuilt at its start and by Refresh,
  unless `[Launcher] reload=no`.

## The registry store

The same session in the registry store (`HKCU\Software\kapper.net\KiTTY`;
with `[KiTTY] KiClassName=PuTTY`, `HKCU\Software\SimonTatham\PuTTY`):

```
Windows Registry Editor Version 5.00

[HKEY_CURRENT_USER\Software\kapper.net\KiTTY\Sessions\Linux%5Cweb01]
"Protocol"="ssh"
"HostName"="web01.example.com"
"PortNumber"=dword:00000016
"UserName"="admin"
```

- One key per session, the whole path in its name, `\` between folders as
  `%5C`. Escaped as uppercase `%xx`: space, `\ * ? %`, control characters,
  non-ASCII bytes and a leading dot (`Default%20Settings`). `/ : " < > |` stay.
- Text is `REG_SZ`, numbers and switches `REG_DWORD`. A value of the wrong
  type is ignored and the default used.

The order of a folder is the key `SessionOrder` beside `Sessions`, one key per
folder (escaped as `Sessions` escapes a name, the top level as `%5C`), one
`REG_SZ` value per item: `s:<name>` or `f:<name>`, the data is the anchor,
empty = first:

```
[HKEY_CURRENT_USER\Software\kapper.net\KiTTY\SessionOrder\Linux]
"s:web01"=""
"s:web02"="web01"
"f:databases"=""
```

---
*See also [KITTY-PORTABLE.md](KITTY-PORTABLE.md) (the folder store as a
whole) and [KITTY-INI.md](KITTY-INI.md) (every `kitty.ini` key).*
