/*
 * kitty_tags.h - session tags (hknet/KiTTY#60): a session's "Tags" setting is
 * a comma-separated list of names ("lab,switches"). Session > Startup edits a
 * session's tags; Manage Sessions lists every session of a tag and launches
 * them. A tag exists while a session carries it - there is no list of tags of
 * its own, so nothing can go stale.
 */
#ifndef KITTY_TAGS_H
#define KITTY_TAGS_H

#include <windows.h>
#include <stdbool.h>

/* A tag name is usable: not empty, no ',', at most KITTY_TAG_MAXLEN chars.
 * Leading and trailing blanks are trimmed by the callers first. */
#define KITTY_TAG_MAXLEN 64
bool kitty_tag_name_ok(const char *tag);

/* The names of a tags value, trimmed, empties dropped, each once (compared
 * without case, the first spelling kept): an snewn'd array of snewn'd
 * strings, *n of them. Free with kitty_tags_free. */
char **kitty_tags_split(const char *tags, int *n);
void kitty_tags_free(char **v, int n);
/* The value for a list of names ("a,b"), snewn'd. */
char *kitty_tags_join(char **v, int n);
/* Whether a value holds the tag (without case). */
bool kitty_tags_has(const char *tags, const char *tag);
/* The value with the tag added (unchanged when it holds it) or removed;
 * snewn'd. */
char *kitty_tags_add(const char *tags, const char *tag);
char *kitty_tags_remove(const char *tags, const char *tag);

/* A tag's text colour: one of a fixed palette, chosen by its name, so a tag
 * looks the same everywhere without any colour being stored. */
COLORREF kitty_tag_colour(const char *tag, bool dark);

/* A saved session's tags value ("" when none), snewn'd; and the value
 * written back (only that setting; the session's others stay). */
char *kitty_session_tags(const char *session);
bool kitty_session_set_tags(const char *session, const char *tags);

/* Every tag any saved session carries, sorted without case: snewn'd as
 * kitty_tags_split's. */
char **kitty_all_tags(int *n);

/* Session > Startup, "Session Tags": builds the group's controls into s.
 * kitty_tags_enter: the box's Enter (its OK button's handler, before any
 * other use) while the "New tag:" field had the focus adds the typed tag;
 * answers true when it did, and the button must then do nothing else.
 * (Conf, dlgcontrol and dlgparam: include putty.h first.) */
struct controlset;
void kitty_tags_controls(struct controlset *s);
bool kitty_tags_enter(dlgcontrol *okbutton, dlgparam *dlg, Conf *conf);

#endif
