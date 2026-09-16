/*
 * kitty_winpos.c - where a terminal window is remembered.
 *
 * "Remember window position" (Window > Appearance > Position) keeps, for
 * each session and each monitor layout, the window's top-left and its
 * terminal size in columns x rows. The layout is an order-independent hash
 * of the monitor rectangles, so a docked and an undocked laptop each keep
 * their own set of positions.
 *
 * Two places hold such an entry:
 *
 *   - the session itself: value TermPos_<layout> beside the session's other
 *     settings (one registry value, or one line of the portable session
 *     file). Deleting the session takes it along.
 *   - the SHARED entry, WindowPos\WinPos_<layout> under the registry base
 *     (or WindowPos\WinPos_<layout> as a text file in a portable store).
 *     Written only by windows that have no session of their own - an
 *     unnamed "type a host and go" window, or one opened as "Default
 *     Settings" - and read once as the fallback for a session that has no
 *     entry of its own yet. "Default Settings" itself is never written on
 *     close; the shared entry is what its window updates instead.
 *
 * Both are the text "left,top,cols,rows". An earlier version wrote the
 * shared entry as a binary RECT in pixels; that is still read, as a
 * position without a size, and is replaced the next time it is written.
 *
 * The configuration window's own position (WindowPos\ConfigBox) is a
 * different thing and is left alone here.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "storage.h"
#include "kitty_defs.h"
#include "kitty_storage.h"
#include "kitty_winpos.h"

#define KWP_SHARED_SUBDIR   "WindowPos"     /* registry subkey and portable folder */
#define KWP_SHARED_PREFIX   "WinPos"
#define KWP_SESSION_PREFIX  "TermPos"

/* Each monitor contributes FNV-1a of its rectangle and the per-monitor
 * hashes are SUMMED: EnumDisplayMonitors does not promise the same order in
 * the saving and the restoring process, and a sum does not care. */
static BOOL CALLBACK kwp_topo_enum(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    unsigned long *acc = (unsigned long *)lp;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(hm, &mi)) {
        unsigned long h = 2166136261UL;
        const unsigned char *p = (const unsigned char *)&mi.rcMonitor;
        size_t i;
        for (i = 0; i < sizeof(mi.rcMonitor); i++) {
            h ^= p[i];
            h *= 16777619UL;
        }
        *acc += h;
    }
    (void)dc; (void)rc;
    return TRUE;
}

unsigned long kitty_winpos_layout_hash(void)
{
    unsigned long h = 0;
    EnumDisplayMonitors(NULL, NULL, kwp_topo_enum, (LPARAM)&h);
    return h;
}

void kitty_winpos_layout_key(char *buf, size_t n, const char *prefix,
                             unsigned long layout)
{
    _snprintf(buf, n, "%s_%08lx", prefix, layout);
    if (n) buf[n - 1] = '\0';
}

void kitty_winpos_format(const struct kitty_termpos *pos, char *buf, size_t n)
{
    _snprintf(buf, n, "%d,%d,%d,%d", pos->left, pos->top, pos->cols, pos->rows);
    if (n) buf[n - 1] = '\0';
}

int kitty_winpos_parse(const char *s, struct kitty_termpos *out)
{
    int l, t, c, r;
    if (!s || !out) return 0;
    if (sscanf(s, "%d,%d,%d,%d", &l, &t, &c, &r) != 4) return 0;
    if (c < 0 || r < 0) return 0;
    out->left = l; out->top = t; out->cols = c; out->rows = r;
    return 1;
}

int kitty_winpos_is_shared_window(const char *sessionname)
{
    return !sessionname || !*sessionname ||
           !strcmp(sessionname, KITTY_DEFAULT_SESSION);
}

/* ---------------------------------------------------------------- session */

int kitty_winpos_session_get(const char *session, unsigned long layout,
                             struct kitty_termpos *out)
{
    char key[64];
    settings_r *r;
    char *v;
    int ok = 0;
    if (kitty_winpos_is_shared_window(session)) return 0;
    kitty_winpos_layout_key(key, sizeof(key), KWP_SESSION_PREFIX, layout);
    r = open_settings_r(session);
    if (!r) return 0;
    v = read_setting_s(r, key);
    close_settings_r(r);
    if (v) {
        ok = kitty_winpos_parse(v, out);
        sfree(v);
    }
    return ok;
}

