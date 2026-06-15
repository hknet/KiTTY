# KiTTY 0.84 Port — Config-dialog drift audit

**Status:** in progress (opened 2026-06-15). Tracks KiTTY configuration-dialog UI that
exists in stock KiTTY but was **not** ported into `kitty/kitty_config.c` during the 0.84
forward-port. Many of the *backends* for these were ported and "tested" — but only by
injecting `conf` values directly (registry sessions / programmatic `conf_set` / `.ktx`
import), never through the dialog. So the engines work when the setting is present; there
was simply **no UI to set them**.

## How the gap was found
Diff of `CONF_` keys that have a control bound in the original KiTTY config vs ours:
```
grep -oE "CONF_[A-Za-z0-9_]+" kitty-build/0.76b_My_PuTTY/config.c | sort -u  > orig
grep -oE "CONF_[A-Za-z0-9_]+" kitty-0.84/kitty/kitty_config.c       | sort -u  > ours
comm -23 orig ours
```
→ 44 KiTTY conf keys with controls in the original, absent from our config tree, plus
whole `ctrl_settitle` panels missing. (NB: this diff only catches controls bound to a
`CONF_` key — pure buttons like **Start** and browse buttons are *not* caught and must be
checked by hand.)

## Category-tree panels missing entirely (vs original KiTTY)
| Panel | Settings | Backend wired in port? |
|---|---|---|
| **Connection/Port knocking** | `portknockingoptions` | ✅ `kitty_port_knock`/`ManagePortKnocking` |
| **Connection/ZModem** (+ `/rz`, `/sz`) | `rzcommand` `rzoptions` `szcommand` `szoptions` `zdownloaddir` | ✅ `kitty_zmodem` |
| **Connection/SSH/PSCP and WinSCP** | `pscpoptions` `pscpremotedir` `pscpshell` `scp_auto_pwd` `sftpconnect` `winscpoptions` `winscpprot` `winscprawsettings` | ⚠️ launch only (`StartWinSCP`) |
| **Window/Back.&Image** | `bg_type` `bg_image_filename` `bg_image_style` `bg_opacity` `bg_slideshow` `bg_image_abs_*` | ⚠️ partial (`load_bg_bmp`; render not fully wired) |

## Controls missing inside panels we already have
- **Session/Scripting** (rutty): `script_filename` `script_char_delay` `script_crlf` `script_except` `script_cond_line` `script_cond_use`
- **Window/Colours** (TuTTY): `bold_colour` `sel_colour` `under_colour`
- **Auto-reconnect**: `failure_reconnect` `wakeup_reconnect`
- **Misc toggles/fields**: `disablealtgr` `enter_sends_crlf` `no_focus_rep` `host_alt` `scrolllines` `ssh_tunnel_print_in_title` `logtimestamp` `logtimerotation` `printclip` `proxyselection`, session **folders** (`folder`)

## Not a setting (button)
- **"Start" button** (`MOD_STARTBUTTON`) — opens a session in a new window without closing the config box. Plus any related browse/launch buttons the key-diff can't see.

## conf.h coverage
Already defined (control will compile): all of the above EXCEPT these, which must be added
to `conf.h` first: `bg_slideshow` `bold_colour` `sel_colour` `under_colour` `disablealtgr`
`failure_reconnect` `wakeup_reconnect` `printclip` `proxyselection` `script_filename`.

## Port order (backend-wired first)
1. Connection/Port knocking ✅ wired
2. Auto-reconnect (`failure_reconnect`/`wakeup_reconnect`) — verify backend
3. Connection/ZModem (+rz/sz) ✅ wired
4. Connection/SSH/PSCP and WinSCP — backend partial
5. Session/Scripting rutty controls ✅ wired
6. Window/Colours TuTTY colours
7. Window/Back.&Image — backend partial
8. Misc toggles/fields + session folders
9. "Start" button

## Verification rule (the lesson)
After porting, **walk the actual dialog tree** in a running build (isolated via
`KITTY_INI_FILE=<clean.ini>`) and confirm each control appears AND round-trips its
`conf` value — do NOT rely on "the backend symbol exists." Config panels must be created
in **tree order** (each path extends the previous by one level) or `dialog.c:~610` asserts.

## Progress (2026-06-15)
Ported into `kitty/kitty_config.c` (build-verified: compiles + links clean for kitty &
kitty_portable). **Render-not-yet-verified** — see caveat below.
- ✅ **Connection/Port knocking** (`portknockingoptions`) — backend wired.
- ✅ **Connection/ZModem** + `/rz` + `/sz` (`zdownloaddir` `rzcommand` `rzoptions`
  `szcommand` `szoptions`) — backend wired; panel gated by `GetZModemFlag()`.
- ✅ **Connection/SSH/PSCP and WinSCP** (8 keys) — backend wired (`StartWinSCP`/`SendFile`).
  Also fixed `scp_auto_pwd` INT→BOOL in conf.h + the two `conf_get_int` reads in kitty.c
  (checkbox handler asserts BOOL in 0.84).
- ✅ **Session/Scripting** — added the 5 missing rutty controls (`script_char_delay`,
  `script_cond_line`, `script_crlf`, `script_except`, `script_cond_use`); engine wired.
- Added `int GetZModemFlag(void);` forward-decl in kitty_config.c.

**VERIFICATION: DONE (and it caught a real bug).** Two ways: (1) GUI — with no other KiTTY
running and **no `KITTY_INI_FILE` override** (that override triggers the launcher/exit path),
a no-arg launch opens a fresh config box; enumerating its `SysTreeView32` cross-process showed
**Connection/ZModem (+rz/sz)** and **Connection/SSH/PSCP and WinSCP** as categories. (2) A
standalone harness (`wsl_cfgtree_unit.sh`) that links `kitty_config.c` + the control libs,
calls `setup_config_box()` directly, and dumps every control label — confirmed `Connection ::
Sequence:` (port-knock is a group under Connection, not its own node — matches the original,
whose `ctrl_settitle` was commented out), the 8 PSCP/WinSCP controls, and the 5 rutty controls.

