/*
 * kitty_scriptbridge.c - the script bridge: another program on this desktop
 * drives the rutty script engine (kitty_rutty.c) of this terminal over
 * WM_COPYDATA, and at level "prompt" hears when the host's prompt appears.
 *
 * The successor of rutty's AutoHotkey interface, rebuilt on the engine's own
 * API instead of on its internals. Two levels are built:
 *
 *   [KiTTY] scriptbridge=no       off (the default)
 *   [KiTTY] scriptbridge=inbound  _send only: a script in, played like
 *                                 "Run script file" (yes = inbound)
 *   [KiTTY] scriptbridge=prompt   inbound, plus _enable / _set: one _prompt
 *                                 message to the bound window each time the
 *                                 set pattern ends the host's output
 *
 * Session output to another program (rutty's _received / _transmit) is NOT
 * built: the event carries the matched pattern only, never a byte of the
 * session.
 *
 * THE WIRE (dwData of WM_COPYDATA, ids beside the broadcast's 1-4):
 *   5  _send    in   "<key>\0<script>"   the script, at most 64 KiB in all
 *   6  _enable  in   "<key>\0" "1"       bind the window in wParam as the
 *                                        target of _prompt; "0" unbinds
 *   7  _set     in   "<key>\0<pattern>"  the prompt pattern, rutty's form
 *                                        (plain text, or "a" "b" for several);
 *                                        empty clears it
 *   8  _prompt  out  "<pattern>"         the alternative that matched, no NUL;
 *                                        wParam = this terminal's window
 *   9, 10            kept for _received / _transmit, refused here
 * <key> is the broadcast key of the session (its own BroadcastKey, else the
 * install's sendcmdgroup), so an installed copy and a portable one do not
 * drive each other by accident.
 *
 * THE GATE, in this order, one Terminal Window's Event Log line per refusal:
 * scriptmode, the bridge level, the size, the key, the session's arm
 * (AcceptScriptBridge, Session > Scripting or the Tools toggle), the level
 * rule (_enable and _set need "prompt").
 *
 * THE TARGET of _prompt is a window HANDLE, checked once at _enable: a
 * window, not this terminal, same user, integrity not below ours. Its process
 * id is kept and checked again before every message, so a new window that
 * inherits the handle value gets nothing. Nothing is ever looked up by title.
 * Messages are queued (256 KiB) and sent from a top-level callback with
 * SendMessageTimeout, 1000 ms: a target that does not answer in time is
 * unbound, the session goes on.
 *
 * Not a boundary against a program running as the same user (it could type
 * into the session by other means): like the broadcast, it stops accidents
 * and keeps the feature off unless it is switched on.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "security-api.h"   /* get_user_sid */
#include "kitty_text.h"     /* the Event Log wordings */
#include "kitty_rutty.h"    /* kitty_script_enabled, kitty_script_send_buffer */
#include "kitty_scriptbridge.h"
#include "kitty_inikeys.h"     /* KI_SECTION_KITTY, KI_SCRIPTBRIDGEKEY */
#include "kitty_params.h"      /* ReadParameterDup */
#include "kitty_oldwin.h"      /* kitty_module_path_dup */
#include "kitty_storage.h"     /* kitty_registry_base */
#include "kitty_random.h"      /* kitty_os_random */

#define SB_LINE_MAX     4096            /* the received line kept for matching */
#define SB_KEY_MAX      128             /* longest key accepted on the wire */

/* ---- the level ([KiTTY] scriptbridge) ----------------------------------- */

static int sb_level = KSB_LEVEL_NO;

int kitty_sbridge_level(void) { return sb_level; }

/* ---- the key ([KiTTY] scriptbridgekey) ---------------------------------
 * Its own key, not the broadcast's: a program allowed to drive the script
 * engine is not thereby allowed to type into every broadcast group, and
 * the reverse. Derived like the broadcast key (the same in every terminal
 * of an installation, also under readonly=yes), with a salt of its own so
 * the two never match. "New key" stores a random one. */

static char *sb_install_key = NULL;
static int sb_key_stored = 0;

void kitty_sbridge_set_install_key(const char *k)
{
    char *n = (k == NULL || k[0] == '\0') ? NULL : dupstr(k);
    sfree(sb_install_key);
    sb_install_key = n;
    sb_key_stored = n != NULL;
}

