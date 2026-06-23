#include "kitty_win.h"
#include <wininet.h>   /* CheckVersionFromWebSite: GitHub releases query */
#include <wintrust.h>  /* in-app updater: Authenticode trust verification */
#include <softpub.h>   /* WINTRUST_ACTION_GENERIC_VERIFY_V2 */
#include <msi.h>       /* in-app updater: install-type detection by UpgradeCode */
/* wincrypt.h (CryptQueryObject / signer cert) comes in via windows.h */

/* MOD_PERSO event-log wrapper, defined in windows/window.c */
void do_eventlog(const char *st) ;

// Modifie la transparence
void SetTransparency( HWND hwnd, int value ) {
#ifndef MOD_NOTRANSPARENCY
	SetLayeredWindowAttributes( hwnd, 0, value, LWA_ALPHA ) ;
#endif
	}


// Numéro de version de l'OS
void GetOSInfo( char * version ) { // ==> Deprecated with version >= Windows 8.1
	OSVERSIONINFO osvi;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&osvi);
	sprintf( version, "%ld.%ld %ld %ld %s %dx%d", osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber, osvi.dwPlatformId, osvi.szCSDVersion, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) ) ;
}
/*
http://msdn.microsoft.com/en-us/library/windows/desktop/ms724832%28v=vs.85%29.aspx
Operating system 			Version number
Windows 10 Insider Preview		10.0*
Windows Server Technical Preview	10.0*
Windows Server 2019 			10.0*
Windows Server 2016 			10.0*
Windows 8.1				6.3*
Windows Server 2012 R2			6.3*
Windows 8				6.2
Windows Server 2012			6.2
Windows 7				6.1
Windows Server 2008 R2			6.1
Windows Server 2008			6.0
Windows Vista				6.0
Windows Server 2003 R2			5.2
Windows Server 2003			5.2
Windows XP 64-Bit Edition		5.2
Windows XP				5.1
Windows 2000				5.0
*/

/*
https://msdn.microsoft.com/en-us/library/aa383745%28v=vs.85%29.aspx#faster_builds_with_smaller_header_files
Minimum system required					Minimum value for _WIN32_WINNT and WINVER
Windows 8.1						_WIN32_WINNT_WINBLUE (0x0602)
Windows 8						_WIN32_WINNT_WIN8 (0x0602)
Windows 7						_WIN32_WINNT_WIN7 (0x0601)
Windows Server 2008					_WIN32_WINNT_WS08 (0x0600)
Windows Vista						_WIN32_WINNT_VISTA (0x0600)
Windows Server 2003 with SP1, Windows XP with SP2	_WIN32_WINNT_WS03 (0x0502)
Windows Server 2003, Windows XP				_WIN32_WINNT_WINXP (0x0501)

Minimum version required		Minimum value of _WIN32_IE
Internet Explorer 10.0			_WIN32_IE_IE100 (0x0A00)
Internet Explorer 9.0			_WIN32_IE_IE90 (0x0900)
Internet Explorer 8.0			_WIN32_IE_IE80 (0x0800)
Internet Explorer 7.0			_WIN32_IE_IE70 (0x0700)
Internet Explorer 6.0 SP2		_WIN32_IE_IE60SP2 (0x0603)
Internet Explorer 6.0 SP1		_WIN32_IE_IE60SP1 (0x0601)
Internet Explorer 6.0			_WIN32_IE_IE60 (0x0600)
Internet Explorer 5.5			_WIN32_IE_IE55 (0x0550)
Internet Explorer 5.01			_WIN32_IE_IE501 (0x0501)
Internet Explorer 5.0, 5.0a, 5.0b	_WIN32_IE_IE50 (0x0500)
*/

typedef BOOL (WINAPI *LPFN_ISWOW64PROCESS) (HANDLE, PBOOL);
LPFN_ISWOW64PROCESS fnIsWow64Process;
BOOL IsWow64() {
    BOOL bIsWow64 = FALSE;
    //IsWow64Process is not available on all supported versions of Windows.
    //Use GetModuleHandle to get a handle to the DLL that contains the function
    //and GetProcAddress to get a pointer to the function if available.
    fnIsWow64Process = (LPFN_ISWOW64PROCESS) GetProcAddress(
        GetModuleHandle(TEXT("kernel32")),"IsWow64Process");
    if(NULL != fnIsWow64Process) {
        if (!fnIsWow64Process(GetCurrentProcess(),&bIsWow64)) {
            //handle error
        }
    }
    return bIsWow64 ;
}

int OpenFileName( HWND hFrame, char * filename, char * Title, char * Filter ) {
	char * szTitle = Title ;
	char szFilter[256] ; strcpy( szFilter, Filter ) ;
	// on remplace les caractères '|' par des caractères NULL.
	int i = 0;
	while(i < sizeof(szFilter) && szFilter[i] != '\0')
	{
		if(szFilter[i] == '|')
			szFilter[i] = '\0';

		i++;
	}

	// boîte de dialogue de demande d'ouverture de fichier
	//char szFileName[_MAX_PATH + 1] = "";
	char * szFileName = filename ;
	szFileName[0] = '\0' ;
	OPENFILENAME ofn	= {0};
	ofn.lStructSize		= sizeof(OPENFILENAME);
	ofn.hwndOwner		= hFrame;
	ofn.lpstrFilter		= szFilter;
	ofn.nFilterIndex	= 1;
	ofn.lpstrFile		= szFileName;
	//ofn.nMaxFile		= sizeof(szFileName);
	ofn.nMaxFile		= 4096 ;
	ofn.lpstrTitle		= szTitle;
	ofn.Flags		= OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST 
				| OFN_HIDEREADONLY | OFN_LONGNAMES
				| OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_EXTENSIONDIFFERENT | OFN_DONTADDTORECENT
				;

	// si aucun nom de fichier n'a été sélectionné, on abandonne
	if(!GetOpenFileName(&ofn)) { return 0 ; }
	else { return 1 ; }
	}

