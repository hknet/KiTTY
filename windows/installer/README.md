# KiTTY MSI installers

Two MSI installers are produced from this directory, both embedding the packaged
binaries from `release-084/` (kitty.exe + the k* command-line tools; the
portable and `_nocompress` variants are intentionally **not** installed — the
portable build is the no-install story).

| File | Scope | Location | Admin / UAC |
|------|-------|----------|-------------|
| `kitty-system.wxs` → `KiTTY-…-x64-system.msi`  | per-machine (all users) | `C:\Program Files\KiTTY` | **Yes** (elevation required) |
| `kitty-peruser.wxs` → `KiTTY-…-x64-peruser.msi` | per-user (current user)  | `%LOCALAPPDATA%\Programs\KiTTY` | **No** (no UAC) |

Both add Start-Menu + Desktop shortcuts and an Add/Remove-Programs entry, and
remove everything cleanly on uninstall. They use **distinct UpgradeCodes**, so
they are independent products and can even coexist.

## Building

```powershell
dotnet tool install --global wix --version 5.0.2   # one-time
# (ensure release-084\ holds the freshly packaged binaries)
pwsh -File build.ps1 -Ver <version> -Rel <release-084 path>
# -> release-084\KiTTY-<version>-x64-{system,peruser}.msi
```

## Upgrades: what happens to running KiTTY windows

An in-place upgrade has to replace `kitty.exe` while sessions may be open, so
both packages take part in the **Restart Manager (RM)** protocol. Every KiTTY
window registers itself with `RegisterApplicationRestart` — terminals register
their own `-load "SESSION"`, so RM reopens *the session*, not a dead command
line. Config-box-spawned children (launched internally as `putty &<handle>`)
register the resolved `-load "SESSION"` too, so their expired file-mapping
handle is never replayed.

| Install mode | Files-in-Use prompt | Running windows |
|---|---|---|
| Full UI (`msiexec /i`, and the in-app updater) | yes, defaulting to *"close and restart"* | closed by RM, then reopened and reconnected |
| Silent (`/qn`) — user or winget | none; Windows always uses RM at silent UI level | closed by RM, then reopened and reconnected |
| Silent, **SYSTEM-initiated** (SCCM, Intune, a SYSTEM scheduled task) | none | closed by RM, **not** reopened |

The last row is deliberate: a SYSTEM-context deployment has no interactive
desktop to relaunch into, so the package sets `MSIDISABLERMRESTART=1` when
`UserSID` is `S-1-5-18`. Only the *restart* leg is suppressed — the shutdown
still happens, so the upgrade itself is unaffected. An elevated but
user-initiated install still reports the real user's SID and therefore still
reopens its windows.

To override either way, set the standard Windows Installer property on the
command line:

```
msiexec /i KiTTY-<ver>-x64-system.msi /qn MSIDISABLERMRESTART=1   # never reopen
```

Two consequences worth knowing:

- **kageant is restarted empty.** It registers a bare relaunch, so the agent
  comes back with no keys loaded and passphrases must be entered again. Its
  `loadonstartup` / `startupkeyN` settings reduce this.
- **The `util:CloseApplication` steps are a fallback, not the main path.** They
  are sequenced at 3999, well after `InstallValidate` (1400) where RM does its
  work, so on a healthy upgrade RM has already closed everything and they find
  nothing to terminate. They exist to keep the upgrade itself unblockable.

## GUIDs are fixed on purpose

The `UpgradeCode` and `Component` GUIDs in the `.wxs` files are **hard-coded and
committed deliberately**. They are **not secrets** — every shipped MSI exposes
them (`msiinfo` / Orca) — but they **must stay stable across releases** so
Windows recognises upgrades of a prior install. **Do not regenerate them.**

- per-machine UpgradeCode: `69EA2DD5-EF19-4811-B324-EF34CAA6942C`
- per-user UpgradeCode:    `578952A6-AA7F-4146-918B-47803234700B`
