---
title: KiTTY Window Title Placeholders
---

# KiTTY Window Title Placeholders

KiTTY can expand dynamic placeholders in the **Window Title** setting so that the
title reflects the active connection. To use a placeholder, type the two-character
sequence shown below in the **Window Title** field of a saved session.

## Supported placeholders

| Placeholder | Value shown in the window title |
|-------------|---------------------------------|
| `%%h` | Hostname (falls back to the configured host if no hostname is available) |
| `%%s` | Saved session name |
| `%%u` | Username configured for the session |
| `%%p` | Port number |
| `%%P` | Protocol display name (e.g. `SSH`) |
| `%%f` | Folder name the saved session belongs to |
| `%%l` | List of forwarded local ports (blank if none are configured) |
| `%%d` | List of forwarded dynamic (SOCKS) ports (blank if none are configured) |

Any other `%%X` sequence is treated as a literal `%X`.

## Example

Setting the window title to:

```text
%%h - %%s
```

produces a title like:

```text
oakleaf.csup.uk - my-session
```

A longer example that shows all available values:

```text
host=%%h | sess=%%s | user=%%u | port=%%p | proto=%%P | folder=%%f | local=%%l | dynamic=%%d
```

## Notes

* Placeholders are evaluated each time the window title is set up, including when
  the title is restored by the **Protect** menu.
* `%%s` is populated only when a saved session is loaded. If a host is entered
  directly on the command line without loading a saved session, the session name
  placeholder will be blank.
