#include "kitty_win.h"
#include "kitty_authenticode.h"   /* shared Authenticode trust + CN gate */
#include "kitty_notice.h"          /* near-the-clock warning window */
#include "kitty_rc_additions.h"   /* IDD_UPDATEBOX, IDC_UPD_TEXT, IDC_UPD_UPDATE */
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
	char szFilter[4096] ; snprintf( szFilter, sizeof(szFilter), "%s", Filter ) ;
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
	char szFilter[4096] ; snprintf( szFilter, sizeof(szFilter), "%s", Filter ) ;
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
#include <shobjidl.h>   /* IFileOpenDialog (Common Item Dialog folder picker) */
int OpenDirName( HWND hFrame, char * dirname ) {
	dirname[0] = '\0' ;
	/* Modern Common Item Dialog folder picker (Vista+): the full Explorer window
	 * with an address bar you can paste a path into, type-ahead and favourites -
	 * not the old tree-only SHBrowseForFolder. Falls back to the tree picker (with
	 * a New Folder button) if COM or the dialog is unavailable. */
	static const GUID clsid_fod = {0xDC1C5A9C,0xE88A,0x4dde,{0xA5,0xA1,0x60,0xF8,0x2A,0x20,0xAE,0xF7}} ;
	static const GUID iid_fod   = {0xd57c7288,0xd4ad,0x4768,{0xbe,0x02,0x9d,0x96,0x95,0x32,0xd9,0x60}} ;
	IFileOpenDialog *pfd = NULL ;
	HRESULT hrInit = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE ) ;
	if( SUCCEEDED( CoCreateInstance( &clsid_fod, NULL, CLSCTX_INPROC_SERVER,
	                                 &iid_fod, (void**)&pfd ) ) && pfd ) {
		DWORD opts = 0 ;
		pfd->lpVtbl->GetOptions( pfd, &opts ) ;
		pfd->lpVtbl->SetOptions( pfd, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST ) ;
		pfd->lpVtbl->SetTitle( pfd, L"Select a folder..." ) ;
		if( SUCCEEDED( pfd->lpVtbl->Show( pfd, hFrame ) ) ) {
			IShellItem *psi = NULL ;
			if( SUCCEEDED( pfd->lpVtbl->GetResult( pfd, &psi ) ) && psi ) {
				PWSTR wpath = NULL ;
				if( SUCCEEDED( psi->lpVtbl->GetDisplayName( psi, SIGDN_FILESYSPATH, &wpath ) ) && wpath ) {
					WideCharToMultiByte( CP_ACP, 0, wpath, -1, dirname, 4096, NULL, NULL ) ;
					CoTaskMemFree( wpath ) ;
				}
				psi->lpVtbl->Release( psi ) ;
			}
		}
		pfd->lpVtbl->Release( pfd ) ;
		if( SUCCEEDED( hrInit ) ) CoUninitialize() ;
		return dirname[0] ? 1 : 0 ;
	}
	if( SUCCEEDED( hrInit ) ) CoUninitialize() ;

	/* Fallback: classic tree picker (with the New Folder button). */
	{
		BROWSEINFO bi ; LPITEMIDLIST il ; char Buffer[4096], Result[4096] = "" ;
		memset( &bi, 0, sizeof(bi) ) ;
		bi.hwndOwner = hFrame ;
		bi.pszDisplayName = Buffer ;
		bi.lpszTitle = "Select a folder..." ;
		bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE ;
		if( (il = SHBrowseForFolder( &bi )) != NULL ) {
			SHGetPathFromIDList( il, Result ) ;
			GlobalFree( il ) ;
			if( Result[0] ) { strcpy( dirname, Result ) ; return 1 ; }
		}
	}
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
	char module[MAX_PATH+1]="", cmd[4096]="" ;
	/* Do not rely on GetShortPathName(): 8.3 short names can be disabled on
	 * modern Windows volumes, which made Shift+F2/Ctrl+Shift+F2 silently do
	 * nothing. Quote the real module path instead. */
	if( GetModuleFileName( NULL, (LPTSTR)module, MAX_PATH ) ) {
		snprintf( cmd, sizeof(cmd), "\"%s\" -ed", module );
		if( filename!=NULL ) if( strlen(filename)>0 ) {
			strncat( cmd, "b ", sizeof(cmd)-strlen(cmd)-1 ) ;
			strncat( cmd, filename, sizeof(cmd)-strlen(cmd)-1 ) ;
		}
		debug_logevent( cmd ) ;
		RunCommand( hwnd, cmd ) ;
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

/* Our MSI UpgradeCodes, stable across versions - they must match the ones in
 * windows/installer/*.wxs, which is where they are defined.
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
		/* CREATE_NEW avoids clobbering or following an attacker-precreated file in
		 * %TEMP%. The caller supplies a fresh random-ish path and failure deletes
		 * partial output. */
		HANDLE hf = CreateFileA( path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL ) ;
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

/* SECURITY GATE for the downloaded installer: a valid Authenticode trust
 * chain AND an exact publisher-CN match. The implementation now lives in
 * kitty_authenticode.c so the kageant "New key" launcher shares the exact
 * same gate - see kitty_authenticode.h. The installer is a NEWER version than
 * the running kitty, so only trust+CN is checked here, never a version match. */

/* Launch the (already verified) MSI. System installs need elevation (runas);
 * per-user installs run unelevated. Restart Manager inside msiexec will close
 * the running kitty.exe to perform the in-place upgrade. */
static int kitty_run_installer( HWND hwnd, kitty_install_t type, const char *path ) {
	char args[MAX_PATH+32] ;
	snprintf( args, sizeof(args), "/i \"%s\"", path ) ;
	HINSTANCE r = ShellExecuteA( hwnd, (type==KITTY_INST_SYSTEM) ? "runas" : "open",
		"msiexec.exe", args, NULL, SW_SHOWNORMAL ) ;
	return ((INT_PTR)r > 32) ;
}

/* ---- KiTTY: background "update available" check (cached; shown at session start) ----
 * The blocking GitHub query runs on a worker thread and ONLY refreshes a cached
 * "latest version" in the registry. The in-terminal notice is rendered later,
 * synchronously, at the clean top of a session (window.c) -- never injected
 * mid-session, which would corrupt a full-screen TUI. So the notice can be at
 * most one launch behind for a brand-new release, which is fine for a nudge. */
extern const char *kitty_registry_base( void ) ;
extern int kitty_portable_store_state_string(const char *key, const char *value);
extern int kitty_portable_load_state_string(const char *key, char *buf, int buflen);
extern int kitty_portable_store_state_dword(const char *key, DWORD value);
extern int kitty_portable_load_state_dword(const char *key, DWORD *value);

/* Fetch the newest release's numeric version + prerelease flag from GitHub.
 * Returns 1 on success. Leaner sibling of CheckVersionFromWebSite's fetch
 * (no asset URLs needed). */
static int kitty_fetch_latest_version( char *ver, int verlen, int *is_beta ) {
	char *body = NULL ; DWORD bodylen = 0 ; int ok = 0 ;
	HINTERNET hi = InternetOpenA( "KiTTY-UpdateCheck",
	                              INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0 ) ;
	if( hi == NULL ) return 0 ;
	DWORD tmo = 8000 ;
	InternetSetOption( hi, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo) ) ;
	InternetSetOption( hi, INTERNET_OPTION_SEND_TIMEOUT,    &tmo, sizeof(tmo) ) ;
	InternetSetOption( hi, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo) ) ;
	HINTERNET hu = InternetOpenUrlA( hi, KITTY_RELEASES_API,
	                                 "Accept: application/vnd.github+json\r\n", (DWORD)-1,
	                                 INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_SECURE, 0 ) ;
	if( hu != NULL ) {
		DWORD cap = 65536 ; body = (char*)malloc( cap ) ; bodylen = 0 ;
		if( body != NULL ) {
			DWORD nread = 0 ;
			for( ;; ) {
				if( cap - bodylen < 4096 ) { char *nb=(char*)realloc(body,cap*2); if(nb==NULL) break; body=nb; cap*=2; }
				if( !InternetReadFile( hu, body+bodylen, cap-bodylen-1, &nread ) || nread==0 ) break ;
				bodylen += nread ;
				}
			body[bodylen] = '\0' ; ok = (bodylen>0) ;
			}
		InternetCloseHandle( hu ) ;
		}
	InternetCloseHandle( hi ) ;

	int got = 0 ;
	if( ok && body!=NULL ) {
		if( is_beta != NULL ) {
			char *pr = strstr( body, "\"prerelease\"" ) ; *is_beta = 0 ;
			if( pr!=NULL ) { pr=strchr(pr,':'); if(pr!=NULL) pr++; while(pr!=NULL&&(*pr==' '||*pr=='\t'))pr++;
				*is_beta = ( pr!=NULL && strncmp(pr,"true",4)==0 ) ; }
			}
		char *p = strstr( body, "\"tag_name\"" ) ;
		if( p!=NULL ) {
			p=strchr(p,':'); if(p!=NULL)p++; while(p!=NULL&&(*p==' '||*p=='\"'))p++;
			char tag[128]=""; int j=0; while(p!=NULL&&*p&&(*p!='\"')&&(j<(int)sizeof(tag)-1)){tag[j++]=*p++;} tag[j]='\0';
			char *d=tag; while(*d&&!((*d>='0')&&(*d<='9')))d++;
			int k=0; while(*d&&(((*d>='0')&&(*d<='9'))||(*d=='.'))&&(k<verlen-1)){ver[k++]=*d++;} ver[k]='\0';
			got = (ver[0]!='\0') ;
			}
		}
	if( body!=NULL ) free( body ) ;
	return got ;
}

