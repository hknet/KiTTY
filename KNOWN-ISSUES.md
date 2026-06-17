# KiTTY 0.84.1.4 — Known issues & limitations

The port builds **clean** (all binaries, 0 warnings, 0 errors) and ~46 KiTTY
features are working and verified. Known limitations as of this release:

## Functional limitations

- **far2l shared clipboard (GET):** writing the Windows clipboard from a remote
  `far2l` (**SET**) is verified end-to-end. The **GET** direction (remote reads
  your clipboard) and its reply transmit only over **SSH**, not over the **raw**
  protocol (a pre-existing PuTTY-over-raw behavior, not specific to far2l), so GET
  is best tested against a live `far2l` over SSH.
- **far2l clipboard privacy latch:** when **far2l shared clipboard** is set to
  **Ask** (Window → Selection), answering **OK** grants the remote access to your
  clipboard for the rest of that session — it does not re-prompt per request. Set
  it to **Disabled** if you do not want a remote `far2l` to read/write your
  clipboard.
- **adb backend & rutty scripting:** functional and verified against test
  fixtures (a fake adb server / a scripted listener), but **not** yet validated
  against a real Android device or a live remote shell.
- **Background image:** renders correctly inside the terminal cell grid; the thin
  margin strip outside the grid is still solid-filled (cosmetic).

## Packaging / cosmetic

- **Antivirus & UPX:** `kitty.exe` and `kitty_portable.exe` are UPX-compressed,
  which can trip heuristic AV/SmartScreen. The `*_nocompress.exe` variants are
  provided as an identical, unpacked fallback.
- **Version string:** binaries report `0.84.1.4-beta @ 2026-06-17`.

## New in 0.84.1.4

Suite-wide branding polish (cosmetic; no functional changes):

- **kageant** — the key-list window is now titled **"kageant Key List"** and the
  tray-icon tooltip reads **"kageant (KiTTY authentication agent)"** (was "Pageant").
- **kitty_tel.exe** — rebranded to **"KiTTYtel"** (window title, About box, configuration
  dialog, error dialogs, file properties) and now wears the KiTTY icon instead of PuTTY's.
- **kittygen-cli.exe** — gained an application icon (matching the GUI keygen) and full
  file version information (description, file/product version, product name, copyright,
  company, language); it previously exposed none.
- **Company name** — every binary now reports **KAPPER NETWORK-COMMUNICATIONS GmbH** as
  the file "Company" (was "Simon Tatham"), matching the Authenticode signing publisher.

## New in 0.84.1.3

- **Command-line tools report their own name.** `klink`/`kscp`/`ksftp` now
  identify themselves by their KiTTY names in `--version`, usage, and error
  messages (and the interactive `ksftp>` prompt) instead of the inherited
  `plink`/`pscp`/`psftp`.
- **GUI key generator (`kittygen.exe`) rebranded to KiTTY** — window title,
  About box, sub-dialog captions, and message boxes now say KiTTY/KiTTYgen, and
  the exe file-properties (Product name "KiTTY suite", etc.) match.
- **`kittygen` now defaults to EdDSA / Ed25519 (255 bits)** instead of RSA-2048
  — a stronger, modern key type out of the box. (Other types still selectable.)
- **kageant per-use key confirmation is now discoverable.** Both `kittygen`
  (a tip under the Key comment field) and `kittygen-cli` (`--help`) explain that
  including the word `confirmation` in a key's comment makes the `kageant` agent
  prompt for approval before each use of that key.
- **`kittygen-cli --help` gained usage examples**, including generating a
  passphrase-protected Ed25519 key.

## New in 0.84.1.2

- **`kittygen-cli` now shows its own name.** The console key generator's
  `--help`, `--version`, usage and error messages displayed the inherited
  `puttygen` / `PuTTYgen` program name; they now use the binary's own filename
  (`kittygen-cli`), derived from `argv[0]`. Cosmetic only — no behaviour change.

## New in 0.84.1.1

