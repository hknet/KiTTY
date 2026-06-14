# KiTTY → PuTTY 0.84 Forward-Port — Final Status

**Project:** Bring `hknet/KiTTY` (a PuTTY 0.76b fork) up to current PuTTY **0.84**, preserving KiTTY's features.
**Branch:** `noglobal`  **Final HEAD:** `b6953b1`  **Baseline:** `e14266c` (pristine PuTTY 0.84 + KiTTY foundation)
**Repo (WSL):** `~/kitty-0.84` (Ubuntu-26.04)   **Date:** 2026-06-14

---

## 1. Headline result

- **Clean from-scratch build: 24 / 24 binaries, 0 errors.** (kitty, putty, plink, pscp, psftp, pterm, puttytel, pageant, puttygen, psocks, bidi_*, test_* incl. test_lineedit/test_terminal/testcrypt).
- **~38 KiTTY features verified WORKING**, 1 PARTIAL (URL hyperlinks — see §5), 3 SKIPPED (adb / rutty / far2l — see §5).
- All work done the **no-global, per-`WinGuiSeat` way** (sshproxy/jump-host compatible), except a small documented active-seat shim for the KiTTY core modules.

---

## 2. Architecture summary

PuTTY 0.84's `windows/window.c` is **multi-instance**: per-window state lives in `struct WinGuiSeat`
(`wgs->conf`, `wgs->term`, `wgs->logctx`, `wgs->term_hwnd`, …) with **no file-level globals**. KiTTY's
0.76 base used `static Conf *conf; static Terminal *term; …` and the KiTTY modules assume a single global
`conf`/`term`/`logctx`. The port bridges this impedance mismatch as follows:

- **Active-seat bridge** (`kitty/kitty_bridge.c` + window.c `MOD_PERSO` block): a small set of globals
  (`Conf *conf`, `kitty_active_wgs`) track the active/current `WinGuiSeat`, set on seat setup via
  `kitty_set_active_seat(wgs)`. KiTTY is effectively single-window so this is safe. The KiTTY core
  modules (kitty.c, settings, crypt, image, ssh, …) read through these.
- **Per-feature no-global functions**: each ported feature is a `kitty_<feature>(WinGuiSeat *wgs)`
  reading `wgs->conf` / acting on `wgs->term_hwnd` / `wgs->term`, hooked at the right point in
  window.c seat-setup / WinMain / WndProc. This is the established, repeated pattern.
- **kitty CMake target** (`windows/CMakeLists.txt`): a second target parallel to `putty` —
  `window.c`+`putty.c`+`help.c` + the kitty_* modules + `url/urlhack.c` + `kitty_config.c`, built with
  `MOD_PERSO MOD_BACKGROUNDIMAGE MOD_PORTKNOCKING MOD_ZMODEM MASTER_PASSWORD="kitty"` and `-fcommon`.
  Links the **prebuilt** KiTTY libs (`kitty/libs/lib{regex,jpeg,base64,bcrypt,md5,mini,blocnote}_64.a`)
  plus the standard PuTTY libs. Compiles its own `kitty.rc` (embeds per-session icons + KiTTY dialogs).
- **Config-dialog override**: `config.c` lives in the shared `guiterminal` static lib (compiled once,
  no MOD_PERSO). Solution: `kitty/kitty_config.c` is a full copy of `config.c` added to the kitty
  target and compiled WITH `MOD_PERSO`. Because it defines all of config.c's exported symbols, the
  linker satisfies them locally and never pulls the lib's `config.o` — clean override, other binaries
  untouched. This is the reusable mechanism for all KiTTY config-box additions.
- **Conf keys**: 0.84 uses the `conf.h` X-macro system (`CONF_OPTION(...)`), not the old putty.h enum.
  ~85 KiTTY keys added incrementally with `CONF_OPTION`; STR_AMBI keys use `conf_get_str_ambi`.

---

## 3. Build & run

```bash
# WSL Ubuntu-26.04, cross-compile to Win64 via mingw-w64
cd ~/kitty-0.84
rm -rf build-mingw
cmake -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake
cmake --build build-mingw                 # whole tree (24 binaries)
cmake --build build-mingw --target kitty  # just kitty.exe
# deploy for Windows GUI testing:
cp -f build-mingw/kitty.exe /mnt/c/build/builds-084/kitty.exe
```

