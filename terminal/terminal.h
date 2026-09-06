/*
 * Internals of the Terminal structure, used by other modules in the
 * terminal subdirectory and by test suites.
 */

#ifndef PUTTY_TERMINAL_H
#define PUTTY_TERMINAL_H

#include "tree234.h"

struct beeptime {
    struct beeptime *next;
    unsigned long ticks;
};

#define TRUST_SIGIL_WIDTH 3
#define TRUST_SIGIL_CHAR 0xDFFE

typedef struct {
    int y, x;
} pos;

typedef struct termchar termchar;
typedef struct termline termline;

struct termchar {
    /*
     * Any code in terminal.c which definitely needs to be changed
     * when extra fields are added here is labelled with a comment
     * saying FULL-TERMCHAR.
     */
    unsigned long chr;
    unsigned long attr;
    truecolour truecolour;

    /*
     * The cc_next field is used to link multiple termchars
     * together into a list, so as to fit more than one character
     * into a character cell (Unicode combining characters).
     *
     * cc_next is a relative offset into the current array of
     * termchars. I.e. to advance to the next character in a list,
     * one does `tc += tc->next'.
     *
     * Zero means end of list.
     */
    int cc_next;
};

struct termline {
    unsigned short lattr;
    int cols;                          /* number of real columns on the line */
    int size;                          /* number of allocated termchars
                                        * (cc-lists may make this > cols) */
    bool temporary;                    /* true if decompressed from scrollback */
    int cc_free;                       /* offset to first cc in free list */
    struct termchar *chars;
    bool trusted;
};

struct bidi_cache_entry {
    int width;
    bool trusted;
    struct termchar *chars;
    int *forward, *backward;           /* the permutations of line positions */
};

struct term_utf8_decode {
    int state;                         /* Is there a pending UTF-8 character */
    int chr;                           /* and what is it so far? */
    int size;                          /* The size of the UTF character. */
};

struct term_userpass_state;

typedef enum {
    OSCLIKE_OSC,
    OSCLIKE_OSC_W,
    OSCLIKE_APC,
    OSCLIKE_DCS,
    OSCLIKE_PM,
    OSCLIKE_SOS,
} OscType;

/* One clipboard format of an OSC 5522 write, as handed to the platform seam
 * kitty_osc52_set_clipboard_formats(): the MIME type names the format, the bytes
 * are what the host sent for it. */
typedef struct KittyClipFormat {
    const char *mime;
    const void *data;
    size_t len;
} KittyClipFormat;

struct terminal_tag {

    int compatibility_level;

    tree234 *scrollback;               /* lines scrolled off top of screen */
    tree234 *screen;                   /* lines on primary screen */
    tree234 *alt_screen;               /* lines on alternate screen */
    int disptop;                       /* distance scrolled back (0 or -ve) */
    int tempsblines;                   /* number of lines of .scrollback that
                                          can be retrieved onto the terminal
                                          ("temporary scrollback") */

    termline **disptext;               /* buffer of text on real screen */

#define VBELL_TIMEOUT (TICKSPERSEC/10) /* visual bell lasts 1/10 sec */

    struct beeptime *beephead, *beeptail;
    int nbeeps;
    bool beep_overloaded;
    long lastbeep;

#define TTYPE termchar
#define TSIZE (sizeof(TTYPE))

    int default_attr, curr_attr, save_attr;
    truecolour curr_truecolour, save_truecolour;
    termchar basic_erase_char, erase_char;

    bufchain inbuf;                    /* terminal input buffer */

