/*
 * kitty_onetime.c - kitty.exe's one-time steps from older versions and
 * other programs, in one file (kitty_onetime.h). Each step carries the
 * release that introduced it. kitty_startup.c runs them at five moments of
 * its start, in this order. The order matters (see each moment below).
 *
 *  moment                 step                            since
 *  KOT_BEFORE_INI         MigratePuttyIni                 0.85.1.16-beta
 *  KOT_AFTER_INI          PortableWriteSaveMode           0.85.1.16-beta
 *                         AutoTextFromRegistry            0.85.1.20-beta
 *                         LauncherKeysRename              0.85.1.20-beta
 *                         ConfNoBackupCountOnce           after 0.85.1.21-beta
 *                         ShortcutsRenameOldKeys          0.85.1.20-beta
 *  KOT_AFTER_HIVE_CHECK   MigrateOldKittyHive             1.14-beta
 *                         RepairSharrowDefaults           1.46-beta
 *                         MigrateScpAutoPwd               1.57-beta
 *                         kitty_migrate_old_proxies       1.48-beta
 *                         RetireConfigPasswordLeftovers   1.63-beta
 *                         RetireCountUpLeftovers          1.68-beta
 *  KOT_STORE_LOAD         kitty_store_ini_takeover        0.85.1.13-beta
 *                         (in kitty_store.c: it works on the store's
 *                         private state)
 *  KOT_END                kitty_migrate_portable_mpw_state 1.65-beta
 *                         (in kitty_secretstore.c: it works on the
 *                         master password's private state)
 *
 * The launcher runs MigrateOldKittyHive at its own start as well.
 */
#include <dirent.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/locking.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include "putty.h"
#include "terminal.h"
#include "putty-rc.h"
#include <windows.h>
#include "kitty_oldwin.h"   /* record what an older Windows does not have */
#include <psapi.h>
#include <iphlpapi.h>
#include "kitty.h"
#include "kitty_params.h"
#include "kitty_broadcast.h"   /* the other KiTTY windows: count, broadcast, resize */
#include "kitty_portfwd.h"     /* the port-forward display */
#include "kitty_regbackup.h"   /* the .sav export/import and the backup rotation */
#include "kitty_int.h"         /* what the split-off files share with this one */
#include "kitty_defs.h"     /* KITTY_DEFAULT_SESSION */
#include "kitty_common.h"
#include "kitty_image.h"
#include "kitty_crypt.h"
#include "kitty_registry.h"
#include "kitty_tools.h"
#include "kitty_inifile.h"   /* the suite's kitty.ini resolver: the search, savemode */
#include "kitty_win.h"
#include "kitty_updater.h"
#include "kitty_winutil.h"
#include "kitty_dlgbox.h"
#include "kitty_launcher.h"
#include "winfont_fallback.h"
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_globalsettings.h"   /* the hive through the settings module */
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
#include "kitty_winerr.h"   /* kitty_win_reason: Windows' reason, for the reports */
#include "kitty_proxy.h"   /* InitProxyList, kitty_migrate_old_proxies (kitty.c includes it mid-file) */
#include <winsock2.h>   /* before kitty_registry.h's windows.h: putty.h pulls it in further down */
#include "kitty_atomicfile.h"   /* kitty_write_file_whole: whole or not at all */
#include "kitty_buildlabel.h"   /* KITTY_TEST_BUILD_LABEL: KITTY_NO_SETTINGS_LAUNCH */
#include <objbase.h>        /* CoInitializeEx: the ms-settings: launch */
#include <stdarg.h>
#include "kitty_winkeys.h"   /* Windows' own keys */
#include "dialog.h"
#include "storage.h"
#include "kitty_store_acl.h"   /* the Proxies folder: owner-only rights */
#include <stddef.h>
#include <string.h>
#include "kitty_zmodem.h"   /* sc_menu_offered: what the ZModem entries need */
#include "kitty_onetime.h"

/* One-time step since 0.85.1.16-beta. */
/* The putty.ini beside the exe was kitty.exe's settings file when no kitty.ini
 * was there - read from its [KiTTY] section only, in full (every section is
 * kitty.exe's own). Copied to kitty.ini once, as it is. The copy is reported
 * at the prompt that started kitty.exe, else in a box - never a box in a
 * scripted do-and-exit run. A putty.ini with no [KiTTY] keys (classic KiTTY's own,
 * [PuTTY]) did nothing here and is left alone: copying it would create a
 * kitty.ini that hides %APPDATA%\KiTTY\kitty.ini. putty.ini stays as it is. */
