/*
 * kitty_dlgbox.c - the suite's themed message, notice and confirmation boxes
 * (kitty_msgbox.h routes a file's plain MessageBox calls here), and the
 * dialog helpers every KiTTY window shares: fit-to-text, centre on owner,
 * centre in parent, the dialog icon. The demo-templates walk of a test build
 * lives here too, since it exercises these dialogs.
 */
#include "kitty_dlgbox.h"
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

// Centre a dialog in the middle of its parent window
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

/*
 * Generic notice box: a caption, a block of text, and OK.
 *
 * A real dialog rather than MessageBox because it must GROW TO FIT a long
 * explanation instead of clipping it, and because it should look like the rest of
 * KiTTY.
 *
 * NOT for DPI reasons, whatever this comment used to say. A MessageBox is drawn
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
	int info ;           /* 1 = one OK button instead of Yes/No */
	int defyes ;         /* 1 = Yes is the default (close-confirm keeps
	                      * Enter meaning close); everything else stays No */
	int three ;          /* 1 = 3-way mode: the optional third button is shown
	                      * and laid out. 0 = Yes/No or info, UNCHANGED. */
	const char *b_over ; /* 3-way: the default button (IDYES),  e.g. Overwrite */
	const char *b_keep ; /* 3-way: the third button (IDC_CONFIRM_THIRD), Keep both */
	const char *b_cancel;/* 3-way: the No button (IDNO), Cancel */
} kitty_confirm_t ;

/* Grow one text control to fit its text at the DIALOG's font, offset by extra_dy,
 * and return the height change in pixels. Same measure-then-move approach the
 * notice box uses, so neither hand-rolls DPI scaling. */