int SaveFileName( HWND hFrame, char * filename, char * Title, char * Filter ) {
	char * szTitle = Title ;
	char szFilter[256] ; strcpy( szFilter, Filter ) ;
	// on remplace les caractères '|' par des caractères NULL.
	int i = 0;
	while(i < sizeof(szFilter) && szFilter[i] != '\0')
	{
		if(szFilter[i] == '|')
			szFilter[i] = '\0';

		i++;
	}

	// boîte de dialogue de demande d'ouverture de fichier
	//char szFileName[_MAX_PATH + 1] = "";
	char * szFileName = filename ;
	szFileName[0] = '\0' ;
	OPENFILENAME ofn	= {0};
	ofn.lStructSize		= sizeof(OPENFILENAME);
	ofn.hwndOwner		= hFrame;
	ofn.lpstrFilter		= szFilter;
	ofn.nFilterIndex	= 1;
	ofn.lpstrFile		= szFileName;
	//ofn.nMaxFile		= sizeof(szFileName);
	ofn.nMaxFile		= 4096 ;
	ofn.lpstrTitle		= szTitle;
	ofn.lpstrDefExt 	= ".ktx" ;
	ofn.Flags		= OFN_PATHMUSTEXIST 
				| OFN_HIDEREADONLY | OFN_LONGNAMES | OFN_OVERWRITEPROMPT
				| OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_EXTENSIONDIFFERENT | OFN_DONTADDTORECENT
				;

	// si aucun nom de fichier n'a été sélectionné, on abandonne
	if(!GetSaveFileName(&ofn)) { return 0 ; }
	else { return 1 ; }
	}

#include <shlobj.h>
int OpenDirName( HWND hFrame, char * dirname ) {
	BROWSEINFO bi ;
	ITEMIDLIST *il ;
	LPITEMIDLIST ol = NULL ;
	char Buffer[4096],Result[4096]="" ;
	dirname[0]='\0' ;

	strcpy( Buffer, getenv("ProgramFiles") ) ;
	
	//SHGetSpecialFolderLocation( hFrame, CSIDL_MYDOCUMENTS, &ol );
	
	memset(&bi,0,sizeof(BROWSEINFO));
	bi.hwndOwner = hFrame ;
	//bi.pidlRoot=NULL ; //
	bi.pidlRoot=ol ;
	bi.pszDisplayName=&Buffer[0];
	bi.lpszTitle="Select a folder...";
	bi.ulFlags=0;
	bi.lpfn=NULL;
	if ((il=SHBrowseForFolder(&bi))!=NULL) {
		SHGetPathFromIDList(il,&Result[0]) ;
		//ILFree( il ) ; ILFree( ol ) ;
		GlobalFree(il);GlobalFree(ol);
		if( strlen( Result ) == 0 ) return 0 ;
		strcpy( dirname, Result ) ;
		return 1 ;
		}
	//ILFree( ol ) ;
	GlobalFree(ol);
	return 0 ;
	}

// Centre un dialog au milieu de la fenetre parent
void CenterDlgInParent(HWND hDlg) {
  RECT rcDlg;
  HWND hParent;
  RECT rcParent;
  MONITORINFO mi;
  HMONITOR hMonitor;

  int xMin, yMin, xMax, yMax, x, y;

  GetWindowRect(hDlg,&rcDlg);

  hParent = GetParent(hDlg);
  GetWindowRect(hParent,&rcParent);

  hMonitor = MonitorFromRect(&rcParent,MONITOR_DEFAULTTONEAREST);
  mi.cbSize = sizeof(mi);
  GetMonitorInfo(hMonitor,&mi);

  xMin = mi.rcWork.left;
  yMin = mi.rcWork.top;

  xMax = (mi.rcWork.right) - (rcDlg.right - rcDlg.left);
  yMax = (mi.rcWork.bottom) - (rcDlg.bottom - rcDlg.top);

  if ((rcParent.right - rcParent.left) - (rcDlg.right - rcDlg.left) > 20)
    x = rcParent.left + (((rcParent.right - rcParent.left) - (rcDlg.right - rcDlg.left)) / 2);
  else
    x = rcParent.left + 70;

  if ((rcParent.bottom - rcParent.top) - (rcDlg.bottom - rcDlg.top) > 20)
    y = rcParent.top  + (((rcParent.bottom - rcParent.top) - (rcDlg.bottom - rcDlg.top)) / 2);
  else
    y = rcParent.top + 60;

  SetWindowPos(hDlg,NULL,max(xMin,min(xMax,x)),max(yMin,min(yMax,y)),0,0,SWP_NOZORDER|SWP_NOSIZE);
}


//
// Envoi vers l'imprimante
//
// Parametres de l'impression
int PrintCharSize = 100 ;
int PrintMaxLinePerPage = 60 ;
int PrintMaxCharPerLine = 85 ;

