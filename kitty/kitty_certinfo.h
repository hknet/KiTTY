/*
 * kitty_certinfo.h - what kageant shows of an OpenSSH certificate it holds:
 * the CA, the principals, the validity and the critical options read from
 * the certificate's public blob, and its state at a given time (valid,
 * expiring within KCERT_WARN_DAYS, expired, not yet valid) for the key
 * list's Certificate column, Key details and the validity notices.
 *
 * No Windows here: the state takes the time and the local time's offset
 * from UTC as numbers, so test/test_certinfo.c can pin every case.
 */
#ifndef KITTY_CERTINFO_H
#define KITTY_CERTINFO_H

#include "defs.h"   /* ptrlen, bool */

/* a certificate that expires within this many days is shown amber */
#define KCERT_WARN_DAYS 14

typedef struct kcert_info {
    uint64_t valid_after;   /* 0: valid from any time */
    uint64_t valid_before;  /* UINT64_MAX: valid for ever */
    char *ca_fp;            /* the CA key, "SHA256:..." */
    char *principals;       /* "alice, deploy"; "" for none */
    char *critical;         /* "force-command=/usr/bin/backup, ..."; "" */
    char *key_id;
} kcert_info;

/* Fill *ci from a certificate's public blob. false (and *ci untouched)
 * when the blob is no certificate kageant can read. */
bool kcert_info_get(ptrlen blob, kcert_info *ci);
void kcert_info_free(kcert_info *ci);

/* The public blob of the key a blob signs with: a certificate's base key,
 * else a copy of the blob. The key list shows one row per private key, a
 * key and its certificates together, grouped by this. */
strbuf *kcert_base_blob(ptrlen blob);

/* The state of a certificate at now (seconds since 1970, UTC). tzoff is the
 * local time's offset from UTC in seconds: days are counted in local
 * calendar days, so "expires today" is today's date on this computer. */
enum {
    KCERT_VALID,        /* valid, more than KCERT_WARN_DAYS days left */
    KCERT_EXPIRING,     /* valid, expires within KCERT_WARN_DAYS days */
    KCERT_EXPIRES_TODAY,/* valid, expires on today's date */
    KCERT_EXPIRED,      /* now is at or after valid_before */
    KCERT_NOT_YET       /* now is before valid_after */
};
/* *days: the local calendar days to the expiry (KCERT_EXPIRING) */
int kcert_state(const kcert_info *ci, uint64_t now, int64_t tzoff, int *days);

/* How a state is marked: 0 normal, 1 amber, 2 red */
int kcert_state_level(int state);

/* Which of two states is more urgent: < 0 when a is, 0 when equal. days
 * orders two KCERT_EXPIRING states. */
int kcert_state_cmp(int a, int adays, int b, int bdays);

/* A state as the key list and Key details show it ("expires in 3 days",
 * "EXPIRED since <date>", ...). date formats a time as the date the user
 * reads; the result is the caller's to sfree. */
typedef char *(*kcert_datefn)(uint64_t t, void *ctx);
char *kcert_state_text(const kcert_info *ci, int state, int days,
                       kcert_datefn date, void *ctx);

/* The key list's Certificate column for a key's n certificates: the most
 * urgent one's state, "(+N)" for the others; "certificate" or
 * "N certificates" while every one is valid; "" for none. *level the
 * column's marking (kcert_state_level of the most urgent). */
char *kcert_column_text(const kcert_info *certs, size_t n, uint64_t now,
                        int64_t tzoff, kcert_datefn date, void *ctx,
                        int *level);

#endif /* KITTY_CERTINFO_H */
