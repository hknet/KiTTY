# KiTTY Portable (`kitty_portable.exe`)

`kitty_portable.exe` is the portable edition of KiTTY. It is functionally identical to
`kitty.exe` — same SSH/Telnet/Rlogin/Raw client, same KiTTY feature set — but it keeps
saved sessions and most normal runtime state in files next to the executable/config
directory instead of the Windows registry.

## What's different

| | `kitty.exe` (standard) | `kitty_portable.exe` |
|---|---|---|
| Saved sessions stored in | Windows registry (`HKCU\Software\kapper.net\KiTTY`) | local files beside the exe/config dir, one file per session at its folder path (`Sessions\Linux\web\srv01`) |
| SSH host keys / host CAs | Windows registry | `SshHostKeys\` (settable, `[KiTTY] sshhostkeys`) / `SshHostCAs\` under the portable config dir |
| Random seed | user profile / normal PuTTY location | `PUTTY.RND` under the portable config dir |
| Recent/jump-list/update state | Windows registry / profile state | portable files such as `Jumplist` and `KiTTYState` |
| Where windows were left (configuration window, Manage Sessions, kageant's key list and agent log, About boxes, ...) | Windows registry (`AuxWinPos`) | `kitty_windowpos.ini` beside the exe |
| Global options (`kitty.ini`) | next to the exe / `%APPDATA%` | next to the exe/config dir |
| Needs install / admin rights | no | no |
| Roams with the Windows user profile | yes | no — the portable state travels with the folder instead |

Under the hood it's the same code with `MOD_PORTABLE` enabled, which makes KiTTY default
to **directory save-mode** (`savemode=dir`) and resolve its config (`kitty.ini`) from the
executable's own folder first. Auto-login passwords inside the session files are protected
at rest with a master password (the default) or with Windows DPAPI
(`[KiTTY] PortablePasswordProtection`).

**Session folders are folder paths.** A session's name is its folder path plus
its name, `Linux\web\srv01`, kept as the file `Sessions\Linux\web\srv01` (a
session folder that can live in Git). The same name can exist in two folders.
Sessions filed by their Folder value only keep working by their bare name;
Manage Sessions > Arrange... moves them to their folder path.

**Only session files are listed, and session files can carry a suffix.** The
folder store lists a file only when it holds a `HostName` or `Protocol`
setting, in either file format; dot-files and dot-folders (`.git`,
`.gitignore`) are never looked at, so a README or a log beside the sessions
stays out of the list. `[KiTTY] fileextension` - old KiTTY's key, a dot put in
front if missing, none by default - (Application > KiTTY++ Settings > Storage &
Backup, "Session File Extension") adds an ending such as `.ktx` to every session file name, which keeps
session files apart from other files in the folder; the list shows the names
without it. It is also the file type registered for double-clicked session
files. A session file without
the suffix is still listed and gets it on its next save, a save whose file name
is already another session's is refused, and changing the suffix offers to
rename the existing files. A session named `CON`, `NUL`, `COM1` or another
Windows device name, or one ending in a dot or a space, can be saved.

**The host-key folder can be set, and classic KiTTY's putty.conf is taken
over.** Application > KiTTY++ Settings > Storage & Backup, group "This
KiTTY++", has a "Host Keys Folder" field with a folder picker
(`[KiTTY] sshhostkeys`), and below it "Host Key File Extension"
(`hostkeyextension`, old KiTTY's key too; none by default), which adds an
ending such as `.khk` to host-key file names. Its Apply renames the existing
host-key files at once. Either ending changed in kitty.ini by hand is offered
as a rename at the next start of the configuration window (Yes / No /
Storage & Backup...; after No, host keys under an old ending are not read).
A file
arriving later without it is renamed when its host is first used, and where
both names exist the one with the ending is read. A putty.conf's `Sessions`,
`sessionsuffix`, `SshHostKeys` and `keysuffix` are copied into kitty.ini once
(as `sessions`, `fileextension`, `sshhostkeys` and `hostkeyextension`) and
kitty.ini rules from then on; putty.conf itself is left as it is. 0.85.1.13's
`sessionsuffix` / `keysuffix` in kitty.ini are copied over the same way and
removed.

**Migration copes with nested session folders.** Application > Migration:
"Make a portable copy..." writes the copy's sessions into folders, "Take a
folder store..." reads every subfolder (and the store's own
`sessions`/`sshhostkeys` location and suffixes). Import of old KiTTY Folders
lists only real session files inside Sessions too, and a "Suffix to remove from
file names" field, filled in from the old store's putty.conf, shows the names
as they will be imported.

## Current scope and password portability

Portable mode now covers the normal saved-session and SSH trust/cache state used by
`kitty_portable.exe`: sessions, SSH host keys, SSH host CAs, the random seed, recent/
last-session state, jump-list state, and the update-check cache live under the portable
config directory. Where its windows were left (and how big) is kept in
`kitty_windowpos.ini` beside the exe, per monitor layout, by every program of the suite
that runs from the folder - not in the registry.

One important exception remains: a DPAPI-encrypted session password is **machine-bound**.
It protects the password at rest on the current Windows account, but it will not decrypt if
you move the folder to another PC. For passwords that travel, use a **master
password**: Application > KiTTY++ Settings > Storage & Backup, "Passwords in a
folder store", "Passwords protected with: a master password"
(`[KiTTY] PortablePasswordProtection=master`, the default in portable mode).
Copy the files and know the master password to use them on another machine.

## Why use it

- **Run from a USB stick or network share.** Drop `kitty_portable.exe` in a folder and
  your saved sessions and SSH trust cache travel with it — no install, no admin rights.
- **Less registry footprint.** Your session list, host keys/CAs, random seed, recent
  state, and update cache live in files, not normal HKCU/profile locations.
- **Trivial backup & sync.** Back up or sync the *folder* — no registry-export step for
  normal portable state.

## How to use it

1. Put `kitty_portable.exe` in its own folder (e.g. on a USB stick).
2. Run it. It stores saved sessions in `Sessions\`, SSH host keys in `SshHostKeys\`
   (or the folder `sshhostkeys` names),
   host CAs in `SshHostCAs\`, and small state/cache files in the portable config folder.
3. To pre-seed options, drop a `kitty.ini` next to the exe. The portable build already
   defaults to directory storage, so this is optional.
4. Backups are kept under `Backups\kitty-portable-latest` plus timestamped
   `Backups\kitty-portable-YYYYMMDD-HHMMSS` folders. Set `[KiTTY] portablebackupcount=0`
   to disable timestamped backups, or another number to change retention.

> Tip: the standard `kitty.exe` can be made to use file storage on demand by placing a
> `kitty.ini` with `savemode=dir` next to it. `kitty_portable.exe` simply makes that the
> built-in default.

---
*Part of the KiTTY++ port to PuTTY 0.85. Built via the CMake/MinGW cross-compile flow,
Release-optimized, stripped. The standard ZIP ships plain, uncompressed executables; the
`-upx.zip` flavour and the installers carry UPX-packed ones.*
