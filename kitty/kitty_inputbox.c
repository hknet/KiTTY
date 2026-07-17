/*
 * KiTTY input/info dialog boxes, moved verbatim out of kitty.c to shrink
 * that monolith: the F8 send-text line input, the SHIFT+F8 multiline input
 * (with its Notes load/save and crypt/decrypt edit-control shortcuts), the
 * password prompt used by the encrypted-kitty.sav check, and the InfoBox
 * progress popup. Compiled into the same targets as kitty.c (kitty +
 * kitty_portable), so behaviour is unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "putty-rc.h"      /* dialog resource ids (kitty_rc_additions.h) */

#include <windows.h>

#include "kitty.h"
#include "kitty_commun.h"
#include "kitty_crypt.h"
#include "kitty_registry.h"

/* Provided elsewhere in the KiTTY tree (not in kitty.h). */
extern Conf *conf ;                     /* active-seat global (window.c) */
extern HWND MainHwnd ;                  /* kitty.c: the terminal window */
void SendKeyboardPlus( HWND hwnd, const char * st ) ;   /* kitty.c */

/* Result of the last completed input dialog (malloc'd; NULL = none). The
 * dialog procs below own it; kitty.c reads it via GetInputBoxResult(). */
static char * InputBoxResult = NULL ;

// Boite de dialogue d'information
static LRESULT CALLBACK InfoCallBack( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam ) {
	switch(message) {
		case WM_INITDIALOG: SetWindowText( GetDlgItem(hwnd,IDC_RESULT), "" ) ; break;
		case WM_COMMAND:
			if( LOWORD(wParam) == 1001 ) {
				SetWindowText( GetDlgItem(hwnd,IDC_RESULT), (char*)lParam ) ;
				}

			if (LOWORD(wParam) == IDCANCEL)
			{
				EndDialog(hwnd, LOWORD(0));
			}
			break;
		case WM_CLOSE:
			EndDialog(hwnd, LOWORD(0));
			break;

		return DefWindowProc (hwnd, message, wParam, lParam);
		}
	return 0;
	}
	
HWND InfoBox( HINSTANCE hInstance, HWND hwnd ) {
	HWND hdlg = CreateDialog( hInstance, MAKEINTRESOURCE(IDD_INFOBOX), hwnd, (DLGPROC)InfoCallBack ) ;
	return hdlg ;
	}
	
void InfoBoxSetText( HWND hwnd, char * st ) { SendMessage( hwnd, WM_COMMAND, 1001, (LPARAM) st ) ; SendMessage( hwnd, WM_PAINT, 0,0 ) ; }

void InfoBoxClose( HWND hwnd ) { EndDialog(hwnd, LOWORD(0)) ; DestroyWindow( hwnd ) ; }

//CallBack du dialog InputBox
static int InputBox_Flag = 0 ;

static LRESULT CALLBACK InputCallBack(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam ) {
	HWND handle;
	switch (message) {
		case WM_INITDIALOG:
			/* Advertise the internal-command console this box doubles as. */
			if( IniFileFlag == SAVEMODE_DIR ) {
				SetWindowText(hwnd,"Text input (portable mode) - /help = KiTTY commands");
			} else {
				SetWindowText(hwnd,"Text input - /help = KiTTY commands");
			}
			handle = GetDlgItem(hwnd,IDC_RESULT);
			if( InputBoxResult == NULL ) SetWindowText(handle,"") ;
			else SetWindowText( handle, InputBoxResult ) ;
			break;
		case WM_COMMAND:
			if (LOWORD(wParam) == IDOK)
			{
				handle = GetDlgItem(hwnd,IDC_RESULT);
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				size_t length = GetWindowTextLength( handle ) ;
				InputBoxResult = (char*) malloc( length + 10 ) ;
				GetWindowText(handle,InputBoxResult,length+1);
				//EndDialog(hwnd, LOWORD(1));
				if( !InternalCommand( hwnd, InputBoxResult ) )
					SendKeyboardPlus( MainHwnd, InputBoxResult );
				SetWindowText(handle,"") ;
				
			}
			if (LOWORD(wParam) == IDCANCEL)
			{
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				EndDialog(hwnd, LOWORD(0));
			}
			
			break;

		case WM_CLOSE:
			EndDialog(hwnd, LOWORD(0));
			break;

		return DefWindowProc (hwnd, message, wParam, lParam);
	}
	return 0;
}

