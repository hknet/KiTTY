/*
 * kitty_pwmem.h - the password fields of a RUNNING configuration, held
 * encrypted in memory and unwrapped only into a caller's buffer at the
 * instant they are used.
 *
 * A Conf keeps its strings as plain heap allocations for the lifetime of the
 * window, so a stored - or typed - login sat in the process image, in the
 * pagefile and in every crash dump for as long as the session was open. These
 * accessors keep the value wrapped instead:
 *
 *   {kpw1}<base64>    CryptProtectMemory(SAME_PROCESS) - what a Conf holds
 *   {kpwl1}<base64>   CryptProtectMemory(SAME_LOGON) - what travels in the
 *                     file mapping a new window inherits, because the reader
 *                     is a different process
 *
 * The one-line password FILE handed to kscp/ksftp/klink carries no format of
 * its own: it holds the ordinary AT-REST secret form ("DPAPI1:" + base64 of a
 * DPAPI blob for this user), which every KiTTY helper has read for as long as
 * stored passwords have been protected at rest. Its two functions are declared
 * here with the rest of the password handling and IMPLEMENTED in
 * kitty/kitty_secretstore.c, where that crypto lives.
 *
 * The protected block is 4 bytes of length (little endian) followed by the
 * password bytes, zero padded to a multiple of the 16-byte block the API
 * requires. Keeping the length inside the block rather than in the prefix
 * means the stored form says nothing about how long the password is.
 *
 * An EMPTY password is stored as the empty string and never wrapped, so
 * kitty_pw_empty() answers without decrypting anything - the many
 * "is there a password at all" tests cost nothing.
 *
 * A value with no prefix is legacy plaintext: the getters return it as it is,
 * and kitty_pw_seal_all() wraps it the first time the settings are loaded.
 *
 * What this does NOT do (stated so nobody expects more): it cannot protect a
 * buffer that is currently unwrapped, which is the case for the milliseconds
 * around a connect or a helper-tool start, and it cannot protect anything from
 * code running inside this process. It protects the RESTING state - dumps,
 * crash reports, swap, a stale copy of a Conf.
 *
 * Where the crypt API is missing (Windows before Vista) or a protect/unprotect
 * round trip fails, the password is kept in the clear exactly as before and the
 * session prints one line naming the feature this Windows does not support -
 * the same rule the agent's key protection follows. A password is never
 * refused or dropped because the protection is unavailable.
 *
 * Windows-only in substance; the non-Windows build gets pass-through inlines so
 * that the shared files calling these (settings.c, cmdline.c, proxy/*.c) keep
 * compiling.
 */
#ifndef KITTY_PWMEM_H
#define KITTY_PWMEM_H

/* Longest password these accessors will carry. Callers size their stack
 * buffers with this. */
#define KITTY_PW_MAX 4096

#ifdef _WINDOWS

/*
 * Unwrap a STORED string - one of the forms above, or legacy plaintext - into
 * the caller's buffer. Returns the length written, 0 for an empty or
 * unreadable value (out is then an empty string). Never returns a pointer into
 * anything; the caller smemclr()s the buffer when it is done.
 *
 * A password that does not fit in outlen is NOT truncated: the call answers 0
 * and empties the buffer, because a truncated password is a failed login with
 * no explanation.
 */
size_t kitty_pw_unwrap_str(const char *stored, char *out, size_t outlen);

/* The same for a password key of a Conf. */
size_t kitty_pw_get(Conf *conf, int key, char *out, size_t outlen);

/* Does this stored string carry one of our in-memory markers? For the few
 * callers that must tell "wrapped" from "legacy plaintext" rather than simply
 * read the value - the settings store, which has to know whether an unwrap
 * failing means a lost password or a value that was never wrapped. */
int kitty_pw_is_wrapped(const char *stored);

/* Is this password key empty? True also when the value cannot be unwrapped. */
int kitty_pw_empty(Conf *conf, int key);

/*
 * Wrap `plain` and store it under `key`. The empty string (and NULL) clears
 * the key.
 *
 * kitty_pw_set does NOT touch the caller's string - it takes a const, and a
 * caller passing a literal or a reference it does not own must not have it
 * wiped underneath. kitty_pw_set_burn is for the usual case, where `plain` is
 * the caller's own buffer: it smemclr()s it before returning (it does not free
 * it - the caller's allocation stays the caller's).
 */
void kitty_pw_set(Conf *conf, int key, const char *plain);
void kitty_pw_set_burn(Conf *conf, int key, char *plain);

/*
 * Wrap a bare string. Returns a fresh "{kpw1}..." / "{kpwl1}..." string the
 * caller frees, or a plain copy when the protection is unavailable. NULL only
 * for a NULL or empty input.
 */
char *kitty_pw_wrap_str(const char *plain);
char *kitty_pw_wrap_logon_str(const char *plain);

/*
 * Bring every password key of `conf` into the {kpw1} form: legacy plaintext is
 * wrapped, a {kpwl1} value handed over by the window that started this one is
 * unwrapped and re-wrapped for this process, an already-wrapped value is left
 * alone. Run once after a settings load and once after a Conf arrives from
 * another process.
 *
 * It also probes the protection, so that a Windows lacking it is reported even
 * in a session that has no password to seal.
 */
