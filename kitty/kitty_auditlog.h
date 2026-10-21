/*
 * KiTTY: the audit log - a local, append-only record of what the agent did
 * with keys: what signed, what was refused and why, what was added or
 * removed and by which process. Two sinks, independent of each other: a
 * capped, rotated FILE, and the Windows EVENT LOG (one event per record,
 * one event ID per kind, every field its own insertion string).
 *
 * FILE FORMAT - one line per event, extensible by design:
 *     ts=2026-08-22T12:34:56Z ev="sign" fp="SHA256:..." result="allowed" ...
 * Keys are bare [a-z0-9_]; every value is double-quoted. Inside a value a
 * quote is doubled ("") and every control byte (CR and LF included) is
 * written as ? - a value arriving over the pipe must never be able to close
 * its quote early or FORGE a line. A backslash is written as itself, so a
 * path reads as it is. (Files written before this change escaped with a
 * backslash: \" \\ \n. The viewer reads both, line by line.) New fields may
 * be appended to any event at any time; parsers carry unknown keys.
 * Timestamps are UTC (the viewer renders local time).
 *
 * HONESTY: a local file is evidence, never proof - same-user code can
 * delete it. The Event Log is tamper-evident (a clear is itself an event);
 * real audit comes from forwarding it off the machine.
 */

#ifndef KITTY_AUDITLOG_H
#define KITTY_AUDITLOG_H

/* The sinks, for kitty_audit_set_sinks. */
#define KITTY_AUDIT_SINK_FILE      1
#define KITTY_AUDIT_SINK_EVENTLOG  2

/* The Event Log source every record is reported under, and the log the
 * system-wide installer registers it in. Without that registration the
 * source is unknown to Windows and its events land in the Application log;
 * the event data (the insertion strings) is the same either way. */
#define KITTY_AUDIT_EVENT_SOURCE   "KiTTY++_agent"
#define KITTY_AUDIT_EVENT_LOG      "KiTTY++_Agent"

/* Configure (or reconfigure) the sink. path = full path of the active log
 * file; enabled 0 silences everything; maxkb caps the active file before
 * rotation; keep = rotated generations (.1 .. .keep); expiredays deletes
 * rotated generations older than that at rotation time (0 = keep). */
void kitty_audit_configure(const char *path, int enabled,
                           int maxkb, int keep, int expiredays);
/* Which sinks get the records: KITTY_AUDIT_SINK_* or-ed together. The file
 * sink also needs a path (kitty_audit_configure). Default: the file only. */
void kitty_audit_set_sinks(int sinks);
int kitty_audit_sinks(void);
int kitty_audit_enabled(void);
const char *kitty_audit_path(void);   /* "" until configured */

/* 1 when a record's result value means a refusal or a failure. */
int kitty_audit_result_is_warning(const char *result);
/* 1 for the records the Event Log sink reports as warnings: refusals,
 * failures and key file warnings (ev "keyfile-warning"). */
int kitty_audit_is_warning(const char *ev, const char *result);

/* Write one event. Arguments are key,value string pairs, terminated by a
 * NULL key; a NULL or empty value drops that pair, so optional fields can
 * be passed unconditionally. The timestamp and quoting are handled here.
 * A field the Event Log sink is to carry must be listed for its kind in
 * kitty_auditlog.c (audit_kinds) and in windows/kitty_events.mc. */
void kitty_audit(const char *ev, ...);

#endif /* KITTY_AUDITLOG_H */
