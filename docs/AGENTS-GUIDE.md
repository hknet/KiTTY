valid for 0.85.1.20-beta

# KiTTY++ tasks for an AI agent

This page is for the AI agent. The human in control gives the job and
sets up the keys, the host keys and the records. Each block below is one
task. Start with "How to work with this guide". The AI agent needs
PowerShell, the KiTTY++ tools and this page, nothing else.

How to read a block:

- **status:** (some blocks, first line) the test state or "not available".
- **goal:** what the block achieves.
- **inputs:** every placeholder and every special term of the block. Each
  placeholder entry gives its form, one example value and where to get it.
- **command:** the exact lines to run. When a fence holds more than one
  command line, its first line marks how to use them:
  `# run these lines in order` or `# one of these forms`. Lines that start
  with `#` are comments. In an ini fence, lines that start with `;` are
  comments.
- **expect:** the exit code and the output of a good run.
- **failure:** the result of a bad run and what to do then.
- **never:** what not to do. A block that is only a prohibition has the goal
  "know what not to do" and no command line, or a command line with the safe
  form.
- **not available in 0.85.1.20-beta:** (some blocks, last line) what KiTTY++
  does not do yet.

Rules of this page:

- Each block works alone. A reference to another block is extra reading,
  never a required step.
- Commands are PowerShell unless the fence or a comment marks another shell.
- Placeholders are in angle brackets. Replace the brackets and the text
  between them with the value. Each block defines its own placeholders.
- klink (SSH commands), kscp (file copy), ksftp (SFTP) and kageant (SSH agent)
  are called by name. The KiTTY++ install folder must be in PATH. Else call
  each tool by its full path: `& "<install-dir>\klink.exe"`,
  `& "<install-dir>\kscp.exe"`, `& "<install-dir>\ksftp.exe"`,
  `& "<install-dir>\kageant.exe"`. `<install-dir>` is
  `%LOCALAPPDATA%\Programs\KiTTY` for the per-user MSI or
  `C:\Program Files\KiTTY` for the system MSI.
- A block marked "untested" has no test behind it in 0.85.1.20-beta.
- "not available in 0.85.1.20-beta" marks what KiTTY++ does not do yet.

Contents:

- How to work with this guide
- How a host key becomes trusted
- A. Get KiTTY++ and check it
  - block 1: Download a release
  - block 2: Check the Authenticode signature
  - block 3: Check the ZIP checksums
  - block 4: Check the installed version
  - block 5: Find the settings store and the logs of this install
- B. Host keys and batch mode
  - block 6: List the host keys the user already trusts
  - block 7: Scan the server's keys and compare them with the store
  - block 8: Layer 1: connect to a host whose key a trusted CA signed
  - block 9: Layer 2: accept-new (only when the job allows it for this host)
  - block 10: Layer 3: pin a stored key or a fingerprint the human confirmed
  - block 11: Jump hosts: pin or accept the jump host's key
  - block 12: Batch mode: no question ever reaches the AI agent
- C. Remote commands
  - block 13: Run one remote command
  - block 14: Run commands from a file
  - block 15: Feed data on standard input
  - block 16: Use a saved session
  - block 17: Log in with a password file (only when the human provides the file)
  - block 18: kitty.exe has no capture mode
- D. File transfer
  - block 19: Upload a file or folder
  - block 20: Download a file or folder
  - block 21: Check a remote path
  - block 22: List a remote folder
  - block 23: Several SFTP steps in one login
- E. kageant from other tools
  - block 24: Find the kageant pipe
  - block 25: Windows OpenSSH (ssh.exe) with kageant
  - block 26: git with kageant through ssh.exe
  - block 27: git with klink
  - block 28: WSL 1
  - block 29: WSL 2
  - block 30: List the keys in kageant (ssh-add)
  - block 31: Restricted keys
- F. Lock and unlock
  - block 32: Lock kageant (only with allowipclock=yes)
  - block 33: Unlock kageant
- G. Add and remove keys (only where the policy allows it)
  - block 34: Read the IPC policy first
  - block 35: Add a key
  - block 36: Remove one key this AI agent added
- H. What an AI agent must not do
  - block 37: Never switch off host key checking
  - block 38: Never empty or change kageant
  - block 39: Never read stored passwords or secrets
  - block 40: Never put a password on the command line
  - block 41: Never forward kageant
  - block 42: Never answer a confirmation for the human
  - block 43: Never change settings or the machine
  - block 44: Never use unsafe transfer modes
- I. Exit codes and output
  - block 45: klink exit codes
  - block 46: klink -scan and -knownhosts output
  - block 47: kscp exit codes
  - block 48: ksftp exit codes

## How to work with this guide

The human in control gives the job: the hosts, the accounts, the host-key
layer of each host and what is allowed. Work in this loop:

1. Read the job. Before the first command, reply with the hosts, the
   host-key layer of each and the blocks you plan to use. Wait for the
   human's go.
2. Pick the block for the next step. Fill in every placeholder from the job
   or from an earlier result. A value you would have to guess: stop and
   request it from the human.
3. Run the command of the block as written. One step at a time.
4. Compare the exit code and the output with "expect" and "failure".
5. At a stop point: stop, report and wait for the human. The stop points:
   - a `new` host key outside the job's accept-new lines, a `MISMATCH`, a
     changed host key
   - each "Stop." in the failure line of a block
   - a refusal by kageant
   - an exit code the block does not expect
   - a step the job does not cover, or one a "never" line forbids
6. At the end: report each command with its exit code, every
   `Host key stored` and `Host key not stored` line and every stop.

You never get a private key, a passphrase or a password. kageant signs the
logins. Never work around a refusal: no other tool,
no other switch, no run without `-batch`. A refusal is a result for the
human.

## How a host key becomes trusted

Every connection needs a host key the AI agent can trust. Three layers, in
this order:

1. **Host certificates (SSH CA).** Recommended for many hosts. The human
   trusts a certification authority (CA) once. klink, kscp and ksftp then
   accept every host whose host key that CA signed, jump hosts included. No
   fingerprint, no question. Block 8.
2. **accept-new, only when the human allows it in the job.** The job lists
   the hosts or a host group, e.g. "accept-new for `*.lab.example.com`". An
   unknown key is stored and recorded. The AI agent lists every new key in its
   report. A changed key is always a hard stop. No switch limits
   `-accept-new` to a host group: the AI agent keeps to the list itself.
   Block 9.
3. **Neither.** The AI agent shows the scan rows to the human and requests a
   decision. Then it pins the fingerprint the human confirmed. This is the
   fallback, not the rule. Block 10.

A jump host has switches of its own: `-proxyhostkey` and
`-proxy-accept-new`. `-hostkey` and `-accept-new` apply to the target only.
Block 11.
The job holds one line per host or group: `CA for <host>` (layer 1),
`accept-new for <hostgroup>` (layer 2) or `pin <host> <fp>` (layer 3, a
fingerprint the human confirmed). A host without such a line: layer 3, show
the scan rows to the human.

## A. Get KiTTY++ and check it

### 1. Download a release

**goal:** get the official KiTTY++ package.

**inputs:**

- `<version>`: the KiTTY++ release version, e.g. `0.85.1.20-beta`. Read it
  from the asset names on the release page.
- `-peruser.msi`: installs for the current Windows account. `-system.msi`: a
  system-wide install. `.zip`: a portable copy. `-32bit.zip`: the 32-bit
  portable copy.

**command:**

```text
https://github.com/hknet/KiTTY/releases/latest
assets: KiTTY-<version>-x64-peruser.msi
        KiTTY-<version>-x64-system.msi
        kitty-<version>.zip
        kitty-<version>-32bit.zip
```

**expect:** the file in a local folder.

**failure:** no asset of that name: stop. Report the content of the release
page to the human.

**never:** take KiTTY++ from a mirror, from a package found by a search or
from a file a server offers.

### 2. Check the Authenticode signature

**goal:** prove the file comes from the KiTTY++ publisher.

**inputs:**

- `<file>`: the path of a downloaded MSI file or of an executable unpacked
  from a ZIP, e.g.
  `C:\Users\deploy\Downloads\KiTTY-0.85.1.20-beta-x64-peruser.msi`.
- Authenticode: the Windows code signature of an executable or MSI file.
- CN: the common name in the subject of the signer's certificate.

**command:**

```powershell
Get-AuthenticodeSignature -FilePath <file> | Format-List Status,SignerCertificate
```

**expect:** `Status` is `Valid`. The `SignerCertificate` subject contains
`CN=KAPPER NETWORK-COMMUNICATIONS GmbH`. Every executable and both MSI files
are signed.

