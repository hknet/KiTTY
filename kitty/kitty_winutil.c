/*
 * kitty_winutil.c - small Win32 helpers shared by the KiTTY additions:
 * window transparency and the OS version, the file and folder selection
 * dialogs (open, save, folder picker), printing a block of text, the
 * clipboard write helper, launching a process (RunCommand, the -ed editor),
 * the event-log printf and the absolute-path test.
 */
#include "kitty_winutil.h"
#include "kitty_win.h"
#include "kitty_authenticode.h"   /* shared Authenticode trust + CN gate */
#include "kitty_notice.h"          /* near-the-clock warning window */
#include "kitty_rc_additions.h"   /* IDD_UPDATEBOX, IDC_UPD_TEXT, IDC_UPD_UPDATE */
#include "kitty_theme.h"           /* the app-wide colour theme */
#include "../windows/putty-rc.h"   /* -demo-templates: the shared dialog ids */
#include "kitty_oldwin.h"   /* APIs newer than the oldest Windows we load on */
#include "kitty_text.h"     /* shared captions */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include <wininet.h>   /* CheckVersionFromWebSite: GitHub releases query */
#include <wintrust.h>  /* in-app updater: Authenticode trust verification */
#include <softpub.h>   /* WINTRUST_ACTION_GENERIC_VERIFY_V2 */
#include <msi.h>       /* in-app updater: install-type detection by UpgradeCode */
#include "kitty_gui.h"
#include "kitty.h"
#include "kitty_storage.h"
#include "kitty_auxpos.h"
/* wincrypt.h (CryptQueryObject / signer cert) comes in via windows.h */

/* MOD_PERSO event-log wrapper, defined in windows/window.c */

// Change the window transparency
void SetTransparency( HWND hwnd, int value ) {
#ifndef MOD_NOTRANSPARENCY
	SetLayeredWindowAttributes( hwnd, 0, value, LWA_ALPHA ) ;
#endif
	}


// Operating system version number
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
BOOL IsWow64(void) {
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
	return OpenFileNameFrom( hFrame, filename, Title, Filter, NULL ) ;
}

int OpenFileNameFrom( HWND hFrame, char * filename, char * Title, char * Filter, const char * initialdir ) {
	char * szTitle = Title ;
	char szFilter[4096] ; snprintf( szFilter, sizeof(szFilter), "%s", Filter ) ;
	// replace the '|' characters with NUL characters.
	int i = 0;
	while(i < sizeof(szFilter) && szFilter[i] != '\0')
	{
		if(szFilter[i] == '|')
			szFilter[i] = '\0';

		i++;
	}

	// the file open dialog box
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
	ofn.lpstrInitialDir	= ( initialdir && initialdir[0] ) ? initialdir : NULL ;
	ofn.Flags		= OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST
				| OFN_HIDEREADONLY | OFN_LONGNAMES
				| OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_EXTENSIONDIFFERENT | OFN_DONTADDTORECENT
				;

	// if no file name was selected, give up
	if(!GetOpenFileName(&ofn)) { return 0 ; }
	else { return 1 ; }
	}

int SaveFileName( HWND hFrame, char * filename, char * Title, char * Filter ) {
	char * szTitle = Title ;
	char szFilter[4096] ; snprintf( szFilter, sizeof(szFilter), "%s", Filter ) ;
	// replace the '|' characters with NUL characters.
	int i = 0;
	while(i < sizeof(szFilter) && szFilter[i] != '\0')
	{
		if(szFilter[i] == '|')
			szFilter[i] = '\0';

		i++;
	}

	// the file open dialog box
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

	// if no file name was selected, give up
	if(!GetSaveFileName(&ofn)) { return 0 ; }
	else { return 1 ; }
	}

/* The classic Save-As (GetSaveFileName): SaveFileNameFrom's fallback where the
 * Common Item Dialog cannot be created (XP). On Windows 11 the same call was
 * seen opening in the folder Windows remembers for the program, or in
 * Documents, ignoring both the initial folder and the path in the prefilled
 * name - which is why it is only the fallback now. */
