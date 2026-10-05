/*
 * kitty_image.c - the terminal background image (MOD_BACKGROUNDIMAGE).
 * It loads the picture (a BMP through LoadImage, a JPEG or PNG through the
 * Windows Imaging Component, or the desktop wallpaper read from the shell's
 * own settings),
 * then tiles, centres, stretches or places it in an off-screen device
 * context sized to the whole virtual desktop, and applies the configured
 * opacity, including the gradient styles computed pixel by pixel.
 * What it leaves behind is that background DC, plus a pre-blended copy for
 * fast fills, which the painting code draws behind the terminal text; it is
 * rebuilt on a resize or a configuration change. The /screenshot command's
 * PNG writer lives here too.
 */

#include <stdio.h>

#include <winsock2.h>	/* must precede windows.h (putty.h pulls winsock2 later) */
#include <windows.h>

#include "kitty_wic.h"

#ifdef MOD_BACKGROUNDIMAGE

#include <math.h>
#include "putty.h"
#include "terminal.h"

#include "kitty_image.h"
#include "kitty_oldwin.h"   /* record what an older Windows does not have */
#include "kitty_text.h"     /* the feature name in the old-Windows report */
#include "kitty_gui.h"
#include "kitty_tools.h"
#include "kitty.h"

//extern int offset_width, offset_height ;
//extern int font_width, font_height ;

#ifndef NCFGCOLOURS
#define NCFGCOLOURS 24
#endif
#ifndef NEXTCOLOURS
#define NEXTCOLOURS 240
#endif
#ifndef NALLCOLOURS
#define NALLCOLOURS (NCFGCOLOURS + NEXTCOLOURS)
#endif

//extern COLORREF colours[NALLCOLOURS] ;
extern HWND MainHwnd ;

#ifndef stricmp	/* platform.h may #define stricmp _stricmp (CRT); don't redeclare */
#endif



static BOOL (WINAPI * pAlphaBlend)( HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION ) = 0 ;

//static HWND hwnd;

HDC textdc = NULL ;
HBITMAP textbm = NULL ;
COLORREF colorinpixel;
HDC colorinpixeldc = NULL ;
HBITMAP colorinpixelbm = NULL;
HDC backgrounddc = NULL ;
/* Where the image DC's (0,0) sits on screen: the virtual desktop's origin
 * (SM_XVIRTUALSCREEN / SM_YVIRTUALSCREEN, negative with a monitor left of or
 * above the primary). A consumer with screen coordinates subtracts it. */
int kitty_bg_origin_x = 0, kitty_bg_origin_y = 0 ;
/* Bumped whenever the image DC is rebuilt or redrawn, so a painter that
 * keeps its own copy of it (windows/paint-d2d.c) knows to refresh. */
int kitty_bg_generation = 0 ;
HBITMAP backgroundbm = NULL ;
HDC backgroundblenddc = NULL ;
HBITMAP backgroundblendbm = NULL;
BOOL bBgRelToTerm;
bool resizing;
RECT size_before;


#ifdef DLL
#define TARGET extern __declspec(dllexport)
#else
#define TARGET
#endif

static int ShrinkBitmapEnable = 1 ;
void SetShrinkBitmapEnable( int v ) {
	if( v ) ShrinkBitmapEnable = 1 ;
	else ShrinkBitmapEnable = 0 ;
}
int GetShrinkBitmapEnable( void ) { return ShrinkBitmapEnable ; }
	

//
// Bitmap shrinking functions
//
#define Alloc(p,t) (t *)malloc((p)*sizeof(t))
#define For(i,n) for ((i)=0;(i)<(n);(i)++)
#define iFor(n) For (i,n)
#define jFor(n) For (j,n)

typedef struct {
	WORD x, y ;	// dimensions
	WORD l ;	// bytes per scan-line (32-bit allignment)
	BYTE *b ;	// bits of bitmap,3 bytes/pixel, BGR
} tWorkBMP ;		// 24-bit working bitmap

static void CreateWorkingBitmap( WORD dx, WORD dy, tWorkBMP *w ) {
	w->x=dx ;
	w->y=dy ;
	w->l=(dx+1)*3&0xfffc ;
	w->b=Alloc( w->l*dy, BYTE ) ;
}

static HBITMAP CreateEmptyBitmap( WORD dx, WORD dy ) {
	HDC h = GetDC( NULL ) ;
	HBITMAP b = CreateCompatibleBitmap( h, dx, dy ) ;
	ReleaseDC( NULL, h ) ;
	return(b) ;
}

static void SetBMIHeader( BITMAPINFO *b, short dx, short dy ) {
	b->bmiHeader.biSize = sizeof(BITMAPINFOHEADER) ;
	b->bmiHeader.biWidth = dx ;
	b->bmiHeader.biHeight = -dy ;
	b->bmiHeader.biPlanes = 1 ;
	b->bmiHeader.biBitCount = 24 ;
	b->bmiHeader.biCompression = BI_RGB ;
	b->bmiHeader.biSizeImage = 0 ;
	b->bmiHeader.biXPelsPerMeter = 1 ;
	b->bmiHeader.biYPelsPerMeter = 1 ;
	b->bmiHeader.biClrUsed = 0 ;
	b->bmiHeader.biClrImportant = 0 ;
}

