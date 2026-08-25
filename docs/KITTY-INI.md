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
| `[KiTTY]` | The main section: feature switches (hyperlinks, transparency, icons, background image, …), `savemode`, security options (`PortablePasswordProtection`, `readonly`, `restrictacl`), window/title behaviour, scripting. |
| `[Agent]` | kageant (the SSH agent): `askconfirmation` (`yes`/`auto`/`no`/`hello` - the last one demands a Windows Hello gesture for the confirmation), `messageonkeyusage`, `loadonstartup` + the `startupkeyN` list, `retrykeys` (what to do when a startup key's media returns), the `agentlog*` settings, and `hellocacheseconds` (how long one Windows Hello unlock keeps covering further protected keys; `0` asks every time). |
| `[ConfigBox]` | Configuration-box behaviour: `dblclick` (double-click on a saved session = Open or Start), `defaultsettings` visibility, `loadlastsession` (off = quick connect: open on Default Settings with the caret in Host Name), `foldernavigation` (session folders as ROWS of the saved-session list rather than a drop-down), box height. |
| `[Shortcuts]` | Keyboard shortcuts for KiTTY menu actions, e.g. `duplicate={CONTROL}N`. |
| `[Print]` | Text printing: character size, lines per page, characters per line. |
| `[Launcher]` | The tray launcher, e.g. session-list `reload` on each menu open. |
| `[FontFallback]` | Missing-glyph font fallback: `active` master switch (default yes), `fallback` font list, `override` Unicode-range pinning, `log`/`logfile` troubleshooting. |

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
