/*
 * kitty_logkeep.c - per-session log retention (cyd01/KiTTY#439).
 *
 * A session with a time code in its log name (kitty_&H_&Y&M&D.log) leaves a
 * new file behind every day, or every rotation, and nothing ever removed them.
 * Two per-session settings trim them the way journald trims its journal: by
 * age ("Delete logs after N days", like MaxRetentionSec) and by the total size
 * of the set ("Keep logsize below N MB", like SystemMaxUse, oldest first).
 *
 * The whole design is about what may be deleted, because a wrong answer
 * destroys somebody's data permanently:
 *
 *  - Only names this session's log-name pattern could have produced. The
 *    configured name is turned into a pattern code by code, exactly as
 *    xlatlognam() in logging.c substitutes it: a fixed-width date or time
 *    code becomes that many digit positions, a host or port becomes "any
 *    run". Candidates are matched against that pattern HERE, character by
 *    character. FindFirstFileW's own wildcards are not trusted: they also
 *    match the 8.3 short name (REPORT~1.LOG), and their '?' may match no
 *    character at all before a dot or at the end.
 *  - Only in the folder the session's log goes to, never a subfolder.
 *  - Nothing at all when the name part has no time code: such a session
 *    writes one file only, and a pattern with &H but no date would match the
 *    logs of every other host.
 *  - Never the log this session writes or is about to write, and never a
 *    file that anything else has open (see lk_delete).
 *  - The pattern must match the session's own current file name. If it does
 *    not - for example an environment variable whose value contains '&', so that the
 *    two expansions disagree - the pattern is not this session's and nothing
 *    is touched.
 *
 * Deletion is permanent (DeleteFileW, no recycle bin): a retention rule that
 * filled the recycle bin instead would not free the space it was set for.
 *
 * Everything here is Windows XP era: FindFirstFileW, CreateFileW, DeleteFileW,
 * GetSystemTimeAsFileTime and the Application event log.
 */
#include <limits.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>

#include "putty.h"
#include "kitty_text.h"
#include "kitty_renameguard.h"   /* kitty_eventlog_line */
#include "kitty_logkeep.h"

/* One position of the name pattern. */
enum { LK_LIT, LK_DIGIT, LK_ANY };
typedef struct {
    int kind;
    wchar_t ch;                        /* LK_LIT only */
} lk_tok;

typedef struct {
    wchar_t *path;
    ULONGLONG size;
    ULONGLONG mtime;                   /* last write, FILETIME ticks */
    bool gone;
} lk_file;

/* 100 ns FILETIME ticks in a day. */
#define LK_TICKS_PER_DAY 864000000000ULL

static bool lk_is_sep(wchar_t c)
{
    return c == L'\\' || c == L'/' || c == L':';
}

static void lk_add(lk_tok *t, size_t *n, int kind, wchar_t ch, int count)
{
    while (count-- > 0) {
        /* Two "any run" in a row match what one does; keep one, so the
         * matcher's backtracking stays linear. */
        if (kind == LK_ANY && *n > 0 && t[*n - 1].kind == LK_ANY)
            continue;
        t[*n].kind = kind;
        t[*n].ch = ch;
        (*n)++;
    }
}

/*
 * The NAME part of the log file name as a pattern, following xlatlognam()
 * code by code (case-insensitive, as it is):
 *
 *   &Y -> 4 digits    &M -> 2 digits    &D -> 2 digits    &T -> 6 digits
 *   &H -> any run     &P -> any run     &&  -> '&'
 *   &x (any other)    -> '&' and x, x sanitised as a file name character
 *   '&' at the very end -> nothing
 *
 * &Y &M &D &T are the time codes. Only the part after the last literal path
 * separator is kept: a code's output never contains one (xlatlognam
 * sanitises it), so the separators that split folder from name are all
 * literal. *has_time reports whether that name part holds a time code.
 */
