# Demo application: pages, navigation, smash animation and 2.5D physics

This document covers the application-side content beyond `App/` and `BSP/board_key`: application assembly, the pages and how navigation is driven,
the logo smash animation, the bounce ball, the `cgl_phys2d` fixed-point physics engine, the `cgl_iso2d` isometric projection and depth sorting.
For the framework itself see the CytGL manual; for the order within one frame see chapter 3 of that manual.

> The `cgl_*.[ch]` rows in the table below are **not in this directory**: the physics/projection
> module lives in the repository's own [`src/ext/`](../../../src/ext/) (code) and
> [`include/ext/`](../../../include/ext/) (public API) trees, as an optional module beside the
> framework rather than part of the demo. It borrows the framework's shape, so it is included the
> framework's way: `#include <ext/cgl_phys2d.h>`, with `include/` as the one `-I` root. In the table,
> `cgl_x.[ch]` therefore means `include/ext/cgl_x.h` + `src/ext/cgl_x.c`. Its contract is
> the CytGL manual, chapter 15. The `App/...` rows are the demo's own
> files, and `App/` is where the module's *consumer* lives (the logo widget is the single renderer of
> the shared world).

| File | Size | Responsibility |
|---|---|---|
| `App/app.[ch]` | 47 / 18 lines | `app_init()` (builds the UI) / `app_run()` (superloop, one frame per millisecond) |
| `App/app_ui.[ch]` | 174 / 55 lines | Assembly: registers the four pages on the board port's screen + navigator, then forwards one frame per call |
| `App/app_page_menu.[ch]` | 338 / 113 lines | Main menu page (**3 items**) + sliding highlight bar; every page change is made inside its own `on_event` |
| `App/app_page_info.[ch]` | 186 / 81 lines | Info page (10 readouts) presented as a semi-transparent overlay; consumes no key at all |
| `App/app_page_smash.[ch]` | 486 / 185 lines | Smash page: **the only place that advances the `cgl_phys2d` world**, and the owner of the RAM both animation pages share + top/bottom bars. The effect itself is the module's; what is here is the bitmap as a callback, the palette, the one widget that paints the whole world, and the ball's playground |
| `cgl_debris.[ch]` | 309 / 190 lines | **The shatter scene** (moved out of `App/`): bitmap -> rigid body cloud, camera fit, SMASH/SETTLE cycle. Framework free — it includes `cgl_phys2d` / `cgl_iso2d` and nothing that knows what a canvas is |
| `cgl_debris_view.[ch]` | 302 / 105 lines | The scene's canvas-facing half: floor, grid lines, depth-sorted dots and balls, shadows. The module's only file that includes `<cytgl/cgl_canvas.h>` |
| `cgl_debris_config.h` | header only | The scene's knobs: the `CGL_CFG_DEBRIS` off switch, grid geometry, the wrecking ball, the settle time, the camera fit |
| `App/app_bounce_ball.[ch]` | *removed* | The bounce ball is no longer the application's: it is the optional module's `cgl_bounce.[ch]` (motion) + `cgl_bounce_view.[ch]` (painter and drop-in widget). What stayed in `App/` is the one thing that is a scene decision — `smash__ball_area()` in `app_page_smash.c`, i.e. where the ball is allowed to go |
| `cgl_bounce.[ch]` | 525 / 263 lines | **The gravity ball** (moved out of `App/`): gravity, four walls, restitution, and self-healing by body identity. Framework free — it includes `cgl_phys2d` and nothing that knows what a canvas is |
| `cgl_bounce_view.[ch]` | 136 / 167 lines | The ball's canvas-facing half: one culled disc, **plus** a drop-in `cgl_bounce_widget_t` for a ball that floats on its own. The reference page does not use the widget — it has a sorted scene, so the scene's painter draws the ball |
| `cgl_bounce_config.h` | header only | The ball's knobs: the `CGL_CFG_BOUNCE` off switch, radius, gravity, restitution, the spawn |
| `cgl_particles.[ch]` | 984 / 876 lines | **Bullet time particle engine** (moved out of `App/`): Q8.8 simulation, 64-entry sine LUT, reciprocal-table perspective divide, orbiting 3-D camera, counting-sort depth ordering. No libm, no heap, no float, **and no framework header** — it builds and unit-tests on its own |
| `cgl_particles_view.[ch]` | 91 / 88 lines | The module's canvas-facing half: paints one band of the sorted cloud. The only file of the particle module that includes `<cytgl/cgl_canvas.h>` |
| `cgl_particles_config.h` | header only | The particle module's knobs: the `CGL_CFG_PARTICLES` off switch, pool size, sort buckets and the panel-size default |
| `cgl_particles_tables.[ch]` | generated | The engine's sine and reciprocal tables + `tools/mkparticletables.py`, which generates them |
| `App/app_page_bullet.[ch]` | 820 / 328 lines | Bullet time page: the ball, the sweep, the integer time-scale state machine, the orbit, the HUD — and the widget that paints the frozen cloud |
| `cgl_phys2d.[ch]` | 430 / 165 lines | Fixed-point circle-collision physics engine (Q12.4, including the 2.5D vertical axis) |
| `cgl_iso2d.[ch]` | 405 / 210 lines | Isometric camera + counting-sort depth sorter. Knows no rigid bodies: it sorts *positions* through the accessor in `cgl_iso2d_src_t` |
| `cgl_iso2d_phys2d.h` | header only | **The bridge** between the two modules above: a `cgl_phys2d_t` seen as `cgl_iso2d` input (`cgl_iso2d_sort_bodies()`). The only file of the module allowed to include both layers |
| `cgl_phys2d_config.h` | header only | Every tunable of the **physics** layer and of the simulated world: domain and origin, grid cell size, body pool, velocity ceiling, the 2.5D vertical axis, plus the consistency gates |
| `cgl_iso2d_config.h` | header only | Every tunable of the **projection** layer: camera constants, depth bucket count, sort table capacity |
| `cgl_ext_basis.h` | header only | What both layers genuinely share: the Q12.4 fixed point convention (`CGL_EXT_Q4_*`) and the force-inline shim (`CGL_EXT_INLINE`) |
| `App/logo_bitmap.h` | generated | Dot-matrix data (106 x 14 cells, 480 lit dots) + `logo_bitmap_get()` |

> The `cgl_*` rows above are the **optional `cytgl_ext` module**, which holds two layers and three
> effects: the 2.5D physics/projection pair (`cgl_phys2d` + `cgl_iso2d`), the particle engine
> (`cgl_particles` + its painter) and the gravity ball (`cgl_bounce` + its painter). Its
> **engine** translation units include nothing but the freestanding C headers (`<stdint.h>`,
> `<stdbool.h>`, `<stddef.h>`; no framework header, no HAL) and build and unit-test with no
> framework in the graph at all; the parts that must draw (`cgl_iso2d`'s consumers,
> `cgl_particles_view.c`, `cgl_debris_view.c`, `cgl_bounce_view.c`) are split out precisely so
> that the maths stays framework-free. `cgl_phys2d_config.h`, `cgl_particles_config.h` and
> `cgl_debris_config.h` are the only headers that optionally look at the panel size
> (`CGL_CFG_SCREEN_W/H`, as a default only). See
> the CytGL manual, chapter 15.

## 1. Application assembly (`app_ui`)

```c
typedef struct {
    app_page_smash_t smash;   /* owns the shared RAM: a ~12.4 KB cgl_phys2d world, or
                                 the bullet time page's 7.1 KB particle engine */
    app_page_bullet_t bullet; /* holds NO world: borrows the smash page's bytes */
    app_page_menu_t  menu;    app_page_info_t info;
} app_ui_t;                   /* ~18.8 KB of pages, statically allocated (s_app) */
```

