/*
 * KiTTY keyboard shortcuts, moved verbatim out of kitty.c to shrink that
 * monolith: DefineShortcuts (parses a "control+shift+F5"-style kitty.ini
 * value into a key code), TranslateShortcuts, InitShortcuts (loads the
 * [Shortcuts] / [KiTTY] user-defined-shortcut config), and ManageShortcuts
 * (the WM_KEYDOWN dispatcher for all fork key bindings, called from
 * window.c). The shortcut types are declared in kitty.h.
 * Compiled into the same targets as kitty.c (kitty + kitty_portable).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "putty.h"
#include "terminal.h"

#include <windows.h>

#include "kitty.h"
#include "kitty_commun.h"
#include "kitty_crypt.h"
#include "kitty_tools.h"
#include "kitty_text.h"     /* KT_KEYTEXT_*: the key names shown in menus */
#include "kitty_inikeys.h"  /* KI_SC_*: the [Shortcuts] key names */

/* Provided elsewhere in the KiTTY tree (not in kitty.h). */
extern HWND MainHwnd ;                  /* kitty.c: the terminal window */
void SendKeyboardPlus( HWND hwnd, const char * st ) ;   /* kitty.c */
int SendCommandAllWindows( HWND hwnd, char * cmd ) ;    /* kitty.c */
void set_title( TermWin *tw, const char *title ) ;      /* kitty.c */
void ManageProtect( HWND hwnd, TermWin *tw, char * title ) ;  /* kitty.c */
void SendFile( HWND hwnd ) ;                            /* kitty_xfer.c */
void GetFile( HWND hwnd ) ;                             /* kitty_xfer.c */
void RunCmd( HWND hwnd ) ;                              /* kitty_xfer.c */
void StartWinSCP( HWND hwnd, char * directory, char * host, char * user ) ;  /* kitty_xfer.c */
void RunSessionWithCurrentSettings( HWND hwnd, Conf *conf, const char * host, const char * user, const char * pass, const int port, const char * remotepath ) ;  /* kitty.c */
void RunPuttyEd( HWND hwnd, char * filename ) ;         /* kitty_win.c */
void debug_logevent( const char *fmt, ... ) ;           /* kitty_win.c */
int readINI( const char * filename, const char * section, const char * key, char * pStr, size_t size ) ;  /* kitty_ini.h defines data, not includable twice per TU-group */

/* Shims so the moved bodies below stay textually identical to their kitty.c
 * originals: these five stayed behind as kitty.c statics with accessors. */
#define KittyIniFile (GetKittyIniFile())
#define TransparencyFlag (GetTransparencyFlag())
#define ProtectFlag (GetProtectFlag())
#define WinHeight (GetWinHeight())
#define ImageViewerFlag (GetImageViewerFlag())

/* The tables themselves (types in kitty.h). */
struct TShortcuts shortcuts_tab ;
int NbShortCuts = 0 ;
struct TShortcuts2 shortcuts_tab2[512] ;

// Gestion des raccourcis
#define SHIFTKEY 500
#define CONTROLKEY 1000
#define ALTKEY 2000
#define ALTGRKEY 4000
#define WINKEY 8000

// F1=0x70 (112) => F12=0x7B (123)
	

