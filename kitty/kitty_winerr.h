/*
 * kitty_winerr.h - Windows' reason for an error, as a message's last words:
 * "Access is denied (error 5)". Failure messages ("Not saved: <file>
 * (<reason>)") end in it. PuTTY's win_strerror gives
 * "Error 5: Access is denied." with the line's \r still on it, which put a
 * double full stop and a broken line into every such message.
 *
 * Header only, so every program that reports takes it without a link change.
 * The text lives in a buffer of the including file: read it before the next
 * call from the same file.
 */
#ifndef KITTY_WINERR_H
#define KITTY_WINERR_H

#include <windows.h>
#include <stdio.h>
#include <string.h>

static const char *kitty_win_reason(DWORD err)
{
    static char buf[512];
    char msg[400];
    size_t n;
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                        msg, sizeof(msg), NULL))
        msg[0] = '\0';
    n = strlen(msg);
    while (n && (msg[n-1] == '\r' || msg[n-1] == '\n' || msg[n-1] == ' ' || msg[n-1] == '.'))
        msg[--n] = '\0';
    if (n)
        snprintf(buf, sizeof(buf), "%s (error %lu)", msg, (unsigned long)err);
    else
        snprintf(buf, sizeof(buf), "Windows error %lu", (unsigned long)err);
    return buf;
}

#endif /* KITTY_WINERR_H */
