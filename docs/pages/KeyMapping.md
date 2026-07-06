<div style="text-align: center;"><iframe src="gad.html" frameborder="0" scrolling="no" style="border: 1px solid gray; padding: 0; overflow:hidden; scrolling: no; top:0; left: 0; width: 100%;" onload="this.style.height=(this.contentWindow.document.body.scrollHeight+5)+'px';"></iframe></div>
## Enhanced Key Mapping

### Ctrl+Arrow Word Jump

KiTTY now supports **Ctrl+Left Arrow** and **Ctrl+Right Arrow** for word-boundary cursor movement in the terminal. This sends the standard xterm-compatible escape sequences so that the remote shell or line discipline can interpret them as word jumps.

| Key combination | Escape sequence sent | Action |
|----------------|----------------------|--------|
| **Ctrl+Left**  | `\x1B[1;5D`          | Jump to previous word boundary |
| **Ctrl+Right** | `\x1B[1;5C`          | Jump to next word boundary     |

These sequences are recognised by most modern shells (bash, zsh, fish) and by any application that uses the **readline** or **libedit** libraries. If your shell does not move by word, ensure your `~/.inputrc` or shell configuration binds the sequences correctly. For example, in **bash**:

```bash
bind '"\e[1;5C": forward-word'
bind '"\e[1;5D": backward-word'
```

### Modifier key support

With **MOD_KEYMAPPING** now unconditionally enabled, KiTTY sends fully-qualified xterm-style modifier sequences for arrow keys and other special keys. This means Shift, Alt and Ctrl combinations are correctly distinguished and passed to the terminal.

| Modifier | Arrow sequence format |
|----------|-----------------------|
| Normal   | `\x1B[A` / `\x1B[B` / `\x1B[C` / `\x1B[D` |
| Shift    | `\x1B[1;2A` ... |
| Alt      | `\x1B[1;3A` ... |
| Ctrl     | `\x1B[1;5A` ... |
| Ctrl+Shift | `\x1B[1;6A` ... |
| Ctrl+Alt | `\x1B[1;7A` ... |
| Ctrl+Alt+Shift | `\x1B[1;8A` ... |

This behaviour is automatically active for all builds and does not require any configuration change.
