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
- `windows/storage.c` — runtime registry-root selection (KiTTY 9bis hive + PuTTY merge); see §11.
- `windows/dialog.c` — config-box **About** text branded for KiTTY (Cyril Dupont/9bis credit; web
  button → 9bis) + startup config dialog brought to front (TOPMOST-toggle + SetForegroundWindow in
  `GenericMainDlgProc` `WM_INITDIALOG`). Unconditional edits (shared lib; all shipped binaries are KiTTY).
- `version.h` — KiTTY `TEXTVER`/`SSHVER`/`BINARY_VERSION` (was the "Unidentified build" defaults).
- `windows/installer/` — the WiX v5 MSI sources (`kitty-system.wxs`, `kitty-peruser.wxs`, `build.ps1`).

## 9. Current state (read this first for new work)

- **Branch `kitty-0.84` is the GitHub default branch** of `hknet/KiTTY`; HEAD ≈ `1bcea79`. (`noglobal`
  in the local `~/kitty-0.84` repo == pushed `kitty-0.84`.) The repo has **no other meaningful remote
  history** — it's a fresh pristine-0.84 tree, history-disconnected from the old 0.76b `master`.
- **Latest release: `kitty-0.84.0.9-beta`** (pre-release), 3 **code-signed** assets: per-user MSI,
  system MSI, portable zip. Each release deletes its predecessor — only the newest tag/release remains.
  (0.84.0.8 added far2l payload handling; 0.84.0.9 added TuTTY extra colours + the session-folder
  filter droplist. See `PORT_0.84_CONFIG_GAP.md` "Deep-tail status".)
- **0.84.0.7 = the big restoration pass** (see `PORT_0.84_CONFIG_GAP.md` for the audit that drove it):
  restored config-dialog panels (Port knocking, ZModem, PSCP/WinSCP, Background-Image, full rutty
  Scripting, auto-reconnect UI, Start button, ~9 toggles); **revived dead engines** —
  **auto-reconnect** (TIMER_RECONNECT + connection-fatal/start-backend hooks + WM_POWERBROADCAST +
  notify_session_started seat-vtable hook for the SSH first-connected gate), **shortcuts**
  (ManageShortcuts in WndProc + mouse + Ctrl-Tab, cbWndExtra 0→8), **proxyselection**
  (kitty_proxy_select in start_backend), **bg slideshow** (TIMER_SLIDEBG_WIN 8710); plus menu items
  (rutty send/stop/file, New-dup, winrol dblclk) and CLI switches (-fullscreen/-xpos/-ypos/-folder in
  putty.c). New CMake defines: MOD_RECONNECT, MOD_PRINTCLIP, MOD_DISABLEALTGR, MOD_PROXY. **Engines are
  build+smoke verified; their runtime BEHAVIOUR (real reconnect, shortcut firing, proxy routing) needs
  live testing.** STILL UNPORTED: TuTTY 34-colour rendering (CONF_NCOLOURS 22→34 ripple + dlg_control_enable),
  session-folder UI, far2l clipboard payload, CLI Tier-C (-kload/-cmd/-log/-edit/-classname).
- **Config-tree verification harness:** `C:\build\wsl_cfgtree_unit.sh` links `kitty_config.c` +
  the control libs, calls `setup_config_box()` and dumps every panel/control — deterministic, GUI-free.
  Use it after any kitty_config.c change (caught a ctrl_radiobuttons pairs-vs-triples crash this round).
- **RELEASE PUBLISH BUG (avoid):** the delete-superseded-release step must match by EXACT id, one at a
  time — `... | Where-Object {tag==X}` can return an ARRAY and `DELETE /releases/$($arr.id)` builds a
  malformed multi-id URL that fails silently (it deleted/again-missed both releases in 0.84.0.7). Verify
  `@(...).Count -eq 1` before deleting; re-check `/releases` afterwards.