static POINT GetBitmapSize( HBITMAP h ) {
	POINT p ;
	BITMAP o ;
	GetObject( h, sizeof(o), &o ) ;
	p.x = o.bmWidth ;
	p.y = o.bmHeight ;
	return(p) ;
}

static void OpenBitmapForWork( HBITMAP b, tWorkBMP *w ) {
	BITMAPINFO s ;
	HDC h = GetDC( NULL ) ;
	POINT v = GetBitmapSize( b ) ;
	CreateWorkingBitmap( v.x, v.y, w ) ;
	SetBMIHeader( &s,w->x, w->y ) ;
	GetDIBits( h, b, 0, w->y, w->b, &s, DIB_RGB_COLORS ) ;
	ReleaseDC( NULL, h ) ;
}

static void SaveWorkingBitmap( tWorkBMP *w, HBITMAP b ) {
	BITMAPINFO s ;
	HDC h = GetDC( NULL ) ;
	SetBMIHeader( &s, w->x, w->y ) ;
	SetDIBits( h, b, 0, w->y, w->b, &s, DIB_RGB_COLORS ) ;
	ReleaseDC( NULL, h ) ;
}

static void ShrinkWorkingBitmap( tWorkBMP *a, tWorkBMP *b, WORD bx, WORD by ) {
	BYTE *uy = a->b, *ux, i ;
	WORD x, y, nx, ny = 0 ;
	DWORD df = 3*bx, nf = df*by, j ;
	float k, qx[2], qy[2], q[4], *f = Alloc( nf, float ) ;

	CreateWorkingBitmap( bx, by, b) ;

	jFor (nf) f[j]=0;
	j=0;

	For( y, a->y ) {
		ux=uy;
		uy+=a->l;
		nx=0;
		ny+=by;
		if (ny>a->y) {
			qy[0]=1-(qy[1]=(ny-a->y)/(float)by);
			For (x,a->x) {
				nx+=bx;
				if (nx>a->x) {
					qx[0]=1-(qx[1]=(nx-a->x)/(float)bx);
					iFor (4) q[i]=qx[i&1]*qy[i>>1];
					iFor (3) {
						f[j]+=(*ux)*q[0];
						f[j+3]+=(*ux)*q[1];
						f[j+df]+=(*ux)*q[2];
						f[(j++)+df+3]+=(*(ux++))*q[3];
					}
				} else iFor (3) {
					f[j+i]+=(*ux)*qy[0];
					f[j+df+i]+=(*(ux++))*qy[1];
				}
				if (nx>=a->x) nx-=a->x;
				if (!nx) j+=3;
			}
		} else {
			For (x,a->x) {
				nx+=bx;
				if (nx>a->x) {
					qx[0]=1-(qx[1]=(nx-a->x)/(float)bx);
					iFor (3) {
						f[j]+=(*ux)*qx[0];
						f[(j++)+3]+=(*(ux++))*qx[1];
					}
				}
				else iFor (3) f[j+i]+=*(ux++);
				if (nx>=a->x) nx-=a->x;
				if (!nx) j+=3;
			}
			if (ny<a->y) j-=df;
		}
		if (ny>=a->y) ny-=a->y;
	}

	nf=0;
	k=bx*by/(float)(a->x*a->y);
	uy=b->b;

	For (y,by) {
		jFor (df) uy[j]=(unsigned char)(f[nf++]*k+.5);
		uy+=b->l;
	}

	free (f);
}

static TARGET HBITMAP ShrinkBitmap( HBITMAP a, WORD bx, WORD by )
// creates and returns new bitmap with dimensions of
// [bx,by] by shrinking bitmap a both [bx,by] must be less or equal
// than the dims of a, unless the result is nonsense
{
	tWorkBMP in, out ;
	HBITMAP b=CreateEmptyBitmap( bx, by ) ;
	OpenBitmapForWork( a, &in ) ;
	ShrinkWorkingBitmap( &in, &out, bx, by ) ;
	free( in.b ) ;
	SaveWorkingBitmap( &out, b ) ;
	free( out.b ) ;
	return( b ) ;
}






static HBITMAP ResizeBmp( HBITMAP hBmpSrc, WORD bx, WORD by ) {
	SIZE newSize ;
	newSize.cx = bx;
	newSize.cy = by;
	// current size
	BITMAP bmpInfo;
	GetObject(hBmpSrc, sizeof(BITMAP), &bmpInfo);
	SIZE oldSize;
	oldSize.cx = bmpInfo.bmWidth;
	oldSize.cy = bmpInfo.bmHeight;

	// select the source into a DC
	HDC hdc = GetDC(NULL);
	HDC hDCSrc = CreateCompatibleDC(hdc);
	HBITMAP hOldBmpSrc = (HBITMAP)SelectObject(hDCSrc, hBmpSrc);

	// create the destination bitmap and select it into a DC
	HDC hDCDst = CreateCompatibleDC(hdc);
	HBITMAP hBmpDst = CreateCompatibleBitmap(hdc, newSize.cx, newSize.cy);
	HBITMAP hOldBmpDst = (HBITMAP)SelectObject(hDCDst, hBmpDst);

	// resize
	StretchBlt(hDCDst, 0, 0, newSize.cx, newSize.cy, hDCSrc, 0, 0, oldSize.cx, oldSize.cy, SRCCOPY);
	
	// release the resources
	SelectObject(hDCSrc, hOldBmpSrc);
	SelectObject(hDCDst, hOldBmpDst);
	DeleteDC(hDCSrc);
	DeleteDC(hDCDst);
	ReleaseDC(NULL, hdc);
	return hBmpDst;
}

