/*
 * kitty_selfcheck_stamp.c - the integrity stamp block, on its own.
 *
 * The 256-byte block in the `.ktstamp` section that the release's stamp tool
 * fills with the Ed25519-signed SHA-256 of the file (layout and rules in
 * kitty_selfcheck_core.h). It lives in this translation unit, apart from the
 * check in kitty_selfcheck.c, because the check needs the crypto library and
 * two shipped programs link none (pterm, puttytel): they carry the stamp so the
 * release can stamp every file and the Applications panel and the release
 * check can verify them, while the programs that have the crypto library also
 * verify themselves at startup.
 *
 * Compiled to nothing without KITTY_SELFCHECK, like the check itself. `used`
 * keeps the linker from dropping the block: nothing in the program reads this
 * copy - the check reads the stamp from the FILE, exactly as the stamp tool
 * does. The magic makes an unfilled block read as `no stamp`.
 */
#ifdef KITTY_SELFCHECK
#include <windows.h>
#include "kitty_selfcheck_core.h"

const unsigned char kt_stamp_block[KT_STAMP_SIZE]
    __attribute__((section(KT_STAMP_SECTION), used, aligned(16))) =
    { 'K', 'T', 'S', 'T', 'A', 'M', 'P', 0 };
#else
/* nothing: a build without the check carries no stamp block either */
typedef int kitty_selfcheck_stamp_c_is_not_empty;
#endif
