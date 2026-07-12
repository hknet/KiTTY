# KiTTY changelog

KiTTY is the full KiTTY feature set forward-ported onto a modern, security-patched
**PuTTY 0.84** core. Versions below are this port's own `0.84.1.x` line. For current
known limitations see [KNOWN-ISSUES.md](KNOWN-ISSUES.md); for the full feature list
see [FEATURES.md](FEATURES.md).

## 0.84.1.50-beta — 2026-07-12

- **The send-text input boxes are back.** The classic Ctrl+F8 (single line) and
  Shift+F8 (multiline, pre-filled from the clipboard) pop-up boxes — compose
  text locally, then send it to the terminal in one go with OK or Shift+Return —
  did nothing in this port: their dialog resources were never forward-ported,
  and the old dialog id is nowadays taken by PuTTY's certificate-authority
  panel. The dialogs are restored under fresh ids, with two age-old bugs fixed
  on the way: the multiline box's caption no longer shows the raw window-title
  template (`%%h|%%s|…`), and its layout no longer overlaps on high-DPI
  displays. The masked password variant and the small progress banner some
  registry operations show were revived by the same fix.
- **The `keyexchange` shortcut really repeats the key exchange now.** The
  configurable `[Shortcuts] keyexchange` binding fired a fixed menu position
  that no longer holds "Repeat key exchange" in PuTTY 0.84's dynamically built
  specials menu — so it could do nothing or hit a different special. It now
  looks the rekey command up by its type, and is a clean no-op for non-SSH
  sessions.
- **kageant can ask before any key is used.** A new tray-menu toggle, *Ask
  confirmation before key use* (persisted, default off), pops an allow/deny
  prompt naming the key on every signing request — an agent-side guard against
  a compromised or careless client silently using your loaded keys. The classic
  per-key opt-in (key comment containing `confirmation`) still works on its own.
- **Paste size guard.** `pastesize=<N>` in the kitty.ini `[KiTTY]` section now
  works: pasting a clipboard larger than N characters asks for confirmation
  first, so a mis-aimed paste cannot flood the shell (default 0 = unlimited).
- **`initdelay` is honored again.** The delay before the auto-command /
  auto-password is first sent after connecting follows `initdelay` (seconds,
  default 2.0) instead of a hard-coded 1.5 s — raise it for hosts that are slow
  to present their prompt.
- **`[ConfigBox] filter=no` disables the live session search.** Typing in the
  Saved Sessions box narrows the list as you type by default; the classic
  switch to turn that off is honored again for those who prefer typing a name
  without the list moving underneath.
- FEATURES.md documents the send-text boxes and their `[Shortcuts]` key names
  (`input`, `inputm` — an earlier revision showed a key name that never
  existed), the paste size guard, and the new kageant toggle.

## 0.84.1.49-beta — 2026-07-12