- **`kittygen-cli.exe`** — a new console-mode CLI key generator. Generate,
  convert, and inspect SSH keys from any Windows console or script without
  opening the GUI. Full `puttygen` CLI feature set (all key types, output
  formats, passphrase change, fingerprint, Argon2 KDF). No Start-menu shortcut —
  add it to PATH for convenience. Run `kittygen-cli --help` for usage.
- **PQ key-exchange warning label** — the "Warn if Key Exchange is not
  post-quantum secure" checkbox in Connection/SSH/Kex had its text truncated in
  the dialog; shortened to fit.

## New in 0.84.1.0

- **Post-quantum key-exchange warning** — when an SSH-2 session negotiates a
  key exchange that is **not** post-quantum-secure, KiTTY prints a terminal
  warning at session start (mirrors OpenSSH 10.1+), flagging exposure to
  "harvest now, decrypt later" attacks. Default **on**; a checkbox in
  Connection/SSH/Kex disables it.

## New in 0.84.0.18

This beta is the result of a full **feature audit** — a sweep of every KiTTY-over-PuTTY
feature found several that were ported into the code but never actually wired up. The
following are now functional and live-tested:

- **Automatic logon script** — the challenge/response engine was present but never
  driven; the hook (dropped during the port) is restored, so scripted prompts fire.
- **Force CR/LF on the Enter key** — the option was saved but never read at runtime.
- **Shortcuts for pre-defined commands** — the User Command menu and Ctrl+Shift+A..Z
  registry commands now run.
- **Private-key usage confirmation (Kageant)** — a key whose comment contains
  `confirmation` prompts for approval before each use.
- **Automatic saving** — the registry is exported to `kitty.sav` when you apply the
  configuration dialog.
- **Proxy choice** — the Session-panel proxy dropdown is restored (enable with
  `[ConfigBox] proxyselection=yes`).
- **Standard output to clipboard** — `ESC[5i … ESC[4i` to the *Windows clipboard*
  printer copies remote output to the clipboard again.
- **Hidden text editor** (`SHIFT+F2`) and **Session launcher** (`-launcher`) open again.
- **SSH auto-login password** — the stored password is supplied to SSH authentication;
  the storage warning/consent now appears when you *set* the password, so login is
  silent. (Note: still stored reversibly — prefer SSH keys.)
- **`-fileassoc` / `-sshhandler`** command-line switches register file/URL associations.
- **Fix:** pscp / WinSCP auto-password was being corrupted; it now passes correctly.

## New in 0.84.0.17

- **FIX — SSH interactive prompts work again (regression in 0.84.0.15–0.16).**
  Connecting with a **passphrase-protected key** or **password authentication**
  aborted with *"Terminal not prepared for interactive prompts"*. Root cause: the
  far2l clipboard fields added to the `Terminal` struct were `#ifdef MOD_FAR2L`,
  so `terminal.c` (built with MOD_FAR2L) and the rest of the terminal library
  (built without it) disagreed on the struct layout, corrupting `term->ldisc`.
  The fields are now unconditional. **Verified**: a passphrase-key SSH login now
  shows the passphrase prompt instead of the fatal error. If you hit this on a
  prior build, update.
- **Check for updates — now also a button in the config dialog** (between
  **About** and **Start**), in addition to the system-menu item added in 0.84.0.16.
- **About box → "Visit Web Site"** now opens this fork's GitHub repo
  (`github.com/hknet/KiTTY`) instead of the upstream KiTTY home page.
- **README/credits** now attribute the **far2l** extensions (putty4far2l — Ivan
  Sorokin / unxed / Ivan Shatsky; far2l by elfmz).

## New in 0.84.0.16

- **Check for updates** — the system menu (right-click the title bar / Ctrl-right-
  click) gains **"Check for updates…"**. It queries the GitHub releases API
  (`hknet/KiTTY`), compares the newest published version to the one you're running,
  and tells you whether you're up to date or offers to open the download page.
  It's **manual only** (no automatic phone-home on startup), runs on a short
  timeout, and falls back to opening the releases page in your browser if the API
  is unreachable (offline / proxy / TLS). Uses the `/releases` list rather than
  `/releases/latest` because every KiTTY build is a `-beta` pre-release.

