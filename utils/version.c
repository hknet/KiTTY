/*
 * PuTTY version numbering
 */

/*
 * The difficult part of deciding what goes in these version strings
 * is done in Buildscr, and then written into version.h. All we have
 * to do here is to drop it into variables of the right names.
 */

#include "putty.h"
#include "ssh.h"

#include "version.h"

const char ver[] = TEXTVER;
/* KiTTY: sshver is mutable (set_sshver) so the SSH client version string can be
 * overridden via kitty.ini 'sshversion'. Fixed 40-byte buffer keeps the static
 * assert below valid (sizeof == 40). */
char sshver[40] = SSHVER;

/*
 * SSH local version string MUST be under 40 characters. Here's a
 * compile time assertion to verify this.
 */
enum { vorpal_sword = 1 / (sizeof(sshver) <= 40) };