    pos curs;                          /* cursor */
    pos savecurs;                      /* saved cursor position */
    int marg_t, marg_b;                /* scroll margins */
    bool dec_om;                       /* DEC origin mode flag */
    bool wrap, wrapnext;               /* wrap flags */
    bool insert;                       /* insert-mode flag */
    int cset;                          /* 0 or 1: which char set */
    int save_cset, save_csattr;        /* saved with cursor position */
    bool save_utf, save_wnext;         /* saved with cursor position */
    bool rvideo;                       /* global reverse video flag */
    unsigned long rvbell_startpoint;   /* for ESC[?5hESC[?5l vbell */
    bool cursor_on;                    /* cursor enabled flag */
    bool reset_132;                    /* Flag ESC c resets to 80 cols */
    bool use_bce;                      /* Use Background coloured erase */
    bool cblinker;                     /* When blinking is the cursor on ? */
    bool tblinker;                     /* When the blinking text is on */
    bool blink_is_real;                /* Actually blink blinking text */
    bool sel_colour;                   /* TuTTY: colour selection vs reverse (unconditional field for stable struct layout) */
    int sco_acs, save_sco_acs;         /* CSI 10,11,12m -> OEM charset */
    bool vt52_bold;                    /* Force bold on non-bold colours */
    bool utf;                          /* Are we in toggleable UTF-8 mode? */
    term_utf8_decode utf8;             /* If so, here's our decoding state */
    bool printing, only_printing;      /* Are we doing ANSI printing? */
    int print_state;                   /* state of print-end-sequence scan */
    bufchain printer_buf;              /* buffered data for printer */
    printer_job *print_job;

    /* ESC 7 saved state for the alternate screen */
    pos alt_savecurs;
    int alt_save_attr;
    truecolour alt_save_truecolour;
    int alt_save_cset, alt_save_csattr;
    bool alt_save_utf;
    bool alt_save_wnext;
    int alt_save_sco_acs;

    int rows, cols, savelines;
    bool has_focus;
    bool in_vbell;
    long vbell_end;
    bool app_cursor_keys, app_keypad_keys, vt52_mode;
    bool repeat_off, srm_echo, cr_lf_return;
    bool big_cursor;

    bool xterm_mouse_forbidden;
    int xterm_mouse;                   /* send mouse messages to host */
    bool xterm_extended_mouse;
    bool urxvt_extended_mouse;
    int mouse_is_down;                 /* used while tracking mouse buttons */
    int raw_mouse_reported_x;
    int raw_mouse_reported_y;

    bool bracketed_paste, bracketed_paste_active;
    bool no_bracketed_paste;           /* disabled in configuration */

