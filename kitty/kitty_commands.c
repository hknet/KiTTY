/*
 * kitty_commands.c - the send-text-box internal commands ("/commands").
 * A translation unit of its own; what it reaches into kitty.c for is
 * declared in kitty_int.h.
 *
 * Every command is one row of internal_commands[]: the dispatcher
 * (InternalCommand) and the /help text are both generated from the table,
 * so a new command needs exactly one handler and one row. docs/COMMANDS.md
 * must carry a "### /name" section per row, in both directions - the
 * release guard parses the rows below and fails the release otherwise.
 */

#include <dirent.h>
#include <io.h>
#include <stdarg.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/locking.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

// PuTTY includes
#include "putty.h"
#include "terminal.h"
#include "putty-rc.h"

// Windows-specific includes (windows.h must come first)
#include <windows.h>
#include "kitty_oldwin.h"   /* record what an older Windows does not have */
#include <psapi.h>
#include <iphlpapi.h>
#include <shlobj.h>         /* SHGetFolderPathA: Pictures, for /screenshot */

// KiTTY includes
#include "kitty.h"
#include "kitty_params.h"
#include "kitty_broadcast.h"   /* the other KiTTY windows: count, broadcast, resize */
#include "kitty_portfwd.h"     /* the port-forward display */
#include "kitty_regbackup.h"   /* the .sav export/import and the backup rotation */
#include "kitty_int.h"         /* what the split-off files share with this one */
#include "kitty_startup.h"    /* InitWinMain */
#include "kitty_defs.h"     /* KITTY_DEFAULT_SESSION */
#include "kitty_common.h"
#include "kitty_image.h"
#include "kitty_crypt.h"
#include "kitty_registry.h"
#include "kitty_tools.h"
#include "kitty_win.h"
#include "kitty_updater.h"
#include "kitty_winutil.h"
#include "kitty_dlgbox.h"
#include "kitty_launcher.h"
#include "winfont_fallback.h"
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_text.h"   /* shared captions and wordings (also for the .c files included below) */
#include "kitty_inifile.h"   /* kitty_inifile_write_blocked: readonly=yes */
#include "kitty_inikeys.h"   /* KI_*: the kitty.ini key names */
#include "kitty_notes.h"   /* the application notification, marked owed at startup */
#include "kitty_pwmem.h"   /* passwords wrapped in memory (kitty_commands.c too) */
#include "kitty_storage.h"
#include "kitty_secretstore.h"
#include "kitty_gui.h"
#include "kitty_bridge.h"
#include "kitty_exportbundle.h"
#include "mini/mini.h"
#include "kitty_commands.h"
#include "kitty_theme.h"   /* kitty_theme_apply_hooked: /help dark from its first frame */
#include "kitty_winerr.h"   /* kitty_win_reason: Windows' reason, for the reports */
#include "kitty_sessionpath.h"   /* kitty_session_resolve: does a name exist */
#include "kitty_url_regex.h"   /* KITTY_URL_REGEX_DEFAULT, for /urlregex */
#include "kitty_notice.h"   /* kitty_notice_show: Save as New Session refused */

/* /save + /savenew <name>: write the LIVE settings to a saved session. With a
 * name, the window's session identity switches to it first (save-as), so later
 * /save calls and save-on-exit land there. Mirrors the config-box Save button
 * (folder default + launcher refresh broadcast, cf. kitty_config.c). The
 * classic /save (.ktx file exporter, SaveCurrentSetting) lives on as /savektx.
 * The outcome goes to *out (in full, sfree it; NULL = nothing to report),
 * for the command box's result line: no box, which would stop the session
 * while it is open. 1 = saved. */
static int kitty_save_current_session( const char * newname, char ** out ) {
	char * errmsg ;
	*out = NULL ;
	if( newname != NULL ) {
		while( newname[0]==' ' ) newname++ ;
		if( newname[0]=='\0' ) return 0 ;
	}
	/* readonly=yes: nothing is saved, and the window keeps its session name */
	if( GetReadOnlyFlag() ) {
		*out = dupprintf( KT_STORE_RO_NOT_SAVED, kitty_store_ro_inifile() ) ;
		return 0 ;
	}
	if( newname != NULL ) conf_set_str( conf, CONF_sessionname, newname ) ;
	if( strlen( conf_get_str(conf,CONF_folder) ) == 0 ) conf_set_str( conf, CONF_folder, "Default" ) ;
	kitty_mpw_user_action_begin() ;   /* prompts again after a cancel */
	errmsg = save_settings( conf_get_str(conf,CONF_sessionname), conf ) ;
	kitty_mpw_user_action_end() ;
	if( errmsg != NULL ) {
		*out = dupprintf( KT_CMD_RES_NOT_SAVED, errmsg ) ;
		sfree( errmsg ) ;
		return 0 ;
	}
	if( newname != NULL ) kitty_set_last_session( conf_get_str(conf,CONF_sessionname) ) ;
	{	/* same best-effort refresh broadcast as the config-box Save button */
		UINT msg = RegisterWindowMessageA( "KiTTYLauncherRefreshSessionsAndHotkeys" ) ;
		if( msg ) PostMessageA( HWND_BROADCAST, msg, 0, 0 ) ;
	}
	*out = dupprintf( KT_CMD_RES_SESSION_SAVED, conf_get_str(conf,CONF_sessionname) ) ;
	return 1 ;
}

/* Window menu "Save as New Session...": Change Settings, opened on the
 * Session panel with the name field holding a new name - the host, or
 * user@host when a session of the host's name exists. Its Save writes the
 * session, and puts up a question first when the name exists already. */
void kitty_savenew_box( HWND hwnd ) {
	const char * host = conf_get_str( conf, CONF_host ) ;
	const char * user = conf_get_str_ambi( conf, CONF_username, NULL ) ;   /* STR_AMBI: conf_get_str asserts */
	char * suggest ;
	if( GetReadOnlyFlag() ) {
		/* readonly=yes: refused in the notice window, which leaves the
		 * session running (a box would stop it) */
		char * msg = dupprintf( KT_STORE_RO_NOT_SAVED, kitty_store_ro_inifile() ) ;
		kitty_notice_show( KT_CAP_SAVE_SESSION, msg, RGB(190, 110, 0),
		                   GetNoticeSeconds(), NULL, 0 ) ;
		sfree( msg ) ;
		return ;
	}
	if( host && *host && user && *user && kitty_session_resolve( host, NULL, NULL, 0 ) == KSP_EXACT )
		suggest = dupprintf( "%s@%s", user, host ) ;
	else
		suggest = dupstr( host ? host : "" ) ;
	kitty_cfgbox_prefill_session_name( suggest ) ;
	sfree( suggest ) ;
	PostMessage( hwnd, WM_SYSCOMMAND, IDM_RECONF, 0 ) ;
}


typedef enum {
	IC_ARG_NONE,		/* exact match only: "/cmd" */
	IC_ARG_OPTIONAL,	/* "/cmd" or "/cmd <arg>" */
	IC_ARG_REQUIRED		/* "/cmd <arg>" only; a bare "/cmd" is not a command */
} IntCmdArg ;