// Shortcuts managment
int DefineShortcuts( char * buf ) {
	char * pst = buf ;
	if( strlen(buf)==0 ) return 0 ;
	int key = 0 ;
	// Special keys
	while( (strstr(pst,"{SHIFT}")==pst) || (strstr(pst,"{CONTROL}")==pst) || (strstr(pst,"{ALT}")==pst) || (strstr(pst,"{ALTGR}")==pst) || (strstr(pst,"{WIN}")==pst) ) {
		while( strstr(pst,"{ALT}")==pst ) { key += ALTKEY ; pst += 5 ; }
		while( strstr(pst,"{ALTGR}")==pst ) { key += ALTGRKEY ; pst += 7 ; }
		while( strstr(pst,"{WIN}")==pst ) { key += WINKEY ; pst += 5 ; }
		while( strstr(pst,"{SHIFT}")==pst ) { key += SHIFTKEY ; pst += 7 ; }
		while( strstr(pst,"{CONTROL}")==pst ) { key += CONTROLKEY ; pst += 9 ; }
	}
	
	if( strstr( pst, "{F12}" )==pst ) { key = key + VK_F12 ; pst += 5 ; }
	else if( strstr( pst, "{F11}" )==pst ) { key = key + VK_F11 ; pst += 5 ; }
	else if( strstr( pst, "{F10}" )==pst ) { key = key + VK_F10 ; pst += 5 ; }
	else if( strstr( pst, "{F9}" )==pst ) { key = key + VK_F9 ; pst += 4 ; }
	else if( strstr( pst, "{F8}" )==pst ) { key = key + VK_F8 ; pst += 4 ; }
	else if( strstr( pst, "{F7}" )==pst ) { key = key + VK_F7 ; pst += 4 ; }
	else if( strstr( pst, "{F6}" )==pst ) { key = key + VK_F6 ; pst += 4 ; }
	else if( strstr( pst, "{F5}" )==pst ) { key = key + VK_F5 ; pst += 4 ; }
	else if( strstr( pst, "{F4}" )==pst ) { key = key + VK_F4 ; pst += 4 ; }
	else if( strstr( pst, "{F3}" )==pst ) { key = key + VK_F3 ; pst += 4 ; }
	else if( strstr( pst, "{F2}" )==pst ) { key = key + VK_F2 ; pst += 4 ; }
	else if( strstr( pst, "{F1}" )==pst ) { key = key + VK_F1 ; pst += 4 ; }
	else if( strstr( pst, "{RETURN}" )==pst ) { key = key + VK_RETURN ; pst += 8 ; }
	else if( strstr( pst, "{ESCAPE}" )==pst ) { key = key + VK_ESCAPE ; pst += 8 ; }
	else if( strstr( pst, "{SPACE}" )==pst ) { key = key + VK_SPACE ; pst += 7 ; }
	else if( strstr( pst, "{PRINT}" )==pst ) { key = key + VK_SNAPSHOT ; pst += 7 ; }
	else if( strstr( pst, "{PAUSE}" )==pst ) { key = key + VK_PAUSE ; pst += 7 ; }
	else if( strstr( pst, "{PRIOR}" )==pst ) { key = key + VK_PRIOR ; pst += 7 ; }
	else if( strstr( pst, "{RIGHT}" )==pst ) { key = key + VK_RIGHT ; pst += 7 ; }
	else if( strstr( pst, "{LEFT}" )==pst ) { key = key + VK_LEFT ; pst += 6 ; }
	else if( strstr( pst, "{NEXT}" )==pst ) { key = key + VK_NEXT ; pst += 6 ; }
	else if( strstr( pst, "{BACK}" )==pst ) { key = key + VK_BACK ; pst += 6 ; }
	else if( strstr( pst, "{HOME}" )==pst ) { key = key + VK_HOME ; pst += 6 ; }
	else if( strstr( pst, "{DOWN}" )==pst ) { key = key + VK_DOWN ; pst += 6 ; }
	else if( strstr( pst, "{ATTN}" )==pst ) { key = key + VK_ATTN ; pst += 6 ; }
	else if( strstr( pst, "{END}" )==pst ) { key = key + VK_END ; pst += 5 ; }
	else if( strstr( pst, "{TAB}" )==pst ) { key = key + VK_TAB ; pst += 5 ; }
	else if( strstr( pst, "{INS}" )==pst ) { key = key + VK_INSERT ; pst += 5 ; }
	else if( strstr( pst, "{DEL}" )==pst ) { key = key + VK_DELETE ; pst += 5 ; }
	else if( strstr( pst, "{UP}" )==pst ) { key = key + VK_UP ; pst += 4 ; }
	else if( strstr( pst, "{NUMPAD0}" )==pst ) { key = key + VK_NUMPAD0 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD1}" )==pst ) { key = key + VK_NUMPAD1 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD2}" )==pst ) { key = key + VK_NUMPAD2 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD3}" )==pst ) { key = key + VK_NUMPAD3 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD4}" )==pst ) { key = key + VK_NUMPAD4 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD5}" )==pst ) { key = key + VK_NUMPAD5 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD6}" )==pst ) { key = key + VK_NUMPAD6 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD7}" )==pst ) { key = key + VK_NUMPAD7 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD8}" )==pst ) { key = key + VK_NUMPAD8 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD9}" )==pst ) { key = key + VK_NUMPAD9 ; pst += 9 ; }
	else if( strstr( pst, "{DECIMAL}" )==pst ) { key = key + VK_DECIMAL ; pst += 9 ; }
	else if( strstr( pst, "{BREAK}" )==pst ) { key = key + VK_CANCEL ; pst += 7 ; }
	else if( strstr( pst, "{NUMLOCK}" )==pst ) { key = key + VK_NUMLOCK ; pst += 9 ; }
	else if( strstr( pst, "{SCROLL}" )==pst ) { key = key + VK_SCROLL ; pst += 8 ; }
	else if( strstr( pst, "{ADD}" )==pst ) { key = key + VK_ADD ; pst += 5 ; }
	else if( strstr( pst, "{MULTIPLY}" )==pst ) { key = key + VK_MULTIPLY ; pst += 10 ; }
	else if( strstr( pst, "{SEPARATOR}" )==pst ) { key = key + VK_SEPARATOR ; pst += 11 ; }
	else if( strstr( pst, "{SUBTRACT}" )==pst ) { key = key + VK_SUBTRACT ; pst += 10 ; }
	else if( strstr( pst, "{DECIMAL}" )==pst ) { key = key + VK_DECIMAL ; pst += 9 ; }
	else if( strstr( pst, "{DIVIDE}" )==pst ) { key = key + VK_DIVIDE ; pst += 8 ; }
	else if( strstr( pst, "{ATTN}" )==pst ) { key = key + VK_ATTN ; pst += 6 ; }
	
	else if( strstr( pst, "{OEM_PLUS}" )==pst ) { key = key + VK_OEM_PLUS  ; pst += 10 ; } // +
	else if( strstr( pst, "{OEM_COMMA}" )==pst ) { key = key + VK_OEM_COMMA  ; pst += 11 ; } // ,
	else if( strstr( pst, "{OEM_MINUS}" )==pst ) { key = key + VK_OEM_MINUS  ; pst += 11 ; } // -
	else if( strstr( pst, "{OEM_PERIOD}" )==pst ) { key = key + VK_OEM_PERIOD  ; pst += 12 ; } // .
	
/* 
Example to change automatically , in .
[Shortcuts]
list={OEM_COMMA}
{OEM_COMMA}=.\
*/
	
	else if( pst[0] == '{' ) { key = 0 ; }
	else { key = key + toupper(pst[0]) ; }
	
	if( key==0 ) { key = -1 ; }
	return key ;
}
	
void TranslateShortcuts( char * st ) {
	int i,j,k,r ;
	char *buffer;
	if( st==NULL ) return ;
	if( strlen(st)==0 ) return ;
	buffer = (char*) malloc( strlen(st)+1 ) ;
	for( i=0 ; i<strlen(st) ; i++ ) {
		if( st[i]=='{' ) {
			if( strstr(st+i,"{{")==(st+i) ) {
				del(st,i+1,1);
			} else if( strstr(st+i,"{END}")==(st+i) ) {
				del(st,i+1,1);st[i]='\\';st[i+1]='k';st[i+2]='2';st[i+3]='3';
				i=i+3;
			} else if( strstr(st+i,"{HOME}")==(st+i) ) {
				del(st,i+1,2);st[i]='\\';st[i+1]='k';st[i+2]='2';st[i+3]='4';
				i=i+3;
			} else if( strstr(st+i,"{ESCAPE}")==(st+i) ) {
				del(st,i+1,4);st[i]='\\';st[i+1]='k';st[i+2]='1';st[i+3]='b';
				i=i+3;
			} else if( (j=poss( "}", st+i )) > 3 ) {
				for( k=i ; k<(i+j) ; k++ ) {
					buffer[k-i]=st[k] ;
					buffer[k-i+1]='\0' ;
				}
				r=DefineShortcuts( buffer ) ;
				del(st,i+1,j-1);
				st[i]=r ;
			}
		}
	}
	free(buffer);
}

