# SSH certificates with KiTTY

An SSH certificate is a public key plus a statement, signed by a **certification
authority (CA)**, about what that key is allowed to be. Instead of copying every
user's public key into `~/.ssh/authorized_keys` on every server, each server is
told to trust one CA — and every key that CA has signed is accepted, until its
certificate expires.

There are two independent halves, and you can use either without the other:

| | who proves what | you configure |
|---|---|---|
| **User certificates** | your key is vouched for by a CA, so the server accepts it without knowing it | your key gets a certificate; the server trusts the user CA |
| **Host certificates** | the server's host key is vouched for by a CA, so you never see "the host key is not cached" again | the server presents a certificate; your client trusts the host CA |

KiTTY supports both, inherited from PuTTY and unchanged in behaviour. This page
covers how to do it from KiTTY, and the OpenSSH server side that makes it useful.
Everything here was run end to end against a real `sshd` before it was written.

## Coming from an older KiTTY?

**Then you remember this as missing, and you were right.** Classic KiTTY is built
on a PuTTY that predates certificate support, so it has neither the certificate
items in the key generator nor the certificate fields in the configuration box.
That is what the long-standing *"DetachedCertificate supported in PuTTY but not
in KiTTY"* report is about.

This port is built on PuTTY 0.84, so all of it is here — including the detached
form that report names: *Connection → SSH → Auth → Credentials* → **"Certificate
to use with the private key (optional)"**. A saved session stores it under the
keyword `DetachedCertificate`, the same keyword PuTTY uses, so a session migrated
from PuTTY brings its certificate setting with it.

⚠️ One thing does **not** travel automatically: **host CA records** are kept per
user rather than per session (see the note below), in KiTTY's own registry area.
After moving from PuTTY you may have to add trusted host CAs again under
*Connection → SSH → Host keys → Configure host CAs*. User certificates are
unaffected — those live in the key file or in the session.

## What KiTTY gives you

| Where | What |
|---|---|
| **kittygen** (GUI) | `Key` menu → **Add certificate to key** / **Remove certificate from key** — folds a certificate into the `.ppk`, so the key file carries it |
| **kittygen-cli** | `--certificate <file>`, `--remove-certificate`, `-O cert-info`, and the `sha256-cert` / `md5-cert` fingerprint types |
| **Config box** | *Connection → SSH → Auth → Credentials* → **"Certificate to use with the private key (optional)"** — a certificate kept as a separate file, alongside an ordinary `.ppk` |
| **Config box** | *Connection → SSH → Host keys* → **"Configure host CAs"** — the CAs you trust to certify **host** keys |
| **Command line** | `-i <key.ppk> -cert <certificate>` on `kitty.exe`, `klink.exe`, `kscp.exe`, `ksftp.exe` |

Host CA records are stored per user, not per session: under
`HKCU\Software\kapper.net\KiTTY\SshHostCAs`, or in an `SshHostCAs\` folder under
the config directory in [portable mode](KITTY-PORTABLE.md).

**Two ways to carry a user certificate — pick one:**

- **Inside the `.ppk`** (kittygen *Add certificate to key*). One file to move
  around, and every session that uses the key gets the certificate automatically.
  When the certificate expires you re-import a new one into the key file.
- **As a detached file** (*Certificate to use with the private key*, or `-cert`).
  The `.ppk` stays untouched, so a renewed certificate is a file drop with no key
  handling at all. Better when certificates are short-lived.

## Certifying your key

A CA signs the **public** half. Your private key never leaves your machine, and
the CA never needs it.

### From the kittygen GUI

1. **Generate** a key as usual (or load an existing `.ppk`).
2. Get the public half to whoever runs the CA:
   - the big **"Public key for pasting into OpenSSH authorized_keys file"** box is
     already in the one-line OpenSSH format a CA expects — copy it out; or
   - **Save public key**, which writes the RFC 4716 block format. Convert it with
     `ssh-keygen -i -f saved.pub > key.pub` if the CA wants the one-liner.
3. They sign it and send back a `*-cert.pub` file.
4. **Key → Add certificate to key**, choose that file. The key type shown changes
   to e.g. `ssh-ed25519-cert-v01@openssh.com`.
5. **Save private key** — the `.ppk` now carries the certificate.

`Key → Remove certificate from key` reverses step 4.

### From kittygen-cli

```
:: 1. generate a key (or use an existing .ppk)
kittygen-cli -t ed25519 -o mykey.ppk

:: 2. export the public half in OpenSSH one-line format - this is what gets signed
kittygen-cli -L mykey.ppk -o mykey.pub

:: 3. ... the CA signs mykey.pub and returns mykey-cert.pub ...

:: 4. fold the certificate into the key file
kittygen-cli --certificate mykey-cert.pub -o mykey-cert.ppk mykey.ppk

:: check what you got
kittygen-cli -l mykey-cert.ppk
ssh-ed25519-cert-v01@openssh.com 255 SHA256:x4DVPaC5Y7XgtwPL7JMp4tKp9PZC/3k8/nz3dUmvgYA

:: everything the certificate asserts
kittygen-cli -O cert-info mykey-cert.ppk
```

`--remove-certificate` produces a plain key file again.

### If you run the CA yourself

The CA is just an SSH key kept somewhere safe. With OpenSSH:

```
ssh-keygen -t ed25519 -f user_ca -C "user CA"      # once, guard the private half
ssh-keygen -s user_ca -I alice -n alice,deploy -V +52w alice.pub
```

- `-I` is the identity shown in server logs — put something traceable there.
- `-n` lists the **principals**: the names this certificate may log in as. This is
  the part that replaces `authorized_keys`.
- `-V` is the validity window. Certificates that never expire throw away most of
  the benefit; `+52w` for people, hours or days for automation.

## The server side (OpenSSH `sshd`)

Tell the server to trust the CA, and which principals map to which account:

```
# /etc/ssh/sshd_config
TrustedUserCAKeys        /etc/ssh/user_ca.pub
AuthorizedPrincipalsFile /etc/ssh/principals/%u

