/*
 * kitty_kageant_openssh.h - the declarations for kitty_kageant_openssh.c,
 * kageant's optional integration with the Windows OpenSSH client.
 */
#ifndef KITTY_KAGEANT_OPENSSH_H
#define KITTY_KAGEANT_OPENSSH_H

#include <stdio.h>

int kageant_openssh_get(void);
int kageant_openssh_set(int on);
/* add/remove the managed ~/.ssh block: 1 done, 0 not (err: the file and
 * Windows' reason, ~/.ssh/config left as it was); *backup 1 when a
 * config.kageant.bak exists */
int kageant_openssh_apply(int on, int *backup, char *err, size_t errlen);
char *kageant_ssh_path(const char *leaf);  /* malloc'd %USERPROFILE%\.ssh\<leaf>, or NULL */
void kageant_write_identityagent(FILE *fp, const char *pipename);

#endif /* KITTY_KAGEANT_OPENSSH_H */
