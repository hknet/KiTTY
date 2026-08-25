# KiTTY 0.85.1.3 — Known issues & limitations

The port builds **clean** (all binaries, 0 warnings, 0 errors) and ~46 KiTTY
features are working and verified. Known limitations as of this release:

> **Where the clipboard settings live.** They moved in 0.84.1.68 and are now
> under **Window → Selection → Remote clipboard**, with the numeric limits in
> **→ Limits** and the title/tray markers in **→ Notices**. References below use
> the new locations.

## Functional limitations

- **Workplace proxy mode lives only as long as the launcher holding it.** That is
  the design — logging off, rebooting or killing the launcher ends the mode, so
  it cannot be left switched on by accident — but it does mean the mode does not
  survive a restart. The next start says once that it is not active and offers
  nothing further; switching it on again is one click, in the tray menu or on the
  Proxy panel.
- **Connections already open keep the proxy they connected through.** Switching
  the mode on does not re-route a running session, and switching it off does not
  take the proxy away from one: an established connection cannot be re-routed.
  Only new connections follow the mode — which is why the green frame and the
  title marker describe the *connection* rather than the mode.
- **The green frame needs Windows 11.** Colouring a window's frame and caption is
  only possible on build 22000 or newer. On Windows 10 the title still carries
  `⇄ workplace proxy`, which is what actually states that the connection is
  proxied; the colour is a bonus on top of it. (Same limitation as the clipboard
  tints.)
- **Workplace proxy mode is per installation.** It is keyed to the folder KiTTY
  runs from, so a portable copy and an installed one each have their own:
  switching it on in one does not affect the other, and each shows its own tray
  icon.
- **A named proxy stores a username and password, but no key or agent settings.**
  Those two travel with the definition and are encrypted at rest like a session
  password. What it cannot state is how an **SSH jump host** should authenticate
  with a key: the private-key file and the *attempt authentication using kageant*
  setting come from your Default Settings, or from a saved session when the
  proxy's Name/IP is one. So a jump host that needs a specific key still means
  saving a session for it and pointing the proxy at that session. A future
  release will let the proxy editor own those settings directly. Since 0.84.1.69
  the proxy at least **says which of the two its Name/IP means** (the
  *.. this is ..* setting beside it), so that is no longer decided by whether a
  session happens to share the name.
- **`/size` and `/wintitle` are application-wide, not per session.** They are
  runtime toggles rather than session settings, so `/save` does not store them —
  persist them with `[KiTTY] size=yes` / `wintitle=no` in kitty.ini. Making the
  title decorations per-session is still owed.
- **A brand-new release is noticed one launch late.** The startup update check
  runs on a worker thread and only refreshes a cached answer, so the notice about
  a release published since your last start appears on the *next* start. *Check
  for updates* in the system menu always asks the server there and then.
- **The update check looks at the newest release only.** A stable user is
  therefore told about the newest *beta* rather than the newest stable, and asked
  before anything is installed. Full stable-only channel filtering waits for
  stable releases to resume.
- **far2l shared clipboard: both directions verified.** SET (a remote `far2l`
  writing your Windows clipboard) and GET (a remote reading it) were both driven
  over the wire and confirmed in 0.84.1.68, including an 80 KB payload.
  ⚠️ Earlier releases of this file said GET travelled only over **SSH** and not
  over **raw** — that was **wrong**, and the measurement that produced it was
  faulty. GET works on both. Whether **non-text** clipboard formats (images)
  round-trip is still unverified; only text has been tested.
- **far2l clipboard privacy latch:** when **far2l shared clipboard** is set to
  **Ask** (Window → Selection → Remote clipboard), answering **OK** grants the
  remote access to your clipboard for the rest of that session — it does not
  re-prompt per request. Set it to **Deny** if you do not want a remote `far2l`
  to read/write your clipboard. The focus rule below still applies to it.