int PrintText( const char * Text ) {
	int return_code = 0 ; 
	PRINTDLG	pd;
	DOCINFO		di;
	int i, TextLen = 0, Index1 = 0, Index2 = 2;
	//int Exit = 0 ;
	char*		LinePrint = NULL ;
	char*		szMessage = NULL ;

	if( Text == NULL ) return 1 ;
	if( strlen( Text ) == 0 ) return 1 ;

	memset (&pd, 0, sizeof(PRINTDLG));
	memset (&di, 0, sizeof(DOCINFO));

	di.cbSize = sizeof(DOCINFO);
	di.lpszDocName = "Test";

	pd.lStructSize = sizeof(PRINTDLG);
	pd.Flags = PD_PAGENUMS | PD_RETURNDC;
	pd.nFromPage = 1;
	pd.nToPage = 1;
	pd.nMinPage = 1;
	pd.nMaxPage = 1;
	szMessage = 0;

	if( PrintDlg( &pd ) ) {
		if( pd.hDC ) {
			if (StartDoc (pd.hDC, &di) != SP_ERROR)	{
				TextLen = strlen( Text ) ;
				if( TextLen > 0 ) {
					LinePrint = (char*) malloc( TextLen + 2 ) ;
					Index1 = 0 ; Index2 = 2 ; 
					//Exit = 0 ;
					for( i = 0 ; i < TextLen ; i++ ) {
						if( Text[i]=='\r' ) i++;
                    				LinePrint[Index1] = Text[i] ;
                    				if( Text[i] == '\n' ) {
                      					Index2++ ;
							LinePrint[Index1] = '\0' ;
                      					TextOut(pd.hDC,100, Index2*PrintCharSize, LinePrint, strlen(LinePrint) ) ;
							Index1 = 0 ;
                    					}
						else if( Index1>=PrintMaxCharPerLine ) {
							Index2++ ;
							LinePrint[Index1+1] = '\0' ;
                      					TextOut(pd.hDC,100, Index2*PrintCharSize, LinePrint, strlen(LinePrint) ) ;
							Index1 = 0 ;
							}
                    				else { Index1++ ; }
                    				if( Index2 >= PrintMaxLinePerPage ) {
                  	   				EndPage( pd.hDC ) ;
                       					//EndDoc(pd.hDC) ;
                       					//StartDoc(pd.hDC, &di) ;
							StartPage( pd.hDC ) ;
                       					Index2 = 2 ;
                       					}
                  				}
                  			Index2++ ; 
                  			LinePrint[Index1] = '\0'; // Impression de la dernière page
                  			TextOut(pd.hDC,100, Index2*PrintCharSize, LinePrint, strlen(LinePrint)) ;
               	  			EndPage(pd.hDC) ;
                  			EndDoc(pd.hDC) ;
                  			szMessage = "Print successful";
					free( LinePrint ) ;
              				}
              			else { return_code = 1 ;  /* Chaine vide */ }
				}
			else { // Problème StartDoc
				szMessage = "ERROR Type 1" ;
				return_code = 2 ;
				}
			}
		else { // Probleme pd.hDC
			szMessage = "ERROR Type 2." ;
			return_code = 3 ;
			}
		}
	else { // Problème PrintDlg
		//szMessage = "Impression annulée par l'utilisateur" ;
		return_code = 4 ;
		}
	if (szMessage) { MessageBox (NULL, szMessage, "Print report", MB_OK) ; }
	
	return return_code ;
	}

// Impression du texte dans le bloc-notes
void ManagePrint( HWND hwnd ) {
	char *pst = NULL ;
	if( OpenClipboard(NULL) ) {
		HGLOBAL hglb ;
		if( (hglb = GetClipboardData( CF_TEXT ) ) != NULL ) {
			if( ( pst = GlobalLock( hglb ) ) != NULL ) {
				PrintText( pst ) ;
				GlobalUnlock( hglb ) ;
			}
		}
		CloseClipboard();
	}
}

// Met un texte dans le press-papier
int SetTextToClipboard( const char * buf ) {
	HGLOBAL hglbCopy ;
	LPTSTR lptstrCopy ;
	if( !IsClipboardFormatAvailable(CF_TEXT) ) return 0 ;
	if( !OpenClipboard(NULL) ) return 0 ;
	EmptyClipboard() ; 
	if( (hglbCopy= GlobalAlloc(GMEM_MOVEABLE, (strlen(buf)+1) * sizeof(TCHAR)) ) == NULL ) {
		CloseClipboard() ; 
		return 0 ;	
	}
	lptstrCopy = GlobalLock( hglbCopy ) ; 
	memcpy( lptstrCopy, buf, (strlen(buf)+1) * sizeof(TCHAR) ) ;
	GlobalUnlock( hglbCopy ) ; 
	if( SetClipboardData(CF_TEXT, hglbCopy) == NULL ) {
		CloseClipboard() ;
		return 0 ; 
	}
	CloseClipboard() ;
	return 1 ;
}

// Execute une commande	
void RunCommand( HWND hwnd, const char * cmd ) {
	PROCESS_INFORMATION ProcessInformation ;
	ZeroMemory( &ProcessInformation, sizeof(ProcessInformation) );
	
	STARTUPINFO StartUpInfo ;
	ZeroMemory( &StartUpInfo, sizeof(StartUpInfo) );
	StartUpInfo.cb=sizeof(STARTUPINFO);
	StartUpInfo.lpReserved=0;
	StartUpInfo.lpDesktop=0;
	StartUpInfo.lpTitle=0;
	StartUpInfo.dwX=0;
	StartUpInfo.dwY=0;
	StartUpInfo.dwXSize=0;
	StartUpInfo.dwYSize=0;
	StartUpInfo.dwXCountChars=0;
	StartUpInfo.dwYCountChars=0;
	StartUpInfo.dwFillAttribute=0;
	StartUpInfo.dwFlags=0;
	StartUpInfo.wShowWindow=0;
	StartUpInfo.cbReserved2=0;
	StartUpInfo.lpReserved2=0;
	StartUpInfo.hStdInput=0;
	StartUpInfo.hStdOutput=0;
	StartUpInfo.hStdError=0;
//MessageBox(hwnd,cmd,"Info",MB_OK);

	if( !CreateProcess(NULL,(CHAR*)cmd,NULL,NULL,FALSE,NORMAL_PRIORITY_CLASS,NULL,NULL,&StartUpInfo,&ProcessInformation) ) {
		ShellExecute(hwnd, "open", cmd ,0 , 0, SW_SHOWDEFAULT);
	} else {
		/* Grant the spawned session the right to bring its window to the
		 * foreground. Without this, the new KiTTY window's SetForegroundWindow()
		 * (window.c) is blocked by Windows' foreground lock when we launch from
		 * the tray launcher, so the window opens behind and never gets focus. */
		AllowSetForegroundWindow( ProcessInformation.dwProcessId ) ;
		WaitForInputIdle(ProcessInformation.hProcess, INFINITE );
		CloseHandle( &StartUpInfo );
		CloseHandle( &ProcessInformation );
	}
}

