# OSC 7 shell integration — directory-aware uploads

KiTTY can track the **current working directory of your remote shell** and use
it as the default target for **drag-and-drop uploads** and **Start WinSCP**,
instead of always landing in your remote home directory.

It learns the directory from **OSC 7**, a small, standard escape sequence that a
shell emits on every prompt to report where it is:

```
ESC ] 7 ; file://HOSTNAME/current/path BEL
```

This is **data only** — KiTTY validates the path and stores it; it never runs
anything the shell sends. (It is the safe replacement for KiTTY's old
title-scan "local command" mechanism.)

## 1. Turn it on in KiTTY

Per session: **Connection → SSH → KSCP and WinSCP → “Track remote directory
(OSC 7 shell integration)”**, then **Save**. It is **off by default**.

## 2. Make your remote shell emit OSC 7

Some setups already do this — many prompt frameworks (Starship, oh-my-zsh /
oh-my-posh themes, `ble.sh`, `vte.sh` shipped by several Linux distros in
`/etc/profile.d/`) emit OSC 7 already, and other terminals (VTE/GNOME Terminal,
WezTerm, iTerm2) rely on it too. **Check first** — if drag-drop already lands in
your current directory, you're done.

If not, add one of the snippets below to the remote account's shell startup file.

### bash — `~/.bashrc`

```bash
# Report the working directory to the terminal (OSC 7) before every prompt.
__osc7_cwd() { printf '\033]7;file://%s%s\007' "${HOSTNAME}" "${PWD}"; }
case "$PROMPT_COMMAND" in
  *__osc7_cwd*) ;;                                  # already installed
  *) PROMPT_COMMAND="__osc7_cwd${PROMPT_COMMAND:+; $PROMPT_COMMAND}" ;;
esac
```

### zsh — `~/.zshrc`

```zsh
__osc7_cwd() { printf '\033]7;file://%s%s\007' "${HOST}" "${PWD}"; }
autoload -Uz add-zsh-hook
add-zsh-hook precmd __osc7_cwd
```

### fish — `~/.config/fish/config.fish`

```fish
function __osc7_cwd --on-variable PWD --on-event fish_prompt
    printf '\033]7;file://%s%s\007' (hostname) "$PWD"
end
```

Reconnect (or re-source the file and `cd` once) so KiTTY sees the first report.

## 3. Try it

```sh
cd /var/tmp        # any writable directory
```

Then drag a file from Windows Explorer onto the terminal — it should upload into
`/var/tmp`. **Start WinSCP** (in the tools menu) opens there too. The transfer
window's header line shows the exact `user@host:directory` target.

## Notes & safety

- **Off = home.** With tracking off (or no OSC 7 received yet), uploads go to the
  remote home directory, exactly as before.
- **Only plain absolute paths are used.** For safety, KiTTY accepts a reported
  path only if it is absolute and contains just letters, digits, `/ . _ - ~` and
  UTF-8 characters. A directory whose path contains a space, quote, or shell
  metacharacter (`; | & $ \` < > * ? ( ) [ ] { }`) is **rejected** and the upload
  falls back to home — it is never spliced into a command. So the simple
  snippets above are enough; you do not need to %-encode anything for normal
  paths.
- **Fixed directory instead.** If you prefer a single hard-wired remote upload
  directory, use **Fixed remote upload directory** in the same panel — it is
  mutually exclusive with OSC 7 tracking.
