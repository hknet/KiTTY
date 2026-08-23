/*
 * kitty_hello_keys.c - Hello-protected keys, kageant side.
 *
 * A protected key is an ordinary PPK whose passphrase is a random secret
 * in its printed form (see kitty_hello_secret_text). The secret lives,
 * wrapped, in "<keyfile>.hello" beside it: one W door per enrolled
 * Windows account+machine, an R door keyed on the recovery (by default
 * the key's original) passphrase, and the printed form itself as the
 * last door - it is literally the passphrase, so any PuTTY-compatible
 * tool opens the file with it.
 *
 * Protection is never a side effect of loading: the source file is never
 * rewritten, only a protected COPY is written. Nothing in here shows a
 * dialog except the Hello prompts themselves.
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "kitty_hello.h"
#include "kitty_hello_keys.h"

char *kageant_hello_sidecar_path(const char *keypath)
{
    return dupcat(keypath, ".hello");
}

int kageant_hello_has_sidecar(const char *keypath)
{
    char *p = kageant_hello_sidecar_path(keypath);
    DWORD a = GetFileAttributesA(p);
    sfree(p);
    return a != INVALID_FILE_ATTRIBUTES &&
           !(a & FILE_ATTRIBUTE_DIRECTORY);
}

char *kageant_hello_read_sidecar(const char *keypath)
{
    char *p = kageant_hello_sidecar_path(keypath);
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char *text = NULL;
    sfree(p);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(h, NULL), got = 0;
        if (size != INVALID_FILE_SIZE && size > 0 && size < 1024 * 1024) {
            text = snewn(size + 1, char);
            if (ReadFile(h, text, size, &got, NULL) && got == size) {
                char *e;
                text[size] = '\0';
                /* One line; tolerate trailing whitespace/newlines. */
                for (e = text + size; e > text &&
                     (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' ||
                      e[-1] == '\t'); e--)
                    e[-1] = '\0';
            } else {
                sfree(text);
                text = NULL;
            }
        }
        CloseHandle(h);
    }
    if (text && !kitty_hello_container_valid(text)) {
        sfree(text);
        text = NULL;
    }
    return text;
}

int kageant_hello_write_sidecar(const char *keypath, const char *container)
{
    char *p = kageant_hello_sidecar_path(keypath);
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    int ok = 0;
    sfree(p);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD len = (DWORD)strlen(container), put = 0;
        ok = WriteFile(h, container, len, &put, NULL) && put == len &&
             WriteFile(h, "\r\n", 2, &put, NULL) && put == 2;
        CloseHandle(h);
    }
    return ok;
}

char *kageant_hello_default_destpath(const char *srcpath)
{
    const char *base = srcpath, *q, *dot = NULL;
    for (q = srcpath; *q; q++)
        if (*q == '\\' || *q == '/')
            base = q + 1;
    for (q = base; *q; q++)
        if (*q == '.')
            dot = q;
    if (dot && dot > base)
        return dupprintf("%.*s-hello%s", (int)(dot - srcpath), srcpath, dot);
    return dupcat(srcpath, "-hello.ppk");
}

int kageant_hello_offerable(void)
{
    return kitty_hello_prf_available() == 1 ||
           kitty_hello_key_available() == 1;
}

int kageant_hello_enrolled_here(const char *keypath)
{
    char *c = kageant_hello_read_sidecar(keypath);
    int ret;
    if (!c)
        return -1;
    ret = kitty_hello_container_my_w(c) >= 0 ? 1 : 0;
    sfree(c);
    return ret;
}

