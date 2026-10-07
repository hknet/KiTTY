/*
 * kitty_termenv.c - the merge rule for the environment variables a
 * connection sends. Plain C, no PuTTY headers (test/test_termenv.c).
 * See kitty_termenv.h.
 */
#include <string.h>

#include "kitty_termenv.h"

int (*kitty_termenv_switch_hook)(void) = NULL;

KittyTermEnvMode kitty_termenv_mode(void)
{
    if (!kitty_termenv_switch_hook)
        return KITTY_TERMENV_AS_IS;
    return kitty_termenv_switch_hook() ? KITTY_TERMENV_DEFAULTS
                                       : KITTY_TERMENV_NO_DEFAULTS;
}

static bool is_default_name(const char *name)
{
    return !strcmp(name, KITTY_TERMENV_COLORTERM) ||
           !strcmp(name, KITTY_TERMENV_TERM_PROGRAM);
}

static bool session_has(const char *const *names, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(names[i], name))
            return true;
    return false;
}

size_t kitty_termenv_merge(const char *const *names, const char *const *values,
                           size_t n, KittyTermEnvMode mode,
                           KittyTermEnvVar *out)
{
    static const KittyTermEnvVar defaults[KITTY_TERMENV_EXTRA] = {
        { KITTY_TERMENV_COLORTERM, KITTY_TERMENV_COLORTERM_VALUE, true },
        { KITTY_TERMENV_TERM_PROGRAM, KITTY_TERMENV_TERM_PROGRAM_VALUE, true },
    };
    size_t i, count = 0;

    for (i = 0; i < n; i++) {
        /* An empty COLORTERM / TERM_PROGRAM is the session saying "do not
         * send it"; outside KiTTY++ it goes out as stored. */
        if (mode != KITTY_TERMENV_AS_IS && !*values[i] &&
            is_default_name(names[i]))
            continue;
        out[count].name = names[i];
        out[count].value = values[i];
        out[count].added = false;
        count++;
    }
    if (mode == KITTY_TERMENV_DEFAULTS) {
        for (i = 0; i < KITTY_TERMENV_EXTRA; i++)
            if (!session_has(names, n, defaults[i].name))
                out[count++] = defaults[i];
    }
    return count;
}
