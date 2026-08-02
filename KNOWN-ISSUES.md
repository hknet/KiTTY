# KiTTY 0.84.1.67 — Known issues & limitations

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
- **OSC 52 remote clipboard writes are on by default, text-only, and
  write-only.** *Remote clipboard writes (OSC 52)* (Window → Selection) defaults
  to **Allow**, matching every comparable terminal — the write direction changes
  what you paste next but cannot disclose anything to the host. Set it to
  **Deny** if you would rather no host touched your clipboard, or **Ask** to be
  prompted once per session. Only **text** travels this way; the sequence
  carries nothing else, so images and other clipboard formats are unaffected.
  As with far2l, answering **OK** to an **Ask** prompt grants access for the rest
  of that session rather than re-prompting per payload; changing any setting in
  the configuration box makes it ask again. A payload too large to fit, or one
  that is not valid base64, is refused entirely rather than pasted in part.
- **OSC 52 remote clipboard *reads* are off by default, and there is no way to
  switch them permanently on.** *Remote clipboard reads (OSC 52)* (Window →
  Selection) is the other direction: a host asking for the contents of your
  clipboard, which are then sent to it. It offers **Deny** (the default) and
  **Ask** — and deliberately no "Allow", because a clipboard holds a password
  often enough to matter and the host chooses the moment it asks. A read can be
  permitted only by answering the prompt, and only for as long as that answer
  says: one request, a number of minutes, a number of requests, or the rest of
  the session, the last of which asks a second time before it takes effect. No
  permission to read is ever written to disk. Every limit is a setting in the same
  panel.
- **No remote clipboard access at all while the window has no keyboard focus.**
  *Only allow clipboard access while this window has focus* (Window → Selection)
  defaults to on and covers reads **and writes**. An existing permission is
  suspended rather than cancelled — the title shows `(clip read paused)` — and
  resumes without asking again when you come back. Turn it off if you rely on a
  background job that copies its own output into your clipboard.
- **The lilac tint on the title bar and border needs Windows 11.** While a
  clipboard permission is live the window is marked; the *text* marker in the
  title works everywhere, but the colouring uses an API that exists only on
  Windows 11 build 22000 and newer and silently does nothing on Windows 10.
- **adb backend & rutty scripting:** functional and verified against test
  fixtures (a fake adb server / a scripted listener), but **not** yet validated
  against a real Android device or a live remote shell.
- **Background image:** renders correctly inside the terminal cell grid; the thin
  margin strip outside the grid is still solid-filled (cosmetic).
- **Font fallback renders monochrome.** Missing-glyph fallback draws with plain
  GDI, so emoji and other colour glyphs taken from a fallback font come out as
  monochrome outlines. Supplementary-plane emoji may additionally need an
  explicit `override=` range in `[FontFallback]`. With a raster (non-TrueType)
  primary font such as Terminal or Fixedsys, the fallback feature disables
  itself for that session. `active=no` turns it off entirely.
- **Command-line tools use the registry session store.** `klink`/`kscp`/`ksftp`
  (plink/pscp/psftp) read saved sessions from the Windows **registry**, not from a
  portable (`savemode=dir`) store — so a portable install's sessions, and any
  passwords a **master password** protects there, are usable from the KiTTY
  **GUI** but not from the command-line tools. Registry-mode sessions (with their
  DPAPI-protected passwords) work from the CLI tools as before.

## Connectivity tips