static void MigratePuttyIni( const char * kittyini ) {
	char *puttyini, keys[64], err[256], *msg ;
	int ok ;
	/* both file paths in full */
	puttyini = dupprintf( "%s\\putty.ini", InitialDirectory ) ;
	if( !existfile( puttyini ) || existfile( kittyini ) ||
	    GetPrivateProfileSectionA( KI_SECTION_KITTY, keys, sizeof(keys), puttyini ) == 0 ) {
		sfree( puttyini ) ;
		return ;
	}
	ok = CopyFileA( puttyini, kittyini, TRUE ) ;
	if( ok ) msg = dupprintf( KT_MAIN_PUTTYINI_COPIED_FMT, kittyini ) ;
	else {
		DWORD e = GetLastError() ;
		err[0] = '\0' ;
		FormatMessageA( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, err, sizeof(err), NULL ) ;
		str_rtrim( err, "\r\n .\t" ) ;
		msg = dupprintf( KT_MAIN_PUTTYINI_COPY_FAILED_FMT, kittyini, err[0] ? err : "unknown error" ) ;
	}
	StartupNotice( msg, !ok ) ;
	sfree( msg ) ;
	sfree( puttyini ) ;
}

/* One-time step since 0.85.1.16-beta. */
/* The portable build is the folder store without a savemode key. The
 * programs beside it (kageant, klink, kscp, ksftp) take their store from
 * kitty.ini (kitty_inifile.c). The first start writes the key down for them:
 * a kitty.ini that is not there yet is created from the template, which then
 * carries savemode=dir. One without the key gets it. A savemode already
 * there is the user's and stays. Nothing is written with conf=no or
 * readonly=yes. A failed write raises no message - on a read-only stick it
 * would come at every start. The resolver's layout guess covers that copy. */
static void PortableWriteSaveMode( void ) {
	char buffer[64] = "" ;
	if( GetReadOnlyFlag() ) return ;   /* the folder store: conf=no does not hold it back */
	if( (KittyIniFile == NULL) || !KittyIniFile[0] ) return ;
	if( IniFileFlag != SAVEMODE_DIR ) return ;
	if( !existfile( KittyIniFile ) ) { CreateIniFile( KittyIniFile ) ; return ; }
	if( readINI( KittyIniFile, INIT_SECTION, KI_SAVEMODE, buffer, sizeof(buffer) ) ) {
		str_rtrim( buffer, "\n\r \t" ) ;
		if( buffer[0] ) return ;
	}
	writeINI( KittyIniFile, INIT_SECTION, KI_SAVEMODE, "dir" ) ;
}

/* One AutoText key name of an old hive list: a number, or a key
 * DefineShortcuts knows. Nothing kitty.ini cannot hold as a key. */
static int AutoTextNameOk( const char * name ) {
	const char * p ;
	char * probe ;
	int ok ;
	if( !name[0] || name[0] == ';' || name[0] == '#' || name[0] == '[' ) return 0 ;
	for( p = name ; *p ; p++ )
		if( (unsigned char)*p < 0x20 || *p == '=' || *p == ']' ) return 0 ;
	if( name[0] >= '0' && name[0] <= '9' ) {
		for( p = name ; *p ; p++ ) if( *p < '0' || *p > '9' ) return 0 ;
		return 1 ;
	}
	probe = dupstr( name ) ;
	ok = DefineShortcuts( probe ) > 0 ;
	sfree( probe ) ;
	return ok ;
	}

/* One-time step since 0.85.1.20-beta. */
/*
 * The AutoText (the [Shortcuts] list and the texts of its keys) an older
 * build read with ReadParameterDup, so a registry copy kept it in the hive.
 * [Shortcuts] is kitty.ini only now (InitShortcuts), so that AutoText is
 * copied into kitty.ini once, before InitShortcuts first reads it.
 *
 * Only when the registry is the store in charge (not savemode=dir), kitty.ini
 * has no list in [Shortcuts] yet and the hive has a list. Copied: the list
 * and the texts of the key names it holds. A list with a word that is no key
 * name is left as it is. The list is written last, so a copy cut short is
 * done again at the next start. The hive values are deleted only after every
 * kitty.ini write succeeded: that deletion is the done-marker, as for the
 * retired values of kitty_regbackup.c.
 *
 * Under readonly=yes nothing is copied: the hive's AutoText stays unseen
 * until readonly is lifted. With conf=no and no kitty.ini, none is created.
 */
