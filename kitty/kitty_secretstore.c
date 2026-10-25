/*
 * kitty_secretstore.c - the at-rest credential crypto of the settings store:
 * the DPAPI1 wrap for the registry hive and the MPW2 (master password) wrap
 * for portable session files, the master-password unlock state with its
 * per-store salt and verifier, the cross-process unlock hand-over, the
 * export-bundle passphrase, the password file handed to kscp/ksftp/klink,
 * the retirement of an orphan master password, the .ktx forced-export glue
 * and the legacy (<=0.76 old-KiTTY) password decrypt.
 * Split from kitty_storage.c, which keeps the store state (registry root,
 * fallback hives, portable file backend); what the two share is declared
 * in kitty_storage_int.h.
 *
 * Like kitty_storage.c, this compiles into the settings lib AND standalone
 * into puttygen/kittygen_cli (windows/CMakeLists.txt): keep it free of
 * MOD_PERSO-style guards and GUI dependencies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <assert.h>
#include "putty.h"
#include "storage.h"
#include "kitty_defs.h"   /* KITTY_DEFAULT_SESSION (dependency-free) */
#include "kitty_b64.h"    /* ksec_b64_encode/decode (at-rest secret codec) */
#include "kitty_storage.h"
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_inifile.h"     /* kitty_store_readonly: readonly=yes */
#include "kitty_oldwin.h"   /* kitty_api_record: what this Windows cannot do */
#include "kitty_text.h"     /* KT_WINFEAT_*: the feature names that report lists */
#include "kitty_pwmem.h"    /* the -pwfile helpers declared there, defined here */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty.h"
#include "kitty_secretstore.h"
#include "kitty_storage_int.h"

/* ===================================================================== *
 * KiTTY 0.84.1.38+: encrypt credential fields at rest (Windows DPAPI).
 *
 * write_setting_s/read_setting_s is the generic registry/ini session save+load
 * chokepoint (the conf SAVE_KEYWORD path used by GUI Save/Load AND the CLI tools
 * klink/kscp/ksftp), so hooking here protects "Password"/"ProxyPassword"
 * everywhere while leaving the portable .ktx forced-file export on its own
 * legacy format. Runtime conf stays PLAINTEXT; only the stored form changes.
 * Write protection is BACKEND-scoped:
 * registry hive -> DPAPI1 always; portable session files -> MPW1 (master
 * password) or the explicit-compat escape hatch. Reads dispatch on the stored
 * marker regardless of backend.
 *
 * Self-contained (DPAPI + base64, no bcrypt) so it links into every tool that
 * uses libsettings. The "legacy"/unmarked stored value is returned VERBATIM:
 * a registry session password was always stored plaintext (the generic save
 * wrote conf verbatim), so existing sessions behave exactly as before and only
 * new saves convert to DPAPI1:.
 * ===================================================================== */
#include <wincrypt.h>
#include "kitty_winerr.h"   /* kitty_win_reason: Windows' reason, for the reports */
#include "kitty_buildlabel.h"   /* KITTY_TEST_BUILD_LABEL: the bundle-wrap test door */


int kitty_secret_slot(const char *key)
{
    if (!key) return -1;
    if (!strcmp(key, KR_PASSWORD)) return 0;
    if (!strcmp(key, "ProxyPassword")) return 1;
    return -1;
}

static char *ksec_dup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }

/* Base64 codec (ksec_b64_encode / ksec_b64_val / ksec_b64_decode) moved to
 * kitty/kitty_b64.c to shrink this file's divergence from upstream. */

/* Portable-context write policy (kitty.ini [KiTTY] PortablePasswordProtection):
 * "master" (default) -> portable secrets are written MPW2; "dpapi" -> Windows
 * DPAPI, i.e. this Windows account on this PC, and NO master password is ever
 * created or prompted for; "legacy" -> unmarked plaintext, the explicit
 * compatibility escape hatch for automation/audit setups. Set from kitty.c when
 * the portable (savemode=dir) backend is activated; registry-backed stores never
 * consult it. This replaces the retired registry-global "PasswordScheme" DWORD,
 * which is no longer read at all - a leftover value of any kind is ignored, so
 * it can no longer make the hive plaintext or master-password.
 *
 * Why "dpapi" exists: it is the only at-rest choice a portable store could not
 * make without a human. Master password is scriptable (-masterpwfile) and so is
 * plaintext (legacy), but DPAPI was reachable ONLY by cancelling the setup
 * dialog - so an unattended import/save that wanted machine-bound protection had
 * to settle for plaintext. This is a WRITE policy only: reads are unchanged, and
 * a store that already holds MPW values still unlocks them normally. */
#define KITTY_PORTABLE_PW_MASTER 0
#define KITTY_PORTABLE_PW_DPAPI  1
#define KITTY_PORTABLE_PW_LEGACY 2
static int g_portable_pw_mode = KITTY_PORTABLE_PW_MASTER;
void kitty_set_portable_password_protection(const char *mode)
{
    if (mode && !_stricmp(mode, "legacy"))     g_portable_pw_mode = KITTY_PORTABLE_PW_LEGACY;
    else if (mode && !_stricmp(mode, "dpapi")) g_portable_pw_mode = KITTY_PORTABLE_PW_DPAPI;
    else                                       g_portable_pw_mode = KITTY_PORTABLE_PW_MASTER;
}
/* True when the portable store is configured to protect secrets with DPAPI
 * instead of a master password. Public so the command line can refuse
 * -masterpwfile rather than quietly honour one of the two (see cmdline.c). */
int kitty_portable_password_dpapi(void)
{
    return g_portable_pw_mode == KITTY_PORTABLE_PW_DPAPI;
}

/* ---- master-password (MPW1) glue ----------------------------------------
 * The crypto lives in kitty/kitty_mpw.c (Argon2id + AES-CBC/HMAC), which needs
 * the crypto lib + CSPRNG and is linked only into tools that have them; it
 * self-registers these ops via a constructor. Tools without it (puttytel/pterm)
 * leave the pointers NULL, so MPW is unavailable: under the master-password
 * policy their saves are refused (kept and reported), never DPAPI instead.
 * The unlock state, per-store salt and verifier (registry) live here. */
#define KSEC_MPW_KEYLEN  64    /* must match KITTY_MPW_DERIVED_LEN */
#define KSEC_MPW_SALTLEN 16    /* must match KITTY_MPW_SALT_LEN */
#define KSEC_MPW_MARK    "MPW1:"
/* MPW2 = the same AES-256-CBC+HMAC envelope with the Argon2id salt embedded:
 * "MPW2:<b64 salt>.<MPW1 payload>". Self-contained, so a value can be
 * unlocked on another machine from the master password alone - required for
 * exported .ktx files, which carry no Security\ salt store. MPW1 (store-salt
 * only) stays read-compatible; all new writes are MPW2. */
#define KSEC_MPW2_MARK   "MPW2:"
#define KSEC_MPW_VERIFY  "KiTTY-MPW-verify"
/* For a store minted elsewhere (a portable copy): the token its verifier
 * must decrypt to. */
const char *kitty_mpw_verify_token(void) { return KSEC_MPW_VERIFY; }
static void (*g_mpw_derive)(const char *, const unsigned char *, int, unsigned char *) = NULL;
static char *(*g_mpw_protect)(const char *, const unsigned char *) = NULL;
static int  (*g_mpw_unprotect)(const char *, const unsigned char *, char **) = NULL;
static void (*g_mpw_randsalt)(unsigned char *, int) = NULL;
void kitty_register_mpw_crypto(
    void (*derive)(const char *, const unsigned char *, int, unsigned char *),
    char *(*protect)(const char *, const unsigned char *),
    int  (*unprotect)(const char *, const unsigned char *, char **),
    void (*randsalt)(unsigned char *, int))
{
    g_mpw_derive = derive; g_mpw_protect = protect;
    g_mpw_unprotect = unprotect; g_mpw_randsalt = randsalt;
}

/* ---- the master key and the supplied passphrase at rest -------------------
 * Both stay for the life of the process, so they are kept wrapped with
 * CryptProtectMemory (this process only) and unwrapped into a local buffer
 * only around each use; the local is wiped afterwards. Resolved at run time
 * from crypt32, as the hand-off below does (kmpw_mem): XP has no
 * CryptProtectMemory. Where it is missing or fails, the value is kept plain,
 * as it was before - the unlock is not lost. A wrapped value that does not
 * unwrap again is dropped (the store is locked again), never used. */
#ifndef CRYPTPROTECTMEMORY_BLOCK_SIZE
#define CRYPTPROTECTMEMORY_BLOCK_SIZE 16
#endif
#ifndef CRYPTPROTECTMEMORY_SAME_PROCESS
#define CRYPTPROTECTMEMORY_SAME_PROCESS 0x00
#endif
#ifndef CRYPTPROTECTMEMORY_SAME_LOGON
#define CRYPTPROTECTMEMORY_SAME_LOGON 0x02
#endif
static int kmpw_mem_flags(const char *fn, void *p, DWORD n, DWORD flags) {
    HMODULE c = GetModuleHandleA("crypt32.dll") ; if(!c) c = LoadLibraryA("crypt32.dll") ;
    if(!c) return 0 ;
    BOOL (WINAPI *pf)(LPVOID,DWORD,DWORD) = (BOOL(WINAPI*)(LPVOID,DWORD,DWORD))GetProcAddress(c,fn) ;
    return pf ? (pf(p,n,flags) ? 1 : 0) : 0 ;
}
typedef struct {
    unsigned char k[KSEC_MPW_KEYLEN];   /* KEYLEN is a multiple of the 16-byte block */
    int wrapped;                        /* 1 = k is CryptProtectMemory'd */
} ksec_keyslot;
static void mpw_slot_put(ksec_keyslot *s, const unsigned char *key)
{
    memcpy(s->k, key, KSEC_MPW_KEYLEN);
    s->wrapped = kmpw_mem_flags("CryptProtectMemory", s->k, KSEC_MPW_KEYLEN,
                                CRYPTPROTECTMEMORY_SAME_PROCESS);
}
static void mpw_slot_clear(ksec_keyslot *s)
{
    SecureZeroMemory(s->k, sizeof(s->k));
    s->wrapped = 0;
}
/* 1 and the key in out (the caller wipes it after use); 0 = it did not
 * unwrap, out wiped */
static int mpw_slot_get(const ksec_keyslot *s, unsigned char *out)
{
    memcpy(out, s->k, KSEC_MPW_KEYLEN);
    if (s->wrapped &&
        !kmpw_mem_flags("CryptUnprotectMemory", out, KSEC_MPW_KEYLEN,
                        CRYPTPROTECTMEMORY_SAME_PROCESS)) {
        SecureZeroMemory(out, KSEC_MPW_KEYLEN);
        return 0;
    }
    return 1;
}
static ksec_keyslot g_mpw_key;                    /* the unlocked store's key */
static int   g_mpw_unlocked = 0;
/* The unlocked store's key into out; 0 when it did not unwrap - the store
 * counts as locked again. */
static int mpw_key_get(unsigned char *out)
{
    if (mpw_slot_get(&g_mpw_key, out)) return 1;
    kitty_pwdebug("mpw: the cached key did not unwrap - locked again");
    mpw_slot_clear(&g_mpw_key);
    g_mpw_unlocked = 0;
    return 0;
}
/* Cancelled (or three wrong answers): the automatic triggers - a read or a
 * save the user did not start (OSC 52 / far2l clipboard, reconnect, save on
 * exit) - prompt no more this run. A user-started action prompts again (the
 * scope below). Reset only by a new supplied passphrase. */
static int   g_mpw_declined = 0;
/* The cancel behind the latch was the first-time SETUP prompt's: there is
 * no master password, and a save kept for it reports that, not "locked"
 * (ksec_report_locked_save). Cleared with the latch and by an unlock. A
 * setup whose salt or verifier could not be stored sets it too: there is no
 * master password then either. */
static int   g_mpw_setup_declined = 0;
/* That failed setup was reported (g_mpw_store_failed), and the report
 * already states that the password was not saved: the report of the save
 * that ran the setup is left out - counted, not shown. Set by the failure,
 * consumed by that report, and cleared by the next unlock attempt, so a save
 * that reported nothing does not swallow a later save's report. */
static int   g_mpw_setup_failed_reported = 0;
/* -masterpwfile / API: NUL-padded to whole blocks, wrapped like the key;
 * non-NULL = a passphrase was supplied */
static unsigned char *g_mpw_passphrase = NULL;
static size_t g_mpw_pass_cap = 0;
static int    g_mpw_pass_wrapped = 0;
static char *(*g_mpw_prompt)(int creating) = NULL; /* GUI/console prompt */
/* The supplied passphrase as a malloc'd string (the caller wipes and frees
 * it), or NULL (none, or it did not unwrap). */
static char *mpw_supplied_dup(void)
{
    char *p;
    if (!g_mpw_passphrase) return NULL;
    p = malloc(g_mpw_pass_cap);
    if (!p) return NULL;
    memcpy(p, g_mpw_passphrase, g_mpw_pass_cap);
    if (g_mpw_pass_wrapped &&
        !kmpw_mem_flags("CryptUnprotectMemory", p, (DWORD)g_mpw_pass_cap,
                        CRYPTPROTECTMEMORY_SAME_PROCESS)) {
        SecureZeroMemory(p, g_mpw_pass_cap);
        free(p);
        return NULL;
    }
    return p;                                     /* NUL-padded: a C string */
}
/* A user-started action - Save in the configuration window, /save, Export,
 * Export All, Make a portable copy - runs inside kitty_mpw_user_action_begin
 * / _end: there the latch above is not consulted, so it prompts again after
 * an earlier cancel. A cancel (or three wrong answers) inside it ends the
 * prompting for the rest of that action only - a store of forty sessions must
 * not prompt forty times - and sets the latch for the automatic triggers. */
static int   g_mpw_user_depth = 0;
static int   g_mpw_user_declined = 0;
void kitty_mpw_user_action_begin(void)
{
    if (g_mpw_user_depth++ == 0)
        g_mpw_user_declined = 0;
}
void kitty_mpw_user_action_end(void)
{
    if (g_mpw_user_depth > 0)
        g_mpw_user_depth--;
}
static int mpw_declined(void)
{
    return g_mpw_user_depth ? g_mpw_user_declined : g_mpw_declined;
}
static void mpw_decline(void)
{
    g_mpw_declined = 1;
    if (g_mpw_user_depth)
        g_mpw_user_declined = 1;
}
/* The interactive prompt's tries, and which one is on screen (1-based,
 * 0 outside a prompt): the dialog shows a wrong answer from the try before. */