struct kitty_update_notify {
	HWND hwnd ;
	UINT msg ;
} ;

static DWORD WINAPI kitty_update_worker( LPVOID param ) {
	struct kitty_update_notify *notify = (struct kitty_update_notify *)param ;
	char ver[64]="" ; int is_beta=0 ;
	if( kitty_fetch_latest_version( ver, sizeof(ver), &is_beta ) ) {
		DWORD b = is_beta ? 1 : 0 ;
		if( !kitty_portable_store_state_string( "UpdateLatest", ver ) ||
		    !kitty_portable_store_state_dword( "UpdateLatestBeta", b ) ) {
			HKEY hk ; char base[512] ;
			snprintf( base, sizeof(base), "%s", kitty_registry_base() ) ;
			if( RegCreateKeyExA( HKEY_CURRENT_USER, base, 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL ) == ERROR_SUCCESS ) {
				RegSetValueExA( hk, "UpdateLatest", 0, REG_SZ, (const BYTE*)ver, (DWORD)strlen(ver)+1 ) ;
				RegSetValueExA( hk, "UpdateLatestBeta", 0, REG_DWORD, (const BYTE*)&b, sizeof(b) ) ;
				RegCloseKey( hk ) ;
				}
			}
		}
	if( notify != NULL ) {
		if( notify->hwnd != NULL && notify->msg != 0 )
			PostMessage( notify->hwnd, notify->msg, 0, 0 ) ;
		free( notify ) ;
		}
	return 0 ;
}

/* Launch the background update check once per process (fire-and-forget). */
void kitty_start_update_check( void ) {
	static int started = 0 ;
	if( started ) return ; started = 1 ;
	HANDLE th = CreateThread( NULL, 0, kitty_update_worker, NULL, 0, NULL ) ;
	if( th != NULL ) CloseHandle( th ) ;
}

/* Launcher variant: notify a window after the async cache refresh, so the tray
 * balloon can appear on the first launcher run after a new release instead of
 * only after a previous process has already populated the cache. */
void kitty_start_update_check_notify( HWND hwnd, UINT msg ) {
	static int started = 0 ;
	struct kitty_update_notify *notify ;
	if( started ) return ; started = 1 ;
	notify = (struct kitty_update_notify *)malloc( sizeof(*notify) ) ;
	if( notify == NULL ) return ;
	notify->hwnd = hwnd ;
	notify->msg = msg ;
	HANDLE th = CreateThread( NULL, 0, kitty_update_worker, notify, 0, NULL ) ;
	if( th != NULL ) CloseHandle( th ) ;
	else free( notify ) ;
}

/* If the cached latest version is newer than this build and the channel rule
 * allows surfacing it, fill buf with a one-line ASCII notice and return 1. */
/* Shared "is a newer build available?" check, used by the terminal notice and
 * the launcher tray balloon. Reads the cached latest version (refreshed async by
 * the worker), applies the channel rule (a stable build ignores betas), and on a
 * positive result fills the caller's buffers. Returns 1 if an update should be
 * surfaced, else 0. Any out pointer may be NULL. */
int kitty_update_available( char *latest_out, int latest_n,
                            char *cur_out, int cur_n, int *beta_out ) {
	char curnum[64]="" ; int i ;
	strncpy( curnum, BuildVersionTime, sizeof(curnum)-1 ) ; curnum[sizeof(curnum)-1]='\0' ;
	for( i=0 ; i<(int)strlen(curnum) ; i++ )
		if( !(((curnum[i]>='0')&&(curnum[i]<='9'))||(curnum[i]=='.')) ) { curnum[i]='\0'; break; }
	/* This build's channel: BUILD_VERSION carries no "-beta" suffix, so detect
	 * from the version scheme (KiTTY stable = x.y.M.0, beta = x.y.M.P, P>0); also
	 * honour an explicit "beta" in the build string if one is ever added. */
	int cur_is_beta ;
	{ int cvb[4] ; kitty_parse_version( curnum, cvb ) ;
	  cur_is_beta = ( cvb[3] != 0 ) || ( strstr(BuildVersionTime,"beta")!=NULL )
	                                || ( strstr(BuildVersionTime,"BETA")!=NULL ) ; }

	char base[512], latest[64]="" ; DWORD sz=sizeof(latest), beta=0, bsz=sizeof(beta) ;
	if( !kitty_portable_load_state_string( "UpdateLatest", latest, sizeof(latest) ) ) {
		snprintf( base, sizeof(base), "%s", kitty_registry_base() ) ;
		if( RegGetValueA( HKEY_CURRENT_USER, base, "UpdateLatest", RRF_RT_REG_SZ, NULL, latest, &sz ) != ERROR_SUCCESS ) return 0 ;
		RegGetValueA( HKEY_CURRENT_USER, base, "UpdateLatestBeta", RRF_RT_REG_DWORD, NULL, &beta, &bsz ) ;
	} else {
		kitty_portable_load_state_dword( "UpdateLatestBeta", &beta ) ;
	}
	if( latest[0]=='\0' ) return 0 ;

	int cv[4], lv[4] ;
	kitty_parse_version( curnum, cv ) ;
	kitty_parse_version( latest, lv ) ;
	if( kitty_version_cmp( cv, lv ) >= 0 ) return 0 ;   /* not newer */
	if( !cur_is_beta && beta ) return 0 ;               /* stable build ignores betas */

	if( latest_out && latest_n>0 ) { strncpy( latest_out, latest, latest_n-1 ) ; latest_out[latest_n-1]='\0' ; }
	if( cur_out && cur_n>0 ) { strncpy( cur_out, curnum, cur_n-1 ) ; cur_out[cur_n-1]='\0' ; }
	if( beta_out ) *beta_out = (int)beta ;
	return 1 ;
}

