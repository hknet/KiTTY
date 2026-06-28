# KiTTY Window Title Placeholders

KiTTY can expand dynamic placeholders in the **Window Title** setting so that the
title reflects the active connection. To use a placeholder, type the sequence
shown below in the **Window Title** field of a saved session.

## Supported placeholders

| Placeholder | Value shown in the window title |
|-------------|---------------------------------|
| `%%h` | Hostname (falls back to the configured host if no hostname is available) |
| `%%s` | Saved session name |
| `%%u` | Username configured for the session |
| `%%p` | Port number |
| `%%P` | Protocol display name (e.g. `SSH`) |
| `%%f` | Folder name the saved session belongs to |
| `%%l` | List of local forwarded ports (blank if none are configured) |
| `%%d` | List of dynamic/SOCKS forwarded ports (blank if none are configured) |

Any other `%%X` sequence is treated as a literal `%X`.

## Examples

Setting the window title to:

```text
%%h - %%s
```

produces a title like:

```text
oakleaf.example.net - my-session
```

A longer example that shows all available values:

```text
host=%%h | sess=%%s | user=%%u | port=%%p | proto=%%P | folder=%%f | local=%%l | dynamic=%%d
```

## Notes

- Placeholders are evaluated when the window title is set up and on
  reconfiguration. If the title string contains placeholders, KiTTY re-expands it
  even when only dependent settings such as host, username, port, protocol,
  folder, or port forwardings changed.
- Remote programs can still change the window title after login by sending
  terminal title-control escape sequences. This is why a placeholder title may
  appear briefly and then change to something like `user@host:~`. To keep the
  placeholder-expanded title, enable **Terminal → Features → Disable
  remote-controlled window title changing** for the session. In saved settings,
  this is `NoRemoteWinTitle=1`.
- `%%s` is populated when a saved session is loaded. If a host is entered
  directly on the command line without loading a saved session, the session name
  placeholder is blank.