int kageant_hello_unlock(const char *keypath, HWND owner,
                         char **passphrase_out, char **owners_out)
{
    char *c;
    unsigned char secret[KITTY_HELLO_SECRET_LEN];
    int ret;

    *passphrase_out = NULL;
    if (owners_out)
        *owners_out = NULL;
    if (!kageant_hello_has_sidecar(keypath))
        return KAGEANT_HELLO_NOSIDECAR;
    c = kageant_hello_read_sidecar(keypath);
    if (!c)
        return KAGEANT_HELLO_ERROR;

    ret = kitty_hello_unwrap_auto(owner, c, secret);
    switch (ret) {
      case KITTY_HELLO_VERIFIED:
        *passphrase_out = kitty_hello_secret_text(secret);
        smemclr(secret, sizeof(secret));
        ret = *passphrase_out ? KAGEANT_HELLO_OK : KAGEANT_HELLO_ERROR;
        break;
      case KITTY_HELLO_DENIED:
        ret = KAGEANT_HELLO_DENIED;
        break;
      case KITTY_HELLO_UNAVAILABLE:
        if (owners_out)
            *owners_out = kitty_hello_container_owners_text(c);
        ret = KAGEANT_HELLO_NODOOR;
        break;
      default:
        ret = KAGEANT_HELLO_ERROR;
        break;
    }
    sfree(c);
    return ret;
}

char *kageant_hello_translate(const char *keypath, const char *typed,
                              int *via_recovery_out)
{
    char *c;
    unsigned char secret[KITTY_HELLO_SECRET_LEN];
    char *out = NULL;

    if (via_recovery_out)
        *via_recovery_out = 0;
    if (!typed)
        return NULL;
    /* The printed secret typed in (any case/separators): canonicalise,
     * so the PPK sees exactly the string it was saved with. */
    if (kitty_hello_secret_from_text(typed, secret) == 1) {
        out = kitty_hello_secret_text(secret);
        smemclr(secret, sizeof(secret));
        return out;
    }
    c = kageant_hello_read_sidecar(keypath);
    if (!c)
        return NULL;
    if (kitty_hello_container_open_recovery(c, typed, secret) == 1) {
        out = kitty_hello_secret_text(secret);
        smemclr(secret, sizeof(secret));
        if (via_recovery_out)
            *via_recovery_out = 1;
    }
    sfree(c);
    return out;
}

