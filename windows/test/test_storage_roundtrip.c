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
 *     guard re-persists the original blob on the next save;
 *   - a refused wrap (DPAPI failing, injected; the master password locked)
 *     keeps what is stored and reports it, never the plaintext or DPAPI.
 *
 * Touches only a zz-* scratch session in the live hive (deleted afterwards)
 * and a private %TEMP% portable tree (deleted afterwards). The portable test
 * seeds its own Security\ salt+verifier so it never falls through to the real
 * hive's MPW state (mpw_state_get reads the registry when the portable file
 * is absent).
 *
 * readonly=yes (kitty.ini in charge, kitty_inifile.c): every store write -
 * a session save and delete, a host key, the jump list, the state values
 * (kitty_dword_store and the rest), PUTTY.RND - leaves a %TEMP% folder store
 * byte-identical, and in registry mode creates nothing below a throwaway
 * base key (Software\KiTTYpp-selftest-ro-<pid>, never the live hive). The
 * kitty.ini in charge is pinned for the whole run: a file of the test's own,
 * or none - never the machine's.
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
#include "../kitty/kitty_inifile.h"   /* readonly=yes: the pin and the gate */
#include "../kitty/kitty_oldwin.h"    /* kitty_temp_path_dup: %TEMP% whole */
#include "../kitty/kitty_crypt.h"     /* cryptpassword: classic KiTTY's stored form */

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

/* Every path of this run is built whole (dupprintf) and kept here until
 * the run ends: a test that returns early leaks nothing, none is ever cut,
 * and a %TEMP% that cannot be read ends the run instead of naming a folder
 * from an unfilled buffer. */
static char **st_kept = NULL;
static size_t st_nkept = 0, st_kept_size = 0;

static char *st_keep(char *s)
{
    sgrowarray(st_kept, st_kept_size, st_nkept);
    st_kept[st_nkept++] = s;
    return s;
}

static char *st_path(const char *fmt, ...)
{
    va_list ap;
    char *s;
    va_start(ap, fmt);
    s = dupvprintf(fmt, ap);
    va_end(ap);
    return st_keep(s);
}

static char *st_temp(void)
{
    char *t = kitty_temp_path_dup();
    if (!t) {
        printf("FAIL  GetTempPath failed (%lu)\n",
               (unsigned long)GetLastError());
        exit(1);
    }
    return st_keep(t);
}

static void st_paths_free(void)
{
    while (st_nkept)
        sfree(st_kept[--st_nkept]);
    sfree(st_kept);
    st_kept = NULL;
    st_kept_size = 0;
}

static int write_text(const char *dir, const char *name, const char *val)
{
    char *path;
    path = st_path("%s\\%s", dir, name);
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
    char *tmp, *root, *sess, *sec;
    tmp = st_temp();
    root = st_path("%skitty-selftest-%lu",
             tmp, (unsigned long)GetCurrentProcessId());
    sess = st_path("%s\\Sessions", root);
    sec = st_path("%s\\Security", root);
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

    char *fpath;
    fpath = st_path("%s\\%s", sess, PSESS);
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
    char *p;
    p = st_path("%s\\MasterPwSalt", sec);     DeleteFileA(p);
    p = st_path("%s\\MasterPwVerifier", sec); DeleteFileA(p);
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
        char *tmp, *root, *sess, *fpath;
        tmp = st_temp();
        root = st_path("%skitty-selftest-long-%lu",
                 tmp, (unsigned long)GetCurrentProcessId());
        sess = st_path("%s\\Sessions", root);
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
        fpath = st_path("%s\\%s", sess, LSESS);
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
    char *dir, *path;
    FILE *fp;
    Conf *conf;

    if (!(dir = kitty_temp_path_dup())) {
        check(0, "ktx: no temp dir (skipped)");
        return;
    }
    st_keep(dir);
    path = st_path("%szz-kitty-selftest.ktx", dir);
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
    char *path;
    FILE *fp;
    path = st_path("%s\\%s", dir, name);
    fp = fopen(path, "wb");
    if (!fp) return 0;
    fputs(text, fp);
    return fclose(fp) == 0;
}

