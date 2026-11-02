/*
 * kitty_sendesc.h - the operand of the \k and \x escapes of the texts
 * SendKeyboardPlus types (kitty/kitty_broadcast.c): two hex digits after the
 * letter. No Win32 and no PuTTY types, so test/test_sendesc.c covers it
 * without a window.
 */
#ifndef KITTY_SENDESC_H
#define KITTY_SENDESC_H

/* The two hex digits at s (the characters after "\k" or "\x"): 1 and their
 * value in *val when both are there and both are hex digits, else 0 and *val
 * untouched. Reads s[1] only when s[0] is a hex digit, so it never reads past
 * the string's terminating NUL. */
int kitty_sendesc_hex2(const char *s, unsigned *val);

#endif /* KITTY_SENDESC_H */
