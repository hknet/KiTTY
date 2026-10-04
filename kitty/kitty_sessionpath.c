/*
 * kitty_sessionpath.c - a saved session's name as a path (hknet/KiTTY#55)
 * and what counts as a session file (hknet/KiTTY#56). See
 * kitty_sessionpath.h for the model.
 *
 * Compiles into the settings library (and standalone into the targets that
 * list kitty_storage.c, windows/CMakeLists.txt), so it stays free of GUI
 * dependencies: klink, kscp and ksftp resolve a -load name through the same
 * code as the terminal window. It needs no store either (the store-aware
 * names and resolver are in kitty_storage.c): kageant links it without the
 * settings library, for its Saved Sessions menu.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "putty.h"
#include "storage.h"
#include "kitty_storage.h"
#include "kitty_sessionpath.h"
#include "kitty_text.h"

/* Windows' reserved device names. A name is reserved when the part before its
 * first dot - trailing spaces ignored - is one of these, whatever follows:
 * "CON", "con.txt" and "COM1 .log" all open the device, never a file. */
static int ksp_reserved_base(const char *s, size_t n)
{
    static const char *const names[] = { "CON", "PRN", "AUX", "NUL" };
    size_t i;
    while (n > 0 && s[n - 1] == ' ')
        n--;
    if (n == 3) {
        for (i = 0; i < lenof(names); i++)
            if (!strnicmp(s, names[i], 3))
                return 1;
    }
    if (n == 4 && (!strnicmp(s, "COM", 3) || !strnicmp(s, "LPT", 3)) &&
        s[3] >= '1' && s[3] <= '9')
        return 1;
    return 0;
}

static const char ksp_hex[] = "0123456789ABCDEF";

/* The folder store's %xx escape of a name or a value (declared in
 * kitty_storage.h). It lives here, with the component rules built on it, so
 * a binary without the settings library (kageant) links the same escape. */
static int ksf_special(unsigned char c)
{
    return c < 0x20 || c == 0x7f || c == '%' || c == '\\' || c == '/' ||
           c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
           c == '>' || c == '|';
}
char *ksf_munge(const char *in)            /* snewn'd */
{
    char *out = snewn(strlen(in) * 3 + 1, char), *o = out;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (ksf_special(c)) { *o++ = '%'; *o++ = ksp_hex[c >> 4]; *o++ = ksp_hex[c & 15]; }
        else *o++ = (char)c;
    }
    *o = '\0';
    return out;
}
static int ksf_hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
char *ksf_unmunge(const char *in)          /* snewn'd */
{
    char *out = snewn(strlen(in) + 1, char), *o = out;
    while (*in) {
        if (*in == '%' && in[1] && in[2]) {
            int hi = ksf_hexv(in[1]), lo = ksf_hexv(in[2]);
            if (hi >= 0 && lo >= 0) { *o++ = (char)((hi << 4) | lo); in += 3; continue; }
        }
        *o++ = *in++;
    }
    *o = '\0';
    return out;
}

char *ksp_component_munge(const char *component)
{
    char *m = ksf_munge(component ? component : "");
    size_t len = strlen(m), base;
    strbuf *sb;
    char *dot;

    dot = strchr(m, '.');
    base = dot ? (size_t)(dot - m) : len;
    if (len == 0)
        return m;
    /* Nothing to add: the common case, returned as ksf_munge made it. */
    if (m[0] != '.' && m[len - 1] != '.' && m[len - 1] != ' ' &&
        !ksp_reserved_base(m, base))
        return m;

    sb = strbuf_new();
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)m[i];
        bool esc =
            (i == 0 && c == '.') ||                       /* dot-file */
            (i == 0 && ksp_reserved_base(m, base)) ||     /* %43ON, %63om1.txt */
            (i == len - 1 && (c == '.' || c == ' '));     /* Windows drops these */
        if (esc) {
            put_byte(sb, '%');
            put_byte(sb, ksp_hex[c >> 4]);
            put_byte(sb, ksp_hex[c & 15]);
        } else {
            put_byte(sb, c);
        }
    }
    sfree(m);
    return strbuf_to_str(sb);
}

char *ksp_component_unmunge(const char *filename)
{
    return ksf_unmunge(filename ? filename : "");
}

char *ksp_normalise(const char *path)
{
    strbuf *sb = strbuf_new();
    const char *p = path ? path : "";
    bool need_sep = false;
    while (*p) {
        const char *e = strchr(p, '\\');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n) {
            if (need_sep)
                put_byte(sb, '\\');
            put_data(sb, p, n);
            need_sep = true;
        }
        p += n;
        if (*p == '\\')
            p++;
    }
    return strbuf_to_str(sb);
}

char *ksp_folder_of(const char *path)
{
    const char *bs;
    if (!path)
        return NULL;
    bs = strrchr(path, '\\');
    if (!bs || bs == path)
        return NULL;
    return dupprintf("%.*s", (int)(bs - path), path);
}

const char *ksp_leaf(const char *path)
{
    const char *bs;
    if (!path)
        return "";
    bs = strrchr(path, '\\');
    return bs ? bs + 1 : path;
}

