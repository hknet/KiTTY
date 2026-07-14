/*
 * KiTTY storage at-rest crypto round-trip regression test.
 *
 * Exercises the real settings API (open/write/read/close/del) through both
 * backends so the behaviour can be compared before/after storage.c
 * refactoring:
 *   - registry backend: Password is stored DPAPI1-wrapped, round-trips to
 *     plaintext, unmarked legacy values pass through verbatim;
 *   - portable file backend: Password is stored MPW2-wrapped under the master
 *     password, wrong-passphrase reads come back empty and the never-wipe
 *     guard re-persists the original blob on the next save.
 *
 * Touches only a zz-* scratch session in the live hive (deleted afterwards)
 * and a private %TEMP% portable tree (deleted afterwards). The portable test
 * seeds its own Security\ salt+verifier so it never falls through to the real
 * hive's MPW state (mpw_state_get reads the registry when the portable file
 * is absent).
 */

#include <stdio.h>
#include <string.h>

#include "putty.h"   /* pulls in windows.h (winsock2-first) via platform.h */
#include "storage.h"
#include "../kitty/kitty_b64.h"
#include "../kitty/kitty_mpw.h"

/* KiTTY storage API surface (fork style: declared extern by callers) */
void kitty_set_storage_mode(int mode);
void kitty_set_session_dir(const char *dir);
void kitty_set_master_passphrase(const char *pass);
const char *kitty_registry_base(void);
char *kitty_secret_wrap_current_backend(const char *plaintext);
int kitty_secret_unwrap(const char *stored, char **out);

/* app-level error sinks the linked objects expect (console-app style) */
void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(2);
}
void nonfatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "nonfatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

/* ---------- registry backend (DPAPI) ---------- */

#define RSESS   "zz-kitty-selftest-storage"
#define RSECRET "s3cretRegistry123"

static void test_registry(void)
{
    char *err = NULL;
    settings_w *w = open_settings_w(RSESS, &err);
    check(w != NULL, "registry: open_settings_w");
    if (!w) return;
    write_setting_s(w, "HostName", "selftest.example");
    write_setting_s(w, "Password", RSECRET);
    close_settings_w(w);

    char path[600];
    snprintf(path, sizeof(path), "%s\\Sessions\\%s",
             kitty_registry_base(), RSESS);
    char raw[4096]; DWORD sz = sizeof(raw);
    LONG rc = RegGetValueA(HKEY_CURRENT_USER, path, "Password",
                           RRF_RT_REG_SZ, NULL, raw, &sz);
    check(rc == ERROR_SUCCESS, "registry: stored Password value exists");
    check(rc == ERROR_SUCCESS && strncmp(raw, "DPAPI1:", 7) == 0,
          "registry: stored form is DPAPI1-wrapped");
    check(rc == ERROR_SUCCESS && strstr(raw, RSECRET) == NULL,
          "registry: stored form is not plaintext");

    settings_r *r = open_settings_r(RSESS);
    check(r != NULL, "registry: open_settings_r");
    if (r) {
        char *pw = read_setting_s(r, "Password");
        check(pw && !strcmp(pw, RSECRET), "registry: password round-trips");
        if (pw) sfree(pw);
        char *hn = read_setting_s(r, "HostName");
        check(hn && !strcmp(hn, "selftest.example"),
              "registry: hostname round-trips");
        if (hn) sfree(hn);
        close_settings_r(r);
    }

    /* unmarked (legacy) stored value must pass through verbatim */
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &hk, NULL) == ERROR_SUCCESS) {
        static const char legacy[] = "legacy-plain-pw";
        RegSetValueExA(hk, "Password", 0, REG_SZ,
                       (const BYTE *)legacy, sizeof(legacy));
        RegCloseKey(hk);
        r = open_settings_r(RSESS);
        char *pw = r ? read_setting_s(r, "Password") : NULL;
        check(pw && !strcmp(pw, legacy),
              "registry: unmarked legacy value passes through verbatim");
        if (pw) sfree(pw);
        if (r) close_settings_r(r);
    }

    del_settings(RSESS);
    sz = sizeof(raw);
    rc = RegGetValueA(HKEY_CURRENT_USER, path, "Password",
                      RRF_RT_REG_SZ, NULL, raw, &sz);
    check(rc != ERROR_SUCCESS, "registry: del_settings removed scratch session");
}

/* ---------- portable file backend (master password / MPW2) ---------- */

#define PSESS   "zz-portable-selftest"
#define PSECRET "s3cretPortable123"
#define PPASS   "selftest-master-pw"

static int write_text(const char *dir, const char *name, const char *val)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    fputs(val, fp);
    fputc('\n', fp);
    return fclose(fp) == 0;
}

static char *read_whole_file(const char *path)   /* malloc'd or NULL */
{
    FILE *fp = fopen(path, "rb");
    long len;
    char *buf = NULL;
    if (!fp) return NULL;
    if (!fseek(fp, 0, SEEK_END) && (len = ftell(fp)) >= 0 &&
        !fseek(fp, 0, SEEK_SET)) {
        buf = malloc(len + 1);
        if (buf) {
            if (fread(buf, 1, len, fp) == (size_t)len) buf[len] = '\0';
            else { free(buf); buf = NULL; }
        }
    }
    fclose(fp);
    return buf;
}

