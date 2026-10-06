# Re-attach tmux, screen, abduco or zellij per window

After a connection drops, we have an option to deterministically reconnect.
Meaning: KiTTY++ can report to the server **which terminal-window** is
connecting, so that window lands straight back in its own tmux (or screen,
abduco, zellij) session - every time, without typing anything.

This page covers what to set in KiTTY++, what to set on the server, and the
three ways to wire this together.

## 1. The window's identity: `%KITTY_WINDOW%`

Every terminal window has a value called `%KITTY_WINDOW%`. Nothing has to be
set to use it:

| Situation | `%KITTY_WINDOW%` |
|---|---|
| First window of session `db` | `db` |
| Second and third window of `db`, open at the same time | `db-2`, `db-3` |
| `db` closed, a new window of the session opened | `db` again (the lowest free number is taken) |
| Window set via **Terminal Name...** or `-winname prod` | `prod` |

- **The number stays with the window.** A reconnect after a drop sends the same
  value, and after a Windows restart the Restart Manager brings the window back
  with its number (`-winslot <n>`) or its name (`-winname <name>`).
- **Terminal Name base** (session setting, Connection > Login > Environment)
  replaces the session name in front of the number: `mon` gives `mon`,
  `mon-2`. Windows variables in it are expanded: `%USERNAME%-mon` gives
  `username-mon`. The base is never displayed.
- **A name is for one window.** **Terminal Name...** is in the terminal's
  system menu (right-click the title bar, **Window** > **Terminal Name...**) and
  in the launcher's **Open Sessions** (right-click a window's entry). A named
  window shows its name in front of the title and of its launcher entry. The
  name is not saved; for a fixed one, start the window as
  `kitty.exe -load "db" -winname prod`, for example from a shortcut.

Related values, expanded the same way: `%KITTY_SESSION%` (saved session name),
`%KITTY_USER%`, `%KITTY_HOST%`, `%KITTY_PORT%`, `%KITTY_HWND%` (the window
handle, hex). Those can also be set in the following way to be handed to the server.

## 2. Three ways to reach the server

| Way | Server change needed | Where it runs |
|---|---|---|
| **A. Environment variable** (recommended) | `AcceptEnv KITTY_*` in `sshd_config` | the login shell's startup file decides |
| **B. Auto-command** | none | typed into the shell after login |
| **C. Remote command** | none | replaces the login shell |

The value is filled in on every connection attempt. The settings keep the
`%KITTY_WINDOW%` token as typed.

### A. Environment variable + shell startup file

**On the server**, once, as root:

```sh
# /etc/ssh/sshd_config (or a file in /etc/ssh/sshd_config.d/)
AcceptEnv KITTY_*
```

```sh
sudo systemctl reload ssh      # the unit is "sshd" on some systems
sudo sshd -T | grep -i acceptenv   # must list KITTY_*
```

**In KiTTY++**, per session: Connection > Login > Environment, add

| Variable | Value |
|---|---|
| `KITTY_WINDOW` | `%KITTY_WINDOW%` |

and save the session.

**In `~/.bashrc`** of the remote account (other shells: the same logic in
their interactive startup file):

```sh
if [ -n "$KITTY_WINDOW" ] && [ -z "$KITTY_ATTACHED" ] && [ -t 0 ]; then
    case $- in *i*)
        export KITTY_ATTACHED=1
        exec tmux new -A -s "$KITTY_WINDOW" ;;
    esac
fi
```

Pick the `exec` line for your multiplexer; each attaches to the session of
that name or creates it:

```sh
exec tmux new -A -s "$KITTY_WINDOW"
exec screen -D -R -S "$KITTY_WINDOW"
exec abduco -A "$KITTY_WINDOW" "$SHELL"
exec zellij attach --create "$KITTY_WINDOW"
exec dtach -A "${XDG_RUNTIME_DIR:-/tmp}/dtach-$KITTY_WINDOW" -r winch "$SHELL"
```

