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
#include "kitty_text.h"     /* the report wordings and feature names */

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
    /* Never store a NULL description: BOTH report builders format it with
     * %s / compare it with strcmp, and the first NULL to arrive took the
     * process down on XP - twice, once per report, because the first fix
     * guarded one consumer instead of the source. */
    if (!feature)
        feature = KT_OLDWIN_UNNAMED;
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
                      ? KT_OLDWIN_TOO_OLD
                      : KT_OLDWIN_NOT_AVAILABLE);
        }
        put_fmt(sb, KT_OLDWIN_ITEM, kapi_notes[i].feature,
                kapi_notes[i].symbol, kapi_notes[i].dll);
    }
    if (!sb)
        return NULL;
    if (need == KITTY_API_REQUIRED)
        put_dataz(sb, KT_OLDWIN_NEEDS_XP);
    return strbuf_to_str(sb);
}

/* A NULL feature description must never reach strcmp/printf: one did, and
 * the degraded-features line took the whole process down on XP - inside
 * msvcrt, in the report about the degradation. Normalized here so every
 * consumer below can trust the field. */
static const char *kapi_feature_of(int i)
{
    return kapi_notes[i].feature ? kapi_notes[i].feature : KT_OLDWIN_UNNAMED;
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
                !strcmp(kapi_feature_of(j), kapi_feature_of(i)))
                break;
        if (j < i)
            continue;                  /* this feature is already named */
        if (!sb)
            sb = strbuf_new();
        else
            put_dataz(sb, ", ");
        put_dataz(sb, kapi_feature_of(i));
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
                  KT_WINFEAT_TICK64);
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
                  KITTY_API_OPTIONAL, KT_WINFEAT_PROCESS_NAME);
    if (!p_QueryFullProcessImageNameA)
        p_GetModuleFileNameExA = (getmodulefilenameexa_t)
            kitty_api("psapi.dll", "GetModuleFileNameExA",
                      KITTY_API_OPTIONAL,
                      KT_WINFEAT_PROCESS_NAME_OLD);
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
                      KT_WINFEAT_CONSOLE);
        resolved = 1;
    }
    if (!p_AttachConsole)
        return FALSE;                  /* pre-XP: no parent console to attach */
    return p_AttachConsole(ATTACH_PARENT_PROCESS);
}

/* ---- registry APIs newer than XP (see kitty_oldwin_reg.h) ---------------
 *
 * OPTIONAL and silent, like GetTickCount64: the fallbacks are exact for
 * every shape this codebase uses (subkey non-NULL; flags exactly one of
 * RRF_RT_REG_SZ / _DWORD / _BINARY), so nothing is lost and the user is
 * told nothing. The NULL-subkey and multi-type-mask variants are
 * deliberately not emulated - no caller has them, and an unemulated case
 * failing loudly beats one working differently.
 */
typedef LSTATUS (WINAPI *regdeltree_t)(HKEY, LPCSTR);
typedef LSTATUS (WINAPI *reggetvalue_t)(HKEY, LPCSTR, LPCSTR, DWORD,
                                        LPDWORD, PVOID, LPDWORD);
static regdeltree_t p_RegDeleteTreeA;
static reggetvalue_t p_RegGetValueA;
static int reg_resolved;

static void reg_resolve(void)
{
    if (reg_resolved)
        return;
    p_RegDeleteTreeA = (regdeltree_t)
        kitty_api("advapi32.dll", "RegDeleteTreeA", KITTY_API_OPTIONAL,
                  KT_WINFEAT_REG_DELTREE);
    p_RegGetValueA = (reggetvalue_t)
        kitty_api("advapi32.dll", "RegGetValueA", KITTY_API_OPTIONAL,
                  KT_WINFEAT_REG_GETVALUE);
    reg_resolved = 1;
}

static LSTATUS reg_deltree_fallback(HKEY key, LPCSTR subkey)
{
    HKEY sub;
    LSTATUS st = RegOpenKeyExA(key, subkey, 0,
                               KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &sub);
    if (st != ERROR_SUCCESS)
        return st;
    /* Always index 0, re-enumerated after each delete: deleting shifts the
     * enumeration under an index that walks forward. Values never block a
     * RegDeleteKey; only subkeys do. */
    for (;;) {
        char name[256];
        DWORD cch = sizeof(name);
        st = RegEnumKeyExA(sub, 0, name, &cch, NULL, NULL, NULL, NULL);
        if (st == ERROR_NO_MORE_ITEMS) {
            st = ERROR_SUCCESS;
            break;
        }
        if (st != ERROR_SUCCESS)
            break;
        st = reg_deltree_fallback(sub, name);
        if (st != ERROR_SUCCESS)
            break;
    }
    RegCloseKey(sub);
    if (st == ERROR_SUCCESS)
        st = RegDeleteKeyA(key, subkey);
    return st;
}

LSTATUS kitty_oldwin_RegDeleteTreeA(HKEY key, LPCSTR subkey)
{
    reg_resolve();
    if (p_RegDeleteTreeA)
        return p_RegDeleteTreeA(key, subkey);
    if (!subkey || !*subkey)
        return ERROR_CALL_NOT_IMPLEMENTED;   /* no caller passes this */
    return reg_deltree_fallback(key, subkey);
}

