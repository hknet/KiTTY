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
# KiTTY: shared title-suffix composer ((portable)/(RESTRICTED)/test label).
target_sources(guiterminal PRIVATE ${CMAKE_SOURCE_DIR}/kitty/kitty_title.c)

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
  ${CMAKE_SOURCE_DIR}/kitty/kitty_authenticode.c  # KiTTY: shared verify for the New key launcher
  ${CMAKE_SOURCE_DIR}/kitty/kitty_notice.c    # KiTTY: shared near-the-clock notice window (replaces tray balloons)
  ${CMAKE_SOURCE_DIR}/kitty/kitty_title.c     # KiTTY: shared title-suffix composer
  ${CMAKE_SOURCE_DIR}/kitty/kitty_inilight.c  # KiTTY: light kitty.ini resolver for satellite binaries
  ${CMAKE_SOURCE_DIR}/kitty/kitty_startup_shortcut.c  # KiTTY: Startup-folder shortcut (portable autostart)
  ${CMAKE_SOURCE_DIR}/kitty/kitty_protkey.c  # KiTTY: THE CryptProtectMemory key-blob wrapper - the agent core (pageant.c) uses this shared copy too now (a unix agent build would need this file added beside it)
  ${CMAKE_SOURCE_DIR}/kitty/kitty_hello.c    # KiTTY: Windows Hello presence check (confirm-on-use gating)
  ${CMAKE_SOURCE_DIR}/kitty/kitty_auditlog.c # KiTTY: the audit log's file sink
  pageant.rc)
add_dependencies(pageant generated_licence_h)
target_link_libraries(pageant
  guimisc agent network crypto eventloop utils
  uuid.lib   # KiTTY: CLSID_ShellLink / IID_IShellLink GUIDs (kitty_startup_shortcut.c)
  comctl32.lib   # KiTTY: InitCommonControlsEx for the key-list ListView
  wintrust crypt32 version   # KiTTY: kitty_authenticode.c (New key launcher gate)
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
  ${CMAKE_SOURCE_DIR}/kitty/kitty_title.c    # KiTTY: shared (portable)/(RESTRICTED)/test-label title composer
  ${CMAKE_SOURCE_DIR}/kitty/kitty_protkey.c  # KiTTY: shared CryptProtectMemory key holder (#4)
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
  ${CMAKE_SOURCE_DIR}/kitty/kitty_protkey.c  # KiTTY: shared CryptProtectMemory key holder (#4)
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
  # the .ktx reader, so the old-export test drives the real parser rather than a
  # copy of it (design doc 8.17), plus the two files it needs: the legacy string
  # decoder and the mungestr/salt helpers it decodes through
  ${CMAKE_SOURCE_DIR}/kitty/kitty_settings_load.c
  ${CMAKE_SOURCE_DIR}/kitty/kitty_crypt.c
  ${CMAKE_SOURCE_DIR}/kitty/kitty_commun.c
  no-jump-list.c
  ${CMAKE_SOURCE_DIR}/sshrand.c)
target_include_directories(test_storage_roundtrip PRIVATE
  ${CMAKE_SOURCE_DIR}/kitty ${CMAKE_SOURCE_DIR}/kitty/base64
  ${CMAKE_SOURCE_DIR}/kitty/bcrypt ${CMAKE_SOURCE_DIR}/kitty/md5)
# MOD_PERSO + MASTER_PASSWORD because the .ktx reader is KiTTY code and uses the
# compiled-in constant on its legacy decode path. Matching the shipping targets
# here matters: a test built with different defines would exercise a different
# decoder from the one users run.
target_compile_definitions(test_storage_roundtrip PRIVATE
  MOD_PERSO MASTER_PASSWORD="kitty")