static void AutoTextFromRegistry( void ) {
	const char * base = kitty_registry_base() ;
	char * have, * list, * words, * p, * q ;
	char ** names = NULL ;
	size_t n = 0, size = 0, i ;
	int ok = 1 ;
	if( IniFileFlag == SAVEMODE_DIR || GetReadOnlyFlag() ) return ;
	if( KittyIniFile == NULL || !KittyIniFile[0] ) return ;
	if( kitty_inifile_noconf_blocks() ) return ;   /* conf=no: nothing into kitty.ini */
	if( ( have = kitty_read_ini_dup( KittyIniFile, KI_SECTION_SHORTCUTS, KI_SC_LIST ) ) != NULL ) {
		sfree( have ) ;
		return ;
	}
	/* store-pairs: read param Shortcuts KI_SC_LIST - the one-time copy of an older build's AutoText out of the hive */
	if( ( list = GetValueDataDup( HKEY_CURRENT_USER, base, KI_SC_LIST ) ) == NULL ) return ;
	words = dupstr( list ) ;
	for( p = words ; *p ; p = q ) {
		while( *p == ' ' ) p++ ;
		if( !*p ) break ;
		q = p ;
		while( *q && *q != ' ' ) q++ ;
		if( *q ) *q++ = '\0' ;
		if( !AutoTextNameOk( p ) ) { ok = 0 ; break ; }
		sgrowarray( names, size, n ) ;
		names[n++] = p ;
	}
	if( ok && n ) {
		for( i = 0 ; i < n && ok ; i++ ) {
			char * text = GetValueDataDup( HKEY_CURRENT_USER, base, names[i] ) ;
			if( text == NULL ) continue ;          /* a key name without a text */
			/* store-pairs: write file Shortcuts * - the one-time copy: the AutoText texts into kitty.ini */
			if( !writeINI( KittyIniFile, KI_SECTION_SHORTCUTS, names[i], text ) ) ok = 0 ;
			smemclr( text, strlen( text ) ) ;      /* a text can hold a password */
			sfree( text ) ;
		}
		if( ok && !writeINI( KittyIniFile, KI_SECTION_SHORTCUTS, KI_SC_LIST, list ) ) ok = 0 ;
		if( ok ) {
			for( i = 0 ; i < n ; i++ ) RegDelValue( HKEY_CURRENT_USER, base, names[i] ) ;
			RegDelValue( HKEY_CURRENT_USER, base, KI_SC_LIST ) ;
		}
	}
	sfree( names ) ;
	sfree( words ) ;
	sfree( list ) ;
	}

/* One-time step since 0.85.1.20-beta. */
/*
 * [Launcher] dblclick and noticeseconds are traydblclick and
 * proxynoticeseconds now. The registry is flat (WriteParameter drops the
 * section): in registry mode they were one hive value with [ConfigBox]
 * dblclick and [KiTTY] noticeseconds, written so by 0.85.1.14-beta to
 * 0.85.1.19-beta. Renamed once at startup, nothing under readonly=yes.
 *
 * kitty.ini [Launcher]: the old key's value goes to the new key unless the
 * new key is there, then the old key is removed.
 * The hive (registry mode): a dblclick holding a launcher value (config,
 * manage, quickconnect) moves to traydblclick and is deleted. Open and start
 * are the configuration box's and stay. A noticeseconds value cannot be told
 * apart: it is copied to proxynoticeseconds when that is absent, so both
 * notices keep the time in effect.
 */
static void LauncherKeysRename( void ) {
	static const char * const old_keys[] = { "dblclick", "noticeseconds" } ;
	static const char * const new_keys[] = { KI_LAUNCHER_DBLCLICK, KI_LAUNCHER_NOTICESECONDS } ;
	size_t i ;
	if( GetReadOnlyFlag() ) return ;
	if( KittyIniFile != NULL && KittyIniFile[0] && existfile( KittyIniFile ) ) {
		for( i = 0 ; i < lenof( old_keys ) ; i++ ) {
			/* store-pairs: read file Launcher dblclick,noticeseconds - the one-time rename: the old [Launcher] names */
			char * old = kitty_read_ini_dup( KittyIniFile, KI_SECTION_LAUNCHER, old_keys[i] ) ;
			char * cur ;
			if( old == NULL ) continue ;
			/* store-pairs: read param Launcher KI_LAUNCHER_DBLCLICK,KI_LAUNCHER_NOTICESECONDS - the one-time rename: is the new name there */
			cur = kitty_read_ini_dup( KittyIniFile, KI_SECTION_LAUNCHER, new_keys[i] ) ;
			/* store-pairs: write param Launcher KI_LAUNCHER_DBLCLICK,KI_LAUNCHER_NOTICESECONDS - the one-time rename: the kitty.ini value under its new name */
			if( cur != NULL || writeINI( KittyIniFile, KI_SECTION_LAUNCHER, new_keys[i], old ) )
				/* store-pairs: write file Launcher dblclick,noticeseconds - the one-time rename: the old name removed */
				delINI( KittyIniFile, KI_SECTION_LAUNCHER, old_keys[i] ) ;
			sfree( cur ) ;
			sfree( old ) ;
		}
	}
	if( IniFileFlag != SAVEMODE_DIR ) {
		const char * base = kitty_registry_base() ;
		char * v = GetValueDataDup( HKEY_CURRENT_USER, base, "dblclick" ) ;
		char * have ;
		if( v != NULL ) {
			if( !stricmp( v, "config" ) || !stricmp( v, "manage" ) || !stricmp( v, "quickconnect" ) ) {
				have = GetValueDataDup( HKEY_CURRENT_USER, base, KI_LAUNCHER_DBLCLICK ) ;
				if( have != NULL || RegTestOrCreate( HKEY_CURRENT_USER, base, KI_LAUNCHER_DBLCLICK, v ) )
					RegDelValue( HKEY_CURRENT_USER, base, "dblclick" ) ;
				sfree( have ) ;
			}
			sfree( v ) ;
		}
		if( ( v = GetValueDataDup( HKEY_CURRENT_USER, base, "noticeseconds" ) ) != NULL ) {
			if( ( have = GetValueDataDup( HKEY_CURRENT_USER, base, KI_LAUNCHER_NOTICESECONDS ) ) == NULL )
				RegTestOrCreate( HKEY_CURRENT_USER, base, KI_LAUNCHER_NOTICESECONDS, v ) ;
			sfree( have ) ;
			sfree( v ) ;
		}
	}
}

