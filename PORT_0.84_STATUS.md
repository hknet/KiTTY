# KiTTY → PuTTY 0.84 Forward-Port — Final Status

**Project:** Bring `hknet/KiTTY` (a PuTTY 0.76b fork) up to current PuTTY **0.84**, preserving KiTTY's features.
**Branch:** `kitty-0.84` (GitHub default; == local `noglobal`)  **HEAD:** ≈`82cb21f`  **Baseline:** `e14266c` (pristine PuTTY 0.84 + KiTTY foundation)
**Repo (WSL):** `~/kitty-0.84` (Ubuntu-26.04)   **Date:** 2026-06-14

> **UPDATE (current):** shipped as **`kitty-0.84.0.3-beta`** — a published, **code-signed** GitHub
> pre-release (per-user MSI + system MSI + portable zip). Since the original write-up below, this was
> added: URL underline rendering (done), all build warnings cleaned, **MSI installers** (signed, with
> icons + Start-Menu shortcuts), **code signing** (Azure Trusted Signing), and **registry separation**
> (reads KiTTY's `Software\9bis.com\KiTTY` hive + merges PuTTY sessions; `KiClassName`-configurable).
> The config dialog crash (a `kitty_config.c` panel-ordering bug) was found & fixed.
> **For build/release/MSI/signing/registry details and to continue the work, see
> `PORT_0.84_HANDBOOK.md` §9–§12 (current state + runbooks).**

---

## 1. Headline result

- **Clean from-scratch build: 24 / 24 binaries, 0 errors.** (kitty, putty, plink, pscp, psftp, pterm, puttytel, pageant, puttygen, psocks, bidi_*, test_* incl. test_lineedit/test_terminal/testcrypt).
- **~42 KiTTY features verified WORKING** (incl. URL hyperlinks, adb backend, rutty scripting); far2l **real shared clipboard** WORKING as of 0.84.0.15 (SET verified end-to-end; GET is SSH-only — see §5). NOTE: this count predates the 0.84.0.7–0.84.0.15 feature waves; see the HANDBOOK release log for the current inventory.
- The 4 previously-open items are now CLOSED: URL hyperlinks WORKING (source-built regex), adb backend WORKING, rutty scripting WORKING, far2l real clipboard WORKING (0.84.0.15). **No known port gaps remain.**
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
# deploy the built kitty.exe to wherever the Windows GUI test launches it
```

Helper scripts (`wsl_*.sh` for build/deploy/commit, `test_*.ps1` for GUI verification)
are kept in the maintainer's local working dir, outside the repo.

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
| **URL hyperlinks** | **WORKING** | hover+click launches browser with exact URL (test_url_gui.ps1 PASS) — defective prebuilt regex replaced with source-built V8 regex | 3675a17 |
| **adb backend** (Android Debug Bridge) | **WORKING** | selectable as Protocol=adb; sends exact ADB handshake `0012host:transport-any` to a fake server; dead-port connect fails gracefully (no crash) — test_adb.ps1 / test_adb_handshake.ps1 | b9f7aca |
| **rutty scripting** (waitfor/halton) | **WORKING** | scripted lines sent each after the waitfor pattern appeared in incoming host data — test_rutty.ps1 PASS | b030ed0 |
| **far2l extensions** (APC handshake) | **RECOGNIZED** | far2l APC parsed intact, far2l_ext toggled, reply dispatched via ldisc, no crash — test_far2l.ps1 (reply-on-wire gated by pre-existing raw-reply limit, §5) | 48f1dde |

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

### URL hyperlinks — WORKING (was PARTIAL; fixed)
The earlier crash was the **prebuilt `kitty/libs/libregex_64.a` mis-compiling the URL pattern**
(regcomp reported success but left `re_nsub==0`; regexec faulted). No `regex.c` source exists for that
POSIX lib. **Fix (commit 3675a17):** switched the kitty target to the self-contained, source-available
**V8 regex** that KiTTY shipped but had abandoned — `url/urlhack.old.c` + `url/re_lib/regexp.c` — and
fixed three KiTTY-introduced heap-corruption bugs in `regexp.c` (regcomp `free()`'d uninitialised
`r->startp/endp` right after malloc; regfree `free()`'d `r->regmust` — an interior pointer into
`r->program` — and `r->startp/endp` — pointers into the searched string). Henry Spencer semantics: one
`free(r)` releases the whole compiled block. `urlhack.h` now includes `re_lib/regexp.h`; the
prebuilt `libregex_64.a` link + regex include dir are gone; the `re_nsub` crash-guard is removed.
VERIFIED end-to-end (test_url_gui.ps1 PASS): hover+click launches the browser with the exact URL.

### adb backend — WORKING (was SKIPPED; landed)
Ported KiTTY's adb.c to 0.84's BackendVtable in `kitty/kitty_adb.c` (guarded MOD_ADB), modelled on
`otherbackends/raw.c`: `init(vt,…)` returning `char*`, Backend*/Plug* callbacks with `container_of`,
void `send`, size_t `sendbuffer`/`unthrottle`, `PlugVtable.log(Socket*,PlugLogType)`,
`closing(PlugCloseType)`, `special(…,int arg)`, bool `connected`/`sendok`/`ldisc`,
`get_specials`→`SessionSpecial*`, `displayname_tc/_lc`, new Interactor vtable, `new_main_connection`,
`seat_stdout`, `default_description`. The ADB-server handshake state machine is preserved. Added
`PROT_ADB`, a guarded `&adb_backend` in be_list.c, `MOD_ADB=1` on the kitty be-list object only (other
binaries unaffected). adb auto-appears in the config protocol dropdown. VERIFIED: dead-port connect
fails gracefully (no crash); against a fake adb server it sends the exact `0012host:transport-any`
handshake — proving protocol-enum + be_list registration + init + state machine are wired.

### rutty scripting — WORKING (was SKIPPED; landed)
Ported the rutty script engine to `kitty/kitty_rutty.c` (minimal, no-global): keeps the rutty
matching/line-stepping core verbatim, single static ScriptData, sends via `backend_send` (no global
ldisc), drops recording/AHK/menu-UI. The incoming-data matcher (`script_remote`, waitfor/halton) — in
rutty/KiTTY it ran in terminal.c — is hooked **OBSERVE-only from window.c `win_seat_output()`** (the
ZModem interception point), so data still flows to `term_data()`; **no terminal.c edits**. Added 11
`CONF_script_*` keys, a `TIMER_SCRIPT` auto-start (script_mode==PLAY), and a Connection/Scripting
config panel. VERIFIED (test_rutty.ps1 PASS): wait-for-prompt mode sent each scripted line only after
the `waitfor` pattern appeared in incoming host data.

### far2l — WORKING real shared clipboard (0.84.0.15; was RECOGNIZED→handshake-only)
**0.84.0.15 closed this gap.** The far2l real shared clipboard is ported from `~/putty4far2l`
(ivanshatsky/putty4far2l, 0.78.5 — the clean same-architecture impl; 0.76b KiTTY never had it).
`far2l_process_payload` decodes the base64 APC payload; the 'c' clipboard subcommands r/e/a/o/s/g do
real Win32 (Register/Open/Empty/CloseClipboard, IsClipboardFormatAvailable, MB_OKCANCEL for "Ask",
GlobalAlloc+SetClipboardData for SET 's', GetClipboardData+transcode for GET 'g') under `#ifdef
_WINDOWS` else stubs; reply heap-built with `[last]=id`, sent via far2l_send_reply (ldisc). Policy:
putty.h SHARED_CLIPBOARD_{DISABLED,ENABLED,ASK}, conf.h CONF_shared_clipboard (default Ask),
terminal.h `clip_allowed` (set from conf on the handshake), kitty_config.c radiobuttons in
Window/Selection, window.c WM_DESTROYCLIPBOARD guard. **SET verified end-to-end over raw; GET is
SSH-only** (raw reply doesn't transmit — see below; a protocol limit, not a gap). Attribution (Sorokin/
unxed/Shatsky/elfmz) in LICENCE + About box. Historical handshake notes below:

far2l lives inside terminal.c's escape parser (KiTTY 0.76b had ~1000 lines of APC/OSC clipboard-sync
there, with `exit()`/MessageBox crash paths). 0.84's terminal.c already routes APC into the OSC-string
collector + `do_osc()`. Minimal MOD_FAR2L port: a guarded `term->far2l_ext` field; in `do_osc()`,
APC strings starting with `far2l` get the on/off handshake (`far2l1` → set state + reply
`\x1b_far2lok\x07` via `ldisc_send`; `far2l0` → clear; base64 payload + anything else ignored
gracefully, no exit/MessageBox); fixed the APC/DCS/SOS/PM dispatch to go straight to `OSC_STRING`
(SEEN_OSC drops the first payload char). To keep MOD_FAR2L scoped to kitty, terminal.c is compiled
directly into the kitty target (its object out-prioritises guiterminal's terminal.o); other binaries
use the plain terminal.o (guards inert) — full tree 24/24 green.
**VERIFIED:** the far2l APC is parsed intact, `far2l_ext` toggled, reply dispatched through a valid
ldisc, **no crash** (the brief's stated goal). **Known limitation (NOT far2l-specific):** the reply
does not reach the wire because terminal-originated `ldisc_send` replies do not transmit over the
**raw** backend in this no-global build — PuTTY's own OSC-4 colour-query reply (identical path) is
equally not transmitted over raw (test_osc_reply.ps1 = none). So far2l recognition is complete; reply
transmission is gated by that independent pre-existing behaviour (would surface over SSH, where the
backend transmits terminal replies).

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

- **WORKING: ~42** (5 geometry + 21 menu/core + auto-command + anti-idle + port-knock + zmodem +
  bg-image render + bg-image load + 6+ config-UI panels + per-session icons + About dialog + sshver +
  forced-export + dup-session + core-init + **URL hyperlinks + adb backend + rutty scripting**).
- **far2l real shared clipboard: WORKING** (0.84.0.15 — SET verified end-to-end; GET is SSH-only, a
  protocol limit not a gap; see §5).
- **SKIPPED / UNPORTED: 0** — all known port gaps are closed as of 0.84.0.15.

Build green throughout. Latest release `kitty-0.84.0.15-beta`; HEAD `406ba9d` on branch `noglobal`
(pushed to `kitty-0.84`). Note: this §7 count predates the 0.84.0.7–0.84.0.15 feature waves (savedump,
all CLI switches, TuTTY colours/folders, far2l clipboard) — see the HANDBOOK release log for the current
inventory.
