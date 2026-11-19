/*
 * kitty_demotpl.c - kitty.exe's -demo-templates walk (the template
 * dialogs as authored, for review). Split out of kitty_dlgbox.c, whose
 * boxes kageant links too.
 */
#include "kitty_dlgbox.h"
#include "kitty_rc_additions.h"
#include "../windows/putty-rc.h"   /* the shared dialog ids */
#include "kitty_text.h"
#include "kitty_buildlabel.h"

#ifdef KITTY_TEST_BUILD_LABEL

/*
 * -demo-templates: page through every template dialog AS AUTHORED - real
 * resource, real font, no dlgproc filling anything in - so template spacing
 * can be reviewed by eye without arranging each window's live trigger
 * (a changed host key, a passphrase save, an update offer...). Esc, Enter or
 * closing advances to the next. The caption shows the template. Review
 * tooling, reached only via the explicit command-line flag.
 */
static INT_PTR CALLBACK kitty_demo_tpl_proc( HWND h, UINT msg, WPARAM wp,
                                             LPARAM lp ) {
	switch( msg ) {
	  case WM_INITDIALOG:
		if( lp ) SetWindowTextA( h, (const char *)lp ) ;
		return TRUE ;
	  case WM_COMMAND:
		switch( LOWORD(wp) ) {
		  case IDOK: case IDCANCEL: case IDYES: case IDNO:
			EndDialog( h, 0 ) ; return TRUE ;
		}
		return FALSE ;
	  case WM_CLOSE: EndDialog( h, 0 ) ; return TRUE ;
	}
	return FALSE ;
}

void kitty_demo_templates( void ) {
	static const struct { const char *name ; int id ; } tpls[] = {
		{ "IDD_HOSTKEY (security alert)",   IDD_HOSTKEY },
		{ "IDD_HK_MOREINFO",                IDD_HK_MOREINFO },
		{ "IDD_CONFIRMBOX (confirm/info)",  IDD_CONFIRMBOX },
		{ "IDD_NOTICEBOX",                  IDD_NOTICEBOX },
		{ "IDD_INFOBOX",                    IDD_INFOBOX },
		{ "IDD_LOGBOX (event log)",         IDD_LOGBOX },
		{ "IDD_ABOUTBOX",                   IDD_ABOUTBOX },
		{ "IDD_LICENCEBOX",                 IDD_LICENCEBOX },
		{ "IDD_KITTYABOUT",                 IDD_KITTYABOUT },
		{ "IDD_TITLEVARS",                  IDD_TITLEVARS },
		{ "IDD_UPDATEBOX",                  IDD_UPDATEBOX },
		{ "IDD_INPUTBOX",                   IDD_INPUTBOX },
		{ "IDD_INPUTBOXMULTI",              IDD_INPUTBOXMULTI },
		{ "IDD_INPUTBOXPW",                 IDD_INPUTBOXPW },
		{ "IDD_MASTERPW",                   IDD_MASTERPW },
		{ "IDD_EXPORTPW",                   IDD_EXPORTPW },
		{ "IDD_EXPORTDONE",                 IDD_EXPORTDONE },
		{ "IDD_IMPORTPW",                   IDD_IMPORTPW },
		{ "IDD_MPWMOVED",                   IDD_MPWMOVED },
		{ "IDD_OSC52READ",                  IDD_OSC52READ },
		{ "IDD_HELPBOX",                    IDD_HELPBOX },
		{ "IDD_GETFILEBOX",                 IDD_GETFILEBOX },
		{ "IDD_SEARCHBAR",                  IDD_SEARCHBAR },
	} ;
	size_t i ;
	/* IDD_HOSTKEY refers to a window CLASS that ShinyDialogBox normally
	 * registers. A bare DefDlgProc registration is all the gallery needs. */
	{
		WNDCLASSA wc ;
		memset( &wc, 0, sizeof(wc) ) ;
		wc.lpfnWndProc = DefDlgProcA ;
		wc.cbWndExtra = DLGWINDOWEXTRA ;
		wc.hInstance = GetModuleHandle(NULL) ;
		wc.lpszClassName = "PuTTYHostKeyDialog" ;
		RegisterClassA( &wc ) ;   /* already registered = fine */
	}
	for( i = 0 ; i < sizeof(tpls)/sizeof(tpls[0]) ; i++ )
		DialogBoxParamA( GetModuleHandle(NULL),
			MAKEINTRESOURCEA( tpls[i].id ), NULL,
			kitty_demo_tpl_proc, (LPARAM)tpls[i].name ) ;
}
#endif /* KITTY_TEST_BUILD_LABEL */
