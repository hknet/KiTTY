/*
 * test_inifile - the readonly=yes gate of kitty/kitty_inifile.c: with
 * [KiTTY] readonly=yes in the resolved kitty.ini, kitty_inifile_write leaves
 * that file byte-identical (a new key, a changed key, a removed key) and
 * kitty_inifile_write_blocked holds back that file under any spelling of its
 * name, and no other; a second kitty.ini without the key, and one with
 * readonly=no, are written as before. Everything in a private %TEMP% folder,
 * removed afterwards; never the registry.
 *
 * Returns non-zero on any failure.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
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

static int ini_holds(const char *path, const char *sec, const char *key,
                     const char *want)
{
    char v[64];
    GetPrivateProfileStringA(sec, key, "\x01", v, sizeof(v), path);
    return !strcmp(v, want);
}

int main(void)
{
    char tmp[MAX_PATH], root[MAX_PATH], sub[MAX_PATH];
    char ro[MAX_PATH], ro_alt[MAX_PATH], other[MAX_PATH], rw[MAX_PATH];
    char env[MAX_PATH + 32];
    char *before, *after;
    long nb = 0, na = 0;
    const char *f;

    GetTempPathA(sizeof(tmp), tmp);
    snprintf(root, sizeof(root), "%skitty-inifile-%lu",
             tmp, (unsigned long)GetCurrentProcessId());
    snprintf(sub, sizeof(sub), "%s\\rw", root);
    snprintf(ro, sizeof(ro), "%s\\kitty.ini", root);
    snprintf(ro_alt, sizeof(ro_alt), "%s\\rw\\..\\KITTY.INI", root);
    snprintf(other, sizeof(other), "%s\\kitty_windowpos.ini", root);
    snprintf(rw, sizeof(rw), "%s\\kitty.ini", sub);
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
    snprintf(env, sizeof(env), "KITTY_INI_FILE=%s", ro);
    _putenv(env);

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

    DeleteFileA(ro);
    DeleteFileA(other);
    DeleteFileA(rw);
    RemoveDirectoryA(sub);
    RemoveDirectoryA(root);

    printf("%s\n", failures ? "test_inifile: FAILED" : "test_inifile: all ok");
    return failures ? 1 : 0;
}
