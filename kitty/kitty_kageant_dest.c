/*
 * kitty_kageant_dest.c - kageant's text of a session-bind destination
 * (session-bind@openssh.com; the agent core verifies and keeps the
 * bindings, pageant.c). A host key is always shown by its SHA256
 * fingerprint. A host name is added when the key is found in the KiTTY++
 * host-key store in use - the registry, or the folder store of a portable
 * or savemode=dir install, as kitty.ini sets it up - or, while kageant's
 * known_hosts setting is on, in the user's OpenSSH known_hosts (plain
 * entries: a hashed one holds no name; such a name is marked
 * "(known_hosts)", kitty_knownhosts.c reads the file). A client that sent
 * session-bind-host-v00@kapper.net after the binding has its name shown
 * first, marked "(client)". Up to five names, then "+N". The names are for
 * the reader only: nothing is decided by them.
 *
 * With savemode=file the host keys are in the registry only while a
 * kitty.exe runs; otherwise no name is found there.
 *
 * The other way round, for the rule editor of destination-restricted keys:
 * a host name to every host key stored for it - the same host-key store,
 * known_hosts while it is read (plain, hashed and @cert-authority entries)
 * and the CA store (kageant_dest_lookup). The keys found are what a rule
 * holds. The CA store by a CA's name, for the destinations "All servers of
 * a host CA" (kageant_ca_by_name, kageant_ca_list).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "pageant.h"

#include "kitty_kageant_dest.h"
#include "kitty_inifile.h"      /* the store in charge (kitty.ini) */
#include "kitty_inikeys.h"      /* KI_SECTION_KITTY, KI_KICLASSNAME */
#include "kitty_sessionpath.h"  /* ksp_component_unmunge */
#include "kitty_text.h"         /* KT_KA_DEST_*, KT_KARULES_SRC_* */
#include "kitty_agentrestrict.h" /* kar_blob_valid, kar_wildcard */
#include "kitty_hostkey_blob.h"  /* the store's text -> the blob */
#include "kitty_knownhosts.h"    /* OpenSSH's known_hosts, on opt-in */
#include "kitty_pageant.h"       /* kageant_readknownhosts_get */

char *kageant_known_hosts_path(void)
{
    const char *prof;
    if (!kageant_readknownhosts_get())
        return NULL;                    /* the setting is off: never read */
    prof = getenv("USERPROFILE");
    if (!prof || !*prof)
        return NULL;
    return dupprintf("%s\\.ssh\\known_hosts", prof);
}

/* The key behind a host key blob as the stores hold it: a certificate's
 * own key, its blob, the store's type name and the store's text of it. */
struct dest_key {
    strbuf *blob;
    const char *cache_id;      /* rsa2, ssh-ed25519, ecdsa-sha2-nistp256 ... */
    char *cache;
};

static bool dest_key_init(struct dest_key *k, ptrlen hostkey)
{
    BinarySource src[1];
    const ssh_keyalg *alg = NULL;
    ssh_key *key = NULL, *base;

    memset(k, 0, sizeof(*k));
    BinarySource_BARE_INIT_PL(src, hostkey);
    ptrlen type = get_string(src);
    if (!get_err(src))
        alg = find_pubkey_alg_len(type);
    if (!alg || (key = ssh_key_new_pub(alg, hostkey)) == NULL)
        return false;
    base = ssh_key_base_key(key);      /* the key itself, or a certificate's */
    k->blob = strbuf_new();
    ssh_key_public_blob(base, BinarySink_UPCAST(k->blob));
    k->cache_id = ssh_key_alg(base)->cache_id;
    k->cache = ssh_key_cache_str(base);
    ssh_key_free(key);
    return true;
}

static void dest_key_free(struct dest_key *k)
{
    if (k->blob)
        strbuf_free(k->blob);
    sfree(k->cache);
}

/* The names found for one key, each once (case ignored), in the order
 * found: the store first, then known_hosts (kh[i] set). */
struct dest_names {
    char **v;
    bool *kh;
    size_t n, size, khsize;
};

static void names_add_from(struct dest_names *nm, const char *name, bool kh)
{
    for (size_t i = 0; i < nm->n; i++)
        if (!stricmp(nm->v[i], name))
            return;
    sgrowarray(nm->v, nm->size, nm->n);
    sgrowarray(nm->kh, nm->khsize, nm->n);
    nm->v[nm->n] = dupstr(name);
    nm->kh[nm->n] = kh;
    nm->n++;
}

