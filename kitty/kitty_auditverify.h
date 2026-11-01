/*
 * kitty_auditverify.h - kageant's check of the agent log's chain against
 * the Windows Event Log: the files read from disk, the anchors (event 171)
 * read from the log KiTTY++_Agent and the Application log (Vista on:
 * EvtQuery, Windows XP: the classic event log API), or from an exported
 * .evtx file. kitty_auditchain.c does the checking.
 */
#ifndef KITTY_AUDITVERIFY_H
#define KITTY_AUDITVERIFY_H

struct strbuf;

/* Verify the log whose active file is path, with its generations path.N.
 * evtx: an exported log to read the anchors from instead (NULL = this
 * computer's logs). live_run: the run id of the kageant writing this log
 * now (kitty_audit_run_id), or NULL. The report goes to out. Returns 0
 * intact, 1 broken, 2 cannot verify. */
int kitty_auditverify(const char *path, const char *evtx,
                      const char *live_run, struct strbuf *out);

#endif /* KITTY_AUDITVERIFY_H */