int kitty_fit_text( HWND dlg, int ctlid, const char *text, int extra_dy ) {
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

/*
 * Put a modal dialog over the window that raised it. DS_CENTER in a template
 * centres on the SCREEN and ignores the owner, so a confirmation raised from
 * the configuration box on a wide monitor landed mid-screen while the box sat
 * at one side. Called from WM_INITDIALOG, after the dialog has its final
 * size. Does nothing without a visible owner (the template's own placement
 * then stands); the result is kept inside the owner's monitor.
 */
/*
 * Give a dialog the icon of the window that raised it.
 *
 * A dialog with no icon of its own gets the system's generic one in its
 * caption and a blank in the taskbar and in Alt-Tab. The icon to take is the
 * OWNER's rather than the application's, because a session can carry an icon
 * of its own ([KiTTY] Icone / IconeFile, put on the terminal window by
 * SetNewIcon) - and a question raised by that session should be recognisable
 * as belonging to it.
 *
 * Big and small are asked for separately: they are different bitmaps at
 * different sizes, the caption uses the small one and the taskbar the big
 * one, and a window that has only one of them must not be made to stretch it.
 * What the owner does not have falls back to its window class's icon and then
 * to the application's own, so a dialog with no owner still gets a proper one.
 *
 * One function, not a copy per window: this is the third place in the suite
 * that needed it.
 */
#ifndef IDI_MAINICON
#define IDI_MAINICON 200   /* windows/putty-rc.h, when not included first */
#endif
void kitty_dialog_icon( HWND dlg, HWND owner ) {
	HICON big = NULL, small = NULL ;
	HINSTANCE inst = GetModuleHandle( NULL ) ;
	if( !dlg ) return ;
	if( !owner ) owner = GetWindow( dlg, GW_OWNER ) ;
	if( owner ) {
		big = (HICON)SendMessage( owner, WM_GETICON, ICON_BIG, 0 ) ;
		small = (HICON)SendMessage( owner, WM_GETICON, ICON_SMALL, 0 ) ;
		if( !big ) big = (HICON)(LONG_PTR)GetClassLongPtr( owner, GCLP_HICON ) ;
		if( !small ) small = (HICON)(LONG_PTR)GetClassLongPtr( owner, GCLP_HICONSM ) ;
	}
	if( !big ) big = (HICON)LoadImage( inst, MAKEINTRESOURCE(IDI_MAINICON),
		IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
		LR_SHARED ) ;
	if( !small ) small = (HICON)LoadImage( inst, MAKEINTRESOURCE(IDI_MAINICON),
		IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
		LR_SHARED ) ;
	if( big ) SendMessage( dlg, WM_SETICON, ICON_BIG, (LPARAM)big ) ;
	if( small ) SendMessage( dlg, WM_SETICON, ICON_SMALL, (LPARAM)small ) ;
}

void kitty_centre_on_owner( HWND dlg ) {
	HWND owner = GetWindow( dlg, GW_OWNER ) ;
	RECT o, d, wa ;
	MONITORINFO mi ;
	int x, y, w, hgt ;
	if( !owner || !IsWindowVisible( owner ) || IsIconic( owner ) ) return ;
	if( !GetWindowRect( owner, &o ) || !GetWindowRect( dlg, &d ) ) return ;
	w = d.right - d.left ; hgt = d.bottom - d.top ;
	x = o.left + ((o.right - o.left) - w) / 2 ;
	y = o.top + ((o.bottom - o.top) - hgt) / 2 ;
	mi.cbSize = sizeof(mi) ;
	if( GetMonitorInfo( MonitorFromWindow( owner, MONITOR_DEFAULTTONEAREST ), &mi ) ) {
		wa = mi.rcWork ;
		if( x + w > wa.right ) x = wa.right - w ;
		if( y + hgt > wa.bottom ) y = wa.bottom - hgt ;
		if( x < wa.left ) x = wa.left ;
		if( y < wa.top ) y = wa.top ;
	}
	SetWindowPos( dlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE ) ;
}

static INT_PTR CALLBACK kitty_confirm_dlgproc( HWND h, UINT msg, WPARAM wp, LPARAM lp ) {
	static const kitty_confirm_t *cf = NULL ;
	switch( msg ) {
	  case WM_INITDIALOG: {
		int d1, d2, dh, id ;
		cf = (const kitty_confirm_t *)lp ;
		if( cf && cf->caption ) SetWindowTextA( h, cf->caption ) ;
		if( cf && cf->info ) {
			/* Info mode: one OK where Yes/No stand. The No button is the
			 * template's default, so relabelling THAT one keeps Return
			 * meaning "acknowledged". */
			ShowWindow( GetDlgItem( h, IDYES ), SW_HIDE ) ;
			SetDlgItemTextA( h, IDNO, KT_WIN_OK ) ;
		}
		if( cf && cf->three ) {
			/* 3-way mode: label the three buttons and reveal the optional
			 * third. IDYES=Overwrite (default), IDC_CONFIRM_THIRD=Keep both,
			 * IDNO=Cancel. Laid out below, after the text-fit shift. */
			SetDlgItemTextA( h, IDYES, cf->b_over ? cf->b_over : "" ) ;
			SetDlgItemTextA( h, IDNO,  cf->b_cancel ? cf->b_cancel : "" ) ;
			SetDlgItemTextA( h, IDC_CONFIRM_THIRD, cf->b_keep ? cf->b_keep : "" ) ;
			ShowWindow( GetDlgItem( h, IDC_CONFIRM_THIRD ), SW_SHOW ) ;
		} else {
			/* Every existing caller: the third button never appears. */
			ShowWindow( GetDlgItem( h, IDC_CONFIRM_THIRD ), SW_HIDE ) ;
		}
		SetDlgItemTextA( h, IDC_CONFIRM_TEXT, cf && cf->text ? cf->text : "" ) ;
		SetDlgItemTextA( h, IDC_CONFIRM_WARN, cf && cf->warn ? cf->warn : "" ) ;
		if( cf && cf->warn && *cf->warn )
			kitty_theme_mark_ink( GetDlgItem( h, IDC_CONFIRM_WARN ),
				KITTY_INK_BAD ) ;   /* the warning line, and only it */
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
			if( cf && cf->three ) {          /* the third button drops with the row */
				HWND b = GetDlgItem( h, IDC_CONFIRM_THIRD ) ;
				RECT br ; GetWindowRect( b, &br ) ; MapWindowPoints( NULL, h, (POINT*)&br, 2 ) ;
				MoveWindow( b, br.left, br.top + dh, br.right-br.left, br.bottom-br.top, TRUE ) ;
			}
			{ RECT wr ; GetWindowRect( h, &wr ) ;
			  SetWindowPos( h, NULL, 0, 0, wr.right-wr.left,
				(wr.bottom-wr.top)+dh, SWP_NOMOVE|SWP_NOZORDER ) ; }
		}
		if( cf && cf->three ) {
			/* Size each button to its text (the shared helper) and lay the
			 * three out right-aligned: Overwrite(default) | Keep both | Cancel.
			 * Widen the dialog if the template width cannot hold them. Then the
			 * default is IDYES and focus goes there; the 2-way logic below is
			 * skipped, so Yes/No and info boxes are untouched. */
			HWND bo = GetDlgItem( h, IDYES ), bk = GetDlgItem( h, IDC_CONFIRM_THIRD ), bc = GetDlgItem( h, IDNO ) ;
			RECT rc ; GetClientRect( h, &rc ) ;
			RECT ro ; GetWindowRect( bo, &ro ) ; MapWindowPoints( NULL, h, (POINT*)&ro, 2 ) ;
			int by = ro.top, bh = ro.bottom - ro.top ;
			int wo = kitty_theme_button_width( bo, 50 ) ;
			int wk = kitty_theme_button_width( bk, 50 ) ;
			int wc = kitty_theme_button_width( bc, 50 ) ;
			int gap = 6, margin = 10 ;
			int total = wo + wk + wc + 2*gap + 2*margin ;
			int cw = rc.right - rc.left ;
			if( total > cw ) {
				RECT wr ; GetWindowRect( h, &wr ) ;
				SetWindowPos( h, NULL, 0, 0, (wr.right-wr.left) + (total-cw),
					(wr.bottom-wr.top), SWP_NOMOVE|SWP_NOZORDER ) ;
				GetClientRect( h, &rc ) ; cw = rc.right - rc.left ;
			}
			int xC = cw - margin - wc, xK = xC - gap - wk, xO = xK - gap - wo ;
			MoveWindow( bc, xC, by, wc, bh, TRUE ) ;
			MoveWindow( bk, xK, by, wk, bh, TRUE ) ;
			MoveWindow( bo, xO, by, wo, bh, TRUE ) ;
			SendMessage( h, DM_SETDEFID, IDYES, 0 ) ;
			SendDlgItemMessage( h, IDNO, BM_SETSTYLE, BS_PUSHBUTTON, TRUE ) ;
			SendDlgItemMessage( h, IDC_CONFIRM_THIRD, BM_SETSTYLE, BS_PUSHBUTTON, TRUE ) ;
			SendDlgItemMessage( h, IDYES, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE ) ;
			SetFocus( bo ) ;
			kitty_centre_on_owner( h ) ;
			return FALSE ;
		}
		if( cf && cf->defyes && !cf->info ) {
			SendMessage( h, DM_SETDEFID, IDYES, 0 ) ;
			SendDlgItemMessage( h, IDNO, BM_SETSTYLE, BS_PUSHBUTTON, TRUE ) ;
			SendDlgItemMessage( h, IDYES, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE ) ;
			SetFocus( GetDlgItem( h, IDYES ) ) ;
		} else
			SetFocus( GetDlgItem( h, IDNO ) ) ;
		kitty_centre_on_owner( h ) ;       /* over the window that asked, not mid-screen */
		return FALSE ;                     /* focus set here, not by the manager */
	  }
	  /* The warning line's colour is not answered here: it is marked above
	   * and painted by the theme engine, which is the only handler a dark
	   * window ever reaches (kitty_theme_mark_ink). */
	  case WM_COMMAND:
		switch( LOWORD(wp) ) {
		  case IDYES: EndDialog( h, 1 ) ; return TRUE ;   /* 3-way: Overwrite */
		  case IDC_CONFIRM_THIRD: EndDialog( h, 2 ) ; return TRUE ;  /* 3-way: Keep both */
		  case IDOK:
			/* Only the info dress has an OK to press (drivers that used to
			 * answer a MessageBox send IDOK); on a real question OK must
			 * not silently mean Yes. */
			if( cf && cf->info ) { EndDialog( h, 0 ) ; return TRUE ; }
			return FALSE ;
		  case IDNO:
		  case IDCANCEL: EndDialog( h, 0 ) ; return TRUE ;
		}
		return FALSE ;
	  case WM_CLOSE: EndDialog( h, 0 ) ; return TRUE ;   /* closing means No */
	}
	return FALSE ;
}

