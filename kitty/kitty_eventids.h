/*
 * kitty_eventids.h - every Windows Event Log event ID of the suite, in one
 * table: its block, the source it is reported under, the kind of record
 * (insertion string %1) and the fields that follow it (%2 onwards, in this
 * order). windows/kitty_events.mc carries one message per ID, written
 * "%1%nfield: %2%n...%0" from this table, and the manual's appendix
 * doc/eventids.but one row per ID; tools/check-kitty-events.py compares
 * both with this table, test/test_eventids.c checks the blocks.
 * docs/EVENT-IDS.md is generated from this table and kept in sync by the
 * same check (--write-appendix rewrites it).
 *
 * The blocks:
 *   0        text only (source KiTTY++): the one string is the message
 *   100-199  kageant's agent log (KiTTY++_agent, log KiTTY++_Agent)
 *   200-299  kitty.exe sessions (KiTTY++_kitty, log KiTTY++_Session)
 *   300-399  klink, kscp, ksftp (KiTTY++_cli, log KiTTY++_Session)
 *   400-499  kittygen, kittygen-cli (KiTTY++_kittygen, log KiTTY++_Session)
 *   900-999  the suite (KiTTY++, Application log, always written)
 * Without the system-wide installation a source is unknown to Windows and
 * its events land in the Application log, with the same event data.
 *
 * In 2xx and 3xx the same offset is the same kind with the same fields:
 * kitty.exe and the command-line tools share the SSH code that reports
 * them, and only the block differs. The order of an existing field list
 * never changes; a new field is appended at its end, here and in the .mc.
 *
 * No Win32 here. The table itself is compiled only where
 * KITTY_EVENTIDS_TABLE is defined (kitty_winevent.c, the unit test).
 */
#ifndef KITTY_EVENTIDS_H
#define KITTY_EVENTIDS_H

/* Sources, and the log the system-wide installation registers 2xx-4xx in */
#define KITTY_EVSRC_SUITE      "KiTTY++"
#define KITTY_EVSRC_AGENT      "KiTTY++_agent"
#define KITTY_EVSRC_KITTY      "KiTTY++_kitty"
#define KITTY_EVSRC_CLI        "KiTTY++_cli"
#define KITTY_EVSRC_KITTYGEN   "KiTTY++_kittygen"
#define KITTY_EVLOG_SESSION    "KiTTY++_Session"

/* Block bases */
#define KITTY_EVB_TEXT         0
#define KITTY_EVB_AGENT        100
#define KITTY_EVB_KITTY        200
#define KITTY_EVB_CLI          300
#define KITTY_EVB_KITTYGEN     400
#define KITTY_EVB_SUITE        900

/* Offsets in the session blocks (kitty.exe 2xx, klink/kscp/ksftp 3xx) */
#define KITTY_EVO_SESSION_OPEN      0
#define KITTY_EVO_SESSION_CLOSE     1
#define KITTY_EVO_HOSTKEY_KNOWN     10
#define KITTY_EVO_HOSTKEY_NEW       11
#define KITTY_EVO_HOSTKEY_MISMATCH  12
#define KITTY_EVO_AUTH_OK           20
#define KITTY_EVO_AUTH_FAILED       21
#define KITTY_EVO_PROXY_USED        25
#define KITTY_EVO_FAREND_REFUSED    30   /* kitty.exe only */
#define KITTY_EVO_TRANSFER_DONE     30   /* command-line tools only */
#define KITTY_EVO_TRANSFER_FAILED   31   /* command-line tools only */
#define KITTY_EVO_TERM_TRANSFER_DONE   40   /* kitty.exe only: in-terminal */
#define KITTY_EVO_TERM_TRANSFER_FAILED 41   /* kitty.exe only: in-terminal */
/* The offsets below this one are shared by 2xx and 3xx. */
#define KITTY_EVO_SHARED_END        30

/* Offsets in the kittygen block */
#define KITTY_EVO_KEY_GENERATED     0
#define KITTY_EVO_PRIVATE_SAVED     1
#define KITTY_EVO_PUBLIC_SAVED      2
#define KITTY_EVO_PASSPHRASE_CHANGED 3