static void fill_dc(HDC dc, int width, int height, COLORREF color)
{
    HBRUSH clrBrush = CreateSolidBrush(color);
    HPEN clrPen = CreatePen(PS_SOLID, 0, color);

    HBRUSH oldBrush;
    HPEN oldPen;

    oldBrush = SelectObject(dc, clrBrush);
    oldPen = SelectObject(dc, clrPen);

    Rectangle(dc, 0, 0, width, height);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
}

static BOOL load_wallpaper_bmp(HBITMAP* rawImage, int* style, int* x, int* y)
{
    LONG lRes;
    HKEY kDesktop;
    DWORD pathLen = MAX_PATH;
    DWORD numBufLen = 10;
    char wpPath[MAX_PATH];
    char wpStyleBuf[10];
    char wpTileBuf[10];

    int wpStyle = -1;
    int wpTile = -1;

    // NOTE: In the non-wallpaper case (i.e., load_file_bmp), we load parameters
    // like x, y and style from cfg, but in the wallpaper case we ignore our
    // stored configuration and get that information from the system.

    // ENHANCE: It's possible to set WallpaperOriginX and WallpaperOriginY to
    // specify an exact position for the start of the wallpaper, but this
    // function doesn't support that yet.  I don't think it's possible to set
    // it through the normal UI anyway, you have to hack the registry to do it.
    // For now, we'll never return an (x,y) positioning request to the caller.
    *x = *y = 0;

    lRes = RegOpenKeyEx(
        HKEY_CURRENT_USER, "Control Panel\\Desktop", 0, KEY_READ, &kDesktop
    );
    if(lRes != ERROR_SUCCESS)
    {
        RegCloseKey(kDesktop);
        return FALSE; // TODO: Should the error be reported to the user here?
    }

    lRes = RegQueryValueEx(kDesktop, "Wallpaper", NULL, NULL, (LPBYTE)wpPath, &pathLen);
    if(lRes != ERROR_SUCCESS)
    {
        RegCloseKey(kDesktop);
        return FALSE; // TODO: Should the error be reported to the user here?
    }

    lRes = RegQueryValueEx( kDesktop, "WallpaperStyle", NULL, NULL, (LPBYTE)wpStyleBuf, &numBufLen );
    if(lRes == ERROR_SUCCESS)
        wpStyle = atoi(wpStyleBuf);

    lRes = RegQueryValueEx( kDesktop, "TileWallpaper", NULL, NULL, (LPBYTE)wpTileBuf, &numBufLen );
    if(lRes == ERROR_SUCCESS)
        wpTile = atoi(wpTileBuf);

    if(wpStyle < 0 && wpStyle > 3)
        wpStyle = 0;  // Default to tile.
    else if(wpTile > 0)
        wpStyle = 0;  // Force tile.
    else if(wpStyle == 0 && wpTile == 0)
        wpStyle = 1;  // For Explorer, wpStyle == wpTile == 0 means center, and
                      // it doesn't ever set wpStyle to 1.  We call wpStyle == 1
                      // center and don't use wpTile, to simplify things after
                      // this point.

    RegCloseKey(kDesktop);
 
    if( *rawImage!=NULL ) { DeleteObject( *rawImage ) ; *rawImage=NULL ; }
    *rawImage = LoadImage(
        NULL, wpPath, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE
    );
    if(*rawImage == 0)
        return FALSE; // TODO: Should the error be reported to the user here?

    *style = wpStyle;

    return TRUE;
}

static BOOL load_file_bmp(HBITMAP* rawImage, int* style, int* x, int* y)
{
    *x = conf_get_int( conf,CONF_bg_image_abs_x); 
    *y = conf_get_int( conf,CONF_bg_image_abs_y );
    *style = conf_get_int( conf,CONF_bg_image_style);

    if( *rawImage!=NULL ) { DeleteObject( *rawImage ) ; *rawImage=NULL ; }
    /* KiTTY: %VAR% expansion (cyd01/KiTTY#472) - expand right before the
     * real Win32 open; the stored CONF_bg_image_filename stays literal. */
    {
        char *expanded = filename_expand_str(conf_get_filename(conf, CONF_bg_image_filename));
        *rawImage = LoadImage(
            NULL, expanded, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE
        );
        sfree(expanded);
    }
    if(*rawImage == 0)
        return FALSE; // TODO: Should the error be reported to the user here?

    return TRUE;
}