/* Handlers return 1 = handled, 0 = not an internal command after all (the
 * input is then sent to the host like any other text). */
typedef int (*IntCmdHandler)( HWND hwnd, char * arg ) ;

/* What the last command did, for the command box's result line. A handler
 * that has something to report writes it with cmd_say(); one that writes
 * nothing gets "Done: /name" from InternalCommand. A file is always shown
 * with its full path - the person typing it must never have to guess.
 * Several lines are fine: the line grows with its text (kitty_inputbox.c).
 * Only /passwd, in debug mode, still reports in a box of its own; no other
 * command does - a modal box stops the session
 * while it is open; a result that comes later (an answered question, a
 * picked file) goes to the box through kitty_inputbox_result. */
static char * cmd_result = NULL ;   /* in full: a long path is never cut */

static void cmd_say( const char * fmt, ... ) {
	char * s ;
	va_list ap ;
	va_start( ap, fmt ) ;
	s = dupvprintf( fmt, ap ) ;   /* before the old one goes: it may be an argument */
	va_end( ap ) ;
	sfree( cmd_result ) ;
	cmd_result = s ;
}

static void cmd_say_clear( void ) {
	sfree( cmd_result ) ;
	cmd_result = NULL ;
}

static void cmd_say_toggle( const char * what, int on ) {
	cmd_say( KT_CMD_RES_TOGGLE, what, on ? KT_CMD_RES_ON : KT_CMD_RES_OFF ) ;
}

const char * kitty_cmd_result( void ) { return cmd_result ? cmd_result : "" ; }

/* ---- Window & title ---- */

static int cmd_size( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	if( !TitleBarFlag ) {
		/* Decorations off (the wintitle master switch): the suffix is
		 * invisible regardless of SizeFlag, and blind toggling made
		 * /size look dead every other time. Typing /size here can only
		 * mean "show the size" - enable both. */
		TitleBarFlag = 1 ;
		SizeFlag = 1 ;
	} else {
		SizeFlag = abs( SizeFlag - 1 ) ;
	}
	kitty_refresh_title() ;
	cmd_say_toggle( KT_CMD_RES_SIZE, SizeFlag ) ;
	return 1 ;
}

static int cmd_wintitle( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	TitleBarFlag = abs( TitleBarFlag - 1 ) ;
	kitty_refresh_title() ;
	cmd_say_toggle( KT_CMD_RES_WINTITLE, TitleBarFlag ) ;
	return 1 ;
}

/* /title <text>: the window-only title (placeholders expanded, never
 * saved); /title alone: the session's configured title again. */
static int cmd_title( HWND hwnd, char * arg ) {
	(void)hwnd ;
	kitty_window_only_title_set( ( arg && *arg ) ? arg : NULL ) ;
	cmd_say( "%s", ( arg && *arg ) ? KT_CMD_RES_TITLE : KT_CMD_RES_TITLE_RESET ) ;
	return 1 ;
}

