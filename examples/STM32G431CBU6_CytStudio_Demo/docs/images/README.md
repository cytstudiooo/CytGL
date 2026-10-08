# img/ -- real frames rendered on the PC

These are not design mockups: they are the result of **the real UI framework code** rendering on a PC (the same `src/` + `App/` sources,
with only "SPI pushing to the ST7789" replaced by "writing into a memory array").

How they are generated (no development board needed): configure the PC test build, build it, then
run the snapshot tests that write these frames into `build/`:

```bash
cmake -B build/host -DCYTGL_BUILD_TESTS=ON
cmake --build build/host
ctest --test-dir build/host -R "render|bar" --output-on-failure
```

`render_test` writes the 26 `frame00..25` snapshots (page transitions and the Genie "suck-in"),
`bar_test` writes the built-in bar widget's gallery; both land in `build/` as `.ppm` and `.bmp`. This
directory holds snapshot copies of a few of those frames (renamed to `01..07`), which is handy for
referencing from the documentation and for people who do not have gcc installed.

## The two animations in the UI

| Element | Colour | Notes |
|---|---|---|
| Dot-matrix logo | cream `#EEE8D5` | the `CytStudio` assembled from 480 physics balls, rebuilt after the heavy ball smashes it apart |
| Heavy ball | amber `#FFB020` | flies in from off the left edge, ploughs through the dot matrix, and triggers a delayed rebuild after leaving the screen (cycle ~2.6 s) |
| Bounce ball | red `#FF0000` | falls under gravity across **the whole viewport**, bounces off all four walls, passes through the dot matrix and knocks away the dots in its way |

> Both animation widgets have **exactly the same frame** (both are screen y 19..152).
> The bounce ball's active area is the whole viewport (ball centre screen y 22..151), so it passes through the dot matrix
> and knocks away the dots in its way -- a deliberately kept interaction.

Four readouts in the bottom bar: `FPS` frame rate, `DOTS` number of live dots, `SMASH` logo rebuild rounds, `B` cumulative bounce count of the bounce ball.

## Snapshots

| File | tick | Time | Picture | Bottom-bar readouts |
|---|---|---|---|---|
| `01-clean-logo.png` | 10 | 0.17 s | the dot matrix is intact, the heavy ball is still off screen, the bounce ball has just been launched | `FPS 0` `DOTS 480` `SMASH 0` `B 1` |
| `02-ball-entering.png` | 60 | 1.02 s | the heavy ball cuts in, the bounce ball is in the middle | `FPS 58` `DOTS 476` `SMASH 0` `B 2` |
| `03-smash-spread.png` | 90 | 1.53 s | large-scale scatter; the bounce ball is below at y=78 | `FPS 58` `DOTS 471` `SMASH 0` `B 3` |
| `04-settle-aftermath.png` | 120 | 2.04 s | the aftermath, debris scattered | `FPS 58` `DOTS 433` `SMASH 0` `B 4` |
| `05-second-cycle.png` | 200 | 3.40 s | the second SMASH round is in progress | `FPS 58` `DOTS 480` `SMASH 1` `B 6` |
| `06-bar-widget-gallery.png` | `bar_test` output | the built-in `cgl_bar` widget in every shape the test asserts on | four horizontal bars (plain 65%, signed range `-50..50`, border + 4 segments, reversed) and five vertical level meters at 0/25/50/75/100% |
| `07-bounce-ball.png` | 10 | 0.17 s | how the bounce ball looks right after launch (same as `01`) | `B 1` |

> The `FPS` in the table is the clock reading under the PC-side mock driver; it does not represent the frame rate on the board.
> The numeric changes can be cross-checked with `build/logo_test.exe` (pure logo physics) and `build/bounce_test.exe`
> (bounce-ball physics); neither needs hardware.

Related documents:

- [`../../../../README.md`](../../../../README.md) -- project overview and quick start
- [`../application.md`](../application.md) -- the smash animation, the bounce ball, physics and pseudo-3D
- [`../hardware.md`](../hardware.md) -- this board: pinout, SPI/DMA, panel orientation
- the CytGL manual (chapter 3) -- the layers and the life cycle of one frame
- the CytGL manual (chapter 18) -- the PC test suite and the snapshot tooling
