#ifndef KITTY_WORKPLACE_H
#define KITTY_WORKPLACE_H

/*
 * Workplace proxy mode: the arming channel (design/TASK_workplace_proxy.md §3,
 * §4). The launcher holds the arming; every connection asks whether one is held
 * right now. Nothing about "armed" is ever written to disk.
 */

/* Launcher side. arm() returns 1 when this process now holds the arming, 0 if
 * it could not take it (another holder already exists, or the proxy name is
 * empty/too long). disarm() releases it; it is also released implicitly when
 * the process dies, which is the whole point of the design. */
int kitty_workplace_arm(const char *proxyname, unsigned int minutes);
void kitty_workplace_disarm(void);
int kitty_workplace_holding(void);           /* does THIS process hold it? */

/* Connection side. 1 when an arming is held right now, with the proxy name
 * copied into name[len]. Cheap and non-blocking: it opens the holder's section
 * or it does not, so a hung launcher cannot stall the startup path. */
int kitty_workplace_query(char *name, int len);

/* Minutes left before the arming expires, or 0 when it has no timeout. Only
 * meaningful while armed. */
unsigned int kitty_workplace_minutes_left(void);

/* The window message that asks a launcher of THIS install to switch the mode on
 * (wParam 1) or off (wParam 0). Install-keyed like the arming itself, so a
 * broadcast cannot reach another install's launcher. The proxy to arm with is
 * not in the message: the launcher reads the remembered selection, which is
 * what the config box has just written. */
unsigned int kitty_workplace_message(void);

/* Ask a running launcher to switch the mode on/off and wait briefly for the
 * answer (the arming itself is the answer - nothing is trusted to reply).
 * minutes 0 = no timeout, it lasts until switched off or the launcher exits.
 * Returns 1 if the mode is in the requested state afterwards. */
int kitty_workplace_request(int arm, unsigned int minutes);

/* Start a launcher already armed, for when there is none running. Returns 1 if
 * the mode is armed afterwards. */
int kitty_workplace_start_launcher(const char *proxyname, unsigned int minutes);

/*
 * Telling the user the mode is not on any more - ONCE, and only when they might
 * not know.
 *
 * Switching it off deliberately, or a timeout running out, says so at that
 * moment and settles the account. What is left is the mode ending because the
 * launcher went away - closed, killed, logged off, rebooted - where nobody was
 * told anything. Arming leaves a breadcrumb; being told clears it; a later start
 * that finds the breadcrumb with no arming held knows it owes exactly one
 * notice, and shows it on whichever path starts first (launcher, -load, or the
 * config box).
 */
void kitty_workplace_mark_armed(void);        /* arming: a notice may be owed */
void kitty_workplace_notice_settled(void);    /* the user has just been told */
int  kitty_workplace_notice_owed(void);       /* ended with nobody told? */
void kitty_workplace_show_pending_notice(void);  /* show it, once, and settle */

/* "3 h 20 min", or "" when the arming has no timeout. Hours and minutes, never
 * seconds: this is read at a glance, not counted down. */
void kitty_workplace_left_text(char *out, int len);

#endif