static int cmd_transparency( HWND hwnd, char * arg ) {
	(void)arg ;
#ifndef MOD_NOTRANSPARENCY
	/* The same two opt-outs as the menu and the keyboard: a session at -1 is
	 * not to be dimmed, and transparency=no in kitty.ini is not a state this
	 * command may leave. */
	if( conf_get_int(conf,CONF_transparencynumber) == -1 || !GetTransparencyAllowed() ) {
		cmd_say( "%s", KT_CMD_RES_TRANSPARENCY_NO ) ;
		return 1 ;
	}
	/* A Direct2D window is layered from its creation or not at all: its
	 * layered style is never switched while it is open (windows/window.c,
	 * kitty_window_may_layer), so "off" keeps it layered at full opacity. */
	if( TransparencyFlag == 0 ) {
		if( !kitty_window_may_layer( MainHwnd ) ) return 1 ;
		TransparencyFlag = 1 ;
		SetWindowLongPtr(MainHwnd, GWL_EXSTYLE, GetWindowLong(MainHwnd, GWL_EXSTYLE) | WS_EX_LAYERED ) ;
		SetWindowPos( MainHwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER ) ;
		if( conf_get_int(conf,CONF_transparencynumber) == -1 ) conf_set_int(conf,CONF_transparencynumber,0) ;
		SetTransparency( MainHwnd, 255-conf_get_int(conf,CONF_transparencynumber) ) ;
		SetForegroundWindow( hwnd ) ;
		cmd_say_toggle( KT_CMD_RES_TRANSPARENCY, 1 ) ;
	} else {
		TransparencyFlag = 0 ;
		SetTransparency( MainHwnd, 255 ) ;
		if( (ULONG_PTR)GetPropA( MainHwnd, "KiTTY.renderer" ) <= 1 ) {
			SetWindowLongPtr(MainHwnd, GWL_EXSTYLE, GetWindowLong(MainHwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED ) ;
			RedrawWindow(MainHwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
			SetWindowPos( MainHwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER ) ;
		}
		SetForegroundWindow( hwnd ) ;
		cmd_say_toggle( KT_CMD_RES_TRANSPARENCY, 0 ) ;
	}
#else
	(void)hwnd ;
#endif
	return 1 ;
}

static int cmd_backgroundimage( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	SetBackgroundImageFlag( abs( GetBackgroundImageFlag() - 1 ) ) ;
	cmd_say_toggle( KT_CMD_RES_BACKGROUND, GetBackgroundImageFlag() ) ;
	return 1 ;
}

static int cmd_hyperlink( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	HyperlinkFlag = abs( HyperlinkFlag - 1 ) ;
	cmd_say_toggle( KT_CMD_RES_HYPERLINK, HyperlinkFlag ) ;
	return 1 ;
}

static int cmd_winroll( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	WinrolFlag = abs( WinrolFlag - 1 ) ;
	cmd_say_toggle( KT_CMD_RES_WINROLL, WinrolFlag ) ;
	return 1 ;
}

static int cmd_redraw( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	InvalidateRect( MainHwnd, NULL, TRUE ) ;
	cmd_say( "%s", KT_CMD_RES_REDRAW ) ;
	return 1 ;
}

static int cmd_refresh( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	RefreshBackground( MainHwnd ) ;
	cmd_say( "%s", KT_CMD_RES_REDRAW ) ;
	return 1 ;
}

/* ---- Info ---- */

/* /init, /session, /urlregex: the answer on the result line, which grows to
 * hold /init's six lines. Each used to come in a message box of its own. */
static int cmd_init( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	cmd_say( KT_CMD_RES_INIT, ConfigDirectory ? ConfigDirectory : "", IniFileFlag,
	         InitialDirectory, KittyIniFile ? KittyIniFile : "",
	         KittySavFile ? KittySavFile : "", KiTTYClassName ) ;
	return 1 ;
}

static int cmd_session( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	if( strlen( conf_get_str(conf,CONF_sessionname) ) > 0 )
		cmd_say( KT_CMD_RES_SESSION, conf_get_str(conf,CONF_sessionname) ) ;
	else
		cmd_say( "%s", KT_CMD_RES_NO_SESSION_NAME ) ;
	return 1 ;
}

/* The pattern in force, as kitty_url_config picks it: the built-in default
 * unless the session turned it off, then the session's own - with the
 * engine's reason when that one does not compile (the terminal then finds
 * links with the default, kitty/url/urlhack.c). */
static int cmd_urlregex( HWND hwnd, char * arg ) {
	const char * re = conf_get_str( conf, CONF_url_regex ) ;
	const char * err ;
	(void)hwnd ; (void)arg ;
	if( conf_get_int( conf, CONF_url_defregex ) != 0 )
		cmd_say( KT_CMD_RES_URLREGEX_DEFAULT, KITTY_URL_REGEX_DEFAULT ) ;
	else if( re == NULL || !re[0] )
		cmd_say( "%s", KT_CMD_RES_URLREGEX_EMPTY ) ;
	else if( (err = kitty_urlregex_error( re )) != NULL )
		cmd_say( KT_CMD_RES_URLREGEX_BAD, err, re ) ;
	else
		cmd_say( KT_CMD_RES_URLREGEX_CUSTOM, re ) ;
	return 1 ;
}

static int cmd_help( HWND hwnd, char * arg ) ;

/* ---- Settings & storage ---- */

/* readonly=yes: a command that writes the settings store refuses on the
 * result line, naming the kitty.ini that sets it. 1 = refused. */
static int cmd_store_refused( void ) {
	if( !GetReadOnlyFlag() ) return 0 ;
	cmd_say( KT_STORE_RO_NOT_SAVED, kitty_store_ro_inifile() ) ;
	return 1 ;
}

static int cmd_save( HWND hwnd, char * arg ) {
	char * out ;
	(void)hwnd ; (void)arg ;
	if( strlen( conf_get_str(conf,CONF_sessionname) ) > 0 ) {
		kitty_save_current_session( NULL, &out ) ;
		cmd_say( "%s", out ? out : "" ) ;
		sfree( out ) ;
	} else {
		cmd_say( "%s", KT_CMD_RES_SAVE_NO_SESSION ) ;
	}
	return 1 ;
}

static int cmd_savenew( HWND hwnd, char * arg ) {
	char * out ;
	(void)hwnd ;
	kitty_save_current_session( arg, &out ) ;
	if( out != NULL && out[0] ) cmd_say( "%s", out ) ;
	sfree( out ) ;
	return 1 ;
}

/* /savektx: the Save-As box runs beside the session (kitty_pick_async), not
 * on this thread - its own message loop would stop the session while it is
 * open. The file is written when it has been picked, with the settings live
 * at that moment, and the outcome goes to the result line then. One box at
 * a time: the token is set while it is open. */
static void * savektx_pick = NULL ;

static void savektx_picked( void * ctx, const char * path ) {
	char * out = NULL ;
	(void)ctx ;
	savektx_pick = NULL ;
	if( path == NULL )
		cmd_say( "%s", KT_CMD_RES_CANCELLED ) ;
	else if( SaveCurrentSettingTo( path, &out ) == 1 )
		cmd_say( KT_CMD_RES_SAVED, out ) ;
	else
		cmd_say( KT_CMD_RES_NOT_SAVED, out ) ;
	sfree( out ) ;
	kitty_inputbox_result( kitty_cmd_result() ) ;
}

static int cmd_savektx( HWND hwnd, char * arg ) {
	char * out = NULL, * filter ;
	(void)arg ;
	if( savektx_pick != NULL ) {
		cmd_say( "%s", KT_CMD_RES_SAVEKTX_OPEN ) ;
		return 1 ;
	}
	filter = SaveCurrentSettingFilterDup() ;
	savektx_pick = kitty_pick_async( hwnd, KITTY_PICK_SAVE, NULL, KT_CAP_SAVE_FILE, filter,
	                                 savektx_picked, NULL ) ;
	sfree( filter ) ;   /* the picker keeps a copy of its own */
	if( savektx_pick != NULL ) {
		cmd_say( "%s", KT_CMD_RES_SAVEKTX_PENDING ) ;
		return 1 ;
	}
	/* no thread for the picker: on this one, as before */
	switch( SaveCurrentSetting( hwnd, &out ) ) {
	  case 1:  cmd_say( KT_CMD_RES_SAVED, out ) ; break ;
	  case 0:  cmd_say( "%s", KT_CMD_RES_CANCELLED ) ; break ;
	  default: cmd_say( KT_CMD_RES_NOT_SAVED, out ) ; break ;
	}
	sfree( out ) ;
	return 1 ;
}

/* /savemode: the next mode and whether kitty.ini now holds it - judged by
 * reading it back (delINI also returns 0 when there was nothing to remove).
 * It wrote unchecked and showed a fixed box, so a kitty.ini that could not
 * be written still read "Savemode is ..." although the next start would use
 * the old mode. */
static int cmd_savemode( HWND hwnd, char * arg ) {
	const char * mode ;
	char v[32] = "" ;
	int stored, have ;
	DWORD err = 0 ;
	(void)hwnd ; (void)arg ;
	IniFileFlag++ ; if( IniFileFlag>SAVEMODE_DIR ) IniFileFlag = 0 ;
	mode = IniFileFlag == SAVEMODE_FILE ? "file" : IniFileFlag == SAVEMODE_DIR ? "dir" : "registry" ;
	if( NoKittyFileFlag || KittyIniFile == NULL || !KittyIniFile[0] ) {
		cmd_say( KT_CMD_RES_SAVEMODE_NOINI, mode ) ;
		return 1 ;
	}
	if( kitty_inifile_write_blocked( KittyIniFile ) ) {
		cmd_say( KT_CMD_RES_SAVEMODE_READONLY, mode, KittyIniFile ) ;
		return 1 ;
	}
	if( IniFileFlag == SAVEMODE_FILE ) {
		if( !writeINI( KittyIniFile, INIT_SECTION, KI_SAVEMODE, "file" ) ) err = GetLastError() ;
	} else if( !delINI( KittyIniFile, INIT_SECTION, KI_SAVEMODE ) ) err = GetLastError() ;
	have = readINI( KittyIniFile, INIT_SECTION, KI_SAVEMODE, v, sizeof(v) ) && v[0] ;
	stored = IniFileFlag == SAVEMODE_FILE ? ( have && !stricmp( v, "file" ) ) : !have ;
	if( stored ) cmd_say( KT_CMD_RES_SAVEMODE, mode, KittyIniFile ) ;
	else cmd_say( KT_CMD_RES_SAVEMODE_NOT_STORED, mode, KittyIniFile, kitty_win_reason( (int)( err ? err : ERROR_WRITE_FAULT ) ) ) ;
	return 1 ;
}

/* /savereg: a backup of the store NOW, and its file on the result line. It
 * used to go through the routine snapshot, which writes only when the store
 * changed since the last one - so the command often did nothing, silently. */
static int cmd_savereg( HWND hwnd, char * arg ) {
	char * path = NULL ;   /* the backup file in full */
	(void)hwnd ; (void)arg ;
	if( cmd_store_refused() ) return 1 ;   /* no backup under readonly=yes */
	switch( kitty_sav_backup_report_dup( &path ) ) {
	  case 1:  cmd_say( KT_CMD_RES_SAVED, path ? path : "" ) ; break ;
	  case -1: cmd_say( "%s", IniFileFlag == SAVEMODE_DIR ? KT_CMD_RES_BACKUP_OFF_DIR
	                                                      : KT_CMD_RES_BACKUP_OFF ) ; break ;
	  case -2: cmd_say( "%s", KT_CMD_RES_BACKUP_NONE ) ; break ;
	  default: cmd_say( KT_CMD_RES_NOT_SAVED, kitty_sav_save_reason()[0] ? kitty_sav_save_reason()
	                                                                     : ( path ? path : "" ) ) ; break ;
	}
	sfree( path ) ;
	return 1 ;
}

/* /loadreg: the backup a restore takes - the NEWEST dated one, as start-up
 * picks it (sav_find_for_restore). It used to read only the fixed name
 * kittynew.sav, which nothing writes any more (backups are dated), and so
 * loaded an old kitty.sav or nothing, without a word either way. */
static int cmd_loadreg( HWND hwnd, char * arg ) {
	char * found = NULL, * dir ;   /* both in full */
	(void)hwnd ; (void)arg ;
	if( cmd_store_refused() ) return 1 ;   /* nothing loaded into the store */
	if( (KittySavFile == NULL) || !KittySavFile[0] ||
	    ( found = sav_find_for_restore_dup( KittySavFile ) ) == NULL ) {
		dir = kitty_sav_dir_dup() ;
		cmd_say( KT_CMD_RES_NOTHING_TO_LOAD, dir ? dir : "" ) ;
		sfree( dir ) ;
		return 1 ;
	}
	{	char * saved = KittySavFile ;
		int loaded ;
		KittySavFile = found ;          /* LoadRegistryKey reads the global */
		loaded = LoadRegistryKeyLive() ;   /* never ends the session */
		KittySavFile = saved ;
		/* "Loaded:" only when the import did: reg.exe's result was thrown away */
		if( !loaded ) { cmd_say( KT_CMD_RES_NOT_LOADED, kitty_sav_load_reason() ) ; sfree( found ) ; return 1 ; }
	}
	cmd_say( KT_CMD_RES_LOADED, found ) ;
	sfree( found ) ;
	return 1 ;
}

/* The answer to /delreg's question, whenever it comes: the box is modeless,
 * so the session runs on while it waits. */
static HWND delreg_question = NULL ;

static void delreg_answered( int yes, void * ctx ) {
	(void)ctx ;
	delreg_question = NULL ;
	if( !yes )
		cmd_say( "%s", KT_CMD_RES_NOTHING_DELETED ) ;
	/* judged by the key being gone: RegDelTree's own answer is not trusted
	 * alone, a key still there is the failure that matters */
	else if( RegDelTree( HKEY_CURRENT_USER, kitty_registry_base() ) && !RegTestKey( HKEY_CURRENT_USER, kitty_registry_base() ) )
		cmd_say( KT_CMD_RES_DELETED, kitty_registry_base() ) ;
	else
		cmd_say( KT_CMD_RES_NOT_DELETED, kitty_registry_base(), kitty_win_reason( GetLastError() ) ) ;
	kitty_inputbox_result( kitty_cmd_result() ) ;
}

static int cmd_delreg( HWND hwnd, char * arg ) {
	/* Deletes KiTTY's OWN hive - after a Yes to a question that defaults to
	 * No. The question is modeless (kitty_confirm_modeless): a message box
	 * held the session still for as long as it stood unanswered.
	 *
	 * It used to delete TEXT(PUTTY_REG_PARENT), i.e. Software\kapper.net: the
	 * VENDOR key, not ours. Anything else stored under that vendor was deleted,
	 * on one typed word, with no confirmation and nothing to undo it. It also
	 * used the compile-time macro, so with KiClassName=PuTTY it deleted a hive
	 * the running KiTTY was not even using while leaving the one it WAS using
	 * untouched - the same compile-time/runtime split fixed in kitty_proxy.c.
	 *
	 * kitty_registry_base() is the hive this process actually reads and writes.
	 * The confirmation is not decoration: this is the only command in the
	 * console that destroys data outright, and the person typing it is usually
	 * aiming at "clear my settings", not "clear everything any kapper.net
	 * program ever stored". */
	char question[1024] ;
	(void)arg ;
	/* readonly=yes: refused before the question - nothing would be deleted */
	if( cmd_store_refused() ) return 1 ;
	if( delreg_question != NULL && IsWindow( delreg_question ) ) {
		SetForegroundWindow( delreg_question ) ;   /* one question at a time */
		cmd_say( KT_CMD_RES_DELREG_PENDING, kitty_registry_base() ) ;
		return 1 ;
	}
	snprintf( question, sizeof(question),
		KT_CMD_DELREG_QUESTION,
		kitty_registry_base() ) ;
	delreg_question = kitty_confirm_modeless( hwnd, KT_CAP_DELETE_HIVE, question,
	                                          NULL, NULL, delreg_answered, NULL ) ;
	if( delreg_question == NULL ) {   /* not made, no callback: that is a No */
		cmd_say( "%s", KT_CMD_RES_NOTHING_DELETED ) ;
		return 1 ;
	}
	cmd_say( KT_CMD_RES_DELREG_PENDING, kitty_registry_base() ) ;
	return 1 ;
}

/* /savesessions: the Sessions key as kitty.ses, beside the .sav backups. It
 * used to go to the program folder (a relative name after chdir there), which
 * an installed copy cannot write to - so no file, and no word about it. */
static int cmd_savesessions( HWND hwnd, char * arg ) {
	/* the folder and the file in full: one too long for Windows makes the
	 * export fail with its reason, never a file under a shorter name */
	const char * buffer = kitty_reg_sessions() ;
	char * dir, * file ;
	(void)hwnd ; (void)arg ;
	{	/* no Sessions key: reg.exe has nothing to export - report why, rather
		 * than a bare "not saved" */
		HKEY k ;
		if( RegOpenKeyExA( HKEY_CURRENT_USER, buffer, 0, KEY_READ, &k ) != ERROR_SUCCESS ) {
			cmd_say( KT_CMD_RES_NO_SESSIONS, buffer ) ;
			return 1 ;
		}
		RegCloseKey( k ) ;
	}
	dir = kitty_sav_dir_dup() ;
	if( dir == NULL ) dir = dupstr( "" ) ;
	CreateDirectoryA( dir, NULL ) ;
	file = dupprintf( "%s\\kitty.ses", dir ) ;
	sfree( dir ) ;
	if( SaveRegistryKeyEx( HKEY_CURRENT_USER, buffer, file ) )
		cmd_say( KT_CMD_RES_SAVED, file ) ;
	else
		cmd_say( KT_CMD_RES_NOT_SAVED, kitty_sav_save_reason() ) ;
	sfree( file ) ;
	return 1 ;
}

static int cmd_copytoputty( HWND hwnd, char * arg ) {
	const char * buffer ;   /* the hive's key names, as kitty_storage.c holds them */
	(void)hwnd ; (void)arg ;
	/* REFUSE when our own hive IS PuTTY's (kitty.ini KiClassName=PuTTY).
	 * This deletes PuTTY\Sessions before copying into it, so in that mode it
	 * would delete the sessions and then copy the emptied key over itself -
	 * total loss, from a command that reads like a backup. */
	if( kitty_root_is_putty() ) {
		cmd_say( "%s", KT_CMD_RES_COPYTOPUTTY_SAME_HIVE ) ;
		return 1 ;
	}
	/* Copied into a staging key FIRST, PuTTY's sessions replaced only by
	 * a whole copy: they were deleted before an unchecked copy, so a copy
	 * that failed left PuTTY with nothing - and the command reported nothing. */
	{
		static const char *stage = "Software\\SimonTatham\\PuTTY\\Sessions.kittycopy" ;
		static const char *dest = "Software\\SimonTatham\\PuTTY\\Sessions" ;
		DWORD n = 0 ;
		HKEY k ;
		buffer = kitty_reg_sessions() ;
		if( !RegTestKey( HKEY_CURRENT_USER, buffer ) ) {
			cmd_say( KT_CMD_RES_NO_SESSIONS, buffer ) ;
			return 1 ;
		}
		RegDelTree( HKEY_CURRENT_USER, stage ) ;
		if( !kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, stage ) ) {
			DWORD err = GetLastError() ;
			RegDelTree( HKEY_CURRENT_USER, stage ) ;
			cmd_say( KT_CMD_RES_NOT_COPIED_TO_PUTTY, kitty_win_reason( err ) ) ;
			return 1 ;
		}
		RegDelTree( HKEY_CURRENT_USER, dest ) ;
		if( !RegRenameTree( NULL, HKEY_CURRENT_USER, stage, dest ) ) {
			cmd_say( KT_CMD_RES_NOT_COPIED_TO_PUTTY, kitty_win_reason( GetLastError() ) ) ;
			return 1 ;
		}
		if( RegOpenKeyExA( HKEY_CURRENT_USER, dest, 0, KEY_READ, &k ) == ERROR_SUCCESS ) {
			RegQueryInfoKeyA( k, NULL, NULL, NULL, &n, NULL, NULL, NULL, NULL, NULL, NULL, NULL ) ;
			RegCloseKey( k ) ;
		}
		buffer = kitty_reg_hostkeys() ;
		if( RegTestKey( HKEY_CURRENT_USER, buffer ) &&
		    !kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, "Software\\SimonTatham\\PuTTY\\SshHostKeys" ) ) {
			DWORD err = GetLastError() ;
			RegCleanPuTTY() ;
			cmd_say( KT_CMD_RES_HOSTKEYS_NOT_COPIED, (unsigned long)n, kitty_win_reason( err ) ) ;
			return 1 ;
		}
		RegCleanPuTTY() ;
		cmd_say( KT_CMD_RES_COPIED_TO_PUTTY, (unsigned long)n ) ;
	}
	return 1 ;
}

