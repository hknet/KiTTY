/*
 * kitty_knownhosts.c - OpenSSH's known_hosts as kageant reads it, only
 * while its setting is on (kitty_knownhosts.h). Moved here from
 * kitty_kageant_dest.c so that the gate and the parsing are in one file
 * the unit test can reach.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"

#include "kitty_knownhosts.h"
#include "kitty_agentrestrict.h"    /* kar_wildcard */

char *(*kkh_path_hook)(void) = NULL;
unsigned long kkh_opened = 0;

/* The file, opened for reading; NULL when the setting is off or it is not
 * there. */
static FILE *kkh_open(void)
{
    char *path;
    FILE *fp;

    if (!kkh_path_hook || (path = kkh_path_hook()) == NULL)
        return NULL;
    fp = fopen(path, "r");
    sfree(path);
    if (fp)
        kkh_opened++;
    return fp;
}

/* A host name with its port when that is not 22. sfree it. */
static char *kkh_host_with_port(const char *host, int port)
{
    return (port > 0 && port != 22) ? dupprintf("%s:%d", host, port)
                                    : dupstr(host);
}

/* Split "[@marker] hosts keytype base64 [comment]" in place: *hosts and
 * *b64 point into line, *marker is the marker without "@" or "". false
 * for a comment, an empty line or a line without a key. */
static bool kkh_split(char *line, char **marker, char **hosts, char **b64)
{
    char *p = line;

    *marker = "";
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#')
        return false;
    if (*p == '@') {
        *marker = p + 1;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (!*p)
            return false;
        *p++ = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
    }
    *hosts = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    if (!*p)
        return false;
    *p++ = '\0';
    while (*p == ' ' || *p == '\t')
        p++;
    while (*p && *p != ' ' && *p != '\t')      /* the key type */
        p++;
    while (*p == ' ' || *p == '\t')
        p++;
    *b64 = p;
    while (*p && !strchr(" \t\r\n", *p))
        p++;
    *p = '\0';
    return **b64 && base64_valid(ptrlen_from_asciz(*b64));
}

void kkh_names(ptrlen keyblob, void (*add)(void *ctx, const char *name),
               void *ctx)
{
    FILE *fp = kkh_open();
    char *line;

    if (!fp)
        return;
    while ((line = fgetline(fp)) != NULL) {
        char *marker, *hosts, *b64, *tok, *next;
        strbuf *blob;
        bool match;
        if (!kkh_split(line, &marker, &hosts, &b64) || *marker ||
            !strncmp(hosts, "|1|", 3)) {
            sfree(line);
            continue;
        }
        blob = base64_decode_sb(ptrlen_from_asciz(b64));
        match = ptrlen_eq_ptrlen(ptrlen_from_strbuf(blob), keyblob);
        strbuf_free(blob);
        for (tok = match ? hosts : NULL; tok; tok = next) {
            char *rb, *name;
            next = strchr(tok, ',');
            if (next)
                *next++ = '\0';
            if (!*tok || *tok == '!' || strpbrk(tok, "*?"))
                continue;
            if (*tok == '[' && (rb = strstr(tok, "]:")) != NULL) {
                *rb = '\0';
                name = kkh_host_with_port(tok + 1, atoi(rb + 2));
            } else {
                name = dupstr(tok);
            }
            add(ctx, name);
            sfree(name);
        }
        sfree(line);
    }
    fclose(fp);
}

bool kkh_count(size_t *total, size_t *hashed)
{
    FILE *fp = kkh_open();
    char *line;

    *total = *hashed = 0;
    if (!fp)
        return false;
    while ((line = fgetline(fp)) != NULL) {
        char *marker, *hosts, *b64;
        if (kkh_split(line, &marker, &hosts, &b64) && !*marker) {
            (*total)++;
            if (!strncmp(hosts, "|1|", 3))
                (*hashed)++;
        }
        sfree(line);
    }
    fclose(fp);
    return true;
}

/* "host" for port 22, else "[host]:port", lower case: the name a
 * known_hosts entry is written with. sfree it. */
