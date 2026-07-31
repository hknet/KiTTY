set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_sources_from_current_dir(utils
  utils/agent_mutex_name.c
  utils/agent_named_pipe_name.c
  utils/arm_arch_queries.c
  utils/aux_match_opt.c
  utils/blink_time.c
  utils/centre_window.c
  utils/cmdline_arg.c
  utils/cryptoapi.c
  utils/defaults.c
  utils/dll_hijacking_protection.c
  utils/dputs.c
  utils/escape_registry_key.c
  utils/filename.c
  utils/fontspec.c
  utils/getdlgitemtext_alloc.c
  utils/get_system_dir.c
  utils/get_username.c
  utils/gui-timing.c
  utils/interprocess_mutex.c
  utils/is_console_handle.c
  utils/load_system32_dll.c
  utils/ltime.c
  utils/makedlgitemborderless.c
  utils/make_spr_sw_abort_winerror.c
  utils/message_box.c
  utils/minefield.c
  utils/open_for_write_would_lose_data.c
  utils/pgp_fingerprints_msgbox.c
  utils/platform_get_x_display.c
  utils/registry.c
  utils/request_file.c
  utils/screenshot.c
  utils/security.c
  utils/shinydialogbox.c
  utils/split_into_argv.c
  utils/split_into_argv_w.c
  utils/subprocess_waiter.c
  utils/version.c
  utils/win_strerror.c
  unicode.c)
if(NOT HAVE_STRTOUMAX)
  add_sources_from_current_dir(utils utils/strtoumax.c)
endif()
add_sources_from_current_dir(eventloop
  cliloop.c handle-wait.c)
add_sources_from_current_dir(console
  select-cli.c nohelp.c console.c)
add_sources_from_current_dir(settings
  storage.c)
# KiTTY: base64 codec used by storage.c's at-rest secret handling, split out to
# shrink storage.c's divergence from upstream. Added by absolute path since it
# lives in the kitty/ dir, not windows/.
target_sources(settings PRIVATE ${CMAKE_SOURCE_DIR}/kitty/kitty_b64.c)
# KiTTY: the fork half of storage.c (runtime registry root, portable file
# backend, at-rest credential crypto) — split out of storage.c likewise.
# NOTE: like storage.c itself, this must ALSO be listed in the puttygen and
# kittygen_cli source lists below, or those targets fail to link.
target_sources(settings PRIVATE ${CMAKE_SOURCE_DIR}/kitty/kitty_storage.c)
# KiTTY: storage.c encrypts stored passwords at rest via DPAPI (CryptProtectData/
# CryptUnprotectData in crypt32) and decrypts legacy old-KiTTY passwords via the
# bcrypt archive. PUBLIC so every consumer (CLI tools incl.) links them; the
# bcrypt include dir lets storage.c see nbcrypt.h.
target_link_libraries(settings PUBLIC crypt32 ${CMAKE_SOURCE_DIR}/kitty/libs/bcrypt_64.a)
add_sources_from_current_dir(network
  network.c handle-socket.c named-pipe-client.c named-pipe-server.c local-proxy.c x11.c)
add_sources_from_current_dir(sshcommon
  noise.c)
add_sources_from_current_dir(sshclient
  agent-client.c gss.c sharing.c)
add_sources_from_current_dir(sftpclient
  sftp.c)
add_sources_from_current_dir(otherbackends
  serial.c)
add_sources_from_current_dir(agent
  agent-client.c)
add_sources_from_current_dir(guiterminal
  dialog.c controls.c config.c printing.c jump-list.c sizetip.c)
add_dependencies(guiterminal generated_licence_h) # dialog.c uses licence.h
# KiTTY: self-contained aux-window placement/position-memory helper, called from
# dialog.c's AboutProc (and reusable by the other suite exes).
target_sources(guiterminal PRIVATE ${CMAKE_SOURCE_DIR}/kitty/kitty_auxpos.c)

# This object awkwardly needs to live in the network library as well
# as the eventloop library, in case it didn't get pulled in from the
# latter before handle-socket.c needed it.
add_library(handle-io OBJECT
  handle-io.c)
