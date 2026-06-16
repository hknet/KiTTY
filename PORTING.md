# Porting KiTTY onto PuTTY 0.84

This repository is a **forward-port of the entire [KiTTY](https://github.com/cyd01/KiTTY)
feature set onto current PuTTY 0.84**. KiTTY's original tree was based on PuTTY 0.76b;
this port re-applies KiTTY's additions on top of a modern, security-patched PuTTY core
(~1,200 upstream commits newer). This document is a short orientation for contributors.
End users want [`README.md`](README.md) and [`FEATURES.md`](FEATURES.md) instead.

## Design: additive, with a parallel target

The port is intentionally **mostly additive**, which keeps it tractable to rebase onto
future PuTTY releases:

- All KiTTY-specific code lives in a **`kitty/`** directory (`kitty_*.c` modules plus a
  few bundled libraries).
- A **parallel `kitty` CMake target** is defined alongside the stock `putty` target. It
  compiles the same shared PuTTY sources **with** the KiTTY feature macros defined; the
  stock targets (`putty`, `plink`, `pscp`, …) compile them **without**.
- KiTTY's edits to upstream files are fenced with **`MOD_*` macros** (`MOD_PERSO` is the
  dominant one). This keeps the delta visible and the upstream files close to pristine.

0.84's `windows/window.c` is multi-instance (per-`WinGuiSeat` state, no file-level
globals), whereas KiTTY's modules assume single-instance globals. The gap is bridged with
a small active-seat shim so the kitty modules see the current seat's `conf`/`term`.

## Building

A mingw-w64 cross-build (the project is developed on WSL Ubuntu, but any mingw-w64
toolchain works):

```sh
cmake -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake
cmake --build build-mingw --target kitty       # just the GUI
cmake --build build-mingw                       # whole tree incl. tests
```

`kitty_portable` is the same target plus `MOD_PORTABLE` (file-based config, no registry).

## Things to know before you change shared files

- **Never add a field to a struct that crosses the `MOD_*` boundary under `#ifdef`.**
  If a struct (e.g. `Terminal`) is compiled both with and without a feature macro across
  different targets, conditional fields make the two disagree on layout — an ODR trap that
  produces silent, misaligned-memory bugs. Always give the field **unconditional storage**
  and guard only the *code* that uses it. (This one has bitten the port more than once.)
- **Config-dialog panels must be created in tree order** — each panel path extends the
  previous by one level, or `dialog.c` asserts and the dialog crashes. Smoke-test the
  config box (`kitty.exe` with no arguments) after touching `kitty/kitty_config.c`.
- **0.84's `conf.h` uses the `CONF_OPTION` X-macro system** — add options there (type +
  default + save keyword) rather than the old enum.
- Some former KiTTY features are now native in upstream PuTTY (e.g. always-on-top); check
  `conf.h` / `window.c` before re-porting one.

## Layout

| Path | What |
|---|---|
| `kitty/` | KiTTY modules (`kitty_*.c`) and bundled libraries |
| `conf.h`, `config.c` | options + the config-dialog tree (shared; KiTTY adds via `MOD_*`) |
| `windows/window.c` | window/message integration (the active-seat shim) |
| `ssh/`, `terminal/terminal.c` | upstream subsystems with `MOD_*` deltas |
| `FEATURES.md` | the end-user feature reference |

## Releases

Tagged releases carry Authenticode-signed MSIs (per-user and system-wide) and a portable
ZIP. `README.md` always points at the current version.
