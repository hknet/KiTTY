valid for 0.85.1.20-beta

# KiTTY++ for AI agents

Part 1 is for an AI agent that runs KiTTY++ tools on Windows. Part 2 is for
the human who sets up an AI agent's access.

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

# Part 1. Tasks for an AI agent

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
The human's setup of the three layers: block 58.

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

**status:** the certificate check is PuTTY's code. Its use with `-batch` is
untested in 0.85.1.20-beta.

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

**status:** tested with klink. kscp and ksftp take the same switches,
untested in 0.85.1.20-beta.

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

**status:** untested in 0.85.1.20-beta.

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
$env:GIT_SSH = "C:\Windows\System32\OpenSSH\ssh.exe"
git <git-command>
```

**expect:** the managed Include block in `~/.ssh/config` makes ssh.exe use
kageant. git exits with 0 on success.

**failure:** `Permission denied (publickey)`: kageant is locked (empty key
list), the key is not loaded or a confirmation was refused. Report it. Do not
retry in a loop. ssh.exe checks the git server's host key in
`%USERPROFILE%\.ssh\known_hosts` (block 25). Without `BatchMode=yes` an
unknown or changed key leads to a prompt or a refusal (OpenSSH behaviour):
the run waits or fails. Stop. Report it to the human.

**never:** `git config --global` changes the human did not request.
`GIT_SSH_COMMAND` with `StrictHostKeyChecking=no`.

### 27. git with klink

**status:** untested in 0.85.1.20-beta. git's handling of klink.exe
(`GIT_SSH_VARIANT`, `-batch`, `-P`) is git behaviour.

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
$env:GIT_SSH = "<install-dir>\klink.exe"
$env:GIT_SSH_VARIANT = "plink"
git <git-command>
```

**expect:** git exits with 0 on success. klink uses the keys loaded in
kageant.

**failure:** a host key question waits forever: git did not pass `-batch`.
Stop the run. Report it.

**never:** `git config --global` changes the human did not request.

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