static lk_tok *lk_pattern(const wchar_t *s, size_t *ntok, bool *has_time)
{
    lk_tok *t = snewn(wcslen(s) * 3 + 1, lk_tok);  /* "&T" = 2 chars, 6 tokens */
    size_t n = 0;

    *has_time = false;
    while (*s) {
        wchar_t c;
        if (*s != L'&') {
            if (lk_is_sep(*s)) {
                n = 0;                 /* a folder part: start the name over */
                *has_time = false;
            } else {
                lk_add(t, &n, LK_LIT, *s, 1);
            }
            s++;
            continue;
        }
        s++;
        if (!*s)
            break;                     /* a trailing '&' produces nothing */
        c = *s++;
        switch (c < 128 ? tolower((int)c) : (int)c) {
          case 'y': lk_add(t, &n, LK_DIGIT, 0, 4); *has_time = true; break;
          case 'm': lk_add(t, &n, LK_DIGIT, 0, 2); *has_time = true; break;
          case 'd': lk_add(t, &n, LK_DIGIT, 0, 2); *has_time = true; break;
          case 't': lk_add(t, &n, LK_DIGIT, 0, 6); *has_time = true; break;
          case 'h':
          case 'p': lk_add(t, &n, LK_ANY, 0, 1); break;
          default:
            lk_add(t, &n, LK_LIT, L'&', 1);
            if (c != L'&')
                lk_add(t, &n, LK_LIT,
                       c < 128 ? (wchar_t)filename_char_sanitise((char)c) : c, 1);
            break;
        }
    }
    *ntok = n;
    return t;
}

/* Does `s` match the pattern, whole? Letters compare case-insensitively, as
 * Windows file names do; a digit position takes exactly one digit. */
static bool lk_match(const lk_tok *t, size_t nt, const wchar_t *s)
{
    while (nt > 0) {
        if (t->kind == LK_ANY) {
            do {
                if (lk_match(t + 1, nt - 1, s))
                    return true;
            } while (*s++);
            return false;
        }
        if (!*s)
            return false;
        if (t->kind == LK_DIGIT ? !(*s >= L'0' && *s <= L'9')
                                : towlower(*s) != towlower(t->ch))
            return false;
        t++;
        nt--;
        s++;
    }
    return !*s;
}

/* The folder and pattern as the event log shows them: '?' per digit, '*' per
 * run - the familiar wildcard spelling of what was searched. */
static char *lk_display(const wchar_t *folder, const lk_tok *t, size_t nt)
{
    size_t fl = wcslen(folder), i;
    wchar_t *w = snewn(fl + nt + 1, wchar_t);
    char *out;

    wmemcpy(w, folder, fl);
    for (i = 0; i < nt; i++)
        w[fl + i] = t[i].kind == LK_LIT ? t[i].ch :
                    t[i].kind == LK_DIGIT ? L'?' : L'*';
    w[fl + nt] = L'\0';
    out = dup_wc_to_mb(DEFAULT_CODEPAGE, w, "?");
    sfree(w);
    return out;
}

/*
 * Delete one file, but only if nothing else has it open.
 *
 * The open requests DELETE access and shares nothing but delete. That fails
 * with a sharing violation while any other handle has the file open for
 * reading or writing - another KiTTY still logging into it, a viewer, a copy
 * in progress - and a failure means the file is skipped. While the handle is
 * held nobody else can open the file either, so the DeleteFileW that follows
 * (which opens it again with delete sharing, which this handle allows)
 * removes the file that was checked; it goes when the handle closes.
 */
static bool lk_delete(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, DELETE, FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    bool ok;

    if (h == INVALID_HANDLE_VALUE)
        return false;
    ok = DeleteFileW(path) != 0;
    CloseHandle(h);
    return ok;
}

static int lk_by_age(const void *a, const void *b)
{
    const lk_file *fa = (const lk_file *)a, *fb = (const lk_file *)b;
    return fa->mtime < fb->mtime ? -1 : fa->mtime > fb->mtime ? 1 : 0;
}

bool kitty_logkeep_has_time_code(const Filename *configured)
{
    size_t n;
    bool has_time = false;
    lk_tok *t;
    if (!configured || !configured->wpath)
        return false;
    t = lk_pattern(configured->wpath, &n, &has_time);
    sfree(t);
    return has_time;
}

