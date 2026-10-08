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
    bool added;   /* a default, not the session's: a refusal names the sshd_config fix */
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
    bool *refused; /* set by the SSH backend from the server's replies */
    size_t n_added;
} KittyTermEnvList;

struct conf_tag;
KittyTermEnvList *kitty_termenv_list_new(struct conf_tag *conf);
void kitty_termenv_list_free(KittyTermEnvList *list);

/*
 * A server that refuses a default (an `added` entry) gets, besides its
 * Event Log line, ONE terminal NOTE per host - the first connection that
 * meets the refusal says it, no later one does (fixed or not: a fixed server
 * simply refuses nothing). Nothing default sent (sendtermenv=no, the session
 * sets both, Telnet) means nothing refused and no NOTE.
 *
 * The host is the one the host-key cache is keyed on (CONF_loghost applied),
 * lower case, with ":port" when the port is not 22 and the IPv6 literal then
 * in brackets: "example.com", "example.com:2222", "[::1]:2222". Writes it to
 * buf and returns its length; 0 when there is no host or it does not fit.
 */
#define KITTY_TERMENV_HOST_ID_MAX 300

/*
 * The session's OWN refused variables (refused[i] and not added[i]) as one
 * terminal line, on every connection (kitty_text.h, KT_TERMENV_REFUSED_OWN_*):
 * "NOTE: the server refused A, B; its sshd_config needs "AcceptEnv A B"."
 * colour: "NOTE:" in yellow through colour sequences; false = plain text.
 * Like snprintf: returns the full length (0 = nothing of the session's own
 * was refused), writes at most size bytes with the terminating NUL; buf may
 * be NULL when size is 0.
 */
size_t kitty_termenv_refused_own_note(const char *const *names,
                                      const bool *added, const bool *refused,
                                      size_t n, bool colour, char *buf,
                                      size_t size);

/* Set by klink / kscp / ksftp at startup to kitty_termenv_console_note: gets
 * the plain NOTE and writes it to a console stderr with "NOTE:" in yellow
 * (true), or returns false when stderr is not a console, and the plain NOTE
 * goes out through the normal output. NULL (the terminals, which show colour
 * sequences) means the NOTE with its colour sequences. */
extern bool (*kitty_termenv_note_hook)(const char *plain_note);
bool kitty_termenv_console_note(const char *plain_note);
size_t kitty_termenv_host_id(const char *host, int port, char *buf,
                             size_t size);

/* Where "the NOTE was said for this host" is kept: said() reads the mark,
 * mark() sets it (false = could not). */
typedef struct KittyTermEnvHintStore {
    bool (*said)(void *ctx, const char *host_id);
    bool (*mark)(void *ctx, const char *host_id);
    void *ctx;
} KittyTermEnvHintStore;

/* The rule, pure: true when the NOTE is due for host_id, which is then
 * marked as said. A mark that cannot be written still lets this one NOTE
 * through. */
bool kitty_termenv_hint_due(const KittyTermEnvHintStore *store,
                            const char *host_id);

/* Set by a KiTTY++ terminal at startup to kitty_termenv_hint_first: asked
 * once per connection that had a default refused. NULL (every other
 * program) means no NOTE. */
extern bool (*kitty_termenv_hint_hook)(const char *host_id);

/* kitty_termenv_hint.c, in the terminals only: kitty_termenv_hint_due on
 * the store in use - the folder store's TermEnvHint folder, or the
 * registry's <base>\TermEnvHint key. */
bool kitty_termenv_hint_first(const char *host_id);

#endif
