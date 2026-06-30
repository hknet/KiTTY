/* =============================================================================
 * NOT BUILT - dead code, kept for reference only.
 * Standalone kitty.dll plugin wrapper (see the g++ -shared command below). No
 * DLL target exists in this port's CMake build, so it is never compiled.
 * Not listed in any CMakeLists and not #included anywhere.
 * ============================================================================= */
// g++ -shared -o kitty.dll kitty_dll.res.o ../../kitty_dll.c -Wl,--whole-archive ../../base64.a -Wl,--no-whole-archive -DBUILD_DLL -static-libgcc
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifdef BUILD_DLL
#define LINKDLL extern "C" __declspec(dllimport)
#else
#define LINKDLL 
#endif

extern "C" const char * get_param_str( const char * val ) {
	if( !stricmp( val, "CLASS" ) ) return "KiTTY" ;
	return NULL ;
	}