int ksp_folder_is_root(const char *f)
{
    return !f || !*f || !strcmp(f, "Default");
}

int ksp_folder_same(const char *a, const char *b)
{
    if (ksp_folder_is_root(a) || ksp_folder_is_root(b))
        return ksp_folder_is_root(a) && ksp_folder_is_root(b);
    return !stricmp(a, b);
}

int ksp_folder_within(const char *folder, const char *top)
{
    size_t n;
    if (ksp_folder_is_root(top))
        return 1;
    if (ksp_folder_is_root(folder))
        return 0;
    n = strlen(top);
    return !strnicmp(folder, top, n) && (folder[n] == '\0' || folder[n] == '\\');
}

char *ksp_folder_parent(const char *f)
{
    char *p = ksp_folder_of(f);
    return p ? p : dupstr("Default");
}

char *ksp_folder_child_row(const char *level, const char *folder)
{
    const char *rest, *bs;
    if (ksp_folder_is_root(folder))
        return NULL;
    if (ksp_folder_is_root(level)) {
        bs = strchr(folder, '\\');
        return bs ? dupprintf("%.*s", (int)(bs - folder), folder) : dupstr(folder);
    }
    if (!ksp_folder_within(folder, level) || !stricmp(folder, level))
        return NULL;
    rest = folder + strlen(level) + 1;
    bs = strchr(rest, '\\');
    return bs ? dupprintf("%.*s", (int)(bs - folder), folder) : dupstr(folder);
}

char *ksp_folder_moved_path(const char *path, const char *from, const char *to)
{
    const char *rest = path + (ksp_folder_is_root(from) ? 0 : strlen(from));
    while (*rest == '\\')
        rest++;
    if (ksp_folder_is_root(to))
        return dupstr(*rest ? rest : "Default");
    return *rest ? dupprintf("%s\\%s", to, rest) : dupstr(to);
}

/* ---- the order a list shows sessions in ----
 * By the name a row SHOWS. Sorting by the stored identity put a session stored
 * by its bare name (Folder=VSS) among sessions stored as "VSS\..." paths by
 * comparing two different strings: "monitor" against "VSS\k7". */
int ksp_natcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int da = (*a >= '0' && *a <= '9'), db = (*b >= '0' && *b <= '9');
        int ca, cb;
        if (da && db) {
            const char *sa, *sb;
            size_t la, lb;
            int c;
            while (*a == '0') a++;     /* leading zeros carry no value */
            while (*b == '0') b++;
            sa = a; sb = b;
            while (*a >= '0' && *a <= '9') a++;
            while (*b >= '0' && *b <= '9') b++;
            la = a - sa; lb = b - sb;
            if (la != lb)              /* more digits = larger number */
                return la < lb ? -1 : +1;
            c = strncmp(sa, sb, la);
            if (c)
                return c < 0 ? -1 : +1;
            continue;
        }
        /* A folder separator ends a component, so it sorts below every
         * character: "net\x" stays next to "net", ahead of "net-2". */
        ca = (*a == '\\') ? 1 : tolower((unsigned char)*a);
        cb = (*b == '\\') ? 1 : tolower((unsigned char)*b);
        if (ca != cb)
            return ca < cb ? -1 : +1;
        a++; b++;
    }
    if (*a) return +1;
    if (*b) return -1;
    return 0;
}

static const char *ksp_shown_folder(const struct ksp_shown_row *r)
{
    return ksp_folder_is_root(r->folder) ? "" : r->folder;
}

/* Default Settings first; then the shown name, then the folder, then the
 * identity itself, so the order is total and the same on every refresh. */
static int ksp_shown_cmp_rows(const struct ksp_shown_row *a,
                              const struct ksp_shown_row *b, int folder_first)
{
    int c, da = !strcmp(a->id, "Default Settings"),
        db = !strcmp(b->id, "Default Settings");
    if (da || db)
        return db - da;
    if (folder_first &&
        (c = ksp_natcasecmp(ksp_shown_folder(a), ksp_shown_folder(b))) != 0)
        return c;
    if ((c = ksp_natcasecmp(ksp_leaf(a->id), ksp_leaf(b->id))) != 0)
        return c;
    if ((c = ksp_natcasecmp(ksp_shown_folder(a), ksp_shown_folder(b))) != 0)
        return c;
    return strcmp(a->id, b->id);
}
static int ksp_shown_qcmp(const void *a, const void *b)
{
    return ksp_shown_cmp_rows(a, b, 0);
}
static int ksp_shown_qcmp_folder(const void *a, const void *b)
{
    return ksp_shown_cmp_rows(a, b, 1);
}
void ksp_sort_shown(struct ksp_shown_row *rows, int n)
{
    if (n > 1)
        qsort(rows, n, sizeof(*rows), ksp_shown_qcmp);
}
void ksp_sort_shown_by_folder(struct ksp_shown_row *rows, int n)
{
    if (n > 1)
        qsort(rows, n, sizeof(*rows), ksp_shown_qcmp_folder);
}

