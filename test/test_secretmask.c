/*
 * test_secretmask - regression tests for kitty/kitty_secretmask.c: secrets
 * masked out of a command line in each form a hand-off writes them (as they
 * are, Win32-quoted, "" doubled, percent-encoded), and spans.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kitty/kitty_secretmask.h"

void smemclr(void *b, size_t len) { memset(b, 0, len); }   /* utils/smemclr.c stand-in */

static int failures = 0;

static void masked(const char *what, const char *line, const char *secret,
                   const char *want)
{
    char *b = malloc(strlen(line) + 1);
    strcpy(b, line);
    kitty_mask_secret(b, secret);
    if (strcmp(b, want)) {
        printf("FAIL [%s]:\n  got  <%s>\n  want <%s>\n", what, b, want);
        failures++;
    }
    free(b);
}

static void span(const char *what, const char *line, size_t at, size_t len,
                 const char *want)
{
    char *b = malloc(strlen(line) + 1);
    strcpy(b, line);
    kitty_mask_span(b, at, len);
    if (strcmp(b, want)) {
        printf("FAIL [%s]: got <%s>, want <%s>\n", what, b, want);
        failures++;
    }
    free(b);
}

int main(void)
{
    masked("plain", "pscp -pw s3cret host", "s3cret", "pscp -pw ****** host");
    masked("twice", "a s3cret b s3cret", "s3cret", "a ****** b ******");
    masked("Win32-quoted", "kscp -pw \"a\\\"b\" h", "a\"b",
           "kscp -pw \"****\" h");
    masked("Win32-quoted, trailing backslash", "x \"p\\\\\" y", "p\\",
           "x \"***\" y");
    masked("rawcat", "/rawsettings X=\"a\"\"b\"", "a\"b",
           "/rawsettings X=\"****\"");
    masked("URL, upper hex", "sftp://u:p%40ss%2F@h/", "p@ss/",
           "sftp://u:*********@h/");
    masked("URL, lower hex", "sftp://u:p%40ss%2f@h/", "p@ss/",
           "sftp://u:*********@h/");
    masked("unreserved stays as is in a URL", "ftp://u:Ab-9.~_@h", "Ab-9.~_",
           "ftp://u:*******@h");
    masked("not there", "winscp.exe /ini=nul", "s3cret", "winscp.exe /ini=nul");
    masked("empty secret", "abc", "", "abc");
    masked("NULL secret", "abc", NULL, "abc");
    span("span", "-pw secret x", 4, 6, "-pw ****** x");
    span("span past the end", "-pw sec", 4, 99, "-pw ***");
    span("span outside", "abc", 5, 2, "abc");
    span("empty span", "abc", 1, 0, "abc");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_secretmask: all passed\n");
    return 0;
}