/* One-time step since the release after 0.85.1.21-beta. */
/* conf=no used to switch the .sav backups off too. Now the count alone does
 * (savbackupcount=0): an install with conf=no and no count gets the count
 * once, so it keeps writing no backups. */
static void ConfNoBackupCountOnce( void ) {
	char kb[16] = "" ;
	if( !kitty_inifile_noconf() || GetReadOnlyFlag() ) return ;
	if( ReadParameterN( INIT_SECTION, KI_SAVBACKUPCOUNT, kb, sizeof(kb) ) && kb[0] ) return ;
	WriteParameter( INIT_SECTION, KI_SAVBACKUPCOUNT, "0" ) ;
}

/* One-time step since 0.85.1.20-beta. */
/* An action renamed in kitty.ini (the oldkey column): the old key's value
 * goes to the new key unless the new key is there, then the old key is
 * removed. A key that is there counts, not a value: an empty old key (the
 * action switched off) becomes an empty new key. Once at startup, before
 * InitShortcuts first reads the keys (kitty_startup.c). Nothing under
 * readonly=yes. [Shortcuts] is kitty.ini only (see InitShortcuts). */
static void ShortcutsRenameOldKeys( void ) {
	int i ;
	if( GetReadOnlyFlag() ) return ;
	if( KittyIniFile == NULL || !KittyIniFile[0] || !existfile( KittyIniFile ) ) return ;
	for( i=0 ; i<ShortcutActionCount() ; i++ ) {
		char * old, * cur ;
		if( ShortcutActionOldKey( i ) == NULL ) continue ;
		/* store-pairs: read file Shortcuts script - the one-time rename: the old name of typetextfile */
		if( ( old = kitty_read_ini_dup( KittyIniFile, KI_SECTION_SHORTCUTS, ShortcutActionOldKey( i ) ) ) == NULL ) continue ;
		/* store-pairs: read file Shortcuts KI_SC_TYPETEXTFILE - the one-time rename: is the new name there */
		cur = kitty_read_ini_dup( KittyIniFile, KI_SECTION_SHORTCUTS, ShortcutActionKey( i ) ) ;
		/* store-pairs: write file Shortcuts KI_SC_TYPETEXTFILE - the one-time rename: the value under its new name */
		if( cur != NULL || writeINI( KittyIniFile, KI_SECTION_SHORTCUTS, ShortcutActionKey( i ), old ) == 1 )
			/* store-pairs: write file Shortcuts script - the one-time rename: the old name removed */
			delINI( KittyIniFile, KI_SECTION_SHORTCUTS, ShortcutActionOldKey( i ) ) ;
		sfree( cur ) ;
		sfree( old ) ;
	}
}

/* One-time step since 1.14-beta. */
/* KiTTY 0.84: one-time migration of the registry hive from the old KiTTY namespace
 * (Software\9bis.com\KiTTY) to the current one (Software\kapper.net\KiTTY). Idempotent
 * -- copies only when the destination is absent and the source exists. Non-destructive
 * (the old hive is left intact and also serves as a read-only fallback in storage.c).
 * Call it from every kitty.exe entry path (terminal init AND -launcher) so the launcher
 * sees migrated sessions even on a boot where it runs before the main terminal.
 *
 * Hive paths are spelled out as string literals on purpose (not PUTTY_REG_POS): this is
 * a one-off migration between two FIXED endpoints -- the legacy 9bis hive and the current
 * kapper.net hive -- so we specifically do NOT want the runtime-flippable base. The
 * 9bis side has no global macro anyway. */