static void names_add(struct dest_names *nm, const char *name)
{
    names_add_from(nm, name, false);
}

static void names_free(struct dest_names *nm)
{
    for (size_t i = 0; i < nm->n; i++)
        sfree(nm->v[i]);
    sfree(nm->v);
    sfree(nm->kh);
}

/* A host name with its port when that is not 22. sfree it. */
static char *host_with_port(const char *host, int port)
{
    return (port > 0 && port != 22) ? dupprintf("%s:%d", host, port)
                                    : dupstr(host);
}

/* a name known_hosts holds for the key (kkh_names) */
static void names_add_kh(void *ctx, const char *name)
{
    names_add_from((struct dest_names *)ctx, name, true);
}

/* A store entry's name "<type>@<port>:<escaped host>" of the wanted type ->
 * the host; NULL for another type or a time stamp beside a key
 * (":first", ":when"). sfree it. */
static char *store_name_host(const char *name, const char *type)
{
    size_t tl = strlen(type), len = strlen(name);
    const char *colon;
    strbuf *sb;
    char *host, *out;
    int port;

    if (strncmp(name, type, tl) || name[tl] != '@')
        return NULL;
    if ((len > 6 && !strcmp(name + len - 6, ":first")) ||
        (len > 5 && !strcmp(name + len - 5, ":when")))
        return NULL;
    port = atoi(name + tl + 1);
    colon = strchr(name + tl + 1, ':');
    if (!colon || !colon[1])
        return NULL;
    sb = strbuf_new();
    unescape_registry_key(colon + 1, sb);
    host = strbuf_to_str(sb);
    out = host_with_port(host, port);
    sfree(host);
    return out;
}

/* The store's text of a key against the one wanted, surrounding white
 * space and the case of the hex digits ignored. */
static bool store_text_matches(const char *text, const char *want)
{
    size_t n;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    n = strlen(text);
    while (n > 0 && strchr(" \t\r\n", text[n - 1]))
        n--;
    return n == strlen(want) && !_strnicmp(text, want, n);
}

/* The host-key store in the registry: kitty.exe's hive, which
 * KiClassName=PuTTY in kitty.ini moves to PuTTY's (as kageant's session
 * menu reads it, windows/pageant.c). */
static void names_from_registry(const struct dest_key *k,
                                struct dest_names *nm)
{
    const char *path = "Software\\kapper.net\\KiTTY\\SshHostKeys";
    DWORD nvals = 0, maxname = 0, maxdata = 0;
    char cls[64], *name, *data;
    HKEY key;

    cls[0] = '\0';
    if (kitty_inifile_read(KI_SECTION_KITTY, KI_KICLASSNAME, cls,
                           sizeof(cls)) && !stricmp(cls, "PuTTY"))
        path = "Software\\SimonTatham\\PuTTY\\SshHostKeys";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return;
    if (RegQueryInfoKeyA(key, NULL, NULL, NULL, NULL, NULL, NULL, &nvals,
                         &maxname, &maxdata, NULL, NULL) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return;
    }
    name = snewn(maxname + 1, char);
    data = snewn(maxdata + 1, char);
    for (DWORD idx = 0; idx < nvals; idx++) {
        DWORD nlen = maxname + 1, dlen = maxdata, type = 0;
        char *host;
        if (RegEnumValueA(key, idx, name, &nlen, NULL, &type, (LPBYTE)data,
                          &dlen) != ERROR_SUCCESS || type != REG_SZ)
            continue;
        data[dlen <= maxdata ? dlen : maxdata] = '\0';
        if ((host = store_name_host(name, k->cache_id)) == NULL)
            continue;
        if (store_text_matches(data, k->cache))
            names_add(nm, host);
        sfree(host);
    }
    sfree(name);
    sfree(data);
    RegCloseKey(key);
}

/* A small text file whole, NULL when it cannot be read. sfree it. */
static char *read_small_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long len;
    char *buf = NULL;
    if (!fp)
        return NULL;
    if (!fseek(fp, 0, SEEK_END) && (len = ftell(fp)) >= 0 &&
        len <= 65536 && !fseek(fp, 0, SEEK_SET)) {
        buf = snewn(len + 1, char);
        if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
            sfree(buf);
            buf = NULL;
        } else {
            buf[len] = '\0';
        }
    }
    fclose(fp);
    return buf;
}