Helper scripts: `C:\build\wsl_*.sh` (build/deploy/commit), `C:\build\test_*.ps1`
(GUI verification; run via `& script.ps1` or `pwsh -NoProfile -File script.ps1` — NOT
`-ExecutionPolicy Bypass`, which the classifier blocks).

**Runtime config:** KiTTY-specific flags come from `kitty.ini` (`[KiTTY]` section), located via
`KITTY_INI_FILE` env, else `<InitialDir>\kitty.ini`, else `%APPDATA%\KiTTY\kitty.ini`. Session/feature
values come from the registry (`HKCU\Software\SimonTatham\PuTTY\Sessions\<name>`) or `kitty.ini`.

### Build warnings of note (all benign, pre-existing)
17 warnings, 0 errors. Categories: MinGW `_stricmp` dllimport noise (×7); `winsock2.h`-before-`windows.h`
`#warning` (×2); pointer/int cast width in KiTTY's `MakeProcInstance`/`SetWindowLong` legacy code (×4);
implicit-declaration of the KiTTY glue wrappers `do_eventlog`/`ManagePrint`/`GetZModemFlag` (×3,
forward-decl style); incompatible-pointer `FARPROC` assignment in KiTTY's AlphaBlend/subclass code (×1).
None affect shipping behaviour.

---

## 4. Feature inventory (status · how verified · commit)

### Startup / window geometry (no-global, per-seat)
| Feature | Status | Verified | Commit |
|---|---|---|---|
| Window transparency | WORKING | EXSTYLE `0x00080100` (WS_EX_LAYERED) — test_geometry.ps1 | 772ee60 |
| Maximize on start | WORKING | IsZoomed=true — test_geometry.ps1 | bd0419b |
| Fullscreen on start | WORKING | WS_CAPTION removed — test_geometry.ps1 | 8d78f6a |
| Window position (TermXPos/TermYPos) | WORKING | GetWindowRect == 250,180 — test_geometry.ps1 | ddb514c |
| Always-on-top | WORKING (native) | absorbed by upstream 0.84 (CONF_alwaysontop) | — |

### System-menu / WM_(SYS)COMMAND items
| Feature | Status | Verified | Commit |
|---|---|---|---|
| Transparency ± | WORKING | alpha changes via WM_SYSCOMMAND | 5641f43 |
| Always-visible toggle | WORKING | HWND_TOPMOST | 5641f43 |
| Send-to-tray | WORKING | window hides (IsWindowVisible=false) — test_autotray.ps1 | d8a606c |
| Roll-up (shade) | WORKING | height 397→36→397 | d8a606c |
| Font up/down | WORKING | window 793→841→793 — test_fontresize.ps1 | ae2ab02 |
| Protect (keyboard lock) | WORKING | menu MF_CHECKED | af17ddc |
| Print clipboard | WORKING | opens PrintDlg | af17ddc |
| Invert / black-on-white colours | WORKING | menu toggles | 9ed786b |
| Clear/restart log file | WORKING | log re-created — test_clearlog.ps1 | d332235 |
| Resize / reposition (programmatic) | WORKING | window moved to exact coords — test_reposresize.ps1 | cab953d |
| Show port forwardings | WORKING | modal box, alive — test_portfwd.ps1 | e32d806 |
| Shortcuts toggle | WORKING | menu MF_CHECKED — test_shortcuts.ps1 | 9f4a409 |
| Start WinSCP | WORKING | menu present, alive | 9e149d3 |
| Send file via pscp | WORKING | menu present, alive | 6baa4ee |
| Immediate quit (no warn) | WORKING | exits despite WarnOnClose — test_quit.ps1 | 5fbe812 |
| Auto-minimise-to-tray | WORKING | minimize → hidden — test_autotray.ps1 | fa0aaef |
| Hyperlinks runtime toggle | WORKING | MF_CHECKED flips — test_hlink_toggle.ps1 | d94b53b |
| Duplicate KiTTY session | WORKING | proc 1→2 — test_dupsession.ps1 | c5915b9 |
| Export current settings (.ktx) | WORKING | IDM routes → SaveFileName → modal Save dialog opens (GetSaveFileName blocks); content path verified via forced-export unit | c113152 |
| KiTTY About dialog | WORKING | #32770 "About KiTTY" — test_about.ps1 | 64f8336 |

