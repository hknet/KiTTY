/*
 * kitty_commands.c - the send-text-box internal commands ("/commands").
 * #include'd into kitty.c (same translation unit: relies on its globals).
 *
 * Every command is one row of internal_commands[]: the dispatcher
 * (InternalCommand) and the /help text are both generated from the table,
 * so a new command needs exactly one handler and one row. docs/COMMANDS.md
 * must carry a "### /name" section per row, in both directions - the
 * release guard parses the rows below and fails the release otherwise.
 */

typedef enum {
	IC_ARG_NONE,		/* exact match only: "/cmd" */
	IC_ARG_OPTIONAL,	/* "/cmd" or "/cmd <arg>" */
	IC_ARG_REQUIRED		/* "/cmd <arg>" only; a bare "/cmd" is not a command */
} IntCmdArg ;

/* Handlers return 1 = handled, 0 = not an internal command after all (the
 * input is then sent to the host like any other text). */
typedef int (*IntCmdHandler)( HWND hwnd, char * arg ) ;

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
	return 1 ;
}

static int cmd_wintitle( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	TitleBarFlag = abs( TitleBarFlag - 1 ) ;
	kitty_refresh_title() ;
	return 1 ;
}

static int cmd_title( HWND hwnd, char * arg ) {
	(void)hwnd ;
	set_title( NULL, arg ) ;
	return 1 ;
}

static int cmd_transparency( HWND hwnd, char * arg ) {
	(void)arg ;
#ifndef MOD_NOTRANSPARENCY
	/* The same two opt-outs as the menu and the keyboard: a session at -1 is
	 * not to be dimmed, and transparency=no in kitty.ini is not a state this
	 * command may leave. */
	if( conf_get_int(conf,CONF_transparencynumber) == -1 ) return 1 ;
	if( !GetTransparencyAllowed() ) return 1 ;
	if( TransparencyFlag == 0 ) {
		TransparencyFlag = 1 ;
		SetWindowLongPtr(MainHwnd, GWL_EXSTYLE, GetWindowLong(MainHwnd, GWL_EXSTYLE) | WS_EX_LAYERED ) ;
		SetWindowPos( MainHwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER ) ;
		if( conf_get_int(conf,CONF_transparencynumber) == -1 ) conf_set_int(conf,CONF_transparencynumber,0) ;
		SetTransparency( MainHwnd, 255-conf_get_int(conf,CONF_transparencynumber) ) ;
		SetForegroundWindow( hwnd ) ;
	} else {
		TransparencyFlag = 0 ;
		SetTransparency( MainHwnd, 255 ) ;
		SetWindowLongPtr(MainHwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED ) ;
		RedrawWindow(MainHwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
		SetWindowPos( MainHwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER ) ;
		SetForegroundWindow( hwnd ) ;
	}
#else
	(void)hwnd ;
#endif
	return 1 ;
}

static int cmd_backgroundimage( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	SetBackgroundImageFlag( abs( GetBackgroundImageFlag() - 1 ) ) ;
	return 1 ;
}

static int cmd_hyperlink( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	HyperlinkFlag = abs( HyperlinkFlag - 1 ) ;
	return 1 ;
}

static int cmd_winroll( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	WinrolFlag = abs( WinrolFlag - 1 ) ;
	return 1 ;
}

static int cmd_redraw( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	InvalidateRect( MainHwnd, NULL, TRUE ) ;
	return 1 ;
}

static int cmd_refresh( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	RefreshBackground( MainHwnd ) ;
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
	(void)arg ;
	SaveCurrentSetting(hwnd);
	return 1 ;
}

static int cmd_savemode( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	IniFileFlag++ ; if( IniFileFlag>SAVEMODE_DIR ) IniFileFlag = 0 ;
	if( IniFileFlag == SAVEMODE_REG )  {
		delINI( KittyIniFile, INIT_SECTION, "savemode" ) ;
		MessageBox( NULL, KT_CMD_SAVEMODE_REGISTRY, KT_CAP_INFO, MB_OK ) ;
	} else if( IniFileFlag == SAVEMODE_FILE ) {
		if(!NoKittyFileFlag) writeINI( KittyIniFile, INIT_SECTION, "savemode", "file" ) ;
		MessageBox( NULL, KT_CMD_SAVEMODE_FILE, KT_CAP_INFO, MB_OK ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		delINI( KittyIniFile, INIT_SECTION, "savemode" ) ;
		MessageBox( NULL, KT_CMD_SAVEMODE_DIR, KT_CAP_INFO, MB_OK ) ;
	}
	return 1 ;
}

static int cmd_savereg( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	chdir( InitialDirectory ) ;
	SaveRegistryKey() ;
	return 1 ;
}

static int cmd_loadreg( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	chdir( InitialDirectory ) ;
	LoadRegistryKey(NULL) ;
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
	extern const char *kitty_registry_base( void ) ;
	char question[1024] ;
	(void)arg ;
	snprintf( question, sizeof(question),
		KT_CMD_DELREG_QUESTION,
		kitty_registry_base() ) ;
	if( MessageBox( hwnd, question, KT_CAP_DELETE_HIVE,
	                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 ) != IDYES )
		return 1 ;
	RegDelTree( HKEY_CURRENT_USER, kitty_registry_base() ) ;
	return 1 ;
}

static int cmd_savesessions( HWND hwnd, char * arg ) {
	char buffer[4096] ;
	(void)hwnd ; (void)arg ;
	chdir( InitialDirectory ) ;
	snprintf( buffer, sizeof(buffer), "%s", kitty_reg_sessions() ) ;
	SaveRegistryKeyEx( HKEY_CURRENT_USER, buffer, "kitty.ses" ) ;
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
	RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Sessions" ) ;
	snprintf( buffer, sizeof(buffer), "%s", kitty_reg_sessions() ) ;
	kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, "Software\\SimonTatham\\PuTTY\\Sessions" ) ;
	snprintf( buffer, sizeof(buffer), "%s", kitty_reg_hostkeys() ) ;
	kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, "Software\\SimonTatham\\PuTTY\\SshHostKeys" ) ;
	RegCleanPuTTY() ;
	return 1 ;
}

