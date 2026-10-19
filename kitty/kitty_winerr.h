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
    {
        /* Windows' texts keep their inserts with IGNORE_INSERTS: error 216
         * reads "This version of %1 is not compatible ...". Each %<digit>
         * becomes "the file" - what they stand for in these texts. */
        char out[sizeof(msg)];
        size_t i = 0, o = 0;
        while (msg[i] && o + 9 < sizeof(out)) {
            if (msg[i] == '%' && msg[i+1] >= '1' && msg[i+1] <= '9') {
                memcpy(out + o, "the file", 8);
                o += 8;
                i += 2;
            } else {
                out[o++] = msg[i++];
            }
        }
        out[o] = '\0';
        memcpy(msg, out, o + 1);
    }
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
