/*
 * kitty_hostkey_scan.c: klink -scan and klink -knownhosts. See the header.
 *
 * The scan seat answers nothing: the key arrives through the capture point
 * in verify_ssh_host_key() (armed per connection), which ends the connection
 * before any authentication. Weak-crypto questions are waved through - the
 * scan wants to SEE the key, whatever the server's taste in ciphers - and a
 * fatal error is recorded, not printed and exited on, so the next type can
 * still be tried. klink uses the registry store; the GUI compares what this
 * prints against its own store, which is why the JSON carries the key text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "putty.h"
#include "ssh.h"
#include "storage.h"
#include "kitty_hostkeys.h"
#include "kitty_hostkey_scan.h"

/* The key types a scan without -t tries: kitty_hostkey_scan_types(). */

/* ssh-keyscan's names and the SSH wire names -> the store's cache ids.
 * "ecdsa" fans out to the three curves. Returns how many were added, 0 for
 * an unknown name. */
static int types_for_name(const char *name, const char **out, int max)
{
    struct { const char *name, *id; } const table[] = {
        { "rsa", "rsa2" }, { "ssh-rsa", "rsa2" }, { "rsa2", "rsa2" },
        { "dsa", "dss" }, { "dss", "dss" }, { "ssh-dss", "dss" },
        { "ed25519", "ssh-ed25519" }, { "ssh-ed25519", "ssh-ed25519" },
        { "ed448", "ssh-ed448" }, { "ssh-ed448", "ssh-ed448" },
        { "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp256" },
        { "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp384" },
        { "ecdsa-sha2-nistp521", "ecdsa-sha2-nistp521" },
    };
    int n = 0;
    if (!stricmp(name, "ecdsa")) {
        if (max < 3) return 0;
        out[n++] = "ecdsa-sha2-nistp256";
        out[n++] = "ecdsa-sha2-nistp384";
        out[n++] = "ecdsa-sha2-nistp521";
        return n;
    }
    for (size_t i = 0; i < lenof(table); i++)
        if (!stricmp(name, table[i].name) && max >= 1) {
            out[0] = table[i].id;
            return 1;
        }
    return 0;
}

/* "host", "host:port", "[v6::addr]:port" -> host + port (22 when absent).
 * The host is a fresh string. false for an empty or malformed spec. */
static bool parse_hostspec(const char *spec, char **host, int *port)
{
    const char *colon;
    *port = 22;
    if (!spec || !*spec)
        return false;
    if (*spec == '[') {
        const char *close = strchr(spec, ']');
        if (!close)
            return false;
        *host = dupprintf("%.*s", (int)(close - spec - 1), spec + 1);
        if (close[1] == ':' && close[2])
            *port = atoi(close + 2);
        else if (close[1])
            { sfree(*host); return false; }
        return **host != '\0';
    }
    colon = strchr(spec, ':');
    if (colon && !strchr(colon + 1, ':')) {   /* exactly one colon: a port */
        *host = dupprintf("%.*s", (int)(colon - spec), spec);
        *port = atoi(colon + 1);
    } else
        *host = dupstr(spec);                 /* none, or a bare IPv6 address */
    if (**host == '\0' || *port <= 0 || *port > 65535) {
        sfree(*host);
        return false;
    }
    return true;
}

/* ---- JSON --------------------------------------------------------------- */