void kitty_pw_seal_all(Conf *conf);

/*
 * The reverse, for a Conf about to be serialised into the file mapping a child
 * process inherits: the password keys travel wrapped for the LOGON, since the
 * reader is another process. Call it on a copy, never on the live Conf.
 */
void kitty_pw_seal_for_handoff(Conf *conf);

/*
 * Wipe the password keys of a Conf that is about to be freed.
 *
 * conf_free() releases its strings without clearing them, so the wrapped blob -
 * and, where the protection is unavailable, the password itself - would be left
 * in freed heap for whatever allocates that memory next. Call this immediately
 * before conf_free(): on a window's Conf at teardown, and on the copies made
 * for a hand-off once they have been serialised.
 */
void kitty_pw_wipe(Conf *conf);

/*
 * The single line of the password file handed to kscp/ksftp/klink: the at-rest
 * secret form, "DPAPI1:" + base64. Returns a fresh string the caller frees;
 * NULL only for an empty password. Where the protection fails it answers the
 * password itself - the plain format -pwfile has always taken - and the
 * missing-features line names what was unavailable.
 *
 * Implemented in kitty/kitty_secretstore.c (the `settings` library, which every
 * helper links), because it is the at-rest protection and nothing new.
 */
char *kitty_pwfile_line(const char *plain);

/*
 * Did that call actually protect the value, or hand the password back as it
 * was? It decides what may be SAID about the hand-over, and whether the value
 * is one that may go on a command line at all.
 */
int kitty_pwfile_line_is_protected(const char *line);

/*
 * The reader side, for -pwfile and -pw. Both run the value through the same
 * at-rest reader a stored session password goes through, so a value with no
 * marker is the plain password and comes back as it stands, and a marked one
 * is unprotected. NULL out for a marked value that cannot be read here.
 *
 *   _copy  leaves the input alone - it is an argument string (-pw), and the
 *          caller owns it. Answers a fresh string the caller frees.
 *   plain  TAKES OWNERSHIP of `line`, burning and freeing it whatever the
 *          answer - for a line just read off a file (-pwfile).
 *
 * Both implemented in kitty/kitty_secretstore.c, beside kitty_pwfile_line.
 */
char *kitty_pwfile_decode_copy(const char *value);
char *kitty_pwfile_decode(char *line);

#else /* !_WINDOWS */

/* No CryptProtectMemory outside Windows: the shared files that call these
 * still have to compile, so every password is its own plain value here. */
#include <string.h>
static inline size_t kitty_pw_unwrap_str(const char *stored, char *out,
                                         size_t outlen)
{
    size_t len = stored ? strlen(stored) : 0;
    if (!out || outlen == 0) return 0;
    if (len == 0 || len >= outlen) { out[0] = '\0'; return 0; }
    memcpy(out, stored, len + 1);
    return len;
}
static inline size_t kitty_pw_get(Conf *conf, int key, char *out, size_t outlen)
{ return kitty_pw_unwrap_str(conf_get_str(conf, key), out, outlen); }
static inline int kitty_pw_empty(Conf *conf, int key)
{ return conf_get_str(conf, key)[0] == '\0'; }
static inline int kitty_pw_is_wrapped(const char *stored) { (void)stored; return 0; }
static inline void kitty_pw_set(Conf *conf, int key, const char *plain)
{ conf_set_str(conf, key, plain ? plain : ""); }
static inline void kitty_pw_set_burn(Conf *conf, int key, char *plain)
{ conf_set_str(conf, key, plain ? plain : "");
  if (plain) smemclr(plain, strlen(plain)); }
static inline char *kitty_pw_wrap_str(const char *plain)
{ return (plain && *plain) ? dupstr(plain) : NULL; }
static inline char *kitty_pw_wrap_logon_str(const char *plain)
{ return (plain && *plain) ? dupstr(plain) : NULL; }
static inline void kitty_pw_seal_all(Conf *conf) { (void)conf; }
static inline void kitty_pw_seal_for_handoff(Conf *conf) { (void)conf; }
static inline void kitty_pw_wipe(Conf *conf)
{ if (conf) { char *v = (char *)conf_get_str(conf, CONF_password);
              if (v && *v) smemclr(v, strlen(v));
              conf_set_str(conf, CONF_password, "");
              v = (char *)conf_get_str(conf, CONF_proxy_password);
              if (v && *v) smemclr(v, strlen(v));
              conf_set_str(conf, CONF_proxy_password, ""); } }
static inline char *kitty_pwfile_line(const char *plain)
{ return (plain && *plain) ? dupstr(plain) : NULL; }
static inline int kitty_pwfile_line_is_protected(const char *line)
{ (void)line; return 0; }
static inline char *kitty_pwfile_decode_copy(const char *value)
{ return value ? dupstr(value) : NULL; }
static inline char *kitty_pwfile_decode(char *line) { return line; }

#endif /* _WINDOWS */

#endif /* KITTY_PWMEM_H */