- **0.84.0.6 changes (terminal menu overhaul + colour-menu fix):** the long flat system/context menu
  was grouped into two submenus — **Window** (transparency, font ±, invert colours, black-on-white,
  always-visible, roll-up, send-to-tray, protect) and **Tools** (port forwardings, WinSCP, pscp, ZModem,
  print, clear log, export, shortcuts, hyperlinks); top level keeps New/Duplicate/Saved/Change-Settings,
  Copy-All, Clear-Scrollback, Reset, Full Screen, Window▸, Tools▸, Exit, About. **Copy/Paste removed**
  from the menu (select auto-copies; right-click/Shift+Ins pastes — handlers kept). **Full Screen** label
  shows `(Alt+Enter)` when that session has it enabled, and `CONF_fullscreenonaltenter` now **defaults
  true** (`conf.h`; `test_conf.c` updated to match). **"Invert colours"/"Black on white" fixed** — they
  recolour in place instead of opening the Reconfiguration dialog (see §12). Earlier 0.84.0.6-batched
  fixes: About unified (terminal-menu About → `showabout`), redundant "Duplicate KiTTY session" removed,
  send-to-tray restored (tray icon + click-to-restore via `MYWM_NOTIFYICON`), taller config box.
- **Version scheme:** display/app version `0.84.0.<sub>-beta` (set in `windows/CMakeLists.txt`
  `BUILD_VERSION`/`BUILD_TIME` for both kitty & kitty_portable targets); MSI ProductVersion numeric
  `0.84.<sub>`. **Every new build bumps the sub-release by +1** (user rule). Bump in: **`version.h`**
  (`TEXTVER` + `BINARY_VERSION` — this is what the config-box About + file Properties show),
  `windows/CMakeLists.txt` (`BUILD_VERSION`, both targets), both `windows/installer/*.wxs`
  (Name + MSI `Version` = `0.84.<sub>`), `windows/installer/build.ps1` (`-Ver` arg = MSI filenames),
  `README.md` (download links), `beta-084/README-BETA.md` + `KNOWN-ISSUES.md`.
- **Open/tabled items:** **resizable config dialog** (currently a taller fixed 402-unit box — making it
  truly resizable was deferred); **Check-Update** button (needs an update endpoint); far2l reply over
  `raw` is a pre-existing PuTTY limitation. DONE: URL underline; About-box branding (config-box About
  branded in `dialog.c` `AboutProc` in 0.84.0.4, and the terminal-menu About unified onto `showabout`
  in 0.84.0.6 — both now consistent). SmartScreen reputation for the new signing cert builds over
  downloads (OV, not EV).

## 10. Release / packaging / MSI / signing runbook

Outputs land in `C:\build\release-084\`. Helper scripts in `C:\build\`. Run WSL via
`wsl -d Ubuntu-26.04 -- bash -c "..."` (use **absolute** `/home/user/...` paths — PowerShell mangles
`~`). The full pipeline for a signed release:

1. **Build + package:** `wsl_release.sh` (configures `build-release`, builds 8 binaries) →
   `cmake --build build-release --target kitty_portable` → `wsl_package.sh` (strip/rename to k* +
   UPX kitty.exe) → `wsl_package_portable.sh` (UPX kitty_portable.exe).
2. **MSIs:** `windows/installer/build.ps1 -Ver 0.84.0.<sub>-beta` (**Windows** — `dotnet tool install
   -g wix --version 5.0.2`) → both MSIs into `release-084/` via **WiX v5** (`wix build -arch x64
   -bindpath release-084`). NOT wixl/v3 anymore (migrated in 0.84.0.5). Stay on **WiX v5** — v6/v7
   require the paid OSMF EULA. The `.wxs` use the v4 schema (`<Package>` root, `<StandardDirectory>`,
   `<MediaTemplate>`), **non-advertised** shortcuts (`Target="[INSTALLFOLDER]x.exe"`) each with
   `<ShortcutProperty Key="System.AppUserModel.ID" Value="kappernet.X"/>`. UpgradeCodes fixed/committed
   (per-machine `69EA2DD5-…`, per-user `578952A6-…`); component GUIDs auto (path-derived, stable).
   `File Source=` uses bare filenames resolved by `-bindpath`.
3. **Code signing** (Azure Trusted Signing aka "Artifact Signing"; tooling already installed: .NET SDK
   + Azure CLI + the `sign` tool at `%USERPROFILE%\.dotnet\tools`). `az login` first (identity needs
   the *Trusted Signing Certificate Profile Signer* role). Account `REDACTED-account`, endpoint
   `https://REDACTED-endpoint/`, profile `REDACTED-profile` (PublicTrust). **Order matters:**
   (a) sign all `release-084/*.exe` *after* UPX:
   `sign code artifact-signing <exes> -act azure-cli -ase https://REDACTED-endpoint/ -asa REDACTED-account -ascp REDACTED-profile -fd sha256 -d "KiTTY (PuTTY 0.84 fork)" -u https://github.com/hknet/KiTTY`
   (b) **rebuild the MSIs** (`windows/installer/build.ps1`) so they embed the signed exes;
   (c) sign the two MSIs the same way. Verify with `Get-AuthenticodeSignature` (Status=Valid, signer
   `REDACTED Publisher`, timestamped).
