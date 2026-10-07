/*
 * kitty_termenv.h - the environment variables a connection sends: the
 * session's own (Connection > Login > Environment) plus, in
 * KiTTY++, COLORTERM=truecolor and TERM_PROGRAM=KiTTY++.
 *
 * Rules (KiTTY++ terminals only; every other program sends the session's
 * variables exactly as stored):
 *  - with [KiTTY] sendtermenv on (the default), a session that does not set
 *    COLORTERM / TERM_PROGRAM itself sends the default value;
 *  - a session's own value for either one wins;
 *  - a session's EMPTY value for either one means it is not sent at all.
 *    Other empty variables go out as before.
 *
 * Used by ssh/mainchan.c (SSH "env" requests) and otherbackends/telnet.c
 * (NEW-ENVIRON / OLD-ENVIRON). Lives in `utils`, because those files are
 * compiled without MOD_PERSO into libraries every program links; the
 * KiTTY++ terminals switch it on by setting kitty_termenv_switch_hook.
 */
#ifndef KITTY_TERMENV_H
#define KITTY_TERMENV_H

#include <stddef.h>
#include <stdbool.h>

#define KITTY_TERMENV_COLORTERM         "COLORTERM"
#define KITTY_TERMENV_COLORTERM_VALUE   "truecolor"
#define KITTY_TERMENV_TERM_PROGRAM      "TERM_PROGRAM"
#define KITTY_TERMENV_TERM_PROGRAM_VALUE "KiTTY++"
/* The most entries kitty_termenv_merge adds beyond the session's own. */
#define KITTY_TERMENV_EXTRA 2

typedef enum KittyTermEnvMode {
    KITTY_TERMENV_AS_IS,        /* not a KiTTY++ terminal: the session's list unchanged */
    KITTY_TERMENV_NO_DEFAULTS,  /* sendtermenv=no: the session's list, empty COLORTERM/TERM_PROGRAM dropped */
    KITTY_TERMENV_DEFAULTS      /* sendtermenv=yes: as above, plus the defaults the session does not set */
} KittyTermEnvMode;

typedef struct KittyTermEnvVar {
    const char *name;
    const char *value;
    bool added;   /* a default, not the session's: a refusal is not shown in the terminal */
} KittyTermEnvVar;

/*
 * The merge rule, pure. names[i] / values[i] are the session's n variables;
 * out has room for n + KITTY_TERMENV_EXTRA entries. The session's variables
 * come first in their order, the defaults after them. Returns the count.
 * out[] points into names / values or at static strings.
 */
size_t kitty_termenv_merge(const char *const *names, const char *const *values,
                           size_t n, KittyTermEnvMode mode,
                           KittyTermEnvVar *out);

/* Set by a KiTTY++ terminal at startup: returns the [KiTTY] sendtermenv
 * switch. NULL (every other program) means KITTY_TERMENV_AS_IS. */
extern int (*kitty_termenv_switch_hook)(void);
KittyTermEnvMode kitty_termenv_mode(void);

/* The list a connection sends, built from a Conf's CONF_environmt and
 * kitty_termenv_mode() (kitty_termenv_conf.c). Owns copies of the strings. */
typedef struct KittyTermEnvList {
    size_t n;
    char **names;
    char **values;
    bool *added;   /* as KittyTermEnvVar.added */
    size_t n_added;
} KittyTermEnvList;

struct conf_tag;
KittyTermEnvList *kitty_termenv_list_new(struct conf_tag *conf);
void kitty_termenv_list_free(KittyTermEnvList *list);

#endif