**failure:** a status other than `Valid` or another CN: do not run the file.
Report the file name and the status.

**never:** run a file with an unknown or broken signature to test it.

### 3. Check the ZIP checksums

**goal:** prove the content of a ZIP package is unchanged.

**inputs:**

- `<zipdir>`: the folder where the ZIP was unpacked, e.g.
  `C:\work\kitty-0.85.1.20-beta`.
- `<file>`: one file in `<zipdir>`, e.g.
  `C:\work\kitty-0.85.1.20-beta\klink.exe`. Repeat the check for every file
  that SHA256SUMS lists.
- SHA256SUMS: the checksum file inside the ZIP. Each line: 64 hexadecimal
  digits in lower case, two spaces, the file name.

**command:**

```powershell
# run these lines in order
Get-Content <zipdir>\SHA256SUMS
Get-FileHash -Algorithm SHA256 <file>
# compare the Hash value with the line for <file> in SHA256SUMS
```

**expect:** the hashes match for every file in SHA256SUMS. Get-FileHash
prints the hash in upper case: compare without regard to case.

**failure:** one mismatch: discard the package in full. Report the file name.

**never:** edit SHA256SUMS. Skip a file.

### 4. Check the installed version

**goal:** confirm the installed tools match this page.

**inputs:**

- `<install-dir>`: the KiTTY++ install folder.
  `%LOCALAPPDATA%\Programs\KiTTY` for the per-user MSI.
  `C:\Program Files\KiTTY` for the system MSI. For a portable copy: the
  folder the human unpacked it to, e.g. `C:\work\kitty-0.85.1.20-beta`.

**command:**

```powershell
& "<install-dir>\klink.exe" -V
```

**expect:** exit code 0. First line:
`klink: 0.85.1.20-beta (KiTTY++, PuTTY 0.85 base)`.

**failure:** another version: use the guide of that version, or report the
version to the human.

**never:** run the tools of another version against this page.

### 5. Find the settings store and the logs of this install

**goal:** know where KiTTY++ keeps its settings and where its records go,
without a change.

**inputs:**

- `<install-dir>`: the KiTTY++ install folder.
  `%LOCALAPPDATA%\Programs\KiTTY` for the per-user MSI.
  `C:\Program Files\KiTTY` for the system MSI. For a portable copy: the
  folder the human unpacked it to, e.g. `C:\work\kitty-0.85.1.20-beta`.
- settings store: the registry key `HKCU\Software\kapper.net\KiTTY`, or a
  kitty.ini file that is authoritative.
- kitty.ini: KiTTY++ reads the first of: the file in the environment
  variable `KITTY_INI_FILE`, `kitty.ini` next to the program,
  `%APPDATA%\KiTTY\kitty.ini`.
- system MSI: the system-wide installer. Only it registers the logs
  `KiTTY++_Agent` and `KiTTY++_Session` in the Windows Event Log.

**command:**

```powershell
# run these lines in order
$env:KITTY_INI_FILE
Test-Path "<install-dir>\kitty.ini"
Select-String -Path "<install-dir>\kitty.ini" -Pattern '^\s*savemode\s*='
Test-Path "<install-dir>\Sessions"
Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\KiTTY++_Agent'
Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\KiTTY++_Session'
```

**expect:** the kitty.ini is authoritative with `savemode=dir` or
`savemode=file`. A `Sessions` folder next to a kitty.ini without a savemode
line has the same effect. Else the registry is authoritative and kitty.ini
holds first-run defaults only. `True` for the two Event Log keys: the system
MSI is installed. The records then go to the Windows Event Log (KiTTY++_Agent,
source KiTTY++_agent) and the Windows Event Log (KiTTY++_Session, source
KiTTY++_cli). `False`: the same records go to the Windows Event Log
(Application, source KiTTY++_agent) and the Windows Event Log (Application,
source KiTTY++_cli).

**failure:** `Test-Path` of the kitty.ini gives `False`: no portable layout.
The `Select-String` line then reports that the path was not found. That
error is expected.
The registry is authoritative unless `KITTY_INI_FILE` points to a kitty.ini
with `savemode=dir` or `savemode=file`.

**never:** write to the kitty.ini, the registry key or the Event Log keys.

## B. Host keys and batch mode

### 6. List the host keys the user already trusts

**goal:** read the stored host key of `<host>` without a connection.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- host key: the server's public key. It proves the identity of the server.
- stored host key: a host key the user accepted before. KiTTY++ keeps these
  keys in its host key store.

**command:**

```powershell
klink -knownhosts <host>:<port> -json
```

**expect:** exit code 0. One JSON array. Fields: `host`, `port`, `type`,
`bits`, `sha256` (`SHA256:...`), `md5`, `status` (`stored`), `first_seen`,
`last_written`.

**failure:** exit code 1 and `[]`: no key is stored for `<host>`. Use a
host CA (block 8), accept-new where the job allows it (block 9) or a
fingerprint the human confirmed (block 10).

**never:** edit, delete or add host key files or registry values.

### 7. Scan the server's keys and compare them with the store

**goal:** see which keys `<host>` presents and whether they match the store.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `-t ed25519,ecdsa`: after `-scan`, `-t` is the list of key types to fetch,
  separated by commas. Types: `rsa`, `dsa`, `ecdsa`, `ed25519`, `ed448`.
  Without `-t` the scan fetches all types. Here `-t` is not the pty switch.
- store: the host key store, the server keys the user already trusts.
- `status` values: `stored` (the key matches the store), `new` (the store has
  no key of this type for `<host>`), `MISMATCH` (the key differs from the
  stored key), `not offered` (the server does not offer this host key
  algorithm), `unreachable` (no key came for another reason: the `error`
  field holds it), `not signing` (one more stored key of the same type. The
  server does not sign with it now).
- `sha256`: the fingerprint of the key the server presented. In a
  `not signing` row: the stored key. `stored_sha256`: the stored key of a
  `MISMATCH` row. In every other row it is `-`.

**command:**

```powershell
# one of these forms: pick the one that fits
klink -scan <host>:<port> -json
klink -scan -t ed25519,ecdsa <host>:<port> -json
```

**expect:** exit code 0: at least one key came and none is a `MISMATCH`.
One JSON array with one object per key type. `status` is one of the values
above. No login takes place. The store is never written.

**failure:** exit code 2: at least one `MISMATCH`. Stop. Report the host,
`sha256` and `stored_sha256` of that row. Exit code 1: no key at all (host
down, wrong host name, wrong `-t` value).

**never:** treat a `new` key as trusted. A scan proves nothing about the
identity of the server.

### 8. Layer 1: connect to a host whose key a trusted CA signed

**goal:** connect without a fingerprint: a CA the human trusts vouches for
the host key.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job. Use
  the name the server's certificate lists.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- host certificate: the server's host key signed by a CA, for listed host
  names.
- host CA record: the human's entry for one trusted CA. Fields: the CA's
  name, the CA's public key and "Valid hosts", an expression such as
  `*.example.com`. The records hold for every session of this Windows
  account (registry `HKCU\Software\kapper.net\KiTTY\SshHostCAs`, or the
  `SshHostCAs` folder of a portable copy).
- In the output: `<ca-name>` is the name of the CA record. `<reason>` is the
  reason klink gives. klink fills both in.
- `-v`: verbose. klink shows the steps of the connection on standard error.

**command:**

```powershell
# one of these forms: the second shows the certificate check on standard error
klink -batch -l <user> -P <port> <host> "<command>"
klink -batch -v -l <user> -P <port> <host> "<command>"
```

**expect:** no question. The exit code is the exit code of `<command>`.
Without `-v` standard error shows nothing about the host key. With `-v`:
`Certification authority matches '<ca-name>'` and `Accepted certificate`.
With `clieventlog=yes`: event 310 `hostkey-known`, `basis: ca`, in the
Windows Event Log (KiTTY++_Session, source KiTTY++_cli). The store is not
written. A `-hostkey` on the same command line is not consulted for an
accepted certificate.

**failure:** klink offers certificates only when a CA record's "Valid hosts"
matches `<host>` as typed. No record matches: the plain host key check
follows. An unknown key then ends the run with
`Cannot confirm a host key in batch mode`, exit code 1. A certificate signed
by another CA, expired or not issued for `<host>`: with `-v`
`Rejected host key certificate: <reason>`, then the same refusal. Stop.
Report the host and the lines to the human. An IP address fails when the
certificate lists host names only.

**never:** add, change or delete a host CA record. Fall back to
`-accept-new` because a certificate was refused.

### 9. Layer 2: accept-new (only when the job allows it for this host)

