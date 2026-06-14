# KiTTY → PuTTY 0.84 Port — Engineering Handbook

> Definitive guide for building, maintaining, and re-basing this port. Written for humans
> **and** future agents. Companion docs in the repo: `PORT_0.84_STATUS.md` (feature inventory),
> `PORT_0.84_INVENTORY.md` (original delta analysis), `PORT_0.84_PHASE2_STEP1.md` (drift map).
> Project memory: `…/REDACTED/projects/REDACTED/memory/kitty-084-port.md`.

---

## 1. What this is

KiTTY is a feature fork of **PuTTY 0.76** (a hand-assembled mid-2021 snapshot). This project
forward-ports the entire KiTTY feature set onto **PuTTY 0.84** (≈1,186 upstream commits later).
Result: a `kitty` build tree based on pristine 0.84 with ~42 KiTTY features working, building via
**CMake + MinGW cross-compile** in WSL.

- Working tree: `~/kitty-0.84` (WSL Ubuntu), git repo, branch **`noglobal`**.
- Branches: `master` = pristine-0.84+foundation baseline (`e14266c`); `bridge` = intermediate
  global-shim PoC; `noglobal` = the real port (current).
- Upstream reference checkouts: `~/putty-upstream` (all PuTTY tags), `~/kitty-build` (the original
  KiTTY 0.76b repo — source of the `kitty_*` modules, prebuilt libs, icons).

## 2. Why it was hard (the three structural problems)

1. **No clean upstream ancestor.** KiTTY's "0.76b" tree mixes states that never coexisted in
   PuTTY's history (combined `crypto/sha256.c`, the `ssh/` subdir, pre-`dss→dsa`-rename symbols).
   So a `git merge`/version-step was impossible → strategy is a **feature-delta forward-port**.
2. **Globals → multi-instance refactor.** 0.84 rewrote `windows/window.c` so config/terminal/
   logctx live in a per-window `struct WinGuiSeat` (`wgs->conf`, `wgs->term`, …); there are NO
   file-scope globals. KiTTY's ~30 modules assume a single global `conf`. (See §4 the bridge.)
3. **Build system + API churn.** 0.84 uses CMake (KiTTY used a hand `MAKEFILE.MINGW`), the conf
   system became an X-macro table (`conf.h`), `Filename`/`FontSpec`/`win_set_title`/`appname`/
   backend vtable all changed shape, and `config.c` is now a shared static lib.

## 3. Architecture of the port (key mechanisms)

- **The `kitty` CMake target** (`windows/CMakeLists.txt`, added parallel to `putty`): compiles
  `window.c`+`putty.c`+the `kitty/` modules+`url/urlhack`+`kitty_bridge.c`+`kitty_config.c`+
  `kitty.rc`; `target_compile_definitions(kitty PRIVATE MOD_PERSO MASTER_PASSWORD="kitty")`;
  `target_compile_options(kitty PRIVATE -fcommon)`; links KiTTY's **prebuilt `*_64.a`** libs
  (regex was later replaced — see §5) from `kitty/libs/`. `kitty_portable` = same + `MOD_PORTABLE`.
- **The active-seat bridge** (`kitty_bridge.c` + a MOD_PERSO block in `window.c`): defines the
  global `Conf *conf` KiTTY modules expect, plus `kitty_set_active_seat(wgs)` called right after the
  primary seat's `term_init` (~window.c WinMain) which points the globals at the active `WinGuiSeat`.
  KiTTY is effectively single-window so this is sound. `kitty_bridge.c` `#include`s `kitty.h` and
  holds the **wrapper functions** (`kitty_send_to_tray`, `kitty_font_resize`, …) that window.c calls
  via 1-line forward decls — because **window.c cannot include kitty.h**.
- **Shared-lib override technique** (CRITICAL, reused 3×): 0.84 puts `config.c`, `be_list.c`,
  `terminal.c` in static libs compiled ONCE without MOD_PERSO. To get a MOD_PERSO variant for the
  kitty target only, compile a **target-local copy** of the .c into the kitty target: the linker
  satisfies the symbols from the local object and never pulls the lib's — clean override, **zero
  impact on putty/plink/etc.** Used for: `kitty/kitty_config.c` (config dialog), per-target
  `be_list.c` (`MOD_ADB`), and `terminal.c` compiled into the kitty target (far2l).
