# examples/common — the code the two STM32 demos share

`STM32G431CBU6_CytStudio_Demo` and `STM32H563RGT6_CytStudio_Demo` are the same
application on two different boards. Everything that does not depend on the
chip, the pins or the HAL lives here, so a page, the logo, the panel driver or
the key debouncer is written and changed **once**.

## What is shared, and what stays in the project

| Directory | Shared here | Stays in the project |
|---|---|---|
| `App/` | every page, the UI assembly, the logo bitmap, the effect switches (`app_fx_config.h`) | — |
| `Devices/` | the ST7789 panel driver (`st7789.[ch]`) and the `lcd_bus_t` transport contract (`lcd_bus.h`) | — |
| `BSP/` | the key state machine (`board_key.[ch]`) and the transport accessor (`board_lcd.h`) | `board.h` (pin map, key polarity, backlight wiring) and `board_lcd.c` (the SPI + DMA engine: DMA1_Channel2 on the G431, GPDMA1 channel 0 on the H563) |

`cgl_port.c` / `cgl_port.h` deliberately stay in each project: they are the
board's port, the file to read (and to copy) when bringing CytGL up on your own
hardware. Between the two demos they differ only in the port's name string and
in their comments — the framework-facing half is identical.

## How shared code reaches the board

Nothing here names a chip. Where a shared file needs something board specific it
goes through one of the seams below, and the *project* fills it in:

| The shared code wants… | …and gets it from |
|---|---|
| the key's pin, pull resistor and active level | `BSP/board.h` — `BOARD_KEY_PORT`, `BOARD_KEY_PIN`, `BOARD_KEY_PRESSED_LEVEL` |
| the LCD transport instance | `BSP/board_lcd.c` — `board_lcd_bus()`, declared in `BSP/board_lcd.h` |
| the millisecond clock (App/app.c paces its super loop with it) | `cgl_port_board_tick_ms()` in the project's `cgl_port.c` — never a chip HAL header |
| how bright the panel comes up (`App/app_ui.c`) | `CGL_PORT_BOOT_BACKLIGHT_PERCENT` in the project's `cgl_port.h` (100 on the G431, 18 on the H563) |

Both projects put `../common/App`, `../common/BSP` and `../common/Devices` on
their `IncludePaths` and `SourcePaths` (see `CytGL.rproj`), so shared code needs
no path prefix: `#include "app_page_clock.h"` and `#include "board_key.h"`
resolve exactly as they did when those files lived in the project directory.

## Adding a board

1. Start from `examples/STM32G431CBU6_CytStudio_Demo` (or a fresh CubeMX
   project) and keep its `Core/`, `Drivers/`, `BSP/board.h`, `BSP/board_lcd.c`
   and `cgl_port.*`.
2. Point that project's `CytGL.rproj` `IncludePaths` / `SourcePaths` at
   `../common/App`, `../common/BSP` and `../common/Devices`.
3. Set the board's own `CGL_PORT_BOOT_BACKLIGHT_PERCENT`, and the key polarity
   and pin map in `BSP/board.h`.
4. `App/`, `Devices/` and the key state machine come for free.

> **CubeIDE caveat** — the CubeMX-generated `.cproject` / `.project` only scan
> the project directory, so an STM32CubeIDE build of an example needs the three
> `common` folders added as linked folders. This is not needed for the project of
> record: CytStudio (`CytGL.cproj`) and `tools/verify_firmware_build.ps1` both
> resolve the path lists in `CytGL.rproj`.
