# KiTTY changelog

KiTTY is the full KiTTY feature set forward-ported onto a modern, security-patched
**PuTTY 0.84** core. Versions below are this port's own `0.84.1.x` line. For current
known limitations see [KNOWN-ISSUES.md](KNOWN-ISSUES.md); for the full feature list
see [FEATURES.md](FEATURES.md).

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
