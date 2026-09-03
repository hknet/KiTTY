#ifdef MOD_BACKGROUNDIMAGE

#include <stdbool.h>

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