static LRESULT CALLBACK InputCallBackPassword(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam){
	HWND handle;
	switch (message) {
		case WM_INITDIALOG:
			handle = GetDlgItem(hwnd,IDC_RESULT);
			if( InputBoxResult == NULL ) SetWindowText(handle,"");
			else SetWindowText( handle, InputBoxResult ) ;
			break;

		case WM_COMMAND:
			if (LOWORD(wParam) == IDOK)
			{
				handle = GetDlgItem(hwnd,IDC_RESULT);
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				size_t length = GetWindowTextLength( handle ) ;
				InputBoxResult = (char*) malloc( length + 10 ) ;
				GetWindowText(handle,InputBoxResult,length+1);
				EndDialog(hwnd, LOWORD(0));
			}
			if (LOWORD(wParam) == IDCANCEL)
			{
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				EndDialog(hwnd, LOWORD(0));
			}
			
			break;

		case WM_CLOSE:
			EndDialog(hwnd, LOWORD(0));
			break;

		return DefWindowProc (hwnd, message, wParam, lParam);
	}
	return 0;
}

// Procedure specifique à la editbox multiligne (SHIFT+F8)
FARPROC lpfnOldEditProc ;
BOOL FAR PASCAL EditMultilineCallBack(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	char buffer[4096], key_name[1024] ;
	switch (message) {
		case WM_KEYDOWN:
			if( (wParam==VK_RETURN) && (GetKeyState( VK_SHIFT )& 0x8000) ){
				SendMessage(GetParent(hwnd),WM_COMMAND,IDB_OK,0 ) ;
				return 0;
				}
			else if( (wParam==VK_F12) && (GetKeyState( VK_SHIFT )& 0x8000) ){
				GetWindowText( hwnd, buffer, 4096 ) ;
				cryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ;
				SetWindowText( hwnd, buffer ) ;
				return 0 ;
				}
			else if( (wParam==VK_F11) && (GetKeyState( VK_SHIFT )& 0x8000) ){
				GetWindowText( hwnd, buffer, 4096 ) ;
				decryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ;
				SetWindowText( hwnd, buffer ) ;
				return 0 ;
				}
			else
				return CallWindowProc((WNDPROC)lpfnOldEditProc, hwnd, message, wParam, lParam);
			break;
		case WM_KEYUP:
		case WM_CHAR:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
			if( (wParam==VK_RETURN) && (GetKeyState( VK_SHIFT )& 0x8000) )
				return 0;
			else if( (wParam==VK_F2) && (GetKeyState( VK_SHIFT )& 0x8000) ) { // Charge une Notes
				snprintf( key_name, sizeof(key_name), "%s\\Sessions\\%s", TEXT(PUTTY_REG_POS), conf_get_str(conf,CONF_sessionname) ) ;
				if( GetValueDataN(HKEY_CURRENT_USER, key_name, "Notes", buffer, sizeof(buffer)) != NULL ) {
					if( GetWindowTextLength(hwnd) > 0 ) 
						if( MessageBox(hwnd, "Are you sure you want to load Notes\nand erase this edit box ?","Load Warning", MB_YESNO|MB_ICONWARNING ) != IDYES ) break ;
					SetWindowText( hwnd, buffer ) ;
					}
				}
			else if( (wParam==VK_F3) && (GetKeyState( VK_SHIFT )& 0x8000) ) { // Sauve une Notes
				GetSessionField( conf_get_str(conf,CONF_sessionname), conf_get_str(conf,CONF_folder), "Notes", buffer ) ;
				if( strlen( buffer ) > 0 ) 
					if( MessageBox(hwnd, "Are you sure you want to save Edit box\ninto Notes registry ?","Save Warning", MB_YESNO|MB_ICONWARNING ) != IDYES ) break ;
				GetWindowText( hwnd, buffer, 4096 ) ;
				snprintf( key_name, sizeof(key_name), "%s\\Sessions\\%s", TEXT(PUTTY_REG_POS), conf_get_str(conf,CONF_sessionname) ) ;
				RegTestOrCreate( HKEY_CURRENT_USER, key_name, "Notes", buffer ) ;
				}
			else 
				return CallWindowProc((WNDPROC)lpfnOldEditProc, hwnd, message, wParam, lParam);	
			break ;
            	default:
			return CallWindowProc((WNDPROC)lpfnOldEditProc, hwnd, message, wParam, lParam);
			break;
		}
	return TRUE ;
	}