static void sb_fnv(unsigned long long *h, const char *p)
{
    for (; p && *p; p++) {                  /* case-insensitive */
        *h ^= (unsigned char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        *h *= 1099511628211ULL;
    }
}

const char *kitty_sbridge_install_key(void)
{
    unsigned long long h = 1469598103934665603ULL;     /* FNV-1a, 64-bit */
    char *buf, *exe;
    if (sb_install_key)
        return sb_install_key;
    buf = ReadParameterDup(KI_SECTION_KITTY, KI_SCRIPTBRIDGEKEY);
    if (buf[0]) {
        sb_install_key = buf;
        sb_key_stored = 1;
        return sb_install_key;
    }
    sfree(buf);
    sb_fnv(&h, "script bridge");
    exe = kitty_module_path_dup(NULL);
    sb_fnv(&h, exe);
    sfree(exe);
    sb_fnv(&h, kitty_registry_base());
    sb_install_key = dupprintf("sb-%016llx", h);
    return sb_install_key;
}

int kitty_sbridge_install_key_stored(void)
{
    (void)kitty_sbridge_install_key();
    return sb_key_stored;
}

/* NULL when Windows gives no random bytes: the caller reports it rather
 * than store a key someone could guess. */
char *kitty_sbridge_new_key_dup(void)
{
    unsigned char r[16];
    char *k;
    size_t i;
    if (!kitty_os_random(r, sizeof(r)))
        return NULL;
    k = snewn(3 + 2 * sizeof(r) + 1, char);
    strcpy(k, "sb-");
    for (i = 0; i < sizeof(r); i++)
        sprintf(k + 3 + 2 * i, "%02x", r[i]);
    smemclr(r, sizeof(r));
    return k;
}

void kitty_sbridge_set_level(int level)
{
    if (level < KSB_LEVEL_NO || level > KSB_LEVEL_PROMPT)
        level = KSB_LEVEL_NO;
    sb_level = level;
    if (sb_level < KSB_LEVEL_PROMPT)
        kitty_sbridge_unbind(KT_SBRIDGE_WHY_OFF);
}

/* The kitty.ini value. Anything not known (the level-c word "output"
 * included) is off: an unknown value never opens anything. */
void kitty_sbridge_set_level_text(const char *v)
{
    char w[16];
    size_t n;
    if (!v)
        v = "";
    while (*v == ' ' || *v == '\t')
        v++;
    n = strcspn(v, " \t\r\n");
    if (n >= sizeof(w))
        n = sizeof(w) - 1;                 /* longer than any value: off */
    memcpy(w, v, n);
    w[n] = '\0';
    if (!stricmp(w, "inbound") || !stricmp(w, "yes"))
        kitty_sbridge_set_level(KSB_LEVEL_INBOUND);
    else if (!stricmp(w, "prompt"))
        kitty_sbridge_set_level(KSB_LEVEL_PROMPT);
    else
        kitty_sbridge_set_level(KSB_LEVEL_NO);
}

static const char *sb_level_word(int level)
{
    return level == KSB_LEVEL_PROMPT ? "prompt" :
           level == KSB_LEVEL_INBOUND ? "inbound" : "no";
}

int kitty_sbridge_is_id(ULONG_PTR id)
{
    return id >= KSB_ID_SEND && id <= KSB_ID_LAST;
}

/* ---- the prompt pattern -------------------------------------------------- */

/* The alternatives of the pattern, each its own string. */
static char **sb_alt = NULL;
static size_t *sb_alt_len = NULL;
static int sb_nalt = 0;

static void sb_pattern_free(void)
{
    int i;
    for (i = 0; i < sb_nalt; i++)
        sfree(sb_alt[i]);
    sfree(sb_alt);
    sfree(sb_alt_len);
    sb_alt = NULL;
    sb_alt_len = NULL;
    sb_nalt = 0;
}

static void sb_pattern_add(const char *p, size_t n)
{
    if (n == 0)
        return;
    sb_alt = sresize(sb_alt, sb_nalt + 1, char *);
    sb_alt_len = sresize(sb_alt_len, sb_nalt + 1, size_t);
    sb_alt[sb_nalt] = snewn(n + 1, char);
    memcpy(sb_alt[sb_nalt], p, n);
    sb_alt[sb_nalt][n] = '\0';
    sb_alt_len[sb_nalt] = n;
    sb_nalt++;
}

/* rutty's condition form: plain text is one alternative; text that starts
 * with a quote is a list of quoted alternatives ("a" "b"). Trailing line
 * ends are dropped, as the engine drops them. */
static void sb_pattern_set(const char *in, size_t n)
{
    sb_pattern_free();
    while (n > 0 && (in[n - 1] == '\n' || in[n - 1] == '\r'))
        n--;
    if (n == 0)
        return;
    if (in[0] != '"') {
        sb_pattern_add(in, n);
        return;
    }
    {
        size_t i = 1;                      /* past the opening quote */
        while (i < n) {
            size_t start = i;
            while (i < n && in[i] != '"')
                i++;
            sb_pattern_add(in + start, i - start);
            i++;                           /* the closing quote */
            while (i < n && in[i] == ' ')  /* to the next alternative */
                i++;
            while (i < n && in[i] == '"')
                i++;
        }
    }
}

/* ---- the binding and its queue ------------------------------------------- */

struct sb_event {
    struct sb_event *next;
    size_t len;
    char data[1];                          /* len bytes follow */
};

static HWND sb_self = NULL;                /* this terminal's window */
static HWND sb_target = NULL;              /* the bound window, NULL = none */
static DWORD sb_target_pid = 0;
static LogContext *sb_logctx = NULL;       /* lives as long as the process */
static Conf *sb_conf = NULL;               /* the session's conf, for the arm */
static unsigned sb_gen = 0;                /* changes at every bind and unbind */
static unsigned sb_sent = 0;               /* _prompt messages sent this binding */
static bool sb_dropped_logged = false;
static struct sb_event *sb_head = NULL, *sb_tail = NULL;
static size_t sb_queued = 0;               /* bytes in the queue */
static bool sb_drain_queued = false;

static char *sb_line = NULL;               /* the received line, SB_LINE_MAX */
static size_t sb_line_len = 0;

static void sb_log(const char *text)
{
    if (sb_logctx)
        logevent(sb_logctx, text);
}

static void sb_queue_free(void)
{
    while (sb_head) {
        struct sb_event *e = sb_head;
        sb_head = e->next;
        sfree(e);
    }
    sb_tail = NULL;
    sb_queued = 0;
}

void kitty_sbridge_unbind(const char *why)
{
    char *m;
    if (!sb_target)
        return;
    m = dupprintf(KT_SBRIDGE_UNBOUND, why, sb_sent);
    sb_log(m);
    sfree(m);
    sb_target = NULL;
    sb_target_pid = 0;
    sb_sent = 0;
    sb_gen++;
    sb_queue_free();
    sb_line_len = 0;
}

int kitty_sbridge_bound(void)
{
    return sb_target != NULL && sb_nalt > 0;
}

/* Still allowed to send right now: the switches may have changed since the
 * bind (Change Settings, the Tools toggle). */
static bool sb_still_allowed(void)
{
    return kitty_script_enabled() && sb_level >= KSB_LEVEL_PROMPT &&
        sb_conf && conf_get_bool(sb_conf, CONF_kitty_accept_scriptbridge);
}

static bool sb_target_alive(void)
{
    DWORD pid = 0;
    if (!sb_target || !IsWindow(sb_target))
        return false;
    GetWindowThreadProcessId(sb_target, &pid);
    return pid != 0 && pid == sb_target_pid;
}

static void sb_drain(void *ctx)
{
    (void)ctx;
    sb_drain_queued = false;
    while (sb_head) {
        struct sb_event *e = sb_head;
        COPYDATASTRUCT cds;
        DWORD_PTR res = 0;
        unsigned gen = sb_gen;
        LRESULT ok;

        if (!sb_still_allowed()) {
            kitty_sbridge_unbind(KT_SBRIDGE_WHY_OFF);
            return;
        }
        if (!sb_target_alive()) {
            kitty_sbridge_unbind(KT_SBRIDGE_WHY_LOST);
            return;
        }
        /* Off the queue before the send: the target may answer with a
         * message of its own while this one waits (SMTO_NORMAL lets it in),
         * and that may unbind or bind again. */
        sb_head = e->next;
        if (!sb_head)
            sb_tail = NULL;
        sb_queued -= e->len;

        cds.dwData = KSB_ID_PROMPT;
        cds.cbData = (DWORD)e->len;
        cds.lpData = e->data;
        SetLastError(0);
        ok = SendMessageTimeoutA(sb_target, WM_COPYDATA, (WPARAM)sb_self,
                                 (LPARAM)&cds, SMTO_NORMAL | SMTO_ABORTIFHUNG,
                                 KSB_SEND_TIMEOUT_MS, &res);
        if (ok == 0) {
            DWORD err = GetLastError();
            sfree(e);
            if (gen != sb_gen)
                return;                    /* unbound meanwhile */
            kitty_sbridge_unbind(err == ERROR_TIMEOUT || err == 0 ?
                                 KT_SBRIDGE_WHY_TIMEOUT : KT_SBRIDGE_WHY_LOST);
            return;
        }
        sfree(e);
        if (gen != sb_gen)
            return;
        sb_sent++;
    }
}

static void sb_enqueue(const char *p, size_t n)
{
    struct sb_event *e;
    if (sb_queued + n > KSB_QUEUE_MAX) {
        if (!sb_dropped_logged) {
            sb_log(KT_SBRIDGE_QUEUE_FULL);
            sb_dropped_logged = true;
        }
        return;
    }
    e = (struct sb_event *)smalloc(sizeof(struct sb_event) + n);
    e->next = NULL;
    e->len = n;
    memcpy(e->data, p, n);
    if (sb_tail)
        sb_tail->next = e;
    else
        sb_head = e;
    sb_tail = e;
    sb_queued += n;
    if (!sb_drain_queued) {
        sb_drain_queued = true;
        queue_toplevel_callback(sb_drain, NULL);
    }
}

/* OBSERVE-only, from win_seat_output: the host's output is matched against
 * the pattern, the data itself goes on to the terminal untouched. The line
 * starts again at every line end, as the engine's does; after a match it
 * starts again too, so one prompt is one event. */
void kitty_sbridge_remote(const void *vdata, size_t len)
{
    const char *data = (const char *)vdata;
    size_t i;
    int a;

    if (!kitty_sbridge_bound())
        return;
    if (!sb_line)
        sb_line = snewn(SB_LINE_MAX, char);
    for (i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n' || c == '\r' || c == '\0') {
            sb_line_len = 0;
            continue;
        }
        if (sb_line_len >= SB_LINE_MAX) {
            /* keep the tail a pattern can still end in */
            size_t keep = KSB_PATTERN_MAX;
            memmove(sb_line, sb_line + sb_line_len - keep, keep);
            sb_line_len = keep;
        }
        sb_line[sb_line_len++] = c;
        for (a = 0; a < sb_nalt; a++) {
            size_t n = sb_alt_len[a];
            if (n <= sb_line_len &&
                !memcmp(sb_line + sb_line_len - n, sb_alt[a], n)) {
                sb_enqueue(sb_alt[a], n);
                sb_line_len = 0;
                break;
            }
        }
    }
}

