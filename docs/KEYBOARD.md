# The keyboard: what happens when you press a key

A terminal has no keys. What travels to the machine at the other end is a
sequence of **bytes**, and the whole of *Terminal → Keyboard* is about which
bytes a key produces. Almost every "this key does nothing" report is a
disagreement between the sequence KiTTY sends and the one the program at the
other end is listening for.

This page explains the journey, then each setting on that panel, then the
questions people actually arrive with.

---

## The journey of one keystroke

1. **Windows** turns the physical key into a virtual key code plus modifier
   state — and, on an international layout, may turn it into a *character*
   first: AltGr+Q is `@` on a German keyboard before KiTTY sees anything.
2. **KiTTY** decides whether the keystroke is for *it* — a shortcut, a hotkey,
   a menu accelerator — or for the session.
3. If it is for the session, KiTTY encodes it: a printable character in the
   session's character set, or a **control sequence** for a key that has no
   character, such as F5, Home or Shift+Right.
4. Those bytes go down the connection. The program at the far end matches them
   against its **terminfo** entry for `$TERM` (see *Connection → Data →
   Terminal-type string*, `xterm` by default).

Step 3 is where the settings below live, and step 4 is why "correct" means
"what the other end expects", not "what looks tidy".

---

## The settings, one by one

### The Backspace key — `Control-H` or `Control-? (127)`

Which byte Backspace sends: `^H` (0x08) or DEL (0x7F). Modern Unix expects
DEL; some embedded and older systems expect `^H`. If Backspace prints `^H`,
`^?` or deletes the wrong way round, this is the setting — and the far end's
`stty erase` has to agree with it.

Ctrl+Backspace always sends the *other* one, so you can produce either without
changing the setting.

### The Home and End keys — `Standard` or `rxvt`

Two conventions for the same two keys. `Standard` sends the xterm/VT sequences;
`rxvt` sends rxvt's shorter ones. Pick whichever your host's terminfo describes;
if Home jumps somewhere odd in `vi` or `bash`, try the other.

### The Function keys and keypad

The one that causes the most confusion, because the modes differ in what they
do with **Shift** as much as in the base sequences.

| Mode | F1–F4 | F5–F12 | Shift+F1…F10 |
|---|---|---|---|
| `ESC[n~` (PuTTY's default) | `ESC[11~`…`ESC[14~` | `ESC[15~`…`ESC[24~` | folded into the number: F1 becomes F11 |
| `Linux` | `ESC[[A`…`ESC[[D` | `ESC[15~`… | as above |
| `Xterm R6` | `ESCOP`…`ESCOS` | `ESC[15~`… | as above |
| `VT400` | `ESC[11~`… | `ESC[15~`… | as above |
| `VT100+` | `ESCOP`…`ESCOS` | `ESC[15~`… | as above |
| `SCO` | `ESC[M`…`ESC[P` | `ESC[Q`… | separate SCO codes |
| **`Xterm 216+`** | `ESCOP`…, or `ESC[1;<mod>P` when modified | `ESC[<n>;<mod>~` | **modifier encoded, not folded** |

**`Xterm 216+` is the modern one.** It encodes Shift, Ctrl and Alt as a
*modifier parameter* rather than changing the key number, which is what every
current terminfo entry describes and what applications match against.

### Shift/Ctrl/Alt with the arrow keys — `Ctrl toggles app mode` or `xterm-style bitmap`

`xterm-style bitmap` sends `ESC[1;<mod>C` style sequences, so the far end can
tell Ctrl+Right from Right. `Ctrl toggles app mode` is PuTTY's older behaviour,
where Ctrl switches the arrows between normal and application mode instead of
being reported.

⭐ **KiTTY defaults to `xterm-style bitmap`**, unlike PuTTY, because that is what
makes **Ctrl+Left/Right jump by word** in a shell — the single most missed
behaviour when moving to a terminal that folds the modifier away.

### Word navigation (Left/Right arrows) — `Alt`, `Ctrl` or `Both`

A KiTTY addition, on top of the setting above: which modifier means "move by
word". `Both` is convenient if you switch between habits from other terminals.

### Enter key sends CR LF

Off by default: Enter sends CR (`\r`) alone, as terminals have since forever.
Some line-based hosts and serial devices want CR LF; this is for them. Turning
it on when the host does not expect it produces doubled blank lines.

### AltGr and Compose

Two unrelated things with confusingly similar names:

- **"AltGr acts as Compose key"** is PuTTY's two-keystroke Compose feature.
- **"Disable AltGr: right Alt acts as Alt, not as a character key"** decides
  whether AltGr keeps composing characters (off, the default — AltGr+Q is `@` on
  a German layout) or behaves as a plain Alt modifier (on). Turn it on only if
  you need Alt+key combinations that your layout has stolen for characters.

### Initial state of cursor keys / numeric keypad — `Normal`, `Application`, `NetHack`

The *initial* state only: full-screen programs switch these themselves through
the connection, and the setting merely says how a session starts. `NetHack`
maps the numeric keypad to `hjkl`-style movement.

---

## Questions people arrive with

**"F13–F24 do nothing."** On any keyboard made this century those keys *are*
Shift+F1…F12 — terminfo says so outright: xterm's `kf13` is `ESC[1;2P` and
`kf24` is `ESC[24;2~`, i.e. the ordinary function key with the Shift modifier
encoded. Only **`Xterm 216+`** produces that. In every other mode Shift is
folded into the key number, so Shift+F1 arrives as F11 and F13 upward simply
cannot be expressed.

Set *The Function keys and keypad* to `Xterm 216+` for the session, or put
`funkeys=xterm216` in the `[KiTTY]` section of `kitty.ini` to make it the
starting mode for **new** sessions and for Default Settings. Sessions you have
already saved keep the mode they were saved with — a keyboard that changes
under an existing session is exactly the surprise this setting exists to avoid.

**"Ctrl+Left/Right doesn't jump by word."** That needs the arrow keys reported
with their modifier — see *Shift/Ctrl/Alt with the arrow keys*, which KiTTY
already defaults to. If it stopped working, that setting was changed, or the
host's `$TERM` does not describe modified arrows.

**"Backspace deletes forwards / prints ^H."** The Backspace setting and the
host's `stty erase` disagree. Change either, not both.

**"A key works in PuTTY but not here, or the other way round."** Compare the
*Terminal-type string* (*Connection → Data*) first: the application is matching
terminfo for that name, so `putty` and `xterm` genuinely behave differently.

**"How do I see what a key actually sends?"** Run `cat -v` on the far end and
press it: the sequence is printed literally, `^[` being Escape. That answers the
question in one step, and turns "it does nothing" into something reportable.

---

## KiTTY's own keys

Separate from everything above, KiTTY reserves some combinations for itself —
shortcuts (*Window → Shortcuts*), the session launcher's global hotkeys, and the
special-command keys. Those never reach the session. If a host application wants
a combination KiTTY has taken, the shortcut can be changed or switched off; see
[FEATURES.md](FEATURES.md).
