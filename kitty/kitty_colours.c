/*
 * KiTTY colour / font / background-viewer runtime toggles, moved verbatim out
 * of kitty.c to shrink that monolith. These operate on the active-seat global
 * Conf and drive a reconfigure; behaviour is unchanged from when they lived in
 * kitty.c. Compiled into the same target as kitty.c, so the MOD_TUTTYCOLOR /
 * MOD_BACKGROUNDIMAGE guards resolve identically.
 */
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "terminal.h"

#include <windows.h>

#include "kitty.h"

/* Provided elsewhere in the KiTTY tree (not in kitty.h). */
extern Conf *conf;                       /* active-seat global (window.c) */
void ResetWindow(int reinit);            /* window.c */
void set_title(TermWin *tw, const char *title);  /* kitty.c */

void NegativeColours(HWND hwnd) {
	int i ;
    /* Classic KiTTY had 34 (TuTTY) / 22 colours; this port's Conf holds
     * exactly CONF_NCOLOURS (25) - reading past that asserts in conf.c. */
    for (i = 0; i < CONF_NCOLOURS; i++) {
	conf_set_int_int(conf, CONF_colours, i*3+0, 256-conf_get_int_int(conf, CONF_colours, i*3+0));
	conf_set_int_int(conf, CONF_colours, i*3+1, 256-conf_get_int_int(conf, CONF_colours, i*3+1));
	conf_set_int_int(conf, CONF_colours, i*3+2, 256-conf_get_int_int(conf, CONF_colours, i*3+2));
    }
    force_reconf = 0 ;
    PostMessage( hwnd, WM_COMMAND, IDM_RECONF, 0 ) ;

    RefreshBackground( hwnd ) ;
}

static int * BlackOnWhiteColoursSave = NULL ;
void BlackOnWhiteColours(HWND hwnd) {
	if( BlackOnWhiteColoursSave==NULL ) {
		BlackOnWhiteColoursSave = (int*) malloc( 6*sizeof(int) ) ;
		BlackOnWhiteColoursSave[0]=conf_get_int_int(conf, CONF_colours, 0); conf_set_int_int(conf, CONF_colours, 0, 0);
		BlackOnWhiteColoursSave[1]=conf_get_int_int(conf, CONF_colours, 1); conf_set_int_int(conf, CONF_colours, 1, 0);
		BlackOnWhiteColoursSave[2]=conf_get_int_int(conf, CONF_colours, 2); conf_set_int_int(conf, CONF_colours, 2, 0);
		BlackOnWhiteColoursSave[3]=conf_get_int_int(conf, CONF_colours, 6); conf_set_int_int(conf, CONF_colours, 6, 255);
		BlackOnWhiteColoursSave[4]=conf_get_int_int(conf, CONF_colours, 7); conf_set_int_int(conf, CONF_colours, 7, 255);
		BlackOnWhiteColoursSave[5]=conf_get_int_int(conf, CONF_colours, 8); conf_set_int_int(conf, CONF_colours, 8, 255);
	} else {
		if(conf_get_int_int(conf, CONF_colours, 0)==0) {
			conf_set_int_int(conf, CONF_colours, 0, 255);
			conf_set_int_int(conf, CONF_colours, 1, 255);
			conf_set_int_int(conf, CONF_colours, 2, 255);
			conf_set_int_int(conf, CONF_colours, 6, 0);
			conf_set_int_int(conf, CONF_colours, 7, 0);
			conf_set_int_int(conf, CONF_colours, 8, 0);
		} else {
			conf_set_int_int(conf, CONF_colours, 0, BlackOnWhiteColoursSave[0]) ;
			conf_set_int_int(conf, CONF_colours, 1, BlackOnWhiteColoursSave[1]) ;
			conf_set_int_int(conf, CONF_colours, 2, BlackOnWhiteColoursSave[2]) ;
			conf_set_int_int(conf, CONF_colours, 6, BlackOnWhiteColoursSave[3]) ;
			conf_set_int_int(conf, CONF_colours, 7, BlackOnWhiteColoursSave[4]) ;
			conf_set_int_int(conf, CONF_colours, 8, BlackOnWhiteColoursSave[5]) ;
			free(BlackOnWhiteColoursSave) ;
			BlackOnWhiteColoursSave=NULL ;
		}
	}
	force_reconf = 0 ;
	PostMessage( hwnd, WM_COMMAND, IDM_RECONF, 0 ) ;

	ResetWindow(2);
}

static int original_fontsize = -1 ;
void ChangeFontSize(Terminal *term, Conf *conf,HWND hwnd, int dec) {
	FontSpec *fontspec = conf_get_fontspec(conf, CONF_font);
	if( original_fontsize<0 ) original_fontsize = fontspec->height ;
	if( dec == 0 ) { fontspec->height = original_fontsize ; }
	else {
		fontspec->height = fontspec->height + dec ;
		if(fontspec->height <=0 ) fontspec->height = 1 ;
	}
	conf_set_fontspec(conf, CONF_font, fontspec);
        fontspec_free(fontspec);
	force_reconf = 0 ;
	term_size(term,
				conf_get_int(conf, CONF_height),
				conf_get_int(conf, CONF_width),
				conf_get_int(conf, CONF_savelines));
	//PostMessage( hwnd, WM_COMMAND, IDM_RECONF, 0 ) ;

	ResetWindow(2);
}
void ChangeSettings(HWND hwnd) {
	//NegativeColours(hwnd);
	BlackOnWhiteColours(hwnd);
	//ChangeFontSize(hwnd,1);
	//ChangeFontSize(hwnd,-1);
}

#ifdef MOD_BACKGROUNDIMAGE
// Gestion de l'image viewer
int ManageViewer( HWND hwnd, WORD wParam ) { // Gestion du mode image
	if( wParam==VK_BACK )
		{ if( PreviousBgImage( hwnd ) ) InvalidateRect(hwnd, NULL, TRUE) ;
		set_title(NULL, conf_get_str(conf,CONF_wintitle) ) ;
		return 1 ;
		}
	else if( wParam==VK_SPACE )
		{ if( NextBgImage( hwnd ) ) InvalidateRect(hwnd, NULL, TRUE) ;
		set_title(NULL, conf_get_str(conf,CONF_wintitle)) ;
		return 1 ;
		}
	else if( wParam == VK_DOWN ) 	// Augmenter l'opacite de l'image de fond
		{ if( conf_get_int(conf,CONF_bg_type) != 0 ) {
			int n=conf_get_int(conf,CONF_bg_opacity) ;
			n += 5 ; if( n>100 ) n = 0 ;
			conf_set_int( conf, CONF_bg_opacity, n ) ;
			RefreshBackground( hwnd ) ;
			return 1 ;
			}
		}
	else if( wParam == VK_UP ) 		// Diminuer l'opacite de l'image de fond
		{ if( conf_get_int(conf,CONF_bg_type) != 0 ) {
			int n=conf_get_int(conf,CONF_bg_opacity) ;
			n -= 5 ;
			if( n<0 ) n = 100 ;
			conf_set_int( conf, CONF_bg_opacity, n ) ;
			RefreshBackground( hwnd ) ;
			return 1 ;
			}
		}
	else if( wParam == VK_RETURN ) {
		  if (IsZoomed(hwnd)) { ShowWindow(hwnd, SW_RESTORE); }
		  else { ShowWindow(hwnd, SW_MAXIMIZE); }
		return 1 ;
		}
	return 0 ;
	}
#endif
