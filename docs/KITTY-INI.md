# kitty.ini — the KiTTY settings file

`kitty.ini` is KiTTY's optional plain-text configuration file. It holds the
KiTTY-specific settings — most feature switches live in its `[KiTTY]` section —
and configures the companion tools (the `[Agent]` section for kageant, the
launcher, printing, the configuration box, keyboard shortcuts).

Nothing requires the file: without a `kitty.ini`, KiTTY runs with built-in
defaults and stores sessions in the Windows registry.

## The complete settings reference

Every supported key is documented — with its default and allowed values — in
the annotated **[`kitty.ini.example`](examples/kitty.ini.example)** that ships
with every release. That file is deliberately **inert**: every line is
commented out, so it never changes behaviour by itself. To use it, copy it to
`kitty.ini` next to the executable and uncomment only the settings you want.
A release-time drift check keeps the example in sync with the source, so it is
the authoritative key list.

For the features *behind* the switches — what they do and how to use them —
see **[`FEATURES.md`](../FEATURES.md)**. The internal `/commands` you can type
into the send-text box are documented in **[`COMMANDS.md`](COMMANDS.md)**.

## How KiTTY finds the file

The first match wins:

1. the file named by the **`KITTY_INI_FILE`** environment variable, if it exists;
2. **`kitty.ini` next to the executable**;
3. `putty.ini` next to the executable;
4. `%APPDATA%\KiTTY\kitty.ini` — registry mode only (auto-created on first run).

A **portable** install (`savemode=dir`) uses only the locations next to the
executable — it never falls back to `%APPDATA%`. The companion binaries
(kageant, the launcher) resolve the ini with the same order, keyed off *their*
executable's folder — so a portable stick stays self-contained even when
autostart launches them with a different working directory.

## savemode — where your sessions live

The `[KiTTY]` key `savemode` selects the session store:

| Value | Store |
|---|---|
| `registry` *(default)* | Windows registry, like stock PuTTY/KiTTY. |
| `dir` | One file per session under the install folder — the **portable** mode (`kitty_portable.exe` forces this). |
| `file` | **Not a file store, despite the name** — sessions stay in the registry exactly as in `registry` mode; all it adds is importing a `.sav` registry dump at startup when the hive is missing. Abandoned upstream and unmaintained; use `dir` if you want sessions in files. |

When kitty.ini says `savemode=file` or `savemode=dir`, the ini is the
**authoritative store** for the companion tools too: kageant's tray toggles
write back to the ini and the registry is never touched. A portable folder
counts even without the savemode line — with `savemode` absent, a `Sessions`
folder or `KiTTYState` file next to the ini also makes it authoritative.
Otherwise (`savemode=registry`, or absent with no portable layout) the
registry stays authoritative and the ini keys act as first-run defaults only.

Note: the command-line tools (`klink`/`kscp`/`ksftp`) always read the
**registry** session store — a portable `savemode=dir` store is usable from
the GUI only (see [KNOWN-ISSUES.md](../KNOWN-ISSUES.md)).

## When an edited kitty.ini appears to do nothing

Outside portable mode, a global `[KiTTY]` switch is looked up **in the registry
first**, and read from `kitty.ini` only when the registry has no value of that
name. So if a value was ever written to
`HKCU\Software\kapper.net\KiTTY`, editing the same key in `kitty.ini` has no
effect at all — the file is never consulted for it. Delete the registry value
and the ini line takes over.

Portable mode is the exception and is deliberately registry-independent: with
`savemode=dir`, global parameters are read from `kitty.ini` only, so a stale
registry value left behind by an installed copy cannot reach in.

Two other reasons a line can look dead:

- **The key is misspelt.** Keys are matched exactly; an unrecognised key is
  silently ignored rather than reported.
- **The value is not one the key accepts.** Several switches only act on one
  of `yes`/`no` and ignore the other — each such key says so in
  [`kitty.ini.example`](examples/kitty.ini.example).

## kageant and the store — the `(kitty.ini mode)` and `(portable)` markers

kageant (the SSH agent) follows the *same* store decision as the sessions,
and the same decision governs three things at once: kageant's settings, its
remembered **startup key list**, and the **offer order** of those keys. There
are three cases, and kageant's window title and tray tooltip tell you which
one you are in.

| Mode | Where settings + the key list + key order live | Markers shown |
|---|---|---|
| **Registry** *(default)* | `HKCU\...\kapper.net\KiTTY` — settings, plus the `StartupKeys` and `KeyOrder` values | *(none)* |
| **kitty.ini** | the ini file — settings, plus `[Agent] startupkeyN` and `keyorderN` | `(kitty.ini mode)` |
| **Portable** | the ini and its store, sitting **next to the exe** so they travel | `(kitty.ini mode)` **and** `(portable)` |

Reading the markers:

- **No marker** — registry mode, the default. A `kitty.ini` may still be
  present, but it is not the authoritative store; its keys act as first-run
  defaults only. Nothing kageant holds is written to the ini.
- **`(kitty.ini mode)`** — the ini is authoritative (`savemode=file`/`dir`, or
  a portable layout was found). kageant's settings, the keys it re-loads at
  startup, and their order are all read from and written to the ini, at
  whatever path the ini resolved to. The registry is not touched.
- **`(portable)`** — a stricter case of kitty.ini mode: kageant found a store
  **beside the executable** (a `Sessions\` folder or a `KiTTYState` file), which
  forces ini/dir mode. Because the store travels with the exe, a portable stick
  stays self-contained. **Portable always implies kitty.ini mode**; the reverse
  is not true — a `savemode=file` install is kitty.ini mode without being
  portable, and shows only the first marker.

So the two markers are levels, not duplicates: `(kitty.ini mode)` says *the ini
is the store*, and the extra `(portable)` says *and that store lives next to the
exe*.

## Sections at a glance

| Section | What it configures |
|---|---|
| `[KiTTY]` | The main section: feature switches (hyperlinks, transparency, icons, background image, …), `savemode`, security options (`PortablePasswordProtection`, `readonly`, `restrictacl`), window/title behaviour, scripting, `theme` (`system`/`light`/`dark` - the colours every KiTTY window paints in, kageant and kittygen included; dark needs Windows 10 1809 or newer), and `checkupdate` (look for a new release at startup), and `showforeignsessions` (also list an older KiTTY's or PuTTY's own saved sessions - `auto`/`yes`/`no`, default `auto`), and where the helper programs live on this PC: `WinSCPPath`, `rzcommand` and `szcommand`, and `warnmissingfeatures` (name in the terminal whatever this version of Windows is too old to provide), and `renderer` (`gdi` or `d2d`: how the terminal window is painted - see Terminal renderer in FEATURES.md), and `framepace` (`auto`, a number of milliseconds, or 0: how often the window may repaint while output streams in - see Frame pacing in FEATURES.md). |
| `[Agent]` | kageant (the SSH agent): `askconfirmation` (`yes`/`auto`/`no`/`hello` - the last one demands a Windows Hello gesture for the confirmation), `messageonkeyusage`, `loadonstartup` + the `startupkeyN` list, `retrykeys` (what to do when a startup key's media returns), the `agentlog*` settings, `hellocacheseconds` (how long one Windows Hello unlock keeps covering further protected keys; `0` asks every time), `autoencryptmode` + `autoencryptseconds` (re-encrypt keys after idle: `off` / `default` for keys without their own value / `enforce` for every key, and the time - seconds, `10m`, `2h`, `1d`, or `use` for right after each signature). |
| `[ConfigBox]` | Configuration-box behaviour: `dblclick` (double-click on a saved session = Open or Start), `defaultsettings` visibility, `loadlastsession` (off = quick connect: open on Default Settings with the caret in Host Name), `foldernavigation` (session folders as ROWS of the saved-session list rather than a drop-down), box height, `windowheight` and `windowwidth` (the size of the configuration window itself), `fixedsizewindow` (lock that size: no resize frame, size fields read-only), `applicationsettings` (`no` = no Application tab at all; kitty.ini only), `applicationpanel` (the Application tab's leaf, remembered between configuration windows), `switchpaint` (`erase` = paint a panel switch the old way, erase then repaint on screen, instead of the freeze frame; a diagnostic), and `collapsed` (the Category-tree folds the user changed by hand, by path - written by the window itself; they beat the categoryexpand default in both directions). |
| `[Shortcuts]` | Keyboard shortcuts for KiTTY menu actions, e.g. `duplicate={CONTROL}N`. |
| `[Print]` | Text printing: character size, lines per page, characters per line. |
| `[Launcher]` | The tray launcher, e.g. session-list `reload` on each menu open. |
| `[FontFallback]` | Missing-glyph font fallback: `active` master switch (default yes), `fallback` font list, `override` Unicode-range pinning, `log`/`logfile` troubleshooting. |

## Old PuTTY and KiTTY sessions — `[KiTTY] showforeignsessions`

A machine that has run stock PuTTY, or an older 9bis KiTTY, keeps those saved
sessions in registry hives of their own. `showforeignsessions` decides whether
KiTTY lists them alongside its own, where they can be opened, edited and
deleted. It applies to registry save modes only; a portable store has no
foreign hive to read.

* `auto` — the default. List them only while this KiTTY has no sessions of
  its own, so an upgrade never opens onto an apparently empty list and the old
  sessions retire themselves once you have your own. This is what KiTTY did
  before the key existed.
* `yes` — always list them.
* `no` — never list them.

The switch is in the configuration box under **Application > Migration**, which
appears only on a machine that actually has such a hive with sessions in it.
Ticking it records an explicit choice, and that choice then wins over this key.

The key does not change what happens by default - it exists so the answer can
be pinned. `yes` suits a machine that will go on using both; `no` suits one
where the old hive is history and its sessions are noise.

## Update check — `[KiTTY] checkupdate`

`checkupdate=yes` (the default) makes KiTTY look for a newer release when a
session starts and print a one-line notice at the top of the terminal if there
is one. The fetch is asynchronous and the terminal is touched once, at the
clean top of the session, so a full-screen program is never corrupted by it.

It is an **application** setting, and it did not use to be: the answer lived in
every saved session as `CheckUpdateStartup`, so which session you opened first
decided whether KiTTY checked. Saving a session now deletes that key, and the
old values drain out of the store as sessions are touched. Nothing is migrated,
because there was nothing sensible to migrate from - each session carried its
own answer.

The switch is in the configuration box under **Application > Updates**, beside
**Check for updates now**, which runs the check on demand whatever this is set
to.

## Quick connect — `[ConfigBox] loadlastsession`

By default the configuration box opens pre-filled with the session you used
last. If you mostly connect by **typing an address**, that is the wrong starting
point: the settings that arrive belong to whichever host you visited last.

```ini
[ConfigBox]
loadlastsession=no
```

With this, every start opens on **Default Settings** with the cursor already in
*Host Name (or IP address)* and its contents selected — type, press Enter, and
the connection uses the same known configuration every time.

You do not have to set anything to get this occasionally: **load "Default
Settings" once**. KiTTY remembers it like any other session and comes up in
quick connect from then on, until you load a different session. Typing an
address into an unsaved session records nothing, so the mode survives
connecting — it is left by loading a session, not by switching a setting back.

## The size of the configuration window — `[ConfigBox] windowheight`, `windowwidth`

The configuration window can be dragged to whatever size suits you, and it
opens at that size next time. The two keys are where that size is kept, in
pixels:

```ini
[ConfigBox]
windowheight=800
windowwidth=520
```

They are the same setting as the two fields on **Application > Config
window** — dragging the window fills those fields in, and typing a number into
them resizes the window you are looking at. Leave a key out (or set it to 0)
and that dimension is whatever the window's own layout asks for.

The numbers are *logical* pixels, so the same file gives the same apparent size
on a display scaled to 150% as on one at 100%. Neither can make the window
smaller than its own minimum.

`fixedsizewindow=yes` locks the size: the window loses its resize frame, and
the two fields on **Application > Config Window** refuse
edits until the **Lock window size** box there is cleared again. With no size
set, the window keeps the size its own layout gives it.

`[ConfigBox] height` is a different thing: it is the saved-session list's
length in ROWS, and because that list is built into the Session panel it takes
effect in the next configuration window rather than the open one.

## No Application tab — `[ConfigBox] applicationsettings`

```ini
[ConfigBox]
applicationsettings=no
```

Removes the **Application** tab from the configuration window: none of its
panels is built, `-cfgpanel Application/...` and the buttons that jump there
("Edit named proxies", the workplace-proxy notice) open the Session tab
instead. There is deliberately no checkbox for it - a switch that keeps users
out of the application settings cannot live among them - so it is set here,
and an administrator who also denies write access to kitty.ini has closed the
door. `[KiTTY] readonly=yes` is not a substitute: it stops the file being
written, but the tab still shows and edits there are accepted on screen and
dropped. Default `yes`.

## Helper programs — `WinSCPPath`, `rzcommand`, `szcommand`

Where WinSCP and the ZModem helpers (`rz.exe` / `sz.exe` from lrzsz) are
installed:

```ini
[KiTTY]
WinSCPPath=C:\Program Files\WinSCP\WinSCP.exe
rzcommand=C:\Tools\lrzsz\rz.exe
szcommand=C:\Tools\lrzsz\sz.exe
```

These are properties of the machine, not of a connection, so they live here
and every session shares them. Set them on **Application > External tools**,
which has a leaf per tool.

Everything else about those tools stays per session, because it describes the
remote rather than this PC: WinSCP's protocol, SFTP connect string and extra
options on *Connection > SSH > WinSCP*, and the ZModem options and download
folder on *Connection > ZModem*.

⚠️ `rzcommand` and `szcommand` used to be per-session settings
(`rzCommand` / `szCommand` in a saved session). Those values are no longer
read: set the path once here instead.

## A minimal example

```ini
[KiTTY]
savemode=dir
hyperlink=yes

[Agent]
askconfirmation=auto
```

## Retired keys

Classic-KiTTY keys that were documented but never (or no longer) had an
effect were removed rather than silently accepted; the list is in
[FEATURES.md](../FEATURES.md) under *retired kitty.ini settings*. If a retired
key mattered to your workflow, please open an issue — where feasible the
wiring gets restored, as already done for `noexit`, `scriptmode`, `size`,
`wintitle` and `dblclick`.