## New in 0.84.0.15

- **far2l real shared clipboard** — the last known port gap is closed. A remote
  `far2l` can now read and write the Windows clipboard through the far2l TTY
  extension (CF_TEXT / CF_UNICODETEXT and registered formats), instead of the
  request being politely denied. A new **far2l shared clipboard** control in
  **Window → Selection** chooses **Disabled / Enabled / Ask** (default **Ask**).
  The clipboard **SET** path (remote → your clipboard) is verified end-to-end;
  **GET** transmits over SSH only (see Functional limitations).
  This support is derived from the **putty4far2l** project (far2l PuTTY
  extensions originally by Ivan Sorokin; putty4far2l by unxed, 0.78.5 port by
  Ivan Shatsky) — credited in `LICENCE` and the About box. **All known KiTTY
  port gaps are now closed.**

## New in 0.84.0.14

- **`-savedump`** — the last deferred CLI switch now works: it writes an
  (encoded) `kitty.dmp` diagnostic dump of the full configuration, then exits.
  This required porting the whole `kitty_savedump.c` module to 0.84
  (`Filename->path` → `filename_to_str`, the 0.76→0.84 BOOL/STR_AMBI conf-typing
  drift fixed, and small `GetTerminal`/`copyall`/event-log shims). The
  terminal/clipboard and event-log dump sections are omitted from a command-line
  dump (no live session at that point). **All KiTTY command-line switches are
  now ported.**

## New in 0.84.0.13

- **More command-line switches:** `-classname <name>` (set the window class —
  overrides the `kitty.ini` `KiClassName` default), `-mungestr <str>` /
  `-sendcmd <cmd>` / `-edit <file>` (utility switches that do their thing and
  exit: show a string's munged form, send a command to all running KiTTY
  windows, open the session-file editor). Verified by GUI smoke.

## New in 0.84.0.12

- **`-loginscript <file>`** — load a KiTTY login/init script from the command
  line. The script is read by a **post-window-create hook** (after the session's
  config becomes active), which is the correct point: the original in-parse call
  would dereference a not-yet-initialised global and crash. Verified: launching
  with `-loginscript` no longer crashes and the session starts normally.

## New in 0.84.0.11

- **`-kload` / `-loadfile <file.ktx>`** — load a KiTTY exported-session file
  (`.ktx`) from the command line. Both **encrypted and plaintext** `.ktx` files
  are supported (the encrypted-file decrypt path is verified by a round-trip
  test and an end-to-end launch). Pairs with the existing **Export Settings**
  menu item that writes these files.
- **Fixed: Export Settings crash** — exporting a session to a `.ktx` read the
  `SCPAutoPwd` setting with the wrong accessor (it became a boolean in the 0.84
  base), which asserted/crashed in an asserts-on build. Export now works.

## New in 0.84.0.10

- **TuTTY selection-colour rendering** — selected text is now drawn with the
  dedicated **Selected Text** / **Selected Background** palette slots instead of
  reverse-video when **"Colour selected text"** (Window → Colours) is enabled.
  Works over 24-bit truecolour cells too. (The colour *mapping* itself is only
  visually verifiable — please eyeball it; the build/structure are verified.)
- **Folder management buttons** — the Session panel gains **New folder**,
  **Del folder** and **Up folder** buttons next to the saved-session list,
  wired to the folder engine (create / delete / reorder, persisted). The Default
  folder is protected from deletion. (Folders live in the registry path; a
  browse-mode on-disk folder create is intentionally not wired.)
- **Command-line switches** — `-cmd <command>` (auto-command after login),
  `-codepage <cp>`, `-rcmd <remote command>`, and `-log <file>` (overwrite,
  flush).
