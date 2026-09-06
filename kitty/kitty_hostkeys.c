/*
 * kitty_hostkeys.c: the stored SSH host keys, listed and described.
 * See kitty_hostkeys.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "putty.h"
#include "ssh.h"
#include "mpint.h"
#include "storage.h"
#include "kitty_storage.h"
#include "kitty_hostkeys.h"

extern int existfile(const char *filename);                 /* kitty_tools.c */

/* ---- the algorithm behind a store id -------------------------------------- */

/* The host-key algorithms KiTTY negotiates (the same set as
 * ssh2_hostkey_algs[] in ssh/transport2.c, whose header drags the whole SSH
 * layer in), matched by the id the store files them under. */
static const ssh_keyalg *alg_for_cache_id(const char *keytype)
{
    static const ssh_keyalg *const algs[] = {
        &ssh_ecdsa_ed25519, &ssh_ecdsa_ed448,
        &ssh_ecdsa_nistp256, &ssh_ecdsa_nistp384, &ssh_ecdsa_nistp521,
        &ssh_rsa, &ssh_dsa,
    };
    for (size_t i = 0; i < lenof(algs); i++)
        if (!strcmp(algs[i]->cache_id, keytype))
            return algs[i];
    return NULL;
}

/* ---- the stored text -> the public blob ---------------------------------- */

/* "0x1a,0x2b,..." (and for ECDSA a leading curve name) into its pieces. */
static int split_commas(const char *text, char **out, int max)
{
    int n = 0;
    const char *p = text;
    while (p && *p && n < max) {
        const char *c = strchr(p, ',');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        out[n] = snewn(len + 1, char);
        memcpy(out[n], p, len);
        out[n][len] = '\0';
        n++;
        p = c ? c + 1 : NULL;
    }
    return n;
}

static mp_int *hex_token(const char *tok)
{
    if (strlen(tok) > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))
        return mp_from_hex(tok + 2);
    return NULL;
}

/* Fixed-width big-endian bytes of x (a Weierstrass coordinate). */
static void put_mp_be_fixed(strbuf *sb, mp_int *x, size_t width)
{
    for (size_t i = 0; i < width; i++)
        put_byte(sb, mp_get_byte(x, width - 1 - i));
}

