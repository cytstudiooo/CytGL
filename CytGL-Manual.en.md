<div align="center">

**English** | [简体中文](CytGL-Manual.zh-CN.md)

# CytGL User Manual

<div align="left">

**Library version** 0.1.0 (`CGL_VERSION_STRING`, which also provides `CGL_VERSION_MAJOR` / `CGL_VERSION_MINOR` / `CGL_VERSION_PATCH`)
**Language standard** C11 (no compiler extensions are used; `CMAKE_C_EXTENSIONS` is disabled)
**Target platform** Microcontrollers without an operating system and without a heap. The reference hardware is an STM32G431CB (Cortex-M4, 128 KB Flash / 32 KB RAM) driving a 320×172 ST7789 panel.

---

## About This Manual

This manual is the complete usage description of CytGL and is self-contained. It covers the design premises, the layered structure, the complete execution order of one frame, porting-layer implementation, canvas and rasterization, integer mathematics, fonts, the widget tree, built-in widgets, display and band presentation, the clock and tweening, pages and navigation, the optional extension module, footprint accounting and trimming, build and test, extension recipes, and troubleshooting.

Reading conventions:

- Paths appearing in the body text are source paths inside this repository. They serve to locate the implementation; they are not material that must be consulted elsewhere.
- Code fragments are all C11. Portions omitted to keep the text short are marked with a comment; the omitted content does not affect the point the fragment illustrates.
- Identifiers, macro names, function names and file names are reproduced in their original English spelling; all other terminology uses the fixed renderings established in the terminology table.
- "Must" indicates that violating the statement produces an incorrect result or a defect that is difficult to localize; "should" indicates an engineering convention that is followed consistently; "may" indicates a choice left to the user.

---

## Contents

1. Overview
2. Getting Started
3. Architecture and the Lifecycle of a Frame
4. The Porting Layer: Connecting CytGL to Hardware
5. Base Types and Compile-Time Configuration
6. Canvas and Software Rasterization
7. Integer Mathematics
8. Fonts and Text
9. The Widget Tree
10. Built-In Widgets
11. Display Devices and the Band Presentation Pipeline
12. The Clock and Tweening
13. Pages and Navigation
14. The Screen
15. The Optional Extension Module `cytgl_ext`
16. Extension Recipes
17. Footprint and Trimming
18. Build, Test, and Tools
19. Complete Example: A Runnable Application Skeleton
20. Troubleshooting Quick Reference
Appendix A Public Header and Responsibility Index
Appendix B Compile-Time Configuration Macro Index
Appendix C Observable Counter Index
Appendix D Quick Reference: Where to Change What
Appendix E Pre-Commit Self-Check and Numerical Consistency

---

## 1. Overview

### 1.1 What CytGL Is

CytGL is a retained-mode graphical user interface framework written in C11 for microcontrollers that have no full-screen display memory. Its central judgement is that on a device with 32 KB of RAM a full-screen 320×172 RGB565 buffer requires 110 KB, so the conventional approach of "draw a complete screen and then push it" cannot hold. CytGL therefore uses **band rendering**: the screen is cut into horizontal bands a few rows high, and while the CPU rasterizes the next band, the DMA pushes the previous band to the panel.

On top of this, CytGL provides a complete set of interface facilities:

| Capability | Description |
|---|---|
| Retained-mode widget tree | Parent-child tree, absolute coordinates, clipping, culling, subtree pan, hit testing, event dispatch |
| Canvas and rasterization | Pixels, lines, rectangles, discs, dots, anti-aliased circles/thick lines/arcs, blit, several blending paths |
| Integer mathematics | 16-bit angles, Q8 sine/cosine, polar mapping, range scaling, integer square root. libm is not used |
| Bitmap fonts | Two ASCII glyph sets, 8×16 and 12×24, written row by row directly into the buffer |
| Built-in widgets | Panel `cgl_panel`, text label `cgl_label`, progress/level bar `cgl_bar`, radial toolbox `cgl_arc`, watch `cgl_watch` |
| Transitions and pages | Page object + fixed-length navigation stack, 12 transitions (including the macOS-style Genie suck-in and the S-shaped path) |
| Group opacity | A whole subtree is composited as one translucent layer; no full-screen buffer is required |
| Single time source | One clock object provides both fixed-step logic scheduling and per-frame tweening |
| Band presentation | Double-buffered bands, asynchronous pixel pushing, damage rectangle accumulation |

The framework makes no assumption about "what a screen is" or "where time comes from". Time enters only through `tick_ms` of the porting layer, input enters only through `poll_input`, and pixels leave only through one vtable.

### 1.2 Design Premises

These premises determine the shape of the code, and they are also the basis for judging whether CytGL suits a given project.

| Premise | Value | Effect on the design |
|---|---|---|
| RAM | 32 KB | A full-screen buffer is impossible, hence band rendering; every object is a static member and every size is a compile-time budget |
| Flash | 128 KB | libm is not relied upon and all arithmetic is integer or fixed-point; 64-bit division helper functions are avoided |
| Panel interface | SPI + DMA | Pixel pushing must be asynchronous, so that CPU rendering overlaps bus transfer |
| Input | A single user button (reference board) | All interaction is compressed into three event classes: short press, long press and repeat |
| Runtime environment | No RTOS, no heap | The whole project contains no `malloc`; pages, widgets and world state all live in static storage |

### 1.3 Design Rules

The following seven rules are the load-bearing structure of the framework. Violating any one of them while modifying the framework or writing an application produces display defects that are difficult to localize.

1. `include/cytgl` and `src/` contain no HAL header, no `malloc`, no libm call, and no concrete screen size or font size. The only dependencies are the C standard library headers `<stdint.h>`, `<stdbool.h>` and `<stddef.h>`.
2. Time enters the framework only in the form of `dt_ms`. The library must not call a time-base function such as `HAL_GetTick()`; the porting layer only reports the reading of the hardware timer.
3. The widget tree changes only inside `cgl_page_nav_frame()`, and that call precedes rendering. Navigation may only be registered through `cgl_page_nav_*()`; it must never take effect immediately.
4. `on_step` runs exactly once per logical step and only for the page on top of the stack. Simulation of an overlaid page is therefore structurally frozen and accumulates no backlog.
5. A drawing primitive applies the translation to its own coordinates exactly once; internal functions always use screen coordinates. `cgl_canvas_clear_clip()` deliberately takes the internal path with no translation.
6. A band pushed to the panel must be complete. The correctness of group opacity rests on this.
7. A vtable always uses designated initializers (`.type = ..., .draw = ...`). When a structure member is added, positional initializers silently shift.

### 1.4 Source Tree Structure

```
include/cytgl/          Framework public API (the externally delivered artifact)
  cytgl.h               Single entry point: one include brings in the whole public API
  cytgl_config.h        All compile-time switches
  cgl_types.h           RGB565 color, point, rectangle, input event
  cgl_math.h            Integer trigonometry and fixed-point helpers
  cgl_canvas.h          Band canvas + software rasterization (including warp)
  cgl_font.h            Bitmap font
  cgl_widget.h          Retained-mode widget tree
  cgl_panel.h           Built-in widget: background/border container
  cgl_label.h           Built-in widget: text label
  cgl_bar.h             Built-in widget: progress/level bar
  cgl_arc.h             Radial toolbox: shaded face, tapered pointer, ring gauge
  cgl_watch.h           Built-in widget: analog watch (depends on cgl_arc)
  cgl_display.h         Display driver vtable + band presentation pipeline
  cgl_time.h            The single clock
  cgl_anim.h            Integer tweening + Q8 easing curves
  cgl_page.h            Page object, navigation stack, transitions
  cgl_screen.h          Display + clock + pages + input = one frame
  cgl_port.h            Porting contract (the part the user must implement)

include/ext/            Public API of the optional extension module, included as <ext/...>
  cgl_ext_basis.h       Q12.4 conventions and force-inline macros
  cgl_phys2d.h          Fixed-point disc rigid-body physics (including the 2.5D vertical axis)
  cgl_iso2d.h           Isometric camera + depth sort (knows nothing about rigid bodies)
  cgl_iso2d_phys2d.h    The only bridge between those two layers
  cgl_particles.h       Particle engine (no dependency on the framework)
  cgl_debris.h          Debris scene (bitmap -> rigid-body cloud, camera fitting, looping)
  cgl_bounce.h          Gravity balls (gravity, four walls, restitution)
  *_view.h              The corresponding band painters (the only layer that knows the canvas)
  *_config.h            Compile-time switches split by layer

src/core/               Pure logic, entirely independent of hardware
src/render/             Band presentation and the DMA pipeline
src/port/               Porting glue layer (contains no hardware code, contains one template)
src/ext/                Engine implementations of the optional extension module (never enter libcytgl.a)

tests/                  Host (PC) regression tests, asserting per pixel against the real framework code
tools/                  Resource generation and inspection tools (glyphs, logo, particle tables, snapshot viewer)
examples/               Complete application examples: two STM32 board projects + shared application code
```

Beneath `include/` there are two parallel trees: `cytgl/` is the public API of the framework itself, and `ext/` is the public API of the optional module. Both are included in the same way (`#include <cytgl/cgl_bar.h>` and `#include <ext/cgl_phys2d.h>`), and a single `-I include` serves both. The engine of the optional module does not enter `libcytgl.a`, and a build that does not link it pays zero cost in both Flash and RAM.

### 1.5 Terminology

| Term | Meaning |
|---|---|
| Band | A horizontal run of screen rows, `CGL_CFG_BAND_H` rows high. Rendering and pushing are both performed in units of one band |
| Canvas | A drawing context bound to one band buffer, carrying the clip window, pan, warp and non-linear mapping |
| Pan | The linear displacement of a whole subtree, accumulated from the `offset` fields of the widget tree |
| Warp | The non-linear mapping of a whole subtree, used for the Genie suck-in transition; the landing point is recomputed per row, with no pixel resampling |
| Damage | The union of the rectangles that "need to be redrawn". It is a global singleton |
| Group opacity | A whole subtree is first rendered in isolation and then composited onto the background as a single unit according to an alpha value |
| Ink alpha | The opacity of a single blended primitive; it requires no extra memory |
| Logical step | The fixed-step logical advance unit, 16 ms by default, delivered only to the `on_step` of the top page |
| Frame | One `cgl_screen_frame()` call, comprising input, navigation, logic, visuals and presentation |
| Page | An object carrying a vtable and its own widget tree; an element of the navigation stack |
| Navigator | The page host widget and the fixed-length page stack; it commits navigation requests and advances transitions |
| Screen | The combination of display device + clock + navigator + input queue, exposing only "one frame" to the outside |

---

## 2. Getting Started

### 2.1 Building the Static Library

```bash
cmake -B build -DCYTGL_PORT_TEMPLATE=OFF
cmake --build build
# Output: build/libcytgl.a
```

The top-level `CMakeLists.txt` provides four options:

| Option | Default | Effect |
|---|---|---|
| `CYTGL_BUILD_TESTS` | `OFF` | Build the host (PC) regression tests and register them with CTest |
| `CYTGL_BUILD_EXAMPLES` | `OFF` | Add the `examples/` subdirectory (the firmware targets in it exist only in a checkout that has an ARM toolchain and the project settings) |
| `CYTGL_BUILD_EXT` | `OFF` | Also build the optional extension module, producing `cytgl_ext` / `cytgl::ext` |
| `CYTGL_PORT_TEMPLATE` | `ON` | Compile `src/port/cgl_port_template.c` into the library as a syntax check |

The library target is named `cytgl`, with the alias `cytgl::cytgl`. The install rules place `libcytgl.a` into `lib/` and `include/cytgl/` into `include/`.

### 2.2 Integrating into Your Own Project

Including it as a subproject:

```cmake
add_subdirectory(third_party/CytGL)
target_link_libraries(my_app PRIVATE cytgl::cytgl)
```

Without CMake, add every `.c` file under `src/core`, `src/render` and `src/port` (except `src/port/cgl_port_template.c`) to the project, and add `include/` to the include path.

**Include path rule (must be observed)**: framework sources include their own headers by bare file name (for example `#include "cgl_screen.h"`), and that form resolves inside the directory of the source file. Therefore `src/core` and `include/` must not both be added to the global include path — public headers and private headers share the same names, and the resolution order becomes indeterminate. Only `include/` should appear in the global include path.

### 2.3 Minimal Runnable Program

The following program depends on no hardware and runs on a PC: the display driver writes the bands into a memory array, which is enough to verify the entire chain and to export the result as a PPM image for inspection.

```c
#include <cytgl/cytgl.h>
#include <stdio.h>

/* ---- 1. Display driver: write pixels into a memory array (equivalent to a "panel") ---- */
#define FB_W CGL_CFG_SCREEN_W
#define FB_H CGL_CFG_SCREEN_H

static cgl_color_t s_fb[FB_W * FB_H];

typedef struct { uint16_t x0, y0, x1, y1; } mock_panel_t;
static mock_panel_t s_panel;

static int mock_init(void *ctx) { (void)ctx; return 0; }

static void mock_set_window(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    mock_panel_t *p = (mock_panel_t *)ctx;
    p->x0 = x0; p->y0 = y0; p->x1 = x1; p->y1 = y1;
}

/* On real hardware this must start the DMA and return immediately; the memory mock copies directly */
static void mock_write_pixels(void *ctx, const cgl_color_t *px, uint32_t count)
{
    mock_panel_t *p = (mock_panel_t *)ctx;
    uint16_t w = (uint16_t)(p->x1 - p->x0 + 1u);
    for (uint32_t i = 0; i < count; ++i)
    {
        uint16_t x = (uint16_t)(p->x0 + (i % w));
        uint16_t y = (uint16_t)(p->y0 + (i / w));
        s_fb[(uint32_t)y * FB_W + x] = px[i];
    }
}

static void mock_wait_idle(void *ctx) { (void)ctx; }

static const cgl_display_drv_t s_mock_drv = {
    .name = "mock",
    .init = mock_init,
    .set_window = mock_set_window,
    .write_pixels = mock_write_pixels,
    .wait_idle = mock_wait_idle,
    .set_backlight = NULL,               /* Optional member; NULL is permitted */
    .width = CGL_CFG_SCREEN_W,           /* Must agree with the compile-time configuration */
    .height = CGL_CFG_SCREEN_H,
};

/* ---- 2. Clock: a monotonically increasing millisecond count ---- */
static uint32_t mock_tick_ms(void)
{
    static uint32_t ms;
    ms += 16u;                            /* Simulate 16 ms per frame */
    return ms;
}

/* ---- 3. Interface: one panel plus one label ---- */
static cgl_panel_t s_bg;
static cgl_label_t s_title;

int main(void)
{
    static cgl_port_ctx_t port;
    static const cgl_port_t desc = {
        .name = "host",
        .display = &s_mock_drv,
        .display_ctx = &s_panel,
        .tick_ms = mock_tick_ms,          /* Supplying the clock removes the need to compute dt */
        .poll_input = NULL,               /* No input source */
    };

    if (cgl_port_bind(&desc, &port, NULL) != 0) { return 1; }

    cgl_screen_t *screen = cgl_port_screen(&port);
    cgl_widget_t *root   = cgl_screen_root(screen);   /* Navigator host, covering the whole screen */

    cgl_panel_init(&s_bg, "bg",
                   cgl_rect(0, 0, CGL_CFG_SCREEN_W - 1, CGL_CFG_SCREEN_H - 1),
                   CGL_CFG_BG_COLOR, CGL_CFG_BG_COLOR);
    (void)cgl_widget_add_child(root, &s_bg.base);

    cgl_label_init(&s_title, "title", cgl_rect(16, 16, 240, 40),
                   &cgl_font_12x24, "Hello CytGL",
                   CGL_CFG_FG_COLOR, CGL_CFG_BG_COLOR, false);
    (void)cgl_widget_add_child(&s_bg.base, &s_title.base);

    cgl_screen_redraw_all(screen);        /* Force a full-screen redraw on the first frame */

    for (int i = 0; i < 4; ++i) { (void)cgl_port_frame(&port); }

    /* At this point s_fb holds the whole screen; it can be written out as a PPM image for inspection */
    FILE *f = fopen("frame.ppm", "wb");
    if (f != NULL)
    {
        fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
        for (int i = 0; i < FB_W * FB_H; ++i)
        {
            cgl_color_t c = s_fb[i];
            unsigned char rgb[3] = {
                (unsigned char)(((c >> 11) & 0x1Fu) << 3),
                (unsigned char)(((c >> 5) & 0x3Fu) << 2),
                (unsigned char)((c & 0x1Fu) << 3),
            };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    return 0;
}
```

Key points of this example:

- When the third argument of `cgl_port_bind()` is `NULL`, the framework holds a static navigator internally and initializes it with the full-screen rectangle. Pass your own instance when you need to build the navigator yourself.
- `cgl_screen_root()` returns the navigator host widget; attaching widgets to it directly yields an interface with no page model. When the page model is used, widgets should be attached to the root widget of a page (see Chapter 13).
- `cgl_screen_redraw_all()` is called before entering the loop for the first time. Although `cgl_display_init()` itself marks the whole screen as damaged, the explicit call expresses the intent.
- A widget must draw the entire rectangle it occupies by itself. The framework does not clear the background: band rendering has no "previous frame" available for restoration (see 10.1 and 11.4).

### 2.4 Running the Regression Tests on a PC

```bash
cmake -B build/host -DCYTGL_BUILD_TESTS=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The tests compile and run the **real framework sources**, replacing only the display driver with a mock driver that writes into a memory array, so the assertions check the shipping code path. Rendering tests also emit `frameNN.ppm` / `.bmp` snapshots that can be opened directly. Running a single test:

```bash
ctest --test-dir build/host -R alpha --output-on-failure
```

---

## 3. Architecture and the Lifecycle of a Frame

### 3.1 Layering and Dependency Direction

```
         Application Layer (App)
            |  depends downward only
     cgl_port.c/.h      Porting layer: the only file that binds hardware to the framework
            |
         CytGL framework  types / canvas / fonts / widgets / display / clock / tween / pages / screen
            |            <- no HAL headers, no malloc, no libm
        Panel driver      e.g. ST7789, knows only one bus vtable
            |
        Board support (BSP)  pins, SPI, DMA, keys, knows only CubeMX handles
            |
        Core/ + Drivers/  CubeMX / HAL generated code

  include/ext + src/ext  not on this stack: it is an optional module beside the stack
```

Dependencies are strictly unidirectional. The criterion for portability is: copy the entire `src/` tree into any other project and it remains usable — because it does not know what a screen is, nor where time comes from.

| Layer | May depend on | Must never do |
|---|---|---|
| Application layer | Public framework API, the optional extension module, `cgl_port.h` | Know about a particular SPI instance, a particular pin, panel orientation, or a particular millisecond count |
| Porting layer | Framework contracts, panel driver, BSP, HAL | Contain user-interface logic (pages, widgets, layout) |
| Optional module (engine) | Only the C standard library | Include framework headers, allocate memory, know that a screen exists |
| Optional module (renderer) | The engine and `cgl_canvas.h` / `cgl_types.h` | Allocate memory, know that a "widget" exists |
| Framework | Only the C standard library | `stm32g4xx_hal.h`, concrete colors, concrete font sizes, `malloc` |
| Panel driver | The bus vtable | Touch registers (that is the responsibility of the BSP) |
| Board support | HAL handles and macros generated by `.ioc` | Know the concepts of "panel" or "user interface" |

### 3.2 The Call Chain of One Frame

```
app_run()                                application main loop
 +- cgl_port_frame(&port)                porting-layer frame entry
     +- dt = tick_ms() - last_tick       unsigned subtraction, wrap-safe
     +- poll_input(dt)                   porting-layer key/touch scan, posts events only
     +- cgl_screen_frame(&screen, dt)    the one frame owned by the framework
        1) cgl_clock_begin(dt)          the only place in the project that accumulates time
        2) drain the input queue        dispatched at frame boundaries (the queue head is not consumed during a transition)
        3) cgl_page_nav_frame()         commits page changes, advances transitions
        4) while (cgl_clock_next_step()) page->on_step(step_ms)
        5) page->on_frame(&clock)       once per frame: tweens, state machines, text refresh
        6) cgl_widget_update_tree()     purely visual animation
        7) cgl_display_present()        per-band rasterization + DMA push
```

Three hard rules concerning ordering:

1. **There is only one time source.** The clock is the only place in the project that accumulates time; the porting layer only reports the reading of the hardware timer (`cgl_port_t::tick_ms`), and the framework guarantees the order of the remaining six steps. No requirement of the form "A must be called before B, otherwise the physics freezes" exists in the project.
2. **The widget tree changes only at the start of a frame.** Only `cgl_page_nav_frame()` calls `cgl_widget_add_child()` / `remove_child()` / `set_visible()`, and it runs before `on_step` and before presentation, so the widget tree is constant during rendering — "the tree changed halfway through drawing" cannot occur.
3. **Logic and visuals are two domains.** Physics runs only in `on_step` (fixed step, replayable); tweens and text refresh run in `on_frame` (following the frame rate).

### 3.3 The Time Model: Two Domains, One Owner Each

| Domain | Field | Where it is used | Characteristics |
|---|---|---|---|
| Logic (fixed step) | `step_ms` (default `CGL_CFG_CLOCK_STEP_MS` = 16 ms) | Page `on_step` | Fixed step, independent of the frame rate; simulation speed is unaffected by SPI bandwidth |
| Visuals (per frame) | `dt_ms` (capped at 100 ms) | Page `on_frame`, widget `update` | Follows the frame rate, dropped frames are permitted |

- `cgl_clock_begin()` derives from an accumulator "the number of steps this frame should execute" (capped by `CGL_CFG_CLOCK_MAX_STEPS` = 4), and `while (cgl_clock_next_step())` iterates exactly that many times. A page can neither run one step too many nor one step too few.
- **Stalls are not caught up**: the part of a single frame's `dt` that exceeds `max_dt_ms` is truncated; when the number of owed whole steps exceeds the cap, the whole-step backlog is discarded at once (the sub-step remainder is retained). Simulation time is allowed to run slightly slow rather than performing a "fast-forward after a stall".
- The frame rate is computed from accumulated time (a 500 ms statistics window) and shares the same time as the simulation, so the illusion of "58 fps displayed while the physics runs at 60 Hz" cannot arise.
- 16 ms is used rather than 16.67 ms: the clock counts in whole milliseconds, the 4% difference between 62.5 steps per second and 60 steps per second is imperceptible in every tuning constant of this project, and floating-point arithmetic is avoided.

### 3.4 The Band Presentation Pipeline

```
buffer[cur]  rasterize -> set_window(band) -> write_pixels (DMA, asynchronous)   ] CPU continues
buffer[!cur] rasterize (the DMA is still shifting out the previous band)         ] in parallel with the bus
                                       -> wait_idle() -> next window ...
```

| Item | Value |
|---|---|
| Band buffer RAM | 2 × 320 × 4 × 2 B = 5 KB |
| Full-screen push | 172 / 4 = 43 bands |
| Damage handling | Every **complete band** covered by a damage rectangle is redrawn (the panel has no frame buffer, so the bands handed to it must be complete) |
| Rasterization order | Each primitive is first offset by `pan` to obtain screen coordinates, then clipped, and finally landed on the rows within the band |

Two constraints that must be maintained (group opacity depends on them):

1. **Every band must be redrawn in full starting from the root of the widget tree.** `cgl_display_present()` never "redraws only the damaged part". The correctness of the save-under compositor also depends on this: the "background copy" it saves is the content already drawn for that band; if partial redraw were actually implemented, the background would become stale and the blending result would immediately be wrong.
2. **`cgl_display_t.alpha[]` is written only by the CPU and never enters the DMA.** It differs from `band[]`: while band N is being rendered, `band[!cur]` is being read by the DMA, so that block cannot be used as a scratch area.

### 3.5 The Three Decoupling Seams

| Seam | Location | What it isolates | Benefit |
|---|---|---|---|
| `cgl_display_drv_t` | `include/cytgl/cgl_display.h` | How pixels reach the panel | After switching to a simulation driver that writes a memory array, the real framework code can be regression-tested pixel by pixel on a PC |
| Bus vtable (`lcd_bus_t` in the example project) | `examples/common/Devices/lcd_bus.h` | How electrical signals leave the chip | Hardware SPI+DMA, software emulation, a parallel bus, and a simulator are interchangeable without changing a single line of the panel driver |
| `cgl_widget_vt_t` | `include/cytgl/cgl_widget.h` | What a widget looks like | Adding a widget = a struct whose first member is a `cgl_widget_t` plus one vtable |

The only place not abstracted into a seam is the damage sink: it is a global singleton, so only one `cgl_screen` can be active at a time. This is sufficient for a single-screen device; to support two screens, the sink should be changed to one per screen.

---

## 4. The Porting Layer: Connecting CytGL to Hardware

### 4.1 Contract Overview

The framework places only three requirements on the hardware, all handed to `cgl_port_bind()` at once through a single `cgl_port_t` descriptor:

| # | Item | Type | Required |
|---|---|---|---|
| 1 | Display driver: how pixels reach the panel | `const cgl_display_drv_t *` | Yes |
| 2 | Millisecond clock: where time comes from | `uint32_t (*)(void)` | No (if dt is computed independently) |
| 3 | Input source: scans for and posts events | `void (*)(void *, uint32_t)` | No |

After binding, only one `cgl_port_frame()` call is needed per frame; internally it performs "fetch time → scan input → advance one frame".

### 4.2 `cgl_port_t` Fields

| Field | Type | Meaning |
|---|---|---|
| `name` | `const char *` | Board/porting-layer name, used for debugging and logging |
| `display` | `const cgl_display_drv_t *` | Display driver, must be provided |
| `display_ctx` | `void *` | Context passed to the driver callbacks (usually the panel object); may be `NULL` |
| `tick_ms` | `uint32_t (*)(void)` | Monotonically increasing millisecond count. May be `NULL`, in which case the framework advances by a fixed `CGL_CFG_CLOCK_STEP_MS` |
| `poll_input` | `void (*)(void *, uint32_t)` | Input source hook. Called once per frame, with the `dt_ms` that the frame is about to use as its argument; it is called before the screen drains the input queue, so events produced here are handled by the current frame |
| `input_ctx` | `void *` | Argument passed to `poll_input` |
| `board_init` | `int (*)(void *)` | Power-on initialization hook, may be `NULL`. Called inside `cgl_port_bind()`, before the display driver's `init()`. Suitable for pin configuration, bus clock setup, and starting the backlight PWM |
| `board_ctx` | `void *` | Argument passed to `board_init` |

### 4.3 The Display Driver `cgl_display_drv_t`

```c
typedef struct cgl_display_drv
{
    const char *name;

    int  (*init)(void *ctx);                                       /* power-on */
    void (*set_window)(void *ctx, uint16_t x0, uint16_t y0,
                       uint16_t x1, uint16_t y1);                  /* open a window, both ends inclusive */
    void (*write_pixels)(void *ctx, const cgl_color_t *px,
                         uint32_t count);                          /* asynchronous push */
    void (*wait_idle)(void *ctx);                                  /* wait for the bus to become idle */
    void (*set_backlight)(void *ctx, uint8_t percent);             /* may be NULL */

    int16_t width;   /* must equal CGL_CFG_SCREEN_W */
    int16_t height;  /* must equal CGL_CFG_SCREEN_H */
} cgl_display_drv_t;
```

**`init`**: power-on, reset, and writing the controller register sequence. Returns 0 on success and non-zero on failure. `cgl_port_bind()` propagates that failure, so "the panel did not come up" does not become "the user interface half works".

**`set_window`**: programs the write window. Both coordinates are **inclusive**, consistent with the `CASET` / `RASET` convention of the ST7789 / ILI9341. The framework calls it once per band, and the implementation should be as lean as possible; a common optimization is to skip repeated `CASET` / `RASET` writes when only `y` changes. Window coordinates are panel coordinates rather than frame-buffer coordinates: if the visible area lies within a larger GRAM, the offset should be added here.

**`write_pixels`**: **must be asynchronous** (a typical implementation starts the DMA and returns immediately). The entire performance model rests on the overlap between "the CPU rasterizing the next band" and "the bus shifting out the previous band". A blocking implementation still works, but the frame rate is roughly halved (pushing a full frame of 110 KB over 42.5 MHz SPI: about 48 fps asynchronous, about 24 fps synchronous). The pointer passed in points to one band buffer within `cgl_display_t.band[]`, and is valid until `wait_idle()` returns. Three common errors:

- DC must be high (data mode) before pixels are sent; commands and parameters require DC low. A color shift of one pixel usually stems from this.
- CS must remain asserted throughout the entire transfer. Because the function returns immediately, CS cannot be released inside the function and should be placed in `wait_idle()`.
- One RGB565 pixel corresponds to one 16-bit SPI frame. Sending byte by byte makes the DMA arbitrate the bus per byte, roughly halving the pixel rate. Therefore `count` is a **pixel count, not a byte count**.

**`wait_idle`**: blocks until the transfer started by `write_pixels` completes, then releases CS and other per-transfer state. Under the STM32 HAL this means waiting for the SPI state to return to `HAL_SPI_STATE_READY`.

**`set_backlight`**: optional; when `NULL` is passed, the framework skips it safely. `percent` ranges from 0..100; a backlight that supports only on/off need only treat any non-zero value as on.

**`width` / `height`**: the geometric dimensions of the panel in the **landscape** orientation (width > height). CytGL renders in landscape only. `cgl_port_bind()` compares these two values against `CGL_CFG_SCREEN_W` / `CGL_CFG_SCREEN_H` and refuses to start when they disagree. This is deliberate: a silent disagreement yields a slightly torn or offset image that is extremely difficult to localize, whereas refusing the binding points directly at the configuration item.

### 4.4 The Clock

```c
static uint32_t my_tick_ms(void)
{
    return (uint32_t)HAL_GetTick();      /* or SysTick, or clock_gettime()/1e6 */
}
```

Requirements: monotonically increasing, a resolution of 1 ms is sufficient, and it must not go backwards. The framework computes `dt = now - last` by unsigned subtraction, so a single wrap is harmless.

**Do not accumulate a private counter here.** CytGL is the sole owner of time in the system: the caller supplies `dt`, and the framework schedules everything else from it. A counter private to the porting layer reintroduces the class of defect of "who ticks first", which the clock exists precisely to eliminate.

### 4.5 Input

The framework never polls any device on its own, but it calls the porting layer's polling hook once per frame. Translate the scan result into a `cgl_event_t` and **post** it:

```c
static void my_poll_input(void *user, uint32_t dt_ms)
{
    cgl_port_ctx_t *port = (cgl_port_ctx_t *)user;
    key_event_t k = key_scan(dt_ms);              /* debouncing uses the dt passed in */

    if (k == KEY_NONE) { return; }

    cgl_event_t ev = {
        .kind = CGL_EV_KEY_DOWN,
        .key = 0,
        .x = 0,
        .y = 0,
        .hold_ms = key_hold_ms(),
    };
    (void)cgl_port_post_event(port, &ev);
}
```

`dt_ms` is the dt of the frame about to run, so this is the only place where debouncing and long-press timing can be handled correctly — do not read the clock here. If the board has no input, or if events arrive through another path (touch interrupt, serial protocol, simulator), leave `poll_input` as `NULL` and call the posting function at the point where the event is produced:

```c
cgl_port_post_event(&g_port, &ev);   /* equivalent to cgl_screen_post_event(cgl_port_screen(&g_port), &ev) */
```

Two rules keep input handling simple:

1. **Post only, never dispatch.** Events are enqueued first and dispatched uniformly at frame boundaries, so a page is never entered or left in the middle of a frame. The queue length is determined by `CGL_CFG_INPUT_QUEUE` (default 8, must be a power of 2); when the queue is full events are dropped and counted in a counter that can be read with `cgl_screen_dropped_events()`. Calling `cgl_port_post_event()` from an interrupt is therefore safe, but debouncing/scanning itself should remain inside the frame.
2. **Keep exactly one producer.** Scan all inputs and post from a single function, called once per frame carrying that frame's dt — this is precisely the semantics of `poll_input`, and the reason it is superior to posting events from various places in the application layer. Touch support requires no additional framework code: post `CGL_EV_TOUCH_*`, and hit testing is already implemented by `cgl_widget_hit_test()`.

The reference example shows how a single key is handled: one long press is translated into `CGL_EV_KEY_LONG` immediately followed by a `CGL_EV_BACK`, so that "long press to go back" does not need to be implemented by every page individually.

### 4.6 Framework-Side Instance and API

```c
typedef struct cgl_port_ctx
{
    const cgl_port_t *port;   /* the bound descriptor                */
    cgl_display_t display;    /* display instance (with band buffers, static storage) */
    cgl_screen_t screen;      /* the screen: display + clock + navigator + input */
    uint32_t last_tick;       /* timestamp of the previous frame     */
    bool bound;               /* whether binding succeeded           */
} cgl_port_ctx_t;
```

The caller treats this structure as an opaque object; the members are public only to avoid heap allocation.

| API | Meaning |
|---|---|
| `int cgl_port_bind(const cgl_port_t *port, cgl_port_ctx_t *ctx, cgl_page_nav_t *nav)` | Performs `board_init` → bind the display driver → initialize the screen, in that order. When `NULL` is passed for `nav`, the framework uses an internal static navigator and initializes it with the full-screen rectangle. A return of 0 indicates success; non-zero indicates that `board_init`, display initialization, or the geometry check failed |
| `uint32_t cgl_port_frame(cgl_port_ctx_t *ctx)` | Advances one frame: computes dt → `poll_input(dt)` → `cgl_screen_frame()`. Returns the dt used for this call (returns 0 when not bound) |
| `cgl_display_t *cgl_port_display(ctx)` / `cgl_screen_t *cgl_port_screen(ctx)` | Retrieves the framework objects; returns `NULL` when not bound |
| `bool cgl_port_post_event(ctx, const cgl_event_t *ev)` | Posts one input event |
| `void cgl_port_set_backlight(ctx, uint8_t percent)` | Sets the backlight; a safe no-op when the driver does not implement it |

If the application needs to control the cadence itself (an RTOS tick hook, a fixed 16 ms timer, a 60 Hz simulator), skip `cgl_port_frame()` and call `cgl_screen_frame(&screen, dt_ms)` directly; input scanning is then also driven by the caller with the same dt.

### 4.7 Porting Checklist

- [ ] `cgl_display_drv_t` is implemented; `init` returns non-zero on failure.
- [ ] `set_window` uses inclusive bounds at both ends and panel coordinates (the offset required by the module is added).
- [ ] `write_pixels` returns immediately after starting the asynchronous transfer.
- [ ] `wait_idle` blocks until that transfer completes, then releases CS.
- [ ] `drv->width/height` equals `CGL_CFG_SCREEN_W/H` and is landscape.
- [ ] `tick_ms` returns a monotonic millisecond count derived from the system tick.
- [ ] `poll_input` (if used) posts events only, is driven by a single producer, and performs no dispatch itself.
- [ ] `cgl_port_bind()` returns 0.
- [ ] The first frame shows an image: after `cgl_screen_redraw_all()` is called, content appears on the panel.
- [ ] Colors are correct (no byte swapping occurs): a red square is displayed as red.
- [ ] Orientation is correct: a square is not displayed as a rectangle.

### 4.8 Porting Troubleshooting

| Symptom | Possible cause |
|---|---|
| `cgl_port_bind()` immediately returns non-zero | `drv->width/height` disagrees with `CGL_CFG_SCREEN_W/H`, or `init` failed |
| The image is uniformly offset | The visible area of the module lies within a larger GRAM, so the offset must be added in `set_window` |
| The image tears, with a moving horizontal seam | `write_pixels` is not actually asynchronous, or CS is released too early |
| Colors are wrong / channels are interchanged | A byte-order problem. RGB565 is native-endian; check whether a redundant byte swap is performed and whether `COLMOD` is set to 16 bits |
| Colors are inverted | IPS panels usually require `INVON` |
| The full screen shows only one color (usually the background color) | The damage region is empty: no widget called `cgl_widget_invalidate()`. Call `cgl_screen_redraw_all()` |
| Only the top rows are drawn | Geometry disagreement: the driver reports portrait while the configuration is landscape (or the reverse) |
| The frame rate is about half the value the bus allows | `write_pixels` is synchronous, so the CPU and the bus no longer overlap |
| Roughly one band is missing, or the image jumps | The buffer handed to `write_pixels` is reused before `wait_idle`; confirm that the DMA has actually finished |
| A compile-time `_Static_assert` reports `CGL_CFG_ALPHA_BUDGET` | The RAM required for group opacity exceeds the budget. Lower `CGL_CFG_ALPHA_LEVELS` / `CGL_CFG_BAND_H`, or deliberately raise the budget |

---

## 5. Base Types and Compile-Time Configuration

### 5.1 Colors

`cgl_color_t` is 16-bit native RGB565. The byte order of the physical bus/panel is the responsibility of the display driver; the framework does not intervene.

```c
#define CGL_RGB565(r, g, b)   /* 5-6-5 packing */
```

Predefined colors: `CGL_COLOR_BLACK`, `CGL_COLOR_WHITE`, `CGL_COLOR_RED`, `CGL_COLOR_GREEN`, `CGL_COLOR_BLUE`, `CGL_COLOR_YELLOW`, `CGL_COLOR_CYAN`, `CGL_COLOR_MAGENTA`, `CGL_COLOR_GRAY`, `CGL_COLOR_DARK`, `CGL_COLOR_DIM`, `CGL_COLOR_AMBER`, `CGL_COLOR_CREAM`.

The only blending formula in the framework (every blending path goes through it):

```c
cgl_color_t cgl_color_blend_q8(cgl_color_t dst, cgl_color_t src, uint16_t alpha_q8);
```

- `alpha_q8` is Q8: 0 keeps `dst` entirely, 256 becomes `src` entirely. Both endpoints are exact, so callers may safely pass endpoint values.
- Rounding is **truncation** (`>>8`) rather than round-to-nearest. This is established framework behavior, and changing it would rewrite the pixel values of every existing image. The cost is at most 1 LSB of error per level (3.2% of a 5-bit channel), and at most 2 LSB with two nested levels.

### 5.2 Geometry

All coordinates are signed screen pixels. Rectangle bounds are **closed intervals** at both ends: `width == x1 - x0 + 1`.

```c
typedef struct { int16_t x, y; }                       cgl_point_t;
typedef struct { int16_t x0, y0, x1, y1; }             cgl_rect_t;
```

| Inline function | Semantics |
|---|---|
| `cgl_rect(x0, y0, x1, y1)` | Constructs from the four edges |
| `cgl_rect_wh(x, y, w, h)` | Constructs from the top-left corner and the width/height (converted internally to a closed interval) |
| `cgl_rect_none()` | A rectangle guaranteed to be empty (`x0 > x1`) |
| `cgl_rect_is_empty(r)` | Whether it is empty |
| `cgl_rect_width(r)` / `cgl_rect_height(r)` | Width / height |
| `cgl_rect_contains_point(r, x, y)` | Whether the point lies inside the rectangle |
| `cgl_rect_contains_rect(r, o)` | Whether the rectangle fully contains another rectangle (an empty rectangle returns true) |
| `cgl_rect_intersect(a, b)` / `cgl_rect_union(a, b)` | Intersection / union (the union special-cases empty rectangles) |
| `cgl_rect_translate(r, d)` | Translation |

### 5.3 Input Events

```c
typedef enum {
    CGL_EV_NONE = 0,
    CGL_EV_KEY_DOWN,     /* just pressed                          */
    CGL_EV_KEY_REPEAT,   /* automatic repeat while held           */
    CGL_EV_KEY_LONG,     /* past the threshold, emitted only once */
    CGL_EV_KEY_UP,       /* released (hold_ms is the total time)  */
    CGL_EV_TOUCH_DOWN,
    CGL_EV_TOUCH_MOVE,
    CGL_EV_TOUCH_UP,
    CGL_EV_BACK          /* logical "back" (encoder side key / gesture / remote) */
} cgl_event_kind_t;

