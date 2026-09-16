/*
 * kitty_startup.c - the once-per-process start of a KiTTY terminal: which
 * kitty.ini and kitty.sav are in use, the start counter, whether this run is
 * a do-and-exit command line, and InitWinMain, which brings the settings,
 * the icon library, the store and the startup helpers up in order.
 */
#include <dirent.h>
#include <io.h>
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

// KiTTY includes
#include "kitty.h"
#include "kitty_broadcast.h"   /* the other KiTTY windows: count, broadcast, resize */
#include "kitty_portfwd.h"     /* the port-forward display */
#include "kitty_regbackup.h"   /* the .sav export/import and the backup rotation */
#include "kitty_int.h"         /* what the split-off files share with this one */
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
#include "kitty_b64.h"
#include "kitty_store.h"
#include "kitty_startup.h"
#ifdef MOD_PROXY
#include "kitty_proxy.h"   /* InitProxyList, kitty_migrate_old_proxies (kitty.c includes it mid-file) */
#endif

// Work out the names of the kitty.ini and kitty.sav configuration files
// APPDATA = 	C:\Documents and Settings\<user>\Application Data on XP
//		C:\Users\<user>\AppData\Roaming on Vista
//
// In registry mode the configuration file is looked for
// - in the KITTY_INI_FILE environment variable
// - kitty.ini in the directory kitty.exe was started from, if it exists
// - else putty.ini in the directory kitty.exe was started from, if it exists
// - else kitty.ini in the %APPDATA%/KiTTY directory, if it exists
//
// In portable mode the configuration file is looked for
// - kitty.ini in the directory kitty.exe was started from, if it exists
// - else putty.ini in the directory kitty.exe was started from, if it exists
//
static void InitNameConfigFile( void ) {
	char buffer[4096] = "" ;   /* the KITTY_INI_FILE test below reads this even
	                            * when the variable is unset - it used to be
	                            * uninitialised stack, so the first existfile()
	                            * ran on whatever happened to be there */
	if( KittyIniFile != NULL ) { free( KittyIniFile ) ; }
	KittyIniFile=NULL ;

	/* snprintf, not strcpy: the value comes from the environment and is not
	 * length-bounded. */
	if( getenv("KITTY_INI_FILE") != NULL ) { snprintf( buffer, sizeof(buffer), "%s", getenv("KITTY_INI_FILE") ) ; }
	if( !existfile( buffer ) ) {
		snprintf( buffer, sizeof(buffer), "%s\\%s", InitialDirectory, DEFAULT_INIT_FILE ) ;
		if( !existfile( buffer ) ) {
			snprintf( buffer, sizeof(buffer), "%s\\putty.ini", InitialDirectory ) ;
			if( !existfile( buffer ) ) {
				if( IniFileFlag != SAVEMODE_DIR ) {
					snprintf( buffer, sizeof(buffer), "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_INIT_FILE ) ;
					if( !existfile( buffer ) ) {
						snprintf( buffer, sizeof(buffer), "%s\\%s", getenv("APPDATA"), INIT_SECTION ) ;
						CreateDirectory( buffer, NULL ) ;
						snprintf( buffer, sizeof(buffer), "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_INIT_FILE ) ;
					}
				} else {
					snprintf( buffer, sizeof(buffer), "%s\\%s", InitialDirectory, DEFAULT_INIT_FILE ) ;
				}
			}
		}
	}
	KittyIniFile=(char*)malloc( strlen( buffer)+2 ) ; strcpy( KittyIniFile, buffer) ;

	if( KittySavFile != NULL ) { free( KittySavFile ) ; } 
	KittySavFile=NULL ;
	snprintf( buffer, sizeof(buffer), "%s\\%s", InitialDirectory, DEFAULT_SAV_FILE ) ;
	if( !existfile( buffer ) ) {
		if( IniFileFlag != SAVEMODE_DIR ) {
			snprintf( buffer, sizeof(buffer), "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_SAV_FILE ) ;
			if( !existfile( buffer ) ) {
				snprintf( buffer, sizeof(buffer), "%s\\%s", getenv("APPDATA"), INIT_SECTION ) ;
				CreateDirectory( buffer, NULL ) ;
				snprintf( buffer, sizeof(buffer), "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_SAV_FILE ) ;
			}
		}
	}
	KittySavFile=(char*)malloc( strlen( buffer)+2 ) ; strcpy( KittySavFile, buffer) ;
	
	snprintf( buffer, sizeof(buffer), "%s\\kitty.dft", InitialDirectory ) ;
	if( existfile( KittyIniFile ) && existfile( buffer ) )  unlink( buffer ) ;
	if( !existfile( KittyIniFile ) )
		if( existfile( buffer ) ) rename( buffer, KittyIniFile ) ;
}
	
// Write the counter increment
static void WriteCountUpAndPath( void ) {
	// Save the folder list
	SaveFolderList() ;

	// Increment the usage counter
	CountUp() ;

	// Record the binary version
	WriteParameter( INIT_SECTION, KI_BUILD, BuildVersionTime ) ;

	/* find the file-copy helper (kscp) if it is there */
	SearchPSCP() ;

	// Look for WinSCP if it is there
	SearchWinSCP() ;
	}

// KiTTY-specific initialisation
void appendPath(const char *append) ;
#ifdef MOD_NETDEBUG
/* KiTTY netdebug: append a millisecond-timestamped startup checkpoint to the
 * same %USERPROFILE%\kitty_netdebug.log used by the event-log tee, so a slow
 * once-per-process startup can be pinpointed offline. Compiled only in the
 * MOD_NETDEBUG build; a no-op (absent) otherwise. */
void kitty_netdbg_ts( const char *msg ) {
	static FILE *f = NULL ;
	SYSTEMTIME s ; GetLocalTime( &s ) ;
	if( !f ) { char p[MAX_PATH] ; const char *h=getenv("USERPROFILE") ;
		snprintf( p, sizeof(p), "%s\\kitty_netdebug.log", h?h:"C:" ) ; f=fopen( p, "a" ) ; }
	if( f ) { fprintf( f, "%02d:%02d:%02d.%03d  [STARTUP] %s\n",
		s.wHour, s.wMinute, s.wSecond, s.wMilliseconds, msg ) ; fflush( f ) ; }
}
#define NETDBG_TS(m) kitty_netdbg_ts(m)
#else
#define NETDBG_TS(m) ((void)0)
#endif

#ifdef MOD_PERSO
/* Is this run one of the do-and-exit command-line paths (-importdir,
 * -exportall, -fileassoc, ...)?
 *
 * InitWinMain() runs BEFORE the command line is parsed, so anything it wants to
 * ask the user has to work this out from the raw command line itself. It only
 * needs to be right about one thing: those paths run unattended, from scripts
 * and test harnesses, so a modal box during startup does not inform anybody -
 * it hangs the caller until something kills it.
 * Deliberately conservative: an unrecognised switch counts as interactive, so
 * the worst case is asking a question rather than swallowing one. */
static int kitty_cli_do_and_exit( void ) {
	static const char * const batch[] = {
		"-importdir", "-exportall", "-portablecopy", "-takefolder", "-backupnow",
		"-mungestr", "-sendcmd", "-edit", "-ed", "-edb",
		"-fileassoc", "-sshhandler", "-cleanup", "-pgpfp",
		/* Deliberately NOT "-h"/"-?": this scan has no notion of quoting, so a
		 * one-letter token is far too easy to hit inside a session name or a
		 * path and would silence the box in a genuinely interactive run. */
		"-help", "--help",
		"-demo-config-box", "-demo-terminal", NULL } ;
	const char * cl = GetCommandLineA() ;
	int i ;
	if( cl == NULL ) return 0 ;
	for( i = 0 ; batch[i] != NULL ; i++ ) {
		size_t len = strlen( batch[i] ) ;
		const char * p = cl ;
		while( ( p = strstr( p, batch[i] ) ) != NULL ) {
			char before = ( p == cl ) ? ' ' : p[-1] ;
			char after  = p[len] ;
			/* whole word only: "-h" must not match inside "-hwndparent" */
			if( ( before == ' ' || before == '\t' || before == '"' )
			 && ( after == '\0' || after == ' ' || after == '\t' || after == '"' ) )
				return 1 ;
			p += len ;
			}
		}
	return 0 ;
	}
#endif

void InitWinMain( void ) {
	char buffer[4096];
	int i ;

	NETDBG_TS("InitWinMain: enter");

	/* KiTTY: install the client-side serving-agent check (security). */
	{ extern void kitty_install_agent_check(void); kitty_install_agent_check(); }
	srand(time(NULL));

	/* [KiTTY] framepace: auto (the default), a number of milliseconds, or 0
	 * for PuTTY's fixed cooldown - the pacing itself is windows/kitty_pace.c. */
	{
		char framepace[16] = "" ;
		void kitty_pace_set_setting( const char * ) ;
		ReadParameterN( KI_SECTION_KITTY, KI_FRAMEPACE, framepace, sizeof(framepace) ) ;
		kitty_pace_set_setting( framepace ) ;
	}
	
	if( existfile("kitty.log") ) { unlink( "kitty.log" ) ; }
	
	//if( !RegTestKey(HKEY_CLASSES_ROOT,"kitty.connect.1") ) { CreateFileAssoc() ; }

	// Build the binary version string
	sprintf( BuildVersionTime, "%s @ %s", BUILD_VERSION, BUILD_TIME ) ;
#ifdef MOD_PORTABLE
	sprintf( BuildVersionTime, "%s-portable @ %s", BUILD_VERSION, BUILD_TIME ) ;
#endif
#ifdef MOD_NOTRANSPARENCY
	sprintf( BuildVersionTime, "%s-nt @ %s", BUILD_VERSION, BUILD_TIME ) ;
#endif

	// Initialise the encryption library
	NETDBG_TS("before bcrypt_init");
	bcrypt_init( 0 ) ;
	NETDBG_TS("after bcrypt_init");

	// Get the startup directory and, for savemode=dir, the configuration one
	GetInitialDirectory( InitialDirectory ) ;
	NETDBG_TS("after GetInitialDirectory");

	// Work out the kitty.ini and kitty.sav configuration file names
	InitNameConfigFile() ;

	// Initialise the window class name
	strcpy( KiTTYClassName, appname ) ;

#ifdef MOD_PERSO
	if( ReadParameterN( INIT_SECTION, KI_KICLASSNAME, buffer, sizeof(buffer) ) )
		{ if( (strlen(buffer)>0) && (strlen(buffer)<128) ) { buffer[127]='\0'; strcpy( KiTTYClassName, buffer ) ; } }
	appname = KiTTYClassName ;
	/* Select the registry hive to match KiClassName: default KiTTY's own
	 * (Software\9bis.com\KiTTY); PuTTY's hive when KiClassName=PuTTY. */
	{ extern void kitty_set_registry_root(int use_putty);
	  kitty_set_registry_root( !stricmp(KiTTYClassName, "PuTTY") ) ; }
#endif

	// Initialise the menu table
	InitSpecialMenuTab() ;
	
	// Determine how sessions are saved
	GetSaveMode() ;
	NETDBG_TS("after GetSaveMode");

	/* Aux-window position memory (About boxes, etc.) is registry-backed. In portable
	 * modes (anything but SAVEMODE_REG) place windows correctly but do NOT persist, so
	 * we leave no registry footprint -- consistent with the rest of portable KiTTY. */
	{ void kitty_auxpos_set_persist( int on ) ;
	  if( IniFileFlag != SAVEMODE_REG ) kitty_auxpos_set_persist( 0 ) ; }

	// Initialise the parameters from the kitty.ini file
	LoadParameters() ;
	NETDBG_TS("after LoadParameters (kitty.ini read)");

	// Add the InitialDirectory and ConfigDirectory directories to PATH

	// Initialise the shortcuts
	InitShortcuts() ;
	NETDBG_TS("after InitShortcuts");

	/* Does our hive exist? ASK NOW, BEFORE the migrations below, because they
	 * create it as a side effect.
	 *
	 * RepairSharrowDefaults() and MigrateScpAutoPwd() finish by writing a
	 * one-time marker with RegTestOrCreateDWORD(), which CREATES the key when it
	 * is missing. The first-run block further down then asks "does the hive
	 * exist?" - and by then it always does, so neither of its branches could
	 * ever run: no adopting a PuTTY installation's sessions, and, worse, no
	 * restoring our own newest kittynew-*.sav after the hive is lost. A user who
	 * loses their profile got an empty KiTTY with their backup sitting unused. */
	int kitty_hive_existed ;

	/* The hive this run actually uses, and the name file mode parks it under.
	 * Taken at RUNTIME (kitty.ini KiClassName, applied by kitty_set_registry_root
	 * further up in this same function) rather than from the PUTTY_REG_POS
	 * macros: with KiClassName=PuTTY the sessions live in PuTTY's hive, so the
	 * macro version set aside a store nothing was reading and left the one in
	 * use in place - the same compile-time/runtime split that sent named proxies
	 * to the wrong hive. */
	char kitty_reg_live[512], kitty_reg_park[512], kitty_reg_park_sess[600], kitty_reg_live_sess[600] ;
	{ extern const char *kitty_registry_base( void ) ;
	  snprintf( kitty_reg_live, sizeof(kitty_reg_live), "%s", kitty_registry_base() ) ;
	  snprintf( kitty_reg_park, sizeof(kitty_reg_park), "%s_save", kitty_registry_base() ) ;
	  snprintf( kitty_reg_park_sess, sizeof(kitty_reg_park_sess), "%s\\Sessions", kitty_reg_park ) ; }
	snprintf( kitty_reg_live_sess, sizeof(kitty_reg_live_sess), "%s\\Sessions", kitty_reg_live ) ;
	kitty_hive_existed = RegTestKey( HKEY_CURRENT_USER, kitty_reg_live ) ;

	/* KiTTY 0.84: migrate the old 9bis.com\KiTTY hive to kapper.net\KiTTY BEFORE the
	 * PuTTY-import check below, so once our hive exists that import path stays out of the
	 * way. Idempotent + non-destructive (see kitty_registry.c). */
	if( (IniFileFlag == SAVEMODE_REG) || (IniFileFlag == SAVEMODE_FILE) ) {
		NETDBG_TS("before MigrateOldKittyHive");
		MigrateOldKittyHive() ;
		NETDBG_TS("after MigrateOldKittyHive");
		/* One-time repair of registry sessions that persisted the buggy
		 * SHARROW_APPLICATION default; must run after the hive exists. Registry-only:
		 * it reads/writes the kapper.net hive and its marker, so it must NOT run in
		 * portable (SAVEMODE_FILE) mode -- portable sessions are files, healed instead
		 * by the conf.h SHARROW_BITMAP default; letting portable flip the marker would
		 * also consume the one-shot before an installed KiTTY could run it. Idempotent
		 * (marker-guarded). */
		if( IniFileFlag == SAVEMODE_REG ) { RepairSharrowDefaults() ; MigrateScpAutoPwd() ; }
		/* One-time migration of legacy 9bis named proxies into our hive, with its
		 * own marker so it fires even when sessions were migrated in an earlier
		 * build. Registry-mode only (REG||FILE); DPAPI-protects passwords on copy
		 * and in place (hknet/KiTTY#11). */
		kitty_migrate_old_proxies() ;
	}
	/* Not gated on the save mode: the obsolete kitty.ini copy exists in
	 * portable installs too. */
	RetireConfigPasswordLeftovers() ;
	/* Same reasoning, same place: the retired CountUp bookkeeping can be in a
	 * kitty.ini as well as in the hive, and two of those values held the user's
	 * username and machine name. */
	RetireCountUpLeftovers() ;

	// Load the registry store if needed
	if( IniFileFlag == SAVEMODE_REG ) { // registry save mode
#ifdef MOD_PERSO
		/* The way back out of file mode.
		 *
		 * A file-mode start parks the registry store at PUTTY_REG_POS_SAVE and
		 * loads kitty.sav over PUTTY_REG_POS (see SAVEMODE_FILE below). Nothing
		 * ever put it back: dropping savemode=file left the user looking at the
		 * last .sav content and their real sessions apparently gone. Offering
		 * the restore HERE - on the next normal start - also covers the case an
		 * exit-time restore cannot, a file-mode run that crashed or was killed.
		 *
		 * What PUTTY_REG_POS holds right now is the file-mode working copy, and
		 * that copy still lives in kitty.sav, so replacing it loses nothing.
		 * Asked at most once: Yes consumes the parked key, No leaves a marker
		 * IN the parked key so the offer does not nag while the data stays put.
		 *
		 * Only offered when the parked key actually holds sessions. A modal at
		 * startup is expensive when nobody is in front of it (test runs, an
		 * "@session" shortcut), so it must not appear for an empty leftover -
		 * with nothing to restore the question has no answer worth asking. */
		if( RegTestKey( HKEY_CURRENT_USER, kitty_reg_park )
		 && RegCountKey( HKEY_CURRENT_USER, kitty_reg_park_sess ) > 0
		 && !kitty_cli_do_and_exit()
		 && WindowsCount( MainHwnd ) == 1 ) {
			DWORD declined = 0, dwsize = sizeof(DWORD), dwtype = 0 ;
			HKEY hsave = NULL ;
			if( RegOpenKeyEx( HKEY_CURRENT_USER, kitty_reg_park, 0, KEY_READ, &hsave ) == ERROR_SUCCESS ) {
				if( RegQueryValueEx( hsave, KR_RESTOREDECLINED, NULL, &dwtype, (LPBYTE)&declined, &dwsize ) != ERROR_SUCCESS )
					declined = 0 ;
				RegCloseKey( hsave ) ;
				}
			if( !declined ) {
				char question[1200] ;
				snprintf( question, sizeof(question),
					KT_MAIN_RESTORE_QUESTION,
					kitty_reg_park ) ;
				if( MessageBox( NULL, question,
					KT_CAP_RESTORE_REG_SESSIONS,
					MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 ) == IDYES ) {
					HWND hdlg = InfoBox( hinst, NULL ) ;
					InfoBoxSetText( hdlg, KT_MAIN_INFO_RESTORING ) ;
					/* Copies the parked tree over the working copy and removes
					 * the parked key, so the question cannot come back. */
					RegRenameTree( hdlg, HKEY_CURRENT_USER, kitty_reg_park, kitty_reg_live ) ;
					InfoBoxClose( hdlg ) ;
					}
				else {
					RegTestOrCreateDWORD( HKEY_CURRENT_USER, kitty_reg_park, KR_RESTOREDECLINED, 1 ) ;
					}
				}
			}
#endif
		/* If the key did not exist AT STARTUP ...
		 *
		 * kitty_hive_existed, not a fresh RegTestKey: by this point the one-time
		 * migrations above have created the key whether or not there was
		 * anything to migrate, so testing here answers a question about
		 * ourselves rather than about the machine.
		 * The second half of the condition is the old-hive case: when
		 * MigrateOldKittyHive() has just copied 9bis.com\KiTTY across, the
		 * sessions are already here and there is nothing to restore or adopt. */
		if( !kitty_hive_existed
		 && !RegTestKey( HKEY_CURRENT_USER, kitty_reg_live_sess ) ) {
			HWND hdlg = InfoBox( hinst, NULL ) ;
			// ... load the most recent backup (kittynew-<timestamp>.sav),
			// or the old fixed-name file if it is still there.
			char newestsav[4096] = "" ;
			int havesav = sav_find_for_restore( KittySavFile, newestsav, sizeof(newestsav) ) ;
			if( havesav ) {
				char *savedptr = KittySavFile ;
				KittySavFile = newestsav ;   /* LoadRegistryKey reads the global */
				InfoBoxSetText( hdlg, KT_MSG_INIT_REGISTRY ) ;
				InfoBoxSetText( hdlg, KT_MAIN_INFO_LOADING_FROM_FILE ) ;
				LoadRegistryKey( hdlg ) ;
				InfoBoxClose( hdlg ) ;
				KittySavFile = savedptr ;
			} else { // Otherwise look for PuTTY's key and take it over
				InfoBoxSetText( hdlg, KT_MSG_INIT_REGISTRY ) ;
				InfoBoxSetText( hdlg, KT_MAIN_INFO_FIRST_RUN_PUTTY ) ;
				/* Copy DIRECTLY rather than through TestRegKeyOrCopyFromPuTTY():
				 * that helper first tests whether our key exists and does
				 * nothing if it does - and the migration markers above have
				 * created it, so it would decline every time. We have already
				 * established that this is a first run (kitty_hive_existed);
				 * what is in our hive at this point is markers, nothing a copy
				 * could overwrite.
				 *
				 * And copy INTO the hive this run uses, not the compile-time one -
				 * with KiClassName=PuTTY that hive IS PuTTY's, where there is by
				 * definition nothing to adopt and copying a key onto itself is
				 * the one thing worth refusing outright. */
				if( stricmp( kitty_reg_live, "Software\\SimonTatham\\PuTTY" )
				 && RegTestKey( HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY" ) )
					kitty_RegCopyTree( HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY", kitty_reg_live ) ;
				InfoBoxClose( hdlg ) ;
			}
		}
	} else if( IniFileFlag == SAVEMODE_FILE ){ // file save mode
		if( !RegTestKey( HKEY_CURRENT_USER, kitty_reg_live ) ) { // the registry key does not exist
			HWND hdlg = InfoBox( hinst, NULL ) ;
			InfoBoxSetText( hdlg, KT_MSG_INIT_REGISTRY ) ;
			InfoBoxSetText( hdlg, KT_MAIN_INFO_LOADING_FROM_FILE ) ;
			LoadRegistryKey( hdlg ) ;
			InfoBoxClose( hdlg ) ;
			}
#ifdef MOD_PERSO
		else { // the registry key already exists
			if( WindowsCount( MainHwnd ) == 1 ) { // first kitty: back the registry key up before loading kitty.sav
				HWND hdlg = InfoBox( hinst, NULL ) ;
				InfoBoxSetText( hdlg, KT_MSG_INIT_REGISTRY ) ;
				/* File mode owns the registry view: kitty.sav is loaded INTO
				 * PUTTY_REG_POS, so whatever is there now has to move aside.
				 *
				 * Park it ONCE, and only once. The first park holds the user's
				 * real registry-mode store and must never be overwritten; every
				 * later start would otherwise park the PREVIOUS RUN'S .sav
				 * content on top of it, destroying the one copy worth keeping
				 * (and, with a dated name, littering the registry instead).
				 * If the parked key already exists, the current one is just the
				 * last run's .sav content and can be replaced. */
				if( !RegTestKey( HKEY_CURRENT_USER, kitty_reg_park ) ) {
					RegRenameTree( hdlg, HKEY_CURRENT_USER, kitty_reg_live, kitty_reg_park ) ;
					InfoBoxClose( hdlg ) ;
					/* SAY SO. Silence here is the actual harm: the sessions
					 * simply vanish from normal mode, with nothing pointing at
					 * where they went. Shown only on the first park, so it does
					 * not nag on every file-mode start - and never on a
					 * do-and-exit run (-importdir and friends), where a modal
					 * informs nobody and hangs the script that started it. */
					if( !kitty_cli_do_and_exit() ) {
						char notice[1200] ;
						snprintf( notice, sizeof(notice),
							KT_MAIN_SET_ASIDE_NOTICE,
							kitty_reg_park ) ;
						MessageBox( NULL, notice,
							KT_CAP_REG_SESSIONS_SET_ASIDE,
							MB_OK | MB_ICONINFORMATION ) ;
						}
					hdlg = InfoBox( hinst, NULL ) ;
					}
				InfoBoxSetText( hdlg, KT_MAIN_INFO_LOADING_SESSIONS ) ;
				LoadRegistryKey( hdlg ) ;
				InfoBoxClose( hdlg ) ;
				}
			}
#endif
		}
	else if( IniFileFlag == SAVEMODE_DIR ){ // directory save mode
		if( strlen(sesspath) == 0 ) { loadPath() ; }
		/* KiTTY 0.84: activate the portable file storage backend (windows/storage.c)
		 * now that sesspath is known. Sessions are then read/written as one file per
		 * session under sesspath, instead of the registry. Decoupled setters so
		 * libsettings stays registry-only in tools that never call them. */
		{
			char ppmode[32] = "" ;
			kitty_set_storage_mode( SAVEMODE_DIR ) ;
			kitty_set_session_dir( sesspath ) ;
			/* Portable at-rest password policy: master (default) or the
			 * explicit legacy/plaintext compatibility escape hatch. */
			if( readINI( KittyIniFile, INIT_SECTION, KI_PORTABLEPASSWORDPROTECTION, ppmode, sizeof(ppmode) ) )
				kitty_set_portable_password_protection( ppmode ) ;
		}
		/* Test Default Settings */
		/*
		char * defaultfile = (char*)malloc( strlen(sesspath)+20 ) ;
		sprintf( defaultfile, "%s\\Default Settings", sesspath ) ;
		if( !existfile(defaultfile) && GetDefaultSettingsFlag() ) {
			create_settings(KITTY_DEFAULT_SESSION) ;
		}
		free( defaultfile ) ;
		*/
	}

	/* Both of these ask the storage layer which backend is active, so they must
	 * run AFTER kitty_set_storage_mode() above - not next to the other one-time
	 * startup repairs further up, where a portable run still looks like a
	 * registry one and the registry branch fires by mistake.
	 *
	 * Portable stores used to read their master-password salt out of the
	 * registry on every unlock. That is gone, so a store that relied on it gets
	 * the state copied in once here - before anything tries to unlock. */
	if( kitty_migrate_portable_mpw_state() ) { kitty_show_mpw_moved( NULL ) ; }
	/* Same idea for the master password the OLD export behaviour created as a
	 * side effect: drop it when nothing in the store is wrapped with it
	 * Portable stores only - see the
	 * function's comment for why the registry hive is left alone. */
	kitty_retire_orphan_master_password() ;

	// Make mandatory registry keys
	snprintf( buffer, sizeof(buffer), "%s\\%s", kitty_registry_base(), "Commands" ) ;
	if( (IniFileFlag == SAVEMODE_REG)||( IniFileFlag == SAVEMODE_FILE) ) 
		RegTestOrCreate( HKEY_CURRENT_USER, buffer, NULL, NULL ) ;

#ifdef MOD_PROXY
	// Initiate proxies list
	InitProxyList() ;
#endif
#ifdef MOD_LAUNCHER
	// Initiate launcher
	snprintf( buffer, sizeof(buffer), "%s\\%s", kitty_registry_base(), "Launcher" ) ;
	if( (IniFileFlag == SAVEMODE_REG)||( IniFileFlag == SAVEMODE_FILE) )  
		if( !RegTestKey( HKEY_CURRENT_USER, buffer ) ) { InitLauncherRegistry() ; }
#endif
	NETDBG_TS("after registry/savemode block");
	// Initiate folders list
	InitFolderList() ;
	NETDBG_TS("after InitFolderList");

	// Increment and write the counters
	if( IniFileFlag == SAVEMODE_REG ) {
		WriteCountUpAndPath() ;
	}

	// Set up icon loading from the kitty.dll library if it exists
	if( !GetPuttyFlag() ) {
		if( IconFile != NULL )
		if( existfile( IconFile ) ) 
			{ HMODULE hDll ; if( ( hDll = LoadLibrary( TEXT(IconFile) ) ) != NULL ) hInstIcons = hDll ; }
		if( hInstIcons==NULL )
		if( existfile( "kitty.dll" ) )
			{ HMODULE hDll ; if( ( hDll = LoadLibrary( TEXT("kitty.dll") ) ) != NULL ) hInstIcons = hDll ; }
		// No external icon DLL: fall back to icons embedded in the executable itself
		if( hInstIcons==NULL ) hInstIcons = GetModuleHandle( NULL ) ;
		}

	NETDBG_TS("after icon-dll init");
	/* The application notification is owed. Only marked here: this runs
	 * before any window exists, and the note is shown in the notice window by
	 * the first window this process opens (kitty_notes.c). It used to be a
	 * modal box raised from right here, which stopped every start dead. */
	kitty_notes_mark_pending() ;

	// Generate an initialisation file (4096 KB max) for all the sessions
	snprintf( buffer, sizeof(buffer), "%s\\%s.ses.updt", InitialDirectory, appname ) ;
	if( existfile( buffer ) ) { InitAllSessions( HKEY_CURRENT_USER, kitty_registry_base(), "Sessions", buffer ) ; }
	/* Format: registry like => UTF-8 encoded !!!
	"ProxyUsername"="mylogin"
	"ProxyPassword"="mypassword"
	*/
	
	// Initialise the logs
	char hostname[4096], username[4096] ;
	NETDBG_TS("before GetUserName/GetComputerName");
	i = sizeof(username) ;
	GetUserName( username, (void*)&i ) ;
	i = 4095 ;
	GetComputerName( hostname, (void*)&i ) ;
	NETDBG_TS("after GetUserName/GetComputerName");
	snprintf( buffer, sizeof(buffer), "Starting %ld from %s@%s", GetCurrentProcessId(), username, hostname ) ;
	debug_logevent(buffer) ;

	/* KiTTY: remove any leftover "KiTTY++ download in progress" staging folder
	 * from a crashed Get File (only those whose .lock is free - a live download
	 * holds it). The folder chosen for a wildcard download is swept again then.
	 * LAST in InitWinMain, on purpose: it reads the global download folder
	 * through the settings store, so it must run after the registry root is
	 * set and after the first-run block above has decided whether to adopt
	 * PuTTY's sessions or restore a backup - placed before that capture it
	 * touched the store first and made a fresh machine look like a known one. */
	{ extern void kitty_xfer_sweep_downloads(void) ; kitty_xfer_sweep_downloads() ; }
	NETDBG_TS("InitWinMain: return");
}