- restricted key: a key with destination rules in kageant (`ssh-add -h`, or
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

**never:** change a key's destinations: `ssh-add -h` replaces the key's
rules, and only the human decides them. Reach a server outside the rules
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

### 37. Switch off host key checking

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

### 38. Empty or change kageant

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

### 39. Read stored passwords or secrets

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

### 40. Put a password on the command line

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

### 41. Forward kageant

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

### 42. Answer a confirmation for the human

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

### 43. Change settings or the machine

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

### 44. Unsafe transfer modes

**goal:** know what not to do: no unsafe transfer modes.

**inputs:**

- `kscp -unsafe`: kscp accepts server-side wildcards.
- `ksftp -be`: ksftp continues after a failed command.

**expect:** you are about to break this rule when your kscp command line
contains `-unsafe` or your ksftp command line contains `-be`.

**failure:** you ran one of them: stop. Check the transferred files. Report
it.

**never:** `kscp -unsafe`. `ksftp -be`.

## I. For the human who sets it up

### 45. Agent log on

**goal:** every signature, refusal, add, remove and lock on record.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- agent log: kageant's record of key uses, refusals, adds, removes and
  locks. `agentlog=yes` is the default.
- agent log file: `kageant.log`. Its folder: next to the kitty.ini in use
  when the kitty.ini is authoritative (portable copy, `savemode=dir` or
  `savemode=file`). Else `%LOCALAPPDATA%\kapper.net\KiTTY`. `[Agent]
  agentlogpath` sets another path. Rotated copies: `kageant.log.1`,
  `kageant.log.2`, ... (`agentlogmaxkb`, default 5120, `agentlogkeep`,
  default 3, `agentlogexpiredays`, default 90).
- line format: one line per record: `ts=` with the time, `ev="..."` and
  further `name="value"` fields, e.g.
  `ts=2026-08-22T12:34:56Z ev="sign" fp="SHA256:..." result="allowed"`.
  The time is UTC.
- `agentlogsink`: `file`, `eventlog` or `both`. Without a value: `both` with
  the system MSI, else `file`.
- agent event IDs (the `ev` value in the file, the event ID in the Windows
  Event Log):

  | ID | `ev` | meaning |
  |---:|---|---|
  | 100 | `sign` | a signature: allowed or denied |
  | 101 | `confirm` | the answer to a confirmation |
  | 102 | `session-bind` | a client bound the connection to a server |
  | 103 | `request` | a request kageant refused |
  | 110 | `add` | a key added: done or blocked |
  | 111 | `loadkey` | a key file loaded |
  | 112 | `forget-path` | a key file path forgotten |
  | 113 | `retry` | startup keys retried |
  | 114 | `remove` | a key removed: done or blocked |
  | 115 | `remove-all` | every key removed: done or blocked |
  | 120 | `lock` | a lock |
  | 121 | `unlock` | an unlock |
  | 130 | `hello-enrol` | Windows Hello added to a key |
  | 131 | `hello-protect` | a key protected with Windows Hello |
  | 132 | `hello-unlock` | a Windows Hello protected key opened |
  | 140 | `keyfile-warning` | a weak or old key file |
  | 150 | `reencrypt-all` | every key re-encrypted |
  | 151 | `reencrypt` | one key re-encrypted |
  | 152 | `decrypt` | a key decrypted |
  | 160 | `agent` | kageant started or stopped |
  | 170 | `logrotate` | the log rotation failed |

  The full field list: docs/EVENT-IDS.md.

**command:**

```ini
; kageant Settings > Log, or kitty.ini:
[Agent]
agentlog=yes
agentlogsink=both
```

```powershell
# one of these forms: check that records arrive (after one login with a kageant key)
Get-Content "$env:LOCALAPPDATA\kapper.net\KiTTY\kageant.log" -Tail 5
Get-WinEvent -FilterHashtable @{LogName='KiTTY++_Agent'} -MaxEvents 5
Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='KiTTY++_agent'} -MaxEvents 5
```

**expect:** the agent log file plus the Windows Event Log (KiTTY++_Agent,
source KiTTY++_agent) for a system-wide install. Else the Windows Event Log
(Application, source KiTTY++_agent). Event IDs 100-199. The check shows the
`sign` record of the login. The first check line is for a registry install
without `agentlogpath`. Read the second line with the system MSI, the third
line without it.

**failure:** no KiTTY++_Agent log: a per-user install or a portable copy. The
records are in the Windows Event Log (Application, source KiTTY++_agent). No
new record after the login: `agentlog` is off or the sink leaves out that
destination. `ssh-add -l` writes no record.

### 46. Records of klink, kscp, ksftp

**goal:** each AI agent run on record.

**inputs:**

- `clieventlog=yes`: klink, kscp and ksftp write a record of each run to the
  Windows Event Log.

**command:**

```ini
; Application > Security > Windows Event Log, or kitty.ini:
[KiTTY]
clieventlog=yes
```

```powershell
# one of these forms: check that records arrive (after one klink run)
Get-WinEvent -FilterHashtable @{LogName='KiTTY++_Session'; ProviderName='KiTTY++_cli'} -MaxEvents 5
Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='KiTTY++_cli'} -MaxEvents 5
```

**expect:** the Windows Event Log (KiTTY++_Session, source KiTTY++_cli),
event IDs 300-331. Without the system MSI: the Windows Event Log
(Application, source KiTTY++_cli). The host key records: 310
`hostkey-known` (`basis`: `cache`, `manual` or `ca`), 311 `hostkey-new`
(`result`: `accept-new`, `accepted` or `refused`), 312 `hostkey-mismatch`.
A jump host's records carry `proxy: ssh`. The full list:
docs/EVENT-IDS.md.

**failure:** no KiTTY++_Session log: KiTTY++ was not installed with the
system MSI. The records are in the Windows Event Log (Application, source
KiTTY++_cli).

### 47. Session logs

**goal:** the output of AI agent sessions on file.

**inputs:**

- `<host>`: the server, as a DNS name or an IP address, e.g.
  `server.example.com` or `192.0.2.10`.
- `<port>`: the SSH port of `<host>`, a number, e.g. `22` (the SSH default).
- `<user>`: the login account on `<host>`, e.g. `deploy`.
- `<command>`: the command line to run on `<host>`, between the double
  quotes, e.g. `uname -a`.
- `<fp>`: the SHA256 fingerprint of a host key of `<host>`. Form: `SHA256:`
  plus 43 base64 characters, e.g.
  `SHA256:q3RmT8vXa1bC2dE4fG5hJ6kL7mN8pQ9rS0tU1vW2xY3`. Source: run
  `klink -scan <host>:<port> -json`. Take the `sha256` field of a row whose
  `status` is `stored`, or a fingerprint the human confirmed. Give one
  `-hostkey <fp>` per key type the scan returned. A host a trusted CA signed
  (block 8): leave `-hostkey <fp>` out.
- `<logfile>`: the path of the protocol log file, e.g.
  `C:\work\logs\aiagent-ssh.log`.
- `<local>`: a file on this computer to upload, e.g. `C:\work\report.csv`.
- `<remote>`: the target path on `<host>`, absolute (`/srv/data/`) or
  relative to the home folder of `<user>` on `<host>` (`data/`).
- `<batchfile>`: a local text file with one SFTP command per line, e.g.
  `C:\work\upload.sftp`.
- protocol log: the SSH protocol messages of klink, kscp or ksftp, not the
  terminal output. `-logappend`: add to an existing log file.

**command:**

```powershell
# one of these forms: one line per tool
# kitty.exe: Session > Logging on the session or on Default Settings (LogType, LogFileName)
# klink, kscp, ksftp: the protocol log, not the terminal output
klink -batch -hostkey <fp> -l <user> -P <port> -sshlog <logfile> -logappend <host> "<command>"
kscp -batch -q -hostkey <fp> -l <user> -P <port> -sshlog <logfile> -logappend <local> <host>:<remote>
ksftp -batch -hostkey <fp> -l <user> -P <port> -sshlog <logfile> -logappend -b <batchfile> <host>
```

**expect:** one log per session.

**failure:** with nobody at the desktop an existing log file is not
written: event 907 in the Windows Event Log (Application, source KiTTY++).

### 48. Confirmation per key, for every key, or by Windows Hello

**goal:** the human approves each signature.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- `askconfirmation=auto`: a key needs a confirmation when its comment
  contains `confirmation`.
- Windows Hello gesture: a PIN, a fingerprint or a face check of Windows
  Hello.

**command:**

```ini
; every key:
[Agent]
askconfirmation=yes
; one key: kageant key details > "Confirm each use:"
; (or "confirmation" in the key comment with askconfirmation=auto)
; Windows Hello for every confirmation:
helloconfirm=yes
```

**expect:** a prompt per signature. With `helloconfirm` the prompt is a
Windows Hello gesture. When Windows Hello cannot run, the request is refused,
never downgraded to a click.

**failure:** the AI agent's klink or ssh.exe waits until the human answers.
Plan for the wait.

### 49. Session-bind with destination

**goal:** the agent log records which server each signature was for.

**inputs:**

- session-bind: klink, kscp, ksftp, kitty.exe and ssh.exe (OpenSSH 8.9 or
  newer) send kageant the server's host key with a signature by the
  server. kageant learns the destination of each signature from it.
- agent log: kageant's record of key uses, refusals, adds, removes and
  locks. `dest`: the destination field. `fwd`: the field that marks a
  forwarded agent connection.

**command:**

```text
nothing to switch on. The AI agent logs in with klink, kscp, ksftp, kitty.exe or Windows ssh.exe (OpenSSH 8.9 or newer).
```

**expect:** the confirmation shows `Destination:` (host and SHA256 of the
host key). Agent log fields `dest` and `fwd`. Event 102 for each binding.
klink `-v` shows `Agent connection bound to host key`.

**failure:** `Agent connection not bound`: the agent does not know
session-bind (not kageant, or an older kageant). The login works, without a
destination in the record. ssh.exe older than OpenSSH 8.9 sends no binding.

### 50. Restrict a key to the AI agent's hosts

**status:** untested with ssh-add.exe. The same message, sent down the
kageant pipe, is tested.

**goal:** the AI agent's key signs a login to the job's servers only.

**inputs:**

- `<keyfile>`: the AI agent's private key file (block 59).
- `<host>`: a server of the job, e.g. `web01`. Its host key must be known:
  in the KiTTY++ host-key store, signed by a host CA of the CA store (block
  58), or in known_hosts when kageant reads that file (Settings > Agent >
  "Use OpenSSH's known_hosts as a host-key source", default off).
- `<jump>`: a jump host the job goes through, e.g. `jump01`.
- kageant key details: double-click the key in kageant's key list.
- `kageant.conf`: the file `%USERPROFILE%\.ssh\kageant.conf`. It holds the
  path of the kageant pipe (the Windows named pipe of kageant).
- `SSH_AUTH_SOCK`: the environment variable that points ssh-add at the
  agent. The first two command lines set it from `kageant.conf`.

**command:**

```text
# one of these forms: in kageant's window
In kageant's key list: double-click the key, Restrict..., Add destination, Host: <host>, Look up host keys, OK.
For a jump host: one destination for <jump>, one for <host> with Via host: <jump> (the key then works through an agent forwarded to <jump>).
For a group of servers: Host: *.<domain>, then Pattern: Fixed list: host keys known now, or Checked at each login: any matching host (the human decides: it trusts the host-key store, and known_hosts while kageant reads it).
For every server of a host CA: Add destination, All servers of a host CA, CA: <the CA of the CA store> (the server's certificate must hold its names).
```

```powershell
# one of these forms: OpenSSH's ssh-add (host keys from known_hosts)
# kageant.conf holds the pipe with forward slashes. The next line turns them into
# backslashes: the KiTTY++ tests ran ssh-add with that form.
$line = Get-Content "$env:USERPROFILE\.ssh\kageant.conf" -TotalCount 1
$env:SSH_AUTH_SOCK = ($line -replace '^IdentityAgent "(.*)"$','$1') -replace '/','\'
C:\Windows\System32\OpenSSH\ssh-add.exe -h <host> <keyfile>
C:\Windows\System32\OpenSSH\ssh-add.exe -h <jump> -h "<jump>><host>" <keyfile>
```

**expect:** key details lists under `Destinations:` the count on the first
line, then one line per destination: the host, then `user`, `via` and
`forwarding` where the destination has them, and below each its origin
(`Set by <program path>, <date time> (host keys looked up by that program)`
for ssh-add, `Set in the rule editor`). The key list draws the key's
row in blue. Its State column ends in `, restricted`.
The rules are kept in `kageant-rules.dat` (`%APPDATA%\KiTTY`, or beside
kitty.ini in a portable copy), encrypted for the Windows account, and apply
again after a restart, whichever way the key comes back. Event 110 `add`
carries them in `restrict`. A refusal is event 104.

**failure:** `No host key of <host> in the KiTTY++ host-key store or the
CA store.` (with known_hosts in the list while kageant reads it): log in to
`<host>` once yourself (or scan it, block 7) to store its key, then look up
again. ssh-add with no known_hosts
entry stops with an error before it reaches kageant.

**never:** restrict a key that other programs need for other hosts. Use a
key of its own for the AI agent (block 59). A re-add with `ssh-add -h`
replaces the rules. A re-add without `-h` keeps them.

### 51. IPC policy and IPC lock

**goal:** decide what programs may do with kageant.

**inputs:**

- IPC: requests that other programs send to kageant over its Windows named
  pipe.
- `lockdownmode`: refuse every add, remove and remove-all over IPC.
  `blockipcadd`: refuse adds. `blockipcremove`: refuse removes.
  `allowipclock`: let programs lock and unlock kageant.

**command:**

```ini
; kageant Settings > Security, or kitty.ini:
[Agent]
lockdownmode=no
blockipcadd=yes
blockipcremove=yes
allowipclock=no
```

**expect:** signing and listing always work. Add, remove and lock follow the
policy.

**failure:** a program's add, remove or lock is refused: the agent log
records `blocked` with `ipc-policy`. Change the policy only when that
program needs the right.

### 52. Register kageant as the Windows OpenSSH agent

**goal:** ssh.exe and git find kageant.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- Include block: lines in `%USERPROFILE%\.ssh\config` that load
  `kageant.conf`. `kageant.conf` points ssh.exe at the kageant pipe.

**command:**

```text
kageant tray menu > "Register as Windows OpenSSH agent" (default off)
```

**expect:** `%USERPROFILE%\.ssh\kageant.conf` plus a marked Include block in
`%USERPROFILE%\.ssh\config`. A one-time backup `config.kageant.bak`.

**failure:** an existing manual `Include kageant.conf` stays as it is.

### 53. Keep jobs running while Windows is locked

**goal:** a screen lock does not break the AI agent's job.

**inputs:**

- `lockwithwindows`: what kageant does when Windows locks the screen.
  `keep`: kageant stays usable. `lock`, `reencrypt`: kageant blocks key use.

**command:**

```ini
[Agent]
lockwithwindows=keep
```

**expect:** kageant stays usable while the screen is locked. `keep` is the
default.

**failure:** `lock` or `reencrypt`: the job's logins fail until the human
unlocks Windows.

## J. Exit codes and output

### 54. klink exit codes

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

### 55. klink -scan and -knownhosts output

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

### 56. kscp exit codes

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

### 57. ksftp exit codes

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

# Part 2. Setting up a safe workspace for an AI agent

For the human who sets up an AI agent's access. KiTTY++ covers the
authentication. The rights of the AI agent on the remote systems are set on
those systems. The server steps of this part assume Linux servers with
OpenSSH.

### 58. Host keys: set up the three layers

**goal:** the AI agent can trust a host key without guessing.

**inputs:**

- `<ca-name>`: a name for the CA record, e.g. `lab-host-ca`.
- `<id>`: the certificate's identity, e.g. `web01`.
- `<names>`: the host names the certificate is valid for, separated by
  commas, e.g. `web01.example.com,192.0.2.10`. List every name the AI agent
  connects by.
- `<hostgroup>`: the hosts layer 2 may accept, e.g. `*.lab.example.com`.
- host CA: an SSH key pair that signs host keys. `host_ca` is its private
  half, `host_ca.pub` its public half.
- Valid hosts: the expression of a CA record that limits the hosts the CA
  may vouch for, e.g. `*.example.com`. Several are joined with `||`.

**command:**

```text
Layer 1, host certificates (recommended for many hosts):
1. Once: create the host CA and keep host_ca offline:
   ssh-keygen -t ed25519 -f host_ca -C "host CA"
2. Per server: sign its host key and point sshd at the certificate:
   ssh-keygen -s host_ca -I <id> -h -n <names> -V +52w /etc/ssh/ssh_host_ed25519_key.pub
   /etc/ssh/sshd_config: HostCertificate /etc/ssh/ssh_host_ed25519_key-cert.pub
   then reload sshd
3. Signed in as the Windows account the AI agent runs as: KiTTY++
   configuration box > Application > Security > Certificate Authorities
   (or start: kitty.exe -host-ca). "Name for this CA" <ca-name>,
   "Public key of certification authority" the content of host_ca.pub,
   "Valid hosts this key is trusted to certify" a narrow expression. Save.

Layer 2, accept-new: write it into the job, one line per group:
   accept-new for <hostgroup>

Layer 3, a fingerprint: compare each sha256 the AI agent shows with the
server's own record, read on the server:
   ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
```

**expect:** layer 1: klink, kscp and ksftp accept every host the CA signed
for a name the AI agent connects by, jump hosts included. With
`clieventlog=yes` each such connection is event 310 with `basis: ca`. Layer
2: every new key is in the AI agent's report and in event 311. Layer 3: the
AI agent pins the confirmed fingerprints.

**failure:** layer 1 does not take effect: the "Valid hosts" expression does
not match the name the AI agent connects by, the certificate does not list
that name, or it expired. Connecting by an IP address fails when only host
names are listed. Layer 2: the AI agent reports a changed key. Check the
server before anything else.

**never:** a "Valid hosts" of `*`. Put `host_ca` on a server or on the AI
agent's computer. Set `autostoresshkey=yes` as a replacement for layer 2.

### 59. Create a key for the AI agent

**goal:** a key pair that belongs to the AI agent's job only.

**inputs:**

- `<keyname>`: the file name of the new key without extension, e.g.
  `aiagent`.
- kittygen-cli: the KiTTY++ key generator for the command line. kittygen is
  the same with a window.

**command:**

```text
# run these lines in order, in a folder only the human (or the AI agent's account) can read
kittygen-cli -t ed25519 -o <keyname>.ppk
kittygen-cli -L <keyname>.ppk -o <keyname>.pub
```

**expect:** `<keyname>.ppk` (the private key) and `<keyname>.pub` (the
public half in OpenSSH's one-line form for `authorized_keys`, block 68). Protect
the `.ppk` with Windows Hello next (block 60).

**failure:** kittygen-cli ends with an error: the reason is on standard
error.

**never:** reuse a human's key for the AI agent. Give the AI agent the
`.ppk` of a key it does not need.

### 60. Keys protected by Windows Hello

**goal:** a key that a leaked passphrase alone cannot open or use.

**inputs:**

- `<name>`: the file name of the original key without `.ppk`, e.g. `deploy`
  for `deploy.ppk`. kageant builds the names of the protected copy from it.
- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- Windows Hello gesture: a PIN, a fingerprint or a face check of Windows
  Hello.
- printout: the secret kageant shows once for the recovery of the protected
  copy. Without the recovery code choice it is the protected copy's
  passphrase: it opens the key in any PuTTY-compatible tool on any computer.
  With the choice "Printout is a recovery code bound to the .hello file" it
  opens the key only together with the `.hello` file, in KiTTY++ tools.
- recovery passphrase: a second way to open the protected copy where Windows
  Hello cannot run (another computer, a new Windows Hello setup).

**command:**

```text
1. kageant key list > the key > key details > "Protect with Windows Hello..."
2. Recovery passphrase, one of:
   - a new "Recovery passphrase:" that is not the key's current passphrase:
     the key opens anywhere with it
   - "No recovery passphrase - Windows Hello and the printed secret only":
     lose Windows Hello on this computer and only the printout opens the key
   (not "Use the current passphrase as the recovery passphrase": see failure)
3. Printout form: tick "Printout is a recovery code bound to the .hello
   file" when a stolen printout alone must open nothing. Then back up the
   .hello file too: without it the code opens nothing.
4. Store the printout offline.
5. Test the protected copy. Then delete the original key file.
6. key details > "Confirm each use:" > the Windows Hello entry
7. Deploy the public half of the key to the target servers. The protected
   copy is the same key: its public half is unchanged.
```

**expect:** the protected copy `<name>-hello.ppk` with `<name>-hello.ppk.hello`
beside it. Its passphrase is a random secret. The file opens through Windows
Hello on this account and computer, the recovery passphrase (when one was
set) or the printout. With the Windows Hello entry of "Confirm each use:"
each signature needs a Windows Hello gesture. When Windows Hello cannot run (no Hello credential,
policy, a remote desktop session) the use is refused, never downgraded to a
click.

**failure:** with "Use the current passphrase as the recovery passphrase" the
old passphrase still opens the key together with the two files. Protect the
key again without that choice. Windows Hello is not set up on the computer:
the box refuses the protection.

**never:** keep the original key file beside the protected copy. Deploy the
public half of an unprotected key to the AI agent's servers.

### 61. Confirm each use of one key

**goal:** the human approves each signature of one key.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- Windows Hello gesture: a PIN, a fingerprint or a face check of Windows
  Hello.

**command:**

```text
kageant key list > the key > key details > "Confirm each use:"
  the Yes/No entry:          a Yes/No box per signature
  the Windows Hello entry:   a Windows Hello gesture per signature
for every key: kageant Settings > "Confirmations require Windows Hello"
  (kitty.ini [Agent] helloconfirm=yes)
```

**expect:** each signature waits for the human. The confirmation shows the
key. For klink, kscp, ksftp, kitty.exe and Windows ssh.exe (OpenSSH 8.9 or
newer) it shows the destination too.
A Yes/No box can be clicked by any program that runs as the same Windows
user. A Windows Hello prompt cannot.

**failure:** Windows Hello cannot run: the use is refused, never downgraded.
The AI agent's login fails with `Permission denied (publickey)`.

**never:** leave a key the AI agent uses on "no" when the AI agent runs as
the same Windows user.

### 62. A separate Windows account for the AI agent

**status:** the combination of a second account, its own kageant and its own
Windows Hello keys is untested in 0.85.1.20-beta.

**goal:** the AI agent cannot read the KiTTY++ files, registry values or keys
of the user.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- kageant pipe: the Windows named pipe through which ssh.exe and ssh-add
  reach kageant.
- HKCU: the registry hive of the signed-in Windows account.

**command:**

```text
1. Create a standard Windows account for the AI agent. Install KiTTY++ for it
   (per-user MSI or a portable copy in a folder only that account can read).
   A portable folder: in its Properties > Security, remove every entry but
   the AI agent's account, SYSTEM and Administrators.
2. Give the account the tools it needs. Give it no access to the user's
   profile, the user's kitty.ini or HKCU of the user.
3. Sign in to the account once. Set up Windows Hello (PIN) for it.
4. In that account: start its own kageant. Tray menu > "Start kageant at
   login" starts it at every sign-in. "Load remembered keys at startup"
   loads its keys. Load keys made for the AI agent (block 59). Protect
   them with Windows Hello. Set "Confirm each use:".
5. In that account: set up the host CA records (block 58). They are kept per
   Windows account.
6. Deploy only the public halves of those keys to the target servers.
```

**expect:** the AI agent's account has its own kageant pipe, its own keys,
its own settings and its own agent log. The user's keys never reach it.

**failure:** the AI agent's ssh-add or ssh.exe finds no kageant: kageant is
not running in the AI agent's account. Start it there.

**never:** point the AI agent's account at the user's kageant pipe. It does
not work. Copy the user's key files into the AI agent's account.

**not available in 0.85.1.20-beta:** access of a second Windows account to
selected keys of the user's kageant. The kageant pipe admits only the
account that started kageant. Every other account is refused. The pipe name
also contains the user name.

### 63. Destination limits for a key

**goal:** know what kageant does with the destination of a signature.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- session-bind: klink, kscp, ksftp, kitty.exe and ssh.exe (OpenSSH 8.9 or
  newer) send kageant the server's host key with a signature by the
  server. kageant learns the destination of each signature from it.
- destination rules (`ssh-add -h`, restrict-destination-v00@openssh.com, or
  kageant's key details > Restrict...): the servers a key may sign a login
  to.
- agent log: kageant's record of key uses, refusals, adds, removes and
  locks. `dest`: the destination field. `fwd`: the field that marks a
  forwarded agent connection.

**command:**

```text
nothing to switch on for the record. Rules for a key: block 50.
```

**expect:** kageant verifies the server's session-bind signature. It shows
the destination in the confirmation (`Destination:`). The agent log records
`dest` and `fwd`. Event 102 records each binding. A key with destination
rules signs a login to those servers only and is left out of the key list
of a connection bound elsewhere. A key without rules behaves as before.

**failure:** a restricted key on a connection that is not bound (PuTTY,
ssh.exe older than OpenSSH 8.9): listed, every signature refused (event 104,
`unbound`). A rule file that cannot be read: every key refused (event 104,
`store-damaged`) until the human saves rules again in kageant.

**never:** rely on the rules for a client that sends no binding.

### 64. Limits on keys added by programs

**goal:** keys a program adds stay limited.

**inputs:**

- `<keyfile>`: the path of the private key file the program adds, e.g.
  `C:\work\deploy_job_key`.
- `<seconds>`: how long the key stays in kageant, in seconds, e.g. `3600`.
- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- IPC policy: the kageant settings for requests from other programs
  (`lockdownmode`, `blockipcadd`, `blockipcremove`, `allowipclock`).

**command:**

```text
ssh-add -t <seconds> <keyfile>   the key leaves kageant after <seconds>
ssh-add -c <keyfile>             the key needs a confirmation for each use
```

**expect:** kageant applies both. A constraint kageant does not implement is
refused, never ignored. A notice on screen identifies the program that added
the key.

**failure:** the add is refused by the IPC policy: the agent log records
`blocked` with `ipc-policy`.

**never:** allow adds over IPC (`blockipcadd=no`) when the AI agent needs
none.

### 65. IPC policy and IPC lock

**goal:** decide what programs may do with kageant.

**inputs:**

- IPC: requests that other programs send to kageant over its Windows named
  pipe.
- remove-all: `ssh-add -D`, the removal of every key in kageant.
- tray: the kageant icon in the Windows notification area. Its menu has
  "Lock agent" and "Unlock agent".

**command:**

```ini
; kageant Settings > Security, or kitty.ini:
[Agent]
; refuse every add, remove and remove-all over IPC:
lockdownmode=yes
; or one by one:
blockipcadd=yes
blockipcremove=yes
; programs may not lock or unlock kageant (ssh-add -x / -X):
allowipclock=no
```

**expect:** signing and listing always work. Add, remove and remove-all
follow the policy. Refusals are in the agent log (`blocked`, `ipc-policy`).
The tray's "Lock agent" and "Unlock agent" always work. The tray's unlock
also ends a lock set by a program.

**failure:** a program's add, remove or lock is refused: the agent log
records `blocked` with `ipc-policy`. Change the policy only when that
program needs the right.

**never:** set `allowipclock=yes` unless the AI agent must lock kageant.

### 66. The agent log and the Windows Event Log

**goal:** every key use, refusal, add, remove and lock on record.

**inputs:**

- agent log: kageant's record of key uses, refusals, adds, removes and
  locks.
- `agentlogexpiredays=90`: the expiry of the agent log, in days.
- system MSI: the system-wide KiTTY++ installer, `KiTTY-<version>-x64-system.msi`.
  `<version>` is the release version, e.g. `0.85.1.20-beta`.

**command:**

```ini
; kageant Settings > Log, or kitty.ini:
[Agent]
agentlog=yes
agentlogsink=both
agentlogexpiredays=90
```

**expect:** one line per event in the agent log file. The same records in the
Windows Event Log (KiTTY++_Agent, source KiTTY++_agent) with the system MSI.
Without it: the Windows Event Log (Application, source KiTTY++_agent). Event
IDs 100-199. The Windows account of the writer is in the Event Viewer's User
column.

**failure:** the agent log file is a local file. A program that runs as the
same Windows user can delete it. Forward the Windows Event Log to another
system for evidence: docs/WINDOWS-EVENT-LOG.md, section 6 "Forwarding",
covers a domain and a workgroup setup.

### 67. A dedicated remote account

**goal:** the AI agent logs in to an account that holds only what it needs.

**inputs:**

- `aiagent`: the example name of the remote account. Any name works. Use the
  same name in every line.

**command:**

```sh
# on each target server (Linux, outside KiTTY++), as root:
useradd --create-home --shell /bin/bash aiagent
install -d -m 700 -o aiagent -g aiagent /home/aiagent/.ssh
# put the AI agent's public key in /home/aiagent/.ssh/authorized_keys (mode 600)
```

**expect:** the AI agent logs in as `aiagent`. Its files and rights are
separate from every human account.

**failure:** the AI agent's login fails with `Permission denied (publickey)`:
check that the public key is in `/home/aiagent/.ssh/authorized_keys` with
mode 600.

**never:** let the AI agent log in as root or as a human's account.

### 68. authorized_keys options

**goal:** limit what the AI agent's key can do on the server.

**inputs:**

- `<agent-ip>`: the IP address the AI agent connects from, e.g.
  `192.0.2.50`.
- `<command>`: the one command the key may run, e.g.
  `/usr/local/bin/backup.sh`.
- `AAAA...`: the base64 part of the AI agent's public key, from its `.pub`
  file. `aiagent`, `aiagent-job`: the key comments.

**command:**

```text
# one of these forms: the first for a job of several commands, the second
# for a key that serves one fixed job (one script). Use a key of its own for it.
restrict,from="<agent-ip>" ssh-ed25519 AAAA... aiagent
restrict,from="<agent-ip>",command="<command>" ssh-ed25519 AAAA... aiagent-job
```

**expect:** `restrict` turns off port forwarding, agent forwarding, X11
forwarding and the pty. `from=` accepts the key only from `<agent-ip>`.
`command=` runs only `<command>`. The client's command is in
`SSH_ORIGINAL_COMMAND`.

**failure:** the AI agent needs a pty or a forwarding: add only that one
option back after `restrict` (`pty`, `port-forwarding`).

**never:** deploy the key without `restrict`.

### 69. sudo rules for the AI agent

**goal:** root rights only for the commands the AI agent needs.

**inputs:**

- `<service>`: the systemd service the AI agent may restart and read, e.g.
  `myapp.service`.
- `aiagent`: the remote account of the AI agent.

**command:**

```text
# /etc/sudoers.d/aiagent (edit with: visudo -f /etc/sudoers.d/aiagent)
aiagent ALL=(root) NOPASSWD: /usr/bin/systemctl restart <service>
aiagent ALL=(root) NOPASSWD: /usr/bin/journalctl -u <service> --no-pager
```

**expect:** `sudo -l` as `aiagent` lists exactly these commands.

**failure:** a command with a free argument or a shell escape (an editor, a
pager, `find -exec`) gives full root. Remove it.

**never:** `aiagent ALL=(ALL) NOPASSWD: ALL`. A rule for a script the AI agent
can write.

### 70. kitty.ini for an AI agent workspace

**goal:** logs on, host keys checked, kageant limited.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- authoritative store: the settings store KiTTY++ reads at every start.
- `savemode=file`, `savemode=dir`: kitty.ini is the authoritative store of
  the settings. With `savemode=dir` the sessions and host keys are files
  too. `savemode=file` keeps them in the registry.
- variant 1: for an AI agent that only uses keys the human loaded.
  Variant 2: for a job with blocks 35 and 36 (add and remove a key).

**command:**

```ini
; The [Agent] keys rule only when kitty.ini is the authoritative store
; (portable copy, savemode=file or savemode=dir). In a registry install they
; are first-run defaults only: set the same values in kageant Settings.

[KiTTY]
clieventlog=yes
sessioneventlog=yes
autostoresshkey=no

[Agent]
agentlog=yes
agentlogsink=both
agentlogexpiredays=90
messageonkeyusage=yes
askconfirmation=yes
helloconfirm=yes
lockwithwindows=keep
allowipclock=no

; variant 1: the AI agent may only sign
lockdownmode=no
blockipcadd=yes
blockipcremove=yes

; variant 2: the AI agent may add and remove keys with ssh-add -t
; lockdownmode=no
; blockipcadd=no
; blockipcremove=no
```

**expect:** every key use waits for a Windows Hello gesture. Every run of
klink, kscp and ksftp is in the Windows Event Log (KiTTY++_Session, source
KiTTY++_cli). Unknown host keys are never stored without a question,
except by a klink run with `-accept-new` (block 9).

**failure:** a registry install ignores the [Agent] keys after the first run:
set the same values in kageant Settings.

**never:** set `allowipclock=yes` unless the AI agent must lock kageant.
