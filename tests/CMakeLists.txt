# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
#
# ============================================================================
#  CytGL host (PC) test suite
#
#  These tests compile the REAL framework sources (nothing is stubbed) and run
#  them against mock display drivers that write into memory arrays. That means
#  every assertion below exercises shipping code paths, pixel for pixel.
#
#  Build & run:
#      cmake -B build/host -DCYTGL_BUILD_TESTS=ON
#      cmake --build build/host
#      ctest --test-dir build/host --output-on-failure
#
#  NOTE: all relative paths in this file resolve against THIS directory, i.e.
#  <repo>/tests. Source files therefore appear as `alpha_host_test.c`, not
#  `tests/alpha_host_test.c`.
# ============================================================================

# ---------------------------------------------------------------------------
# Where the demo application lives. Several tests deliberately drive the real
# demo pages to prove the framework survives a realistic workload.
#
# WARNING: this pointed at `examples/STM32G431CBU6_CytStudio_Demo` until the two
# STM32 demos were de-duplicated and their shared `App/`, `BSP/` and `Devices/`
# trees moved to `examples/common/`. Nothing failed loudly when that happened --
# the *test* build broke with "ninja: error: ...app_page_smash.c, missing and no
# known rule to make it", which names a file rather than the directory that moved.
# The shared tree is what the tests want: they drive the pages, and the pages are
# shared, while everything under the board directory is board-specific (a linker
# script, CubeMX's HAL, the .ioc) and none of it is compiled here.
# ---------------------------------------------------------------------------
set(CYTGL_DEMO "${CMAKE_CURRENT_SOURCE_DIR}/../examples/common")

# The optional module's two trees: `src/ext/` holds its sources and `include/ext/`
# its public API, which is reached as `<ext/...>` through the `include/` root --
# the same way `src/core/cgl_anim.c` reaches `<cytgl/cgl_anim.h>`. The tests that
# drive the demo pages compile its sources next to the widgets, exactly like the
# firmware does (see the NOTE on DEMO_SRC below); the tests that only exercise the
# module link the `cytgl_ext` target instead.
set(CYTGL_EXT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src/ext")
set(CYTGL_EXT_INC "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# Framework sources come from the real library target, so the tests can never
# drift from what ships.
set(CGL_TEST_LIBS cytgl)

# Demo application sources used by the integration tests.
# NOTE: app_ui.c / app.c are deliberately NOT here: they sit on the board port
# (cgl_port.c), which pulls in board_lcd.h and st7789.h, i.e. real hardware. The
# tests assemble their own app harness -- their own cgl_display_t / cgl_screen_t --
# instead.
#
# NOTE: the module's cgl_phys2d.c / cgl_iso2d.c are listed here (rather than being taken
# from the optional `cytgl_ext` target) on purpose: these tests compile the module
# as loose sources next to the demo widgets, which is exactly how the firmware
# builds it. `ext_smoke_test` and `ext_example` below are the tests that go through
# the target.
set(DEMO_SRC
    ${CYTGL_EXT_SRC}/cgl_phys2d.c
    ${CYTGL_EXT_SRC}/cgl_iso2d.c
    ${CYTGL_EXT_SRC}/cgl_debris.c
    ${CYTGL_EXT_SRC}/cgl_debris_view.c
    ${CYTGL_EXT_SRC}/cgl_bounce.c
    ${CYTGL_EXT_SRC}/cgl_bounce_view.c
    ${CYTGL_DEMO}/App/app_page_smash.c
    ${CYTGL_EXT_SRC}/cgl_particles.c
    ${CYTGL_EXT_SRC}/cgl_particles_view.c
    ${CYTGL_EXT_SRC}/cgl_particles_tables.c
    ${CYTGL_DEMO}/App/app_page_bullet.c
    ${CYTGL_DEMO}/App/app_page_menu.c
    ${CYTGL_DEMO}/App/app_page_info.c
    ${CYTGL_DEMO}/App/app_page_clock.c
)

set(DEMO_INC
    ${CYTGL_DEMO}/App
    ${CYTGL_EXT_INC}           # the module's public API: <ext/cgl_phys2d.h> (moved out of App/)
    ${CYTGL_DEMO}/BSP          # app_page_menu.c / app_page_smash.c include board_key.h
)

# The demo's own effect switches (App/app_fx_config.h) are **application**
# configuration, not test configuration. The test suite names those pages
# explicitly in DEMO_SRC, so it forces the switches **on**: an application that
# turns an effect off (which frees the smash page's 12.7 KB world) must not be
# able to empty a translation unit out from under a test. Without this, a local
# `APP_FX_SMASH=0` turns into "render_test, bullet_test, bounce_test and
# bullet_frames fail to link", which tells a reader nothing about what broke.
set(DEMO_DEFINES APP_FX_SMASH=1 APP_FX_PARTICLES=1)

# Applied to every test target in this directory (the pure-framework tests do not
# include app_fx_config.h, so it is a no-op for them). One place, because the
# alternative -- the same two defines repeated on six `cgl_add_host_test` calls --
# is exactly how one of them ends up missing.
add_compile_definitions(${DEMO_DEFINES})

# The snapshot tests write PPM/BMP frames to a *relative* `build/` path. Create
# that directory up front so the very first run does not silently skip every
# image (the writers report a failure and the assertions that read the images
# back then fail for an unrelated reason).
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/build)

# ---------------------------------------------------------------------------
# Helper: declare one host test executable.
#
#   cgl_add_host_test(<name> SOURCES ... [INCLUDES ...] [LIBS ...] [DEFINES ...] [ARGS ...])
#
# Every test is registered with CTest and runs with the build directory as its
# working directory, so files it writes (PPM/BMP snapshots) land in a
# predictable place next to the executable.
#
# LIBS is for targets a single test needs on top of the framework (currently only
# the optional `cytgl_ext`); the common case stays "link cytgl and nothing else".
# DEFINES is for a test that deliberately builds a source in a different
# *configuration* than the library does (see `user_storage_test` below).
# ---------------------------------------------------------------------------
function(cgl_add_host_test name)
    cmake_parse_arguments(T "" "" "SOURCES;INCLUDES;ARGS;LIBS;DEFINES" ${ARGN})

    if(NOT T_SOURCES)
        message(FATAL_ERROR "cgl_add_host_test(${name}): no SOURCES given")
    endif()

    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE ${CGL_TEST_LIBS} ${T_LIBS})
    target_include_directories(${name} PRIVATE ${T_INCLUDES})
    target_compile_definitions(${name} PRIVATE ${T_DEFINES})

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        FOLDER "tests")

    add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()

