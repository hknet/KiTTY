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
	if( (conf_get_int(conf,CONF_transparencynumber) == -1) || (TransparencyFlag == 0 ) ) {
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
	snprintf( buffer, sizeof(buffer),"ConfigDirectory=%s\nIniFileFlag=%d\nDirectoryBrowseFlag=%d\nInitialDirectory=%s\nKittyIniFile=%s\nKittySavFile=%s\nKiTTYClassName=%s\n"
		,ConfigDirectory,IniFileFlag,DirectoryBrowseFlag,InitialDirectory,KittyIniFile,KittySavFile,KiTTYClassName ) ;
	MessageBox(hwnd,buffer,"Configuration infomations",MB_OK);
	return 1 ;
}

static int cmd_session( HWND hwnd, char * arg ) {
	(void)arg ;
	if( strlen( conf_get_str(conf,CONF_sessionname) ) > 0 ) {
		char buffer[1024] ;
		snprintf( buffer, sizeof(buffer), "Your session name is\n-%s-", conf_get_str(conf,CONF_sessionname) ) ;
		MessageBox( hwnd, buffer, "Session name", MB_OK|MB_ICONWARNING ) ;
	} else
		MessageBox( hwnd, "No session name.", "Session name", MB_OK|MB_ICONWARNING ) ;
	return 1 ;
}

static int cmd_urlregex( HWND hwnd, char * arg ) {
	char b[1024] ;
	(void)hwnd ; (void)arg ;
	snprintf(b,sizeof(b),"%d: %s",conf_get_int(conf,CONF_url_defregex),conf_get_str(conf,CONF_url_regex));
	MessageBox( NULL, b, "URL regex", MB_OK ) ;
	return 1 ;
}

static int cmd_message( HWND hwnd, char * arg ) {
	MessageBox( hwnd, arg, "Info", MB_OK ) ;
	return 1 ;
}

static int cmd_help( HWND hwnd, char * arg ) ;

/* ---- Settings & storage ---- */

static int cmd_save( HWND hwnd, char * arg ) {
	(void)arg ;
	if( strlen( conf_get_str(conf,CONF_sessionname) ) > 0 ) {
		kitty_save_current_session( hwnd, NULL ) ;
	} else {
		MessageBox( hwnd, "No saved session is associated with this window.\n"
		            "Use  /savenew <name>  to create one.",
		            "Save session", MB_OK|MB_ICONINFORMATION ) ;
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
		MessageBox( NULL, "Savemode is \"registry\"", "Info", MB_OK ) ;
	} else if( IniFileFlag == SAVEMODE_FILE ) {
		if(!NoKittyFileFlag) writeINI( KittyIniFile, INIT_SECTION, "savemode", "file" ) ;
		MessageBox( NULL, "Savemode is \"file\"", "Info", MB_OK ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		delINI( KittyIniFile, INIT_SECTION, "savemode" ) ;
		MessageBox( NULL, "Savemode is \"dir\"", "Info", MB_OK ) ;
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
	(void)hwnd ; (void)arg ;
	RegDelTree (HKEY_CURRENT_USER, TEXT(PUTTY_REG_PARENT)) ;
	return 1 ;
}

static int cmd_savesessions( HWND hwnd, char * arg ) {
	char buffer[4096] ;
	(void)hwnd ; (void)arg ;
	chdir( InitialDirectory ) ;
	snprintf( buffer, sizeof(buffer), "%s\\Sessions", PUTTY_REG_POS ) ;
	SaveRegistryKeyEx( HKEY_CURRENT_USER, buffer, "kitty.ses" ) ;
	return 1 ;
}

static int cmd_copytoputty( HWND hwnd, char * arg ) {
	char buffer[4096] ;
	(void)hwnd ; (void)arg ;
	RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Sessions" ) ;
	snprintf( buffer, sizeof(buffer), "%s\\Sessions", PUTTY_REG_POS ) ;
	kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, "Software\\SimonTatham\\PuTTY\\Sessions" ) ;
	snprintf( buffer, sizeof(buffer), "%s\\SshHostKeys", PUTTY_REG_POS ) ;
	kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, "Software\\SimonTatham\\PuTTY\\SshHostKeys" ) ;
	RegCleanPuTTY() ;
	return 1 ;
}