char *ksp_path_to_relfile(const char *path, const char *suffix)
{
    char *norm = ksp_normalise(path);
    strbuf *sb = strbuf_new();
    const char *p = norm;
    while (*p) {
        const char *e = strchr(p, '\\');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char *comp = dupprintf("%.*s", (int)n, p);
        char *m = ksp_component_munge(comp);
        if (sb->len)
            put_byte(sb, '\\');
        put_dataz(sb, m);
        sfree(m);
        sfree(comp);
        p += n;
        if (*p == '\\')
            p++;
    }
    if (sb->len && suffix && *suffix)
        put_dataz(sb, suffix);
    sfree(norm);
    return strbuf_to_str(sb);
}

struct ksp_relname {
    char *rel;
    const char *name;
};

static int ksp_relname_cmp(const void *a, const void *b)
{
    return stricmp(((const struct ksp_relname *)a)->rel,
                   ((const struct ksp_relname *)b)->rel);
}

char *ksp_layout_blocker(const char *const *names, int n, const char *suffix)
{
    struct ksp_relname *v = snewn(n > 0 ? n : 1, struct ksp_relname);
    char *found = NULL;
    int i;
    for (i = 0; i < n; i++) {
        v[i].rel = ksp_path_to_relfile(names[i], suffix);
        v[i].name = names[i];
    }
    qsort(v, n, sizeof(*v), ksp_relname_cmp);
    /* Each folder above a session file, looked up among the session files:
     * file names compare without case, as on the disk. */
    for (i = 0; i < n && !found; i++) {
        const char *p;
        for (p = strchr(v[i].rel, '\\'); p && !found; p = strchr(p + 1, '\\')) {
            struct ksp_relname key, *hit;
            key.rel = dupprintf("%.*s", (int)(p - v[i].rel), v[i].rel);
            hit = bsearch(&key, v, n, sizeof(*v), ksp_relname_cmp);
            if (hit)
                found = dupstr(hit->name);
            sfree(key.rel);
        }
    }
    for (i = 0; i < n; i++)
        sfree(v[i].rel);
    sfree(v);
    return found;
}

int ksp_strip_suffix(char *filename, const char *suffix)
{
    size_t fl, sl;
    if (!filename || !suffix || !*suffix)
        return 0;
    fl = strlen(filename);
    sl = strlen(suffix);
    if (fl <= sl || stricmp(filename + fl - sl, suffix))
        return 0;
    filename[fl - sl] = '\0';
    return 1;
}

/* Does this line start a HostName or Protocol setting, in either form? */
static int ksp_line_is_session_key(const char *line, size_t n)
{
    static const char *const keys[] = { "HostName", "Protocol" };
    size_t i;
    for (i = 0; i < lenof(keys); i++) {
        size_t kl = strlen(keys[i]);
        if (n > kl && !memcmp(line, keys[i], kl) &&
            (line[kl] == '=' || line[kl] == '\\'))
            return 1;
    }
    return 0;
}

int ksp_text_is_session(const char *buf, size_t len)
{
    size_t i = 0;
    if (!buf)
        return 0;
    while (i < len) {
        size_t s = i;
        while (i < len && buf[i] != '\n' && buf[i] != '\r')
            i++;
        if (ksp_line_is_session_key(buf + s, i - s))
            return 1;
        while (i < len && (buf[i] == '\n' || buf[i] == '\r'))
            i++;
    }
    return 0;
}

int ksp_file_is_session(const char *path)
{
    /* Line by line rather than the whole file: a stray multi-megabyte log in
     * the session folder must not be read in full just to be ruled out. */
    FILE *fp;
    char line[512];
    int found = 0, at_line_start = 1;
    if (!path || !(fp = fopen(path, "rb")))
        return 0;
    while (!found && fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        if (at_line_start && ksp_line_is_session_key(line, n))
            found = 1;
        /* A line longer than the buffer arrives in pieces; only the first
         * piece is the start of a line. */
        at_line_start = (n > 0 && line[n - 1] == '\n');
    }
    fclose(fp);
    return found;
}

/* ---- the verdict cache ----
 * One entry per file judged, keyed by its full path (case-insensitive, as on
 * the disk), with the size and last-write time the directory walk reported
 * when the file was read. A walk opens a file again only when those differ.
 *
 * A file written in the last few seconds is judged afresh at every lookup:
 * its time may not move on a second write - the timestamp tick is about
 * 16 ms on NTFS and two seconds on FAT - and the directory entry of a file
 * another process still has open can lag behind its content. Any writer, this
 * process or another, is covered that way; ksf_save forgets its own file
 * besides. Lookups run on whatever thread lists the store, so the map is
 * under a critical section (lazily set up with Interlocked calls: no Vista+
 * import, the 32-bit build loads on XP). A file is read outside it. */
#define KSP_VERDICT_SETTLE (3ULL * 10000000ULL)   /* 100 ns units */

struct ksp_verdict {
    char *path;
    unsigned long long size, mtime;
    int session;
    int settled;
    unsigned long seen;
};

