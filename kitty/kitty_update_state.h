/*
 * kitty_update_state.h - the STORED answer of the update check, for a program
 * that must not ask the network itself. kitty.exe and the launcher ask GitHub
 * and store what they learn (kitty_updater.c); kageant, which holds the private
 * keys, only reads that and compares it with its own version.
 *
 * No network code, no PuTTY dependencies: the registry, the light kitty.ini
 * resolver and the version resource of the running program.
 */
#ifndef KITTY_UPDATE_STATE_H
#define KITTY_UPDATE_STATE_H

/* [KiTTY] checkupdate, the application-wide "check for updates" switch.
 * Absent means on, as in kitty.exe. */
int kitty_update_state_enabled(void);

/* 1 when the stored latest release is newer than THIS program and its channel
 * may be shown (a stable build ignores betas, as in kitty.exe). latest_out
 * gets the version text, *beta_out whether it is a beta. Either may be NULL. */
int kitty_update_state_newer(char *latest_out, int latest_n, int *beta_out);

#endif
