# FIDO security keys (YubiKey and others) with KiTTY++

A FIDO security key keeps the SSH secret on the device. It never leaves the
device and cannot be copied. Every login needs the device plugged in and a
touch. A PIN-protected key also needs the PIN.

KiTTY++ uses these keys through **kageant**: kageant holds the key. For each
login it sends the signing request to the device through Windows. kitty.exe, klink, kscp and
ksftp take the key from kageant like any other agent key.

## What you need

- **Windows 10 1903 or later.** kageant reaches the device through Windows'
  security-key support (webauthn.dll). Older Windows refuses to add the key.
- **A FIDO2 security key.** `ed25519-sk` needs a device that supports
  Ed25519 (YubiKey 5 firmware 5.2.3 or later). Every FIDO2 key supports
  `ecdsa-sk`.
- **OpenSSH 8.2 or later on the server.** Older servers do not know the key
  types `sk-ssh-ed25519@openssh.com` and `sk-ecdsa-sha2-nistp256@openssh.com`.
- Only for OpenSSH's own tools on your PC (`ssh-keygen`, `ssh-add`):
  **OpenSSH for Windows 8.9 or later**. Check with `ssh -V`. The OpenSSH
  built into Windows 10 (8.1) is too old: install a newer one from
  github.com/PowerShell/Win32-OpenSSH. kittygen and kageant need none.

## How to create a key

Whichever tool creates the key, Windows shows its security-key prompt: enter
the device's PIN if it has one, then touch the device.

### With kittygen

1. Under **Parameters**, pick **FIDO**.
2. **Security key type:** *Ed25519*, or *NIST p256* for a device without
   Ed25519.
3. Tick **PIN required (verify-required)** for the PIN at every login.
4. **Generate**, then the PIN and the touch in Windows' prompt.
5. Change **Key comment**: the name the key shows in kageant's key list and
   log.
6. **Save private key** writes the key file as a PPK. kageant loads it like
   any key file.
7. For the server: copy the line from **Public key for pasting into
   authorized_keys file** (**Copy**).
8. For OpenSSH (`ssh-add`, `ssh`): **Conversions > Export OpenSSH key**
   writes the same key in OpenSSH's format.

### With kittygen-cli

```
kittygen-cli -t ed25519-sk -C "you@yourpc" -o %USERPROFILE%\.ssh\id_yubikey.ppk
kittygen-cli %USERPROFILE%\.ssh\id_yubikey.ppk -L
```

The first line generates the key (a PPK). The second one prints the line for
the server's `authorized_keys`. Options:

- `-t ecdsa-sk` instead of `-t ed25519-sk`: for a device without Ed25519.
- `--verify-required`: every login requires the device's PIN as well as the
  touch.
- `-O private-openssh-new -o <file>`: the key file in OpenSSH's format, for
  `ssh-add`.

### With OpenSSH's ssh-keygen

```
ssh-keygen -t ed25519-sk -C "you@yourpc"
```

After the touch ssh-keygen prompts for a passphrase for the key file.

The result is two files in `%USERPROFILE%\.ssh`:

| File | What it is |
|---|---|
| `id_ed25519_sk` | the key file: the public key and a key handle that identifies which of the device's keys to use. It holds **no secret**. |
| `id_ed25519_sk.pub` | the public key, one line, for the server's `authorized_keys` |

Options worth knowing:

- `-f <file>`: the key file's name, for example
  `-f %USERPROFILE%\.ssh\id_yubikey` (in PowerShell
  `-f $env:USERPROFILE\.ssh\id_yubikey`). Without it ssh-keygen proposes
  `id_ed25519_sk` and requires a confirmation before it overwrites an
  existing one.
- `-C <comment>`: the name the key shows in kageant's key list and log. The
  default is `user@pc` for every key, so give each key its own.
- `-O verify-required`: every login requires the device's PIN as well as the
  touch.
- `-t ecdsa-sk` instead of `-t ed25519-sk`: for a device without Ed25519.

### Passphrase or PIN?

The key file holds no secret: a copy of it is useless without the device. A
passphrase only matters when someone has the file AND the device. The
device's PIN (`-O verify-required`) guards that case better:

| | checked by | when |
|---|---|---|
| passphrase on the key file | ssh-add (or kageant) | once, when the key is loaded |
| device PIN (`-O verify-required`) | the device | at every login |

A YubiKey locks its FIDO part after 8 wrong PINs. The server can demand the
PIN (`verify-required` in `authorized_keys`, below). An empty passphrase
with `-O verify-required` is the usual choice. The device needs a FIDO PIN
set for this: Windows prompts for one when the key is created if it has none.

## How to put the public key on the server

Add the line from `id_ed25519_sk.pub` to `~/.ssh/authorized_keys` on the
server, as for any other key. With a session that can already log in (by
password or another key), from a Command Prompt:

```
type %USERPROFILE%\.ssh\id_ed25519_sk.pub | klink -load "myserver" "umask 077; mkdir -p ~/.ssh; cat >> ~/.ssh/authorized_keys"
```

For a key created with `-O verify-required`, put `verify-required` in front of
the line in `authorized_keys`, so the server also refuses a login without
the PIN:

```
verify-required sk-ssh-ed25519@openssh.com AAAA... you@yourpc
```

## How to load the key into kageant

kageant reads the key file itself: in its key list, **Add Key** and pick
the PPK from kittygen, or `id_ed25519_sk` from ssh-keygen (the file without
`.pub`). kageant prompts for a passphrase on the file as for any key file. With **Load remembered keys at startup** on (tray
menu), kageant loads the key again at its next start.

On the command line: `kageant.exe %USERPROFILE%\.ssh\id_ed25519_sk`.

Or with OpenSSH's `ssh-add` (the OpenSSH key file, not the PPK), pointed at
kageant. A key added this way is gone when kageant ends:

1. In kageant's tray menu, turn on **Register as Windows OpenSSH agent**.
   This writes `%USERPROFILE%\.ssh\kageant.conf` with kageant's pipe.
2. In PowerShell:

```powershell
$line = Get-Content "$env:USERPROFILE\.ssh\kageant.conf" -TotalCount 1
$env:SSH_AUTH_SOCK = ($line -replace '^IdentityAgent "(.*)"$','$1') -replace '/','\'
ssh-add "$env:USERPROFILE\.ssh\id_ed25519_sk"
ssh-add -l
```

`ssh-add -l` lists the key with `(ED25519-SK)`. kageant's key list shows it
as *Ed25519 security key* (or *NIST p256 security key*).

⚠️ Without `SSH_AUTH_SOCK`, ssh-add talks to the Windows ssh-agent service,
not to kageant. Check it with `$env:SSH_AUTH_SOCK`: it shows
`\\.\pipe\pageant.<user>.<hash>`.

`SSH_AUTH_SOCK` holds for this PowerShell window only. Do not set it as a
permanent user variable: kageant's pipe name can change at the next start.
kageant rewrites `kageant.conf` each time it starts. OpenSSH's `ssh`
does not need the variable: it finds kageant through `~/.ssh/config`.

## How to log in with it

- **kitty.exe, klink, kscp, ksftp:** nothing to set. Session > Connection >
  SSH > Auth > **Attempt authentication using kageant** is on by default.
  The key is offered from kageant. To offer only this key, set
  Session > Connection > SSH > Auth > Credentials > **Private key file for
  authentication** to `id_ed25519_sk.pub`.
- **OpenSSH `ssh`:** with *Register as Windows OpenSSH agent* on, `ssh` uses
  kageant too.

At each login Windows shows its security-key prompt: touch the device (and
enter the PIN for a `verify-required` key). kageant shows no confirmation
box of its own for these keys, whatever its confirmation setting: the touch
is the confirmation.

With several security keys in kageant, the client offers them one after
another: Windows prompts once for each key until one is accepted. Set the
session's key file to the key's `.pub` (above) to get one prompt only.

## How to use the key on another PC

The secret stays on the device. The key file only identifies which of
the device's keys to use. To log in from another PC you need both: the device and a
copy of the key file.

1. Copy the key file to the other PC: the PPK from kittygen, or
   `id_ed25519_sk` from ssh-keygen. A copy is useless without the device.
   With a passphrase on the file, it is also useless with the device but
   without the passphrase.
2. On the other PC: KiTTY++ on Windows 10 1903 or later.
3. Plug in the device.
4. Load the key file into kageant there: **Add Key**, kageant's command
   line or a startup key, as above.
5. Log in as on the first PC: Windows prompts for the touch (and the PIN).

The server needs nothing new: the line in `authorized_keys` is the same
for every PC.

Without the key file the device alone cannot log in: kageant cannot read
keys from the device yet (resident keys, below).

## When a login fails

kageant writes the reason to its agent log as `security key: <reason>`:

| Reason | What to do |
|---|---|
| cancelled or timed out | the prompt was closed, nobody touched the device within 60 seconds, or the device is not plugged in (Windows prompts to insert it) |
| key not found on the inserted security key | plug in the device the key was created on |
| another Windows Hello or security key request in progress | finish or close the other prompt first |
| Windows refused the request: ... | Windows' own error name and code |

When kageant refuses to add the key with *security keys require Windows 10 1903
or later (webauthn.dll)*, this Windows has no security-key support.

## Not there yet

- kageant's `-h` destinations on the command line take PPK key files only:
  use the PPK from kittygen there.
- Resident keys (`ssh-keygen -O resident`, `ssh-add -K`): Windows cannot
  read the keys stored on a security key (webauthn.dll has no way to list
  them), so `ssh-keygen -K` fails on Windows. Run `ssh-keygen -K` on Linux
  or a Mac and copy the key file it writes to Windows: kageant loads it,
  kittygen saves it as a PPK.
