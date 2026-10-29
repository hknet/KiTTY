/*
 * kitty_kageant_rules.c - kageant's file of destination rules: where it
 * is, DPAPI, loading it in full at the start, writing it in full, and what
 * a damaged file does (kitty_kageant_rules.h). The store and its bytes are
 * kitty_agentrestrict.c's.
 *
 * The file: the 8 bytes "KTRULES1", then CryptProtectData's output for the
 * Windows account (no extra entropy, no UI) over kar_store_encode()'s
 * bytes, which end in their own SHA-256. Written through
 * kitty_write_file_whole: a temporary file in the same folder, renamed over
 * the old one once complete.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include <wincrypt.h>

#include "kitty_agentrestrict.h"
#include "kitty_kageant_rules.h"
#include "kitty_atomicfile.h"   /* kitty_write_file_whole */
#include "kitty_inifile.h"      /* the store in charge, readonly=yes */
#include "kitty_auditlog.h"     /* kitty_audit: event 104 store-damaged */
#include "kitty_text.h"         /* KT_KARULES_* */
#include "kitty_winerr.h"       /* kitty_win_reason */

#define RULES_MAGIC "KTRULES1"
#define RULES_MAGIC_LEN 8
#define RULES_MAX_FILE (64u * 1024 * 1024)

static char *rules_damage = NULL;   /* why the store is damaged */

/* An environment variable in full, NULL when not set or empty. sfree it. */
static char *rules_env_dup(const char *name)
{
    DWORD need = GetEnvironmentVariableA(name, NULL, 0), n;
    for (int tries = 0; need && tries < 4; tries++) {
        char *buf = snewn(need, char);
        n = GetEnvironmentVariableA(name, buf, need);
        if (n > 0 && n < need)
            return buf;
        sfree(buf);
        need = n;
    }
    return NULL;
}

/* The folder part of a path (up to the last \ or /), or NULL. sfree it. */
static char *rules_dir_of(const char *path)
{
    const char *bs = strrchr(path, '\\'), *fs = strrchr(path, '/');
    const char *sl = (bs && fs) ? (bs > fs ? bs : fs) : (bs ? bs : fs);
    return sl ? dupprintf("%.*s", (int)(sl - path), path) : NULL;
}

char *kageant_rules_path_dup(void)
{
    const char *ini;
    char *base, *out;

    /* A portable install keeps it beside its kitty.ini, as the agent log
     * (kageant_audit_default_path_dup): the whole install stays on the
     * stick. */
    if (!kitty_inifile_registry_authoritative() &&
        (ini = kitty_inifile_file()) != NULL) {
        char *dir = rules_dir_of(ini);
        if (dir) {
            out = dupprintf("%s\\kageant-rules.dat", dir);
            sfree(dir);
            return out;
        }
    }
    if ((base = rules_env_dup("APPDATA")) == NULL)
        return NULL;
    out = dupprintf("%s\\KiTTY\\kageant-rules.dat", base);
    sfree(base);
    return out;
}

const char *kageant_rules_damage_text(void)
{
    return kar_store_get()->damaged ? rules_damage : NULL;
}

/* The store is damaged: empty, marked, the reason kept and recorded. */
static int rules_damaged(char *why)
{
    kar_store *st = kar_store_get();
    kar_store_clear(st);
    st->damaged = true;
    sfree(rules_damage);
    rules_damage = why;
    kitty_audit("restricted", "result", "store-damaged", "reason", why,
                (const char *)NULL);
    return 0;
}

int kageant_rules_load(void)
{
    char *path = kageant_rules_path_dup();
    HANDLE h;
    LARGE_INTEGER size;
    unsigned char *buf;
    DWORD got = 0, err;
    DATA_BLOB in, out;
    const char *derr;
    bool ok;

    if (!path)
        return 1;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            sfree(path);
            return 1;                   /* no rules yet */
        }
        {
            char *why = dupprintf(KT_KARULES_DMG_READ_FMT, path,
                                  kitty_win_reason(err));
            sfree(path);
            return rules_damaged(why);
        }
    }
    if (!GetFileSizeEx(h, &size) || size.QuadPart > RULES_MAX_FILE ||
        size.QuadPart < RULES_MAGIC_LEN) {
        char *why = dupprintf(KT_KARULES_DMG_FORMAT_FMT, path);
        CloseHandle(h);
        sfree(path);
        return rules_damaged(why);
    }
    buf = snewn((size_t)size.QuadPart, unsigned char);
    ok = ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) &&
        got == (DWORD)size.QuadPart;
    err = GetLastError();
    CloseHandle(h);
    if (!ok) {
        char *why = dupprintf(KT_KARULES_DMG_READ_FMT, path,
                              kitty_win_reason(err));
        sfree(buf);
        sfree(path);
        return rules_damaged(why);
    }
    if (memcmp(buf, RULES_MAGIC, RULES_MAGIC_LEN)) {
        char *why = dupprintf(KT_KARULES_DMG_FORMAT_FMT, path);
        sfree(buf);
        sfree(path);
        return rules_damaged(why);
    }
    in.pbData = buf + RULES_MAGIC_LEN;
    in.cbData = got - RULES_MAGIC_LEN;
    out.pbData = NULL;
    out.cbData = 0;
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        char *why;
        err = GetLastError();
        why = dupprintf(KT_KARULES_DMG_DECRYPT_FMT, path,
                        kitty_win_reason(err));
        sfree(buf);
        sfree(path);
        return rules_damaged(why);
    }
    sfree(buf);
    ok = kar_store_decode(kar_store_get(),
                          make_ptrlen(out.pbData, out.cbData), &derr);
    smemclr(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (!ok) {
        char *why = dupprintf(KT_KARULES_DMG_CONTENT_FMT, path,
                              derr ? derr : "");
        sfree(path);
        return rules_damaged(why);
    }
    sfree(path);
    sfree(rules_damage);
    rules_damage = NULL;
    return 1;
}

