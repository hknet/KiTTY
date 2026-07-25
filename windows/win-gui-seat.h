/*
 * Main state structure for an instance of the Windows PuTTY front
 * end, containing all the PuTTY objects and all the Windows API
 * resources like the window handle.
 */

typedef struct WinGuiSeat WinGuiSeat;

struct PopupMenu {
    HMENU menu;
};
enum { SYSMENU, CTXMENU };             /* indices into popup_menus field */

#define FONT_NORMAL 0
#define FONT_BOLD 1
#define FONT_UNDERLINE 2
#define FONT_BOLDUND 3
#define FONT_WIDE       0x04
#define FONT_HIGH       0x08
#define FONT_NARROW     0x10

#define FONT_OEM        0x20
#define FONT_OEMBOLD    0x21
#define FONT_OEMUND     0x22
#define FONT_OEMBOLDUND 0x23

#define FONT_MAXNO      0x40
#define FONT_SHIFT      5

enum BoldMode {
    BOLD_NONE, BOLD_SHADOW, BOLD_FONT
};
enum UnderlineMode {
    UND_LINE, UND_FONT
};

struct _dpi_info {
    POINT cur_dpi;
    RECT new_wnd_rect;
};

/*
 * Against the future possibility of having more than one of these
 * in a process (and the current possibility of having zero), we
 * keep a linked list of all live WinGuiSeats, so that cleanups
 * can be done to any that exist.
 */
struct WinGuiSeatListNode {
    struct WinGuiSeatListNode *next, *prev;
};
extern struct WinGuiSeatListNode wgslisthead; /* static end pointer */

struct WinGuiSeat {
    struct WinGuiSeatListNode wgslistnode;

    Seat seat;
    TermWin termwin;
    LogPolicy logpolicy;

    HWND term_hwnd;

    int extra_width, extra_height;
    int font_width, font_height;
    bool font_dualwidth, font_varpitch;
    int offset_width, offset_height;
    bool was_zoomed;
    int prev_rows, prev_cols; // FIXME I don't think these are even used

    HBITMAP caretbm;
    int caret_x, caret_y;

    int kbd_codepage;

    Ldisc *ldisc;
    Backend *backend;

    cmdline_get_passwd_input_state cmdline_get_passwd_state;

    struct unicode_data ucsdata;
    bool session_closed;
    bool error_close;        /* KiTTY (cyd01/KiTTY #548): the session closed due to
                              * a FATAL error; close_session shows a warning-glyph
                              * titlebar marker. Reset per connection in start_backend. */
    bool reconfiguring;
    bool autopw_tried;       /* KiTTY auto-login: the stored CONF_password has been
                              * auto-answered once on THIS connection. Prevents
                              * re-sending a rejected password on every server
                              * re-prompt (which burns MaxAuthTries -> IP ban).
                              * Reset per connection in start_backend. */
    /* These fields MUST stay unconditional (do NOT wrap in #ifdef MOD_RECONNECT).
     * WinGuiSeat is shared between window.c (compiled into the kitty executable
     * target, where MOD_RECONNECT is a PRIVATE compile-def) and dialog.c (compiled
     * into the guiterminal static library, which does NOT get that def). Guarding
     * these on MOD_RECONNECT put `term` and every later field at different offsets
     * in the two TUs, so dialog.c read wgs->term from the wrong offset and crashed
     * writing to a bogus terminal during the inline host-key prompt. Keeping them
     * always present makes the layout identical in every TU. */
    time_t last_reconnect;   /* KiTTY auto-reconnect: wakeup de-bounce, per window */
    int    reconnect_tries;  /* KiTTY auto-reconnect: backoff/loop cap */
    bool   ever_authenticated; /* KiTTY: THIS session reached post-auth at least once.
                                * Gates auto-reconnect so an authentication failure is
                                * never re-dialed (which would burn auth attempts and get
                                * the client IP banned). Per-session, unlike the process-
                                * global is_backend_first_connected. */

    const SessionSpecial *specials;
    HMENU specials_menu;
    int n_specials;

    struct PopupMenu popup_menus[2];
    HMENU savedsess_menu;

    Conf *conf;
    LogContext *logctx;
    Terminal *term;

    int cursor_type;
    int vtmode;

    HFONT fonts[FONT_MAXNO];
    LOGFONT lfont;
    bool fontflag[FONT_MAXNO];
    enum BoldMode bold_font_mode;

    bool bold_colours;
    enum UnderlineMode und_mode;
    int descent, font_strikethrough_y;

    COLORREF colours[OSC4_NCOLOURS];
    HPALETTE pal;
    LPLOGPALETTE logpal;
    bool tried_pal;
    COLORREF colorref_modifier;

    struct _dpi_info dpi_info;

    int dbltime, lasttime, lastact;
    Mouse_Button lastbtn;

    bool send_raw_mouse;
    int wheel_accumulator;

    bool pointer_indicates_raw_mouse;

    BusyStatus busy_status;

    wchar_t *window_name, *icon_name;

    int alt_numberpad_accumulator;
    int compose_state;
    int compose_char;
    WPARAM compose_keycode;

    HDC wintw_hdc;

    bool resizing;
    bool need_backend_resize;

    long next_flash;
    bool flashing;

    long last_beep_time;

    bool ignore_clip;
    bool fullscr_on_max;
    bool processed_resize;
    bool embed_dpi_synced;   /* #554: font DPI re-detected after host reparent */
    bool in_scrollbar_loop;
    UINT last_mousemove;
    WPARAM last_wm_mousemove_wParam, last_wm_ncmousemove_wParam;
    LPARAM last_wm_mousemove_lParam, last_wm_ncmousemove_lParam;
    wchar_t pending_surrogate;
};

extern const LogPolicyVtable win_gui_logpolicy_vt; /* in dialog.c */

static inline void wgs_link(WinGuiSeat *wgs)
{
    wgs->wgslistnode.prev = wgslisthead.prev;
    wgs->wgslistnode.next = &wgslisthead;
    wgs->wgslistnode.prev->next = &wgs->wgslistnode;
    wgs->wgslistnode.next->prev = &wgs->wgslistnode;
}

static inline void wgs_unlink(WinGuiSeat *wgs)
{
    wgs->wgslistnode.prev->next = wgs->wgslistnode.next;
    wgs->wgslistnode.next->prev = wgs->wgslistnode.prev;
}
