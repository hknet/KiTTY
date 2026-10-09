/*
 * kitty_console_mpw.c: klink, kscp and ksftp prompt for the master password.
 *
 * A folder-store session (or a -pwfile / -proxypwfile line) can hold a
 * password wrapped with the portable master password. kitty.exe prompts for it
 * in a dialog (kitty_mpw_gui.c); the console tools prompt on the console:
 *
 *  - only when such a value is actually met - the secret store calls the
 *    prompt (kitty_set_master_pw_prompt), so nothing protected, no prompt;
 *  - with no echo, before the connection opens (sessions are read first);
 *  - three tries, then the tool stops - as with a key passphrase;
 *  - never where nobody can answer: under -batch, or when standard input is
 *    not a console (a pipe or a file - klink's piped stdin is the session's
 *    input, a prompt would swallow a line of it), the tool stops and points
 *    to -masterpwfile instead of hanging;
 *  - once per run: the store caches the unlocked key.
 *
 * A wrong -masterpwfile stops the tool too, rather than going on as if the
 * session had no stored password.
 */

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "putty.h"
#include "kitty_console_store.h"
#include "kitty_secretstore.h"
#include "kitty_text.h"

static int kcm_asked = 0;

/* -batch anywhere on the command line. Read from the command line itself, not
 * from console_batch_mode alone: the prompt can come while -load is parsed,
 * before a later -batch has been. */
static bool kcm_batch(void)
{
    int argc = 0, i;
    LPWSTR *argv;
    bool batch = console_batch_mode;

    if (batch)
        return true;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return false;
    for (i = 1; i < argc; i++)
        if (!wcscmp(argv[i], L"-batch"))
            batch = true;
    LocalFree(argv);
    return batch;
}

static char *kcm_prompt(int creating)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode, got = 0;
    wchar_t buf[512];
    char *res;
    int n;

    (void)creating;              /* the console tools only read a store */
    if (kcm_batch()) {
        fputs(KT_CLI_MPW_NEEDED_BATCH, stderr);
        cleanup_exit(1);
    }
    if (!in || in == INVALID_HANDLE_VALUE || !GetConsoleMode(in, &mode)) {
        fputs(KT_CLI_MPW_NEEDED_NOCONSOLE, stderr);
        cleanup_exit(1);
    }
    if (kcm_asked++)
        fprintf(stderr, "%s\n", KT_CLI_MPW_WRONG_RETRY);
    fputs(KT_CLI_MPW_PROMPT, stderr);
    fflush(stderr);
    SetConsoleMode(in, (mode | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT) &
                   ~ENABLE_ECHO_INPUT);
    if (!ReadConsoleW(in, buf, lenof(buf) - 1, &got, NULL))
        got = 0;
    SetConsoleMode(in, mode);
    fputs("\n", stderr);
    buf[got] = L'\0';
    while (got && (buf[got - 1] == L'\n' || buf[got - 1] == L'\r'))
        buf[--got] = L'\0';
    /* UTF-8, as the dialog hands it over: the same master password must
     * derive the same key in both */
    n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
    res = (char *)malloc(n > 0 ? n : 1);   /* the store free()s it */
    if (res) {
        res[0] = '\0';
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, res, n, NULL, NULL);
    }
    SecureZeroMemory(buf, sizeof(buf));
    return res;                  /* empty = cancelled: the store stops prompting */
}

static void kcm_wrong(int supplied)
{
    fputs(supplied ? KT_CLI_MPW_WRONG_SUPPLIED : KT_CLI_MPW_WRONG_FINAL, stderr);
    cleanup_exit(1);
}

void kitty_console_mpw_init(void)
{
    kitty_set_master_pw_prompt(kcm_prompt);
    kitty_set_master_pw_wrong(kcm_wrong);
}
