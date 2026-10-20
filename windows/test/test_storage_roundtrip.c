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
#include "../kitty/kitty_oldwin_reg.h"   /* XP: post-XP APIs via oldwin */
#include "../kitty/kitty_pwmem.h"        /* passwords wrapped in memory */
#include "../kitty/kitty_storage.h"
#include "../kitty/kitty_secretstore.h"
#include "../kitty/kitty.h"

/* KiTTY storage API surface (fork style: declared extern by callers) */

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

/*
 * Stubs for the .ktx reader's environment (design doc 8.17).
 *
 * kitty_settings_load.c is application code and reaches for the registry, the ini
 * file and the backend table. None of that is on the path a .ktx parse takes, so
 * these stand in for it - but each one matches the REAL declaration exactly,
 * because a stub with a convenient signature links happily and then lies about
 * what the tested code does.
 *
 * If any of these ever starts being called for real by this test, it should fail
 * loudly rather than return something plausible; that is why the two lookups
 * return "not found" rather than an empty string.
 */

const struct BackendVtable *const backends[] = { NULL };
const int be_default_protocol = 0;
const struct keyvalwhere gsslibkeywords[] = { { "", 0, -1, -1 } };
const int ngsslibs = 0;
Conf *conf = NULL;                  /* the app's active-seat global */
bool conf_launchable(Conf *c) { return true; }
char *get_username(void) { return dupstr("selftest"); }
void burnwcs(wchar_t *s) { if (s) { while (*s) *s++ = 0; } }
char *GetValueData(HKEY k, const char *sub, const char *name, char *out)
{ return NULL; }                    /* "no such registry value" */
int readINI(const char *f, const char *sec, const char *key, char *p, size_t n)
{ return 0; }                       /* "no such ini key" */
/* Deliberately LOUD rather than silent. The .ktx test fixture is not encrypted, so
 * this should never fire - but if someone later adds an encrypted fixture, a quiet
 * stub would swallow the deprecation notice and the test would pass while telling
 * nobody. Printing means it shows up in the run. */
void KittyCliReport(const char *title, const char *text, int warn)
{ printf("  [KittyCliReport] %s\n", title ? title : "(no title)"); }