#define KSEC_MPW_TRIES 3
static int   g_mpw_try_no = 0;
void kitty_mpw_prompt_try(int *no, int *max)
{
    if (no) *no = g_mpw_try_no;
    if (max) *max = KSEC_MPW_TRIES;
}
static char *mpw_prompt_call(int creating, int no)
{
    char *p;
    g_mpw_try_no = no;
    p = g_mpw_prompt ? g_mpw_prompt(creating) : NULL;
    g_mpw_try_no = 0;
    return p;
}
/* Defer the interactive master-password prompt: when set, a locked MPW value
 * reads back empty (blob preserved by never-wipe) instead of popping the unlock
 * dialog. Used around the startup auto-load of the last session so the config
 * box opens WITHOUT a premature prompt; an explicit Load / "show password" /
 * connect clears it and prompts at the real point of use. A supplied
 * passphrase (-masterpwfile) or an already-unlocked store still works. */
static int   g_mpw_defer = 0;
void kitty_set_defer_mpw_prompt(int on) { g_mpw_defer = on; }
/*
 * For an IMPORT from another store: open one "MPW2:<salt>.<payload>" value
 * with a passphrase given explicitly. Pure - derives against the value's own
 * embedded salt and checks the envelope's HMAC; touches none of this store's
 * state (no unlock, no prompt, no cache, no decline latch). The import owns
 * its own passphrase and its own prompt; this store's crypto path stays
 * exactly as it was. malloc'd plaintext, or NULL (wrong passphrase, not an
 * MPW2 value, or the MPW crypto not linked in this tool).
 */
char *kitty_mpw2_unprotect_with_passphrase(const char *stored, const char *passphrase)
{
    const char *p, *dot;
    char *sb, *m1 = NULL, *pt = NULL, *res = NULL;
    unsigned char *salt = NULL;
    unsigned char key[KSEC_MPW_KEYLEN];
    int sn = 0;

    if (!stored || !passphrase || !*passphrase || !g_mpw_derive || !g_mpw_unprotect)
        return NULL;
    if (strncmp(stored, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK)) != 0)
        return NULL;
    p = stored + strlen(KSEC_MPW2_MARK);
    dot = strchr(p, '.');
    if (!dot || dot == p)
        return NULL;
    sb = malloc((size_t)(dot - p) + 1);
    if (!sb) return NULL;
    memcpy(sb, p, dot - p); sb[dot - p] = '\0';
    salt = ksec_b64_decode(sb, &sn);
    free(sb);
    if (!salt || sn != KSEC_MPW_SALTLEN) { free(salt); return NULL; }
    m1 = malloc(strlen(KSEC_MPW_MARK) + strlen(dot + 1) + 1);
    if (m1) {
        sprintf(m1, "%s%s", KSEC_MPW_MARK, dot + 1);
        g_mpw_derive(passphrase, salt, KSEC_MPW_SALTLEN, key);
        if (g_mpw_unprotect(m1, key, &pt) == 1 && pt)
            res = ksec_dup(pt);
        if (pt) { memset(pt, 0, strlen(pt)); sfree(pt); }
        SecureZeroMemory(key, sizeof(key));
        free(m1);
    }
    free(salt);
    return res;
}
static unsigned char g_mpw_salt[KSEC_MPW_SALTLEN]; /* store salt, cached at unlock */
static int   g_mpw_salt_valid = 0;
/* one-slot cache for a FOREIGN salt (imported MPW2 from another store), so a
 * bulk import derives/prompts once, not per session */
static ksec_keyslot  g_mpw_fkey;
static unsigned char g_mpw_fsalt[KSEC_MPW_SALTLEN];
static int   g_mpw_f_valid = 0;
static int mpw_fkey_get(unsigned char *out)
{
    if (mpw_slot_get(&g_mpw_fkey, out)) return 1;
    mpw_slot_clear(&g_mpw_fkey);
    g_mpw_f_valid = 0;
    return 0;
}

/* ---- export-bundle passphrase --------------------------------------------
 * An export bundle is a TRANSPORT artifact and carries its own password, which
 * is a different concept from the store's master password. While this context
 * is set, secrets written by the export path are wrapped with THIS passphrase
 * and the master password is neither read, created, prompted for nor written;
 * reads of a self-contained MPW2 value try it before falling back to the normal
 * master-password route.
 *
 * Process-scoped, following the established decoupling pattern
 * (kitty_set_storage_mode / kitty_set_session_dir / kitty_set_defer_mpw_prompt)
 * so libsettings stays standalone: the export/import loop sets it, runs, and
 * clears it. Nothing here is ever persisted - no salt, no verifier, no writes
 * to the g_mpw_* store state. */
static char *g_bundle_pass = NULL;
/* "This PC and this account only": wrap the bundle with DPAPI deliberately.
 * A distinct flag rather than merely "no passphrase", because the no-context
 * default must keep behaving exactly as it always has - and because in this
 * mode a master-password prompt must never appear either, which is precisely
 * what the ordinary portable policy would do. */
static int   g_bundle_dpapi = 0;
/* The passwords this bundle left out because their wrap failed
 * (ksec_wrap_bundle): how many, and the first few as the summary's lines
 * (kitty_bundle_wrap_report). */
#define KSEC_BUNDLE_SKIP_LINES 10
static int     g_bundle_wrap_failed = 0;
static strbuf *g_bundle_skip_lines = NULL;
static void ksec_bundle_skips_clear(void)
{
    g_bundle_wrap_failed = 0;
    if (g_bundle_skip_lines) {
        strbuf_free(g_bundle_skip_lines);
        g_bundle_skip_lines = NULL;
    }
}
/* The bundle context is direction-aware. On IMPORT the passphrase must open the
 * bundle's values but must NOT be used to re-protect them: what gets saved
 * belongs to the destination store and has to carry the destination's
 * protection (registry -> DPAPI, portable -> master password). Without this
 * flag an import would rewrite every imported password under the transport
 * password, which nothing on that machine would know to prompt for. */
static int   g_bundle_import = 0;
void kitty_set_bundle_import(int on) { g_bundle_import = (on != 0); }
/* "Make a portable copy" writes its sessions through the export, but its files
 * ARE the copy's folder store, not .ktx exchange files: they take the store's
 * own "key=value" lines (save_open_settings_forced). Line form only - the
 * passwords are wrapped as for any bundle. */
static int   g_bundle_store_form = 0;
void kitty_set_bundle_store_form(int on) { g_bundle_store_form = (on != 0); }
int kitty_bundle_store_form(void) { return g_bundle_store_form; }
void kitty_set_bundle_passphrase(const char *pass)
{
    if (g_bundle_pass) {
        SecureZeroMemory(g_bundle_pass, strlen(g_bundle_pass));
        free(g_bundle_pass);
    }
    g_bundle_pass = (pass && pass[0]) ? ksec_dup(pass) : NULL;
    ksec_bundle_skips_clear();
}
void kitty_set_bundle_dpapi_only(int on) { g_bundle_dpapi = (on != 0); }
void kitty_clear_bundle_context(void)
{
    kitty_set_bundle_passphrase(NULL);
    g_bundle_dpapi = 0;
    ksec_bundle_skips_clear();
    g_bundle_import = 0;
    g_bundle_store_form = 0;
}
int kitty_bundle_wrap_failed(void) { return g_bundle_wrap_failed; }
/* The summary's lines for the passwords left out, snewn'd, or NULL when none
 * was: read it before kitty_clear_bundle_context. */
char *kitty_bundle_wrap_report(void)
{
    strbuf *sb;
    if (!g_bundle_wrap_failed)
        return NULL;
    sb = strbuf_new();
    put_fmt(sb, KT_BUNDLE_SKIPPED_HEAD_FMT, g_bundle_wrap_failed);
    if (g_bundle_skip_lines)
        put_datapl(sb, ptrlen_from_strbuf(g_bundle_skip_lines));
    if (g_bundle_wrap_failed > KSEC_BUNDLE_SKIP_LINES)
        put_fmt(sb, KT_BUNDLE_SKIPPED_MORE_FMT,
                g_bundle_wrap_failed - KSEC_BUNDLE_SKIP_LINES);
    return strbuf_to_str(sb);
}
/* A bundle left out the password of `where` (KSEC_EXPORT_*): counted, and
 * listed while the summary has room. */
static void ksec_bundle_note_skip(int what, const char *where, const char *why)
{
    g_bundle_wrap_failed++;
    kitty_pwdebug("bundle wrap failed: password %d of '%s' left out (%s)",
                  what, where ? where : "", why ? why : "");
    if (g_bundle_wrap_failed > KSEC_BUNDLE_SKIP_LINES)
        return;
    if (!g_bundle_skip_lines)
        g_bundle_skip_lines = strbuf_new();
    put_fmt(g_bundle_skip_lines,
            what == KSEC_EXPORT_SESSION_PXPW ? KT_BUNDLE_SKIPPED_PXPW_FMT :
            what == KSEC_EXPORT_PROXY_PW     ? KT_BUNDLE_SKIPPED_PROXY_FMT :
                                               KT_BUNDLE_SKIPPED_PW_FMT,
            where ? where : "", why ? why : "");
}

void kitty_set_master_passphrase(const char *pass)
{
    if (g_mpw_passphrase) {
        SecureZeroMemory(g_mpw_passphrase, g_mpw_pass_cap);
        free(g_mpw_passphrase);
    }
    g_mpw_passphrase = NULL;
    g_mpw_pass_cap = 0;
    g_mpw_pass_wrapped = 0;
    if (pass && pass[0]) {
        size_t n = strlen(pass) + 1;              /* the NUL is in the buffer */
        size_t cap = (n + CRYPTPROTECTMEMORY_BLOCK_SIZE - 1) /
                     CRYPTPROTECTMEMORY_BLOCK_SIZE * CRYPTPROTECTMEMORY_BLOCK_SIZE;
        g_mpw_passphrase = calloc(1, cap);
        if (g_mpw_passphrase) {
            memcpy(g_mpw_passphrase, pass, n - 1);
            g_mpw_pass_cap = cap;
            g_mpw_pass_wrapped = kmpw_mem_flags("CryptProtectMemory",
                g_mpw_passphrase, (DWORD)cap, CRYPTPROTECTMEMORY_SAME_PROCESS);
        }
    }
    g_mpw_unlocked = 0;                           /* re-derive with the new passphrase */
    mpw_slot_clear(&g_mpw_key);
    g_mpw_declined = 0;                           /* fresh passphrase -> allow another try */
    g_mpw_user_declined = 0;
    g_mpw_setup_declined = 0;
}
void kitty_set_master_pw_prompt(char *(*fn)(int creating)) { g_mpw_prompt = fn; }
/* Called when the master password given was WRONG - every try of the prompt, or
 * the one try of a supplied passphrase - never on a cancel. The console tools
 * refuse there (kitty_console_mpw.c); the GUI sets none and goes on as it did:
 * the value reads as no stored password. */
static void (*g_mpw_wrong)(int supplied) = NULL;
void kitty_set_master_pw_wrong(void (*fn)(int supplied)) { g_mpw_wrong = fn; }

/* Backend-scoped MPW store state (salt + verifier). The registry hive is only
 * right for registry-backed stores; in portable mode these MUST live next to
 * the session files (Security\ subdir at the portable root) so that "copy the
 * files + know the master password" actually unlocks on another machine, and
 * portable mode stays registry-free. Portable reads fall back to the registry
 * read-only, so a dev-era store (salt minted in the hive) keeps unlocking on
 * the same machine; the next state write lands in the portable store. */
#define KSEC_MPW_SUBDIR "Security"
static char *mpw_state_get(const char *name)    /* malloc'd (ksec_dup) or NULL */
{
    if (store_is_file()) {
        /* Portable stores answer from their OWN Security\ folder and nowhere
         * else. There used to be a read-through to the registry here, so that a
         * store whose salt had been minted in the hive kept unlocking on the
         * same machine - but a standing fallback made every FRESH portable store
         * inherit the hive's state, conclude a master password already existed,
         * and therefore never mint its own (its saves then silently degraded to
         * machine-bound DPAPI). The rescue is now a one-shot copy at startup
         * instead: kitty_migrate_portable_mpw_state(). */
        char *v = portable_read_text_file(KSEC_MPW_SUBDIR, name);   /* snewn'd */
        char *res = v ? ksec_dup(v) : NULL;
        if (v) sfree(v);
        return res;
    }
    char b[2048]; DWORD sz = sizeof(b);
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, name, RRF_RT_REG_SZ, NULL, b, &sz)
        != ERROR_SUCCESS) return NULL;
    return ksec_dup(b);
}
/* 1 stored; 0 not, with Windows' reason in GetLastError(). readonly=yes:
 * KITTY_RO_REFUSED (mpw_ensure_unlocked sets nothing up then). */
static int mpw_state_set(const char *name, const char *val)
{
    if (kitty_store_readonly())
        return KITTY_RO_REFUSED;
    if (store_is_file())
        return portable_write_text_file(KSEC_MPW_SUBDIR, name, val);
    HKEY hk;
    LONG rc = RegCreateKeyExA(HKEY_CURRENT_USER, reg_base_buf, 0, NULL, 0,
                              KEY_SET_VALUE, NULL, &hk, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExA(hk, name, 0, REG_SZ, (const BYTE *)val, (DWORD)strlen(val) + 1);
        RegCloseKey(hk);
    }
    if (rc != ERROR_SUCCESS) { SetLastError((DWORD)rc); return 0; }
    return 1;
}

/* The master password's salt or verifier could not be stored: reported to
 * the user by the front end (a message box in the GUI, stderr in the console
 * tools); the save that set it up stores nothing and keeps a password
 * stored before (KT_MPW_STORE_FAILED_FMT). */
static void (*g_mpw_store_failed)(const char *reason) = NULL;
void kitty_set_master_pw_store_failed(void (*fn)(const char *reason)) { g_mpw_store_failed = fn; }
static int mpw_load_salt(unsigned char salt[KSEC_MPW_SALTLEN])
{
    char *b = mpw_state_get("MasterPwSalt");
    if (!b) return 0;
    int n = 0; unsigned char *d = ksec_b64_decode(b, &n);
    free(b);
    if (!d || n != KSEC_MPW_SALTLEN) { if (d) free(d); return 0; }
    memcpy(salt, d, KSEC_MPW_SALTLEN); free(d); return 1;
}

/* Derive (and cache) the master key. creating=1 (a save) may generate the salt +
 * verifier; creating=0 (a load) requires an existing store. Returns 1 if unlocked. */
