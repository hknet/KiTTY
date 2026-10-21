/*
 * KiTTY: reading the agent log's lines (the format is in kitty_auditlog.h).
 * No Win32 here, so test/test_auditparse.c can drive it directly; the agent
 * log viewer (windows/pageant.c) is the user.
 *
 * Two escapings meet in one log: files written before 0.85.1.18 escaped a
 * value with a backslash (\" \\ \n), later ones double a quote and write a
 * backslash as itself. kitty_auditparse_is_old decides per line, and
 * kitty_auditparse_from_old turns an old line into the current form, so
 * everything after it reads one form only.
 */

#ifndef KITTY_AUDITPARSE_H
#define KITTY_AUDITPARSE_H

#include <stddef.h>

/* 1 when the line is in the old form: it holds a doubled backslash that is
 * not the start of a value (a UNC path "\\server\..." of the current form
 * begins with one). Old lines with a path always carry doubled backslashes;
 * an old line whose only escapes are \" or \n is taken as current and
 * shows them as written. */
int kitty_auditparse_is_old(const char *line);

/* The old-form line re-quoted the current way (\\ -> \, \" -> "", \n -> ?).
 * Returns a malloc'd string; NULL only when out of memory. */
char *kitty_auditparse_from_old(const char *line);

/* One field of a current-form record, from *pp on: its key, and its value
 * unquoted ("" is one quote; a bare value such as the ts= stamp runs to the
 * next space). val may be NULL to skip the value. Returns 0 at the end of
 * the line. Walking field by field keeps a value's CONTENT - a comment
 * reading x req="evil", written x req=""evil"" - from being taken for a
 * field of its own. */
int kitty_auditparse_next(const char **pp, char *key, size_t ksz,
                          char *val, size_t vsz);

/* One field's value by key (current form). Returns 0 if absent. */
int kitty_auditparse_field(const char *line, const char *key,
                           char *out, size_t outsz);

#endif /* KITTY_AUDITPARSE_H */
