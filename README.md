# KiTTY (PuTTY 0.85 port)

**KiTTY** is a feature-rich fork of [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/),
the free Windows SSH/Telnet client. This branch is a **forward-port of the KiTTY features and more
onto current PuTTY 0.85** — so you get KiTTY's extras on top of a modern, security-patched
PuTTY core (≈1,200 upstream commits newer than KiTTY's original 0.76b base).

> ⚠️ **Beta release** (`0.85.1.8-beta`). The full KiTTY feature set on a modern, security-patched PuTTY 0.85 core — including a post-quantum key-exchange warning and a console CLI key generator (`kittygen-cli.exe`). Please still read the known issues below.

## Screenshots

*(click an image to enlarge)*

A terminal session with a clickable, underlined hyperlink:

<a href="screenshots/terminal.png"><img src="screenshots/terminal.png" width="600" alt="KiTTY terminal with a clickable, underlined URL"></a>

The configuration of the GPU-Renderer

<img width="420" alt="KiTTY++ GPU-Renderer" src="https://github.com/user-attachments/assets/b1bdcae0-ea23-4458-a77f-d393c2210fca" />

---

## ⬇️ Download

Grab the latest build from the **[Releases page →](https://github.com/hknet/KiTTY/releases/latest)**.

Current release — **[KiTTY 0.85.1.8-beta](https://github.com/hknet/KiTTY/releases/tag/kitty-0.85.1.8-beta)**:

| Download | Use it when |
|---|---|
| **[Installer — per-user (no admin)](https://github.com/hknet/KiTTY/releases/download/kitty-0.85.1.8-beta/KiTTY-0.85.1.8-beta-x64-peruser.msi)** | **Recommended.** Installs for your user only, **no UAC prompt** (`%LOCALAPPDATA%\Programs\KiTTY`). |
| **[Installer — system-wide](https://github.com/hknet/KiTTY/releases/download/kitty-0.85.1.8-beta/KiTTY-0.85.1.8-beta-x64-system.msi)** | All users, into `Program Files` (requires admin). |
| **[Portable ZIP](https://github.com/hknet/KiTTY/releases/download/kitty-0.85.1.8-beta/kitty-0.85.1.8-beta.zip)** | No install — run from a folder or USB stick. Includes `kitty_portable.exe` and all command-line tools. Executables are **not** UPX-packed (antivirus-friendly). |
| **[Portable ZIP (UPX)](https://github.com/hknet/KiTTY/releases/download/kitty-0.85.1.8-beta/kitty-0.85.1.8-beta-upx.zip)** | Same contents with `kitty.exe`/`kitty_portable.exe` UPX-packed for the smallest download. Some antivirus engines dislike UPX — if in doubt, take the standard ZIP. |
| **[Portable ZIP — 32-bit](https://github.com/hknet/KiTTY/releases/download/kitty-0.85.1.8-beta/kitty-0.85.1.8-beta-32bit.zip)** | The same suite built for 32-bit Windows — for systems a 64-bit binary cannot reach. |
| **[ISO image](https://github.com/hknet/KiTTY/releases/download/kitty-0.85.1.8-beta/kitty-0.85.1.8-beta.iso)** | Both builds (64-bit and 32-bit) as plain files on one ISO — mount it into a virtual machine, no network or install needed. |

Both installers add Start-Menu + Desktop shortcuts and an Add/Remove-Programs entry, and uninstall
cleanly. Every download is checksummed (`SHA256SUMS` in the ZIP), and all executables are
Authenticode-signed.

### Windows versions

The floor is Windows XP: the 32-bit ZIP runs there (every program of the suite, plus SSH and
Telnet sessions, verified by an automated run on an XP SP3 virtual machine), because every
API the suite needs beyond XP is loaded dynamically with a fallback instead of being
imported, so an old system starts rather than dying in the loader.
The 64-bit builds need a 64-bit Windows from Vista / Server 2008 on. Day-to-day testing and
the QA gate run on Windows 10 and Windows 11; versions in between should run but are not
verified — reports welcome.

---

## What's included (KiTTY++ features on PuTTY 0.85)

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
- **Storage:** registry **or** portable file/dir storage (`kitty_portable.exe`), `kitty.ini` configuration,
  and a **portable copy** made from the configuration window (Migration > KiTTY storage) - or a folder
  store taken back into the registry.
- **KiTTY++ Settings:** every setting of the program itself has a panel (Application > KiTTY++ Settings),
  saved as you change it - including a **System** leaf that registers the `ssh://` / `telnet://` /
  `kitty://` links and `.ktx` files with Windows.
- Plus the standard PuTTY tools, renamed KiTTY-style: `klink`, `kscp`, `ksftp`, `kageant`, `kittygen`.
- `kittygen-cli.exe` — a console-mode CLI key generator (generate, convert, fingerprint) for use in scripts and pipelines. Run `kittygen-cli --help` for options.
- **Quick-Connect or Last-Session Mode:** set "loadlastsession=yes/no" and either get fast load the last session or your cursor set to the hostname to enter for a quick connection.
- **Double the Folder-Navigation** whatever drives you: the mouse or the keyboard the "foldernavigation=yes/no" got you covered set it to yes and you can simply click through your folders in the Session-List; set it no and Ctrl+F and Ctrl+G help you to locate the sessions you need for your next adventure.
- **Modal Box Free Work** if you don't want Message Boxes popping up if things go sideways, this KiTTY can put those notices in the terminalwindow. Switch to "modalerrors=no" and a message for another terminal can't block your work anymore. More modal-settings are in the kitty.ini and highly recommended if you hate Message Boxes.
- **Workplace Proxy Mode** working on the go you sometimes have to set a proxy for all your needs.  If you have named proxies configured you can simply activate one on the go using the launcher or by starting the workplace-proxy in the KiTTY-config-window (Connection - Proxy) and you are set.

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

**[`docs/KITTY-INI.md`](docs/KITTY-INI.md)** explains the settings file — how KiTTY finds it, the `savemode`/portable rules, and all its sections — and links the fully annotated [`kitty.ini.example`](docs/examples/kitty.ini.example). See **[`FEATURES.md`](FEATURES.md)** for the full feature reference, including how to enable each one, and **[`CHANGELOG.md`](CHANGELOG.md)** for what changed in each release. Using SSH certificates instead of per-server `authorized_keys` entries? **[`docs/SSH-CERTIFICATES.md`](docs/SSH-CERTIFICATES.md)** covers the KiTTY side and the OpenSSH server side end to end.

---

## Known issues

This is a **beta**: most KiTTY features are restored and verified, but a few have limitations
or still want real-world testing. The full, per-release list is in
**[`KNOWN-ISSUES.md`](KNOWN-ISSUES.md)** — current highlights:

- **Antivirus / SmartScreen & UPX** — the standard ZIP ships only plain, uncompressed signed
  executables. The `-upx.zip` flavour and the installers carry UPX-packed
  `kitty.exe`/`kitty_portable.exe` (smallest download), which can trip heuristic AV — if
  flagged, use the standard ZIP.
- **far2l shared clipboard, non-text formats** — both directions are verified over the
  wire (a remote writing your clipboard and reading it, including an 80 KB payload). What
  is untested is whether **non-text** formats such as images survive the round trip; only
  text has been exercised.
- **far2l clipboard "Ask" mode** (Window → Selection) — answering **OK** grants the remote
  clipboard access for the rest of the session (no per-request reprompt). Choose **Deny**
  instead if a remote `far2l` should never reach your clipboard.
- **adb backend** — verified against test fixtures, not yet against a real Android device.
- **Stored passwords** — saving passwords is optional; saved ones are encrypted at rest:
  Windows **DPAPI** in registry mode (bound to your account/machine), an opt-in **master
  password** in portable mode (travels between machines; it is never stored — if you forget
  it, the passwords it protected are unrecoverable). Neither defends against malware already
  running as your user; for the strongest security prefer SSH keys (kageant).
- **Command-line tools use the registry session store** — `klink`/`kscp`/`ksftp` do not read
  a portable (`savemode=dir`) store, so portable sessions and their master-password-protected
  passwords are usable from the GUI only.
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

## Related Projects

If you got a related project to this KiTTY++ please notify the repo and will link you here:

- **QuickPutty** (https://github.com/wiesl/QuickPutty) by friend of this repo @wiesl

---

## Credits & licence

- **PuTTY** © [Simon Tatham](https://github.com/sgtatham) and the PuTTY team — the upstream
  this is built on ([putty homepage](https://www.chiark.greenend.org.uk/~sgtatham/putty/)).
- **KiTTY** © Cyril Dupont (9bis) — the feature fork this port carries forward (https://www.9bis.net/kitty/).
- **far2l terminal extensions** (the far2l shared clipboard) — derived from the
  [**putty4far2l**](https://github.com/ivanshatsky/putty4far2l) project: far2l's PuTTY
  extensions originally by **Ivan Sorokin**, putty4far2l by **unxed**, the 0.78.5 port by
  **Ivan Shatsky**; the [far2l](https://github.com/elfmz/far2l) file manager by **elfmz** and contributors.
- **RuTTY** © 2013-2014 Ernst Dijk — the scripting patch behind *Session → Scripting*
  (`kitty/rutty/`).
- This port keeps PuTTY's **MIT licence** — see [`LICENCE`](LICENCE).

The original PuTTY source README is preserved as [`README`](README).