static int save_file_name_classic( HWND hFrame, char * filename, char * Title, char * Filter, const char * initialdir ) {
	char szFilter[4096] ; snprintf( szFilter, sizeof(szFilter), "%s", Filter ) ;
	int i = 0 ;
	while( i < (int)sizeof(szFilter) && szFilter[i] != '\0' ) { if( szFilter[i]=='|' ) szFilter[i]='\0' ; i++ ; }
	OPENFILENAME ofn = {0} ;
	ofn.lStructSize   = sizeof(OPENFILENAME) ;
	ofn.hwndOwner     = hFrame ;
	ofn.lpstrFilter   = szFilter ;
	ofn.nFilterIndex  = 1 ;
	ofn.lpstrFile     = filename ;      /* kept as prefilled: the remote base name */
	ofn.nMaxFile      = 4096 ;
	ofn.lpstrTitle    = Title ;
	ofn.lpstrInitialDir = ( initialdir && initialdir[0] ) ? initialdir : NULL ;
	ofn.Flags         = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_LONGNAMES
	                  | OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_NOCHANGEDIR
	                  | OFN_DONTADDTORECENT ;
	if( !GetSaveFileName( &ofn ) ) return 0 ;
	return 1 ;
}

#include <shlobj.h>
#include <shobjidl.h>   /* IFileOpenDialog (Common Item Dialog folder picker) */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_tools.h"        /* existdirectory */

/* Save-As opening in a given folder with a name PREFILLED by the caller (the
 * remote base name, the last component of `filename`). Used by Get File for a
 * single named file; FOS_OVERWRITEPROMPT is Windows' own replace question, so
 * a file is never overwritten silently. Returns 1 with the chosen full path in
 * filename (4096 bytes), 0 on cancel.
 *
 * KiTTY: the Common Item Dialog (Vista+) with SetFolder, which FORCES the
 * folder: GetSaveFileName opened in the folder Windows remembers for the
 * program (or Documents), so Get File's Save-As never showed the download
 * folder it was given. GetSaveFileName stays as the fallback (XP). */
