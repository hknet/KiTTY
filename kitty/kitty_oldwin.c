/*
 * kitty_oldwin.c: runtime lookup of the Windows APIs that are newer than the
 * oldest Windows KiTTY is meant to load on. See kitty_oldwin.h for why.
 *
 * Each entry follows the same shape: resolve once, remember the answer
 * (including "not there"), and fall back to something the older system has.
 * Nothing here fails a call by itself - a caller gets either the modern
 * answer, the old one, or an explicit "cannot tell".
 */
#include "putty.h"
#include <windows.h>
#include "kitty_oldwin.h"

/* ------------------------------------------------------------ resolving -- */

/*
 * What a lookup found, remembered so the reports can name it later. Small and
 * fixed: this is the list of APIs we are willing to be uncertain about, and it
 * does not grow at runtime.
 */
#define KAPI_MAX 32
static struct kapi_note {
    const char *dll, *symbol, *feature;
    int need;
    int found;
} kapi_notes[KAPI_MAX];
static int kapi_count;

static void kapi_note(const char *dll, const char *symbol, int need,
                      const char *feature, int found)
{
    int i;
    for (i = 0; i < kapi_count; i++)
        if (!strcmp(kapi_notes[i].symbol, symbol) &&
            !strcmp(kapi_notes[i].dll, dll))
            return;                    /* already recorded; resolve once */
    if (kapi_count >= KAPI_MAX)
        return;
    kapi_notes[kapi_count].dll = dll;
    kapi_notes[kapi_count].symbol = symbol;
    kapi_notes[kapi_count].feature = feature;
    kapi_notes[kapi_count].need = need;
    kapi_notes[kapi_count].found = found;
    kapi_count++;
}

void kitty_api_record(const char *dll, const char *symbol, int need,
                      const char *feature, int found)
{
    kapi_note(dll, symbol, need, feature, found);
}

FARPROC kitty_api_from(HMODULE module, const char *dll, const char *symbol,
                       int need, const char *feature)
{
    FARPROC fn = module ? GetProcAddress(module, symbol) : NULL;
    kapi_note(dll, symbol, need, feature, fn != NULL);
    return fn;
}

FARPROC kitty_api(const char *dll, const char *symbol, int need,
                  const char *feature)
{
    FARPROC fn = NULL;
    HMODULE h;

    /*
     * GetModuleHandle first: for a DLL already in the process (kernel32,
     * user32) this must never become a reason to LOAD one, which is how a
     * search-path hijack gets in. Only if it is not loaded do we ask for it by
     * plain name - by which time dll_hijacking_protection() has restricted the
     * search path.
     */
    h = GetModuleHandleA(dll);
    if (!h)
        h = LoadLibraryA(dll);
    if (h)
        fn = GetProcAddress(h, symbol);

    kapi_note(dll, symbol, need, feature, fn != NULL);
    return fn;
}

static char *kapi_report(int need)
{
    strbuf *sb = NULL;
    int i;

    for (i = 0; i < kapi_count; i++) {
        if (kapi_notes[i].found || kapi_notes[i].need != need)
            continue;
        if (!sb) {
            sb = strbuf_new();
            put_dataz(sb, need == KITTY_API_REQUIRED
                      ? "This version of Windows is too old to run KiTTY.\r\n\r\n"
                        "It is missing:"
                      : "Not available on this version of Windows:");
        }
        put_fmt(sb, "\r\n    %s - needs %s from %s", kapi_notes[i].feature,
                kapi_notes[i].symbol, kapi_notes[i].dll);
    }
    if (!sb)
        return NULL;
    if (need == KITTY_API_REQUIRED)
        put_dataz(sb, "\r\n\r\nKiTTY needs Windows XP or newer.");
    return strbuf_to_str(sb);
}

char *kitty_oldwin_required_missing(void) { return kapi_report(KITTY_API_REQUIRED); }
char *kitty_oldwin_degraded(void)         { return kapi_report(KITTY_API_OPTIONAL); }

char *kitty_oldwin_degraded_brief(void)
{
    strbuf *sb = NULL;
    int i, j;

    /*
     * The FEATURES, not the symbols: one line in a terminal has room for
     * "dark mode, Windows Hello", not for four entry points from
     * combase.dll. Several symbols share a feature, so each name is printed
     * once.
     */
    for (i = 0; i < kapi_count; i++) {
        if (kapi_notes[i].found || kapi_notes[i].need != KITTY_API_OPTIONAL)
            continue;
        for (j = 0; j < i; j++)
            if (!kapi_notes[j].found &&
                kapi_notes[j].need == KITTY_API_OPTIONAL &&
                !strcmp(kapi_notes[j].feature, kapi_notes[i].feature))
                break;
        if (j < i)
            continue;                  /* this feature is already named */
        if (!sb)
            sb = strbuf_new();
        else
            put_dataz(sb, ", ");
        put_dataz(sb, kapi_notes[i].feature);
    }
    return sb ? strbuf_to_str(sb) : NULL;
}

