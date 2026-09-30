/*
 * kitty_url_regex.h - the default expression that finds URLs in the output
 * (kitty/url/urlhack.c), and the ones it replaced.
 *
 * The old ones are here for one job: a session whose custom expression is
 * one of them, word for word, is a session that took KiTTY's default at the
 * time (the "Reset to KiTTY default" button copies it into the field). Such
 * a session gets the current default when it is loaded (settings.c,
 * load_open_settings), so the fix reaches it too. A pattern the user wrote
 * differs in some character and is left alone.
 *
 * The engine (regcomp in urlhack.c) has no {m,n} counts.
 */
#ifndef KITTY_URL_REGEX_H
#define KITTY_URL_REGEX_H

/* The top-level domain is any run of two or more letters. The old ones
 * below took a list (com, net, ...) or exactly two letters, so a
 * newer domain was cut to its first two letters and lost its path -
 * https://pi.dev/changelog was found as https://pi.de. */
#define KITTY_URL_REGEX_DEFAULT \
    "(((https?|ftp):\\/\\/)|www\\.)(([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)|localhost|([a-zA-Z0-9\\-]+\\.)*[a-zA-Z0-9\\-]+\\.([a-zA-Z][a-zA-Z]+))(:[0-9]+)?((\\/|\\?)[^ \"]*[^ ,;\\.:\">)])?"

/* the previous default */
#define KITTY_URL_REGEX_OLD_1 \
    "(((https?|ftp):\\/\\/)|www\\.)(([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)|localhost|([a-zA-Z0-9\\-]+\\.)*[a-zA-Z0-9\\-]+\\.(com|net|org|info|biz|gov|name|edu|[a-zA-Z][a-zA-Z]))(:[0-9]+)?((\\/|\\?)[^ \"]*[^ ,;\\.:\">)])?"

/* the one before that (PuttyTray / older KiTTY) */
#define KITTY_URL_REGEX_OLD_2 \
    "(((https?|ftp):\\/\\/)|www\\.)(([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)|localhost|([a-zA-Z0-9\\-]+\\.)*[a-zA-Z0-9\\-]+\\.(aero|asia|biz|cat|com|coop|info|int|jobs|mobi|museum|name|net|org|post|pro|tel|travel|xxx|edu|gov|mil|[a-zA-Z][a-zA-Z]))(:[0-9]+)?((\\/|\\?)[^ \"]*[^ ,;\\.:\">)])?"

#endif
