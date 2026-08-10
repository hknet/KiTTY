/*
 * kitty_protkey.h - a private SSH key held ENCRYPTED in memory (Windows
 * CryptProtectMemory, SAME_PROCESS), materialised to a live ssh_key only for
 * the instant of use. Shrinks the window in which the plaintext private key
 * sits in the process image / pagefile / a crash dump - the case a normal
 * on-close wipe cannot cover, because a crash or kill never runs it.
 *
 * Extracted from pageant's protected_skey so kageant, the kittygen GUI and the
 * kittygen CLI can share ONE implementation. On non-Windows, or if the crypt
 * API is unavailable, kitty_protkey_from_key() returns NULL and the caller is
 * expected to keep the cleartext key (no worse than before).
 *
 * NB - DEDUP PENDING: pageant.c still carries its own protected_skey_* copy
 * (protected_skey_from_key / _to_temp_key / _free). That is the code this was
 * extracted from; it will be migrated onto this module in a follow-up. The
 * duplication is deliberate and temporary - not a missed refactor - so that
 * building the kittygen protection does not also destabilise the running agent.
 */
#ifndef KITTY_PROTKEY_H
#define KITTY_PROTKEY_H

typedef struct KittyProtKey KittyProtKey;
struct ssh_key;

/* Serialise + encrypt `key`. Does NOT take ownership of `key` (caller still
 * frees it). Returns NULL if the crypt API is unavailable. */
KittyProtKey *kitty_protkey_from_key(struct ssh_key *key);

/* Decrypt + rebuild a fresh live key; the caller ssh_key_free()s it. The
 * stored blob is re-encrypted before this returns. NULL on failure. */
struct ssh_key *kitty_protkey_to_temp_key(KittyProtKey *pk);

/* Wipe the encrypted blob and free. */
void kitty_protkey_free(KittyProtKey *pk);

#endif /* KITTY_PROTKEY_H */
