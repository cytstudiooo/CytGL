# Hardware, pins and generated code

Target board: **WeAct STM32G431CxUx Core Board V1.0** + **1.47" ST7789 SPI module** (172x320,
this project uses it in landscape at 320x172). The schematic PDF is in this project's own `docs/` directory (not tracked by git, see section 6).

## 1. MCU and clocks

| Item | Value | Source |
|---|---|---|
| Model / package | STM32G431CBU6, UFQFPN48 | `.ioc` `Mcu.CPN` / `Mcu.Package` |
| Core | Cortex-M4 + FPU (hard float ABI, fpv4-sp-d16) | `Project.rproj`, `.cproject` |
| Flash / RAM | 128 KB @0x08000000 / 32 KB @0x20000000 | `STM32G431CBUX_FLASH.ld` |
| HSE | **8 MHz** external crystal (PF0-OSC_IN / PF1-OSC_OUT) | `.ioc`, `main.c` |
| PLL | source HSE, **M=/2 -> 4 MHz**, **N=x85 -> 340 MHz**, **R=/2 -> 170 MHz** (P and Q are /2 as well) | `Core/Src/main.c` `SystemClock_Config()` |
| SYSCLK / HCLK | **170 MHz** (AHB /1) | same as above |
| PCLK1 / PCLK2 | **170 MHz / 170 MHz** (APB /1) | same as above |
| Flash wait states | `FLASH_LATENCY_4` + `PWR_REGULATOR_VOLTAGE_SCALE1_BOOST` | same as above |
| Unused clock sources | there is no LSE / LSI / HSI48 / CSI initialisation | same as above |

> WARNING: after regenerating the code you **must double-check the PLL dividers in `main.c`**: `RCC.IPParameters` in the `.ioc`
> does not store PLLP/Q/R, only the resulting frequency (170 MHz). If the dividers are changed, the SPI bit clock changes with them.

## 2. Wiring table

| LCD silkscreen | MCU pin | Function | Configuration | Code behaviour |
|---|---|---|---|---|
| SCL / SCK | **PA5** | SPI1_SCK (AF5) | push-pull, no pull-up/down, Very High | hardware SCK |
| SDA / MOSI | **PA7** | SPI1_MOSI (AF5) | same as above | hardware MOSI (single line, transmit only) |
| CS | **PA3** | GPIO output (label `LCD_CS`) | push-pull, High, initial value **high = not selected** | software chip select: pulled low before every transfer; raised high after a command transfer, held low during a pixel transfer and wrapped up by `wait_idle()` |
| DC | **PA1** | GPIO output (`LCD_DC`) | push-pull, High, initial value high | `0 = command`, `1 = data` |
| RES | **PB0** | GPIO output (`LCD_RST`) | push-pull, High, initial value high = released | `reset(0)` pulls it low to reset and `reset(1)` releases it; in init, low 30 ms -> high 120 ms |
| BLK | **PB1** | GPIO output (`LCD_BLK`) | push-pull, High, initial value **low = backlight off** | **on/off only, no PWM dimming**; `percent > 0` means high |
| VCC / GND | 3V3 / GND | -- | -- | -- |
| KEY | **PC13** | GPIO input (`KEY`) | **input + internal pull-down** | **pressed = high level**: on the board PC13 -- R4(330R) -- SW2 -- 3.3V; when released the pin floats and is settled by the internal pull-down |
| LED | PC6 | GPIO output | push-pull + pull-up, initial value high = off | **unused** (lit by a low level) |
| SWD | PA13 / PA14 | SWDIO / SWCLK | -- | debug port |

Other idle pins that CubeMX forces to analog input: PA0/PA2/PA4/PA6/PA8...PA12/PA15, PB2...PB9,
PC4/PC10/PC11, PG10 (see `Core/Src/gpio.c`). PC14/PC15 are not configured.

## 3. SPI1

| Item | Value |
|---|---|
| Instance | `hspi1` (SPI1, on APB2) |
| Role / direction | Master, **1-line half-duplex (transmit only)** |
| Data width | initially **8 bit**, switched between 8 / 16 bit at runtime (see below) |
| Clock polarity/phase | CPOL=0, CPHA=0 -> **SPI mode 0** |
| Baud rate | **BR = /4 -> 42.5 MHz** (PCLK2 170 MHz / 4; the value computed by `.ioc` is 42.5 MBits/s) |
| NSS | **software** (`SPI_NSS_SOFT`); the panel CS is driven manually by PA3 |
| Other | MSB first, CRC off, TI mode off, NSSP off |

**Two paths** (`BSP/board_lcd.c`):

| Path | Frame width | Method | Purpose |
|---|---|---|---|
| Command / parameter | **8 bit** | `HAL_SPI_Transmit()` polling, 100 ms timeout | init sequence, `CASET`/`RASET`/`RAMWR` |
| Pixels | **16 bit** | `HAL_SPI_Transmit_DMA()` asynchronous | 320 x 4 = 1280 pixels = 2560 B per band |

