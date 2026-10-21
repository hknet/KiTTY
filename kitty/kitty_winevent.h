/*
 * kitty_winevent.h - the Windows Event Log records of kitty.exe (2xx), the
 * command-line tools (3xx), kittygen (4xx) and the suite (9xx). The IDs,
 * sources and field lists are kitty_eventids.h's; this is the writer.
 *
 * The 2xx-4xx records are switched per program ([KiTTY] sessioneventlog,
 * clieventlog, kittygeneventlog, default no). Each program reads its switch
 * once and calls kitty_wev_configure; until then, and while it is off,
 * every call below returns at its first line, so the SSH code may call
 * them on any path without a cost. The 9xx records are always written.
 *
 * Every field is its own insertion string; an absent field is an empty
 * string, a control byte becomes ?. Never a passphrase or a password.
 *
 * In `utils` with no import library of its own (advapi32 is in every
 * program), so the shared SSH and settings code can call it directly.
 */
#ifndef KITTY_WINEVENT_H
#define KITTY_WINEVENT_H

#include "kitty_eventids.h"

/* The program's block (KITTY_EVB_KITTY / _CLI / _KITTYGEN) and its switch.
 * Called once, early; a second call changes nothing. */
void kitty_wev_configure(unsigned block, int enabled);
/* 1 while the 2xx-4xx records are written. */
int kitty_wev_enabled(void);

/* The session (2xx/3xx). The Seat of the program's own connection: records
 * from a proxy or jump host's connection, which runs on a Seat of its own,
 * are left out. Callers unwrap a TempSeat first. A NULL Seat registered
 * accepts every Seat. */
void kitty_wev_main_seat(const void *seat);
int kitty_wev_is_main(const void *seat);
/* What this connection is: the saved session's name (or ""), the host and
 * port asked for, the protocol's name. Set before the connection starts;
 * the session name may come earlier from the command line. */
void kitty_wev_session_name(const char *session);
void kitty_wev_target(const char *host, int port, const char *proto);
/* 200/300: the connection is up (addr = where to, from = the local end). */
void kitty_wev_session_open(const void *seat, const char *addr,
                            const char *from);
/* 201/301, once per open: result closed / remote-closed / fatal / user;
 * exitcode < 0 = none. Warning for fatal. */
void kitty_wev_session_close(const char *result, const char *reason,
                             int exitcode);

/* 210/310 (basis cache / manual / ca), 211-212/311-312 (result accept-new /
 * accepted / refused; Warning when refused, and always for a mismatch).
 * fp is the fingerprint as PuTTY prints it; only its hash goes in. */
void kitty_wev_hostkey(unsigned offset, const char *host, int port,
                       const char *keytype, const char *fp,
                       const char *basis_or_result);
/* A host key question is open: its answer is reported by
 * kitty_wev_hostkey_answer once the SSH layer has it. */
void kitty_wev_hostkey_pending(unsigned offset, const char *host, int port,
                               const char *keytype, const char *fp);
void kitty_wev_hostkey_answer(int accepted);

/* 220/320 and 221/321 (once per connection: no method was left). fp = the
 * key used, or NULL. methods = what the server still offered. */
void kitty_wev_auth_ok(const char *host, int port, const char *user,
                       const char *method, const char *fp);
void kitty_wev_auth_failed(const char *host, int port, const char *user,
                           const char *method, const char *reason,
                           const char *methods);

/* 230, kitty.exe: the far end asked for something it refused. request =
 * osc52-read / osc5522-read / far2l-clipboard / file-read / file-write. */
void kitty_wev_farend_refused(const char *request, const char *reason,
                              const char *detail);

/* 330 / 331, the command-line tools: one per job. */
void kitty_wev_transfer(int ok, const char *user, const char *direction,
                        const char *local, const char *remote,
                        unsigned long files, unsigned long long bytes,
                        unsigned long errors, const char *reason);

/* 4xx and anything else in the program's block: vals holds every field of
 * the table but "pid", in the table's order (NULL = empty); pid is filled
 * in here. */
void kitty_wev_report(unsigned offset, int warning,
                      const char *const *vals, int n);

/* 9xx: always written, source KiTTY++, the Application log. type is an
 * EVENTLOG_*_TYPE value. "pid" is not among their fields. Only where the
 * source has a message file (the system MSI registers kitty.exe for it):
 * without one the Event Viewer shows no description, so `text` - the same
 * record as one readable line - goes out as event ID 0 instead. */
void kitty_wev_suite(unsigned id, unsigned type,
                     const char *const *vals, int n, const char *text);
/* 1 when the source KiTTY++ has an EventMessageFile; looked up once. */
int kitty_wev_suite_registered(void);

/* The fingerprint's hash: what follows the last space ("ssh-ed25519 255
 * SHA256:abc" -> "SHA256:abc"). */
const char *kitty_wev_fp_hash(const char *fp);

/* A fatal connection message as a session-close result: "remote-closed"
 * when the far end closed the connection, else "fatal". */
const char *kitty_wev_close_result(const char *msg);

/* 1 when this Seat is the program's own connection and the records are on
 * (a TempSeat is taken for the Seat it stands for). In
 * utils/backend_socket_log.c, where PuTTY's Seat is known. */
struct Seat;
int kitty_wev_own_seat(struct Seat *seat);

/* [KiTTY] clieventlog / kittygeneventlog = yes, read the way kitty.exe
 * stores it (kitty_winevent_cfg.c: registry first in registry mode, else
 * kitty.ini). Not in kitty.exe, which reads sessioneventlog itself. */
int kitty_wev_switch_on(const char *key);

#endif /* KITTY_WINEVENT_H */
