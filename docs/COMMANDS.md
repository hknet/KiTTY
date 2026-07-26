# KiTTY internal commands

KiTTY's *send-text* box (keyboard shortcut `input`, default **Ctrl+F8**) normally
sends the line you type to the remote host. A line starting with `/` is instead
treated as an **internal command** and executed locally by KiTTY itself. Typing
**`/help`** in the box shows the compact one-line summary of every command; this
document is the full reference. A `/line` that matches no internal command is
sent to the host as ordinary text.

Each entry below notes whether the effect **persists**:

- *runtime only* — lasts until the window is closed;
- *kitty.ini* — an `[KiTTY]` (or other section) key sets the startup default
  for **all** windows;
- *session* — stored in the saved session (survives via `/save` / `/savenew`).

See [FEATURES.md](../FEATURES.md) for the surrounding features (send-text boxes,
`[Shortcuts]`, save modes, the configuration password).

## Window & title

### /size

Toggle the `[rows x cols]` size suffix in the window title. If the title
decorations are off entirely (see `/wintitle`), `/size` turns both the
decorations and the suffix on — "show me the size" always works.

**Persists:** runtime only; startup default via kitty.ini `size=yes`
(the key can only enable the suffix, `size=no` has no effect).

### /wintitle

Toggle the KiTTY title decorations (the bracketed session/size markers) on or
off for this KiTTY instance.

**Persists:** runtime only; kitty.ini `wintitle=no` disables decorations at
startup (only `no` has an effect — decorations are the default).

### /title `<text>`

Set the window title to `<text>`. The title set in the session configuration
(*Window → Behaviour*) is what persists; this override lasts until the server
or KiTTY next changes the title.

**Persists:** runtime only.

### /transparency

Toggle window transparency. When turning on, the transparency level comes from
the session's configured value (*Window → Transparency*); a session with no
value configured starts fully opaque.

**Persists:** runtime only; the level is a session setting, and kitty.ini
`transparency=yes/no` enables/disables the feature at startup.

### /backgroundimage

Toggle the background-image feature on or off.

**Persists:** runtime only; startup default via kitty.ini `bgimage=`.

### /icon

Toggle the per-session window icons feature (each session shows its configured
icon instead of the default KiTTY icon).

**Persists:** runtime only; startup default via kitty.ini `icon=yes` (the key
can only enable). The icon *number* itself is a session setting.

### /hyperlink

Toggle clickable URLs (hyperlink detection) in the terminal.

**Persists:** runtime only; startup default via kitty.ini `hyperlink=`.

### /winroll

Toggle the roll-up feature: double-clicking the title bar collapses the window
to just its title bar (and back).

**Persists:** runtime only; startup default via kitty.ini `winroll=`.

### /redraw

Force a full repaint of the terminal window.

**Persists:** action, nothing stored.

### /refresh

Refresh (re-render) the background image.

**Persists:** action, nothing stored.

## Info

### /init

Show the configuration environment of this KiTTY instance: configuration
directory, save mode, initial directory, the kitty.ini and kitty.sav paths, and
the window class name. Useful to see *which* configuration a running KiTTY is
actually using.

**Persists:** informational, nothing stored.

### /session

Show the name of the saved session this window is bound to (the session
`/save` would write to), or "No session name." for an ad-hoc connection.

**Persists:** informational, nothing stored.

### /urlregex

Show the regular expression used for URL detection (see `/hyperlink`), and
whether it is the built-in default.

**Persists:** informational, nothing stored.

### /message `<text>`

Show `<text>` in a message box. Mainly useful from scripts and from
`/command` broadcasts.

**Persists:** action, nothing stored.

### /help

Show the compact list of all internal commands (the one-line summaries in this
document, grouped the same way) in a separate resizable window that stays open
while you keep typing commands into the send-text box. `/help` with the window
already open brings it to the front; Esc or Close dismisses it, and the text
can be selected and copied (Ctrl+A, Ctrl+C).

**Persists:** informational, nothing stored (the window position is remembered
like the other pop-up windows).

## Settings & storage

### /save

Save the window's **live** settings back to its saved session — font, colours,
window size, logging, everything the configuration box would save. If the
window is not bound to a saved session, KiTTY tells you to use `/savenew`
instead.

**Persists:** session.

### /savenew `<name>`

Save the live settings as a **new** session called `<name>` and switch this
window's identity to it (later `/save` calls write there, and the title
decorations show the new name).

**Persists:** session.

### /savektx

Export the live settings to a `.ktx` connection file (a file dialog asks
where). A `.ktx` file double-clicked or passed on the command line opens that
connection directly.

**Persists:** the exported file.

### /savemode

Cycle the configuration save mode: **registry → file → dir → registry…**.
The mode applies immediately to this instance; "file" is also written to
kitty.ini (`savemode=file`) so it survives a restart. Registry and dir modes
remove the `savemode=` key.

**Persists:** kitty.ini for "file"; runtime only for the others.

### /savereg

Export the complete KiTTY registry configuration (sessions, host keys,
settings) to `kitty.sav` next to the exe.

**Persists:** the exported file.

### /loadreg

Import `kitty.sav` (see `/savereg`) back into the registry.

**Persists:** registry.

### /delreg

