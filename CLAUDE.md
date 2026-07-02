# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**KiTTY** is a feature-rich fork of PuTTY (Windows SSH/Telnet client) that forward-ports the full KiTTY feature set onto PuTTY 0.84. This is a cross-platform codebase developed on WSL/Linux with MinGW to target Windows.

See [README.md](README.md), [FEATURES.md](FEATURES.md), and [KNOWN-ISSUES.md](KNOWN-ISSUES.md) for end-user documentation.

## Build & Development

### Quick build (MinGW cross-compile, WSL/Linux):

```bash
# Configure
cmake -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake

# Build entire tree (all tools + tests)
cmake --build build-mingw

# Build just the GUI
cmake --build build-mingw --target kitty

# Build portable version (file-based config, no registry)
cmake --build build-mingw --target kitty_portable
```

### Running tests:

```bash
# From build directory
cd build-mingw
ctest                              # run all tests
ctest --output-on-failure          # show test output on failure
ctest -R <test_name>               # run a specific test
```

Individual test binaries (built from `CMakeLists.txt`):
- `testcrypt` — crypto/key tests
- `test_conf` — configuration parsing
- `test_tree234`, `test_wildcard`, `test_cert_expr` — utility tests
- `test_host_strfoo`, `test_decode_utf8`, `test_unicode_norm` — string/encoding tests
- `bidi_test`, `bidi_gettype` — bidirectional text tests

Python test scripts in `test/` (require Python 3):
- `cryptsuite.py` — comprehensive crypto test suite
- `agenttest.py` — SSH agent tests
- `testcrypt.py` — interop with testcrypt binary

## Architecture & Design Principles

### The port strategy: Additive with parallel targets

The port is intentionally mostly **additive** to keep it rebasceable onto future PuTTY releases:

- **`kitty/` directory** — all KiTTY-specific modules (`kitty_*.c`) and bundled libraries
- **Parallel `kitty` CMake target** — compiles shared PuTTY sources WITH KiTTY macros defined; stock targets (putty, plink, pscp, …) compile WITHOUT
- **`MOD_*` macros** — fence KiTTY edits to upstream files (mostly `MOD_PERSO`). This keeps the delta visible and upstream files close to pristine.

### Key constraint: Multi-instance architecture

PuTTY 0.84's `windows/window.c` is multi-instance (per-`WinGuiSeat` state, no file-level globals), while KiTTY modules assume single-instance globals. A small active-seat shim in `windows/window.c` bridges the gap so kitty modules see the current seat's `conf`/`term`.

## Critical Things to Know Before Changing Shared Files

These are the most error-prone gotchas:

1. **Never add conditional struct fields across `MOD_*` boundaries.** If a struct (e.g. `Terminal`, `Config`) is compiled both with and without a feature macro across different targets, conditional fields make them disagree on layout — **silent, misaligned-memory bugs**. Always give the field unconditional storage and guard only the *code* that uses it.

2. **Config-dialog panels must be created in tree order.** Each panel path must extend the previous by one level, or `dialog.c` asserts and the config box crashes. **Always smoke-test the config dialog** after touching `kitty/kitty_config.c`:
   ```bash
   ./build-mingw/kitty.exe  # run with no session to open config
   ```

3. **PuTTY 0.84 uses the `CONF_OPTION` X-macro system** in `conf.h`. Add new options there (type + default + save keyword), not via the old enum. Some former KiTTY features are now native in upstream (e.g. always-on-top) — check `conf.h` / `window.c` before re-porting.

## Directory Layout

| Path | Purpose |
|---|---|
| `kitty/` | KiTTY modules and bundled libraries |
| `conf.h`, `config.c` | Config options + dialog tree (shared; KiTTY adds via `MOD_*`) |
| `windows/window.c` | Window/message integration (active-seat shim) |
| `ssh/`, `terminal/terminal.c` | Upstream subsystems with `MOD_*` edits |
| `crypto/`, `utils/`, `keygen/` | Shared crypto/utility infrastructure |
| `test/` | Test suite (C tests + Python scripts) |
| `doc/`, `docs/` | Documentation |

## Releases & Versioning

- Releases are tagged (e.g. `kitty-0.84.1.39-beta`)
- MSI installers are built from `windows/installer/` with WiX v5 (Authenticode-signed per-user and system-wide)
- Portable ZIP includes `kitty_portable.exe` and all command-line tools
- `CHANGELOG.md` documents all releases; `README.md` always points to the current version

## Useful References

- **[PORTING.md](PORTING.md)** — orientation for contributors (architecture deep-dive)
- **[FEATURES.md](FEATURES.md)** — full feature reference (which features are ported, how to enable them)
- **[KNOWN-ISSUES.md](KNOWN-ISSUES.md)** — known limitations and testing status
- **[CHANGELOG.md](CHANGELOG.md)** — release history and bug fixes