/*
 * /switchcrypt no longer does anything, and its result line states that
 * rather than the command disappearing.
 *
 * It used to toggle a global that made exported .ktx files "encrypted" with a
 * constant compiled into every build - obfuscation, not encryption, undoable by
 * anyone holding a copy of KiTTY. The write path is gone.
 *
 * The command is KEPT, answering with an explanation, because deleting it outright
 * would answer anyone who has it in their fingers with "unknown command" - which
 * gives them no reason, and looks like a bug rather than a decision.
 * Reading old encrypted .ktx files still works.
 */
static int cmd_switchcrypt( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	cmd_say( "%s", KT_CMD_RES_SWITCHCRYPT_REMOVED ) ;
	return 1 ;
}

static int cmd_delfolder( HWND hwnd, char * arg ) {
	(void)hwnd ;
	/* readonly=yes: the folder list is stored through WriteParameter, which
	 * refuses - the list stays as it is */
	if( cmd_store_refused() ) return 1 ;
	StringList_Del( FolderList, arg ) ;
	return 1 ;
}

static int cmd_loadinitscript( HWND hwnd, char * arg ) {
	(void)hwnd ;
	ReadInitScript( arg ) ;
	/*
	 * The other order of the same clash the Event Log warns about at connect:
	 * a login script loaded by hand while a rutty script is already configured.
	 * On the result line rather than in the log because the user is right
	 * here, having just typed the command - a log line is for something read
	 * later. The rutty panel is given, since the two features live in
	 * different places and each looks like "the" scripting one.
	 */
	if( ScriptFileContent != NULL && conf != NULL &&
	    conf_get_int( conf, CONF_script_mode ) == 1 ) {
		Filename *sf = conf_get_filename( conf, CONF_scriptfile ) ;
		if( sf && filename_to_str(sf)[0] )
			cmd_say( "%s", KT_CMD_RES_RUTTY_ALSO ) ;
	}
	return 1 ;
}