void MigrateOldKittyHive( void ) {
	if( !RegTestKey( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY" )
	    && RegTestKey( HKEY_CURRENT_USER, "Software\\9bis.com\\KiTTY" ) ) {
		/* a partial copy must not stay: the key's presence is what marks the
		 * migration done, so it would never be tried again */
		if( !kitty_RegCopyTree( HKEY_CURRENT_USER, "Software\\9bis.com\\KiTTY", "Software\\kapper.net\\KiTTY" ) )
			RegDelTree( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY" ) ;
	}
}

static int kitty_repairs_own_hive( void ) {
	return strcmp( kitty_registry_base(), "Software\\kapper.net\\KiTTY" ) == 0 ;
}

/* One-time step since 1.46-beta. */
/* KiTTY 0.84: one-time repair of the ShiftedArrowKeys default regression.
 * The 0.84 port inherited PuTTY's SHARROW_APPLICATION(0) compiled default instead
 * of KiTTY's historical SHARROW_BITMAP(1). A session migrated from the old 9bis
 * hive that relied on that old default, once loaded and *re-saved* under the buggy
 * build, had ShiftedArrowKeys=0 persisted into the kapper.net hive -- silently
 * losing Ctrl+Left/Right word navigation. conf.h now restores BITMAP as the
 * default, which auto-heals every session that has NO explicit value. This repair
 * mops up the ones that persisted an explicit 0.
 *
 * We reset ONLY where we can prove the 0 was written by us and not chosen by the
 * user: the corresponding session must still exist in the untouched old 9bis hive
 * AND have no explicit ShiftedArrowKeys there. Deleting the kapper.net value then
 * lets the session fall back to the new BITMAP default. Any deliberate choice
 * (an explicit value already in 9bis, or a session absent from 9bis so we cannot
 * know) is left untouched. Idempotent via a marker value -- runs at most once.
 *
 * Registry paths are string literals, not PUTTY_REG_POS -- same rationale as
 * MigrateOldKittyHive above: fixed 9bis<->kapper.net migration endpoints (not the
 * runtime-flippable base). */
static void RepairSharrowDefaults( void ) {
	HKEY hSess ;
	char *cur, *old ;
	char name[MAX_KEY_LENGTH+1], *kpath, *oldsess ;
	DWORD idx, len ;
	int failed = 0 ;

	/* Only when kapper.net\\KiTTY is the hive this run uses (KiClassName can
	 * point the store at another hive): the repair reads and marks THAT hive
	 * by literal path. It must not create it as a side effect under any other. */
	if( !kitty_repairs_own_hive() ) return ;

	/* one-time guard: skip if we've already run */
	if( ( cur = GetValueDataDup( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", KR_SHARROWREPAIRDONE ) ) != NULL ) {
		sfree( cur ) ;
		return ;
	}

	if( RegOpenKeyEx( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY\\Sessions", 0, KEY_READ, &hSess ) == ERROR_SUCCESS ) {
		for( idx = 0 ; ; idx++ ) {
			int zero ;
			len = sizeof(name) ;
			if( RegEnumKeyEx( hSess, idx, name, &len, NULL, NULL, NULL, NULL ) != ERROR_SUCCESS ) break ;
			/* only sessions with an explicit APPLICATION(0) are candidates */
			kpath = dupprintf( "Software\\kapper.net\\KiTTY\\Sessions\\%s", name ) ;
			cur = GetValueDataDup( HKEY_CURRENT_USER, kpath, "ShiftedArrowKeys" ) ;
			zero = cur != NULL && strcmp( cur, "0" ) == 0 ;
			sfree( cur ) ;
			if( zero ) {
				oldsess = dupprintf( "Software\\9bis.com\\KiTTY\\Sessions\\%s", name ) ;
				/* repair only if the 9bis original exists and had NO explicit value:
				 * that proves the 0 is our persisted default, not the user's choice. */
				if( RegTestKey( HKEY_CURRENT_USER, oldsess ) ) {
					if( ( old = GetValueDataDup( HKEY_CURRENT_USER, oldsess, "ShiftedArrowKeys" ) ) == NULL ) {
						if( !RegDelValue( HKEY_CURRENT_USER, kpath, "ShiftedArrowKeys" ) ) failed = 1 ;
					} else sfree( old ) ;
				}
				sfree( oldsess ) ;
			}
			sfree( kpath ) ;
		}
		RegCloseKey( hSess ) ;
	}

	/* set the marker, so we don't rescan every boot (even if the old 9bis hive
	 * is gone and nothing could be repaired) - but not after a repair that
	 * failed: the next start tries it again. RegTestOrCreateDWORD hides the
	 * RegSetValueEx byte-buffer boilerplate and creates the base key if needed. */
	if( !failed ) RegTestOrCreateDWORD( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", KR_SHARROWREPAIRDONE, 1 ) ;
}

/* One-time step since 1.57-beta. */
/* One-time migration: the per-session "Send file in current
 * directory" option (SCPAutoPwd) was retired -- it drove uploads off the removed
 * __pw title-scan (CVE-2024-23749) and is superseded by opt-in OSC 7 cwd
 * tracking. For every registry session that had SCPAutoPwd=1, enable
 * OSC7CwdTracking (the safe way to get the "upload into the current remote dir"
 * behaviour the user had chosen). Then delete the retired SCPAutoPwd value
 * from every session so it does not linger. Idempotent via the ScpAutoPwdMigrated
 * marker -- runs at most once. Registry mode only (like RepairSharrowDefaults);
 * portable sessions simply stop persisting the key once it is no longer saved. */
static void MigrateScpAutoPwd( void ) {
	HKEY hSess ;
	int failed = 0 ;
	char *cur ;
	char name[MAX_KEY_LENGTH+1], *kpath ;
	DWORD idx, len ;

	/* Only when kapper.net\\KiTTY is the hive this run uses (KiClassName can
	 * point the store at another hive): the repair reads and marks THAT hive
	 * by literal path. It must not create it as a side effect under any other. */
	if( !kitty_repairs_own_hive() ) return ;

	/* one-time guard: skip if we've already run */
	if( ( cur = GetValueDataDup( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", KR_SCPAUTOPWDMIGRATED ) ) != NULL ) {
		sfree( cur ) ;
		return ;
	}

	if( RegOpenKeyEx( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY\\Sessions", 0, KEY_READ, &hSess ) == ERROR_SUCCESS ) {
		for( idx = 0 ; ; idx++ ) {
			len = sizeof(name) ;
			if( RegEnumKeyEx( hSess, idx, name, &len, NULL, NULL, NULL, NULL ) != ERROR_SUCCESS ) break ;
			kpath = dupprintf( "Software\\kapper.net\\KiTTY\\Sessions\\%s", name ) ;
			if( ( cur = GetValueDataDup( HKEY_CURRENT_USER, kpath, "SCPAutoPwd" ) ) == NULL ) { sfree( kpath ) ; continue ; }
			/* the old option was on -> turn on its safe replacement */
			if( strcmp( cur, "1" ) == 0
			    && !RegTestOrCreateDWORD( HKEY_CURRENT_USER, kpath, "OSC7CwdTracking", 1 ) ) {
				/* the replacement is not stored: keep the old value, the
				 * next start tries again - deleting it lost the setting */
				failed = 1 ;
				sfree( cur ) ; sfree( kpath ) ;
				continue ;
			}
			/* drop the retired key regardless of its value */
			if( !RegDelValue( HKEY_CURRENT_USER, kpath, "SCPAutoPwd" ) ) failed = 1 ;
			sfree( cur ) ; sfree( kpath ) ;
		}
		RegCloseKey( hSess ) ;
	}

	/* set the marker, so we don't rescan every boot - unless a session was
	 * left unmigrated, which the next start tries again */
	if( !failed )
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", KR_SCPAUTOPWDMIGRATED, 1 ) ;
}

/* One-time, marker-gated migration of named proxies from the legacy 9bis hive
 * (Software\9bis.com\KiTTY\Proxies) into ours (PUTTY_REG_POS\Proxies). Runs at
 * startup in registry mode only (portable reads its Proxies\ folder in place);
 * INDEPENDENT of the session-hive migration so it also fires for users who
 * migrated sessions in an earlier build. Registry-only, so at-rest protection
 * is DPAPI (transparent, account-bound, losslessly reversible - no prompt):
 *   - a proxy absent on our side is copied over in full, then its ProxyPassword
 *     is DPAPI-encrypted;
 *   - a proxy already present keeps its (possibly user-edited) fields, but a
 *     still-plaintext ProxyPassword is DPAPI-encrypted in place - this also
 *     auto-protects proxies the full-tree copy of MigrateOldKittyHive made earlier.
 * A marked (already-encrypted) password is never re-wrapped. The one-shot marker
 * means a proxy the user later deletes never resurrects (hknet/KiTTY#11). */
static int kitty_proxies_migrated( void ) {
	DWORD val = 0, len = 0, type = 0 ; int got = 0 ;
	/* through the settings module. A value over 4 bytes did not fit the
	 * read and counts as not set */
	unsigned char *v = kitty_settings_reg_stored_dup_at( kitty_registry_base(), "ProxiesMigrated", &type, &len ) ;
	if( v != NULL && len <= sizeof(val) ) {
		memcpy( &val, v, len ) ;
		if( type == REG_DWORD && val ) got = 1 ;
	}
	free( v ) ;
	return got ;
}

/* One-time step since 1.48-beta. */
static void kitty_migrate_old_proxies( void ) {
	if( (IniFileFlag != SAVEMODE_REG) && (IniFileFlag != SAVEMODE_FILE) ) return ;
	if( kitty_store_readonly() ) return ;   /* readonly=yes: a later start migrates */
	if( kitty_proxies_migrated() ) return ;
	HKEY hSrc ;
	if( RegOpenKeyEx( HKEY_CURRENT_USER, "Software\\9bis.com\\KiTTY\\Proxies", 0, KEY_READ, &hSrc ) == ERROR_SUCCESS ) {
		DWORD idx ; FILETIME ft ;
		for( idx = 0 ; ; idx++ ) {
			/* a key name is at most 255 characters, plus its NUL */
			char sub[MAX_KEY_LENGTH + 1] ; DWORD subsz = sizeof(sub) ;
			if( RegEnumKeyEx( hSrc, idx, sub, &subsz, NULL, NULL, NULL, &ft ) != ERROR_SUCCESS ) break ;
			if( !strcmp(sub,"None") || !strcmp(sub,"Default") ) continue ;
			char *srcpath = dupprintf( "Software\\9bis.com\\KiTTY\\Proxies\\%s", sub ) ;
			char *dstpath = dupprintf( "%s\\Proxies\\%s", kitty_registry_base(), sub ) ;
			if( !RegTestKey( HKEY_CURRENT_USER, dstpath ) )
				kitty_RegCopyTree( HKEY_CURRENT_USER, srcpath, dstpath ) ;  /* all fields (pw plaintext) */
			kitty_proxy_encrypt_in_place( dstpath, sub ) ;                  /* protect if still plaintext */
			sfree( srcpath ) ;
			sfree( dstpath ) ;
		}
		RegCloseKey( hSrc ) ;
	}
	RegTestOrCreateDWORD( HKEY_CURRENT_USER, kitty_registry_base(), "ProxiesMigrated", 1 ) ;
}

/* Is the value there, in the registry hive in use? The value is read in full
 * and cleared again: it may be a secret. */
static int retire_reg_value_present( const char * name ) {
	char * v = GetValueDataDup( HKEY_CURRENT_USER, kitty_registry_base(), name ) ;
	if( v == NULL ) return 0 ;
	smemclr( v, strlen( v ) ) ; sfree( v ) ;
	return 1 ;
	}

/* ... in kitty.ini (any value, empty included, as readINI answered) */
static int retire_ini_value_present( const char * name ) {
	char * v = kitty_read_ini_dup( KittyIniFile, INIT_SECTION, name ) ;
	if( v == NULL ) return 0 ;
	smemclr( v, strlen( v ) ) ; sfree( v ) ;
	return 1 ;
	}

/* One-time step since 1.63-beta. */
static void RetireConfigPasswordLeftovers( void ) {
	/* the registry only for a copy that keeps its settings there: a portable
	 * copy (folder store) leaves HKCU alone - an installed copy cleans its
	 * own hive at its next start */
	if( IniFileFlag != SAVEMODE_DIR && retire_reg_value_present( KI_PASSWORD ) )
		/* store-pairs: write param KiTTY KI_PASSWORD - the retired configuration password, deleted from the hive */
		RegDelValue( HKEY_CURRENT_USER, kitty_registry_base(), KI_PASSWORD ) ;
	if( ( KittyIniFile != NULL ) && !GetReadOnlyFlag() && retire_ini_value_present( KI_PASSWORD ) )
		delINI( KittyIniFile, INIT_SECTION, KI_PASSWORD ) ;
	}

/* One-time step since 1.68-beta. */
/*
 * CountUp() used to write five bookkeeping values on every run - KiLastUp,
 * KiLastUH, KiSess, KiVers, KiPath - that nothing ever read back. The writes are
 * gone. This removes them from stores that already have them, because stopping
 * the writes on its own would just freeze stale values in place for ever.
 *
 * Two of them mattered more than the rest. KiLastUH held your Windows username
 * and computer name. KiVers held your OS version. Both were scrambled with the
 * compiled-in public constant - which is obfuscation, not encryption. It
 * made the leak invisible to the one person who would care: kitty.ini gets
 * shared. Nobody reading their own file could see those two lines were
 * their username and machine.
 *
 * Self-limiting, like RetireConfigPasswordLeftovers above: every delete is
 * skipped when the value is absent, so this costs nothing on a clean store and
 * runs at most once usefully.
 */
static void RetireCountUpLeftovers( void ) {
	/* char* rather than const char*: RegDelValue takes LPTSTR. */
	static char *const dead[] = {
		KR_KILASTUP, KR_KILASTUH, KR_KISESS, KR_KISESS_COMMENTED, KR_KIVERS, KR_KIPATH } ;
	size_t i ;
	for( i = 0 ; i < lenof(dead) ; i++ ) {
		if( IniFileFlag != SAVEMODE_DIR   /* a portable copy leaves HKCU alone (above) */
		    /* store-pairs: read param KiTTY KR_KILASTUP,KR_KILASTUH,KR_KISESS,KR_KISESS_COMMENTED,KR_KIVERS,KR_KIPATH - the retired bookkeeping values (dead[]) */
		    && retire_reg_value_present( dead[i] ) )
			/* store-pairs: write param KiTTY KR_KILASTUP,KR_KILASTUH,KR_KISESS,KR_KISESS_COMMENTED,KR_KIVERS,KR_KIPATH - the retired bookkeeping values (dead[]) */
			RegDelValue( HKEY_CURRENT_USER, kitty_registry_base(), dead[i] ) ;
		/* store-pairs: read file KiTTY KR_KILASTUP,KR_KILASTUH,KR_KISESS,KR_KISESS_COMMENTED,KR_KIVERS,KR_KIPATH - the retired bookkeeping values (dead[]) */
		if( ( KittyIniFile != NULL ) && !GetReadOnlyFlag() && retire_ini_value_present( dead[i] ) )
			/* store-pairs: write file KiTTY KR_KILASTUP,KR_KILASTUH,KR_KISESS,KR_KISESS_COMMENTED,KR_KIVERS,KR_KIPATH - the retired bookkeeping values (dead[]) */
			delINI( KittyIniFile, INIT_SECTION, dead[i] ) ;
	}
	}

int kitty_onetime_run( enum kitty_onetime_moment at ) {
	switch( at ) {
	case KOT_BEFORE_INI:
		/* The putty.ini copy is for the kitty.ini beside the exe - not when
		 * %KITTY_INI_FILE% points to a file that is there. Before kitty.ini
		 * is read: the copied keys count from this start on. */
		if( !GetReadOnlyFlag() && ( getenv("KITTY_INI_FILE") == NULL || !existfile( getenv("KITTY_INI_FILE") ) ) ) {
			char * ini = dupprintf( "%s\\%s", InitialDirectory, DEFAULT_INIT_FILE ) ;
			MigratePuttyIni( ini ) ;
			sfree( ini ) ;
		}
		return 0 ;
	case KOT_AFTER_INI:
		/* kitty.ini is read, the shortcuts are not set up yet */
#ifdef MOD_PORTABLE
		PortableWriteSaveMode() ;
#endif
		AutoTextFromRegistry() ;     /* AutoText an older build kept in the hive: into kitty.ini */
		LauncherKeysRename() ;       /* [Launcher] dblclick, noticeseconds: their new names */
		ConfNoBackupCountOnce() ;
		ShortcutsRenameOldKeys() ;   /* [Shortcuts] script: its new name typetextfile */
		return 0 ;
	case KOT_AFTER_HIVE_CHECK:
		/* After kitty_startup.c noted whether the hive existed: these steps
		 * create it (their markers). A lost profile's newest
		 * kittynew-*.sav is restored only when the hive was missing.
		 * readonly=yes: they write the store, a later writable start runs
		 * them. */
		if( !GetReadOnlyFlag() && ( (IniFileFlag == SAVEMODE_REG) || (IniFileFlag == SAVEMODE_FILE) ) ) {
			/* the old 9bis.com\KiTTY hive to kapper.net\KiTTY first: once
			 * our hive exists, the PuTTY import stays out of the way */
			MigrateOldKittyHive() ;
			/* registry only: portable sessions are files, healed by the
			 * conf.h default. A portable run must not use up the marker
			 * before an installed KiTTY runs it. */
			if( IniFileFlag == SAVEMODE_REG ) { RepairSharrowDefaults() ; MigrateScpAutoPwd() ; }
			/* its own marker: it runs even when the sessions moved in an
			 * earlier build. Passwords DPAPI-protected (hknet/KiTTY#11). */
			kitty_migrate_old_proxies() ;
		}
		/* not by save mode: the retired values can be in a kitty.ini of a
		 * portable copy too */
		if( !GetReadOnlyFlag() ) RetireConfigPasswordLeftovers() ;
		if( !GetReadOnlyFlag() ) RetireCountUpLeftovers() ;
		return 0 ;
	case KOT_STORE_LOAD:
		/* the folder store, before it starts: it may move the session
		 * folder. 1 = moved just now (kitty_startup.c reports it) */
		return kitty_store_ini_takeover() ;
	case KOT_END:
		/* before anything unlocks: 1 = copied just now */
		return kitty_migrate_portable_mpw_state() ;
	}
	return 0 ;
}