static int nd_file_is(const char *dir, const char *name, const char *want)
{
    char *path;
    char *got;
    int ok;
    path = st_path("%s\\%s", dir, name);
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
    char *tmp, *root, *sess, *p;
    char *err = NULL, *v;
    settings_w *w;
    settings_r *r;
    int n, failed = -1;
    size_t i;

    tmp = st_temp();
    root = st_path("%skitty-selftest-nd-%lu",
             tmp, (unsigned long)GetCurrentProcessId());
    sess = st_path("%s\\Sessions", root);
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
    p = st_path("%s\\nd-old", sess);
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
        p = st_path("%s\\%s", sess, names[i]);
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

/* ---------- readonly=yes: the store is not written ---------- */

/* Every file and folder below dir, with each file's bytes, into out (the
 * walk's order is the file system's, the same for both snapshots). */
static void ro_snap(const char *dir, const char *rel, strbuf *out)
{
    char *pat = dupprintf("%s\\*", dir), *full, *sub;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    h = FindFirstFileA(pat, &fd);
    sfree(pat);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        full = dupprintf("%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            put_fmt(out, "D %s%s\n", rel, fd.cFileName);
            sub = dupprintf("%s%s\\", rel, fd.cFileName);
            ro_snap(full, sub, out);
            sfree(sub);
        } else {
            FILE *fp = fopen(full, "rb");
            char buf[4096];
            size_t n;
            put_fmt(out, "F %s%s\n", rel, fd.cFileName);
            if (fp) {
                while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
                    put_data(out, buf, n);
                fclose(fp);
            }
            put_byte(out, '\n');
        }
        sfree(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* The test's own folders only: everything below dir, then dir. */
static void ro_rmtree(const char *dir)
{
    char *pat = dupprintf("%s\\*", dir), *full;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    h = FindFirstFileA(pat, &fd);
    sfree(pat);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            full = dupprintf("%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                ro_rmtree(full);
            else
                DeleteFileA(full);
            sfree(full);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

/* The calls of the automatic and the explicit writers, under readonly=yes. */
static void ro_try_writes(const char *what, int folder)
{
    char *err = (char *)"unset";
    char seed[32];
    char msg[160];
    int nfailed = -1;
    settings_w *w;

    w = open_settings_w("zz-ro-session", &err);
    snprintf(msg, sizeof(msg), "%s: saving a session is refused, no message", what);
    check(w == NULL && err == NULL, msg);
    if (w) {                               /* the gate failed: still close it */
        write_setting_s(w, "HostName", "changed.example");
        close_settings_w(w);
    }
    err = (char *)"unset";
    w = open_settings_w("zz-ro-new", &err);
    snprintf(msg, sizeof(msg), "%s: a new session is refused, no message", what);
    check(w == NULL && err == NULL, msg);
    if (w) {
        write_setting_s(w, "HostName", "new.example");
        close_settings_w(w);
    }
    del_settings("zz-ro-session");
    store_host_key(NULL, "ro.example", 22, "ssh-ed25519", "0xchanged");
    store_host_key(NULL, "ro-new.example", 2222, "ssh-ed25519", "0xnew");
    add_to_jumplist_registry("zz-ro-new");
    remove_from_jumplist_registry("zz-ro-session");
    kitty_set_show_foreign_sessions(1);          /* kitty_dword_store */
    kitty_arrange_offer_made();                  /* kitty_dword_store */
    kitty_set_last_session("zz-ro-other");
    kitty_set_last_folder("zz-ro-folder");
    kitty_state_set_string("ManageSession", "zz-ro-other");
    /* PUTTY.RND only where it is the store's: in registry mode it lives in
     * %LOCALAPPDATA%, outside the store, and is written as ever */
    if (folder) {
        memset(seed, 0x5a, sizeof(seed));
        write_random_seed(seed, sizeof(seed));
    }
    host_ca_delete("zz-ro-ca");
    snprintf(msg, sizeof(msg), "%s: Comment out defaults changes nothing", what);
    check(kitty_store_comment_out_defaults("Default Settings", &nfailed) == 0 &&
          nfailed == 0, msg);
}

static void test_readonly_folder(const char *roini, const char *noini)
{
    char *tmp, *root, *sess;
    char *err = NULL;
    settings_w *w;
    strbuf *before, *after;
    char seed[32];

    tmp = st_temp();
    root = st_path("%skitty-selftest-ro-%lu",
             tmp, (unsigned long)GetCurrentProcessId());
    sess = st_path("%s\\Sessions", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(sess, NULL);
    kitty_set_session_dir(sess);
    kitty_set_storage_mode(1);

    /* the store as a writable run leaves it */
    kitty_inifile_pin(noini);
    check(!kitty_store_readonly(), "readonly folder: writable without a readonly kitty.ini");
    w = open_settings_w("zz-ro-session", &err);
    check(w != NULL, "readonly folder: seed session written");
    if (w) {
        write_setting_s(w, "HostName", "ro.example");
        close_settings_w(w);
    }
    store_host_key(NULL, "ro.example", 22, "ssh-ed25519", "0xseed");
    add_to_jumplist_registry("zz-ro-session");
    kitty_set_last_session("zz-ro-session");
    memset(seed, 0x33, sizeof(seed));
    write_random_seed(seed, sizeof(seed));

    /* readonly=yes from here */
    kitty_inifile_pin(roini);
    check(kitty_store_readonly() && kitty_inifile_readonly(),
          "readonly folder: kitty.ini readonly=yes is in force");
    before = strbuf_new();
    ro_snap(root, "", before);
    check(strstr(before->s, "zz-ro-session") && strstr(before->s, "KiTTYState") &&
          strstr(before->s, "PUTTY.RND") && strstr(before->s, "SshHostKeys"),
          "readonly folder: the seeded store holds a session, state, seed and a host key");

    ro_try_writes("readonly folder", 1);
    check(portable_write_text_file("SshHostKeys", "zz-ro", "x") == KITTY_RO_REFUSED,
          "readonly folder: portable_write_text_file answers KITTY_RO_REFUSED");
    check(kitty_portable_store_state_string("zz-ro", "x") == KITTY_RO_REFUSED,
          "readonly folder: the state writer answers KITTY_RO_REFUSED");

    after = strbuf_new();
    ro_snap(root, "", after);
    check(before->len == after->len && !memcmp(before->s, after->s, before->len),
          "readonly folder: the folder store is byte-identical");
    strbuf_free(before);
    strbuf_free(after);

    kitty_inifile_pin(noini);
    kitty_set_storage_mode(0);
    kitty_set_session_dir("");
    ro_rmtree(root);
}

static void test_readonly_registry(const char *roini, const char *noini)
{
    char base[128];
    HKEY k;
    int made;

    /* a throwaway base: under readonly nothing may appear there */
    snprintf(base, sizeof(base), "Software\\KiTTYpp-selftest-ro-%lu",
             (unsigned long)GetCurrentProcessId());
    kitty_set_registry_base(base);
    kitty_set_storage_mode(0);
    kitty_inifile_pin(roini);
    check(kitty_store_ro_regpath(1, base) &&
          kitty_store_ro_regpath(1, kitty_reg_sessions()),
          "readonly registry: the base in use is the store");
    check(!kitty_store_ro_regpath(1, "Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
          "readonly registry: the Run key is not");

    ro_try_writes("readonly registry", 0);


    made = RegOpenKeyExA(HKEY_CURRENT_USER, base, 0, KEY_READ, &k) == ERROR_SUCCESS;
    check(!made, "readonly registry: nothing created below the base");
    if (made) {                         /* the gate failed: the test's own key goes */
        RegCloseKey(k);
        RegDeleteTreeA(HKEY_CURRENT_USER, base);
    }

    kitty_inifile_pin(noini);
    kitty_set_registry_root(0);
}

/* ---------- a master password setup whose salt cannot be stored ----------
 *
 * The first password saved under the master-password policy sets the master
 * password up (here supplied, as -masterpwfile does, so no prompt). The
 * Security\ of this tree is a FILE, so the salt cannot be written: the store
 * failure is reported once and the save keeps nothing. That save reports
 * nothing more - the failure report states that the password was not saved;
 * it used to be followed by "The master password is locked: ..." - and a
 * later save of the same run reports that no master password was set up,
 * never that it is locked: there is none. */
#define FSESS1 "zz-kitty-selftest-mpwfail-1"
#define FSESS2 "zz-kitty-selftest-mpwfail-2"

static int fail_store_reports = 0, fail_kept_reports = 0;
static char fail_kept_text[512];
static void fail_store_failed(const char *reason)
{
    fail_store_reports++;
}
static void fail_locked_save(const char *cap, const char *msg)
{
    fail_kept_reports++;
    snprintf(fail_kept_text, sizeof(fail_kept_text), "%s", msg ? msg : "");
}

/* One session saved with a password; 1 when the save opened. */
static int fail_save(const char *name, const char *pw)
{
    char *err = NULL;
    settings_w *w = open_settings_w(name, &err);
    if (!w)
        return 0;
    write_setting_s(w, "HostName", "mpwfail.example");
    write_setting_s(w, "Password", pw);
    close_settings_w(w);
    return 1;
}

static void test_mpw_setup_store_failed(void)
{
    char *tmp, *root, *sess, *blocker;
    char *fpath, *fbuf, *line;
    int kept0 = kitty_mpw_kept_count(), nosetup0 = kitty_mpw_kept_nosetup_count();
    DWORD attr;

    tmp = st_temp();
    root = st_path("%skitty-selftest-mpwfail-%lu",
             tmp, (unsigned long)GetCurrentProcessId());
    sess = st_path("%s\\Sessions", root);
    blocker = st_path("%s\\Security", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(sess, NULL);
    check(write_text(root, "Security", "not a folder"),
          "mpw setup failed: Security\\ is a file");
    kitty_set_session_dir(sess);
    kitty_set_storage_mode(1);
    kitty_set_master_pw_store_failed(fail_store_failed);
    kitty_set_master_pw_locked_save(fail_locked_save);
    kitty_set_master_passphrase("selftest-setup-pw");

    check(fail_save(FSESS1, "not-stored-1"),
          "mpw setup failed: open_settings_w");
    check(fail_store_reports == 1,
          "mpw setup failed: the store failure is reported once");
    check(fail_kept_reports == 0,
          "mpw setup failed: no further report for that save (no \"is locked\")");
    check(kitty_mpw_kept_count() - kept0 == 1 &&
          kitty_mpw_kept_nosetup_count() - nosetup0 == 1,
          "mpw setup failed: the kept password counts as not set up");
    fpath = st_path("%s\\%s", sess, FSESS1);
    fbuf = read_whole_file(fpath);
    line = fbuf ? password_line(fbuf) : NULL;
    check(fbuf && (!line || !strcmp(line, "Password=")) &&
          !strstr(fbuf, "not-stored-1") && !strstr(fbuf, "DPAPI1:"),
          "mpw setup failed: the session stores no password");
    free(line);
    free(fbuf);
    attr = GetFileAttributesA(blocker);
    check(attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY),
          "mpw setup failed: Security\\ is still the file");

    check(fail_save(FSESS2, "not-stored-2"),
          "mpw setup failed: a second session saved");
    check(fail_store_reports == 1,
          "mpw setup failed: no second setup in the same run");
    check(fail_kept_reports == 1 &&
          !strncmp(fail_kept_text, "No master password was set up:", 30) &&
          !strstr(fail_kept_text, "is locked"),
          "mpw setup failed: a later save reports no master password set up, "
          "not locked");

    kitty_set_master_pw_store_failed(NULL);
    kitty_set_master_pw_locked_save(NULL);
    kitty_set_master_passphrase(NULL);
    kitty_set_storage_mode(0);
    kitty_set_session_dir("");
    ro_rmtree(root);
}

/* ---------- the login script in memory ----------
 *
 * For the length of a connection the script (kitty.c ScriptFileContent) is
 * held wrapped with CryptProtectMemory for this process; the matcher opens
 * it into a copy for one compare, and a pair that ran leaves the rest of the
 * script, wrapped again (kitty_loginscript_mem_*). */
static int bytes_contain(const unsigned char *hay, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    for (size_t i = 0; k && i + k <= n; i++)
        if (!memcmp(hay + i, needle, k))
            return 1;
    return 0;
}

static void test_script_in_memory(void)
{
    static const unsigned char blob[] =
        "login:\0user-mk7Q\0password:\0s3cret-mk7Q\0";   /* + NUL: the end entry */
    static const unsigned char plainheld[] = "a:\0b\0";
    char *held, *held2, *held3;
    unsigned char *open1 = NULL, *open2 = NULL, *dec = NULL;
    size_t n1 = 0, n2 = 0;
    int dn = 0, leak = 1;

    held = kitty_loginscript_mem_wrap(blob, sizeof(blob));
    check(held && kitty_pw_is_wrapped(held),
          "script in memory: held wrapped ({kpw1})");
    if (held && kitty_pw_is_wrapped(held)) {
        dec = ksec_b64_decode(held + strlen("{kpw1}"), &dn);
        leak = bytes_contain((const unsigned char *)held, strlen(held), "s3cret-mk7Q") ||
               bytes_contain((const unsigned char *)held, strlen(held), "user-mk7Q") ||
               !dec ||
               bytes_contain(dec, (size_t)dn, "s3cret-mk7Q") ||
               bytes_contain(dec, (size_t)dn, "user-mk7Q") ||
               bytes_contain(dec, (size_t)dn, "password:");
        if (dec) { smemclr(dec, dn); sfree(dec); }
    }
    check(!leak, "script in memory: the held form carries none of the plaintext bytes");

    open1 = kitty_loginscript_mem_open(held, &n1);
    check(open1 && n1 == sizeof(blob) && !memcmp(open1, blob, sizeof(blob)) &&
          open1[n1] == 0 && open1[n1 + 1] == 0,
          "script in memory: opens to the blob, NULs and all, two zero bytes after");

    held2 = open1 ? kitty_loginscript_mem_next(open1, n1) : NULL;
    open2 = held2 ? kitty_loginscript_mem_open(held2, &n2) : NULL;
    check(held2 && kitty_pw_is_wrapped(held2) && open2 &&
          !strcmp((char *)open2, "password:") &&
          !strcmp((char *)open2 + strlen("password:") + 1, "s3cret-mk7Q") &&
          n2 == sizeof(blob) - strlen("login:") - strlen("user-mk7Q") - 2,
          "script in memory: a pair that ran leaves the next pair, wrapped again");
    held3 = open2 ? kitty_loginscript_mem_next(open2, n2) : (char *)1;
    check(held3 == NULL, "script in memory: after the last pair nothing is held");

    {
        static const unsigned char empty[] = "";
        char *h = kitty_loginscript_mem_wrap(empty, sizeof(empty));
        check(h == NULL, "script in memory: an empty script holds nothing");
    }
    /* where the protection is unavailable the plain blob is held, as before:
     * the reader takes it */
    {
        char *h = snewn(sizeof(plainheld) + 1, char);
        unsigned char *o;
        size_t on = 0;
        memcpy(h, plainheld, sizeof(plainheld));
        h[sizeof(plainheld)] = '\0';
        o = kitty_loginscript_mem_open(h, &on);
        check(o && on == sizeof(plainheld) && !memcmp(o, plainheld, on),
              "script in memory: a plain held blob (no protection) opens as is");
        if (o) { smemclr(o, on + 2); sfree(o); }
        kitty_loginscript_mem_free(h);
    }

    if (open1) { smemclr(open1, n1 + 2); sfree(open1); }
    if (open2) { smemclr(open2, n2 + 2); sfree(open2); }
    kitty_loginscript_mem_free(held);
    kitty_loginscript_mem_free(held2);
}

/* ---------- a refused wrap: DPAPI failing, the master password locked ----------
 *
 * A save whose DPAPI wrap fails stores nothing for that secret: what is
 * stored stays as it was, never the plaintext, and the save is reported
 * (caption and text of kitty_text.h KT_DPAPI_KEPT_*) and counted as kept.
 * The failure is injected through kitty_selftest_dpapi_fail, compiled into
 * this test's own copy of kitty_secretstore.c only (KITTY_SELFTEST_HOOKS).
 * Under the master-password policy a locked master password refuses the
 * named-proxy, login-script and .ktx wraps exactly like the session save -
 * no DPAPI value in their place. SaveProxyInfo, ReadInitScript and the .ktx
 * writer are not linked here: their wraps are driven directly, and their
 * reports through ksec_report_locked_save as those callers make them. */
#define DSESS1 "zz-kitty-selftest-dpapifail-1"
#define DSESS2 "zz-kitty-selftest-dpapifail-2"
#define DSESS3 "zz-kitty-selftest-dpapifail-p"
#define DP_ERR ERROR_ACCESS_DENIED          /* "... (error 5)" */
#define DP_HEAD_PW     "Windows could not encrypt the password (DPAPI: "
#define DP_HEAD_SCRIPT "Windows could not encrypt the login script (DPAPI: "

static int dp_reports = 0;
static char dp_cap[128], dp_text[1024];
static void dp_kept(const char *cap, const char *msg)
{
    dp_reports++;
    snprintf(dp_cap, sizeof(dp_cap), "%s", cap ? cap : "");
    snprintf(dp_text, sizeof(dp_text), "%s", msg ? msg : "");
}
/* the last report: head, Windows' reason (its text is in the system's
 * language; only the number is fixed) and tail */
static int dp_text_is(const char *head, const char *tail)
{
    size_t n = strlen(dp_text), hl = strlen(head), tl = strlen(tail);
    return n > hl + tl && !strncmp(dp_text, head, hl) &&
        !strcmp(dp_text + n - tl, tail);
}
static char *reg_raw(const char *sess, const char *name)   /* malloc'd or NULL */
{
    char path[600], raw[4096];
    DWORD sz = sizeof(raw);
    snprintf(path, sizeof(path), "%s\\Sessions\\%s", kitty_registry_base(), sess);
    if (RegGetValueA(HKEY_CURRENT_USER, path, name, RRF_RT_REG_SZ, NULL,
                     raw, &sz) != ERROR_SUCCESS)
        return NULL;
    return _strdup(raw);
}
static void dp_save(const char *sess, const char *pw, const char *pxpw)
{
    char *err = NULL;
    settings_w *w = open_settings_w(sess, &err);
    if (!w)
        return;
    write_setting_s(w, "HostName", "dpapifail.example");
    write_setting_s(w, "Password", pw);
    if (pxpw)
        write_setting_s(w, "ProxyPassword", pxpw);
    close_settings_w(w);
}

static void test_dpapi_fail_registry(void)
{
    char *pw0, *px0, *pw1, *px1, *line;
    int kept0 = kitty_mpw_kept_count(), dp0 = kitty_mpw_kept_dpapi_count();

    kitty_set_master_pw_locked_save(dp_kept);
    dp_reports = 0;
    dp_save(DSESS1, "dp-first", "dp-px-first");
    pw0 = reg_raw(DSESS1, "Password");
    px0 = reg_raw(DSESS1, "ProxyPassword");
    check(pw0 && !strncmp(pw0, "DPAPI1:", 7) && px0 && !strncmp(px0, "DPAPI1:", 7),
          "dpapi fail, registry: the first save is DPAPI1");

    kitty_selftest_dpapi_fail(DP_ERR);
    dp_save(DSESS1, "dp-second", "dp-px-second");
    pw1 = reg_raw(DSESS1, "Password");
    px1 = reg_raw(DSESS1, "ProxyPassword");
    check(pw0 && pw1 && !strcmp(pw0, pw1) && px0 && px1 && !strcmp(px0, px1),
          "dpapi fail, registry: a changed password keeps the stored value");
    check(pw1 && !strstr(pw1, "dp-second") && px1 && !strstr(px1, "dp-px-second"),
          "dpapi fail, registry: ...never the plaintext");
    check(dp_reports == 1,
          "dpapi fail, registry: Password + ProxyPassword of one save report once");
    check(!strcmp(dp_cap, "KiTTY++ password encryption") &&
          dp_text_is(DP_HEAD_PW, "(error 5)): the password of session '"
                     DSESS1 "' was not saved."),
          "dpapi fail, registry: the report names Windows' reason and the session");
    check(kitty_mpw_kept_count() - kept0 == 1 &&
          kitty_mpw_kept_dpapi_count() - dp0 == 1,
          "dpapi fail, registry: counted as kept (and as a DPAPI refusal)");

    /* a session that had no password: none is stored */
    del_settings(DSESS2);
    dp_save(DSESS2, "dp-new", NULL);
    line = reg_raw(DSESS2, "Password");
    check(line == NULL, "dpapi fail, registry: a new password stores no value at all");
    check(dp_reports == 2, "dpapi fail, registry: ...and is reported");
    free(line);

    line = kitty_secret_wrap_current_backend("dp-wrap");
    check(line == NULL, "dpapi fail, registry: the backend wrap refuses (no plaintext)");
    free(line);
    /* the -pwfile line for a helper is no stored secret: it keeps its plain
     * fallback, the format -pwfile has always taken */
    line = kitty_pwfile_line("dp-pwfile");
    check(line && !strcmp(line, "dp-pwfile") && !kitty_pwfile_line_is_protected(line),
          "dpapi fail: the helper's password file keeps its plain fallback");
    free(line);

    kitty_selftest_dpapi_fail(0);
    dp_save(DSESS1, "dp-third", NULL);
    pw1 = (free(pw1), reg_raw(DSESS1, "Password"));
    check(pw1 && !strncmp(pw1, "DPAPI1:", 7) && pw0 && strcmp(pw0, pw1),
          "dpapi fail, registry: DPAPI back, the next save stores again");
    free(pw0); free(px0); free(pw1); free(px1);
    del_settings(DSESS1);
    del_settings(DSESS2);
    kitty_set_master_pw_locked_save(NULL);
}

static void test_dpapi_fail_portable(void)
{
    char *tmp, *root, *sess, *fpath;
    char *fb, *before, *after, *w;
    int lk, sk;
    int kept0 = kitty_mpw_kept_count();

    tmp = st_temp();
    root = st_path("%skitty-selftest-dpapifail-%lu",
             tmp, (unsigned long)GetCurrentProcessId());
    sess = st_path("%s\\Sessions", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(sess, NULL);
    kitty_set_session_dir(sess);
    kitty_set_storage_mode(1);
    kitty_set_portable_password_protection("dpapi");
    kitty_set_master_pw_locked_save(dp_kept);
    dp_reports = 0;

    dp_save(DSESS3, "dpp-first", NULL);
    fpath = st_path("%s\\%s", sess, DSESS3);
    fb = read_whole_file(fpath);
    before = fb ? password_line(fb) : NULL;
    /* the folder store escapes the marker's ':' as %3A (folderstore.but) */
    check(before && (!strncmp(before, "Password=DPAPI1:", 16) ||
                     !strncmp(before, "Password=DPAPI1%3A", 18)),
          "dpapi fail, portable dpapi: the first save is DPAPI1");
    free(fb);

    kitty_selftest_dpapi_fail(DP_ERR);
    dp_save(DSESS3, "dpp-second", NULL);
    fb = read_whole_file(fpath);
    after = fb ? password_line(fb) : NULL;
    check(before && after && !strcmp(before, after),
          "dpapi fail, portable dpapi: a changed password keeps the stored line");
    check(fb && !strstr(fb, "dpp-second"),
          "dpapi fail, portable dpapi: ...never the plaintext");
    check(dp_reports == 1 && !strcmp(dp_cap, "KiTTY++ password encryption") &&
          dp_text_is(DP_HEAD_PW, "(error 5)): the password of session '"
                     DSESS3 "' was not saved."),
          "dpapi fail, portable dpapi: reported with the session's file");
    free(fb); free(before); free(after);

    /* the named proxy, the login script and a .ktx under the same policy */
    w = kitty_secret_wrap_current_backend_kept("dpp-proxy", &lk);
    check(w == NULL && lk == 1, "dpapi fail, portable dpapi: a named proxy's wrap refuses");
    ksec_report_locked_save(KSEC_KEPT_PROXY, "dp-proxy");
    check(dp_text_is(DP_HEAD_PW, "(error 5)): the password of named proxy 'dp-proxy' was not saved."),
          "dpapi fail, portable dpapi: ...reported for the named proxy");
    w = kitty_secret_wrap_current_backend_kept("bG9naW46AHVzZXIA", &lk);
    check(w == NULL && lk == 1, "dpapi fail, portable dpapi: a login script's wrap refuses");
    ksec_report_locked_save(KSEC_KEPT_SCRIPT, "dp-script");
    check(dp_text_is(DP_HEAD_SCRIPT, "(error 5)): the login script of session 'dp-script' was not saved."),
          "dpapi fail, portable dpapi: ...reported for the login script");
    w = kitty_secret_wrap_export("dpp-export", KSEC_EXPORT_SESSION_PW, "dp-export",
                                 &lk, &sk);
    check(w == NULL && lk == 1 && sk == 0, "dpapi fail, portable dpapi: a .ktx export's wrap refuses");
    ksec_report_locked_save(KSEC_KEPT_EXPORT, "C:\\dp\\out.ktx");
    check(dp_text_is(DP_HEAD_PW, "(error 5)): no password was written to 'C:\\dp\\out.ktx'."),
          "dpapi fail, portable dpapi: ...reported for the export file");
    check(dp_reports == 4 && kitty_mpw_kept_count() - kept0 == 4,
          "dpapi fail, portable dpapi: every refusal reported and counted");
    w = kitty_secret_wrap_current_backend("dpp-plainwrap");
    check(w == NULL, "dpapi fail, portable dpapi: the backend wrap refuses (no plaintext)");
    free(w);

    /* legacy stays the user's explicit plain choice */
    kitty_set_portable_password_protection("legacy");
    w = kitty_secret_wrap_current_backend_kept("dpp-legacy", &lk);
    check(w && !strcmp(w, "dpp-legacy") && lk == 0,
          "dpapi fail, portable legacy: plain as chosen, no refusal");
    free(w);

    kitty_selftest_dpapi_fail(0);
    kitty_set_portable_password_protection("master");
    kitty_set_master_pw_locked_save(NULL);
    kitty_set_storage_mode(0);
    kitty_set_session_dir("");
    ro_rmtree(root);
}

static void test_mpw_locked_refuses(void)
{
    char *tmp, *root, *sess, *sec;
    unsigned char salt[KITTY_MPW_SALT_LEN], key[KITTY_MPW_DERIVED_LEN];
    char *ver, *saltb64, *w;
    int lk, sk;

    tmp = st_temp();
    root = st_path("%skitty-selftest-mpwlocked-%lu",
             tmp, (unsigned long)GetCurrentProcessId());
    sess = st_path("%s\\Sessions", root);
    sec = st_path("%s\\Security", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(sess, NULL);
    CreateDirectoryA(sec, NULL);
    /* a salt of its own: test_portable's key is cached for its salt and
     * would unlock this store without a passphrase */
    for (int i = 0; i < KITTY_MPW_SALT_LEN; i++)
        salt[i] = (unsigned char)(0x60 + i);
    kitty_mpw_derive(PPASS, salt, sizeof(salt), key);
    ver = kitty_mpw_protect("KiTTY-MPW-verify", key);
    saltb64 = ksec_b64_encode(salt, sizeof(salt));
    SecureZeroMemory(key, sizeof(key));
    check(ver && saltb64 && write_text(sec, "MasterPwSalt", saltb64) &&
          write_text(sec, "MasterPwVerifier", ver),
          "mpw locked: store seeded");
    free(saltb64);
    sfree(ver);
    kitty_set_session_dir(sess);
    kitty_set_storage_mode(1);
    kitty_set_portable_password_protection("master");
    kitty_set_master_passphrase("wrong-master-pw");   /* locked */
    kitty_set_master_pw_locked_save(dp_kept);
    dp_reports = 0;

    w = kitty_secret_wrap_current_backend_kept("mpl-proxy", &lk);
    check(w == NULL && lk == 1, "mpw locked: a named proxy's wrap refuses, no DPAPI");
    free(w);
    ksec_report_locked_save(KSEC_KEPT_PROXY, "mpl-proxy");
    check(!strcmp(dp_cap, "KiTTY++ master password") &&
          !strcmp(dp_text, "The master password is locked: the password of named proxy 'mpl-proxy' was not saved."),
          "mpw locked: ...reported as locked (not as DPAPI)");
    w = kitty_secret_wrap_current_backend_kept("bG9naW46AHVzZXIA", &lk);
    check(w == NULL && lk == 1, "mpw locked: a login script's wrap refuses, no DPAPI");
    free(w);
    ksec_report_locked_save(KSEC_KEPT_SCRIPT, "mpl-script");
    check(!strcmp(dp_text, "The master password is locked: the login script of session 'mpl-script' was not saved."),
          "mpw locked: ...reported for the login script");
    w = kitty_secret_wrap_export("mpl-export", KSEC_EXPORT_SESSION_PW, "mpl-export",
                                 &lk, &sk);
    check(w == NULL && lk == 1 && sk == 0, "mpw locked: a .ktx export's wrap refuses, no DPAPI");
    free(w);
    ksec_report_locked_save(KSEC_KEPT_EXPORT, "C:\\mpl\\out.ktx");
    check(!strcmp(dp_text, "The master password is locked: no password was written to 'C:\\mpl\\out.ktx'."),
          "mpw locked: ...reported for the export file");
    w = kitty_secret_wrap_current_backend("mpl-plainwrap");
    check(w == NULL, "mpw locked: the non-kept backend wrap refuses too (its DPAPI fallback is gone)");
    free(w);
    check(dp_reports == 3, "mpw locked: one report per refused save");

    kitty_set_master_pw_locked_save(NULL);
    kitty_set_master_passphrase(NULL);
    kitty_set_storage_mode(0);
    kitty_set_session_dir("");
    ro_rmtree(root);
}

/* ---------- classic KiTTY's stored forms, protected by every save ----------
 *
 * A session classic KiTTY wrote keeps its three at-rest-protected settings in
 * classic forms: the Password bcrypt-encoded (keyed on the host name, the
 * terminal type and "KiTTY"), the ProxyPassword in clear, the login script
 * (ScriptfileContent) scrambled with the built-in key. Every save stores
 * them protected, also when it does not change them and with "save only
 * non-default settings" on; PortablePasswordProtection=legacy writes the
 * obfuscated ones in clear; a refused protection keeps the stored value and
 * reports it; a write of a few values (a folder move, the Session Lockdown
 * switch) leaves all three as stored. The folder store (a classic
 * "Key\value\" file) and the registry store (classic KiTTY's hive copied
 * value for value into ours) alike.
 */
#define CHOST     "classic.example"
#define CSECRET   "Classic-pw-2026"
#define CPROXY    "Classic-proxy-2026"
#define CSCRIPT   "login: send-this-2026"

/* A password in classic KiTTY's stored form, kept until the run ends.
 * Encoded in place: room for bcrypt's base64 form, under 4n + 64. */
static char *classic_form(const char *pw, const char *host)
{
    char *enc = st_keep(snewn(4 * strlen(pw) + 64, char));
    strcpy(enc, pw);
    cryptpassword(0, enc, host, "xterm");
    return enc;
}
/* A login script in classic KiTTY's stored form (the built-in key). */
static char *classic_script_form(const char *text)
{
    char *enc = st_keep(snewn(4 * strlen(text) + 64, char));
    strcpy(enc, text);
    cryptstring(0, enc, MASTER_PASSWORD);
    return enc;
}
/* kitty.exe's decoder of that form, as it hands it to the store */
static unsigned char *classic_script_decode(const char *stored, int *len)
{
    char *buf = dupstr(stored);
    int l = decryptstring(0, buf, MASTER_PASSWORD);
    if (l <= 0) {
        sfree(buf);
        return NULL;
    }
    *len = l;
    return (unsigned char *)buf;
}

/* The value of "<key>=" in a session file's text, malloc'd; NULL = none. */
static char *file_value(const char *buf, const char *key)
{
    size_t kl = strlen(key);
    const char *p = buf;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            /* unmunged: the file escapes ':' and others as %HH */
            size_t n = strcspn(p + kl + 1, "\r\n"), i, o = 0;
            const char *s = p + kl + 1;
            char *v = malloc(n + 1);
            if (!v)
                return NULL;
            for (i = 0; i < n; i++) {
                unsigned hv;
                if (s[i] == '%' && i + 2 < n && sscanf(s + i + 1, "%2x", &hv) == 1) {
                    v[o++] = (char)hv;
                    i += 2;
                } else {
                    v[o++] = s[i];
                }
            }
            v[o] = '\0';
            return v;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

/* The login script a stored value holds: protected, opened, the text. */
static int script_reads_back(const char *stored)
{
    char *b64 = NULL;
    unsigned char *raw = NULL;
    int n = 0, ok;
    if (!stored || !kitty_secret_is_marked(stored) ||
        strncmp(stored, "PLAIN:", 6) == 0)
        return 0;
    if (kitty_secret_unwrap(stored, &b64) != 1 || !b64)
        return 0;
    raw = ksec_b64_decode(b64, &n);
    ok = raw && n == (int)strlen(CSCRIPT) && !memcmp(raw, CSCRIPT, n);
    free(raw);
    free(b64);
    return ok;
}

static int classic_reports = 0;
static void classic_kept(const char *cap, const char *msg)
{
    classic_reports++;
}

#define CW_PW 1
#define CW_PX 2
#define CW_SC 4

/* A session file as classic KiTTY writes it, with the settings in `what`;
 * the forms written are kept in cl_pw / cl_sc. */
static const char *cl_pw = "", *cl_sc = "";
static int classic_file(const char *path, int what)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return 0;
    cl_pw = classic_form(CSECRET, CHOST);
    cl_sc = classic_script_form(CSCRIPT);
    fprintf(fp, "HostName\\%s\\\nTerminalType\\xterm\\\nProtocol\\ssh\\\n", CHOST);
    if (what & CW_PW)
        fprintf(fp, "Password\\%s\\\n", cl_pw);
    if (what & CW_PX)
        fprintf(fp, "ProxyPassword\\%s\\\n", CPROXY);
    if (what & CW_SC)
        fprintf(fp, "ScriptfileContent\\%s\\\n", cl_sc);
    return fclose(fp) == 0;
}
/* The classic lines of the three, as written by classic_file, all there. */
static int classic_lines_kept(const char *buf, int what)
{
    char *l;
    int ok = buf != NULL;
    if (ok && (what & CW_PW)) {
        l = dupprintf("Password\\%s\\", cl_pw);
        ok = strstr(buf, l) != NULL;
        sfree(l);
    }
    if (ok && (what & CW_PX))
        ok = strstr(buf, "ProxyPassword\\" CPROXY "\\") != NULL;
    if (ok && (what & CW_SC)) {
        l = dupprintf("ScriptfileContent\\%s\\", cl_sc);
        ok = strstr(buf, l) != NULL;
        sfree(l);
    }
    return ok;
}

/* A real save that changes nothing: the session loaded and saved as a
 * whole (load_settings, save_settings - what Save in the box does). nd:
 * "save only non-default settings", with Inherit Default Settings. */
static void real_save(const char *sess, int nd)
{
    Conf *c = conf_new();
    char *err;
    if (nd) {
        kitty_set_inherit_default_settings(1);
        kitty_set_save_only_nondefault(1);
    }
    load_settings(sess, c);
    err = save_settings(sess, c);
    if (err) {
        printf("  save_settings(%s): %s\n", sess, err);
        sfree(err);
    }
    conf_free(c);
    if (nd) {
        kitty_set_inherit_default_settings(0);
        kitty_set_save_only_nondefault(0);
    }
}

/* A write of one value only, as a folder move writes Folder. */
static void folder_write(const char *sess, const char *folder)
{
    char *err = NULL;
    settings_w *w = open_settings_w(sess, &err);
    if (!w)
        return;
    write_setting_s(w, "Folder", folder);
    close_settings_w(w);
}

static int classic_prompts = 0;
static char *classic_prompt(int creating)
{
    classic_prompts++;
    return NULL;                        /* cancelled */
}

/* The settings in `what` are stored protected (DPAPI1) and read back. */
static void check_protected_file(const char *label, const char *sess,
                                 const char *fpath, int what)
{
    char *fbuf = read_whole_file(fpath), *v;
    settings_r *r;
    char msg[160];
    if (what & CW_PW) {
        v = fbuf ? file_value(fbuf, "Password") : NULL;
        snprintf(msg, sizeof(msg), "%s: Password stored DPAPI1 (got %.12s)",
                 label, v ? v : "(none)");
        check(v && !strncmp(v, "DPAPI1:", 7), msg);
        free(v);
    }
    if (what & CW_PX) {
        v = fbuf ? file_value(fbuf, "ProxyPassword") : NULL;
        snprintf(msg, sizeof(msg), "%s: ProxyPassword stored DPAPI1 (got %.12s)",
                 label, v ? v : "(none)");
        check(v && !strncmp(v, "DPAPI1:", 7), msg);
        free(v);
    }
    if (what & CW_SC) {
        v = fbuf ? file_value(fbuf, "ScriptfileContent") : NULL;
        snprintf(msg, sizeof(msg), "%s: login script stored protected and reads back (got %.12s)",
                 label, v ? v : "(none)");
        check(script_reads_back(v), msg);
        free(v);
    }
    snprintf(msg, sizeof(msg), "%s: no classic form, no clear text left", label);
    check(fbuf && !strstr(fbuf, "Password\\") && !strstr(fbuf, CSECRET) &&
          !strstr(fbuf, CPROXY) && !strstr(fbuf, "ScriptfileContent\\"), msg);
    if (fbuf) free(fbuf);
    r = open_settings_r(sess);
    if (what & CW_PW) {
        char *pw = r ? read_setting_s(r, "Password") : NULL;
        snprintf(msg, sizeof(msg), "%s: Password reads back", label);
        check(pw && !strcmp(pw, CSECRET), msg);
        if (pw) sfree(pw);
    }
    if (what & CW_PX) {
        char *px = r ? read_setting_s(r, "ProxyPassword") : NULL;
        snprintf(msg, sizeof(msg), "%s: ProxyPassword reads back", label);
        check(px && !strcmp(px, CPROXY), msg);
        if (px) sfree(px);
    }
    if (r) close_settings_r(r);
}

static void test_classic_forms_folder(void)
{
    static const struct { const char *name; int what; int nd; } cases[] = {
        { "zz-cl-pw", CW_PW, 0 },
        { "zz-cl-px", CW_PX, 0 },
        { "zz-cl-sc", CW_SC, 0 },
        { "zz-cl-all", CW_PW | CW_PX | CW_SC, 0 },
        { "zz-cl-all-nd", CW_PW | CW_PX | CW_SC, 1 },
    };
    char *tmp, *root, *sess, *dpath, *fpath;
    size_t i;
    FILE *fp;

    tmp = st_temp();
    root = st_path("%skitty-selftest-classic-%lu",
                   tmp, (unsigned long)GetCurrentProcessId());
    sess = st_path("%s\\Sessions", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(sess, NULL);
    /* Default Settings holding the same three, for the nd case */
    dpath = st_path("%s\\Default Settings", sess);
    fp = fopen(dpath, "wb");
    if (fp) {
        fprintf(fp, "HostName=%s\nPassword=%s\nProxyPassword=%s\nScriptfileContent=%s\n",
                CHOST, CSECRET, CPROXY, classic_script_form(CSCRIPT));
        fclose(fp);
    }

    kitty_set_session_dir(sess);
    kitty_set_storage_mode(1);
    kitty_set_portable_password_protection("dpapi");
    kitty_set_script_classic_decoder(classic_script_decode);

    for (i = 0; i < lenof(cases); i++) {
        char label[64];
        fpath = st_path("%s\\%s", sess, cases[i].name);
        snprintf(label, sizeof(label), "classic file %s", cases[i].name);
        if (!classic_file(fpath, cases[i].what)) {
            check(0, "classic file: could not write the session file (skipped)");
            continue;
        }
        real_save(cases[i].name, cases[i].nd);
        check_protected_file(label, cases[i].name, fpath, cases[i].what);
        DeleteFileA(fpath);
    }

    /* legacy: clear text on purpose - the obfuscated forms written in clear */
    kitty_set_portable_password_protection("legacy");
    fpath = st_path("%s\\zz-cl-legacy", sess);
    if (classic_file(fpath, CW_PW | CW_PX | CW_SC)) {
        char *after, *pw, *px, *sc;
        settings_r *r;
        real_save("zz-cl-legacy", 0);
        after = read_whole_file(fpath);
        pw = after ? file_value(after, "Password") : NULL;
        px = after ? file_value(after, "ProxyPassword") : NULL;
        sc = after ? file_value(after, "ScriptfileContent") : NULL;
        check(pw && !strcmp(pw, CSECRET),
              "classic file, legacy: the Password stored in clear");
        /* clear already: its line stays as read (classic syntax) */
        check((px && !strcmp(px, CPROXY)) || classic_lines_kept(after, CW_PX),
              "classic file, legacy: the ProxyPassword stays in clear");
        check(sc && !strncmp(sc, "PLAIN:", 6) && script_reads_back(sc) == 0,
              "classic file, legacy: the login script stored as legacy stores it (PLAIN:)");
        {
            int n = 0;
            unsigned char *raw = sc ? ksec_b64_decode(sc + 6, &n) : NULL;
            check(raw && n == (int)strlen(CSCRIPT) && !memcmp(raw, CSCRIPT, n),
                  "classic file, legacy: ...holding the script's text");
            free(raw);
        }
        check(after && !classic_lines_kept(after, CW_PW) &&
              !classic_lines_kept(after, CW_SC),
              "classic file, legacy: no classic form left");
        r = open_settings_r("zz-cl-legacy");
        {
            char *rp = r ? read_setting_s(r, "Password") : NULL;
            check(rp && !strcmp(rp, CSECRET), "classic file, legacy: Password reads back");
            if (rp) sfree(rp);
        }
        if (r) close_settings_r(r);
        free(pw);
        free(px);
        free(sc);
        free(after);
        DeleteFileA(fpath);
    }

    /* refused: DPAPI failing - every stored value kept, the refusal reported */
    kitty_set_portable_password_protection("dpapi");
    kitty_set_master_pw_locked_save(classic_kept);
    kitty_selftest_dpapi_fail(ERROR_ACCESS_DENIED);
    fpath = st_path("%s\\zz-cl-refused", sess);
    if (classic_file(fpath, CW_PW | CW_PX | CW_SC)) {
        char *after;
        classic_reports = 0;
        real_save("zz-cl-refused", 0);
        after = read_whole_file(fpath);
        check(classic_lines_kept(after, CW_PW | CW_PX | CW_SC) &&
              !strstr(after, "Password=" CSECRET) && !strstr(after, "DPAPI1"),
              "classic file, the protection refused: every stored value kept");
        check(classic_reports > 0, "classic file, the protection refused: reported");
        free(after);
        DeleteFileA(fpath);
    }
    kitty_selftest_dpapi_fail(0);
    kitty_set_master_pw_locked_save(NULL);

    /* a write of a few values - a folder move, the Session Lockdown switch -
     * leaves the three as stored, under the master-password policy with no
     * master password unlocked: no prompt; the next real save protects them */
    kitty_set_portable_password_protection("");
    kitty_set_master_passphrase(NULL);
    kitty_set_master_pw_prompt(classic_prompt);
    fpath = st_path("%s\\zz-cl-partial", sess);
    if (classic_file(fpath, CW_PW | CW_PX | CW_SC)) {
        char *after;
        classic_prompts = 0;
        folder_write("zz-cl-partial", "Moved");
        kitty_session_set_lockdown("zz-cl-partial", 1);
        kitty_session_set_lockdown("zz-cl-partial", 0);
        after = read_whole_file(fpath);
        check(classic_prompts == 0, "classic file, a folder move and the Lockdown switch: no master-password prompt");
        check(classic_lines_kept(after, CW_PW | CW_PX | CW_SC),
              "classic file, a folder move and the Lockdown switch: the three stored as they were");
        check(after && strstr(after, "Folder=Moved") != NULL,
              "classic file, the folder move: Folder written");
        free(after);

        /* the "always" answers of a running window: one value each */
        classic_prompts = 0;
        check(kitty_session_write_value("zz-cl-partial", "OSC52ClipboardRead", NULL, 2) &&
              kitty_session_write_value("zz-cl-partial", "Far2lClientIds", "qa-far2l-id", 0),
              "classic file, the OSC 52 and far2l \"always\" answers: written");
        after = read_whole_file(fpath);
        check(classic_prompts == 0, "classic file, the \"always\" answers: no master-password prompt");
        check(classic_lines_kept(after, CW_PW | CW_PX | CW_SC),
              "classic file, the \"always\" answers: the three stored as they were");
        check(after && strstr(after, "OSC52ClipboardRead=2") &&
              strstr(after, "Far2lClientIds=qa-far2l-id"),
              "classic file, the \"always\" answers: each answer in the file");
        free(after);
        kitty_set_master_pw_prompt(NULL);
        kitty_set_portable_password_protection("dpapi");
        real_save("zz-cl-partial", 0);
        check_protected_file("classic file, the next real save", "zz-cl-partial",
                             fpath, CW_PW | CW_PX | CW_SC);
        DeleteFileA(fpath);
    }
    kitty_set_master_pw_prompt(NULL);

    kitty_set_script_classic_decoder(NULL);
    kitty_set_portable_password_protection("");
    kitty_set_storage_mode(0);
    kitty_set_session_dir("");
    DeleteFileA(dpath);
    RemoveDirectoryA(sess);
    RemoveDirectoryA(root);
}

/* The registry: classic KiTTY's hive copied value for value into ours (the
 * first start, MigrateOldKittyHive). The old hive and ours are throwaway
 * keys here. */
static void classic_reg_copy(const char *old, const char *dst, int what)
{
    HKEY src = NULL, to = NULL;
    DWORD i, maxname = 0, maxdata = 0;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, old, 0, NULL, 0,
                        KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &src,
                        NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExA(src, "HostName", 0, REG_SZ, (const BYTE *)CHOST, sizeof(CHOST));
    RegSetValueExA(src, "TerminalType", 0, REG_SZ, (const BYTE *)"xterm", 6);
    if (what & CW_PW) {
        const char *enc = classic_form(CSECRET, CHOST);
        RegSetValueExA(src, "Password", 0, REG_SZ, (const BYTE *)enc,
                       (DWORD)strlen(enc) + 1);
    }
    if (what & CW_PX)
        RegSetValueExA(src, "ProxyPassword", 0, REG_SZ, (const BYTE *)CPROXY,
                       sizeof(CPROXY));
    if (what & CW_SC) {
        const char *enc = classic_script_form(CSCRIPT);
        RegSetValueExA(src, "ScriptfileContent", 0, REG_SZ, (const BYTE *)enc,
                       (DWORD)strlen(enc) + 1);
    }
    RegQueryInfoKeyA(src, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                     &maxname, &maxdata, NULL, NULL);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, dst, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &to, NULL) == ERROR_SUCCESS) {
        char *name = snewn(maxname + 1, char);
        BYTE *data = snewn(maxdata + 1, BYTE);
        for (i = 0;; i++) {
            DWORD nl = maxname + 1, dl = maxdata + 1, type;
            if (RegEnumValueA(src, i, name, &nl, NULL, &type, data, &dl) !=
                ERROR_SUCCESS)
                break;
            RegSetValueExA(to, name, 0, type, data, dl);
        }
        sfree(name);
        sfree(data);
        RegCloseKey(to);
    }
    RegCloseKey(src);
}

static void test_classic_forms_registry(void)
{
    static const struct { const char *name; int what; int nd; } cases[] = {
        { "zz-rg-pw", CW_PW, 0 },
        { "zz-rg-px", CW_PX, 0 },
        { "zz-rg-sc", CW_SC, 0 },
        { "zz-rg-all", CW_PW | CW_PX | CW_SC, 0 },
        { "zz-rg-all-nd", CW_PW | CW_PX | CW_SC, 1 },
    };
    char *base, *oldbase;
    size_t i;

    base = st_path("Software\\KiTTYpp-selftest-mig-%lu",
                   (unsigned long)GetCurrentProcessId());
    oldbase = st_path("%s-old", base);
    kitty_set_registry_base(base);
    kitty_set_storage_mode(0);
    kitty_set_script_classic_decoder(classic_script_decode);
    /* Default Settings holding the same three, for the nd case */
    {
        char *dk = st_path("%s\\Default%%20Settings", kitty_reg_sessions());
        HKEY k;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, dk, 0, NULL, 0, KEY_SET_VALUE,
                            NULL, &k, NULL) == ERROR_SUCCESS) {
            const char *sc = classic_script_form(CSCRIPT);
            RegSetValueExA(k, "Password", 0, REG_SZ, (const BYTE *)CSECRET, sizeof(CSECRET));
            RegSetValueExA(k, "ProxyPassword", 0, REG_SZ, (const BYTE *)CPROXY, sizeof(CPROXY));
            RegSetValueExA(k, "ScriptfileContent", 0, REG_SZ, (const BYTE *)sc,
                           (DWORD)strlen(sc) + 1);
            RegCloseKey(k);
        }
    }

    for (i = 0; i < lenof(cases); i++) {
        char *old = st_path("%s\\Sessions\\%s", oldbase, cases[i].name);
        char *dst = st_path("%s\\%s", kitty_reg_sessions(), cases[i].name);
        char *raw;
        char msg[160];
        settings_r *r;
        classic_reg_copy(old, dst, cases[i].what);

        r = open_settings_r(cases[i].name);
        if (cases[i].what & CW_PW) {
            char *pw = r ? read_setting_s(r, "Password") : NULL;
            snprintf(msg, sizeof(msg), "registry %s: the copied classic password loads decoded",
                     cases[i].name);
            check(pw && !strcmp(pw, CSECRET), msg);
            if (pw) sfree(pw);
        }
        if (r) close_settings_r(r);

        /* the Lockdown switch writes one value: the three stay as stored */
        kitty_session_set_lockdown(cases[i].name, 1);
        kitty_session_set_lockdown(cases[i].name, 0);
        {
            char *p1 = reg_raw(cases[i].name, "Password");
            char *p2 = reg_raw(cases[i].name, "ProxyPassword");
            char *p3 = reg_raw(cases[i].name, "ScriptfileContent");
            snprintf(msg, sizeof(msg), "registry %s: the Lockdown switch leaves the three as stored",
                     cases[i].name);
            check((!(cases[i].what & CW_PW) || (p1 && !kitty_secret_is_marked(p1))) &&
                  (!(cases[i].what & CW_PX) || (p2 && !strcmp(p2, CPROXY))) &&
                  (!(cases[i].what & CW_SC) || (p3 && !kitty_secret_is_marked(p3))), msg);
            free(p1);
            free(p2);
            free(p3);
        }
        real_save(cases[i].name, cases[i].nd);

        if (cases[i].what & CW_PW) {
            raw = reg_raw(cases[i].name, "Password");
            snprintf(msg, sizeof(msg), "registry %s: Password stored DPAPI1", cases[i].name);
            check(raw && !strncmp(raw, "DPAPI1:", 7), msg);
            free(raw);
        }
        if (cases[i].what & CW_PX) {
            raw = reg_raw(cases[i].name, "ProxyPassword");
            snprintf(msg, sizeof(msg), "registry %s: ProxyPassword stored DPAPI1", cases[i].name);
            check(raw && !strncmp(raw, "DPAPI1:", 7), msg);
            free(raw);
        }
        if (cases[i].what & CW_SC) {
            raw = reg_raw(cases[i].name, "ScriptfileContent");
            snprintf(msg, sizeof(msg), "registry %s: login script stored protected and reads back",
                     cases[i].name);
            check(script_reads_back(raw), msg);
            free(raw);
        }
        r = open_settings_r(cases[i].name);
        if (cases[i].what & CW_PW) {
            char *pw = r ? read_setting_s(r, "Password") : NULL;
            snprintf(msg, sizeof(msg), "registry %s: Password reads back", cases[i].name);
            check(pw && !strcmp(pw, CSECRET), msg);
            if (pw) sfree(pw);
        }
        if (cases[i].what & CW_PX) {
            char *px = r ? read_setting_s(r, "ProxyPassword") : NULL;
            snprintf(msg, sizeof(msg), "registry %s: ProxyPassword reads back", cases[i].name);
            check(px && !strcmp(px, CPROXY), msg);
            if (px) sfree(px);
        }
        if (r) close_settings_r(r);
    }

    /* refused: DPAPI failing - the stored values kept, the refusal reported */
    {
        char *old = st_path("%s\\Sessions\\zz-rg-refused", oldbase);
        char *dst = st_path("%s\\zz-rg-refused", kitty_reg_sessions());
        char *pw, *px, *sc;
        classic_reg_copy(old, dst, CW_PW | CW_PX | CW_SC);
        kitty_set_master_pw_locked_save(classic_kept);
        kitty_selftest_dpapi_fail(ERROR_ACCESS_DENIED);
        classic_reports = 0;
        real_save("zz-rg-refused", 0);
        kitty_selftest_dpapi_fail(0);
        kitty_set_master_pw_locked_save(NULL);
        pw = reg_raw("zz-rg-refused", "Password");
        px = reg_raw("zz-rg-refused", "ProxyPassword");
        sc = reg_raw("zz-rg-refused", "ScriptfileContent");
        check(pw && pw[0] && !kitty_secret_is_marked(pw) &&
              px && !strcmp(px, CPROXY) &&
              sc && sc[0] && !kitty_secret_is_marked(sc),
              "registry, the protection refused: every stored value kept");
        check(classic_reports > 0, "registry, the protection refused: reported");
        free(pw);
        free(px);
        free(sc);
    }

    kitty_set_script_classic_decoder(NULL);
    RegDeleteTreeA(HKEY_CURRENT_USER, base);
    RegDeleteTreeA(HKEY_CURRENT_USER, oldbase);
    kitty_set_registry_root(0);
}

int main(int argc, char **argv)
{
    char *rotmp, *rodir, *roini, *noini;
    FILE *rofp;
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
    /* The kitty.ini in charge: none (a name nothing creates) for every test,
     * the readonly one below for the readonly tests. The machine's own
     * kitty.ini (beside the exe, %APPDATA%) is never consulted. */
    rotmp = st_temp();
    rodir = st_path("%skitty-selftest-roini-%lu",
             rotmp, (unsigned long)GetCurrentProcessId());
    roini = st_path("%s\\kitty.ini", rodir);
    noini = st_path("%s\\none\\kitty.ini", rodir);
    CreateDirectoryA(rodir, NULL);
    if ((rofp = fopen(roini, "wb")) != NULL) {
        fputs("[KiTTY]\r\nreadonly=yes\r\n", rofp);
        fclose(rofp);
    }
    kitty_inifile_pin(noini);

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
    printf("== classic KiTTY's stored forms: protected by every save ==\n");
    test_classic_forms_folder();
    test_classic_forms_registry();
    printf("== old .ktx export is still read (design doc 8.17) ==\n");
    test_old_ktx();
    printf("== login script protected like a password ==\n");
    test_script_protection();
    printf("== login script readable form ==\n");
    test_script_lines();
    printf("== login script in memory ==\n");
    test_script_in_memory();
    printf("== save only non-default settings ==\n");
    test_nondefault_file();
    test_nondefault_registry();
    printf("== readonly=yes: the store is not written ==\n");
    test_readonly_folder(roini, noini);
    test_readonly_registry(roini, noini);
    printf("== a refused wrap: DPAPI failing, the master password locked ==\n");
    test_dpapi_fail_registry();
    test_dpapi_fail_portable();
    test_mpw_locked_refuses();
    printf("== master password setup whose salt cannot be stored ==\n");
    test_mpw_setup_store_failed();
    DeleteFileA(roini);
    RemoveDirectoryA(rodir);
    st_paths_free();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
