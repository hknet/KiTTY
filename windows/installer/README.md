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

```bash
sudo apt install -y wixl uuid-runtime          # one-time
# (ensure release-084/ holds the freshly packaged binaries)
bash build.sh                                   # -> release-084/KiTTY-*-x64-{system,peruser}.msi
```

## GUIDs are fixed on purpose

The `UpgradeCode` and `Component` GUIDs in the `.wxs` files are **hard-coded and
committed deliberately**. They are **not secrets** — every shipped MSI exposes
them (`msiinfo` / Orca) — but they **must stay stable across releases** so
Windows recognises upgrades of a prior install. **Do not regenerate them.**

- per-machine UpgradeCode: `69EA2DD5-EF19-4811-B324-EF34CAA6942C`
- per-user UpgradeCode:    `578952A6-AA7F-4146-918B-47803234700B`
