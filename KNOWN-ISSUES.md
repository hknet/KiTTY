# KiTTY++ 0.85.1.11 — Known issues & limitations

The port builds **clean** (all binaries, 0 warnings, 0 errors) and ~46 old KiTTY
features are working and verified. Known limitations as of this release:

> **Where the clipboard settings live.** They moved in 0.84.1.68 and are now
> under **Window → Copy & Paste → Remote clipboard**, with the numeric limits in
> **→ Limits** and the title/tray markers in **→ Notices**. References below use
> the new locations.

## Functional limitations

- **Workplace proxy mode lives only as long as the launcher is holding it.**
  This is by design — logging off, rebooting or killing the launcher ends it, so
  it cannot be left switched on by accident, and it does not survive a restart.
  The next start displays once that it is not active and offers
  nothing further; switching it on again is one click, in the tray menu or on the
  Workplace Proxy panel in the application tree.
- **Connections already open keep the proxy they connected through.** Switching
  the mode on does not re-route a running session, and switching it off does not
  take the proxy away from an active one: established connections cannot
  be re-routed.
  Only new connections follow the mode — which is why the green frame and the
  title marker describe the *connection* rather than the mode.
- **The green frame needs Windows 11.** Colouring a window's frame and caption is
  only possible on build 22000 or newer. On Windows 10 the title still carries
  `⇄ workplace proxy`, which is what actually shows that the connection is
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
  session happens to share the name accidentally.
- **`/size` and `/wintitle` are application-wide, not per session.** They are
  runtime toggles rather than session settings, so `/save` does not store them —
  persist them with `[KiTTY] size=yes` / `wintitle=no` in kitty.ini.
- **A brand-new release is noticed one launch late.** The startup update check
  runs on a worker thread and only refreshes a cached answer, so the notice about
  a release published since your last start appears on the *next* start. *Check
  for updates* in the system menu always asks the server there and then. A
  launcher left running looks again after every 24 hours; kageant never
  queries the network and shows what the terminal or the launcher stored last.
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
  **Ask** (Window → Copy & Paste → Remote clipboard), answering **OK** grants the
  remote access to your clipboard for the rest of that session — it does not
  re-prompt per request. Set it to **Deny** if you do not want a remote `far2l`
  to read/write your clipboard. The focus rule below still applies to it.
- **OSC 52 remote clipboard writes are asked about, text-only, and write-only.**
  *Remote clipboard writes (OSC 52)* (Window → Copy & Paste) defaults
  to **Ask** as of 0.84.1.69 — the write direction changes what you paste next
  and cannot disclose anything to the host, but it is still a remote host
  reaching into something you use for passwords. **Sessions saved before this
  release keep whatever they had**, including Allow; only sessions created from
  now on start at Ask. Set it to **Deny** if you would rather no host touched
  your clipboard at all, or back to **Allow** for the old behaviour.
  Only **text** travels this way; the sequence carries nothing else, so images
  and other clipboard formats are unaffected.
  As with far2l, answering **OK** to an **Ask** prompt grants access for the rest
  of that session rather than re-prompting per payload; changing any setting in
  the configuration box makes it ask again. A payload too large to fit, or one
  that is not valid base64, is refused entirely rather than pasted in part.
- **OSC 52 remote clipboard *reads* are off by default, and there is no way to
  switch them permanently on.** *Remote clipboard reads (OSC 52)* (Window →
  Copy & Paste) is the other direction: a host asking for the contents of your
  clipboard, which are then sent to it. It offers **Deny** (the default) and
  **Ask** — and deliberately no "Allow", because a clipboard holds a password
  often enough to matter and the host chooses the moment it asks. A read can be
  permitted only by answering the prompt, and only for as long as that answer
  says: one request, a number of minutes, a number of requests, or the rest of
  the session, the last of which asks a second time before it takes effect. No
  permission to read is ever written to disk. Every limit is a setting in the
  same panel.
- **No remote clipboard access at all while the window has no keyboard focus.**
  *Only while this window has focus* (Window → Copy & Paste → Remote clipboard)
  defaults to on and covers reads **and writes**, across all three protocols. An
  existing permission is suspended rather than cancelled — the title marker gains
  a pause sign and greys — and resumes without asking again when you come back.
  Verified end-to-end in 0.84.1.68. Turn it off if you rely on a background job
  that copies its own output into your clipboard.