/* One row per [Shortcuts] action: its ini key, where its code lives in
 * shortcuts_tab, the built-in default (0 = no key) and the name the
 * Shortcuts panel and the help show it under. InitShortcuts reads the
 * table; the panel lists it, sorted by name, so the order here is free. */
struct sc_action { const char * key ; size_t offset ; int dflt ; const char * name ; } ;
#define SC_ACT( field, key, dflt, name ) { key, offsetof(struct TShortcuts, field), dflt, name }
static const struct sc_action sc_actions[] = {
	SC_ACT( editor,            KI_SC_EDITOR,            SHIFTKEY+VK_F2,            KT_SC_ACT_EDITOR ),
	SC_ACT( editorclipboard,   KI_SC_EDITORCLIPBOARD,   CONTROLKEY+SHIFTKEY+VK_F2, KT_SC_ACT_EDITORCLIPBOARD ),
	SC_ACT( winscp,            KI_SC_WINSCP,            SHIFTKEY+VK_F3,            KT_SC_ACT_WINSCP ),
	SC_ACT( filezilla,         KI_SC_FILEZILLA,         SHIFTKEY+VK_F4,            KT_SC_ACT_FILEZILLA ),
	SC_ACT( switchlogmode,     KI_SC_SWITCHLOGMODE,     SHIFTKEY+VK_F5,            KT_SC_ACT_SWITCHLOGMODE ),
	SC_ACT( showportforward,   KI_SC_SHOWPORTFORWARD,   SHIFTKEY+VK_F6,            KT_SC_ACT_SHOWPORTFORWARD ),
	SC_ACT( print,             KI_SC_PRINT,             SHIFTKEY+VK_F7,            KT_SC_ACT_PRINT ),
	SC_ACT( printall,          KI_SC_PRINTALL,          VK_F7,                     KT_SC_ACT_PRINTALL ),
	SC_ACT( inputm,            KI_SC_INPUTM,            SHIFTKEY+VK_F8,            KT_SC_ACT_INPUTM ),
#ifdef MOD_BACKGROUNDIMAGE
	SC_ACT( viewer,            KI_SC_VIEWER,            SHIFTKEY+VK_F11,           KT_SC_ACT_VIEWER ),
#endif
	SC_ACT( autocommand,       KI_SC_AUTOCOMMAND,       SHIFTKEY+VK_F12,           KT_SC_ACT_AUTOCOMMAND ),
	SC_ACT( script,            KI_SC_SCRIPT,            CONTROLKEY+VK_F2,          KT_SC_ACT_SCRIPT ),
	SC_ACT( sendfile,          KI_SC_SENDFILE,          CONTROLKEY+VK_F3,          KT_SC_ACT_SENDFILE ),
	SC_ACT( getfile,           KI_SC_GETFILE,           CONTROLKEY+VK_F4,          KT_SC_ACT_GETFILE ),
	SC_ACT( command,           KI_SC_COMMAND,           CONTROLKEY+VK_F5,          KT_SC_ACT_COMMAND ),
	SC_ACT( tray,              KI_SC_TRAY,              CONTROLKEY+VK_F6,          KT_SC_ACT_TRAY ),
	SC_ACT( visible,           KI_SC_VISIBLE,           CONTROLKEY+VK_F7,          KT_SC_ACT_VISIBLE ),
	SC_ACT( input,             KI_SC_INPUT,             CONTROLKEY+VK_F8,          KT_SC_ACT_INPUT ),
	SC_ACT( protect,           KI_SC_PROTECT,           CONTROLKEY+VK_F9,          KT_SC_ACT_PROTECT ),
#ifdef MOD_BACKGROUNDIMAGE
	SC_ACT( imagechange,       KI_SC_IMAGECHANGE,       CONTROLKEY+VK_F11,         KT_SC_ACT_IMAGECHANGE ),
#endif
	SC_ACT( rollup,            KI_SC_ROLLUP,            CONTROLKEY+VK_F12,         KT_SC_ACT_ROLLUP ),
	SC_ACT( resetterminal,     KI_SC_RESETTERMINAL,     0,                         KT_SC_ACT_RESETTERMINAL ),
	SC_ACT( duplicate,         KI_SC_DUPLICATE,         CONTROLKEY+ALTKEY+'T',     KT_SC_ACT_DUPLICATE ),
	SC_ACT( opennew,           KI_SC_OPENNEW,           0,                         KT_SC_ACT_OPENNEW ),
	SC_ACT( opennewcurrent,    KI_SC_OPENNEWCURRENT,    0,                         KT_SC_ACT_OPENNEWCURRENT ),
	SC_ACT( changesettings,    KI_SC_CHANGESETTINGS,    0,                         KT_SC_ACT_CHANGESETTINGS ),
	SC_ACT( clearscrollback,   KI_SC_CLEARSCROLLBACK,   0,                         KT_SC_ACT_CLEARSCROLLBACK ),
	SC_ACT( clearlogfile,      KI_SC_CLEARLOGFILE,      0,                         KT_SC_ACT_CLEARLOGFILE ),
	SC_ACT( openlogfile,       KI_SC_OPENLOGFILE,       0,                         KT_SC_ACT_OPENLOGFILE ),
	SC_ACT( closerestart,      KI_SC_CLOSERESTART,      0,                         KT_SC_ACT_CLOSERESTART ),
	SC_ACT( eventlog,          KI_SC_EVENTLOG,          0,                         KT_SC_ACT_EVENTLOG ),
	SC_ACT( fullscreen,        KI_SC_FULLSCREEN,        0,                         KT_SC_ACT_FULLSCREEN ),
	SC_ACT( fontup,            KI_SC_FONTUP,            CONTROLKEY+VK_ADD,         KT_SC_ACT_FONTUP ),
	SC_ACT( fontdown,          KI_SC_FONTDOWN,          CONTROLKEY+VK_SUBTRACT,    KT_SC_ACT_FONTDOWN ),
	SC_ACT( fontreset,         KI_SC_FONTRESET,         CONTROLKEY+VK_NUMPAD0,     KT_SC_ACT_FONTRESET ),
	SC_ACT( copyall,           KI_SC_COPYALL,           0,                         KT_SC_ACT_COPYALL ),
	SC_ACT( fontnegative,      KI_SC_FONTNEGATIVE,      0,                         KT_SC_ACT_FONTNEGATIVE ),
	SC_ACT( fontblackandwhite, KI_SC_FONTBLACKANDWHITE, 0,                         KT_SC_ACT_FONTBLACKANDWHITE ),
	SC_ACT( keyexchange,       KI_SC_KEYEXCHANGE,       0,                         KT_SC_ACT_KEYEXCHANGE ),
	SC_ACT( transparencyup,    KI_SC_TRANSPARENCYUP,    CONTROLKEY+VK_UP,          KT_SC_ACT_TRANSPARENCYUP ),
	SC_ACT( transparencydown,  KI_SC_TRANSPARENCYDOWN,  CONTROLKEY+VK_DOWN,        KT_SC_ACT_TRANSPARENCYDOWN ),
} ;
#undef SC_ACT
#define SC_SLOT( i ) ( (int *) ( (char *) &shortcuts_tab + sc_actions[i].offset ) )