int kitty_update_notice( char *buf, int n ) {
	char latest[64]="", curnum[64]="" ; int beta=0 ;
	if( !kitty_update_available( latest, sizeof(latest), curnum, sizeof(curnum), &beta ) ) return 0 ;
	/* UTF-8 source text (incl. a real "->" arrow); window.c renders it via
	 * term_data_wide(), which encodes to the terminal's charset (no mojibake). */
	snprintf( buf, n,
		"\r\n[KiTTY] An update is available: %s (you have %s)%s.\r\n"
		"        System menu \xe2\x86\x92 Check for updates to install it.\r\n\r\n",
		latest, curnum, beta ? " (beta)" : "" ) ;
	return 1 ;
}

/* ============================================================================
 * KiTTY: non-modal, self-dismissing "update" popup + a transient title notice.
 * Replaces the modal, sound-playing MessageBox result boxes for the
 * update-available / no-update cases. Error boxes (download/signature failures)
 * stay modal WITH their sound on purpose - a failed install must not slip by.
 * ==========================================================================*/
#define KUP_ACT_NONE       0
#define KUP_ACT_OPENPAGE   1
#define KUP_ACT_MSI        2
/* Button IDs come from the IDD_UPDATEBOX template: IDC_UPD_UPDATE ("Update
 * now"), IDCANCEL ("Later"), IDOK ("OK"). */
#define KUP_TIMER_ID       1
#define KUP_AUTODISMISS_MS 5000

typedef struct {
	HWND  owner ;
	int   action ;
	kitty_install_t itype ;
	char  asseturl[1024] ;
	char  notesurl[512] ; /* KiTTY: that release's GitHub page; "" => no button */
	char  text[2048] ;    /* message; the dialog is grown to fit it */
	int   has_update ;    /* update available (Update now/Later) vs info (OK) */
} kitty_upd_ctx ;

/* Download+verify+install the MSI (unchanged flow, just factored out so the
 * non-modal popup's "Update" button can call it). Keeps its modal MB_ICONERROR
 * boxes (with the system sound) so failures are impossible to miss. */
static void kitty_do_msi_update( HWND owner, const char *asseturl, kitty_install_t itype ) {
	char tmpdir[MAX_PATH]="", tmpbase[MAX_PATH]="", tmpfile[MAX_PATH]="" ;
	if( !GetTempPathA( sizeof(tmpdir), tmpdir ) ||
	    !GetTempFileNameA( tmpdir, "kty", 0, tmpbase ) ) {
		MessageBox( owner, "Could not create a temporary installer path; aborting the update.",
			"KiTTY Update", MB_OK|MB_ICONERROR ) ;
		return ;
	}
	DeleteFileA( tmpbase ) ;
	snprintf( tmpfile, sizeof(tmpfile), "%s.msi", tmpbase ) ;

	HCURSOR oldc = SetCursor( LoadCursor(NULL, IDC_WAIT) ) ;
	int dok = kitty_download_to_file( asseturl, tmpfile ) ;
	SetCursor( oldc ) ;
	if( !dok ) {
		MessageBox( owner, "Download failed. Opening the download page instead.",
			"KiTTY Update", MB_OK|MB_ICONERROR ) ;
		ShellExecute( owner, "open", KITTY_RELEASES_URL, 0, 0, SW_SHOWDEFAULT ) ;
		return ;
	}
	/* TOCTOU guard: hold the downloaded file open denying write/delete for the
	 * rest of the flow so it cannot be swapped between verify and launch. */
	HANDLE updguard = CreateFileA( tmpfile, GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) ;
	if( updguard == INVALID_HANDLE_VALUE ) {
		DeleteFileA( tmpfile ) ;
		MessageBox( owner, "Could not secure the downloaded installer; aborting the update.",
			"KiTTY Update", MB_OK|MB_ICONERROR ) ;
		return ;
	}
	/* SECURITY GATE: reject anything not genuinely KAPPER-signed. */
	if( !kitty_authenticode_verify( tmpfile ) ) {
		CloseHandle( updguard ) ;
		DeleteFileA( tmpfile ) ;
		MessageBox( owner, "The downloaded installer FAILED signature verification "
			"and was NOT run; it has been deleted.\n\nPlease install KiTTY only "
			"from the official release page.",
			"KiTTY Update - signature rejected", MB_OK|MB_ICONERROR ) ;
		return ;
	}
	if( !kitty_run_installer( owner, itype, tmpfile ) ) {
		CloseHandle( updguard ) ;
		DeleteFileA( tmpfile ) ;
		MessageBox( owner, "Could not start the verified installer. The downloaded file has been deleted.",
			"KiTTY Update", MB_OK|MB_ICONERROR ) ;
		return ;
	}
	/* keep the verified bytes locked while msiexec reads them (released on exit). */
}

/* Grow the message control + the whole dialog to fit `text` at the dialog's
 * (DPI-correct) font, and slide the buttons down by the same amount. Keeps the
 * "sized to the text" look now that the dialog manager owns the font. */
static void kitty_upd_fit_to_text( HWND h, const char *text ) {
	HWND txt = GetDlgItem( h, IDC_UPD_TEXT ) ;
	HFONT f = (HFONT)SendMessage( h, WM_GETFONT, 0, 0 ) ;
	if( !txt ) return ;
	RECT tr ; GetWindowRect( txt, &tr ) ; MapWindowPoints( NULL, h, (POINT*)&tr, 2 ) ;
	int tw = tr.right - tr.left, cur_th = tr.bottom - tr.top ;
	HDC dc = GetDC( txt ) ; HFONT of = (HFONT)SelectObject( dc, f ) ;
	RECT mr = { 0, 0, tw, 0 } ;
	DrawText( dc, text, -1, &mr, DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX ) ;
	int new_th = mr.bottom ;
	SelectObject( dc, of ) ; ReleaseDC( txt, dc ) ;
	int dh = new_th - cur_th ;
	if( dh == 0 ) return ;
	MoveWindow( txt, tr.left, tr.top, tw, new_th, TRUE ) ;
	int ids[] = { IDOK, IDCANCEL, IDC_UPD_UPDATE, IDC_UPD_NOTES } ;
	for( int i=0 ; i<(int)(sizeof(ids)/sizeof(ids[0])) ; i++ ) {
		HWND b = GetDlgItem( h, ids[i] ) ; if( !b ) continue ;
		RECT br ; GetWindowRect( b, &br ) ; MapWindowPoints( NULL, h, (POINT*)&br, 2 ) ;
		MoveWindow( b, br.left, br.top + dh, br.right-br.left, br.bottom-br.top, TRUE ) ;
	}
	RECT wr ; GetWindowRect( h, &wr ) ;
	SetWindowPos( h, NULL, 0, 0, wr.right-wr.left, (wr.bottom-wr.top)+dh, SWP_NOMOVE|SWP_NOZORDER ) ;
}

