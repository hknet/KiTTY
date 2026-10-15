/*
 * kitty_term_int.h - KiTTY++: what terminal/terminal.c, kitty_osc.c and
 * kitty_clipboard.c share. Private to terminal/: nothing outside these
 * three files includes it. Everything in it is MOD_PERSO's.
 */
#ifndef KITTY_TERM_INT_H
#define KITTY_TERM_INT_H
#ifdef MOD_PERSO

/*
 * KiTTY OSC 52 clipboard-READ seams. Implemented on the platform side
 * (kitty/kitty_osc52.c) so this cross-platform file stays free of Win32 and of
 * dialog code; the test target supplies its own stubs, which is also how it
 * exercises the permission logic without a window ever appearing.
 *
 * kitty_osc52_read_confirm() opens the read box, modeless: the request waits,
 * and the answer comes back later through term_osc52_read_answer() - allow or
 * deny, how long it applies (one of OSC52_GRANT_*) and whether the user ticked
 * "always deny for this host". The box is the ONLY place a read can be
 * permitted; there is no setting that does it. _end: the terminal is going
 * away, the box closes without answering.
 */
enum {
    OSC52_GRANT_ONCE,          /* just this request */
    OSC52_GRANT_MINUTES,       /* CONF_osc52_read_minutes */
    OSC52_GRANT_REQUESTS,      /* CONF_osc52_read_requests */
    OSC52_GRANT_SESSION,       /* the rest of this terminal session */
};
void kitty_osc52_read_confirm(Terminal *term, const wchar_t *clip, int clip_len,
                              const char *claim);
void kitty_osc52_read_confirm_end(Terminal *term);
/* Fetch the local clipboard as wide text. Caller frees. NULL if empty or if the
 * clipboard holds something that is not text. */
/* ...and the variant that distinguishes "empty" from "somebody else has it
 * open", because only one of those is an answer. */
/* The clipboard as PNG bytes: the "PNG" / "image/png" formats as they are, or a
 * bitmap encoded to PNG. NULL when there is no image. Caller frees. */
/* Is there an image on the clipboard at all? For the type list; no data moves. */
/* Random bytes from the system CSPRNG, for the paste-event token. A seam because
 * PuTTY's own pool asserts when nothing has referenced it yet. */
/* OSC 5522 write: put every format of one transaction on the clipboard in ONE
 * open/empty/set/close. text/plain becomes CF_UNICODETEXT, anything else a
 * registered format named by its MIME type. False if the clipboard could not be
 * set, which the caller reports as EIO. */
/* Write "always deny for this host" into the saved session. Returns false if
 * there is no saved session to write it into, in which case the caller tells the
 * user rather than inventing a hidden host list behind their back. */
/* Transient tray balloon on the session window; no-op when notifications are
 * switched off. `action` is one of CLIP_BALLOON_* and says what CLICKING it does,
 * because a rate-limited balloon stands for more events than it can describe and
 * so has to lead somewhere. */
void kitty_osc52_notify(Terminal *term, const char *title, const char *msg,
                        int action);

/* kitty_clipboard.c: what terminal.c (term_free, power_on, far2l, the
 * paste path) and kitty_osc.c call. */
/* A clipboard permission started, expired, or changed between active and paused:
 * re-apply the title marker and the window colouring. */
/* Is there a title bar to put a marker on? False in full screen and with window
 * decorations off, where the icon and the caption tint have nowhere to appear. */
/* "A clipboard payload was too big and was dropped" - Event Log plus a balloon,
 * both rate-limited. Declared up here because far2l_process_payload sits earlier
 * in the file than the definition, and all three protocols share it. */
void clip_payload_dropped(Terminal *term, const char *what, bool enabled);
/* The remote-clipboard-write rate cap, and its report. Declared up here for the
 * same reason as above: far2l's set path sits earlier in the file. */
bool clip_write_allowed(Terminal *term);
void clip_write_throttled(Terminal *term);
/* "The host just touched the clipboard" - lights the transient activity marker.
 * Declared here because far2l's set path sits earlier in the file. */
void clip_note_activity(Terminal *term, int dir);
/* Drop an OSC 5522 write transaction, wiping its data. Declared here because
 * term_free() sits earlier in the file than the OSC 5522 code. */
void osc5522_write_reset(Terminal *term);
/* Drop what waits for the write confirmation box, wiping it. Declared here for
 * term_free() too. */
void osc52_write_held_drop(Terminal *term);
/* The same for the read box: the waiting request and the clipboard it showed. */
void osc52_read_wait_drop(Terminal *term);
/* Paste-events mode off (and its token gone). `timed`: by the auto-disarm timer,
 * which is logged, as opposed to a reset or the application clearing it. */
void osc5522_paste_disarm(Terminal *term, bool timed);
void osc5522_paste_set(Terminal *term, bool state);
bool osc5522_paste_event(Terminal *term);
size_t clip_ceiling_bytes(Terminal *term);   /* terminal.c */
void osc5522_process(Terminal *term);
void osc52_set_clipboard(Terminal *term);

/* kitty_osc.c */
bool kitty_osc_dispatch(Terminal *term);
bool kitty_osc_dcs(Terminal *term);
void kitty_links_free(Terminal *term);
int im_col(Terminal *term, int y);

/* terminal.c: the KiTTY++ helpers the moved code calls */
int term_sync_hold_ms(Terminal *term);
void term_sync_release(Terminal *term);
void term_sync_begin(Terminal *term, long ms);
void gfx_cursor_advance(Terminal *term, int dx, int dy);
void mark_set(Terminal *term, int y, unsigned bits, bool on);
int mark_find_up(Terminal *term, int y, unsigned bits);
timer_fn_t kitty_term_timer_fn(void);

#ifdef MOD_FAR2L
/* KiTTY: Sixel graphics. Needs both: the DCS reaches OSC_STRING whole only
 * under MOD_FAR2L (see the DCS entry), the store is MOD_PERSO's. */
#define KITTY_SIXEL 1
#endif

#endif /* MOD_PERSO */
#endif /* KITTY_TERM_INT_H */