    int cset_attr[2];

/*
 * Saved settings on the alternate screen.
 */
    int alt_x, alt_y;
    bool alt_wnext, alt_ins;
    bool alt_om, alt_wrap;
    int alt_cset, alt_sco_acs;
    bool alt_utf;
    int alt_t, alt_b;
    int alt_which;
    int alt_sblines; /* # of lines on alternate screen that should be used for scrollback. */

#define ARGS_MAX 32                    /* max # of esc sequence arguments */
#define ARG_DEFAULT 0                  /* if an arg isn't specified */
#define def(a,d) ( (a) == ARG_DEFAULT ? (d) : (a) )
    unsigned esc_args[ARGS_MAX];
    int esc_nargs;
    int esc_query;
#define ANSI(x,y)       ((x)+((y)*256))
#define ANSI_QUE(x)     ANSI(x,1)

/*
 * The OSC accumulation buffer. Upstream this is a fixed 2 KB array; KiTTY grows
 * it on demand, because an OSC 52 clipboard payload is a whole copied selection
 * (a log, a config file) rather than a window title, and a truncated clipboard
 * is worse than none at all.
 *
 * OSC_STR_MAX is now the BASE allocation and the per-sequence ceiling for every
 * sequence EXCEPT OSC 52, so titles, the Linux palette sequence, OSC 4 and OSC 7
 * behave exactly as before and no host can hand us a multi-megabyte window title
 * as a side effect of the clipboard feature. osc_str_limit is chosen when the
 * parser enters OSC_STRING, at which point esc_args[0] is already known.
 *
 * The CLIPBOARD ceiling is a setting instead - ClipboardMaxMB, read through
 * clip_ceiling_bytes() in terminal.c, which clamps it (bounds below). It is a
 * backstop against a hostile or broken host that opens a sequence and never
 * terminates it: without a ceiling that is remote, unauthenticated memory
 * exhaustion needing no user interaction. It bounds a runaway; it is not meant as
 * a limit on what may legitimately be copied.
 */
#define OSC_STR_MAX 2048
/* OSC 5522 chunks at 4 KB before base64, so no single sequence of it is ever
 * large - it is the transaction that adds up, not the packet. 16 KB covers a full
 * chunk (5462 bytes once base64'd) plus its metadata several times over, and still
 * refuses anything absurd. A write packet that does not fit is a protocol
 * violation and is answered EINVAL, not a buffer problem. */
#define OSC_STR_MAX_5522 (16 * 1024)
/* The spec's floor for a WRITE transaction: a terminal "must accept at least
 * 64MB of data" before it may answer EFBIG. The user's ClipboardMaxMB still
 * applies to OSC 52 and far2l unchanged; a 5522 write gets the larger of the two,
 * because a setting must not make us non-compliant. */
#define OSC5522_WRITE_MIN_MB 64
/*
 * The clipboard payload ceiling is a SETTING (ClipboardMaxMB, default 64 MB),
 * shared by OSC 52 and far2l - see conf.h, and clip_ceiling_bytes() in terminal.c,
 * which clamps it. These are the bounds of that clamp.
 *
 * Why 64 MB by default: far2l carries arbitrary Windows clipboard formats, so it
 * is the path an IMAGE travels, and an uncompressed 4K CF_DIB is about 33 MB raw
 * and about 44 MB once base64'd.
 *
 * Why it is clamped at all: this bounds a hostile or broken host that opens a
 * sequence and never terminates it, which is otherwise remote memory exhaustion
 * needing no user interaction. The decode transiently costs about the same again,
 * so a maximal payload is roughly twice the ceiling in peak memory. A user may
 * lower it freely - that only ever helps - but must not be able to raise it into
 * an unbounded denial of service by typing a big number into a settings box.
 */
/* Must match DEFAULT_INT on CONF_clipboard_max_mb in conf.h, which carries the
 * reasoning for the number (lowered 64 -> 16 on 2026-08-05). */
#define CLIP_MAX_MB_DEFAULT 16
#define CLIP_MAX_MB_CAP 256                    /* the most a user may ask for */
/* There is deliberately no floor constant: the unit is megabytes, so the smallest
 * value anyone can set is 1 MB, and anything <= 0 falls back to the default. A
 * floor would be dead code that read as though it were doing something. */
/* The far2l payload prefix, and its length. Recognised mid-accumulation, because
 * the ceiling has to be raised before the payload has arrived. */
#define FAR2L_DATA_PREFIX "far2l:"
#define FAR2L_DATA_PREFIX_LEN 6
    OscType osc_type;
    int osc_strlen;
    char *osc_string;           /* heap; always has room for a terminating NUL */
    size_t osc_strsize;         /* bytes allocated */
    size_t osc_str_limit;       /* ceiling for the sequence being accumulated */
    /* set when the sequence exceeded osc_str_limit. OSC 52 then refuses the
     * whole payload rather than pasting a truncated one; the sequences that were
     * silently truncated before this existed still are. */
    bool osc_str_overflow;

    /* KiTTY far2l terminal extensions. These fields are UNCONDITIONAL (NOT under
     * #ifdef MOD_FAR2L): terminal.c is compiled into the kitty target WITH
     * MOD_FAR2L while lineedit.c and the rest of libguiterminal are compiled
     * WITHOUT it. Guarding struct fields gives those TUs a different Terminal
     * layout (ODR violation), corrupting later fields such as term->ldisc — which
     * broke interactive prompts ("Terminal not prepared for interactive prompts"
     * on a key passphrase / password). Same lesson as sel_colour. The *code*
     * using these stays guarded by MOD_FAR2L; only the storage is unconditional. */
    /* set when the far2l APC handshake enabled extensions mode
     * (\x1b_far2l1\x07 -> reply \x1b_far2lok\x07). */
    int far2l_ext;
    /* far2l clipboard-sync permission, seeded from CONF_shared_clipboard at the
     * handshake: 0=deny, 1=allow, 2=ask-then-latch (SHARED_CLIPBOARD_*). */
    int clip_allowed;
    /* KiTTY OSC 52 permission, seeded from CONF_osc52_clipboard whenever config
     * is copied in: 0=deny, 1=allow, 2=ask-then-latch (OSC52_CLIPBOARD_*).
     * Latching matters: OSC 52 has no handshake, so a per-payload prompt would
     * be a MessageBox storm any host could trigger at will. UNCONDITIONAL
     * storage, for the ODR reason given above. */
    int osc52_allowed;