static INT_PTR CALLBACK kitty_upd_dlgproc( HWND h, UINT msg, WPARAM wp, LPARAM lp ) {
	kitty_upd_ctx *c = (kitty_upd_ctx*)GetWindowLongPtr( h, DWLP_USER ) ;
	switch( msg ) {
	  case WM_INITDIALOG:
		c = (kitty_upd_ctx*)lp ;
		SetWindowLongPtr( h, DWLP_USER, (LONG_PTR)c ) ;
		SetDlgItemTextA( h, IDC_UPD_TEXT, c ? c->text : "" ) ;
		if( c && c->has_update ) {
			ShowWindow( GetDlgItem( h, IDOK ), SW_HIDE ) ;   /* Update now + Later */
		} else {
			ShowWindow( GetDlgItem( h, IDC_UPD_UPDATE ), SW_HIDE ) ;   /* just OK */
			ShowWindow( GetDlgItem( h, IDCANCEL ), SW_HIDE ) ;
		}
		/* KiTTY: the "View release notes" button only when we have an update
		 * and a release URL to point at. */
		if( !( c && c->has_update && c->notesurl[0] ) )
			ShowWindow( GetDlgItem( h, IDC_UPD_NOTES ), SW_HIDE ) ;
		if( c ) kitty_upd_fit_to_text( h, c->text ) ;
		CenterDlgInParent( h ) ;
		SetWindowPos( h, HWND_TOP, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW ) ;
		SetForegroundWindow( h ) ;
		SetFocus( GetDlgItem( h, (c && c->has_update) ? IDC_UPD_UPDATE : IDOK ) ) ;
		/* Only the info box may dismiss itself; an available update stays until
		 * the user picks Update/Later so the offer can't silently vanish. */
		if( !(c && c->has_update) ) SetTimer( h, KUP_TIMER_ID, KUP_AUTODISMISS_MS, NULL ) ;
		return FALSE ;   /* focus set ourselves */
	  case WM_TIMER:
		if( wp==KUP_TIMER_ID ) DestroyWindow( h ) ;   /* auto-dismiss */
		return TRUE ;
	  case WM_COMMAND:
		if( LOWORD(wp)==IDC_UPD_UPDATE ) {
			KillTimer( h, KUP_TIMER_ID ) ;
			HWND owner = c ? c->owner : NULL ;
			int  action = c ? c->action : KUP_ACT_NONE ;
			kitty_install_t itype = c ? c->itype : KITTY_INST_PORTABLE ;
			char url[1024] ; url[0]='\0' ;
			if( c ) { strncpy(url, c->asseturl, sizeof(url)-1) ; url[sizeof(url)-1]='\0' ; }
			DestroyWindow( h ) ;   /* close the popup, then act */
			if( action==KUP_ACT_OPENPAGE ) ShellExecute( owner, "open", KITTY_RELEASES_URL, 0, 0, SW_SHOWDEFAULT ) ;
			else if( action==KUP_ACT_MSI ) kitty_do_msi_update( owner, url, itype ) ;
			return TRUE ;
		}
		if( LOWORD(wp)==IDC_UPD_NOTES ) {
			/* KiTTY: open the release page in the browser; leave the popup up
			 * so the user can still choose Update now / Later afterwards. */
			if( c && c->notesurl[0] )
				ShellExecute( c->owner, "open", c->notesurl, 0, 0, SW_SHOWDEFAULT ) ;
			return TRUE ;
		}
		if( LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL ) { DestroyWindow( h ) ; return TRUE ; }
		return FALSE ;
	  case WM_CLOSE: DestroyWindow( h ) ; return TRUE ;
	  case WM_NCDESTROY:
		if( c ) { free(c) ; SetWindowLongPtr( h, DWLP_USER, 0 ) ; }
		return FALSE ;
	}
	return FALSE ;
}

/*
 * Generic notice box: a caption, a block of text, and OK.
 *
 * A real dialog rather than MessageBox because it must GROW TO FIT a long
 * explanation instead of clipping it, and because it should look like the rest of
 * KiTTY.
 *
 * ⚠️ NOT for DPI reasons, whatever this comment used to say. A MessageBox is drawn
 * by WINDOWS in the system dialog font, and the system scales it for the process's
 * DPI awareness - it is correct on a scaled display and always was. The DPI
 * problems this project actually had came from windows we laid out OURSELVES with
 * hard-coded pixel sizes. The record is corrected here because that wrong reason
 * was about to be used to justify rewriting a working popup.
 *
 * MODAL, unlike the update popup: this is used to tell somebody that a thing they
 * just did no longer works, and a notice that answers a deliberate action has to
 * be acknowledged rather than time out unread.
 */
typedef struct { const char *caption ; const char *text ; } kitty_notice_t ;

static INT_PTR CALLBACK kitty_notice_dlgproc( HWND h, UINT msg, WPARAM wp, LPARAM lp ) {
	switch( msg ) {
	  case WM_INITDIALOG: {
		const kitty_notice_t *n = (const kitty_notice_t *)lp ;
		const char *text = n ? n->text : NULL ;
		HWND txt = GetDlgItem( h, IDC_NOTICE_TEXT ) ;
		HFONT f = (HFONT)SendMessage( h, WM_GETFONT, 0, 0 ) ;
		if( n && n->caption ) SetWindowTextA( h, n->caption ) ;
		SetDlgItemTextA( h, IDC_NOTICE_TEXT, text ? text : "" ) ;
		if( txt && text ) {
			/* same fit-to-text as the update popup: measure at the DIALOG's font,
			 * grow the control, push the button down, grow the window. */
			RECT tr ; GetWindowRect( txt, &tr ) ;
			MapWindowPoints( NULL, h, (POINT*)&tr, 2 ) ;
			int tw = tr.right - tr.left, cur_th = tr.bottom - tr.top ;
			HDC dc = GetDC( txt ) ; HFONT of = (HFONT)SelectObject( dc, f ) ;
			RECT mr = { 0, 0, tw, 0 } ;
			DrawText( dc, text, -1, &mr, DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX ) ;
			int dh = mr.bottom - cur_th ;
			SelectObject( dc, of ) ; ReleaseDC( txt, dc ) ;
			if( dh != 0 ) {
				HWND b = GetDlgItem( h, IDOK ) ;
				MoveWindow( txt, tr.left, tr.top, tw, mr.bottom, TRUE ) ;
				if( b ) {
					RECT br ; GetWindowRect( b, &br ) ;
					MapWindowPoints( NULL, h, (POINT*)&br, 2 ) ;
					MoveWindow( b, br.left, br.top + dh,
						br.right-br.left, br.bottom-br.top, TRUE ) ;
				}
				RECT wr ; GetWindowRect( h, &wr ) ;
				SetWindowPos( h, NULL, 0, 0, wr.right-wr.left,
					(wr.bottom-wr.top)+dh, SWP_NOMOVE|SWP_NOZORDER ) ;
			}
		}
		return TRUE ;
	  }
	  case WM_COMMAND:
		if( LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL ) { EndDialog( h, 0 ) ; return TRUE ; }
		return FALSE ;
	  case WM_CLOSE: EndDialog( h, 0 ) ; return TRUE ;
	}
	return FALSE ;
}

void kitty_notice_box( HWND owner, const char *caption, const char *text ) {
	kitty_notice_t n ;
	n.caption = caption ; n.text = text ;
	DialogBoxParamA( GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_NOTICEBOX),
		owner, kitty_notice_dlgproc, (LPARAM)&n ) ;
}

/*
 * Yes/No confirmation with an optional second line in RED.
 *
 * Two things a MessageBox cannot do, which is the whole reason this exists - and
 * NEITHER of them is DPI, since a MessageBox is drawn by Windows and scales
 * correctly: it grows to fit a long explanation, and it can colour text. The red
 * line is reserved for "this will persist even if you never press Save", which
 * the ordinary wording of a confirmation understates.
 *
 * "No" is the default: this is asked precisely when something is about to be
 * overwritten, so pressing Return without reading must not agree to it.
 */
typedef struct {
	const char *caption ;
	const char *text ;
	const char *warn ;   /* NULL/"" = no red line, and that row collapses */
} kitty_confirm_t ;

/* Grow one text control to fit its text at the DIALOG's font, offset by extra_dy,
 * and return the height change in pixels. Same measure-then-move approach the
 * notice box uses, so neither hand-rolls DPI scaling. */