- **Conf keys**: 0.84 uses `CONF_OPTION(name, VALUE_TYPE(...), DEFAULT_*, SAVE_KEYWORD("..."))`
  X-macros in `conf.h` — adding an entry auto-generates the key + load/save. All KiTTY conf keys
  are appended there.
- **Resources**: `windows/kitty_rc_additions.h` (KiTTY IDM_/IDD_/IDI_ defines, `#ifndef`-guarded,
  included from `windows/putty-rc.h`); `windows/kitty.rc` (per-session ICON resources + KiTTY
  About dialog), compiled into the kitty target.

## 4. Issues encountered & how they were fixed (the gotcha catalogue)

| # | Symptom | Root cause | Fix |
|---|---|---|---|
| 1 | `kitty_help.h`: "missing terminating "", stray '\\'" | CRLF line endings — the generator appends `\r\n\` and CRLF corrupts the C string | Build from an **LF** checkout (clone in WSL, not the Windows clone). Repo-wide: `core.autocrlf` matters. |
| 2 | `multiple definition of TrayIcone/conf/...` at link | GCC ≥10 defaults `-fno-common`; KiTTY's tentative globals collide | Add **`-fcommon`** (CFLAGS / `target_compile_options`). |
| 3 | `conflicting types for RegCopyTreeA` | KiTTY's `RegCopyTree` macro-expands (via windows.h) to the now-existing Win32 `RegCopyTreeA` | Rename KiTTY's to `kitty_RegCopyTree`. |
| 4 | hundreds of "unknown type CONF_x" | 0.84 conf is an X-macro table, not the old putty.h enum | Port keys as `CONF_OPTION(...)` in `conf.h`. |
| 5 | `Filename has no member 'path'` | 0.84 `Filename` uses `wpath/cpath` | `filename_to_str(fn)` (or `->cpath`). |
| 6 | `win_set_title` too few args | gained a `codepage` param | `win_set_title(tw,title,CP_ACP)`. |
| 7 | `assignment of read-only appname` | 0.84 `const char *const appname` | make it `const char *appname` (putty.h + be_list.c; also fix test/*.c). |
| 8 | link: 12 undefined KiTTY-glue symbols | window.c's static globals/wrappers removed in 0.84 | the **bridge** (§3): global conf + wrappers. |
| 9 | crash on font resize | KiTTY freed `conf_get_fontspec()`'s pointer; 0.84 returns conf's **internal** pointer | build a NEW `fontspec_new(...)`, `conf_set_fontspec` (copies), free only the new one. |
| 10 | assert in `conf_get_str` | `CONF_username`/`remote_cmd` are **STR_AMBI** in 0.84 | use `conf_get_str_ambi`. |
| 11 | type mismatch | `CONF_rxvt_homeend` changed INT→BOOL (general conf type-drift) | match the new type when porting save/load. |
| 12 | URL detection crash | prebuilt `libregex_64.a` is defective (regcomp ok but `re_nsub==0`, regexec faults; also 64-bit `regoff_t` vs `int` header) | **dropped the prebuilt lib**; switched urlhack to the source-available V8 regex (`url/re_lib/regexp.c`) and fixed 3 heap-corruption bugs in it. |
| 13 | background image didn't render | 0.84 `do_paint`/`do_text_internal` draws opaque cells (`ETO_OPAQUE`), no compositing | minimal hook in `do_text_internal`: `BitBlt` from `backgrounddc` for default-bg cells, draw glyphs `opaque=false`; inert when no image. |
| 14 | config controls couldn't be added | `config.c` is a shared lib compiled w/o MOD_PERSO | **shared-lib override** (§3): `kitty/kitty_config.c`. `conf_checkbox_handler` asserts on INT keys → local `kitty_checkbox_int_handler`; editbox ctx uses `ED_INT`/`ED_STR`. |
| 15 | adb backend wouldn't link | 0.84 `BackendVtable` drift (init sig, `displayname_tc/_lc`, void `send`), old `name_lookup`/plug APIs | port modelled on `otherbackends/raw.c`; register via per-target `be_list.c` with `MOD_ADB`. |
| 16 | ZModem spawn did nothing | KiTTY 0.76b's `xyz_SpawnProcess` was a **no-op stub**; no auto-detect existed | implemented a real pipe+CreateProcess spawn; intercept I/O in `win_seat_output()` + message loop (no terminal.c edits). |

### Process lessons (equally important — these cost real time)
- **Always redeploy before a GUI test**: `cp -f ~/kitty-0.84/build-mingw/kitty.exe /mnt/c/build/builds-084/`. A stale binary made correct code look broken (~8 wasted iterations once).
- **DPI**: make the test process DPI-aware (`SetProcessDpiAwarenessContext(-4)`) or `GetWindowRect` returns *virtualized* coords on HiDPI (off by the scale factor).
- **Modal menu items** (print, export → file dialog): trigger with `PostMessage`, not `SendMessage` (SendMessage blocks on the modal loop).
- **GUI tools have no console `-V`** (kitty/kittygen/kageant/pterm) — `--version` pops a usage dialog. Verify with `Start-Process` + process-alive; only `klink`/`kscp`/`ksftp` (console) print `-V`.
- **PowerShell mangles inline multi-line `bash -lc "..."`** — always write a `.sh` to `C:\build\wsl_*.sh` and run `wsl -d Ubuntu-26.04 -- bash /mnt/c/...`.
- **One agent at a time on this tree** — shared working tree/git/build dir; parallel agents corrupt each other. Delegate sequentially (keeps the orchestrator's context clean).
- **Verify, don't assume**: re-test after seemingly-unrelated changes (URL hyperlinks silently regressed to a crash and wasn't re-tested for 4 sessions).

## 5. Remaining issues / known gaps (even minor)

- **far2l reply over `raw`**: the `far2l` APC handshake is recognized/parsed, but its reply (and PuTTY's own OSC-4 reply) is not transmitted over the **raw** protocol in this build — a **pre-existing PuTTY-over-raw behavior**, not a far2l defect; works over SSH.
- **adb / rutty verified against fakes**: adb tested against a fake TCP adb server (correct handshake, graceful fail); rutty tested against a scripted listener. Not validated against a real Android device / live shell (none available in the build env).
- **URL underline rendering**: detection, hover hand-cursor, and ctrl/click-to-open work; if the *visual underline* of the URL text isn't drawn, it shares the `do_text_internal` paint path used for the background image — extend there if wanted (cosmetic).
- **Build warnings**: ~17 benign mingw warnings (`_stricmp` dllimport, winsock2 include-order, legacy ptr/int casts, KiTTY-glue implicit decls). None affect correctness; could be cleaned for tidiness.
- **UPX & AV**: `kitty.exe`/`kitty_portable.exe` are UPX-compressed (smaller, but UPX can trip antivirus heuristics). `*_nocompress.exe` copies are shipped for that reason.
- **Not integrated into `hknet/KiTTY`**: this is a fresh 0.84-based tree, structurally unlike the `0.76b_My_PuTTY/` layout in the original repo. Integration/publishing is a separate decision (see §7).

## 6. How to build / rebuild

Prereqs (WSL Ubuntu): `gcc-mingw-w64-x86-64 make upx-ucl cmake ninja-build` (all installed).

```bash
cd ~/kitty-0.84
# Debug/dev build of the GUI:
cmake -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake
cmake --build build-mingw --target kitty            # -> build-mingw/kitty.exe
# Whole tree (all 24 binaries incl tests):
cmake --build build-mingw
# Release set:
cmake -B build-release -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target kitty kitty_portable plink pscp psftp pageant puttygen pterm
```
Package (strip + rename to KiTTY conventions + UPX the GUI): see `C:\build\wsl_release.sh`
and `wsl_package.sh`. Release output lands in `C:\build\release-084\`
(`kitty.exe`, `kitty_portable.exe`, `klink/kscp/ksftp/kageant/kittygen.exe`, `*_nocompress.exe`).

Test scripts (run via `& script.ps1`): `C:\build\test_*.ps1` (per-feature) and
`wsl_*.sh` verifiers. Always `cp -f` the fresh binary to `builds-084/` before GUI tests.

## 7. How to rebase onto the next PuTTY (e.g. 0.85)

The port is intentionally **mostly additive** (a `kitty/` dir + a parallel CMake target), with a
**small, well-known set of touched upstream files**. To move to the next release:

1. **Get pristine new PuTTY**: `git -C ~/putty-upstream fetch --tags`; export the new tag, e.g.
   `git -C ~/putty-upstream archive 0.85 | tar -x -C ~/kitty-0.85`. Init a git repo there; this is
   the new baseline (like `master`/`e14266c` was for 0.84).
2. **Replay the additive layer (low conflict)** — copy from the 0.84 tree:
   - `kitty/` (all the modules, `kitty_bridge.c`, `kitty_config.c`, `kitty_settings_forced.c`,
     `kitty_zmodem.c`, `kitty_url.c`, `kitty_adb.c`, `kitty_rutty.c`, `kitty/libs/`, `kitty/url/`).
   - `windows/kitty_rc_additions.h`, `windows/kitty.rc`.
   - The `conf.h` KiTTY `CONF_OPTION` block (append-only section — copy verbatim, then reconcile any
     conf keys the new version added/renamed).
   - The `kitty`/`kitty_portable` blocks in `windows/CMakeLists.txt`.
3. **Re-reconcile the touched upstream files (the real work)** — diff these between 0.84-pristine and
   our 0.84, then re-apply the deltas onto the new version (they're the high-coupling spots):
   - `windows/window.c` — the MOD_PERSO blocks: bridge globals + `kitty_set_active_seat`, the
     seat-setup feature hooks (transparency/maximize/fullscreen/position/icons/bg/autocmd/antiidle/
     portknock), the menu `AppendMenu` additions, the `WM_COMMAND/WM_SYSCOMMAND` cases, the
     `win_seat_output` interception (zmodem/rutty), `do_text_internal` bg-image hook, `WM_TIMER` cases.
     **Verify `struct WinGuiSeat` still exists** and its fields (`conf/term/term_hwnd/ldisc/logctx`).
   - `kitty/kitty_config.c` — it's a **copy of `config.c`**; regenerate it from the NEW `config.c`
     and re-insert the KiTTY control additions (don't carry the old copy forward — config.c changes).
   - `terminal.c` (compiled into the kitty target for far2l) — same: re-copy NEW terminal.c, re-add
     the far2l `do_osc` hook. Check `do_text_internal`/`do_paint` for the bg-image hook too.
   - `be_list.c` (per-target, `MOD_ADB`) and `utils/version.c` (mutable `sshver[40]` for set_sshver).
   - `putty.h` (`appname` non-const) and `test/*.c` (matching appname qualifier).
4. **Re-validate the gotcha catalogue (§4)** against the new version — these API shapes are the most
   likely to have drifted again: conf VALUE_TYPE/STR_AMBI of any key you touch, `Filename`/`FontSpec`
   structs, `win_set_title`/seat/backend vtables, `do_paint` internals, `BackendVtable`.
5. **Build incrementally, feature-by-feature, keep the build green**, using the test scripts. The
   per-feature commits on `noglobal` are your checklist of what must work.

**Tip**: keep the port as a `noglobal`-style branch on top of each pristine baseline, so the next
rebase can `git diff baseline..noglobal` to see exactly the KiTTY delta to carry forward.

## 8. File map (what's KiTTY-specific in the tree)
- `kitty/kitty*.c|h` — the KiTTY feature modules (sessions, settings, image, registry, etc.).
- `kitty/kitty_bridge.c` — active-seat bridge + menu-action wrappers + (formerly-stub) glue.
- `kitty/kitty_config.c` — MOD_PERSO copy of config.c (config dialog override).
- `kitty/kitty_settings_forced.c` — settings export (`save_open_settings_forced` + helpers).
- `kitty/kitty_url.c|kitty_zmodem.c|kitty_adb.c|kitty_rutty.c` — feature implementations.
- `kitty/url/`, `kitty/{base64,bcrypt,md5,mini,jpeg,zmodem,...}/`, `kitty/libs/*_64.a` — bundled libs.
- `windows/kitty.rc`, `windows/kitty_rc_additions.h` — KiTTY resources.
- `conf.h` (KiTTY CONF_OPTION block), `windows/CMakeLists.txt` (kitty/kitty_portable targets),
  `windows/window.c` (MOD_PERSO hooks), `be_list.c`, `utils/version.c`, `putty.h` — touched upstream.