Switching the frame width at runtime: `lcd__set_datasize()` changes `hspi1.Init.DataSize` and then calls `HAL_SPI_Init()` again
(the DS bits can only be changed while SPI is disabled, and the HAL disables/enables it for us). Every switch goes through `HAL_SPI_MspInit()`
to re-enable the SPI1 clock, reconfigure PA5/PA7 and call `HAL_DMA_Init(&hdma_spi1_tx)` again.
A 16 bit frame + half-word-aligned DMA, plus the SPI shifting the high byte out first, is exactly the byte order ST7789 expects,
so **the pixel buffer needs no byte swapping**, and `count` in `write_pixels()` is simply the number of pixels.

**Transfer completion is polled**: this project has **no** user-implemented `HAL_SPI_TxCpltCallback` at all;
`lcd__wait_ready()` polls `hspi1.State`, waiting for the DMA completion interrupt to put the state back to READY.
To add a callback, the user-owned places are `USER CODE BEGIN 4` in `main.c` or `USER CODE BEGIN 1` in `spi.c`.

## 4. DMA

| Item | Value |
|---|---|
| Handle / instance | `hdma_spi1_tx` / **DMA1_Channel2** |
| Request | `DMA_REQUEST_SPI1_TX` = **11**, routed through **DMAMUX1** (DMAMUX channel n serves DMA channel n+1) |
| Direction / width | MEM->PERIPH, **half-word/half-word** |
| Mode / priority | **NORMAL** (not circular) / LOW |
| Interrupt | **DMA1_Channel2_IRQn**, pre-emption 0 / subpriority 0, `DMA1_Channel2_IRQHandler()` -> `HAL_DMA_IRQHandler(&hdma_spi1_tx)` |

The length of a single transfer is limited by `(uint16_t)count` (at most 65 535 pixels); each band is 1280 pixels and a full screen is 43 bands.

## 5. Panel driver (`Devices/st7789.c`)

Visible area **172x320** (GRAM 240x320); this project uses `ST7789_ROT_LANDSCAPE = 2`:

| Rotation | width x height | x_off | y_off | MADCTL |
|---|---|---|---|---|
| PORTRAIT (0) | 172 x 320 | 34 | 0 | 0x00 |
| PORTRAIT_R (1) | 172 x 320 | 34 | 0 | 0xC0 |
| **LANDSCAPE (2)** | **320 x 172** | **0** | **34** | **0x70** |
| LANDSCAPE_R (3) | 320 x 172 | 0 | 34 | 0xA0 |

* `st7789_bind()` copies the geometry; **MADCTL is written only once, inside `init()`**, and there is no runtime API to change the rotation.
* Init sequence: hard reset -> `SWRESET` -> `SLPOUT` -> `MADCTL` -> `COLMOD=0x05` (16 bit RGB565)
  -> `PORCTRL/GCTRL/VCOMS/LCMCTRL/VDVVRHEN/VRHS/VDVSET/FRCTR2/PWCTRL1/PVGAM/NVGAM`
  -> **`INVON` (an IPS panel must invert)** -> `NORON` -> `DISPON` -> backlight 100.
* `set_window()` adds the offset and then sends `CASET` / `RASET` / `RAMWR` (three separate transfers).
* The width and height in the device table are **hard-coded as 320x172**; `cgl_port_board_init()`
  (`cgl_port.c`, through `cgl_screen_init()` inside `cgl_port_bind()`) checks that they match
  `CGL_CFG_SCREEN_W/H` and returns non-zero if they do not (a deliberate guard: a geometry mismatch
  would compute transition offsets and dirty rectangles wrong). The application layer no longer
  knows this table exists at all.

## 6. The boundary between `.ioc` and generated code

**Generated by CubeMX; hand-editing is forbidden except inside USER CODE blocks**:
`Core/Src/{main,gpio,dma,spi,stm32g4xx_it,stm32g4xx_hal_msp,system_stm32g4xx,syscalls,sysmem}.c`,
`Core/Inc/*.h`, `Core/Startup/*.s`, `Drivers/**` (HAL + CMSIS), `STM32G431CBUX_FLASH.ld`,
`.mxproject`, `.cproject`, `.project`, `.settings/*`.

**Hand-written (Apache-2.0)**: `BSP/`, `Devices/`, `src/`, `App/`, the port at the project root
(`cgl_port.[ch]`), `tests/`, `tools/`, the root documents and `img/`.

The **only** custom code in the generated files is in the three USER CODE blocks of `Core/Src/main.c`:
`#include "app.h"`, `app_init()` (which calls `Error_Handler()` on failure) and `app_run()` in the main loop.
The USER CODE blocks of `spi.c` / `dma.c` / `gpio.c` / `stm32g4xx_it.c` are **all empty**.

Key settings in `.ioc`: `KeepUserCode=true`, `DeletePrevious=true`, `CoupleFile=true`,
`HeapSize=0x200` / `StackSize=0x400` (consistent with the `.ld`),
`MxCube.Version=6.18.1`, `FirmwarePackage=STM32Cube FW_G4 V1.6.3`, `TargetToolchain=STM32CubeIDE`.

### 6.1 Implicit contracts that must be re-checked item by item after regenerating

