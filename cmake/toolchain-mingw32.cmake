# 32-bit variant of toolchain-mingw.cmake: cross-build Windows i686 PuTTY
# on Linux using MinGW.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER  i686-w64-mingw32-gcc)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)
set(CMAKE_AR          i686-w64-mingw32-ar)
set(CMAKE_RANLIB      i686-w64-mingw32-ranlib)

add_compile_definitions(__USE_MINGW_ANSI_STDIO)