**goal:** store an unknown host key without a question. Report every new key
to the human.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- accept-new permission: a line in the human's job that lists the hosts or a
  host group, e.g. `accept-new for *.lab.example.com`. Without such a line,
  or for a host outside it, this block does not apply. Use block 10.
- In the output lines: `<type>` is the key type (e.g. `ssh-ed25519`), `<bits>`
  the key size (e.g. `255`), `SHA256:<hash>` the fingerprint, `<types>` the
  stored key types, separated by commas. klink fills these in. The AI agent
  supplies none of them.
- `-accept-new`: store an unknown host key without a prompt. A changed key is
  never stored. A host certificate is never stored this way.
- `readonly=yes`: a KiTTY++ setting. The host key store is read-only.
- `-batch`: batch mode. Every question that would wait for an answer fails at
  once.

**command:**

```powershell
# only for a host the job's accept-new line covers
klink -batch -accept-new -l <user> -P <port> <host> "<command>"
```

**expect:** standard error shows
`Host key stored (accept-new): <type> <bits> SHA256:<hash>, <host> port <port>`.
With `clieventlog=yes`: event 311 `hostkey-new`, `result: accept-new`, in the
Windows Event Log (KiTTY++_Session, source KiTTY++_cli). The exit code is the
exit code of `<command>`. With `readonly=yes` the key holds for this
connection only. Standard error then shows
`Host key not stored (readonly=yes): <type> <bits> SHA256:<hash>, <host> port <port>`.
A known key gives no line. The AI agent copies every `Host key stored` and
`Host key not stored` line into its report to the human.

**failure:** a changed key is a hard stop. It is never stored. Standard error
shows `WARNING - POTENTIAL SECURITY BREACH!`, the new fingerprint and
`Cannot confirm a host key in batch mode`. Exit code 1. With
`clieventlog=yes`: event 312 `hostkey-mismatch`, `result: refused`. A stored
key of another type gives `Host key not stored (accept-new): a key of another
type (<types>) is stored for <host> port <port>`, then
`The host key is not cached for this server:` and
`Cannot confirm a host key in batch mode`, exit code 1. Stop. Report the
lines to the human.

**never:** `-accept-new` for a host the job does not list. `-accept-new`
against a host whose scan shows `MISMATCH`. Work around a changed key: no
run without `-batch`, no `-hostkey` with the new fingerprint, no
`autostoresshkey=yes`.

### 10. Layer 3: pin a stored key or a fingerprint the human confirmed

**goal:** connect only when the server's key is a stored key or one the
human confirmed.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- `<fp-ed25519>`, `<fp-ecdsa>`: the SHA256 fingerprint of the ed25519 key and
  of the ecdsa key of `<host>`. Form: `SHA256:` plus 43 base64 characters,
  e.g. `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: the first
  command line below. Take the `sha256` field of each row whose `status` is
  `stored`. For a `new` row: show the scan rows to the human. Take only a
  fingerprint the human confirmed. Give one `-hostkey` per key type. A server
  with other key types needs other `-hostkey` values in the same way.
- scan rows: the JSON objects of `klink -scan`. Show `host`, `port`, `type`,
  `sha256` and `status` of each.
- pin: give the expected host keys on the command line. They replace the
  host key store for this run. The store is not written.

**command:**

```powershell
# run these lines in order
# 1. new rows: show the scan rows to the human. Stop until the human decides.
klink -scan <host>:<port> -json
# 2. with stored or confirmed fingerprints only:
klink -batch -hostkey <fp-ed25519> -hostkey <fp-ecdsa> -l <user> -P <port> <host> "<command>"
```

**expect:** the human compares each `sha256` with the server's own record
and confirms or refuses it. The pinned keys replace the store for this run.
The exit code is the exit code of `<command>`. A fingerprint can also be the
MD5 hex form or the base64 key blob. With `clieventlog=yes`: event 310
`hostkey-known`, `basis: manual`.

**failure:** `Host key not in manually configured list`, exit code 1: the
server's key is not among the pinned keys. A changed key fails this way in a
pinned run. A missing key type fails the same way. With `clieventlog=yes`:
event 312 `hostkey-mismatch`, `result: refused`. Stop. Report the host and
the fingerprints. The human refused a fingerprint: stop. Report it.

**never:** pin a `new` fingerprint without the human's confirmation. Copy a
fingerprint from the server's own output.

### 11. Jump hosts: pin or accept the jump host's key

**goal:** connect through a jump host with a host key check on both hosts.

**inputs:**

- `<host>`: the target server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`, stored or
  confirmed by the human (block 10). Form: `SHA256:` plus 43 base64
  characters, e.g. `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`.
- `<jump>`: the jump host, a DNS name or an IP address, e.g.
  `jump.example.com` or `192.0.2.20`. The human gives it with the job.
- `<jumpport>`: the SSH port of `<jump>`, e.g. `22`.
- `<jumpuser>`: the login account on `<jump>`, e.g. `deploy`. The login
  uses the keys in kageant.
- `<jumpfp>`: the SHA256 fingerprint of a host key of `<jump>`, stored or
  confirmed by the human. Same form as `<fp>`. Source: the first command
  line below, as in block 10.
- jump host: an SSH server klink logs in to first. The connection to `<host>`
  runs through it.
- `-J <jumpuser>@<jump>:<jumpport>`: the jump host. One host only. A chain
  (`a,b`) is refused.
- `-proxyhostkey`: a host key of `<jump>`, as `-hostkey` is for `<host>`.
  May be repeated.
- `-proxy-accept-new`: store an unknown key of `<jump>`, as `-accept-new`
  does for `<host>` (block 9). Only for a jump host the job's accept-new
  line covers.

**command:**

```powershell
# run these lines in order
# 1. new rows: show the scan rows to the human. Stop until the human decides.
klink -scan <jump>:<jumpport> -json
klink -scan <host>:<port> -J <jumpuser>@<jump>:<jumpport> -proxyhostkey <jumpfp> -json
# 2. with stored or confirmed fingerprints only:
klink -batch -J <jumpuser>@<jump>:<jumpport> -proxyhostkey <jumpfp> -hostkey <fp> -l <user> -P <port> <host> "<command>"
```

**expect:** the scan through `-J` shows the keys of `<host>` only.
`-proxyhostkey` and `-proxy-accept-new` apply to `<jump>` only. `-hostkey`
and `-accept-new` apply to `<host>` only. The exit code is the exit code of
`<command>`. With `clieventlog=yes` the host key records of `<jump>` carry
`proxy: ssh`: event 310 `hostkey-known`, `basis: manual`. With
`-proxy-accept-new` in place of `-proxyhostkey` an unknown key of `<jump>`
is stored. Standard error then shows
`Host key stored (accept-new): <type> <bits> SHA256:<hash>, <jump> port <jumpport>`.
With `clieventlog=yes`: event 311 `hostkey-new`, `result: accept-new`,
`proxy: ssh`. With neither switch the key of `<jump>` must be in the host
key store or signed by a trusted CA (block 8).

**failure:** a key of `<jump>` that is not among the `-proxyhostkey` keys:
standard error shows
`jump host <jump> port <jumpport>: Host key not in manually configured list`,
exit code 1. An unknown key of `<jump>` with neither switch, or a changed
key of `<jump>` (with `-proxy-accept-new` too): standard error shows the
host key text of `<jump>`, `Cannot confirm a host key in batch mode` and
`jump host <jump> port <jumpport>:`, exit code 1. Stop. Report the jump
host and the lines to the human. A jump host given as the name of a saved
session also uses that session's own host key settings.

**never:** run without `-batch` to answer the question of `<jump>`.
`-proxy-accept-new` for a jump host the job does not list. Work around a
changed key of `<jump>`: no `-proxyhostkey` with the new fingerprint, no
`autostoresshkey=yes`. Edit a saved session.

**not available in 0.85.1.20-beta:** a chain of jump hosts on the command
line (`-J a,b`). Host key switches for a second jump host.

### 12. Batch mode: no question ever reaches the AI agent

