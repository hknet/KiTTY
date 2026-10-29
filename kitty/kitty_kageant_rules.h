/*
 * kitty_kageant_rules.h - kageant's file of destination rules
 * (kitty_agentrestrict.h holds the store itself): kageant-rules.dat beside
 * a portable kitty.ini, else in %APPDATA%\KiTTY. One file, encrypted with
 * DPAPI for the Windows account, loaded in full at the start and written
 * in full through a temporary file and a rename.
 *
 * A file that cannot be read, decrypted or checked marks the store damaged:
 * every key is then treated as restricted with no rule (signing refused
 * over the pipe, never unrestricted), the window shows it and the agent log
 * has it (event 104, store-damaged). The damaged file is never written over
 * by ssh-add -h; saving rules in the rule editor first moves it aside as
 * <file>.damaged-<date>-<time>.
 * Include after putty.h.
 */
#ifndef KITTY_KAGEANT_RULES_H
#define KITTY_KAGEANT_RULES_H

struct kar_store;

/* Where the file is; NULL when there is no place (no %APPDATA%). sfree
 * it. */
char *kageant_rules_path_dup(void);

/* Load the file into the agent's store (kar_store_get()). No file is an
 * empty store. 1 loaded or absent; 0 damaged, with the record written. */
int kageant_rules_load(void);

/* Why the store is damaged, as the window shows it; NULL when it is not. */
const char *kageant_rules_damage_text(void);

/* Write the agent's store (ssh-add -h changed it). NULL when written, else
 * the reason, as a notice shows it (sfree it). A damaged store is not
 * written. */
char *kageant_rules_save(void);

/* The rule editor's OK: src becomes the agent's store and is written. A
 * damaged file is moved aside first (*aside = its new path, sfree it). NULL
 * when written; else the reason (sfree it), and *applied tells whether the
 * rules are in force anyway (readonly=yes: for the running agent only). */
char *kageant_rules_replace(const struct kar_store *src, char **aside,
                            bool *applied);

/* A key's destinations as the records and the key details show them:
 * "web01 (SHA256:...), jump01 > web01 (SHA256:...)"; NULL when the key has
 * none. fp in the agent's form (kar_key_fp). sfree it. */
char *kageant_rules_text_for_fp(const char *fp);

/* Is this SSH-2 key restricted (it has links, or the file is damaged)? */
bool kageant_rules_key_restricted(ptrlen blob);

#endif /* KITTY_KAGEANT_RULES_H */