HBITMAP CreateHBitmap(int w, int h, LPVOID *lpBits)
{
	HBITMAP bitmap;
	BITMAPINFOHEADER BIH ;
	int iSize = sizeof(BITMAPINFOHEADER) ;
	memset(&BIH, 0, iSize);

	// Fill in the header info.
	BIH.biSize = iSize;
	BIH.biWidth = w;
	BIH.biHeight = h;
	BIH.biPlanes = 1;
	BIH.biBitCount = 24;
	BIH.biCompression = BI_RGB;
	HDC hDC = CreateCompatibleDC(NULL);
	bitmap = CreateDIBSection(hDC,
							(BITMAPINFO*)&BIH,
							DIB_RGB_COLORS,
							lpBits,
							NULL,
							0);
	DeleteDC(hDC);
	return bitmap;
}

/* KiTTY: a JPEG or PNG background through Windows' own decoder of that
 * format (kitty_wic.c) - the bundled libjpeg is gone. A Windows without WIC
 * (XP unless .NET 3.0 or the WIC redistributable is installed) shows no
 * image for these files; BMP goes through LoadImage everywhere. */
static BOOL load_file_wic(HBITMAP* rawImage, int* style, int* x, int* y, int fmt) {
    *x = conf_get_int( conf, CONF_bg_image_abs_x );
    *y = conf_get_int( conf, CONF_bg_image_abs_y );
    *style = conf_get_int( conf, CONF_bg_image_style );

    if( *rawImage!=NULL ) { DeleteObject( *rawImage ) ; *rawImage=NULL ; }
    /* KiTTY: %VAR% expansion (cyd01/KiTTY#472) - see load_file_bmp() above. */
    {
        char *expanded = filename_expand_str(conf_get_filename(conf, CONF_bg_image_filename));
        *rawImage = kitty_wic_load_file( expanded, fmt ) ;
        sfree(expanded);
    }
    return *rawImage != NULL ;
}

static HBITMAP CreateDIBSectionWithFileMapping(HDC dc, int width, int height, HANDLE fmap)
{
    BITMAPINFOHEADER BMI;
    
    BMI.biSize = sizeof(BITMAPINFOHEADER);
    BMI.biWidth = width;
    BMI.biHeight = height;
    BMI.biPlanes = 1;
    BMI.biBitCount = 32;
    BMI.biCompression = BI_RGB;
    BMI.biSizeImage = 0;
    BMI.biXPelsPerMeter = 0;
    BMI.biYPelsPerMeter = 0;
    BMI.biClrUsed = 0;
    BMI.biClrImportant = 0;
    
    return(CreateDIBSection(dc, (BITMAPINFO *)&BMI, DIB_RGB_COLORS, 0, fmap, 0));
}


/***********SCREEN CAPTURE*******************/

/******************************/

void color_blend(
    HDC destDc, int x, int y, int width, int height, 
    COLORREF alphacolor, int opacity)
{
    if(pAlphaBlend) {
    	// Fast alpha blending for Win98&2000 and newer...
        BLENDFUNCTION blender;

        // Create one pixel size bitmap for use in color_blend.
        if(colorinpixel != alphacolor) 
        {
            colorinpixel = alphacolor;
            SetPixelV(colorinpixeldc, 0, 0, alphacolor);
        }
        
        blender.BlendOp = AC_SRC_OVER;
        blender.BlendFlags = 0;
        blender.SourceConstantAlpha = (0xff * opacity) / 100;
        blender.AlphaFormat = 0;
        
        (*pAlphaBlend)(destDc, x, y, width, height, colorinpixeldc, 0, 0, 1, 1, blender);
    } 
    else
    {
        // Slow alpha blending for Win95&NT...
        // Note: Only tested with WinXP, should work on 95/NT.. probably.
        int i, alphacolorR, alphacolorG, alphacolorB, bk_opacity;
        HBITMAP tmpbm;
        HDC tmpdc;
        static HANDLE fmap;
        static int fmap_size;
        static unsigned char * pRGB;
        
        if(fmap_size < width * height * 4)
        {
            if(fmap) {
            	UnmapViewOfFile(pRGB);
                CloseHandle(fmap);
            }
            fmap_size = width * height * 4;
            fmap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, fmap_size, NULL);
            pRGB = MapViewOfFile(fmap, FILE_MAP_ALL_ACCESS, 0, 0, fmap_size);
        }
        
        // Create DIBSection so we get pixels easily.
        tmpdc = CreateCompatibleDC(destDc);
        tmpbm = CreateDIBSectionWithFileMapping(destDc, width, height, fmap);
        SelectObject(tmpdc, tmpbm);
        
        // Copy bitmap to temporary bitmap for easy pixel access.
        BitBlt(tmpdc, 0, 0, width, height, destDc, x, y, SRCCOPY);

        // Moved stuff out from the loop
        alphacolorR = GetRValue(alphacolor) * opacity;
        alphacolorG = GetGValue(alphacolor) * opacity;
        alphacolorB = GetBValue(alphacolor) * opacity;
	bk_opacity = 100 - opacity;

        for(i=0; i<width*height*4; i+=4)
        {
	    pRGB[i + 0] = (pRGB[i + 0] * bk_opacity + alphacolorB) / 100;
            pRGB[i + 1] = (pRGB[i + 1] * bk_opacity + alphacolorG) / 100;
            pRGB[i + 2] = (pRGB[i + 2] * bk_opacity + alphacolorR) / 100;
        }
        
        // Copy temporary bitmap back to original
        BitBlt(destDc, x, y, width, height, tmpdc, 0, 0, SRCCOPY);
        
        DeleteObject(tmpbm);
        DeleteDC(tmpdc);
    }
}

