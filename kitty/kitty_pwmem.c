/*
 * kitty_pwmem.c - see kitty_pwmem.h.
 *
 * In the `utils` library because EVERY binary links that: the password keys
 * are read from shared files (settings.c, cmdline.c, proxy/*.c,
 * utils/format_connection_setup_command.c) which are compiled into libraries
 * built WITHOUT MOD_PERSO, so those files call these functions directly rather
 * than through a guard that the preprocessor would delete.
 *
 * CryptProtectMemory is resolved at RUNTIME - a statically imported symbol the
 * running Windows does not export makes the loader refuse the whole binary -
 * and resolved HERE, with kitty_oldwin.c's loader, rather than through
 * got_crypt() in windows/utils/cryptoapi.c. That detour cost a link: this file
 * is pulled into every binary that has a Conf, referencing got_crypt() pulls
 * cryptoapi.c.obj in beside it, and that object also carries
 * capi_obfuscate_string(), which needs ssh_sha256 from the crypto library -
 * which psocks does not link at all, and which comes before `utils` in the
 * single-pass link everywhere else. kitty_renameguard.c, also in `utils` and
 * also linked everywhere, resolves its APIs the same way for the same reason.
 *
 * Nothing here touches DPAPI or the settings store. The one-line password file
 * handed to kscp/ksftp/klink is declared in kitty_pwmem.h but IMPLEMENTED in
 * kitty/kitty_secretstore.c, because it writes the ordinary at-rest secret form
 * and that crypto lives there, in the `settings` library - which comes BEFORE
 * `utils` in the single-pass static link, so the call could not go the other
 * way round.
 *
 * The base64 codec here is private for the same reason: kitty_b64.c is in
 * `settings` too, so a reference to it from here would not resolve.
 */
#include "putty.h"
#include <windows.h>
#include "kitty_oldwin.h"
#include "kitty_text.h"
#include "kitty_pwmem.h"

#ifndef CRYPTPROTECTMEMORY_BLOCK_SIZE
#define CRYPTPROTECTMEMORY_BLOCK_SIZE 16
#endif
#ifndef CRYPTPROTECTMEMORY_SAME_PROCESS
#define CRYPTPROTECTMEMORY_SAME_PROCESS 0x00
#endif
#ifndef CRYPTPROTECTMEMORY_SAME_LOGON
#define CRYPTPROTECTMEMORY_SAME_LOGON 0x02
#endif

#define KPW_MARK_PROC   "{kpw1}"
#define KPW_MARK_LOGON  "{kpwl1}"

/* The password keys of a Conf. Named proxies have no separate in-memory home:
 * LoadProxyInfo() puts the definition it reads into CONF_proxy_password, so
 * that key covers them too. */
static const int kpw_keys[] = { CONF_password, CONF_proxy_password };
#define KPW_NKEYS (int)(sizeof(kpw_keys) / sizeof(kpw_keys[0]))

/* ------------------------------------------------------------- base64 ---- */