target_sources(eventloop PRIVATE $<TARGET_OBJECTS:handle-io>)
target_sources(network PRIVATE $<TARGET_OBJECTS:handle-io>)

add_library(guimisc STATIC
  select-gui.c)

add_executable(pageant
  pageant.c
  help.c
  ${CMAKE_SOURCE_DIR}/kitty/kitty_auxpos.c   # KiTTY: About-box placement + memory
  ${CMAKE_SOURCE_DIR}/kitty/kitty_pageant.c   # KiTTY: kageant additions (split out of pageant.c)
  ${CMAKE_SOURCE_DIR}/kitty/kitty_inilight.c  # KiTTY: light kitty.ini resolver for satellite binaries
  ${CMAKE_SOURCE_DIR}/kitty/kitty_startup_shortcut.c  # KiTTY: Startup-folder shortcut (portable autostart)
  pageant.rc)
add_dependencies(pageant generated_licence_h)
target_link_libraries(pageant
  guimisc agent network crypto eventloop utils
  uuid.lib   # KiTTY: CLSID_ShellLink / IID_IShellLink GUIDs (kitty_startup_shortcut.c)
  ${platform_libraries})  # order matters (single-pass static link): network ->
                          # eventloop (network.c's new schedule_timer pulls
                          # timing.c) -> utils (timing.c's timer_change_notify +
                          # gui-timing.c's run_timers resolve). pageant itself uses
                          # no timers, so timing.c isn't pulled earlier.
set_target_properties(pageant PROPERTIES WIN32_EXECUTABLE ON)
installed_program(pageant)

add_sources_from_current_dir(plink no-jump-list.c nohelp.c plink.rc)
add_dependencies(plink generated_licence_h)

add_sources_from_current_dir(pscp no-jump-list.c nohelp.c pscp.rc)
add_dependencies(pscp generated_licence_h)

add_sources_from_current_dir(psftp no-jump-list.c nohelp.c psftp.rc)
add_dependencies(psftp generated_licence_h)

add_sources_from_current_dir(psocks nohelp.c)

add_executable(putty
  window.c
  putty.c
  help.c
  kitty_config_stubs.c
  ${CMAKE_SOURCE_DIR}/stubs/no-console.c
  putty.rc)
be_list(putty PuTTY SSH SERIAL OTHERBACKENDS)
add_dependencies(putty generated_licence_h)
target_link_libraries(putty
  guiterminal guimisc eventloop sshclient otherbackends settings network crypto
  utils
  ${platform_libraries})
set_target_properties(putty PROPERTIES WIN32_EXECUTABLE ON)
installed_program(putty)

add_executable(puttytel
  window.c
  putty.c
  help.c
  kitty_config_stubs.c
  ${CMAKE_SOURCE_DIR}/stubs/no-gss.c
  ${CMAKE_SOURCE_DIR}/stubs/no-ca-config.c
  ${CMAKE_SOURCE_DIR}/stubs/no-console.c
  ${CMAKE_SOURCE_DIR}/stubs/no-rand.c
  ${CMAKE_SOURCE_DIR}/stubs/no-dit.c
  ${CMAKE_SOURCE_DIR}/proxy/nocproxy.c
  ${CMAKE_SOURCE_DIR}/proxy/nosshproxy.c
  puttytel.rc)
be_list(puttytel KiTTYtel SERIAL OTHERBACKENDS)
add_dependencies(puttytel generated_licence_h)
target_link_libraries(puttytel
  guiterminal guimisc eventloop otherbackends settings network utils
  ${platform_libraries})
set_target_properties(puttytel PROPERTIES WIN32_EXECUTABLE ON)
installed_program(puttytel)

