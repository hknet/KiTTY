#ifndef KITTY_URLSESSION_H
#define KITTY_URLSESSION_H

/* kitty_urlsession.c: an ssh:// or telnet:// link opens a matching saved
 * session ([KiTTY] urlsessions=yes). */

int GetUrlSessionsFlag(void);
int GetOfferSessionFlag(void);   /* [KiTTY] offernewsessioncreation, default yes */

/* windows/putty.c, after the command line is read and before its saved
 * options are replayed. Loads the session the link matches into conf (the
 * chooser when several do; its Cancel ends the run). Does nothing without a
 * link, with urlsessions off, or when nothing matches. */
void kitty_urlsession_apply(Conf *conf);

#endif
