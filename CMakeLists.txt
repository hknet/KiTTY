cmake_minimum_required(VERSION 3.7...3.28)
project(putty LANGUAGES C)

set(CMAKE_C_STANDARD 99)

include(cmake/setup.cmake)

# Scan the docs directory first, so that when we start calling
# installed_program(), we'll know if we have man pages available
add_subdirectory(doc)

add_compile_definitions(HAVE_CMAKE_H)

include_directories(terminal)

add_library(utils STATIC
  ${GENERATED_COMMIT_C})
add_dependencies(utils cmake_commit_c)
add_subdirectory(utils)
add_subdirectory(stubs)

add_library(logging OBJECT
  logging.c utils/logeventf.c)

add_library(eventloop STATIC
  callback.c timing.c)

add_library(console STATIC
  clicons.c console.c)

add_library(settings STATIC
  cmdline.c settings.c)

add_library(crypto STATIC
  proxy/cproxy.c proxy/sshproxy.c)
add_subdirectory(crypto)

add_library(network STATIC
  errsock.c x11disp.c
  $<TARGET_OBJECTS:logging>
  proxy/proxy.c
  proxy/http.c
  proxy/socks4.c
  proxy/socks5.c
  proxy/telnet.c
  proxy/newmain.c
  proxy/local.c
  proxy/interactor.c)

add_library(keygen STATIC
  import.c)
add_subdirectory(keygen)

add_library(agent STATIC
  sshpubk.c pageant.c aqsync.c)

add_library(guiterminal STATIC
  terminal/terminal.c terminal/bidi.c
  ldisc.c terminal/lineedit.c config.c dialog.c
  $<TARGET_OBJECTS:logging>)

# KiTTY: the config box's activation trace (windows/dialog.c) - what named the
# cause of hknet/KiTTY#38, a click falling through the box to the window
# behind. Kept in the tree for the next window that loses its place, compiled
# in only when asked for, so a released binary carries none of it. The define
# belongs on THIS target: dialog.c is part of guiterminal, so setting it over
# in windows/ reached nothing (found by a check that saw a "trace" build with
# no trace in it).
option(KITTY_CFGBOX_ACTIVATION_TRACE
       "compile the config box's activation trace into the GUI targets" OFF)
if(KITTY_CFGBOX_ACTIVATION_TRACE)
  message(STATUS "KITTY_CFGBOX_ACTIVATION_TRACE on: set KITTY_CFGBOX_ACTIVATION_DEBUG=1 at run time to write %TEMP%/kitty_cfgbox_activation.log")
  target_compile_definitions(guiterminal PRIVATE KITTY_CFGBOX_ACTIVATION_TRACE)
endif()

add_library(noterminal STATIC
  stubs/no-term.c ldisc.c)

add_library(all-backends OBJECT
  pinger.c)

add_library(sftpclient STATIC
  psftpcommon.c)
add_subdirectory(ssh)

add_library(otherbackends STATIC
  $<TARGET_OBJECTS:all-backends>
  $<TARGET_OBJECTS:logging>)
add_subdirectory(otherbackends)

add_executable(testcrypt
  test/testcrypt.c sshpubk.c ssh/crc-attack-detector.c)
target_link_libraries(testcrypt
  keygen crypto utils ${platform_libraries})

add_executable(test_bpp
  test/test_bpp.c
  test/test_random.c
  callback.c
  stubs/no-logging.c
  stubs/no-network.c
  stubs/no-agent.c
  stubs/no-ldisc.c
  stubs/no-timing.c
  stubs/no-storage.c
  stubs/no-ca-config.c
  proxy/noproxy.c)
target_link_libraries(test_bpp
  sshclient crypto network utils ${platform_libraries})

add_executable(test_host_strfoo
  utils/host_strchr_internal.c)
target_compile_definitions(test_host_strfoo PRIVATE TEST)
target_link_libraries(test_host_strfoo utils ${platform_libraries})

add_executable(test_decode_utf8
  utils/decode_utf8.c)
target_compile_definitions(test_decode_utf8 PRIVATE TEST)
target_link_libraries(test_decode_utf8 utils ${platform_libraries})

add_executable(test_unicode_norm
  utils/unicode-norm.c)
