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
#include "kitty_commands.h"   /* InternalCommand, kitty_cmd_result */
#include "kitty_theme.h"      /* kitty_theme_apply_hooked: dark from the first frame */
#include "kitty_broadcast.h"
/* The hive in use, not the compile-time default - see kitty_storage.c. */

#include "kitty_common.h"
#include "kitty_crypt.h"
#include "kitty_registry.h"
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_text.h"     /* the words this box shows */
#include "kitty_win.h"      /* kitty_dialog_icon: the caption/taskbar icon */
#include "kitty_dlgbox.h"
#include "kitty_storage.h"

/* Provided elsewhere in the KiTTY tree (not in kitty.h). */
extern Conf *conf ;                     /* active-seat global (window.c) */
extern HWND MainHwnd ;                  /* kitty.c: the terminal window */

/* Result of the last completed input dialog (malloc'd; NULL = none). The
 * dialog procs below own it; kitty.c reads it via GetInputBoxResult(). */
static char * InputBoxResult = NULL ;

// Information dialog box
static LRESULT CALLBACK InfoCallBack( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam ) {
	switch(message) {
		case WM_INITDIALOG:
			SetWindowText( GetDlgItem(hwnd,IDC_RESULT), "" ) ;
			kitty_dialog_icon( hwnd, NULL ) ;   /* its owner's icon */
			break;
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

//Callback of the InputBox dialog
static int InputBox_Flag = 0 ;

/* The command box's result line: what the last /command did, under the input
 * field (kitty_commands.c kitty_cmd_result). Made here, not in the template:
 * IDD_INPUTBOX is shared with the Terminal Name and text-prompt boxes, which have
 * nothing to report. A read-only multi-line edit of two lines that wraps, so a
 * long path shows WHOLE and can be selected and copied - never cut to "...".
 * A longer result (/init's six lines) grows it, and the box with it, up to
 * INPUTBOX_STATUS_MAXLINES lines; beyond that it scrolls. A short result
 * takes it back to two. */
#define IDC_INPUTBOX_STATUS 1990
#define INPUTBOX_STATUS_MAXLINES 10

static int inputbox_status_base_h = 0 ;   /* the field's height for two lines, pixels */
static int inputbox_status_line_h = 0 ;   /* one line of its font, pixels */
static int inputbox_status_lines = 2 ;    /* the lines it is sized for now */

static void inputbox_add_status( HWND hwnd ) {
	RECT r, g, wr ;
	HWND st ;
	HDC dc ;
	/* below the input row (it ends at 34 of the template's 50 dialog units):
	 * two lines from 38, the box 22 units taller */
	r.left = 7 ; r.top = 38 ; r.right = 7 + 233 ; r.bottom = 38 + 18 ;
	MapDialogRect( hwnd, &r ) ;
	g.left = g.top = g.right = 0 ; g.bottom = 22 ;
	MapDialogRect( hwnd, &g ) ;
	st = CreateWindowExA( 0, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_MULTILINE |
	                      ES_READONLY | ES_NOHIDESEL,
	                      r.left, r.top, r.right - r.left, r.bottom - r.top,
	                      hwnd, (HMENU)(INT_PTR)IDC_INPUTBOX_STATUS,
	                      GetModuleHandle(NULL), NULL ) ;
	if( !st ) return ;
	SendMessage( st, WM_SETFONT, SendMessage( hwnd, WM_GETFONT, 0, 0 ), FALSE ) ;
	inputbox_status_base_h = r.bottom - r.top ;
	inputbox_status_lines = 2 ;
	inputbox_status_line_h = 0 ;
	if( (dc = GetDC( st )) != NULL ) {
		TEXTMETRICA tm ;
		HGDIOBJ old = SelectObject( dc, (HGDIOBJ)SendMessage( st, WM_GETFONT, 0, 0 ) ) ;
		if( GetTextMetricsA( dc, &tm ) ) inputbox_status_line_h = tm.tmHeight ;
		SelectObject( dc, old ) ;
		ReleaseDC( st, dc ) ;
	}
	GetWindowRect( hwnd, &wr ) ;
	SetWindowPos( hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top + g.bottom,
	              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE ) ;
}

/* The scroll bar only where the text needs it: a two-line result with a
 * dead bar beside it would lose width for nothing. */
static void inputbox_status_scrollbar( HWND st, int on ) {
	LONG_PTR style = GetWindowLongPtr( st, GWL_STYLE ) ;
	RECT cr ;
	if( !!(style & WS_VSCROLL) == !!on ) return ;
	SetWindowLongPtr( st, GWL_STYLE, on ? (style | WS_VSCROLL) : (style & ~(LONG_PTR)WS_VSCROLL) ) ;
	SetWindowPos( st, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
	              SWP_NOZORDER | SWP_NOACTIVATE ) ;
	/* the window kept its size, so no WM_SIZE came: the edit wraps its text
	 * to the client width it learns from one */
	GetClientRect( st, &cr ) ;
	SendMessage( st, WM_SIZE, SIZE_RESTORED, MAKELPARAM( cr.right, cr.bottom ) ) ;
}

static void inputbox_status( HWND hwnd, const char * text ) {
	HWND st = GetDlgItem( hwnd, IDC_INPUTBOX_STATUS ) ;
	const char * s = text ? text : "" ;
	char * buf ;
	size_t i, j, n = 0 ;
	int count, lines ;
	if( !st ) return ;
	/* an edit control breaks a line only at CR LF: a bare LF becomes one */
	for( i = 0 ; s[i] ; i++ ) if( s[i] == '\n' ) n++ ;
	if( (buf = (char *)malloc( strlen( s ) + n + 1 )) == NULL ) { SetWindowTextA( st, s ) ; return ; }
	for( i = j = 0 ; s[i] ; i++ ) {
		if( s[i] == '\n' && (i == 0 || s[i-1] != '\r') ) buf[j++] = '\r' ;
		buf[j++] = s[i] ;
	}
	buf[j] = '\0' ;
	/* counted at the full width, then the bar where it is needed */
	inputbox_status_scrollbar( st, 0 ) ;
	SetWindowTextA( st, buf ) ;
	free( buf ) ;
	count = (int)SendMessage( st, EM_GETLINECOUNT, 0, 0 ) ;
	lines = count < 2 ? 2 : count > INPUTBOX_STATUS_MAXLINES ? INPUTBOX_STATUS_MAXLINES : count ;
	inputbox_status_scrollbar( st, count > INPUTBOX_STATUS_MAXLINES ) ;
	if( lines != inputbox_status_lines && inputbox_status_line_h > 0 && inputbox_status_base_h > 0 ) {
		RECT sr, wr ;
		int dh = ( lines - inputbox_status_lines ) * inputbox_status_line_h ;
		int top, height ;
		HMONITOR mon ;
		MONITORINFO mi ;
		GetWindowRect( st, &sr ) ;
		SetWindowPos( st, NULL, 0, 0, sr.right - sr.left,
		              inputbox_status_base_h + ( lines - 2 ) * inputbox_status_line_h,
		              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE ) ;
		GetWindowRect( hwnd, &wr ) ;
		top = wr.top ;
		height = wr.bottom - wr.top + dh ;
		/* grown, it stays on its screen: moved up when its foot would leave it */
		mon = MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ) ;
		mi.cbSize = sizeof(mi) ;
		if( mon && GetMonitorInfoA( mon, &mi ) && top + height > mi.rcWork.bottom ) {
			top = mi.rcWork.bottom - height ;
			if( top < mi.rcWork.top ) top = mi.rcWork.top ;
		}
		SetWindowPos( hwnd, NULL, wr.left, top, wr.right - wr.left, height,
		              SWP_NOZORDER | SWP_NOACTIVATE ) ;
		inputbox_status_lines = lines ;
	}
}

/* The single-line send-text box is modeless (unlike the multiline and password
 * boxes, which stay modal): it doubles as the /command console, and /help opens
 * its own modeless window, so both must coexist with working Esc/Tab keyboard
 * handling routed by the main message pump. One instance per process. */
static HWND InputBoxHwnd = NULL ;

void kitty_inputbox_result( const char * text ) {
	if( InputBoxHwnd != NULL && IsWindow( InputBoxHwnd ) )
		inputbox_status( InputBoxHwnd, text ) ;
}

static LRESULT CALLBACK InputCallBack(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam ) {
	HWND handle;
	switch (message) {
		case WM_INITDIALOG:
			/* Advertise the internal-command console this box doubles as. */
			if( IniFileFlag == SAVEMODE_DIR ) {
				SetWindowText(hwnd,KT_INPUTBOX_TITLE_PORTABLE);
			} else {
				SetWindowText(hwnd,KT_INPUTBOX_TITLE);
			}
			handle = GetDlgItem(hwnd,IDC_RESULT);
			if( InputBoxResult == NULL ) SetWindowText(handle,"") ;
			else SetWindowText( handle, InputBoxResult ) ;
			kitty_dialog_icon( hwnd, NULL ) ;   /* its owner's icon */
			inputbox_add_status( hwnd ) ;
			/* In the theme from its first frame: the dialog hook themes a
			 * window on its ACTIVATION, so a box shown before it had the
			 * focus came up light and turned dark only once clicked. */
			kitty_theme_apply_hooked( hwnd ) ;
			break;
		case WM_COMMAND:
			if (LOWORD(wParam) == IDOK)
			{
				handle = GetDlgItem(hwnd,IDC_RESULT);
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				size_t length = GetWindowTextLength( handle ) ;
				InputBoxResult = (char*) malloc( length + 10 ) ;
				GetWindowText(handle,InputBoxResult,length+1);
				/* Modeless: OK sends the line and keeps the box open (clear
				 * it and re-focus the edit) so several lines can be sent in a
				 * row, exactly as the old modal box did (it never EndDialog'd
				 * on OK either). */
				if( InternalCommand( hwnd, InputBoxResult ) )
					inputbox_status( hwnd, kitty_cmd_result() ) ;
				else {
					/* "/something" that is no command still goes to the host,
					 * as always - but the line reports it; plain text needs no
					 * word, sending it is what the box is for */
					inputbox_status( hwnd, InputBoxResult[0] == '/' ?
					                 KT_CMD_RES_NOT_A_COMMAND : "" ) ;
					SendKeyboardPlus( MainHwnd, InputBoxResult );
				}
				SetWindowText(handle,"") ;
				SetFocus(handle) ;

			}
			if (LOWORD(wParam) == IDCANCEL)
			{
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				/* no late result into a box on its way out: an open
				 * question it owns answers No as it goes */
				InputBoxHwnd = NULL ;
				DestroyWindow(hwnd);
			}

			break;

		case WM_CLOSE:
			InputBoxHwnd = NULL ;
			DestroyWindow(hwnd);
			break;

		case WM_DESTROY:
			ShinyRemoveAuxDialog(hwnd) ;
			InputBoxHwnd = NULL ;
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
			kitty_dialog_icon( hwnd, NULL ) ;   /* its owner's icon */
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

// Procedure specific to the multiline edit box (SHIFT+F8)
FARPROC lpfnOldEditProc ;
static BOOL FAR PASCAL EditMultilineCallBack(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
		case WM_KEYDOWN:
			if( (wParam==VK_RETURN) && (GetKeyState( VK_SHIFT )& 0x8000) ){
				SendMessage(GetParent(hwnd),WM_COMMAND,IDB_OK,0 ) ;
				return 0;
				}
			/* Shift+F12 / Shift+F11 removed: they scrambled and
			 * unscrambled the text in this box with the constant compiled into
			 * every build, so the "encrypted" result could be read back by anyone
			 * with a copy of KiTTY. Nothing was stored, so there is nothing to
			 * migrate - it was a keystroke that made text look protected. */
			else
				return CallWindowProc((WNDPROC)lpfnOldEditProc, hwnd, message, wParam, lParam);
			break;
		case WM_KEYUP:
		case WM_CHAR:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
			if( (wParam==VK_RETURN) && (GetKeyState( VK_SHIFT )& 0x8000) )
				return 0;
			/* Shift+F2 / Shift+F3 removed: they loaded and saved the session's
			 * separate "Notes" registry value through this box. There is one
			 * note field per session now - the Comment, with its own panel and
			 * its own editor - and an old Notes value is folded into it the
			 * first time the session is loaded or imported
			 * (kitty_merge_legacy_note). */
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
				strcpy( buffer, KT_CAP_KITTY ) ;
			strcat( buffer, KT_INPUTBOX_TITLE_SUFFIX ) ;
			SetWindowText( hwnd, buffer ) ;
			free(buffer);
			/* This one is created with NO owner, so the terminal window has
			 * to be named - it is where the caption above comes from too. */
			kitty_dialog_icon( hwnd, MainHwnd ) ;
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

				// If some text is selected, take only that
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

/* Open (or raise) the modeless single-line send-text / command box. Created on
 * the main thread and owned by the terminal window, registered as an aux dialog
 * so window.c's message pump keeps its Esc/Tab handling working - and so it and
 * the /help window can be open at once. */
void ShowInputBox( HINSTANCE hInstance, HWND hwnd ) {
	if( InputBoxHwnd && IsWindow(InputBoxHwnd) ) { SetForegroundWindow(InputBoxHwnd) ; return ; }
	if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
	InputBoxHwnd = CreateDialog(hInstance, MAKEINTRESOURCE(IDD_INPUTBOX), hwnd, (DLGPROC)InputCallBack) ;
	if( InputBoxHwnd ) {
		ShinyAddAuxDialog( InputBoxHwnd ) ;
		ShowWindow( InputBoxHwnd, SW_SHOW ) ;
		SetForegroundWindow( InputBoxHwnd ) ;
		}
	}

/*
 * "Terminal Name..." (system menu > Window, hknet/KiTTY#54): the send-text
 * box's template (IDD_INPUTBOX) with its own caption and prompt, prefilled
 * with the current name. OK hands the text to kitty_window_name_set (window.c)
 * - empty clears the name - and closes the box; Esc or the close button
 * closes it unchanged. Modeless, one per window, registered as an aux dialog
 * like the send-text box; the dialog hook themes it. A Unicode dialog, so a
 * name is not limited to the ANSI codepage.
 */
#define KITTY_WINNAME_MAXLEN	100
static HWND WinNameBoxHwnd = NULL ;

static INT_PTR CALLBACK WinNameCallBack( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam ) {
	switch( message ) {
		case WM_INITDIALOG: {
			HWND edit = GetDlgItem( hwnd, IDC_RESULT ) ;
			HWND prompt = FindWindowExA( hwnd, NULL, "Static", NULL ) ;
			SetWindowTextA( hwnd, KT_WINNAME_CAPTION ) ;
			if( prompt ) SetWindowTextA( prompt, KT_WINNAME_PROMPT ) ;
			SendMessageW( edit, EM_LIMITTEXT, KITTY_WINNAME_MAXLEN, 0 ) ;
			SetWindowTextW( edit, kitty_window_name_get() ) ;
			SendMessageW( edit, EM_SETSEL, 0, -1 ) ;
			kitty_dialog_icon( hwnd, NULL ) ;   /* its owner's icon */
			SetFocus( edit ) ;
			return FALSE ;                      /* the focus is set */
		}
		case WM_COMMAND:
			if( LOWORD(wParam) == IDOK ) {
				wchar_t name[KITTY_WINNAME_MAXLEN + 1] = L"" ;
				GetWindowTextW( GetDlgItem( hwnd, IDC_RESULT ), name, KITTY_WINNAME_MAXLEN + 1 ) ;
				kitty_window_name_set( name ) ;
				DestroyWindow( hwnd ) ;
				return TRUE ;
			}
			if( LOWORD(wParam) == IDCANCEL ) {
				DestroyWindow( hwnd ) ;
				return TRUE ;
			}
			break ;
		case WM_CLOSE:
			DestroyWindow( hwnd ) ;
			return TRUE ;
		case WM_DESTROY:
			ShinyRemoveAuxDialog( hwnd ) ;
			WinNameBoxHwnd = NULL ;
			break ;
	}
	(void)lParam ;
	return FALSE ;
}

void kitty_winname_box( HWND owner ) {
	if( WinNameBoxHwnd && IsWindow( WinNameBoxHwnd ) ) { SetForegroundWindow( WinNameBoxHwnd ) ; return ; }
	WinNameBoxHwnd = CreateDialogParamW( hinst, MAKEINTRESOURCEW(IDD_INPUTBOX), owner, WinNameCallBack, 0 ) ;
	if( WinNameBoxHwnd ) {
		ShinyAddAuxDialog( WinNameBoxHwnd ) ;
		ShowWindow( WinNameBoxHwnd, SW_SHOW ) ;
		SetForegroundWindow( WinNameBoxHwnd ) ;
	}
}

/*
 * A text question on the same template, for any caller: caption, prompt, the
 * text it starts with (selected), and done(text, ctx) on OK - a Unicode
 * dialog, so a name outside the ANSI code page shows as it is. done returns 1
 * to close the box, 0 to keep it open for another try (it has shown why).
 * Cancel, Esc or closing calls done(NULL, ctx) once, so ctx can be freed
 * there. Modeless, registered as an aux dialog; the dialog hook themes it.
 */
#define KITTY_ASKTEXT_MAXLEN	260
struct kitty_asktext {
	char *caption, *prompt ;
	wchar_t *init ;
	int (*done)( const wchar_t *text, void *ctx ) ;
	void *ctx ;
	int finished ;
} ;

static INT_PTR CALLBACK kitty_asktext_proc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam ) {
	struct kitty_asktext *a = (struct kitty_asktext *)GetWindowLongPtr( hwnd, GWLP_USERDATA ) ;
	switch( message ) {
		case WM_INITDIALOG: {
			HWND edit = GetDlgItem( hwnd, IDC_RESULT ) ;
			HWND prompt = FindWindowExA( hwnd, NULL, "Static", NULL ) ;
			a = (struct kitty_asktext *)lParam ;
			SetWindowLongPtr( hwnd, GWLP_USERDATA, lParam ) ;
			SetWindowTextA( hwnd, a->caption ) ;
			if( prompt ) SetWindowTextA( prompt, a->prompt ) ;
			SendMessageW( edit, EM_LIMITTEXT, KITTY_ASKTEXT_MAXLEN, 0 ) ;
			SetWindowTextW( edit, a->init ? a->init : L"" ) ;
			SendMessageW( edit, EM_SETSEL, 0, -1 ) ;
			kitty_dialog_icon( hwnd, NULL ) ;
			kitty_centre_on_owner( hwnd ) ;
			SetFocus( edit ) ;
			return FALSE ;
		}
		case WM_COMMAND:
			if( !a ) break ;
			if( LOWORD(wParam) == IDOK ) {
				wchar_t text[KITTY_ASKTEXT_MAXLEN + 1] = L"" ;
				GetWindowTextW( GetDlgItem( hwnd, IDC_RESULT ), text, KITTY_ASKTEXT_MAXLEN + 1 ) ;
				if( a->done( text, a->ctx ) ) {
					a->finished = 1 ;
					DestroyWindow( hwnd ) ;
				}
				return TRUE ;
			}
			if( LOWORD(wParam) == IDCANCEL ) {
				DestroyWindow( hwnd ) ;
				return TRUE ;
			}
			break ;
		case WM_CLOSE:
			DestroyWindow( hwnd ) ;
			return TRUE ;
		case WM_DESTROY:
			ShinyRemoveAuxDialog( hwnd ) ;
			if( a ) {
				if( !a->finished ) a->done( NULL, a->ctx ) ;
				free( a->caption ) ; free( a->prompt ) ; free( a->init ) ;
				free( a ) ;
				SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 ) ;
			}
			break ;
	}
	return FALSE ;
}

HWND kitty_ask_text( HWND owner, const char *caption, const char *prompt,
                     const wchar_t *init, int (*done)( const wchar_t *text, void *ctx ),
                     void *ctx ) {
	struct kitty_asktext *a = calloc( 1, sizeof(*a) ) ;
	HWND h ;
	if( !a ) return NULL ;
	a->caption = _strdup( caption ? caption : "" ) ;
	a->prompt = _strdup( prompt ? prompt : "" ) ;
	a->init = _wcsdup( init ? init : L"" ) ;
	a->done = done ; a->ctx = ctx ;
	h = CreateDialogParamW( hinst, MAKEINTRESOURCEW(IDD_INPUTBOX), owner, kitty_asktext_proc, (LPARAM)a ) ;
	if( !h ) {
		free( a->caption ) ; free( a->prompt ) ; free( a->init ) ; free( a ) ;
		return NULL ;
	}
	ShinyAddAuxDialog( h ) ;
	ShowWindow( h, SW_SHOW ) ;
	SetForegroundWindow( h ) ;
	return h ;
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
	/* Modeless now: no InputBox_Flag re-entrancy dance (ShowInputBox raises
	 * the existing window instead of opening a second one) and no worker
	 * thread - it must run on the main thread whose pump routes aux dialogs. */
	ShowInputBox( hinst, hwnd ) ;
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
	InputBoxPassword( hinst, hwnd ) ; // Could be tried with GetModuleHandle(NULL)
	InputBox_Flag = 0 ;
	}

void routine_inputbox( void * phwnd ) { 
	GetAndSendLine( MainHwnd ) ;
	}

void routine_inputbox_multiline( void * phwnd ) { 
	GetAndSendMultiLine( MainHwnd ) ;
	}

/* Seam accessor: the encrypted-kitty.sav password check (kitty_regbackup.c) reads the
 * password typed into the GetAndSendLinePassword dialog. */
char * GetInputBoxResult( void ) {
	return InputBoxResult ;
	}
