<div align="center">

**English** | [简体中文](README.zh-CN.md)

# CytGL

**A hardware-independent, no-heap retained-mode UI framework for microcontrollers.**

CytGL renders a screen in horizontal bands and streams them to the panel, so it needs
no framebuffer. It provides a widget tree, a page and navigation model with animated
transitions, group opacity, a single injected clock with fixed-step scheduling, dot-matrix
fonts, and a software rasteriser. It contains no `malloc`, no libm call, no HAL header and
no hard-coded panel geometry.

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/language-C11-informational.svg)](#)

<div align="left">

- Language: C11, no compiler extensions, no generated files, no third-party dependencies.
- Dependencies: `<stdint.h>`, `<stdbool.h>`, `<stddef.h>` only.
- License: Apache License 2.0.
- Version: 0.1.0 (`CGL_VERSION_STRING`).
- Reference platform: STM32G431CB (Cortex-M4, 128 KB flash / 32 KB RAM) driving a
  320x172 ST7789 panel over SPI with DMA.

---

## 1. Why band rendering

A 320x172 RGB565 framebuffer occupies 110 KB. The reference part has 32 KB of RAM, so the
conventional approach — compose the frame in memory, then transfer it — cannot be used.
CytGL instead cuts the screen into horizontal strips of `CGL_CFG_BAND_H` rows (4 by
default) and rasterises one strip while the bus shifts out the previous one. Two
alternating strip buffers of `CGL_CFG_SCREEN_W * CGL_CFG_BAND_H` pixels each give the
whole pipeline 5 KB instead of 110 KB.

Everything else follows from that decision:

- a band handed to the panel must be complete, so a band is always painted from the widget
  tree root; there is no partial-band redraw and no stored previous frame;
- a widget must paint its entire frame itself, because nothing restores the background;
- a group-opacity compositor saves the region it covers per band and blends it back, which
  works precisely because a band is painted in full.

---

## 2. What it provides

| Area | Content |
|---|---|
| Types | RGB565 colours, points, rects (inclusive on both edges), input events, one blend formula |
| Canvas | Band canvas with clip, pan and non-linear warp; pixel, line, rect, disc, dot, blit; antialiased circle, thick line and arc; per-primitive ink alpha; group-opacity scratch |
| Integer maths | 16-bit angles, Q8 sine/cosine with linear interpolation, polar mapping, range fraction, integer square root |
| Fonts | 8x16 and 12x24 ASCII dot-matrix fonts, row-major with bit 0 leftmost |
| Widget tree | Parent/child tree in absolute coordinates, clipping, culling, subtree translation, subtree warp, hit testing, event dispatch, a single global damage sink |
| Built-in widgets | `cgl_panel` (background/border), `cgl_label` (fixed-length text, hand-written integer formatting), `cgl_bar` (arbitrary signed range, both orientations, border, segments, tweened value) |
| Radial toolkit | `cgl_arc`: coverage ramp, tapered antialiased needle rasteriser, shaded radial surfaces with optional baked tables, ring-gauge widget, five alpha wrappers over the canvas primitives |
| Watch face | `cgl_watch`: shaded dial, recessed ring, chapter ring, three tapered hands, hub, optional comet trail and crystal cover; time base plus a counted calendar, no RTC required |
| Display | Driver vtable, double-buffered band presentation with asynchronous pixel push, damage accumulation |
| Clock | One clock for the whole project: frame time, fixed-step logic scheduling, frame-rate statistics |
| Tweens | Integer Q8 tweens and easing curves |
| Pages | Page objects with a lifecycle vtable, a fixed-length navigation stack, transition preferences per page, eleven transition behaviours |
| Scene | Display + clock + navigator + input queue, exposed as one frame call |

The framework owns no hardware knowledge. Time enters through the port's `tick_ms`, input
through its `poll_input`, and pixels leave through a vtable.

### Core properties

- **No heap.** Every page, widget, world and buffer is a static or caller-declared object;
  sizes are compile-time budgets.
- **No floating point, no libm.** All drawing, timing and easing are integer or fixed-point.
- **No 64-bit division.** Where a value-to-pixel mapping exceeds `int32_t` in intermediate
  range it uses shift/subtract long division, avoiding the libgcc helpers.
- **A single source of time.** The port reports the hardware tick; the framework accumulates
  it and schedules everything else. There is no requirement to call A before B.
- **Structural freezing.** Only the top page of the navigation stack receives logic steps, so
  a covered page's simulation stops without accumulating debt.
- **Observable failure counters.** Alpha that could not be applied, widget children refused
  past the limit, rejected ranges, dropped events, rejected navigation requests and lost
  navigation requests are all readable numbers rather than visual guesses.

### Optional extension module

A separate add-on lives in its own two trees (`include/ext/` for its API, `src/ext/` for its
sources) and is never part of `libcytgl.a`. A build that does not link it pays zero bytes in
flash and RAM.

| Effect | Engines | Painter |
|---|---|---|
| Fixed-point 2.5D physics and projection | `cgl_phys2d` (Q12.4 circle world, uniform-grid broad phase, inelastic solver, presentation-only vertical axis), `cgl_iso2d` (orthographic camera fit, counting-sort depth order), `cgl_iso2d_phys2d` (the one bridge) | application's own, or the module's shatter painter |
| Particle cloud | `cgl_particles` (Q8.8, 12 bytes per particle, orbiting camera, reciprocal-table perspective divide, generated sine table) | `cgl_particles_view` |
| Shatter scene | `cgl_debris` (bitmap to rigid-body cloud, camera fit, SMASH/SETTLE cycle) | `cgl_debris_view` |
| Gravity ball | `cgl_bounce` (semi-implicit gravity, four walls, restitution, self-healing by identity) | `cgl_bounce_view` plus a standalone ball widget |

The engine translation units include only the freestanding C headers. The painter files are
the only part that includes `cgl_canvas.h`, and they are added to the module target only when
the framework is present in the build graph.

---

## 3. Building

```bash
cmake -B build -DCYTGL_PORT_TEMPLATE=OFF
cmake --build build
# -> build/libcytgl.a
```

CMake options:

| Option | Default | Effect |
|---|---|---|
| `CYTGL_BUILD_TESTS` | `OFF` | Builds the host (PC) regression suite and registers it with CTest |
| `CYTGL_BUILD_EXAMPLES` | `OFF` | Adds the example directory; its firmware target is informational unless the ARM toolchain and the checked-in project settings are present |
| `CYTGL_BUILD_EXT` | `OFF` | Builds the optional extension module (`cytgl_ext`, alias `cytgl::ext`, `libcytgl_ext.a`) |
| `CYTGL_PORT_TEMPLATE` | `ON` | Compiles `src/port/cgl_port_template.c` as a syntax check |

Installing places `libcytgl.a` in `lib/` and the headers in `include/cytgl/`. No
`find_package` configuration is generated; consume the target through `add_subdirectory()` or
`FetchContent()`.

### As a subproject

```cmake
add_subdirectory(third_party/CytGL)
target_link_libraries(my_app PRIVATE cytgl::cytgl)
```

### Without CMake

Add every `.c` under `src/core`, `src/render` and `src/port` (except
`src/port/cgl_port_template.c`) to the project and put `include/` on the include path.

Do **not** add `src/core` to the global include path together with `include/`. Framework
sources include their own headers by plain filename, which resolves within the source file's
own directory; a global path entry would make the private `cgl_screen.h` ambiguous with the
public one. `include/` alone serves both `<cytgl/...>` and `<ext/...>`.

### Host regression suite

```bash
cmake -B build/host -DCYTGL_BUILD_TESTS=ON -DCYTGL_BUILD_EXT=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure

ctest --test-dir build/host -R alpha_test --output-on-failure   # one test
```

The tests compile and run the real framework sources against mock display drivers that write
into memory arrays, so the assertions check shipping code paths pixel by pixel. Rendering
tests also write viewable `frameNN.ppm` and `.bmp` snapshots. The only seam between the panel
driver and the band presenter is the `cgl_display_drv_t` vtable, which is what makes this
possible.

---

## 4. Porting in one page

Three things are required, handed to the framework once through a single descriptor.

### 4.1 Display driver

```c
typedef struct cgl_display_drv
{
    const char *name;

    int  (*init)(void *ctx);                                       /* power up       */
    void (*set_window)(void *ctx, uint16_t x0, uint16_t y0,
                       uint16_t x1, uint16_t y1);                  /* window, inclusive */
    void (*write_pixels)(void *ctx, const cgl_color_t *px,
                         uint32_t count);                          /* ASYNC push      */
    void (*wait_idle)(void *ctx);                                  /* drain the bus   */
    void (*set_backlight)(void *ctx, uint8_t percent);             /* may be NULL     */

    int16_t width;   /* must equal CGL_CFG_SCREEN_W */
    int16_t height;  /* must equal CGL_CFG_SCREEN_H */
} cgl_display_drv_t;
```

- `init` returns 0 on success; a non-zero result aborts the boot.
- `set_window` takes inclusive coordinates in panel space. Apply any offset required by a
  panel whose visible area sits inside a larger GRAM.
- `write_pixels` must start an asynchronous transfer and return. The pixel rate depends on it:
  the framework overlaps rasterisation of the next band with the transfer of this one. The
  buffer is valid until `wait_idle()` returns. `count` is a pixel count, not a byte count.
- `wait_idle` blocks until that transfer completes and is where deferred bus cleanup belongs
  (releasing chip select, for example).
- `width` and `height` must be the panel geometry in landscape orientation; the framework
  refuses to start on a mismatch.

### 4.2 Port descriptor and frame entry point

```c
#include <cytgl/cytgl.h>

static const cgl_display_drv_t my_display_drv = { /* as above */ };
static uint32_t my_tick_ms(void)     { return (uint32_t)HAL_GetTick(); }
static void     my_poll_input(void *user, uint32_t dt_ms);

static const cgl_port_t s_port = {
    .name        = "my-board",
    .display     = &my_display_drv,
    .display_ctx = &my_panel,
    .tick_ms     = my_tick_ms,
    .poll_input  = my_poll_input,     /* optional; NULL = no input source */
    .input_ctx   = NULL,
    .board_init  = my_board_init,     /* optional: pins, bus clocks, backlight */
    .board_ctx   = NULL,
};

static cgl_port_ctx_t g_port;

int main(void)
{
    board_init();                                  /* clocks, pins, generated inits */

    if (cgl_port_bind(&s_port, &g_port, NULL) != 0)
    {
        Error_Handler();                           /* board, driver or geometry failure */
    }

    for (;;)
    {
        (void)cgl_port_frame(&g_port);             /* dt + input + one frame */
    }
}
```

Passing `NULL` as the third argument makes the framework hold its own static navigator,
initialised to the full screen rectangle. An application that registers pages against a
navigator it can reach passes its own instance instead.

`cgl_port_frame()` reads `tick_ms()`, computes the elapsed time, calls `poll_input(dt)`, and
then advances the frame. Skipping `cgl_port_frame()` and calling `cgl_screen_frame()` directly
is supported when the application owns the pacing; the input scan is then the caller's to
pump with the same `dt`.

### 4.3 Input

The framework never polls anything itself; it calls the port's poller once per frame with the
frame's `dt`, which is the only place a debounce can be advanced against real elapsed time.
Translate what you find into a `cgl_event_t` and post it:

```c
cgl_event_t ev = { .kind = CGL_EV_KEY_DOWN, .key = 0, .hold_ms = key_hold_ms() };
(void)cgl_port_post_event(&g_port, &ev);
```

Two rules: **post, do not dispatch** (the screen delivers events at the frame boundary, so no
page is entered or left mid-frame), and keep a **single producer** so the scan and the debounce
live in one place. Event kinds are `CGL_EV_KEY_DOWN / REPEAT / LONG / UP`,
`CGL_EV_TOUCH_DOWN / MOVE / UP` and `CGL_EV_BACK`. Touch needs nothing beyond posting
`CGL_EV_TOUCH_*`; hit testing already exists.

A single-key board commonly translates one long press into `CGL_EV_KEY_LONG` followed by
`CGL_EV_BACK`. A page that consumes the long press reads it as "confirm"; a page that does not
lets the following `CGL_EV_BACK` reach the navigator and pop the stack.

### 4.4 Checklist

- [ ] Display driver implemented; `init` returns non-zero on failure.
- [ ] `set_window` inclusive, in panel coordinates.
- [ ] `write_pixels` returns after starting an asynchronous transfer.
- [ ] `wait_idle` blocks until the transfer is done, then releases chip select.
- [ ] `width`/`height` equal `CGL_CFG_SCREEN_W`/`CGL_CFG_SCREEN_H` and are landscape.
- [ ] `tick_ms` is monotonic.
- [ ] `poll_input`, if used, posts events and never dispatches them.
- [ ] `cgl_port_bind()` returns 0, and the first frame paints.

---

## 5. One frame

```
cgl_port_frame(&port)                dt = tick_ms() - last_tick (wrap-safe)
  poll_input(dt)                     the port's key or touch scan; posts events
cgl_screen_frame(&screen, dt)
  1. cgl_clock_begin(dt)             the only place time is accumulated
  2. drain input at the frame boundary
  3. cgl_page_nav_frame()            commit navigation, advance transitions
  4. while (cgl_clock_next_step())   fixed-step logic: page->on_step(step_ms)
  5. page->on_frame(&clock)          once per frame: tweens, state, text refresh
  6. cgl_widget_update_tree()        purely visual animation
  7. cgl_display_present()           rasterise and push one band at a time
```

Three rules hold the design together.

1. **One source of time.** The clock is the only thing that accumulates it; the port only
   reports the hardware tick, and the framework orders the remaining six steps.
2. **The widget tree changes only at the top of a frame**, inside `cgl_page_nav_frame()`, and
   always before rendering. Navigation is requested, never applied immediately.
3. **Logic and visuals are separate domains.** Physics runs in `on_step` with a fixed step and
   is replayable; tweens and text refresh run in `on_frame` and follow the frame rate.

### Band presentation

```
buffer[cur]  rasterise -> set_window(band) -> write_pixels (DMA, async)  [CPU continues]
buffer[!cur] rasterise  while the DMA is still shifting out the previous band
                                          -> wait_idle() -> next window ...
```

A damage rectangle causes every complete band it intersects to be repainted. The opacity
scratch buffer is written by the CPU only and never enters DMA, because the other band buffer
is being read by the DMA at that moment.

---

## 6. Architecture

Four layers with strictly one-directional dependencies:

```
        App/            application: pages, scene assembly
         |  depends down only
      cgl_port.c/.h     the board port: the one file that binds hardware to the framework
         |              (display vtable + clock + input source)
        CytGL           the framework: types / canvas / font / widgets / display /
         |              clock / tween / pages / screen
         |              <- no HAL header, no malloc, no libm
      Devices/          panel driver (for example ST7789)   <- knows only a bus vtable
         |
        BSP/            board: pins, SPI, DMA, buttons      <- knows only CubeMX handles
         |
       Core/ + Drivers/ generated code

  include/ext + src/ext  sit beside those four: an optional module (fixed-point physics and
         isometric projection, a particle engine, a shatter scene, a gravity ball), each a
         framework-free engine plus a thin painter. Its sources and its public API use the
         framework's own two trees, so it is included the framework's own way:
         <ext/cgl_phys2d.h>.
```

The portability test: copy the library out of this repository into any other project and it
still works, because it does not know what a screen is and does not know where time comes
from.

### Three decoupling seams

| Seam | Header | What it isolates |
|---|---|---|
| `cgl_display_drv_t` | `cgl_display.h` | How pixels reach a panel. Substituting a memory-array mock makes the real framework PC-testable pixel by pixel |
| Bus vtable | example `Devices/lcd_bus.h` | How the electrical signal leaves. Hardware SPI with DMA, bit-banged GPIO, a parallel bus and a simulator are interchangeable |
| `cgl_widget_vt_t` | `cgl_widget.h` | What a widget looks like. A new widget is a struct whose first member is `cgl_widget_t` plus a vtable |

The damage sink is the one seam that is not abstracted: it is a global singleton, so only one
screen can be active at a time.

### Pages and transitions

A page is an object carrying a vtable (`on_create`, `on_destroy`, `on_enter`, `on_exit`,
`on_pause`, `on_resume`, `on_step`, `on_frame`, `on_event`), and the navigator is a
fixed-length stack of `CGL_CFG_PAGE_STACK_MAX` entries. `cgl_trans_t` holds 13 values: eleven
transition behaviours, plus `CGL_TRANS_NONE` (instant cut) and `CGL_TRANS_AUTO` (choose by
navigation action).

Transitions are implemented by setting an offset on the page root widget, which is why no
child widget has to change. Genie ("suck in") is the non-linear version of the same seam: a
warp descriptor is installed on the page root and every primitive recomputes its landing row,
producing a geometric deformation rather than a pixel resample. The `S_OUT` and `S_IN` types
reuse the same funnel with the suction point at the bottom-right corner and a horizontal
S-shaped trajectory.

A page may declare its own transition preference:

```c
cgl_page_set_trans_enter(page, CGL_TRANS_COVER_UP, CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
```

The resolution order is page declaration, then the caller's request, then the per-action
default. The one exception is an explicit `CGL_TRANS_NONE`, which always wins so that "cut
instantly this time" remains expressible.

### Group opacity

```c
cgl_widget_set_alpha(&panel.base, 160u);   /* 0..255, 255 = opaque (default) */
```

The whole widget and its subtree are rendered in isolation per band and then composited onto
the background as one unit, so overlapping children are not blended twice. The isolation
region is the widget's frame when `CGL_WF_CLIP` is set, which is the default. With
`CGL_CFG_ALPHA` set to 0 the feature is compiled out, no scratch is allocated, and the setter
degrades to a counted no-op.

---

## 7. Repository layout

```
include/cytgl/           public API of the framework
  cytgl.h                single entry point; includes everything
  cytgl_config.h         all compile-time knobs
  cgl_types.h            RGB565 colours, points, rects, events, blend formula
  cgl_math.h             integer trigonometry, polar maps, integer square root
  cgl_canvas.h           band canvas, software rasteriser, warp, ink alpha
  cgl_font.h             dot-matrix fonts
  cgl_widget.h           retained-mode widget tree
  cgl_panel.h            built-in widget: background and border container
  cgl_label.h            built-in widget: text label
  cgl_bar.h              built-in widget: progress and level bar
  cgl_arc.h              radial toolkit: shaded surfaces, tapered needles, ring gauge
  cgl_watch.h            built-in widget: analog watch face (built on cgl_arc)
  cgl_display.h          display vtable and band presenter
  cgl_time.h             the single frame clock
  cgl_anim.h             integer tweens and Q8 easing curves
  cgl_page.h             page objects, navigation stack, transitions
  cgl_screen.h           display + clock + pages + input = one frame
  cgl_port.h             the porting contract

include/ext/             public API of the optional module, included as <ext/...>
  cgl_ext_basis.h        the shared Q12.4 convention and the force-inline macro
  cgl_phys2d.h           fixed-point circle physics with the 2.5D vertical axis
  cgl_iso2d.h            isometric camera and counting-sort depth order (knows no bodies)
  cgl_iso2d_phys2d.h     the only bridge between those two layers
  cgl_particles.h        Q8.8 particle engine (framework-free)
  cgl_debris.h           shatter scene: bitmap to rigid-body cloud, camera fit, cycle
  cgl_bounce.h           gravity ball: gravity, four walls, restitution
  cgl_particles_view.h / cgl_debris_view.h / cgl_bounce_view.h   band painters
  cgl_*_config.h         compile-time knobs, split by layer

src/core/                pure logic, fully hardware independent
src/render/              band presentation and the DMA pipeline
src/port/                porting glue (contains no hardware code, plus one template)
src/ext/                 engines of the optional module (never in libcytgl.a)

tests/                   host regression suite
tools/                   asset generators and inspection helpers
examples/                complete applications: two STM32 projects plus shared code
```

The extension module's engines include nothing but the freestanding C headers, so a UI-only
application that does not link them pays zero bytes. Everything that must draw is split into
its own file so the maths stays framework-free and can be unit-tested alone. The module is
packaged as an opt-in CMake target, `cytgl::ext`; linking it is the only thing that puts a
byte of it into a build.

---

## 8. Resource usage

Reference build: the bundled demo on an STM32G431CB (Cortex-M4 at 170 MHz, 128 KB flash /
32 KB RAM), `-O2`, `arm-none-eabi-gcc` 14.3, every optional effect linked.

| Item | Bytes | Share |
|---|---|---|
| Flash (`text` + `data`) | 62 192 | 47.5 % of 128 KB |
| Static data only (`.data` + `.bss`) | 30 856 | 94.2 % |
| Stack reserve (`_Min_Stack_Size` = 0x540 in the linker script) | 1 344 | 4.1 % |
| RAM used (`data` + `bss` + reserve) | 32 200 | 98.3 % of 32 KB |
| Remaining unallocated | 568 | 1.7 % |

The flash and static-data columns are measured with `arm-none-eabi-gcc` 14.3; the reserve is
read from the linker script, so the last three rows are that measurement plus the reserve
actually in force. Physically available stack is 32 KB minus the static data — 1 912 B with
every effect linked.

With the two animation pages removed (`-DAPP_FX_SMASH=0 -DAPP_FX_PARTICLES=0`, which also
drops the physics world and the particle pool because nothing references them) the same
sources measure 46 720 B of flash and 15 968 B of RAM. RAM, not flash, is the binding
constraint.

The framework is a small fraction of either figure. The bulk is the demo's physics world
(480 rigid bodies plus a collision grid), the watch dial, and the 5 KB band buffers plus 5 KB
of opacity scratch that the configuration enables.

Knobs that reduce the footprint:

| Knob | Effect |
|---|---|
| `CGL_CFG_ALPHA 0` | Removes the opacity scratch entirely: about 5 KB of RAM at the reference geometry. `cgl_widget_set_alpha()` becomes a counted no-op |
| `CGL_CFG_BAND_H` | Band buffer RAM is `2 * W * BAND_H * 2` bytes. Halving it saves about 2.5 KB at the cost of more SPI window commands |
| `CGL_CFG_LABEL_MAX_TEXT` | 32 bytes per label |
| `CGL_CFG_PAGE_STACK_MAX` | 4 bytes per stack entry |
| `CGL_CFG_WATCH_TRAIL 0` | Removes the watch face's comet-trail state and its per-frame damage |
| `CGL_CFG_WATCH_DIAL_LUT 0` | Removes 578 bytes of baked dial tables, shading the surfaces procedurally instead |
| `CGL_CFG_WATCH 0` | Removes the watch face, its calendar and its damage model |
| `CGL_CFG_ARC 0` | Removes the whole radial toolkit (requires `CGL_CFG_WATCH 0`) |
| `CGL_PHYS2D_MAX_BODIES` | 16 bytes plus 2 bytes per rigid body |
| `CGL_PARTICLES_MAX` | 12 bytes per particle |
| `-DCGL_CFG_PARTICLES=0` / `-DCGL_CFG_DEBRIS=0` / `-DCGL_CFG_BOUNCE=0` | Empties that effect's translation units for builds that collect sources per directory |

---

## 9. Reference applications

`examples/STM32G431CBU6_CytStudio_Demo` is a complete application: a menu page with an
animated selection bar, a page that drops a heavy ball onto a dot-matrix logo and shatters it
into up to 480 fixed-point rigid bodies rendered through an isometric projection with
painter's-algorithm depth sorting, an information page presented as a translucent overlay, a
page that demonstrates fixed-step logic with a variable time scale, and a watch-face page
built on the `cgl_watch` widget with an attached ring gauge.

The STM32H563RGT6 example runs the same application on a different board (SPI1 with GPDMA1
channel 0, PWM backlight, inverted key polarity), which is how the portability claim is
exercised: the pages, the panel driver and the key state machine are shared, and each project
keeps only its generated code, its pin map and its DMA engine.

Build the demo by opening `examples/STM32G431CBU6_CytStudio_Demo/CytGL.cproj` in CytStudio,
which carries the ARM toolchain the firmware needs, or by using the project's own build
scripting.

### Tooling

| Tool | Purpose |
|---|---|
| `tools/mkfont.py` | TTF to glyph tables for `src/core/cgl_font_data.c`; sizes, stroke threshold, dry-run preview |
| `tools/mklogo.py` | Text or image to a dot-matrix header with grid dimensions and a cell accessor |
| `tools/mkparticletables.py` | Generates the particle engine's sine and reciprocal tables |
| `tools/mkwatchlut.c` | Prints the watch module's default dial tables, which are then pasted into the source |
| `tools/ppm_view.ps1` | Renders a PPM snapshot as character art in the terminal, with a brightness threshold for single-pixel dot matrices |
| `tests/test_image_out.c` | The only image writer: PPM and 24-bit BMP from an RGB565 buffer |

---

## 10. Design rules

These are load-bearing. Breaking one produces subtle, hard-to-locate artefacts.

1. The library includes no HAL header, no `malloc`, no libm, and no concrete colour or font
   size. It depends only on the C standard library.
2. Time enters only as `dt_ms`. The library never reads a hardware clock.
3. The widget tree changes only inside `cgl_page_nav_frame()`. Navigation is always requested,
   never applied immediately.
4. `on_step` runs exactly once per step and only for the top page, so a covered page's
   simulation freezes structurally and accumulates no debt.
5. Primitive drawing functions apply pan once each; internal functions take screen
   coordinates only.
6. A band pushed to the panel must be complete. Group opacity depends on this.
7. Write vtables with designated initialisers (`.type = ..., .draw = ...`) so that adding a
   struct member cannot silently reorder things.
8. Compile-time switches that change a struct's layout — `CGL_PHYS2D_USER_STORAGE`,
   `CGL_CFG_PARTICLES`, `CGL_CFG_DEBRIS`, `CGL_CFG_BOUNCE` — are build-wide. Two translation
   units that disagree cannot share the affected object.

---

## 11. License

CytGL is released under the Apache License 2.0. Every source file carries an
`SPDX-License-Identifier` header.

Bundled third-party assets keep their own terms:

| Directory or file | Rights holder | Licence | Where the text is |
|---|---|---|---|
| `Drivers/STM32G4xx_HAL_Driver/**`, `Drivers/STM32H5xx_HAL_Driver/**` | STMicroelectronics | BSD-3-Clause | `LICENSE.txt` in the same directory |
| `Drivers/CMSIS/**` | Arm Limited | Apache-2.0 | `Drivers/CMSIS/LICENSE.txt` |
| `Drivers/CMSIS/Device/ST/**` | Arm / STMicroelectronics | Apache-2.0 | `LICENSE.txt` in the same directory |
| `Core/**`, `*.ld`, `startup_*.s` (generated) | STMicroelectronics | BSD-3-Clause | as the HAL driver. Only the USER CODE sections of `main.c` belong to this project |
| `src/core/cgl_font_data.c` (glyph tables) | DejaVu, derived from Bitstream Vera | Bitstream Vera | `licenses/LICENSE_DEJAVU.txt`; the generated file's SPDX is `Apache-2.0 AND Bitstream-Vera` |
| `tools/DelaGothicOne-Regular.ttf` and the logo dot matrix rasterised from it | The Dela Gothic Project Authors | SIL OFL 1.1 | `licenses/LICENSE_DELAGOTHIC.txt`; the generated header's SPDX is `Apache-2.0 AND OFL-1.1` |

Two consequences worth stating explicitly:

- The project must stay **Apache-2.0**, not GPL-2.0: CMSIS is Apache-2.0, which is
  incompatible with GPLv2.
- Both the glyph tables and the logo dot matrix are **rasterised derivatives of fonts**, so the
  relevant font licence has to be confirmed before redistributing them. They are not covered by
  the Apache-2.0 licence that covers the rest of the library.

Copyright (C) 2026 CytStudio &lt;cyte_studio@foxmail.com&gt;
