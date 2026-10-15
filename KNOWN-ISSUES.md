# KiTTY++ 0.85.1.16 — Known issues & limitations

Known limitations of this release:

## Functional limitations

- **Workplace proxy mode lasts as long as the launcher holds it.** Logging
  off, a reboot or ending the launcher ends it, by design. It is per
  installation (a portable and an installed copy each have their own), open
  connections keep the proxy they connected through, and the green frame needs
  Windows 11 - on Windows 10 the title marker `⇄ workplace proxy` shows it.
- **A named proxy stores a user name and password, but no key or agent
  settings.** An SSH jump host that needs a specific key still needs a saved
  session for it, with the proxy pointing at that session.
- **The update check looks at the newest release only**, a beta included, and
  a release published since the last start is noticed one start later.
  *Check for updates* in the system menu asks the server at once.
- **far2l shared clipboard:** text travels both ways, pictures are untested.
  far2l gets the Windows clipboard only within 5 seconds of a paste from this
  window. While far2l's key events are on, KiTTY++'s own shortcuts are off
  (the title ends in `[far2l keys]`). far2l images need Vista and sit over the
  text, not scrolling with it.
- **OSC 52 clipboard reads cannot be allowed for good.** Reads offer Deny
  (default) and Ask only, and a grant lasts as long as the answer to the
  prompt states. Writes default to Ask. Sessions saved by older versions keep
  what they had. No remote clipboard access while the window has no focus,
  unless *Only while this window has focus* is turned off.
- **The clipboard and workplace frame tints need Windows 11** (build 22000).
  The title icon works everywhere.
- **Login script and rutty scripting both react to the server's output.**
  With both configured, the login script runs first. Use one per session.
- **adb backend and rutty scripting** are verified against test fixtures, not
  yet against a real Android device or a live remote shell.
- **Encrypted `.ktx` files are read, not written.**
- **Background image:** the thin margin outside the cell grid stays solid. A
  second monitor is verified in code only.
- **The Direct2D renderer is opt-in** (`renderer=d2d`, Windows 8.1 or newer).
  A transparency change applies to the next window. DirectWrite text looks
  slightly different from GDI's, and right-to-left text is not reordered.
  Untested: a DPI change of an open window, WARP-only machines.
- **A maximised GDI window under steady output** reaches 38-54 frames/s, not
  the display's rate.
- **Font fallback draws colour glyphs in monochrome**, on both renderers.
  Supplementary-plane emoji may need an `override=` range in `[FontFallback]`
  (under GDI, plain Latin text never uses it). A raster primary font switches
  the fallback off. `active=no` does not switch off Windows' own fallback.
- **The restricted process ACL cannot be undone in a running process.**
  `restrictacl=yes` also blocks screen readers and keeps an in-place upgrade
  from reopening your windows. kittygen needs `-restrict-acl` itself.
- **A key on removable media is dropped when the media returns on a different
  drive letter.** Keep keys in memory (a kageant setting) and add the key
  again from the new letter.
- **First run copies PuTTY's sessions once** (registry mode, no KiTTY++
  sessions yet). PuTTY is not changed.
- **A bare session name held by several folders does not load**: `-load`
  ends with an error naming them (exit code 1), a jump host fails.
- **A `%` in a session name is written `%25` in a `kitty://` link.**
- **The folder store lists only session files** (a `HostName` or `Protocol`
  setting) and never looks at dot-files and dot-folders.
- **Folder store: classic KiTTY's putty.conf is taken over once** into
  kitty.ini, and only from beside kitty.exe. Later edits to it have no effect.
- **Session Lockdown does not stop a delete.** A locked session can still be
  deleted, renamed and moved. The lock blocks saving its settings only.
- **Host-key files are renamed when `hostkeyextension` is set or changed.**
  An older KiTTY on the same folder prompts once for each host key.
- **Session list limits:** the launcher lists up to 4096 sessions, kageant's
  Saved Sessions menu up to 3584.
- **OSC 8 links to a file on another computer are not opened**, nor web
  addresses with a numeric or encoded host.
- **The mid-session Change Settings list does not navigate folders.**
- **Inline SSH security prompts are not available during a rekey**: the
  connection is closed instead.

## Connectivity tips

- **Slow first connect (2-5 s) without Kerberos:** GSSAPI lookups time out
  first. Untick Connection > SSH > Auth > GSSAPI > "Attempt GSSAPI
  authentication" (and key exchange) in Default Settings.
- **klink requests no terminal when its input is not a console**, as ssh
  does. Use `-t` to force one.

## Security

- **A Windows Hello protected key opens only with Windows Hello or its
  recovery door.** Keep the printed secret or the recovery code away from the
  machine, and back up the `.hello` file together with the `.ppk`. pscp,
  psftp and WinSCP cannot open such a key: load it in kageant once.
- **The agent-identity check works only in signed builds.** A self-built,
  unsigned KiTTY skips it.
- **Stored passwords are DPAPI-encrypted**: safe against offline and
  cross-user theft, not against malware running as you, and they do not move
  to another PC. A portable master-password store moves with its `Security`
  folder. **A forgotten master password cannot be recovered.**
- **SSH-1 keys are held in plain memory**, in kageant and in kittygen. SSH-2
  keys are `CryptProtectMemory`-encrypted except while in use.
- **Helper programs without the KiTTY++ signature need a confirmation the
  first time** (`[TrustedHelpers]`). A file with a broken signature is not
  started.
- **kageant lock:** `ssh-add -x` does not survive a restart of kageant.
- **kageant's key-file check stops a swapped file, not a program running as
  you**: the stored fingerprint is plain text.

## Packaging / cosmetic

- **Dark mode needs Windows 10 1809 or newer.** Session colours are per
  session and stay as set.
- **Antivirus & UPX:** the standard ZIP holds plain signed executables. The
  `-upx.zip` and the installers carry UPX-compressed ones, which some
  antivirus tools object to - take the standard ZIP then.
- **Version string:** binaries report `0.85.1.16-beta @ 2026-10-15`.
- **The configuration box's Exit button has no Alt key.** Esc works.
- **Script with `kittygen-cli.exe`, not `kittygen.exe`**: the window version
  reports an unknown switch in a dialog and waits for OK.
- **Embedded in mRemoteNG**, dragging the pane's height can make the terminal
  wobble a few pixels until released.
- **Silent (`/qn`) installs close and reopen your windows.** Under a machine
  account they close without reopening (`MSIDISABLERMRESTART=1` forces that).
- **Registry backups are UTF-16 `.reg` files** an older KiTTY cannot load.
  `reg import <file>.sav` restores them.
