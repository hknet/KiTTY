valid for 0.85.1.20-beta

# KiTTY++ for AI agentic use cases

This page is for the human in control: how to hand KiTTY++ to an AI agent,
how to write its job and how to keep the SSH keys, the host keys and the
records in human hands. The AI agent works from a page of its own,
[`AGENTS-GUIDE.md`](AGENTS-GUIDE.md): one block per task. Point the AI
agent at that page only ([block 1](#1-point-the-ai-agent-at-its-guide)), never at this one. The setup fits any
agent program and any model.

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

- [Overview](#overview)
- [A. Hand the guide and a job to the AI agent](#a-hand-the-guide-and-a-job-to-the-ai-agent)
  - [block 1: Point the AI agent at its guide](#1-point-the-ai-agent-at-its-guide)
  - [block 2: Write the job](#2-write-the-job)
- [B. Keep the SSH keys under your control](#b-keep-the-ssh-keys-under-your-control)
  - [block 3: Create a key for the AI agent](#3-create-a-key-for-the-ai-agent)
  - [block 4: Keys protected by Windows Hello](#4-keys-protected-by-windows-hello)
  - [block 5: Confirm each use of a key](#5-confirm-each-use-of-a-key)
  - [block 6: Restrict a key to the AI agent's hosts](#6-restrict-a-key-to-the-ai-agents-hosts)
  - [block 7: Limits on keys added by programs](#7-limits-on-keys-added-by-programs)
  - [block 8: IPC policy and IPC lock](#8-ipc-policy-and-ipc-lock)
  - [block 9: Keep jobs running while Windows is locked](#9-keep-jobs-running-while-windows-is-locked)
  - [block 10: Register kageant as the Windows OpenSSH agent](#10-register-kageant-as-the-windows-openssh-agent)
- [C. Host keys](#c-host-keys)
  - [block 11: Host keys: set up the three layers](#11-host-keys-set-up-the-three-layers)
- [D. Records](#d-records)
  - [block 12: Agent log on](#12-agent-log-on)
  - [block 13: Records of klink, kscp, ksftp](#13-records-of-klink-kscp-ksftp)
  - [block 14: Session logs](#14-session-logs)
  - [block 15: Session-bind with destination](#15-session-bind-with-destination)
- [E. The servers](#e-the-servers)
  - [block 16: A dedicated remote account](#16-a-dedicated-remote-account)
  - [block 17: authorized_keys options](#17-authorized_keys-options)
  - [block 18: sudo rules for the AI agent](#18-sudo-rules-for-the-ai-agent)
- [F. One kitty.ini for the workspace](#f-one-kittyini-for-the-workspace)
  - [block 19: kitty.ini for an AI agent workspace](#19-kittyini-for-an-ai-agent-workspace)

## Overview

The AI agent never holds a private key or a password. kageant holds the
keys and signs each login on your rules: which key, for which hosts, with
which confirmation. You decide which host keys count as trusted. The
records of kageant and of klink, kscp and ksftp show afterwards what the AI
agent did.

KiTTY++ covers the authentication. The rights of the AI agent on the remote
systems are set on those systems. The server steps of this page assume
Linux servers with OpenSSH.

Set up sections B to F once, before the first job:

- B. Keep the SSH keys under your control: [blocks 3-10](#3-create-a-key-for-the-ai-agent).
- C. Host keys: [block 11](#11-host-keys-set-up-the-three-layers).
- D. Records: [blocks 12-15](#12-agent-log-on).
- E. The servers: [blocks 16-18](#16-a-dedicated-remote-account).
- F. One kitty.ini for the workspace: [block 19](#19-kittyini-for-an-ai-agent-workspace).

Then hand over each job with section A: blocks [1](#1-point-the-ai-agent-at-its-guide) and [2](#2-write-the-job).

## A. Hand the guide and a job to the AI agent

### 1. Point the AI agent at its guide

**goal:** the AI agent reads `AGENTS-GUIDE.md` and works by its blocks,
whatever agent program and model run it.

**inputs:**

- `<guide>`: where the AI agent reads its guide. The web address
  `https://raw.githubusercontent.com/hknet/KiTTY/kitty-0.85/docs/AGENTS-GUIDE.md`
  (plain text), or a local copy, e.g. `C:\work\AGENTS-GUIDE.md`. A local
  copy works without web access. Its first line holds the release it is
  valid for.
- instruction file: the file of standing instructions an agent program
  reads at each start. Its name and place depend on the agent program.
  Without one: the top of each task message.
- `<job>`: the job text of [block 2](#2-write-the-job).

**command:**

```text
# put these lines into the agent's instruction file, or at the top of the task message
SSH, file transfer and SSH keys on this computer go through KiTTY++
(klink, kscp, ksftp, kageant). Before the first such step, read <guide>
in full. Work by its blocks: run the
command of a block as written, compare the result with its "expect" and
"failure" lines, keep to its "never" lines.
You never get a private key, a passphrase or a password. kageant signs the
logins.
The job below sets the hosts, the accounts, the host-key layer of each host
and what you may do. Stop and report to me at each stop point of the guide
and at every step the job does not cover. Never work around a refusal.
Before the first command, reply with the hosts, the host-key layer of each
and the blocks you plan to use. Then wait for my go.

<job>
```

**expect:** the AI agent's first reply lists the hosts of the job, the
host-key layer of each and the blocks it plans to use. Compare the list
with the job. Then give the go.

**failure:** the reply lists a host, a layer or a block the job does not
allow: correct the job or the AI agent before the go. The AI agent cannot
read `<guide>`: give it a local copy.

**never:** put a private key, a passphrase or a password into the
instruction file, the job or a message. Give the AI agent a job without a
host list.

### 2. Write the job

**goal:** every decision the AI agent may need is made before the first
run.

**inputs:**

- `<user>`, `<host>`, `<port>`: each server of the job with its login
  account and SSH port, e.g. `aiagent`, `web01.example.com`, `22`.
- `<hostgroup>`: the hosts the AI agent may run with `-accept-new`, e.g.
  `*.lab.example.com`. A rule the AI agent keeps itself: KiTTY++ has no host
  filter for `-accept-new`.
- `<fp>`: a SHA256 fingerprint of a host key, read on the server itself
  ([block 11](#11-host-keys-set-up-the-three-layers), layer 3). Form: `SHA256:` plus 43 base64 characters.
- layer: how a host key becomes trusted. 1: a host CA signed it ([block 11](#11-host-keys-set-up-the-three-layers)).
  2: accept-new for a host group. 3: a fingerprint you confirmed.

**command:**

```text
# the job: fill in every line, delete the lines the job does not need
Job: <what the job is for, one line>
Hosts:
  <user>@<host>:<port>
Host keys, one line per host or group:
  CA for <host>
  accept-new for <hostgroup>
  pin <host> <fp>
Allowed: <the tasks, e.g. run commands, upload to /srv/data/, download from /var/log/>
Not allowed: <e.g. sudo, deleting files>
kageant: sign only
  (or: add and remove keys with ssh-add -t, only with the IPC policy of block 8)
Stop and report: a changed host key, a new host key outside the accept-new
  lines, a refusal by kageant, an exit code the block does not expect, a step
  outside "Allowed".
Report: each command with its exit code, every "Host key stored" and
  "Host key not stored" line, every stop.
```

**expect:** the AI agent's reply of [block 1](#1-point-the-ai-agent-at-its-guide) matches the job line by line.

**failure:** the AI agent requests a value the job does not hold: add the
line to the job. Never answer with a key, a passphrase or a password.

**never:** `accept-new for *`. Pin a fingerprint the AI agent reports
without reading it on the server itself ([block 11](#11-host-keys-set-up-the-three-layers)).

## B. Keep the SSH keys under your control

The AI agent gets signatures, never keys. Each control below closes one way
around that. Set up what the job needs, in this order:

1. A key of its own for the AI agent ([block 3](#3-create-a-key-for-the-ai-agent)), protected by Windows Hello
   ([block 4](#4-keys-protected-by-windows-hello)).
2. A confirmation for each use of the key ([block 5](#5-confirm-each-use-of-a-key)). A Yes/No box can be
   clicked by any program that runs as the same Windows user. A Windows
   Hello gesture cannot.
3. The key signs logins to the job's hosts only ([block 6](#6-restrict-a-key-to-the-ai-agents-hosts)).
4. Limits on keys that programs add ([block 7](#7-limits-on-keys-added-by-programs)). The IPC policy ([block 8](#8-ipc-policy-and-ipc-lock)).
5. Two settings for the AI agent's tools: jobs while Windows is locked
   ([block 9](#9-keep-jobs-running-while-windows-is-locked)), ssh.exe and git with kageant ([block 10](#10-register-kageant-as-the-windows-openssh-agent)).

### 3. Create a key for the AI agent

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
public half in OpenSSH's one-line form for `authorized_keys`, [block 17](#17-authorized_keys-options)). Protect
the `.ppk` with Windows Hello next ([block 4](#4-keys-protected-by-windows-hello)).

**failure:** kittygen-cli ends with an error: the reason is on standard
error.

**never:** reuse a human's key for the AI agent. Give the AI agent the
`.ppk` of a key it does not need.

### 4. Keys protected by Windows Hello

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

### 5. Confirm each use of a key

**goal:** the human approves each signature of one key or of every key.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- `askconfirmation=auto`: a key needs a confirmation when its comment
  contains `confirmation`. `askconfirmation=yes`: every key needs one.
- Windows Hello gesture: a PIN, a fingerprint or a face check of Windows
  Hello.

**command:**

```text
# one of these forms
one key: kageant key list > the key > key details > "Confirm each use:"
  the Yes/No entry:          a Yes/No box per signature
  the Windows Hello entry:   a Windows Hello gesture per signature,
                             also with askconfirmation=no
every confirmation by Windows Hello: kageant Settings >
  "Confirmations require Windows Hello" (kitty.ini [Agent] helloconfirm=yes)
```

```ini
; every key, or one key by its comment:
[Agent]
askconfirmation=yes
; (or "confirmation" in the key comment with askconfirmation=auto)
; Windows Hello for every confirmation:
helloconfirm=yes
```

**expect:** each signature waits for the human. The confirmation shows the
key. For klink, kscp, ksftp, kitty.exe and Windows ssh.exe (OpenSSH 8.9 or
newer) it shows the destination too. With the Windows Hello entry or
`helloconfirm` the prompt is a Windows Hello gesture.
A Yes/No box can be clicked by any program that runs as the same Windows
user. A Windows Hello prompt cannot.

**failure:** Windows Hello cannot run: the use is refused, never downgraded
to a click. The AI agent's login fails with `Permission denied (publickey)`.
The AI agent's klink or ssh.exe waits until the human answers: plan for the
wait.

**never:** leave a key the AI agent uses on "no" when the AI agent runs as
the same Windows user.

### 6. Restrict a key to the AI agent's hosts

**goal:** the AI agent's key signs a login to the job's servers only.

**inputs:**

- `<keyfile>`: the AI agent's private key file ([block 3](#3-create-a-key-for-the-ai-agent)).
- `<host>`: a server of the job, e.g. `web01`. Its host key must be known:
  in the KiTTY++ host-key store, signed by a host CA of the CA store
  ([block 11](#11-host-keys-set-up-the-three-layers)), or in known_hosts when kageant reads that file (Settings > Agent >
  "Use OpenSSH's known_hosts as a host-key source", default off).
- `<port>`: the SSH port of `<host>` when it is not 22, e.g. `2222`.
- `<jump>`: a jump host the job goes through, e.g. `jump01`.
- kageant key details: double-click the key in kageant's key list.
- session-bind: klink, kscp, ksftp, kitty.exe and ssh.exe (OpenSSH 8.9 or
  newer) send kageant the server's host key with a signature by the
  server. kageant learns the destination of each signature from it.

**command:**

```text
# one of these forms: in kageant's window
In kageant's key list: double-click the key, Restrict..., Add destination, Host: <host>, Look up host keys, OK.
For a jump host: one destination for <jump>, one for <host> with Via host: <jump> (the key then works through an agent forwarded to <jump>).
For a group of servers: Host: *.<domain>, then Pattern: Fixed list: host keys known now, or Checked at each login: any matching host (the human decides: it trusts the host-key store, and known_hosts while kageant reads it).
For every server of a host CA: Add destination, All servers of a host CA, CA: <the CA of the CA store> (the server's certificate must hold its names).
```

```powershell
# one of these forms: kageant's command line (host keys from the KiTTY++ host-key store)
kageant -h <host> <keyfile>
kageant -h "[<host>]:<port>" <keyfile>
```

**expect:** key details lists under `Destinations:` the count on the first
line, then one line per destination: the host, then `user`, `via` and
`forwarding` where the destination has them, and below each its origin
(`Set by <program path>, <date time> (host keys looked up by that program)`
for the command line, `Set in the rule editor`). `kageant -h` exits with 0. The key list draws the key's
row in blue. Its State column ends in `, restricted`.
The rules are kept in `kageant-rules.dat` (`%APPDATA%\KiTTY`, or beside
kitty.ini in a portable copy), encrypted for the Windows account, and apply
again after a restart, whichever way the key comes back. Event 110 `add`
carries them in `restrict`. A refusal is event 104.
kageant verifies the server's session-bind signature. A key with
destination rules signs a login to those servers only and is left out of
the key list of a connection bound elsewhere. A key without rules behaves
as before.

**failure:** `No host key of <host> in the KiTTY++ host-key store or the
CA store.` (with known_hosts in the list while kageant reads it): log in to
`<host>` once yourself (or scan it, [block 7 of `AGENTS-GUIDE.md`](AGENTS-GUIDE.md#7-scan-the-servers-keys-and-compare-them-with-the-store)) to store
its key, then look up again. `kageant -h` without a stored host key:
`-h: No host key found for <host> in the KiTTY++ host-key store.`, exit
code 1, no key added. A server on a port other than 22 needs the form
`[<host>]:<port>`. The ssh-add.exe of Windows (OpenSSH for Windows 9.5)
has no `-h`: `unknown option -- h`.
A restricted key on a connection that is not bound (PuTTY, ssh.exe older
than OpenSSH 8.9): listed, every signature refused (event 104, `unbound`).
A rule file that cannot be read: every key refused (event 104,
`store-damaged`) until the human saves rules again in kageant.

**never:** restrict a key that other programs need for other hosts. Use a
key of its own for the AI agent ([block 3](#3-create-a-key-for-the-ai-agent)). A re-add with `kageant -h`
replaces the rules. A re-add without `-h` keeps them. Rely on the rules for
a client that sends no binding.

### 7. Limits on keys added by programs

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

### 8. IPC policy and IPC lock

**goal:** decide what programs may do with kageant.

**inputs:**

- IPC: requests that other programs send to kageant over its Windows named
  pipe.
- remove-all: `ssh-add -D`, the removal of every key in kageant.
- `lockdownmode`: refuse every add, remove and remove-all over IPC.
  `blockipcadd`: refuse adds. `blockipcremove`: refuse removes.
  `allowipclock`: let programs lock and unlock kageant.
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

### 9. Keep jobs running while Windows is locked

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

### 10. Register kageant as the Windows OpenSSH agent

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

## C. Host keys

### 11. Host keys: set up the three layers

**goal:** the AI agent can trust a host key without guessing.

**inputs:**

- `<ca-name>`: a name for the CA record, e.g. `lab-host-ca`.
- `<id>`: the certificate's identity, e.g. `web01`.
- `<names>`: the host names the certificate is valid for, separated by
  commas, e.g. `web01.example.com,192.0.2.10`. List every name the AI agent
  connects by.
- `<hostgroup>`: the hosts the AI agent may run with `-accept-new`, e.g.
  `*.lab.example.com`. A rule the AI agent keeps itself: KiTTY++ has no host
  filter for `-accept-new`.
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
2: every new key is in the AI agent's report and in event 311. These records
are the only check of the host group: the program stores an unknown key for
any host it gets `-accept-new` for. Layer 3: the
AI agent pins the confirmed fingerprints.

**failure:** layer 1 does not take effect: the "Valid hosts" expression does
not match the name the AI agent connects by, the certificate does not list
that name, or it expired. Connecting by an IP address fails when only host
names are listed. Layer 2: the AI agent reports a changed key. Check the
server before anything else.

**never:** a "Valid hosts" of `*`. Put `host_ca` on a server or on the AI
agent's computer. Set `autostoresshkey=yes` as a replacement for layer 2.

## D. Records

### 12. Agent log on

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
  | 141 | `certificate` | a certificate of a key expiring within 14 days, expired or not yet valid |
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
agentlogexpiredays=90
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
line without it. The Windows account of the writer is in the Event Viewer's
User column.

**failure:** no KiTTY++_Agent log: a per-user install or a portable copy. The
records are in the Windows Event Log (Application, source KiTTY++_agent). No
new record after the login: `agentlog` is off or the sink leaves out that
destination. `ssh-add -l` writes no record.
The agent log file is a local file. A program that runs as the same Windows
user can delete it. Forward the Windows Event Log to another system for
evidence: docs/WINDOWS-EVENT-LOG.md, section 6 "Forwarding", covers a domain
and a workgroup setup.

### 13. Records of klink, kscp, ksftp

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

### 14. Session logs

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
  ([block 8 of `AGENTS-GUIDE.md`](AGENTS-GUIDE.md#8-layer-1-connect-to-a-host-whose-key-a-trusted-ca-signed)): leave `-hostkey <fp>` out.
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

### 15. Session-bind with destination

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

## E. The servers

### 16. A dedicated remote account

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

### 17. authorized_keys options

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

### 18. sudo rules for the AI agent

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

## F. One kitty.ini for the workspace

### 19. kitty.ini for an AI agent workspace

**goal:** logs on, host keys checked, kageant limited.

**inputs:**

- kageant: the KiTTY++ SSH agent. It holds the user's loaded private keys
  and signs logins with them.
- authoritative store: the settings store KiTTY++ reads at every start.
- `savemode=file`, `savemode=dir`: kitty.ini is the authoritative store of
  the settings. With `savemode=dir` the sessions and host keys are files
  too. `savemode=file` keeps them in the registry.
- variant 1: for an AI agent that only uses keys the human loaded.
  Variant 2: for a job with blocks [35](AGENTS-GUIDE.md#35-add-a-key) and [36](AGENTS-GUIDE.md#36-remove-one-key-this-ai-agent-added) of `AGENTS-GUIDE.md` (add and
  remove a key).

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
except by a klink run with `-accept-new` ([block 9 of `AGENTS-GUIDE.md`](AGENTS-GUIDE.md#9-layer-2-accept-new-only-when-the-job-allows-it-for-this-host)).

**failure:** a registry install ignores the [Agent] keys after the first run:
set the same values in kageant Settings.

**never:** set `allowipclock=yes` unless the AI agent must lock kageant.