/* ---- the bind checks ----------------------------------------------------- */

/* The integrity level of a token, or -1. */
static long sb_token_integrity(HANDLE tok)
{
    DWORD len = 0;
    TOKEN_MANDATORY_LABEL *ml;
    long rid = -1;
    if (!GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &len) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return -1;
    ml = (TOKEN_MANDATORY_LABEL *)smalloc(len ? len : 1);
    if (GetTokenInformation(tok, TokenIntegrityLevel, ml, len, &len)) {
        PSID sid = ml->Label.Sid;
        if (IsValidSid(sid) && *GetSidSubAuthorityCount(sid) > 0)
            rid = (long)*GetSidSubAuthority(sid,
                                            *GetSidSubAuthorityCount(sid) - 1);
    }
    sfree(ml);
    return rid;
}

/* 0 = same user, integrity not below ours; else the refusal text. */
static const char *sb_check_process(DWORD pid)
{
    HANDLE proc, tok = NULL, mytok = NULL;
    DWORD len = 0;
    TOKEN_USER *tu = NULL;
    PSID me = get_user_sid();
    const char *why = KT_SBRIDGE_TARGET_UNCHECKED;
    long theirs, ours;

    if (!me)
        return why;
    proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)                             /* Windows XP knows only this one */
        proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc)
        return why;
    if (!OpenProcessToken(proc, TOKEN_QUERY, &tok))
        goto done;
    if (!GetTokenInformation(tok, TokenUser, NULL, 0, &len) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        goto done;
    tu = (TOKEN_USER *)smalloc(len ? len : 1);
    if (!GetTokenInformation(tok, TokenUser, tu, len, &len))
        goto done;
    if (!EqualSid(tu->User.Sid, me)) {
        why = KT_SBRIDGE_TARGET_USER;
        goto done;
    }
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mytok))
        goto done;
    theirs = sb_token_integrity(tok);
    ours = sb_token_integrity(mytok);
    /* Neither known: a Windows without integrity levels (XP), nothing to
     * compare. One of them unknown: unchecked, refused. */
    if ((theirs < 0) != (ours < 0))
        goto done;
    if (theirs < ours) {
        why = KT_SBRIDGE_TARGET_INTEGRITY;
        goto done;
    }
    why = NULL;
  done:
    sfree(tu);
    if (mytok)
        CloseHandle(mytok);
    if (tok)
        CloseHandle(tok);
    CloseHandle(proc);
    return why;
}