static void test_portable(void)
{
    char tmp[MAX_PATH], root[MAX_PATH], sess[MAX_PATH], sec[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    snprintf(root, sizeof(root), "%skitty-selftest-%lu",
             tmp, (unsigned long)GetCurrentProcessId());
    snprintf(sess, sizeof(sess), "%s\\Sessions", root);
    snprintf(sec, sizeof(sec), "%s\\Security", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(sess, NULL);
    CreateDirectoryA(sec, NULL);

    /* Seed Security\ deterministically with the same primitives the store
     * uses, so this run is independent of the machine's real MPW state. */
    unsigned char salt[KITTY_MPW_SALT_LEN];
    for (int i = 0; i < KITTY_MPW_SALT_LEN; i++)
        salt[i] = (unsigned char)(0x40 + i);
    unsigned char key[KITTY_MPW_DERIVED_LEN];
    kitty_mpw_derive(PPASS, salt, sizeof(salt), key);
    char *ver = kitty_mpw_protect("KiTTY-MPW-verify", key);
    char *saltb64 = ksec_b64_encode(salt, sizeof(salt));
    check(ver && saltb64, "portable: seed salt+verifier built");
    if (!ver || !saltb64) return;
    check(write_text(sec, "MasterPwSalt", saltb64) &&
          write_text(sec, "MasterPwVerifier", ver),
          "portable: Security store seeded");
    free(saltb64);
    sfree(ver);

    kitty_set_session_dir(sess);
    kitty_set_storage_mode(1);
    kitty_set_master_passphrase(PPASS);

    char *err = NULL;
    settings_w *w = open_settings_w(PSESS, &err);
    check(w != NULL, "portable: open_settings_w");
    if (!w) { kitty_set_storage_mode(0); return; }
    write_setting_s(w, "HostName", "portable.example");
    write_setting_s(w, "Password", PSECRET);
    close_settings_w(w);

    char fpath[MAX_PATH];
    snprintf(fpath, sizeof(fpath), "%s\\%s", sess, PSESS);
    char *fbuf = read_whole_file(fpath);
    check(fbuf != NULL, "portable: session file written");
    check(fbuf && strstr(fbuf, "MPW2") != NULL,
          "portable: stored form is MPW2-wrapped");
    check(fbuf && strstr(fbuf, PSECRET) == NULL,
          "portable: stored form is not plaintext");
    if (fbuf) free(fbuf);

    /* Wrong master password: reads back empty, never plaintext/garbage.
     * MUST run before any successful MPW2 read in this process: a successful
     * decrypt primes the foreign-key cache (g_mpw_fkey), which deliberately
     * survives kitty_set_master_passphrase and would keep decrypting. */
    kitty_set_master_passphrase("wrong-master-pw");
    settings_r *r = open_settings_r(PSESS);
    check(r != NULL, "portable: open_settings_r");
    if (r) {
        char *pw = read_setting_s(r, "Password");
        check(pw && !pw[0], "portable: wrong master pw reads back empty");
        if (pw) sfree(pw);
        close_settings_r(r);
    }

    /* never-wipe: saving the (empty) in-memory value after a failed decrypt
     * must re-persist the original blob, not clobber it */
    w = open_settings_w(PSESS, &err);
    if (w) {
        write_setting_s(w, "Password", "");
        close_settings_w(w);
    }
    fbuf = read_whole_file(fpath);
    check(fbuf && strstr(fbuf, "MPW2") != NULL,
          "portable: never-wipe kept the undecryptable blob");
    if (fbuf) free(fbuf);

    /* right password: fresh re-derive + verify against the seeded verifier,
     * and proves the never-wipe save above preserved a decryptable blob */
    kitty_set_master_passphrase(PPASS);
    r = open_settings_r(PSESS);
    if (r) {
        char *pw = read_setting_s(r, "Password");
        check(pw && !strcmp(pw, PSECRET),
              "portable: password round-trips after never-wipe save");
        if (pw) sfree(pw);
        close_settings_r(r);
    }

    /* the .ktx / named-proxy wrap helpers use the same backend policy */
    char *blob = kitty_secret_wrap_current_backend("ktx-secret-42");
    char *out = NULL;
    int rv = blob ? kitty_secret_unwrap(blob, &out) : 0;
    check(rv == 1 && out && !strcmp(out, "ktx-secret-42"),
          "portable: wrap/unwrap helper round-trips");
    if (blob) free(blob);
    if (out) free(out);

    kitty_set_storage_mode(0);

    /* cleanup the temp tree (known file names only) */
    DeleteFileA(fpath);
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\MasterPwSalt", sec);     DeleteFileA(p);
    snprintf(p, sizeof(p), "%s\\MasterPwVerifier", sec); DeleteFileA(p);
    RemoveDirectoryA(sess);
    RemoveDirectoryA(sec);
    RemoveDirectoryA(root);
}

int main(void)
{
    printf("== registry backend (DPAPI) ==\n");
    test_registry();
    printf("== portable backend (master password) ==\n");
    test_portable();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