static int EditReadOnly = 0 ;
#ifndef GWL_WNDPROC
#define GWL_WNDPROC GWLP_WNDPROC
#endif
static LRESULT CALLBACK InputMultilineCallBack (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	HWND handle;

	switch (message)
	{
		case WM_INITDIALOG: {
			char * buffer ;
			buffer=(char*)malloc(1024);
			/* Caption from the live window title, not CONF_wintitle: the
			 * conf value can hold an unexpanded %%-placeholder template. */
			buffer[0] = '\0' ;
			if( (MainHwnd==NULL) || (GetWindowText( MainHwnd, buffer, 900 )<=0) || (buffer[0]=='\0') )
				strcpy( buffer, "KiTTY" ) ;
			strcat( buffer, " - Text input" ) ;
			SetWindowText( hwnd, buffer ) ;
			free(buffer);
			handle = GetDlgItem(hwnd,IDC_RESULT) ;
			if( EditReadOnly ) SendMessage(handle, EM_SETREADONLY, 1, 0);
			if( InputBoxResult == NULL ) SetWindowText(handle,"");
			else {
				int i,j=0;
				buffer=(char*)malloc(2*strlen(InputBoxResult)+10);
				for( i=0; i<=strlen(InputBoxResult);i++ ) {
					if( (InputBoxResult[i]=='\n') && (buffer[j-1]!='\r') ) 
						{ buffer[j]='\r';j++;}
					buffer[j]=InputBoxResult[i];
					j++;
					}
				SetWindowText( handle, buffer ) ;
				free(buffer);
				}
			
			FARPROC lpfnSubClassProc = (FARPROC)MakeProcInstance( EditMultilineCallBack, hInst );
			if( lpfnSubClassProc )
				lpfnOldEditProc = (FARPROC)SetWindowLongPtr( handle, GWLP_WNDPROC, (LONG_PTR)lpfnSubClassProc );
			}
			break;
		case WM_COMMAND:
			if (LOWORD(wParam) == IDB_OK) {
				handle = GetDlgItem(hwnd,IDC_RESULT);
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				size_t length = GetWindowTextLength( handle ) ;
				InputBoxResult = (char*) malloc( length + 10 ) ;

				GetWindowText(handle,InputBoxResult,length+1);

				// Si il y un texte selectionne, on ne recupere que celui-ci
				DWORD result = SendMessage( handle, EM_GETSEL, (WPARAM)0, (LPARAM)NULL );
				if( LOWORD(result) != HIWORD(result) ) {
					int i ;
					InputBoxResult[HIWORD(result)] = '\0' ;
					if( LOWORD(result) > 0 ) 
						for( i=0 ; i<=(HIWORD(result)-LOWORD(result)) ; i++ )
						InputBoxResult[i]=InputBoxResult[i+LOWORD(result)];
					}
				if( strlen(InputBoxResult)==0 || InputBoxResult[strlen(InputBoxResult)-1] != '\n' )
					strcat( InputBoxResult, "\n" ) ;

				SendKeyboard( MainHwnd, InputBoxResult ) ;
				ShowWindow( MainHwnd, SW_RESTORE ) ;
				BringWindowToTop( MainHwnd ) ;
				SetFocus( handle ) ;
				}
			else if (LOWORD(wParam) == IDCANCEL) {
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				EditReadOnly = 0 ;
				EndDialog(hwnd, LOWORD(0));
				}
			else if( HIWORD( wParam ) == EN_SETFOCUS ) {
				ShowWindow( MainHwnd, SW_SHOWNOACTIVATE ) ;
				DefWindowProc (hwnd, message, wParam, lParam) ;
				}
			break;
		case WM_CHAR: {
			DefWindowProc (hwnd, message, wParam, lParam) ;
			}
			break ;
		case WM_SIZE: {
			int h = HIWORD(lParam),w = LOWORD(lParam) ;
			int top = 35 ;
			RECT br ;
			handle = GetDlgItem( hwnd, IDC_RESULT ) ;
			/* The classic 35/43 constants are 96-dpi pixels and make the
			 * edit overlap the label/button row on high-DPI displays:
			 * derive the row height from the OK button's real bottom. */
			if( GetWindowRect( GetDlgItem( hwnd, IDB_OK ), &br ) ) {
				POINT pt ;
				pt.x = br.left ; pt.y = br.bottom ;
				ScreenToClient( hwnd, &pt ) ;
				top = pt.y + 4 ;
			}
			SetWindowPos( handle, HWND_TOP, 7, top, w-15, h-top-8, 0 ) ;
			DefWindowProc (hwnd, message, wParam, lParam) ;
			}
			break ;
		case WM_CLOSE:
			EditReadOnly = 0 ;
			EndDialog(hwnd, LOWORD(0));
			break;
		
		return DefWindowProc(hwnd, message, wParam, lParam);
	}
	return 0;
}

