# KiTTY changelog

KiTTY is basically the full KiTTY feature set forward-ported onto a modern, security-patched
and enhanced **PuTTY 0.85** core. Versions below are this port's own `0.85.1.x` line. For current
known limitations see [KNOWN-ISSUES.md](KNOWN-ISSUES.md); for the full feature list
see [FEATURES.md](FEATURES.md).

## 0.85.1.3-beta — unreleased

### New

- **kageant can find a startup key again when its media comes back on a
  different drive letter.** Windows hands a returning USB stick whichever
  letter is free, and a key recorded as `E:\...` then read as "still absent"
  forever. The retry setting is now three-valued — never / from the stored
  drive and path / from the stored path on any drive (`[Agent]
  retrykeys=ignoredriveletter`, off unless chosen). The new mode tries each
  waiting key's stored path on exactly the letter that just arrived (drives
  are never scanned), loads a key only if its recorded fingerprint matches,
  and only then rewrites the stored path so the next start needs no retry. A
  file with no recorded fingerprint to check against, or with the wrong one,
  is refused and reported.

## 0.85.1.2-beta — 2026-08-21

### New

- **KiTTYgen: an "Add confirmation" button beside the key comment.** kageant
  asks before each use of a key whose comment carries the word
  "confirmation" — a feature that was invisible unless you knew the magic
  word. The button appends it (and does nothing if a confirm marker is
  already there), so a key can be made confirm-on-use the moment it is
  created. Also fixed there: after generating a key, no control had the
  keyboard focus — Tab went nowhere and only beeped; the passphrase field
  takes the focus now.

### Fixed

- **kageant never blocks itself with a message box you didn't ask for.** A
  modal dialog stops the agent from answering requests while it is up, so
  every ssh/git/scp call waits on it. The old-key-format warning could
  appear during the automatic key load at login — it is a self-dismissing
  notice there now. And a second kageant started with no arguments exits
  quietly instead of complaining "already running": upgrades and Windows'
  restart-apps machinery legitimately start one beside the autostart, and
  keys passed on the command line were always handed to the running agent
  silently.

- **kageant and KiTTYgen say so when in-memory key protection is not
  working.** SSH-2 private keys are normally held `CryptProtectMemory`-
  encrypted; when that API fails — which on a normal Windows it never does —
  the tools silently fell back to holding keys in plain memory. Now kageant
  shows a one-time warning at startup and carries a permanent "keys
  UNPROTECTED in memory" line in its tray tooltip, KiTTYgen warns once in
  the window, and kittygen-cli prints a warning to stderr. The check is a
  real protect/unprotect round trip, not just "does the DLL load", so a
  hooked or stripped crypt API is caught too.

