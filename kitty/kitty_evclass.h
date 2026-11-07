/*
 * kitty_evclass.h - the class of a terminal Event Log line: NOTABLE lines
 * (something went differently than set, or a far-end request was refused
 * or limited) are the ones the Event Log notice shows. Every
 * other line is routine, upstream PuTTY's among them.
 */
#ifndef KITTY_EVCLASS_H
#define KITTY_EVCLASS_H

#include <stdbool.h>

/* line: the text as handed to logevent, without the Event Log's time stamp */
bool kitty_evclass_is_notable(const char *line);

/* the glob the table uses: '*' matches any run of characters (also none),
 * every other character itself. The text must match in full. */
bool kitty_evclass_match(const char *pattern, const char *text);

#endif /* KITTY_EVCLASS_H */