char *kitty_logkeep_run(const Filename *configured, const Filename *active,
                        int keep_days, int keep_mb)
{
    wchar_t *cfg = NULL, *act = NULL, *folder = NULL, *search = NULL;
    const wchar_t *actname;
    lk_tok *pat = NULL;
    size_t npat = 0, nfiles = 0, filesize = 0, i;
    lk_file *files = NULL;
    bool has_time;
    ULONGLONG total = 0, freed = 0;
    int ndays = 0, nmb = 0;
    char *ret = NULL;
    WIN32_FIND_DATAW fd;
    HANDLE fh;

    if ((keep_days <= 0 && keep_mb <= 0) || !configured || !active)
        return NULL;

    /* %VAR% first, as the log open does (f_open expands the whole path), so
     * a variable holding the folder is split off like a typed folder. */
    cfg = filename_expand_wstr(configured);
    pat = lk_pattern(cfg, &npat, &has_time);
    if (!has_time)
        goto out;

    /* The folder searched is the one this session's log is in. */
    act = filename_expand_wstr(active);
    actname = act + wcslen(act);
    while (actname > act && !lk_is_sep(actname[-1]))
        actname--;
    if (!*actname || !lk_match(pat, npat, actname))
        goto out;                      /* the pattern is not this session's */
    folder = snewn(actname - act + 1, wchar_t);
    wmemcpy(folder, act, actname - act);
    folder[actname - act] = L'\0';

    search = snewn(wcslen(folder) + 2, wchar_t);
    wcscpy(search, folder);
    wcscat(search, L"*");
    fh = FindFirstFileW(search, &fd);
    if (fh == INVALID_HANDLE_VALUE)
        goto out;
    do {
        ULONGLONG size;
        size_t fl;
        if (fd.dwFileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            continue;                  /* files only, and no links */
        if (!lk_match(pat, npat, fd.cFileName))
            continue;
        size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        total += size;                 /* the active log counts toward the cap */
        if (!_wcsicmp(fd.cFileName, actname))
            continue;                  /* ...but is never a candidate */
        sgrowarray(files, filesize, nfiles);
        fl = wcslen(folder) + wcslen(fd.cFileName) + 1;
        files[nfiles].path = snewn(fl, wchar_t);
        wcscpy(files[nfiles].path, folder);
        wcscat(files[nfiles].path, fd.cFileName);
        files[nfiles].size = size;
        files[nfiles].mtime =
            ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) |
            fd.ftLastWriteTime.dwLowDateTime;
        files[nfiles].gone = false;
        nfiles++;
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);

    if (nfiles)
        qsort(files, nfiles, sizeof(*files), lk_by_age);

    /* By age: everything last written more than N days ago. */
    if (keep_days > 0) {
        FILETIME ft;
        ULONGLONG now;
        GetSystemTimeAsFileTime(&ft);
        now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        /* A limit longer than the FILETIME epoch has nothing to delete, and
         * would overflow the multiplication. */
        if ((ULONGLONG)keep_days <= now / LK_TICKS_PER_DAY) {
            ULONGLONG cutoff = now - (ULONGLONG)keep_days * LK_TICKS_PER_DAY;
            for (i = 0; i < nfiles; i++) {
                if (files[i].mtime >= cutoff)
                    break;             /* sorted: the rest are newer */
                if (lk_delete(files[i].path)) {
                    files[i].gone = true;
                    total -= files[i].size;
                    freed += files[i].size;
                    ndays++;
                }
            }
        }
    }

    /* By size: oldest first until the set is within the cap. A file that
     * cannot be deleted still counts, so the loop may run out of candidates
     * with the total still over - it ends on the list, not on the total. */
    if (keep_mb > 0) {
        ULONGLONG cap = (ULONGLONG)keep_mb * 1048576ULL;
        for (i = 0; i < nfiles && total > cap; i++) {
            if (files[i].gone)
                continue;
            if (lk_delete(files[i].path)) {
                files[i].gone = true;
                total -= files[i].size;
                freed += files[i].size;
                nmb++;
            }
        }
    }

    if (ndays + nmb > 0) {
        char *rd = ndays ? dupprintf(KT_LOGKEEP_RULE_DAYS, keep_days) : NULL;
        char *rm = nmb ? dupprintf(KT_LOGKEEP_RULE_MB, keep_mb) : NULL;
        char *rules = dupprintf("%s%s%s", rd ? rd : "",
                                (rd && rm) ? ", " : "", rm ? rm : "");
        char *disp = lk_display(folder, pat, npat);
        /* Kilobytes in an unsigned long: the 32-bit build's C library does
         * not print a 64-bit integer portably. */
        ULONGLONG kb = (freed + 1023) / 1024;
        ret = dupprintf(KT_LOGKEEP_DELETED, ndays + nmb,
                        kb > ULONG_MAX ? ULONG_MAX : (unsigned long)kb,
                        rules, disp);
        kitty_eventlog_line(EVENTLOG_INFORMATION_TYPE, ret);
        sfree(rd);
        sfree(rm);
        sfree(rules);
        sfree(disp);
    }

  out:
    for (i = 0; i < nfiles; i++)
        sfree(files[i].path);
    sfree(files);
    sfree(search);
    sfree(folder);
    sfree(act);
    sfree(pat);
    sfree(cfg);
    return ret;
}