    /* KiTTY OSC 52 READ direction (a host asking for the contents of the local
     * clipboard). All UNCONDITIONAL storage, for the ODR reason above.
     *
     * osc52_read_policy is the setting (OSC52_READ_DENY / _ASK). The rest is the
     * live decision, which never reaches disk: a grant is a permission given for
     * a moment, and a moment does not outlive the window. A REFUSAL may be saved,
     * because refusing only ever takes permission away - that is what the
     * dialog's "always deny for this host" does, and it goes into the session
     * settings, not here. The POLICY itself is not mirrored here on purpose - it
     * is read live from the Conf, so changing it in Change Settings takes effect
     * on the next request instead of at the next connection. */
    /* the standing decision: 0 = none/ask again, 1 = allowing, -1 = refusing */
    int osc52_read_decision;
    /* when that decision expires. 0 = it does not (rest of session). Wall clock
     * seconds, from time(NULL) - a grant has to expire while the machine sleeps
     * too, so this cannot be a tick count. */
    unsigned long osc52_read_until;
    /* or how many further requests it covers. -1 = unlimited (rest of session) */
    int osc52_read_remaining;
    /* hand-overs actually served in this window, and when the last one was:
     * the rate limit that stops a single grant becoming an hour of clipboard */
    int osc52_read_served;
    unsigned long osc52_read_last_served;
    /* dialogs shown, and the start of the ten-second window they are counted in */
    int osc52_read_prompts;
    unsigned long osc52_read_prompt_window;
    /* true while a permission dialog is open for this terminal: the next request
     * is refused, not stacked behind it */
    bool osc52_read_asking;
    /* refusals suppressed since the last Event Log line, and when that line was
     * written. A host that asks in a loop must not be able to fill the log. */
    int osc52_read_refused_quiet;
    unsigned long osc52_read_refused_logged;

    /* KiTTY OSC 5522 (the kitty clipboard protocol). Unconditional storage, same
     * ODR reason as everything above.
     *
     * Approvals remembered by (password, name). This is the one thing OSC 5522 can
     * do that OSC 52 fundamentally cannot: identify the asker, so an editor
     * polling the clipboard is something we can recognise instead of something we
     * prompt about every time. In MEMORY ONLY and bounded - no permission to read
     * is ever written to disk, and a fixed array means a host cannot make us
     * allocate by sending endless distinct passwords. Oldest is evicted. */
#define OSC5522_MAX_APPROVALS 8
    char *osc5522_pw[OSC5522_MAX_APPROVALS];    /* the base64 password, verbatim */
    char *osc5522_pw_name[OSC5522_MAX_APPROVALS];        /* the claimed name */
    unsigned long osc5522_pw_until[OSC5522_MAX_APPROVALS];  /* 0 = rest of session */
    int osc5522_pw_count;

    /* KiTTY OSC 5522 WRITE transaction - the host putting data on the clipboard:
     * type=write, then wdata chunks per MIME type, then an empty wdata. It arrives
     * over many sequences, so its state lives here between them. Nothing here
     * means "failed": an error resets the transaction, and packets that arrive
     * with no transaction open are ignored, which is the spec's rule for the rest
     * of a failed write. Unconditional storage, same ODR reason as above. */
#define OSC5522_WRITE_MAX_TYPES 64    /* distinct MIME types in one write */
#define OSC5522_WRITE_MAX_ALIASES 64
    bool osc5522_w_active;
    char osc5522_w_id[48];            /* from the type=write packet; "" if none */
    size_t osc5522_w_bytes;           /* decoded bytes so far, all types together */
    size_t osc5522_w_limit_override;  /* tests only: 0 = the real limit */
    int osc5522_w_ntypes;
    struct {
        char *mime;
        strbuf *data;                 /* decoded bytes */
        char carry[4];                /* base64 characters not yet forming a quad */
        int carry_len;
    } osc5522_w_type[OSC5522_WRITE_MAX_TYPES];
    int osc5522_w_naliases;
    char *osc5522_w_alias[OSC5522_WRITE_MAX_ALIASES];         /* the extra name */
    char *osc5522_w_alias_target[OSC5522_WRITE_MAX_ALIASES];  /* the type it names */

