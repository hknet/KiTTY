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
#include "kitty_commun.h"
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

/* /save + /savenew <name>: write the LIVE settings to a saved session. With a
 * name, the window's session identity switches to it first (save-as), so later
 * /save calls and save-on-exit land there. Mirrors the config-box Save button
 * (folder default + launcher refresh broadcast, cf. kitty_config.c). The
 * classic /save (.ktx file exporter, SaveCurrentSetting) lives on as /savektx. */
static void kitty_save_current_session( HWND hwnd, const char * newname ) {
	char buffer[1024] ;
	char * errmsg ;
	if( newname != NULL ) {
		while( newname[0]==' ' ) newname++ ;
		if( newname[0]=='\0' ) return ;
		conf_set_str( conf, CONF_sessionname, newname ) ;
	}
	if( strlen( conf_get_str(conf,CONF_folder) ) == 0 ) conf_set_str( conf, CONF_folder, "Default" ) ;
	errmsg = save_settings( conf_get_str(conf,CONF_sessionname), conf ) ;
	if( errmsg != NULL ) {
		MessageBox( hwnd, errmsg, KT_CAP_SAVE_SESSION, MB_OK|MB_ICONERROR ) ;
		sfree( errmsg ) ;
		return ;
	}
	if( newname != NULL ) kitty_set_last_session( conf_get_str(conf,CONF_sessionname) ) ;
	{	/* same best-effort refresh broadcast as the config-box Save button */
		UINT msg = RegisterWindowMessageA( "KiTTYLauncherRefreshSessionsAndHotkeys" ) ;
		if( msg ) PostMessageA( HWND_BROADCAST, msg, 0, 0 ) ;
	}
	snprintf( buffer, sizeof(buffer), KT_MAIN_SESSION_SAVED, conf_get_str(conf,CONF_sessionname) ) ;
	MessageBox( hwnd, buffer, KT_CAP_SAVE_SESSION, MB_OK|MB_ICONINFORMATION ) ;
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
 * nothing gets "Done: /name" from InternalCommand. A file is always named
 * with its full path - the person typing it must never have to guess. */
static char cmd_result[1024] ;

static void cmd_say( const char * fmt, ... ) {
	va_list ap ;
	va_start( ap, fmt ) ;
	vsnprintf( cmd_result, sizeof(cmd_result), fmt, ap ) ;
	va_end( ap ) ;
}

static void cmd_say_toggle( const char * what, int on ) {
	cmd_say( KT_CMD_RES_TOGGLE, what, on ? KT_CMD_RES_ON : KT_CMD_RES_OFF ) ;
}

const char * kitty_cmd_result( void ) { return cmd_result ; }

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

static int cmd_title( HWND hwnd, char * arg ) {
	(void)hwnd ;
	set_title( NULL, arg ) ;
	cmd_say( "%s", KT_CMD_RES_TITLE ) ;
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

static int cmd_init( HWND hwnd, char * arg ) {
	char buffer[4096] ;
	(void)arg ;
	snprintf( buffer, sizeof(buffer),KT_CMD_INIT_INFO
		,ConfigDirectory,IniFileFlag,DirectoryBrowseFlag,InitialDirectory,KittyIniFile,KittySavFile,KiTTYClassName ) ;
	MessageBox(hwnd,buffer,KT_CAP_CONFIG_INFO,MB_OK);
	return 1 ;
}

static int cmd_session( HWND hwnd, char * arg ) {
	(void)arg ;
	if( strlen( conf_get_str(conf,CONF_sessionname) ) > 0 ) {
		char buffer[1024] ;
		snprintf( buffer, sizeof(buffer), KT_CMD_SESSION_NAME_IS, conf_get_str(conf,CONF_sessionname) ) ;
		MessageBox( hwnd, buffer, KT_CAP_SESSION_NAME, MB_OK|MB_ICONWARNING ) ;
	} else
		MessageBox( hwnd, KT_CMD_NO_SESSION_NAME, KT_CAP_SESSION_NAME, MB_OK|MB_ICONWARNING ) ;
	return 1 ;
}

static int cmd_urlregex( HWND hwnd, char * arg ) {
	char b[1024] ;
	(void)hwnd ; (void)arg ;
	snprintf(b,sizeof(b),"%d: %s",conf_get_int(conf,CONF_url_defregex),conf_get_str(conf,CONF_url_regex));
	MessageBox( NULL, b, KT_CAP_URL_REGEX, MB_OK ) ;
	return 1 ;
}

static int cmd_message( HWND hwnd, char * arg ) {
	MessageBox( hwnd, arg, KT_CAP_INFO, MB_OK ) ;
	return 1 ;
}

static int cmd_help( HWND hwnd, char * arg ) ;

/* ---- Settings & storage ---- */

static int cmd_save( HWND hwnd, char * arg ) {
	(void)arg ;
	if( strlen( conf_get_str(conf,CONF_sessionname) ) > 0 ) {
		kitty_save_current_session( hwnd, NULL ) ;
	} else {
		MessageBox( hwnd, KT_CMD_SAVE_NO_SESSION,
		            KT_CAP_SAVE_SESSION, MB_OK|MB_ICONINFORMATION ) ;
	}
	return 1 ;
}

static int cmd_savenew( HWND hwnd, char * arg ) {
	kitty_save_current_session( hwnd, arg ) ;
	return 1 ;
}

static int cmd_savektx( HWND hwnd, char * arg ) {
	char out[4400] ;
	(void)arg ;
	switch( SaveCurrentSetting( hwnd, out, sizeof(out) ) ) {
	  case 1:  cmd_say( KT_CMD_RES_SAVED, out ) ; break ;
	  case 0:  cmd_say( "%s", KT_CMD_RES_CANCELLED ) ; break ;
	  default: cmd_say( KT_CMD_RES_NOT_SAVED, out ) ; break ;
	}
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
	char path[4096] ;
	(void)hwnd ; (void)arg ;
	switch( kitty_sav_backup_report( path, sizeof(path) ) ) {
	  case 1:  cmd_say( KT_CMD_RES_SAVED, path ) ; break ;
	  case -1: cmd_say( "%s", IniFileFlag == SAVEMODE_DIR ? KT_CMD_RES_BACKUP_OFF_DIR
	                                                      : KT_CMD_RES_BACKUP_OFF ) ; break ;
	  case -2: cmd_say( "%s", KT_CMD_RES_BACKUP_NONE ) ; break ;
	  default: cmd_say( KT_CMD_RES_NOT_SAVED, kitty_sav_save_reason()[0] ? kitty_sav_save_reason() : path ) ; break ;
	}
	return 1 ;
}

/* /loadreg: the backup a restore takes - the NEWEST dated one, as start-up
 * picks it (sav_find_for_restore). It used to read only the fixed name
 * kittynew.sav, which nothing writes any more (backups are dated), and so
 * loaded an old kitty.sav or nothing, without a word either way. */
static int cmd_loadreg( HWND hwnd, char * arg ) {
	char found[4096], dir[4096] ;
	(void)hwnd ; (void)arg ;
	if( (KittySavFile == NULL) || !KittySavFile[0] ||
	    !sav_find_for_restore( KittySavFile, found, sizeof(found) ) ) {
		kitty_sav_dir( dir, sizeof(dir) ) ;
		cmd_say( KT_CMD_RES_NOTHING_TO_LOAD, dir ) ;
		return 1 ;
	}
	{	char * saved = KittySavFile ;
		int loaded ;
		KittySavFile = found ;          /* LoadRegistryKey reads the global */
		loaded = LoadRegistryKey( NULL ) ;
		KittySavFile = saved ;
		/* "Loaded:" only when the import did: reg.exe's result was thrown away */
		if( !loaded ) { cmd_say( KT_CMD_RES_NOT_LOADED, kitty_sav_load_reason() ) ; return 1 ; }
	}
	cmd_say( KT_CMD_RES_LOADED, found ) ;
	return 1 ;
}

static int cmd_delreg( HWND hwnd, char * arg ) {
	/* Deletes KiTTY's OWN hive - and asks first.
	 *
	 * It used to delete TEXT(PUTTY_REG_PARENT), i.e. Software\kapper.net: the
	 * VENDOR key, not ours. Anything else stored under that vendor went with it,
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
	snprintf( question, sizeof(question),
		KT_CMD_DELREG_QUESTION,
		kitty_registry_base() ) ;
	if( MessageBox( hwnd, question, KT_CAP_DELETE_HIVE,
	                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 ) != IDYES ) {
		cmd_say( "%s", KT_CMD_RES_NOTHING_DELETED ) ;
		return 1 ;
	}
	/* judged by the key being gone: RegDelTree's own answer is not trusted
	 * alone, a key still there is the failure that matters */
	if( RegDelTree( HKEY_CURRENT_USER, kitty_registry_base() ) && !RegTestKey( HKEY_CURRENT_USER, kitty_registry_base() ) )
		cmd_say( KT_CMD_RES_DELETED, kitty_registry_base() ) ;
	else
		cmd_say( KT_CMD_RES_NOT_DELETED, kitty_registry_base(), kitty_win_reason( GetLastError() ) ) ;
	return 1 ;
}

/* /savesessions: the Sessions key as kitty.ses, beside the .sav backups. It
 * used to go to the program folder (a relative name after chdir there), which
 * an installed copy cannot write to - so no file, and no word about it. */
static int cmd_savesessions( HWND hwnd, char * arg ) {
	char buffer[4096], dir[4096], file[4200] ;
	(void)hwnd ; (void)arg ;
	kitty_sav_dir( dir, sizeof(dir) ) ;
	CreateDirectoryA( dir, NULL ) ;
	snprintf( file, sizeof(file), "%s\\kitty.ses", dir ) ;
	snprintf( buffer, sizeof(buffer), "%s", kitty_reg_sessions() ) ;
	{	/* no Sessions key: reg.exe has nothing to export - say why, rather
		 * than a bare "not saved" */
		HKEY k ;
		if( RegOpenKeyExA( HKEY_CURRENT_USER, buffer, 0, KEY_READ, &k ) != ERROR_SUCCESS ) {
			cmd_say( KT_CMD_RES_NO_SESSIONS, buffer ) ;
			return 1 ;
		}
		RegCloseKey( k ) ;
	}
	if( SaveRegistryKeyEx( HKEY_CURRENT_USER, buffer, file ) )
		cmd_say( KT_CMD_RES_SAVED, file ) ;
	else
		cmd_say( KT_CMD_RES_NOT_SAVED, kitty_sav_save_reason() ) ;
	return 1 ;
}

static int cmd_copytoputty( HWND hwnd, char * arg ) {
	char buffer[4096] ;
	(void)hwnd ; (void)arg ;
	/* REFUSE when our own hive IS PuTTY's (kitty.ini KiClassName=PuTTY).
	 * This deletes PuTTY\Sessions before copying into it, so in that mode it
	 * would delete the sessions and then copy the emptied key over itself -
	 * total loss, from a command that reads like a backup. */
	if( kitty_root_is_putty() ) {
		MessageBox( hwnd, KT_CMD_COPYTOPUTTY_SAME_HIVE,
			KT_CAP_COPY_TO_PUTTY, MB_OK | MB_ICONINFORMATION ) ;
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
		snprintf( buffer, sizeof(buffer), "%s", kitty_reg_sessions() ) ;
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
		snprintf( buffer, sizeof(buffer), "%s", kitty_reg_hostkeys() ) ;
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
 * /switchcrypt no longer does anything, and says so rather than disappearing.
 *
 * It used to toggle a global that made exported .ktx files "encrypted" with a
 * constant compiled into every build - obfuscation, not encryption, undoable by
 * anyone holding a copy of KiTTY. The write path is gone.
 *
 * The command is KEPT, answering with an explanation, because deleting it outright
 * would answer anyone who has it in their fingers with "unknown command" - which
 * tells them nothing about why, and looks like a bug rather than a decision.
 * Reading old encrypted .ktx files still works.
 */
static int cmd_switchcrypt( HWND hwnd, char * arg ) {
	(void)arg ;
	/* kitty_notice_box, not MessageBox: a real dialog, so the dialog manager
	 * gives it the shell font at the right DPI and it grows to fit the text -
	 * the same treatment every other KiTTY dialog gets. */
	kitty_notice_box( hwnd, KT_CAP_SETTING_REMOVED,
		KT_CMD_SWITCHCRYPT_REMOVED ) ;
	return 1 ;
}

static int cmd_delfolder( HWND hwnd, char * arg ) {
	(void)hwnd ;
	StringList_Del( FolderList, arg ) ;
	return 1 ;
}

static int cmd_loadinitscript( HWND hwnd, char * arg ) {
	ReadInitScript( arg ) ;
	/*
	 * The other order of the same clash the Event Log warns about at connect:
	 * a login script loaded by hand while a rutty script is already configured.
	 * Said in a box rather than the log because the user is right here, having
	 * just typed the command - a log line is for something they will read later.
	 * Both panels are named, since the difficulty is that the two features live
	 * in different places and each looks like "the" scripting one.
	 */
	if( ScriptFileContent != NULL && conf != NULL &&
	    conf_get_int( conf, CONF_script_mode ) == 1 ) {
		Filename *sf = conf_get_filename( conf, CONF_scriptfile ) ;
		if( sf && filename_to_str(sf)[0] )
			kitty_notice_box( hwnd, KT_CAP_RUTTY_ALSO,
				KT_CMD_RUTTY_ALSO_TEXT ) ;
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
	 * typing /fileassoc as the confirmation a portable copy would ask for. */
	CreateFileAssoc( 0, 0, 1 ) ;
	return 1 ;
}

#ifdef MOD_LAUNCHER
static int cmd_initlauncher( HWND hwnd, char * arg ) {
	int failed = 0, n ;
	DWORD err = 0 ;
	(void)hwnd ; (void)arg ;
	n = InitLauncherRegistry( &failed, &err ) ;
	if( failed )
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
	char dir[MAX_PATH], file[MAX_PATH + 64] ;
	time_t now = time( 0 ) ;
	struct tm * t = localtime( &now ) ;
	HANDLE h ;
	(void)arg ;
	if( IniFileFlag == SAVEMODE_DIR ) {
		snprintf( dir, sizeof(dir), "%s", InitialDirectory ) ;
	} else {
		char pics[MAX_PATH] ;
		if( SHGetFolderPathA( NULL, CSIDL_MYPICTURES | CSIDL_FLAG_CREATE, NULL,
		                      SHGFP_TYPE_CURRENT, pics ) != S_OK )
			snprintf( pics, sizeof(pics), "%s", InitialDirectory ) ;
		snprintf( dir, sizeof(dir), "%s\\KiTTY++", pics ) ;
		if( !CreateDirectoryA( dir, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS ) {
			cmd_say( KT_CMD_RES_SHOT_NO_DIR, dir ) ;
			return 1 ;
		}
	}
	snprintf( file, sizeof(file), "%s\\screenshot-%04d%02d%02d-%02d%02d%02d.png", dir,
	          t ? t->tm_year + 1900 : 0, t ? t->tm_mon + 1 : 0, t ? t->tm_mday : 0,
	          t ? t->tm_hour : 0, t ? t->tm_min : 0, t ? t->tm_sec : 0 ) ;
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
		return 1 ;
	}
	CloseHandle( h ) ;
	if( screenCaptureClientRect( GetParent(hwnd), file ) )
		cmd_say( KT_CMD_RES_SHOT_SAVED, file ) ;
	else {
		DeleteFileA( file ) ;              /* not the empty file the test left */
		cmd_say( KT_CMD_RES_SHOT_FAILED, file, KT_CMD_RES_SHOT_NO_PICTURE ) ;
	}
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
	{ "/title",		IC_ARG_REQUIRED, "<text>", CAT_WINDOW, KT_CMD_HELP_TITLE,					cmd_title },
	{ "/transparency",	IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_TRANSPARENCY,				cmd_transparency },
	{ "/backgroundimage",	IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_BACKGROUNDIMAGE,			cmd_backgroundimage },
	{ "/hyperlink",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_HYPERLINK,					cmd_hyperlink },
	{ "/winroll",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_WINROLL,			cmd_winroll },
	{ "/redraw",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_REDRAW,					cmd_redraw },
	{ "/refresh",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, KT_CMD_HELP_REFRESH,				cmd_refresh },

	{ "/init",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   KT_CMD_HELP_INIT,				cmd_init },
	{ "/session",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   KT_CMD_HELP_SESSION,					cmd_session },
	{ "/urlregex",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   KT_CMD_HELP_URLREGEX,				cmd_urlregex },
	{ "/message",		IC_ARG_REQUIRED, "<text>", CAT_INFO,   KT_CMD_HELP_MESSAGE,					cmd_message },
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
	char buffer[8192] ;
	const char * cat = NULL ;
	size_t i ;
	(void)hwnd ; (void)arg ;
	if( kitty_help_dlg && IsWindow( kitty_help_dlg ) ) {
		SetForegroundWindow( kitty_help_dlg ) ;
		return 1 ;
	}
	buffer[0] = '\0' ;
#define HCAT(s) strncat( buffer, s, sizeof(buffer)-strlen(buffer)-1 )
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
			char h[256] ;
			snprintf( h, sizeof(h), c->help, FileExtension[0] ? FileExtension : ".ktx" ) ;
			HCAT( h ) ;
		} else
			HCAT( c->help ) ;
		HCAT( "\r\n" ) ;
	}
#undef HCAT
	kitty_help_dlg = CreateDialog( hinst, MAKEINTRESOURCE(IDD_HELPBOX), MainHwnd, HelpBoxProc ) ;
	if( kitty_help_dlg ) {
		SetDlgItemText( kitty_help_dlg, IDC_HELPTEXT, buffer ) ;
		ShinyAddAuxDialog( kitty_help_dlg ) ;
		ShowWindow( kitty_help_dlg, SW_SHOW ) ;
		SetForegroundWindow( kitty_help_dlg ) ;
	}
	return 1 ;
}

int InternalCommand( HWND hwnd, char * st ) {
	size_t i ;
	for( i = 0 ; i < lenof(internal_commands) ; i++ ) {
		const struct InternalCmdDef * c = &internal_commands[i] ;
		size_t l = strlen( c->name ) ;
		int r ;
		if( (c->arg != IC_ARG_REQUIRED) && !strcmp( st, c->name ) ) {
			cmd_result[0] = '\0' ;
			r = c->handler( hwnd, NULL ) ;
		} else if( (c->arg != IC_ARG_NONE) && !strncmp( st, c->name, l ) && (st[l] == ' ') ) {
			cmd_result[0] = '\0' ;
			r = c->handler( hwnd, st+l+1 ) ;
		} else
			continue ;
		if( r && !cmd_result[0] )
			cmd_say( KT_CMD_RES_DONE, c->name ) ;
		return r ;
	}
	return 0 ;
}