int kitty_winpos_session_set(const char *session, unsigned long layout,
                             const struct kitty_termpos *pos)
{
    char key[64], val[64];
    settings_r *r;
    settings_w *w;
    char *err = NULL;
    if (kitty_winpos_is_shared_window(session)) return 0;
    /* Only a session that exists IN THE STORE WE WRITE gets an entry. The
     * write handle would otherwise create an empty session holding nothing
     * but a position; worse, a session only loaded from a read-only fallback
     * hive (old 9bis KiTTY, stock PuTTY) would be written into the own hive
     * as a phantom, which then loads by name whether or not the old stores
     * are shown. So the read must come from our own store, not the fallback. */
    r = open_settings_r(session);
    if (!r) return 0;
    if (!kitty_settings_r_is_own(r)) { close_settings_r(r); return 0; }
    close_settings_r(r);
    kitty_winpos_layout_key(key, sizeof(key), KWP_SESSION_PREFIX, layout);
    kitty_winpos_format(pos, val, sizeof(val));
    w = open_settings_w(session, &err);
    if (!w) {
        sfree(err);
        return 0;
    }
    /* The handle pre-loads a portable session file and overwrites a registry
     * key in place, so this touches the one value and nothing else. */
    write_setting_s(w, key, val);
    close_settings_w(w);
    return 1;
}

/* A collected TermPos_* entry of the source session, for the copy. */
struct kwp_pair { char *key; char *val; };
struct kwp_collect { struct kwp_pair *v; int n, cap; };

static void kwp_collect_add(struct kwp_collect *c, const char *key, const char *val)
{
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 8;
        c->v = sresize(c->v, c->cap, struct kwp_pair);
    }
    c->v[c->n].key = dupstr(key);
    c->v[c->n].val = dupstr(val ? val : "");
    c->n++;
}

static void kwp_collect_from_file(const char *key, const char *val, void *ctx)
{
    if (!strncmp(key, KWP_SESSION_PREFIX "_", strlen(KWP_SESSION_PREFIX "_")))
        kwp_collect_add((struct kwp_collect *)ctx, key, val);
}

int kitty_winpos_session_copy(const char *from, const char *to)
{
    struct kwp_collect c;
    settings_r *r;
    settings_w *w;
    char *err = NULL;
    int i, copied = 0;
    const size_t plen = strlen(KWP_SESSION_PREFIX "_");

    if (kitty_winpos_is_shared_window(from) || kitty_winpos_is_shared_window(to))
        return 0;
    if (!strcmp(from, to)) return 0;
    c.v = NULL; c.n = 0; c.cap = 0;

    if (store_is_file()) {
        char *path = ksf_session_path(from);
        struct ksf_item *items = path ? ksf_load(path) : NULL;
        sfree(path);
        if (items) {
            ksf_list_foreach(items, kwp_collect_from_file, &c);
            ksf_list_free(items);
        }
    } else {
        strbuf *sb = strbuf_new();
        char *path;
        HKEY hk;
        escape_registry_key(from, sb);
        path = dupprintf("%s\\%s", kitty_reg_sessions(), sb->s);
        strbuf_free(sb);
        if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &hk)
            == ERROR_SUCCESS) {
            DWORD idx;
            for (idx = 0; ; idx++) {
                char name[128];
                BYTE raw[256];
                DWORD nlen = sizeof(name), type = 0, sz = sizeof(raw) - 1;
                if (RegEnumValueA(hk, idx, name, &nlen, NULL, &type, raw, &sz)
                    != ERROR_SUCCESS)
                    break;
                if (type != REG_SZ) continue;
                if (_strnicmp(name, KWP_SESSION_PREFIX "_", plen)) continue;
                raw[sz] = '\0';
                kwp_collect_add(&c, name, (const char *)raw);
            }
            RegCloseKey(hk);
        }
        sfree(path);
    }

    if (c.n) {
        /* The target exists: this runs right after it was saved. */
        r = open_settings_r(to);
        if (r) {
            close_settings_r(r);
            w = open_settings_w(to, &err);
            if (w) {
                for (i = 0; i < c.n; i++)
                    write_setting_s(w, c.v[i].key, c.v[i].val);
                close_settings_w(w);
                copied = c.n;
            } else {
                sfree(err);
            }
        }
    }
    for (i = 0; i < c.n; i++) {
        sfree(c.v[i].key);
        sfree(c.v[i].val);
    }
    sfree(c.v);
    return copied;
}

/* ----------------------------------------------------------------- shared */

static void kwp_reg_path(char *buf, size_t n)
{
    _snprintf(buf, n, "%s\\%s", kitty_registry_base(), KWP_SHARED_SUBDIR);
    if (n) buf[n - 1] = '\0';
}