static int sb_bind(HWND self, HWND target)
{
    DWORD pid = 0;
    const char *why;
    char *m;

    if (!target || !IsWindow(target)) {
        sb_log(KT_SBRIDGE_TARGET_NOWINDOW);
        return 0;
    }
    GetWindowThreadProcessId(target, &pid);
    if (target == self || pid == GetCurrentProcessId()) {
        sb_log(KT_SBRIDGE_TARGET_SELF);
        return 0;
    }
    why = pid ? sb_check_process(pid) : KT_SBRIDGE_TARGET_UNCHECKED;
    if (why) {
        m = dupprintf(why, (unsigned long)pid);
        sb_log(m);
        sfree(m);
        return 0;
    }
    if (sb_target)
        kitty_sbridge_unbind(KT_SBRIDGE_WHY_REBIND);
    sb_self = self;
    sb_target = target;
    sb_target_pid = pid;
    sb_sent = 0;
    sb_dropped_logged = false;
    sb_gen++;
    sb_line_len = 0;
    m = dupprintf(KT_SBRIDGE_BOUND, (unsigned long)(ULONG_PTR)target,
                  (unsigned long)pid);
    sb_log(m);
    sfree(m);
    return 1;
}

/* ---- the receiving side ------------------------------------------------- */

static void sb_refuse(LogContext *logctx, const char *fmt, const char *arg)
{
    char *m = arg ? dupprintf(fmt, arg) : dupstr(fmt);
    logevent(logctx, m);
    sfree(m);
}