- **Close + Restart** — a menu item (and the existing keyboard binding) that
  cleanly tears down the live session and immediately reconnects, in one
  toplevel callback (no close/restart ordering race).

### From 0.84.0.9 — TuTTY colours + folder filter
- **TuTTY extra colours** — the colour palette gained dedicated "Underlined
  Text", "Selected Text" and "Selected Background" slots (Window → Colours),
  with **"Colour underlined text"** / **"Colour selected text"** toggles.
  Underlined text is drawn in its own colour. (Selection-colour *rendering* is
  now wired — see "New in this beta" above.)
- **Session-folder filter** — a **Folder** droplist in the Session panel filters
  the saved-session list to the chosen folder (Default = show all).

### From 0.84.0.8 — far2l payload handling
The terminal decodes far2l extension payloads and replies to every request so a
remote `far2l` no longer hangs (real clipboard get/set still deferred).

### From 0.84.0.7 — big KiTTY restoration pass

A broad audit found that many KiTTY features had their *config UI* and/or
*engines* dropped during the 0.84 forward-port. These were restored in 0.84.0.7:

**Configuration dialog — restored panels/options:**
- **Connection → Port knocking** (knock sequence)
- **Connection → ZModem** (rz/sz commands, options, download folder)
- **Connection → SSH → PSCP and WinSCP** (protocol, options, remote dir, shell)
- **Window → Back.&Image** (style, opacity, slideshow, image file, placement)
- **Session → Scripting** — the missing rutty options (char delay, conditions,
  CR/LF, …)
- Auto-reconnect checkboxes, plus toggles: log timestamp/rotation, print-to-
  clipboard, Enter-sends-CR-LF, disable-AltGr, disable-focus-reporting, scroll
  lines, alternate host, SSH-tunnel-in-title
- **Start** button (open a session without closing the config box)

**Revived engines** (were config-only / dead before):
- **Auto-reconnect** — reconnect on connection drop and on system resume.
- **Keyboard/mouse/Ctrl-Tab shortcuts** — shortcut dispatch + Ctrl-Tab session
  switching.
- **Proxy selection** — a named saved proxy is applied at connect.
- **Background-image slideshow** — rotates images on the configured delay.

**Menus & CLI:** rutty script menu (send/stop/send-file), New duplicated
session, title-bar double-click roll-up; command-line switches `-fullscreen`,
`-xpos`, `-ypos`, `-folder`.

> ⚠️ **Please test these.** The config UI is verified to render; the engine
> behaviours (an actual reconnect, a shortcut key firing, a proxy routing,
> slideshow rotation, selection colouring) need real use to confirm — that's
> exactly what this beta is for. Report anything that misbehaves.

## Still not ported (known)
- *(None known.)* far2l **real clipboard** landed this beta (0.84.0.15) and the
  last deferred CLI switch (`-savedump`) landed in 0.84.0.14. GET-direction
  clipboard transfer is SSH-only (see Functional limitations), which is a
  protocol limitation rather than a port gap.

## Fixed in earlier betas (0.84.0.6)

- Terminal menu grouped into Window/Tools submenus; Copy/Paste removed.
- "Invert colours" / "Black on white" recolour in place (no reconfig dialog).
- "Send to tray" tray icon + click-to-restore; Full Screen shows Alt+Enter.
- Taller config dialog; About box unified; redundant Duplicate entry removed.

## Fixed in earlier betas

- Code-signed builds (Authenticode, KAPPER NETWORK-COMMUNICATIONS GmbH).
- Start-Menu / taskbar pins survive updates; shortcuts show the app icon.
- Existing KiTTY sessions found (reads `Software\9bis.com\KiTTY`; set
  `KiClassName=PuTTY` in `kitty.ini` to use PuTTY's hive). Saved PuTTY sessions
  are also shown.
- Config dialog no longer crashes on open; Scripting panel under Session and the
  Comment panel restored.
- URL hyperlink underline renders (enable with `hyperlink=yes` in `kitty.ini`).
- All build warnings cleaned.

Please report anything not listed here. Thank you for testing!
