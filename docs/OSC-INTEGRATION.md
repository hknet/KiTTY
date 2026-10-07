# OSC integration: what the host side needs

KiTTY++ understands a number of OSC escape sequences that programs on the
host send. Some work as soon as a program sends them; some need a line in the
shell's startup file on the host. This page lists, for each one, the setting
in KiTTY++ and what the host needs.

## Overview

| Feature | Sequence | KiTTY++ setting | On the host |
|---|---|---|---|
| Current directory for uploads | OSC 7 | **Connection > File-Transfer-Settings > Track remote directory (OSC 7 shell integration)**, off by default | The [shell snippet](#shell-integration-bash-zsh-fish) |
| Prompt marks | OSC 133 | none | The [shell snippet](#shell-integration-bash-zsh-fish) |
| Program status | OSC 7501 | **Terminal > Features**, both settings of the group | A program that reports its status ([below](#program-status-osc-7501)) |
| Desktop notifications | OSC 9, 777, 99 | **Terminal > Features > Desktop notifications** | A program that sends them: `printf`, `kitten notify` |
| Taskbar progress | OSC 9;4 | **Terminal > Features > Progress and status on the taskbar button** | A program that sends it |
| Hyperlinks | OSC 8 | **Window > Hyperlinks > Allow OSC 8 hyperlinks**, on | Programs that write them: `ls --hyperlink=auto`, Rich/Textual applications |
| Remote clipboard | OSC 52 | **Window > Copy & Paste > Remote clipboard** | Neovim 0.10 and later has it built in; Vim needs a plugin (vim-oscyank) |
| Clipboard with types, paste events | OSC 5522 | the same as OSC 52 | `kitten clipboard` |
| File transfer through the terminal | OSC 5113 | **Connection > File-Transfer-Settings**, **Transfers & Tools > OSC 5113 (kitten)** | `kitten transfer` |
| Inline images, kitty graphics | APC `_G` | **Terminal > Features > Images > Show inline images (kitty graphics protocol)**, on | Nothing; programs: `kitten icat`, `chafa -f kitty`, `timg -pk`, pi ([below](#inline-images)) |
| Inline images, iTerm2 | OSC 1337 `File=` | **Terminal > Features > Images > Show inline images (iTerm2 OSC 1337)**, on | Nothing; programs: `imgcat`, `chafa -f iterm`, `timg -pi` |
| Inline images, Sixel | DCS `q` | **Terminal > Features > Images > Show inline images (Sixel)**, on | Nothing; programs: `chafa -f sixels`, `timg` (finds it by itself), img2sixel, lsix |
| True colour and terminal name | `COLORTERM`, `TERM_PROGRAM` (environment, not OSC) | **Application > KiTTY++ Settings > Terminal > Send COLORTERM and TERM_PROGRAM to the server**, on | SSH: `AcceptEnv COLORTERM TERM_PROGRAM` in `sshd_config` ([below](#colorterm-and-term_program)) |

Details of each feature: [FEATURES.md](../FEATURES.md). Inside tmux, GNU
screen, zellij or dtach, most of these need more: see
[tmux, screen, zellij and dtach](#tmux-screen-zellij-and-dtach).

## COLORTERM and TERM_PROGRAM

Each new SSH or Telnet connection sends `COLORTERM=truecolor` and
`TERM_PROGRAM=KiTTY++`; programs read `COLORTERM` to decide on 24-bit
colour. A session's own value (**Connection > Login > Environment**) wins;
an empty value there means the variable is not sent.

An SSH server accepts only the variables `sshd_config` names and drops the
rest **silently** - the session opens, the variables are just not there.
On the host, as root:

```
# /etc/ssh/sshd_config (or a file in /etc/ssh/sshd_config.d/)
AcceptEnv COLORTERM TERM_PROGRAM
```

then reload sshd (`systemctl reload ssh` or `sshd`). Check from a new
session: `env | grep -E 'COLORTERM|TERM_PROGRAM'`. The Event Log lists a
variable the server refused. Without access to `sshd_config`, set
`COLORTERM=truecolor` in the shell's startup file instead.

## Inline images

Nothing to install on the host besides the program that draws: the three
protocols travel in the terminal stream like text. How a program picks one:

- **Sixel** is found by itself: KiTTY++ reports it in its device
  attributes (`CSI c`). `timg` without options uses Sixel in KiTTY++.
- **kitty graphics and iTerm2** are found by name. Programs look for
  `TERM=xterm-kitty`, or match the terminal's name (`CSI > q`, answered
  `KiTTY++ <version>`) against a list that does not know KiTTY++ yet. Name
  the protocol: `timg -pk` (kitty), `timg -pi` (iTerm2), `chafa -f kitty`.
  Setting the session's terminal type to `xterm-kitty` (**Connection >
  Login > Terminal details > Terminal-type string**) also works, but needs
  kitty's terminfo on the host (Debian/Ubuntu: `kitty-terminfo`) and
  promises programs a few kitty features KiTTY++ does not have (curly and
  coloured underlines, kitty's remote control).

Inside tmux, pictures need passthrough (`set -g allow-passthrough on`,
tmux 3.3 and later) and a program that wraps its sequences for tmux (chafa
and timg do when they see tmux); tmux does not keep them on its redraws.
Sixel inside tmux needs a tmux built with Sixel support (3.4 and later).
Not measured here.

## Shell integration (bash, zsh, fish)

One snippet per shell sends two things before every prompt:

- **OSC 7**, the current directory. Drag-and-drop uploads, *Send File* and
  *Start WinSCP* use it when tracking is on.
- **OSC 133**, the prompt marks: `A` (a prompt starts), `C` (a command's
  output starts) and `D;<exit code>` (the command finished; not after an empty
  Enter). A prompt ends a program's *working* or *blocked* status (OSC 7501).
  The marks let **Alt+PgUp** / **Alt+PgDn** jump between prompts,
  **Alt+Shift+PgUp** / **Alt+Shift+PgDn** between failed commands, and
  **Alt+End** select a command's output; a failed command is marked red.
  **Alt+Home** selects the command line itself and needs the optional `B`
  mark (the start of the typed command): one more line per shell, below.

Many setups send OSC 7 already: Starship, several oh-my-zsh and oh-my-posh
themes, `ble.sh`, and the `vte.sh` some Linux distributions install in
`/etc/profile.d/`. Sending it twice does no harm.

Inside tmux and GNU screen the snippets wrap each sequence, which both
need; zellij drops them either way: see
[tmux, screen, zellij and dtach](#tmux-screen-zellij-and-dtach).

### bash: end of `~/.bashrc`

```bash
# KiTTY++ shell integration: current directory (OSC 7), prompt marks (OSC 133).
__kpp_osc() {
    if [ -n "$TMUX" ]; then printf '\033Ptmux;\033\033]%s\007\033\\' "$1"
    elif [ -n "$STY" ]; then printf '\033P\033]%s\007\033\\' "$1"
    else printf '\033]%s\007' "$1"; fi
}
__kpp_prompt() {
    local ec=$?
    [ -n "$__kpp_ran" ] && __kpp_osc "133;D;$ec"
    __kpp_ran=$__kpp_always
    __kpp_osc "133;A"
    __kpp_osc "7;file://${HOSTNAME}${PWD}"
    return $ec
}
case "$PROMPT_COMMAND" in
  *__kpp_prompt*) ;;
  *) PROMPT_COMMAND="__kpp_prompt${PROMPT_COMMAND:+; $PROMPT_COMMAND}" ;;
esac
# Output starts (133;C), and a command ran: PS0, before each command (bash 4.4+).
if (( BASH_VERSINFO[0] > 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 4) )); then
    __kpp_C=$(__kpp_osc "133;C")
    case "$PS0" in
      *__kpp_C*) ;;
      *) PS0='${__kpp_ran[__kpp_ran=1]}${__kpp_C}'"$PS0" ;;
    esac
else
    __kpp_always=1      # no PS0: the exit code goes with every prompt
fi
```

**Optional: the command's start (`133;B`)**, which **Alt+Home** (select the
command line) needs. It goes at the end of `PS1`, so it must come after
anything that sets the prompt: prompt themes (Starship, oh-my-posh, `ble.sh`)
rewrite `PS1` on every prompt and drop it. With such a theme, the mark has to
go into the theme's own prompt string. The variable is expanded after bash has
processed the backslashes in `PS1`, so the tmux and screen wrapping survives.

```bash
# Optional: the start of the typed command (133;B), for Alt+Home. Last, after
# any prompt theme.
__kpp_B=$(__kpp_osc "133;B")
case "$PS1" in
  *__kpp_B*) ;;
  *) PS1="$PS1"'\[${__kpp_B}\]' ;;
esac
```

### zsh: end of `~/.zshrc`

```zsh
# KiTTY++ shell integration: current directory (OSC 7), prompt marks (OSC 133).
__kpp_osc() {
    if [[ -n $TMUX ]]; then printf '\033Ptmux;\033\033]%s\007\033\\' "$1"
    elif [[ -n $STY ]]; then printf '\033P\033]%s\007\033\\' "$1"
    else printf '\033]%s\007' "$1"; fi
}
__kpp_preexec() { __kpp_ran=1; __kpp_osc "133;C"; }
__kpp_precmd() {
    local ec=$?
    [[ -n $__kpp_ran ]] && __kpp_osc "133;D;$ec"
    __kpp_ran=
    __kpp_osc "133;A"
    __kpp_osc "7;file://${HOST}${PWD}"
}
autoload -Uz add-zsh-hook
add-zsh-hook preexec __kpp_preexec
add-zsh-hook precmd __kpp_precmd
```

**Optional: the command's start (`133;B`)**, which **Alt+Home** (select the
command line) needs. It goes at the end of `PS1`, so it must come after
anything that sets the prompt: prompt themes (oh-my-zsh, powerlevel10k,
Starship) rewrite `PROMPT` on every prompt and drop it. With such a theme, the
mark has to go into the theme's own prompt string.

```zsh
# Optional: the start of the typed command (133;B), for Alt+Home. Last, after
# any prompt theme.
[[ $PS1 == *$']133;B'* ]] || PS1+="%{$(__kpp_osc '133;B')%}"
```

### fish: `~/.config/fish/config.fish`

fish 4 sends the OSC 133 marks `A`, `C` and `D` itself, the exit code
included; the `A` below is for older versions and does no harm twice. Older
fish gets no output or failed-command marks.

```fish
# KiTTY++ shell integration: current directory (OSC 7), prompt marks (OSC 133).
function __kpp_osc
    if set -q TMUX
        printf '\033Ptmux;\033\033]%s\007\033\\' $argv[1]
    else if set -q STY
        printf '\033P\033]%s\007\033\\' $argv[1]
    else
        printf '\033]%s\007' $argv[1]
    end
end
function __kpp_prompt --on-event fish_prompt
    __kpp_osc "133;A"
    __kpp_osc "7;file://"(hostname)"$PWD"
end
```

**Optional: the command's start (`133;B`)**, which **Alt+Home** (select the
command line) needs; fish does not send it. It goes at the end of the prompt,
so it must come after anything that defines `fish_prompt`: prompt themes
(tide, Starship, oh-my-posh) replace the function and drop it.

```fish
# Optional: the start of the typed command (133;B), for Alt+Home. Last, after
# any prompt theme.
if not functions -q __kpp_prompt_orig
    functions -c fish_prompt __kpp_prompt_orig
    function fish_prompt
        __kpp_prompt_orig
        __kpp_osc "133;B"
    end
end
```

Reconnect, or source the file, to start.

### Notes

- **Directories KiTTY++ refuses.** A reported directory is used only when it
  is absolute and holds letters, digits, `/ . _ - ~` and UTF-8 characters. A
  path with a space, a quote or a shell metacharacter is refused and uploads
  go to the home directory. Nothing reported is ever run.
- **Older snippets.** The OSC 7 snippet KiTTY++ published before
  (`__osc7_cwd`) keeps working; it can stay or be replaced by the one above.
- **bash older than 4.4** has no `PS0`: no output mark, and the exit code
  goes with every prompt, an empty Enter included.
- **Prompt themes** (oh-my-zsh, powerlevel10k, Starship) do not get in the
  way: the snippets use the prompt hook, not `PS1`. The optional `B` line is
  the exception: it is part of `PS1`, see above.

## Program status (OSC 7501)

A program reports what it is doing: working (with a percentage or without),
blocked (waiting for a permission, an answer or a login), done, failed or
idle. The specification is at
<https://www.superlogical.com/rex/docs/build/program-status>.

KiTTY++ shows on the window's taskbar button:

- **Overlay icon:** the most urgent state of all the window's reports: blocked
  (amber), failed (red), working (blue), done (green). Done and failed go once
  the window has been in front.
- **Progress bar:** the percentage while working, busy without one. Yellow
  while blocked.
- **Flashing:** the button flashes when a program becomes blocked while the
  window is in the background.

These follow **Progress and status on the taskbar button (OSC 9;4, 7501)**.
OSC 9;4 uses the same bar, so whichever program reports last wins.

**Notices:** blocked, done and failed raise a notice, "app: state" with the
program's message below. It follows **Desktop notifications** (by default only
while the window is not focused) and its limits: one notice every 2 seconds
per window.

*Working* and *blocked* end at the next prompt (OSC 133, see the shell
snippets) and when the session ends. Without the snippets they stay until the
program reports again.

The one reply KiTTY++ sends is to the feature query `OSC 7501 ; ?`. It is
answered with the same `?` while either setting is on. Nothing a program
reports is sent back.

Try it by hand:

```sh
b64() { printf '%s' "$1" | base64 | tr -d '\n'; }
printf '\033]7501;state=working:app=demo:progress=40\033\\'
printf '\033]7501;state=blocked:kind=question:app=demo:msg=%s\033\\' "$(b64 'Continue?')"
printf '\033]7501;state=done:app=demo\033\\'
printf '\033]7501;state=clear\033\\'
```

## tmux, screen, zellij and dtach

A multiplexer sits between the program and KiTTY++. tmux, GNU screen and
zellij draw the screen themselves and drop most of these sequences; dtach only
detaches and re-attaches and passes everything on unchanged. Measured with
tmux 3.6, screen 4.09, zellij 0.44 and dtach 0.9, a program sending the plain
sequence:

| Sequence | tmux, defaults | tmux, the 3 lines below | screen | zellij | dtach |
|---|---|---|---|---|---|
| OSC 52 clipboard | dropped | passed | dropped | passed | passed |
| OSC 8 hyperlinks | dropped | passed | dropped | dropped | passed |
| OSC 7, 133 (the shell snippets above) | dropped | passed (the snippets wrap) | passed (the snippets wrap) | dropped, wrapped too | passed |
| OSC 9, 777, 99 notices, 9;4 progress, 7501 status | dropped | wrapped only | wrapped only, at most 768 bytes | dropped, wrapped too | passed |

"Wrapped only": the sequence gets through only when the program wraps it for
the multiplexer, as the shell snippets and `kpp-osc` below do. Most programs
do not.

### tmux: `~/.tmux.conf`

```
set -g allow-passthrough on        # wrapped sequences (tmux 3.3 and later)
set -g set-clipboard on            # OSC 52 from programs inside tmux
set -as terminal-features ',xterm*:hyperlinks'   # OSC 8
```

The last line matches the terminal type KiTTY++ sends (Connection > Login >
Terminal details > Terminal-type string), `xterm` and `xterm-256color`
alike. Reload with
`tmux source-file ~/.tmux.conf`, or start a new tmux server.

### screen

No setting lets the plain sequences through. A wrapped sequence passes when it
is at most about 768 bytes long; a longer one is dropped whole. That is an OSC
52 copy of more than about 560 characters, or an OSC 7501 report with a long
message. For the clipboard, notices and program status, use tmux or dtach.

### zellij

Passes OSC 52 by itself. It drops the rest, wrapped or not, so neither the
shell snippets nor `kpp-osc` get through: no current directory for uploads,
no prompt marks, no notices, progress or program status from inside zellij.

### dtach

Passes every sequence, of any length, with no setting. It keeps no screen: what
a program sends while no window is attached is lost, a notice included, and a
full-screen program redraws when a window attaches again (`-r winch`). For
re-attaching per window, see [TERMINAL-REATTACH.md](TERMINAL-REATTACH.md).

### Wrapping your own signals: `kpp-osc`

A script of your own (a backup, a build, a long copy) can send notices,
progress and status through tmux and screen with this helper (not through
zellij). It writes to the
terminal directly, so it also works from a script whose output is redirected.

```sh
#!/bin/sh
# ~/.local/bin/kpp-osc   (chmod +x)
# Send one OSC sequence to the terminal, wrapped for tmux and screen.
#   kpp-osc '9;Backup finished'            a notice
#   kpp-osc '9;4;1;50'                     taskbar progress 50 %
#   kpp-osc '7501;state=done:app=backup'   program status
seq=$1
if [ -n "$TMUX" ]; then
    out=$(printf '\033Ptmux;\033\033]%s\007\033\\' "$seq")
elif [ -n "$STY" ]; then
    out=$(printf '\033P\033]%s\007\033\\' "$seq")
else
    out=$(printf '\033]%s\007' "$seq")
fi
printf '%s' "$out" > /dev/tty 2>/dev/null || printf '%s' "$out"
```

A program that writes the sequences itself, such as a coding agent, cannot be
wrapped from the shell. Its notices, progress and status reach KiTTY++ only
outside tmux, screen and zellij, or under dtach.

## Notifications and progress, by hand

```sh
printf '\033]9;Build done\007'                  # OSC 9 notice
printf '\033]777;notify;Backup;Finished\007'    # OSC 777: title, text
printf '\033]9;4;1;50\007'                      # taskbar progress 50 %
printf '\033]9;4;0\007'                         # progress off
```

Inside tmux or screen use `kpp-osc` instead of `printf`:
`kpp-osc '9;Build done'`.