`app_ui` owns **the pages and nothing else**. The panel, the bus, the display, the
screen, the navigator, the clock and the key all belong to the board port
(`cgl_port.[ch]` in the project root), whose own static state (`s_board`, ~10.7 KB,
carrying the two band buffers and the opacity scratch) is already in place by the
time `app_ui_build()` runs. Keeping a private `cgl_display_t` / `cgl_screen_t` /
`cgl_page_nav_t` here -- which is exactly what this layer used to do -- duplicated
those buffers and handed the framework a second damage sink, so the screen is now
only ever *borrowed* through `cgl_port_board_screen()`.

The `bullet` row is the interesting one. The bullet time page needs 500 particles, a sort
table and 128 depth buckets -- 7.1 KB -- and the part has 32 KB of RAM with about
2.5 KB of BSS left over before this page existed. So the two animation pages **share one
buffer**, because they can never be live at the same time (they are two entries of the
same menu, and exactly one page is on top of the stack). `smash_storage_t` in
`app_page_smash.[ch]` is the union that makes the linker agree with that, and a
`_Static_assert` fails the build if the particle engine ever outgrows the physics world.
The result: **the page costs no extra RAM at all** -- `s_app` grew by 1.2 KB (its own
widgets and state), not by 7.1 KB.

The order inside `app_ui_build()` (every step is required):

1. `cgl_port_board_init()` -- the port brings the bus and the panel up (`board_init` ->
   `st7789_bind` -> `cgl_display_init`), validates the panel geometry against `CGL_CFG_SCREEN_W/H`
   (`cgl_screen_init()` returns -1 on a mismatch -- that is the geometry self-check this layer
   used to repeat by hand), installs the damage sink, arms the clock (`cgl_clock_init(16 ms, 100 ms)`)
   and resets the key state machine (`board_key_init()`);
2. `cgl_port_board_screen()` / `cgl_port_board_nav()` borrow the screen and the navigation stack
   the port created (`cgl_page_nav_init(&nav, "pages", the whole screen)`, so the page host fills
   the whole screen and is the parent of every page's root widget);
3. the four pages' `*_init()` (**they only register objects**; the widget trees are built inside each page's own `on_create`);
4. `cgl_page_nav_reset(&nav, &menu.base)` installs the main menu at the bottom of the stack, then
   `cgl_port_board_frame()` runs one frame -- this commits the RESET and draws the whole screen
   (its dt is 0, or 1 ms if a millisecond boundary falls in between: no physics step runs and the
   FPS statistics stay unbiased).

`app_ui_tick()` does exactly one thing: call `cgl_port_board_frame()`. That single call scans the
key with the frame's dt (the port's own `poll_input` hook), then runs time, input, page changes,
physics and rendering in the order fixed by `cgl_screen_frame()`.

**Nothing in `app_ui` changes the page stack.** There is no idle timer, no script and no
"demo mode": every push/pop is either made inside the `on_event` callback of the page that
receives the key (the menu page's `KEY_LONG` -> `menu__activate()`), or comes from an explicit
`app_ui_push/back/home()` call made by an outside caller. A page change can therefore always be
traced back to an input event.

### 1.1 Why "long press = back" posts two events

The board port translates one long press into two events,
**`CGL_EV_KEY_LONG` + `CGL_EV_BACK`** (`port__poll_input()` in `cgl_port.c`):

| Page | Handling of `KEY_LONG` | Result |
|---|---|---|
| Menu page | Eats it, interpreting it as "enter the selected item" | The following `BACK` is eaten as well (it is at the bottom of the stack, nowhere to go) |
| Smash page | Eats it | Back is left to the navigator |
| Info page | `on_event = NULL`, nobody eats it | `CGL_EV_BACK` falls through to the navigator -> it pops the stack automatically |

If only `BACK` were posted, the menu could not use a long press to confirm; if only `KEY_LONG` were posted, every page would have to write its own back handling.
By posting both, a page only has to declare which one it cares about.

### 1.2 Public entry points

`app_ui_push/back/home/top`, `app_ui_smash()`, `app_ui_restart()`
-- reserved for a future remote control / serial commands / more buttons.

They all go through `CGL_TRANS_AUTO`, that is, they **listen to the target page's own transition declaration** (smash page = `GENIE_IN`,
info page = `COVER_UP`, see the CytGL manual, section 13.5): when pages are added later, the transition follows the
page and there is no need to come back and change this code.

## 2. The pages

The menu now has **three** items: Logo Smash, Bullet Time, System Info.

### 2.1 Smash page (stack depth 2, enters with `GENIE_IN`, returns with `GENIE_OUT`)

```
 x=0                                                          x=319
y=0   +--------------------------------------------------------------+
      | Logo Smash                    TAP=REDO HOLD=BACK             |  top_bar 0..17
y=18  +--------------------------------------------------------------+  sep 1 px
      |                                                              |
      |  dot matrix CytStudio (318x42 px) + heavy ball + bounce ball |  animation viewport 19..152
      |  (both animation widgets use the same frame and world)      |  134 px
y=153 +--------------------------------------------------------------+  sep 1 px
      | FPS 58   DOTS 480   SMASH 1     B 12                         |  bottom_bar 154..171
y=172 +--------------------------------------------------------------+
```

| Region | Macro / coordinate |
|---|---|
| Bar height | `BAR_H = 18` (font `cgl_font_8x16`); for something thicker switch to `cgl_font_12x24` and change `BAR_H` to 26 |
| Viewport | `y 19..152` (the full screen width); the dot matrix and both balls live inside it |
| Bottom-bar readouts | `FPS x4 w60`, `DOTS x72 w92`, `SMASH x168 w74`, `B x244 w72` (right-aligned) |
| Hint blink | Derived from the shared clock: `(clk->now_ms / 700) & 1`, it does not accumulate dt itself |