target_compile_definitions(test_unicode_norm PRIVATE TEST)
target_link_libraries(test_unicode_norm utils ${platform_libraries})

add_executable(test_tree234
  utils/tree234.c)
target_compile_definitions(test_tree234 PRIVATE TEST)
target_link_libraries(test_tree234 utils ${platform_libraries})

add_executable(test_wildcard
  utils/wildcard.c)
target_compile_definitions(test_wildcard PRIVATE TEST)
target_link_libraries(test_wildcard utils ${platform_libraries})

# KiTTY: unit test for the OSC 7 remote-cwd path validator (self-contained;
# the validators live in kitty/kitty_osc7_parse.h, shared with kitty.c).
add_executable(test_osc7
  test/test_osc7.c)

add_executable(test_cert_expr
  utils/cert-expr.c)
target_compile_definitions(test_cert_expr PRIVATE TEST)
target_link_libraries(test_cert_expr utils ${platform_libraries})

add_executable(bidi_gettype
  terminal/bidi_gettype.c)
target_link_libraries(bidi_gettype guiterminal utils ${platform_libraries})

add_executable(bidi_test
  terminal/bidi_test.c)
target_link_libraries(bidi_test guiterminal utils ${platform_libraries})

add_executable(plink
  ${platform}/plink.c
  stubs/no-lineedit.c)
# Note: if we ever port Plink to a platform where we can't implement a
# serial backend, this be_list command will need to become platform-
# dependent, so that it only sets the SERIAL option on platforms where
# that backend exists. For the moment, though, we have serial port
# backends for both our platforms, so we can do this unconditionally.
be_list(plink Plink SSH SERIAL OTHERBACKENDS)
target_link_libraries(plink
  eventloop noterminal console sshclient otherbackends settings network crypto
  utils
  ${platform_libraries})
installed_program(plink)

add_executable(pscp
  pscp.c)
be_list(pscp PSCP SSH)
target_link_libraries(pscp
  sftpclient eventloop console sshclient settings network crypto utils
  ${platform_libraries})
installed_program(pscp)

add_executable(psftp
  psftp.c)
be_list(psftp PSFTP SSH)
target_link_libraries(psftp
  sftpclient eventloop console sshclient settings network crypto utils
  ${platform_libraries})
installed_program(psftp)

add_executable(psocks
  ${platform}/psocks.c
  psocks.c
  stubs/no-rand.c
  proxy/nocproxy.c
  proxy/nosshproxy.c
  ssh/portfwd.c)
target_link_libraries(psocks
  eventloop console network utils
  ${platform_libraries})

add_executable(test_conf
  test/test_conf.c
  stubs/no-agent.c
  stubs/no-callback.c
  stubs/no-gss.c
  stubs/no-ldisc.c
  stubs/no-network.c
  stubs/no-timing.c
  proxy/noproxy.c # FIXME: move this to stubs
)
be_list(test_conf TestConf SSH SERIAL OTHERBACKENDS)
target_link_libraries(test_conf sshclient otherbackends settings network crypto utils ${platform_libraries})

# KiTTY: the session list's baseline behaviour - enumeration, order, the Folder
# attribute, the name round-trip. Hermetic: it puts the storage layer into file
# mode against a temp directory, so it never reads or writes real sessions.
add_executable(test_sessionlist
  test/test_sessionlist.c
  stubs/no-agent.c
  stubs/no-callback.c
  stubs/no-gss.c
  stubs/no-ldisc.c
  stubs/no-network.c
  stubs/no-timing.c
  proxy/noproxy.c
)
be_list(test_sessionlist TestSessionList SSH SERIAL OTHERBACKENDS)
target_link_libraries(test_sessionlist
  sshclient otherbackends settings network crypto utils ${platform_libraries})

foreach(subdir ${platform} ${extra_dirs})
  add_subdirectory(${subdir})
endforeach()

# Nasty bodge: we'd like to run this command inside unix/CMakeLists,
# adding the 'charset' library to everything that links with utils.
# But that wasn't allowed until cmake 3.13 (see cmake policy CMP0073),
# and we still have a min cmake version less than that. So we do it
# here instead.
if(platform STREQUAL unix)
  target_link_libraries(utils charset)
endif()

