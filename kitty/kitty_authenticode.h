/*
 * kitty_authenticode.h - the ONE Authenticode trust + publisher-CN gate,
 * shared by the in-app updater (kitty_win.c) and the kageant "New key"
 * launcher (windows/pageant.c). Both used to need this check; having a single
 * implementation is the point - a security gate must not exist in two copies
 * that can drift.
 */
#ifndef KITTY_AUTHENTICODE_H
#define KITTY_AUTHENTICODE_H

/* SECURITY GATE. 1 only if the file at `path` has a valid Authenticode trust
 * chain AND its signing certificate's subject CN is EXACTLY our publisher.
 * Fail-closed: every error path returns 0 (reject). */
int kitty_authenticode_verify(const char *path);

/* The PE fixed-file-version of `path` into *ms/*ls (dwFileVersionMS /
 * dwFileVersionLS). 1 on success, 0 on failure. */
int kitty_file_version(const char *path, unsigned long *ms, unsigned long *ls);

/* Verify `path` is a genuine, same-version sibling of THIS running binary -
 * our publisher CN and an exact file-version match - as when kageant is about
 * to launch kittygen. When THIS binary is itself unsigned (a dev/test build),
 * the signature leg is skipped: an unsigned build cannot honestly demand a
 * signed sibling. The exact-version match always applies. 1 = allow. */
int kitty_verify_sibling(const char *path);

/* KiTTY: client-side "which agent answers us" check. agent-client.c calls
 * this hook (when installed) with the serving agent's process id, once per
 * process; kitty.exe installs an implementation that verifies the server
 * binary against our publisher and warns if it is not a genuine
 * KiTTY/kageant. Console tools (plink/pscp/psftp) leave it NULL. */
enum { KITTY_AGENT_TRANSPORT_PIPE, KITTY_AGENT_TRANSPORT_WMCOPYDATA };
extern void (*agent_serving_check_hook)(unsigned long server_pid,
                                        int transport);
void kitty_install_agent_check(void);   /* kitty.exe: install the hook */

#endif /* KITTY_AUTHENTICODE_H */