static const char kpw_b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *kpw_b64_encode(const unsigned char *in, size_t len)
{
    size_t i, o = 0;
    char *out = snewn(((len + 2) / 3) * 4 + 1, char);

    for (i = 0; i < len; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        int n = 1;
        if (i + 1 < len) { v |= (unsigned)in[i + 1] << 8; n++; }
        if (i + 2 < len) { v |= (unsigned)in[i + 2];      n++; }
        out[o++] = kpw_b64_alphabet[(v >> 18) & 0x3F];
        out[o++] = kpw_b64_alphabet[(v >> 12) & 0x3F];
        out[o++] = n > 1 ? kpw_b64_alphabet[(v >> 6) & 0x3F] : '=';
        out[o++] = n > 2 ? kpw_b64_alphabet[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}

static int kpw_b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* NULL on anything malformed - a corrupted value must not decode to a
 * plausible-looking short password. */
static unsigned char *kpw_b64_decode(const char *in, size_t *outlen)
{
    size_t len = strlen(in), i, o = 0;
    unsigned char *out;

    *outlen = 0;
    if (len == 0 || (len % 4) != 0)
        return NULL;
    out = snewn(len / 4 * 3, unsigned char);
    for (i = 0; i < len; i += 4) {
        int q[4], j, pad = 0;
        unsigned v;
        for (j = 0; j < 4; j++) {
            if (in[i + j] == '=') {
                /* Padding is only legal in the last group, and only as the
                 * last one or two characters. */
                if (i + 4 != len || j < 2) { sfree(out); return NULL; }
                pad++;
                q[j] = 0;
            } else if (pad) {
                sfree(out); return NULL;      /* data after padding */
            } else if ((q[j] = kpw_b64_val((unsigned char)in[i + j])) < 0) {
                sfree(out); return NULL;
            }
        }
        v = ((unsigned)q[0] << 18) | ((unsigned)q[1] << 12) |
            ((unsigned)q[2] << 6) | (unsigned)q[3];
        out[o++] = (unsigned char)((v >> 16) & 0xFF);
        if (pad < 2) out[o++] = (unsigned char)((v >> 8) & 0xFF);
        if (pad < 1) out[o++] = (unsigned char)(v & 0xFF);
    }
    *outlen = o;
    return out;
}

/* --------------------------------------------------- the protection ------ */

typedef BOOL (WINAPI *kpw_memfn)(LPVOID, DWORD, DWORD);

/* Resolve the pair once. GetModuleHandle first, so an already-loaded crypt32
 * never becomes a reason to LOAD one - that is how a search-path hijack gets
 * in - and the module handle is kept for the life of the process, as every
 * other runtime lookup in the tree keeps its own. Answers non-zero when both
 * are there; kitty_api_from() has already RECORDED an absent one as a missing
 * feature by then. */
static int kpw_resolve(kpw_memfn *protect, kpw_memfn *unprotect)
{
    static int tried = 0;
    static kpw_memfn fn_protect = NULL, fn_unprotect = NULL;

    if (!tried) {
        HMODULE m;
        tried = 1;
        m = GetModuleHandleA("crypt32.dll");
        if (!m)
            m = LoadLibraryA("crypt32.dll");
        if (m) {
            fn_protect = (kpw_memfn)kitty_api_from(
                m, "crypt32.dll", "CryptProtectMemory",
                KITTY_API_OPTIONAL, KT_WINFEAT_PASSWORD_IN_MEMORY);
            fn_unprotect = (kpw_memfn)kitty_api_from(
                m, "crypt32.dll", "CryptUnprotectMemory",
                KITTY_API_OPTIONAL, KT_WINFEAT_PASSWORD_IN_MEMORY);
        } else {
            kitty_api_record("crypt32.dll", "CryptProtectMemory",
                             KITTY_API_OPTIONAL,
                             KT_WINFEAT_PASSWORD_IN_MEMORY, 0);
        }
    }
    *protect = fn_protect;
    *unprotect = fn_unprotect;
    return fn_protect != NULL && fn_unprotect != NULL;
}

static int kpw_mem(void *p, size_t n, int unprotect, DWORD flags)
{
    kpw_memfn fn_protect, fn_unprotect;

    if (!kpw_resolve(&fn_protect, &fn_unprotect))
        return 0;
    if (unprotect)
        return fn_unprotect(p, (DWORD)n, flags) ? 1 : 0;
    return fn_protect(p, (DWORD)n, flags) ? 1 : 0;
}

/*
 * Is the protection actually WORKING here? A real round trip, not just a
 * successful lookup: the DLL loading proves nothing about the calls
 * succeeding, and the whole point of asking is to tell the user when their
 * passwords are not protected. Probed once and remembered; the failure is
 * recorded as a missing feature, so the session prints one line about it.
 */
static int kpw_available(void)
{
    static int cached = -1;

    if (cached < 0) {
        unsigned char buf[CRYPTPROTECTMEMORY_BLOCK_SIZE];
        unsigned char ref[sizeof(buf)];
        memset(buf, 0xA5, sizeof(buf));
        memcpy(ref, buf, sizeof(buf));
        cached = kpw_mem(buf, sizeof(buf), 0, CRYPTPROTECTMEMORY_SAME_PROCESS) &&
                 kpw_mem(buf, sizeof(buf), 1, CRYPTPROTECTMEMORY_SAME_PROCESS) &&
                 memcmp(buf, ref, sizeof(buf)) == 0;
        smemclr(buf, sizeof(buf));
        if (!cached) {
            kpw_memfn fn_protect, fn_unprotect;
            /* Recorded here only when the API IS there and the round trip
             * still failed - something is interfering on a Windows that has
             * it. An absent one was already recorded by the lookup, and
             * recording it twice would list it twice in the full report. */
            if (kpw_resolve(&fn_protect, &fn_unprotect))
                kitty_api_record("crypt32.dll", "CryptProtectMemory",
                                 KITTY_API_OPTIONAL,
                                 KT_WINFEAT_PASSWORD_IN_MEMORY, 0);
        }
    }
    return cached;
}

/* Wrap `plain` under `mark`. Returns a fresh string the caller frees: the
 * marked form, or - when the protection is unavailable or the value is too
 * long to carry - a plain copy, which every reader below still accepts. */
static char *kpw_wrap(const char *plain, DWORD flags, const char *mark)
{
    size_t len, blocklen;
    unsigned char *blk;
    char *b64, *out;

    if (!plain || !*plain)
        return NULL;
    len = strlen(plain);
    if (len > KITTY_PW_MAX || !kpw_available())
        return dupstr(plain);

    /* 4 bytes of length, the password, then zero padding to the block size.
     * The length lives INSIDE the protected block so the stored form says
     * nothing about how long the password is. */
    blocklen = ((4 + len + CRYPTPROTECTMEMORY_BLOCK_SIZE - 1) /
                CRYPTPROTECTMEMORY_BLOCK_SIZE) * CRYPTPROTECTMEMORY_BLOCK_SIZE;
    blk = snewn(blocklen, unsigned char);
    memset(blk, 0, blocklen);
    blk[0] = (unsigned char)(len & 0xFF);
    blk[1] = (unsigned char)((len >> 8) & 0xFF);
    blk[2] = (unsigned char)((len >> 16) & 0xFF);
    blk[3] = (unsigned char)((len >> 24) & 0xFF);
    memcpy(blk + 4, plain, len);

    if (!kpw_mem(blk, blocklen, 0, flags)) {
        smemclr(blk, blocklen);
        sfree(blk);
        return dupstr(plain);
    }
    b64 = kpw_b64_encode(blk, blocklen);
    smemclr(blk, blocklen);
    sfree(blk);
    out = dupprintf("%s%s", mark, b64);
    smemclr(b64, strlen(b64));
    sfree(b64);
    return out;
}

size_t kitty_pw_unwrap_str(const char *stored, char *out, size_t outlen)
{
    const char *body;
    DWORD flags;
    unsigned char *blk;
    size_t blocklen = 0, len;

    if (!out || outlen == 0)
        return 0;
    out[0] = '\0';
    if (!stored || !*stored)
        return 0;

    if (!strncmp(stored, KPW_MARK_PROC, strlen(KPW_MARK_PROC))) {
        body = stored + strlen(KPW_MARK_PROC);
        flags = CRYPTPROTECTMEMORY_SAME_PROCESS;
    } else if (!strncmp(stored, KPW_MARK_LOGON, strlen(KPW_MARK_LOGON))) {
        body = stored + strlen(KPW_MARK_LOGON);
        flags = CRYPTPROTECTMEMORY_SAME_LOGON;
    } else {
        /* Legacy plaintext: a value written before this existed, or one kept
         * in the clear because the protection is unavailable. */
        len = strlen(stored);
        if (len >= outlen)
            return 0;
        memcpy(out, stored, len + 1);
        return len;
    }

    blk = kpw_b64_decode(body, &blocklen);
    if (!blk)
        return 0;
    if (blocklen < CRYPTPROTECTMEMORY_BLOCK_SIZE ||
        (blocklen % CRYPTPROTECTMEMORY_BLOCK_SIZE) != 0 ||
        !kpw_mem(blk, blocklen, 1, flags)) {
        smemclr(blk, blocklen);
        sfree(blk);
        return 0;
    }
    len = (size_t)blk[0] | ((size_t)blk[1] << 8) |
          ((size_t)blk[2] << 16) | ((size_t)blk[3] << 24);
    /* A password that does not fit is NOT truncated: a short password is a
     * failed login that looks like a wrong one. */
    if (len == 0 || len > blocklen - 4 || len >= outlen) {
        smemclr(blk, blocklen);
        sfree(blk);
        return 0;
    }
    memcpy(out, blk + 4, len);
    out[len] = '\0';
    smemclr(blk, blocklen);
    sfree(blk);
    return len;
}

int kitty_pw_is_wrapped(const char *stored)
{
    if (!stored)
        return 0;
    return !strncmp(stored, KPW_MARK_PROC, strlen(KPW_MARK_PROC)) ||
           !strncmp(stored, KPW_MARK_LOGON, strlen(KPW_MARK_LOGON));
}

size_t kitty_pw_get(Conf *conf, int key, char *out, size_t outlen)
{
    if (!conf) {
        if (out && outlen) out[0] = '\0';
        return 0;
    }
    return kitty_pw_unwrap_str(conf_get_str(conf, key), out, outlen);
}

int kitty_pw_empty(Conf *conf, int key)
{
    const char *v;
    if (!conf)
        return 1;
    /* An empty password is stored as the empty string and never wrapped, so
     * this answers without decrypting anything. */
    v = conf_get_str(conf, key);
    return (!v || !*v);
}

char *kitty_pw_wrap_str(const char *plain)
{
    return kpw_wrap(plain, CRYPTPROTECTMEMORY_SAME_PROCESS, KPW_MARK_PROC);
}

char *kitty_pw_wrap_logon_str(const char *plain)
{
    return kpw_wrap(plain, CRYPTPROTECTMEMORY_SAME_LOGON, KPW_MARK_LOGON);
}

void kitty_pw_set(Conf *conf, int key, const char *plain)
{
    char *blob;

    if (!conf)
        return;
    if (!plain || !*plain) {
        conf_set_str(conf, key, "");
        return;
    }
    if (kitty_pw_is_wrapped(plain)) {
        /* Already one of ours: a caller that read a password key and is
         * putting it into another Conf. Wrapping it a second time would make
         * a blob whose unwrap yields the first blob, and the reader would
         * stop there - a login that fails with nothing to see. The markers
         * are reserved, so taking the value as it stands is exact. */
        conf_set_str(conf, key, plain);
        return;
    }
    blob = kpw_wrap(plain, CRYPTPROTECTMEMORY_SAME_PROCESS, KPW_MARK_PROC);
    conf_set_str(conf, key, blob ? blob : plain);
    if (blob) {
        smemclr(blob, strlen(blob));
        sfree(blob);
    }
}

void kitty_pw_set_burn(Conf *conf, int key, char *plain)
{
    kitty_pw_set(conf, key, plain);
    if (plain)
        smemclr(plain, strlen(plain));
}

void kitty_pw_seal_all(Conf *conf)
{
    int i;

    /* Probe unconditionally, even with nothing to seal: the missing-features
     * line is printed once, at the start of the session, and a session whose
     * password is typed rather than stored would otherwise probe too late to
     * appear in it. */
    kpw_available();
    if (!conf)
        return;

    for (i = 0; i < KPW_NKEYS; i++) {
        const char *v = conf_get_str(conf, kpw_keys[i]);
        char buf[KITTY_PW_MAX + 1];
        size_t n;
        int marked;

        if (!v || !*v)
            continue;
        if (!strncmp(v, KPW_MARK_PROC, strlen(KPW_MARK_PROC)))
            continue;                        /* already ours */
        marked = !strncmp(v, KPW_MARK_LOGON, strlen(KPW_MARK_LOGON));

        n = kitty_pw_unwrap_str(v, buf, sizeof(buf));
        if (n > 0)
            kitty_pw_set(conf, kpw_keys[i], buf);
        else if (marked)
            conf_set_str(conf, kpw_keys[i], "");   /* unreadable here */
        /* else: plaintext we could not copy (longer than we carry) - leave it
         * exactly as it is rather than destroy a password. */
        smemclr(buf, sizeof(buf));
    }
}

void kitty_pw_wipe(Conf *conf)
{
    int i;

    if (!conf)
        return;
    for (i = 0; i < KPW_NKEYS; i++) {
        /* The cast is to a string the Conf OWNS and is about to release, and
         * the value is replaced in the same breath: conf_set_str frees the
         * (now zeroed) allocation and stores a fresh empty one. Clearing the
         * bytes in place is the only way to reach them - conf_free() does not
         * clear what it frees, and conf.c is upstream code this fork leaves
         * alone. */
        char *v = (char *)conf_get_str(conf, kpw_keys[i]);
        if (v && *v)
            smemclr(v, strlen(v));
        conf_set_str(conf, kpw_keys[i], "");
    }
}

void kitty_pw_seal_for_handoff(Conf *conf)
{
    int i;

    if (!conf)
        return;
    for (i = 0; i < KPW_NKEYS; i++) {
        const char *v = conf_get_str(conf, kpw_keys[i]);
        char buf[KITTY_PW_MAX + 1], *blob;
        size_t n;

        if (!v || !*v)
            continue;
        n = kitty_pw_unwrap_str(v, buf, sizeof(buf));
        if (n > 0) {
            blob = kpw_wrap(buf, CRYPTPROTECTMEMORY_SAME_LOGON, KPW_MARK_LOGON);
            conf_set_str(conf, kpw_keys[i], blob ? blob : buf);
            if (blob) {
                smemclr(blob, strlen(blob));
                sfree(blob);
            }
        }
        smemclr(buf, sizeof(buf));
    }
}

/* The two -pwfile helpers declared in kitty_pwmem.h are implemented in
 * kitty/kitty_secretstore.c: they write and read the ordinary at-rest secret form,
 * whose crypto is there. */