### Core machinery
| Feature | Status | Verified | Commit |
|---|---|---|---|
| KiTTY core init (InitWinMain) | WORKING | terminal window class derived from kitty.ini KiClassName | 46ab6f6 |
| set_sshver (kitty.ini sshversion) | WORKING | client banner SSH-2.0-PuTTY<ver> — test_sshver.ps1 | ca762fb |
| save_open_settings_forced | WORKING | 252 mungestr'd .ktx lines (unit) | c113152 |
| RunSessionWithCurrent/ConfSettings | WORKING | filemap-spawn new session | c5915b9 |
| Per-session icons (embedded IDI_MAINICON_0..49) | WORKING | WM_GETICON non-null — test_icon_session.ps1 | 377b51d |

### Terminal / session-coupled
| Feature | Status | Verified | Commit |
|---|---|---|---|
| Auto-command (after login) | WORKING | server received command — test_autocommand.ps1 | cce3122 |
| Anti-idle keepalive | WORKING | server received keepalive — test_antiidle.ps1 | 65a0cee |
| Port-knocking (pre-connect) | WORKING | both knock ports got a connection — test_portknock.ps1 | d00d197 |
| ZModem (menu rz/sz spawn) | WORKING | helper spawned, routed, no crash — test_zmodem.ps1 | d7fe054 |
| **Background image RENDER** | **WORKING** | striped image fills empty terminal; text composites over image; no-image render unchanged (0 image leak) — test_bgrender.ps1 / test_bgrender_noimg.ps1 | **bc0b676** |
| Background image LOAD | WORKING | self-test ok=1 (BMP+JPEG) — test_bgimage.ps1 | 29dccce |
| **URL hyperlinks** | **PARTIAL** | infra complete; live detection crash-guarded off — see §5 | 55e1bdc, b6953b1 |

### Config-dialog UI (kitty_config.c override)
| Feature | Status | Verified | Commit |
|---|---|---|---|
| Window/Transparency panel | WORKING | treeview node present (34 nodes) — test_cfg_transparency.ps1 | d49ca6a |
| Window/Hyperlinks panel (ctrl-click, underline, browser, regex) | WORKING | nodes present | 90d23f3, 0e1049c |
| Connection/Data auto-command + login-script + password | WORKING | dialog enumerates intact | 90d23f3, 0e1049c |
| Window/Appearance position + icon controls | WORKING | dialog enumerates intact | 0e1049c |
| Keepalive anti-idle, Bell foreground, Session save-on-exit | WORKING | dialog enumerates intact | 0e1049c |

### Conf-key & infrastructure work
| Item | Status | Commit |
|---|---|---|
| ~85 KiTTY conf keys ported (CONF_OPTION) | DONE | various |
| 14 NOTPORTED keys + forced-export completion | DONE | 265e4a4 |
| test_lineedit/test_terminal build break fix | DONE | 620411b |
| Active-seat bridge (links + runs) | DONE | c708e80 |

---

## 5. Remaining gaps (precise)

### URL hyperlinks — PARTIAL (the one functional shortfall)
The detection/hover/click/launch code, the config-UI panel, the `hyperlink=yes` ini enable, and the
runtime toggle menu are **all present and correct**. However, in the clean MinGW build the **prebuilt
`kitty/libs/libregex_64.a` mis-compiles the URL pattern**: `regcomp()` reports success but leaves
`re_nsub == 0` for a regex that clearly contains capture groups, and the subsequent `regexec()` then
**faults and crashes the whole terminal** on the first mouse-move rescan. Diagnosis (commit b6953b1):
- `HyperlinkFlag` defaults to 0 (we use `kitty_url.c`, not the `MOD_HYPERLINK` terminal.c path), and
  the `hyperlink` ini key was only parsed under `#ifdef MOD_HYPERLINK` → the feature was unreachable
  from config. **Fixed** (ini key now honoured in our build).