void RunPuttyEd( HWND hwnd, char * filename ) {
	char buffer[1024]="", shortname[1024]="" ;
	if( GetModuleFileName( NULL, (LPTSTR)buffer, 1023 ) ) if( GetShortPathName( buffer, shortname, 1023 ) ) {
		strcat( shortname, " -ed" );
		if( filename!=NULL ) if( strlen(filename)>0 ) { 
			strcat( shortname, "b " ) ; 
			strcat( shortname, filename ) ; 
		}
		debug_logevent( shortname ) ;
		RunCommand( hwnd, shortname ) ; 
	}
}

// Verifie si une mise a jour est disponible (depot GitHub hknet/KiTTY)
extern char BuildVersionTime[256] ;

/* Parse a dotted version "0.84.0.15" into 4 comparable integers. */
static void kitty_parse_version( const char *s, int v[4] ) {
	v[0]=v[1]=v[2]=v[3]=0 ;
	sscanf( s, "%d.%d.%d.%d", &v[0], &v[1], &v[2], &v[3] ) ;
}
/* Return <0 if a<b, 0 if equal, >0 if a>b. */
static int kitty_version_cmp( const int a[4], const int b[4] ) {
	int i ;
	for( i=0 ; i<4 ; i++ ) { if( a[i]!=b[i] ) return (a[i]<b[i]) ? -1 : 1 ; }
	return 0 ;
}

/* The page a user lands on to download a new build, and the JSON API we query.
 * We use the /releases list (newest first) rather than /releases/latest, because
 * /releases/latest skips pre-releases and every KiTTY build is a -beta prerelease,
 * so /latest would 404. The first "tag_name" in the array is the newest release. */
#define KITTY_RELEASES_URL "https://github.com/hknet/KiTTY/releases"
#define KITTY_RELEASES_API "https://api.github.com/repos/hknet/KiTTY/releases?per_page=1"

/* ===================== In-app updater =====================
 * When "Check for updates" finds a newer release, we can download the correct
 * installer asset and run it - but ONLY after verifying it is a genuine,
 * KAPPER-signed artifact. The Authenticode gate (kitty_verify_signature) is the
 * security control here: it is fail-closed (any error rejects), it requires both
 * a valid trust chain (WinVerifyTrust) AND an exact publisher-CN match, and the
 * downloaded file is deleted if it does not pass. */

typedef enum { KITTY_INST_PERUSER, KITTY_INST_SYSTEM, KITTY_INST_PORTABLE } kitty_install_t ;

/* Our MSI UpgradeCodes (stable across versions; see the .wxs / BUILD_PRIVATE.md).
 * MsiEnumRelatedProducts takes the braced GUID form. */
#define KITTY_UPGRADE_SYSTEM  "{69EA2DD5-EF19-4811-B324-EF34CAA6942C}"
#define KITTY_UPGRADE_PERUSER "{578952A6-AA7F-4146-918B-47803234700B}"

static int kitty_msi_installed( const char *upgradecode ) {
	char prodbuf[40] = "" ;   /* a ProductCode GUID is 38 chars + NUL */
	return MsiEnumRelatedProductsA( upgradecode, 0, 0, prodbuf ) == ERROR_SUCCESS ;
}

/* How was this copy installed? Decides which asset to fetch and how to run it.
 * Primary, robust signal: ask Windows Installer whether OUR product (by its
 * stable UpgradeCode) is installed, and which kind — this is independent of the
 * install path, locale, or whether the exe was copied elsewhere. The path sniff
 * is only a fallback. The portable build (MOD_PORTABLE) is always download-only. */
static kitty_install_t kitty_detect_install_type( void ) {
#ifdef MOD_PORTABLE
	return KITTY_INST_PORTABLE ;
#else
	if( kitty_msi_installed( KITTY_UPGRADE_SYSTEM ) )  return KITTY_INST_SYSTEM ;
	if( kitty_msi_installed( KITTY_UPGRADE_PERUSER ) ) return KITTY_INST_PERUSER ;

	/* Fallback: path sniff (older installs / unusual setups). */
	char exe[MAX_PATH]="", env[MAX_PATH]="" ;
	if( GetModuleFileNameA( NULL, exe, sizeof(exe)-1 ) ) {
		if( GetEnvironmentVariableA("ProgramFiles", env, sizeof(env)-1) && env[0]
		    && _strnicmp(exe, env, strlen(env))==0 ) return KITTY_INST_SYSTEM ;
		if( GetEnvironmentVariableA("ProgramW6432", env, sizeof(env)-1) && env[0]
		    && _strnicmp(exe, env, strlen(env))==0 ) return KITTY_INST_SYSTEM ;
		if( GetEnvironmentVariableA("ProgramFiles(x86)", env, sizeof(env)-1) && env[0]
		    && _strnicmp(exe, env, strlen(env))==0 ) return KITTY_INST_SYSTEM ;
		if( GetEnvironmentVariableA("LOCALAPPDATA", env, sizeof(env)-1) && env[0]
		    && _strnicmp(exe, env, strlen(env))==0 ) return KITTY_INST_PERUSER ;
	}
	/* Unknown location (loose exe): treat as portable -> download-only, no auto-run. */
	return KITTY_INST_PORTABLE ;
#endif
}