char *str_rtrim(char *s, const char *set)
{
    size_t n = s ? strlen(s) : 0;
    while (n > 0 && strchr(set, s[n - 1])) s[--n] = '\0';
    return s;
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

/* The "Password=..." line of a session file's text, malloc'd, or NULL. */
static char *password_line(const char *buf)
{
    const char *p = buf;
    while (p && *p) {
        if (!strncmp(p, "Password=", 9)) {
            size_t n = strcspn(p, "\r\n");
            char *l = malloc(n + 1);
            if (l) { memcpy(l, p, n); l[n] = '\0'; }
            return l;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
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

    /* A CHANGED password saved while the master password is locked (the
     * supplied one is wrong): the stored line stays as it was, and is never
     * replaced by a DPAPI1 value (it used to be: the save fell back to DPAPI
     * and changed the password's protection without a word). */
    {
        char *before = fbuf ? password_line(fbuf) : NULL;
        char *after = NULL, *fb2;
        w = open_settings_w(PSESS, &err);
        if (w) {
            write_setting_s(w, "Password", "changed-while-locked");
            close_settings_w(w);
        }
        fb2 = read_whole_file(fpath);
        if (fb2) after = password_line(fb2);
        check(before && after && !strcmp(before, after),
              "portable: a password changed while locked keeps the stored line");
        check(fb2 && !strstr(fb2, "DPAPI1:") && !strstr(fb2, "changed-while-locked"),
              "portable: ...and is neither DPAPI1 nor plain");
        free(before); free(after); free(fb2);
    }
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

    /* The unlocked key is kept wrapped in memory (CryptProtectMemory, this
     * process) and unwrapped for each use: every use opens what the others
     * wrapped. */
    {
        static const char *const vals[] = { "k-one", "k-two-longer", "k3" };
        int good = 1;
        for (int i = 0; i < 3; i++) {
            char *b = kitty_secret_wrap_current_backend(vals[i]);
            char *o = NULL;
            int r2 = b ? kitty_secret_unwrap(b, &o) : 0;
            if (!(r2 == 1 && o && !strcmp(o, vals[i]))) good = 0;
            free(b); free(o);
        }
        check(good, "portable: the key kept wrapped in memory opens every value it wrapped");
    }
    /* The hand-off to a child (the key of a running window to the window it
     * starts) carries the derived key itself, wrapped for this logon - not
     * the in-process wrapping, which the child could not open. */
    {
        char tok[80];
        HANDLE fm = kitty_mpw_export_inherit_blob("", tok, sizeof(tok));
        int same = 0;
        check(fm != NULL, "portable: the unlocked key is handed off");
        if (fm) {
            struct {
                unsigned magic, version, wrapped;
                unsigned char salt[KITTY_MPW_SALT_LEN];
                unsigned char key[KITTY_MPW_DERIVED_LEN];
            } hb;
            void *v = MapViewOfFile(fm, FILE_MAP_READ, 0, 0, sizeof(hb));
            if (v) {
                memcpy(&hb, v, sizeof(hb));
                UnmapViewOfFile(v);
                if (hb.wrapped) {
                    HMODULE c = LoadLibraryA("crypt32.dll");
                    BOOL (WINAPI *unprot)(LPVOID, DWORD, DWORD) = c
                        ? (BOOL (WINAPI *)(LPVOID, DWORD, DWORD))
                              GetProcAddress(c, "CryptUnprotectMemory")
                        : NULL;
                    if (!unprot || !unprot(hb.key, sizeof(hb.key), 0x02))   /* SAME_LOGON */
                        hb.wrapped = 2;
                }
                same = hb.wrapped != 2 && !memcmp(hb.key, key, sizeof(key)) &&
                       !memcmp(hb.salt, salt, sizeof(salt));
                SecureZeroMemory(&hb, sizeof(hb));
            }
            CloseHandle(fm);
        }
        check(same, "portable: the handed-off key is the derived key");
    }
    SecureZeroMemory(key, sizeof(key));

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

/* ---------- long settings values (cyd01/KiTTY#541) ----------
 *
 * That report is a session whose Tunnels list covers a whole network - ssh,
 * rdp and vnc per host - which KiTTY then loses: the list comes back blank.
 * PortForwardings is stored as ONE value holding the entire map, so a /24
 * times three protocols is about 19 KB in a single setting. Anything on the
 * path that assumes a "reasonable" line or value length silently truncates
 * it, and a truncated map reads back as no forwardings at all.
 *
 * Both backends are checked with exactly that shape. */
#define LSESS "zz-kitty-selftest-longvalue"

static char *build_big_portfwd(int hosts, int *entries_out)
{
    strbuf *sb = strbuf_new();
    int n = 0;
    for (int h = 1; h <= hosts; h++) {
        put_fmt(sb, "%sL%d=192.168.7.%d:22", n ? "," : "", 10000 + n, h); n++;
        put_fmt(sb, ",L%d=192.168.7.%d:3389", 10000 + n, h); n++;
        put_fmt(sb, ",L%d=192.168.7.%d:5900", 10000 + n, h); n++;
    }
    if (entries_out) *entries_out = n;
    return strbuf_to_str(sb);
}

/* The Proxies folder of the folder store is in the configuration directory
 * (kitty.ini configdir), not in the store root, when the two part - the
 * place kitty_proxy.c reads and writes. The master-password scan went to
 * the store root and missed MPW values there. */
static void test_proxies_dir(void)
{
    char *d;
    kitty_set_session_dir("C:\\qa-store\\elsewhere\\mysess");
    kitty_set_storage_mode(1);
    kitty_set_config_dir("C:\\qa-store\\cfg");
    d = portable_proxies_dir();
    check(d && !strcmp(d, "C:\\qa-store\\cfg\\Proxies"),
          "proxies dir: in the configuration directory when sessions= points elsewhere");
    sfree(d);
    d = portable_config_subdir("Launcher");
    check(d && !strcmp(d, "C:\\qa-store\\cfg\\Launcher"),
          "launcher dir: in the configuration directory, where the launcher writes it");
    sfree(d);
    kitty_set_config_dir("");
    d = portable_proxies_dir();
    check(d && !strcmp(d, "C:\\qa-store\\elsewhere\\mysess\\Proxies"),
          "proxies dir: the store root when no configuration directory is set (tools)");
    sfree(d);
    kitty_set_storage_mode(0);
    kitty_set_session_dir("");
}

static void test_long_values(void)
{
    int entries = 0;
    char *big = build_big_portfwd(254, &entries);
    char *err = NULL;
    char what[160];

    snprintf(what, sizeof(what),
             "long value is %d entries / %d bytes (a /24 of ssh+rdp+vnc)",
             entries, (int)strlen(big));
    check(strlen(big) > 8192, what);

    /* registry backend */
    settings_w *w = open_settings_w(LSESS, &err);
    check(w != NULL, "registry: open_settings_w (long value)");
    if (w) {
        write_setting_s(w, "PortForwardings", big);
        close_settings_w(w);
        settings_r *r = open_settings_r(LSESS);
        char *got = r ? read_setting_s(r, "PortForwardings") : NULL;
        check(got && !strcmp(got, big),
              "registry: full PortForwardings map round-trips");
        if (got && strcmp(got, big))
            printf("      (wrote %d bytes, read back %d)\n",
                   (int)strlen(big), (int)strlen(got));
        if (got) sfree(got);
        if (r) close_settings_r(r);
        del_settings(LSESS);
    }

    /* portable file backend, in its own temp tree */
    {
        char tmp[MAX_PATH], root[MAX_PATH], sess[MAX_PATH], fpath[MAX_PATH];
        GetTempPathA(sizeof(tmp), tmp);
        snprintf(root, sizeof(root), "%skitty-selftest-long-%lu",
                 tmp, (unsigned long)GetCurrentProcessId());
        snprintf(sess, sizeof(sess), "%s\\Sessions", root);
        CreateDirectoryA(root, NULL);
        CreateDirectoryA(sess, NULL);
        kitty_set_session_dir(sess);
        kitty_set_storage_mode(1);

        settings_w *pw = open_settings_w(LSESS, &err);
        check(pw != NULL, "portable: open_settings_w (long value)");
        if (pw) {
            write_setting_s(pw, "PortForwardings", big);
            close_settings_w(pw);
            settings_r *pr = open_settings_r(LSESS);
            char *got = pr ? read_setting_s(pr, "PortForwardings") : NULL;
            check(got && !strcmp(got, big),
                  "portable: full PortForwardings map round-trips");
            if (got && strcmp(got, big))
                printf("      (wrote %d bytes, read back %d - TRUNCATED)\n",
                       (int)strlen(big), (int)strlen(got));
            if (got) sfree(got);
            if (pr) close_settings_r(pr);
        }
        kitty_set_storage_mode(0);
        snprintf(fpath, sizeof(fpath), "%s\\%s", sess, LSESS);
        DeleteFileA(fpath);
        RemoveDirectoryA(sess);
        RemoveDirectoryA(root);
    }
    sfree(big);
}

/* ---------- renamed-setting migration (SaveWindowPos -> SetWindowPos) ----
 *
 * The old name must still be READ, and must be GONE once the session is saved
 * again - migrating on save, never on load. Checked on the registry backend,
 * which is the one that keeps values nobody rewrote. */
#define MSESS "zz-kitty-selftest-rename"

static void test_renamed_key(void)
{
    char path[600];
    char *err = NULL;
    HKEY hk;
    DWORD v = 1, sz = sizeof(v), type = 0;

    snprintf(path, sizeof(path), "%s\\Sessions\\%s", kitty_registry_base(), MSESS);

    /* a session as an older KiTTY left it: only the legacy name */
    settings_w *w = open_settings_w(MSESS, &err);
    check(w != NULL, "rename: open_settings_w");
    if (!w) return;
    write_setting_s(w, "HostName", "rename.example");
    close_settings_w(w);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, "SaveWindowPos", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(hk);
    }
    check(RegGetValueA(HKEY_CURRENT_USER, path, "SaveWindowPos", RRF_RT_REG_DWORD,
                       &type, &v, &sz) == ERROR_SUCCESS,
          "rename: legacy SaveWindowPos seeded");

    /* Any later save of that session retires the legacy key. (This drives the
     * storage layer directly - the settings layer that maps CONF_set_windowpos
     * onto the new keyword lives in settings.c, which this test does not
     * link; the read-fallback half is exercised by loading a session in the
     * application.) */
    w = open_settings_w(MSESS, &err);
    check(w != NULL, "rename: reopen for save");
    if (w) {
        write_setting_s(w, "HostName", "rename.example");
        write_setting_i(w, "SetWindowPos", 1);
        close_settings_w(w);
    }
    sz = sizeof(v);
    check(RegGetValueA(HKEY_CURRENT_USER, path, "SetWindowPos", RRF_RT_REG_DWORD,
                       &type, &v, &sz) == ERROR_SUCCESS && v == 1,
          "rename: new SetWindowPos present after save");
    sz = sizeof(v);
    check(RegGetValueA(HKEY_CURRENT_USER, path, "SaveWindowPos", RRF_RT_REG_DWORD,
                       &type, &v, &sz) != ERROR_SUCCESS,
          "rename: legacy SaveWindowPos removed on save");

    del_settings(MSESS);
}

/* ---------- reading a session out of the OLD 9bis KiTTY hive (section 8.10) ----------
 *
 * Sessions written by classic KiTTY live under Software\9bis.com\KiTTY, and we
 * still read them: precedence is our own base, then that hive, then stock PuTTY's.
 * This was on the owed-live-tests list as "legacy read", but the DECODER is
 * already covered transitively - what was never checked is the PLUMBING, i.e. that
 * a session which exists ONLY in the old hive is found at all, and that its values
 * come back intact. That part needs no human, so it should not have been on a
 * hands-on list.
 *
 * Deliberately uses a cleartext password: the legacy-encrypted form is the same
 * decoder tested elsewhere, and mixing the two would test the decoder twice while
 * still not testing the lookup.
 */
#define OSESS   "zz-kitty-selftest-oldhive"
#define OSECRET "old-hive-plain-pw"

static void test_old_kitty_hive(void)
{
    char path[600];
    HKEY hk;
    settings_r *r;

    snprintf(path, sizeof(path), "Software\\9bis.com\\KiTTY\\Sessions\\%s", OSESS);

    /* make sure our own hive does NOT have it, or we would be testing that */
    del_settings(OSESS);

    if (RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &hk, NULL) != ERROR_SUCCESS) {
        check(0, "old hive: could not seed a session (skipped)");
        return;
    }
    RegSetValueExA(hk, "HostName", 0, REG_SZ,
                   (const BYTE *)"oldhive.example", 16);
    RegSetValueExA(hk, "Password", 0, REG_SZ,
                   (const BYTE *)OSECRET, sizeof(OSECRET));
    RegCloseKey(hk);

    /* Loading by name follows "show foreign sessions" (the list always did).
     * The switch is pinned in the base key by kitty_set_show_foreign_sessions,
     * so any existing value is saved here and put back at the end. */
    DWORD saved_show = 0, saved_sz = sizeof(saved_show);
    int had_show = (RegGetValueA(HKEY_CURRENT_USER, kitty_registry_base(),
                                 "ShowForeignSessions", RRF_RT_REG_DWORD, NULL,
                                 &saved_show, &saved_sz) == ERROR_SUCCESS);

    kitty_set_show_foreign_sessions(0);
    r = open_settings_r(OSESS);
    check(r == NULL, "old hive: with the old stores hidden, the session is NOT found by name");
    if (r) close_settings_r(r);

    kitty_set_show_foreign_sessions(1);
    r = open_settings_r(OSESS);
    check(r != NULL, "old hive: a session only in the 9bis hive is found");
    if (r) {
        char *hn = read_setting_s(r, "HostName");
        check(hn && !strcmp(hn, "oldhive.example"),
              "old hive: hostname reads back");
        if (hn) sfree(hn);
        char *pw = read_setting_s(r, "Password");
        check(pw && !strcmp(pw, OSECRET),
              "old hive: password reads back");
        if (pw) sfree(pw);
        close_settings_r(r);
    }

    /* our own hive must win when both exist - precedence, not merely fallback */
    {
        char *err = NULL;
        settings_w *w = open_settings_w(OSESS, &err);
        if (w) {
            write_setting_s(w, "HostName", "ourhive.example");
            close_settings_w(w);
        }
        r = open_settings_r(OSESS);
        char *hn = r ? read_setting_s(r, "HostName") : NULL;
        check(hn && !strcmp(hn, "ourhive.example"),
              "old hive: our own hive takes precedence over it");
        if (hn) sfree(hn);
        if (r) close_settings_r(r);
        del_settings(OSESS);
    }

    RegDeleteKeyA(HKEY_CURRENT_USER, path);

    /* put the previous switch value back (or remove ours) */
    {
        HKEY bk;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, kitty_registry_base(), 0,
                          KEY_SET_VALUE, &bk) == ERROR_SUCCESS) {
            if (had_show)
                RegSetValueExA(bk, "ShowForeignSessions", 0, REG_DWORD,
                               (const BYTE *)&saved_show, sizeof(saved_show));
            else
                RegDeleteValueA(bk, "ShowForeignSessions");
            RegCloseKey(bk);
        }
    }
}