static int mpw_ensure_unlocked(int creating)
{
    g_mpw_setup_failed_reported = 0;             /* a new save reports for itself */
    if (g_mpw_unlocked) return 1;
    if (mpw_declined())  return 0;               /* cancelled earlier (mpw_declined) */
    /* Deferred (startup auto-load): don't prompt now unless a passphrase was
     * supplied non-interactively. Leaves the value locked; the next explicit
     * load/show/connect runs with defer cleared and prompts then. */
    if (g_mpw_defer && !g_mpw_passphrase) return 0;
    if (!g_mpw_derive || !g_mpw_protect || !g_mpw_unprotect || !g_mpw_randsalt) {
        kitty_pwdebug("mpw unlock: crypto not linked (d=%d p=%d u=%d r=%d)",
                      g_mpw_derive != NULL, g_mpw_protect != NULL,
                      g_mpw_unprotect != NULL, g_mpw_randsalt != NULL);
        return 0;                                /* MPW crypto not linked in this tool */
    }

    unsigned char salt[KSEC_MPW_SALTLEN];
    unsigned char key[KSEC_MPW_KEYLEN];
    int have_salt = mpw_load_salt(salt);
    char *ver = mpw_state_get("MasterPwVerifier");    /* malloc'd (ksec_dup) or NULL */
    int first_time = (!have_salt || !ver);

    /* A load (creating==0) needs an existing store; nothing to unlock otherwise. */
    if (!creating && first_time) { if (ver) free(ver); return 0; }
    /* readonly=yes: no master password is set up (its salt and verifier
     * would have to be stored) - locked, without a prompt */
    if (first_time && kitty_store_readonly()) { if (ver) free(ver); return 0; }

    /* ONE prompt per process: if a preceding decrypt already derived the
     * key for THIS store's salt (foreign-salt cache), and it validates against
     * the stored verifier, promote it to the unlocked key instead of prompting
     * again. This is what makes open-proxy -> save -> connect (all MPW2 in the
     * same store) prompt only on the first step. Salt-matched + verifier-checked,
     * so a genuinely foreign (imported, different-password) key can't slip in. */
    if (ver && have_salt && g_mpw_f_valid &&
        !memcmp(g_mpw_fsalt, salt, KSEC_MPW_SALTLEN) && mpw_fkey_get(key)) {
        char *vpt = NULL; int rv = g_mpw_unprotect(ver, key, &vpt);
        int good = (rv == 1 && vpt && !strcmp(vpt, KSEC_MPW_VERIFY));
        if (vpt) { memset(vpt, 0, strlen(vpt)); sfree(vpt); }
        if (good) {
            mpw_slot_put(&g_mpw_key, key);
            SecureZeroMemory(key, sizeof(key));
            memcpy(g_mpw_salt, salt, KSEC_MPW_SALTLEN);
            g_mpw_salt_valid = 1; g_mpw_unlocked = 1;
            free(ver); return 1;
        }
        SecureZeroMemory(key, sizeof(key));
    }

    if (!have_salt)                              /* first-time setup: mint a salt */
        g_mpw_randsalt(salt, KSEC_MPW_SALTLEN);
    /* NOTE: the minted salt is NOT persisted here. It is written together
     * with the verifier below, only once the user has actually set a master
     * password - cancelling the setup prompt must leave no state behind
     * (no Security\ dir in a portable tree the user said no to). */

    /* A non-interactive passphrase (-masterpwfile/API) gets a single try; an
     * interactive prompt gets a few, re-prompting on a wrong master password. */
    int from_supplied = (g_mpw_passphrase != NULL);
    int tries = from_supplied ? 1 : KSEC_MPW_TRIES;
    int ok = 0, wrong = 0, attempt = 0;

    while (tries-- > 0 && !ok) {
        char *pass = from_supplied ? mpw_supplied_dup()
                   : mpw_prompt_call(first_time, ++attempt);
        if (!pass || !pass[0]) {                 /* cancelled / no prompt available */
            if (pass) { memset(pass, 0, strlen(pass)); free(pass); }
            if (!from_supplied && g_mpw_prompt) {
                mpw_decline();
                g_mpw_setup_declined = first_time;
            }
            break;
        }
        g_mpw_derive(pass, salt, KSEC_MPW_SALTLEN, key);
        memset(pass, 0, strlen(pass)); free(pass);

        if (ver) {                               /* verify against the stored token */
            char *vpt = NULL; int rv = g_mpw_unprotect(ver, key, &vpt);
            ok = (rv == 1 && vpt && !strcmp(vpt, KSEC_MPW_VERIFY));
            if (vpt) { memset(vpt, 0, strlen(vpt)); sfree(vpt); } /* snew'd by kitty_mpw */
            if (!ok) { SecureZeroMemory(key, sizeof(key)); wrong++; } /* wrong: re-prompt */
        } else {                                 /* first time: persist salt + verifier */
            /* The master password exists only once BOTH are stored. It used
             * to count as set either way, so a failed write left a store
             * whose next start prompted for a NEW master password - with the
             * values saved now needing the old one - and nothing reported it. */
            int stored = 1;
            DWORD err = 0;
            if (!have_salt) {
                char *sb = ksec_b64_encode(salt, KSEC_MPW_SALTLEN);
                stored = sb && mpw_state_set("MasterPwSalt", sb);
                if (!stored) err = sb ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
                if (sb) free(sb);
                if (stored) have_salt = 1;
            }
            if (stored) {
                char *vb = g_mpw_protect(KSEC_MPW_VERIFY, key); /* snew'd by kitty_mpw */
                stored = vb && mpw_state_set("MasterPwVerifier", vb);
                if (!stored) err = vb ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
                if (vb) sfree(vb);
            }
            if (!stored) {
                kitty_pwdebug("mpw setup: salt/verifier not stored (error %lu)", (unsigned long)err);
                SecureZeroMemory(key, sizeof(key));
                mpw_decline();                   /* no further setup prompts this run */
                g_mpw_setup_declined = 1;        /* no master password exists */
                if (g_mpw_store_failed) {
                    g_mpw_store_failed(kitty_win_reason((int)err));
                    g_mpw_setup_failed_reported = 1;   /* covers this save's report */
                }
                break;
            }
            ok = 1;
        }
    }

    if (ver) free(ver);
    if (ok) {
        mpw_slot_put(&g_mpw_key, key);
        SecureZeroMemory(key, sizeof(key));
        memcpy(g_mpw_salt, salt, KSEC_MPW_SALTLEN);
        g_mpw_salt_valid = 1;
        g_mpw_unlocked = 1;
        g_mpw_setup_declined = 0;
        return 1;
    }
    kitty_pwdebug("mpw unlock FAILED: first_time=%d supplied=%d prompt=%d declined=%d",
                  first_time, from_supplied, g_mpw_prompt != NULL, mpw_declined());
    /* Three wrong answers end the prompting for this run, exactly as a cancel
     * does: without this, every further value prompted three more times - a
     * store of forty sessions became a hundred and twenty prompts. A
     * user-started action prompts again (kitty_mpw_user_action_begin). */
    if (!from_supplied && g_mpw_prompt)
        mpw_decline();
    SecureZeroMemory(key, sizeof(key));
    mpw_slot_clear(&g_mpw_key);
    if (wrong && g_mpw_wrong)
        g_mpw_wrong(from_supplied);
    return 0;
}

/* Unprotect an "MPW1:..." payload whose Argon2id salt is KNOWN (embedded in an
 * MPW2 value). The derived key is salt-specific, so this works for values from
 * ANY store - the unlocked store key and one foreign key are cached; otherwise
 * the passphrase (from -masterpwfile/API, else a bounded prompt) is derived
 * against the given salt and validated by the envelope's own HMAC.
 * Returns 1 + snew'd plaintext in *outp, else 0. */
static int mpw_unprotect_with_salt(const char *m1blob,
                                   const unsigned char salt[KSEC_MPW_SALTLEN],
                                   char **outp)
{
    *outp = NULL;
    if (!g_mpw_derive || !g_mpw_unprotect) return 0;
    /* Import of an export bundle in progress: the bundle's own passphrase is
     * the right key, and it must be tried BEFORE anything that could prompt for
     * the master password - importing must never raise a master-password
     * dialog. The envelope's HMAC authenticates the attempt, so a wrong guess
     * simply fails through to the normal route. */
    if (g_bundle_pass) {
        unsigned char bkey[KSEC_MPW_KEYLEN];
        g_mpw_derive(g_bundle_pass, salt, KSEC_MPW_SALTLEN, bkey);
        int bok = (g_mpw_unprotect(m1blob, bkey, outp) == 1 && *outp);
        SecureZeroMemory(bkey, sizeof(bkey));
        if (bok) return 1;
        if (*outp) { sfree(*outp); *outp = NULL; }
    }
    if (g_mpw_unlocked && g_mpw_salt_valid &&
        !memcmp(salt, g_mpw_salt, KSEC_MPW_SALTLEN)) {
        unsigned char k[KSEC_MPW_KEYLEN];
        if (mpw_key_get(k)) {
            int got = (g_mpw_unprotect(m1blob, k, outp) == 1 && *outp);
            SecureZeroMemory(k, sizeof(k));
            if (got) return 1;
        }
        if (*outp) { sfree(*outp); *outp = NULL; }
    }
    if (g_mpw_f_valid && !memcmp(salt, g_mpw_fsalt, KSEC_MPW_SALTLEN)) {
        unsigned char k[KSEC_MPW_KEYLEN];
        if (mpw_fkey_get(k)) {
            int got = (g_mpw_unprotect(m1blob, k, outp) == 1 && *outp);
            SecureZeroMemory(k, sizeof(k));
            if (got) return 1;
        }
        if (*outp) { sfree(*outp); *outp = NULL; }
    }
    int from_supplied = (g_mpw_passphrase != NULL);
    int tries = from_supplied ? 1 : KSEC_MPW_TRIES;
    int attempt = 0;
    /* Which key opened a value is invisible otherwise, and "why did it prompt
     * when I passed -masterpwfile?" is the question that gets asked - it was a
     * real defect once (the switch was SAVEABLE, so it arrived after -load).
     * Flags only, no secrets. */
    kitty_pwdebug("mpw2 unwrap: supplied=%d unlocked=%d f_valid=%d declined=%d defer=%d",
                  from_supplied, g_mpw_unlocked, g_mpw_f_valid,
                  mpw_declined(), g_mpw_defer);
    /* Deferred startup load or no interactive prompt available -> stay locked
     * (the cached-key fast paths above already ran). */
    if (!from_supplied && (mpw_declined() || !g_mpw_prompt || g_mpw_defer)) return 0;
    while (tries-- > 0) {
        char *pass = from_supplied ? mpw_supplied_dup()
                                   : mpw_prompt_call(0, ++attempt);
        if (!pass || !pass[0]) {
            if (pass) { memset(pass, 0, strlen(pass)); free(pass); }
            if (!from_supplied) mpw_decline();
            return 0;
        }
        unsigned char key[KSEC_MPW_KEYLEN];
        g_mpw_derive(pass, salt, KSEC_MPW_SALTLEN, key);
        memset(pass, 0, strlen(pass)); free(pass);
        if (g_mpw_unprotect(m1blob, key, outp) == 1 && *outp) {
            mpw_slot_put(&g_mpw_fkey, key);
            memcpy(g_mpw_fsalt, salt, KSEC_MPW_SALTLEN);
            g_mpw_f_valid = 1;
            SecureZeroMemory(key, sizeof(key));
            return 1;
        }
        if (*outp) { sfree(*outp); *outp = NULL; }
        SecureZeroMemory(key, sizeof(key));
    }
    /* Three wrong answers end the prompting for this run, as a cancel does -
     * otherwise every further value of the same store prompted three more
     * times. A user-started action prompts again
     * (kitty_mpw_user_action_begin). */
    if (!from_supplied)
        mpw_decline();
    if (g_mpw_wrong)                    /* every try ran and none opened it */
        g_mpw_wrong(from_supplied);
    return 0;
}

/* Compose a self-contained "MPW2:<b64 salt>.<payload>" from an "MPW1:..." blob
 * and the salt it was derived with. malloc'd, or NULL. */
static char *mpw2_compose(const char *m1blob, const unsigned char salt[KSEC_MPW_SALTLEN])
{
    char *sb = ksec_b64_encode(salt, KSEC_MPW_SALTLEN);
    char *res = NULL;
    if (sb) {
        const char *payload = m1blob + strlen(KSEC_MPW_MARK);
        size_t n = strlen(KSEC_MPW2_MARK) + strlen(sb) + 1 + strlen(payload) + 1;
        res = malloc(n);
        if (res) snprintf(res, n, "%s%s.%s", KSEC_MPW2_MARK, sb, payload);
        free(sb);
    }
    return res;
}

/* Wrap `plaintext` under an EXPLICIT passphrase, as a self-contained MPW2 value
 * (fresh salt minted per call and embedded in the result), so it unlocks
 * anywhere from that passphrase alone. This is what an export bundle needs.
 *
 * Persists NOTHING: no MasterPwSalt, no MasterPwVerifier, no writes to the
 * g_mpw_* store state and no touching of the key caches. That is the entire
 * point - exporting must not create a master password as a side effect.
 *
 * malloc'd result, or NULL if the MPW crypto is not linked into this tool or
 * the wrap fails. */
char *ksec_wrap_with_passphrase(const char *plaintext, const char *passphrase)
{
    unsigned char salt[KSEC_MPW_SALTLEN], key[KSEC_MPW_KEYLEN];
    char *m1, *res = NULL;
    if (!plaintext || !passphrase || !passphrase[0]) return NULL;
    if (!g_mpw_derive || !g_mpw_protect || !g_mpw_randsalt) return NULL;
    g_mpw_randsalt(salt, KSEC_MPW_SALTLEN);
    g_mpw_derive(passphrase, salt, KSEC_MPW_SALTLEN, key);
    m1 = g_mpw_protect(plaintext, key);          /* "MPW1:..." (snew'd) */
    if (m1) {
        res = mpw2_compose(m1, salt);
        memset(m1, 0, strlen(m1));
        sfree(m1);
    }
    SecureZeroMemory(key, sizeof(key));
    SecureZeroMemory(salt, sizeof(salt));
    return res;
}

/* Unwrap a self-contained MPW2 value with an EXPLICIT passphrase. Touches no
 * cached key and no store state, so a wrong passphrase costs nothing but the
 * derivation. Returns 1 and a malloc'd plaintext in *out, else 0. */
int ksec_unwrap_with_passphrase(const char *stored, const char *passphrase,
                                char **out)
{
    if (out) *out = NULL;
    if (!stored || !out || !passphrase || !passphrase[0]) return 0;
    if (!g_mpw_derive || !g_mpw_unprotect) return 0;
    if (strncmp(stored, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK)) != 0) return 0;

    const char *p = stored + strlen(KSEC_MPW2_MARK);
    const char *dot = strchr(p, '.');
    if (!dot || dot <= p) return 0;

    char *sb = malloc((size_t)(dot - p) + 1);
    if (!sb) return 0;
    memcpy(sb, p, dot - p); sb[dot - p] = '\0';
    int sn = 0;
    unsigned char *salt = ksec_b64_decode(sb, &sn);
    free(sb);
    if (!salt || sn != KSEC_MPW_SALTLEN) { if (salt) free(salt); return 0; }

    size_t mn = strlen(KSEC_MPW_MARK) + strlen(dot + 1) + 1;
    char *m1 = malloc(mn);
    int ok = 0;
    if (m1) {
        unsigned char key[KSEC_MPW_KEYLEN];
        char *pt = NULL;
        snprintf(m1, mn, "%s%s", KSEC_MPW_MARK, dot + 1);
        g_mpw_derive(passphrase, salt, KSEC_MPW_SALTLEN, key);
        if (g_mpw_unprotect(m1, key, &pt) == 1 && pt) {   /* HMAC authenticates it */
            *out = ksec_dup(pt);
            ok = (*out != NULL);
        }
        if (pt) { memset(pt, 0, strlen(pt)); sfree(pt); }
        SecureZeroMemory(key, sizeof(key));
        free(m1);
    }
    free(salt);
    return ok;
}