    /* KiTTY: accounting for "a clipboard payload was too big and was dropped",
     * shared by OSC 52, OSC 5522 and far2l so all three report it the same way.
     * Every one of these events fires at a moment the REMOTE HOST chose, so both
     * the Event Log line and the balloon are rate-limited; suppressed ones are
     * counted and mentioned in the next line rather than pretended away.
     * Unconditional storage, same ODR reason as everything above. */
    int clip_dropped_quiet;
    unsigned long clip_dropped_logged;
    unsigned long clip_notified_last;

    /* KiTTY: rate-capping remote clipboard WRITES (ClipboardWritesPerSecond),
     * shared by OSC 52 and far2l. A fixed one-second window: clip_write_second is
     * which second we are counting in, clip_write_count how many have been APPLIED
     * in it. Counting applied rather than attempted writes means a host cannot use
     * refused ones to exhaust anybody's budget but its own.
     * Unconditional storage, same ODR reason as everything above. */
    unsigned long clip_write_second;
    int clip_write_count;
    int clip_write_quiet;
    unsigned long clip_write_logged;

    /* KiTTY: the host actually TOUCHED the clipboard - as against merely having
     * permission to. Direction bits (CLIP_ACT_*) and when the marker stops
     * applying. Repeated activity extends the deadline without repainting, so a
     * host writing at the permitted rate cannot make the title flicker.
     * Unconditional storage, same ODR reason as everything above. */
    int clip_activity_dir;
    unsigned long clip_activity_until;

    char id_string[1024];

    unsigned char *tabs;

    enum {
        TOPLEVEL,
        SEEN_ESC,
        SEEN_CSI,
        SEEN_OSC,
        SEEN_OSC_W,

        DO_CTRLS,

        SEEN_OSC_P,
        OSC_STRING, OSC_MAYBE_ST, OSC_MAYBE_ST_UTF8,
        VT52_ESC,
        VT52_Y1,
        VT52_Y2,
        VT52_FG,
        VT52_BG
    } termstate;

    enum {
        NO_SELECTION, ABOUT_TO, DRAGGING, SELECTED
    } selstate;
    enum {
        LEXICOGRAPHIC, RECTANGULAR
    } seltype;
    enum {
        SM_CHAR, SM_WORD, SM_LINE
    } selmode;
    pos selstart, selend, selanchor;

    short wordness[256];

    /* Mask of attributes to pay attention to when painting. */
    int attr_mask;

    wchar_t *paste_buffer;
    size_t paste_len, paste_pos;

    Backend *backend;

    Ldisc *ldisc;

    TermWin *win;

    LogContext *logctx;

    struct unicode_data *ucsdata;

    unsigned long last_graphic_char;

    /*
     * We maintain a full copy of a Conf here, not merely a pointer
     * to it. That way, when we're passed a new one for
     * reconfiguration, we can check the differences and adjust the
     * _current_ setting of (e.g.) auto wrap mode rather than only
     * the default.
     */
    Conf *conf;

    /*
     * GUI implementations of seat_output call term_out, but it can
     * also be called from the ldisc if the ldisc is called _within_
     * term_out. So we have to guard against re-entrancy - if
     * seat_output is called recursively like this, it will simply add
     * data to the end of the buffer term_out is in the process of
     * working through.
     */
    bool in_term_out;

    /*
     * We don't permit window updates too close together, to avoid CPU
     * churn pointlessly redrawing the window faster than the user can
     * read. So after an update, we set window_update_cooldown = true
     * and schedule a timer to reset it to false. In between those
     * times, window updates are not performed, and instead we set
     * window_update_pending = true, which will remind us to perform
     * the deferred redraw when the cooldown period ends and
     * window_update_cooldown is reset to false.
     */
    bool window_update_pending, window_update_cooldown;
    long window_update_cooldown_end;