/* The suite's IDs */
#define KITTY_EV_PROGRAM_REFUSED    900
#define KITTY_EV_LOG_RETENTION      901
#define KITTY_EV_MISSING_FEATURES   902
#define KITTY_EV_SESSION_SAVE_FAILED 903

struct kitty_eventid {
    unsigned id;
    unsigned block;
    const char *source;
    const char *kind;       /* %1; NULL for ID 0, whose %1 is the text */
    const char *fields;     /* %2 on, space separated */
    const char *level;      /* the level, as the manual's appendix gives it
                             * (doc/eventids.but, kitty-eventids) */
};

#ifdef KITTY_EVENTIDS_TABLE
static const struct kitty_eventid kitty_eventids[] = {
    {   0, KITTY_EVB_TEXT,  KITTY_EVSRC_SUITE, NULL, "",
      "the line's own level" },

    /* kageant: kitty/kitty_auditlog.c, audit_kinds */
    { 100, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "sign",
      "fp comment result reason req reqpath pid",
      "Information; Warning for a refusal or failure" },
    { 101, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "confirm",
      "fp comment result reason req reqpath pid",
      "Information; Warning for a refusal or failure" },
    { 110, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "add",
      "fp comment result reason req reqpath pid path",
      "Information; Warning for a refusal or failure" },
    { 111, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "loadkey", "path result enc",
      "Information; Warning for a refusal or failure" },
    { 112, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "forget-path", "path result",
      "Information; Warning for a refusal or failure" },
    { 113, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "retry",
      "trigger loaded refused absent broken",
      "Information; Warning for a refusal or failure" },
    { 114, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "remove",
      "fp comment result reason req reqpath pid",
      "Information; Warning for a refusal or failure" },
    { 115, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "remove-all",
      "fp comment result reason req reqpath pid",
      "Information; Warning for a refusal or failure" },
    { 120, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "lock",
      "fp comment result reason req reqpath pid",
      "Information; Warning for a refusal or failure" },
    { 121, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "unlock",
      "fp comment result reason req reqpath pid",
      "Information; Warning for a refusal or failure" },
    { 130, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "hello-enrol",
      "path result detail",
      "Information; Warning for a refusal or failure" },
    { 131, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "hello-protect",
      "path dest result detail err",
      "Information; Warning for a refusal or failure" },
    { 132, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "hello-unlock",
      "path result detail",
      "Information; Warning for a refusal or failure" },
    { 140, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "keyfile-warning",
      "path warning",
      "Warning" },
    { 150, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "reencrypt-all",
      "fp comment result reason req reqpath pid",
      "Information; Warning for a refusal or failure" },
    { 151, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "reencrypt",
      "fp comment result reason req reqpath pid",
      "Information; Warning for a refusal or failure" },
    { 152, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "decrypt",
      "fp comment result trigger req reqpath pid",
      "Information; Warning for a refusal or failure" },
    { 160, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "agent", "result",
      "Information; Warning for a refusal or failure" },
    { 170, KITTY_EVB_AGENT, KITTY_EVSRC_AGENT, "logrotate", "result",
      "Information; Warning for a refusal or failure" },

    /* kitty.exe */
    { 200, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "session-open",
      "session host port proto addr from pid",
      "Information" },
    { 201, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "session-close",
      "session host port result reason exitcode pid",
      "Information; Warning when fatal" },
    { 210, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "hostkey-known",
      "host port keytype fp basis pid proxy",
      "Information" },
    { 211, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "hostkey-new",
      "host port keytype fp result pid proxy",
      "Information; Warning when refused" },
    { 212, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "hostkey-mismatch",
      "host port keytype fp result pid proxy",
      "Warning" },
    { 220, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "auth-ok",
      "host port user method fp pid proxy for",
      "Information" },
    { 221, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "auth-failed",
      "host port user method reason methods pid proxy for",
      "Warning" },
    { 225, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "proxy-used",
      "proxy host port user credentials pid for",
      "Information" },
    { 230, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "farend-refused",
      "session host request reason detail pid",
      "Warning" },
    { 240, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "transfer-done",
      "session host port user method direction local remote bytes pid",
      "Information" },
    { 241, KITTY_EVB_KITTY, KITTY_EVSRC_KITTY, "transfer-failed",
      "session host port user method direction local remote bytes pid "
      "reason",
      "Warning" },

    /* klink, kscp, ksftp */
    { 300, KITTY_EVB_CLI, KITTY_EVSRC_CLI, "session-open",
      "session host port proto addr from pid",
      "Information" },
    { 301, KITTY_EVB_CLI, KITTY_EVSRC_CLI, "session-close",
      "session host port result reason exitcode pid",
      "Information; Warning when fatal" },
    { 310, KITTY_EVB_CLI, KITTY_EVSRC_CLI, "hostkey-known",
      "host port keytype fp basis pid proxy",
      "Information" },
    { 311, KITTY_EVB_CLI, KITTY_EVSRC_CLI, "hostkey-new",
      "host port keytype fp result pid proxy",
      "Information; Warning when refused" },
    { 312, KITTY_EVB_CLI, KITTY_EVSRC_CLI, "hostkey-mismatch",
      "host port keytype fp result pid proxy",
      "Warning" },
    { 320, KITTY_EVB_CLI, KITTY_EVSRC_CLI, "auth-ok",
      "host port user method fp pid proxy for",
      "Information" },
    { 321, KITTY_EVB_CLI, KITTY_EVSRC_CLI, "auth-failed",
      "host port user method reason methods pid proxy for",
      "Warning" },
    { 325, KITTY_EVB_CLI, KITTY_EVSRC_CLI, "proxy-used",
      "proxy host port user credentials pid for",
      "Information" },
    { 330, KITTY_EVB_CLI, KITTY_EVSRC_CLI, "transfer-done",
      "session host user direction local remote bytes pid",
      "Information" },
    { 331, KITTY_EVB_CLI, KITTY_EVSRC_CLI, "transfer-failed",
      "session host user direction local remote bytes pid reason",
      "Warning" },

    /* kittygen, kittygen-cli */
    { 400, KITTY_EVB_KITTYGEN, KITTY_EVSRC_KITTYGEN, "key-generated",
      "tool keytype bits fp comment pid",
      "Information" },
    { 401, KITTY_EVB_KITTYGEN, KITTY_EVSRC_KITTYGEN, "private-saved",
      "tool fp comment path format protection result reason pid",
      "Information; Warning when failed" },
    { 402, KITTY_EVB_KITTYGEN, KITTY_EVSRC_KITTYGEN, "public-saved",
      "tool fp path format result reason pid",
      "Information; Warning when failed" },
    { 403, KITTY_EVB_KITTYGEN, KITTY_EVSRC_KITTYGEN, "passphrase-changed",
      "tool fp path protection pid",
      "Information" },

    /* the suite, always written */
    { 900, KITTY_EVB_SUITE, KITTY_EVSRC_SUITE, "program-refused",
      "check found program",
      "Error" },
    { 901, KITTY_EVB_SUITE, KITTY_EVSRC_SUITE, "log-retention",
      "files kb days mb pattern",
      "Information" },
    { 902, KITTY_EVB_SUITE, KITTY_EVSRC_SUITE, "missing-features",
      "features missing",
      "Information" },
    { 903, KITTY_EVB_SUITE, KITTY_EVSRC_SUITE, "session-save-failed",
      "session path reason",
      "Warning" },
};
#endif /* KITTY_EVENTIDS_TABLE */