static int cmd_copytokitty( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	/* Same trap the other way round: with KiClassName=PuTTY the destination
	 * IS the source, and a tree copied onto itself is at best pointless. */
	if( kitty_root_is_putty() ) {
		MessageBox( hwnd, KT_CMD_COPYTOKITTY_SAME_HIVE,
			KT_CAP_COPY_FROM_PUTTY, MB_OK | MB_ICONINFORMATION ) ;
		return 1 ;
	}
	kitty_RegCopyTree( HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY", kitty_registry_base() ) ;
	return 1 ;
}

/*
 * /switchcrypt no longer does anything, and says so rather than disappearing.
 *
 * It used to toggle a global that made exported .ktx files "encrypted" with a
 * constant compiled into every build - obfuscation, not encryption, undoable by
 * anyone holding a copy of KiTTY. The write path went on 2026-08-02.
 *
 * The command is KEPT, answering with an explanation, because deleting it outright
 * would answer anyone who has it in their fingers with "unknown command" - which
 * tells them nothing about why, and looks like a bug rather than a decision.
 * Reading old encrypted .ktx files still works.
 */
static int cmd_switchcrypt( HWND hwnd, char * arg ) {
	extern void kitty_notice_box( HWND owner, const char *caption, const char *text ) ; /* kitty_win.c */
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
	extern void kitty_notice_box( HWND owner, const char *caption, const char *text ) ; /* kitty_win.c */
	extern char * ScriptFileContent ;                                                  /* kitty.c */
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
	return 1 ;
}

static int cmd_noshortcuts( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	ShortcutsFlag = 0 ;
	return 1 ;
}

static int cmd_nomouseshortcuts( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	MouseShortcutsFlag = 0 ;
	return 1 ;
}

static int cmd_bcdelay( HWND hwnd, char * arg ) {
	(void)hwnd ;
	between_char_delay = (arg==NULL) ? 3 : atoi( arg ) ;
	return 1 ;
}

static int cmd_printcharsize( HWND hwnd, char * arg ) {
	(void)hwnd ;
	PrintCharSize = atoi( arg ) ;
	return 1 ;
}

static int cmd_printmaxline( HWND hwnd, char * arg ) {
	(void)hwnd ;
	PrintMaxLinePerPage = atoi( arg ) ;
	return 1 ;
}

static int cmd_printmaxchar( HWND hwnd, char * arg ) {
	(void)hwnd ;
	PrintMaxCharPerLine = atoi( arg ) ;
	return 1 ;
}

#ifdef MOD_ZMODEM
static int cmd_zmodem( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	SetZModemFlag( abs(GetZModemFlag()-1) ) ;
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
	(void)hwnd ; (void)arg ;
	InitLauncherRegistry() ;
	return 1 ;
}
#endif

static int cmd_debug( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	debug_flag = abs( debug_flag - 1 ) ;
	return 1 ;
}

static int cmd_passwd( HWND hwnd, char * arg ) {
	(void)arg ;
	if( !debug_flag ) return 0 ;	/* debug mode only; otherwise treated as plain text */
	if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
		char bufpass[4096], buffer[4096] ;
		strcpy( bufpass, conf_get_str(conf,CONF_password) ) ;
		/* plaintext at runtime; do NOT MASKPASS */
		snprintf( buffer, sizeof(buffer), KT_CMD_PASSWORD_IS, bufpass ) ;
		SetTextToClipboard( bufpass ) ;
		memset(bufpass,0,strlen(bufpass));
		MessageBox( hwnd, buffer, KT_CAP_PASSWORD, MB_OK|MB_ICONWARNING ) ;
		memset(buffer,0,strlen(buffer));
	} else
		MessageBox( hwnd, KT_CMD_NO_PASSWORD, KT_CAP_PASSWORD, MB_OK|MB_ICONWARNING ) ;
	return 1 ;
}