add_executable(puttygen
  puttygen.c
  ${CMAKE_SOURCE_DIR}/kitty/kitty_auxpos.c   # KiTTY: About-box placement + memory
  ${CMAKE_SOURCE_DIR}/stubs/no-timing.c
  noise.c
  no-jump-list.c
  storage.c
  ${CMAKE_SOURCE_DIR}/kitty/kitty_b64.c   # base64 codec used by storage.c (split out of it)
  ${CMAKE_SOURCE_DIR}/kitty/kitty_storage.c   # fork half of storage.c (split out of it)
  help.c
  ${CMAKE_SOURCE_DIR}/sshpubk.c
  ${CMAKE_SOURCE_DIR}/sshrand.c
  controls.c
  puttygen.rc)
add_dependencies(puttygen generated_licence_h)
target_link_libraries(puttygen
  keygen guimisc crypto utils
  ${platform_libraries} crypt32 ${CMAKE_SOURCE_DIR}/kitty/libs/bcrypt_64.a)  # storage.c password crypto (DPAPI + legacy bcrypt)
set_target_properties(puttygen PROPERTIES WIN32_EXECUTABLE ON)
installed_program(puttygen)

add_executable(kittygen_cli
  ${CMAKE_SOURCE_DIR}/cmdgen.c
  ${CMAKE_SOURCE_DIR}/stubs/no-timing.c
  noise.c
  storage.c
  ${CMAKE_SOURCE_DIR}/kitty/kitty_b64.c   # base64 codec used by storage.c (split out of it)
  ${CMAKE_SOURCE_DIR}/kitty/kitty_storage.c   # fork half of storage.c (split out of it)
  no-jump-list.c
  nohelp.c
  keygen-noise.c
  ${CMAKE_SOURCE_DIR}/sshpubk.c
  ${CMAKE_SOURCE_DIR}/sshrand.c
  kittygen_cli.rc)
add_dependencies(kittygen_cli generated_licence_h)
target_link_libraries(kittygen_cli
  keygen console crypto utils
  ${platform_libraries} crypt32 ${CMAKE_SOURCE_DIR}/kitty/libs/bcrypt_64.a)  # storage.c password crypto (DPAPI + legacy bcrypt)
# console subsystem - no WIN32_EXECUTABLE
installed_program(kittygen_cli)


if(HAVE_CONPTY)
  add_executable(pterm
    window.c
    pterm.c
    help.c
    conpty.c
    kitty_config_stubs.c
    ${CMAKE_SOURCE_DIR}/stubs/no-gss.c
    ${CMAKE_SOURCE_DIR}/stubs/no-ca-config.c
    ${CMAKE_SOURCE_DIR}/stubs/no-console.c
    ${CMAKE_SOURCE_DIR}/stubs/no-rand.c
    ${CMAKE_SOURCE_DIR}/stubs/no-dit.c
    ${CMAKE_SOURCE_DIR}/proxy/nosshproxy.c
    pterm.rc)
  be_list(pterm pterm)
  add_dependencies(pterm generated_licence_h)
  target_link_libraries(pterm
    guiterminal guimisc eventloop settings network utils
    ${platform_libraries})
  set_target_properties(pterm PROPERTIES WIN32_EXECUTABLE ON)
  installed_program(pterm)
else()
  message("ConPTY not available; cannot build Windows pterm")
endif()

add_executable(test_split_into_argv
  test/test_split_into_argv.c)
target_compile_definitions(test_split_into_argv PRIVATE TEST)
target_link_libraries(test_split_into_argv utils ${platform_libraries})

add_executable(test_screenshot
  test/test_screenshot.c)
target_link_libraries(test_screenshot utils ${platform_libraries})

# KiTTY: storage at-rest crypto round-trip regression test (registry DPAPI +
# portable master-password paths through the real settings API). Compiles
# storage.c directly, like the keygen tools. Run on Windows; writes only a
# zz-* scratch session in the hive (deleted) and a %TEMP% tree (deleted).
add_executable(test_storage_roundtrip
  test/test_storage_roundtrip.c
  ${CMAKE_SOURCE_DIR}/stubs/no-timing.c
  noise.c
  storage.c
  ${CMAKE_SOURCE_DIR}/kitty/kitty_b64.c
  ${CMAKE_SOURCE_DIR}/kitty/kitty_storage.c
  ${CMAKE_SOURCE_DIR}/kitty/kitty_mpw.c
  no-jump-list.c
  ${CMAKE_SOURCE_DIR}/sshrand.c)
