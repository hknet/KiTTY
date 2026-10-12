# KiTTY internal commands

KiTTY's *send-text* box (keyboard shortcut `input`, default **Ctrl+F8**) normally
sends the line you type to the remote host. A line starting with `/` is instead
treated as an **internal command** and executed locally by KiTTY itself. Typing
**`/help`** in the box shows the compact one-line summary of every command; this
document is the full reference. A `/line` that matches no internal command is
sent to the host as ordinary text.

The line under the input field shows what the last command did: `Done: /name`,
a setting's new state (`The size in the title: on`), a file written with its
full path (`Saved: C:\Users\...\Pictures\KiTTY++\screenshot-....png`) or why
not (`Not saved: ... (Access is denied)`), or `Not a command: sent to the host
as text`. It wraps rather than cutting a long path, and can be selected and
copied.

Each entry below notes whether the effect **persists**:

- *runtime only* — lasts until the window is closed;
- *kitty.ini* — an `[KiTTY]` (or other section) key sets the startup default
  for **all** windows;
- *session* — stored in the saved session (survives via `/save` / `/savenew`).

See [FEATURES.md](../FEATURES.md) for the surrounding features (send-text boxes,
`[Shortcuts]` and save modes).

## Window & title

### /size

Toggle the `[cols x rows]` size suffix in the window title. If the title
decorations are off entirely (see `/wintitle`), `/size` turns both the
decorations and the suffix on — "show me the size" always works.

**Persists:** runtime only; startup default via kitty.ini `size=yes`
(the key can only enable the suffix, `size=no` has no effect).

### /wintitle

Toggle the status information in the title bar (the size suffix, the
PROTECTED / ONTOP / BROADCAST / RESTRICTED markers and a program's reported
status) on or off for this KiTTY++ instance.

**Persists:** runtime only; kitty.ini `wintitle=no` disables decorations at
startup (only `no` has an effect — decorations are the default).

### /title `<text>`

Set the window title to `<text>`. The title set in the session configuration
(*Window → Behaviour*) is what persists; this override lasts until the server
or KiTTY next changes the title.

**Persists:** runtime only.

### /transparency

Toggle window transparency. When turning on, the transparency level comes from
the session's configured value (*Window → Appearance → Background*); a session with no
value configured starts fully opaque. It acts on the terminal window. A
Direct2D window opened opaque is left as it is: a Direct2D window that may be
transparent is layered from its creation, and a transparency change applies
to the next window.

**Persists:** runtime only; the level is a session setting, and kitty.ini
`transparency=yes/no` enables/disables the feature at startup.

### /backgroundimage

Toggle the background-image feature on or off.

**Persists:** runtime only; startup default via kitty.ini `bgimage=`.

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
directory, save mode, initial directory, the kitty.ini and backup-file paths, and
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
window is not bound to a saved session, the result line points to
`/savenew` instead. The result line also shows the session written, or why
not.

**Persists:** session.

### /savenew `<name>`

Save the live settings as a **new** session called `<name>` and switch this
window's identity to it (later `/save` calls write there, and the title
decorations show the new name). **Save as New Session...** in the window menu
opens Change Settings with the host (`user@host` when a session of the host's
name exists) in the session name field; its **Save** asks before it replaces
a session of that name.

**Persists:** session.

### /savektx

Export the live settings to a `.ktx` connection file (a file dialog prompts
for the location). A `.ktx` file double-clicked or passed on the command line opens that
connection directly. The result line shows the file written, or why not.

**Persists:** the exported file.

### /savemode

Cycle the configuration save mode: **registry → file → dir → registry…**.
The mode applies immediately to this instance; "file" is also written to
kitty.ini (`savemode=file`) so it survives a restart. Registry and dir modes
remove the `savemode=` key. The result line shows the new mode and whether
kitty.ini now holds it; when kitty.ini could not be written, the mode holds
for this window only and the line gives the reason.

**Persists:** kitty.ini for "file"; runtime only for the others.

### /savereg

