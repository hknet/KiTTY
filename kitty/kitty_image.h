/*
 * kitty_image.h - what kitty_image.c exports: the device contexts and
 * bitmaps holding the terminal background image, the alpha-blend helper the
 * painting code calls over them, and the screen-capture entry points.
 */
#ifdef MOD_BACKGROUNDIMAGE

#include <stdbool.h>
#include "putty.h"   /* HDC, HBITMAP, Conf via platform.h */
#include <setjmp.h>   /* jmp_buf: the JPEG loader bails out through one */

extern HDC textdc;
extern HBITMAP textbm;
extern HBITMAP backgroundbm;
extern HDC backgroundblenddc;
extern HBITMAP backgroundblendbm;

extern COLORREF colorinpixel;
extern HDC colorinpixeldc;
extern HBITMAP colorinpixelbm;
extern HDC backgrounddc;
extern BOOL bBgRelToTerm;

extern bool resizing;
extern RECT size_before;

void color_blend(HDC destDc, int x, int y, int width, int height, COLORREF alphacolor, int opacity) ;

int screenCapturePart(int x, int y, int w, int h, LPCSTR fname,int quality) ;
int screenCaptureClientRect( HWND hwnd, LPCSTR fname, int quality ) ;

#endif

int screenCaptureClientRect( HWND hwnd, LPCSTR fname, int quality ) ;

/* ---- exported from kitty/kitty_image.c ---- */
HBITMAP CreateHBitmap(int w, int h, LPVOID *lpBits);
int GetShrinkBitmapEnable( void );
void RedrawBackground( HWND hwnd );
void SetShrinkBitmapEnable( int v );
void clean_bg(void);
BOOL load_bg_bmp(void);
extern jmp_buf JPEG_bailout;
extern int kitty_bg_generation;
extern int kitty_bg_origin_x, kitty_bg_origin_y;
extern char *loadError;