/* ---------- reading an old .ktx export (section 8.17) ----------
 *
 * A .ktx is KiTTY's exported-session file: "Key\value\" lines, optionally with the
 * whole file encrypted. Same reasoning as the old-hive test above - the crypto is
 * covered elsewhere, the PARSER is what was never exercised - so this drives
 * load_open_settings_forced() over a file written by hand in the old shape, which
 * is exactly what an old KiTTY would have produced.
 */
#define KSESS_HOST "ktx.example"

static void test_old_ktx(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    FILE *fp;
    Conf *conf;

    if (!GetTempPathA(sizeof(dir), dir)) {
        check(0, "ktx: no temp dir (skipped)");
        return;
    }
    snprintf(path, sizeof(path), "%szz-kitty-selftest.ktx", dir);
    fp = fopen(path, "wb");
    if (!fp) {
        check(0, "ktx: could not write a scratch .ktx (skipped)");
        return;
    }
    /* the old shape, including a key we retired (SaveWindowPos) and one with an
     * escaped separator, both of which a real old export could contain */
    fprintf(fp, "HostName\\%s\\\n", KSESS_HOST);
    fprintf(fp, "PortNumber\\2222\\\n");
    fprintf(fp, "Password\\PLAIN:ktx-secret\\\n");
    fprintf(fp, "SaveWindowPos\\1\\\n");
    fprintf(fp, "TerminalType\\xterm\\\n");
    fclose(fp);

    /*
     * NOTE on the port, found while writing this: load_open_settings_forced()
     * reads PortNumber only INSIDE the "did Protocol name a backend we have?"
     * branch, so a .ktx whose Protocol line is missing - or whose protocol this
     * binary was not built with - loads with port 0 rather than a default. Real
     * exports always carry a Protocol line, so this is not a bug in practice, but
     * it does mean the port cannot be asserted in this harness: the backend table
     * here is a deliberate stub, so no protocol ever resolves. Asserting it would
     * mean linking the real backends to test a parser, which is the wrong trade.
     */

    conf = conf_new();
    do_defaults(NULL, conf);
    load_open_settings_forced(path, conf);

    check(!strcmp(conf_get_str(conf, CONF_host), KSESS_HOST),
          "ktx: hostname parsed from an old export");
    check(!strcmp(conf_get_str(conf, CONF_termtype), "xterm"),
          "ktx: terminal type parsed from an old export");
    /* PLAIN: means "this is the password, do not try to decode it" - the marker
     * that stopped cleartext in an imported .ktx being mangled by the legacy
     * decoder. Read through kitty_pw_get: the loader leaves the password
     * WRAPPED in the Conf (kitty/kitty_pwmem.c), so the raw string is a blob. */
    {
        char pw[KITTY_PW_MAX + 1];
        kitty_pw_get(conf, CONF_password, pw, sizeof(pw));
        check(!strcmp(pw, "ktx-secret"),
              "ktx: PLAIN: cleartext password imports verbatim");
        /* Where the protection is available the Conf must not hold the
         * password itself; where it is not, keeping it is the documented
         * fallback and there is nothing to assert. */
        if (kitty_pw_is_wrapped(conf_get_str(conf, CONF_password)))
            check(strcmp(conf_get_str(conf, CONF_password), "ktx-secret") != 0,
                  "ktx: the loaded password is wrapped, not left in the clear");
        smemclr(pw, sizeof(pw));
    }

    conf_free(conf);
    DeleteFileA(path);
}