int ShortcutActionCount( void ) { return (int) ( sizeof(sc_actions) / sizeof(sc_actions[0]) ) ; }
const char * ShortcutActionKey( int i ) { return ( i >= 0 && i < ShortcutActionCount() ) ? sc_actions[i].key : "" ; }
const char * ShortcutActionName( int i ) { return ( i >= 0 && i < ShortcutActionCount() ) ? sc_actions[i].name : "" ; }
int ShortcutActionValue( int i ) { return ( i >= 0 && i < ShortcutActionCount() ) ? *SC_SLOT(i) : 0 ; }
int ShortcutActionDefault( int i ) { return ( i >= 0 && i < ShortcutActionCount() ) ? sc_actions[i].dflt : 0 ; }

// Init shortcuts map at startup
void InitShortcuts( void ) {
	char buffer[4096], list[4096], *pl ;
	int i, t=0 ;
	/* An action's key: the kitty.ini value when there is one and it parses
	 * (an empty value parses to 0 = no key), the default otherwise. */
	for( i=0 ; i<ShortcutActionCount() ; i++ ) {
		int * slot = SC_SLOT(i) ;
		if( !readINI(KittyIniFile,KI_SECTION_SHORTCUTS,sc_actions[i].key,buffer, sizeof(buffer)) || ( (*slot=DefineShortcuts(buffer))<0 ) )
			*slot = sc_actions[i].dflt ;
	}

	if( NbShortCuts>0 ) for( i=0 ; i<NbShortCuts ; i++ ) { if( shortcuts_tab2[i].st!=NULL ) { free(shortcuts_tab2[i].st) ; } }
	NbShortCuts=0 ;
	if( ReadParameterN( KI_SECTION_SHORTCUTS, KI_SC_LIST, list, sizeof(list) ) ) {
		pl=list ;
		while( strlen(pl) > 0 ) {
			i=0;
			while( (i<strlen(pl))&&(pl[i]!=' ') ) { i++ ; }
			if( pl[i]==' ' ) { pl[i]='\0' ; t=1 ; }
			if( strlen(pl)>0 )
			if( ReadParameterN( KI_SECTION_SHORTCUTS, pl, buffer, sizeof(buffer) ) ) {
				if( (pl[0]<'0')||(pl[0]>'9') ) {
					shortcuts_tab2[NbShortCuts].num = DefineShortcuts( pl );
				} else {
					shortcuts_tab2[NbShortCuts].num = atoi(pl) ;
				}
				TranslateShortcuts( buffer ) ;
				if( debug_flag ) { debug_logevent( "Remap key %s to %s", pl, buffer ) ; }

				shortcuts_tab2[NbShortCuts].st=(char*)malloc( strlen(buffer)+1 ) ;
				strcpy( shortcuts_tab2[NbShortCuts].st, buffer ) ;
				NbShortCuts++;
			}
			if( t==1 ) { pl[i]=' ' ; t = 0 ; pl=pl+i+1 ; }
			else pl=pl+i ;

			while( pl[0]==' ' ) pl++ ;
		}
	}
}

/* Menu text for a shortcut code: the inverse of DefineShortcuts, for the keys
 * it names. Modifiers first, joined with "+", in the order Ctrl, Alt, Shift,
 * Win, AltGr: "Ctrl+F3", "Shift+F4", "Ctrl+Alt+T". Returns the length
 * written; 0 with an empty buffer for an unset key (0) and for a key no name
 * is known for. The modifier values are far enough apart that peeling them
 * off from the largest down is unambiguous; they are written afterwards, in
 * the reading order above. */