4. **Zip:** `wsl_beta_zip.sh` stages `release-084` + `beta-084/README-BETA.md`+`KNOWN-ISSUES.md` and
   writes `SHA256SUMS`; then `Compress-Archive` it to `release-084/kitty-0.84.0.<sub>-beta.zip`.
5. **Publish:** tag `kitty-0.84.0.<sub>-beta` (push via **Windows git over the `\\wsl.localhost\…` UNC
   path** — WSL has no SSH key; Windows git has the agent), then GitHub REST API (auth = the stored
   Windows git credential via `git credential fill` → `Authorization: Bearer`; there is **no `gh`**).
   As of 0.84.0.6 that credential is a **fine-grained PAT** (`REDACTED_pat_…`, user `hknet`, scope repo
   `hknet/KiTTY` **Contents: Read+Write** — sufficient; no Deployments/Packages), with GCM forced to PAT
   mode (`git config --global credential.https://github.com.gitHubAuthModes pat`). This replaced the GCM
   **OAuth app token**, which rotated on every fetch (regenerate/destroy/create churn in the security
   log + repeated GitHub authorization windows). **Fetch the token once per script and reuse in-memory**
   — do NOT call `git credential fill` per call. PAT **expires 2026-07-15**; renew via
   `printf "protocol=https\nhost=github.com\nusername=hknet\npassword=<PAT>\n" | git credential approve`
   (a Slack reminder is scheduled for 2026-07-13). Create release (prerelease=true), upload assets to
   `https://uploads.github.com/repos/hknet/KiTTY/releases/<id>/assets?name=<n>` (build the upload URL
   explicitly — the `upload_url` template trick fails). `/releases/latest` API 404s for a prerelease-only
   repo but the web URL works.

## 11. Registry storage (KiTTY hive + PuTTY merge)