**`smash__step()` is the only place in the whole project that advances physics** (the page's `on_step`, about 62.5 times per second):

```c
cgl_bounce_pre_step(&p->ball);    /* 1) apply forces (gravity): must come before the integration (semi-implicit Euler) */
cgl_phys2d_step(&p->storage.u.w.world); /* 2) the one and only integration + collision solve */
cgl_bounce_post_step(&p->ball);   /* 3) wrap-up: wall reflections + counting, must come after the integration */
cgl_debris_step(&p->debris, step_ms);   /* 4) observation phase: SMASH/SETTLE/rebuild */
p->steps++;
```

The order must not change; because there is only this one call site, "several widgets each stepping once -> physics runs N times too fast" is structurally impossible.
The widgets do not even need `dt` (both animation widgets have `update` set to `NULL`).

| Callback | What it does |
|---|---|
| `on_create` | Builds the root (whole screen, CLIP), the top bar/separator, the logo widget, the bottom bar and 6 labels; hands the shared world to `cgl_debris_init()` and to `cgl_bounce_init()` (there is **no ball widget** -- see sections 3.3 and 4); sets `shown_*` to `0xFFFFFFFF` to force a first-frame refresh |
| `on_enter` | `app_page_smash_restart()`: every time we come back to this page it starts a fresh round (so a "freeze" is visible at a glance) |
| `on_pause` / `on_resume` | Only sets the `paused` flag (the page is about to be hidden, so drawing would be seen by nobody) |
| `on_frame` | The four readouts are passed to `cgl_label_set_int()` **only when the value changed**; the hint blinks |
| `on_event` | `KEY_UP` with `hold_ms < 600` -> start a new round; `KEY_DOWN/REPEAT` are eaten |

### 2.2 Main menu page (bottom of the stack, installed by `CGL_NAV_RESET`, no transition)

* Two items: `"Logo Smash"` -> `GENIE_IN` (poured out of the "suck-in point" below the screen);
  `"System Info"` -> `COVER_UP` slides over. **Both transitions are written on the target page itself**
  (`cgl_page_set_trans_enter()` inside `app_page_smash_attach()` / `app_page_info_attach()`),
  `menu__activate()` only passes `CGL_TRANS_AUTO` -- when pages are moved or added the transition follows the page,
  so there is no need to come back and change the menu.
* Back (long press) uses the navigator's default transition: `POP` = `GENIE_OUT`, that is, **the current page is sucked
  back into the suck-in point** -- the macOS minimise action.
* The `highlight` bar is a `cgl_panel_t` inserted **before** the menu-item label (draw order = insertion order,
  so it is painted under the text); when the selection changes it **tweens the y coordinate directly** with `cgl_anim` (`OUT_CUBIC`, 140 ms)
  -- the easing curve is what makes the bar "slide over there".
* Layout: `MENU_ITEM_X 14`, width 292, height 22, spacing 6, first item y 40; counter y 106; footer y 130.
* The `SMASH STEPS` field reads the smash page's accumulated logical step count: **while the smash page is covered it does not grow at all**,
  so going back to the menu shows how far behind it is.
* Events: `REPEAT` and a "short `KEY_UP`" -> move the selection down (long-press auto-repeat works as fast browsing);
  `KEY_LONG` -> enter the selected item; `CGL_EV_BACK` -> eat it (we are already at the bottom of the stack).

### 2.3 Info page (stack depth 2 or 3)

* `on_event = NULL`: it consumes no key at all -> the `CGL_EV_BACK` after `KEY_LONG` pops the stack via the navigator.
  This is what a page without its own back logic looks like.
* 10 readouts arranged in two columns (left column even indices x=8, right column odd indices x=166, column width 148, line height 18,
  first line y=24); the last line gets its own colour:

| Left column | Right column |
|---|---|
| `FPS`, `UPTIME` (x10 fixed-point, printed as `12.3 s`), `STEPS` (steps accumulated by the global clock), `NAV DEPTH` (`n / 4`), `SMASH STP` (**steps received by the smash page**) | `FRAME` (this frame's dt ms), `FRAMES` (frame number), `STEP MS`, `TRANS` (number of completed transitions), `DROPPED` (illegal navigation requests + dropped events) |

* **The refresh cadence is derived from the shared clock**: `clk->now_ms / 150`; it does not accumulate dt itself and needs no extra state variables.
* `PHYSICS RUNNING` (green) / `PHYSICS FROZEN (page below)` (amber): the latter comes from
  `app_page_smash_is_paused()`. While the info page is stacked on top of the smash page, `STEPS` keeps growing but `SMASH STP` is frozen
  -- that is the visible proof that "a covered page -> physics frozen" (not an estimate, but the regression criterion for the catch-up-after-timeout problem).

### 2.4 Bullet time page (stack depth 2, enters with `GENIE_IN`, returns with `GENIE_OUT`)

The same 320x172 layout as the smash page -- same bars, same viewport -- so switching
between the two does not move anything on screen. The bottom bar reads:

```
y=154 +--------------------------------------------------------------+
      | FPS 62   P 500        ORBIT        Y 90                      |
y=172 +--------------------------------------------------------------+
```

| Region | Macro / coordinate |
|---|---|
| Viewport | `y 19..152`, same as the smash page |
| Readouts | `FPS x4 w60`, `P x72 w88` (particles alive), `STATE x164 w74`, `Y x244 w72` (camera yaw in degrees, right-aligned) |
| `STATE` colour | cream in `BUILD`, green while time flows, **amber while the time scale is 0** |

The animation is a six-phase state machine driven by the page's `on_step`:

| Phase | What happens | Time scale (Q8) |
|---|---|---|
| `BUILD` | the Logo is intact **and at rest** (the parked cloud is not simulated), the ball flies in from `-210 px`, i.e. from **off the stage** | 256 (real time) |
| `FLY` | the ball's edge reaches the Logo's near face and starts **carving** (`cgl_particles_impact()` releases the dots it reaches, one contact at a time); `CGL_PARTICLES_TRIGGER_TICKS` (78 steps = 1.25 s) later the freeze begins | 256 |
| `FREEZE` | the scale ramps down; **the camera starts turning during the ramp**, not after it | 256 -> 0 |
| `ORBIT` | time stopped, camera orbits (`BULLET_ORBIT_MS` at `BULLET_YAW_STEP_*` per tick) | 0 |
| `RELEASE` | the scale ramps back up, the debris finishes its flight, and the **camera walks back to the front** (`bullet__camera_home()`): the ball has to finish its run across the frame, and "which way the ball goes" only means anything head-on -- with the orbit left wherever it stopped, an edge-on yaw moves the ball in depth and it would never leave the stage | 0 -> 256 |
| `SETTLE` | the finished cloud is left on screen and the ball runs off the far side; the round restarts when the ball is **gone** (`bullet__ball_gone()`, at least `BULLET_SETTLE_MS` in, and never later than `BULLET_SETTLE_MAX_MS`) | 256 |

#### The wrecking ball is not a particle

It is page state, and that shows up in two places worth knowing about when tuning it:

* **It is not clamped to the ±128 px particle world.** The particles are built from the Logo
  grid and culled when they leave that Q8.8 window, but the ball starts 210 px off the near
  side of the stage and has to run off the far side. `cgl_particles_project()` therefore takes int32
  world coordinates (the particles just happen to pass int16 values), and the page passes
  `ball_x_q8` straight through: clamping it with `cgl_particles_clamp_i16()` -- which is what the first
  version did -- saturates at ±128 px, barely past the screen centre at this camera scale, so
  the ball could never cross the frame. Measured on the chip, the ball now spans screen x
  **-71 -> 349** on a 320 px wide screen, i.e. in from the left and out past the right.
* **It is not depth sorted**, and it does not collide: it is a constant-speed body drawn on
  top of the cloud (see "who draws what" above), and its only interaction with the engine is
  `cgl_particles_impact()` asking it which dots it has reached.

**The time scale is a counter, not a multiplier.** "Time nearly stops" is not
`pos += vel * timeScale` -- that would make the integration frame-rate dependent and
could not express "exactly stopped" without a special case. Instead:

```c
p->time_accum_q8 += p->time_scale_q8;        /* once per physical step (62.5 Hz) */
while (p->time_accum_q8 >= BULLET_SCALE_ONE) {
    p->time_accum_q8 -= BULLET_SCALE_ONE;
    bullet__logical_step(p);                 /* one whole simulation step */
}
```

so particles only ever advance by **whole** steps, at 256 the loop runs exactly once per
physical step (bit-identical to no scaling at all), and at 0 it never runs -- "frozen" is
not a flag, it is an accumulator that never reaches its threshold. The camera update sits
**outside** that loop, which is the entire effect: the world stopped, the camera did not.

#### Where the depth sort runs, and why that is not a detail

`cgl_particles_sort()` runs in `bullet__step`, once per logical step, and **not** in the
widget's draw callback. The draw callback is invoked once per **band**, and the band
walker aligns its bands to the screen (rows 0..3, 4..7, ...), so the first band
overlapping the viewport is rows 16..19 while the viewport starts at row 19. The obvious
"once per frame" test

```c
if (band_y0 == w->frame.y0) { cgl_particles_sort(...); }   /* never fires */
```

is therefore **never true**: the sorter never runs, `s->sorted` stays false,
`cgl_particles_view_draw_band()` returns immediately for every band, and the whole viewport renders
nothing at all -- with no error, no warning, and a symptom ("the engine was never
initialised") that points at the wrong place. The sort also genuinely belongs in the
step: it depends on the camera, and the camera is updated by the step.

`app_page_bullet_restart()` sorts as well, because a frame can be **drawn** before it is
**stepped** (`cgl_screen_frame(dt)` applies a pending page change before stepping, and the
`dt = 0` frame in `app_ui_build()` never steps at all).

#### The engine's four fixed-point facts

| Fact | Where |
|---|---|
| No float, no `libm`, no heap, no division in the render path | `cgl_particles.[ch]`, i.e. `include/ext/cgl_particles.h` + `src/ext/cgl_particles.c` (and the painter in `src/ext/cgl_particles_view.c`) |
| Sine: a 64-entry 0..90 deg table (64 B), other quadrants by index mirror + sign | generated, `cgl_particles_sin_q8()` / `cgl_particles_cos_q8()` |
| Perspective divide: a reciprocal table indexed by the depth itself, one linear step | generated, `cgl_particles_recip_q16()` |
| Particle: **12 B** -- `int16 x,y,z` (Q8.8) + `int8 vx,vy,vz` (Q8.8/step) + life/bucket/pad | `cgl_particle_t` |
| Depth order: counting sort into 128 buckets, once per step, 3 linear passes | `cgl_particles_sort()` |

The tables are **generated** (`tools/mkparticletables.py`) rather than hand-typed, and both
carry a range check that fails the generation instead of wrapping: `sin(90 deg)` rounds to
256 in Q8.8, which does not fit the `uint8_t` the sine table is stored in, so the
generator saturates it at 255. An unclamped 256 is emitted as `0x100`, truncates to 0 in
the array, and makes `sin(90 deg)` **zero** -- a singular camera matrix, not a rounding
artefact. `tests/bullet_host_test.c` checks the saturation, the fold's continuity, the
reciprocal's accuracy against `1/d`, and the sort against a reference insertion sort at
52 camera angles.

#### The burst magnitude: 3.5e9 does not fit an int32

The one arithmetic in this engine whose **intermediates**, not its inputs, are the problem.
Both release paths -- `cgl_particles_burst()` (the whole sheet at once, which is what the engine tests
drive) and `cgl_particles_impact()` (the page's sweep, via the shared `cgl_particles__release()`) -- throw particles
away from the impact along the unit vector `d/|d|`, and `d` is the vector from the impact to
the particle: with the Logo 232 px wide and the ball striking its near face, `|d|` reaches
59 392 Q8.8 units, so `d0^2 = 3.5e9` -- past `INT32_MAX` (2.1e9). The wrapped sum is negative,
and since signed overflow is undefined behaviour the compiler may assume it is not: both
compilers deleted the `v <= 0` guard in the square root as dead code and the
`while (bit > v) bit >>= 2;` loop behind it then never terminated. **The page froze on the
panel at the frame the ball touched the Logo, with the Logo still intact** -- and the host test
hung in the same place (120 s ctest timeout, section [7]), which is how the defect was finally
pinned down.

`cgl_particles__dir_mag()` now keeps the sum of squares in uint32 with the low two bits of every square
carried separately, `q = sum(d_i^2 >> 2)` and `r = sum(d_i^2 & 3)`, so `N = 4q + r` exactly
and no intermediate exceeds 32 bits; `m = isqrt(q)` gives `|d| = 2m + delta` with `delta` in
0..2, and two comparisons pick `delta` out. The result is still the **exact** integer
magnitude (verified against an int64 reference over 4 million vectors), so the direction is
not quantised, and `cgl_particles__isqrt_u32()` takes an **unsigned** argument for the same reason: there
is then no value for which its restore loop cannot terminate. `bullet_host_test` section
[6b] bursts at the real geometry -- 479 particles, none parked, all flying radially outward
-- which is the case the synthetic grid of section [6] could not reach.

#### How far the debris flies is the whole effect

The second half of "the Logo does not shatter" was not a bug in the burst at all, and it is
worth writing down because the arithmetic is easy to miss: **a debris velocity is an int8**
(that is what makes a particle 12 B), so 127 units -- 0.5 px per logical step -- is the hard
speed ceiling, and the distance the cloud opens up is

```
spread  ~=  speed x fly steps x 2^CGL_PARTICLES_DAMP_SHIFT      (the damped geometric series)
```

The original tuning was 31 steps (0.5 s) with `CGL_PARTICLES_DAMP_SHIFT` 3 (v *= 7/8 per step, 1.6 % of
the launch speed left at the freeze) on the theory that "the cloud coasts and then hangs
almost still, so the freeze does not have to hide a fast moving scene". That reasoning
covered the *velocity* and forgot the *displacement*: the measured spread of the frozen
cloud was **3.3 px mean / 4.2 px max**, i.e. 1.5 dot pitches. The Logo became ragged; it
never flew apart -- and the freeze then froze a ragged Logo, which is what came back from the
panel.

The deceleration the freeze needs is already the page's job (`BULLET_FREEZE_RAMP_MS` scales
time down to 0 and the gated steps stop), so damping only has to keep the cloud from being in
motion forever. With `CGL_PARTICLES_DAMP_SHIFT` 7 (v *= 127/128) and 78 fly steps (`CGL_PARTICLES_TRIGGER_TICKS`),
the debris the ball reaches spreads a measured **9.9 px mean / 27.3 px max** by the freeze --
near the contact the dots have just been struck and are still on their cells, which is
correct, while the sheet the ball ploughed through first has thrown its dots most of a
letter-height away. `bullet_host_test` section [7] asserts the spread (>= 8 px mean, >= 20 px
max) and prints it, so "the Logo does not fly apart" cannot come back unnoticed.

#### The smash has to be **local** (`cgl_particles_impact()`, not `cgl_particles_burst()`)

Fixing the two problems above still left the wrong effect, and this is the part that took a
third report from the panel to see: `cgl_particles_burst()` releases the **whole** sheet from one point,
so the moment the ball's edge touched the near face, all 500 dots flew away -- including the
ones the ball was nowhere near. On the panel that reads as "the whole Logo dissolves before
the ball arrives", and the freeze then holds a full explosion that the ball never caused.

The page therefore sweeps instead: every logical step it calls

```c
cgl_particles_impact(&particles, ball_x_q8, BULLET_BALL_R_PX * CGL_PARTICLES_ONE);   /* release what it reached */
cgl_particles_step(&particles);                                  /* then simulate            */
```

`cgl_particles_impact()` releases each parked dot the ball's leading edge has passed, with the *same*
velocity model as `cgl_particles_burst()` (both call `cgl_particles__release()`, so they cannot drift apart) but with
the direction taken from the **ball's centre** instead of one fixed plane. A parked dot has
`hit == 0` and `cgl_particles_step()` skips it entirely -- no gravity, no life burn -- so the
part of the Logo the ball has not reached stays **bit for bit** on the cell `cgl_particles_build()` put
it on. The result is the smash page's read: the ball carves a path, the debris sprays from the
contact point, and the frozen frame is a half-eaten Logo with the balls of debris hanging
around it. Measured at the freeze of the default round: the ball has reached 307 of the 500
dots, **193 of the 193 it has not reached are exactly where they were built**, and `[7]`
asserts both halves of that sentence.

### 2.5 Watch face page (stack depth 2, enters with `COVER_DOWN`, returns the same way)

The one page that a build **always** contains (it links no optional module and borrows no RAM), and
the reason the framework grew `cgl_math.h` and the three radial canvas primitives. It is the first
radial drawing in the project: every element of the dial -- the chapter ring, the hands, the
seconds ring, the hub -- is "a point at an angle on a radius".

```
 x=0                     x=176                x=320
y=0   +-----------------------------+----------------------+
      |        chapter ring         |       10:09          |  12x24 digital
      |    ,--------------------.   |     WEDNESDAY        |  8x16
      |   /   seconds ring (r=75) \  |      14 JAN          |
      |  |     ,--.                | |                      |
      |  |    | hub |   hands      | |    ( seconds )       |  ring gauge, r=28
      |   \    `--'               /  |                      |
      |    `--------------------'   |                      |
y=172 +-----------------------------+----------------------+
        dial centre (86, 86), r=80
```

| Region | Value |
|---|---|
| Dial | centre `(86,86)`, face radius **80**, 2 px antialiased bezel, 1 px inner ring |
| Seconds ring | stroke radius **75**, width 5: a dim track plus the orange progress arc and its cap |
| Chapter ring | 60 marks; the 12 hour marks `r 64..70` at 3 px, the 48 minute marks `r 67..70` at 2 px |
| Hands | hour 42 px / 9 px wide, minute 62 / 7, second 71 with a 14 px counterweight, hub 5 |
| Complication column | centre x = 246: `10:09` (12x24), the weekday, `14 JAN`, and a seconds ring gauge of radius 28 |
| Colours | pure black background, dial face `(10,12,16)`, cream hands, **Apple orange** `(255,106,0)` for the second hand, the ring and the gauge |

**The time comes from the framework clock, and the page says so.** This board has no RTC (CubeMX
brings up LSE on PC14/PC15 but never instantiates the peripheral), so the page anchors a wall time
`t0` at a `clk->now_ms` and shows `t0 + (now - t0_ms)`. That is exact, monotonic and drift-free, and
it keeps running while the page is covered, because the framework clock is global and the page
simply was not reading it. `app_page_clock_set_time()` / `app_page_clock_set_date()` re-anchor at
any instant -- that is the whole RTC hook -- and the date counts forward from the anchor through a
real calendar (month lengths and leap years, tested across a month, a leap day and a year boundary
by `clock_test`).

**What is animated, and what is a function of the clock.** Only the *events* are tweened:

| Animation | Driven by | Why not a tween |
|---|---|---|
| the hand sweep | a pure function of `now` | nothing accumulates, so nothing can drift, and the sweep is as smooth as the frame rate |
| the comet trail | one byte per minute mark, decaying | it is history, not motion |
| **reveal** 0 -> 256 over 760 ms | `cgl_anim`, `OUT_CUBIC` | the entrance: the hands sweep up from 12:00, the chapter ring is drawn mark by mark, the ring fills |
| **pulse** 256 -> 0 over 620 ms | `cgl_anim`, `OUT_QUAD` | the flash when the minute turns over (note the direction: it *decays* to zero, so its resting value is "nothing is happening") |
| **roll** 0 -> 256 over 300 ms | `cgl_anim`, `OUT_CUBIC` | the digital digits roll to their new value, old glyph leaving through the top of the cell and the new arriving from below |

**Cost, and the three things that brought it down.** A watch face repaints continuously, so its cost
is the frame budget. On the reference board (170 MHz, 42.5 MHz SPI) the first working version ran at
**13 fps** (72 ms per frame, 143 000 pixels painted per frame), which is a stutter, not a watch face.
Three changes, each measured on the board:

| Change | Effect |
|---|---|
| The page's root is a **container**, not a panel: the navigator's host widget already fills every band it presents, so a page that fills the screen again paints 55 000 pixels for nothing | 143 k -> 103 k px/frame |
| The dial marks damage **only what moved** (the second hand at its old and new angle, the ring's leading end, the trailing marks, and the creep of the minute/hour hands only when their integer endpoints change) instead of all 43 bands | 103 k -> 51 k px/frame, 13 -> 24 fps |
| The seconds **complication steps once per second** (the dial hands still sweep): the panel's damage model is one rectangle, so a smoothly sweeping gauge would union with the dial's damage and push the whole screen every frame | 51 k -> 39 k px/frame, 24 -> **39 fps** |

The framework side of that was the band clamp every scanline primitive now carries (`cgl_canvas.c`):
the presenter draws a widget **once per band**, so a shape that walks its whole geometry in every
band pays for one picture forty times over. The arcs walk their rows with a two-pointer bound seeded
by one `cgl_math_isqrt()` at the band's first row, and `line_thick()` rejects a stroke that cannot
reach the band with four comparisons *before* it computes anything. `tests/arc_host_test.c` has a
band-by-band consistency case (including a panned canvas, which is what a page transition produces)
and `tests/clock_host_test.c` has a **ghost test**: it drives hundreds of real frames, then forces a
full repaint at the same instant and requires the two framebuffers to be byte identical.

**What the page costs in flash and RAM.** Measured the way the CytGL manual, section 17.3, describes
section 1.1 prescribes: the parent commit in a `git worktree` whose path has the same length (six
STM32 HAL sources embed `__FILE__`, so the absolute column is path-dependent), both trees built
with `tools/verify_firmware_build.ps1` at `-O2`, GCC 14.3. The three numbers are `text` / `data` /
`bss` from `arm-none-eabi-size`:

| Configuration | parent commit | with the watch face | difference |
|---|---|---|---|
| every effect linked (`App/app_fx_config.h` as it stands) | 47 048 / 16 / 31 608 | 62 176 / 16 / 32 376 | +15 128 / 0 / +768 |
| both effect pages off (`-DAPP_FX_SMASH=0 -DAPP_FX_PARTICLES=0`) | 30 416 / 16 / 15 176 | 46 704 / 16 / 15 952 | **+16 288 / 0 / +776** |

Where that flash goes (per object, from the linker map, both effect pages off):

| Part | Bytes | Why |
|---|---|---|
| `App/app_page_clock.c` | 6 255 | the dial, the hands, the calendar, the layout, the three entrance animations |
| `src/core/cgl_canvas.c` | 4 592 | the antialiased radial primitives (section 6.4 of the CytGL manual); part of it is code that always existed and was dropped by `--gc-sections` until a page called it |
| `src/core/cgl_font_data.c` | 4 572 | the **12x24** digit font: nothing used it before, so all 4 560 B of its glyph table were collected |
| `src/core/cgl_math.c` | 522 | the Q8 sine table and the integer square root |
| `App/app_page_menu.c` | 380 | the fourth menu item and the counter line |

Those five rows sum to 16 321 B, and the same map attributes another 60 B to the page table's glue
(`app_ui.o`, `cgl_page.o`). The ELF's own `text` delta is 16 288 B, so the accounting closes to
within 93 B of section alignment.

The 776 B of RAM are 772 B for the page object itself inside the application's `s_app` plus 4 B
for the `struct app_page_clock *clock` pointer in `app_links_t`. The page allocates nothing and
owns no buffer: every dial coordinate is computed from the framework clock each frame.

> WARNING: the two configurations are **not** interchangeable on this board. With every effect
> linked, the firmware uses 32 392 B of the 32 KB of RAM (98.9 %, 376 B unallocated), which leaves
> 1 912 B of physically available stack; with both effect pages off, the same tree has 18 340 B.
> The watch face itself is only the 776 B above -- the rest of that difference is the physics
> world and the particle pool, which the two `APP_FX_*` switches remove.

### 2.6 Navigation is event-driven only

There is no automatic tour (no idle timer, no script, no demo mode): the five pages never
navigate on their own schedule. The whole set of page changes is:

| Change | Made by | Transition |
|---|---|---|
| Menu -> item 0 (watch face) | `menu__activate()` inside `menu__event()`, on `KEY_LONG` | `CGL_TRANS_AUTO` -> the page's own `COVER_DOWN` |
| Menu -> item 1 (smash) | same | `CGL_TRANS_AUTO` -> the page's own `GENIE_IN` |
| Menu -> item 2 (bullet) | same | `CGL_TRANS_AUTO` -> the page's own `GENIE_IN` |
| Menu -> item 3 (info) | same | `CGL_TRANS_AUTO` -> the page's own `COVER_UP` |
| Watch/Smash/Bullet -> back to menu | the navigator, on the `CGL_EV_BACK` that `port__poll_input()` (`cgl_port.c`) posts after a long press (those pages eat `KEY_LONG` but not `BACK`) | the page's own leave type |
| Info -> back | `on_event = NULL`, so `CGL_EV_BACK` reaches the navigator directly | the navigator's POP default |
| Any -> menu (reset) | an explicit outside call to `app_ui_home()` | `RESET` (instant) |

This is why `menu__activate()` passes `CGL_TRANS_AUTO` and why no call site keeps a
"source -> transition" table: the transition belongs to the target page, and the *decision to
navigate* belongs to a key event.

## 3. Logo smash animation (`<ext/cgl_debris.h>`)

### 3.1 Data and geometry

| Item | Value |
|---|---|
| Dot matrix | `logo_bitmap.h`: 106 x 14 cells, **480 lit dots** (`"CytStudio"`, DelaGothic One @160px) |
| Grid spacing | `CGL_DEBRIS_STEP_Q4 = 48` -> **exactly 3.00 px/cell** (the dot matrix actually occupies 318 x 42 px) |
| Collision radius of a dot | `CGL_DEBRIS_DOT_R_Q4 = 23` (1.44 px; `2x23 <= 48` guarantees that neighbouring dots do not overlap at the start) |
| Painted dot | `CGL_DEBRIS_DOT_PX = 2` (a solid 2x2 square dot) |
| Heavy ball | radius 5 px, speed `BALL_SPEED_Q4 = 75` (4.69 px/step ~ 293 px/s), flies in from x = -140 px off screen |
| Rebuild delay | `CGL_DEBRIS_SETTLE_MS = 900` (accumulated from `step_ms`, so changing the clock step does not make it "taste different") |
| Measured cycle | ~2.6 s (fly-in ~1.7 s + settle 0.9 s); the `SMASH` field in the bottom bar is the round counter |

`cgl_debris__build()` scans the bitmap cell by cell: a lit dot -> `cgl_phys2d_add(x, y, 0, 0, r, 0)`;
the grid deliberately has **no random jitter** (the spacing is an integer 3 px, every dot lands on the same pixel phase, and that is what keeps the dot matrix crisp);
finally the heavy ball is added (`KINEMATIC | SPHERE`) and the sorter is reset (after a world rebuild `world->step` starts over,
and without the reset an old sort stamp could be hit).

### 3.2 Two-phase state machine

| Phase | Entry condition | Behaviour |
|---|---|---|
| `CGL_DEBRIS_PHASE_SMASH` | initial / rebuild | the ball ploughs through the dot matrix; once the ball centre `x > screen width+30` or all dynamic dots have left the domain -> switch to `SETTLE` (if it was entered because "all the dots are gone", `settle_ms` is pre-loaded with half, to avoid waiting for nothing) |
| `CGL_DEBRIS_PHASE_SETTLE` | see left | zero the ball's velocity (so the kinematic ball does not run out of the simulation domain), accumulate from `step_ms`, and at 900 ms -> `cgl_debris__build()` + `restarts++` |

`cgl_debris_step()` **does not step the world** (that is the page's `on_step`); it only observes the result, advances the phase and
marks dirty. This is the direct expression of "a widget has no `cgl_phys2d_step` to call".

### 3.3 One widget draws the whole world

The painter's algorithm requires all objects to be sorted in **one single sequence**, while the widget tree paints its children in insertion order.
If the dot matrix were drawn by one widget and the balls by another, the balls would always cover the dot matrix, whatever the depth
(when a ball flies behind the dot matrix the occlusion would be wrong). Therefore:

* `cgl_debris_view_draw()` paints **all** rigid bodies in `world` (480 dots + heavy ball + bounce ball), in depth order;
* the bounce ball therefore has **no widget at all**: it is a `cgl_bounce_t` member of the page, driven only by
  `cgl_bounce_pre_step()` / `cgl_bounce_post_step()` in `on_step`. The module does ship a self-drawing
  `cgl_bounce_widget_t`, and using it here would be exactly the mistake this section is about.

What is drawn, and in what order:

1. **Floor**: project the world rectangle into a convex quad, fill it row by row as a diamond using the horizontal span
   (one step brighter than the background), then draw two sets of grid lines, one per direction, 32 px apart; each grid line is first clipped to the current band
   and only then run through Bresenham (otherwise every 300 px long line would waste 300 pixel tests in every 4 px high band);
2. **Rigid bodies** (far to near): an `SPHERE` body is raised by one radius and drawn as a filled circle + a ground shadow + a one-pixel highlight;
   an ordinary dot draws its shadow only when `z > 0.5 px`, and is then drawn as a 3x3 square dot.
   Colours: floor `RGB565(24,30,42)`, grid `(38,48,66)`, shadow `(8,9,13)`,
   dots `CGL_COLOR_CREAM`, this widget's heavy ball amber, the bounce ball red.

On a dark background a shadow is only visible if there is a "ground", so the floor is not decoration, it is the precondition for the shadow.

### 3.4 Camera fitting

`logo__fit_rect()` computes the bounding box from **only the dots that are actually lit** (not from the whole 106 x 14 grid):
the bitmap has margins all around, and fitting to the full grid would treat the blanks as content too and, measured, waste about 40% of the screen for nothing.
`cgl_iso2d_cam_fit()` squeezes this world rectangle into the widget frame (leaving a 2 px margin).

| `CGL_DEBRIS_SCENE_STYLE` | Effect | Minimum dot spacing | Colour bleeding (`verify_logo_dots`) |
|---|---|---|---|
| `CGL_ISO2D_STYLE_ISO` (**current default**, fitted isometric) | the vertical budget is allocated by "maximise the minimum dot spacing", so the long axis gets less -> the text is easier to read | ~2.79 px | **3.0 % PASS** |
| `CGL_ISO2D_STYLE_ISO_UNIFORM` (textbook 30 degrees, both axes split equally) | the longer the world, the flatter the dot matrix | ~2.16 px | 32.9 % FAIL -- smeared into one diagonal band |
| `CGL_ISO2D_STYLE_FLAT` | identity matrix, XY laid out 1:1 on the screen (the old flat look) | 3.00 px | -- |

> WARNING: **These two macros are a pair; changing one means you must change the other**: if a dot is bigger than the
> minimum dot spacing, neighbouring dots touch each other and fill in the text's negative space. Measured, `CGL_DEBRIS_SCENE_STYLE` x `CGL_DEBRIS_DOT_PX` has only these viable combinations:
>
> | Camera style | Dot size | Colour bleeding | Verdict |
> |---|---|---|---|
> | `ISO` (fitted) | **2 px** | **3.0 %** | PASS, current default |
> | `ISO_UNIFORM` | 1 px | 6.5 % | FAIL -- just over the 5 % line, and the strokes break into dotted lines |
> | `ISO_UNIFORM` | 3 px | 32.9 % | FAIL -- smeared into one diagonal band |
>
> The "textbook 30 degrees" route is rejected by **geometry** (the vertical direction must fit 106+14=120 cells while the viewport is only 130 px,
> the two world axes differ by a factor of 7.6 in cell count, and the long axis thins out the vertical budget), not by bad tuning.
> After changing anything you **must** run `render_test` and look at the colour-bleeding percentage it prints --
> more than 5 % of blank cells misjudged as lit dots fails outright.

## 4. Bounce ball (`cgl_bounce`, plus the scene's playground)

The motion is the optional module's (`include/ext/cgl_bounce.h`) and every number below is its
default; what this application owns is **where the ball is allowed to go**, which is a scene
decision rather than a physics one, and it lives in `smash__ball_area()` in `App/app_page_smash.c`.

| Parameter | Value | Where |
|---|---|---|
| Radius | 3 px (identical for collision and drawing -- deliberately one knob) | `CGL_BOUNCE_RADIUS_PX` |
| Playground (screen px, the range the ball's **centre** may reach) | x 3..316, y 22..151 | `smash__ball_area()` |
| Corresponding ball pixels | x 0..319, y 19..154 -- the disc runs 2 px past the frame's floor and is trimmed by `CGL_WF_CLIP` | -- |
| Spawn | x = playground left + 7 px, y = the ceiling, `vx = 4 px/step` (~250 px/s), `vy = -0.5 px/step` (a slight upward throw first) | `CGL_BOUNCE_SPAWN_*` |
| Gravity | `0.2 px/step^2` (~781 px/s^2 at 62.5 steps/s); it is not in `cgl_phys2d`, every step does `vy += g` by hand -- **before** the integration | `CGL_BOUNCE_GRAVITY_Q4` |
| Restitution | `240/256 = 0.9375` -- it has to be this high: the playground is 129 px tall, one round trip loses ~`1-e^2`, and with `e=0.85` it sticks to the floor after two or three bounces (measured: only 2 bounces in 20 s) | `CGL_BOUNCE_RESTITUTION_Q8` |

* **The ball bounces across the whole viewport**, so it **passes through the dot matrix** and knocks away any dot in its way -- a deliberately kept interaction.
  The module's own default (`cgl_bounce_view_area()`) would inset the frame by the radius on all four sides; this scene wants the full height, so it writes its playground out instead.
  To make the ball bounce only above the dot matrix, raise `a.y1`'s floor to `view.y0 + 36` (the ball's bottom pixel = 19+36+3 = 58 < 64).
* It is a `KINEMATIC` rigid body (inverse mass 0): it is never pushed by the dot matrix, but it ploughs the dot matrix apart.
* **Self-healing, by body identity**: `cgl_debris__build()` calls `cgl_phys2d_reset()` every round (it only zeroes `count`, it does not free memory),
  so the ball's index can end up **inside** the new pool pointing at a body that belongs to somebody else. The module checks the body's radius and sphere flag
  (both written by it and by nothing else) and re-adds the ball when they do not match, rather than writing over whatever is there.
  Position reads always return coordinates **clamped into the playground**, so callers can never get an illegal position (independent of "which step runs first").
* The page has **no ball widget**. A ball in a depth-sorted scene must be drawn by that scene's one painter (see the CytGL manual, section 15.6 ("the one-renderer rule")),
  which is exactly what `smash__logo_draw()` does. `cgl_bounce` also ships a self-drawing `cgl_bounce_widget_t`, and this page deliberately does not use it.
* Both the motion and the disc colour (`ball_other` in `cgl_debris_palette_t`) are the module's and the palette's; nothing about the ball is left in `App/` except the playground above.

## 5. `cgl_phys2d`: the fixed-point circle-collision engine

### 5.1 Why write our own

The per-body / per-shape bookkeeping of a general physics engine does not fit in 32 KB by itself. This animation needs only a very small slice of
capability: **circles**, **infinite moment of inertia** (no rotation), equal masses for the dots + one infinitely heavy ball,
zero gravity, zero friction, zero elasticity. So it degenerates into purely translational circle dynamics.

| Data structure | Contents |
|---|---|
| `cgl_phys2d_body_t` (16 B) | `x,y,vx,vy,r,z,vz` (all Q12.4 = int16) + `flags` |
| `cgl_phys2d_t` (~12.4 KB) | `body[528]` 8 448 B + `next[528]` 1 056 B + `cell_head[1600]` 3 200 B + counters |
| Fixed-point format | Q12.4 (1 unit = 1/16 px), the inner loops are all integer, no libm, no floating-point context save |
| Simulation domain | x in [-40, 360) px, y in [-40, 216) px: the 400 x 256 world rectangle, offset -40 on both axes. The margin is therefore **not** symmetric (40 px left/right, 42 px top/bottom) -- do not "tidy" the origin into a centred derivation, see the WARNING in `include/ext/cgl_phys2d_config.h`. Leaving the domain deactivates the body |
| Broad phase | uniform grid, 8 px cells, 50 x 32 = 1600 cells, chained through linked lists |
| Velocity limit | `CGL_PHYS2D_MAX_VELOCITY = 8 px/step` |
| Body pool | `CGL_PHYS2D_MAX_BODIES = 528` (the scene actually needs 480 + 2 = 482, 9 % headroom). WARNING: do not trim it further: below about 500 `cgl_phys2d_add()` starts returning -1 and the dot matrix silently loses corners |

> **Where the numbers are set.** Every value in the table above is an overridable
> `#ifndef` in [`include/ext/cgl_phys2d_config.h`](../../../include/ext/cgl_phys2d_config.h) -- domain size and
> origin, grid cell size, body pool, velocity ceiling, the whole 2.5D vertical
> axis. The projection layer's own knobs (camera constants, depth bucket count,
> sort table capacity) are in the same style in
> [`include/ext/cgl_iso2d_config.h`](../../../include/ext/cgl_iso2d_config.h), and the Q12.4 convention both
> layers speak is defined once in [`include/ext/cgl_ext_basis.h`](../../../include/ext/cgl_ext_basis.h). Those
> files are also where the panel size is picked up
> (`CGL_CFG_SCREEN_W/H`, used only as the default) and where the consistency gates
> live that turn a geometry change into a **compile error** rather than into
> silently lost collisions. `cgl_phys2d.h` / `cgl_iso2d.h` keep only what is structural.
> The module still includes nothing but `<stdint.h>` / `<stdbool.h>` from the C
> standard library and never depends on the CytGL framework.
>
> The two config headers are split by *layer*, not by taste: `cgl_iso2d` sits above
> `cgl_phys2d` and knows no rigid bodies, so it must not have to be configured by a
> header named after the physics layer. `grep -n cgl_phys2d include/ext/cgl_iso2d.h` prints
> nothing, and `cgl_iso2d.c` compiles with `cgl_iso2d.h` alone -- that separation is the
> point of the two-config split.
>
> **The pool can also live outside the struct.** The demo keeps it embedded (the ~12.4 KB table
> above, a compile time `CGL_PHYS2D_MAX_BODIES`). A consumer that wants a different pool size, or wants
> the memory somewhere else, builds the module with `-DCGL_PHYS2D_USER_STORAGE=1`: `cgl_phys2d_t` shrinks
> to 40 B of pointers and counters, the caller declares the three arrays (there is still no heap,
> ever) and hands them over with `cgl_phys2d_init_storage()`, and `CGL_PHYS2D_STORAGE_BYTES(n)` states how
> many bytes that is. Note that `cell_head` is not part of the pool: it is 1600 x 2 B = 3200 B of
> *world geometry* in both forms, so a tiny world is dominated by its grid. The demo itself does
> not use the mode -- it would only move the same bytes to a different line of the file -- but it is
> what makes the module usable at a scale the demo does not need.

### 5.2 The four sections of `cgl_phys2d_step()`

```
1) cgl_phys2d__integrate()  x += vx; y += vy;  vertical axis: vz -= g; z += vz; landing handling
2) cgl_phys2d__build_grid() clear the cell heads -> attach the active bodies to the grid; dynamic bodies outside the domain are deactivated
3) cgl_phys2d__solve_grid() each cell only looks at the 5 neighbours in the "direction of travel" -> circle/circle narrow phase
4) cgl_phys2d__launch()     turn the planar speed into a vertical launch (must come after the solve, so that it sees the post-impact velocity)
   world->step++
```

**Narrow phase** (`cgl_phys2d__resolve`): a fully inelastic impulse (restitution = 0), with masses expressed as **inverse mass**
(`KINEMATIC` has inverse mass 0, so kinematic bodies do not move); after the impulse a **position correction** follows, which
pushes the two completely apart, weighted by inverse mass, so that no overlap is left. Within a cell only `ib > ia` is processed; across cells only the
five offsets `{(0,0),(1,-1),(1,0),(1,1),(0,1)}` are visited -- every unordered cell pair is visited exactly once, halving the narrow-phase work.

**2.5D vertical axis (z / vz)**: it only feels gravity, only lands on the plane `z = 0`, and **never takes part in collisions**. Two reasons:

* Semantically correct: XY is the "floor", and debris flying overhead should still hit what it passes -- doing a real 3D solve would instead make the
  heavy ball "visually plough through" things without hitting them;
* Cost: the broad-phase grid, the impulse solve and their tuning stay completely unchanged, the smash feel stays the same and the tests remain valid.

| Vertical-axis parameter | Value | Meaning |
|---|---|---|
| `CGL_PHYS2D_GRAVITY_Z_Q4` | 1.0 px/step^2 | a dot hit hard can bounce up about 30 px and fall back within 0.3 s |
| `CGL_PHYS2D_LAUNCH_MIN_Q4` | 1.0 px/step | below this planar speed there is no launch ("on the ground means no movement") |
| `CGL_PHYS2D_LAUNCH_GAIN_Q8` | 384 (1.5) | `vz = (speed - 1) x 1.5`, capped by `CGL_PHYS2D_MAX_VZ` |
| `CGL_PHYS2D_LAND_REST_Q8` | 64 (25 %) | vertical velocity retained on landing |
| `CGL_PHYS2D_LAND_MIN_Q4` | 0.5 px/step | below this speed it is absorbed instead of bounced (otherwise dots lying on the ground "buzz" at sub-pixel heights) |

The only criterion for `launch` is "**is it on the ground right now**" (`z == 0`); it deliberately does not require `vz <= 0`:
the landing restitution leaves a resting dot with a small positive `vz` (which is exactly why it does not jitter), and requiring `vz <= 0` would mean the launch never triggers.

### 5.3 Numerical safety

* Fixed-point `int16` overflow protection: positions and velocities stay within `+/-30000` (about +/-1875 px), asserted step by step by the tests;
* `CGL_PHYS2D_MAX_VELOCITY` is clamped after every impulse;
* `resolve()` first does a belt-and-braces "return immediately if dx/dy exceed 1024" (the broad phase only hands out neighbours, this is defence);
* when the two bodies coincide completely (`d == 0`), an arbitrary axis is used to separate them, avoiding a division by zero.

## 6. `cgl_iso2d`: isometric projection + painter's algorithm

**In one sentence**: `cgl_phys2d` still only collides in the XY plane, and `cgl_iso2d` is only responsible for drawing that plane as a
3D space with height. Physics and projection are separated, so the collision feel, the tuning and the tests all stay unchanged.

```
ground_x = ((m00*x + m01*y) >> 12) + ox
ground_y = ((m10*x + m11*y) >> 12) + oy      <- also the depth key
screen_y = ground_y - ((kz*z) >> 12)
```

| Style | Coefficients | Notes |
|---|---|---|
| `ISO` (fitted isometric) | `m00=+kx, m01=-kx, m10=ky1, m11=ky2` | the vertical budget is found by scanning for "maximise the minimum dot spacing" (`cgl_iso2d__split_ky()`) |
| `ISO_UNIFORM` (textbook 30 degrees) | `ky1 == ky2`, `kx = ky*sqrt(3)` (capped by the viewport width) | optimal for a square world (equilateral-triangle lattice); a long world flattens the dot matrix |
| `FLAT` | identity matrix, scaled | `m10 = 0`, the depth key degenerates to `y` (the old flat look) |

**Why the depth key is not `y`**: under an isometric projection the on-screen front/back relation is decided by the camera depth, and the camera depth is the projected
ground y (`m10*x + m11*y`). The dot matrix is a long strip (318 x 42 px): x differs by 300 px while y differs by only a few dozen px
-- sorting by y alone would judge dots 300 px apart in the same row to be at the same depth, and front/back occlusion would be random. Taking `ground_y`
satisfies both modes at once: under isometric it automatically equals the weighted sum of the two axes, and under `FLAT` `m10 = 0` degenerates to y.

**Why counting sort**: the depth key is a **bounded screen coordinate** (0..171 < 256 buckets); bucket sort is O(n + 256),
fully deterministic, has no comparisons and no recursion, needs none of the temporary space of a comparison sort, and does not degrade in the worst case; the result is
"far to near" and can simply be drawn in order, with no z-buffer needed (and the band renderer has nowhere to put one anyway).
The bucket count is `CGL_ISO2D_DEPTH_BUCKETS` in `include/ext/cgl_iso2d_config.h`; only 0..171 are reachable on this panel, and a port that wants the
168 B back can set it to its real panel height (see the WARNING there).

**What the sorter sorts**: *positions*, not rigid bodies. `cgl_iso2d_sort()` takes an `cgl_iso2d_src_t` -- one accessor ("fill me element `i`") plus a
count -- and the world is turned into one by the bridge `include/ext/cgl_iso2d_phys2d.h` (`cgl_iso2d_sort_bodies()`). The indirection is what lets a sprite
list, a particle array or a physics pool all use the same sorter, and it is the reason the whole of `cgl_iso2d` can be compiled and tested with no
physics module at all. Its measured price on the firmware is **+112 B of `.text` and 0 B of RAM**: the accessor's out-of-line
copy, the sorter's two indirect passes and the call site's source descriptor.

**The sort runs only once per step**: the renderer is called per 4 px band (43 bands), and sorting once per band would be a 43-fold waste.
The input carries a `stamp` (the physics bridge passes `world->step`), so repeated calls with the same stamp are no-ops; when the world is
rebuilt or the camera changes, `cgl_iso2d_sorter_reset()` is called to make it sort again for certain.

The tail of `cgl_iso2d_cam_fit()` is worth mentioning: fixed-point rounding makes the actual bounding box 1~2 px larger than the theoretical one,
so it **measures and shrinks again** -- load it into the camera coefficients, project the four corners, and if they go out of bounds reduce the offending dimension by one step and retry
(at most 64 rounds), guaranteeing that after the return the four corners of the world rectangle lie inside the viewport.

## 7. Tuning and observation quick reference

| Want to change | Location |
|---|---|
| Bar height / font size / layout | the macros at the top of `App/app_page_smash.c` + `smash__create()` |
| Hint blink period | `HINT_BLINK_MS` (700 ms) |
| Heavy ball size / speed / entry position / rebuild delay | the macros at the top of `include/ext/cgl_debris_config.h` (`BALL_*`, `CGL_DEBRIS_SETTLE_MS`) |
| Dot spacing / dot size / dot radius | `CGL_DEBRIS_STEP_Q4`, `CGL_DEBRIS_DOT_PX`, `CGL_DEBRIS_DOT_R_Q4` (the three constrain each other) |
| Camera style / floor grid / shadow threshold | `CGL_DEBRIS_SCENE_STYLE`, `CGL_DEBRIS_FLOOR_STEP_PX`, `CGL_DEBRIS_SHADOW_MIN_Z_Q4` |
| Bounce-ball radius / gravity / restitution / spawn | `include/ext/cgl_bounce_config.h` (`CGL_BOUNCE_*`) |
| Bounce-ball playground (where it may go) | `smash__ball_area()` in `App/app_page_smash.c` -- the module deliberately does not decide this |
| Physics domain, grid granularity, velocity limit, body pool | `include/ext/cgl_phys2d_config.h` (overridable `#ifndef`s) |
| Pool placed and sized by the caller instead of inside `cgl_phys2d_t` | `-DCGL_PHYS2D_USER_STORAGE=1` + `cgl_phys2d_init_storage()` / `CGL_PHYS2D_STORAGE_BYTES(n)` (see `include/ext/cgl_phys2d.h` and the CytGL manual, section 15.9) |
| Vertical-axis (airborne) feel | `CGL_PHYS2D_GRAVITY_Z_Q4` / `LAUNCH_GAIN_Q8` / `LAND_REST_Q8` in `include/ext/cgl_phys2d_config.h` |
| Camera angle constants / depth buckets / sort table capacity | `CGL_ISO2D_COS30_Q8` / `SIN30_Q8` / `SQRT3_Q8` / `LIFT_Q8` / `DEPTH_BUCKETS` / `MAX_POINTS` in `include/ext/cgl_iso2d_config.h` |
| The Q12.4 fixed point convention / force-inline shim | `CGL_EXT_Q4_SHIFT` / `CGL_EXT_Q4_ONE` / `CGL_EXT_INLINE` in `include/ext/cgl_ext_basis.h` |
| Menu items and the pages they open | `App/app_page_menu.c` (`s_menu_names[]`, `menu__activate()`) |
| Logo pattern / text | `python tools/mklogo.py ...` (see the CytGL manual, section 18.6) |

Observing these two animations on the board / on the PC does not need a picture:

* Pure physics long run: `build/logo_test.exe` (20 s simulation; prints the cycle, dot count, bounding box, fixed-point overflow and
  2.5D airborne statistics, and asserts that "at least some dots must be knocked up");
* Bounce ball + page fixed step: `build/bounce_test.exe` (walks `cgl_screen_frame` end to end, and asserts
  "the step count is independent of the number of widgets", "while covered the step count does not change at all and there is no catch-up after returning", "the ball survives a logo rebuild", and that the module's drop-in ball widget really paints a disc in the band that holds it);
* The gravity ball engine on its own: `build/cgl_bounce_test.exe` (links `cytgl_ext` with no framework header in its include graph; asserts the exact semi-implicit step, decaying energy over 900 steps, self-healing by body identity, rejected playgrounds, and determinism);
* Projection and sorting: `build/cgl_iso2d_test.exe` (projection formulas, the depth key must come from x+y, the sort is a permutation and monotonic,
  camera fitting stays in bounds, fitted dot spacing > textbook spacing, and on the vertical axis only a fast ball launches).