int ShortcutKeyText( int key, char * buf, size_t size ) {
	char name[32] ;
	int vk, win = 0, altgr = 0, alt = 0, ctrl = 0, shift = 0 ;
	size_t n = 0 ;
	if( ( buf == NULL ) || ( size == 0 ) ) return 0 ;
	buf[0] = '\0' ;
	if( key <= 0 ) return 0 ;
	name[0] = '\0' ;
	if( key >= WINKEY )     { key -= WINKEY ; win = 1 ; }
	if( key >= ALTGRKEY )   { key -= ALTGRKEY ; altgr = 1 ; }
	if( key >= ALTKEY )     { key -= ALTKEY ; alt = 1 ; }
	if( key >= CONTROLKEY ) { key -= CONTROLKEY ; ctrl = 1 ; }
	if( key >= SHIFTKEY )   { key -= SHIFTKEY ; shift = 1 ; }
#define KEYTEXT_MOD( on, txt ) \
	if( on ) { n += (size_t) snprintf( buf + n, ( n < size ) ? size - n : 0, "%s+", txt ) ; }
	KEYTEXT_MOD( ctrl, KT_KEYTEXT_CTRL )
	KEYTEXT_MOD( alt, KT_KEYTEXT_ALT )
	KEYTEXT_MOD( shift, KT_KEYTEXT_SHIFT )
	KEYTEXT_MOD( win, KT_KEYTEXT_WIN )
	KEYTEXT_MOD( altgr, KT_KEYTEXT_ALTGR )
#undef KEYTEXT_MOD
	vk = key ;
	if( ( vk >= VK_F1 ) && ( vk <= VK_F12 ) ) {
		snprintf( name, sizeof(name), KT_KEYTEXT_FKEY, vk - VK_F1 + 1 ) ;
	} else if( ( vk >= VK_NUMPAD0 ) && ( vk <= VK_NUMPAD9 ) ) {
		snprintf( name, sizeof(name), KT_KEYTEXT_NUMPAD, vk - VK_NUMPAD0 ) ;
	} else if( ( ( vk >= 'A' ) && ( vk <= 'Z' ) ) || ( ( vk >= '0' ) && ( vk <= '9' ) ) ) {
		name[0] = (char) vk ; name[1] = '\0' ;
	} else {
		const char * s = NULL ;
		switch( vk ) {
			case VK_RETURN:   s = KT_KEYTEXT_ENTER ; break ;
			case VK_ESCAPE:   s = KT_KEYTEXT_ESC ; break ;
			case VK_SPACE:    s = KT_KEYTEXT_SPACE ; break ;
			case VK_SNAPSHOT: s = KT_KEYTEXT_PRINTSCREEN ; break ;
			case VK_PAUSE:    s = KT_KEYTEXT_PAUSE ; break ;
			case VK_CANCEL:   s = KT_KEYTEXT_BREAK ; break ;
			case VK_PRIOR:    s = KT_KEYTEXT_PAGEUP ; break ;
			case VK_NEXT:     s = KT_KEYTEXT_PAGEDOWN ; break ;
			case VK_LEFT:     s = KT_KEYTEXT_LEFT ; break ;
			case VK_RIGHT:    s = KT_KEYTEXT_RIGHT ; break ;
			case VK_UP:       s = KT_KEYTEXT_UP ; break ;
			case VK_DOWN:     s = KT_KEYTEXT_DOWN ; break ;
			case VK_HOME:     s = KT_KEYTEXT_HOME ; break ;
			case VK_END:      s = KT_KEYTEXT_END ; break ;
			case VK_BACK:     s = KT_KEYTEXT_BACKSPACE ; break ;
			case VK_TAB:      s = KT_KEYTEXT_TAB ; break ;
			case VK_INSERT:   s = KT_KEYTEXT_INSERT ; break ;
			case VK_DELETE:   s = KT_KEYTEXT_DELETE ; break ;
			case VK_ATTN:     s = KT_KEYTEXT_ATTN ; break ;
			case VK_NUMLOCK:  s = KT_KEYTEXT_NUMLOCK ; break ;
			case VK_SCROLL:   s = KT_KEYTEXT_SCROLLLOCK ; break ;
			case VK_ADD:      s = KT_KEYTEXT_NUM_ADD ; break ;
			case VK_SUBTRACT: s = KT_KEYTEXT_NUM_SUBTRACT ; break ;
			case VK_MULTIPLY: s = KT_KEYTEXT_NUM_MULTIPLY ; break ;
			case VK_DIVIDE:   s = KT_KEYTEXT_NUM_DIVIDE ; break ;
			case VK_DECIMAL:  s = KT_KEYTEXT_NUM_DECIMAL ; break ;
			case VK_OEM_PLUS:   s = KT_KEYTEXT_OEM_PLUS ; break ;
			case VK_OEM_COMMA:  s = KT_KEYTEXT_OEM_COMMA ; break ;
			case VK_OEM_MINUS:  s = KT_KEYTEXT_OEM_MINUS ; break ;
			case VK_OEM_PERIOD: s = KT_KEYTEXT_OEM_PERIOD ; break ;
			default: break ;
		}
		if( s != NULL ) { strncpy( name, s, sizeof(name) - 1 ) ; name[sizeof(name) - 1] = '\0' ; }
	}
	if( name[0] == '\0' ) { buf[0] = '\0' ; return 0 ; }
	n += (size_t) snprintf( buf + n, ( n < size ) ? size - n : 0, "%s", name ) ;
	if( n >= size ) { buf[0] = '\0' ; return 0 ; }
	return (int) n ;
}

/* The shortcut bound to a menu command, for the Tools entries that have one;
 * 0 for any other command. window.c does not see shortcuts_tab. */
int GetShortcutKey( int idm ) {
	switch( idm ) {
		case IDM_WINSCP:    return shortcuts_tab.winscp ;
		case IDM_PSCP:      return shortcuts_tab.sendfile ;
		case IDM_GETFILE:   return shortcuts_tab.getfile ;
		case IDM_FILEZILLA: return shortcuts_tab.filezilla ;
		default:            return 0 ;
	}
}

/* "<menu text>\t<key text>", the Windows convention for a menu item with a
 * keyboard shortcut. The key part is left off when the key is unset, has no
 * name, or when the [KiTTY] shortcuts switch is off - the menu then shows no
 * key it would not honour. Returns buf. */
const char * ShortcutMenuText( const char * text, int key, char * buf, size_t size ) {
	char keytext[64] ;
	if( ( buf == NULL ) || ( size == 0 ) ) return text ;
	if( GetShortcutsFlag() && ShortcutKeyText( key, keytext, sizeof(keytext) ) ) {
		snprintf( buf, size, "%s\t%s", text, keytext ) ;
	} else {
		snprintf( buf, size, "%s", text ) ;
	}
	return buf ;
}

/* A code from a virtual key and the modifier flags, composed as
 * ManageShortcuts composes the pressed key - so what the editor captures
 * compares equal to what the terminal window will see. */
int ShortcutKeyCode( int vk, int shift, int control, int alt, int altgr, int win ) {
	int key = vk ;
	if( vk <= 0 ) return 0 ;
	if( alt ) key += ALTKEY ;
	if( altgr ) key += ALTGRKEY ;
	if( shift ) key += SHIFTKEY ;
	if( control ) key += CONTROLKEY ;
	if( win ) key += WINKEY ;
	return key ;
}

/* The kitty.ini spelling of a code - {CONTROL}{SHIFT}{F4}, {ALT}{HOME},
 * {CONTROL}{ALT}T - the inverse of DefineShortcuts, so a key the editor
 * saves reads back as the same code and the file stays hand-editable.
 * Returns the length written; 0 with an empty buffer for an unset key (0)
 * and for a key DefineShortcuts has no name for. */
