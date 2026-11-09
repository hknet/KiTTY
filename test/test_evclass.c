/*
 * test_evclass.c - unit test for the class of a terminal Event Log line
 * (kitty/kitty_evclass.c): NOTABLE lines match the table, routine KiTTY++
 * lines and upstream PuTTY's lines do not. Also the glob itself.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_evclass.h"

static int failures = 0;

static void expect(const char *line, bool want)
{
    bool got = kitty_evclass_is_notable(line);
    if (got != want) {
        printf("FAIL  %s: \"%s\"\n", want ? "not NOTABLE" : "NOTABLE", line);
        failures++;
    }
}

int main(void)
{
    static const char *const notable[] = {
        "Reconnect: connection refused (Network error: Connection refused) - stopped",
        "Host key replaced (proven by the server): SHA256:abc, was SHA256:def, web01 port 22",
        "Blocked by Windows policy: the zone does not allow downloads",
        "Run clipboard command: not started, 33000 characters (Windows starts at most 32767).",
        "Start FileZilla: no temporary site entry could be written. FileZilla gets the address only and prompts for the password.",
        "File transfer 3: refused \"x.tar\" (Permission denied)",
        "Clipboard read refused (window not focused); 4 further requests also refused",
        "Unable to open connection to\nweb01\nNetwork error: Connection timed out",
        "Remote clipboard write ignored: the window does not have focus",
        "Host keys not updated for web01 port 22: the limit \"Keys per type and host\" (2) blocks the change of the ssh-ed25519 keys.",
        "Host key not stored (updatehostkeys=ask, not answered): SHA256:abc, web01 port 22",
        "Host key kept (updatehostkeys=ask, not answered): SHA256:abc, web01 port 22",
        "Bell: the sound file C:\\snd\\gone.wav does not play, the bell uses the default sound for this session",
        "OSC 8 folder link: no program for folders is installed (WinSCP or FileZilla), the path is on the clipboard: /srv/www",
        "Remote clipboard write ignored: more than the permitted number in one second; 3 further writes also ignored",
    };
    static const char *const routine[] = {
        /* upstream PuTTY */
        "Looking up host \"web01\" for SSH connection",
        "Connecting to 192.0.2.1 port 22",
        "Initialised AES-256 SDCTR (AES-NI accelerated) outbound encryption",
        "Host key fingerprint is:",
        "ssh-ed25519 255 SHA256:abcdef",
        "Access granted",
        /* routine KiTTY++ lines */
        "Reconnect: connection refused (Network error) - waiting, attempt 2, next in 5 s",
        "Reconnect: the agent's key is usable again - reconnecting",
        "Unable to connect, trying to reconnect...",
        "Woken up from suspend, trying to reconnect...",
        "Host key stored (accept-new): SHA256:abc, web01 port 22",
        "Host key stored (proven by the server): SHA256:abc, web01 port 22",
        "File transfer 3: received x.tar (1024 bytes)",
        "File transfer 3: the host wants to send files to C:\\dl",
        "Clipboard sent to the server on request (12 characters; 1 read served in this window)",
        "Clipboard reads set to Deny for this host, and saved in the session",
        "Remote clipboard writes blocked for this session at the user's request",
        "broadcast accepted (12 bytes), typing it into this session",
        "broadcast refused: disabled on this install ([KiTTY] sendcmdmode=no in kitty.ini)",
        "broadcast refused: from another KiTTY++ install (group 'a', ours is 'b')",
        "Key exchange 'curve25519-sha256' is not post-quantum secure; session may be vulnerable to 'harvest now, decrypt later' attacks",
        "Host key not stored (readonly=yes): SHA256:abc, web01 port 22",
        "Not saved: the settings store is read-only (readonly=yes in C:\\kitty\\kitty.ini).",
        "File transfer 3: refused by the user",
        "Host key not stored (declined): SHA256:abc, web01 port 22",
        "File transfer 3: read request refused by the user",
        "File transfer 3: no destination folder chosen, refused",
        "Remote clipboard write ignored: empty payload",
        "script stopped",
        "Could not be started: notepad.exe: The system cannot find the file specified.",
        "OSC 8 file link: web01 is the session's server (hostname).",
        "Program status: vim: running",
        "",
    };
    size_t i;

    for (i = 0; i < sizeof(notable) / sizeof(notable[0]); i++)
        expect(notable[i], true);
    for (i = 0; i < sizeof(routine) / sizeof(routine[0]); i++)
        expect(routine[i], false);

    /* the glob: '*' takes any run, also none; the whole text must match */
    if (!kitty_evclass_match("a*c", "ac") || !kitty_evclass_match("a*c", "abbbc") ||
        kitty_evclass_match("a*c", "abcd") || !kitty_evclass_match("*", "") ||
        !kitty_evclass_match("a**b*", "ab") || kitty_evclass_match("abc", "ab") ||
        !kitty_evclass_match("*x*y", "xxxxy") || kitty_evclass_match("", "a")) {
        printf("FAIL  the glob\n");
        failures++;
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS  %zu NOTABLE and %zu routine lines classed, the glob\n",
           sizeof(notable) / sizeof(notable[0]),
           sizeof(routine) / sizeof(routine[0]));
    return 0;
}