int kitty_winpos_shared_get(unsigned long layout, struct kitty_termpos *out)
{
    char key[64];
    kitty_winpos_layout_key(key, sizeof(key), KWP_SHARED_PREFIX, layout);

    if (store_is_file()) {
        char *v = portable_read_text_file(KWP_SHARED_SUBDIR, key);
        int ok = 0;
        if (v) {
            ok = kitty_winpos_parse(v, out);
            sfree(v);
        }
        return ok;
    } else {
        char base[600];
        HKEY hk;
        DWORD type = 0, sz;
        BYTE raw[256];
        int ret = 0;
        kwp_reg_path(base, sizeof(base));
        if (RegOpenKeyExA(HKEY_CURRENT_USER, base, 0, KEY_QUERY_VALUE, &hk)
            != ERROR_SUCCESS)
            return 0;
        sz = sizeof(raw) - 1;
        if (RegQueryValueExA(hk, key, NULL, &type, raw, &sz) == ERROR_SUCCESS) {
            if (type == REG_SZ) {
                raw[sz] = '\0';
                ret = kitty_winpos_parse((const char *)raw, out) ? 1 : 0;
            } else if (type == REG_BINARY && sz == sizeof(RECT)) {
                /* Written by an earlier version: a pixel rectangle. The
                 * position is still good; the size is not carried over. */
                RECT r;
                memcpy(&r, raw, sizeof(r));
                out->left = (int)r.left;
                out->top = (int)r.top;
                out->cols = 0;
                out->rows = 0;
                ret = 2;
            }
        }
        RegCloseKey(hk);
        return ret;
    }
}

int kitty_winpos_shared_set(unsigned long layout, const struct kitty_termpos *pos)
{
    char key[64], val[64];
    kitty_winpos_layout_key(key, sizeof(key), KWP_SHARED_PREFIX, layout);
    kitty_winpos_format(pos, val, sizeof(val));

    if (store_is_file()) {
        return portable_write_text_file(KWP_SHARED_SUBDIR, key, val) ? 1 : 0;
    } else {
        char base[600];
        HKEY hk;
        int ok = 0;
        kwp_reg_path(base, sizeof(base));
        if (RegCreateKeyExA(HKEY_CURRENT_USER, base, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
            ok = RegSetValueExA(hk, key, 0, REG_SZ, (const BYTE *)val,
                                (DWORD)strlen(val) + 1) == ERROR_SUCCESS;
            RegCloseKey(hk);
        }
        return ok;
    }
}

/* Walk the shared entries; with remove set, delete each one. Returns the
 * number seen (or removed). Only names starting with "WinPos_" count, so
 * the configuration window's "ConfigBox" value is never touched. */
static int kwp_shared_walk(int remove)
{
    int count = 0;
    const size_t plen = strlen(KWP_SHARED_PREFIX "_");

    if (store_is_file()) {
        char *dir = portable_subdir_path(KWP_SHARED_SUBDIR);
        char *pattern;
        WIN32_FIND_DATAA fd;
        HANDLE h;
        if (!dir) return 0;
        pattern = dupprintf("%s\\%s_*", dir, KWP_SHARED_PREFIX);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (_strnicmp(fd.cFileName, KWP_SHARED_PREFIX "_", plen)) continue;
                if (remove) {
                    char *p = dupprintf("%s\\%s", dir, fd.cFileName);
                    if (DeleteFileA(p)) count++;
                    sfree(p);
                } else {
                    count++;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        sfree(pattern);
        sfree(dir);
        return count;
    } else {
        char base[600];
        HKEY hk;
        char name[128];
        DWORD i, nlen;
        char **doomed = NULL;
        int ndoomed = 0, cap = 0, k;
        kwp_reg_path(base, sizeof(base));
        if (RegOpenKeyExA(HKEY_CURRENT_USER, base, 0,
                          KEY_QUERY_VALUE | (remove ? KEY_SET_VALUE : 0), &hk)
            != ERROR_SUCCESS)
            return 0;
        /* Collect first, delete after: deleting while enumerating shifts
         * the indices under the enumeration. */
        for (i = 0; ; i++) {
            nlen = sizeof(name);
            if (RegEnumValueA(hk, i, name, &nlen, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
                break;
            if (_strnicmp(name, KWP_SHARED_PREFIX "_", plen)) continue;
            count++;
            if (remove) {
                if (ndoomed == cap) {
                    cap = cap ? cap * 2 : 8;
                    doomed = sresize(doomed, cap, char *);
                }
                doomed[ndoomed++] = dupstr(name);
            }
        }
        if (remove) {
            count = 0;
            for (k = 0; k < ndoomed; k++) {
                if (RegDeleteValueA(hk, doomed[k]) == ERROR_SUCCESS) count++;
                sfree(doomed[k]);
            }
            sfree(doomed);
        }
        RegCloseKey(hk);
        return count;
    }
}

int kitty_winpos_shared_count(void)
{
    return kwp_shared_walk(0);
}

int kitty_winpos_shared_reset(void)
{
    return kwp_shared_walk(1);
}