/* Scan the release JSON for a "browser_download_url" whose value ends with the
 * given suffix (e.g. "-x64-system.msi"). Robust to the exact version string.
 * Copies the URL into out and returns 1; returns 0 if no asset matches. */
static int kitty_find_asset_url( const char *body, const char *suffix, char *out, size_t outsz ) {
	const char *p = body ;
	size_t slen = strlen(suffix) ;
	while( (p = strstr(p, "\"browser_download_url\"")) != NULL ) {
		p += strlen("\"browser_download_url\"") ;
		const char *q = strchr(p, ':') ;
		if( q==NULL ) break ;
		q++ ;
		while( *q==' ' || *q=='\"' ) q++ ;
		char url[1024] ; int j=0 ;
		while( *q && *q!='\"' && j<(int)sizeof(url)-1 ) url[j++]=*q++ ;
		url[j]='\0' ;
		p = q ;
		if( (size_t)j>=slen && _stricmp(url + j - slen, suffix)==0 ) {
			strncpy(out, url, outsz-1) ; out[outsz-1]='\0' ;
			return 1 ;
		}
	}
	return 0 ;
}

/* Download a URL to a local file. Uses generous timeouts (multi-MB installer,
 * not the JSON probe) and follows GitHub's redirect to the CDN. Returns 1 on
 * success; on any failure the partial file is removed. */
static int kitty_download_to_file( const char *url, const char *path ) {
	int ok = 0 ;
	HINTERNET hi = InternetOpenA( "KiTTY-UpdateDownload", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0 ) ;
	if( hi==NULL ) return 0 ;
	DWORD tmo = 30000 ;
	InternetSetOption( hi, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo) ) ;
	InternetSetOption( hi, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo) ) ;
	HINTERNET hu = InternetOpenUrlA( hi, url, NULL, (DWORD)-1,
		INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_SECURE, 0 ) ;
	if( hu!=NULL ) {
		HANDLE hf = CreateFileA( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL ) ;
		if( hf!=INVALID_HANDLE_VALUE ) {
			char buf[16384] ; DWORD nread=0 ; ok=1 ;
			for( ;; ) {
				if( !InternetReadFile( hu, buf, sizeof(buf), &nread ) ) { ok=0 ; break ; }
				if( nread==0 ) break ;
				DWORD nwr=0 ;
				if( !WriteFile( hf, buf, nread, &nwr, NULL ) || nwr!=nread ) { ok=0 ; break ; }
			}
			CloseHandle( hf ) ;
		}
		InternetCloseHandle( hu ) ;
	}
	InternetCloseHandle( hi ) ;
	if( !ok ) DeleteFileA( path ) ;
	return ok ;
}

/* SECURITY GATE. Verify an Authenticode signature on the downloaded installer.
 * Returns 1 only if BOTH hold:
 *   (1) WinVerifyTrust reports a valid trust chain (kills self-signed spoofs);
 *   (2) the signing certificate's subject CN is EXACTLY our publisher (kills a
 *       different-but-valid certificate).
 * Fail-closed: every error path returns 0 (reject). */
static int kitty_verify_signature( const char *path ) {
	wchar_t wpath[MAX_PATH] ;
	if( MultiByteToWideChar( CP_ACP, 0, path, -1, wpath, MAX_PATH ) == 0 ) return 0 ;

	/* (1) Trust chain. */
	WINTRUST_FILE_INFO fi ; memset(&fi,0,sizeof(fi)) ;
	fi.cbStruct = sizeof(fi) ;
	fi.pcwszFilePath = wpath ;
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2 ;
	WINTRUST_DATA wd ; memset(&wd,0,sizeof(wd)) ;
	wd.cbStruct = sizeof(wd) ;
	wd.dwUIChoice = WTD_UI_NONE ;
	wd.fdwRevocationChecks = WTD_REVOKE_NONE ;
	wd.dwUnionChoice = WTD_CHOICE_FILE ;
	wd.pFile = &fi ;
	wd.dwStateAction = WTD_STATEACTION_VERIFY ;
	LONG st = WinVerifyTrust( (HWND)INVALID_HANDLE_VALUE, &action, &wd ) ;
	wd.dwStateAction = WTD_STATEACTION_CLOSE ;
	WinVerifyTrust( (HWND)INVALID_HANDLE_VALUE, &action, &wd ) ;
	if( st != ERROR_SUCCESS ) return 0 ;

	/* (2) Signer CN pin. */
	int matched = 0 ;
	HCERTSTORE hStore = NULL ; HCRYPTMSG hMsg = NULL ;
	if( CryptQueryObject( CERT_QUERY_OBJECT_FILE, wpath,
			CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
			CERT_QUERY_FORMAT_FLAG_BINARY, 0, NULL, NULL, NULL,
			&hStore, &hMsg, NULL ) ) {
		DWORD si_sz = 0 ;
		if( CryptMsgGetParam( hMsg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &si_sz ) && si_sz>0 ) {
			CMSG_SIGNER_INFO *si = (CMSG_SIGNER_INFO*)malloc( si_sz ) ;
			if( si!=NULL && CryptMsgGetParam( hMsg, CMSG_SIGNER_INFO_PARAM, 0, si, &si_sz ) ) {
				CERT_INFO ci ; memset(&ci,0,sizeof(ci)) ;
				ci.Issuer = si->Issuer ;
				ci.SerialNumber = si->SerialNumber ;
				PCCERT_CONTEXT cert = CertFindCertificateInStore( hStore,
					X509_ASN_ENCODING|PKCS_7_ASN_ENCODING, 0,
					CERT_FIND_SUBJECT_CERT, &ci, NULL ) ;
				if( cert!=NULL ) {
					char cn[256]="" ;
					if( CertGetNameStringA( cert, CERT_NAME_ATTR_TYPE, 0,
							szOID_COMMON_NAME, cn, sizeof(cn) ) > 1 ) {
						if( _stricmp( cn, "KAPPER NETWORK-COMMUNICATIONS GmbH" )==0 )
							matched = 1 ;
					}
					CertFreeCertificateContext( cert ) ;
				}
			}
			if( si!=NULL ) free( si ) ;
		}
	}
	if( hMsg!=NULL ) CryptMsgClose( hMsg ) ;
	if( hStore!=NULL ) CertCloseStore( hStore, 0 ) ;
	return matched ;
}