/* ---- All KiTTY windows ---- */

static int cmd_command( HWND hwnd, char * arg ) {
	SendCommandAllWindows( hwnd, arg ) ;
	return 1 ;
}

static int cmd_sizeall( HWND hwnd, char * arg ) {
	(void)arg ;
	ResizeWinList( hwnd, conf_get_int(conf,CONF_width), conf_get_int(conf,CONF_height) ) ;
	return 1 ;
}

/* ---- Behaviour & diagnostics ---- */

static int cmd_shortcuts( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	InitShortcuts() ;
	cmd_say( "%s", KT_CMD_RES_SHORTCUTS ) ;
	return 1 ;
}

static int cmd_noshortcuts( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	ShortcutsFlag = 0 ;
	cmd_say( "%s", KT_CMD_RES_NOSHORTCUTS ) ;
	return 1 ;
}

static int cmd_nomouseshortcuts( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	MouseShortcutsFlag = 0 ;
	cmd_say( "%s", KT_CMD_RES_NOMOUSESHORTCUTS ) ;
	return 1 ;
}

static int cmd_bcdelay( HWND hwnd, char * arg ) {
	(void)hwnd ;
	between_char_delay = (arg==NULL) ? 3 : atoi( arg ) ;
	cmd_say( KT_CMD_RES_BCDELAY, between_char_delay ) ;
	return 1 ;
}

static int cmd_printcharsize( HWND hwnd, char * arg ) {
	(void)hwnd ;
	PrintCharSize = atoi( arg ) ;
	cmd_say( KT_CMD_RES_NUMBER, "PrintCharSize", PrintCharSize ) ;
	return 1 ;
}

static int cmd_printmaxline( HWND hwnd, char * arg ) {
	(void)hwnd ;
	PrintMaxLinePerPage = atoi( arg ) ;
	cmd_say( KT_CMD_RES_NUMBER, "PrintMaxLinePerPage", PrintMaxLinePerPage ) ;
	return 1 ;
}

static int cmd_printmaxchar( HWND hwnd, char * arg ) {
	(void)hwnd ;
	PrintMaxCharPerLine = atoi( arg ) ;
	cmd_say( KT_CMD_RES_NUMBER, "PrintMaxCharPerLine", PrintMaxCharPerLine ) ;
	return 1 ;
}

#ifdef MOD_ZMODEM
static int cmd_zmodem( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	SetZModemFlag( abs(GetZModemFlag()-1) ) ;
	cmd_say_toggle( KT_CMD_RES_ZMODEM, GetZModemFlag() ) ;
	return 1 ;
}
#endif