typedef struct {
    uint8_t  kind;      /* cgl_event_kind_t                       */
    uint8_t  key;       /* key index; always 0 on a single-key board */
    int16_t  x, y;      /* touch coordinates (0 for key events)   */
    uint32_t hold_ms;   /* time elapsed from press to this event  */
} cgl_event_t;
```

Events are "one-shot actions" independent of geometry, and therefore do not belong to the widget tree; the widget tree is responsible only for dispatching downward, and whether an event is accepted is decided by the page.

### 5.4 Compile-Time Configuration Macros

All switches live in `include/cytgl/cytgl_config.h`, and nearly all of them are protected by `#ifndef`, so the build system can override them with `-D` without modifying library sources. This point is especially important when multiple products share a single checkout.

**Panel Geometry and Bands**

| Macro | Default | Effect |
|---|---|---|
| `CGL_CFG_SCREEN_W` / `CGL_CFG_SCREEN_H` | 320 / 172 | The **sole source** of panel geometry; when the driver geometry does not match, `cgl_screen_init()` returns -1 |
| `CGL_CFG_BAND_H` | 4 | Band height. A compromise between RAM and SPI window-command overhead (the two buffers total 5 KB) |

**Group Opacity**

| Macro | Default | Effect |
|---|---|---|
| `CGL_CFG_ALPHA` | 1 | Feature master switch. When set to 0 the display layer allocates no scratch area, `cgl_widget_set_alpha()` degenerates to "records the value but takes no effect" (it still counts), and RAM drops to zero |
| `CGL_CFG_ALPHA_LEVELS` | 2 | Number of group levels that may be nested simultaneously; each level consumes one band of RAM. The permitted range is 1..8, and out-of-range values are trapped by `_Static_assert` |
| `CGL_CFG_ALPHA_LEVEL_STRIDE` | `SCREEN_W * BAND_H` | Pixels per level; must be no less than the actual band width × band height |
| `CGL_CFG_ALPHA_BUDGET` | 6144 | Upper limit on the RAM this feature may consume (bytes). Exceeding it is a **compile-time** error and does not turn into a runtime stack overflow |
| `CGL_CFG_ALPHA_BYTES` | derived | The actual usage computed from the three items above; may be used directly in compile-time assertions |

**Feature Limits**

| Macro | Default | Effect |
|---|---|---|
| `CGL_CFG_LABEL_MAX_TEXT` | 32 | Static text buffer of each `cgl_label_t` (including the terminating `\0`) |
| `CGL_CFG_WIDGET_MAX_CHILDREN` | 16 | Upper limit on the **direct** children of a single widget. The limit is enforced: when it is exceeded, `cgl_widget_add_child()` returns false and the event is counted in `cgl_widget_children_dropped()`. It consumes no extra RAM (the child list is counted by traversal at build time) |

**Radial Module Switches**

| Macro | Default | Effect |
|---|---|---|
| `CGL_CFG_ARC` | 1 | Radial toolbox (shaded disc faces, tapered anti-aliased needles, ring gauges). When set to 0 the entire module disappears from the link |
| `CGL_CFG_WATCH` | 1 | Watch widget (time base and calendar, graduation ring, three hands, hub, glass cover). **Requires `CGL_CFG_ARC`**, and the relationship between the two is checked by `_Static_assert` |
| `CGL_CFG_WATCH_DIAL_LUT` | follows `CGL_CFG_WATCH` | Whether to bake the two **default** watch dial lookup tables (256-entry face + 33-entry well, 578 B of Flash). When set to 0 the tables disappear, and faces whose `lut` is `NULL` switch to procedural shading, which is slightly slower per pixel but consumes no Flash |
| `CGL_CFG_WATCH_TRAIL` | follows `CGL_CFG_WATCH` | Second-hand comet tail: 60 B of brightness per division plus a decay accumulator. This is the watch's only per-element state, and the part most worth compiling out when memory is tight |
| `CGL_CFG_WATCH_GLASS` | follows `CGL_CFG_WATCH` | Glass cover pass: composites one reflection layer **above** the hands. Pure Flash cost, but adds one full-dial pass per frame |

`CGL_CFG_WATCH` and `CGL_CFG_ARC` being mutually independent is deliberate: the toolbox is a general facility, and the watch is merely one application of it. Setting only `-DCGL_CFG_ARC=0` halts at the assertion and gives the name of the switch that must also be set, which is better than "a default value quietly dragging the whole watch into the toolbox".

**Watch Geometry (Defaults, Not Limits)**

`cgl_watch_geometry_default()` fills a `cgl_watch_geom_t` from the macros below; to change the dial dimensions, modify your own structure — there is no need to touch these macros.

| Macro | Default | Meaning |
|---|---|---|
| `CGL_CFG_WATCH_CX` / `_CY` | 86 / 86 | Dial center |
| `CGL_CFG_WATCH_R_FACE` | 80 | Dial face radius |
| `CGL_CFG_WATCH_R_RING` / `_RING_W` | 75 / 3 | Center radius and width of the second ring |
| `CGL_CFG_WATCH_R_WELL_IN` / `_WELL_OUT` | 71 / 79 | Inner and outer radii of the second-ring well cross-section |
| `CGL_CFG_WATCH_R_TICK_OUT` | 70 | Outer-end radius of the graduation ticks |
| `CGL_CFG_WATCH_R_TICK_HOUR_IN` / `_MIN_IN` | 64 / 67 | Inner-end radii of the hour and minute tick marks |
| `CGL_CFG_WATCH_R_HOUR` / `_MINUTE` / `_SECOND` | 42 / 62 / 71 | Tip radii of the three hands |
| `CGL_CFG_WATCH_R_TAIL` | 14 | Counterweight tail length of the second hand |
| `CGL_CFG_WATCH_R_HUB` | 5 | Hub radius |
| `CGL_CFG_WATCH_R_HOUR_BASE` / `_MINUTE_BASE` | 14 / 16 | Rearward extension length of the hands behind the hub (the hands are tapered blades with their wide end hidden under the hub) |
| `CGL_CFG_WATCH_HOUR_W_BASE` / `_W_TIP` | 13 / 4 | Full width of the hour hand at the hub and at the tip |
| `CGL_CFG_WATCH_MINUTE_W_BASE` / `_W_TIP` | 11 / 3 | The same for the minute hand |
| `CGL_CFG_WATCH_SECOND_W` | 3 | Second-hand width |
| `CGL_CFG_WATCH_SHADOW_PX` / `_SHADOW_ALPHA` | 2 / 77 | Offset (lower right) and strength of the hand shadow (Q8; 30% reads as "the hands float above the dial") |
| `CGL_CFG_WATCH_TICK_MS` | 100 | Time resolution of the second hand's "tick" movement: the hand rests on the current second's mark, and the last 100 ms is the flick that carries it to the next division |
| `CGL_CFG_WATCH_REVEAL_MS` | 760 | Duration of the entrance sweep (12:00 → the current time) |
| `CGL_CFG_WATCH_PULSE_MS` | 620 | Flash duration when the minute jumps |
| `CGL_CFG_WATCH_TRAIL_MS` | 2400 | Comet-tail afterglow duration. It must be several times the tick interval, otherwise the "comet tail" is only a single bright dot |
| `CGL_CFG_WATCH_SECOND_MODE` | 1 | Initial value of `cgl_watch_anim_default()`: 1 = mechanical tick, one step per second; 0 = continuous sweep. Note that this is the initial value of the **runtime field** `cgl_watch_anim_t`, and an application may offer both in a settings screen |

**Time**

| Macro | Default | Effect |
|---|---|---|
| `CGL_CFG_CLOCK_STEP_MS` | 16 | Logical step |
| `CGL_CFG_CLOCK_MAX_STEPS` | 4 | Maximum number of logical steps per frame (the upper bound on catch-up that prevents stalling) |

**Pages and Transitions**

| Macro | Default | Effect |
|---|---|---|
| `CGL_CFG_PAGE_STACK_MAX` | 4 | Page stack depth (fixed-length array, no heap) |
| `CGL_CFG_TRANSITION_MS` | 220 | Default duration of the slide/cover/fade series; 0 means these types switch instantly |
| `CGL_CFG_GENIE_MS` | 320 | Default duration of the Genie suck-in. Being slightly longer than a slide is deliberate: the suck-in is a geometric deformation and the shape itself must be seen clearly |
| `CGL_CFG_TRANSITION_EASE` | `CGL_EASE_OUT_CUBIC` | Default easing curve of the series above |
| `CGL_CFG_GENIE_OUT_EASE` | `CGL_EASE_IN_QUAD` | When being sucked away: wind up first, then fling out |
| `CGL_CFG_GENIE_IN_EASE` | `CGL_EASE_OUT_CUBIC` | When pouring out: brake to a stable stop |
| `CGL_CFG_S_OUT_EASE` / `CGL_CFG_S_IN_EASE` | same as the two Genie directions | The two direction curves of the S-shaped snap. Identical to the vertical suck-in: the difference lies in the geometric path, not in time |
| `CGL_CFG_GENIE_NARROW_Q8` | 64 | Width at the end near the intake when fully sucked in (Q8; 64 = 25%). The larger it is, the more it resembles a horn; the smaller, the more it resembles an awl |
| `CGL_CFG_GENIE_DROP_PX` | 6 | How many pixels below the page's bottom edge the intake lies (placed off-screen so the last frame leaves no residue) |
| `CGL_CFG_S_AMP_MAX_Q8` | 160 | The **upper limit** on the maximum lateral offset of the S-shaped path (Q8), which is also the monotonicity boundary. The mathematical bound is 682 (beyond it adjacent rows cross and the image folds); the default value 160 is exactly the value derived from "half the screen width" for a 320-pixel panel, so the limit and the derived value coincide by construction |
| `CGL_CFG_S_DROP_PX` | `CGL_CFG_GENIE_DROP_PX` | How many pixels below the bottom-right corner the S-shaped intake lies |

**Input and Palette**

| Macro | Default | Effect |
|---|---|---|
| `CGL_CFG_INPUT_QUEUE` | 8 | Input ring queue length, **must be a power of 2** |
| `CGL_CFG_BG_COLOR` / `_FG_COLOR` / `_ACCENT_COLOR` | `CGL_COLOR_DARK` / `CGL_COLOR_CREAM` / `CGL_COLOR_AMBER` | Default colors of the built-in widgets and of transition masks |

### 5.5 Retargeting Recipes

**A Smaller Panel or a Tighter RAM Budget**

1. Set `CGL_CFG_SCREEN_W/H` to the actual panel dimensions and make `drv->width/height` match.
2. When RAM is insufficient, first set `CGL_CFG_ALPHA 0`: this is the least troublesome large reclaim, at the cost of losing only the "see-through group" capability.
3. Then lower `CGL_CFG_BAND_H` to 2, at the cost of more SPI window commands per frame.

**A Larger Panel or a More Ample RAM Budget**

1. Raise `CGL_CFG_BAND_H` to 8 or 16 to reduce window-command overhead.
2. If semi-transparent layers are nested, raise `CGL_CFG_ALPHA_LEVELS`.
3. Raise `CGL_CFG_ALPHA_BUDGET` accordingly, otherwise the build fails by design.

**Memory Accounting**

- Band buffers: `2 × CGL_CFG_SCREEN_W × CGL_CFG_BAND_H × 2` bytes. The default is 5 KB.
- Group opacity scratch: `CGL_CFG_ALPHA_LEVELS × CGL_CFG_ALPHA_LEVEL_STRIDE × 2` bytes. The default is 5 KB.
- Both are held by `cgl_display_t`, and that instance usually exists statically in the porting layer.

---

## 6. Canvas and Software Rasterization

### 6.1 Model

A canvas = one `width × height` horizontal band buffer + `origin_y` (the screen row corresponding to the first row of this band) + a clip window (**screen coordinates**) + `pan` (the translation of this level) + `warp` (an optional **non-linear** deformation) + a layered scratch area (group opacity).

```c
typedef struct {
    cgl_color_t *buf;       /* width x height pixels, row-major        */
    int16_t  width;         /* pixels per row                          */
    int16_t  height;        /* rows in this band                       */
    int16_t  origin_y;      /* screen y of band row 0                  */
    cgl_rect_t clip;        /* clip window, screen coords, closed      */
    cgl_point_t pan;        /* pan of this level; primitive coords get it added before clipping/pixel write */
    const cgl_warp_t *warp; /* non-NULL = this subtree drawn through the suck-in warp */
    cgl_rect_t src_clip;    /* clip window in **source coordinates**; tested only when warp is active */

    /* group opacity */
    cgl_color_t *alpha_scratch;  /* layered scratch with exactly the band buffer's geometry */
    uint32_t alpha_stride;       /* pixels per level (>= width * height)  */
    uint8_t  alpha_levels;       /* number of scratch levels              */
    uint8_t  alpha_used;         /* number of levels currently nested     */

    uint16_t ink_alpha_q8;       /* ink alpha: opacity of blending primitives */
} cgl_canvas_t;
```

Coordinate pipeline:

```
primitive coords --(+pan)--> screen coords --(warp)--> target coords --(clip / row index within band)--> pixel
                                   `-- src_clip culls by source coords **before** warp
```

**There is only one rule**: every public primitive function translates its own coordinates once; internal functions accept screen coordinates only. `cgl_canvas_clear_clip()` uses the clip window itself as the rectangle, so it deliberately takes the internal path without translation.

### 6.2 Initialization and State

| API | Semantics |
|---|---|
| `cgl_canvas_init(c, buf, width, height, origin_y)` | Binds the band buffer. `origin_y` is the screen row number of the band's first row |
| `cgl_canvas_get_clip(c)` / `set_clip(c, clip)` / `reset_clip(c)` | Reads/writes the clip window (screen coordinates) |
| `cgl_canvas_get_pan(c)` / `set_pan(c, pan)` | Reads/writes the pan. The caller is responsible for restoring it in pairs; the widget tree traversal uses save/restore rather than incremental modification |
| `cgl_canvas_set_ink_alpha(c, alpha_q8)` / `get_ink_alpha(c)` | Sets/reads the ink alpha of blending primitives (see 6.6) |
| `cgl_canvas_set_warp(c, w)` / `get_warp(c)` | Installs/removes the warp descriptor (`NULL` disables it) |
| `cgl_canvas_map_y(c, y)` / `map_x(c, x, y)` | Source coordinates → target coordinates; an identity mapping when there is no warp. The values passed must be **screen coordinates that already include pan**, and `map_x` needs the source row index of that pixel |
| `cgl_canvas_map_rect(c, r)` | The **conservative** bounding box of a source rectangle in target space (the rectangle itself when there is no warp). The widget tree uses it for culling |
| `cgl_canvas_set_src_clip(c, clip)` / `get_src_clip(c)` | Source-coordinate clip window, tested only when warp is active |
| `cgl_canvas_src_window(c)` | The window in **source space** that can still land in this band (closed interval, screen coordinates). Painters that scan a shape row by row use it to bound their scan range |
| `cgl_canvas_row(c, y)` | Obtains a row pointer directly. `y` is a **screen row number**, and this function **does not add pan**; the caller must add pan itself and observe clipping |
| `cgl_canvas_set_alpha_scratch(c, scratch, levels, stride_px)` / `alpha_room(c)` | Installs/removes the layered scratch area and queries how many more levels may be nested |
| `cgl_canvas_copy_rect(c, dst, r)` | Copies the pixels of the current buffer inside `r` to `dst`; `dst` must have **identical geometry** to this buffer |
| `cgl_canvas_blend_src_q8(c, r, src, alpha_q8)` | Per-pixel source blending: `dst = dst*(1-a) + src*a`; `src` has the same geometry as this buffer |

### 6.3 Primitives

| Primitive | Description |
|---|---|
| `cgl_canvas_clear` / `clear_clip` | Clears the whole band / clears only the clip window |
| `cgl_canvas_pixel` | Single pixel |
| `cgl_canvas_hline` / `vline` | Horizontal line / vertical line |
| `cgl_canvas_line` | Bresenham line, 1 pixel wide |
| `cgl_canvas_fill_rect` / `rect` | Filled rectangle / rectangle outline |
| `cgl_canvas_disc` | Filled disc (midpoint circle algorithm) |
| `cgl_canvas_dot` | Filled square dot of size 1..3 centered on `(cx, cy)`. Cheaper than `disc`, and it preserves the negative space between adjacent dots, which is exactly what makes a dot-matrix logo readable |
| `cgl_canvas_circle` | 1-pixel-wide **anti-aliased** circle outline, equivalent to "an arc of radius `r` and width 1" (the annulus `r ± 0.5`). A radius `<= 0` draws one anti-aliased pixel: degenerating to a point is a legitimate result, not an error |
| `cgl_canvas_line_thick` | Thick line segment with **round end caps**, anti-aliased. `thickness <= 1` takes the ordinary Bresenham path, pixel-for-pixel identical to `cgl_canvas_line()` |
| `cgl_canvas_arc` | Anti-aliased thick circular arc with round end caps: the base primitive for rings/gauges/radial progress. `radius <= 0` degenerates to an anti-aliased dot of diameter `thickness` |
| `cgl_canvas_pixel_blend_q8` | Blends a single pixel by coverage `cov_q8/256`. The basic building block of anti-aliased strokes, also used by custom rasterizers |
| `cgl_canvas_blit` | Unscaled blit (the caller must subtract pan from the source coordinates) |
| `cgl_canvas_fill_rect_blend_q8` | Q8 alpha blend fill of a rectangle with a **single constant color** |
| `cgl_canvas_fill_rect_blend` | Fixed 50/50 blend fill (filled after intersecting with the clip window) |

Full-ring form of an arc:

```c
cgl_canvas_arc(c, cx, cy, radius, thickness, 0u, CGL_ANGLE_TURN, color);
```

The type of `sweep` is `uint32_t` rather than `cgl_angle_t`, because `cgl_angle_t` wraps at one turn, so a "full turn" (`CGL_ANGLE_TURN`) would be indistinguishable from "zero". A `sweep` larger than one turn is legal and wraps naturally; `sweep == 0` draws nothing, which is exactly the "empty gauge" state every caller needs.

### 6.4 Anti-Aliased Radial Primitives: Four Properties to Know in Advance

Circles, thick lines, and arcs are anti-aliased; rectangles, horizontal lines, discs, and square dots are not. Rectangles and horizontal lines have no jaggies to eliminate; circles, needles, and rings have nothing but jaggies — near a tangent of 45° there are two- or three-pixel steps that read as burrs rather than curves at 250 dpi. The strokes named above therefore carry per-pixel **coverage** (0..256) and blend, whereas `fill_rect` / `line` / `disc` / `dot` stay sharp: they are the hot path for dot-matrix/bar/panel drawing, where one pixel is one pixel.

Coverage is computed in the **squared-distance** domain (two multiplications), linearized with `(r² − d²)/(r + d) == r − d` and refined once near the boundary; there is no `sqrt` anywhere in the rasterizer.

1. **The boundary pixels of a stroke are blended pixels**; whether "this pixel is the stroke color" can be asked only of interior pixels. A 1-pixel-wide anti-aliased stroke has **no** fully covered pixel at all (every pixel it touches lies on the gradient), so a border or thin line that must look solid should be requested at 2 pixels.
2. **The drawn width is exactly `thickness`**, odd or even. Anti-aliasing removed the "odd only" restriction of earlier implementations.
3. **The end caps are round**, so a stroke extends up to `thickness / 2` pixels beyond the length requested for the segment/arc at each end. Callers that must fit strictly inside a box should shorten the geometry themselves.
4. **Cost is proportional to stroke area.** `line_thick` walks along the **major axis** and derives the minor-axis range step by step (so an oblique thin stroke need not pay for the empty corners of its bounding box); `arc` walks screen row by screen row, and the half-width of each row is obtained from `cgl_math_isqrt()`. The arc rasterizes the shape **directly**, not by sampling along the angle — which is also why "increasing sweep does not move the part already drawn" (a mesh sampled by angular steps repositions every stamp when the step changes, and a gauge built that way jitters while sweeping).

### 6.5 Warp

`pan` is **linear**: every primitive is translated by the same displacement. `warp` is its non-linear counterpart: source coordinates (the "resting screen coordinates" that already include pan) map to target coordinates by the following rules.

```
Rows: y' = ty + (y - ty) * (1 - p)                monotonic, step 0 or 1
Cols: x' = cx' + (x - cx) * k(y) / 256 + S(y)     k(y) lies in [0, 256]
```

```c
typedef struct {
    int16_t  src_y0;        /* start of the source row interval (the screen row where the page rests) */
    int16_t  src_h;         /* number of source rows (= page height)  */
    int16_t  cx;            /* source center column: the axis of narrowing */
    int16_t  tx, ty;        /* intake coordinates                     */
    uint16_t progress_q8;   /* 0 = identity; 256 = fully sucked into a point */
    uint16_t narrow_q8;     /* width factor at the far end (near the intake), Q8 */
    uint16_t row_step_q8;   /* normalized step = 256*256/src_h        */
    uint16_t s_amp_q8;      /* max lateral S-path offset, Q8; 0 = no S path */
} cgl_warp_t;
```

Key properties:

- When `p = 0` the entire mapping is an **exact identity** (rows floored, columns rounded, both derivable), so pulling p from 0 to 256 is "from as-is to fully sucked into a point". This is also why no jump is visible in the first frame of a page switch.
- The row mapping step is always 0/1, so **discrete writes leave no holes** (a target row can be computed directly from a source row, with no reverse lookup), and the front-to-back occlusion order is guaranteed to be unchanged by the warp.
- Flooring rows and rounding columns is not an arbitrary choice: the screen has only 172 rows and the intake lies at the screen edge, where `floor(d*(1-eps)) == d`, so the row mapping is an exact identity at small p; if columns were floored as well, a seam would split open in the middle of the image (the right half shifting left by one pixel).
- `src_clip` is the clip window in **source coordinates**, tested only when warp is active. The warp is "deform after drawing", so clipping naturally belongs to source space.
- **Culling must not use the source frame**: the warp is compressive, and when the page is sucked to the other end of the screen the source box and the target box may not overlap at all, so culling with the source box would draw only the top few rows of the whole page. Use the conservative bounding box given by `cgl_canvas_map_rect()`.
- **`CGL_WF_CLIP` must stay in source space**: inside a warped subtree it modifies `src_clip` rather than the target `clip`.
- **Row-by-row scanning must be bound to the preimage of the band rather than to the band itself**: a primitive is drawn once per band, and its row scan must be clipped to the rows that can actually land in that band. `cgl_canvas_arc()` / `line_thick()` / `disc()` all do this, and widgets with their own rasterizers should handle it the same way through `cgl_canvas_src_window()`. With warp active that window is **far taller** than the band, which is exactly why the function exists: without it, every shape would walk all 43 bands in full and then apply the per-pixel warp mapping on top, and the frame rate would collapse.
- `hit_test` does not perform an inverse lookup of the warp: the warp exists only **during a transition**, and input is locked during a transition (see 13.8), so `warp` is always `NULL` when hit testing occurs.

`cgl_warp_t` is owned by the caller; widgets store only a pointer. During a transition it is owned by the navigator.

### 6.6 Ink Alpha

```c
void     cgl_canvas_set_ink_alpha(cgl_canvas_t *c, uint16_t alpha_q8);  /* 0..256 */
uint16_t cgl_canvas_get_ink_alpha(const cgl_canvas_t *c);
#define  CGL_CANVAS_INK_OPAQUE 256u
```

It makes a **single primitive** semi-transparent without any additional memory, and is the way "draw this ring at 40%" is implemented. It takes effect by scaling the coverage the primitive has already computed, so edges remain anti-aliased and the result is still the framework's only blending formula rather than a second one.

Which primitives honor it: all paths that pass through `canvas__blend()` / `canvas__store()`, namely `cgl_canvas_arc()`, `circle()`, `disc()`, `line_thick()` (thickness > 1), and `pixel_blend_q8()`. The **sharp** fast paths (`fill_rect`, `line`, `hline`, `vline`, `dot`, `clear`, and 1-pixel-wide `line_thick`) do not read it: they are byte writes, and "draw a semi-transparent filled rectangle" already has its own door — `cgl_canvas_fill_rect_blend_q8()`.

**It is state, not a parameter, and it is not saved and restored automatically.** Set it, draw, then set it back to `CGL_CANVAS_INK_OPAQUE`; the `cgl_arc_draw_*()` wrapper functions in `cgl_arc.h` do exactly this around one call. Values above 256 are clamped.

### 6.7 The Canvas-Side Group Opacity Interface

The canvas provides only the mechanism; the policy belongs to the widget layer (see 9.3). Four calls: `set_alpha_scratch()` installs the layered buffer, `alpha_room()` queries the remaining levels, `copy_rect()` copies the background, and `blend_src_q8()` composites it back by alpha. The per-band compositing flow:

```
1) Copy the region of this band covered by that widget into the scratch area (= a copy of the background)
2) Render the entire subtree **as usual** into that scratch area
   -- the implementation merely points c->buf at the scratch area temporarily: the band
      geometry is identical, so pan / warp / clip / src_clip all remain valid as-is, **even
      raw-pointer users of cgl_canvas_row() (fonts, custom widgets) follow automatically**. Zero changes to primitive code.