int kageant_hello_protect(HWND owner, const char *srcpath,
                          const char *srcpass, const char *recovery_pass,
                          const char *destpath, char **printed_out,
                          char **err_out)
{
    Filename *srcfn, *dstfn;
    ssh2_userkey *key;
    const char *loaderr = NULL;
    unsigned char secret[KITTY_HELLO_SECRET_LEN];
    char *printed = NULL, *container = NULL;
    const char *rpass;
    int ret = KAGEANT_HELLO_ERROR, hret;

    *printed_out = NULL;
    *err_out = NULL;

    /* recovery_pass: NULL = "use the source passphrase"; "" = explicitly
     * NONE (Windows Hello only - the caller has warned the user); text =
     * that passphrase. */
    if (recovery_pass && !*recovery_pass)
        rpass = NULL;
    else
        rpass = recovery_pass ? recovery_pass : srcpass;
    if (!(recovery_pass && !*recovery_pass) && (!rpass || !*rpass)) {
        *err_out = dupstr("a recovery passphrase is required: the key's "
                          "own passphrase is empty, so it cannot serve as "
                          "one");
        return KAGEANT_HELLO_ERROR;
    }
    if (GetFileAttributesA(destpath) != INVALID_FILE_ATTRIBUTES) {
        *err_out = dupprintf("%s already exists - not overwriting it",
                             destpath);
        return KAGEANT_HELLO_ERROR;
    }
    if (!kageant_hello_offerable()) {
        *err_out = dupstr("Windows Hello key protection is not available "
                          "on this machine");
        return KAGEANT_HELLO_ERROR;
    }

    srcfn = filename_from_str(srcpath);
    key = ppk_load_f(srcfn, (srcpass && *srcpass) ? srcpass : NULL,
                     &loaderr);
    filename_free(srcfn);
    if (!key || key == SSH2_WRONG_PASSPHRASE) {
        *err_out = dupprintf("could not load %s: %s", srcpath,
                             key == SSH2_WRONG_PASSPHRASE ?
                             "wrong passphrase" :
                             (loaderr ? loaderr : "unknown error"));
        return KAGEANT_HELLO_ERROR;
    }

    if (!kitty_hello_new_secret(secret)) {
        *err_out = dupstr("the system random generator failed");
        goto out;
    }
    printed = kitty_hello_secret_text(secret);
    if (!printed) {
        *err_out = dupstr("out of memory");
        goto out;
    }

    /* The Hello door first - it is the step that can be refused, and
     * refusing must leave no file behind. */
    hret = kitty_hello_wrap_auto(owner, secret, rpass, &container, NULL);
    if (hret != KITTY_HELLO_VERIFIED || !container) {
        *err_out = dupstr(hret == KITTY_HELLO_DENIED ?
                          "the Windows Hello prompt was cancelled" :
                          hret == KITTY_HELLO_UNAVAILABLE ?
                          "Windows Hello key protection is not available" :
                          "wrapping the secret failed");
        ret = hret == KITTY_HELLO_DENIED ? KAGEANT_HELLO_DENIED
                                         : KAGEANT_HELLO_ERROR;
        goto out;
    }

    dstfn = filename_from_str(destpath);
    if (!ppk_save_f(dstfn, key, printed, &ppk_save_default_parameters)) {
        filename_free(dstfn);
        *err_out = dupprintf("could not write %s", destpath);
        goto out;
    }
    filename_free(dstfn);
    if (!kageant_hello_write_sidecar(destpath, container)) {
        char *sc = kageant_hello_sidecar_path(destpath);
        DeleteFileA(destpath);      /* no protected copy without its door */
        *err_out = dupprintf("could not write %s", sc);
        sfree(sc);
        goto out;
    }

    *printed_out = printed;
    printed = NULL;
    ret = KAGEANT_HELLO_OK;

  out:
    smemclr(secret, sizeof(secret));
    if (printed)
        burnstr(printed);
    if (container)
        burnstr(container);
    ssh_key_free(key->key);
    sfree(key->comment);
    sfree(key);
    return ret;
}

int kageant_hello_enrol(HWND owner, const char *keypath,
                        const char *passphrase, char **err_out)
{
    char *c, *c2 = NULL;
    unsigned char secret[KITTY_HELLO_SECRET_LEN];
    int hret, ret = KAGEANT_HELLO_ERROR;

    *err_out = NULL;
    if (kitty_hello_secret_from_text(passphrase, secret) != 1) {
        *err_out = dupstr("that is not this key's secret");
        return KAGEANT_HELLO_ERROR;
    }
    c = kageant_hello_read_sidecar(keypath);
    if (!c) {
        smemclr(secret, sizeof(secret));
        *err_out = dupstr("the key has no readable .hello sidecar");
        return KAGEANT_HELLO_ERROR;
    }
    hret = kitty_hello_enrol_auto(owner, c, secret, &c2);
    smemclr(secret, sizeof(secret));
    if (hret == KITTY_HELLO_VERIFIED && c2) {
        if (!strcmp(c, c2)) {
            ret = KAGEANT_HELLO_OK;          /* already enrolled */
        } else if (kageant_hello_write_sidecar(keypath, c2)) {
            ret = KAGEANT_HELLO_OK;
        } else {
            *err_out = dupstr("could not rewrite the .hello sidecar");
        }
    } else {
        *err_out = dupstr(hret == KITTY_HELLO_DENIED ?
                          "the Windows Hello prompt was cancelled" :
                          hret == KITTY_HELLO_UNAVAILABLE ?
                          "Windows Hello (passkey) protection is not "
                          "available on this machine" :
                          "adding this account's Hello door failed");
        ret = hret == KITTY_HELLO_DENIED ? KAGEANT_HELLO_DENIED
                                         : KAGEANT_HELLO_ERROR;
    }
    sfree(c);
    if (c2)
        burnstr(c2);
    return ret;
}
