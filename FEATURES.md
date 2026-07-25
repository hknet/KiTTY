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
  - [Restricted process ACL (-restrict-acl)](#restricted-process-acl--restrict-acl)
- **Graphical features**
  - [An icon for each session](#an-icon-for-each-session)
  - [Send to tray](#send-to-tray)
  - [Transparency](#transparency)
  - [Protection against keyboard input](#protection-against-keyboard-input)
  - [Roll-up](#roll-up)
  - [Always on top](#always-on-top)
  - [Font management](#font-management)
  - [Word navigation modifier](#word-navigation-modifier)
  - [Quick start of a duplicate session](#quick-start-of-a-duplicate-session)
  - [Window title placeholders](#window-title-placeholders)
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
  - [Non-blocking connection errors](#non-blocking-connection-errors)
  - [Run the clipboard as a command](#run-the-clipboard-as-a-command)
  - [In-app updater (Check for updates)](#in-app-updater-check-for-updates)
- **Bonus**
  - [Hidden text editor](#hidden-text-editor)

---

## Most requested features

### Sessions filter (folders)

If you manage a large number of saved sessions, KiTTY lets you organize them into folders, for example one folder per machine, per environment, or per type of application. A dropdown in the Session panel lets you pick a folder so the saved-session list shows only the sessions it contains, making a long list far easier to navigate. You can also filter the visible list as you type in the Saved Sessions field; prefix and token matches are ranked before substring matches, and folder names are shown in brackets while searching. The root list shows every session, so there each one that lives in a folder is marked with it in brackets; sessions in no folder are left unmarked.

**How to enable:** Automatic in KiTTY mode: the Session panel shows a **Folder** dropdown that filters the saved-session list to one folder, plus New folder / Delete folder controls. To create a folder, pick the **`<new folder...>`** entry at the top of the dropdown, type the name, and click *New folder*. To rename a folder, select it, type the new name over it, and click *Rename* — the button renames itself to say so. The sessions in it move with it. Deleting a folder that still contains sessions asks first, and moves them to the root list rather than deleting them. Sessions that are in no folder live in the root list, shown as **All sessions (root)** — that is not a folder and cannot be deleted, but you can rename what it is called: select it, type your own name over the label, and the button changes to *Rename* to confirm what will happen. Typing the built-in name back restores it. The new name is cosmetic, so no session or setting is moved or changed by it (it is stored as `RootFolderLabel`, in the registry or in kitty.ini's `[KiTTY]` section depending on your save mode). To search within the active folder filter, type in the Saved Sessions field; Up/Down moves into the filtered list and Enter loads or starts the highlighted visible session. Press **Ctrl+F** anywhere in the config window — from any settings panel, or right after starting a session with Enter — to jump back to the Session panel with the search field focused and its content selected, so just typing starts a new search. The two buttons differ in where the session opens: clicking **Open** opens the chosen session in the current window (the config box closes), while **Start** — like pressing Enter — starts it in a new window and keeps the config box open for launching the next one. If you prefer the classic behaviour where typing never narrows the list, set `filter=no` in the kitty.ini `[ConfigBox]` section.

![Sessions filter (folders)](docs/features/img/config_folder.jpg)

*See also: [How session folders work (PDF)](docs/features/kitty-folders_list_feature.pdf)*

### Portability

By default KiTTY stores its configuration in the Windows registry. In **portable mode** it instead keeps normal KiTTY runtime state next to the executable/config directory, so you can carry KiTTY, its sessions, and its SSH trust cache on a USB stick. Saved sessions are one file each (`Sessions\<name>`), and SSH host keys, SSH host CAs, the random seed, recent-session state, and the update-check cache are file-backed as well.

**Protecting passwords in portable mode:** by default, auto-login and proxy passwords are DPAPI-encrypted — which protects them at rest on the current Windows account, but means they will *not* decrypt if you copy the portable folder to another PC. You can instead set an opt-in **master password**, which encrypts them so the store *does* move between machines. **The master password is never stored and cannot be recovered — if you forget it, the passwords it protected are lost** (you clear and re-enter them). To supply it non-interactively use `-masterpwfile`; to keep the classic plaintext form set `[KiTTY] PortablePasswordProtection=legacy`.

**Command-line tools use the registry:** `klink`/`kscp`/`ksftp` (plink/pscp/psftp) read saved sessions from the Windows registry, not from a portable store — so a portable install's sessions, and any passwords a master password protects there, are usable from the KiTTY GUI but not from the command-line tools.

**How to enable:** Use the dedicated **kitty_portable.exe** (defaults to file mode), or place a `kitty.ini` next to `kitty.exe` containing `[KiTTY]` then `savemode=dir`. The release includes `kitty.ini.example` as a commented, inert starting point; copy/rename it only when you want an active config file.

(no screenshot)

### Shortcuts for pre-defined commands

KiTTY lets you define your own list of pre-defined commands that appear in a dedicated **User Command** menu, reachable by holding Ctrl and right-clicking anywhere inside a KiTTY window. Each command you define is automatically assigned a keyboard shortcut (Ctrl+Shift+A, Ctrl+Shift+B, and so on, in the order they were created), so you can fire frequently used commands instantly. You can add as many commands as you like, and define them globally, per saved session, or per session folder. If the shortcuts ever clash with another program running inside the window (for example Midnight Commander), you can turn them off by adding `shortcuts=no` under a `[KiTTY]` section in your `kitty.ini` file.

**How to enable:** Define commands under the `Commands` registry/ini key (global, per-folder, or per-session). They appear in the **User Command** menu and each gets a **Ctrl+Shift+<letter>** shortcut.

![Shortcuts for pre-defined commands](docs/features/img/menu_shortcuts.jpg)

### Session launcher

The session launcher gives you a quick way to open your saved sessions without digging through menus. It lives in the system tray and lists your sessions organized into menus and sub-menus that mirror your session folders, using the backslash (\) as the separator. By default it rebuilds its menu from your saved sessions each time it starts, but you can also arrange the menu yourself and keep it fixed. It also includes an **Opened sessions** menu that lets you hide and unhide running sessions, removing them from the desktop and taskbar when you have too many open at once. When the launcher detects that a newer KiTTY release is available, the tray tooltip mentions the available version and the launcher menu shows a disabled **Update available: KiTTY ...** line, so the notice is not lost if a Windows tray balloon is suppressed.

**How to enable:** Run **`kitty.exe -launcher`** to open the tray launcher listing your saved sessions.

You can keep individual sessions out of the launcher menu while leaving them in the normal session list: tick **"Hide this session from the launcher"** in the session's **Session** panel.

For favourite sessions, you can assign a **global hotkey** in the session's **Window → Behaviour** panel. The hotkey is registered only while `kitty.exe -launcher` is running; when you save a session, a running launcher is notified and refreshes its registered hotkeys automatically. The same panel includes a check button that tells you whether the combination is currently available or already reserved by Windows/another application.

![Session launcher](docs/features/img/ex_launcher.jpg)

### Automatic logon script

KiTTY can automatically respond to a server's login prompts using a simple challenge-and-response script. You write a plain text file that alternates lines: an expected piece of text the server prints (such as `login:` or `password:`), followed by the text KiTTY should send in reply. This is handy for automating connections to passive protocols like telnet, where you can have your username and password sent for you. Note that it cannot handle SSH authentication, since SSH builds authentication into the protocol itself rather than exchanging plain prompts.

**How to enable:** Configuration > **Connection > Data**: set *Login script file* / *Login script content*, or launch with **`kitty.exe -loginscript <file>`**. The script's expect/answer lines auto-respond to the server's login prompts.

(no screenshot)

### Automatic logon script (RuTTY patch)

Based on the RuTTY patch, this lets you automate actions on a session by running a small script as soon as you connect. The script uses simple waitfor/halton commands to watch for text from the server and send responses, so common logon sequences and repetitive steps happen for you automatically. It is a handy way to script logins and routine interactions without typing them each time.

**How to enable:** Configuration > **Connection > Scripting**: set a RuTTY script (waitfor/halton style). It plays automatically once connected. You can also run a script on demand in a live session: system menu > **Tools > Send recorded script**. The whole engine can be disabled with `[KiTTY] scriptmode=no` in `kitty.ini`.

**Script file format** (see [docs/examples/logon-script.ksh](docs/examples/logon-script.ksh)):
a script is a plain text file sent line by line. With **Wait for a prompt before
each line** enabled, KiTTY waits until the server's output ends with the
**Wait-for text** (e.g. `$`) before sending the next line, aborts when the
**Halt-on text** appears, and gives up after the configured **Timeout**. Lines
starting with the condition character twice (`::` by default) are comments.
With **Use conditions from file** enabled, a line starting with a single `:`
overrides the wait pattern for the following line only:

```
:: minimal logon script: run two commands, each after a "$" prompt
uname -a
df -h
:password:
secret123
:: the line above is sent only after the server printed "password:"
```

![Automatic logon script (RuTTY patch)](docs/features/img/config_rutty.jpg)

### URL hyperlinks

KiTTY can detect URLs in the terminal output and turn them into clickable hyperlinks, so you can jump straight to a web address without copying and pasting it. You decide how links behave, including whether they are underlined, which modifier key activates them, and which browser opens them. This makes it quick to follow links that appear in logs, command output, or chat sessions.

**How to enable:** Configuration > **Window > Hyperlinks**: choose underline, whether Ctrl is required to activate links, browser, and optional hand cursor on hover. Ctrl+click a URL in the terminal to open it by default.

![URL hyperlinks](docs/features/img/config_hyperlinks.jpg)

---

## SSH and network

### In-terminal (inline) security confirmations

KiTTY's SSH security confirmations — an unknown (first-seen) host key, a changed host key, or a weak/legacy algorithm (including key exchange) — are shown as modal dialog boxes by default. Each can optionally be shown **inline in the terminal instead**, OpenSSH-style: the details are printed in the session window and you answer by typing, so the prompt never steals window focus. Every choice the dialog offers is available inline too, so nothing is lost by going inline.

- **Unknown host key** — type `yes` to accept and cache the key, or `once` to connect this time without caching it (anything else cancels).
- **Changed host key** — a deliberate two-step confirmation. Type `yes` to accept the new key for *this* connection; then, because KiTTY cannot verify the authenticity of a plain SSH host key (it is trusted on first use), type the exact word `confirmed` to *replace* the stored key for future connections. A reflexive `yes` is re-asked rather than accepted; `no` or Enter keeps the old key and connects once; `Ctrl-C`/`Ctrl-D` abandons. The heading and security warning are highlighted.
- **Weak algorithm / key exchange** — type `yes` to accept the risk and continue.

During an already-authenticated session (a rekey) the running program owns the terminal, so these cannot be prompted inline — KiTTY prints the details and abandons the connection instead (reconnect to review). Connection error messages can likewise be shown in the terminal rather than a box (see `modalerrors`).

**How to enable:** in `kitty.ini [KiTTY]`, set `modalnewhostkeyconfirmation`, `modalchangedhostkeyconfirmation` and/or `modalweakkeyconfirmation` to `no` (default `yes` = classic modal dialog). Each is independent, so you can keep some prompts modal and make others inline.

### Automatic password

KiTTY can log you in automatically to telnet, SSH-1 and SSH-2 servers by storing a password alongside the session. For SSH connections the password is supplied during authentication; for telnet it is sent once the connection comes up, just as if you typed it, and you can even send several lines (for example a login name, a password, and a command) by separating them with `\n`. Because the stored value is tied to the host, a password cannot be saved in a session that has an empty hostname.

**How to enable:** Configuration > **Connection > Data > Auto-login password**. It is stored with the session and sent automatically at SSH login. Tick **Show password** beside the field to reveal the stored value. As of 0.84.1.38 the password is **encrypted at rest with Windows DPAPI** (tied to your Windows account), rather than stored reversibly; existing/legacy passwords still load and are re-encrypted on the next save. NOTE: a one-time security warning still appears when you set one. DPAPI is machine-bound (it defeats offline/cross-user theft, not same-user malware, and does not move to another PC) — for the strongest security, prefer SSH public-key auth (kageant). In **portable mode** you can additionally protect it with a **master password** for cross-machine portability, which — unlike DPAPI — cannot be recovered if you forget it (see *Portable mode*). You are asked for the master password **once per running KiTTY**: unlocking it once shares it with the session windows KiTTY opens next — from the config box, *New Session*, *Duplicate Session*, or the tray launcher — so you are not prompted again for each window. The unlock is handed only to KiTTY's own child processes (through an inherited handle, wrapped in memory with Windows CryptProtectMemory); the saved files stay master-password-encrypted at rest.

![Automatic password](docs/features/img/config_password.jpg)

### Private-key usage confirmation

When you store private keys in KiTTY's key agent (kageant), you can require an explicit confirmation each time a key is used. With this enabled, every session that needs the key triggers a pop-up asking you to approve its use before authentication proceeds, giving you a clear chance to spot and refuse unexpected sign-in attempts. This adds a helpful safeguard against a loaded key being used without your knowledge. Thanks to [Patrick Cernko](https://people.mpi-klsb.mpg.de/~pcernko/pageant.html) for this patch.

**How to enable:** For all keys at once, tick **"Ask confirmation before key use"** in the kageant tray menu (persisted, default off): every signing request then pops an allow/deny prompt naming the key. Or per key: generate a key whose **comment contains the word `confirmation`** (in kittygen), then load it into **kageant.exe** — only that key asks for confirmation. Or from **kitty.ini**: `[Agent] askconfirmation=` with the classic three states — `yes` (every use), `auto` (per-key comments only; the default), `no` (never — also silences the per-key prompts, for automation). kageant finds the ini on its own (`KITTY_INI_FILE`, else next to the exe, else `%APPDATA%`); when that file says `[KiTTY] savemode=file` or `dir` — or, with no savemode line, when a portable layout (a `Sessions` folder or `KiTTYState` file) sits beside it — the ini is the authoritative store and the tray toggle writes back to it, so a **portable** kageant never touches the registry; the key-list window and the tray tooltip show *kitty.ini mode* when this is in effect. The related **“Notify when a key is used”** tray balloon (default on) is controllable the same way with `[Agent] messageonkeyusage=yes/no`.

![Private-key usage confirmation](docs/features/img/config_kittygen.jpg)
![Private-key usage confirmation](docs/features/img/ex_kageant.jpg)


### kageant — Windows OpenSSH agent integration

kageant (KiTTY's SSH agent) can act as the agent for the **Windows OpenSSH client** (`ssh.exe`), so the keys you load in kageant are usable by `ssh`, `git`, `scp` and any tool that uses Windows OpenSSH. When enabled, kageant writes `%USERPROFILE%\.ssh\kageant.conf` (an `IdentityAgent` line pointing at its named pipe) and adds a marker-delimited managed block to `%USERPROFILE%\.ssh\config` that includes it. It is **off by default** so kageant never alters your SSH configuration unless you ask, and only its own marker block is touched (the rest of `~/.ssh/config` is preserved byte-for-byte, written atomically, with a one-time `config.kageant.bak` backup).

**How to enable:** right-click the kageant tray icon → **Register as Windows OpenSSH agent**. Untick to remove the managed block again.

(no screenshot)

### kageant — load keys on startup

kageant can remember the keys you load and re-add them automatically at the next login, added **encrypted/deferred** (the passphrase is only requested the first time a key is actually used). After a passphrase-protected SSH-2 key is first used, kageant returns the long-lived in-memory key state to a Windows `CryptProtectMemory`-protected private blob and only unprotects/deserializes it temporarily for signing. Normally added SSH-2 keys use the same protected steady state. It auto-tracks the file paths of the keys you load; enabling the option also installs an autostart entry so kageant starts at login — replacing the need for a hand-made Startup shortcut. Only key-file *paths* are stored, never passphrases or key material.

In a **portable** install (kitty.ini authoritative — see the confirmation section) the list lives in kitty.ini instead of the registry, so it travels with the stick: keys inside the install folder are stored as paths relative to it (surviving a drive-letter change), and a key added from elsewhere prompts you to either copy it into the portable  folder or reference it where it is (machine-local). The autostart itself is machine-local either way: a portable kageant places a **login shortcut in your Startup folder** (registry-free), while an installed/registry-mode kageant uses the classic `HKCU\…\Run` entry. Both are pinned to the current machine and path rather than following the stick — handy for a fixed or unattended install — and disabling the option removes them. Because the agent is single-instance, if another kageant/pageant is already set to autostart from a different location, enabling this warns you (only one agent runs at a time; it does not touch the other entry). If a remembered key cannot be found at startup (e.g. the stick is on another machine), kageant shows a single tray notice and skips it rather than dropping it from the list. The tray launcher has the same one-click **Start at login** toggle (a Startup-folder shortcut) in its menu.

**How to enable:** right-click the kageant tray icon → **Load keys on startup**.

(no screenshot)

### kageant — reorder loaded keys

kageant offers its loaded keys to a server in list order, and the server tries them in turn — so the order matters when you hold several keys (offering the wrong ones first can even hit a server's "too many authentication failures" limit before the right key is reached). The key-list window has **Move Up** / **Move Down** buttons to set that offer order, e.g. to put your most-used key first. The chosen order is saved by key fingerprint and restored on the next start, including when *Load keys on startup* is enabled. Reordering works independently of whether a key is still encrypted/deferred, already protected in memory, or temporarily unprotected for a signing operation.

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

When you regularly reach hosts through a bastion, jump server, or a corporate HTTP/SOCKS proxy, Proxy choice saves you from setting up the same proxy by hand for every session. You define your proxies once as reusable **named proxies**, then pick the one you want from a dropdown in the main Session panel — and the choice is remembered per session. This is handy when you keep a SOCKS proxy open on a bastion (for example via a dynamic port forward), or route through a company HTTP proxy, and don't want to re-enter it on the Connection/Proxy panel every time.

Named proxies are managed from a built-in editor — an **Edit** button sits next to the dropdown, and on the Connection/Proxy panel — where you create, edit, and delete definitions with the full set of proxy settings: type, host, port, username/password, the command for Telnet/Local types, excluded hosts, DNS-at-proxy, and diagnostics. Each proxy's password is **encrypted at rest** exactly like a session password (Windows DPAPI in the registry, or your master password in portable mode), and is decrypted only when a session that uses the proxy actually connects.

A named proxy can also be an **SSH jump host**: pick one of the *SSH jump host* types in the editor and KiTTY opens a real SSH connection to that host and tunnels the session through it (the equivalent of OpenSSH's `ProxyJump`). The three variants differ in *how* the tunnel is made on the jump host:

- **SSH jump host (port forwarding)** — the normal choice. After logging in to the jump host, KiTTY asks its SSH server for a standard forwarded connection to the destination (a `direct-tcpip` channel, like `ssh -J` / `plink -nc`). Nothing is executed on the jump host, so it works even for restricted accounts with no shell — but the server must allow port forwarding.
- **SSH jump host (execute a command)** — for jump hosts where port forwarding is disabled but you can run programs: KiTTY logs in, runs the command from the *Command* field on the jump host, and uses that command's input/output as the tunnel. `%host` and `%port` in the command are replaced by the real destination, e.g. `nc %host %port` or `socat - TCP:%host:%port`.
- **SSH jump host (invoke a subsystem)** — like *execute a command*, but starts a named SSH **subsystem** (the *Command* field holds the subsystem name) instead of a shell command. Only useful when the jump host's sshd is configured with a dedicated forwarding subsystem; if you don't know you need this, you don't.

If you leave the password empty, the jump connection authenticates like any SSH session — keys loaded in **kageant** are tried automatically. The jump host's host key is checked and cached like any other host's.

**Where the jump host's settings come from.** Unlike OpenSSH's `ProxyJump`, the jump connection does *not* inherit the target session's settings. KiTTY loads a separate configuration for it: if the proxy **Host** field matches the name of one of your **saved sessions**, that whole session is used (its private-key file, username, port, agent setting, even its own proxy); otherwise the field is treated as a bare hostname and the jump uses your **Default Settings**, with only the proxy's host/port/username/password applied on top. So agent authentication to the jump host follows the *Attempt authentication using Pageant* checkbox (Connection → SSH → Auth) of that saved session — or of Default Settings for a bare hostname — which is independent of the same checkbox on the target session. If the jump host needs a specific private key rather than an agent key, save a session for it and name that session in the Host field.

**Multiple jump hops.** There is no comma-separated jump list; chain instead. Because a named proxy whose Host field is a saved session inherits *that* session's proxy too, you can reach `A → B → C` by giving session C a proxy that points at a saved session B, whose own proxy points at A.

![Named proxy editor](docs/features/img/config_proxyeditor.png)

**How to enable:** The dropdown appears automatically once you have any named proxy defined — click **Edit** beside it (or the button on the Connection/Proxy panel) to add one, then pick it from the **Proxy choice** dropdown in the Session panel and save the session. To force the dropdown always on or off, set `[ConfigBox]` `proxyselection=yes` (or `no`) in kitty.ini; the default is `auto`.

![Proxy choice](docs/features/img/config_proxychoice.jpg)

### SSH handler (URL/OS integration)

KiTTY can register itself with Windows as the program that opens **putty://**, **telnet://**, and **ssh://** links. Once registered, clicking such a link in your browser (Internet Explorer, Firefox, or any other) or in another application launches KiTTY and connects to the target host automatically. This makes it easy to publish clickable connection links on intranet pages or in documentation, so colleagues can start a session with a single click.

**How to enable:** Run **`kitty.exe -sshhandler`** as administrator to register KiTTY as the telnet:// ssh:// putty:// URL protocol handler.

(no screenshot)

---

## Technical features

### Automatic command

KiTTY can send a command to the server automatically as soon as a Telnet or SSH connection is established, saving you from typing the same startup command every time you log in. You can send several commands at once by separating them with the two characters `\n`. A few special sequences let you pace the input: `\p` waits one second (repeat it for longer pauses), `\s05` pauses for five seconds (use any value), and `\\` sends a literal backslash.

Three `kitty.ini` `[KiTTY]` settings fine-tune the timing: **initdelay** — seconds before the first automatic send after the connection opens (default 2.0); **commanddelay** — seconds between two lines of the command script (default 0.05 = 50 ms); and **bcdelay** — milliseconds between each *character*, default 0 = off. `bcdelay` is the one to reach for when a host drops characters that arrive too fast (serial consoles, slow embedded devices): set e.g. `bcdelay=3` and it paces every automatic keyboard send — autocommand, login scripts, user commands, and the send-text boxes.

**How to enable:** Configuration > **Connection > Data > Auto-command**: a command sent to the server automatically right after login. The delay before the first send is `initdelay` (seconds, default 2.0) in the kitty.ini `[KiTTY]` section — raise it for hosts that are slow to present their prompt; the delay between subsequent lines is `commanddelay`.

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

### Restricted process ACL (-restrict-acl)

Inherited from PuTTY: starting KiTTY with the `-restrict-acl` command-line option locks down the Windows process ACL, so other programs running under the same user account cannot open the KiTTY process (for example to read its memory, which holds session passwords while connected). Every window KiTTY spawns from such a process — sessions started from the configuration box, duplicates, "open new with current settings" — inherits the restriction. `kageant` and `kittygen` accept the option too. Be aware of the trade-offs before enabling it: the lockdown also blocks legitimate same-user tooling such as screen readers and other accessibility or automation software, and during an in-place MSI upgrade the Windows Restart Manager cannot inspect restricted windows, so the installer reports *"a critical application holds files in use"*, offers no close-and-restart handling and simply closes the windows. Up to and including 0.84.1.60, sessions spawned from the configuration box ran with this restriction unintentionally — always, regardless of options; since 0.84.1.61 it applies only when requested.

**How to enable:** off by default. Add `-restrict-acl` to the command line, typically in your shortcut target: `"C:\Program Files\KiTTY\kitty.exe" -restrict-acl` (with the launcher: `kitty.exe -restrict-acl -launcher`).

(no screenshot)

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

### Always on top

Always on top (formerly "Always visible") keeps a KiTTY window in the foreground, on top of all your other windows, so you can keep an eye on it while you work elsewhere. This is handy for monitoring a session, a log, or a long-running command without it slipping behind other applications. You can toggle it from the system menu, or with the **CTRL+F7** keyboard shortcut. While active, the window title carries an **(ONTOP)** marker.

**How to enable:** System menu **Window > Always On Top**.

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
(Left/Right arrows)"**, pick Alt / Ctrl / Both.

This option only takes effect when **Shift/Ctrl/Alt with the arrow keys** is set to
**xterm-style bitmap** — KiTTY's default. In the *"Ctrl toggles application mode"*
setting the arrow-key modifiers aren't encoded, so no word-navigation remapping is
possible.

(no screenshot)

### Quick start of a duplicate session

KiTTY lets you instantly open a second window that inherits all of the current session's settings, so you can run another connection to the same host without reopening the launcher or re-entering details. For an even faster shortcut, hold **CTRL + SHIFT** and click the middle of the active window with the **left mouse button** to launch the duplicate.

**How to enable:** System menu **Duplicate Session** — opens a new window with the current session's settings.

(no screenshot)

### Window title placeholders

KiTTY can expand dynamic placeholders in the **Window Title** setting so the title reflects the active connection.

**How to use:** Enter a title string such as `%%h - %%s` in the session's **Window Title** field. The available placeholders are:

| Placeholder | Value shown in the window title |
|---|---|
| `%%h` | Hostname (falls back to the configured host) |
| `%%s` | Saved session name |
| `%%u` | Username |
| `%%p` | Port number |
| `%%P` | Protocol display name (e.g. `SSH`) |
| `%%f` | Folder name the session belongs to |
| `%%l` | Local forwarded ports (blank if none configured) |
| `%%d` | Dynamic/SOCKS forwarded ports (blank if none configured) |

For full details and examples, see [`docs/window-title-placeholders.md`](docs/window-title-placeholders.md). If a remote shell later replaces the title, enable **Terminal → Features → Disable remote-controlled window title changing** (`NoRemoteWinTitle=1`) to keep the placeholder-expanded title.

(no screenshot)

### Background image

KiTTY can display a picture behind your terminal text, giving each session window a custom backdrop. It supports BMP and JPEG images, and you can adjust how strongly the image shows through with an opacity setting or rotate through several pictures as a slideshow. This feature grows out of the covidimus patch integrated into KiTTY.

**How to enable:** Add `bgimage=yes` to `[KiTTY]` in kitty.ini (the key is `bgimage`, not `backgroundimage`), then configure **Window > Back.Image** (image file, opacity, slideshow).

![Background image](docs/features/img/ex_background.jpg)

---

## Other features

### Automatic saving

KiTTY can keep a registry-mode backup of its settings, sessions, and host keys as **kittynew.sav** files. In registry mode, each time settings are saved from the configuration dialog KiTTY writes a *fresh, timestamped* `kittynew-YYYYMMDD-HHMMSS.sav` — so the filename always reflects when that copy was written — and keeps the newest `[KiTTY] savbackupcount` of them (default 5). By default they live under `%APPDATA%\KiTTY` next to `kitty.ini`; advanced users can override the base path with `[KiTTY] sav=`. The name is `kittynew.sav`, not `kitty.sav`, so an older KiTTY installed side by side never has its backup overwritten.

**Backup/restore:** each `kittynew-*.sav` is a Windows Registry export of KiTTY's configuration hive. To restore one, close KiTTY/kageant/launcher first, then import the chosen file with Registry Editor or `reg import kittynew-YYYYMMDD-HHMMSS.sav`, and start KiTTY again. If the backup was protected with KiTTY's legacy config-password mechanism, KiTTY prompts for that password while loading it. Portable directory mode (`kitty_portable.exe` / `savemode=dir`) stores sessions as files instead; when settings are applied, KiTTY refreshes `Backups\kitty-portable-latest` and keeps timestamped backups such as `Backups\kitty-portable-YYYYMMDD-HHMMSS` under the portable config directory. By default it keeps 5 timestamped backups; set `[KiTTY] portablebackupcount=0` to disable or another number to change retention. The backup contains `kitty.ini`, `Sessions`, `Proxies` (named proxy definitions), `Security` (master-password salt/verifier), `SshHostKeys`, `SshHostCAs`, `Commands`, `Folders`, `PUTTY.RND`, `KiTTYState`, `Jumplist`, and related portable config folders — i.e. a complete copy of your portable store, so a restored backup keeps its sessions, proxies, and master-password protection intact. To restore, close KiTTY/kageant/launcher and copy the backup contents back into the portable config directory.

**How to enable:** Automatic in registry mode: each time you apply the configuration dialog, KiTTY exports its registry hive to a timestamped **kittynew-*.sav** (in `%APPDATA%\KiTTY`, or the `[KiTTY] sav=` path) as a safety backup, keeping the newest `savbackupcount`.

(no screenshot)

### Non-blocking connection errors

When a connection drops, is closed by the remote host, or the server reports a non-fatal error, KiTTY prints the message **inline in the terminal** rather than popping a modal dialog that blocks the window until you click **OK**. The window stays usable and closable, and the text remains in the scrollback so you can read or copy it. A **fatal** disconnect also badges the window title with a **⚠ (disconnected)** marker, so a minimised or background window shows at a glance that its session died; the marker clears automatically the next time the session connects. Host-key and weak-crypto confirmations still use a normal prompt.

**How to enable:** on by default. To restore the classic modal error boxes, set `[KiTTY] modalerrors=yes` in `kitty.ini`.

(no screenshot)

### Run the clipboard as a command

KiTTY can run the current Windows clipboard contents as a local command with the **Ctrl+F5** shortcut — handy for sending a prepared command line straight into execution. Because that runs whatever happens to be on the clipboard, KiTTY shows a **confirmation prompt** (displaying the command) before running it and a **tray notification** after launch, so nothing runs unexpectedly.

**How to enable:** the shortcut is built in; the two safeguards are on by default and toggled per session in **Window → Selection** ("Running the clipboard as a local command").

(no screenshot)

### Paste size guard

Pasting into a terminal executes whatever the clipboard contains, line by line — so an accidental paste of the wrong (or huge) clipboard can flood the shell with unintended commands. KiTTY can ask for confirmation before pasting more than a configurable number of characters, telling you how large the clipboard is so you can abort a mis-aimed paste.

**How to enable:** set `pastesize=<N>` in the kitty.ini `[KiTTY]` section (number of characters above which the confirmation appears; `0`, the default, pastes without asking).

(no screenshot)

### In-app updater (Check for updates)

KiTTY can check whether a newer release is available and install it for you. *Check for updates* queries the official release list and, if a newer build exists, fetches the right asset for **how KiTTY was installed**: the per-user MSI, the system MSI (with an elevation prompt), or — for a **portable** copy — it just opens the download page. The downloaded installer runs only after it passes an **Authenticode check** (valid signature chain *and* the expected KAPPER publisher), is downloaded to a unique temporary `.msi` path, and is held locked against modification from verification through launch; anything that fails verification, fails to launch, or is cancelled is deleted and never run. A **stable** build will not silently install a **beta**: if the newest available build is a beta, KiTTY tells you and asks first (proceed with caution). The launcher also surfaces update availability in its tray tooltip and menu.

**How to enable:** system menu → **Check for updates**. Requires network access and honours your system/IE proxy settings; if the check can't complete it falls back to opening the releases page.

(no screenshot)

### pscp.exe and WinSCP integration

KiTTY lets you transfer files without opening a separate program, reusing the host and credentials of your current session. From the system menu you can send a file straight into the running session with pscp (CTRL+F3) or open a full WinSCP file-transfer session on the same server (SHIFT+F3), and you can also drag and drop a file or folder directly onto the terminal window to upload it. Helper shell functions are available so that, from within a UNIX session, you can grab a file or launch WinSCP in your current directory with a single command.

**Directory-aware uploads (OSC 7).** Enable **Track remote directory (OSC 7 shell integration)** in **Connection → SSH → KSCP and WinSCP** and drag-and-drop uploads — and *Start WinSCP* — target your shell's **current remote directory** instead of your home directory. KiTTY reads the directory from the standard **OSC 7** sequence (`ESC ] 7 ; file://host/path BEL`) your shell emits on every prompt, validated strictly as data — absolute path, whitelisted characters, never executed. It is the safe, data-only replacement for KiTTY's old "send file to the current directory" option, which was retired after that title-scan mechanism proved to be a remote-code-execution hole (CVE-2024-23749); sessions that used it are migrated to OSC 7 tracking automatically. Prefer a single hard-wired target? Use **Fixed remote upload directory** in the same panel (mutually exclusive with tracking). Two lines in your shell startup enable OSC 7 — see [docs/examples/osc7-shell-integration.md](docs/examples/osc7-shell-integration.md).

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

### Export all / Import all sessions

Beyond exporting a single session, KiTTY can move your **whole set of saved sessions** — and your named proxy definitions — between installs or machines. **Export all…** writes every saved session as a protected `.ktx` file into a folder you choose (plus a `Proxies\` folder for named proxies); **Import all…** reads them back from a folder. Passwords are re-wrapped for the destination: in a portable install they travel under your master password (so they work on another machine that has the same master password), and in the registry they are re-protected with Windows DPAPI. When you import into a store that already contains sessions or proxies of the same name, KiTTY asks once whether to overwrite them or import only the new ones, and then reports how many sessions and proxies were imported, kept, or failed. Both pickers are the modern folder dialog with an address bar you can paste a path into.

**How to enable:** Use the **Export all…** / **Import all…** buttons in the Session panel of the configuration box, or the `-exportall <dir>` / `-importdir <dir>` command-line options for scripted or new-PC setups (`-masterpwfile` supplies a portable master password non-interactively).

(no screenshot)

### ZModem file transfer

KiTTY integrates ZModem support (originally from LePuTTY) so you can transfer files directly over an interactive terminal session. With the rz/sz helper tools in place, you trigger a receive or upload straight from the menu and move files to and from the remote host without opening a separate file-transfer client.

**How to enable:** Add `zmodem=yes` to `[KiTTY]` in kitty.ini and set the rz/sz (lrzsz) helper paths in **Connection > ZModem**. The Tools menu then offers **ZModem Receive / Upload / Abort**.

![ZModem file transfer](docs/features/img/config_zmodem.jpg)

### Savedump (diagnostic dump)

When you run into a problem and want help diagnosing it, KiTTY can capture a diagnostic dump of its current state. Reproduce the issue in your session, then trigger a dump to write a `kitty.dmp` file alongside the program. The dump path redacts stored passwords, proxy passwords, key passphrases, private-key filenames, clipboard contents, and other known secret fields, and the command-line `kitty.exe -savedump` path works in both registry and portable directory modes.

**Caution:** diagnostic dumps can still contain operational context and are under ongoing review for script-content edge cases. Review a dump before sharing it publicly.

**How to enable:** Press **Ctrl+F8**, type **`/savedump`** and Enter (or launch **`kitty.exe -savedump`**) to write an encrypted diagnostic dump (`kitty.dmp`) of the configuration next to the exe.

(no screenshot)

### Menu key shortcuts definition

KiTTY lets you assign a keyboard shortcut to almost any item in its main menu, so you can trigger actions like opening the connected text editor, printing the screen, running a local command, sending or receiving files, or toggling full screen without reaching for the mouse. Each action has a sensible default shortcut (for example, the editor opens with SHIFT+F2 and the local command box with CONTROL+F5), and you can override any of them to fit your own habits. This is handy for keeping frequently used commands a single keystroke away.

**How to enable:** Define key shortcuts in the kitty.ini `[Shortcuts]` section (e.g. `editor=`, `print=`, `input=`, `inputm=` ...; the full commented list is in `kitty.ini.example`).

**Syntax:** modifiers in braces, then the key — `{CONTROL}`, `{SHIFT}`, `{ALT}`, `{ALTGR}`, `{WIN}` combined freely, followed by a single letter/digit or a named key (`{F1}`…`{F12}`, `{RETURN}`, `{SPACE}`, `{TAB}`, `{HOME}`, `{END}`, `{DEL}`, …). For example, to make **Ctrl+N duplicate the current session**:

```ini
[Shortcuts]
duplicate={CONTROL}N
```

Other popular targets: `opennew=` (new session), `changesettings=`, `fullscreen=`, `visible=` (always on top), `protect=`, `rollup=`, `eventlog=`. Setting `shortcuts=no` in `[KiTTY]` disables the whole shortcut layer.

Two of these open KiTTY's *send-text* boxes: `input` (default CTRL+F8) pops up a one-line box and `inputm` (default SHIFT+F8, with CTRL+SHIFT+F8 as a fixed alias) a resizable multiline box pre-filled from the clipboard. Text is composed locally and sent to the terminal only when you confirm (OK, or SHIFT+RETURN in the multiline box; if you select part of the text, only the selection is sent) — handy on slow links, and for sending a multi-line snippet as one block. In the one-line box, a line starting with `/` is a KiTTY *internal command* executed locally instead of being sent — type **`/help`** for the full list; e.g. `/size` and `/wintitle` toggle the title-bar decorations at runtime, `/save` writes the live settings back to this window's saved session, `/savenew <name>` saves them as a new session and switches the window to it, `/savedump` writes the diagnostic dump. The complete reference for every internal command — arguments, persistence, sharp edges — is in [docs/COMMANDS.md](docs/COMMANDS.md).

![Menu key shortcuts definition](docs/features/img/menu_shortcuts.jpg)

### PuTTY masquerade mode (KiClassName)

KiTTY can impersonate PuTTY for the benefit of external tools that only know PuTTY. With `KiClassName=PuTTY` set, the Win32 window class and the window/dialog titles become **PuTTY**, and sessions, host keys, and the jumplist are read from **and written to** PuTTY's registry hive (`Software\SimonTatham\PuTTY`) instead of KiTTY's own. That makes KiTTY a drop-in replacement wherever tooling is hard-wired to PuTTY: window/connection managers that find or embed windows by the `PuTTY` class name, automation that matches "PuTTY" window titles, and tools that provision sessions into PuTTY's registry before launching the terminal — all work unmodified while you keep KiTTY's features. Note you do **not** need this just to *see* an existing PuTTY installation's sessions: KiTTY already lists sessions from PuTTY's hive alongside its own (read-only) by default.

**How to enable:** set `KiClassName=PuTTY` in the kitty.ini `[KiTTY]` section (takes effect at startup, for all windows). For a one-off or per-shortcut override, the `-classname <name>` command-line switch sets the window class of a single window instead.

(no screenshot)

### New command-line options

KiTTY extends PuTTY's command line with a long list of extra switches, letting you control nearly every feature when launching from a shortcut, script, or the Run dialog. You can open a session straight in full screen or in the system tray, edit a session's settings, load a portable `.ktx` configuration, set a title, icon, password, or window class name, generate SSH keys, or disable individual features on the fly. All of PuTTY's original command-line options keep working alongside these additions.

**How to enable:** KiTTY adds many switches on top of PuTTY's, e.g. `-loginscript`, `-fileassoc`, `-sshhandler`, `-launcher`, `-ed`, `-savedump`, `-kload`, `-classname`, `-noconfirm` (close the window without the "Are you sure?" prompt — handy for scripted/automated launches; it does not affect the SSH host-key or weak-crypto security confirmations), and `-hwndparent <handle>` (embed the terminal as a child of another application's window, so connection managers such as **mRemoteNG** and **Remote4Support** can host KiTTY inside their own tabs — pass the host window handle as a decimal number). See the list.

(no screenshot)

---

## Bonus

### Hidden text editor

KiTTY includes a small built-in text editor that is tied to your terminal window. It gives you a simple scratch area where you can compose or paste text before sending it to the running session, which is handy for preparing multi-line commands or notes without typing them straight into the terminal. Anything you write in the editor can be sent directly to the active session.

**How to enable:** Use terminal **Tools > Open mNotepad**, or press **Shift+F2** to open the built-in editor (**Ctrl+Shift+F2** opens it pre-filled with the clipboard). Use **Send (F12)**, **Ctrl+Enter**, or the editor's **Send** menu item to send text to the parent KiTTY session. If text is selected, the selection is sent; otherwise mNotepad sends the current line/block according to the delimiter selected in its **Delimiter** menu. The editor font is DPI-scaled on high-DPI displays.

(no screenshot)

---

## Differences vs classic KiTTY: retired and revived kitty.ini settings

A settings audit of this 0.84-based port found a number of classic kitty.ini keys
that were still parsed but had lost their effect (their consumer was never
forward-ported, was superseded, or never worked upstream either). Rather than let
them silently pretend to work, each was either removed entirely or brought back
as a working feature.

**Missing a retired setting?** If a key listed below mattered to your workflow,
please [open an issue](https://github.com/hknet/KiTTY/issues) — several keys were
revived on request exactly that way, and where feasible we will restore the
wiring of a requested feature too.

**Revived — these keys work again, properly wired (some for the first time in
the port):**

- `[ConfigBox] dblclick=start` — a double-click on a saved session acts like the
  Start button: the session launches in a new window and the config box stays
  open (`open`, the default, keeps stock behaviour).
- `[ConfigBox] noexit=yes` — closing a window that ran a connected session
  reopens the configuration box, so you land back in the session picker. Fixed
  vs classic: a window that never connected (e.g. a cancelled config box) exits
  normally, and nothing respawns during system shutdown.
- `[KiTTY] scriptmode=no` — master off-switch for the RuTTY script engine (the
  session auto-script and the "Send a script file" menu entry).
- `[KiTTY] size=yes` — appends the live terminal size `[rows x cols]` to the
  window title, updated as you resize (hidden while maximized).
- `[KiTTY] wintitle` — KiTTY's title decorations, reimplemented safely: the
  size suffix plus `(PROTECTED)` and `(ONTOP)` status markers, refreshed live
  when the state changes. `wintitle=no` gives plain stock titles. **Security
  note:** in classic KiTTY this same title machinery also *parsed* titles for
  `__xy` remote commands — that channel stays removed; decoration here is
  strictly one-way output.

**Removed — ignored if present in an old kitty.ini:**

- `[KiTTY] adb`, `capslock`, `maxblinkingtime`, `paste`, `hostkeyextension`,
  `PlinkPath`, `KiPP` — read-but-dead in all 0.84.x builds; the features they
  once toggled either no longer exist or no longer consult them (ADB support is
  simply always available).
- `[KiTTY] localcmd`, `localunsecurecmd` — the `__xy` remote metacommand
  dispatcher they gated was removed for security (remote command-injection
  surface, cf. CVE-2024-23749); the switches were a booby trap without it.
- `[KiTTY] autostoresshkey` — deliberately not supported: silently accepting
  SSH host keys defeats the host-key check, so the port keeps the confirmation
  prompt unconditionally.
- `[ConfigBox] default`, `left`, `top` — dead getters; `left`/`top` are
  superseded by the automatic config-box position memory.
- `[Agent] scrumble` — the classic key shuffle was never functional in any
  KiTTY release (its shuffle table was never built at runtime), so there is
  nothing to restore. `askconfirmation` and `messageonkeyusage` were revived
  with full — and portable — wiring on request (hknet/KiTTY#14); see the
  *Private-key usage confirmation* section.
- `[PuTTY] keys` — the KiTTY→PuTTY registry replication it triggered had no
  remaining caller.

Everything else in `docs/examples/kitty.ini.example` is verified honored.

---

## Credits

KiTTY is developed by **Cyril Dupont** ([cyd01/KiTTY](https://github.com/cyd01/KiTTY/)),
based on **PuTTY** by **Simon Tatham** and contributors. Several features integrate
third-party patches (RuTTY, the covidimus background-image patch, Patrick Cernko's
key-confirmation patch, LePuTTY ZModem, and others), credited in their sections above.
This 0.84-based port preserves those features on a current PuTTY base.