**goal:** every prompt fails at once instead of waiting.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<local>`: a file on this computer, e.g. `C:\work\report.csv`.
- `<remote>`: the target path on `<host>`, absolute (`/srv/data/`) or
  relative to the home folder of `<user>` on `<host>` (`data/`).
- `<batchfile>`: a local text file with one SFTP command per line (`cd`,
  `put`, `get`, `mkdir`, `rm`), e.g. `C:\work\upload.sftp`. The AI agent
  writes it for the job.
- `-batch`: batch mode. Every question that would wait for an answer fails at
  once.

**command:**

```powershell
# one of these forms: one line per tool
klink -batch -hostkey <fp> -l <user> -P <port> <host> "<command>"
kscp -batch -q -hostkey <fp> -l <user> -P <port> <local> <host>:<remote>
ksftp -batch -hostkey <fp> -l <user> -P <port> -b <batchfile> <host>
```

**expect:** no prompt. A refused question ends the run with exit code 1.

**failure:** standard error shows `Cannot confirm a host key in batch mode`,
`Cannot confirm a weak crypto primitive in batch mode` or
`Cannot answer interactive prompts in batch mode`. Without `-hostkey`, a changed key shows `WARNING - POTENTIAL SECURITY
BREACH!` and the new fingerprint first. With `-hostkey`, a changed key shows
`Host key not in manually configured list`. Stop. Report the line. Do not
retry without `-batch`.

**never:** pipe `y` into a tool to answer a question.

## C. Remote commands

### 13. Run one remote command

**goal:** run `<command>` on `<host>`. Capture standard output, standard
error and the exit code.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<errfile>`: a local file for standard error, e.g. `C:\work\klink.err`.
- `$out`, `$code`: PowerShell variables. `$out` holds standard output, one
  string per line. `$code` holds the exit code.
- In the output: `<option>`, `<file>` and `<text>` are the option, the file
  and the error text klink reports. klink fills them in.
- pty: a pseudo-terminal. A remote program with a pty behaves as on an
  interactive terminal.

**command:**

```powershell
# run these lines in order
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
$out = klink -batch -hostkey <fp> -l <user> -P <port> <host> "<command>" 2> <errfile>
$code = $LASTEXITCODE
Get-Content <errfile>
```

**expect:** `$out` holds the standard output of `<command>`. `<errfile>`
holds the standard error of `<command>` plus klink's messages. `$code` is the
remote exit code. No pty is allocated. Windows PowerShell 5.1 writes the
first line of standard error to `<errfile>` as an error record that starts
with `klink.exe : <text>`, followed by PowerShell's own lines. The further
lines of standard error follow as plain text. PowerShell 7 writes the text
alone.

**failure:** exit code 1 with `Remote process exit code unavailable`: the
connection was lost. klink's own errors exit with 1 too. Its own messages on
standard error: `klink: unknown option "<option>"`,
`klink: no valid host name provided`, `Unable to open connection:`,
`FATAL ERROR: <text>`, `klink: unable to open command file "<file>"`. An exit
code 1 is ambiguous: read standard error. Exit code 2 comes from
`<command>` itself. klink's own exit code 2 exists only for `-scan` (block
7).

**never:** add `-t` unless the remote program needs a terminal.

### 14. Run commands from a file

**goal:** send a longer script without shell quoting.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<cmdfile>`: a local text file with the shell commands to run on `<host>`,
  e.g. `C:\work\job.sh`. The AI agent writes it for the job, or the human
  gives it. A file the human gives: read it in full before the run. Run it
  only when every line belongs to the job.
- remote shell: the login shell of `<user>` on `<host>`. It runs the content
  of `<cmdfile>`.
- pty: a pseudo-terminal. A remote program with a pty behaves as on an
  interactive terminal.

**command:**

```powershell
# run these lines in order
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
klink -batch -hostkey <fp> -l <user> -P <port> -m <cmdfile> <host>
$LASTEXITCODE
```

**expect:** klink sends the content of `<cmdfile>` as one remote command.
Standard output and standard error of the commands. The exit code is the exit
code of the remote shell: with a POSIX shell, that of its last command. No
pty is allocated. Whether a failing line stops the rest is the remote
shell's behaviour, not klink's: a POSIX shell goes on unless the file starts
with `set -e`. Write `set -e` as the first line when a failure must stop the
run.

**failure:** a missing `<cmdfile>` ends the run before the connection with
`klink: unable to open command file "<cmdfile>"`, exit code 1. Exit code 1
with `Remote process exit code unavailable`: the connection was lost.
klink's own errors exit with 1 too. An exit code 1 is ambiguous: read
standard error.

**never:** put passwords, keys or tokens in `<cmdfile>`. Run a `<cmdfile>`
the human gave without reading it first.

### 15. Feed data on standard input

**goal:** pipe local data into a remote command.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line on `<host>` that reads standard input,
  between the double quotes, e.g. `wc -l`. The job defines it.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<local>`: the local file whose content goes to `<command>`, e.g.
  `C:\work\data.csv`.
- pty: a pseudo-terminal. A remote program with a pty behaves as on an
  interactive terminal.

**command:**

```powershell
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
Get-Content <local> | klink -batch -hostkey <fp> -l <user> -P <port> <host> "<command>"
```

**expect:** no pty when standard input is not a console. The remote command
gets the end of input. The exit code is the remote exit code.

**failure:** the run hangs: a pty was forced (`-t`, or the session's remote
command setting). Drop `-t`. Exit code 1 is ambiguous: read standard error.

**never:** pipe secrets into a remote command the human did not request.

### 16. Use a saved session

**goal:** reuse the host, port, user and key settings of a saved session.

**inputs:**

- `<session>`: the name of a saved KiTTY++ session, between the double
  quotes, e.g. `prod-web`. The human gives it.
- `<command>`: the command line to run on the session's host, between the
  double quotes, e.g. `uname -a`. The job defines it.
- master password: the password that protects the passwords stored in
  KiTTY++ sessions.
- `-masterpwfile`: the klink switch that reads the master password from a
  file.

**command:**

```powershell
# run these lines in order
klink -batch -load "<session>" "<command>"
$LASTEXITCODE
```

**expect:** standard output and standard error of `<command>`. The exit code
is the remote exit code. A stored session password is sent by klink itself.

**failure:** the store is protected by a master password: klink prompts for
it and with `-batch` the run fails. Report it. Only the human supplies
`-masterpwfile`.

**never:** read the session's password from the registry or a session file.
Request the master password from the human in a chat.

### 17. Log in with a password file (only when the human provides the file)

**goal:** password login without a prompt.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<pwfile>`: the path of a password file the human created, e.g.
  `C:\work\deploy.pw`. It holds the password on one line. The human gives the
  path.
- `<password>`: a password in clear text. It appears only in the never line:
  the AI agent never types it.

**command:**

```powershell
# run these lines in order
# <pwfile>: created by the human. <fp>: SHA256 fingerprint of a stored key from klink -scan.
klink -batch -hostkey <fp> -l <user> -P <port> -pwfile <pwfile> <host> "<command>"
$LASTEXITCODE
```

**expect:** standard output and standard error of `<command>`. The exit code
is the remote exit code.

**failure:** wrong password: `Cannot answer interactive prompts in batch
mode`, exit code 1. Stop. Report it.

**never:** `-pw <password>`. The password would pass through your own context: the prompt, the logs, the model provider. klink wipes it from its command line after reading it, but a process-creation audit (event 4688, Sysmon) records it first. Write `<pwfile>` from a password found anywhere.

### 18. kitty.exe has no capture mode

**goal:** know what not to do: kitty.exe gives no output to capture. Use
klink for remote commands.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- kitty.exe: the KiTTY++ terminal window program.

**command:**

```powershell
# the safe form: klink, not kitty.exe
klink -batch -hostkey <fp> -l <user> -P <port> <host> "<command>"
```

**expect:** you are about to break this rule when your command line starts
with kitty.exe and contains `-cmd`, `-rcmd` or `-sendcmd`. These switches
type text into a terminal window. They give no standard output and no exit
code.

**failure:** you drove a kitty.exe window: stop. Report it. Run the job again
with the klink command above.

**never:** drive kitty.exe windows with injected keys. Run remote commands
with `klink -batch -hostkey <fp> -l <user> -P <port> <host> "<command>"`.

## D. File transfer

### 19. Upload a file or folder

**goal:** copy `<local>` or `<localdir>` to `<host>`.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<local>`: the file to upload, a path on this computer, e.g.
  `C:\work\report.csv`. The job defines it.
- `<localdir>`: the folder to upload, a path on this computer, e.g.
  `C:\work\site`. The job defines it.
- `<remote>`: the target path on `<host>`, absolute (`/srv/data/`) or
  relative to the home folder of `<user>` on `<host>` (`data/`). The job
  defines it.
- `-q`: no progress output. `-r`: copy a folder with its content.

**command:**

```powershell
# one of these forms: a file (first line) or a folder (second line)
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
kscp -batch -q -hostkey <fp> -l <user> -P <port> <local> <host>:<remote>
kscp -batch -q -r -hostkey <fp> -l <user> -P <port> <localdir> <host>:<remote>
```

