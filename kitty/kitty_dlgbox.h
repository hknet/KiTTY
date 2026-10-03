/*
 * kitty_dlgbox.h - the declarations for kitty_dlgbox.c: the suite's themed
 * message, notice and confirmation boxes and the shared dialog helpers
 * (icon, centring, fit-to-text). kitty_msgbox.h routes a file's plain
 * MessageBox calls to kitty_message_box.
 */
#ifndef KITTY_DLGBOX_H
#define KITTY_DLGBOX_H

#include "putty.h"
#include <windows.h>

// Centre a dialog in the middle of its parent window
void CenterDlgInParent(HWND hDlg) ;
/* Centre a modal dialog over its owner (WM_INITDIALOG, after final size). */
void kitty_centre_on_owner( HWND dlg ) ;
/* Give a dialog the caption and taskbar icon of the window that raised it -
 * which is the SESSION's own icon when that session carries one. `owner`
 * NULL = the dialog's owner. Called from WM_INITDIALOG. */
void kitty_dialog_icon( HWND dlg, HWND owner ) ;
/* Grow one static control to fit `text` at the dialog's font, moved down by
 * extra_dy; returns the height change in pixels (the caller moves what sits
 * below and grows the window). Empty text collapses and hides the control. */
int kitty_fit_text( HWND dlg, int ctlid, const char *text, int extra_dy ) ;

int kitty_confirm_box( HWND owner, const char *caption, const char *text, const char *warn_red );
int kitty_confirm_box3( HWND owner, const char *caption, const char *text, const char *b_over, const char *b_keep, const char *b_cancel );
int kitty_confirm_box_yes( HWND owner, const char *caption, const char *text, const char *warn_red );
int kitty_confirm_box_yes_front( HWND owner, const char *caption, const char *text, const char *warn_red );
/* The same box, modeless: done(yes, ctx) once; NULL = not made, no callback.
 * detail: NULL, or a read-only field under the question (scrolls if long). */
HWND kitty_confirm_modeless( HWND owner, const char *caption, const char *text,
                             const char *detail, const char *warn_red,
                             void (*done)( int yes, void *ctx ), void *ctx );
/* The same box with its two buttons named (b_no stays the default; NULL keeps
 * the template's word). */
HWND kitty_confirm_modeless_words( HWND owner, const char *caption, const char *text,
                                   const char *detail, const char *b_yes, const char *b_no,
                                   void (*done)( int yes, void *ctx ), void *ctx );
/* The same box with named buttons (b_no stays the default) and an optional
 * checkbox (check = its label, or NULL): done(yes, checked, ctx) once;
 * checked only counts with yes. NULL = not made, no callback. */
HWND kitty_confirm_modeless_check( HWND owner, const char *caption, const char *text,
                                   const char *b_yes, const char *b_no, const char *check,
                                   void (*done)( int yes, int checked, void *ctx ), void *ctx );
void kitty_info_box( HWND owner, const char *caption, const char *text, const char *warn_red );
/* kitty_info_box, modeless (detail as above); NULL = not made. */
HWND kitty_info_modeless( HWND owner, const char *caption, const char *text,
                          const char *detail, const char *warn_red );
int kitty_message_box( HWND owner, const char *text, const char *caption, unsigned type );
void kitty_notice_box( HWND owner, const char *caption, const char *text );
void kitty_demo_templates( void );

#endif