target_link_libraries(test_storage_roundtrip
  crypto utils ${platform_libraries}
  crypt32 ${CMAKE_SOURCE_DIR}/kitty/libs/bcrypt_64.a)

add_executable(test_lineedit
  ${CMAKE_SOURCE_DIR}/test/test_lineedit.c
  ${CMAKE_SOURCE_DIR}/stubs/no-gss.c
  ${CMAKE_SOURCE_DIR}/stubs/no-logging.c
  ${CMAKE_SOURCE_DIR}/stubs/no-printing.c
  ${CMAKE_SOURCE_DIR}/stubs/no-storage.c
  ${CMAKE_SOURCE_DIR}/stubs/no-timing.c
  no-jump-list.c)
target_link_libraries(test_lineedit
  guiterminal settings eventloop utils ${platform_libraries})

add_executable(test_terminal
  ${CMAKE_SOURCE_DIR}/test/test_terminal.c
  ${CMAKE_SOURCE_DIR}/stubs/no-gss.c
  ${CMAKE_SOURCE_DIR}/stubs/no-storage.c
  ${CMAKE_SOURCE_DIR}/stubs/no-timing.c
  no-jump-list.c)
target_link_libraries(test_terminal
  guiterminal settings eventloop utils ${platform_libraries})

add_sources_from_current_dir(test_conf no-jump-list.c handle-wait.c)

# ===================== KiTTY target =====================
set(KITTY_DIR ${CMAKE_SOURCE_DIR}/kitty)
add_executable(kitty
  window.c
  putty.c
  help.c
  # terminal.c compiled directly into the kitty target (not via guiterminal)
  # so it can carry MOD_FAR2L; the directly-listed object takes precedence over
  # guiterminal's terminal.o, leaving other binaries untouched.
  ${CMAKE_SOURCE_DIR}/terminal/terminal.c
  # cmdline.c likewise, so its MOD_PERSO section (the "ssh:" URL form) is
  # actually compiled: the settings library is built without MOD_PERSO, where
  # such a block would be silently dead.
  ${CMAKE_SOURCE_DIR}/cmdline.c
  ${KITTY_DIR}/far2l/cdecode.c
  ${KITTY_DIR}/far2l/cencode.c
  ${CMAKE_SOURCE_DIR}/stubs/no-console.c
  kitty.rc
  ${KITTY_DIR}/kitty.c
  ${KITTY_DIR}/kitty_commun.c
  ${KITTY_DIR}/kitty_crypt.c
  ${KITTY_DIR}/kitty_mpw.c
  ${KITTY_DIR}/kitty_mpw_gui.c
  ${KITTY_DIR}/kitty_image.c
  ${KITTY_DIR}/kitty_proxy.c
  ${KITTY_DIR}/kitty_proxy_gui.c
  ${KITTY_DIR}/kitty_registry.c
  ${KITTY_DIR}/kitty_ssh.c
  ${KITTY_DIR}/kitty_store.c
  ${KITTY_DIR}/kitty_tools.c
  ${KITTY_DIR}/blocnote/notepad.c
  ${KITTY_DIR}/blocnote/notepad.rc
  ${KITTY_DIR}/kitty_win.c
  ${KITTY_DIR}/winfont_fallback.c
  ${KITTY_DIR}/url/urlhack.c
  ${KITTY_DIR}/url/re_lib/regexp.c
  ${KITTY_DIR}/kitty_url.c
  ${KITTY_DIR}/kitty_bridge.c
  ${KITTY_DIR}/kitty_settings_forced.c
  ${KITTY_DIR}/kitty_settings_load.c
  ${KITTY_DIR}/kitty_zmodem.c
  ${KITTY_DIR}/kitty_adb.c
  ${KITTY_DIR}/kitty_rutty.c
  ${KITTY_DIR}/kitty_config.c
  ${KITTY_DIR}/kitty_colours.c
  ${KITTY_DIR}/kitty_inputbox.c
  ${KITTY_DIR}/kitty_specialmenu.c
  ${KITTY_DIR}/kitty_shortcuts.c
  ${KITTY_DIR}/kitty_xfer.c
  ${KITTY_DIR}/kitty_startup_shortcut.c
  ${KITTY_DIR}/mini/mini.c)