target_link_libraries(test_storage_roundtrip
  crypto utils settings ${platform_libraries}
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

# Bulk-output benchmark: terminal.c and nothing else, so the same source can be
# built by different toolchains and timed. Same stub set as test_terminal.
add_executable(bench_term
  ${CMAKE_SOURCE_DIR}/test/bench_term.c
  ${CMAKE_SOURCE_DIR}/stubs/no-gss.c
  ${CMAKE_SOURCE_DIR}/stubs/no-storage.c
  ${CMAKE_SOURCE_DIR}/stubs/no-timing.c
  no-jump-list.c)
target_link_libraries(bench_term
  guiterminal settings eventloop utils ${platform_libraries})

# OSC 52 regression tests. terminal.c is compiled directly into this target, and
# with MOD_PERSO, for the same reason the kitty target does it: the OSC 52
# handler lives behind that define and guiterminal is built WITHOUT it, so a
# test that merely linked the library would silently exercise nothing at all.
add_executable(test_osc52
  ${CMAKE_SOURCE_DIR}/test/test_osc52.c
  ${CMAKE_SOURCE_DIR}/terminal/terminal.c
  # far2l's own base64, and its include dir below: MOD_FAR2L makes terminal.c
  # include cdecode.h/cencode.h, which live under kitty/far2l/ and are not on the
  # default include path.
  ${CMAKE_SOURCE_DIR}/kitty/far2l/cdecode.c
  ${CMAKE_SOURCE_DIR}/kitty/far2l/cencode.c
  ${CMAKE_SOURCE_DIR}/stubs/no-gss.c
  ${CMAKE_SOURCE_DIR}/stubs/no-storage.c
  ${CMAKE_SOURCE_DIR}/stubs/no-timing.c
  no-jump-list.c)
target_include_directories(test_osc52 PRIVATE ${CMAKE_SOURCE_DIR}/kitty/far2l)
# MOD_FAR2L as well as MOD_PERSO, so this target compiles terminal.c with the SAME
# defines the shipping kitty target does. Without it the far2l APC paths - including
# the accumulation-buffer ceiling that made large clipboard payloads vanish - are
# not compiled here at all, and a test of them would pass by testing nothing. The
# Terminal struct is unaffected either way: far2l's fields are unconditional
# storage precisely so these builds share one layout.
target_compile_definitions(test_osc52 PRIVATE MOD_PERSO MOD_FAR2L)
target_link_libraries(test_osc52
  guiterminal settings eventloop utils ${platform_libraries})

# The master password's Argon2id cost, checked against the limits PuTTY 0.85
# enforces for PPK parameters (argon2_params_bad). Ours are compile-time
# constants, so nothing validates them at run time - and the MPW1 envelope does
# not record them, so a tuning pass would not merely be unvalidated, it would
# lock every existing stored secret out. Links crypto for argon2_params_bad.
add_executable(test_mpw_params
  ${CMAKE_SOURCE_DIR}/test/test_mpw_params.c)
target_link_libraries(test_mpw_params
  crypto utils ${platform_libraries})

add_sources_from_current_dir(test_conf no-jump-list.c handle-wait.c)

# ===================== KiTTY target =====================
set(KITTY_DIR ${CMAKE_SOURCE_DIR}/kitty)
# Stage timers for "where did the time go" builds. OFF by default; the header
# compiles to nothing at all when it is off, so this costs a normal build
# nothing. See kitty/kitty_perf.h for what the buckets mean.
option(KITTY_PERF "instrument kitty.exe with per-stage timers" OFF)
if(KITTY_PERF)
  message(STATUS "KITTY_PERF: kitty.exe will write %TEMP%/kitty_perf.txt at exit")
endif()

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
  # logging.c likewise, for the per-line LogTimestamp: it reaches other binaries
  # through the utils library, built without MOD_PERSO.
  ${CMAKE_SOURCE_DIR}/logging.c
  # NOTE: config.c is deliberately NOT here. kitty/kitty_config.c below is a
  # full KiTTY replacement for it, compiled with MOD_PERSO, and it already wins
  # at link time. Adding config.c as well gives "multiple definition of
  # setup_config_box". Config-box changes go in kitty_config.c - a MOD_PERSO
  # block added to config.c is dead code twice over.
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
  ${KITTY_DIR}/kitty_workplace.c
  ${KITTY_DIR}/kitty_notice.c
  ${KITTY_DIR}/kitty_registry.c
  ${KITTY_DIR}/kitty_ssh.c
  ${KITTY_DIR}/kitty_store.c
  ${KITTY_DIR}/kitty_tools.c
  ${KITTY_DIR}/blocnote/notepad.c
  ${KITTY_DIR}/blocnote/notepad.rc
  ${KITTY_DIR}/kitty_win.c
  ${KITTY_DIR}/kitty_authenticode.c   # KiTTY: shared Authenticode trust + CN gate
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
  ${KITTY_DIR}/kitty_osc52.c
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
target_compile_definitions(kitty PRIVATE MOD_PERSO MOD_BACKGROUNDIMAGE MOD_PORTKNOCKING MOD_ZMODEM MOD_ADB MOD_FAR2L MOD_RECONNECT MOD_PRINTCLIP MOD_DISABLEALTGR MOD_PROXY MOD_TUTTYCOLOR MOD_LAUNCHER MASTER_PASSWORD="kitty" BUILD_VERSION="0.85.1.2" BUILD_TIME="2026-08-21")
target_compile_options(kitty PRIVATE -fcommon)
if(KITTY_PERF)
  # The SAME timers in upstream's own binary. guiterminal already carries the
  # define, so putty.exe's terminal.c is instrumented either way - it just needs
  # the accumulator linked in to have somewhere to count. This is what separates
  # "our MOD_PERSO/MOD_FAR2L additions cost the time" from "this file is simply
  # slower when built for Windows": same payload, same timer, two binaries.
  target_sources(putty PRIVATE ${CMAKE_SOURCE_DIR}/kitty/kitty_perf.c)
  target_compile_definitions(putty PRIVATE KITTY_PERF)
endif()

if(KITTY_PERF)
  # Stage timers (kitty/kitty_perf.h). The define has to reach every file that
  # carries a timer, and terminal.c/network.c reach the binary through the
  # guiterminal and network libraries rather than through this target.
  target_sources(kitty PRIVATE ${CMAKE_SOURCE_DIR}/kitty/kitty_perf.c)
  target_compile_definitions(kitty PRIVATE KITTY_PERF)
  target_link_libraries(kitty psapi)
  target_compile_definitions(guiterminal PRIVATE KITTY_PERF)
  target_compile_definitions(guimisc PRIVATE KITTY_PERF)
  target_compile_definitions(network PRIVATE KITTY_PERF)
endif()
target_include_directories(kitty PRIVATE
  ${KITTY_DIR} ${KITTY_DIR}/base64 ${KITTY_DIR}/far2l ${KITTY_DIR}/bcrypt ${KITTY_DIR}/md5
  ${KITTY_DIR}/url ${KITTY_DIR}/jpeg)
target_link_libraries(kitty
  guiterminal guimisc eventloop sshclient otherbackends settings network crypto utils
  ${KITTY_DIR}/libs/libjpeg_64.a
  ${KITTY_DIR}/libs/base64_64.a ${KITTY_DIR}/libs/bcrypt_64.a
  ${KITTY_DIR}/libs/MD5check_64.a
  ${platform_libraries} wininet wintrust crypt32 msi version
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
  # cmdline.c: see the comment on the kitty target above (and the note there on
  # why config.c must NOT be listed)
  ${CMAKE_SOURCE_DIR}/cmdline.c
  ${CMAKE_SOURCE_DIR}/logging.c
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
  ${KITTY_DIR}/kitty_workplace.c
  ${KITTY_DIR}/kitty_notice.c
  ${KITTY_DIR}/kitty_registry.c
  ${KITTY_DIR}/kitty_ssh.c
  ${KITTY_DIR}/kitty_store.c
  ${KITTY_DIR}/kitty_tools.c
  ${KITTY_DIR}/blocnote/notepad.c
  ${KITTY_DIR}/blocnote/notepad.rc
  ${KITTY_DIR}/kitty_win.c
  ${KITTY_DIR}/kitty_authenticode.c   # KiTTY: shared Authenticode trust + CN gate
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
  ${KITTY_DIR}/kitty_osc52.c
  ${KITTY_DIR}/kitty_specialmenu.c
  ${KITTY_DIR}/kitty_shortcuts.c
  ${KITTY_DIR}/kitty_xfer.c
  ${KITTY_DIR}/kitty_startup_shortcut.c
  ${KITTY_DIR}/mini/mini.c)
be_list(kitty_portable KiTTY SSH SERIAL OTHERBACKENDS)
target_compile_definitions(kitty_portable-be-list PRIVATE MOD_ADB=1)
add_dependencies(kitty_portable generated_licence_h)
target_compile_definitions(kitty_portable PRIVATE MOD_PERSO MOD_PORTABLE MOD_BACKGROUNDIMAGE MOD_PORTKNOCKING MOD_ZMODEM MOD_ADB MOD_FAR2L MOD_RECONNECT MOD_PRINTCLIP MOD_DISABLEALTGR MOD_PROXY MOD_TUTTYCOLOR MOD_LAUNCHER MASTER_PASSWORD="kitty" BUILD_VERSION="0.85.1.2" BUILD_TIME="2026-08-21")
target_compile_options(kitty_portable PRIVATE -fcommon)

set(KITTY_TEST_BUILD_LABEL "" CACHE STRING "Visible label for ad-hoc KiTTY test/debug builds (shown in About boxes); leave empty for releases")
if(KITTY_TEST_BUILD_LABEL)
  target_compile_definitions(kitty PRIVATE KITTY_TEST_BUILD_LABEL="${KITTY_TEST_BUILD_LABEL}")
  target_compile_definitions(kitty_portable PRIVATE KITTY_TEST_BUILD_LABEL="${KITTY_TEST_BUILD_LABEL}")
  target_compile_definitions(guiterminal PRIVATE KITTY_TEST_BUILD_LABEL="${KITTY_TEST_BUILD_LABEL}")
  target_compile_definitions(pageant PRIVATE KITTY_TEST_BUILD_LABEL="${KITTY_TEST_BUILD_LABEL}")
  target_compile_definitions(puttygen PRIVATE KITTY_TEST_BUILD_LABEL="${KITTY_TEST_BUILD_LABEL}")
endif()

# KiTTY: opt-in build switch that tees the Event Log to
# %USERPROFILE%\kitty_netdebug.log (windows/dialog.c). Off by default, so
# releases are unaffected. Worth having as a switch rather than a separate
# source variant: an intermittent disconnect, and any QA that has to know WHY
# something was refused, both need the log as a FILE that outlives the window.
option(KITTY_NETDEBUG "Tee the Event Log to %USERPROFILE%\\kitty_netdebug.log" OFF)
if(KITTY_NETDEBUG)
  target_compile_definitions(kitty PRIVATE MOD_NETDEBUG)
  target_compile_definitions(kitty_portable PRIVATE MOD_NETDEBUG)
endif()

target_include_directories(kitty_portable PRIVATE
  ${KITTY_DIR} ${KITTY_DIR}/base64 ${KITTY_DIR}/far2l ${KITTY_DIR}/bcrypt ${KITTY_DIR}/md5
  ${KITTY_DIR}/url ${KITTY_DIR}/jpeg)
target_link_libraries(kitty_portable
  guiterminal guimisc eventloop sshclient otherbackends settings network crypto utils
  ${KITTY_DIR}/libs/libjpeg_64.a
  ${KITTY_DIR}/libs/base64_64.a ${KITTY_DIR}/libs/bcrypt_64.a
  ${KITTY_DIR}/libs/MD5check_64.a
  ${platform_libraries} wininet wintrust crypt32 msi version
  uuid.lib)   # KiTTY: CLSID_ShellLink GUIDs (kitty_startup_shortcut.c, launcher autostart)
set_target_properties(kitty_portable PROPERTIES WIN32_EXECUTABLE ON)

# ===================================================================
# MOD_PERSO COVERAGE CHECK - fail the build, not the user
# ===================================================================
#
# The single most expensive recurring mistake in this port: writing KiTTY code
# inside "#ifdef MOD_PERSO" in a SHARED PuTTY source file that reaches the
# executable through a library built WITHOUT that define. The preprocessor
# deletes the code, the build is clean, zero warnings, and the feature simply is
# not there. It has cost this project cmdline.c, terminal.c and - most recently -
# config.c, where a guard meant to stop a saved session being destroyed was
# deployed, declared working, and then destroyed a saved session.
#
# Comments were written each time saying "remember this". Comments do not fail
# builds. This does.
#
# The rule: any shared source file (repo root, terminal/, windows/ - anything
# outside kitty/, which is always compiled with the define) that contains a
# "#ifdef MOD_PERSO" style guard MUST be listed directly in the kitty target's
# sources, where the per-target define reaches it. If it is not, configuring
# stops here with an explanation.
#
# Deliberately NOT a fix-it-for-you: adding a file to the executable changes
# which objects win at link time, and that is a decision to take deliberately.
# The alternative, when the code does not really need the define, is to drop the
# guard entirely - see kitty_dlg_confirm() in controls.c and the note in
# dialog.c.
file(GLOB _kitty_shared_sources
  "${CMAKE_SOURCE_DIR}/*.c"
  "${CMAKE_SOURCE_DIR}/terminal/*.c"
  "${CMAKE_SOURCE_DIR}/windows/*.c"
  # proxy/ and ssh/ added 2026-08-03: the check missed them, and proxy/sshproxy.c
  # then needed a KiTTY change. It compiles into the shared `crypto` library, so a
  # MOD_PERSO guard there would have been dead exactly like config.c's was.
  "${CMAKE_SOURCE_DIR}/proxy/*.c"
  "${CMAKE_SOURCE_DIR}/ssh/*.c")

get_target_property(_kitty_target_sources kitty SOURCES)
set(_kitty_covered "")
foreach(_src ${_kitty_target_sources})
  get_filename_component(_abs "${_src}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  list(APPEND _kitty_covered "${_abs}")
endforeach()

set(_kitty_uncovered "")
foreach(_src ${_kitty_shared_sources})
  file(READ "${_src}" _content)
  # A real preprocessor guard, not a passing mention in a comment. ANCHORED to
  # the start of a line (whitespace only before the #), because a directive must
  # begin its line while a comment about one is preceded by " * " or "/* ".
  # Without the anchor this check flagged proxy/sshproxy.c for a COMMENT
  # explaining why it deliberately does not use the guard - the check tripping
  # over its own documentation.
  if(_content MATCHES "(^|\n)[ \t]*#[ \t]*if(n?def[ \t]+MOD_PERSO|[ \t]+[^\n]*defined[ \t]*\\([ \t]*MOD_PERSO)")
    get_filename_component(_abs "${_src}" ABSOLUTE)
    if(NOT "${_abs}" IN_LIST _kitty_covered)
      list(APPEND _kitty_uncovered "${_abs}")
    endif()
  endif()
endforeach()

if(_kitty_uncovered)
  string(REPLACE ";" "\n    " _kitty_uncovered_pretty "${_kitty_uncovered}")
  message(FATAL_ERROR
    "MOD_PERSO coverage check failed.\n\n"
    "These shared source files contain '#ifdef MOD_PERSO' but are NOT compiled\n"
    "directly into the kitty target, so they reach the binary through a library\n"
    "built WITHOUT MOD_PERSO. Every guarded line in them is silently deleted by\n"
    "the preprocessor - the build succeeds and the code is not there:\n\n"
    "    ${_kitty_uncovered_pretty}\n\n"
    "Fix it one of two ways:\n"
    "  1. Add the file to BOTH the kitty and kitty_portable target source lists\n"
    "     (see cmdline.c and config.c there for the pattern), or\n"
    "  2. Drop the MOD_PERSO guard if the code does not need it - see\n"
    "     kitty_dlg_confirm() in windows/controls.c.\n")
endif()