/* The last DPAPI wrap's failure: Windows' error, ERROR_NOT_ENOUGH_MEMORY when
 * the wrapped value could not be built, E_FAIL when Windows gave no code. */
static DWORD g_ksec_dpapi_err = 0;
#ifdef KITTY_SELFTEST_HOOKS
/* test_storage_roundtrip only (its target defines KITTY_SELFTEST_HOOKS, no
 * shipped program does): a non-zero error makes every DPAPI wrap fail with
 * it, as a failing CryptProtectData would. */
static DWORD g_ksec_test_dpapi_fail = 0;
void kitty_selftest_dpapi_fail(DWORD err) { g_ksec_test_dpapi_fail = err; }
#endif

/* DPAPI1 wrap: malloc'd "DPAPI1:<b64>", or NULL on failure with the reason in
 * g_ksec_dpapi_err and GetLastError(). */
static char *ksec_dpapi_protect(const char *plaintext)
{
    DATA_BLOB in, out;
    DWORD err;
#ifdef KITTY_SELFTEST_HOOKS
    if (g_ksec_test_dpapi_fail) {
        g_ksec_dpapi_err = g_ksec_test_dpapi_fail;
        SetLastError(g_ksec_dpapi_err);
        return NULL;
    }
#endif
    in.pbData = (BYTE *)plaintext; in.cbData = (DWORD)strlen(plaintext);
    out.pbData = NULL; out.cbData = 0;
    if (CryptProtectData(&in, L"KiTTY stored credential", NULL, NULL, NULL,
                         CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        char *b64 = ksec_b64_encode(out.pbData, (int)out.cbData);
        if (out.pbData) LocalFree(out.pbData);
        if (b64) {
            size_t n = sizeof(KITTY_SECRET_DPAPI_MARK) + strlen(b64);
            char *res = malloc(n);
            if (res) { strcpy(res, KITTY_SECRET_DPAPI_MARK); strcat(res, b64); }
            free(b64);
            if (res) return res;
        }
        err = ERROR_NOT_ENOUGH_MEMORY;
    } else {
        err = GetLastError();
        if (!err)
            err = (DWORD)E_FAIL;
    }
    g_ksec_dpapi_err = err;
    SetLastError(err);
    return NULL;
}

/* Why the last refused wrap (the _kept wraps: NULL and *locked = 1) refused,
 * for the report of that save (ksec_report_locked_save): 1 = DPAPI failed,
 * with its error; 0 = the master password (locked, not set up). Set by every
 * refusal, consumed by the report. */
static int   g_ksec_refused_dpapi = 0;
static DWORD g_ksec_refused_err = 0;
static void ksec_refuse_dpapi(void)
{
    g_ksec_refused_dpapi = 1;
    g_ksec_refused_err = g_ksec_dpapi_err;
}

/* ---- Cross-process master-password sharing (launcher-mpw-sharing) ----------
 * Hand the unlocked master key to a spawned child KiTTY through an INHERITABLE
 * file mapping - child-only, mirroring the existing Duplicate-Session conf
 * hand-off - with the key wrapped by CryptProtectMemory(SAME_LOGON) so the
 * shared section never holds it in the clear and only this logon can unwrap it.
 * Fail-safe throughout: any failure just leaves the child to prompt as before. */
#ifndef CRYPTPROTECTMEMORY_BLOCK_SIZE
#define CRYPTPROTECTMEMORY_BLOCK_SIZE 16
#endif
#ifndef CRYPTPROTECTMEMORY_SAME_LOGON
#define CRYPTPROTECTMEMORY_SAME_LOGON 0x02
#endif
#define KMPW_INHERIT_MAGIC 0x57504D4Bu   /* 'KMPW' */

struct kmpw_inherit_blob {
    unsigned magic ;
    unsigned version ;
    unsigned wrapped ;                       /* 1 = key is CryptProtectMemory'd */
    unsigned char salt[KSEC_MPW_SALTLEN] ;
    unsigned char key[KSEC_MPW_KEYLEN] ;     /* KEYLEN is a multiple of the 16-byte block */
} ;

static int kmpw_mem(const char *fn, void *p, DWORD n) {
    return kmpw_mem_flags(fn, p, n, CRYPTPROTECTMEMORY_SAME_LOGON) ;
}

/* Core: map the inherited mapping `fm` (sz bytes), validate + unlock, close it.
 * Verifies the key against this store's verifier when the store is readable;
 * otherwise trusts the inherited-handle authenticity (only our parent could have
 * created it, and CryptUnprotectMemory only unwraps in this logon). */
static void kmpw_consume(HANDLE fm, unsigned sz) {
    if (!fm || sz != sizeof(struct kmpw_inherit_blob)) { if(fm) CloseHandle(fm) ; return ; }
    struct kmpw_inherit_blob *b = (struct kmpw_inherit_blob*)MapViewOfFile(fm, FILE_MAP_READ, 0, 0, sz) ;
    if (b) {
        struct kmpw_inherit_blob loc ; memcpy(&loc, b, sizeof(loc)) ; UnmapViewOfFile(b) ;
        int ok = (loc.magic==KMPW_INHERIT_MAGIC && loc.version==1) ;
        if (ok && loc.wrapped) ok = kmpw_mem("CryptUnprotectMemory", loc.key, KSEC_MPW_KEYLEN) ;
        if (ok) {
            unsigned char salt[KSEC_MPW_SALTLEN] ;
            char *ver = mpw_state_get("MasterPwVerifier") ;
            int trust = 1 ;
            if (mpw_load_salt(salt) && ver && g_mpw_unprotect) {   /* store readable -> verify */
                trust = 0 ;
                if (!memcmp(salt, loc.salt, KSEC_MPW_SALTLEN)) {
                    char *vpt=NULL ;
                    if (g_mpw_unprotect(ver, loc.key, &vpt)==1 && vpt && !strcmp(vpt,KSEC_MPW_VERIFY)) trust = 1 ;
                    if (vpt) { memset(vpt,0,strlen(vpt)) ; sfree(vpt) ; }
                }
            }
            if (ver) free(ver) ;
            if (trust) {
                mpw_slot_put(&g_mpw_key, loc.key) ;   /* wrapped for this process */
                memcpy(g_mpw_salt, loc.salt, KSEC_MPW_SALTLEN) ;
                g_mpw_salt_valid = 1 ; g_mpw_unlocked = 1 ;
            }
        }
        SecureZeroMemory(&loc, sizeof(loc)) ;
    }
    CloseHandle(fm) ;
}

/* Parent: if the portable master password is unlocked, publish the key into a
 * fresh mapping and write "<prefix><hex>:<size>" into tok for the child command
 * line (prefix e.g. "&K" for the @/& dispatch, " -mpwkey " for the argument
 * parser). Returns the mapping HANDLE, or NULL (and tok emptied) when there is
 * nothing to share. The handle is NOT inheritable: it reaches the one child
 * through kitty_create_process_with_handles (kitty_winutil.c) only - it used
 * to be inheritable from creation, so any program started with inheritance
 * before it was closed received the key as well. Close it after the start. */
HANDLE kitty_mpw_export_inherit_blob(const char *prefix, char *tok, size_t toklen) {
    if (tok && toklen) tok[0] = '\0' ;
    if (!store_is_file() || !g_mpw_unlocked || !g_mpw_salt_valid) return NULL ;
    struct kmpw_inherit_blob blob ; memset(&blob, 0, sizeof(blob)) ;
    blob.magic = KMPW_INHERIT_MAGIC ; blob.version = 1 ;
    memcpy(blob.salt, g_mpw_salt, KSEC_MPW_SALTLEN) ;
    /* unwrapped from this process's protection, wrapped for the logon */
    if (!mpw_key_get(blob.key)) { SecureZeroMemory(&blob,sizeof(blob)) ; return NULL ; }
    blob.wrapped = kmpw_mem("CryptProtectMemory", blob.key, KSEC_MPW_KEYLEN) ;
    HANDLE fm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(blob), NULL) ;
    if (!fm || fm==INVALID_HANDLE_VALUE) { SecureZeroMemory(&blob,sizeof(blob)) ; return NULL ; }
    void *p = MapViewOfFile(fm, FILE_MAP_WRITE, 0, 0, sizeof(blob)) ;
    if (!p) { CloseHandle(fm) ; SecureZeroMemory(&blob,sizeof(blob)) ; return NULL ; }
    memcpy(p, &blob, sizeof(blob)) ; UnmapViewOfFile(p) ;
    SecureZeroMemory(&blob, sizeof(blob)) ;
    if (tok && toklen) snprintf(tok, toklen, "%s%p:%u", prefix?prefix:"", fm, (unsigned)sizeof(blob)) ;
    return fm ;
}

/* Child (@/& dispatch): consume a leading "&K<hex>:<size>" token from `p` and
 * return `p` advanced past it (or unchanged if no token). */
char *kitty_mpw_import_inherit_blob(char *p) {
    while (*p && isspace((unsigned char)*p)) p++ ;
    if (!(p[0]=='&' && p[1]=='K')) return p ;
    HANDLE fm=NULL ; unsigned sz=0 ; int n=0 ;
    if (sscanf(p+2, "%p:%u%n", &fm, &sz, &n) < 2 || n<=0) return p ;
    kmpw_consume(fm, sz) ;
    return p + 2 + n ;
}

/* Child (argument parser, launcher -load path): consume a "<hex>:<size>" string
 * from the -mpwkey option value. */
void kitty_mpw_consume_handle_str(const char *s) {
    HANDLE fm=NULL ; unsigned sz=0 ;
    if (s && sscanf(s, "%p:%u", &fm, &sz) == 2) kmpw_consume(fm, sz) ;
}

/* Child (modes that never reach the argument parser: -launcher, -manage):
 * consume a "-mpwkey <hex>:<size>" pair from the raw command line `cl` and
 * blank it there, so it is neither taken for something else nor handed on.
 * They read none, so a key passed to them was never used and its handle stayed
 * open in them for their whole run. */
void kitty_mpw_consume_cmdline_switch(char *cl) {
    char *p = cl ;
    if (!cl) return ;
    while ((p = strstr(p, "-mpwkey")) != NULL) {
        char *q = p + strlen("-mpwkey"), *v, *e ;
        if ((p > cl && !isspace((unsigned char)p[-1])) || !isspace((unsigned char)*q)) { p = q ; continue ; }
        v = q ;
        while (*v && isspace((unsigned char)*v)) v++ ;
        e = v ;
        while (*e && !isspace((unsigned char)*e)) e++ ;
        if (e > v) {
            char val[64] ;
            size_t n = (size_t)(e - v) < sizeof(val) - 1 ? (size_t)(e - v) : sizeof(val) - 1 ;
            memcpy(val, v, n) ; val[n] = '\0' ;
            kitty_mpw_consume_handle_str(val) ;
        }
        while (p < e) *p++ = ' ' ;
        return ;
    }
}

/* Root process: prompt once at startup to unlock the portable master password so
 * the whole run - and every child it spawns - is silent afterwards. No-op in
 * registry mode, when already unlocked, or when no master password is set.
 * Also a no-op when PortablePasswordProtection is dpapi or legacy: a master
 * password set up before the switch stays in Security\, and its prompt at
 * every start came up even in a store whose passwords are all DPAPI1
 * (hknet/KiTTY#69). A value still MPW2 brings the prompt when it is read. */
int kitty_mpw_startup_unlock(void) {
    if (g_mpw_unlocked) return 1 ;
    if (!store_is_file()) return 0 ;
    if (g_portable_pw_mode != KITTY_PORTABLE_PW_MASTER) return 0 ;
    char *ver = mpw_state_get("MasterPwVerifier") ;
    if (!ver) return 0 ;
    free(ver) ;
    return mpw_ensure_unlocked(0) ;
}

/* plaintext -> stored form (malloc'd), backend-scoped policy:
 * the protection is chosen by WHERE the
 * value is stored, not by a global user scheme.
 *
 * Registry backend: always DPAPI1. A leftover PasswordScheme=1/2 DWORD must
 * not make the hive plaintext or master-password, and a master-password
 * prompt must never appear for a registry save (this helper never calls
 * mpw_ensure_unlocked). DPAPI failure -> NULL and *locked = 1: nothing is
 * stored for that secret, the caller keeps what is stored and reports it
 * (ksec_report_locked_save words it from the DPAPI error). It used to store
 * the plaintext instead. */
char *ksec_protect_registry_kept(const char *plaintext, int *locked)
{
    char *res;
    if (locked) *locked = 0;
    if (!plaintext || !plaintext[0]) return ksec_dup("");
    res = ksec_dpapi_protect(plaintext);
    if (!res) {
        kitty_pwdebug("protect registry: DPAPI failed (error %lu), not stored",
                      (unsigned long)g_ksec_dpapi_err);
        ksec_refuse_dpapi();
        if (locked) *locked = 1;
    }
    return res;
}
char *ksec_protect_registry(const char *plaintext)
{
    return ksec_protect_registry_kept(plaintext, NULL);
}

/* Portable-file backend: MPW2 when the master password is set/unlockable (the
 * first non-empty secret save may prompt to create it; a cancel stops further
 * automatic prompts this run). kitty.ini PortablePasswordProtection selects
 * DPAPI ("this PC and account", never a prompt) or the explicit-compat
 * plaintext escape hatch ("legacy") instead.
 *
 * Every save (the session, a named proxy, a login script, a .ktx export):
 * under the master-password policy a locked master password - cancelled,
 * wrong, latched, its setup not stored, the crypto not linked - gives NULL
 * and *locked = 1: the caller keeps what is stored and reports it. A save
 * used to fall back to DPAPI there and silently change the stored password's
 * protection (a portable store then held a value only this Windows account
 * could open). A DPAPI wrap that fails (the dpapi policy) gives NULL and
 * *locked = 1 the same way, never the plaintext. */
