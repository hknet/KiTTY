/*
 * The KiTTY side of the built-in mNotepad editor, included by notepad.c when
 * it is compiled with NOMAIN. It finds the KiTTY or PuTTY window the editor
 * was started from, sends the current line or the selection to that window
 * (or to every KiTTY window) as keystrokes, with the line delimiter chosen in
 * the Delimiter menu, normalises the edit control to CRLF, and lays the
 * windows out: the editor under its session, or all KiTTY windows tiled in a
 * grid or cascaded.
 */
#include <time.h>
#include <math.h>
#include "../kitty.h"

HWND ParentWindow = NULL ;		// HWND of the parent window


void TestIfParentIsKiTTY( void ) {
	ParentWindow = GetForegroundWindow() ;
	char buffer[256] ;
	GetClassName( ParentWindow, buffer, 256 ) ;
	if( strcmp( buffer, "KiTTY" ) && strcmp( buffer, "PuTTY" ) ) ParentWindow = NULL ;
		//SendMessage(ParentWindow, WM_CHAR, buffer[i], 0) ; 
	}

void ActiveParent( void ) { if( IsIconic( ParentWindow ) ) ShowWindow(ParentWindow,SW_RESTORE) ; }
	
static void Send2Window( HWND SrcWnd, HWND TargetWnd, char * buffer ) {
	int i ;
	if( (buffer!=NULL) && (strlen( buffer) > 0 ) ) {
		if( IsIconic( TargetWnd ) ) { 
			ShowWindow( TargetWnd, SW_RESTORE ) ;
			for( i=0; i< strlen( buffer ) ; i++ ) 
				if( buffer[i] == '\n' ) { SendMessage( TargetWnd, WM_KEYDOWN, VK_RETURN, 0 ) ; }
				else if( buffer[i] == '\r' ) { }
				else SendMessage( TargetWnd, WM_CHAR, buffer[i], 0 ) ;
			if( SrcWnd!=NULL ) { SetForegroundWindow( SrcWnd ) ; }
		} else {
			BringWindowToTop( TargetWnd ) ;
			for( i=0; i< strlen( buffer ) ; i++ ) 
				if( buffer[i] == '\n' ) { SendMessage( TargetWnd, WM_KEYDOWN, VK_RETURN, 0 ) ; }
				else if( buffer[i] == '\r' ) { }
				else SendMessage( TargetWnd, WM_CHAR, buffer[i], 0 ) ;
			if ( SrcWnd!=NULL ) { BringWindowToTop( SrcWnd ) ; }
		}
	}
}
	
static void Send2Parent( HWND ChildWnd , char * buffer ) {
	if( (ParentWindow != NULL) && (buffer!=NULL) ) {
		Send2Window( ChildWnd, ParentWindow, buffer ) ;
	}
}

void InitKiTTYNotepad( HWND hwnd ) {
	if( ParentWindow!=NULL ) {
		int pw=0, ph=0, px=0, py=0 ;
		RECT Rect ;
		GetWindowRect( ParentWindow, &Rect ) ;
		px=Rect.left+20 ; 
		py=Rect.top+20 ;
		pw=Rect.right-Rect.left+1 ;
		ph=Rect.bottom-Rect.top+1 ;
		SetWindowPos(hwnd, NULL, px, py, pw, ph, SWP_NOZORDER) ;
		}
	}

static int CRLF_flag = 1 ;
static int Semic_flag = 1 ;
static int Slash_flag = 0 ;
static int Tilde_flag = 0 ;
	
// Get the contents of the Edit window
/* The editor's text in full, in memory of its own (free it), ended after
 * the current line or the selection; *start = where that begins. Two bytes
 * of room beyond the text for the delimiter and the newline added here: a
 * text of 32000 bytes or more overran the old fixed buffer. NULL = no
 * memory. */
