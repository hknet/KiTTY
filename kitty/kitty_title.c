/*
 * KiTTY: one place for the standard window-title / tooltip state suffixes.
 *
 * Every window that advertises the process state appends the same markers:
 * "(portable)" when the portable storage layout is in use, "(RESTRICTED)"
 * when the process runs with the restricted ACL, and the test-build label
 * on ad-hoc test builds. Composing them here means a new marker (or a
 * wording change) happens once, not once per dialog - the Reconfiguration
 * box already missed (RESTRICTED) when each window rolled its own.
 *
 * The caller supplies the state flags: what "portable" means differs per
 * binary (kitty.exe asks its storage layer, kageant its inilight resolver).
 */

#include "putty.h"
#include "kitty_title.h"

char *kitty_title_compose_sep(const char *base, const char *sep,
                              int portable, int restricted, int test_label)
{
    strbuf *sb = strbuf_new();
    put_dataz(sb, base);
    if (portable) {
        put_dataz(sb, sep);
        put_dataz(sb, "(portable)");
    }
    if (restricted) {
        put_dataz(sb, sep);
        put_dataz(sb, "(RESTRICTED)");
    }
#ifdef KITTY_TEST_BUILD_LABEL
    if (test_label) {
        put_dataz(sb, sep);
        put_dataz(sb, "*** TEST BUILD: " KITTY_TEST_BUILD_LABEL " ***");
    }
#else
    (void)test_label;
#endif
    return strbuf_to_str(sb);
}

char *kitty_title_compose(const char *base, int portable, int restricted,
                          int test_label)
{
    return kitty_title_compose_sep(base, " ", portable, restricted,
                                   test_label);
}