static int kitty_fit_text( HWND dlg, int ctlid, const char *text, int extra_dy ) {
	HWND c = GetDlgItem( dlg, ctlid ) ;
	HFONT f = (HFONT)SendMessage( dlg, WM_GETFONT, 0, 0 ) ;
	RECT r ; int w, cur, dh = 0 ;
	if( !c ) return 0 ;
	GetWindowRect( c, &r ) ; MapWindowPoints( NULL, dlg, (POINT*)&r, 2 ) ;
	w = r.right - r.left ; cur = r.bottom - r.top ;
	if( text && *text ) {
		HDC dc = GetDC( c ) ; HFONT of = (HFONT)SelectObject( dc, f ) ;
		RECT m = { 0, 0, w, 0 } ;
		DrawText( dc, text, -1, &m, DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX ) ;
		SelectObject( dc, of ) ; ReleaseDC( c, dc ) ;
		dh = m.bottom - cur ;
		MoveWindow( c, r.left, r.top + extra_dy, w, m.bottom, TRUE ) ;
	} else {
		dh = -cur ;                       /* collapse, do not leave a gap */
		MoveWindow( c, r.left, r.top + extra_dy, w, 0, TRUE ) ;
		ShowWindow( c, SW_HIDE ) ;
	}
	return dh ;
}

static INT_PTR CALLBACK kitty_confirm_dlgproc( HWND h, UINT msg, WPARAM wp, LPARAM lp ) {
	static const kitty_confirm_t *cf = NULL ;
	switch( msg ) {
	  case WM_INITDIALOG: {
		int d1, d2, dh, id ;
		cf = (const kitty_confirm_t *)lp ;
		if( cf && cf->caption ) SetWindowTextA( h, cf->caption ) ;
		SetDlgItemTextA( h, IDC_CONFIRM_TEXT, cf && cf->text ? cf->text : "" ) ;
		SetDlgItemTextA( h, IDC_CONFIRM_WARN, cf && cf->warn ? cf->warn : "" ) ;
		d1 = kitty_fit_text( h, IDC_CONFIRM_TEXT, cf ? cf->text : NULL, 0 ) ;
		d2 = kitty_fit_text( h, IDC_CONFIRM_WARN, cf ? cf->warn : NULL, d1 ) ;
		dh = d1 + d2 ;
		if( dh != 0 ) {
			for( id = IDYES ; ; id = IDNO ) {
				HWND b = GetDlgItem( h, id ) ;
				if( b ) {
					RECT br ; GetWindowRect( b, &br ) ;
					MapWindowPoints( NULL, h, (POINT*)&br, 2 ) ;
					MoveWindow( b, br.left, br.top + dh,
						br.right-br.left, br.bottom-br.top, TRUE ) ;
				}
				if( id == IDNO ) break ;
			}
			{ RECT wr ; GetWindowRect( h, &wr ) ;
			  SetWindowPos( h, NULL, 0, 0, wr.right-wr.left,
				(wr.bottom-wr.top)+dh, SWP_NOMOVE|SWP_NOZORDER ) ; }
		}
		SetFocus( GetDlgItem( h, IDNO ) ) ;
		return FALSE ;                     /* focus set here, not by the manager */
	  }
	  case WM_CTLCOLORSTATIC:
		/* the warning line, and only it, is red */
		if( cf && cf->warn && *cf->warn &&
		    (HWND)lp == GetDlgItem( h, IDC_CONFIRM_WARN ) ) {
			SetTextColor( (HDC)wp, RGB(200,0,0) ) ;
			SetBkMode( (HDC)wp, TRANSPARENT ) ;
			return (INT_PTR)GetSysColorBrush( COLOR_3DFACE ) ;
		}
		return FALSE ;
	  case WM_COMMAND:
		switch( LOWORD(wp) ) {
		  case IDYES: EndDialog( h, 1 ) ; return TRUE ;
		  case IDNO:
		  case IDCANCEL: EndDialog( h, 0 ) ; return TRUE ;
		}
		return FALSE ;
	  case WM_CLOSE: EndDialog( h, 0 ) ; return TRUE ;   /* closing means No */
	}
	return FALSE ;
}

/* True only if Yes was pressed. No, Escape and closing the box all mean no, which
 * is the safe reading of every one of them. */
int kitty_confirm_box( HWND owner, const char *caption, const char *text,
                       const char *warn_red ) {
	kitty_confirm_t cf ;
	cf.caption = caption ; cf.text = text ; cf.warn = warn_red ;
	return DialogBoxParamA( GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_CONFIRMBOX),
		owner, kitty_confirm_dlgproc, (LPARAM)&cf ) == 1 ;
}

/* Show the modeless "update available / up to date" popup over `owner`. It is a
 * real dialog (IDD_UPDATEBOX) so the dialog manager gives it the shell font at
 * the correct DPI, exactly like every other KiTTY window - no hand-rolled DPI
 * scaling. action==KUP_ACT_NONE => info only (single "OK", auto-dismisses in
 * 5s); otherwise "Update now" runs the action and "Later" dismisses, and it
 * stays up until the user decides so the offer can't silently vanish. */
static void kitty_show_update_popup( HWND owner, const char *text, int action,
                                     const char *asseturl, kitty_install_t itype,
                                     const char *notesurl ) {
	kitty_upd_ctx *c = (kitty_upd_ctx*)calloc( 1, sizeof(kitty_upd_ctx) ) ;
	if( !c ) return ;
	c->owner = owner ; c->action = action ; c->itype = itype ;
	c->has_update = ( action != KUP_ACT_NONE ) ;
	if( text ) { strncpy( c->text, text, sizeof(c->text)-1 ) ; }
	if( asseturl ) { strncpy( c->asseturl, asseturl, sizeof(c->asseturl)-1 ) ; }
	if( notesurl ) { strncpy( c->notesurl, notesurl, sizeof(c->notesurl)-1 ) ; }
	/* Modeless: don't block the session that's starting up. The dialog frees c
	 * on WM_NCDESTROY. */
	HWND h = CreateDialogParamA( GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_UPDATEBOX),
		owner, kitty_upd_dlgproc, (LPARAM)c ) ;
	if( !h ) free( c ) ;
}

/* Transient terminal-title notice: show `text` for `ms`, then restore the title.
 * Runs on a short detached thread so it never blocks; guarded by IsWindow. */
typedef struct { HWND hwnd ; int ms ; char text[512] ; char saved[512] ; } kitty_titlenotice_t ;

/* Tint the title bar green while the notice shows so it is actually noticed
 * (Windows 11 DWMWA_CAPTION_COLOR/DWMWA_TEXT_COLOR; silent no-op on older
 * Windows). dwmapi.dll is loaded on demand so nothing new is linked. */
