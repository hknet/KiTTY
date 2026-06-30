# KiTTY features

KiTTY is a fork of [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/),
Simon Tatham's telnet/SSH client for Windows. On top of everything PuTTY does,
KiTTY adds a large set of convenience and automation features. This page documents
the features that are built into this 0.84-based port, grouped the same way as the
original [KiTTY website](https://github.com/cyd01/KiTTY/) by Cyril Dupont.

Each entry includes a short description, how to turn it on, and a screenshot where
one is available.

**Table of contents**

- **Most requested features**
  - [Sessions filter (folders)](#sessions-filter-folders)
  - [Portability](#portability)
  - [Shortcuts for pre-defined commands](#shortcuts-for-pre-defined-commands)
  - [Session launcher](#session-launcher)
  - [Automatic logon script](#automatic-logon-script)
  - [Automatic logon script (RuTTY patch)](#automatic-logon-script-rutty-patch)
  - [URL hyperlinks](#url-hyperlinks)
- **SSH and network**
  - [Automatic password](#automatic-password)
  - [Private-key usage confirmation](#private-key-usage-confirmation)
  - [Post-quantum key-exchange warning](#post-quantum-key-exchange-warning)
  - [Command-line key generator (kittygen-cli)](#command-line-key-generator-kittygen-cli)
  - [kageant — Windows OpenSSH agent integration](#kageant--windows-openssh-agent-integration)
  - [kageant — load keys on startup](#kageant--load-keys-on-startup)
  - [kageant — reorder loaded keys](#kageant--reorder-loaded-keys)
  - [Port knocking](#port-knocking)
  - [Proxy choice](#proxy-choice)
  - [SSH handler (URL/OS integration)](#ssh-handler-urlos-integration)
- **Technical features**
  - [Automatic command](#automatic-command)
  - [Force CR/LF on the Enter key](#force-crlf-on-the-enter-key)
  - [Run a locally saved script on a remote session](#run-a-locally-saved-script-on-a-remote-session)
  - [Standard output to the clipboard](#standard-output-to-the-clipboard)
- **Graphical features**
  - [An icon for each session](#an-icon-for-each-session)
  - [Send to tray](#send-to-tray)
  - [Transparency](#transparency)
  - [Protection against keyboard input](#protection-against-keyboard-input)
  - [Roll-up](#roll-up)
  - [Always visible](#always-visible)
  - [Font management](#font-management)
  - [Word navigation modifier](#word-navigation-modifier)
  - [Quick start of a duplicate session](#quick-start-of-a-duplicate-session)
  - [Background image](#background-image)
- **Other features**
  - [Automatic saving](#automatic-saving)
  - [pscp.exe and WinSCP integration](#pscpexe-and-winscp-integration)
  - [Binary compression](#binary-compression)
  - [Clipboard printing](#clipboard-printing)
  - [Start Cygwin or cmd.exe inside KiTTY](#start-cygwin-or-cmdexe-inside-kitty)
  - [File association](#file-association)
  - [ZModem file transfer](#zmodem-file-transfer)
  - [Savedump (diagnostic dump)](#savedump-diagnostic-dump)
  - [Menu key shortcuts definition](#menu-key-shortcuts-definition)
  - [New command-line options](#new-command-line-options)
  - [In-app updater (Check for updates)](#in-app-updater-check-for-updates)
- **Bonus**
  - [Hidden text editor](#hidden-text-editor)

---

## Most requested features

### Sessions filter (folders)

If you manage a large number of saved sessions, KiTTY lets you organize them into folders, for example one folder per machine, per environment, or per type of application. A dropdown in the Session panel lets you pick a folder so the saved-session list shows only the sessions it contains, making a long list far easier to navigate. You can also filter the list as you type, matching sessions by name or hostname, and clear any active filter to see everything again.

**How to enable:** Automatic in KiTTY mode: the Session panel shows a **Folder** dropdown that filters the saved-session list to one folder, plus New/Del/Up folder buttons. Create a folder by typing its name and clicking *New folder*.

![Sessions filter (folders)](docs/features/img/config_folder.jpg)

*See also: [How session folders work (PDF)](docs/features/kitty-folders_list_feature.pdf)*

### Portability

By default KiTTY stores its configuration in the Windows registry. In **portable mode** it instead keeps each **saved session as a file** in a `Sessions\` folder next to the executable, so you can carry KiTTY and its sessions on a USB stick. As of 0.84.1.38 each session is one file (`Sessions\<name>`), and any auto-login password inside it is DPAPI-encrypted just like the registry path.

**Current scope (this port):** only **saved sessions** are file-based so far. **SSH host keys and the random seed still use the registry**, so portable mode is not yet 100% registry-free, and a DPAPI-encrypted password is machine-bound (it will not decrypt if you copy the folder to another PC — a portable master-password option is planned).

**How to enable:** Use the dedicated **kitty_portable.exe** (defaults to file mode), or place a `kitty.ini` next to `kitty.exe` containing `[KiTTY]` then `savemode=dir`.

(no screenshot)

### Shortcuts for pre-defined commands

KiTTY lets you define your own list of pre-defined commands that appear in a dedicated **User Command** menu, reachable by holding Ctrl and right-clicking anywhere inside a KiTTY window. Each command you define is automatically assigned a keyboard shortcut (Ctrl+Shift+A, Ctrl+Shift+B, and so on, in the order they were created), so you can fire frequently used commands instantly. You can add as many commands as you like, and define them globally, per saved session, or per session folder. If the shortcuts ever clash with another program running inside the window (for example Midnight Commander), you can turn them off by adding `shortcuts=no` under a `[KiTTY]` section in your `kitty.ini` file.

**How to enable:** Define commands under the `Commands` registry/ini key (global, per-folder, or per-session). They appear in the **User Command** menu and each gets a **Ctrl+Shift+<letter>** shortcut.

![Shortcuts for pre-defined commands](docs/features/img/menu_shortcuts.jpg)

### Session launcher

The session launcher gives you a quick way to open your saved sessions without digging through menus. It lives in the system tray and lists your sessions organized into menus and sub-menus that mirror your session folders, using the backslash (\) as the separator. By default it rebuilds its menu from your saved sessions each time it starts, but you can also arrange the menu yourself and keep it fixed. It also includes an **Opened sessions** menu that lets you hide and unhide running sessions, removing them from the desktop and taskbar when you have too many open at once.

**How to enable:** Run **`kitty.exe -launcher`** to open a small quick-launch window listing your saved sessions.

You can keep individual sessions out of the launcher menu while leaving them in the normal session list: tick **"Hide this session from the launcher"** in the session's **Session** panel.

![Session launcher](docs/features/img/ex_launcher.jpg)

### Automatic logon script

KiTTY can automatically respond to a server's login prompts using a simple challenge-and-response script. You write a plain text file that alternates lines: an expected piece of text the server prints (such as `login:` or `password:`), followed by the text KiTTY should send in reply. This is handy for automating connections to passive protocols like telnet, where you can have your username and password sent for you. Note that it cannot handle SSH authentication, since SSH builds authentication into the protocol itself rather than exchanging plain prompts.

**How to enable:** Configuration > **Connection > Data**: set *Login script file* / *Login script content*, or launch with **`kitty.exe -loginscript <file>`**. The script's expect/answer lines auto-respond to the server's login prompts.

(no screenshot)

### Automatic logon script (RuTTY patch)

Based on the RuTTY patch, this lets you automate actions on a session by running a small script as soon as you connect. The script uses simple waitfor/halton commands to watch for text from the server and send responses, so common logon sequences and repetitive steps happen for you automatically. It is a handy way to script logins and routine interactions without typing them each time.

**How to enable:** Configuration > **Connection > Scripting**: set a RuTTY script (waitfor/halton style). It plays automatically once connected.

![Automatic logon script (RuTTY patch)](docs/features/img/config_rutty.jpg)

### URL hyperlinks

KiTTY can detect URLs in the terminal output and turn them into clickable hyperlinks, so you can jump straight to a web address without copying and pasting it. You decide how links behave, including whether they are underlined, which modifier key activates them, and which browser opens them. This makes it quick to follow links that appear in logs, command output, or chat sessions.

**How to enable:** Configuration > **Window > Hyperlinks**: enable, choose underline/modifier/browser. Ctrl+click a URL in the terminal to open it.

![URL hyperlinks](docs/features/img/config_hyperlinks.jpg)

---

## SSH and network

### Automatic password

KiTTY can log you in automatically to telnet, SSH-1 and SSH-2 servers by storing a password alongside the session. For SSH connections the password is supplied during authentication; for telnet it is sent once the connection comes up, just as if you typed it, and you can even send several lines (for example a login name, a password, and a command) by separating them with `\n`. Because the stored value is tied to the host, a password cannot be saved in a session that has an empty hostname.

**How to enable:** Configuration > **Connection > Data > Auto-login password**. It is stored with the session and sent automatically at SSH login. Tick **Show password** beside the field to reveal the stored value. As of 0.84.1.38 the password is **encrypted at rest with Windows DPAPI** (tied to your Windows account), rather than stored reversibly; existing/legacy passwords still load and are re-encrypted on the next save. NOTE: a one-time security warning still appears when you set one. DPAPI is machine-bound (it defeats offline/cross-user theft, not same-user malware, and does not move to another PC) — for the strongest security, prefer SSH public-key auth (kageant).

![Automatic password](docs/features/img/config_password.jpg)

### Private-key usage confirmation

When you store private keys in KiTTY's key agent (kageant), you can require an explicit confirmation each time a key is used. With this enabled, every session that needs the key triggers a pop-up asking you to approve its use before authentication proceeds, giving you a clear chance to spot and refuse unexpected sign-in attempts. This adds a helpful safeguard against a loaded key being used without your knowledge. Thanks to [Patrick Cernko](https://people.mpi-klsb.mpg.de/~pcernko/pageant.html) for this patch.

**How to enable:** Generate a key whose **comment contains the word `confirmation`** (in kittygen), then load it into **kageant.exe**. Each time a session uses that key, kageant asks you to confirm.

![Private-key usage confirmation](docs/features/img/config_kittygen.jpg)
![Private-key usage confirmation](docs/features/img/ex_kageant.jpg)


### kageant — Windows OpenSSH agent integration

kageant (KiTTY's SSH agent) can act as the agent for the **Windows OpenSSH client** (`ssh.exe`), so the keys you load in kageant are usable by `ssh`, `git`, `scp` and any tool that uses Windows OpenSSH. When enabled, kageant writes `%USERPROFILE%\.ssh\kageant.conf` (an `IdentityAgent` line pointing at its named pipe) and adds a marker-delimited managed block to `%USERPROFILE%\.ssh\config` that includes it. It is **off by default** so kageant never alters your SSH configuration unless you ask, and only its own marker block is touched (the rest of `~/.ssh/config` is preserved byte-for-byte, written atomically, with a one-time `config.kageant.bak` backup).

**How to enable:** right-click the kageant tray icon → **Register as Windows OpenSSH agent**. Untick to remove the managed block again.

(no screenshot)

### kageant — load keys on startup

kageant can remember the keys you load and re-add them automatically at the next login, added **encrypted/deferred** (the passphrase is only requested the first time a key is actually used). It auto-tracks the file paths of the keys you load; enabling the option also installs an autostart entry so kageant starts at login — replacing the need for a hand-made Startup shortcut. Only key-file *paths* are stored, never passphrases or key material.

**How to enable:** right-click the kageant tray icon → **Load keys on startup**.

(no screenshot)

### kageant — reorder loaded keys

kageant offers its loaded keys to a server in list order, and the server tries them in turn — so the order matters when you hold several keys (offering the wrong ones first can even hit a server's "too many authentication failures" limit before the right key is reached). The key-list window has **Move Up** / **Move Down** buttons to set that offer order, e.g. to put your most-used key first. The chosen order is saved by key fingerprint and restored on the next start, including when *Load keys on startup* is enabled.

**How to enable:** in the kageant key-list window, select a key and use the **Move Up** / **Move Down** buttons.

(no screenshot)

### Post-quantum key-exchange warning

When you connect to an SSH server, KiTTY checks whether the negotiated key-exchange algorithm is one of the post-quantum hybrid algorithms (mlkem768x25519, mlkem768nistp256, mlkem1024nistp384, sntrup761x25519). If the server does not support any of these — which is common on older or unpatched servers — the key exchange falls back to a classical algorithm. KiTTY prints a warning to the terminal at connection time so you know the session is not protected against "harvest now, decrypt later" attacks.

This mirrors the behaviour added in OpenSSH 10.1/10.2 and is enabled by default.

**How to enable:** On by default. To turn it off: **Connection > SSH > Kex > Warn if Key Exchange is not post-quantum secure** (uncheck). The warning appears once per session (not on rekey).

(no screenshot)

### Command-line key generator (kittygen-cli)

`kittygen-cli.exe` is a console-mode SSH key generator that brings the full `puttygen` CLI to Windows. The existing `kittygen.exe` is a GUI tool only; `kittygen-cli` lets you generate, convert, and inspect keys from a script, a CI pipeline, or any Windows console without opening a GUI window.

Supported operations:

| What | Example |
|---|---|
| Generate Ed25519 key | `kittygen-cli -t ed25519 --new-passphrase NUL -o mykey.ppk` |
| Generate RSA 3072 key | `kittygen-cli -t rsa -b 3072 --new-passphrase NUL -o mykey.ppk` |
| Export PPK → OpenSSH | `kittygen-cli mykey.ppk -O private-openssh -o mykey` |
| Export OpenSSH → PPK | `kittygen-cli mykey -O private -o mykey.ppk` |
| Show fingerprint | `kittygen-cli -l mykey.ppk` |
| Show public key | `kittygen-cli -O public-openssh mykey.ppk` |
| Change passphrase | `kittygen-cli mykey.ppk -P --new-passphrase newpass.txt -o mykey.ppk` |

The full set of key types, output formats, and Argon2 KDF options from upstream PuTTY are all available. Run `kittygen-cli --help` for the complete list.

`kittygen-cli.exe` is included in the installer and the release ZIP alongside `kittygen.exe`. It does not have a Start-menu shortcut (it is a command-line tool; add it to your `PATH` for convenience).

(no screenshot)

### Port knocking

Port knocking lets you hide a server's SSH port behind a secret sequence of connection attempts, so the real service stays closed to anyone who doesn't know the pattern. KiTTY can send this knock sequence automatically just before it opens the actual connection, making it easy to reach servers that are otherwise locked down against attacks from the internet. You define the sequence as a comma-separated list, and KiTTY performs the knocks for you each time you connect.

**How to enable:** Configuration > **Connection > Port knocking**: define the sequence of host:port knocks sent before the real connection is opened.

(no screenshot)

### Proxy choice

When you regularly reach hosts through a bastion or jump server, Proxy choice saves you from setting up the same proxy by hand for every session. You define a set of named proxies once, then pick the one you want straight from a dropdown in the main Session panel of the configuration box. This is handy when you keep a SOCKS proxy open on a bastion (for example via a dynamic port forward) and want your sessions to route through it without manually editing the Connection/Proxy panel each time.

**How to enable:** Add `[ConfigBox]` then `proxyselection=yes` to kitty.ini, define named proxies under the `Proxies` key, then pick one from the **Proxy choice** dropdown in the Session panel.

![Proxy choice](docs/features/img/config_proxychoice.jpg)

### SSH handler (URL/OS integration)

KiTTY can register itself with Windows as the program that opens **putty://**, **telnet://**, and **ssh://** links. Once registered, clicking such a link in your browser (Internet Explorer, Firefox, or any other) or in another application launches KiTTY and connects to the target host automatically. This makes it easy to publish clickable connection links on intranet pages or in documentation, so colleagues can start a session with a single click.

**How to enable:** Run **`kitty.exe -sshhandler`** as administrator to register KiTTY as the telnet:// ssh:// putty:// URL protocol handler.

(no screenshot)

---

## Technical features

### Automatic command

KiTTY can send a command to the server automatically as soon as a Telnet or SSH connection is established, saving you from typing the same startup command every time you log in. You can send several commands at once by separating them with the two characters `\n`. A few special sequences let you pace the input: `\p` waits one second (repeat it for longer pauses), `\s05` pauses for five seconds (use any value), and `\\` sends a literal backslash. The delays involved can be fine-tuned through the autocommand settings if you need finer control over timing.

**How to enable:** Configuration > **Connection > Data > Auto-command**: a command sent to the server automatically right after login.

![Automatic command](docs/features/img/config_autocommand.jpg)

### Force CR/LF on the Enter key

By default, pressing Enter sends a single carriage return to the remote host. In some situations it's useful to send a full CR+LF line ending instead, which is what certain servers and devices expect to recognise the end of a line. This option lets you force that behaviour so your input is interpreted correctly.

**How to enable:** Tick **Terminal > 'Enter key sends CR LF'** (session key `EnterSendsCrLf`). When on, pressing Enter sends CR+LF instead of CR only — for servers that need both.

![Force CR/LF on the Enter key](docs/features/img/config_forcecrlf.jpg)

### Run a locally saved script on a remote session

KiTTY can take a script file stored on your local PC and replay its contents into the currently connected remote session. The lines from the file are sent to the remote machine as if you had typed them yourself, so you can automate repetitive command sequences without retyping them each time. This is handy for setup routines, repeated diagnostics, or any series of commands you run often on a server.

**How to enable:** Use the scripting menu / Connection > Scripting to play a locally stored script line-by-line into the active remote session.

(no screenshot)

### Standard output to the clipboard

KiTTY can route a session's terminal output straight into the Windows clipboard. By treating the clipboard as a kind of printer, you can capture the result of any remote command and paste it directly into another Windows application, with no manual selecting or copying. It's a handy way to grab a directory listing, a config file, or any command output and reuse it locally.

**How to enable:** Pick **'Windows clipboard'** as the printer in **Terminal > printing** (or tick *Print to clipboard*). Then send terminal output to the clipboard with the ANSI printer-controller sequence: `printf '\e[5i'; cat file; printf '\e[4i'`.

![Standard output to the clipboard](docs/features/img/StdoutToClipboard.png)

---

## Graphical features

### An icon for each session

KiTTY lets you assign a distinct window icon to each saved session, so you can tell your terminals apart at a glance in the taskbar and on screen. You can pick from a large set of built-in icons (more than fifty, ranging from PuTTY-style logos to numbered and cartoon icons) or point to your own .ico file. There's even a "random icon" option that picks a different one for you each time.

**How to enable:** Configuration > **Window > Appearance**: choose a per-session icon (from the embedded icon set or a .ico file).

![An icon for each session](docs/features/img/config_icon.jpg)

### Send to tray

When you run long background batches or just keep KiTTY open to maintain SSH tunnels, you can tuck the window away into the Windows system tray (the notification area in the bottom-right corner of the screen) so it stays out of your way. You can send an open session to the tray on demand, have a session start there automatically, or launch one straight into the tray from the command line. Clicking the tray icon brings the window back when you need it.

**How to enable:** System menu **Send to tray** (or enable auto-minimise-to-tray). The window hides to the notification area; click the tray icon to restore it.

![Send to tray](docs/features/img/config_sendtotray.jpg)

### Transparency

KiTTY lets you make a terminal window see-through, so you can watch what's happening behind it while you work. You set how transparent the window is, and you can fine-tune the level on the fly using the numeric keypad: **CTRL +** (or **CTRL+UP**) makes the window more opaque, while **CTRL -** (or **CTRL+DOWN**) makes it more transparent. The setting can be defined separately for each session. Note that transparency may interfere with certain window-management or screen-capture tools, so leave it off if you rely on those.

**How to enable:** Configuration > **Window > Transparency** (set the level), and the system-menu **Transparency +/-** items to adjust it live.

![Transparency](docs/features/img/config_transparency.jpg)

### Protection against keyboard input

KiTTY lets you shield a session against accidental or unintended keystrokes. When you turn on **Protect**, the terminal window ignores all keyboard input, so a stray key press can't disturb a running command or important output. You can also toggle this mode with the **CTRL+F9** key combination, and the window title changes while protection is active so you can tell at a glance that the session is locked.

**How to enable:** System menu **Protect** — locks the keyboard so accidental keystrokes can't reach the session.

![Protection against keyboard input](docs/features/img/ex_protected.jpg)

### Roll-up

Roll-up makes the window collapse "into" its title bar, hiding the terminal area so only the title bar remains visible. It's a handy way to save screen space and keep your desktop tidy when you have several windows open, and you can expand the window again whenever you need it. Besides the menu item, you can also roll up by pressing CTRL+F12, or by holding CTRL and clicking the title bar with the left mouse button.

**How to enable:** System menu **Roll-up** — shades the window down to just its title bar (click again to restore).

(no screenshot)

### Always visible

Always visible keeps a KiTTY window in the foreground, on top of all your other windows, so you can keep an eye on it while you work elsewhere. This is handy for monitoring a session, a log, or a long-running command without it slipping behind other applications. You can toggle it from the system menu, or with the **CTRL+F7** keyboard shortcut.

**How to enable:** System menu **Always visible** — keeps the window on top of other windows (always-on-top).

(no screenshot)

### Font management

KiTTY adds a **Font settings** option to the main menu that lets you adjust the terminal's appearance on the fly. From here you can increase or decrease the font size, switch to negative colors, and toggle between black-on-white and white-on-black backgrounds. Font size can also be changed quickly by holding **CTRL** and scrolling the mouse wheel.

**How to enable:** System menu **Font Up / Font Down** to resize the terminal font on the fly.

![Font management](docs/features/img/ex_fonts.jpg)

### Word navigation modifier

In a terminal, jumping the cursor a whole word left/right is driven by an xterm
escape sequence that the remote shell binds to *backward-word* / *forward-word*.
PuTTY emits it on **Alt + ←/→**. KiTTY adds a setting to choose which modifier
sends it — **Alt** (the default, unchanged), **Ctrl**, or **Both** — so you can do
word navigation with **Ctrl + ←/→** if that matches your shell bindings or muscle
memory.

**How to enable:** Configuration box → **Terminal → Keyboard → "Word navigation
(Left/Right arrows)"**, pick Alt / Ctrl / Both. Applies in the default
xterm-bitmap arrow-key mode.

(no screenshot)

### Quick start of a duplicate session

KiTTY lets you instantly open a second window that inherits all of the current session's settings, so you can run another connection to the same host without reopening the launcher or re-entering details. For an even faster shortcut, hold **CTRL + SHIFT** and click the middle of the active window with the **left mouse button** to launch the duplicate.

**How to enable:** System menu **Duplicate Session** — opens a new window with the current session's settings.

(no screenshot)

### Background image

KiTTY can display a picture behind your terminal text, giving each session window a custom backdrop. It supports BMP and JPEG images, and you can adjust how strongly the image shows through with an opacity setting or rotate through several pictures as a slideshow. This feature grows out of the covidimus patch integrated into KiTTY.

**How to enable:** Add `bgimage=yes` to `[KiTTY]` in kitty.ini (the key is `bgimage`, not `backgroundimage`), then configure **Window > Back.Image** (image file, opacity, slideshow).

![Background image](docs/features/img/ex_background.jpg)

---

## Other features

### Automatic saving

KiTTY stores all of its configuration (sessions, host keys, and parameters) in the Windows registry. To keep that configuration safe, KiTTY automatically saves a backup copy every time you change settings and close the configuration dialog. The backup is written to **kitty.sav**, kept alongside kitty.ini. The first time you run KiTTY, it also picks up any existing sessions defined for PuTTY so you don't have to recreate them.

**How to enable:** Automatic in registry mode: each time you apply the configuration dialog, KiTTY exports its registry hive to **kitty.sav** (in %APPDATA%\KiTTY, or the `[KiTTY] sav=` path) as a safety backup.

(no screenshot)

### In-app updater (Check for updates)

KiTTY can check whether a newer release is available and install it for you. *Check for updates* queries the official release list and, if a newer build exists, fetches the right asset for **how KiTTY was installed**: the per-user MSI, the system MSI (with an elevation prompt), or — for a **portable** copy — it just opens the download page. The downloaded installer runs only after it passes an **Authenticode check** (valid signature chain *and* the expected KAPPER publisher), and it is held locked against modification from verification through launch; anything that fails verification is deleted and never run. A **stable** build will not silently install a **beta**: if the newest available build is a beta, KiTTY tells you and asks first (proceed with caution).

**How to enable:** system menu → **Check for updates**. Requires network access and honours your system/IE proxy settings; if the check can't complete it falls back to opening the releases page.

(no screenshot)

### pscp.exe and WinSCP integration

KiTTY lets you transfer files without opening a separate program, reusing the host and credentials of your current session. From the system menu you can send a file straight into the running session with pscp (CTRL+F3) or open a full WinSCP file-transfer session on the same server (SHIFT+F3), and you can also drag and drop a file or folder directly onto the terminal window to upload it. Helper shell functions are available so that, from within a UNIX session, you can grab a file or launch WinSCP in your current directory with a single command.

**How to enable:** Configuration: set the WinSCP/pscp path (SCP/WinSCP options). System-menu items then launch a file transfer reusing the session's host and credentials.

![pscp.exe and WinSCP integration](docs/features/img/config_winscp_integration.jpg)

### Binary compression

To keep the download small, KiTTY's executables are compressed with UPX, the Ultimate Packer for eXecutables. This shrinks the binary file size without changing how the program runs, so you get the same KiTTY in a more compact file. The compression is transparent in everyday use.

**How to enable:** No action needed: the shipped kitty.exe and kitty_portable.exe are UPX-compressed for a smaller download; identical uncompressed `*_nocompress.exe` variants are provided in the ZIP as a fallback.

(no screenshot)

### Clipboard printing

KiTTY lets you send text straight from the terminal screen to a printer. Use the mouse to select the section of text you want, then trigger the print action and the selected contents are sent to your printer device. It's a quick way to get a hard copy of command output, logs, or any on-screen text without saving a file first. You can also invoke it with the **Shift+F7** shortcut.

**How to enable:** System menu **Print clipboard** — sends the current clipboard contents to a printer.

(no screenshot)

### Start Cygwin or cmd.exe inside KiTTY

KiTTY can host a local shell right inside its terminal window, so you can run a Cygwin session, the Windows `cmd.exe` prompt, or even PowerShell without leaving KiTTY. This is handled by a small helper called `cygtermd.exe`, which you place in your Cygwin `/bin` directory (or alongside `kitty.exe` together with `cygwin1.dll` if you don't have a full Cygwin install). When launching `cmd.exe` this way, remember to pick the matching code page in the Translation settings (or via the `-codepage` option), and you can combine cygtermd with the winpty tool to run `cmd.exe` or PowerShell. With thanks to lars18th for the help.

**How to enable:** Run a local shell via the cygtermd helper, e.g. `kitty.exe -localproxy "C:\cygwin64\bin\cygtermd.exe /home/%USERNAME% /bin/bash -login" localhost`.

(no screenshot)

### Local terminal (kitty_pterm)

`kitty_pterm.exe` is KiTTY's terminal **emulator** running a **local shell** instead of a network connection. You get the exact same terminal as a KiTTY SSH window — fonts, colour schemes, mouse selection/copy-paste, scrollback, clickable URLs, transparency — but the backend is a local process (via the Windows ConPTY pseudo-console). Its value is consistency: your local shell looks and behaves just like your remote sessions. It is an interactive terminal, not a scripting tool, and has no special tie to saved sessions beyond sharing the appearance.

**How to change the shell (cmd → PowerShell):** by default it runs `cmd.exe`. Launch it with `-e` to run a different shell, e.g. `kitty_pterm.exe -e powershell.exe` (Windows PowerShell) or `kitty_pterm.exe -e pwsh.exe` (PowerShell 7). For a saved session, set the same command in **Connection → Data → "Remote command"**.

(no screenshot)

### File association

KiTTY can export the settings of your running session to a plain-text file with the **.ktx** extension, using the **Export current settings** item in the main menu. Once the **.ktx** file type is associated with KiTTY, you can launch any saved session simply by double-clicking its file. This makes it easy to share ready-to-run sessions or keep handy shortcuts to the connections you use most.

**How to enable:** Run **`kitty.exe -fileassoc`** as administrator to associate KiTTY session files (.ktx) with KiTTY.

(no screenshot)

### ZModem file transfer

KiTTY integrates ZModem support (originally from LePuTTY) so you can transfer files directly over an interactive terminal session. With the rz/sz helper tools in place, you trigger a receive or upload straight from the menu and move files to and from the remote host without opening a separate file-transfer client.

**How to enable:** Add `zmodem=yes` to `[KiTTY]` in kitty.ini and set the rz/sz (lrzsz) helper paths in **Connection > ZModem**. The Tools menu then offers **ZModem Receive / Upload / Abort**.

![ZModem file transfer](docs/features/img/config_zmodem.jpg)

### Savedump (diagnostic dump)

When you run into a problem and want help diagnosing it, KiTTY can capture a diagnostic dump of its current state. Reproduce the issue in your session, then trigger a dump to write a kitty.dmp file alongside the program. You can share that file to help track down the cause of the problem.

**How to enable:** Press **Ctrl+F8**, type **`/savedump`** and Enter (or launch **`kitty.exe -savedump`**) to write an encrypted diagnostic dump (kitty.dmp) of the configuration next to the exe.

(no screenshot)

### Menu key shortcuts definition

KiTTY lets you assign a keyboard shortcut to almost any item in its main menu, so you can trigger actions like opening the connected text editor, printing the screen, running a local command, sending or receiving files, or toggling full screen without reaching for the mouse. Each action has a sensible default shortcut (for example, the editor opens with SHIFT+F2 and the local command box with CONTROL+F5), and you can override any of them to fit your own habits. This is handy for keeping frequently used commands a single keystroke away.

**How to enable:** Define key shortcuts in the kitty.ini `[Shortcuts]` section (e.g. `editor=`, `print=`, `inputbox=` ...).

![Menu key shortcuts definition](docs/features/img/menu_shortcuts.jpg)

### New command-line options

KiTTY extends PuTTY's command line with a long list of extra switches, letting you control nearly every feature when launching from a shortcut, script, or the Run dialog. You can open a session straight in full screen or in the system tray, edit a session's settings, load a portable `.ktx` configuration, set a title, icon, password, or window class name, generate SSH keys, or disable individual features on the fly. All of PuTTY's original command-line options keep working alongside these additions.

**How to enable:** KiTTY adds many switches on top of PuTTY's, e.g. `-loginscript`, `-fileassoc`, `-sshhandler`, `-launcher`, `-ed`, `-savedump`, `-kload`, `-classname`, `-noconfirm` (close the window without the "Are you sure?" prompt — handy for scripted/automated launches; it does not affect the SSH host-key or weak-crypto security confirmations), and `-hwndparent <handle>` (embed the terminal as a child of another application's window, so connection managers such as **mRemoteNG** and **Remote4Support** can host KiTTY inside their own tabs — pass the host window handle as a decimal number). See the list.

(no screenshot)

---

## Bonus

### Hidden text editor

KiTTY includes a small built-in text editor that is tied to your terminal window. It gives you a simple scratch area where you can compose or paste text before sending it to the running session, which is handy for preparing multi-line commands or notes without typing them straight into the terminal. Anything you write in the editor can be sent directly to the active session.

**How to enable:** Press **Shift+F2** to open the built-in editor (Ctrl+Shift+F2 opens it pre-filled with the clipboard). Text typed there can be sent straight to the session.

(no screenshot)

---

## Credits

KiTTY is developed by **Cyril Dupont** ([cyd01/KiTTY](https://github.com/cyd01/KiTTY/)),
based on **PuTTY** by **Simon Tatham** and contributors. Several features integrate
third-party patches (RuTTY, the covidimus background-image patch, Patrick Cernko's
key-confirmation patch, LePuTTY ZModem, and others), credited in their sections above.
This 0.84-based port preserves those features on a current PuTTY base.