static char *ksec_protect_portable_x(const char *plaintext, int *locked)
{
    char *res;
    if (locked) *locked = 0;
    if (!plaintext || !plaintext[0]) return ksec_dup("");
    /* Which branch this takes is otherwise invisible, and "why did I get DPAPI
     * when I expected MPW2?" is the question that actually gets asked. No
     * plaintext, just the decision inputs. */
    kitty_pwdebug("protect portable: mode=%d crypto=%d supplied=%d unlocked=%d declined=%d defer=%d",
                  g_portable_pw_mode, g_mpw_derive != NULL,
                  g_mpw_passphrase != NULL, g_mpw_unlocked, mpw_declined(),
                  g_mpw_defer);
    if (g_portable_pw_mode == KITTY_PORTABLE_PW_LEGACY)
        return ksec_dup(plaintext);
    /* DPAPI policy: fall straight through to the DPAPI wrap below, without
     * touching the master-password machinery -- no unlock, no setup dialog, no
     * Security\ state written. */
    if (g_portable_pw_mode == KITTY_PORTABLE_PW_MASTER) {
        unsigned char k[KSEC_MPW_KEYLEN];
        if (mpw_ensure_unlocked(1) && mpw_key_get(k)) {
            char *mb = g_mpw_protect(plaintext, k);   /* "MPW1:..." (snew'd) */
            SecureZeroMemory(k, sizeof(k));
            if (mb) {
                /* Wrap as self-contained MPW2 (salt embedded) so the value
                 * stays unlockable away from this store's Security\/registry
                 * salt. */
                char *res = g_mpw_salt_valid ? mpw2_compose(mb, g_mpw_salt) : NULL;
                sfree(mb);
                if (res) return res;
                /* MPW2 build failed (salt b64 OOM) -> do NOT persist bare
                 * MPW1: it is the least portable form and would make MPW1 a
                 * live write format again. */
                kitty_pwdebug("MPW2 wrap failed (salt b64 OOM)");
            }
        }
        /* "Use DPAPI" in the setup prompt switched the store's policy
         * (kitty_mpw_gui.c): DPAPI is then the user's own choice, and the
         * wrap below takes it. Otherwise: refused, never DPAPI instead. */
        if (g_portable_pw_mode != KITTY_PORTABLE_PW_DPAPI) {
            g_ksec_refused_dpapi = 0;
            if (locked) *locked = 1;
            return NULL;
        }
    }
    res = ksec_dpapi_protect(plaintext);
    if (!res) {
        kitty_pwdebug("protect portable: DPAPI failed (error %lu), not stored",
                      (unsigned long)g_ksec_dpapi_err);
        ksec_refuse_dpapi();
        if (locked) *locked = 1;
    }
    return res;
}
char *ksec_protect_portable(const char *plaintext)
{
    return ksec_protect_portable_x(plaintext, NULL);
}
char *ksec_protect_portable_kept(const char *plaintext, int *locked)
{
    return ksec_protect_portable_x(plaintext, locked);
}

/* A save kept a stored secret because its wrap was refused (the _kept wraps):
 * the master password is locked, or Windows' DPAPI could not encrypt it.
 * Reported by the front end - a notice and an Event Log line in a terminal,
 * the command line's output in a do-and-exit run, a box elsewhere. Without
 * one (the console tools): stderr. `what` is KSEC_KEPT_*, `where` the
 * session (its file, relative to the session folder, or its name in the
 * registry), the named proxy or the export file. Password and ProxyPassword
 * of one save - the same report within two seconds - report once. After a
 * cancelled first-time setup the report is that no master password was set
 * up: nothing is locked then. A DPAPI refusal counts as kept like the
 * others (-importdir's exit code). */
static void (*g_mpw_locked_save)(const char *cap, const char *msg) = NULL;
static int g_mpw_kept = 0, g_mpw_kept_nosetup = 0, g_mpw_kept_dpapi = 0;
void kitty_set_master_pw_locked_save(void (*fn)(const char *cap, const char *msg)) { g_mpw_locked_save = fn; }
int kitty_mpw_kept_count(void) { return g_mpw_kept; }
int kitty_mpw_kept_nosetup_count(void) { return g_mpw_kept_nosetup; }
int kitty_mpw_kept_dpapi_count(void) { return g_mpw_kept_dpapi; }
/* A do-and-exit run where no output took a report as it came (no console,
 * nothing redirected - kitty_mpw_gui.c): the lines wait for the run's
 * result, which shows them (windows/putty.c, -importdir). Twenty at most,
 * the rest counted. */
#define KSEC_KEPT_HELD_MAX 20
static char *g_kept_held = NULL;
static int g_kept_held_n = 0, g_kept_held_more = 0;
void ksec_kept_hold(const char *line)
{
    size_t have = g_kept_held ? strlen(g_kept_held) : 0, add;
    char *p;
    if (!line || !line[0]) return;
    if (g_kept_held_n >= KSEC_KEPT_HELD_MAX) { g_kept_held_more++; return; }
    add = strlen(line);
    p = realloc(g_kept_held, have + add + 2);
    if (!p) { g_kept_held_more++; return; }
    p[have] = '\n';
    memcpy(p + have + 1, line, add + 1);
    g_kept_held = p;
    g_kept_held_n++;
}
const char *kitty_mpw_kept_held(int *more)
{
    if (more) *more = g_kept_held_more;
    return g_kept_held;
}
/* The report's way out: the front end's, else stderr */
static void ksec_kept_deliver(const char *cap, const char *msg)
{
    if (g_mpw_locked_save) {
        g_mpw_locked_save(cap, msg);
        return;
    }
    fprintf(stderr, "%s\n", msg);
}
void ksec_report_locked_save(int what, const char *where)
{
    static char *last = NULL;   /* the last report's key, whole */
    static DWORD last_tick;
    char *key, *msg;
    DWORD now = GetTickCount();
    int dpapi = g_ksec_refused_dpapi;
    DWORD dpapi_err = g_ksec_refused_err;
    int nosetup = !dpapi && g_mpw_setup_declined;
    g_ksec_refused_dpapi = 0;                    /* consumed */
    if (!where) where = "";
    key = dupprintf("%d:%s", what, where);
    if (last && !strcmp(last, key) && now - last_tick < 2000) {
        sfree(key);
        return;
    }
    sfree(last);
    last = key;
    last_tick = now;
    g_mpw_kept++;
    if (dpapi) {
        /* Windows' reason: its text and number (kitty_win_reason) */
        g_mpw_kept_dpapi++;
        msg = dupprintf(
                 what == KSEC_KEPT_PROXY  ? KT_DPAPI_KEPT_PROXY_FMT :
                 what == KSEC_KEPT_SCRIPT ? KT_DPAPI_KEPT_SCRIPT_FMT :
                 what == KSEC_KEPT_EXPORT ? KT_DPAPI_KEPT_EXPORT_FMT :
                                            KT_DPAPI_KEPT_SAVE_FMT,
                 kitty_win_reason(dpapi_err), where);
        kitty_pwdebug("save kept the stored secret: DPAPI failed (error %lu; %d, %s)",
                      (unsigned long)dpapi_err, what, where);
        ksec_kept_deliver(KT_DPAPI_KEPT_CAP, msg);
        sfree(msg);
        return;
    }
    if (nosetup)
        g_mpw_kept_nosetup++;
    if (nosetup)
        msg = dupprintf(
                 what == KSEC_KEPT_PROXY  ? KT_MPW_NOSETUP_PROXY_FMT :
                 what == KSEC_KEPT_SCRIPT ? KT_MPW_NOSETUP_SCRIPT_FMT :
                 what == KSEC_KEPT_EXPORT ? KT_MPW_NOSETUP_EXPORT_FMT :
                                            KT_MPW_NOSETUP_SAVE_FMT, where);
    else
        msg = dupprintf(
                 what == KSEC_KEPT_PROXY  ? KT_MPW_LOCKED_PROXY_FMT :
                 what == KSEC_KEPT_SCRIPT ? KT_MPW_LOCKED_SCRIPT_FMT :
                 what == KSEC_KEPT_EXPORT ? KT_MPW_LOCKED_EXPORT_FMT :
                                            KT_MPW_LOCKED_SAVE_FMT, where);
    kitty_pwdebug("save kept the stored secret: master password %s (%d, %s)",
                  nosetup ? "not set up" : "locked", what, where);
    if (g_mpw_setup_failed_reported) {
        /* the save whose setup failed: that failure's report covers it */
        g_mpw_setup_failed_reported = 0;
        sfree(msg);
        return;
    }
    ksec_kept_deliver(KT_MPW_LOCKED_SAVE_CAP, msg);
    sfree(msg);
}

/* stored -> plaintext in *out (malloc'd). 1=ok, 0=absent, -1=undecryptable DPAPI blob. */
int ksec_unprotect(const char *stored, char **out)
{
    size_t marklen = strlen(KITTY_SECRET_DPAPI_MARK);
    *out = NULL;
    if (!stored || !stored[0]) { *out = ksec_dup(""); return 0; }
    /* PLAIN: - a password deliberately provisioned in the clear by an external
     * script. Handled here, at the shared chokepoint, so it works wherever a
     * stored value is read: an imported .ktx, a portable session file rolled
     * out by that script, or a registry value written by one. Strip the marker
     * and report success; the value is then re-protected by the destination
     * backend on the next save, so the cleartext lives only in the rollout
     * file. KiTTY never writes this marker itself. */
    if (!strncmp(stored, KITTY_SECRET_PLAIN_MARK,
                 strlen(KITTY_SECRET_PLAIN_MARK))) {
        *out = ksec_dup(stored + strlen(KITTY_SECRET_PLAIN_MARK));
        return 1;
    }
    if (!strncmp(stored, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK))) {
        /* Self-contained: split "<b64 salt>.<payload>", derive with the
         * embedded salt (works for values from ANY store, e.g. imported .ktx). */
        const char *p = stored + strlen(KSEC_MPW2_MARK);
        const char *dot = strchr(p, '.');
        if (dot && dot > p) {
            char *sb = malloc((size_t)(dot - p) + 1);
            unsigned char *salt = NULL;
            int sn = 0;
            if (sb) {
                memcpy(sb, p, dot - p); sb[dot - p] = '\0';
                salt = ksec_b64_decode(sb, &sn);
                free(sb);
            }
            if (salt && sn == KSEC_MPW_SALTLEN) {
                size_t mn = strlen(KSEC_MPW_MARK) + strlen(dot + 1) + 1;
                char *m1 = malloc(mn);
                if (m1) {
                    snprintf(m1, mn, "%s%s", KSEC_MPW_MARK, dot + 1);
                    char *pt = NULL;
                    int rv = mpw_unprotect_with_salt(m1, salt, &pt); /* snew'd */
                    free(m1);
                    if (rv == 1 && pt) {
                        char *res = ksec_dup(pt);
                        memset(pt, 0, strlen(pt)); sfree(pt);
                        free(salt);
                        *out = res; return 1;
                    }
                    if (pt) sfree(pt);
                }
            }
            if (salt) free(salt);
        }
        *out = ksec_dup(""); return -1;   /* locked/wrong/malformed -> never-wipe */
    }
    if (!strncmp(stored, KSEC_MPW_MARK, strlen(KSEC_MPW_MARK))) {
        unsigned char k[KSEC_MPW_KEYLEN];
        if (mpw_ensure_unlocked(0) && mpw_key_get(k)) {
            char *pt = NULL; int rv = g_mpw_unprotect(stored, k, &pt); /* snew'd */
            SecureZeroMemory(k, sizeof(k));
            if (rv == 1 && pt) { char *res = ksec_dup(pt); sfree(pt); *out = res; return 1; }
            if (pt) sfree(pt);
        }
        *out = ksec_dup(""); return -1;   /* MPW locked/wrong -> undecryptable (never-wipe) */
    }
    if (!strncmp(stored, KITTY_SECRET_DPAPI_MARK, marklen)) {
        int blen = 0;
        unsigned char *blob = ksec_b64_decode(stored + marklen, &blen);
        if (blob) {
            DATA_BLOB in, dec;
            in.pbData = blob; in.cbData = (DWORD)blen;
            dec.pbData = NULL; dec.cbData = 0;
            if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                                   CRYPTPROTECT_UI_FORBIDDEN, &dec)) {
                char *pt = malloc(dec.cbData + 1);
                if (pt) { memcpy(pt, dec.pbData, dec.cbData); pt[dec.cbData] = '\0'; }
                if (dec.pbData) { SecureZeroMemory(dec.pbData, dec.cbData); LocalFree(dec.pbData); }
                free(blob);
                if (pt) { *out = pt; return 1; }
                *out = ksec_dup(""); return -1;
            }
            free(blob);
        }
        *out = ksec_dup(""); return -1;   /* present blob, could not decrypt */
    }
    *out = ksec_dup(stored); return 1;     /* unmarked legacy == plaintext */
}

/* A save of an unchanged password keeps the stored value: every wrap draws a
 * fresh salt / nonce, so wrapping again would change the stored line on every
 * save. 1 when `stored` is in the form a save to this backend writes now
 * (registry or PortablePasswordProtection=dpapi: DPAPI1; master password:
 * MPW2 under this store's salt, the master password unlocked) and opens -
 * never with a prompt - to `plaintext`. Anything else answers 0 and the
 * caller wraps afresh: an unmarked or PLAIN: value, MPW1, MPW2 under another
 * salt, a locked master password, and DPAPI1 under the master-password policy,
 * which a protected save turns into MPW2. */
int ksec_stored_keeps(const char *stored, const char *plaintext, int portable)
{
    char *pt = NULL;
    int same = 0;
    if (!stored || !stored[0] || !plaintext || !plaintext[0])
        return 0;
    if (portable && g_portable_pw_mode == KITTY_PORTABLE_PW_LEGACY)
        return 0;
    if (portable && g_portable_pw_mode == KITTY_PORTABLE_PW_MASTER) {
        const char *p, *dot;
        unsigned char *salt = NULL;
        char *sb, *m1;
        int sn = 0;
        /* Unlocked here, by the same step the wrap below would take: the
         * first save of a process found the store still locked (nothing
         * had needed the key yet), declined, and the wrap that followed
         * unlocked and wrote a new ciphertext - every first save changed
         * the line, and with -masterpwfile every save did. No extra
         * prompt: the wrap prompts anyway, this only comes first; a
         * cancel leaves the store locked and the caller keeps the line. */
        if (strncmp(stored, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK)) != 0 ||
            !mpw_ensure_unlocked(0) || !g_mpw_salt_valid || !g_mpw_unprotect)
            return 0;
        p = stored + strlen(KSEC_MPW2_MARK);
        dot = strchr(p, '.');
        if (!dot || dot == p)
            return 0;
        sb = malloc((size_t)(dot - p) + 1);
        if (!sb) return 0;
        memcpy(sb, p, dot - p); sb[dot - p] = '\0';
        salt = ksec_b64_decode(sb, &sn);
        free(sb);
        if (salt && sn == KSEC_MPW_SALTLEN &&
            !memcmp(salt, g_mpw_salt, KSEC_MPW_SALTLEN)) {
            m1 = malloc(strlen(KSEC_MPW_MARK) + strlen(dot + 1) + 1);
            unsigned char k[KSEC_MPW_KEYLEN];
            if (m1 && mpw_key_get(k)) {
                sprintf(m1, "%s%s", KSEC_MPW_MARK, dot + 1);
                if (g_mpw_unprotect(m1, k, &pt) == 1 && pt)
                    same = !strcmp(pt, plaintext);
                SecureZeroMemory(k, sizeof(k));
                if (pt) { memset(pt, 0, strlen(pt)); sfree(pt); }
            }
            free(m1);
        }
        free(salt);
        return same;
    }
    if (strncmp(stored, KITTY_SECRET_DPAPI_MARK,
                strlen(KITTY_SECRET_DPAPI_MARK)) != 0)
        return 0;
    if (ksec_unprotect(stored, &pt) == 1 && pt)
        same = !strcmp(pt, plaintext);
    if (pt) { memset(pt, 0, strlen(pt)); free(pt); }
    return same;
}