int ShortcutKeySyntax( int key, char * buf, size_t size ) {
	char name[16] ;
	int vk, win = 0, altgr = 0, alt = 0, ctrl = 0, shift = 0 ;
	size_t n = 0 ;
	if( ( buf == NULL ) || ( size == 0 ) ) return 0 ;
	buf[0] = '\0' ;
	if( key <= 0 ) return 0 ;
	if( key >= WINKEY )     { key -= WINKEY ; win = 1 ; }
	if( key >= ALTGRKEY )   { key -= ALTGRKEY ; altgr = 1 ; }
	if( key >= ALTKEY )     { key -= ALTKEY ; alt = 1 ; }
	if( key >= CONTROLKEY ) { key -= CONTROLKEY ; ctrl = 1 ; }
	if( key >= SHIFTKEY )   { key -= SHIFTKEY ; shift = 1 ; }
	vk = key ;
	name[0] = '\0' ;
	if( ( vk >= VK_F1 ) && ( vk <= VK_F12 ) ) {
		snprintf( name, sizeof(name), "{F%d}", vk - VK_F1 + 1 ) ;
	} else if( ( vk >= VK_NUMPAD0 ) && ( vk <= VK_NUMPAD9 ) ) {
		snprintf( name, sizeof(name), "{NUMPAD%d}", vk - VK_NUMPAD0 ) ;
	} else if( ( ( vk >= 'A' ) && ( vk <= 'Z' ) ) || ( ( vk >= '0' ) && ( vk <= '9' ) ) ) {
		name[0] = (char) vk ; name[1] = '\0' ;
	} else {
		const char * s = NULL ;
		switch( vk ) {
			case VK_RETURN:     s = "{RETURN}" ; break ;
			case VK_ESCAPE:     s = "{ESCAPE}" ; break ;
			case VK_SPACE:      s = "{SPACE}" ; break ;
			case VK_SNAPSHOT:   s = "{PRINT}" ; break ;
			case VK_PAUSE:      s = "{PAUSE}" ; break ;
			case VK_PRIOR:      s = "{PRIOR}" ; break ;
			case VK_NEXT:       s = "{NEXT}" ; break ;
			case VK_LEFT:       s = "{LEFT}" ; break ;
			case VK_RIGHT:      s = "{RIGHT}" ; break ;
			case VK_UP:         s = "{UP}" ; break ;
			case VK_DOWN:       s = "{DOWN}" ; break ;
			case VK_HOME:       s = "{HOME}" ; break ;
			case VK_END:        s = "{END}" ; break ;
			case VK_BACK:       s = "{BACK}" ; break ;
			case VK_TAB:        s = "{TAB}" ; break ;
			case VK_INSERT:     s = "{INS}" ; break ;
			case VK_DELETE:     s = "{DEL}" ; break ;
			case VK_ATTN:       s = "{ATTN}" ; break ;
			case VK_CANCEL:     s = "{BREAK}" ; break ;
			case VK_NUMLOCK:    s = "{NUMLOCK}" ; break ;
			case VK_SCROLL:     s = "{SCROLL}" ; break ;
			case VK_ADD:        s = "{ADD}" ; break ;
			case VK_SUBTRACT:   s = "{SUBTRACT}" ; break ;
			case VK_MULTIPLY:   s = "{MULTIPLY}" ; break ;
			case VK_DIVIDE:     s = "{DIVIDE}" ; break ;
			case VK_SEPARATOR:  s = "{SEPARATOR}" ; break ;
			case VK_DECIMAL:    s = "{DECIMAL}" ; break ;
			case VK_OEM_PLUS:   s = "{OEM_PLUS}" ; break ;
			case VK_OEM_COMMA:  s = "{OEM_COMMA}" ; break ;
			case VK_OEM_MINUS:  s = "{OEM_MINUS}" ; break ;
			case VK_OEM_PERIOD: s = "{OEM_PERIOD}" ; break ;
			default: break ;
		}
		if( s != NULL ) { strncpy( name, s, sizeof(name) - 1 ) ; name[sizeof(name) - 1] = '\0' ; }
	}
	if( name[0] == '\0' ) return 0 ;
	n = (size_t) snprintf( buf, size, "%s%s%s%s%s%s",
		ctrl ? "{CONTROL}" : "", alt ? "{ALT}" : "", shift ? "{SHIFT}" : "",
		win ? "{WIN}" : "", altgr ? "{ALTGR}" : "", name ) ;
	if( n >= size ) { buf[0] = '\0' ; return 0 ; }
	return (int) n ;
}

/* Ctrl+Shift+A..Z is also the predefined user commands' row: the slot
 * (1..26) such a code takes away from them, 0 for any other code. */
int ShortcutKeyUserCommand( int key ) {
	if( ( key >= CONTROLKEY+SHIFTKEY+'A' ) && ( key <= CONTROLKEY+SHIFTKEY+'Z' ) )
		return key - ( CONTROLKEY+SHIFTKEY+'A' ) + 1 ;
	return 0 ;
}

/* Codes no action or typing key may take: the fixed aliases ManageShortcuts
 * tests by value (Ctrl+Shift+F8 opens the multiline box beside inputm,
 * Ctrl+Shift+F12 resizes every window) return 1; Alt+F4, which closes the
 * window before any shortcut sees it, returns 2. */
int ShortcutKeyReserved( int key ) {
	if( ( key == CONTROLKEY+SHIFTKEY+VK_F8 ) || ( key == CONTROLKEY+SHIFTKEY+VK_F12 ) ) return 1 ;
	if( key == ALTKEY+VK_F4 ) return 2 ;
	return 0 ;
}

