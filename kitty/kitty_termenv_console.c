/*
 * kitty_termenv_console.c - klink / kscp / ksftp's side of the refused-
 * variables NOTE (kitty_termenv.h). On a console, "NOTE:" is coloured
 * yellow through the console's text attributes, not colour sequences: the
 * console tools never switch the console to sequence processing, and older
 * consoles cannot do it at all. Redirected to a file or a pipe, the NOTE
 * stays plain text and goes out through the normal output.
 */
#include <windows.h>
#include <string.h>

#include "kitty_termenv.h"
#include "kitty_text.h"

/* Writes a part of the NOTE; false = the console refused it. */
static bool put(HANDLE h, const char *s, size_t len)
{
    DWORD done;
    return !len || (WriteConsoleA(h, s, (DWORD)len, &done, NULL) &&
                    done == (DWORD)len);
}

bool kitty_termenv_console_note(const char *plain_note)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    const char *tag;
    size_t tag_len = strlen(KT_TERMENV_NOTE_TAG);
    WORD yellow;
    bool ok;

    /* not a console (a file, a pipe, none): the plain NOTE, as it is */
    if (!h || h == INVALID_HANDLE_VALUE ||
        !GetConsoleScreenBufferInfo(h, &info))
        return false;
    tag = strstr(plain_note, KT_TERMENV_NOTE_TAG);
    if (!tag)
        return false;

    /* the background as it is; the tag bright yellow */
    yellow = (WORD)((info.wAttributes & ~(FOREGROUND_RED | FOREGROUND_GREEN |
                                          FOREGROUND_BLUE |
                                          FOREGROUND_INTENSITY)) |
                    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    if (!put(h, plain_note, (size_t)(tag - plain_note)))
        return false;
    SetConsoleTextAttribute(h, yellow);
    ok = put(h, tag, tag_len);
    SetConsoleTextAttribute(h, info.wAttributes);
    if (!ok)
        return false;
    /* the rest; a failure here has already shown the head, so it counts as
     * written rather than printing the NOTE twice */
    put(h, tag + tag_len, strlen(tag + tag_len));
    return true;
}