/* ---- the password file handed to kscp/ksftp/klink -------------------------
 *
 * Send/Get file starts a helper with "-pwfile <path>" instead of
 * "-pw <password>", so the password is not on a command line that every
 * process lister and audit log can read. What goes IN that file is the
 * ordinary AT-REST form written and read above - "DPAPI1:" + base64 of a DPAPI
 * blob for this user - and not a format of its own.
 *
 * That is the point of it. klink, kscp and ksftp link this file, so every
 * KiTTY helper has understood this marker for as long as stored passwords have
 * been protected at rest. A format invented for the hand-off would be
 * understood only by a helper built alongside it, and the helper that actually
 * runs is whichever one the machine has - routinely an installed one, older
 * than the window starting it. That is not a theoretical risk: an earlier
 * attempt used its own marker, the installed helper took the marker and the
 * base64 for the password, and the server refused it.
 *
 * A failing protect leaves the password itself in the file, which is the plain
 * format -pwfile has always taken, and the session lists the missing feature
 * once. The file is private to the user and deleted after the start
 * (kitty_xfer.c). Declared in kitty_pwmem.h, which is where the rest of the
 * password handling lives; implemented here because the at-rest crypto is
 * here, in the settings library the helpers link. */
char *kitty_pwfile_line(const char *plain)
{
    static int reported = 0;
    char *line;

    if (!plain || !*plain)
        return NULL;
    /* Not a stored secret: a wrap that fails puts the password itself in
     * the file (above), unlike a save, which then keeps what is stored. */
    line = ksec_dpapi_protect(plain);
    if (!line)
        line = ksec_dup(plain);
    if (line && strncmp(line, KITTY_SECRET_DPAPI_MARK,
                        strlen(KITTY_SECRET_DPAPI_MARK)) != 0 && !reported) {
        /* Protection failed, so the value itself goes in the file.
         * Reported once per process. */
        reported = 1;
        kitty_api_record("crypt32.dll", "CryptProtectData",
                         KITTY_API_OPTIONAL, KT_WINFEAT_PASSWORD_FILE, 0);
    }
    return line;
}

/* Did kitty_pwfile_line() actually protect the value, or hand it back as it
 * was? The caller needs to know which, to report it and to decide what may
 * go on a command line. */
int kitty_pwfile_line_is_protected(const char *line)
{
    return line && !strncmp(line, KITTY_SECRET_DPAPI_MARK,
                            strlen(KITTY_SECRET_DPAPI_MARK)) ? 1 : 0;
}

/* The reader, NON-DESTRUCTIVE: `value` stays the caller's - it is what -pw
 * hands over, an argument string this must not free. Returns a fresh string
 * the caller frees, or NULL when a MARKED value could not be read here (a
 * blob from another account or machine). An unmarked value is the password
 * itself and comes back as a copy, which is what -pw has always done. */
char *kitty_pwfile_decode_copy(const char *value)
{
    char *pt = NULL, *res = NULL;

    if (!value)
        return NULL;
    /* The same reader a stored session password goes through, so every form
     * this tree has ever written is accepted. */
    if (ksec_unprotect(value, &pt) == 1 && pt)
        res = dupstr(pt);
    if (pt) { memset(pt, 0, strlen(pt)); free(pt); }
    return res;
}

/* The OWNING variant, for a line the caller has just read off a file and wants
 * taken over: burned and freed whatever the answer. */
char *kitty_pwfile_decode(char *line)
{
    char *res;

    if (!line)
        return NULL;
    res = kitty_pwfile_decode_copy(line);
    smemclr(line, strlen(line));
    sfree(line);
    return res;
}

/* ---- the session password handed to OUR helpers in memory (-pwhandle) ------
 * kscp/ksftp started by Send/Get file get the session's password in a file
 * mapping that only they inherit (kitty_xfer.c, through
 * kitty_create_process_with_handles): nothing on disk, nothing readable in the
 * command line, which carries only "<handle>:<size>". The password in the
 * record is wrapped with CryptProtectMemory for this logon - the helper runs
 * in it and unwraps; a dump of the pagefile does not show it. The helper
 * takes it once: the record is wiped and the handle closed. Same transport as
 * the master-key handover (kmpw_*, above). */
#define KPWH_MAGIC 0x4B505748u   /* "KPWH" */
#define KPWH_DATA  (((KITTY_PW_MAX + 1) + 15) / 16 * 16)   /* CryptProtectMemory blocks */
struct kpwh_record {
    unsigned magic, version;
    unsigned wrapped;              /* 1 = data is CryptProtectMemory'd */
    unsigned len;                  /* the password's length, without the NUL */
    unsigned char data[KPWH_DATA];
};

HANDLE kitty_pwhandle_make(const char *plain, char *tok, size_t toklen)
{
    struct kpwh_record *r;
    size_t len = plain ? strlen(plain) : 0;
    HANDLE fm;
    void *p;

    if (tok && toklen)
        tok[0] = '\0';
    if (len == 0 || len > KITTY_PW_MAX || !tok || toklen < 24)
        return NULL;
    r = snew(struct kpwh_record);
    memset(r, 0, sizeof(*r));
    r->magic = KPWH_MAGIC;
    r->version = 1;
    r->len = (unsigned)len;
    memcpy(r->data, plain, len);
    r->wrapped = kmpw_mem("CryptProtectMemory", r->data, KPWH_DATA);
    fm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                            sizeof(*r), NULL);
    if (!fm) {
        smemclr(r, sizeof(*r));
        sfree(r);
        return NULL;
    }
    p = MapViewOfFile(fm, FILE_MAP_WRITE, 0, 0, sizeof(*r));
    if (!p) {
        CloseHandle(fm);
        smemclr(r, sizeof(*r));
        sfree(r);
        return NULL;
    }
    memcpy(p, r, sizeof(*r));
    UnmapViewOfFile(p);
    smemclr(r, sizeof(*r));
    sfree(r);
    snprintf(tok, toklen, "%p:%u", fm, (unsigned)sizeof(struct kpwh_record));
    return fm;
}

char *kitty_pwhandle_take(const char *value)
{
    struct kpwh_record *r;
    HANDLE fm = NULL;
    unsigned sz = 0;
    void *p;
    char *res = NULL;

    if (!value || sscanf(value, "%p:%u", &fm, &sz) != 2 || !fm ||
        sz != sizeof(struct kpwh_record))
        return NULL;
    p = MapViewOfFile(fm, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sz);
    if (!p) {
        CloseHandle(fm);
        return NULL;
    }
    r = snew(struct kpwh_record);
    memcpy(r, p, sizeof(*r));
    SecureZeroMemory(p, sz);       /* taken once: nothing is left behind */
    UnmapViewOfFile(p);
    CloseHandle(fm);
    if (r->magic == KPWH_MAGIC && r->version == 1 && r->len > 0 &&
        r->len <= KITTY_PW_MAX &&
        (!r->wrapped || kmpw_mem("CryptUnprotectMemory", r->data, KPWH_DATA))) {
        res = snewn(r->len + 1, char);
        memcpy(res, r->data, r->len);
        res[r->len] = '\0';
    }
    smemclr(r, sizeof(*r));
    sfree(r);
    return res;
}

/* ---- does the active store hold anything wrapped with the master password? --
 * Answered by a scan of the sessions and proxies, in whichever backend is in
 * use. kitty_migrate_portable_mpw_state() checks before it copies any
 * master-password state into a portable store: with nothing wrapped, the
 * store mints its own. (A startup repair that removed an orphan salt and
 * verifier pair - the leftover of the 0.84.1.48-0.84.1.65 export behaviour,
 * which minted a master password as a side effect - also checked here; it was
 * retired once every install that could have carried the leftover had been
 * started under a build that cleaned it.)
 */
static int ksec_value_is_mpw(const char *v)
{
    return v && (!strncmp(v, KSEC_MPW_MARK,  strlen(KSEC_MPW_MARK)) ||
                 !strncmp(v, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK)));
}
/* For the folder import: is this stored value under a master password?
 * 2 = MPW2 (self-contained: the salt is embedded, so THAT store's master
 * password unlocks it here), 1 = MPW1 (needs its store's salt - not
 * decodable from a foreign folder), 0 = no. */
int kitty_secret_is_mpw(const char *stored)
{
    if (!stored) return 0;
    if (!strncmp(stored, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK))) return 2;
    if (!strncmp(stored, KSEC_MPW_MARK, strlen(KSEC_MPW_MARK))) return 1;
    return 0;
}

/* Registry: every session (and named proxy) is a subkey holding at most two
 * secret values. */