/* A key is printable ASCII, no space, at most SB_KEY_MAX: anything else is
 * a malformed message, never a line of untrusted bytes in the log. */
static bool sb_key_ok(const char *k, size_t n)
{
    size_t i;
    if (n == 0 || n > SB_KEY_MAX)
        return false;
    for (i = 0; i < n; i++)
        if ((unsigned char)k[i] <= 0x20 || (unsigned char)k[i] >= 0x7f)
            return false;
    return true;
}

LRESULT kitty_sbridge_copydata(const struct kitty_sbridge_msg *in)
{
    const COPYDATASTRUCT *cds = in->cds;
    LogContext *logctx = in->logctx;
    const char *data, *nul, *rest;
    size_t n, keylen, restlen;
    char *key, *m;
    ULONG_PTR id;

    if (!cds)
        return 0;
    id = cds->dwData;

    /* 1. the engine's own master switch */
    if (!kitty_script_enabled()) {
        logevent(logctx, KT_SBRIDGE_REFUSED_SCRIPTMODE);
        return 0;
    }
    /* 2. the bridge's level */
    if (sb_level == KSB_LEVEL_NO) {
        logevent(logctx, KT_SBRIDGE_REFUSED_OFF);
        return 0;
    }
    /* 3. the size, before a byte of it is read: never cut */
    if (cds->cbData > KSB_INBOUND_MAX) {
        m = dupprintf(KT_SBRIDGE_REFUSED_SIZE, (unsigned long)cds->cbData,
                      KSB_INBOUND_MAX);
        logevent(logctx, m);
        sfree(m);
        return 0;
    }
    /* 4. the key, "<key>\0..." */
    n = cds->cbData;
    data = (const char *)cds->lpData;
    nul = (data && n) ? memchr(data, '\0', n) : NULL;
    keylen = nul ? (size_t)(nul - data) : 0;
    if (!nul || !sb_key_ok(data, keylen)) {
        sb_refuse(logctx, KT_SBRIDGE_REFUSED_MALFORMED, KT_SBRIDGE_WHY_NOKEY);
        return 0;
    }
    key = snewn(keylen + 1, char);
    memcpy(key, data, keylen);
    key[keylen] = '\0';
    if (!in->group || strcmp(key, in->group) != 0) {
        m = dupprintf(KT_SBRIDGE_REFUSED_KEY, key, in->group ? in->group : "");
        logevent(logctx, m);
        sfree(m);
        sfree(key);
        return 0;
    }
    sfree(key);
    rest = nul + 1;
    restlen = n - keylen - 1;

    /* 5. this session's arm */
    if (!in->armed) {
        logevent(logctx, KT_SBRIDGE_REFUSED_SESSION);
        return 0;
    }
    /* 6. the id and the level rule */
    if (id != KSB_ID_SEND && id != KSB_ID_ENABLE && id != KSB_ID_SET) {
        m = dupprintf(KT_SBRIDGE_REFUSED_ID, (unsigned long)id);
        logevent(logctx, m);
        sfree(m);
        return 0;
    }
    if (id != KSB_ID_SEND && sb_level < KSB_LEVEL_PROMPT) {
        m = dupprintf(KT_SBRIDGE_REFUSED_LEVEL,
                      id == KSB_ID_ENABLE ? "_enable" : "_set",
                      sb_level_word(sb_level));
        logevent(logctx, m);
        sfree(m);
        return 0;
    }

    if (id == KSB_ID_SEND) {
        if (restlen == 0) {
            sb_refuse(logctx, KT_SBRIDGE_REFUSED_MALFORMED, KT_SBRIDGE_WHY_EMPTY);
            return 0;
        }
        if (!in->backend) {
            logevent(logctx, KT_SBRIDGE_REFUSED_NOSESSION);
            return 0;
        }
        /* the engine writes its own refusal (busy) */
        if (!kitty_script_send_buffer(in->conf, in->backend, rest, restlen,
                                      logctx, in->self))
            return 0;
        m = dupprintf(KT_SBRIDGE_ACCEPTED, (int)restlen);
        logevent(logctx, m);
        sfree(m);
        return 1;
    }

    sb_logctx = logctx;
    sb_conf = in->conf;
    if (id == KSB_ID_ENABLE) {
        if (restlen == 1 && rest[0] == '1')
            return sb_bind(in->self, in->sender);
        if (restlen == 1 && rest[0] == '0') {
            if (sb_target)
                kitty_sbridge_unbind(KT_SBRIDGE_WHY_DISABLE);
            return 1;
        }
        sb_refuse(logctx, KT_SBRIDGE_REFUSED_MALFORMED, KT_SBRIDGE_WHY_FLAG);
        return 0;
    }

    /* KSB_ID_SET */
    if (restlen > KSB_PATTERN_MAX) {
        sb_refuse(logctx, KT_SBRIDGE_REFUSED_MALFORMED, KT_SBRIDGE_WHY_PATTERN);
        return 0;
    }
    sb_pattern_set(rest, restlen);
    sb_line_len = 0;
    if (sb_nalt == 0) {
        logevent(logctx, KT_SBRIDGE_PATTERN_CLEARED);
    } else {
        m = dupprintf(KT_SBRIDGE_PATTERN_SET, sb_nalt);
        logevent(logctx, m);
        sfree(m);
    }
    return 1;
}
