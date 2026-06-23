# KiTTY 0.84.1.28 — Known issues & limitations

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
- **Version string:** binaries report `0.84.1.28-beta @ 2026-06-24`.

## New in 0.84.1.28

- **"Update available" notice at session start (opt-in, on by default).** A small
  background check refreshes the latest-known release version; when you open a
  session, KiTTY prints a one-line notice at the top of the terminal if a newer
  version is available (then use *Check for updates* to install). It honours the
  same channel rule — a stable build is not nudged toward betas. The check runs on
  a worker thread and only updates a cached version; the notice itself is rendered
  synchronously at the *clean top of a session*, so it never corrupts a full-screen
  program (vim/htop/tmux/…). One consequence: a brand-new release is flagged on the
  *next* start (the notice is at most one launch behind). Turn it off in
  **Session → "Check for updates on startup"**.
- **Updater channel detection** now reads GitHub's own `prerelease` flag rather
  than matching "beta" in the tag text (more robust).

## New in 0.84.1.27

- **In-app updater respects your release channel.** If you are on a **stable**
  release, *Check for updates* no longer silently installs a **beta** — it tells
  you the newest available build is a beta and asks first (proceed with caution).
  Beta builds continue to track the newest beta as before. *(Current limitation:
  the check looks at the single newest release, so a stable user is offered/warned
  about the newest beta rather than the newest stable; once stable releases resume,
  full stable-only channel filtering will be added.)*

## New in 0.84.1.26

- **Faster failover on a dead/unreachable address (capped connect timeout).** A
  connection attempt that gets no response — e.g. an IPv6 address that has gone
  unreachable on a flaky path — now fails over to the next candidate address after
  **~5 seconds** instead of hanging on Windows' default ~21 s SYN timeout. This
  removes the long freeze on auto-reconnect and on first connect to a multi-address
  host. The cap only ever triggers on a silently-dropped connection (a working
  connect completes in well under a second), so normal connections are unaffected.
  *(Groundwork toward a fuller Happy-Eyeballs parallel IPv6/IPv4 connect, planned.
  Note: this speeds up recovery; it does not change why a link drops.)*

## New in 0.84.1.25

- **Fix: opening Settings no longer crashes (regression in 0.84.1.24-beta).**
  A malformed control in the new *Word navigation* option corrupted the dialog's
  argument list, so KiTTY crashed whenever the configuration box was built —
  i.e. on **Change Settings** (and on the initial Settings dialog). Fixed. The
  **Ctrl + mouse-wheel font zoom** and **Word navigation (Alt/Ctrl/Both)** options
  introduced in 0.84.1.24 are now usable.

## New in 0.84.1.24

- **Ctrl + mouse wheel zooms the terminal font.** Hold **Ctrl** and scroll the
  wheel up/down over the terminal to grow/shrink the font on the fly (clamped to a
  sane range). A KiTTY classic, restored on the 0.84 core.
- **Configurable word-navigation modifier (Terminal → Keyboard).** A new
  *"Word navigation (Left/Right arrows)"* option — **Alt** (default, = PuTTY),
  **Ctrl**, or **Both** — chooses which modifier emits the xterm word-navigation
  sequence (`ESC[1;3 D/C`) that shells bind to back/forward-word. Applies in
  xterm-bitmap arrow mode (the default); it is a no-op in VT52/application-cursor
  modes, where the modifier isn't encoded.
- **Security hardening (cont.): the WinSCP command builder is now fully
  length-bounded.** The remaining FTP / options / proxy append paths were converted
  to bounded appends, so the whole builder is overflow-safe. No behaviour change for
  normal SSH-session WinSCP launches.

## New in 0.84.1.23

- **kageant passphrase prompt opens over the requesting terminal.** When a
  terminal asks kageant to unlock an encrypted key, the "enter passphrase" dialog
  now appears centred over that terminal window (the foreground window at the time
  of the request) rather than at the centre of the screen. Falls back to centre if
  the window can't be determined.
- **Security hardening (cont.): the WinSCP launcher command is length-bounded** (no
  fixed-buffer overflow). WinSCP's `scp://…` URL format is unchanged, so launches
  behave exactly as before. This completes the transfer/launch command-builder
  hardening across pscp, plink and WinSCP.

## New in 0.84.1.22