`windows/storage.c` uses a **runtime** registry root (was the compile-time `PUTTY_REG_POS`). Default
`Software\9bis.com\KiTTY` (matches stock KiTTY, so existing KiTTY sessions/host-keys are found and
PuTTY isn't touched). `kitty.c` `InitWinMain` calls `kitty_set_registry_root(!stricmp(KiTTYClassName,
"PuTTY"))` after parsing `kitty.ini` `KiClassName`, so `KiClassName=PuTTY` switches to
`Software\SimonTatham\PuTTY`. For convenience, `open_settings_r` falls back to the PuTTY hive and
`enum_settings_start` **merges+dedups** both hives; writes/deletes stay on the primary hive (PuTTY hive
is read-only). NB: the `test_*.ps1` scripts create sessions under `SimonTatham\PuTTY` and still work
via that fallback.

## 12. Process lessons (session 12)
- **Always smoke-test the config box** (`kitty.exe` with no args) after editing `kitty_config.c` — its
  panels MUST be created in tree order (each new path extends the previous by one level) or
  `dialog.c:~610` asserts and the dialog crashes. `-load` feature tests don't exercise this.
- **Use non-advertised shortcuts (WiX v5), not advertised (wixl)**: advertised shortcuts are iconless
  AND key Start/taskbar **pins to the per-ProductCode descriptor**, so pins break on every upgrade.
  Non-advertised shortcuts (stable `Target` path) show the exe's icon and carry a stable
  `AppUserModelID` (`kappernet.*`) so pins survive. This is why 0.84.0.5 left wixl for WiX v5.
- **Sign after UPX**, and rebuild MSIs after signing exes so they embed signed payloads.
- KiTTY is **MIT** (its own `LICENCE.TXT`, © Cyril Dupont) — web "GPL" claims are wrong; `LICENCE`
  credits both Tatham and Dupont.

### Process lessons (session 13 — 0.84.0.6)
- **Anything added to `window.c` that only KiTTY defines must be `#ifdef MOD_PERSO`-guarded** — `window.c`
  is shared and also compiled into `putty`/`pterm`/`puttytel`, which do **not** link `kitty_bridge.c`.
  The `force_reconf` silent-apply use in the `IDM_RECONF` handler broke those targets until guarded.
  These non-KiTTY targets only build during a **full** release build, so a kitty-only rebuild won't catch
  it — build `plink pscp psftp pageant puttygen pterm puttytel` too before publishing.
- **KiTTY colour-menu actions (Invert / Black-on-white) apply via `force_reconf`.** `NegativeColours` /
  `BlackOnWhiteColours` (kitty.c) mutate the active-seat `conf` colours, set the global `force_reconf=0`,
  then `PostMessage(WM_COMMAND, IDM_RECONF)`. The `IDM_RECONF` handler must honour `force_reconf`: when 0,
  skip `do_reconfig()` (the dialog) and fall straight through to the apply/repaint tail (`init_palette` +
  `InvalidateRect` + `reset_window`). Stock PuTTY's handler always opens the dialog — that was the
  "Invert opens the Reconfiguration dialog" bug. `WM_COMMAND` and `WM_SYSCOMMAND` share the same
  `switch (wParam & ~0xF)`, so the internal `PostMessage` reaches the handler.
- **Runtime-verify GUI changes on the actual binary** (the advisor's catch) — static checks (builds,
  signatures, MSI extract) miss menu/handler bugs. Drive the system menu by `PostMessage(hwnd,
  WM_SYSCOMMAND, IDM_*, 0)` (e.g. `IDM_FONTNEGATIVE`=0xB080, `IDM_RECONF`=0x0050); enumerate the menu via
  `GetSubMenu`/`GetMenuString`; detect the config dialog by window class **`PuTTYConfigBox`**. Use
  **`PostMessage`, not `SendMessage`**, for `IDM_RECONF` — `do_reconfig` is modal and `SendMessage`
  blocks until the dialog closes. PuTTY paints via `WM_PAINT`, so `PrintWindow`/`WM_PRINTCLIENT` won't
  capture the client area — screen-DC `BitBlt` over the window rect works if you must sample pixels.
- **GUI testing on the user's own machine: isolate and never mass-kill.** The user runs a real KiTTY +
  "KiTTY Session Manager"; its single-instance/handoff means a bare test launch can hand off to the live
  instance (process stays alive, no window of its own). Isolate the test with a clean ini via
  **`KITTY_INI_FILE=<clean.ini>`**. Copy the test exe to a **distinct name** (e.g. `kitty_086test.exe`)
  and only ever `Stop-Process` by that name or your own PID — **never** `Get-Process kitty | Stop-Process`
  (that kills the user's sessions; this rule cost real trust once).
- **GitHub auth = static PAT, not the OAuth app** (see §10 step 5). The OAuth-app token rotated on every
  `git credential fill`, spamming the security log and popping authorization windows; a fine-grained PAT
  + `gitHubAuthModes pat` fixed it. Fetch the token once per script.