/* ---------- the login script is protected like a password ----------
 *
 * CONF_scriptfilecontent holds the inlined login script, in the same session
 * record as CONF_password. It used to be scrambled with the constant compiled
 * into every build; it is now wrapped by the same at-rest protection the password
 * gets. What is checked here is the STORAGE property, not ReadInitScript's file
 * parsing: that the wrapped form survives a round trip, that it is not stored in
 * the clear, and - the part that matters for a two-phase migration - that a value
 * carrying NO marker is still returned verbatim so the legacy decoder downstream
 * can have it.
 *
 * base64 first is the reason this is worth a test at all: the script is
 * NUL-separated, and handing the raw blob to a chokepoint that takes C strings
 * would store only up to the first NUL and lose the rest in silence.
 */
#define SCRIPTSESS "zz-kitty-selftest-script"

static void test_script_protection(void)
{
    /* a NUL-separated blob, exactly the shape ScriptFileContent has:
     * "login:\0user\0password:\0s3cret\0\0" */
    static const char blob[] = "login:\0user\0password:\0s3cret\0";
    const int bloblen = (int)sizeof(blob);          /* includes the trailing NUL */
    char *b64, *wrapped, *err = NULL;

    b64 = ksec_b64_encode((const unsigned char *)blob, bloblen);
    check(b64 != NULL, "script: blob base64-encodes");
    if (!b64) return;
    /* base64 of a NUL-containing blob must be longer than the first segment -
     * catches the "silently truncated at the first NUL" failure directly */
    check(strlen(b64) > 12, "script: base64 covers the whole blob, not just the first string");

    wrapped = kitty_secret_wrap_current_backend(b64);
    check(wrapped != NULL, "script: wraps for the active backend");
    if (!wrapped) { sfree(b64); return; }
    check(strstr(wrapped, "s3cret") == NULL,
          "script: the wrapped form is not plaintext");

    /* store it in a real session and read it back the way KiTTY would */
    settings_w *w = open_settings_w(SCRIPTSESS, &err);
    if (w) {
        write_setting_s(w, "ScriptfileContent", wrapped);
        close_settings_w(w);
    }
    settings_r *r = open_settings_r(SCRIPTSESS);
    char *got = r ? read_setting_s(r, "ScriptfileContent") : NULL;
    if (r) close_settings_r(r);

    if (got) {
        char *plain = NULL;
        int rc = ksec_unprotect(got, &plain);
        check(rc > 0 && plain, "script: stored value unprotects");
        if (rc > 0 && plain) {
            int blen = 0;
            unsigned char *raw = ksec_b64_decode(plain, &blen);
            check(raw && blen == bloblen,
                  "script: round-trips to the original length, NULs and all");
            check(raw && blen == bloblen && !memcmp(raw, blob, bloblen),
                  "script: round-trips byte for byte");
            if (raw) sfree(raw);
        }
        if (plain) sfree(plain);
        sfree(got);
    } else {
        check(0, "script: could not read the value back");
    }

    /*
     * The trap that a login script must not fall into, pinned exactly.
     *
     * ksec_unprotect returns 1 for an UNMARKED value and hands it back verbatim -
     * "unmarked legacy == plaintext". So a caller that decides "protected or
     * legacy?" by looking at the RETURN VALUE gets 1 for both and sends every
     * pre-existing scrambled script down the base64 path, where it decodes to
     * rubbish. The right question is whether the value carries a marker, which is
     * what ksec_stored_is_legacy answers.
     *
     * Asserted as an equality, not an "either is fine": the first version of this
     * test accepted rc <= 0 OR verbatim, which passed while the caller was wrong.
     */
    {
        static const char legacy[] = "some-legacy-scrambled-value";
        char *plain = NULL;
        int rc = ksec_unprotect(legacy, &plain);
        check(rc == 1 && plain && !strcmp(plain, legacy),
              "script: an unmarked value comes back VERBATIM with rc==1");
        check(ksec_stored_is_legacy(legacy) == 1,
              "script: ...and is identified as legacy by the marker test");
        check(ksec_stored_is_legacy(wrapped) == 0,
              "script: a wrapped value is NOT identified as legacy");
        if (plain) sfree(plain);
    }

    del_settings(SCRIPTSESS);
    sfree(wrapped);
    sfree(b64);
}