int SaveFileNameFrom( HWND hFrame, char * filename, char * Title, char * Filter, const char * initialdir ) {
	static const GUID clsid_fsd = {0xC0B4E2F3,0xBA21,0x4773,{0x8D,0xBA,0x33,0x5E,0xC9,0x46,0xEB,0x8B}} ;   /* FileSaveDialog */
	static const GUID iid_fsd   = {0x84bccd23,0x5fde,0x4cdb,{0xae,0xa4,0xaf,0x64,0xb8,0x3d,0x78,0xab}} ;   /* IFileSaveDialog */
	static const GUID iid_si    = {0x43826d1e,0xe718,0x42ee,{0xbc,0x55,0xa1,0xe2,0x61,0xc3,0x7b,0xfe}} ;   /* IShellItem */
	IFileSaveDialog *pfd = NULL ;
	int ret = 0 ;
	HRESULT hrInit = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE ) ;

	if( FAILED( CoCreateInstance( &clsid_fsd, NULL, CLSCTX_INPROC_SERVER, &iid_fsd, (void**)&pfd ) ) || !pfd ) {
		if( SUCCEEDED( hrInit ) ) CoUninitialize() ;
		return save_file_name_classic( hFrame, filename, Title, Filter, initialdir ) ;
	}
	{
		DWORD opts = 0 ;
		pfd->lpVtbl->GetOptions( pfd, &opts ) ;
		pfd->lpVtbl->SetOptions( pfd, opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST
		                               | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT ) ;
	}
	if( Title ) {
		wchar_t wt[256] ;
		MultiByteToWideChar( CP_ACP, 0, Title, -1, wt, 256 ) ; wt[255] = 0 ;
		pfd->lpVtbl->SetTitle( pfd, wt ) ;
	}
	/* The filter: "Name|pattern|Name|pattern|" as the classic dialog takes it. */
	if( Filter && Filter[0] ) {
		COMDLG_FILTERSPEC spec[8] ; wchar_t wf[8][2][256] ; UINT n = 0 ;
		char f[1024] ; char *p, *name, *pat ;
		snprintf( f, sizeof(f), "%s", Filter ) ;
		for( p = f ; n < 8 && *p ; n++ ) {
			name = p ; if( !(p = strchr( p, '|' )) ) break ; *p++ = '\0' ;
			pat = p ; if( (p = strchr( p, '|' )) ) *p++ = '\0' ; else p = pat + strlen( pat ) ;
			MultiByteToWideChar( CP_ACP, 0, name, -1, wf[n][0], 256 ) ; wf[n][0][255] = 0 ;
			MultiByteToWideChar( CP_ACP, 0, pat, -1, wf[n][1], 256 ) ; wf[n][1][255] = 0 ;
			spec[n].pszName = wf[n][0] ; spec[n].pszSpec = wf[n][1] ;
		}
		if( n ) pfd->lpVtbl->SetFileTypes( pfd, n, spec ) ;
	}
	if( initialdir && initialdir[0] && existdirectory( initialdir ) ) {
		/* SHCreateItemFromParsingName is Vista+: resolved at run time, as in
		 * OpenDirNameFrom, so the XP build still loads. */
		typedef HRESULT (WINAPI *pfn_scifpn)( PCWSTR, IBindCtx *, REFIID, void ** ) ;
		HMODULE sh = GetModuleHandleA( "shell32.dll" ) ;
		pfn_scifpn scifpn = sh ? (pfn_scifpn)GetProcAddress( sh, "SHCreateItemFromParsingName" ) : NULL ;
		if( scifpn ) {
			wchar_t wi[4096] ; IShellItem *psi0 = NULL ;
			MultiByteToWideChar( CP_ACP, 0, initialdir, -1, wi, 4096 ) ; wi[4095] = 0 ;
			if( SUCCEEDED( scifpn( wi, NULL, &iid_si, (void**)&psi0 ) ) && psi0 ) {
				pfd->lpVtbl->SetFolder( pfd, psi0 ) ;
				psi0->lpVtbl->Release( psi0 ) ;
			}
		}
	}
	{
		/* the prefilled name without its folder: the folder is set above */
		const char *base = strrchr( filename, '\\' ) ;
		wchar_t wn[1024] ;
		MultiByteToWideChar( CP_ACP, 0, base ? base + 1 : filename, -1, wn, 1024 ) ; wn[1023] = 0 ;
		if( wn[0] ) pfd->lpVtbl->SetFileName( pfd, wn ) ;
	}
	if( SUCCEEDED( pfd->lpVtbl->Show( pfd, hFrame ) ) ) {
		IShellItem *psi = NULL ;
		if( SUCCEEDED( pfd->lpVtbl->GetResult( pfd, &psi ) ) && psi ) {
			PWSTR wpath = NULL ;
			if( SUCCEEDED( psi->lpVtbl->GetDisplayName( psi, SIGDN_FILESYSPATH, &wpath ) ) && wpath ) {
				if( WideCharToMultiByte( CP_ACP, 0, wpath, -1, filename, 4096, NULL, NULL ) > 0 ) ret = 1 ;
				CoTaskMemFree( wpath ) ;
			}
			psi->lpVtbl->Release( psi ) ;
		}
	}
	pfd->lpVtbl->Release( pfd ) ;
	if( SUCCEEDED( hrInit ) ) CoUninitialize() ;
	return ret ;   /* a cancel is a cancel: no fallback to the classic box */
}
/* The tree picker's initial selection: SHBrowseForFolder takes it through a
 * callback, not a field. lParam carries the ANSI path. */
static int CALLBACK browse_start_folder( HWND hwnd, UINT msg, LPARAM lParam, LPARAM data ) {
	(void)lParam ;
	if( msg == BFFM_INITIALIZED && data )
		SendMessage( hwnd, BFFM_SETSELECTIONA, TRUE, data ) ;
	return 0 ;
}

int OpenDirName( HWND hFrame, char * dirname ) {
	return OpenDirNameFrom( hFrame, dirname, NULL, NULL ) ;
}