- **The auto-command runs on every connect too.** 0.85.1.1 made the
  Session → Scripting script and the login script run again on reconnect,
  but *Auto-command after login* (Connection → Data) was the third sender
  with the same fault and was missed: it ran on the first connection and
  never again. It now re-runs on every connection, from its first line, like
  the other two. (hknet/KiTTY#36)

### Faster

- **The configuration window pre-builds its panels in the background.** Panels
  are cached since 0.85.1.1; now the box quietly builds the not-yet-visited
  ones while it sits open, one every 120 ms, so by the time you click a
  category its panel already exists. Every switch — first visit or revisit —
  now costs about 40 ms (from 94 ms two releases ago).

## 0.85.1.1-beta — 2026-08-21

### Faster

- **Switching category in the configuration window no longer rebuilds the
  panel.** Every switch used to destroy the panel's controls and create the new
  panel's set from scratch — most of the switch time was window creation. Each
  panel is now built the first time you visit it and simply shown or hidden
  after that. A typical switch drops from 85 ms to about 51 ms, and the slowest
  from 245 ms to 134 ms; values shown always follow the loaded session, and
  saving after browsing every panel stores exactly what was loaded.

### Fixed

- **Installer: shortcut bookkeeping moved to `Software\kapper.net\KiTTY-installer`,
  and the system-wide installer tracks its Startup shortcut machine-wide.**
  The old bookkeeping key carried this project's internal porting name
  (`Software\KiTTY-0.84-port`); upgrading removes it. In the system-wide MSI
  the autostart shortcut for the launcher was additionally tracked in the
  *installing user's* registry although it applies to every account — the
  cause of "Warning 1946" during installation on some machines, and of an
  uninstall run by a different administrator leaving the shortcut behind. It
  is now tracked machine-wide, like the other shortcuts.

- **A session's script starts on every connect (again), not just once.**
  With *Run the script on connect* ticked under **Session → Scripting**, the
  script ran when the session first connected and never again - a session
  that lost its connection and came back logged in and didn't do the script.
  It now starts on every connection, including auto-reconnect and Close+Restart.
  A script interrupted by the disconnect no longer blocks the next one either. (hknet/KiTTY#36)

- **A script that starts by waiting for the login prompt now works on fast
  connections.** With *Wait for a prompt before each line* on and *Except for
  first command* off,
  an on-connect script's first wait could only see output that arrived after
  the scripting engine started — and the engine started on a timer, one and a
  half seconds after connecting. On a fast link the prompt had already been
  printed by then, so the script silently timed out without typing anything,
  while the same session worked against a distant server. The engine now
  starts before the connection's first byte can arrive — and when a login
  script (Connection → Data) runs first, the scripting engine takes over the
  moment that script finishes, instead of up to one and a half seconds
  later. (hknet/KiTTY#36)

- **A login script read from a file survives without password protection.**
  In a portable install with `PortablePasswordProtection=legacy`, a login
  script given as a file (Connection → Data) worked on its first connect and
  then silently never again: KiTTY inlines the file into the session, and the
  unprotected stored form was misread as old-style scrambled content on the
  way back. Installs using the default protection were not affected.

- **Correcting a Ctrl+G search no longer ends it.** In folder-navigation mode,
  Ctrl+G searches across every folder — but deleting a mistyped letter back to
  an empty box silently dropped the search back to the current folder, so the
  corrected text searched the wrong scope. Editing the search text, even
  clearing it entirely, now keeps the cross-folder search armed. It ends when
  you step into or out of a folder, or when you come back to the search box
  after leaving it for other controls — browsing and picking results in the
  session list does not count as leaving.

- **Merely visiting the Translation page no longer pins the character set.**
  A session with no character set of its own follows the default, and the
  Translation page shows the name of that default. Opening the page wrote the
  shown name back into the session, so the next save stored it pinned — the
  session stopped following the default without anyone choosing that. The
  value is now only stored when a genuinely different character set is picked.

- **`antiidledelay` is limited at both ends.** The keepalive interval was held
  at a minimum of 5 seconds, but had no upper limit - and the value is turned
  into milliseconds in 32 bits, so an absurdly large one wrapped and produced a
  keepalive several times a second instead of once every few days. It is now
  capped at a day. If you have never set `antiidledelay` in `kitty.ini`,
  nothing changes.

## 0.85.1.0-beta — 2026-08-20

### The Harder, Better, Faster, Stronger Release

- **This release is built on PuTTY 0.85**, which is mostly security work. What it
  brings: a buffer overflow in ETM-mode packet decode, a limit on the length of a
  remote SSH greeting, rejection of a zero maximum packet size when opening a
  channel, rejection of RSA public keys with exponent 1, Argon2 parameter
  validation when both loading and saving key files, two memory leaks, and a
  use-after-free when a key is deleted while its decryption prompt is open.
- **The version scheme is now `0.85.x.y`**, starting at `0.85.1.0`.
- **The shipped binaries credit all three copyright holders** — PuTTY, the
  original KiTTY features, and this port — rather than upstream alone.

### Faster and less Flicker

- **Bulk output is about nine times faster.** Printing text asked Windows how
  fast the cursor blinks once per character — a system call — and the scrollback
  compressor grew its buffer a byte at a time. Testresults on our end: 3.55 MB
  output took 5.61 s and now takes 0.63 s to render.
  Both problems are inherited from upstream PuTTY and will be reported.
- **The configuration window repaints only what changed.** Switching category
  repainted the whole dialog, including the category tree and the buttons, and
  the panel visibly blinked once as the background erase caught up with the new
  controls. 94 ms per switch is now 74 ms, and the flicker is gone (hopefully).

### Broadcasting to other windows

- **`-sendcmd` and `/command` work again, and are now gated.** The receiving half
  was lost, but were advertised in `-help` and did nothing, in all our 0.84 releases.
  They are back, and a broadcast is only accepted when: (a) the sender identifies
  its target-group, (b) `[KiTTY] sendcmdmode` is on (default off), (c) the
  broadcast key matches, and (d) the session accepts broadcasts. Every refusal says
  which it was in the Event Log.
- **Session → Scripting** carries the per-session switch and the broadcast key,
  with Copy and Clear; an empty key means the session follows the installation.
  `-sendcmdkey <key>` aims a broadcast at the sessions carrying one key, and the
  terminal's Tools menu still toggles the current window.
- **Safety First**  is the name of the game here: a sequence sent to any terminal
  from an unconfirmed sender is a serious risk, therefore we have four gates now.
  You can easily switch these on and you got the massconfig-tool at hand.
  Keep in mind a rogue process sending commands might enter your commandline.
  The sendcmdkey exists to prevent accidential sends into terminals it is not security!
  User discretion is advised.

### Fixed

- **A clipboard read request could vanish silently.** When another program held
  the clipboard, KiTTY read that as "the clipboard is empty" and refused the
  request without asking and without a word in the Event Log. It now retries, and
  says so when the clipboard could not be read.
- **Copy did nothing when the clipboard held anything other than text** — an
  image, or nothing at all since login. Writing to the clipboard was guarded by a
  test that belongs to reading it.
- **`/delreg` deleted the wrong hive, and asked nothing first.** It now deletes
  the hive this KiTTY is actually using, and asks before doing it. But hey,
  don't delete us.
- **Settings written to one hive and read from another.** Under
  `KiClassName=PuTTY` the compile-time hive and the hive in use disagreed, so a
  global setting could be saved and then keep returning its old value. Named
  proxies, the `.sav` backup and the file-mode park had the same fault, and
  `/copytoputty` would have deleted the sessions it was copying from — it refuses
  that case now. kageant's session menu was empty under the same setting.
- **An imported session lost its folder.** and we don't want you to have to re-sort
  your folders after an import.
- **A connection that fails at the start is reported in the terminal**, this was
  a prior gap when the user opted to have no modal errors, but rather get those
  in the terminal window (which incidentially closed immediately). Now we keep
  the window open and Restart Session available, but we don't block the process
  anymore with a modal messagebox (if you want this).
- **ZModem menu entries are hidden when no helper is configured**, instead of
  offering actions that reported the error "unable to find ZModem program".
- **`-masterpwfile` is applied when it is parsed**, not later. The fix-translation
  would be: previously we missed to load the masterpassword and unlock failed.

## 0.84.1.75-beta — 2026-08-14

### Session folders

- **Folders can be browsed as rows of the saved-session list.** Opt-in with
  `foldernavigation=yes` in kitty.ini's `[ConfigBox]` section; off by default,
  and switching it off restores the folder dropdown exactly as before. Folders
  are drawn as rows (`work/`), Enter or a double-click steps inside, `..` steps
  back out, and the root lists the sessions that are in no folder. **New folder**
  takes its name from the session-name box, and selecting a folder row relabels
  **Save** to **Rename**, so a folder is renamed where it is shown — every
  session in it moves with it. **Ctrl+G** searches every folder and says which
  one each result came from. Storage is unchanged: a folder is still an
  attribute of a session.
- **The terminal's Saved Sessions menu groups by folder**, one submenu per
  folder, with the sessions that are in no folder below them, matching what the
  tray launcher has always done. Independent of the setting above.

### Fixed

- **Changing settings mid-session could move the session to another folder.**
  The saved-session list in *Change Settings* showed whichever folder was last
  browsed — possibly in an earlier run — and saving re-filed the session there,
  so a session started from a shortcut and saved after a tweak could end up
  somewhere unrelated. Searching with Ctrl+G could move one to the root the same
  way. The folder is now read from the store, so a mid-session save keeps the
  session where it is, and the list opens on the session's own folder.
- **A folder recorded on Default Settings is ignored.** It is invisible, because
  that entry is listed at every level, yet it was inherited by everything
  created from the defaults — including quick connect — and it kept deleted
  folders alive in the folder list.
- **Portable installs showed no folders.** Saving to a directory switched on the
  `browsedirectory` layout by itself, which looks for folders held as
  subdirectories that this version never creates, so the folder list stayed
  empty and the tray menu could not group them. `browsedirectory` is now off
  unless it is asked for.

## 0.84.1.74-beta — 2026-08-13

### Security

- **kageant checks that a remembered key is still the key it remembers.** A key
  loaded automatically at startup, when the drive holding it appears, or after
  being unloaded because that drive was removed, is now compared against the
  fingerprint recorded for that path — and the comparison
  is made on the key the agent actually ended up holding, not on a separate look
  at the file beforehand, so a file that changes between the two cannot slip
  through. A key that does not match is taken back out of the agent instead of
  being offered to servers. Nothing is asked while you are logging in or
  plugging a device in: the key list shows such a key as a *mismatch*, the tray
  tooltip says how many are being held back, and if a program asks the agent for
  its keys while one is, a notice says so and names the program. A changed key
  is accepted in one place only — open it in the key list and use *Accept this
  key*, which shows the fingerprint recorded beside the one the file holds now.
  Keys remembered by an earlier version have no fingerprint on record: those
  load once, are recorded then, and are checked from that point on.
- **Keys whose file was not there can be retried without restarting.** *Retry
  unavailable keys* in the key list loads the ones whose file has since turned
  up — a share that came back, or a stick that was already plugged in when
  kageant started, neither of which produces a device event.
- **Confirmation before key use now covers every route to a key.** Keys added
  through the agent's add-key extension, and SSH-1 challenge signatures, did not
  consult the per-key *ask before use* flag, so a key that should have prompted
  could be used without one. A malformed agent message with a zero-length body
  could also read past the end of its buffer and stop the agent.
- **A key that will not load no longer takes the agent off the air.** kageant
  reported it with a message box while loading its remembered keys, and a modal
  blocks the agent's message loop: until someone clicked it, the agent answered
  nothing at all, so every `ssh`, `git` or `scp` call that wanted a key simply
  hung. It is a tray notice now. The offer that box carried is not lost — the
  key stays in the key list, where *Remove* drops it for good, and clicking the
  notice opens that window.
- **kageant no longer blames an agent that is not there.** A key it could not
  add was reported as *"The already running agent refused to add the key"* even
  when kageant was the only agent running and had refused its own key; it now
  says which case it is.
- **"Ask confirmation before key use" remembers *Never*.** The three-state
  setting was stored as a plain on/off in the registry, so *Never* came back as
  *by key comment* on the next start — silently restoring prompts that had been
  switched off.

### Added

- **Open this session's log file from the window menu.** *Tools > Open log
  file* hands the log the session is writing to whatever opens `.log` files on
  your machine, so there is no hunting for it — which matters most with a
  rotating file name, where "the current one" changes through the day. It
  flushes first, so the newest lines are there even with *Flush log file
  frequently* switched off. A key can be bound to it with `openlogfile` in the
  `[Shortcuts]` section of `kitty.ini`. Refs hknet/KiTTY#31

### Fixed

- **"Clear log file" does what it says, and says what it does.** It closed and
  reopened the log through the same path a session start uses, which asks the
  *"what to do if the log file already exists"* question again — so with
  *append* selected an explicit clear silently did nothing. It now empties the
  file regardless of that setting. It also renames itself to *Start a new log
  file now* when the log file name contains a time, because there a
  close-and-reopen writes a new file and keeps the old one rather than clearing
  anything. Both log items are greyed out when the session is not logging.

- **Session logs carry timestamps again.** `Session > Logging > Timestamp
  (strftime format)` has been present in the dialog all along, and the value
  was saved with the session, but nothing ever wrote it to the log — so the
  field looked like it worked and did nothing. Setting a pattern now stamps
  every logged line, as it did before the move to the 0.84 core. `%f` gives
  milliseconds; an empty pattern, the default, leaves logs exactly as they
  were. Packet and SSH raw-data logs are unchanged: they already carry their
  own timestamps. A button beside the field fills in a working pattern, or
  clears it, so the feature can be tried without knowing strftime. A pattern
  the system cannot make sense of is ignored rather than acted on, and says so
  once in the Event Log, so it is clear that the format is the problem.
  Refs hknet/KiTTY#30

- **Log rotation works.** `Session > Logging > Log rotation delay` was the
  same kind of dead setting: stored, shown, never acted on. It now starts a
  new log file at the interval you set. Rotation is declined, with a note in
  the Event Log, when the log file name has no time-varying `&`-code in it —
  reopening the same name would overwrite the log rather than rotate it, so
  a rotation delay on a fixed name would have destroyed the log every few
  seconds. Put `&T` (or `&Y&M&D`) in the name to use it.

### Changed

- **A `kitty.ini` now says which KiTTY wrote it, and the shipped
  `kitty.ini.example` says which release it came from.** Neither file carried
  a version, so a copy of one - pasted into a support thread, or found in an
  install folder years later - could not be traced back to a release. The
  `kitty.ini` KiTTY creates on first run now opens with the build version and
  date, and the sample shipped in the installers and ZIPs opens with the
  release it was packaged for plus a link to that version of the file. The
  copy in the repository is deliberately left unstamped: there, the branch or
  tag you are reading already says which version it is.

## 0.84.1.73-beta — 2026-08-11

### Fixed

- **No `{CONTROL}{SHIFT}<letter>` shortcut could ever fire.** Ctrl+Shift+A..Z
  was claimed for the User Command menu whether or not a command existed
  behind that letter, so a binding in `[Shortcuts]` was shadowed by an
  accelerator that led nowhere. The letters are claimed only where there is a
  command behind them.
- **Ctrl+Tab window switching could not be enabled.** The per-session checkbox
  it reads had gone missing in the port, leaving the feature with no way in.
  It is back in *Window > Behaviour*, with `-noctrltab` to override it from
  the command line.
- **`debug=yes` logged the session password.** The pscp and WinSCP command
  lines were written to the Event Log verbatim, credentials included; the
  logged copy is now redacted. The command actually executed is unchanged.
- **kageant: enabling "load remembered keys at startup" with nothing loaded
  overwrote the remembered list with an empty one.** The snapshot is taken
  only when there is something to snapshot.
- **The Event Log dialog is keyboard-usable**: its buttons are in tab order
  and the list itself is reachable with Tab.
- **kageant's key list lost every colour it set.** The custom-draw handler
  returned `CDRF_DODEFAULT` after choosing its colours, which also explains
  the grey that never appeared on not-loaded rows; and a selected row ignored
  them until `CDIS_SELECTED` is cleared for the paint.
- **A new kageant menu id aliased an existing command**, because the handler
  masks the low nibble of `wParam` — ids there must be multiples of `0x10`.

### kitty.ini: the shipped file is generated, and it is the documentation

- **`kitty/kitty_ini.h` is generated from `docs/examples/kitty.ini.example`.**
  The two were maintained separately and the shipped template had drifted 36
  options behind the sample, so a KiTTY that wrote its own kitty.ini never
  mentioned the `[Agent]` block, `restrictacl`, `verifyagent` or `namedproxy`.
  Every line is emitted commented out, so the generated file sets nothing and
  the compiled-in defaults continue to rule.
- **Every one of the 134 documented options was read against the code that
  consumes it**, and the descriptions that were wrong or empty were rewritten.
  Among the corrections: `savemode=file` is not a file-based session store
  (sessions stay in the registry; the mode only adds the `.sav` import, and it
  was abandoned upstream); `[Print] height` is the line pitch, not a character
  size; `bgimage`'s text named a key KiTTY does not read; `Folders` is the
  live folder list, not a legacy cache; `mouseshortcuts` named neither of the
  two chords it governs; `shrinkbitmap` applies to the *Stretch+* placement
  only; and fifteen `[Shortcuts]` actions ship with no default key, which
  nothing said.
- **The drift check covers all seven sections in both directions** and now
  also reports a key that is read into a variable nothing consults. It runs
  from the build (`check_kitty_ini`) and again as a release guard.

### Changed

- **`[KiTTY] antiidledelay` is now a plain interval in seconds.** It was
  divided by ten to count ticks of a fixed 30-second timer, so the value meant
  roughly three times what it said (60 gave 180 seconds) with 30-second
  granularity. The timer period is now the interval itself. Divide a
  carried-over value by three to keep the interval you had; values below 5 are
  treated as 5.
- **`[KiTTY] transparency=no` switches the feature off, not only its
  controls.** A session with a level saved used to open translucent while the
  configuration panel and menu entries were hidden. A session set to `-1` is
  now a real lock as well: the window menu does not offer the Transparency
  entries, the keyboard refuses, and `/transparency` declines. The ini answer
  is kept separately from the live state, so the console command cannot
  re-enable what the file disabled.
- **A paste of more than 5120 characters asks for confirmation**, matching
  Windows Terminal, and `pastesize=0` disables the warning — the reader
  previously ignored any value not greater than zero, so it could not be
  turned off.
- **ZModem is enabled by default.** The flag was 0 in the code while the
  shipped template said `yes`, so whether you had ZModem depended on whether a
  kitty.ini existed.
- **The saved-session list sorts naturally**, so `2` precedes `10`.
- **kageant's single startup toggle became two.** One tray item installed the
  login autostart *and* remembered the loaded keys; they are now separate
  items and separate checkboxes. `[Agent] loadonstartup` is renamed
  `loadkeysonstartup` and migrated on first read. Whether kageant starts at
  login is read from the autostart artifact, and the portable path verifies
  the Startup shortcut points at *this* kageant.
- **kageant tints the row of a key that has just been used** — blue when it
  signed, amber when the request was refused — reported from the agent core
  and identified by the stored key's fingerprint.
- **The multiple-icon feature is removed**: one flag, a key that could only
  switch it on, and a cycling path no caller reached. The 50 embedded icons
  and the per-session icon choice are unaffected, and the icon file selector
  now filters on `*.ico`.
- **`config.c` is back to upstream.** `kitty/kitty_config.c` replaces it for
  kitty.exe, so the KiTTY edits that sat in `config.c` were compiled into
  nothing.
- `licence.pl` moved to `cmake/`, beside the file that runs it.

## 0.84.1.72-beta — 2026-08-10

### Fixed

- **Ghost keys in the agent's list — cleaned up automatically.** 0.84.1.71's
  registry parser stripped a startup entry's `,plain`/`,encrypted`/
  `,SHA256:…` markers in place inside the stored list, so its walk re-entered
  the record and read each marker as one more entry — and the next save
  persisted those fragments as startup keys. The result: phantom entries
  named `plain`, `encrypted` or `SHA256:…` in the key list and an inflated
  "keys not loaded" count. The parser is fixed, and the first start of this
  release removes the phantoms .71 wrote. Only entries whose "path" is
  exactly such a fragment *and* names no existing file are touched — real
  keys, including ones waiting for an absent USB drive, keep their entry and
  their place in the offer order.

- **Generating a key of one type over a key of another could crash
  kittygen.** Generate an Ed25519 key, then generate an RSA key in the same
  window, and freeing the first key's remains could free a stray pointer.
  Pre-existing; found while building the in-memory protection below.

- **The licence text can be scrolled at last.** The licence boxes of KiTTY,
  kageant and kittygen had no scrollbar, and our licence text is longer than
  upstream's, so its tail — including the actual MIT licence — could not be
  reached at all.

- **Removing a key over IPC while the key list was open could trip an
  assertion.** The list now survives the key set changing under it.

### kageant: a real key-list window

The keys window was a fixed-size listbox with three buttons. It is now the
place the agent is actually operated from.

- **A real list.** Columns for a key's type, size, state, confirmation and
  remaining lifetime, with draggable widths; the window resizes and remembers
  its geometry. Double-clicking a key opens a details dialog whose fields can
  be copied — every fingerprint form, the comment, and each file the key was
  loaded from, with a *Load key now* for deferred keys.

- **The list is the offer order, so you can arrange it.** Drag rows to
  reorder; the order is what servers are offered, and every refused key
  spends one of the attempts a server allows.

- **Drop a key file on the window to add it.** And each key chooses for
  itself whether it loads decrypted or deferred (passphrase on first use);
  one state-aware button decrypts or re-encrypts whatever is selected.

- **New key, Stop agent, Settings.** *New key* starts the key generator —
  after verifying its signature, with an explicit override if you know why it
  is unsigned. *Settings* collects the `[Agent]` options that used to live
  only in kitty.ini or scattered tray toggles, including the notice timeout
  and the IPC controls below. The window opens by itself when kageant starts
  with nothing loaded and nothing pending — an empty tray icon told nobody
  anything.

### kageant: the agent protocol's security features now work

- **`ssh-add -t` (key lifetime) is honoured.** The agent used to read a
  constrained add, drop the constraint bytes unread, and answer *success* —
  stock PuTTY behaviour, but it meant a client asking for a one-hour key was
  told it got one and did not. Lifetimes now count down in their own column
  and the key unloads when its time is up.

- **`ssh-add -c` (confirm each use) is honoured per key.** The confirm flag
  is a real per-key property — set by `-c`, by the comment convention, or by
  a checkbox in the key's details — and the global setting is three-state
  (always / per-key / never) everywhere it appears.

- **You are told when a program changes the key set.** Any process running as
  you may add or remove keys over the agent protocol — that is the protocol —
  but it now happens in the open: a notice names the key and, where it can be
  established, the requesting program. A notice, deliberately not a prompt:
  an agent that blocks on a dialog breaks every scripted `ssh-add`.

- **And you can refuse it.** `[Agent] lockdownmode` refuses all key
  management over IPC; `blockipcadd` / `blockipcremove` refuse just one
  direction. The window's own buttons are never affected, and signing is
  never blocked — this locks the key *set*, not the agent.

- **Notices are kageant's own windows now, not tray balloons.** Windows focus
  assist swallowed balloons silently, which for security notices is the worst
  possible failure. The new notices stay up for a configurable time
  (`[Agent] noticetimeout`), hold while the pointer is over them, and open
  the key list when clicked.

- **Denied confirmations cannot wedge the agent quietly.** A denied
  confirm-storm can be stopped with one click ("stop asking"), and resuming
  is offered three ways — the tray, a button in the key list, and the notice
  itself — instead of being buried.

### kittygen: a fresh key no longer sits in memory in the clear

- **The generated (or loaded) private key is held encrypted in memory** and
  decrypted only for the moment it is saved, exported or has a certificate
  attached. The window is typically open for minutes while a comment,
  passphrase and filename are chosen; a crash dump, the pagefile or the
  hibernation file taken in that window used to contain the key in the
  clear — a clean close wiped it, but a crash never runs the wipe. kageant
  has protected its held keys this way since 0.84.1.44; the key generator
  now does the same, in both the window and the command-line tool.

- **kittygen states its circumstances.** Its title now carries the same
  `(portable)` / `(RESTRICTED)` markers as the other windows — this window
  holds a fresh private key, so "am I restricted?" matters here too — and a
  restricted kageant launches it restricted instead of unrestricted.

### kitty.exe: knowing which agent answered

- **A signed KiTTY now checks who is answering its agent requests.** Any
  process may own the agent name and pipe, and whoever does receives every
  key request. When the answering process is not our own signed kageant,
  KiTTY says so — once, quietly — instead of letting stock Pageant, an old
  KiTTY agent or something pretending to be one collect requests while you
  believe your keys sit behind kageant's protections. (Inactive in unsigned
  local builds, which cannot vouch for anyone.) Running another agent on
  purpose is a fine reason to switch the warning off: a checkbox under
  *Connection → SSH → Auth* — marked as the application-wide setting it is —
  or `[KiTTY] verifyagent=no`.

### Changed

- **kageant answers `-h`, `-help` and `--help`** with its actual options
  instead of silently starting. The PGP-fingerprints box now presents
  upstream PuTTY's master keys as the historic information they are, rather
  than implying they vouch for these binaries.

- **`kageant -noload`** starts the agent with the startup keys left alone —
  for a test run, or a machine where the sticks are elsewhere.

- **The tray menu can start terminal sessions again.** The classic-KiTTY
  session launcher in kageant's tray menu is back, launching the kitty.exe
  found beside the agent — and hard-gated: it appears only when that binary
  verifies.

- **The update-available box gained a "View release notes" button**, so what
  changed can be read before agreeing to it.

- **kitty.ini.example documents eight keys that had escaped it**, including
  the removable-media `[Agent]` settings from 0.84.1.71 and the
  `[KiTTY] funkeys` / `namedproxy` globals; the drift checker is clean again.

## 0.84.1.71-beta — 2026-08-08

### kageant: keys that live on removable media

A key kept on a USB stick or a network share is not there when you log in, and
is there later. kageant skipped it, said so, and left you to add it by hand every
time.

- **It waits for the drive instead.** A startup key whose file is not reachable
  is kept, and loaded the moment the drive appears — Windows says when that
  happens, so there is no polling. `[Agent] retrykeys` (on by default) turns it
  off; it does nothing at all unless a startup key is actually missing.

- **Optionally, the key goes when its media does.** `[Agent] unloadonremove`
  (off by default) drops keys whose drive has been removed, and brings them back
  when it returns. Off by default deliberately: pulling a stick should not break
  the session you are authenticating right then, and a loaded key is already
  protected in memory. **Nothing is ever deleted from disk.** A key that is also
  loaded from a file still present is left alone — the agent holds one key
  however many files it came from.

- **Keys keep their place.** The list is the order keys are offered to a server,
  and every key offered that the server does not want spends one of the attempts
  it allows before locking the account out. A key that comes back returns to its
  own position rather than the end.

- **A key you asked for is no longer lost.** The startup list was rebuilt from
  the keys currently loaded, so an entry whose drive was absent was dropped
  permanently: start with the stick unplugged, add any other key, and the entry
  was gone with nothing said.

### kageant: knowing which key is which

- **Each startup entry now remembers its key's fingerprint.** If a key file has
  been replaced — swapped on a stick, overwritten by an old backup, a copy that
  is not yours — kageant says so before loading it and shows both fingerprints.
  Accepting is permanent, because a key you rotated yourself looks exactly the
  same as one that was tampered with, and a warning you can only dismiss is one
  people learn to click through.

- **A failing key names its file**, and if it is in the startup list, offers to
  stop trying it. "Couldn't load this key (unable to open file)" at every login,
  naming nothing, was not something you could act on. (cyd01/KiTTY#522)

- **Removing a key removes it properly.** It used to come back at the next
  start: the startup list was only written when a key was *added*. Removal now
  drops every file that provided that key, including ones whose media is absent,
  and takes the key out of the saved offer order so it cannot reclaim its old
  position later. Remove also asks first now.

- **The key list shows the state in its own column**, so "(encrypted)" is no
  longer pushed off the edge by a long comment — that is the field saying
  whether a key is usable now or will ask for a passphrase first.
  Double-clicking a key shows its full fingerprint, its comment, and every file
  it was loaded from.

### Fixed

- **An empty remote clipboard write no longer clears your clipboard.** A
  multiplexer sends one whenever a selection gesture selects nothing — a
  double-click below the prompt, a drag that did not move — and an accidental
  click threw away whatever you had copied. (cyd01/KiTTY#495)

- **Three settings that were shown but did nothing, or lied.** "Lines scrolled
  per wheel turn" (*Window → Scrollback*) had never been read by anything; it
  works now, and the panel explains its two negative values. The kscp flags
  default to `-r`, so dropping a **folder** on a terminal uploads it, which the
  panel had always claimed. The "Alternate host name (HostAlt)" field is gone —
  nothing ever read it.

- **Importing a `.ktx` session file could silently weaken it.** Its loader kept
  its own copy of every default, and two had drifted: remote clipboard writes
  came back as *Allow* where the setting says *Ask*, at a 64 MB limit rather
  than 16. The fallbacks all come from one place now.

### Changed

- **Passphrases typed while adding keys are kept encrypted** and scrubbed after
  a minute even if the add is abandoned half-way. They were held in the clear
  until the add finished, and an add that never finished held them until kageant
  exited. `[Agent] passphrasecacheseconds` adjusts it.

- **File transfers default to SFTP** rather than the legacy SCP protocol, which
  OpenSSH deprecated and which many hardened servers no longer offer. SCP stays
  selectable per session for equipment that has nothing else.

- **The WinSCP executable says it is not a setting of this session** — it sits
  among per-session options but is shared by all of them.

## 0.84.1.70-beta — 2026-08-08

### Fixed

- **"Inherit New Session…" inherited nothing.** It opens a new window at the
  configuration box carrying the current session's settings — except it did not:
  the settings were handed over through a temporary saved session that the
  original window deleted before the new one could read it, so the box came up
  blank. Everything now travels through memory shared between the two windows,
  which cannot race. As a consequence a session's stored password no longer takes
  a trip through the settings store on its way to a new window, a box opened from
  a restricted window is itself restricted, and one opened from a window whose
  master password is unlocked no longer asks for it again.

- **`kitty.ini` no longer eats its own values.** A value was taken verbatim from
  after the `=` to the end of the line, so `configdir = C:\somewhere` carried the
  space into the path and the directory was never found — and quoting the path,
  the obvious next move, made it worse by keeping the quotes. Values are now
  trimmed of surrounding whitespace and of one pair of matching quotes.
  (cyd01/KiTTY#549)

- **A `configdir` that is not there says so.** It used to be ignored in silence,
  and KiTTY started on a different set of sessions with nothing to explain why.
  You are now told what was not found — whether the drive is missing, or the path
  under an existing parent — and asked whether to start anyway.

- **The `opennewcurrent` shortcut opened a connection, not a configuration box.**
  It duplicated the session instead, which is what the `duplicate` shortcut is
  for.

### Line spacing

- **Rows can be given more air** — *Window → Appearance*, "Line spacing", a
  percentage of the font's own line height from 100 to 300. The extra is shared
  above and below the text, so the glyphs stay centred rather than hanging from
  the top of a taller row. Above 100 % the line-drawing characters stop joining
  up between rows: the gap between cells is real and the terminal cannot paint
  across it, which is why the default is 100. (cyd01/KiTTY#524)

### Quick connect

- **The host comes with you.** In quick connect, "Inherit New Session…" now
  carries the current host name into the new configuration box, selected — so the
  next machine in a cluster is reached by editing one character and pressing
  Enter, with every other setting already in place. Bind
  `[Shortcuts] opennewcurrent` to a key and that is one keystroke per machine.
  Outside quick connect the box opens without a host, exactly as before.
  (cyd01/KiTTY#519)

- **The configuration box says when quick connect is on**, in its title. Until
  now the only sign was where the caret happened to be.

- **Loading "Default Settings" arms quick connect immediately.** It used to take
  effect only at the next start, so the documented way of switching into that
  mode appeared not to work at all. Loading any other session switches it off
  again.

### Changed

- **The "Alternate host name (HostAlt)" field is gone** from *Connection → Data*.
  Nothing ever read it: it held the host that "Inherit New Session…" had just
  cleared, for a feature that was never finished. Saved sessions that carry the
  value still load.

- **The SSH certificates guide** explains why someone arriving from an older
  KiTTY thinks certificate support is missing, and how to revoke a certificate.

## 0.84.1.69-beta — 2026-08-07

### Workplace proxy mode

Some days the proxy is not a property of any session — it is a fact about where
you are sitting. At a customer site, on a VPN, in a hotel, everything has to go
through one local proxy, and editing every session to say so (and remembering to
undo it) is the wrong shape of work.

- **One proxy for every connection, until you switch it off.** Pick a named proxy
  in *Connection → Proxy*, say how long for, and every connection KiTTY makes
  goes through it — from the configuration box, the launcher, a desktop shortcut,
  an `ssh://` link or an auto-reconnect — whatever each session stores. The
  launcher's tray menu does the same in one click, using the proxy and the
  duration you chose last. **No session is modified**, so there is nothing to
  undo afterwards.

- **It cannot be left on by accident.** The mode is held by the session launcher:
  switch it off, let the time run out, or stop the launcher — logging off,
  shutting down or killing it all end it, with nothing left behind. A launcher
  that KiTTY started only to hold the mode closes again when the mode ends
  (`[Launcher] exitwithworkplace=no` keeps it); one you started yourself always
  stays, and switching the mode off from its own menu never closes it under you.

- **A window says whether *this connection* went through the proxy** — a dark
  green frame and `⇄ workplace proxy` in the title, for as long as that
  connection lives. A window that was already open when you switched the mode on
  is left alone, because it is not going through it; one that is keeps saying so
  after the mode ends, because an established connection cannot be re-routed.

- **You are told when it matters, once.** A notice near the clock says when the
  mode goes on, when it goes off and when it times out — and once, on the next
  start of any kind, if it ended because the launcher went away. Only the timeout
  notice offers to switch it back on; if you switched it off yourself, KiTTY
  assumes you meant it. The notice stays up while the pointer rests on it.

- **If the proxy stops answering** — usually because you have left the place it
  belongs to — the failed connection offers to take you to *Connection → Proxy*.
  It does not switch anything off on your behalf.

### Named proxies

- **A named proxy can say whether its Name/IP is a machine or a saved session.**
  PuTTY always tries a proxy name as the title of a saved session first, which is
  how a jump host that happens to share a name with one of your sessions quietly
  pulls that session's entire configuration — its user name, its key, its own
  proxy — into the connection. Here KiTTY parts company with PuTTY deliberately,
  because a setting whose meaning depends on what your session list contains is
  neither clear nor safe: each proxy now states which it is. Nothing changes
  unless you want it to — proxies that say nothing keep PuTTY's behaviour, and
  `[KiTTY] namedproxy=hostname` switches the default for all of them.

- **The Event Log says which reading was used.** The two can reach the same
  machine by the same route and differ in everything else, so a connection that
  worked was never evidence of which one ran.

- **Choosing a named proxy for a connection no longer overwrites the session.**
  The droplist on the Session panel is an override that lasts for that connection
  only, and its caption says so while it is armed. Adopting a preset permanently
  is now a separate, deliberate act — *Load named proxy pre-sets…* on the Proxy
  panel — with a confirmation naming everything it replaces, including the user
  name and password.

- **The editor fills in the usual port, and asks before saving without one.** A
  definition with no port reached the connection as port 0 and surfaced as
  "Cannot assign requested address", which reads as a network fault rather than
  an empty field.

- **Proxy chains are bounded at five links** (`[KiTTY] proxychainmax`), with every
  link written to the Event Log in order. Refusing an over-long chain used to
  crash instead of reporting it.

### Fixed

- **Save could save over the wrong session.** With the session-name box empty,
  Save used the position of the highlighted row against the *unfiltered* list, so
  after typing in the search box it could write your settings over a different
  session entirely. Save is now greyed out until the box has a name in it.

- **A fresh configuration box treated the restored session as "loaded"** and
  could save over it without being asked to.

### Changed

- **A host that writes your clipboard is asked about first.** For new sessions,
  remote clipboard writes default to **Ask** rather than Allow, and the largest
  payload a host may write is 16 MB rather than 64. Existing sessions keep the
  settings they have.

- **KNOWN-ISSUES.md carries limitations, not five betas of history.** The
  release-by-release changelog it had accumulated is on the releases page, pinned
  per tag; four live limitations that were buried in it moved up to where they
  belong.

## 0.84.1.68-beta — 2026-08-03

### The remote clipboard

A terminal can be asked by the machine at the other end to put something on your
clipboard, or to hand over what is already there. KiTTY now does both, and the
second one asks first.

- **A remote host can set your clipboard (OSC 52).** Programs that copy for you —
  `tmux`, `neovim`, a script that wants to hand you a URL — work over the
  connection instead of needing a mouse selection. *Window → Selection → Remote
  clipboard* controls it, and it defaults to **Allow**, matching every comparable
  terminal: this direction changes what you paste next, but it cannot disclose
  anything to the host. Set it to **Deny**, or **Ask** to be prompted.

- **A remote host can ask to READ your clipboard — and you are asked every
  time.** This is the direction that can leak, because a clipboard holds a
  password for half a minute at a time and the host chooses when it asks. It
  defaults to **Deny** and there is deliberately **no Allow setting**: a
  permission to read can only be granted from the prompt, with the request on
  screen, and it always expires. The prompt shows how much text would be sent and
  the first few characters of it, and offers to allow the one request, the next
  few minutes, a number of requests, or the rest of the session — with the
  narrowest option pre-selected. **Deny** is the default button.

- **Nothing leaves the window while you are working somewhere else.** A granted
  permission is suspended when the window loses focus and resumes when you come
  back, without asking again. Covers all three clipboard protocols. Switch it off
  in *Remote clipboard* if a background job needs to copy its own output.

- **A host that asks too often gets refused, not rewarded.** There are limits on
  how many reads a grant is worth, how close together they may come, and how
  often you can be prompted at all. Exceeding the pacing limit refuses that one
  request and leaves your grant alone — the settings are in *Remote clipboard →
  Limits*, along with a ceiling on payload size (64 MB) and a cap on how often a
  server may overwrite your clipboard (10/second).

- **You can see when the clipboard is being used.** The title bar shows a
  clipboard icon with an arrow — up when data left you, down when the host put
  something in — and on Windows 11 the window frame is tinted, amber for a read
  and blue for a write. A standing permission is shown at the end of the title in
  brackets, greyed with a pause mark while it is suspended. Tray notifications
  cover refusals and oversized payloads. All of it is in *Remote clipboard →
  Notices*.

- **KiTTY answers the kitty terminal's clipboard protocol (OSC 5522) as well.**
  Unlike OSC 52 it can say who is asking, so a program you have approved once is
  not asked about again while that lasts, and it has real error codes instead of
  silence. Reads are served; writes answer "not implemented" so an application
  falls back to OSC 52.

- **far2l clipboard payloads over about 2 KB no longer vanish.** Anything larger
  was silently truncated and then dropped, with no error at either end. Verified
  to 80 KB.

- **The clipboard settings have their own panels.** They had outgrown *Window →
  Selection*, which was showing labels cut off at the edge. They are now under
  *Window → Selection → Remote clipboard*, split into permissions, *Limits* and
  *Notices*.

### Fixes

- **The login script never ran.** A session's login script — the stored
  expect/send pairs that answer a login prompt for you — has not executed at any
  point in the 0.84 line. The settings saved, the interface was present, and
  nothing about it looked wrong; what the port had lost was the step that arms it
  before a connection. It now runs on connect, on **Restart Session** and on every
  automatic reconnect. Inherited from classic KiTTY.

- **The login script is readable and editable again.** *Connection → Data* showed
  its raw stored form in a single-line box, where it could not be read and where
  typing produced a value nothing could open. It is now a multiline editor with
  one entry per line, plus a **Load from file** button. A file picker beside it
  was writing to the *rutty* setting instead, silently changing a different
  feature; it is gone.

- **A saved login script is protected at rest**, the same way the password stored
  beside it is.

- **Save no longer replaces a session you never opened.** Clicking a name in the
  session list only fills in the name — it does not load that session — so
  pressing **Save** afterwards overwrote it with whatever was in the dialog:
  host, port, protocol and all. KiTTY now asks first, defaulting to **No**, and
  only when the session exists and was not the one you loaded.

- **Long settings are no longer truncated**, and the buffer they are read back
  into grew to match.

- **The restricted-ACL option says when it is on** — and is now applied in the
  places it was being skipped.

- **"Save settings on exit" and the AltGr option do what they say.**

- **Title placeholders and a fixed window position behave.** The pinned position
  is stored the way Windows actually applies it.

- **The launcher's update notice does something when clicked.**

### Removed

- **KiTTY no longer writes encrypted `.ktx` configuration files.** They were
  encrypted under a key compiled into every copy of KiTTY, so anyone with the
  program could read them — the protection was apparent rather than real. Reading
  them still works, so existing files and imports are unaffected, and anyone who
  used the feature is told once, with a dialog, what to do instead. The
  **Shift+F12 / Shift+F11** scramble shortcuts are gone for the same reason.

- **Five bookkeeping values are no longer written to the registry.** Nothing ever
  read them.

- **An installer action that never worked has been removed.** It was meant to
  stop Windows reopening your KiTTY windows after a background upgrade, but it
  ran too late in the install to have any effect. Deployers who want that can
  pass `MSIDISABLERMRESTART=1` on the `msiexec` command line, where it does work.

### Other

- **A session can hide its own window buttons** — system menu, Close, Minimize,
  Maximize — for kiosk and embedded use.

- **KiTTY explains why it still asks for the master password** after you switch
  the feature off.

## 0.84.1.67-beta — 2026-08-01

- **A disconnected window still says which connection it was.** When a session
  ended, the title was replaced with `KiTTY (inactive)` — so a screenful of dead
  windows all read the same thing and none could be told apart. The window now
  keeps the title the session gave it and adds the state at the end:
  `user@host: ~ (inactive)`, or `⚠ user@host: ~ (disconnected)` when the
  connection was lost. The name stays in the first few characters, where it
  survives the taskbar cutting the title short. Fixes hknet/KiTTY#22.

- **A lost connection is marked as lost.** The warning marker in the title was
  unreachable whenever auto-reconnect was enabled — which is the default, and
  precisely the case that leaves a screenful of dead windows. It is now set when
  the link drops, and cleared when the session comes back.

- **A session set not to reconnect reports its errors again.** With the global
  auto-reconnect switch on and *Attempt to reconnect on connection failure* off
  for a session, a dropped connection produced neither a retry nor a message:
  the error was swallowed and the window simply went quiet. The per-session
  setting is now part of the decision, so such a session falls through to the
  normal error report. Inherited from classic KiTTY.

- **Quick connect: start from Default Settings and type a host.** The
  configuration box opens with the session used last, which is the wrong
  starting point if you connect by typing an address — the settings that arrive
  belong to whichever host you visited last. Load **Default Settings** once and
  KiTTY remembers that, opening on the defaults with the cursor already in
  *Host Name (or IP address)* and its contents selected, until you load another
  session. Typing an address into an unsaved session does not change that, so
  the mode survives connecting. `loadlastsession=no` in the `[ConfigBox]`
  section of `kitty.ini` makes it permanent. Fixes hknet/KiTTY#23.

- **`ssh://` links work on the command line and from the browser.** KiTTY
  registers itself as the handler for `ssh://` URLs but could not read one:
  every link its own handler delivered arrived as a hostname that could not be
  resolved. `ssh://[user@]host[:port]` is understood again, as is
  `kitty://session-name` for a saved session. A password in the URL is parsed
  only so that it cannot be taken for part of the address, and is then
  discarded.

- **A URL without a port connects to the right port.** `ssh://host` and
  `telnet://host` set the port to "unspecified", and nothing turned that back
  into 22 or 23, so the connection attempt read `port -1` and failed. Both now
  take the protocol's own default.

- **Registering KiTTY for URLs and for `.ktx` files works without administrator
  rights, and stops taking over other programs' settings.** `-sshhandler` and
  `-fileassoc` wrote to a machine-wide part of the registry, so from an ordinary
  prompt they registered nothing at all — while reporting success. They now
  register for the machine when run as administrator (the machine-wide
  installation asks for the rights itself) and for your account otherwise; a
  protocol or extension another program opens is reported and left alone unless
  `-force` is given, in which case the previous setting is exported to a `.reg`
  file and the report names the command that restores it. `-uninstall` removes
  what KiTTY registered, and a portable KiTTY asks before writing to the
  registry at all. The URL scheme for opening a saved session is now `kitty://`;
  `putty://` is registered only with `-puttyurl`, and read either way.

- **`-help` prints the command-line options.** KiTTY had no way to ask: the list
  existed in the program but nothing ever displayed it, and it had gone stale —
  it documented seventeen options this port does not have. It has been checked
  against the real command line and now covers the current ones, including quick
  connect and the registration switches above.

- **The Event Log has a Clear button**, and no longer disappears from under you.
  A device that sends a channel message for a channel that has just closed — a
  Cisco does this on every logout — could make the window and its Event Log
  vanish at the moment you clicked something, because that path quit the program
  without honouring the rule that an open Event Log defers the close.

## 0.84.1.66-beta — 2026-07-29

- **The configuration box no longer crashes when you start a session from
  another page.** Changing a session name, switching to any other page of the
  dialog — *Connection > SSH > Bugs*, say — and pressing **Start** ended KiTTY
  with an assertion failure instead of opening the session. The saved-session
  list exists only while the *Session* page is on screen, while **Start** and
  **Open** sit at the bottom of the window and can be pressed from anywhere;
  with the list not displayed there is nothing highlighted to prefer, so those
  buttons now simply launch the settings in front of you. The same fault could
  be triggered with **Ctrl+G** from another page, and is fixed too.

- **Clicking a saved session no longer jumps the highlight to the first one.**
  With session names that sort before "Default Settings" — anything starting
  with a digit, so IP addresses in particular — clicking a session moved the
  highlight to the first stored session while the name box showed the one you
  clicked; typing such a name did the same. The list is found with a binary
  search, which needs a sorted list, and "Default Settings" is pinned to the top
  without sorting with the rest, so the search always ran off the near end; it
  is now confined to the sorted part. Picking a session with the mouse also no
  longer re-runs the type-ahead search at all. Reported as hknet/KiTTY#19.

- **kittygen no longer crashes when a certificate is added to a key you have
  just generated.** *Key > Add certificate to key* (and *Remove certificate from
  key*) ended the program outright if the key had been generated in that session
  rather than loaded from a file. The fault is inherited from upstream PuTTY,
  and it survived because the natural way to try the feature is on a key you
  loaded, which was never affected.

- **kittygen clears a generated private key out of memory.** A key you generated
  stayed in the program's memory in the clear until the process ended — not
  wiped when the window closed, and a second **Generate** left the first key
  behind as well. Keys are now cleared when they are replaced and when the
  window closes, along with the mouse-movement entropy collected to make them.
  Passphrases were already handled correctly. `kittygen-cli` gets the same
  treatment on one error path: failing to open the output file skipped the
  cleanup that wipes the key and both passphrases.

- **How to use SSH certificates is now documented.** KiTTY has long been able to
  attach a certificate to your key, take one on the command line, and trust a
  certification authority that signs host keys — none of which was written down
  anywhere. **[docs/SSH-CERTIFICATES.md](docs/SSH-CERTIFICATES.md)** covers both
  halves end to end, including the OpenSSH server side (`TrustedUserCAKeys`,
  `AuthorizedPrincipalsFile`, host certificates) and a throwaway local lab for
  trying it out without touching a production server.

- **Binaries no longer carry the build machine's directory names.** Assertion
  messages embedded the full path of the source tree as built; they now read
  `/kitty/...`, which also makes builds reproducible across machines. From
  upstream PuTTY, together with a fix to three SSH-1 error paths that blamed the
  server for a protocol violation it had not been told about.

## 0.84.1.65-beta — 2026-07-28

- **"Send to tray on startup" is back, and works again.** The setting was still
  saved with every session, but there was no way left to see or change it, no
  way to switch it on for a single launch, and a session that had it set simply
  opened as a normal window — so sessions kept for SSH tunnels no longer tucked
  themselves away. The checkbox is back in **Window > Behaviour**, together with
  **Maximize on startup** and **Full screen on startup**, which had disappeared
  the same way; `-send-to-tray` works on the command line again (this is what
  the launcher writes into the shortcuts it creates); and a session set to start
  in the tray now goes there once it is connected, so a host-key or password
  prompt is never hidden behind the tray icon. Sessions that had the option set
  all along need no change — they simply behave as configured again.
  Refs hknet/KiTTY#20

- **The kageant passphrase prompt no longer opens off-screen.** When the
  terminal asking for the key was minimised — a shortcut set to "Run:
  minimized", for instance — the prompt was positioned relative to that
  minimised window and ended up far outside the visible desktop: listed in the
  taskbar, impossible to bring into view, with the session waiting for a
  passphrase that could not be typed. The prompt now ignores a minimised window
  and always opens inside the visible area of a monitor.
  Refs hknet/KiTTY#21

- **Portable KiTTY no longer depends on the PC it was set up on.** A portable
  install used to keep its master password in the Windows registry of that one
  machine, so the same folder copied to another PC could not open its saved
  passwords — and a newly created portable install inherited that leftover,
  could never set up a master password of its own, and silently protected its
  passwords in a way that only worked on the machine that wrote them. Portable
  installs now keep this next to their sessions, in a `Security` folder. If
  yours relied on the registry, it is moved there once, at startup, and KiTTY
  tells you where the folder is — copy it into your other portable installs if
  they share the same master password.

- **A master password that protects nothing is now cleaned up.** Exporting
  sessions on 0.84.1.48–0.84.1.65 quietly turned the password you typed into a
  master password for your own sessions. Where nothing is actually protected
  with it, it is now removed at startup, silently. Anything still protected with
  it is left exactly as it is.

- **Exporting sessions no longer creates a master password.** "Export all" now
  asks how the exported files should be protected: with a password of your
  choosing, which lets them be imported on any PC, or for this Windows account
  on this PC only. The password belongs to the exported files alone — it is
  shown once when the export finishes, with a Copy button, and nothing about
  the sessions saved on your machine is changed. Previously exporting quietly
  set up (and permanently stored) a master password for your own session store
  as a side effect, one you may never have been told about.

- **Importing asks for the import password.** If the files were exported with a
  password, KiTTY asks for it once for the whole import; a wrong password can
  be retyped three times and then leaves your sessions untouched rather than
  half-imported. Files exported for one PC and account import with no prompt at
  all, and say so plainly if they are opened on a different PC or account
  instead of importing sessions with blank passwords. Imported passwords are
  always re-protected by the store they land in.

- **A portable install can now be told to protect passwords with Windows,
  permanently.** `[KiTTY] PortablePasswordProtection` accepts a new value,
  `dpapi`: saved passwords are protected for this Windows account on this PC,
  no master password is created, and KiTTY never asks you to set one. This was
  previously reachable only by cancelling the master-password dialog by hand,
  so an unattended install or import that wanted it had no way to say so and
  the only scriptable alternative was storing passwords in the clear.
  Passwords protected this way do not travel: copied to another PC or Windows
  account they cannot be decrypted. `-masterpwfile` is refused in this mode
  rather than one of the two quietly winning. The default (`master`) is
  unchanged.

- **Session files can now carry a password in the clear, on purpose.** If you
  roll sessions out with a script, write the password as
  `Password\PLAIN:yourpassword\` and KiTTY will take it exactly as given, then
  protect it properly the first time that session is saved. Previously any
  password without one of KiTTY's own protection markers was assumed to be
  encrypted by a very old KiTTY and was decoded on that assumption — so a
  password written in the clear was silently turned into garbage, and you only
  found out when the login failed. Unmarked passwords are now decoded only when
  the value really carries the old format's signature, and are otherwise taken
  literally. Genuine old session files still import exactly as before. Note
  that a file containing `PLAIN:` is a cleartext secret until it is imported —
  treat it accordingly and delete it afterwards.

- **The diagnostic dump has been removed.** `/savedump` and the `kitty.exe
  -savedump` command-line switch are gone, along with the `kitty.dmp` file they
  produced. The dump was meant to be sent in with a bug report, but it was
  written encrypted under a key compiled into the program and KiTTY shipped no
  way to read one back — so neither you nor anyone helping you could open the
  file the instructions told you to send. Alongside that it carried a standing
  risk of leaking configuration secrets into a file people were encouraged to
  share, and it could hang partway through writing. For troubleshooting, use
  the **Event Log** (right-click the title bar → *Event Log*) and session
  logging (**Session → Logging**) and attach those instead. A `kitty.dmp` left
  over from an earlier version is not read or updated by KiTTY and can be
  deleted.

## 0.84.1.64-beta — 2026-07-26

- **`restrictacl=yes` hardens every KiTTY process from one setting.** KiTTY
  inherits PuTTY's `-restrict-acl` option, which locks the Windows process down
  so other programs running as you cannot read its memory — where a session
  password sits while you are connected. It previously had to be added to each
  shortcut target one by one, and anything you overlooked stayed unprotected.
  `restrictacl=yes` in the `[KiTTY]` section of `kitty.ini` now applies it to
  the terminal windows, to sessions opened for you from the configuration box,
  to the tray launcher, and to **kageant**, the process that actually holds
  your loaded private keys. Off by default. `kittygen` does not read it and
  still takes `-restrict-acl` on its command line.

- **…and that setting is read from `kitty.ini` only, never the registry.**
  Global settings are normally looked up in the registry first and in
  `kitty.ini` only as a fallback. For a hardening switch that order fails in
  the dangerous direction: a leftover registry value would silently discard the
  `restrictacl=yes` written in the file, with nothing to say the hardening had
  been skipped.

- **Silent installs have defined behaviour for open windows.** A silent (`/qn`)
  upgrade — also how package managers drive the installer — closes open KiTTY
  windows and reopens them afterwards with their sessions reconnected, as the
  interactive upgrade does. An unattended installation run by a management
  system under the machine account closes them without reopening, because
  there is no desktop to reopen onto; `MSIDISABLERMRESTART=1` selects that
  behaviour explicitly.

## 0.84.1.63-beta — 2026-07-26

- **The configuration backup is now taken *before* something is overwritten or
  deleted.** KiTTY keeps timestamped copies of its configuration, but it only
  ever wrote one *after* a change — the wrong moment for the case a backup
  exists for. A copy is now written immediately before a saved session is
  overwritten, a session is deleted, or a folder is deleted, so the newest
  backup still contains whatever just disappeared. Saving a session under a
  name that does not exist yet writes none: there is nothing to preserve yet.

- **…and no longer on every session you open.** Opening a session went through
  the same path and wrote a copy each time, so a handful of opens pushed every
  earlier backup out of the retention window. A copy is now written only when
  something has actually been changed, and the export runs in the background
  instead of holding up the configuration window.

- **The backup file no longer loses part of what it stores.** The registry
  backup is produced by Windows' own registry exporter rather than written by
  KiTTY. The previous writer stored no binary values at all and turned
  multi-value entries into plain text, so window positions and sizes, and
  kageant's startup key list, came back missing or unusable after a restore.
  Everything is now preserved exactly, and the file is a genuine `.reg` that
  can be inspected or imported by hand. Backups written by earlier versions are
  still restored as before.

- **The configuration password has been retired.** `/configpassword` and
  `/-configpassword` are gone and backups are no longer encrypted. The
  mechanism protected a copy of data the registry already protects — saved
  passwords are held there with Windows DPAPI — while keeping its own key in
  plain text in that same registry, and the copy in `kitty.ini` was scrambled
  only with a value built into every KiTTY. Existing encrypted backups remain
  readable: KiTTY asks for the password when loading one.

- **Portable installs: backups were incomplete and were never tidied up.** The
  launcher configuration was never included in a portable backup although the
  documentation described the backup as a complete copy, so restoring one
  silently lost the launcher entries; and the clean-up meant to keep the newest
  `portablebackupcount` folders never ran at all, whatever that setting said. A
  portable backup now contains everything in the configuration folder except
  the programs, the `Backups` folder and log or dump files, so future additions
  are included automatically. **On upgrading, the first backup written will trim
  accumulated folders to `portablebackupcount` (5 by default)** — raise it, or
  copy them aside, to keep more.

- **New registry backups are named `kittynew-*.sav`.** The intended name never
  took effect, so they were written as `kitty-*.sav` — the same name an older
  KiTTY installed alongside uses for its own backup, which was exactly what the
  name change was meant to avoid. Existing `kitty-*.sav` files are left alone
  and are still read if a restore is needed.

- **The Kex, Host keys and Cipher lists show every algorithm.** The lists were
  shorter than their contents, so the last entries — and the
  `-- warn below here --` marker that separates the algorithms KiTTY considers
  weak — could only be reached by scrolling. They now show everything at once
  and grow by themselves if an algorithm is added.

- **Two configuration-window fixes.** *Delete* on "Default Settings" used to
  beep, which reads as a broken button; it now explains that this is the
  template new sessions start from and offers what is actually available —
  hiding it from the list. And **Ctrl+G** jumps to the session search after
  clearing the folder filter, so it searches every folder, where Ctrl+F
  searches only the one being viewed.

- **Documented:** what a restored backup does and does not bring back. Restoring
  a **registry** backup on another computer or user account returns the sessions
  but not their saved passwords, because Windows ties those to the account that
  saved them. Portable stores are unaffected — there passwords are protected
  with the master password and are made to travel with the store.

## 0.84.1.62-beta — 2026-07-25

- **SSH security confirmations can now appear inline in the terminal.** The
  confirmations for an unknown host key, a changed host key, or a weak/legacy
  algorithm can be shown OpenSSH-style in the session window — the details are
  printed and you answer by typing — instead of a modal dialog box that grabs
  focus. Turn them on per prompt in `kitty.ini` under `[KiTTY]` (default `yes` =
  classic dialog, `no` = inline); every choice the dialog offers is available
  inline, so nothing is lost by going inline:
  - `modalnewhostkeyconfirmation` — unknown (first-seen) host key: `yes` accepts
    and caches it, `once` connects without caching.
  - `modalchangedhostkeyconfirmation` — changed host key: a deliberate two-step
    confirmation — `yes` accepts the new key for this connection, then
    `confirmed` replaces the stored key. A bare `yes` is re-asked, `no` or Enter
    keeps the old key and connects once, Ctrl-C abandons.
  - `modalweakkeyconfirmation` — weak or legacy algorithm, including the key
    exchange used by older servers.

  Each setting is independent, so some prompts can stay dialogs while others go
  inline. During an already-authenticated session (a rekey) the running program
  owns the terminal, so these abort the connection rather than prompting.

- **Session folders can be renamed, and deleted without losing sessions.**
  Renaming a folder was previously impossible, and deleting one that still held
  sessions quietly did nothing at all — the folder was back the next time KiTTY
  started. Both now work properly, in the registry and in portable mode:
  - **Rename** by selecting the folder, typing the new name over it and pressing
    the button, which relabels itself to *Rename* so the action is visible. The
    sessions in the folder move with it.
  - **Deleting** a folder that still contains sessions asks first, and moves
    those sessions to the root list rather than deleting them.
  - **Creating** a folder is now an explicit choice — pick `<new folder...>` at
    the top of the folder list, type the name, press *New folder*. Previously a
    name typed while something else was selected could create a folder nobody
    asked for, including one whose name was no longer visible anywhere.
  - The **root list** shows every session, so each one that lives in a folder is
    marked with it in brackets. The root's own label can be renamed too — it is
    only a label, no session is moved or changed by it, and typing the built-in
    name back restores it.
  - Fixed: saving a session no longer re-files it based on which folder happened
    to be **viewed**. Loading a session now follows it into its folder, and only
    a folder that is deliberately changed is applied — so moving a session
    between folders still works, but can no longer happen by accident.

- **Application-wide settings are now grouped** in the configuration window:
  *Check for updates* and *show / edit / delete old putty/kitty sessions* sit in
  their own **Application** box instead of among the settings of the session
  being edited.

- **Documented:** the `-restrict-acl` command-line switch, which starts KiTTY
  with a restricted process access control list so that other programs running
  as the same user cannot open or tamper with the process, is now described in
  the feature documentation. The switch itself is long-standing; 0.84.1.61-beta
  fixed it being applied unconditionally to sessions started from the
  configuration window.

## 0.84.1.61-beta — 2026-07-23

- **Sessions started from the configuration box no longer run with an
  unintended restricted process ACL.** Since the 0.84 port began, every session
  window spawned from the configuration box (Start, Enter, Duplicate Session,
  "open new with current settings" — including every password-authenticated
  session) was unintentionally launched in the `-restrict-acl` hardening mode,
  regardless of how KiTTY was started. That mode locks the process down so
  tightly that the Windows **Restart Manager cannot inspect it**: during an
  in-place upgrade it reports *"a critical application holds files in use — a
  reboot will be necessary"*, shows no files-in-use dialog, and closes nothing
  gracefully — which is why windows never restarted after an upgrade, and the
  root cause behind the 0.84.1.58 upgrade rollbacks. It can also interfere with
  accessibility and automation tools. The restricted ACL now applies only when
  explicitly requested via `-restrict-acl`. The improved upgrade behaviour
  takes effect for upgrades **from** this version to a future one: the
  installer's files-in-use handling can then offer to close and restart the
  open KiTTY windows.

## 0.84.1.60-beta — 2026-07-22

- **Session picking in the configuration box is predictable now — the buttons act
  on what you see.** The **Start** button starts the session you just selected in
  the list with a single click; previously it silently started the configuration
  loaded earlier (usually the auto-loaded last session) while the name box
  already showed your selection (hknet/KiTTY#18). While the live search is
  filtering, **Open** and **Start** both act on the highlighted match. The two
  buttons also differ in where the session opens now: **clicking Open** opens it
  in the current window — classic behaviour, the configuration box closes —
  while **Start**, like pressing **Enter**, starts it in a new window and keeps
  the box open for launching the next one. Loading a session, tweaking its
  settings and test-driving them with Start works unchanged.
- **Ctrl+F jumps to the session search from anywhere in the configuration
  window.** From any settings panel — or right after starting a session with
  Enter — Ctrl+F returns to the Session panel with the search field focused and
  its content selected, so just typing starts a new search. No more clicking
  back to the Session panel and tabbing to the field.
- **Tray icons are easier to hit.** kageant's tray icon now opens its menu on a
  plain left click too (previously right-click or double-click only), and
  double-clicking the launcher's tray icon opens a new KiTTY configuration
  window.

## 0.84.1.59-beta — 2026-07-22

- **In-place upgrades are reliable again — they no longer stall or roll back when
  KiTTY windows are open.** Installing a newer version over a running KiTTY could
  silently fail and leave the old one in place: the Windows Restart Manager
  reported *"a critical application holds files in use — a reboot will be
  necessary"* and rolled the whole upgrade back. It struck when a session started
  from the configuration box's **Start/Open** was running — which includes **every
  password-authenticated session**, plus *Duplicate Session* and *open new with
  current settings* — because those sessions were launched by an internal path
  that never registered with the Restart Manager, so it refused to shut them down.
  Two changes fix it: the installer now **closes any running KiTTY before it
  upgrades**, so an in-place upgrade always completes no matter what is open; and
  those config-box / password sessions now register with the Restart Manager like
  every other window. This also gives a clean way off the affected 0.84.1.57 and
  0.84.1.58 — install 0.84.1.59 once and updates work normally from then on. Note
  that the installer now closes your open sessions during an upgrade; reconnect
  afterwards. **0.84.1.58-beta was withdrawn shortly after release because of this
  upgrade bug; 0.84.1.59 supersedes it** (it also carries 0.84.1.58's Ctrl+arrow
  fix, below).
- **Ctrl+arrow word navigation (and other modified arrow keys) work again inside
  full-screen terminal apps such as Midnight Commander.** In the default
  *xterm-style bitmap* arrow mode, a modified cursor key — Ctrl+Left/Right for
  word jumping, Shift+arrow for selection, Alt+arrow — lost its modifier whenever
  the running program had switched the terminal into *application cursor keys*
  mode, which `mc`/`mcedit` and most ncurses apps do on startup. KiTTY sent a
  bare `ESC O x`, indistinguishable from an unmodified arrow, so `mcedit` never
  saw the Ctrl and word-jump did nothing — while the same keys kept working at
  the shell prompt, which does not enable application cursor mode. KiTTY now
  always sends the `CSI 1;<mod> x` form for a *modified* cursor key regardless of
  application-cursor mode — matching xterm and pre-0.84 KiTTY — while unmodified
  arrows still honour the application/normal distinction. If Ctrl+arrow still does
  not jump words, check that **Terminal → Keyboard → "Shift/Ctrl/Alt with the
  arrow keys"** is set to **xterm-style bitmap** (the default), not "Ctrl toggles
  app mode." (hknet/KiTTY#16)

## 0.84.1.57-beta — 2026-07-21

- **Directory-aware file uploads (OSC 7 shell integration).** Turn on **Track
  remote directory (OSC 7 shell integration)** in **Connection → SSH → KSCP and
  WinSCP**, and drag-and-drop uploads — and *Start WinSCP* — land in your remote
  shell's **current working directory** instead of always in your home
  directory. KiTTY learns the directory from the standard **OSC 7** sequence
  (`ESC ] 7 ; file://host/path BEL`) that shells emit on every prompt; the path
  is validated as data only (absolute, strict character whitelist, %-decoded —
  anything with a space or a shell metacharacter is rejected and the upload
  falls back to your home directory) and is **never executed**. It is opt-in per
  session (off by default) and honoured live. Two lines in your `.bashrc` /
  `.zshrc` set it up — see
  [docs/examples/osc7-shell-integration.md](docs/examples/osc7-shell-integration.md).
  This replaces KiTTY's old **"Send file in current directory"** option, which
  drove uploads off the removed `__pw` title-scan mechanism (retired as the
  remote-code-execution hole CVE-2024-23749) and no longer captured anything: it
  is removed from the UI and the code, and sessions that had it enabled are
  migrated to OSC 7 tracking automatically. A **Fixed remote upload directory**
  field (mutually exclusive with tracking) covers the "always upload here" case.
- **The transfer window is high-DPI aware and easier to live with.** The
  progress/output window shown during a drag-drop or WinSCP transfer now scales
  its font and layout to the display DPI (it used to render tiny on high-DPI
  screens), shows the exact upload target (`user@host:directory`) at the top,
  closes with **Esc**, and has a per-session option to **stay open after a
  successful transfer** instead of auto-closing. The configuration panel is now
  titled **KSCP and WinSCP** (KiTTY ships pscp as `kscp`), with inline help.
- **Terminal and configuration windows come back after an in-place MSI upgrade.**
  Previously an in-place upgrade relaunched only the tray apps (kageant, the
  launcher) while the terminal and the configuration window silently vanished.
  KiTTY now registers the running window with the Windows Restart Manager from
  the loaded session, so a saved session reconnects and the configuration window
  reopens after the upgrade, and it shuts down cleanly on the upgrade/logoff
  rather than being force-terminated.

## 0.84.1.56-beta — 2026-07-20

- **`/help` no longer blocks the box you type commands into.** The internal-
  command list (`/help` in the Ctrl+F8 send-text box) used to be a modal
  message box that had to be dismissed before you could type anything — so you
  could not read a command and use it at the same time. It now opens in a
  separate resizable window that stays open while you keep issuing commands:
  a second `/help` brings it to the front, Esc or Close dismisses it, the text
  can be selected and copied (Ctrl+A, Ctrl+C), and its position is remembered
  like the other pop-up windows.
- **The Ctrl+F8 send-text box is now modeless.** It no longer freezes the
  terminal window while open, so you can scroll or click the terminal with the
  box up, and it coexists with the `/help` window (Esc works in either).
  Sending a line still clears the box and keeps it open for the next one, as
  before; Esc, Close or the X dismiss it. The Shift+F8 multiline box and the
  password prompt are unchanged.

## 0.84.1.55-beta — 2026-07-19

- **Missing-glyph font fallback (ported from upstream PR cyd01/KiTTY#555, by
  blreay — thanks!).** When the terminal font is missing a character — Nerd
  Font / Powerline icons, box drawing, CJK, symbols — KiTTY now probes a list
  of fallback fonts and draws that character from the first font that has it.
  Cell widths are unchanged; only the glyph's source font differs. This covers
  what Windows' own font linking cannot, notably private-use-area icons.
  Configured in a new kitty.ini **`[FontFallback]`** section (see
  [docs/KITTY-INI.md](docs/KITTY-INI.md)): `active` master switch (default
  yes), `fallback=` comma-separated font list tried before the built-in
  defaults (leading `!` replaces them), `override=U+range:Font` pinning, and
  `log`/`logfile` for troubleshooting. The built-in fallback list leads with
  Cascadia Mono/Code and the Segoe UI symbol fonts, then the major CJK UI
  fonts. Rendering is plain GDI, so emoji drawn via fallback come out
  monochrome; with a raster primary font (Terminal/Fixedsys) the feature
  disables itself. The port also fixes two issues in the original module: a
  raster primary font no longer sends *all* text to the first fallback font,
  and very long mixed-font lines are no longer truncated at 64 runs.
- **Internals:** the configuration self-test (`test_conf`) now expects
  KiTTY's own defaults for `ProxyLogToTerm` and `ShiftedArrowKeys` — the two
  remaining failures dating from the 0.84 baseline import — so the suite runs
  green and serves as a regression gate for future upstream rebases.

## 0.84.1.54-beta — 2026-07-19

- **Ctrl-Tab and Ctrl-Shift-Tab reach the host again (hknet/KiTTY#15).** Tab
  with Ctrl held produces no character message on Windows, so the keystroke
  was silently swallowed — classic KiTTY mapped the pair to the xterm
  sequences `ESC[27;5;9~` / `ESC[27;6;9~`, and that mapping had not been
  carried over into the 0.84 port. It is restored with exactly the classic
  sequences, so tmux/vim bindings that worked with 0.76 work unchanged. The
  mapping is skipped in putty-compatibility mode, and KiTTY's own Ctrl-Tab
  window-switching option, when enabled, still takes precedence.
- **Diagnostic dumps: inline script content is now redacted everywhere.**
  The visible `/savedump` text has redacted the stored inline login/RuTTY
  script since 0.84.1.37, but the embedded `current.ktx` copy of the session
  still carried it. The embedded copy now redacts it like the other secret
  fields, and the long-standing "script-content path under review" caveat is
  retired from KNOWN-ISSUES.
- **A settings-file guide: [docs/KITTY-INI.md](docs/KITTY-INI.md).** One page
  explaining kitty.ini — how KiTTY finds the file, the `savemode` values and
  the portable-layout rule, and what each section configures — with the fully
  annotated `kitty.ini.example` as the complete key reference. The release
  pipeline cross-checks the guide against the example, so it cannot drift.
- **README refresh:** the download notes describe the two ZIP flavours
  correctly, the stored-passwords summary reflects the portable master
  password (shipped in 0.84.1.48), the feature digest gained the command
  console, named proxies/jump hosts, the security set and the kageant
  capabilities, and the credits link the original PuTTY author's GitHub page.

## 0.84.1.53-beta — 2026-07-18

- **kageant honours kitty.ini (hknet/KiTTY#14).** The SSH agent now reads an
  `[Agent]` section from kitty.ini: `askconfirmation` with the classic three
  states — `yes` (confirm every key use), `auto` (only keys whose comment
  contains the word `confirmation`; the default) and `no` (never ask, for
  automation) — and `messageonkeyusage=yes/no` for the key-used tray balloon.
  When kitty.ini says `savemode=file` or `dir` — or, with no savemode line,
  when a portable layout (a `Sessions` folder or `KiTTYState` file) sits
  beside the ini — the ini is the **authoritative store**: the tray toggles
  write back to it and a portable kageant never touches the registry. The
  key-list window shows a *kitty.ini mode* status line and new three-state
  *Confirm key use* radio buttons (they collapse in registry mode, where
  behaviour is unchanged).
- **The startup-key list travels.** With kitty.ini authoritative, the
  remembered keys live in the ini (`startupkey1=…`, with an `,encrypted`
  marker) instead of the registry: keys inside the install folder are stored
  as relative paths (surviving a drive-letter change), a key added from
  elsewhere asks whether to copy it into the portable folder or reference it
  in place, a key missing at login produces one tray notice and is skipped —
  not dropped from the list — and gaps in the numbering are tolerated.
- **Registry-free autostart.** A portable kageant installs its start-at-login
  entry as a **Startup-folder shortcut** (no registry write); an
  installed/registry-mode kageant keeps the classic `HKCU\…\Run` entry, and
  the tray launcher has the same one-click *Start at login* toggle. Enabling
  autostart now checks for conflicts *before* creating anything — another
  agent already autostarting from elsewhere, or a same-named entry from
  another install — and asks first. Both programs identify their own entries
  strictly by target path, so toggling autostart in one install can no longer
  remove another install's entry. (Prompt wording corrected along the way:
  Windows *Settings → Apps → Startup* disables an autostart entry, it does
  not delete it.)
- **Tray-menu polish:** the launcher's *Quit* entries are named **Exit**
  everywhere, the Opened-sessions submenu no longer duplicates About/Exit,
  kageant's tray checkmarks re-sync from the live settings each time the menu
  opens, and the portable launcher's tooltip identifies itself with a second
  line "(portable)".
- **Send-text commands: a real reference, and /help that cannot drift.** The
  internal command dispatch is table-driven now and `/help` is generated from
  the same table, so the in-app list can no longer miss a command (the old
  hand-written text had drifted: seven live commands were absent). A
  long-standing `/zmodem` quirk is fixed — it toggled its flag but also sent
  the literal text `/zmodem` on to the host. All 46 commands are documented
  in [docs/COMMANDS.md](docs/COMMANDS.md), linked from FEATURES.md, and the
  release pipeline cross-checks the command table against that document so
  future commands arrive documented.
- **Upstream fix:** plugged a memory leak in the console tools' weak-hostkey
  confirmation prompt (cherry-picked from PuTTY upstream).

## 0.84.1.52-beta — 2026-07-17

- **Invert colours actually inverts now — and no longer crashes.** *Window →
  Invert colours* could abort the whole session with an assertion failure (a
  colour-count left over from classic KiTTY's 34-colour palette; this port has
  25), and even when it ran, pure black stayed black so a dark scheme merely got
  darker. It is now a true photographic negative across all colours and returns
  to the original scheme when used twice. The same colour-count fix applies to
  `.ktx` session files: loading no longer plants stray colour entries, and
  saving no longer silently drops the underline/selection colours.
- **The Event Log is resizable.** Drag any edge or maximize it; it also opens
  noticeably larger, the log list grows with the window, **Ctrl+A** selects
  every line and **Ctrl+C** copies (with nothing selected, Copy still grabs the
  whole log).
- **Esc closes the About box again** (lost when the box became non-modal), and
  Tab cycles its buttons — from a terminal window or from the configuration box.
- **The "up to date" reply of *Check for updates* is visible now:** the title
  bar turns green for five seconds with the notice text (Windows 11; older
  Windows just shows the text as before).
- **System menu cleanup:** *New Session* is removed (starting kitty.exe gives
  the same box); *New duplicated session* is renamed **Inherit New Session** —
  it opens a configuration box pre-loaded with the current window's settings and
  an empty hostname, for "connect somewhere else with exactly this setup";
  *Always visible* is renamed **Always On Top**.
- **kitty.ini housekeeping.** Sixteen-plus documented-but-dead kitty.ini keys
  were retired (the list is in FEATURES.md under *retired kitty.ini settings*),
  and four wanted ones were revived as working features: `noexit` (closing a
  window that ran a connected session reopens the configuration box),
  `scriptmode` (master off-switch for RuTTY scripting), `size` (live
  `[rows x cols]` window-title suffix) and `wintitle` (title decorations,
  including the `(PROTECTED)`/`(ONTOP)` markers — strictly display-only; the
  old title-parsing remote-command channel stays removed). New:
  `[ConfigBox] dblclick=start` makes double-clicking a saved session act like
  the Start button (new window, box stays open).
- **RuTTY scripting is documented:** FEATURES.md explains the script-file
  format (line-by-line send, `::` comments, wait-for/halt-on, per-line `:`
  conditions) and `docs/examples/logon-script.ksh` ships as a commented starter.
- **The send-text box got discoverable:** typing **`/help`** in the one-line box
  (Ctrl+F8) lists all KiTTY internal commands, and the box title says so.
  `/size` now re-enables the title decorations when they were switched off with
  `/wintitle` (it looked dead before). Note that `/size` and `/wintitle` are
  app-global *runtime* toggles: they are not part of the per-session settings
  `/save` stores — make them permanent with `size=yes` / `wintitle=no` in the
  kitty.ini `[KiTTY]` section. **Ctrl+Shift+F8** always opens the multiline box
  as an alias next to Shift+F8.
- **Save your runtime tweaks as a session:** `/save` now writes the window's
  live settings back to its saved session, and the new **`/savenew <name>`**
  saves them as a new session and switches the window's identity to it (so
  later `/save` calls and save-on-exit land there). The classic `/save`
  behaviour — exporting a `.ktx` connection file — moved to `/savektx`.
- **Two ZIP flavours instead of one mixed archive (hknet/KiTTY#13).** Some antivirus
  engines block any download containing UPX-packed executables, which made the
  old ZIP (UPX-packed `kitty.exe` next to `_nocompress` twins) undownloadable
  for affected users. `kitty-<version>.zip` now contains only plain,
  uncompressed signed executables (recommended), and the new
  `kitty-<version>-upx.zip` carries the UPX-packed `kitty.exe` /
  `kitty_portable.exe` for the smallest download. The `_nocompress` duplicates
  inside the archive are gone.
- **Large internal restructuring, no intended behaviour change:** the KiTTY
  additions were carved out of the biggest source files into focused modules
  (kitty.c shrank from ~6100 to ~3900 lines; storage.c and pageant.c were
  split; the configuration dialog is built one panel per function; kitty.ini
  parsing is table-driven). Every code move was verified byte-identical, the
  configuration box was verified panel-by-panel, and a new storage round-trip
  regression test covers the settings/crypto layer.

## 0.84.1.51-beta — 2026-07-12

- **A named proxy can now be an SSH jump host.** The named-proxy editor gains three
  SSH types, so a bastion/jump host can be defined once and picked per session from
  the Proxy choice dropdown instead of being set up by hand each time: *SSH jump host
  (port forwarding)* opens a standard forwarded connection through the jump host (the
  equivalent of OpenSSH's `ProxyJump` / `ssh -J`, and the usual choice); *(execute a
  command)* and *(invoke a subsystem)* cover jump hosts where forwarding is disabled
  but you can run a program or start an SSH subsystem. With no password stored the
  jump connection authenticates like any SSH session, so keys in kageant are used
  automatically. FEATURES.md documents each type, where the jump host's settings come
  from, and how to chain multiple hops.
- **The named-proxy editor is easier to read**, with its fields grouped into
  Definition / Proxy-jump host / Options sections.
- **The SSH-auth agent checkbox now names kageant.** *Connection → SSH → Auth* reads
  "Attempt authentication using kageant (Pageant)" to match the tray agent KiTTY
  actually ships.

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