static void kitty_caption_tint( HWND hwnd, int on ) {
	typedef HRESULT (WINAPI *dwmswa_t)( HWND, DWORD, LPCVOID, DWORD ) ;
	static dwmswa_t dwmswa = NULL ;
	static int inited = 0 ;
	if( !inited ) {
		HMODULE dwm = LoadLibraryA( "dwmapi.dll" ) ;
		if( dwm ) dwmswa = (dwmswa_t)GetProcAddress( dwm, "DwmSetWindowAttribute" ) ;
		inited = 1 ;
	}
	if( dwmswa ) {
		/* 35/36 = DWMWA_CAPTION_COLOR/DWMWA_TEXT_COLOR (absent from older
		 * MinGW headers); 0xFFFFFFFF = DWMWA_COLOR_DEFAULT. */
		DWORD caption = on ? (DWORD)RGB(16,124,16)    : 0xFFFFFFFFu ;
		DWORD text    = on ? (DWORD)RGB(255,255,255)  : 0xFFFFFFFFu ;
		dwmswa( hwnd, 35, &caption, sizeof(caption) ) ;
		dwmswa( hwnd, 36, &text, sizeof(text) ) ;
	}
}
static DWORD WINAPI kitty_titlenotice_thread( LPVOID p ) {
	kitty_titlenotice_t *t = (kitty_titlenotice_t*)p ;
	if( IsWindow(t->hwnd) ) { kitty_caption_tint( t->hwnd, 1 ) ; SetWindowTextA( t->hwnd, t->text ) ; }
	Sleep( t->ms ) ;
	if( IsWindow(t->hwnd) ) { kitty_caption_tint( t->hwnd, 0 ) ; SetWindowTextA( t->hwnd, t->saved ) ; }
	free( t ) ;
	return 0 ;
}
static void kitty_title_notice( HWND hwnd, const char *text, int ms ) {
	if( !hwnd || !IsWindow(hwnd) ) return ;
	kitty_titlenotice_t *t = (kitty_titlenotice_t*)calloc( 1, sizeof(*t) ) ;
	if( !t ) return ;
	t->hwnd = hwnd ; t->ms = ms ;
	GetWindowTextA( hwnd, t->saved, sizeof(t->saved)-1 ) ;
	strncpy( t->text, text, sizeof(t->text)-1 ) ;
	HANDLE th = CreateThread( NULL, 0, kitty_titlenotice_thread, t, 0, NULL ) ;
	if( th ) CloseHandle( th ) ; else free( t ) ;
}