/*
 * The field glossary: every field name the IDs above use, once, in name
 * order, with its meaning and its fixed values - the manual's appendix
 * (doc/eventids.but) carries it as written here. Plain text: no quotes,
 * backslashes or braces (Halibut). tools/check-kitty-events.py and
 * test/test_eventids.c check it against the table. Compiled only where
 * KITTY_EVENTFIELDS_TABLE is defined (the unit test).
 */
struct kitty_eventfield {
    const char *name;
    const char *text;
};

#ifdef KITTY_EVENTFIELDS_TABLE
static const struct kitty_eventfield kitty_eventfields[] = {
    { "absent",
      "113: startup keys whose file was not there" },
    { "addr",
      "200, 300: the address connected to (a proxy's when one is used); a "
      "serial line's name" },
    { "basis",
      "210, 310: how the host key was known; values: cache, manual,"
      " ca" },
    { "bits",
      "400: the key size in bits (255 for Ed25519, the curve size "
      "for ECDSA)" },
    { "broken",
      "113: startup keys whose file could not be read" },
    { "bytes",
      "240, 241, 330, 331: the file's bytes moved; empty when not known" },
    { "check",
      "900: the check that refused the program file; values: "
      "rename, signature, integrity" },
    { "comment",
      "100-170: the comment of the key concerned; 400, 401: of the "
      "key generated or saved" },
    { "credentials",
      "225, 325: yes when a user name or password went to the proxy, "
      "else no" },
    { "days",
      "901: LogKeepDays as set, 0 = off" },
    { "dest",
      "131: the protected copy written" },
    { "detail",
      "130, 132: the detail Windows Hello reported; 230: the "
      "refusals the rate limit merged into this record (a number), "
      "open (a far2l clipboard open), the transfer id or the file "
      "name" },
    { "direction",
      "240, 241, 330, 331: put (upload, to the server) or get (download, "
      "from it)" },
    { "enc",
      "111: whether the key file has a passphrase; values: "
      "password, blank" },
    { "err",
      "131: the error text when the protection failed" },
    { "exitcode",
      "201: the remote command's exit code; 301: the tool's own "
      "exit code; empty when there is none" },
    { "features",
      "902: the features this Windows lacks, comma-separated" },
    { "files",
      "901: the log files deleted" },
    { "for",
      "220, 221, 225, 320, 321, 325: what the connection served; empty "
      "for the connection's own login; session for a jump host's login "
      "and for a proxy the session went through; forward for a port "
      "forwarding's connection, x11 for an X11 forwarding's" },
    { "format",
      "401: ppk3, ppk2, openssh, ssh.com or ssh1; 402: rfc4716, "
      "openssh or ssh1" },
    { "found",
      "900: what the check found; rename: the program's file name; "
      "signature: bad digest, not signed, or signer: followed by "
      "the signer name; integrity: no stamp, truncated, bad stamp, "
      "modified or cannot check" },
    { "fp",
      "the key fingerprint; 100-170: as the agent shows it (ssh-ed25519 "
      "255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...); "
      "210-221, 310-321, 400-403: the hash alone (SHA256:...; an SSH-1 "
      "key's MD5 hash, aa:bb:...), empty for a proxy's login" },
    { "from",
      "200, 300: the local end of the connection, address and port" },
    { "host",
      "the host name as given for the connection; a serial line's name "
      "(COM3); with proxy set and in 225, 325: the proxy's; 225, 325 "
      "local-command: the program's file name" },
    { "kb",
      "901: the kilobytes freed" },
    { "keytype",
      "210-212, 310-312: the host key's type as its fingerprint "
      "names it (ssh-ed25519, ssh-rsa, ...), rsa1 for an SSH-1 key; "
      "400: the key's SSH type" },
    { "loaded",
      "113: startup keys loaded" },
    { "local",
      "240, 241, 330, 331: the file's full local path; a 331 for a run "
      "that failed before any file: the local path as given" },
    { "mb",
      "901: LogKeepMB as set, 0 = off" },
    { "method",
      "220, 221, 320, 321: the authentication method; for 221 and 321 the "
      "one refused, or the last one tried when no method was left; "
      "values: none, publickey, password, keyboard-interactive, "
      "gssapi-with-mic, over SSH-1 also tis and cryptocard; for a proxy's "
      "login: password, chap, none, ident; 240, 241: zmodem or "
      "kitten-osc5113" },
    { "methods",
      "221, 321: the methods the server still offered, comma-separated "
      "(over SSH-1 from its authentication mask: publickey, password, "
      "tis, cryptocard); empty for a proxy's login" },
    { "missing",
      "902: each missing entry point with its DLL and the feature "
      "it powers, separated by semicolons" },
    { "path",
      "100-170: the key file; 401-403: the file written; 903: the "
      "session file" },
    { "pattern",
      "901: the folder and the log file name pattern searched" },
    { "pid",
      "100-170: the process that asked the agent, empty when none "
      "did; 200-403: the process writing the record" },
    { "port",
      "the port connected to; a serial line's speed; with proxy set and "
      "in 225, 325: the proxy's; 225, 325 local-command: empty" },
    { "program",
      "900: the refused program file's path" },
    { "protection",
      "401, 403: none, passphrase or hello" },
    { "proto",
      "200, 300: the protocol; values: ssh, telnet, raw, rlogin, supdup, "
      "serial" },
    { "proxy",
      "210-221, 310-321: empty for the connection's own host key and "
      "login; for a proxy's its type: ssh (a jump host), http, socks5, "
      "socks4; 225, 325: the proxy's type: http, socks4, socks5, "
      "telnet, local-command" },
    { "reason",
      "100-170: a token - gui, cmdline, ipc, ipc-policy, tray, lifetime, "
      "device-removal, idle, windows-lock, windows-unlock, click-allow, "
      "click-deny, click-cancel-latch, pileup, suppress-latch, "
      "hello-verified, hello-denied, hello-unavailable, hello-error; 201, "
      "301: the fatal error's text; 221, 321: access denied, public-key "
      "signature refused, no supported authentication methods available, "
      "the server's message when it ended the connection instead of "
      "answering, or a proxy's refusal as text; 230, 241, 331, 401, 402, "
      "903: the reason as text" },
    { "refused",
      "113: startup keys refused" },
    { "remote",
      "240, 241, 330, 331: the file's remote path - in full where the "
      "protocol names it (sftp), else as the server sent it (scp "
      "protocol: the name; ZModem download: the name rz announced, or "
      "empty; kitten: the far end's name)" },
    { "req",
      "100-170: the file name of the program that asked the agent" },
    { "reqpath",
      "100-170: that program's full path" },
    { "request",
      "230: what the far end asked for; values: osc52-read, "
      "osc5522-read, far2l-clipboard, file-read, file-write" },
    { "result",
      "the outcome; 100, 101: allowed, denied; 110, 114, 115: done, "
      "blocked; 111: loaded, failed, fp-refused, not-verified; 112: "
      "forgotten; 120, 121: done, refused, waiting, unlock-shut, shut, "
      "blocked; 130: added, denied, failed; 131: protected, denied, "
      "failed, startup-entry-replaced; 132: hello, hello-cached, "
      "recovery-passphrase, printed-secret, denied, no-door-here, failed; "
      "150, 151: done, failed; 152: done, failed (a wrong passphrase), "
      "cancelled; 160: start, stop; 170: failed; 201, 301: closed, "
      "remote-closed, fatal, user; 211, 212, 311, 312: accept-new, "
      "accepted, refused; 401, 402: saved, failed" },
    { "session",
      "the saved session's name, empty when none was loaded" },
    { "tool",
      "400-403: kittygen or kittygen-cli" },
    { "trigger",
      "113: what started the retry; values: button, device-arrival; 152: "
      "what asked for the passphrase; values: first-use (a signature), "
      "gui (the key list's Decrypt)" },
    { "user",
      "220, 221, 320, 321, 240, 241, 330, 331: the user name of the login "
      "(a proxy's: the name sent to it); 225, 325: the user name sent to "
      "the proxy, empty when none" },
    { "warning",
      "140: weak-protection or old-format" },
};
#endif /* KITTY_EVENTFIELDS_TABLE */

#endif /* KITTY_EVENTIDS_H */