static int cmd_screenshot( HWND hwnd, char * arg ) {
	char screenShotFile[1024] ;
	(void)arg ;
	snprintf( screenShotFile, sizeof(screenShotFile), "%s\\screenshot-%d-%ld.jpg", InitialDirectory, getpid(), time(0) );
	screenCaptureClientRect( GetParent(hwnd), screenShotFile, 100 ) ;
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
	{ "/copytokitty",	IC_ARG_NONE,	 NULL,	   CAT_STORE,  KT_CMD_HELP_COPYTOKITTY,		cmd_copytokitty },
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

static INT_PTR CALLBACK HelpBoxProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam ) {
	switch( msg ) {
		case WM_INITDIALOG: {
			HWND edit = GetDlgItem( hwnd, IDC_HELPTEXT ) ;
			kitty_help_edit_proc = (WNDPROC)SetWindowLongPtr( edit, GWLP_WNDPROC, (LONG_PTR)HelpEditProc ) ;
			kitty_auxpos_apply( hwnd, "CmdHelp", GetWindow(hwnd, GW_OWNER), 0 ) ;
			help_box_layout( hwnd ) ;
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
			kitty_auxpos_save( hwnd, "CmdHelp" ) ;
			ShinyRemoveAuxDialog( hwnd ) ;
			kitty_help_dlg = NULL ;
			return 0 ;
	}
	return 0 ;
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
		if( (c->arg != IC_ARG_REQUIRED) && !strcmp( st, c->name ) )
			return c->handler( hwnd, NULL ) ;
		if( (c->arg != IC_ARG_NONE) && !strncmp( st, c->name, l ) && (st[l] == ' ') )
			return c->handler( hwnd, st+l+1 ) ;
	}
	return 0 ;
}
