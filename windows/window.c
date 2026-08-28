/*
 * window.c - the PuTTY(tel)/pterm main program, which runs a PuTTY
 * terminal emulator and backend in a window.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <assert.h>
#include <wchar.h>

#define COMPILE_MULTIMON_STUBS

#include "putty.h"
#include "../kitty/kitty_hello_keys.h"  /* KiTTY: Hello-protected key files */
#include "ssh.h"
#include "terminal.h"
#include "storage.h"
#include "putty-rc.h"
#include "security-api.h"
#include "win-gui-seat.h"
#include "tree234.h"

#ifdef NO_MULTIMON
#include <multimon.h>
#endif

#include <imm.h>
#include <commctrl.h>
#include <richedit.h>
#include <mmsystem.h>

/* From MSDN: In the WM_SYSCOMMAND message, the four low-order bits of
 * wParam are used by Windows, and should be masked off, so we shouldn't
 * attempt to store information in them. Hence all these identifiers have
 * the low 4 bits clear. Also, identifiers should < 0xF000. */

#define IDM_SHOWLOG   0x0010
#define IDM_NEWSESS   0x0020
#define IDM_DUPSESS   0x0030
#define IDM_RESTART   0x0040
#define IDM_RECONF    0x0050
#define IDM_CLRSB     0x0060
#define IDM_RESET     0x0070
#define IDM_HELP      0x0140
#define IDM_ABOUT     0x0150
#define IDM_SAVEDSESS 0x0160
#define IDM_COPYALL   0x0170
#define IDM_FULLSCREEN  0x0180
#define IDM_COPY      0x0190
#define IDM_PASTE     0x01A0
#define IDM_CHECKUPDATE 0x01B0  /* check GitHub releases for a newer KiTTY */
#ifdef MOD_PERSO
/* kitty.c: types a string into this session (the WM_COPYDATA broadcast). */
void SendKeyboardPlus( HWND hwnd, const char * st ) ;   /* kitty.c */
int  kitty_broadcast_default( void ) ;        /* kitty.c: [KiTTY] sendcmdmode */
const char *kitty_broadcast_group( void ) ;  /* kitty.c: which KiTTYs hear us */
#define IDM_BROADCASTTOGGLE 0x01C0           /* Tools > Accept broadcast */
/*
 * ONE state, two editors. The arming lives in the session's own conf
 * (CONF_kitty_accept_broadcast), so Session > Scripting and this window's Tools
 * toggle are two views of the SAME value: flip the menu item and Change Settings
 * shows it; press Apply and the menu tick follows.
 *
 * A first attempt kept it in a window property instead. That was a SECOND state
 * the config box could not see, and every awkward question about "what wins on
 * Apply" came from it. Unsaved changes revert when the session is next loaded,
 * which is ordinary conf behaviour and exactly what is wanted.
 */
static int kitty_broadcast_armed( WinGuiSeat *wgs )
{
    return wgs && wgs->conf && conf_get_bool( wgs->conf, CONF_kitty_accept_broadcast );
}
#endif /* MOD_PERSO */
#ifdef MOD_PERSO
#ifndef IDM_SCRIPTSEND
#define IDM_SCRIPTSEND  0xB180  /* send recorded script (rutty) */
#endif
#ifndef IDM_SCRIPTHALT
#define IDM_SCRIPTHALT  0xB190  /* stop running script */
#endif
#ifndef IDM_SCRIPTFILE2
#define IDM_SCRIPTFILE2 0xB1A0  /* send a script file (file picker) */
#endif
#ifndef IDM_NEWDUPSESS
#define IDM_NEWDUPSESS  0xB1B0  /* new duplicated session (new window) */
#endif
#ifndef IDM_MNOTEPAD
#define IDM_MNOTEPAD    0xB1C0  /* open KiTTY's embedded mNotepad editor */
#endif
#ifndef IDM_MNOTEPAD_CLIP
#define IDM_MNOTEPAD_CLIP 0xB1D0  /* open mNotepad with clipboard contents */
#endif
#endif
#ifdef MOD_RECONNECT
#ifndef IDM_RESTARTSESSION
#define IDM_RESTARTSESSION 0xB110  /* close current session and reconnect */
#endif
#endif
#ifndef IDM_REKEY
#define IDM_REKEY 0xB200  /* [Shortcuts] keyexchange -> SS_REKEY special */
#endif
#define IDM_SPECIALSEP 0x0200

#define IDM_SPECIAL_MIN 0x0400
#define IDM_SPECIAL_MAX 0x0800

#define IDM_SAVED_MIN 0x1000
#define IDM_SAVED_MAX 0x5000
#define MENU_SAVED_STEP 16
/* Maximum number of sessions on saved-session submenu */
#define MENU_SAVED_MAX ((IDM_SAVED_MAX-IDM_SAVED_MIN) / MENU_SAVED_STEP)

#define WM_IGNORE_CLIP (WM_APP + 2)
#define WM_FULLSCR_ON_MAX (WM_APP + 3)
#define WM_GOT_CLIPDATA (WM_APP + 4)

/* Needed for Chinese support and apparently not always defined. */
#ifndef VK_PROCESSKEY
#define VK_PROCESSKEY 0xE5
#endif

/* Mouse wheel support. */
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A           /* not defined in earlier SDKs */
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E          /* not defined in earlier SDKs */
#endif
#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

/* DPI awareness support */
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#define WM_DPICHANGED_BEFOREPARENT 0x02E2
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#define WM_GETDPISCALEDSIZE 0x02E4
#endif

/* VK_PACKET, used to send Unicode characters in WM_KEYDOWNs */
#ifndef VK_PACKET
#define VK_PACKET 0xE7
#endif

static Mouse_Button translate_button(WinGuiSeat *wgs, Mouse_Button button);
static void show_mouseptr(WinGuiSeat *wgs, bool show);
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static int TranslateKey(WinGuiSeat *wgs, UINT message, WPARAM wParam,
                        LPARAM lParam, unsigned char *output);
static void init_palette(WinGuiSeat *wgs);
static void init_fonts(WinGuiSeat *wgs, int, int);
static void init_dpi_info(WinGuiSeat *wgs);
static void another_font(WinGuiSeat *wgs, int);
static void deinit_fonts(WinGuiSeat *wgs);
static void set_input_locale(WinGuiSeat *wgs, HKL);
static void update_savedsess_menu(WinGuiSeat *wgs);
static void init_winfuncs(void);

static bool is_full_screen(WinGuiSeat *wgs);
static void make_full_screen(WinGuiSeat *wgs);
static void clear_full_screen(WinGuiSeat *wgs);
static void flip_full_screen(WinGuiSeat *wgs);
static void process_clipdata(WinGuiSeat *wgs, HGLOBAL clipdata, bool unicode);
static void setup_clipboards(Terminal *, Conf *);

/* Window layout information */
static void reset_window(WinGuiSeat *wgs, int reinit);
#ifdef MOD_PERSO
void kitty_set_active_seat(WinGuiSeat *wgs);
void InitWinMain(void);
int WINAPI Notepad_WinMain(HINSTANCE, HINSTANCE, LPSTR, int); /* blocnote hidden editor */
#ifdef MOD_LAUNCHER
int WINAPI Launcher_WinMain(HINSTANCE, HINSTANCE, LPSTR, int); /* session launcher window */
#endif
void ReadInitScript(const char *filename);     /* kitty.c: load a login script file */
void SaveRegistryKey(void);                    /* kitty.c: back up the registry hive to kitty.sav */
extern char *kitty_cli_loginscript;            /* kitty_bridge.c: -loginscript path, consumed post-create */
void ManageInitScript(const char *input_str, const int len); /* kitty.c: scan server output, auto-reply to login prompt */
extern char *ScriptFileContent;                /* kitty.c: loaded login-script buffer (NULL = none) */
extern HWND MainHwnd;                          /* kitty.c/bridge: active terminal hwnd for keystroke injection */
void CheckVersionFromWebSite(HWND hwnd, int is_terminal);   /* kitty_win.c: query GitHub releases for an update */
void RunPuttyEd(HWND hwnd, char *filename);     /* kitty_win.c: open embedded mNotepad editor */
void kitty_start_update_check(void);           /* kitty_win.c: async refresh of cached latest version */
int kitty_update_notice(char *buf, int n);     /* kitty_win.c: notice text if a newer version is cached */
void kitty_apply_transparency(WinGuiSeat *wgs);
void kitty_apply_window_pos(WinGuiSeat *wgs);
void kitty_save_window_placement(HWND hwnd);
void kitty_send_to_tray(HWND);
int RestoreFromTray(HWND);            /* kitty.c: restore a window from the systray */
#define MYWM_NOTIFYICON (WM_USER+3)  /* tray-icon click callback (matches kitty.c) */
void kitty_rollup(HWND, int);
void kitty_font_resize(Terminal*, Conf*, int);
void kitty_protect(HWND, TermWin*, Conf*);
void kitty_print(HWND);
void kitty_negative(HWND);
void kitty_bw(HWND);
extern int force_reconf;   /* kitty_bridge.c: 0 => apply conf silently (no dialog) */
void kitty_showportfwd(HWND, Conf*);
void kitty_shortcuts_toggle(HWND);
/* KiTTY shortcut/ctrl-tab engine (kitty.c / kitty_commun.c) */
int GetPuttyFlag(void);
int GetModalErrorsFlag(void);   /* kitty_commun.c: modal vs inline error surfacing */
void OnDropFiles(HWND hwnd, HDROP hDropInfo);   /* KiTTY drag-drop pscp upload (kitty_xfer.c) */
int GetTransparencyFlag(void);
int GetShortcutsFlag(void);
int GetMouseShortcutsFlag(void);
int GetCtrlTabFlag(void);
int GetProtectFlag(void);
int GetSizeFlag(void);      /* kitty.c: [KiTTY] size - live [rows x cols] title suffix */
int GetTitleBarFlag(void);  /* kitty.c: [KiTTY] wintitle - title decorations on/off */
void kitty_refresh_title(void);  /* below: re-apply the title decorations */
extern char KiTTYClassName[128];
int ManageShortcuts(Terminal *term, Conf *conf, HWND hwnd,
                    const int *clips_system, int key_num, int shift_flag,
                    int control_flag, int alt_flag, int altgr_flag, int win_flag);
/* KiTTY predefined-command shortcuts (User Command menu + Ctrl+Shift+A..Z) */
void InitSpecialMenu(HMENU m, const char *folder, const char *sessionname);
void ManageSpecialCommand(HWND hwnd, int menunum);
#ifndef IDM_USERCMD
#define IDM_USERCMD 0x8000
#endif
#ifndef NB_MENU_MAX
#define NB_MENU_MAX 1024
#endif
void kitty_start_winscp(HWND);
void kitty_send_file(HWND);
void kitty_export_settings(HWND, Conf*);
void kitty_dup_session(HWND, Conf*);
int GetAutoSendToTray(void);
void SetAutoSendToTray(const int flag);
int GetZModemFlag(void);
/* URL hyperlinks (kitty_url.c + kitty.c flag) */
int  GetHyperlinkFlag(void);
void SetHyperlinkFlag(const int flag);
void kitty_url_init(void);
void kitty_url_config(Conf *conf);
int kitty_url_rescan(Terminal *term);
int kitty_url_hover(Terminal *term, HWND hwnd, int cx, int cy, int hover_cursor);
void kitty_term_print_inline_error(Terminal *term, const char *msg, int fatal);
void kitty_menu_adjust_transparency(HWND term_hwnd, Conf *conf, int up);
void kitty_sync_transparency_menu(HMENU menu, Conf *conf, UINT id_up,
                                  UINT id_down, UINT id_anchor);
void kitty_menu_toggle_alwaysontop(HWND term_hwnd, Conf *conf);
void kitty_menu_reposition(HWND term_hwnd, Conf *conf, int x, int y);
void kitty_menu_toggle_hyperlink(HWND hwnd);
int kitty_url_click(Terminal *term, Conf *conf, int x, int y, int ctrl_down);
int kitty_url_cell_in_link(int col, int row);
int kitty_url_row_dirty(int row);
/* Per-session icon (CONF_icone / CONF_iconefile). */
void kitty_apply_icon(HWND hwnd, Conf *conf);
/* Restore the normal icon after a reconnect (undo SetConnBreakIcon). */
void kitty_restore_icon(HWND hwnd, Conf *conf);
/* KiTTY-specific About dialog. */
void kitty_about(HWND hwnd);
#ifdef MOD_PORTKNOCKING
/* Port-knocking: knock the configured host:port sequence before connecting. */
void kitty_port_knock(Conf *conf);
#endif
#ifdef MOD_PROXY
/* Proxy selection: overlay a named saved proxy definition before connecting. */
void kitty_proxy_select(Conf *conf);
/* Remember the proxy this connection resolved to, for the transfer helpers that
 * only ever see the session Conf (see kitty/kitty.h). NULL = no override. */
void kitty_proxy_record_connection(Conf *resolved);
#endif
#ifdef MOD_ZMODEM
/* ZModem file transfer (kitty_zmodem.c). Menu-driven receive (rz) / send (sz);
 * receive data is intercepted in win_seat_output, send is pumped from the
 * message loop. No terminal.c edits. */
int kitty_zmodem_active(void);
int kitty_zmodem_receive(Conf *conf, Backend *backend, LogContext *logctx, Terminal *term);
int kitty_zmodem_send(HWND owner, Conf *conf, Backend *backend, LogContext *logctx, Terminal *term);
void kitty_zmodem_cancel(void);
size_t kitty_zmodem_recv_data(const void *data, size_t len);
int kitty_zmodem_process(void);
#endif
#ifdef MOD_BACKGROUNDIMAGE
/* Background image: load the configured image (CONF_bg_image_filename etc.). */
int kitty_apply_background(HWND hwnd, Conf *conf);
/* KiTTY background slideshow: advance to next image (kitty.c, active-seat conf). */
int NextBgImage(HWND hwnd);
int GetBackgroundImageFlag(void);
extern int ImageSlideDelay;
#define TIMER_SLIDEBG_WIN 8710   /* free across window.c + kitty.c timer ids */
#endif
#ifdef MOD_PERSO
/* KiTTY rutty scripting (kitty_rutty.c). Sends a script file line by line and,
 * in wait-for-prompt mode, waits for a pattern in the incoming host data
 * (waitfor) before each line / aborts on halton. Observe-hooked in
 * win_seat_output; no terminal.c edits. */
int  kitty_script_active(void);
int  kitty_script_enabled(void);         /* [KiTTY] scriptmode master switch */
int  kitty_script_send_file(Conf *conf, Backend *backend, Filename *fn);
void kitty_script_remote(const void *data, size_t len);
void kitty_script_stop(void);
int  OpenFileName(HWND hFrame, char *filename, char *Title, char *Filter); /* kitty_win.c */
void OpenAndSendScriptFile(HWND hwnd);   /* kitty.c: legacy autocommand script */
int  GetWinrolFlag(void);                /* kitty.c */
void RunSessionWithCurrentSettings(HWND hwnd, Conf *oldconf, const char *host,
                                   const char *user, const char *pass,
                                   const int port, const char *remotepath); /* kitty_bridge.c */
void RunConfigBoxWithConfSettings(Conf *conf); /* kitty_bridge.c */
int  GetLoadLastSessionFlag(void);       /* kitty.c: [ConfigBox] loadlastsession */
int  GetQuickConnectMode(void);          /* kitty.c: quick connect armed this run */
/* KiTTY font fallback (kitty/winfont_fallback.c, ported from upstream PR
 * cyd01/KiTTY#555): characters the primary font lacks are drawn from a
 * configurable list of fallback fonts. kitty.ini [FontFallback]. */
#include "../kitty/winfont_fallback.h"
#endif
/* Auto-command: send a command automatically after login (CONF_autocommand). */
int kitty_autocommand_tick(HWND hwnd);
void kitty_autocommand_rearm(void);  /* reset for a NEW connection */
extern int autocommand_delay;
extern int init_delay;      /* kitty.c: [KiTTY] initdelay, ms before the first
                             * auto-command/auto-password send (default 2000) */
int GetPasteSize(void);     /* kitty.c: [KiTTY] pastesize, confirm before
                             * pasting more than N chars (0 = unlimited) */
int GetProxyChainMax(void); /* kitty.c: [KiTTY] proxychainmax, how many SSH
                             * proxies may be chained before we refuse
                             * (default 5); enforced in proxy/sshproxy.c */
/* KiTTY: set by kitty_proxy_select() (kitty/kitty_bridge.c) when the proxy it
 * just applied came from workplace proxy mode; start_backend records it on the
 * seat, where it stays true for the life of that connection. Declared here
 * rather than by including a KiTTY header, to leave this shared file's includes
 * as they are; every use is inside MOD_PERSO. */
extern int kitty_workplace_applied;
void kitty_frame_restore_resting(void);   /* kitty/kitty_osc52.c: the frame's
                                           * standing colour for this window */
int kitty_workplace_query(char *name, int len);   /* kitty/kitty_workplace.c */
int kitty_workplace_request(int arm, unsigned int minutes);
#include "../kitty/kitty_notice.h"  /* kitty_notice_show + the notice click
                                     * messages (WM_KITTY_AGENT_UNVERIFIED) */
#include "../kitty/kitty_theme.h"   /* KiTTY: dark mode for the dialogs */
/* KiTTY: whether to look for a new release at startup - an application
 * setting in kitty.ini, not a per-session one (kitty/kitty_win.c). */
int kitty_check_update_enabled(void);
bool kitty_theme_app_dark(void);    /* kitty/kitty_win.c: the app-wide setting */
void kitty_workplace_show_pending_notice(void);
void kitty_cfgbox_open_on_panel(const char *path);   /* kitty/kitty_config.c */
/* Posted by that notice when it is clicked: switch workplace proxy mode off. */
#define WM_KITTY_WORKPLACE_DISARM (WM_APP + 72)
#define TIMER_AUTOCOMMAND 8702
/* Anti-idle: periodically send a keepalive string (CONF_antiidle). */
void kitty_antiidle_tick(HWND hwnd);
extern char AntiIdleStr[128];
extern int AntiIdleSeconds;   /* KiTTY: [KiTTY] antiidledelay, in seconds */
#define TIMER_ANTIIDLE 8703
#define TIMER_SCRIPT 8704
#ifdef MOD_PERSO
#define TIMER_EMBEDFILL 8706   /* #554: poll host client rect, keep embedded child filling it */
#define TIMER_SENDTOTRAY 8711  /* free across window.c + kitty.c timer ids */
/* Session > Logging > "Log rotation delay": start a new log file every N
 * seconds (CONF_logtimerotation). Repeating; logfile_rotate() decides whether
 * rotating is actually safe.
 * ⚠️ 8712 is TIMER_CLIPACTIVITY (windows/kitty_rc_additions.h) - using it here
 * put this timer behind that branch, which KillTimer()s it, so rotation fired
 * once and never again. When picking an id, grep windows/*.h too, not just the
 * .c files and kitty.h (which has its own unused TIMER_LOGROTATION 8707). */
#define TIMER_LOGROTATION 8713
#endif
#ifdef MOD_RECONNECT
#define TIMER_RECONNECT 8705
int  GetAutoreconnectFlag(void);       /* kitty.c */
int  GetReconnectDelay(void);          /* kitty.c, seconds, clamped >=1 */
void SetConnBreakIcon(HWND hwnd);      /* kitty.c */
void SetSSHConnected(int flag);        /* kitty_commun.c: sets is_backend_first_connected */
extern int is_backend_first_connected; /* kitty_commun.c */
#endif
#ifdef MOD_PERSO
int  GetConfigBoxNoExitFlag(void);     /* kitty.c: [ConfigBox] noexit */
void kitty_respawn_config_box(void);   /* kitty_win.c */
#endif
#endif

static void flash_window(WinGuiSeat *wgs, int mode);
static void sys_cursor_update(WinGuiSeat *wgs);
static bool get_fullscreen_rect(WinGuiSeat *wgs, RECT *ss);
static bool get_workingarea_rect(WinGuiSeat *wgs, RECT *ss);

static void conf_cache_data(WinGuiSeat *wgs);

static struct sesslist sesslist;       /* for saved-session menu */

enum MONITOR_DPI_TYPE { MDT_EFFECTIVE_DPI, MDT_ANGULAR_DPI, MDT_RAW_DPI, MDT_DEFAULT };
DECL_WINDOWS_FUNCTION(static, BOOL, GetMonitorInfoA, (HMONITOR, LPMONITORINFO));
DECL_WINDOWS_FUNCTION(static, HMONITOR, MonitorFromPoint, (POINT, DWORD));
DECL_WINDOWS_FUNCTION(static, HMONITOR, MonitorFromWindow, (HWND, DWORD));
DECL_WINDOWS_FUNCTION(static, HRESULT, GetDpiForMonitor, (HMONITOR hmonitor, enum MONITOR_DPI_TYPE dpiType, UINT *dpiX, UINT *dpiY));
DECL_WINDOWS_FUNCTION(static, HRESULT, GetSystemMetricsForDpi, (int nIndex, UINT dpi));
DECL_WINDOWS_FUNCTION(static, HRESULT, AdjustWindowRectExForDpi, (LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle, UINT dpi));

static UINT wm_mousewheel = WM_MOUSEWHEEL;

#ifdef MOD_PERSO
/* KiTTY #554: -hwndparent <decimal HWND> embeds the terminal window as a child of
 * a host application's window (mRemoteNG / Remote4Support). Set from the command
 * line (windows/putty.c). When non-NULL the terminal is reparented just AFTER
 * creation (passing the parent to CreateWindow breaks keyboard focus -- per the
 * Remote4Support fork this is modelled on) and top-level-only behaviours are
 * suppressed: saved window-position memory, maximise/fullscreen-on-start,
 * always-on-top and send-to-tray. The host app owns sizing and lifecycle. */
HWND kitty_hwnd_parent = NULL;
static HWND kitty_hwnd_parent_main = NULL;
#define KITTY_EMBEDDED() (kitty_hwnd_parent != NULL)
/* #554: the host may embed us WITHOUT -hwndparent by reparenting our window
 * itself (this is what mRemoteNG actually does). So detect embedding at runtime:
 * the effective host is the explicit -hwndparent, else our actual parent window.
 * A normal top-level KiTTY has no parent, so this is NULL and behaviour is
 * unchanged. */
static HWND kitty_embed_host(HWND h)
{
    if (kitty_hwnd_parent) return kitty_hwnd_parent;
    /* mRemoteNG (and the Remote4Support fork) embed by calling SetParent WITHOUT
     * setting WS_CHILD. For such a window GetParent() returns the OWNER (NULL),
     * not the parent -- so it misses the embed. GetAncestor(GA_PARENT) returns
     * the true parent; if that's not the desktop, we're embedded in a host. */
    HWND p = GetAncestor(h, GA_PARENT);
    if (p && p != GetDesktopWindow())
        return p;
    return NULL;
}
#define KITTY_EMBED_HOST(h)  kitty_embed_host(h)
#define KITTY_IS_EMBEDDED(h) (kitty_embed_host(h) != NULL)
#else
#define KITTY_EMBEDDED() 0
#define KITTY_EMBED_HOST(h)  (NULL)
#define KITTY_IS_EMBEDDED(h) 0
#endif

struct WinGuiSeatListNode wgslisthead = {
    .next = &wgslisthead, .prev = &wgslisthead,
};

#define IS_HIGH_VARSEL(wch1, wch2) \
    ((wch1) == 0xDB40 && ((wch2) >= 0xDD00 && (wch2) <= 0xDDEF))
#define IS_LOW_VARSEL(wch) \
    (((wch) >= 0x180B && (wch) <= 0x180D) || /* MONGOLIAN FREE VARIATION SELECTOR */ \
     ((wch) >= 0xFE00 && (wch) <= 0xFE0F)) /* VARIATION SELECTOR 1-16 */

static bool wintw_setup_draw_ctx(TermWin *);
static void wintw_draw_text(TermWin *, int x, int y, wchar_t *text, int len,
                            unsigned long attrs, int lattrs, truecolour tc);
static void wintw_draw_cursor(TermWin *, int x, int y, wchar_t *text, int len,
                              unsigned long attrs, int lattrs, truecolour tc);
static void wintw_draw_trust_sigil(TermWin *, int x, int y);
static int wintw_char_width(TermWin *, int uc);
static void wintw_free_draw_ctx(TermWin *);
static void wintw_set_cursor_pos(TermWin *, int x, int y);
static void wintw_set_raw_mouse_mode(TermWin *, bool enable);
static void wintw_set_raw_mouse_mode_pointer(TermWin *, bool enable);
static void wintw_set_scrollbar(TermWin *, int total, int start, int page);
static void wintw_bell(TermWin *, int mode);
static void wintw_clip_write(
    TermWin *, int clipboard, wchar_t *text, int *attrs,
    truecolour *colours, int len, bool must_deselect);
static void wintw_clip_request_paste(TermWin *, int clipboard);
static void wintw_refresh(TermWin *);
static void wintw_request_resize(TermWin *, int w, int h);
static void wintw_set_title(TermWin *, const char *title, int codepage);
static void wintw_set_icon_title(TermWin *, const char *icontitle,
                                 int codepage);
static void wintw_set_minimised(TermWin *, bool minimised);
static void wintw_set_maximised(TermWin *, bool maximised);
static void wintw_move(TermWin *, int x, int y);
static void wintw_set_zorder(TermWin *, bool top);
static void wintw_palette_set(TermWin *, unsigned, unsigned, const rgb *);
static void wintw_palette_get_overrides(TermWin *, Terminal *);
static void wintw_unthrottle(TermWin *win, size_t bufsize);

static const TermWinVtable windows_termwin_vt = {
    .setup_draw_ctx = wintw_setup_draw_ctx,
    .draw_text = wintw_draw_text,
    .draw_cursor = wintw_draw_cursor,
    .draw_trust_sigil = wintw_draw_trust_sigil,
    .char_width = wintw_char_width,
    .free_draw_ctx = wintw_free_draw_ctx,
    .set_cursor_pos = wintw_set_cursor_pos,
    .set_raw_mouse_mode = wintw_set_raw_mouse_mode,
    .set_raw_mouse_mode_pointer = wintw_set_raw_mouse_mode_pointer,
    .set_scrollbar = wintw_set_scrollbar,
    .bell = wintw_bell,
    .clip_write = wintw_clip_write,
    .clip_request_paste = wintw_clip_request_paste,
    .refresh = wintw_refresh,
    .request_resize = wintw_request_resize,
    .set_title = wintw_set_title,
    .set_icon_title = wintw_set_icon_title,
    .set_minimised = wintw_set_minimised,
    .set_maximised = wintw_set_maximised,
    .move = wintw_move,
    .set_zorder = wintw_set_zorder,
    .palette_set = wintw_palette_set,
    .palette_get_overrides = wintw_palette_get_overrides,
    .unthrottle = wintw_unthrottle,
};

static HICON trust_icon = INVALID_HANDLE_VALUE;

const bool share_can_be_downstream = true;
const bool share_can_be_upstream = true;

static bool is_utf8(WinGuiSeat *wgs)
{
    return wgs->ucsdata.line_codepage == CP_UTF8;
}

static bool win_seat_is_utf8(Seat *seat)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    return is_utf8(wgs);
}

static char *win_seat_get_ttymode(Seat *seat, const char *mode)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    return term_get_ttymode(wgs->term, mode);
}

static StripCtrlChars *win_seat_stripctrl_new(
    Seat *seat, BinarySink *bs_out, SeatInteractionContext sic)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    return stripctrl_new_term(bs_out, false, 0, wgs->term);
}

static size_t win_seat_output(
    Seat *seat, SeatOutputType type, const void *, size_t);
static bool win_seat_eof(Seat *seat);
static SeatPromptResult win_seat_get_userpass_input(Seat *seat, prompts_t *p);
#ifdef MOD_RECONNECT
/* KiTTY auto-reconnect: fired (via the seat vtable) when the main SSH channel
 * opens post-auth. Marks this session as having connected at least once, which
 * gates SSH reconnect. Runs in the kitty target where SetSSHConnected links;
 * no change to the ssh/ library (keeps plink/pscp/psftp unaffected). */
static void win_seat_notify_session_started(Seat *seat)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    SetSSHConnected(1);
#ifdef MOD_RECONNECT
    /* This SSH session has now completed authentication at least once, so a
     * later network drop is a genuine reconnect candidate (an auth failure,
     * which never reaches here, is not). */
    wgs->ever_authenticated = true;
#endif
    /* KiTTY: SSH session is (re)connected post-auth - restore the normal
     * window icon so a prior SetConnBreakIcon() drop no longer shows. */
    kitty_restore_icon(wgs->term_hwnd, wgs->conf);
#ifdef MOD_PERSO
    /* KiTTY: kick off the async update check once per process, and (once) show a
     * cached "update available" notice here at the clean top of the session.
     * The fetch is async (worker thread refreshing a registry cache); only this
     * synchronous, top-of-session render touches the terminal, so a full-screen
     * TUI is never corrupted by a mid-session injection. */
    /* An application setting now, not a per-session one: see
     * kitty_check_update_enabled() in kitty/kitty_win.c. */
    if (kitty_check_update_enabled()) {
        kitty_start_update_check();
        static int update_notice_shown = 0;
        if (!update_notice_shown) {
            char nb[512];
            if (kitty_update_notice(nb, sizeof(nb))) {
                update_notice_shown = 1;
                /* nb is UTF-8; render as Unicode via term_data_wide so it is
                 * encoded into the terminal's current charset (no mojibake). */
                WCHAR wnb[512];
                int wn = MultiByteToWideChar(CP_UTF8, 0, nb, -1, wnb, lenof(wnb));
                if (wn > 1)
                    term_data_wide(wgs->term, wnb, (size_t)(wn - 1)); /* drop NUL */
                else
                    term_data(wgs->term, nb, strlen(nb));             /* fallback */
            }
        }
    }
#endif
}
#endif

static void win_seat_notify_remote_exit(Seat *seat);
static void win_seat_connection_fatal(Seat *seat, const char *msg);
#ifdef MOD_RECONNECT
static void win_seat_notify_session_started(Seat *seat);
#endif
static void win_seat_nonfatal(Seat *seat, const char *msg);
static void win_seat_update_specials_menu(Seat *seat);
static void win_seat_set_busy_status(Seat *seat, BusyStatus status);
static void win_seat_set_trust_status(Seat *seat, bool trusted);
static bool win_seat_can_set_trust_status(Seat *seat);
static bool win_seat_get_cursor_position(Seat *seat, int *x, int *y);
static bool win_seat_get_window_pixel_size(Seat *seat, int *x, int *y);

static const SeatVtable win_seat_vt = {
    .output = win_seat_output,
    .eof = win_seat_eof,
    .sent = nullseat_sent,
    .banner = nullseat_banner_to_stderr,
    .get_userpass_input = win_seat_get_userpass_input,
#ifdef MOD_RECONNECT
    .notify_session_started = win_seat_notify_session_started,
#else
    .notify_session_started = nullseat_notify_session_started,
#endif
    .notify_remote_exit = win_seat_notify_remote_exit,
    .notify_remote_disconnect = nullseat_notify_remote_disconnect,
    .connection_fatal = win_seat_connection_fatal,
    .nonfatal = win_seat_nonfatal,
    .update_specials_menu = win_seat_update_specials_menu,
    .get_ttymode = win_seat_get_ttymode,
    .set_busy_status = win_seat_set_busy_status,
    .confirm_ssh_host_key = win_seat_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = win_seat_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = win_seat_confirm_weak_cached_hostkey,
    .prompt_descriptions = win_seat_prompt_descriptions,
    .is_utf8 = win_seat_is_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_display = nullseat_get_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = win_seat_get_window_pixel_size,
    .stripctrl_new = win_seat_stripctrl_new,
    .set_trust_status = win_seat_set_trust_status,
    .can_set_trust_status = win_seat_can_set_trust_status,
    .has_mixed_input_stream = nullseat_has_mixed_input_stream_yes,
    .verbose = nullseat_verbose_yes,
    .interactive = nullseat_interactive_yes,
    .get_cursor_position = win_seat_get_cursor_position,
};

#ifdef MOD_RECONNECT
static void close_session(void *vctx);   /* defined below; used by reconnect path */
#endif
static void start_backend(WinGuiSeat *wgs)
{
    const struct BackendVtable *vt;
    char *error, *realhost;
    int i;

    wgs->cmdline_get_passwd_state = cmdline_get_passwd_input_state_new;

    vt = backend_vt_from_conf(wgs->conf);

#ifdef MOD_PORTKNOCKING
    /* KiTTY feature: knock the configured port sequence before connecting. */
    kitty_port_knock(wgs->conf);
#endif
#ifdef MOD_PROXY
    /*
     * KiTTY feature: apply the selected proxy override for THIS CONNECTION ONLY.
     *
     * ⚠️ Applied to a THROWAWAY COPY, never to wgs->conf. It used to be applied
     * to the seat's own conf, and that destroyed user data: the preset's fields
     * were written over the session's own proxy settings, so anything that later
     * saved the session persisted them - and a proxy password that existed only
     * in that session was gone. It also left sessions in a split state, with the
     * stored fields saying one thing and the remembered choice another, so which
     * proxy you got depended on whether you had connected before saving.
     * The rule is that an override may amend the connection and must never
     * rewrite the stored session.
     *
     * Safe because backends copy the Conf they are given (ssh_init does
     * conf_copy), so the connection keeps its own copy after this function
     * returns and the temporary can be freed immediately.
     *
     * Everything else here still reads wgs->conf, which is correct: the session
     * is unchanged, and only the transport is being routed differently.
     */
    /* KiTTY: publish the proxy-chain limit into the Conf. proxy/sshproxy.c enforces
     * it but is built without MOD_PERSO, so it cannot read the kitty.ini value
     * itself; the Conf is the only channel it shares with us. NOT_SAVED, so this
     * never reaches a session file. */
    conf_set_int(wgs->conf, CONF_proxy_chain_max, GetProxyChainMax());

    Conf *connconf = wgs->conf;
    Conf *proxyconf = NULL;
    {
        Conf *tmp = conf_copy(wgs->conf);
        kitty_proxy_select(tmp);
        /* Whether workplace proxy mode routed THIS connection - recorded now,
         * because it stays true of the connection for as long as it lives, and
         * the mode itself can be switched off meanwhile. Every start path comes
         * through here, including auto-reconnect and Restart Session, so a
         * reconnect re-answers the question rather than inheriting the answer. */
        wgs->workplace_proxied = (kitty_workplace_applied != 0);
        if (conf_get_int(tmp, CONF_proxy_type) !=
            conf_get_int(wgs->conf, CONF_proxy_type) ||
            strcmp(conf_get_str(tmp, CONF_proxy_host),
                   conf_get_str(wgs->conf, CONF_proxy_host)) ||
            conf_get_int(tmp, CONF_proxy_port) !=
            conf_get_int(wgs->conf, CONF_proxy_port)) {
            /* an override really is in force - connect through the copy */
            proxyconf = tmp;
            connconf = tmp;
        }
        /* Tell the transfer helpers how this connection is really reaching the
         * host, BEFORE the copy goes: it is freed either here or as soon as
         * backend_init() returns, and they read the session Conf, which by
         * design does not know. Recorded from the resolved copy in both cases,
         * not only when the test above fired: that test asks whether to connect
         * differently, and credentials alone do not change that answer while
         * they do change what WinSCP must be told. When nothing was overridden
         * the copy still holds the session's own settings, so recording it is
         * the same answer either way. Every connect comes through here, so a
         * reconnect re-answers rather than leaving the old answer standing. */
        kitty_proxy_record_connection(tmp);
        if (!proxyconf)
            conf_free(tmp);       /* nothing overridden; keep it simple */
    }
#else
    Conf *connconf = wgs->conf;
#endif

#ifdef MOD_PERSO
    /*
     * KiTTY: (re-)arm the session's login script for the connection about to be
     * made. ReadInitScript self-skips when nothing is stored.
     *
     * Here rather than at session setup, because ManageInitScript CONSUMES entries
     * as they match: once a script has run it is spent, so a session that drops
     * and reconnects would log in the first time and then sit at a login prompt
     * for ever - precisely the case auto-reconnect exists for. Classic KiTTY
     * re-armed it in both of its reconnect paths for this reason.
     *
     * start_backend is the one funnel for initial connect, Restart Session and
     * MOD_RECONNECT auto-reconnect alike (see the MOD_PROXY note above), so one
     * call covers what classic needed three for.
     *
     * Re-reading from the Conf is what makes this work: ManageInitScript mutates
     * only the runtime copy, so CONF_scriptfilecontent still holds the whole
     * script. It is also why the -loginscript command line stays safe - that path
     * writes what it loaded into the Conf, so re-arming reproduces it rather than
     * overwriting it.
     */
    ReadInitScript(NULL);

    /*
     * ...and RESET the SESSION > SCRIPTING (rutty) engine, for the same reason
     * and in the same place. kitty_script_send_file() refuses while a script is
     * still marked as running - and a script interrupted by a drop is exactly
     * that, so without this the next connect would be refused for ever
     * (hknet/KiTTY#36 covered the arming half; this is the reset half).
     * kitty_script_stop() is safe when nothing is running and logs nothing.
     *
     * The script itself is STARTED after backend_init below, not here: the
     * engine can only match output received after it starts, so it has to be
     * observing before the connection's first byte can arrive.
     */
    if (conf_get_int(wgs->conf, CONF_script_mode) == 1) {
        Filename *sf = conf_get_filename(wgs->conf, CONF_scriptfile);
        if (sf && filename_to_str(sf)[0])
            kitty_script_stop();
    }
#endif

    seat_set_trust_status(&wgs->seat, true);
    /* connconf == wgs->conf unless a proxy override is in force, in which case it
     * is the throwaway copy carrying it (see the MOD_PROXY note above). Host,
     * port and the TCP options are identical in both - only the proxy differs. */
    error = backend_init(vt, &wgs->seat, &wgs->backend, wgs->logctx, connconf,
                         conf_get_str(wgs->conf, CONF_host),
                         conf_get_int(wgs->conf, CONF_port),
                         &realhost,
                         conf_get_bool(wgs->conf, CONF_tcp_nodelay),
                         conf_get_bool(wgs->conf, CONF_tcp_keepalives));
#ifdef MOD_PROXY
    /* The backend has taken its own copy by now. */
    if (proxyconf) { conf_free(proxyconf); proxyconf = NULL; connconf = wgs->conf; }
#endif
    if (error) {
        char *str = dupprintf("%s Error", appname);
        char *msg;
        if (cmdline_tooltype & TOOLTYPE_NONNETWORK) {
            /* Special case for pterm. */
            msg = dupprintf("Unable to open terminal:\n%s", error);
        } else {
            msg = dupprintf("Unable to open connection to\n%s\n%s",
                            conf_dest(wgs->conf), error);
        }
        sfree(error);
#ifdef MOD_RECONNECT
        if (GetAutoreconnectFlag() && conf_get_int(wgs->conf, CONF_failure_reconnect)
            && wgs->ever_authenticated) {
            lp_eventlog(&wgs->logpolicy, msg);
            sfree(str); sfree(msg);
            SetSSHConnected(0);
            SetConnBreakIcon(wgs->term_hwnd);
            wgs->session_closed = true;
            queue_toplevel_callback(close_session, wgs);
            lp_eventlog(&wgs->logpolicy,
                        "Unable to connect, trying to reconnect...");
            if (wgs->reconnect_tries < 1000) {
                wgs->reconnect_tries++;
                SetTimer(wgs->term_hwnd, TIMER_RECONNECT,
                         GetReconnectDelay()*1000, NULL);
            }
            return;
        }
#endif
#ifdef MOD_PERSO
        /*
         * KiTTY: the one detection workplace proxy mode is worth having
         * (design/TASK_workplace_proxy.md §9). This connection went through the
         * mode's proxy and did not come up - overwhelmingly because the user has
         * left the place where that proxy exists. Ask, at the exact moment it
         * matters, instead of leaving them to work out why nothing connects any
         * more.
         *
         * It replaces the ordinary failure box rather than adding a second one,
         * and it states what happened rather than blaming the proxy: the failure
         * could equally be the far end. Answering No leaves the mode alone.
         */
        {
            char wp[256];
            if (wgs->workplace_proxied && kitty_workplace_query(wp, sizeof(wp))) {
                extern int kitty_confirm_box(HWND owner, const char *caption,
                                             const char *text, const char *warn_red);
                char *q = dupprintf(
                    "%s\n\n"
                    "Workplace proxy mode is on, so this connection was made "
                    "through the proxy \"%s\" rather than through this session's "
                    "own settings.\n\n"
                    "Switch workplace proxy mode off? Connections would then use "
                    "each session's own proxy settings again.", msg, wp);
                if (kitty_confirm_box(NULL, "Connection failed", q, NULL))
                    kitty_workplace_request(0, 0);
                sfree(q);
                sfree(str);
                sfree(msg);
                exit(0);
            }
        }
#endif
#ifdef MOD_PERSO
        /*
         * KiTTY (upstream cyd01/KiTTY #548, the last leftover of it): the
         * INITIAL connect failure - a host that does not resolve, a refused
         * port - used to be the one error that ignored `modalerrors` entirely.
         * Every other error surface honours it; this one always popped a box
         * and then exit(0)'d the process, so a mistyped hostname made the
         * window vanish behind an OK-click.
         *
         * Now it follows the same rule as connection_fatal: inline in the
         * terminal, window LEFT OPEN with the ⚠ titlebar marker, so the reason
         * can be read (and copied) and the user closes when ready. Restart
         * Session is right there for a typo.
         *
         * The box is still used where boxes are configured (modalerrors),
         * in PuTTY-compat mode, or when there is no terminal to print into -
         * printing where nothing can be seen is worse than a box.
         */
        if (!GetPuttyFlag() && !GetModalErrorsFlag() && wgs->term) {
            /* Name the window after what we FAILED to reach, before closing the
             * session down: the normal title is set further below, which this
             * path never reaches, so without this a failed window is called
             * plain "KiTTY" and ten of them are indistinguishable. */
            term_setup_window_titles(wgs->term,
                                     conf_get_str(wgs->conf, CONF_host));
            kitty_term_print_inline_error(wgs->term, msg, true);
            show_mouseptr(wgs, true);
            wgs->error_close = true;   /* ⚠ + "(disconnected)" via close_session */
            queue_toplevel_callback(close_session, wgs);
            sfree(str);
            sfree(msg);
            return;                    /* NOT exit(0): the window stays up */
        }
#endif
        MessageBox(NULL, msg, str, MB_ICONERROR | MB_OK);
        sfree(str);
        sfree(msg);
        exit(0);
    }
#ifdef MOD_PERSO
    /*
     * KiTTY: START the Session > Scripting (rutty) script for this connection,
     * now that the backend exists. Directly, not on a timer: the engine only
     * matches output received AFTER it starts, and a script whose first step
     * waits for the login prompt - the default - must be observing before the
     * first byte arrives. No data can flow until this function returns to the
     * message loop, so this point is early enough by construction; the old
     * 1500 ms timer start lost that race to any fast link, and the script died
     * on its own wait timeout without typing a byte.
     *
     * The TIMER_SCRIPT path remains for one case only: a login script
     * (Connection > Data) is pending, and the two engines must not react to
     * each other's output - the timer handler polls until the login script has
     * run out and only then starts this one (see the TIMER_SCRIPT handler).
     */
    wgs->script_deferred = false;
    if (conf_get_int(wgs->conf, CONF_script_mode) == 1) {
        Filename *sf = conf_get_filename(wgs->conf, CONF_scriptfile);
        if (sf && filename_to_str(sf)[0]) {
            if (ScriptFileContent != NULL && ScriptFileContent[0]) {
                /* Defer to the login script. The real start is in
                 * win_seat_output, the moment that script consumes its last
                 * entry - waiting for a TIMER tick there would reopen the
                 * missed-prompt race this block exists to close. The timer is
                 * kept only as the bounded fallback for a login script whose
                 * prompt never arrives. */
                wgs->script_deferred = true;
                wgs->script_defer_ticks = 0;
                SetTimer(wgs->term_hwnd, TIMER_SCRIPT, 1500, NULL);
            } else if (kitty_script_enabled()) {
                kitty_script_send_file(wgs->conf, wgs->backend, sf);
            }
        }
    }

    /*
     * ...and the AUTO-COMMAND (Connection > Data, CONF_autocommand), the
     * third of the three per-connection senders and the one the first #36
     * fix missed: its timer was armed once in WinMain, so the command ran on
     * the first connection of the process and never again - reconnect, and
     * nothing was typed (classic KiTTY re-ran it). Reported on the same
     * issue after 0.85.1.1-beta shipped the other two.
     *
     * The rearm drops any half-consumed copy a dropped connection left
     * behind, so the command always restarts from its first line. The first
     * fire keeps the same establishment delay the WinMain arming used.
     */
    {
        const char *ac = conf_get_str(wgs->conf, CONF_autocommand);
        if (ac && ac[0]) {
            kitty_autocommand_rearm();
            SetTimer(wgs->term_hwnd, TIMER_AUTOCOMMAND,
                     init_delay > 0 ? init_delay : 1500, NULL);
        }
    }
#endif
    term_setup_window_titles(wgs->term, realhost);
    sfree(realhost);

#ifdef MOD_PERSO
    /* KiTTY: the connection has just been made, so paint the window for what it
     * actually is - the workplace-proxy green belongs to a connection, and this
     * is the moment that answer changes. Also covers Restart Session and
     * auto-reconnect, which come through here too and may well answer
     * differently from the connection they replace. */
    kitty_frame_restore_resting();
    /* And if workplace proxy mode ended with nobody being told - the launcher
     * killed, the machine rebooted - say so once, here as well as in the
     * launcher and the config box, because a session started from a shortcut or
     * an ssh:// link may be the first KiTTY of the day. Self-clearing: whichever
     * path gets there first is the only one that shows it. */
    kitty_workplace_show_pending_notice();
#endif

    /*
     * Connect the terminal to the backend for resize purposes.
     */
    term_provide_backend(wgs->term, wgs->backend);

    /*
     * Set up a line discipline.
     */
    wgs->ldisc = ldisc_create(wgs->conf, wgs->term, wgs->backend, &wgs->seat);

    /*
     * Destroy the Restart Session menu item. (This will return
     * failure if it's already absent, as it will be the very first
     * time we call this function. We ignore that, because as long
     * as the menu item ends up not being there, we don't care
     * whether it was us who removed it or not!)
     */
    for (i = 0; i < lenof(wgs->popup_menus); i++) {
        DeleteMenu(wgs->popup_menus[i].menu, IDM_RESTART, MF_BYCOMMAND);
    }

#ifdef MOD_RECONNECT
    /* KiTTY auto-reconnect: mark first-connected (non-SSH here; SSH is marked
     * from notify_session_started) and reset the backoff counter on success. */
    if (conf_get_int(wgs->conf, CONF_protocol) != PROT_SSH) {
        is_backend_first_connected = 1;
        wgs->ever_authenticated = true; /* non-SSH has no auth phase; a connect counts */
        /* Non-SSH (re)connect: restore the normal icon (SSH does this from
         * notify_session_started once auth completes). */
        kitty_restore_icon(wgs->term_hwnd, wgs->conf);
    }
    wgs->last_reconnect = time(NULL);
    wgs->reconnect_tries = 0;
#endif
    wgs->autopw_tried = false;   /* new connection: allow one auto-password answer */
    wgs->session_closed = false;
    wgs->error_close = false;    /* #548: new connection clears the error titlebar marker */
}

#ifdef MOD_PERSO
/* hknet/KiTTY#22: the session's own title in UTF-8, or NULL if it never set
 * one. Taken from the Terminal rather than from the window, because the window
 * title is what we are about to overwrite - the Terminal's copy still holds the
 * connection's name and is never touched by a close, so no amount of closing
 * and reconnecting can nest markers. UTF-8 because the title may have arrived
 * in any codepage and has to be concatenated with the UTF-8 warning glyph. */
/*
 * KiTTY (classic parity): the four window-button settings.
 *
 * Two halves, because Windows expresses them in two different places. The system
 * menu and the minimise/maximise boxes are WINDOW STYLES, applied here and again
 * whenever the window is reconfigured. "Closable" is not a style at all - there is
 * no WS_ bit for it - so it is done by greying SC_CLOSE on the system menu, which
 * is what also greys the X in the caption and disables Alt+F4.
 *
 * WS_SYSMENU dominates: without it Windows draws no caption button at all, so
 * turning the system menu off hides close, minimise and maximise regardless of
 * what those three are set to.
 */
static LONG kitty_window_button_styles(Conf *conf, LONG style)
{
    if (!conf_get_bool(conf, CONF_window_has_sysmenu))
        style &= ~WS_SYSMENU;
    else
        style |= WS_SYSMENU;
    if (!conf_get_bool(conf, CONF_window_minimizable))
        style &= ~WS_MINIMIZEBOX;
    else
        style |= WS_MINIMIZEBOX;
    if (!conf_get_bool(conf, CONF_window_maximizable))
        style &= ~WS_MAXIMIZEBOX;
    else
        style |= WS_MAXIMIZEBOX;
    return style;
}

/* Grey (or restore) the Close item, which is what disables the X and Alt+F4.
 * MF_GRAYED rather than removing the item: a Close that is visibly unavailable
 * explains itself, where a missing one just looks like a bug. */
static void kitty_apply_close_button(WinGuiSeat *wgs, HWND hwnd)
{
    HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
    if (!sysmenu)
        return;
    EnableMenuItem(sysmenu, SC_CLOSE,
                   MF_BYCOMMAND |
                   (conf_get_bool(wgs->conf, CONF_window_closable)
                    ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
}

/* kitty/kitty_osc52.c: what clicking the most recent clipboard balloon should do
 * (CLIP_BALLOON_*), and re-applying the window's clipboard markers/tint. */
int kitty_clipboard_balloon_action(void);
void kitty_osc52_state_changed(Terminal *term);
void kitty_notice_box(HWND owner, const char *caption, const char *text); /* kitty_win.c */

/*
 * KiTTY: the clipboard markers on the window title, as icons.
 *
 * Two different things, told apart by POSITION and by BRACKETS:
 *
 *   "[]v Session"     - at the FRONT and bare: the host just DID something. Here
 *                       it wrote to your clipboard. Transient; clears itself.
 *   "Session ([]^)"   - at the END and in brackets: a standing PERMISSION is in
 *                       force. Brackets because a configuration is not an event,
 *                       and the two must not be confused at a glance.
 *
 * (Above written in ASCII; the real markers are U+1F4CB CLIPBOARD plus an arrow.)
 * The arrow says which way the data went: UP means it left you for the host, DOWN
 * means the host put something into your clipboard, UP-DOWN means both.
 *
 * Icons rather than the words "(clip read+write)" because the title bar is scarce
 * and shared with the connection name - three glyphs say it where fourteen
 * characters did. A pause sign is appended when a permission is suspended for want
 * of focus.
 *
 * Built as wide characters on purpose. Doing this in kitty_decorate_title() would
 * mean appending UTF-8 to a title that arrived in some other codepage.
 */
static const wchar_t *kitty_clip_icon(int dir, bool paused, bool standing)
{
    /*
     * SOLID SHAPES, not colour, and not thin line art.
     *
     * Two attempts got this wrong before this one, both for the same reason -
     * they were invisible to the person they exist for:
     *
     *  1. The clipboard glyph and a thin arrow. Line art, the same weight as the
     *     title text beside it. A title that changes by one small glyph looks
     *     unchanged.
     *  2. A coloured disc in front of it (U+1F7E0 and friends). Colour emoji do
     *     NOT render in colour in a Windows title bar: DWM draws caption text
     *     monochrome, so the disc arrived as a black-brown blob on the amber
     *     tint - worse than nothing, because it was mass with no meaning.
     *
     * So the title marker carries SHAPE and the frame tint carries COLOUR, and
     * neither tries to do the other's job. Solid BMP triangles: filled, high
     * contrast against any caption background, guaranteed monochrome so they
     * cannot be mangled by an emoji font, and the direction is readable at a
     * glance. UP means data left you for the host, DOWN means the host put
     * something into your clipboard.
     *
     * Colour still has to be optional-extra rather than the signal: the tint is
     * Windows 11 only, and on Windows 10 these glyphs are the whole marker.
     *
     * U+1F4CB is astral, so it is a surrogate pair; the triangles and the pause
     * sign are BMP.
     */
    if (standing) {
        /* the direction is still carried: a permission to read and a permission
         * to write are not the same standing state */
        if (dir == (CLIP_ACT_READ | CLIP_ACT_WRITE))
            return paused ? L"\U0001F4CB▲▼⏸" : L"\U0001F4CB▲▼";
        if (dir == CLIP_ACT_READ)
            return paused ? L"\U0001F4CB▲⏸" : L"\U0001F4CB▲";
        return paused ? L"\U0001F4CB▼⏸" : L"\U0001F4CB▼";
    }
    if (dir == (CLIP_ACT_READ | CLIP_ACT_WRITE))
        return L"\U0001F4CB▲▼";
    if (dir == CLIP_ACT_READ)
        return L"\U0001F4CB▲";
    return L"\U0001F4CB▼";
}

/* Wrap the converted title with whichever clipboard markers apply. Returns a
 * fresh string; the caller frees. */
static wchar_t *kitty_clip_decorate_wide(WinGuiSeat *wgs, wchar_t *name)
{
    int activity = 0, state = OSC52_PERM_NONE;
    bool rd = false, wr = false;
    const wchar_t *lead = NULL, *front = NULL, *tail = NULL;
    wchar_t *out;
    size_t len;

    if (!wgs->term)
        return name;

    if (conf_get_bool(wgs->conf, CONF_clipboard_activity_mark))
        activity = term_clipboard_activity(wgs->term);
    if (conf_get_bool(wgs->conf, CONF_osc52_title_mark)) {
        state = term_osc52_perm_state(wgs->term, &rd, &wr);
        if (state != OSC52_PERM_NONE)
            tail = kitty_clip_icon((rd ? CLIP_ACT_READ : 0) |
                                   (wr ? CLIP_ACT_WRITE : 0),
                                   state == OSC52_PERM_PAUSED, true);
    }
    if (activity)
        front = kitty_clip_icon(activity, false, false);
    /* KiTTY: THIS connection went through workplace proxy mode's proxy. Keyed to
     * the connection, not to the mode - a window opened before the mode was
     * switched on is not going through it and must not say so. In words, because
     * DWM renders the caption monochrome and a coloured glyph arrives as a black
     * blob: the title carries the shape, the frame tint carries the colour. */
    if (wgs->workplace_proxied)
        lead = L"⇄ workplace proxy";
    if (!front && !tail && !lead)
        return name;

    len = wcslen(name) + 1;
    if (lead)  len += wcslen(lead) + 3;
    if (front) len += wcslen(front) + 1;
    if (tail)  len += wcslen(tail) + 4;      /* " (" + ")" + slack */
    out = snewn(len, wchar_t);
    out[0] = L'\0';
    if (lead) {
        wcscat(out, lead);
        wcscat(out, L" ");
    }
    if (front) {
        wcscat(out, front);
        wcscat(out, L" ");
    }
    wcscat(out, name);
    if (tail) {
        wcscat(out, L" (");
        wcscat(out, tail);
        wcscat(out, L")");
    }
    sfree(name);
    return out;
}

static char *kitty_session_title_utf8(Terminal *term)
{
    wchar_t *wide;
    char *utf8;
    if (!term || !term->window_title || !*term->window_title)
        return NULL;
    wide = dup_mb_to_wc(term->wintitle_codepage, term->window_title);
    utf8 = dup_wc_to_mb(CP_UTF8, wide, "?");
    sfree(wide);
    return utf8;
}
#endif

static void close_session(void *vctx)
{
    WinGuiSeat *wgs = (WinGuiSeat *)vctx;
    char *newtitle;
    int i;
#ifdef MOD_PERSO
    char *base = kitty_session_title_utf8(wgs->term);
#endif

    wgs->session_closed = true;
    int title_cp = DEFAULT_CODEPAGE;
#ifdef MOD_PERSO
    /* KiTTY (upstream cyd01/KiTTY #548): a FATAL-error close gets a warning-glyph
     * titlebar marker (U+26A0, passed as UTF-8 so it survives the conversion) so a
     * backgrounded/minimised window shows the session died; a normal close keeps
     * the plain "(inactive)". The marker clears automatically when the next
     * session sets its title (reconnect / Restart).
     *
     * KiTTY (hknet/KiTTY#22): keep the session's OWN title rather than replacing
     * it with the bare appname - with ten dead windows, "KiTTY (inactive)" says
     * nothing about which connection died. The marker goes in front and the
     * state suffix at the end, so the connection name stays in the first few
     * characters and survives taskbar truncation. */
    if (wgs->error_close) {
        newtitle = dupprintf("\xe2\x9a\xa0 %s (disconnected)", base ? base : appname);
        title_cp = CP_UTF8;
    } else if (base) {
        newtitle = dupprintf("%s (inactive)", base);
        title_cp = CP_UTF8;
    } else
#endif
        newtitle = dupprintf("%s (inactive)", appname);
    win_set_icon_title(&wgs->termwin, newtitle, title_cp);
    win_set_title(&wgs->termwin, newtitle, title_cp);
    sfree(newtitle);
#ifdef MOD_PERSO
    sfree(base);
#endif

    if (wgs->ldisc) {
        ldisc_free(wgs->ldisc);
        wgs->ldisc = NULL;
    }
    if (wgs->backend) {
        backend_free(wgs->backend);
        wgs->backend = NULL;
        term_provide_backend(wgs->term, NULL);
        seat_update_specials_menu(&wgs->seat);
    }

    /*
     * Show the Restart Session menu item. Do a precautionary
     * delete first to ensure we never end up with more than one.
     */
    for (i = 0; i < lenof(wgs->popup_menus); i++) {
        DeleteMenu(wgs->popup_menus[i].menu, IDM_RESTART, MF_BYCOMMAND);
        InsertMenu(wgs->popup_menus[i].menu, IDM_DUPSESS,
                   MF_BYCOMMAND | MF_ENABLED, IDM_RESTART, "&Restart Session");
    }
}

#ifdef MOD_RECONNECT
/*
 * KiTTY "Close + Restart": tear down the live session and immediately
 * bring up a fresh backend in ONE toplevel callback, so there is no
 * ordering race between a queued close and a posted restart. Mirrors the
 * IDM_RESTART body with a forced close_session in front. start_backend
 * removes the Restart menu item and clears session_closed, so the
 * menu/state end consistent.
 */
static void close_and_restart(void *vctx)
{
    WinGuiSeat *wgs = (WinGuiSeat *)vctx;
    SetSSHConnected(0);
    close_session(wgs);          /* frees ldisc+backend, nulls wgs->backend, sets session_closed */
    if (!wgs->backend) {         /* always true after close_session */
        lp_eventlog(&wgs->logpolicy, "----- Session restarted -----");
        term_pwron(wgs->term, false);
        start_backend(wgs);      /* may MessageBox+exit(0) on connect fail (inherited) */
    }
}
#endif

/*
 * Some machinery to deal with switching the window type between ANSI
 * and Unicode. We prefer Unicode, but some PuTTY builds still try to
 * run on machines so old that they don't support that mode. So we're
 * prepared to fall back to an ANSI window if we have to. For this
 * purpose, we swap out a few Windows API functions, and wrap
 * SetWindowText so that if we're not in Unicode mode we first convert
 * the wide string we're given.
 */
static bool unicode_window;
static BOOL (WINAPI *sw_PeekMessage)(LPMSG, HWND, UINT, UINT, UINT);
static LRESULT (WINAPI *sw_DispatchMessage)(const MSG *);
static LRESULT (WINAPI *sw_DefWindowProc)(HWND, UINT, WPARAM, LPARAM);
static void sw_SetWindowText(HWND hwnd, wchar_t *text)
{
    if (unicode_window) {
        SetWindowTextW(hwnd, text);
    } else {
        char *mb = dup_wc_to_mb(DEFAULT_CODEPAGE, text, "?");
        SetWindowTextA(hwnd, mb);
        sfree(mb);
    }
}

static HINSTANCE hprev;

/*
 * Also, registering window classes has to be done in a fiddly way.
 */
#define SETUP_WNDCLASS(wndclass, classname) do {                        \
        wndclass.style = 0;                                             \
        wndclass.lpfnWndProc = WndProc;                                 \
        wndclass.cbClsExtra = 0;                                        \
        wndclass.cbWndExtra = 8; /* KiTTY Ctrl-Tab: 2 DWORD create-timestamp */ \
        wndclass.hInstance = hinst;                                     \
        wndclass.hIcon = LoadIcon(hinst, MAKEINTRESOURCE(IDI_MAINICON)); \
        wndclass.hCursor = LoadCursor(NULL, IDC_IBEAM);                 \
        wndclass.hbrBackground = NULL;                                  \
        wndclass.lpszMenuName = NULL;                                   \
        wndclass.lpszClassName = classname;                             \
    } while (0)
wchar_t *terminal_window_class_w(void)
{
    static wchar_t *classname = NULL;
    if (!classname)
        classname = dup_mb_to_wc(DEFAULT_CODEPAGE, appname);
    if (!hprev) {
        WNDCLASSW wndclassw;
        SETUP_WNDCLASS(wndclassw, classname);
        RegisterClassW(&wndclassw);
    }
    return classname;
}
char *terminal_window_class_a(void)
{
    static char *classname = NULL;
    if (!classname)
        classname = dupcat(appname, ".ansi");
    if (!hprev) {
        WNDCLASSA wndclassa;
        SETUP_WNDCLASS(wndclassa, classname);
        RegisterClassA(&wndclassa);
    }
    return classname;
}

HINSTANCE hinst;

#ifdef MOD_NETDEBUG
extern void kitty_netdbg_ts(const char *msg);   /* kitty.c: startup checkpoint logger */
#define NETDBG_TS(m) kitty_netdbg_ts(m)
#else
#define NETDBG_TS(m) ((void)0)
#endif

#ifdef MOD_PERSO
/* KiTTY: is the user looking at the Event Log right now?
 *
 * "Close window on exit" ends the whole process with PostQuitMessage(), and the
 * Event Log is a modeless dialog of that process - so closing the session takes
 * the log down with it, usually just as the user has opened it to find out what
 * the session did. An open Event Log is taken to mean "I am reading this", and
 * defers the AUTOMATIC close only; closing the window yourself still closes
 * everything immediately.
 *
 * Defined up here because the main message loop below is the first user. */
static bool kitty_eventlog_is_open(void)
{
    HWND log = event_log_window();
    return log != NULL && IsWindow(log);
}

/* >= 0 while an automatic close is waiting for the user to finish with the
 * Event Log; the value is the exit code that close would have used (0 for a
 * clean session end, 1 for a fatal error). The message loop carries it out as
 * soon as the log window is gone, so "close window on exit" is still honoured -
 * just deferred until the user has read what they opened the log for.
 *
 * Deliberately a file static rather than a field on WinGuiSeat: that struct is
 * shared with translation units compiled WITHOUT this executable's private
 * MOD_* defines, and a conditionally-compiled field in it once produced a
 * silent layout mismatch and a crash. */
static int kitty_coe_pending_exit = -1;
#endif

#ifdef MOD_LAUNCHER
/* Defined with the other command-line helpers below; WinMain's launcher
 * dispatch is its first user. */
static bool kitty_cmdline_has_token(const char *cl, const char *tok);
#endif

#ifdef MOD_PERSO
/* Defined with the window-placement code below; the message loop's deferred
 * close is its first user. */
static void kitty_on_window_closing(WinGuiSeat *wgs, HWND hwnd);
#endif

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    MSG msg;
    HRESULT hr;
    int guess_width, guess_height;

    NETDBG_TS("WinMain: enter");
    dll_hijacking_protection();
    enable_dit();

    hinst = inst;
    hprev = prev;

    sk_init();
    NETDBG_TS("after sk_init (winsock)");

    init_common_controls();

    /* Set Explicit App User Model Id so that jump lists don't cause
       PuTTY to hang on to removable media. */

    set_explicit_app_user_model_id();

    /* Ensure a Maximize setting in Explorer doesn't maximise the
     * config box. */
    defuse_showwindow();

    init_winver();

    /*
     * If we're running a version of Windows that doesn't support
     * WM_MOUSEWHEEL, find out what message number we should be
     * using instead.
     */
    if (osMajorVersion < 4 ||
        (osMajorVersion == 4 && osPlatformId != VER_PLATFORM_WIN32_NT))
        wm_mousewheel = RegisterWindowMessage("MSWHEEL_ROLLMSG");

    init_help();

    init_winfuncs();
    NETDBG_TS("after init_winfuncs");

    setup_gui_timing();

#ifdef MOD_PERSO
    /* KiTTY core activation: initialise KiTTY (crypt, config dir, kitty.ini,
     * shortcuts, save-mode/registry). hinst is set; PuTTY registry exists so
     * no first-run dialog. */
    NETDBG_TS("before InitWinMain");
    InitWinMain();
    NETDBG_TS("after InitWinMain");

    /* KiTTY: from here on every dialog this thread creates - the configuration
     * box, the auxiliary windows, and the message boxes whose window procedure
     * lives inside Windows - paints in the chosen theme. It must come after
     * InitWinMain, which is what makes the setting readable, and before the
     * first window, so none is created untreated. The TERMINAL is not a dialog
     * and is deliberately untouched: its colours are its own settings. */
    kitty_theme_hook_dialogs(kitty_theme_app_dark);
    /* The configuration box is a dialog with a window class of its own, so it
     * is not the "#32770" the hook recognises by default and has to be named.
     * (windows/dialog.c passes this string to ShinyDialogBox.) */
    kitty_theme_hook_class("PuTTYConfigBox");

    /* KiTTY hidden editor (blocnote): SHIFT+F2 / CTRL+SHIFT+F2 / the kitty.ini
     * drag-drop / -edit relaunch KiTTY as "kitty.exe -ed[b] [file]". Intercept
     * that here and run the editor's own message loop instead of opening a
     * terminal session. (The intercept that the original KiTTY put at the top
     * of WinMain was dropped during the port; -ed/-edb otherwise fall through
     * to cmdline_error "unknown option".) */
    {
        char *cl = cmdline ? cmdline : (char *)"";
        while (*cl == ' ') cl++;
        if (!strncmp(cl, "-ed ", 4) || !strcmp(cl, "-ed") ||
            !strncmp(cl, "-edb ", 5) || !strcmp(cl, "-edb"))
            return Notepad_WinMain(inst, prev, cl, show);
#ifdef MOD_LAUNCHER
        /* KiTTY session launcher: "kitty.exe -launcher" opens the launcher
         * window (a quick-launch list of saved sessions) instead of a session.
         *
         * The option is looked for ANYWHERE on the command line, not only at
         * its start. It used to be matched as a prefix, so
         * "kitty.exe -restrict-acl -launcher" - the form FEATURES.md
         * recommends for hardening the process that holds the session list -
         * fell through to the ordinary parser and died with "unknown option".
         *
         * And because the launcher is dispatched from here, BEFORE the command
         * line is parsed, -restrict-acl has to be honoured here too: otherwise
         * that command would have started an unrestricted launcher, which is
         * the exact failure this marker work is about - believing you are
         * hardened when you are not. "&R" from a parent KiTTY is handled the
         * same way, by the shared prefix helper. */
        /* KiTTY: "-cfgpanel <path>" opens the configuration box on that panel
         * instead of Session. Used by the workplace-proxy "did not answer"
         * notice to land the user on Connection/Proxy. Stripped here rather
         * than parsed later: the ordinary parser would reject it, and the panel
         * is not a session setting. */
        {
            char *p = strstr(cl, "-cfgpanel");
            if (p) {
                char panel[128];
                int i = 0;
                char *q = p + strlen("-cfgpanel");
                while (*q == ' ' || *q == '\t') q++;
                if (*q == '"') {
                    /* Quoted: panel paths can contain spaces
                     * ("Connection/SSH/Host keys"), and the screenshot
                     * pipeline addresses every panel by its exact path. */
                    q++;
                    while (*q && *q != '"' && i < (int)sizeof(panel)-1)
                        panel[i++] = *q++;
                    if (*q == '"') q++;
                } else {
                    while (*q && *q != ' ' && *q != '\t' && i < (int)sizeof(panel)-1)
                        panel[i++] = *q++;
                }
                panel[i] = '\0';
                if (panel[0])
                    kitty_cfgbox_open_on_panel(panel);
                /* blank the option out so the ordinary parser never sees it */
                while (p < q) *p++ = ' ';
            }
        }
        {
            char *lcl = handle_restrict_acl_cmdline_prefix(cl);
            if (kitty_cmdline_has_token(lcl, "-launcher")) {
                if (kitty_cmdline_has_token(lcl, "-restrict-acl") ||
                    kitty_cmdline_has_token(lcl, "-restrict_acl") ||
                    kitty_cmdline_has_token(lcl, "-restrictacl"))
                    restrict_process_acl();
                /* Let the MSI Restart Manager relaunch the tray launcher after
                 * an in-place upgrade closes it - restricted if we are. */
                RegisterApplicationRestart(restricted_acl() ?
                                           L"-restrict-acl -launcher" :
                                           L"-launcher", 0);
                return Launcher_WinMain(inst, prev, lcl, show);
            }
        }
#endif
        /* KiTTY: terminal-session Restart Manager registration used to live here,
         * keyed off the raw command line. It moved to gui_term_process_cmdline()
         * in putty.c (just before prepare_session), where conf is fully populated
         * so we can (a) also cover a saved session Opened from the config box -
         * which reaches WinMain with NO -load/@ on the command line and was
         * therefore never registered - and (b) rebuild a CLEAN "-load NAME"
         * instead of replaying "-mpwkey <handle>"/"-send-to-tray", which broke
         * the relaunch. Only "-launcher" (tray) and "-ed" (editor) are handled
         * from the raw command line, above, because they never load a session. */
    }
#endif

    WinGuiSeat *wgs = snew(WinGuiSeat);
    memset(wgs, 0, sizeof(*wgs));
    wgs_link(wgs);

    wgs->seat.vt = &win_seat_vt;
    wgs->logpolicy.vt = &win_gui_logpolicy_vt;
    wgs->termwin.vt = &windows_termwin_vt;

    wgs->caret_x = wgs->caret_y = -1;
    wgs->busy_status = BUSY_NOT;

    wgs->conf = conf_new();

    /*
     * Initialize COM.
     */
    hr = CoInitialize(NULL);
    if (hr != S_OK && hr != S_FALSE) {
        char *str = dupprintf("%s Fatal Error", appname);
        MessageBox(NULL, "Failed to initialize COM subsystem",
                   str, MB_OK | MB_ICONEXCLAMATION);
        sfree(str);
        return 1;
    }

    /*
     * Process the command line.
     * (If the command line doesn't provide enough info to start a
     * session, this will detour via the config box.)
     */
    NETDBG_TS("before gui_term_process_cmdline");
    gui_term_process_cmdline(wgs->conf, cmdline);
    NETDBG_TS("after gui_term_process_cmdline (config box / connect decided)");

    memset(&wgs->ucsdata, 0, sizeof(wgs->ucsdata));

    conf_cache_data(wgs);

    /*
     * Guess some defaults for the window size. This all gets
     * updated later, so we don't really care too much. However, we
     * do want the font width/height guesses to correspond to a
     * large font rather than a small one...
     */

    wgs->font_width = 10;
    wgs->font_height = 20;
    wgs->extra_width = 25;
    wgs->extra_height = 28;
    guess_width = wgs->extra_width + wgs->font_width * conf_get_int(
        wgs->conf, CONF_width);
    guess_height = wgs->extra_height + wgs->font_height * conf_get_int(
        wgs->conf, CONF_height);
    {
        RECT r;
        get_fullscreen_rect(wgs, &r);
        if (guess_width > r.right - r.left)
            guess_width = r.right - r.left;
        if (guess_height > r.bottom - r.top)
            guess_height = r.bottom - r.top;
    }

    {
        int winmode = WS_OVERLAPPEDWINDOW | WS_VSCROLL;
        int exwinmode = 0;
        const struct BackendVtable *vt =
            backend_vt_from_proto(be_default_protocol);
        bool resize_forbidden = false;
        if (vt && vt->flags & BACKEND_RESIZE_FORBIDDEN)
            resize_forbidden = true;
        wchar_t *uappname = dup_mb_to_wc(DEFAULT_CODEPAGE, appname);
        wgs->window_name = dup_mb_to_wc(DEFAULT_CODEPAGE, appname);
        wgs->icon_name = dup_mb_to_wc(DEFAULT_CODEPAGE, appname);
        if (!conf_get_bool(wgs->conf, CONF_scrollbar))
            winmode &= ~(WS_VSCROLL);
        if (conf_get_int(wgs->conf, CONF_resize_action) == RESIZE_DISABLED ||
            resize_forbidden)
            winmode &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        if (conf_get_bool(wgs->conf, CONF_alwaysontop))
            exwinmode |= WS_EX_TOPMOST;
        if (conf_get_bool(wgs->conf, CONF_sunken_edge))
            exwinmode |= WS_EX_CLIENTEDGE;
#ifdef MOD_PERSO
        /* KiTTY (classic parity): which of the window's own buttons exist. See
         * conf.h - dropping WS_SYSMENU removes all three caption buttons whatever
         * the other settings say, because Windows will not draw one without it.
         * "Closable" is handled separately, on the system menu (kitty_apply_window_
         * buttons), so that it also greys the X and disables Alt+F4. */
        winmode = kitty_window_button_styles(wgs->conf, winmode);
#endif

#ifdef TEST_ANSI_WINDOW
        /* For developer testing of ANSI window support, pretend
         * CreateWindowExW failed */
        wgs->term_hwnd = NULL;
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
#else
        unicode_window = true;
        sw_PeekMessage = PeekMessageW;
        sw_DispatchMessage = DispatchMessageW;
        sw_DefWindowProc = DefWindowProcW;
        wgs->term_hwnd = CreateWindowExW(
            exwinmode, terminal_window_class_w(), uappname,
            winmode, CW_USEDEFAULT, CW_USEDEFAULT,
            guess_width, guess_height, NULL, NULL, inst, NULL);
#endif

#if defined LEGACY_WINDOWS || defined TEST_ANSI_WINDOW
        if (!wgs->term_hwnd && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED) {
            /* Fall back to an ANSI window, swapping in all the ANSI
             * window message handling functions */
            unicode_window = false;
            sw_PeekMessage = PeekMessageA;
            sw_DispatchMessage = DispatchMessageA;
            sw_DefWindowProc = DefWindowProcA;
            wgs->term_hwnd = CreateWindowExA(
                exwinmode, terminal_window_class_a(), appname,
                winmode, CW_USEDEFAULT, CW_USEDEFAULT,
                guess_width, guess_height, NULL, NULL, inst, NULL);
        }
#endif

        if (!wgs->term_hwnd) {
            modalfatalbox("Unable to create terminal window: %s",
                          win_strerror(GetLastError()));
        }
        /* KiTTY: Hello-protected key files (userauth hooks). The window
         * anchors the prompt card and names this instance on it. */
        kitty_hello_terminal_init(wgs->term_hwnd);
#ifdef MOD_PERSO
        /* KiTTY #554: embed into the host window. SetParent is done HERE (after
         * creation), not via CreateWindow's hWndParent, because the latter breaks
         * keyboard focus for the embedded terminal (Remote4Support fork note).
         * We also switch to WS_CHILD: a plain SetParent on an overlapped window
         * leaves it an owned pop-up (GetParent==0, drawn at its own screen
         * coordinates, invisible inside the host) -- WS_CHILD makes it a real
         * clipped child and wires up the parent focus chain. Then fill the host's
         * client area; the host may resize us afterwards via WM_SIZE. */
        if (KITTY_EMBEDDED()) {
            if (IsWindow(kitty_hwnd_parent)) {
                LONG_PTR st = GetWindowLongPtr(wgs->term_hwnd, GWL_STYLE);
                st &= ~(WS_OVERLAPPEDWINDOW | WS_POPUP);
                st |= WS_CHILD;
                SetWindowLongPtr(wgs->term_hwnd, GWL_STYLE, st);
                /* Drop the sunken/raised frame so the terminal sits flush in the
                 * host pane (no sub-window border). */
                LONG_PTR ex = GetWindowLongPtr(wgs->term_hwnd, GWL_EXSTYLE);
                ex &= ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE |
                        WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
                SetWindowLongPtr(wgs->term_hwnd, GWL_EXSTYLE, ex);
                SetParent(wgs->term_hwnd, kitty_hwnd_parent);
                kitty_hwnd_parent_main =
                    GetAncestor(kitty_hwnd_parent, GA_ROOTOWNER);
                SetWindowPos(wgs->term_hwnd, NULL, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_FRAMECHANGED);
                RECT prc;
                if (GetClientRect(kitty_hwnd_parent, &prc) &&
                    prc.right > 0 && prc.bottom > 0)
                    MoveWindow(wgs->term_hwnd, 0, 0, prc.right - prc.left,
                               prc.bottom - prc.top, TRUE);
                /* fill-tracking timer is armed after ShowWindow (below). */
            } else {
                kitty_hwnd_parent = NULL;   /* stale handle: behave normally */
            }
        }
#endif
        memset(&wgs->dpi_info, 0, sizeof(struct _dpi_info));
        init_dpi_info(wgs);
        sfree(uappname);
    }

    SetWindowLongPtr(wgs->term_hwnd, GWLP_USERDATA, (LONG_PTR)wgs);
#ifdef MOD_PERSO
    /* KiTTY Ctrl-Tab: stamp this window's creation time into the 8 class-extra
     * bytes so CtrlTabWindowProc can order windows for next/prev switching. */
    if (conf_get_int(wgs->conf, CONF_ctrl_tab_switch) && GetCtrlTabFlag()) {
        int wx = GetClassLong(wgs->term_hwnd, GCL_CBWNDEXTRA);
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        SetWindowLong(wgs->term_hwnd, wx - 8, ft.dwHighDateTime);
        SetWindowLong(wgs->term_hwnd, wx - 4, ft.dwLowDateTime);
    }
    /* KiTTY: accept files dropped on the terminal window (pscp upload). The 0.84
     * port had OnDropFiles() defined but never registered the window for drops,
     * so the cursor showed "forbidden". Re-enable it + the WM_DROPFILES handler. */
    DragAcceptFiles(wgs->term_hwnd, TRUE);
#endif

    /*
     * Initialise the fonts, simultaneously correcting the guesses
     * for font_{width,height}.
     */
    NETDBG_TS("win: before init_fonts");
    init_fonts(wgs, 0, 0);
    NETDBG_TS("win: after init_fonts");

    /*
     * Prepare a logical palette.
     */
    init_palette(wgs);

    /*
     * Initialise the terminal. (We have to do this _after_
     * creating the window, since the terminal is the first thing
     * which will call schedule_timer(), which will in turn call
     * timer_change_notify() which will expect hwnd to exist.)
     */
    wgs->term = term_init(wgs->conf, &wgs->ucsdata, &wgs->termwin);
#ifdef MOD_PERSO
    kitty_set_active_seat(wgs);
    /* -loginscript: deferred here because ReadInitScript writes the GLOBAL conf,
     * which kitty_set_active_seat has only just made valid (it was NULL during
     * the putty.c command-line parse). Consume once. */
    if (kitty_cli_loginscript) {
        ReadInitScript(kitty_cli_loginscript);
        sfree(kitty_cli_loginscript);
        kitty_cli_loginscript = NULL;
    } else {
        /*
         * Arm the session's stored login script, exactly as classic KiTTY did
         * (ReadInitScript(NULL) at this point in its own window.c).
         *
         * This wiring was LOST in the forward-port to 0.84: the driver that scans
         * incoming data was re-added, but nothing ever loaded the stored script
         * into ScriptFileContent, so for the life of 0.84 a saved script has been
         * written, shown and protected - and then silently never run unless
         * somebody typed /loadinitscript. Classic armed it here, on reconnect, and
         * when the file was picked in the config box.
         *
         * Restored without a gate, deliberately. It is our regression, so the fix
         * is to do what the user already configured and classic already did;
         * making them re-approve their own working setup would charge them for
         * our mistake. ReadInitScript self-skips when there is nothing stored.
         */
        ReadInitScript(NULL);
    }
    kitty_apply_transparency(wgs);
    /* kitty_apply_window_pos is deferred until AFTER the startup sizing/clamp
     * block below, so the single-monitor working-area clamp can't undo a
     * cross-monitor restore (see kitty_apply_window_pos). */
    /* KiTTY feature: per-session icon (CONF_icone / CONF_iconefile) */
    kitty_apply_icon(wgs->term_hwnd, wgs->conf);
#ifdef MOD_BACKGROUNDIMAGE
    /* KiTTY feature: background image - load the configured image into the
     * module-global backgrounddc. The paint-time blit is wired in
     * do_text_internal (per-cell compositing for default-bg cells), adapted
     * to 0.84's direct-to-window paint path. */
    kitty_apply_background(wgs->term_hwnd, wgs->conf);
    /* KiTTY background-image slideshow: arm a repeating timer rotating the image
     * through the folder. Period = CONF_bg_slideshow, else ImageSlideDelay (ini
     * "slidedelay"). Inert unless a bg image is active with a positive delay. */
    if (GetBackgroundImageFlag() && !GetPuttyFlag()
        && conf_get_int(wgs->conf, CONF_bg_type) != 0) {
        int slide = conf_get_int(wgs->conf, CONF_bg_slideshow);
        int period = slide > 0 ? slide : (ImageSlideDelay > 0 ? ImageSlideDelay : 0);
        if (period > 0)
            SetTimer(wgs->term_hwnd, TIMER_SLIDEBG_WIN, period * 1000, NULL);
    }
#endif
    /* KiTTY feature: auto-minimise-to-tray when SendToTray is set.
     * Skipped when embedded (#554): a child window in the tray is nonsense. */
    if (conf_get_int(wgs->conf, CONF_sendtotray) && !KITTY_EMBEDDED())
        SetAutoSendToTray(1);
    /* KiTTY feature: URL hyperlinks - init urlhack + compile regex */
    kitty_url_init();
    kitty_url_config(wgs->conf);
    /* KiTTY feature: auto-command sent automatically after login.
     * First fire is delayed to let the connection establish ([KiTTY]
     * initdelay, seconds); subsequent lines re-arm at autocommand_delay
     * (see WM_TIMER below). */
    {
        const char *ac = conf_get_str(wgs->conf, CONF_autocommand);
        if (ac && ac[0])
            SetTimer(wgs->term_hwnd, TIMER_AUTOCOMMAND,
                     init_delay > 0 ? init_delay : 1500, NULL);
    }
    /* KiTTY feature: anti-idle. The timer PERIOD is the configured interval
     * ([KiTTY] antiidledelay, in seconds), so kitty_antiidle_tick() just sends.
     * Classic KiTTY instead ran a fixed 30-second timer and counted ticks to
     * AntiIdleCountMax; that counter is gone (7e2e290f) and nothing here counts
     * any more.
     *
     * Milliseconds computed in 64 bits and bounded before the cast: seconds is
     * an int from kitty.ini, and a 32-bit multiply by 1000 wraps at 4,294,968
     * seconds - which arms a 704 ms timer instead of a 49-day one. kitty.c caps
     * the value at a day, and this is the second half of that same belt: a
     * caller reaching AntiIdleSeconds another way cannot produce a flood here.
     * (USER_TIMER_MAXIMUM is Windows' own ceiling, ~24.8 days.) */
    {
        const char *ai = conf_get_str(wgs->conf, CONF_antiidle);
        if ((ai && ai[0]) || AntiIdleStr[0]) {
            long long secs = (AntiIdleSeconds > 0 ? AntiIdleSeconds : 180);
            long long ms = secs * 1000LL;
            if (ms > (long long)USER_TIMER_MAXIMUM) ms = USER_TIMER_MAXIMUM;
            SetTimer(wgs->term_hwnd, TIMER_ANTIIDLE, (UINT)ms, NULL);
        }
    }
    /* KiTTY feature: rutty scripting. The on-connect script is armed in
     * start_backend, not here: this runs once per PROCESS, and the script has to
     * run once per CONNECTION. See kitty_arm_script_on_connect(). */
#endif
    setup_clipboards(wgs->term, wgs->conf);
    wgs->logctx = log_init(&wgs->logpolicy, wgs->conf);
    term_provide_logctx(wgs->term, wgs->logctx);
#ifdef MOD_PERSO
    {
        /* KiTTY: arm log rotation. Repeating timer - SetTimer keeps firing
         * until it is killed, which is what "every N seconds" wants. */
        int rot = conf_get_int(wgs->conf, CONF_logtimerotation);
        if (rot > 0 && wgs->term_hwnd)
            SetTimer(wgs->term_hwnd, TIMER_LOGROTATION,
                     (UINT)rot * 1000, NULL);
    }
#endif
    term_size(wgs->term, conf_get_int(wgs->conf, CONF_height),
              conf_get_int(wgs->conf, CONF_width),
              conf_get_int(wgs->conf, CONF_savelines));

    /*
     * Correct the guesses for extra_{width,height}.
     */
    {
        RECT cr, wr;
        GetWindowRect(wgs->term_hwnd, &wr);
        GetClientRect(wgs->term_hwnd, &cr);
        wgs->offset_width = wgs->offset_height =
            conf_get_int(wgs->conf, CONF_window_border);
        wgs->extra_width =
            wr.right - wr.left - cr.right + cr.left + wgs->offset_width*2;
        wgs->extra_height =
            wr.bottom - wr.top - cr.bottom + cr.top +wgs->offset_height*2;
    }

    /*
     * Compute what size we _really_ want the window to be.
     */
    guess_width = wgs->extra_width + wgs->font_width * wgs->term->cols;
    guess_height = wgs->extra_height + wgs->font_height * wgs->term->rows;

    /*
     * Resize the window to that size, also repositioning it if it's extended
     * off the edge of a monitor.
     */
    {
        /* Find the previous coordinates of the window */
        RECT winr;
        GetWindowRect(wgs->term_hwnd, &winr);

        int x = winr.left;
        int y = winr.top;

        /* Adjust them if necessary */
        RECT war;
        if (get_workingarea_rect(wgs, &war)) {
            /*
             * Try to ensure the window is entirely within the monitor's
             * working area, by adjusting its position if not.
             *
             * We first move it left, if it overlaps off the right side. Then
             * we move it right if it overlaps off the left side. This means
             * that if it's wider than the working area (so that some overlap
             * is unavoidable), we prefer to get its left edge in bounds than
             * its right edge. Similarly, we do the y checks in the same
             * order, privileging the top edge over the bottom.
             */
            if (x + guess_width > war.right)
                x = war.right - guess_width;
            if (x < war.left)
                x = war.left;
            if (y + guess_height > war.bottom)
                y = war.bottom - guess_height;
            if (y < war.top)
                y = war.top;
        }

        /* And set the window to the final size and position we've chosen.
         * Skipped when embedded (#554): the window is sized to the host's client
         * area and must not be repositioned to monitor coordinates. */
#ifdef MOD_PERSO
        if (!KITTY_EMBEDDED())
#endif
        SetWindowPos(wgs->term_hwnd, NULL, x, y, guess_width, guess_height,
                    SWP_NOREDRAW | SWP_NOZORDER);
    }

#ifdef MOD_PERSO
    /* KiTTY: apply remembered/pinned window position AFTER the sizing+clamp
     * block above, so the single-monitor working-area clamp can't undo a
     * restore onto another (possibly different-DPI) monitor. Done before
     * ShowWindow, so there's no visible jump. */
    kitty_apply_window_pos(wgs);
#endif

    /*
     * Set up a caret bitmap, with no content.
     */
    {
        char *bits;
        int size = (wgs->font_width + 15) / 16 * 2 * wgs->font_height;
        bits = snewn(size, char);
        memset(bits, 0, size);
        wgs->caretbm = CreateBitmap(wgs->font_width, wgs->font_height,
                                    1, 1, bits);
        sfree(bits);
    }
    CreateCaret(wgs->term_hwnd, wgs->caretbm,
                wgs->font_width, wgs->font_height);

    /*
     * Initialise the scroll bar.
     */
    {
        SCROLLINFO si;

        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
        si.nMin = 0;
        si.nMax = wgs->term->rows - 1;
        si.nPage = wgs->term->rows;
        si.nPos = 0;
        SetScrollInfo(wgs->term_hwnd, SB_VERT, &si, false);
    }

    /*
     * Prepare the mouse handler.
     */
    wgs->lastact = MA_NOTHING;
    wgs->lastbtn = MBT_NOTHING;
    wgs->dbltime = GetDoubleClickTime();

    /*
     * Set up the session-control options on the system menu.
     */
    {
        HMENU m;
        int j;
        char *str;
#ifdef MOD_PERSO
        HMENU winmenu, toolmenu;
#endif

        wgs->popup_menus[SYSMENU].menu = GetSystemMenu(wgs->term_hwnd, false);
        wgs->popup_menus[CTXMENU].menu = CreatePopupMenu();

        /* KiTTY: keep an explicit Paste command in both the system menu and
         * right-click context menu. In Windows mouse mode, right-click opens the
         * context menu instead of pasting, so the menu item is the discoverable
         * paste path (matching classic KiTTY/PuTTY behaviour). */

        wgs->savedsess_menu = CreateMenu();
        get_sesslist(&sesslist, true);
        update_savedsess_menu(wgs);

        for (j = 0; j < lenof(wgs->popup_menus); j++) {
            m = wgs->popup_menus[j].menu;

            AppendMenu(m, MF_ENABLED, IDM_SHOWLOG, "&Event Log");
            AppendMenu(m, MF_SEPARATOR, 0, 0);
            /* KiTTY: "New Session..." (config box with DEFAULT settings) is
             * hidden - "Inherit New Session" covers the case with this
             * window's settings pre-loaded, and a fresh default box is one
             * kitty.exe start away. Restore the line if users miss it (the
             * IDM_NEWSESS handler is still in place). */
            /* AppendMenu(m, MF_ENABLED, IDM_NEWSESS, "Ne&w Session..."); */
            AppendMenu(m, MF_ENABLED, IDM_DUPSESS, "&Duplicate Session");
#ifdef MOD_PERSO
            AppendMenu(m, MF_ENABLED, IDM_NEWDUPSESS, "&Inherit New Session...");
#endif
#ifdef MOD_RECONNECT
            AppendMenu(m, MF_ENABLED, IDM_RESTARTSESSION, "Close+&Restart");
#endif
            AppendMenu(m, MF_POPUP | MF_ENABLED, (UINT_PTR)wgs->savedsess_menu,
                       "Sa&ved Sessions");
            AppendMenu(m, MF_ENABLED, IDM_RECONF, "Chan&ge Settings...");
            AppendMenu(m, MF_SEPARATOR, 0, 0);
            AppendMenu(m, MF_ENABLED, IDM_PASTE, "&Paste");
            AppendMenu(m, MF_ENABLED, IDM_COPYALL, "C&opy All to Clipboard");
            AppendMenu(m, MF_ENABLED, IDM_CLRSB, "C&lear Scrollback");
            AppendMenu(m, MF_ENABLED, IDM_RESET, "Rese&t Terminal");
            AppendMenu(m, MF_SEPARATOR, 0, 0);
            /* Full Screen: when Alt+Enter toggling is enabled for this
             * session, advertise the key in the label so the user knows
             * how to leave full screen (no title bar / menu is visible). */
            AppendMenu(m, (conf_get_int(wgs->conf, CONF_resize_action)
                           == RESIZE_DISABLED) ? MF_GRAYED : MF_ENABLED,
                       IDM_FULLSCREEN,
                       conf_get_bool(wgs->conf, CONF_fullscreenonaltenter)
                       ? "&Full Screen (Alt+Enter)" : "&Full Screen");
#ifdef MOD_PERSO
            /* ---- "Window" submenu: appearance & window state ---- */
            winmenu = CreatePopupMenu();
            /* Two ways to not get these: the feature switched off globally
             * (kitty.ini [KiTTY] transparency=no), or this session carrying -1,
             * which means "never dim this window". kitty_sync_transparency_menu
             * keeps that in step if the session value changes later. */
            if (GetTransparencyFlag()
                && conf_get_int(wgs->conf, CONF_transparencynumber) != -1) {
                AppendMenu(winmenu, MF_ENABLED, IDM_TRANSPARUP,   "Transparency &+");
                AppendMenu(winmenu, MF_ENABLED, IDM_TRANSPARDOWN, "Transparency &-");
                AppendMenu(winmenu, MF_SEPARATOR, 0, 0);
            }
            AppendMenu(winmenu, MF_ENABLED, IDM_FONTUP,       "Font &Up");
            AppendMenu(winmenu, MF_ENABLED, IDM_FONTDOWN,     "Font &Down");
            AppendMenu(winmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(winmenu, MF_ENABLED, IDM_FONTNEGATIVE, "Invert co&lours");
            AppendMenu(winmenu, MF_ENABLED, IDM_FONTBLACKANDWHITE, "&Black on white");
            AppendMenu(winmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(winmenu, MF_ENABLED, IDM_VISIBLE,      "Always On &Top");
            AppendMenu(winmenu, MF_ENABLED, IDM_WINROL,       "Roll-u&p");
            AppendMenu(winmenu, MF_ENABLED, IDM_TOTRAY,       "Send to tra&y");
            AppendMenu(winmenu, MF_ENABLED, IDM_PROTECT,      "Prote&ct");
            AppendMenu(m, MF_POPUP | MF_ENABLED, (UINT_PTR)winmenu, "&Window");

            /* ---- "Tools" submenu: transfer & integration ---- */
            toolmenu = CreatePopupMenu();
            AppendMenu(toolmenu, MF_ENABLED, IDM_SHOWPORTFWD, "Port forwar&dings");
            AppendMenu(toolmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(toolmenu, MF_ENABLED, IDM_WINSCP, "Start Win&SCP");
            AppendMenu(toolmenu, MF_ENABLED, IDM_PSCP, "Send file (&pscp)");
            AppendMenu(toolmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(toolmenu, MF_ENABLED, IDM_MNOTEPAD, "Open &mNotepad");
            AppendMenu(toolmenu, MF_ENABLED, IDM_MNOTEPAD_CLIP, "Open mNotepad with clip&board");
            AppendMenu(toolmenu, MF_SEPARATOR, 0, 0);
            {
                int sa = kitty_script_active();
                AppendMenu(toolmenu, sa ? MF_GRAYED : MF_ENABLED,
                           IDM_SCRIPTSEND, "Send &recorded script");
                AppendMenu(toolmenu, sa ? MF_ENABLED : MF_GRAYED,
                           IDM_SCRIPTHALT, "S&top script");
                AppendMenu(toolmenu, MF_ENABLED,
                           IDM_SCRIPTFILE2, "Send scr&ipt file");
            }
#ifdef MOD_ZMODEM
            /*
             * KiTTY does not implement ZModem: it drives the rz/sz helpers the
             * user supplies (Connection > ZModem). With no helper configured
             * the entries are NOT SHOWN - an entry whose only possible outcome
             * is "Unable to find ZModem receive program" reads as a broken
             * feature rather than an unconfigured one, and a greyed row that
             * explains itself in its own label is still a row nobody asked for.
             * Receive needs rz and Upload needs sz, set independently, so they
             * appear independently.
             *
             * The CONFIG PANEL stays visible regardless: it is where the paths
             * are set, so gating it on them would lock the feature away.
             */
            /* Nothing is added here: the ZModem entries are inserted (and
             * removed) in WM_INITMENUPOPUP, because whether they belong in the
             * menu at all depends on paths Change Settings can alter while the
             * window is open. */
#endif
            AppendMenu(toolmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(toolmenu, MF_ENABLED, IDM_PRINT,        "Print clip&board");
            /* Both log items are re-labelled and enabled/greyed in
             * WM_INITMENUPOPUP: whether there is a log at all, and whether
             * clearing or rotating is what will happen, are only known when
             * the menu is opened. */
            AppendMenu(toolmenu, MF_ENABLED, IDM_OPENLOGFILE,  "&Open log file");
            AppendMenu(toolmenu, MF_ENABLED, IDM_CLEARLOGFILE, "Clear log fil&e");
            /* "Export current settings" exports the RUNNING session, a per-
             * connection action, so it belongs on the terminal menu. Whole-store
             * "Export all" / "Import" live in the config box (Session panel),
             * reachable at launch without a connection. */
            AppendMenu(toolmenu, MF_ENABLED, IDM_EXPORTSETTINGS, "Export &current settings");
            AppendMenu(toolmenu, MF_SEPARATOR, 0, 0);
            AppendMenu(toolmenu, MF_ENABLED, IDM_SHORTCUTSTOGGLE, "Shortcut&s");
            AppendMenu(toolmenu, MF_ENABLED | (GetHyperlinkFlag() ? MF_CHECKED : 0),
                       IDM_HYPERLINKTOGGLE, "Hyper&links");
            /* Armed windows are visibly armed: the check mark is the only way to
             * tell, before typing arrives, which of your windows will accept it. */
            /* Named like the ZModem entries above: the menu says WHERE the
             * setting lives, because a toggle with no home is the kind of thing
             * people flip once and never find again. The label carries the
             * panel; the Event Log carries the reason when a broadcast is
             * refused. */
            AppendMenu(toolmenu, MF_ENABLED |
                       (conf_get_bool(wgs->conf, CONF_kitty_accept_broadcast) ? MF_CHECKED : 0),
                       IDM_BROADCASTTOGGLE,
                       "Accept &broadcast (Session > Scripting)");
            AppendMenu(m, MF_POPUP | MF_ENABLED, (UINT_PTR)toolmenu, "&Tools");

            /* KiTTY "Shortcuts for predefined commands": read the registry
             * Commands keys into SpecialMenu[] and add a "&User Command"
             * submenu. Keyboard shortcuts (Ctrl+Shift+A..Z) dispatch via
             * WM_COMMAND IDM_USERCMD+n -> ManageSpecialCommand (handled in the
             * WM_COMMAND default case). Added to both the system menu and the
             * right-click context menu; context-menu clicks fire WM_COMMAND. */
            InitSpecialMenu(m, conf_get_str(wgs->conf, CONF_folder),
                            conf_get_str(wgs->conf, CONF_sessionname));

            AppendMenu(m, MF_SEPARATOR, 0, 0);
            AppendMenu(m, MF_ENABLED, IDM_QUIT, "E&xit");
#endif
            AppendMenu(m, MF_SEPARATOR, 0, 0);
            if (has_help())
                AppendMenu(m, MF_ENABLED, IDM_HELP, "&Help");
            str = dupprintf("&About %s", appname);
            AppendMenu(m, MF_ENABLED, IDM_ABOUT, str);
            sfree(str);
#ifdef MOD_PERSO
            AppendMenu(m, MF_ENABLED, IDM_CHECKUPDATE, "Check for &updates...");
#endif
        }
    }

#ifdef MOD_PERSO
    /* KiTTY: grey Close if this session may not be closed. Must run AFTER the
     * system menu above has been built, because GetSystemMenu(FALSE) hands back
     * the menu that setup populated. */
    kitty_apply_close_button(wgs, wgs->term_hwnd);
#endif

    if (restricted_acl()) {
        lp_eventlog(&wgs->logpolicy, "Running with restricted process ACL");
    }

    winselgui_set_hwnd(wgs->term_hwnd);
    NETDBG_TS("win: before start_backend");
    start_backend(wgs);
    NETDBG_TS("win: after start_backend");

    /*
     * Set up the initial input locale.
     */
    set_input_locale(wgs, GetKeyboardLayout(0));

    /*
     * Finally show the window!
     */
#ifdef MOD_PERSO
    /* KiTTY feature: maximize on start (no-global; reads this seat's conf).
     * Skipped when embedded (#554): the host window owns sizing. */
    if (conf_get_int(wgs->conf, CONF_maximize) && !KITTY_EMBEDDED())
        show = SW_SHOWMAXIMIZED;
#endif
    ShowWindow(wgs->term_hwnd, show);
    SetForegroundWindow(wgs->term_hwnd);

    term_set_focus(wgs->term, GetForegroundWindow() == wgs->term_hwnd);
#ifdef MOD_PERSO
    /* #554: a WS_CHILD doesn't take focus from SetForegroundWindow; focus it
     * directly so keystrokes go to the embedded terminal. */
    if (KITTY_EMBEDDED()) {
        SetFocus(wgs->term_hwnd);
        term_set_focus(wgs->term, true);
        /* The host (mRemoteNG) does not reliably resize a self-parented child,
         * so poll its client rect and keep filling it. Armed here (post-show)
         * rather than at creation, where SetTimer didn't take. */
        SetTimer(wgs->term_hwnd, TIMER_EMBEDFILL, 200, NULL);
    }
#endif
    UpdateWindow(wgs->term_hwnd);
#ifdef MOD_PERSO
    /* KiTTY feature: fullscreen on start (no-global; reads this seat's conf).
     * Skipped when embedded (#554). */
    if (conf_get_int(wgs->conf, CONF_fullscreen) && !KITTY_EMBEDDED())
        PostMessage(wgs->term_hwnd, WM_COMMAND, IDM_FULLSCREEN, 0);
    /* KiTTY feature: send to tray on start - the session setting "Send to tray"
     * (CONF_sendtotray, armed in SetAutoSendToTray above) or "-send-to-tray" on
     * the command line. Deferred rather than done right here: hiding the window
     * the instant it appears would also hide the host-key prompt, the password
     * prompt and any connection error, so we poll and only drop to the tray once
     * the session is really up (see TIMER_SENDTOTRAY below). A session that
     * never connects therefore stays visible, which is what you want.
     * Skipped when embedded (#554): a child window in the tray is nonsense. */
    if (GetAutoSendToTray() && !KITTY_EMBEDDED())
        SetTimer(wgs->term_hwnd, TIMER_SENDTOTRAY,
                 init_delay > 0 ? init_delay : 2000, NULL);
#endif

    gui_terminal_ready(wgs->term_hwnd, &wgs->seat, wgs->backend);

    while (1) {
        int n;
        DWORD timeout;

        if (toplevel_callback_pending() ||
            PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE)) {
            /*
             * If we have anything we'd like to do immediately, set
             * the timeout for MsgWaitForMultipleObjects to zero so
             * that we'll only do a quick check of our handles and
             * then get on with whatever that was.
             *
             * One such option is a pending toplevel callback. The
             * other is a non-empty Windows message queue, which you'd
             * think we could leave to MsgWaitForMultipleObjects to
             * check for us along with all the handles, but in fact we
             * can't because once PeekMessage in one iteration of this
             * loop has removed a message from the queue, the whole
             * queue is considered uninteresting by the next
             * invocation of MWFMO. So we check ourselves whether the
             * message queue is non-empty, and if so, set this timeout
             * to zero to ensure MWFMO doesn't block.
             */
            timeout = 0;
        } else {
            timeout = INFINITE;
            /* The messages seem unreliable; especially if we're being tricky */
            term_set_focus(wgs->term, GetForegroundWindow() == wgs->term_hwnd);
        }

        HandleWaitList *hwl = get_handle_wait_list();

        n = MsgWaitForMultipleObjects(hwl->nhandles, hwl->handles, false,
                                      timeout, QS_ALLINPUT);

        if ((unsigned)(n - WAIT_OBJECT_0) < (unsigned)hwl->nhandles)
            handle_wait_activate(hwl, n - WAIT_OBJECT_0);
        handle_wait_list_free(hwl);

        while (sw_PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto finished;         /* two-level break */

            HWND logbox = event_log_window();
            if (!(IsWindow(logbox) && IsDialogMessage(logbox, &msg)) &&
                !ShinyAuxDialogMessage(&msg) /* KiTTY modeless About + /help */)
                sw_DispatchMessage(&msg);

            /*
             * WM_NETEVENT messages seem to jump ahead of others in
             * the message queue. I'm not sure why; the docs for
             * PeekMessage mention that messages are prioritised in
             * some way, but I'm unclear on which priorities go where.
             *
             * Anyway, in practice I observe that WM_NETEVENT seems to
             * jump to the head of the queue, which means that if we
             * were to only process one message every time round this
             * loop, we'd get nothing but NETEVENTs if the server
             * flooded us with data, and stop responding to any other
             * kind of window message. So instead, we keep on round
             * this loop until we've consumed at least one message
             * that _isn't_ a NETEVENT, or run out of messages
             * completely (whichever comes first). And we don't go to
             * run_toplevel_callbacks (which is where the netevents
             * are actually processed, causing fresh NETEVENT messages
             * to appear) until we've done this.
             */
            if (msg.message != WM_NETEVENT)
                break;
        }

        run_toplevel_callbacks();
#ifdef MOD_ZMODEM
        /* KiTTY ZModem: pump the helper's stdout to the backend each loop. */
        if (kitty_zmodem_active())
            kitty_zmodem_process();
#endif
#ifdef MOD_PERSO
        /* KiTTY: an automatic close was held back while the user read the Event
         * Log (see kitty_coe_pending_exit). They have now closed it, so do what
         * "close window on exit" asked for in the first place. */
        if (kitty_coe_pending_exit >= 0 && !kitty_eventlog_is_open()) {
            int code = kitty_coe_pending_exit;
            kitty_coe_pending_exit = -1;
            /* PostQuitMessage gives no WM_DESTROY, so save the position here,
             * exactly as the immediate close paths do. */
            kitty_on_window_closing(wgs, wgs->term_hwnd);
            PostQuitMessage(code);
        }
#endif
    }

  finished:
#if defined(MOD_PERSO) && defined(MOD_RECONNECT)
    /* KiTTY [ConfigBox] noexit=yes: when a window that actually ran a
     * connected session closes, spawn a fresh instance (which starts at the
     * config box) so the user lands back in the session picker. Gating on
     * is_backend_first_connected fixes classic KiTTY's bug of respawning on
     * config-box exit too; skipped during system shutdown/logoff. */
    if (GetConfigBoxNoExitFlag() && is_backend_first_connected &&
        !GetSystemMetrics(SM_SHUTTINGDOWN))
        kitty_respawn_config_box();
#endif
    cleanup_exit(msg.wParam);          /* this doesn't return... */
    return msg.wParam;                 /* ... but optimiser doesn't know */
}

static void wgs_cleanup(WinGuiSeat *wgs)
{
    deinit_fonts(wgs);
    sfree(wgs->logpal);
    if (wgs->pal)
        DeleteObject(wgs->pal);
    wgs_unlink(wgs);
    sfree(wgs);
}

#ifdef MOD_LAUNCHER
/* KiTTY: is `tok` present on `cl` as a whole whitespace-separated word? Used by
 * the launcher dispatch in WinMain, which runs before the real command-line
 * parser and so has to recognise its own options by hand. Whole-word matching
 * keeps "-launcher" from being found inside a path or a session name. */
static bool kitty_cmdline_has_token(const char *cl, const char *tok)
{
    size_t n = strlen(tok);
    while (*cl) {
        const char *start;
        while (*cl == ' ' || *cl == '\t')
            cl++;
        if (!*cl)
            break;
        start = cl;
        while (*cl && *cl != ' ' && *cl != '\t')
            cl++;
        if ((size_t)(cl - start) == n && !strncmp(start, tok, n))
            return true;
    }
    return false;
}
#endif

char *handle_restrict_acl_cmdline_prefix(char *p)
{
    /*
     * Process the &R prefix on a command line, which is equivalent to
     * -restrict-acl but lexically easier to prepend when another
     * instance of ourself automatically constructs a command line.
     *
     * If successful, restricts the process ACL and advances the input
     * pointer past the prefix. Returns the updated pointer (whether
     * it moved or not).
     */
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == '&' && p[1] == 'R' &&
        (!p[2] || p[2] == '@' || p[2] == '&')) {
        /* &R restrict-acl prefix */
        restrict_process_acl();
        p += 2;
    }
    return p;
}

bool handle_special_sessionname_cmdline(char *p, Conf *conf)
{
    /*
     * Process the special form of command line with an initial @
     * followed by the name of a saved session with _no quoting or
     * escaping_. This is a very convenient means of automated
     * saved-session launching, via IDM_SAVEDSESS or Windows 7 jump
     * lists.
     *
     * If successful, the whole command line has been interpreted in
     * this way, so there's nothing left to parse into other arguments.
     */
    if (*p != '@')
        return false;

    ptrlen sessionname = ptrlen_from_asciz(p + 1);
    while (sessionname.len > 0 &&
           isspace(((unsigned char *)sessionname.ptr)[sessionname.len-1]))
        sessionname.len--;

    char *dup = mkstr(sessionname);
    bool loaded = do_defaults(dup, conf);
    sfree(dup);

    return loaded;
}

bool handle_special_filemapping_cmdline(char *p, Conf *conf)
{
    /*
     * Process the special form of command line with an initial &
     * followed by the hex value of a HANDLE for a file mapping object
     * and the size of the data contained in it, which we must
     * interpret as a serialised Conf.
     *
     * If successful, the whole command line has been interpreted in
     * this way, so there's nothing left to parse into other arguments.
     */

    if (*p != '&')
        return false;

    HANDLE filemap;
    unsigned cpsize;
    if (sscanf(p + 1, "%p:%u", &filemap, &cpsize) != 2)
        return false;

    void *cp = MapViewOfFile(filemap, FILE_MAP_READ, 0, 0, cpsize);
    if (!cp)
        return false;

    BinarySource src[1];
    BinarySource_BARE_INIT(src, cp, cpsize);
    if (!conf_deserialise(conf, src))
        modalfatalbox("Serialised configuration data was invalid");
    UnmapViewOfFile(cp);
    CloseHandle(filemap);
    return true;
}

static void setup_clipboards(Terminal *term, Conf *conf)
{
    assert(term->mouse_select_clipboards[0] == CLIP_LOCAL);

    term->n_mouse_select_clipboards = 1;

    if (conf_get_bool(conf, CONF_mouseautocopy)) {
        term->mouse_select_clipboards[
            term->n_mouse_select_clipboards++] = CLIP_SYSTEM;
    }

    switch (conf_get_int(conf, CONF_mousepaste)) {
      case CLIPUI_IMPLICIT:
        term->mouse_paste_clipboard = CLIP_LOCAL;
        break;
      case CLIPUI_EXPLICIT:
        term->mouse_paste_clipboard = CLIP_SYSTEM;
        break;
      default:
        term->mouse_paste_clipboard = CLIP_NULL;
        break;
    }
}

/*
 * Clean up and exit.
 */
void cleanup_exit(int code)
{
    /*
     * Clean up.
     */
    while (wgslisthead.next != &wgslisthead) {
        WinGuiSeat *wgs = container_of(
            wgslisthead.next, WinGuiSeat, wgslistnode);
        wgs_cleanup(wgs);
    }
    sk_cleanup();

#ifdef MOD_PERSO
    winfb_log_close();                 /* KiTTY font fallback */
#endif
    random_save_seed();
    shutdown_help();

    /* Clean up COM. */
    CoUninitialize();

    exit(code);
}

/*
 * Refresh the saved-session submenu from `sesslist'.
 */
static void update_savedsess_menu(WinGuiSeat *wgs)
{
    int i;
    int limit = ((sesslist.nsessions <= MENU_SAVED_MAX+1) ? sesslist.nsessions
                                                          : MENU_SAVED_MAX+1);
    /* DeleteMenu, not RemoveMenu: it destroys a submenu along with its item, so
     * the folder popups built below do not leak on every rebuild. */
    while (DeleteMenu(wgs->savedsess_menu, 0, MF_BYPOSITION)) ;
#ifdef MOD_PERSO
    /*
     * KiTTY: one submenu per folder, unfiled sessions after them.
     *
     * A folder is an attribute of a session, so this is a view, exactly as the
     * config box's folder rows are - the store is not touched. It is NOT gated
     * on [ConfigBox] foldernavigation: that setting chooses how the config box
     * DIALOG presents the list, while the launcher's tray menu has grouped by
     * folder since long before it existed. Gating here would have produced a
     * fourth answer to "what sessions do I have" rather than removing one.
     *
     * The command id still derives from the session's index in sesslist, so
     * where an item sits in the menu does not affect what it opens.
     */
    {
        extern char *kitty_read_session_folder(const char *sessionname);
#define KITTY_MENU_FOLDERS_MAX 64
        HMENU fmenu[KITTY_MENU_FOLDERS_MAX];
        char *fname[KITTY_MENU_FOLDERS_MAX];
        int nfolders = 0, j;
        int *unfiled = (limit > 1) ? snewn(limit, int) : NULL;
        int nunfiled = 0;

        /* skip sesslist.sessions[0] == Default Settings */
        for (i = 1; i < limit; i++) {
            char *fld = kitty_read_session_folder(sesslist.sessions[i]);
            bool filed = (fld && *fld && strcmp(fld, "Default") != 0);
            int slot = -1;
            if (filed) {
                for (j = 0; j < nfolders; j++)
                    if (!strcmp(fname[j], fld)) { slot = j; break; }
                if (slot < 0 && nfolders < KITTY_MENU_FOLDERS_MAX) {
                    slot = nfolders++;
                    fname[slot] = dupstr(fld);
                    fmenu[slot] = CreateMenu();
                }
            }
            if (slot >= 0) {
                AppendMenu(fmenu[slot], MF_ENABLED,
                           IDM_SAVED_MIN + (i-1)*MENU_SAVED_STEP,
                           sesslist.sessions[i]);
            } else if (unfiled) {
                /* Unfiled, or more folders than this menu will hold: either way
                 * the session stays VISIBLE at the top level rather than being
                 * dropped into a folder that was never created. */
                unfiled[nunfiled++] = i;
            }
            sfree(fld);
        }

        for (j = 0; j < nfolders; j++) {
            AppendMenu(wgs->savedsess_menu, MF_POPUP | MF_ENABLED,
                       (UINT_PTR)fmenu[j], fname[j]);
            sfree(fname[j]);
        }
        if (nfolders && nunfiled)
            AppendMenu(wgs->savedsess_menu, MF_SEPARATOR, 0, 0);
        for (j = 0; j < nunfiled; j++)
            AppendMenu(wgs->savedsess_menu, MF_ENABLED,
                       IDM_SAVED_MIN + (unfiled[j]-1)*MENU_SAVED_STEP,
                       sesslist.sessions[unfiled[j]]);
        sfree(unfiled);

        if (sesslist.nsessions <= 1)
            AppendMenu(wgs->savedsess_menu, MF_GRAYED, IDM_SAVED_MIN,
                       "(No sessions)");
        return;
    }
#undef KITTY_MENU_FOLDERS_MAX
#endif
    /* skip sesslist.sessions[0] == Default Settings */
    for (i = 1; i < limit; i++)
        AppendMenu(wgs->savedsess_menu, MF_ENABLED,
                   IDM_SAVED_MIN + (i-1)*MENU_SAVED_STEP,
                   sesslist.sessions[i]);
    if (sesslist.nsessions <= 1)
        AppendMenu(wgs->savedsess_menu, MF_GRAYED, IDM_SAVED_MIN,
                   "(No sessions)");
}

/*
 * Update the Special Commands submenu.
 */
static void win_seat_update_specials_menu(Seat *seat)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    HMENU new_menu;
    int i, j;

    if (wgs->backend)
        wgs->specials = backend_get_specials(wgs->backend);
    else
        wgs->specials = NULL;

    if (wgs->specials) {
        /* We can't use Windows to provide a stack for submenus, so
         * here's a lame "stack" that will do for now. */
        HMENU saved_menu = NULL;
        int nesting = 1;
        new_menu = CreatePopupMenu();
        for (i = 0; nesting > 0; i++) {
            assert(IDM_SPECIAL_MIN + 0x10 * i < IDM_SPECIAL_MAX);
            switch (wgs->specials[i].code) {
              case SS_SEP:
                AppendMenu(new_menu, MF_SEPARATOR, 0, 0);
                break;
              case SS_SUBMENU:
                assert(nesting < 2);
                nesting++;
                saved_menu = new_menu; /* XXX lame stacking */
                new_menu = CreatePopupMenu();
                AppendMenu(saved_menu, MF_POPUP | MF_ENABLED,
                           (UINT_PTR) new_menu, wgs->specials[i].name);
                break;
              case SS_EXITMENU:
                nesting--;
                if (nesting) {
                    new_menu = saved_menu; /* XXX lame stacking */
                    saved_menu = NULL;
                }
                break;
              default:
                AppendMenu(new_menu, MF_ENABLED, IDM_SPECIAL_MIN + 0x10 * i,
                           wgs->specials[i].name);
                break;
            }
        }
        /* Squirrel the highest special. */
        wgs->n_specials = i - 1;
    } else {
        new_menu = NULL;
        wgs->n_specials = 0;
    }

    for (j = 0; j < lenof(wgs->popup_menus); j++) {
        if (wgs->specials_menu) {
            /* XXX does this free up all submenus? */
            DeleteMenu(wgs->popup_menus[j].menu, (UINT_PTR)wgs->specials_menu,
                       MF_BYCOMMAND);
            DeleteMenu(wgs->popup_menus[j].menu, IDM_SPECIALSEP, MF_BYCOMMAND);
        }
        if (new_menu) {
            InsertMenu(wgs->popup_menus[j].menu, IDM_SHOWLOG,
                       MF_BYCOMMAND | MF_POPUP | MF_ENABLED,
                       (UINT_PTR) new_menu, "S&pecial Command");
            InsertMenu(wgs->popup_menus[j].menu, IDM_SHOWLOG,
                       MF_BYCOMMAND | MF_SEPARATOR, IDM_SPECIALSEP, 0);
        }
    }
    wgs->specials_menu = new_menu;
}

static void update_mouse_pointer(WinGuiSeat *wgs)
{
    LPTSTR curstype = NULL;
    bool force_visible = false;
    static bool forced_visible = false;
    switch (wgs->busy_status) {
      case BUSY_NOT:
        if (wgs->pointer_indicates_raw_mouse)
            curstype = IDC_ARROW;
        else
            curstype = IDC_IBEAM;
        break;
      case BUSY_WAITING:
        curstype = IDC_APPSTARTING; /* this may be an abuse */
        force_visible = true;
        break;
      case BUSY_CPU:
        curstype = IDC_WAIT;
        force_visible = true;
        break;
      default:
        unreachable("Bad busy_status");
    }
    {
        HCURSOR cursor = LoadCursor(NULL, curstype);
        SetClassLongPtr(wgs->term_hwnd, GCLP_HCURSOR, (LONG_PTR)cursor);
        SetCursor(cursor); /* force redraw of cursor at current posn */
    }
    if (force_visible != forced_visible) {
        /* We want some cursor shapes to be visible always.
         * Along with show_mouseptr(), this manages the ShowCursor()
         * counter such that if we switch back to a non-force_visible
         * cursor, the previous visibility state is restored. */
        ShowCursor(force_visible);
        forced_visible = force_visible;
    }
}

static void win_seat_set_busy_status(Seat *seat, BusyStatus status)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    wgs->busy_status = status;
    update_mouse_pointer(wgs);
}

static void wintw_set_raw_mouse_mode(TermWin *tw, bool activate)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    wgs->send_raw_mouse = activate;
}

static void wintw_set_raw_mouse_mode_pointer(TermWin *tw, bool activate)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    wgs->pointer_indicates_raw_mouse = activate;
    update_mouse_pointer(wgs);
}

#ifdef MOD_RECONNECT
/* A fatal disconnect whose reason is authentication-related must NOT trigger
 * auto-reconnect: re-dialing with the same rejected credentials just burns the
 * server's MaxAuthTries and gets the client IP banned. PuTTY's fatal messages
 * for these cases all contain the word "authentication" (e.g. "Too many
 * authentication failures", "No supported authentication methods available").
 * Case-insensitive substring scan, no platform-specific helpers. */
static bool kitty_is_auth_failure_msg(const char *msg)
{
    static const char needle[] = "authentication";
    if (!msg)
        return false;
    for (const char *p = msg; *p; p++) {
        size_t k = 0;
        while (needle[k] && p[k] &&
               tolower((unsigned char)p[k]) == needle[k])
            k++;
        if (!needle[k])
            return true;
    }
    return false;
}
#endif

/* Some network devices (switches/routers) emit a channel-referencing packet
 * (e.g. SSH2_MSG_CHANNEL_REQUEST) for a channel they just tore down, right after
 * sending a clean "exit status 0" on logout. PuTTY makes any message for a
 * nonexistent channel a FATAL protocol error, so without special handling an
 * ordinary logout would either spuriously auto-reconnect or pop a scary fatal
 * box. Recognise that post-logout artifact so connection_fatal can close
 * cleanly. (Observed on a Cisco switch: exit status 0, immediately followed by
 * SSH2_MSG_CHANNEL_REQUEST for a nonexistent channel.)
 *
 * NOTE: this is belt-and-braces with ssh_post_exit_teardown_error() in
 * ssh/ssh.c, which suppresses the fatal at the protocol layer -- but (since
 * v2 of the upstream patch) only when the exit code is known AND the
 * connection has nothing else live (no forwarding/X11/agent channels, no
 * sharing downstreams). This helper additionally catches the same device
 * artifact if it arrives without an exit status, or while another channel
 * is still open. Kept for now; revisit if upstream PuTTY accepts the ssh.c
 * change. */
static bool kitty_is_benign_channel_close_msg(const char *msg)
{
    return msg && strstr(msg, "nonexistent channel") != NULL;
}

/* KiTTY hyperlink underline: repaint only the rows whose link membership
 * changed in the last kitty_url_rescan(), instead of InvalidateRect(whole
 * window) which forced a full repaint on every screen update and flickered on
 * live output.  Rows being drawn anyway just repaint once more (same content,
 * no visible flicker); untouched rows are left alone. */
#ifdef MOD_PERSO   /* KiTTY-only; both call sites are MOD_PERSO-guarded too, so
                    * this is inert (and uncompiled) in the stock GUI variants. */
static void kitty_url_invalidate_dirty_rows(WinGuiSeat *wgs)
{
    int r;
    RECT rc;
    if (!wgs->term || !wgs->term_hwnd)
        return;
    for (r = 0; r < wgs->term->rows; r++) {
        if (!kitty_url_row_dirty(r))
            continue;
        rc.left   = wgs->offset_width;
        rc.top    = wgs->offset_height + r * wgs->font_height;
        rc.right  = wgs->offset_width + wgs->term->cols * wgs->font_width;
        rc.bottom = rc.top + wgs->font_height;
        InvalidateRect(wgs->term_hwnd, &rc, FALSE);
    }
}
#endif

/*
 * Print a message box and close the connection.
 */
static void win_seat_connection_fatal(Seat *seat, const char *msg)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
#ifdef MOD_PERSO
    /* KiTTY: a benign post-logout channel artifact (see helper) is not a real
     * disconnect -- the remote already sent a clean exit status. Treat it as a
     * normal session end: no auto-reconnect, no fatal box, honour close-on-exit
     * (the exit was clean, so AUTO closes the window). */
    if (!GetPuttyFlag() && kitty_is_benign_channel_close_msg(msg)) {
        int coe = conf_get_int(wgs->conf, CONF_close_on_exit);
        show_mouseptr(wgs, true);
        wgs->session_closed = true;
        if (coe == FORCE_ON || coe == AUTO) {
            /* KiTTY: defer while the user is reading the Event Log, exactly as
             * exit_callback and the fatal-error close below do. This path was
             * missed when that was introduced, and it is the one a Cisco takes:
             * its late channel message arrives AFTER the session ended, so the
             * program quit from under an open Event Log - at whatever moment
             * the message loop next ran, which looks like "the window vanished
             * when I clicked something". */
            if (kitty_eventlog_is_open()) {
                kitty_coe_pending_exit = 0;
                logevent(wgs->logctx, "Session ended; window kept open while "
                         "the Event Log is open (it closes when you close the "
                         "log)");
                return;
            }
            kitty_on_window_closing(wgs, wgs->term_hwnd);
            PostQuitMessage(0);
        } else {
            queue_toplevel_callback(close_session, wgs);
        }
        return;
    }
#endif
#ifdef MOD_RECONNECT
    /* KiTTY auto-reconnect: on an abnormal drop of a session that had FULLY
     * authenticated at least once, arm the reconnect timer instead of
     * message-boxing -- but never on an authentication failure (would burn the
     * server's auth-try budget and risk an IP ban), never after the normal exit
     * path has already marked the session closed, and never on a session that
     * never authenticated (gated per-session, not on the stale process-global
     * is_backend_first_connected).
     *
     * The per-session FailureReconnect option is part of the CONDITION, not
     * just of the timer below: inherited from classic KiTTY, this branch was
     * entered on the global flag alone (which defaults to ON), so a session
     * with reconnect-on-failure switched OFF still had its fatal error
     * swallowed here - no retry AND no error shown, the window just went
     * quiet. A session that is not going to retry now falls through to the
     * normal inline-error path, like the other three call sites of this flag
     * pair (start_backend, the wakeup handler, the keypress handler). */
    if (GetAutoreconnectFlag() && conf_get_int(wgs->conf, CONF_failure_reconnect) &&
        !wgs->session_closed &&
        wgs->ever_authenticated && !kitty_is_auth_failure_msg(msg)) {
        SetConnBreakIcon(wgs->term_hwnd);
        SetSSHConnected(0);
        wgs->session_closed = true;
        /* KiTTY (hknet/KiTTY#22): the link was lost, so the titlebar gets the
         * warning marker here too. Without this the marker was unreachable
         * whenever auto-reconnect is enabled - i.e. in exactly the case that
         * leaves a screenful of dead windows - because this branch returns
         * before the one below that sets the flag. start_backend() clears it
         * again once the session is back. */
        wgs->error_close = true;
        queue_toplevel_callback(close_session, wgs);
        lp_eventlog(&wgs->logpolicy, "Lost connection, trying to reconnect...");
        if (wgs->reconnect_tries < 1000) {
            wgs->reconnect_tries++;
            SetTimer(wgs->term_hwnd, TIMER_RECONNECT,
                     GetReconnectDelay()*1000, NULL);
        }
        return;
    }
#endif
#ifdef MOD_PERSO
    /*
     * KiTTY, workplace proxy mode (design §9): this connection went through the
     * mode's proxy and never came up. Overwhelmingly that means the user has
     * left the place where that proxy exists - so say so, and offer to switch
     * the mode off, at the one moment the offer is useful.
     *
     * A NOTICE, not a dialog: the whole point of the inline error below is that
     * a fatal error does not block the terminal (#548), and a modal question
     * here would take that back. Clicking the notice switches the mode off.
     *
     * Gated on never having authenticated, so this is about a connection that
     * failed to establish, not about a working session that later dropped.
     */
    {
        char wp[256];
        if (!wgs->ever_authenticated && wgs->workplace_proxied &&
            kitty_workplace_query(wp, sizeof(wp))) {
            char *note = dupprintf(
                "This connection went through \"%s\" because workplace proxy "
                "mode is on, and it did not come up. If you have left the place "
                "that proxy belongs to, click here to switch the mode off.", wp);
            kitty_notice_show("Workplace proxy mode is on", note,
                              RGB(0, 100, 0), 20, wgs->term_hwnd,
                              WM_KITTY_WORKPLACE_DISARM);
            sfree(note);
        }
    }
    /* KiTTY: instead of a modal "Fatal Error" box that blocks the terminal,
     * print the error INLINE (red label, default-coloured detail) and let the
     * session go inactive, so the user can read it and choose Restart / next
     * steps without an OK-click (cf. upstream #548). Auto-reconnect has already
     * been ruled out above for this disconnect. The modal box is kept only when
     * "close window on exit" is forced ON (the window is about to vanish, so
     * inline text wouldn't be seen) or in PuTTY-compat mode. */
    if (!GetPuttyFlag() && !GetModalErrorsFlag() && conf_get_int(wgs->conf, CONF_close_on_exit) != FORCE_ON) {
        kitty_term_print_inline_error(wgs->term, msg, true);
        show_mouseptr(wgs, true);
        wgs->error_close = true;   /* #548: warning titlebar marker via close_session */
        queue_toplevel_callback(close_session, wgs);
        return;
    }
#endif
    char *title = dupprintf("%s Fatal Error", appname);
    show_mouseptr(wgs, true);
    MessageBox(wgs->term_hwnd, msg, title, MB_ICONERROR | MB_OK);
    sfree(title);

    bool coe_force = (conf_get_int(wgs->conf, CONF_close_on_exit) == FORCE_ON);
#ifdef MOD_PERSO
    /* KiTTY: as in exit_callback - an open Event Log is the user reading why
     * this went wrong, and a fatal error is exactly when that matters. */
    if (coe_force && kitty_eventlog_is_open()) {
        coe_force = false;
        kitty_coe_pending_exit = 1;   /* close once the log is dismissed */
        logevent(wgs->logctx, "Connection closed; window kept open while the "
                 "Event Log is open (it closes when you close the log)");
    }
#endif
    if (coe_force) {
#ifdef MOD_PERSO
        /* Same as exit_callback: this fatal-error close uses PostQuitMessage
         * (no WM_DESTROY), so save the remembered position here too. */
        kitty_on_window_closing(wgs, wgs->term_hwnd);
#endif
        PostQuitMessage(1);
    } else {
        queue_toplevel_callback(close_session, wgs);
    }
}

/*
 * Print a message box and don't close the connection.
 */
static void win_seat_nonfatal(Seat *seat, const char *msg)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
#ifdef MOD_PERSO
    /* KiTTY (upstream cyd01/KiTTY #548): surface non-fatal errors INLINE in the terminal (yellow
     * label) instead of a modal box that traps the window. The connection stays
     * up, so we only print -- no session close. PuTTY-compat mode (GetPuttyFlag)
     * keeps the classic modal box. */
    if (!GetPuttyFlag() && !GetModalErrorsFlag() && wgs->term) {
        kitty_term_print_inline_error(wgs->term, msg, false);
        show_mouseptr(wgs, true);
        return;
    }
#endif
    char *title = dupprintf("%s Error", appname);
    show_mouseptr(wgs, true);
    MessageBox(wgs->term_hwnd, msg, title, MB_ICONERROR | MB_OK);
    sfree(title);
}

static HWND find_window_for_msgbox(void)
{
    if (wgslisthead.next != &wgslisthead) {
        WinGuiSeat *wgs = container_of(
            wgslisthead.next, WinGuiSeat, wgslistnode);
        return wgs->term_hwnd;
    }
    return NULL;
}

/*
 * Report an error at the command-line parsing stage.
 */
void cmdline_error(const char *fmt, ...)
{
    va_list ap;
    char *message, *title;

    va_start(ap, fmt);
    message = dupvprintf(fmt, ap);
    va_end(ap);
    title = dupprintf("%s Command Line Error", appname);
    MessageBox(find_window_for_msgbox(), message, title, MB_ICONERROR | MB_OK);
    sfree(message);
    sfree(title);
    exit(1);
}

static inline rgb rgb_from_colorref(COLORREF cr)
{
    rgb toret;
    toret.r = GetRValue(cr);
    toret.g = GetGValue(cr);
    toret.b = GetBValue(cr);
    return toret;
}

static void wintw_palette_get_overrides(TermWin *tw, Terminal *term)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (conf_get_bool(wgs->conf, CONF_system_colour)) {
        rgb rgb;

        rgb = rgb_from_colorref(GetSysColor(COLOR_WINDOWTEXT));
        term_palette_override(term, OSC4_COLOUR_fg, rgb);
        term_palette_override(term, OSC4_COLOUR_fg_bold, rgb);

        rgb = rgb_from_colorref(GetSysColor(COLOR_WINDOW));
        term_palette_override(term, OSC4_COLOUR_bg, rgb);
        term_palette_override(term, OSC4_COLOUR_bg_bold, rgb);

        rgb = rgb_from_colorref(GetSysColor(COLOR_HIGHLIGHTTEXT));
        term_palette_override(term, OSC4_COLOUR_cursor_fg, rgb);

        rgb = rgb_from_colorref(GetSysColor(COLOR_HIGHLIGHT));
        term_palette_override(term, OSC4_COLOUR_cursor_bg, rgb);
    }
}

/*
 * This is a wrapper to ExtTextOut() to force Windows to display
 * the precise glyphs we give it. Otherwise it would do its own
 * bidi and Arabic shaping, and we would end up uncertain which
 * characters it had put where.
 */
static void exact_textout(HDC hdc, int x, int y, CONST RECT *lprc,
                          unsigned short *lpString, UINT cbCount,
                          CONST INT *lpDx, bool opaque)
{
#if HAVE_GCP_RESULTSW
    GCP_RESULTSW gcpr;
#else
    /*
     * If building against old enough headers that the GCP_RESULTSW
     * type isn't available, we can make do with GCP_RESULTS proper:
     * the differences aren't important to us (the only variable-width
     * string parameter is one we don't use anyway).
     */
    GCP_RESULTS gcpr;
#endif
    char *buffer = snewn(cbCount*2+2, char);
    char *classbuffer = snewn(cbCount, char);
    memset(&gcpr, 0, sizeof(gcpr));
    memset(buffer, 0, cbCount*2+2);
    memset(classbuffer, GCPCLASS_NEUTRAL, cbCount);

    gcpr.lStructSize = sizeof(gcpr);
    gcpr.lpGlyphs = (void *)buffer;
    gcpr.lpClass = (void *)classbuffer;
    gcpr.nGlyphs = cbCount;
    GetCharacterPlacementW(hdc, lpString, cbCount, 0, &gcpr,
                           FLI_MASK | GCP_CLASSIN | GCP_DIACRITIC);

    ExtTextOut(hdc, x, y,
               ETO_GLYPH_INDEX | ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
               lprc, buffer, cbCount, lpDx);
}

/*
 * The exact_textout() wrapper, unfortunately, destroys the useful
 * Windows `font linking' behaviour: automatic handling of Unicode
 * code points not supported in this font by falling back to a font
 * which does contain them. Therefore, we adopt a multi-layered
 * approach: for any potentially-bidi text, we use exact_textout(),
 * and for everything else we use a simple ExtTextOut as we did
 * before exact_textout() was introduced.
 */
static void general_textout(
    WinGuiSeat *wgs, HDC hdc, int x, int y, CONST RECT *lprc,
    unsigned short *lpString, UINT cbCount, CONST INT *lpDx, bool opaque)
{
    int i, j, xp, xn;
    int bkmode = 0;
    bool got_bkmode = false;

    xp = xn = x;

    for (i = 0; i < (int)cbCount ;) {
        bool rtl = is_rtl(lpString[i]);

        xn += lpDx[i];

        for (j = i+1; j < (int)cbCount; j++) {
            if (rtl != is_rtl(lpString[j]))
                break;
            xn += lpDx[j];
        }

        /*
         * Now [i,j) indicates a maximal substring of lpString
         * which should be displayed using the same textout
         * function.
         */
        if (rtl) {
            exact_textout(hdc, xp, y, lprc, lpString+i, j-i,
                          wgs->font_varpitch ? NULL : lpDx+i, opaque);
        } else {
            ExtTextOutW(hdc, xp, y, ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
                        lprc, lpString+i, j-i,
                        wgs->font_varpitch ? NULL : lpDx+i);
        }

        i = j;
        xp = xn;

        bkmode = GetBkMode(hdc);
        got_bkmode = true;
        SetBkMode(hdc, TRANSPARENT);
        opaque = false;
    }

    if (got_bkmode)
        SetBkMode(hdc, bkmode);
}

static int get_font_width(WinGuiSeat *wgs, HDC hdc, const TEXTMETRIC *tm)
{
    int ret;
    /* Note that the TMPF_FIXED_PITCH bit is defined upside down :-( */
    if (!(tm->tmPitchAndFamily & TMPF_FIXED_PITCH)) {
        ret = tm->tmAveCharWidth;
    } else {
#define FIRST '0'
#define LAST '9'
        ABCFLOAT widths[LAST-FIRST + 1];
        int j;

        wgs->font_varpitch = true;
        wgs->font_dualwidth = true;
        if (GetCharABCWidthsFloat(hdc, FIRST, LAST, widths)) {
            ret = 0;
            for (j = 0; j < lenof(widths); j++) {
                int width = (int)(0.5 + widths[j].abcfA +
                                  widths[j].abcfB + widths[j].abcfC);
                if (ret < width)
                    ret = width;
            }
        } else {
            ret = tm->tmMaxCharWidth;
        }
#undef FIRST
#undef LAST
    }
    return ret;
}

static void init_dpi_info(WinGuiSeat *wgs)
{
    if (wgs->dpi_info.cur_dpi.x == 0 || wgs->dpi_info.cur_dpi.y == 0) {
        if (p_GetDpiForMonitor && p_MonitorFromWindow) {
            UINT dpiX, dpiY;
            HMONITOR currentMonitor = p_MonitorFromWindow(
                wgs->term_hwnd, MONITOR_DEFAULTTOPRIMARY);
            if (p_GetDpiForMonitor(currentMonitor, MDT_EFFECTIVE_DPI,
                                   &dpiX, &dpiY) == S_OK) {
                wgs->dpi_info.cur_dpi.x = (int)dpiX;
                wgs->dpi_info.cur_dpi.y = (int)dpiY;
            }
        }

        /* Fall back to system DPI */
        if (wgs->dpi_info.cur_dpi.x == 0 || wgs->dpi_info.cur_dpi.y == 0) {
            HDC hdc = GetDC(wgs->term_hwnd);
            wgs->dpi_info.cur_dpi.x = GetDeviceCaps(hdc, LOGPIXELSX);
            wgs->dpi_info.cur_dpi.y = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(wgs->term_hwnd, hdc);
        }
    }
}

/*
 * Initialise all the fonts we will need initially. There may be as many as
 * three or as few as one.  The other (potentially) twenty-one fonts are done
 * if/when they are needed.
 *
 * We also:
 *
 * - check the font width and height, correcting our guesses if
 *   necessary.
 *
 * - verify that the bold font is the same width as the ordinary
 *   one, and engage shadow bolding if not.
 *
 * - verify that the underlined font is the same width as the
 *   ordinary one (manual underlining by means of line drawing can
 *   be done in a pinch).
 *
 * - find a trust sigil icon that will look OK with the chosen font.
 */
static void init_fonts(WinGuiSeat *wgs, int pick_width, int pick_height)
{
    TEXTMETRIC tm;
    OUTLINETEXTMETRIC otm;
    CPINFO cpinfo;
    FontSpec *font;
    int fontsize[3];
    int i;
    int quality;
    HDC hdc;
    int fw_dontcare, fw_bold;

    for (i = 0; i < FONT_MAXNO; i++)
        wgs->fonts[i] = NULL;

    wgs->bold_font_mode =
        conf_get_int(wgs->conf, CONF_bold_style) & BOLD_STYLE_FONT ?
        BOLD_FONT : BOLD_NONE;
    wgs->bold_colours =
        conf_get_int(wgs->conf, CONF_bold_style) & BOLD_STYLE_COLOUR ?
        true : false;
    wgs->und_mode = UND_FONT;

    font = conf_get_fontspec(wgs->conf, CONF_font);
    if (font->isbold) {
        fw_dontcare = FW_BOLD;
        fw_bold = FW_HEAVY;
    } else {
        fw_dontcare = FW_DONTCARE;
        fw_bold = FW_BOLD;
    }

    hdc = GetDC(wgs->term_hwnd);

#ifdef MOD_PERSO
    /*
     * KiTTY line spacing (CONF_line_spacing, a percentage; 100 = the font's own
     * metrics). Needed here because the branch below has to know it: a
     * pick_height handed to us is a CELL height that ALREADY includes the extra,
     * and the font itself must be created at the glyph height. Resizing calls
     * this function with the current font_height, so without the inverse step a
     * cell would grow a little more on every resize.
     *
     * Clamped: under 100 would crop glyphs against their own metrics, and past
     * 300 the text is lost in the gap.
     */
    int kitty_linespc = conf_get_int(wgs->conf, CONF_line_spacing);
    if (kitty_linespc < 100) kitty_linespc = 100;
    if (kitty_linespc > 300) kitty_linespc = 300;
#endif

    if (pick_height) {
#ifdef MOD_PERSO
        wgs->font_height = MulDiv(pick_height, 100, kitty_linespc);  /* cell -> glyph */
#else
        wgs->font_height = pick_height;
#endif
    } else {
        wgs->font_height = font->height;
        if (wgs->font_height > 0) {
            wgs->font_height = -MulDiv(
                wgs->font_height, wgs->dpi_info.cur_dpi.y, 72);
        }
    }
    wgs->font_width = pick_width;

    quality = conf_get_int(wgs->conf, CONF_font_quality);
#define f(i,c,w,u)                                                      \
    wgs->fonts[i] = CreateFont(                                         \
        wgs->font_height, wgs->font_width, 0, 0, w, false, u, false, c, \
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, FONT_QUALITY(quality), \
        FIXED_PITCH | FF_DONTCARE, font->name)

    f(FONT_NORMAL, font->charset, fw_dontcare, false);

    SelectObject(hdc, wgs->fonts[FONT_NORMAL]);
    GetTextMetrics(hdc, &tm);
    if (GetOutlineTextMetrics(hdc, sizeof(otm), &otm))
        wgs->font_strikethrough_y = tm.tmAscent - otm.otmsStrikeoutPosition;
    else
        wgs->font_strikethrough_y = tm.tmAscent - (tm.tmAscent * 3 / 8);

    GetObject(wgs->fonts[FONT_NORMAL], sizeof(LOGFONT), &wgs->lfont);

    /* Note that the TMPF_FIXED_PITCH bit is defined upside down :-( */
    if (!(tm.tmPitchAndFamily & TMPF_FIXED_PITCH)) {
        wgs->font_varpitch = false;
        wgs->font_dualwidth = (tm.tmAveCharWidth != tm.tmMaxCharWidth);
    } else {
        wgs->font_varpitch = true;
        wgs->font_dualwidth = true;
    }
    if (pick_width == 0 || pick_height == 0) {
        wgs->font_height = tm.tmHeight;
        wgs->font_width = get_font_width(wgs, hdc, &tm);
    }

    {
        CHARSETINFO info;
        DWORD cset = tm.tmCharSet;
        memset(&info, 0xFF, sizeof(info));

        /* !!! Yes the next line is right */
        if (cset == OEM_CHARSET)
            wgs->ucsdata.font_codepage = GetOEMCP();
        else if (TranslateCharsetInfo ((DWORD *)(ULONG_PTR)cset,
                                       &info, TCI_SRCCHARSET))
            wgs->ucsdata.font_codepage = info.ciACP;
        else
            wgs->ucsdata.font_codepage = -1;

        GetCPInfo(wgs->ucsdata.font_codepage, &cpinfo);
        wgs->ucsdata.dbcs_screenfont = (cpinfo.MaxCharSize > 1);
    }

    f(FONT_UNDERLINE, font->charset, fw_dontcare, true);

    /*
     * Some fonts, e.g. 9-pt Courier, draw their underlines
     * outside their character cell. We successfully prevent
     * screen corruption by clipping the text output, but then
     * we lose the underline completely. Here we try to work
     * out whether this is such a font, and if it is, we set a
     * flag that causes underlines to be drawn by hand.
     *
     * Having tried other more sophisticated approaches (such
     * as examining the TEXTMETRIC structure or requesting the
     * height of a string), I think we'll do this the brute
     * force way: we create a small bitmap, draw an underlined
     * space on it, and test to see whether any pixels are
     * foreground-coloured. (Since we expect the underline to
     * go all the way across the character cell, we only search
     * down a single column of the bitmap, half way across.)
     */
    {
        HDC und_dc;
        HBITMAP und_bm, und_oldbm;
        int i;
        bool gotit;
        COLORREF c;

        und_dc = CreateCompatibleDC(hdc);
        und_bm = CreateCompatibleBitmap(
            hdc, wgs->font_width, wgs->font_height);
        und_oldbm = SelectObject(und_dc, und_bm);
        SelectObject(und_dc, wgs->fonts[FONT_UNDERLINE]);
        SetTextAlign(und_dc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
        SetTextColor(und_dc, RGB(255, 255, 255));
        SetBkColor(und_dc, RGB(0, 0, 0));
        SetBkMode(und_dc, OPAQUE);
        ExtTextOut(und_dc, 0, 0, ETO_OPAQUE, NULL, " ", 1, NULL);
        gotit = false;
        for (i = 0; i < wgs->font_height; i++) {
            c = GetPixel(und_dc, wgs->font_width / 2, i);
            if (c != RGB(0, 0, 0))
                gotit = true;
        }
        SelectObject(und_dc, und_oldbm);
        DeleteObject(und_bm);
        DeleteDC(und_dc);
        if (!gotit) {
            wgs->und_mode = UND_LINE;
            DeleteObject(wgs->fonts[FONT_UNDERLINE]);
            wgs->fonts[FONT_UNDERLINE] = 0;
        }
    }

    if (wgs->bold_font_mode == BOLD_FONT) {
        f(FONT_BOLD, font->charset, fw_bold, false);
    }
#undef f

    wgs->descent = tm.tmAscent + 1;
    if (wgs->descent >= wgs->font_height)
        wgs->descent = wgs->font_height - 1;

#ifdef MOD_PERSO
    /*
     * KiTTY line spacing: the glyph height is settled by here, so grow the CELL
     * and remember by how much the glyph has to move down inside it. The extra
     * is split evenly above and below - hanging it all underneath looks like a
     * misaligned font rather than like spacing.
     *
     * descent and font_strikethrough_y both follow the glyph, because they
     * position the underline, the underline cursor and the strike-through, and
     * all three are measured downwards from the TOP of the cell. Leave them
     * alone and they stay where the text used to be: the strike-through in
     * particular then lands above the letters and reads as an overline.
     * Everything else keys off font_height and needs no changes - the
     * background of a cell is filled to the full, taller rectangle, so the extra
     * space carries the cell's own colour.
     *
     * ⚠️ Box-drawing characters (U+2500 and friends) stop joining vertically at
     * anything above 100: the glyph is drawn at its own size in a taller cell,
     * so gaps appear between rows. That is inherent - every terminal offering
     * line spacing does it - and is why the default is 100.
     */
    wgs->font_yshift = 0;
    if (kitty_linespc > 100) {
        int glyph_height = wgs->font_height;
        int cell_height = MulDiv(glyph_height, kitty_linespc, 100);
        wgs->font_yshift = (cell_height - glyph_height) / 2;
        wgs->font_height = cell_height;
        wgs->descent += wgs->font_yshift;
        wgs->font_strikethrough_y += wgs->font_yshift;
    }
#endif

    for (i = 0; i < 3; i++) {
        if (wgs->fonts[i]) {
            if (SelectObject(hdc, wgs->fonts[i]) && GetTextMetrics(hdc, &tm))
                fontsize[i] = (get_font_width(wgs, hdc, &tm) +
                               256 * tm.tmHeight);
            else
                fontsize[i] = -i;
        } else
            fontsize[i] = -i;
    }

#ifdef MOD_PERSO
    /* KiTTY font fallback: (re)build the fallback state against the freshly
     * created primary font (also runs on font/DPI changes via reset_window's
     * deinit_fonts + init_fonts cycle). No-op when [FontFallback] active=no. */
    winfb_reinit_from_config(hdc, &wgs->lfont, wgs->font_width,
                             wgs->font_height);
#endif

    ReleaseDC(wgs->term_hwnd, hdc);

    if (trust_icon != INVALID_HANDLE_VALUE) {
        DestroyIcon(trust_icon);
    }
    trust_icon = LoadImage(hinst, MAKEINTRESOURCE(IDI_MAINICON),
                           IMAGE_ICON, wgs->font_width*2, wgs->font_height,
                           LR_DEFAULTCOLOR);

    if (fontsize[FONT_UNDERLINE] != fontsize[FONT_NORMAL]) {
        wgs->und_mode = UND_LINE;
        DeleteObject(wgs->fonts[FONT_UNDERLINE]);
        wgs->fonts[FONT_UNDERLINE] = 0;
    }

    if (wgs->bold_font_mode == BOLD_FONT &&
        fontsize[FONT_BOLD] != fontsize[FONT_NORMAL]) {
        wgs->bold_font_mode = BOLD_SHADOW;
        DeleteObject(wgs->fonts[FONT_BOLD]);
        wgs->fonts[FONT_BOLD] = 0;
    }
    wgs->fontflag[0] = true;
    wgs->fontflag[1] = true;
    wgs->fontflag[2] = true;

    init_ucs(wgs->conf, &wgs->ucsdata);
}

static void another_font(WinGuiSeat *wgs, int fontno)
{
    int basefont;
    int fw_dontcare, fw_bold, quality;
    int c, w, x;
    bool u;
    char *s;
    FontSpec *font;

    if (fontno < 0 || fontno >= FONT_MAXNO || wgs->fontflag[fontno])
        return;

    basefont = (fontno & ~(FONT_BOLDUND));
    if (basefont != fontno && !wgs->fontflag[basefont])
        another_font(wgs, basefont);

    font = conf_get_fontspec(wgs->conf, CONF_font);

    if (font->isbold) {
        fw_dontcare = FW_BOLD;
        fw_bold = FW_HEAVY;
    } else {
        fw_dontcare = FW_DONTCARE;
        fw_bold = FW_BOLD;
    }

    c = font->charset;
    w = fw_dontcare;
    u = false;
    s = font->name;
    x = wgs->font_width;

    if (fontno & FONT_WIDE)
        x *= 2;
    if (fontno & FONT_NARROW)
        x = (x+1)/2;
    if (fontno & FONT_OEM)
        c = OEM_CHARSET;
    if (fontno & FONT_BOLD)
        w = fw_bold;
    if (fontno & FONT_UNDERLINE)
        u = true;

    quality = conf_get_int(wgs->conf, CONF_font_quality);

    wgs->fonts[fontno] =
        CreateFont(wgs->font_height * (1 + !!(fontno & FONT_HIGH)), x, 0, 0, w,
                   false, u, false, c, OUT_DEFAULT_PRECIS,
                   CLIP_DEFAULT_PRECIS, FONT_QUALITY(quality),
                   DEFAULT_PITCH | FF_DONTCARE, s);

    wgs->fontflag[fontno] = true;
}

static void deinit_fonts(WinGuiSeat *wgs)
{
    int i;
    for (i = 0; i < FONT_MAXNO; i++) {
        if (wgs->fonts[i])
            DeleteObject(wgs->fonts[i]);
        wgs->fonts[i] = 0;
        wgs->fontflag[i] = false;
    }

    if (trust_icon != INVALID_HANDLE_VALUE) {
        DestroyIcon(trust_icon);
    }
    trust_icon = INVALID_HANDLE_VALUE;
#ifdef MOD_PERSO
    winfb_cleanup();                   /* KiTTY font fallback */
#endif
}

static void wintw_request_resize(TermWin *tw, int w, int h)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    const struct BackendVtable *vt;
    int width, height;
    int resize_action = conf_get_int(wgs->conf, CONF_resize_action);
    bool deny_resize = false;

    /* Suppress server-originated resizing attempts if local resizing
     * is disabled entirely, or if it's supposed to change
     * rows/columns but the window is maximised. */
    if (resize_action == RESIZE_DISABLED
        || (resize_action == RESIZE_TERM && IsZoomed(wgs->term_hwnd))) {
        deny_resize = true;
    }

    vt = backend_vt_from_proto(be_default_protocol);
    if (vt && vt->flags & BACKEND_RESIZE_FORBIDDEN)
        deny_resize = true;
    if (h == wgs->term->rows && w == wgs->term->cols) deny_resize = true;

    /* We still need to acknowledge a suppressed resize attempt. */
    if (deny_resize) {
        term_resize_request_completed(wgs->term);
        return;
    }

    /* Sanity checks ... */
    {
        RECT ss;
        if (get_fullscreen_rect(wgs, &ss)) {
            /* Make sure the values aren't too big */
            width = (ss.right - ss.left - wgs->extra_width) / 4;
            height = (ss.bottom - ss.top - wgs->extra_height) / 6;

            if (w > width || h > height) {
                term_resize_request_completed(wgs->term);
                return;
            }
            if (w < 15)
                w = 15;
            if (h < 1)
                h = 1;
        }
    }

    if (resize_action != RESIZE_FONT && !IsZoomed(wgs->term_hwnd)
        && !KITTY_IS_EMBEDDED(wgs->term_hwnd)) {
        width = wgs->extra_width + wgs->font_width * w;
        height = wgs->extra_height + wgs->font_height * h;

        SetWindowPos(wgs->term_hwnd, NULL, 0, 0, width, height,
                     SWP_NOACTIVATE | SWP_NOCOPYBITS |
                     SWP_NOMOVE | SWP_NOZORDER);
    } else {
        /*
         * If we're resizing by changing the font, we must tell the
         * terminal the new size immediately, so that reset_window
         * will know what to do.
         */
        term_size(wgs->term, h, w, conf_get_int(wgs->conf, CONF_savelines));
        reset_window(wgs, 0);
    }

    term_resize_request_completed(wgs->term);
    InvalidateRect(wgs->term_hwnd, NULL, true);
}

static void recompute_window_offset(WinGuiSeat *wgs)
{
    RECT cr;
    GetClientRect(wgs->term_hwnd, &cr);

    int win_width  = cr.right - cr.left;
    int win_height = cr.bottom - cr.top;

    int new_offset_width = (win_width-wgs->font_width*wgs->term->cols)/2;
    int new_offset_height = (win_height-wgs->font_height*wgs->term->rows)/2;

    if (wgs->offset_width != new_offset_width ||
        wgs->offset_height != new_offset_height) {
        wgs->offset_width = new_offset_width;
        wgs->offset_height = new_offset_height;
        InvalidateRect(wgs->term_hwnd, NULL, true);
    }
}

static void reset_window(WinGuiSeat *wgs, int reinit)
{
    /*
     * This function decides how to resize or redraw when the
     * user changes something.
     *
     * This function doesn't like to change the terminal size but if the
     * font size is locked that may be it's only soluion.
     */
    int win_width, win_height, resize_action, window_border;
    RECT cr, wr;

    /* Current window sizes ... */
    GetWindowRect(wgs->term_hwnd, &wr);
    GetClientRect(wgs->term_hwnd, &cr);

    win_width  = cr.right - cr.left;
    win_height = cr.bottom - cr.top;

    resize_action = conf_get_int(wgs->conf, CONF_resize_action);
    window_border = conf_get_int(wgs->conf, CONF_window_border);

    if (resize_action == RESIZE_DISABLED)
        reinit = 2;

    /* Are we being forced to reload the fonts ? */
    if (reinit>1) {
        deinit_fonts(wgs);
        init_fonts(wgs, 0, 0);
    }

    /* Oh, looks like we're minimised */
    if (win_width == 0 || win_height == 0)
        return;

    /* Is the window out of position ? */
    if (!reinit) {
        recompute_window_offset(wgs);
    }

    if (IsZoomed(wgs->term_hwnd) || KITTY_IS_EMBEDDED(wgs->term_hwnd)) {
        /* We're fullscreen (or embedded as a child via -hwndparent, #554): we
         * must not change the size of the window, so absorb the change into the
         * font size or the terminal itself. When embedded we always reflow the
         * TERMINAL (rows/cols) to the host-fixed window, so a font-size change
         * keeps the chosen font and just changes how much fits -- it never
         * resizes the embedded window (which would break the host's layout).
         */

        wgs->extra_width = wr.right - wr.left - cr.right + cr.left;
        wgs->extra_height = wr.bottom - wr.top - cr.bottom + cr.top;

        if (resize_action != RESIZE_TERM && !KITTY_IS_EMBEDDED(wgs->term_hwnd)) {
            if (wgs->font_width != win_width/wgs->term->cols ||
                wgs->font_height != win_height/wgs->term->rows) {
                int fw = (win_width - 2*window_border) / wgs->term->cols;
                int fh = (win_height - 2*window_border) / wgs->term->rows;
                /* In case that subtraction made the font size go
                 * negative in an edge case, bound it below by 1 */
                if (fw < 1) fw = 1;
                if (fh < 1) fh = 1;
                deinit_fonts(wgs);
                init_fonts(wgs, fw, fh);
                wgs->offset_width =
                    (win_width - wgs->font_width*wgs->term->cols) / 2;
                wgs->offset_height =
                    (win_height - wgs->font_height*wgs->term->rows) / 2;
                InvalidateRect(wgs->term_hwnd, NULL, true);
            }
        } else {
            if (wgs->font_width * wgs->term->cols != win_width ||
                wgs->font_height * wgs->term->rows != win_height) {
                /* Our only choice at this point is to change the
                 * size of the terminal; Oh well.
                 */
                term_size(wgs->term,
                          (win_height - 2*window_border) / wgs->font_height,
                          (win_width - 2*window_border) / wgs->font_width,
                          conf_get_int(wgs->conf, CONF_savelines));
                wgs->offset_width =
                    (win_width - window_border - wgs->font_width*wgs->term->cols) / 2;
                wgs->offset_height =
                    (win_height - window_border - wgs->font_height*wgs->term->rows) / 2;
                InvalidateRect(wgs->term_hwnd, NULL, true);
            }
        }
        return;
    }

    /* Resize window after DPI change */
    if (reinit == 3 && p_GetSystemMetricsForDpi && p_AdjustWindowRectExForDpi) {
        RECT rect;
        rect.left = rect.top = 0;
        rect.right = (wgs->font_width * wgs->term->cols);
        if (conf_get_bool(wgs->conf, CONF_scrollbar))
            rect.right += p_GetSystemMetricsForDpi(SM_CXVSCROLL,
                                                   wgs->dpi_info.cur_dpi.x);
        rect.bottom = (wgs->font_height * wgs->term->rows);
        p_AdjustWindowRectExForDpi(
            &rect, GetWindowLongPtr(wgs->term_hwnd, GWL_STYLE),
            FALSE, GetWindowLongPtr(wgs->term_hwnd, GWL_EXSTYLE),
            wgs->dpi_info.cur_dpi.x);
        rect.right += (window_border * 2);
        rect.bottom += (window_border * 2);
        OffsetRect(&wgs->dpi_info.new_wnd_rect,
                   ((wgs->dpi_info.new_wnd_rect.right -
                     wgs->dpi_info.new_wnd_rect.left) -
                    (rect.right - rect.left)) / 2,
                   ((wgs->dpi_info.new_wnd_rect.bottom -
                     wgs->dpi_info.new_wnd_rect.top) -
                    (rect.bottom - rect.top)) / 2);
        SetWindowPos(wgs->term_hwnd, NULL,
                     wgs->dpi_info.new_wnd_rect.left,
                     wgs->dpi_info.new_wnd_rect.top,
                     rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOZORDER);

        InvalidateRect(wgs->term_hwnd, NULL, true);
        return;
    }

    /* Hmm, a force re-init means we should ignore the current window
     * so we resize to the default font size.
     */
    if (reinit>0) {
        wgs->offset_width = wgs->offset_height = window_border;
        wgs->extra_width =
            wr.right - wr.left - cr.right + cr.left + wgs->offset_width*2;
        wgs->extra_height =
            wr.bottom - wr.top - cr.bottom + cr.top + wgs->offset_height*2;

        if (win_width != (wgs->font_width*wgs->term->cols +
                          wgs->offset_width*2) ||
            win_height != (wgs->font_height*wgs->term->rows +
                           wgs->offset_height*2)) {

            /* If this is too large windows will resize it to the maximum
             * allowed window size, we will then be back in here and resize
             * the font or terminal to fit.
             */
            SetWindowPos(wgs->term_hwnd, NULL, 0, 0,
                         wgs->font_width*wgs->term->cols + wgs->extra_width,
                         wgs->font_height*wgs->term->rows + wgs->extra_height,
                         SWP_NOMOVE | SWP_NOZORDER);
        }

        InvalidateRect(wgs->term_hwnd, NULL, true);
        return;
    }

    /* Okay the user doesn't want us to change the font so we try the
     * window. But that may be too big for the screen which forces us
     * to change the terminal.
     */
    if ((resize_action == RESIZE_TERM && reinit<=0) ||
        (resize_action == RESIZE_EITHER && reinit<0) ||
        reinit>0) {
        wgs->offset_width = wgs->offset_height = window_border;
        wgs->extra_width =
            wr.right - wr.left - cr.right + cr.left + wgs->offset_width*2;
        wgs->extra_height =
            wr.bottom - wr.top - cr.bottom + cr.top + wgs->offset_height*2;

        if (win_width != (wgs->font_width*wgs->term->cols +
                          wgs->offset_width*2) ||
            win_height != (wgs->font_height*wgs->term->rows +
                           wgs->offset_height*2)) {

            RECT ss;
            int width, height;

            get_fullscreen_rect(wgs, &ss);

            width = (ss.right - ss.left - wgs->extra_width) / wgs->font_width;
            height = (ss.bottom - ss.top - wgs->extra_height)/wgs->font_height;

            /* Grrr too big */
            if ( wgs->term->rows > height || wgs->term->cols > width ) {
                if (resize_action == RESIZE_EITHER) {
                    /* Make the font the biggest we can */
                    if (wgs->term->cols > width)
                        wgs->font_width =
                            (ss.right - ss.left - wgs->extra_width) /
                            wgs->term->cols;
                    if (wgs->term->rows > height)
                        wgs->font_height =
                            (ss.bottom - ss.top - wgs->extra_height) /
                            wgs->term->rows;

                    deinit_fonts(wgs);
                    init_fonts(wgs, wgs->font_width, wgs->font_height);

                    width = (ss.right - ss.left - wgs->extra_width) /
                        wgs->font_width;
                    height = (ss.bottom - ss.top - wgs->extra_height) /
                        wgs->font_height;
                } else {
                    if ( height > wgs->term->rows ) height = wgs->term->rows;
                    if ( width > wgs->term->cols )  width = wgs->term->cols;
                    term_size(wgs->term, height, width,
                              conf_get_int(wgs->conf, CONF_savelines));
                }
            }

            SetWindowPos(wgs->term_hwnd, NULL, 0, 0,
                         wgs->font_width*wgs->term->cols + wgs->extra_width,
                         wgs->font_height*wgs->term->rows + wgs->extra_height,
                         SWP_NOMOVE | SWP_NOZORDER);

            InvalidateRect(wgs->term_hwnd, NULL, true);
        }
        return;
    }

    /* We're allowed to or must change the font but do we want to ?  */

    if (wgs->font_width != (win_width-window_border*2)/wgs->term->cols ||
        wgs->font_height != (win_height-window_border*2)/wgs->term->rows) {

        deinit_fonts(wgs);
        init_fonts(wgs, (win_width-window_border*2)/wgs->term->cols,
                   (win_height-window_border*2)/wgs->term->rows);
        wgs->offset_width = (win_width-wgs->font_width*wgs->term->cols)/2;
        wgs->offset_height = (win_height-wgs->font_height*wgs->term->rows)/2;

        wgs->extra_width =
            wr.right - wr.left - cr.right + cr.left + wgs->offset_width*2;
        wgs->extra_height =
            wr.bottom - wr.top - cr.bottom + cr.top + wgs->offset_height*2;

        InvalidateRect(wgs->term_hwnd, NULL, true);
    }
}

static void set_input_locale(WinGuiSeat *wgs, HKL kl)
{
    char lbuf[20];

    GetLocaleInfo(LOWORD(kl), LOCALE_IDEFAULTANSICODEPAGE,
                  lbuf, sizeof(lbuf));

    wgs->kbd_codepage = atoi(lbuf);
}

static void click(WinGuiSeat *wgs, Mouse_Button b, int x, int y,
                  bool shift, bool ctrl, bool alt)
{
    int thistime = GetMessageTime();

    if (wgs->send_raw_mouse &&
        !(shift && conf_get_bool(wgs->conf, CONF_mouse_override))) {
        wgs->lastbtn = MBT_NOTHING;
        term_mouse(wgs->term, b, translate_button(wgs, b), MA_CLICK,
                   x, y, shift, ctrl, alt);
        return;
    }

    if (wgs->lastbtn == b && thistime - wgs->lasttime < wgs->dbltime) {
        wgs->lastact = (wgs->lastact == MA_CLICK ? MA_2CLK :
                   wgs->lastact == MA_2CLK ? MA_3CLK :
                   wgs->lastact == MA_3CLK ? MA_CLICK : MA_NOTHING);
    } else {
        wgs->lastbtn = b;
        wgs->lastact = MA_CLICK;
    }
    if (wgs->lastact != MA_NOTHING)
        term_mouse(wgs->term, b, translate_button(wgs, b), wgs->lastact,
                   x, y, shift, ctrl, alt);
    wgs->lasttime = thistime;
}

/*
 * Translate a raw mouse button designation (LEFT, MIDDLE, RIGHT)
 * into a cooked one (SELECT, EXTEND, PASTE).
 */
static Mouse_Button translate_button(WinGuiSeat *wgs, Mouse_Button button)
{
    if (button == MBT_LEFT)
        return MBT_SELECT;
    if (button == MBT_MIDDLE)
        return conf_get_int(wgs->conf, CONF_mouse_is_xterm) == MOUSE_XTERM ?
            MBT_PASTE : MBT_EXTEND;
    if (button == MBT_RIGHT)
        return conf_get_int(wgs->conf, CONF_mouse_is_xterm) == MOUSE_XTERM ?
            MBT_EXTEND : MBT_PASTE;
    return 0;                          /* shouldn't happen */
}

static void show_mouseptr(WinGuiSeat *wgs, bool show)
{
    /* NB that the counter in ShowCursor() is also frobbed by
     * update_mouse_pointer() */
    static bool cursor_visible = true;
    if (wgs) {
        if (!conf_get_bool(wgs->conf, CONF_hide_mouseptr))
            show = true; /* hiding mouse pointer disabled in Conf */
    } else {
        /*
         * You can pass wgs==NULL if you want to _show_ the pointer
         * rather than hiding it, because that's never disallowed.
         */
        assert(show);
    }
    if (cursor_visible && !show)
        ShowCursor(false);
    else if (!cursor_visible && show)
        ShowCursor(true);
    cursor_visible = show;
}

static bool is_alt_pressed(void)
{
    BYTE keystate[256];
    int r = GetKeyboardState(keystate);
    if (!r)
        return false;
    if (keystate[VK_MENU] & 0x80)
        return true;
    if (keystate[VK_RMENU] & 0x80)
        return true;
    return false;
}

static void exit_callback(void *vctx)
{
    WinGuiSeat *wgs = (WinGuiSeat *)vctx;
    int exitcode, close_on_exit;

    if (!wgs->session_closed &&
        (exitcode = backend_exitcode(wgs->backend)) >= 0) {
        close_on_exit = conf_get_int(wgs->conf, CONF_close_on_exit);
        /* Abnormal exits will already have set session_closed and taken
         * appropriate action. */
        bool coe_close = (close_on_exit == FORCE_ON ||
                          (close_on_exit == AUTO && exitcode != INT_MAX));
#ifdef MOD_PERSO
        /* KiTTY: not while the Event Log is open - see kitty_eventlog_is_open().
         * Say so in the log itself, which is the window the user is reading. */
        if (coe_close && kitty_eventlog_is_open()) {
            coe_close = false;
            kitty_coe_pending_exit = 0;   /* close once the log is dismissed */
            logevent(wgs->logctx, "Session ended; window kept open while the "
                     "Event Log is open (it closes when you close the log)");
        }
#endif
        if (coe_close) {
#ifdef MOD_PERSO
            /* KiTTY: the session ended (e.g. Ctrl+D / remote logout) and we're
             * about to close. This path uses PostQuitMessage, which does NOT
             * generate WM_DESTROY, so the position must be saved here too -- else
             * "remember window position" never records a window closed this way. */
            kitty_on_window_closing(wgs, wgs->term_hwnd);
#endif
            PostQuitMessage(0);
        } else {
            queue_toplevel_callback(close_session, wgs);
            wgs->session_closed = true;
            /* exitcode == INT_MAX indicates that the connection was closed
             * by a fatal error, so an error box will be coming our way and
             * we should not generate this informational one. */
            if (exitcode != INT_MAX) {
                show_mouseptr(wgs, true);
#ifdef MOD_PERSO
                /* KiTTY (upstream cyd01/KiTTY #548): print the informational close INLINE instead of a
                 * modal box that traps the window (which stays open in this
                 * branch). PuTTY-compat mode keeps the classic box. */
                if (!GetPuttyFlag() && !GetModalErrorsFlag() && wgs->term) {
                    char *line = dupprintf(
                        "\r\n\x1b[1;33m%s:\x1b[0m Connection closed by remote host\r\n",
                        appname);
                    term_data(wgs->term, line, strlen(line));
                    sfree(line);
                } else
#endif
                MessageBox(wgs->term_hwnd, "Connection closed by remote host",
                           appname, MB_OK | MB_ICONINFORMATION);
            }
        }
    }
}

static void win_seat_notify_remote_exit(Seat *seat)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    queue_toplevel_callback(exit_callback, wgs);
}

static void conf_cache_data(WinGuiSeat *wgs)
{
    /* Cache some items from conf to speed lookups in very hot code */
    wgs->cursor_type = conf_get_int(wgs->conf, CONF_cursor_type);
    wgs->vtmode = conf_get_int(wgs->conf, CONF_vtmode);
}

static const int clips_system[] = { CLIP_SYSTEM };

static HDC make_hdc(WinGuiSeat *wgs)
{
    HDC hdc;

    if (!wgs->term_hwnd)
        return NULL;

    hdc = GetDC(wgs->term_hwnd);
    if (!hdc)
        return NULL;

    SelectPalette(hdc, wgs->pal, false);
    return hdc;
}

static void free_hdc(WinGuiSeat *wgs, HDC hdc)
{
    assert(wgs->term_hwnd);
    SelectPalette(hdc, GetStockObject(DEFAULT_PALETTE), false);
    ReleaseDC(wgs->term_hwnd, hdc);
}

static void wm_size_resize_term(WinGuiSeat *wgs, LPARAM lParam)
{
    int width = LOWORD(lParam);
    int height = HIWORD(lParam);
    int border_size = conf_get_int(wgs->conf, CONF_window_border);

    int w = (width - border_size*2) / wgs->font_width;
    int h = (height - border_size*2) / wgs->font_height;

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (wgs->resizing) {
        /*
         * If we're in the middle of an interactive resize, we don't
         * call term_size. This means that, firstly, the user can drag
         * the size back and forth indecisively without wiping out any
         * actual terminal contents, and secondly, the Terminal
         * doesn't call back->size in turn for each increment of the
         * resizing drag, so we don't spam the server with huge
         * numbers of resize events.
         */
        wgs->need_backend_resize = true;
    } else {
        term_size(wgs->term, h, w,
                  conf_get_int(wgs->conf, CONF_savelines));
    }
    conf_set_int(wgs->conf, CONF_height, h);
    conf_set_int(wgs->conf, CONF_width, w);
}

#ifdef MOD_PERSO
/* KiTTY Ctrl-Tab session switching: find the next/prev KiTTY window by the
 * per-window creation timestamp stored in the 8 extra window-class bytes. */
struct ctrl_tab_info {
    int direction;
    HWND  self;
    DWORD self_hi_date_time;
    DWORD self_lo_date_time;
    HWND  next;
    DWORD next_hi_date_time;
    DWORD next_lo_date_time;
    int   next_self;
};
static BOOL CALLBACK CtrlTabWindowProc(HWND hwnd, LPARAM lParam) {
    struct ctrl_tab_info* info = (struct ctrl_tab_info*) lParam;
    char class_name[16];
    int wndExtra;
    if (info->self != hwnd
        && (wndExtra = GetClassLong(hwnd, GCL_CBWNDEXTRA)) >= 8
        && GetClassName(hwnd, class_name, sizeof class_name) >= 5
        && memcmp(class_name, KiTTYClassName, 5) == 0) {
        DWORD hwnd_hi_date_time = GetWindowLong(hwnd, wndExtra - 8);
        DWORD hwnd_lo_date_time = GetWindowLong(hwnd, wndExtra - 4);
        int hwnd_self, hwnd_next;
        hwnd_self = hwnd_hi_date_time - info->self_hi_date_time;
        if (hwnd_self == 0) hwnd_self = hwnd_lo_date_time - info->self_lo_date_time;
        hwnd_self *= info->direction;
        hwnd_next = hwnd_hi_date_time - info->next_hi_date_time;
        if (hwnd_next == 0) hwnd_next = hwnd_lo_date_time - info->next_lo_date_time;
        hwnd_next *= info->direction;
        if (((hwnd_self > 0) && (hwnd_next < 0))
            || (((hwnd_self > 0) || (hwnd_next < 0)) && (info->next_self <= 0))) {
            info->next = hwnd;
            info->next_hi_date_time = hwnd_hi_date_time;
            info->next_lo_date_time = hwnd_lo_date_time;
            info->next_self = hwnd_self;
        }
    }
    return TRUE;
}
#endif

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message,
                                WPARAM wParam, LPARAM lParam)
{
    HDC hdc;
    int resize_action;
    WinGuiSeat *wgs = (WinGuiSeat *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (message) {
      case WM_CREATE:
        break;
#ifdef MOD_PERSO
      case WM_DROPFILES:
        /* KiTTY: a file was dropped on the terminal -> pscp upload. */
        OnDropFiles(hwnd, (HDROP)wParam);
        return 0;
      case MYWM_NOTIFYICON:
        /* systray icon clicked -> restore the window sent to the tray */
        if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP ||
            lParam == WM_LBUTTONDBLCLK)
            RestoreFromTray(hwnd);
        return 0;
      case WM_TIMER:
#ifdef MOD_PERSO
        if ((UINT_PTR)wParam == TIMER_CLIPACTIVITY) {
            /* The clipboard activity marker has run its few seconds. Take it
             * down: refreshing the title re-reads term_clipboard_activity(),
             * which has now lapsed, so both the icon and the tint go with it. */
            KillTimer(hwnd, TIMER_CLIPACTIVITY);
            kitty_refresh_title();
            if (wgs && wgs->term)
                kitty_osc52_state_changed(wgs->term);
            return 0;
        }
        if ((UINT_PTR)wParam == TIMER_EMBEDFILL) {
            /* #554: keep the embedded child filling the host's client area. */
            if (KITTY_EMBEDDED() && IsWindow(kitty_hwnd_parent)) {
                RECT prc, wr;
                if (GetClientRect(kitty_hwnd_parent, &prc) &&
                    prc.right > 0 && prc.bottom > 0 &&
                    GetWindowRect(hwnd, &wr)) {
                    /* our current size (child origin is 0,0 in the parent) */
                    int cw = wr.right - wr.left, ch = wr.bottom - wr.top;
                    if (cw != prc.right || ch != prc.bottom)
                        MoveWindow(hwnd, 0, 0, prc.right, prc.bottom, TRUE);
                }
            } else {
                KillTimer(hwnd, TIMER_EMBEDFILL);
            }
            return 0;
        }
        if ((UINT_PTR)wParam == TIMER_SENDTOTRAY) {
            /* Repeating tick: hide to the tray as soon as the session is up.
             * ever_authenticated is set post-auth for SSH and on connect for
             * every other backend, so we never hide a window that is still
             * showing a host-key/password prompt or an error. If the session
             * never gets there, the timer just keeps ticking and the window
             * stays where the user can see it. AutoSendToTray is deliberately
             * left set: from now on a manual minimise goes to the tray too,
             * which is the rest of what this option means. */
            if (wgs && wgs->backend && wgs->ever_authenticated) {
                KillTimer(hwnd, TIMER_SENDTOTRAY);
                kitty_send_to_tray(hwnd);
            }
            return 0;
        }
#endif
        if ((UINT_PTR)wParam == TIMER_AUTOCOMMAND) {
            KillTimer(hwnd, TIMER_AUTOCOMMAND);
            if (kitty_autocommand_tick(hwnd))
                SetTimer(hwnd, TIMER_AUTOCOMMAND,
                         autocommand_delay > 0 ? autocommand_delay : 5, NULL);
            return 0;
        }
        if ((UINT_PTR)wParam == TIMER_ANTIIDLE) {
            /* Repeating, and left armed: the period already IS the configured
             * interval, so every tick sends. No counter (see where it is set). */
            kitty_antiidle_tick(hwnd);
            return 0;
        }
        if ((UINT_PTR)wParam == TIMER_SCRIPT) {
            KillTimer(hwnd, TIMER_SCRIPT);
            /*
             * KiTTY has two scripting engines and they must not run at once.
             * Both watch the same incoming data in win_seat_output and both
             * send, with nothing between them, so each would end up reacting to
             * output the other caused.
             *
             * They are SEQUENCED rather than one being switched off, because the
             * two do different jobs and the order is obvious: the login script
             * answers the prompts that get you in, and the rutty script sends a
             * file once you are in. So rutty waits while the login script still
             * has entries left, and starts when it has run out.
             *
             * Bounded, because a login script whose prompt never arrives would
             * otherwise hold rutty off for ever. After the cap, rutty starts
             * anyway and says so - a script that was going to be sent should not
             * be silently dropped because another one is stuck.
             */
            if (ScriptFileContent != NULL && ScriptFileContent[0]) {
                if (++wgs->script_defer_ticks <= 40) {          /* ~60s */
                    if (wgs->script_defer_ticks == 1)
                        lp_eventlog(&wgs->logpolicy,
                            "Rutty script (Session > Scripting) is waiting for "
                            "the login script (Connection > Data) to finish");
                    SetTimer(hwnd, TIMER_SCRIPT, 1500, NULL);
                    return 0;
                }
                lp_eventlog(&wgs->logpolicy,
                    "Login script (Connection > Data) has not finished; starting "
                    "the rutty script (Session > Scripting) anyway - if the "
                    "automation misbehaves, that is why");
            }
            /* This tick takes over the start (normally the exhaustion hook in
             * win_seat_output gets there first and this timer is disarmed);
             * either way the hook must not fire again afterwards. */
            wgs->script_deferred = false;
            if (wgs->backend && kitty_script_enabled()) {
                Filename *sf = conf_get_filename(wgs->conf, CONF_scriptfile);
                kitty_script_send_file(wgs->conf, wgs->backend, sf);
            }
            return 0;
        }
#ifdef MOD_BACKGROUNDIMAGE
        if ((UINT_PTR)wParam == TIMER_SLIDEBG_WIN) {
            /* periodic: advance the slideshow image and repaint (no KillTimer). */
            if (GetBackgroundImageFlag()) {
                NextBgImage(hwnd);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
#endif
        if ((UINT_PTR)wParam == TIMER_LOGROTATION) {
            /* KiTTY: time to start a new log file. logfile_rotate() only
             * closes the current one - the next write reopens it under a
             * freshly substituted name - and declines if that name would be
             * the same, which would truncate the log instead of rotating it. */
            if (wgs && wgs->logctx)
                logfile_rotate(wgs->logctx);
            return 0;
        }
#ifdef MOD_RECONNECT
        if ((UINT_PTR)wParam == TIMER_RECONNECT) {
            KillTimer(hwnd, TIMER_RECONNECT);
            if (wgs && !wgs->backend) {
                lp_eventlog(&wgs->logpolicy,
                            "No backend connection, reconnecting...");
                PostMessage(hwnd, WM_COMMAND, IDM_RESTART, 0);
            }
            return 0;
        }
#endif
        break;
#endif
#ifdef MOD_RECONNECT
      case WM_POWERBROADCAST:
        if (wgs && GetAutoreconnectFlag()
            && conf_get_int(wgs->conf, CONF_wakeup_reconnect)
            && wgs->ever_authenticated) {
            switch (wParam) {
              case PBT_APMRESUMESUSPEND:
              case PBT_APMRESUMEAUTOMATIC:
              case PBT_APMRESUMECRITICAL:
              case PBT_APMQUERYSUSPENDFAILED:
                if (wgs->session_closed && !wgs->backend) {
                    lp_eventlog(&wgs->logpolicy,
                                "Woken up from suspend, trying to reconnect...");
                    SetTimer(wgs->term_hwnd, TIMER_RECONNECT,
                             GetReconnectDelay()*1000, NULL);
                }
                break;
              case PBT_APMSUSPEND:
                if (!wgs->session_closed && wgs->backend) {
                    lp_eventlog(&wgs->logpolicy,
                                "Suspend detected, disconnecting cleanly...");
                    wgs->session_closed = true;
                    queue_toplevel_callback(close_session, wgs);
                }
                break;
            }
        }
        break;
#endif
#ifdef MOD_PERSO
      case WM_QUERYENDSESSION:
        /* KiTTY: consent to a Windows logoff/shutdown or an MSI Restart
         * Manager shutdown. (DefWindowProc already returns TRUE here, but be
         * explicit - the real teardown happens in WM_ENDSESSION.) */
        return true;
      case WM_ENDSESSION:
        if (wParam) {
            /* The session really is ending (logoff / shutdown / an in-place
             * MSI upgrade driven by the Restart Manager). Tear THIS window
             * down promptly and unconditionally - deliberately NOT through the
             * WM_CLOSE "Are you sure you want to close this session?" prompt,
             * which would block the end-session until Windows/RM times out and
             * FORCE-terminates us. Exiting cleanly here (WM_DESTROY saves the
             * window placement, then PostQuitMessage) is what lets the Restart
             * Manager bring a registered session back after the upgrade. */
            DestroyWindow(hwnd);
        }
        return 0;
#endif
      case WM_CLOSE: {
        char *title, *msg, *additional = NULL;
#ifdef MOD_PERSO
        /*
         * KiTTY: this session may not be closed by the user.
         *
         * Greying SC_CLOSE takes care of the X and Alt+F4, but NOT of the
         * taskbar's own "Close window", which posts WM_CLOSE straight here and
         * would otherwise walk past a disabled menu item as though it were not
         * there. This is the point of the setting for an embedded or kiosk
         * KiTTY, so it has to hold from every direction.
         *
         * Only WM_CLOSE. A Windows shutdown or logoff arrives as
         * WM_QUERYENDSESSION and is deliberately left alone - refusing to close
         * for a user is a feature, refusing to close for the operating system is
         * a hung machine at shutdown. A session that ends on its own still ends:
         * this blocks the request to close, not the closing.
         */
        if (!conf_get_bool(wgs->conf, CONF_window_closable) &&
            !wgs->session_closed) {
            MessageBeep(MB_ICONWARNING);
            return 0;
        }
#endif
        show_mouseptr(wgs, true);
        title = dupprintf("%s Exit Confirmation", appname);
        if (wgs->backend && wgs->backend->vt->close_warn_text) {
            additional = wgs->backend->vt->close_warn_text(wgs->backend);
        }
        msg = dupprintf("Are you sure you want to close this session?%s%s",
                        additional ? "\n" : "",
                        additional ? additional : "");
        if (wgs->session_closed ||
            !conf_get_bool(wgs->conf, CONF_warn_on_close) ||
            MessageBox(hwnd, msg, title,
                       MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON1)
            == IDOK)
            DestroyWindow(hwnd);
        sfree(title);
        sfree(msg);
        sfree(additional);
        return 0;
      }
#ifdef MOD_PERSO
      case WM_COPYDATA: {
        /*
         * KiTTY: BROADCAST TO ALL WINDOWS - the receiving half.
         *
         * SendCommandAllWindows() (kitty.c) posts WM_COPYDATA with dwData==1 to
         * every window of KiTTY's class. It is reached two ways, both offered to
         * the user today:
         *     /command <text>          - "run a command or send text in ALL windows"
         *     kitty.exe -sendcmd <text>
         * The receiver was lost in the 0.85 port, so both silently did nothing:
         * the sender exited 0, DefWindowProc dropped the message, and no window
         * ever saw it. 0.76 had this case (KiTTY/0.76b_My_PuTTY/windows/window.c).
         *
         * It TYPES the text into the session (SendKeyboardPlus), exactly as 0.76
         * did - it does NOT run internal commands. Worth keeping that way:
         * routing this through InternalCommand would let any process on the
         * machine drive /delreg and friends in every open window, which is a far
         * larger thing than typing into a terminal.
         *
         * 0.76 put this case and rutty's AHK ids in an #ifdef either/or, so a
         * build with rutty silently lost the broadcast. Both are dispatched from
         * the one switch on dwData here.
         */
        PCOPYDATASTRUCT cds = (PCOPYDATASTRUCT)lParam;
        if (!cds || cds->cbData == 0 || cds->lpData == NULL)
            return 0;

        if (cds->dwData == 1) {
            /* The pre-group format: bare text, no way to tell which install
             * sent it. Refused rather than obeyed, and SAID so - this whole
             * feature spent two releases broken precisely because it failed
             * silently. */
            logevent(wgs->logctx, "broadcast refused: sender did not identify "
                     "its group (pre-0.85 format)");
            return 0;
        }

        if (cds->dwData == 2) {
            /* "<group>\0<text>", neither part assumed NUL-terminated: it comes
             * from another process, so it is copied into a bounded buffer of
             * our own before anything looks at it. */
            size_t n = cds->cbData;
            char *buf, *text;
            const char *mygroup;
            if (n > 65536) n = 65536;
            buf = snewn(n + 2, char);
            memcpy(buf, cds->lpData, n);
            buf[n] = '\0';
            buf[n + 1] = '\0';              /* so the text half is terminated */
            text = buf + strlen(buf) + 1;
            if (text > buf + n) text = buf + n;   /* no NUL found: empty text */

            /* MASTER first: [KiTTY] sendcmdmode=no means the feature is
             * inert on this install, whatever a session says. */
            if (!kitty_broadcast_default()) {
                logevent(wgs->logctx, "broadcast refused: disabled on this "
                         "install ([KiTTY] sendcmdmode=no in kitty.ini)");
                sfree(buf);
                return 0;
            }
            /* The key this SESSION listens for: its own if set, else the
             * install's. A session with its own key answers only to that one. */
            mygroup = conf_get_str(wgs->conf, CONF_kitty_broadcast_key);
            if (!mygroup || !*mygroup) mygroup = kitty_broadcast_group();
            if (strcmp(buf, mygroup) != 0) {
                /* Another KiTTY install - the portable copy on a stick talking
                 * to the laptop's windows, typically. Not ours to obey. */
                char *m = dupprintf("broadcast refused: from another KiTTY "
                                    "install (group '%s', ours is '%s')",
                                    buf, mygroup);
                logevent(wgs->logctx, m);
                sfree(m);
                sfree(buf);
                return 0;
            }
            if (!kitty_broadcast_armed(wgs)) {
                logevent(wgs->logctx, "broadcast refused: this session does not "
                         "accept broadcasts (Session > Scripting, or the "
                         "Tools > Accept broadcast toggle)");
                sfree(buf);
                return 0;
            }
            {
                char *m = dupprintf("broadcast accepted (%d bytes), typing it "
                                    "into this session", (int)strlen(text));
                logevent(wgs->logctx, m);
                sfree(m);
            }
            SendKeyboardPlus(hwnd, text);
            sfree(buf);
            return 1;
        }
        return 0;                      /* not ours - let the default happen */
      }
#endif
      case WM_DESTROY:
#ifdef MOD_PERSO
        /* KiTTY: remember this window's position (topology-keyed) for next time. */
        kitty_on_window_closing(wgs, hwnd);
#endif
        show_mouseptr(wgs, true);
        PostQuitMessage(0);
        return 0;
      case WM_INITMENUPOPUP:
#ifdef MOD_PERSO
        /* Change Settings can turn a session's transparency to or from -1 while
         * the window is open; the menu was built once, so re-check it here. */
        kitty_sync_transparency_menu((HMENU)wParam, wgs->conf, IDM_TRANSPARUP,
                                     IDM_TRANSPARDOWN, IDM_FONTUP);
        {
            /* KiTTY: the two log items. Both are dead without a log, and the
             * second one does two different jobs: with a time in the file name
             * a close-and-reopen starts a NEW file (rotate), with a fixed name
             * it can only truncate the one we have (clear). Say which. */
            HMENU mp = (HMENU)wParam;
            bool haslog = (wgs && wgs->logctx &&
                           logfile_current_name(wgs->logctx) != NULL);
            EnableMenuItem(mp, IDM_OPENLOGFILE, MF_BYCOMMAND |
                           (haslog ? MF_ENABLED : (MF_DISABLED | MF_GRAYED)));
            EnableMenuItem(mp, IDM_CLEARLOGFILE, MF_BYCOMMAND |
                           (haslog ? MF_ENABLED : (MF_DISABLED | MF_GRAYED)));
            if (haslog)
                ModifyMenu(mp, IDM_CLEARLOGFILE, MF_BYCOMMAND | MF_STRING,
                           IDM_CLEARLOGFILE,
                           logfile_name_varies(wgs->logctx) ?
                           "Start a new log file &now" : "Clear log fil&e");
        }
#ifdef MOD_ZMODEM
        if (GetZModemFlag()) {
            /*
             * The ZModem entries are rebuilt every time this menu opens, and a
             * helper that is not configured gets no entry at all. Change
             * Settings can set or clear a path while the window is open, so the
             * decision cannot be made when the menu is built.
             *
             * ⚠️ DELETE FIRST, THEN COMPUTE POSITIONS. MF_BYCOMMAND deletion is
             * position-independent, but the insertion below is BY POSITION, and
             * a position taken before the deletions would be off by however
             * many rows the deletions removed.
             */
            HMENU mp = (HMENU)wParam;
            bool xfer = kitty_zmodem_active();
            bool has_rz = *filename_to_str(
                conf_get_filename(wgs->conf, CONF_rzcommand)) != '\0';
            bool has_sz = *filename_to_str(
                conf_get_filename(wgs->conf, CONF_szcommand)) != '\0';
            int i, count, at = -1;

            DeleteMenu(mp, IDM_XYZSTART,  MF_BYCOMMAND);
            DeleteMenu(mp, IDM_XYZUPLOAD, MF_BYCOMMAND);
            DeleteMenu(mp, IDM_XYZABORT,  MF_BYCOMMAND);

            /* Where the block belongs: directly before the separator that
             * precedes "Print clipboard". Anchoring on a command rather than a
             * fixed index keeps this correct as the menu above it changes -
             * the script entries, for one, are conditional too. */
            count = GetMenuItemCount(mp);
            for (i = 0; i < count; i++) {
                if (GetMenuItemID(mp, i) == IDM_PRINT) {
                    at = (i > 0) ? i - 1 : 0;   /* before that separator */
                    break;
                }
            }
            if (at >= 0 && (has_rz || has_sz)) {
                if (has_rz)
                    InsertMenu(mp, at++, MF_BYPOSITION | MF_STRING |
                               (xfer ? (MF_DISABLED | MF_GRAYED) : MF_ENABLED),
                               IDM_XYZSTART, "&ZModem Receive");
                if (has_sz)
                    InsertMenu(mp, at++, MF_BYPOSITION | MF_STRING |
                               (xfer ? (MF_DISABLED | MF_GRAYED) : MF_ENABLED),
                               IDM_XYZUPLOAD, "ZModem &Upload");
                /* Abort belongs with them, and is live only during a transfer. */
                InsertMenu(mp, at++, MF_BYPOSITION | MF_STRING |
                           (xfer ? MF_ENABLED : (MF_DISABLED | MF_GRAYED)),
                           IDM_XYZABORT, "ZModem &Abort");
            }
        }
#endif
#endif
        if ((HMENU)wParam == wgs->savedsess_menu) {
            /* About to pop up Saved Sessions sub-menu.
             * Refresh the session list. */
            get_sesslist(&sesslist, false); /* free */
            get_sesslist(&sesslist, true);
            update_savedsess_menu(wgs);
            return 0;
        }
        break;
#ifdef MOD_PERSO
      case WM_NCLBUTTONDBLCLK:
        /* KiTTY: double-click the title bar to roll up the window (when winrol
         * enabled); otherwise fall through to the normal maximise toggle. */
        if (wParam == HTCAPTION && GetWinrolFlag()) {
            kitty_rollup(wgs->term_hwnd,
                         conf_get_int(wgs->conf, CONF_resize_action));
            return 0;
        }
        break;
#endif
#ifdef MOD_PERSO
      case WM_KITTY_CLIPBALLOON:
        /*
         * KiTTY: the transient clipboard tray balloon was clicked. Open the Event
         * Log, because that is where the detail is.
         *
         * The balloon is rate-limited on purpose - every one of these events fires
         * at a moment the remote host chose - so it can only ever say THAT
         * something was dropped, never how many times or which. Saying so and then
         * making the log one click away is the difference between a notification
         * and an answer.
         *
         * lParam carries the mouse/balloon event; NIN_BALLOONUSERCLICK is the
         * balloon body being clicked, and a plain left-click on the icon in the
         * few seconds it exists means the same thing here.
         */
        if (lParam == NIN_BALLOONUSERCLICK || lParam == WM_LBUTTONUP) {
            SetForegroundWindow(hwnd);
            if (kitty_clipboard_balloon_action() == CLIP_BALLOON_BLOCK_WRITES) {
                /*
                 * A server is repeatedly overwriting the clipboard. Being told
                 * that is no use without a way to stop it, and hunting through
                 * the configuration box while it is still happening is not a way
                 * to stop it - so the click IS the fix, behind one confirmation.
                 */
                if (MessageBox(hwnd,
                               "A server has been changing your clipboard "
                               "repeatedly.\n\n"
                               "Stop it changing your clipboard at all for the "
                               "rest of this session?\n\n"
                               "You can turn it back on under "
                               "Window > Selection, \"Remote clipboard writes\".",
                               "KiTTY - block this server's clipboard writes?",
                               MB_YESNO | MB_ICONWARNING) == IDYES) {
                    if (wgs->term)
                        wgs->term->osc52_allowed = OSC52_CLIPBOARD_DENY;
                    /* the live Conf too, so Change Settings shows the truth and
                     * far2l's own policy is not left saying something else */
                    conf_set_int(wgs->conf, CONF_osc52_clipboard,
                                 OSC52_CLIPBOARD_DENY);
                    if (wgs->term)
                        wgs->term->clip_allowed = 0;
                    conf_set_int(wgs->conf, CONF_shared_clipboard,
                                 SHARED_CLIPBOARD_DISABLED);
                    logevent(wgs->logctx, "Remote clipboard writes blocked for "
                             "this session at the user's request");
                } else {
                    showeventlog(hwnd);
                }
            } else {
                showeventlog(hwnd);
            }
        }
        return 0;
#endif
      case WM_COMMAND:
      case WM_SYSCOMMAND:
        switch (wParam & ~0xF) {       /* low 4 bits reserved to Windows */
          case SC_VSCROLL:
          case SC_HSCROLL:
            if (message == WM_SYSCOMMAND) {
                /* As per the long comment in WM_VSCROLL handler: give
                 * this message the default handling, which starts a
                 * subsidiary message loop, but set a flag so that
                 * when we're re-entered from that loop, scroll events
                 * within an interactive scrollbar-drag can be handled
                 * differently. */
                wgs->in_scrollbar_loop = true;
                LRESULT result = sw_DefWindowProc(
                    hwnd, message, wParam, lParam);
                wgs->in_scrollbar_loop = false;
                return result;
            }
            break;
          case IDM_SHOWLOG:
            showeventlog(hwnd);
            break;
          case IDM_NEWSESS:
          case IDM_DUPSESS:
          case IDM_SAVEDSESS: {
            char b[2048];
            char *cl;
            const char *argprefix;
            bool inherit_handles;
            STARTUPINFO si;
            PROCESS_INFORMATION pi;
            HANDLE filemap = NULL;
            /* KiTTY: share the unlocked master password with the spawned child so
             * it doesn't re-prompt (launcher-mpw-sharing). mpwtok is prepended to
             * the child command line; mpwmap is inheritable and closed after the
             * spawn. Empty/NULL when nothing is unlocked or not in portable mode.
             * Declared unconditionally so the dupprintf below compiles in the
             * stock (non-MOD_PERSO) variants too. */
            HANDLE mpwmap = NULL; char mpwtok[64] = "";
#ifdef MOD_PERSO
            { extern HANDLE kitty_mpw_export_inherit_blob(const char*, char*, size_t);
              mpwmap = kitty_mpw_export_inherit_blob("&K", mpwtok, sizeof(mpwtok)); }
#endif

            if (restricted_acl())
                argprefix = "&R";
            else
                argprefix = "";

            if (wParam == IDM_DUPSESS) {
                /*
                 * Allocate a file-mapping memory chunk for the
                 * config structure.
                 */
                SECURITY_ATTRIBUTES sa;
                strbuf *serbuf;
                void *p;
                int size;

                serbuf = strbuf_new();
                conf_serialise(BinarySink_UPCAST(serbuf), wgs->conf);
                size = serbuf->len;

                sa.nLength = sizeof(sa);
                sa.lpSecurityDescriptor = NULL;
                sa.bInheritHandle = true;
                filemap = CreateFileMapping(INVALID_HANDLE_VALUE,
                                            &sa,
                                            PAGE_READWRITE,
                                            0, size, NULL);
                if (filemap && filemap != INVALID_HANDLE_VALUE) {
                    p = MapViewOfFile(filemap, FILE_MAP_WRITE, 0, 0, size);
                    if (p) {
                        memcpy(p, serbuf->s, size);
                        UnmapViewOfFile(p);
                    }
                }

                strbuf_free(serbuf);
                inherit_handles = true;
                cl = dupprintf("putty %s%s&%p:%u", argprefix, mpwtok,
                               filemap, (unsigned)size);
            } else if (wParam == IDM_SAVEDSESS) {
                unsigned int sessno = ((lParam - IDM_SAVED_MIN)
                                       / MENU_SAVED_STEP) + 1;
                if (sessno < (unsigned)sesslist.nsessions) {
                    const char *session = sesslist.sessions[sessno];
                    cl = dupprintf("putty %s%s@%s", argprefix, mpwtok, session);
                    inherit_handles = (mpwtok[0] != '\0');
                } else
                    break;
            } else /* IDM_NEWSESS */ {
                cl = dupprintf("putty%s%s%s",
                               (*argprefix || *mpwtok) ? " " : "",
                               argprefix, mpwtok);
                inherit_handles = (mpwtok[0] != '\0');
            }

            GetModuleFileName(NULL, b, sizeof(b) - 1);
            si.cb = sizeof(si);
            si.lpReserved = NULL;
            si.lpDesktop = NULL;
            si.lpTitle = NULL;
            si.dwFlags = 0;
            si.cbReserved2 = 0;
            si.lpReserved2 = NULL;
            if (CreateProcess(b, cl, NULL, NULL, inherit_handles,
                          NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
#ifdef MOD_PERSO
                /* Hand the foreground right to the spawned window. Without this
                 * the Windows foreground lock keeps focus on the parent window,
                 * so a duplicated/new session opens behind and unfocused. */
                AllowSetForegroundWindow(pi.dwProcessId);
#endif
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }

            if (filemap)
                CloseHandle(filemap);
            if (mpwmap)
                CloseHandle(mpwmap);
            sfree(cl);
            break;
          }
          case IDM_RESTART:
            if (!wgs->backend) {
                lp_eventlog(&wgs->logpolicy, "----- Session restarted -----");
                term_pwron(wgs->term, false);
                start_backend(wgs);
            }

            break;
#ifdef MOD_RECONNECT
          case IDM_RESTARTSESSION:
            /* If already disconnected, just reconnect (same as IDM_RESTART).
             * If live, do the safe close+restart on a single toplevel cb so
             * close->pwron->start cannot interleave with a posted message. */
            if (!wgs->backend)
                PostMessage(hwnd, WM_COMMAND, IDM_RESTART, 0);
            else
                queue_toplevel_callback(close_and_restart, wgs);
            break;
#endif
          case IDM_REKEY:
            /* [Shortcuts] keyexchange: fire the SSH "Repeat key exchange"
             * special by looking up its SS_REKEY code. The classic-KiTTY
             * shortcut sent the command id of a fixed specials-menu index
             * (19), but 0.84 builds the specials list dynamically per
             * session, so a fixed index can land on a different special
             * (e.g. a signal) or past the end of the menu. No-op when the
             * backend has no rekey special (non-SSH, or session dead). */
            if (wgs->backend && wgs->specials) {
                int si;
                for (si = 0; si < wgs->n_specials; si++) {
                    if (wgs->specials[si].code == SS_REKEY) {
                        backend_special(wgs->backend, SS_REKEY,
                                        wgs->specials[si].arg);
                        break;
                    }
                }
            }
            break;
          case IDM_RECONF: {
            Conf *prev_conf;
            int init_lvl = 1;
            bool reconfig_result;

            if (wgs->reconfiguring)
                break;
            else
                wgs->reconfiguring = true;

            term_pre_reconfig(wgs->term, wgs->conf);
            prev_conf = conf_copy(wgs->conf);

#ifdef MOD_PERSO
            if (force_reconf == 0) {
                /* KiTTY silent apply: conf was already mutated in-place
                 * (Invert colours / Black on white etc.) — skip the dialog
                 * and just push the new conf into the terminal/palette. */
                force_reconf = 1;
                reconfig_result = true;
            } else
#endif
            reconfig_result = do_reconfig(
                hwnd, wgs->conf,
                wgs->backend ? backend_cfg_info(wgs->backend) : 0);
            wgs->reconfiguring = false;
            if (!reconfig_result) {
                conf_free(prev_conf);
                break;
            }

            conf_cache_data(wgs);

#ifdef MOD_PERSO
            /* KiTTY automatic saving: back up the registry hive to kitty.sav
             * each time the configuration dialog is closed with changes
             * applied. Self-skips in dir mode or when no sav file is set. */
            SaveRegistryKey();
#endif

            resize_action = conf_get_int(wgs->conf, CONF_resize_action);
            {
                /* Disable full-screen if resizing forbidden */
                int i;
                for (i = 0; i < lenof(wgs->popup_menus); i++)
                    EnableMenuItem(wgs->popup_menus[i].menu, IDM_FULLSCREEN,
                                   MF_BYCOMMAND |
                                   (resize_action == RESIZE_DISABLED
                                    ? MF_GRAYED : MF_ENABLED));
                /* Gracefully unzoom if necessary */
                if (IsZoomed(hwnd) && (resize_action == RESIZE_DISABLED))
                    ShowWindow(hwnd, SW_RESTORE);
            }

            /* Pass new config data to the logging module */
            log_reconfig(wgs->logctx, wgs->conf);

            sfree(wgs->logpal);
            /*
             * Flush the line discipline's edit buffer in the
             * case where local editing has just been disabled.
             */
            if (wgs->ldisc) {
                ldisc_configure(wgs->ldisc, wgs->conf);
                ldisc_echoedit_update(wgs->ldisc);
            }

            if (conf_get_bool(wgs->conf, CONF_system_colour) !=
                conf_get_bool(prev_conf, CONF_system_colour))
                term_notify_palette_changed(wgs->term);

            /* Pass new config data to the terminal */
            term_reconfig(wgs->term, wgs->conf);
            setup_clipboards(wgs->term, wgs->conf);

            /* Reinitialise the colour palette, in case the terminal
             * just read new settings out of Conf */
            if (wgs->pal)
                DeleteObject(wgs->pal);
            wgs->logpal = NULL;
            wgs->pal = NULL;
            init_palette(wgs);

            /* Pass new config data to the back end */
            if (wgs->backend)
                backend_reconfig(wgs->backend, wgs->conf);

            /* Screen size changed ? */
            if (conf_get_int(wgs->conf, CONF_height) !=
                conf_get_int(prev_conf, CONF_height) ||
                conf_get_int(wgs->conf, CONF_width) !=
                conf_get_int(prev_conf, CONF_width) ||
                conf_get_int(wgs->conf, CONF_savelines) !=
                conf_get_int(prev_conf, CONF_savelines) ||
                resize_action == RESIZE_FONT ||
                (resize_action == RESIZE_EITHER && IsZoomed(hwnd)) ||
                resize_action == RESIZE_DISABLED)
                term_size(wgs->term, conf_get_int(wgs->conf, CONF_height),
                          conf_get_int(wgs->conf, CONF_width),
                          conf_get_int(wgs->conf, CONF_savelines));

            /* Enable or disable the scroll bar, etc */
            {
                LONG nflg, flag = GetWindowLongPtr(hwnd, GWL_STYLE);
                LONG nexflag, exflag =
                    GetWindowLongPtr(hwnd, GWL_EXSTYLE);

                nexflag = exflag;
                if (conf_get_bool(wgs->conf, CONF_alwaysontop) !=
                    conf_get_bool(prev_conf, CONF_alwaysontop)) {
                    if (conf_get_bool(wgs->conf, CONF_alwaysontop)) {
                        nexflag |= WS_EX_TOPMOST;
                        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE);
                    } else {
                        nexflag &= ~(WS_EX_TOPMOST);
                        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE);
                    }
                }
                if (conf_get_bool(wgs->conf, CONF_sunken_edge))
                    nexflag |= WS_EX_CLIENTEDGE;
                else
                    nexflag &= ~(WS_EX_CLIENTEDGE);

                nflg = flag;
                if (conf_get_bool(wgs->conf, is_full_screen(wgs) ?
                                  CONF_scrollbar_in_fullscreen :
                                  CONF_scrollbar))
                    nflg |= WS_VSCROLL;
                else
                    nflg &= ~WS_VSCROLL;

                if (resize_action == RESIZE_DISABLED ||
                    is_full_screen(wgs))
                    nflg &= ~WS_THICKFRAME;
                else
                    nflg |= WS_THICKFRAME;

                if (resize_action == RESIZE_DISABLED)
                    nflg &= ~WS_MAXIMIZEBOX;
                else
                    nflg |= WS_MAXIMIZEBOX;

#ifdef MOD_PERSO
                /* KiTTY: the window-button settings, re-applied after Change
                 * Settings. LAST, so that switching maximise off wins over the
                 * resize-action line above rather than being undone by it - the
                 * two disagree only when the user asked for no maximise button
                 * AND left resizing enabled, and the explicit setting is the one
                 * they typed. */
                nflg = kitty_window_button_styles(wgs->conf, nflg);
                kitty_apply_close_button(wgs, hwnd);
#endif

                if (nflg != flag || nexflag != exflag) {
                    if (nflg != flag)
                        SetWindowLongPtr(hwnd, GWL_STYLE, nflg);
                    if (nexflag != exflag)
                        SetWindowLongPtr(hwnd, GWL_EXSTYLE, nexflag);

                    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                                 SWP_NOACTIVATE | SWP_NOCOPYBITS |
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                 SWP_FRAMECHANGED);

                    init_lvl = 2;
                }
            }

            /* Oops */
            if (resize_action == RESIZE_DISABLED && IsZoomed(hwnd)) {
                force_normal(hwnd);
                init_lvl = 2;
            }

            {
                FontSpec *font = conf_get_fontspec(wgs->conf, CONF_font);
                FontSpec *prev_font = conf_get_fontspec(prev_conf,
                                                        CONF_font);

                if (!strcmp(font->name, prev_font->name) ||
                    !strcmp(conf_get_str(wgs->conf, CONF_line_codepage),
                            conf_get_str(prev_conf, CONF_line_codepage)) ||
                    font->isbold != prev_font->isbold ||
                    font->height != prev_font->height ||
                    font->charset != prev_font->charset ||
                    conf_get_int(wgs->conf, CONF_font_quality) !=
                    conf_get_int(prev_conf, CONF_font_quality) ||
                    conf_get_int(wgs->conf, CONF_vtmode) !=
                    conf_get_int(prev_conf, CONF_vtmode) ||
                    conf_get_int(wgs->conf, CONF_bold_style) !=
                    conf_get_int(prev_conf, CONF_bold_style) ||
#ifdef MOD_PERSO
                    /* KiTTY line spacing changes the CELL height, so the fonts
                     * and the whole layout have to be rebuilt - without this,
                     * changing it in Change Settings did nothing until the
                     * window was next resized. */
                    conf_get_int(wgs->conf, CONF_line_spacing) !=
                    conf_get_int(prev_conf, CONF_line_spacing) ||
#endif
                    resize_action == RESIZE_DISABLED ||
                    resize_action == RESIZE_EITHER ||
                    resize_action != conf_get_int(prev_conf,
                                                  CONF_resize_action))
                    init_lvl = 2;
            }

            InvalidateRect(hwnd, NULL, true);
            reset_window(wgs, init_lvl);

            conf_free(prev_conf);
            break;
          }
          case IDM_COPYALL:
            term_copyall(wgs->term, clips_system, lenof(clips_system));
            break;
          case IDM_COPY:
            term_request_copy(wgs->term, clips_system, lenof(clips_system));
            break;
          case IDM_PASTE:
            term_request_paste(wgs->term, CLIP_SYSTEM);
            break;
          case IDM_CLRSB:
            term_clrsb(wgs->term);
            break;
          case IDM_RESET:
            term_pwron(wgs->term, true);
            if (wgs->ldisc)
                ldisc_echoedit_update(wgs->ldisc);
            break;
          case IDM_ABOUT:
            /* Unified branded About box (same one as the config dialog's About
             * button -> AboutProc); the old KiTTY-specific KittyAboutProc dialog
             * is retired to keep the two About boxes consistent. */
            showabout(hwnd);
            break;
#ifdef MOD_PERSO
          case IDM_MNOTEPAD:
            RunPuttyEd(hwnd, NULL);
            break;
          case IDM_MNOTEPAD_CLIP:
            RunPuttyEd(hwnd, "1");
            break;
          case IDM_CHECKUPDATE:
            CheckVersionFromWebSite(hwnd, 1);   /* live terminal: no-update -> title notice */
            break;
#endif
          case IDM_HELP:
            launch_help(hwnd, NULL);
            break;
          case SC_MOUSEMENU:
            /*
             * We get this if the System menu has been activated
             * using the mouse.
             */
            show_mouseptr(wgs, true);
            break;
          case SC_KEYMENU:
            /*
             * We get this if the System menu has been activated
             * using the keyboard. This might happen from within
             * TranslateKey, in which case it really wants to be
             * followed by a `space' character to actually _bring
             * the menu up_ rather than just sitting there in
             * `ready to appear' state.
             */
            show_mouseptr(wgs, true);    /* make sure pointer is visible */
            if (lParam == 0)
                PostMessage(hwnd, WM_CHAR, ' ', 0);
            break;
          case IDM_FULLSCREEN:
            flip_full_screen(wgs);
            break;
#ifdef MOD_PERSO
          case IDM_TRANSPARUP:
          case IDM_TRANSPARDOWN:
            kitty_menu_adjust_transparency(wgs->term_hwnd, wgs->conf,
                                           (wParam & ~0xF) == IDM_TRANSPARUP);
            break;
          case IDM_VISIBLE:
            kitty_menu_toggle_alwaysontop(wgs->term_hwnd, wgs->conf);
            break;
          case IDM_TOTRAY:
            kitty_send_to_tray(wgs->term_hwnd);
            break;
          case IDM_WINROL:
            kitty_rollup(wgs->term_hwnd, conf_get_int(wgs->conf, CONF_resize_action));
            break;
          case IDM_SCRIPTSEND: {
            char fn[4096];
            if (!kitty_script_enabled()) {
                MessageBox(wgs->term_hwnd, "RuTTY scripting is disabled"
                           " ([KiTTY] scriptmode=no in kitty.ini).",
                           "KiTTY", MB_OK | MB_ICONINFORMATION);
                break;
            }
            if (wgs->backend && !kitty_script_active() &&
                OpenFileName(wgs->term_hwnd, fn, "Send script file...",
                    "Script files (*.ksh,*.sh)|*.ksh;*.sh|All files (*.*)|*.*|")) {
                Filename *sf = filename_from_str(fn);
                kitty_script_send_file(wgs->conf, wgs->backend, sf);
                filename_free(sf);
            }
            break;
          }
          case IDM_SCRIPTHALT:
            kitty_script_stop();
            lp_eventlog(&wgs->logpolicy, "script stopped");
            break;
          case IDM_SCRIPTFILE2:
            OpenAndSendScriptFile(wgs->term_hwnd);
            break;
          case IDM_NEWDUPSESS: {
            /* A new window that starts at the configuration box carrying this
             * session's settings.
             *
             * KiTTY (cyd01/KiTTY#519): in the quick-connect way of working the
             * host comes WITH them, so the next machine in a cluster is reached
             * by editing one character of the one in front of you. The box puts
             * the caret in Host Name with the text selected, so typing replaces
             * it and Enter keeps it. Otherwise the host is dropped and the box
             * comes up empty, as it always has.
             *
             * Quick connect is reached two ways: [ConfigBox] loadlastsession=no
             * sets it for good, and loading "Default Settings" arms it for one
             * run (windows/putty.c). GetQuickConnectMode() is still true here -
             * the config box and the session it opened are the same process. */
            Conf *handoff = conf_copy(wgs->conf);
            if (GetLoadLastSessionFlag() && !GetQuickConnectMode())
                conf_set_str(handoff, CONF_host, "");
            RunConfigBoxWithConfSettings(handoff);
            conf_free(handoff);
            break;
          }
          case IDM_FONTUP:
            kitty_font_resize(wgs->term, wgs->conf, 1);
            break;
          case IDM_FONTDOWN:
            kitty_font_resize(wgs->term, wgs->conf, -1);
            break;
          case IDM_PROTECT:
            kitty_protect(wgs->term_hwnd, &wgs->termwin, wgs->conf);
            break;
          case IDM_PRINT:
            kitty_print(wgs->term_hwnd);
            break;
          case IDM_FONTNEGATIVE:
            kitty_negative(wgs->term_hwnd);
            break;
          case IDM_FONTBLACKANDWHITE:
            kitty_bw(wgs->term_hwnd);
            break;
          case IDM_CLEARLOGFILE:
            /* Do what the label promised (see WM_INITMENUPOPUP). Previously
             * this closed and reopened through logfopen(), which asks the
             * "file already exists" setting again - so in append mode an
             * explicit clear silently did nothing. */
            if (wgs->logctx &&
                conf_get_int(wgs->conf, CONF_logtype) != LGTYP_NONE) {
                if (logfile_name_varies(wgs->logctx))
                    logfile_rotate(wgs->logctx);
                else
                    logfile_clear(wgs->logctx);
            }
            break;
          case IDM_OPENLOGFILE: {
            /* KiTTY: open the session's own log in whatever the system uses
             * for it (hknet/KiTTY#31). Flush first - with "flush log file
             * frequently" off, the newest lines are still in the C buffer,
             * and those are exactly the ones being looked for. */
            const char *lf = wgs->logctx ? logfile_current_name(wgs->logctx)
                                         : NULL;
            if (lf) {
                logflush(wgs->logctx);
                ShellExecute(hwnd, "open", lf, NULL, NULL, SW_SHOWNORMAL);
            } else {
                /* No log open: nothing to show. The menu greys this out, so
                 * getting here means the state changed under the menu. */
                MessageBeep(MB_ICONWARNING);
            }
            break;
          }
          case IDM_RESIZE: {
            /* KiTTY: resize terminal to lParam cols(LOWORD) x rows(HIWORD) */
            int w = LOWORD(lParam), h = HIWORD(lParam);
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            conf_set_int(wgs->conf, CONF_width, w);
            conf_set_int(wgs->conf, CONF_height, h);
            term_size(wgs->term, h, w,
                      conf_get_int(wgs->conf, CONF_savelines));
            reset_window(wgs, 0);
            break;
          }
          case IDM_REPOS:
            /* KiTTY: move window to lParam x(LOWORD) y(HIWORD) */
            kitty_menu_reposition(wgs->term_hwnd, wgs->conf,
                                  LOWORD(lParam), HIWORD(lParam));
            break;
          case IDM_SHOWPORTFWD:
            kitty_showportfwd(wgs->term_hwnd, wgs->conf);
            break;
          case IDM_SHORTCUTSTOGGLE:
            kitty_shortcuts_toggle(wgs->term_hwnd);
            break;
          case IDM_WINSCP:
            kitty_start_winscp(wgs->term_hwnd);
            break;
          case IDM_PSCP:
            kitty_send_file(wgs->term_hwnd);
            break;
          case IDM_EXPORTSETTINGS:
            kitty_export_settings(wgs->term_hwnd, wgs->conf);
            break;
          case IDM_BROADCASTTOGGLE: {
            /* The quick view of the session setting: flips the SAME conf value
             * Session > Scripting edits, so the two never disagree. Not saved
             * unless the session is - an unsaved flip reverts on next load. */
            int on = !kitty_broadcast_armed(wgs);
            conf_set_bool(wgs->conf, CONF_kitty_accept_broadcast, on != 0);
            CheckMenuItem(GetSystemMenu(hwnd, FALSE), IDM_BROADCASTTOGGLE,
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            logevent(wgs->logctx, on ? "broadcasts accepted for this window"
                                     : "broadcasts refused for this window");
            break;
          }
          case IDM_HYPERLINKTOGGLE:
            /* KiTTY: enable/disable URL hyperlink detection at runtime */
            kitty_menu_toggle_hyperlink(hwnd);
            break;
          case IDM_QUIT:
            /* KiTTY: immediate exit without the close confirmation prompt */
            DestroyWindow(hwnd);
            break;
#ifdef MOD_ZMODEM
          case IDM_XYZSTART:
            if (GetZModemFlag())
                kitty_zmodem_receive(wgs->conf, wgs->backend, wgs->logctx, wgs->term);
            break;
          case IDM_XYZUPLOAD:
            if (GetZModemFlag())
                kitty_zmodem_send(hwnd, wgs->conf, wgs->backend, wgs->logctx, wgs->term);
            break;
          case IDM_XYZABORT:
            if (GetZModemFlag())
                kitty_zmodem_cancel();
            break;
#endif
#endif
          default:
            if (wParam >= IDM_SAVED_MIN && wParam < IDM_SAVED_MAX) {
                SendMessage(hwnd, WM_SYSCOMMAND, IDM_SAVEDSESS, wParam);
            }
            if (wParam >= IDM_SPECIAL_MIN && wParam <= IDM_SPECIAL_MAX) {
                int i = (wParam - IDM_SPECIAL_MIN) / 0x10;
                /*
                 * Ensure we haven't been sent a bogus SYSCOMMAND
                 * which would cause us to reference invalid memory
                 * and crash. Perhaps I'm just too paranoid here.
                 */
                if (i >= wgs->n_specials)
                    break;
                if (wgs->backend)
                    backend_special(wgs->backend, wgs->specials[i].code,
                                    wgs->specials[i].arg);
            }
#ifdef MOD_PERSO
            /* KiTTY predefined-command shortcuts: Ctrl+Shift+A..Z (and User
             * Command context-menu clicks) arrive here as WM_COMMAND
             * IDM_USERCMD+n; run the n-th SpecialMenu[] command. */
            {
                int nb = (int)LOWORD(wParam) - IDM_USERCMD;
                if (nb >= 0 && nb < NB_MENU_MAX)
                    ManageSpecialCommand(wgs->term_hwnd, nb);
            }
#endif
        }
        break;

#define X_POS(l) ((int)(short)LOWORD(l))
#define Y_POS(l) ((int)(short)HIWORD(l))

#define TO_CHR_X(x) ((((x)<0 ? (x)-wgs->font_width+1 :                  \
                       (x))-wgs->offset_width) / wgs->font_width)
#define TO_CHR_Y(y) ((((y)<0 ? (y)-wgs->font_height+1 :                 \
                       (y))-wgs->offset_height) / wgs->font_height)
      case WM_LBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_MBUTTONUP:
      case WM_RBUTTONUP:
#ifdef MOD_PERSO
        /* KiTTY mouse shortcuts: Shift+Ctrl+LClick = duplicate session,
         * Ctrl+MClick = send to tray. Protect mode disables them. */
        if (GetProtectFlag()) break;
        if (!GetPuttyFlag() && GetMouseShortcutsFlag()) {
            if (message == WM_LBUTTONUP &&
                (wParam & MK_SHIFT) && (wParam & MK_CONTROL)) {
                if (wgs->backend)
                    SendMessage(hwnd, WM_COMMAND, IDM_DUPSESS, 0);
                break;
            } else if (message == WM_MBUTTONUP && (wParam & MK_CONTROL)) {
                SendMessage(hwnd, WM_COMMAND, IDM_TOTRAY, 0);
                break;
            }
        }
#endif
        if (message == WM_RBUTTONDOWN &&
            ((wParam & MK_CONTROL) ||
             (conf_get_int(wgs->conf, CONF_mouse_is_xterm) == MOUSE_WINDOWS))) {
            POINT cursorpos;

            /* Just in case this happened in mid-select */
            term_cancel_selection_drag(wgs->term);

            show_mouseptr(wgs, true);    /* make sure pointer is visible */
            GetCursorPos(&cursorpos);
            TrackPopupMenu(wgs->popup_menus[CTXMENU].menu,
                           TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                           cursorpos.x, cursorpos.y,
                           0, hwnd, NULL);
            break;
        }
        {
            int button;
            bool press;

            switch (message) {
              case WM_LBUTTONDOWN:
                button = MBT_LEFT;
                wParam |= MK_LBUTTON;
                press = true;
                break;
              case WM_MBUTTONDOWN:
                button = MBT_MIDDLE;
                wParam |= MK_MBUTTON;
                press = true;
                break;
              case WM_RBUTTONDOWN:
                button = MBT_RIGHT;
                wParam |= MK_RBUTTON;
                press = true;
                break;
              case WM_LBUTTONUP:
                button = MBT_LEFT;
                wParam &= ~MK_LBUTTON;
                press = false;
                break;
              case WM_MBUTTONUP:
                button = MBT_MIDDLE;
                wParam &= ~MK_MBUTTON;
                press = false;
                break;
              case WM_RBUTTONUP:
                button = MBT_RIGHT;
                wParam &= ~MK_RBUTTON;
                press = false;
                break;
              default: /* shouldn't happen */
                button = 0;
                press = false;
            }
            show_mouseptr(wgs, true);
            /*
             * Special case: in full-screen mode, if the left
             * button is clicked in the very top left corner of the
             * window, we put up the System menu instead of doing
             * selection.
             */
            {
                bool mouse_on_hotspot = false;
                POINT pt;

                GetCursorPos(&pt);
#ifndef NO_MULTIMON
                if (p_GetMonitorInfoA && p_MonitorFromPoint) {
                    HMONITOR mon;
                    MONITORINFO mi;

                    mon = p_MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);

                    if (mon != NULL) {
                        mi.cbSize = sizeof(MONITORINFO);
                        p_GetMonitorInfoA(mon, &mi);

                        if (mi.rcMonitor.left == pt.x &&
                            mi.rcMonitor.top == pt.y) {
                            mouse_on_hotspot = true;
                        }
                    }
                } else
#endif
                if (pt.x == 0 && pt.y == 0) {
                    mouse_on_hotspot = true;
                }
                if (is_full_screen(wgs) && press &&
                    button == MBT_LEFT && mouse_on_hotspot) {
                    SendMessage(hwnd, WM_SYSCOMMAND, SC_MOUSEMENU,
                                MAKELPARAM(pt.x, pt.y));
                    return 0;
                }
            }

            if (press) {
                click(wgs, button,
                      TO_CHR_X(X_POS(lParam)), TO_CHR_Y(Y_POS(lParam)),
                      wParam & MK_SHIFT, wParam & MK_CONTROL,
                      is_alt_pressed());
                SetCapture(hwnd);
            } else {
#ifdef MOD_PERSO
                /* KiTTY URL hyperlinks: on left-button release, if (ctrl+)click
                 * lands on a detected URL region, launch it instead of
                 * completing a selection. */
                if (message == WM_LBUTTONUP && GetHyperlinkFlag() &&
                    kitty_url_click(wgs->term, wgs->conf,
                                    TO_CHR_X(X_POS(lParam)),
                                    TO_CHR_Y(Y_POS(lParam)),
                                    (wParam & MK_CONTROL) != 0)) {
                    term_cancel_selection_drag(wgs->term);
                    if (!(wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)))
                        ReleaseCapture();
                    return 0;
                }
#endif
                term_mouse(wgs->term, button, translate_button(wgs, button),
                           MA_RELEASE, TO_CHR_X(X_POS(lParam)),
                           TO_CHR_Y(Y_POS(lParam)), wParam & MK_SHIFT,
                           wParam & MK_CONTROL, is_alt_pressed());
                if (!(wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)))
                    ReleaseCapture();
            }
        }
        return 0;
      case WM_MOUSEMOVE:
        /*
         * Windows seems to like to occasionally send MOUSEMOVE
         * events even if the mouse hasn't moved. Don't unhide
         * the mouse pointer in this case.
         */
        if (wgs->last_mousemove != WM_MOUSEMOVE ||
            wParam != wgs->last_wm_mousemove_wParam ||
            lParam != wgs->last_wm_mousemove_lParam) {
            show_mouseptr(wgs, true);
            wgs->last_mousemove = WM_MOUSEMOVE;
            wgs->last_wm_mousemove_wParam = wParam;
            wgs->last_wm_mousemove_lParam = lParam;
        }
        /*
         * Add the mouse position and message time to the random
         * number noise.
         */
        noise_ultralight(NOISE_SOURCE_MOUSEPOS, lParam);

        if (wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON) &&
            GetCapture() == hwnd) {
            Mouse_Button b;
            if (wParam & MK_LBUTTON)
                b = MBT_LEFT;
            else if (wParam & MK_MBUTTON)
                b = MBT_MIDDLE;
            else
                b = MBT_RIGHT;
            term_mouse(wgs->term, b, translate_button(wgs, b), MA_DRAG,
                       TO_CHR_X(X_POS(lParam)),
                       TO_CHR_Y(Y_POS(lParam)), wParam & MK_SHIFT,
                       wParam & MK_CONTROL, is_alt_pressed());
        } else {
            term_mouse(wgs->term, MBT_NOTHING, MBT_NOTHING, MA_MOVE,
                       TO_CHR_X(X_POS(lParam)),
                       TO_CHR_Y(Y_POS(lParam)), false,
                       false, false);
        }
#ifdef MOD_PERSO
        /* KiTTY URL hyperlinks: update the hand cursor when hovering over a
         * link.  Link regions are kept current by the rescan in
         * wintw_setup_draw_ctx() whenever screen content changes; re-scanning
         * here would re-run the URL regex over the whole visible screen at
         * pointer speed for no new information. */
        if (GetHyperlinkFlag()) {
            kitty_url_hover(wgs->term, hwnd,
                            TO_CHR_X(X_POS(lParam)), TO_CHR_Y(Y_POS(lParam)),
                            conf_get_int(wgs->conf, CONF_url_hover_cursor));
        }
#endif
        return 0;
      case WM_NCMOUSEMOVE:
        if (wgs->last_mousemove != WM_NCMOUSEMOVE ||
            wParam != wgs->last_wm_ncmousemove_wParam ||
            lParam != wgs->last_wm_ncmousemove_lParam) {
            show_mouseptr(wgs, true);
            wgs->last_mousemove = WM_NCMOUSEMOVE;
            wgs->last_wm_ncmousemove_wParam = wParam;
            wgs->last_wm_ncmousemove_lParam = lParam;
        }
        noise_ultralight(NOISE_SOURCE_MOUSEPOS, lParam);
        break;
      case WM_IGNORE_CLIP:
        wgs->ignore_clip = wParam; /* don't panic on DESTROYCLIPBOARD */
        break;
      case WM_DESTROYCLIPBOARD:
#ifdef MOD_FAR2L
        /* In far2l clipboard-sync mode the clipboard is owned/managed by the
         * far2l extension, so don't treat loss of ownership as a paste-cancel. */
        if (!(wgs->term->far2l_ext == 1 && wgs->term->clip_allowed)) {
            if (!wgs->ignore_clip)
                term_lost_clipboard_ownership(wgs->term, CLIP_SYSTEM);
        }
#else
        if (!wgs->ignore_clip)
            term_lost_clipboard_ownership(wgs->term, CLIP_SYSTEM);
#endif
        wgs->ignore_clip = false;
        return 0;
      case WM_PAINT: {
        PAINTSTRUCT p;

        HideCaret(hwnd);
        hdc = BeginPaint(hwnd, &p);
        if (wgs->pal) {
            SelectPalette(hdc, wgs->pal, true);
            RealizePalette(hdc);
        }

        /*
         * We have to be careful about term_paint(). It will
         * set a bunch of character cells to INVALID and then
         * call do_paint(), which will redraw those cells and
         * _then mark them as done_. This may not be accurate:
         * when painting in WM_PAINT context we are restricted
         * to the rectangle which has just been exposed - so if
         * that only covers _part_ of a character cell and the
         * rest of it was already visible, that remainder will
         * not be redrawn at all. Accordingly, we must not
         * paint any character cell in a WM_PAINT context which
         * already has a pending update due to terminal output.
         * The simplest solution to this - and many, many
         * thanks to Hung-Te Lin for working all this out - is
         * not to do any actual painting at _all_ if there's a
         * pending terminal update: just mark the relevant
         * character cells as INVALID and wait for the
         * scheduled full update to sort it out.
         *
         * I have a suspicion this isn't the _right_ solution.
         * An alternative approach would be to have terminal.c
         * separately track what _should_ be on the terminal
         * screen and what _is_ on the terminal screen, and
         * have two completely different types of redraw (one
         * for full updates, which syncs the former with the
         * terminal itself, and one for WM_PAINT which syncs
         * the latter with the former); yet another possibility
         * would be to have the Windows front end do what the
         * GTK one already does, and maintain a bitmap of the
         * current terminal appearance so that WM_PAINT becomes
         * completely trivial. However, this should do for now.
         */
        assert(!wgs->wintw_hdc);
        wgs->wintw_hdc = hdc;
        term_paint(wgs->term,
                   (p.rcPaint.left-wgs->offset_width)/wgs->font_width,
                   (p.rcPaint.top-wgs->offset_height)/wgs->font_height,
                   (p.rcPaint.right-wgs->offset_width-1)/wgs->font_width,
                   (p.rcPaint.bottom-wgs->offset_height-1)/wgs->font_height,
                   !wgs->term->window_update_pending);
        wgs->wintw_hdc = NULL;

        if (p.fErase ||
            p.rcPaint.left  < wgs->offset_width  ||
            p.rcPaint.top   < wgs->offset_height ||
            p.rcPaint.right >= (wgs->offset_width +
                                wgs->font_width*wgs->term->cols) ||
            p.rcPaint.bottom>= (wgs->offset_height +
                                wgs->font_height*wgs->term->rows)) {
            HBRUSH fillcolour, oldbrush;
            HPEN   edge, oldpen;
            fillcolour = CreateSolidBrush (
                wgs->colours[ATTR_DEFBG>>ATTR_BGSHIFT]);
            oldbrush = SelectObject(hdc, fillcolour);
            edge = CreatePen(PS_SOLID, 0,
                             wgs->colours[ATTR_DEFBG>>ATTR_BGSHIFT]);
            oldpen = SelectObject(hdc, edge);

            /*
             * Jordan Russell reports that this apparently
             * ineffectual IntersectClipRect() call masks a
             * Windows NT/2K bug causing strange display
             * problems when the PuTTY window is taller than
             * the primary monitor. It seems harmless enough...
             */
            IntersectClipRect(hdc,
                              p.rcPaint.left, p.rcPaint.top,
                              p.rcPaint.right, p.rcPaint.bottom);

            ExcludeClipRect(
                hdc, wgs->offset_width, wgs->offset_height,
                wgs->offset_width+wgs->font_width*wgs->term->cols,
                wgs->offset_height+wgs->font_height*wgs->term->rows);

            Rectangle(hdc, p.rcPaint.left, p.rcPaint.top,
                      p.rcPaint.right, p.rcPaint.bottom);

            /* SelectClipRgn(hdc, NULL); */

            SelectObject(hdc, oldbrush);
            DeleteObject(fillcolour);
            SelectObject(hdc, oldpen);
            DeleteObject(edge);
        }
        SelectObject(hdc, GetStockObject(SYSTEM_FONT));
        SelectObject(hdc, GetStockObject(WHITE_PEN));
        EndPaint(hwnd, &p);
        ShowCaret(hwnd);
        return 0;
      }
      case WM_NETEVENT:
      case WM_DONE_WITH_SOCKET:
        winselgui_response(message, wParam, lParam);
        return 0;
      case WM_SETFOCUS:
        term_set_focus(wgs->term, true);
        CreateCaret(hwnd, wgs->caretbm, wgs->font_width, wgs->font_height);
        ShowCaret(hwnd);
        flash_window(wgs, 0);               /* stop */
        wgs->compose_state = 0;
        term_update(wgs->term);
        break;
      case WM_KILLFOCUS:
        show_mouseptr(wgs, true);
        term_set_focus(wgs->term, false);
        DestroyCaret();
        wgs->caret_x = wgs->caret_y = -1; /* ensure caret replaced next time */
        term_update(wgs->term);
        break;
      case WM_ENTERSIZEMOVE:
        EnableSizeTip(true);
        wgs->resizing = true;
        wgs->need_backend_resize = false;
        break;
      case WM_EXITSIZEMOVE:
        EnableSizeTip(false);
        wgs->resizing = false;
        if (wgs->need_backend_resize) {
            term_size(wgs->term, conf_get_int(wgs->conf, CONF_height),
                      conf_get_int(wgs->conf, CONF_width),
                      conf_get_int(wgs->conf, CONF_savelines));
            InvalidateRect(hwnd, NULL, true);
        }
        recompute_window_offset(wgs);
        break;
#ifdef MOD_PERSO
      case WM_WINDOWPOSCHANGING:
        /* #554: hard-lock the embedded child to the host's client area. Every
         * SetWindowPos/MoveWindow funnels through here, so whatever tries to move
         * or resize the window (a font-size change in particular) is overridden to
         * fill the parent. The terminal can then only reflow rows/cols -- it can
         * never move or resize the pane. (Parent-driven resizes don't reach the
         * child, so TIMER_EMBEDFILL still covers those.) */
        {
          /* Only clamp for the explicit -hwndparent SELF-embed (we manage the
           * window and fill the host). When a host like mRemoteNG reparents us
           * itself, IT positions/sizes the window (deliberately offsetting the
           * frame off-screen); clamping there just fights it and makes the window
           * wobble. reset_window already stops a font change from resizing the
           * window, so no clamp is needed for the host-managed case. */
          HWND host = kitty_hwnd_parent;
          if (host && IsWindow(host)) {
            RECT prc;
            if (GetClientRect(host, &prc) &&
                prc.right > 0 && prc.bottom > 0) {
                WINDOWPOS *wp = (WINDOWPOS *)lParam;
                wp->x = 0; wp->y = 0;
                wp->cx = prc.right; wp->cy = prc.bottom;
                wp->flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
            }
          }
        }
        break;   /* let DefWindowProc apply the (adjusted) WINDOWPOS */
#endif
      case WM_SIZING:
        /*
         * This does two jobs:
         * 1) Keep the sizetip uptodate
         * 2) Make sure the window size is _stepped_ in units of the font size.
         */
        resize_action = conf_get_int(wgs->conf, CONF_resize_action);
        if (resize_action == RESIZE_TERM ||
            (resize_action == RESIZE_EITHER && !is_alt_pressed())) {
            int width, height, w, h, ew, eh;
            LPRECT r = (LPRECT) lParam;

            if (!wgs->need_backend_resize && resize_action == RESIZE_EITHER &&
                (conf_get_int(wgs->conf, CONF_height) != wgs->term->rows ||
                 conf_get_int(wgs->conf, CONF_width) != wgs->term->cols)) {
                /*
                 * Great! It seems that both the terminal size and the
                 * font size have been changed and the user is now dragging.
                 *
                 * It will now be difficult to get back to the configured
                 * font size!
                 *
                 * This would be easier but it seems to be too confusing.
                 */
                conf_set_int(wgs->conf, CONF_height, wgs->term->rows);
                conf_set_int(wgs->conf, CONF_width, wgs->term->cols);

                InvalidateRect(hwnd, NULL, true);
                wgs->need_backend_resize = true;
            }

            width = r->right - r->left - wgs->extra_width;
            height = r->bottom - r->top - wgs->extra_height;
            w = (width + wgs->font_width / 2) / wgs->font_width;
            if (w < 1)
                w = 1;
            h = (height + wgs->font_height / 2) / wgs->font_height;
            if (h < 1)
                h = 1;
            UpdateSizeTip(hwnd, w, h);
            ew = width - w * wgs->font_width;
            eh = height - h * wgs->font_height;
            if (ew != 0) {
                if (wParam == WMSZ_LEFT ||
                    wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_TOPLEFT)
                    r->left += ew;
                else
                    r->right -= ew;
            }
            if (eh != 0) {
                if (wParam == WMSZ_TOP ||
                    wParam == WMSZ_TOPRIGHT || wParam == WMSZ_TOPLEFT)
                    r->top += eh;
                else
                    r->bottom -= eh;
            }
            if (ew || eh)
                return 1;
            else
                return 0;
        } else {
            int width, height, w, h, rv = 0;
            int window_border = conf_get_int(wgs->conf, CONF_window_border);
            int ex_width = wgs->extra_width +
                (window_border - wgs->offset_width) * 2;
            int ex_height = wgs->extra_height +
                (window_border - wgs->offset_height) * 2;
            LPRECT r = (LPRECT) lParam;

            width = r->right - r->left - ex_width;
            height = r->bottom - r->top - ex_height;

            w = (width + wgs->term->cols/2)/wgs->term->cols;
            h = (height + wgs->term->rows/2)/wgs->term->rows;
            if ( r->right != r->left + w*wgs->term->cols + ex_width)
                rv = 1;

            if (wParam == WMSZ_LEFT ||
                wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_TOPLEFT)
                r->left = r->right - w*wgs->term->cols - ex_width;
            else
                r->right = r->left + w*wgs->term->cols + ex_width;

            if (r->bottom != r->top + h*wgs->term->rows + ex_height)
                rv = 1;

            if (wParam == WMSZ_TOP ||
                wParam == WMSZ_TOPRIGHT || wParam == WMSZ_TOPLEFT)
                r->top = r->bottom - h*wgs->term->rows - ex_height;
            else
                r->bottom = r->top + h*wgs->term->rows + ex_height;

            return rv;
        }
        /* break;  (never reached) */
      case WM_FULLSCR_ON_MAX:
        wgs->fullscr_on_max = true;
        break;
      case WM_MOVE:
        term_notify_window_pos(wgs->term, LOWORD(lParam), HIWORD(lParam));
        sys_cursor_update(wgs);
        break;
      case WM_SIZE:
        resize_action = conf_get_int(wgs->conf, CONF_resize_action);
#ifdef MOD_PERSO
        /* #554: once a host (mRemoteNG) has reparented us, our font DPI may be
         * stale -- the window was created on whatever monitor Windows first
         * placed it (often a different scaling than the host's pane), and a
         * reparent doesn't send WM_DPICHANGED. Re-detect the DPI of the monitor
         * we're actually shown on now and re-init the fonts once, so the text
         * isn't rendered 2x too big (or small) for the host. */
        if (!wgs->embed_dpi_synced && KITTY_IS_EMBEDDED(hwnd)) {
            wgs->embed_dpi_synced = true;
            /* NB: do NOT strip the window frame here. The host (mRemoteNG) sizes
             * and positions us assuming our normal frame (it offsets the caption
             * off-screen); removing the frame desynchronises that and makes the
             * window wobble on BOTH axes during a resize drag. Leave the frame
             * intact and only correct the font DPI below. */
            int olddpi = wgs->dpi_info.cur_dpi.y;
            wgs->dpi_info.cur_dpi.x = wgs->dpi_info.cur_dpi.y = 0;
            init_dpi_info(wgs);
            if (wgs->dpi_info.cur_dpi.y != olddpi)
                reset_window(wgs, 2);   /* re-init fonts at the corrected DPI */
        }
#endif
        term_notify_minimised(wgs->term, wParam == SIZE_MINIMIZED);
#ifdef MOD_PERSO
        /* KiTTY feature: when minimised and SendToTray active, hide to tray */
        if (wParam == SIZE_MINIMIZED && GetAutoSendToTray()) {
            kitty_send_to_tray(hwnd);
            return 0;
        }
#endif
        {
            /*
             * WM_SIZE's lParam tells us the size of the client area.
             * But historic PuTTY practice is that we want to tell the
             * terminal the size of the overall window.
             */
            RECT r;
            GetWindowRect(hwnd, &r);
            term_notify_window_size_pixels(
                wgs->term, r.right - r.left, r.bottom - r.top);
        }
        if (wParam == SIZE_MINIMIZED)
            sw_SetWindowText(hwnd,
                             conf_get_bool(wgs->conf, CONF_win_name_always) ?
                             wgs->window_name : wgs->icon_name);
        if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED)
            sw_SetWindowText(hwnd, wgs->window_name);
        if (wParam == SIZE_RESTORED) {
            wgs->processed_resize = false;
            clear_full_screen(wgs);
            if (wgs->processed_resize) {
                /*
                 * Inhibit normal processing of this WM_SIZE; a
                 * secondary one was triggered just now by
                 * clear_full_screen which contained the correct
                 * client area size.
                 */
                return 0;
            }
        }
        if (wParam == SIZE_MAXIMIZED && wgs->fullscr_on_max) {
            wgs->fullscr_on_max = false;
            wgs->processed_resize = false;
            make_full_screen(wgs);
            if (wgs->processed_resize) {
                /*
                 * Inhibit normal processing of this WM_SIZE; a
                 * secondary one was triggered just now by
                 * make_full_screen which contained the correct client
                 * area size.
                 */
                return 0;
            }
        }

        wgs->processed_resize = true;

        if (resize_action == RESIZE_DISABLED) {
            /* A resize, well it better be a minimize. */
            reset_window(wgs, -1);
        } else {
            if (wParam == SIZE_MAXIMIZED) {
                wgs->was_zoomed = true;
                wgs->prev_rows = wgs->term->rows;
                wgs->prev_cols = wgs->term->cols;
                if (resize_action == RESIZE_TERM)
                    wm_size_resize_term(wgs, lParam);
                reset_window(wgs, 0);
            } else if (wParam == SIZE_RESTORED && wgs->was_zoomed) {
                wgs->was_zoomed = false;
                if (resize_action == RESIZE_TERM) {
                    wm_size_resize_term(wgs, lParam);
                    reset_window(wgs, 2);
                } else if (resize_action != RESIZE_FONT)
                    reset_window(wgs, 2);
                else
                    reset_window(wgs, 0);
            } else if (wParam == SIZE_MINIMIZED) {
                /* do nothing */
            } else if (resize_action == RESIZE_TERM ||
                       (resize_action == RESIZE_EITHER &&
                        !is_alt_pressed())) {
                wm_size_resize_term(wgs, lParam);

                /*
                 * Sometimes, we can get a spontaneous resize event
                 * outside a WM_SIZING interactive drag which wants to
                 * set us to a new specific SIZE_RESTORED size. An
                 * example is what happens if you press Windows+Right
                 * and then Windows+Up: the first operation fits the
                 * window to the right-hand half of the screen, and
                 * the second one changes that for the top right
                 * quadrant. In that situation, if we've responded
                 * here by resizing the terminal, we may still need to
                 * recompute the border around the window and do a
                 * full redraw to clear the new border.
                 */
                if (!wgs->resizing)
                    recompute_window_offset(wgs);
            } else {
                reset_window(wgs, 0);
            }
        }
        sys_cursor_update(wgs);
#ifdef MOD_PERSO
        /* [KiTTY] size=yes: keep the [rows x cols] title suffix live while
         * resizing (the set-title path dedupes, so this is a no-op unless the
         * decorated title actually changed). */
        if (GetTitleBarFlag() && GetSizeFlag())
            kitty_refresh_title();
#endif
        return 0;
      case WM_DPICHANGED:
        wgs->dpi_info.cur_dpi.x = LOWORD(wParam);
        wgs->dpi_info.cur_dpi.y = HIWORD(wParam);
        wgs->dpi_info.new_wnd_rect = *(RECT*)(lParam);
        reset_window(wgs, 3);
        return 0;
      case WM_VSCROLL:
        switch (LOWORD(wParam)) {
          case SB_BOTTOM:
            term_scroll(wgs->term, -1, 0);
            break;
          case SB_TOP:
            term_scroll(wgs->term, +1, 0);
            break;
          case SB_LINEDOWN:
            term_scroll(wgs->term, 0, +1);
            break;
          case SB_LINEUP:
            term_scroll(wgs->term, 0, -1);
            break;
          case SB_PAGEDOWN:
            term_scroll(wgs->term, 0, +wgs->term->rows / 2);
            break;
          case SB_PAGEUP:
            term_scroll(wgs->term, 0, -wgs->term->rows / 2);
            break;
          case SB_THUMBPOSITION:
          case SB_THUMBTRACK: {
            /*
             * Use GetScrollInfo instead of HIWORD(wParam) to get
             * 32-bit scroll position.
             */
            SCROLLINFO si;

            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            if (GetScrollInfo(hwnd, SB_VERT, &si) == 0)
                si.nTrackPos = HIWORD(wParam);
            term_scroll(wgs->term, 1, si.nTrackPos);
            break;
          }
        }

        if (wgs->in_scrollbar_loop) {
            /*
             * Allow window updates to happen during interactive
             * scroll.
             *
             * When the user takes hold of our window's scrollbar and
             * wobbles it interactively back and forth, or presses on
             * one of the arrow buttons at the ends, the first thing
             * that happens is that this window procedure receives
             * WM_SYSCOMMAND / SC_VSCROLL. [1] The default handler for
             * that window message starts a subsidiary message loop,
             * which continues to run until the user lets go of the
             * scrollbar again. All WM_VSCROLL / SB_THUMBTRACK
             * messages are generated by the handlers within that
             * subsidiary message loop.
             *
             * So, during that time, _our_ message loop is not
             * running, which means toplevel callbacks and timers and
             * so forth are not happening, which means that when we
             * redraw the window and set a timer to clear the cooldown
             * flag 20ms later, that timer never fires, and we aren't
             * able to keep redrawing the window.
             *
             * The 'obvious' answer would be to seize that SYSCOMMAND
             * ourselves and inhibit the default handler, so that our
             * message loop carries on running. But that would mean
             * we'd have to reimplement the whole of the scrollbar
             * handler!
             *
             * So instead we apply a bodge: set a static variable that
             * indicates that we're _in_ that sub-loop, and if so,
             * decide it's OK to manually call term_update() proper,
             * bypassing the timer and cooldown and rate-limiting
             * systems completely, whenever we see an SB_THUMBTRACK.
             * This shouldn't cause a rate overload, because we're
             * only doing it once per UI event!
             *
             * [1] Actually, there's an extra oddity where SC_HSCROLL
             * and SC_VSCROLL have their documented values the wrong
             * way round. Many people on the Internet have noticed
             * this, e.g. https://stackoverflow.com/q/55528397
             */
            term_update(wgs->term);
        }
        break;
      case WM_PALETTECHANGED:
        if ((HWND) wParam != hwnd && wgs->pal != NULL) {
            HDC hdc = make_hdc(wgs);
            if (hdc) {
                if (RealizePalette(hdc) > 0)
                    UpdateColors(hdc);
                free_hdc(wgs, hdc);
            }
        }
        break;
      case WM_QUERYNEWPALETTE:
        if (wgs->pal != NULL) {
            HDC hdc = make_hdc(wgs);
            if (hdc) {
                if (RealizePalette(hdc) > 0)
                    UpdateColors(hdc);
                free_hdc(wgs, hdc);
                return true;
            }
        }
        return false;
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYUP:
        /*
         * Add the scan code and keypress timing to the random
         * number noise.
         */
        noise_ultralight(NOISE_SOURCE_KEY, lParam);

#ifdef MOD_PERSO
        /* When the session is inactive (ended/disconnected, no backend), let
         * Ctrl+D close the window from the keyboard -- otherwise only Alt+F4 or
         * the mouse can dismiss it. Goes through WM_CLOSE, which closes without
         * a prompt once session_closed and saves the remembered position. */
        if (message == WM_KEYDOWN && !wgs->backend && wgs->session_closed &&
            (wParam == 'D' || wParam == 'd') &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
#ifdef MOD_RECONNECT
        /* Classic KiTTY: an ordinary keypress (e.g. Enter) in a window whose
         * session has ended restarts the session (hknet/KiTTY#12). Only plain
         * typing keys qualify: modifier keys themselves, Tab, arrows and
         * F-keys are ignored, as is any chord with Ctrl/Alt held, so Ctrl+D
         * close, Ctrl-Tab switching and the shortcuts dispatcher keep working
         * on a dead window. Gated like the other MOD_RECONNECT paths:
         * autoreconnect enabled and the session authenticated at least once
         * (never re-dial on a failed login). */
        if (message == WM_KEYDOWN && !wgs->backend && wgs->session_closed &&
            GetAutoreconnectFlag() && wgs->ever_authenticated &&
            !(GetKeyState(VK_CONTROL) & 0x8000) &&
            !(GetKeyState(VK_MENU) & 0x8000) &&
            wParam != VK_CONTROL && wParam != VK_SHIFT && wParam != VK_MENU &&
            wParam != VK_TAB && wParam != VK_LEFT && wParam != VK_UP &&
            wParam != VK_RIGHT && wParam != VK_DOWN &&
            !(wParam >= VK_F1 && wParam <= VK_F16)) {
            lp_eventlog(&wgs->logpolicy,
                        "No connection on key pressed, trying to reconnect...");
            PostMessage(hwnd, WM_COMMAND, IDM_RESTART, 0);
            return 0;
        }
#endif
        /* KiTTY Ctrl-Tab session switching (consume VK_TAB+Ctrl first). */
        if (wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (conf_get_int(wgs->conf, CONF_ctrl_tab_switch) && GetCtrlTabFlag()) {
                if (message == WM_KEYUP) {
                    int wx = GetClassLong(hwnd, GCL_CBWNDEXTRA);
                    struct ctrl_tab_info info = {
                        (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : -1, hwnd, };
                    info.next_hi_date_time = info.self_hi_date_time =
                        GetWindowLong(hwnd, wx - 8);
                    info.next_lo_date_time = info.self_lo_date_time =
                        GetWindowLong(hwnd, wx - 4);
                    EnumWindows(CtrlTabWindowProc, (LPARAM)&info);
                    if (info.next != NULL && info.next != hwnd)
                        SetForegroundWindow(info.next);
                    return 0;
                }
                return sw_DefWindowProc(hwnd, message, wParam, lParam);
            }
        }
        /* KiTTY keyboard shortcuts dispatcher. */
        if (GetShortcutsFlag()) {
            if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
                if (ManageShortcuts(
                        wgs->term, wgs->conf, hwnd, clips_system, (int)wParam,
                        GetKeyState(VK_SHIFT)   & 0x8000,
                        GetKeyState(VK_CONTROL) & 0x8000,
                        (GetKeyState(VK_MENU) & 0x8000) || (GetKeyState(VK_LMENU) & 0x8000),
                        GetKeyState(VK_RMENU)   & 0x8000,
                        (GetKeyState(VK_RWIN) & 0x8000) || (GetKeyState(VK_LWIN) & 0x8000)))
                    return 0;
            }
        } else {
            if (GetProtectFlag() == 1)
                return 0;
        }
#endif

        /*
         * We don't do TranslateMessage since it disassociates the
         * resulting CHAR message from the KEYDOWN that sparked it,
         * which we occasionally don't want. Instead, we process
         * KEYDOWN, and call the Win32 translator functions so that
         * we get the translations under _our_ control.
         */
        {
            unsigned char buf[20];
            int len;

            if (wParam == VK_PROCESSKEY || /* IME PROCESS key */
                wParam == VK_PACKET) {     /* 'this key is a Unicode char' */
                if (message == WM_KEYDOWN) {
                    MSG m;
                    m.hwnd = hwnd;
                    m.message = WM_KEYDOWN;
                    m.wParam = wParam;
                    m.lParam = lParam & 0xdfff;
                    TranslateMessage(&m);
                } else break; /* pass to Windows for default processing */
            } else {
                len = TranslateKey(wgs, message, wParam, lParam, buf);
                if (len == -1)
                    return sw_DefWindowProc(hwnd, message, wParam, lParam);

                if (len != 0) {
                    /*
                     * We need not bother about stdin backlogs
                     * here, because in GUI PuTTY we can't do
                     * anything about it anyway; there's no means
                     * of asking Windows to hold off on KEYDOWN
                     * messages. We _have_ to buffer everything
                     * we're sent.
                     */
                    term_keyinput(wgs->term, -1, buf, len);
                    show_mouseptr(wgs, false);
                }
            }
        }
        return 0;
      case WM_INPUTLANGCHANGE:
        /* wParam == Font number */
        /* lParam == Locale */
        set_input_locale(wgs, (HKL)lParam);
        sys_cursor_update(wgs);
        break;
      case WM_IME_STARTCOMPOSITION: {
        HIMC hImc = ImmGetContext(hwnd);
        ImmSetCompositionFont(hImc, &wgs->lfont);
        ImmReleaseContext(hwnd, hImc);
        break;
      }
      case WM_IME_COMPOSITION: {
        HIMC hIMC;
        int n;
        char *buff;

        if (osPlatformId == VER_PLATFORM_WIN32_WINDOWS ||
            osPlatformId == VER_PLATFORM_WIN32s)
            break; /* no Unicode */

        if ((lParam & GCS_RESULTSTR) == 0) /* Composition unfinished. */
            break; /* fall back to DefWindowProc */

        hIMC = ImmGetContext(hwnd);
        n = ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, NULL, 0);

        if (n > 0) {
            int i;
            buff = snewn(n, char);
            ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, buff, n);
            /*
             * Jaeyoun Chung reports that Korean character
             * input doesn't work correctly if we do a single
             * term_keyinputw covering the whole of buff. So
             * instead we send the characters one by one.
             */
            /* don't divide SURROGATE PAIR */
            if (wgs->ldisc) {
                for (i = 0; i < n; i += 2) {
                    WCHAR hs = *(unsigned short *)(buff+i);
                    if (IS_HIGH_SURROGATE(hs) && i+2 < n) {
                        WCHAR ls = *(unsigned short *)(buff+i+2);
                        if (IS_LOW_SURROGATE(ls)) {
                            term_keyinputw(
                                wgs->term, (unsigned short *)(buff+i), 2);
                            i += 2;
                            continue;
                        }
                    }
                    term_keyinputw(
                        wgs->term, (unsigned short *)(buff+i), 1);
                }
            }
            free(buff);
        }
        ImmReleaseContext(hwnd, hIMC);
        return 1;
      }

      case WM_IME_CHAR:
        if (wParam & 0xFF00) {
            char buf[2];

            buf[1] = wParam;
            buf[0] = wParam >> 8;
            term_keyinput(wgs->term, wgs->kbd_codepage, buf, 2);
        } else {
            char c = (unsigned char) wParam;
            term_seen_key_event(wgs->term);
            term_keyinput(wgs->term, wgs->kbd_codepage, &c, 1);
        }
        return (0);
      case WM_CHAR:
      case WM_SYSCHAR:
        /*
         * Nevertheless, we are prepared to deal with WM_CHAR
         * messages, should they crop up. So if someone wants to
         * post the things to us as part of a macro manoeuvre,
         * we're ready to cope.
         */
        if (unicode_window) {
            wchar_t c = wParam;

            if (IS_HIGH_SURROGATE(c)) {
                wgs->pending_surrogate = c;
            } else if (IS_SURROGATE_PAIR(wgs->pending_surrogate, c)) {
                wchar_t pair[2];
                pair[0] = wgs->pending_surrogate;
                pair[1] = c;
                term_keyinputw(wgs->term, pair, 2);
            } else if (!IS_SURROGATE(c)) {
                term_keyinputw(wgs->term, &c, 1);
            }
        } else {
            char c = (unsigned char)wParam;
            term_seen_key_event(wgs->term);
            if (wgs->ldisc)
                term_keyinput(wgs->term, CP_ACP, &c, 1);
        }
        return 0;
      case WM_SYSCOLORCHANGE:
        if (conf_get_bool(wgs->conf, CONF_system_colour)) {
            /* Refresh palette from system colours. */
            term_notify_palette_changed(wgs->term);
            init_palette(wgs);
            /* Force a repaint of the terminal window. */
            term_invalidate(wgs->term);
        }
        break;
      case WM_GOT_CLIPDATA:
        process_clipdata(wgs, (HGLOBAL)lParam, wParam);
        return 0;
#ifdef MOD_PERSO
      case WM_KITTY_WORKPLACE_DISARM:
        /*
         * The "your workplace proxy did not answer" notice was clicked.
         *
         * It does NOT switch the mode off. Disarming a whole proxy setup from
         * one click on a notice is too much for a click to do, and the user may
         * well want the mode and simply be somewhere else for an hour. So this
         * opens a configuration window straight at Connection/Proxy, where the
         * switch, the proxy in use and the timeout are all in front of them and
         * the decision is theirs.
         *
         * A new window rather than this session's Change Settings: the Proxy
         * panel is not offered mid-session, so Change Settings would open on a
         * box that does not contain the thing they came for.
         */
        {
            char exe[MAX_PATH], cmd[MAX_PATH + 64];
            DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
            if (n && n < sizeof(exe)) {
                STARTUPINFOA si;
                PROCESS_INFORMATION pi;
                memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
                memset(&pi, 0, sizeof(pi));
                /* The workplace switch lives on its own leaf now. */
                sprintf(cmd, "\"%s\" -cfgpanel \"Application/Workplace proxy\"", exe);
                if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL,
                                   &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }
        }
        return 0;
      case WM_KITTY_AGENT_UNVERIFIED:
        /*
         * The "SSH agent not verified" notice (kitty_win.c) was clicked.
         * Same shape as the workplace notice above: put the user in front
         * of the setting rather than changing anything for them - and a
         * NEW window rather than Change Settings, because the SSH/Auth
         * panel is not offered mid-session.
         */
        {
            char exe[MAX_PATH], cmd[MAX_PATH + 64];
            DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
            if (n && n < sizeof(exe)) {
                STARTUPINFOA si;
                PROCESS_INFORMATION pi;
                memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
                memset(&pi, 0, sizeof(pi));
                sprintf(cmd, "\"%s\" -cfgpanel Connection/SSH/Auth", exe);
                if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL,
                                   &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }
        }
        return 0;
#endif
      default:
        if (message == wm_mousewheel || message == WM_MOUSEWHEEL
                                                || message == WM_MOUSEHWHEEL) {
            bool shift_pressed = false, control_pressed = false;

            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
                wgs->wheel_accumulator += (short)HIWORD(wParam);
                shift_pressed=LOWORD(wParam) & MK_SHIFT;
                control_pressed=LOWORD(wParam) & MK_CONTROL;
            } else {
                BYTE keys[256];
                wgs->wheel_accumulator += (int)wParam;
                if (GetKeyboardState(keys)!=0) {
                    shift_pressed=keys[VK_SHIFT]&0x80;
                    control_pressed=keys[VK_CONTROL]&0x80;
                }
            }

            /* process events when the threshold is reached */
            while (abs(wgs->wheel_accumulator) >= WHEEL_DELTA) {
                int b;

                /* reduce amount for next time */
                if (wgs->wheel_accumulator > 0) {
                    b = message == WM_MOUSEHWHEEL ? MBT_WHEEL_RIGHT : MBT_WHEEL_UP;
                    wgs->wheel_accumulator -= WHEEL_DELTA;
                } else if (wgs->wheel_accumulator < 0) {
                    b =  message == WM_MOUSEHWHEEL ? MBT_WHEEL_LEFT : MBT_WHEEL_DOWN;
                    wgs->wheel_accumulator += WHEEL_DELTA;
                } else
                    break;

                if (wgs->send_raw_mouse &&
                    !(conf_get_bool(wgs->conf, CONF_mouse_override) &&
                      shift_pressed)) {
                    /* Mouse wheel position is in screen coordinates for
                     * some reason */
                    POINT p;
                    p.x = X_POS(lParam); p.y = Y_POS(lParam);
                    if (ScreenToClient(hwnd, &p)) {
                        /* send a mouse-down followed by a mouse up */
                        term_mouse(wgs->term, b, translate_button(wgs, b),
                                   MA_CLICK,
                                   TO_CHR_X(p.x),
                                   TO_CHR_Y(p.y), shift_pressed,
                                   control_pressed, is_alt_pressed());
                    } /* else: not sure when this can fail */
                }
#ifdef MOD_PERSO
                else if (control_pressed && message != WM_MOUSEHWHEEL) {
                    /* KiTTY: Ctrl + mouse wheel = zoom the terminal font, via the
                     * same path as the Font Up/Down menu items. MOD_PERSO only:
                     * kitty_font_resize() lives in kitty_bridge.c, which the stock
                     * pterm/puttytel builds (which also compile this file) don't link. */
                    kitty_font_resize(wgs->term, wgs->conf,
                                      b == MBT_WHEEL_UP ? 1 : -1);
                }
#endif
                else if (message != WM_MOUSEHWHEEL) {
#ifdef MOD_PERSO
                    /*
                     * KiTTY: how far one wheel notch scrolls, from
                     * CONF_scrolllines ("Lines at a time" / LinesAtAScroll).
                     *
                     *   -1  half a screen - the default, and what this did
                     *       unconditionally before, so nobody's scrolling moves
                     *       unless they ask for it
                     *   -2  a whole screen
                     *    N  that many lines, for N > 0
                     *
                     * Anything below -2 is a value nobody meant; 3 lines is the
                     * Windows convention and beats scrolling by a negative
                     * number of lines.
                     *
                     * The setting has been on the Window panel and in every
                     * saved session all along - nothing read it, so it did
                     * nothing at all. */
                    int lines;
                    {
                        int cs = conf_get_int(wgs->conf, CONF_scrolllines);
                        lines = (cs == -1) ? wgs->term->rows / 2 :
                                (cs == -2) ? wgs->term->rows :
                                (cs   <  0) ? 3 : cs;
                        if (lines < 1)
                            lines = 1;   /* 0 would make the wheel do nothing */
                    }
                    term_scroll(wgs->term, 0,
                                b == MBT_WHEEL_UP ? -lines : lines);
#else
                    /* trigger a scroll */
                    term_scroll(wgs->term, 0,
                                b == MBT_WHEEL_UP ?
                                -wgs->term->rows / 2 : wgs->term->rows / 2);
#endif
                }
            }
            return 0;
        }
    }

    /*
     * Any messages we don't process completely above are passed through to
     * DefWindowProc() for default processing.
     */
    return sw_DefWindowProc(hwnd, message, wParam, lParam);
}

/*
 * Move the system caret. (We maintain one, even though it's
 * invisible, for the benefit of blind people: apparently some
 * helper software tracks the system caret, so we should arrange to
 * have one.)
 */
static void wintw_set_cursor_pos(TermWin *tw, int x, int y)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    int cx, cy;

    if (!wgs->term->has_focus) return;

    /*
     * Avoid gratuitously re-updating the cursor position and IMM
     * window if there's no actual change required.
     */
    cx = x * wgs->font_width + wgs->offset_width;
    cy = y * wgs->font_height + wgs->offset_height;
    if (cx == wgs->caret_x && cy == wgs->caret_y)
        return;
    wgs->caret_x = cx;
    wgs->caret_y = cy;

    sys_cursor_update(wgs);
}

static void sys_cursor_update(WinGuiSeat *wgs)
{
    COMPOSITIONFORM cf;
    HIMC hIMC;

    if (!wgs->term->has_focus) return;

    if (wgs->caret_x < 0 || wgs->caret_y < 0)
        return;

    SetCaretPos(wgs->caret_x, wgs->caret_y);

    /* IMM calls on Win98 and beyond only */
    if (osPlatformId == VER_PLATFORM_WIN32s) return; /* 3.11 */

    if (osPlatformId == VER_PLATFORM_WIN32_WINDOWS &&
        osMinorVersion == 0) return; /* 95 */

    /* we should have the IMM functions */
    hIMC = ImmGetContext(wgs->term_hwnd);
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = wgs->caret_x;
    cf.ptCurrentPos.y = wgs->caret_y;
    ImmSetCompositionWindow(hIMC, &cf);

    ImmReleaseContext(wgs->term_hwnd, hIMC);
}

static void draw_horizontal_line_on_text(
    WinGuiSeat *wgs, int y, int lattr, RECT line_box, COLORREF colour)
{
    if (lattr == LATTR_TOP || lattr == LATTR_BOT) {
        y *= 2;
        if (lattr == LATTR_BOT)
            y -= wgs->font_height;
    }

    if (!(0 <= y && y < wgs->font_height))
        return;

    HPEN oldpen = SelectObject(wgs->wintw_hdc, CreatePen(PS_SOLID, 0, colour));
    MoveToEx(wgs->wintw_hdc, line_box.left, line_box.top + y, NULL);
    LineTo(wgs->wintw_hdc, line_box.right, line_box.top + y);
    oldpen = SelectObject(wgs->wintw_hdc, oldpen);
    DeleteObject(oldpen);
}

/*
 * Draw a line of text in the window, at given character
 * coordinates, in given attributes.
 *
 * We are allowed to fiddle with the contents of `text'.
 */
static void do_text_internal(
    WinGuiSeat *wgs, int x, int y, wchar_t *text, int len,
    unsigned long attr, int lattr, truecolour truecolour)
{
    COLORREF fg, bg, t;
    int nfg, nbg, nfont;
    RECT line_box;
    bool force_manual_underline = false;
    int fnt_width, char_width;
    int text_adjust = 0;
    int xoffset = 0;
    int maxlen, remaining;
    bool opaque;
    bool is_cursor = false;
    int *lpDx = NULL;
    size_t lpDx_len = 0;
    bool use_lpDx;
    wchar_t *wbuf = NULL;
    char *cbuf = NULL;
    size_t wbuflen = 0, cbuflen = 0;
    int len2; /* for SURROGATE PAIR */

    lattr &= LATTR_MODE;

    char_width = fnt_width = wgs->font_width * (1 + (lattr != LATTR_NORM));

    if (attr & ATTR_WIDE)
        char_width *= 2;

    /* Only want the left half of double width lines */
    if (lattr != LATTR_NORM && x*2 >= wgs->term->cols)
        return;

#ifdef MOD_PERSO
    /* Capture character coords before the pixel conversion below, for the URL
     * hyperlink underline span test at the end of this function. */
    int kitty_url_col = x, kitty_url_row = y;
#endif

    x *= fnt_width;
    y *= wgs->font_height;
    x += wgs->offset_width;
    y += wgs->offset_height;

    if ((attr & ATTR_ACTCURS) &&
        (wgs->cursor_type == CURSOR_BLOCK || wgs->term->big_cursor)) {
        truecolour.fg = truecolour.bg = optionalrgb_none;
        attr &= ~(ATTR_REVERSE|ATTR_BLINK|ATTR_COLOURS|ATTR_DIM);
        /* cursor fg and bg */
        attr |= (260 << ATTR_FGSHIFT) | (261 << ATTR_BGSHIFT);
        is_cursor = true;
    }

    nfont = 0;
    if (wgs->vtmode == VT_POORMAN && lattr != LATTR_NORM) {
        /* Assume a poorman font is borken in other ways too. */
        lattr = LATTR_WIDE;
    } else
        switch (lattr) {
          case LATTR_NORM:
            break;
          case LATTR_WIDE:
            nfont |= FONT_WIDE;
            break;
          default:
            nfont |= FONT_WIDE + FONT_HIGH;
            break;
        }
    if (attr & ATTR_NARROW)
        nfont |= FONT_NARROW;

#ifdef USES_VTLINE_HACK
    /* Special hack for the VT100 linedraw glyphs. */
    if (text[0] >= 0x23BA && text[0] <= 0x23BD) {
        switch ((unsigned char) (text[0])) {
          case 0xBA:
            text_adjust = -2 * wgs->font_height / 5;
            break;
          case 0xBB:
            text_adjust = -1 * wgs->font_height / 5;
            break;
          case 0xBC:
            text_adjust = wgs->font_height / 5;
            break;
          case 0xBD:
            text_adjust = 2 * wgs->font_height / 5;
            break;
        }
        if (lattr == LATTR_TOP || lattr == LATTR_BOT)
            text_adjust *= 2;
        text[0] = wgs->ucsdata.unitab_xterm['q'];
        if (attr & ATTR_UNDER) {
            attr &= ~ATTR_UNDER;
            force_manual_underline = true;
        }
    }
#endif

#ifdef MOD_PERSO
    /*
     * KiTTY line spacing: push the glyph down by half the cell's spare height,
     * so the extra is shared above and below the text. Every glyph draw in this
     * function offsets by text_adjust, so one addition covers them all - and it
     * happens AFTER the VT100 linedraw hack above, which ASSIGNS text_adjust
     * rather than adding to it and would otherwise throw the shift away. The
     * background rectangles do not use text_adjust, so they keep filling the
     * whole, taller cell.
     */
    text_adjust += wgs->font_yshift;
#endif

    /* Anything left as an original character set is unprintable. */
    if (DIRECT_CHAR(text[0]) &&
        (len < 2 || !IS_SURROGATE_PAIR(text[0], text[1]))) {
        int i;
        for (i = 0; i < len; i++)
            text[i] = 0xFFFD;
    }

    /* OEM CP */
    if ((text[0] & CSET_MASK) == CSET_OEMCP)
        nfont |= FONT_OEM;

    nfg = ((attr & ATTR_FGMASK) >> ATTR_FGSHIFT);
    nbg = ((attr & ATTR_BGMASK) >> ATTR_BGSHIFT);
    if (wgs->bold_font_mode == BOLD_FONT && (attr & ATTR_BOLD))
        nfont |= FONT_BOLD;
    if (wgs->und_mode == UND_FONT && (attr & ATTR_UNDER))
        nfont |= FONT_UNDERLINE;
    another_font(wgs, nfont);
    if (!wgs->fonts[nfont]) {
        if (nfont & FONT_UNDERLINE)
            force_manual_underline = true;
        /* Don't do the same for manual bold, it could be bad news. */

        nfont &= ~(FONT_BOLD | FONT_UNDERLINE);
    }
    another_font(wgs, nfont);
    if (!wgs->fonts[nfont])
        nfont = FONT_NORMAL;
    if (attr & ATTR_REVERSE) {
        struct optionalrgb trgb;

        t = nfg;
        nfg = nbg;
        nbg = t;

        trgb = truecolour.fg;
        truecolour.fg = truecolour.bg;
        truecolour.bg = trgb;
    }
    if (wgs->bold_colours && (attr & ATTR_BOLD) && !is_cursor) {
        if (nfg < 16) nfg |= 8;
        else if (nfg >= 256) nfg |= 1;
    }
    if (wgs->bold_colours && (attr & ATTR_BLINK)) {
        if (nbg < 16) nbg |= 8;
        else if (nbg >= 256) nbg |= 1;
    }
#ifdef MOD_TUTTYCOLOR
    /* KiTTY (TuTTY): colour underlined text with the dedicated under_fg slot. */
    if ((attr & ATTR_UNDER) && !is_cursor &&
        conf_get_int(wgs->conf, CONF_under_colour))
        nfg = OSC4_COLOUR_under_fg;
#endif
#ifdef MOD_TUTTYCOLOR
    /* KiTTY (TuTTY): colour the selection with the dedicated sel_fg/sel_bg
     * slots instead of reverse-video. terminal.c only sets ATTR_SELECTED
     * when CONF_sel_colour is on, so no extra conf check is needed here. */
    if ((attr & ATTR_SELECTED) && !is_cursor) {
        nfg = OSC4_COLOUR_sel_fg;
        nbg = OSC4_COLOUR_sel_bg;
        /* Palette slots must override any 24-bit truecolour on the cell,
         * otherwise the truecolour.fg.enabled branch below ignores nfg/nbg. */
        truecolour.fg.enabled = false;
        truecolour.bg.enabled = false;
    }
#endif
    if (!wgs->pal && truecolour.fg.enabled)
        fg = RGB(truecolour.fg.r, truecolour.fg.g, truecolour.fg.b);
    else
        fg = wgs->colours[nfg];

    if (!wgs->pal && truecolour.bg.enabled)
        bg = RGB(truecolour.bg.r, truecolour.bg.g, truecolour.bg.b);
    else
        bg = wgs->colours[nbg];

    if (!wgs->pal && (attr & ATTR_DIM)) {
        fg = RGB(GetRValue(fg) * 2 / 3,
                 GetGValue(fg) * 2 / 3,
                 GetBValue(fg) * 2 / 3);
    }

    SelectObject(wgs->wintw_hdc, wgs->fonts[nfont]);
    SetTextColor(wgs->wintw_hdc, fg);
    SetBkColor(wgs->wintw_hdc, bg);
    if (attr & TATTR_COMBINING)
        SetBkMode(wgs->wintw_hdc, TRANSPARENT);
    else
        SetBkMode(wgs->wintw_hdc, OPAQUE);
    line_box.left = x;
    line_box.top = y;
    line_box.right = x + char_width * len;
    line_box.bottom = y + wgs->font_height;
    /* adjust line_box.right for SURROGATE PAIR & VARIATION SELECTOR */
    {
        int i;
        int rc_width = 0;
        for (i = 0; i < len ; i++) {
            if (i+1 < len && IS_HIGH_VARSEL(text[i], text[i+1])) {
                i++;
            } else if (i+1 < len && IS_SURROGATE_PAIR(text[i], text[i+1])) {
                rc_width += char_width;
                i++;
            } else if (IS_LOW_VARSEL(text[i])) {
                /* do nothing */
            } else {
                rc_width += char_width;
            }
        }
        line_box.right = line_box.left + rc_width;
    }

    /* Only want the left half of double width lines */
    if (line_box.right > wgs->font_width*wgs->term->cols+wgs->offset_width)
        line_box.right = wgs->font_width*wgs->term->cols+wgs->offset_width;

    if (wgs->font_varpitch) {
        /*
         * If we're using a variable-pitch font, we unconditionally
         * draw the glyphs one at a time and centre them in their
         * character cells (which means in particular that we must
         * disable the lpDx mechanism). This gives slightly odd but
         * generally reasonable results.
         */
        xoffset = char_width / 2;
        SetTextAlign(wgs->wintw_hdc, TA_TOP | TA_CENTER | TA_NOUPDATECP);
        use_lpDx = false;
        maxlen = 1;
    } else {
        /*
         * In a fixed-pitch font, we draw the whole string in one go
         * in the normal way.
         */
        xoffset = 0;
        SetTextAlign(wgs->wintw_hdc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
        use_lpDx = true;
        maxlen = len;
    }

    opaque = true;                     /* start by erasing the rectangle */
#ifdef MOD_BACKGROUNDIMAGE
    /*
     * KiTTY background-image compositing, adapted to 0.84's direct-to-window
     * paint path (0.76b used a textdc back-buffer that 0.84 removed). For
     * cells whose background is the terminal default background AND a
     * background image is loaded, blit the matching region of the desktop-
     * sized background DC straight into this cell's rectangle, then draw the
     * glyphs with a TRANSPARENT background so the image shows through. Cells
     * with a non-default background (selections, colour runs, the cursor)
     * keep the normal opaque fill, exactly as without an image. Entirely
     * inert when no image is loaded (backgrounddc == NULL), so normal
     * rendering is provably unchanged.
     */
    {
        extern HDC backgrounddc;       /* kitty_image.c, NULL until loaded */
        if (backgrounddc && bg == wgs->colours[258] &&
            line_box.right > line_box.left) {
            POINT bgloc;
            bgloc.x = line_box.left;
            bgloc.y = line_box.top;
            /* backgrounddc holds the image in screen coordinates */
            ClientToScreen(wgs->term_hwnd, &bgloc);
            BitBlt(wgs->wintw_hdc, line_box.left, line_box.top,
                   line_box.right - line_box.left,
                   line_box.bottom - line_box.top,
                   backgrounddc, bgloc.x, bgloc.y, SRCCOPY);
            SetBkMode(wgs->wintw_hdc, TRANSPARENT);
            opaque = false;            /* don't ETO_OPAQUE over the image */
        }
    }
#endif
    for (remaining = len; remaining > 0;
         text += len, remaining -= len, x += char_width * len2) {
        len = (maxlen < remaining ? maxlen : remaining);
        /* don't divide SURROGATE PAIR and VARIATION SELECTOR */
        len2 = len;
        if (maxlen == 1) {
            if (remaining >= 1 && IS_SURROGATE_PAIR(text[0], text[1]))
                len++;
            if (remaining-len >= 1 && IS_LOW_VARSEL(text[len]))
                len++;
            else if (remaining-len >= 2 &&
                     IS_HIGH_VARSEL(text[len], text[len+1]))
                len += 2;
        }

        if (len > lpDx_len)
            sgrowarray(lpDx, lpDx_len, len);

        {
            int i;
            /* only last char has dx width in SURROGATE PAIR and
             * VARIATION sequence */
            for (i = 0; i < len; i++) {
                lpDx[i] = char_width;
                if (i+1 < len && IS_HIGH_VARSEL(text[i], text[i+1])) {
                    if (i > 0) lpDx[i-1] = 0;
                    lpDx[i] = 0;
                    i++;
                    lpDx[i] = char_width;
                } else if (i+1 < len && IS_SURROGATE_PAIR(text[i],text[i+1])) {
                    lpDx[i] = 0;
                    i++;
                    lpDx[i] = char_width;
                } else if (IS_LOW_VARSEL(text[i])) {
                    if (i > 0) lpDx[i-1] = 0;
                    lpDx[i] = char_width;
                }
            }
        }

        /* We're using a private area for direct to font. (512 chars.) */
        if (wgs->ucsdata.dbcs_screenfont &&
            (text[0] & CSET_MASK) == CSET_ACP) {
            /* Ho Hum, dbcs fonts are a PITA! */
            /* To display on W9x I have to convert to UCS */
            int nlen, mptr;

            sgrowarray(wbuf, wbuflen, len);
            for (nlen = mptr = 0; mptr<len; mptr++) {
                wbuf[nlen] = 0xFFFD;
                if (IsDBCSLeadByteEx(wgs->ucsdata.font_codepage,
                                     (BYTE) text[mptr])) {
                    char dbcstext[2];
                    dbcstext[0] = text[mptr] & 0xFF;
                    dbcstext[1] = text[mptr+1] & 0xFF;
                    lpDx[nlen] += char_width;
                    MultiByteToWideChar(
                        wgs->ucsdata.font_codepage, MB_USEGLYPHCHARS,
                        dbcstext, 2, wbuf+nlen, 1);
                    mptr++;
                } else {
                    char dbcstext[1];
                    dbcstext[0] = text[mptr] & 0xFF;
                    MultiByteToWideChar(
                        wgs->ucsdata.font_codepage, MB_USEGLYPHCHARS,
                        dbcstext, 1, wbuf+nlen, 1);
                }
                nlen++;
            }
            if (nlen <= 0)
                goto out;                /* Eeek! */

            ExtTextOutW(
                wgs->wintw_hdc, x + xoffset,
                y - wgs->font_height * (lattr == LATTR_BOT) + text_adjust,
                ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
                &line_box, wbuf, nlen, (use_lpDx ? lpDx : NULL));
            if (wgs->bold_font_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
                SetBkMode(wgs->wintw_hdc, TRANSPARENT);
                ExtTextOutW(
                    wgs->wintw_hdc, x + xoffset - 1,
                    y - wgs->font_height * (lattr == LATTR_BOT) + text_adjust,
                    ETO_CLIPPED, &line_box, wbuf, nlen,
                    (use_lpDx ? lpDx : NULL));
            }

            lpDx[0] = -1;
        } else if (DIRECT_FONT(text[0])) {
            sgrowarray(cbuf, cbuflen, len);
            for (size_t i = 0; i < len; i++)
                cbuf[i] = text[i] & 0xFF;

            ExtTextOut(
                wgs->wintw_hdc, x + xoffset,
                y - wgs->font_height * (lattr == LATTR_BOT) + text_adjust,
                ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
                &line_box, cbuf, len, (use_lpDx ? lpDx : NULL));
            if (wgs->bold_font_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
                SetBkMode(wgs->wintw_hdc, TRANSPARENT);

                /* GRR: This draws the character outside its box and
                 * can leave 'droppings' even with the clip box! I
                 * suppose I could loop it one character at a time ...
                 * yuk.
                 *
                 * Or ... I could do a test print with "W", and use +1
                 * or -1 for this shift depending on if the leftmost
                 * column is blank...
                 */
                ExtTextOut(
                    wgs->wintw_hdc, x + xoffset - 1,
                    y - wgs->font_height * (lattr == LATTR_BOT) + text_adjust,
                    ETO_CLIPPED, &line_box, cbuf, len,
                    (use_lpDx ? lpDx : NULL));
            }
        } else {
            /* And 'normal' unicode characters */
            sgrowarray(wbuf, wbuflen, len);
            for (int i = 0; i < len; i++)
                wbuf[i] = text[i];

#ifdef MOD_PERSO
            /* KiTTY font fallback (cyd01/KiTTY#555): when a character here
             * is missing from the primary font but present in a configured
             * fallback font, draw per-font runs instead. The all-primary
             * case (the overwhelmingly common one) stays on general_textout
             * below, keeping RTL handling and Windows' built-in font
             * linking exactly as before. */
            WinFB_Run fb_runs[WINFB_MAX_RUNS];
            int fb_nruns = winfb_split(wbuf, len, fb_runs, WINFB_MAX_RUNS);
            if (fb_nruns > 1 || (fb_nruns == 1 && fb_runs[0].slot >= 0)) {
                winfb_draw_runs(
                    wgs->wintw_hdc, x + xoffset,
                    y - wgs->font_height * (lattr==LATTR_BOT) + text_adjust,
                    &line_box, wbuf, len, (use_lpDx ? lpDx : NULL),
                    fb_runs, fb_nruns,
                    opaque && !(attr & TATTR_COMBINING),
                    (nfont & FONT_BOLD) != 0, false,
                    (nfont & FONT_UNDERLINE) != 0);
            } else
#endif
            /* print Glyphs as they are, without Windows' Shaping*/
            general_textout(
                wgs, wgs->wintw_hdc, x + xoffset,
                y - wgs->font_height * (lattr==LATTR_BOT) + text_adjust,
                &line_box, wbuf, len, lpDx,
                opaque && !(attr & TATTR_COMBINING));

            /* And the shadow bold hack. */
            if (wgs->bold_font_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
                SetBkMode(wgs->wintw_hdc, TRANSPARENT);
                ExtTextOutW(
                    wgs->wintw_hdc, x + xoffset - 1,
                    y - wgs->font_height * (lattr == LATTR_BOT) + text_adjust,
                    ETO_CLIPPED, &line_box, wbuf, len,
                    (use_lpDx ? lpDx : NULL));
            }
        }

        /*
         * If we're looping round again, stop erasing the background
         * rectangle.
         */
        SetBkMode(wgs->wintw_hdc, TRANSPARENT);
        opaque = false;
    }

    if (lattr != LATTR_TOP && 
        (force_manual_underline || (wgs->und_mode == UND_LINE &&
                                    (attr & ATTR_UNDER))))
        draw_horizontal_line_on_text(wgs, wgs->descent, lattr, line_box, fg);

    if (attr & ATTR_STRIKE)
        draw_horizontal_line_on_text(wgs, wgs->font_strikethrough_y, lattr,
                                     line_box, fg);

#ifdef MOD_PERSO
    /* KiTTY URL hyperlink underline: underline the cells of this run that fall
     * inside a detected link region.  The underline-enabled conf lookup is
     * hoisted out here, once per text run, rather than paid per cell.
     * Coalesce contiguous link cells into spans so we issue one line per span
     * rather than per cell. */
    if (lattr != LATTR_TOP && GetHyperlinkFlag() &&
        conf_get_int(wgs->conf, CONF_url_underline)) {
        int kk, span0 = -1;
        for (kk = 0; kk <= len; kk++) {
            int inlink = (kk < len) &&
                kitty_url_cell_in_link(kitty_url_col + kk, kitty_url_row);
            if (inlink) {
                if (span0 < 0) span0 = kk;
            } else if (span0 >= 0) {
                RECT ul = line_box;
                ul.left  = line_box.left + span0 * char_width;
                ul.right = line_box.left + kk * char_width;
                draw_horizontal_line_on_text(wgs, wgs->descent, lattr, ul, fg);
                span0 = -1;
            }
        }
    }
#endif

  out:
    sfree(lpDx);
    sfree(wbuf);
    sfree(cbuf);
}

/*
 * Wrapper that handles combining characters.
 */
static void wintw_draw_text(
    TermWin *tw, int x, int y, wchar_t *text, int len,
    unsigned long attr, int lattr, truecolour truecolour)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (attr & TATTR_COMBINING) {
        unsigned long a = 0;
        int len0 = 1;
        /* don't divide SURROGATE PAIR and VARIATION SELECTOR */
        if (len >= 2 && IS_SURROGATE_PAIR(text[0], text[1]))
            len0 = 2;
        if (len-len0 >= 1 && IS_LOW_VARSEL(text[len0])) {
            attr &= ~TATTR_COMBINING;
            do_text_internal(wgs, x, y, text, len0+1, attr, lattr, truecolour);
            text += len0+1;
            len -= len0+1;
            a = TATTR_COMBINING;
        } else if (len-len0 >= 2 && IS_HIGH_VARSEL(text[len0], text[len0+1])) {
            attr &= ~TATTR_COMBINING;
            do_text_internal(wgs, x, y, text, len0+2, attr, lattr, truecolour);
            text += len0+2;
            len -= len0+2;
            a = TATTR_COMBINING;
        } else {
            attr &= ~TATTR_COMBINING;
        }

        while (len--) {
            if (len >= 1 && IS_SURROGATE_PAIR(text[0], text[1])) {
                do_text_internal(wgs, x, y, text, 2, attr | a, lattr,
                                 truecolour);
                len--;
                text++;
            } else
                do_text_internal(wgs, x, y, text, 1, attr | a, lattr,
                                 truecolour);

            text++;
            a = TATTR_COMBINING;
        }
    } else
        do_text_internal(wgs, x, y, text, len, attr, lattr, truecolour);
}

static void wintw_draw_cursor(
    TermWin *tw, int x, int y, wchar_t *text, int len,
    unsigned long attr, int lattr, truecolour truecolour)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    int fnt_width;
    int char_width;
    int ctype = wgs->cursor_type;

    lattr &= LATTR_MODE;

    if ((attr & ATTR_ACTCURS) &&
        (ctype == CURSOR_BLOCK || wgs->term->big_cursor)) {
        if (*text != UCSWIDE) {
            win_draw_text(tw, x, y, text, len, attr, lattr, truecolour);
            return;
        }
        ctype = CURSOR_VERTICAL_LINE;
        attr |= ATTR_RIGHTCURS;
    }

    fnt_width = char_width = wgs->font_width * (1 + (lattr != LATTR_NORM));
    if (attr & ATTR_WIDE)
        char_width *= 2;
    x *= fnt_width;
    y *= wgs->font_height;
    x += wgs->offset_width;
    y += wgs->offset_height;

    if ((attr & ATTR_PASCURS) &&
        (ctype == CURSOR_BLOCK || wgs->term->big_cursor)) {
        POINT pts[5];
        HPEN oldpen;
        pts[0].x = pts[1].x = pts[4].x = x;
        pts[2].x = pts[3].x = x + char_width - 1;
        pts[0].y = pts[3].y = pts[4].y = y;
        pts[1].y = pts[2].y = y + wgs->font_height - 1;
        oldpen = SelectObject(wgs->wintw_hdc,
                              CreatePen(PS_SOLID, 0, wgs->colours[261]));
        Polyline(wgs->wintw_hdc, pts, 5);
        oldpen = SelectObject(wgs->wintw_hdc, oldpen);
        DeleteObject(oldpen);
    } else if ((attr & (ATTR_ACTCURS | ATTR_PASCURS)) &&
               ctype != CURSOR_BLOCK) {
        int startx, starty, dx, dy, length, i;
        if (ctype == CURSOR_UNDERLINE) {
            startx = x;
            starty = y + wgs->descent;
            dx = 1;
            dy = 0;
            length = char_width;
        } else /* ctype == CURSOR_VERTICAL_LINE */ {
            int xadjust = 0;
            if (attr & ATTR_RIGHTCURS)
                xadjust = char_width - 1;
            startx = x + xadjust;
            starty = y;
            dx = 0;
            dy = 1;
            length = wgs->font_height;
        }
        if (attr & ATTR_ACTCURS) {
            HPEN oldpen;
            oldpen =
                SelectObject(wgs->wintw_hdc,
                             CreatePen(PS_SOLID, 0, wgs->colours[261]));
            MoveToEx(wgs->wintw_hdc, startx, starty, NULL);
            LineTo(wgs->wintw_hdc, startx + dx * length, starty + dy * length);
            oldpen = SelectObject(wgs->wintw_hdc, oldpen);
            DeleteObject(oldpen);
        } else {
            for (i = 0; i < length; i++) {
                if (i % 2 == 0) {
                    SetPixel(wgs->wintw_hdc, startx, starty,
                             wgs->colours[261]);
                }
                startx += dx;
                starty += dy;
            }
        }
    }
}

static void wintw_draw_trust_sigil(TermWin *tw, int x, int y)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);

    x *= wgs->font_width;
    y *= wgs->font_height;
    x += wgs->offset_width;
    y += wgs->offset_height;

    DrawIconEx(wgs->wintw_hdc, x, y, trust_icon,
               wgs->font_width * 2, wgs->font_height, 0, NULL, DI_NORMAL);
}

/* This function gets the actual width of a character in the normal font.
 */
static int wintw_char_width(TermWin *tw, int uc)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    int ibuf = 0;

    /* If the font max is the same as the font ave width then this
     * function is a no-op.
     */
    if (!wgs->font_dualwidth) return 1;

    switch (uc & CSET_MASK) {
      case CSET_ASCII:
        uc = wgs->ucsdata.unitab_line[uc & 0xFF];
        break;
      case CSET_LINEDRW:
        uc = wgs->ucsdata.unitab_xterm[uc & 0xFF];
        break;
      case CSET_SCOACS:
        uc = wgs->ucsdata.unitab_scoacs[uc & 0xFF];
        break;
    }
    if (DIRECT_FONT(uc)) {
        if (wgs->ucsdata.dbcs_screenfont) return 1;

        /* Speedup, I know of no font where ascii is the wrong width */
        if ((uc&~CSET_MASK) >= ' ' && (uc&~CSET_MASK)<= '~')
            return 1;

        if ( (uc & CSET_MASK) == CSET_ACP ) {
            SelectObject(wgs->wintw_hdc, wgs->fonts[FONT_NORMAL]);
        } else if ( (uc & CSET_MASK) == CSET_OEMCP ) {
            another_font(wgs, FONT_OEM);
            if (!wgs->fonts[FONT_OEM]) return 0;

            SelectObject(wgs->wintw_hdc, wgs->fonts[FONT_OEM]);
        } else
            return 0;

        if (GetCharWidth32(wgs->wintw_hdc, uc & ~CSET_MASK,
                           uc & ~CSET_MASK, &ibuf) != 1 &&
            GetCharWidth(wgs->wintw_hdc, uc & ~CSET_MASK,
                         uc & ~CSET_MASK, &ibuf) != 1)
            return 0;
    } else {
        /* Speedup, I know of no font where ascii is the wrong width */
        if (uc >= ' ' && uc <= '~') return 1;

        SelectObject(wgs->wintw_hdc, wgs->fonts[FONT_NORMAL]);
        if (GetCharWidth32W(wgs->wintw_hdc, uc, uc, &ibuf) == 1)
            /* Okay that one worked */ ;
        else if (GetCharWidthW(wgs->wintw_hdc, uc, uc, &ibuf) == 1)
            /* This should work on 9x too, but it's "less accurate" */ ;
        else
            return 0;
    }

    ibuf += wgs->font_width / 2 -1;
    ibuf /= wgs->font_width;

    return ibuf;
}

DECL_WINDOWS_FUNCTION(static, BOOL, FlashWindowEx, (PFLASHWINFO));
DECL_WINDOWS_FUNCTION(static, BOOL, ToUnicodeEx,
                      (UINT, UINT, const BYTE *, LPWSTR, int, UINT, HKL));
DECL_WINDOWS_FUNCTION(static, BOOL, PlaySoundW, (LPCWSTR, HMODULE, DWORD));
DECL_WINDOWS_FUNCTION(static, BOOL, PlaySoundA, (LPCSTR, HMODULE, DWORD));

static void init_winfuncs(void)
{
    HMODULE user32_module = load_system32_dll("user32.dll");
    HMODULE winmm_module = load_system32_dll("winmm.dll");
    HMODULE shcore_module = load_system32_dll("shcore.dll");
    GET_WINDOWS_FUNCTION(user32_module, FlashWindowEx);
    GET_WINDOWS_FUNCTION(user32_module, ToUnicodeEx);
    GET_WINDOWS_FUNCTION(winmm_module, PlaySoundW);
    GET_WINDOWS_FUNCTION(winmm_module, PlaySoundA);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(user32_module, GetMonitorInfoA);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(user32_module, MonitorFromPoint);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(user32_module, MonitorFromWindow);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(shcore_module, GetDpiForMonitor);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(user32_module, GetSystemMetricsForDpi);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(user32_module, AdjustWindowRectExForDpi);
}

/*
 * Translate a WM_(SYS)?KEY(UP|DOWN) message into a string of ASCII
 * codes. Returns number of bytes used, zero to drop the message,
 * -1 to forward the message to Windows, or another negative number
 * to indicate a NUL-terminated "special" string.
 */
static int TranslateKey(WinGuiSeat *wgs, UINT message, WPARAM wParam,
                        LPARAM lParam, unsigned char *output)
{
    BYTE keystate[256];
    int scan, shift_state;
    bool left_alt = false, key_down;
    int r, i;
    unsigned char *p = output;
    int funky_type = conf_get_int(wgs->conf, CONF_funky_type);
    bool no_applic_k = conf_get_bool(wgs->conf, CONF_no_applic_k);
    bool ctrlaltkeys = conf_get_bool(wgs->conf, CONF_ctrlaltkeys);
    bool nethack_keypad = conf_get_bool(wgs->conf, CONF_nethack_keypad);
    char keypad_key = '\0';

    HKL kbd_layout = GetKeyboardLayout(0);

    r = GetKeyboardState(keystate);
    if (!r)
        memset(keystate, 0, sizeof(keystate));
    else {
#if 0
#define SHOW_TOASCII_RESULT
        {                              /* Tell us all about key events */
            static BYTE oldstate[256];
            static int first = 1;
            static int scan;
            int ch;
            if (first)
                memcpy(oldstate, keystate, sizeof(oldstate));
            first = 0;

            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) == KF_REPEAT) {
                debug("+");
            } else if ((HIWORD(lParam) & KF_UP)
                       && scan == (HIWORD(lParam) & 0xFF)) {
                debug(". U");
            } else {
                debug(".\n");
                if (wParam >= VK_F1 && wParam <= VK_F20)
                    debug("K_F%d", wParam + 1 - VK_F1);
                else
                    switch (wParam) {
                      case VK_SHIFT:
                        debug("SHIFT");
                        break;
                      case VK_CONTROL:
                        debug("CTRL");
                        break;
                      case VK_MENU:
                        debug("ALT");
                        break;
                      default:
                        debug("VK_%02x", wParam);
                    }
                if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
                    debug("*");
                debug(", S%02x", scan = (HIWORD(lParam) & 0xFF));

                ch = MapVirtualKeyEx(wParam, 2, kbd_layout);
                if (ch >= ' ' && ch <= '~')
                    debug(", '%c'", ch);
                else if (ch)
                    debug(", $%02x", ch);

                if ((keystate[VK_SHIFT] & 0x80) != 0)
                    debug(", S");
                if ((keystate[VK_CONTROL] & 0x80) != 0)
                    debug(", C");
                if ((HIWORD(lParam) & KF_EXTENDED))
                    debug(", E");
                if ((HIWORD(lParam) & KF_UP))
                    debug(", U");
            }

            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) == KF_REPEAT);
            else if ((HIWORD(lParam) & KF_UP))
                oldstate[wParam & 0xFF] ^= 0x80;
            else
                oldstate[wParam & 0xFF] ^= 0x81;

            for (ch = 0; ch < 256; ch++)
                if (oldstate[ch] != keystate[ch])
                    debug(", M%02x=%02x", ch, keystate[ch]);

            memcpy(oldstate, keystate, sizeof(oldstate));
        }
#endif

        if (wParam == VK_MENU && (HIWORD(lParam) & KF_EXTENDED)) {
            keystate[VK_RMENU] = keystate[VK_MENU];
        }

#ifdef MOD_DISABLEALTGR
        /* KiTTY "Disable AltGr": make the RIGHT Alt key behave like the left
         * one. The tests below (KF_ALTDOWN, and the key_down branch) treat a
         * keystroke as Alt only while right-Alt is NOT down, which is exactly
         * what lets an international layout compose AltGr+Q into "@". Clearing
         * the state here removes that exemption, so AltGr stops composing and
         * sends Alt+key instead - for people who want Alt shortcuts from either
         * Alt key. It is the off switch for AltGr, not a fix for it.
         *
         * The setting existed in this port - saved, loaded, with a checkbox -
         * but this line, its entire implementation, was never carried over, so
         * the box did nothing at all. */
        if (!GetPuttyFlag() && conf_get_int(wgs->conf, CONF_disablealtgr)) {
            keystate[VK_RMENU] = 0;
        }
#endif


        /* Nastiness with NUMLock - Shift-NUMLock is left alone though */
        if ((funky_type == FUNKY_VT400 ||
             (funky_type <= FUNKY_LINUX && wgs->term->app_keypad_keys &&
              !no_applic_k))
            && wParam == VK_NUMLOCK && !(keystate[VK_SHIFT] & 0x80)) {

            wParam = VK_EXECUTE;

            /* UnToggle NUMLock */
            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) == 0)
                keystate[VK_NUMLOCK] ^= 1;
        }

        /* And write back the 'adjusted' state */
        SetKeyboardState(keystate);
    }

    /* Disable Auto repeat if required */
    if (wgs->term->repeat_off &&
        (HIWORD(lParam) & (KF_UP | KF_REPEAT)) == KF_REPEAT)
        return 0;

    if ((HIWORD(lParam) & KF_ALTDOWN) && (keystate[VK_RMENU] & 0x80) == 0)
        left_alt = true;

    key_down = ((HIWORD(lParam) & KF_UP) == 0);

    /* Make sure Ctrl-ALT is not the same as AltGr for ToAscii unless told. */
    if (left_alt && (keystate[VK_CONTROL] & 0x80)) {
        if (ctrlaltkeys)
            keystate[VK_MENU] = 0;
        else {
            keystate[VK_RMENU] = 0x80;
            left_alt = false;
        }
    }

    scan = (HIWORD(lParam) & (KF_UP | KF_EXTENDED | 0xFF));
    shift_state = ((keystate[VK_SHIFT] & 0x80) != 0)
        + ((keystate[VK_CONTROL] & 0x80) != 0) * 2;

    /* Note if AltGr was pressed and if it was used as a compose key */
    if (!wgs->compose_state) {
        wgs->compose_keycode = 0x100;
        if (conf_get_bool(wgs->conf, CONF_compose_key)) {
            if (wParam == VK_MENU && (HIWORD(lParam) & KF_EXTENDED))
                wgs->compose_keycode = wParam;
        }
        if (wParam == VK_APPS)
            wgs->compose_keycode = wParam;
    }

    if (wParam == wgs->compose_keycode) {
        if (wgs->compose_state == 0 &&
            (HIWORD(lParam) & (KF_UP | KF_REPEAT)) == 0)
            wgs->compose_state = 1;
        else if (wgs->compose_state == 1 && (HIWORD(lParam) & KF_UP))
            wgs->compose_state = 2;
        else
            wgs->compose_state = 0;
    } else if (wgs->compose_state == 1 && wParam != VK_CONTROL)
        wgs->compose_state = 0;

    if (wgs->compose_state > 1 && left_alt)
        wgs->compose_state = 0;

    /* Sanitize the number pad if not using a PC NumPad */
    if (left_alt || (wgs->term->app_keypad_keys && !no_applic_k
                     && funky_type != FUNKY_XTERM) ||
        funky_type == FUNKY_VT400 || nethack_keypad || wgs->compose_state) {
        if ((HIWORD(lParam) & KF_EXTENDED) == 0) {
            int nParam = 0;
            switch (wParam) {
              case VK_INSERT:
                nParam = VK_NUMPAD0;
                break;
              case VK_END:
                nParam = VK_NUMPAD1;
                break;
              case VK_DOWN:
                nParam = VK_NUMPAD2;
                break;
              case VK_NEXT:
                nParam = VK_NUMPAD3;
                break;
              case VK_LEFT:
                nParam = VK_NUMPAD4;
                break;
              case VK_CLEAR:
                nParam = VK_NUMPAD5;
                break;
              case VK_RIGHT:
                nParam = VK_NUMPAD6;
                break;
              case VK_HOME:
                nParam = VK_NUMPAD7;
                break;
              case VK_UP:
                nParam = VK_NUMPAD8;
                break;
              case VK_PRIOR:
                nParam = VK_NUMPAD9;
                break;
              case VK_DELETE:
                nParam = VK_DECIMAL;
                break;
            }
            if (nParam) {
                if (keystate[VK_NUMLOCK] & 1)
                    shift_state |= 1;
                wParam = nParam;
            }
        }
    }

    /* If a key is pressed and AltGr is not active */
    if (key_down && (keystate[VK_RMENU] & 0x80) == 0 && !wgs->compose_state) {
        /* Okay, prepare for most alts then ... */
        if (left_alt)
            *p++ = '\033';

        /* Lets see if it's a pattern we know all about ... */
        if (wParam == VK_PRIOR && shift_state == 1) {
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_PAGEUP, 0);
            return 0;
        }
        if (wParam == VK_PRIOR && shift_state == 3) { /* ctrl-shift-pageup */
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_TOP, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 3) { /* ctrl-shift-pagedown */
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_BOTTOM, 0);
            return 0;
        }

        if (wParam == VK_PRIOR && shift_state == 2) {
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_LINEUP, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 1) {
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_PAGEDOWN, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 2) {
            SendMessage(wgs->term_hwnd, WM_VSCROLL, SB_LINEDOWN, 0);
            return 0;
        }
        if ((wParam == VK_PRIOR || wParam == VK_NEXT) && shift_state == 3) {
            term_scroll_to_selection(wgs->term, (wParam == VK_PRIOR ? 0 : 1));
            return 0;
        }
        if (wParam == VK_INSERT && shift_state == 2) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftins)) {
              case CLIPUI_IMPLICIT:
                break;          /* no need to re-copy to CLIP_LOCAL */
              case CLIPUI_EXPLICIT:
                term_request_copy(wgs->term, clips_system,
                                  lenof(clips_system));
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == VK_INSERT && shift_state == 1) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftins)) {
              case CLIPUI_IMPLICIT:
                term_request_paste(wgs->term, CLIP_LOCAL);
                break;
              case CLIPUI_EXPLICIT:
                term_request_paste(wgs->term, CLIP_SYSTEM);
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == 'C' && shift_state == 3) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftcv)) {
              case CLIPUI_IMPLICIT:
                break;          /* no need to re-copy to CLIP_LOCAL */
              case CLIPUI_EXPLICIT:
                term_request_copy(wgs->term, clips_system,
                                  lenof(clips_system));
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == 'V' && shift_state == 3) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftcv)) {
              case CLIPUI_IMPLICIT:
                term_request_paste(wgs->term, CLIP_LOCAL);
                break;
              case CLIPUI_EXPLICIT:
                term_request_paste(wgs->term, CLIP_SYSTEM);
                break;
              default:
                break;
            }
            return 0;
        }
        if (left_alt && wParam == VK_F4 &&
            conf_get_bool(wgs->conf, CONF_alt_f4)) {
            return -1;
        }
        if (left_alt && wParam == VK_SPACE &&
            conf_get_bool(wgs->conf, CONF_alt_space)) {
            SendMessage(wgs->term_hwnd, WM_SYSCOMMAND, SC_KEYMENU, 0);
            return -1;
        }
        if (left_alt && wParam == VK_RETURN &&
            conf_get_bool(wgs->conf, CONF_fullscreenonaltenter) &&
            (conf_get_int(wgs->conf, CONF_resize_action) != RESIZE_DISABLED)) {
            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) != KF_REPEAT)
                flip_full_screen(wgs);
            return -1;
        }
        /* Control-Numlock for app-keypad mode switch */
        if (wParam == VK_PAUSE && shift_state == 2) {
            wgs->term->app_keypad_keys = !wgs->term->app_keypad_keys;
            return 0;
        }

        if (wParam == VK_BACK && shift_state == 0) {    /* Backspace */
            *p++ = (conf_get_bool(wgs->conf, CONF_bksp_is_delete) ?
                    0x7F : 0x08);
            *p++ = 0;
            return -2;
        }
        if (wParam == VK_BACK && shift_state == 1) {    /* Shift Backspace */
            /* We do the opposite of what is configured */
            *p++ = (conf_get_bool(wgs->conf, CONF_bksp_is_delete) ?
                    0x08 : 0x7F);
            *p++ = 0;
            return -2;
        }
        if (wParam == VK_TAB && shift_state == 1) {     /* Shift tab */
            *p++ = 0x1B;
            *p++ = '[';
            *p++ = 'Z';
            return p - output;
        }
#ifdef MOD_PERSO
        /* Tab with Ctrl held produces no WM_CHAR, so without an explicit
         * mapping the keystroke is swallowed; send the xterm modifyOtherKeys
         * encoding like classic KiTTY (hknet/KiTTY#15). Only reached when
         * CtrlTabSwitch is off - window switching consumes the key first. */
        if (!GetPuttyFlag() && wParam == VK_TAB && shift_state == 2) {
            p += sprintf((char *)p, "\x1b[27;5;9~");   /* Ctrl-Tab */
            return p - output;
        }
        if (!GetPuttyFlag() && wParam == VK_TAB && shift_state == 3) {
            p += sprintf((char *)p, "\x1b[27;6;9~");   /* Ctrl-Shift-Tab */
            return p - output;
        }
#endif
        if (wParam == VK_SPACE && shift_state == 2) {   /* Ctrl-Space */
            *p++ = 0;
            return p - output;
        }
        if (wParam == VK_SPACE && shift_state == 3) {   /* Ctrl-Shift-Space */
            *p++ = 160;
            return p - output;
        }
        if (wParam == VK_CANCEL && shift_state == 2) {  /* Ctrl-Break */
            if (wgs->backend)
                backend_special(wgs->backend, SS_BRK, 0);
            return 0;
        }
        if (wParam == VK_PAUSE) {      /* Break/Pause */
            *p++ = 26;
            *p++ = 0;
            return -2;
        }
        /* Control-2 to Control-8 are special */
        if (shift_state == 2 && wParam >= '2' && wParam <= '8') {
            *p++ = "\000\033\034\035\036\037\177"[wParam - '2'];
            return p - output;
        }
        if (shift_state == 2 && (wParam == 0xBD || wParam == 0xBF)) {
            *p++ = 0x1F;
            return p - output;
        }
        if (shift_state == 2 && (wParam == 0xDF || wParam == 0xDC)) {
            *p++ = 0x1C;
            return p - output;
        }
        if (shift_state == 3 && wParam == 0xDE) {
            *p++ = 0x1E;               /* Ctrl-~ == Ctrl-^ in xterm at least */
            return p - output;
        }

        switch (wParam) {
            bool consumed_alt;

          case VK_NUMPAD0: keypad_key = '0'; goto numeric_keypad;
          case VK_NUMPAD1: keypad_key = '1'; goto numeric_keypad;
          case VK_NUMPAD2: keypad_key = '2'; goto numeric_keypad;
          case VK_NUMPAD3: keypad_key = '3'; goto numeric_keypad;
          case VK_NUMPAD4: keypad_key = '4'; goto numeric_keypad;
          case VK_NUMPAD5: keypad_key = '5'; goto numeric_keypad;
          case VK_NUMPAD6: keypad_key = '6'; goto numeric_keypad;
          case VK_NUMPAD7: keypad_key = '7'; goto numeric_keypad;
          case VK_NUMPAD8: keypad_key = '8'; goto numeric_keypad;
          case VK_NUMPAD9: keypad_key = '9'; goto numeric_keypad;
          case VK_DECIMAL: keypad_key = '.'; goto numeric_keypad;
          case VK_ADD: keypad_key = '+'; goto numeric_keypad;
          case VK_SUBTRACT: keypad_key = '-'; goto numeric_keypad;
          case VK_MULTIPLY: keypad_key = '*'; goto numeric_keypad;
          case VK_DIVIDE: keypad_key = '/'; goto numeric_keypad;
          case VK_EXECUTE: keypad_key = 'G'; goto numeric_keypad;
            /* also the case for VK_RETURN below can sometimes come here */
          numeric_keypad:
            /* Left Alt overrides all numeric keypad usage to act as
             * numeric character code input */
            if (left_alt) {
                if (keypad_key >= '0' && keypad_key <= '9')
                    wgs->alt_numberpad_accumulator =
                        wgs->alt_numberpad_accumulator * 10 + keypad_key - '0';
                else
                    wgs->alt_numberpad_accumulator = 0;
                break;
            }

            {
                int nchars = format_numeric_keypad_key(
                    (char *)p, wgs->term, keypad_key,
                    shift_state & 1, shift_state & 2);
                if (!nchars) {
                    /*
                     * If we didn't get an escape sequence out of the
                     * numeric keypad key, then that must be because
                     * we're in Num Lock mode without application
                     * keypad enabled. In that situation we leave this
                     * keypress to the ToUnicode/ToAsciiEx handler
                     * below, which will translate it according to the
                     * appropriate keypad layout (e.g. so that what a
                     * Brit thinks of as keypad '.' can become ',' in
                     * the German layout).
                     *
                     * An exception is the keypad Return key: if we
                     * didn't get an escape sequence for that, we
                     * treat it like ordinary Return, taking into
                     * account Telnet special new line codes and
                     * config options.
                     */
                    if (keypad_key == '\r')
                        goto ordinary_return_key;
                    break;
                }

                p += nchars;
                return p - output;
            }

            int fkey_number;
          case VK_F1: fkey_number = 1; goto numbered_function_key;
          case VK_F2: fkey_number = 2; goto numbered_function_key;
          case VK_F3: fkey_number = 3; goto numbered_function_key;
          case VK_F4: fkey_number = 4; goto numbered_function_key;
          case VK_F5: fkey_number = 5; goto numbered_function_key;
          case VK_F6: fkey_number = 6; goto numbered_function_key;
          case VK_F7: fkey_number = 7; goto numbered_function_key;
          case VK_F8: fkey_number = 8; goto numbered_function_key;
          case VK_F9: fkey_number = 9; goto numbered_function_key;
          case VK_F10: fkey_number = 10; goto numbered_function_key;
          case VK_F11: fkey_number = 11; goto numbered_function_key;
          case VK_F12: fkey_number = 12; goto numbered_function_key;
          case VK_F13: fkey_number = 13; goto numbered_function_key;
          case VK_F14: fkey_number = 14; goto numbered_function_key;
          case VK_F15: fkey_number = 15; goto numbered_function_key;
          case VK_F16: fkey_number = 16; goto numbered_function_key;
          case VK_F17: fkey_number = 17; goto numbered_function_key;
          case VK_F18: fkey_number = 18; goto numbered_function_key;
          case VK_F19: fkey_number = 19; goto numbered_function_key;
          case VK_F20: fkey_number = 20; goto numbered_function_key;
          numbered_function_key:
            consumed_alt = false;
            p += format_function_key((char *)p, wgs->term, fkey_number,
                                     shift_state & 1, shift_state & 2,
                                     left_alt, &consumed_alt);
            if (consumed_alt) {
                /* supersedes the usual prefixing of Esc */
                p -= 1;
                memmove(output, output + 1, p - output);
            }
            return p - output;

            SmallKeypadKey sk_key;
          case VK_HOME: sk_key = SKK_HOME; goto small_keypad_key;
          case VK_END: sk_key = SKK_END; goto small_keypad_key;
          case VK_INSERT: sk_key = SKK_INSERT; goto small_keypad_key;
          case VK_DELETE: sk_key = SKK_DELETE; goto small_keypad_key;
          case VK_PRIOR: sk_key = SKK_PGUP; goto small_keypad_key;
          case VK_NEXT: sk_key = SKK_PGDN; goto small_keypad_key;
          small_keypad_key:
            /* These keys don't generate terminal input with Ctrl */
            if (shift_state & 2)
                break;

            consumed_alt = false;
            p += format_small_keypad_key((char *)p, wgs->term, sk_key,
                                         shift_state & 1, shift_state & 2,
                                         left_alt, &consumed_alt);
            if (consumed_alt) {
                /* supersedes the usual prefixing of Esc */
                p -= 1;
                memmove(output, output + 1, p - output);
            }
            return p - output;

            char xkey;
          case VK_UP: xkey = 'A'; goto arrow_key;
          case VK_DOWN: xkey = 'B'; goto arrow_key;
          case VK_RIGHT: xkey = 'C'; goto arrow_key;
          case VK_LEFT: xkey = 'D'; goto arrow_key;
          case VK_CLEAR: xkey = 'G'; goto arrow_key; /* close enough */
          arrow_key:
            consumed_alt = false;
            {
                bool a_shift = (shift_state & 1) != 0;
                bool a_ctrl  = (shift_state & 2) != 0;
                bool a_alt   = left_alt;
                /*
                 * KiTTY: optionally route the Alt-style "word navigation"
                 * sequence (ESC[1;3 D/C, which shells bind to back/forward-word)
                 * onto Ctrl and/or Alt for the Left/Right arrows. Only meaningful
                 * in xterm-bitmap arrow mode and not in VT52 (where the modifier
                 * isn't encoded), so those are left untouched.
                 */
                if ((xkey == 'C' || xkey == 'D') && !wgs->term->vt52_mode &&
                    wgs->term->sharrow_type == SHARROW_BITMAP) {
                    switch (conf_get_int(wgs->conf, CONF_word_nav_modifier)) {
                      case WORDNAV_CTRL:        /* swap Ctrl <-> Alt */
                        { bool t = a_ctrl; a_ctrl = a_alt; a_alt = t; }
                        break;
                      case WORDNAV_BOTH:        /* either modifier -> word-nav */
                        if (a_ctrl || a_alt) { a_ctrl = false; a_alt = true; }
                        break;
                      /* WORDNAV_ALT: default PuTTY behaviour, no change */
                    }
                }
                p += format_arrow_key((char *)p, wgs->term, xkey,
                                      a_shift, a_ctrl, a_alt, &consumed_alt);
                /*
                 * The leading ESC at output[0] exists iff REAL Alt was down (see
                 * `if (left_alt) *p++='\033'` earlier). Strip it only when Alt was
                 * consumed into the bitmap AND really down, so a faked Alt (e.g.
                 * Ctrl-as-word-nav with real Alt up) never strips a byte that was
                 * never prefixed.
                 */
                if (consumed_alt && left_alt) {
                    /* supersedes the usual prefixing of Esc */
                    p -= 1;
                    memmove(output, output + 1, p - output);
                }
            }
            return p - output;

          case VK_RETURN:
            if (HIWORD(lParam) & KF_EXTENDED) {
                keypad_key = '\r';
                goto numeric_keypad;
            }
          ordinary_return_key:
            if (shift_state == 0 && wgs->term->cr_lf_return) {
                *p++ = '\r';
                *p++ = '\n';
                return p - output;
            } else {
                *p++ = 0x0D;
                *p++ = 0;
                return -2;
            }
        }
    }

    /* Okay we've done everything interesting; let windows deal with
     * the boring stuff */
    {
        bool capsOn = false;

        /* helg: clear CAPS LOCK state if caps lock switches to cyrillic */
        if (keystate[VK_CAPITAL] != 0 &&
            conf_get_bool(wgs->conf, CONF_xlat_capslockcyr)) {
            capsOn = !left_alt;
            keystate[VK_CAPITAL] = 0;
        }

        wchar_t keys_unicode[3];

        /* XXX how do we know what the max size of the keys array should
         * be is? There's indication on MS' website of an Inquire/InquireEx
         * functioning returning a KBINFO structure which tells us. */
        if (osPlatformId == VER_PLATFORM_WIN32_NT && p_ToUnicodeEx) {
            r = p_ToUnicodeEx(wParam, scan, keystate, keys_unicode,
                              lenof(keys_unicode), 0, kbd_layout);
        } else {
            /* XXX 'keys' parameter is declared in MSDN documentation as
             * 'LPWORD lpChar'.
             * The experience of a French user indicates that on
             * Win98, WORD[] should be passed in, but on Win2K, it should
             * be BYTE[]. German WinXP and my Win2K with "US International"
             * driver corroborate this.
             * Experimentally I've conditionalised the behaviour on the
             * Win9x/NT split, but I suspect it's worse than that.
             * See wishlist item `win-dead-keys' for more horrible detail
             * and speculations. */
            int i;
            WORD keys[3];
            BYTE keysb[3];
            r = ToAsciiEx(wParam, scan, keystate, keys, 0, kbd_layout);
            if (r > 0) {
                for (i = 0; i < r; i++) {
                    keysb[i] = (BYTE)keys[i];
                }
                MultiByteToWideChar(CP_ACP, 0, (LPCSTR)keysb, r,
                                    keys_unicode, lenof(keys_unicode));
            }
        }
#ifdef SHOW_TOASCII_RESULT
        if (r == 1 && !key_down) {
            if (wgs->alt_numberpad_accumulator) {
                if (in_utf(term) || ucsdata.dbcs_screenfont)
                    debug(", (U+%04x)", wgs->alt_numberpad_accumulator);
                else
                    debug(", LCH(%d)", wgs->alt_numberpad_accumulator);
            } else {
                debug(", ACH(%d)", keys_unicode[0]);
            }
        } else if (r > 0) {
            int r1;
            debug(", ASC(");
            for (r1 = 0; r1 < r; r1++) {
                debug("%s%d", r1 ? "," : "", keys_unicode[r1]);
            }
            debug(")");
        }
#endif
        if (r > 0) {
            WCHAR keybuf;

            p = output;
            for (i = 0; i < r; i++) {
                wchar_t wch = keys_unicode[i];

                if (wgs->compose_state == 2 && wch >= ' ' && wch < 0x80) {
                    wgs->compose_char = wch;
                    wgs->compose_state++;
                    continue;
                }
                if (wgs->compose_state == 3 && wch >= ' ' && wch < 0x80) {
                    int nc;
                    wgs->compose_state = 0;

                    if ((nc = check_compose(wgs->compose_char, wch)) == -1) {
                        MessageBeep(MB_ICONHAND);
                        return 0;
                    }
                    keybuf = nc;
                    term_keyinputw(wgs->term, &keybuf, 1);
                    continue;
                }

                wgs->compose_state = 0;

                if (!key_down) {
                    if (wgs->alt_numberpad_accumulator) {
                        if (in_utf(wgs->term) ||
                            wgs->ucsdata.dbcs_screenfont) {
                            keybuf = wgs->alt_numberpad_accumulator;
                            term_keyinputw(wgs->term, &keybuf, 1);
                        } else {
                            char ch = (char) wgs->alt_numberpad_accumulator;
                            /*
                             * We need not bother about stdin
                             * backlogs here, because in GUI PuTTY
                             * we can't do anything about it
                             * anyway; there's no means of asking
                             * Windows to hold off on KEYDOWN
                             * messages. We _have_ to buffer
                             * everything we're sent.
                             */
                            term_keyinput(wgs->term, -1, &ch, 1);
                        }
                        wgs->alt_numberpad_accumulator = 0;
                    } else {
                        term_keyinputw(wgs->term, &wch, 1);
                    }
                } else {
                    if (capsOn && wch < 0x80) {
                        WCHAR cbuf[2];
                        cbuf[0] = 27;
                        cbuf[1] = xlat_uskbd2cyrllic(wch);
                        term_keyinputw(
                            wgs->term, cbuf+!left_alt, 1+!!left_alt);
                    } else {
                        WCHAR cbuf[2];
                        cbuf[0] = '\033';
                        cbuf[1] = wch;
                        term_keyinputw(
                            wgs->term, cbuf +!left_alt, 1+!!left_alt);
                    }
                }
                show_mouseptr(wgs, false);
            }

            return p - output;
        }
    }

    /*
     * ALT alone may or may not want to bring up the System menu.
     * If it's not meant to, we return 0 on presses or releases of
     * ALT, to show that we've swallowed the keystroke. Otherwise
     * we return -1, which means Windows will give the keystroke
     * its default handling (i.e. bring up the System menu).
     */
    if (wParam == VK_MENU && !conf_get_bool(wgs->conf, CONF_alt_only))
        return 0;

    return -1;
}

#ifdef MOD_PERSO
/* KiTTY [KiTTY] wintitle=yes: decorate every window title with live status
 * markers - the [rows x cols] size suffix ([KiTTY] size=yes, skipped while
 * maximized) and (PROTECTED)/(ONTOP). The raw title is remembered so
 * kitty_refresh_title() can re-decorate when a state changes (resize, protect
 * or always-on-top toggle). SECURITY: unlike classic KiTTY the title text is
 * never PARSED here - the old __xy title-scan dispatcher stays dead and
 * decoration is strictly one-way output. */
static char *kitty_raw_title = NULL;
static int kitty_raw_title_cp = DEFAULT_CODEPAGE;

static char *kitty_decorate_title(WinGuiSeat *wgs, const char *title)
{
    strbuf *sb;
    if (!GetTitleBarFlag())
        return dupstr(title);
    sb = strbuf_new();
    put_dataz(sb, title);
    if (GetSizeFlag() && wgs->term && !IsZoomed(wgs->term_hwnd))
        put_fmt(sb, " [%dx%d]", wgs->term->rows, wgs->term->cols);
    if (GetProtectFlag())
        put_dataz(sb, " (PROTECTED)");
    if (conf_get_bool(wgs->conf, CONF_alwaysontop))
        put_dataz(sb, " (ONTOP)");
    /* KiTTY: this process runs with the restricted ACL (-restrict-acl, "&R"
     * from a parent, or [KiTTY] restrictacl=yes). Unlike its neighbours the
     * state cannot change after startup, so it needs no kitty_refresh_title()
     * plumbing - but it is worth showing, because a restrictacl= line in the
     * wrong kitty.ini leaves the user believing they are hardened in silence. */
    if (restricted_acl())
        put_dataz(sb, " (RESTRICTED)");
    /*
     * KiTTY: a clipboard permission is live. Agreeing to be read once is not
     * agreeing to be read invisibly from then on, so while the permission lasts it
     * is on the window.
     *
     * "(paused)" is the case that earns this its keep. Losing focus suspends the
     * permission, and losing focus is exactly when the user CAN read the title -
     * they are looking at something in front of this window. A permission that
     * silently stopped applying would be worse than none, because they would have
     * no idea why the editor had stopped seeing pastes.
     *
     * Short markers, and at the end: the connection name has to survive the
     * taskbar cutting the title off, so it keeps the first characters.
     */
    /* The clipboard markers are NOT added here. They are Unicode, and this
     * function works on the session's raw title in whatever codepage it arrived
     * in - appending UTF-8 to a codepage-1252 title mangles both. They go on in
     * wintw_set_title instead, after the conversion to wide characters, where
     * there is no codepage left to get wrong. */
    return strbuf_to_str(sb);
}

/* Re-apply the decorations to the last raw title (called after a state
 * change). The set-title path dedupes, so this is cheap when nothing moved. */
void kitty_refresh_title(void)
{
    WinGuiSeat *wgs;
    if (!MainHwnd || !kitty_raw_title) return;
    wgs = (WinGuiSeat *)GetWindowLongPtr(MainHwnd, GWLP_USERDATA);
    if (!wgs) return;
    win_set_title(&wgs->termwin, kitty_raw_title, kitty_raw_title_cp);
}
#endif

static void wintw_set_title(TermWin *tw, const char *title, int codepage)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
#ifdef MOD_PERSO
    wchar_t *new_window_name;
    {
        char *decorated;
        if (kitty_raw_title != title) {   /* self-alias via kitty_refresh_title */
            sfree(kitty_raw_title);
            kitty_raw_title = dupstr(title);
            kitty_raw_title_cp = codepage;
        }
        decorated = kitty_decorate_title(wgs, kitty_raw_title);
        new_window_name = dup_mb_to_wc(codepage, decorated);
        sfree(decorated);
        /* Clipboard markers go on AFTER the codepage conversion: they are Unicode,
         * and the title itself may have arrived in any codepage. */
        new_window_name = kitty_clip_decorate_wide(wgs, new_window_name);
    }
#else
    wchar_t *new_window_name = dup_mb_to_wc(codepage, title);
#endif
    if (!wcscmp(new_window_name, wgs->window_name)) {
        sfree(new_window_name);
        return;
    }
    sfree(wgs->window_name);
    wgs->window_name = new_window_name;
    if (conf_get_bool(wgs->conf, CONF_win_name_always) ||
        !IsIconic(wgs->term_hwnd))
        sw_SetWindowText(wgs->term_hwnd, wgs->window_name);
}

static void wintw_set_icon_title(TermWin *tw, const char *title, int codepage)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    wchar_t *new_icon_name = dup_mb_to_wc(codepage, title);
    if (!wcscmp(new_icon_name, wgs->icon_name)) {
        sfree(new_icon_name);
        return;
    }
    sfree(wgs->icon_name);
    wgs->icon_name = new_icon_name;
    if (!conf_get_bool(wgs->conf, CONF_win_name_always) &&
        IsIconic(wgs->term_hwnd))
        sw_SetWindowText(wgs->term_hwnd, wgs->icon_name);
}

static void wintw_set_scrollbar(TermWin *tw, int total, int start, int page)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    SCROLLINFO si;

    if (!conf_get_bool(wgs->conf, is_full_screen(wgs) ?
                       CONF_scrollbar_in_fullscreen : CONF_scrollbar))
        return;

    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = total - 1;
    si.nPage = page;
    si.nPos = start;
    if (wgs->term_hwnd)
        SetScrollInfo(wgs->term_hwnd, SB_VERT, &si, true);
}

static bool wintw_setup_draw_ctx(TermWin *tw)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    assert(!wgs->wintw_hdc);
    wgs->wintw_hdc = make_hdc(wgs);
#ifdef MOD_PERSO
    /* Keep URL link regions current at paint time: content changes always
     * repaint, so one scan per repaint burst keeps hover/click hit-tests and
     * the hyperlink underline current.  This is the only rescan driver (the
     * mouse-move path only hit-tests), so it must run even with underlining
     * off; repainting the changed rows only matters when underlines are
     * drawn. */
    if (wgs->wintw_hdc && GetHyperlinkFlag()) {
        if (kitty_url_rescan(wgs->term) &&
            conf_get_int(wgs->conf, CONF_url_underline))
            kitty_url_invalidate_dirty_rows(wgs);
    }
#endif
    return wgs->wintw_hdc != NULL;
}

static void wintw_free_draw_ctx(TermWin *tw)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    assert(wgs->wintw_hdc);
    free_hdc(wgs, wgs->wintw_hdc);
    wgs->wintw_hdc = NULL;
}

/*
 * Set up the colour palette.
 */
static void init_palette(WinGuiSeat *wgs)
{
    wgs->pal = NULL;
    wgs->logpal = snew_plus(
        LOGPALETTE, (OSC4_NCOLOURS - 1) * sizeof(PALETTEENTRY));
    wgs->logpal->palVersion = 0x300;
    wgs->logpal->palNumEntries = OSC4_NCOLOURS;
    for (unsigned i = 0; i < OSC4_NCOLOURS; i++)
        wgs->logpal->palPalEntry[i].peFlags = PC_NOCOLLAPSE;
}

static void wintw_palette_set(TermWin *tw, unsigned start,
                              unsigned ncolours, const rgb *colours_in)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    assert(start <= OSC4_NCOLOURS);
    assert(ncolours <= OSC4_NCOLOURS - start);

    for (unsigned i = 0; i < ncolours; i++) {
        const rgb *in = &colours_in[i];
        PALETTEENTRY *out = &wgs->logpal->palPalEntry[i + start];
        out->peRed = in->r;
        out->peGreen = in->g;
        out->peBlue = in->b;
        wgs->colours[i + start] =
            RGB(in->r, in->g, in->b) ^ wgs->colorref_modifier;
    }

    bool got_new_palette = false;

    if (!wgs->tried_pal && conf_get_bool(wgs->conf, CONF_try_palette)) {
        HDC hdc = GetDC(wgs->term_hwnd);
        if (GetDeviceCaps(hdc, RASTERCAPS) & RC_PALETTE) {
            wgs->pal = CreatePalette(wgs->logpal);
            if (wgs->pal) {
                SelectPalette(hdc, wgs->pal, false);
                RealizePalette(hdc);
                SelectPalette(hdc, GetStockObject(DEFAULT_PALETTE), false);

                /* Convert all RGB() values in colours[] into PALETTERGB(),
                 * and ensure we stick to that later */
                wgs->colorref_modifier = PALETTERGB(0, 0, 0) ^ RGB(0, 0, 0);
                for (unsigned i = 0; i < OSC4_NCOLOURS; i++)
                    wgs->colours[i] ^= wgs->colorref_modifier;

                /* Inhibit the SetPaletteEntries call below */
                got_new_palette = true;
            }
        }
        ReleaseDC(wgs->term_hwnd, hdc);
        wgs->tried_pal = true;
    }

    if (wgs->pal && !got_new_palette) {
        /* We already had a palette, so replace the changed colours in the
         * existing one. */
        SetPaletteEntries(wgs->pal, start, ncolours,
                          wgs->logpal->palPalEntry + start);

        HDC hdc = make_hdc(wgs);
        UnrealizeObject(wgs->pal);
        RealizePalette(hdc);
        free_hdc(wgs, hdc);
    }

    if (start <= OSC4_COLOUR_bg && OSC4_COLOUR_bg < start + ncolours) {
        /* If Default Background changes, we need to ensure any space between
         * the text area and the window border is redrawn. */
        InvalidateRect(wgs->term_hwnd, NULL, true);
    }
}

void write_aclip(HWND hwnd, int clipboard, char *data, int len)
{
    HGLOBAL clipdata;
    void *lock;

    if (clipboard != CLIP_SYSTEM)
        return;

    clipdata = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, len + 1);
    if (!clipdata)
        return;
    lock = GlobalLock(clipdata);
    if (!lock)
        return;
    memcpy(lock, data, len);
    ((unsigned char *) lock)[len] = 0;
    GlobalUnlock(clipdata);

    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, clipdata);
        CloseClipboard();
    } else
        GlobalFree(clipdata);
}

typedef struct _rgbindex {
    int index;
    COLORREF ref;
} rgbindex;

int cmpCOLORREF(void *va, void *vb)
{
    COLORREF a = ((rgbindex *)va)->ref;
    COLORREF b = ((rgbindex *)vb)->ref;
    return (a < b) ? -1 : (a > b) ? +1 : 0;
}

/*
 * Note: unlike write_aclip() this will not append a nul.
 */
static void wintw_clip_write(
    TermWin *tw, int clipboard, wchar_t *data, int *attr,
    truecolour *truecolour, int len, bool must_deselect)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    HGLOBAL clipdata, clipdata2, clipdata3;
    int len2;
    void *lock, *lock2, *lock3;

    if (clipboard != CLIP_SYSTEM)
        return;

    len2 = WideCharToMultiByte(CP_ACP, 0, data, len, 0, 0, NULL, NULL);

    clipdata = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE,
                           len * sizeof(wchar_t));
    clipdata2 = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, len2);

    if (!clipdata || !clipdata2) {
        if (clipdata)
            GlobalFree(clipdata);
        if (clipdata2)
            GlobalFree(clipdata2);
        return;
    }
    if (!(lock = GlobalLock(clipdata))) {
        GlobalFree(clipdata);
        GlobalFree(clipdata2);
        return;
    }
    if (!(lock2 = GlobalLock(clipdata2))) {
        GlobalUnlock(clipdata);
        GlobalFree(clipdata);
        GlobalFree(clipdata2);
        return;
    }

    memcpy(lock, data, len * sizeof(wchar_t));
    WideCharToMultiByte(CP_ACP, 0, data, len, lock2, len2, NULL, NULL);

    if (conf_get_bool(wgs->conf, CONF_rtf_paste)) {
        wchar_t unitab[256];
        strbuf *rtf = strbuf_new();
        unsigned char *tdata = (unsigned char *)lock2;
        wchar_t *udata = (wchar_t *)lock;
        int uindex = 0, tindex = 0;
        int multilen, blen, alen, i;
        char before[16], after[4];
        int fgcolour,  lastfgcolour  = -1;
        int bgcolour,  lastbgcolour  = -1;
        COLORREF fg,   lastfg = -1;
        COLORREF bg,   lastbg = -1;
        int attrBold,  lastAttrBold  = 0;
        int attrUnder, lastAttrUnder = 0;
        int palette[OSC4_NCOLOURS];
        int numcolours;
        tree234 *rgbtree = NULL;
        FontSpec *font = conf_get_fontspec(wgs->conf, CONF_font);

        get_unitab(CP_ACP, unitab, 0);

        put_fmt(
            rtf, "{\\rtf1\\ansi\\deff0{\\fonttbl\\f0\\fmodern %s;}\\f0\\fs%d",
            font->name, font->height*2);

        /*
         * Add colour palette
         * {\colortbl ;\red255\green0\blue0;\red0\green0\blue128;}
         */

        /*
         * First - Determine all colours in use
         *    o  Foregound and background colours share the same palette
         */
        if (attr) {
            memset(palette, 0, sizeof(palette));
            for (i = 0; i < (len-1); i++) {
                fgcolour = ((attr[i] & ATTR_FGMASK) >> ATTR_FGSHIFT);
                bgcolour = ((attr[i] & ATTR_BGMASK) >> ATTR_BGSHIFT);

                if (attr[i] & ATTR_REVERSE) {
                    int tmpcolour = fgcolour;   /* Swap foreground and background */
                    fgcolour = bgcolour;
                    bgcolour = tmpcolour;
                }

                if (wgs->bold_colours && (attr[i] & ATTR_BOLD)) {
                    if (fgcolour  <   8)        /* ANSI colours */
                        fgcolour +=   8;
                    else if (fgcolour >= 256)   /* Default colours */
                        fgcolour ++;
                }

                if ((attr[i] & ATTR_BLINK)) {
                    if (bgcolour  <   8)        /* ANSI colours */
                        bgcolour +=   8;
                    else if (bgcolour >= 256)   /* Default colours */
                        bgcolour ++;
                }

                palette[fgcolour]++;
                palette[bgcolour]++;
            }

            if (truecolour) {
                rgbtree = newtree234(cmpCOLORREF);
                for (i = 0; i < (len-1); i++) {
                    if (truecolour[i].fg.enabled) {
                        rgbindex *rgbp = snew(rgbindex);
                        rgbp->ref = RGB(truecolour[i].fg.r,
                                        truecolour[i].fg.g,
                                        truecolour[i].fg.b);
                        if (add234(rgbtree, rgbp) != rgbp)
                            sfree(rgbp);
                    }
                    if (truecolour[i].bg.enabled) {
                        rgbindex *rgbp = snew(rgbindex);
                        rgbp->ref = RGB(truecolour[i].bg.r,
                                        truecolour[i].bg.g,
                                        truecolour[i].bg.b);
                        if (add234(rgbtree, rgbp) != rgbp)
                            sfree(rgbp);
                    }
                }
            }

            /*
             * Next - Create a reduced palette
             */
            numcolours = 0;
            for (i = 0; i < OSC4_NCOLOURS; i++) {
                if (palette[i] != 0)
                    palette[i]  = ++numcolours;
            }

            if (rgbtree) {
                rgbindex *rgbp;
                for (i = 0; (rgbp = index234(rgbtree, i)) != NULL; i++)
                    rgbp->index = ++numcolours;
            }

            /*
             * Finally - Write the colour table
             */
            put_datapl(rtf, PTRLEN_LITERAL("{\\colortbl ;"));

            for (i = 0; i < OSC4_NCOLOURS; i++) {
                if (palette[i] != 0) {
                    const PALETTEENTRY *pe = &wgs->logpal->palPalEntry[i];
                    put_fmt(rtf, "\\red%d\\green%d\\blue%d;",
                            pe->peRed, pe->peGreen, pe->peBlue);
                }
            }
            if (rgbtree) {
                rgbindex *rgbp;
                for (i = 0; (rgbp = index234(rgbtree, i)) != NULL; i++)
                    put_fmt(rtf, "\\red%d\\green%d\\blue%d;",
                            GetRValue(rgbp->ref), GetGValue(rgbp->ref),
                            GetBValue(rgbp->ref));
            }
            put_datapl(rtf, PTRLEN_LITERAL("}"));
        }

        /*
         * We want to construct a piece of RTF that specifies the
         * same Unicode text. To do this we will read back in
         * parallel from the Unicode data in `udata' and the
         * non-Unicode data in `tdata'. For each character in
         * `tdata' which becomes the right thing in `udata' when
         * looked up in `unitab', we just copy straight over from
         * tdata. For each one that doesn't, we must WCToMB it
         * individually and produce a \u escape sequence.
         *
         * It would probably be more robust to just bite the bullet
         * and WCToMB each individual Unicode character one by one,
         * then MBToWC each one back to see if it was an accurate
         * translation; but that strikes me as a horrifying number
         * of Windows API calls so I want to see if this faster way
         * will work. If it screws up badly we can always revert to
         * the simple and slow way.
         */
        while (tindex < len2 && uindex < len &&
               tdata[tindex] && udata[uindex]) {
            if (tindex + 1 < len2 &&
                tdata[tindex] == '\r' &&
                tdata[tindex+1] == '\n') {
                tindex++;
                uindex++;
            }

            /*
             * Set text attributes
             */
            if (attr) {
                /*
                 * Determine foreground and background colours
                 */
                if (truecolour && truecolour[tindex].fg.enabled) {
                    fgcolour = -1;
                    fg = RGB(truecolour[tindex].fg.r,
                             truecolour[tindex].fg.g,
                             truecolour[tindex].fg.b);
                } else {
                    fgcolour = ((attr[tindex] & ATTR_FGMASK) >> ATTR_FGSHIFT);
                    fg = -1;
                }

                if (truecolour && truecolour[tindex].bg.enabled) {
                    bgcolour = -1;
                    bg = RGB(truecolour[tindex].bg.r,
                             truecolour[tindex].bg.g,
                             truecolour[tindex].bg.b);
                } else {
                    bgcolour = ((attr[tindex] & ATTR_BGMASK) >> ATTR_BGSHIFT);
                    bg = -1;
                }

                if (attr[tindex] & ATTR_REVERSE) {
                    int tmpcolour = fgcolour;       /* Swap foreground and background */
                    fgcolour = bgcolour;
                    bgcolour = tmpcolour;

                    COLORREF tmpref = fg;
                    fg = bg;
                    bg = tmpref;
                }

                if (wgs->bold_colours && (attr[tindex] & ATTR_BOLD) &&
                    (fgcolour >= 0)) {
                    if (fgcolour  <   8)            /* ANSI colours */
                        fgcolour +=   8;
                    else if (fgcolour >= 256)       /* Default colours */
                        fgcolour ++;
                }

                if ((attr[tindex] & ATTR_BLINK) && (bgcolour >= 0)) {
                    if (bgcolour  <   8)            /* ANSI colours */
                        bgcolour +=   8;
                    else if (bgcolour >= 256)       /* Default colours */
                        bgcolour ++;
                }

                /*
                 * Collect other attributes
                 */
                if (wgs->bold_font_mode != BOLD_NONE)
                    attrBold  = attr[tindex] & ATTR_BOLD;
                else
                    attrBold  = 0;

                attrUnder = attr[tindex] & ATTR_UNDER;

                /*
                 * Reverse video
                 *   o  If video isn't reversed, ignore colour attributes for default foregound
                 *      or background.
                 *   o  Special case where bolded text is displayed using the default foregound
                 *      and background colours - force to bolded RTF.
                 */
                if (!(attr[tindex] & ATTR_REVERSE)) {
                    if (bgcolour >= 256)            /* Default color */
                        bgcolour  = -1;             /* No coloring */

                    if (fgcolour >= 256) {          /* Default colour */
                        if (wgs->bold_colours && (fgcolour & 1) &&
                            bgcolour == -1)
                            attrBold = ATTR_BOLD;   /* Emphasize text with bold attribute */

                        fgcolour  = -1;             /* No coloring */
                    }
                }

                /*
                 * Write RTF text attributes
                 */
                if ((lastfgcolour != fgcolour) || (lastfg != fg)) {
                    lastfgcolour  = fgcolour;
                    lastfg        = fg;
                    if (fg == -1) {
                        put_fmt(rtf, "\\cf%d ",
                                (fgcolour >= 0) ? palette[fgcolour] : 0);
                    } else {
                        rgbindex rgb, *rgbp;
                        rgb.ref = fg;
                        if ((rgbp = find234(rgbtree, &rgb, NULL)) != NULL)
                            put_fmt(rtf, "\\cf%d ", rgbp->index);
                    }
                }

                if ((lastbgcolour != bgcolour) || (lastbg != bg)) {
                    lastbgcolour  = bgcolour;
                    lastbg        = bg;
                    if (bg == -1)
                        put_fmt(rtf, "\\highlight%d ",
                                (bgcolour >= 0) ? palette[bgcolour] : 0);
                    else {
                        rgbindex rgb, *rgbp;
                        rgb.ref = bg;
                        if ((rgbp = find234(rgbtree, &rgb, NULL)) != NULL)
                            put_fmt(rtf, "\\highlight%d ", rgbp->index);
                    }
                }

                if (lastAttrBold != attrBold) {
                    lastAttrBold  = attrBold;
                    put_datapl(rtf, attrBold ?
                               PTRLEN_LITERAL("\\b ") :
                               PTRLEN_LITERAL("\\b0 "));
                }

                if (lastAttrUnder != attrUnder) {
                    lastAttrUnder  = attrUnder;
                    put_datapl(rtf, attrUnder ?
                               PTRLEN_LITERAL("\\ul ") :
                               PTRLEN_LITERAL("\\ulnone "));
                }
            }

            if (unitab[tdata[tindex]] == udata[uindex]) {
                multilen = 1;
                before[0] = '\0';
                after[0] = '\0';
                blen = alen = 0;
            } else {
                multilen = WideCharToMultiByte(CP_ACP, 0, unitab+uindex, 1,
                                               NULL, 0, NULL, NULL);
                if (multilen != 1) {
                    blen = sprintf(before, "{\\uc%d\\u%d", (int)multilen,
                                   (int)udata[uindex]);
                    alen = 1; strcpy(after, "}");
                } else {
                    blen = sprintf(before, "\\u%d", (int)udata[uindex]);
                    alen = 0; after[0] = '\0';
                }
            }
            assert(tindex + multilen <= len2);

            put_data(rtf, before, blen);
            for (i = 0; i < multilen; i++) {
                if (tdata[tindex+i] == '\\' ||
                    tdata[tindex+i] == '{' ||
                    tdata[tindex+i] == '}') {
                    put_byte(rtf, '\\');
                    put_byte(rtf, tdata[tindex+i]);
                } else if (tdata[tindex+i] == 0x0D || tdata[tindex+i] == 0x0A) {
                    put_datapl(rtf, PTRLEN_LITERAL("\\par\r\n"));
                } else if (tdata[tindex+i] > 0x7E || tdata[tindex+i] < 0x20) {
                    put_fmt(rtf, "\\'%02x", tdata[tindex+i]);
                } else {
                    put_byte(rtf, tdata[tindex+i]);
                }
            }
            put_data(rtf, after, alen);

            tindex += multilen;
            uindex++;
        }

        put_datapl(rtf, PTRLEN_LITERAL("}\0\0")); /* Terminate RTF stream */

        clipdata3 = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, rtf->len);
        if (clipdata3 && (lock3 = GlobalLock(clipdata3)) != NULL) {
            memcpy(lock3, rtf->u, rtf->len);
            GlobalUnlock(clipdata3);
        }
        strbuf_free(rtf);

        if (rgbtree) {
            rgbindex *rgbp;
            while ((rgbp = delpos234(rgbtree, 0)) != NULL)
                sfree(rgbp);
            freetree234(rgbtree);
        }
    } else
        clipdata3 = NULL;

    GlobalUnlock(clipdata);
    GlobalUnlock(clipdata2);

    if (!must_deselect)
        SendMessage(wgs->term_hwnd, WM_IGNORE_CLIP, true, 0);

    if (OpenClipboard(wgs->term_hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, clipdata);
        SetClipboardData(CF_TEXT, clipdata2);
        if (clipdata3)
            SetClipboardData(RegisterClipboardFormat(CF_RTF), clipdata3);
        CloseClipboard();
    } else {
        GlobalFree(clipdata);
        GlobalFree(clipdata2);
    }

    if (!must_deselect)
        SendMessage(wgs->term_hwnd, WM_IGNORE_CLIP, false, 0);
}

static DWORD WINAPI clipboard_read_threadfunc(void *param)
{
    HWND hwnd = (HWND)param;
    HGLOBAL clipdata;

    if (OpenClipboard(NULL)) {
        if ((clipdata = GetClipboardData(CF_UNICODETEXT))) {
            SendMessage(hwnd, WM_GOT_CLIPDATA,
                        (WPARAM)true, (LPARAM)clipdata);
        } else if ((clipdata = GetClipboardData(CF_TEXT))) {
            SendMessage(hwnd, WM_GOT_CLIPDATA,
                        (WPARAM)false, (LPARAM)clipdata);
        }
        CloseClipboard();
    }

    return 0;
}

static void process_clipdata(WinGuiSeat *wgs, HGLOBAL clipdata, bool unicode)
{
    wchar_t *clipboard_contents = NULL;
    size_t clipboard_length = 0;

    if (unicode) {
        wchar_t *p = GlobalLock(clipdata);
        wchar_t *p2;

        if (p) {
            /* Unwilling to rely on Windows having wcslen() */
            for (p2 = p; *p2; p2++);
            clipboard_length = p2 - p;
            clipboard_contents = snewn(clipboard_length + 1, wchar_t);
            memcpy(clipboard_contents, p, clipboard_length * sizeof(wchar_t));
            clipboard_contents[clipboard_length] = L'\0';
        }
    } else {
        char *s = GlobalLock(clipdata);
        int i;

        if (s) {
            i = MultiByteToWideChar(CP_ACP, 0, s, strlen(s) + 1, 0, 0);
            clipboard_contents = snewn(i, wchar_t);
            MultiByteToWideChar(CP_ACP, 0, s, strlen(s) + 1,
                                clipboard_contents, i);
            clipboard_length = i - 1;
            clipboard_contents[clipboard_length] = L'\0';
        }
    }

    if (clipboard_contents) {
#ifdef MOD_PERSO
        /* [KiTTY] pastesize: ask before pasting more than N characters
         * (0 = unlimited), so a mis-aimed paste of a huge clipboard
         * cannot flood the shell unconfirmed. */
        int limit = GetPasteSize();
        if (limit > 0 && clipboard_length > (size_t)limit) {
            char msg[160];
            sprintf(msg, "The clipboard holds %lu characters, more than"
                    " the configured pastesize limit of %d.\n\n"
                    "Paste it anyway?",
                    (unsigned long)clipboard_length, limit);
            if (MessageBox(wgs->term_hwnd, msg, "KiTTY paste",
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)
                    != IDYES) {
                sfree(clipboard_contents);
                return;
            }
        }
#endif
        term_do_paste(wgs->term, clipboard_contents, clipboard_length);
    }

    sfree(clipboard_contents);
}

static void wintw_clip_request_paste(TermWin *tw, int clipboard)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    assert(clipboard == CLIP_SYSTEM);

    /*
     * I always thought pasting was synchronous in Windows; the
     * clipboard access functions certainly _look_ synchronous,
     * unlike the X ones. But in fact it seems that in some
     * situations the contents of the clipboard might not be
     * immediately available, and the clipboard-reading functions
     * may block. This leads to trouble if the application
     * delivering the clipboard data has to get hold of it by -
     * for example - talking over a network connection which is
     * forwarded through this very PuTTY.
     *
     * Hence, we spawn a subthread to read the clipboard, and do
     * our paste when it's finished. The thread will send a
     * message back to our main window when it terminates, and
     * that tells us it's OK to paste.
     */
    DWORD in_threadid; /* required for Win9x */
    HANDLE hThread = CreateThread(NULL, 0, clipboard_read_threadfunc,
                                  wgs->term_hwnd, 0, &in_threadid);
    if (hThread)
        CloseHandle(hThread);          /* we don't need the thread handle */
}

/*
 * Print a modal (Really Bad) message box and perform a fatal exit.
 */
void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    char *message, *title;

    va_start(ap, fmt);
    message = dupvprintf(fmt, ap);
    va_end(ap);
    show_mouseptr(NULL, true);
    title = dupprintf("%s Fatal Error", appname);
    MessageBox(find_window_for_msgbox(), message, title,
               MB_SYSTEMMODAL | MB_ICONERROR | MB_OK);
    sfree(message);
    sfree(title);
    cleanup_exit(1);
}

/*
 * Print a message box and don't close the connection.
 */
void nonfatal(const char *fmt, ...)
{
    va_list ap;
    char *message, *title;

    va_start(ap, fmt);
    message = dupvprintf(fmt, ap);
    va_end(ap);
    show_mouseptr(NULL, true);
    title = dupprintf("%s Error", appname);
    MessageBox(find_window_for_msgbox(), message, title, MB_ICONERROR | MB_OK);
    sfree(message);
    sfree(title);
}

static bool flash_window_ex(WinGuiSeat *wgs, DWORD dwFlags,
                            UINT uCount, DWORD dwTimeout)
{
    if (p_FlashWindowEx) {
        FLASHWINFO fi;
        fi.cbSize = sizeof(fi);
        fi.hwnd = wgs->term_hwnd;
        fi.dwFlags = dwFlags;
        fi.uCount = uCount;
        fi.dwTimeout = dwTimeout;
        return (*p_FlashWindowEx)(&fi);
    }
    else
        return false; /* shrug */
}

/*
 * Timer for platforms where we must maintain window flashing manually
 * (e.g., Win95).
 */
static void flash_window_timer(void *vctx, unsigned long now)
{
    WinGuiSeat *wgs = (WinGuiSeat *)vctx;
    if (wgs->flashing && now == wgs->next_flash) {
        flash_window(wgs, 1);
    }
}

/*
 * Manage window caption / taskbar flashing, if enabled.
 * 0 = stop, 1 = maintain, 2 = start
 */
static void flash_window(WinGuiSeat *wgs, int mode)
{
    int beep_ind = conf_get_int(wgs->conf, CONF_beep_ind);
    if ((mode == 0) || (beep_ind == B_IND_DISABLED)) {
        /* stop */
        if (wgs->flashing) {
            wgs->flashing = false;
            if (p_FlashWindowEx)
                flash_window_ex(wgs, FLASHW_STOP, 0, 0);
            else
                FlashWindow(wgs->term_hwnd, false);
        }

    } else if (mode == 2) {
        /* start */
        if (!wgs->flashing) {
            wgs->flashing = true;
            if (p_FlashWindowEx) {
                /* For so-called "steady" mode, we use uCount=2, which
                 * seems to be the traditional number of flashes used
                 * by user notifications (e.g., by Explorer).
                 * uCount=0 appears to enable continuous flashing, per
                 * "flashing" mode, although I haven't seen this
                 * documented. */
                flash_window_ex(wgs, FLASHW_ALL | FLASHW_TIMER,
                                (beep_ind == B_IND_FLASH ? 0 : 2),
                                0 /* system cursor blink rate */);
                /* No need to schedule timer */
            } else {
                FlashWindow(wgs->term_hwnd, true);
                wgs->next_flash = schedule_timer(450, flash_window_timer, wgs);
            }
        }

    } else if ((mode == 1) && (beep_ind == B_IND_FLASH)) {
        /* maintain */
        if (wgs->flashing && !p_FlashWindowEx) {
            FlashWindow(wgs->term_hwnd, true);    /* toggle */
            wgs->next_flash = schedule_timer(450, flash_window_timer, wgs);
        }
    }
}

/*
 * Beep.
 */
static void wintw_bell(TermWin *tw, int mode)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (mode == BELL_DEFAULT) {
        /*
         * For MessageBeep style bells, we want to be careful of
         * timing, because they don't have the nice property of
         * PlaySound bells that each one cancels the previous
         * active one. So we limit the rate to one per 50ms or so.
         */
        long beepdiff;

        beepdiff = GetTickCount() - wgs->last_beep_time;
        if (beepdiff >= 0 && beepdiff < 50)
            return;
        MessageBeep(MB_OK);
        /*
         * The above MessageBeep call takes time, so we record the
         * time _after_ it finishes rather than before it starts.
         */
        wgs->last_beep_time = GetTickCount();
    } else if (mode == BELL_WAVEFILE) {
        Filename *bell_wavefile = conf_get_filename(
            wgs->conf, CONF_bell_wavefile);
        bool success = (
            p_PlaySoundW ? p_PlaySoundW(bell_wavefile->wpath, NULL,
                                        SND_ASYNC | SND_FILENAME) :
            p_PlaySoundA ? p_PlaySoundA(bell_wavefile->cpath, NULL,
                                        SND_ASYNC | SND_FILENAME) : false);
        if (!success) {
            char *buf, *otherbuf;
            show_mouseptr(wgs, true);
            buf = dupprintf(
                "Unable to play sound file\n%s\nUsing default sound instead",
                bell_wavefile->utf8path);
            otherbuf = dupprintf("%s Sound Error", appname);
            message_box(wgs->term_hwnd, buf, otherbuf,
                        MB_OK | MB_ICONEXCLAMATION, true, 0);
            sfree(buf);
            sfree(otherbuf);
            conf_set_int(wgs->conf, CONF_beep, BELL_DEFAULT);
        }
    } else if (mode == BELL_PCSPEAKER) {
        long beepdiff;

        beepdiff = GetTickCount() - wgs->last_beep_time;
        if (beepdiff >= 0 && beepdiff < 50)
            return;

        /*
         * We must beep in different ways depending on whether this
         * is a 95-series or NT-series OS.
         */
        if (osPlatformId == VER_PLATFORM_WIN32_NT)
            Beep(800, 100);
        else
            MessageBeep(-1);
        wgs->last_beep_time = GetTickCount();
    }
    /* Otherwise, either visual bell or disabled; do nothing here */
    if (!wgs->term->has_focus) {
        flash_window(wgs, 2);               /* start */
    }
}

/*
 * Minimise or restore the window in response to a server-side
 * request.
 */
static void wintw_set_minimised(TermWin *tw, bool minimised)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (IsIconic(wgs->term_hwnd)) {
        if (!minimised)
            ShowWindow(wgs->term_hwnd, SW_RESTORE);
    } else {
        if (minimised)
            ShowWindow(wgs->term_hwnd, SW_MINIMIZE);
    }
}

/*
 * Move the window in response to a server-side request.
 */
static void wintw_move(TermWin *tw, int x, int y)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    int resize_action = conf_get_int(wgs->conf, CONF_resize_action);
    if (resize_action == RESIZE_DISABLED ||
        resize_action == RESIZE_FONT ||
        IsZoomed(wgs->term_hwnd))
        return;

    SetWindowPos(wgs->term_hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

/*
 * Move the window to the top or bottom of the z-order in response
 * to a server-side request.
 */
static void wintw_set_zorder(TermWin *tw, bool top)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (conf_get_bool(wgs->conf, CONF_alwaysontop))
        return;                        /* ignore */
    SetWindowPos(wgs->term_hwnd, top ? HWND_TOP : HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
}

/*
 * Refresh the window in response to a server-side request.
 */
static void wintw_refresh(TermWin *tw)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    InvalidateRect(wgs->term_hwnd, NULL, true);
}

/*
 * Maximise or restore the window in response to a server-side
 * request.
 */
static void wintw_set_maximised(TermWin *tw, bool maximised)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (IsZoomed(wgs->term_hwnd)) {
        if (!maximised)
            ShowWindow(wgs->term_hwnd, SW_RESTORE);
    } else {
        if (maximised)
            ShowWindow(wgs->term_hwnd, SW_MAXIMIZE);
    }
}

/*
 * See if we're in full-screen mode.
 */
static bool is_full_screen(WinGuiSeat *wgs)
{
    if (!IsZoomed(wgs->term_hwnd))
        return false;
    if (GetWindowLongPtr(wgs->term_hwnd, GWL_STYLE) & WS_CAPTION)
        return false;
    return true;
}

/* Get a MONITORINFO structure for the nearest available monitor, if the
 * multimon API is available and returns success. Shared subroutine between
 * get_fullscreen_rect() and get_workingarea_rect(). */
static bool get_monitor_info(WinGuiSeat *wgs, MONITORINFO *mi)
{
#if defined(MONITOR_DEFAULTTONEAREST) && !defined(NO_MULTIMON)
    if (p_GetMonitorInfoA && p_MonitorFromWindow) {
        HMONITOR mon;
        mon = p_MonitorFromWindow(wgs->term_hwnd, MONITOR_DEFAULTTONEAREST);
        mi->cbSize = sizeof(*mi);
        p_GetMonitorInfoA(mon, mi);
        return true;
    }
#endif
    return false;
}


/* Get the rect/size of a full-screen window on the nearest available
 * monitor in multimon systems; default to something sensible if only
 * one monitor is present. */
static bool get_fullscreen_rect(WinGuiSeat *wgs, RECT *ss)
{
#if defined(MONITOR_DEFAULTTONEAREST) && !defined(NO_MULTIMON)
    MONITORINFO mi;
    if (get_monitor_info(wgs, &mi)) {
        /* structure copy */
        *ss = mi.rcMonitor;
        return true;
    }
#endif
/* could also use code like this:
        ss->left = ss->top = 0;
        ss->right = GetSystemMetrics(SM_CXSCREEN);
        ss->bottom = GetSystemMetrics(SM_CYSCREEN);
*/
    return GetClientRect(GetDesktopWindow(), ss);
}


/* Similar to get_fullscreen_rect, but retrieves the working area of the
 * monitor (minus the taskbar) instead of its full extent. */
static bool get_workingarea_rect(WinGuiSeat *wgs, RECT *ss)
{
#if defined(MONITOR_DEFAULTTONEAREST) && !defined(NO_MULTIMON)
    MONITORINFO mi;
    if (get_monitor_info(wgs, &mi)) {
        /* structure copy */
        *ss = mi.rcWork;
        return true;
    }
#endif
    /* Fallback is the same as get_monitor_rect, which is good _enough_:
     * if the window overlaps the taskbar, that's not too bad a failure. */
    return GetClientRect(GetDesktopWindow(), ss);
}


/*
 * Go full-screen. This should only be called when we are already
 * maximised.
 */
static void make_full_screen(WinGuiSeat *wgs)
{
    DWORD style;
    RECT ss;

    assert(IsZoomed(wgs->term_hwnd));

    if (is_full_screen(wgs))
        return;

    /* Remove the window furniture. */
    style = GetWindowLongPtr(wgs->term_hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_BORDER | WS_THICKFRAME);
    if (conf_get_bool(wgs->conf, CONF_scrollbar_in_fullscreen))
        style |= WS_VSCROLL;
    else
        style &= ~WS_VSCROLL;
    SetWindowLongPtr(wgs->term_hwnd, GWL_STYLE, style);

    /* Resize ourselves to exactly cover the nearest monitor. */
    get_fullscreen_rect(wgs, &ss);
    SetWindowPos(wgs->term_hwnd, HWND_TOP, ss.left, ss.top,
                 ss.right - ss.left, ss.bottom - ss.top, SWP_FRAMECHANGED);

    /* We may have changed size as a result */

    reset_window(wgs, 0);

    /* Tick the menu item in the System and context menus. */
    {
        int i;
        for (i = 0; i < lenof(wgs->popup_menus); i++)
            CheckMenuItem(wgs->popup_menus[i].menu,
                          IDM_FULLSCREEN, MF_CHECKED);
    }
}

/*
 * Clear the full-screen attributes.
 */
static void clear_full_screen(WinGuiSeat *wgs)
{
    DWORD oldstyle, style;

    /* Reinstate the window furniture. */
    style = oldstyle = GetWindowLongPtr(wgs->term_hwnd, GWL_STYLE);
    style |= WS_CAPTION | WS_BORDER;
    if (conf_get_int(wgs->conf, CONF_resize_action) == RESIZE_DISABLED)
        style &= ~WS_THICKFRAME;
    else
        style |= WS_THICKFRAME;
    if (conf_get_bool(wgs->conf, CONF_scrollbar))
        style |= WS_VSCROLL;
    else
        style &= ~WS_VSCROLL;
    if (style != oldstyle) {
        SetWindowLongPtr(wgs->term_hwnd, GWL_STYLE, style);
        SetWindowPos(wgs->term_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_FRAMECHANGED);
    }

    /* Untick the menu item in the System and context menus. */
    {
        int i;
        for (i = 0; i < lenof(wgs->popup_menus); i++)
            CheckMenuItem(wgs->popup_menus[i].menu,
                          IDM_FULLSCREEN, MF_UNCHECKED);
    }
}

/*
 * Toggle full-screen mode.
 */
static void flip_full_screen(WinGuiSeat *wgs)
{
    if (is_full_screen(wgs)) {
        ShowWindow(wgs->term_hwnd, SW_RESTORE);
    } else if (IsZoomed(wgs->term_hwnd)) {
        make_full_screen(wgs);
    } else {
        SendMessage(wgs->term_hwnd, WM_FULLSCR_ON_MAX, 0, 0);
        ShowWindow(wgs->term_hwnd, SW_MAXIMIZE);
    }
}

static size_t win_seat_output(Seat *seat, SeatOutputType type,
                              const void *data, size_t len)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
#ifdef MOD_ZMODEM
    /* KiTTY ZModem: while a transfer is active, route incoming backend bytes to
     * the helper's stdin instead of the terminal. (0.76b did this in
     * terminal.c term_data; here we keep terminal.c untouched.) */
    if (kitty_zmodem_active() && type == SEAT_OUTPUT_STDOUT)
        return kitty_zmodem_recv_data(data, len);
#endif
#ifdef MOD_PERSO
    /* KiTTY rutty scripting: observe incoming host data for waitfor/halton
     * (OBSERVE-only - the data still flows on to the terminal below). Same
     * interception point as ZModem; no terminal.c edits. */
    if (kitty_script_active() && type == SEAT_OUTPUT_STDOUT)
        kitty_script_remote(data, len);
#endif
    {
        size_t consumed = term_data(wgs->term, data, len);
#ifdef MOD_PERSO
        /*
         * KiTTY automatic logon script: scan incoming server output for the
         * challenge string and auto-send the configured reply. The driving call
         * lived in 0.76b term_data and was dropped during the forward-port; it is
         * re-added here as an OBSERVE-only hook, since this port does not edit
         * terminal.c. ManageInitScript self-skips when ScriptFileContent is NULL.
         *
         * AFTER term_data, not before, and it matters on screen. Running it first
         * meant the script matched the prompt and echoed its reply BEFORE the
         * prompt itself had been drawn, so a login read back-to-front:
         *
         *     testuser          <- the reply, echoed
         *     login: hunter2    <- the prompt that caused it, then the next reply
         *     Password:         <- drawn last
         *
         * The script sees identical bytes either way, so this costs nothing and
         * restores the order classic KiTTY showed when it drove this from inside
         * term_data.
         */
        if (!GetPuttyFlag() && ScriptFileContent != NULL && type == SEAT_OUTPUT_STDOUT)
            ManageInitScript(data, len);
        /*
         * A rutty script deferred behind that login script starts HERE, the
         * moment the login script has consumed its last entry - still inside
         * the processing of the chunk whose match consumed it. Whatever the
         * login script's final send provokes needs a server round trip and so
         * arrives in a LATER chunk, which the engine is now observing; a
         * timer tick instead of this reopened the missed-prompt race for any
         * first step that waits. The fallback timer is disarmed with it.
         */
        if (wgs->script_deferred &&
            !(ScriptFileContent != NULL && ScriptFileContent[0])) {
            wgs->script_deferred = false;
            KillTimer(wgs->term_hwnd, TIMER_SCRIPT);
            if (kitty_script_enabled() &&
                conf_get_int(wgs->conf, CONF_script_mode) == 1) {
                Filename *sf = conf_get_filename(wgs->conf, CONF_scriptfile);
                if (sf && filename_to_str(sf)[0])
                    kitty_script_send_file(wgs->conf, wgs->backend, sf);
            }
        }
#endif
        return consumed;
    }
}

static void wintw_unthrottle(TermWin *tw, size_t bufsize)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (wgs->backend)
        backend_unthrottle(wgs->backend, bufsize);
}

static bool win_seat_eof(Seat *seat)
{
    return true;   /* do respond to incoming EOF with outgoing */
}

static SeatPromptResult win_seat_get_userpass_input(Seat *seat, prompts_t *p)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    SeatPromptResult spr;
    spr = cmdline_get_passwd_input(p, &wgs->cmdline_get_passwd_state, true);
#ifdef MOD_PERSO
    /* KiTTY auto-login password: when no -pw was supplied, answer a single
     * password prompt to the server (plain SSH password OR keyboard-interactive)
     * from the stored CONF_password. Login is silent by design - the user
     * consented when they set the password in the config dialog.
     * NOTE: at runtime CONF_password holds the PLAIN-TEXT password - the load
     * path (kitty_settings_load.c) decrypts it and the config dialog stores it
     * plain. So use it directly; do NOT call GetPasswordInConfig(), which applies
     * an extra MASKPASS that would garble an already-plaintext password (that
     * helper assumes the MASKPASS-encoded form produced by the now-stubbed
     * RenewPassword). TODO(security): the password is recoverable from the saved
     * session; a future hardening pass should revisit storage / prefer key auth. */
    if (spr.kind == SPRK_INCOMPLETE && !GetPuttyFlag() && !wgs->autopw_tried &&
        p->n_prompts == 1 && !p->prompts[0]->echo && p->to_server &&
        strlen(conf_get_str(wgs->conf, CONF_password)) > 0) {
        /* Answer the stored password ONCE per connection. If the server rejects
         * it and re-prompts, do NOT auto-resend (that would burn the server's
         * MaxAuthTries and risk an IP ban); fall through to the interactive
         * prompt so the user can correct it or cancel. */
        wgs->autopw_tried = true;
        {
            extern void kitty_pwdebug(const char *fmt, ...);
            const char *pwv = conf_get_str(wgs->conf, CONF_password);
            unsigned h = 0; const char *q;
            for (q = pwv; q && *q; q++) h = h * 131 + (unsigned char)*q;
            kitty_pwdebug("AUTH send pw: len=%d cksum=%04x prompt=[%s]",
                          pwv ? (int)strlen(pwv) : -1, h & 0xffff,
                          p->prompts[0]->prompt ? p->prompts[0]->prompt : "");
        }
        prompt_set_result(p->prompts[0], conf_get_str(wgs->conf, CONF_password));
        spr = SPR_OK;
    }
#endif
    if (spr.kind == SPRK_INCOMPLETE)
        spr = term_get_userpass_input(wgs->term, p);
    return spr;
}

static void win_seat_set_trust_status(Seat *seat, bool trusted)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    term_set_trust_status(wgs->term, trusted);
}

static bool win_seat_can_set_trust_status(Seat *seat)
{
    return true;
}

static bool win_seat_get_cursor_position(Seat *seat, int *x, int *y)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    term_get_cursor_position(wgs->term, x, y);
    return true;
}

static bool win_seat_get_window_pixel_size(Seat *seat, int *x, int *y)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    RECT r;
    GetWindowRect(wgs->term_hwnd, &r);
    *x = r.right - r.left;
    *y = r.bottom - r.top;
    return true;
}

#ifdef MOD_PERSO
/* ===== KiTTY bridge: expose the active WinGuiSeat as globals (single-window shim) =====
 * Intermediate step toward the no-global integration. */
Conf *conf = NULL;
static WinGuiSeat *kitty_active_wgs = NULL;
void kitty_set_active_seat(WinGuiSeat *wgs) {
    kitty_active_wgs = wgs;
    conf = wgs ? wgs->conf : NULL;
    /* Track the active terminal hwnd so KiTTY keystroke-injection paths
     * (login script, special commands, input box) have a valid target.
     * Previously only set on the background-image / input-box paths, leaving
     * it NULL in a plain session. */
    if (wgs && wgs->term_hwnd) MainHwnd = wgs->term_hwnd;
}

/* Did the active window's connection go through workplace proxy mode's proxy?
 * The frame tint (kitty/kitty_osc52.c) asks this rather than asking whether the
 * MODE is on, for the same reason the title marker does: the colour describes
 * this connection, and connections outlive switchings of the mode in both
 * directions. */
int kitty_active_seat_workplace_proxied(void) {
    return kitty_active_wgs && kitty_active_wgs->workplace_proxied;
}

void do_eventlog(const char *st) {
    if (kitty_active_wgs && kitty_active_wgs->logctx)
        logevent(kitty_active_wgs->logctx, st);
}
void SendStrToTerminal(const char *str, const int len) {
    int i;
    if (len <= 0 || !kitty_active_wgs || !kitty_active_wgs->term) return;
    for (i = 0; i < len; i++) {
        char c = (char)(unsigned char)str[i];
        if (kitty_active_wgs->ldisc)
            term_keyinput(kitty_active_wgs->term, -1, &c, 1);
    }
}
void ResetWindow(int reinit) {
    if (kitty_active_wgs) reset_window(kitty_active_wgs, reinit);
}
void resize(int height, int width) {
    (void)height; (void)width; /* TODO: terminal resize bridge */
}

#ifdef MOD_BACKGROUNDIMAGE
/* Geometry/colour accessors required by kitty_image.c, served from the
 * active WinGuiSeat (KiTTY is effectively single-window). */
int return_offset_height(void) {
    return kitty_active_wgs ? kitty_active_wgs->offset_height : 0;
}
int return_offset_width(void) {
    return kitty_active_wgs ? kitty_active_wgs->offset_width : 0;
}
int return_font_height(void) {
    return kitty_active_wgs ? kitty_active_wgs->font_height : 0;
}
int return_font_width(void) {
    return kitty_active_wgs ? kitty_active_wgs->font_width : 0;
}
COLORREF return_colours258(void) {
    /* 258 = ATTR_DEFBG index in KiTTY's colour table = default background. */
    return kitty_active_wgs ? kitty_active_wgs->colours[258] : RGB(0,0,0);
}
#endif
#endif

#ifdef MOD_PERSO
/* ===== KiTTY feature: window transparency (NO-GLOBAL) =====
 * Reads CONF_transparencynumber from THIS seat's conf and applies it to
 * THIS seat's window. No global conf/term/hwnd -- fully per-WinGuiSeat,
 * compatible with multiple simultaneous seats (e.g. sshproxy).
 * Value 0 = opaque (default, no effect); 1..254 = increasing translucency. */
void kitty_apply_transparency(WinGuiSeat *wgs)
{
    if (!wgs || !wgs->term_hwnd) return;
    /* kitty.ini transparency=no switches the feature off, not merely its
     * controls: a session carrying a level must still open opaque. */
    if (!GetTransparencyFlag()) return;
    int t = conf_get_int(wgs->conf, CONF_transparencynumber);
    if (t <= 0) return;                 /* opaque / disabled */
    if (t > 254) t = 254;
    SetWindowLongPtr(wgs->term_hwnd, GWL_EXSTYLE,
                     GetWindowLongPtr(wgs->term_hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(wgs->term_hwnd, 0, (BYTE)(255 - t), LWA_ALPHA);
}
#endif

#ifdef MOD_PERSO
/* ===== KiTTY: window position memory =====
 * Remembers the last window position GLOBALLY, keyed by the current monitor
 * TOPOLOGY (so a docked dual-monitor layout and an undocked single screen each
 * remember their own spot, Word-style). Uses PHYSICAL screen coordinates
 * (GetWindowRect on save / SetWindowPos on restore) rather than
 * GetWindowPlacement/SetWindowPlacement: under Per-Monitor-V2 DPI awareness
 * (see windows/putty.mft) WINDOWPLACEMENT.rcNormalPosition is NOT reinterpreted
 * for the target monitor's DPI, so a placement captured on a secondary monitor
 * at a different scale is misapplied on restore (the window ends up at a default
 * position) -- the exact failure seen on mixed-DPI multi-monitor setups.
 * GetWindowRect/SetWindowPos work in the unified virtual-desktop pixel space and
 * round-trip correctly across mixed-DPI monitors. Position only: the session's
 * own size is kept. A session that pins CONF_xpos/ypos still wins. The restore
 * is applied AFTER the startup sizing/clamp block (see the call site), so the
 * single-monitor working-area clamp can't undo it. */
extern const char *kitty_registry_base(void);

/* Order-INDEPENDENT hash of the monitor layout: each monitor contributes its own
 * FNV-1a(rcMonitor), and the per-monitor hashes are SUMMED. EnumDisplayMonitors'
 * enumeration order is not guaranteed identical between the saving and restoring
 * processes, so an order-dependent fold could yield different keys for the same
 * physical layout (-> key not found -> no restore). Summation is commutative. */
static BOOL CALLBACK kitty_topo_enum(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    unsigned long *acc = (unsigned long *)lp;
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(hm, &mi)) {
        unsigned long h = 2166136261UL;
        const unsigned char *p = (const unsigned char *)&mi.rcMonitor;
        size_t i;
        for (i = 0; i < sizeof(mi.rcMonitor); i++) { h ^= p[i]; h *= 16777619UL; }
        *acc += h;
    }
    (void)dc; (void)rc;
    return TRUE;
}
static void kitty_winpos_key(char *buf, int n)
{
    unsigned long h = 0;
    EnumDisplayMonitors(NULL, NULL, kitty_topo_enum, (LPARAM)&h);
    _snprintf(buf, n, "WinPos_%08lx", h);
}

/* ---- window-position diagnostics ---------------------------------------------
 * Inert unless the environment variable KITTY_WINPOS_DEBUG is set; then it
 * appends a trace (monitor topology, topology key, save/restore rects, results)
 * to %TEMP%\kitty_winpos.log. Used to diagnose "remember window position" on
 * multi-monitor / mixed-DPI layouts. */
static int kitty_winpos_dbg_enabled(void)
{
    /* Off by default; set KITTY_WINPOS_DEBUG=1 to trace to
     * %TEMP%\kitty_winpos.log (works in any build, no separate debug exe). */
    static int cached = -1;
    if (cached < 0)
        cached = GetEnvironmentVariableA("KITTY_WINPOS_DEBUG", NULL, 0) ? 1 : 0;
    return cached;
}
static void kitty_winpos_dbg(const char *fmt, ...)
{
    if (!kitty_winpos_dbg_enabled()) return;
    char path[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(path), path);
    if (!n || n >= sizeof(path) - 20) return;
    strcat(path, "kitty_winpos.log");
    FILE *fp = fopen(path, "a");
    if (!fp) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(fp, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(fp, fmt, ap); va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}
static BOOL CALLBACK kitty_topo_logenum(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(hm, &mi))
        kitty_winpos_dbg("  monitor rcMonitor=(%ld,%ld,%ld,%ld) rcWork=(%ld,%ld,%ld,%ld) primary=%d",
            mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
            mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom,
            (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0);
    (void)dc; (void)rc; (void)lp;
    return TRUE;
}
static void kitty_winpos_dump_topo(const char *when)
{
    if (!kitty_winpos_dbg_enabled()) return;
    kitty_winpos_dbg("%s: monitor topology:", when);
    EnumDisplayMonitors(NULL, NULL, kitty_topo_logenum, 0);
}

/* KiTTY: everything a closing window has to persist, in ONE place - there are
 * five exit routes (four PostQuitMessage paths that never see WM_DESTROY, plus
 * WM_DESTROY itself), and each used to carry its own copy of the placement
 * save. A sixth route would have silently skipped it.
 *
 * Two independent per-session options:
 *   "Remember window position"  -> topology-keyed placement, app-wide.
 *   "Save settings on exit"     -> write this session back, including the
 *                                  window's current size and position.
 *
 * The settings save deliberately skips an unnamed session (an ad-hoc "type a
 * host and go" window has nothing to be saved into) and "Default Settings"
 * (rewriting the template from one window's final state is how every later
 * session inherits a stray change). CONF_width/height already track the live
 * terminal size - they are updated on every resize - so only the position and
 * the maximised state need filling in here. */
static void kitty_on_window_closing(WinGuiSeat *wgs, HWND hwnd)
{
    if (!wgs) return;
    if (!hwnd) hwnd = wgs->term_hwnd;

    if (conf_get_bool(wgs->conf, CONF_remember_winpos))
        kitty_save_window_placement(hwnd);

    if (conf_get_bool(wgs->conf, CONF_saveonexit)) {
        const char *name = conf_get_str(wgs->conf, CONF_sessionname);
        if (name && *name && strcmp(name, "Default Settings") != 0) {
            char *err;
            if (hwnd && !IsIconic(hwnd) && !IsZoomed(hwnd)) {
                RECT r;
                if (GetWindowRect(hwnd, &r)) {
                    conf_set_int(wgs->conf, CONF_xpos, (int)r.left);
                    conf_set_int(wgs->conf, CONF_ypos, (int)r.top);
                }
            }
            conf_set_int(wgs->conf, CONF_windowstate,
                         (hwnd && IsZoomed(hwnd)) ? 1 : 0);
            err = save_settings(name, wgs->conf);
            if (err) sfree(err);   /* closing: nowhere left to show it */
        }
    }
}

/* Save THIS window's physical rect under the current-topology key (on close).
 * Minimised/maximised states are not remembered (we only persist a normal
 * restored position). */
void kitty_save_window_placement(HWND hwnd)
{
    if (!hwnd) return;
    if (KITTY_IS_EMBEDDED(hwnd)) { kitty_winpos_dbg("SAVE skipped: embedded"); return; }   /* #554 */
    if (IsIconic(hwnd) || IsZoomed(hwnd)) { kitty_winpos_dbg("SAVE skipped: iconic/zoomed"); return; }
    RECT r;
    if (!GetWindowRect(hwnd, &r)) { kitty_winpos_dbg("SAVE skipped: GetWindowRect failed"); return; }
    char keyname[64]; kitty_winpos_key(keyname, sizeof(keyname));
    kitty_winpos_dump_topo("SAVE");
    kitty_winpos_dbg("SAVE key=%s rect=(%ld,%ld,%ld,%ld)", keyname, r.left, r.top, r.right, r.bottom);
    char base[600]; _snprintf(base, sizeof(base), "%s\\WindowPos", kitty_registry_base());
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, base, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, keyname, 0, REG_BINARY, (const BYTE *)&r, sizeof(r));
        RegCloseKey(hk);
        kitty_winpos_dbg("SAVE ok -> HKCU\\%s [%s]", base, keyname);
    } else {
        kitty_winpos_dbg("SAVE FAILED: RegCreateKeyEx HKCU\\%s", base);
    }
}

/* True if the screen point (x,y) lies on some visible monitor, so we never
 * strand a window off-screen when the layout has changed since the save. */
static BOOL kitty_point_on_monitor(int x, int y)
{
    POINT pt; pt.x = x; pt.y = y;
    if (!p_MonitorFromPoint) return TRUE;   /* can't check -> allow */
    return p_MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) != NULL;
}

/* Restore the saved top-left for the current topology, keeping THIS window's
 * current size. Returns 1 if a placement was applied. */
static int kitty_restore_window_placement(HWND hwnd)
{
    if (!hwnd) return 0;
    char keyname[64]; kitty_winpos_key(keyname, sizeof(keyname));
    char base[600]; _snprintf(base, sizeof(base), "%s\\WindowPos", kitty_registry_base());
    kitty_winpos_dump_topo("RESTORE");
    RECT saved; DWORD sz = sizeof(saved);
    if (RegGetValueA(HKEY_CURRENT_USER, base, keyname, RRF_RT_REG_BINARY,
                     NULL, &saved, &sz) != ERROR_SUCCESS) {
        kitty_winpos_dbg("RESTORE key=%s: no saved value in HKCU\\%s", keyname, base);
        return 0;
    }
    if (sz != sizeof(saved)) { kitty_winpos_dbg("RESTORE key=%s: bad value size %lu", keyname, (unsigned long)sz); return 0; }
    /* Only restore if the saved title-bar area is still on a visible monitor
     * (probe a point a little inside the top-left corner). */
    int onmon = kitty_point_on_monitor(saved.left + 8, saved.top + 8);
    kitty_winpos_dbg("RESTORE key=%s saved=(%ld,%ld,%ld,%ld) onmonitor=%d",
                     keyname, saved.left, saved.top, saved.right, saved.bottom, onmon);
    if (!onmon) { kitty_winpos_dbg("RESTORE skipped: saved top-left off-screen"); return 0; }
    /* Restore position AND size. The saved rect (GetWindowRect on close) holds
     * both; the topology key guarantees the same monitor/DPI, so the physical
     * size maps back to the same terminal cols/rows. Applying the size triggers
     * WM_SIZE, which snaps the terminal grid to the client area. A degenerate
     * saved size falls back to position-only. */
    int w = saved.right - saved.left;
    int h = saved.bottom - saved.top;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (w < 64 || h < 64) { w = 0; h = 0; flags |= SWP_NOSIZE; }
    int ok = SetWindowPos(hwnd, NULL, saved.left, saved.top, w, h, flags) ? 1 : 0;
    kitty_winpos_dbg("RESTORE SetWindowPos(%ld,%ld,%dx%d) -> %d",
                     saved.left, saved.top, w, h, ok);
    return ok;
}

/* Drag a pinned top-left back onto a visible monitor. A fixed position is typed
 * by hand (or comes from -xpos/-ypos, or from a session saved on a machine with
 * other screens), so it can easily name a spot that no longer exists - and a
 * window placed there is gone as far as the user is concerned. Nudged onto the
 * nearest monitor's work area instead, keeping the window fully on screen where
 * its size allows. */
static void kitty_clamp_pin_to_visible(HWND hwnd, int *x, int *y)
{
    POINT pt;
    HMONITOR mon;
    MONITORINFO mi;
    RECT wr;
    int w = 0, h = 0;

    if (kitty_point_on_monitor(*x + 8, *y + 8)) return;   /* already visible */
    if (!p_MonitorFromPoint || !p_GetMonitorInfoA) return;

    pt.x = *x; pt.y = *y;
    mon = p_MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!mon) return;
    mi.cbSize = sizeof(mi);
    if (!p_GetMonitorInfoA(mon, &mi)) return;

    if (hwnd && GetWindowRect(hwnd, &wr)) {
        w = (int)(wr.right - wr.left);
        h = (int)(wr.bottom - wr.top);
    }
    if (w > 0 && *x + w > mi.rcWork.right)  *x = (int)mi.rcWork.right - w;
    if (h > 0 && *y + h > mi.rcWork.bottom) *y = (int)mi.rcWork.bottom - h;
    if (*x < mi.rcWork.left) *x = (int)mi.rcWork.left;
    if (*y < mi.rcWork.top)  *y = (int)mi.rcWork.top;
    kitty_winpos_dbg("APPLY pin was off-screen, clamped to (%d,%d)", *x, *y);
}

/* Move THIS seat's window.
 *
 * A FIXED position (Window > Appearance: "Open the window at a fixed position"
 * + Top/Left) wins, because it is an explicit instruction - the user typed
 * coordinates, or passed -xpos/-ypos - while "remember window position" is a
 * convenience that follows the window around. If the pinned spot is not on any
 * current monitor it is clamped onto the nearest one rather than obeyed.
 *
 * The precedence used to be the other way round, and had to be: nothing
 * consulted the enabling checkbox, so a session whose stored TermXPos/TermYPos
 * happened to be (0,0) looked like an explicit pin and was forced to the screen
 * corner. Now that CONF_set_windowpos actually gates the pin, "no pin" and
 * "pinned at the corner" are distinguishable and the natural order works. */
void kitty_apply_window_pos(WinGuiSeat *wgs)
{
    if (!wgs || !wgs->term_hwnd) return;
    if (KITTY_EMBEDDED()) { kitty_winpos_dbg("APPLY skipped: embedded"); return; }   /* #554 */
    int x = conf_get_int(wgs->conf, CONF_xpos);
    int y = conf_get_int(wgs->conf, CONF_ypos);
    int remember = conf_get_bool(wgs->conf, CONF_remember_winpos);
    int pinned = conf_get_bool(wgs->conf, CONF_set_windowpos);
    kitty_winpos_dbg("APPLY xpos=%d ypos=%d pinned=%d remember=%d",
                     x, y, pinned, remember);

    if (pinned && x >= 0 && y >= 0) {
        kitty_clamp_pin_to_visible(wgs->term_hwnd, &x, &y);
        SetWindowPos(wgs->term_hwnd, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        kitty_winpos_dbg("APPLY pinned CONF pos (%d,%d) set", x, y);
        return;
    }
    if (remember && kitty_restore_window_placement(wgs->term_hwnd)) {
        kitty_winpos_dbg("APPLY restored remembered position");
        return;
    }
}
#endif