static void color_opacity_gradient( HDC destDc, int x, int y, int width, int height, COLORREF alphacolor, int style ) {
	int i, alphacolorR, alphacolorG, alphacolorB, bk_opacity, opacity, h, w ;
	double l ;
        HBITMAP tmpbm;
        HDC tmpdc;
        static HANDLE fmap;
        static int fmap_size;
        static unsigned char * pRGB;
	int OpacityMin = 0, OpacityMax = 100 ;
	
	char buf[256] = "" ;
	if( GetSessionField( conf_get_str(conf,CONF_sessionname), conf_get_str(conf,CONF_folder), "BgOpacityRange", buf ) ) { 
		sscanf( buf, "%d-%d", &OpacityMin, &OpacityMax ) ; 
		if( OpacityMin == OpacityMax ) { OpacityMin = 0 ; OpacityMax = 100 ; }
		}
        
        if(fmap_size < width * height * 4) {
		if(fmap) { UnmapViewOfFile(pRGB) ; CloseHandle(fmap) ; }
		fmap_size = width * height * 4;
		fmap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, fmap_size, NULL);
		pRGB = MapViewOfFile(fmap, FILE_MAP_ALL_ACCESS, 0, 0, fmap_size);
		}
        
        // Create DIBSection so we get pixels easily.
        tmpdc = CreateCompatibleDC(destDc);
        tmpbm = CreateDIBSectionWithFileMapping(destDc, width, height, fmap);
        SelectObject(tmpdc, tmpbm);
        
        // Copy bitmap to temporary bitmap for easy pixel access.
        BitBlt(tmpdc, 0, 0, width, height, destDc, x, y, SRCCOPY);

        opacity = 0 ; bk_opacity = 100 ; alphacolorR = 0 ; alphacolorG = 0 ; alphacolorB = 0 ;
	w = 0 ; h = 0 ;

        for(i=0; i<width*height*4; i+=4) {
		
		switch( style ) {
			case 2: // Bottom to top
			if( (i%(4*width)) == 0 ) {
				h++ ;
				opacity = OpacityMin + 1.0*( OpacityMax-OpacityMin ) * (1.0*h)/(1.0*height) ;
				opacity = 100 - opacity ;
				}
				break ;
			case 3: // Left to right
				w++ ; if( w >= width ) { w = 0 ; }
				opacity = OpacityMin + 1.0*( OpacityMax-OpacityMin ) * (1.0*w)/(1.0*width) ;
				break ;
			case 4: // Right to left
				w++ ; if( w >= width ) { w = 0 ; }
				opacity = OpacityMin + 1.0*( OpacityMax-OpacityMin ) * (1.0*w)/(1.0*width) ;
				opacity = 100 - opacity ;
				break ;
			case 5: // From the centre outwards
				if( (i%(4*width)) == 0 ) { h++ ; }
				w++ ; if( w >= width ) { w = 0 ; }
				l = sqrt( pow(1.0*width/2.0-w,2.0)+pow(1.0*height/2.0-h,2.0) ) / 
					sqrt( pow(1.0*width/2.0,2.0)+pow(1.0*height/2.0,2.0) );
				opacity = OpacityMin + 1.0*( OpacityMax-OpacityMin ) * l ;
				break ;
			case 6: // From the outside towards the centre
				if( (i%(4*width)) == 0 ) { h++ ; }
				w++ ; if( w >= width ) { w = 0 ; }
				l = sqrt( pow(1.0*width/2.0-w,2.0)+pow(1.0*height/2.0-h,2.0) ) / 
					sqrt( pow(1.0*width/2.0,2.0)+pow(1.0*height/2.0,2.0) );
				opacity = OpacityMin + 1.0*( OpacityMax-OpacityMin ) * l ;
				opacity = 100 - opacity ;					
				break ;
			case 7:
				if( (i%(4*width)) == 0 ) { h++ ; }
				w++ ; if( w >= width ) { w = 0 ; }
				l = sqrt( pow(1.0*w,2.0)+pow(1.0*h,2.0) )/sqrt( pow(1.0*width,2.0)+pow(1.0*height,2.0) ) ;
				opacity = OpacityMin + 1.0*( OpacityMax-OpacityMin ) * l ;
				break ;
			case 8:
				if( (i%(4*width)) == 0 ) { h++ ; }
				w++ ; if( w >= width ) { w = 0 ; }
				l = sqrt( pow(1.0*w,2.0)+pow(1.0*h,2.0) )/sqrt( pow(1.0*width,2.0)+pow(1.0*height,2.0) ) ;
				opacity = OpacityMin + 1.0*( OpacityMax-OpacityMin ) * l ;
				opacity = 100 - opacity ;
				break ;
			case 9:
				if( (i%(4*width)) == 0 ) { h++ ; }
				w++ ; if( w >= width ) { w = 0 ; }
				l = sqrt( pow(1.0*(width-w),2.0)+pow(1.0*h,2.0) )/sqrt( pow(1.0*width,2.0)+pow(1.0*height,2.0) ) ;
				opacity = OpacityMin + 1.0*( OpacityMax-OpacityMin ) * l ;
				break ;
			case 10:
				if( (i%(4*width)) == 0 ) { h++ ; }
				w++ ; if( w >= width ) { w = 0 ; }
				l = sqrt( pow(1.0*(width-w),2.0)+pow(1.0*h,2.0) )/sqrt( pow(1.0*width,2.0)+pow(1.0*height,2.0) ) ;
				opacity = OpacityMin + 1.0*( OpacityMax-OpacityMin ) * l ;
				opacity = 100 - opacity ;
				break ;
			default: // Top to bottom
			if( (i%(4*width)) == 0 ) {
				h++ ;
				opacity = OpacityMin + 1.0*( OpacityMax-OpacityMin ) * (1.0*h)/(1.0*height) ;
				}
				break ;
			}
		if( opacity < 0 ) opacity = 0 ;
		if( opacity > 100 ) opacity = 100 ;

		alphacolorR = GetRValue(alphacolor) * opacity;
		alphacolorG = GetGValue(alphacolor) * opacity;
		alphacolorB = GetBValue(alphacolor) * opacity;
		bk_opacity = 100 - opacity ;

		pRGB[i + 0] = (pRGB[i + 0] * bk_opacity + alphacolorB) / 100;
		pRGB[i + 1] = (pRGB[i + 1] * bk_opacity + alphacolorG) / 100;
		pRGB[i + 2] = (pRGB[i + 2] * bk_opacity + alphacolorR) / 100;
		}
        
        // Copy temporary bitmap back to original
        BitBlt(destDc, x, y, width, height, tmpdc, 0, 0, SRCCOPY);
        
        DeleteObject(tmpbm);
        DeleteDC(tmpdc);
	}

