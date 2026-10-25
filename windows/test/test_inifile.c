/*
 * test_inifile - the readonly=yes gate of kitty/kitty_inifile.c: with
 * [KiTTY] readonly=yes in the resolved kitty.ini, kitty_inifile_write leaves
 * that file byte-identical (a new key, a changed key, a removed key) and
 * kitty_inifile_write_blocked holds back that file under any spelling of its
 * name, and no other; a second kitty.ini without the key, and one with
 * readonly=no, are written as before. The store gate on top of it
 * (kitty_store_readonly, kitty_store_ro_regpath): which registry keys are
 * the store, the savemode=file load scope, the answer cached and read again
 * when the file changes. Everything in a private %TEMP% folder, removed
 * afterwards; never the registry (the key test only compares names).
 *
 * Returns non-zero on any failure.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "kitty_inifile.h"
#include "kitty_inikeys.h"

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

/* The whole file, malloc'd; NULL when it cannot be read. */
static char *slurp(const char *path, long *len)
{
    FILE *fp = fopen(path, "rb");
    char *buf;
    long n;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc(n + 1);
    if (buf && fread(buf, 1, n, fp) != (size_t)n) { free(buf); buf = NULL; }
    fclose(fp);
    if (buf) { buf[n] = '\0'; *len = n; }
    return buf;
}

static int put_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    fputs(text, fp);
    return fclose(fp) == 0;
}

static int same_bytes(const char *a, long na, const char *b, long nb)
{
    return a && b && na == nb && !memcmp(a, b, na);
}

/* A path built whole (malloc'd); a failed build ends the run. */
static char *tf_path(const char *fmt, ...)
{
    va_list ap;
    int n;
    char *s;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !(s = malloc((size_t)n + 1))) {
        printf("FAIL  a test path could not be built\n");
        exit(1);
    }
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

/* %TEMP% whole, with its backslash; a failed read ends the run - a test
 * never builds its folder from an unfilled buffer. */
static char *tf_temp(void)
{
    DWORD need = GetTempPathA(0, NULL), n;
    char *s;
    if (!need || !(s = malloc(need))) {
        printf("FAIL  GetTempPath failed (%lu)\n", (unsigned long)GetLastError());
        exit(1);
    }
    n = GetTempPathA(need, s);
    if (!n || n >= need) {
        printf("FAIL  GetTempPath failed (%lu)\n", (unsigned long)GetLastError());
        exit(1);
    }
    return s;
}

static int ini_holds(const char *path, const char *sec, const char *key,
                     const char *want)
{
    char v[64];
    GetPrivateProfileStringA(sec, key, "\x01", v, sizeof(v), path);
    return !strcmp(v, want);
}