/* ---------- the login script's readable form ----------
 *
 * The config box shows the script as one entry per line and stores it as a
 * NUL-separated blob. This is where a mistake silently eats somebody's login
 * script, which is why the conversion lives apart from the crypto that wraps it -
 * so it can be tested at all.
 */
static void test_script_lines(void)
{
    static const unsigned char blob[] = "login:\0user\0password:\0s3cret\0";
    const int bloblen = (int)sizeof(blob);
    char *text;
    unsigned char *back;
    int backlen = 0;

    text = kitty_loginscript_blob_to_lines(blob, bloblen);
    check(text && !strcmp(text, "login:\r\nuser\r\npassword:\r\ns3cret"),
          "lines: blob shows as one entry per line");

    back = kitty_loginscript_lines_to_blob(text ? text : "", &backlen);
    check(back && backlen == bloblen && !memcmp(back, blob, bloblen),
          "lines: and converts back byte for byte");
    if (back) sfree(back);
    if (text) sfree(text);

    /* Blank lines are dropped rather than becoming an entry. An empty wait-for
     * string would match ANY output, firing the next send-text immediately -
     * so a stray blank line in the box must not become one. */
    back = kitty_loginscript_lines_to_blob("a\r\n\r\n\r\nb\r\n", &backlen);
    check(back && backlen == 5 && !memcmp(back, "a\0b\0", 5),
          "lines: blank lines are dropped, not turned into match-anything entries");
    if (back) sfree(back);

    /* Bare LF as well as CRLF - anything pasted from a Unix-side file. */
    back = kitty_loginscript_lines_to_blob("a\nb", &backlen);
    check(back && backlen == 5 && !memcmp(back, "a\0b\0", 5),
          "lines: bare LF is accepted too");
    if (back) sfree(back);

    /* Empty input must produce a well-formed empty blob, not a null one. */
    back = kitty_loginscript_lines_to_blob("", &backlen);
    check(back && backlen == 1 && back[0] == 0,
          "lines: empty text gives a terminated empty blob");
    if (back) sfree(back);

    /* A blob with no terminator must not be walked off the end. */
    {
        static const unsigned char trunc[] = { 'a','b','c' };
        char *t = kitty_loginscript_blob_to_lines(trunc, (int)sizeof(trunc));
        check(t && !strcmp(t, "abc"),
              "lines: an unterminated blob is read to its length, not past it");
        if (t) sfree(t);
    }
}

