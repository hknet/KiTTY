# KiTTY changelog

KiTTY is the full KiTTY feature set forward-ported onto a modern, security-patched
**PuTTY 0.84** core. Versions below are this port's own `0.84.1.x` line. For current
known limitations see [KNOWN-ISSUES.md](KNOWN-ISSUES.md); for the full feature list
see [FEATURES.md](FEATURES.md).

## 0.84.1.14-beta — 2026-06-20
- **Saved-session list:** single-clicking a session now copies its name into the
  "Saved Sessions" box, so Save/Load act on it without retyping — e.g. select
  **Default Settings** and Save to update it directly.
- **Windows file-info rebranding:** kageant's file description no longer reads
  "PuTTY SSH authentication agent" (it showed up that way in Task Manager's Startup
  list); `kitty_pterm` no longer says "PuTTY-style"; and the renamed CLI tools now
  carry their shipped names (klink/kscp/ksftp/kageant/kitty_pterm) instead of the
  old PuTTY tool names. (The version string still notes the "PuTTY 0.84 base"
  lineage and the copyright still credits Simon Tatham — deliberate attribution.)

## 0.84.1.13-beta — 2026-06-20
- **kageant — "Load keys on startup"** (opt-in, off by default): kageant remembers
  the file paths of the keys you load and re-adds them at the next login, added
  **encrypted/deferred** (passphrase only on first use). Enabling it also installs
  an autostart entry, replacing a manual kageant Startup shortcut. Only key-file
  paths are stored, never secrets.
- **Hide a session from the launcher:** new per-session option that keeps a session
  out of the `kitty -launcher` menu while leaving it in the normal session list.

## 0.84.1.12-beta — 2026-06-20
- **Fixed the blank taskbar icon:** the terminal window declared the AppUserModelID
  `SimonTatham.PuTTY`, which didn't match the installer's pinned shortcuts; it now
  declares `kappernet.KiTTY`, so the taskbar button shows the proper KiTTY icon.

## 0.84.1.11-beta — 2026-06-20
- **Reconnect restores the window icon** (it used to stay on the broken-connection
  icon after a successful reconnect).
- **Session comments show for pre-existing sessions:** the read-only comment box now
  reads "Comment" across all registry hives, so comments authored by an older KiTTY
  appear without needing to re-save the session.
- **Launcher About-box** character artifacts (mojibake) fixed.
- **TCP keepalives default to on** for newly-created sessions.

## 0.84.1.10-beta — 2026-06-19
- **kageant — optional Windows OpenSSH agent integration** (off by default): lets
  kageant serve as the agent for the Windows `ssh.exe` (writes `~/.ssh/kageant.conf`
  + a managed `Include` block in `~/.ssh/config`, byte-safe with a one-time backup).

## 0.84.1.9-beta — 2026-06-18
- **Config dialog:** a "WinSCP executable path" field; the session **Comment** field
  is now multiline; and a read-only **"Comment of selected session"** box in the
  Session panel that follows the saved-session selection.

## 0.84.1.8-beta — 2026-06-18
- **Registry namespace consolidated** to `Software\kapper.net\KiTTY` with automatic,
  non-destructive migration from the old hive; kageant passphrase/About dialogs
  rebranded.

## 0.84.1.1 – 0.84.1.7-beta — 2026-06-17/18
- **kittygen-cli**: a console (command-line) SSH key generator.
- Launcher fixes (saved-session list reads KiTTY's own hive; new sessions take
  focus).
- Suite-wide KiTTY rebranding (icons, window/About text, file metadata); the GUI key
  generator defaults to EdDSA/Ed25519.

## 0.84.1.0 — 2026-06-16 — first stable of the 0.84 port
- **Post-quantum key-exchange warning** (OpenSSH-style; on by default): warns at
  connection time when the SSH key exchange is not post-quantum-secure.
- The complete KiTTY feature set forward-ported onto PuTTY 0.84 (≈1,200 upstream
  commits newer than KiTTY's original 0.76b base).