Which one decides what reaches KiTTY++ from inside it: the clipboard,
hyperlinks, notices and the shell integration - see
[section 5](#5-osc-features-inside-the-multiplexer).

- `KITTY_ATTACHED` is inherited by every shell inside the multiplexer, so those
  shells do not try to attach again.
- `-t 0` and the interactive check keep `scp`, `sftp`, `rsync` and remote
  commands away from it.
- A connection without `KITTY_WINDOW` (another client, a session without the
  variable, a server without `AcceptEnv`) gets an ordinary shell.

### B. Auto-command (no server change)

Connection > Login > **Auto-command**:

```
 exec tmux new -A -s '%KITTY_WINDOW%'
```

It is typed into the shell after login. The **leading space** keeps it out of
the shell history where `HISTCONTROL` contains `ignorespace` (the default on
many systems). Quote the token: a name may contain spaces. A backslash in the
value arrives as one.

### C. Remote command (no server change)

Connection > SSH > **Remote command**:

```
tmux new -A -s '%KITTY_WINDOW%'
```

The session then always opens in the multiplexer; leaving it ends the
connection. SSH only.

#### How to set a command-alias

Not a shell alias: the remote command runs via `sh -c` (non-interactive, no
alias expansion). Use a script:

```sh
#!/bin/sh
# ~/.local/bin/kitty-attach   (chmod +x)
name=${1:-${KITTY_WINDOW:-default}}
exec tmux new -A -s "$name"
# exec screen -D -R -S "$name"
# exec abduco -A "$name" "$SHELL"
# exec zellij attach --create "$name"
# exec dtach -A "${XDG_RUNTIME_DIR:-/tmp}/dtach-$name" -r winch "$SHELL"
```

Remote command (full path - `PATH` may lack `~/.local/bin`):

```
$HOME/.local/bin/kitty-attach '%KITTY_WINDOW%'
```

Interactive shells may alias it:

```sh
alias ka='~/.local/bin/kitty-attach'     # ~/.bashrc, ~/.zshrc
alias ka '~/.local/bin/kitty-attach'     # fish: ~/.config/fish/config.fish
```

## 3. Several people, several PCs

- **Different accounts on the server never meet.** tmux keeps one server per
  account (`/tmp/tmux-<uid>/`), screen keeps its sockets per user: two
  accounts can both have a session called `db`.
- **One account used from several places does meet.** Two admins logging in
  as `root`, or you from two PCs, would both send `db`, and `tmux new -A`
  attaches the second one to the first one's session - one screen, both
  typing. Give each client its own base: **Terminal Name base**
  `%USERNAME%-db` (per Windows user) or `%COMPUTERNAME%-db` (per PC).
- The numbering (`db-2`, `db-3`) only sees the windows on one Windows desktop;
  it cannot keep two PCs apart. The base is what does.

## 4. When it does not work

| Symptom | Check |
|---|---|
| `echo $KITTY_WINDOW` prints nothing | the variable is on Connection > Login > Environment and the session was saved; `sshd -T` lists `KITTY_*`; sshd was reloaded |
| It prints `%KITTY_WINDOW%` literally | the build predates this feature |
| A rename is not seen | the running shell keeps the value at login; the next connection (Restart Session, a reconnect) carries the new value |
| Every window attaches to the same session | the same **name** was given to several windows - names are per window, use the base + numbers instead |
| klink / kscp / ksftp | the console tools do not fill in `%KITTY_...%` |

## 5. OSC features inside the multiplexer

tmux, screen and zellij draw the screen themselves and drop most of the
sequences programs send to KiTTY++: the remote clipboard (OSC 52), hyperlinks
(OSC 8), notices (OSC 9, 777, 99), taskbar progress and program status (OSC
9;4, 7501), and the shell integration (OSC 7, 133). dtach passes all of them.

| | What to do |
|---|---|
| **tmux** | Three lines in `~/.tmux.conf` let the clipboard, hyperlinks and the shell integration through. Notices, progress and status pass only when the program wraps them. |
| **screen** | Nothing plain passes. The shell integration and `kpp-osc` wrap; anything wrapped over about 768 bytes is still dropped. |
| **zellij** | The clipboard passes; nothing else does, wrapped or not. |
| **dtach** | Everything passes. What arrives while no window is attached is lost. |
| **abduco** | Not measured. |

The measurements, the tmux lines, the shell snippets and the `kpp-osc` helper
for your own scripts are in [OSC-INTEGRATION.md](OSC-INTEGRATION.md#tmux-screen-zellij-and-dtach).
For a window per server session, with KiTTY++ doing the windows, dtach loses
nothing on the way.

The same text, with the details, is in the manual: *Terminal Name and
%KITTY_WINDOW%*, and *Re-attaching tmux, screen, abduco or zellij*.
