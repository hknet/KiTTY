# KiTTY Portable (`kitty_portable.exe`)

`kitty_portable.exe` is the portable edition of KiTTY. It is functionally identical to
`kitty.exe` — same SSH/Telnet/Rlogin/Raw client, same KiTTY feature set — but it keeps
its **saved sessions** in files next to the executable instead of the Windows registry.

## What's different

| | `kitty.exe` (standard) | `kitty_portable.exe` |
|---|---|---|
| Saved sessions stored in | Windows registry (`HKCU\Software\kapper.net\KiTTY`) | local files beside the exe (`Sessions\<name>`, one file per session) |
| Global options (`kitty.ini`) | next to the exe / `%APPDATA%` | next to the exe |
| Needs install / admin rights | no | no |
| Roams with the Windows user profile | yes | no — sessions travel with the folder instead |

Under the hood it's the same code with `MOD_PORTABLE` enabled, which makes KiTTY default
to **directory save-mode** (`savemode=dir`) and resolve its config (`kitty.ini`) from the
executable's own folder first. Auto-login passwords inside the session files are
encrypted at rest with Windows DPAPI, exactly as in the registry path.

## Current scope (this port)

Portable mode currently covers **saved sessions** only. **SSH host keys and the random
seed still use the registry**, so:

- it is **not yet 100% registry-free** — host-key cache and entropy seed remain on the PC;
- a DPAPI-encrypted session password is **machine-bound** — it will not decrypt if you
  move the folder to another PC. (A portable, opt-in **master-password** option, which
  would make passwords travel, is the next planned step.)

Moving host keys and the seed into the folder, and a portable master password, are
tracked follow-ups.

## Why use it

- **Run from a USB stick or network share.** Drop `kitty_portable.exe` in a folder and
  your saved sessions travel with it — no install, no admin rights.
- **No saved-session footprint in the registry.** Your session list lives in files, not
  `HKCU\...\KiTTY\Sessions`.
- **Trivial backup & sync.** Back up or sync the *folder* — no registry-export step.

## How to use it

1. Put `kitty_portable.exe` in its own folder (e.g. on a USB stick).
2. Run it. It stores saved sessions in a `Sessions\` subfolder — no install.
3. To pre-seed options, drop a `kitty.ini` next to the exe. The portable build already
   defaults to file storage, so this is optional.

> Tip: the standard `kitty.exe` can be made to use file storage on demand by placing a
> `kitty.ini` with `savemode=dir` next to it. `kitty_portable.exe` simply makes that the
> built-in default.

---
*Part of the KiTTY → PuTTY 0.84 port. Built via the CMake/MinGW cross-compile flow,
64-bit, Release-optimized, stripped, UPX-compressed.*
