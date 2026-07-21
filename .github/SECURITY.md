# Security Policy

KiTTY (this 0.84 line) is a fork of [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/)
maintained by KAPPER NETWORK-COMMUNICATIONS GmbH, forward-ported onto a modern,
security-patched PuTTY 0.84 core.

## Supported versions

Only the **latest published release** receives fixes. Beta builds roll (each new
beta supersedes the previous one); see the
[releases page](https://github.com/hknet/KiTTY/releases) for the current build.

## Reporting a vulnerability

Please report security issues **privately** — do not open a public issue for
anything exploitable.

- Use GitHub **Security Advisories** → *Report a vulnerability* on this repo, with
  details and, if possible, a proof of concept and the affected version (from
  *Help → About*, e.g. `0.84.1.x-beta`).

We aim to acknowledge reports within a few days. Coordinated disclosure is
appreciated; we will credit reporters who wish to be named.

## Vulnerabilities in PuTTY itself

This fork tracks PuTTY upstream. Issues in unmodified PuTTY code are best reported
to the [PuTTY team](https://www.chiark.greenend.org.uk/~sgtatham/putty/feedback.html)
as well; we forward-port their fixes when a new PuTTY base is released.

## Integrity of releases

All shipped binaries and MSIs are **Authenticode-signed** by
*KAPPER NETWORK-COMMUNICATIONS GmbH* (trusted-timestamped). Verify the signature
before running, and download only from the official
[releases page](https://github.com/hknet/KiTTY/releases). The portable ZIP ships a
`SHA256SUMS` file.

## Known security notes

See [KNOWN-ISSUES.md](../KNOWN-ISSUES.md) for current limitations. In particular,
stored auto-login passwords are saved in a **reversibly-encrypted** form and are
not a substitute for SSH public-key authentication — prefer keys (and `kageant`)
wherever possible.