**BUG FOUND + FIXED (a705ca0):** the ported WinSCP and rutty CR/LF radios used `label,value`
PAIRS; 0.84 `ctrl_radiobuttons` takes `label,shortcut,value` TRIPLES per button. The missing
per-button shortcut misaligned the varargs and **crashed `setup_config_box` on config-box open**
(would have shipped a config dialog that crashes). Fixed by adding `NO_SHORTCUT` to each button.
Lesson reinforced: compile-clean + idiom-match is NOT enough for varargs control APIs — open the
actual dialog. The harness is the deterministic, environment-independent way to do it.

## Expanded audit — drift BEYOND the config dialog (found 2026-06-15)
A wider sweep (original `…/0.76b_My_PuTTY/window.c` + kitty*.c vs port) found this is bigger
than missing dialog panels. Prioritised by user impact:
1. **All KiTTY command-line switches dropped** — `WinMain` calls `InitWinMain()` but the
   entire KiTTY argv loop is gone (no `-folder`, `-classname`, `-auto_store_sshkey`, `-kload`,
   `-launcher`, `-fullscreen`, `-edit`, `-savedump`, `-send`, `-pos`, `-log`, `-title`).
   Breaks scripted/launcher use. Backend mostly exists; needs the argv loop re-added.
2. **Session folders UI + launcher unreachable** — the port's `sessionsaver_data` is the
   STOCK PuTTY struct: no folder combo, no New/Del/Up-folder buttons, no **Start** button.
   `RunConfig`/`RunSession`/`InitLauncher` exist but no entry point reaches them; saved-session
   submenu is flat (stock `get_sesslist`, not KiTTY's folder tree). Flagship feature, UI gone.
3. **Auto-reconnect — DEAD** — `GetAutoreconnectFlag` has 0 live callers; no reconnect
   timer/exit hook. Conf keys + serialization only.
4. **Keyboard/mouse/Ctrl-Tab shortcuts — DEAD** — `ManageShortcuts` has 0 callers (only the
   menu *toggle* `ManageShortcutsFlag` is wired).
5. **proxyselection — DEAD** — `LoadProxyInfo` has 0 callers.
6. **Start button** — pure config-box wiring; engine (`RunConfig`) present. (Part of #2.)
7. **STUB-ONLY (serialized, never applied):** `sel_colour`/`under_colour` (absent entirely),
   `enter_sends_crlf`, `scrolllines`, `no_focus_rep`, `host_alt`, `ssh_tunnel_print_in_title`,
   `logtimestamp`/`logtimerotation`, hostkey extension (`keysuffix`).
8. **Menu items absent:** rutty script menu (Send script / recorded / halt), "New duplicated
   session", "Close+Restart session", winroll titlebar double-click gesture.
9. **Narrower:** far2l payload sync (handshake only), bg-image slideshow, telnet stored-pwd
   auto-login, global restore hotkey, capslock/pastesize/initdelay/maxblinkingtime ini keys.

WIRED & OK (for the record): transparency, antiidle, autocommand, hyperlink, per-session icon,
static bg image, zmodem, rutty scripting engine, adb, port-knock (auto at connect), WinSCP/pscp
launch. far2l = partial (announces, syncs nothing).

## Deep-tail status (2026-06-15, post-0.84.0.7)
- ✅ **far2l payload** — DONE (commit 7559990): decode + reply to every request,
  clipboard denied byte-exactly, no more remote hang. Real clipboard sync deferred.
- ⛔ **TuTTY 34-colour** — BLOCKED for blind integration. The 0.76→0.84 colour
  subsystem was **refactored**: 0.76b uses explicit `CONF_NCOLOURS`/`OSCP_NCOLOURS`/
  `OSC4_NCOLOURS` + `colour_indices_conf_to_oscp/_osc4` arrays; 0.84 is table-driven
  via `CONF_COLOUR_LIST(X)` macro (putty.h:100, OSC4_NCOLOURS=262). The 12 TuTTY
  underline/selection slots must be **re-expressed in 0.84's macro**, not pasted
  from 0.76 — and the colour *rendering* can't be visually verified in this
  harness. Needs a supervised session (eyes on the palette). Plan: extend
  CONF_COLOUR_LIST with the 12 slots under #ifdef MOD_TUTTYCOLOR, add
  bold/under/sel_colour conf keys (SAVE_KEYWORD), the config colour-panel
  checkboxes, and the window.c OSC4 underline remap; gate sel_colour (new attr
  bit + terminal.c) behind a second step.
- ⛔ **Session-folder UI** — needs the session-list folder-FILTERING REFRESH
  rewrite (GetSessionFolderName/filter_sessionname) + the Clear button's
  proxy/host symbols (absent). Non-mechanical; the Start button is already done.
- ⬜ CLI Tier-C (-kload/-cmd/-log/-edit/-classname) + Close+Restart menu — small,
  deferred.

## Root cause (how it was missed)
1. `kitty_config.c` was built additively from stock PuTTY 0.84 `config.c`, re-adding only a
   handful of KiTTY panels; the original config tree was never diffed against the port.
2. Feature tests verified the runtime path with conf values injected directly — a backend
   passes those with zero UI, so "tested" meant the engine, not the dialog.
3. Config-box testing was limited to "does it open without asserting"; the tree was never
   walked against KiTTY's.
4. The "NOTPORTED conf keys" work added `conf.h` *storage* for many keys but never the
   matching dialog controls.