- **OSC 52 remote clipboard writes are asked about, text-only, and write-only.**
  *Remote clipboard writes (OSC 52)* (Window → Selection) defaults
  to **Ask** as of 0.84.1.69 — the write direction changes what you paste next
  and cannot disclose anything to the host, but it is still a remote host
  reaching into something you use for passwords. **Sessions saved before this
  release keep whatever they had**, including Allow; only sessions created from
  now on start at Ask. Set it to **Deny** if you would rather no host touched
  your clipboard at all, or back to **Allow** for the old behaviour. Only **text** travels this way; the sequence
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
  *Only while this window has focus* (Window → Selection → Remote clipboard)
  defaults to on and covers reads **and writes**, across all three protocols. An
  existing permission is suspended rather than cancelled — the title marker gains
  a pause sign and greys — and resumes without asking again when you come back.
  Verified end-to-end in 0.84.1.68. Turn it off if you rely on a background job
  that copies its own output into your clipboard.
- **A host that asks too fast is refused, but keeps its permission.** *Shortest
  gap between reads* (Window → Selection → Remote clipboard → Limits) refuses a
  request that arrives too soon; it does **not** revoke a grant you gave. Only the
  per-window read ceiling ends a grant early. In 0.84.1.67 and earlier the pacing
  limit withdrew the permission and prompted again, which turned a chatty program
  into a stream of dialogs.
- **far2l shared clipboard: payloads over ~2 KB used to be dropped in silence.**
  Fixed — a far2l clipboard payload may now be up to 16 MB (raise *Largest
  payload, in MB* if you copy 4K screenshots), sized for an image
  rather than a line of text, and one that still does not fit is refused whole and
  recorded in the Event Log instead of vanishing. If copying large selections in a
  far2l session appeared to do nothing before, that was this. Whether non-text
  formats (images) round-trip is not yet verified.
- **OSC 5522 (kitty's clipboard protocol): reads work, writes do not.** Reads go
  through the same permission control and the same limits as OSC 52 reads above —
  it is one permission, reachable two ways, not two settings. Because this protocol
  can identify the program asking, a program that sends a password and a name can
  be approved once and then not asked about again for as long as that answer lasts;
  those approvals are never written to disk. Writes (`type=write`, `wdata`,
  `walias`) answer **ENOSYS**, so a program falls back to OSC 52 for text. Paste
  events (`CSI ? 5522 h`) are not implemented, and the mode is ignored rather than
  accepted — enabling it would otherwise look like it had worked.
- **The title-bar and border tint needs Windows 11.** While a clipboard
  permission is live, or just after the clipboard has been used, the window is
  marked. The *icon* in the title works everywhere; the colouring uses an API that
  exists only on Windows 11 build 22000 and newer and silently does nothing on
  Windows 10, so the icon has to carry the meaning by itself.
- **The title marker is monochrome, by necessity.** Windows draws window-title
  text without colour, so a coloured glyph there arrives as a dark blob. The
  marker is therefore a clipboard icon plus a solid triangle — up for a read, down
  for a write — and the colour lives in the frame tint instead, where Windows 11
  will render it.
- **Two scripting features watch the same output, and only one should be armed.**
  A session's **login script** (Connection → Data) and **rutty** scripting
  (Session → Scripting) both react to what the server sends. If both are
  configured, KiTTY warns and runs the login script first, then rutty. Prefer one
  per session. The login script did not run at all before 0.84.1.68.
- **adb backend & rutty scripting:** functional and verified against test
  fixtures (a fake adb server / a scripted listener), but **not** yet validated
  against a real Android device or a live remote shell.
- **Encrypted `.ktx` configuration files are no longer written.** They were
  encrypted under a key compiled into every copy of KiTTY, so possession of the
  program was enough to read them. Existing files are still **read**, so imports
  and old backups keep working, and anyone whose settings ask for the feature is
  told once what to do instead. The **Shift+F12 / Shift+F11** scramble shortcuts
  are gone with it.
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

- **A Windows Hello protected key can only be opened by Windows Hello - or
  by its recovery door.** Protection puts the key's real passphrase in a
  `.hello` file beside it, wrapped so that only this Windows account on this
  machine can unwrap it. That is the point, and it is also the risk: a
  reinstalled Windows, a deleted passkey or a different account cannot open
  the key. Every protected key therefore has a second door, and KiTTY refuses
  to create one without it - either the printed secret (which IS the key's
  passphrase, so it opens the file in any PuTTY-compatible tool) or a recovery
  code that only works together with the `.hello` file. Keep whichever you
  chose somewhere other than the machine holding the key.
- **The sidecar is part of the key.** Copying a protected `.ppk` without its
  `.hello` file leaves a key nobody can open unless the printout is the
  passphrase kind. Back them up together.
- **Other programs cannot open a protected key file.** pscp, psftp and WinSCP
  know nothing about the sidecar, so KiTTY does not hand them the path: load
  the key in kageant once and they get it from the agent instead. A transfer
  started without the key in the agent says so rather than failing obscurely.
- **The Hello gesture is per unlock, with a short cache.** One gesture covers a
  batch of keys loaded together, and the agent's cache (60 seconds by default,
  `0` disables it) covers quick successive unlocks; after that the next unlock
  asks again.