    /*
     * Track pending blinks and tblinks.
     */
    bool tblink_pending, cblink_pending;
    long next_tblink, next_cblink;

    /*
     * These are buffers used by the bidi and Arabic shaping code.
     */
    termchar *ltemp;
    int ltemp_size;
    bidi_char *wcFrom, *wcTo;
    int wcFromTo_size;
    struct bidi_cache_entry *pre_bidi_cache, *post_bidi_cache;
    size_t bidi_cache_size;

    /*
     * Current trust state, used to annotate every line of the
     * terminal that a graphic character is output to.
     */
    bool trusted;

    /*
     * We copy a bunch of stuff out of the Conf structure into local
     * fields in the Terminal structure, to avoid the repeated
     * tree234 lookups which would be involved in fetching them from
     * the former every time.
     */
    bool ansi_colour;
    strbuf *answerback;
    bool no_arabicshaping;
    int beep;
    bool bellovl;
    int bellovl_n;
    int bellovl_s;
    int bellovl_t;
    bool no_bidi;
    bool bksp_is_delete;
    bool blink_cur;
    bool blinktext;
    bool cjk_ambig_wide;
    int conf_height;
    int conf_width;
    bool crhaslf;
    bool erase_to_scrollback;
    int funky_type, sharrow_type;
    bool lfhascr;
    bool logflush;
    int logtype;
    bool mouse_override;
    bool nethack_keypad;
    bool no_alt_screen;
    bool no_applic_c;
    bool no_applic_k;
    bool no_dbackspace;
    bool no_mouse_rep;
    bool no_remote_charset;
    bool no_remote_resize;
    bool no_remote_wintitle;
    bool no_remote_clearscroll;
    bool rawcnp;
    bool utf8linedraw;
    bool rect_select;
    int remote_qtitle_action;
    bool rxvt_homeend;
    bool scroll_on_disp;
    bool scroll_on_key;
    bool xterm_256_colour;
    bool true_colour;

    wchar_t *last_selected_text;
    int *last_selected_attr;
    truecolour *last_selected_tc;
    size_t last_selected_len;
    int mouse_select_clipboards[N_CLIPBOARDS];
    int n_mouse_select_clipboards;
    int mouse_paste_clipboard;

    char *window_title, *icon_title;
    int wintitle_codepage, icontitle_codepage;
    bool minimised;

    BidiContext *bidi_ctx;

    /* Multi-layered colour palette. The colours from Conf (plus the
     * default xterm-256 ones that don't have Conf ids at all) have
     * lowest priority, followed by platform overrides if any,
     * followed by escape-sequence overrides during the session. */
    struct term_subpalette {
        rgb values[OSC4_NCOLOURS];
        bool present[OSC4_NCOLOURS];
    } subpalettes[3];
#define SUBPAL_CONF 0
#define SUBPAL_PLATFORM 1
#define SUBPAL_SESSION 2

    /* The composite palette that we make out of the above */
    rgb palette[OSC4_NCOLOURS];

    unsigned winpos_x, winpos_y, winpixsize_x, winpixsize_y;

    /*
     * Assorted 'pending' flags for ancillary window changes performed
     * in term_update. Generally, to trigger one of these operations,
     * you set the pending flag and/or the parameters here, then call
     * term_schedule_update.
     */
    bool win_move_pending;
    int win_move_pending_x, win_move_pending_y;
    bool win_zorder_pending;
    bool win_zorder_top;
    bool win_minimise_pending;
    bool win_minimise_enable;
    bool win_maximise_pending;
    bool win_maximise_enable;
    bool win_title_pending, win_icon_title_pending;
    bool win_pointer_shape_pending;
    bool win_pointer_shape_raw;
    bool win_refresh_pending;
    bool win_scrollbar_update_pending;
    bool win_palette_pending;
    unsigned win_palette_pending_min, win_palette_pending_limit;

