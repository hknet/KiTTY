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
#include <string.h>

#include "putty.h"
#include "terminal.h"

#include <windows.h>

#include "kitty.h"
#include "kitty_commun.h"
#include "kitty_crypt.h"
#include "kitty_tools.h"

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

// Init shortcuts map at startup
void InitShortcuts( void ) {
	char buffer[4096], list[4096], *pl ;
	int i, t=0 ;
	if( !readINI(KittyIniFile,"Shortcuts","editor",buffer, sizeof(buffer)) || ( (shortcuts_tab.editor=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.editor = SHIFTKEY+VK_F2 ;
	if( !readINI(KittyIniFile,"Shortcuts","editorclipboard",buffer, sizeof(buffer)) || ( (shortcuts_tab.editorclipboard=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.editorclipboard = CONTROLKEY+SHIFTKEY+VK_F2 ;
	if( !readINI(KittyIniFile,"Shortcuts","winscp",buffer, sizeof(buffer)) || ( (shortcuts_tab.winscp=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.winscp = SHIFTKEY+VK_F3 ;
	if( !readINI(KittyIniFile,"Shortcuts","switchlogmode",buffer, sizeof(buffer)) || ( (shortcuts_tab.switchlogmode=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.switchlogmode = SHIFTKEY+VK_F5 ;
	if( !readINI(KittyIniFile,"Shortcuts","showportforward",buffer, sizeof(buffer)) || ( (shortcuts_tab.showportforward=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.showportforward = SHIFTKEY+VK_F6 ;
//	if( !IsWow64() ) {
		if( !readINI(KittyIniFile,"Shortcuts","print",buffer, sizeof(buffer)) || ( (shortcuts_tab.print=DefineShortcuts(buffer))<0 ) )
			shortcuts_tab.print = SHIFTKEY+VK_F7 ;
		if( !readINI(KittyIniFile,"Shortcuts","printall",buffer, sizeof(buffer)) || ( (shortcuts_tab.printall=DefineShortcuts(buffer))<0 ) )
			shortcuts_tab.printall = VK_F7 ;
//	}
	if( !readINI(KittyIniFile,"Shortcuts","inputm",buffer, sizeof(buffer)) || ( (shortcuts_tab.inputm=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.inputm = SHIFTKEY+VK_F8 ;
#ifdef MOD_BACKGROUNDIMAGE
	if( !readINI(KittyIniFile,"Shortcuts","viewer",buffer, sizeof(buffer)) || ( (shortcuts_tab.viewer=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.viewer = SHIFTKEY+VK_F11 ;
#endif
	if( !readINI(KittyIniFile,"Shortcuts","autocommand",buffer, sizeof(buffer)) || ( (shortcuts_tab.autocommand=DefineShortcuts(buffer))<0 ) ) 
		shortcuts_tab.autocommand = SHIFTKEY+VK_F12 ;

	if( !readINI(KittyIniFile,"Shortcuts","script",buffer, sizeof(buffer)) || ( (shortcuts_tab.script=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.script = CONTROLKEY+VK_F2 ;
	if( !readINI(KittyIniFile,"Shortcuts","sendfile",buffer, sizeof(buffer)) || ( (shortcuts_tab.sendfile=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.sendfile = CONTROLKEY+VK_F3 ;
	if( !readINI(KittyIniFile,"Shortcuts","getfile",buffer, sizeof(buffer)) || ( (shortcuts_tab.getfile=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.getfile = CONTROLKEY+VK_F4 ;
	if( !readINI(KittyIniFile,"Shortcuts","command",buffer, sizeof(buffer)) || ( (shortcuts_tab.command=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.command = CONTROLKEY+VK_F5 ;
	if( !readINI(KittyIniFile,"Shortcuts","tray",buffer, sizeof(buffer)) || ( (shortcuts_tab.tray=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.tray = CONTROLKEY+VK_F6 ;
	if( !readINI(KittyIniFile,"Shortcuts","visible",buffer, sizeof(buffer)) || ( (shortcuts_tab.visible=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.visible = CONTROLKEY+VK_F7 ;
	if( !readINI(KittyIniFile,"Shortcuts","input",buffer, sizeof(buffer)) || ( (shortcuts_tab.input=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.input = CONTROLKEY+VK_F8 ;
	if( !readINI(KittyIniFile,"Shortcuts","protect",buffer, sizeof(buffer)) || ( (shortcuts_tab.protect=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.protect = CONTROLKEY+VK_F9 ;
#ifdef MOD_BACKGROUNDIMAGE
	if( !readINI(KittyIniFile,"Shortcuts","imagechange",buffer, sizeof(buffer)) || ( (shortcuts_tab.imagechange=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.imagechange = CONTROLKEY+VK_F11 ;
#endif
	if( !readINI(KittyIniFile,"Shortcuts","rollup",buffer, sizeof(buffer)) || ( (shortcuts_tab.rollup=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.rollup = CONTROLKEY+VK_F12 ;
	if( !readINI(KittyIniFile,"Shortcuts","resetterminal",buffer, sizeof(buffer)) || ( (shortcuts_tab.resetterminal=DefineShortcuts(buffer))<0 ) ) 
		shortcuts_tab.resetterminal = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","duplicate",buffer, sizeof(buffer)) || ( (shortcuts_tab.duplicate=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.duplicate = CONTROLKEY+ALTKEY+84 ;
	if( !readINI(KittyIniFile,"Shortcuts","opennew",buffer, sizeof(buffer)) || ( (shortcuts_tab.opennew=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.opennew = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","opennewcurrent",buffer, sizeof(buffer)) || ( (shortcuts_tab.opennewcurrent=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.opennewcurrent = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","changesettings",buffer, sizeof(buffer)) || ( (shortcuts_tab.changesettings=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.changesettings = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","clearscrollback",buffer, sizeof(buffer)) || ( (shortcuts_tab.clearscrollback=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.clearscrollback = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","clearlogfile",buffer, sizeof(buffer)) || ( (shortcuts_tab.clearlogfile=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.clearlogfile = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","closerestart",buffer, sizeof(buffer)) || ( (shortcuts_tab.closerestart=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.closerestart = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","eventlog",buffer, sizeof(buffer)) || ( (shortcuts_tab.eventlog=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.eventlog = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","fullscreen",buffer, sizeof(buffer)) || ( (shortcuts_tab.fullscreen=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.fullscreen = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","fontup",buffer, sizeof(buffer)) || ( (shortcuts_tab.fontup=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.fontup = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","fontdown",buffer, sizeof(buffer)) || ( (shortcuts_tab.fontdown=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.fontdown = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","copyall",buffer, sizeof(buffer)) || ( (shortcuts_tab.copyall=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.copyall = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","fontnegative",buffer, sizeof(buffer)) || ( (shortcuts_tab.fontnegative=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.fontnegative = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","fontblackandwhite",buffer, sizeof(buffer)) || ( (shortcuts_tab.fontblackandwhite=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.fontblackandwhite = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","keyexchange",buffer, sizeof(buffer)) || ( (shortcuts_tab.keyexchange=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.keyexchange = 0 ;

	
	if( NbShortCuts>0 ) for( i=0 ; i<NbShortCuts ; i++ ) { if( shortcuts_tab2[i].st!=NULL ) { free(shortcuts_tab2[i].st) ; } }
	NbShortCuts=0 ;
	if( ReadParameterN( "Shortcuts", "list", list, sizeof(list) ) ) {
		pl=list ;
		while( strlen(pl) > 0 ) {
			i=0;
			while( (i<strlen(pl))&&(pl[i]!=' ') ) { i++ ; }
			if( pl[i]==' ' ) { pl[i]='\0' ; t=1 ; }
			if( strlen(pl)>0 )
			if( ReadParameterN( "Shortcuts", pl, buffer, sizeof(buffer) ) ) {
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
	} else if( key == shortcuts_tab.winscp ) {			// Lancement de WinSCP
		SendMessage( hwnd, WM_COMMAND, IDM_WINSCP, 0 ) ; return 1 ;
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
	else if( key == shortcuts_tab.sendfile ) 		// Envoi d'un fichier par SCP
		{ SendMessage( hwnd, WM_COMMAND, IDM_PSCP, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.getfile ) 		// Reception d'un fichier par SCP
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
	else if( key == shortcuts_tab.clearlogfile )		// Clear log file
		{ SendMessage( hwnd, WM_COMMAND, IDM_CLEARLOGFILE, 0 ) ; return 1 ; }
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
/*
	if( control_flag && shift_flag ) {
		if(key_num == VK_UP) { SendMessage( hwnd, WM_COMMAND, IDM_FONTUP, 0 ) ; return 1 ; }
		if(key_num == VK_DOWN) { SendMessage( hwnd, WM_COMMAND, IDM_FONTDOWN, 0 ) ; return 1 ; }
		if(key_num == VK_LEFT ) {  ChangeFontSize(hwnd,0) ; return 1 ; }
	}*/
	if( control_flag && !shift_flag ) {
		if( TransparencyFlag && (conf_get_int(conf,CONF_transparencynumber)!=-1)&&(key_num == VK_UP) ) // Augmenter l'opacite (diminuer la transparence)
			{ SendMessage( hwnd, WM_COMMAND, IDM_TRANSPARUP, 0 ) ; return 1 ; }
		if( TransparencyFlag && (conf_get_int(conf,CONF_transparencynumber)!=-1)&&(key_num == VK_DOWN) ) // Diminuer l'opacite (augmenter la transparence)
			{ SendMessage( hwnd, WM_COMMAND, IDM_TRANSPARDOWN, 0 ) ; return 1 ; }

		if (key_num == VK_ADD) { SendMessage( hwnd, WM_COMMAND, IDM_FONTUP, 0 ) ; return 1 ; }
		if (key_num == VK_SUBTRACT) { SendMessage( hwnd, WM_COMMAND, IDM_FONTDOWN, 0 ) ; return 1 ; }
		if (key_num == VK_NUMPAD0) { ChangeFontSize(term,conf,hwnd,0) ; return 1 ; }
#ifdef MOD_LAUNCHER
		/*    ====> Ne fonctionne pas !!!
		if (key_num == VK_LEFT ) //Fenetre KiTTY precedente
			{ SendMessage( hwnd, WM_COMMAND, IDM_GOPREVIOUS, 0 ) ; return 1 ; }
		if (key_num == VK_RIGHT ) //Fenetre KiTTY Suivante
			{ SendMessage( hwnd, WM_COMMAND, IDM_GONEXT, 0 ) ; return 1 ; }
		*/
#endif
	}

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