static int cmd_copytokitty( HWND hwnd, char * arg ) {
	(void)hwnd ; (void)arg ;
	kitty_RegCopyTree( HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY", PUTTY_REG_POS ) ;
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
	kitty_notice_box( hwnd, "KiTTY - this setting has been removed",
		"Encrypted configuration files are no longer written.\n\n"
		"This setting used to scramble exported .ktx files with a key built "
		"into every copy of KiTTY, so anyone with KiTTY could unscramble them. "
		"It protected nothing, and it is gone.\n\n"
		"Existing encrypted .ktx files are still read normally. Saved passwords "
		"are unaffected - those are protected properly, with Windows DPAPI or "
		"your master password." ) ;
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
			kitty_notice_box( hwnd, "KiTTY - a rutty script is also configured",
				"A login script has just been loaded, and this session also has "
				"a rutty script.\n\n"
				"They are separate features. At CONNECT they are sequenced - the "
				"login script gets you in, then the rutty script sends its file. "
				"Loading one by hand mid-session skips that ordering, so if the "
				"rutty script is already running the two will now be watching "
				"the same output and both sending.\n\n"
				"You can see them here:\n"
				"    Session > Scripting        - the rutty script file\n"
				"    Connection > Data          - the login script\n\n"
				"Nothing has been stopped; this is only a warning." ) ;
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
		snprintf( buffer, sizeof(buffer), "Your password is\n-%s-", bufpass ) ;
		SetTextToClipboard( bufpass ) ;
		memset(bufpass,0,strlen(bufpass));
		MessageBox( hwnd, buffer, "Password", MB_OK|MB_ICONWARNING ) ;
		memset(buffer,0,strlen(buffer));
	} else
		MessageBox( hwnd, "No password.", "Password", MB_OK|MB_ICONWARNING ) ;
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

#define CAT_WINDOW	"Window & title (runtime toggles - persist via kitty.ini [KiTTY] size= / wintitle=)"
#define CAT_INFO	"Info"
#define CAT_STORE	"Settings & storage"
#define CAT_ALLWIN	"All KiTTY windows"
#define CAT_DIAG	"Behaviour & diagnostics"

static const struct InternalCmdDef {
	const char * name ;
	IntCmdArg arg ;
	const char * argname ;	/* shown in /help, e.g. "<name>" / "[ms]" */
	const char * category ;	/* /help group heading; keep the table grouped */
	const char * help ;	/* the /help one-liner */
	IntCmdHandler handler ;
} internal_commands[] = {
	{ "/size",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, "toggle the [rows x cols] title suffix",			cmd_size },
	{ "/wintitle",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, "toggle the title decorations",				cmd_wintitle },
	{ "/title",		IC_ARG_REQUIRED, "<text>", CAT_WINDOW, "set the window title",					cmd_title },
	{ "/transparency",	IC_ARG_NONE,	 NULL,	   CAT_WINDOW, "toggle window transparency",				cmd_transparency },
	{ "/backgroundimage",	IC_ARG_NONE,	 NULL,	   CAT_WINDOW, "toggle the background image feature",			cmd_backgroundimage },
	{ "/hyperlink",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, "toggle clickable URLs",					cmd_hyperlink },
	{ "/winroll",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, "toggle title-bar double-click roll-up",			cmd_winroll },
	{ "/redraw",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, "repaint the window",					cmd_redraw },
	{ "/refresh",		IC_ARG_NONE,	 NULL,	   CAT_WINDOW, "refresh the background image",				cmd_refresh },

	{ "/init",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   "show configuration paths",				cmd_init },
	{ "/session",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   "show the session name",					cmd_session },
	{ "/urlregex",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   "show the URL detection regex",				cmd_urlregex },
	{ "/message",		IC_ARG_REQUIRED, "<text>", CAT_INFO,   "show a message box",					cmd_message },
	{ "/help",		IC_ARG_NONE,	 NULL,	   CAT_INFO,   "show this list",					cmd_help },

	{ "/save",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  "save the live settings to this window's saved session",	cmd_save },
	{ "/savenew",		IC_ARG_REQUIRED, "<name>", CAT_STORE,  "save as a NEW session and switch this window to it",	cmd_savenew },
	{ "/savektx",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  "export the settings to a .ktx connection file",		cmd_savektx },
	{ "/savemode",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  "cycle the save mode (registry / file / dir)",		cmd_savemode },
	{ "/savereg",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  "export the KiTTY registry to kitty.sav",		cmd_savereg },
	{ "/loadreg",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  "import the KiTTY registry from kitty.sav",		cmd_loadreg },
	{ "/delreg",		IC_ARG_NONE,	 NULL,	   CAT_STORE,  "DELETE the whole KiTTY registry",			cmd_delreg },
	{ "/savesessions",	IC_ARG_NONE,	 NULL,	   CAT_STORE,  "export the saved sessions to kitty.ses",		cmd_savesessions },
	{ "/copytoputty",	IC_ARG_NONE,	 NULL,	   CAT_STORE,  "copy the sessions to stock PuTTY (replaces its sessions)", cmd_copytoputty },
	{ "/copytokitty",	IC_ARG_NONE,	 NULL,	   CAT_STORE,  "copy stock PuTTY's sessions into KiTTY",		cmd_copytokitty },
	{ "/switchcrypt",	IC_ARG_NONE,	 NULL,	   CAT_STORE,  "(removed) encrypted config files are no longer written",	cmd_switchcrypt },
	{ "/delfolder",		IC_ARG_REQUIRED, "<name>", CAT_STORE,  "delete a session folder",				cmd_delfolder },
	{ "/loadinitscript",	IC_ARG_OPTIONAL, "[file]", CAT_STORE,  "(re)load the init script",				cmd_loadinitscript },

	{ "/command",		IC_ARG_REQUIRED, "<text>", CAT_ALLWIN, "run a command or send text in ALL windows",		cmd_command },
	{ "/sizeall",		IC_ARG_NONE,	 NULL,	   CAT_ALLWIN, "resize all windows to this window's size",		cmd_sizeall },

	{ "/shortcuts",		IC_ARG_NONE,	 NULL,	   CAT_DIAG,   "reload the [Shortcuts] key bindings",			cmd_shortcuts },
	{ "/noshortcuts",	IC_ARG_NONE,	 NULL,	   CAT_DIAG,   "disable the keyboard-shortcut layer",			cmd_noshortcuts },
	{ "/nomouseshortcuts",	IC_ARG_NONE,	 NULL,	   CAT_DIAG,   "disable the mouse-shortcut layer",			cmd_nomouseshortcuts },
	{ "/bcdelay",		IC_ARG_OPTIONAL, "[ms]",   CAT_DIAG,   "between-character send delay",				cmd_bcdelay },
	{ "/PrintCharSize",	IC_ARG_REQUIRED, "<n>",	   CAT_DIAG,   "printing font size",					cmd_printcharsize },
	{ "/PrintMaxLinePerPage", IC_ARG_REQUIRED, "<n>",  CAT_DIAG,   "printing lines per page",				cmd_printmaxline },
	{ "/PrintMaxCharPerLine", IC_ARG_REQUIRED, "<n>",  CAT_DIAG,   "printing characters per line",				cmd_printmaxchar },
#ifdef MOD_ZMODEM
	{ "/zmodem",		IC_ARG_NONE,	 NULL,	   CAT_DIAG,   "toggle the ZModem file-transfer feature",		cmd_zmodem },
#endif
	{ "/fileassoc",		IC_ARG_NONE,	 NULL,	   CAT_DIAG,   "register the .ktx file association",			cmd_fileassoc },
#ifdef MOD_LAUNCHER
	{ "/initlauncher",	IC_ARG_NONE,	 NULL,	   CAT_DIAG,   "(re)create the launcher registry key",			cmd_initlauncher },
#endif
	{ "/debug",		IC_ARG_NONE,	 NULL,	   CAT_DIAG,   "toggle debug mode",					cmd_debug },
	{ "/passwd",		IC_ARG_NONE,	 NULL,	   CAT_DIAG,   "show + copy the session password (debug mode only)",	cmd_passwd },
	{ "/screenshot",	IC_ARG_NONE,	 NULL,	   CAT_DIAG,   "save a screenshot of the terminal",			cmd_screenshot },
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