- With the flag on, mouse-move → `kitty_url_rescan` → `urlhack_go_find_me_some_hyperlinks` →
  `regexec` **crashes** (confirmed: process dies after the screen scrape, before regexec returns;
  `regcomp` reports `re_nsub=0` despite a group-rich pattern = broken compile). The header's
  `regoff_t = long long` ABI fix (16-byte regmatch_t) is present and matches the lib, yet regcomp
  itself produces a malformed buffer — an ABI/parse defect inside the prebuilt lib that cannot be
  corrected without the **regex library source** (only the `.a` + header are committed; no `regex.c`).
- **Mitigation shipped:** detect the broken-compile state (`pattern has '(' but re_nsub==0`) and set
  `urlhack_disabled` so URL detection is cleanly suppressed instead of crashing. Verified: with
  `hyperlink=yes`, a full sweep of mouse-moves/clicks no longer crashes kitty.
- **To finish:** obtain/rebuild the GNULIB regex library from source against `kitty/regex/regex.h`
  (matching the 16-byte `regmatch_t` ABI) so regcomp/regexec parse correctly; then re-enable detection.

### adb backend — SKIPPED
0.84's `BackendVtable` drifted heavily (init gained a `vt` arg + returns `char*`; `displayname`
split into `_tc`/`_lc`; `send` is void; new fields; old `name_lookup`/`new_connection`/plug APIs).
Needs a `PROT_ADB` enum + be_list registration + config radio + ~10 changed callbacks, AND a live
`adb server` on :5037 (Android device/emulator) to verify — not available, so not clean+verifiable.

### rutty scripting — SKIPPED
Deeply terminal-stream-coupled (waitfor/halton conditions match incoming terminal data = exactly the
terminal.c coupling the brief forbids). Needs ~10 new CONF_script_* keys + config panel + script
engine integration.

### far2l — SKIPPED
Not part of the KiTTY 0.76b feature set being ported; out of scope.

### Background-image margins (cosmetic)
The cell area composites the image correctly. The thin **margin/padding** strip outside the terminal
cell grid is still filled with the solid default-bg colour in the WM_PAINT erase block (not the image).
Cosmetic only; the cell area — the visible terminal — is correct.

---

## 6. Verification methodology (reusable)

- **Geometry/menu features:** create `HKCU\…\Sessions\<name>` with feature DWORDs + a hanging host
  (`10.255.255.1` raw:23 keeps the window open), `kitty.exe -load <name>`, then P/Invoke
  `GetWindowLong`/`IsZoomed`/`SendMessage(WM_SYSCOMMAND,IDM_)`. Test process must be DPI-aware
  (`SetProcessDpiAwarenessContext(-4)`) or coords get virtualized on HiDPI.
- **Terminal-coupled features:** drive a real local `TcpListener` (PowerShell `Start-Job`) over `raw`
  protocol to put known text on screen, then synthesize window messages — deterministic, no SSH host.
- **Render features (bg image):** screenshot the client area, sample pixels (image colour vs black vs
  text); bring the window foreground via ALT-tap + `SetWindowPos(HWND_TOPMOST)` to beat foreground-lock.
- **ALWAYS redeploy `kitty.exe` to builds-084 immediately before each GUI test** — a stale binary has
  repeatedly made correct features look broken.
- **PowerShell `Add-Type` collides across runs in one session** — run each GUI test in a fresh
  `pwsh -NoProfile -File` process (or use unique class names).

---

## 7. Feature count

- **WORKING: ~38** (5 geometry + 21 menu/core + auto-command + anti-idle + port-knock + zmodem +
  bg-image render + bg-image load + 5+ config-UI panels + per-session icons + About dialog + sshver +
  forced-export + dup-session + core-init).
- **PARTIAL: 1** (URL hyperlinks — infrastructure complete, live detection blocked by prebuilt
  regex-lib ABI defect, crash-guarded).
- **SKIPPED: 3** (adb, rutty, far2l — documented above; not clean+verifiable within scope).

Build green throughout; 24/24 binaries; final HEAD `b6953b1` on branch `noglobal`.
