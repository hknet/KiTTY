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
void load_open_settings_forced(char *filename, Conf *conf);   /* kitty_settings_load.c */

const struct BackendVtable *const backends[] = { NULL };
const int be_default_protocol = 0;
const struct keyvalwhere gsslibkeywords[] = { { "", 0, -1, -1 } };
const int ngsslibs = 0;
Conf *conf = NULL;                  /* the app's active-seat global */
bool conf_launchable(Conf *c) { return true; }
char *get_username(void) { return dupstr("selftest"); }
void burnwcs(wchar_t *s) { if (s) { while (*s) *s++ = 0; } }
char *GetValueData(HKEY k, char *sub, const char *name, char *out)
{ return NULL; }                    /* "no such registry value" */
int readINI(const char *f, const char *sec, const char *key, char *p, size_t n)
{ return 0; }                       /* "no such ini key" */
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

/* ---------- reading a session out of the OLD 9bis KiTTY hive (§8.10) ----------
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
}

/* ---------- reading an old .ktx export (§8.17) ----------
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
     * decoder. */
    check(!strcmp(conf_get_str(conf, CONF_password), "ktx-secret"),
          "ktx: PLAIN: cleartext password imports verbatim");

    conf_free(conf);
    DeleteFileA(path);
}

int main(void)
{
    printf("== registry backend (DPAPI) ==\n");
    test_registry();
    printf("== portable backend (master password) ==\n");
    test_portable();
    printf("== long settings values (cyd01/KiTTY#541) ==\n");
    test_long_values();
    printf("== renamed setting migrates on save ==\n");
    test_renamed_key();
    printf("== old 9bis KiTTY hive is still read (design doc 8.10) ==\n");
    test_old_kitty_hive();
    printf("== old .ktx export is still read (design doc 8.17) ==\n");
    test_old_ktx();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