char * InputBox( HINSTANCE hInstance, HWND hwnd ) {
	if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
	DialogBox(hInstance, MAKEINTRESOURCE(IDD_INPUTBOX), hwnd, (DLGPROC)InputCallBack) ;
	return InputBoxResult ;
	}

char * InputBoxMultiline( HINSTANCE hInstance, HWND hwnd ) {
	if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
	
	if( IsClipboardFormatAvailable(CF_TEXT) ) {
		char * pst = NULL ;
		if( OpenClipboard(NULL) ) {
			HGLOBAL hglb ;
			if( (hglb = GetClipboardData( CF_TEXT ) ) != NULL ) {
				if( ( pst = GlobalLock( hglb ) ) != NULL ) {
					InputBoxResult = (char*) malloc( strlen( pst ) +1 ) ;
					strcpy( InputBoxResult, pst ) ;
					GlobalUnlock( hglb ) ;
					}
				}
			CloseClipboard();
			}
		}

	DialogBox(hInstance, MAKEINTRESOURCE(IDD_INPUTBOXMULTI), NULL, (DLGPROC)InputMultilineCallBack) ;
	return InputBoxResult ;
	}
	
char * InputBoxPassword( HINSTANCE hInstance, HWND hwnd ) {
	if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
	DialogBox(hInstance, MAKEINTRESOURCE(IDD_INPUTBOXPW), hwnd, (DLGPROC)InputCallBackPassword) ;
	return InputBoxResult ;
	}

void GetAndSendLine( HWND hwnd ) {
	if( InputBox_Flag == 1 ) return ;
	InputBox_Flag = 1 ;
	InputBox( hinst, hwnd ) ; // Essayer avec GetModuleHandle(NULL)
	InputBox_Flag = 0 ;
	}
	
void GetAndSendMultiLine( HWND hwnd ) {
	if( InputBox_Flag == 1 ) return ;

	InputBox_Flag = 1 ;
	InputBoxMultiline( hinst, hwnd ) ;
	InputBox_Flag = 0 ;
	}

void GetAndSendLinePassword( HWND hwnd ) {
	if( InputBox_Flag == 1 ) return ;
	InputBox_Flag = 1 ;
	InputBoxPassword( hinst, hwnd ) ; // Essayer avec GetModuleHandle(NULL)
	InputBox_Flag = 0 ;
	}

void routine_inputbox( void * phwnd ) { 
	GetAndSendLine( MainHwnd ) ;
	}

void routine_inputbox_multiline( void * phwnd ) { 
	GetAndSendMultiLine( MainHwnd ) ;
	}

void routine_inputbox_password( void * phwnd ) { 
	GetAndSendLinePassword( MainHwnd ) ;
	}

/* Seam accessor: kitty.c's encrypted-kitty.sav password check reads the
 * password typed into the GetAndSendLinePassword dialog. */
char * GetInputBoxResult( void ) {
	return InputBoxResult ;
	}