/* ---------- save only non-default settings ----------
 *
 * kitty.ini saveonlynondefaultsettings: with Inherit Default Settings on, a
 * session save writes only what differs from Default Settings, and "Comment
 * out defaults" does that once for every session. The folder store runs in a
 * %TEMP% tree with its own Default Settings; the registry leg uses zz-*
 * scratch keys, a scratch session standing in for Default Settings (the real
 * one is never written), and the button's per-session step on that key only
 * (the whole-store walk would reach the real sessions). */
#define NDDEF  "zz-kitty-selftest-nd-defaults"
#define NDSESS "zz-kitty-selftest-nd"

static int nd_write(const char *dir, const char *name, const char *text)
{
    char path[MAX_PATH];
    FILE *fp;
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    fp = fopen(path, "wb");
    if (!fp) return 0;
    fputs(text, fp);
    return fclose(fp) == 0;
}

static int nd_file_is(const char *dir, const char *name, const char *want)
{
    char path[MAX_PATH];
    char *got;
    int ok;
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    got = read_whole_file(path);
    ok = got && !strcmp(got, want);
    if (!ok)
        printf("      got:\n%s      want:\n%s", got ? got : "(none)\n", want);
    free(got);
    return ok;
}

static void test_nondefault_file(void)
{
    static const char *const names[] = {
        "Default Settings", "nd-gate", "nd-new", "nd-old", "nd-locked" };
    static const char deftext[] =
        "HostName=\nProtocol=ssh\nTermWidth=80\nTermHeight=24\n"
        "RememberWindowPos\\0\\\nFolder=Default\n";
    static const char lockedtext[] =
        "HostName=lock.example\nTermWidth=80\nSessionLockdown=1\n";
    char tmp[MAX_PATH], root[MAX_PATH], sess[MAX_PATH], p[MAX_PATH];
    char *err = NULL, *v;
    settings_w *w;
    settings_r *r;
    int n, failed = -1;
    size_t i;

    GetTempPathA(sizeof(tmp), tmp);
    snprintf(root, sizeof(root), "%skitty-selftest-nd-%lu",
             tmp, (unsigned long)GetCurrentProcessId());
    snprintf(sess, sizeof(sess), "%s\\Sessions", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(sess, NULL);
    kitty_set_session_dir(sess);
    kitty_set_storage_mode(1);

    check(nd_write(sess, "Default Settings", deftext) &&
          nd_write(sess, "nd-locked", lockedtext) &&
          nd_write(sess, "nd-old",
                   "HostName=old.example\nTermWidth=80\nRememberWindowPos\\1\n"
                   "; TermType=vt100\n# TermHeight=40\n#TermWidth=99\n"),
          "nondefault: folder store seeded");

    /* the reader: a line starting with # or ; is a comment, whatever it holds */
    snprintf(p, sizeof(p), "%s\\nd-old", sess);
    r = kitty_open_settings_r_file(p);
    check(r && read_setting_i(r, "TermHeight", -7) == -7 &&
          read_setting_i(r, "TermWidth", -7) == 80,
          "nondefault: '# Key=value' is a comment, not a setting");
    v = r ? read_setting_s(r, "TermType") : NULL;
    check(r && !v, "nondefault: '; Key=value' is a comment, not a setting");
    sfree(v);
    v = r ? read_setting_s(r, "# TermHeight") : NULL;
    check(r && !v, "nondefault: no key '# TermHeight' is read");
    sfree(v);
    if (r) close_settings_r(r);

    /* without Inherit the switch does nothing: written whole */
    kitty_set_save_only_nondefault(1);
    kitty_set_inherit_default_settings(0);
    w = open_settings_w("nd-gate", &err);
    check(w != NULL, "nondefault: open nd-gate");
    if (w) {
        kitty_settings_w_save_rules(w, "nd-gate");
        write_setting_s(w, "HostName", "gate.example");
        write_setting_i(w, "TermWidth", 80);
        close_settings_w(w);
    }
    check(nd_file_is(sess, "nd-gate", "HostName=gate.example\nTermWidth=80\n"),
          "nondefault: Inherit off - the session is written whole");
    kitty_set_inherit_default_settings(1);

    /* a new file stays short */
    w = open_settings_w("nd-new", &err);
    check(w != NULL, "nondefault: open nd-new");
    if (w) {
        kitty_settings_w_save_rules(w, "nd-new");
        write_setting_s(w, "HostName", "");       /* marks a session file */
        write_setting_s(w, "Protocol", "ssh");    /* idem */
        write_setting_i(w, "TermWidth", 80);      /* the default */
        write_setting_i(w, "TermHeight", 30);
        write_setting_i(w, "RememberWindowPos", 0);   /* the default */
        write_setting_s(w, "Folder", "Default");  /* not a setting */
        write_setting_s(w, "TermType", "vt100");  /* Default Settings lacks it */
        close_settings_w(w);
    }
    check(nd_file_is(sess, "nd-new",
                     "HostName=\nProtocol=ssh\nTermHeight=30\nFolder=Default\n"
                     "TermType=vt100\n"),
          "nondefault: a new file holds only what differs from Default Settings");

    /* an existing file: unchanged lines stay, a value set back to the
     * default is commented out in its own form, a new value goes onto its
     * commented-out line */
    w = open_settings_w("nd-old", &err);
    check(w != NULL, "nondefault: open nd-old");
    if (w) {
        kitty_settings_w_save_rules(w, "nd-old");
        write_setting_s(w, "HostName", "old.example");
        write_setting_i(w, "TermWidth", 80);          /* unchanged, = default */
        write_setting_i(w, "RememberWindowPos", 0);   /* back to the default */
        write_setting_i(w, "TermHeight", 50);         /* was commented out */
        close_settings_w(w);
    }
    check(nd_file_is(sess, "nd-old",
                     "HostName=old.example\nTermWidth=80\n# RememberWindowPos\\0\n"
                     "; TermType=vt100\nTermHeight=50\n#TermWidth=99\n"),
          "nondefault: unchanged lines stay, default commented out, new value uncommented in place");
    w = open_settings_w("nd-old", &err);
    if (w) {
        kitty_settings_w_save_rules(w, "nd-old");
        write_setting_i(w, "TermHeight", 24);         /* back to the default */
        close_settings_w(w);
    }
    check(nd_file_is(sess, "nd-old",
                     "HostName=old.example\nTermWidth=80\n# RememberWindowPos\\0\n"
                     "; TermType=vt100\n# TermHeight=24\n#TermWidth=99\n"),
          "nondefault: a key=value line set back to the default is '# Key=value'");

    /* Default Settings is never shortened: no rule on it */
    w = open_settings_w("Default Settings", &err);
    if (w) {
        kitty_settings_w_save_rules(w, "Default Settings");
        write_setting_i(w, "TermWidth", 80);
        close_settings_w(w);
    }
    check(nd_file_is(sess, "Default Settings", deftext),
          "nondefault: Default Settings stays whole");

    /* Comment out defaults: every session once, Default Settings and a
     * locked session left, idempotent */
    n = kitty_store_comment_out_defaults("Default Settings", &failed);
    check(n == 2 && failed == 0,
          "nondefault: Comment out defaults changed nd-gate and nd-old");
    check(nd_file_is(sess, "nd-old",
                     "HostName=old.example\n# TermWidth=80\n# RememberWindowPos\\0\n"
                     "; TermType=vt100\n# TermHeight=24\n#TermWidth=99\n") &&
          nd_file_is(sess, "nd-gate", "HostName=gate.example\n# TermWidth=80\n") &&
          nd_file_is(sess, "nd-new",
                     "HostName=\nProtocol=ssh\nTermHeight=30\nFolder=Default\n"
                     "TermType=vt100\n"),
          "nondefault: each line equal to Default Settings is '# <line>', HostName/Protocol/Folder kept");
    check(nd_file_is(sess, "Default Settings", deftext) &&
          nd_file_is(sess, "nd-locked", lockedtext),
          "nondefault: Default Settings and a locked session untouched");
    n = kitty_store_comment_out_defaults("Default Settings", &failed);
    check(n == 0 && failed == 0, "nondefault: Comment out defaults is idempotent");

    kitty_set_save_only_nondefault(0);
    kitty_set_inherit_default_settings(0);
    kitty_set_storage_mode(0);
    kitty_set_session_dir("");
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(p, sizeof(p), "%s\\%s", sess, names[i]);
        DeleteFileA(p);
    }
    RemoveDirectoryA(sess);
    RemoveDirectoryA(root);
}