be_list(kitty KiTTY SSH SERIAL OTHERBACKENDS)
# kitty's backend list also includes the ADB backend (MOD_ADB) - applied to the
# per-target be-list object so other binaries' be_list.c is unaffected.
target_compile_definitions(kitty-be-list PRIVATE MOD_ADB=1)
set_source_files_properties(${KITTY_DIR}/blocnote/notepad.c PROPERTIES COMPILE_DEFINITIONS NOMAIN)
set_source_files_properties(${KITTY_DIR}/blocnote/notepad.rc PROPERTIES COMPILE_DEFINITIONS NOTEPAD_EMBEDDED)
add_dependencies(kitty generated_licence_h)
target_compile_definitions(kitty PRIVATE MOD_PERSO MOD_BACKGROUNDIMAGE MOD_PORTKNOCKING MOD_ZMODEM MOD_ADB MOD_FAR2L MOD_RECONNECT MOD_PRINTCLIP MOD_DISABLEALTGR MOD_PROXY MOD_TUTTYCOLOR MOD_LAUNCHER MASTER_PASSWORD="kitty" BUILD_VERSION="0.84.1.66" BUILD_TIME="2026-07-29")
target_compile_options(kitty PRIVATE -fcommon)
target_include_directories(kitty PRIVATE
  ${KITTY_DIR} ${KITTY_DIR}/base64 ${KITTY_DIR}/far2l ${KITTY_DIR}/bcrypt ${KITTY_DIR}/md5
  ${KITTY_DIR}/url ${KITTY_DIR}/jpeg)
target_link_libraries(kitty
  guiterminal guimisc eventloop sshclient otherbackends settings network crypto utils
  ${KITTY_DIR}/libs/libjpeg_64.a
  ${KITTY_DIR}/libs/base64_64.a ${KITTY_DIR}/libs/bcrypt_64.a
  ${KITTY_DIR}/libs/MD5check_64.a
  ${platform_libraries} wininet wintrust crypt32 msi
  uuid.lib)   # KiTTY: CLSID_ShellLink GUIDs (kitty_startup_shortcut.c, launcher autostart)
set_target_properties(kitty PROPERTIES WIN32_EXECUTABLE ON)

# ===================== KiTTY PORTABLE target =====================
# Identical to the kitty target above, but with MOD_PORTABLE also defined so
# the binary defaults to file/dir storage (savemode=dir) instead of the
# registry. Produces build-*/kitty_portable.exe.
add_executable(kitty_portable
  window.c
  putty.c
  help.c
  ${CMAKE_SOURCE_DIR}/terminal/terminal.c
  # cmdline.c: see the comment on the kitty target above
  ${CMAKE_SOURCE_DIR}/cmdline.c
  ${KITTY_DIR}/far2l/cdecode.c
  ${KITTY_DIR}/far2l/cencode.c
  ${CMAKE_SOURCE_DIR}/stubs/no-console.c
  kitty.rc
  ${KITTY_DIR}/kitty.c
  ${KITTY_DIR}/kitty_commun.c
  ${KITTY_DIR}/kitty_crypt.c
  ${KITTY_DIR}/kitty_mpw.c
  ${KITTY_DIR}/kitty_mpw_gui.c
  ${KITTY_DIR}/kitty_image.c
  ${KITTY_DIR}/kitty_proxy.c
  ${KITTY_DIR}/kitty_proxy_gui.c
  ${KITTY_DIR}/kitty_registry.c
  ${KITTY_DIR}/kitty_ssh.c
  ${KITTY_DIR}/kitty_store.c
  ${KITTY_DIR}/kitty_tools.c
  ${KITTY_DIR}/blocnote/notepad.c
  ${KITTY_DIR}/blocnote/notepad.rc
  ${KITTY_DIR}/kitty_win.c
  ${KITTY_DIR}/winfont_fallback.c
  ${KITTY_DIR}/url/urlhack.c
  ${KITTY_DIR}/url/re_lib/regexp.c
  ${KITTY_DIR}/kitty_url.c
  ${KITTY_DIR}/kitty_bridge.c
  ${KITTY_DIR}/kitty_settings_forced.c
  ${KITTY_DIR}/kitty_settings_load.c
  ${KITTY_DIR}/kitty_zmodem.c
  ${KITTY_DIR}/kitty_adb.c
  ${KITTY_DIR}/kitty_rutty.c
  ${KITTY_DIR}/kitty_config.c
  ${KITTY_DIR}/kitty_colours.c
  ${KITTY_DIR}/kitty_inputbox.c
  ${KITTY_DIR}/kitty_specialmenu.c
  ${KITTY_DIR}/kitty_shortcuts.c
  ${KITTY_DIR}/kitty_xfer.c
  ${KITTY_DIR}/kitty_startup_shortcut.c
  ${KITTY_DIR}/mini/mini.c)