/* If the dialog cannot be created at all (-1: out of resources, or so early or
 * so broken that no template loads), fall back to a plain MessageBox with the
 * same words - a fatal error must never pass unshown. */
static INT_PTR kitty_confirm_run( HWND owner, const kitty_confirm_t *cf ) {
	INT_PTR r = DialogBoxParamA( GetModuleHandle(NULL),
		MAKEINTRESOURCEA(IDD_CONFIRMBOX), owner, kitty_confirm_dlgproc,
		(LPARAM)cf ) ;
	if( r == -1 ) {
		char *joined = NULL ;
		const char *body = cf->text ? cf->text : "" ;
		if( cf->warn && cf->warn[0] ) {
			joined = malloc( strlen(body) + strlen(cf->warn) + 3 ) ;
			if( joined ) sprintf( joined, "%s\n\n%s", body, cf->warn ) ;
		}
		if( cf->three ) {
			/* No themed template: a plain 3-button box. Yes=Overwrite(1),
			 * No=Keep both(2), Cancel=0 - the same three answers. */
			int mb = MessageBoxA( owner, joined ? joined : body, cf->caption,
				MB_YESNOCANCEL|MB_ICONWARNING|MB_DEFBUTTON1 ) ;
			r = ( mb==IDYES ) ? 1 : ( mb==IDNO ) ? 2 : 0 ;
		} else
		r = ( MessageBoxA( owner, joined ? joined : body, cf->caption,
			cf->info ? (MB_OK|MB_ICONINFORMATION)
			         : (MB_YESNO|MB_ICONWARNING|
			            (cf->defyes ? MB_DEFBUTTON1 : MB_DEFBUTTON2)) )
			== IDYES ) ? 1 : 0 ;
		if( joined ) free( joined ) ;
	}
	return r ;
}