/* Launch the (already verified) MSI. System installs need elevation (runas);
 * per-user installs run unelevated. Restart Manager inside msiexec will close
 * the running kitty.exe to perform the in-place upgrade. */
static void kitty_run_installer( HWND hwnd, kitty_install_t type, const char *path ) {
	char args[MAX_PATH+32] ;
	sprintf( args, "/i \"%s\"", path ) ;
	ShellExecuteA( hwnd, (type==KITTY_INST_SYSTEM) ? "runas" : "open",
		"msiexec.exe", args, NULL, SW_SHOWNORMAL ) ;
}

void CheckVersionFromWebSite( HWND hwnd ) {
	char curnum[64]="" ;
	int i ;

	/* Reduce the build string ("0.84.0.15-beta @ ...") to its numeric prefix. */
	strncpy( curnum, BuildVersionTime, sizeof(curnum)-1 ) ; curnum[sizeof(curnum)-1]='\0' ;
	for( i=0 ; i<(int)strlen(curnum) ; i++ ) {
		if( !(((curnum[i]>='0')&&(curnum[i]<='9'))||(curnum[i]=='.')) ) { curnum[i]='\0' ; break ; }
		}

	/* KiTTY: is THIS build a beta? (stable builds don't silently take betas) */
	int cur_is_beta = ( strstr(BuildVersionTime,"beta")!=NULL
	                 || strstr(BuildVersionTime,"BETA")!=NULL ) ;

	/* Fetch the latest release JSON from GitHub. GitHub requires a User-Agent
	 * (set via InternetOpen); PRECONFIG honours the system/IE proxy settings. */
	char *body = NULL ; DWORD bodylen = 0 ; int ok = 0 ;
	HINTERNET hi = InternetOpenA( "KiTTY-UpdateCheck",
	                              INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0 ) ;
	if( hi != NULL ) {
		/* Bound the synchronous request so a blackholed network falls back to
		 * the browser in seconds instead of freezing the UI on the default timeout. */
		DWORD tmo = 8000 ;
		InternetSetOption( hi, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo) ) ;
		InternetSetOption( hi, INTERNET_OPTION_SEND_TIMEOUT,    &tmo, sizeof(tmo) ) ;
		InternetSetOption( hi, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo) ) ;
		HINTERNET hu = InternetOpenUrlA( hi, KITTY_RELEASES_API,
		                                 "Accept: application/vnd.github+json\r\n",
		                                 (DWORD)-1,
		                                 INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_SECURE,
		                                 0 ) ;
		if( hu != NULL ) {
			DWORD cap = 65536 ; body = (char*)malloc( cap ) ; bodylen = 0 ;
			if( body != NULL ) {
				DWORD nread = 0 ;
				for( ;; ) {
					if( cap - bodylen < 4096 ) {
						char *nb = (char*)realloc( body, cap*2 ) ;
						if( nb==NULL ) break ; body = nb ; cap *= 2 ;
						}
					if( !InternetReadFile( hu, body+bodylen, cap-bodylen-1, &nread ) || nread==0 ) break ;
					bodylen += nread ;
					}
				body[bodylen] = '\0' ;
				ok = (bodylen>0) ;
				}
			InternetCloseHandle( hu ) ;
			}
		InternetCloseHandle( hi ) ;
		}

	/* Extract "tag_name":"kitty-0.84.0.16-beta" and compare. */
	if( ok && (body!=NULL) ) {
		char *p = strstr( body, "\"tag_name\"" ) ;
		char latestnum[64]="" ;
		int latest_is_beta = 0 ;
		if( p != NULL ) {
			p = strchr( p, ':' ) ; if( p!=NULL ) p++ ;
			while( (p!=NULL) && (*p==' '||*p=='\"') ) p++ ;
			char tag[128]="" ; int j=0 ;
			while( (p!=NULL) && *p && (*p!='\"') && (j<(int)sizeof(tag)-1) ) { tag[j++]=*p++ ; }
			tag[j]='\0' ;
			latest_is_beta = ( strstr(tag,"beta")!=NULL || strstr(tag,"BETA")!=NULL ) ;
			/* tag is e.g. "kitty-0.84.0.16-beta": skip to the first digit, keep digits/dots. */
			char *d = tag ; while( *d && !((*d>='0')&&(*d<='9')) ) d++ ;
			int k=0 ; while( *d && (((*d>='0')&&(*d<='9'))||(*d=='.')) && (k<(int)sizeof(latestnum)-1) ) { latestnum[k++]=*d++ ; }
			latestnum[k]='\0' ;
			}
		if( latestnum[0] ) {
			int cv[4], lv[4] ; char msg[512] ;
			kitty_parse_version( curnum, cv ) ;
			kitty_parse_version( latestnum, lv ) ;
			if( kitty_version_cmp( cv, lv ) < 0 ) {
				/* KiTTY: a stable build must not silently take a beta update. If we
				 * are on a stable release and the newest build is a beta, warn and
				 * require explicit opt-in (default No). Beta builds proceed as usual. */
				if( !cur_is_beta && latest_is_beta ) {
					char wmsg[512] ;
					sprintf( wmsg, "You are running a stable release.\n\n"
						"Installed: %s\nLatest:    %s  (BETA)\n\n"
						"The newest available build is a BETA, which may be less "
						"tested than a stable release. Install this beta anyway? "
						"(Proceed with caution.)", curnum, latestnum ) ;
					if( MessageBox( hwnd, wmsg, "KiTTY Update - beta available",
							MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2 )!=IDYES ) {
						free( body ) ; body = NULL ;
						return ;
					}
				}
				/* An update is available. Decide how to deliver it by install type. */
				kitty_install_t itype = kitty_detect_install_type() ;
				char asseturl[1024]="" ; int haveasset = 0 ;
				if( itype != KITTY_INST_PORTABLE ) {
					const char *suffix = (itype==KITTY_INST_SYSTEM)
						? "-x64-system.msi" : "-x64-peruser.msi" ;
					haveasset = kitty_find_asset_url( body, suffix, asseturl, sizeof(asseturl) ) ;
				}
				free( body ) ; body = NULL ;   /* done with JSON before the large download */

				/* Refuse a non-HTTPS asset URL (defence-in-depth: the JSON is already
				 * fetched over TLS, but never auto-download+run over plain http). */
				if( haveasset && strncmp( asseturl, "https://", 8 )!=0 ) haveasset = 0 ;

				if( itype==KITTY_INST_PORTABLE || !haveasset ) {
					/* Portable copy, or no matching installer asset: just offer the page. */
					sprintf( msg, "An update is available.\n\nInstalled: %s\nLatest:    %s\n\n%s",
						curnum, latestnum,
						(itype==KITTY_INST_PORTABLE)
						  ? "This is a portable copy, so auto-install is disabled. Open the download page now?"
						  : "The matching installer could not be located automatically. Open the download page now?" ) ;
					if( MessageBox( hwnd, msg, "KiTTY Update", MB_YESNO|MB_ICONINFORMATION )==IDYES )
						ShellExecute( hwnd, "open", KITTY_RELEASES_URL, 0, 0, SW_SHOWDEFAULT ) ;
					return ;
				}

				/* MSI install: confirm, download, verify the signature, then run. */
				sprintf( msg, "An update is available.\n\nInstalled: %s\nLatest:    %s\n\n"
					"Download and install it now?\n\n"
					"KiTTY will close and any active sessions will be disconnected during "
					"the upgrade. The installer's signature is verified before it runs.",
					curnum, latestnum ) ;
				if( MessageBox( hwnd, msg, "KiTTY Update",
						MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 )!=IDYES )
					return ;

				char tmpdir[MAX_PATH]="", tmpfile[MAX_PATH]="" ;
				GetTempPathA( sizeof(tmpdir), tmpdir ) ;
				const char *base = strrchr( asseturl, '/' ) ;
				base = base ? base+1 : "KiTTY-update.msi" ;
				snprintf( tmpfile, sizeof(tmpfile), "%s%s", tmpdir, base ) ;

				HCURSOR oldc = SetCursor( LoadCursor(NULL, IDC_WAIT) ) ;
				int dok = kitty_download_to_file( asseturl, tmpfile ) ;
				SetCursor( oldc ) ;
				if( !dok ) {
					MessageBox( hwnd, "Download failed. Opening the download page instead.",
						"KiTTY Update", MB_OK|MB_ICONERROR ) ;
					ShellExecute( hwnd, "open", KITTY_RELEASES_URL, 0, 0, SW_SHOWDEFAULT ) ;
					return ;
				}
				/* TOCTOU guard: hold the downloaded file open denying write/delete
				 * (FILE_SHARE_READ only) for the rest of the flow, so it cannot be
				 * swapped between signature verification and the (possibly elevated)
				 * launch. WinVerifyTrust and msiexec can still READ it. Kept open
				 * across the launch on purpose (released when KiTTY exits / the
				 * upgrade restarts it) so the verified bytes stay immutable while
				 * msiexec opens them. A swap in the tiny download->lock gap is caught
				 * by the verify below, which runs on the now-locked file. */
				HANDLE updguard = CreateFileA( tmpfile, GENERIC_READ, FILE_SHARE_READ,
					NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) ;
				if( updguard == INVALID_HANDLE_VALUE ) {
					DeleteFileA( tmpfile ) ;
					MessageBox( hwnd, "Could not secure the downloaded installer; aborting the update.",
						"KiTTY Update", MB_OK|MB_ICONERROR ) ;
					return ;
				}
				/* SECURITY GATE: reject anything not genuinely KAPPER-signed. */
				if( !kitty_verify_signature( tmpfile ) ) {
					CloseHandle( updguard ) ;
					DeleteFileA( tmpfile ) ;
					MessageBox( hwnd, "The downloaded installer FAILED signature verification "
						"and was NOT run; it has been deleted.\n\nPlease install KiTTY only "
						"from the official release page.",
						"KiTTY Update - signature rejected", MB_OK|MB_ICONERROR ) ;
					return ;
				}
				kitty_run_installer( hwnd, itype, tmpfile ) ;
				/* deliberately do NOT CloseHandle(updguard) here: keep the verified
				 * bytes locked against modification while msiexec reads them. */
				return ;
			} else {
				sprintf( msg, "You are running the latest version.\n\nInstalled: %s\nLatest:    %s", curnum, latestnum ) ;
				MessageBox( hwnd, msg, "KiTTY Update", MB_OK|MB_ICONINFORMATION ) ;
			}
			free( body ) ;
			return ;
			}
		}

	/* Fallback (offline / proxy / TLS / parse failure): open the releases page. */
	if( body!=NULL ) free( body ) ;
	ShellExecute( hwnd, "open", KITTY_RELEASES_URL, 0, 0, SW_SHOWDEFAULT ) ;
}