static void json_str(const char *s)
{
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void json_field(const char *name, const char *value, bool first)
{
    if (!first) printf(", ");
    json_str(name); printf(": "); json_str(value && *value ? value : "-");
}

static void json_int(const char *name, int value)
{
    printf(", "); json_str(name); printf(": %d", value);
}

/* ---- the scan ----------------------------------------------------------- */

struct scan_state {
    bool done;
    char *keytype, *keystr;         /* what the server presented */
    char *fatal;                    /* what ended the connection instead */
};

static struct scan_state *cur;      /* the seat is static; one scan at a time */

static void scan_capture(void *ctx, const char *host, int port,
                         const char *keytype, const char *keystr,
                         char **fingerprints)
{
    struct scan_state *st = (struct scan_state *)ctx;
    if (!st->keytype) {
        st->keytype = dupstr(keytype);
        st->keystr = dupstr(keystr);
    }
    st->done = true;
}

static void scan_fatal(Seat *seat, const char *msg)
{
    if (cur && !cur->fatal)
        cur->fatal = dupstr(msg);
    if (cur) cur->done = true;
}

static void scan_disconnect(Seat *seat)
{
    if (cur) cur->done = true;
}

static SeatPromptResult scan_userpass(Seat *seat, prompts_t *p)
{
    /* Cannot happen - the capture ends the connection before authentication
     * - but if it did, the answer is no. */
    if (cur) cur->done = true;
    return SPR_USER_ABORT;
}

static SeatPromptResult scan_yes(Seat *seat, SeatDialogText *text,
                                 void (*callback)(void *ctx, SeatPromptResult result),
                                 void *ctx)
{
    return SPR_OK;                  /* weak crypto: we only want to look */
}

static const SeatVtable scan_seat_vt = {
    .output = nullseat_output,
    .eof = nullseat_eof,
    .sent = nullseat_sent,
    .banner = nullseat_banner,
    .get_userpass_input = scan_userpass,
    .notify_session_started = nullseat_notify_session_started,
    .notify_remote_exit = nullseat_notify_remote_exit,
    .notify_remote_disconnect = scan_disconnect,
    .connection_fatal = scan_fatal,
    .nonfatal = nullseat_nonfatal,
    .update_specials_menu = nullseat_update_specials_menu,
    .get_ttymode = nullseat_get_ttymode,
    .set_busy_status = nullseat_set_busy_status,
    .confirm_ssh_host_key = nullseat_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = scan_yes,
    .confirm_weak_cached_hostkey = scan_yes,
    .prompt_descriptions = nullseat_prompt_descriptions,
    .is_utf8 = nullseat_is_never_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_display = nullseat_get_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = nullseat_get_window_pixel_size,
    .stripctrl_new = nullseat_stripctrl_new,
    .set_trust_status = nullseat_set_trust_status,
    .can_set_trust_status = nullseat_can_set_trust_status_no,
    .has_mixed_input_stream = nullseat_has_mixed_input_stream_no,
    .verbose = nullseat_verbose_no,
    .interactive = nullseat_interactive_no,
    .get_cursor_position = nullseat_get_cursor_position,
};
static Seat scan_seat[1] = {{ &scan_seat_vt }};

static bool scan_pre(void *vctx, const HANDLE **eh, size_t *neh)
{
    return !((struct scan_state *)vctx)->done;
}

static void scan_timeout(void *ctx, unsigned long now)
{
    struct scan_state *st = (struct scan_state *)ctx;
    if (!st->done) {
        st->fatal = dupstr("timed out waiting for the host key");
        st->done = true;
    }
}

#define SCAN_TIMEOUT_MS 20000

/* One connection: the key of one type, or the reason there is none. */
static void scan_one(const char *host, int port, const char *cache_id,
                     struct scan_state *st)
{
    Conf *conf = conf_new();
    LogContext *logctx;
    Backend *backend = NULL;
    char *realhost = NULL, *err;

    memset(st, 0, sizeof(*st));
    do_defaults(NULL, conf);
    conf_set_int(conf, CONF_protocol, PROT_SSH);
    conf_set_str(conf, CONF_host, host);
    conf_set_int(conf, CONF_port, port);
    conf_set_bool(conf, CONF_ssh_prefer_known_hostkeys, false);
    conf_set_bool(conf, CONF_tryagent, false);
    conf_set_bool(conf, CONF_ssh_simple, true);

    cur = st;
    kitty_hostkey_scan_only = cache_id;
    kitty_hostkey_scan_set_capture(scan_capture, st);
    logctx = log_init(console_cli_logpolicy, conf);
    err = backend_init(&ssh_backend, scan_seat, &backend, logctx, conf,
                       host, port, &realhost, false, false);
    if (err) {
        st->fatal = err;
        st->done = true;
    } else {
        schedule_timer(SCAN_TIMEOUT_MS, scan_timeout, st);
        cli_main_loop(scan_pre, cliloop_null_post, st);
        expire_timer_context(st);
        backend_free(backend);
        sfree(realhost);
    }
    kitty_hostkey_scan_set_capture(NULL, NULL);
    kitty_hostkey_scan_only = NULL;
    cur = NULL;
    log_free(logctx);
    conf_free(conf);
}

/* The store's entry for host:port of this type, or NULL. */
static const struct kitty_hostkey_entry *stored_entry(
    const struct kitty_hostkey_list *l, const char *host, int port,
    const char *cache_id)
{
    for (int i = 0; i < l->n; i++)
        if (l->items[i].port == port && !strcmp(l->items[i].keytype, cache_id) &&
            !strcmp(l->items[i].host, host))
            return &l->items[i];
    return NULL;
}

int kitty_hostkey_scan_main(const char *hostspec, const char *types, bool json)
{
    const char *want[16];
    int nwant = 0;
    char *host;
    int port;
    struct kitty_hostkey_list *stored;
    bool any_key = false, any_mismatch = false;

    if (!parse_hostspec(hostspec, &host, &port)) {
        fprintf(stderr, "klink -scan: bad host \"%s\" (host, host:port or [v6]:port)\n",
                hostspec ? hostspec : "");
        return 1;
    }
    if (types && *types) {
        char *copy = dupstr(types), *p = copy, *tok;
        while ((tok = strtok(p, ",")) != NULL) {
            int n = types_for_name(tok, want + nwant, (int)lenof(want) - nwant);
            p = NULL;
            if (!n) {
                fprintf(stderr, "klink -scan: unknown key type \"%s\" "
                        "(rsa, dsa, ecdsa, ed25519, ed448 or an SSH name)\n", tok);
                sfree(copy); sfree(host);
                return 1;
            }
            nwant += n;
        }
        sfree(copy);
        if (!nwant) { fprintf(stderr, "klink -scan: no key type given\n"); sfree(host); return 1; }
    } else {
        int nall;
        const char *const *all = kitty_hostkey_scan_types(&nall);
        for (int i = 0; i < nall && nwant < (int)lenof(want); i++)
            want[nwant++] = all[i];
    }

    stored = kitty_hostkeys_enumerate();
    if (json) printf("[");
    for (int i = 0; i < nwant; i++) {
        struct scan_state st;
        struct kitty_hostkey_entry e;
        const struct kitty_hostkey_entry *have = stored_entry(stored, host, port, want[i]);
        const char *status, *error = NULL;

        memset(&e, 0, sizeof(e));
        scan_one(host, port, want[i], &st);
        if (st.keytype) {
            int cmp = check_stored_host_key(host, port, st.keytype, st.keystr);
            kitty_hostkey_describe_text(st.keytype, st.keystr, &e);
            status = cmp == 0 ? "stored" : cmp == 1 ? "new" : "MISMATCH";
            any_key = true;
            if (cmp == 2) any_mismatch = true;
        } else {
            e.type_display = dupstr(want[i]);
            {
                /* the wire name for the row even when nothing came */
                struct kitty_hostkey_entry probe;
                memset(&probe, 0, sizeof(probe));
                kitty_hostkey_describe_text(want[i], "", &probe);
                if (probe.type_display) { sfree(e.type_display); e.type_display = probe.type_display; probe.type_display = NULL; }
                sfree(probe.sha256); sfree(probe.md5);
            }
            error = st.fatal ? st.fatal : "connection closed before the key exchange";
            status = strstr(error, "host key algorithm") ? "not offered" : "unreachable";
        }

        if (json) {
            printf("%s{", i ? ", " : "");
            json_field("host", host, true);
            json_int("port", port);
            json_field("type", e.type_display, false);
            json_int("bits", e.bits);
            json_field("sha256", e.sha256, false);
            json_field("md5", e.md5, false);
            json_field("key", st.keystr, false);
            json_field("status", status, false);
            json_field("first_seen", have ? have->first_seen : "-", false);
            json_field("last_written", have ? have->last_written : "-", false);
            json_field("stored_sha256", have && !strcmp(status, "MISMATCH") ? have->sha256 : "-", false);
            json_field("stored_md5", have && !strcmp(status, "MISMATCH") ? have->md5 : "-", false);
            json_field("error", error, false);
            printf("}");
        } else {
            char bits[16];
            if (e.bits) sprintf(bits, "%d", e.bits); else strcpy(bits, "-");
            printf("%s:%d  %s  %s  %s\n", host, port, e.type_display, bits, status);
            if (st.keytype)
                printf("    %s  MD5:%s\n", e.sha256, e.md5);
            if (have && !strcmp(status, "stored"))
                printf("    first seen %s, last written %s\n",
                       have->first_seen[0] ? have->first_seen : "-",
                       have->last_written[0] ? have->last_written : "-");
            if (have && !strcmp(status, "MISMATCH"))
                printf("    stored: %s  MD5:%s  (first seen %s, last written %s)\n",
                       have->sha256, have->md5,
                       have->first_seen[0] ? have->first_seen : "-",
                       have->last_written[0] ? have->last_written : "-");
            if (error)
                printf("    %s\n", error);
        }
        fflush(stdout);
        sfree(e.type_display); sfree(e.sha256); sfree(e.md5); sfree(e.keytype);
        sfree(st.keytype); sfree(st.keystr); sfree(st.fatal);
    }
    if (json) printf("]\n");
    kitty_hostkeys_free(stored);
    sfree(host);
    return any_mismatch ? 2 : any_key ? 0 : 1;
}

/* ---- the store, listed -------------------------------------------------- */

static int entry_cmp(const void *av, const void *bv)
{
    const struct kitty_hostkey_entry *a = av, *b = bv;
    int c = stricmp(a->host, b->host);
    if (c) return c;
    if (a->port != b->port) return a->port < b->port ? -1 : 1;
    return strcmp(a->type_display, b->type_display);
}

int kitty_hostkey_knownhosts_main(const char *hostspec, bool json)
{
    char *host = NULL;
    int port = 0;                       /* 0 = every port */
    struct kitty_hostkey_list *l;
    int shown = 0;

    if (hostspec && *hostspec) {
        if (!parse_hostspec(hostspec, &host, &port)) {
            fprintf(stderr, "klink -knownhosts: bad host \"%s\"\n", hostspec);
            return 1;
        }
        if (!strchr(hostspec, ':') || (hostspec[0] == '[' && !strstr(hostspec, "]:")))
            port = 0;
    }
    l = kitty_hostkeys_enumerate();
    qsort(l->items, l->n, sizeof(l->items[0]), entry_cmp);
    if (json) printf("[");
    for (int i = 0; i < l->n; i++) {
        const struct kitty_hostkey_entry *e = &l->items[i];
        if (host && stricmp(e->host, host)) continue;
        if (port && e->port != port) continue;
        if (json) {
            printf("%s{", shown ? ", " : "");
            json_field("host", e->host, true);
            json_int("port", e->port);
            json_field("type", e->type_display, false);
            json_int("bits", e->bits);
            json_field("sha256", e->sha256, false);
            json_field("md5", e->md5, false);
            json_field("status", "stored", false);
            json_field("first_seen", e->first_seen, false);
            json_field("last_written", e->last_written, false);
            printf("}");
        } else {
            printf("%s:%d  %s  %d  %s  MD5:%s  first seen %s, last written %s\n",
                   e->host, e->port, e->type_display, e->bits,
                   e->sha256[0] ? e->sha256 : "-", e->md5[0] ? e->md5 : "-",
                   e->first_seen[0] ? e->first_seen : "-",
                   e->last_written[0] ? e->last_written : "-");
        }
        shown++;
    }
    if (json) printf("]\n");
    kitty_hostkeys_free(l);
    sfree(host);
    return shown ? 0 : 1;
}
