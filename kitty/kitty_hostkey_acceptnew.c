/*
 * kitty_hostkey_acceptnew.c: accept a NEW host key without a question - the
 * switch, the decision and the record's second destination. See
 * kitty_hostkey_acceptnew.h. No Windows or PuTTY headers: test/test_acceptnew.c
 * links it alone.
 */
#include <stddef.h>
#include <stdbool.h>
#include "kitty_hostkey_acceptnew.h"

static bool accept_new_all = false;
static kitty_hostkey_accept_new_report_fn report_fn = NULL;

void kitty_hostkey_accept_new_set_all(int on)
{
    accept_new_all = (on != 0);
}

bool kitty_hostkey_accept_new_all(void)
{
    return accept_new_all;
}

bool kitty_hostkey_accept_new_applies(bool enabled, int storage_status,
                                      bool certificate, bool other_type_stored)
{
    /* unknown only, and no key of any other type stored: 0 needs nothing,
     * 2 (changed) always gets the question */
    return enabled && storage_status == 1 && !certificate && !other_type_stored;
}

void kitty_hostkey_accept_new_set_report(kitty_hostkey_accept_new_report_fn fn)
{
    report_fn = fn;
}

void kitty_hostkey_accept_new_report(const char *line)
{
    if (report_fn)
        report_fn(line);
}