1. **GPIO labels**: `LCD_CS / LCD_DC / LCD_RST / LCD_BLK / KEY` -> `Core/Inc/main.h` macros ->
   the mapping in `BSP/board.h`. Change a label and the BSP immediately fails to compile (this is deliberate).
2. **PLL dividers** (see the warning in section 1) and the **SPI prescaler**: 42.5 MHz depends on PCLK2 = 170 MHz.
3. **SPI1 direction/mode**: it must still be 1LINE master, mode 0; if it is changed to interrupt mode or DMA circular mode,
   the busy-wait in `lcd__wait_ready()` must be rewritten.
4. **The DMA data width is still HALFWORD**: the "count = number of pixels" contract in `board_lcd.c` depends on it;
   changing it to BYTE would corrupt the 16 bit pixel path.
5. **PC14/PC15 are marked as LSE-External-Oscillator in the `.ioc`, but the code has no LSE initialisation at all**
   (a pre-existing inconsistency); the `.ioc`'s `EXTERNAL_CLOCK_VALUE=12288000` is not used by any IP either.
6. **Two sets of project files coexist**: `.cproject` (CubeIDE) contains only `Core/` and `Drivers/`;
   the source paths and header paths of BSP/Devices/UI/App are only written in `Project.rproj` (the CytStudio project).
   Regenerating with CubeIDE will not damage the `.rproj`, but neither will it bring BSP/App into its build.

### 6.2 Version control

The project's `.gitignore` ignores its own `/build` and `/docs`. Therefore the **vendor ST7789 examples** and the
**WeAct schematic PDF** under this project's `docs/` are not in the repository (a freshly cloned machine cannot verify the init sequence or the RC wiring of PC13,
it can only rely on the text record in `BSP/board.h`). This project's own Markdown documents need to be in the repository,
so the project's `.gitignore` should ignore `/docs/*.pdf` rather than its whole `/docs/`.

> `.gitignore` does not ignore `Debug/` (the output directory of `.cproject`), so a future CubeIDE build will add
> an un-ignored directory tree.

## 7. The BSP layer: the key (`BSP/board_key.c`)

| Threshold | Value |
|---|---|
| Debounce window | `BOARD_KEY_DEBOUNCE_MS = 20 ms` |
| Long press | `BOARD_KEY_LONG_MS = 600 ms` |
| Long-press repeat period | `BOARD_KEY_REPEAT_MS = 200 ms` |

```
Press  DOWN --(600 ms)--> LONG --(every 200 ms)--> REPEAT ... --> UP
Short press = DOWN + UP    Long press = DOWN + LONG (+REPEAT...) + UP
```

* **Polling instead of EXTI**: the main loop is naturally rate-limited by SPI bandwidth, so a 20 ms debounce is seen at most 1~2 frames late, invisible to the eye;
  an interrupt solution would have to deal with NVIC priorities, contention with the DMA interrupt and "touching UI state inside an interrupt",
  for zero benefit. The whole project has no EXTI initialisation and no EXTI handler.
* `board_key_poll(dt_ms)` is **called once per frame period** (inside `port__poll_input()` in
  `cgl_port.c`, which the framework calls from `cgl_port_frame()` with that frame's dt); debouncing is done by **hold duration**
  rather than sample count, so changing the frame rate needs no retuning; `hold_ms` is accumulated from each frame's `dt_ms`.
* `board_key.c` itself **does not call** `HAL_GetTick` / `HAL_Delay`; it uses only `HAL_GPIO_ReadPin`
  and integer accumulation. Side effect: a huge dt caused by a long debugger halt is accumulated as "hold duration"
  (clamping dt is `cgl_clock`'s job, and `app.c` deliberately does not clamp).

## 8. On-board observation and hardware troubleshooting

| Symptom | Most likely cause |
|---|---|
| Garbled first frame / the whole picture offset | SPI too fast: lower `SPI1.BaudRatePrescaler` by one step in the `.ioc` and regenerate |
| Inverted colours | the ST7789 init is missing `INVON(0x21)` (required for an IPS panel) |
| The first frame waits forever, `s_board.ctx.display.frame_id` stays at 1 | wrong DMA request/channel: SPI1_TX must go through DMA1_Channel2 (DMAMUX1 channel n serves DMA1 channel n+1) |
| Random horizontal stripes | incomplete band rendering / a wrong clip window (every pushed band must be redrawn in full) |
| An element at the edge disappears or pollutes the neighbouring area | `canvas__span()` drops out-of-bounds spans instead of narrowing them, or it tests x but not y |
| The backlight does not light up | PB1's initial value is low; the `set_backlight` driver is on/off only |
| The key needs a long press before anything happens | 20 ms debounce + 600 ms long press; a short press is only decided once `UP` arrives |

On-board observation (J-Link + SWD, for the symbols see the CytGL manual, section 18.5):
`s_board.ctx.display.frame_id` (number of pushed frames), `s_board.ctx.display.bands` (cumulative band count),
`hspi1.State`, `hdma_spi1_tx.Instance->CNDTR`, `s_key.stable/presses/hold_ms`.
