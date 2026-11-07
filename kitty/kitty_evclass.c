/*
 * kitty_evclass.c - which terminal Event Log lines are NOTABLE: the one
 * table the Event Log notifier reads. The class of each KiTTY++ line comes
 * from this table, never from the call site, so a line changes class by one
 * table entry. A line not matched here is routine - upstream PuTTY's lines
 * among them.
 *
 * Each pattern is the line's fixed text with '*' for each variable part
 * (kitty_evclass_match). The comment before a group states which event it
 * covers. Lines that answer a click of the user with a notice of their own
 * ("Could not be started", "Open Logfile failed") stay routine: the
 * notifier would only repeat the notice.
 */
#include <stddef.h>
#include "kitty_evclass.h"

static const char *const kitty_evclass_notable[] = {
    /* auto-reconnect waits for a locked agent key, gives up, or meets a
     * failure it does not know (KT_RC_LOG_WAIT, _STOP, _REPORT, _KEYGONE) */
    "Reconnect: * - waiting until the agent's key is usable*",
    "Reconnect: * - stopped",
    "Reconnect: this failure is not recognised*",
    /* a reconnect attempt cannot connect (window.c) */
    "Unable to open connection to*",
    /* the connection drops with auto-reconnect on */
    "Lost connection, trying to reconnect...",
    /* the login script ran out of time: the rutty script starts anyway */
    "Login script (Connection > Login) has not finished; starting *",
    /* broadcasts refused (old format, feature off, other install, session
     * does not accept them) */
    "broadcast refused: *",
    /* a script request refused or failed (rutty) */
    "script not sent: *",
    "script timeout !",
    "script file not found",
    "script file read failed",
    /* session log: rotation skipped, time stamp pattern empty */
    "Log rotation skipped: *",
    "Log timestamp: that strftime pattern produces nothing*",
    /* host keys: accept-new did not apply, readonly=yes, the update's proof
     * failed or was refused, a stored key replaced or removed by the server */
    "Host key not stored (accept-new): a key of another type *",
    "Host key not stored (readonly=yes): *",
    "Host key not stored (the server's proof failed): *",
    "Host keys not updated (the server refused the proof request): *",
    "Host key replaced (proven by the server): *",
    "Host key removed (no longer offered by the server): *",
    /* readonly=yes kept a change out of the store (host key, far2l client
     * IDs, the clipboard's "always deny") */
    "Not saved: the settings store is read-only (readonly=yes in *).",
    /* the server refused an environment variable */
    "Server refused to set environment variable * - its sshd_config needs *",
    /* the key exchange is not post-quantum */
    "Key exchange '*' is not post-quantum secure*",
    /* the key file does not match its pinned fingerprint */
    "Key file \"*\" REFUSED: fingerprint * is not the recorded *",
    /* a download check blocked, failed, timed out or lost the file */
    "Blocked by Windows policy: *",
    "Windows' check of the download failed: *",
    "Mark of the Web not written (Zone.Identifier): *",
    "The file is gone after Windows' check: *",
    "Windows' check of the download gave no answer within * seconds.",
    /* a transfer tool is missing */
    "kscp was not found. *",
    "WinSCP was not found. *",
    "FileZilla was not found. *",
    /* a hyperlink or a clicked URL refused */
    "OSC 8 link not opened: *",
    "Link not opened: *",
    /* workplace proxy mode without its proxy, a named proxy that does not
     * load */
    "workplace proxy mode: proxy \"*\" is not defined - connection NOT proxied",
    "Unable to load proxy definition",
    /* a save kept the stored secret: master password locked or never set
     * up, DPAPI failed */
    "The master password is locked: * was not saved.",
    "No master password was set up: * was not saved.",
    "Windows could not encrypt the * (DPAPI: *): * was not saved.",
    /* a session name twice in the file store: one copy unused */
    "Session \"*\": opened from *; * stands for the same name and is not used",
    /* OSC 5113 file transfers: refused, limited, failed */
    "File transfer *: refused by the user",
    "File transfer *: read request refused by the user",
    "File transfer *: no destination folder chosen, refused",
    "File transfer *: refused, another transfer is active",
    "File transfer *: dropped (command out of order)",
    "File transfer *: expired after * minutes without activity",
    "File transfer: a command over the size limit was ignored",
    "File transfer *: refused \"*\" (*)",
    "File transfer *: path \"*\" refused (*)",
    "File transfer *: * failed (*)",
    "File transfer *: could not read *",
    /* host notifications dropped by the flood rule */
    "Notifications from the host: * dropped (more than one in 2 seconds).",
    /* remote clipboard: payloads dropped, writes and reads refused or
     * limited, a grant withdrawn, "always deny" not stored */
    "* was too large and was dropped",
    "* was too large and was dropped; * further payload* also dropped",
    "Remote clipboard write ignored: *",
    "Clipboard read refused (*)",
    "Clipboard read refused (*); * further request* also refused",
    "Clipboard approval withdrawn: *",
    "Clipboard permission withdrawn: *",
    "Clipboard reads refused for the rest of this session; *",
    "Malformed OSC 5522 write request, aborting (EINVAL)",
    "Clipboard write request exceeds the size limit (* MB), aborting (EFBIG)",
    "Clipboard write request has too many MIME types or aliases, aborting (EFBIG)",
    "Clipboard write failed: could not open the clipboard (EIO)",
    "Paste event not sent: clipboard reads set to Deny, pasted as text",
    "far2l clipboard read refused: *",
    /* a folder reported by the host (OSC 7) refused */
    "OSC 7 folder refused: *",
};

bool kitty_evclass_match(const char *p, const char *t)
{
    /* iterative glob: on a mismatch after a '*', the star takes one more
     * character and the rest is tried again - no recursion */
    const char *star = NULL, *mark = NULL;

    if (!p || !t)
        return false;
    while (*t) {
        if (*p == '*') {
            star = ++p;
            mark = t;
        } else if (*p == *t) {
            p++;
            t++;
        } else if (star) {
            p = star;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (*p == '*')
        p++;
    return *p == '\0';
}

bool kitty_evclass_is_notable(const char *line)
{
    size_t i;

    if (!line || !*line)
        return false;
    for (i = 0; i < sizeof(kitty_evclass_notable) /
                    sizeof(kitty_evclass_notable[0]); i++)
        if (kitty_evclass_match(kitty_evclass_notable[i], line))
            return true;
    return false;
}