static char *kkh_target(const char *host, int port)
{
    char *t = (port > 0 && port != 22) ? dupprintf("[%s]:%d", host, port)
                                       : dupstr(host);
    for (char *p = t; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    return t;
}

/* Does a host field (comma list of names or patterns, ! for a negation,
 * or one hashed |1|salt|hash entry) take this target? */
static bool kkh_hosts_match(const char *field, const char *target)
{
    bool matched = false;
    char *copy, *tok, *next;

    if (!strncmp(field, "|1|", 3)) {
        const char *salt64 = field + 3, *bar = strchr(salt64, '|');
        strbuf *salt, *want;
        unsigned char got[20];
        ssh2_mac *mac;
        bool ok;
        if (!bar)
            return false;
        if (!base64_valid(make_ptrlen(salt64, bar - salt64)) ||
            !base64_valid(ptrlen_from_asciz(bar + 1)))
            return false;
        salt = base64_decode_sb(make_ptrlen(salt64, bar - salt64));
        want = base64_decode_sb(ptrlen_from_asciz(bar + 1));
        mac = ssh2_mac_new(&ssh_hmac_sha1, NULL);
        ssh2_mac_setkey(mac, ptrlen_from_strbuf(salt));
        ssh2_mac_start(mac);
        put_dataz(mac, target);
        ssh2_mac_genresult(mac, got);
        ssh2_mac_free(mac);
        ok = want->len == sizeof(got) && smemeq(want->u, got, sizeof(got));
        strbuf_free(salt);
        strbuf_free(want);
        return ok;
    }
    copy = dupstr(field);
    for (char *p = copy; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    for (tok = copy; tok; tok = next) {
        bool neg = false;
        next = strchr(tok, ',');
        if (next)
            *next++ = '\0';
        if (*tok == '!') {
            neg = true;
            tok++;
        }
        if (*tok && kar_wildcard(target, tok)) {
            if (neg) {
                matched = false;
                break;              /* a negated match excludes the host */
            }
            matched = true;
        }
    }
    sfree(copy);
    return matched;
}

/* A pattern against a name, case ignored. */
static bool kkh_pattern_takes(const char *pattern, const char *name)
{
    char *n = dupstr(name), *p = dupstr(pattern);
    bool ok;
    for (char *q = n; *q; q++)
        if (*q >= 'A' && *q <= 'Z')
            *q += 'a' - 'A';
    for (char *q = p; *q; q++)
        if (*q >= 'A' && *q <= 'Z')
            *q += 'a' - 'A';
    ok = kar_wildcard(n, p);
    sfree(n);
    sfree(p);
    return ok;
}

/* A pattern lookup: does a plain name of a host field ("name" or
 * "[name]:port"; a hashed entry, a negation or a pattern is no name) on
 * the port match the pattern? */
static bool kkh_names_take(const char *pattern, int port, const char *field)
{
    char *copy, *tok, *next;
    bool ok = false;

    if (!strncmp(field, "|1|", 3))
        return false;
    copy = dupstr(field);
    for (tok = copy; tok && !ok; tok = next) {
        char *rb;
        int p = 22;
        next = strchr(tok, ',');
        if (next)
            *next++ = '\0';
        if (!*tok || *tok == '!' || strpbrk(tok, "*?"))
            continue;
        if (*tok == '[' && (rb = strstr(tok, "]:")) != NULL) {
            *rb = '\0';
            p = atoi(rb + 2);
            tok++;
        }
        ok = p == port && kkh_pattern_takes(pattern, tok);
    }
    sfree(copy);
    return ok;
}

void kkh_lookup(const char *host, int port, bool pattern,
                void (*add)(void *ctx, ptrlen blob, bool is_ca), void *ctx)
{
    FILE *fp;
    char *line, *target;

    if (!host || !*host || (fp = kkh_open()) == NULL)
        return;
    if (port <= 0)
        port = 22;
    target = kkh_target(host, port);
    while ((line = fgetline(fp)) != NULL) {
        char *marker, *hosts, *b64;
        bool is_ca = false;
        if (!kkh_split(line, &marker, &hosts, &b64)) {
            sfree(line);
            continue;
        }
        if (*marker) {
            /* a pattern takes plain entries only; of the markers only
             * @cert-authority holds a key that vouches for hosts */
            if (pattern || strcmp(marker, "cert-authority")) {
                sfree(line);
                continue;
            }
            is_ca = true;
        }
        if (pattern ? kkh_names_take(host, port, hosts)
                    : kkh_hosts_match(hosts, target)) {
            strbuf *blob = base64_decode_sb(ptrlen_from_asciz(b64));
            add(ctx, ptrlen_from_strbuf(blob), is_ca);
            strbuf_free(blob);
        }
        sfree(line);
    }
    sfree(target);
    fclose(fp);
}