- **The agent-identity check works only in signed builds.** Since 0.84.1.72 a
  KiTTY that is itself Authenticode-signed verifies which process answers its
  agent requests and warns when it is not our signed kageant. A locally built,
  unsigned KiTTY cannot vouch for anyone and skips the check entirely — so
  "no warning" in a self-built binary is absence of the check, not a clean
  bill.
- **kittygen's in-memory protection covers SSH-2 keys.** A generated or
  loaded SSH-2 private key is held `CryptProtectMemory`-encrypted from
  0.84.1.72 and decrypted only for the instant of use. Legacy **SSH-1** keys
  stay in the clear while the window is open — the format is obsolete and the
  retrofit deliberately did not touch that path. The residual for SSH-2 is the
  brief decrypt-to-use window itself.
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
  temporarily while signing. Legacy **SSH-1** keys are the exception, in the
  agent just as in kittygen: they are held in plain process memory for as
  long as they are loaded — the format is obsolete, and the protection was
  deliberately not extended to that path. **If security matters, prefer
  public-key authentication (kageant) with SSH-2 keys, and avoid saving
  passwords unless you understand these limits.**

- **Remote clipboard reads report when they could not be served.** A read request
  that arrives while another program holds the clipboard used to be refused in
  silence; it is now refused with a reason in the Event Log, so "nothing
  happened" can be told apart from "it was denied".

## Packaging / cosmetic

- **Antivirus & UPX:** the standard `kitty-<version>.zip` contains only plain,
  uncompressed signed executables (antivirus-friendly). The `-upx.zip` flavour
  and the installers carry UPX-compressed `kitty.exe`/`kitty_portable.exe` for
  the smallest download; UPX can trip heuristic AV/SmartScreen, so if your
  antivirus objects, take the standard ZIP.
- **Version string:** binaries report `0.85.1.3-beta @ 2026-08-25`.
- **`kittygen.exe` and `kittygen-cli.exe` are two programs with two command
  lines.** The window one takes only `-t`, `-b`, `-E`, `-primes`, `-strong-rsa`,
  `-ppk-param`, `-restrict-acl` and `-pgpfp`; `-C`, `-q`, `-o`, `-l` and
  `--new-passphrase` belong to **`kittygen-cli.exe`**. Give the window version a
  switch it does not know and it says so in a dialog and waits for OK — correct
  for a window, fatal in a script, which then hangs until someone clicks it.
  **Script with `kittygen-cli.exe`.** Merging the two is on the list.
- **kageant's key-file check protects against a swapped file, not against a
  program running as you.** The fingerprint it compares against, and the path it
  watches, are stored in the registry (or in `kitty.ini` on a portable install)
  in plain text, so anything running under your account can rewrite them to
  match a file it has put there. It answers "is this still the key that was on
  my stick?", which is the case it was built for; it is not a defence against
  malware already running as you.
- **Embedded in mRemoteNG — vertical-drag wobble:** when KiTTY is hosted inside a
  connection manager, dragging the pane's **height** can make the terminal wobble
  a few pixels while you drag. It's the host's own caption-offset compensation;
  it settles when you release. Cosmetic.

## New in 0.85.1.3

- **Windows Hello protected keys are new in this release**, across kageant,
  KiTTYgen, the terminal and the file-transfer hand-offs. What that means for
  recovery and for other programs is under **Security** above; the short
  version is that a protected key always has a second door, and the `.hello`
  file belongs with the key.