**expect:** exit code 0. No output with `-q`. `<remote>` is an existing
folder: the file or folder goes into it. A file of the same name on `<host>`
is overwritten without a question. Check the target first (block 21) when it
must not be overwritten.

**failure:** exit code 1. The reason is on standard error. A missing
`<local>` or `<localdir>`: `<local>: No such file or directory`, before any
transfer. A `<remote>` with a trailing slash for a folder that does not
exist: not documented, test it before relying on it.

**never:** `-unsafe` (it allows server-side wildcards).

### 20. Download a file or folder

**goal:** copy `<remote>` from `<host>`.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<remote>`: the source path on `<host>`, a file or a folder, absolute
  (`/srv/data/report.csv`) or relative to the home folder of `<user>` on
  `<host>` (`data/report.csv`). The job defines it.
- `<local>`: the target file or folder on this computer, e.g.
  `C:\work\report.csv`. The job defines it.
- `<localdir>`: the target folder on this computer for a folder download,
  e.g. `C:\work\download`. The job defines it.
- `-q`: no progress output. `-r`: copy a folder with its content.

**command:**

```powershell
# one of these forms: a file (first line) or a folder (second line)
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
kscp -batch -q -hostkey <fp> -l <user> -P <port> <host>:<remote> <local>
kscp -batch -q -r -hostkey <fp> -l <user> -P <port> <host>:<remote> <localdir>
```

**expect:** exit code 0. An existing local file of the same name is
overwritten without a question.

**failure:** exit code 1. The reason is on standard error.

**never:** `-unsafe` (it allows server-side wildcards in SCP mode).
Download over a local file the job did not name.

### 21. Check a remote path

**goal:** learn whether `<remote>` is a file or a folder before a transfer.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<remote>`: the path to check on `<host>`, absolute (`/srv/data`) or
  relative to the home folder of `<user>` on `<host>` (`data`). The job
  defines it.
- In the output: `<bytes>` is the file size in bytes (e.g. `10240`). `<text>`
  is the error text of kscp.

**command:**

```powershell
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
kscp -batch -stat -hostkey <fp> -P <port> <user>@<host>:<remote>
```

**expect:** one line on standard output: `file <bytes>` or `dir`. Exit code 0.

**failure:** `missing`, `other` or `error <text>` (also `error no SFTP on the
server`), exit code 1.

**never:** transfer to a path whose check failed.

### 22. List a remote folder

**goal:** list `<remotedir>` on `<host>`.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<remotedir>`: the folder on `<host>`, absolute (`/srv/data`) or relative
  to the home folder of `<user>` on `<host>` (`data`). The job defines it.

**command:**

```powershell
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
kscp -batch -ls -hostkey <fp> -P <port> <user>@<host>:<remotedir>
```

**expect:** the listing on standard output. Exit code 0.

**failure:** exit code 1. The reason is on standard error.

**never:** list folders outside the job the human gave.

### 23. Several SFTP steps in one login

**goal:** run a list of SFTP commands (`cd`, `put`, `get`, `mkdir`, `rm`).

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<batchfile>`: a local text file with one SFTP command per line, e.g.
  `C:\work\upload.sftp` with the lines `cd /srv/data` and
  `put C:\work\report.csv`. The AI agent writes it for the job.

**command:**

```powershell
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
ksftp -batch -hostkey <fp> -l <user> -P <port> -b <batchfile> -bc <host>
```

**expect:** exit code 0. `-bc` echoes each command on standard output.

**failure:** exit code 2: a command in `<batchfile>` failed. ksftp stopped
there. Exit code 1: the connection or the SFTP start failed.

**never:** `-be` (it continues after a failed command). An `rm` in
`<batchfile>` the human did not request.

## E. kageant from other tools

### 24. Find the kageant pipe

**goal:** get the pipe path that ssh.exe and ssh-add need.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- kageant pipe: the Windows named pipe through which ssh.exe and ssh-add
  reach kageant.
- kageant tray menu: the menu of the kageant icon in the Windows
  notification area.
- In the output: `<winuser>` is the Windows user name of the account that
  runs kageant (e.g. `deploy`). `<hash>` is a string of hexadecimal digits.
  kageant writes both. The AI agent supplies neither.

**command:**

```powershell
Get-Content "$env:USERPROFILE\.ssh\kageant.conf"
```

**expect:** one line: `IdentityAgent "//./pipe/pageant.<winuser>.<hash>"`
with forward slashes.

**failure:** no file or an empty file: the human has not switched on
"Register as Windows OpenSSH agent" in the kageant tray menu, or kageant is
not running. Stop. Report it.

**never:** compute the pipe name. Start kageant. Edit `kageant.conf` or
`~/.ssh/config`.

### 25. Windows OpenSSH (ssh.exe) with kageant

**goal:** log in with the keys loaded in kageant.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- managed Include block: the lines kageant writes into
  `%USERPROFILE%\.ssh\config` when the human switches on "Register as
  Windows OpenSSH agent". They point ssh.exe at the kageant pipe.
- `-o BatchMode=yes`: ssh.exe fails a question at once instead of waiting.
- known_hosts: OpenSSH's own host key file,
  `%USERPROFILE%\.ssh\known_hosts`. ssh.exe checks host keys there, not in
  the KiTTY++ host key store. A host CA for ssh.exe is an `@cert-authority`
  line in that file.
- agent log: kageant's record of key uses, refusals, adds, removes and
  locks.
- locked: kageant refuses every key use until an unlock. A locked kageant
  lists no keys.

**command:**

```powershell
# run these lines in order
# the first line prints the OpenSSH version (on standard error)
C:\Windows\System32\OpenSSH\ssh.exe -V
C:\Windows\System32\OpenSSH\ssh.exe -o BatchMode=yes -p <port> <user>@<host> "<command>"
$LASTEXITCODE
```

**expect:** the version line holds the OpenSSH version, e.g.
`OpenSSH_for_Windows_9.5p1`. The managed Include block in `~/.ssh/config` makes ssh.exe use
kageant. The exit code is the exit code of `<command>`. OpenSSH 8.9 or newer
binds the agent connection: the agent log records the destination.

**failure:** `Permission denied (publickey)`: kageant is locked (empty key
list), the key is not loaded or a confirmation was refused. Report it. Do not
retry in a loop. A host key that is unknown or changed in known_hosts: with
`BatchMode=yes` ssh.exe fails without a prompt (OpenSSH behaviour). ssh.exe
exits with 255 on its own errors (OpenSSH behaviour). Stop. Report the host
and the lines to the human. Only the human adds a key to known_hosts. A
version below 8.9: the login works, but the agent log has no destination.

**never:** ssh.exe from Git for Windows or MSYS (no named pipes).
`-o StrictHostKeyChecking=no` or `-o UserKnownHostsFile=NUL`. Edit
known_hosts.

### 26. git with kageant through ssh.exe

**goal:** `git fetch` and `git push` with the keys loaded in kageant.

**inputs:**

- `<git-command>`: the git subcommand with its arguments, e.g.
  `fetch origin` or `push origin main`. The job defines it.
- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- managed Include block: the lines kageant writes into
  `%USERPROFILE%\.ssh\config` when the human switches on "Register as
  Windows OpenSSH agent". They point ssh.exe at the kageant pipe.
- locked: kageant refuses every key use until an unlock. A locked kageant
  lists no keys.

**command:**

```powershell
# run these lines in order
# forward slashes: git hands GIT_SSH_COMMAND to its shell, which drops unquoted backslashes
$env:GIT_SSH_COMMAND = "C:/Windows/System32/OpenSSH/ssh.exe -o BatchMode=yes"
git <git-command>
```

**expect:** the managed Include block in `~/.ssh/config` makes ssh.exe use
kageant. git exits with 0 on success. `GIT_SSH_COMMAND` wins over a
`core.sshCommand` in git's settings.

**failure:** `Permission denied (publickey)`: kageant is locked (empty key
list), the key is not loaded or a confirmation was refused. Report it. Do not
retry in a loop. ssh.exe checks the git server's host key in
`%USERPROFILE%\.ssh\known_hosts` (block 25). An unknown or changed key:
`Host key verification failed.`, git exit code 128, nothing written to
known_hosts. Stop. Report it to the human. `fatal: ssh variant 'simple'
does not support setting port`: the path in `GIT_SSH_COMMAND` lost its
backslashes. Use forward slashes.

**never:** `git config --global` changes the human did not request.
`GIT_SSH_COMMAND` with `StrictHostKeyChecking=no`. ssh.exe without
`BatchMode=yes`: an unknown host key waits on a question.

### 27. git with klink

**goal:** git over klink. klink uses kageant on its own.

