/*
 * kitty_storage_int.h - private to kitty_storage.c and kitty_secretstore.c:
 * what the store half and the crypto half of the settings store share.
 * Not for other files; the public interfaces are kitty_storage.h and
 * kitty_secretstore.h.
 */
#ifndef KITTY_STORAGE_INT_H
#define KITTY_STORAGE_INT_H

/* The registry base in use (kitty_storage.c; kitty_registry_base() is the
 * public accessor). The crypto half keeps its master-password state under it. */
extern char reg_base_buf[256];

/* Old-KiTTY password decrypt keyed on host + terminal type
 * (kitty_secretstore.c); the portable-file loader applies it while converting
 * a cyd01-syntax session file. */
char *ksec_legacy_decrypt_hostterm(const char *stored, const char *host,
                                   const char *term);

#endif /* KITTY_STORAGE_INT_H */
