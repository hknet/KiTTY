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
 * Every line ends with seq="N" prev="<base64 SHA-256 of the line before>";
 * an anchor record also with sig="<base64 Ed25519 signature>" over the
 * bytes before ` sig="` (kitty_auditchain.h).
 *
 * HONESTY: a local file is evidence, never proof - same-user code can
 * delete it. The chain shows a changed, missing or added line up to the
 * last anchor; the anchors in the Event Log show a cut or a replaced file
 * (a clear of the Event Log is itself an event). Real audit comes from
 * forwarding it off the machine.
 */

#ifndef KITTY_AUDITLOG_H
#define KITTY_AUDITLOG_H

#include <stddef.h>

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
 * kitty_auditlog.c (audit_kinds) and in windows/kitty_events.mc; a kind
 * not listed there goes to the file only (the build check refuses one). */
void kitty_audit(const char *ev, ...);

/*
 * THE CHAIN (kitty_auditchain.h): while the file is written, every line
 * carries seq, prev and sig, signed with a key made for this run and kept
 * in this process's memory only. The anchor records (ev="logchain", event
 * 171) go to the Windows Event Log whatever the sinks are. A record that
 * repeats the last one written in full is counted, not written: a summary
 * (ev="repeated", event 172) stands for the repeats. kageant is the one
 * writer: a kageant started as a client of a running one never configures
 * the log, so it writes nothing.
 *
 * The CSPRNG the run key is drawn from; set before the first configure.
 */
void kitty_audit_set_random(void (*fn)(void *buf, size_t len));
/* This run's id (32 hex digits), NULL before the chain started. */
const char *kitty_audit_run_id(void);
/* A head anchor when records were written since the last anchor and
 * KITTY_AUDITCHAIN_HEAD_MS have passed, and the summary of repeats counted
 * KITTY_AUDITCHAIN_REPEAT_MS ago; kageant calls it every 10 seconds. */
void kitty_audit_chain_tick(void);
/* That interval in ms (a test build takes KITTY_LOGCHAIN_HEAD_MS). */
unsigned long kitty_audit_chain_head_ms(void);
/* A config anchor before a change of agentlog, agentlogpath or
 * agentlogsink takes effect: value = "<setting>=<new value>". */
void kitty_audit_chain_config(const char *value);
/* The summary of repeats still counted, then the stop anchor, at an
 * orderly exit after the last record; the run key is wiped. */
void kitty_audit_chain_end(void);

#endif /* KITTY_AUDITLOG_H */
