:: Example KiTTY logon script (RuTTY engine).
::
:: Configure it under Configuration > Session > Scripting ("Script file",
:: with "Run the script on connect"), or run it on demand from a live session
:: via the system menu > Tools > Send recorded script.
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
:: The three commands below each run after a "$" prompt:

uname -a
df -h
uptime

:: Example of a per-line condition: wait for "password:" before sending.
:: (Remove the "::" from the next two lines to try it.)
:::password:
::secret123