static int cmd_fileassoc( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	/* KiTTY: from the /commands console there is no command line to add
	 * options to, so this is the plain form - never take the extension away
	 * from another program (say so with -fileassoc -force instead), and treat
	 * typing /fileassoc as the confirmation a portable copy would request. */
	CreateFileAssoc( 0, 0, 1 ) ;
	return 1 ;
}

#ifdef MOD_LAUNCHER
static int cmd_initlauncher( HWND hwnd, char * arg ) {
	int failed = 0, n ;
	DWORD err = 0 ;
	(void)hwnd ; (void)arg ;
	if( cmd_store_refused() ) return 1 ;   /* the Launcher copy is not rebuilt */
	n = InitLauncherRegistry( &failed, &err ) ;
	if( n < 0 )		/* the folder store's Launcher folder could not be made */
		cmd_say( KT_CMD_RES_LAUNCHER_DIR_FAILED, ConfigDirectory ? ConfigDirectory : "", kitty_win_reason( err ) ) ;
	else if( failed )
		cmd_say( KT_CMD_RES_LAUNCHER_PARTLY, n, failed, kitty_win_reason( err ) ) ;
	else
		cmd_say( KT_CMD_RES_LAUNCHER_REBUILT, n ) ;
	return 1 ;
}
#endif

static int cmd_debug( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	debug_flag = abs( debug_flag - 1 ) ;
	cmd_say_toggle( KT_CMD_RES_DEBUG, debug_flag ) ;
	return 1 ;
}

static int cmd_passwd( HWND hwnd, char * arg ) {
	(void)arg ;
	if( !debug_flag ) return 0 ;	/* debug mode only; otherwise treated as plain text */
	if( !kitty_pw_empty(conf,CONF_password) ) {
		char bufpass[KITTY_PW_MAX+1], buffer[KITTY_PW_MAX+256] ;
		/* Unwrapped into these buffers and burned again before returning; the
		 * value is already the password, so do NOT MASKPASS it. */
		kitty_pw_get( conf, CONF_password, bufpass, sizeof(bufpass) ) ;
		snprintf( buffer, sizeof(buffer), KT_CMD_PASSWORD_IS, bufpass ) ;
		SetTextToClipboard( bufpass ) ;
		smemclr(bufpass,sizeof(bufpass));
		MessageBox( hwnd, buffer, KT_CAP_PASSWORD, MB_OK|MB_ICONWARNING ) ;
		smemclr(buffer,sizeof(buffer));
	} else
		MessageBox( hwnd, KT_CMD_NO_PASSWORD, KT_CAP_PASSWORD, MB_OK|MB_ICONWARNING ) ;
	return 1 ;
}

/* /screenshot: the terminal's client area as a PNG.
 *
 * Where: an installed copy writes to the user's Pictures\KiTTY++ (made when
 * missing). It used to write next to kitty.exe, which for an MSI install is
 * C:\Program Files\KiTTY++ - not writable for a user, so the file never came
 * to be, and the result was not checked, so nothing reported it. A portable copy
 * keeps its own folder, which is writable by being portable.
 * The result line shows the file with its full path, or why it is not there. */
