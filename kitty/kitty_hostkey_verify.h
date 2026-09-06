/*
 * kitty_hostkey_verify.h: the Verify button of Application > Security >
 * Host keys - a live look at what a host presents today, next to what the
 * store holds. GUI only.
 *
 * The connection is NOT made in this process: the config box has no network
 * pump before a session exists, and inside a session it would ride the
 * terminal's message loop from inside a modal dialog. Instead each row runs
 * `klink -scan -json -t <type> host:port` as a hidden child process (klink
 * ships beside kitty.exe in every ZIP and the MSI), read through a pipe on a
 * worker thread, one job after the other. The panel polls the jobs from a
 * timer on the UI thread and judges each key against ITS OWN store - klink's
 * own stored/new/MISMATCH refers to the registry, which a portable KiTTY does
 * not use; that is why the JSON carries the key text.
 */
#ifndef KITTY_HOSTKEY_VERIFY_H
#define KITTY_HOSTKEY_VERIFY_H

struct kitty_hkv_job {
    /* what to ask */
    char *host;
    int port;
    char *keytype;            /* the store's id: rsa2, ssh-ed25519, ... */
    /* the answer; valid once state is KHKV_DONE */
    volatile LONG state;      /* KHKV_PENDING / KHKV_RUNNING / KHKV_DONE */
    char *status;             /* klink's word: stored/new/MISMATCH/not offered/unreachable, or ours: "no klink", "klink failed" */
    char *sha256, *md5;       /* what the host presented ("" when nothing) */
    char *key;                /* the presented key in store text, "" when none */
    char *error;              /* the network error, "" when none */
    char *when;               /* ISO local time the answer came */
};
#define KHKV_PENDING 0
#define KHKV_RUNNING 1
#define KHKV_DONE    2

struct kitty_hkv_run;

/* Start verifying `n` jobs (the array is copied; strings are duplicated).
 * NULL when the thread could not be started. */
struct kitty_hkv_run *kitty_hkv_start(const struct kitty_hkv_job *jobs, int n);

/* The jobs, for polling. Read a job's answer only when its state is
 * KHKV_DONE (the worker sets the strings before the state). */
struct kitty_hkv_job *kitty_hkv_jobs(struct kitty_hkv_run *run, int *n);

/* true when every job is done. */
bool kitty_hkv_finished(struct kitty_hkv_run *run);

/* The UI is done with the run. Frees it now when the worker has finished,
 * otherwise the worker frees it when it does; either way the pointer is
 * dead after this call. */
void kitty_hkv_release(struct kitty_hkv_run *run);

/* Where the child comes from: "<dir of this exe>\klink.exe". Caller frees. */
char *kitty_hkv_klink_path(void);

#endif /* KITTY_HOSTKEY_VERIFY_H */