- **A host that asks too fast is refused, but keeps its permission.** *Shortest
  gap between reads* (Window → Copy & Paste → Remote clipboard → Limits) refuses a
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
- **OSC 5522 (the other kitty's clipboard protocol): reads work, writes do not.**
  Reads go through the same permission control and the same limits as OSC 52 reads
  above — it is one permission, reachable two ways, not two settings.
  Because this protocol can identify the program asking, a program that sends
  a password and a name can be approved once and then not asked about again
  for as long as that answer lasts;
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
  A session's **login script** (Connection → Login) and **rutty** scripting
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
  margin strip outside the grid is still solid-filled (cosmetic). The image now
  covers the whole virtual desktop; that this reaches a second monitor has
  been verified in code but not yet on a multi-monitor desk.
- **The Direct2D (GPU) renderer is opt-in.** `renderer=d2d` needs
  Windows 8.1 or newer. A translucent window runs it on the blit-model swap
  chain, which costs the flip model's present path and the compositor's frame
  signal (the pacing then runs on the timer); the badge in the top-right
  corner says whether a window is on Direct2D at all. DirectWrite rasterises
  the text, so it looks slightly different from GDI's, and right-to-left text
  is placed glyph by glyph without the reordering GDI applies. Untested so
  far on this path: the trust sigil on screen, a DPI change of the terminal
  window while it is open, and a machine that has only the software (WARP)
  Direct3D device. Report what you see; GDI is one setting away if things
  don't work for you.
- **Font fallback renders monochrome**, on both renderers. Missing-glyph
  fallback draws with plain GDI or a plain DirectWrite glyph run, so emoji and
  other colour glyphs taken from a fallback font come out as monochrome
  outlines. Supplementary-plane emoji may additionally need an
  explicit `override=` range in `[FontFallback]`. With a raster (non-TrueType)
  primary font such as Terminal or Fixedsys, the fallback feature disables
  itself for that session. `active=no` turns it off entirely.
- **Command-line tools use the registry session store.** `klink`/`kscp`/`ksftp`
  (plink/pscp/psftp) read saved sessions from the Windows **registry**, not from a
  portable (`savemode=dir`) store — so a portable install's sessions, and any
  passwords a **master password** protects there, are usable from the KiTTY
  **GUI** but not from the command-line tools. Registry-mode sessions (with their
  DPAPI-protected passwords) work from the CLI tools as before.
- **The restricted process ACL cannot be inspected or undone.** `[KiTTY]
  restrictacl=yes` (and the older `-restrict-acl` switch) hardens the process so
  nothing running as you can inspect it — which also **blocks accessibility
  software** such as screen readers, stops an **in-place upgrade from reopening
  your windows** (Windows' Restart Manager cannot inspect a restricted process),
  and cannot be lifted within a running process (`restrictacl=no` does not
  un-restrict — restart for unrestricted processes again). kittygen does not
  read the setting; pass it `-restrict-acl` if you want the same there.
- **A key on removable media is dropped if the media returns on a different
  drive letter.** kageant loads a startup key by its path: pull the stick and
  put it back as the same letter and it carries on, but a different letter means
  the path is gone and the key is no longer held. Keep keys in memory (a kageant
  setting) so they keep signing while the media is away, then add the key again
  from the new letter. Re-locating a key by its fingerprint when the volume
  returns is planned.
- **Session folders are one level deep.** A folder holds sessions, not other
  folders, so with `foldernavigation=yes` the `..` row always returns to the
  root.
- **First run copies PuTTY's sessions once.** In registry mode, a first start
  with no KiTTY++ hive of its own copies stock PuTTY's sessions into KiTTY++'s
  store (or restores KiTTY++'s newest backup if one exists); PuTTY is not
  changed. It happens once per machine and only while KiTTY++ has no sessions
  yet. The settings of an old KiTTY (`9bis.com`) are taken first; when they
  came along without a session, only PuTTY's sessions are copied, not its
  host key cache.
- **A portable store whose sessions live in SUBDIRECTORIES is not read.** This
  version writes one flat file per session under `Sessions\` with its folder
  recorded inside, and lists only the files directly there; a classic
  `browsedirectory` layout copied across shows none of its sessions, and
  `browsedirectory=yes` does not change which sessions are listed. Move the files
  up into `Sessions\` — each keeps working — and re-file them from the config box.
- **The mid-session Change Settings list does not navigate folders.** It opens on
  the running session's folder and stays there; a rename or a move belongs in the
  config box you start from, where the whole store is in front of you.
- **kageant's own Saved Sessions menu is a flat list.** It reads the registry
  directly, so it does not group by folder and does not see a portable store;
  KiTTY's tray launcher is the one that mirrors your folders.
- **Inline SSH security confirmations are not available during a rekey.** The
  opt-in in-terminal host-key/weak-key prompts
  (`modalnewhostkeyconfirmation` and friends) cannot run while an
  already-authenticated session rekeys — the running program owns the terminal —
  so those confirmations abort the connection instead of asking.

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

- **Dark mode: what it does not cover.** It needs **Windows 10 1809 or newer**;
  on anything older every value behaves as *Always light*.
  Windows never gave Win32 dialogs a dark mode, so a few pieces are drawn by
  hand — the tab strip, the radio buttons and check boxes, the group boxes, the
  progress bars, the list column headers — and a selected row in the agent log
  gives up its colour coding for the system's own highlight colours, the one
  combination certain to stay readable. A terminal's own colours are a
  per-session setting and are left alone: a dark KiTTY++ still opens each session
  in the colours that session requires.
- **Antivirus & UPX:** the standard `kitty-<version>.zip` contains only plain,
  uncompressed signed executables (antivirus-friendly). The `-upx.zip` flavour
  and the installers carry UPX-compressed `kitty.exe`/`kitty_portable.exe` for
  the smallest download; UPX can trip heuristic AV/SmartScreen, so if your
  antivirus objects, take the standard ZIP.
- **Version string:** binaries report `0.85.1.11-beta @ 2026-09-19`.
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
- **Silent (`/qn`) installs close and reopen your windows**, like the
  interactive upgrade. Started under a machine account there is no desktop to
  reopen onto, so they close **without** reopening; `MSIDISABLERMRESTART=1`
  forces that behaviour in any silent install.
- **Registry backups are standard UTF-16 `.reg` files, and are no longer
  encrypted.** An older KiTTY version cannot load one itself — it treats the
  file as unreadable — but Windows restores it perfectly well with
  `reg import kittynew-YYYYMMDD-HHMMSS.sav` (no administrator rights) or from
  Registry Editor; this only arises if you downgrade. Existing encrypted backups
  from older versions are still read, KiTTY asking for the password on load.

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