BOOL load_bg_bmp(void)
{
    HBITMAP rawImage = NULL;
    BITMAP rawImageInfo;
    HDC hdcPrimary;
    HDC bmpdc;
    int deskWidth, deskHeight, clientWidth, clientHeight;
    int x, y;
    int style;

    //COLORREF backgroundcolor = colours[258]; // Default Background
    COLORREF backgroundcolor = return_colours258() ;
    COLORREF alphacolor = backgroundcolor;

    // Start off assuming this is true.
    bBgRelToTerm = conf_get_int( conf,CONF_bg_image_abs_fixed);
	
    RECT clientRect;
    GetWindowRect( MainHwnd, &clientRect ) ;
    clientWidth = clientRect.right-clientRect.left+1 ;
    clientHeight = clientRect.bottom-clientRect.top+1 ;

    switch( conf_get_int( conf,CONF_bg_type) )
    {
        // Solid
    case 0:
        // No bitmap file to load.  We'll handle this case below.
        break;

        // Wallpaper
    case 1:
        backgroundcolor = GetSysColor(COLOR_BACKGROUND);
        bBgRelToTerm = FALSE; // Wallpaper is never positioned relative to term.
        if(!load_wallpaper_bmp(&rawImage, &style, &x, &y))
            rawImage = NULL; // Make sure rawImage is still NULL.
        break;

        // Image
    case 2:
    	{
	backgroundcolor = GetSysColor(COLOR_BACKGROUND) ;
	const char *bgpath = filename_to_str(conf_get_filename(conf,CONF_bg_image_filename)) ;
	int bgpathlen = (int)strlen(bgpath) ;
	if( bgpath[0] == '#' ) {
		int r=0,g=0,b=0;
		sscanf( bgpath, "#%02X%02X%02X", &r, &g, &b ) ;
		backgroundcolor = RGB( r, g, b ) ;
		BYTE *pDst = NULL;
		rawImage = CreateHBitmap(10, 10, (void**)&pDst);
		style = 4 ;
		}
    	else if( ( bgpathlen>=4 && !stricmp( bgpath+bgpathlen-4, ".jpg" ) ) ||
    	         ( bgpathlen>=5 && !stricmp( bgpath+bgpathlen-5, ".jpeg" ) ) ) {
    		if(!load_file_wic(&rawImage, &style, &x, &y, KITTY_WIC_JPG))
        	    rawImage = NULL; // Make sure rawImage is still NULL.
    		}
    	else if( bgpathlen>=4 && !stricmp( bgpath+bgpathlen-4, ".png" ) ) {
    		if(!load_file_wic(&rawImage, &style, &x, &y, KITTY_WIC_PNG))
        	    rawImage = NULL; // Make sure rawImage is still NULL.
    		}
    	else 
        if(!load_file_bmp(&rawImage, &style, &x, &y))
            rawImage = NULL; // Make sure rawImage is still NULL.
        break;
	}
    }

    hdcPrimary = GetDC(MainHwnd);
    kitty_bg_generation++ ;
    /* The whole virtual desktop, every monitor: a window on a second screen
     * or straddling the primary's edge is otherwise beyond the image. */
    deskWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    deskHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    kitty_bg_origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    kitty_bg_origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if( deskWidth <= 0 || deskHeight <= 0 ) {
        deskWidth = GetDeviceCaps(hdcPrimary, HORZRES);
        deskHeight = GetDeviceCaps(hdcPrimary, VERTRES);
        kitty_bg_origin_x = kitty_bg_origin_y = 0 ;
    }


	// Safety check: do not go beyond the limits of the main screen
	if( (bBgRelToTerm == 0) 
		&&((clientRect.right>kitty_bg_origin_x+deskWidth)||(clientRect.bottom>kitty_bg_origin_y+deskHeight)
		   ||(clientRect.left<kitty_bg_origin_x)||(clientRect.top<kitty_bg_origin_y)) ) {
		//DeleteObject(rawImage); rawImage = NULL ;
		bBgRelToTerm = 1 ;
		}

    if(rawImage == NULL)
    {
        // Create a solid bitmap.  We'll get here in two cases: either the user
        // selected the Solid background option, or the attempt to do something
        // fancier (use the system wallpaper or an image file) failed.
        /*
        if( backgrounddc!=NULL ) DeleteDC(backgrounddc) ; backgrounddc = CreateCompatibleDC(hdcPrimary);
        if( backgroundbm!=NULL ) DeleteObject(backgroundbm); backgroundbm
            = CreateCompatibleBitmap(hdcPrimary, deskWidth, deskHeight);
        SelectObject(backgrounddc, backgroundbm);
        */
        // Do not create anything, use default 'no background'-code instead.
    }
    else
    {
        // We've managed to load a good image.  Now, we need to manipulate it to
        // the right size, location, etc.

        // Find the width and height of the image we just loaded.
        GetObject(rawImage, sizeof(rawImageInfo), &rawImageInfo);

        // Create a temporary DC to wrap the raw image.
        bmpdc = CreateCompatibleDC(hdcPrimary);
        SelectObject(bmpdc, rawImage);

        // Create a memory DC that has a new bitmap of the appropriate final
        // image size.
        if( textdc!=NULL ) { DeleteDC(textdc) ; } textdc = CreateCompatibleDC(hdcPrimary);
        if( textbm!= NULL ) { DeleteObject(textbm) ; } textbm = CreateCompatibleBitmap(hdcPrimary, deskWidth, deskHeight);
        SelectObject(textdc, textbm);

        if( backgrounddc!=NULL ) { DeleteDC(backgrounddc) ; } backgrounddc = CreateCompatibleDC(hdcPrimary);
        if( backgroundbm!=NULL ) { DeleteObject(backgroundbm) ; } backgroundbm = CreateCompatibleBitmap(hdcPrimary, deskWidth, deskHeight) ;
        SelectObject(backgrounddc, backgroundbm);

	switch(style)
        {
        case 0: { // Tile
             for(y = 0; y < deskHeight; y += rawImageInfo.bmHeight)
            {
                for(x = 0; x < deskWidth; x += rawImageInfo.bmWidth)
                {
                    BitBlt(
                        backgrounddc,
                        x, y, rawImageInfo.bmWidth, rawImageInfo.bmHeight,
                        bmpdc, 0, 0, SRCCOPY
                    );
                }
            }
           }
           break;

        case 1:    // Center

            // Calculate x & y, ignoring values they may already have, then drop
            // down to the (X,Y) placement case below.

            x = (deskWidth - rawImageInfo.bmWidth) / 2;
            y = (deskHeight - rawImageInfo.bmHeight) / 2;

        case 3: {  // Absolute Place at given (X,Y)
            // Start out with a background color fill.
            fill_dc(backgrounddc, deskWidth, deskHeight, backgroundcolor);

            BitBlt(
                backgrounddc,
                x, y, rawImageInfo.bmWidth, rawImageInfo.bmHeight,
                bmpdc, 0, 0, SRCCOPY
            );
            break;
            }

        case 2: // Stretch
            StretchBlt(
                backgrounddc, 0, 0, deskWidth, deskHeight,
                bmpdc, 0, 0, rawImageInfo.bmWidth, rawImageInfo.bmHeight,
                SRCCOPY
            );

            break;

	case 4: // blank background
		fill_dc(backgrounddc, deskWidth, deskHeight, backgroundcolor) ;
		break ;
	
	case 5: // Stretch to the size of the window
		{
		if( (ShrinkBitmapEnable)&&(clientWidth<rawImageInfo.bmWidth)&&(clientHeight<rawImageInfo.bmHeight) ) {
			HBITMAP newhbmpBMP ;
			if( (newhbmpBMP = ShrinkBitmap( rawImage,clientWidth,clientHeight)) != NULL ) {
			//if( (newhbmpBMP = ResizeBmp( rawImage,clientWidth,clientHeight)) != NULL ) {
				DeleteDC(bmpdc) ;
				bmpdc = CreateCompatibleDC(0) ;
				DeleteDC(backgrounddc); backgrounddc = GetDC(MainHwnd);
				SelectObject(bmpdc, newhbmpBMP ) ;
				BitBlt(backgrounddc, 0, 0,clientWidth,clientHeight, bmpdc, 0, 0, SRCCOPY ) ;
				DeleteObject(newhbmpBMP);
				}
			else
			StretchBlt( backgrounddc,0,0,clientWidth,clientHeight,bmpdc,0,0,rawImageInfo.bmWidth,rawImageInfo.bmHeight,SRCCOPY);
			}
		else
			StretchBlt( backgrounddc,0,0,clientWidth,clientHeight,bmpdc,0,0,rawImageInfo.bmWidth,rawImageInfo.bmHeight,SRCCOPY);
		}
		break ;
        }

        // Create a version of the background DC with opacity already applied
        // for fast screen fill in areas with no text.

        if( backgroundblenddc!=NULL ) { DeleteDC(backgroundblenddc) ; } backgroundblenddc = CreateCompatibleDC(hdcPrimary);
        if( backgroundblendbm!=NULL ) { DeleteObject(backgroundblendbm) ; } backgroundblendbm = CreateCompatibleBitmap( hdcPrimary, deskWidth, deskHeight );
        
        DeleteObject(rawImage);
        DeleteDC(bmpdc);
	
	SelectObject(backgroundblenddc, backgroundblendbm);
	BitBlt(
            backgroundblenddc, 0, 0, deskWidth, deskHeight,
            backgrounddc, 0, 0, SRCCOPY
        );
	
	if( conf_get_int(conf,CONF_bg_opacity) >= 0 ) 
		{ color_blend( backgroundblenddc, 0, 0, deskWidth, deskHeight, alphacolor, conf_get_int(conf,CONF_bg_opacity)); }
	else 
		{ 
		if( bBgRelToTerm == 1 ) {
			deskWidth = clientWidth ;
			deskHeight = clientHeight ;
			}
		
		color_opacity_gradient( backgroundblenddc, 0, 0, deskWidth, deskHeight, alphacolor, -conf_get_int(conf,CONF_bg_opacity) ); 
		}
    }

    ReleaseDC(MainHwnd, hdcPrimary);

//DeleteDC(hdcPrimary);
    return TRUE;
}

