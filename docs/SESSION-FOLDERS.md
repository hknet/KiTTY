# Generating a session folder (`Sessions\`)

For tools that write KiTTY++ sessions themselves - from an inventory, a CMDB,
a template in Git. Everything here applies to the **folder store**
(`[KiTTY] savemode=dir`, the default of `kitty_portable.exe`): one file per
session, folders as directories. The registry store's counterpart is at the
end.

## Where the folder is

| `kitty.ini` (`[KiTTY]`) | Session folder |
|---|---|
| `sessions=D:\ops\Sessions` | that folder (`%VAR%` expanded). A relative value is below the folder of the exe. Use a drive path: a value starting with `\` is also taken below the exe folder, so a UNC path does not work here |
| no `sessions`, `configdir=...` set | `<configdir>\Sessions` (configdir may be a UNC path) |
| neither | `Sessions\` beside the exe |

- `kitty_portable.exe` uses the folder store by default; `kitty.exe` only with
  `savemode=dir` in its `kitty.ini` (beside the exe, or `%APPDATA%\KiTTY\`, or
  the file `%KITTY_INI_FILE%` names - see [KITTY-INI.md](KITTY-INI.md)).
- `klink`, `kscp` and `ksftp` from the same folder read the same store.
- kitty.exe creates a missing session folder; the command-line tools do not.
- **State lives beside the sessions.** When the folder's last component is
  not `Sessions` (e.g. `sessions=D:\ops\mysess`), KiTTY++ keeps `KiTTYState`
  and `SshHostKeys\` inside it. They are never listed as sessions - a tool that
  regenerates the folder must not delete them.

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

- One file per session; each directory is a folder level, nested up to 24
  deep. The session `Linux\web01` is the file `Linux\web01<suffix>`.
- **A folder exists through its sessions.** An empty directory, or one with
  no session file in it, is not shown. A directory that should survive
  KiTTY++ deleting its last session keeps a `.gitkeep`.
- **Skipped when listing:** every name starting with a dot (`.git`,
  `.gitignore`, `.gitkeep`), directory junctions and symbolic links, and
  every file that is not a session file (below).
- **A session file** is a file with a line that starts with exactly
  `HostName` or `Protocol`, followed directly by `=`. Case counts; no spaces
  before the key, no BOM in front of the first line. A `README.md` or a log
  beside the sessions stays out of the list.

## File names

**Suffix.** `[KiTTY] fileextension` (Application > KiTTY++ Settings > Storage &
Backup, "Session File Extension"; none by default, a dot is put in front if
missing) is added to every session file name, e.g. `.ktx`. The list shows the
names without it. A file without the suffix is still listed and gets it on
its next save. Do not generate both `web01` and `web01.ktx`: a save is then
refused.

**Escaping, per path component** (file name and each directory name):

| In the session name | In the file or directory name |
|---|---|
| `% \ / : * ? " < > \|`, control characters, DEL | `%` + two UPPERCASE hex digits: `%25`, `%2F`, `%3A` ... |
| a leading dot | `%2E` (`.x` -> `%2Ex`) |
| a trailing dot or space | `%2E` / `%20` (`a.` -> `a%2E`) |
| a Windows device name: `CON` `PRN` `AUX` `NUL` `COM1`-`COM9` `LPT1`-`LPT9`, also with an extension (`con.txt`) | its first character escaped: `%43ON`, `%63on.txt` |
| everything else (spaces inside, `=`, `#`, `&`, non-ASCII) | as it is |

- Names are in the ANSI code page of Windows, not UTF-8. A file or folder
  whose name does not fit it is shown as a red row asking for a rename, and a
  folder with such a name is not looked into.
- KiTTY++ reads `%xx` in either case and other spellings too (`%20` for a
  space); such a file is listed under its decoded name and saved in place,
  never renamed. Where two files decode to the same session, the second is a
  red row. Generate exactly the form above and neither happens.

## File content

```
Protocol=ssh
HostName=web01.example.com
PortNumber=22
UserName=admin
```

- One `Key=value` per line, the key at the start of the line, no spaces
  around `=`. Keys are case-sensitive (`HostName`, not `hostname`). The last
  of two equal keys wins. Line order does not matter.
- LF or CRLF; KiTTY++ writes LF. No BOM.
- **Values** use the `%xx` set of file names - `% \ / : * ? " < > |`,
  control characters and DEL - without the dot and device-name rules: `%` is
  `%25`, `\` is `%5C` (`D:%5Ckeys%5Cid.ppk`), a line break `%0A`; `=` and
  spaces stay as they are. Non-ASCII in the ANSI code page.
- Numbers and switches are decimal text: `PortNumber=2222`, `AgentFwd=1`.
- Classic KiTTY's `Key\value\` lines are read too - do not generate them.
- **Keys a tool adds survive a KiTTY++ save**: the file is read before it is
  written. KiTTY++ adds keys of its own (every setting, `Folder=` among them).

**The keys that matter:**

| Key | Value |
|---|---|
| `Protocol` | `ssh`, `telnet`, `raw`, `rlogin`, `serial`, `supdup`, `adb` - exactly these, lower case |
| `HostName` | host or address |
| `PortNumber` | port. **Read only when `Protocol` is one of the names above**: without `Protocol`, or with a misspelt one, the session is SSH on port 22 whatever `PortNumber` holds |
| `UserName` | login name |
| `PublicKeyFile` | private key file, escaped (`%5C` for `\`) |
| `SerialLine`, `SerialSpeed` | for `Protocol=serial` (`COM3`, `115200`) |
| `Tags` | session tags, for Manage Sessions and the launcher |
| `SessionLockdown` | `1`: KiTTY++ refuses to save over this session until the lock is switched off (not for `Default Settings`) - a generated file stays as generated |

**`Folder=` is not needed.** The directory decides the folder; a `Folder=`
value in a file inside a directory is ignored and corrected on the next save.
A file at the top level with `Folder=Linux` is shown in `Linux` by its bare
name - Manage Sessions > Arrange... moves such files into their directory.

**Missing keys** come from the built-in defaults. With
`[KiTTY] inheritdefaultsettings=yes` (Storage & Backup, "Inherit Default
Settings") they come from the session `Default Settings` instead - the file
`Default Settings<suffix>`, with a real space, not `%20`. That way a generated
session can hold only what differs per host.

## When KiTTY++ sees the changes

- Nothing watches the folder. A session started by name (`kitty.exe -load
  Linux\web01`) is always read fresh; the configuration window reads the list
  when it opens; kageant reads it each time its tray menu opens.
- The launcher's tray menu shows a copy, rebuilt at its start and by its
  menu's Refresh - while `[Launcher] reload` is not `no`.

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

- One key per session, the whole path in its name; `\` between folders is
  `%5C`. Escaped with uppercase `%xx`: space, `\ * ? %`, control characters,
  every non-ASCII byte and a leading dot - `Default Settings` is
  `Default%20Settings`. `/ : " < > |` stay as they are.
- Text is `REG_SZ`, numbers and switches `REG_DWORD` (`PortNumber` 22 =
  `dword:00000016`). A value of the wrong type is ignored and the default
  used.

---
*See also [KITTY-PORTABLE.md](KITTY-PORTABLE.md) (the folder store as a
whole) and [KITTY-INI.md](KITTY-INI.md) (every `kitty.ini` key).*
