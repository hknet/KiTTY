/*
 * KiTTY: the audit log - a local, append-only record of what the agent did
 * with keys: what signed, what was refused and why, what was added or
 * removed and by which process. The FIRST STAGE of an auditable agent: a
 * capped, rotated file. (A Windows Event Log sink and off-box forwarding
 * are later stages; this format is designed so they bolt on.)
 *
 * FORMAT - one line per event, extensible by design:
 *     ts=2026-08-22T12:34:56Z ev=sign fp="SHA256:..." result="allowed" ...
 * Keys are bare [a-z0-9_]; every value is double-quoted with `\` escaping
 * for `"`, `\`, and newlines (written as the two characters \n - a value
 * arriving over the pipe must never be able to FORGE a log line). New
 * fields may be appended to any event at any time; parsers carry unknown
 * keys. Timestamps are UTC (the viewer renders local time).
 *
 * HONESTY: a local file is evidence, never proof - same-user code can
 * delete it. Tamper-EVIDENCE and real audit come from the later sinks.
 */

#ifndef KITTY_AUDITLOG_H
#define KITTY_AUDITLOG_H

/* Configure (or reconfigure) the sink. path = full path of the active log
 * file; enabled 0 silences everything; maxkb caps the active file before
 * rotation; keep = rotated generations (.1 .. .keep); expiredays deletes
 * rotated generations older than that at rotation time (0 = keep). */
void kitty_audit_configure(const char *path, int enabled,
                           int maxkb, int keep, int expiredays);
int kitty_audit_enabled(void);
const char *kitty_audit_path(void);   /* "" until configured */

/* Write one event. Arguments are key,value string pairs, terminated by a
 * NULL key; a NULL or empty value drops that pair, so optional fields can
 * be passed unconditionally. The timestamp and quoting are handled here. */
void kitty_audit(const char *ev, ...);

#endif /* KITTY_AUDITLOG_H */