int main(void)
{
    char *tmp, *root, *sub, *ro, *ro_alt, *other, *rw, *env;
    char *before, *after;
    long nb = 0, na = 0;
    const char *f;

    tmp = tf_temp();
    root = tf_path("%skitty-inifile-%lu",
                   tmp, (unsigned long)GetCurrentProcessId());
    sub = tf_path("%s\\rw", root);
    ro = tf_path("%s\\kitty.ini", root);
    ro_alt = tf_path("%s\\rw\\..\\KITTY.INI", root);
    other = tf_path("%s\\kitty_windowpos.ini", root);
    rw = tf_path("%s\\kitty.ini", sub);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(sub, NULL);

    put_file(ro,
             "[KiTTY]\r\n"
             "readonly=Yes\r\n"
             "theme=dark\r\n"
             "[Agent]\r\n"
             "startupkey1=a.ppk\r\n");
    put_file(other, "[Pos]\r\nx=1\r\n");
    put_file(rw, "[KiTTY]\r\ntheme=dark\r\n");

    /* the resolver reads the CRT environment (getenv), before its first use */
    env = tf_path("KITTY_INI_FILE=%s", ro);
    _putenv(env);                    /* the CRT copies it */

    f = kitty_inifile_file();
    check(f && !stricmp(f, ro), "the resolver takes the readonly kitty.ini");
    check(kitty_inifile_readonly(), "readonly=Yes is read as readonly");

    /* the store's kitty.ini: nothing written, every call reports success */
    before = slurp(ro, &nb);
    check(before != NULL, "the readonly kitty.ini is readable");
    check(kitty_inifile_write(KI_SECTION_KITTY, KI_THEME, "light") == 1,
          "readonly: a changed key reports success");
    check(kitty_inifile_write(KI_SECTION_AGENT, "startupkey2", "b.ppk") == 1,
          "readonly: a new key reports success");
    check(kitty_inifile_write(KI_SECTION_AGENT, "startupkey1", NULL) == 1,
          "readonly: a removed key reports success");
    after = slurp(ro, &na);
    check(same_bytes(before, nb, after, na),
          "readonly: kitty.ini is byte-identical afterwards");
    free(before);
    free(after);
    /* ...and the changes hold for the running program */
    {
        char v[64];
        check(kitty_inifile_read(KI_SECTION_KITTY, KI_THEME, v, sizeof(v)) &&
              !strcmp(v, "light"), "readonly: a changed key reads back changed");
        check(kitty_inifile_read(KI_SECTION_AGENT, "startupkey2", v, sizeof(v)) &&
              !strcmp(v, "b.ppk"), "readonly: a new key reads back");
        check(!kitty_inifile_read(KI_SECTION_AGENT, "startupkey1", v, sizeof(v)),
              "readonly: a removed key reads back absent");
        check(kitty_inifile_readonly(), "readonly: still read from the file");
    }

    /* the predicate for the writers that name the file themselves */
    check(kitty_inifile_write_blocked(ro), "blocked: the store's kitty.ini");
    check(kitty_inifile_write_blocked(ro_alt),
          "blocked: the same file under another spelling");
    check(!kitty_inifile_write_blocked(other),
          "not blocked: another ini beside it");
    check(!kitty_inifile_write_blocked(NULL) && !kitty_inifile_write_blocked(""),
          "not blocked: no file named");
    check(WritePrivateProfileStringA("Pos", "x", "2", other) &&
          ini_holds(other, "Pos", "x", "2"),
          "another ini is written as before");

    /* another kitty.ini in charge, without the key: written as before */
    kitty_inifile_pin(rw);
    check(!kitty_inifile_readonly(), "no readonly key: not readonly");
    check(!kitty_inifile_write_blocked(ro),
          "not blocked: the readonly file, now not the store's");
    check(kitty_inifile_write(KI_SECTION_KITTY, KI_THEME, "light") == 1 &&
          ini_holds(rw, KI_SECTION_KITTY, KI_THEME, "light"),
          "no readonly key: the write lands");

    /* readonly=no: written as before */
    check(WritePrivateProfileStringA(KI_SECTION_KITTY, KI_READONLY, "no", rw),
          "readonly=no set");
    check(!kitty_inifile_readonly() && !kitty_inifile_write_blocked(rw),
          "readonly=no: not readonly");
    check(kitty_inifile_write(KI_SECTION_AGENT, "startupkey1", "c.ppk") == 1 &&
          ini_holds(rw, KI_SECTION_AGENT, "startupkey1", "c.ppk"),
          "readonly=no: the write lands");

    /* and switched on again in the running process: held back at once */
    check(WritePrivateProfileStringA(KI_SECTION_KITTY, KI_READONLY, "yes", rw),
          "readonly=yes set");
    check(kitty_inifile_write(KI_SECTION_AGENT, "startupkey1", "d.ppk") == 1 &&
          ini_holds(rw, KI_SECTION_AGENT, "startupkey1", "c.ppk"),
          "readonly=yes read fresh: the write is skipped");

    /* the store gate: kitty_store_readonly follows the file */
    check(kitty_store_readonly(), "store: readonly=yes makes the store read-only");
    kitty_store_ro_allow(1);
    check(!kitty_store_readonly() && kitty_inifile_readonly(),
          "store: the savemode=file load scope lets the hive be written");
    kitty_store_ro_allow(1);
    kitty_store_ro_allow(0);
    check(!kitty_store_readonly(), "store: the scope nests");
    kitty_store_ro_allow(0);
    check(kitty_store_readonly(), "store: closed again, read-only again");

    /* which registry keys are the store (names only, nothing opened) */
    check(kitty_store_ro_regpath(1, "Software\\kapper.net\\KiTTY"),
          "regpath: KiTTY++'s hive");
    check(kitty_store_ro_regpath(1, "Software\\kapper.net\\KiTTY\\Sessions\\x") &&
          kitty_store_ro_regpath(1, "software\\KAPPER.NET\\kitty\\AuxWinPos"),
          "regpath: a key below it, any case");
    check(kitty_store_ro_regpath(1, "Software\\kapper.net\\KiTTY_save"),
          "regpath: the parked copy of savemode=file");
    check(kitty_store_ro_regpath(1, "Software\\kapper.net") &&
          kitty_store_ro_regpath(1, "Software"),
          "regpath: a key above it (a tree delete there takes the store)");
    check(!kitty_store_ro_regpath(0, "Software\\kapper.net\\KiTTY"),
          "regpath: not under HKEY_CURRENT_USER");
    check(!kitty_store_ro_regpath(1, "Software\\Microsoft\\Windows\\CurrentVersion\\Run") &&
          !kitty_store_ro_regpath(1, "Software\\Classes\\ssh") &&
          !kitty_store_ro_regpath(1, "Environment"),
          "regpath: the Run key, file associations, PATH are not the store");
    check(!kitty_store_ro_regpath(1, "Software\\SimonTatham\\PuTTY\\Sessions"),
          "regpath: PuTTY's hive while it is not in use");
    kitty_store_ro_set_regbase("Software\\SimonTatham\\PuTTY");
    check(kitty_store_ro_regpath(1, "Software\\SimonTatham\\PuTTY\\Sessions"),
          "regpath: PuTTY's hive once it is the hive in use (KiClassName=PuTTY)");
    kitty_store_ro_set_regbase("");
    kitty_store_ro_allow(1);
    check(!kitty_store_ro_regpath(1, "Software\\kapper.net\\KiTTY\\Sessions"),
          "regpath: inside the load scope, the hive is written");
    kitty_store_ro_allow(0);

    /* readonly=no again: nothing refused */
    check(WritePrivateProfileStringA(KI_SECTION_KITTY, KI_READONLY, "no", rw),
          "readonly=no set again");
    check(!kitty_store_readonly() &&
          !kitty_store_ro_regpath(1, "Software\\kapper.net\\KiTTY"),
          "store: readonly=no - the hive is written");

    /* the hive's own readonly value (kitty.exe, SetReadOnlyFlag): switched
     * on for the rest of the process, whatever the file holds - last */
    kitty_inifile_readonly_force(1);
    check(kitty_store_readonly() && kitty_inifile_readonly(),
          "force: readonly from the hive holds against readonly=no");

    DeleteFileA(ro);
    DeleteFileA(other);
    DeleteFileA(rw);
    RemoveDirectoryA(sub);
    RemoveDirectoryA(root);
    free(tmp); free(root); free(sub); free(ro); free(ro_alt);
    free(other); free(rw); free(env);

    printf("%s\n", failures ? "test_inifile: FAILED" : "test_inifile: all ok");
    return failures ? 1 : 0;
}
