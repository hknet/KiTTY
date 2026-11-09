:: Example script file for the KiTTY++ script engine (RuTTY's engine).
:: Not a login script: that one is set on Connection > Login, see
:: docs/SCRIPTING.md "Which one to use".
::
:: Configure it under Configuration > Session > Scripting ("Script file",
:: with "Run the script on connect"), or run it on demand from a live session
:: via the system menu > Tools > Run script file.
::
:: Format: plain text, sent line by line.
::   - "::" lines (the condition character twice) are comments and are skipped.
::   - Blank lines are skipped.
::   - With "Wait for a prompt before each line" enabled, KiTTY sends the next
::     line only after the server's output ends with the configured
::     "Wait-for text" (e.g. "$"); "Halt-on text" aborts the script and the
::     "Timeout" gives up when the prompt never appears.
::   - With "Use conditions from file" enabled, a line starting with a single
::     ":" changes the wait pattern for the NEXT line only.
::
:: The three commands below wait for a "$" prompt each only when the session
:: is set to wait: "Wait for a prompt before each line" on, "Wait-for text"
:: "$". The file itself holds no wait for them. Without that setting they go
:: out one after another on the line delay.

uname -a
df -h
uptime

:: Example of a per-line condition: wait for "password:" before sending.
:: It needs "Use conditions from file" on. (Remove the "::" from the next
:: two lines to try it.)
:::password:
::secret123
