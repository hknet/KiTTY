/*
 * kitty_hello_terminal.c - the terminal's side of Hello-protected key
 * files: implementations for the three hooks in ssh/userauth2-client.c.
 * With several terminals open, the anchor card carries the asking
 * window's title and opens over it, so the user knows WHICH session
 * wants the authorization.
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "kitty_hello.h"
#include "kitty_hello_keys.h"

extern char *(*kitty_hello_keyfile_unlock_hook)(const char *path);
extern char *(*kitty_hello_keyfile_translate_hook)(const char *path,
                                                   const char *typed);
extern int (*kitty_hello_keyfile_protected_hook)(const char *path);

static HWND kht_window;

static void kht_set_context(const char *path)
{
    char title[160];
    char line[220];
    const char *base = path, *q;
    title[0] = '\0';
    if (kht_window && IsWindow(kht_window))
        GetWindowTextA(kht_window, title, sizeof(title));
    for (q = path; *q; q++)
        if (*q == '\\' || *q == '/')
            base = q + 1;
    snprintf(line, sizeof(line), "%s  -  key %s",
             title[0] ? title : "KiTTY", base);
    kitty_hello_set_context(line, kht_window);
}

static char *kht_unlock(const char *path)
{
    char *pass = NULL;
    if (!kageant_hello_has_sidecar(path))
        return NULL;
    kht_set_context(path);
    if (kageant_hello_unlock(path, kht_window, &pass, NULL) ==
            KAGEANT_HELLO_OK)
        return pass;
    burnstr(pass);
    kitty_hello_set_context(NULL, NULL);   /* not consumed on failure */
    return NULL;
}

static char *kht_translate(const char *path, const char *typed)
{
    return kageant_hello_translate(path, typed, NULL);
}

static int kht_protected(const char *path)
{
    return kageant_hello_has_sidecar(path);
}

/* Install the hooks; the window is the terminal's, for the anchor card
 * and its context line. Call once the window exists. */
void kitty_hello_terminal_init(HWND terminal_window)
{
    kht_window = terminal_window;
    kitty_hello_keyfile_unlock_hook = kht_unlock;
    kitty_hello_keyfile_translate_hook = kht_translate;
    kitty_hello_keyfile_protected_hook = kht_protected;
}