strbuf *kitty_hostkey_blob_from_text(const char *keytype, const char *text)
{
    char *tok[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    int n, i;
    strbuf *blob = NULL;

    if (!keytype || !text || !*text)
        return NULL;
    n = split_commas(text, tok, lenof(tok));

    if (!strcmp(keytype, "rsa2") && n == 2) {
        /* rsastr_fmt: exponent, modulus */
        mp_int *e = hex_token(tok[0]), *m = hex_token(tok[1]);
        if (e && m) {
            blob = strbuf_new();
            put_stringz(blob, "ssh-rsa");
            put_mp_ssh2(blob, e);
            put_mp_ssh2(blob, m);
        }
        if (e) mp_free(e);
        if (m) mp_free(m);
    } else if (!strcmp(keytype, "dss") && n == 4) {
        mp_int *v[4];
        bool ok = true;
        for (i = 0; i < 4; i++) { v[i] = hex_token(tok[i]); ok = ok && v[i]; }
        if (ok) {
            blob = strbuf_new();
            put_stringz(blob, "ssh-dss");
            for (i = 0; i < 4; i++) put_mp_ssh2(blob, v[i]);
        }
        for (i = 0; i < 4; i++) if (v[i]) mp_free(v[i]);
    } else if (!strncmp(keytype, "ecdsa-sha2-nistp", 16) && n == 3) {
        /* ecc_cache_str_shared: curve name, x, y. The point goes on the wire
         * uncompressed: 0x04, X, Y, each the curve's field width. */
        size_t width = !strcmp(tok[0], "nistp256") ? 32 :
                       !strcmp(tok[0], "nistp384") ? 48 :
                       !strcmp(tok[0], "nistp521") ? 66 : 0;
        mp_int *x = hex_token(tok[1]), *y = hex_token(tok[2]);
        if (width && x && y && !strcmp(keytype + 11, tok[0])) {
            strbuf *pt = strbuf_new();
            put_byte(pt, 0x04);
            put_mp_be_fixed(pt, x, width);
            put_mp_be_fixed(pt, y, width);
            blob = strbuf_new();
            put_stringz(blob, keytype);
            put_stringz(blob, tok[0]);
            put_stringpl(blob, ptrlen_from_strbuf(pt));
            strbuf_free(pt);
        }
        if (x) mp_free(x);
        if (y) mp_free(y);
    } else if ((!strcmp(keytype, "ssh-ed25519") || !strcmp(keytype, "ssh-ed448")) && n == 2) {
        /* Edwards: y little-endian in the curve's byte width, the top bit of
         * the last byte carrying the parity of x. (No curve name in the text:
         * the Edwards curves have name == NULL.) */
        size_t width = !strcmp(keytype, "ssh-ed25519") ? 32 : 57;
        mp_int *x = hex_token(tok[0]), *y = hex_token(tok[1]);
        if (x && y) {
            strbuf *pt = strbuf_new();
            for (size_t b = 0; b < width; b++) {
                unsigned char c = mp_get_byte(y, b);
                if (b == width - 1)
                    c = (unsigned char)((c & 0x7f) | ((mp_get_byte(x, 0) & 1) << 7));
                put_byte(pt, c);
            }
            blob = strbuf_new();
            put_stringz(blob, keytype);
            put_stringpl(blob, ptrlen_from_strbuf(pt));
            strbuf_free(pt);
        }
        if (x) mp_free(x);
        if (y) mp_free(y);
    }

    for (i = 0; i < n; i++) sfree(tok[i]);
    return blob;
}

/* ---- describing one entry -------------------------------------------------- */

/* The last word of a PuTTY fingerprint string, in a fresh buffer; the
 * input is freed. "ssh-rsa 3072 SHA256:abc" -> "SHA256:abc". */
static char *hash_only(char *fp)
{
    const char *sp = strrchr(fp, ' ');
    char *out = dupstr(sp ? sp + 1 : fp);
    sfree(fp);
    return out;
}

static void describe_into(struct kitty_hostkey_entry *e, const char *text)
{
    const ssh_keyalg *alg = alg_for_cache_id(e->keytype);
    strbuf *blob = kitty_hostkey_blob_from_text(e->keytype, text);

    e->type_display = dupstr(alg ? alg->ssh_id : e->keytype);
    e->bits = 0;
    e->sha256 = dupstr("");
    e->md5 = dupstr("");
    if (blob) {
        ptrlen pl = ptrlen_from_strbuf(blob);
        char *fp;
        if (alg) {
            int bits = ssh_key_public_bits(alg, pl);
            /* Edwards keys: PuTTY counts the curve's field bits (255,
             * 448); ssh-keygen counts the encoded point (256, 456). The
             * columns are compared against ssh-keygen, so follow it. */
            if (alg == &ssh_ecdsa_ed25519) bits = 256;
            else if (alg == &ssh_ecdsa_ed448) bits = 456;
            e->bits = bits > 0 ? bits : 0;
        }
        /* PuTTY's fingerprint reads "ssh-rsa 3072 SHA256:...": keep only
         * the hash, the type and bits have their own columns. */
        fp = ssh2_fingerprint_blob(pl, SSH_FPTYPE_SHA256);
        if (fp) { sfree(e->sha256); e->sha256 = hash_only(fp); }
        fp = ssh2_fingerprint_blob(pl, SSH_FPTYPE_MD5);
        if (fp) { sfree(e->md5); e->md5 = hash_only(fp); }
        strbuf_free(blob);
    }
}

const char *const *kitty_hostkey_scan_types(int *n)
{
    static const char *const types[] = {
        "ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
        "ecdsa-sha2-nistp521", "rsa2", "ssh-ed448", "dss",
    };
    *n = (int)lenof(types);
    return types;
}

void kitty_hostkey_describe_text(const char *keytype, const char *text,
                                 struct kitty_hostkey_entry *e)
{
    e->keytype = dupstr(keytype);
    describe_into(e, text);
}

char *kitty_hostkey_describe(const struct kitty_hostkey_entry *e)
{
    return dupprintf("%s:%d  %s  %d  %s  %s", e->host, e->port, e->type_display,
                     e->bits, e->sha256[0] ? e->sha256 : "-",
                     e->md5[0] ? e->md5 : "-");
}

char *kitty_hostkey_now_iso(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return dupprintf("%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth,
                     st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/* ---- enumeration ----------------------------------------------------------- */

/* "<keytype>@<port>:<escaped host>" -> the pieces; false for anything else
 * (an old-style name without '@', or a stamp). */
static bool parse_store_name(const char *name, char **keytype, int *port, char **host)
{
    const char *at = strchr(name, '@'), *colon;
    strbuf *sb;
    if (!at) return false;
    colon = strchr(at + 1, ':');
    if (!colon) return false;
    {
        /* a stamp name ends in :first / :when - the key's own name does not */
        size_t len = strlen(name);
        if ((len > 6 && !strcmp(name + len - 6, KITTY_HOSTKEY_STAMP_FIRST)) ||
            (len > 5 && !strcmp(name + len - 5, KITTY_HOSTKEY_STAMP_WHEN)))
            return false;
    }
    *keytype = snewn((size_t)(at - name) + 1, char);
    memcpy(*keytype, name, at - name);
    (*keytype)[at - name] = '\0';
    *port = atoi(at + 1);
    sb = strbuf_new();
    unescape_registry_key(colon + 1, sb);
    *host = strbuf_to_str(sb);
    return true;
}

static struct kitty_hostkey_entry *list_add(struct kitty_hostkey_list *l)
{
    struct kitty_hostkey_entry *e;
    sgrowarray(l->items, l->alloc, l->n);
    e = &l->items[l->n++];
    memset(e, 0, sizeof(*e));
    return e;
}

struct kitty_hostkey_list *kitty_hostkeys_enumerate(void)
{
    struct kitty_hostkey_list *l = snew(struct kitty_hostkey_list);
    memset(l, 0, sizeof(*l));

    if (store_is_file()) {
        char *dir = portable_item_path("SshHostKeys", "");
        char *pattern;
        WIN32_FIND_DATAA fd;
        HANDLE h;
        if (!dir) return l;
        pattern = dupcat(dir, "*");
        h = FindFirstFileA(pattern, &fd);
        sfree(pattern);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char *keytype, *host, *text, *stamp, *raw;
                int port;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                /* File names are the store names MUNGED (ksf_munge); the
                 * readers munge again, so they take the raw name. */
                raw = ksf_unmunge(fd.cFileName);
                if (!parse_store_name(raw, &keytype, &port, &host)) { sfree(raw); continue; }
                text = portable_read_text_file("SshHostKeys", raw);
                if (text) {
                    struct kitty_hostkey_entry *e = list_add(l);
                    char *n1 = dupcat(raw, KITTY_HOSTKEY_STAMP_FIRST);
                    char *n2 = dupcat(raw, KITTY_HOSTKEY_STAMP_WHEN);
                    e->host = host; e->port = port; e->keytype = keytype;
                    describe_into(e, text);
                    stamp = portable_read_text_file("SshHostKeys", n1);
                    e->first_seen = stamp ? stamp : dupstr("");
                    stamp = portable_read_text_file("SshHostKeys", n2);
                    if (!stamp) {
                        /* No stamp yet: the file's own write time says it. */
                        FILETIME lt; SYSTEMTIME st;
                        FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt);
                        FileTimeToSystemTime(&lt, &st);
                        stamp = dupprintf("%04d-%02d-%02dT%02d:%02d:%02d", st.wYear,
                                          st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                    }
                    e->last_written = stamp;
                    sfree(n1); sfree(n2);
                    sfree(text);
                } else {
                    sfree(keytype); sfree(host);
                }
                sfree(raw);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        sfree(dir);
        return l;
    }

    {
        HKEY key = open_regkey_ro(HKEY_CURRENT_USER, kitty_reg_hostkeys());
        char name[16384];
        DWORD idx = 0, len;
        if (!key) return l;
        while (1) {
            char *keytype, *host, *text;
            int port;
            len = lenof(name);
            if (RegEnumValueA(key, idx, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            idx++;
            if (!parse_store_name(name, &keytype, &port, &host)) continue;
            text = get_reg_sz(key, name);
            if (text) {
                struct kitty_hostkey_entry *e = list_add(l);
                char *n1 = dupcat(name, KITTY_HOSTKEY_STAMP_FIRST);
                char *n2 = dupcat(name, KITTY_HOSTKEY_STAMP_WHEN);
                char *stamp;
                e->host = host; e->port = port; e->keytype = keytype;
                describe_into(e, text);
                stamp = get_reg_sz(key, n1);
                e->first_seen = stamp ? stamp : dupstr("");
                stamp = get_reg_sz(key, n2);
                e->last_written = stamp ? stamp : dupstr("");
                sfree(n1); sfree(n2);
                sfree(text);
            } else {
                sfree(keytype); sfree(host);
            }
        }
        close_regkey(key);
    }
    return l;
}

void kitty_hostkeys_free(struct kitty_hostkey_list *l)
{
    if (!l) return;
    for (int i = 0; i < l->n; i++) {
        struct kitty_hostkey_entry *e = &l->items[i];
        sfree(e->host); sfree(e->keytype); sfree(e->type_display);
        sfree(e->sha256); sfree(e->md5); sfree(e->first_seen); sfree(e->last_written);
    }
    sfree(l->items);
    sfree(l);
}

/* ---- deleting -------------------------------------------------------------- */

bool kitty_hostkey_delete(const char *host, int port, const char *keytype)
{
    strbuf *name = strbuf_new();
    bool done = false;
    put_fmt(name, "%s@%d:", keytype, port);
    escape_registry_key(host, name);
    kitty_store_mark_dirty();

    if (store_is_file()) {
        const char *suffix[] = { "", KITTY_HOSTKEY_STAMP_FIRST, KITTY_HOSTKEY_STAMP_WHEN };
        for (size_t i = 0; i < lenof(suffix); i++) {
            char *n = dupcat(name->s, suffix[i]);
            char *path = portable_item_path("SshHostKeys", n);
            if (path) {
                if (DeleteFileA(path) && i == 0) done = true;
                sfree(path);
            }
            sfree(n);
        }
    } else {
        HKEY key = open_regkey_rw(HKEY_CURRENT_USER, kitty_reg_hostkeys());
        if (key) {
            char *n1 = dupcat(name->s, KITTY_HOSTKEY_STAMP_FIRST);
            char *n2 = dupcat(name->s, KITTY_HOSTKEY_STAMP_WHEN);
            done = (RegDeleteValueA(key, name->s) == ERROR_SUCCESS);
            RegDeleteValueA(key, n1);
            RegDeleteValueA(key, n2);
            sfree(n1); sfree(n2);
            close_regkey(key);
        }
    }
    strbuf_free(name);
    return done;
}
