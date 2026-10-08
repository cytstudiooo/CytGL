# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
#
# ============================================================================
#  cytgl_ext -- the optional 2.5D module (cgl_phys2d + cgl_iso2d)
#
#  The module is laid out exactly like the framework it extends -- sources under
#  src/, public headers under include/ -- so a consumer includes it the same way:
#
#      #include <cytgl/cgl_anim.h>          the framework
#      #include <ext/cgl_phys2d.h>          the optional module
#
#  and the two include roots are two -I paths, not five.
#
#  No dependency beyond <stdint.h> / <stdbool.h>, no framework header, no HAL,
#  no heap. It can be built three ways, and all three are exercised by the
#  project's tests:
#
#     1. as part of CytGL          cmake -B build -DCYTGL_BUILD_EXT=ON
#                                  (the root CMakeLists enters this file, and the
#                                   target is a cytgl *extension*)
#     2. on its own                cmake -B build src/ext && cmake --build build
#                                  (no framework in the build graph at all)
#     3. as two plain sources      gcc -I include app.c src/ext/cgl_phys2d.c src/ext/cgl_iso2d.c
#                                  (see the CytGL manual, section 15.10)
#
#  Why the module has its own CMakeLists instead of living in the root one:
#  the root file is the framework's; a module that a third party should be able
#  to lift out of this repository has to be able to describe *itself*. The root
#  keeps exactly what is explicit about it -- the option and the
#  `add_subdirectory(src/ext)` -- so nothing about "is the module built" is implicit.
#
#  The three layers are one-way and the include graph is flat on purpose:
#
#     cgl_iso2d            positions + projection + depth sort; knows NO bodies
#     cgl_iso2d_phys2d.h   the bridge (the only header that includes both layers)
#     cgl_phys2d           bodies, broad phase grid, solver, vertical axis
#     cgl_ext_basis.h      the Q12.4 convention and CGL_EXT_INLINE, shared by both
#
#  A subdirectory per layer was considered and rejected: it would change nothing
#  functionally while adding one *include path* per layer (include/ext/iso2d,
#  include/ext/phys2d, ...) to every entry point that lists a tree by directory --
#  the IDE project file, the firmware build script and the tests. The layering is
#  already stated where it matters: in the headers and in the contract. Flat means
#  `#include <ext/...>` is always one path and never depends on which side of the
#  bridge a header sits on.
# ============================================================================

# Standalone use (way 2): this file is the top level, so it has to be a project.
# When it is entered from the root CMakeLists (way 1) it must NOT be one, or it
# would reset the parent's project() and its CMake version requirements.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(cytgl_ext
        VERSION 0.1.0
        DESCRIPTION "Fixed point 2.5D physics and isometric projection for MCUs"
        LANGUAGES C)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    message(STATUS "cytgl_ext ${PROJECT_VERSION} (standalone: no CytGL framework in this build)")
endif()

# Unconditionally, and before the INSTALL_INTERFACE below needs it: when this file
# is entered from the root, the root's own include(GNUInstallDirs) has not run
# yet, and an undefined ${CMAKE_INSTALL_INCLUDEDIR} would silently install the
# headers into the wrong place (or nowhere) instead of failing. Including it twice
# is a no-op.
include(GNUInstallDirs)