**Deletes the whole KiTTY registry tree** — all saved sessions, host keys and
settings stored in the registry, without further confirmation. Configurations
saved in file/dir mode are not touched. Use with care; `/savereg` first makes
a backup.

**Persists:** destructive registry action.

### /savesessions

Export only the saved sessions (not host keys or global settings) to
`kitty.ses` next to the exe.

**Persists:** the exported file.

### /copytoputty

Copy KiTTY's saved sessions and SSH host keys into stock PuTTY's registry
location (`Software\SimonTatham\PuTTY`). **PuTTY's existing sessions are
deleted first** and KiTTY-specific settings are stripped from the copies.

**Persists:** registry (PuTTY's).

### /copytokitty

Copy stock PuTTY's whole registry tree (sessions, host keys) into KiTTY's
registry location. Existing KiTTY entries with the same names are overwritten.

**Persists:** registry (KiTTY's).

### /configpassword `[pw]`

With an argument: set the configuration password to `pw`, switch the save mode
to *file* and store the password (encrypted) in kitty.ini. With no argument:
clear the configuration password. Check your save mode after clearing — KiTTY
reminds you at the next launch.

**Persists:** kitty.ini / registry.

### /-configpassword

Show the currently stored configuration password in a message box, or report
that none is set. The password is read from whichever store actually holds it —
the registry in registry/file save mode, `kitty.ini` in portable mode — because
the two keep it in different forms; a stored value that cannot be decoded is
reported as such rather than displayed as garbage.

**Persists:** informational, nothing stored.

### /switchcrypt

Switch the variant of the settings-encryption used for stored secrets (the
`cryptsalt=` mechanism in kitty.ini). Advanced — only relevant when moving
configurations between builds that differ in crypt mode.

**Persists:** runtime only.

### /delfolder `<name>`

Delete the session folder `<name>` from the folder list (the sessions in it
are not deleted).

**Persists:** folder list.

### /loadinitscript `[file]`

(Re)load the login-script file for this session — with `[file]` given, load
that file; without, reload the session's configured script (*Connection →
Data → Login script*). The script content is stored in the session settings.

**Persists:** session (after `/save`).

## All KiTTY windows

### /command `<text>`

Send `<text>` to **all** open KiTTY windows at once, as if it had been typed
in each window's send-text box — so `<text>` can itself be an internal command
(e.g. `/command /size`) or text for every remote host.

**Persists:** action, nothing stored.

### /sizeall

Resize all open KiTTY windows to this window's configured size.

**Persists:** runtime only.

## Behaviour & diagnostics

### /shortcuts

Reload the `[Shortcuts]` key bindings from kitty.ini (after editing the file,
no restart needed). See FEATURES.md for the `[Shortcuts]` syntax.

**Persists:** kitty.ini is the source.

### /noshortcuts

Disable the keyboard-shortcut layer (all `[Shortcuts]` bindings) until this
instance restarts or `/shortcuts` re-enables them.

**Persists:** runtime only; kitty.ini `shortcuts=no` disables at startup.

### /nomouseshortcuts

Disable the mouse-shortcut layer until this instance restarts.

**Persists:** runtime only; kitty.ini `mouseshortcuts=no` disables at startup.

### /bcdelay `[ms]`

Set the between-character send delay to `ms` milliseconds (no argument: 3 ms;
`0` switches it off). Paces every automatic keyboard send — autocommand, login
scripts, send-text boxes — for hosts that drop characters arriving too fast.

**Persists:** runtime only; startup default via kitty.ini `bcdelay=`.

### /PrintCharSize `<n>`

Set the font size used for printing (*File → Print* / print controls).

**Persists:** runtime only; startup default via kitty.ini `[Print] height=`.

### /PrintMaxLinePerPage `<n>`

Set the maximum number of lines per printed page.

**Persists:** runtime only; startup default via kitty.ini `[Print] maxline=`.

### /PrintMaxCharPerLine `<n>`

Set the maximum number of characters per printed line.

**Persists:** runtime only; startup default via kitty.ini `[Print] maxchar=`.

### /zmodem

Toggle the ZModem file-transfer feature (the `rz`/`sz` integration).

**Persists:** runtime only; startup default via kitty.ini `zmodem=`.

### /fileassoc

Register the `.ktx` file association for the current user, so double-clicking
a `.ktx` connection file opens it with this kitty.exe.

**Persists:** registry (per-user file association).

### /initlauncher

(Re)create the registry key used by the KiTTY launcher's menu.

**Persists:** registry.

### /debug

Toggle debug mode. Debug mode enables extra diagnostics (and `/passwd`).

**Persists:** runtime only; startup default via kitty.ini `debug=`.

### /passwd

**Debug mode only** (see `/debug`; without it the line is sent to the host as
plain text). Shows the session's stored password in a message box **and copies
it to the clipboard** — remember the clipboard keeps it until overwritten.

**Persists:** informational, nothing stored.

### /savedump

Write `kitty.dmp` next to the exe: an encrypted diagnostic dump of the
configuration and runtime state, for support purposes. Same as launching
`kitty.exe -savedump`.

**Persists:** the dump file.

### /screenshot

Save a screenshot of the terminal window as `screenshot-<pid>-<time>.jpg`
next to the exe.

**Persists:** the image file.