/* The folder store's host keys, where kitty.exe keeps them
 * (kitty_storage.c portable_subdir_path): [KiTTY] sshhostkeys=, else
 * SshHostKeys beside the Sessions folder (in it, when the session folder
 * has another name). A file name is the entry's name escaped
 * (ksf_munge), plus [KiTTY] hostkeyextension when that is set. */
static void names_from_folder_store(const struct dest_key *k,
                                    const struct kitty_ini_startup *ini,
                                    struct dest_names *nm)
{
    size_t sl = strlen(ini->hostkey_suffix), dl;
    WIN32_FIND_DATAA fd;
    char *dir, *pattern;
    HANDLE h;

    if (ini->hostkey_dir[0]) {
        dir = dupstr(ini->hostkey_dir);
    } else {
        char *root = dupstr(ini->session_dir), *bs;
        size_t rl = strlen(root);
        while (rl > 0 && root[rl - 1] == '\\')
            root[--rl] = '\0';
        bs = strrchr(root, '\\');
        if (bs && !stricmp(bs + 1, "Sessions"))
            *bs = '\0';
        dir = dupprintf("%s\\SshHostKeys", root);
        sfree(root);
    }
    dl = strlen(dir);
    while (dl > 0 && dir[dl - 1] == '\\')
        dir[--dl] = '\0';
    pattern = dupprintf("%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    sfree(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *fname, *raw, *host;
            size_t fl;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            fname = dupstr(fd.cFileName);
            fl = strlen(fname);
            if (sl && fl > sl && !stricmp(fname + fl - sl, ini->hostkey_suffix))
                fname[fl - sl] = '\0';
            raw = ksp_component_unmunge(fname);
            host = raw ? store_name_host(raw, k->cache_id) : NULL;
            if (host) {
                char *path = dupprintf("%s\\%s", dir, fd.cFileName);
                char *text = read_small_file(path);
                if (text && store_text_matches(text, k->cache))
                    names_add(nm, host);
                sfree(text);
                sfree(path);
                sfree(host);
            }
            sfree(raw);
            sfree(fname);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    sfree(dir);
}

/* known_hosts, while kageant reads it (kitty_knownhosts.c) */
static void names_from_known_hosts(const struct dest_key *k,
                                   struct dest_names *nm)
{
    kkh_names(ptrlen_from_strbuf(k->blob), names_add_kh, nm);
}

/* "SHA256:<hash>" of a host key (a certificate's: of its key). sfree it. */
static char *dest_fp(ptrlen hostkey)
{
    char *full = ssh2_fingerprint_blob(hostkey, SSH_FPTYPE_SHA256);
    const char *sp = strrchr(full, ' ');
    char *out = dupstr(sp ? sp + 1 : full);
    sfree(full);
    return out;
}

/* The names of a host key: the client's (client, may be NULL) first, then
 * the ones found, five in all, "+N" for the others (kar_dest_names_text);
 * NULL when there is none. sfree it. */
static char *dest_name(ptrlen hostkey, const char *client)
{
    struct dest_names nm;
    struct kitty_ini_startup ini;
    struct dest_key k;
    char *out = NULL;

    if (!dest_key_init(&k, hostkey))
        return client ? kar_dest_names_text(client, NULL, 0) : NULL;
    memset(&nm, 0, sizeof(nm));
    memset(&ini, 0, sizeof(ini));
    kitty_inifile_startup(&ini);
    if (ini.folder_store)
        names_from_folder_store(&k, &ini, &nm);
    else
        names_from_registry(&k, &nm);
    kitty_inifile_startup_free(&ini);
    names_from_known_hosts(&k, &nm);
    out = kar_dest_names_text_kh(client, nm.v, nm.kh, nm.n);
    names_free(&nm);
    dest_key_free(&k);
    return out;
}

size_t kageant_dest_names(ptrlen hostkey, char ***out, bool **kh)
{
    struct dest_names nm;
    struct kitty_ini_startup ini;
    struct dest_key k;

    *out = NULL;
    *kh = NULL;
    if (!dest_key_init(&k, hostkey))
        return 0;
    memset(&nm, 0, sizeof(nm));
    memset(&ini, 0, sizeof(ini));
    kitty_inifile_startup(&ini);
    if (ini.folder_store)
        names_from_folder_store(&k, &ini, &nm);
    else
        names_from_registry(&k, &nm);
    kitty_inifile_startup_free(&ini);
    names_from_known_hosts(&k, &nm);
    dest_key_free(&k);
    *out = nm.v;
    *kh = nm.kh;
    return nm.n;
}

char *kageant_dest_hostkey_text_for(ptrlen hostkey, const char *client)
{
    char *fp = dest_fp(hostkey), *name = dest_name(hostkey, client), *out;
    out = name ? dupprintf("%s %s", fp, name) : dupstr(fp);
    sfree(fp);
    sfree(name);
    return out;
}

char *kageant_dest_hostkey_text(ptrlen hostkey)
{
    return kageant_dest_hostkey_text_for(hostkey, NULL);
}

void kageant_dest_fields(const struct kageant_dest *d, char **dest,
                         char **fwd)
{
    strbuf *hops = NULL;

    *dest = *fwd = NULL;
    if (!d || !d->n)
        return;
    for (size_t i = 0; i < d->n; i++) {
        if (d->forwarding[i]) {
            char *t = kageant_dest_hostkey_text_for(d->hostkey[i],
                                                    d->host[i]);
            if (!hops)
                hops = strbuf_new();
            else
                put_datapl(hops, PTRLEN_LITERAL(", "));
            put_dataz(hops, t);
            sfree(t);
        } else if (i == d->n - 1) {
            /* the core keeps no binding after one for a login */
            *dest = kageant_dest_hostkey_text_for(d->hostkey[i], d->host[i]);
        }
    }
    if (hops)
        *fwd = strbuf_to_str(hops);
}

/* One host key in the box: "web01 (client) web-01 (SHA256:...)", the
 * fingerprint alone when there is no name. sfree it. */
static char *box_host(ptrlen hostkey, const char *client)
{
    char *fp = dest_fp(hostkey), *name = dest_name(hostkey, client), *out;
    out = name ? dupprintf("%s (%s)", name, fp) : dupstr(fp);
    sfree(fp);
    sfree(name);
    return out;
}

/* The no-key notice: the server's name alone when one is known (the
 * fingerprint does not fit the notice's width; the agent log keeps it),
 * else the host-key fingerprint */
char *kageant_dest_server_text(const struct kageant_dest *d)
{
    char *name;
    if (!d || !d->n)
        return NULL;
    name = dest_name(d->hostkey[d->n - 1], d->host[d->n - 1]);
    return name ? name : dest_fp(d->hostkey[d->n - 1]);
}

char *kageant_dest_box_text(const struct kageant_dest *d)
{
    strbuf *sb;
    char *h;

    if (!d || !d->n)
        return NULL;
    sb = strbuf_new();
    if (!d->forwarding[d->n - 1]) {
        h = box_host(d->hostkey[d->n - 1], d->host[d->n - 1]);
        put_fmt(sb, KT_KA_DEST_LINE_FMT, h);
        sfree(h);
    }
    for (size_t i = 0; i < d->n; i++) {
        if (!d->forwarding[i])
            continue;
        if (sb->len)
            put_byte(sb, '\n');
        h = box_host(d->hostkey[i], d->host[i]);
        put_fmt(sb, KT_KA_DEST_VIA_FMT, h);
        sfree(h);
    }
    return strbuf_to_str(sb);
}

/* ---- a host name -> its host keys (the rule editor) ----------------------- */

static void found_add(struct kageant_dest_found **v, size_t *n,
                      size_t *size, ptrlen blob, bool is_ca,
                      const char *source)
{
    for (size_t i = 0; i < *n; i++)
        if ((*v)[i].is_ca == is_ca &&
            ptrlen_eq_ptrlen(ptrlen_from_strbuf((*v)[i].blob), blob))
            return;
    if (!kar_blob_valid(blob))
        return;
    sgrowarray(*v, *size, *n);
    (*v)[*n].blob = strbuf_dup(blob);
    (*v)[*n].is_ca = is_ca;
    (*v)[*n].source = dupstr(source);
    (*n)++;
}

struct lookup {
    const char *host;
    int port;
    bool pattern;           /* host is a pattern: every stored name it
                             * matches (kageant_dest_lookup_pattern) */
    struct kageant_dest_found *v;
    size_t n, size;
};

/* A stored host name against the lookup's pattern, case ignored. */
static bool lookup_pattern_takes(const struct lookup *lk, const char *name)
{
    char *n = dupstr(name), *p = dupstr(lk->host);
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

/* A store entry's name "<type>[#n]@<port>:<escaped host>" of this host and
 * port -> its type (sfree it); NULL for another host or a time stamp. */
static char *store_name_type(const char *name, const struct lookup *lk)
{
    const char *at = strchr(name, '@'), *colon, *hash;
    size_t len = strlen(name), tl;
    strbuf *sb;
    char *type = NULL;

    if (!at || (colon = strchr(at + 1, ':')) == NULL)
        return NULL;
    if ((len > 6 && !strcmp(name + len - 6, ":first")) ||
        (len > 5 && !strcmp(name + len - 5, ":when")))
        return NULL;
    if (atoi(at + 1) != lk->port)
        return NULL;
    sb = strbuf_new();
    unescape_registry_key(colon + 1, sb);
    if (lk->pattern ? lookup_pattern_takes(lk, sb->s)
                    : !stricmp(sb->s, lk->host)) {
        hash = memchr(name, '#', (size_t)(at - name));
        tl = (size_t)((hash ? hash : at) - name);
        type = snewn(tl + 1, char);
        memcpy(type, name, tl);
        type[tl] = '\0';
    }
    strbuf_free(sb);
    return type;
}

static void store_entry(struct lookup *lk, const char *name, const char *text)
{
    char *type = store_name_type(name, lk);
    strbuf *blob;
    if (!type)
        return;
    while (*text == ' ' || *text == '\t')
        text++;
    blob = kitty_hostkey_blob_from_text(type, text);
    if (blob) {
        found_add(&lk->v, &lk->n, &lk->size, ptrlen_from_strbuf(blob), false,
                  KT_KARULES_SRC_STORE);
        strbuf_free(blob);
    }
    sfree(type);
}

/* The registry's SshHostKeys or SshHostCAs of the store in charge: kitty.exe's
 * hive, or PuTTY's with KiClassName=PuTTY in kitty.ini. */
static const char *reg_base(void)
{
    char cls[64];
    cls[0] = '\0';
    if (kitty_inifile_read(KI_SECTION_KITTY, KI_KICLASSNAME, cls,
                           sizeof(cls)) && !stricmp(cls, "PuTTY"))
        return "Software\\SimonTatham\\PuTTY";
    return "Software\\kapper.net\\KiTTY";
}

static void lookup_registry_keys(struct lookup *lk)
{
    char *path = dupprintf("%s\\SshHostKeys", reg_base());
    DWORD nvals = 0, maxname = 0, maxdata = 0;
    char *name, *data;
    HKEY key;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        sfree(path);
        return;
    }
    sfree(path);
    if (RegQueryInfoKeyA(key, NULL, NULL, NULL, NULL, NULL, NULL, &nvals,
                         &maxname, &maxdata, NULL, NULL) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return;
    }
    name = snewn(maxname + 1, char);
    data = snewn(maxdata + 1, char);
    for (DWORD idx = 0; idx < nvals; idx++) {
        DWORD nlen = maxname + 1, dlen = maxdata, type = 0;
        if (RegEnumValueA(key, idx, name, &nlen, NULL, &type, (LPBYTE)data,
                          &dlen) != ERROR_SUCCESS || type != REG_SZ)
            continue;
        data[dlen <= maxdata ? dlen : maxdata] = '\0';
        store_entry(lk, name, data);
    }
    sfree(name);
    sfree(data);
    RegCloseKey(key);
}

/* The folder store's root: the folder above Sessions (kitty_storage.c
 * portable_root_dir), as names_from_folder_store finds it. sfree it. */
static char *folder_root(const struct kitty_ini_startup *ini)
{
    char *root = dupstr(ini->session_dir), *bs;
    size_t rl = strlen(root);
    while (rl > 0 && root[rl - 1] == '\\')
        root[--rl] = '\0';
    bs = strrchr(root, '\\');
    if (bs && !stricmp(bs + 1, "Sessions"))
        *bs = '\0';
    return root;
}

static void lookup_folder_keys(struct lookup *lk,
                               const struct kitty_ini_startup *ini)
{
    size_t sl = strlen(ini->hostkey_suffix), dl;
    WIN32_FIND_DATAA fd;
    char *dir, *pattern;
    HANDLE h;

    if (ini->hostkey_dir[0]) {
        dir = dupstr(ini->hostkey_dir);
    } else {
        char *root = folder_root(ini);
        dir = dupprintf("%s\\SshHostKeys", root);
        sfree(root);
    }
    dl = strlen(dir);
    while (dl > 0 && dir[dl - 1] == '\\')
        dir[--dl] = '\0';
    pattern = dupprintf("%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    sfree(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *fname, *raw;
            size_t fl;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            fname = dupstr(fd.cFileName);
            fl = strlen(fname);
            if (sl && fl > sl && !stricmp(fname + fl - sl, ini->hostkey_suffix))
                fname[fl - sl] = '\0';
            raw = ksp_component_unmunge(fname);
            if (raw) {
                char *path = dupprintf("%s\\%s", dir, fd.cFileName);
                char *text = read_small_file(path);
                if (text) {
                    size_t tl = strlen(text);
                    while (tl > 0 && strchr(" \t\r\n", text[tl - 1]))
                        text[--tl] = '\0';
                    store_entry(lk, raw, text);
                }
                sfree(text);
                sfree(path);
            }
            sfree(raw);
            sfree(fname);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    sfree(dir);
}

/* a key known_hosts holds for the lookup (kkh_lookup) */
static void lookup_kh_add(void *ctx, ptrlen blob, bool is_ca)
{
    struct lookup *lk = (struct lookup *)ctx;
    found_add(&lk->v, &lk->n, &lk->size, blob, is_ca,
              is_ca ? KT_KARULES_SRC_KNOWN_HOSTS_CA : KT_KARULES_SRC_KNOWN_HOSTS);
}

/* known_hosts, while kageant reads it: plain, hashed and @cert-authority
 * entries; a pattern takes plain entries only */
static void lookup_known_hosts(struct lookup *lk)
{
    kkh_lookup(lk->host, lk->port, lk->pattern, lookup_kh_add, lk);
}

/* One CA of the CA store as the readers below hand it on: its name, its
 * public key (base64), its host expression and whether its record accepts
 * certificates without names (PermitNameless). */
typedef void (*ca_fn)(void *ctx, const char *name, const char *pubkey,
                      const char *expr, bool nameless);

/* The host expression of a record: Validity, percent-encoded, or an old
 * MatchHosts list of wildcards. NULL for none. sfree it. */
static char *ca_expr(const char *validity, const char *const *matchhosts)
{
    char *expr = NULL;
    if (validity && *validity) {
        expr = strbuf_to_str(percent_decode_sb(ptrlen_from_asciz(validity)));
    } else if (matchhosts) {
        CertExprBuilder *eb = cert_expr_builder_new();
        for (size_t i = 0; matchhosts[i]; i++)
            cert_expr_builder_add(eb, matchhosts[i]);
        expr = cert_expr_expression(eb);
        cert_expr_builder_free(eb);
    }
    return expr;
}

/* The lookup: a CA whose expression takes the host. */
static void ca_entry(void *ctx, const char *name, const char *pubkey,
                     const char *expr, bool nameless)
{
    struct lookup *lk = (struct lookup *)ctx;
    char *src;
    strbuf *blob;

    (void)nameless;
    if (!pubkey || !base64_valid(ptrlen_from_asciz(pubkey)) || !expr ||
        !cert_expr_match_str(expr, lk->host, (unsigned)lk->port))
        return;
    blob = base64_decode_sb(ptrlen_from_asciz(pubkey));
    src = dupprintf(KT_KARULES_SRC_CA_FMT, name);
    found_add(&lk->v, &lk->n, &lk->size, ptrlen_from_strbuf(blob), true, src);
    sfree(src);
    strbuf_free(blob);
}

/* A REG_SZ value whole, NULL when absent. sfree it. */
static char *reg_sz_dup(HKEY key, const char *value)
{
    DWORD type = 0, size = 0;
    char *buf;
    if (RegQueryValueExA(key, value, NULL, &type, NULL, &size) !=
        ERROR_SUCCESS || type != REG_SZ)
        return NULL;
    buf = snewn(size + 1, char);
    if (RegQueryValueExA(key, value, NULL, &type, (LPBYTE)buf, &size) !=
        ERROR_SUCCESS) {
        sfree(buf);
        return NULL;
    }
    buf[size] = '\0';
    return buf;
}

static void ca_enum_registry(ca_fn fn, void *ctx)
{
    char *path = dupprintf("%s\\SshHostCAs", reg_base());
    DWORD nsub = 0, maxsub = 0;
    char *sub;
    HKEY key;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0,
                      KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        sfree(path);
        return;
    }
    sfree(path);
    if (RegQueryInfoKeyA(key, NULL, NULL, NULL, &nsub, &maxsub, NULL, NULL,
                         NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return;
    }
    sub = snewn(maxsub + 1, char);
    for (DWORD idx = 0; idx < nsub; idx++) {
        DWORD slen = maxsub + 1, type = 0, size = 0;
        HKEY ca;
        char *pubkey, *validity, *multi = NULL, *expr;
        const char **hosts = NULL;
        strbuf *name;
        DWORD nl = 0, nlsize = sizeof(nl), nltype = 0;
        bool nameless;
        if (RegEnumKeyExA(key, idx, sub, &slen, NULL, NULL, NULL, NULL) !=
            ERROR_SUCCESS)
            continue;
        if (RegOpenKeyExA(key, sub, 0, KEY_QUERY_VALUE, &ca) != ERROR_SUCCESS)
            continue;
        pubkey = reg_sz_dup(ca, "PublicKey");
        validity = reg_sz_dup(ca, "Validity");
        if (!validity &&
            RegQueryValueExA(ca, "MatchHosts", NULL, &type, NULL, &size) ==
            ERROR_SUCCESS && type == REG_MULTI_SZ) {
            size_t nh = 0;
            multi = snewn(size + 2, char);
            if (RegQueryValueExA(ca, "MatchHosts", NULL, &type,
                                 (LPBYTE)multi, &size) == ERROR_SUCCESS) {
                multi[size] = multi[size + 1] = '\0';
                for (char *q = multi; *q; q += strlen(q) + 1)
                    nh++;
                hosts = snewn(nh + 1, const char *);
                nh = 0;
                for (char *q = multi; *q; q += strlen(q) + 1)
                    hosts[nh++] = q;
                hosts[nh] = NULL;
            }
        }
        nameless = RegQueryValueExA(ca, "PermitNameless", NULL, &nltype,
                                    (LPBYTE)&nl, &nlsize) == ERROR_SUCCESS &&
            nltype == REG_DWORD && nl != 0;
        RegCloseKey(ca);
        name = strbuf_new();
        unescape_registry_key(sub, name);
        expr = ca_expr(validity, hosts);
        fn(ctx, name->s, pubkey, expr, nameless);
        sfree(expr);
        strbuf_free(name);
        sfree(pubkey);
        sfree(validity);
        sfree(hosts);
        sfree(multi);
    }
    sfree(sub);
    RegCloseKey(key);
}

/* A folder store file's value: "Key=value" (or the old "Key\value\"),
 * %HH-escaped (ksf_munge). sfree it. */
static char *folder_value(const char *text, const char *want)
{
    const char *p = text;
    size_t wl = strlen(want);
    while (*p) {
        const char *eol = strpbrk(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > wl && !strncmp(p, want, wl) &&
            (p[wl] == '=' || p[wl] == '\\')) {
            char *raw = dupprintf("%.*s", (int)(len - wl - 1), p + wl + 1), *v;
            size_t rl = strlen(raw);
            if (p[wl] == '\\' && rl && raw[rl - 1] == '\\')
                raw[rl - 1] = '\0';
            v = ksp_component_unmunge(raw);
            sfree(raw);
            return v;
        }
        p += len;
        while (*p == '\r' || *p == '\n')
            p++;
    }
    return NULL;
}

static void ca_enum_folder(const struct kitty_ini_startup *ini, ca_fn fn,
                           void *ctx)
{
    char *root = folder_root(ini), *dir, *pattern;
    WIN32_FIND_DATAA fd;
    HANDLE h;

    dir = dupprintf("%s\\SshHostCAs", root);
    sfree(root);
    pattern = dupprintf("%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    sfree(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *path, *text, *name, *pubkey, *validity, *nl, *expr;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            path = dupprintf("%s\\%s", dir, fd.cFileName);
            text = read_small_file(path);
            sfree(path);
            if (!text)
                continue;
            name = ksp_component_unmunge(fd.cFileName);
            pubkey = folder_value(text, "PublicKey");
            validity = folder_value(text, "Validity");
            nl = folder_value(text, "PermitNameless");
            expr = ca_expr(validity, NULL);
            fn(ctx, name ? name : fd.cFileName, pubkey, expr,
               nl && atoi(nl) != 0);
            sfree(expr);
            sfree(nl);
            sfree(name);
            sfree(pubkey);
            sfree(validity);
            sfree(text);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    sfree(dir);
}

size_t kageant_dest_lookup(const char *host, int port,
                           struct kageant_dest_found **out)
{
    struct lookup lk;
    struct kitty_ini_startup ini;

    memset(&lk, 0, sizeof(lk));
    lk.host = host;
    lk.port = port > 0 ? port : 22;
    *out = NULL;
    if (!host || !*host)
        return 0;
    memset(&ini, 0, sizeof(ini));
    kitty_inifile_startup(&ini);
    if (ini.folder_store) {
        lookup_folder_keys(&lk, &ini);
        ca_enum_folder(&ini, ca_entry, &lk);
    } else {
        lookup_registry_keys(&lk);
        ca_enum_registry(ca_entry, &lk);
    }
    kitty_inifile_startup_free(&ini);
    lookup_known_hosts(&lk);
    *out = lk.v;
    return lk.n;
}

size_t kageant_dest_lookup_pattern(const char *pattern, int port,
                                   struct kageant_dest_found **out)
{
    struct lookup lk;
    struct kitty_ini_startup ini;

    memset(&lk, 0, sizeof(lk));
    lk.host = pattern;
    lk.port = port > 0 ? port : 22;
    lk.pattern = true;
    *out = NULL;
    if (!pattern || !*pattern)
        return 0;
    memset(&ini, 0, sizeof(ini));
    kitty_inifile_startup(&ini);
    if (ini.folder_store)
        lookup_folder_keys(&lk, &ini);
    else
        lookup_registry_keys(&lk);
    kitty_inifile_startup_free(&ini);
    lookup_known_hosts(&lk);
    *out = lk.v;
    return lk.n;
}

void kageant_dest_found_free(struct kageant_dest_found *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        strbuf_free(v[i].blob);
        sfree(v[i].source);
    }
    sfree(v);
}

/* ---- the CA store by a CA's name ------------------------------------------ */

/* Every CA of the store in charge. */
static void ca_enum(ca_fn fn, void *ctx)
{
    struct kitty_ini_startup ini;
    memset(&ini, 0, sizeof(ini));
    kitty_inifile_startup(&ini);
    if (ini.folder_store)
        ca_enum_folder(&ini, fn, ctx);
    else
        ca_enum_registry(fn, ctx);
    kitty_inifile_startup_free(&ini);
}

static void ca_list_add(void *ctx, const char *name, const char *pubkey,
                        const char *expr, bool nameless)
{
    struct kageant_ca_list *l = (struct kageant_ca_list *)ctx;
    struct kageant_ca *c;
    strbuf *blob;
    if (!pubkey || !base64_valid(ptrlen_from_asciz(pubkey)))
        return;
    blob = base64_decode_sb(ptrlen_from_asciz(pubkey));
    if (!kar_blob_valid(ptrlen_from_strbuf(blob))) {
        strbuf_free(blob);
        return;
    }
    sgrowarray(l->v, l->size, l->n);
    c = &l->v[l->n++];
    c->name = dupstr(name);
    c->pubkey = blob;
    c->expr = dupstr(expr ? expr : "");
    c->nameless = nameless;
}

void kageant_ca_list(struct kageant_ca_list *out)
{
    memset(out, 0, sizeof(*out));
    ca_enum(ca_list_add, out);
}

void kageant_ca_list_free(struct kageant_ca_list *l)
{
    for (size_t i = 0; i < l->n; i++) {
        sfree(l->v[i].name);
        strbuf_free(l->v[i].pubkey);
        sfree(l->v[i].expr);
    }
    sfree(l->v);
    memset(l, 0, sizeof(*l));
}

bool kageant_ca_by_name(const char *name, strbuf *pubkey, char **expr,
                        bool *nameless)
{
    struct kageant_ca_list l;
    bool found = false;

    *expr = NULL;
    *nameless = false;
    if (!name || !*name)
        return false;
    kageant_ca_list(&l);
    for (size_t i = 0; i < l.n && !found; i++) {
        if (strcmp(l.v[i].name, name))
            continue;
        put_datapl(pubkey, ptrlen_from_strbuf(l.v[i].pubkey));
        *expr = dupstr(l.v[i].expr);
        *nameless = l.v[i].nameless;
        found = true;
    }
    kageant_ca_list_free(&l);
    return found;
}
