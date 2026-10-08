/*
 * kitty_termenv.c - the merge rule for the environment variables a
 * connection sends. Plain C, no PuTTY headers (test/test_termenv.c).
 * See kitty_termenv.h.
 */
#include <stdio.h>
#include <string.h>

#include "kitty_termenv.h"
#include "kitty_text.h"   /* the refusal's wording */

int (*kitty_termenv_switch_hook)(void) = NULL;
bool (*kitty_termenv_hint_hook)(const char *host_id) = NULL;
bool (*kitty_termenv_note_hook)(const char *plain_note) = NULL;

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

size_t kitty_termenv_host_id(const char *host, int port, char *buf,
                             size_t size)
{
    char portstr[16];
    size_t hlen, need, i, at = 0;
    bool bracket;

    if (!host || !*host || !buf || !size)
        return 0;
    hlen = strlen(host);
    portstr[0] = '\0';
    if (port != 22)
        snprintf(portstr, sizeof(portstr), ":%d", port);
    /* "::1:2222" would not say where the address ends */
    bracket = *portstr && strchr(host, ':') != NULL;
    need = hlen + strlen(portstr) + (bracket ? 2 : 0);
    if (need + 1 > size)
        return 0;
    if (bracket)
        buf[at++] = '[';
    for (i = 0; i < hlen; i++) {
        char c = host[i];
        buf[at++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    if (bracket)
        buf[at++] = ']';
    strcpy(buf + at, portstr);
    return need;
}

/* Appends s to buf as far as size allows, counting the whole length. */
static void note_put(char *buf, size_t size, size_t *len, const char *s)
{
    size_t l = strlen(s);
    if (*len + 1 < size) {
        size_t room = size - 1 - *len;
        memcpy(buf + *len, s, l < room ? l : room);
    }
    *len += l;
}

size_t kitty_termenv_refused_own_note(const char *const *names,
                                      const bool *added, const bool *refused,
                                      size_t n, bool colour, char *buf,
                                      size_t size)
{
    size_t i, len = 0, own = 0;
    int pass;

    for (i = 0; i < n; i++)
        if (refused[i] && !added[i])
            own++;
    if (!own) {
        if (buf && size)
            *buf = '\0';
        return 0;
    }
    if (!buf)
        size = 0;
    note_put(buf, size, &len, colour ? KT_TERMENV_REFUSED_OWN_HEAD
                                     : KT_TERMENV_REFUSED_OWN_HEAD_PLAIN);
    for (pass = 0; pass < 2; pass++) {
        bool first = true;
        if (pass)
            note_put(buf, size, &len, KT_TERMENV_REFUSED_OWN_MID);
        for (i = 0; i < n; i++) {
            if (!refused[i] || added[i])
                continue;
            if (!first)
                note_put(buf, size, &len, pass ? " " : ", ");
            note_put(buf, size, &len, names[i]);
            first = false;
        }
    }
    note_put(buf, size, &len, KT_TERMENV_REFUSED_OWN_TAIL);
    if (size)
        buf[len < size ? len : size - 1] = '\0';
    return len;
}

bool kitty_termenv_hint_due(const KittyTermEnvHintStore *store,
                            const char *host_id)
{
    if (!store || !host_id || !*host_id)
        return false;
    if (store->said(store->ctx, host_id))
        return false;
    store->mark(store->ctx, host_id);
    return true;
}