static tree234 *ksp_verdicts;
static CRITICAL_SECTION ksp_verdict_cs;
static volatile LONG ksp_verdict_cs_state;    /* 0 none, 1 being set up, 2 up */
static unsigned long ksp_verdict_gen = 1;
static volatile LONG ksp_verdict_nreads;

static int ksp_verdict_cmp(void *av, void *bv)
{
    return stricmp(((struct ksp_verdict *)av)->path,
                   ((struct ksp_verdict *)bv)->path);
}

static void ksp_verdict_lock(void)
{
    if (ksp_verdict_cs_state != 2) {
        if (InterlockedCompareExchange(&ksp_verdict_cs_state, 1, 0) == 0) {
            InitializeCriticalSection(&ksp_verdict_cs);
            InterlockedExchange(&ksp_verdict_cs_state, 2);
        } else {
            while (ksp_verdict_cs_state != 2)
                Sleep(0);
        }
    }
    EnterCriticalSection(&ksp_verdict_cs);
    if (!ksp_verdicts)
        ksp_verdicts = newtree234(ksp_verdict_cmp);
}

static void ksp_verdict_unlock(void)
{
    LeaveCriticalSection(&ksp_verdict_cs);
}

static void ksp_verdict_free(struct ksp_verdict *e)
{
    sfree(e->path);
    sfree(e);
}

int ksp_file_verdict(const char *path, unsigned long long size,
                     unsigned long long mtime)
{
    struct ksp_verdict key, *e;
    unsigned long long now;
    FILETIME ft;
    int session;

    if (!path)
        return 0;
    key.path = (char *)path;
    ksp_verdict_lock();
    e = find234(ksp_verdicts, &key, NULL);
    if (e && e->settled && e->size == size && e->mtime == mtime) {
        e->seen = ksp_verdict_gen;
        session = e->session;
        ksp_verdict_unlock();
        return session;
    }
    ksp_verdict_unlock();

    GetSystemTimeAsFileTime(&ft);
    now = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    session = ksp_file_is_session(path);
    InterlockedIncrement(&ksp_verdict_nreads);

    ksp_verdict_lock();
    e = find234(ksp_verdicts, &key, NULL);
    if (!e) {
        e = snew(struct ksp_verdict);
        e->path = dupstr(path);
        add234(ksp_verdicts, e);
    }
    e->size = size;
    e->mtime = mtime;
    e->session = session;
    e->settled = mtime < now && now - mtime >= KSP_VERDICT_SETTLE;
    e->seen = ksp_verdict_gen;
    ksp_verdict_unlock();
    return session;
}

void ksp_verdict_forget(const char *path)
{
    struct ksp_verdict key, *e;
    if (!path)
        return;
    key.path = (char *)path;
    ksp_verdict_lock();
    e = find234(ksp_verdicts, &key, NULL);
    if (e) {
        del234(ksp_verdicts, e);
        ksp_verdict_free(e);
    }
    ksp_verdict_unlock();
}

unsigned long ksp_verdict_walk_begin(void)
{
    unsigned long g;
    ksp_verdict_lock();
    g = ++ksp_verdict_gen;
    ksp_verdict_unlock();
    return g;
}

void ksp_verdict_walk_end(unsigned long gen)
{
    int i;
    ksp_verdict_lock();
    /* Every file of the store was looked up since `gen` began; an entry no
     * lookup touched is a file that is gone (or outside the store now). A
     * walk started later stamps newer generations, so it loses nothing. */
    for (i = count234(ksp_verdicts) - 1; i >= 0; i--) {
        struct ksp_verdict *e = index234(ksp_verdicts, i);
        if ((long)(e->seen - gen) < 0) {
            delpos234(ksp_verdicts, i);
            ksp_verdict_free(e);
        }
    }
    ksp_verdict_unlock();
}

unsigned long ksp_verdict_reads(void)
{
    return (unsigned long)ksp_verdict_nreads;
}

int ksp_verdict_count(void)
{
    int n;
    ksp_verdict_lock();
    n = count234(ksp_verdicts);
    ksp_verdict_unlock();
    return n;
}

int ksp_lookup(char *const *names, int n, const char *wanted,
               int *matches, int maxmatches, int *nmatches)
{
    int i, nm = 0;
    if (nmatches)
        *nmatches = 0;
    if (!wanted || !*wanted || !names)
        return KSP_NONE;
    for (i = 0; i < n; i++)
        if (names[i] && !stricmp(names[i], wanted)) {
            if (matches && maxmatches > 0)
                matches[0] = i;
            if (nmatches)
                *nmatches = 1;
            return KSP_EXACT;
        }
    if (strchr(wanted, '\\'))
        return KSP_NONE;               /* a path is never guessed */
    for (i = 0; i < n; i++)
        if (names[i] && !stricmp(ksp_leaf(names[i]), wanted)) {
            if (matches && nm < maxmatches)
                matches[nm] = i;
            nm++;
        }
    if (nmatches)
        *nmatches = nm;
    if (nm == 0)
        return KSP_NONE;
    return nm == 1 ? KSP_UNIQUE : KSP_AMBIGUOUS;
}