**inputs:**

- `<install-dir>`: the KiTTY++ install folder.
  `%LOCALAPPDATA%\Programs\KiTTY` for the per-user MSI.
  `C:\Program Files\KiTTY` for the system MSI. For a portable copy: the
  folder the human unpacked it to, e.g. `C:\work\kitty-0.85.1.20-beta`.
- `<git-command>`: the git subcommand with its arguments, e.g.
  `fetch origin` or `push origin main`. The job defines it.
- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.

**command:**

```powershell
# run these lines in order
# 1. must print nothing: a core.sshCommand in git's settings wins over GIT_SSH
git config --get core.sshCommand
# 2. tortoiseplink: git passes -batch to klink
$env:GIT_SSH = "<install-dir>\klink.exe"
$env:GIT_SSH_VARIANT = "tortoiseplink"
git <git-command>
```

**expect:** line 1 prints nothing. git exits with 0 on success. git runs
klink with `-batch`. klink uses the keys loaded in kageant.

**failure:** line 1 prints a command: git ignores `GIT_SSH`. Stop. Report
it to the human. An unknown host key: klink ends at once with
`Cannot confirm a host key in batch mode`, git exit code 128. Stop.
Report it.

**never:** `git config --global` changes the human did not request.
`GIT_SSH_VARIANT=plink`: git passes no `-batch` then. An unknown host key
waits on a question.

### 28. WSL 1

**goal:** Linux ssh inside WSL 1 uses kageant.

**inputs:**

- `<sockpath>`: the Windows path of the agent socket file, in a private
  folder, e.g. `C:\Users\deploy\kageant\agent.sock`. The human picks it and
  gives it to the AI agent.
- `<wslsockpath>`: the same path as WSL 1 sees it: `/mnt/c/` plus the rest
  of `<sockpath>` with forward slashes, e.g.
  `/mnt/c/Users/deploy/kageant/agent.sock`.
- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- `-unix`: kageant also serves an AF_UNIX agent socket at that path.

**command:**

```powershell
# the human, once, in PowerShell: start kageant with a socket in a private folder
kageant -unix <sockpath>
```

```sh
# the AI agent, in WSL 1 (bash):
export SSH_AUTH_SOCK=<wslsockpath>
```

**expect:** ssh and ssh-add in WSL 1 see the keys loaded in kageant.

**failure:** the socket is missing: kageant was not started with `-unix`.
Report it.

**never:** put the socket in a folder other accounts can read. Start or
restart kageant.

### 29. WSL 2

**status:** not available in 0.85.1.20-beta. kageant cannot serve WSL 2
sockets.

**goal:** know what not to do: kageant keys cannot reach WSL 2.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- relay tool: a program that forwards the Windows agent pipe into WSL 2.

**expect:** you are about to break this rule when you plan to download or
install a relay tool so that WSL 2 reaches kageant.

**failure:** you installed a relay tool without the human's go: stop. Report
what you installed and where.

**never:** install relay tools on the machine without the human's go.

### 30. List the keys in kageant (ssh-add)

**goal:** see which keys kageant offers.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- `kageant.conf`: the file `%USERPROFILE%\.ssh\kageant.conf`. It holds the
  path of the kageant pipe (the Windows named pipe of kageant).
- `SSH_AUTH_SOCK`: the environment variable that points ssh-add at the
  agent. The first two command lines set it from `kageant.conf`.
- locked: kageant refuses every key use until an unlock. A locked kageant
  lists no keys.

**command:**

```powershell
# run these lines in order
# kageant.conf holds the pipe with forward slashes. The next line turns them into
# backslashes: the KiTTY++ tests ran ssh-add with that form.
$line = Get-Content "$env:USERPROFILE\.ssh\kageant.conf" -TotalCount 1
$env:SSH_AUTH_SOCK = ($line -replace '^IdentityAgent "(.*)"$','$1') -replace '/','\'
C:\Windows\System32\OpenSSH\ssh-add.exe -l
```

**expect:** one line per key. Exit code 0.

**failure:** `The agent has no identities.`: no key is loaded or kageant is
LOCKED (a locked kageant lists no keys). No `kageant.conf`: stop, report it.

**never:** run ssh-add without `SSH_AUTH_SOCK` set to the kageant pipe. It
then talks to the Windows ssh-agent service, not to kageant. List keys for
anything but a job step: a check before or after an add, a remove or a lock
is a job step.

### 31. Restricted keys

**goal:** know what kageant does with a key the human restricted to
destinations.

**inputs:**