// Affichage d'un message dans l'event log
void debug_logevent( const char *fmt, ... ) {
	va_list ap;
	char *buf;
	va_start(ap, fmt);
	buf = dupvprintf(fmt, ap) ;
	va_end(ap);
	do_eventlog(buf) ;
	free(buf);
}

// Test si un chemin est absolu
bool IsPathAbsolute( const char * path ) {
	bool test = false ;
	if( path == NULL ) { return false ; }
	if( strlen( path ) < 3 ) { return false ; }
	if( ((path[0]>='a') && (path[0]<='z')) || ((path[0]>='A') && (path[0]<='Z')) ) 
		if( path[1]==':' )
			if( (path[2]=='/') || (path[2]=='\\') ) test = true ;
	return test ;
}

void PopUpSystemMenu( HWND hwnd, int npos ) {
	RECT rc ;
	GetWindowRect( hwnd, &rc ) ;
	HMENU m = GetSystemMenu( hwnd, FALSE) ;
	TrackPopupMenu( m, 0, rc.left, rc.top, 0, hwnd, NULL) ;

	if( npos>0 ) {
	int nb = GetMenuItemCount(m), i;
	MENUITEMINFO mi ;
	mi.cbSize = sizeof(MENUITEMINFO) ;
	for( i=0; i<nb; i++ ) {
		mi.dwTypeData  = NULL ;
		GetMenuItemInfoA( m, i, TRUE, &mi);
		char *txt = (char*)malloc(mi.cch+1);
		mi.dwTypeData  = txt ;
		mi.cch=	mi.cch+1;
		GetMenuItemInfoA( m, i, FALSE, &mi);
		MessageBox(NULL,txt,"info",MB_OK);
		free(txt);
	}
	}

}