static char * GetEditContent( HWND hWndEdit, int * start ) {
	int d = 0, f = 0, i, len ;
	char * buffer ;
	char CharLim = '\n' ;

	if( Semic_flag ) CharLim = ';' ;
	if( Slash_flag ) CharLim = '/' ;
	if( Tilde_flag ) CharLim = '~' ;
	if( CRLF_flag ) CharLim = '\n' ;

	len = GetWindowTextLength( hWndEdit ) ;
	if( len < 0 ) len = 0 ;
	if( ( buffer = (char *)malloc( (size_t)len + 3 ) ) == NULL ) return NULL ;
	buffer[0] = '\0' ;
	len = GetWindowText( hWndEdit, buffer, len + 1 ) ;
	if( len < 0 ) len = 0 ;
	buffer[len] = '\0' ;
	SendMessage( hWndEdit, EM_GETSEL, (WPARAM)&d, (LPARAM)&f ) ;
	if( d < 0 ) d = 0 ;
	if( d > len ) d = len ;
	if( f < d ) f = d ;
	if( f > len ) f = len ;

	if( d==f ) {
		while( (d>0)&&(buffer[d-1]!=CharLim) ) d-- ;
		while( (buffer[f]!=CharLim)&&(buffer[f]!='\0') ) f++ ;

		if( f == 0 || buffer[f-1] != CharLim ) { buffer[f] = CharLim ; buffer[f+1] = '\0' ; f++ ; }
		else { buffer[f] = '\0' ; }

		SendMessage( hWndEdit, EM_SETSEL, (WPARAM)d, (LPARAM)f ) ;
		}
	else buffer[f] = '\0' ;

	if( Slash_flag ) if( f > 0 && buffer[f-1]=='/' ) {
		i = f-2 ;
		while( (i>0)&&( (buffer[i]==' ')||(buffer[i]=='	')||(buffer[i]=='\n')||(buffer[i]=='\r') ) ) i-- ;
		if( (i>0) && (buffer[i]==';') ) buffer[f-1]=' ' ;
		}

	if( CRLF_flag && buffer[0] && (buffer[strlen(buffer)-1]!='\n') ) strcat( buffer, "\n" ) ;
	*start = d ;
	return buffer ;
}

/* Lone CR and lone LF become CRLF; the text in full (each byte at most
 * doubles), where a fixed 32000-byte buffer cut it and the insertions ran
 * past its end. */
void ChangeToCRLF( HWND hWndEdit ) {
	int len = GetWindowTextLength( hWndEdit ), i, n = 0 ;
	char *in, *out ;
	if( len <= 0 ) return ;
	if( ( in = (char *)malloc( (size_t)len + 1 ) ) == NULL ) return ;
	len = GetWindowText( hWndEdit, in, len + 1 ) ;
	if( len > 0 && ( out = (char *)malloc( 2 * (size_t)len + 1 ) ) != NULL ) {
		in[len] = '\0' ;
		for( i = 0 ; i < len ; i++ ) {
			if( in[i] == '\n' && ( i == 0 || in[i-1] != '\r' ) ) out[n++] = '\r' ;
			out[n++] = in[i] ;
			if( in[i] == '\r' && in[i+1] != '\n' ) out[n++] = '\n' ;
		}
		out[n] = '\0' ;
		SetWindowText( hWndEdit, out ) ;
		free( out ) ;
	}
	free( in ) ;
}

// Send a string to the parent window
void SendStrToParent( HWND hWndEdit ) {
	int d = 0 ;
	char * buffer = GetEditContent( hWndEdit, &d ) ;

	if( buffer != NULL && strlen(buffer)>0 ) {
		Send2Parent( GetParent( hWndEdit ), buffer+d ) ;
		SetFocus( hWndEdit ) ;
		SendMessage( hWndEdit, EM_SETSEL, (WPARAM)d, (LPARAM)d ) ;
		}
	free( buffer ) ;
	}

// Functions to send a string to all KiTTY windows
static BOOL CALLBACK SendToAllProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	GetClassName( hwnd, buffer, 256 ) ;
	if( !strcmp( buffer, get_param_str("CLASS") ) )
		Send2Window( NULL, hwnd, (char*)lParam ) ;
	return TRUE ;
	}

void SendStrToAll( HWND hWndEdit ) {
	int d = 0 ;
	char * buffer = GetEditContent( hWndEdit, &d ) ;

	if( buffer != NULL && strlen(buffer)>0 ) {
		EnumWindows( SendToAllProc, (LPARAM)(buffer+d) ) ;
		SetFocus( hWndEdit ) ;
		SendMessage( hWndEdit, EM_SETSEL, (WPARAM)d, (LPARAM)d ) ;
	}
	free( buffer ) ;
}