Export the complete KiTTY registry configuration (sessions, host keys,
settings) now, whether or not anything changed since the last backup, and wait
for it. The file is a timestamped `kittynew-YYYYMMDD-HHMMSS.sav` written
beside `kitty.ini` in `%APPDATA%\KiTTY`, or at the `[KiTTY] sav=` path if one
is set, and the oldest are pruned to `[KiTTY] savbackupcount`. A portable copy
writes a dated copy of its folder store to `Backups\kitty-portable-...`
instead (`portablebackupcount`). The result line shows the file or folder
written, or that backups are switched off (a count of 0).

**Persists:** the exported file.

### /loadreg

Import the newest backup (see `/savereg`) back into the registry - the one
start-up would restore from, and only a backup of the hive in use (a copy
with `KiClassName=PuTTY` skips the backups of the default hive and the other
way round). Backups written by older KiTTY versions are
recognised and read as well. The result line shows the file loaded, or that
there was none.

**Persists:** registry.

### /delreg

**Deletes the whole KiTTY registry tree** — all saved sessions, host keys and
settings stored in the registry, after a Yes/No question that shows the key and
defaults to No. Configurations
saved in file/dir mode are not touched. Use with care; `/savereg` first makes
a backup. The result line shows the key deleted, or why it is still there.

**Persists:** destructive registry action.

### /savesessions

Export only the saved sessions (not host keys or global settings) to
`kitty.ses` beside the `/savereg` backups (`%APPDATA%\KiTTY` for an installed
copy). The result line shows the file with its full path.

**Persists:** the exported file.

### /copytoputty

Copy KiTTY's saved sessions and SSH host keys into stock PuTTY's registry
location (`Software\SimonTatham\PuTTY`). **PuTTY's existing sessions are
replaced** - only by a complete copy: the sessions are copied aside first,
and when that fails PuTTY's own sessions stay as they were. KiTTY-specific
settings are stripped from the copies. The result line shows how many
sessions were copied, or why nothing was.

**Persists:** registry (PuTTY's).

### /switchcrypt

Removed. Encrypted configuration files are no longer written; the command only
shows a notice. Existing encrypted `.ktx` files are still read.

**Persists:** nothing.

### /delfolder `<name>`

Drops `<name>` from this window's folder list in memory. Nothing is saved
(`[KiTTY] Folders` is unchanged), no session is moved or deleted, and the list
is read again from the store the next time it is loaded (configuration box,
Manage Sessions). A folder that holds sessions is listed from them anyway.
To delete a folder, use Del folder in the session list or Manage Sessions.

**Persists:** nothing.

### /loadinitscript `[file]`

(Re)load the login-script file for this session — with `[file]` given, load
that file; without, reload the session's configured script (*Connection →
Login → Login script*). The script content is stored in the session settings.

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

Register the session file extension for the current user, so double-clicking
such a file opens it with this kitty.exe. The extension is the one set in
kitty.ini (`fileextension`, shown at Application > KiTTY++ Settings > Storage &
Backup as "Session file extension"), `.ktx` when none is set; `/help` shows the
one in force. An extension another program owns is left alone and named in
the report.

**Persists:** registry (per-user file association).

### /initlauncher

Rebuild the launcher's menu from the saved sessions. The result line shows the
entries written. When some could not be written, it shows how many and the
first reason.

**Persists:** the `Launcher` key in the registry store, the `Launcher` folder
in a folder store.

### /debug

Toggle debug mode. Debug mode enables extra diagnostics (and `/passwd`).

**Persists:** runtime only; startup default via kitty.ini `debug=`.

### /passwd

**Debug mode only** (see `/debug`; without it the line is sent to the host as
plain text). Shows the session's stored password in a message box **and copies
it to the clipboard** — remember the clipboard keeps it until overwritten.

**Persists:** informational, nothing stored.

### /screenshot

Save a screenshot of the terminal window as
`screenshot-<YYYYMMDD>-<HHMMSS>.png`: an installed KiTTY++ writes to the
user's `Pictures\KiTTY++` (made when missing), a portable one next to its exe.
The result line shows the file with its full path, or why it was not written.
Written with Windows' imaging component: on XP only with the Microsoft .NET
Framework 3.0 installed.

**Persists:** the image file.