void CheckVersionFromWebSite( HWND hwnd, int is_terminal ) {
	char curnum[64]="" ;
	int i ;

	/* Reduce the build string ("0.84.0.15-beta @ ...") to its numeric prefix. */
	strncpy( curnum, BuildVersionTime, sizeof(curnum)-1 ) ; curnum[sizeof(curnum)-1]='\0' ;
	for( i=0 ; i<(int)strlen(curnum) ; i++ ) {
		if( !(((curnum[i]>='0')&&(curnum[i]<='9'))||(curnum[i]=='.')) ) { curnum[i]='\0' ; break ; }
		}

	/* KiTTY: is THIS build a beta? (stable builds don't silently take betas) */
	/* Channel of THIS build (see kitty_update_notice): version scheme + string. */
	int cur_is_beta ;
	{ int cvb[4] ; kitty_parse_version( curnum, cvb ) ;
	  cur_is_beta = ( cvb[3] != 0 ) || ( strstr(BuildVersionTime,"beta")!=NULL )
	                                || ( strstr(BuildVersionTime,"BETA")!=NULL ) ; }

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
		char tag[128]="" ;   /* KiTTY: the full tag, e.g. "kitty-0.84.1.72-beta",
		                      * kept for the "View release notes" URL */
		int latest_is_beta = 0 ;
		/* Channel of the newest release from GitHub's own "prerelease" flag
		 * (betas are published with "prerelease":true) rather than the tag text;
		 * per_page=1 so the first occurrence is this newest release. */
		{
			char *pr = strstr( body, "\"prerelease\"" ) ;
			if( pr != NULL ) {
				pr = strchr( pr, ':' ) ; if( pr!=NULL ) pr++ ;
				while( (pr!=NULL) && (*pr==' '||*pr=='\t') ) pr++ ;
				latest_is_beta = ( (pr!=NULL) && (strncmp(pr,"true",4)==0) ) ;
				}
			}
		if( p != NULL ) {
			p = strchr( p, ':' ) ; if( p!=NULL ) p++ ;
			while( (p!=NULL) && (*p==' '||*p=='\"') ) p++ ;
			int j=0 ;
			while( (p!=NULL) && *p && (*p!='\"') && (j<(int)sizeof(tag)-1) ) { tag[j++]=*p++ ; }
			tag[j]='\0' ;
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
				/* An update is available. Compose the message + pick the action,
				 * then show the NON-modal, no-sound popup over the caller window
				 * (it stays up until the user picks Update/Later). The download/
				 * verify/install runs from its "Update" button
				 * (kitty_do_msi_update), which keeps modal error boxes. */
				int stable_taking_beta = ( !cur_is_beta && latest_is_beta ) ;
				kitty_install_t itype = kitty_detect_install_type() ;
				/* KiTTY: that release's GitHub page, for "View release notes". */
				char notesurl[512]="" ;
				if( tag[0] )
					snprintf( notesurl, sizeof(notesurl), "%s/tag/%s",
						KITTY_RELEASES_URL, tag ) ;
				char asseturl[1024]="" ; int haveasset = 0 ;
				if( itype != KITTY_INST_PORTABLE ) {
					const char *suffix = (itype==KITTY_INST_SYSTEM)
						? "-x64-system.msi" : "-x64-peruser.msi" ;
					haveasset = kitty_find_asset_url( body, suffix, asseturl, sizeof(asseturl) ) ;
				}
				free( body ) ; body = NULL ;
				/* Refuse a non-HTTPS asset URL (never auto-download+run over plain http). */
				if( haveasset && strncmp( asseturl, "https://", 8 )!=0 ) haveasset = 0 ;

				if( itype==KITTY_INST_PORTABLE || !haveasset ) {
					/* Portable copy or no matching asset: offer the download page. */
					snprintf( msg, sizeof(msg),
						"An update is available.\r\n\r\nInstalled: %s\r\nLatest:    %s%s\r\n\r\n%s",
						curnum, latestnum, stable_taking_beta ? "  (BETA)" : "",
						(itype==KITTY_INST_PORTABLE)
						  ? "Portable copy - auto-install is disabled. Open the download page?"
						  : "The matching installer wasn't found. Open the download page?" ) ;
					kitty_show_update_popup( hwnd, msg, KUP_ACT_OPENPAGE, KITTY_RELEASES_URL, itype, notesurl ) ;
					return ;
				}
				/* MSI install path. If a stable build is offered a beta, say so in
				 * the text (the "Update now" button is the explicit opt-in). */
				snprintf( msg, sizeof(msg),
					"An update is available.\r\n\r\nInstalled: %s\r\nLatest:    %s%s\r\n\r\n%s"
					"KiTTY will close and reconnect during the upgrade; the installer's "
					"signature is verified before it runs.",
					curnum, latestnum, stable_taking_beta ? "  (BETA)" : "",
					stable_taking_beta
					  ? "You are on a STABLE release and the newest build is a BETA (less tested). "
					  : "" ) ;
				kitty_show_update_popup( hwnd, msg, KUP_ACT_MSI, asseturl, itype, notesurl ) ;
				return ;
			} else {
				/* No update. In a live terminal, state it in the title for 3s (no
				 * box at all). Elsewhere (config box), a non-modal auto-dismiss box. */
				if( is_terminal ) {
					char note[256] ;
					snprintf( note, sizeof(note), "KiTTY - up to date (%s%s is the latest)",
						curnum, cur_is_beta ? "-beta" : "" ) ;
					kitty_title_notice( hwnd, note, 5000 ) ;
				} else {
					snprintf( msg, sizeof(msg),
						"You are running the latest version.\r\n\r\nInstalled: %s\r\nLatest:    %s",
						curnum, latestnum ) ;
					kitty_show_update_popup( hwnd, msg, KUP_ACT_NONE, NULL, 0, NULL ) ;
				}
				free( body ) ;
				return ;
			}
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


/*
 * Shared renderer for the non-modal connection-error paths in
 * windows/window.c win_seat_connection_fatal / win_seat_nonfatal (cf.
 * upstream cyd01/KiTTY #548): print the error INLINE in the terminal --
 * red "Fatal Error" or yellow "Error" label, default-coloured detail --
 * instead of a modal box that traps the window.  Newlines are normalised
 * to CRLF so the message doesn't staircase down the terminal; for fatal
 * errors a trailing empty quoted description (servers often send
 * '...: ""') is trimmed.  The caller keeps the seat-side consequences
 * (mouse pointer, session close / titlebar marker).
 */
void kitty_term_print_inline_error(Terminal *term, const char *msg, int fatal)
{
    size_t mlen = msg ? strlen(msg) : 0;
    char *body = snewn(mlen * 2 + 1, char);
    size_t bl = 0;
    const char *p;
    char *line;
    for (p = msg ? msg : ""; *p; p++) {
        if (*p == '\r') continue;
        else if (*p == '\n') { body[bl++] = '\r'; body[bl++] = '\n'; }
        else body[bl++] = *p;
    }
    body[bl] = 0;
    if (fatal && bl >= 2 && body[bl-1] == '"' && body[bl-2] == '"') {
        bl -= 2;
        while (bl > 0 && (body[bl-1] == ' ' || body[bl-1] == ':' ||
                          body[bl-1] == '\r' || body[bl-1] == '\n')) bl--;
        body[bl] = 0;
    }
    line = dupprintf("\r\n\x1b[1;3%cm%s %s:\x1b[0m %s\r\n",
                     fatal ? '1' : '3', appname,
                     fatal ? "Fatal Error" : "Error", body);
    term_data(term, line, strlen(line));
    sfree(line);
    sfree(body);
}


/* ---- System-menu command handlers (KiTTY) --------------------------------
 * Bodies of a few WM_COMMAND cases in windows/window.c that manipulate only
 * the Win32 window and the session Conf (no window.c statics), lifted here so
 * the WndProc dispatch stays a thin one-line call per case and the upstream
 * file keeps a smaller diff. Cases that touch window.c internals (e.g. the
 * terminal resize path via reset_window) deliberately stay inline there. */

int GetTransparencyFlag(void);          /* kitty.c */

/* IDM_TRANSPARUP / IDM_TRANSPARDOWN: step the layered-window transparency.
 * Refuses on both opt-outs. -1 used to be clamped to 0 and stepped from there,
 * which let the menu undo a setting the keyboard already respected. */
void kitty_menu_adjust_transparency(HWND term_hwnd, Conf *conf, int up)
{
    int t = conf_get_int(conf, CONF_transparencynumber);
    if (!GetTransparencyFlag() || t < 0) return;
    t += up ? 10 : -10;
    if (t < 0) t = 0; if (t > 254) t = 254;
    conf_set_int(conf, CONF_transparencynumber, t);
    SetWindowLongPtr(term_hwnd, GWL_EXSTYLE,
        GetWindowLongPtr(term_hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(term_hwnd, 0, (BYTE)(255 - t), LWA_ALPHA);
}

/* Position of a DIRECT child of this menu, or -1. Deliberately not
 * GetMenuState/MF_BYCOMMAND: those search submenus, so asked about the system
 * menu they answer about the Window submenu inside it - which meant the edits
 * landed one menu deeper than the position checks around them. */
static int kitty_menu_pos_of(HMENU menu, UINT id)
{
    int n = GetMenuItemCount(menu), i;
    for (i = 0; i < n; i++)
        if (GetMenuItemID(menu, i) == id)
            return i;
    return -1;
}

/* Add or remove the two transparency entries so the menu matches the session.
 * Called for every popup that opens; the anchor item (Font Up) identifies the
 * Window submenu, and being a DIRECT child is the test, so the system menu
 * that merely contains that submenu is left alone. IDs are passed in rather
 * than included, to keep the IDM_ table in one place. */
void kitty_sync_transparency_menu(HMENU menu, Conf *conf, UINT id_up,
                                  UINT id_down, UINT id_anchor)
{
    MENUITEMINFO mii;
    int anchor;
    bool want, have;

    if (!menu)
        return;
    anchor = kitty_menu_pos_of(menu, id_anchor);
    if (anchor < 0)
        return;                         /* not the Window submenu itself */

    want = GetTransparencyFlag() &&
        conf_get_int(conf, CONF_transparencynumber) != -1;
    have = kitty_menu_pos_of(menu, id_up) >= 0;
    if (want == have)
        return;

    if (!want) {
        DeleteMenu(menu, id_up, MF_BYCOMMAND);
        DeleteMenu(menu, id_down, MF_BYCOMMAND);
        /* The pair was followed by a separator, i.e. the item just before the
         * anchor. Found relative to the anchor rather than assumed at the top,
         * so it stays right if the submenu is reordered later. */
        anchor = kitty_menu_pos_of(menu, id_anchor);
        if (anchor > 0) {
            memset(&mii, 0, sizeof(mii));
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_FTYPE;
            if (GetMenuItemInfo(menu, anchor - 1, TRUE, &mii) &&
                (mii.fType & MFT_SEPARATOR))
                DeleteMenu(menu, anchor - 1, MF_BYPOSITION);
        }
    } else {
        /* each insert goes before the anchor, which shifts down by one */
        InsertMenu(menu, anchor, MF_BYPOSITION, id_up, "Transparency &+");
        InsertMenu(menu, anchor + 1, MF_BYPOSITION, id_down, "Transparency &-");
        InsertMenu(menu, anchor + 2, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    }
}

/* IDM_VISIBLE: toggle always-on-top. */
void kitty_menu_toggle_alwaysontop(HWND term_hwnd, Conf *conf)
{
    bool on = !conf_get_bool(conf, CONF_alwaysontop);
    conf_set_bool(conf, CONF_alwaysontop, on);
    SetWindowPos(term_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    kitty_refresh_title();   /* keep the (ONTOP) title marker in sync */
}

/* [ConfigBox] noexit=yes: launch a fresh instance of ourselves with no
 * arguments, i.e. the configuration box, so closing a session lands the user
 * back in the session picker. Called from WinMain's exit path. */
void kitty_respawn_config_box(void)
{
    char module[MAX_PATH + 1] = "", cmd[MAX_PATH + 3] = "";
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    if (!GetModuleFileName(NULL, module, MAX_PATH)) return;
    snprintf(cmd, sizeof(cmd), "\"%s\"", module);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        /* let the new config box take the foreground despite us being the
         * dying foreground process (same dance as RunCommand) */
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

/* IDM_REPOS: move the window to x,y (clamped to >=1), remembering it in conf. */
void kitty_menu_reposition(HWND term_hwnd, Conf *conf, int x, int y)
{
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    conf_set_int(conf, CONF_xpos, x);
    conf_set_int(conf, CONF_ypos, y);
    SetWindowPos(term_hwnd, 0, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

/* ---- Window-title placeholder reference (config box, Window > Behaviour) ----
 *
 * The placeholders KiTTY expands in a window title. Classic KiTTY listed them
 * as eight static lines squeezed into the panel; this is a modeless window, so
 * the list stays readable WHILE the title is being typed, and each entry can be
 * copied instead of retyped from memory. Keep in step with
 * kitty_expand_wintitle() and docs/window-title-placeholders.md.
 *
 * The codes carry two '%' because that is what goes into the field: a title is
 * run through a printf-style expansion first, so the clipboard has to hand over
 * exactly what must be pasted. */
static const struct { const char *code, *desc; } kitty_title_vars[] = {
    { "%%h", "Hostname (the configured host if none is known yet)" },
    { "%%s", "Saved session name" },
    { "%%u", "Username configured for the session" },
    { "%%p", "Port number" },
    { "%%P", "Protocol name, e.g. SSH" },
    { "%%f", "Folder the saved session lives in" },
    { "%%l", "Local forwarded ports (blank if none)" },
    { "%%d", "Dynamic/SOCKS forwarded ports (blank if none)" },
};

static HWND kitty_titlevars_dlg = NULL;

/* kitty_auxpos.c - shared aux-window placement/memory, as used by the About
 * boxes and the /help window. */
void kitty_auxpos_apply(HWND hwnd, const char *name, HWND owner, int centre);
void kitty_auxpos_save(HWND hwnd, const char *name);

/* Put the selected placeholder - the code alone, not its description - on the
 * clipboard. */
static void kitty_titlevars_copy(HWND hwnd)
{
    int sel = (int)SendDlgItemMessage(hwnd, IDC_TITLEVARS_LIST, LB_GETCURSEL, 0, 0);
    HGLOBAL h;
    char *p;
    size_t n;
    if (sel < 0 || sel >= (int)lenof(kitty_title_vars)) { MessageBeep(0); return; }
    n = strlen(kitty_title_vars[sel].code) + 1;
    h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!h) return;
    p = (char *)GlobalLock(h);
    if (!p) { GlobalFree(h); return; }
    memcpy(p, kitty_title_vars[sel].code, n);
    GlobalUnlock(h);
    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, h);   /* the clipboard owns it now */
        CloseClipboard();
    } else {
        GlobalFree(h);
    }
}

static INT_PTR CALLBACK TitleVarsProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
      case WM_INITDIALOG: {
        size_t i;
        char line[160];
        for (i = 0; i < lenof(kitty_title_vars); i++) {
            snprintf(line, sizeof(line), "%-6s %s",
                     kitty_title_vars[i].code, kitty_title_vars[i].desc);
            SendDlgItemMessage(hwnd, IDC_TITLEVARS_LIST, LB_ADDSTRING,
                               0, (LPARAM)line);
        }
        SendDlgItemMessage(hwnd, IDC_TITLEVARS_LIST, LB_SETCURSEL, 0, 0);
        kitty_auxpos_apply(hwnd, "TitleVars", GetWindow(hwnd, GW_OWNER), 1);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDC_TITLEVARS_COPY:
            kitty_titlevars_copy(hwnd);
            return 1;
          case IDC_TITLEVARS_LIST:
            if (HIWORD(wParam) == LBN_DBLCLK) { kitty_titlevars_copy(hwnd); return 1; }
            return 0;
          case IDOK: case IDCANCEL:
            DestroyWindow(hwnd);
            return 1;
        }
        return 0;
      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 1;
      case WM_DESTROY:
        kitty_auxpos_save(hwnd, "TitleVars");
        ShinyRemoveAuxDialog(hwnd);
        kitty_titlevars_dlg = NULL;
        return 0;
    }
    return 0;
}

/* Open (or re-focus) the placeholder list. Modeless, and registered as an aux
 * dialog so it keeps its keyboard handling while the modal configuration box is
 * up - the whole point being that it can sit beside the field being edited. */
void kitty_show_title_placeholders(HWND owner)
{
    if (kitty_titlevars_dlg && IsWindow(kitty_titlevars_dlg)) {
        SetForegroundWindow(kitty_titlevars_dlg);
        return;
    }
    kitty_titlevars_dlg = CreateDialog(hinst, MAKEINTRESOURCE(IDD_TITLEVARS),
                                       owner, TitleVarsProc);
    if (kitty_titlevars_dlg) {
        ShinyAddAuxDialog(kitty_titlevars_dlg);
        ShowWindow(kitty_titlevars_dlg, SW_SHOW);
        SetForegroundWindow(kitty_titlevars_dlg);
    }
}

/* IDM_HYPERLINKTOGGLE: flip runtime URL detection and sync the menu check. */
void kitty_menu_toggle_hyperlink(HWND hwnd)
{
    int GetHyperlinkFlag(void);
    void SetHyperlinkFlag(int flag);
    int nf = !GetHyperlinkFlag();
    SetHyperlinkFlag(nf);
    CheckMenuItem(GetSystemMenu(hwnd, FALSE), IDM_HYPERLINKTOGGLE,
                  MF_BYCOMMAND | (nf ? MF_CHECKED : MF_UNCHECKED));
}


/* ------------------------------------------------------------------ *
 * KiTTY: client-side serving-agent verification (security pass #3).
 *
 * When kitty.exe asks the SSH agent to list or sign, SOMETHING answers on
 * our agent pipe / Pageant window. This confirms that something is a
 * genuine, our-publisher-signed KiTTY/kageant, and warns once if not - a
 * hostile program that grabbed the pipe/window would otherwise see every
 * key operation this session performs.
 *
 * Fail-quiet and best-effort: only a SIGNED (release) kitty.exe can
 * honestly demand a signed agent, so an unsigned dev build says nothing;
 * an unreadable server process says nothing. Never blocks the query - the
 * answer is already in hand when this runs. Configurable off via
 * [KiTTY] verifyagent=no.
 * ------------------------------------------------------------------ */
extern int ReadParameter(const char *key, const char *name, char *value);

static void kitty_agent_serving_check(unsigned long server_pid, int transport)
{
    static int done = 0;
    char self[MAX_PATH], srv[MAX_PATH], cfg[16];
    HANDLE h;
    DWORD sz = sizeof(srv);
    int gotpath;
    (void)transport;

    if (done)
        return;

    /* Opt-out. */
    if (ReadParameter("KiTTY", "verifyagent", cfg) && !stricmp(cfg, "no")) {
        done = 1;
        return;
    }

    if (GetModuleFileNameA(NULL, self, sizeof(self)) == 0)
        return;                        /* try again on the next query */

    /* An unsigned build cannot honestly insist the agent be signed. */
    if (!kitty_authenticode_verify(self)) {
        done = 1;
        return;
    }

    if (server_pid == 0)
        return;                        /* unknown this time; retry later */

    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                    (DWORD)server_pid);
    if (!h) {
        done = 1;                      /* cannot inspect: stay quiet */
        return;
    }
    gotpath = QueryFullProcessImageNameA(h, 0, srv, &sz);
    CloseHandle(h);
    if (!gotpath) {
        done = 1;
        return;
    }

    done = 1;
    if (kitty_authenticode_verify(srv))
        return;                        /* genuine KiTTY/kageant - all good */

    {
        /* Say WHO is speaking (this terminal, not the agent) before saying
         * what was found - an anonymous amber box reads as "something says
         * my key is compromised" and confuses more than it warns. And say
         * what is actually at stake: the program SERVES the keys, so it can
         * see and sign with them - that is not the same as "your key
         * material leaked", which the first wording implied. Clicking the
         * notice lands on the setting that turns the warning off, for
         * people who run another agent on purpose. */
        HWND GetMainHwnd(void);
        const char *base = strrchr(srv, '\\');
        char *msg = dupprintf(
            "This KiTTY terminal window checked which program answers its "
            "SSH agent requests. The answer came from an unverified "
            "program:\n\n%s\n\nThat program sees, and can sign with, every "
            "key this session uses. That is expected if you chose to run "
            "stock Pageant, the Windows OpenSSH agent or another agent - "
            "click this notice to open the setting that turns the warning "
            "off. If you did not choose that agent, find out what that "
            "program is before trusting this session.",
            base ? base + 1 : srv);
        kitty_notice_show("KiTTY: SSH agent not verified", msg,
                          RGB(190, 110, 0), 15,
                          GetMainHwnd(), WM_KITTY_AGENT_UNVERIFIED);
        sfree(msg);
    }
}

void kitty_install_agent_check(void)
{
    agent_serving_check_hook = kitty_agent_serving_check;
}