/* KiTTY auto-login password consent. Shown the first time the user sets an
 * auto-login password in the configuration dialog (NOT at login time, so the
 * auto-login the user configured is never interrupted). Returns nonzero if the
 * user agrees to store the (reversibly-encrypted) password. */
int kitty_autopw_warn( void ) {
	int r = MessageBox( NULL,
		"You are setting a KiTTY auto-login password.\r\n\r\n"
		"SECURITY: this password is saved in your session settings in a "
		"REVERSIBLY-ENCRYPTED form. Anyone with access to this machine or to "
		"your saved configuration can recover the plain-text password.\r\n\r\n"
		"SSH public-key authentication is significantly more secure and is the "
		"recommended way to log in automatically. Use a stored password only for "
		"legacy hosts (such as network devices) that genuinely cannot accept key "
		"authentication.\r\n\r\n"
		"Store this auto-login password?",
		"KiTTY auto-login password",
		MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 ) ;
	return (r == IDYES) ;
}

// Description:
//   Creates a tooltip for an item in a dialog box. 
// Parameters:
//   idTool - identifier of an dialog box item.
//   nDlg - window handle of the dialog box.
//   pszText - string to use as the tooltip text.
// Returns:
//   The handle to the tooltip.
//
HWND CreateToolTip(int toolID, HWND hDlg, PTSTR pszText)
{
    if (!toolID || !hDlg || !pszText)
    {
        return FALSE;
    }
    // Get the window of the tool.
    HWND hwndTool = GetDlgItem(hDlg, toolID);
    
    // Create the tooltip. g_hInst is the global instance handle.
    HWND hwndTip = CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
                              WS_POPUP |TTS_ALWAYSTIP | TTS_BALLOON,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              hDlg, NULL, 
                              hinst /*g_hInst*/, NULL);
    
   if (!hwndTool || !hwndTip)
   {
       return (HWND)NULL;
   }                              
                              
    // Associate the tooltip with the tool.
    TOOLINFO toolInfo = { 0 };
    toolInfo.cbSize = sizeof(toolInfo);
    toolInfo.hwnd = hDlg;
    toolInfo.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    toolInfo.uId = (UINT_PTR)hwndTool;
    toolInfo.lpszText = pszText;
    SendMessage(hwndTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);

    return hwndTip;
}
/*
HWND CreateToolTip2(int toolID, HWND hDlg, PTSTR pszText) {
    HWND hwndToolTips = CreateWindow(TOOLTIPS_CLASS, NULL, 
                            WS_POPUP | TTS_NOPREFIX | TTS_BALLOON, 
                            0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (hwndToolTips)
{
    TOOLINFO ti;

    ti.cbSize   = sizeof(ti);
    ti.uFlags   = TTF_TRANSPARENT | TTF_CENTERTIP;
    ti.hwnd     = hDlg;
    ti.uId      = toolID;
    ti.hinst    = NULL;
    ti.lpszText = pszText;

    GetClientRect(hwnd, &ti.rect);

    SendMessage(hwndToolTips, TTM_ADDTOOL, 0, (LPARAM) &ti );

}
return hwndToolTips ;
}
*/
