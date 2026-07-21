# KiTTY (PuTTY 0.84 port)

**KiTTY** is a feature-rich fork of [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/),
the free Windows SSH/Telnet client. This branch is a **forward-port of the entire KiTTY feature
set onto current PuTTY 0.84** — so you get KiTTY's extras on top of a modern, security-patched
PuTTY core (≈1,200 upstream commits newer than KiTTY's original 0.76b base).

> ⚠️ **Beta release** (`0.84.1.57-beta`). The full KiTTY feature set on a modern, security-patched PuTTY 0.84 core — including a post-quantum key-exchange warning and a console CLI key generator (`kittygen-cli.exe`). Please still read the known issues below.

## Screenshots

*(click an image to enlarge)*

The configuration dialog (start KiTTY with no session):

<a href="screenshots/config.png"><img src="screenshots/config.png" width="420" alt="KiTTY configuration dialog"></a>

A terminal session with a clickable, underlined hyperlink:

<a href="screenshots/terminal.png"><img src="screenshots/terminal.png" width="600" alt="KiTTY terminal with a clickable, underlined URL"></a>

---

## ⬇️ Download

Grab the latest build from the **[Releases page →](https://github.com/hknet/KiTTY/releases/latest)**.

Current release — **[KiTTY 0.84.1.57-beta](https://github.com/hknet/KiTTY/releases/tag/kitty-0.84.1.57-beta)**:

| Download | Use it when |
|---|---|
| **[Installer — per-user (no admin)](https://github.com/hknet/KiTTY/releases/download/kitty-0.84.1.57-beta/KiTTY-0.84.1.57-beta-x64-peruser.msi)** | **Recommended.** Installs for your user only, **no UAC prompt** (`%LOCALAPPDATA%\Programs\KiTTY`). |
| **[Installer — system-wide](https://github.com/hknet/KiTTY/releases/download/kitty-0.84.1.57-beta/KiTTY-0.84.1.57-beta-x64-system.msi)** | All users, into `Program Files` (requires admin). |
| **[Portable ZIP](https://github.com/hknet/KiTTY/releases/download/kitty-0.84.1.57-beta/kitty-0.84.1.57-beta.zip)** | No install — run from a folder or USB stick. Includes `kitty_portable.exe` and all command-line tools. Executables are **not** UPX-packed (antivirus-friendly). |
| **[Portable ZIP (UPX)](https://github.com/hknet/KiTTY/releases/download/kitty-0.84.1.57-beta/kitty-0.84.1.57-beta-upx.zip)** | Same contents with `kitty.exe`/`kitty_portable.exe` UPX-packed for the smallest download. Some antivirus engines dislike UPX — if in doubt, take the standard ZIP. |

Both installers add Start-Menu + Desktop shortcuts and an Add/Remove-Programs entry, and uninstall
cleanly. Every download is checksummed (`SHA256SUMS` in the ZIP), and all executables are
Authenticode-signed.

---

## What's included (KiTTY features on PuTTY 0.84)

~46 KiTTY features are ported and verified, including:

- **Window:** transparency, maximize / fullscreen / saved position on start, always-on-top, roll-up,
  send-to-tray (auto + on-minimize), per-session icons, background image.
- **Hyperlinks:** clickable URLs in the terminal (Ctrl+click configurable) with **underlining**.
- **Sessions & automation:** auto-command after login, auto-password, anti-idle keepalive,
  port-knocking, duplicate-session, immediate-quit, session export/import, scripting (rutty),
  and an in-terminal command console (Ctrl+F8, type `/help`; see [`docs/COMMANDS.md`](docs/COMMANDS.md)).
- **Proxies & jump hosts:** reusable **named proxy** definitions (including SSH jump hosts),
  selectable per session.
- **Security:** saved passwords encrypted at rest — Windows **DPAPI** in registry mode, an opt-in
  **master password** for the portable store (travels between machines); a **post-quantum
  key-exchange warning**; a built-in **update checker** that verifies the download's
  Authenticode signature chain.
- **SSH agent (kageant):** per-key or global use-confirmation, key-use notifications, a
  remembered startup-key list, a Windows-OpenSSH-agent bridge, `kitty.ini` configuration and
  registry-free autostart for portable installs.
- **Transfers / backends:** ZModem send/receive, WinSCP & **kscp** file transfer with
  **directory-aware uploads** (OSC 7 — see the callout below), **adb** (Android) backend.
- **Terminal:** font resize, protect, print, negative/B&W colours, clear/restart log, far2l extensions.
- **Storage:** registry **or** portable file/dir storage (`kitty_portable.exe`), `kitty.ini` configuration.
- Plus the standard PuTTY tools, renamed KiTTY-style: `klink`, `kscp`, `ksftp`, `kageant`, `kittygen`.
- `kittygen-cli.exe` — a console-mode CLI key generator (generate, convert, fingerprint) for use in scripts and pipelines. Run `kittygen-cli --help` for options.

> 📂 **Put your files where your `cwd` is.** Turn on **OSC 7 directory tracking** and
> drag-and-drop uploads — and *Start WinSCP* — land in your shell's **current remote
> directory** instead of always dropping into `$HOME`. It's the safe, **data-only**
> rework of KiTTY's old "send file to the current directory" trick, which was retired
> after that mechanism turned out to be a remote-code-execution hole
> (**CVE-2024-23749**): the new one only ever *reads* a strictly-validated path and
> never runs anything the remote sends. Two lines in your shell startup do it —
> **[OSC 7 how-to →](docs/examples/osc7-shell-integration.md)**.

Most KiTTY extras read from a `kitty.ini` (`[KiTTY]` section). The release includes an inert `kitty.ini.example` with every supported key commented out; copy/rename it to `kitty.ini` only when you want an active config file. For example, URL hyperlinks are enabled with:

```ini
[KiTTY]
hyperlink=yes
```

**[`docs/KITTY-INI.md`](docs/KITTY-INI.md)** explains the settings file — how KiTTY finds it, the `savemode`/portable rules, and all its sections — and links the fully annotated [`kitty.ini.example`](docs/examples/kitty.ini.example). See **[`FEATURES.md`](FEATURES.md)** for the full feature reference, including how to enable each one, and **[`CHANGELOG.md`](CHANGELOG.md)** for what changed in each release.

---

## Known issues

This is a **beta**: most KiTTY features are restored and verified, but a few have limitations
or still want real-world testing. The full, per-release list is in
**[`KNOWN-ISSUES.md`](KNOWN-ISSUES.md)** — current highlights:

- **Antivirus / SmartScreen & UPX** — the standard ZIP ships only plain, uncompressed signed
  executables. The `-upx.zip` flavour and the installers carry UPX-packed
  `kitty.exe`/`kitty_portable.exe` (smallest download), which can trip heuristic AV — if
  flagged, use the standard ZIP.
- **far2l shared clipboard (GET)** — remote→clipboard (**SET**) is verified end-to-end; the
  **GET** direction (remote reads your clipboard) transmits over **SSH only**, not raw (a
  pre-existing PuTTY behaviour).
- **far2l clipboard "Ask" mode** (Window → Selection) — answering **OK** grants the remote
  clipboard access for the rest of the session (no per-request reprompt). Set it to **Disabled** to deny.
- **adb backend & rutty scripting** — verified against test fixtures, not yet against a real
  Android device / live remote shell.
- **Stored passwords** — saving passwords is optional; saved ones are encrypted at rest:
  Windows **DPAPI** in registry mode (bound to your account/machine), an opt-in **master
  password** in portable mode (travels between machines; it is never stored — if you forget
  it, the passwords it protected are unrecoverable). Neither defends against malware already
  running as your user; for the strongest security prefer SSH keys (kageant).
- **Command-line tools use the registry session store** — `klink`/`kscp`/`ksftp` do not read
  a portable (`savemode=dir`) store, so portable sessions and their master-password-protected
  passwords are usable from the GUI only.
- **Diagnostic dumps** — `/savedump` redacts the known high-risk secret fields (passwords,
  passphrases, key files, clipboard, login-automation strings and inline script content —
  including inside the bundled `current.ktx`).
- **Background image** — the thin margin outside the terminal cell grid is still solid-filled (cosmetic).

Found something else? Please **[open an issue](https://github.com/hknet/KiTTY/issues)**.

---

## Building from source

Cross-compiled to Win64 with MinGW + CMake (Ninja) under WSL/Linux:

```bash
cmake -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake
cmake --build build-mingw                 # whole tree
cmake --build build-mingw --target kitty  # just kitty.exe
```

The MSI installers are built from [`windows/installer/`](windows/installer/) with WiX v5.
For an orientation on how this fork is structured and ported — architecture, the `MOD_*` /
parallel-target approach, and the constraints to know before editing shared files — see
**[`PORTING.md`](PORTING.md)**.

---

## Credits & licence

- **PuTTY** © [Simon Tatham](https://github.com/sgtatham) and the PuTTY team — the upstream
  this is built on ([putty homepage](https://www.chiark.greenend.org.uk/~sgtatham/putty/)).
- **KiTTY** © Cyril Dupont (9bis) — the feature fork this port carries forward (https://www.9bis.net/kitty/).
- **far2l terminal extensions** (the far2l shared clipboard) — derived from the
  [**putty4far2l**](https://github.com/ivanshatsky/putty4far2l) project: far2l's PuTTY
  extensions originally by **Ivan Sorokin**, putty4far2l by **unxed**, the 0.78.5 port by
  **Ivan Shatsky**; the [far2l](https://github.com/elfmz/far2l) file manager by **elfmz** and contributors.
- This 0.84 port keeps PuTTY's **MIT licence** — see [`LICENCE`](LICENCE).

The original PuTTY source README is preserved as [`README`](README).