- restricted key: a key with destination rules in kageant (`kageant -h`, or
  kageant's key details > Restrict...). It signs a login to the servers of
  its rules only.
- session-bind: klink, kscp, ksftp, kitty.exe and ssh.exe (OpenSSH 8.9 or
  newer) send kageant the server's host key with a signature by the
  server. kageant learns the destination of each signature from it.
- agent log: kageant's record of key uses, refusals, adds, removes and
  locks. Event 104 `restricted`: a request refused by a key's destination
  rules, with the reason as a token.

**command:**

```text
nothing to run. The AI agent logs in with klink, kscp, ksftp, kitty.exe or Windows ssh.exe (OpenSSH 8.9 or newer).
```

**expect:** a restricted key signs a login to a server of its rules. On a
connection bound to another server kageant leaves the key out of the key
list: the login tries the next key or method. klink, kscp, ksftp and
kitty.exe then show `kageant: 1 key restricted to other servers was not
offered.` (`N keys ... were not offered.`) before the next method.
`ssh-add -l` (no login, no binding) lists every key.

**failure:** `Server refused our key` or `Permission denied (publickey)`
where the restricted key was meant: the server is not in the key's rules, or
the client sent no binding (ssh.exe older than OpenSSH 8.9). The agent log
records event 104 with the reason: `destination`, `unbound`, `bind-failed`,
`session-id`, `hostkey`, `forwarding-hop`, `no-port` (the Valid hosts of a
CA destination has a port condition, the client sent no port) or
`store-damaged` (the rule file cannot be read: every key is refused). Stop.
Report it to the human.

**never:** change a key's destinations: `kageant -h` replaces the key's
rules. Only the human decides them. Reach a server outside the rules
another way (agent forwarding, another key).

## F. Lock and unlock

### 32. Lock kageant (only with allowipclock=yes)

**goal:** stop every key use until the unlock.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- `kageant.conf`: the file `%USERPROFILE%\.ssh\kageant.conf`. It holds the
  path of the kageant pipe (the Windows named pipe of kageant).
- `SSH_AUTH_SOCK`: the environment variable that points ssh-add at the
  agent. The first two command lines set it from `kageant.conf`.
- `allowipclock`: the kageant setting that lets programs lock and unlock
  kageant over its pipe. Default `no`.
- lock passphrase: a passphrase the AI agent picks at the lock. The unlock
  needs the same passphrase. The AI agent keeps it in memory for this
  session only. It never writes it to a file, a log or a report. Unsure
  whether it can keep it until the unlock: the human locks instead (tray
  menu "Lock agent").
- console: a Windows console window attached to the shell. ssh-add reads the
  passphrase from it. A shell without a console cannot lock or unlock with
  ssh-add.
- agent log: kageant's record of key uses, refusals, adds, removes and
  locks.

**command:**

```powershell
# run these lines in order
# kageant.conf holds the pipe with forward slashes. The next line turns them into
# backslashes: the KiTTY++ tests ran ssh-add with that form.
$line = Get-Content "$env:USERPROFILE\.ssh\kageant.conf" -TotalCount 1
$env:SSH_AUTH_SOCK = ($line -replace '^IdentityAgent "(.*)"$','$1') -replace '/','\'
C:\Windows\System32\OpenSSH\ssh-add.exe -x
# ssh-add -x reads the lock passphrase from the console: it needs a console
```

**expect:** kageant is LOCKED. A notice on screen identifies the program. The
key list is empty.

**failure:** refused (`allowipclock=no` is the default): ssh-add reports a
failure. The agent log records event 120 `lock` with `result: blocked`. Stop.
Report it to the human. The human locks kageant from the tray menu or sets
`allowipclock=yes`. A lock set by another program is refused too. No
console: the AI agent cannot lock kageant. Stop. Report it to the human.

**never:** lock without the human's instruction. Lose the passphrase. Change
`allowipclock`.

### 33. Unlock kageant

**goal:** end a lock this AI agent set.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- `kageant.conf`: the file `%USERPROFILE%\.ssh\kageant.conf`. It holds the
  path of the kageant pipe (the Windows named pipe of kageant).
- `SSH_AUTH_SOCK`: the environment variable that points ssh-add at the
  agent. The first two command lines set it from `kageant.conf`.
- passphrase: the lock passphrase this AI agent gave at `ssh-add -x`, kept
  in memory for this session only.
- console: a Windows console window attached to the shell. ssh-add reads the
  passphrase from it. A shell without a console cannot unlock with ssh-add.
- tray: the kageant icon in the Windows notification area. Its menu has
  "Unlock agent".

**command:**

```powershell
# run these lines in order
# kageant.conf holds the pipe with forward slashes. The next line turns them into
# backslashes: the KiTTY++ tests ran ssh-add with that form.
$line = Get-Content "$env:USERPROFILE\.ssh\kageant.conf" -TotalCount 1
$env:SSH_AUTH_SOCK = ($line -replace '^IdentityAgent "(.*)"$','$1') -replace '/','\'
C:\Windows\System32\OpenSSH\ssh-add.exe -X
# ssh-add -X reads the passphrase from the console: it needs a console
```

**expect:** kageant offers the keys again.

**failure:** wrong passphrase: the next try waits 0.1 s per earlier failure
(10 s at most). After 10 wrong passphrases only the tray's "Unlock agent"
works. Stop after one failure. Report it. No console or a lost passphrase:
stop. Report it to the human, who unlocks from the tray menu.

**never:** try passphrases in a loop. Unlock a lock the human set. Write the
passphrase to a file or a log.

## G. Add and remove keys (only where the policy allows it)

### 34. Read the IPC policy first

**goal:** know before a request whether kageant accepts add and remove over
the pipe.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- IPC: requests that other programs send to kageant over its Windows named
  pipe.
- registry install: KiTTY++ keeps its settings in the registry key
  `HKCU\Software\kapper.net\KiTTY`.
- ini store: a portable copy, or `savemode=file` or `savemode=dir`. KiTTY++
  keeps its settings in a kitty.ini file.
- remove-all: `ssh-add -D`, the removal of every key in kageant.

**command:**

```powershell
# run these lines in order
# portable copy or ini store (savemode=file/dir): the [Agent] section of the kitty.ini in use
# registry install:
reg query "HKCU\Software\kapper.net\KiTTY" /v LockdownMode
reg query "HKCU\Software\kapper.net\KiTTY" /v BlockIpcAdd
reg query "HKCU\Software\kapper.net\KiTTY" /v BlockIpcRemove
reg query "HKCU\Software\kapper.net\KiTTY" /v AllowIpcLock
```

**expect:** `lockdownmode=yes`: no add, no remove. `blockipcadd=yes`: no add.
`blockipcremove=yes`: no remove, no remove-all. All default to no.

**failure:** a value is absent: the default (no) applies.

**never:** write any of these values. `reg add` or `reg delete` on this key.

### 35. Add a key

**goal:** load `<keyfile>` into kageant for this job.

**inputs:**

- `<keyfile>`: the path of the private key file the human gave for this job,
  e.g. `C:\work\deploy_job_key`. ssh-add reads it. The AI agent passes the
  path only: it never opens the file itself.
- `<seconds>`: how long the key stays in kageant, in seconds, e.g. `3600`.
  Pick the expected length of the job. The length is unknown: request a
  value from the human before the add.
- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- `kageant.conf`: the file `%USERPROFILE%\.ssh\kageant.conf`. It holds the
  path of the kageant pipe (the Windows named pipe of kageant).
- `SSH_AUTH_SOCK`: the environment variable that points ssh-add at the
  agent. The first two command lines set it from `kageant.conf`.
- `messageonkeyusage`: the kageant setting that shows the on-screen notice.
- agent log: kageant's record of key uses, refusals, adds, removes and
  locks. `ipc-policy`: a refusal by the setting for requests from other
  programs.

**command:**

```powershell
# run these lines in order
# kageant.conf holds the pipe with forward slashes. The next line turns them into
# backslashes: the KiTTY++ tests ran ssh-add with that form.
$line = Get-Content "$env:USERPROFILE\.ssh\kageant.conf" -TotalCount 1
$env:SSH_AUTH_SOCK = ($line -replace '^IdentityAgent "(.*)"$','$1') -replace '/','\'
C:\Windows\System32\OpenSSH\ssh-add.exe -l
C:\Windows\System32\OpenSSH\ssh-add.exe -t <seconds> <keyfile>
C:\Windows\System32\OpenSSH\ssh-add.exe -l
```

**expect:** exit code 0. A notice on screen identifies the program
(`messageonkeyusage`). The key leaves kageant after `<seconds>`. The line
that the second `ssh-add -l` shows and the first did not is the key this AI
agent added. Keep its fingerprint and `<keyfile>` for the remove (block 36)
and for the report. The agent log records event 110 `add`.

**failure:** refused by policy: ssh-add reports a failure. The agent log
records `blocked` with `ipc-policy`. Stop. Report it.

**never:** add a key without the human's instruction. Add a key without `-t`.

### 36. Remove one key this AI agent added

**goal:** take back a key added for this job.

**inputs:**

- `<pubkeyfile>`: the public key file of the key this AI agent added, e.g.
  `C:\work\deploy_job_key.pub`. The human gives it with the private key
  file. No `.pub` file: request it from the human. What `ssh-add -d` does
  with the private key file alone is not documented, test it before relying
  on it.
- added key: a key that block 35 showed as new in `ssh-add -l`, in this
  session. Only these keys are this AI agent's to remove.
- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- `kageant.conf`: the file `%USERPROFILE%\.ssh\kageant.conf`. It holds the
  path of the kageant pipe (the Windows named pipe of kageant).
- `SSH_AUTH_SOCK`: the environment variable that points ssh-add at the
  agent. The first two command lines set it from `kageant.conf`.
- `blockipcremove`, `lockdownmode`: kageant settings that refuse a remove
  from other programs.

**command:**

```powershell
# run these lines in order
# kageant.conf holds the pipe with forward slashes. The next line turns them into
# backslashes: the KiTTY++ tests ran ssh-add with that form.
$line = Get-Content "$env:USERPROFILE\.ssh\kageant.conf" -TotalCount 1
$env:SSH_AUTH_SOCK = ($line -replace '^IdentityAgent "(.*)"$','$1') -replace '/','\'
C:\Windows\System32\OpenSSH\ssh-add.exe -d <pubkeyfile>
```

**expect:** exit code 0. A notice on screen. The agent log records event 114
`remove`. The fingerprint is gone from `ssh-add -l`.

**failure:** refused by policy (`blockipcremove` or `lockdownmode`): report
it. The key leaves kageant at the end of its `-t` time in any case.

**never:** remove a key this AI agent did not add. `ssh-add -D` (it removes
every key in kageant).

## H. What an AI agent must not do

### 37. Never switch off host key checking

**goal:** know what not to do: no switch turns the host key check off. Use
the pinned form.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- host key store: the server keys the user already trusts. KiTTY++ keeps it.
- pinned form: a command line with `-hostkey <fp>`. The given host keys
  replace the host key store for this run.
- `new` scan row: a key the store does not have. `MISMATCH`: a key that
  differs from the stored key.

**command:**

```powershell
# use the pinned form instead. One -hostkey per key type klink -scan returned as "stored".
klink -batch -hostkey <fp> -l <user> -P <port> <host> "<command>"
```

**expect:** the pinned form runs `<command>` only when the server's key
matches `<fp>`. You are about to break this rule when a command line or a
file edit of yours contains `autostoresshkey=yes`,
`StrictHostKeyChecking=no`, a fingerprint from a `new` row or
`-accept-new` for a host with a `MISMATCH`.

**failure:** a host key refusal: stop. Report the host and the fingerprints.

**never:** set `[KiTTY] autostoresshkey=yes`. Edit the host key store. Pin a
key from a `new` scan row. Use `-accept-new` against a `MISMATCH`. Pass
`StrictHostKeyChecking=no` to ssh.exe.

### 38. Never empty or change kageant

**goal:** know what not to do: keep the keys and the state of kageant as the
human left them.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- `-noload`: a kageant start switch. kageant then skips the stored startup
  keys.
- lock: a locked kageant refuses every key use until an unlock.

**expect:** you are about to break this rule when your command line contains
`ssh-add -D`, `ssh-add -d` for a key you did not add, `ssh-add` with a key
file the human did not give, `kageant -noload`, or a command that stops or
starts kageant.

**failure:** you changed kageant: stop. Report what you ran. Do not try to
restore the state yourself.

**never:** `ssh-add -D` (it removes every key in kageant). `ssh-add -d` on a
key this AI agent did not add. Add keys without an instruction. Start
kageant with `-noload`. Restart or end kageant (a restart also drops a lock).

### 39. Never read stored passwords or secrets

**goal:** know what not to do: klink sends a stored session password itself.

**inputs:**

- `<session>`: the name of a saved KiTTY++ session, between the double
  quotes, e.g. `prod-web`. The human gives it.
- `<command>`: the command line to run on the session's host, between the
  double quotes, e.g. `uname -a`. The job defines it.
- master password: the password that protects the passwords stored in
  KiTTY++ sessions.

**command:**

```powershell
klink -batch -load "<session>" "<command>"
```

**expect:** the command runs with the session's stored password. You are
about to break this rule when you open a `Password` value, run
`kitty.exe -exportall`, `-portablecopy` or `-bundlepwfile`, or open a key
file of the user.

**failure:** you read a secret: stop. Do not copy it anywhere. Report which
secret you read.

**never:** read the `Password` values of sessions (registry or session
files). Export sessions (`kitty.exe -exportall`, `-portablecopy`,
`-bundlepwfile`). Read or request the master password. Open or read the
user's key files. Allowed: the path of a key file the human gave for this
job, passed to `ssh-add` (block 35).

### 40. Never put a password on the command line

**goal:** know what not to do: use a password file the human created.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- `<pwfile>`: the path of a password file the human created, e.g.
  `C:\work\deploy.pw`. It holds the password on one line. The human gives the
  path.
- `<proxypwfile>`: the path of a proxy password file the human created, e.g.
  `C:\work\proxy.pw`. The human gives the path.
- `<password>`: a password in clear text. It appears only in the never line:
  the AI agent never types it.

**command:**

```powershell
klink -batch -l <user> -pwfile <pwfile> <host> "<command>"
# a proxy password: -proxypwfile <proxypwfile>
```

**expect:** the login uses the password from `<pwfile>`. You are about to
break this rule when your command line contains `-pw` followed by a
password.

**failure:** you passed `-pw`: stop. Report it to the human.

**never:** `-pw <password>`. The password would pass through your own context: the prompt, the logs, the model provider. klink wipes it from its command line after reading it, but a process-creation audit (event 4688, Sysmon) records it first.

### 41. Never forward kageant

**goal:** know what not to do: do not forward kageant to a server.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- `klink -A`: agent forwarding. The server gets access to kageant for the
  time of the connection.

**expect:** you are about to break this rule when your klink command line
contains `-A` and the human did not order it for that host.

**failure:** you forwarded kageant: end the connection. Report the host.

**never:** `klink -A` unless the human orders it for one trusted host. The far
end can then sign with every key loaded in kageant.

### 42. Never answer a confirmation for the human

**goal:** know what not to do: wait for the human or report the wait.

**inputs:**

- kageant confirmation: a kageant box in which the human approves or refuses
  one key use.
- Windows Hello prompt: a Windows box that requires a PIN, a fingerprint or
  a face check.
- `askconfirmation`: the kageant setting that turns these confirmations on.

**expect:** you are about to break this rule when you plan to click, type or
send keys into a window that kageant or Windows Hello opened, or to change
`askconfirmation`.

**failure:** you answered a confirmation: stop. Report it to the human.

**never:** click, type or send keys into a kageant confirmation or a Windows
Hello prompt. Set `askconfirmation=no` to get past one.

### 43. Never change settings or the machine

**goal:** know what not to do: leave the KiTTY++ settings and the machine as
they are.

**inputs:**

- kitty.ini: a KiTTY++ settings file.
- `HKCU\Software\kapper.net\KiTTY`: the registry key with the KiTTY++
  settings.
- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.

**expect:** you are about to break this rule when you open kitty.ini or the
registry key above for writing, or your command line contains one of the
switches in the never line.

**failure:** you changed a setting: stop. Report the setting and the old
value.

**never:** edit kitty.ini, the kageant settings or
`HKCU\Software\kapper.net\KiTTY`. `kitty.exe -cleanup`, `-sshhandler`,
`-fileassoc`, `-takefolder`, `-importdir`, `-backupnow`.
`kageant -openssh-config` (it overwrites the file).

### 44. Never use unsafe transfer modes

**goal:** know what not to do: no unsafe transfer modes.

**inputs:**

- `kscp -unsafe`: kscp accepts server-side wildcards.
- `ksftp -be`: ksftp continues after a failed command.

**expect:** you are about to break this rule when your kscp command line
contains `-unsafe` or your ksftp command line contains `-be`.

**failure:** you ran one of them: stop. Check the transferred files. Report
it.

**never:** `kscp -unsafe`. `ksftp -be`.

## I. Exit codes and output

### 45. klink exit codes

**goal:** read a klink result correctly.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`. The job defines it.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.

**command:**

```powershell
# run these lines in order
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
klink -batch -hostkey <fp> -l <user> -P <port> <host> "<command>"
$LASTEXITCODE
```

**expect:** after a completed run the exit code is the exit code of
`<command>`, whatever its value. Standard output is the remote standard
output. Standard error is the remote standard error plus klink's messages.

**failure:** exit code 1 is the remote exit code 1 or a klink failure
(options, host, connection, a refused question in `-batch`,
`Remote process exit code unavailable`, a cancelled question). Read standard
error to separate the two. klink's own messages: block 13. Exit code 2 is a
remote exit code. klink itself returns 2 only for `-scan` with a
`MISMATCH`.

**never:** treat exit code 1 as a remote result without reading standard
error.

### 46. klink -scan and -knownhosts output

**goal:** parse the output of the host key tools.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `status` values: `stored` (the key matches the store), `new` (the store has
  no key of this type for `<host>`), `MISMATCH` (the key differs from the
  stored key), `not offered` (the server does not offer this host key
  algorithm), `unreachable` (no key came for another reason: the `error`
  field holds it), `not signing` (one more stored key of the same type. The
  server does not sign with it now).
- `sha256`, `md5`: the key the server presented. In a `not signing` row:
  the stored key. `stored_sha256`, `stored_md5`: the stored key of a
  `MISMATCH` row. In every other row they are `-`.
- store: the host key store, the server keys the user already trusts.

**command:**

```powershell
# one of these forms: pick the one that fits
klink -scan <host>:<port> -json
klink -knownhosts <host>:<port> -json
```

**expect:** `-scan`: exit code 0 when keys were seen and none is a
`MISMATCH`. One JSON array with the
fields `host`, `port`, `type`, `bits`, `sha256`, `md5`, `key`, `status`,
`first_seen`, `last_written`, `stored_sha256`, `stored_md5`, `error`.
`-knownhosts`: exit code 0 with at least one key.

**failure:** `-scan` exit code 2: at least one `MISMATCH`. `-scan` exit code
1: no key. `-knownhosts` exit code 1: no key.

**never:** trust a `new` row.

### 47. kscp exit codes

**goal:** read a kscp result correctly.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<local>`: the file to upload, a path on this computer, e.g.
  `C:\work\report.csv`.
- `<remote>`: the target path on `<host>` (first line) or the path to check
  (second line), absolute (`/srv/data/`) or relative to the home folder of
  `<user>` on `<host>` (`data/`).
- In the output: `<bytes>` is the file size in bytes (e.g. `10240`). `<text>`
  is the error text of kscp.

**command:**

```powershell
# one of these forms: a copy (first line) or a path check (second line)
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
kscp -batch -q -hostkey <fp> -P <port> <local> <user>@<host>:<remote>
kscp -batch -stat -hostkey <fp> -P <port> <user>@<host>:<remote>
```

**expect:** exit code 0: every file was copied. `-stat`: one line on standard
output, `file <bytes>` or `dir`, exit code 0.

**failure:** exit code 1: any error or an option error. `-stat`: `other`,
`missing` or `error <text>`, exit code 1.

**never:** treat a partial copy as done.

### 48. ksftp exit codes

**goal:** read a ksftp result correctly.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`. The human gives it with the job.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`. The human gives it
  with the job.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<batchfile>`: a local text file with one SFTP command per line (`cd`,
  `put`, `get`, `mkdir`, `rm`), e.g. `C:\work\upload.sftp`. The AI agent
  writes it for the job.
- `-be`: ksftp continues after a failed command.

**command:**

```powershell
# <fp>: a stored key from klink -scan, or one the human confirmed. One -hostkey per key type.
ksftp -batch -hostkey <fp> -l <user> -P <port> -b <batchfile> <host>
```

**expect:** exit code 0: all commands done.

**failure:** exit code 1: the connection or the SFTP start failed, or
`<batchfile>` is not readable. Exit code 2: a command of `<batchfile>` failed
(without `-be`).

**never:** `-be`.