void SetWindowsSize( HWND hwnd ) {
	//int w=GetSystemMetrics(SM_CXSCREEN), h=GetSystemMetrics(SM_CYSCREEN) ;
	int w=GetSystemMetrics(SM_CXFULLSCREEN), h=GetSystemMetrics(SM_CYFULLSCREEN) ;

	//char buffer[256];
	//sprintf( buffer, "%d %d %d %d",w,h,GetSystemMetrics(SM_CXFULLSCREEN),GetSystemMetrics(SM_CYFULLSCREEN));
	//MessageBox(hwnd, buffer,"Info",MB_OK);
	
	//SetWindowPos(ParentWindow, NULL, 0, 0, w, (int)(h/2.), SWP_NOZORDER) ;
	//SetWindowPos(hwnd, NULL, 0, (int)(h/2.)+1, w, h, SWP_NOZORDER) ;
	
	MoveWindow( ParentWindow, 0, 0, w, (int)(h/2.), TRUE );
	MoveWindow( hwnd, 0, (int)(h/2.)+1, w, (int)(h/2.), TRUE );
	}

// Count the KiTTY windows
static int nbProgWin = 0 ;
static BOOL CALLBACK CountProgWinProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	GetClassName( hwnd, buffer, 256 ) ;
	if( !strcmp( buffer, get_param_str("CLASS") ) )	nbProgWin++ ;
	return TRUE ;
	}

static int CountProgWin( HWND hwnd ) {
	nbProgWin=0 ;
	EnumWindows( CountProgWinProc, 0 ) ;
	return nbProgWin ;
	}

// Resize all the windows
static BOOL CALLBACK ResizeAllWindowsProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	int cxScreen, cyScreen ;
	//cxScreen = GetSystemMetrics (SM_CXFULLSCREEN);
	//cyScreen = GetSystemMetrics (SM_CYFULLSCREEN);
	cxScreen = GetSystemMetrics (SM_CXSCREEN);
	cyScreen = GetSystemMetrics (SM_CYSCREEN)-40 ;
	int nbCol = floor(sqrt(nbProgWin));
	int nbLig = ceil(1.0*nbProgWin/nbCol) ;
	int *i=(int*)lParam;
	int larg, haut;
	larg = (int)(cxScreen/nbCol) ;
	haut = (int)(cyScreen/nbLig) ;
	
	GetClassName( hwnd, buffer, 256 ) ;
	if( !strcmp( buffer, get_param_str("CLASS") ) ) {
		int num=nbProgWin-*i;
		int px=0, py=0 ;
		if( num>0 ) {
			int j ;
			for( j=1 ; j<=num ; j++ ) {
				px++ ;
				if( px>= nbCol ) { py++ ; px=0 ; }
			}
		}
//char b[256];sprintf(b,"nbProgWin=%d nbCol=%d nbLig=%d num=%d px=%d py=%d",nbProgWin,nbCol,nbLig,num,px,py);MessageBox(NULL,b,"info",MB_OK);
		MoveWindow( hwnd, px*larg, py*haut, larg, haut, TRUE ) ;
		SetForegroundWindow(hwnd); ;
		*i=*i-1;
	}
	return TRUE;
}
void ResizeAllWindows( HWND hwnd ) {
	int i;
	i=CountProgWin( hwnd ) ;
	EnumWindows( ResizeAllWindowsProc,(LPARAM)&i) ;
}

// Cascade all the KiTTY windows
static BOOL CALLBACK CascadeAllWindowsProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	int cxScreen, cyScreen ;
	cxScreen = GetSystemMetrics (SM_CXSCREEN);
	cyScreen = GetSystemMetrics (SM_CYSCREEN)-40 ;
	int *i=(int*)lParam;
	int larg, haut, dec=30;
	larg = (int)(cxScreen-dec*nbProgWin) ;
	haut = (int)(cyScreen-dec*nbProgWin) ;
	
	GetClassName( hwnd, buffer, 256 ) ;
	if( !strcmp( buffer, get_param_str("CLASS") ) ) {
		int num=nbProgWin-*i;
		MoveWindow( hwnd, num*dec, num*dec, larg, haut, TRUE ) ;
		SetForegroundWindow(hwnd); ;
		*i=*i-1;
	}
	return TRUE;
}
void CascadeAllWindows( HWND hwnd ) {
	int i;
	i=CountProgWin( hwnd ) ;
	EnumWindows( CascadeAllWindowsProc,(LPARAM)&i) ;
}