/* True only if Yes was pressed. No, Escape and closing the box all mean no, which
 * is the safe reading of every one of them. */
int kitty_confirm_box( HWND owner, const char *caption, const char *text,
                       const char *warn_red ) {
	kitty_confirm_t cf = {0} ;
	cf.caption = caption ; cf.text = text ; cf.warn = warn_red ;
	cf.info = 0 ; cf.defyes = 0 ;
	return kitty_confirm_run( owner, &cf ) == 1 ;
}

/* The same question with Yes as the default: for confirmations where Enter has
 * always meant "go ahead" (closing a window) and must keep meaning that. */
int kitty_confirm_box_yes( HWND owner, const char *caption, const char *text,
                           const char *warn_red ) {
	kitty_confirm_t cf = {0} ;
	cf.caption = caption ; cf.text = text ; cf.warn = warn_red ;
	cf.info = 0 ; cf.defyes = 1 ;
	return kitty_confirm_run( owner, &cf ) == 1 ;
}

/* A three-way choice on the SAME shared template: the optional third button is
 * shown, the other two relabelled. Returns 1 = first/default (over), 2 = third
 * (keep), 0 = second/Cancel/Escape/close. Every existing Yes/No and info caller
 * is unaffected (they leave `three` zero). */
int kitty_confirm_box3( HWND owner, const char *caption, const char *text,
                        const char *b_over, const char *b_keep, const char *b_cancel ) {
	kitty_confirm_t cf = {0} ;
	cf.caption = caption ; cf.text = text ; cf.warn = NULL ;
	cf.info = 0 ; cf.defyes = 0 ; cf.three = 1 ;
	cf.b_over = b_over ; cf.b_keep = b_keep ; cf.b_cancel = b_cancel ;
	return (int)kitty_confirm_run( owner, &cf ) ;
}

/* The MessageBox shapes the suite actually uses, in the themed dress: MB_OK
 * becomes the info box, MB_YESNO the confirm box with the site's own default
 * button kept. Anything else - a three-way choice, a system-modal fatal - is
 * not imitated and goes to the real MessageBox unchanged. Call sites reach
 * this through kitty_msgbox.h without being edited. */
int kitty_message_box( HWND owner, const char *text, const char *caption,
                       unsigned type ) {
	unsigned btns = type & MB_TYPEMASK ;
	if( !(type & (MB_SYSTEMMODAL|MB_TASKMODAL)) ) {
		if( btns == MB_OK ) {
			kitty_info_box( owner, caption, text, NULL ) ;
			return IDOK ;
		}
		if( btns == MB_YESNO ) {
			int yes = ( (type & MB_DEFMASK) == MB_DEFBUTTON2 )
				? kitty_confirm_box( owner, caption, text, NULL )
				: kitty_confirm_box_yes( owner, caption, text, NULL ) ;
			return yes ? IDYES : IDNO ;
		}
	}
	return MessageBoxA( owner, text, caption, type ) ;
}

/* The same themed box carrying an announcement rather than a question: one
 * OK, no choice. What MessageBox did, in the suite's own dress. */
void kitty_info_box( HWND owner, const char *caption, const char *text,
                     const char *warn_red ) {
	kitty_confirm_t cf = {0} ;
	cf.caption = caption ; cf.text = text ; cf.warn = warn_red ;
	cf.info = 1 ; cf.defyes = 0 ;
	kitty_confirm_run( owner, &cf ) ;
}

#ifdef KITTY_TEST_BUILD_LABEL
/*
 * -demo-templates: page through every template dialog AS AUTHORED - real
 * resource, real font, no dlgproc filling anything in - so template spacing
 * can be reviewed by eye without arranging each window's live trigger
 * (a changed host key, a passphrase save, an update offer...). Esc, Enter or
 * closing advances to the next; the caption names the template. Review
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
		{ "IDD_MIGRATEWARN",                IDD_MIGRATEWARN },
		{ "IDD_OSC52READ",                  IDD_OSC52READ },
		{ "IDD_HELPBOX",                    IDD_HELPBOX },
	} ;
	size_t i ;
	/* IDD_HOSTKEY names a window CLASS that ShinyDialogBox normally
	 * registers; a bare DefDlgProc registration is all the gallery needs. */
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
