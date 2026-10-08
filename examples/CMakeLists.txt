# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
#
# ============================================================================
#  CytGL examples
#
#  The bundled examples are full applications living in a subdirectory, so this
#  file is built with the *example directory* as its source dir. When the top
#  level enables CYTGL_BUILD_EXAMPLES we pull them in here.
#
#  NOTE: examples are built by CMake only when their toolchain matches. The
#  STM32G431 demo targets a bare-metal ARM part, so it is added as an
#  EXCLUDE_FROM_ALL custom target that shells out to the project's own build
#  script -- that way a host build (or a CI host build) is never broken by it,
#  and the exact CytStudio/SourcePaths-derived build is still reproducible:
#
#      cmake --build build --target cytgl_demo_firmware
#
#  To work on the demo day to day, open
#  examples/STM32G431_CytStudio_Demo/CytGL.cproj in CytStudio instead.
# ============================================================================

find_package(Python3 COMPONENTS Interpreter QUIET)

# The CLI firmware build is driven by a maintainer-side script that is NOT
# published (.gitignore excludes it: it hardcodes the maintainer's toolchain
# path). A clone therefore does not have it, and an unconditional
# add_custom_target() would configure fine and then fail at build time -- so the
# target is only created when the script is actually present. Keep this file
# inside a tree that has the script (the maintainer checkout) and the behaviour
# is exactly what it was.
set(CYTGL_FIRMWARE_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/verify_firmware_build.ps1")

if(WIN32 AND EXISTS "${CYTGL_FIRMWARE_SCRIPT}")
    add_custom_target(cytgl_demo_firmware
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CYTGL_FIRMWARE_SCRIPT}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
        COMMENT "Building the STM32G431 CytGL demo firmware (arm-none-eabi-gcc)"
        USES_TERMINAL
        VERBATIM)
    message(STATUS "Example: cytgl_demo_firmware -> tools/verify_firmware_build.ps1 (local only)")
else()
    add_custom_target(cytgl_demo_firmware
        COMMAND ${CMAKE_COMMAND} -E echo
                "The STM32G431 demo firmware is built with CytStudio: open examples/STM32G431_CytStudio_Demo/CytGL.cproj"
        COMMAND ${CMAKE_COMMAND} -E echo
                "There is no CLI build in a published checkout -- it needs the ARM toolchain and the checked-in project settings."
        VERBATIM)
    message(STATUS "Example: cytgl_demo_firmware -> informational only (open CytGL.cproj in CytStudio to build the firmware)")
endif()
