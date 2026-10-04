/*
 * test_menuslots.c - the payload table behind the IDM_USERCMD + n menu
 * entries (the launcher's sessions, the terminal's User Commands):
 * kitty/kitty_menuslots.c.
 *
 * The ceiling and its clamp, the numbering, the copy of each payload, the
 * growth past several reallocations up to NB_MENU_MAX, the count of the
 * entries left out - summed over a walk of nested folders the way
 * ReadSpecialMenu adds them (each folder's subfolders first, then its own
 * entries) - and the reset that empties the table for the next menu build.
 *
 * Linked with kitty_menuslots.c alone: no Win32, no PuTTY.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty/kitty_menuslots.h"

static int checks, failures;
static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* One folder of a menu walk: `own` entries of its own, `nsub` subfolders. */
struct folder { int own; int nsub; const struct folder *sub; };

static int walk(const struct folder *f, int *shown)
{
    int i, total = 0;
    char name[32];
    for (i = 0; i < f->nsub; i++)
        total += walk(&f->sub[i], shown);
    for (i = 0; i < f->own; i++) {
        snprintf(name, sizeof(name), "s%d", i);
        if (kitty_menuslots_add(name) >= 0)
            (*shown)++;
        total++;
    }
    return total;
}

int main(void)
{
    char buf[16];
    int i, n, last;

    /* The ceiling: the id range 0x8000..0x8FFF, and its clamp. */
    ok(NB_MENU_MAX == 0x1000, "NB_MENU_MAX is 0x1000");
    ok(0x8000 + NB_MENU_MAX <= 0x9000, "IDM_USERCMD + NB_MENU_MAX stays below IDM_GOHIDE");
    kitty_menuslots_reset(0);
    ok(kitty_menuslots_cap() == NB_MENU_MAX, "cap 0 = NB_MENU_MAX");
    kitty_menuslots_reset(-4);
    ok(kitty_menuslots_cap() == NB_MENU_MAX, "negative cap = NB_MENU_MAX");
    kitty_menuslots_reset(NB_MENU_MAX + 1);
    ok(kitty_menuslots_cap() == NB_MENU_MAX, "cap above NB_MENU_MAX is clamped");
    kitty_menuslots_reset(3);
    ok(kitty_menuslots_cap() == 3, "cap 3 kept");

    /* Numbering, the copy, the cut count. */
    ok(kitty_menuslots_count() == 0 && kitty_menuslots_cut() == 0, "empty after reset");
    ok(kitty_menuslots_get(0) == NULL, "no entry 0 in an empty table");
    strcpy(buf, "alpha");
    ok(kitty_menuslots_add(buf) == 0, "first entry is 0");
    strcpy(buf, "XXXXX");
    ok(kitty_menuslots_get(0) && !strcmp(kitty_menuslots_get(0), "alpha"),
       "the payload is a copy");
    ok(kitty_menuslots_add("beta") == 1, "second entry is 1");
    ok(kitty_menuslots_add(NULL) == 2, "a NULL payload is stored as empty");
    ok(kitty_menuslots_get(2) && kitty_menuslots_get(2)[0] == '\0', "...as an empty string");
    ok(kitty_menuslots_add("delta") == -1, "past the cap: -1");
    ok(kitty_menuslots_add("epsilon") == -1, "past the cap again: -1");
    ok(kitty_menuslots_count() == 3, "three stored");
    ok(kitty_menuslots_cut() == 2, "two left out");
    ok(kitty_menuslots_get(3) == NULL, "no entry past the count");
    ok(kitty_menuslots_get(-1) == NULL, "no entry -1");

    /* The reset empties the table and the count of the ones left out. */
    kitty_menuslots_reset(0);
    ok(kitty_menuslots_count() == 0 && kitty_menuslots_cut() == 0, "reset clears count and cut");
    ok(kitty_menuslots_get(0) == NULL, "reset frees the entries");

    /* Growth to the full ceiling, then past it. */
    last = -2;
    for (i = 0; i < NB_MENU_MAX + 7; i++) {
        snprintf(buf, sizeof(buf), "%d", i);
        n = kitty_menuslots_add(buf);
        if (n >= 0) last = n;
    }
    ok(kitty_menuslots_count() == NB_MENU_MAX, "NB_MENU_MAX stored");
    ok(last == NB_MENU_MAX - 1, "the last number is NB_MENU_MAX - 1");
    ok(kitty_menuslots_cut() == 7, "seven left out at the full ceiling");
    ok(kitty_menuslots_get(0) && !strcmp(kitty_menuslots_get(0), "0"), "entry 0 kept across growth");
    ok(kitty_menuslots_get(1000) && !strcmp(kitty_menuslots_get(1000), "1000"), "entry 1000 kept across growth");
    snprintf(buf, sizeof(buf), "%d", NB_MENU_MAX - 1);
    ok(kitty_menuslots_get(NB_MENU_MAX - 1) && !strcmp(kitty_menuslots_get(NB_MENU_MAX - 1), buf),
       "the last entry");

    /* A walk of nested folders with a cap of 5: root has 4 of its own and
     * folder A (2 of its own) which holds folder B (3 of its own). Walked
     * subfolders first: B 3, A 2, root 4 = 9 sessions, 5 shown, 4 left out -
     * counted across the folder levels, not per level. */
    {
        static const struct folder b = { 3, 0, NULL };
        static const struct folder a = { 2, 1, &b };
        static const struct folder root = { 4, 1, &a };
        int shown = 0, total;
        kitty_menuslots_reset(5);
        total = walk(&root, &shown);
        ok(total == 9, "nine sessions walked");
        ok(shown == 5, "five shown");
        ok(kitty_menuslots_cut() == 4, "four left out, summed over the levels");
        ok(kitty_menuslots_cut() + kitty_menuslots_count() == total, "shown + left out = all");
        /* A rebuild with the same store gives the same count, not a sum. */
        shown = 0;
        kitty_menuslots_reset(5);
        walk(&root, &shown);
        ok(kitty_menuslots_cut() == 4, "a rebuild counts anew");
        /* The test-build door: limit + 3 sessions -> "(3 more sessions not shown)". */
        kitty_menuslots_reset(6);
        shown = 0;
        walk(&root, &shown);
        ok(kitty_menuslots_cut() == 3, "cap 6 of 9: three left out");
    }

    kitty_menuslots_reset(0);
    printf("test_menuslots: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
