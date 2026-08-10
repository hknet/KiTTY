/*
 * Display the fingerprints of the PGP Master Keys to the user as a
 * GUI message box.
 */

#include "putty.h"

void pgp_fingerprints_msgbox(HWND owner)
{
    message_box(
        owner,
        /* KiTTY: the upstream text claimed these fingerprints "establish
         * a trust path from this executable to another one" - for a fork
         * that claim is wrong, and it was never more than data any
         * program could print. Shown for historic reasons only. */
        "These are the fingerprints of the PuTTY PGP Master Keys, shown "
        "for historic reasons only: this program is KiTTY 0.84 code, not "
        "a PuTTY release. The fingerprints say nothing about the "
        "authenticity of this executable, and printing them proves "
        "nothing - any program could print them. They are only useful "
        "for checking the PGP signatures on downloads from the PuTTY "
        "project itself.\n"
        "(Note: these fingerprints have nothing to do with SSH!)\n"
        "\n"
        "PuTTY Master Key as of " PGP_MASTER_KEY_YEAR
        " (" PGP_MASTER_KEY_DETAILS "):\n"
        "  " PGP_MASTER_KEY_FP "\n\n"
        "Previous Master Key (" PGP_PREV_MASTER_KEY_YEAR
        ", " PGP_PREV_MASTER_KEY_DETAILS "):\n"
        "  " PGP_PREV_MASTER_KEY_FP "\n"
        "\n"
        "This command-line option (-pgpfp) is deprecated and will be "
        "removed in a future KiTTY release.",
        "PGP fingerprints", MB_ICONINFORMATION | MB_OK,
        false, HELPCTXID(pgp_fingerprints));
}
