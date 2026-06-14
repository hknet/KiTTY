# KiTTY Portable (`kitty_portable.exe`)

`kitty_portable.exe` is the **self-contained, registry-free** edition of KiTTY. It is
functionally identical to `kitty.exe` — same SSH/Telnet/Rlogin/Raw client, same KiTTY
feature set — but it keeps *all* of its state next to the executable instead of writing
to the Windows registry.

## What's different

| | `kitty.exe` (standard) | `kitty_portable.exe` |
|---|---|---|
| Sessions / settings stored in | Windows registry (`HKCU\Software\SimonTatham\PuTTY`) | local files beside the exe (`kitty.ini` + a `Sessions` directory / `kitty.sav`) |
| Leaves traces on the host PC | yes (registry keys) | **no** |
| Needs install / admin rights | no | no |
| Roams with the Windows user profile | yes | no — travels with the folder instead |

Under the hood it's the same code with `MOD_PORTABLE` enabled, which makes KiTTY default
to **file/directory save-mode** (`GetSaveMode`) rather than the registry, and resolves its
config (`kitty.ini`) from the executable's own folder first.

## Why use it

- **Run from a USB stick or network share.** Drop `kitty_portable.exe` and its config on
  removable media and your sessions, host keys, and preferences travel with you. Plug into
  any Windows machine, your whole setup is there; unplug and it leaves nothing behind.
- **No registry footprint.** Nothing is written to `HKCU\...\SimonTatham\PuTTY`. Ideal for
  shared, locked-down, or "leave-no-trace" machines.
- **No installation, no admin rights.** It just runs.
- **Trivial backup & sync.** Back up or sync the *folder* — there's no registry-export step.
  Copy it to a new PC and everything (sessions, keys, options) comes with it.

## How to use it

1. Put `kitty_portable.exe` in its own folder (e.g. on a USB stick).
2. Run it. It stores sessions and settings in that folder — no registry, no install.
3. To pre-seed options, drop a `kitty.ini` next to the exe (KiTTY reads it from the exe's
   own directory). The portable build already defaults to local storage, so this is optional.

> Tip: even the standard `kitty.exe` can be made portable on demand by placing a `kitty.ini`
> with `savemode=dir` next to it. `kitty_portable.exe` simply makes that the built-in default,
> so it behaves portably out of the box.

## Trade-offs

- Settings do **not** roam with your Windows profile (that's the point — they live with the
  folder/stick).
- Per-machine state such as SSH host keys is stored with the portable copy, not the PC, so
  the same key cache follows you between machines.

---
*Part of the KiTTY → PuTTY 0.84 port. Built via the CMake/MinGW cross-compile flow,
64-bit, Release-optimized, stripped, UPX-compressed.*