- **Slow first connect (~2–5 s) on non-Kerberos networks.** PuTTY (and thus KiTTY)
  attempts **GSSAPI** authentication by default — useful for Kerberos/Active-Directory
  single sign-on, but on a machine with no Kerberos realm it does DNS/KDC lookups that
  **time out** before falling back to your key/password, adding a few seconds before
  the session connects (you'll see a pause before *"No GSSAPI security context
  available"* in the Event Log). If you don't use Kerberos SSO, turn it off:
  **Connection → SSH → Auth → GSSAPI → untick "Attempt GSSAPI authentication"**
  (and "Attempt GSSAPI key exchange"), then save — set it in **Default Settings** to
  apply to new sessions. Connect time drops to ~1 s.

## Security

- **Diagnostic dumps have been removed.** `/savedump` and `kitty.exe -savedump`
  are gone as of **0.84.1.65**. The dump was written encrypted under a key
  compiled into the program, and KiTTY shipped no way to read one back, so a
  dump could not actually be used for support by you or by us. Any `kitty.dmp`
  left over from an earlier version is still readable only with that build's
  key; it is safe to delete. For troubleshooting use the **Event Log**
  (right-click the title bar → *Event Log*) and session logging
  (**Session → Logging**), and attach those to an issue instead.
- **Stored passwords are DPAPI-encrypted at rest.** KiTTY can *optionally*
  save a session password (PuTTY itself never stores one). As of **0.84.1.38**,
  new and re-saved passwords are protected with **Windows DPAPI** and stored as
  `DPAPI1:` blobs, tied to your Windows account/machine. This defeats offline
  and cross-user theft of the registry/session files, but **not** malware already
  running as the same Windows user, and DPAPI blobs do **not** move to another
  PC. Existing legacy/old-KiTTY passwords still load and are re-encrypted on the
  next save. In **portable mode** you can now protect saved session *and* proxy
  passwords with an opt-in **master password** (you are prompted on first save;
  `-masterpwfile` supplies it non-interactively). Unlike DPAPI, a master-password
  store **moves between machines**: from 0.84.1.65 it is kept in a `Security`
  folder inside the portable store itself, so carrying the folder carries the
  protection — earlier versions kept it in the registry of the PC it was set up
  on, and such an install is moved over automatically on the next start (several
  portable copies sharing one master password need that folder copied into
  each). Exporting sessions no longer creates a master password as a side
  effect: an exported bundle carries its own password, see FEATURES.md.
  **The master password is never stored and
  cannot be recovered: if you forget it, the passwords it protected are
  unrecoverable** — you would clear and re-enter them. Declining the prompt falls
  back to DPAPI, and `[KiTTY] PortablePasswordProtection=legacy` keeps the classic
  plaintext form for automation/audit. kageant can keep SSH-2 keys in an
  **encrypted/deferred** state when they are added with **Add key (encrypted)**,
  loaded at startup, or added with `-encrypted`/`-nodecrypt`; the passphrase is
  requested on first use. After first use, and also for normally added SSH-2
  keys, kageant stores the long-lived private key material as a Windows
  `CryptProtectMemory`-protected blob and only unprotects/deserializes it
  temporarily while signing. **If security matters, prefer public-key
  authentication (kageant) and avoid saving passwords unless you understand
  these limits.**

## Packaging / cosmetic

- **Antivirus & UPX:** the standard `kitty-<version>.zip` contains only plain,
  uncompressed signed executables (antivirus-friendly). The `-upx.zip` flavour
  and the installers carry UPX-compressed `kitty.exe`/`kitty_portable.exe` for
  the smallest download; UPX can trip heuristic AV/SmartScreen, so if your
  antivirus objects, take the standard ZIP.
- **Version string:** binaries report `0.84.1.67-beta @ 2026-08-01`.
- **Embedded in mRemoteNG — vertical-drag wobble:** when KiTTY is hosted inside a
  connection manager, dragging the pane's **height** can make the terminal wobble
  a few pixels while you drag. It's the host's own caption-offset compensation;
  it settles when you release. Cosmetic.

## New in 0.84.1.67

- **A window whose session has ended keeps that session's title**, with the
  state added at the end — `user@host: ~ (inactive)`, or
  `⚠ user@host: ~ (disconnected)` when the connection was lost. Ten dead windows
  can be told apart again (hknet/KiTTY#22).
- **Quick connect**: load "Default Settings" once and the configuration box
  opens on the defaults with the cursor in *Host Name*, until another session is
  loaded; `loadlastsession=no` in `[ConfigBox]` makes it permanent
  (hknet/KiTTY#23).
- **`ssh://` and `kitty://` links** are understood on the command line and from
  a browser, and a URL with no port now connects to the protocol's default port
  instead of failing.
- **`-sshhandler` / `-fileassoc` register without administrator rights** (for
  your account), never take a protocol or extension from another program without
  `-force`, can be undone with `-uninstall`, and ask first when run from a
  portable KiTTY.
- **`-help`** prints the command-line options, which have been audited against
  the real command line for the first time.
- **The Event Log** has a **Clear** button and no longer vanishes with its window
  when a device sends a late channel message on logout.

## New in 0.84.1.66

- **Two configuration-box crashes are fixed.** Changing a session name and then
  starting the session from a page other than *Session* ended KiTTY with an
  assertion failure; **Ctrl+G** from another page could do the same. Both gone.
- **The saved-session highlight no longer jumps to the first entry.** It hit
  session names that sort before "Default Settings" — anything starting with a
  digit, so IP addresses above all — when clicking *and* when typing
  (hknet/KiTTY#19).
- **kittygen: adding a certificate to a freshly generated key no longer ends the
  program**, and a generated key no longer stays in memory in the clear after
  the window is closed or another key is generated.
- **SSH certificates are documented** in
  [docs/SSH-CERTIFICATES.md](docs/SSH-CERTIFICATES.md) — attaching one to your
  key, the OpenSSH server side, host certificates, and a local lab to try it on.
- **Binaries no longer embed the build machine's directory names** in assertion
  messages.

## New in 0.84.1.65

- **The diagnostic dump is gone.** `/savedump` and `kitty.exe -savedump` no
  longer exist. They wrote `kitty.dmp` encrypted under a key compiled into the
  program, and KiTTY shipped no way to read one back — so the file bug reports
  asked for could not be opened by you or by us. Use the **Event Log**
  (right-click the title bar) and session logging (**Session → Logging**)
  instead. An old `kitty.dmp` is neither read nor updated any more; delete it.
- **Exports now carry their own password, and it is shown only once.** "Export
  all" asks whether the files should be protected by a password you choose
  (importable on any PC) or for this Windows account on this PC only. The
  password is displayed once when the export finishes, with a Copy button —
  there is no way to recover it afterwards, and without it the export cannot be
  imported. Previously exporting silently created a **master password for your
  own session store** as a side effect; it no longer touches your store at all.
- **A master password that protects nothing is retired at startup**, silently.
  If it still protects something, it is left alone. On a registry install the
  old values are archived rather than deleted, so a portable store that has not
  yet been opened under this version can still be migrated.
- **Portable installs keep master-password state in their own `Security`
  folder.** A portable install that relied on the registry is migrated once, at
  startup, and tells you which folder to copy if you keep several portable
  installs sharing one master password. Copies of that folder are what makes the
  same passwords work on another PC.
- **`Password\PLAIN:…` in a session file is a cleartext secret.** The new
  provisioning form is taken literally and re-protected on first save, but until
  it is imported the file holds the password in the clear — treat it like one
  and delete it afterwards.
- **`[KiTTY] PortablePasswordProtection=dpapi`** protects a portable install's
  passwords for this Windows account on this PC and never asks for a master
  password. Those passwords **do not travel**: copied to another PC or account
  they cannot be decrypted. `-masterpwfile` is refused in this mode.
- **"Send to tray on startup" waits until the session is connected.** The window
  stays visible for a host-key or password prompt and drops to the tray a moment
  after login, so a session that never connects never disappears. The checkbox
  is in **Window → Behaviour** (with **Maximize** and **Full screen on
  startup**, which had also gone missing); `-send-to-tray` works again on the
  command line.

## New in 0.84.1.64

- **`[KiTTY] restrictacl=yes` applies the restricted process ACL everywhere**,
  including kageant, without editing shortcut targets. Three limitations are
  inherent to the hardening rather than to this setting, and all three apply
  equally to the older `-restrict-acl` switch:
  - **It blocks accessibility software.** A restricted process cannot be
    inspected by other programs running as you, and screen readers and similar
    tools rely on exactly that. If you use one, do not enable this.
  - **In-place upgrades will no longer reopen your sessions.** Windows' Restart
    Manager cannot inspect a restricted process, so the installer closes your
    windows and does not restore them afterwards.
  - **There is no way back within a running process.** `restrictacl=no` does
    not lift a restriction — a process cannot un-restrict itself — so the
    setting only ever turns the hardening on. Remove it and restart to get
    unrestricted processes again.
  **kittygen does not read the setting**; pass it `-restrict-acl` if wanted.
- **Silent (`/qn`) installs close and reopen your windows**, like the
  interactive upgrade. An installation started by a management system under
  the machine account closes them **without** reopening, since there is no
  desktop to reopen onto; `MSIDISABLERMRESTART=1` forces that behaviour in
  any silent install.

## New in 0.84.1.63

- **The registry backup is written by Windows' own registry exporter**, not by
  KiTTY, so it no longer drops binary values (window positions and sizes) or
  turns multi-value entries such as kageant's startup key list into plain text —
  both came back missing or unusable after a restore before. **Limitation:** the
  file is now a standard UTF-16 `.reg`, which an **older KiTTY version cannot
  load itself** — it treats the file as unreadable. This only arises if you
  downgrade and then want a backup made by this version, and Windows restores it
  perfectly well without KiTTY's help: `reg import kittynew-YYYYMMDD-HHMMSS.sav`
  (no administrator rights needed), or open it in Registry Editor. That is the
  same standard format the file's new writer produces, which is precisely what
  makes importing it by hand possible. Backups written by earlier versions are
  still restored normally by this one.
- **Backups are taken *before* a destructive change** — overwriting a saved
  session, deleting a session, deleting a folder — instead of only after a
  change, so the newest copy still holds what was just lost. They are also **no
  longer written when you merely open a session**: a copy is written only when
  something was actually changed. Saving a session under a name that does not
  exist yet writes none, because nothing exists to preserve. `/savereg` still
  writes one on demand.
- **The configuration password is retired.** `/configpassword` and
  `/-configpassword` are gone and backups are no longer encrypted: the registry
  already protects saved passwords with DPAPI, while this mechanism kept its own
  key in plain text beside them and the `kitty.ini` copy was scrambled only with
  a value built into every KiTTY. **Limitation:** there is no longer any way to
  encrypt a backup file. Existing encrypted backups stay readable — KiTTY asks
  for the password when loading one.
- **Portable backups are complete, and are finally pruned.** The launcher
  configuration was missing from every portable backup although the
  documentation called it a complete copy, and the clean-up meant to keep the
  newest `portablebackupcount` folders never ran at all. **On the first backup
  after upgrading, accumulated folders are trimmed to that setting (5 by
  default)** — raise it, or copy them aside, if you want to keep more.
- **New registry backups are named `kittynew-*.sav`.** The intended name never
  actually took effect, so they were written as `kitty-*.sav` — the same name an
  older KiTTY installed alongside uses, which the name change existed to avoid.
  Existing `kitty-*.sav` files are left alone and are still read if a restore is
  needed.
- **Restoring a registry backup on another computer or user account** returns
  your sessions but not their saved passwords: DPAPI ties those to the account
  that saved them. This is long-standing, not new — it is simply documented now.
  Portable stores are unaffected: passwords there are protected with your master
  password and are made to travel with the store.

## New in 0.84.1.62

- **Inline (in-terminal) SSH security confirmations**, opt-in per prompt in
  `kitty.ini` (`modalnewhostkeyconfirmation`, `modalchangedhostkeyconfirmation`,
  `modalweakkeyconfirmation`; default `yes` keeps the classic dialog). A changed
  host key takes a deliberate two-step confirmation (`yes`, then `confirmed` to
  replace the stored key). **Limitation:** inline prompts are not available
  during a rekey of an already-authenticated session — the running program owns
  the terminal, so those confirmations abort the connection instead of asking.
- **Folder rename and safe delete** in the configuration box, both in the
  registry and in portable mode: deleting a folder that still holds sessions
  asks first and moves them to the root list, folder creation is an explicit
  `<new folder...>` choice, and saving a session no longer re-files it by
  whichever folder was being viewed.
- **Application box** groups *Check for updates* and the old putty/kitty session
  controls. Note that *Check for updates* is still stored **per session**, as
  before; the neutral box title does not imply it became a global setting.

## New in 0.84.1.61

- **Config-box-spawned sessions no longer run with an unintended restricted
  process ACL** (`-restrict-acl` hardening was always on for them since the
  port). This unblocks the Windows Restart Manager during in-place upgrades —
  effective for upgrades **from** this version onward; the upgrade **to** this
  version still behaves like before (windows are closed by the installer and
  do not restart).

## New in 0.84.1.60

- **Config-box buttons act on the visible selection.** Start launches the session
  you just single-click selected (hknet/KiTTY#18); while the search filter is
  active, Open and Start act on the highlighted match. Clicking **Open** opens
  the session in the current window (the box closes); **Start** and **Enter**
  start it in a new window and keep the box open.
- **Ctrl+F** from anywhere in the configuration window jumps to the Session
  panel with the saved-session search field focused and selected.
- **Tray usability:** kageant's menu opens on a plain left click too;
  double-clicking the launcher tray icon opens a new configuration window.

## New in 0.84.1.59

- **In-place upgrades no longer stall or roll back with KiTTY windows open.** An
  upgrade over a running KiTTY could fail silently ("a critical application holds
  files in use — a reboot will be necessary") and leave the old version, when a
  session opened from the config box's Start/Open — including any
  password-authenticated session — was running. The installer now closes any
  running KiTTY before upgrading, and those sessions register with the Restart
  Manager, so upgrades always complete. Installing 0.84.1.59 once is the clean way
  off an affected 0.84.1.57. (The installer closes your open sessions during an
  upgrade; reconnect afterwards.)
- **Modified arrow keys work inside application-cursor apps.** Ctrl+arrow word
  navigation, Shift+arrow selection and Alt+arrow now keep their modifier inside
  full-screen programs such as Midnight Commander (`mc`/`mcedit`) that switch the
  terminal into application cursor-keys mode; previously the modifier was dropped
  there in the default *xterm-style bitmap* arrow mode, so e.g. Ctrl+Left/Right
  stopped jumping words. (hknet/KiTTY#16)

## New in 0.84.1.56

- **`/help` is a separate resizable window**, not a modal box — it stays open
  while you keep typing commands into the Ctrl+F8 send-text box; Esc/Close
  dismiss it, Ctrl+A/Ctrl+C copy the list, position remembered.
- **The Ctrl+F8 send-text box is modeless** — it no longer freezes the
  terminal while open and coexists with the `/help` window. Sending still
  keeps the box open for the next line. The Shift+F8 multiline box and the
  password prompt stay modal.

## New in 0.84.1.55

- **Missing-glyph font fallback** (ported from upstream PR cyd01/KiTTY#555 by
  blreay): characters your terminal font lacks — Nerd Font icons, CJK, box
  drawing, symbols — are drawn from the first fallback font that has them.
  New kitty.ini `[FontFallback]` section: `active` (default yes), `fallback=`
  font list, `override=` Unicode-range pinning, `log`/`logfile`. See the
  monochrome-rendering limitation above.
- **`test_conf` runs green:** the configuration self-test now expects KiTTY's
  defaults for `ProxyLogToTerm` and `ShiftedArrowKeys` (failures dating from
  the 0.84 baseline import).

## New in 0.84.1.54

- **Ctrl-Tab / Ctrl-Shift-Tab are sent to the host** as the classic xterm
  sequences `ESC[27;5;9~` / `ESC[27;6;9~` (hknet/KiTTY#15) — tmux/vim
  bindings from classic KiTTY work again. Skipped in putty-compatibility
  mode; the Ctrl-Tab window-switching option takes precedence when enabled.
- **`/savedump`: the stored inline login/RuTTY script is now redacted inside
  the embedded `current.ktx` too** — the last known script-content gap; the
  "under review" caveat above is gone.
- **New [docs/KITTY-INI.md](docs/KITTY-INI.md)** settings-file guide
  (resolution order, `savemode`/portable rules, section overview), plus a
  README refresh (two ZIP flavours, portable master password, feature
  digest, credits).

## New in 0.84.1.53

- **kageant honours kitty.ini `[Agent]` (hknet/KiTTY#14):** `askconfirmation`
  (`yes` / `auto` / `no`) and `messageonkeyusage`, with matching three-state
  *Confirm key use* radios in the key-list window. With `savemode=file`/`dir`
  — or a portable layout detected beside the ini — kitty.ini is the
  authoritative store and a portable kageant never touches the registry; the
  UI shows *kitty.ini mode*.
- **Portable startup-key list:** remembered keys live in kitty.ini
  (`startupkey1=…`, paths relative to the install folder, `,encrypted`
  marker); keys added from outside offer copy-or-reference; a key missing at
  login is skipped with a tray notice, not dropped; numbering gaps are
  tolerated.
- **Registry-free autostart:** portable kageant and the tray launcher use a
  Startup-folder shortcut instead of `HKCU\…\Run`; the conflict check runs
  *before* an entry is created (and asks), and entries are identified by
  target path so one install can no longer remove another install's entry.
- **Tray polish:** *Quit* → *Exit*, no duplicated About/Exit in the
  Opened-sessions submenu, checkmarks re-sync when the menu opens, and the
  portable launcher tooltip adds a "(portable)" line.
- **Send-text commands:** table-driven dispatch with `/help` generated from
  the table (seven live commands were missing from the old text); `/zmodem`
  no longer also sends its literal text to the host; all 46 commands are
  documented in `docs/COMMANDS.md`.
- **Upstream fix:** memory leak in the console tools' weak-hostkey
  confirmation prompt.

## New in 0.84.1.52

- **Invert colours fixed:** no longer crashes (a classic-KiTTY 34-colour count
  vs. this port's 25), and is a true negative now — black actually flips to
  white. The `.ktx` colour handling got the same count fix (saving no longer
  drops the underline/selection colours).
- **Event Log:** resizable + maximizable, opens larger, Ctrl+A selects all,
  Ctrl+C copies; Copy with nothing selected still copies the whole log.
- **Esc closes the About box** again; Tab cycles its buttons.
- **Check for updates:** the "up to date" notice tints the title bar green for
  5 s (Windows 11).
- **System menu:** *New Session* removed, *Inherit New Session* (config box
  pre-loaded with this window's settings, empty hostname) replaces *New
  duplicated session*, *Always On Top* replaces *Always visible*.
- **kitty.ini:** 16+ dead keys retired (list in FEATURES.md); `noexit`,
  `scriptmode`, `size`, `wintitle` revived as working; new
  `[ConfigBox] dblclick=start`. RuTTY script-file format documented with a
  shipped example (`docs/examples/logon-script.ksh`).
- **Send-text box:** `/help` (in the Ctrl+F8 box) lists the internal commands;
  `/size` re-enables title decorations after `/wintitle`-off; Ctrl+Shift+F8 is
  a fixed alias for the multiline box. `/save` saves the live settings to this
  window's session, `/savenew <name>` saves them as a new session and switches
  to it; the old `.ktx` exporter is now `/savektx`. Limitation: `/size` and
  `/wintitle` are app-global runtime toggles, not per-session settings, so
  `/save` does not store them — persist them via kitty.ini (`[KiTTY] size=yes`
  / `wintitle=no`). Making the title decorations per-session is on the list
  for a future release.
- **Two ZIP flavours (hknet/KiTTY#13):** `kitty-<version>.zip` = uncompressed signed
  executables only (recommended; antivirus-friendly); `kitty-<version>-upx.zip`
  = UPX-packed kitty.exe/kitty_portable.exe for the smallest download. No more
  `_nocompress` duplicates inside the archive.
- **Internals:** major source restructuring (verified byte-identical moves +
  panel-by-panel config-box check); no intended behaviour change — if you use
  an exotic kitty.ini and something stopped reacting, check the retired-keys
  list in FEATURES.md first. **If a retired key mattered to you, open an
  issue** — where feasible we will restore the wiring, as already done for
  `noexit`, `scriptmode`, `size`, `wintitle` and `dblclick`.

## New in 0.84.1.51

- **SSH jump hosts via named proxies.** A named proxy can be an SSH jump host
  (port forwarding, execute-a-command, or invoke-a-subsystem); define a bastion
  once and pick it per session. Empty password ⇒ the jump authenticates with
  your kageant keys. See FEATURES.md for the config-source and multi-hop notes.
- The named-proxy editor is grouped into Definition / Proxy-jump host / Options.
- The *Connection → SSH → Auth* agent checkbox now reads "…using kageant
  (Pageant)".

**Known limitation (to be reworked):** a named proxy stores no authentication
settings of its own — the jump hop's key/agent choice comes from your Default
Settings, or from a saved session if the proxy Host field is named like one. A
future release will let the proxy editor own its auth settings directly.

## New in 0.84.1.50

- **Send-text input boxes restored.** Ctrl+F8 opens a one-line box, Shift+F8 a
  resizable multiline box pre-filled from the clipboard; text is composed
  locally and sent only when you confirm (OK / Shift+Return; a selection sends
  only the selected part). Their dialog resources were missing from this port,
  so the shortcuts previously did nothing.
- **kageant: "Ask confirmation before key use"** tray toggle (default off) —
  every signing request pops an allow/deny prompt naming the key. The per-key
  variant (key comment containing `confirmation`) works as before.
- **Paste size guard:** `pastesize=<N>` in `[KiTTY]` asks before pasting more
  than N characters (0 = unlimited, the default).
- **`initdelay` honored** for the first auto-command/auto-password send;
  **`[ConfigBox] filter=no`** disables the live session-list search; the
  **`keyexchange`** shortcut reliably triggers an SSH rekey.

## New in 0.84.1.49

- **Press a key to reconnect a finished session.** With *Close window on exit* set
  to *Never* (or after a dropped connection), pressing Enter — or any ordinary
  typing key — in the finished terminal restarts the session in the same window,
  as classic KiTTY did (hknet/KiTTY#12). Ctrl/Alt chords, Tab, arrows and F-keys
  are ignored (so Ctrl+D still closes the dead window); a session that never
  authenticated is not re-dialed; `autoreconnect=no` in kitty.ini disables it.
  Unlike classic KiTTY, a mouse click does **not** reconnect, so selecting and
  copying scrollback text from a finished session still works.

## New in 0.84.1.48

- **Named proxies are back, with a built-in editor and encrypted passwords.**
  KiTTY's classic *Proxy choice* is restored: define reusable named proxies, pick
  one per session from the Session panel, and create/edit/delete them (with the
  full set of proxy settings) from a built-in editor. Each proxy password is
  encrypted at rest like a session password, and proxies from a classic-KiTTY
  (9bis) registry hive migrate across automatically. See FEATURES.md.
- **Master password for saved passwords in portable mode.** A portable install can
  protect its session and proxy passwords with an opt-in master password, so the
  store is safe to carry between machines (the registry continues to use Windows
  DPAPI). See the Security note above — it is unrecoverable if lost.
- **Enter the master password once per running KiTTY.** In portable mode, once you
  unlock the master password it is shared with the session windows KiTTY opens next
  — from the config box, *New Session*, *Duplicate Session*, or the tray launcher —
  so you are not asked again for each window. The key is handed only to KiTTY's own
  child processes through an inherited handle, wrapped in memory with Windows
  CryptProtectMemory (same-logon); saved files stay master-password-encrypted at
  rest. It is only ever requested when a master password is actually configured.
- **Export and import your whole set of sessions.** New *Export all sessions…* /
  *Import sessions…* menu entries, and the `-exportall` / `-importdir` flags, move
  every saved session — each password re-wrapped for the destination machine — as
  a bundle of files.
- **Proxy connections show their handshake by default,** printing proxy
  diagnostics in the terminal until the session starts, so a failed proxied
  connection is diagnosable instead of a bare error.
- **Config box: a resizable session list and export/import buttons.** The Session
  panel gained *Export all* / *Import* buttons, and the saved-session list and the
  window can be resized via `[ConfigBox] height` / `windowheight`.
- **Update-available popup rebuilt as a standard dialog.** The non-modal update
  popup (new in 0.84.1.46) is now a real dialog, so it uses the system font at the
  correct DPI — a hugely oversized font on high-DPI displays is fixed — sizes
  itself to its wrapped text, and when an update is available it stays open until
  you pick *Update now* or *Later*. The "you're up to date" notice still
  self-dismisses.
- **Config box: `defaultsettings = no` hides "Default Settings".** The
  `[ConfigBox] defaultsettings = no` flag previously only skipped auto-creating the
  pseudo-session; it now also removes it from the saved-session list (it still
  works as the new-session template, loaded by name). The saved-session list also
  defaults to 16 rows.
- **Registry backups are timestamped and renamed `kittynew.sav`.** The registry
  backup is renamed from `kitty.sav` to `kittynew.sav`, so it never clashes with an
  older KiTTY's file, and each save now writes a fresh
  `kittynew-YYYYMMDD-HHMMSS.sav` whose filename reflects when it was written,
  keeping the newest `[KiTTY] savbackupcount` copies.
- **Whole-store import is more robust and interactive.** Importing a session bundle
  no longer crashes on a partial `.ktx`. *Export all* and *Import all* now use a
  modern folder chooser with an address bar (paste a path), *Import all* lets you
  overwrite existing sessions and proxies or import only the new ones (and reports
  the counts), and Export no longer includes "Default Settings" or an empty proxies
  folder.
- **Config box: Category tree fully expanded, plus a selection fix.** The Category
  tree opens fully expanded (`[ConfigBox] categoryexpand` sets the depth), and
  selecting a saved session with "Default Settings" hidden no longer jumps the
  highlight to the next row.

## New in 0.84.1.47

- **Clean-logout error suppression narrowed (hardening).** 0.84.1.45 taught KiTTY
  to treat a connection error arriving after the session's exit status as a clean
  end — but it did so for the entire remaining life of the connection. The quiet
  path now applies only when nothing else is using the connection (no
  port-forwarding, X11 or agent channels, no connection-sharing downstreams):
  exactly the state in which KiTTY was already about to close the connection of
  its own accord. A network failure while forwardings are live is a visible error
  again, as it was before 0.84.1.45. Ordinary logouts, including the Cisco case
  the original fix targeted, are unchanged. The same narrowing has been applied
  to the version of this change submitted upstream to PuTTY.
- **Config dialog keeps "Default Settings" selected.** Loading Default Settings
  no longer clears the session-name box, and the saved-sessions list no longer
  jumps back to the previously loaded session after a visit to another settings
  panel — the highlight follows the session actually loaded. The dialog still
  pre-selects your last-used session when it opens.

## New in 0.84.1.46

- **Ctrl + ←/→ word navigation restored as the default.** The arrow-key modifier
  encoding again defaults to the xterm-style bitmap (KiTTY's historical default),
  so Ctrl+Left/Right jump words on fresh and hive-migrated sessions. A session that
  had persisted the wrong value is corrected automatically on first run, but only
  where it can be proven to be the erroneous default and never a setting you chose
  yourself — no registry or PowerShell script is needed.
- **"Check for updates" is non-modal and quiet.** The result no longer opens a
  focus-stealing, sound-playing modal box: already-current shows a brief title-bar
  notice (terminal) or a self-dismissing box (config dialog), and an available
  update shows an auto-dismissing popup over the active window. Only genuine errors
  stay modal and keep the alert sound. Renders correctly on high-DPI displays.
- **About boxes are non-modal and DPI / multi-monitor aware.** In KiTTY, kageant
  and kittygen the About window opens by the window it was launched from (or the
  notification area for the tray tools), no longer blocks what is behind it, and
  remembers its position per monitor layout. Portable mode places these windows
  correctly but stores nothing in the registry.

## New in 0.84.1.45

- **Spurious auto-reconnect on clean logout fixed.** A device that sends its exit
  status and closes the channel, then drops the TCP connection in the same burst
  (e.g. Cisco IOS SSH), is no longer misread as an *unexpected* drop that triggers
  auto-reconnect — a clean `exit`/logout now stays closed. Root-cause fix in the
  SSH layer (also submitted upstream to PuTTY); it hardens and completes the
  0.84.1.43 logout-reconnect fix for the abrupt-close case. Genuine mid-session
  network drops still reconnect as before.
- **Hyperlink-underline flicker fixed.** With URL-hyperlink underline enabled,
  busy/continuously-updating output (e.g. a switch's `show` output) no longer
  flickers: only the rows whose underline state changed are repainted, instead of
  the whole window on every frame. Underlines still render, update, and stay
  clickable.

## New in 0.84.1.44

- **kageant private-key memory protection:** SSH-2 private keys no longer remain
  as long-lived decrypted `ssh_key` objects after loading or first use. kageant
  stores them as Windows `CryptProtectMemory`-protected private blobs, temporarily
  unprotects/deserializes only for signing, then frees the temporary key again.
  Encrypted/deferred startup keys, normal key loading, key order, re-encrypt, and
  Windows/OpenSSH agent client use were smoke-tested with multiple keys.

## New in 0.84.1.43

- **Launcher update visibility improved:** when `kitty.exe -launcher` detects a
  newer KiTTY release, the update is no longer only a transient Windows tray
  balloon; the tray tooltip also mentions the available version and the launcher
  menu shows a disabled `Update available: KiTTY ...` line until you upgrade.
- **Saved-session search/filter polish:** typing in the Saved Sessions field
  narrows the visible list within the active folder filter, ranks prefix/token
  matches before substring matches, and shows folder names in brackets while
  searching. Focus starts in that field; Up/Down moves into the filtered list;
  Enter loads or starts the highlighted visible session instead of a hidden
  previous selection.
- **Session comment display fixed:** empty comments in the primary KiTTY hive no
  longer get overwritten in the config dialog by stale comments from older
  fallback registry hives with the same session name.
- **Normal logout no longer auto-reconnects:** a clean remote logout/`exit` that
  has already closed the session is no longer treated as a reconnect-worthy
  connection failure.

## New in 0.84.1.42

- **Savedump crash fixed:** `kitty.exe -savedump` now has a valid configuration context and works in registry and portable directory modes.
- **Updater hardening:** MSI update downloads use unique temporary files, avoid clobbering pre-existing paths, and are deleted if launch fails or is cancelled.
- **Portable mode is more registry-free:** SSH host keys/CAs, random seed, jump-list state, last-session state, and small KiTTY state/cache files now live under the portable config directory in `kitty_portable.exe` / `savemode=dir`.
- **Session folders polished:** editable folder selector, clearer root label, reliable folder filtering, remembered folder/session, a larger saved-session list, and aligned action buttons.
- **Folder UI assertions fixed:** editable combo boxes now work with the list APIs used by the configuration dialog.

## New in 0.84.1.41

- **Launcher global hotkeys:** saved sessions can define launcher-only global hotkeys under **Window → Behaviour**; saving a session notifies a running launcher to refresh and re-register them.
- **Hyperlink polish:** improved URL underline repainting, optional hand cursor on hover, and more reliable browser foregrounding.
- **mNotepad polish:** high-DPI font, restored menus/resources, direct terminal Tools menu entries, and fixed Shift+F2 / Ctrl+Shift+F2 clipboard launch.
- **Portable backups:** portable directory mode keeps `Backups\kitty-portable-latest` plus timestamped backups with configurable retention.
- **Paste menu restored:** terminal system and right-click menus include **Paste** again for Windows mouse-button mode.
- **Packaging/docs:** releases include an inert `kitty.ini.example`; kageant/KiTTYgen branding and test-build About labels were polished.

## New in 0.84.1.40

- **Window title placeholders.** The **Window Title** setting supports dynamic
  placeholders such as `%%h` (host), `%%s` (session), `%%u` (user), `%%p` (port),
  `%%P` (protocol), `%%f` (folder), and forwarded-port summaries. Contributed by
  m-hume.
- **WinSCP polish:** KiTTY now detects common 32-bit/per-user/PATH WinSCP installs,
  re-probes stale paths, and the config dialog uses a file picker for the WinSCP
  executable.
- **Launcher Refresh keeps you in context:** the tray menu is rebuilt and reopened
  at the original menu position after Refresh.
- **Cleanup / migration hardening:** stale historical `MOD_*` guards no longer hide
  active RuTTY scripting, hyperlink, KTX import/export, or savedump paths; the
  hyperlink backend source layout was normalized.

## New in 0.84.1.37

- **Saved session passwords work again for auto-login and WinSCP.** The launcher
  passed the stored password to the connecting process in a masked form that was
  then sent verbatim, so every saved password failed (looked like a wrong
  password). Fixed the hand-off; a stored password now authenticates as typed.
- **Event Log "Copy" copies the whole log when nothing is selected** (was: beep).
- **No more credential-hammering that gets your IP banned.** A stored password is now
  auto-answered only **once per connection** (a rejected password is no longer resent
  on every re-prompt; KiTTY falls through to the interactive prompt instead), and
  **auto-reconnect no longer retries on authentication failures** (only network drops
  of a session that actually authenticated). Both previously could exhaust the server's
  `MaxAuthTries` ("Too many authentication failures") and trip fail2ban.
- **Security fix (remote): far2l clipboard parser out-of-bounds read.** A malicious
  SSH server could crash KiTTY with a short, crafted `far2l` clipboard sequence (a heap
  under-read), and register-format / is-available ran with no consent. Now bounds-checked
  and behind the clipboard-consent prompt. Crash/DoS only (no code execution/disclosure),
  but network-reachable — recommended for anyone connecting to untrusted hosts.
- **Whole-codebase + PuTTY-base security sweep.** Every KiTTY-added file, the upstream
  files KiTTY modifies, the remote SSH/terminal parsers and the diagnostic dumps were
  audited and findings adversarially re-verified. Besides far2l, this bounds a batch of
  local-input overflows (session/`.ini`/registry values, autocommand lines, port-knock
  sequences, the proxy list, rutty scripts, exported passwords) and guards config-line
  parsers against malformed/blank lines.
- **Removed the dead `__xy` remote-command dispatcher** (`ManageLocalCmd`) — unreachable
  in the 0.84 base but a latent RCE surface; deleted.
- **Config dialog: position memory, Save keeps selection, delete imported sessions,
  remember last session.** The window remembers its position (re-centres if its monitor
  was removed); **Save** keeps the saved session selected; a new *"show / edit / delete
  old sessions"* checkbox lets you see and remove sessions imported from older
  PuTTY/KiTTY hives (tagged `(old KiTTY)` / `(PuTTY)`; auto-on when your own hive is
  empty); and the last-loaded session is auto-selected and auto-loaded on reopen.
- **WinSCP launch: credentials URL-encoded.** A `@`/`/`/`:` in the username or
  password no longer breaks the WinSCP connection URL or risks redirecting the
  transfer to the wrong host. (Upstream cyd01/KiTTY #535.)
- **`.ini` load: out-of-bounds read/write fixed** on a blank/CR-LF-only line.
  (Upstream cyd01/KiTTY #541.)
- **`/savedump` redacts the known high-risk secret fields.** Session/proxy
  passwords, the SSH key passphrase, the private key file, the password-store
  protection password and clipboard contents are redacted. A legacy script-content
  dump path remains under review, so inspect dumps before public sharing if you
  use login/RuTTY scripting.
- **`__ti` title handler hardened** against a long-title overflow. (Upstream #405.)
- **kageant About box shows the KiTTY/kapper.net copyright.**
- **Verified — not affected:** upstream #531 (CVE-2024-31497 P-521 nonce) and #520
  (Terrapin) are already fixed by the PuTTY 0.84 base.

## New in 0.84.1.36

- **Security fix (CVE-2024-25003 / CVE-2024-25004).** A malicious or compromised
  SSH server could crash KiTTY (or potentially run code) by sending a crafted
  duplicate-session/WinSCP escape sequence with an over-long host or user field
  (a stack buffer overflow). Fixed with bounded parsing. **Update recommended if
  you connect to hosts you don't fully trust.** (Upstream cyd01/KiTTY #525.)
- **Launcher Ctrl+Shift+letter shortcuts work now** — previously they were shown
  in the tray menu but only beeped; they now launch the session while the menu is
  open. (Upstream cyd01/KiTTY #544.)
- **Verified — not affected:** upstream #526 (file-get command injection) and #523
  (UTF-8 window titles) do not affect this port.

## New in 0.84.1.35

- **Run KiTTY inside mRemoteNG / Remote4Support.** KiTTY now embeds correctly in
  connection-manager tabs: it auto-detects when the host docks its window, fixes
  the font size for the host's monitor (no more huge startup font), fills the
  pane, and reflows the terminal on resize (changing the font size no longer
  resizes the pane). A `-hwndparent <handle>` switch is also available for hosts
  that pass one. (Addresses upstream cyd01/KiTTY #554.) See the known-issues note
  above about a minor wobble while dragging the pane height in mRemoteNG.
- **Verified — upstream cyd01/KiTTY #549 does not affect this port.** A
  `savemode=dir` configuration directory whose path contains **spaces** loads
  saved sessions correctly here (live-tested). No change needed.

## New in 0.84.1.34

- **Fix: window position is now remembered when you close with Ctrl+D / remote
  logout.** Previously the position was only saved when you closed via the window
  X / close prompt; a session ended by the remote side (the common case) exited by
  a path that skipped the save, so the window reopened at the default spot. Now
  saved on that path too. *(Note: the first launch after upgrading still won't
  restore — the saved format changed in 0.84.1.33; it self-heals after one move →
  close → reopen.)*
- **Launcher: "Refresh" reopens the menu** so you can immediately pick a
  just-reloaded session instead of going back to the tray icon.
- **Verified — upstream cyd01/KiTTY #545 and #546 do not affect this port.**
  #545 (reconnect fails with a password over 126 characters) and #546 (klink
  always returns exit code 0) were KiTTY-specific bugs; on this PuTTY-0.84 base,
  reconnect re-authenticates through the normal SSH path with the full password
  (no fixed buffer), and klink returns a proper non-zero exit code on auth failure
  in batch mode. No change needed.

## New in 0.84.1.33

- **Fix: slow startup — windows now open instantly.** New windows (a fresh config
  window, a connecting session, or **Duplicate Session**) could take several
  seconds — up to ~10s on some machines — before appearing. The cause was the
  Windows taskbar **Jump List** being rebuilt synchronously during startup; it now
  runs in the background and is skipped for the "Default Settings" load. Opening
  windows is immediate again.
- **Fix: window position on multi-monitor / mixed-DPI setups.** The per-monitor
  position memory added in 0.84.1.32 didn't restore correctly when monitors use
  **different display-scaling** (it relied on `WINDOWPLACEMENT`, which isn't
  DPI-corrected across monitors), so the window opened at a default spot. It now
  uses physical screen coordinates and an order-independent monitor-layout key.
  *Note:* the first launch after upgrading won't restore (the saved format
  changed); it self-heals after one move → close → reopen.
- **New `-noconfirm` command-line flag.** Launch `kitty -noconfirm …` to close the
  terminal window **without** the "Are you sure you want to close this session?"
  prompt — handy for scripts, automation, and quick testing. It only suppresses
  the close prompt; the SSH host-key and weak-crypto **security** confirmations
  are unaffected. (The permanent equivalent is Window → Behaviour → uncheck "Warn
  before closing window".) Addresses part of upstream
  [cyd01/KiTTY #548](https://github.com/cyd01/KiTTY/issues/548).

## New in 0.84.1.32

- **Remember window position (per monitor layout).** New terminal windows — and
  **Duplicate Session** — now reopen where you last closed a window, instead of
  always at the same spot. The position is remembered **per monitor setup** (a
  signature of your connected monitors), so a docked multi-monitor layout and an
  undocked single screen each keep their own position (Word-style). It's
  restored via Windows' placement API, which **clamps a now-off-screen position
  back onto a visible monitor**, so changing/unplugging a display can't strand a
  window. Only the position is remembered; the session keeps its own size. A
  session that pins an explicit X/Y position still wins. On by default —
  **Session → "Remember window position (per monitor layout)"** to turn off.

## New in 0.84.1.31

- **Launcher: tray balloon when an update is available.** On startup the
  `kitty -launcher` tray app now shows a balloon if a newer build is known — a
  backstop to the terminal-start notice, since the launcher itself has no terminal.
- **kageant: tray balloon when an SSH key is used.** When a key signs an
  authentication request, kageant pops a short balloon naming the key. Toggle it
  from the kageant tray menu — **"Notify when a key is used"** (on by default,
  remembered). Non-blocking.

## New in 0.84.1.30

- **Fix: the updater now correctly recognises a beta build.** The build string
  carries no `-beta` marker (it lives only in the release tag), so every build
  was mistaken for a *stable* release. As a result *Check for updates* wrongly
  warned beta users that they were "installing a beta", and the new startup
  "update available" notice was being **suppressed** for beta users. Both are
  fixed — the channel is now derived from the version scheme (stable = `x.y.M.0`,
  beta = `x.y.M.P`). If you run a beta, you now get the update notice and no
  spurious stable-vs-beta warning.

## New in 0.84.1.29

- **About boxes now render proper Unicode.** Both the launcher and the main
  Help → About box previously used ASCII/CP1252 work-arounds to dodge mojibake;
  they now display real Unicode (©, em-dash, the update-notice arrow) correctly on
  any system codepage, via the wide Windows APIs.
- **Main About box credits the port author.** The central Help → About box now
  carries the **KAPPER NETWORK-COMMUNICATIONS GmbH** copyright for the PuTTY 0.84
  port, alongside the existing KiTTY (Cyril Dupont) and PuTTY (Simon Tatham)
  attributions. (The launcher About box already had it.)

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
- Follow-up status: argument quoting + length bounding in the transfer
  command builders shipped in 0.84.1.21, and DPAPI at-rest password
  encryption shipped in 0.84.1.38.

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
- Note: later security passes closed the reversible at-rest password issue with
  DPAPI in 0.84.1.38. Same-user malware remains out of scope; prefer SSH keys /
  kageant for strongest security.

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
- **Connection → SSH → KSCP and WinSCP** (protocol, options, remote dir, shell)
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
