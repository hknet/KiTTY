/*
 * kitty_hostkey_acceptnew.h: accept a NEW host key without a question
 * (OpenSSH's StrictHostKeyChecking=accept-new). An unknown key is stored and
 * recorded; a changed key is never accepted this way and gets the usual
 * question (under -batch: refused). Decided in verify_ssh_host_key
 * (ssh/common.c); on per session (CONF_ssh_accept_new_hostkeys, also set by
 * -accept-new) or for every session (kitty.ini [KiTTY] autostoresshkey).
 *
 * Member of the sshcommon library, so every SSH-capable binary links it.
 */
#ifndef KITTY_HOSTKEY_ACCEPTNEW_H
#define KITTY_HOSTKEY_ACCEPTNEW_H

#include <stdbool.h>

/* kitty.ini [KiTTY] autostoresshkey=yes: every session of this process */
void kitty_hostkey_accept_new_set_all(int on);
bool kitty_hostkey_accept_new_all(void);

/* Store the presented key without a question? `storage_status` is
 * check_stored_host_key's answer: 0 stored, 1 unknown, 2 a different key
 * stored. A certificate never qualifies: it reaches the question only when
 * no trusted CA signed it. `other_type_stored`: the host and port have a key
 * of another type - then the question too, since a server posing as this
 * one could offer only a type with nothing stored and so skip the
 * comparison of the known type. */
bool kitty_hostkey_accept_new_applies(bool enabled, int storage_status,
                                      bool certificate, bool other_type_stored);

/* Where the record goes besides the Event Log: klink, kscp and ksftp set
 * standard error (kitty_console_store.c); kitty.exe sets nothing. */
typedef void (*kitty_hostkey_accept_new_report_fn)(const char *line);
void kitty_hostkey_accept_new_set_report(kitty_hostkey_accept_new_report_fn fn);
void kitty_hostkey_accept_new_report(const char *line);

#endif
