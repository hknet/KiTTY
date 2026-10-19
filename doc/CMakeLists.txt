cmake_minimum_required(VERSION 3.7...3.28)
project(putty-documentation LANGUAGES)

# This build script can be run standalone, or included as a
# subdirectory of the main build system. KiTTY: it builds the manual
# only - PuTTY's Unix man pages (man-*.but) are not part of this
# Windows-only suite and were removed.

include(FindGit)
include(FindPerl)
find_program(HALIBUT halibut)

set(doc_outputs)

if(HALIBUT AND PERL_EXECUTABLE)
  # Build the main manual, which requires not only Halibut, but also
  # Perl to run cmake/licence.pl to generate the copyright and licence
  # sections from the master data outside this directory.

  # If this is a source archive in which a fixed version.but was
  # provided, use that. Otherwise, infer one from the git checkout (if
  # possible).

  set(manual_dependencies) # extra target names to depend on

  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/version.but)
    set(VERSION_BUT ${CMAKE_CURRENT_SOURCE_DIR}/version.but)
  else()
    set(VERSION_BUT ${CMAKE_CURRENT_BINARY_DIR}/cmake_version.but)
    set(INTERMEDIATE_VERSION_BUT ${VERSION_BUT}.tmp)
    add_custom_target(check_git_commit_for_doc
      BYPRODUCTS ${INTERMEDIATE_VERSION_BUT}
      COMMAND ${CMAKE_COMMAND}
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -DOUTPUT_FILE=${INTERMEDIATE_VERSION_BUT}
        -DOUTPUT_TYPE=halibut
        -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/gitcommit.cmake
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/gitcommit.cmake
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
      COMMENT "Checking current git commit")
    add_custom_target(cmake_version_but
      BYPRODUCTS ${VERSION_BUT}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${INTERMEDIATE_VERSION_BUT} ${VERSION_BUT}
      DEPENDS check_git_commit_for_doc ${INTERMEDIATE_VERSION_BUT}
      COMMENT "Updating cmake_version.but")
    set(manual_dependencies ${manual_dependencies} cmake_version_but)
  endif()

  add_custom_target(copy_but
    BYPRODUCTS copy.but
    COMMAND ${PERL_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/licence.pl
      --copyrightdoc -o copy.but
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/licence.pl ${CMAKE_CURRENT_SOURCE_DIR}/../LICENCE)
  add_custom_target(licence_but
    BYPRODUCTS licence.but
    COMMAND ${PERL_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/licence.pl
      --licencedoc -o licence.but
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/licence.pl ${CMAKE_CURRENT_SOURCE_DIR}/../LICENCE)
  set(manual_dependencies ${manual_dependencies} copy_but licence_but)

  # KiTTY: the "KiTTY additions" chapter is GENERATED from FEATURES.md, so the
  # shipped help covers our features without a second copy of the text to
  # drift out of date.
  add_custom_target(kitty_but
    BYPRODUCTS kitty.but
    COMMAND ${PERL_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/features2but.pl
      ${CMAKE_CURRENT_SOURCE_DIR}/../FEATURES.md -o kitty.but
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../tools/features2but.pl
      ${CMAKE_CURRENT_SOURCE_DIR}/../FEATURES.md)
  set(manual_dependencies ${manual_dependencies} kitty_but)

  set(manual_sources
    ${CMAKE_CURRENT_BINARY_DIR}/copy.but
    ${CMAKE_CURRENT_SOURCE_DIR}/blurb.but
    ${CMAKE_CURRENT_SOURCE_DIR}/intro.but
    ${CMAKE_CURRENT_SOURCE_DIR}/gs.but
    ${CMAKE_CURRENT_SOURCE_DIR}/using.but
    ${CMAKE_CURRENT_SOURCE_DIR}/config.but
    # KiTTY: the settings the fork ADDS to that configuration box, documented
    # the same way and in the same order as the panels. Written by hand, unlike
    # kitty.but below, which is generated from FEATURES.md: this is the
    # reference the Help button on a control opens, that one is the feature
    # overview.
    ${CMAKE_CURRENT_SOURCE_DIR}/kitty-config.but
    ${CMAKE_CURRENT_SOURCE_DIR}/pscp.but
    ${CMAKE_CURRENT_SOURCE_DIR}/psftp.but
    ${CMAKE_CURRENT_SOURCE_DIR}/plink.but
    ${CMAKE_CURRENT_SOURCE_DIR}/pubkey.but
    ${CMAKE_CURRENT_SOURCE_DIR}/pageant.but
    ${CMAKE_CURRENT_BINARY_DIR}/kitty.but
    ${CMAKE_CURRENT_SOURCE_DIR}/errors.but
    ${CMAKE_CURRENT_SOURCE_DIR}/faq.but
    ${CMAKE_CURRENT_SOURCE_DIR}/feedback.but
    ${CMAKE_CURRENT_SOURCE_DIR}/pubkeyfmt.but
    ${CMAKE_CURRENT_SOURCE_DIR}/folderstore.but
    ${CMAKE_CURRENT_BINARY_DIR}/licence.but
    ${CMAKE_CURRENT_SOURCE_DIR}/pgpkeys.but
    ${CMAKE_CURRENT_SOURCE_DIR}/sshnames.but
    ${CMAKE_CURRENT_SOURCE_DIR}/authplugin.but
    ${CMAKE_CURRENT_SOURCE_DIR}/privacy.but
    ${CMAKE_CURRENT_SOURCE_DIR}/index.but
    ${VERSION_BUT})

  # The HTML manual goes in a subdirectory, for convenience.
  set(html_dir ${CMAKE_CURRENT_BINARY_DIR}/html)
  file(MAKE_DIRECTORY ${html_dir})
  add_custom_command(OUTPUT ${html_dir}/index.html
    COMMAND ${HALIBUT} --html ${manual_sources}
    WORKING_DIRECTORY ${html_dir}
    DEPENDS ${manual_sources} ${manual_dependencies})
  list(APPEND doc_outputs ${html_dir}/index.html)

  # Windows help. KiTTY: blurb.but's \cfg{chm-filename} names the output
  # kitty.chm (what windows/help.c looks for beside the exe), so the OUTPUT
  # declared here must match or the target never sees its own product.
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/chmextra.but
    "\\cfg{chm-extra-file}{${CMAKE_CURRENT_SOURCE_DIR}/chm.css}{chm.css}\n")
  add_custom_command(OUTPUT kitty.chm
    COMMAND ${HALIBUT} --chm chmextra.but ${manual_sources}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${manual_sources} ${manual_dependencies})
  list(APPEND doc_outputs kitty.chm)

  # Plain text. (kittydoc.txt: named by blurb.but's \cfg{text-filename}.)
  add_custom_command(OUTPUT kittydoc.txt
    COMMAND ${HALIBUT} --text ${manual_sources}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${manual_sources} ${manual_dependencies})
  list(APPEND doc_outputs kittydoc.txt)

  # PDF. (We don't ship this; so it's only built on explicit request.
  # kitty.pdf: named by blurb.but's \cfg{pdf-filename}.)
  add_custom_command(OUTPUT kitty.pdf
    COMMAND ${HALIBUT} --pdf ${manual_sources}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${manual_sources} ${manual_dependencies})
  add_custom_target(pdf DEPENDS kitty.pdf)
endif()

add_custom_target(doc DEPENDS ${doc_outputs})

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # If we're doing a cmake from just the doc subdir, we expect the
  # user to want to make all the documentation, including HTML and so
  # forth. (What else would be the point?)
  #
  # But if we're included from the main makefile, the manual is left to
  # a separate 'make doc' target which the user can invoke if they need
  # to.
  add_custom_target(doc-default ALL DEPENDS doc)
endif()