# optional, and the real test: with this, ONLY certificates get in
AuthorizedKeysFile       none
```

`/etc/ssh/user_ca.pub` is the **public** half of the CA key. Then, for the account
`alice`, `/etc/ssh/principals/alice` lists the principal names accepted for it:

```
alice
deploy
```

A certificate is accepted when the CA signed it, it has not expired, and at least
one of its principals appears in that file. Reload `sshd` afterwards
(`sudo systemctl reload ssh`).

Onboarding is then "the CA signs their key", and offboarding is "stop signing" —
no file is edited on any server. For revoking a certificate before it expires, see
`RevokedKeys` in `sshd_config(5)`.

## Host certificates — no more "host key is not cached"

Sign the server's host key with a **host** CA (note `-h`):

```
ssh-keygen -t ed25519 -f host_ca -C "host CA"
ssh-keygen -s host_ca -I web01 -h -n web01.example.com,10.0.0.5 -V +52w \
           /etc/ssh/ssh_host_ed25519_key.pub
```

```
# /etc/ssh/sshd_config
HostKey         /etc/ssh/ssh_host_ed25519_key
HostCertificate /etc/ssh/ssh_host_ed25519_key-cert.pub
```

Then trust that CA once, on the client side:

- **In KiTTY:** *Connection → SSH → Host keys* → **Configure host CAs**. Give the
  record a **Name for this CA (shown in log messages)**, paste the CA's public key
  into **Public key of certification authority** (or *Read from file*), and set
  **Valid hosts this key is trusted to certify** — a wildcard expression such as
  `*.example.com`, so one CA cannot vouch for hosts it has no business vouching
  for. **Save**, then **Done**.
- **In OpenSSH:** one line in `known_hosts`:
  ```
  @cert-authority *.example.com ssh-ed25519 AAAAC3Nz...   host CA
  ```

New and rebuilt servers are then trusted from the first connection, with no
fingerprint prompt and nothing to clear afterwards.

## Try it locally first

If you want to see the whole thing work before touching a production server, a
throwaway `sshd` on a spare port is enough. This is exactly the lab this page was
verified against (run it on a Linux box or in WSL):

```bash
mkdir -p /tmp/certlab/principals && cd /tmp/certlab

ssh-keygen -q -t ed25519 -N '' -C user-ca -f user_ca
ssh-keygen -q -t ed25519 -N '' -C host-ca -f host_ca
ssh-keygen -q -t ed25519 -N '' -C lab-host -f ssh_host_ed25519_key
ssh-keygen -s host_ca -I lab-host -h -n localhost,127.0.0.1 -V +1d \
           ssh_host_ed25519_key.pub
echo kittylab > principals/$USER
chmod 600 user_ca host_ca ssh_host_ed25519_key

cat > sshd_config <<EOF
Port 2222
ListenAddress 127.0.0.1
HostKey /tmp/certlab/ssh_host_ed25519_key
HostCertificate /tmp/certlab/ssh_host_ed25519_key-cert.pub
TrustedUserCAKeys /tmp/certlab/user_ca.pub
AuthorizedPrincipalsFile /tmp/certlab/principals/%u
AuthorizedKeysFile none
PasswordAuthentication no
UsePAM no
StrictModes no
PidFile /tmp/certlab/sshd.pid
EOF

/usr/sbin/sshd -f /tmp/certlab/sshd_config -E /tmp/certlab/sshd.log
```

A non-root `sshd` can only accept logins for the user that started it, which is
all this needs. Now sign a KiTTY key with `user_ca` for the principal `kittylab`,
attach the certificate, and connect:

```
klink -ssh -P 2222 -i mykey-cert.ppk you@127.0.0.1
```

Two checks worth doing, because they prove the certificate is what is being
accepted and not something else:

- connect with the **same key without its certificate** — it must be refused
  (`AuthorizedKeysFile none` leaves nothing else to fall back on);
- sign a certificate for a **different principal**, or with an expiry in the past
  — both must be refused.

Stop the lab with `kill $(cat /tmp/certlab/sshd.pid)` and delete `/tmp/certlab`.

## When it does not work

| What you see | What it usually means |
|---|---|
| `Server refused our key` | the certificate never reached the server: the `.ppk` has no certificate attached, or the detached-certificate field points at the wrong file |
| `Permission denied (publickey)` with the certificate loaded | principal mismatch (the name in `-n` is not in `AuthorizedPrincipalsFile`), the certificate has expired, or the server does not trust that CA |
| Prompt for the host key even though a host certificate is configured | the host CA record's **Valid hosts** expression does not match the name you are connecting to — connecting by IP when the pattern lists a hostname is the common case |
| Certificate seems ignored | it does not match the key: a certificate is bound to one public key, so re-signing after regenerating a key is mandatory |

Useful checks: `kittygen-cli -O cert-info key.ppk` on the client, and
`ssh-keygen -L -f something-cert.pub` on any certificate file. On the server,
`LogLevel VERBOSE` in `sshd_config` logs the certificate identity (`-I`) and the
principal that was matched.

Clock skew matters: a certificate that is valid "from now" is not yet valid on a
server whose clock is a minute behind.