char *ksp_ambiguous_text(const char *wanted, char *const *list, int nlist,
                         int with_load_hint)
{
    strbuf *lines = strbuf_new();
    char *out;
    int i;
    for (i = 0; i < nlist; i++) {
        if (i)
            put_byte(lines, '\n');
        put_dataz(lines, list[i]);
    }
    if (with_load_hint)
        out = dupprintf(KT_SP_LOAD_AMBIGUOUS, wanted, lines->s,
                        nlist > 0 ? list[0] : wanted);
    else
        out = dupprintf(KT_SP_JUMP_AMBIGUOUS, wanted, lines->s);
    strbuf_free(lines);
    return out;
}

/* ---- walking a folder store ----
 * Every session file below `dir`, as path identities. Dot-files and
 * dot-directories are never looked at (a session folder kept in Git holds .git
 * and .gitignore); a file counts only when its content says it is a session
 * (the verdict cache, ksp_file_verdict). Junctions and symlinks are not
 * followed, and the depth is capped, so a loop cannot hang the list. The
 * terminal's store (ksf_enum_sessions) and kageant's tray menu both walk
 * through here, so the two cannot disagree on what a session is. */
static void ksp_walk_add(struct ksp_store_file **v, int *count, int *alloc,
                         char *id, const char *path, const WIN32_FIND_DATAA *fd)
{
    int j;
    for (j = 0; j < *count; j++)
        if (!stricmp((*v)[j].id, id)) {      /* "foo" and "foo<suffix>" */
            sfree(id);
            return;
        }
    if (*count >= *alloc) {
        *alloc = *alloc ? *alloc * 2 : 16;
        *v = sresize(*v, *alloc, struct ksp_store_file);
    }
    (*v)[*count].id = id;
    (*v)[*count].path = dupstr(path);
    (*v)[*count].size =
        ((unsigned long long)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    (*v)[*count].mtime =
        ((unsigned long long)fd->ftLastWriteTime.dwHighDateTime << 32) |
        fd->ftLastWriteTime.dwLowDateTime;
    (*count)++;
}

static void ksp_walk_dir(const char *dir, const char *prefix, int depth,
                         const char *suffix, const char *leaf,
                         struct ksp_store_file **v, int *count, int *alloc)
{
    char *pat = dupprintf("%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(pat, &fd);
    sfree(pat);
    if (hf == INVALID_HANDLE_VALUE)
        return;
    do {
        char *full, *comp;
        if (fd.cFileName[0] == '.')
            continue;                    /* ".", "..", .git, .gitignore */
        /* A folder junction or link is not followed (a loop would never end);
         * a FILE with the attribute is an ordinary placeholder of a synced
         * folder (OneDrive, Dropbox) and is a session like any other. */
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        full = dupprintf("%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            comp = ksp_component_unmunge(fd.cFileName);
            if (depth < KSP_WALK_MAXDEPTH && comp[0] && !strchr(comp, '\\')) {
                char *sub = dupcat(prefix, comp, "\\");
                ksp_walk_dir(full, sub, depth + 1, suffix, leaf, v, count, alloc);
                sfree(sub);
            }
            sfree(comp);
        } else {
            char *fname = dupstr(fd.cFileName);
            ksp_strip_suffix(fname, suffix);
            comp = ksp_component_unmunge(fname);
            sfree(fname);
            /* A '\' inside one component cannot be told from a folder; only
             * the flat legacy files at the top level carry one (that is what
             * they mean: a whole path in one file name). */
            if (comp[0] && (!prefix[0] || !strchr(comp, '\\')) &&
                (!leaf || !stricmp(ksp_leaf(comp), leaf)) &&
                ksp_file_verdict(full,
                    ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                    ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) |
                    fd.ftLastWriteTime.dwLowDateTime))
                ksp_walk_add(v, count, alloc, dupcat(prefix, comp), full, &fd);
            sfree(comp);
        }
        sfree(full);
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
}

struct ksp_store_file *ksp_walk_store(const char *dir, const char *suffix,
                                      const char *leaf, int *count)
{
    struct ksp_store_file *v = NULL;
    int alloc = 0;
    *count = 0;
    if (!dir || !*dir || (leaf && !*leaf))
        return NULL;
    if (!leaf) {
        /* The whole store is met: verdicts of files gone since are dropped. */
        unsigned long gen = ksp_verdict_walk_begin();
        ksp_walk_dir(dir, "", 0, suffix ? suffix : "", NULL, &v, count, &alloc);
        ksp_verdict_walk_end(gen);
    } else {
        ksp_walk_dir(dir, "", 0, suffix ? suffix : "", leaf, &v, count, &alloc);
    }
    return v;
}

void ksp_walk_store_free(struct ksp_store_file *v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        sfree(v[i].id);
        sfree(v[i].path);
    }
    sfree(v);
}

/* ---- a registry key tree, copied and then proved equal ----
 * Every buffer is sized from RegQueryInfoKey, so a value of any length (a
 * long Commands list, a DPAPI blob, a colour table) is copied whole; a value
 * that changes size under the copy fails it rather than being cut short. */

static bool ksp_reg_copy_key(HKEY src, HKEY dst)
{
    DWORD nsub = 0, maxsub = 0, nval = 0, maxname = 0, maxdata = 0, i;
    char *name, *data, *sub;
    bool ok = true;
    if (RegQueryInfoKeyA(src, NULL, NULL, NULL, &nsub, &maxsub, NULL, &nval,
                         &maxname, &maxdata, NULL, NULL) != ERROR_SUCCESS)
        return false;
    name = snewn(maxname + 2, char);
    data = snewn(maxdata + 2, char);
    sub = snewn(maxsub + 2, char);
    for (i = 0; ok && i < nval; i++) {
        DWORD nl = maxname + 1, dl = maxdata + 1, type = 0;
        if (RegEnumValueA(src, i, name, &nl, NULL, &type, (BYTE *)data,
                          &dl) != ERROR_SUCCESS ||
            RegSetValueExA(dst, name, 0, type, (BYTE *)data, dl) != ERROR_SUCCESS)
            ok = false;
    }
    for (i = 0; ok && i < nsub; i++) {
        DWORD sl = maxsub + 1;
        HKEY s2, d2;
        if (RegEnumKeyExA(src, i, sub, &sl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS ||
            RegOpenKeyExA(src, sub, 0, KEY_READ, &s2) != ERROR_SUCCESS) {
            ok = false;
            break;
        }
        if (RegCreateKeyExA(dst, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &d2,
                            NULL) != ERROR_SUCCESS) {
            RegCloseKey(s2);
            ok = false;
            break;
        }
        ok = ksp_reg_copy_key(s2, d2);
        RegCloseKey(d2);
        RegCloseKey(s2);
    }
    sfree(name);
    sfree(data);
    sfree(sub);
    return ok;
}

/* Same value names, types and bytes, same subkeys, all the way down. */
static bool ksp_reg_equal_key(HKEY a, HKEY b)
{
    DWORD nsub = 0, maxsub = 0, nval = 0, maxname = 0, maxdata = 0, i;
    DWORD bsub = 0, bval = 0, bmaxdata = 0;
    char *name, *da, *db, *sub;
    bool ok = true;
    if (RegQueryInfoKeyA(a, NULL, NULL, NULL, &nsub, &maxsub, NULL, &nval,
                         &maxname, &maxdata, NULL, NULL) != ERROR_SUCCESS ||
        RegQueryInfoKeyA(b, NULL, NULL, NULL, &bsub, NULL, NULL, &bval,
                         NULL, &bmaxdata, NULL, NULL) != ERROR_SUCCESS ||
        nsub != bsub || nval != bval || maxdata != bmaxdata)
        return false;
    name = snewn(maxname + 2, char);
    da = snewn(maxdata + 2, char);
    db = snewn(maxdata + 2, char);
    sub = snewn(maxsub + 2, char);
    for (i = 0; ok && i < nval; i++) {
        DWORD nl = maxname + 1, dla = maxdata + 1, dlb = maxdata + 1;
        DWORD ta = 0, tb = 0;
        if (RegEnumValueA(a, i, name, &nl, NULL, &ta, (BYTE *)da, &dla) != ERROR_SUCCESS ||
            RegQueryValueExA(b, name, NULL, &tb, (BYTE *)db, &dlb) != ERROR_SUCCESS ||
            ta != tb || dla != dlb || memcmp(da, db, dla))
            ok = false;
    }
    for (i = 0; ok && i < nsub; i++) {
        DWORD sl = maxsub + 1;
        HKEY a2, b2;
        if (RegEnumKeyExA(a, i, sub, &sl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS ||
            RegOpenKeyExA(a, sub, 0, KEY_READ, &a2) != ERROR_SUCCESS) {
            ok = false;
            break;
        }
        if (RegOpenKeyExA(b, sub, 0, KEY_READ, &b2) != ERROR_SUCCESS) {
            RegCloseKey(a2);
            ok = false;
            break;
        }
        ok = ksp_reg_equal_key(a2, b2);
        RegCloseKey(b2);
        RegCloseKey(a2);
    }
    sfree(name);
    sfree(da);
    sfree(db);
    sfree(sub);
    return ok;
}

static bool ksp_reg_exists(HKEY root, const char *path)
{
    HKEY k;
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

/* Delete a key and everything below it (RegDeleteTree is Vista+, and the
 * 32-bit build still loads on XP). */
bool ksp_reg_delete_tree(HKEY root, const char *path)
{
    HKEY k;
    char sub[512];
    if (RegOpenKeyExA(root, path, 0, KEY_ALL_ACCESS, &k) != ERROR_SUCCESS)
        return !ksp_reg_exists(root, path);
    for (;;) {
        DWORD sl = sizeof(sub);
        char *child;
        bool ok;
        if (RegEnumKeyExA(k, 0, sub, &sl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        child = dupprintf("%s\\%s", path, sub);
        ok = ksp_reg_delete_tree(root, child);
        sfree(child);
        if (!ok) {
            RegCloseKey(k);
            return false;
        }
    }
    RegCloseKey(k);
    return RegDeleteKeyA(root, path) == ERROR_SUCCESS;
}

int ksp_reg_copy_verified(HKEY root, const char *src, const char *dst)
{
    HKEY s, d;
    DWORD disp = 0;
    bool ok;
    if (ksp_reg_exists(root, dst))
        return 0;                       /* never onto something already there */
    if (RegOpenKeyExA(root, src, 0, KEY_READ, &s) != ERROR_SUCCESS)
        return 0;
    if (RegCreateKeyExA(root, dst, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &d,
                        &disp) != ERROR_SUCCESS) {
        RegCloseKey(s);
        return 0;
    }
    ok = ksp_reg_copy_key(s, d) && ksp_reg_equal_key(s, d);
    RegCloseKey(d);
    RegCloseKey(s);
    if (!ok)
        ksp_reg_delete_tree(root, dst);
    return ok;
}

/* Make `to` whole again from the proven copy `from`, after a delete of `to`
 * stopped part-way (what is left of it is a part of the same settings):
 * every value and subkey is written back over it and the two are compared.
 * Returns 1 when `to` equals `from` again. */
static int ksp_reg_restore(HKEY root, const char *from, const char *to)
{
    HKEY s, d;
    DWORD disp = 0;
    bool ok;
    if (RegOpenKeyExA(root, from, 0, KEY_READ, &s) != ERROR_SUCCESS)
        return 0;
    if (RegCreateKeyExA(root, to, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &d,
                        &disp) != ERROR_SUCCESS) {
        RegCloseKey(s);
        return 0;
    }
    ok = ksp_reg_copy_key(s, d) && ksp_reg_equal_key(s, d);
    RegCloseKey(d);
    RegCloseKey(s);
    return ok;
}

/* The temporary key of a change of case: `src` plus a suffix that
 * escape_registry_key never produces ('%' is always followed by two
 * upper-case hex digits there, and the registry ignores case), so no
 * session's key can be taken for it. */
#define KSP_REKEY_TMP_SUFFIX "%rekey"

int ksp_reg_move_tree(HKEY root, const char *src, const char *dst,
                      char **leftover)
{
    if (leftover)
        *leftover = NULL;
    if (!strcmp(src, dst))
        return 1;
    if (stricmp(src, dst)) {
        if (!ksp_reg_copy_verified(root, src, dst))
            return 0;
        if (!ksp_reg_delete_tree(root, src)) {
            /* The copy is proven; the source could not go, and may be only
             * partly there. Make it whole again from the copy and only then
             * drop the copy, so the session is not listed twice. When the
             * source cannot be made whole, the copy is the one complete set
             * of settings left, and it stays. */
            if (ksp_reg_restore(root, dst, src))
                ksp_reg_delete_tree(root, dst);
            else if (leftover)
                *leftover = dupstr(dst);
            return 0;
        }
        return 1;
    }
    /* A change of case only: the registry is case-insensitive, so the copy
     * would land on the source itself. Go through a temporary key, proving
     * each copy before anything is deleted. */
    {
        char *tmp = dupcat(src, KSP_REKEY_TMP_SUFFIX);
        int ok = 0;
        ksp_reg_delete_tree(root, tmp);   /* a leftover of an earlier failure */
        if (ksp_reg_copy_verified(root, src, tmp)) {
            if (ksp_reg_delete_tree(root, src)) {
                if (ksp_reg_copy_verified(root, tmp, dst)) {
                    ok = 1;
                } else if (!ksp_reg_copy_verified(root, tmp, src)) {
                    /* Neither name could be written back: the temporary key
                     * is the only copy left, and it stays. */
                    if (leftover)
                        *leftover = dupstr(tmp);
                    sfree(tmp);
                    return 0;
                }
            } else {
                /* The source is still (at least partly) there: rebuild it
                 * from the proven copy before giving up. */
                if (!ksp_reg_restore(root, tmp, src)) {
                    if (leftover)
                        *leftover = dupstr(tmp);
                    sfree(tmp);
                    return 0;
                }
            }
            ksp_reg_delete_tree(root, tmp);
        }
        sfree(tmp);
        return ok;
    }
}

/* ---- planning a move ---- */

static void ksp_plan_add(char ***a, char ***b, int *n, const char *x,
                         const char *y)
{
    *a = sresize(*a, *n + 1, char *);
    (*a)[*n] = dupstr(x);
    if (b) {
        *b = sresize(*b, *n + 1, char *);
        (*b)[*n] = dupstr(y);
    }
    (*n)++;
}

static void ksp_plan_clash(struct ksp_plan *p, const char *name)
{
    int i;
    for (i = 0; i < p->nclash; i++)
        if (!stricmp(p->clash[i], name))
            return;
    ksp_plan_add(&p->clash, NULL, &p->nclash, name, NULL);
}

/* A target is taken when another session already has that identity, or when
 * an earlier move of the same plan goes there. Its own source does not count
 * (a change of case only). */
static void ksp_plan_check(struct ksp_plan *p, char *const *names, int n)
{
    int i, j;
    for (i = 0; i < p->n; i++) {
        bool taken = false;
        for (j = 0; j < n && !taken; j++)
            if (names[j] && !stricmp(names[j], p->to[i]) &&
                stricmp(names[j], p->from[i]))
                taken = true;
        for (j = 0; j < i && !taken; j++)
            if (!stricmp(p->to[j], p->to[i]))
                taken = true;
        if (taken)
            ksp_plan_clash(p, p->to[i]);
    }
}

/* The same test for one move before it is planned (Arrange): taken by
 * another session, or by a move already in the plan. */
static bool ksp_plan_target_taken(const struct ksp_plan *p, char *const *names,
                                  int n, const char *from, const char *to)
{
    int j;
    for (j = 0; j < n; j++)
        if (names[j] && !stricmp(names[j], to) && stricmp(names[j], from))
            return true;
    for (j = 0; j < p->n; j++)
        if (!stricmp(p->to[j], to))
            return true;
    return false;
}

void ksp_plan_init(struct ksp_plan *p)
{
    memset(p, 0, sizeof(*p));
}

void ksp_plan_free(struct ksp_plan *p)
{
    int i;
    for (i = 0; i < p->n; i++) {
        sfree(p->from[i]);
        sfree(p->to[i]);
    }
    for (i = 0; i < p->nfolder; i++) {
        sfree(p->fname[i]);
        sfree(p->fvalue[i]);
    }
    for (i = 0; i < p->nclash; i++)
        sfree(p->clash[i]);
    sfree(p->from);
    sfree(p->to);
    sfree(p->fname);
    sfree(p->fvalue);
    sfree(p->clash);
    ksp_plan_init(p);
}

static bool ksp_is_default_session(const char *name)
{
    return !strcmp(name, "Default Settings");
}

void ksp_plan_arrange(char *const *names, char *const *folders, int n,
                      struct ksp_plan *p)
{
    int i;
    ksp_plan_init(p);
    for (i = 0; i < n; i++) {
        char *fld, *to;
        if (!names[i] || strchr(names[i], '\\') ||
            ksp_is_default_session(names[i]))
            continue;
        if (!folders || ksp_folder_is_root(folders[i]))
            continue;
        fld = ksp_normalise(folders[i]);
        if (!*fld) {
            sfree(fld);
            continue;
        }
        to = dupprintf("%s\\%s", fld, names[i]);
        /* A taken path is listed, not planned: the free ones still move. */
        if (ksp_plan_target_taken(p, names, n, names[i], to))
            ksp_plan_clash(p, to);
        else
            ksp_plan_add(&p->from, &p->to, &p->n, names[i], to);
        sfree(to);
        sfree(fld);
    }
}

int ksp_plan_arrange_offer_due(const struct ksp_plan *p)
{
    return p->nclash > 0;
}

void ksp_plan_folder_move(char *const *names, char *const *folders, int n,
                          const char *from, const char *to,
                          struct ksp_plan *p)
{
    int i;
    ksp_plan_init(p);
    for (i = 0; i < n; i++) {
        if (!names[i] || ksp_is_default_session(names[i]))
            continue;
        if (strchr(names[i], '\\')) {
            char *fld = ksp_folder_of(names[i]);
            if (fld && ksp_folder_within(fld, from)) {
                char *t = ksp_folder_moved_path(names[i], from, to);
                ksp_plan_add(&p->from, &p->to, &p->n, names[i], t);
                sfree(t);
            }
            sfree(fld);
        } else if (folders && !ksp_folder_is_root(folders[i]) &&
                   ksp_folder_within(folders[i], from)) {
            char *v = ksp_folder_moved_path(folders[i], from, to);
            ksp_plan_add(&p->fname, &p->fvalue, &p->nfolder, names[i], v);
            sfree(v);
        }
    }
    ksp_plan_check(p, names, n);
}

void ksp_plan_sessions_move(char *const *names, int n, char *const *sel,
                            int nsel, const char *dest, struct ksp_plan *p)
{
    int i;
    ksp_plan_init(p);
    for (i = 0; i < nsel; i++) {
        char *t;
        if (!sel[i] || ksp_is_default_session(sel[i]))
            continue;
        t = ksp_folder_is_root(dest) ? dupstr(ksp_leaf(sel[i]))
                                     : dupprintf("%s\\%s", dest, ksp_leaf(sel[i]));
        ksp_plan_add(&p->from, &p->to, &p->n, sel[i], t);
        sfree(t);
    }
    ksp_plan_check(p, names, n);
}

char *ksp_import_name(const char *relpath, const char *suffix)
{
    char *norm = ksp_normalise(relpath ? relpath : "");
    strbuf *sb = strbuf_new();
    const char *q = norm;
    while (*q) {
        const char *e = strchr(q, '\\');
        size_t len = e ? (size_t)(e - q) : strlen(q);
        char *comp = dupprintf("%.*s", (int)len, q), *un;
        if (!e)
            ksp_strip_suffix(comp, suffix);   /* the file name only */
        un = ksp_component_unmunge(comp);
        if (sb->len)
            put_byte(sb, '\\');
        put_dataz(sb, un);
        sfree(un);
        sfree(comp);
        q += len;
        if (*q == '\\')
            q++;
    }
    sfree(norm);
    return strbuf_to_str(sb);
}