static int reg_subtree_has_mpw(const char *subpath)
{
    static const char *const secrets[] = { KR_PASSWORD, "ProxyPassword" };
    char path[2048];
    HKEY h;
    DWORD i;
    int found = 0;
    snprintf(path, sizeof(path), "%s%s", reg_base_buf, subpath);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return 0;
    for (i = 0; !found; i++) {
        char name[512], full[2600];
        DWORD nl = sizeof(name);
        unsigned k;
        if (RegEnumKeyExA(h, i, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        snprintf(full, sizeof(full), "%s\\%s", path, name);
        for (k = 0; k < lenof(secrets); k++) {
            char v[4096];
            DWORD sz = sizeof(v);
            if (RegGetValueA(HKEY_CURRENT_USER, full, secrets[k], RRF_RT_REG_SZ,
                             NULL, v, &sz) == ERROR_SUCCESS &&
                ksec_value_is_mpw(v)) {
                found = 1;
                break;
            }
        }
    }
    RegCloseKey(h);
    return found;
}

/* Portable: session and proxy files are small key/value text. Values are munged
 * on the way out, so look for both spellings of the marker. */
static int ksf_text_has_mpw(const char *path)
{
    static const char *const marks[] = {
        "MPW1:", "MPW1%3A", "MPW2:", "MPW2%3A"
    };
    FILE *fp = fopen(path, "rb");
    long len;
    size_t got;
    char *buf;
    unsigned i;
    int found = 0;
    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    len = ftell(fp);
    if (len < 0 || len > 4L * 1024 * 1024) { fclose(fp); return 0; }
    rewind(fp);
    buf = snewn((size_t)len + 1, char);
    got = fread(buf, 1, (size_t)len, fp);
    buf[got] = '\0';
    fclose(fp);
    for (i = 0; i < lenof(marks) && !found; i++)
        if (strstr(buf, marks[i])) found = 1;
    smemclr(buf, (size_t)len + 1);
    sfree(buf);
    return found;
}

static int ksf_dir_has_mpw_depth(const char *dir, int depth)
{
    char *pat;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int found = 0;
    if (!dir) return 0;
    pat = dupprintf("%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    sfree(pat);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char *path;
        /* KiTTY (hknet/KiTTY#55): sessions live in nested folders now, so
         * the folders are searched too - not the dot ones (.git), and no
         * junction is followed. */
        if (fd.cFileName[0] == '.' ||
            ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
             (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)))
            continue;
        path = dupprintf("%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            found = depth < 24 && ksf_dir_has_mpw_depth(path, depth + 1);
        else
            found = ksf_text_has_mpw(path);
        sfree(path);
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static int ksf_dir_has_mpw(const char *dir)
{
    return ksf_dir_has_mpw_depth(dir, 0);
}

static int store_has_mpw_wrapped_secret(void)
{
    if (store_is_file()) {
        char *prox;
        int found = ksf_dir_has_mpw(kitty_session_dir());
        if (found) return 1;
        prox = portable_proxies_dir();     /* in configdir, as kitty_proxy.c */
        found = ksf_dir_has_mpw(prox);
        sfree(prox);
        return found;
    }
    return reg_subtree_has_mpw("\\Sessions") || reg_subtree_has_mpw("\\Proxies");
}

/* Master-password state that an earlier build moved out of the live names
 * (the startup repair of an orphan pair archived rather than deleted in the
 * hive; nothing writes these names any more). Salt and verifier are check
 * material, not the secret: the salt is a public random input to Argon2id and
 * the verifier only proves a typed password is the right one. They are still
 * READ, because a portable store that depends on the archived salt can be
 * repaired from them (kitty_migrate_portable_mpw_state) - deleting the salt
 * would make MPW1 values permanently unopenable. */
#define KSEC_MPW_RETIRED_SALT "RetiredMasterPwSalt"
#define KSEC_MPW_RETIRED_VER  "RetiredMasterPwVerifier"

/* Read a value from our registry base. 1 + filled buf, or 0. */
static int reg_base_read(const char *name, char *buf, DWORD bufsz)
{
    DWORD sz = bufsz;
    return RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, name, RRF_RT_REG_SZ,
                        NULL, buf, &sz) == ERROR_SUCCESS;
}

/* Prove that an archived salt+verifier really is this store's master-password
 * state, by prompting for the password and checking it, before writing
 * anything. On success the derived key is kept as the unlocked key, so the
 * user gets one prompt here and none from the first unlock that follows.
 * salt_b64/ver are the archived values; returns 1 if they were adopted. */
static int mpw_adopt_retired_state(const char *salt_b64, const char *ver)
{
    unsigned char salt[KSEC_MPW_SALTLEN];
    unsigned char key[KSEC_MPW_KEYLEN];
    unsigned char *d;
    int n = 0, tries, ok = 0, attempt = 0;

    if (!g_mpw_derive || !g_mpw_unprotect) return 0;
    d = ksec_b64_decode(salt_b64, &n);
    if (!d || n != KSEC_MPW_SALTLEN) { if (d) free(d); return 0; }
    memcpy(salt, d, KSEC_MPW_SALTLEN);
    free(d);

    /* Same retry policy as a normal unlock: one shot for a supplied passphrase,
     * three for an interactive prompt. */
    tries = g_mpw_passphrase ? 1 : KSEC_MPW_TRIES;
    while (tries-- > 0 && !ok) {
        char *vpt = NULL;
        char *pass = g_mpw_passphrase ? mpw_supplied_dup()
                   : mpw_prompt_call(0, ++attempt);
        if (!pass || !pass[0]) {                   /* cancelled, or no prompt */
            if (pass) { memset(pass, 0, strlen(pass)); free(pass); }
            break;
        }
        g_mpw_derive(pass, salt, KSEC_MPW_SALTLEN, key);
        memset(pass, 0, strlen(pass)); free(pass);
        ok = (g_mpw_unprotect(ver, key, &vpt) == 1 && vpt &&
              !strcmp(vpt, KSEC_MPW_VERIFY));
        if (vpt) { memset(vpt, 0, strlen(vpt)); sfree(vpt); }
        if (!ok) SecureZeroMemory(key, sizeof(key));
    }
    if (!ok) {
        kitty_pwdebug("portable MPW migrate: archived state did not verify, not adopted");
        return 0;
    }
    if (!portable_write_text_file(KSEC_MPW_SUBDIR, "MasterPwSalt", salt_b64)) {
        kitty_pwdebug("portable MPW migrate: verified, but Security\\ is not writable");
        SecureZeroMemory(key, sizeof(key));
        return 0;
    }
    portable_write_text_file(KSEC_MPW_SUBDIR, "MasterPwVerifier", ver);
    mpw_slot_put(&g_mpw_key, key);
    SecureZeroMemory(key, sizeof(key));
    memcpy(g_mpw_salt, salt, KSEC_MPW_SALTLEN);
    g_mpw_salt_valid = 1;
    g_mpw_unlocked = 1;                 /* prompted once, here - not again */
    kitty_pwdebug("portable MPW migrate: adopted archived master-password state");
    return 1;
}

/* ---- one-shot: give a portable store its own master-password state --------
 * A portable store whose passwords were wrapped before this build may have no
 * Security\ folder of its own, because the salt was read through to the registry
 * on every unlock. That read-through is gone (see mpw_state_get), so the state
 * has to be COPIED once, or those passwords would stop opening.
 *
 * Copy only when this store actually has something wrapped with it. A store with
 * no MPW values needs no salt at all, and must be left free to mint its own.
 *
 * Returns 1 when state was copied - the caller warns the user, because the copy
 * only fixes THIS store: another portable KiTTY on the same machine, wrapped
 * with the same hive salt, needs the Security\ folder copied across too, and
 * only the user knows where those live.
 */
int kitty_migrate_portable_mpw_state(void)
{
    char salt[2048], ver[2048];
    const DWORD ssz = sizeof(salt), vsz = sizeof(ver);
    char *own;

    if (!store_is_file()) return 0;
    if (kitty_store_readonly()) return 0;   /* readonly=yes: a later start copies it */

    /* Already self-contained? */
    own = portable_read_text_file(KSEC_MPW_SUBDIR, "MasterPwSalt");
    if (own) { sfree(own); return 0; }

    /* Nothing wrapped here -> no salt is needed, and copying one in would
     * recreate exactly the inheritance this change removes. */
    if (!store_has_mpw_wrapped_secret()) {
        kitty_pwdebug("portable MPW migrate: nothing wrapped, store mints its own");
        return 0;
    }

    /* Live names are the store's own former state, read through the fallback
     * that used to exist: copying those back in is exactly the status quo, so
     * they are taken as-is. The RETIRED names are different - they were archived
     * because some store stopped using them, and there is no guarantee they
     * belong to THIS one. Writing an unverified salt+verifier into Security\
     * would hand the user a store that looks protected and cannot be opened, so
     * the retired path is proved with the master password before it is kept. */
    if (reg_base_read("MasterPwSalt", salt, ssz)) {
        if (!portable_write_text_file(KSEC_MPW_SUBDIR, "MasterPwSalt", salt)) {
            /* Read-only media, or no permission. Nothing is lost by not
             * copying: the store keeps working until something needs the salt. */
            kitty_pwdebug("portable MPW migrate: could not write Security\\MasterPwSalt");
            return 0;
        }
        if (reg_base_read("MasterPwVerifier", ver, vsz))
            portable_write_text_file(KSEC_MPW_SUBDIR, "MasterPwVerifier", ver);
        kitty_pwdebug("portable MPW migrate: copied master-password state into the store");
        return 1;
    }
    if (!reg_base_read(KSEC_MPW_RETIRED_SALT, salt, ssz) ||
        !reg_base_read(KSEC_MPW_RETIRED_VER, ver, vsz)) {
        /* Wrapped values but no salt anywhere we can see. Do not guess and do
         * not delete anything - record it in the log and leave it alone. */
        kitty_pwdebug("portable MPW migrate: wrapped values but no salt in the hive");
        return 0;
    }
    if (!mpw_adopt_retired_state(salt, ver)) return 0;
    return 1;
}

/* ---- .ktx forced-export glue (kitty_settings_forced.c / kitty_settings_load.c
 * and the bulk export/import in kitty_bridge.c). The forced serializer is a
 * SECOND portable-file write path that bypasses the write_setting_s chokepoint,
 * so it must apply the same backend password policy: wrap = portable policy
 * (MPW1 / DPAPI1 fallback / explicit-legacy plain), unwrap = stored-marker
 * dispatch. Session-name <-> filename munging is exposed alongside so bulk
 * export/import can round-trip arbitrary session names. Wrap/unwrap results
 * are malloc'd (free()); the munge results are snewn'd (sfree()). */
int ksec_stored_is_legacy(const char *stored);
/* An export bundle's wrap. 0: no bundle, the caller's policy applies. 1: a
 * bundle - *out is the wrapped value (malloc'd; "" for no password), or NULL
 * when the wrap failed, with the reason in *why.
 *
 * Export bundle in progress: protect with the bundle's OWN passphrase and
 * leave the master password alone entirely - no setup dialog, no
 * MasterPwSalt/MasterPwVerifier written, no Security\ folder created in a
 * portable tree. This is the whole point of the bundle password, and it is
 * why the hook sits here: both the session export (save_open_settings_forced)
 * and the named-proxy export (kitty_export_proxies_to_dir) come through this
 * one function, so a single bundle password covers both.
 *
 * A wrap that fails (the MPW crypto not linked into this program, a CSPRNG or
 * memory failure, DPAPI refusing) writes NO password for that entry: the rest
 * of the export goes on and the summary lists it (kitty_bundle_wrap_report).
 * It used to fall back to DPAPI - and a failed DPAPI wrap to the plaintext -
 * which made a password-protected bundle this-PC-only behind the user's back:
 * DPAPI is written only when the user chose it ("this PC only"). */
static int ksec_wrap_bundle(const char *plaintext, char **out, const char **why)
{
    static char dpapi_why[600];
    int test_fail = 0;
    *out = NULL;
    *why = NULL;
    if (g_bundle_import || (!g_bundle_pass && !g_bundle_dpapi))
        return 0;                       /* not a bundle: the caller's policy */
    if (!plaintext || !plaintext[0]) {
        *out = ksec_dup("");
        return 1;
    }
#ifdef KITTY_TEST_BUILD_LABEL
    /* Test builds only: KITTY_TEST_WRAP_FAIL=1 fails every bundle wrap, as a
     * CSPRNG or memory failure would. Nothing else reaches this path in a
     * test. */
    {
        char tv[8];
        DWORD tn = GetEnvironmentVariableA("KITTY_TEST_WRAP_FAIL", tv, sizeof(tv));
        test_fail = (tn && tn < sizeof(tv) && tv[0] == '1');
    }
#endif
    if (g_bundle_pass) {
        if (!g_mpw_derive || !g_mpw_protect || !g_mpw_randsalt)
            *why = KT_BUNDLE_WRAP_NOCRYPTO;
        else if (!test_fail)
            *out = ksec_wrap_with_passphrase(plaintext, g_bundle_pass);
        if (!*out && !*why)
            *why = KT_BUNDLE_WRAP_FAILED;
    } else {
        /* "This PC only", the user's choice: DPAPI directly, never the
         * portable policy, so no master-password prompt can appear on an
         * export. */
        DWORD err = 0;
        if (!test_fail) {
            *out = ksec_dpapi_protect(plaintext);
            err = GetLastError();
        }
        if (!*out) {
            if (test_fail || !err)
                *why = KT_BUNDLE_WRAP_FAILED;
            else {
                snprintf(dpapi_why, sizeof(dpapi_why), KT_BUNDLE_WRAP_DPAPI_FMT,
                         kitty_win_reason(err));
                *why = dpapi_why;
            }
        }
    }
    return 1;
}
/* An export's wrap: the .ktx sessions and the named proxies of a bundle, and
 * a single .ktx. In a bundle: the bundle's protection, and a failed wrap
 * gives NULL and *skipped = 1 - no password is written for `where`, the
 * summary lists it. Outside one: the portable policy, and a refused wrap - the
 * master password locked (no DPAPI value nobody chose), DPAPI failing (never
 * the plaintext) - gives NULL and *locked = 1. */
char *kitty_secret_wrap_export(const char *plaintext, int what, const char *where,
                               int *locked, int *skipped)
{
    char *res;
    const char *why;
    if (locked) *locked = 0;
    if (skipped) *skipped = 0;
    if (ksec_wrap_bundle(plaintext, &res, &why)) {
        if (!res) {
            ksec_bundle_note_skip(what, where, why);
            if (skipped) *skipped = 1;
        }
        return res;
    }
    return ksec_protect_portable_kept(plaintext ? plaintext : "", locked);
}
/*
 * The login script's two shapes, with no crypto in sight.
 *
 * STORED it is a NUL-separated blob - expect\0send\0expect\0send\0\0 - which is
 * what ManageInitScript walks. SHOWN to a person it is one entry per line, which
 * is exactly the format of the script FILE it came from, so the config box gives
 * back what the user originally wrote.
 *
 * These live here, apart from the protection that wraps them, for one reason:
 * this is where a mistake silently eats somebody's login script, so it has to be
 * somewhere the tests can reach. The crypto half stays in kitty.c with the legacy
 * decode it depends on.
 */
char *kitty_loginscript_blob_to_lines(const unsigned char *blob, int len)
{
    strbuf *sb;
    int i = 0;
    if (!blob || len <= 0) return ksec_dup("");
    sb = strbuf_new_nm();
    while (i < len && blob[i]) {
        const char *e = (const char *)blob + i;
        size_t n = strnlen(e, (size_t)(len - i));
        if (sb->len) put_dataz(sb, "\r\n");
        put_data(sb, e, n);
        i += (int)n + 1;
    }
    return strbuf_to_str(sb);
}

/* text -> blob. *outlen gets the byte count INCLUDING the terminating empty
 * entry. Caller frees with sfree. Empty lines are dropped, matching how the
 * script file is read - a blank line there would become an entry that matches
 * everything. */
unsigned char *kitty_loginscript_lines_to_blob(const char *text, int *outlen)
{
    strbuf *raw = strbuf_new_nm();
    const char *p = text ? text : "";
    unsigned char *out;
    while (*p) {
        const char *nl = p;
        while (*nl && *nl != '\r' && *nl != '\n') nl++;
        if (nl > p) { put_data(raw, p, nl - p); put_byte(raw, 0); }
        while (*nl == '\r' || *nl == '\n') nl++;
        p = nl;
    }
    put_byte(raw, 0);
    *outlen = (int)raw->len;
    out = (unsigned char *)strbuf_to_str(raw);
    return out;
}

/*
 * The login script in memory, for the length of a connection
 * (ScriptFileContent, kitty.c). It holds what is typed at the login prompts,
 * so it is kept like a password in a Conf: wrapped with CryptProtectMemory
 * for this process (kitty_pw_wrap_bytes, "{kpw1}") and opened only by the
 * matcher, into a copy wiped straight after the compare. Where that
 * protection is unavailable it is held as the plain blob, as before - the
 * rule the password helpers follow. Here rather than in kitty.c so the tests
 * reach it.
 */
/* The blob's length up to and including its terminating empty entry, or max
 * when it has none within max bytes. */
static size_t ksl_blob_len(const unsigned char *b, size_t max)
{
    size_t i = 0;
    while (i < max && b[i]) {
        while (i < max && b[i])
            i++;
        i++;                            /* the entry's NUL */
    }
    return i < max ? i + 1 : max;
}
char *kitty_loginscript_mem_wrap(const unsigned char *blob, size_t len)
{
    size_t n;
    char *held;
    if (!blob || len == 0 || !blob[0])
        return NULL;                    /* no script, or one used up */
    n = ksl_blob_len(blob, len);
    held = kitty_pw_wrap_bytes(blob, n);
    if (held)
        return held;
    held = snewn(n + 2, char);          /* plain, ending in an empty entry */
    memcpy(held, blob, n);
    held[n] = held[n + 1] = '\0';
    return held;
}
unsigned char *kitty_loginscript_mem_open(const char *held, size_t *len)
{
    unsigned char *out;
    size_t n = 0;
    if (len) *len = 0;
    if (!held || !held[0])
        return NULL;
    if (kitty_pw_is_wrapped(held)) {
        out = kitty_pw_unwrap_bytes(held, &n, 1);   /* n + 2 zero bytes */
        if (!out)
            return NULL;
    } else {
        n = ksl_blob_len((const unsigned char *)held, (size_t)-1);
        out = snewn(n + 2, unsigned char);
        memcpy(out, held, n);
        out[n] = out[n + 1] = 0;
    }
    if (len) *len = n;
    return out;
}
char *kitty_loginscript_mem_next(const unsigned char *blob, size_t len)
{
    size_t l;
    if (!blob || len == 0 || !blob[0])
        return NULL;
    /* the open copy ends in two zero bytes: both strlens stay inside it */
    l = strlen((const char *)blob) + 1;
    if (l < len)
        l += strlen((const char *)blob + l) + 1;
    if (l >= len)
        return NULL;
    return kitty_loginscript_mem_wrap(blob + l, len - l);
}
void kitty_loginscript_mem_free(char *held)
{
    if (!held)
        return;
    smemclr(held, kitty_pw_is_wrapped(held) ? strlen(held)
            : ksl_blob_len((const unsigned char *)held, (size_t)-1));
    sfree(held);
}

/* Wrap for whatever backend is active now (registry -> DPAPI1, portable ->
 * MPW/DPAPI/legacy), for stores that live outside the write_setting_s
 * chokepoint (e.g. named proxies). Malloc'd; free() the result. NULL when the
 * wrap was refused (the master password locked, DPAPI failing): nothing to
 * store, never DPAPI or the plaintext in its place. */
char *kitty_secret_wrap_current_backend(const char *plaintext)
{
    return store_is_file() ? ksec_protect_portable(plaintext ? plaintext : "")
                           : ksec_protect_registry(plaintext ? plaintext : "");
}
/* The same, kept: NULL and *locked = 1 when the wrap was refused (the caller
 * keeps what is stored and reports it with ksec_report_locked_save). */
char *kitty_secret_wrap_current_backend_kept(const char *plaintext, int *locked)
{
    if (locked) *locked = 0;
    return store_is_file()
        ? ksec_protect_portable_kept(plaintext ? plaintext : "", locked)
        : ksec_protect_registry_kept(plaintext ? plaintext : "", locked);
}
/* True when portable at-rest protection is the explicit legacy/plaintext hatch
 * (kitty.ini PortablePasswordProtection=legacy): callers then keep passwords
 * plaintext deliberately and must NOT nag about it. */
int kitty_portable_password_legacy(void)
{
    return g_portable_pw_mode == KITTY_PORTABLE_PW_LEGACY;
}
/* Past the PLAIN: provisioning marker if the value carries one, else the value
 * itself. Borrowed pointer, never NULL for a non-NULL argument. For the few
 * write-side callers that re-protect a raw stored value without reading it
 * through ksec_unprotect() first, so the marker is not wrapped up as part of
 * the password. */
const char *kitty_secret_strip_plain(const char *stored)
{
    if (stored && !strncmp(stored, KITTY_SECRET_PLAIN_MARK,
                           strlen(KITTY_SECRET_PLAIN_MARK)))
        return stored + strlen(KITTY_SECRET_PLAIN_MARK);
    return stored;
}
int kitty_secret_is_marked(const char *stored)
{
    return stored && !ksec_stored_is_legacy(stored) && stored[0];
}
int kitty_secret_unwrap(const char *stored, char **out)
{
    return ksec_unprotect(stored, out);
}
char *kitty_session_fname_munge(const char *name)
{
    return ksf_munge(name ? name : "");
}
char *kitty_session_fname_unmunge(const char *name)
{
    return ksf_unmunge(name ? name : "");
}

/* never-wipe guard: keep an undecryptable-here blob so the next save re-persists it. */
static char *g_ksec_orig[2] = { NULL, NULL };
void ksec_after_load(int slot, const char *stored, int rv)
{
    if (slot < 0 || slot > 1) return;
    if (g_ksec_orig[slot]) { free(g_ksec_orig[slot]); g_ksec_orig[slot] = NULL; }
    if (rv < 0 && stored && stored[0]) g_ksec_orig[slot] = ksec_dup(stored);
}

/* Legacy->protected migration consent (portable files only). Before a
 * portable save rewrites a password stored in the old unprotected
 * (unmarked) form into a protected one, the GUI-registered warner puts up
 * one question per save: 1 = re-encrypt, 0 = keep the stored value verbatim.
 * CLI tools never register a warner, so batch saves keep the old form. */
static int (*g_ksec_migrate_warn)(void) = NULL;
void kitty_set_legacy_migrate_warn(int (*fn)(void)) { g_ksec_migrate_warn = fn; }
int ksec_stored_is_legacy(const char *stored)
{
    return stored && stored[0] &&
        strncmp(stored, KITTY_SECRET_DPAPI_MARK,
                strlen(KITTY_SECRET_DPAPI_MARK)) != 0 &&
        strncmp(stored, KSEC_MPW_MARK, strlen(KSEC_MPW_MARK)) != 0 &&
        strncmp(stored, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK)) != 0;
}
/* ---- legacy (<=0.76 old-KiTTY) password decrypt. Applied ONLY where a value
 * is guaranteed to be old-KiTTY-written: the old 9bis hive (read_setting_s)
 * and cyd01-syntax portable session files (ksf_load format conversion). Old
 * KiTTY stored "Password" as bcrypt_base64(plaintext) - plus a MASKPASS XOR
 * layer in some configurations - keyed on host+termtype+"KiTTY" (dopasskey
 * mode 0) or the fixed key "KiTTY" (mode >0). bcrypt is unauthenticated so we
 * must NOT apply this anywhere we might have written cleartext (our own hive,
 * our own ksf files, or the PuTTY hive via KiClassName=PuTTY). ---- */
#include "../kitty/bcrypt/nbcrypt.h"   /* buncrypt_string_base64, bcrypt_init (relative: storage.c is built standalone in some targets) */
/* exact bytes of kitty_crypt.c's MASKKEY ("\xc2\xa4..\xc2\xbe", UTF-8, 16 bytes) */
static const unsigned char ksec_maskkey[16] = {
    0xC2,0xA4,0xC2,0xA5,0xC2,0xA9,0xC2,0xAA,
    0xC2,0xB3,0xC2,0xBC,0xC2,0xBD,0xC2,0xBE
};
static void ksec_maskpass(char *s)   /* exact replica of kitty_crypt.c MASKPASS */
{
    int i, j = 0, len = (int)strlen(s);
    char *buf = malloc(len + 1), c;
    if (!buf) return;
    buf[0] = '\0';
    for (i = 0; i < len; i++) {
        c = s[i] ^ (char)ksec_maskkey[j];
        if (c == 0) { free(buf); return; }     /* original aborts (leaves s unchanged) */
        buf[i] = c; buf[i+1] = '\0';
        j++; if (j >= 16) j = 0;
    }
    strcpy(s, buf);
    memset(buf, 0, strlen(s));
    free(buf);
}
/* True only if every byte is printable ASCII (0x20..0x7e). Used to
 * distinguish a clean plaintext password from MASKPASS XOR output, which
 * leaves high-bit/control bytes. */
static int ksec_all_printable(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7e) return 0;
    }
    return 1;
}
/* Weaker sanity check for the last-resort path: no control bytes, but high-bit
 * (ANSI/UTF-8) bytes allowed - a legacy NON-ASCII password decodes to these. */