- **Press a key to reconnect a finished session.** When the window outlives its
  session — *Close window on exit* set to *Never*, or a dropped connection — an
  ordinary keypress (Enter, or any other typing key) in the dead terminal now
  restarts the session in the same window, as classic KiTTY did (reported in
  hknet/KiTTY#12). Chords with Ctrl or Alt held, Tab, the arrow keys and F-keys
  are ignored, so Ctrl+D (close the dead window), Ctrl+Tab and the configurable
  keyboard shortcuts keep working; a session that never got past login is not
  re-dialed (no hammering a host that rejected the password), and
  `autoreconnect=no` in kitty.ini turns the whole behaviour off. Classic KiTTY
  also reconnected on a mouse click; that is deliberately not revived, so you
  can still select and copy scrollback text from a finished session.

## 0.84.1.48-beta — 2026-07-11

- **Named proxies are back — with a built-in editor and encrypted passwords.**
  KiTTY's classic *Proxy choice* is restored and modernised: define a set of
  reusable named proxies once, then pick one from a dropdown in the Session panel,
  and the choice is remembered per session. A built-in editor — reachable from an
  *Edit* button beside the dropdown and on the Connection/Proxy panel — creates,
  edits and deletes definitions with the full set of proxy settings (type, host,
  port, credentials, the command for Telnet/Local types, excluded hosts,
  DNS-at-proxy and diagnostics). Each proxy password is now **encrypted at rest**
  exactly like a session password — Windows DPAPI in the registry, or your master
  password in a portable install — and is decrypted only when a session that uses
  the proxy actually connects. Proxies from a classic-KiTTY (9bis) registry hive
  are migrated across automatically and encrypted on the way, and they travel with
  the whole-store export/import. The dropdown appears automatically once any proxy
  is defined; `[ConfigBox] proxyselection = auto|no|yes` forces it on or off.
- **Master-password protection for saved passwords in portable mode.** Which
  protection guards a stored password is now chosen by *where* it is stored: the
  registry always uses Windows DPAPI (bound to your account, no prompt), while a
  portable install can protect its session and proxy passwords with a **master
  password**, so a portable tree is safe to carry on a USB stick or sync between
  machines. Reading classic-KiTTY password formats still works — one-way: KiTTY
  reads the old form but never writes it back — and re-encrypting an old portable
  file to the new form only happens after an explicit confirmation. For automation,
  `-masterpwfile` supplies the master password non-interactively and
  `[KiTTY] PortablePasswordProtection=legacy` keeps the classic plaintext form.
- **Export and import your whole set of sessions.** New *Export all sessions…* and
  *Import sessions…* menu entries (and the `-exportall` / `-importdir` command-line
  flags) move every saved session — each password re-wrapped so it works on the
  destination machine — as a bundle of files, so setting KiTTY up on a new PC is a
  copy-and-import away.
- **Proxy connections show their handshake by default.** Proxy diagnostics now
  default to *"only until session starts"* instead of off, so when a proxied
  connection fails you see the proxy's response in the terminal rather than a bare
  error; it goes quiet once the session is up.
- **Config box: a resizable session list and export/import buttons.** The Session
  panel gained *Export all* / *Import* buttons, and the saved-session list and the
  whole window can be resized via `[ConfigBox] height` and `windowheight`. The
  KiTTY-added exit options are grouped below the stock "Close window on exit", and
  the "show old putty/kitty sessions" checkbox is hidden when there is no old hive.
- **Enter the master password once per running KiTTY (portable).** After you unlock
  the master password, it is shared with every session window KiTTY opens — a session
  from the config box, *New Session*, *Duplicate Session*, or the tray launcher — so
  you are no longer prompted again for each window. The key travels only to KiTTY's
  own child processes through an inherited handle, wrapped in memory with Windows
  CryptProtectMemory (same-logon); the saved files stay master-password-encrypted at
  rest. It is only ever requested when a master password is actually configured.
- **"Update available" popup reimplemented as a standard dialog.** The non-modal
  update popup is now a real dialog, so it uses the system font at the correct DPI (a
  hugely oversized font on high-DPI displays is fixed), sizes itself to its wrapped
  text, and — when an update is available — stays open until you choose *Update now*
  or *Later* instead of auto-dismissing. Only the "you're already up to date" box
  still closes itself.
- **Named-proxy fixes.** A selected named proxy is now reliably applied when the
  session actually connects, and the proxy editor opens with blank fields for a new
  proxy instead of pre-filling the Default Settings proxy.
- **Config box: `defaultsettings = no` now hides "Default Settings".** The flag
  previously only stopped auto-creating the pseudo-session; it now also removes it
  from the saved-session list (it still works as the new-session template, loaded by
  name). The saved-session list also defaults to 16 rows.
- **Registry backup: timestamped copies named `kittynew.sav`.** The registry backup
  is renamed from `kitty.sav` to `kittynew.sav` so it never clashes with an older
  KiTTY's file, and each save now writes a fresh `kittynew-YYYYMMDD-HHMMSS.sav` whose
  filename matches when it was written, keeping the newest `savbackupcount` copies.
- **Whole-store import is more robust and interactive.** Importing a session bundle
  no longer crashes on a partial `.ktx` file. Both *Export all* and *Import all* now
  use a modern folder chooser with an address bar you can paste a path into (instead
  of the tree-only picker), and *Import all* — when the folder contains sessions or
  proxies that already exist — lets you choose once to overwrite them or import only
  the new ones (applied to both sessions and proxies), then reports how many sessions
  and proxies were imported, kept, or failed. Export no longer includes the "Default
  Settings" template or creates an empty proxies folder.
- **Config box: fully expanded Category tree, and a selection fix.** The Category tree
  now opens fully expanded by default (`[ConfigBox] categoryexpand` sets the depth: 1 =
  top categories only, N = N levels), and clicking a saved session with "Default
  Settings" hidden no longer jumps the highlight to the next row.

## 0.84.1.47-beta — 2026-07-09

- **Clean-logout error suppression narrowed (hardening).** The 0.84.1.45 clean-logout
  fix suppressed *any* connection error arriving after the session's exit status was
  known — for the entire remaining life of the connection. That was broader than
  intended: with a port forwarding, X11/agent channel or connection-sharing
  downstream still active, a genuine network failure (or even evidence of tampering
  with the connection) would have been reported as a quiet clean exit. The
  suppression now applies only when nothing else is using the connection — that is,
  when KiTTY was already about to close it of its own accord and the server merely
  got there first. Ordinary logouts (including the Cisco case) behave exactly as in
  0.84.1.45; a connection error while forwardings are live is once again a visible
  error. The same narrowing has been applied to the version of this change
  submitted upstream to PuTTY.
- **Config dialog: "Default Settings" no longer loses its selection.** Loading
  Default Settings cleared the session-name box, and any later refresh of the
  saved-sessions list (typically switching to another settings panel and back)
  yanked the highlight back to the previously loaded session — irritating when
  you were editing the defaults. The name box now keeps showing "Default
  Settings" after Load, and the list keeps the session you actually have loaded
  selected across panel switches. The remembered last session is still used to
  pre-select the list when the dialog opens.

## 0.84.1.46-beta — 2026-07-08

- **Ctrl + ←/→ word navigation restored as the default.** KiTTY's historical
  default for *Shift/Ctrl/Alt with the arrow keys* is the **xterm-style bitmap**
  encoding, which sends a distinguishable modifier sequence (e.g. `ESC [ 1 ; 5 C`
  for Ctrl + →) that shells bind to *backward-word* / *forward-word*. The PuTTY 0.84
  port had silently inherited PuTTY's *"Ctrl toggles application mode"* default
  instead, which encodes no modifier — so on a freshly-created or hive-migrated
  session, **Ctrl + ←/→ stopped performing word navigation**, and the *Word
  navigation (Alt/Ctrl/Both)* option (which only acts in bitmap mode) was itself
  inert. The default is once again the xterm-style bitmap. Sessions that had already
  persisted the wrong value through a save are corrected automatically on first run
  — but only where KiTTY can prove the value was the erroneous default and not a
  deliberate choice (it cross-checks the untouched legacy hive as reference); any
  setting you chose yourself is left untouched. Reported by **m-hume**, with thanks
  for a clear and concise bug report.
- **Quieter, non-modal "Check for updates".** The result no longer opens a modal
  box that grabs focus and plays a sound. When you are already up to date it is
  unobtrusive: a brief title-bar notice in a terminal window (no box at all), or a
  small self-dismissing box from the configuration dialog. When an update *is*
  available the popup appears over the active window and also dismisses itself after
  a few seconds. Only genuine errors stay modal and keep the alert sound, so a real
  failure cannot be missed. The pop-up now renders correctly on high-DPI displays.
- **About boxes are non-modal and DPI / multi-monitor aware.** Across KiTTY, kageant
  and kittygen the About window opens in a sensible place — centred over the window
  it was launched from, or beside the notification area for the tray tools — no
  longer blocks the window behind it, and remembers where you last moved it (per
  monitor layout, and only when that spot is still on a connected screen). Portable
  mode places these windows correctly but stores nothing in the registry.

## 0.84.1.45-beta — 2026-07-07

- **Spurious auto-reconnect on clean logout fixed (Cisco and similar).** When a
  device sent its command exit status and closed the channel, then tore down the
  network connection in the same burst — before KiTTY had finished its own half of
  the close handshake — the session was misreported as an *unexpected* drop and
  auto-reconnect re-dialled it, so an ordinary `exit`/logout could bounce straight
  back into a new session. KiTTY now treats a socket close, or a late
  channel-referencing message, that arrives **after** the session's exit status is
  already known as the clean end it is (consistent with RFC 4254 §6.10, where the
  exit-status message is unacknowledged and may be ignored). Genuine mid-session
  network drops still auto-reconnect as before. Observed with Cisco IOS SSH; the
  root-cause fix is in the SSH layer and has also been submitted upstream to PuTTY.
- **Hyperlink-underline flicker on live output fixed.** With the URL-hyperlink
  underline enabled, a continuously-updating screen (e.g. a switch's `show` output,
  full of IP/hostname strings the URL detector matches) flickered, because every
  frame forced a full-window repaint just to refresh the underlines. KiTTY now
  repaints only the rows whose underline state actually changed, so busy sessions
  no longer flicker while underlined hyperlinks keep working and updating.
- **Non-blocking connection errors.** A dropped or remotely-closed connection,
  and non-fatal server errors, are now shown **inline in the terminal** instead of
  a modal pop-up that trapped the window until dismissed — the window stays usable
  and closable, and the message stays in the scrollback. A **fatal** disconnect
  also badges the window title with a `⚠ … (disconnected)` marker that clears on
  the next connect. Host-key / weak-crypto confirmation prompts are unchanged.
  Prefer the classic modal error boxes? Set `modalerrors=yes` under `[KiTTY]` in
  `kitty.ini`. (upstream cyd01/KiTTY #548)
- **"Run clipboard as a command" safeguards.** The Ctrl+F5 shortcut that runs the
  clipboard contents as a local command now, by default, shows a confirmation
  prompt (with the command) and a tray notification after launch. Both are
  per-session and can be toggled in **Window → Selection**.
- **Security hardening.**
  - `/savedump` no longer embeds a recoverable auto-login / proxy password (in the
    bundled `current.ktx`) nor the `autocommand` login string — both are redacted
    like the visible dump.
  - *Check for updates* now enforces whole-chain certificate-revocation checking
    before running a downloaded installer.
  - Hardened the `kitty.ini` reader against a startup buffer overflow from an
    over-long value and a 1-byte over-read on malformed lines, and fixed a benign
    1-byte over-read in the far2l base64 decoder.

## 0.84.1.44-beta — 2026-07-06

- **kageant private-key memory protection.** SSH-2 private keys now settle into a
  Windows `CryptProtectMemory`-protected private blob instead of remaining as
  long-lived decrypted `ssh_key` objects. kageant temporarily unprotects and
  deserializes the key for signing, then frees the temporary key and re-protects
  the blob. The add-key receive path also wipes clear private-key request bytes
  as they are consumed.
- **kageant encrypted/deferred flow preserved.** `Add key (encrypted)`, startup
  keys, `-encrypted`/`-nodecrypt`, key ordering, Windows/OpenSSH agent clients,
  and explicit **Re-encrypt** continue to work; re-encrypt returns eligible keys
  to encrypted-PPK-only state so the next use prompts again.

- **PuTTY upstream fix:** cherry-picked PuTTY `ac7919db`, fixing a small memory
  leak when PuTTY inserts CBC-mode `SSH_MSG_IGNORE` packets to randomise the IV.
- **PuTTY upstream cleanup:** cherry-picked PuTTY `aaa5fc51`, removing a
  misleading timer-scheduling guard that no longer worked as intended with the
  unsigned tick type.
- **RuTTY script timeout hardening:** negative imported/configured
  `ScriptTimeout` values are clamped to the default instead of wrapping into a
  huge timer interval; overly large values are capped before tick conversion.

## 0.84.1.43-beta — 2026-07-06

- **Launcher update visibility improved.** When `kitty.exe -launcher` detects a
  newer KiTTY release, the update is no longer only a transient Windows tray
  balloon: the launcher tray tooltip also mentions the available version, and
  the launcher menu shows a disabled `Update available: KiTTY ...` line until
  the user upgrades.
- **Saved-session search/filter polish.** Typing in the Saved Sessions field now
  narrows the visible saved-session list within the active folder filter, ranking
  prefix/token matches before substring matches and showing folder names in
  brackets while searching. Focus starts in that field; Up/Down moves into the
  filtered list; Enter loads or starts the highlighted visible session instead of
  a hidden previous selection. The root-folder delete message is clearer and
  auto-dismisses.
- **Session comment display fixed.** Empty comments in the primary KiTTY hive no
  longer get overwritten in the config dialog by stale comments from older
  fallback registry hives with the same session name.
- **Normal logout no longer auto-reconnects.** A clean remote logout/`exit` that
  has already closed the session is no longer treated as a reconnect-worthy
  connection failure.

## 0.84.1.42-beta — 2026-07-05

- **Savedump crash fixed.** `kitty.exe -savedump` now has a valid configuration
  context during command-line processing, so it produces encrypted `kitty.dmp`
  output in both registry and portable directory modes instead of crashing before
  any terminal window exists.
- **Updater hardening.** The in-app MSI updater already verified KAPPER-signed
  installers and held a read-only lock across launch; it now also downloads to a
  unique temporary `.msi` path with `CREATE_NEW` and deletes the verified download
  if launching `msiexec` fails or is cancelled.
- **Portable mode is more registry-free.** In `kitty_portable.exe` / `savemode=dir`,
  SSH host keys, SSH host CAs, the random seed, recent-session state, last-session
  state, and the update-check cache now live in the portable config directory
  instead of normal HKCU registry/profile locations.
- **Session folder UI fixed.** The config dialog now uses an editable folder
  selector with a New folder action, labels the root/all-sessions view clearly,
  maps filtered session selections correctly, saves sessions into the currently
  selected folder, refreshes folder filters reliably, and gives the session list
  more vertical room with aligned action buttons.

## 0.84.1.41-beta — 2026-07-04

- **Launcher global hotkeys.** Individual saved sessions can now have a global
  launcher hotkey under **Window → Behaviour**. The hotkey is registered only while
  `kitty.exe -launcher` is running; saving a session notifies a running launcher to
  refresh its session list and re-register hotkeys automatically. The config dialog
  can probe whether the combination is currently available or already reserved by
  Windows/another app.
- **Launcher update balloon is no longer one launch behind.** The launcher now
  re-checks after the async GitHub update query finishes, so a newly available
  update can show a tray balloon on the first launcher run.
- **mNotepad high-DPI polish.** The built-in editor now scales its default font for
  the current display DPI, and the feature docs explain how Send/F12/Ctrl+Enter
  sends text to the parent KiTTY session.
- **Hyperlink polish.** URL underline repainting is improved for freshly typed
  links, and the hand cursor on hyperlink hover is now a separate opt-in setting
  under **Window → Hyperlinks**.
- **Test-build labels.** Ad-hoc builds can define `KITTY_TEST_BUILD_LABEL` so About
  boxes visibly distinguish test/debug EXEs from normal beta releases without
  source edits.
- **`kitty.ini.example`.** Releases now include a commented, inert sample
  configuration file plus a drift-check helper so source-level `kitty.ini` option
  changes are less likely to go undocumented.
- **Portable backup.** In portable directory mode, applying settings refreshes
  `Backups\kitty-portable-latest` and keeps timestamped backups with `kitty.ini`
  and the portable config folders (`Sessions`, `SshHostKeys`, commands/folders/
  proxies) for simple manual restore. `[KiTTY] portablebackupcount=` controls
  retention; default 5, 0 disables.
- **Paste menu restored.** The terminal system menu and right-click context menu
  again include **Paste**, so users using Windows mouse-button mode can paste from
  the menu instead of relying on right-click paste.
- **Branding polish.** kageant error dialogs now use kageant naming instead of
  upstream Pageant titles, KiTTYgen About includes the kapper.net port branding,
  and terminal Tools menus expose mNotepad directly. mNotepad shortcuts are also
  fixed: Shift+F2 opens the editor and Ctrl+Shift+F2 opens it with clipboard
  contents.

## 0.84.1.40-beta — 2026-07-03

- **Window title placeholders.** The **Window Title** setting now expands dynamic
  placeholders so the title can reflect the active connection: `%%h` (hostname),
  `%%s` (saved session name), `%%u` (username), `%%p` (port), `%%P` (protocol),
  `%%f` (folder), `%%l` (local forwarded ports), and `%%d` (dynamic/SOCKS
  forwarded ports). See [`docs/window-title-placeholders.md`](docs/window-title-placeholders.md).
  Contributed by m-hume.
- **WinSCP launch detection improved.** KiTTY now finds 32-bit WinSCP installs under
  `Program Files (x86)`, per-user installs under `%LOCALAPPDATA%\Programs`, and
  `WinSCP.exe` on `PATH`; stale configured paths are re-probed. The config dialog
  now uses a file picker for the WinSCP executable instead of a plain text field.
- **Launcher tray Refresh is easier to use.** Refresh rebuilds and reopens the tray
  menu at the original menu position, so you can keep navigating from the same spot
  after reloading sessions.
- **Cleanup / migration hardening.** RuTTY scripting, hyperlink settings, savedump,
  and KTX import/export no longer depend on stale historical `MOD_*` guards. The
  hyperlink backend source is normalized to `kitty/url/urlhack.c`, and dead old
  `MOD_STARTBUTTON` / `MOD_TUTTY` fragments were retired.

## 0.84.1.39-beta — 2026-06-30

- **Data-integrity fix: legacy (old-KiTTY) passwords now decrypt correctly.**
  0.84.1.38's "auto-decrypt old 9bis-hive passwords on load" applied an extra
  MASKPASS step that **corrupted** them — a migrated session got a wrong password,
  and re-saving it persisted the mangled value. The decrypt now matches the real
  cyd01 format (`bcrypt(plaintext)`, verified against genuine cyd01 0.76 registry
  *and* portable session files); the MASKPASS layer is applied only when a session
  actually used it. **Update before opening old-KiTTY sessions** so their passwords
  migrate intact.
- **Saved-session list selection fixed** (PR #3, thanks @m-hume). Type-ahead in
  the saved-session list no longer jumps the highlight to "Default Settings" when
  you type a name that sorts before it, and **Save** now keeps the just-saved
  session selected even when a folder filter is active.
- **Portable mode now reads existing cyd01-KiTTY session files.** A portable
  (`savemode=dir`) session saved by an older cyd01 KiTTY used a `key\value\` line
  format that 0.84.1.38 couldn't parse, so sessions loaded with an **empty host**
  (the list showed names, but loading gave a blank IP). The reader now accepts
  both that legacy format and our `key=value` format; on the next Save the session
  is rewritten in our format. (Reported on issue #1 by Kyogre.)

## 0.84.1.38-beta — 2026-06-28
- **Stored session passwords are now encrypted at rest with Windows DPAPI**
  (`CryptProtectData`, tied to the Windows account; stored as a `DPAPI1:` blob).
  Existing plaintext/legacy values still load (old ≤0.76 KiTTY passwords are
  auto-decrypted) and re-store encrypted on the next save. DPAPI is machine-bound
  (defeats offline/cross-user theft, not same-user malware; does not move PCs).
- **Auto-login password reliable on every launch path** (normal / Duplicate /
  open-new-with-current); an internal masking step could garble it on the
  serialise-launch paths even for ASCII. Passwords are stored/sent as UTF-8 now,
  matching the SSH prompt, so non-ASCII passwords work without re-entry.
- **Portable mode now stores sessions as files** under `Sessions\` next to the
  exe (`kitty_portable.exe` / `savemode=dir`) instead of falling back to the
  registry; passwords in those files are DPAPI-encrypted too.
- **"Show password" checkbox** in Connection → Data to reveal the stored
  auto-login password.
- **Window size is remembered** alongside position (same monitor layout restores
  the previous terminal dimensions).
- **Fixed a crash** opening Terminal → Keyboard (duplicate Alt-shortcut assertion).
- **Launcher tray menu:** Refresh no longer re-opens the menu at the cursor; it
  refreshes the list and dismisses.
- **Portable polish:** the read-only Comment box no longer shows a stale registry
  comment, and the registry-only "show / edit / delete old sessions" control is
  hidden in file mode.

## 0.84.1.37-beta — 2026-06-27
- **Saved session passwords now work for auto-login and WinSCP.** When a session
  was launched, KiTTY passed the stored password from the launcher to the terminal
  (and to WinSCP) in an internal **obfuscated (masked)** form, but the connecting
  process sent it **verbatim** — so SSH auto-login and WinSCP launches failed for
  *every* saved password (regardless of its characters; it just looked like a
  "wrong password"). The hand-off now passes the password correctly, so a stored
  password authenticates as typed. (This also makes the auto-login try the stored
  password **once** and then fall back to an interactive prompt, rather than
  retrying it — see below.)
- **Event Log: "Copy" with nothing selected now copies the whole log** instead of
  just beeping (selecting specific lines still copies only those).
- **No more credential-hammering that gets your IP banned.** Two related fixes:
  - **Auto-login sends a stored password only once per connection.** Previously KiTTY
    re-answered the server's password prompt from the saved password on *every*
    re-prompt, so a wrong/rejected password was resent until the server's
    `MaxAuthTries` tripped ("Too many authentication failures") — exactly what gets an
    IP banned (fail2ban etc.). Now the stored password is offered once; if rejected,
    KiTTY falls through to the interactive prompt instead of resending. (This also
    stops the password being mis-sent into a second prompt such as a 2FA/OTP round.)
  - **Auto-reconnect no longer retries on an authentication failure.** Reconnect is now
    gated **per session** (only a session that actually authenticated is eligible) and
    **never** fires when the disconnect reason is an authentication failure.
    Network-drop reconnect of an established session is unchanged.
- **Security fix (remote): far2l clipboard parser out-of-bounds read.** A malicious
  SSH server could send a short, crafted `far2l` clipboard APC sequence that made the
  parser read a 4-byte length **before** its decode buffer (a heap under-read), and the
  *register-format* / *is-available* sub-commands ran with **no user consent**. The
  parser now requires the full header before reading, and register-format / is-available
  now also require the same per-session clipboard consent as get/set. Impact is a
  crash/denial-of-service (no code execution, no data disclosure), but it is reachable
  from the network, so **recommended for anyone connecting to untrusted hosts.**
  *Behaviour change:* far2l clipboard register/availability now wait for the one-time
  clipboard-consent prompt — if you use a far2l server, answer **OK** (Window → Selection)
  once per session.
- **Security hardening pass over the whole codebase + the PuTTY base.** A full security
  sweep audited every KiTTY-added source file, the upstream files KiTTY modifies, the
  remote SSH/terminal parsers, and the diagnostic dumps, then adversarially re-verified
  each finding. Besides the far2l fix above, this release closes a batch of confirmed
  **local-input** overflows — every place a session/`.ini`/registry value, an
  autocommand line, a port-knock sequence, a proxy list, a rutty script, or an exported
  password was copied into a fixed-size buffer is now length-bounded, and config-line
  parsers are guarded against malformed/blank lines. Defence-in-depth for crafted or
  shared config files.
- **Removed dead remote-command dispatcher.** The old `__xy` escape-metacommand handler
  (`ManageLocalCmd`: `__cm` run-command, `__pl` plink, etc.) was unreachable in the 0.84
  base but carried a latent remote-code-execution surface if ever re-wired; it has been
  deleted outright.
- **Config dialog fixes.** The configuration window now **remembers its position**
  (and re-centres if it was on a monitor that has since been removed); **Save** keeps the
  saved session selected; you can **delete sessions imported from older PuTTY/KiTTY
  hives** (a new *"show / edit / delete old sessions"* checkbox under the session list
  governs whether those foreign sessions are shown — and, when shown, are tagged
  `(old KiTTY)` / `(PuTTY)` so you always know which hive you're acting on); and the
  **last-loaded session is remembered**, auto-selected and auto-loaded when the dialog
  re-opens. When your own hive has no sessions of its own, the old-sessions view turns
  on automatically so your existing sessions still appear.
- **WinSCP launch: credentials are now URL-encoded.** A `@`, `/` or `:` in the
  username/password is percent-encoded in the WinSCP connection URL; previously such
  a character could break the URL and, worst case, point the transfer at the wrong
  host. (Addresses upstream cyd01/KiTTY #535.)
- **`.ini` settings load: out-of-bounds read/write fixed.** A blank or CR/LF-only
  line in a hand-edited `.ini` could trigger a wild memory access while the line was
  trimmed. (Addresses upstream cyd01/KiTTY #541.)
- **Diagnostic dumps no longer leak secrets.** `/savedump` is a support diagnostic that
  writes a plaintext dump; it previously included the session password, proxy password,
  **SSH key passphrase**, the **private key file** itself, the password-store protection
  password, the current **clipboard** contents, and login/rutty **script content**
  (both encrypted and decrypted). All of these are now redacted, so a dump is safe to
  share for support.
- **`__ti` title handler hardened** against an overflow on a near-maximum-length
  window title. (Addresses upstream cyd01/KiTTY #405.)
- **kageant: About box now shows the KiTTY/kapper.net copyright** instead of only
  the upstream PuTTY notice.
- **Verified (no change needed):** upstream **#531** (CVE-2024-31497, P-521 ECDSA
  nonce) and **#520** (Terrapin, CVE-2023-48795) are already fixed by the PuTTY 0.84
  base (RFC 6979 deterministic nonces; strict key-exchange).
- *Note:* from this release KiTTY is a **public beta** — older beta builds are kept
  available on GitHub. *Known limitation:* an optionally-saved session password is
  still stored reversibly (DPAPI protection planned) — prefer key auth (kageant).

## 0.84.1.36-beta — 2026-06-25
- **Security fix (CVE-2024-25003 / CVE-2024-25004): stack buffer overflow via a
  malicious server.** The `__dt` (duplicate-session) and `__wt` (WinSCP)
  metacommands — triggered by an ANSI escape sequence carrying a `host:user:path`
  payload — copied that remote-controlled data into fixed-size stack buffers
  without bounds checking, so a hostile or compromised SSH host could crash KiTTY
  or potentially execute code. The payload is now parsed with length-capped
  copies. **Recommended update for anyone connecting to untrusted hosts.**
  (Addresses upstream cyd01/KiTTY #525.)
- **Launcher: Ctrl+Shift+letter session shortcuts now work.** They were shown in
  the tray menu but only produced a beep (a Win32 popup menu displays accelerator
  text but never acts on it). While the launcher menu is open, the shortcut now
  launches the matching session/command. (Addresses upstream cyd01/KiTTY #544.)
- **Verified (no change needed):** upstream cyd01/KiTTY **#526** (command
  injection via the file-get escape command) does not affect this port — the
  pscp/scp builders run without a shell and with bounded, quoted arguments; and
  **#523** (UTF-8 window titles) already renders correctly here.

## 0.84.1.35-beta — 2026-06-25
- **Embed KiTTY in connection managers (mRemoteNG, Remote4Support).** KiTTY's
  terminal now hosts correctly inside their connection tabs. Embedding is
  auto-detected (the host reparents our window), the font DPI is corrected for the
  host's monitor (no more oversized startup font), the terminal fills the pane and
  **reflows** on resize (a font-size change reflows rows/cols instead of resizing
  the pane). Also adds an explicit `-hwndparent <handle>` switch for hosts that
  pass it. Addresses upstream
  [cyd01/KiTTY #554](https://github.com/cyd01/KiTTY/issues/554).
  *Known limitation:* a minor window wobble can occur while dragging the pane's
  height in mRemoteNG (host-side caption-offset behaviour); cosmetic.
- **Verified (no change needed): upstream cyd01/KiTTY #549 does not affect this
  port.** A `savemode=dir` `configdir` path containing **spaces** loads saved
  sessions correctly here (the ini parser preserves spaces in the value; paths are
  built/opened space-safely). Live-tested.

## 0.84.1.34-beta — 2026-06-24
- **Fix: window position now saved when a session ends by Ctrl+D / remote logout.**
  The position memory (0.84.1.32/.33) only saved on WM_DESTROY (the X button /
  close prompt). A session closed by the remote side exits via PostQuitMessage,
  which never sends WM_DESTROY, so a window closed with Ctrl+D wasn't remembered —
  it reopened at the default spot. Now saved on the remote-exit and fatal-error
  close paths too.
- **Launcher: "Refresh" reopens the menu.** Clicking Refresh reloaded the session
  list but dismissed the popup, forcing a second trip to the tray icon. The menu
  now reopens after a refresh so a just-reloaded session can be picked immediately.
- **Verified (no change needed): upstream cyd01/KiTTY #545 and #546 do not affect
  this port.** #545 (reconnect fails with a password > 126 chars) was an upstream
  keyboard-injection / fixed-buffer bug; this port re-authenticates on reconnect
  through the standard SSH userpass path with the full password, so the limit
  doesn't exist. #546 (klink always exits 0) does not reproduce: klink returns a
  non-zero exit code on authentication failure in batch mode (and the remote
  command's real exit code on success).

## 0.84.1.33-beta — 2026-06-24
- **Fix: multi-second delay before every new window.** Each new KiTTY process
  paused for seconds (≈10s on some machines) before its window appeared — a fresh
  config window, a connecting session, or Duplicate Session — because the Windows
  taskbar Jump List was rebuilt **synchronously** on the startup path (via the
  per-launch `do_defaults` load). The Jump List COM rebuild now runs on a
  background thread, and the "Default Settings" load no longer touches it, so
  windows open immediately. Recent-sessions Jump List still works.
- **Fix: window position not restored on multi-monitor / mixed-DPI setups.** The
  per-monitor position memory (0.84.1.32) used `WINDOWPLACEMENT`, whose
  coordinates are not reinterpreted for a target monitor's DPI under
  Per-Monitor-V2, so on mixed-DPI multi-monitor layouts the window opened at a
  default position. It now saves/restores physical screen coordinates
  (`GetWindowRect`/`SetWindowPos`), uses an order-independent monitor-layout key,
  and applies the restore after the startup sizing pass.
- **`-noconfirm` command-line flag.** Closes the terminal window without the
  "Are you sure?" prompt (forces CONF_warn_on_close off for that launch); for
  scripts/automation/testing. Does not affect SSH host-key / weak-crypto
  security confirmations. (Addresses part of upstream cyd01/KiTTY #548.)

## 0.84.1.32-beta — 2026-06-24
- **Remember window position (per monitor layout).** New windows and Duplicate
  Session reopen at the last-closed window position, remembered per monitor-setup
  signature and restored via SetWindowPlacement (clamps off-screen back onto a
  visible monitor). Position only; size stays per-session. On by default (Session
  panel toggle). An explicit per-session X/Y still wins.

## 0.84.1.31-beta — 2026-06-24
- **Launcher update balloon.** The tray launcher shows a balloon on startup when a
  newer build is known (backstop to the terminal-start notice).
- **kageant "key used" balloon.** kageant pops a short tray balloon naming the key
  when it signs an authentication request; tray-menu toggle "Notify when a key is
  used" (default on, persisted).

## 0.84.1.30-beta — 2026-06-24
- **Fix: beta-channel self-detection in the updater.** The binary's version string
  has no `-beta` marker, so every build was treated as stable — causing a spurious
  "you are installing a beta" warning in *Check for updates* and suppressing the
  startup update notice for beta users. The channel is now derived from the version
  scheme (stable `x.y.M.0` vs beta `x.y.M.P`). Beta users get the notice and no
  bogus warning.

## 0.84.1.29-beta — 2026-06-24
- **About boxes render proper Unicode.** Launcher + main Help->About now use the
  wide Windows APIs (MessageBoxW / SetDlgItemTextW / term_data_wide) so ©, em-dash
  and the update-notice arrow display correctly on any system codepage, replacing
  the earlier ASCII/CP1252 mojibake work-arounds.
- **Main About box credits the port author** — KAPPER NETWORK-COMMUNICATIONS GmbH
  for the PuTTY 0.84 port, next to the KiTTY (Cyril Dupont) and PuTTY (Simon
  Tatham) attributions.

## 0.84.1.28-beta — 2026-06-24
- **"Update available" notice at session start (opt-in, on by default).** A worker
  thread refreshes a cached latest-version; at the clean top of a session KiTTY
  prints a one-line notice if a newer version is available (channel rule applies —
  stable builds ignore betas). Display is synchronous at session top, so a
  full-screen TUI is never corrupted; the trade-off is the notice can be one launch
  behind for a brand-new release. Toggle: **Session → "Check for updates on
  startup."**
- **Updater channel detection** now uses GitHub's `prerelease` flag instead of the
  tag text.

## 0.84.1.27-beta — 2026-06-24
- **In-app updater respects your release channel.** A stable build no longer
  silently installs a beta via *Check for updates*: if the newest available build
  is a beta, KiTTY warns and asks first (proceed with caution). Beta builds track
  the newest beta as before.
- **Docs:** FEATURES.md now documents the in-app updater and kageant key reorder
  (and lists the kageant sections in its contents).

## 0.84.1.26-beta — 2026-06-23
- **Faster failover on a dead address (capped connect timeout).** A pending
  connect that gets no response now fails over to the next candidate address after
  ~5 s instead of hanging ~21 s on Windows' SYN timeout — removing the long freeze
  on auto-reconnect / first connect to a multi-address host. Only ever triggers on
  a silently-dropped connection; normal connects are unaffected. Groundwork toward
  a fuller Happy-Eyeballs parallel connect (planned).

## 0.84.1.25-beta — 2026-06-23
- **Fix: config dialog crash (regression in 0.84.1.24).** The new Word-navigation
  radio-button control was built with a malformed argument list (`NO_SHORTCUT`
  without per-button shortcuts), corrupting the dialog varargs and crashing KiTTY
  whenever the configuration box was built (Change Settings / opening Settings).
  Fixed; the 0.84.1.24 font-zoom and word-navigation options are now usable.

## 0.84.1.24-beta — 2026-06-23
- **Ctrl + mouse wheel zooms the terminal font.** Hold Ctrl and scroll the wheel
  up/down to grow/shrink the font on the fly (clamped).
- **Configurable word-navigation modifier.** New Terminal → Keyboard option
  "Word navigation (Left/Right arrows)": **Alt** (default), **Ctrl**, or **Both** —
  picks which modifier sends the xterm word-nav sequence (`ESC[1;3 D/C`) that shells
  bind to back/forward-word. Bitmap arrow mode only.
- **Security hardening (cont.): WinSCP command builder fully bounded.** The remaining
  FTP/options/proxy append paths now use bounded appends; no change for SSH launches.

## 0.84.1.23-beta — 2026-06-23
- **kageant: the passphrase prompt opens over the requesting terminal.** When a
  terminal asks kageant to use an encrypted key, the "enter passphrase" dialog now
  appears centred over that terminal window (the active window at request time)
  instead of the middle of the screen.
- **Security hardening (cont.): WinSCP launcher command bounded.** The WinSCP
  launch command builder now appends with length bounds (no fixed-buffer
  overflow); its scp:// URL format is unchanged, so launches behave identically.

## 0.84.1.22-beta — 2026-06-22
- **MSI upgrade now relaunches the tray apps it closes.** During an in-place
  upgrade, Windows' Restart Manager closes apps that lock the files being
  replaced; it only restarts ones that asked to be restarted. KiTTY now asks:
  **kageant** and the **tray launcher** are relaunched after the upgrade, and a
  **terminal opened from a saved session** (`-load NAME` / `@NAME`) is relaunched
  with the same session so it reconnects. (Ad-hoc/host-typed terminals are left
  closed on purpose — a blank reopen would be noise and the live session can't be
  restored regardless.)

## 0.84.1.21-beta — 2026-06-22
- **Security hardening — transfer command builders quote + bound their inputs.**
  The pscp/plink command builders (upload, download, plink, clipboard-get) now
  wrap each session-derived value (password, key path, source/target paths,
  remote command) as a single, properly **argv-quoted** argument and append with
  **length bounds**. An unusual character (e.g. a quote) in a session field can no
  longer inject an extra command-line switch, and over-long fields truncate
  instead of overflowing a fixed buffer. (Raw user "extra options" fields stay
  unquoted by design.) Completes the command-builder hardening begun in 0.84.1.19.

## 0.84.1.20-beta — 2026-06-22
- **Fixed file upload (pscp), which was broken across the 0.84 series.** Dropping a
  file on the terminal — or the Send-file menu — now works again. Three distinct
  faults were fixed: a crash (assertion) because `username`/`remote command`
  became "ambiguous" string types in PuTTY 0.84 and KiTTY used the old accessor;
  a stray byte appended to a dropped file's path (so pscp couldn't find it); and
  the terminal window no longer registering for dropped files (the "forbidden"
  cursor). Combined with the 0.84.1.19 no-shell change, uploads run safely again.
- Note: a follow-up will add argument quoting + length bounding to the transfer
  command builders (so an unusual character in a session field can't inject an
  extra command-line switch).

## 0.84.1.19-beta — 2026-06-21
- **Security hardening (cont.):** external file-transfer (pscp) and plink commands
  are now launched directly via CreateProcess instead of through the Windows
  command shell (`system()`), so characters in session fields (password, host,
  username, remote command, etc.) can no longer be interpreted as shell commands.
  Each transfer/command opens in its own console window. (A follow-up will add
  buffer-length bounding and argument quoting.)

## 0.84.1.18-beta — 2026-06-21
- **Security hardening (from an internal review):**
  - In-app updater: the downloaded installer is now locked against modification
    (deny-write) from signature verification through launch, closing a
    time-of-check/time-of-use window, and the installer is only fetched over HTTPS.
  - Fixed a buffer-size mismatch when reading a stored password from the registry,
    and made the registry string reader NUL-terminate and bounds-check its results.

## 0.84.1.17-beta — 2026-06-21
- **In-app updater — fix install-type detection.** A system (or per-user) install
  could be misreported as "portable" (so auto-install was refused). Detection now
  asks Windows Installer whether KiTTY is installed, by its stable product
  UpgradeCode, instead of guessing from the executable's path — robust to
  non-default install locations and localized systems. Path-sniffing remains only
  as a fallback.

## 0.84.1.16-beta — 2026-06-21
- **kageant — reorder loaded keys:** the key-list window has **Move Up** / **Move
  Down** buttons. The list order is the order keys are *offered* to servers, so
  you can put your most-used key first. The order is saved (by key fingerprint)
  and restored on the next start, including when "Load keys on startup" is on.
- **kageant — passphrase prompt focus:** when a terminal asks kageant to use an
  encrypted key, the passphrase prompt now comes to the foreground with focus
  instead of opening behind the terminal window.

## 0.84.1.15-beta — 2026-06-21
- **In-app updater:** *Check for updates* now downloads and installs the correct
  asset for the detected install type — per-user MSI, system MSI (elevated), or
  portable ZIP (download-only). The installer runs only after an Authenticode
  gate confirms it is a genuine KAPPER-signed artifact (valid chain **and**
  matching publisher CN); a file that fails verification is deleted and never run.
- **Duplicate Session / New Session:** the new window now takes the foreground and
  focus instead of opening behind the current window.
- **Launcher About box:** no longer plays the Windows "asterisk" sound when opened.
- **Transparency:** off by default (sessions start opaque) and still configurable
  per session via *Window → Transparency*; `kitty.ini` `transparency=no` is now a
  complete master switch (hides the config panel **and** the system-menu adjust
  items).
- **Saved-session comment box:** the read-only comment display no longer blanks
  after pressing Load.

## 0.84.1.14-beta — 2026-06-20
- **Saved-session list:** single-clicking a session now copies its name into the
  "Saved Sessions" box, so Save/Load act on it without retyping — e.g. select
  **Default Settings** and Save to update it directly.
- **Windows file-info rebranding:** kageant's file description no longer reads
  "PuTTY SSH authentication agent" (it showed up that way in Task Manager's Startup
  list); `kitty_pterm` no longer says "PuTTY-style"; and the renamed CLI tools now
  carry their shipped names (klink/kscp/ksftp/kageant/kitty_pterm) instead of the
  old PuTTY tool names. (The version string still notes the "PuTTY 0.84 base"
  lineage and the copyright still credits Simon Tatham — deliberate attribution.)

## 0.84.1.13-beta — 2026-06-20
- **kageant — "Load keys on startup"** (opt-in, off by default): kageant remembers
  the file paths of the keys you load and re-adds them at the next login, added
  **encrypted/deferred** (passphrase only on first use). Enabling it also installs
  an autostart entry, replacing a manual kageant Startup shortcut. Only key-file
  paths are stored, never secrets.
- **Hide a session from the launcher:** new per-session option that keeps a session
  out of the `kitty -launcher` menu while leaving it in the normal session list.

## 0.84.1.12-beta — 2026-06-20
- **Fixed the blank taskbar icon:** the terminal window declared the AppUserModelID
  `SimonTatham.PuTTY`, which didn't match the installer's pinned shortcuts; it now
  declares `kappernet.KiTTY`, so the taskbar button shows the proper KiTTY icon.

## 0.84.1.11-beta — 2026-06-20
- **Reconnect restores the window icon** (it used to stay on the broken-connection
  icon after a successful reconnect).
- **Session comments show for pre-existing sessions:** the read-only comment box now
  reads "Comment" across all registry hives, so comments authored by an older KiTTY
  appear without needing to re-save the session.
- **Launcher About-box** character artifacts (mojibake) fixed.
- **TCP keepalives default to on** for newly-created sessions.

## 0.84.1.10-beta — 2026-06-19
- **kageant — optional Windows OpenSSH agent integration** (off by default): lets
  kageant serve as the agent for the Windows `ssh.exe` (writes `~/.ssh/kageant.conf`
  + a managed `Include` block in `~/.ssh/config`, byte-safe with a one-time backup).

## 0.84.1.9-beta — 2026-06-18
- **Config dialog:** a "WinSCP executable path" field; the session **Comment** field
  is now multiline; and a read-only **"Comment of selected session"** box in the
  Session panel that follows the saved-session selection.

## 0.84.1.8-beta — 2026-06-18
- **Registry namespace consolidated** to `Software\kapper.net\KiTTY` with automatic,
  non-destructive migration from the old hive; kageant passphrase/About dialogs
  rebranded.

## 0.84.1.1 – 0.84.1.7-beta — 2026-06-17/18
- **kittygen-cli**: a console (command-line) SSH key generator.
- Launcher fixes (saved-session list reads KiTTY's own hive; new sessions take
  focus).
- Suite-wide KiTTY rebranding (icons, window/About text, file metadata); the GUI key
  generator defaults to EdDSA/Ed25519.

## 0.84.1.0 — 2026-06-16 — first stable of the 0.84 port
- **Post-quantum key-exchange warning** (OpenSSH-style; on by default): warns at
  connection time when the SSH key exchange is not post-quantum-secure.
- The complete KiTTY feature set forward-ported onto PuTTY 0.84 (≈1,200 upstream
  commits newer than KiTTY's original 0.76b base).
