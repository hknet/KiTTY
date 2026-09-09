/*
 * test_transfer_parse - regression tests for the OSC 5113 (kitty file
 * transfer) command parser and the file-name rules in kitty/kitty_transfer.h.
 *
 * The rules under test are what keeps a hostile far end inside the
 * destination folder: a name with "..", a backslash, a drive letter, a
 * reserved device name or a character Windows forbids must be REJECTED; the
 * paths the reference client actually sends must be accepted. The parser side
 * checks the wire details the client depends on: unpadded base64 in both
 * directions, order-independent keys, unknown keys ignored, ids that are not
 * safe strings refused. Dependency-free: builds natively on the build host.
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KT5113_PARSE_IMPL
#include "../kitty/kitty_transfer.h"

static int failures = 0;

#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", what, __LINE__); failures++; } \
} while (0)

static kt5113_cmd *parse(const char *s)
{
    kt5113_cmd *c = malloc(sizeof(*c));
    kt5113_parse(s, strlen(s), c);
    return c;
}

static void expect_basename(const char *name, int want_ok, const char *want_base)
{
    const char *b = NULL;
    size_t bl = 0;
    int ok = kt5113_basename(name, strlen(name), &b, &bl);
    if (ok != want_ok) {
        printf("FAIL basename: \"%s\" -> %s (wanted %s)\n", name,
               ok ? "accept" : "reject", want_ok ? "accept" : "reject");
        failures++;
        return;
    }
    if (ok && (bl != strlen(want_base) || memcmp(b, want_base, bl))) {
        printf("FAIL basename: \"%s\" -> \"%.*s\" (wanted \"%s\")\n", name,
               (int)bl, b, want_base);
        failures++;
    }
}

static void expect_component(const char *c, int want)
{
    int got = kt5113_component_ok(c, strlen(c));
    if (got != want) {
        printf("FAIL component: \"%s\" -> %s (wanted %s)\n", c,
               got ? "accept" : "reject", want ? "accept" : "reject");
        failures++;
    }
}

static void expect_spec(const char *name, int want_kind, const char *want_rel, char want_drive)
{
    const char *rel = NULL;
    size_t rn = 0;
    char drive = 0;
    int kind = kt5113_spec_classify(name, strlen(name), &rel, &rn, &drive);
    if (kind != want_kind) {
        printf("FAIL spec: \"%s\" -> kind %d (wanted %d)\n", name, kind, want_kind);
        failures++;
        return;
    }
    if (kind != KT5113_SPEC_BAD) {
        if (rn != strlen(want_rel) || memcmp(rel, want_rel, rn)) {
            printf("FAIL spec: \"%s\" -> rel \"%.*s\" (wanted \"%s\")\n", name,
                   (int)rn, rel, want_rel);
            failures++;
        }
        if (drive != want_drive) {
            printf("FAIL spec: \"%s\" -> drive '%c' (wanted '%c')\n", name,
                   drive ? drive : '-', want_drive ? want_drive : '-');
            failures++;
        }
    }
}

int main(void)
{
    kt5113_cmd *c;
    char b64[64];
    unsigned char raw[64];
    size_t n = 0;

    /* ---- base64 ---- */
    kt5113_b64_encode((const unsigned char *)"somefile", 8, b64);
    CHECK(!strcmp(b64, "c29tZWZpbGU"), "encode is unpadded (client rejects padding)");
    kt5113_b64_encode((const unsigned char *)"\x01\x02\x03", 3, b64);
    CHECK(!strcmp(b64, "AQID"), "encode of the spec's example");
    CHECK(kt5113_b64_decode("c29tZWZpbGU", 11, raw, sizeof(raw), &n) && n == 8 &&
          !memcmp(raw, "somefile", 8), "decode unpadded");
    CHECK(kt5113_b64_decode("c29tZWZpbGU=", 12, raw, sizeof(raw), &n) && n == 8,
          "decode padded");
    CHECK(kt5113_b64_decode("", 0, raw, sizeof(raw), &n) && n == 0, "decode empty");
    CHECK(!kt5113_b64_decode("c29t ZWZ", 8, raw, sizeof(raw), &n), "decode refuses a space");
    CHECK(!kt5113_b64_decode("A", 1, raw, sizeof(raw), &n), "decode refuses a lone sextet");
    CHECK(!kt5113_b64_decode("AQIDAQID", 8, raw, 4, &n), "decode refuses to overflow");

    /* ---- the spec's serialisation example ---- */
    c = parse("ac=send;id=test;n=c29tZWZpbGU=;sz=3;d=AQID");
    CHECK(c->action == KT5113_AC_SEND, "action send");
    CHECK(!strcmp(c->id, "test"), "id");
    CHECK(c->name_len == 8 && !strcmp(c->name, "somefile"), "name decoded");
    CHECK(c->has_size && c->size == 3, "size");
    CHECK(c->has_data && c->data_len == 3 && c->data[0] == 1 && c->data[2] == 3, "data");
    CHECK(!c->bad, "not bad");
    free(c);

    /* ---- what the Go client sends: id first, then the payload keys in any order ---- */
    c = parse("id=abc-1.2/x@y:z;tt=rsync;fid=1a;ac=file;zip=zlib;prm=420;mod=1700000000123456789;n=fi9yZXBvcnQucGRm");
    CHECK(c->action == KT5113_AC_FILE, "file action");
    CHECK(!strcmp(c->id, "abc-1.2/x@y:z"), "safe-string id with every allowed char");
    CHECK(!strcmp(c->fid, "1a"), "fid");
    CHECK(c->ttype == KT5113_TT_RSYNC, "tt=rsync");
    CHECK(c->zip == KT5113_ZIP_ZLIB, "zip=zlib");
    CHECK(c->has_perms && c->perms == 420, "prm decimal");
    CHECK(c->has_mtime && c->mtime == 1700000000123456789LL, "mod 64-bit");
    CHECK(!strcmp(c->name, "~/report.pdf"), "name ~/report.pdf");
    CHECK(c->ftype == KT5113_FT_REGULAR, "ft defaults to regular");
    free(c);

    c = parse("id=s;ac=receive;sz=2;q=1;pw=c2VjcmV0");
    CHECK(c->action == KT5113_AC_RECEIVE && c->size == 2 && c->quiet == 1, "receive with quiet");
    CHECK(c->has_bypass, "bypass noticed (and never honoured)");
    free(c);

    c = parse("id=s;ac=end_data;fid=7;ft=directory");
    CHECK(c->action == KT5113_AC_END_DATA && !c->has_data && c->data_len == 0, "end_data without d=");
    CHECK(c->ftype == KT5113_FT_DIRECTORY, "ft=directory");
    free(c);

    /* ---- unknown keys are ignored; malformed pairs skipped ---- */
    c = parse("id=s;ac=cancel;future_key=whatever;=novalue;nokey;ac2=file");
    CHECK(c->action == KT5113_AC_CANCEL && !c->bad, "unknown keys ignored");
    free(c);
    c = parse("id=s;ac=frobnicate");
    CHECK(c->action == KT5113_AC_UNKNOWN, "unknown action flagged");
    free(c);
    c = parse("ac=send");
    CHECK(c->id[0] == '\0', "missing id stays empty");
    free(c);

    /* ---- ids must be safe strings: a ';' or ESC could forge our framing ---- */
    c = parse("id=bad\x1bid;ac=send");
    CHECK(c->bad, "ESC in id refused");
    free(c);
    c = parse("id=has space;ac=send");
    CHECK(c->bad, "space in id refused");
    free(c);
    {
        char longid[200];
        memset(longid, 'a', sizeof(longid) - 1);
        longid[sizeof(longid) - 1] = '\0';
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "id=%s;ac=send", longid);
        c = parse(cmd);
        CHECK(c->bad, "over-long id refused");
        free(c);
    }
    c = parse("id=s;ac=file;fid=f1;n=not*base64!");
    CHECK(c->name_bad && c->name_len == 0, "bad base64 name flagged");
    free(c);
    c = parse("id=s;ac=data;fid=f1;d=####");
    CHECK(c->data_bad && c->data_len == 0, "bad base64 data flagged");
    free(c);
    c = parse("id=s;ac=file;fid=f1;n=YQBi");           /* "a\0b" */
    CHECK(c->name_bad, "embedded NUL in name refused");
    free(c);
    c = parse("id=s;ac=file;fid=f1;sz=99999999999999999999");
    CHECK(!c->has_size, "integer overflow ignored");
    free(c);
    c = parse("id=s;ac=file;fid=f1;sz=-1");
    CHECK(c->has_size && c->size == -1, "negative size parsed");
    free(c);

    /* ---- file names from the far end: basenames ---- */
    expect_basename("~/report.pdf", 1, "report.pdf");
    expect_basename("/home/user/dir/file.txt", 1, "file.txt");
    expect_basename("report.pdf", 1, "report.pdf");
    expect_basename("/C:/Users/x/file.txt", 1, "file.txt");
    expect_basename("~/dir/", 1, "dir");
    expect_basename("~/\xc3\xa9t\xc3\xa9.txt", 1, "\xc3\xa9t\xc3\xa9.txt");
    expect_basename("../evil.txt", 0, "");
    expect_basename("~/../evil.txt", 0, "");
    expect_basename("/home/../../evil.txt", 0, "");
    expect_basename("a/..", 0, "");
    expect_basename("C:\\evil.txt", 0, "");
    expect_basename("..\\evil.txt", 0, "");
    expect_basename("dir\\file.txt", 0, "");
    expect_basename("", 0, "");
    expect_basename("/", 0, "");
    expect_basename("~/", 0, "");
    expect_basename(".", 0, "");
    expect_basename("~/CON", 0, "");
    expect_basename("~/nul.txt", 0, "");
    expect_basename("~/com1", 0, "");
    expect_basename("~/lpt9.log", 0, "");
    expect_basename("~/file.txt:stream", 0, "");
    expect_basename("~/a<b", 0, "");
    expect_basename("~/a|b", 0, "");
    expect_basename("~/a?b", 0, "");
    expect_basename("~/a*b", 0, "");
    expect_basename("~/a\"b", 0, "");
    expect_basename("~/trailing.", 0, "");
    expect_basename("~/trailing ", 0, "");
    expect_basename("~/ctl\x01char", 0, "");
    expect_basename("~/tab\tchar", 0, "");
    {
        char longname[300];
        memset(longname, 'x', sizeof(longname) - 1);
        longname[sizeof(longname) - 1] = '\0';
        expect_basename(longname, 0, "");
    }

    /* ---- components (used for every level under a declared directory) ---- */
    expect_component("ok name.txt", 1);
    expect_component("CONSOLE", 1);       /* not a device name */
    expect_component("con.", 0);
    expect_component("..", 0);
    expect_component(".", 0);
    expect_component(".hidden", 1);
    expect_component("a/b", 0);

    /* ---- inside a declared directory ---- */
    {
        const char *rel = NULL; size_t rn = 0;
        CHECK(kt5113_under("~/dest/dir", 10, "~/dest/dir/sub/f.txt", 20, &rel, &rn) &&
              rn == 9 && !memcmp(rel, "sub/f.txt", 9), "under: relative remainder");
        CHECK(!kt5113_under("~/dest/dir", 10, "~/dest/dir2/f.txt", 17, &rel, &rn),
              "under: prefix must end at a slash");
        CHECK(!kt5113_under("~/dest/dir", 10, "~/dest/dir", 10, &rel, &rn),
              "under: the directory itself is not inside itself");
        CHECK(kt5113_rel_ok("sub/f.txt", 9) == 2, "rel components ok");
        CHECK(kt5113_rel_ok("sub/../f.txt", 12) == 0, "rel refuses ..");
        CHECK(kt5113_rel_ok("sub//f.txt", 10) == 0, "rel refuses an empty component");
        CHECK(kt5113_rel_ok("sub/nul", 7) == 0, "rel refuses a device name");
    }

    /* ---- paths the far end asks to READ ---- */
    expect_spec("~/report.pdf", KT5113_SPEC_RELATIVE, "report.pdf", 0);
    expect_spec("report.pdf", KT5113_SPEC_RELATIVE, "report.pdf", 0);
    expect_spec("/report.pdf", KT5113_SPEC_RELATIVE, "report.pdf", 0);
    expect_spec("/some/posix/path.txt", KT5113_SPEC_RELATIVE, "some/posix/path.txt", 0);
    expect_spec("~", KT5113_SPEC_RELATIVE, "", 0);
    expect_spec("~/", KT5113_SPEC_RELATIVE, "", 0);
    expect_spec("/C:/Users/x/file.txt", KT5113_SPEC_DRIVE, "Users/x/file.txt", 'C');
    expect_spec("/d:/", KT5113_SPEC_DRIVE, "", 'd');
    expect_spec("/C:", KT5113_SPEC_DRIVE, "", 'C');
    expect_spec("~/../x", KT5113_SPEC_BAD, "", 0);
    expect_spec("../x", KT5113_SPEC_BAD, "", 0);
    expect_spec("/C:/../x", KT5113_SPEC_BAD, "", 0);
    expect_spec("C:\\x", KT5113_SPEC_BAD, "", 0);
    expect_spec("~user/x", KT5113_SPEC_BAD, "", 0);
    expect_spec("/C:/x:stream", KT5113_SPEC_BAD, "", 0);
    expect_spec("~/nul", KT5113_SPEC_BAD, "", 0);
    expect_spec("", KT5113_SPEC_BAD, "", 0);

    /* ---- Adler-32 (RFC 1950 example: "Wikipedia" -> 0x11E60398) ---- */
    CHECK(kt5113_adler32(1, (const unsigned char *)"Wikipedia", 9) == 0x11E60398u, "adler32");
    CHECK(kt5113_adler32(1, (const unsigned char *)"", 0) == 1, "adler32 of nothing");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_transfer_parse: all checks passed\n");
    return 0;
}
