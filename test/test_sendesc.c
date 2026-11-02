/*
 * test_sendesc - regression tests for kitty/kitty_sendesc.c: the two hex
 * digits of the \k and \x escapes. A text that ends inside the escape
 * ("\k", "\kA") made SendKeyboardPlus step over the string's NUL and copy
 * the bytes behind it; the operand is now taken only when both digits are
 * there.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_sendesc.h"

static int failures = 0;

static void hex2(const char *what, const char *s, int want_ok, unsigned want_val)
{
    unsigned v = 0xDEAD;
    int ok = kitty_sendesc_hex2(s, &v);
    if (ok != want_ok || (ok && v != want_val) || (!ok && v != 0xDEAD)) {
        printf("FAIL [%s]: got ok=%d val=%#x, want ok=%d val=%#x\n",
               what, ok, v, want_ok, want_ok ? want_val : 0xDEAD);
        failures++;
    }
}

int main(void)
{
    /* The NUL right behind the first digit sits in a buffer whose next byte
     * is a hex digit: a reader that looked past the NUL would take it. */
    char cut[3] = { 'A', '\0', 'B' };
    char none[2] = { '\0', '4' };

    hex2("two digits", "41", 1, 0x41);
    hex2("lower case", "1b", 1, 0x1b);
    hex2("upper case", "FF", 1, 0xff);
    hex2("zero", "00", 1, 0x00);
    hex2("followed by text", "0Dxyz", 1, 0x0d);
    hex2("end of text: nothing", "", 0, 0);
    hex2("end of text after one digit", "A", 0, 0);
    hex2("one digit, the NUL, a digit behind it", cut, 0, 0);
    hex2("the NUL first, a digit behind it", none, 0, 0);
    hex2("not hex", "G1", 0, 0);
    hex2("second not hex", "4G", 0, 0);
    hex2("a backslash", "\\41", 0, 0);
    hex2("NULL", NULL, 0, 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_sendesc: all passed\n");
    return 0;
}