    /*
     * Unlike the rest of the above 'pending' flags, the one for
     * window resizing has to be more complicated, because it's very
     * likely that a server sending a window-resize escape sequence is
     * going to follow it up immediately with further terminal output
     * that draws a full-screen application expecting the terminal to
     * be the new size.
     *
     * So, once we've requested a window resize from the TermWin, we
     * have to stop processing terminal data until we get back the
     * notification that our window really has changed size (or until
     * we find out that it's not going to).
     *
     * Hence, window resizes go through a small state machine with two
     * different kinds of 'pending'. NEED_SEND is the state where
     * we've received an escape sequence asking for a new size but not
     * yet sent it to the TermWin via win_request_resize; AWAIT_REPLY
     * is the state where we've sent it to the TermWin and are
     * expecting a call back to term_size().
     *
     * So _both_ of those 'pending' states inhibit terminal output
     * processing.
     *
     * (Hence, once we're in either state, we should never handle
     * another resize sequence, so the only possible path through this
     * state machine is to get all the way back to the ground state
     * before doing anything else interesting.)
     */
    enum {
        WIN_RESIZE_NO, WIN_RESIZE_NEED_SEND, WIN_RESIZE_AWAIT_REPLY
    } win_resize_pending;
    int win_resize_pending_w, win_resize_pending_h;

    /*
     * Indicates whether term_get_userpass_input is currently using
     * the terminal to present a password prompt or similar, and if
     * so, whether it's overridden the terminal into UTF-8 mode.
     */
    struct term_userpass_state *userpass_state;
    bool userpass_utf8_override;

    /* Input method state. */
    termline *preedit_termline;
};

static inline bool in_utf(Terminal *term)
{
    return (term->utf ||
            term->ucsdata->line_codepage == CP_UTF8 ||
            (term->userpass_state && term->userpass_utf8_override));
}

unsigned long term_translate(
    Terminal *term, term_utf8_decode *utf8, unsigned char c);
static inline int term_char_width(Terminal *term, unsigned int c)
{
    return term->cjk_ambig_wide ? mk_wcwidth_cjk(c) : mk_wcwidth(c);
}

/*
 * UCSINCOMPLETE is returned from term_translate if it's successfully
 * absorbed a byte but not emitted a complete character yet.
 * UCSTRUNCATED indicates a truncated multibyte sequence (so the
 * caller emits an error character and then calls term_translate again
 * with the same input byte). UCSINVALID indicates some other invalid
 * multibyte sequence, such as an overlong synonym, or a standalone
 * continuation byte, or a completely illegal thing like 0xFE. These
 * values are not stored in the terminal data structures at all.
 */
#define UCSINCOMPLETE 0x8000003FU    /* '?' */
#define UCSTRUNCATED  0x80000021U    /* '!' */
#define UCSINVALID    0x8000002AU    /* '*' */

/*
 * Maximum number of combining characters we're willing to store in a
 * character cell. Our linked-list data representation permits an
 * unlimited number of these in principle, but if we allowed that in
 * practice then it would be an easy DoS to just squirt a squillion
 * identical combining characters to someone's terminal and cause
 * their PuTTY or pterm to consume lots of memory and CPU pointlessly.
 *
 * The precise figure of 32 is more or less arbitrary, but one point
 * supporting it is UAX #15's comment that 30 combining characters is
 * "significantly beyond what is required for any linguistic or
 * technical usage".
 */
#define CC_LIMIT 32

/* ----------------------------------------------------------------------
 * Helper functions for dealing with the small 'pos' structure.
 */

static inline bool poslt(pos p1, pos p2)
{
    if (p1.y != p2.y)
        return p1.y < p2.y;
    return p1.x < p2.x;
}

static inline bool posle(pos p1, pos p2)
{
    if (p1.y != p2.y)
        return p1.y < p2.y;
    return p1.x <= p2.x;
}

static inline bool poseq(pos p1, pos p2)
{
    return p1.y == p2.y && p1.x == p2.x;
}

static inline int posdiff_fn(pos p1, pos p2, int cols)
{
    return (p1.y - p2.y) * (cols+1) + (p1.x - p2.x);
}

/* Convenience wrapper on posdiff_fn which uses the 'Terminal *term'
 * that more or less every function in terminal.c will have in scope.
 * For safety's sake I include a TYPECHECK that ensures it really is a
 * structure pointer of the right type. */
