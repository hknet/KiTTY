# KiTTY Portable (`kitty_portable.exe`)

`kitty_portable.exe` is the portable edition of KiTTY. It is functionally identical to
`kitty.exe` — same SSH/Telnet/Rlogin/Raw client, same KiTTY feature set — but it keeps
saved sessions and most normal runtime state in files next to the executable/config
directory instead of the Windows registry.

## What's different

| | `kitty.exe` (standard) | `kitty_portable.exe` |
|---|---|---|
| Saved sessions stored in | Windows registry (`HKCU\Software\kapper.net\KiTTY`) | local files beside the exe/config dir (`Sessions\<name>`, one file per session) |
| SSH host keys / host CAs | Windows registry | `SshHostKeys\` / `SshHostCAs\` under the portable config dir |
| Random seed | user profile / normal PuTTY location | `PUTTY.RND` under the portable config dir |
| Recent/jump-list/update state | Windows registry / profile state | portable files such as `Jumplist` and `KiTTYState` |
| Global options (`kitty.ini`) | next to the exe / `%APPDATA%` | next to the exe/config dir |
| Needs install / admin rights | no | no |
| Roams with the Windows user profile | yes | no — the portable state travels with the folder instead |

Under the hood it's the same code with `MOD_PORTABLE` enabled, which makes KiTTY default
to **directory save-mode** (`savemode=dir`) and resolve its config (`kitty.ini`) from the
executable's own folder first. Auto-login passwords inside the session files are encrypted
at rest with Windows DPAPI, exactly as in the registry path.

## Current scope and password portability

Portable mode now covers the normal saved-session and SSH trust/cache state used by
`kitty_portable.exe`: sessions, SSH host keys, SSH host CAs, the random seed, recent/
last-session state, jump-list state, and the update-check cache live under the portable
config directory.

One important exception remains: a DPAPI-encrypted session password is **machine-bound**.
It protects the password at rest on the current Windows account, but it will not decrypt if
you move the folder to another PC. A portable, opt-in **master-password** option for
machine-independent protected passwords is still planned/not product-finished.

## Why use it

- **Run from a USB stick or network share.** Drop `kitty_portable.exe` in a folder and
  your saved sessions and SSH trust cache travel with it — no install, no admin rights.
- **Less registry footprint.** Your session list, host keys/CAs, random seed, recent
  state, and update cache live in files, not normal HKCU/profile locations.
- **Trivial backup & sync.** Back up or sync the *folder* — no registry-export step for
  normal portable state.

## How to use it

1. Put `kitty_portable.exe` in its own folder (e.g. on a USB stick).
2. Run it. It stores saved sessions in `Sessions\`, SSH host keys in `SshHostKeys\`,
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
*Part of the KiTTY → PuTTY 0.84 port. Built via the CMake/MinGW cross-compile flow,
64-bit, Release-optimized, stripped, UPX-compressed.*