static int cmd_screenshot( HWND hwnd, char * arg ) {
	/* the folder and the file in full: one too long for Windows fails in
	 * CreateDirectory / CreateFile with its own reason, never a folder or a
	 * file under a shorter name */
	char * dir, * file ;
	time_t now = time( 0 ) ;
	struct tm * t = localtime( &now ) ;
	HANDLE h ;
	(void)arg ;
	if( IniFileFlag == SAVEMODE_DIR ) {
		dir = dupstr( InitialDirectory ) ;
	} else {
		char pics[MAX_PATH] ;   /* SHGetFolderPath's own size */
		if( SHGetFolderPathA( NULL, CSIDL_MYPICTURES | CSIDL_FLAG_CREATE, NULL,
		                      SHGFP_TYPE_CURRENT, pics ) == S_OK )
			dir = dupprintf( "%s\\KiTTY++", pics ) ;
		else
			dir = dupprintf( "%s\\KiTTY++", InitialDirectory ) ;
		if( !CreateDirectoryA( dir, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS ) {
			cmd_say( KT_CMD_RES_SHOT_NO_DIR, dir ) ;
			sfree( dir ) ;
			return 1 ;
		}
	}
	file = dupprintf( "%s\\screenshot-%04d%02d%02d-%02d%02d%02d.png", dir,
	          t ? t->tm_year + 1900 : 0, t ? t->tm_mon + 1 : 0, t ? t->tm_mday : 0,
	          t ? t->tm_hour : 0, t ? t->tm_min : 0, t ? t->tm_sec : 0 ) ;
	sfree( dir ) ;
	/* Can it be written? Tried first, so the line can give the reason. */
	h = CreateFileA( file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	                 FILE_ATTRIBUTE_NORMAL, NULL ) ;
	if( h == INVALID_HANDLE_VALUE ) {
		char why[256] ;
		DWORD e = GetLastError(), n ;
		n = FormatMessageA( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		                    NULL, e, 0, why, sizeof(why), NULL ) ;
		while( n > 0 && (why[n-1] == '\r' || why[n-1] == '\n' || why[n-1] == '.') )
			why[--n] = '\0' ;
		if( !n ) snprintf( why, sizeof(why), "error %lu", (unsigned long)e ) ;
		cmd_say( KT_CMD_RES_SHOT_FAILED, file, why ) ;
		sfree( file ) ;
		return 1 ;
	}
	CloseHandle( h ) ;
	if( screenCaptureClientRect( GetParent(hwnd), file ) )
		cmd_say( KT_CMD_RES_SHOT_SAVED, file ) ;
	else {
		DeleteFileA( file ) ;              /* not the empty file the test left */
		cmd_say( KT_CMD_RES_SHOT_FAILED, file, KT_CMD_RES_SHOT_NO_PICTURE ) ;
	}
	sfree( file ) ;
	return 1 ;
}

/* ---- The command table ---- */

#define CAT_WINDOW	KT_CMD_CAT_WINDOW
#define CAT_INFO	KT_CMD_CAT_INFO
#define CAT_STORE	KT_CMD_CAT_STORE
#define CAT_ALLWIN	KT_CMD_CAT_ALLWIN
#define CAT_DIAG	KT_CMD_CAT_DIAG

static const struct InternalCmdDef {
	const char * name ;
	IntCmdArg arg ;
	const char * argname ;	/* shown in /help, e.g. "<name>" / "[ms]" */
	const char * category ;	/* /help group heading; keep the table grouped */
	const char * help ;	/* the /help one-liner */
	IntCmdHandler handler ;
} internal_commands[] = {
	{ "/size",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_SIZE,			cmd_size },
	{ "/wintitle",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_WINTITLE,				cmd_wintitle },
	{ "/title",		IC_ARG_OPTIONAL, "[text]", CAT_WINDOW, KT_CMD_HELP_TITLE,					cmd_title },
	{ "/transparency",	IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_TRANSPARENCY,				cmd_transparency },
	{ "/backgroundimage",	IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_BACKGROUNDIMAGE,			cmd_backgroundimage },
	{ "/hyperlink",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_HYPERLINK,					cmd_hyperlink },
	{ "/winroll",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_WINROLL,			cmd_winroll },
	{ "/redraw",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_REDRAW,					cmd_redraw },
	{ "/refresh",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_REFRESH,				cmd_refresh },

	{ "/init",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   KT_CMD_HELP_INIT,				cmd_init },
	{ "/session",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   KT_CMD_HELP_SESSION,					cmd_session },
	{ "/urlregex",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   KT_CMD_HELP_URLREGEX,				cmd_urlregex },
	{ "/help",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   KT_CMD_HELP_HELP,					cmd_help },

	{ "/save",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  KT_CMD_HELP_SAVE,	cmd_save },
	{ "/savenew",		IC_ARG_REQUIRED, "<name>", CAT_STORE,  KT_CMD_HELP_SAVENEW,	cmd_savenew },
	{ "/savektx",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  KT_CMD_HELP_SAVEKTX,		cmd_savektx },
	{ "/savemode",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  KT_CMD_HELP_SAVEMODE,		cmd_savemode },
	{ "/savereg",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  KT_CMD_HELP_SAVEREG,		cmd_savereg },
	{ "/loadreg",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  KT_CMD_HELP_LOADREG,		cmd_loadreg },
	{ "/delreg",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  KT_CMD_HELP_DELREG,			cmd_delreg },
	{ "/savesessions",	IC_ARG_NONE,	 NULL,	   CAT_STORE,  KT_CMD_HELP_SAVESESSIONS,		cmd_savesessions },
	{ "/copytoputty",	IC_ARG_NONE,	 NULL,	   CAT_STORE,  KT_CMD_HELP_COPYTOPUTTY, cmd_copytoputty },
	{ "/switchcrypt",	IC_ARG_NONE,	 NULL,	   CAT_STORE,  KT_CMD_HELP_SWITCHCRYPT,	cmd_switchcrypt },
	{ "/delfolder",		IC_ARG_REQUIRED, "<name>", CAT_STORE,  KT_CMD_HELP_DELFOLDER,				cmd_delfolder },
	{ "/loadinitscript",	IC_ARG_OPTIONAL, "[file]", CAT_STORE,  KT_CMD_HELP_LOADINITSCRIPT,				cmd_loadinitscript },

	{ "/command",		IC_ARG_REQUIRED, "<text>", CAT_ALLWIN, KT_CMD_HELP_COMMAND,		cmd_command },
	{ "/sizeall",		IC_ARG_NONE,	 NULL,	   CAT_ALLWIN, KT_CMD_HELP_SIZEALL,		cmd_sizeall },

	{ "/shortcuts",		IC_ARG_NONE,	 NULL,	   CAT_DIAG,   KT_CMD_HELP_SHORTCUTS,			cmd_shortcuts },
	{ "/noshortcuts",	IC_ARG_NONE,	 NULL,	   CAT_DIAG,   KT_CMD_HELP_NOSHORTCUTS,			cmd_noshortcuts },
	{ "/nomouseshortcuts",	IC_ARG_NONE,	 NULL,	   CAT_DIAG,   KT_CMD_HELP_NOMOUSESHORTCUTS,			cmd_nomouseshortcuts },
	{ "/bcdelay",		IC_ARG_OPTIONAL, "[ms]",   CAT_DIAG,   KT_CMD_HELP_BCDELAY,				cmd_bcdelay },
	{ "/PrintCharSize",	IC_ARG_REQUIRED, "<n>",	   CAT_DIAG,   KT_CMD_HELP_PRINTCHARSIZE,					cmd_printcharsize },
	{ "/PrintMaxLinePerPage", IC_ARG_REQUIRED, "<n>",  CAT_DIAG,   KT_CMD_HELP_PRINTMAXLINE,				cmd_printmaxline },
	{ "/PrintMaxCharPerLine", IC_ARG_REQUIRED, "<n>",  CAT_DIAG,   KT_CMD_HELP_PRINTMAXCHAR,				cmd_printmaxchar },
#ifdef MOD_ZMODEM
	{ "/zmodem",		IC_ARG_NONE,	 NULL,	   CAT_DIAG,   KT_CMD_HELP_ZMODEM,		cmd_zmodem },
#endif
	{ "/fileassoc",		IC_ARG_NONE,	 NULL,	   CAT_DIAG,   KT_CMD_HELP_FILEASSOC,			cmd_fileassoc },
#ifdef MOD_LAUNCHER
	{ "/initlauncher",	IC_ARG_NONE,	 NULL,	   CAT_DIAG,   KT_CMD_HELP_INITLAUNCHER,			cmd_initlauncher },
#endif
	{ "/debug",		IC_ARG_NONE,	 NULL,	   CAT_DIAG,   KT_CMD_HELP_DEBUG,					cmd_debug },
	{ "/passwd",		IC_ARG_NONE,	 NULL,	   CAT_DIAG,   KT_CMD_HELP_PASSWD,	cmd_passwd },
	{ "/screenshot",	IC_ARG_NONE,	 NULL,	   CAT_DIAG,   KT_CMD_HELP_SCREENSHOT,			cmd_screenshot },
} ;

/* /help: generated from the table, grouped by category. Shown in a modeless,
 * resizable window (IDD_HELPBOX) owned by the terminal window, so the list
 * stays readable while commands are typed into the send-text box - the old
 * modal MessageBox blocked exactly that box. One instance per process; /help
 * with the window already open just brings it to the front. */

#include "kitty_auxpos.h"

static HWND kitty_help_dlg = NULL ;
static WNDPROC kitty_help_edit_proc = NULL ;

/* The classic EDIT control has no native Ctrl+A; give the read-only text
 * select-all so Ctrl+A, Ctrl+C grabs the whole list (Event Log parity).
 * Also drop DLGC_WANTALLKEYS from the multiline edit's dialog code: with it,
 * IsDialogMessage() would feed Esc and Tab to the edit (which ignores them)
 * instead of closing the window / cycling focus like every other dialog. */
static LRESULT CALLBACK HelpEditProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam ) {
	if( (msg == WM_KEYDOWN) && (wParam == 'A') && (GetKeyState(VK_CONTROL) & 0x8000) ) {
		SendMessage( hwnd, EM_SETSEL, 0, -1 ) ;
		return 0 ;
	}
	if( msg == WM_GETDLGCODE ) {
		LRESULT code = CallWindowProc( kitty_help_edit_proc, hwnd, msg, wParam, lParam ) ;
		return code & ~(LRESULT)(DLGC_WANTALLKEYS | DLGC_WANTTAB) ;
	}
	return CallWindowProc( kitty_help_edit_proc, hwnd, msg, wParam, lParam ) ;
}

/* Fill the client area with the text, Close button pinned bottom-right.
 * Sizes derive from the button's current (template+DPI-scaled) metrics, so
 * this stays correct at any DPI. */