static void test_nondefault_registry(void)
{
    char path[600], dpath[600], sbuf[64];
    char *err = NULL;
    settings_w *w;
    HKEY k, dk;
    DWORD v, sz, type;

    snprintf(path, sizeof(path), "%s\\Sessions\\%s", kitty_registry_base(), NDSESS);
    snprintf(dpath, sizeof(dpath), "%s\\Sessions\\%s", kitty_registry_base(), NDDEF);

    w = open_settings_w(NDDEF, &err);
    check(w != NULL, "nondefault registry: open the scratch defaults");
    if (!w) return;
    write_setting_i(w, "TermWidth", 80);
    write_setting_i(w, "TermHeight", 24);
    write_setting_i(w, "Beep", 1);
    write_setting_s(w, "TermType", "xterm");
    close_settings_w(w);

    /* the session as a full save left it */
    w = open_settings_w(NDSESS, &err);
    check(w != NULL, "nondefault registry: open the scratch session");
    if (w) {
        write_setting_s(w, "HostName", "nd.example");
        write_setting_i(w, "TermWidth", 100);
        write_setting_i(w, "TermHeight", 24);
        write_setting_s(w, "TermType", "vt100");
        close_settings_w(w);
    }
    /* saved again under the rule */
    w = open_settings_w(NDSESS, &err);
    if (w) {
        kitty_settings_w_nondefault(w, NDDEF);
        write_setting_s(w, "HostName", "nd.example");
        write_setting_i(w, "TermWidth", 80);       /* back to the default */
        write_setting_i(w, "TermHeight", 24);      /* unchanged, = default */
        write_setting_s(w, "TermType", "vt100");   /* unchanged */
        write_setting_i(w, "Beep", 1);             /* the default, no value */
        close_settings_w(w);
    }
    sz = sizeof(v);
    check(RegGetValueA(HKEY_CURRENT_USER, path, "TermWidth", RRF_RT_REG_DWORD,
                       &type, &v, &sz) != ERROR_SUCCESS,
          "nondefault registry: a value set back to the default is deleted");
    sz = sizeof(v);
    check(RegGetValueA(HKEY_CURRENT_USER, path, "Beep", RRF_RT_REG_DWORD,
                       &type, &v, &sz) != ERROR_SUCCESS,
          "nondefault registry: a default never stored is not written");
    sz = sizeof(v);
    check(RegGetValueA(HKEY_CURRENT_USER, path, "TermHeight", RRF_RT_REG_DWORD,
                       &type, &v, &sz) == ERROR_SUCCESS && v == 24,
          "nondefault registry: an unchanged value equal to the default stays");
    sz = sizeof(sbuf);
    check(RegGetValueA(HKEY_CURRENT_USER, path, "TermType", RRF_RT_REG_SZ,
                       &type, sbuf, &sz) == ERROR_SUCCESS && !strcmp(sbuf, "vt100"),
          "nondefault registry: a non-default value stays");

    /* the button's step on this session: the default values are deleted */
    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_READ | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (RegOpenKeyExA(HKEY_CURRENT_USER, dpath, 0, KEY_READ, &dk) == ERROR_SUCCESS) {
            check(kitty_reg_delete_defaults(k, dk) == 1,
                  "nondefault registry: Comment out defaults deletes the one default value");
            check(kitty_reg_delete_defaults(k, dk) == 0,
                  "nondefault registry: and is idempotent");
            RegCloseKey(dk);
        }
        RegCloseKey(k);
    }
    sz = sizeof(v);
    check(RegGetValueA(HKEY_CURRENT_USER, path, "TermHeight", RRF_RT_REG_DWORD,
                       &type, &v, &sz) != ERROR_SUCCESS,
          "nondefault registry: TermHeight (= default) gone");
    sz = sizeof(sbuf);
    check(RegGetValueA(HKEY_CURRENT_USER, path, "HostName", RRF_RT_REG_SZ,
                       &type, sbuf, &sz) == ERROR_SUCCESS,
          "nondefault registry: HostName kept");

    del_settings(NDSESS);
    del_settings(NDDEF);
}

