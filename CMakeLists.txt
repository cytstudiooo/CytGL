# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
#
# ============================================================================
#  CytGL -- top-level CMake
#
#  Produces:
#     cytgl                 the static library target (libcytgl.a), the core asset
#     cytgl::cytgl          alias, for add_subdirectory / FetchContent use
#
#  Typical use (consuming it as a third-party library):
#
#     add_subdirectory(third_party/CytGL)
#     target_link_libraries(my_app PRIVATE cytgl::cytgl)
#
#  Cross compile (STM32):
#
#     cmake -B build/arm -G Ninja \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#     cmake --build build/arm
#
#  Native (PC) build + run the tests:
#
#     cmake -B build/host -DCYTGL_BUILD_TESTS=ON
#     cmake --build build/host
#     ctest --test-dir build/host --output-on-failure
# ============================================================================

cmake_minimum_required(VERSION 3.16)

project(CytGL
    VERSION 0.1.0
    DESCRIPTION "A tiny, hardware independent, no-heap UI toolkit for MCUs"
    LANGUAGES C
    HOMEPAGE_URL "https://github.com/CytStudio/CytGL")

# ---------------------------------------------------------------------------
# Global language standard
#
# CytGL started out as C99 but uses C11 _Static_assert (see cytgl_config.h),
# so C11 is required. There is no GNU extension dependency: `c_std_11` instead of `gnu_std_11`,
# so compilers such as MSVC / armcc / IAR can be plugged in as well.
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(CYTGL_BUILD_TESTS   "Build the PC (host) regression tests"        OFF)
option(CYTGL_BUILD_EXAMPLES "Build the bundled example applications"     OFF)
option(CYTGL_BUILD_EXT     "Build the optional 2.5D extension module (cgl_phys2d / cgl_iso2d)" OFF)
option(CYTGL_PORT_TEMPLATE "Compile src/port/cgl_port_template.c as a syntax check" ON)

# ---------------------------------------------------------------------------
# Source file list
#
# Split into three logical layers, one to one with the directory structure:
#   core    -- pure logic: types / canvas / font / widget / clock / tween / page
#   render  -- rasterise the widget tree into horizontal bands and push them to the display driver
#   port    -- porting layer glue (contains no board-level code)
# ---------------------------------------------------------------------------
set(CYTGL_SOURCES
    # --- core -----------------------------------------------------------------
    src/core/cgl_canvas.c
    src/core/cgl_math.c
    src/core/cgl_font.c
    src/core/cgl_font_data.c
    src/core/cgl_widget.c
    src/core/cgl_panel.c
    src/core/cgl_label.c
    src/core/cgl_bar.c
    src/core/cgl_arc.c
    src/core/cgl_watch.c
    src/core/cgl_watch_dial.c
    src/core/cgl_time.c
    src/core/cgl_anim.c
    src/core/cgl_page.c
    src/core/cgl_screen.c
    # --- render ---------------------------------------------------------------
    src/render/cgl_display.c
    # --- port (glue only, no hardware) ---------------------------------------
    src/port/cgl_port.c
)

# The template is only syntax checked (it is all empty functions; putting it in the library is pointless and would drag the stub macro along)
if(CYTGL_PORT_TEMPLATE)
    list(APPEND CYTGL_SOURCES src/port/cgl_port_template.c)
    set_source_files_properties(src/port/cgl_port_template.c
        PROPERTIES COMPILE_DEFINITIONS "CGL_PORT_TEMPLATE_STUB=1")
endif()

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(cytgl STATIC ${CYTGL_SOURCES})
add_library(cytgl::cytgl ALIAS cytgl)

target_include_directories(cytgl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# The whole framework only depends on the C standard library -- this PRIVATE is deliberate: HAL headers must not
# leak into consumers.
target_compile_definitions(cytgl PRIVATE CYTGL_INTERNAL=1)

# ---------------------------------------------------------------------------
# Warnings
#
# WARNING: the target firmware is compiled with -fno-inline (nicer call stacks while debugging), and on the pixel-level hot path
#    one pixel has to pass through 5~7 small two-or-three-line functions. The framework uses CGL_AINLINE (always_inline)
#    to guarantee the shape itself, independent of the -O level; see cgl_types.h.
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cytgl PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wcast-align
        -Wstrict-prototypes -Wmissing-prototypes -Wundef
        # The many narrowing conversions in fixed-point maths are deliberate; -Wconversion would drown the useful information
        -Wno-sign-conversion
    )
elseif(MSVC)
    target_compile_options(cytgl PRIVATE /W4)
endif()

# Position independent code: easier to link into all kinds of firmware projects (some toolchains default to PIE)
set_target_properties(cytgl PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Optional extension module: cytgl_ext (cgl_phys2d / cgl_iso2d)
#
# `cgl_phys2d` (fixed point circle physics) and `cgl_iso2d` (isometric camera + depth
# sort) are a self contained 2.5D add-on living in its own pair of trees, `src/ext/`
# for the sources and `include/ext/` for its public API. They are deliberately NOT
# part of the framework: `src/core`, `src/render` and `src/port` never include them,
# `CYTGL_SOURCES` does not list them and `libcytgl.a` does not
# contain them. A consumer that does not want them pays 0 bytes in both flash and
# RAM -- and that is bought by **opt-in linking**, not by a compile time `#ifdef`
# sprinkled through the core:
#
#     add_subdirectory(third_party/CytGL)
#     target_link_libraries(my_app PRIVATE cytgl::ext)   # brings cytgl::cytgl with it
#
# This file keeps only what is explicit about the module -- the option and the
# entry -- and `src/ext/CMakeLists.txt` describes the module itself. A module that a
# third party should be able to lift out of this repository has to be able to
# describe *itself*, and that also makes `cmake -B build src/ext` work: the module
# builds without the framework in the graph at all.
#
# WARNING: moving a module source is not a "just move the file" operation here.
# The demo firmware build collects its sources from *directory lists* by
# non-recursive glob (.rproj SourcePaths, tools/verify_firmware_build.ps1), so a
# file that leaves a listed directory leaves the firmware build with no error at
# all -- the link simply stops seeing it. Every entry point that has to move with a
# source is: the root CMakeLists, `CytGL.rproj` (SourcePaths *and* IncludePaths),
# `tools/verify_firmware_build.ps1`, `tests/CMakeLists.txt` and `tools/mklogo.py`.
# The byte-for-byte `size` comparison is the detector, not "the build succeeded".
#
# The same applies to the headers: `src/ext/*.c` reach their own API through
# `<ext/...>`, which resolves against the `include/` root, so a header that moves
# out of `include/ext/` breaks the IDE project and the firmware script's -I list
# just as quietly.
# ---------------------------------------------------------------------------
if(CYTGL_BUILD_EXT)
    add_subdirectory(src/ext)
endif()

# ---------------------------------------------------------------------------
# Install rules (for use as a CMake package)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS cytgl
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT cytgl
)
install(DIRECTORY include/cytgl
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT cytgl
)

# ---------------------------------------------------------------------------
# Colour / status
# ---------------------------------------------------------------------------
message(STATUS "CytGL ${PROJECT_VERSION}")
message(STATUS "  include dir   : ${CMAKE_CURRENT_SOURCE_DIR}/include")
message(STATUS "  build tests   : ${CYTGL_BUILD_TESTS}")
message(STATUS "  build examples: ${CYTGL_BUILD_EXAMPLES}")
message(STATUS "  build ext     : ${CYTGL_BUILD_EXT}")

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(CYTGL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CYTGL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