# The include root this module publishes: <repo>/include, the same directory the
# framework publishes. `<ext/...>` and `<cytgl/...>` are therefore siblings under
# one -I, and moving a header between the two trees is a one-line change at the
# include site rather than a build-system change.
get_filename_component(CYTGL_EXT_INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../include" ABSOLUTE)

# Two source groups, and the split is the module's contract rather than a build detail.
#
# The **core** includes nothing but <stdint.h> / <stdbool.h>: it builds, unit-tests and
# ships with no framework in the graph at all (ways 2 and 3 above). Stating that in
# CMake is what keeps it true -- the promise is checked by `cmake -B build src/ext`
# succeeding, not by reading the headers.
set(CYTGL_EXT_SOURCES
    cgl_phys2d.c
    cgl_iso2d.c
    # The particle engine and the shatter scene. Both translation units are inside
    # #if CGL_CFG_PARTICLES / #if CGL_CFG_DEBRIS, so a build that globs this
    # directory can drop them with the macro and pay nothing.
    cgl_particles.c
    cgl_particles_tables.c
    cgl_debris.c
    # The gravity ball: the smallest of the three effects, and the one a caller is
    # most likely to want on its own. Inside #if CGL_CFG_BOUNCE, same trade.
    cgl_bounce.c
)

# The **painters** are the half that shows something, so they include
# <cytgl/cgl_types.h> and <cytgl/cgl_canvas.h> and cannot exist without the framework.
# They are added only when the framework target is in the graph; a standalone build of
# `src/ext/` therefore produces the framework-free core, which is exactly what the module
# promises (see the CytGL manual, section 15.9).
set(CYTGL_EXT_VIEW_SOURCES
    cgl_particles_view.c
    cgl_debris_view.c
    cgl_bounce_view.c
)

if(TARGET cytgl)
    list(APPEND CYTGL_EXT_SOURCES ${CYTGL_EXT_VIEW_SOURCES})
endif()

add_library(cytgl_ext STATIC ${CYTGL_EXT_SOURCES})
add_library(cytgl::ext ALIAS cytgl_ext)

# PUBLIC: the module's public API is every header in include/ext/ -- cgl_phys2d.h,
# cgl_iso2d.h, the two *_config.h, cgl_ext_basis.h, cgl_iso2d_phys2d.h and the
# particle/debris pairs. They are included by their path (`#include <ext/cgl_phys2d.h>`),
# so the one directory published here is the include root, not include/ext: that is what
# makes the module's headers and the framework's live side by side under a single -I.
#
# They deliberately do NOT go into include/cytgl/: that one is the framework's public
# API, and filling it with domain modules is how a library loses its promise.
target_include_directories(cytgl_ext
    PUBLIC
        $<BUILD_INTERFACE:${CYTGL_EXT_INCLUDE_ROOT}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# TARGET cytgl, not a hard dependency: inside this repository the module is an
# *extension* of the framework, so linking it brings the framework with it. On
# its own (way 2) there is no framework target and none is needed -- the module's
# code includes nothing but the C standard library, which is exactly why it can
# be optional in the first place.
if(TARGET cytgl)
    target_link_libraries(cytgl_ext PUBLIC cytgl)
endif()

# Same warning set as the core; the module is held to the same bar.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cytgl_ext PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wcast-align
        -Wstrict-prototypes -Wmissing-prototypes -Wundef
        # The many narrowing conversions in fixed-point maths are deliberate; -Wconversion would drown the useful information
        -Wno-sign-conversion
    )
elseif(MSVC)
    target_compile_options(cytgl_ext PRIVATE /W4)
endif()

# Position independent code: easier to link into all kinds of firmware projects (some toolchains default to PIE)
set_target_properties(cytgl_ext PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Install rules
#
# Artifacts, not a CMake package: the core does not ship a `find_package` config
# either (no cytglConfig.cmake is generated), so this stays at the same level --
# lib + headers in the right places, and the target usable through
# add_subdirectory() / FetchContent() as cytgl::ext.
#
# The headers land in <prefix>/include/ext/, a sibling of <prefix>/include/cytgl/,
# which is exactly the layout the source tree has. A consumer therefore points at
# the one include root it already points at for the framework:
#
#     gcc -I <prefix>/include app.c -lcytgl_ext
# ---------------------------------------------------------------------------
install(TARGETS cytgl_ext
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT cytgl-ext
)
install(DIRECTORY ${CYTGL_EXT_INCLUDE_ROOT}/ext
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT cytgl-ext
    FILES_MATCHING PATTERN "*.h"
)