/* ---------------------------------------------------------------- ticks -- */

typedef ULONGLONG (WINAPI *gettickcount64_t)(void);

static gettickcount64_t p_GetTickCount64;
static int tick_resolved;

/*
 * The wrap accountancy for the fallback, as ONE 64-bit word: the high half
 * counts wraps seen, the low half is the last reading. Updated with a
 * compare-and-swap because the agent answers requests on more than one
 * thread, and two threads reading either side of a wrap must not disagree
 * about the high half.
 *
 * A GCC atomic builtin rather than InterlockedCompareExchange64: the builtin
 * compiles to an instruction (cmpxchg8b on 32-bit), while the API of that
 * name is itself a Vista import - which is the exact problem this file
 * exists to avoid.
 */
static volatile unsigned long long tick_state;

static void tick_resolve(void)
{
    if (tick_resolved)
        return;
    /* OPTIONAL: the wrap-carrying fallback below is exact for the deltas both
     * callers measure, so nothing is lost and the user is told nothing. */
    p_GetTickCount64 = (gettickcount64_t)
        kitty_api("kernel32.dll", "GetTickCount64", KITTY_API_OPTIONAL,
                  "a 64-bit millisecond clock");
    tick_resolved = 1;
}

ULONGLONG kitty_tick_count64(void)
{
    tick_resolve();
    if (p_GetTickCount64)
        return p_GetTickCount64();

    /* Pre-Vista: 32 bits that wrap every 49.7 days. Carry the wrap count so a
     * caller comparing two readings is never handed one that went backwards. */
    for (;;) {
        unsigned long long old = tick_state;
        DWORD last = (DWORD)(old & 0xFFFFFFFFULL);
        unsigned long long wraps = old >> 32;
        DWORD now = GetTickCount();

        if (now < last)
            wraps++;               /* the counter went round since last time */
        unsigned long long updated = (wraps << 32) | now;

        if (__sync_bool_compare_and_swap(&tick_state, old, updated))
            return (wraps << 32) + now;
        /* Another thread got there first; read it again and redo the sum. */
    }
}

/* --------------------------------------------------------- process path -- */

typedef BOOL (WINAPI *queryfullprocessimagenamea_t)(HANDLE, DWORD, LPSTR, PDWORD);
typedef DWORD (WINAPI *getmodulefilenameexa_t)(HANDLE, HMODULE, LPSTR, DWORD);

static queryfullprocessimagenamea_t p_QueryFullProcessImageNameA;
static getmodulefilenameexa_t p_GetModuleFileNameExA;
static int path_resolved;

static void path_resolve(void)
{
    if (path_resolved)
        return;
    p_QueryFullProcessImageNameA = (queryfullprocessimagenamea_t)
        kitty_api("kernel32.dll", "QueryFullProcessImageNameA",
                  KITTY_API_OPTIONAL, "naming the program behind a process");
    if (!p_QueryFullProcessImageNameA)
        p_GetModuleFileNameExA = (getmodulefilenameexa_t)
            kitty_api("psapi.dll", "GetModuleFileNameExA",
                      KITTY_API_OPTIONAL,
                      "naming the program behind a process (older Windows)");
    path_resolved = 1;
}

BOOL kitty_process_image_path(HANDLE proc, char *buf, DWORD bufsize)
{
    if (!buf || bufsize == 0)
        return FALSE;
    path_resolve();

    if (p_QueryFullProcessImageNameA) {
        DWORD n = bufsize;
        if (p_QueryFullProcessImageNameA(proc, 0, buf, &n) && n > 0) {
            buf[bufsize - 1] = '\0';
            return TRUE;
        }
        return FALSE;
    }

    if (p_GetModuleFileNameExA) {
        /* The NT 4 way. It wants PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,
         * which is a stronger right than the modern call needs, so this can
         * fail where the other would have worked - reported as "cannot tell",
         * which is the safe answer for every caller here. */
        DWORD n = p_GetModuleFileNameExA(proc, NULL, buf, bufsize);
        if (n > 0 && n < bufsize) {
            buf[n] = '\0';
            return TRUE;
        }
    }
    return FALSE;
}

/* -------------------------------------------------------------- console -- */

typedef BOOL (WINAPI *attachconsole_t)(DWORD);

BOOL kitty_attach_parent_console(void)
{
    static attachconsole_t p_AttachConsole;
    static int resolved;

    if (!resolved) {
        p_AttachConsole = (attachconsole_t)
            kitty_api("kernel32.dll", "AttachConsole", KITTY_API_OPTIONAL,
                      "printing to the console that started KiTTY");
        resolved = 1;
    }
    if (!p_AttachConsole)
        return FALSE;                  /* pre-XP: no parent console to attach */
    return p_AttachConsole(ATTACH_PARENT_PROCESS);
}