- **Upgrades relaunch the tray apps.** When an in-place MSI upgrade closes apps to
  replace their files, Windows' Restart Manager only restarts apps that registered
  for it. KiTTY now registers: **kageant** and the **tray launcher** are brought
  back after the upgrade, and a terminal opened from a **saved session**
  (`-load NAME` / `@NAME`) is relaunched with that session so it reconnects.
  Ad-hoc/host-typed terminals are intentionally not relaunched (a blank window
  would be noise, and a live SSH session can't be restored).

## New in 0.84.1.21

- **Security hardening: transfer command builders quote and bound their inputs.**
  The pscp/plink builders (upload, download, plink, clipboard-get) now treat each
  session-derived value (password, key path, source/target paths, remote command)
  as a single, properly **argv-quoted** argument and append it with **length
  bounds**. So a quote (or other unusual character) in a session field can no
  longer inject an extra command-line switch, and over-long fields truncate rather
  than overflow a fixed buffer. Raw "extra options" fields stay unquoted by design.
  This completes the command-builder hardening started in 0.84.1.19 (no shell).
  *(The WinSCP launcher uses WinSCP's own URL format and is hardened separately.)*

## New in 0.84.1.20

- **File upload (pscp) works again.** Dropping a file on the terminal, or the
  Send-file menu, had been broken across the whole 0.84 series. Three faults were
  fixed: (1) an assertion crash — `username`/`remote command` became `STR_AMBI`
  string types in PuTTY 0.84 and KiTTY still used the plain string accessor;
  (2) a stray byte appended to a dropped file's path so pscp couldn't find it;
  (3) the terminal window no longer accepted dropped files (the "forbidden"
  cursor) because the drop registration was lost in the port. Together with the
  0.84.1.19 no-shell change, uploads run again and without shell exposure.
- Follow-up still planned: argument quoting + length bounding in the transfer
  command builders (so an unusual character in a session field can't inject an
  extra command-line switch); and DPAPI for stored passwords.

## New in 0.84.1.19

- **Security hardening (cont.):** the external **pscp** file-transfer and **plink**
  command builders now launch the tool **directly (CreateProcess)** instead of via
  the Windows command shell (`system()`). Characters in session fields (password,
  host, username, remote command, …) are therefore taken literally and can no
  longer be interpreted as shell commands. Each transfer/command opens in its own
  console window. A follow-up will add command-buffer length bounding and argument
  quoting (so a quote in a field can't inject extra switches).

## New in 0.84.1.18

- **Security hardening** (from an internal code review):
  - The in-app updater locks the downloaded installer against modification from
    signature verification through launch (closing a time-of-check/time-of-use
    gap), and only downloads the installer over HTTPS.
  - Fixed a buffer-size mismatch when reading a stored password from the registry;
    the registry string reader now NUL-terminates and bounds-checks its output.
- Note: a broader security pass is still planned — see the project notes
  (auto-login password storage remains reversible obfuscation; prefer SSH keys /
  kageant).

## New in 0.84.1.17

- **In-app updater — install type detected correctly.** A real system or per-user
  install was sometimes misreported as a *portable* copy, so the updater refused
  to auto-install. It now determines the install type by querying Windows
  Installer for KiTTY's stable product **UpgradeCode** (per-machine vs per-user),
  rather than inferring it from the executable's path — which is robust to
  non-default install folders and localized Windows. The path heuristic is kept
  only as a last-resort fallback.

## New in 0.84.1.16

- **kageant — reorder loaded keys.** The key-list window gained **Move Up** and
  **Move Down** buttons (select a single key, then move it). The list order is
  the order keys are *offered* to servers, so you can control which key is tried
  first. The chosen order is saved per key (by fingerprint) and restored on the
  next start, including when **Load keys on startup** is enabled. The agent's
  internal signing-key lookup is unaffected — only the offer/display order
  changes.
- **kageant — passphrase prompt takes focus.** When a terminal asks kageant to
  use an encrypted key, the on-demand passphrase prompt now comes to the
  foreground with keyboard focus, instead of opening unfocused behind the
  terminal window (kageant is a background process, so Windows' foreground lock
  previously kept focus on the terminal).

## New in 0.84.1.15

- **In-app updater.** "Check for updates" can now download **and install** the
  right asset for how this copy was installed — the per-user MSI, the system MSI
  (with an elevation prompt), or the portable ZIP (download-only). The downloaded
  installer is run **only** after its Authenticode signature is verified to be a
  genuine KAPPER-signed artifact (valid trust chain **and** matching publisher);
  anything that fails is deleted and never executed.
- **Duplicate Session / New Session focus.** The spawned window now comes to the
  foreground and takes focus instead of opening behind the current window.
- **Launcher About box is silent.** Opening the tray launcher's About box no
  longer plays the Windows "asterisk" system sound.
- **Transparency is a clean per-session option.** Window transparency is off by
  default (sessions start fully opaque) and remains configurable per session via
  *Window → Transparency*. Setting `transparency=no` in `kitty.ini` is now a
  complete master switch that removes both the config panel **and** the
  system-menu Transparency +/- items.
- **Saved-session comment box keeps its text after Load.** The read-only
  "comment of selected session" box no longer blanks once you press Load.

## New in 0.84.1.14

- **Saved-session list: single-click fills the name box.** Clicking a session in
  the saved-sessions list now copies its name into the "Saved Sessions" edit box,
  so Save/Load act on it without retyping — e.g. select **Default Settings** and
  Save to update it directly (previously you had to type the name in by hand).
- **Windows file-info rebranding.** Several binaries still carried PuTTY-era
  VERSIONINFO. kageant's file description ("PuTTY SSH authentication agent" — shown
  e.g. in Task Manager's Startup list) is now "kageant (KiTTY SSH authentication
  agent)"; kitty_pterm's description no longer says "PuTTY-style"; and the renamed
  CLI tools report their shipped names (klink/kscp/ksftp/kageant/kitty_pterm)
  instead of the old Plink/PSCP/PSFTP/Pageant/pterm. *(The version string still
  notes the "PuTTY 0.84 base" lineage, and the copyright still credits Simon
  Tatham — both deliberate upstream attribution.)*

## New in 0.84.1.13

- **kageant — "Load keys on startup" (opt-in, off by default).** A new tray item.
  When enabled, kageant remembers the file paths of the keys you have loaded
  (auto-tracked) and re-adds them at the next login, added **encrypted/deferred**
  (the passphrase is only requested on first use). Enabling also installs an
  autostart entry (`HKCU\…\Run\KiTTY-kageant`), so it replaces a manual kageant
  Startup shortcut. Only key-file *paths* are stored — never passphrases or key
  material.
- **Sessions can be hidden from the launcher.** A new per-session option, **"Hide
  this session from the launcher"** (Session panel), excludes a session from the
  `kitty -launcher` tray menu while keeping it in the normal session list.
  *(Registry/file save modes; the directory save mode is not yet covered.)*

## New in 0.84.1.12

- **Taskbar icon fixed.** The terminal window's taskbar button showed a blank
  sheet because the process declared the AppUserModelID `SimonTatham.PuTTY`,
  which did not match the installer's pinned-shortcut id; it now declares
  `kappernet.KiTTY`, so the taskbar button uses the proper KiTTY icon.

## New in 0.84.1.11

- **Reconnect restores the window icon.** When a session dropped, the title-bar
  icon switched to the broken-connection icon and stayed that way after a
  successful reconnect; it is now restored to the normal icon on (re)connect.
- **Session comment now shows for pre-existing sessions.** The read-only
  "Comment of selected session" box in the Session panel read the comment only
  from the first registry hive that held the session; comments authored by an
  older KiTTY (stored in the legacy hive) now display without re-saving — the
  comment is read across all hives, preferring the first non-empty value.
- **Launcher About box:** fixed character artifacts (the `(c)` and `-` were
  shown as mojibake).
- **TCP keepalives default to on** for newly-created sessions (helps keep
  connections alive through NAT/firewall idle timeouts). Existing saved sessions
  keep their stored setting.

## New in 0.84.1.10

- **kageant — optional Windows OpenSSH integration (off by default).** A new tray
  menu item, **"Register as Windows OpenSSH agent"**, lets kageant act as the agent
  for the Windows `ssh.exe`. When ticked, kageant writes `%USERPROFILE%\.ssh\kageant.conf`
  (an `IdentityAgent` line pointing at its named pipe) and adds a marker-delimited
  managed block to `%USERPROFILE%\.ssh\config` that `Include`s it; unticking removes
  the managed block again. It is **off by default** so kageant never alters your SSH
  configuration unless you ask. Only kageant's own marker block is ever touched — the
  rest of your `~/.ssh/config` is preserved byte-for-byte, written atomically, and a
  one-time `config.kageant.bak` backup is taken before the first edit. The setting is
  remembered (registry). *(Replaces the previous manual setup of a Startup-shortcut
  `-openssh-config` flag plus a hand-edited `Include` line.)*

## New in 0.84.1.9

- **Configuration dialog — WinSCP executable path field.** *Connection → SSH → PSCP and
  WinSCP* now has a **"WinSCP executable path"** box. It shows the stored path (or the
  auto-detected `%ProgramFiles%\WinSCP\WinSCP.exe` as a hint) and lets you point KiTTY at a
  WinSCP install in a non-standard location. Stored globally in `kitty.ini [KiTTY] WinSCPPath`.
- **Session comment — multiline + live preview.** The **Comment** panel is now a multiline
  (≈5-line) box, and the **Session** panel shows a read-only **"Comment of selected session"**
  box below the saved-sessions list that updates as you click through your sessions. Comment
  newlines round-trip to both registry and file/directory storage.

## New in 0.84.1.8

- **Registry namespace consolidated to `Software\kapper.net\KiTTY`.** KiTTY now stores
  its sessions and settings under our own registry namespace. **Your existing sessions
  migrate automatically** on first run (a one-time, non-destructive copy — your old
  `Software\9bis.com\KiTTY` data is left untouched). Sessions are still found even if the
  copy is skipped: KiTTY reads, in order, **`kapper.net\KiTTY` → `9bis.com\KiTTY` →
  `SimonTatham\PuTTY`** (your hive wins; PuTTY sessions remain loadable), and any edit is
  written to the new namespace. This also fixes a class of latent bugs where launcher,
  kageant and other features read the wrong (stock PuTTY) hive — a forward-port artifact
  where the original KiTTY registry override had been dropped. *(Verified: migration,
  3-hive read fallback + precedence, write-destination and idempotency tested against the
  live registry.)*
- **kageant — passphrase + About dialogs rebranded.** The deferred-decryption passphrase
  prompt and the About box now read "kageant" instead of "Pageant" (IPC names that PuTTY
  clients rely on are deliberately unchanged).

## New in 0.84.1.7

- **Launcher — new sessions now take focus.** Starting a saved session from the
  `kitty.exe -launcher` tray menu opened the terminal window *behind* the launcher,
  so you had to click it before typing. The launcher now grants the spawned process
  foreground rights (`AllowSetForegroundWindow`), so the new window comes to the
  front with the keyboard focus.
- **Launcher — icon + tooltip.** The tray/Start-menu launcher now uses the **main
  KiTTY application icon** (matching the rest of the suite), and its tray tooltip
  reads **"KiTTY Launcher"**.
- **Launcher — auto-start at login (installer).** Both installers now place a
  **"KiTTY Launcher"** shortcut in your Startup folder, so the tray launcher is
  ready on boot.
- **kageant — session submenu reads the right hive.** kageant's right-click
  "session" submenu listed sessions from the stock PuTTY hive
  (`Software\SimonTatham\PuTTY`) instead of KiTTY's (`Software\9bis.com\KiTTY`),
  matching original KiTTY again. (Agent keys are still not persisted across restarts
  — that's by design in every PuTTY/Pageant; use *Add Key (Encrypted)* + a Startup
  shortcut, e.g. `kageant.exe -encrypted key.ppk`.)

## New in 0.84.1.6

- **Session launcher — saved sessions appear again.** `kitty.exe -launcher` now
  lists your **saved sessions** in the tray menu for quick-launch (previously the
  list was empty). Root cause: the launcher read its sessions from the stock PuTTY
  registry hive (`Software\SimonTatham\PuTTY`) instead of KiTTY's own
  (`Software\9bis.com\KiTTY`) where sessions actually live — a forward-port
  artifact (the 0.84 storage layer moved the registry root to a runtime value, but
  the launcher still used the compile-time PuTTY path). The launcher now reads the
  same hive as session storage. **Verified**: all saved sessions are enumerated and
  shown.

## New in 0.84.1.5

- **Session launcher — discoverable + properly iconned.** `kitty.exe -launcher` (a
  system-tray quick-launch) now has a **Start-menu shortcut** ("KiTTY Launcher", with a
  distinct KiTTY-mascot icon), its previously **blank tray icon** is fixed (the launcher
  icon resources were missing from the build), and its **About** box was expanded.

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