static int ksec_no_ctrl(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++)
        if ((unsigned char)*s < 0x20) return 0;
    return 1;
}
/* One buncrypt attempt with one key. cyd01 saved-session passwords are
 * bcrypt(plaintext): buncrypt ALONE yields the plaintext (verified against
 * real cyd01 0.76 registry AND portable session files). Only the rarer cyd01
 * 'cryptsalt' configuration adds the MASKPASS XOR layer, which leaves
 * high-bit/non-printable bytes. So apply MASKPASS ONLY when it actually turns
 * the result printable. (The pre-0.84.1.44 code MASKPASSed unconditionally,
 * corrupting every legacy password it touched.) Returns a malloc'd result
 * that passed the printable test, or NULL; the raw buncrypt output is handed
 * to *raw0 (once) so the caller can fall back to it. */
static char *ksec_try_legacy_key(const char *stored, const char *passkey,
                                 char **raw0)
{
    char *out = malloc(strlen(stored) + 16);
    int r;
    if (!out) return NULL;
    r = buncrypt_string_base64(stored, out, (unsigned)strlen(stored), passkey);
    if (r <= 0) { free(out); return NULL; }
    out[r] = '\0';            /* buncrypt returns the decoded length */
    if (ksec_all_printable(out)) return out;
    char *u = malloc(strlen(out) + 1);
    if (u) {
        strcpy(u, out);
        ksec_maskpass(u);
        if (ksec_all_printable(u)) {
            memset(out, 0, strlen(out)); free(out);
            return u;
        }
        free(u);
    }
    if (raw0 && !*raw0)
        *raw0 = out;          /* keep for the caller's last-resort fallback */
    else {
        memset(out, 0, strlen(out)); free(out);
    }
    return NULL;
}
/* Does `stored` carry the legacy format's header?
 *
 * bcrypt_string_base64 emits a 5-character header before the payload, and its
 * characters come from a tiny fixed alphabet - measured over 480,000
 * ciphertexts spanning 4 keys, 4 bcrypt_init seeds and plaintext lengths 0-200:
 * positions 0-2 are always one of "0123456bnv" and positions 3-4 one of
 * "0123bnpvx", with NOT ONE exception. The header does not depend on the key,
 * which is what makes it usable as a format test.
 *
 * This matters because buncrypt is unauthenticated: it cheerfully "decrypts"
 * arbitrary text into short garbage, and roughly 6% of ordinary alphanumeric
 * passwords decode to something that passes the printable test and would be
 * silently accepted as a decoded legacy value. Gating on the header cuts that
 * to 2 in 500,000 (measured over the same corpus) while rejecting none of the
 * 480,000 genuine values - i.e. it costs no read compatibility at all.
 *
 * A too-short value cannot be legacy either: the header alone is 5 characters
 * (an empty plaintext encodes to exactly 5). */
static int ksec_has_legacy_header(const char *stored)
{
    static const char *hdr012 = "0123456bnv";
    static const char *hdr34  = "0123bnpvx";
    int i;
    if (!stored || strlen(stored) < 5) return 0;
    for (i = 0; i < 3; i++) if (!strchr(hdr012, stored[i])) return 0;
    for (i = 3; i < 5; i++) if (!strchr(hdr34,  stored[i])) return 0;
    return 1;
}
/* Backend-neutral decoder core (host/term supplied by the caller: registry
 * wrapper below, cyd01 file conversion in ksf_load). Key order = most-specific
 * first; returns malloc'd plaintext or NULL (caller preserves stored bytes). */
char *ksec_legacy_decrypt_hostterm(const char *stored, const char *host,
                                          const char *term)
{
    static int inited = 0;
    char passkey[1100];
    char *pt, *raw0 = NULL;
    if (!stored || !stored[0]) return NULL;
    if (!ksec_has_legacy_header(stored)) return NULL;   /* not the legacy format */
    if (!inited) { bcrypt_init(0); inited = 1; }
    /* dopasskey() mode 0: host + termtype + "KiTTY" (termtype default "xterm"). */
    snprintf(passkey, sizeof(passkey), "%s%sKiTTY",
             host ? host : "", (term && term[0]) ? term : "xterm");
    pt = ksec_try_legacy_key(stored, passkey, &raw0);
    /* dopasskey() mode >0 fallback: the fixed key "KiTTY" (alternate old
     * configurations; the legacy-import fallback). */
    if (!pt)
        pt = ksec_try_legacy_key(stored, "KiTTY", NULL);
    /* Last resort: a legacy non-ASCII password fails the strict printable
     * test; accept the host-keyed output if it at least has no control bytes,
     * else give up so the caller preserves the stored value verbatim. */
    if (!pt && raw0 && ksec_no_ctrl(raw0)) {
        pt = raw0;
        raw0 = NULL;
    }
    if (raw0) { memset(raw0, 0, strlen(raw0)); free(raw0); }
    return pt;
}
/* Decode a password read from an IMPORTED .ktx, where the value's provenance is
 * unknown. Four cases, in this order because the deterministic ones must win:
 *
 *   PLAIN:<pw>   provisioning marker - taken literally, never guessed at. This
 *                is the supported way for a rollout script to ship a password.
 *   DPAPI1: /    our own protection - unwrapped; if it cannot be unwrapped here
 *   MPW1: MPW2:  (wrong PC/account, no master password) the runtime password is
 *                empty, and the .ktx is not rewritten, so nothing is lost.
 *   unmarked     could be old-KiTTY bcrypt+base64 OR simply cleartext. Attempt
 *                the legacy decode and accept it only if it yields a clean
 *                printable plaintext; otherwise take the value literally.
 *
 * That last fallback is the fix: the .ktx loader used to run every unmarked
 * value through decryptpassword + an UNCONDITIONAL MASKPASS, which silently
 * mangled cleartext into garbage (and, per ksec_try_legacy_key above, corrupted
 * genuine legacy values too in the common non-cryptsalt configuration).
 *
 * The unmarked case is a guess, but a well-constrained one: it requires the
 * legacy header (ksec_has_legacy_header) AND a successful decode AND printable
 * output. Measured against the real bcrypt library, that mistakes
 * cleartext for legacy about twice per 500,000 passwords, against roughly 6%
 * for the decode+printable test alone, and it rejects none of 480,000 genuine
 * legacy values. PLAIN: remains the deterministic answer for anyone who needs
 * certainty rather than very good odds.
 *
 * try_legacy=0 suppresses the guess entirely, for fields old KiTTY never
 * encrypted (ProxyPassword): there is no legacy form to find, so guessing could
 * only corrupt a good value.
 *
 * Returns malloc'd plaintext (caller frees), or NULL for empty input. */
char *kitty_secret_decode_imported(const char *stored, const char *host,
                                   const char *term, int try_legacy)
{
    char *pt;
    if (!stored || !stored[0]) return NULL;
    if (!strncmp(stored, KITTY_SECRET_PLAIN_MARK,
                 strlen(KITTY_SECRET_PLAIN_MARK)))
        return ksec_dup(stored + strlen(KITTY_SECRET_PLAIN_MARK));
    if (kitty_secret_is_marked(stored)) {
        pt = NULL;
        if (ksec_unprotect(stored, &pt) > 0 && pt) return pt;
        if (pt) { memset(pt, 0, strlen(pt)); free(pt); }
        return ksec_dup("");
    }
    if (!try_legacy) return ksec_dup(stored);
    pt = ksec_legacy_decrypt_hostterm(stored, host, term);
    return pt ? pt : ksec_dup(stored);
}

char *ksec_legacy_decrypt(const char *stored, HKEY sesskey)
{
    char *host = get_reg_sz(sesskey, "HostName");
    char *term = get_reg_sz(sesskey, "TerminalType");
    char *pt = ksec_legacy_decrypt_hostterm(stored, host, term);
    sfree(host);
    sfree(term);
    return pt;
}

/* Normalise the auto-login password to UTF-8 (the SSH password prompt is UTF-8).
 * A value that's already valid UTF-8 (ASCII included) is left untouched; a legacy
 * value in the system codepage (older KiTTY stored it via GetWindowTextA) is
 * converted CP_ACP -> UTF-8 so it matches what typing it would send. Takes
 * ownership of s (malloc'd); returns a malloc'd result. Idempotent. */
char *ksec_to_utf8(char *s)
{
    if (!s || !s[0]) return s;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) > 0)
        return s;                                  /* already valid UTF-8 */
    int wn = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (wn <= 0) return s;
    wchar_t *w = (wchar_t *)malloc(wn * sizeof(wchar_t));
    if (!w) return s;
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u = (char *)malloc(un > 0 ? un : 1);
    if (u) WideCharToMultiByte(CP_UTF8, 0, w, -1, u, un, NULL, NULL);
    free(w);
    if (!u) return s;
    memset(s, 0, strlen(s)); free(s);
    return u;
}

/* never-wipe guard: the original stored blob for write_setting_s to
 * re-persist when the in-memory secret is empty because THIS session
 * could not decrypt it (NULL when there is nothing to preserve). */
const char *ksec_orig_get(int slot)
{
    return (slot >= 0 && slot <= 1) ? g_ksec_orig[slot] : NULL;
}

/* legacy->protected migration consent (0 = keep stored form; CLI tools
 * never register a warner, so batch saves keep the old form). */
int ksec_migrate_warn_ask(void)
{
    return g_ksec_migrate_warn ? g_ksec_migrate_warn() : 0;
}