static void help_box_layout( HWND hwnd ) {
	RECT rc, rb ;
	HWND edit = GetDlgItem( hwnd, IDC_HELPTEXT ) ;
	HWND btn = GetDlgItem( hwnd, IDCANCEL ) ;
	int bw, bh, m, btop ;
	if( !edit || !btn ) return ;
	GetClientRect( hwnd, &rc ) ;
	GetWindowRect( btn, &rb ) ;
	bw = rb.right - rb.left ; bh = rb.bottom - rb.top ;
	m = bh / 3 ;
	btop = rc.bottom - bh - m ;
	MoveWindow( edit, m, m, rc.right - 2*m, btop - 2*m, TRUE ) ;
	MoveWindow( btn, rc.right - bw - m, btop, bw, bh, TRUE ) ;
}

/* The same window serves "kitty.exe -h" (kitty_cli_help_box below): a run
 * that has no terminal and ends when the window closes. It remembers its place
 * under a name of its own, and closing it ends that run's message loop. */
static const char * kitty_help_poskey = KR_DLGPOS_CMDHELP ;
static int kitty_help_cli = 0 ;

static INT_PTR CALLBACK HelpBoxProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam ) {
	switch( msg ) {
		case WM_INITDIALOG: {
			HWND edit = GetDlgItem( hwnd, IDC_HELPTEXT ) ;
			kitty_help_edit_proc = (WNDPROC)SetWindowLongPtr( edit, GWLP_WNDPROC, (LONG_PTR)HelpEditProc ) ;
			kitty_dialog_icon( hwnd, NULL ) ;   /* the terminal's own icon */
			/* it can be resized: its size comes back too */
			if( !kitty_auxpos_restore( hwnd, kitty_help_poskey, 1, 300, 200 ) )
				kitty_auxpos_apply( hwnd, kitty_help_poskey, GetWindow(hwnd, GW_OWNER), 0 ) ;
			help_box_layout( hwnd ) ;
			/* in the theme from its first frame, as the command box */
			kitty_theme_apply_hooked( hwnd ) ;
			return 1 ;
		}
		case WM_SIZE:
			if( wParam != SIZE_MINIMIZED ) help_box_layout( hwnd ) ;
			return 0 ;
		case WM_GETMINMAXINFO: {
			/* Don't let it shrink below a readable minimum. */
			MINMAXINFO *mmi = (MINMAXINFO *)lParam ;
			mmi->ptMinTrackSize.x = 300 ;
			mmi->ptMinTrackSize.y = 200 ;
			return 0 ;
		}
		case WM_COMMAND:
			if( (LOWORD(wParam) == IDOK) || (LOWORD(wParam) == IDCANCEL) ) {
				DestroyWindow( hwnd ) ;
				return 1 ;
			}
			return 0 ;
		case WM_CLOSE:
			DestroyWindow( hwnd ) ;
			return 1 ;
		case WM_DESTROY:
			kitty_auxpos_save( hwnd, kitty_help_poskey ) ;
			ShinyRemoveAuxDialog( hwnd ) ;
			kitty_help_dlg = NULL ;
			if( kitty_help_cli ) PostQuitMessage( 0 ) ;
			return 0 ;
	}
	return 0 ;
}

/* "kitty.exe -h" with no console to answer on: the option list is some eighty
 * lines, and a message box grows to fit its text - taller than a small screen,
 * its button out of reach. The command list's window scrolls and resizes, so
 * the text goes there. Returns when the window has been closed; 0 when it
 * could not be created, and the caller falls back to the message box. */
int kitty_cli_help_box( const char * caption, const char * text ) {
	MSG m ;
	HWND d ;
	kitty_help_poskey = KR_DLGPOS_CLIHELP ;
	kitty_help_cli = 1 ;
	d = CreateDialog( hinst, MAKEINTRESOURCE(IDD_HELPBOX), NULL, HelpBoxProc ) ;
	if( !d ) { kitty_help_cli = 0 ; kitty_help_poskey = KR_DLGPOS_CMDHELP ; return 0 ; }
	kitty_help_dlg = d ;
	SetWindowText( d, caption ) ;
	SetDlgItemText( d, IDC_HELPTEXT, text ) ;
	ShowWindow( d, SW_SHOW ) ;
	SetForegroundWindow( d ) ;
	while( GetMessage( &m, NULL, 0, 0 ) > 0 ) {
		if( IsWindow( d ) && IsDialogMessage( d, &m ) ) continue ;
		TranslateMessage( &m ) ;
		DispatchMessage( &m ) ;
	}
	return 1 ;
}

static int cmd_help( HWND hwnd, char * arg ) {
	strbuf * buffer ;   /* the list in full: it outgrew a fixed 8 KB */
	const char * cat = NULL ;
	size_t i ;
	(void)hwnd ; (void)arg ;
	if( kitty_help_dlg && IsWindow( kitty_help_dlg ) ) {
		SetForegroundWindow( kitty_help_dlg ) ;
		return 1 ;
	}
	buffer = strbuf_new() ;
#define HCAT(s) put_dataz( buffer, s )
	for( i = 0 ; i < lenof(internal_commands) ; i++ ) {
		const struct InternalCmdDef * c = &internal_commands[i] ;
		if( (cat == NULL) || strcmp( cat, c->category ) ) {
			if( cat != NULL ) HCAT( "\r\n" ) ;
			HCAT( c->category ) ;
			HCAT( ":\r\n" ) ;
			cat = c->category ;
		}
		HCAT( "  " ) ;
		HCAT( c->name ) ;
		if( c->argname != NULL ) { HCAT( " " ) ; HCAT( c->argname ) ; }
		HCAT( " - " ) ;
		if( c->handler == cmd_fileassoc ) {
			char * h = dupprintf( c->help, FileExtension[0] ? FileExtension : ".ktx" ) ;
			HCAT( h ) ;
			sfree( h ) ;
		} else
			HCAT( c->help ) ;
		HCAT( "\r\n" ) ;
	}
#undef HCAT
	kitty_help_dlg = CreateDialog( hinst, MAKEINTRESOURCE(IDD_HELPBOX), MainHwnd, HelpBoxProc ) ;
	if( kitty_help_dlg ) {
		SetDlgItemText( kitty_help_dlg, IDC_HELPTEXT, buffer->s ) ;
		ShinyAddAuxDialog( kitty_help_dlg ) ;
		ShowWindow( kitty_help_dlg, SW_SHOW ) ;
		SetForegroundWindow( kitty_help_dlg ) ;
	}
	strbuf_free( buffer ) ;
	return 1 ;
}

int InternalCommand( HWND hwnd, char * st ) {
	size_t i ;
	for( i = 0 ; i < lenof(internal_commands) ; i++ ) {
		const struct InternalCmdDef * c = &internal_commands[i] ;
		size_t l = strlen( c->name ) ;
		int r ;
		if( (c->arg != IC_ARG_REQUIRED) && !strcmp( st, c->name ) ) {
			cmd_say_clear() ;
			r = c->handler( hwnd, NULL ) ;
		} else if( (c->arg != IC_ARG_NONE) && !strncmp( st, c->name, l ) && (st[l] == ' ') ) {
			cmd_say_clear() ;
			r = c->handler( hwnd, st+l+1 ) ;
		} else
			continue ;
		if( r && !kitty_cmd_result()[0] )
			cmd_say( KT_CMD_RES_DONE, c->name ) ;
		return r ;
	}
	return 0 ;
}
