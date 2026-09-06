/*
 * kitty_hostkey_scan.h: our ssh-keyscan - klink -scan / -knownhosts.
 *
 * A scan connects, completes the key exchange, records the host key the
 * server presented and drops the connection BEFORE authentication: no
 * credentials are ever sent, the store is never written. One connection per
 * key type, because a server presents exactly one host key per connection.
 * The kex-only trick is what ssh-keyscan does too.
 *
 * Two halves. The HOOK half (kitty_hostkey_scan_hook.c) is linked into every
 * SSH-capable binary because ssh/common.c and ssh/transport2.c call it: a
 * type restriction for the key-exchange list and a capture point for the key.
 * Both are inert unless a scan armed them. The SCAN half
 * (kitty_hostkey_scan.c) is klink-only: the scan seat, the connection loop
 * and the two command-line entry points.
 */
#ifndef KITTY_HOSTKEY_SCAN_H
#define KITTY_HOSTKEY_SCAN_H

/* ---- hook half: linked everywhere ---------------------------------------- */

/* While non-NULL, the key exchange offers ONLY the host-key algorithms whose
 * cache_id ("rsa2", "ssh-ed25519", "ecdsa-sha2-nistp256", ...) is this one.
 * A server without such a key fails the exchange with "Couldn't agree a
 * host key algorithm", which the scan reports as "not offered". */
extern const char *kitty_hostkey_scan_only;

/* The capture point, called by verify_ssh_host_key() BEFORE it consults the
 * store or the seat. Returns true when a scan is armed - the caller then
 * abandons the connection (SPR_USER_ABORT) without storing or asking. */
typedef void (*kitty_hostkey_capture_fn)(void *ctx, const char *host, int port,
                                         const char *keytype, const char *keystr,
                                         char **fingerprints);
void kitty_hostkey_scan_set_capture(kitty_hostkey_capture_fn fn, void *ctx);
bool kitty_hostkey_scan_capture(const char *host, int port, const char *keytype,
                                const char *keystr, char **fingerprints);

/* ---- scan half: klink only ----------------------------------------------- */

/* klink -scan [-t types] host[:port] [-json]. `types` is ssh-keyscan's comma
 * list (rsa, dsa, ecdsa, ed25519, ed448, or SSH names) or NULL for every
 * type. Prints the rows; returns the process exit code: 0 all keys seen and
 * none differ, 2 at least one MISMATCH, 1 nothing reachable or bad usage.
 * The network must be up (sk_init, winselcli_setup) before this is called. */
int kitty_hostkey_scan_main(const char *hostspec, const char *types, bool json);

/* klink -knownhosts [host[:port]] [-json]: the store, no connection made.
 * Returns 0 with rows, 1 with none. */
int kitty_hostkey_knownhosts_main(const char *hostspec, bool json);

#endif /* KITTY_HOSTKEY_SCAN_H */
