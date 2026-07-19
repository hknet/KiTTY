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
| `file` | Single-file store — legacy, not maintained; prefer `dir`. |

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

## Sections at a glance

| Section | What it configures |
|---|---|
| `[KiTTY]` | The main section: feature switches (hyperlinks, transparency, icons, background image, …), `savemode`, security options (`PortablePasswordProtection`, `readonly`), window/title behaviour, scripting. |
| `[Agent]` | kageant (the SSH agent): `askconfirmation` (`yes`/`auto`/`no`), `messageonkeyusage`, `loadonstartup` + the `startupkeyN` list. |
| `[ConfigBox]` | Configuration-box behaviour: `dblclick` (double-click on a saved session = Open or Start), `defaultsettings` visibility, box height. |
| `[Shortcuts]` | Keyboard shortcuts for KiTTY menu actions, e.g. `duplicate={CONTROL}N`. |
| `[Print]` | Text printing: character size, lines per page, characters per line. |
| `[Launcher]` | The tray launcher, e.g. session-list `reload` on each menu open. |
| `[FontFallback]` | Missing-glyph font fallback: `active` master switch (default yes), `fallback` font list, `override` Unicode-range pinning, `log`/`logfile` troubleshooting. |

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
