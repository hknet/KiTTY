/*
 * kitty_portfwd.c - the port-forward display: the list of this session's
 * forwardings with their listening state, put on the clipboard and shown
 * on screen.
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
#include "kitty_gui.h"
#include "kitty_bridge.h"
#include "kitty_exportbundle.h"
#include "mini/mini.h"
#include "kitty_b64.h"
#include "kitty_store.h"
#include "kitty_portfwd.h"

	
// Port-forward display window
// Put the port-forward list on the clipboard and show it on screen
// [C] listening in the current process, [X] listening in another process,
// [-] not listening
DWORD (WINAPI *pGetExtendedTcpTable)(
  PVOID pTcpTable,
  PDWORD pdwSize,
  BOOL bOrder,
  ULONG ulAf,
  TCP_TABLE_CLASS TableClass,
  ULONG Reserved
) ;

int GetPortFwdState( const int port, const DWORD pid ) {
	int result = -1 ;
	MIB_TCPTABLE_OWNER_PID *pTCPInfo ;
	MIB_TCPROW_OWNER_PID *owner ;
	DWORD size ;
	DWORD dwResult ;
	DWORD dwLoop ;
	
	HMODULE hLib = LoadLibrary( "iphlpapi.dll" );

	if( hLib ) {
		pGetExtendedTcpTable = (DWORD (WINAPI *)(PVOID,PDWORD,BOOL,ULONG,TCP_TABLE_CLASS,ULONG)) 
		kitty_api_from(hLib, "iphlpapi.dll", "GetExtendedTcpTable", KITTY_API_OPTIONAL,
                                  KT_WINFEAT_TCP_PORT_OWNER) ;
		dwResult = pGetExtendedTcpTable(NULL, &size, 0, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) ;
		pTCPInfo = (MIB_TCPTABLE_OWNER_PID*)malloc(size) ;
		dwResult = pGetExtendedTcpTable(pTCPInfo, &size, 0, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) ;
		if( pGetExtendedTcpTable && (dwResult == NO_ERROR) ) {
			if( pTCPInfo->dwNumEntries > 0 ) {
				for (dwLoop = 0; dwLoop < pTCPInfo->dwNumEntries; dwLoop++) {
					owner = &pTCPInfo->table[dwLoop];
					if ( owner->dwState == MIB_TCP_STATE_LISTEN ) {
						if( ntohs(owner->dwLocalPort) == port ) {
							if( pid == owner->dwOwningPid ) { 
								result = 0 ; 
							} else { 
								result = owner->dwOwningPid ; 
							}
							break;
						}
					}
				}
			}
		}
		
		free(pTCPInfo) ;
		FreeLibrary( hLib ) ;
	}
	return result ;
}

int ShowPortfwd( HWND hwnd, Conf * conf ) {
	char pf[2100]="" ;
	char *key, *val;
	for (val = conf_get_str_strs(conf, CONF_portfwd, NULL, &key) ;
		val != NULL;
		val = conf_get_str_strs(conf, CONF_portfwd, key, &key)) {
		char *p;
		if (( key[0]=='R' ) || ( key[1]=='R' )) {
			p = dupprintf("[-] %s \t\t<-- \t%s\n", (key[1]=='R')? key+2 : key+1,val) ;
		} else if (( key[0]=='L' ) || ( key[1]=='L' )) {
			char *key_pos = (key[1]=='L')? key+2 : key+1 ;
			int res ;
			switch( res=GetPortFwdState( atoi(key_pos), GetCurrentProcessId() ) ) {
				case -1:
					p = dupprintf("[-] %s \t\t--> \t%s\n", key_pos,val) ;
					break ;
				case 0:
					p = dupprintf("[C] %s \t\t--> \t%s\n", key_pos,val) ;
					break ;
				default:
					p = dupprintf("[X] %s(%u)\t--> \t%s\n", key_pos,(unsigned int)res,val) ;
			}
		} else if (( key[0]=='D' ) || ( key[1]=='D' )) {
			p = dupprintf("D%s\t\n", key+1) ;
		} else {
			p = dupprintf("%s\t%s\n", key, val) ;
		}
		if( (strlen(pf)+strlen(p))<2000 ) {
			strcat( pf, p ) ;
			sfree(p) ;
		} else {
			strcat( pf, "...\n" ) ;
			break ;
		}
	}
	/*
	MIB_TCPTABLE_OWNER_PID *pTCPInfo;
	MIB_TCPROW_OWNER_PID *owner;
	DWORD size;
	DWORD dwResult;
	DWORD dwLoop;

	HMODULE hLib = LoadLibrary( "iphlpapi.dll" );

	if( hLib ) {
		pGetExtendedTcpTable = (DWORD (WINAPI *)(PVOID,PDWORD,BOOL,ULONG,TCP_TABLE_CLASS,ULONG))
		kitty_api_from(hLib, "iphlpapi.dll", "GetExtendedTcpTable", KITTY_API_OPTIONAL,
                                  "naming the program that owns a TCP port");
	}
	dwResult = pGetExtendedTcpTable(NULL, &size, 0, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
	pTCPInfo = (MIB_TCPTABLE_OWNER_PID*)malloc(size);
	dwResult = pGetExtendedTcpTable(pTCPInfo, &size, 0, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);

	for (val = conf_get_str_strs(conf, CONF_portfwd, NULL, &key);
		val != NULL;
		val = conf_get_str_strs(conf, CONF_portfwd, key, &key)) {
		char *p;
		
		if (( key[0]=='R' ) || ( key[1]=='R' )) {
			p = dupprintf("[-] %s \t\t<-- \t%s\n", (key[1]=='R')? key+2 : key+1,val);
		} else if (( key[0]=='L' ) || ( key[1]=='L' )) {
			char *key_pos = (key[1]=='L')? key+2 : key+1;
			if( pGetExtendedTcpTable && (dwResult == NO_ERROR) ) {
				int found=0 ;
				if( pTCPInfo->dwNumEntries > 0 ) {
					for (dwLoop = 0; dwLoop < pTCPInfo->dwNumEntries; dwLoop++) {
						owner = &pTCPInfo->table[dwLoop];
						if ( owner->dwState == MIB_TCP_STATE_LISTEN ) {
							if( ntohs(owner->dwLocalPort) == atoi(key_pos) ) {
								if( GetCurrentProcessId() == owner->dwOwningPid ) p = dupprintf("[C] %s \t\t--> \t%s\n", key_pos,val);
								else p = dupprintf("[X] %s(%u)\t--> \t%s\n", key_pos,(unsigned int)owner->dwOwningPid,val) ;
								found=1;
								break;
							}
						}
					}
				}
				if( !found ) { p = dupprintf("[-] %s \t\t--> \t%s\n", key_pos,val); }
			} else {
				p = dupprintf("[-] %s \t\t--> \t%s\n", key_pos,val);
			}
		} else if (( key[0]=='D' ) || ( key[1]=='D' )) {
			p = dupprintf("D%s\t\n", key+1) ;
		} else {
			p = dupprintf("%s\t%s\n", key, val) ;
		}
		
		strcat( pf, p ) ;
		sfree(p);
	}
	
	if( hLib ) { FreeLibrary( hLib ) ; }
	*/
	strcat( pf, KT_MAIN_PORTFWD_LEGEND );
	MessageBox( NULL, pf, KT_CAP_PORT_FORWARDING, MB_OK ) ;
	return SetTextToClipboard( pf ) ;
}