LSTATUS kitty_oldwin_RegGetValueA(HKEY key, LPCSTR subkey, LPCSTR value,
                                  DWORD flags, LPDWORD ptype, PVOID data,
                                  LPDWORD psize)
{
    HKEY sub = key;
    DWORD type = REG_NONE, cb;
    LSTATUS st;
    DWORD want = 0;

    reg_resolve();
    if (p_RegGetValueA)
        return p_RegGetValueA(key, subkey, value, flags, ptype, data, psize);

    if (flags == RRF_RT_REG_SZ)          want = REG_SZ;
    else if (flags == RRF_RT_REG_DWORD)  want = REG_DWORD;
    else if (flags == RRF_RT_REG_BINARY) want = REG_BINARY;
    else return ERROR_CALL_NOT_IMPLEMENTED;   /* shape nobody uses */

    if (subkey && *subkey) {
        st = RegOpenKeyExA(key, subkey, 0, KEY_QUERY_VALUE, &sub);
        if (st != ERROR_SUCCESS)
            return st;
    }
    cb = psize ? *psize : 0;
    st = RegQueryValueExA(sub, value, NULL, &type, (LPBYTE)data, &cb);
    if (st == ERROR_SUCCESS || st == ERROR_MORE_DATA) {
        if (type != want) {
            st = ERROR_UNSUPPORTED_TYPE;
        } else if (st == ERROR_SUCCESS && data) {
            if (want == REG_DWORD && cb != sizeof(DWORD)) {
                st = ERROR_UNSUPPORTED_TYPE;
            } else if (want == REG_SZ) {
                /* RegGetValue GUARANTEES termination; QueryValueEx does
                 * not. Terminate in place, or refuse if the buffer is
                 * exactly full of non-NUL bytes. */
                char *s = (char *)data;
                if (cb == 0 || s[cb - 1] != '\0') {
                    if (psize && cb < *psize) {
                        s[cb] = '\0';
                        cb++;
                    } else {
                        st = ERROR_MORE_DATA;
                    }
                }
            }
        }
    }
    if (ptype)
        *ptype = type;
    if (psize)
        *psize = cb;
    if (sub != key)
        RegCloseKey(sub);
    return st;
}

/* conpty.c's spawn plumbing. Never reached where the APIs are absent -
 * conpty availability (CreatePseudoConsole, itself runtime-resolved) is
 * checked before any spawn - so the fallback only exists to keep the
 * LOADER from refusing pterm on XP. */
typedef BOOL (WINAPI *initptal_t)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD,
                                  DWORD, PSIZE_T);
typedef BOOL (WINAPI *updpta_t)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD,
                                DWORD_PTR, PVOID, SIZE_T, PVOID, PSIZE_T);
static initptal_t p_InitializeProcThreadAttributeList;
static updpta_t p_UpdateProcThreadAttribute;
static int ptal_resolved;

static void ptal_resolve(void)
{
    if (ptal_resolved)
        return;
    p_InitializeProcThreadAttributeList = (initptal_t)
        kitty_api("kernel32.dll", "InitializeProcThreadAttributeList",
                  KITTY_API_OPTIONAL, KT_WINFEAT_CONPTY);
    p_UpdateProcThreadAttribute = (updpta_t)
        kitty_api("kernel32.dll", "UpdateProcThreadAttribute",
                  KITTY_API_OPTIONAL, KT_WINFEAT_CONPTY);
    ptal_resolved = 1;
}

BOOL kitty_oldwin_InitializeProcThreadAttributeList(
    LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD count, DWORD flags, PSIZE_T size)
{
    ptal_resolve();
    if (p_InitializeProcThreadAttributeList)
        return p_InitializeProcThreadAttributeList(list, count, flags, size);
    SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
}

BOOL kitty_oldwin_UpdateProcThreadAttribute(
    LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD flags, DWORD_PTR attr,
    PVOID value, SIZE_T cb, PVOID prev, PSIZE_T rsize)
{
    ptal_resolve();
    if (p_UpdateProcThreadAttribute)
        return p_UpdateProcThreadAttribute(list, flags, attr, value, cb,
                                           prev, rsize);
    SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
}

/* Vista+: who is serving a named pipe. The agent client's unknown-agent
 * check is the one caller, and it is observational - so the fallback just
 * says "cannot tell" and the check is skipped. NOT silent-exact like the
 * registry wrappers: losing the observation is a real (small) degradation,
 * so it goes through the OPTIONAL notes and shows up in the degraded
 * report. */
typedef BOOL (WINAPI *getpipesrvpid_t)(HANDLE, PULONG);
static getpipesrvpid_t p_GetNamedPipeServerProcessId;
static int pipepid_resolved;

BOOL kitty_oldwin_GetNamedPipeServerProcessId(HANDLE pipe, PULONG pid)
{
    if (!pipepid_resolved) {
        p_GetNamedPipeServerProcessId = (getpipesrvpid_t)
            kitty_api("kernel32.dll", "GetNamedPipeServerProcessId",
                      KITTY_API_OPTIONAL,
                      KT_WINFEAT_AGENT_PIPE);
        pipepid_resolved = 1;
    }
    if (p_GetNamedPipeServerProcessId)
        return p_GetNamedPipeServerProcessId(pipe, pid);
    SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
}

/* Vista+: register for Restart-Manager relaunch after an MSI upgrade. On XP
 * there is no Restart Manager, so there is nothing to register with and
 * nothing is lost - silent, per the exact-fallback bar. */
typedef HRESULT (WINAPI *regapprestart_t)(PCWSTR, DWORD);
static regapprestart_t p_RegisterApplicationRestart;
static int appra_resolved;

HRESULT kitty_oldwin_RegisterApplicationRestart(PCWSTR cmdline, DWORD flags)
{
    if (!appra_resolved) {
        p_RegisterApplicationRestart = (regapprestart_t)
            kitty_api("kernel32.dll", "RegisterApplicationRestart",
                      KITTY_API_OPTIONAL,
                      KT_WINFEAT_RESTART);
        appra_resolved = 1;
    }
    if (p_RegisterApplicationRestart)
        return p_RegisterApplicationRestart(cmdline, flags);
    return E_NOTIMPL;
}