int OpenDirNameFrom( HWND hFrame, char * dirname, const char * initial, const char * title ) {
	dirname[0] = '\0' ;
	if( initial && (!initial[0] || !existdirectory( initial )) ) initial = NULL ;
	/* Modern Common Item Dialog folder picker (Vista+): the full Explorer window
	 * with an address bar you can paste a path into, type-ahead and favourites -
	 * not the old tree-only SHBrowseForFolder. Falls back to the tree picker (with
	 * a New Folder button) if COM or the dialog is unavailable. */
	static const GUID clsid_fod = {0xDC1C5A9C,0xE88A,0x4dde,{0xA5,0xA1,0x60,0xF8,0x2A,0x20,0xAE,0xF7}} ;
	static const GUID iid_fod   = {0xd57c7288,0xd4ad,0x4768,{0xbe,0x02,0x9d,0x96,0x95,0x32,0xd9,0x60}} ;
	static const GUID iid_si    = {0x43826d1e,0xe718,0x42ee,{0xbc,0x55,0xa1,0xe2,0x61,0xc3,0x7b,0xfe}} ;   /* IShellItem */
	IFileOpenDialog *pfd = NULL ;
	HRESULT hrInit = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE ) ;
	if( SUCCEEDED( CoCreateInstance( &clsid_fod, NULL, CLSCTX_INPROC_SERVER,
	                                 &iid_fod, (void**)&pfd ) ) && pfd ) {
		DWORD opts = 0 ;
		pfd->lpVtbl->GetOptions( pfd, &opts ) ;
		pfd->lpVtbl->SetOptions( pfd, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST ) ;
		if( title ) {
			wchar_t wt[256] ;
			MultiByteToWideChar( CP_ACP, 0, title, -1, wt, 256 ) ; wt[255] = 0 ;
			pfd->lpVtbl->SetTitle( pfd, wt ) ;
		} else {
			pfd->lpVtbl->SetTitle( pfd, L"Select a folder..." ) ;
		}
		if( initial ) {
			/* SHCreateItemFromParsingName is Vista+: resolved at run time so
			 * the XP build still loads (a static import the OS lacks kills the
			 * process in the loader). */
			typedef HRESULT (WINAPI *pfn_scifpn)( PCWSTR, IBindCtx *, REFIID, void ** ) ;
			HMODULE sh = GetModuleHandleA( "shell32.dll" ) ;
			pfn_scifpn scifpn = sh ? (pfn_scifpn)GetProcAddress( sh, "SHCreateItemFromParsingName" ) : NULL ;
			if( scifpn ) {
				wchar_t wi[4096] ; IShellItem *psi0 = NULL ;
				MultiByteToWideChar( CP_ACP, 0, initial, -1, wi, 4096 ) ; wi[4095] = 0 ;
				if( SUCCEEDED( scifpn( wi, NULL, &iid_si, (void**)&psi0 ) ) && psi0 ) {
					pfd->lpVtbl->SetFolder( pfd, psi0 ) ;
					psi0->lpVtbl->Release( psi0 ) ;
				}
			}
		}
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
		bi.lpszTitle = title ? title : KT_CAP_SELECT_FOLDER ;
		bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE ;
		if( initial ) { bi.lpfn = browse_start_folder ; bi.lParam = (LPARAM)initial ; }
		if( (il = SHBrowseForFolder( &bi )) != NULL ) {
			SHGetPathFromIDList( il, Result ) ;
			GlobalFree( il ) ;
			if( Result[0] ) { strcpy( dirname, Result ) ; return 1 ; }
		}
	}
	return 0 ;
	}