void clean_bg(void) {
	DeleteDC(textdc);textdc=NULL;
	DeleteObject(textbm);textbm=NULL;
	//DeleteObject(colorinpixel);
	//DeleteDC(colorinpixeldc);
	//DeleteObject(colorinpixelbm);
	DeleteDC(backgrounddc);backgrounddc=NULL;
	DeleteObject(backgroundbm);backgroundbm=NULL;
	DeleteDC(backgroundblenddc);backgroundblenddc=NULL;
	DeleteObject(backgroundblendbm);backgroundblendbm=NULL;
	}

void RedrawBackground( HWND hwnd ) {
	kitty_bg_generation++ ;
	if(
		1 && // This function was disabled because of the memory leak caused by the background image !!!  , but then there was a refresh problem?
		(get_param("BACKGROUNDIMAGE"))&&(!get_param("PUTTY"))&&(conf_get_int(conf,CONF_bg_type) != 0) ) 
			{
			clean_bg() ;
			load_bg_bmp();   // Apparently this is what was leaking memory !!!
			}
	/*
	InvalidateRect(hwnd, NULL, true) ;
	RedrawWindow(hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
	*/
	RedrawWindow( hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW ) ;
	
	}

#endif

/* KiTTY: the capture written as a PNG through WIC (kitty_wic.c); the
 * bundled libjpeg that wrote a JPEG is gone. FALSE on a Windows without WIC
 * (XP unless .NET 3.0 or the WIC redistributable is installed). */
int screenCapturePart(int x, int y, int w, int h, LPCSTR fname) {
    int return_code = 0 ;
    HDC hdcSource = GetDC(NULL);
    HDC hdcMemory = CreateCompatibleDC(hdcSource);

    HBITMAP hBitmap = CreateCompatibleBitmap(hdcSource, w, h);
    HBITMAP hBitmapOld = (HBITMAP)SelectObject(hdcMemory, hBitmap);

    BitBlt(hdcMemory, 0, 0, w, h, hdcSource, x, y, SRCCOPY);
    hBitmap = (HBITMAP)SelectObject(hdcMemory, hBitmapOld);

    ReleaseDC(NULL, hdcSource);
    DeleteDC(hdcMemory);

    if( kitty_wic_save_png( hBitmap, fname ) ) { return_code = 1 ; }
    DeleteObject(hBitmap);

    return return_code ;
}

int screenCaptureClientRect( HWND hwnd, LPCSTR fname ) {
	RECT rc;
	POINT p;
	GetClientRect(hwnd, &rc);
	p.x=rc.left; p.y=rc.top;
	ClientToScreen(hwnd,&p);
	return screenCapturePart(p.x,p.y,rc.right-rc.left,rc.bottom-rc.top,fname) ;
}