int main(int argc, char **argv)
{
    /* --wrap PASSPHRASE PLAINTEXT: print the self-contained MPW2 value, and
     * nothing else (no hive, no files) - a fixed test value for the console
     * tools' master-password tests is made this way */
    if (argc == 4 && !strcmp(argv[1], "--wrap")) {
        char *w = ksec_wrap_with_passphrase(argv[3], argv[2]);
        if (!w)
            return 1;
        printf("%s\n", w);
        free(w);
        return 0;
    }
    printf("== registry backend (DPAPI) ==\n");
    test_registry();
    printf("== portable backend (master password) ==\n");
    test_portable();
    printf("== long settings values (cyd01/KiTTY#541) ==\n");
    test_long_values();
    test_proxies_dir();
    printf("== renamed setting migrates on save ==\n");
    test_renamed_key();
    printf("== old 9bis KiTTY hive is still read (design doc 8.10) ==\n");
    test_old_kitty_hive();
    printf("== old .ktx export is still read (design doc 8.17) ==\n");
    test_old_ktx();
    printf("== login script protected like a password ==\n");
    test_script_protection();
    printf("== login script readable form ==\n");
    test_script_lines();
    printf("== save only non-default settings ==\n");
    test_nondefault_file();
    test_nondefault_registry();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
