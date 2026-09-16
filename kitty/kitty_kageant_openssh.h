/*
 * kitty_kageant_openssh.h - the declarations for kitty_kageant_openssh.c,
 * kageant's optional integration with the Windows OpenSSH client.
 */
#ifndef KITTY_KAGEANT_OPENSSH_H
#define KITTY_KAGEANT_OPENSSH_H

#include <stdio.h>

int kageant_openssh_get(void);
void kageant_openssh_set(int on);
void kageant_openssh_apply(int on);    /* add/remove the managed ~/.ssh block */
char *kageant_ssh_path(const char *leaf);  /* malloc'd %USERPROFILE%\.ssh\<leaf>, or NULL */
void kageant_write_identityagent(FILE *fp, const char *pipename);

#endif /* KITTY_KAGEANT_OPENSSH_H */