int SwitchLogMode(void) ;
int ManageShortcuts( Terminal *term, Conf *conf, HWND hwnd, const int* clips_system, int key_num, int shift_flag, int control_flag, int alt_flag, int altgr_flag, int win_flag ) {
	int key, i ;
	key = key_num ;
	if( alt_flag ) key = key + ALTKEY ;
	if( altgr_flag ) key = key + ALTGRKEY ;
	if( shift_flag ) key = key + SHIFTKEY ;
	if( control_flag ) key = key + CONTROLKEY ;
	if( win_flag ) key = key + WINKEY ;

//if( (key_num!=VK_SHIFT)&&(key_num!=VK_CONTROL) ) {char b[256] ; snprintf( b, sizeof(b), "alt=%d altgr=%d shift=%d control=%d key_num=%d key=%d action=%d", alt_flag, altgr_flag, shift_flag, control_flag, key_num, key, shortcuts_tab.duplicate ); MessageBox(hwnd, b, "Info", MB_OK);}

	if( key == shortcuts_tab.protect )				// Protection
		{ SendMessage( hwnd, WM_COMMAND, IDM_PROTECT, 0 ) ; InvalidateRect( hwnd, NULL, TRUE ) ; return 1 ; }
	if( key == shortcuts_tab.rollup ) 				// Winroll
			{ SendMessage( hwnd, WM_COMMAND, IDM_WINROL, 0 ) ; return 1 ; }
	if( key == shortcuts_tab.switchlogmode ) {
		i = SwitchLogMode() ;
		if( i==1 ) { debug_logevent( "Enable logging" ) ; } else { debug_logevent( "Disable logging" ) ; }
		return 1 ;
	}
	if( key == shortcuts_tab.showportforward ) 				// Fonction show port forward
		{ SendMessage( hwnd, WM_COMMAND, IDM_SHOWPORTFWD, 0 ) ; return 1 ; }

	if( (ProtectFlag == 1) || (WinHeight != -1) ) return 1 ;
		
	if( NbShortCuts ) {
		for( i=0 ; i<NbShortCuts ; i++ )
		if( shortcuts_tab2[i].num == key ) {
			SendKeyboardPlus( hwnd, shortcuts_tab2[i].st ) ;
			return 1 ; 
		}
	}
	
#ifdef MOD_BACKGROUNDIMAGE
	if( GetBackgroundImageFlag() && ImageViewerFlag ) { // Gestion du mode image
		if( ManageViewer( hwnd, key_num ) ) return 1 ;
		}
#endif
	if( control_flag && shift_flag && (key_num==VK_F12) ) {
		ResizeWinList( hwnd, conf_get_int(conf,CONF_width), conf_get_int(conf,CONF_height) ) ; return 1 ; 
	} // Resize all PuTTY windows to the size of the current one

	if( key == shortcuts_tab.printall ) {		
		SendMessage( hwnd, WM_COMMAND, IDM_COPYALL, 0 ) ;
		SendMessage( hwnd, WM_COMMAND, IDM_PRINT, 0 ) ;
		return 1 ;
	}

	if( key == shortcuts_tab.editor ) {			// Lancement d'un putty-ed
		if( debug_flag ) { debug_logevent( "Start empty internal editor" ) ; }
		RunPuttyEd( hwnd, NULL ) ; 
		return 1 ; 
	}
	if( key == (shortcuts_tab.editorclipboard ) ) {		// Lancement d'un putty-ed qui charge le contenu du presse-papier
		if( debug_flag ) { debug_logevent( "Start internal editor fullfiled with clipboard" ) ; }
		//term_copyall(term,clips_system,lenof(clips_system)) /* Full term clipboard */
		RunPuttyEd( hwnd, "1" ) ; 
		return 1 ; 
	/* The four Tools menu keys below do nothing while the session hides the
	 * entry (Connection > Transfers, "Tools menu"): the key then reaches the
	 * terminal as if it were no shortcut. */
	} else if( ( key == shortcuts_tab.winscp ) && kitty_xfer_tool_shown( conf, 1 ) ) {	// Lancement de WinSCP
		SendMessage( hwnd, WM_COMMAND, IDM_WINSCP, 0 ) ; return 1 ;
	} else if( ( key == shortcuts_tab.filezilla ) && kitty_xfer_tool_shown( conf, 2 ) && kitty_xfer_tool_ready( 2 ) ) {
		/* Bound only while the Tools menu carries "Start FileZilla", which is
		 * only while its executable exists. Otherwise the key is not a
		 * shortcut and reaches the terminal as it did before. */
		StartFileZilla( hwnd ) ; return 1 ;
	} else if( key == shortcuts_tab.autocommand ) { 		// Rejouer la commande de demarrage
			RenewPassword( conf ) ; 
			SetTimer(hwnd, TIMER_AUTOCOMMAND,autocommand_delay, NULL) ;
			return 1 ; 
	} if( key == shortcuts_tab.print ) {			// Impression presse papier
		SendMessage( hwnd, WM_COMMAND, IDM_PRINT, 0 ) ; 
		return 1 ; 
	}
	/* Ctrl+Shift+F8 is a fixed alias for the multiline box: users expect it
	 * right next to Ctrl+F8 (one-line box) / Shift+F8 (the default binding). */
	if( (key == shortcuts_tab.inputm) ||
	    (key == SHIFTKEY+CONTROLKEY+VK_F8) )	 	// Fenetre de controle
		{
		MainHwnd = hwnd ; _beginthread( routine_inputbox_multiline, 0, (void*)&hwnd ) ;
		return 1 ;
		}
#ifdef MOD_BACKGROUNDIMAGE
	if( GetBackgroundImageFlag() && (key == shortcuts_tab.viewer) ) 	// Switcher le mode visualiseur d'image
		{ SetImageViewerFlag( abs(ImageViewerFlag-1) ) ; set_title(NULL, conf_get_str(conf,CONF_wintitle) ) ; return 1 ; }
#endif
	if( key == shortcuts_tab.script ) 			// Chargement d'un fichier de script
		{ OpenAndSendScriptFile( hwnd ) ; return 1 ; }
	else if( ( key == shortcuts_tab.sendfile ) && kitty_xfer_tool_shown( conf, 0 ) ) 	// Envoi d'un fichier par SCP
		{ SendMessage( hwnd, WM_COMMAND, IDM_PSCP, 0 ) ; return 1 ; }
	else if( ( key == shortcuts_tab.getfile ) && kitty_xfer_tool_shown( conf, 3 ) ) 	// Reception d'un fichier par SCP
		{ GetFile( hwnd ) ; return 1 ; }
	else if( key == shortcuts_tab.command )			// Execution d'une commande locale
		{ RunCmd( hwnd ) ; return 1 ; }
	else if( key == shortcuts_tab.tray ) 		// Send to tray
		{ SendMessage( hwnd, WM_COMMAND, IDM_TOTRAY, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.visible )  		// Always visible 
		{ SendMessage( hwnd, WM_COMMAND, IDM_VISIBLE, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.resetterminal ) 		// Envoi d'un fichier par SCP
		{ SendMessage( hwnd, WM_COMMAND, IDM_RESET, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.duplicate ) 		// Duplicate session
		{ SendMessage( hwnd, WM_COMMAND, IDM_DUPSESS, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.opennew ) 		// Open new session
		{ SendMessage( hwnd, WM_COMMAND, IDM_NEWSESS, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.opennewcurrent ) 		// Open new config box with current settings
		/* The menu item, not a second implementation of it: passing NULL as the
		 * host left CONF_host in place, which makes the conf launchable, which
		 * makes RunSessionWithCurrentSettings connect instead of opening the
		 * box. That is Duplicate Session - already on its own shortcut - and
		 * not what this one is documented to do. */
		{ SendMessage( hwnd, WM_COMMAND, IDM_NEWDUPSESS, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.changesettings ) 		// Change settings
		{ SendMessage( hwnd, WM_COMMAND, IDM_RECONF, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.clearscrollback )		// Clear scrollback
		{ SendMessage( hwnd, WM_COMMAND, IDM_CLRSB, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.clearlogfile )		// Clear log file / start a new one
		{ SendMessage( hwnd, WM_COMMAND, IDM_CLEARLOGFILE, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.openlogfile )		// Open the current log file
		{ SendMessage( hwnd, WM_COMMAND, IDM_OPENLOGFILE, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.closerestart )		// Close + restart
		{ SendMessage( hwnd, WM_COMMAND, IDM_RESTARTSESSION, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.eventlog )		// Event log
		{ SendMessage( hwnd, WM_COMMAND, IDM_SHOWLOG, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fullscreen )		// Full screen
		{ SendMessage( hwnd, WM_COMMAND, IDM_FULLSCREEN, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fontup )			// Font up
		{ SendMessage( hwnd, WM_COMMAND, IDM_FONTUP, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fontdown )		// Font down
		{ SendMessage( hwnd, WM_COMMAND, IDM_FONTDOWN, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.copyall )			// Copy all to clipboard
		{ SendMessage( hwnd, WM_COMMAND, IDM_COPYALL, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fontnegative )		// Font negative
		{ SendMessage( hwnd, WM_COMMAND, IDM_FONTNEGATIVE, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fontblackandwhite )	// Font black and white
		{ SendMessage( hwnd, WM_COMMAND, IDM_FONTBLACKANDWHITE, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.keyexchange )		// Repeat key exchange
		{ SendMessage( hwnd, WM_COMMAND, IDM_REKEY, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fontreset )		// Font size back to the session's
		{ ChangeFontSize( term, conf, hwnd, 0 ) ; return 1 ; }
	/* The two transparency keys do nothing while the session has no
	 * transparency to change (the feature off, or the level locked at -1):
	 * the key then reaches the terminal as if it were no shortcut. */
	else if( ( key == shortcuts_tab.transparencyup ) && TransparencyFlag && ( conf_get_int(conf,CONF_transparencynumber) != -1 ) )
		{ SendMessage( hwnd, WM_COMMAND, IDM_TRANSPARUP, 0 ) ; return 1 ; }
	else if( ( key == shortcuts_tab.transparencydown ) && TransparencyFlag && ( conf_get_int(conf,CONF_transparencynumber) != -1 ) )
		{ SendMessage( hwnd, WM_COMMAND, IDM_TRANSPARDOWN, 0 ) ; return 1 ; }

	else if( key == shortcuts_tab.input ) 			// Fenetre de controle
		{
			/* Modeless box: open it directly on the main (UI) thread whose
			 * message pump routes the aux dialogs - NOT on a worker thread,
			 * which would exit immediately and leave the window unpumped. */
			MainHwnd = hwnd ; GetAndSendLine( hwnd ) ;
			InvalidateRect( hwnd, NULL, TRUE ) ; return 1 ;
		}

#ifdef MOD_BACKGROUNDIMAGE
	else if( GetBackgroundImageFlag() && (key == shortcuts_tab.imagechange) ) 		// Changement d'image de fond
		{ if( NextBgImage( hwnd ) ) InvalidateRect(hwnd, NULL, TRUE) ; return 1 ; }
#endif
	/* Ctrl+Up / Ctrl+Down (transparency), Ctrl+Num+ / Ctrl+Num- / Ctrl+Num 0
	 * (font size) were tested here by value; they are table actions now
	 * (transparencyup, transparencydown, fontup, fontdown, fontreset) with
	 * those keys as their defaults, so the editor shows them and a user can
	 * move or disable them. */

	/* KiTTY predefined-command accelerators: Ctrl+Shift+A..Z fires the Nth
	 * entry of the User Command menu (kitty_specialmenu.c labels the menu
	 * item with the same letter, for the first 26 entries).
	 *
	 * Deliberately LAST, and deliberately conditional on that entry EXISTING.
	 * This block used to run before every shortcuts_tab comparison and claim
	 * the whole A-Z range unconditionally, which had two consequences: any
	 * {CONTROL}{SHIFT}<letter> binding in [Shortcuts] was unreachable, and a
	 * letter with no command behind it was still swallowed - dispatched to a
	 * SpecialMenu[] slot that is NULL, so nothing happened and nothing said
	 * why. With no commands defined at all, which is the common case, that
	 * cost all 26 combinations for a menu KiTTY does not even display (it is
	 * only added when at least one command exists).
	 *
	 * SAVEMODE_DIR stays excluded: the directory loader assigns no
	 * accelerators, so there is nothing to dispatch there. */
	if( ( IniFileFlag != SAVEMODE_DIR ) && shift_flag && control_flag
	 && ( key_num >= 'A' ) && ( key_num <= 'Z' ) ) {
		int usercmd = key_num - 'A' ;
		if( ( usercmd < NB_MENU_MAX ) && ( SpecialMenu[usercmd] != NULL )
		 && ( strlen( SpecialMenu[usercmd] ) > 0 ) )
			{ SendMessage( hwnd, WM_COMMAND, IDM_USERCMD+usercmd, 0 ) ; return 1 ; }
	}

	return 0 ;
}