3) Blend the scratch area back into the band buffer by alpha: band = band*(1-a) + scratch*a
4) Restore c->buf
```

Two properties worth remembering:

- **No coverage mask is needed.** Step 3 is an identity transform for "pixels that were not drawn" (the scratch area there still equals the background, `band*(1-a) + band*a == band`). This is why it uses less RAM than "save the background first, then blend back through a mask".
- **Nesting is exact.** Rendering a group onto "a copy of the background" and blending it back by alpha is mathematically equivalent to "rendering the group onto a transparent layer and then compositing" (provable by induction), so two nested levels simply apply their respective alphas level by level, with no drift.

`alpha[]` together with `band[]` in `cgl_display_t` constitutes the entire memory for group opacity; when `CGL_CFG_ALPHA = 0` that member does not exist at all.

### 6.8 Pitfalls

- `cgl_canvas_row()` **does not add pan**. It returns a raw buffer pointer, so fast paths that use it directly (such as font code) must add pan themselves, otherwise "the rectangle moves with the subtree while the text stays where it was".
- `clip` is always in **screen coordinates** (excluding pan); pan applies only to the coordinates passed to primitives. Therefore "feeding clip to fill_rect again" translates twice.
- Primitive coordinates are "the coordinates of this level"; `pan` is accumulated level by level by the traverser, and a widget itself does not need to know the translation of its ancestors.

---

## 7. Integer Mathematics

This module is a **base module**: it depends only on `<stdint.h>`, so the canvas may include it downward, while the canvas itself still must not depend upward on `cgl_anim` / `cgl_widget`. Its reason for existing is that radial drawing requires an "angle to point" mapping, and the framework rejects libm: `sinf()` / `cosf()` drag in soft-float/64-bit helper functions along with them, while the "draw a circle without trigonometry" trick (`x*x + y*y <= r*r`) can only draw a whole circle and cannot answer "where is 10:09 on the watch".

The implementation is a quarter-wave table plus linear interpolation: 65 `int16` table entries (130 bytes of flash, no RAM, no libm).

### 7.1 Angle Conventions

```c
typedef uint16_t cgl_angle_t;     /* 1/65536 of a turn */
#define CGL_ANGLE_TURN     65536u            /* note: unsigned int, not cgl_angle_t */
#define CGL_ANGLE_QUARTER  ((cgl_angle_t)16384u)   /* 6 o'clock */
#define CGL_ANGLE_HALF     ((cgl_angle_t)32768u)   /* 9 o'clock */
#define CGL_ANGLE_UP       ((cgl_angle_t)49152u)   /* 12 o'clock */
#define CGL_ANGLE_DEG(d)   ((cgl_angle_t)(((uint32_t)(d) * 65536u) / 360u))
#define CGL_ANGLE_FROM_U8(a) ((cgl_angle_t)((uint16_t)(a) << 8))
#define CGL_ANGLE_TO_U8(a)   ((uint8_t)((uint16_t)(a) >> 8))
```

The convention is screen space with the y axis pointing down:

```
angle 0       -> +x (3 o'clock)
increasing angle -> toward +y, i.e. **clockwise** on screen
CGL_ANGLE_UP  -> -y (12 o'clock)        x = cx + r*cos(a), y = cy + r*sin(a)
```

- **16 bits rather than 8.** The optional extension module uses 8-bit angles (sufficient for a camera matrix), but that is insufficient for any quantity whose motion the user stares at: 8 bits step by 1.41°, which at a radius of 70 pixels moves the tip of a hand by 1.7 pixels, and the second hand visibly jumps 3 times per second instead of sweeping. 16 bits make the step 0.007 pixels, with no need to interpolate the position. Convert at the boundary between the two worlds with `CGL_ANGLE_FROM_U8()` / `CGL_ANGLE_TO_U8()`.
- **Q8 rather than Q15.** This is consistent with the rest of the framework (tween curves, alpha, the `progress_q8` of a warp), and the error already lies below the pixel grid: 0.5 out of a full scale of 256 is 0.2 pixels at a radius of 100 pixels. Q15 would only force every consumer to write `(int32_t)r * v >> 15`.
- **`cgl_angle_t` wraps at one turn.** That is precisely its meaning as a "fraction of a turn" rather than a length, and the consequence is that a "full turn" cannot be written as a `cgl_angle_t` (it would equal 0), so the `sweep` of an arc primitive uses the wider `uint32_t`. `CGL_ANGLE_TURN` is deliberately an `unsigned int` and should be used in wider arithmetic and then cast down.

### 7.2 API

| API | Semantics |
|---|---|
| `int16_t cgl_sin_q8(cgl_angle_t a)` | Sine, range −256..256, where 256 represents 1.0 exactly |
| `int16_t cgl_cos_q8(cgl_angle_t a)` | Cosine (the same table, advanced by a quarter turn) |
| `void cgl_math_polar(cgl_angle_t a, int16_t radius, int16_t *dx, int16_t *dy)` | Angle + radius → whole-pixel offset from the center: `dx = round(r*cos/256)`, `dy = round(r*sin/256)`. `radius` may be 0..32767, and either output may be `NULL` |
| `void cgl_math_polar_q6(int16_t cx, int16_t cy, cgl_angle_t a, int16_t radius, int32_t *x_q6, int32_t *y_q6)` | The **Q6** (1/64 pixel) form of the same mapping, about an arbitrary center. `x_q6 = cx<<6 + (r*cos)>>2` |
| `uint16_t cgl_math_frac_q8(int32_t value, int32_t vmin, int32_t vmax)` | The Q8 representation of `(value − vmin)/(vmax − vmin)` (0..256, rounded). `vmax <= vmin` is treated as a rejected range and draws an empty value rather than dividing by zero |
| `uint32_t cgl_math_isqrt(uint32_t v)` | `floor(sqrt(v))`, with no libm and no floating point |
| `uint16_t cgl_angle_deg(cgl_angle_t a)` | The whole-degree reading of an angle, 0..359. This is the **mathematical** reading (0 = +x = 3 o'clock), intended for display and tests, not for drawing |

Three implementation-level points:

- **`cgl_math_polar()` rounds to nearest, and this point is load-bearing**: a truncating `>> 8` biases each axis independently toward the origin, so a point that should lie at radius r falls up to 1.4 pixels inside r. The deviations do not cancel each other out — a ring drawn from those points is noticeably smaller than its nominal radius.
- **The usage of `cgl_math_isqrt()`**: a Q16 input yields a Q8 output, because `sqrt(x*65536) == sqrt(x)*256`. Thus `cgl_math_isqrt(r_q8*r_q8 - dy_q8*dy_q8)` directly gives the half-width of one row of a circle in Q8, with no scaling arithmetic at the call site — `cgl_canvas_arc()` walks row by row in exactly this way. The algorithm is bitwise (restoring remainder), with at most 16 shifts/comparisons/subtractions, so its cost has a determinate upper bound, rather than Newton iteration whose iteration count depends on the starting value.
- **`cgl_math_frac_q8()` is the only implementation of "value to progress"**, shared by `cgl_bar` (linear fill) and `cgl_arc_gauge` (angular sweep), so the two cannot give different answers to "where is the halfway point". Internally it uses unsigned 32-bit throughout: `vmax − vmin` is not a valid `int32_t` over `INT32_MIN..INT32_MAX`, but it is a perfectly valid `uint32_t`. The fractional bits are obtained by a shift/subtract long division rather than `(off << 8) / span` — the latter overflows when the range is wider than 2²³, and `int64` is forbidden in this project (it would pull in `__aeabi_ldivmod` from libgcc).

---

## 8. Fonts and Text

### 8.1 Glyph Storage Format

The bitmap layout is **row-major**, one byte represents 8 horizontal pixels of the same row, and **bit0 is the leftmost pixel**:

```
byte index = row * bytes_per_row + (column / 8)
bit  index = column % 8
```

This is the layout of the classic vendor ASCII tables, so any glyph table in that format can be used by filling in a `cgl_font_t`.

```c
typedef struct {
    uint8_t width;        /* glyph cell width (pixels)         */
    uint8_t height;       /* glyph cell height (pixels)        */
    uint8_t bytes;        /* bytes per glyph = bytes_per_row * height */
    const uint8_t *data;  /* glyph table, in ASCII order starting at first */
    uint8_t first;        /* first encoded character           */
    uint8_t last;         /* last encoded character            */
} cgl_font_t;
```

### 8.2 API

| API | Semantics |
|---|---|
| `extern const cgl_font_t cgl_font_8x16` | 8×16 ASCII font, printable range `0x20..0x7E` (default) |
| `extern const cgl_font_t cgl_font_12x24` | 12×24 ASCII font, legible at 250 dpi |
| `int16_t cgl_font_text_width(const cgl_font_t *font, const char *text)` | Pixel width of a string; `NULL`/empty string returns 0. For a monospaced font this is "character count × glyph width" |
| `int16_t cgl_font_draw_text(c, font, x, y, text, fg)` | Draws with a transparent background and returns the advance width |
| `int16_t cgl_font_draw_text_bg(c, font, x, y, text, fg, bg)` | Fills the glyph cells with the background color (opaque) and returns the advance width |

Text drawing writes `cgl_canvas_row()` directly, row by row, so it **applies the pan itself**. The symptom of omitting this step is that "in a panned subtree the rectangles move with it while the text does not". Both fonts are monospaced bitmaps, and non-ASCII characters (`< 0x20` or `> 0x7E`) are not in the table.

For the tools that generate a new font size or a new font, see Section 18.6.

---

## 9. The Widget Tree

### 9.1 Structure

```c
struct cgl_widget
{
    const cgl_widget_vt_t *vt;   /* type / draw / update / event */
    const char *name;            /* for debugging */
    cgl_rect_t frame;            /* absolute screen rectangle */
    cgl_point_t offset;          /* pan of this subtree (transition/scroll/shake) */
    const cgl_warp_t *warp;      /* non-linear warp of this subtree (Genie suck-in) */
    cgl_widget_t *parent, *first_child, *last_child, *next;
    uint16_t flags;              /* CGL_WF_VISIBLE | CGL_WF_CLIP */
    uint8_t alpha_q8;            /* group opacity; 255 = opaque (default) */
    void *user;
};
```

`alpha_q8` is deliberately placed after `flags`: that position is alignment padding anyway, so this field **occupies no extra bytes** (the total size of the struct remains 48 B).

| Flag | Meaning |
|---|---|
| `CGL_WF_VISIBLE` | Participates in drawing, updating, and hit testing (once cleared, the entire subtree is skipped) |
| `CGL_WF_CLIP` | Child widgets are clipped to this widget's frame (`cgl_widget_init()` sets it by default). Drawing and hit testing **both** honor it: the part of a descendant drawn outside an ancestor's clip window is not hit there either. A widget without this flag may draw outside its frame, and its children remain reachable there |

Bit `0x0004` was once used for `CGL_WF_OPAQUE` ("this widget fills its own frame"). It was set by `cgl_widget_init()` but **read by no code**, so it has been removed: code that still references that name fails to compile outright rather than yielding a silently wrong result. The opacity switch is `alpha_q8` / `cgl_widget_set_alpha()`, whose semantics are **group** opacity.

```c
typedef struct
{
    const char *type;
    void (*draw)(cgl_widget_t *w, cgl_canvas_t *c);
    void (*update)(cgl_widget_t *w, uint32_t dt_ms);
    bool (*event)(cgl_widget_t *w, const cgl_event_t *ev);   /* optional, NULL = not handled */
} cgl_widget_vt_t;
```

An `event` returning `true` means "I have consumed it" and traversal stops; returning `false` means propagation continues. The vtable must be written with designated initializers.

### 9.2 Construction and Traversal

| API | Semantics |
|---|---|
| `cgl_widget_init(w, vt, name, frame)` | Initializes: sets `CGL_WF_VISIBLE \| CGL_WF_CLIP` by default, with `alpha_q8 = CGL_ALPHA_OPAQUE` |
| `bool cgl_widget_add_child(parent, child)` | Appends `child` to the end of the child list, i.e. draws it above earlier siblings. **The return value must be checked**: it returns `false` when an argument is `NULL`, when the child already has a parent (call `remove_child()` first; silently changing the parent corrupts the old list), or when the parent already has `CGL_CFG_WIDGET_MAX_CHILDREN` children. Every rejection is counted in `cgl_widget_children_dropped()` |
| `cgl_widget_remove_child(child)` | Unlinks only, does not destroy |
| `cgl_widget_set_frame(w, frame)` | Sets the absolute screen rectangle and marks damage |
| `cgl_widget_set_visible(w, visible)` | Visibility (once cleared, the entire subtree is not drawn, not updated, not hit) |
| `cgl_widget_set_offset(w, dx, dy)` | Pan of this subtree; marks both the "before move" and the "after move" regions as damaged |
| `cgl_widget_set_warp(w, warp)` | Warp of this subtree; pass `NULL` to restore. The descriptor is held by the caller, and the widget stores only a pointer |
| `cgl_widget_raise(w)` | Moves to the end of the siblings (drawn topmost) |
| `cgl_widget_draw_tree(w, c)` | Pre-order traversal: parent before children, siblings in insertion order |
| `cgl_widget_update_tree(w, dt_ms)` | Advances the animations of this subtree; reaches only visible subtrees |
| `cgl_widget_hit_test(w, x, y)` | The deepest visible widget that is hit; among siblings the **last** one hit wins |
| `cgl_widget_dispatch_event(w, ev)` | Depth-first (parent before children); the first widget to return `true` consumes the event |

**Drawing order is child list order**: widgets added later draw on top, and `cgl_widget_raise()` moves a widget to the topmost layer.

`cgl_widget_hit_test()` has two cases that it deliberately does not answer:

- **Clipping is honored.** A widget with `CGL_WF_CLIP` constrains its children to its own frame, so a descendant drawn outside an ancestor's clip window is not hit there either. A widget without this flag may draw outside its frame, and its children remain reachable there — this is a defined distinction between the two, not an oversight.
- **A warped subtree is never hit.** A warp moves pixels elsewhere, and hit testing has no inverse mapping for it; reporting a hit from the unwarped frame amounts to hitting pixels that do not exist on screen. During a transition input is locked anyway (see 13.8), so for now this is a safety net rather than dependable behavior.

### 9.3 Three Subtree-Level Mechanisms

**Pan (`offset`)**: the traverser adds `offset` to the canvas `pan` on entering a widget and restores it on leaving. Therefore "sliding a whole page out" requires only setting an offset on the page's root widget, without changing the frame of any child widget and without rebuilding the tree, and culling and hit testing are automatically correct. Note that `offset` applies to "this widget and its entire subtree", not only to its children; a page root widget is usually full-screen, so its own background moves with it when it slides, which is the expected behavior.

**Warp (`warp`)**: likewise applies to the entire subtree. It is installed into the canvas on entering this level and restored on leaving. Both places must be changed together, or errors will result (see 6.5 for details).

**Group opacity (`alpha_q8`)**:

```c
cgl_widget_set_alpha(&panel.base, 160u);   /* 0..255, 255 = opaque (default) */
```

The semantics are **group** opacity: the entire widget (its own `draw` plus the whole subtree) is first rendered in isolation and then blended onto the background as one unit. Therefore "a panel plus two translucent highlights inside it that overlap each other" is not blended twice at the overlap — this is precisely how it differs from "multiplying each primitive by alpha separately".

The isolation region is "where this level **may** deposit pixels": when a clipping constraint is present (the `CGL_WF_CLIP` that is on by default, or an active warp), it is the conservative bounding box of the frame and the clip window (tight); a widget with **`CGL_WF_CLIP` cleared** is allowed to draw outside its frame, and in that case only the whole clip window can be taken conservatively — the result is correct but more expensive. To save cost, retain `CGL_WF_CLIP`.

| Case | Behavior |
|---|---|
| `alpha = CGL_ALPHA_OPAQUE` (default) | The whole section is skipped, at the cost of one branch test (a UI that does not use this API pays no time cost) |
| `alpha = 0` | The entire subtree draws not a single pixel and does not touch the staging area |
| The canvas has no staging area (`CGL_CFG_ALPHA = 0`) | alpha is **ignored** (drawn as opaque) and counted in `cgl_widget_alpha_dropped()` |
| Nesting exceeds `CGL_CFG_ALPHA_LEVELS` | Same as above: ignored and counted, no crash, no corruption of the staging area |

Two warnings:

- **It is not inherited.** Setting alpha on a parent widget and then setting alpha on a child widget yields **two** mutually independent groups (each occupying one staging level), not the product of the two.
- **`alpha = 0` only means "not drawn"**, and does not affect `CGL_WF_VISIBLE`, hit testing, or event dispatch: a fully transparent widget is still hit. To remove it entirely, use `cgl_widget_set_visible(w, false)`.

`cgl_widget_set_alpha()` marks damage only for `w->frame`. If the widget deliberately draws outside its frame (an overlay with `CGL_WF_CLIP` cleared), it must mark damage once more itself after changing alpha.

### 9.4 The Damage Mechanism

The framework has only **one** global damage sink: `cgl_widget_set_damage_sink(fn, user)` installs it (`cgl_screen_init()` installs `cgl_display_damage`). A widget need not know whether it is on screen, in an off-screen buffer, or absent altogether.

```c
void cgl_widget_invalidate(const cgl_widget_t *w);        /* reports w->frame */
void cgl_widget_invalidate_rect(cgl_rect_t r);            /* reports an arbitrary rectangle */
```

**What is reported is the untranslated frame** (a widget does not know its ancestors' offset). Therefore in a custom offset animation the caller must itself mark damage for the union of the old and new positions; `cgl_widget_set_offset()` already does this on the user's behalf. During a transition the navigator dirties the whole screen every frame, so the framework itself has no problem.

Damage accumulates as a **rectangle union**, not as a region list. `cgl_display_present()` redraws every complete band that the damage rectangles cover.

### 9.5 Three Observable Counters

Compared with a failure such as "the effect did not appear", which can only be guessed at by eye, the framework turns three kinds of "the setting did not take effect" into numbers:

| Counter | Meaning |
|---|---|
| `cgl_widget_alpha_dropped()` | The cumulative count of "alpha was set but did not take effect" (no staging area / nesting levels exhausted). **The unit is "per band × per traversal"**: the widget tree is redrawn once per band, so a widget spanning 10 bands increments it by 10 rather than 1. It answers "was alpha dropped", not "how many widgets were dropped" |
| `cgl_widget_children_dropped()` | The number of times `cgl_widget_add_child()` was rejected because the parent already had `CGL_CFG_WIDGET_MAX_CHILDREN` children |
| `cgl_bar_range_rejected()` / `cgl_arc_range_rejected()` | The number of range settings rejected because `vmax <= vmin` |

Under a normal configuration all three should be 0.

### 9.6 Writing Your Own Widget

Adding a new widget requires satisfying the following rules:

1. The **first member of the struct must be** `cgl_widget_t base`.
2. Write a `cgl_widget_vt_t` vtable using designated initializers (`.type = "mywidget", .draw = ..., ...`).
3. In `draw` all coordinates are screen coordinates, and the canvas automatically clips to the current band; the widget **must fill its own frame itself**, or contents from the previous frame remain. A widget that draws only "the changed part" therefore has no state from which to restore — `cgl_bar` fills the entire frame with the track color for exactly this reason.
4. In `init`, call `cgl_widget_init(&x->base, &vt, "name", frame)`; when clipping is required, add `x->base.flags |= CGL_WF_CLIP`.
5. In a setter, call `cgl_widget_invalidate(&x->base)` **only when the drawn pixels would actually change**: compare the old and new values first and return early if they are equal. Configuration setters (`set_colors`, `set_align`, etc.) must compare as well: a UI that reapplies its theme every frame should not redraw every frame.
6. Attach the widget to the tree: `cgl_widget_add_child(root, &x->base)`, and check the return value. Construction of a variable number of child widgets should pay attention to it, and `cgl_widget_children_dropped()` counts every rejection.
7. Implement `update(w, dt_ms)` only when continuous animation is present. **Do not put physics/logic here** (that belongs in the page's `on_step`). If a parameter could be set to an impossible value, reject it **and count it**, so that "the effect did not appear" becomes a number rather than a guess.

---

## 10. Built-In Widgets

### 10.1 Panel `cgl_panel`

```c
typedef struct {
    cgl_widget_t base;
    cgl_color_t bg;
    cgl_color_t border;
    uint8_t border_w;
} cgl_panel_t;

void cgl_panel_init(cgl_panel_t *p, const char *name, cgl_rect_t frame,
                    cgl_color_t bg, cgl_color_t border);
void cgl_panel_set_colors(cgl_panel_t *p, cgl_color_t bg, cgl_color_t border);
```

The simplest container: fill the frame with `bg`, then draw a border of `border_w`. When `bg == border` the border is automatically not drawn. The panel is the most direct embodiment of the rule that a widget must fill its own frame.

### 10.2 Text Label `cgl_label`

```c
typedef enum { CGL_ALIGN_LEFT = 0, CGL_ALIGN_CENTER = 1, CGL_ALIGN_RIGHT = 2 } cgl_align_t;

typedef struct {
    cgl_widget_t base;
    const cgl_font_t *font;
    cgl_color_t fg, bg;
    uint8_t opaque;
    uint8_t align;
    char text[CGL_CFG_LABEL_MAX_TEXT];
} cgl_label_t;
```

| API | Semantics |
|---|---|
| `cgl_label_init(l, name, frame, font, text, fg, bg, opaque)` | When `opaque` is true, fill the glyph cells with `bg` |
| `cgl_label_set_text(l, text)` | Replace the text. Redraws **only when the content actually changes** |
| `cgl_label_set_int(l, prefix, value, suffix)` | Replace with "prefix + decimal integer + suffix"; `printf` is not required |
| `cgl_label_set_fixed1(l, prefix, value_x10, suffix)` | Replace with "prefix + value + '.' + one decimal digit + suffix"; `value_x10` is the value scaled by ten |
| `cgl_label_set_align(l, align)` | Left/center/right alignment |
| `cgl_label_set_colors(l, fg, bg, opaque)` | Change the colors and the opaque background |

The label holds a fixed-length buffer (`CGL_CFG_LABEL_MAX_TEXT`, including the terminating `\0`), so no heap and no `printf` are required anywhere in the framework. All readouts — frame rate, uptime, counters — are written out through `set_int` / `set_fixed1`, a decimal conversion implemented inside the project to avoid the Flash cost of libc/printf. The text is vertically centered within the frame.

### 10.3 Progress/Level Bar `cgl_bar`

```c
typedef enum { CGL_BAR_HORIZONTAL = 0, CGL_BAR_VERTICAL = 1 } cgl_bar_orient_t;

typedef struct {
    cgl_widget_t base;
    cgl_color_t track, fill, border, seg;
    cgl_anim_t  anim;        /* tween of the drawn value */
    int32_t value;           /* logical (requested) value, always within [vmin, vmax] */
    int32_t vmin, vmax;
    uint8_t orient, border_w, segments, reverse;
} cgl_bar_t;
```

Geometry relations:

```
frame            filled with track
 +-- inner       frame inset by border_w; the fill lies here
 |    +-- fill   fill, grows along the orientation axis
 +-- ring        border, drawn last, so it is never covered
```

`border_w` also serves as the bar's inner padding: there is no second "inset" parameter, because the two would always be set to the same number.

| API | Semantics |
|---|---|
| `cgl_bar_init(b, name, frame, track, fill)` | Range 0..100, value 0, horizontal, no border, no segments. `track` is also the widget background |
| `bool cgl_bar_set_range(b, vmin, vmax)` | Any `int32_t` range. `vmax <= vmin` is **rejected** (returns false, counted in `cgl_bar_range_rejected()`, the old range is kept). A range change always marks damage, because the same value lands elsewhere |
| `cgl_bar_set_value(b, value)` | Set and draw immediately (a running tween is snapped directly to the new value), clamped to the range. Redraws only when the drawn pixels would really change |
| `cgl_bar_set_percent(b, percent)` | 0..100 mapped onto the range; under the default 0..100 range it is equivalent to `set_value` |
| `bool cgl_bar_animate_to(b, value, dur_ms, ease)` | Move to `value` within `dur_ms` using `b->anim`. The tween starts from the **current drawn value**, so changing the target midway remains smooth. `dur_ms == 0` degenerates to `set_value()` and returns false. The widget advances itself in its own `update`; the caller only has to keep `cgl_widget_update_tree()` being called |
| `cgl_bar_set_orient(b, orient)` / `cgl_bar_set_reverse(b, reverse)` | The vertical orientation fills from the bottom edge upward by default; `reverse` flips it to right-to-left / top-to-bottom. Redraws only when something really changes |
| `cgl_bar_set_colors(b, track, fill)` | Replace the track color and the fill color |
| `cgl_bar_set_border(b, color, width)` | Draw a border ring of `width` pixels; `width == 0` removes the border. `width` is clamped so that at least one pixel remains between the two sides of the ring, so bars thinner than 3 pixels have no border |
| `cgl_bar_set_segments(b, segments, color)` | Split the bar into `segments` equal parts and draw `segments - 1` separators between them. The separators span the **entire** inner rectangle, covering fill and track, so the picture reads as "N equal parts, of which the first k are filled" (battery/signal-strength look). Using the **track color** as the separator color switches to another reading: "the fill is cut into blocks" |
| `cgl_bar_get_value` / `_get_shown_value` / `_get_percent` / `_is_animating` | Logical value / current drawn value (differs only during a tween) / 0..100 / whether a tween is running |

Two implementation facts about the "value → pixel" mapping:

- It uses shift/subtract long division rather than `(off << 8) / span`: the latter overflows when the range is wider than 2²³, and `int64` is unavailable in this project. The complete `int32_t` domain is covered, including the case where `vmax - vmin` cannot be represented in `int32_t`.
- The fractional part is **rounded to nearest** (9 iterations rather than 8), so half of a 5-pixel bar is 3 pixels rather than 2. This is invisible on a 300-pixel bar and very noticeable on short bars.

### 10.4 Radial Toolbox `cgl_arc`

This module provides three things that share "a point on a circle": a coverage ramp, a tapered anti-aliased blade (needle), a shaded radial surface (watch/well), and a ring gauge widget. It depends on `cgl_canvas` / `cgl_math` / `cgl_widget` / `cgl_anim` and allocates no memory: all tables are supplied by the caller, static or on the stack. Every part is a pure function of its inputs; no state is kept between frames.

**Coverage**

```c
#define CGL_ARC_COV_N 128
uint16_t cgl_arc_cov_q6(int32_t d_q6);
```

`d_q6` is the **signed distance** of the pixel inside the shape (Q6 pixels; positive inside, negative outside, 0 exactly on the boundary), and returns a Q8 coverage (0..256). It is obtained from a 128-entry Flash table by one-pixel box filtering, the **same** ramp that the framework's own anti-aliased strokes use.

**Tapered Blades and Needles**

```c
typedef struct {
    int32_t bx_q6, by_q6;   /* base end (hub side) */
    int32_t ax_q6, ay_q6;   /* axis line, base end -> needle tip */
    int32_t ux_q16, uy_q16; /* unit vector of the axis direction, Q16 */
    int32_t len_q6;         /* length along the axis, Q6 */
    int32_t wb_q6, wt_q6;   /* base-end/tip half-width, Q6 */
    int32_t dw_q12;         /* (wt - wb) / length, Q12 */
} cgl_arc_stroke_t;

bool cgl_arc_stroke_setup(cgl_arc_stroke_t *g, int32_t bx_q6, int32_t by_q6,
                          int32_t tx_q6, int32_t ty_q6, int16_t w_base, int16_t w_tip);
void cgl_arc_stroke_draw(cgl_canvas_t *c, const cgl_arc_stroke_t *g, cgl_color_t col,
                         uint16_t alpha_q8, int16_t ox, int16_t oy);
void cgl_arc_needle(cgl_canvas_t *c, int16_t cx, int16_t cy, cgl_angle_t a,
                    int16_t r_base, int16_t r_tip, int16_t w_base, int16_t w_tip,
                    cgl_color_t col, cgl_color_t shadow_col, uint16_t shadow_alpha,
                    int16_t shadow_off, cgl_point_t *tip);
cgl_rect_t cgl_arc_needle_damage(cgl_rect_t dmg, int16_t cx, int16_t cy, cgl_angle_t a,
                                 int16_t r_base, int16_t r_tip, int16_t half_w,
                                 int16_t shadow_off);
```

- `cgl_arc_stroke_setup()` computes the normalized axis and the taper rate from the two endpoints (Q6) and two **full widths** (whole pixels). A blade of zero length (base end equal to the tip) returns `false`; in that case `g` is left untouched and the caller must not draw with it.
- `cgl_arc_stroke_draw()` rasterizes the tapered quad translated by `(ox, oy)` and blends by `alpha_q8`; the translation lets the caller lay down a shadow layer first without rebuilding the geometry. It derives the half-width per pixel from two dot products and **does no stamping at all**, so there are no seams. The extent it walks is clamped to `cgl_canvas_src_window()` — this is not an optimization but the precondition that makes the blade affordable (the widget draws once per band, and walking the full bounding box would mean paying for one image 43 times).
- `cgl_arc_needle()` draws a complete needle: shadow first, then ink. `r_base` is the distance by which the blade extends back **past the center** (hidden by the hub), `r_tip` is the outward distance; both widths are full widths. The shadow is a **blend**, not a second dark needle; when `shadow_alpha == 0` no shadow is drawn. `tip` is an optional output: the whole-pixel coordinates of the needle tip, for use by the damage model.
- `cgl_arc_needle_damage()` returns the union of the "bounding box covered by a needle of the given shape" and the `dmg` passed in, including the tip and the tail end, the stroke half-width, the anti-aliasing ramp, and the shadow offset. **Forgetting to account for `shadow_off`** leaves a two-pixel shadow ghost behind after the needle moves away — exactly the defect the watch "no ghosting" test is meant to catch.

**Alpha Wrapper Functions**

```c
void cgl_arc_draw_arc(c, cx, cy, radius, thickness, start, sweep, color, alpha_q8);
void cgl_arc_draw_ring(c, cx, cy, radius, thickness, color, alpha_q8);
void cgl_arc_draw_circle(c, cx, cy, radius, color, alpha_q8);
void cgl_arc_draw_disc(c, cx, cy, radius, color, alpha_q8);
void cgl_arc_draw_line(c, x0, y0, x1, y1, thickness, color, alpha_q8);
```

They set and restore the ink alpha around a single call (alpha is canvas **state**; failing to restore it would fade every later element). `alpha_q8` ranges from 0 (invisible) to 256 (`CGL_CANVAS_INK_OPAQUE`, fully opaque); values above 256 are clamped, and a value of 256 takes a zero-cost path. Edges remain anti-aliased at any alpha, because what is scaled is the coverage and not the geometry: `cgl_arc_draw_ring(..., 128)` is the same ring at half opacity, not a thinner ring.

Two notes:

- None of these functions **draws a background**. Drawing a ring at 40% over existing content is exactly their purpose; drawing over "empty" yields 40% ink over whatever the band happened to contain at that moment.
- `cgl_arc_draw_disc()` is **deliberately not** `cgl_canvas_disc()`: the latter is a block primitive that fills whole rows with no coverage ramp, and therefore cannot be translucent at all; a translucent disc is the degenerate case of the arc primitive with a radius of zero. When `thickness <= 1`, `cgl_arc_draw_line()` takes the ordinary Bresenham path (byte writes) and is therefore **not** translucent.

**Shaded Radial Surfaces**

```c
typedef enum { CGL_ARC_FACE = 0, CGL_ARC_RING = 1 } cgl_arc_surface_kind_t;
#define CGL_ARC_FACE_LUT_N 256
#define CGL_ARC_RING_LUT_N 33
```

`cgl_arc_surface_t` describes "one color per pixel, taken from a table". `FACE` is a disc (a radial gradient lit from one side, plus per-pixel radial streaks and a highlight), `RING` is a ring cross-section (a half sine from dark at the top to bright at the bottom, plus a tilted wall). Main fields: `kind`, `cx`/`cy` (**primitive coordinates**), `r_in`/`r_out`, `c_hi`/`c_mid`/`c_lo`, `knee_q8` (where the FACE gradient flips), `tex`/`tex_amp`/`tex_density` (brushed texture), `ray_amp`/`ray_min` (light rays), `spec`/`light_dx`/`light_dy`/`light_r`/`light_alpha` (highlight), `tilt_dark`/`tilt_light`/`tilt` (tilted wall), `lut`/`lut_n`.

| API | Semantics |
|---|---|
| `cgl_arc_surface_default_face(s)` / `_default_ring(s)` | Fill in the parameters of the reference watch dial/well (the radii are left for the caller to set, `lut` is set to `NULL`) |
| `uint16_t cgl_arc_surface_lut_entries(const cgl_arc_surface_t *s)` | The number of table entries for that surface: `lut_n` when it is non-zero, otherwise the default for that kind |
| `void cgl_arc_surface_bake(const cgl_arc_surface_t *s, uint16_t *lut, uint16_t n)` | Bake `n` entries into `lut`. It **stores nothing in `s`**; the caller assigns `s.lut = table; s.lut_n = n;`, which leaves the ownership question of "Flash or RAM, and who releases it" with the caller. Entries are colored for the **real** radius they represent (`sqrt(i/(n-1))`), that is, one square root per **entry** rather than per pixel |
| `cgl_color_t cgl_arc_surface_color(const cgl_arc_surface_t *s, int16_t dx, int16_t dy)` | The surface color at an offset from the center: the sum of the gradient, the texture, the radial streaks, and the highlight. A **pure function** that reads and writes no state and may be called before any drawing (and can therefore serve as a test reference) |
| `void cgl_arc_surface_paint(cgl_canvas_t *c, const cgl_arc_surface_t *s)` | Paint the surface into the current band (surface ∩ clip window ∩ band). The per-row span is obtained from one `isqrt`; interior pixels are written opaquely. **Not supported while a warp is active**: it then draws as a flat disc (FACE) or a flat ring (RING) using the surface's edge color — a gradient nobody can see is not worth the frame rate |

The table is indexed by `r²` rather than by `r`, because the inner loop knows only `dx*dx + dy*dy`; entries are built on the radius they really represent, so the curve that is drawn is the curve that was designed. `lut == NULL` is legal: it falls back to per-pixel procedural shading at roughly three times the cost of the table path, but with zero Flash footprint.

**Ring Gauge `cgl_arc_gauge`**

The gauge is the radial twin of `cgl_bar`: it draws a value over a range as a swept segment. It **does not draw its own background** (it draws an empty track, but does not fill the frame), and by design it should sit on top of a watch, a card, or a panel.

| API | Semantics |
|---|---|
| `cgl_arc_gauge_init(g, name, frame, radius, thickness)` | Centered on the frame, covering the full turn (`start = CGL_ANGLE_UP`, `span = CGL_ANGLE_TURN`, clockwise), range 0..100, value 0, no dot, no pulse |
| `bool cgl_arc_gauge_set_range(g, vmin, vmax)` | Same as `cgl_bar_set_range()`: `vmax <= vmin` is rejected and counted in `cgl_arc_range_rejected()` |
| `cgl_arc_gauge_set_value(g, value)` | Set and draw immediately, clamped to the range |
| `bool cgl_arc_gauge_animate_to(g, value, dur_ms, ease)` | Same semantics as the bar function of the same name; starts from the current drawn value, and `dur_ms == 0` degenerates to an immediate set |
| `cgl_arc_gauge_set_span(g, start, span, dir)` | The start of the sweep, the angle covering the full scale, and the direction (`CGL_ARC_CW` / `CGL_ARC_CCW`). For example, a speedometer with a "225-degree opening and 315-degree closing": `start = CGL_ANGLE_DEG(135)`, `span = CGL_ANGLE_DEG(270)`, `CGL_ARC_CW` |
| `cgl_arc_gauge_set_colors(g, track, ink)` | The colors of the unfilled arc and of the completed arc |
| `cgl_arc_gauge_set_pulse_color(g, color)` | The color toward which the track blend tends during a pulse |
| `cgl_arc_gauge_set_dot(g, r, grow, color)` | Draw a dot of radius `r` at the leading edge, growing by `grow` pixels over one full pulse; `r == 0` (the default) removes it |
| `cgl_arc_gauge_set_sweep(g, sweep)` | Set the sweep length **directly** (in angle units), bypassing the "value → ratio" mapping until the next `set_value()`. **Does not mark damage** |
| `uint32_t cgl_arc_gauge_sweep(const cgl_arc_gauge_t *g)` | The current drawn sweep length (0..span) |
| `cgl_arc_gauge_set_scale(g, q8)` | Draw only `q8/256` of the current sweep. 256 is everything; 0 draws only the track (the dot is suppressed). **Does not mark damage** |
| `cgl_arc_gauge_pulse(g, dur_ms, ease)` | Start a pulse: the track blends toward `pulse_to` and the dot grows, both decaying from 256 to 0 within `dur_ms`. The tween runs from the widget's `update` and marks damage while it runs |
| `cgl_arc_gauge_set_pulse(g, q8)` | Set the ambient pulse strength Q8 directly; stops a running pulse tween. **Does not mark damage** (owner-driven, owner-marked) |
| `cgl_arc_gauge_set_center(g, cx_off, cy_off)` | Move the gauge center to an offset relative to the frame origin |
| `cgl_point_t cgl_arc_gauge_tip(const cgl_arc_gauge_t *g)` | The current sweep endpoint (screen coordinates): where the dot sits, and the only place where the arc "changes". **This is exactly what the damage model needs** |
| `cgl_arc_gauge_get_value` / `_get_shown_value` / `_is_animating` | Logical value / drawn value / whether a tween is running |

Grouping convention: **value setters** (`set_value`, `animate_to`, `set_range`, `set_span`, `set_colors`, `set_pulse_color`, `set_dot`, `set_center`) mark damage in the manner of `cgl_bar`; **owner gates** (`set_sweep`, `set_scale`, `set_pulse`) do **not** mark damage. A caller for which "the sweep is derived from the clock and the damage model already knows which bands the leading edge of the ring falls into" should not let the widget merge a 64×64 box into the per-frame damage — that is the difference between pushing 22 bands and 39 bands per frame.

The tween direction of the pulse is **256 → 0** and not 0 → 256; the direction is itself the point: the resting value of the pulse must be the value at which "nothing happened", otherwise every drawing that multiplies by it would carry a permanent tint after the first pulse.

Implementation note (to be treated as a constraint): `set_sweep()` is the only entry point for setting a direct sweep; do not write the `direct` / `direct_sweep` fields of `cgl_arc_gauge_t` by hand — the implementation keeps that state in a small table indexed by gauge address, so setting it by hand has no effect.

### 10.5 Watch (Analog Dial) `cgl_watch`

`cgl_watch` is a complete analog watch widget: a shaded disc face, an inset well, a 60-division index ring, a seconds indicator ring, three tapered hands, a hub, and an optional second-hand comet trail and glass cover. It is **built on top of `cgl_arc`** (the face and the well are two `cgl_arc_surface_t`, the three hands are `cgl_arc_stroke_t`, the seconds ring is an arc of radius 0 and thickness equal to the ring width, and the hand damage boxes come from `cgl_arc_needle_damage()`).

**Time and Calendar Types**

```c
typedef struct {
    uint8_t h, m, s;
    uint16_t ms;    /* milliseconds within the current second, 0..999 */
    uint16_t year;
    uint8_t month;  /* 1..12 */
    uint8_t day;    /* 1..31 */
    uint8_t wday;   /* 0 = Sunday */
} cgl_watch_time_t;
```

Helper functions (all pure):

| API | Semantics |
|---|---|
| `bool cgl_watch_is_leap(uint16_t year)` | Leap-year test (including the 100-year and 400-year rules) |
| `uint8_t cgl_watch_days_in_month(uint16_t year, uint8_t month)` | Number of days in a month; out-of-range months are taken modulo 12, so the result is always the length of a real month |
| `void cgl_watch_next_day(cgl_watch_time_t *t)` | Add one day to the date, including month and year carry |
| `uint32_t cgl_watch_seconds_of_day(const cgl_watch_time_t *t)` | Seconds since midnight |
| `cgl_angle_t cgl_watch_fraction_angle(num, den)` | The angle corresponding to `num/den` of a turn (requires `num <= 65535`) |
| `cgl_angle_t cgl_watch_face_angle(num, den)` | The dial reading angle: `CGL_ANGLE_UP` plus the clockwise part. The "clock convention" is this one line |
| `uint32_t cgl_watch_second_yank_ms(ms, tick_ms, tick_ease)` | The number of milliseconds within the second at which tick mode yanks the second hand past the previous second's index |
| `cgl_angle_t cgl_watch_second_angle(const cgl_watch_time_t *t, uint8_t mode, uint16_t tick_ms, uint8_t tick_ease)` | The second-hand angle. Both modes are pure functions of time and agree at every whole second |
| `cgl_angle_t cgl_watch_hour_angle(const cgl_watch_time_t *t)` | The hour-hand angle, **continuous** (it creeps forward a little every second, like a real hour hand) |
| `cgl_angle_t cgl_watch_minute_angle(const cgl_watch_time_t *t)` | The minute-hand angle, likewise continuous |
| `uint32_t cgl_watch_minute_sweep(const cgl_watch_time_t *t)` | The angle the minute has traveled, for use by the seconds indicator ring |

The second-hand mode is selected by `cgl_watch_second_mode_t`:

| Value | Meaning |
|---|---|
| `CGL_WATCH_SECOND_SWEEP` | Classic quartz sweep: `(s*1000 + ms) / 60 s` of a turn |
| `CGL_WATCH_SECOND_TICK` | Mechanical tick: it rests on the current second's index and is "yanked" into the next division during the last `tick_ms`, slightly overshooting and then settling back midway. That motion is a **function of time and of the mode**, not a tween, so dropped frames, occlusion, and any frame rate cannot make it lose step |

**Configuration**: `cgl_watch_config_t` consists of four parts.

```c
typedef struct { int16_t r_base, r_tip; uint8_t w_base, w_tip; cgl_color_t color; } cgl_watch_hand_t;
```

- `cgl_watch_geom_t`: `cx`/`cy` (the dial center in **screen coordinates**), `r_face`, `r_ring`, `ring_w`, `r_well_in`/`r_well_out`, `r_tick_out`, `r_tick_major_in`, `r_tick_minor_in`, `r_hub`, `r_tail`.
- `cgl_watch_anim_t`: `tick_count` (number of divisions, 60), `tick_major` (one major index every n divisions, 5), `tick_w_minor`/`tick_w_major`, `second_mode`, `tick_ms`, `tick_ease`, `reveal_ms`/`reveal_ease` (entry sweep), `pulse_ms`/`pulse_ease` (minute-jump pulse), `trail_ms` (comet-trail afterglow), `shadow_off`/`shadow_alpha` (hand shadow).
- `cgl_watch_style_t`: `bg`, `bezel`/`bezel_hi`/`bezel_w`/`bezel_lit_start`/`bezel_lit_sweep` (the case lip and its lit half-turn), `tick`/`tick_major`/`trail`/`hand`/`hub`/`accent`/`accent_hi` (indices, hands, hub, accent color), `face`/`well` (two `cgl_arc_surface_t`), `glass` (`cgl_watch_glass_t`).
- Hands: `hour`, `minute`, `second`; the anchor `start` (the starting reading and the date); `flags` (`CGL_WATCH_F_*`); the optional `brand`/`brand_font`/`brand_dx`/`brand_dy`/`brand_color`.

**Runtime flags** (`cgl_watch_config_t::flags`):

```c
#define CGL_WATCH_F_BEZEL  0x0001u   /* case lip */
#define CGL_WATCH_F_TICKS  0x0002u   /* index ring */
#define CGL_WATCH_F_RING   0x0004u   /* seconds indicator ring and dot */
#define CGL_WATCH_F_TRAIL  0x0008u   /* comet trail behind the second hand */
#define CGL_WATCH_F_HUB    0x0010u   /* hub cover */
#define CGL_WATCH_F_GLASS  0x0020u   /* glass cover */
#define CGL_WATCH_F_BRAND  0x0040u   /* brand text */
#define CGL_WATCH_F_HOUR   0x0080u
#define CGL_WATCH_F_MINUTE 0x0100u
#define CGL_WATCH_F_SECOND 0x0200u
#define CGL_WATCH_F_PULSE  0x0400u   /* minute-jump pulse */
#define CGL_WATCH_F_REVEAL 0x0800u   /* entry sweep */
#define CGL_WATCH_F_DEFAULT  /* all of the above, but without TRAIL and BRAND */
```

`CGL_WATCH_F_DEFAULT` is a "plainly usable watch": it contains neither the brand text (which needs a string) nor the comet trail (which needs RAM). Compile-time switches and runtime flags are two different things: a build with `CGL_CFG_WATCH_GLASS = 0` contains no glass-cover code at all, and `CGL_WATCH_F_GLASS` is then accepted and ignored (the configuration structure thus remains portable across builds). For the trail to take effect **at run time**, both `CGL_WATCH_F_TRAIL` and `CGL_WATCH_F_TICKS` must be set: the glow is carried by the index ticks, so setting TRAIL without TICKS amounts to nothing at all.

**API**

| API | Semantics |
|---|---|
| `void cgl_watch_config_default(cgl_watch_config_t *cfg)` | Fill in the complete usable reference watch (the dial of the demo page: geometry, colors, surfaces, hands, indices, starting reading 10:09:00, Wednesday 2026-01-14). With `CGL_CFG_WATCH_DIAL_LUT = 1` it points `style.face.lut` / `well.lut` at the **Flash** tables that ship with the module; with 0 it leaves them `NULL` (procedural shading, no Flash footprint) |
| `void cgl_watch_config_set_start(cfg, h, m, s)` | Set the starting reading (the anchor). Out-of-range values are taken modulo the next higher unit |
| `void cgl_watch_config_set_start_date(cfg, year, month, day, wday)` | Set the anchor date; the month and the day are **clamped** (not taken modulo), because "month 13" or "day 0" is a caller defect, and silently rendering "0 FEB" would only conceal it |
| `void cgl_watch_init(w, name, frame, const cgl_watch_config_t *cfg)` | Create the widget. When `cfg` is `NULL` the default configuration is used. It **copies** the configuration (`w->cfg = *cfg`) and stores no pointer. The widget **does not draw a background**, and its frame should be clipped (`cgl_widget_init()` sets `CGL_WF_CLIP` by default) |
| `void cgl_watch_frame(cgl_watch_t *w, const cgl_clock_t *clk)` | **Once per frame**: keep time, run the entry sweep and the pulse, age the comet trail, and mark damage only where the watch really changed. `clk == NULL` is a safe no-op |
| `void cgl_watch_set_time(w, h, m, s)` | Re-anchor to a wall-clock time (the RTC hook). The request is parked and takes effect at the start of the next `cgl_watch_frame()`, so calling it before the first frame is also legal and exact. The date is unaffected |
| `void cgl_watch_set_date(w, year, month, day, wday)` | Re-anchor the calendar date; `wday` is supplied by the caller (0 = Sunday). Out-of-range values are clamped |
| `const cgl_watch_time_t *cgl_watch_now(const cgl_watch_t *w)` | The currently displayed time (valid after the first frame; before that it is the configured starting reading) |
| `uint32_t cgl_watch_seconds_of_day_now(const cgl_watch_t *w)` | Seconds of the day for the currently displayed reading |
| `void cgl_watch_reveal(cgl_watch_t *w)` | Play the entry sweep: the hands sweep from 12:00 to the current time and the index ring assembles division by division. When `CGL_WATCH_F_REVEAL` is set, `cgl_watch_init()` plays it **once** automatically; a page that wants it replayed on every entry calls it in its own `on_enter`. **Setting the time does not replay it** |
| `uint16_t cgl_watch_reveal_value(const cgl_watch_t *w)` / `cgl_watch_pulse_value(...)` | Entry progress Q8 (256 = fully in place) / current strength of the minute pulse Q8 (256 = peak, 0 = at rest) |
| `void cgl_watch_set_second_mode(w, mode)` / `cgl_watch_set_tick(w, tick_ms, ease)` | Switch sweep/tick, and the yank duration and curve of the tick |
| `void cgl_watch_set_flags(w, flags)` / `cgl_watch_set_feature(w, flag, on)` / `bool cgl_watch_has_flags(w, flags)` | Whole-group/individual element switches, and a query |
| `void cgl_watch_set_center(w, cx, cy)` | Move the watch: the geometry, the centers of the two surfaces, and the widget frame all move by the same delta |
| `void cgl_watch_set_config(w, cfg)` / `const cgl_watch_config_t *cgl_watch_get_config(const cgl_watch_t *w)` | Replace the configuration wholesale (the "Apply" of a settings screen) / read the current configuration. On replacement, the two tweens of the minute hand and the second hand are **deliberately not reset in sync**, otherwise changing the color scheme would make the watch flash back to 12:00 first |
| `cgl_color_t cgl_watch_face_color(w, dx, dy)` / `cgl_watch_ring_color(w, dx, dy)` | The color of the face/well surface at an offset; pure functions that may be called before the first drawing |
| `cgl_color_t cgl_watch_glass_over(w, under, dx, dy)` / `cgl_watch_glass_shade(const cgl_watch_glass_t *gl, under, dx, dy)` | The effect of the glass cover on a color (compositing the light it reflects over `under`, where `under` may be the face, the well, an index, or a hand). The latter looks only at its arguments, so the two cannot give different answers. With `CGL_CFG_WATCH_GLASS = 0` it returns `under` itself (the arguments are still accepted, keeping the configuration structure portable) |
| `uint32_t cgl_watch_frames(const cgl_watch_t *w)` | The number of `cgl_watch_frame()` calls this widget has received |

**Three Contracts That Must Be Known**

1. **The watch does not drive itself.** The `update` vtable entry of `cgl_watch_t` is `NULL`, so `cgl_widget_update_tree()` / `cgl_screen_frame()` **do not** advance it. The page must call `cgl_watch_frame(&face, clk)` in its own `on_frame`. The time is taken from the framework's frame clock rather than from `dt`: the anchor must be some `clk->now_ms`, which is also why "the watch keeps time while it is occluded and not drawn". By contrast, the vtable of `cgl_arc_gauge_t` carries an `update`, and its tween is advanced by the widget-tree update.
2. **Time is an anchor plus an accumulation, while the date is state.** The watch stores `t0` (the wall clock at the anchor instant) and `t0_ms` (the `clk->now_ms` at the anchor instant) and derives `now = t0 + (now_ms - t0_ms)` every frame; that subtraction is unsigned, so a 32-bit millisecond counter wrap is safe. The date **must not** be recomputed from the anchor every frame (otherwise only one frame per day would show the correct date); it is accumulated day by day — which is also why a date can be displayed without an RTC.
3. **Damage discipline.** The incremental path marks only the regions that really change: the second hand (including its counterweight tail) once at the old angle and once at the new one; the minute hand and the hour hand only when the whole-pixel tip endpoints really change; the leading edge of the seconds ring once at the old reading and once at the new one; and the indices that still carry comet-trail afterglow. The whole widget is marked dirty only while the entry sweep or a pulse is running, and before the first frame. Damage is **deliberately not** constrained by the `CGL_WATCH_F_*` switches: a watch whose second hand has just been turned off must still erase the ink left by the previous frame, and pushing one extra band costs only one redraw, whereas pushing one band too few leaves a ghost.

### 10.6 Other Widget Modules

`cgl_panel`, `cgl_label`, `cgl_bar`, `cgl_arc`, and `cgl_watch` all follow the same pattern: a struct whose first member is `cgl_widget_t`, plus a vtable written with designated initializers, plus setters that mark damage only when the pixels really change. When adding a widget, use `cgl_panel.c` (the simplest) and `cgl_bar.c` (with tweening, configurable geometry, and its own failure counter) as references.

---

## 11. Display Devices and the Band Presentation Pipeline

### 11.1 The Display Instance

```c
typedef struct
{
    const cgl_display_drv_t *drv;
    void *ctx;

    cgl_rect_t damage;                     /* region awaiting redraw, screen coordinates */

    cgl_color_t band[2][CGL_CFG_SCREEN_W * CGL_CFG_BAND_H];   /* double-buffered bands */

#if CGL_CFG_ALPHA
    cgl_color_t alpha[CGL_CFG_ALPHA_LEVELS * CGL_CFG_ALPHA_LEVEL_STRIDE];  /* group opacity scratch */
#endif

    uint32_t frame_id;   /* number of frames pushed to the panel (**not** the frame rate) */
    uint32_t bands;      /* cumulative number of bands pushed (for observation) */
} cgl_display_t;
```

### 11.2 API

| API | Semantics |
|---|---|
| `int cgl_display_init(d, drv, ctx)` | Bind the driver; returns non-zero when `init` fails. It also marks the whole screen as damaged |
| `cgl_display_damage(d, r)` / `damage_all(d)` / `has_damage(d)` | Damage accumulation (a rectangle union, **not** a region list) |
| `uint16_t cgl_display_present(d, render, user)` | Process only the damage that intersects the screen; call `render` once per band (redrawing the whole band); return the number of bands pushed |
| `cgl_display_set_backlight(d, percent)` | A safe no-op when the driver does not implement a backlight |

The render callback:

```c
typedef void (*cgl_render_fn)(cgl_canvas_t *canvas, void *user);
```

### 11.3 The Presentation Flow

The pipeline of `present()`: render `band[cur]` → `set_window` → asynchronous `write_pixels` → render `band[!cur]` → wait for the previous band to finish… and the last band is closed out with `wait_idle`. The starting row of a band is aligned to a multiple of `CGL_CFG_BAND_H`, and the end may be a shorter residual band.

`frame_id` is the "number of frames pushed" and `bands` is the cumulative band count; neither of the two is the frame rate — the frame rate lives in `cgl_clock`.

### 11.4 Two Constraints That Must Be Maintained

1. **A whole band must be redrawn from the root of the widget tree.** `present()` never "redraws only the damaged part": the panel has no frame buffer, so the band that is pushed must be complete. The save-under compositor likewise relies on this, and its "background copy" is simply the content already drawn for the current band.
2. **`d->alpha[]` is written by the CPU only and never enters DMA.** It differs from `band[]`: while band N is being rendered, `band[!cur]` is being read by DMA, so that block cannot be used as scratch. This is also why the scratch area must be separate memory rather than a reuse of another band buffer.

### 11.5 Per-Pixel Regression on a PC

Replacing the driver with a mock that writes into an in-memory array, without changing a single line of the framework code itself, allows per-pixel assertions on the host. The test project does exactly this: the mock driver copies each band into a buffer according to the window given by `set_window`, and the assertions read that buffer directly. This is the preferred way to verify the pixel-level correctness of self-drawn widgets.

---

## 12. The Clock and Tweening

### 12.1 The Clock

```c
typedef struct
{
    uint32_t now_ms;      /* milliseconds since cgl_clock_init */
    uint32_t frame;       /* number of frames started (the first frame is 1) */
    uint32_t dt_ms;       /* this frame's dt, already clamped by max_dt_ms  */

    uint32_t step_ms;     /* milliseconds per logical step; 0 = do not run logical steps */
    uint32_t accum_ms;    /* fixed-step accumulator (the sub-step remainder stays here)       */
    uint32_t max_dt_ms;   /* upper limit on a single frame's dt                        */
    uint32_t step_total;  /* total logical steps; **not cleared by reset** (for observation) */
    uint8_t  max_steps;   /* upper limit on the number of steps per frame                        */
    uint8_t  steps;       /* steps planned for this frame                  */
    uint8_t  step_index;  /* iteration cursor of cgl_clock_next_step()    */

    uint32_t fps;         /* frame rate of the most recent statistics window */
    /* _fps_ms / _fps_frames are internal fields */
} cgl_clock_t;

#define CGL_CLOCK_MS_PER_S 1000u
#define CGL_CLOCK_DEFAULT_MAX_DT_MS 100u
```

| API | Semantics |
|---|---|
| `cgl_clock_init(c, step_ms, max_dt_ms)` | `step_ms = 0` means that only frame time runs and no logical steps run (a pure interface scenario). Must be called before the first `cgl_clock_begin()` |
| `cgl_clock_reset(c)` | Clears the counters and keeps the `step_ms` / `max_dt_ms` configuration. Used to restart the screen |
| `cgl_clock_begin(c, dt_ms)` | Begins one frame. **This is the only place in the entire framework where time is accumulated.** The part exceeding `max_dt_ms` is truncated and is never caught up afterwards |
| `bool cgl_clock_next_step(c)` | Iterates this frame's fixed steps: returning `true` means that another step can run, and each call consumes one step. The step count is computed by `begin()` from the accumulator, so the loop body executes exactly the number of times it should; the caller can neither run more nor fewer |
| `uint8_t cgl_clock_steps_left(c)` | How many steps remain in this frame (for the idiom of performing finalization only on the last step) |
| `uint32_t cgl_clock_step_ms(c)` | The number of milliseconds in one logical step (that is, the value received by `on_step`) |
| `uint16_t cgl_clock_alpha_q8(c)` | The inter-frame interpolation factor in Q8 (0..255): how far the accumulator has advanced within the current step. Not used by this project (RAM cannot hold a second position array), but the interface is retained because this is the only place where that ratio can be obtained |
| `uint32_t cgl_clock_fps(c)` | The frame rate of the most recent statistics window; it is updated only when the window ends, so it returns 0 for the first 500 ms |

Design points:

- The `step_ms` domain (logic) has a fixed step, is replayable and is independent of the frame rate; it is handed to `on_step`. The `dt_ms` domain (visuals) follows the frame rate and tolerates dropped frames; it is handed to `on_frame` and to the widget `update`. The differing resolutions are deliberate: simulation speed must not depend on SPI bandwidth, while fades look best when they follow the frame rate.
- "The world being advanced twice" is structurally impossible: widgets have no steps to advance at all, and the only advance point is the page's `on_step`.
- Stalls are not caught up: when the whole-step backlog exceeds the limit it is discarded in one go, and only the sub-step remainder is kept. Letting the simulation run slightly slow is preferable to "fast-forwarding after a stall".

### 12.2 Tweening

A `cgl_anim_t` is a timeline (`from -> to`, duration, easing, optional delay). Pure integer Q8, no floating point, no libm.

```c
typedef enum
{
    CGL_EASE_LINEAR = 0,  /* constant speed: mechanical, but the least error-prone */
    CGL_EASE_IN_QUAD,     /* slow start (acceleration)             */
    CGL_EASE_OUT_QUAD,    /* fast start then deceleration; the most commonly used "natural" curve */
    CGL_EASE_IN_OUT_QUAD, /* slow at both ends, fast in the middle             */
    CGL_EASE_OUT_CUBIC,   /* a "softer" finish than OUT_QUAD; the page default */
    CGL_EASE_OUT_BACK,    /* overshoot then spring back (the Q8 value exceeds 256); used for emphasis */
    CGL_EASE_COUNT
} cgl_ease_t;

#define CGL_ANIM_ONE_Q8 256
#define CGL_ANIM_F_RUNNING 0x01u   /* still running (including the delay phase) */
#define CGL_ANIM_F_DONE    0x02u   /* has reached to; the value no longer changes  */

typedef struct
{
    int32_t  from, to, value;   /* start / end / current value */
    uint32_t dur_ms;            /* duration; 0 means jump straight to to on the next frame */
    uint32_t elapsed_ms;        /* elapsed duration (excluding the delay) */
    uint32_t delay_ms;          /* remaining delay; while >0 the value stays at from */
    uint8_t  ease;              /* cgl_ease_t */
    uint8_t  flags;             /* CGL_ANIM_F_* */
} cgl_anim_t;
```

| API | Semantics |
|---|---|
| `cgl_anim_init(a)` | Clears to a tween that has never started (`value = 0`) |
| `cgl_anim_start(a, from, to, dur_ms, ease)` | Starts; clears the delay and the done flag |
| `cgl_anim_set_delay(a, ms)` | Delays the start. Must be called **after** `start()` (`start` clears the delay) |
| `bool cgl_anim_tick(a, dt_ms)` | Advances one frame; returns whether the value changed this frame, on which the caller bases the decision to redraw. Returns `false` both during the delay and after completion |
| `cgl_anim_finish(a)` / `cgl_anim_stop(a)` | Jumps immediately to the final value and sets DONE / stops in place and clears RUNNING (without jumping to the final value) |
| `cgl_anim_value(a)` / `cgl_anim_running(a)` / `cgl_anim_done(a)` | Inline reads of the current value / whether it is running / whether it is done |
| `uint16_t cgl_anim_progress_q8(a)` | Linear progress in Q8 (0..256), excluding easing and delay |
| `uint16_t cgl_anim_ease(ease, p_q8)` | The easing function itself: linear progress → eased progress. Except for `OUT_BACK`, the output lies within 0..256 and the endpoints hit 0 and 256 exactly |
| `int32_t cgl_anim_lerp(a, b, t_q8)` | Q8 linear interpolation: `t = 0` returns `a`, `t = 256` returns `b` |

A tween is also often used as a timer: `cgl_anim_start(&t, 0, 1, delay_ms, CGL_EASE_LINEAR)`, and when the time is up `cgl_anim_done(&t)` is true.

**Do not pair `CGL_EASE_OUT_BACK` with Genie / S-shaped suck-in.** The slide/cover types rely on its overshoot (progress briefly exceeding 256) to produce "overshoot then spring back", whereas Genie follows the geometric warp path; when `p > 256` the narrowing coefficient exceeds 256, the channel width factor becomes **negative**, and the image is mirrored horizontally and crosses past the suction port. `cgl_page_nav_set_ease()` is a "change-all" operation and therefore affects both of these types; to obtain "slide with bounce, suck-in unchanged", use `cgl_page_nav_set_transition_ease()` to configure each type separately.

---

## 13. Pages and Navigation

### 13.1 The Page Object and Its Vtable

A page is an **object** (not a numeric identifier) carrying a vtable; the navigator is a fixed-length stack (`CGL_CFG_PAGE_STACK_MAX`, default 4, no heap).

```c
typedef struct
{
    const char *type;

    void (*on_create)(cgl_page_t *p);                    /* builds the widget tree on first entry (once only) */
    void (*on_destroy)(cgl_page_t *p);                   /* tears down the tree when it is actually discarded            */

    void (*on_enter)(cgl_page_t *p, cgl_page_t *from, uint8_t kind);  /* entry begins */
    void (*on_exit)(cgl_page_t *p, cgl_page_t *to);                   /* exit begins */

    void (*on_pause)(cgl_page_t *p, cgl_page_t *by);     /* pressed underneath (PUSH/REPLACE)  */
    void (*on_resume)(cgl_page_t *p, cgl_page_t *from);  /* the page above pops away and this page is revealed again     */

    void (*on_step)(cgl_page_t *p, uint32_t step_ms);    /* fixed-step logic; top-of-stack page only         */
    void (*on_frame)(cgl_page_t *p, const cgl_clock_t *clk);  /* once per frame; top-of-stack page only    */

    bool (*on_event)(cgl_page_t *p, const cgl_event_t *ev);   /* input; top of stack only, not dispatched during a transition */
} cgl_page_vt_t;
```

**Every member may be `NULL`** (written with designated initializers, so an omitted member is `NULL`).

```c
struct cgl_page
{
    const cgl_page_vt_t *vt;   /* the first member, so a page can "inherit" like a widget */
    const char *name;
    cgl_widget_t *root;        /* this page's widget tree root; bound in on_create */
    cgl_page_nav_t *nav;       /* the owning navigator; filled in by the navigator on push    */
    void *user;                /* the page's own context (used by the application layer)    */
    void *arg;                 /* the argument of this push, see cgl_page_nav_request */
    uint16_t flags;

    cgl_page_trans_cfg_t enter;  /* this page's transition preference when it **appears** (the entering page of PUSH)   */
    cgl_page_trans_cfg_t leave;  /* this page's transition preference when it **disappears** (the leaving page of POP/REPLACE) */
};

#define CGL_PAGE_F_CREATED    0x0001u   /* on_create has run (internal use) */
#define CGL_PAGE_F_SHOW_BELOW 0x0002u   /* while this page is on top, the pages below keep drawing */
```

A page holds its own widgets as **static members**, so the "destroy → create" cycle can repeat indefinitely over the same static memory, without a heap and without leaking.

### 13.2 Lifecycle Order

| Action | Order |
|---|---|
| PUSH | `on_create` (new page) → `on_pause` (old page) → `on_enter` (new page) → transition → the old page is hidden |
| POP | `on_resume` (lower page) → `on_exit` (top page) → transition → `on_destroy` (top page) |
| REPLACE | `on_exit` (old page) → `on_create`/attach (new page) → `on_enter` (new page) → transition → `on_destroy` (old page) |
| RESET | `on_exit` + `on_destroy` for every old page in turn → attach the new page → `on_enter` → **no transition** |

Timing contract:

- `on_step(step_ms)`: received only by the **top-of-stack** page, 0..N times per frame, at a fixed step. Physics and logic belong here. A pressed-down page does not receive it, so "a paused page whose clock keeps running" is structurally impossible.
- `on_frame(clk)`: received only by the top-of-stack page, exactly once per frame. Tweens, state machines and text refreshes belong here.
- `on_event(ev)`: received only by the top-of-stack page, and **input is locked during a transition** (otherwise the user could stack three levels of navigation halfway through).

### 13.3 Per-Page Transition Preferences

```c
typedef struct
{
    uint8_t  trans;   /* cgl_trans_t; CGL_TRANS_AUTO = type not declared */
    uint8_t  ease;    /* cgl_ease_t; CGL_PAGE_FX_KEEP_EASE = curve not declared */
    uint16_t dur_ms;  /* milliseconds; CGL_PAGE_FX_KEEP_MS = duration not declared */
} cgl_page_trans_cfg_t;

#define CGL_PAGE_FX_KEEP_EASE CGL_EASE_COUNT   /* curve: falls back to nav->ease[] */
#define CGL_PAGE_FX_KEEP_MS   0u               /* duration: falls back to nav->duration_ms[] */
```

```c
void cgl_page_set_trans_enter(cgl_page_t *p, cgl_trans_t trans, cgl_ease_t ease, uint32_t dur_ms);
void cgl_page_set_trans_leave(cgl_page_t *p, cgl_trans_t trans, cgl_ease_t ease, uint32_t dur_ms);
```

The three parameters together describe a **complete** preference rather than "changing only the item passed in": to keep the original type and change only the feel, pass the original type again.

```c
/* declare both the type and the feel */
cgl_page_set_trans_enter(&p->base, CGL_TRANS_COVER_UP, CGL_EASE_OUT_BACK, 180u);

/* declare only the type; the feel falls back to the per-type table */
cgl_page_set_trans_enter(&p->base, CGL_TRANS_GENIE_IN,
                         CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);

/* revoke the declaration: return to "follow the caller / use the action default" */
cgl_page_set_trans_enter(&p->base, CGL_TRANS_AUTO,
                         CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
```

Properties and warnings:

- **The default is "nothing declared"**: `cgl_page_init()` clears both preferences, so pages that do not touch this mechanism behave **exactly** as before.
- **Declaring it in `on_create` takes effect for that navigation**: the preferences are read only when the navigation is committed (`nav__start()`), and the entering page's `on_create` runs before that.
- **A declaration remains in effect after `destroy -> create`**: it is static configuration of the page, not something recomputed every frame or on every push.
- **A `dur_ms` of 0 means "not declared"**; to express "this page switches instantly" the page must declare `trans = CGL_TRANS_NONE`.
- **Easing pitfall**: the curve declared on the page and the type finally used are resolved **separately**. Declaring `OUT_BACK` on a page whose type is not declared and to which the caller finally passes `GENIE_IN` runs into the problem at the end of section 12.3 — to obtain the bounce, declare the type as a slide type as well.
- **`RESET` is always an instant switch**; declaring `enter` has no effect on it.
- An out-of-range `trans` (≥ `CGL_TRANS_COUNT`) makes the **entire call** invalid (it leaves no half-changed preference); an out-of-range `ease` is clamped to `CGL_EASE_LINEAR`; `dur_ms` is clamped to 65535.

### 13.4 Navigation Actions and Transition Types

```c
typedef enum
{
    CGL_NAV_PUSH = 1,  /* push: the old page stays below (on_pause) and gets on_resume on return */
    CGL_NAV_POP,       /* pop: the top page is discarded and the page below is revealed again            */
    CGL_NAV_REPLACE,   /* replace the top: the old page is discarded, depth unchanged                  */
    CGL_NAV_RESET      /* clear the stack leaving only the new page (back to the main screen); no transition              */
} cgl_nav_kind_t;

typedef enum
{
    CGL_TRANS_NONE = 0,     /* instant switch. When the caller passes it explicitly it **overrides the page declaration**      */
    CGL_TRANS_SLIDE_LEFT,   /* the new page enters from the right, the old page exits to the left                    */
    CGL_TRANS_SLIDE_RIGHT,
    CGL_TRANS_SLIDE_UP,
    CGL_TRANS_SLIDE_DOWN,
    CGL_TRANS_COVER_UP,     /* only the new page moves, covering the old page from bottom to top (menu/dialog semantics)  */
    CGL_TRANS_COVER_DOWN,
    CGL_TRANS_FADE,         /* fade out and in through the background colour: the old page fades out in the first half, the new page fades in in the second half */
    CGL_TRANS_GENIE_OUT,    /* the old page is "sucked into" the suction port, revealing the page below (macOS minimise) */
    CGL_TRANS_GENIE_IN,     /* the new page "pours out" of the suction port (macOS restore from the Dock)        */
    CGL_TRANS_S_OUT,        /* same as GENIE_OUT, but the suction port is in the bottom-right corner and the path is S-shaped      */
    CGL_TRANS_S_IN,         /* same as GENIE_IN, emerging from the bottom-right corner along a reversed S shape             */
    CGL_TRANS_AUTO,         /* leave it to the navigator to take the action default (recommended)                */
    CGL_TRANS_COUNT
} cgl_trans_t;
```

| Action | Default transition | Semantics |
|---|---|---|
| `CGL_NAV_PUSH` | `GENIE_IN` | Push: the old page is retained (`on_pause`) and the new page pours out of the suction port |
| `CGL_NAV_POP` | `GENIE_OUT` | Pop: the current page is sucked into the suction port, revealing the page below |
| `CGL_NAV_REPLACE` | `GENIE_OUT` | Replaces the top of the stack, depth unchanged (the old page is sucked away) |
| `CGL_NAV_RESET` | `NONE` | Clears the stack and returns to the main screen (an instant switch is more natural) |

| Transition | Effect |
|---|---|
| `GENIE_OUT` | The leaving page is "sucked into" the suction port, and the page below **stays visible for the whole animation** |
| `GENIE_IN` | The entering page pours out of the suction port while the leaving page stays below unchanged |
| `S_OUT` / `S_IN` | Share the same funnel geometry as `GENIE_*`, with only two differences: the suction port moves to the bottom-right corner, and `cgl_warp_t.s_amp_q8` changes from 0 to non-zero (the lateral S trajectory) |
| `SLIDE_*` | Both pages pan at the same time (the new page enters, the old page exits) |
| `COVER_*` | Only the new page moves, covering the old page (menu/dialog semantics) |
| `FADE` | In the first half the old page fades out to the background colour and in the second half the new page fades in; the switch happens at the moment when the whole screen is exactly the background colour |
| `AUTO` | Left to the navigator: it first asks the page that "moves" in this operation, and falls back to the action default if the page has not declared anything |

A transition uses **one** `cgl_anim_t` (progress 0..256) to drive both pages: the slide types turn the progress into the `offset` of the two pages, so the two pages cannot fall out of step; `FADE` uses a resident mask widget (attached below the host, always topmost, and not drawn during non-FADE transitions).

### 13.5 Who Decides the Transition: Page → Caller → Type Table

Three things (**which transition**, **how fast**, **which curve**) fall back in the same order:

| Decision | 1) Page declaration | 2) Caller | 3) Fallback |
|---|---|---|---|
| Which transition | `page->enter.trans` / `leave.trans` | the `CGL_TRANS_*` passed to `cgl_page_nav_request()` | action default: PUSH→`GENIE_IN`, POP/REPLACE→`GENIE_OUT`, RESET→`NONE` |
| Duration | `page->enter.dur_ms` / `leave.dur_ms` | — | `nav->duration_ms[final type]` |
| Curve | `page->enter.ease` / `leave.ease` | — | `nav->ease[final type]` |

**The page declaration has the highest priority** (including the type): once the transition is written on the page, the call site must obey the page even if it explicitly passes another type. If the caller is to force a choice for one navigation, that item should not be declared on that page.

**The sole exception: the caller explicitly passes `CGL_TRANS_NONE`.** `NONE` is not a "style" (it does not answer "which animation") but rather "no animation", so the rule that the page declaration takes priority does not apply to it — otherwise there would be **no way at all to express "this operation must switch instantly"**. Snapshot tests need to capture stable frames with "no transition", and application initialisation should not play an animation before the first screen is laid out; both depend on this. If a page wants to pin itself to an instant switch, it can declare `trans = CGL_TRANS_NONE` itself.

**Which page has a say** (that is, the page that "moves" in this operation):

| Action | Page with a say | Which item is read |
|---|---|---|
| PUSH | **the entering page** (new page) | `enter` |
| POP / REPLACE | **the leaving page** (the page popped or replaced) | `leave` |
| RESET | — | the framework forces an instant switch (returning to the main screen is a "restart", not a transition) |

### 13.6 Per-Type Duration and Curve Tables

Duration and curve each have a table **indexed by type** (`nav->duration_ms[cgl_trans_t]`, 16 bits per entry; `nav->ease[cgl_trans_t]`, 8 bits per entry).

| Type | Default duration | Default curve |
|---|---|---|
| `NONE` | 0 | `LINEAR` (for observation only; the "change-all" operations do not touch it) |
| `GENIE_OUT` / `S_OUT` | `CGL_CFG_GENIE_MS` (default 320 ms) | `CGL_CFG_GENIE_OUT_EASE` (default `IN_QUAD`) |
| `GENIE_IN` / `S_IN` | `CGL_CFG_GENIE_MS` | `CGL_CFG_GENIE_IN_EASE` (default `OUT_CUBIC`) |
| All others | `CGL_CFG_TRANSITION_MS` (default 220 ms) | `CGL_CFG_TRANSITION_EASE` (default `OUT_CUBIC`) |

The S shape and the vertical suck-in are **identical in timing** (duration and curve share the same source in the default table): the difference in feel between the two suck-ins lies entirely in the **geometry** (the suction port plus the S path), not in the speed. To slow the S shape alone, use `cgl_page_nav_set_transition_duration(nav, CGL_TRANS_S_OUT, 500)` — each of the four suck-ins has its own table entry and they do not affect one another.

Runtime interfaces:

| API | Semantics |
|---|---|
| `cgl_page_nav_set_duration(n, ms)` | Changes the duration of **all** types (without touching `NONE`); 0 = everything switches instantly |
| `cgl_page_nav_set_transition_duration(n, trans, ms)` | Changes **one** type only; passing `CGL_TRANS_AUTO` is equivalent to changing all; clamped to 65535 |
| `cgl_page_nav_duration(n, trans)` | Reads the current duration of one type (milliseconds). It reads the **type table**, not "the value a given page will finally use" |
| `cgl_page_nav_set_ease(n, ease)` | Changes the curve of **all** types (without touching `NONE`); an out-of-range value is clamped to `LINEAR` |
| `cgl_page_nav_set_transition_ease(n, trans, ease)` | Changes the curve of **one** type only; `CGL_TRANS_AUTO` is equivalent to changing all |
| `cgl_page_nav_ease(n, trans)` | Reads the current curve of one type |

Duration determines "how many frames are needed": the frame in which the request is committed itself consumes one `dt`, so the frame count is approximately `ms / dt` rounded up (320 ms @ 17 ms per frame ≈ 19 frames).

These two tables are the **fallback layer**: for a page that has already declared a duration with `cgl_page_set_trans_enter/leave()`, that type's transition ignores them. Therefore "slowing down every page" and "slowing down one page" are two different knobs.

### 13.7 Page Registration

```c
typedef struct
{
    cgl_screen_t *screen;     /* borrowed: the frame clock, frame rate and display device          */
    cgl_page_nav_t *nav;      /* borrowed: the navigator these pages join            */
    void *app;                /* the application's own context (the page cross-link table); the framework never dereferences it */
} cgl_page_ctx_t;

typedef struct
{
    const char *name;
    cgl_page_t *page;         /* caller storage whose first member is cgl_page_t; must not be NULL */
    void (*attach)(cgl_page_t *page, const cgl_page_ctx_t *ctx);   /* may be NULL */
} cgl_page_desc_t;

uint8_t cgl_page_register(const cgl_page_desc_t *pages, uint8_t count,
                          const cgl_page_ctx_t *ctx);
const cgl_page_ctx_t *cgl_page_ctx(const cgl_page_t *p);
void cgl_page_set_root(cgl_page_t *p, cgl_widget_t *root);
void cgl_page_init(cgl_page_t *p, const cgl_page_vt_t *vt, const char *name, void *user);
```

**"Adding a page" means three changes**: write a standard-form `void app_page_xxx_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx)` in its own file; add an object member to the application's page storage structure; add one row to the page table.

```c
static const cgl_page_desc_t s_pages[] = {
    { "menu",  (cgl_page_t *)&s_app.menu,  app_page_menu_attach  },
    { "info",  (cgl_page_t *)&s_app.info,  app_page_info_attach  },
    { "watch", (cgl_page_t *)&s_app.watch, app_page_watch_attach },
};

(void)cgl_page_register(s_pages, (uint8_t)(sizeof(s_pages) / sizeof(s_pages[0])), &s_ctx);
```

Notes:

- `cgl_page_register()` walks the array in order and calls `attach` for each entry; a row whose `page` or `attach` is `NULL` is skipped rather than dereferenced — so "a given build configuration does not contain this page" is a legal state.
- This only **creates** pages; it does not push them. The first page is installed by `cgl_page_nav_reset()` (or `push()`), and that step is what runs the first `on_create`.
- `cgl_page_init(page, &vt, name, (void *)ctx)` stores the context in `cgl_page_t::user`, and any later callback (including `on_create`) can read it back with `cgl_page_ctx(p)`.
- Everything in the context is **borrowed**: `screen` / `nav` are owned by the porting layer or the test host, and `app` points at the application's own storage. The context must outlive the pages — give it static storage.
- Cross-page references are **not passed through extra parameters**: a page that needs a sibling page reads it from `ctx->app` (the application's own link structure). In this way "this build does not have that page" is a legal state (`links->foo == NULL`, the menu item is shown but does nothing) rather than yet another constructor variant.

### 13.8 The Navigation API and the Input Lock

| API | Semantics |
|---|---|
| `void cgl_page_nav_init(n, name, area)` | Initialises. `area` is usually the whole screen and serves as the basis for pan/warp and clipping |
| `cgl_widget_t *cgl_page_nav_host(n)` | The page host widget (attach it to the screen root) |
| `bool cgl_page_nav_request(n, kind, page, trans, arg)` | **Only records the request**; it does not change the widget tree. Of several calls in the same frame, only the **last** takes effect; an illegal request returns false and is counted in `req_dropped`; when a concrete `CGL_TRANS_*` is passed, it takes effect only if that page has not declared a type |
| `cgl_page_nav_push(n, page)` / `_replace` / `_reset` / `_back` | Convenience wrappers. `_back` returns false when the stack holds only one page |
| `void cgl_page_nav_frame(n, clk)` | Frame boundary: if the previous transition is still running when a new request arrives, it is first **run to completion immediately** (`finish` handles hiding/destroying and clearing the offset and warp), after which the request is committed, the transition is advanced, and the whole screen is marked as damaged. **This is the navigator's only entry point for modifying the widget tree** |
| `bool cgl_page_nav_event(n, ev)` | Hands the event to the top-of-stack page (first `page->on_event`, and if it is not consumed, dispatch through the widget tree). During a transition it returns false and performs no dispatch at all — the **input lock**. `CGL_EV_BACK` is popped directly by the **navigator** when the depth is > 1 |
| `cgl_page_nav_top(n)` / `depth(n)` / `busy(n)` / `pending(n)` / `transitions(n)` / `dropped(n)` / `warp(n)` | Observation and forwarding |
| `cgl_page_nav_set_genie_target(n, x, y)` | Sets the suction port position; the default is the midpoint of the bottom edge of `area` moved down by `CGL_CFG_GENIE_DROP_PX` (the off-screen "Dock"). `x` is the **centre column** of the suction port (not its top-left corner), so "suck into the bottom-right corner" should pass `area.x1`. Affects `GENIE_OUT/IN` only |

On the design of the input lock:

- During a transition `screen__drain_input()` **does not consume the head of the queue**; it waits until the transition ends before delivering. If it popped and discarded here ("dispatch returned false, so that is that"), a key pressed by the user during a 200 ms transition would genuinely be lost.
- Multiple requests are not lost: of several navigation requests in the same frame only the last takes effect, so rapid repeated presses are neither lost nor able to overflow the stack.
- The common practice of turning one key press into **two** events (see the reference example): a long press first produces `CGL_EV_KEY_LONG` and then `CGL_EV_BACK`. The menu page's `on_event` consumes `KEY_LONG` (interpreted as "enter the selected item") and along the way consumes the following `BACK` (there is nowhere to go at the bottom of the stack); a page that does not consume `KEY_LONG` (`on_event = NULL`) lets the following `BACK` reach the navigator and pop the stack. Thus "long press to go back" is written only where it is needed and each page does not have to implement it separately.

### 13.9 Semi-Transparent Overlays

By default, when a PUSH transition ends `nav__finish()` calls `cgl_widget_set_visible(false)` on the leaving page — and "hidden" means that the whole subtree **draws not a single pixel**. For an opaque page this is a free saving of one draw; but for a semi-transparent overlay only the host background colour remains below, and `cgl_widget_set_alpha()` shows through the background rather than that page's content. An overlay must therefore **do two things at the same time**:

```c
/* in the overlay's on_create / *_init */
cgl_page_set_show_below(page, true);           /* 1) keep the page below drawing */
cgl_widget_set_alpha(&p->root.base, 208u);     /* 2) composite the whole page with alpha     */
```

```c
void cgl_page_set_show_below(cgl_page_t *p, bool show);
bool cgl_page_get_show_below(const cgl_page_t *p);
```

Note that it **governs drawing only**:

- The page below **still does not receive `on_step`**, and its physics/logic remains frozen as before. This is a core invariant of the page model and must not be changed to make some effect work.
- The cost is determined by the complexity of the content of the page below: overlaying a **static** page is almost free (no damage means no redraw), whereas overlaying an **animated** page is expensive.
- The chained relationship holds as well: if A semi-transparently overlays B and C semi-transparently overlays A, then B keeps being drawn only once A has declared it too.

### 13.10 Implementation Notes on Genie and S-Shaped Suck-In

The real macOS Genie is a mesh warp plus pixel resampling. No full-screen buffer exists here, so an equivalent route was taken: each primitive recomputes its own landing positions row by row, and once drawing is finished the result looks as if it has been sucked in. The properties obtained:

- Solid rectangles are filled as before (for each source row, compute which row it is pressed onto and with what width, then fill one run);
- Text, bitmaps and discs narrow row by row (the font's column mapping goes through the warp as well);
- The cost is two extra multiply-accumulates per row — measured to be not much heavier than a slide transition, because the whole band is drawn only once.

The shape is obtained by multiplying three factors (the `warp__k()` in the source):

| Factor | Formula | Appearance |
|---|---|---|
| `prof` | a quadratic curve, 1.0 at the far end → `CGL_CFG_GENIE_NARROW_Q8` at the end nearest the suction port | a bell mouth concave on the sides; a straight line becomes a triangular cone and does not look like a sprite |
| `depth` | `1-(1-p)²` | funnel depth: narrowing from the bottom begins early while the far end is still wide |
| `shrink` | `p²` | overall narrowing: the whole shape thins later and finally collapses to a single vertical line |

The vertical direction is a **proportional contraction towards the suction port** (`y' = ty + (y-ty)(1-p)`): rows farther from the suction port move more, so it looks like "pouring in from top to bottom" rather than the whole page scaling down uniformly.

Which page is sucked in, which one is revealed below, and how the layers are arranged are all decided by the navigator (`nav__warp_setup()`): `GENIE_OUT` sucks away the leaving page and raises it to the topmost layer, while `GENIE_IN` sucks in the entering page. When the transition ends `nav__finish()` removes the warp, clears the offset and restores the layering.

The S shape (`S_OUT` / `S_IN`) **reuses** the funnel above completely and adds only two things in `nav__warp_setup()`: the suction port moves to the bottom-right corner, and the column mapping gains one lateral term

```
x' = cx'(p) + (x - cx)*k(y)/256 + S(y)
S(y) = 3*amp*phase*g(t)/65536,   g(t) = 256*u²(1-u)²,   u = t/256
```

The shape of `g` is the key: **it is tangent to the axis at both ends (`g'(0) = g'(1) = 0`)**, and the maximum displacement is at the midpoint, so the page bows out to the right and then draws back in the lower half to dive into the corner, forming an S path; `phase` (p for OUT, 1-p for IN) enters the multiplier of S, so the identity property at `p = 0` remains intact (no jitter on the first frame) and the two directions are exact inverses of each other.

The amplitude ceiling is not a matter of tuning taste but a **monotonicity constraint**: `|S'| <= 3*(amp/256)*(6/256)`, so only `s_amp_q8 <= 682` guarantees that the landing positions of adjacent source rows do not cross. `nav__s_amp()` derives the amplitude from the "half screen width" and then clamps it to `CGL_CFG_S_AMP_MAX_Q8` (default 160, which is exactly the value derived for a 320-pixel panel).

Note that this upper bound guarantees that "the lateral displacement itself does not cross into the neighbouring row", but it does **not** guarantee that the rounded output is strictly monotonic row by row — the column term is rounded while the S term is truncated, so even with a small amplitude the rendered result shows 1..2 pixel ripples between adjacent rows. That is rounding, not crossing; genuine folding would appear as a step comparable in size to the page width. In addition, the S shape requires `map_rect()` to **sample more densely**: the extremes are not at the corners of the rectangle (the lateral displacement is greatest in the interior of the page), and taking only the four corners would remove the most bulging segment.

The knobs that change the suck-in shape are `CGL_CFG_GENIE_NARROW_Q8` (the width of the narrow end) and the three factors in `warp__k()`; the suction port is changed with `cgl_page_nav_set_genie_target()`.

### 13.11 The Page Stack and Observation

| Observation | Meaning |
|---|---|
| `cgl_page_nav_depth(n)` | the current stack depth (1 = the main screen only) |
| `cgl_page_nav_busy(n)` | a transition is in progress (input is locked and nothing outside may modify the tree) |
| `cgl_page_nav_pending(n)` | there is a navigation request not yet committed |
| `cgl_page_nav_transitions(n)` | the number of completed transitions (one key press should add 1) |
| `cgl_page_nav_dropped(n)` | the number of requests rejected because the target was illegal (non-zero = a caller defect) |
| `cgl_page_nav_warp(n)` | the warp descriptor currently playing (`NULL` if none); hand it to `cgl_canvas_map_y()` to compute for yourself "what this frame's page has been sucked into" |
| `cgl_page_is_created(p)` / `cgl_page_root(p)` | whether the page has been created / its widget tree root |

Three ways to determine that "a page switch really took effect":

1. after one key press `nav.transitions` increases by one and `nav.depth` changes between 1/2/3;
2. on a pressed-down page, the number of steps that page receives (for example `s_app.smash.steps`) does not move at all, while the global `clock.step_total` keeps growing;
3. after returning, that page's step count resumes and does **not** jump by several hundred steps at once (a jump means that catching up occurred).

---

## 14. The Screen

`cgl_screen_t` combines the display device, the clock, the navigator and the input ring queue:

```c
typedef struct cgl_screen
{
    cgl_display_t *display;  /* not owned */
    cgl_page_nav_t *nav;     /* not owned (held by the application layer) */
    cgl_clock_t clock;       /* the only clock in the whole project */

    cgl_event_t queue[CGL_CFG_INPUT_QUEUE];
    uint8_t q_head, q_tail;
    uint32_t q_dropped;      /* events dropped because the queue was full (for observation) */
} cgl_screen_t;
```

| API | Semantics |
|---|---|
| `int cgl_screen_init(s, display, nav)` | Binds the display and the navigator, installs the damage sink, and loads the clock with `CGL_CFG_CLOCK_STEP_MS`. It verifies that the panel geometry agrees with `CGL_CFG_*` and that the navigator area covers the whole screen. Returns -1 if a parameter is `NULL` or they disagree |
| `void cgl_screen_frame(s, dt_ms)` | **A complete frame** (the seven steps of section 3.2) |
| `bool cgl_screen_post_event(s, ev)` | Enqueues only; returns false and increments `q_dropped` when the queue is full |
| `cgl_screen_clock(s)` / `_fps(s)` / `_top(s)` / `_nav(s)` / `_root(s)` / `_redraw_all(s)` / `_dropped_events(s)` | read-only forwarding: clock / frame rate / top-of-stack page / navigator / host widget / forced full-screen redraw / number of dropped events |

**Only one active `cgl_screen` may exist at a time**: the damage sink is a global singleton (see 3.5). This is a sufficient condition for single-screen devices.

`cgl_screen_root()` returns the navigator host widget, which covers the whole screen. Two ways to use it:

- **Attach widgets directly**: attach widgets to the host to obtain an interface with no page model (rapid prototyping, a single-screen instrument).
- **Use the page model**: attach widgets to the page root widget and let `cgl_page_*` manage them (recommended).

---

## 15. The Optional Extension Module `cytgl_ext`

### 15.1 Module Composition

The module lives in two trees **parallel** to the framework: `include/ext/` is its public API and `src/ext/` is its implementation. It deliberately does not belong to the framework — `src/core`, `src/render`, and `src/port` never include it, and `libcytgl.a` does not contain it. A build that does not link it costs nothing in Flash or RAM, and that is guaranteed by **selective linking** rather than by `#ifdef` in the core.

```
cgl_iso2d            position + projection + depth sort; knows nothing of rigid bodies
     |
cgl_iso2d_phys2d.h   bridge: the only file that includes both layers (all inline)
     |
cgl_phys2d           rigid bodies, broad-phase grid, solver, vertical axis
cgl_ext_basis.h      the Q12.4 convention and the force-inline macros shared by both
```

The dependency is **one-way**: `cgl_iso2d.h` contains no type, flag, or macro of `cgl_phys2d`; what it sorts is **positions** (through an accessor in `cgl_iso2d_src_t`). The bridge is the only place where the two layers meet, and therefore the only file permitted to include both.

The module contains three effects, each separated in the same way into a lower and an upper half (engine below, drawer above):

| Effect | Engine (framework-independent) | Drawer (knows the canvas) |
|---|---|---|
| 2.5D rigid-body physics and projection | `cgl_phys2d.[ch]`, `cgl_iso2d.[ch]`, `cgl_iso2d_phys2d.h`, `cgl_ext_basis.h`, `cgl_*_config.h` | Drawn by the user |
| Particle cloud (bullet time) | `cgl_particles.[ch]`, `cgl_particles_tables.[ch]`, `cgl_particles_config.h` | `cgl_particles_view.[ch]` |
| Shatter scene | `cgl_debris.[ch]`, `cgl_debris_config.h` | `cgl_debris_view.[ch]` |
| Gravity ball | `cgl_bounce.[ch]`, `cgl_bounce_config.h` | `cgl_bounce_view.[ch]` (with a standalone ball widget) |

The **engine** includes only `<stdint.h>` / `<stdbool.h>` / `<stddef.h>`; the **drawer** is the only file that includes `<cytgl/cgl_canvas.h>`. This one-way rule is written into the build scripts as well: drawer source files are added only when the framework target exists in the build graph.

The module's public headers are included as `<ext/...>` and share the single include root `include/` with `<cytgl/...>`, so one `-I include` serves both.

### 15.2 The Fixed-Point Convention

| | Physics and projection layer | Particle engine |
|---|---|---|
| Format | Q12.4 (`CGL_EXT_Q4_SHIFT` = 4, `CGL_EXT_Q4_ONE` = 16) | Q8.8 (`CGL_PARTICLES_SHIFT` = 8, `CGL_PARTICLES_ONE` = 256) |
| Resolution | 1/16 pixel | 1/256 pixel |
| Range (`int16_t`) | about ±2047 pixels | about ±127.99 pixels (guarded by the assertion of a "world half-span of ±120 pixels") |

The two make opposite trade-offs: Q12.4 offers a large range but low resolution, Q8.8 offers high resolution but a small range. The collision world is large and 1/16 pixel is sufficient; the particle scene is only about 232 pixels wide overall and the camera motion is sub-pixel, so **resolution matters and range does not**. The two conventions **never meet**: the particle engine reads no `cgl_phys2d` type and writes no `cgl_phys2d` field.

The coefficients of the camera matrix are **Q8** in the physics layer and Q8.8 in the particle layer; `cgl_phys2d_to_px()` is a **truncating** `>> 4`. Every fixed-point multiply involving a shift in the particle engine is written as a division by a power of 2 (`CGL_PARTICLES_MUL(a,b)` is `(a*b)/256`), so that the rounding direction is "truncate toward zero" rather than "toward −∞" — with `>>`, negative velocities would decay slightly faster than positive ones and the explosion cloud would visibly curve.

### 15.3 `cgl_phys2d`: A Fixed-Point Disc Rigid-Body World

Model: circular shapes; **infinite moment of inertia (bodies never rotate)**; the lattice dots share an equal mass and one "heavy ball" is infinitely heavy; **zero gravity, zero friction, and zero restitution within the plane**; pure translational disc dynamics = uniform-grid broad phase + circle-circle narrow phase + inelastic impulse + infinitely heavy kinematic bodies. All arithmetic is Q12.4 integer.

**The 2.5D vertical axis (z)**: every body carries `z` and `vz`, is integrated under gravity, and lands at `z = 0`, but **never takes part in collisions** — the solver is always a pure disc solver in the XY plane. There are two reasons: correctness of the metaphor (XY is the ground, so a fragment flying overhead still collides with whatever it passes) and cost (the broad phase, the impulse solve, and the tuning all stay unchanged and testable). The vertical axis is projected onto screen y by `cgl_iso2d`. The effect of fragments bouncing on the ground comes from the take-off rule at the end of `cgl_phys2d_step()`: a planar speed above the threshold lifts the body off the ground, it falls back, rebounds, and takes off again as long as it is still fast enough.

**Types**

```c
#define CGL_PHYS2D_FLAG_KINEMATIC 0x0001u  /* exempt from impulses (infinite mass) but still moves */
#define CGL_PHYS2D_FLAG_ACTIVE    0x0002u  /* takes part in the simulation       */
#define CGL_PHYS2D_FLAG_SPHERE    0x0004u  /* drawn as a lit sphere with a ground shadow, not a flat dot */

typedef struct {
    int16_t x, y;      /* position, Q12.4                */
    int16_t vx, vy;    /* velocity, Q12.4/step           */
    int16_t r;         /* collision radius, Q12.4        */
    int16_t z;         /* height above the ground, Q12.4 (2.5D, not below 0) */
    int16_t vz;        /* vertical velocity, Q12.4/step  */
    uint16_t flags;
} cgl_phys2d_body_t;   /* 16 bytes */
```

```c
typedef struct {
#if CGL_PHYS2D_USER_STORAGE
    cgl_phys2d_body_t *body;      /* caller storage, capacity entries            */
    int16_t *next;                /* caller storage, capacity entries            */
    int16_t *cell_head;           /* caller storage, CGL_PHYS2D_GRID_CELLS entries */
    uint16_t capacity;            /* pool capacity, decided by the caller        */
#else
    cgl_phys2d_body_t body[CGL_PHYS2D_MAX_BODIES];
    int16_t next[CGL_PHYS2D_MAX_BODIES];
    int16_t cell_head[CGL_PHYS2D_GRID_CELLS];
#endif
    uint16_t count;           /* number of bodies added (0..capacity)  */
    uint16_t active;          /* number of bodies still inside the simulation domain */
    uint16_t active_dynamic;  /* of those, the number that are not kinematic */
    uint32_t step;            /* number of steps simulated            */
} cgl_phys2d_t;
```

The arrays are **public members** (this is not an opaque type); the examples and the tests index `world.body[i]` directly.

```c
typedef struct {
    cgl_phys2d_body_t *body;   /* capacity entries  */
    int16_t *next;             /* capacity entries  */
    int16_t *cell_head;        /* GRID_CELLS entries */
    uint16_t capacity;         /* 1..32767         */
} cgl_phys2d_storage_t;                                 /* only with CGL_PHYS2D_USER_STORAGE=1 */

#define CGL_PHYS2D_STORAGE_BYTES(cap) /* byte count of pool + next + cell_head */
```

| API | Semantics |
|---|---|
| `uint16_t cgl_phys2d_capacity(const cgl_phys2d_t *w)` | Pool capacity. In runtime-storage mode it returns `capacity`, otherwise the compile-time `CGL_PHYS2D_MAX_BODIES`. It is a function rather than a member because in the embedded form a member would become a constant resident in RAM, and `sizeof(cgl_phys2d_t)` is part of the resource cost the module publishes |
| `void cgl_phys2d_storage(cgl_phys2d_storage_t *st, body, next, cell_head, cap)` | [User-storage mode only] Packs caller-declared storage into a descriptor |
| `bool cgl_phys2d_init_storage(cgl_phys2d_t *w, const cgl_phys2d_storage_t *st)` | [User-storage mode only] The only way to obtain a usable world in that mode. It returns false for a null array, zero capacity, or a capacity beyond the `int16_t` index range, and leaves the world in an **unusable** state (a subsequent `step()` does nothing) |
| `void cgl_phys2d_reset(cgl_phys2d_t *w)` | Empties the world (the storage is retained). It **only zeroes `count`**: it frees nothing and notifies nobody that "your index now belongs to someone else", and it also zeroes `step` |
| `int cgl_phys2d_add(cgl_phys2d_t *w, x, y, vx, vy, r, flags)` | Adds one body and returns its index; returns **-1** when the pool is full |
| `void cgl_phys2d_step(cgl_phys2d_t *w)` | Integrates one step and solves every contact. It is **exactly one step**; when that happens is decided entirely by the caller. `step` is incremented on every step |
| `int16_t cgl_phys2d_to_px(int16_t q)` | Fixed-point to pixels (**truncating**) |

Compile-time consistency gates (all at the end of `cgl_phys2d.h`, using `_Static_assert`): grid cell 1..256 pixels, pool capacity 1..32767, world bounds not above 32767, positive grid W/H, and **the anchors of the reference geometry** — `CGL_PHYS2D_CELL_Q4 == 8 * ONE`, `WORLD_W == 400 * ONE`, `WORLD_H == 256 * ONE`, `GRID_W == 50`, `GRID_H == 32`. The reason is that the physics tuning (the 5-neighbourhood broad-phase sweep and the assumption in `cgl_phys2d__resolve()` that only neighbours are submitted) was measured on a 400×256 world with a 50×32 grid; a macro that quietly changes the grid **does not fail loudly** but makes collisions stop happening, with the sole symptom that "balls pass straight through the lattice". When the world dimensions are changed deliberately, these gates must be re-derived at the same time.

### 15.4 `cgl_iso2d`: Projection and Depth Sorting

Projection (world Q12.4 → screen pixels, entirely integer):

```
ground_x = ((m00 * x + m01 * y) >> 12) + ox
ground_y = ((m10 * x + m11 * y) >> 12) + oy      <- also the depth key
screen_y = ground_y - ((kz * z) >> 12)
```

**Why the depth key is not y**: under isometric projection, "sorting by y" is wrong; the camera depth is the projected ground y, that is `m10*x + m11*y`. The reference lattice is a long strip (106 × 14 cells): x differs by 300 pixels while y differs by only a few dozen pixels, so sorting by y alone makes two dots 300 pixels apart within one row appear equidistant and the occlusion order becomes random. Taking `ground_y` uniformly as the key is automatically a weighted sum of the two world axes under the isometric style, and degenerates to `y` under the flat style (`m10 = 0`), so one piece of code is correct for both cameras.

**Why counting sort**: the depth key is a bounded screen coordinate (0..171), and bucket sort is O(n + 256) and fully deterministic — no comparisons, no recursion, no 2 KB comparison-sort scratch, no worst case. The result is ordered **far → near** (bucket 0 = top of the screen = farthest) and drawn directly in sequence: no z-buffer is required (band rendering has nowhere to put one in any case).

**The sort runs once per physics step, not once per band**: the renderer is called once per 4-pixel band (43 bands for a 320×172 screen), and sorting per band would waste 43 times the work. The sort result carries a **stamp** (it returns immediately when `s->stamp == src->stamp`), so it does not matter who triggers the step. What the stamp actually is, the caller decides: for a physics world it is `world->step`, and for a sprite list it is whatever the caller calls "one update".

**Types**

```c
typedef struct {
    int16_t m00, m01;  /* screen x coefficients, Q8 (multiplied with world Q12.4 coordinates >>12 to obtain pixels) */
    int16_t m10, m11;  /* depth (= ground screen y) coefficients, Q8, also the sort key */
    int16_t kz;        /* height lift coefficient, Q8                   */
    int16_t ox, oy;    /* screen offset, pixels                         */
} cgl_iso2d_cam_t;

typedef enum {
    CGL_ISO2D_STYLE_ISO = 0,          /* oblique ground; the vertical compression of the two world axes is solved separately, aiming to maximise the lattice
                                         minimum spacing after projection (the "most breathable lattice"). Engineering default */
    CGL_ISO2D_STYLE_ISO_UNIFORM = 1,  /* textbook 30 degrees: m10 == m11. The optimum under a square world
                                         (equilateral triangular lattice); the longer and narrower the world, the flatter it squeezes */
    CGL_ISO2D_STYLE_FLAT = 2          /* identity matrix: XY laid one-to-one on the screen (the old flat look, no pseudo-3D) */
} cgl_iso2d_style_t;

typedef struct { int16_t x, y, z; } cgl_iso2d_pos_t;   /* world Q12.4; z is not below 0 */

typedef struct {
    bool (*read)(const void *ctx, uint16_t i, cgl_iso2d_pos_t *out);  /* false = element i is not in the scene */
    const void *ctx;      /* passed back to read unchanged: the caller's own state (for example the physics world) */
    uint16_t count;       /* number of elements submitted to read: 0..count-1 */
    uint32_t stamp;       /* identity of the current set of positions */
} cgl_iso2d_src_t;

typedef struct {
    int16_t order[CGL_ISO2D_MAX_POINTS];      /* element indices, far -> near, drawn directly in sequence */
    uint16_t bucket[CGL_ISO2D_DEPTH_BUCKETS];
    uint16_t count;   /* number of valid elements in order */
    uint32_t stamp;   /* the src->stamp the last sort corresponds to */
    bool valid;       /* when false, the next sort is guaranteed to re-sort */
} cgl_iso2d_sorter_t;

typedef struct { cgl_iso2d_cam_t cam; cgl_iso2d_sorter_t sorter; } cgl_iso2d_view_t;
```

Contract of `read`: it must fill `*out` from element `i` and return `true`, or return `false` to skip the element when it does not belong to the scene. It is called only for `i ∈ [0, count-1]`, in increasing order, and **twice per sort** (one pass counting the elements per bucket, one pass placing them), and "the two passes must give the same answer".

| API | Semantics |
|---|---|
| `void cgl_iso2d_cam_fit(cam, style, wx0, wy0, ww, wh, vx, vy, vw, vh)` | Makes the world rectangle fall exactly inside the viewport rectangle (both centred) and derives the projection coefficients. **Guarantee**: after the call, the projection of the four corners of the world rectangle always lies within `[vx, vx+vw-1] × [vy, vy+vh-1]` — the function measures its own result and keeps shrinking, because fixed-point rounding turns "fits by calculation" into "spills one or two pixels". The only case it cannot fix is a viewport smaller than the world at unit scale: there it stops shrinking and **returns the smallest camera it can produce** rather than failing |
| `void cgl_iso2d_cam_set(cam, m00, m01, m10, m11, kz, ox, oy)` | Low-level entry point: hands over the matrix directly, which makes it convenient to try angles on real hardware |
| `void cgl_iso2d_project_rect(cam, wx0, wy0, ww, wh, *x0, *y0, *x1, *y1)` | The screen bounding box of the projected world rectangle (z = 0). All four corners must be projected and the minimum/maximum taken: under this projection the rectangle becomes a parallelogram, so the diagonal alone is not enough |
| `int32_t cgl_iso2d_ground_x(cam, wx, wy)` / `ground_y(...)` | Ground projection (**without** `ox`/`oy`). `ground_y` is the depth key: the larger it is, the closer to the viewer |
| `void cgl_iso2d_project(cam, wx, wy, wz, *sx, *sy)` | World Q12.4 → screen pixels; `cam.ox`/`cam.oy` are added internally |
| `void cgl_iso2d_sorter_reset(s)` | Forces the next `sort()` to re-sort. **Must be called after the world is rebuilt or the camera changes** |
| `void cgl_iso2d_sort(s, cam, src)` | Sorts `src` into drawing order by depth (far → near). A further call with the same `src->stamp` is a **no-op**, so it is safe to call for every band of a frame. Elements beyond `CGL_ISO2D_MAX_POINTS` are **ignored** (the table cannot hold them) |
| `void cgl_iso2d_view_init(v)` | Initialises the camera + sorter pair: one per scene, shareable by several widgets |

**The bridge `cgl_iso2d_phys2d.h`** (all inline):

```c
_Static_assert(CGL_PHYS2D_MAX_BODIES <= CGL_ISO2D_MAX_POINTS,
   "the cgl_phys2d pool is larger than the cgl_iso2d sort table: raise CGL_ISO2D_MAX_POINTS "
   "(cgl_iso2d_config.h) or lower CGL_PHYS2D_MAX_BODIES (cgl_phys2d_config.h)");

bool cgl_iso2d_phys2d_read(const void *ctx, uint16_t i, cgl_iso2d_pos_t *out);
void cgl_iso2d_phys2d_src(cgl_iso2d_src_t *src, const cgl_phys2d_t *w);
void cgl_iso2d_sort_bodies(cgl_iso2d_sorter_t *s, const cgl_iso2d_cam_t *cam,
                           const cgl_phys2d_t *w);
```

In `read`: element `i` is body `i`; it takes part when it carries `CGL_PHYS2D_FLAG_ACTIVE`; false is returned when `i >= w->count` or the body is inactive. The `stamp` of `src` is `w->step` — the physics step count **is** the identity of the position set. `sort_bodies()` builds `src` on the stack and calls `cgl_iso2d_sort()`.

### 15.5 `cgl_particles`: A Q8.8 Particle Cloud and an Orbiting Camera

Three promises: **no floating point** (every time-varying quantity is Q8.8 or a small integer, and the object files contain no `sinf`/`cosf`/`sqrtf` calls); **no division and no libgcc helper functions on the hot path** (the perspective divide is a reciprocal table plus one linear interpolation, and the trigonometry is a 64-entry 0..90° sine table with quadrant folding, 128 bytes of Flash covering the full circle); **12 bytes per particle**.

Why not extend `cgl_iso2d`: it is a fixed orthographic projection whose matrix is fitted once and never changes afterwards, and its inverse (screen → world) does not exist, so "walking the camera around the scene" cannot be expressed; turning it into a general-purpose 3D camera would make the whole application pay a Flash and RAM budget and would break the contract the module has already published.

**Fixed-point utilities**

```c
#define CGL_PARTICLES_SHIFT 8
#define CGL_PARTICLES_ONE   256
#define CGL_PARTICLES_PX(v) ((int32_t)(v) * CGL_PARTICLES_ONE)      /* compile-time constants only */
#define CGL_PARTICLES_MUL(a, b) ((int32_t)(((int32_t)(a) * (int32_t)(b)) / CGL_PARTICLES_ONE))
#define CGL_PARTICLES_ANGLE_FULL 65536u

int32_t cgl_particles_cell_x(int i);
int32_t cgl_particles_cell_y(int j);
int16_t cgl_particles_clamp_i16(int32_t v);
uint8_t cgl_particles_clamp_u8(int32_t v);
int16_t cgl_particles_sin_q8(uint8_t a);      /* Q8.8, -256..256; a is the high byte of a 16-bit angle */
int16_t cgl_particles_cos_q8(uint8_t a);      /* = sin_q8(a + 0x40) */
int32_t cgl_particles_recip_q16(int32_t d8);  /* 1/depth, Q16.16; table lookup + one linear interpolation */

extern const uint8_t  cgl_particles__sin_q8[64];
extern const uint16_t cgl_particles__recip[385];
```

Angle convention: a full circle = 65536 (`uint16_t`), 64 units = 90°. The sine table clamps `sin(90°)` to **255** rather than 256 — 256 does not fit in a `uint8_t`; without the clamp it would be written as `0x100` and truncated to 0 in the array, making `sin(90°)` become **zero** (a singular camera matrix, not a rounding error). The reciprocal table is indexed by depth itself (`cgl_particles__recip[d] = round(65536/d)`); the first 128 entries (depths 0..127) are never read and exist so that "depth → index" becomes an **identity mapping**, which saves one subtraction or shift in the inner loop: it costs 256 bytes of Flash and buys one instruction fewer per particle per band.

**World and camera constants**

| Macro | Default | Meaning |
|---|---|---|
| `CGL_PARTICLES_GRID_W` / `_GRID_H` | 106 / 14 | Number of cells of the reference lattice (the caller writes the cell coordinate callbacks accordingly) |
| `CGL_PARTICLES_CELL_PITCH_Q8` | 560 | Cell pitch = 2.1875 pixels. The reference logo is 106 cells × 3 pixels = 318 pixels, which does not fit the ±120 pixel window of Q8.8, so the whole scene is scaled to 240/318 = 0.755, and 2.1875 is the closest cell pitch that still keeps a whole cell inside the window |
| `CGL_PARTICLES_WORLD_HALF_SPAN_Q8` | `PX(120)` | World half-span, a safety gate (not a camera parameter): Q8.8 inside an `int16_t` can express only ±127.99 pixels |
| `CGL_PARTICLES_NEAR_Q8` / `_FAR_Q8` | 128 / 383 | Near and far clipping planes; the clamp in `recip_q16()` **is** the near/far plane |
| `CGL_PARTICLES_BUCKETS` | 128 | Counting-sort bucket count: the 128..383 pixel depth window is divided at 2-pixel resolution, finer than the 3-pixel dots the particles are drawn as, so quantisation cannot visibly reorder two particles |
| `CGL_PARTICLES_MAX` | 500 | Number of particles in the pool, 12 bytes each — **this dominates RAM** (500 = 6.0 KB), and it also determines how much debris the explosion can throw out |
| `CGL_PARTICLES_CAM_PITCH_A` | 21 | Fixed pitch angle, 21/256 of a circle = 29.5° |
| `CGL_PARTICLES_CAM_DIST_PX` / `_FOCAL_PX` | 210 / 235 | Camera distance and focal length; together they determine the projection scale and are constrained by two `_Static_assert`s to a range that is "neither outside the viewport nor a small blob in the middle" |
| `CGL_PARTICLES_CAM_DOLLY_Q8` | 40 | Dolly amplitude |
| `CGL_PARTICLES_TRIGGER_TICKS` | 78 | Trigger instant, measured in **logical steps** (not milliseconds; the engine has no notion of time, it only counts steps and reports them through `cgl_particles_tick()`) |
| `CGL_PARTICLES_DOT_SPAN_Q8` / `_DOT_BASE_Q8` | 384 / 256 | The dot diameter grows with the perspective scale from 1.0 pixel to 2.5 pixels and is clamped to 1..3 pixels |

**Types**

```c
typedef struct {
    int16_t x, y, z;    /* position, Q8.8, world space (origin = centre of the logo) */
    int8_t  vx, vy, vz; /* velocity, Q8.8/logical step                  */
    uint8_t life;       /* remaining life (steps); 0 = empty slot       */
    uint8_t bucket;     /* depth bucket for this frame, 0 = farthest from the camera */
    uint8_t hit;        /* 0 = still resting on its own cell, 1 = released */
} cgl_particle_t;       /* exactly 12 bytes (guarded by a _Static_assert) */
```

Velocities use `int8_t` rather than `int16_t`: a position must sit at ±116 pixels from the centre of the world, which requires int16, but a velocity does not, because the maximum speed in a burst is well below 0.5 pixels/step. Reducing the three velocities to 8 bits brings the whole cloud from 7.5 KB down to 6.0 KB — a quarter of a page of RAM saved. `hit` is not wasted: alignment is 2 (because of the int16 members), so 11 bytes would have been padded to 12 in any case. **Parked particles (`hit = 0`) are skipped entirely by `step()`** — no gravity, no life consumed — which is exactly why the untouched part of the lattice stays precisely in place. **The field order is load-bearing**: positions first, then the three velocity bytes, then the scalars, so the compiler has no opportunity to insert padding.

```c
typedef struct {
    int16_t m00, m01, m02;   /* camera x = (m00*x + m01*y + m02*z) / 256 */
    int16_t m10, m11, m12;   /* camera y */
    int16_t m20, m21, m22;   /* camera z — the **sort key** */
    int32_t dist_q8;         /* camera distance along the view axis, Q8.8 */
    int32_t orbit_q8;        /* extra depth offset of the dolly */
    int32_t focal_q8;        /* focal length */
    int32_t ox_q8, oy_q8;    /* projection origin (centre of the screen), Q8.8 */
} cgl_particles_cam_t;

typedef struct {
    int32_t origin_x_q8, origin_y_q8;   /* Q8.8 coordinates of the centre of the logo cell */
    int16_t pitch_q8;                   /* cell pitch */
    uint16_t max_active;
} cgl_particles_world_t;

typedef struct {
    cgl_particle_t particle[CGL_PARTICLES_MAX];
    uint16_t order[CGL_PARTICLES_MAX];       /* sorted indices, far -> near */
    uint16_t bucket[CGL_PARTICLES_BUCKETS];  /* bucket counts, later reused as prefix sums */
    uint16_t active, max_active;
    uint32_t seed;      /* xorshift state: libc rand is not used, deterministic */
    uint8_t tick;       /* logical steps simulated in this round */
    bool sorted;
} cgl_particles_t;

typedef union {
    cgl_particle_t align_particle;
    uint16_t align_u16;
    uint32_t align_u32;
    uint8_t bytes[CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX)];
} cgl_particles_storage_t;

#define CGL_PARTICLES_STORAGE_BYTES(n)   /* exactly sizeof(cgl_particles_t) when n == CGL_PARTICLES_MAX */
```

`ox_q8` / `oy_q8` must be `int32_t`: the centre of a 320-pixel panel needs 160 × 256 = 40960, which does not fit in an `int16_t` and would silently wrap to a negative value, with the symptom that the projection origin shifts 160 pixels to the left (the cloud is drawn off-screen while the depth sort is entirely normal).

The reason the **union type** `cgl_particles_storage_t` exists is to give the user a correctly aligned carrier: the engine casts the storage to `cgl_particles_t`, in which `uint32_t seed` requires 4-byte alignment, and "a cast of the right size but the wrong alignment" is undefined behaviour.

| API | Semantics |
|---|---|
| `cgl_particles_t *cgl_particles_init(void *storage, uint32_t storage_bytes)` | Binds the engine to caller storage; **it allocates no memory**, and the storage is bound once and outlives the engine. The storage must be at least `CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX)` bytes and 4-byte aligned. A buffer that is too small is **rejected** (returns `NULL`) rather than written past its end |
| `void cgl_particles_build(s, dot_count, is_lit, cell_x, cell_y)` | Builds a round in the **resting** state: one particle per lit cell of the lattice, landing exactly on its own cell, with the remaining spare particles parked at the centre. It reaches the bitmap through two callbacks instead of including a bitmap header — this keeps the engine self-contained and host-testable, and lets a test feed it a synthetic grid |
| `void cgl_particles_burst(s, impact_x, impact_y, impact_z)` | Detonates: gives every parked particle an outward velocity, as if a ball struck at `impact_x/y/z` (Q8.8) and is travelling towards +x. This is also where the caller's "bullet time" countdown starts |
| `uint16_t cgl_particles_impact(s, ball_x_q8, ball_r_q8)` | Advances the **sweep** of the heavy ball by one logical step: it releases every particle that the **leading edge** of the ball has already reached, and **returns how many were released** (0 = the ball has not yet touched that region). Releasing dot by dot rather than detonating everything at once is because a wholesale detonation would send the dots flying before the ball had touched most of them, which reads as "the logo dissolved before the ball arrived" |
| `void cgl_particles_reset(s)` | Discards all particles (`life = 0`) |
| `const cgl_particles_world_t *cgl_particles_world(s)` | World constants (origin, cell pitch) |
| `void cgl_particles_step(s)` | One **logical** step: gravity, damping, integration, culling. The step is unitised and knows nothing of milliseconds; the page owns the time scale and calls it 0..N times per fixed step, which is precisely the integer expression of "time almost stopped" |
| `int16_t cgl_particles_pos(s, i, axis)` / `int8_t cgl_particles_vel(s, i, axis)` | Reads position/velocity (axis: 0 = x, 1 = y, 2 = z), intended for tests, which then need no knowledge of the struct layout |
| `uint16_t cgl_particles_sort(s, cam)` | Depth-sorts (far → near) against one camera and **returns the number of particles still alive**. A re-sort is mandatory whenever the camera changes — with the orbit of this page running, that means every frame |
| `uint16_t cgl_particles_order_at(s, k)` | The particle index at position k in drawing order (0 = farthest) |
| `void cgl_particles_cam_look(cam, yaw_u8, orbit_q8, frame)` | Generates the camera matrix. `yaw_u8` is the high byte of a 16-bit angle (0..255 = one circle); `orbit_q8` is the dolly offset along the view axis (keep `\|orbit_q8\| < 64`, otherwise the depth clamp eats the far end of the scene); `frame` determines the projection centre |
| `uint16_t cgl_particles_active(s)` / `max_active(s)` / `uint8_t cgl_particles_tick(s)` | Observation |
| `uint16_t cgl_particles_view_draw_band(s, cam, c, band_y0, band_y1, color)` | Draws into the canvas the particles falling inside `[band_y0, band_y1)`. **Must be called once per band in increasing band order**; the sort **must already have been performed with the camera passed in** (it does not sort itself). Returns the number of particles actually drawn |

The `ox_q8`/`oy_q8` inside `cgl_particles_cam_t`, `cgl_particles_rect_t` (the same shape as `cgl_rect_t` but **deliberately a different type**, so mixing the two is a compile-time error), and `cgl_particles_view_frame()`, the single conversion entry point, together form the boundary at which "the engine does not know the framework and the drawer builds the bridge".

### 15.6 `cgl_debris`: The Shatter Scene

Responsibilities of the scene: `cgl_debris_init()` binds the world + camera + bitmap + viewport and builds the first round; `cgl_debris_step()` observes the world **after the caller has already stepped the physics**, advances the cycle, and rebuilds once the debris has come to rest; `cgl_debris_restart()` rebuilds immediately.

**It never steps the physics.** `cgl_phys2d_step()` is the application's own call — because "who advances the world" must have exactly one answer within a project; a widget or scene helper that stepped the physics on the user's behalf would make "N things each call it once and the world runs N times too fast" possible again. This module only **observes**.

```c
typedef struct { int16_t x0, y0, x1, y1; } cgl_debris_rect_t;

typedef struct {
    int grid_w, grid_h;              /* number of cells horizontally/vertically, > 0 */
    bool (*is_lit)(int i, int j);    /* whether cell (i, j) is lit; unlit cells are skipped outright */
} cgl_debris_src_t;

typedef enum { CGL_DEBRIS_PHASE_SMASH = 0, CGL_DEBRIS_PHASE_SETTLE = 1 } cgl_debris_phase_t;

typedef struct {
    cgl_phys2d_t *world;         /* body pool; declared and stepped by the caller */
    cgl_iso2d_view_t *view;      /* camera + depth sorter; NULL = draw nothing (physics-only tests pass it) */
    const cgl_debris_src_t *src; /* the bitmap (the caller's) */
    cgl_debris_rect_t frame;     /* the viewport the camera is fitted into */
    void (*on_change)(void *user);  /* the module's only outward call: "redraw needed"; may be NULL */
    void *on_change_user;
    uint16_t dot_count;   /* number of dots placed by the last build */
    uint16_t ball_index;  /* index of the heavy ball in the pool */
    uint16_t active;      /* number of dots still inside the simulation domain */
    uint8_t  phase;       /* cgl_debris_phase_t */
    uint8_t  style;       /* current camera style */
    uint16_t settle_ms;   /* milliseconds accumulated in the SETTLE phase */
    uint32_t restarts;    /* number of completed rounds */
} cgl_debris_t;
```

Every pointer in the struct is **borrowed** and must outlive it. The bitmap belongs to the caller — two members plus one callback instead of including a bitmap header, which is exactly what keeps the module a module.

| API | Semantics |
|---|---|
| `void cgl_debris_init(d, world, view, src, frame)` | `world` and `src` are **required**; `view` **may be `NULL`** (nothing is drawn, the physics still runs). `frame` is the viewport used by the next camera fit, and this call performs one fit |
| `void cgl_debris_restart(d)` | Rebuilds the lattice and puts the heavy ball back at its starting point, **then resets the depth sorter** (a rebuilt world has `step` back at zero, and a stale sort stamp would give the next frame the old order) |
| `void cgl_debris_step(d, step_ms)` | The wrap-up after one physics step: observe the world, advance SMASH/SETTLE, and rebuild once the settle timer has expired. The caller **must** already have called `cgl_phys2d_step()`. `settle_ms` is accumulated from `step_ms`, so `CGL_DEBRIS_SETTLE_MS` keeps its "milliseconds" semantics and is unaffected by the clock step |
| `void cgl_debris_set_style(d, style)` | Switches the camera style at run time (re-fitting against the current frame) |
| `bool cgl_debris_cell_world(d, i, j, *x, *y)` | The world coordinates (Q12.4) of cell (i, j), exactly the same set used at rebuild time. **Provided for rendering tests**: projecting each cell's world coordinates back through the current camera and reading pixels lets a machine decide whether the projected lattice is still legible. Returns false when out of range |
| `uint16_t cgl_debris_dot_count(d)` / `active_dots(d)` / `ball_index(d)` | Observation |

The drawer:

```c
typedef struct {
    cgl_color_t floor;       /* one step brighter than the background */
    cgl_color_t grid;        /* ground grid lines  */
    cgl_color_t shadow;      /* one step darker than the ground */
    cgl_color_t dot;         /* ordinary (lattice) bodies */
    cgl_color_t ball;        /* the heavy ball of this scene */
    cgl_color_t ball_other;  /* the other spheres in the world (drawn in a different colour so that "which ball scattered the lattice" stays visible at all times) */
} cgl_debris_palette_t;

uint16_t cgl_debris_view_draw(const cgl_debris_t *d, cgl_canvas_t *c,
                              int16_t band_y0, int16_t band_y1,
                              const cgl_debris_palette_t *pal);
```

`cgl_debris_view_draw()` draws the ground, the grid, and every body falling inside this band, and is called **once per band in increasing band order**. `pal == NULL` means "no colours", that is, nothing is drawn (the function invents no defaults, so a caller that forgets the palette gets an empty picture rather than an unexpected result). Returns the number of bodies actually drawn.

The ground belongs to the module rather than the application: debris receives a one-pixel ground shadow, and against a dark background a shadow is visible only when there is something to fall. The ground is the precondition that makes the statement "this dot is in the air" readable.

**The one-renderer rule**: bodies are drawn in the **single** depth order produced by `cgl_iso2d_sort()`, farthest first and nearest last, the later covering the earlier. The widget tree draws its children in insertion order and cannot express "these two dots should come between those two", so **one call to `cgl_debris_view_draw()` must draw every body in the world**. If one widget draws the lattice and another draws the ball, the ball permanently covers the lattice, and occlusion is immediately wrong when the ball flies behind the lattice.

### 15.7 `cgl_bounce`: The Gravity Ball

The purpose of this module is to eliminate four recurring defects: `vy += g` must come **before** the integration (semi-implicit Euler, stable), while placing it after (explicit Euler) gains height at every arc apex; reflecting the velocity alone is not enough — the position must be clamped back too, otherwise the ball "bounces from a place it has never been", which reads as stuttering; the world does not remember your bodies (`cgl_phys2d_reset()` only zeroes `count`, it frees nothing and notifies nobody), so this module re-establishes its own body by **identity** (radius + sphere flag) rather than by index; and restitution has a lower bound — the reference arena loses `(1 - e²)` on every bounce, and going far below the default makes the ball stick to the ground after two or three bounces.

Call shape:

```
cgl_bounce_init()        binds the world and the active area, throws the ball
cgl_bounce_pre_step()    vy += g        — before cgl_phys2d_step()
cgl_bounce_post_step()   four walls     — after cgl_phys2d_step()
cgl_bounce_restart()     back to the spawn point immediately
```

The two halves must be paired and run **exactly once per step**: omitting `pre_step` gives a floating ball, and omitting `post_step` gives a ball that leaves the area and is clamped back only when someone reads its position. They are split into two entry points rather than one `cgl_bounce_step()` because the single `cgl_phys2d_step()` must sit between them.

```c
typedef struct { int16_t x0, y0, x1, y1; } cgl_bounce_rect_t;
#define CGL_BOUNCE_NO_INDEX 0xFFFFu   /* the value of cgl_bounce_t::index when the ball is not in the pool */

typedef struct {
    cgl_phys2d_t *world;
    uint16_t index;          /* the index in the pool or CGL_BOUNCE_NO_INDEX. **Never read and use it directly**:
                                it is valid only until the next cgl_phys2d_reset();
                                cgl_bounce__ensure() rebuilds the body before each use; use cgl_bounce_index() for display */
    uint16_t bounces;        /* cumulative wall hits (a HUD reading and also what the tests assert on) */
    uint16_t max_speed_q4;   /* historical maximum speed, Q12.4 (observation only) */
    uint16_t area_rejected;  /* number of rejections for an impossible area */
    uint8_t  has_area;       /* 1 = an arena exists, 0 = none (the area was invalid or absent at init) */
    int16_t left_q4, right_q4, top_q4, bot_q4;                  /* active area, Q12.4 */
    int16_t start_x_q4, start_y_q4, start_vx_q4, start_vy_q4;   /* spawn parameters */
    void (*on_change)(void *user);   /* the module's only outward call: "redraw needed"; may be NULL */
    void *on_change_user;
} cgl_bounce_t;
```

The active area is "**a closed-interval rectangle of screen pixels that the centre of the ball may occupy**" — the range of the centre rather than "the box the ball fits into", because that range is exactly what the two clamps and the two reflection comparisons use, so nowhere in the module is there any need to add or subtract the radius at a boundary, and the collision area and the drawing area cannot drift apart. The drawing extent is this rectangle expanded outwards on each side by `CGL_BOUNCE_RADIUS_PX`. A centre lying on a boundary counts as **inside** (the walls are closed intervals). `has_area` makes "rejected" mean **inert** rather than "left in some default state": four boundaries all 0 (a one-pixel-square arena at the origin) is an entirely legal area, otherwise the inert ball would add itself to the world and bounce in the corner of the screen.

| API | Semantics |
|---|---|
| `bool cgl_bounce_init(b, world, area)` | Binds and throws the ball; the spawn point and velocity are derived from the area by `CGL_BOUNCE_SPAWN_*`, so "upper left, thrown to the right" holds wherever the caller places the arena. It returns false when there is no world, or when the area is **rejected** (inverted/empty, or outside the physics simulation domain). On failure no ball is thrown, every subsequent call is a no-op, and the ball stays **inert** — further calls are safe and further stepping is safe |
| `bool cgl_bounce_set_area(b, area)` | Moves the **walls** without moving the ball: a ball outside the new area is clamped back by the next `cgl_bounce_post_step()`; one inside keeps its position and velocity. The spawn point is **not** recomputed. It returns false when the area is rejected, and **the previous area remains in effect** |
| `void cgl_bounce_set_spawn(b, x_px, y_px, vx_q4, vy_q4)` | Sets the origin and velocity of the **next** throw without moving a ball already in flight. The position is given in pixels and the velocity in Q12.4 |
| `void cgl_bounce_pre_step(b)` / `post_step(b)` | Gravity / four-wall reflection + restitution damping + wall-hit counting + speed observation |
| `void cgl_bounce_restart(b)` | Re-throws from the spawn point and clears the counters |
| `bool cgl_bounce_position_px(b, *x, *y)` | The screen coordinates of the centre of the ball, **always clamped inside the active area**. The clamp is the whole point of this function: a step is "integrate first, reflect afterwards", and the reflection comes immediately after `cgl_phys2d_step()`, so **between those two instants** the ball may perfectly well be outside. Rather than depending on the caller's read order, the invariant is enforced at the point of reading. It is self-healing: if a `cgl_phys2d_reset()` wipes the ball from the pool, it adds the ball back. The parameter is **not const** |
| `bool cgl_bounce_draw_position_px(const cgl_bounce_t *b, *x, *y)` | The const version used by the drawer. The only difference is that it **does not self-heal**: a drawer must not add bodies to the world (it runs inside the `draw` callback, and a body added there enters the pool at a moment nobody planned for). It returns false when the ball has been wiped and there has been no step since |
| `uint16_t cgl_bounce_bounces(b)` / `bool cgl_bounce_area_px(b, *area)` / `uint16_t cgl_bounce_max_speed_q4(b)` / `cgl_bounce_index(b)` / `cgl_bounce_area_rejected(b)` | Observation. `area_px()` converts the effective area back to pixels and returns it, so the caller can draw the arena, lay things out inside it, or (as the reference test does) assert that the ball never left it, without re-deriving the arithmetic and drifting away from it |

The drawer and the standalone widget:

```c
cgl_bounce_rect_t cgl_bounce_view_area(cgl_rect_t frame);   /* frame inset by the radius on each side */
uint16_t cgl_bounce_view_draw(const cgl_bounce_t *b, cgl_canvas_t *c,
                              int16_t band_y0, int16_t band_y1, cgl_color_t color);

typedef struct {
    cgl_widget_t base;      /* must be the first member */
    cgl_bounce_t ball;      /* the engine; ball.world belongs to the caller, borrowed */
    cgl_color_t color;
} cgl_bounce_widget_t;

bool cgl_bounce_widget_init(cgl_bounce_widget_t *w, const char *name, cgl_rect_t frame,
                            cgl_phys2d_t *world, cgl_color_t color);
void cgl_bounce_widget_set_color(cgl_bounce_widget_t *w, cgl_color_t color);
```

`cgl_bounce_view_draw()` obtains the centre of the ball through the same clamp used when reading the position, so "one step integrating the ball through a wall" cannot draw a single pixel outside the playground. It clips by **band** (this effect has exactly one element, so returning early is cheaper than any per-primitive test), and returns 1 for drawn and 0 for clipped away or ball not in the pool — the 0/1 return value lets a test assert "this band drew the ball" without reading pixels.

**The either/or rule** (from the header): a **standalone floating ball** uses `cgl_bounce_widget_t` (z is not a concern); **inside an ordered scene** use `cgl_bounce_view_draw()` (one drawer per world). The standalone widget connects the engine's `on_change` to `cgl_widget_invalidate()`, so there is nothing that has to be remembered after a step; but it **does not advance its own physics**, and the caller must drive the step in between:

```c
cgl_bounce_pre_step(&ball.ball);   /* 1) vy += g            */
cgl_phys2d_step(&world);           /* 2) the only integration */
cgl_bounce_post_step(&ball.ball);  /* 3) four walls         */
```

### 15.8 Building, Linking, and Off Switches

**Three ways to consume it**

```bash
# A) As part of CytGL (a CMake target)
cmake -B build -DCYTGL_BUILD_EXT=ON
# Then in CMake: target_link_libraries(my_app PRIVATE cytgl::ext)   # pulls in cytgl::cytgl as well

# B) Standalone build: src/ext is a project in its own right and needs no framework at all
cmake -B build src/ext && cmake --build build      # -> libcytgl_ext.a
# Or: add_subdirectory(third_party/CytGL/src/ext)

# C) As two ordinary source files, with no build system at all
cc -std=c11 -O2 -Iinclude my_app.c src/ext/cgl_phys2d.c src/ext/cgl_iso2d.c -o my_app
```

**A mode-B build contains only the framework-independent half** (the physics pair, the particle engine, the shatter scene, the gravity ball, and the generated tables). The drawers (`cgl_particles_view.c`, `cgl_debris_view.c`, `cgl_bounce_view.c`) include `<cytgl/cgl_canvas.h>`, so they are added to the target only when the framework target exists in the build graph — a standalone build that tried to compile them would fail, which is a promise kept rather than a limitation.

`src/ext/CMakeLists.txt` publishes one include root (the repository's `include/`), so `<ext/...>` and `<cytgl/...>` sit side by side under the same `-I`. The install rules place `libcytgl_ext.a` in `lib/` and the module headers in `<prefix>/include/ext/` (deliberately **not** inside `include/cytgl/`). The core generates no `find_package` configuration, and neither does the module: consume it as `cytgl::ext` through `add_subdirectory()` / `FetchContent()`.

**Turning an individual effect off** (`CGL_CFG_PARTICLES` / `CGL_CFG_DEBRIS` / `CGL_CFG_BOUNCE`, all defaulting to 1)

Not linking them is the normal way to pay nothing, and it requires no macros: `libcytgl.a` never lists the module, and a build that does not reference it costs 0 bytes of Flash and RAM. The macros address one specific situation — a build that **collects source files per directory** (which is what this repository's firmware build does) compiles every `.c` under `src/ext/` whether or not anyone calls it:

```bash
cc -DCGL_CFG_PARTICLES=0 ... src/ext/cgl_particles.c src/ext/cgl_particles_view.c   # both become empty
cc -DCGL_CFG_DEBRIS=0    ... src/ext/cgl_debris.c    src/ext/cgl_debris_view.c      # same as above
cc -DCGL_CFG_BOUNCE=0    ... src/ext/cgl_bounce.c    src/ext/cgl_bounce_view.c      # same as above
```

For the **RAM** to actually come back, the application side must cooperate: storage declared as a static member of a page still exists after the engine has been compiled out. Size the particle pool with `CGL_PARTICLES_STORAGE_BYTES(n)` (or lower `CGL_PARTICLES_MAX`), and drop a page from the build as well if it is not wanted.

**These macros are build-level**: two translation units, one seeing 0 and the other seeing 1, cannot share the same `cgl_particles_t`. Put them in the build system, not in a local definition inside some file. This holds all the more for `CGL_PHYS2D_USER_STORAGE` — it **changes the memory layout of `cgl_phys2d_t`**, and mixing the two layouts in one binary is undefined behaviour.

### 15.9 Contracts, Guarantees, and Limits

**Guarantees**

- **Never uses the heap.** Every byte lives either in a caller-declared struct or on the caller's stack.
- **No mutable global state.** Two independent worlds cannot interfere with each other; the module's own RAM cost is 0.
- **No floating point, no libm, no 64-bit division helper functions.** The kernel loops are entirely integer.
- **Determinism.** The same input state and the same number of steps give bit-identical output: no time source, no randomness (the particles use an xorshift state inside the engine), and no dependence on the order in which the caller adds bodies.
- **The depth sort is a stable total order.** Bucket sort is O(n + buckets), with no comparisons and no recursion, and equal-depth elements keep their input order; `sorter.count` is exactly the number of elements actually in the scene, so culled bodies cost nothing to draw.
- **The camera fit honours its promise** (see 15.4).
- **A rejected state is inert rather than half-alive**: after a failed initialisation, further calls are safe and further stepping is safe.
- **The link surface is small**: the nine functions `cgl_phys2d_reset/add/step`, `cgl_iso2d_cam_set/cam_fit/project_rect/sorter_reset/sort/view_init`, plus a number of force-inline accessors.

**Obligations of the caller**

- **One drawer per world** (the one-renderer rule, see 15.6).
- **A rebuilt world must be re-sorted**: `cgl_phys2d_reset()` zeroes `step`, so `cgl_iso2d_sorter_reset()` must be called after a rebuild, otherwise the first frame may be drawn in the old order.
- **A change of positions must come with a new stamp**: `cgl_iso2d_sort()` is a no-op while `src->stamp` is unchanged; any change of positions must be accompanied by a different stamp. The particle engine is stricter — a change of **camera** mandates a re-sort (rotating the camera changes the painter order even when no particle has moved).
- **Size the sort table to the scene**: `CGL_ISO2D_MAX_POINTS` is a hard gate that must be no smaller than `CGL_PHYS2D_MAX_BODIES` (guarded by the `_Static_assert` in the bridge), and it must also be no smaller than the real element count, otherwise the excess elements are silently ignored — which appears as "some sprites never appear".
- **Caller storage must outlive the world**, and **one storage belongs to exactly one world** (two worlds sharing one storage silently corrupt each other).
- **The world is advanced in exactly one place**: `cgl_phys2d_step()` belongs to the application; the shatter scene only observes; the gravity-ball widget does not advance its own physics.
- **The two halves of the gravity ball must be paired**, exactly once per step, with the single `cgl_phys2d_step()` between them.
- **Do not cache `cgl_bounce_t::index`**, and do not act on it directly.
- **The band protocol**: every band drawer must be called once per band in increasing band order; the particle drawer requires the sort to have already been performed with the camera passed in.

**Deliberately not done (non-goals)**

- **Shapes**: circles only. No boxes, no rotation, no torque (the moment of inertia is infinite).
- **Collisions**: in the XY plane only. `z` is an expressive axis: a fragment flying overhead still collides with whatever it passes, which is exactly why the metaphor reads correctly.
- **Feel**: no gravity and no friction within the plane, and zero restitution; the vertical axis has gravity, the take-off rule, and a landing restitution, all of which are tunable. A "ball falling within the plane" is therefore not the engine's job — `cgl_bounce` is the layer that adds it.
- **No heap and no scene graph**: sorting is solved inside the module, until a second, unrelated consumer appears that demands a framework-level depth key.
- **One storage per world**, and the pool index type is `int16_t` (so a pool holds at most 32767 bodies, gated at build time).
- **Not responsible for timing**: the module has no clock, and one call to `cgl_phys2d_step()` is one step.

### 15.10 Complete Example

The following example uses the module as a whole: build a world, step it, fit the camera, depth-sort, project, and render every body into a canvas band through **one** drawing call.

```c
#include <cytgl/cgl_types.h>
#include <cytgl/cgl_canvas.h>
#include <cytgl/cgl_widget.h>

#include <ext/cgl_phys2d.h>
#include <ext/cgl_iso2d.h>
#include <ext/cgl_iso2d_phys2d.h>
#include <ext/cgl_bounce.h>
#include <ext/cgl_bounce_view.h>
#include <ext/cgl_debris.h>
#include <ext/cgl_debris_view.h>

#define GRID_W 12
#define GRID_H 5

/* ---- 1. Bitmap: the caller supplies is_lit, the module does not know the bitmap header ---- */
static const char *const s_glyph[GRID_H] = {
    "...####.....", "..##..##....", ".##....##...", "..##..##....", "...####....."
};

static bool glyph_is_lit(int i, int j)
{
    if ((i < 0) || (i >= GRID_W) || (j < 0) || (j >= GRID_H)) { return false; }
    return s_glyph[j][i] == '#';
}

/* ---- 2. Scene state: all of it static storage ---- */
typedef struct
{
    cgl_widget_t base;       /* must be the first member: this struct is a widget */
    cgl_phys2d_t world;      /* about 12.4 KB at the default capacity */
    cgl_iso2d_view_t view;   /* camera + depth sorter, one per scene  */
    cgl_debris_t scene;      /* borrows world + view + src            */
    cgl_debris_src_t src;
    cgl_debris_palette_t pal;
    cgl_bounce_t ball;       /* the second body in the same world     */
} demo_t;

static void demo_draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    demo_t *d = (demo_t *)w;
    const int16_t band_y0 = c->origin_y;
    const int16_t band_y1 = (int16_t)(c->origin_y + c->height);

    /* The single drawer for the world: the ground, the grid, and **all** bodies (including the gravity ball, in the ball_other colour)
       are drawn in one call in the same depth order. Never draw this world again with a second widget. */
    (void)cgl_debris_view_draw(&d->scene, c, band_y0, band_y1, &d->pal);
}

static const cgl_widget_vt_t s_demo_vt = {
    .type = "demo", .draw = demo_draw, .update = NULL, .event = NULL
};

static void demo_init(demo_t *d, cgl_rect_t frame)
{
    cgl_widget_init(&d->base, &s_demo_vt, "smash", frame);

    d->src.grid_w = GRID_W;
    d->src.grid_h = GRID_H;
    d->src.is_lit = glyph_is_lit;

    d->pal.floor      = CGL_COLOR_DARK;
    d->pal.grid       = CGL_COLOR_DIM;
    d->pal.shadow     = CGL_COLOR_BLACK;
    d->pal.dot        = CGL_COLOR_CREAM;
    d->pal.ball       = CGL_COLOR_AMBER;
    d->pal.ball_other = CGL_COLOR_CYAN;

    /* Bind world + view + bitmap + viewport, fit the camera per CGL_DEBRIS_SCENE_STYLE, build the first round */
    cgl_debris_init(&d->scene, &d->world, &d->view, &d->src,
                    cgl_debris_view_frame(frame));

    /* One gravity ball in the same world; the debris drawer draws it as well.
       cgl_bounce re-establishes its own body by identity after any rebuild. */
    (void)cgl_bounce_init(&d->ball, &d->world, cgl_bounce_view_area(frame));
}

static void demo_step(demo_t *d, uint32_t step_ms)
{
    cgl_bounce_pre_step(&d->ball);        /* 1) vy += g        */
    cgl_phys2d_step(&d->world);           /* 2) the only integration */
    cgl_bounce_post_step(&d->ball);       /* 3) four walls           */
    cgl_debris_step(&d->scene, step_ms);  /* observe and advance the cycle */
}

/* ---- 3. Explicit sorting and projection (what the drawer does internally) ---- */
static uint16_t demo_project(demo_t *d, int16_t *sx_out, int16_t *sy_out, uint16_t cap)
{
    uint16_t k, n;

    cgl_iso2d_sort_bodies(&d->view.sorter, &d->view.cam, &d->world);  /* within one step this is a stamped no-op */
    n = (d->view.sorter.count < cap) ? d->view.sorter.count : cap;

    for (k = 0u; k < n; k++)                      /* far -> near */
    {
        const cgl_phys2d_body_t *b = &d->world.body[d->view.sorter.order[k]];
        int16_t sx, sy;
        cgl_iso2d_project(&d->view.cam, b->x, b->y, b->z, &sx, &sy);   /* world Q12.4 -> screen pixels */
        sx_out[k] = sx;
        sy_out[k] = sy;
    }
    return n;
}
```

Review of the key points:

- `cgl_debris_init()` performs one camera fit internally, so an explicit `cgl_iso2d_cam_fit()` is not required before the first sort.
- `cgl_bounce_init()` is called **after** `cgl_debris_init()`, so that the ball is not wiped by the scene rebuild; later rebuilds are recovered by the self-healing of `cgl_bounce_post_step()`.
- The per-step order follows exactly the shape laid down by the gravity-ball header, and `cgl_debris_step()` is the caller's observation call after the physics.
- The drawing path obeys the one-renderer rule: exactly one `cgl_debris_view_draw()` per band, in increasing band order; `cgl_bounce_view_draw()` is **not** called on the same world.
- `demo_t.base` is the first member, consistent with the same rule documented for `cgl_bounce_widget_t`.

---

## 16 Extension Recipes

### 16.1 Adding a Widget

There are two reference implementations, answering two different questions:

- **The application's own widget**: it suffices to satisfy the seven rules of Section 9.6, without touching the framework at all. The smallest complete examples are `cgl_panel.c` (about 30 lines) and `cgl_label.c`.
- **The framework's own widget**: `src/core/cgl_bar.c` is the model — it needs `cgl_anim`, configurable geometry and its own failure counters, and it must ship with per-pixel specification tests. `src/core/cgl_arc.c` shows the more complex case (its own rasterizer + color table + dial).

### 16.2 Making a "thing" translucent as a whole

**One line**, and no widget changes are required:

```c
cgl_widget_set_alpha(&veil.base, 128u);   /* 0..255 */
```

| Desired effect | What to use | Cost |
|---|---|---|
| Darken/lighten an entire layer of solid color (a mask) | `cgl_canvas_fill_rect_blend_q8()` | **0 extra RAM**, about 28 cycles/pixel |
| **Translucent text / icons / panels** | `cgl_widget_set_alpha(&w.base, a)` | one staging area per layer per band |
| A container plus several child widgets translucent as a whole, with the child widgets overlapping each other | the same (this is precisely its strength: overlaps are not blended twice) | the same |
| Only a few pixels need translucency and "group" semantics are not required | call `cgl_canvas_fill_rect_blend_q8()` row by row yourself | 0 extra RAM, but pan/clip/warp must be handled manually |

Three pitfalls:

1. **Without a staging area, alpha is ignored** (drawn as opaque) rather than reported as an error. Observe it with `cgl_widget_alpha_dropped()`; normally it should be 0.
2. **There must be something beneath the translucent layer**: the framework has no full-screen framebuffer, so blending can only reveal "pixels drawn earlier in the same band traversal". It cannot reveal the previous frame, nor a page disabled by `CGL_WF_VISIBLE` (that is a navigator-level problem, not a blender-level one).
3. **Do not clear `CGL_WF_CLIP` for the sake of alpha**: that expands the isolation region from "its own frame" to "the entire clip window" — correct, but more expensive. If an overlay must extend beyond the parent widget frame and savings are still wanted, attach it beneath a higher ancestor.

### 16.3 Adding a Page

See 13.7. Three changes: one `*_attach()` function, one object member in `app_ui_t`, one row in the page table. To let the user reach that page, add it to the menu's name table and target resolution function, and increment the menu item count by one.

### 16.4 Adding / Adjusting a Transition

- **Make a particular page use a particular transition (most common)**: write `cgl_page_set_trans_enter/leave()` in that page's own `*_attach()`; one governs entry, the other governs exit.
- **Change the default transition of "all pages"**: the `switch` in `cgl_page.c` keyed on PUSH/POP/REPLACE/RESET (`nav__default_trans()`). If the application layer wants to force a type for one particular navigation, pass a concrete `CGL_TRANS_*` to `cgl_page_nav_request()` (it takes effect only when the page has not declared a type).
- **Change the suck-in target**: `cgl_page_nav_set_genie_target(nav, x, y)`. For example, "suck into the Dock at the bottom-left corner" is `(24, screen height + 6)`.
- **Change the shape**: `CGL_CFG_GENIE_NARROW_Q8` (narrow-end width) plus the three factors of `warp__k()`.
- **Make it faster/slower, or change only one class**: at page level use the `dur_ms` of `cgl_page_set_trans_enter/leave()`; at type level use `cgl_page_nav_set_transition_duration(nav, trans, ms)`; globally use `cgl_page_nav_set_duration(nav, ms)`. Page declarations override the type table, so "slow down only the information page" and "slow down every COVER_UP" are two different knobs.
- **Change the easing**: at page level use `ease`, at type level use `cgl_page_nav_set_transition_ease()`, globally use `cgl_page_nav_set_ease()`. The defaults are "slide with `OUT_CUBIC`, suck-in with `IN_QUAD`, pour-out with `OUT_CUBIC`".
- **Warning**: do not pair `GENIE_IN` / `GENIE_OUT` with `CGL_EASE_OUT_BACK` (see the end of 12.2).

### 16.5 Adding an Input Device (Touch / Encoder / Remote)

It suffices to produce one `cgl_event_t` and post it: either in the porting layer's `poll_input` hook (once per frame, carrying that frame's `dt` — the correct place for anything that needs debouncing), or by calling `cgl_port_post_event()` where the event is generated. Touch requires only implementing `cgl_widget_hit_test()` plus `CGL_EV_TOUCH_*` dispatch (`cgl_page_nav_event()` already forwards them); an encoder side button posts `CGL_EV_BACK`.

With **a single producer** matters are simplest: one place scans all inputs, one place posts, driven by `poll_input` carrying the current frame's `dt` every frame.

### 16.6 Replacing the Display Device / Replacing the Bus

- **Replace the panel controller**: implement a `cgl_display_drv_t`, then replace the `display` field in the porting descriptor `s_port` (the application layer never names any driver).
- **Replace the transport (parallel port / bit-banged SPI / emulator)**: implement a bus vtable (the `lcd_bus_t` of the reference project); the panel driver is not changed in a single line.
- **Validate on a PC**: implement a driver that writes bands into a memory array, and the real framework code can then be asserted per pixel without hardware.

### 16.7 Replacing the Font

Generate a glyph table from a TTF with the tool of Section 18.6, populate a `cgl_font_t` and pass it to the widget; or hand-write a table that follows the layout of Section 8.1.

---

## 17 Footprint and Trimming

### 17.1 Reference Build

The reference build is the bundled example application, running on an STM32G431CB (Cortex-M4 @ 170 MHz, 128 KB Flash / 32 KB RAM), compiled with `-O2`, with every effect switch enabled:

| Item | Value | Share |
|---|---|---|
| Flash (`text` + `data`) | 62 192 B | 47.5% of 128 KB |
| of which static data (`.data` + `.bss`) | 30 856 B | 94.2% of 32 KB |
| Stack reservation (`_Min_Stack_Size` = 0x540 in the linker script) | 1 344 B | 4.1% of 32 KB |
| RAM used (`data` + `bss` + reservation) | 32 200 B | 98.3% of 32 KB |
| **Remaining unallocated** | 568 B | 1.7% of 32 KB |

Flash and static data are measured values from `arm-none-eabi-gcc` 14.3; the stack reservation is read from the linker script, so the last three rows are "that measurement plus the reservation actually in force". **The physically available stack is 32 KB minus the static data**, which is 1 912 B with every effect enabled (this is not the same quantity as "remaining unallocated": the stack grows down from the top of RAM, and the reservation is itself stack).

**RAM is the bottleneck, not Flash.** After disabling the two animation pages (shatter and bullet time) and thereby leaving the physics world and the particle pool unreferenced, the same source is about 46.7 KB Flash / 16.0 KB RAM, and the available stack rises from about 1.9 KB to about 18 KB.

The library itself accounts for only a small part of both; most is the example application's physics world (480 rigid bodies plus the collision grid), the watch, and the configuration-enabled 5 KB band buffer plus 5 KB alpha staging area.

The frame rate ceiling is set by the bus: 42.5 MHz SPI pushing 320×172×2 bytes = 110 080 bytes takes about 20.7 ms, i.e. a push ceiling of about 48 fps; the actual frame rate is further limited by CPU rasterization. Half-screen or partial band redraws raise the effective frame rate accordingly.

### 17.2 Trimming Checklist

| Knob | Effect |
|---|---|
| `CGL_CFG_ALPHA 0` | Removes the alpha staging area entirely: recovers **about 5 KB RAM** at the reference geometry; `cgl_widget_set_alpha()` degenerates into a counted no-op |
| `CGL_CFG_BAND_H` | Band buffer RAM = `2 × W × BAND_H × 2`. Halving it saves about 2.5 KB, at the cost of more SPI window commands |
| `CGL_CFG_LABEL_MAX_TEXT` | 32 bytes per label; the example has about 25 labels, so each reduction of 8 characters saves about 200 bytes |
| `CGL_CFG_PAGE_STACK_MAX` | 4 bytes per entry |
| `CGL_CFG_WATCH_TRAIL 0` | Removes the watch's comet-tail state (60 bytes + accumulator) and the per-frame comet-tail damage marking |
| `CGL_CFG_WATCH_DIAL_LUT 0` | Removes the 578-byte default dial lookup table in favor of procedural shading (about three times the CPU per pixel, zero Flash) |
| `CGL_CFG_WATCH_GLASS 0` | Removes the glass cover pass (Flash only, but one fewer full-dial draw per frame) |
| `CGL_CFG_WATCH 0` | Removes the dial, the calendar and the damage model together (requires `CGL_CFG_ARC` to be 0 as well) |
| `CGL_CFG_ARC 0` | Removes the entire radial toolbox (requires `CGL_CFG_WATCH 0`) |
| `CGL_PHYS2D_MAX_BODIES` | 16 bytes per rigid body plus the 2 bytes of the grid linked list. The example scene needs 480 dots + 2 balls; 528 leaves about 9% headroom. **Do not drop below 500**: the gravity ball borrows the same world and adds one more rigid body, and when capacity is insufficient `cgl_phys2d_add()` starts returning -1 (manifesting as silently missing cells in the dot lattice) |
| `CGL_PARTICLES_MAX` | 12 bytes per particle, 500 in the example (6.0 KB). Size the storage with `CGL_PARTICLES_STORAGE_BYTES(n)` |
| `CGL_ISO2D_MAX_POINTS` / `CGL_ISO2D_DEPTH_BUCKETS` | 2 bytes per sort-table entry; 2 bytes per bucket. Note that `MAX_POINTS` must not be smaller than `PHYS2D_MAX_BODIES`, and being smaller than the real element count makes the excess elements silently disappear |
| `-DCGL_CFG_PARTICLES=0` / `-DCGL_CFG_DEBRIS=0` / `-DCGL_CFG_BOUNCE=0` | Empty the corresponding effect's compilation unit (for builds that collect source files by directory). **The RAM only comes back if the application side removes the static storage as well** |
| Application-side effect switches | Remove an entire page and the storage it owns. `APP_FX_SMASH` / `APP_FX_PARTICLES` of the example application belong to this class, and the former is the switch that recovers the physics world |

### 17.3 Estimation Method

- **Band buffer**: `2 × CGL_CFG_SCREEN_W × CGL_CFG_BAND_H × 2` bytes.
- **Alpha staging**: `CGL_CFG_ALPHA_LEVELS × CGL_CFG_ALPHA_LEVEL_STRIDE × 2` bytes (or read `CGL_CFG_ALPHA_BYTES` directly).
- **Labels**: `CGL_CFG_LABEL_MAX_TEXT` bytes each.
- **Physics world**: `CGL_PHYS2D_MAX_BODIES × (sizeof(cgl_phys2d_body_t) + sizeof(int16_t)) + CGL_PHYS2D_GRID_CELLS × sizeof(int16_t)`, obtained directly with `CGL_PHYS2D_STORAGE_BYTES(n)`.
- **Particle cloud**: `CGL_PARTICLES_STORAGE_BYTES(n)`.
- **Stack**: the physically available stack is 32 KB minus `.data` minus `.bss`. **"Remaining unallocated" is not "stack headroom"**: the `._user_heap_stack` reservation made by the linker script is empty address space, and the stack grows down from the top of RAM into it, so safety is decided by the difference between the physically available stack and the static worst-case call path. To measure that path, compile every translation unit with `-fstack-usage -fcallgraph-info=su` and take the longest call path in the graph (counting DMA interrupts and the 36-byte exception frame). `_Min_Stack_Size` in the linker script turns "a deeper widget tree" into a link failure instead of a runtime stack corruption; each additional level of widget tree depth costs about +136 bytes.
- `arm-none-eabi-size` folds `._user_heap_stack` into `.bss`, so the `bss` line is **not equal** to the amount of static data. "RAM used" (`data + bss`) and "static data" (`.data + .bss`) are two different quantities, and the difference is the stack reservation.

**Incremental measurement ("what did this change cost?")**

1. Check out the parent commit with `git worktree`, build both revisions with the **same compiler and the same linker script** in a directory of the same shape, and compare only the **difference** in `size`.
2. Comparing the absolute figures of two builds is misleading because of the compile switches: the IDE build enables `-fno-inline` / `-fno-omit-frame-pointer`, which costs roughly 1.5 KB more code than plain `-O2`.
3. To account for every byte, do a **symbol-level reconciliation**: `arm-none-eabi-nm --print-size --size-sort --radix=d <elf>`, drop the address column, sort, and compare item by item — what grew and what shrank is then explicit.
4. The `text` section size is **path-dependent** (the HAL source embeds `__FILE__`), so do not compare across directories and do not mix in historical snapshots.

**Handling historical snapshots**: earlier measurement points (for example "38.6 KB / 29.9 KB with only the framework linked") describe a different configuration from the current figures. Do not mix them; when quoting one, state the measurement point explicitly.

---

## 18 Build, Test, and Tools

### 18.1 CMake Options and Targets

| Option | Default | Purpose |
|---|---|---|
| `CYTGL_BUILD_TESTS` | `OFF` | Build the host (PC) regression tests and register them with CTest |
| `CYTGL_BUILD_EXAMPLES` | `OFF` | Include the `examples/` subdirectory; its firmware targets exist only in checkouts that provide an ARM toolchain and project settings, otherwise it is merely an informative empty target |
| `CYTGL_BUILD_EXT` | `OFF` | Build the optional extension module (`cytgl_ext` / `cytgl::ext`, `libcytgl_ext.a`) |
| `CYTGL_PORT_TEMPLATE` | `ON` | Compile `src/port/cgl_port_template.c` into the library as a syntax check |

The language standard is C11 with compiler extensions disabled (`CMAKE_C_EXTENSIONS OFF`); when no build type is specified the default is `Release`; the library target is `cytgl` (alias `cytgl::cytgl`), and the only public usage requirement is the include directory `include/`; under GCC/Clang `-Wall -Wextra -Wpedantic -Wshadow -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wundef -Wno-sign-conversion` is enabled, and under MSVC `/W4`. The install rules place `libcytgl.a` into `lib/` and `include/cytgl/` into `include/`; no `find_package` configuration is generated, and consumption is via `add_subdirectory()` / `FetchContent()`.

### 18.2 Commands

```bash
# Build the library
cmake -B build/host
cmake --build build/host

# Build and run the full regression suite on a PC
cmake -B build/host -DCYTGL_BUILD_TESTS=ON -DCYTGL_BUILD_EXT=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure

# Run a single test, or filter by name
ctest --test-dir build/host -R alpha_test --output-on-failure
ctest --test-dir build/host -R "alpha|page" --output-on-failure
ctest --test-dir build/host -E "render|bar" --output-on-failure

# Optional extension module: standalone build
cmake -B build/ext src/ext && cmake --build build/ext

# Cross-compile to Cortex-M (use gcc directly; the project's own build system is recommended)
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
    -std=c11 -O2 -ffunction-sections -fdata-sections -fno-common -Wall -Wextra \
    -Iinclude -c src/core/*.c src/render/*.c src/port/cgl_port.c
```

MSVC is a multi-configuration generator, so `--config Release` and `ctest -C Release` must be passed. Test executables are ordinary executables and can also be run directly; **snapshot output uses relative paths**, so when running them directly use the build directory as the working directory (CTest already sets each test's working directory to `${CMAKE_BINARY_DIR}`).

### 18.3 Regression Test Checklist

The tests compile and run the **real framework source**, replacing only the display driver with a mock driver that writes into a memory array — between `Devices/st7789.c` and `src/render/cgl_display.c` there is only a single `cgl_display_drv_t` vtable, so per-frame assertions on the host match the behavior on the target board.

| Test | What it verifies |
|---|---|
| `alpha_test` | Per-pixel specification of group opacity: overlaps are not blended twice, correct across 10 bands, bit-identical after restore, nesting beyond `CGL_CFG_ALPHA_LEVELS` is counted |
| `page_test` | Page + clock + tween + subtree pan: fixed-step execution, no catch-up after a stall, per-type and per-page transition tables, Genie warp geometry (`p = 0` identity, per-row step never exceeding 1, no holes in the funnel) |
| `widget_test` | Rules that screenshots cannot adjudicate: `CGL_CFG_WIDGET_MAX_CHILDREN` is enforced and counted, the default flags are exactly `VISIBLE\|CLIP`, hit testing respects `CGL_WF_CLIP`, warped subtrees are not hit |
| `bar_test` | Bar widget per pixel: value-to-fill mapping, rounding, self-clearing drawing, border and segments, damage discipline, tweening. Writes `build/bar_test.ppm` / `.bmp` |
| `arc_test` | Radial primitives and anti-aliasing: all 65536 angles compared against libm, `isqrt` compared against `sqrt`, circle/line/arc geometry, band consistency and pan. Writes `build/arc_test.ppm` / `.bmp` |
| `watch_test` | Using `cgl_arc` and `cgl_watch` **as modules**: the default configuration is already a usable watch, calendar carry, angle conventions, the baked table matching the shipping baker, "an incrementally damaged frame == a full redraw" |
| `logo_test` | A 20-second (1250-step) physics long run: period, point count, bounding box, no `int16` overflow, `z` within range, heavy balls knocking dots away |
| `bounce_test` | Bouncing ball and per-page fixed stepping, over the real `cgl_screen_frame(17 ms)` path: step count independent of widget count, no catch-up after being covered, no passing through walls |
| `bullet_test` | Particle engine and page: the engine storage and the page world are the same bytes, sine folding / reciprocal table / fixed-point multiply, draw order changing with the camera, time really stopping |
| `bullet_frames` | Frame dump: drives the real bullet-time page through one complete cycle |
| `clock_frames` | Frame dump: watch entry, several instants, minute jumps, comet tail |
| `clock_test` | Measured on the watch page: time base (including 32-bit millisecond wraparound), calendar across midnight and leap years, angle conventions, entry assembly, comet tail, digit rolling, a covered page still advancing time but receiving no frames, no ghosting |
| `clock_tick_test` | All assertions of the same page under the "mechanical tick" second-hand movement |
| `clock_tick_frames` | The same set of snapshots for that movement |
| `cgl_iso2d_test` | Projection formulas; the depth key must be `x+y` and not `y`; the sort of 200 rigid bodies is a true permutation and monotonic; sorting happens only once per step; all three camera styles stay inside the viewport |
| `ext_smoke_test` | The module is usable **as a module**: include only `<ext/...>` and link `cytgl_ext`; one step displaces a rigid body by exactly velocity × step count; a repeated sort within the same step is short-circuited by a stamp |
| `cgl_bounce_test` | Gravity ball contract: `vy += g` happens exactly **before** integration, energy decays within 900 steps, self-healing by identity, an invalid field yields an inert ball, two worlds are bit-independent |
| `user_storage_test` | Caller-supplied storage shape: `CGL_PHYS2D_STORAGE_BYTES(n)` matches the struct byte for byte, `add()` returns -1 when over capacity, rejected storage makes the world inert. This test **must not** link `cytgl_ext` (the macro changes the struct layout) |
| `ext_example` | The complete call sequence of the extension module (9 rigid bodies, 20 steps, one camera fit, depth sort, drawing to a 96×48 character screen), printing an ASCII picture to stdout |
| `render_test` | Per-pixel assertions on real scenes: the logo is still legible after projection, the old and new page regions of `COVER_UP`, the funnel outline of `GENIE_OUT/IN`, RESET/stack/pause. Writes the `.ppm` and `.bmp` of `frame00..frame25` |

**Warnings are not failures**: only a non-zero exit code from compilation or execution counts as FAIL. When running the full suite without `CYTGL_BUILD_EXT` enabled, the extension module's tests are not registered (the build system prints an explicit skip notice).

### 18.4 Snapshots and Inspection

Rendering tests write out images that can be opened directly. There are only two output functions:

```c
void img_dump_ppm(const char *path_fmt, int index, const uint16_t *buf, int width, int height);
void img_dump_bmp(const char *path_fmt, int index, const uint16_t *buf, int width, int height);
```

The input is an **RGB565 row-major** buffer. PPM is the `P6` binary format; BMP is 24-bit uncompressed, bottom-up, BGR, with rows aligned to 4 bytes, and writing is refused when the width exceeds 1024. Conversion from RGB565 to 8-bit channels is expanded with high-bit alignment, so exported images are slightly brighter than the panel's real precision — sufficient for checking layout and animation.

Inspection methods:

- `.bmp` can be opened by double-clicking (Paint, Photos);
- `.ppm` (P6) can be opened with GIMP, IrfanView or the corresponding editor extension;
- in a terminal/CI environment, use `tools/ppm_view.ps1` to downsample a PPM into **character art** (brightness ramp `' .:-=+*#%@'`, brightness taken as `0.299R + 0.587G + 0.114B`):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1 -File build/frame02.ppm
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1 -Threshold 200   # show only bright dots (single-pixel lattice)
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1 -Sample 3 -Width 200
```

Parameters: `-File` (if left empty, all `frame*.ppm` under `<repository>/build` are scanned), `-Sample` (how many pixels per character, default 2), `-Width` (maximum character width, default 170), `-Threshold` (0..255; when greater than 0 only blocks whose **brightest** pixel reaches the threshold are shown, so single-pixel dots are not averaged away). Note that snapshots written by a CTest run land in `<build directory>/build/`, which differs from the `<repository>/build` scanned by the script by default, so `-File` must be specified or the files copied first.

### 18.5 Observing on Hardware

The display device, the screen and the navigator are members of the static variable `s_board` inside the porting layer file; the pages and their worlds are in the application's file-static variable `s_app`. A debugger can locate both of them by name.

| Observation point | Meaning |
|---|---|
| `s_board.ctx.screen.clock.now_ms` / `frame` / `dt_ms` | Monotonic time / frame number / this frame's dt (clamped to 100 ms) |
| `...clock.steps` | Logical steps of this frame (normally 1, 2..4 after a stall) |
| `...clock.step_total` | Cumulative logical steps (not cleared by page changes) |
| `...clock.accum_ms` | Fixed-step accumulator remainder (normally 0..15) |
| `...clock.fps` | Frame rate over the most recent statistics window (0 for the first 500 ms) |
| `s_board.ctx.display.frame_id` / `bands` | Frames pushed / cumulative bands (**a full screen is 43 bands**) |
| `s_board.ctx.screen.q_dropped` | Number of input events dropped because the queue was full (non-zero = events arriving too densely) |
| `s_board.nav.depth` / `trans.active` / `trans.trans` | Stack depth / whether a transition is in progress / current transition type |
| `s_board.nav.warp.progress_q8` | Warp progress (0 = unchanged, 256 = sucked into a single point) |
| `s_board.nav.transitions` / `req_dropped` | Completed transitions / rejected navigation requests |
| The page's `steps` / `frames` / `paused` | Logical steps / frames received by that page / whether it is covered |
| Reader functions for the various counters | `cgl_screen_fps()`, `cgl_screen_dropped_events()`, `cgl_page_nav_depth/transitions/dropped()`, `cgl_widget_alpha_dropped()`, `cgl_widget_children_dropped()`, `cgl_bar_range_rejected()`, `cgl_arc_range_rejected()`, `cgl_bounce_bounces()`, `cgl_bounce_area_rejected()`, `cgl_debris_dot_count()`, `cgl_debris_active_dots()`, `cgl_particles_active()`, `cgl_particles_max_active()`, `cgl_particles_tick()`, `cgl_watch_frames()` |

### 18.6 Resource Generation Tools

**Glyph tables** (TTF → `cgl_font_data.c`)

```bash
python tools/mkfont.py --sizes 8x16 12x24 --threshold 45 \
    --out src/core/cgl_font_data.c --license-out licenses/LICENSE_DEJAVU.txt
python tools/mkfont.py --dry-run          # preview only, write no files
```

Parameters: `--sizes` (list of `width x height`, default `8x16 12x24`), `--font` (input TTF, defaults to auto-locating DejaVu Sans Mono), `--threshold` (coverage percentage; lower means thicker strokes, default 45), `--out` (default `src/core/cgl_font_data.c`), `--license-out` (default `licenses/LICENSE_DEJAVU.txt`), `--dry-run`. The defaults already point at the real locations in this repository, so the two path arguments above may be omitted; they are written out only to make the destination explicit. It generates 95 ASCII glyphs (`0x20..0x7E`), 4× supersampled and then box-downsampled, with bit order **bit0 = leftmost pixel**; after writing it reads the data back and verifies the dimensions, the byte count and the set-bit count, exiting with code 1 on any mismatch.

**Dot-matrix logos** (text or image → dot-matrix header)

```bash
python tools/mklogo.py --text "CytStudio" --font tools/DelaGothicOne-Regular.ttf \
    --size 160 --tracking 2 --align left --dot-threshold 40 --pad-x 12 --out App/logo_bitmap.h
python tools/mklogo.py --image logo.png --invert
python tools/mklogo.py --text "SMASH" --dry-run
```

It outputs `logo_bitmap.h`, containing `LOGO_GRID_W`, `LOGO_GRID_H`, `LOGO_ROW_BYTES`, `LOGO_DOT_COUNT`, `logo_bitmap[]` and `logo_bitmap_get()`. The bit order is **MSB first**, **opposite** to `cgl_font_data.c`; do not mix them. The tool reads the constraints from the firmware source (panel dimensions, `CGL_PHYS2D_MAX_BODIES`, etc.), shrinks the grid with a step of 0.94 when over budget, and performs a read-back verification.

**Particle tables** (generate the sine table and the reciprocal table)

```bash
python tools/mkparticletables.py --out include/ext/cgl_particles_tables.h \
                                --src src/ext/cgl_particles_tables.c
```

`--out` must be given explicitly; `--src` defaults to being derived from the mirrored tree (`include/ext/x.h` → `src/ext/x.c`). It generates `cgl_particles__sin_q8[64]` and `cgl_particles__recip[384]` (578 bytes of Flash in total), and both are range-checked.

**Watch default tables** (C program, pasted manually after generation)

```bash
cc -std=c11 -DCGL_CFG_WATCH_DIAL_LUT=0 -Iinclude tools/mkwatchlut.c \
   src/core/cgl_arc.c src/core/cgl_watch.c src/core/cgl_math.c \
   src/core/cgl_canvas.c src/core/cgl_widget.c src/core/cgl_anim.c \
   src/core/cgl_font.c src/core/cgl_font_data.c \
   -o build/mkwatchlut && build/mkwatchlut
```

It takes no arguments and prints the two arrays `cgl_watch_face_lut_default[CGL_ARC_FACE_LUT_N]` and `cgl_watch_well_lut_default[CGL_ARC_RING_LUT_N]` to stdout; paste them over the corresponding arrays in `src/core/cgl_watch_dial.c`. `-DCGL_CFG_WATCH_DIAL_LUT=0` is required (otherwise the default configuration points at the very two arrays being generated). It is merely a wrapper around the framework's own `cgl_arc_surface_bake()`, and `watch_test` re-checks it item by item at runtime with the same baker, so that "the table drifting from its generator" cannot happen.

### 18.7 Build System Pitfalls

1. **Include path rules**: do not add `src/core` and `include/` to the global include path at the same time (public headers and private headers share names). Framework sources include their own headers by plain file name, so a single root `include/` suffices, and it serves both `<cytgl/...>` and `<ext/...>`. Do not add `-I include/ext` or `-I include/cytgl` separately either.
2. **Collecting source files by directory is non-recursive, and moving a file fails silently**. In a build driven by a directory list (IDE project files, firmware build scripts), if a `.c` file is moved out of a listed directory the build produces **no error at all**; the linker simply no longer sees it. The entry points that must be changed together are: the top-level `CMakeLists.txt`, the source paths and include paths of the IDE project file, the firmware build script, `tests/CMakeLists.txt`, and the logo generation tool. The same applies to headers (module sources depend on `include/ext/` via `<ext/...>`). **The detection method is a byte-for-byte `size` comparison, not "the build succeeded"**.
3. **Macros that must be consistent across compilation units**: `CGL_CFG_PARTICLES` / `CGL_CFG_DEBRIS` / `CGL_CFG_BOUNCE`, `CGL_PHYS2D_USER_STORAGE` (changes the struct layout), `CGL_CFG_ALPHA*`, `CGL_CFG_SCREEN_W/H`. Put them into the build system, not into a local definition inside some file.
4. **Compile-time assertions**: `cytgl_config.h` validates the alpha level count and budget and the dependency relationship between the watch and the radial toolbox; `cgl_phys2d.h` validates the reference geometry anchors; `cgl_iso2d.h` and the bridge validate the sort-table capacity; the particles and the gravity ball each validate their own dimensions and parameters (for example, a restitution above 256 triggers `#error`). The purpose of these assertions is to turn "a parameter set to an impossible value" into a build error instead of "the effect looks a bit odd".
5. **Numerical consistency**: `CGL_PHYS2D_MAX_BODIES` and `CGL_ISO2D_MAX_POINTS` must match; `CGL_CFG_SCREEN_W/H` must agree with the geometry table of the panel driver, the validation in `cgl_screen_init()` and the bitmap generation tool; `CGL_CFG_CLOCK_STEP_MS` determines the physics speed; font size and band height affect the page layout and the logo viewport derivation.
6. The `text` section size is **path-dependent** (the HAL source embeds `__FILE__`), so comparing firmware size across directories is meaningless; only different configurations within the same directory can be compared.

### 18.8 Firmware Build, Flashing, and Debugging

**Build settings of the reference firmware**

| Item | Value |
|---|---|
| Compiler | `arm-none-eabi-gcc` 14.3 (Arm GNU Toolchain, hard float, `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16`) |
| Optimization | `-O2` |
| Warnings | `-Wall -Wextra -Wpedantic -Wcast-align`, targeting **0 errors / 0 warnings** |
| Other | `-std=c11`, `-fno-common`, `-fno-strict-aliasing`, `-ffunction-sections`, `-fdata-sections` |
| Macros | `USE_HAL_DRIVER`, `STM32G431xx` |
| Artifacts | `build/CytGL_Demo.elf` / `.bin` / `.hex` / `.srec` / `.map` |

Source paths and include paths are both **directory lists**: the generated code (`Core/Startup`, `Core/Src`, the HAL driver sources), the project directory and its `BSP`, the code shared by the two examples under `../common/{App,BSP,Devices}`, and the framework's `src/core`, `src/render`, `src/port` plus the optional module's `src/ext`. There is exactly one `include/` root on the include path, and it serves both `<cytgl/...>` and `<ext/...>`. `tests/` and `tools/` **take no part in the firmware build**; they are host-side assets.

**Three build modes**

| Mode | Purpose |
|---|---|
| `translate` | Single-file syntax check, the fastest; the first gate after editing a line of code |
| `build` | Incremental build |
| `rebuild` | Full build; use it after changing include paths, macros or the linker script |

Command-line equivalents:

```bash
arm-none-eabi-gcc ... -c <each source file>        # the file list comes from the project's source paths
arm-none-eabi-gcc ... -T <linker script> -o build/CytGL_Demo.elf
arm-none-eabi-size build/CytGL_Demo.elf            # inspect flash / RAM usage
```

**Flashing and debugging**

```
1. Build                          -> produces build/CytGL_Demo.elf
2. Flash the .elf over SWD        -> download
3. Open a debug session (reset and stop at the entry point)  -> attach / start session
4. Run                            -> run
```

Typical debug configuration (the reference project): interface `SWD`, driver `DAPLink` (J-Link also works), speed 40000 kHz, run to `main` on start, and load the chip's SVD file for the peripheral register view.

**When locating a problem on hardware**, break where the symptom appears and read four numbers first: whether `frame_id` is increasing (is the bus really pushing), whether the increment of `bands` is 43 (a whole screen), whether `dt_ms` is being clamped, and whether `steps` is 1 (a value greater than 1 means this frame hitched and the clock ran catch-up steps). Section 18.5 lists the complete set of observation points.

---

## 19. Complete Example: A Runnable Application Skeleton

This section presents a complete "from scratch to usable" skeleton: board-level port + pages + menu + main loop. It omits the initialization details of the chip peripherals (those belong to the code generated by CubeMX); the remaining parts may be copied verbatim.

### 19.1 The Board-Level Port (`cgl_port.c`)

```c
#include "cgl_port.h"                 /* This board project's own header file; do **not** let a header file of the same name conflict with the framework's cgl_port.h */
#include <cytgl/cytgl.h>
#include <cytgl/cgl_port.h>

/* ---- 1. Panel driver: implements cgl_display_drv_t ---- */
static int  panel__init(void *ctx);
static void panel__set_window(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
static void panel__write_pixels(void *ctx, const cgl_color_t *px, uint32_t count);
static void panel__wait_idle(void *ctx);
static void panel__set_backlight(void *ctx, uint8_t percent);

static const cgl_display_drv_t s_panel_drv = {
    .name = "my-panel",
    .init = panel__init,
    .set_window = panel__set_window,
    .write_pixels = panel__write_pixels,   /* must be asynchronous: return immediately after starting the transfer */
    .wait_idle = panel__wait_idle,         /* block until the transfer completes, then release CS   */
    .set_backlight = panel__set_backlight, /* may be NULL                  */
    .width = CGL_CFG_SCREEN_W,             /* must match the compile-time configuration           */
    .height = CGL_CFG_SCREEN_H,
};

/* ---- 2. Clock: monotonic milliseconds ---- */
static uint32_t port__tick_ms(void) { return (uint32_t)HAL_GetTick(); }

/* ---- 3. Input: scan the keys and only post events ---- */
static void port__poll_input(void *user, uint32_t dt_ms);

/* ---- 4. Power-on hook: pins, bus clock, backlight PWM ---- */
static int port__board_init(void *user) { board_lcd_init(); return 0; }

static const cgl_port_t s_port = {
    .name = "my-board",
    .display = &s_panel_drv,
    .display_ctx = &g_panel,
    .tick_ms = port__tick_ms,
    .poll_input = port__poll_input,
    .input_ctx = NULL,
    .board_init = port__board_init,
    .board_ctx = NULL,
};

/* ---- 5. Board-level public interface ---- */
static cgl_port_ctx_t s_ctx;
static cgl_page_nav_t s_nav;

int cgl_port_board_init(void)
{
    /* The application owns the navigator itself: pages must be registered on a known navigator */
    cgl_page_nav_init(&s_nav, "pages",
                      cgl_rect_wh(0, 0, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H));

    if (cgl_port_bind(&s_port, &s_ctx, &s_nav) != 0) { return -1; }
    board_key_init();
    return 0;
}

void cgl_port_board_frame(void) { (void)cgl_port_frame(&s_ctx); }

cgl_display_t *cgl_port_board_display(void) { return cgl_port_display(&s_ctx); }
cgl_screen_t *cgl_port_board_screen(void) { return cgl_port_screen(&s_ctx); }
cgl_page_nav_t *cgl_port_board_nav(void) { return &s_nav; }
uint32_t cgl_port_board_tick_ms(void) { return port__tick_ms(); }
bool cgl_port_board_post_event(const cgl_event_t *ev) { return cgl_port_post_event(&s_ctx, ev); }
void cgl_port_board_set_backlight(uint8_t percent) { cgl_port_set_backlight(&s_ctx, percent); }
```

**Never** copy the `display` / `screen` members out of `cgl_port_ctx_t`: that duplicates roughly 5 KB of band buffers and creates a second damage sink. Always access them through `cgl_port_display()` / `cgl_port_screen()`.

The project's own `cgl_port.h` **must use its own include guard macro** (for example `CGL_BOARD_PORT_H`); do not use `CGL_PORT_H` — the latter belongs to the framework, and a clashing name causes `<cytgl/cgl_port.h>` to expand to nothing, whereupon `cgl_port_t` / `cgl_port_ctx_t` disappear.

### 19.2 Input: One Long Press Becomes Two Events

```c
static void port__poll_input(void *user, uint32_t dt_ms)
{
    (void)user;
    board_key_event_t k = board_key_poll(dt_ms);   /* debouncing and long-press timing use the passed-in dt */
    cgl_event_t ev = { 0 };

    if (k == BOARD_KEY_NONE) { return; }

    ev.key = 0u;
    ev.hold_ms = board_key_hold_ms();

    switch (k)
    {
    case BOARD_KEY_DOWN:   ev.kind = (uint8_t)CGL_EV_KEY_DOWN;   break;
    case BOARD_KEY_REPEAT: ev.kind = (uint8_t)CGL_EV_KEY_REPEAT; break;
    case BOARD_KEY_UP:     ev.kind = (uint8_t)CGL_EV_KEY_UP;     break;
    case BOARD_KEY_LONG:
    default:               ev.kind = (uint8_t)CGL_EV_KEY_LONG;   break;
    }
    (void)cgl_port_post_event(&s_ctx, &ev);

    /* A long press additionally emits a "back": a page that consumes KEY_LONG treats it as confirmation,
       while a page that does not consume it lets the subsequent BACK reach the navigator and pop the stack */
    if (k == BOARD_KEY_LONG)
    {
        ev.kind = (uint8_t)CGL_EV_BACK;
        (void)cgl_port_post_event(&s_ctx, &ev);
    }
}
```

The key state machine is driven entirely by the passed-in `dt_ms` (the reference implementation uses "duration" rather than a sample count): the raw level must be stable for `BOARD_KEY_DEBOUNCE_MS` (20 ms) before it is accepted; while the key is held the duration accumulates, and upon reaching `BOARD_KEY_LONG_MS` (600 ms) one long press is emitted, after which a repeat is emitted every `BOARD_KEY_REPEAT_MS` (200 ms). A short press is therefore not decided until `UP`.

### 19.3 Pages and the Page Table

```c
/* ---------- Page storage: all static; a page owns its own widgets ---------- */
typedef struct
{
    cgl_page_t  base;
    cgl_panel_t root;
    cgl_label_t title;
    cgl_label_t hint;
} app_page_home_t;

/* The second page is isomorphic: the first member is cgl_page_t, the rest are this page's widgets */
typedef struct
{
    cgl_page_t  base;
    cgl_panel_t root;
    cgl_label_t text;
} app_page_second_t;

typedef struct
{
    app_page_home_t   home;
    app_page_second_t second;
} app_storage_t;

static app_storage_t  s_app;
static cgl_page_ctx_t s_ctx;

/* ---------- One page ---------- */
static void home__create(cgl_page_t *page)
{
    app_page_home_t *p = (app_page_home_t *)page;
    const cgl_page_ctx_t *ctx = cgl_page_ctx(page);

    (void)ctx;

    cgl_panel_init(&p->root, "home.root",
                   cgl_rect_wh(0, 0, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H),
                   CGL_CFG_BG_COLOR, CGL_CFG_BG_COLOR);
    (void)cgl_widget_add_child(&p->root.base, &p->title.base);   /* the order is the draw order */
    (void)cgl_widget_add_child(&p->root.base, &p->hint.base);

    cgl_page_set_root(page, &p->root.base);
}

static void home__attach(cgl_page_t *page, const cgl_page_ctx_t *ctx)
{
    app_page_home_t *p = (app_page_home_t *)page;

    cgl_page_init(page, &s_home_vt, "home", (void *)ctx);

    cgl_label_init(&p->title, "home.title", cgl_rect(16, 20, 260, 44),
                   &cgl_font_12x24, "CytGL", CGL_CFG_FG_COLOR, CGL_CFG_BG_COLOR, false);
    cgl_label_init(&p->hint, "home.hint", cgl_rect(16, 60, 300, 76),
                   &cgl_font_8x16, "long press: next", CGL_CFG_ACCENT_COLOR,
                   CGL_CFG_BG_COLOR, false);
}

static bool home__event(cgl_page_t *page, const cgl_event_t *ev)
{
    app_page_home_t *p = (app_page_home_t *)page;

    if (ev->kind == (uint8_t)CGL_EV_KEY_LONG)
    {
        /* Only register the request; it takes effect at the frame boundary in cgl_page_nav_frame() */
        (void)cgl_page_nav_request(p->base.nav, CGL_NAV_PUSH,
                                   (cgl_page_t *)&s_app.second, CGL_TRANS_AUTO, NULL);
        return true;
    }
    return false;   /* BACK is left to the navigator */
}

static const cgl_page_vt_t s_home_vt = {
    .type = "home",
    .on_create = home__create,
    .on_event = home__event,
};
```

```c
/* ---------- Page table: adding a page = one line ---------- */
static const cgl_page_desc_t s_pages[] = {
    { "home",   (cgl_page_t *)&s_app.home,   home__attach   },
    { "second", (cgl_page_t *)&s_app.second, second__attach },
};

int app_init(void)
{
    cgl_screen_t *screen;

    if (cgl_port_board_init() != 0) { return -1; }

    screen = cgl_port_board_screen();
    s_ctx.screen = screen;
    s_ctx.nav = cgl_port_board_nav();
    s_ctx.app = NULL;                 /* Page interlink table (if any) */

    (void)cgl_page_register(s_pages,
                            (uint8_t)(sizeof(s_pages) / sizeof(s_pages[0])), &s_ctx);

    /* Home page: RESET has no transition, so the first frame is already the stable main interface */
    (void)cgl_page_nav_reset(cgl_port_board_nav(), (cgl_page_t *)&s_app.home);

    (void)cgl_port_board_frame();     /* One frame: commits RESET and draws the whole screen */
    cgl_port_board_set_backlight(100u);
    return 0;
}

void app_run(void)
{
    uint32_t last = cgl_port_board_tick_ms();

    for (;;)
    {
        uint32_t now = cgl_port_board_tick_ms();

        if (now == last) { continue; }     /* advance one frame per millisecond */
        last = now;
        cgl_port_board_frame();
    }
}
```

`app_run()` performs tick control only: it does not compute dt, does not clamp (stutter protection is the clock's responsibility, because the clock must also protect the fixed-step accumulator), and includes no HAL header file — it asks the **port** for the time.

### 19.4 Power-On Sequence

There is only one correct power-on sequence for the porting layer and the framework:

1. Chip and peripheral initialization (clock, pins, SPI, DMA, optional PWM), provided by generated code;
2. `app_init()` → `cgl_port_board_init()`:
   a. Initialize the navigator with the full-screen rectangle (when the application holds the navigator itself);
   b. `cgl_port_bind()` → runs the `board_init` hook, binds the display driver (fails if the geometry does not match), creates the screen, and installs the damage sink, in that order;
   c. Reset the key state machine;
3. `cgl_page_register()` creates the pages from the table (at this point nothing is pushed onto the stack and `on_create` does not run);
4. `cgl_page_nav_reset()` installs the home page (this step runs the first `on_create`);
5. **Advance one frame**, so that RESET and the first-screen drawing take effect;
6. Turn on the backlight — placing it at this step prevents the user from seeing the reset and initialization sequence.

### 19.5 Common Patterns of Page Navigation

- **Navigation can only be triggered by events**: there is no idle timer and no scripting. A page registers its request in its own `on_event` and uses `CGL_TRANS_AUTO` to leave the transition to be declared by the target page itself.
- A **menu page** typically maintains a name table and a target resolution function that maps an index to a page pointer; the target is resolved from `ctx->app` (the interlink table), so "this build does not contain that page" is a legal state (the menu item is displayed but does nothing).
- **Back** does not need to be implemented by every page: `CGL_EV_BACK` pops the stack directly from the navigator when the stack depth is greater than 1. A board that needs "long-press back" only has to translate the long press into a pair of `KEY_LONG` + `BACK` in the porting layer.
- An **overlay** (a semi-transparent card, an information page) requires both steps: `cgl_page_set_show_below(page, true)` plus `cgl_widget_set_alpha(&root.base, a)`.

---

## 20. Troubleshooting Quick Reference

### 20.1 Display and Porting

| Symptom | Possible cause |
|---|---|
| `cgl_port_bind()` fails immediately | The `width/height` reported by the driver do not match `CGL_CFG_SCREEN_W/H`; or `board_init` / the driver's `init` returns non-zero; or the navigator area is not the full screen |
| The image is uniformly offset | The visible area of the panel lies inside a larger GRAM; the offset must be added in `set_window` |
| The image tears, with a moving horizontal seam | `write_pixels` is in fact synchronous, or CS is released too early |
| Wrong colors / swapped channels | Byte order: RGB565 is in native order; check whether a redundant byte swap is performed, and whether `COLMOD` is 16-bit |
| Colors are inverted | IPS panels usually require `INVON` |
| The whole screen is a single color (usually the background color) | The damage region is empty: no widget called `cgl_widget_invalidate()`. Call `cgl_screen_redraw_all()` |
| Only the top rows are drawn | Geometry mismatch: the driver reports portrait while the configuration is landscape (or the reverse) |
| The frame rate is about half the bus capability | `write_pixels` executes synchronously, so the CPU and the bus no longer overlap |
| About one band is missing, or the image jumps | The buffer handed to `write_pixels` is reused before `wait_idle()`: the DMA has not actually finished |
| The first frame hangs, `frame_id` stays at 1 | The SPI DMA request/channel configuration is wrong |
| Random horizontal stripes | A band in some push was not fully redrawn (a band pushed to the panel must be complete) |
| The backlight does not light | The initial backlight level, whether `set_backlight` is `NULL`, or the PWM channel not being started |
| A key must be pressed for an unusually long time | 20 ms debounce + 600 ms long press; a short press is not decided until `UP` |
| A compile-time `_Static_assert` reports `CGL_CFG_ALPHA_BUDGET` | The RAM required by group opacity exceeds the budget: reduce `CGL_CFG_ALPHA_LEVELS` / `CGL_CFG_BAND_H`, or intentionally raise the budget |
| Linking fails with a missing `cgl_arc_*` / `cgl_watch_*` symbol | `CGL_CFG_WATCH=1` while `CGL_CFG_ARC=0` (normally this should be caught by an assertion); or the source list omits `src/core/cgl_arc.c` / `cgl_watch.c` / `cgl_watch_dial.c` |

### 20.2 Drawing

| Symptom | Possible cause |
|---|---|
| Garbled text | The glyph bit order is read reversed: the framework requires bit0 = leftmost pixel, whereas dot-matrix logo header files are MSB-first, the two being opposite |
| In a panned subtree the rectangles move but the text does not | The fast path of `cgl_canvas_row()` is used but adding `pan` manually was forgotten |
| Semi-transparency has no effect | There is no scratch area (`CGL_CFG_ALPHA=0`) or the nesting levels are exhausted. Check `cgl_widget_alpha_dropped()` |
| A semi-transparent layer shows the background instead of the page below | The overlay lacks `cgl_page_set_show_below(page, true)` |
| A semi-transparent region does not refresh and retains stale content | The widget cleared `CGL_WF_CLIP` and drew outside its frame; after changing the alpha it must mark the damage again itself |
| A widget is not displayed | The parent's child count reached `CGL_CFG_WIDGET_MAX_CHILDREN` and `cgl_widget_add_child()` returned false. Check `cgl_widget_children_dropped()` |
| Content from the previous frame remains | The widget does not fill its own frame (the framework does not clear the background) |
| Anti-aliased edges show hard edges or "pixels that are not fully covered" | A 1-pixel-wide anti-aliased stroke has no fully covered pixel; when a solid appearance is required, request 2 pixels |
| The stroke overruns the box at both ends | Round end caps make the stroke extend by `thickness / 2` pixels at each end; the geometry must be shortened accordingly |

### 20.3 Pages, Transitions, and Time

| Symptom | Possible cause |
|---|---|
| A piece of the image is missing after a page switch | The widget tree was modified outside `cgl_page_nav_frame()` |
| During Genie suck-in only the upper part of the page remains | Culling used the source frame instead of the conservative bounding box of `cgl_canvas_map_rect()` |
| Stray pixels appear outside the funnel outline | Clipping must be performed in source space (`src_clip`) |
| The first frame of the warp jitters | Wrong rounding: rows are floored and columns are rounded |
| The suck-in transition lasts only a few frames / is extremely slow | A custom row-scanning renderer did not bind its row range to the preimage of the band (use `cgl_canvas_src_window()`) |
| Keys have no effect during a transition | Input is locked and events are retained for delivery after the transition ends; they are not lost |
| Multiple navigations in the same frame take effect only once | This is by design: among the requests in the same frame only the last takes effect |
| A covered page is still stepping | Check whether it really is the top of the stack; only `cgl_page_nav_top()` receives `on_step` |
| After going back the physics suddenly advances several hundred steps | Catch-up occurred: verify that the world is stepped in exactly one place and that the clock is not bypassed |
| The page switch has no animation at all | The caller explicitly passed `CGL_TRANS_NONE` (which overrides the page's declaration), or the duration of that transition type is 0 |
| A transition declared on the page has no effect | `dur_ms` of 0 means "not declared"; to express an instantaneous switch, `trans = CGL_TRANS_NONE` must be declared |
| The image is mirrored after configuring `OUT_BACK` for suck-in | `OUT_BACK` makes the progress exceed 256, while Genie takes the geometric warp path; only slide-type transitions may use it |
| The watch hands do not move | The watch's vtable `update` is `NULL`; the page must call `cgl_watch_frame(face, clk)` in `on_frame` |
| Time stops after the watch is covered | This should not happen: the time comes from `clk->now_ms`. If it does stop, check whether the page is still at the top of the stack (a page that is not at the top does not receive `on_frame`) |
| The hands do not change immediately after `set_time()` | The time setting is staged and takes effect at the beginning of the next `cgl_watch_frame()` |

### 20.4 The Extension Module

| Symptom | Possible cause |
|---|---|
| The logo is blurred / smeared into a single mass | `CGL_DEBRIS_DOT_PX` is larger than the minimum projected dot pitch (the camera style and the dot radius must be adjusted as a pair) |
| The dot matrix disintegrates and then disappears | `CGL_DEBRIS_DOT_R_Q4` is greater than `CGL_DEBRIS_STEP_Q4 / 2` |
| The pseudo-3D is unrecognizable | The camera style and the dot radius do not match (under `ISO_UNIFORM` the minimum dot pitch is about 2.16 pixels) |
| The ball passes through the dot matrix | The broad-phase grid and the world size were changed without re-deriving the consistency gate |
| A rigid body is silently missing | The pool capacity is insufficient: `cgl_phys2d_add()` returned -1. Or `CGL_ISO2D_MAX_POINTS` is smaller than the actual number of elements |
| The occlusion order is wrong on the first frame | `cgl_iso2d_sorter_reset()` was not called after the world was rebuilt |
| The positions changed but the draw order did not | The position change did not update the stamp (`cgl_iso2d_sort()` is a no-op while the stamp is unchanged) |
| The ball covers what is behind it | A second renderer appeared in the sorted screen: a single `cgl_debris_view_draw()` must draw all rigid bodies of the world |
| Particles fly off-screen | The camera is too wide: the upper and lower gates of `CGL_PARTICLES_CAM_EXTENT_Q8`; or the camera dolly amplitude is so large that the depth clamp consumes the far end |
| Particles "fly back around" | The world coordinates exceed the ±128 pixel window of Q8.8 |
| `cgl_particles_init()` returns `NULL` | The storage is smaller than `CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX)`, or its alignment is less than 4 bytes |
| The ball sticks to the ground and stops bouncing | `CGL_BOUNCE_RESTITUTION_Q8` is too low (each bounce loses `1 - e²`) |
| The ball jitters in a corner | The restitution is greater than 256, so every bounce adds energy |
| The ball rises higher at the top of each arc | `vy += g` is applied after the integration (semi-implicit Euler is required) |
| The ball's drawn position does not match its collision position | After the ball is erased by `cgl_phys2d_reset()` it is rebuilt by self-healing; drawing must use the non-self-healing const version `cgl_bounce_draw_position_px()` |
| The module is linked but has no effect | Linking without referencing costs nothing; for the effect to appear it must actually be called, and the engine's storage and the page's storage must truly be identical |

### 20.5 Build and Test

| Symptom | Possible cause |
|---|---|
| The compiler reports an ambiguous symbol such as `cgl_screen.h` | Both `src/core` and `include/` were added to the global include path. Keep only `include/` |
| `<cytgl/cgl_port.h>` expands to nothing and `cgl_port_t` is undefined | The project's own `cgl_port.h` used `CGL_PORT_H` as its include guard macro |
| After moving a source file the firmware "builds successfully" but behaves differently | A build that collects source files by directory is non-recursive; moving a file raises no error. Detect this with a byte-for-byte `size` comparison |
| Multiple translation units disagree about a struct layout | Build-level macros such as `CGL_PHYS2D_USER_STORAGE` / `CGL_CFG_PARTICLES` have different values in different units |
| CTest runs none of the tests | Multi-configuration generators (MSVC) require `--config` and `ctest -C` |
| The snapshot file cannot be found | Snapshots are written to `<build directory>/build/`, whereas the character-art script scans `<repository>/build` by default. Specify it with `-File` or copy it |
| The font was changed but the interface did not change | The generated file is not the one the build actually compiles: confirm that `mkfont.py`'s `--out` points at `src/core/cgl_font_data.c` (which is the default) and that the build really recompiled that file |

---

## Appendix A Public Header and Responsibility Index

| Header | Responsibility | Main identifiers |
|---|---|---|
| `cytgl/cytgl.h` | Single entry point: one include pulls in the entire public API; also defines the version macros | `CGL_VERSION_MAJOR/MINOR/PATCH`, `CGL_VERSION_STRING` |
| `cytgl/cytgl_config.h` | All compile-time switches | `CGL_CFG_*` |
| `cytgl/cgl_types.h` | Colors, points, rectangles, input events, the single blend formula, forced-inline macro | `cgl_color_t`, `cgl_point_t`, `cgl_rect_t`, `cgl_event_t`, `cgl_color_blend_q8()`, `CGL_AINLINE` |
| `cytgl/cgl_math.h` | Integer trigonometry, polar mapping, range scaling, integer square root | `cgl_angle_t`, `cgl_sin_q8()`, `cgl_math_polar()`, `cgl_math_polar_q6()`, `cgl_math_frac_q8()`, `cgl_math_isqrt()` |
| `cytgl/cgl_canvas.h` | Band canvas, primitives, warp descriptor, coverage blending, layered scratch | `cgl_canvas_t`, `cgl_warp_t`, `cgl_canvas_*()` |
| `cytgl/cgl_font.h` | Dot-matrix fonts and text drawing | `cgl_font_t`, `cgl_font_8x16`, `cgl_font_12x24`, `cgl_font_draw_text*()` |
| `cytgl/cgl_widget.h` | Widget tree, flags, pan/warp/group opacity, damage sink, hit testing, event dispatch | `cgl_widget_t`, `cgl_widget_vt_t`, `cgl_widget_*()` |
| `cytgl/cgl_panel.h` | Built-in widget: background/border container | `cgl_panel_t`, `cgl_panel_init()` |
| `cytgl/cgl_label.h` | Built-in widget: text label (fixed-length buffer, self-contained number formatting) | `cgl_label_t`, `cgl_label_set_text/int/fixed1()` |
| `cytgl/cgl_bar.h` | Built-in widget: progress/level bar (arbitrary signed range, two orientations, border, segments, tween) | `cgl_bar_t`, `cgl_bar_*()` |
| `cytgl/cgl_arc.h` | Radial toolbox: coverage, tapered needle, shaded surface, ring gauge, opacity wrapper | `cgl_arc_cov_q6()`, `cgl_arc_stroke_*()`, `cgl_arc_needle*()`, `cgl_arc_surface_*()`, `cgl_arc_gauge_*()`, `cgl_arc_draw_*()` |
| `cytgl/cgl_watch.h` | Built-in widget: analog watch (time base and calendar, tick ring, three hands, hub, comet tail, glass cover) | `cgl_watch_t`, `cgl_watch_config_t`, `cgl_watch_*()` |
| `cytgl/cgl_display.h` | Display driver vtable, band presentation, damage accumulation | `cgl_display_drv_t`, `cgl_display_t`, `cgl_display_present()` |
| `cytgl/cgl_time.h` | The single clock: frame time, fixed-step scheduling, frame rate | `cgl_clock_t`, `cgl_clock_begin/next_step()` |
| `cytgl/cgl_anim.h` | Integer tweens and Q8 easing curves | `cgl_anim_t`, `cgl_ease_t`, `cgl_anim_*()` |
| `cytgl/cgl_page.h` | Page object, lifecycle, navigation stack, transitions, page table | `cgl_page_t`, `cgl_page_vt_t`, `cgl_page_nav_*()`, `cgl_page_register()` |
| `cytgl/cgl_screen.h` | Display + clock + navigator + input queue = one frame | `cgl_screen_t`, `cgl_screen_frame()` |
| `cytgl/cgl_port.h` | Porting contract: port descriptor, framework-side instance, frame entry point | `cgl_port_t`, `cgl_port_ctx_t`, `cgl_port_bind/frame()` |
| `ext/cgl_ext_basis.h` | Shared by the extension modules: Q12.4 convention, forced-inline macro | `CGL_EXT_Q4_SHIFT`, `CGL_EXT_Q4_ONE`, `CGL_EXT_INLINE` |
| `ext/cgl_phys2d.h` | Fixed-point disc rigid-body world (broad phase + solver + 2.5D vertical axis) | `cgl_phys2d_t`, `cgl_phys2d_body_t`, `cgl_phys2d_*()` |
| `ext/cgl_iso2d.h` | Isometric camera and counting-sort depth sorter (knows nothing about rigid bodies) | `cgl_iso2d_cam_t`, `cgl_iso2d_sorter_t`, `cgl_iso2d_view_t`, `cgl_iso2d_*()` |
| `ext/cgl_iso2d_phys2d.h` | The only bridge between the two layers (fully inline) | `cgl_iso2d_phys2d_read/src()`, `cgl_iso2d_sort_bodies()` |
| `ext/cgl_particles.h` | Q8.8 particle cloud, orbiting camera, perspective division table | `cgl_particles_t`, `cgl_particles_cam_t`, `cgl_particles_*()` |
| `ext/cgl_particles_view.h` | Band renderer for the particle cloud | `cgl_particles_view_draw_band()` |
| `ext/cgl_particles_tables.h` | Generated sine and reciprocal tables (do not edit by hand) | `cgl_particles__sin_q8`, `cgl_particles__recip` |
| `ext/cgl_debris.h` | Shatter scene: bitmap → rigid-body cloud, camera fitting, SMASH/SETTLE loop | `cgl_debris_t`, `cgl_debris_*()` |
| `ext/cgl_debris_view.h` | Renderer for the floor, the grid, and depth-sorted rigid bodies | `cgl_debris_view_draw()`, `cgl_debris_palette_t` |
| `ext/cgl_bounce.h` | Gravity ball: gravity, four walls, restitution, self-healing by identity | `cgl_bounce_t`, `cgl_bounce_*()` |
| `ext/cgl_bounce_view.h` | Gravity ball renderer and standalone ball widget | `cgl_bounce_view_draw()`, `cgl_bounce_widget_t` |
| `ext/*_config.h` | Compile-time switches for each layer/effect (split by layer) | `CGL_PHYS2D_*`, `CGL_ISO2D_*`, `CGL_PARTICLES_*`, `CGL_DEBRIS_*`, `CGL_BOUNCE_*`, `CGL_CFG_PARTICLES/DEBRIS/BOUNCE` |

## Appendix B Compile-Time Configuration Macro Index

**Panel and memory**

`CGL_CFG_SCREEN_W`, `CGL_CFG_SCREEN_H`, `CGL_CFG_BAND_H`, `CGL_CFG_ALPHA`, `CGL_CFG_ALPHA_LEVELS`, `CGL_CFG_ALPHA_LEVEL_STRIDE`, `CGL_CFG_ALPHA_BUDGET`, `CGL_CFG_ALPHA_BYTES` (derived), `CGL_CFG_LABEL_MAX_TEXT`, `CGL_CFG_WIDGET_MAX_CHILDREN`, `CGL_CFG_PAGE_STACK_MAX`, `CGL_CFG_INPUT_QUEUE`.

**Radial module**

`CGL_CFG_ARC`, `CGL_CFG_WATCH`, `CGL_CFG_WATCH_DIAL_LUT`, `CGL_CFG_WATCH_TRAIL`, `CGL_CFG_WATCH_GLASS`, `CGL_CFG_WATCH_CX/CY`, `CGL_CFG_WATCH_R_FACE`, `CGL_CFG_WATCH_R_RING`, `CGL_CFG_WATCH_RING_W`, `CGL_CFG_WATCH_R_WELL_IN/OUT`, `CGL_CFG_WATCH_R_TICK_OUT`, `CGL_CFG_WATCH_R_TICK_HOUR_IN/MIN_IN`, `CGL_CFG_WATCH_R_HOUR/MINUTE/SECOND`, `CGL_CFG_WATCH_R_TAIL`, `CGL_CFG_WATCH_R_HUB`, `CGL_CFG_WATCH_R_HOUR_BASE/MINUTE_BASE`, `CGL_CFG_WATCH_HOUR_W_BASE/TIP`, `CGL_CFG_WATCH_MINUTE_W_BASE/TIP`, `CGL_CFG_WATCH_SECOND_W`, `CGL_CFG_WATCH_SHADOW_PX/SHADOW_ALPHA`, `CGL_CFG_WATCH_TICK_MS`, `CGL_CFG_WATCH_REVEAL_MS`, `CGL_CFG_WATCH_PULSE_MS`, `CGL_CFG_WATCH_TRAIL_MS`, `CGL_CFG_WATCH_SECOND_MODE`. Structural constants: `CGL_ARC_FACE_LUT_N`, `CGL_ARC_RING_LUT_N`, `CGL_ARC_COV_N`, `CGL_WATCH_TRAIL_MARKS`, `CGL_CANVAS_INK_OPAQUE`, `CGL_ALPHA_OPAQUE`.

**Time**

`CGL_CFG_CLOCK_STEP_MS`, `CGL_CFG_CLOCK_MAX_STEPS`, `CGL_CLOCK_DEFAULT_MAX_DT_MS`, `CGL_CLOCK_MS_PER_S`.

**Pages and transitions**

`CGL_CFG_TRANSITION_MS`, `CGL_CFG_GENIE_MS`, `CGL_CFG_TRANSITION_EASE`, `CGL_CFG_GENIE_OUT_EASE`, `CGL_CFG_GENIE_IN_EASE`, `CGL_CFG_S_OUT_EASE`, `CGL_CFG_S_IN_EASE`, `CGL_CFG_GENIE_NARROW_Q8`, `CGL_CFG_GENIE_DROP_PX`, `CGL_CFG_S_AMP_MAX_Q8`, `CGL_CFG_S_DROP_PX`, `CGL_PAGE_FX_KEEP_EASE`, `CGL_PAGE_FX_KEEP_MS`.

**Palette**

`CGL_CFG_BG_COLOR`, `CGL_CFG_FG_COLOR`, `CGL_CFG_ACCENT_COLOR`.

**Extension module**

Physics: `CGL_PHYS2D_SCREEN_W/H`, `CGL_PHYS2D_WORLD_W_PX/H_PX`, `CGL_PHYS2D_WORLD_X0_PX/Y0_PX`, `CGL_PHYS2D_CELL_SHIFT`, `CGL_PHYS2D_MAX_BODIES`, `CGL_PHYS2D_MAX_VELOCITY_PX`, `CGL_PHYS2D_GRAVITY_Z_PX`, `CGL_PHYS2D_LAUNCH_MIN_Q4`, `CGL_PHYS2D_LAUNCH_GAIN_Q8`, `CGL_PHYS2D_LAND_REST_Q8`, `CGL_PHYS2D_LAND_MIN_Q4`, `CGL_PHYS2D_USER_STORAGE`.
Projection: `CGL_ISO2D_DEPTH_BUCKETS`, `CGL_ISO2D_MAX_POINTS`, `CGL_ISO2D_COS30_Q8`, `CGL_ISO2D_SIN30_Q8`, `CGL_ISO2D_SQRT3_Q8`, `CGL_ISO2D_LIFT_Q8`.
Particles: `CGL_CFG_PARTICLES`, `CGL_PARTICLES_SCREEN_W/H`, `CGL_PARTICLES_MAX`, `CGL_PARTICLES_BUCKETS`, plus the engine constants `CGL_PARTICLES_GRID_W/H`, `CGL_PARTICLES_CELL_PITCH_Q8`, `CGL_PARTICLES_WORLD_HALF_SPAN_Q8`, `CGL_PARTICLES_NEAR_Q8/FAR_Q8`, `CGL_PARTICLES_TRIGGER_TICKS`, `CGL_PARTICLES_CAM_PITCH_A/DIST_PX/FOCAL_PX/DOLLY_Q8`, `CGL_PARTICLES_DOT_SPAN_Q8/DOT_BASE_Q8`, `CGL_PARTICLES_STORAGE_BYTES(n)`.
Debris: `CGL_CFG_DEBRIS`, `CGL_DEBRIS_SCREEN_W/H`, `CGL_DEBRIS_STEP_Q4`, `CGL_DEBRIS_DOT_R_Q4`, `CGL_DEBRIS_DOT_PX`, `CGL_DEBRIS_BALL_R_Q4`, `CGL_DEBRIS_BALL_SPEED_Q4`, `CGL_DEBRIS_BALL_PX`, `CGL_DEBRIS_BALL_START_X_Q4`, `CGL_DEBRIS_BALL_START_Y_OFFSET_Q4`, `CGL_DEBRIS_SETTLE_MS`, `CGL_DEBRIS_EXIT_MARGIN_PX`, `CGL_DEBRIS_SCENE_STYLE`, `CGL_DEBRIS_SCENE_MARGIN_PX`, `CGL_DEBRIS_FLOOR_STEP_PX`, `CGL_DEBRIS_SHADOW_MIN_Z_Q4`.
Gravity ball: `CGL_CFG_BOUNCE`, `CGL_BOUNCE_RADIUS_PX`, `CGL_BOUNCE_GRAVITY_Q4`, `CGL_BOUNCE_RESTITUTION_Q8`, `CGL_BOUNCE_SPAWN_INSET_PX`, `CGL_BOUNCE_SPAWN_VX_Q4`, `CGL_BOUNCE_SPAWN_VY_Q4`, `CGL_BOUNCE_DRAW_R_PX`.

**Fixed-point formats**

`CGL_EXT_Q4_SHIFT` (4), `CGL_EXT_Q4_ONE` (16), `CGL_PHYS2D_SHIFT` / `CGL_PHYS2D_ONE` (synonyms), `CGL_PARTICLES_SHIFT` (8), `CGL_PARTICLES_ONE` (256), `CGL_PARTICLES_PX(v)`, `CGL_PARTICLES_MUL(a,b)`, `CGL_PARTICLES_ANGLE_FULL`, `CGL_ANGLE_TURN`, `CGL_ANGLE_QUARTER/HALF/UP`, `CGL_ANGLE_DEG()`, `CGL_ANGLE_FROM_U8()` / `CGL_ANGLE_TO_U8()`, `CGL_ANIM_ONE_Q8`.

## Appendix C Observable Counter Index

| Read function | Meaning |
|---|---|
| `cgl_screen_fps(s)` | Frame rate of the most recent statistics window (0 for the first 500 ms) |
| `cgl_screen_dropped_events(s)` | Number of events dropped because the input queue was full |
| `cgl_page_nav_depth(n)` | Page stack depth (1 = only the home page) |
| `cgl_page_nav_busy(n)` | Whether a transition is playing (input is locked) |
| `cgl_page_nav_pending(n)` | Whether there is a navigation request not yet committed |
| `cgl_page_nav_transitions(n)` | Number of completed transitions (one key press should add 1) |
| `cgl_page_nav_dropped(n)` | Number of navigation requests rejected because the target was invalid (non-zero = caller defect) |
| `cgl_widget_alpha_dropped()` | Cumulative count of "alpha was set but had no effect" (unit: per band × per traversal); normally 0 |
| `cgl_widget_children_dropped()` | Number of `add_child()` calls rejected because the parent's child count exceeded the limit; normally 0 |
| `cgl_bar_range_rejected()` | Number of rejected bar range settings; normally 0 |
| `cgl_arc_range_rejected()` | Number of rejected gauge range settings; normally 0 |
| `cgl_display_t::frame_id` | Number of frames pushed to the panel (**not** the frame rate) |
| `cgl_display_t::bands` | Cumulative number of bands pushed (one full screen is `CGL_CFG_SCREEN_H / CGL_CFG_BAND_H` rounded up) |
| `cgl_clock_t::steps` / `step_total` / `accum_ms` | Logical steps in this frame / cumulative logical steps (not cleared by a page change) / fixed-step remainder |
| `cgl_particles_active(s)` / `max_active(s)` / `tick(s)` | Current number of live particles / peak of this round / logical steps run this round |
| `cgl_debris_dot_count(d)` / `active_dots(d)` / `ball_index(d)` | Dots placed by the last build / dots still inside the simulation domain / index of the heavy ball in the pool |
| `cgl_bounce_bounces(b)` / `area_rejected(b)` / `max_speed_q4(b)` | Cumulative wall hits / number of rejected areas / historical maximum speed |
| `cgl_watch_frames(w)` | Number of frame calls received by the watch |
| `cgl_watch_reveal_value(w)` / `pulse_value(w)` | Entry progress Q8 / minute-flash intensity Q8 |
| `cgl_arc_gauge_tip(g)` / `sweep(g)` | Current sweep end point / current sweep length (in angle units) |

---

## Appendix D Quick Reference: Where to Change What

A single table answering "I want to change X — where do I go?". Paths are repository paths; `examples/common/App/` is the example application (it demonstrates application-level practice and is not part of the framework).

| I want to change | Go to |
|---|---|
| Default foreground / background / accent colours | `include/cytgl/cytgl_config.h` (`CGL_CFG_BG_COLOR` and the others), plus the colour macros inside each page's `*_create()` |
| Screen size / band height / stack depth / transition duration and easing / clock step | `include/cytgl/cytgl_config.h` |
| The transition of **one page** (type / curve / duration) | That page's own `*_attach()`: `cgl_page_set_trans_enter/leave()` (`RESET` is the exception: the framework forces an instant cut) |
| A page's layout and text | That page's own `*_create()` / `*_frame()` |
| Navigation graph and transition types | `cgl_page_nav_request()` from the page's `on_event`; the menu's index-to-page mapping lives in `examples/common/App/app_page_menu.c` and the explicit entry points in `App/app_ui.c` |
| Adding a widget | Section 16.1 |
| Adding a page | Section 13.7: one `*_attach()` function, one object member, one row in the page table |
| Physics parameters / dot-matrix geometry | `include/ext/cgl_phys2d_config.h` (overridable `#ifndef`s) and the geometry knobs at the top of `include/ext/cgl_debris_config.h` |
| Camera and pseudo-3D | `include/ext/cgl_iso2d_config.h` (`CGL_ISO2D_*`) and `CGL_DEBRIS_SCENE_STYLE` |
| Particle effect: pool size, sort buckets, colour | `include/ext/cgl_particles_config.h` (`CGL_PARTICLES_MAX`, `CGL_PARTICLES_BUCKETS`) and the camera/simulation knobs in the engine header; the colour is an argument of `cgl_particles_view_draw_band()` |
| Shatter effect: grid, wrecking ball, settle time, camera fit, palette | `include/ext/cgl_debris_config.h` (`CGL_DEBRIS_*`); the palette is an argument of `cgl_debris_view_draw()` |
| Gravity ball: radius, gravity, restitution, spawn | `include/ext/cgl_bounce_config.h` (`CGL_BOUNCE_*`). **Where** the ball may go belongs to the application rather than the module (in the example, `smash__ball_area()`) |
| **Not compiling** an effect (saves flash) | `-DCGL_CFG_PARTICLES=0` / `-DCGL_CFG_DEBRIS=0` / `-DCGL_CFG_BOUNCE=0` empties that effect's translation units; a build that links the module normally already pays 0 bytes as long as it does not reference the effect |
| **Not declaring** an effect's storage (saves RAM) | The application-side effect switches (in the example, `examples/common/App/app_fx_config.h`): `APP_FX_PARTICLES=0` drops the bullet-time page; `APP_FX_SMASH=0` drops the shatter page **and the shared world it owns**, which is the large item (that page is also the only user of the gravity ball, so `CGL_CFG_BOUNCE=0` is needed as well) |
| Using `cgl_iso2d` without a physics world | It already works that way: `cgl_iso2d_sort()` takes a `cgl_iso2d_src_t` (an accessor plus a count), and `include/ext/cgl_iso2d_phys2d.h` is the only bridge from a body pool to the sorter |
| Putting the physics pool on my own storage, or choosing its size at run time | `-DCGL_PHYS2D_USER_STORAGE=1` plus `cgl_phys2d_init_storage()`; the default form keeps the pool inside `cgl_phys2d_t` and costs nothing extra |
| Building or linking the optional module outside this repository | The module is self-contained: `add_subdirectory(.../src/ext)` plus `cytgl::ext`, or install the artifacts (headers land in `include/ext/`) |
| **Linking** the optional module into another project | `-DCYTGL_BUILD_EXT=ON` produces `cytgl_ext` / `cytgl::ext` (`libcytgl_ext.a`), whose public headers belong to the module; the core never lists it, so not asking for it costs 0 bytes |
| The logo artwork | `python tools/mklogo.py ...`, see section 18.6 |
| Pins / SPI / DMA / buttons | Edit the `.ioc` and regenerate; see section 18.8 and chapter 4 |

---

## Appendix E Pre-Commit Self-Check and Numerical Consistency

### E.1 Four self-test questions

1. Did the host rendering tests pass? Have you opened a new snapshot such as `frame00.ppm` and looked at the new layout?
2. Did the physics-related tests pass (`logo_test` / `bounce_test` / `cgl_bounce_test` / `cgl_iso2d_test`)?
3. Does the new widget **fill its own frame** in `draw`? (Otherwise the previous frame's contents linger.)
4. Does the setter invalidate **only when the value really changed**? (Otherwise it paints one extra band every frame.)

### E.2 Four layering questions

1. Does an `#include "stm32g4xx_hal.h"` appear under `src/`? It must not.
2. Do concrete colours or concrete font sizes appear under `src/`? They must not (the caller passes them in).
3. Does the panel driver touch registers directly? It must not (it goes through the bus vtable).
4. Does the application layer compute time with `HAL_GetTick()` / `HAL_Delay()`? It must not; time enters only through the port's `tick_ms`, and input only through `poll_input` carrying the frame's `dt`.

### E.3 Numerical consistency

When any item on the left changes, the positions on the right must be revisited.

| Change | Also needs updating |
|---|---|
| `CGL_CFG_SCREEN_W/H` | The panel driver's mode table (visible-area offset and MADCTL); the geometry validation in `cgl_screen_init()`; the bitmap tool's grid limit |
| `CGL_CFG_CLOCK_STEP_MS` | The physics speed (`CGL_DEBRIS_SETTLE_MS` is unaffected because it accumulates in milliseconds) |
| `CGL_DEBRIS_STEP_Q4` / dot pitch / dot size | The geometry comment at the top of `include/ext/cgl_debris_config.h`; the grid limit derived by the bitmap tool; the three must be tuned together |
| `CGL_BOUNCE_GRAVITY_Q4` / `CGL_BOUNCE_RESTITUTION_Q8` / `CGL_BOUNCE_RADIUS_PX` | The example application's tuning table; the bounce count and energy printed by `cgl_bounce_test` (the defaults were chosen for those) |
| `CGL_PHYS2D_MAX_BODIES` | The bitmap tool's lit-dot budget; RAM usage (16 B plus 2 B per rigid body); **`CGL_ISO2D_MAX_POINTS`**, which must be at least the pool capacity or the `_Static_assert` in the bridge fails the build (leaving it larger only wastes 2 B per unused slot) |
| `CGL_CFG_LABEL_MAX_TEXT` | RAM usage (in the example, about 25 labels each own a buffer) |
| Band height `BAR_H` / font size | The example application's dot-matrix card layout; the bitmap tool's viewport derivation |
| The logo artwork | The generated `logo_bitmap.h` header comment (auto-generated); the lit-dot total (480 in the example) |
| Adding a page | The application's page storage struct and the page table; to let the menu reach it, extend the menu item count and the index-to-page mapping as well |

**Experience**: measured numbers beat comments. This project has historically had comments drift out of step with the code (period, ball speed, default camera style). When in doubt, run the tests and then **fix the comment too**.

---

*This manual applies to CytGL at the current version of this repository (`CGL_VERSION_STRING` = `0.1.0`).*