#define GET_TERM_COLS TYPECHECK(term == (Terminal *)0, term->cols)
#define posdiff(p1,p2) posdiff_fn(p1, p2, GET_TERM_COLS)

/* Product-order comparisons for rectangular block selection. */

static inline bool posPle(pos p1, pos p2)
{
    return p1.y <= p2.y && p1.x <= p2.x;
}

static inline bool posPle_left(pos p1, pos p2)
{
    /*
     * This function is used for checking whether a given character
     * cell of the terminal ought to be highlighted as part of the
     * selection, by comparing with term->selend. term->selend stores
     * the location one space to the right of the last highlighted
     * character. So we want to highlight the characters that are
     * less-or-equal (in the product order) to the character just left
     * of p2.
     *
     * (Setting up term->selend that way was the easiest way to get
     * rectangular selection working at all, in a code base that had
     * done lexicographic selection the way I happened to have done
     * it.)
     */
    return p1.y <= p2.y && p1.x < p2.x;
}

static inline bool incpos_fn(pos *p, int cols)
{
    if (p->x == cols) {
        p->x = 0;
        p->y++;
        return true;
    }
    p->x++;
    return false;
}

static inline bool decpos_fn(pos *p, int cols)
{
    if (p->x == 0) {
        p->x = cols;
        p->y--;
        return true;
    }
    p->x--;
    return false;
}

/* Convenience wrappers on incpos and decpos which use term->cols
 * (similarly to posdiff above), and also (for mild convenience and
 * mostly historical inertia) let you leave off the & at every call
 * site. */
#define incpos(p) incpos_fn(&(p), GET_TERM_COLS)
#define decpos(p) decpos_fn(&(p), GET_TERM_COLS)

struct TermLineEditorCallbackReceiverVtable {
    void (*to_terminal)(TermLineEditorCallbackReceiver *rcv, ptrlen data);
    void (*to_backend)(TermLineEditorCallbackReceiver *rcv, ptrlen data);
    void (*special)(TermLineEditorCallbackReceiver *rcv,
                    SessionSpecialCode code, int arg);
    void (*newline)(TermLineEditorCallbackReceiver *rcv);
};
struct TermLineEditorCallbackReceiver {
    const TermLineEditorCallbackReceiverVtable *vt;
};
TermLineEditor *lineedit_new(Terminal *term, unsigned flags,
                             TermLineEditorCallbackReceiver *receiver);
void lineedit_free(TermLineEditor *le);
void lineedit_input(TermLineEditor *le, char ch, bool dedicated);
void lineedit_modify_flags(TermLineEditor *le, unsigned clr, unsigned flip);
void lineedit_send_line(TermLineEditor *le);

/*
 * Flags controlling the behaviour of TermLineEditor.
 */
#define LINEEDIT_FLAGS(X) \
    X(LE_INTERRUPT)           /* pass SS_IP back to client on ^C */ \
    X(LE_SUSPEND)             /* pass SS_SUSP back to client on ^Z */ \
    X(LE_ABORT)               /* pass SS_ABORT back to client on ^\ */ \
    X(LE_EOF_ALWAYS)          /* pass SS_EOF to client on *any* ^D \
                               * (not just if the line buffer is empty) */ \
    X(LE_ESC_ERASES)          /* make ESC erase the line, as well as ^U */ \
    X(LE_CRLF_NEWLINE)        /* interpret manual ^M^J the same as Return */ \
    /* end of list */
enum {
    #define ALLOCATE_BIT_POSITION(flag) flag ## _bitpos,
    LINEEDIT_FLAGS(ALLOCATE_BIT_POSITION)
    #undef ALLOCATE_BIT_POSITION
};
enum {
    #define DEFINE_FLAG_BIT(flag) flag = 1 << flag ## _bitpos,
    LINEEDIT_FLAGS(DEFINE_FLAG_BIT)
    #undef DEFINE_FLAG_BIT
};

termline *term_get_line(Terminal *term, int y);
void term_release_line(termline *line);

#endif