# ---------------------------------------------------------------------------
# 1. Pure framework tests (no application code at all)
# ---------------------------------------------------------------------------

# Group opacity: overlap must not double-blend, nesting, across bands,
# warp path, out of alpha levels, scratch must not leak.
cgl_add_host_test(alpha_test
    SOURCES alpha_host_test.c)

# Pages + clock + tween + subtree translation:
# fixed stepping, stall does not fast-forward, easing endpoints, page stack
# semantics, lifecycle ordering, frame-boundary commit, input lock, and the
# Genie warp geometry (p=0 identity, per-row step <= 1, conservative bounds,
# funnel is gap-free row by row with zero pixels outside the silhouette).
cgl_add_host_test(page_test
    SOURCES page_host_test.c)

# Widget tree rules that a screenshot cannot settle: the enforced child limit
# (CGL_CFG_WIDGET_MAX_CHILDREN used to be defined and read by nothing) and what hit
# testing does about CGL_WF_CLIP and about a deformed (warped) subtree.
cgl_add_host_test(widget_test
    SOURCES widget_host_test.c)

# The built-in bar widget, pixel by pixel: the value -> fill-length mapping
# (including the full int32 range, where an `int64`-free division is not optional),
# clamping, both orientations and both directions, the border ring and the
# segment separators, "it clears its own background", and "a setter damages only
# when the drawn pixels can really change". Writes build/bar_test.ppm/.bmp as a
# gallery of what it checked.
cgl_add_host_test(bar_test
    SOURCES bar_host_test.c
            test_image_out.c
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# The radial primitives the framework never had (cgl_math + cgl_canvas_circle /
# _line_thick / _arc): the trig is compared against libm at all 65536 angles, a
# circle must be a circle (not a filled disc, not a spiral), a thick line must be
# the capsule it claims to be (round caps, no gaps on a steep slope), and an arc
# must stay on its annulus and honour its sweep. Writes build/arc_test.ppm/.bmp: a
# miniature dial drawn out of nothing but these primitives.
cgl_add_host_test(arc_test
    SOURCES arc_host_test.c
            test_image_out.c
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

# The radial toolkit (cgl_arc) and the watch face (cgl_watch) as **modules**, with no
# application page in the way -- `clock_test` below is the page's own test and asks
# "does the face look like a watch", which is a different question. This one asks the
# module's questions: is the default configuration a working watch, does the calendar
# roll over (leap rules included), is the angle convention the one the clock reads,
# do the two second-hand motions agree at every mark, are the two surfaces *lit* the
# way their design says (brighter in the middle, highlight to the upper left, the
# well dark at the top and bright at the bottom), do the baked default tables agree
# with the shipping baker entry for entry, does the ring gauge map a value to the
# right sweep and lead point -- and the property no screenshot can settle: an
# incrementally damaged frame must be **identical** to a full redraw.
cgl_add_host_test(watch_test
    SOURCES watch_host_test.c)

# ---------------------------------------------------------------------------
# 2. Application integration tests
# ---------------------------------------------------------------------------

# Physics: 20 s of simulation, checks contact cycle and fixed-point overflow.
cgl_add_host_test(logo_test
    SOURCES cgl_phys2d_host_test.c ${DEMO_SRC}
    INCLUDES ${DEMO_INC})

# Bouncing ball + per-page fixed stepping: rate, freeze, bounds, restitution,
# and survival across a logo rebuild.
cgl_add_host_test(bounce_test
    SOURCES bounce_host_test.c ${DEMO_SRC}
    INCLUDES ${DEMO_INC})

# Bullet time: the particle engine and the page that drives it.
#
# Four claims that no screenshot can settle, so they are measured here:
#   1. the engine's storage and the smash page's world are the **same bytes**
#      (the overlay that makes the page cost no extra RAM at all);
#   2. the sine fold, the reciprocal table and CGL_PARTICLES_MUL's truncation;
#   3. the draw order follows the **camera**, so a 90 deg turn reorders a cloud in
#      which not one particle moved -- the case cgl_iso2d's per-step stamp cannot do;
#   4. time really stops: real frames are driven through the whole state machine
#      and the particles are compared byte for byte while the scale is 0, while the
#      camera is required to keep turning.
cgl_add_host_test(bullet_test
    SOURCES bullet_host_test.c ${DEMO_SRC}
    INCLUDES ${DEMO_INC})

# Bullet time snapshots: drives the real page through a whole round and writes PPM/BMP
# frames at the moments that matter (intact Logo, inbound ball, impact, flying debris,
# the freeze, the orbit at each quarter turn, the release). Same idea as render_test,
# for the second animation page; a picture is still the fastest way to see "is the
# Logo on screen, and is the ball actually hitting it".
cgl_add_host_test(bullet_frames
    SOURCES bullet_frames.c
            test_image_out.c
            ${DEMO_SRC}
    INCLUDES ${DEMO_INC}
             ${CMAKE_CURRENT_SOURCE_DIR})

# Watch face snapshots: drives the real clock page through its entrance reveal and
# then through several times of day, a minute rollover and the second hand's comet
# trail, and writes one PPM/BMP per moment. The dial is the first radial drawing in
# the project, so "is it on screen, and does it look like a watch" is a question a
# picture answers faster than any assertion.
cgl_add_host_test(clock_frames
    SOURCES clock_frames.c
            test_image_out.c
            ${DEMO_SRC}
    INCLUDES ${DEMO_INC}
             ${CMAKE_CURRENT_SOURCE_DIR})

# The watch face page, measured rather than photographed: the time base (including
# the calendar's month / leap / year rollovers), the dial's angle convention
# (0 = 3 o'clock, clockwise), that the seconds ring is a stroke that starts at 12,
# that the entrance really assembles the chapter ring, the comet trail, the two
# complications, and the property this page exists to demonstrate -- a covered page
# stops receiving frames but does **not** stop keeping time.
cgl_add_host_test(clock_test
    SOURCES clock_host_test.c
            ${DEMO_SRC}
    INCLUDES ${DEMO_INC})

# The same watch face page with the **other second-hand motion** compiled in: one step
# per second with a whip and an overshoot, instead of the continuous glide
# (`CLOCK_SECOND_MODE = CLOCK_SECOND_TICK`, see App/app_page_clock.h). It is the same
# page and the same test file -- the mode is one switch over one function, and the test
# asserts the shape the switch is supposed to produce (hold, snap, exact marks) while
# every other assertion in the file has to keep passing unchanged.
#
# NOTE: the define is `-DCLOCK_SECOND_MODE=CLOCK_SECOND_TICK` and must stay a single
# token: the app header's `#ifndef` treats "the mode is already chosen from the build"
# as the normal case, so a configure line like
# `cmake -B build/tick -DCLOCK_SECOND_MODE=...` still works, and both builds coexist.
cgl_add_host_test(clock_tick_test
    SOURCES clock_host_test.c
            ${DEMO_SRC}
    INCLUDES ${DEMO_INC}
    DEFINES CLOCK_SECOND_MODE=CLOCK_SECOND_TICK)

# The same page photographed with the tick compiled in: `clock_frames` grows four extra
# snapshots across one snap when the mode is TICK (`#if` in tests/clock_frames.c), which
# is the only way to *look* at a motion whose whole character is the shape of a 100 ms
# curve. The default build of `clock_frames` above stays exactly as it was.
cgl_add_host_test(clock_tick_frames
    SOURCES clock_frames.c
            test_image_out.c
            ${DEMO_SRC}
    INCLUDES ${DEMO_INC}
             ${CMAKE_CURRENT_SOURCE_DIR}
    DEFINES CLOCK_SECOND_MODE=CLOCK_SECOND_TICK)

# The optional module's four tests.
#
# Three of them (cgl_iso2d_test, ext_smoke_test, ext_example) go through the OPTIONAL
# MODULE TARGET instead of listing cgl_phys2d.c / cgl_iso2d.c, and need no demo include
# path at all, because `cytgl_ext` publishes its own headers: that is the
# difference between "the module's code works" and "the module is usable as a
# module". The fourth (user_storage_test) deliberately compiles its own copy of
# the source, for the reason written next to it below.
#
# They only exist when the module is enabled (`-DCYTGL_BUILD_EXT=ON`). The suite
# deliberately does NOT force the option on by itself (that would make an
# explicit -DCYTGL_BUILD_EXT=OFF a lie), so the skip is reported loudly rather
# than silently shrinking the suite.
if(TARGET cytgl_ext)
    # Isometric projection + depth sorting: formula, occlusion order, 2.5D height.
    cgl_add_host_test(cgl_iso2d_test
        SOURCES cgl_iso2d_host_test.c
        LIBS cytgl_ext)

    # The module linked as a target, with no App/ widget and no demo include path.
    cgl_add_host_test(ext_smoke_test
        SOURCES ext_smoke_test.c
        LIBS cytgl_ext)

    # The gravity ball engine, also through the target and with no framework header
    # in its include graph. `bounce_test` above is the end-to-end one (the page, the
    # widget tree, the unified clock, 482 bodies); this one is the module's own
    # contract -- semi-implicit gravity, decaying energy, self-healing by identity,
    # rejected playgrounds, determinism -- none of which the page test can isolate.
    cgl_add_host_test(cgl_bounce_test
        SOURCES cgl_bounce_host_test.c
        LIBS cytgl_ext)

    # The caller-storage variant of the physics world. This one CANNOT link
    # `cytgl_ext`: `CGL_PHYS2D_USER_STORAGE=1` changes the layout of `cgl_phys2d_t`
    # (pointers instead of the embedded arrays), so the test has to own a copy of
    # the source compiled in that configuration -- mixing the two layouts in one
    # binary would be undefined behaviour. It is therefore built from the loose
    # source, exactly like the firmware builds the module.
    cgl_add_host_test(user_storage_test
        SOURCES cgl_phys2d_storage_host_test.c
                ${CYTGL_EXT_SRC}/cgl_phys2d.c
        INCLUDES ${CYTGL_EXT_INC}
        DEFINES CGL_PHYS2D_USER_STORAGE=1)

    # The module's "hello world" (the CytGL manual, section 15.10): a tiny world, projected and
    # drawn onto a 96x48 character screen in memory, with no framework at all. It
    # links the target here, while the docs give the two-source command line, so
    # the example is proven to work both ways. It is a registered test on purpose:
    # an example that nobody compiles is an example that rots.
    cgl_add_host_test(ext_example
        SOURCES ext_example.c
        LIBS cytgl_ext)
else()
    message(STATUS "CytGL tests: cgl_iso2d_test + ext_smoke_test + cgl_bounce_test + user_storage_test + ext_example SKIPPED "
                   "-- pass -DCYTGL_BUILD_EXT=ON to cover the optional cgl_phys2d/cgl_iso2d/cgl_bounce module")
endif()

# ---------------------------------------------------------------------------
# 3. Rendering snapshot tests (write PPM/BMP frames you can just open)
# ---------------------------------------------------------------------------

cgl_add_host_test(render_test
    SOURCES render_host_test.c
            test_image_out.c
            ${DEMO_SRC}
    INCLUDES ${DEMO_INC}
             ${CYTGL_DEMO}/Devices
             ${CMAKE_CURRENT_SOURCE_DIR})

message(STATUS "CytGL tests: alpha_test page_test widget_test logo_test bounce_test bullet_test cgl_iso2d_test render_test bar_test"
               " arc_test watch_test clock_test clock_tick_test clock_frames clock_tick_frames"
               " (+ ext_smoke_test, cgl_bounce_test, user_storage_test, ext_example when CYTGL_BUILD_EXT=ON)")