# KiTTY: keep the two kitty.ini templates honest at BUILD time rather than at
# release time. The failure it catches is "added an ini key, forgot the
# template" - and its mirror, a template key that nothing reads: `winrol=yes`
# sat in the compiled-in template matching nothing at all, because a key no
# code asks for cannot show up as a missing one.
# find_program, not find_package(Python3), because FindPython3 needs cmake 3.12
# and this tree still declares 3.7. Absent python = check skipped, the same way
# doc/CMakeLists.txt skips the manual without Halibut, so nobody's build breaks
# over a lint.
find_program(KITTY_INI_CHECK_PYTHON NAMES python3 python)
if(KITTY_INI_CHECK_PYTHON)
  # The check reads every ini key literal in these, so they are its inputs.
  if(CMAKE_VERSION VERSION_LESS 3.12)
    file(GLOB KITTY_INI_CHECK_SOURCES
      ${CMAKE_CURRENT_SOURCE_DIR}/kitty/*.c ${CMAKE_CURRENT_SOURCE_DIR}/windows/*.c)
  else()
    file(GLOB KITTY_INI_CHECK_SOURCES CONFIGURE_DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/kitty/*.c ${CMAKE_CURRENT_SOURCE_DIR}/windows/*.c)
  endif()
  # Stamped, so an incremental build that touched none of the inputs does not
  # re-scan ~90 source files for nothing.
  set(KITTY_INI_CHECK_STAMP ${CMAKE_CURRENT_BINARY_DIR}/kitty_ini_check.stamp)
  add_custom_command(
    OUTPUT ${KITTY_INI_CHECK_STAMP}
    COMMAND ${KITTY_INI_CHECK_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-kitty-ini-example.py
    COMMAND ${CMAKE_COMMAND} -E touch ${KITTY_INI_CHECK_STAMP}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-kitty-ini-example.py
            ${CMAKE_CURRENT_SOURCE_DIR}/kitty/kitty_ini.h
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/examples/kitty.ini.example
            ${KITTY_INI_CHECK_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking kitty.ini templates against the options the source reads")
  add_custom_target(check_kitty_ini ALL DEPENDS ${KITTY_INI_CHECK_STAMP})

  # Menu command ids. The mistakes this catches do not break the build - they
  # produce a menu item or a shortcut that silently does nothing, which is only
  # found by using the feature. See the comment in the script.
  set(KITTY_MENUID_CHECK_STAMP ${CMAKE_CURRENT_BINARY_DIR}/kitty_menuid_check.stamp)
  add_custom_command(
    OUTPUT ${KITTY_MENUID_CHECK_STAMP}
    COMMAND ${KITTY_INI_CHECK_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-menu-ids.py
    COMMAND ${CMAKE_COMMAND} -E touch ${KITTY_MENUID_CHECK_STAMP}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-menu-ids.py
            ${CMAKE_CURRENT_SOURCE_DIR}/kitty/kitty.h
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/kitty_rc_additions.h
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/window.c
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking menu command ids for duplicates and 0x10 spacing")
  add_custom_target(check_menu_ids ALL DEPENDS ${KITTY_MENUID_CHECK_STAMP})

  # Dialog accelerators. Same class of mistake as the menu ids: it builds, it
  # looks right, and the only symptom is a key that presses the wrong control.
  # "&Add Key" and "Stop &agent" shared Alt+A in kageant's key list until a user
  # hit it - and Alt+A pressed Stop agent.
  file(GLOB KITTY_ACCEL_CHECK_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/windows/*.rc)
  set(KITTY_ACCEL_CHECK_STAMP ${CMAKE_CURRENT_BINARY_DIR}/kitty_accel_check.stamp)
  add_custom_command(
    OUTPUT ${KITTY_ACCEL_CHECK_STAMP}
    COMMAND ${KITTY_INI_CHECK_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-dialog-accelerators.py
    COMMAND ${CMAKE_COMMAND} -E touch ${KITTY_ACCEL_CHECK_STAMP}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-dialog-accelerators.py
            ${KITTY_ACCEL_CHECK_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking dialog accelerators for duplicates within a dialog")
  add_custom_target(check_dialog_accelerators ALL
                    DEPENDS ${KITTY_ACCEL_CHECK_STAMP})
endif()

configure_file(cmake/cmake.h.in ${GENERATED_SOURCES_DIR}/cmake.h)
