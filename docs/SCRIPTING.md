# Scripts and the script bridge

KiTTY++ can type a script into a session for you, line by line. It can wait
for the host's prompt before each line. A script comes from a file
(Session > Scripting
or the terminal window's menu > Tools > Run script file) or from another
program on the same desktop (the script bridge). Both use the same script
engine with the session's settings.

0. [Which one to use](#which-one-to-use): script file, login script,
   auto-command or Type text file.
1. [Script files](#script-files): the format and the settings that make a
   script wait for the host.
2. [The script bridge](#the-script-bridge): another program hands a
   terminal a script and gets the prompt back.
3. [Examples](#examples): a complete program in PowerShell and in
   AutoHotkey v2.

Every setting with its `kitty.ini` key: [KITTY-INI.md](KITTY-INI.md). Every
session key: [SESSION-FILES.md](SESSION-FILES.md).

## Which one to use

KiTTY++ has four ways to type text into a session for you. They are set in
different places and behave differently:

| | Script file | Login script | Auto-command | Type text file |
|---|---|---|---|---|
| **Set on** | Session > Scripting > **Script file:** with **Run the script on connect**, or the terminal window's menu > Tools > **Run script file** | Connection > Login > **Login script (wait-for and send-text, one per line):** | Connection > Login > **Auto-command after login** | the terminal window's menu > Tools > **Type text file** |
| **The text is** | a file of yours | in the session, encrypted like its password | in the session | a file of yours |
| **Runs** | at connect, or when you start it | once, when the session opens | once, after login | when you start it |
| **Waits for the host** | yes, per line: wait-for text, halt-on text, timeout, conditions in the file | yes: each line to send waits for the text before it | no: fixed pauses only (`\p`, `\s05`) | no: fixed pauses only (`\p`, `\s05`) |
| **Other programs** | can hand one over (the script bridge) | no | no | no |
| **Use it for** | command sequences for many hosts, maintenance runs | a login dialogue: a menu, a second password, `sudo` | one command after every login | classic KiTTY files with the escapes below |

A **script file** is a list of commands, one per line (the next sections):

```
uname -a
df -h
```

A **login script** alternates the text to wait for and the text to send,
one per line. It is stored in the session, so it may hold a password:

```
login:
admin
assword:
my-password-example
```

An **auto-command** is text in the session: `cd /var/log`.

**Type text file** takes the same kind of text from a file, one line per
timer tick.

### Escapes in the auto-command, the login script and Type text file

The auto-command, the send lines of a login script, a file sent with
**Type text file** and the AutoText shortcuts understand these escapes. A
script file does not:

| Escape | Effect |
|---|---|
| `\n` | Enter, then the next line |
| `\p` | a pause of 1 second |
| `\s05` | a pause of 5 seconds (two digits) |
| `\t`, `\r`, `\h` | Tab, carriage return, Backspace |
| `\c` | a break to the session |
| `\kHH` | a key by its Windows key code (two hex digits) |
| `\xHH` | a byte typed into the session, for example `\x03` for Ctrl+C (two hex digits) |
| `\\` | a backslash |
| `\` at the end | no Enter after the text |

None of them waits for the host: a pause is a fixed time.

When a session has a login script and a script file to run on connect, the
login script starts first. The script file follows. If the login script has
not finished by then, the Terminal Window's Event Log notes it.

## Script files

A script file is a plain text file. Each line is typed into the session as
if you typed it:

```
uname -a
df -h
exit
```

A commented example of a script file, with conditions: [examples/logon-script.ksh](examples/logon-script.ksh).

**Starting a script:**

- At connect: Session > Scripting > **Run the script on connect** with
  **Script file:** (session keys `ScriptMode`, `Scriptfile`).
- In a running session: the window menu (right-click the title bar, or
  Ctrl+right-click inside the terminal) > Tools > **Send recorded
  script** opens a file picker. Tools > **Stop script** in the same menu
  ends a running script.

One script runs at a time. A second one is refused with "script not sent:
another script is running" in the Terminal Window's Event Log.

**Waiting for the host** (Session > Scripting, all session keys):

| Setting | Key | What it does |
|---|---|---|
| **Wait for a prompt before each line** | `ScriptEnable` | The next line goes out only after the host's output ends with the wait-for text. Off: lines go out on the line delay alone. |
| **Wait-for text:** | `ScriptWait` | The end of your prompt, for example `$ ` (most prompts end in a space). Several alternatives each in double quotes: `"$ " "# "`. |
| **Except for first command** | `ScriptExcept` | The first line goes out at once, without waiting. For a host that greets you with something that is not your prompt. |
| **Halt-on text:** | `ScriptHalt` | When the host's output contains this text, the script stops at once ("script halted"). Empty: never. |
| **Timeout (s):** | `ScriptTimeout` | How long one wait may take. Then the script stops ("script timeout !"). Default 15. |
| **Line delay (ms):** | `ScriptLineDelay` | Pause between lines. Default 5. |
| **Character delay (ms):** | `ScriptCharDelay` | Pause between characters, for hosts that drop fast input. Default 0. |
| **CR/LF translation:** | `ScriptCRLF` | The line end sent: **Off** the file's own, **no LF** without the line feed, **CR** carriage return only (what most interactive hosts expect), **Rec** for a script recorded by RuTTY or classic KiTTY (see below). |

**Rec: old recorded scripts.** RuTTY and classic KiTTY could record a
session into a script file. Their recorder ended every line with an LF of
its own, so a typed CR is in the file as CR LF, an LF as LF LF and CR LF
as CR LF LF LF. **CR/LF translation: Rec** sends what was typed. Use it for
such old recorded scripts only. KiTTY++ cannot record a session yet: a
recorder is planned for a future release.

**Conditions in the file.** With **Use conditions from file** on
(`ScriptCondUse`), a line that starts with the **Start of condition/comment
line:** character (`ScriptCondLine`, `:` by default) is the wait-for text
for the next line only. A line that starts with the character twice (`::`)
is a comment. With the switch off, every such line is a comment.

```
:login:
root
:assword:
my-password-example
:$
uname -a
```

**The end of a script.** A script ends as soon as its last line is sent
("finished sending script"). It does not wait for the prompt that line
brings back, so a last `exit` ends it without a timeout. To end only once
the last command is done, add a condition line after it: the script then
waits for that text, with the timeout and the halt-on text armed. Until the
prompt after the last line arrives, the next script waits for it before its
first line ("script waits for the prompt after the previous script's last
line").

**The Terminal Window's Event Log** holds every start ("sending script to
host ..."), end, halt, timeout and refusal. Application > KiTTY++ Settings
> Automation > **Script engine** turns the engine off for the installation
(`scriptmode=no`, in `kitty.ini` with a folder store, else in the registry under
`HKCU\Software\kapper.net\KiTTY`). A script is then
refused with its reason in the Terminal Window's Event Log.

## The script bridge

Another program on the same desktop (an AutoHotkey script, a test tool,
anything that can send `WM_COPYDATA`) hands a terminal a script. The
terminal plays it as window menu > Tools > Run script file plays a
file, with the session's settings from the table above. At the level `prompt` the program
can also bind a window of its own: it then gets one message each time the
host's output ends with a pattern it set. The message holds the pattern
that matched, never a byte of the session.

### Turning it on

Both switches are off by default:

1. The installation's level: Application > KiTTY++ Settings > Automation >
   Script bridge > **Script bridge level:** (`scriptbridge`, in `kitty.ini`
   with a folder store, else in the registry under
   `HKCU\Software\kapper.net\KiTTY`):
   - **Off** (`no`): every message is refused.
   - **Inbound: scripts in** (`inbound`, also written `yes`): scripts in,
     nothing goes back.
   - **Inbound + prompt event out** (`prompt`): also the prompt messages.
2. The session's switch: Session > Scripting > **Accept script bridge
   messages for this session** (session key `AcceptScriptBridge`), or on a
   running window the window menu > Tools > **Accept script bridge
   (Session > Scripting)**.

With the script engine off (`scriptmode=no`) the bridge is off too.

### Finding the terminal

One kitty.exe process holds one terminal window. Send the messages to that
window: the process's visible top-level window that is not a dialog box.
There are two ways to the process:

1. **Your program starts the terminal and keeps its process ID.** Nothing to
   search, nothing to confuse. PowerShell:
   `$p = Start-Process kitty.exe -ArgumentList '-load', 'web01' -PassThru`,
   then `$p.Id`. AutoHotkey's `Run()` returns it in its `OutputVarPID`
   parameter. Wait until the session has connected: before that `_send` is
   refused ("no session is running").
2. **A terminal that is already open: find it by its session.** The
   launcher starts a session as `kitty.exe -load "<session>"`, so the
   kitty.exe (or kitty_portable.exe) with that in its command line is the
   one. Both examples do this with `-Session web01` (`-session web01` in
   AutoHotkey). They read the command line with `NtQueryInformationProcess`
   (Windows 8.1 or later). A WMI query (`Win32_Process`) gives it too, but
   not everywhere: in Windows Sandbox it returns nothing. Two open windows of
   the same session match twice: the examples then stop instead of picking
   one.

Never find the window by its title: a session or a host can set any title.
Another program can open a window with the same title.

### The key

Every message starts with the script bridge key, then one NUL byte. It is
the script bridge's own key, not the broadcast key:

- **The installation's key:** Application > KiTTY++ Settings > Automation >
  Script bridge > **Script bridge key of this installation:**, with **Copy**
  and **New key**. Unset, it is derived from the installation's location
  (`sb-` and 16 hex digits), the same in each of its terminals. **New key**
  sets a random one. It is `scriptbridgekey`, in `kitty.ini` with a folder
  store, else in the registry under `HKCU\Software\kapper.net\KiTTY`.
- **A session's own key:** Session > Scripting > **Script bridge key:**
  (session key `ScriptBridgeKey`). It wins over the installation's.
  **Clear** goes back to the installation's key.

A message with another key is refused. A program meant for one
installation cannot reach the terminals of another (a portable copy beside
an installed one). A key of their own keeps a program to a few sessions.

### The messages

`dwData` of `WM_COPYDATA` selects the message. Send them with
`SendMessage` or `SendMessageTimeout`. The answer is `1` when the terminal
accepted the message and `0` when it refused it. The reason of every
refusal is in the Terminal Window's Event Log.

| `dwData` | Name | Direction | Data | Level |
|---|---|---|---|---|
| 5 | `_send` | in | `<key>\0<script>`: the script, lines ending in LF or CRLF. At most 65536 bytes for the message, key included: a larger one is refused, never cut. A script already running refuses it. | `inbound` |
| 6 | `_enable` | in | `<key>\0` then `1`: the window whose handle is in `wParam` gets the `_prompt` messages from now on. `0`: no more. | `prompt` |
| 7 | `_set` | in | `<key>\0<pattern>`: the prompt pattern in the form of the wait-for text, plain or several alternatives in double quotes. At most 255 bytes. Empty clears it. | `prompt` |
| 8 | `_prompt` | out | The pattern that matched, without a NUL. `wParam` is the terminal's window handle. | |

`9` and `10` are kept for session output and refused.

The script and the pattern are bytes as they go to the host and come back
from it: for a session whose remote character set is UTF-8, send UTF-8.

### The bound window

`_enable` binds a window by its handle. The window must exist and must not
be the terminal itself. Its process must run as the same Windows user at an
integrity level not below the terminal's. Before every `_prompt` the
terminal checks the handle again, window and process: a window that is
gone ends the binding. The messages wait in a queue of 256 KiB and go out
one at a time, each with a timeout of 1000 ms. A window that does not
answer in time is unbound. The session goes on.

The window needs a message loop: `_prompt` arrives as a sent message while
your program waits for it.

### The Terminal Window's Event Log

Every accepted script, the binding, its end and every refusal is a line in
the Terminal Window's Event Log, without the script or the pattern:

| Line | Meaning |
|---|---|
| `script bridge: script of N bytes accepted` | `_send` taken, the script runs |
| `script bridge: prompt pattern set (N alternatives)` / `... cleared` | `_set` taken |
| `script bridge: prompt events go to window 0x... in process N` | `_enable 1` taken |
| `script bridge: prompt events stopped, <why> (N sent)` | the binding ended: `_enable 0 from the program`, `the target window is gone`, `no answer within 1000 ms`, `the bridge or this session's arm is off`, `a new target` |
| `script bridge refused: the script bridge is off (KiTTY++ Settings > Automation > Script bridge, scriptbridge=no)` | the level is Off |
| `script bridge refused: the script engine is off (KiTTY++ Settings > Automation, scriptmode=no)` | the script engine is off |
| `script bridge refused: this session does not accept it (...)` | the session's switch is off |
| `script bridge refused: key '...' is not the key of this session ('...')` | wrong key |
| `script bridge refused: _enable needs the level "Inbound + prompt event out" (scriptbridge=prompt, this install: inbound)` | `_enable` or `_set` at level `inbound` |
| `script bridge refused: message of N bytes, the limit is 65536` | too large |
| `script bridge refused: malformed message (...)` | no key before the first NUL byte, an empty script, `_enable` not `1` or `0`, a pattern over 255 bytes |
| `script bridge refused: id N is not an inbound message` | `dwData` 8, 9 or 10 sent to the terminal |
| `script bridge refused: no session is running` | `_send` before the session connected |
| `script bridge refused: the _enable target ...` | no window handle in `wParam`, the terminal itself, another user, a lower integrity level |
| `script not sent: another script is running` | `_send` while a script runs |

**Not a security boundary.** Any program running as the same user can send
these messages. It could type into the session by other means too. Like
the broadcast, the two switches stop accidents and keep the bridge off until
it is seriously wanted.

**Not RuTTY's interface.** Scripts written for RuTTY's or classic KiTTY's
AutoHotkey interface need changes: see [KNOWN-ISSUES.md](../KNOWN-ISSUES.md).

## Examples

Two complete programs do the same round: `_set` the pattern, `_enable 1`
with their own window, `_send` the script, wait for one `_prompt`, print
it, `_enable 0`. Both take the kitty.exe process ID or the session name
(see [Finding the terminal](#finding-the-terminal)), and the key. Both are
run as published by the release tests, each way: Windows PowerShell 5.1
and PowerShell 7, AutoHotkey 2.0.31.

The arguments in the command lines below:

- `1234`: the kitty.exe process ID. Or `web01`: the saved session of an
  open terminal. See [Finding the terminal](#finding-the-terminal).
- `<key>`: the script bridge key, from Session > Scripting > **Script
  bridge key:** (the **Copy** button puts it on the clipboard). See
  [The key](#the-key).
- `commands.txt`: the script file, one command per line.
- `'$ '`: the prompt pattern. The program waits until the host's output
  ends with it.

**PowerShell:** [examples/scriptbridge-example.ps1](examples/scriptbridge-example.ps1)

```
powershell -File scriptbridge-example.ps1 -ProcessId 1234 -Key <key> -ScriptFile commands.txt -Pattern '$ '
powershell -File scriptbridge-example.ps1 -Session web01 -Key <key> -ScriptFile commands.txt -Pattern '$ '
```

The heart of it, one message:

```csharp
byte[] k = Encoding.ASCII.GetBytes(key);
byte[] data = new byte[k.Length + 1 + rest.Length];   // "<key>\0<rest>"
Buffer.BlockCopy(k, 0, data, 0, k.Length);
Buffer.BlockCopy(rest, 0, data, k.Length + 1, rest.Length);
// ... COPYDATASTRUCT { dwData = id, cbData = data.Length, lpData = data }
SendMessageTimeoutW(term, WM_COPYDATA, wParam, ref cds, 3, 10000, out result);
```

The prompt message arrives at a hidden window made with plain Win32 calls
(`RegisterClassW`, `CreateWindowExW`). The waiting loop calls `PeekMessageW`
so the message gets in. WinForms is not used: it differs between Windows
PowerShell 5.1 and PowerShell 7.

**AutoHotkey v2:** [examples/scriptbridge-example.ahk](examples/scriptbridge-example.ahk)

```
AutoHotkey64.exe scriptbridge-example.ahk 1234 <key> commands.txt "$ "
AutoHotkey64.exe scriptbridge-example.ahk -session web01 <key> commands.txt "$ "
```

The prompt message arrives in an `OnMessage(0x004A, ...)` handler of a
hidden `Gui()`. Its `Hwnd` goes into `wParam` of `_enable`.

Both print `script accepted`, then `prompt: <pattern>`. Exit code 0 done,
1 refused or not delivered, 2 no prompt in time.