be_list(kitty_portable KiTTY SSH SERIAL OTHERBACKENDS)
target_compile_definitions(kitty_portable-be-list PRIVATE MOD_ADB=1)
add_dependencies(kitty_portable generated_licence_h)
target_compile_definitions(kitty_portable PRIVATE MOD_PERSO MOD_PORTABLE MOD_BACKGROUNDIMAGE MOD_PORTKNOCKING MOD_ZMODEM MOD_ADB MOD_FAR2L MOD_RECONNECT MOD_PRINTCLIP MOD_DISABLEALTGR MOD_PROXY MOD_TUTTYCOLOR MOD_LAUNCHER MASTER_PASSWORD="kitty" BUILD_VERSION="0.84.1.66" BUILD_TIME="2026-07-29")
target_compile_options(kitty_portable PRIVATE -fcommon)

set(KITTY_TEST_BUILD_LABEL "" CACHE STRING "Visible label for ad-hoc KiTTY test/debug builds (shown in About boxes); leave empty for releases")
if(KITTY_TEST_BUILD_LABEL)
  target_compile_definitions(kitty PRIVATE KITTY_TEST_BUILD_LABEL="${KITTY_TEST_BUILD_LABEL}")
  target_compile_definitions(kitty_portable PRIVATE KITTY_TEST_BUILD_LABEL="${KITTY_TEST_BUILD_LABEL}")
  target_compile_definitions(guiterminal PRIVATE KITTY_TEST_BUILD_LABEL="${KITTY_TEST_BUILD_LABEL}")
  target_compile_definitions(pageant PRIVATE KITTY_TEST_BUILD_LABEL="${KITTY_TEST_BUILD_LABEL}")
  target_compile_definitions(puttygen PRIVATE KITTY_TEST_BUILD_LABEL="${KITTY_TEST_BUILD_LABEL}")
endif()

target_include_directories(kitty_portable PRIVATE
  ${KITTY_DIR} ${KITTY_DIR}/base64 ${KITTY_DIR}/far2l ${KITTY_DIR}/bcrypt ${KITTY_DIR}/md5
  ${KITTY_DIR}/url ${KITTY_DIR}/jpeg)
target_link_libraries(kitty_portable
  guiterminal guimisc eventloop sshclient otherbackends settings network crypto utils
  ${KITTY_DIR}/libs/libjpeg_64.a
  ${KITTY_DIR}/libs/base64_64.a ${KITTY_DIR}/libs/bcrypt_64.a
  ${KITTY_DIR}/libs/MD5check_64.a
  ${platform_libraries} wininet wintrust crypt32 msi
  uuid.lib)   # KiTTY: CLSID_ShellLink GUIDs (kitty_startup_shortcut.c, launcher autostart)
set_target_properties(kitty_portable PROPERTIES WIN32_EXECUTABLE ON)