//
// Send to the printer
//
// Printing parameters
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
                  			LinePrint[Index1] = '\0'; // Print the last page
                  			TextOut(pd.hDC,100, Index2*PrintCharSize, LinePrint, strlen(LinePrint)) ;
               	  			EndPage(pd.hDC) ;
                  			EndDoc(pd.hDC) ;
                  			szMessage = KT_WIN_PRINT_OK;
					free( LinePrint ) ;
              				}
              			else { return_code = 1 ;  /* Empty string */ }
				}
			else { // StartDoc problem
				szMessage = KT_WIN_PRINT_ERR1 ;
				return_code = 2 ;
				}
			}
		else { // pd.hDC problem
			szMessage = KT_WIN_PRINT_ERR2 ;
			return_code = 3 ;
			}
		}
	else { // PrintDlg problem
		//szMessage = "Impression annulee par l'utilisateur" ;
		return_code = 4 ;
		}
	if (szMessage) { MessageBox (NULL, szMessage, KT_CAP_PRINT_REPORT, MB_OK) ; }
	
	return return_code ;
	}

// Print the text in the notepad
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

// Put a text into the clipboard
int SetTextToClipboard( const char * buf ) {
	HGLOBAL hglbCopy ;
	LPTSTR lptstrCopy ;
	int attempt ;
	/* NO IsClipboardFormatAvailable(CF_TEXT) TEST HERE. It used to guard this
	 * function, which made putting text on the clipboard depend on text already
	 * being on it: with an image copied, or nothing copied since login, every
	 * Copy in KiTTY silently did nothing and returned 0. That test belongs to
	 * READING the clipboard, not writing it.
	 *
	 * The clipboard is a single system-wide resource and one process holds it at
	 * a time, so OpenClipboard failing is normal and transient - it is worth
	 * waiting for rather than silently dropping the text to be copied. */
	for( attempt = 0 ; attempt < 10 ; attempt++ ) {
		if( OpenClipboard(NULL) ) break ;
		Sleep( 20 ) ;
	}
	if( attempt >= 10 ) return 0 ;
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

// Run a command
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

// Display a message in the event log
void debug_logevent( const char *fmt, ... ) {
	va_list ap;
	char *buf;
	va_start(ap, fmt);
	buf = dupvprintf(fmt, ap) ;
	va_end(ap);
	do_eventlog(buf) ;
	free(buf);
}

// Test whether a path is absolute
bool IsPathAbsolute( const char * path ) {
	bool test = false ;
	if( path == NULL ) { return false ; }
	if( strlen( path ) < 3 ) { return false ; }
	if( ((path[0]>='a') && (path[0]<='z')) || ((path[0]>='A') && (path[0]<='Z')) )
		if( path[1]==':' )
			if( (path[2]=='/') || (path[2]=='\\') ) test = true ;
	return test ;
}

/*
 * KiTTY: %VAR% expansion (cyd01/KiTTY#472) for the plain-string (non-
 * Filename) settings that hold a path: the download/upload folders, the
 * PSCPPath/WinSCPPath/FileZillaPath helper-program paths, the rzcommand/
 * szcommand ZModem programs (kitty_zmodem.c), and the sav= override. These never round-trip through the Filename type - each is a
 * char* read straight out of kitty.ini/the registry (ReadParameterN) into a
 * running global or a local buffer.
 *
 * Called ONLY at the point the stored text is turned into a path a real
 * Win32 API is about to use (existfile/existdirectory, GetShortPathName, the
 * actual open) - never where it is loaded for display or written back, so
 * the config box and the store keep showing/saving exactly what was typed,
 * e.g. "%OneDrive%\Kitty-Logs". An unknown %name% is left literal, which is
 * what ExpandEnvironmentStrings already does on its own.
 *
 * Returns a newly malloc'd string the caller frees with free() (matching
 * the malloc/strcpy idiom every call site here already uses for these
 * globals); never NULL, even if `in` is NULL (an empty string comes back).
 */
char *kitty_expand_env_dup(const char *in)
{
	DWORD need;
	char *out;

	if (in == NULL)
		in = "";

	need = ExpandEnvironmentStrings(in, NULL, 0);
	if (!need) {
		/* Expansion failed: fall back to the literal text, unchanged. */
		out = (char*) malloc(strlen(in) + 1);
		strcpy(out, in);
		return out;
	}

	out = (char*) malloc(need);
	if (!ExpandEnvironmentStrings(in, out, need)) {
		free(out);
		out = (char*) malloc(strlen(in) + 1);
		strcpy(out, in);
	}
	return out;
}