/* Write a store to the file. NULL when written, else the reason. */
static char *rules_write(const kar_store *s)
{
    char *path, *dir;
    strbuf *plain, *file;
    DATA_BLOB in, out;
    DWORD err;
    char *ret = NULL;

    if (kitty_store_readonly())
        return dupstr(KT_KARULES_ERR_READONLY);
    if ((path = kageant_rules_path_dup()) == NULL)
        return dupstr(KT_KARULES_ERR_NOPLACE);
    /* %APPDATA%\KiTTY may not exist yet; beside kitty.ini it does */
    if ((dir = rules_dir_of(path)) != NULL) {
        CreateDirectoryA(dir, NULL);
        sfree(dir);
    }
    plain = strbuf_new_nm();
    kar_store_encode(s, plain);
    in.pbData = (BYTE *)plain->u;
    in.cbData = (DWORD)plain->len;
    out.pbData = NULL;
    out.cbData = 0;
    if (!CryptProtectData(&in, L"KiTTY++ kageant destination rules", NULL,
                          NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        err = GetLastError();
        strbuf_free(plain);
        ret = dupprintf(KT_KARULES_ERR_DPAPI_FMT, kitty_win_reason(err));
        sfree(path);
        return ret;
    }
    strbuf_free(plain);
    file = strbuf_new();
    put_data(file, RULES_MAGIC, RULES_MAGIC_LEN);
    put_data(file, out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (!kitty_write_file_whole(path, file->u, file->len)) {
        err = GetLastError();
        ret = dupprintf(KT_KARULES_ERR_WRITE_FMT, path, kitty_win_reason(err));
    }
    strbuf_free(file);
    sfree(path);
    return ret;
}

char *kageant_rules_save(void)
{
    kar_store *st = kar_store_get();
    if (st->damaged)
        return dupstr(KT_KARULES_ERR_DAMAGED);
    return rules_write(st);
}

char *kageant_rules_replace(const kar_store *src, char **aside,
                            bool *applied)
{
    kar_store *st = kar_store_get(), *t;
    char *err, *path = NULL;

    *aside = NULL;
    *applied = false;
    t = kar_store_new();
    kar_store_copy(t, src);
    t->damaged = false;
    kar_store_tidy(t);

    if (st->damaged && !kitty_store_readonly()) {
        path = kageant_rules_path_dup();
        if (path && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            SYSTEMTIME tm;
            char *to;
            GetLocalTime(&tm);
            to = dupprintf("%s.damaged-%04u%02u%02u-%02u%02u%02u", path,
                           tm.wYear, tm.wMonth, tm.wDay, tm.wHour,
                           tm.wMinute, tm.wSecond);
            if (!MoveFileExA(path, to, MOVEFILE_WRITE_THROUGH)) {
                DWORD e = GetLastError();
                err = dupprintf(KT_KARULES_ERR_ASIDE_FMT, path, to,
                                kitty_win_reason(e));
                sfree(to);
                sfree(path);
                kar_store_free(t);
                return err;
            }
            *aside = to;
        }
    }

    err = rules_write(t);
    if (err && *aside) {
        /* Not written: the damaged file goes back where it was, so the
         * next start finds it damaged again - never an empty file that
         * would lift every restriction. */
        if (MoveFileExA(*aside, path, MOVEFILE_WRITE_THROUGH)) {
            sfree(*aside);
            *aside = NULL;
        }
    }
    sfree(path);
    if (!err || kitty_store_readonly()) {
        /* written, or readonly=yes: in force for the running agent */
        kar_store_copy(st, t);
        st->damaged = false;
        sfree(rules_damage);
        rules_damage = NULL;
        *applied = true;
    }
    kar_store_free(t);
    return err;
}

bool kageant_rules_key_restricted(ptrlen blob)
{
    kar_store *st = kar_store_get();
    char *fp;
    bool ret;
    if (st->damaged)
        return true;
    if (!st->nlinks || !blob.len)
        return false;
    fp = kar_key_fp(blob);
    ret = kar_store_restricted(st, fp);
    sfree(fp);
    return ret;
}

char *kageant_rules_text_for_fp(const char *fp)
{
    kar_rules *r;
    char *t;
    if (!fp || !*fp)
        return NULL;
    r = kar_store_rules(kar_store_get(), fp);
    if (!r)
        return NULL;
    t = kar_rules_text(r);
    kar_rules_free(r);
    return t;
}