- **The configuration window dropping behind other windows is fixed**
  (hknet/KiTTY#38). It could happen while clicking the session list in the
  first seconds after the box opened.

## New in 0.85.1.0

- **A key on removable media is dropped if the media returns on a DIFFERENT
  drive letter.** Pull the stick and put it back as the same letter and kageant
  carries on; put it back as another letter and a key that was loaded from it at
  startup is no longer held, because the path it was loaded from no longer
  exists. One can opt to keep keys loaded in the kageant's settings they are held in
  memory, and they keep signing even while the media is away. Re-locating a key
  by its fingerprint when the volume comes back is planned; until then, add it
  again from the new letter. (A decoy key left at the OLD path is not picked
  up, if we have the Key-Fingerprint already on record.)
- **Bulk output is much faster, and the scrollback is now the slowest part of
  it.** With scrollback off, output is roughly twice as fast again. Nothing is
  wrong with the scrollback — it compresses every line that scrolls off, and
  that is what costs — but if you routinely dump megabytes into a window and
  care about the last second of it, a smaller scrollback is now the setting that
  moves the needle. Of course you can always buy newer hardware :).
- **Switching category in the configuration window still rebuilds every control
  on the page.** It is faster than it was and no longer flickers, but the cost is
  in creating and destroying the controls themselves, which is how the dialog has
  always worked. Panels with many controls are therefore the slowest to switch
  to.
- **`-sendcmd` needs to be switched on before it does anything.** It is off by
  default: a broadcast is refused unless `[KiTTY] sendcmdmode=yes` is set for the
  installation AND the receiving session accepts broadcasts (Session →
  Scripting, or the Tools menu of the terminal-window). This is deliberate — anything
  running under your account can post the same message, so an ungated version
  would let any program type into every open session — but it does mean
  `sendcmdmode=yes` alone changes nothing until a session opts in!
- **A broadcast is typed into the session, not executed.** `-sendcmd "/delreg"`
  types those characters at the far end; it does _not_ run an internal command in
  the receiving windows. That is what the feature always meant, and it is worth
  knowing before aiming one at a shell.
- **The broadcast key is not a password.** Anything running under your account
  can read it and send a matching message. It exists to stop ACCIDENTS — the
  broadcast meant for three lab machines landing in the production session left
  open behind them — not to keep anything out.

## New in 0.84.1.75

- **Session folders are one level deep.** A folder holds sessions, not other
  folders — with `foldernavigation=yes` the `..` row therefore always returns to
  the root. This is the same shape the folder drop-down has always had; the rows
  only change how you move through it.
- **A portable store whose sessions live in SUBDIRECTORIES is not read.** The old
  `browsedirectory` layout kept each folder as a real directory under
  `Sessions\`; this version writes one flat file per session with its folder
  recorded inside, and it lists only the files directly under `Sessions\`. So a
  classic portable KiTTY folder copied across shows none of its sessions.
  Setting `browsedirectory=yes` does **not** fix that — it only changes where a
  folder *name* is looked up, not which sessions are listed. Until an import
  exists, move the session files up into `Sessions\` yourself; each one keeps
  working, and you can re-file it from the config box afterwards.
- **The mid-session Change Settings list does not navigate folders.** It opens on
  the folder the running session is in and stays there: the list is only there to
  name what you are saving, and a rename or a move belongs in the config box you
  start from, where the whole store is in front of you.
- **kageant's own Saved Sessions menu is a flat list**, and reads the registry
  directly — so it does not group by folder, and it does not see a portable
  store. KiTTY's tray launcher is the one that mirrors your folders.

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
- **Running the MSI again over an existing install can fail with 1603.** This
  is Windows' own "SecureRepair" check, not KiTTY: re-running an installer
  puts Windows into repair mode, where it insists on finding the package under
  the file name your FIRST install ran from. If that install was started
  straight from a browser's download list, the name was a temporary one that
  no longer exists, and the repair aborts. The log says
  `SECREPAIR: Error determining package source type`. To install a new
  version, uninstall the old one first, or run the MSI from a normal folder
  under the same file name as before.

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

## Still not ported (known)
- *(None known.)* far2l **real clipboard** landed in 0.84.0.15, and both
  directions were later driven over the wire and confirmed - including over
  **raw**, correcting an earlier claim in this file that GET travelled only over
  SSH. `-savedump`, the last deferred CLI switch, landed in 0.84.0.14 and was
  then removed outright in 0.84.1.65 (see Security).

## Earlier releases

Release-by-release notes for every version before 0.84.1.60 - and for these ones
too - are on the releases page, each pinned to its own tag:

https://github.com/hknet/KiTTY/releases

They used to be repeated here, which made this file 1,400 lines of history
wrapped around the ~200 that answer "what should I know before using it".
