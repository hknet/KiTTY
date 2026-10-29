/*
 * kitty_hostkey_blob.h - a host key as the KiTTY++ host-key store writes it
 * (the type's cache id and its cache text) turned back into the SSH public
 * blob. On its own so that kageant, which does not link the settings
 * library, reads the store with the same code as kitty.exe and klink.
 * Include after putty.h.
 */
#ifndef KITTY_HOSTKEY_BLOB_H
#define KITTY_HOSTKEY_BLOB_H

/* The store's text of a key -> the SSH public-key blob (caller frees), or
 * NULL when the type is unknown or the text does not parse. */
strbuf *kitty_hostkey_blob_from_text(const char *keytype, const char *text);

#endif /* KITTY_HOSTKEY_BLOB_H */
