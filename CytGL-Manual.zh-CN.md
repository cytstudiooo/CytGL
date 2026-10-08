<div align="center">

**简体中文** | [English](CytGL-Manual.en.md)

# CytGL 使用手册

<div align="left">

**库版本** 0.1.0（`CGL_VERSION_STRING`，同时提供 `CGL_VERSION_MAJOR` / `CGL_VERSION_MINOR` / `CGL_VERSION_PATCH`）
**语言标准** C11（不使用任何编译器扩展；`CMAKE_C_EXTENSIONS` 关闭）
**目标平台** 无操作系统、无堆的微控制器。参考硬件为 STM32G431CB（Cortex-M4，128 KB Flash / 32 KB RAM）驱动 320×172 的 ST7789 面板。

---

## 关于本手册

本手册是 CytGL 的完整使用说明，自成体系。内容覆盖设计前提、分层结构、一帧的完整执行顺序、移植层实现、画布与光栅化、整数数学、字体、控件树、内置控件、显示与带呈现、时钟与补间、页面与导航、可选扩展模块、资源核算与裁剪、构建与测试、扩展配方，以及故障排查。

阅读约定：

- 正文中出现的路径均为本仓库中的源码路径，用于定位实现，不是需要另行查阅的资料。
- 代码片段均为 C11。为控制篇幅而省略的部分用注释标出，省略的内容不影响该片段要说明的问题。
- 标识符、宏名、函数名、文件名保留英文原名；其余术语在首次出现时给出中英文对照，之后统一使用中文。
- "必须"表示违反后会产生错误结果或难以定位的缺陷；"应当"表示工程上的一致做法；"可以"表示由使用者自行取舍。

---

## 目录

1. 概述
2. 快速上手
3. 架构与一帧的生命周期
4. 移植层：把 CytGL 接到硬件上
5. 基础类型与编译期配置
6. 画布与软件光栅化
7. 整数数学
8. 字体与文本
9. 控件树
10. 内置控件
11. 显示设备与带呈现流水线
12. 时钟与补间
13. 页面与导航
14. 场景
15. 可选扩展模块 `cytgl_ext`
16. 扩展配方
17. 资源占用与裁剪
18. 构建、测试与工具
19. 完整示例：一个可运行的应用骨架
20. 故障排查速查表
附录 A 公共头文件与职责索引
附录 B 编译期配置宏索引
附录 C 可观测计数器索引
附录 D 改动定位速查
附录 E 提交前自检清单与数值一致性

---

## 1. 概述

### 1.1 CytGL 是什么

CytGL 是一个用 C11 编写的保留模式（retained-mode）图形界面框架，面向"没有全屏显存"的微控制器。它的核心判断是：在 32 KB RAM 的器件上，320×172 的 RGB565 全屏缓冲需要 110 KB，"先画满一屏再推送"的常规做法无法成立。CytGL 因此采用**分带渲染**（band rendering）：把屏幕切成若干行高的水平条带，CPU 光栅化下一条带的同时，DMA 把上一条带推给面板。

在此之上，CytGL 提供了一套完整的界面设施：

| 能力 | 说明 |
|---|---|
| 保留模式控件树 | 父子树、绝对坐标、裁剪、剔除、子树平移、命中测试、事件分发 |
| 画布与光栅化 | 像素、线、矩形、圆盘、圆点、抗锯齿圆/粗线/圆弧、位块传送、多种混合路径 |
| 整数数学 | 16 位角度、Q8 正弦/余弦、极坐标映射、范围内比例、整数平方根。不使用 libm |
| 点阵字体 | 8×16 与 12×24 两套 ASCII 字形，逐行直接写缓冲 |
| 内置控件 | 面板 `cgl_panel`、文本标签 `cgl_label`、进度/电平条 `cgl_bar`、径向工具箱 `cgl_arc`、表盘 `cgl_watch` |
| 转场与页面 | 页面对象 + 定长导航栈、12 种转场（含 macOS 风格的 Genie 吸入与 S 形路径） |
| 分组不透明度 | 整棵子树作为一个半透明层合成，不需要全屏缓冲 |
| 单一时间源 | 一个时钟对象同时提供固定步长逻辑调度与逐帧补间 |
| 带呈现 | 双缓冲条带、异步像素推送、损坏矩形累积 |

框架对"什么是一块屏幕"、"时间从哪里来"没有任何假设。时间只通过移植层的 `tick_ms` 进入，输入只通过 `poll_input` 进入，像素只通过一张虚表离开。

### 1.2 设计前提

这些前提决定了代码的形状，也是评估"CytGL 是否适合我的项目"的依据。

| 前提 | 取值 | 对设计的影响 |
|---|---|---|
| RAM | 32 KB | 全屏缓冲不可能，因此采用条带渲染；所有对象都是静态成员，尺寸是编译期预算 |
| Flash | 128 KB | 不依赖 libm，全部使用整数/定点运算；避免 64 位除法辅助函数 |
| 面板接口 | SPI + DMA | 像素推送必须异步，CPU 渲染与总线传输重叠 |
| 输入 | 单个用户按键（参考板） | 所有交互压缩为短按/长按/重复三类事件 |
| 运行环境 | 无 RTOS，无堆 | 整个项目不含 `malloc`；页面、控件、世界状态都是静态存储 |

### 1.3 设计规则

以下七条是框架的承重结构。修改框架或编写应用时违反其中任何一条，都会产生难以定位的显示缺陷。

1. `include/cytgl` 与 `src/` 中不含任何 HAL 头文件、不含 `malloc`、不调用 libm，也不出现具体的屏幕尺寸或字号。依赖只有 C 标准库的 `<stdint.h>`、`<stdbool.h>`、`<stddef.h>`。
2. 时间只以 `dt_ms` 的形式进入框架。库内部不允许调用 `HAL_GetTick()` 之类的时基函数；移植层只报告硬件计时器的读数。
3. 控件树只在 `cgl_page_nav_frame()` 中发生变化，且该调用位于渲染之前。导航只能通过 `cgl_page_nav_*()` 登记，不允许立即生效。
4. `on_step` 每个逻辑步长执行且仅执行一次，并且只对栈顶页面执行。被覆盖的页面的仿真因此从结构上冻结，且不会累积欠账。
5. 绘制图元只对自己的坐标做一次平移；内部函数一律使用屏幕坐标。`cgl_canvas_clear_clip()` 刻意走无平移的内部路径。
6. 推送给面板的条带必须是完整的。分组不透明度的正确性建立在这一点上。
7. 虚表一律使用指定初始化器（`.type = ..., .draw = ...`）。结构体成员增加时，位置初始化器会静默错位。

### 1.4 源码树结构

```
include/cytgl/          框架公共 API（对外交付物）
  cytgl.h               单一入口：一次包含即引入全部公共 API
  cytgl_config.h        全部编译期开关
  cgl_types.h           RGB565 颜色、点、矩形、输入事件
  cgl_math.h            整数三角与定点辅助
  cgl_canvas.h          条带画布 + 软件光栅化（含 warp 变形）
  cgl_font.h            点阵字体
  cgl_widget.h          保留模式控件树
  cgl_panel.h           内置控件：背景/边框容器
  cgl_label.h           内置控件：文本标签
  cgl_bar.h             内置控件：进度/电平条
  cgl_arc.h             径向工具箱：着色面、渐细指针、圆环仪表
  cgl_watch.h           内置控件：模拟表盘（依赖 cgl_arc）
  cgl_display.h         显示驱动虚表 + 带呈现流水线
  cgl_time.h            唯一的时钟
  cgl_anim.h            整数补间 + Q8 缓动曲线
  cgl_page.h            页面对象、导航栈、转场
  cgl_screen.h          显示 + 时钟 + 页面 + 输入 = 一帧
  cgl_port.h            移植契约（使用者需要实现的部分）

include/ext/            可选扩展模块的公共 API，以 <ext/...> 引入
  cgl_ext_basis.h       Q12.4 约定与强制内联宏
  cgl_phys2d.h          定点圆盘刚体物理（含 2.5D 竖轴）
  cgl_iso2d.h           等轴测相机 + 深度排序（不认识刚体）
  cgl_iso2d_phys2d.h    上述两层之间唯一的桥
  cgl_particles.h       粒子引擎（对框架无依赖）
  cgl_debris.h          碎裂场景（位图 → 刚体云、相机拟合、循环）
  cgl_bounce.h          重力球（重力、四壁、回弹系数）
  *_view.h              对应的条带绘制器（唯一认识画布的那一层）
  *_config.h            分层拆分的编译期开关

src/core/               纯逻辑，完全与硬件无关
src/render/             条带呈现与 DMA 流水线
src/port/               移植胶水层（不含任何硬件代码，含一份模板）
src/ext/                可选扩展模块的引擎实现（从不进入 libcytgl.a）

tests/                  宿主（PC）回归测试，逐像素断言真实框架代码
tools/                  资源生成与检查工具（字形、徽标、粒子表、快照查看）
examples/               完整应用示例：两个 STM32 板级工程 + 共用应用代码
```

`include/` 之下是两棵并列的树：`cytgl/` 是框架自身的公共 API，`ext/` 是可选模块的公共 API。二者的引入方式相同（`#include <cytgl/cgl_bar.h>` 与 `#include <ext/cgl_phys2d.h>`），一个 `-I include` 同时服务于两者。可选模块的引擎不进入 `libcytgl.a`，不链接它的构建在 Flash 与 RAM 上均为零开销。

### 1.5 术语

| 术语 | 含义 |
|---|---|
| 条带（band） | 屏幕的一段水平行，高 `CGL_CFG_BAND_H` 行。渲染与推送都以条带为单位 |
| 画布（canvas） | 绑定到某一条带缓冲的绘制上下文，携带裁剪窗口、平移、变形和非线性映射 |
| 平移（pan） | 子树整体的线性位移，由控件树的 `offset` 累加而来 |
| 变形（warp） | 子树整体的非线性映射，用于 Genie 吸入转场；逐行重算落点，不做像素重采样 |
| 损坏（damage） | "需要重绘"的矩形并集。它是全局单例 |
| 组不透明度（group opacity） | 整棵子树先隔离渲染，再作为一个整体按 alpha 合成到背景 |
| 墨色不透明度（ink alpha） | 单个混合图元的不透明度，不需要任何额外内存 |
| 逻辑步长（step） | 固定步长的逻辑推进单位，默认 16 ms，只交给栈顶页面的 `on_step` |
| 帧（frame） | 一次 `cgl_screen_frame()` 调用，包含输入、导航、逻辑、视觉与呈现 |
| 页面（page） | 携带虚表与自身控件树的对象，导航栈的元素 |
| 导航器（navigator） | 页面宿主控件与定长页面栈，负责提交导航请求并推进转场 |
| 场景（screen） | 显示设备 + 时钟 + 导航器 + 输入队列的组合体，对外只暴露"一帧" |

---

## 2. 快速上手

### 2.1 构建静态库

```bash
cmake -B build -DCYTGL_PORT_TEMPLATE=OFF
cmake --build build
# 产物：build/libcytgl.a
```

顶层 `CMakeLists.txt` 提供四个选项：

| 选项 | 默认 | 作用 |
|---|---|---|
| `CYTGL_BUILD_TESTS` | `OFF` | 构建宿主（PC）回归测试并注册到 CTest |
| `CYTGL_BUILD_EXAMPLES` | `OFF` | 加入 `examples/` 子目录（其中的固件目标仅在具备 ARM 工具链与项目设置的检出中存在） |
| `CYTGL_BUILD_EXT` | `OFF` | 同时构建可选扩展模块，产出 `cytgl_ext` / `cytgl::ext` |
| `CYTGL_PORT_TEMPLATE` | `ON` | 把 `src/port/cgl_port_template.c` 作为语法检查编译进库 |

库目标名为 `cytgl`，别名 `cytgl::cytgl`。安装规则把 `libcytgl.a` 放入 `lib/`，把 `include/cytgl/` 放入 `include/`。

### 2.2 集成到自己的工程

作为子项目引入：

```cmake
add_subdirectory(third_party/CytGL)
target_link_libraries(my_app PRIVATE cytgl::cytgl)
```

不使用 CMake 时，把 `src/core`、`src/render`、`src/port` 下的全部 `.c`（`src/port/cgl_port_template.c` 除外）加入工程，并把 `include/` 加入包含路径。

**包含路径规则（务必遵守）**：框架源码以纯文件名包含自己的头文件（例如 `#include "cgl_screen.h"`），该写法在源文件所在目录内解析。因此不要把 `src/core` 与 `include/` 同时加入全局包含路径——公共头与私有头同名，解析顺序将变得不确定。全局包含路径中只应出现 `include/`。

### 2.3 最小可运行程序

以下程序不依赖任何硬件，在 PC 上即可运行：显示驱动把条带写入内存数组，可以据此验证整条链路，也可以把结果导出为 PPM 图像查看。

```c
#include <cytgl/cytgl.h>
#include <stdio.h>

/* ---- 1. 显示驱动：把像素写入内存数组（等效于"面板"） ---- */
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

/* 真实硬件上这里必须启动 DMA 后立即返回；内存模拟直接拷贝即可 */
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
    .set_backlight = NULL,               /* 可选成员，允许为 NULL */
    .width = CGL_CFG_SCREEN_W,           /* 必须与编译期配置一致 */
    .height = CGL_CFG_SCREEN_H,
};

/* ---- 2. 时钟：单调递增的毫秒计数 ---- */
static uint32_t mock_tick_ms(void)
{
    static uint32_t ms;
    ms += 16u;                            /* 模拟每帧 16 ms */
    return ms;
}

/* ---- 3. 界面：一个面板 + 一个标签 ---- */
static cgl_panel_t s_bg;
static cgl_label_t s_title;

int main(void)
{
    static cgl_port_ctx_t port;
    static const cgl_port_t desc = {
        .name = "host",
        .display = &s_mock_drv,
        .display_ctx = &s_panel,
        .tick_ms = mock_tick_ms,          /* 提供时钟即无需自己计算 dt */
        .poll_input = NULL,               /* 无输入源 */
    };

    if (cgl_port_bind(&desc, &port, NULL) != 0) { return 1; }

    cgl_screen_t *screen = cgl_port_screen(&port);
    cgl_widget_t *root   = cgl_screen_root(screen);   /* 导航器宿主，覆盖整屏 */

    cgl_panel_init(&s_bg, "bg",
                   cgl_rect(0, 0, CGL_CFG_SCREEN_W - 1, CGL_CFG_SCREEN_H - 1),
                   CGL_CFG_BG_COLOR, CGL_CFG_BG_COLOR);
    (void)cgl_widget_add_child(root, &s_bg.base);

    cgl_label_init(&s_title, "title", cgl_rect(16, 16, 240, 40),
                   &cgl_font_12x24, "Hello CytGL",
                   CGL_CFG_FG_COLOR, CGL_CFG_BG_COLOR, false);
    (void)cgl_widget_add_child(&s_bg.base, &s_title.base);

    cgl_screen_redraw_all(screen);        /* 首帧强制全屏重绘 */

    for (int i = 0; i < 4; ++i) { (void)cgl_port_frame(&port); }

    /* 此时 s_fb 中即为整屏像素，可写出 PPM 图像检查 */
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

该示例的关键点：

- `cgl_port_bind()` 的第三个参数传 `NULL` 时，框架在内部持有一个静态导航器，并用整屏矩形初始化它。需要自建导航器时传入自己的实例。
- `cgl_screen_root()` 返回导航器宿主控件；直接挂控件即可得到没有页面模型的界面。使用页面模型时，控件应挂在页面的根控件上（见第 13 章）。
- `cgl_screen_redraw_all()` 在首次进入循环前调用。虽然 `cgl_display_init()` 本身标记了全屏损坏，显式调用可以表达意图。
- 控件必须自行绘制自己占用的整个矩形。框架不会清除背景：条带渲染没有任何"上一帧"可供恢复（见 10.1 与 11.4）。

### 2.4 在 PC 上运行回归测试

```bash
cmake -B build/host -DCYTGL_BUILD_TESTS=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

测试编译并运行**真实的框架源码**，只把显示驱动替换为写入内存数组的模拟驱动，因此断言检查的就是出货代码路径。渲染类测试还会输出可直接打开的 `frameNN.ppm` / `.bmp` 快照。单独运行一个测试：

```bash
ctest --test-dir build/host -R alpha --output-on-failure
```

---

## 3. 架构与一帧的生命周期

### 3.1 分层与依赖方向

```
         应用层（App）
            |  只向下依赖
     cgl_port.c/.h      移植层：把硬件与框架绑定的唯一文件
            |
         CytGL 框架      类型 / 画布 / 字体 / 控件 / 显示 / 时钟 / 补间 / 页面 / 场景
            |            <- 不含 HAL 头、不含 malloc、不含 libm
        面板驱动          例如 ST7789，只认识一条总线虚表
            |
        板级支持（BSP）    引脚、SPI、DMA、按键，只认识 CubeMX 句柄
            |
        Core/ + Drivers/  CubeMX / HAL 生成代码

  include/ext + src/ext  不在这条栈上：它是栈旁的一个可选模块
```

依赖是严格单向的。可移植性的判据是：把 `src/` 整棵树复制到任何其他工程，它仍然可用——因为它不知道屏幕是什么，也不知道时间从哪里来。

| 层 | 可以依赖 | 绝不能做 |
|---|---|---|
| 应用层 | 框架公共 API、可选扩展模块、`cgl_port.h` | 知道某个 SPI 实例、某个引脚、面板方向或某个毫秒计数 |
| 移植层 | 框架契约、面板驱动、BSP、HAL | 包含界面逻辑（页面、控件、布局） |
| 可选模块（引擎） | 仅 C 标准库 | 包含框架头文件、分配内存、知道屏幕的存在 |
| 可选模块（绘制器） | 引擎与 `cgl_canvas.h` / `cgl_types.h` | 分配内存、知道"控件"的存在 |
| 框架 | 仅 C 标准库 | `stm32g4xx_hal.h`、具体颜色、具体字号、`malloc` |
| 面板驱动 | 总线虚表 | 触摸寄存器（那是 BSP 的职责） |
| 板级支持 | HAL 句柄与 `.ioc` 生成的宏 | 知道"面板"或"界面"的概念 |

### 3.2 一帧的调用链

```
app_run()                                应用主循环
 +- cgl_port_frame(&port)                移植层帧入口
     +- dt = tick_ms() - last_tick       无符号减法，回绕安全
     +- poll_input(dt)                   移植层的按键/触摸扫描，只投递事件
     +- cgl_screen_frame(&screen, dt)    框架侧唯一的一帧
        1) cgl_clock_begin(dt)          全项目唯一累积时间的地方
        2) 排空输入队列                  在帧边界分发（转场期间不消费队首）
        3) cgl_page_nav_frame()         提交页面变更、推进转场
        4) while (cgl_clock_next_step()) page->on_step(step_ms)
        5) page->on_frame(&clock)       每帧一次：补间、状态机、文本刷新
        6) cgl_widget_update_tree()     纯视觉动画
        7) cgl_display_present()        逐条带光栅化 + DMA 推送
```

三条关于顺序的硬性规则：

1. **只有一个时间源。** 时钟是全项目唯一累积时间的地方；移植层只报告硬件计时器的读数（`cgl_port_t::tick_ms`），其余六步的顺序由框架保证。项目中不存在"必须先调用 A 再调用 B，否则物理冻结"这类要求。
2. **控件树只在帧的起点变化。** 只有 `cgl_page_nav_frame()` 会调用 `cgl_widget_add_child()` / `remove_child()` / `set_visible()`，而它运行在 `on_step` 与呈现之前，因此渲染期间控件树是常量——不会出现"画到一半树变了"。
3. **逻辑与视觉是两个域。** 物理只跑在 `on_step`（固定步长、可重放）；补间与文本刷新跑在 `on_frame`（跟随帧率）。

### 3.3 时间模型：两个域，各有一个所有者

| 域 | 字段 | 使用位置 | 特性 |
|---|---|---|---|
| 逻辑（固定步长） | `step_ms`（默认 `CGL_CFG_CLOCK_STEP_MS` = 16 ms） | 页面 `on_step` | 固定步长、与帧率无关，仿真速度不受 SPI 带宽影响 |
| 视觉（逐帧） | `dt_ms`（上限 100 ms） | 页面 `on_frame`、控件 `update` | 跟随帧率，允许丢帧 |

- `cgl_clock_begin()` 由累加器算出"本帧应执行的步数"（上限 `CGL_CFG_CLOCK_MAX_STEPS` = 4），`while (cgl_clock_next_step())` 恰好迭代该次数。页面既不能多跑一步，也不会少跑。
- **卡顿不会被追补**：单帧 `dt` 超过 `max_dt_ms` 的部分被截断；当欠下的整步数超过上限时，整步欠账一次性丢弃（保留子步长余数）。宁可让仿真时间略微走慢，也不做"卡顿后快进"。
- 帧率由累积时间算出（500 ms 统计窗口），与仿真共用同一份时间，因此不会出现"显示 58 fps 而物理按 60 Hz 运行"的错觉。
- 采用 16 ms 而非 16.67 ms：时钟以整毫秒计数，二者每秒 62.5 步与 60 步的 4% 差异在本项目的任何调参常量上都不可感知，同时免除了浮点运算。

### 3.4 带呈现流水线

```
buffer[cur]  光栅化 -> set_window(条带) -> write_pixels（DMA，异步）   ] CPU 继续
buffer[!cur] 光栅化（此时 DMA 仍在移出上一条带）                      ] 与总线并行
                                       -> wait_idle() -> 下一个窗口 ...
```

| 项目 | 取值 |
|---|---|
| 条带缓冲 RAM | 2 × 320 × 4 × 2 B = 5 KB |
| 全屏推送 | 172 / 4 = 43 条带 |
| 损坏处理 | 损坏矩形覆盖到的每个**完整条带**都被重绘（面板没有帧缓冲，交给它的条带必须是完整的） |
| 光栅化顺序 | 每个图元先加 `pan` 得到屏幕坐标，再裁剪，最后落到条带内的行 |

两条必须维持的约束（分组不透明度依赖它们）：

1. **每个条带都必须从控件树根开始完整重绘。** `cgl_display_present()` 从不"只重绘损坏部分"。save-under 合成器的正确性也依赖这一点：它保存的"背景副本"就是本条目带已经绘制好的内容；一旦真的实现局部重绘，背景就会过期，混合结果立即出错。
2. **`cgl_display_t.alpha[]` 只由 CPU 写入，永不进入 DMA。** 它与 `band[]` 不同：当第 N 条带在渲染时，`band[!cur]` 正被 DMA 读取，因此那一块不能用作暂存区。

### 3.5 三个解耦缝

| 缝 | 位置 | 隔离的对象 | 收益 |
|---|---|---|---|
| `cgl_display_drv_t` | `include/cytgl/cgl_display.h` | 像素如何到达面板 | 换成写内存数组的模拟驱动后，真实框架代码可以在 PC 上逐像素回归测试 |
| 总线虚表（示例工程中的 `lcd_bus_t`） | `examples/common/Devices/lcd_bus.h` | 电信号如何离开芯片 | 硬件 SPI+DMA、软件模拟、并口、仿真器可以互换，面板驱动一行不改 |
| `cgl_widget_vt_t` | `include/cytgl/cgl_widget.h` | 一个控件长什么样 | 新增控件 = 一个以 `cgl_widget_t` 为首成员的结构体加一张虚表 |

唯一未抽象成缝的地方是损坏汇（damage sink）：它是全局单例，因此同一时刻只能有一个活动的 `cgl_screen`。这是单屏设备的充分条件；若需支持两块屏，应把汇改为每屏一个。

---

## 4. 移植层：把 CytGL 接到硬件上

### 4.1 契约总览

框架对硬件只有三个要求，全部通过一个 `cgl_port_t` 描述符一次性交给 `cgl_port_bind()`：

| # | 内容 | 类型 | 必备 |
|---|---|---|---|
| 1 | 显示驱动：像素如何到达面板 | `const cgl_display_drv_t *` | 是 |
| 2 | 毫秒时钟：时间从哪里来 | `uint32_t (*)(void)` | 否（若自行计算 dt） |
| 3 | 输入源：扫描并投递事件 | `void (*)(void *, uint32_t)` | 否 |

绑定之后，每帧只需要一次 `cgl_port_frame()`，它内部完成"取时间 → 扫描输入 → 推进一帧"。

### 4.2 `cgl_port_t` 字段

| 字段 | 类型 | 语义 |
|---|---|---|
| `name` | `const char *` | 板级/移植层名称，用于调试与日志 |
| `display` | `const cgl_display_drv_t *` | 显示驱动，必须提供 |
| `display_ctx` | `void *` | 传给驱动回调的上下文（通常是面板对象），可以为 `NULL` |
| `tick_ms` | `uint32_t (*)(void)` | 单调递增毫秒计数。可以为 `NULL`，此时框架以固定 `CGL_CFG_CLOCK_STEP_MS` 推进 |
| `poll_input` | `void (*)(void *, uint32_t)` | 输入源钩子。每帧调用一次，参数为该帧即将使用的 `dt_ms`，调用时机早于屏幕排空输入队列，因此此处产生的事件由本帧处理 |
| `input_ctx` | `void *` | 传给 `poll_input` 的参数 |
| `board_init` | `int (*)(void *)` | 上电初始化钩子，可以为 `NULL`。在 `cgl_port_bind()` 内、显示驱动 `init()` 之前调用。适合引脚配置、总线时钟、背光 PWM 启动 |
| `board_ctx` | `void *` | 传给 `board_init` 的参数 |

### 4.3 显示驱动 `cgl_display_drv_t`

```c
typedef struct cgl_display_drv
{
    const char *name;

    int  (*init)(void *ctx);                                       /* 上电 */
    void (*set_window)(void *ctx, uint16_t x0, uint16_t y0,
                       uint16_t x1, uint16_t y1);                  /* 开窗，两端闭区间 */
    void (*write_pixels)(void *ctx, const cgl_color_t *px,
                         uint32_t count);                          /* 异步推送 */
    void (*wait_idle)(void *ctx);                                  /* 等待总线空闲 */
    void (*set_backlight)(void *ctx, uint8_t percent);             /* 可以为 NULL */

    int16_t width;   /* 必须等于 CGL_CFG_SCREEN_W */
    int16_t height;  /* 必须等于 CGL_CFG_SCREEN_H */
} cgl_display_drv_t;
```

**`init`**：上电、复位、写入控制器寄存器序列。成功返回 0，失败返回非零。`cgl_port_bind()` 会传递该失败，因此"面板没起来"不会变成"界面半工作"。

**`set_window`**：编程写入窗口。两个坐标都是**闭区间**，与 ST7789 / ILI9341 的 `CASET` / `RASET` 约定一致。框架每条带调用一次，实现应当尽量精简；常见优化是仅当 `y` 变化时跳过 `CASET` / `RASET` 的重复下发。窗口坐标是面板坐标而非显存坐标：若可见区位于更大的 GRAM 内，偏移量应在此处补上。

**`write_pixels`**：**必须异步**（典型实现是启动 DMA 后立即返回）。整个性能模型建立在"CPU 光栅化下一条带"与"总线上在移出上一条带"的重叠之上。阻塞式实现仍然能工作，但帧率大约减半（42.5 MHz SPI 下推满一帧 110 KB，异步约 48 fps，同步约 24 fps）。传入的指针指向 `cgl_display_t.band[]` 中的一条带缓冲，其有效期到 `wait_idle()` 返回为止。三个常见错误：

- 送出像素前 DC 必须为高（数据模式）；命令与参数需要 DC 为低。颜色错位一个像素通常源于此。
- CS 必须在整个传输期间保持有效。由于函数立即返回，CS 不能在函数内释放，应放到 `wait_idle()` 中。
- 一个 RGB565 像素对应一个 16 位 SPI 帧。按字节发送会让 DMA 逐字节仲裁总线，像素率大约减半。因此 `count` 是**像素数，不是字节数**。

**`wait_idle`**：阻塞到 `write_pixels` 启动的传输完成，然后释放 CS 及其他每传输状态。STM32 HAL 下即等待 SPI 状态回到 `HAL_SPI_STATE_READY`。

**`set_backlight`**：可选，传 `NULL` 时框架安全跳过。`percent` 为 0..100；只支持开/关的背光把任何非零值当作开即可。

**`width` / `height`**：面板在**横向**方向上的几何尺寸（宽 > 高）。CytGL 只按横向渲染。`cgl_port_bind()` 会把这两个值与 `CGL_CFG_SCREEN_W` / `CGL_CFG_SCREEN_H` 比对，不一致时拒绝启动。这是刻意的：静默的不一致会得到一张轻微撕裂或偏移的图像，极难定位；而拒绝绑定会直接指向配置项。

### 4.4 时钟

```c
static uint32_t my_tick_ms(void)
{
    return (uint32_t)HAL_GetTick();      /* 或 SysTick，或 clock_gettime()/1e6 */
}
```

要求：单调递增、分辨率 1 ms 足够、不得倒退。框架以无符号减法计算 `dt = now - last`，因此单次回绕无害。

**不要在这里累加自己的计数器。** CytGL 是系统中时间的唯一所有者：调用者提供 `dt`，框架据此调度其余一切。移植层私有的计数器会重新引入"谁先 tick"这一类缺陷，而时钟的存在正是为了消除它。

### 4.5 输入

框架从不主动轮询任何设备，但会每帧调用一次移植层的轮询钩子。把扫描结果翻译成 `cgl_event_t` 并**投递**：

```c
static void my_poll_input(void *user, uint32_t dt_ms)
{
    cgl_port_ctx_t *port = (cgl_port_ctx_t *)user;
    key_event_t k = key_scan(dt_ms);              /* 去抖使用传入的 dt */

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

`dt_ms` 是即将运行的那一帧的 dt，因此这里是唯一能正确处理去抖与长按计时的地方——不要在此读取时钟。若板卡没有输入，或事件来自其他路径（触摸中断、串口协议、仿真器），把 `poll_input` 留为 `NULL`，在事件产生处自行调用：

```c
cgl_port_post_event(&g_port, &ev);   /* 等价于 cgl_screen_post_event(cgl_port_screen(&g_port), &ev) */
```

两条规则使输入处理保持简单：

1. **只投递，不分发。** 事件先入队，在帧边界统一分发，因此页面不会在帧中间被进入或离开。队列长度由 `CGL_CFG_INPUT_QUEUE` 决定（默认 8，必须为 2 的幂）；队列满时事件被丢弃并计入计数器，可用 `cgl_screen_dropped_events()` 读取。因此在中断中调用 `cgl_port_post_event()` 是安全的，但去抖/扫描本身应留在帧内完成。
2. **只保留一个生产者。** 用一个函数扫描全部输入并投递，每帧携带该帧的 dt 调用一次——这正是 `poll_input` 的语义，也是它优于在应用层各处投递事件的原因。触摸支持不需要额外的框架代码：投递 `CGL_EV_TOUCH_*` 即可，命中测试已由 `cgl_widget_hit_test()` 实现。

参考示例展示了单按键的处理方式：一次长按被转换为 `CGL_EV_KEY_LONG` 后紧跟一个 `CGL_EV_BACK`，于是"长按返回"不需要每个页面各自实现。

### 4.6 框架侧实例与 API

```c
typedef struct cgl_port_ctx
{
    const cgl_port_t *port;   /* 已绑定的描述符                     */
    cgl_display_t display;    /* 显示实例（含条带缓冲，静态存储）    */
    cgl_screen_t screen;      /* 场景：显示 + 时钟 + 导航器 + 输入   */
    uint32_t last_tick;       /* 上一帧的时间戳                     */
    bool bound;               /* 绑定是否成功                       */
} cgl_port_ctx_t;
```

调用者把该结构视为不透明对象；成员公开只是为了免除堆分配。

| API | 语义 |
|---|---|
| `int cgl_port_bind(const cgl_port_t *port, cgl_port_ctx_t *ctx, cgl_page_nav_t *nav)` | 依次执行 `board_init` → 绑定显示驱动 → 初始化场景。`nav` 传 `NULL` 时框架使用内部静态导航器并以整屏矩形初始化它。返回 0 表示成功；非零表示 `board_init`、显示初始化或几何校验失败 |
| `uint32_t cgl_port_frame(cgl_port_ctx_t *ctx)` | 推进一帧：计算 dt → `poll_input(dt)` → `cgl_screen_frame()`。返回本次使用的 dt（未绑定时返回 0） |
| `cgl_display_t *cgl_port_display(ctx)` / `cgl_screen_t *cgl_port_screen(ctx)` | 取框架对象，未绑定时返回 `NULL` |
| `bool cgl_port_post_event(ctx, const cgl_event_t *ev)` | 投递一个输入事件 |
| `void cgl_port_set_backlight(ctx, uint8_t percent)` | 设置背光；驱动未实现时是安全的空操作 |

若应用需要自己控制节拍（RTOS 节拍钩子、固定 16 ms 定时器、60 Hz 仿真器），跳过 `cgl_port_frame()`，直接调用 `cgl_screen_frame(&screen, dt_ms)`；此时输入扫描也由调用者以同样的 dt 驱动。

### 4.7 移植清单

- [ ] 已实现 `cgl_display_drv_t`；`init` 失败时返回非零。
- [ ] `set_window` 两端闭区间，且使用面板坐标（模块所需偏移已补上）。
- [ ] `write_pixels` 在启动异步传输后立即返回。
- [ ] `wait_idle` 阻塞到该传输完成，然后释放 CS。
- [ ] `drv->width/height` 等于 `CGL_CFG_SCREEN_W/H`，且为横向。
- [ ] `tick_ms` 返回来自系统节拍的单调毫秒计数。
- [ ] `poll_input`（若使用）只投递事件，由单一生产者驱动，自身不做分发。
- [ ] `cgl_port_bind()` 返回 0。
- [ ] 首帧有画面：调用 `cgl_screen_redraw_all()` 后，面板上出现内容。
- [ ] 颜色正确（未发生字节交换）：红色方块显示为红色。
- [ ] 方向正确：正方形未显示为矩形。

### 4.8 移植故障排查

| 现象 | 可能原因 |
|---|---|
| `cgl_port_bind()` 立即返回非零 | `drv->width/height` 与 `CGL_CFG_SCREEN_W/H` 不一致，或 `init` 失败 |
| 图像整体固定偏移 | 模块可见区位于更大的 GRAM 内，需要在 `set_window` 中补偏移 |
| 图像撕裂、有移动的水平接缝 | `write_pixels` 实际不是异步，或 CS 释放过早 |
| 颜色错误/通道互换 | 字节序问题。RGB565 是本机序，检查是否做了多余的字节交换以及 `COLMOD` 是否设为 16 位 |
| 颜色反相 | IPS 面板通常需要 `INVON` |
| 全屏只有一种颜色（通常是背景色） | 损坏区为空：没有控件调用 `cgl_widget_invalidate()`。调用 `cgl_screen_redraw_all()` |
| 只画了顶部若干行 | 几何不一致：驱动报告竖向而配置为横向（或相反） |
| 帧率约为总线允许值的一半 | `write_pixels` 是同步的，CPU 与总线不再重叠 |
| 大约缺少一条带，或图像跳动 | 交给 `write_pixels` 的缓冲在 `wait_idle` 之前被复用；确认 DMA 确实结束 |
| 编译期 `_Static_assert` 报 `CGL_CFG_ALPHA_BUDGET` | 分组不透明度所需 RAM 超过预算。降低 `CGL_CFG_ALPHA_LEVELS` / `CGL_CFG_BAND_H`，或有意提高预算 |

---

## 5. 基础类型与编译期配置

### 5.1 颜色

`cgl_color_t` 是 16 位本机 RGB565。物理总线/面板的字节序属于显示驱动的职责，框架不介入。

```c
#define CGL_RGB565(r, g, b)   /* 5-6-5 打包 */
```

预定义颜色：`CGL_COLOR_BLACK`、`CGL_COLOR_WHITE`、`CGL_COLOR_RED`、`CGL_COLOR_GREEN`、`CGL_COLOR_BLUE`、`CGL_COLOR_YELLOW`、`CGL_COLOR_CYAN`、`CGL_COLOR_MAGENTA`、`CGL_COLOR_GRAY`、`CGL_COLOR_DARK`、`CGL_COLOR_DIM`、`CGL_COLOR_AMBER`、`CGL_COLOR_CREAM`。

框架中唯一的混合公式（所有混合路径都经过它）：

```c
cgl_color_t cgl_color_blend_q8(cgl_color_t dst, cgl_color_t src, uint16_t alpha_q8);
```

- `alpha_q8` 为 Q8：0 完全保留 `dst`，256 完全变为 `src`。两个端点都是精确的，因此调用者可以安全地传入端点值。
- 取整方式为**截断**（`>>8`）而非四舍五入。这是框架既定行为，改变它会改写所有既有图像的像素值。代价是每级最多 1 LSB 的误差（5 位通道的 3.2%），两级嵌套时最多 2 LSB。

### 5.2 几何

坐标全部为有符号的屏幕像素。矩形两端**闭区间**：`width == x1 - x0 + 1`。

```c
typedef struct { int16_t x, y; }                       cgl_point_t;
typedef struct { int16_t x0, y0, x1, y1; }             cgl_rect_t;
```

| 内联函数 | 语义 |
|---|---|
| `cgl_rect(x0, y0, x1, y1)` | 由四边构造 |
| `cgl_rect_wh(x, y, w, h)` | 由左上角与宽高构造（内部转换为闭区间） |
| `cgl_rect_none()` | 保证为空的矩形（`x0 > x1`） |
| `cgl_rect_is_empty(r)` | 是否为空 |
| `cgl_rect_width(r)` / `cgl_rect_height(r)` | 宽/高 |
| `cgl_rect_contains_point(r, x, y)` | 点是否在矩形内 |
| `cgl_rect_contains_rect(r, o)` | 矩形是否完全包含另一矩形（空矩形返回真） |
| `cgl_rect_intersect(a, b)` / `cgl_rect_union(a, b)` | 交/并（并对空矩形有特判） |
| `cgl_rect_translate(r, d)` | 平移 |

### 5.3 输入事件

```c
typedef enum {
    CGL_EV_NONE = 0,
    CGL_EV_KEY_DOWN,     /* 刚按下                       */
    CGL_EV_KEY_REPEAT,   /* 按住时的自动重复             */
    CGL_EV_KEY_LONG,     /* 超过阈值，只发出一次          */
    CGL_EV_KEY_UP,       /* 松开（hold_ms 为总时长）      */
    CGL_EV_TOUCH_DOWN,
    CGL_EV_TOUCH_MOVE,
    CGL_EV_TOUCH_UP,
    CGL_EV_BACK          /* 逻辑"返回"（编码器侧键/手势/远程） */
} cgl_event_kind_t;

typedef struct {
    uint8_t  kind;      /* cgl_event_kind_t                       */
    uint8_t  key;       /* 键索引；单键板恒为 0                    */
    int16_t  x, y;      /* 触摸坐标（键盘事件为 0）                */
    uint32_t hold_ms;   /* 从按下到本事件经过的时间                */
} cgl_event_t;
```

事件是"一次性动作"，与几何无关，因此不属于控件树；控件树只负责向下分发，是否接收由页面决定。

### 5.4 编译期配置宏

全部开关位于 `include/cytgl/cytgl_config.h`，且几乎都被 `#ifndef` 保护，因此可以由构建系统用 `-D` 覆盖，而不必修改库源码。这一点在多产品共用一份检出时尤其重要。

**面板几何与条带**

| 宏 | 默认 | 作用 |
|---|---|---|
| `CGL_CFG_SCREEN_W` / `CGL_CFG_SCREEN_H` | 320 / 172 | 面板几何的**唯一来源**；驱动几何不匹配时 `cgl_screen_init()` 返回 -1 |
| `CGL_CFG_BAND_H` | 4 | 条带高度。RAM 与 SPI 开窗命令开销之间的折中（两条缓冲共 5 KB） |

**分组不透明度**

| 宏 | 默认 | 作用 |
|---|---|---|
| `CGL_CFG_ALPHA` | 1 | 功能总开关。置 0 时显示层不分配暂存区，`cgl_widget_set_alpha()` 退化为"记录数值但不生效"（会计数），RAM 归零 |
| `CGL_CFG_ALPHA_LEVELS` | 2 | 可同时嵌套的分组层级数，每级占一条带的 RAM。允许范围 1..8，越界由 `_Static_assert` 拦截 |
| `CGL_CFG_ALPHA_LEVEL_STRIDE` | `SCREEN_W * BAND_H` | 每级的像素数，必须不小于实际条带宽度 × 条带高度 |
| `CGL_CFG_ALPHA_BUDGET` | 6144 | 该功能允许占用的 RAM 上限（字节）。超出是**编译期**错误，不会变成运行时栈溢出 |
| `CGL_CFG_ALPHA_BYTES` | 派生 | 由上述三项算出的实际占用，可直接在编译期断言中使用 |

**功能上限**

| 宏 | 默认 | 作用 |
|---|---|---|
| `CGL_CFG_LABEL_MAX_TEXT` | 32 | 每个 `cgl_label_t` 的静态文本缓冲（含结尾 `\0`） |
| `CGL_CFG_WIDGET_MAX_CHILDREN` | 16 | 单个控件的**直接**子控件上限。该限制被强制执行：超限时 `cgl_widget_add_child()` 返回 false，并计入 `cgl_widget_children_dropped()`。不占用额外 RAM（子链表在构建期遍历计数） |

**径向模块开关**

| 宏 | 默认 | 作用 |
|---|---|---|
| `CGL_CFG_ARC` | 1 | 径向工具箱（着色圆面、渐细抗锯齿指针、圆环仪表）。置 0 时整个模块从链接中消失 |
| `CGL_CFG_WATCH` | 1 | 表盘控件（时基与日历、分度环、三根指针、轴心、玻璃罩）。**要求 `CGL_CFG_ARC`**，二者关系由 `_Static_assert` 检查 |
| `CGL_CFG_WATCH_DIAL_LUT` | 跟随 `CGL_CFG_WATCH` | 是否烘焙两张**默认**表盘查找表（256 项表面 + 33 项凹槽，578 B Flash）。置 0 时表消失，`lut` 为 `NULL` 的表面改用程序化着色，每像素略慢但不占 Flash |
| `CGL_CFG_WATCH_TRAIL` | 跟随 `CGL_CFG_WATCH` | 秒针彗尾：每格亮度 60 B 加一个衰减累加器。这是表盘唯一的逐元素状态，也是内存紧张时最值得编译掉的部分 |
| `CGL_CFG_WATCH_GLASS` | 跟随 `CGL_CFG_WATCH` | 玻璃罩通道：在指针**之上**合成一层反射。纯 Flash 开销，但每帧多一次整盘通道 |

`CGL_CFG_WATCH` 与 `CGL_CFG_ARC` 相互独立是刻意的：工具箱是通用设施，表盘只是它的一个应用。仅设 `-DCGL_CFG_ARC=0` 会在断言处停下，并给出还需同时设置的开关名，这比"一个默认值悄悄把整块表盘拖进工具箱"更好。

**表盘几何（默认值，不是限制）**

`cgl_watch_geometry_default()` 由下列宏填充 `cgl_watch_geom_t`；想改盘面尺寸时改自己的结构体即可，无需触碰这些宏。

| 宏 | 默认 | 含义 |
|---|---|---|
| `CGL_CFG_WATCH_CX` / `_CY` | 86 / 86 | 盘面中心 |
| `CGL_CFG_WATCH_R_FACE` | 80 | 盘面半径 |
| `CGL_CFG_WATCH_R_RING` / `_RING_W` | 75 / 3 | 秒环中心半径与环宽 |
| `CGL_CFG_WATCH_R_WELL_IN` / `_WELL_OUT` | 71 / 79 | 秒环凹槽截面的内外半径 |
| `CGL_CFG_WATCH_R_TICK_OUT` | 70 | 分度刻线外端半径 |
| `CGL_CFG_WATCH_R_TICK_HOUR_IN` / `_MIN_IN` | 64 / 67 | 时标、分标刻线内端半径 |
| `CGL_CFG_WATCH_R_HOUR` / `_MINUTE` / `_SECOND` | 42 / 62 / 71 | 三根指针的针尖半径 |
| `CGL_CFG_WATCH_R_TAIL` | 14 | 秒针配重尾长 |
| `CGL_CFG_WATCH_R_HUB` | 5 | 轴心半径 |
| `CGL_CFG_WATCH_R_HOUR_BASE` / `_MINUTE_BASE` | 14 / 16 | 指针在轴心后方的回伸长度（指针为渐细刃形，宽端藏在轴心下） |
| `CGL_CFG_WATCH_HOUR_W_BASE` / `_W_TIP` | 13 / 4 | 时针在轴心处与针尖处的全宽 |
| `CGL_CFG_WATCH_MINUTE_W_BASE` / `_W_TIP` | 11 / 3 | 分针同上 |
| `CGL_CFG_WATCH_SECOND_W` | 3 | 秒针宽度 |
| `CGL_CFG_WATCH_SHADOW_PX` / `_SHADOW_ALPHA` | 2 / 77 | 指针投影的偏移（右下）与浓度（Q8，30% 读作"指针浮在盘面上"） |
| `CGL_CFG_WATCH_TICK_MS` | 100 | 秒针"跳动"走法的时间分辨率：指针停在本秒刻度上，最后 100 ms 是把它带到下一格的抽击 |
| `CGL_CFG_WATCH_REVEAL_MS` | 760 | 入场扫描（12:00 → 当前时刻）时长 |
| `CGL_CFG_WATCH_PULSE_MS` | 620 | 分钟跳变时的闪光时长 |
| `CGL_CFG_WATCH_TRAIL_MS` | 2400 | 彗尾余辉时长。需大于一个跳动间隔数倍，否则"彗尾"只有一个亮点 |
| `CGL_CFG_WATCH_SECOND_MODE` | 1 | `cgl_watch_anim_default()` 的初值：1 = 每秒一步的机械跳动，0 = 连续扫秒。注意这是**运行时字段** `cgl_watch_anim_t` 的初始值，应用可在设置界面中两者兼有 |

**时间**

| 宏 | 默认 | 作用 |
|---|---|---|
| `CGL_CFG_CLOCK_STEP_MS` | 16 | 逻辑步长 |
| `CGL_CFG_CLOCK_MAX_STEPS` | 4 | 单帧最多执行多少逻辑步（防卡顿追补的上限） |

**页面与转场**

| 宏 | 默认 | 作用 |
|---|---|---|
| `CGL_CFG_PAGE_STACK_MAX` | 4 | 页面栈深度（定长数组，无堆） |
| `CGL_CFG_TRANSITION_MS` | 220 | 滑动/覆盖/淡入淡出系列的默认时长；0 表示这些类型瞬间切换 |
| `CGL_CFG_GENIE_MS` | 320 | Genie 吸入的默认时长。比滑动略长是刻意的：吸入是几何形变，形状本身需要被看清 |
| `CGL_CFG_TRANSITION_EASE` | `CGL_EASE_OUT_CUBIC` | 上述系列的默认缓动曲线 |
| `CGL_CFG_GENIE_OUT_EASE` | `CGL_EASE_IN_QUAD` | 被吸走时：先蓄力再甩出 |
| `CGL_CFG_GENIE_IN_EASE` | `CGL_EASE_OUT_CUBIC` | 倒出时：稳定刹停 |
| `CGL_CFG_S_OUT_EASE` / `CGL_CFG_S_IN_EASE` | 同 Genie 两向 | S 形吸附的两向曲线。与竖向吸入完全相同：区别在几何路径，不在时间 |
| `CGL_CFG_GENIE_NARROW_Q8` | 64 | 完全吸入时靠近吸入口一端的宽度（Q8，64 = 25%）。越大越像喇叭，越小越像锥子 |
| `CGL_CFG_GENIE_DROP_PX` | 6 | 吸入口位于页面下边缘之下多少像素（置于屏幕外，末帧不留残影） |
| `CGL_CFG_S_AMP_MAX_Q8` | 160 | S 形路径最大横向偏移（Q8）的**上限**，同时是单调性边界。数学上界为 682（超过后邻行落点会交叉、画面折叠）；默认值 160 正是 320 像素面板由"半屏宽"推导出的值，因此上限与推导值在构造上一致 |
| `CGL_CFG_S_DROP_PX` | `CGL_CFG_GENIE_DROP_PX` | S 形吸入口位于右下角之下多少像素 |

**输入与调色板**

| 宏 | 默认 | 作用 |
|---|---|---|
| `CGL_CFG_INPUT_QUEUE` | 8 | 输入环形队列长度，**必须为 2 的幂** |
| `CGL_CFG_BG_COLOR` / `_FG_COLOR` / `_ACCENT_COLOR` | `CGL_COLOR_DARK` / `CGL_COLOR_CREAM` / `CGL_COLOR_AMBER` | 内置控件与转场遮罩的默认颜色 |

### 5.5 重定向配方

**更小的面板或更紧的 RAM 预算**

1. 把 `CGL_CFG_SCREEN_W/H` 设为实际面板尺寸，并让 `drv->width/height` 与之匹配。
2. RAM 不足时先设 `CGL_CFG_ALPHA 0`：这是最省事的大额回收，代价只是失去"透视组"能力。
3. 再把 `CGL_CFG_BAND_H` 降到 2，代价是每帧的 SPI 开窗命令变多。

**更大的面板或更充裕的 RAM**

1. 把 `CGL_CFG_BAND_H` 提到 8 或 16，减少开窗命令开销。
2. 若嵌套半透明层，提高 `CGL_CFG_ALPHA_LEVELS`。
3. 相应提高 `CGL_CFG_ALPHA_BUDGET`，否则构建会按设计失败。

**内存核算**

- 条带缓冲：`2 × CGL_CFG_SCREEN_W × CGL_CFG_BAND_H × 2` 字节。默认值为 5 KB。
- 分组不透明度暂存：`CGL_CFG_ALPHA_LEVELS × CGL_CFG_ALPHA_LEVEL_STRIDE × 2` 字节。默认值为 5 KB。
- 二者都由 `cgl_display_t` 持有，而该实例通常静态存在于移植层。

---

## 6. 画布与软件光栅化

### 6.1 模型

一块画布 = 一个 `width × height` 的水平条带缓冲 + `origin_y`（本条带首行对应的屏幕行号）+ 裁剪窗口（**屏幕坐标**）+ `pan`（本级的平移）+ `warp`（可选的**非线性**变形）+ 分层暂存区（分组不透明度）。

```c
typedef struct {
    cgl_color_t *buf;       /* width x height 像素，行优先            */
    int16_t  width;         /* 每行像素数                            */
    int16_t  height;        /* 本条带行数                            */
    int16_t  origin_y;      /* 条带第 0 行对应的屏幕 y               */
    cgl_rect_t clip;        /* 裁剪窗口，屏幕坐标，闭区间             */
    cgl_point_t pan;        /* 本级平移；图元坐标先加它再裁剪/落像素   */
    const cgl_warp_t *warp; /* 非空 = 本子树经过吸入变形绘制          */
    cgl_rect_t src_clip;    /* **源坐标**下的裁剪窗口；仅在 warp 生效时参与测试 */

    /* 分组不透明度 */
    cgl_color_t *alpha_scratch;  /* 与条带缓冲几何完全相同的分层暂存区 */
    uint32_t alpha_stride;       /* 每层像素数（>= width * height）     */
    uint8_t  alpha_levels;       /* 暂存区层数                          */
    uint8_t  alpha_used;         /* 当前已嵌套层数                      */

    uint16_t ink_alpha_q8;       /* 墨色不透明度：混合类图元的不透明度   */
} cgl_canvas_t;
```

坐标流水线：

```
图元坐标 --(+pan)--> 屏幕坐标 --(warp)--> 目标坐标 --(clip / 条带内行号)--> 像素
                                   `-- src_clip 在 warp **之前**按源坐标剔除
```

**只有一条规则**：每个公共图元函数对自己的坐标平移一次；内部函数只接受屏幕坐标。`cgl_canvas_clear_clip()` 以裁剪窗口自身作为矩形，因此刻意走无平移的内部路径。

### 6.2 初始化与状态

| API | 语义 |
|---|---|
| `cgl_canvas_init(c, buf, width, height, origin_y)` | 绑定条带缓冲。`origin_y` 为条带首行的屏幕行号 |
| `cgl_canvas_get_clip(c)` / `set_clip(c, clip)` / `reset_clip(c)` | 读写裁剪窗口（屏幕坐标） |
| `cgl_canvas_get_pan(c)` / `set_pan(c, pan)` | 读写平移。调用者负责成对恢复；控件树遍历采用保存/恢复正常而非增量修改 |
| `cgl_canvas_set_ink_alpha(c, alpha_q8)` / `get_ink_alpha(c)` | 设置/读取混合类图元的墨色不透明度（见 6.6） |
| `cgl_canvas_set_warp(c, w)` / `get_warp(c)` | 安装/移除变形描述符（`NULL` 表示关闭） |
| `cgl_canvas_map_y(c, y)` / `map_x(c, x, y)` | 源坐标 → 目标坐标；无 warp 时是恒等映射。传入的必须是**已加 pan 的屏幕坐标**，且 `map_x` 需要该像素所在的源行号 |
| `cgl_canvas_map_rect(c, r)` | 源矩形在目标空间中的**保守**包围盒（无 warp 时就是自身）。控件树用它做剔除 |
| `cgl_canvas_set_src_clip(c, clip)` / `get_src_clip(c)` | 源坐标裁剪窗口，仅在 warp 生效时参与测试 |
| `cgl_canvas_src_window(c)` | 仍能落到本条带的**源空间**窗口（闭区间，屏幕坐标）。自行逐行扫描形状的绘制器用它界定扫描范围 |
| `cgl_canvas_row(c, y)` | 直接取行指针。`y` 是**屏幕行号**，此函数**不加 pan**；调用者必须自行加 pan 并遵守裁剪 |
| `cgl_canvas_set_alpha_scratch(c, scratch, levels, stride_px)` / `alpha_room(c)` | 安装/移除分层暂存区，查询还可嵌套几层 |
| `cgl_canvas_copy_rect(c, dst, r)` | 把当前缓冲在 `r` 内的像素拷贝到 `dst`；`dst` 必须与本缓冲**几何相同** |
| `cgl_canvas_blend_src_q8(c, r, src, alpha_q8)` | 逐像素源混合：`dst = dst*(1-a) + src*a`，`src` 与本缓冲几何相同 |

### 6.3 图元

| 图元 | 说明 |
|---|---|
| `cgl_canvas_clear` / `clear_clip` | 清除整条带 / 仅清除裁剪窗口 |
| `cgl_canvas_pixel` | 单像素 |
| `cgl_canvas_hline` / `vline` | 水平线 / 垂直线 |
| `cgl_canvas_line` | Bresenham 直线，1 像素宽 |
| `cgl_canvas_fill_rect` / `rect` | 实心矩形 / 矩形轮廓 |
| `cgl_canvas_disc` | 实心圆盘（中点圆算法） |
| `cgl_canvas_dot` | 尺寸 1..3 的实心方点，以 `(cx, cy)` 为中心。比 `disc` 便宜，且保持相邻点之间的负空间，这正是点阵徽标可读的原因 |
| `cgl_canvas_circle` | 1 像素宽的**抗锯齿**圆轮廓，等价于"半径 `r`、宽 1 的圆弧"（环带 `r ± 0.5`）。半径 `<= 0` 画出一个抗锯齿像素：退化为点是合法结果，不是错误 |
| `cgl_canvas_line_thick` | 带**圆头端帽**的粗线段，抗锯齿。`thickness <= 1` 走普通 Bresenham 路径，与 `cgl_canvas_line()` 逐像素完全相同 |
| `cgl_canvas_arc` | 带圆头端帽的抗锯齿粗圆弧：圆环/仪表/径向进度的基础图元。`radius <= 0` 退化为直径 `thickness` 的抗锯齿圆点 |
| `cgl_canvas_pixel_blend_q8` | 单像素按覆盖率 `cov_q8/256` 混合。抗锯齿笔画的基本构件，也供自定义光栅器使用 |
| `cgl_canvas_blit` | 无缩放位块传送（源坐标需自行扣回 pan） |
| `cgl_canvas_fill_rect_blend_q8` | 用**单一常量颜色**对矩形做 Q8 alpha 混合填充 |
| `cgl_canvas_fill_rect_blend` | 固定 50/50 混合填充（与裁剪窗口求交后填充） |

圆弧的完整圆环写法：

```c
cgl_canvas_arc(c, cx, cy, radius, thickness, 0u, CGL_ANGLE_TURN, color);
```

`sweep` 的类型是 `uint32_t` 而非 `cgl_angle_t`，原因是 `cgl_angle_t` 在一圈处回绕，"整圈"（`CGL_ANGLE_TURN`）会与"零"无法区分。大于一圈的 `sweep` 合法，会自然回绕；`sweep == 0` 什么也不画，这正是每个调用者都需要的"空仪表"状态。

### 6.4 抗锯齿径向图元：四条需要事先知道的性质

圆、粗线、圆弧是抗锯齿的；矩形、水平线、圆盘、方点不是。矩形和水平线没有需要消除的锯齿；而圆、指针、圆环除了锯齿别无他物——切线接近 45° 处会有两三像素深的台阶，在 250 dpi 上读作毛刺而不是曲线。因此上述笔画携带逐像素**覆盖率**（0..256）并混合，而 `fill_rect` / `line` / `disc` / `dot` 保持锐利：它们是点阵/条形/面板的热路径，那里一个像素就是一个像素。

覆盖率的计算在**平方距离**域完成（两次乘法），用线性化 `(r² − d²)/(r + d) == r − d` 并在边界附近细化一次，光栅器中没有任何 `sqrt`。

1. **笔画的边界像素是混合像素**，"这个像素是不是笔画颜色"只能问内部像素。一个 1 像素宽的抗锯齿笔画**不存在**任何完全覆盖的像素（它触及的每个像素都落在渐变上），因此需要看起来是实心的边框或细线应以 2 像素索取。
2. **绘制宽度恰好等于 `thickness`**，奇偶均可。抗锯齿消除了早期实现中"只能为奇数"的限制。
3. **端帽是圆的**，因此笔画在两端各比线段/圆弧请求的长度最多多出 `thickness / 2` 像素。需要严格装进某个盒子的调用者应当自行缩短几何。
4. **开销与笔画面积成正比**。`line_thick` 沿**主轴**行走并逐步推导次轴范围（因此斜置细笔画不必为包围盒的空角付费）；`arc` 逐屏幕行行走，每行半宽由 `cgl_math_isqrt()` 得到。圆弧**直接**光栅化形状，不沿角度采样——这也是"增大 sweep 不会让已画部分移动"的原因（按角度步进采样的网格在步长变化时会重新放置每一个戳记，用那种方式构建的仪表在扫动时会抖动）。

### 6.5 变形（warp）

`pan` 是**线性**的：每个图元被同一位移平移。`warp` 是它的非线性版本：源坐标（已加 pan 的"静止屏幕坐标"）按下列规则映射到目标坐标。

```
行：y' = ty + (y - ty) * (1 - p)                  单调，步长 0 或 1
列：x' = cx' + (x - cx) * k(y) / 256 + S(y)        k(y) 落在 [0, 256]
```

```c
typedef struct {
    int16_t  src_y0;        /* 源行区间起点（页面静止时所处的屏幕行） */
    int16_t  src_h;         /* 源行数（= 页面高度）                  */
    int16_t  cx;            /* 源中心列：收窄的轴                    */
    int16_t  tx, ty;        /* 吸入口坐标                            */
    uint16_t progress_q8;   /* 0 = 恒等；256 = 完全吸入一点           */
    uint16_t narrow_q8;     /* 远端（靠吸入口一端）的宽度系数，Q8      */
    uint16_t row_step_q8;   /* 归一化步长 = 256*256/src_h            */
    uint16_t s_amp_q8;      /* 横向 S 路径最大偏移，Q8；0 = 无 S 路径  */
} cgl_warp_t;
```

关键性质：

- `p = 0` 时整个映射是**精确恒等**（行向下取整、列四舍五入，二者都可推导），因此把 p 从 0 拉到 256 就是"从原样到完全吸入一点"。这也是页面切换首帧看不出跳变的原因。
- 行映射的步长恒为 0/1，因此**离散写入不会留下空洞**（目标行可由源行直接算出，无需反查），并且保证变形前后的前后遮挡顺序不变。
- 行向下取整、列四舍五入并非任意选择：屏幕只有 172 行而吸入口位于屏幕边缘，那里 `floor(d*(1-eps)) == d`，所以小 p 时行映射是精确恒等；若列也向下取整，就会在画面中央裂开一道缝（右半部分整体左移一个像素）。
- `src_clip` 是**源坐标**下的裁剪窗口，仅在 warp 生效时参与测试。变形是"画完之后再形变"，因此裁剪天然属于源空间。
- **剔除不能用源 frame**：变形是压缩的，当页面被吸到屏幕另一端时源框与目标框可能完全不重叠，用源框剔除会只画整页的顶部几行。应使用 `cgl_canvas_map_rect()` 给出的保守包围盒。
- **`CGL_WF_CLIP` 必须留在源空间**：在变形的子树内它改的是 `src_clip` 而不是目标 `clip`。
- **逐行扫描必须绑定到条带的原像而不是条带本身**：一个图元在每个条带各绘制一次，其行扫描必须裁到真正能落到该条带的行。`cgl_canvas_arc()` / `line_thick()` / `disc()` 都这么做，自带光栅器的控件应当通过 `cgl_canvas_src_window()` 同样处理。在有 warp 时该窗口**远高于**条带，这正是该函数存在的意义：没有它，每个形状都要在全部 43 个条带里完整走一遍，再叠加逐像素的 warp 映射，帧率会崩坏。
- `hit_test` 不做变形的反查：变形只存在于**转场期间**，而转场期间输入被锁定（见 13.8），因此命中测试发生时 `warp` 恒为 `NULL`。

`cgl_warp_t` 由调用者持有，控件只保存指针。转场期间它由导航器持有。

### 6.6 墨色不透明度（ink alpha）

```c
void     cgl_canvas_set_ink_alpha(cgl_canvas_t *c, uint16_t alpha_q8);  /* 0..256 */
uint16_t cgl_canvas_get_ink_alpha(const cgl_canvas_t *c);
#define  CGL_CANVAS_INK_OPAQUE 256u
```

它使**单个图元**半透明，不需要任何额外内存，是"这个圆环按 40% 画"的实现方式。它通过缩放图元本已算出的覆盖率生效，因此边缘仍然抗锯齿，结果仍是框架唯一的混合公式，而不是第二个公式。

哪些图元遵守它：所有经过 `canvas__blend()` / `canvas__store()` 的路径，即 `cgl_canvas_arc()`、`circle()`、`disc()`、`line_thick()`（厚度 > 1）以及 `pixel_blend_q8()`。**锐利**的快速路径（`fill_rect`、`line`、`hline`、`vline`、`dot`、`clear`，以及 1 像素宽的 `line_thick`）不读它：它们是字节写入，而"画一个半透明实心矩形"已经有自己的门——`cgl_canvas_fill_rect_blend_q8()`。

**它是状态而不是参数，且不会自动保存恢复。** 设置、绘制、再设回 `CGL_CANVAS_INK_OPAQUE`；`cgl_arc.h` 中的 `cgl_arc_draw_*()` 包装函数就是在一调用外围做这件事。超过 256 的值被钳位。

### 6.7 画布侧的分组不透明度接口

画布只提供机制，策略在控件层（见 9.3）。四个调用：`set_alpha_scratch()` 安装分层缓冲，`alpha_room()` 查询剩余层数，`copy_rect()` 拷贝背景，`blend_src_q8()` 按 alpha 合成回去。逐条带的合成流程：

```
1) 把本条带中被该控件覆盖的区域拷贝进暂存区（= 背景副本）
2) 把整棵子树**按常规**渲染进该暂存区
   —— 实现只是临时把 c->buf 指向暂存区：条带几何完全一致，因此 pan / warp /
      clip / src_clip 全部原样有效，**连 cgl_canvas_row() 的裸指针使用者
      （字体、自定义控件）也自动跟随**。图元代码零改动。
3) 按 alpha 把暂存区混回条带缓冲：band = band*(1-a) + scratch*a
4) 还原 c->buf
```

两条值得记住的性质：

- **不需要覆盖掩码。** 第 3 步对"没有被绘制的像素"是恒等变换（那里暂存区仍等于背景，`band*(1-a) + band*a == band`）。这就是它比"先保存背景、再通过掩码混回"更省 RAM 的原因。
- **嵌套是精确的。** 把分组渲染到"背景的副本"上再按 alpha 混回，与"把分组渲染到透明层再合成"在数学上等价（可由归纳证明），因此两层嵌套就是逐级施加各自 alpha，没有漂移。

`cgl_display_t` 中的 `alpha[]` 与 `band[]` 一起，构成分组不透明度的全部内存；`CGL_CFG_ALPHA = 0` 时该成员根本不存在。

### 6.8 陷阱

- `cgl_canvas_row()` **不加 pan**。它返回裸缓冲指针，因此绕过图元函数使用它的快速路径（如字体代码）必须自行加 pan，否则会出现"矩形跟着子树移动、文字留在原地"。
- `clip` 永远是**屏幕坐标**（不含 pan），pan 只作用于传给图元的坐标。因此"把 clip 再喂给 fill_rect"会平移两次。
- 图元的坐标是"本级的坐标"；`pan` 由遍历器按层级累加，控件自身不需要知道祖先的平移。

---

## 7. 整数数学

该模块是**基础模块**：只依赖 `<stdint.h>`，因此画布可以向下包含它，而画布本身仍不得向上依赖 `cgl_anim` / `cgl_widget`。它存在的理由是径向绘图需要"角度 → 点"的映射，而框架拒绝 libm：`sinf()` / `cosf()` 会把软浮点/64 位辅助函数一起拖进来，而"无三角画圆"的技巧（`x*x + y*y <= r*r`）只能画整圆，无法回答"表盘上 10:09 在哪里"。

实现是一个四分之一波表加线性插值的模块：65 个 `int16` 表项（130 字节 Flash、无 RAM、无 libm）。

### 7.1 角度约定

```c
typedef uint16_t cgl_angle_t;     /* 1/65536 圈 */
#define CGL_ANGLE_TURN     65536u            /* 注意：unsigned int，不是 cgl_angle_t */
#define CGL_ANGLE_QUARTER  ((cgl_angle_t)16384u)   /* 6 点钟 */
#define CGL_ANGLE_HALF     ((cgl_angle_t)32768u)   /* 9 点钟 */
#define CGL_ANGLE_UP       ((cgl_angle_t)49152u)   /* 12 点钟 */
#define CGL_ANGLE_DEG(d)   ((cgl_angle_t)(((uint32_t)(d) * 65536u) / 360u))
#define CGL_ANGLE_FROM_U8(a) ((cgl_angle_t)((uint16_t)(a) << 8))
#define CGL_ANGLE_TO_U8(a)   ((uint8_t)((uint16_t)(a) >> 8))
```

约定为屏幕空间、y 轴向下：

```
角度 0        -> +x（3 点钟）
角度增大      -> 朝 +y，即屏幕上**顺时针**
CGL_ANGLE_UP  -> -y（12 点钟）        x = cx + r*cos(a), y = cy + r*sin(a)
```

- **16 位而不是 8 位。** 可选模块使用 8 位角度（相机矩阵足够），但任何用户会盯着看其移动的量都不够：8 位每步 1.41°，在 70 像素半径上指针端点移动 1.7 像素，秒针会每秒可见地跳 3 次而不是扫动。16 位使步长为 0.007 像素，无需对位置做插值。两个世界的边界上用 `CGL_ANGLE_FROM_U8()` / `CGL_ANGLE_TO_U8()` 转换。
- **Q8 而不是 Q15。** 与框架其余部分一致（补间曲线、alpha、变形的 `progress_q8`），且误差已在像素栅格之下：满量程的 0.5/256 在 100 像素半径上是 0.2 像素。Q15 只会让每个使用方写 `(int32_t)r * v >> 15`。
- **`cgl_angle_t` 在一圈处回绕。** 这正是它"转角分数"而非长度的含义，其后果是"整圈"无法写成 `cgl_angle_t`（会等于 0），因此圆弧图元的 `sweep` 使用更宽的 `uint32_t`。`CGL_ANGLE_TURN` 刻意是 `unsigned int`，应在更宽的算术中使用再向下转型。

### 7.2 API

| API | 语义 |
|---|---|
| `int16_t cgl_sin_q8(cgl_angle_t a)` | 正弦，范围 −256..256，256 精确表示 1.0 |
| `int16_t cgl_cos_q8(cgl_angle_t a)` | 余弦（同一张表，超前四分之一圈） |
| `void cgl_math_polar(cgl_angle_t a, int16_t radius, int16_t *dx, int16_t *dy)` | 角度 + 半径 → 相对中心的整像素偏移：`dx = round(r*cos/256)`，`dy = round(r*sin/256)`。`radius` 可取 0..32767，任一输出可为 `NULL` |
| `void cgl_math_polar_q6(int16_t cx, int16_t cy, cgl_angle_t a, int16_t radius, int32_t *x_q6, int32_t *y_q6)` | 同一映射的 **Q6**（1/64 像素）形式，围绕任意中心。`x_q6 = cx<<6 + (r*cos)>>2` |
| `uint16_t cgl_math_frac_q8(int32_t value, int32_t vmin, int32_t vmax)` | `(value − vmin)/(vmax − vmin)` 的 Q8 表示（0..256，四舍五入）。`vmax <= vmin` 视为被拒绝的范围，画出空值而不做除零 |
| `uint32_t cgl_math_isqrt(uint32_t v)` | `floor(sqrt(v))`，无 libm、无浮点 |
| `uint16_t cgl_angle_deg(cgl_angle_t a)` | 角度的整度数读数，0..359。这是**数学**读数（0 = +x = 3 点钟），供显示与测试使用，不供绘制使用 |

三个实现层面的要点：

- **`cgl_math_polar()` 四舍五入到最近，这一点是承重的**：截断的 `>> 8` 会把两个轴各自独立地偏向原点，即本应落在半径 r 上的点最远会落到 r 以内 1.4 像素。偏差不会互相抵消——由这些点画出的圆环会明显小于标称半径。
- **`cgl_math_isqrt()` 的用法**：Q16 输入得到 Q8 输出，因为 `sqrt(x*65536) == sqrt(x)*256`。于是 `cgl_math_isqrt(r_q8*r_q8 - dy_q8*dy_q8)` 直接给出圆的一行在 Q8 下的半宽，调用点无需任何缩放算术——`cgl_canvas_arc()` 正是这样逐行走的。算法为逐位（恢复余数）法，最多 16 次移位/比较/减法，开销有确定上界，而不是迭代次数依赖初值的牛顿迭代。
- **`cgl_math_frac_q8()` 是"值到进度"的唯一实现**，由 `cgl_bar`（线性填充）与 `cgl_arc_gauge`（角向扫掠）共用，因此两者不可能对"一半在哪里"给出不同答案。内部全用无符号 32 位：`vmax − vmin` 在 `INT32_MIN..INT32_MAX` 上不是合法的 `int32_t`，但是完全合法的 `uint32_t`。小数位由移位/减法长除法得到，而不是 `(off << 8) / span`——后者在范围宽于 2²³ 时溢出，而 `int64` 在本项目中是被禁止的（会从 libgcc 引入 `__aeabi_ldivmod`）。

---

## 8. 字体与文本

### 8.1 字形存储格式

点阵布局为**行优先**，一个字节表示同一行的 8 个水平像素，**bit0 是最左像素**：

```
字节索引 = row * bytes_per_row + (column / 8)
位  索引 = column % 8
```

这是经典厂商 ASCII 表的布局，因此任何该格式的字形表填入一个 `cgl_font_t` 即可使用。

```c
typedef struct {
    uint8_t width;        /* 字形单元宽度（像素）              */
    uint8_t height;       /* 字形单元高度（像素）              */
    uint8_t bytes;        /* 每个字形的字节数 = bytes_per_row * height */
    const uint8_t *data;  /* 字形表，按 ASCII 顺序从 first 开始 */
    uint8_t first;        /* 首个编码字符                      */
    uint8_t last;         /* 末个编码字符                      */
} cgl_font_t;
```

### 8.2 API

| API | 语义 |
|---|---|
| `extern const cgl_font_t cgl_font_8x16` | 8×16 ASCII 字体，可打印范围 `0x20..0x7E`（默认） |
| `extern const cgl_font_t cgl_font_12x24` | 12×24 ASCII 字体，在 250 dpi 上可读 |
| `int16_t cgl_font_text_width(const cgl_font_t *font, const char *text)` | 字符串像素宽度，`NULL`/空串返回 0。等宽字体下即"字符数 × 字形宽度" |
| `int16_t cgl_font_draw_text(c, font, x, y, text, fg)` | 透明背景绘制，返回前进宽度 |
| `int16_t cgl_font_draw_text_bg(c, font, x, y, text, fg, bg)` | 用背景色填充字形单元（不透明），返回前进宽度 |

文本绘制逐行直接写 `cgl_canvas_row()`，因此它**自己加 pan**。省略这一步的症状是"在平移的子树里矩形跟着动、文字不动"。两套字体都是等宽点阵，非 ASCII 字符（`< 0x20` 或 `> 0x7E`）不在表内。

生成新字号或新字体的工具见 18.6 节。

---

## 9. 控件树

### 9.1 结构

```c
struct cgl_widget
{
    const cgl_widget_vt_t *vt;   /* type / draw / update / event */
    const char *name;            /* 调试用 */
    cgl_rect_t frame;            /* 绝对屏幕矩形 */
    cgl_point_t offset;          /* 本子树的平移（转场/滚动/抖动） */
    const cgl_warp_t *warp;      /* 本子树的非线性变形（Genie 吸入） */
    cgl_widget_t *parent, *first_child, *last_child, *next;
    uint16_t flags;              /* CGL_WF_VISIBLE | CGL_WF_CLIP */
    uint8_t alpha_q8;            /* 组不透明度；255 = 不透明（默认） */
    void *user;
};
```

`alpha_q8` 被刻意放在 `flags` 之后：该位置本来就是对齐填充，因此这个字段**不占用任何额外字节**（结构体总大小仍为 48 B）。

| 标志 | 含义 |
|---|---|
| `CGL_WF_VISIBLE` | 参与绘制、更新与命中测试（清除后整棵子树被跳过） |
| `CGL_WF_CLIP` | 子控件被裁剪到本控件的 frame（`cgl_widget_init()` 默认设置）。绘制与命中测试**都**遵守它：后代画在祖先裁剪窗口之外的部分也不会在那里被命中。未设置该标志的控件可以画到 frame 之外，其子控件在那里仍然可达 |

位 `0x0004` 曾用于 `CGL_WF_OPAQUE`（"本控件铺满自己的 frame"）。它由 `cgl_widget_init()` 设置，但**没有任何代码读取**，因此已被移除：仍然引用该名字的代码会直接编译失败，而不是得到静默错误的结果。不透明度的开关是 `alpha_q8` / `cgl_widget_set_alpha()`，其语义是**组**不透明度。

```c
typedef struct
{
    const char *type;
    void (*draw)(cgl_widget_t *w, cgl_canvas_t *c);
    void (*update)(cgl_widget_t *w, uint32_t dt_ms);
    bool (*event)(cgl_widget_t *w, const cgl_event_t *ev);   /* 可选，NULL = 不处理 */
} cgl_widget_vt_t;
```

`event` 返回 `true` 表示"我已消费"，遍历停止；返回 `false` 表示继续传递。虚表必须用指定初始化器书写。

### 9.2 构建与遍历

| API | 语义 |
|---|---|
| `cgl_widget_init(w, vt, name, frame)` | 初始化：默认置 `CGL_WF_VISIBLE \| CGL_WF_CLIP`，`alpha_q8 = CGL_ALPHA_OPAQUE` |
| `bool cgl_widget_add_child(parent, child)` | 把 `child` 追加到子链表末尾，即在较早的同级之上绘制。**返回值必须检查**：参数为 `NULL`、子控件已有父控件（需先 `remove_child()`，静默改父会破坏旧链表）、或父控件已有 `CGL_CFG_WIDGET_MAX_CHILDREN` 个子控件时返回 `false`。每次拒绝计入 `cgl_widget_children_dropped()` |
| `cgl_widget_remove_child(child)` | 仅解除链接，不销毁 |
| `cgl_widget_set_frame(w, frame)` | 设置绝对屏幕矩形并标记损坏 |
| `cgl_widget_set_visible(w, visible)` | 可见性（清除后整棵子树不绘制、不更新、不命中） |
| `cgl_widget_set_offset(w, dx, dy)` | 本子树的平移；同时标记"移动前"与"移动后"两个区域损坏 |
| `cgl_widget_set_warp(w, warp)` | 本子树的变形；传 `NULL` 恢复。描述符由调用者持有，控件只存指针 |
| `cgl_widget_raise(w)` | 移到同级末尾（绘制在最上层） |
| `cgl_widget_draw_tree(w, c)` | 先序遍历：父先于子，同级按插入顺序 |
| `cgl_widget_update_tree(w, dt_ms)` | 推进本子树的动画；只到达可见子树 |
| `cgl_widget_hit_test(w, x, y)` | 命中的最深层可见控件；同级之间**最后一个**命中者胜出 |
| `cgl_widget_dispatch_event(w, ev)` | 深度优先（父先于子）；第一个返回 `true` 的控件消费该事件 |

**绘制顺序即子链表顺序**：后加入的绘制在上层，`cgl_widget_raise()` 把某个控件移到最上层。

`cgl_widget_hit_test()` 有两点刻意不回答的情况：

- **裁剪被遵守。** 带 `CGL_WF_CLIP` 的控件把子控件约束在自己的 frame 内，因此画在祖先裁剪窗口之外的后代也不会在那里被命中。未设置该标志的控件可以画到 frame 之外，其子控件在那里仍可达——这是两者的既定区别，而非疏漏。
- **经过变形的子树完全不被命中。** 变形把像素搬到了别处，而命中测试没有它的逆映射；从未变形的 frame 报告命中等于命中屏幕上并不存在的像素。转场期间输入本来就被锁定（见 13.8），因此当前这是安全网而非可依赖的行为。

### 9.3 三种子树级机制

**平移（`offset`）**：遍历器进入控件时把 `offset` 加到画布 `pan` 上，离开时恢复。因此"把整页滑出去"只需给页面根控件设置一个 offset，不必改动任何子控件的 frame，也不必重建树，剔除与命中测试自动正确。注意 `offset` 作用于"本控件及其整棵子树"，而不只是子控件；页面根控件通常是整屏，因此它滑动时自己的背景也随之移动，这正是期望行为。

**变形（`warp`）**：同样作用于整棵子树。进入本级时装入画布，离开时恢复。两处必须同时改动，否则会出错（详见 6.5）。

**组不透明度（`alpha_q8`）**：

```c
cgl_widget_set_alpha(&panel.base, 160u);   /* 0..255，255 = 不透明（默认） */
```

语义是**组**不透明度：整个控件（自身的 `draw` 加整棵子树）先被隔离渲染，然后作为一个整体混合到背景上。因此"一个面板加它内部两个半透明高亮相互重叠"不会在重叠处被混合两次——这正是它与"把每个图元分别乘以 alpha"的区别。

隔离区域是"本级**可能**落下像素的地方"：存在裁剪约束（默认开启的 `CGL_WF_CLIP`，或活动的变形）时，它是 frame 与裁剪窗口的保守包围盒（紧致）；**清除了 `CGL_WF_CLIP`** 的控件允许画到 frame 之外，此时只能保守地取整个裁剪窗口——结果正确但更贵。若想节省，请保留 `CGL_WF_CLIP`。

| 情形 | 行为 |
|---|---|
| `alpha = CGL_ALPHA_OPAQUE`（默认） | 整段被跳过，代价是一次分支判断（不使用该 API 的界面不付出任何时间成本） |
| `alpha = 0` | 整棵子树不画一个像素，也不触碰暂存区 |
| 画布没有暂存区（`CGL_CFG_ALPHA = 0`） | alpha 被**忽略**（按不透明绘制），并计入 `cgl_widget_alpha_dropped()` |
| 嵌套超过 `CGL_CFG_ALPHA_LEVELS` | 同上：忽略并计数，不崩溃，不破坏暂存区 |

两条警告：

- **它不是继承的。** 在父控件上设置 alpha，再在子控件上设置 alpha，得到的是**两个**相互独立的分组（各自占用一个暂存层），而不是两者相乘。
- **`alpha = 0` 只意味着"不绘制"**，不影响 `CGL_WF_VISIBLE`、命中测试或事件分发：完全透明的控件仍然会被命中。要彻底移出，请使用 `cgl_widget_set_visible(w, false)`。

`cgl_widget_set_alpha()` 只标记 `w->frame` 的损坏。若该控件刻意画到 frame 之外（清除了 `CGL_WF_CLIP` 的覆盖层），它必须在改动 alpha 后自行再标记一次损坏。

### 9.4 损坏（damage）机制

框架只有**一个**全局损坏出口：`cgl_widget_set_damage_sink(fn, user)` 安装它（`cgl_screen_init()` 安装 `cgl_display_damage`）。控件不需要知道自己在屏幕上、在离屏缓冲里，还是根本不存在。

```c
void cgl_widget_invalidate(const cgl_widget_t *w);        /* 上报 w->frame */
void cgl_widget_invalidate_rect(cgl_rect_t r);            /* 上报任意矩形 */
```

**上报的是未经平移的 frame**（控件不知道祖先的 offset）。因此在自定义的 offset 动画中，调用者必须自行标记新旧位置的并集损坏；`cgl_widget_set_offset()` 已经替使用者做了这件事。转场期间导航器每帧把整屏标脏，所以框架自身没有问题。

损坏的累积方式是**矩形并集**，而不是区域列表。`cgl_display_present()` 会把损坏矩形覆盖到的每个完整条带重绘一遍。

### 9.5 三个可观测计数器

与"效果没有出现"这种只能靠肉眼猜测的失败相比，框架把三种"设置未生效"变成了数字：

| 计数器 | 含义 |
|---|---|
| `cgl_widget_alpha_dropped()` | "设置了 alpha 但未能生效"的累计次数（无暂存区 / 嵌套层用尽）。**单位是"每条带 × 每次遍历"**：控件树每条带重绘一次，因此横跨 10 条带的控件一次增加 10 而不是 1。它回答的是"alpha 是否被丢弃"，不是"有多少个控件被丢弃" |
| `cgl_widget_children_dropped()` | `cgl_widget_add_child()` 因父控件子数已达 `CGL_CFG_WIDGET_MAX_CHILDREN` 而被拒绝的次数 |
| `cgl_bar_range_rejected()` / `cgl_arc_range_rejected()` | 因 `vmax <= vmin` 被拒绝的范围设置次数 |

正常配置下三者都应当是 0。

### 9.6 编写自己的控件

新增一个控件需要满足以下规则：

1. 结构体的**第一个成员必须是** `cgl_widget_t base`。
2. 写一张 `cgl_widget_vt_t` 虚表，使用指定初始化器（`.type = "mywidget", .draw = ..., ...`）。
3. 在 `draw` 中所有坐标都是屏幕坐标，画布会自动裁剪到当前条带；**必须自行填满自己的 frame**，否则会残留上一帧的内容。只绘制"变化的部分"的控件因此没有可供恢复的状态——`cgl_bar` 用轨道色填满整个 frame 正是出于这个原因。
4. 在 `init` 中调用 `cgl_widget_init(&x->base, &vt, "name", frame)`；需要裁剪时加 `x->base.flags |= CGL_WF_CLIP`。
5. 在 setter 中**仅当绘制像素真的会改变时**才调用 `cgl_widget_invalidate(&x->base)`：先比较新旧值，相同则提前返回。配置类 setter（`set_colors`、`set_align` 等）同样要比较：每帧重新应用主题的界面不应每帧重绘。
6. 把控件挂到树上：`cgl_widget_add_child(root, &x->base)`，并检查返回值。可变数量的子控件构建应当关注它，`cgl_widget_children_dropped()` 会统计每一次拒绝。
7. 只在存在连续动画时才实现 `update(w, dt_ms)`。**不要把物理/逻辑放进这里**（那属于页面的 `on_step`）。若某个参数可能被设成不可能的值，应拒绝它**并计数**，使"效果没有出现"成为一个数字而不是猜测。

---

## 10. 内置控件

### 10.1 面板 `cgl_panel`

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

最简的容器：用 `bg` 填满 frame，再按 `border_w` 画边框。当 `bg == border` 时边框自动不绘制。面板是"必须自行填满 frame"这一规则的最直接体现。

### 10.2 文本标签 `cgl_label`

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

| API | 语义 |
|---|---|
| `cgl_label_init(l, name, frame, font, text, fg, bg, opaque)` | `opaque` 为真时用 `bg` 填充字形单元 |
| `cgl_label_set_text(l, text)` | 替换文本。**仅当内容真的变化时**才重绘 |
| `cgl_label_set_int(l, prefix, value, suffix)` | 替换为"前缀 + 十进制整数 + 后缀"，不需要 `printf` |
| `cgl_label_set_fixed1(l, prefix, value_x10, suffix)` | 替换为"前缀 + 值 + '.' + 一位小数 + 后缀"，`value_x10` 是放大十倍的整数 |
| `cgl_label_set_align(l, align)` | 左/居中/右对齐 |
| `cgl_label_set_colors(l, fg, bg, opaque)` | 改颜色与不透明背景 |

标签持有定长缓冲（`CGL_CFG_LABEL_MAX_TEXT`，含结尾 `\0`），因此框架中任何地方都不需要堆或 `printf`。所有读数——帧率、运行时间、计数器——都通过 `set_int` / `set_fixed1` 写出，这是工程内自制的十进制转换，以避免 libc/printf 的 Flash 开销。文本在 frame 内垂直居中。

### 10.3 进度/电平条 `cgl_bar`

```c
typedef enum { CGL_BAR_HORIZONTAL = 0, CGL_BAR_VERTICAL = 1 } cgl_bar_orient_t;

typedef struct {
    cgl_widget_t base;
    cgl_color_t track, fill, border, seg;
    cgl_anim_t  anim;        /* 绘制值的补间 */
    int32_t value;           /* 逻辑（请求）值，始终落在 [vmin, vmax] 内 */
    int32_t vmin, vmax;
    uint8_t orient, border_w, segments, reverse;
} cgl_bar_t;
```

几何关系：

```
frame            用 track 填满
 +-- inner       frame 内缩 border_w；填充位于此处
 |    +-- fill   fill，沿取向轴生长
 +-- ring        border，最后绘制，因此永不被覆盖
```

`border_w` 同时充当条形的内边距：没有第二个"内缩"参数，因为两者总是会被设成同一个数。

| API | 语义 |
|---|---|
| `cgl_bar_init(b, name, frame, track, fill)` | 0..100 范围，值 0，水平，无边框，无分段。`track` 同时是控件背景 |
| `bool cgl_bar_set_range(b, vmin, vmax)` | 任意 `int32_t` 范围。`vmax <= vmin` 被**拒绝**（返回 false、计入 `cgl_bar_range_rejected()`、保留旧范围）。范围变化总是标记损坏，因为同一个值会落在别处 |
| `cgl_bar_set_value(b, value)` | 立即设置并绘制（运行中的补间被直接吸附到新值），钳位到范围内。仅当绘制像素真的会改变时才重绘 |
| `cgl_bar_set_percent(b, percent)` | 0..100 映射到范围上；在默认 0..100 范围下等价于 `set_value` |
| `bool cgl_bar_animate_to(b, value, dur_ms, ease)` | 用 `b->anim` 在 `dur_ms` 内移动到 `value`。补间从**当前绘制值**出发，因此中途改目标依然平滑。`dur_ms == 0` 退化为 `set_value()` 并返回 false。控件在自己的 `update` 中自行推进，调用者只需保持 `cgl_widget_update_tree()` 被调用 |
| `cgl_bar_set_orient(b, orient)` / `cgl_bar_set_reverse(b, reverse)` | 垂直方向默认自底边向上填充；`reverse` 翻转为右到左 / 上到下。仅在真的改变时重绘 |
| `cgl_bar_set_colors(b, track, fill)` | 替换轨道与填充色 |
| `cgl_bar_set_border(b, color, width)` | 画 `width` 像素的边框环；`width == 0` 取消边框。`width` 会被钳位以在环的两侧之间至少留一条像素，因此薄于 3 像素的条形没有边框 |
| `cgl_bar_set_segments(b, segments, color)` | 把条形分成 `segments` 等份，在它们之间画 `segments - 1` 条分隔线。分隔线横跨**整个**内部矩形，覆盖填充与轨道，因此画面读作"N 个等份，其中前 k 个已填充"（电池/信号强度外观）。把**轨道色**作为分隔色则切换成另一种读法："填充被切成方块" |
| `cgl_bar_get_value` / `_get_shown_value` / `_get_percent` / `_is_animating` | 逻辑值 / 当前绘制值（仅补间中不同）/ 0..100 / 是否有补间在运行 |

两个关于"值 → 像素"映射的实现事实：

- 它使用移位/减法长除法，而不是 `(off << 8) / span`：后者在范围宽于 2²³ 时溢出，而 `int64` 在本项目中不可用。完整的 `int32_t` 定义域都被覆盖，包括 `vmax - vmin` 无法用 `int32_t` 表示的情形。
- 小数**四舍五入**（9 次迭代，而非 8 次），因此 5 像素条形的一半是 3 像素而不是 2 像素。这一点在 300 像素长的条形上看不出来，在短条形上十分明显。

### 10.4 径向工具箱 `cgl_arc`

该模块提供三样共享"圆上一点"的东西：覆盖率渐变、渐细抗锯齿刃形（指针）、着色径向表面（表盘/凹槽），以及圆环仪表控件。它依赖 `cgl_canvas` / `cgl_math` / `cgl_widget` / `cgl_anim`，不分配任何内存：所有表都由调用者提供，静态或栈上。每个部分都是输入的纯函数，帧与帧之间不保存任何状态。

**覆盖率**

```c
#define CGL_ARC_COV_N 128
uint16_t cgl_arc_cov_q6(int32_t d_q6);
```

`d_q6` 是像素在形状内部的**有符号距离**（Q6 像素；内部为正，外部为负，0 恰在边界上），返回 Q8 覆盖率（0..256）。它由一个 128 项的 Flash 表经一像素盒式滤波得到，与框架自身抗锯齿笔画使用的是**同一条**渐变。

**渐细刃形与指针**

```c
typedef struct {
    int32_t bx_q6, by_q6;   /* 基端（轴心侧） */
    int32_t ax_q6, ay_q6;   /* 轴线，基端 -> 针尖 */
    int32_t ux_q16, uy_q16; /* 轴线方向单位向量，Q16 */
    int32_t len_q6;         /* 沿轴线的长度，Q6 */
    int32_t wb_q6, wt_q6;   /* 基端/针尖半宽，Q6 */
    int32_t dw_q12;         /* (wt - wb) / 长度，Q12 */
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

- `cgl_arc_stroke_setup()` 由两个端点（Q6）与两个**全宽**（整像素）算出归一化轴线与渐细率。长度为零的刃形（基端等于针尖）返回 `false`，此时 `g` 未被改动，调用者不得用它绘制。
- `cgl_arc_stroke_draw()` 把渐细四边形按 `(ox, oy)` 平移后光栅化，并按 `alpha_q8` 混合；平移使调用者可以先铺一层投影而不必重建几何。它在逐像素层面由两个点积求半宽，**不做任何戳记**，因此没有接缝。其行走范围被钳位到 `cgl_canvas_src_window()`——这不是优化，而是让刃形可负担的前提（控件每条带绘制一次，走满包围盒等于一张图付 43 次）。
- `cgl_arc_needle()` 画一根完整的指针：先投影后墨色。`r_base` 是刃形向**中心之后**回伸的距离（被轴心遮住），`r_tip` 是伸出距离；宽度均为全宽。投影是**混合**，不是第二只深色指针；`shadow_alpha == 0` 时不画投影。`tip` 是可选输出：针尖的整像素坐标，供损坏模型使用。
- `cgl_arc_needle_damage()` 返回"某形状的指针覆盖的包围盒"与传入 `dmg` 的并集，包含针尖与尾端、笔的半宽、抗锯齿渐变与投影偏移。**忘记把 `shadow_off` 计入**会在指针移走后留下两像素的投影残影——这正是表盘"无残影"测试要捕捉的缺陷。

**不透明度包装函数**

```c
void cgl_arc_draw_arc(c, cx, cy, radius, thickness, start, sweep, color, alpha_q8);
void cgl_arc_draw_ring(c, cx, cy, radius, thickness, color, alpha_q8);
void cgl_arc_draw_circle(c, cx, cy, radius, color, alpha_q8);
void cgl_arc_draw_disc(c, cx, cy, radius, color, alpha_q8);
void cgl_arc_draw_line(c, x0, y0, x1, y1, thickness, color, alpha_q8);
```

它们在一次调用外围设置并恢复墨色不透明度（alpha 是画布的**状态**，不保存恢复会让后面所有元素都变淡）。`alpha_q8` 取 0（不可见）..256（`CGL_CANVAS_INK_OPAQUE`，完全不透明），超过 256 被钳位，等于 256 时走零成本路径。边缘在任何 alpha 下都保持抗锯齿，因为缩放的是覆盖率而不是几何：`cgl_arc_draw_ring(..., 128)` 是同一个圆环的一半不透明度，而不是更细的圆环。

两点注意：

- 这些函数**都不绘制背景**。在已有内容之上以 40% 画一个圆环正是其用途；在"空"之上画则会得到 40% 的墨色叠在条带当时的残留内容上。
- `cgl_arc_draw_disc()` **刻意不是** `cgl_canvas_disc()`：后者是块图元，整行填充、没有覆盖率渐变，因此完全无法半透明；可半透明的圆盘是圆弧图元半径为零的退化情形。`cgl_arc_draw_line()` 在 `thickness <= 1` 时进入普通 Bresenham 路径（字节写入），因此**不是**半透明的。

**着色径向表面**

```c
typedef enum { CGL_ARC_FACE = 0, CGL_ARC_RING = 1 } cgl_arc_surface_kind_t;
#define CGL_ARC_FACE_LUT_N 256
#define CGL_ARC_RING_LUT_N 33
```

`cgl_arc_surface_t` 描述"每像素一个颜色，来自一张表"。`FACE` 是圆盘（自一侧受光的径向渐变，叠加逐像素放射纹与高光），`RING` 是圆环截面（自上而下由暗到亮的半正弦，外加倾斜壁）。主要字段：`kind`、`cx`/`cy`（**图元坐标**）、`r_in`/`r_out`、`c_hi`/`c_mid`/`c_lo`、`knee_q8`（FACE 渐变翻转处）、`tex`/`tex_amp`/`tex_density`（拉丝纹）、`ray_amp`/`ray_min`（受光射线）、`spec`/`light_dx`/`light_dy`/`light_r`/`light_alpha`（高光）、`tilt_dark`/`tilt_light`/`tilt`（倾斜壁）、`lut`/`lut_n`。

| API | 语义 |
|---|---|
| `cgl_arc_surface_default_face(s)` / `_default_ring(s)` | 填入参考表盘/凹槽的参数（半径留给调用者设置，`lut` 置 `NULL`） |
| `uint16_t cgl_arc_surface_lut_entries(const cgl_arc_surface_t *s)` | 该表面的表项数：`lut_n` 非零时取它，否则取该类型的默认值 |
| `void cgl_arc_surface_bake(const cgl_arc_surface_t *s, uint16_t *lut, uint16_t n)` | 烘焙 `n` 项到 `lut`。**不在 `s` 中存任何东西**，由调用者赋值 `s.lut = table; s.lut_n = n;`，把"是 Flash 还是 RAM、谁来释放"的归属问题留在调用者手里。表项按其代表的**真实**半径求色（`sqrt(i/(n-1))`），即每个**表项**一次平方根，而不是每像素 |
| `cgl_color_t cgl_arc_surface_color(const cgl_arc_surface_t *s, int16_t dx, int16_t dy)` | 相对中心偏移处的表面颜色：渐变、纹理、放射纹与高光的总和。**纯函数**，不读写状态，可在任何绘制之前调用（因此可作测试基准） |
| `void cgl_arc_surface_paint(cgl_canvas_t *c, const cgl_arc_surface_t *s)` | 把表面画进当前条带（表面 ∩ 裁剪窗口 ∩ 条带）。逐行跨度由一次 `isqrt` 得到，内部像素不透明写入。**有 warp 时不支持**：此时画成平面圆盘（FACE）或平面圆环（RING），用表面的边缘色——看不出来的渐变不值得付出帧率 |

表按 `r²` 而不是按 `r` 索引，因为内层循环只知道 `dx*dx + dy*dy`；表项建立在它真正代表的半径上，因此画出来的曲线就是设计的那条曲线。`lut == NULL` 合法：改用逐像素程序化着色，约三倍于查表路径的开销，但 Flash 占用为零。

**圆环仪表 `cgl_arc_gauge`**

仪表是 `cgl_bar` 的径向孪生：把一个值按区间画成一段扫掠。它**不绘制自己的背景**（会画空轨道，但不填 frame），设计上应坐在表盘、卡片或面板之上。

| API | 语义 |
|---|---|
| `cgl_arc_gauge_init(g, name, frame, radius, thickness)` | 以 frame 为中心，覆盖整圈（`start = CGL_ANGLE_UP`，`span = CGL_ANGLE_TURN`，顺时针），范围 0..100，值 0，无引导点、无闪光 |
| `bool cgl_arc_gauge_set_range(g, vmin, vmax)` | 同 `cgl_bar_set_range()`：`vmax <= vmin` 被拒绝并计入 `cgl_arc_range_rejected()` |
| `cgl_arc_gauge_set_value(g, value)` | 立即设置并绘制，钳位到范围 |
| `bool cgl_arc_gauge_animate_to(g, value, dur_ms, ease)` | 与条形同名函数同语义，从当前绘制值出发，`dur_ms == 0` 退化为立即设置 |
| `cgl_arc_gauge_set_span(g, start, span, dir)` | 扫掠的起点、整量程覆盖的角度、方向（`CGL_ARC_CW` / `CGL_ARC_CCW`）。例如"225 度开口、315 度闭合"的速度表：`start = CGL_ANGLE_DEG(135)`，`span = CGL_ANGLE_DEG(270)`，`CGL_ARC_CW` |
| `cgl_arc_gauge_set_colors(g, track, ink)` | 未填充弧与已完成弧的颜色 |
| `cgl_arc_gauge_set_pulse_color(g, color)` | 闪光期间轨道混合趋向的颜色 |
| `cgl_arc_gauge_set_dot(g, r, grow, color)` | 在前端画半径 `r` 的引导点，一次完整闪光内长大 `grow` 像素；`r == 0`（默认）取消 |
| `cgl_arc_gauge_set_sweep(g, sweep)` | **直接**设置扫掠长度（角度单位），绕过"值 → 比例"映射，直到下一次 `set_value()`。**不标记损坏** |
| `uint32_t cgl_arc_gauge_sweep(const cgl_arc_gauge_t *g)` | 当前绘制扫掠长度（0..span） |
| `cgl_arc_gauge_set_scale(g, q8)` | 只绘制当前扫掠的 `q8/256`。256 是全部；0 只画轨道（引导点被抑制）。**不标记损坏** |
| `cgl_arc_gauge_pulse(g, dur_ms, ease)` | 启动闪光：轨道向 `pulse_to` 混合、引导点长大，二者在 `dur_ms` 内从 256 衰减到 0。补间从控件的 `update` 运行，并在运行期间标记损坏 |
| `cgl_arc_gauge_set_pulse(g, q8)` | 直接设置环境闪光强度 Q8；停止运行中的闪光补间。**不标记损坏**（由所有者驱动、由所有者标记） |
| `cgl_arc_gauge_set_center(g, cx_off, cy_off)` | 把仪表中心移到相对 frame 原点的偏移处 |
| `cgl_point_t cgl_arc_gauge_tip(const cgl_arc_gauge_t *g)` | 当前扫掠终点（屏幕坐标）：引导点所在处，也是弧"发生变化"的唯一位置。**这正是损坏模型需要的** |
| `cgl_arc_gauge_get_value` / `_get_shown_value` / `_is_animating` | 逻辑值 / 绘制值 / 是否有补间在运行 |

分组约定：**值类 setter**（`set_value`、`animate_to`、`set_range`、`set_span`、`set_colors`、`set_pulse_color`、`set_dot`、`set_center`）按 `cgl_bar` 的方式标记损坏；**所有者门**（`set_sweep`、`set_scale`、`set_pulse`）**不**标记损坏。"扫掠由时钟推导、且损坏模型已知圆环前端落在哪些条带"的调用者，不应让控件把 64×64 的方框并入每帧损坏——那是每帧推 22 条带与 39 条带的差别。

闪光的补间方向是 **256 → 0**，而不是 0 → 256，方向本身就是要点：脉冲的静止值必须是"什么都没发生"的值，否则第一次闪光之后所有乘以它的绘制都会永久带上色调。

实现提示（可视为约束）：`set_sweep()` 是设置直接扫掠的唯一入口；不要手工写 `cgl_arc_gauge_t` 的 `direct` / `direct_sweep` 字段，实现侧把该状态放在按仪表地址索引的小表里，手工置位不会生效。

### 10.5 表盘 `cgl_watch`

`cgl_watch` 是一个完整的模拟表盘控件：着色圆盘表盘、嵌入式凹槽、60 格分度环、秒指示环、三根渐细指针、轴心，可选的秒针彗尾与玻璃罩。它**建立在 `cgl_arc` 之上**（表盘与凹槽是两个 `cgl_arc_surface_t`，三根指针是 `cgl_arc_stroke_t`，秒环是半径为 0、厚度为环宽的圆弧，指针损坏框来自 `cgl_arc_needle_damage()`）。

**时间与日历类型**

```c
typedef struct {
    uint8_t h, m, s;
    uint16_t ms;    /* 当前秒内的毫秒，0..999 */
    uint16_t year;
    uint8_t month;  /* 1..12 */
    uint8_t day;    /* 1..31 */
    uint8_t wday;   /* 0 = 周日 */
} cgl_watch_time_t;
```

辅助函数（均为纯函数）：

| API | 语义 |
|---|---|
| `bool cgl_watch_is_leap(uint16_t year)` | 闰年判定（含 100 与 400 年规则） |
| `uint8_t cgl_watch_days_in_month(uint16_t year, uint8_t month)` | 某月天数；越界月份按 12 取模，因此结果总是一个真实月份的长度 |
| `void cgl_watch_next_day(cgl_watch_time_t *t)` | 日期加一天，含月与年进位 |
| `uint32_t cgl_watch_seconds_of_day(const cgl_watch_time_t *t)` | 自午夜起的秒数 |
| `cgl_angle_t cgl_watch_fraction_angle(num, den)` | `num/den` 圈对应的角度（要求 `num <= 65535`） |
| `cgl_angle_t cgl_watch_face_angle(num, den)` | 表盘读数角度：`CGL_ANGLE_UP` 加上顺时针部分。"时钟约定"就这一行 |
| `uint32_t cgl_watch_second_yank_ms(ms, tick_ms, tick_ease)` | 跳动走法中秒针越过上一秒刻度的毫秒数 |
| `cgl_angle_t cgl_watch_second_angle(const cgl_watch_time_t *t, uint8_t mode, uint16_t tick_ms, uint8_t tick_ease)` | 秒针角度。两种走法都是时间的纯函数，且在每一个整秒处一致 |
| `cgl_angle_t cgl_watch_hour_angle(const cgl_watch_time_t *t)` | 时针角度，**连续的**（像真实时针一样每秒爬过一点点） |
| `cgl_angle_t cgl_watch_minute_angle(const cgl_watch_time_t *t)` | 分针角度，同样连续 |
| `uint32_t cgl_watch_minute_sweep(const cgl_watch_time_t *t)` | 分钟已走过的角度，供秒指示环使用 |

秒针走法由 `cgl_watch_second_mode_t` 选择：

| 取值 | 含义 |
|---|---|
| `CGL_WATCH_SECOND_SWEEP` | 经典石英扫秒：`(s*1000 + ms) / 60 s` 圈 |
| `CGL_WATCH_SECOND_TICK` | 机械跳动：停在本秒刻度上，在最后 `tick_ms` 内被"抽"到下一格，中途略微过冲再回落。该运动是**时间与走法的函数**，不是补间，因此掉帧、被覆盖、任意帧率都不会让它失步 |

**配置**：`cgl_watch_config_t` 由四部分组成。

```c
typedef struct { int16_t r_base, r_tip; uint8_t w_base, w_tip; cgl_color_t color; } cgl_watch_hand_t;
```

- `cgl_watch_geom_t`：`cx`/`cy`（**屏幕坐标**的盘心）、`r_face`、`r_ring`、`ring_w`、`r_well_in`/`r_well_out`、`r_tick_out`、`r_tick_major_in`、`r_tick_minor_in`、`r_hub`、`r_tail`。
- `cgl_watch_anim_t`：`tick_count`（刻度数，60）、`tick_major`（每 n 格一个主刻度，5）、`tick_w_minor`/`tick_w_major`、`second_mode`、`tick_ms`、`tick_ease`、`reveal_ms`/`reveal_ease`（入场扫描）、`pulse_ms`/`pulse_ease`（分钟跳变闪光）、`trail_ms`（彗尾余辉）、`shadow_off`/`shadow_alpha`（指针投影）。
- `cgl_watch_style_t`：`bg`、`bezel`/`bezel_hi`/`bezel_w`/`bezel_lit_start`/`bezel_lit_sweep`（表壳唇边及其受光半圈）、`tick`/`tick_major`/`trail`/`hand`/`hub`/`accent`/`accent_hi`（刻度、指针、轴心、强调色）、`face`/`well`（两个 `cgl_arc_surface_t`）、`glass`（`cgl_watch_glass_t`）。
- 指针：`hour`、`minute`、`second`；锚点 `start`（起始读数与日期）；`flags`（`CGL_WATCH_F_*`）；可选的 `brand`/`brand_font`/`brand_dx`/`brand_dy`/`brand_color`。

**运行时标志**（`cgl_watch_config_t::flags`）：

```c
#define CGL_WATCH_F_BEZEL  0x0001u   /* 表壳唇边 */
#define CGL_WATCH_F_TICKS  0x0002u   /* 分度环 */
#define CGL_WATCH_F_RING   0x0004u   /* 秒指示环与引导点 */
#define CGL_WATCH_F_TRAIL  0x0008u   /* 秒针后的彗尾 */
#define CGL_WATCH_F_HUB    0x0010u   /* 轴心盖 */
#define CGL_WATCH_F_GLASS  0x0020u   /* 玻璃罩 */
#define CGL_WATCH_F_BRAND  0x0040u   /* 品牌文字 */
#define CGL_WATCH_F_HOUR   0x0080u
#define CGL_WATCH_F_MINUTE 0x0100u
#define CGL_WATCH_F_SECOND 0x0200u
#define CGL_WATCH_F_PULSE  0x0400u   /* 分钟跳变闪光 */
#define CGL_WATCH_F_REVEAL 0x0800u   /* 入场扫描 */
#define CGL_WATCH_F_DEFAULT  /* 上述全部，但不含 TRAIL 与 BRAND */
```

`CGL_WATCH_F_DEFAULT` 是一个"普通可用表盘"：不含品牌文字（需要字符串）与彗尾（需要 RAM）。编译期开关与运行时标志是两件不同的事：`CGL_CFG_WATCH_GLASS = 0` 的构建里根本没有玻璃罩代码，此时 `CGL_WATCH_F_GLASS` 被接受并忽略（配置结构因而可在不同构建间移植）。彗尾的**运行时**生效要求 `CGL_WATCH_F_TRAIL` 与 `CGL_WATCH_F_TICKS` **同时**置位：辉光由分度刻度承载，只置 TRAIL 而不置 TICKS 等于什么都没有。

**API**

| API | 语义 |
|---|---|
| `void cgl_watch_config_default(cgl_watch_config_t *cfg)` | 填入完整可用的参考表盘（演示页的盘面：几何、配色、表面、指针、刻度，起始读数 10:09:00，2026-01-14 星期三）。`CGL_CFG_WATCH_DIAL_LUT = 1` 时把 `style.face.lut` / `well.lut` 指向模块自带的 **Flash** 表；为 0 时留 `NULL`（程序化着色，不占 Flash） |
| `void cgl_watch_config_set_start(cfg, h, m, s)` | 设置起始读数（锚点）。越界值按上一级单位取模 |
| `void cgl_watch_config_set_start_date(cfg, year, month, day, wday)` | 设置锚点日期；月份与日期被**钳位**（不是取模），因为"13 月"或"0 日"是调用者缺陷，静默渲染"0 FEB"只会掩盖它 |
| `void cgl_watch_init(w, name, frame, const cgl_watch_config_t *cfg)` | 创建控件。`cfg` 为 `NULL` 时使用默认配置。它**复制**配置（`w->cfg = *cfg`），不保存指针。控件**不绘制背景**，其 frame 应被裁剪（`cgl_widget_init()` 默认设置 `CGL_WF_CLIP`） |
| `void cgl_watch_frame(cgl_watch_t *w, const cgl_clock_t *clk)` | **每帧一次**：保持时间、运行入场与闪光、老化彗尾，并只标记表盘真正改变了的损坏。`clk == NULL` 是安全的空操作 |
| `void cgl_watch_set_time(w, h, m, s)` | 重新锚定到某个墙钟时间（RTC 挂钩）。请求被暂存，在下一次 `cgl_watch_frame()` 开头生效，因此在首帧之前调用也合法且精确。日期不受影响 |
| `void cgl_watch_set_date(w, year, month, day, wday)` | 重新锚定日历日期，`wday` 由调用者给出（0 = 周日）。越界值被钳位 |
| `const cgl_watch_time_t *cgl_watch_now(const cgl_watch_t *w)` | 当前显示的时间（首帧之后有效；之前是配置的起始读数） |
| `uint32_t cgl_watch_seconds_of_day_now(const cgl_watch_t *w)` | 当前显示读数的当日秒数 |
| `void cgl_watch_reveal(cgl_watch_t *w)` | 播放入场：指针从 12:00 扫到当前时刻，分度环逐格组装。`CGL_WATCH_F_REVEAL` 置位时由 `cgl_watch_init()` 自动播放**一次**；希望每次进入页面都重播的页面在自己的 `on_enter` 中调用它。**设置时间不会重播它** |
| `uint16_t cgl_watch_reveal_value(const cgl_watch_t *w)` / `cgl_watch_pulse_value(...)` | 入场进度 Q8（256 = 完全就位）/ 分钟闪光当前强度 Q8（256 = 峰值，0 = 静止） |
| `void cgl_watch_set_second_mode(w, mode)` / `cgl_watch_set_tick(w, tick_ms, ease)` | 切换扫秒/跳动，以及跳动的抽击时长与曲线 |
| `void cgl_watch_set_flags(w, flags)` / `cgl_watch_set_feature(w, flag, on)` / `bool cgl_watch_has_flags(w, flags)` | 整组/单个元素开关，以及查询 |
| `void cgl_watch_set_center(w, cx, cy)` | 移动表盘：几何、两个表面的中心与控件 frame 按同一增量移动 |
| `void cgl_watch_set_config(w, cfg)` / `const cgl_watch_config_t *cgl_watch_get_config(const cgl_watch_t *w)` | 整体替换配置（设置界面的"应用"）/ 读取当前配置。替换时分针与秒针的两个补间**刻意不同步重置**，否则改配色会让表盘先闪回 12:00 |
| `cgl_color_t cgl_watch_face_color(w, dx, dy)` / `cgl_watch_ring_color(w, dx, dy)` | 表盘/凹槽表面在偏移处的颜色，纯函数，可在首次绘制之前调用 |
| `cgl_color_t cgl_watch_glass_over(w, under, dx, dy)` / `cgl_watch_glass_shade(const cgl_watch_glass_t *gl, under, dx, dy)` | 玻璃罩对某个颜色的作用（把它反射的光合成到 `under` 之上，`under` 可以是表盘、凹槽、刻度、指针）。后者只看参数，因此两者不可能给出不同答案。`CGL_CFG_WATCH_GLASS = 0` 时返回 `under` 本身（参数仍然接受，配置结构保持可移植） |
| `uint32_t cgl_watch_frames(const cgl_watch_t *w)` | 该控件收到的 `cgl_watch_frame()` 调用次数 |

**必须知道的三条契约**

1. **表盘不自驱动。** `cgl_watch_t` 的虚表 `update` 为 `NULL`，因此 `cgl_widget_update_tree()` / `cgl_screen_frame()` **不会**推进它。页面必须在自己的 `on_frame` 中调用 `cgl_watch_frame(&face, clk)`。时间取自框架的帧时钟而不是 `dt`：锚点必须是某个 `clk->now_ms`，这也是"表盘在被覆盖、不被绘制时仍然走时"的原因。相比之下，`cgl_arc_gauge_t` 的虚表带 `update`，其补间由控件树更新推进。
2. **时间是锚点加累计，日期是状态。** 表盘内部保存 `t0`（锚点时刻的墙钟）与 `t0_ms`（锚点时刻的 `clk->now_ms`），每帧推导 `now = t0 + (now_ms - t0_ms)`；该减法是无符号的，因此 32 位毫秒计数回绕是安全的。日期**不能**每帧从锚点重算（否则每天只有一帧显示正确日期），它是逐日累计出来的——这也是没有 RTC 也能显示日期的原因。
3. **损坏纪律。** 增量路径只标记真正变化的区域：秒针（含配重尾）在旧角度与新角度各一次；分针与时针仅在整数针尖端点真的改变时标记；秒环前端在旧读数与新读数各一次；仍带彗尾辉光的刻度。入场或闪光运行期间、以及首帧之前，才整控件标脏。损坏**刻意不**受 `CGL_WATCH_F_*` 开关约束：刚把秒针关掉的表盘仍须擦掉上一帧留下的墨色，多推一条带只是一次重绘，而少推一条就是残影。

### 10.6 其它控件模块

`cgl_panel`、`cgl_label`、`cgl_bar`、`cgl_arc`、`cgl_watch` 都遵循同一条模式：以 `cgl_widget_t` 为首成员的结构体 + 一张用指定初始化器写的虚表 + 只在像素真的变化时才标记损坏的 setter。新增控件时以 `cgl_panel.c`（最简）与 `cgl_bar.c`（带补间、可配置几何、自带失败计数器）为参考。

---

## 11. 显示设备与带呈现流水线

### 11.1 显示实例

```c
typedef struct
{
    const cgl_display_drv_t *drv;
    void *ctx;

    cgl_rect_t damage;                     /* 待重绘区域，屏幕坐标 */

    cgl_color_t band[2][CGL_CFG_SCREEN_W * CGL_CFG_BAND_H];   /* 双缓冲条带 */

#if CGL_CFG_ALPHA
    cgl_color_t alpha[CGL_CFG_ALPHA_LEVELS * CGL_CFG_ALPHA_LEVEL_STRIDE];  /* 组不透明度暂存 */
#endif

    uint32_t frame_id;   /* 已推送到面板的帧数（**不是**帧率） */
    uint32_t bands;      /* 累计推送的条带数（供观测）        */
} cgl_display_t;
```

### 11.2 API

| API | 语义 |
|---|---|
| `int cgl_display_init(d, drv, ctx)` | 绑定驱动；`init` 失败时返回非零。同时把整屏标记为损坏 |
| `cgl_display_damage(d, r)` / `damage_all(d)` / `has_damage(d)` | 损坏累积（矩形并集，**不是**区域列表） |
| `uint16_t cgl_display_present(d, render, user)` | 只处理与屏幕相交的损坏；每条带调用一次 `render`（整条带重绘）；返回推送的条带数 |
| `cgl_display_set_backlight(d, percent)` | 驱动未实现背光时的安全空操作 |

渲染回调：

```c
typedef void (*cgl_render_fn)(cgl_canvas_t *canvas, void *user);
```

### 11.3 呈现流程

`present()` 的流水线：渲染 `band[cur]` → `set_window` → 异步 `write_pixels` → 渲染 `band[!cur]` → 等待上一条带结束……最后一条带以 `wait_idle` 收尾。条带的起始行对齐到 `CGL_CFG_BAND_H` 的整数倍，末尾允许是较矮的残余条带。

`frame_id` 是"已推送的帧数"，`bands` 是累计条带数；两者都不是帧率——帧率在 `cgl_clock` 中。

### 11.4 两条必须维持的约束

1. **整条带必须从控件树根重绘。** `present()` 从不"只重绘损坏部分"：面板没有帧缓冲，推上去的条带必须是完整的。save-under 合成器同样依赖这一点，它的"背景副本"就是本条带已经绘制好的内容。
2. **`d->alpha[]` 只由 CPU 写入，永不进入 DMA。** 它与 `band[]` 不同：当第 N 条带在渲染时，`band[!cur]` 正被 DMA 读取，因此那一块不能用作暂存区。这也是暂存区必须是独立内存（而不是复用另一条带缓冲）的原因。

### 11.5 在 PC 上做逐像素回归

把驱动换成写入内存数组的模拟实现，框架代码本身一行不改，即可在主机上逐像素断言。测试工程正是这样做的：模拟驱动把每个条带按 `set_window` 给出的窗口拷进行缓冲，断言直接读该缓冲。这是验证自绘控件像素级正确性的首选方式。

---

## 12. 时钟与补间

### 12.1 时钟

```c
typedef struct
{
    uint32_t now_ms;      /* 自 cgl_clock_init 起的毫秒数 */
    uint32_t frame;       /* 已开始的帧数（首帧为 1）      */
    uint32_t dt_ms;       /* 本帧 dt，已被 max_dt_ms 钳位  */

    uint32_t step_ms;     /* 每个逻辑步长的毫秒数；0 = 不跑逻辑步 */
    uint32_t accum_ms;    /* 定步累加器（子步余数留在这里）       */
    uint32_t max_dt_ms;   /* 单帧 dt 上限                        */
    uint32_t step_total;  /* 逻辑步总数，**不因 reset 清零**（供观测） */
    uint8_t  max_steps;   /* 单帧步数上限                        */
    uint8_t  steps;       /* 本帧计划执行的步数                  */
    uint8_t  step_index;  /* cgl_clock_next_step() 的迭代游标    */

    uint32_t fps;         /* 最近一个统计窗口的帧率 */
    /* _fps_ms / _fps_frames 为内部字段 */
} cgl_clock_t;

#define CGL_CLOCK_MS_PER_S 1000u
#define CGL_CLOCK_DEFAULT_MAX_DT_MS 100u
```

| API | 语义 |
|---|---|
| `cgl_clock_init(c, step_ms, max_dt_ms)` | `step_ms = 0` 表示只跑帧时间、不跑逻辑步（纯界面场景）。必须在第一次 `cgl_clock_begin()` 之前调用 |
| `cgl_clock_reset(c)` | 清除计数，保留 `step_ms` / `max_dt_ms` 配置。用于重启场景 |
| `cgl_clock_begin(c, dt_ms)` | 开始一帧。**这是整个框架中唯一累积时间的地方。** 超过 `max_dt_ms` 的部分被截断，且之后不会追补 |
| `bool cgl_clock_next_step(c)` | 迭代本帧的定步：返回 `true` 表示还有一步可跑，每次调用消耗一步。步数由 `begin()` 从累加器算出，因此循环体恰好执行应执行的次数，调用者既不能多跑也不能少跑 |
| `uint8_t cgl_clock_steps_left(c)` | 本帧还剩几步（供"仅在最后一步做收尾"的写法） |
| `uint32_t cgl_clock_step_ms(c)` | 一个逻辑步长的毫秒数（即 `on_step` 收到的值） |
| `uint16_t cgl_clock_alpha_q8(c)` | 帧间插值因子 Q8（0..255）：累加器在当前步内推进了多少。本项目不使用（RAM 容不下第二份位置数组），但接口保留，因为这是唯一能取得该比例的地方 |
| `uint32_t cgl_clock_fps(c)` | 最近一个统计窗口的帧率；窗口结束才更新，因此前 500 ms 返回 0 |

设计要点：

- `step_ms` 域（逻辑）固定步长、可重放、与帧率无关，交给 `on_step`；`dt_ms` 域（视觉）跟随帧率、允许丢帧，交给 `on_frame` 与控件 `update`。分辨率不同是刻意的：仿真速度不能取决于 SPI 带宽，而淡入淡出跟随帧率最好看。
- "世界被推进了两次"在结构上不可能：控件根本无步可推，唯一的推进点是页面的 `on_step`。
- 卡顿不会被追补：整步欠账超过上限时一次性丢弃，只保留子步余数。宁可让仿真略微走慢，也不做"卡顿后快进"。

### 12.2 补间

一个 `cgl_anim_t` 就是一条时间线（`from -> to`、时长、缓动、可选延迟）。纯整数 Q8，无浮点、无 libm。

```c
typedef enum
{
    CGL_EASE_LINEAR = 0,  /* 匀速：机械，但最不容易出错 */
    CGL_EASE_IN_QUAD,     /* 慢启动（加速）             */
    CGL_EASE_OUT_QUAD,    /* 快启动后减速；最常用的"自然"曲线 */
    CGL_EASE_IN_OUT_QUAD, /* 两端慢、中间快             */
    CGL_EASE_OUT_CUBIC,   /* 比 OUT_QUAD 更"软"的收尾；页面默认 */
    CGL_EASE_OUT_BACK,    /* 过冲再弹回（Q8 值会超过 256），用于强调 */
    CGL_EASE_COUNT
} cgl_ease_t;

#define CGL_ANIM_ONE_Q8 256
#define CGL_ANIM_F_RUNNING 0x01u   /* 仍在运行（含延迟阶段） */
#define CGL_ANIM_F_DONE    0x02u   /* 已到达 to；值不再变化  */

typedef struct
{
    int32_t  from, to, value;   /* 起点 / 终点 / 当前值 */
    uint32_t dur_ms;            /* 时长；0 表示下一帧直接到 to */
    uint32_t elapsed_ms;        /* 已过时长（不含延迟） */
    uint32_t delay_ms;          /* 剩余延迟；>0 期间值停在 from */
    uint8_t  ease;              /* cgl_ease_t */
    uint8_t  flags;             /* CGL_ANIM_F_* */
} cgl_anim_t;
```

| API | 语义 |
|---|---|
| `cgl_anim_init(a)` | 清为"从未启动"的补间（`value = 0`） |
| `cgl_anim_start(a, from, to, dur_ms, ease)` | 启动；清除延迟与完成标志 |
| `cgl_anim_set_delay(a, ms)` | 延迟启动。必须在 `start()` **之后**调用（`start` 会清除延迟） |
| `bool cgl_anim_tick(a, dt_ms)` | 推进一帧；返回"本帧值是否改变"，调用者据此决定是否重绘。延迟期间与完成之后都返回 `false` |
| `cgl_anim_finish(a)` / `cgl_anim_stop(a)` | 立即跳到终值并置 DONE / 原地停止并清除 RUNNING（不跳到终值） |
| `cgl_anim_value(a)` / `cgl_anim_running(a)` / `cgl_anim_done(a)` | 内联读取当前值 / 是否运行中 / 是否完成 |
| `uint16_t cgl_anim_progress_q8(a)` | 线性进度 Q8（0..256），不含缓动与延迟 |
| `uint16_t cgl_anim_ease(ease, p_q8)` | 缓动函数本身：线性进度 → 缓动后进度。除 `OUT_BACK` 外输出都在 0..256 内，端点精确命中 0 与 256 |
| `int32_t cgl_anim_lerp(a, b, t_q8)` | Q8 线性插值：`t = 0` 返回 `a`，`t = 256` 返回 `b` |

补间也常被当作定时器使用：`cgl_anim_start(&t, 0, 1, delay_ms, CGL_EASE_LINEAR)`，时间到时 `cgl_anim_done(&t)` 为真。

**不要给 Genie / S 形吸入配 `CGL_EASE_OUT_BACK`。** 滑动/覆盖类型正是靠它的过冲（进度短暂超过 256）实现"过冲再弹回"，而 Genie 走的是几何变形路径；当 `p > 256` 时收窄系数大于 256，通道宽度因子变为**负数**，画面会水平镜像并越过吸入口。`cgl_page_nav_set_ease()` 是"全改"，会波及这两种类型；若想"滑动带弹跳、吸入不变"，请用 `cgl_page_nav_set_transition_ease()` 按类型分别配置。

---

## 13. 页面与导航

### 13.1 页面对象与虚表

页面是一个**对象**（而不是数字标识），携带一张虚表；导航器是一个定长栈（`CGL_CFG_PAGE_STACK_MAX`，默认 4，无堆）。

```c
typedef struct
{
    const char *type;

    void (*on_create)(cgl_page_t *p);                    /* 首次进入时构建控件树（仅一次） */
    void (*on_destroy)(cgl_page_t *p);                   /* 真正被丢弃时拆树            */

    void (*on_enter)(cgl_page_t *p, cgl_page_t *from, uint8_t kind);  /* 开始进入 */
    void (*on_exit)(cgl_page_t *p, cgl_page_t *to);                   /* 开始离开 */

    void (*on_pause)(cgl_page_t *p, cgl_page_t *by);     /* 被压在下面（PUSH/REPLACE）  */
    void (*on_resume)(cgl_page_t *p, cgl_page_t *from);  /* 上面的页面弹走，重新露出     */

    void (*on_step)(cgl_page_t *p, uint32_t step_ms);    /* 定步逻辑；仅栈顶页面         */
    void (*on_frame)(cgl_page_t *p, const cgl_clock_t *clk);  /* 每帧一次；仅栈顶页面    */

    bool (*on_event)(cgl_page_t *p, const cgl_event_t *ev);   /* 输入；仅栈顶，转场期间不派发 */
} cgl_page_vt_t;
```

**所有成员都可以为 `NULL`**（用指定初始化器书写，缺省即 `NULL`）。

```c
struct cgl_page
{
    const cgl_page_vt_t *vt;   /* 第一个成员，因此可以像控件一样"继承" */
    const char *name;
    cgl_widget_t *root;        /* 本页控件树根；在 on_create 中绑定 */
    cgl_page_nav_t *nav;       /* 所属导航器；由导航器压栈时填入    */
    void *user;                /* 页面自己的上下文（应用层使用）    */
    void *arg;                 /* 本次压栈的参数，见 cgl_page_nav_request */
    uint16_t flags;

    cgl_page_trans_cfg_t enter;  /* 本页**出现**时的转场偏好（PUSH 的进入页）   */
    cgl_page_trans_cfg_t leave;  /* 本页**消失**时的转场偏好（POP/REPLACE 的离开页） */
};

#define CGL_PAGE_F_CREATED    0x0001u   /* on_create 已执行（内部使用） */
#define CGL_PAGE_F_SHOW_BELOW 0x0002u   /* 本页在栈顶时，下面的页面继续绘制 */
```

页面把自己的控件作为**静态成员**持有，因此"销毁 → 创建"可以在同一块静态内存上反复循环，既不需要堆也不会泄漏。

### 13.2 生命周期顺序

| 动作 | 顺序 |
|---|---|
| PUSH | `on_create`（新页）→ `on_pause`（旧页）→ `on_enter`（新页）→ 转场 → 旧页被隐藏 |
| POP | `on_resume`（下层）→ `on_exit`（顶层）→ 转场 → `on_destroy`（顶层） |
| REPLACE | `on_exit`（旧页）→ `on_create`/挂载（新页）→ `on_enter`（新页）→ 转场 → `on_destroy`（旧页） |
| RESET | 依次对所有旧页 `on_exit` + `on_destroy` → 挂载新页 → `on_enter` → **无转场** |

时间契约：

- `on_step(step_ms)`：只有**栈顶**页面收到，每帧 0..N 次，固定步长。物理/逻辑写在这里。被压住的页面收不到，因此"被暂停的页面时钟仍在走"从结构上不可能发生。
- `on_frame(clk)`：只有栈顶页面收到，每帧恰好一次。补间、状态机、文本刷新写在这里。
- `on_event(ev)`：只有栈顶页面收到，且**转场期间输入被锁定**（否则用户可以在半途叠三层导航）。

### 13.3 页面的转场偏好

```c
typedef struct
{
    uint8_t  trans;   /* cgl_trans_t；CGL_TRANS_AUTO = 未声明类型 */
    uint8_t  ease;    /* cgl_ease_t；CGL_PAGE_FX_KEEP_EASE = 未声明曲线 */
    uint16_t dur_ms;  /* 毫秒；CGL_PAGE_FX_KEEP_MS = 未声明时长 */
} cgl_page_trans_cfg_t;

#define CGL_PAGE_FX_KEEP_EASE CGL_EASE_COUNT   /* 曲线：回落到 nav->ease[] */
#define CGL_PAGE_FX_KEEP_MS   0u               /* 时长：回落到 nav->duration_ms[] */
```

```c
void cgl_page_set_trans_enter(cgl_page_t *p, cgl_trans_t trans, cgl_ease_t ease, uint32_t dur_ms);
void cgl_page_set_trans_leave(cgl_page_t *p, cgl_trans_t trans, cgl_ease_t ease, uint32_t dur_ms);
```

三个参数共同描述**完整**的偏好，而不是"只改传入的那一项"：想保留原类型只改手感，就把原类型再传一次。

```c
/* 类型与手感都声明 */
cgl_page_set_trans_enter(&p->base, CGL_TRANS_COVER_UP, CGL_EASE_OUT_BACK, 180u);

/* 只声明类型，手感回落到类型表 */
cgl_page_set_trans_enter(&p->base, CGL_TRANS_GENIE_IN,
                         CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);

/* 撤销声明：回到"听调用者的 / 用动作默认值" */
cgl_page_set_trans_enter(&p->base, CGL_TRANS_AUTO,
                         CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
```

性质与警告：

- **默认是"什么都没声明"**：`cgl_page_init()` 清除两项偏好，因此不碰该机制的页面行为与之前**完全一致**。
- **在 `on_create` 中声明对该次导航有效**：偏好只在导航被提交时（`nav__start()`）读取，而进入页的 `on_create` 在此之前执行。
- **声明在 `destroy -> create` 之后仍然有效**：它是页面的静态配置，不是每帧或每次压栈重算的东西。
- **`dur_ms` 为 0 表示"未声明"**；要表达"本页瞬间切换"必须声明 `trans = CGL_TRANS_NONE`。
- **曲线陷阱**：页面上声明的曲线与最终使用的类型是**分开**解析的。在"类型未声明、调用者最终传了 `GENIE_IN`"的页面上声明 `OUT_BACK` 会撞上 12.3 节末尾的问题——想要弹跳，请把类型也声明成滑动类。
- **`RESET` 永远是瞬间切换**，声明 `enter` 对它无效。
- 越界的 `trans`（≥ `CGL_TRANS_COUNT`）使**整次调用**无效（不会留下"改了一半"的偏好）；越界的 `ease` 被钳位为 `CGL_EASE_LINEAR`；`dur_ms` 被钳位到 65535。

### 13.4 导航动作与转场类型

```c
typedef enum
{
    CGL_NAV_PUSH = 1,  /* 压栈：旧页留在下面（on_pause），返回时 on_resume */
    CGL_NAV_POP,       /* 弹栈：顶层页被丢弃，下面一页重新露出            */
    CGL_NAV_REPLACE,   /* 替换顶层：旧页被丢弃，深度不变                  */
    CGL_NAV_RESET      /* 清空栈只留新页（回主界面）；无转场              */
} cgl_nav_kind_t;

typedef enum
{
    CGL_TRANS_NONE = 0,     /* 瞬间切换。调用者显式传入时**覆盖页面声明**      */
    CGL_TRANS_SLIDE_LEFT,   /* 新页从右侧进入，旧页向左退出                    */
    CGL_TRANS_SLIDE_RIGHT,
    CGL_TRANS_SLIDE_UP,
    CGL_TRANS_SLIDE_DOWN,
    CGL_TRANS_COVER_UP,     /* 只有新页移动，自下而上覆盖旧页（菜单/弹窗语义）  */
    CGL_TRANS_COVER_DOWN,
    CGL_TRANS_FADE,         /* 经背景色淡出淡入：前半段旧页淡出，后半段新页淡入 */
    CGL_TRANS_GENIE_OUT,    /* 旧页被"吸入"吸入口，露出下面一页（macOS 最小化） */
    CGL_TRANS_GENIE_IN,     /* 新页从吸入口"倒出"（macOS 从 Dock 恢复）        */
    CGL_TRANS_S_OUT,        /* 同 GENIE_OUT，但吸入口在右下角，路径为 S 形      */
    CGL_TRANS_S_IN,         /* 同 GENIE_IN，从右下角出来，S 形反向             */
    CGL_TRANS_AUTO,         /* 交给导航器按动作取默认值（推荐）                */
    CGL_TRANS_COUNT
} cgl_trans_t;
```

| 动作 | 默认转场 | 语义 |
|---|---|---|
| `CGL_NAV_PUSH` | `GENIE_IN` | 压栈：旧页保留（`on_pause`），新页从吸入口倒出 |
| `CGL_NAV_POP` | `GENIE_OUT` | 弹栈：当前页被吸入吸入口，露出下面一页 |
| `CGL_NAV_REPLACE` | `GENIE_OUT` | 替换栈顶，深度不变（旧页被吸走） |
| `CGL_NAV_RESET` | `NONE` | 清空栈回到主界面（瞬间切换更自然） |

| 转场 | 效果 |
|---|---|
| `GENIE_OUT` | 离开的页面被"吸入"吸入口，下面一页**在整段动画期间保持可见** |
| `GENIE_IN` | 进入的页面从吸入口倒出，离开的页面留在下面不变 |
| `S_OUT` / `S_IN` | 与 `GENIE_*` 共用同一套漏斗几何，区别只有两点：吸入口移到右下角，且 `cgl_warp_t.s_amp_q8` 由 0 变为非零（横向 S 轨迹） |
| `SLIDE_*` | 两页同时平移（新页进入，旧页退出） |
| `COVER_*` | 只有新页移动，覆盖旧页（菜单/弹窗语义） |
| `FADE` | 前半段旧页淡出到背景色，后半段新页淡入；切换发生在"整屏恰好是背景色"的时刻 |
| `AUTO` | 交给导航器：先问本次"移动"的页面，页面未声明则回落到动作默认值 |

一个转场使用**一个** `cgl_anim_t`（0..256 的进度）驱动两个页面：滑动类把进度变成两页的 `offset`，因此两页不可能失步；`FADE` 使用一个常驻的遮罩控件（挂在宿主之下、始终最上层，非 FADE 转场时不参与绘制）。

### 13.5 谁决定这次转场：页面 → 调用者 → 类型表

三件事（**哪个转场**、**多快**、**哪条曲线**）按同一顺序回落：

| 决策 | 1) 页面声明 | 2) 调用者 | 3) 回落 |
|---|---|---|---|
| 哪个转场 | `page->enter.trans` / `leave.trans` | `cgl_page_nav_request()` 传入的 `CGL_TRANS_*` | 动作默认值：PUSH→`GENIE_IN`，POP/REPLACE→`GENIE_OUT`，RESET→`NONE` |
| 时长 | `page->enter.dur_ms` / `leave.dur_ms` | —— | `nav->duration_ms[最终类型]` |
| 曲线 | `page->enter.ease` / `leave.ease` | —— | `nav->ease[最终类型]` |

**页面声明优先级最高**（包括类型）：把转场写在页面上，调用点即使显式传了别的类型也必须服从页面。若希望调用者对某一次导航强制指定，就不要在该页面上声明该项。

**唯一的例外：调用者显式传入 `CGL_TRANS_NONE`。** `NONE` 不是一种"风格"（它不回答"哪个动画"）而是"不做动画"，因此"页面声明优先"的规则对它不适用——否则就**完全没有办法表达"这次要瞬间切换"**。快照测试需要在"无转场"下取稳定帧，应用初始化也不应在首屏布局前播放动画，这两件事都依赖它。若某个页面想给自己钉死瞬间切换，它自己声明 `trans = CGL_TRANS_NONE` 即可。

**哪个页面有发言权**（即本次"移动"的页面）：

| 动作 | 有发言权的页面 | 读取哪一项 |
|---|---|---|
| PUSH | **进入页**（新页） | `enter` |
| POP / REPLACE | **离开页**（被弹出/被替换的页） | `leave` |
| RESET | —— | 框架强制瞬间切换（回主界面是"重新开始"，不是转场） |

### 13.6 每类型的时长与曲线表

时长与曲线各有一张**按类型索引**的表（`nav->duration_ms[cgl_trans_t]`，每项 16 位；`nav->ease[cgl_trans_t]`，每项 8 位）。

| 类型 | 默认时长 | 默认曲线 |
|---|---|---|
| `NONE` | 0 | `LINEAR`（仅供观测；"全改"不会触碰它） |
| `GENIE_OUT` / `S_OUT` | `CGL_CFG_GENIE_MS`（默认 320 ms） | `CGL_CFG_GENIE_OUT_EASE`（默认 `IN_QUAD`） |
| `GENIE_IN` / `S_IN` | `CGL_CFG_GENIE_MS` | `CGL_CFG_GENIE_IN_EASE`（默认 `OUT_CUBIC`） |
| 其余 | `CGL_CFG_TRANSITION_MS`（默认 220 ms） | `CGL_CFG_TRANSITION_EASE`（默认 `OUT_CUBIC`） |

S 形与竖向吸入在**时间上完全相同**（默认表中时长与曲线同源）：两种吸入的手感差异全部在**几何**（吸入口 + S 路径），不在速度。若想让 S 形单独变慢，使用 `cgl_page_nav_set_transition_duration(nav, CGL_TRANS_S_OUT, 500)`——四种吸入各有自己的表项，互不影响。

运行时接口：

| API | 语义 |
|---|---|
| `cgl_page_nav_set_duration(n, ms)` | 改变**所有**类型的时长（不触碰 `NONE`）；0 = 全部瞬间切换 |
| `cgl_page_nav_set_transition_duration(n, trans, ms)` | 只改**一个**类型；传 `CGL_TRANS_AUTO` 等价于改全部；钳位到 65535 |
| `cgl_page_nav_duration(n, trans)` | 读取某个类型当前的时长（毫秒）。读的是**类型表**，不是"某页最终会用的值" |
| `cgl_page_nav_set_ease(n, ease)` | 改变**所有**类型的曲线（不触碰 `NONE`）；越界值钳位为 `LINEAR` |
| `cgl_page_nav_set_transition_ease(n, trans, ease)` | 只改**一个**类型的曲线；`CGL_TRANS_AUTO` 等价于改全部 |
| `cgl_page_nav_ease(n, trans)` | 读取某个类型当前的曲线 |

时长决定"需要多少帧"：提交请求的那一帧本身消耗一个 `dt`，因此帧数约为 `ms / dt` 向上取整（320 ms @ 17 ms/帧 ≈ 19 帧）。

这两张表是**回落层**：已经用 `cgl_page_set_trans_enter/leave()` 声明了时长的页面，其该类型转场不理会这里。因此"让所有页面变慢"与"只让某个页面变慢"是两个不同的旋钮。

### 13.7 页面注册

```c
typedef struct
{
    cgl_screen_t *screen;     /* 借用：帧时钟、帧率、显示设备          */
    cgl_page_nav_t *nav;      /* 借用：这些页面加入的导航器            */
    void *app;                /* 应用自己的上下文（页面互链表）；框架从不解引用 */
} cgl_page_ctx_t;

typedef struct
{
    const char *name;
    cgl_page_t *page;         /* 首成员为 cgl_page_t 的调用者存储，不得为 NULL */
    void (*attach)(cgl_page_t *page, const cgl_page_ctx_t *ctx);   /* 可以为 NULL */
} cgl_page_desc_t;

uint8_t cgl_page_register(const cgl_page_desc_t *pages, uint8_t count,
                          const cgl_page_ctx_t *ctx);
const cgl_page_ctx_t *cgl_page_ctx(const cgl_page_t *p);
void cgl_page_set_root(cgl_page_t *p, cgl_widget_t *root);
void cgl_page_init(cgl_page_t *p, const cgl_page_vt_t *vt, const char *name, void *user);
```

**"增加一个页面"就是三处改动**：在自己的文件里写一个标准形态的 `void app_page_xxx_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx)`；在应用的页面存储结构里加一个对象成员；在页面表里加一行。

```c
static const cgl_page_desc_t s_pages[] = {
    { "menu",  (cgl_page_t *)&s_app.menu,  app_page_menu_attach  },
    { "info",  (cgl_page_t *)&s_app.info,  app_page_info_attach  },
    { "watch", (cgl_page_t *)&s_app.watch, app_page_watch_attach },
};

(void)cgl_page_register(s_pages, (uint8_t)(sizeof(s_pages) / sizeof(s_pages[0])), &s_ctx);
```

说明：

- `cgl_page_register()` 按顺序遍历，对每一项调用 `attach`；`page` 或 `attach` 为 `NULL` 的行被跳过而不是解引用——"某个构建配置不含这个页面"因此是合法状态。
- 这里只**创建**页面，不压栈。首页由 `cgl_page_nav_reset()`（或 `push()`）安装，那一步才会运行第一次 `on_create`。
- `cgl_page_init(page, &vt, name, (void *)ctx)` 把上下文存进 `cgl_page_t::user`，之后任何回调（包括 `on_create`）都能用 `cgl_page_ctx(p)` 读回它。
- 上下文中的一切都**是借用的**：`screen` / `nav` 由移植层或测试宿主拥有，`app` 指向应用自己的存储。上下文必须比页面活得久——给它静态存储。
- 跨页引用**不通过额外参数传递**：需要兄弟页面的页面从 `ctx->app`（应用自己的链接结构）读取。这样"本构建没有该页面"就是一个合法状态（`links->foo == NULL`，菜单项显示但什么也不做），而不是再多一种构造函数变体。

### 13.8 导航 API 与输入锁

| API | 语义 |
|---|---|
| `void cgl_page_nav_init(n, name, area)` | 初始化。`area` 通常是整屏，作为平移/变形与裁剪的基础 |
| `cgl_widget_t *cgl_page_nav_host(n)` | 页面宿主控件（把它挂到场景根上） |
| `bool cgl_page_nav_request(n, kind, page, trans, arg)` | **只登记请求**，不改变控件树。同一帧内多次调用只有**最后一次**生效；非法请求返回 false 并计入 `req_dropped`；传入具体 `CGL_TRANS_*` 时，只在该页未声明类型时才生效 |
| `cgl_page_nav_push(n, page)` / `_replace` / `_reset` / `_back` | 便捷包装。`_back` 在栈中只有一页时返回 false |
| `void cgl_page_nav_frame(n, clk)` | 帧边界：若上一个转场仍在运行而新请求到来，先**立即跑完**它（`finish` 负责隐藏/销毁/清零 offset 与 warp），然后提交请求、推进转场，并把整屏标记为损坏。**这是导航器修改控件树的唯一入口** |
| `bool cgl_page_nav_event(n, ev)` | 把事件交给栈顶页面（先 `page->on_event`，未消费则走控件树分发）。转场期间返回 false 且不做任何分发——**输入锁**。`CGL_EV_BACK` 在深度 > 1 时由**导航器**直接弹栈 |
| `cgl_page_nav_top(n)` / `depth(n)` / `busy(n)` / `pending(n)` / `transitions(n)` / `dropped(n)` / `warp(n)` | 观测与转发 |
| `cgl_page_nav_set_genie_target(n, x, y)` | 设置吸入口位置，默认是 `area` 底边中点再下移 `CGL_CFG_GENIE_DROP_PX`（屏幕外的"Dock"）。`x` 是吸入口的**中心列**（不是左上角），因此"吸到右下角"应传 `area.x1`。只影响 `GENIE_OUT/IN` |

关于输入锁的设计：

- 转场期间 `screen__drain_input()`**不消费队列头部**，而是等转场结束后再投递。若在这里弹出并丢弃（"分发返回 false 就算了"），用户在 200 ms 转场期间按下的键就会真的丢失。
- 多次请求不丢：同一帧内的多次导航请求只有最后一次生效，快速连按不会丢失，也不会堆栈溢出。
- 一个按键转换为**两个**事件的常见做法（参考示例）：长按先产生 `CGL_EV_KEY_LONG`，再产生 `CGL_EV_BACK`。菜单页的 `on_event` 消费 `KEY_LONG`（解读为"进入选中项"）并顺带消费随后的 `BACK`（栈底无处可去）；不消费 `KEY_LONG` 的页面（`on_event = NULL`）则让随后的 `BACK` 到达导航器并弹栈。于是"长按返回"只写在需要它的地方，每个页面无需各自实现。

### 13.9 半透明覆盖层

默认情况下，PUSH 转场结束时 `nav__finish()` 会对离开的页面调用 `cgl_widget_set_visible(false)`——而"隐藏"意味着整棵子树**不绘制一个像素**。对不透明页面这是免费省下一次绘制；但对半透明覆盖层，下面只剩宿主背景色，`cgl_widget_set_alpha()` 透出的是背景而不是那一页的内容。因此覆盖层必须**同时做两件事**：

```c
/* 在覆盖层的 on_create / *_init 中 */
cgl_page_set_show_below(page, true);           /* 1) 让下面的页面继续绘制 */
cgl_widget_set_alpha(&p->root.base, 208u);     /* 2) 整页按 alpha 合成     */
```

```c
void cgl_page_set_show_below(cgl_page_t *p, bool show);
bool cgl_page_get_show_below(const cgl_page_t *p);
```

注意它**只管是否绘制**：

- 下面的页面**仍然收不到 `on_step`**，物理/逻辑照旧冻结。这是页面模型的核心不变量，不要为了让某个效果成立而改动它。
- 代价由下面页面的内容复杂度决定：覆盖**静态**页面几乎免费（无损坏就没有重绘），覆盖**动画**页面则昂贵。
- 链式关系同样成立：若 A 半透明覆盖 B、C 半透明覆盖 A，则只有 A 也声明之后 B 才会继续被绘制。

### 13.10 Genie 与 S 形吸入的实现要点

真实的 macOS Genie 是网格变形加像素重采样。这里没有全屏缓冲，因此走了等价路线：让每个图元逐行重算自己的落点，画完之后看起来就是被吸进去了。得到的性质：

- 实心矩形照旧填充（每源行算出"被压到哪一行、宽度多少"，然后填一段游程）；
- 文本、点阵与圆盘逐行收窄（字体的列映射同样经过 warp）；
- 代价是每行多两次乘加——实测并不比滑动转场重多少，因为整条带只绘制一次。

形状由三个因子相乘得到（源码中的 `warp__k()`）：

| 因子 | 公式 | 观感 |
|---|---|---|
| `prof` | 二次曲线，远端 1.0 → 靠吸入口一端 `CGL_CFG_GENIE_NARROW_Q8` | 侧面内凹的喇叭口；直线会变成三角锥，不像精灵 |
| `depth` | `1-(1-p)²` | 漏斗深度：早期就开始自下收窄，而远端仍然很宽 |
| `shrink` | `p²` | 整体收窄：稍后整体变细，最终收成一条竖线 |

竖直方向是**向吸入口的比例收缩**（`y' = ty + (y-ty)(1-p)`）：离吸入口越远的行移动越多，因此看起来像"自上而下倒入"，而不是整页等比缩小。

谁被吸、谁在下面露出、层级如何安排，全部由导航器决定（`nav__warp_setup()`）：`GENIE_OUT` 吸走离开的页面并把它抬到最上层，`GENIE_IN` 吸进入的页面。转场结束时 `nav__finish()` 移除 warp、清零 offset 并恢复层级。

S 形（`S_OUT` / `S_IN`）**完全复用**上面的漏斗，只在 `nav__warp_setup()` 中增加两点：吸入口移到右下角，且列映射多了一个横向项

```
x' = cx'(p) + (x - cx)*k(y)/256 + S(y)
S(y) = 3*amp*phase*g(t)/65536,   g(t) = 256*u²(1-u)²,   u = t/256
```

`g` 的形状是关键：**两端与中轴相切（`g'(0) = g'(1) = 0`）**，最大偏移在中点，因此页面向右弓出、在下半段收回来钻进角落，形成 S 路径；`phase`（OUT 取 p，IN 取 1-p）进入 S 的乘子，因此 `p = 0` 时的恒等性质完好（首帧无抖动），两个方向互为精确逆过程。

振幅上限不是调参口味而是**单调性约束**：`|S'| <= 3*(amp/256)*(6/256)`，因此只有 `s_amp_q8 <= 682` 才能保证相邻源行的落点不交叉。`nav__s_amp()` 由"半屏宽"推导振幅，再钳到 `CGL_CFG_S_AMP_MAX_Q8`（默认 160，正是 320 像素面板的推导值）。

需要留意：该上界保证的是"横向偏移本身不会越过邻行"，而**不**保证取整输出的逐行严格单调——列项四舍五入而 S 项截断，因此即使振幅很小，渲染结果在相邻行之间也会出现 1..2 像素的起伏。那是取整而非交叉；真正的折叠会表现为与页面宽度同量级的台阶。另外，S 形要求 `map_rect()` **更密地采样**：极值不在矩形角上（横向偏移在页面内部最大），只取四角会把最鼓的一段剔除掉。

改变吸入形状的旋钮是 `CGL_CFG_GENIE_NARROW_Q8`（窄端宽度）与 `warp__k()` 中的三个因子；改变吸入口用 `cgl_page_nav_set_genie_target()`。

### 13.11 页面栈与观测

| 观测项 | 含义 |
|---|---|
| `cgl_page_nav_depth(n)` | 当前栈深度（1 = 只有主界面） |
| `cgl_page_nav_busy(n)` | 转场进行中（输入被锁，外部不得改树） |
| `cgl_page_nav_pending(n)` | 有尚未提交的导航请求 |
| `cgl_page_nav_transitions(n)` | 已完成的转场次数（一次按键应当 +1） |
| `cgl_page_nav_dropped(n)` | 因目标非法而被拒绝的请求数（非零 = 调用者缺陷） |
| `cgl_page_nav_warp(n)` | 正在播放的变形描述符（无则 `NULL`）；把它交给 `cgl_canvas_map_y()` 即可自行算出"这一帧的页面被吸成了什么样" |
| `cgl_page_is_created(p)` / `cgl_page_root(p)` | 页面是否已创建 / 其控件树根 |

判定"页面切换是否真的生效"的三种方法：

1. 按一次键后 `nav.transitions` 加一，`nav.depth` 在 1/2/3 之间变化；
2. 在被压住的页面上，该页收到的步数（例如 `s_app.smash.steps`）完全不动，而全局 `clock.step_total` 持续增长；
3. 返回后该页步数恢复，且**不会**一次跳几百步（跳变意味着发生了追补）。

---

## 14. 场景

`cgl_screen_t` 把显示设备、时钟、导航器与输入环形队列组合在一起：

```c
typedef struct cgl_screen
{
    cgl_display_t *display;  /* 不拥有 */
    cgl_page_nav_t *nav;     /* 不拥有（由应用层持有） */
    cgl_clock_t clock;       /* 全项目唯一的时钟 */

    cgl_event_t queue[CGL_CFG_INPUT_QUEUE];
    uint8_t q_head, q_tail;
    uint32_t q_dropped;      /* 因队列满而丢弃的事件数（供观测） */
} cgl_screen_t;
```

| API | 语义 |
|---|---|
| `int cgl_screen_init(s, display, nav)` | 绑定显示与导航器、安装损坏汇、用 `CGL_CFG_CLOCK_STEP_MS` 装载时钟。它校验面板几何与 `CGL_CFG_*` 一致，且导航器区域覆盖整屏。参数为 `NULL` 或不一致时返回 -1 |
| `void cgl_screen_frame(s, dt_ms)` | **完整的一帧**（第 3.2 节的七个步骤） |
| `bool cgl_screen_post_event(s, ev)` | 只入队；队列满时返回 false 并递增 `q_dropped` |
| `cgl_screen_clock(s)` / `_fps(s)` / `_top(s)` / `_nav(s)` / `_root(s)` / `_redraw_all(s)` / `_dropped_events(s)` | 只读转发：时钟 / 帧率 / 栈顶页面 / 导航器 / 宿主控件 / 强制全屏重绘 / 丢弃事件数 |

**同一时刻只能有一个活动的 `cgl_screen`**：损坏汇是全局单例（见 3.5）。这是单屏设备的充分条件。

`cgl_screen_root()` 返回导航器宿主控件，覆盖整屏。两种使用方式：

- **直接挂控件**：把控件挂在宿主上，得到一个没有页面模型的界面（快速原型、单屏仪表）。
- **使用页面模型**：控件挂在页面根控件上，由 `cgl_page_*` 管理（推荐）。

---

## 15. 可选扩展模块 `cytgl_ext`

### 15.1 模块构成

该模块位于与框架**并列**的两棵树中：`include/ext/` 是它的公共 API，`src/ext/` 是它的实现。它刻意不属于框架——`src/core`、`src/render`、`src/port` 从不包含它，`libcytgl.a` 也不含它。不链接它的构建在 Flash 与 RAM 上均为零开销，这一点由**选择性链接**保证，而不是靠核心里的 `#ifdef`。

```
cgl_iso2d            位置 + 投影 + 深度排序；不认识刚体
     |
cgl_iso2d_phys2d.h   桥：唯一同时包含两层（全部为内联）
     |
cgl_phys2d           刚体、粗筛网格、求解器、竖直轴
cgl_ext_basis.h      两者共用的 Q12.4 约定与强制内联宏
```

依赖是**单向**的：`cgl_iso2d.h` 里没有 `cgl_phys2d` 的任何类型、标志或宏，它排序的是**位置**（通过 `cgl_iso2d_src_t` 中的一个访问器）。桥是两层唯一汇合的地方，因此也是唯一允许同时包含两者的文件。

模块包含三个效果，每个都按同样方式上下分离（引擎在下，绘制器在上）：

| 效果 | 引擎（框架无关） | 绘制器（认识画布） |
|---|---|---|
| 2.5D 刚体物理与投影 | `cgl_phys2d.[ch]`、`cgl_iso2d.[ch]`、`cgl_iso2d_phys2d.h`、`cgl_ext_basis.h`、`cgl_*_config.h` | 由使用者自行绘制 |
| 粒子云（子弹时间） | `cgl_particles.[ch]`、`cgl_particles_tables.[ch]`、`cgl_particles_config.h` | `cgl_particles_view.[ch]` |
| 碎裂场景 | `cgl_debris.[ch]`、`cgl_debris_config.h` | `cgl_debris_view.[ch]` |
| 重力球 | `cgl_bounce.[ch]`、`cgl_bounce_config.h` | `cgl_bounce_view.[ch]`（含独立小球控件） |

**引擎**只包含 `<stdint.h>` / `<stdbool.h>` / `<stddef.h>`；**绘制器**是唯一包含 `<cytgl/cgl_canvas.h>` 的文件。这条单向规则也写在构建脚本里：绘制器源文件只在框架目标存在于构建图中时才被加入。

模块的公共头文件以 `<ext/...>` 引入，与 `<cytgl/...>` 同属 `include/` 这一个包含根，因此一个 `-I include` 同时服务两者。

### 15.2 定点约定

| | 物理与投影层 | 粒子引擎 |
|---|---|---|
| 格式 | Q12.4（`CGL_EXT_Q4_SHIFT` = 4，`CGL_EXT_Q4_ONE` = 16） | Q8.8（`CGL_PARTICLES_SHIFT` = 8，`CGL_PARTICLES_ONE` = 256） |
| 分辨率 | 1/16 像素 | 1/256 像素 |
| 量程（`int16_t`） | 约 ±2047 像素 | 约 ±127.99 像素（由"世界半跨 ±120 像素"的断言把守） |

两者的取舍相反：Q12.4 量程大但分辨率低，Q8.8 分辨率高但量程小。碰撞世界大而 1/16 像素足够；粒子场景整体只有约 232 像素宽、相机运动是亚像素级的，因此**分辨率重要而量程不重要**。两种约定**从不相遇**：粒子引擎不读任何 `cgl_phys2d` 类型，也不写任何 `cgl_phys2d` 字段。

相机矩阵的系数在物理层是 **Q8**，粒子层是 Q8.8；`cgl_phys2d_to_px()` 是**截断**的 `>> 4`。粒子引擎中所有涉及移位的定点乘法都写成除以 2 的幂（`CGL_PARTICLES_MUL(a,b)` 即 `(a*b)/256`），使取整方向是"向零截断"而不是"向 −∞"——若用 `>>`，负速度的衰减会比正速度略快，爆炸云会可见地弯曲。

### 15.3 `cgl_phys2d`：定点圆盘刚体世界

模型：圆形形状；**转动惯量无穷大（刚体永不旋转）**；点阵与一颗无穷重的"重球"质量相同；**平面内零重力、零摩擦、零恢复系数**；纯平移圆盘动力学 = 均匀网格粗筛 + 圆-圆窄相 + 非弹性冲量 + 无穷重运动学刚体。全部 Q12.4 整数运算。

**2.5D 竖直轴（z）**：每个刚体带 `z` 与 `vz`，受重力积分并在 `z = 0` 落地，但**从不参与碰撞**——求解器始终是 XY 平面内的纯圆盘求解器。原因有二：隐喻的正确性（XY 是地面，因此从上方飞过的碎块仍会撞到它经过的东西）与成本（粗筛、冲量求解与调参都保持不变且可测试）。竖直轴由 `cgl_iso2d` 投影到屏幕 y。碎块在地面上弹跳的效果来自 `cgl_phys2d_step()` 末尾的起飞规则：平面速度超过阈值即离地，落回、回弹，只要仍然够快就再次起飞。

**类型**

```c
#define CGL_PHYS2D_FLAG_KINEMATIC 0x0001u  /* 不受冲量（无穷质量），但仍会移动   */
#define CGL_PHYS2D_FLAG_ACTIVE    0x0002u  /* 参与仿真                        */
#define CGL_PHYS2D_FLAG_SPHERE    0x0004u  /* 渲染为受光球体加地面阴影，而非平面点 */

typedef struct {
    int16_t x, y;      /* 位置，Q12.4                    */
    int16_t vx, vy;    /* 速度，Q12.4/步                  */
    int16_t r;         /* 碰撞半径，Q12.4                 */
    int16_t z;         /* 离地高度，Q12.4（2.5D，不小于 0） */
    int16_t vz;        /* 竖直速度，Q12.4/步               */
    uint16_t flags;
} cgl_phys2d_body_t;   /* 16 字节 */
```

```c
typedef struct {
#if CGL_PHYS2D_USER_STORAGE
    cgl_phys2d_body_t *body;      /* 调用者存储，capacity 项              */
    int16_t *next;                /* 调用者存储，capacity 项              */
    int16_t *cell_head;           /* 调用者存储，CGL_PHYS2D_GRID_CELLS 项 */
    uint16_t capacity;            /* 池容量，由调用者决定                 */
#else
    cgl_phys2d_body_t body[CGL_PHYS2D_MAX_BODIES];
    int16_t next[CGL_PHYS2D_MAX_BODIES];
    int16_t cell_head[CGL_PHYS2D_GRID_CELLS];
#endif
    uint16_t count;           /* 已加入的刚体数（0..容量）      */
    uint16_t active;          /* 仍在仿真域内的刚体数           */
    uint16_t active_dynamic;  /* 其中非运动学的刚体数           */
    uint32_t step;            /* 已仿真的步数                  */
} cgl_phys2d_t;
```

数组是**公开成员**（不是不透明类型），示例与测试直接索引 `world.body[i]`。

```c
typedef struct {
    cgl_phys2d_body_t *body;   /* capacity 项       */
    int16_t *next;             /* capacity 项       */
    int16_t *cell_head;        /* GRID_CELLS 项     */
    uint16_t capacity;         /* 1..32767         */
} cgl_phys2d_storage_t;                                 /* 仅 CGL_PHYS2D_USER_STORAGE=1 */

#define CGL_PHYS2D_STORAGE_BYTES(cap) /* 池 + next + cell_head 的字节数 */
```

| API | 语义 |
|---|---|
| `uint16_t cgl_phys2d_capacity(const cgl_phys2d_t *w)` | 池容量。运行时模式下返回 `capacity`，否则返回编译期的 `CGL_PHYS2D_MAX_BODIES`。它是函数而不是成员：在嵌入式形态下成员会变成一个常驻 RAM 的常量，而 `sizeof(cgl_phys2d_t)` 是模块公布的资源成本的一部分 |
| `void cgl_phys2d_storage(cgl_phys2d_storage_t *st, body, next, cell_head, cap)` | 【仅用户存储模式】把调用者声明的存储打包成描述符 |
| `bool cgl_phys2d_init_storage(cgl_phys2d_t *w, const cgl_phys2d_storage_t *st)` | 【仅用户存储模式】该模式下获得可用世界的唯一途径。空数组、零容量或超出 `int16_t` 索引范围时返回 false，并把世界留在**不可用**状态（随后的 `step()` 什么也不做） |
| `void cgl_phys2d_reset(cgl_phys2d_t *w)` | 清空世界（保留存储）。**只把 `count` 归零**：不释放任何东西，也不通知任何人"你的索引现在属于别人了"，同时把 `step` 归零 |
| `int cgl_phys2d_add(cgl_phys2d_t *w, x, y, vx, vy, r, flags)` | 加入一个刚体，返回其索引；池满时返回 **-1** |
| `void cgl_phys2d_step(cgl_phys2d_t *w)` | 积分一步并求解全部接触。它**恰好是一步**；何时发生完全由调用者决定。`step` 每步自增 |
| `int16_t cgl_phys2d_to_px(int16_t q)` | 定点转像素（**截断**） |

编译期一致性闸门（都在 `cgl_phys2d.h` 末尾，用 `_Static_assert`）：网格单元 1..256 像素、池容量 1..32767、世界边界不超 32767、网格 W/H 为正，以及**参考几何的锚点**——`CGL_PHYS2D_CELL_Q4 == 8 * ONE`、`WORLD_W == 400 * ONE`、`WORLD_H == 256 * ONE`、`GRID_W == 50`、`GRID_H == 32`。理由是物理调参（5 邻域粗筛扫描、`cgl_phys2d__resolve()` 中"只有邻居会被提交"这一假设）是在 400×256 世界、50×32 网格上测出来的；一个悄悄改掉网格的宏**不会响亮地失败**，而是让碰撞停止发生，唯一症状是"球直接穿过点阵"。有意改世界尺寸时，必须同时重新推导这些闸门。

### 15.4 `cgl_iso2d`：投影与深度排序

投影（世界 Q12.4 → 屏幕像素，全整数）：

```
ground_x = ((m00 * x + m01 * y) >> 12) + ox
ground_y = ((m10 * x + m11 * y) >> 12) + oy      <- 同时也是深度键
screen_y = ground_y - ((kz * z) >> 12)
```

**深度键为什么不是 y**：等轴测下"按 y 排序"是错的，相机深度是投影后的地面 y，即 `m10*x + m11*y`。参考点阵是一长条（106 × 14 格）：x 相差 300 像素而 y 只差几十像素，只按 y 排序会让同一行内相距 300 像素的两个点看起来等距，遮挡关系变得随机。统一取 `ground_y` 作为键，既在等轴测风格下自动是两个世界轴的加权和，又在平面风格下（`m10 = 0`）退化为 `y`，因此一段代码对两种相机都正确。

**为什么用计数排序**：深度键是有界的屏幕坐标（0..171），桶排序是 O(n + 256)，完全确定——无比较、无递归、无 2 KB 的比较排序暂存、无最坏情况。结果按**远 → 近**排列（桶 0 = 屏幕顶端 = 最远），直接顺序绘制：不需要 z 缓冲（条带渲染本来也无处安放它）。

**排序每个物理步只做一次，不是每条带一次**：渲染器每 4 像素条带被调用一次（320×172 屏为 43 条带），按条带排序等于浪费 43 倍工作量。排序结果携带**戳记**（`s->stamp == src->stamp` 时立即返回），因此谁触发这一步无关紧要。戳记具体是什么由调用者决定：对物理世界是 `world->step`，对精灵列表就是调用者所谓的"一次更新"。

**类型**

```c
typedef struct {
    int16_t m00, m01;  /* 屏幕 x 系数，Q8（与世界 Q12.4 坐标 >>12 相乘得到像素） */
    int16_t m10, m11;  /* 深度（= 地面屏幕 y）系数，Q8，同时是排序键          */
    int16_t kz;        /* 高度抬升系数，Q8                              */
    int16_t ox, oy;    /* 屏幕偏移，像素                                */
} cgl_iso2d_cam_t;

typedef enum {
    CGL_ISO2D_STYLE_ISO = 0,          /* 斜视地面；两个世界轴的竖向压缩分别求解，目标是让点阵
                                         投影后的最小间距最大（"最透气的点阵"）。工程默认 */
    CGL_ISO2D_STYLE_ISO_UNIFORM = 1,  /* 教科书 30 度：m10 == m11。方世界下的最优解
                                         （等边三角点阵）；世界越狭长，它压得越扁 */
    CGL_ISO2D_STYLE_FLAT = 2          /* 单位矩阵：XY 一比一铺在屏幕上（旧的平面外观，无伪 3D） */
} cgl_iso2d_style_t;

typedef struct { int16_t x, y, z; } cgl_iso2d_pos_t;   /* 世界 Q12.4；z 不小于 0 */

typedef struct {
    bool (*read)(const void *ctx, uint16_t i, cgl_iso2d_pos_t *out);  /* false = 第 i 个元素不在场景中 */
    const void *ctx;      /* 原样回传给 read：调用者自己的状态（例如物理世界） */
    uint16_t count;       /* 提交给 read 的元素数：0..count-1 */
    uint32_t stamp;       /* 当前位置集合的身份标识 */
} cgl_iso2d_src_t;

typedef struct {
    int16_t order[CGL_ISO2D_MAX_POINTS];      /* 元素索引，远 -> 近，直接顺序绘制 */
    uint16_t bucket[CGL_ISO2D_DEPTH_BUCKETS];
    uint16_t count;   /* order 中有效元素的个数 */
    uint32_t stamp;   /* 上次排序对应的 src->stamp */
    bool valid;       /* 为 false 时下一次排序保证重排 */
} cgl_iso2d_sorter_t;

typedef struct { cgl_iso2d_cam_t cam; cgl_iso2d_sorter_t sorter; } cgl_iso2d_view_t;
```

`read` 的契约：必须从元素 `i` 填充 `*out` 并返回 `true`，或在该元素不属于场景时返回 `false` 以跳过。只在 `i ∈ [0, count-1]` 上、按递增顺序、**每次排序两遍**（一遍统计每桶数量，一遍放置）被调用，且"两遍必须给出相同答案"。

| API | 语义 |
|---|---|
| `void cgl_iso2d_cam_fit(cam, style, wx0, wy0, ww, wh, vx, vy, vw, vh)` | 让世界矩形恰好落进视口矩形（两者都居中）并推导投影系数。**保证**：返回后世界矩形四角的投影一定落在 `[vx, vx+vw-1] × [vy, vy+vh-1]` 内——函数内部会测量自己的结果并持续收缩，因为定点取整会把"计算上刚好装下"变成"漏出一两个像素"。唯一无法修复的情况是视口比单位尺度下的世界还小：此时它停止收缩并**返回它能给出的最小相机**，而不是失败 |
| `void cgl_iso2d_cam_set(cam, m00, m01, m10, m11, kz, ox, oy)` | 低层入口：直接交出矩阵，便于在真实硬件上试角度 |
| `void cgl_iso2d_project_rect(cam, wx0, wy0, ww, wh, *x0, *y0, *x1, *y1)` | 世界矩形（z = 0）投影后的屏幕包围盒。四角都要投影再取最小/最大：该投影下矩形变成平行四边形，不能只看对角线 |
| `int32_t cgl_iso2d_ground_x(cam, wx, wy)` / `ground_y(...)` | 地面投影（**不含** `ox`/`oy`）。`ground_y` 就是深度键：越大越靠近观察者 |
| `void cgl_iso2d_project(cam, wx, wy, wz, *sx, *sy)` | 世界 Q12.4 → 屏幕像素，内部会加上 `cam.ox`/`cam.oy` |
| `void cgl_iso2d_sorter_reset(s)` | 强制下一次 `sort()` 重排。**世界重建或相机改变之后必须调用** |
| `void cgl_iso2d_sort(s, cam, src)` | 按深度把 `src` 排成绘制顺序（远 → 近）。同一 `src->stamp` 再次调用是**空操作**，因此一帧的每条带都可以安全调用。超过 `CGL_ISO2D_MAX_POINTS` 的元素被**忽略**（表装不下） |
| `void cgl_iso2d_view_init(v)` | 初始化相机 + 排序器这对组合：每个场景拥有一次，可被多个控件共享 |

**桥 `cgl_iso2d_phys2d.h`**（全部内联）：

```c
_Static_assert(CGL_PHYS2D_MAX_BODIES <= CGL_ISO2D_MAX_POINTS,
   "the cgl_phys2d pool is larger than the cgl_iso2d sort table: raise CGL_ISO2D_MAX_POINTS "
   "(cgl_iso2d_config.h) or lower CGL_PHYS2D_MAX_BODIES (cgl_phys2d_config.h)");
/* 诊断文本为源码原文（英文）；含义：cgl_phys2d 的池比 cgl_iso2d 的排序表大 */

bool cgl_iso2d_phys2d_read(const void *ctx, uint16_t i, cgl_iso2d_pos_t *out);
void cgl_iso2d_phys2d_src(cgl_iso2d_src_t *src, const cgl_phys2d_t *w);
void cgl_iso2d_sort_bodies(cgl_iso2d_sorter_t *s, const cgl_iso2d_cam_t *cam,
                           const cgl_phys2d_t *w);
```

`read` 中：元素 `i` 就是刚体 `i`；带 `CGL_PHYS2D_FLAG_ACTIVE` 时参与；`i >= w->count` 或刚体不活跃时返回 false。`src` 的 `stamp = w->step`——物理步数**就是**位置集合的身份。`sort_bodies()` 在栈上构造 `src` 并调用 `cgl_iso2d_sort()`。

### 15.5 `cgl_particles`：Q8.8 粒子云与环绕相机

三条承诺：**无浮点**（所有时变量都是 Q8.8 或小整数，目标文件中没有 `sinf`/`cosf`/`sqrtf` 调用）；**热路径无除法、无 libgcc 辅助函数**（透视除法是一张倒数表加一次线性插值，三角是 64 项 0..90° 正弦表经象限折叠，128 字节 Flash 覆盖整圈）；**每个粒子 12 字节**。

为什么不扩展 `cgl_iso2d`：它是矩阵一次拟合、此后永不改变的固定正交投影，且它的逆（屏幕 → 世界）不存在，因此"绕场景走动相机"无法表达；把它改成通用 3D 相机会让整个应用付出 Flash 与 RAM 预算，并破坏模块已公布的契约。

**定点工具**

```c
#define CGL_PARTICLES_SHIFT 8
#define CGL_PARTICLES_ONE   256
#define CGL_PARTICLES_PX(v) ((int32_t)(v) * CGL_PARTICLES_ONE)      /* 仅用于编译期常量 */
#define CGL_PARTICLES_MUL(a, b) ((int32_t)(((int32_t)(a) * (int32_t)(b)) / CGL_PARTICLES_ONE))
#define CGL_PARTICLES_ANGLE_FULL 65536u

int32_t cgl_particles_cell_x(int i);
int32_t cgl_particles_cell_y(int j);
int16_t cgl_particles_clamp_i16(int32_t v);
uint8_t cgl_particles_clamp_u8(int32_t v);
int16_t cgl_particles_sin_q8(uint8_t a);      /* Q8.8，-256..256；a 是 16 位角度的最高字节 */
int16_t cgl_particles_cos_q8(uint8_t a);      /* = sin_q8(a + 0x40) */
int32_t cgl_particles_recip_q16(int32_t d8);  /* 1/深度，Q16.16；查表 + 一次线性插值 */

extern const uint8_t  cgl_particles__sin_q8[64];
extern const uint16_t cgl_particles__recip[385];
```

角度约定：一整圈 = 65536（`uint16_t`），64 单位 = 90°。正弦表把 `sin(90°)` 钳到 **255** 而不是 256——256 装不进 `uint8_t`；若不加钳位会被写成 `0x100` 并在数组里截断为 0，使 `sin(90°)` 变成**零**（奇异相机矩阵，而不是取整误差）。倒数表按深度自身索引（`cgl_particles__recip[d] = round(65536/d)`），前 128 项（深度 0..127）从不读取，存在的目的是让"深度 → 索引"成为**恒等映射**，从而在内层循环里省掉一次减法或移位：它花 256 字节 Flash，换取每个粒子每条带少一条指令。

**世界与相机常量**

| 宏 | 默认 | 含义 |
|---|---|---|
| `CGL_PARTICLES_GRID_W` / `_GRID_H` | 106 / 14 | 参考点阵的格数（调用者据此写单元格坐标回调） |
| `CGL_PARTICLES_CELL_PITCH_Q8` | 560 | 格距 = 2.1875 像素。参考徽标 106 格 × 3 像素 = 318 像素，装不进 Q8.8 的 ±120 像素窗口，因此整个场景缩放到 240/318 = 0.755，2.1875 是仍能把整格留在窗口内的最近格距 |
| `CGL_PARTICLES_WORLD_HALF_SPAN_Q8` | `PX(120)` | 世界半跨，安全闸门（不是相机参数）：Q8.8 在 `int16_t` 中只能表达 ±127.99 像素 |
| `CGL_PARTICLES_NEAR_Q8` / `_FAR_Q8` | 128 / 383 | 近、远裁剪面；`recip_q16()` 中的钳位**就是**近远平面 |
| `CGL_PARTICLES_BUCKETS` | 128 | 计数排序桶数：128..383 像素的深度窗口被分成 2 像素分辨率，比粒子画出的 3 像素点更细，因此量化不可能可见地重排两个粒子 |
| `CGL_PARTICLES_MAX` | 500 | 池中粒子数，每个 12 字节——**决定 RAM 的大头**（500 个 = 6.0 KB），同时决定爆炸能喷出多少碎屑 |
| `CGL_PARTICLES_CAM_PITCH_A` | 21 | 固定俯仰角，21/256 圈 = 29.5° |
| `CGL_PARTICLES_CAM_DIST_PX` / `_FOCAL_PX` | 210 / 235 | 相机距离与焦距，两者共同决定投影尺度，并由两条 `_Static_assert` 约束在"既不出视口、也不是中间一小团"的区间内 |
| `CGL_PARTICLES_CAM_DOLLY_Q8` | 40 | 推拉振幅 |
| `CGL_PARTICLES_TRIGGER_TICKS` | 78 | 触发时刻，单位是**逻辑步**（不是毫秒；引擎没有时间概念，它只数步数并通过 `cgl_particles_tick()` 报告） |
| `CGL_PARTICLES_DOT_SPAN_Q8` / `_DOT_BASE_Q8` | 384 / 256 | 点的直径随透视尺度从 1.0 像素长到 2.5 像素，并钳到 1..3 像素 |

**类型**

```c
typedef struct {
    int16_t x, y, z;    /* 位置，Q8.8，世界空间（原点 = 徽标中心） */
    int8_t  vx, vy, vz; /* 速度，Q8.8/逻辑步                     */
    uint8_t life;       /* 剩余寿命（步）；0 = 空槽               */
    uint8_t bucket;     /* 本帧深度桶，0 = 离相机最远              */
    uint8_t hit;        /* 0 = 停在原格上，1 = 已释放              */
} cgl_particle_t;       /* 恰好 12 字节（有 _Static_assert 把守） */
```

速度用 `int8_t` 而非 `int16_t`：位置必须站在世界中心 ±116 像素处，因此需要 int16；速度不需要，因为爆发中的最大速度远低于 0.5 像素/步。把三个速度降到 8 位使整个云从 7.5 KB 降到 6.0 KB——省下页面 RAM 的四分之一。`hit` 并不浪费：对齐是 2（因为 int16 成员），11 字节本来也会被填充到 12。**停放的粒子（`hit = 0`）被 `step()` 完全跳过**——不受重力、不消耗寿命——这正是未触及的那部分点阵能精确留在原地的原因。**字段顺序是承重的**：位置在前、三个速度字节其次、标量最后，编译器没有机会插入空洞。

```c
typedef struct {
    int16_t m00, m01, m02;   /* 相机 x = (m00*x + m01*y + m02*z) / 256 */
    int16_t m10, m11, m12;   /* 相机 y */
    int16_t m20, m21, m22;   /* 相机 z——**排序键** */
    int32_t dist_q8;         /* 沿视轴的相机距离，Q8.8 */
    int32_t orbit_q8;        /* 推拉的额外深度偏移 */
    int32_t focal_q8;        /* 焦距 */
    int32_t ox_q8, oy_q8;    /* 投影原点（屏幕中心），Q8.8 */
} cgl_particles_cam_t;

typedef struct {
    int32_t origin_x_q8, origin_y_q8;   /* 徽标格中心的 Q8.8 坐标 */
    int16_t pitch_q8;                   /* 格距 */
    uint16_t max_active;
} cgl_particles_world_t;

typedef struct {
    cgl_particle_t particle[CGL_PARTICLES_MAX];
    uint16_t order[CGL_PARTICLES_MAX];       /* 排序后的索引，远 -> 近 */
    uint16_t bucket[CGL_PARTICLES_BUCKETS];  /* 桶计数，随后复用为前缀和 */
    uint16_t active, max_active;
    uint32_t seed;      /* xorshift 状态：不使用 libc rand，确定性 */
    uint8_t tick;       /* 本轮已仿真的逻辑步数 */
    bool sorted;
} cgl_particles_t;

typedef union {
    cgl_particle_t align_particle;
    uint16_t align_u16;
    uint32_t align_u32;
    uint8_t bytes[CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX)];
} cgl_particles_storage_t;

#define CGL_PARTICLES_STORAGE_BYTES(n)   /* n == CGL_PARTICLES_MAX 时恰为 sizeof(cgl_particles_t) */
```

`ox_q8` / `oy_q8` 必须是 `int32_t`：320 像素面板的中心需要 160 × 256 = 40960，装不进 `int16_t`，会静默回绕成负数，症状是投影原点左移 160 像素（云画在屏幕外，而深度排序完全正常）。

`cgl_particles_storage_t` 这个 **union 类型**存在的理由是让使用者有一个正确对齐的载体：引擎会把存储强转为 `cgl_particles_t`，其中 `uint32_t seed` 要求 4 字节对齐，而"尺寸正确但对齐错误的强转"是未定义行为。

| API | 语义 |
|---|---|
| `cgl_particles_t *cgl_particles_init(void *storage, uint32_t storage_bytes)` | 把引擎绑到调用者存储；**不分配任何内存**，存储只绑定一次并比引擎活得久。存储必须至少 `CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX)` 字节且按 4 字节对齐。缓冲过小会被**拒绝**（返回 `NULL`）而不是越界写入 |
| `void cgl_particles_build(s, dot_count, is_lit, cell_x, cell_y)` | 构建处于**静止**状态的一轮：点阵每个点亮格一个粒子，精确落在自己的格上，其余备用粒子停在中心待用。它通过两个回调访问位图，而不是包含位图头文件——这使引擎保持自包含、可宿主测试，也让测试可以喂给它一张合成网格 |
| `void cgl_particles_burst(s, impact_x, impact_y, impact_z)` | 引爆：给每个停放粒子一个向外的速度，如同球在 `impact_x/y/z`（Q8.8）处撞击并朝 +x 行进。同时也是调用者"子弹时间"倒计时开始的地方 |
| `uint16_t cgl_particles_impact(s, ball_x_q8, ball_r_q8)` | 把重球的**扫掠**推进一个逻辑步：释放球的**前缘**已经到达的每个粒子，并**返回释放了几个**（0 = 球还没碰到那片区域）。逐点释放而非整体引爆，是因为整体引爆会让球还没碰到大部分点时那些点就飞走，读作"徽标在球到达之前就溶解了" |
| `void cgl_particles_reset(s)` | 丢弃全部粒子（`life = 0`） |
| `const cgl_particles_world_t *cgl_particles_world(s)` | 世界常量（原点、格距） |
| `void cgl_particles_step(s)` | 一个**逻辑**步：重力、阻尼、积分、剔除。步是单位化的，它不知道毫秒是什么；页面拥有时间尺度并每固定步调用它 0..N 次，这正是"时间几乎停止"的整数表达法 |
| `int16_t cgl_particles_pos(s, i, axis)` / `int8_t cgl_particles_vel(s, i, axis)` | 读位置/速度（axis：0 = x，1 = y，2 = z），供测试使用，无需了解结构布局 |
| `uint16_t cgl_particles_sort(s, cam)` | 针对一个相机做深度排序（远 → 近），**返回仍存活的粒子数**。相机变化时必须重排——本页环绕运行时就是每帧 |
| `uint16_t cgl_particles_order_at(s, k)` | 第 k 个绘制顺序的粒子索引（0 = 最远） |
| `void cgl_particles_cam_look(cam, yaw_u8, orbit_q8, frame)` | 生成相机矩阵。`yaw_u8` 是 16 位角度的最高字节（0..255 = 一圈）；`orbit_q8` 是沿视轴的推拉偏移（保持 `\|orbit_q8\| < 64`，否则深度钳位会吃掉场景的远端）；`frame` 决定投影中心 |
| `uint16_t cgl_particles_active(s)` / `max_active(s)` / `uint8_t cgl_particles_tick(s)` | 观测 |
| `uint16_t cgl_particles_view_draw_band(s, cam, c, band_y0, band_y1, color)` | 把落在 `[band_y0, band_y1)` 内的粒子画进画布。**必须按条带递增顺序每带调用一次**；排序**必须已经用传入的相机做过**（它自己不排序）。返回实际绘制的粒子数 |

`cgl_particles_cam_t` 里的 `ox_q8`/`oy_q8`、`cgl_particles_rect_t`（与 `cgl_rect_t` 形状相同但**刻意是不同类型**，把两者混用是编译期错误）、以及 `cgl_particles_view_frame()` 这个唯一的类型转换入口，共同构成"引擎不认识框架、绘制器搭桥"的边界。

### 15.6 `cgl_debris`：碎裂场景

场景职责：`cgl_debris_init()` 绑定世界 + 相机 + 位图 + 视口并构建第一轮；`cgl_debris_step()` 在**调用者已经步进物理之后**观察世界、推进循环、在残骸静止后重建；`cgl_debris_restart()` 立即重建。

**它从不步进物理。** `cgl_phys2d_step()` 是应用自己的调用——因为"谁推进世界"在一个项目中必须只有一个答案；一个替使用者步进物理的控件或场景助手会让"有 N 个东西各调用一次，世界就跑快 N 倍"重新成为可能。本模块只**观察**。

```c
typedef struct { int16_t x0, y0, x1, y1; } cgl_debris_rect_t;

typedef struct {
    int grid_w, grid_h;              /* 横向/纵向格数，> 0 */
    bool (*is_lit)(int i, int j);    /* 格 (i, j) 是否点亮；未点亮的格直接跳过 */
} cgl_debris_src_t;

typedef enum { CGL_DEBRIS_PHASE_SMASH = 0, CGL_DEBRIS_PHASE_SETTLE = 1 } cgl_debris_phase_t;

typedef struct {
    cgl_phys2d_t *world;         /* 刚体池；由调用者声明并步进 */
    cgl_iso2d_view_t *view;      /* 相机 + 深度排序器；NULL = 什么都不画（纯物理测试传它） */
    const cgl_debris_src_t *src; /* 位图（调用者的） */
    cgl_debris_rect_t frame;     /* 相机拟合进的视口 */
    void (*on_change)(void *user);  /* 模块唯一的向外调用："需要重绘"；可以为 NULL */
    void *on_change_user;
    uint16_t dot_count;   /* 上次构建放置的点数 */
    uint16_t ball_index;  /* 池中重球的索引 */
    uint16_t active;      /* 仍在仿真域内的点数 */
    uint8_t  phase;       /* cgl_debris_phase_t */
    uint8_t  style;       /* 当前相机风格 */
    uint16_t settle_ms;   /* SETTLE 阶段累计的毫秒 */
    uint32_t restarts;    /* 已完成轮数 */
} cgl_debris_t;
```

结构体中的每个指针都是**借用**的，必须比它活得久。位图是调用者的——用两个成员加一个回调，而不是包含一个位图头文件，这正是让模块保持为模块的原因。

| API | 语义 |
|---|---|
| `void cgl_debris_init(d, world, view, src, frame)` | `world` 与 `src` **必需**；`view` **可以为 `NULL`**（什么都不画，物理照跑）。`frame` 是下次相机拟合使用的视口，本调用会执行一次拟合 |
| `void cgl_debris_restart(d)` | 重建点阵并把重球放回起点，**然后重置深度排序器**（重建后的世界 `step` 归零，过期的排序戳记会让下一帧拿到旧顺序） |
| `void cgl_debris_step(d, step_ms)` | 一个物理步之后的收尾：观察世界、推进 SMASH/SETTLE、静止计时结束后重建。调用者**必须**已经调用过 `cgl_phys2d_step()`。`settle_ms` 由 `step_ms` 累加，因此 `CGL_DEBRIS_SETTLE_MS` 保持"毫秒"语义，不受时钟步长影响 |
| `void cgl_debris_set_style(d, style)` | 运行时切换相机风格（按当前 frame 重新拟合） |
| `bool cgl_debris_cell_world(d, i, j, *x, *y)` | 格 (i, j) 的世界坐标（Q12.4），与重建时使用的那一套完全一致。**为渲染测试而设**：把每个格的世界坐标用当前相机投回去并读像素，机器即可判定投影后点阵是否仍可辨认。越界时返回 false |
| `uint16_t cgl_debris_dot_count(d)` / `active_dots(d)` / `ball_index(d)` | 观测 |

绘制器：

```c
typedef struct {
    cgl_color_t floor;       /* 比背景亮一档 */
    cgl_color_t grid;        /* 地面网格线   */
    cgl_color_t shadow;      /* 比地面暗一档 */
    cgl_color_t dot;         /* 普通（点阵）刚体 */
    cgl_color_t ball;        /* 本场景的重球 */
    cgl_color_t ball_other;  /* 世界中的其它球体（用不同颜色画，使"是哪颗球撞散了点阵"始终可见） */
} cgl_debris_palette_t;

uint16_t cgl_debris_view_draw(const cgl_debris_t *d, cgl_canvas_t *c,
                              int16_t band_y0, int16_t band_y1,
                              const cgl_debris_palette_t *pal);
```

`cgl_debris_view_draw()` 绘制地面、网格以及落在本条带内的全部刚体，**按条带递增顺序每带调用一次**。`pal == NULL` 意味着"没有颜色"，即什么都不画（函数不发明默认值，忘记调色板的调用者得到空画面而不是意外结果）。返回实际绘制的刚体数。

地面属于模块而不是应用：碎屑会得到一像素的地面阴影，而在深色背景上阴影只有在有东西可落时才看得见。地面是让"这个点在空中"这句话可读的前提。

**一渲染器规则**：刚体按 `cgl_iso2d_sort()` 产生的**唯一**深度顺序绘制，远者先、近者后，后者覆盖前者。控件树按插入顺序绘制子控件，无法表达"这两个点应该在那两个点之间"，因此**一次 `cgl_debris_view_draw()` 调用必须画出世界的全部刚体**。若一个控件画点阵、另一个画球，球会永远盖住点阵，球飞到点阵之后时遮挡立即出错。

### 15.7 `cgl_bounce`：重力球

该模块存在的意义是消除四个反复出现的缺陷：`vy += g` 必须在积分**之前**（半隐式欧拉，稳定），放到之后（显式欧拉）会在每个弧顶获得高度；只反射速度不够——位置也必须夹回，否则球会"从它从未到过的地方弹起"，读作卡顿；世界不记得你的刚体（`cgl_phys2d_reset()` 只把 `count` 归零，不释放、不通知），因此本模块按**身份**（半径 + 球体标志）重新建立自己的刚体，而不是按索引；恢复系数有下限——参考场地每次弹跳损失 `(1 - e²)`，低于默认值太多会让球两三次之后粘在地面上。

调用形态：

```
cgl_bounce_init()        绑定世界与活动区域，抛出球
cgl_bounce_pre_step()    vy += g        —— 在 cgl_phys2d_step() 之前
cgl_bounce_post_step()   四面墙           —— 在 cgl_phys2d_step() 之后
cgl_bounce_restart()     立即回到出生点
```

两个半程必须成对且**每步恰好一次**：漏掉 `pre_step` 是漂浮的球，漏掉 `post_step` 是跑出区域、只在有人读位置时才被夹回来的球。它们分成两个入口而不是一个 `cgl_bounce_step()`，是因为唯一的那次 `cgl_phys2d_step()` 必须夹在中间。

```c
typedef struct { int16_t x0, y0, x1, y1; } cgl_bounce_rect_t;
#define CGL_BOUNCE_NO_INDEX 0xFFFFu   /* 球不在池中时 cgl_bounce_t::index 的取值 */

typedef struct {
    cgl_phys2d_t *world;
    uint16_t index;          /* 池中索引或 CGL_BOUNCE_NO_INDEX。**绝不要直接读取并使用它**：
                                它只在下一次 cgl_phys2d_reset() 之前有效；
                                每次使用前 cgl_bounce__ensure() 会重建刚体；显示请用 cgl_bounce_index() */
    uint16_t bounces;        /* 累计撞墙次数（HUD 读数，也是测试断言的对象） */
    uint16_t max_speed_q4;   /* 历史最大速度，Q12.4（仅供观测） */
    uint16_t area_rejected;  /* 因不可能的区域而被拒绝的次数 */
    uint8_t  has_area;       /* 1 = 有场地，0 = 没有（初始化时区域非法或未给） */
    int16_t left_q4, right_q4, top_q4, bot_q4;                  /* 活动区域，Q12.4 */
    int16_t start_x_q4, start_y_q4, start_vx_q4, start_vy_q4;   /* 出生参数 */
    void (*on_change)(void *user);   /* 模块唯一的向外调用："需要重绘"；可以为 NULL */
    void *on_change_user;
} cgl_bounce_t;
```

活动区域是"**球心可以占据的屏幕像素闭区间矩形**"——用球心范围而不是"球能装进去的盒子"，是因为两个夹取与两次反射比较的就是它，于是模块里没有任何地方需要给边界加/减半径，碰撞区域与绘制区域也不可能漂移。绘制范围是这个矩形每边向外扩 `CGL_BOUNCE_RADIUS_PX`。球心落在边界上算**在内**（墙是闭区间）。`has_area` 使"被拒绝"意味着**惰性**而不是"留在某个默认状态"：四个边界全为 0（原点处的一像素见方场地）是完全合法的区域，否则惰性球会把自己加进世界并在屏幕角落弹跳。

| API | 语义 |
|---|---|
| `bool cgl_bounce_init(b, world, area)` | 绑定并抛出球；出生点与速度由 `CGL_BOUNCE_SPAWN_*` 从区域推导，因此"左上方、向右抛出"在调用者把场地放到哪里都成立。返回 false 的条件：没有世界，或区域被**拒绝**（反向/空，或越出物理仿真域）。失败时不抛球，此后每次调用都是空操作，球保持**惰性**——继续调用安全，继续步进也安全 |
| `bool cgl_bounce_set_area(b, area)` | 移动**墙**而不动球：在新区域外的球会被下一次 `cgl_bounce_post_step()` 夹回；在里面的保持位置与速度。出生点**不**重算。区域被拒绝时返回 false，且**原区域继续生效** |
| `void cgl_bounce_set_spawn(b, x_px, y_px, vx_q4, vy_q4)` | 设置**下一次**抛出的起点与速度，不移动正在飞行的球。位置以像素给出，速度以 Q12.4 给出 |
| `void cgl_bounce_pre_step(b)` / `post_step(b)` | 重力 / 四面墙反射 + 恢复系数衰减 + 撞墙计数 + 速度观测 |
| `void cgl_bounce_restart(b)` | 从出生点重新抛出并清空计数器 |
| `bool cgl_bounce_position_px(b, *x, *y)` | 球心屏幕坐标，**总是被夹在活动区域内**。夹取正是这个函数的要点：一步是"先积分、后反射"，而反射紧接在 `cgl_phys2d_step()` 之后，因此**在这两个时刻之间**球完全可能越界。与其依赖调用者的读取顺序，不如在读取点强制不变量。它会自愈：若一次 `cgl_phys2d_reset()` 把球从池中抹掉，它会把球加回去。参数**不是 const** |
| `bool cgl_bounce_draw_position_px(const cgl_bounce_t *b, *x, *y)` | 给绘制器使用的 const 版本。唯一区别是它**不自愈**：绘制器不得向世界添加刚体（它在 `draw` 回调里运行，在那里加入的刚体会在没人计划的时刻进入池子）。球被抹掉且此后没有步进时返回 false |
| `uint16_t cgl_bounce_bounces(b)` / `bool cgl_bounce_area_px(b, *area)` / `uint16_t cgl_bounce_max_speed_q4(b)` / `cgl_bounce_index(b)` / `cgl_bounce_area_rejected(b)` | 观测。`area_px()` 把生效的区域换算回像素返回，使调用者可以绘制场地、在其中布局，或（如参考测试那样）断言球从未离开它，而不必重新推导算术并与之漂移 |

绘制器与独立控件：

```c
cgl_bounce_rect_t cgl_bounce_view_area(cgl_rect_t frame);   /* frame 每边内缩半径 */
uint16_t cgl_bounce_view_draw(const cgl_bounce_t *b, cgl_canvas_t *c,
                              int16_t band_y0, int16_t band_y1, cgl_color_t color);

typedef struct {
    cgl_widget_t base;      /* 必须是第一个成员 */
    cgl_bounce_t ball;      /* 引擎；ball.world 是调用者的，借用 */
    cgl_color_t color;
} cgl_bounce_widget_t;

bool cgl_bounce_widget_init(cgl_bounce_widget_t *w, const char *name, cgl_rect_t frame,
                            cgl_phys2d_t *world, cgl_color_t color);
void cgl_bounce_widget_set_color(cgl_bounce_widget_t *w, cgl_color_t color);
```

`cgl_bounce_view_draw()` 通过读取位置时用的同一个夹取来取球心，因此"一步把球积分到墙外"不可能画出游乐场之外的一个像素。它按**条带**裁剪（该效果只有一个元素，提前返回比任何逐图元测试都便宜），返回 1 表示画了、0 表示被裁掉或球不在池中——0/1 的返回值让测试可以断言"这条带画了球"而不必读像素。

**二选一的规则**（来自头文件）：**单独漂浮的球**用 `cgl_bounce_widget_t`（z 不是问题）；**在有序场景内**用 `cgl_bounce_view_draw()`（每个世界只有一个绘制者）。独立控件把引擎的 `on_change` 接到 `cgl_widget_invalidate()`，因此步进之后没有任何需要记住的东西；但它**不推进自己的物理**，调用者必须在中间驱动步进：

```c
cgl_bounce_pre_step(&ball.ball);   /* 1) vy += g            */
cgl_phys2d_step(&world);           /* 2) 唯一的一次积分      */
cgl_bounce_post_step(&ball.ball);  /* 3) 四面墙              */
```

### 15.8 构建、链接与关闭开关

**三种消费方式**

```bash
# A) 作为 CytGL 的一部分（CMake 目标）
cmake -B build -DCYTGL_BUILD_EXT=ON
# 然后在 CMake 里：target_link_libraries(my_app PRIVATE cytgl::ext)   # 会连带链接 cytgl::cytgl

# B) 独立构建：src/ext 本身就是一个工程，完全不需要框架
cmake -B build src/ext && cmake --build build      # -> libcytgl_ext.a
# 或：add_subdirectory(third_party/CytGL/src/ext)

# C) 作为两个普通源文件，完全不用构建系统
cc -std=c11 -O2 -Iinclude my_app.c src/ext/cgl_phys2d.c src/ext/cgl_iso2d.c -o my_app
```

**方式 B 的构建只包含框架无关的一半**（物理对、粒子引擎、碎裂场景、重力球与生成的表）。绘制器（`cgl_particles_view.c`、`cgl_debris_view.c`、`cgl_bounce_view.c`）包含 `<cytgl/cgl_canvas.h>`，因此只在框架目标存在于构建图中时才被加入目标——独立构建若试图编译它们会失败，这是承诺被遵守而不是限制。

`src/ext/CMakeLists.txt` 发布一个包含根（仓库的 `include/`），因此 `<ext/...>` 与 `<cytgl/...>` 在同一个 `-I` 下并列。安装规则把 `libcytgl_ext.a` 放进 `lib/`，把模块头文件放进 `<前缀>/include/ext/`（刻意**不在** `include/cytgl/` 内）。核心不生成 `find_package` 配置，模块同样不生成：通过 `add_subdirectory()` / `FetchContent()` 以 `cytgl::ext` 使用。

**关闭某个效果**（`CGL_CFG_PARTICLES` / `CGL_CFG_DEBRIS` / `CGL_CFG_BOUNCE`，默认均为 1）

不链接它们是不付代价的常规方式，且不需要宏：`libcytgl.a` 从不列出该模块，不引用它的构建在 Flash 与 RAM 上都是 0 字节。宏针对的是一个特定场景——**按目录收集源文件**的构建（本仓库的固件构建就是这样）无论有没有人调用都会编译 `src/ext/` 下的每个 `.c`：

```bash
cc -DCGL_CFG_PARTICLES=0 ... src/ext/cgl_particles.c src/ext/cgl_particles_view.c   # 两个都变成空
cc -DCGL_CFG_DEBRIS=0    ... src/ext/cgl_debris.c    src/ext/cgl_debris_view.c      # 同上
cc -DCGL_CFG_BOUNCE=0    ... src/ext/cgl_bounce.c    src/ext/cgl_bounce_view.c      # 同上
```

要让 **RAM** 真的回来，应用侧必须配合：页面作为静态成员声明的存储，在引擎被编译掉之后依然存在。用 `CGL_PARTICLES_STORAGE_BYTES(n)` 给粒子池定量（或降低 `CGL_PARTICLES_MAX`），不想要某个页面就把它也从构建里去掉。

**这些宏是构建级的**：一个编译单元看到 0、另一个看到 1 的两个编译单元不能共享同一个 `cgl_particles_t`。把它们放进构建系统，而不是某个文件内部的局部定义。`CGL_PHYS2D_USER_STORAGE` 更是如此——它**改变 `cgl_phys2d_t` 的内存布局**，同一个二进制里混用两种布局是未定义行为。

### 15.9 契约、保证与限制

**保证**

- **从不用堆。** 每个字节要么在调用者声明的结构里，要么在调用者的栈上。
- **没有可变全局状态。** 两个独立世界无法互相干扰；模块自身的 RAM 开销为 0。
- **无浮点、无 libm、无 64 位除法辅助函数。** 内核循环全是整数。
- **确定性。** 相同输入状态与相同步数给出逐位相同的输出：没有时间源、没有随机性（粒子用的是引擎内的 xorshift 状态）、不依赖调用者添加刚体的顺序。
- **深度排序是稳定全序。** 桶排序 O(n + buckets)，无比较、无递归，等深度元素保持输入顺序；`sorter.count` 就是场景中实际元素数，因此被剔除的刚体绘制代价为零。
- **相机拟合兑现承诺**（见 15.4）。
- **被拒绝的状态是惰性而非半活**：初始化失败后继续调用安全、继续步进安全。
- **链接面很小**：`cgl_phys2d_reset/add/step`、`cgl_iso2d_cam_set/cam_fit/project_rect/sorter_reset/sort/view_init` 九个函数，加上若干强制内联访问器。

**调用者的义务**

- **每个世界只有一个绘制者**（一渲染器规则，见 15.6）。
- **世界重建后必须重排**：`cgl_phys2d_reset()` 把 `step` 归零，因此重建后要调用 `cgl_iso2d_sorter_reset()`，否则首帧可能用旧顺序绘制。
- **位置变化必须换戳记**：`cgl_iso2d_sort()` 在 `src->stamp` 未变时是空操作；任何位置改变都必须伴随不同的戳记。粒子引擎更严格——**相机**变化就必须重排（旋转相机在没有任何粒子移动时也会改变画家顺序）。
- **排序表按场景定量**：`CGL_ISO2D_MAX_POINTS` 是不小于 `CGL_PHYS2D_MAX_BODIES` 的硬性闸门（由桥中的 `_Static_assert` 把守），也必须不小于真实元素数，否则多出的元素被静默忽略——表现为"某些精灵永远不出现"。
- **调用者存储必须比世界活得久**，且**一份存储只能属于一个世界**（两个世界共用一份存储会静默互相破坏）。
- **只有一处推进世界**：`cgl_phys2d_step()` 属于应用；碎裂场景只观察；重力球控件不推进自己的物理。
- **重力球的两个半程必须成对**，每步恰好一次，且唯一的那次 `cgl_phys2d_step()` 夹在中间。
- **不要缓存 `cgl_bounce_t::index`**，也不要直接对它采取行动。
- **条带协议**：每个条带绘制器必须按条带递增顺序每带调用一次；粒子绘制器要求排序已经用传入的相机做过。

**有意不做的（非目标）**

- **形状**：只有圆。没有方块、没有旋转、没有力矩（转动惯量无穷大）。
- **碰撞**：只在 XY 平面。`z` 是表现轴：从上方飞过的碎块仍会与它经过的东西碰撞，这正是隐喻读起来正确的原因。
- **手感**：平面内没有重力也没有摩擦，恢复系数为零；竖直轴有重力、起飞规则与落地恢复系数，都是可调项。因此"在平面内落下的球"不是引擎的职责——`cgl_bounce` 是补上它的那一层。
- **没有堆，也没有场景图**：排序在模块内解决，直到出现第二个无关的消费者要求框架级的深度键。
- **一份存储一个世界**，池索引类型是 `int16_t`（因此池最多 32767 个刚体，构建期把关）。
- **不负责计时**：模块没有时钟，`cgl_phys2d_step()` 一次就是一步。

### 15.10 完整示例

以下示例把模块作为整体使用：建世界、步进、拟合相机、深度排序、投影，并把全部刚体经**一次**绘制调用渲染进画布条带。

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

/* ---- 1. 位图：调用者提供 is_lit，模块不认识位图头文件 ---- */
static const char *const s_glyph[GRID_H] = {
    "...####.....", "..##..##....", ".##....##...", "..##..##....", "...####....."
};

static bool glyph_is_lit(int i, int j)
{
    if ((i < 0) || (i >= GRID_W) || (j < 0) || (j >= GRID_H)) { return false; }
    return s_glyph[j][i] == '#';
}

/* ---- 2. 场景状态：全部为静态存储 ---- */
typedef struct
{
    cgl_widget_t base;       /* 必须是第一个成员：本结构体就是一个控件 */
    cgl_phys2d_t world;      /* 默认容量下约 12.4 KB                   */
    cgl_iso2d_view_t view;   /* 相机 + 深度排序器，每个场景一份          */
    cgl_debris_t scene;      /* 借用 world + view + src                */
    cgl_debris_src_t src;
    cgl_debris_palette_t pal;
    cgl_bounce_t ball;       /* 同一个世界里的第二个刚体                 */
} demo_t;

static void demo_draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    demo_t *d = (demo_t *)w;
    const int16_t band_y0 = c->origin_y;
    const int16_t band_y1 = (int16_t)(c->origin_y + c->height);

    /* 世界的唯一绘制者：地面、网格与**全部**刚体（含重力球，用 ball_other 配色）
       在一次调用里按同一深度顺序画出。绝不要用第二个控件再画一遍这个世界。 */
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

    /* 绑定 world + view + 位图 + 视口，按 CGL_DEBRIS_SCENE_STYLE 拟合相机，构建第一轮 */
    cgl_debris_init(&d->scene, &d->world, &d->view, &d->src,
                    cgl_debris_view_frame(frame));

    /* 同一个世界里的一个重力球；碎屑绘制器也会画它。
       cgl_bounce 在任何重建之后按身份重新建立自己的刚体。 */
    (void)cgl_bounce_init(&d->ball, &d->world, cgl_bounce_view_area(frame));
}

static void demo_step(demo_t *d, uint32_t step_ms)
{
    cgl_bounce_pre_step(&d->ball);        /* 1) vy += g        */
    cgl_phys2d_step(&d->world);           /* 2) 唯一的一次积分   */
    cgl_bounce_post_step(&d->ball);       /* 3) 四面墙          */
    cgl_debris_step(&d->scene, step_ms);  /* 观察并推进循环      */
}

/* ---- 3. 显式排序与投影（绘制器内部做的就是这件事） ---- */
static uint16_t demo_project(demo_t *d, int16_t *sx_out, int16_t *sy_out, uint16_t cap)
{
    uint16_t k, n;

    cgl_iso2d_sort_bodies(&d->view.sorter, &d->view.cam, &d->world);  /* 一步之内是带戳记的空操作 */
    n = (d->view.sorter.count < cap) ? d->view.sorter.count : cap;

    for (k = 0u; k < n; k++)                      /* 远 -> 近 */
    {
        const cgl_phys2d_body_t *b = &d->world.body[d->view.sorter.order[k]];
        int16_t sx, sy;
        cgl_iso2d_project(&d->view.cam, b->x, b->y, b->z, &sx, &sy);   /* 世界 Q12.4 -> 屏幕像素 */
        sx_out[k] = sx;
        sy_out[k] = sy;
    }
    return n;
}
```

要点复核：

- `cgl_debris_init()` 内部会执行一次相机拟合，因此首次排序之前不需要显式 `cgl_iso2d_cam_fit()`。
- `cgl_bounce_init()` 在 `cgl_debris_init()` **之后**调用，以免球被场景重建抹掉；此后的重建由 `cgl_bounce_post_step()` 的自愈恢复它。
- 每步顺序严格遵循重力球头文件规定的形态，`cgl_debris_step()` 是物理之后调用者的观察调用。
- 绘制路径遵守一渲染器规则：每条带恰好一次 `cgl_debris_view_draw()`，按条带递增顺序；同一个世界上**不**再调用 `cgl_bounce_view_draw()`。
- `demo_t.base` 是第一个成员，与 `cgl_bounce_widget_t` 记录的同一条规则一致。

---

## 16. 扩展配方

### 16.1 新增一个控件

参考实现有两个，回答两个不同的问题：

- **应用自己的控件**：只需满足 9.6 节的七条规则，完全不碰框架。最小的完整示例是 `cgl_panel.c`（约 30 行）与 `cgl_label.c`。
- **框架自己的控件**：`src/core/cgl_bar.c` 是范例——它需要 `cgl_anim`、可配置几何与自己的失败计数器，并且必须自带逐像素规格测试。`src/core/cgl_arc.c` 展示了更复杂的情形（自带光栅器 + 颜色表 + 仪表）。

### 16.2 把一个"东西"整体做成半透明

**一行**，不需要改动控件：

```c
cgl_widget_set_alpha(&veil.base, 128u);   /* 0..255 */
```

| 想要的效果 | 用什么 | 代价 |
|---|---|---|
| 一整层纯色变暗/变亮（遮罩） | `cgl_canvas_fill_rect_blend_q8()` | **0 额外 RAM**，约 28 周期/像素 |
| **半透明文字 / 图标 / 面板** | `cgl_widget_set_alpha(&w.base, a)` | 每条带每层一份暂存区 |
| 一个容器加若干子控件作为整体半透明，且子控件之间互相重叠 | 同上（这正是它的强项：重叠处不会被混合两次） | 同上 |
| 只需要几个像素半透明、不需要"组"语义 | 自行逐行调用 `cgl_canvas_fill_rect_blend_q8()` | 0 额外 RAM，但 pan/clip/warp 要自己处理 |

三个陷阱：

1. **没有暂存区时 alpha 被忽略**（按不透明绘制）而不是报错。用 `cgl_widget_alpha_dropped()` 观察，正常应为 0。
2. **半透明层下面必须有东西**：本框架没有全屏帧缓冲，混合只能透出"同一条带遍历中更早绘制的像素"。它无法透出上一帧，也无法透出被 `CGL_WF_VISIBLE` 关闭的页面（那是导航器层面的问题，不是混合器的问题）。
3. **不要为了 alpha 清除 `CGL_WF_CLIP`**：那会把隔离区域从"自己的 frame"扩大到"整个裁剪窗口"——正确但更贵。若某个覆盖层必须伸出父控件 frame 又想要节省，请把它挂到更高的祖先之下。

### 16.3 新增一个页面

见 13.7。三次改动：一个 `*_attach()` 函数、`app_ui_t` 里一个对象成员、页面表里一行。要让用户能到达该页面，把它加入菜单的名称表与目标解析函数，并把菜单项数量加一。

### 16.4 新增/调整一个转场

- **让某个页面使用某种转场（最常见）**：在该页面自己的 `*_attach()` 中写 `cgl_page_set_trans_enter/leave()`，一个管进入、一个管离开。
- **改变"所有页面"的默认转场**：`cgl_page.c` 中按键于 PUSH/POP/REPLACE/RESET 的那个 `switch`（`nav__default_trans()`）。若应用层想对某一次导航强制类型，给 `cgl_page_nav_request()` 传一个具体 `CGL_TRANS_*`（只在页面未声明类型时生效）。
- **改变吸入口**：`cgl_page_nav_set_genie_target(nav, x, y)`。例如"吸进左下角的 Dock"是 `(24, 屏幕高度 + 6)`。
- **改变形状**：`CGL_CFG_GENIE_NARROW_Q8`（窄端宽度）以及 `warp__k()` 的三个因子。
- **变快/变慢，或只改某一类**：页面级用 `cgl_page_set_trans_enter/leave()` 的 `dur_ms`；类型级用 `cgl_page_nav_set_transition_duration(nav, trans, ms)`；全局用 `cgl_page_nav_set_duration(nav, ms)`。页面声明会覆盖类型表，因此"只让信息页更慢"和"让所有 COVER_UP 更慢"是两个不同的旋钮。
- **换节奏**：页面级用 `ease`，类型级用 `cgl_page_nav_set_transition_ease()`，全局用 `cgl_page_nav_set_ease()`。默认是"滑动 `OUT_CUBIC`，吸走 `IN_QUAD`，倒出 `OUT_CUBIC`"。
- **警告**：不要给 `GENIE_IN` / `GENIE_OUT` 配 `CGL_EASE_OUT_BACK`（见 12.2 末尾）。

### 16.5 新增一种输入设备（触摸 / 编码器 / 远程）

只需产生一个 `cgl_event_t` 并投递：可以在移植层的 `poll_input` 钩子里（每帧一次，带该帧的 `dt`——需要去抖的东西的正确位置），也可以在事件产生处调用 `cgl_port_post_event()`。触摸只需实现 `cgl_widget_hit_test()` 加 `CGL_EV_TOUCH_*` 分发（`cgl_page_nav_event()` 已经会转发它们）；编码器侧键投递 `CGL_EV_BACK`。

**只有一个生产者**时最省事：一处扫描全部输入、一处投递，并由 `poll_input` 每帧携带当帧 `dt` 驱动。

### 16.6 更换显示设备 / 更换总线

- **更换面板控制器**：实现一个 `cgl_display_drv_t`，然后替换移植描述符 `s_port` 中的 `display` 字段（应用层从不指名任何驱动）。
- **更换传输方式（并口 / 软件模拟 SPI / 仿真器）**：实现一条总线虚表（参考工程中的 `lcd_bus_t`），面板驱动一行不改。
- **在 PC 上验证**：实现一个把条带写入内存数组的驱动，即可逐像素断言真实框架代码，无需硬件。

### 16.7 更换字体

用第 18.6 节的生成工具从 TTF 生成字形表，填入一个 `cgl_font_t` 并把它传给控件；或直接手写一张符合 8.1 节布局的表。

---

## 17. 资源占用与裁剪

### 17.1 参考构建

参考构建是随附的示例应用，运行在 STM32G431CB（Cortex-M4 @ 170 MHz，128 KB Flash / 32 KB RAM）上，`-O2` 编译，效果开关全部打开：

| 项目 | 数值 | 占比 |
|---|---|---|
| Flash（`text` + `data`） | 62 192 B | 128 KB 的 47.5% |
| 其中静态数据（`.data` + `.bss`） | 30 856 B | 32 KB 的 94.2% |
| 栈预留（链接脚本中 `_Min_Stack_Size` = 0x540） | 1 344 B | 32 KB 的 4.1% |
| RAM 使用量（`data` + `bss` + 预留） | 32 200 B | 32 KB 的 98.3% |
| **剩余未分配** | 568 B | 32 KB 的 1.7% |

Flash 与静态数据是 `arm-none-eabi-gcc` 14.3 的实测值；栈预留取自链接脚本，因此后三行是"该实测值 + 当前实际生效的预留"。**物理可用栈 = 32 KB − 静态数据**，全效果开启时为 1 912 B（与"剩余未分配"不是同一个量：栈从 RAM 顶部向下生长，预留本身也是栈）。

**RAM 才是瓶颈，不是 Flash。** 关掉两个动画页面（碎裂与子弹时间）并因此让物理世界与粒子池无人引用之后，同一套源码约为 46.7 KB Flash / 16.0 KB RAM，可用栈从约 1.9 KB 升到约 18 KB。

库本身在两者中都只占一小部分；大部分是示例应用的物理世界（480 个刚体加碰撞网格）、表盘，以及配置启用的 5 KB 条带缓冲加 5 KB 不透明度暂存区。

帧率的上限由总线决定：42.5 MHz SPI 推送 320×172×2 字节 = 110 080 字节约需 20.7 ms，即约 48 fps 的推送上限；实际帧率还受 CPU 光栅化限制。半屏或部分条带重绘会相应提高有效帧率。

### 17.2 裁剪清单

| 旋钮 | 效果 |
|---|---|
| `CGL_CFG_ALPHA 0` | 完全移除不透明度暂存区：参考几何下**回收约 5 KB RAM**；`cgl_widget_set_alpha()` 退化为被计数的空操作 |
| `CGL_CFG_BAND_H` | 条带缓冲 RAM = `2 × W × BAND_H × 2`。减半省约 2.5 KB，代价是更多 SPI 开窗命令 |
| `CGL_CFG_LABEL_MAX_TEXT` | 每个标签 32 字节；示例有约 25 个标签，每减 8 字节省约 200 字节 |
| `CGL_CFG_PAGE_STACK_MAX` | 每项 4 字节 |
| `CGL_CFG_WATCH_TRAIL 0` | 移除表盘的彗尾状态（60 字节 + 累加器），以及每帧的彗尾损坏标记 |
| `CGL_CFG_WATCH_DIAL_LUT 0` | 移除 578 字节的默认盘面查找表，改为程序化着色（每像素约三倍 CPU，零 Flash） |
| `CGL_CFG_WATCH_GLASS 0` | 移除玻璃罩通道（纯 Flash，但每帧少一次整盘绘制） |
| `CGL_CFG_WATCH 0` | 连同表盘、日历与损坏模型一起移除（要求 `CGL_CFG_ARC` 同时为 0） |
| `CGL_CFG_ARC 0` | 移除整个径向工具箱（要求 `CGL_CFG_WATCH 0`） |
| `CGL_PHYS2D_MAX_BODIES` | 刚体池每个 16 字节加网格链表的 2 字节。示例场景需要 480 点 + 2 球，528 留了约 9% 余量。**不要降到 500 以下**：重力球会借用同一个世界再加一个刚体，容量不足时 `cgl_phys2d_add()` 开始返回 -1（表现为点阵静默缺格） |
| `CGL_PARTICLES_MAX` | 每个粒子 12 字节，示例为 500（6.0 KB）。用 `CGL_PARTICLES_STORAGE_BYTES(n)` 给存储定量 |
| `CGL_ISO2D_MAX_POINTS` / `CGL_ISO2D_DEPTH_BUCKETS` | 排序表每项 2 字节；桶每项 2 字节。注意 `MAX_POINTS` 必须不小于 `PHYS2D_MAX_BODIES`，且小于真实元素数会让多出的元素静默消失 |
| `-DCGL_CFG_PARTICLES=0` / `-DCGL_CFG_DEBRIS=0` / `-DCGL_CFG_BOUNCE=0` | 把对应效果的编译单元清空（用于按目录收集源文件的构建）。**RAM 要靠应用侧同时移除静态存储才能回来** |
| 应用侧效果开关 | 去掉整个页面及其拥有的存储。示例应用的 `APP_FX_SMASH` / `APP_FX_PARTICLES` 就属于这一类，且前者是回收物理世界的开关 |

### 17.3 估算方法

- **条带缓冲**：`2 × CGL_CFG_SCREEN_W × CGL_CFG_BAND_H × 2` 字节。
- **不透明度暂存**：`CGL_CFG_ALPHA_LEVELS × CGL_CFG_ALPHA_LEVEL_STRIDE × 2` 字节（也可直接读 `CGL_CFG_ALPHA_BYTES`）。
- **标签**：每个 `CGL_CFG_LABEL_MAX_TEXT` 字节。
- **物理世界**：`CGL_PHYS2D_MAX_BODIES × (sizeof(cgl_phys2d_body_t) + sizeof(int16_t)) + CGL_PHYS2D_GRID_CELLS × sizeof(int16_t)`，用 `CGL_PHYS2D_STORAGE_BYTES(n)` 可直接得到。
- **粒子云**：`CGL_PARTICLES_STORAGE_BYTES(n)`。
- **栈**：物理可用栈 = 32 KB − `.data` − `.bss`。**"剩余未分配"不等于"栈余量"**：链接脚本预留的 `._user_heap_stack` 是空地址空间，栈从 RAM 顶部向下生长进入其中，因此真正的安全性由"物理可用栈"与"静态最坏调用路径"之差决定。静态最坏路径的测法是把所有编译单元都加 `-fstack-usage -fcallgraph-info=su` 编译，然后取调用图中最长的一条路径之和（要计入 DMA 中断与 36 字节的异常帧）。链接脚本里的 `_Min_Stack_Size` 把"控件树变深"变成链接失败而不是运行时踩踏；控件树每深一层约 +136 字节。
- `arm-none-eabi-size` 会把 `._user_heap_stack` 折进 `.bss`，因此 `bss` 那一行**不等于**静态数据量。"RAM used"（`data + bss`）与"static data"（`.data + .bss`）是两个不同的量，差值就是栈预留。

**增量测量（"这次改动花了多少"）**

1. 用 `git worktree` 检出父提交，在**同一目录风格**下用同一个编译器与同一个链接脚本各构建一次，只比较 `size` 的**差值**。
2. 直接比较两个构建的绝对值会被编译开关误导：IDE 构建启用 `-fno-inline` / `-fno-omit-frame-pointer`，比纯 `-O2` 多约 1.5 KB 代码。
3. 想解释每一个字节的来去，做**符号级对账**：`arm-none-eabi-nm --print-size --size-sort --radix=d <elf>`，去掉地址列后排序逐项比对，谁增谁减一目了然。
4. `text` 段大小**依赖路径**（HAL 源码中嵌有 `__FILE__`），因此不要跨目录比较，也不要与历史快照混用。

**历史快照的处理**：早期测量点（例如"只链接框架时的 38.6 KB / 29.9 KB"）与当前数值属于不同配置，**不要混用**；需要引用时明确标注测量点。

---

## 18. 构建、测试与工具

### 18.1 CMake 选项与目标

| 选项 | 默认 | 作用 |
|---|---|---|
| `CYTGL_BUILD_TESTS` | `OFF` | 构建宿主（PC）回归测试并注册到 CTest |
| `CYTGL_BUILD_EXAMPLES` | `OFF` | 加入 `examples/` 子目录；其中的固件目标只在具备 ARM 工具链与项目设置的检出中存在，否则只是一个提示性的空目标 |
| `CYTGL_BUILD_EXT` | `OFF` | 构建可选扩展模块（`cytgl_ext` / `cytgl::ext`，`libcytgl_ext.a`） |
| `CYTGL_PORT_TEMPLATE` | `ON` | 把 `src/port/cgl_port_template.c` 作为语法检查编译进库 |

语言标准为 C11 且关闭编译器扩展（`CMAKE_C_EXTENSIONS OFF`）；未指定构建类型时默认 `Release`；库目标 `cytgl`（别名 `cytgl::cytgl`），公开的唯一使用要求是包含目录 `include/`；GCC/Clang 下启用 `-Wall -Wextra -Wpedantic -Wshadow -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wundef -Wno-sign-conversion`，MSVC 下为 `/W4`。安装规则把 `libcytgl.a` 放进 `lib/`、`include/cytgl/` 放进 `include/`；不生成 `find_package` 配置，消费方式是 `add_subdirectory()` / `FetchContent()`。

### 18.2 命令

```bash
# 构建库
cmake -B build/host
cmake --build build/host

# 在 PC 上构建并运行完整回归套件
cmake -B build/host -DCYTGL_BUILD_TESTS=ON -DCYTGL_BUILD_EXT=ON
cmake --build build/host
ctest --test-dir build/host --output-on-failure

# 只跑一个测试，或按名字筛选
ctest --test-dir build/host -R alpha_test --output-on-failure
ctest --test-dir build/host -R "alpha|page" --output-on-failure
ctest --test-dir build/host -E "render|bar" --output-on-failure

# 可选扩展模块：独立构建
cmake -B build/ext src/ext && cmake --build build/ext

# 交叉编译到 Cortex-M（直接使用 gcc；推荐用工程自带的构建系统）
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
    -std=c11 -O2 -ffunction-sections -fdata-sections -fno-common -Wall -Wextra \
    -Iinclude -c src/core/*.c src/render/*.c src/port/cgl_port.c
```

MSVC 为多配置生成器，需要传 `--config Release` 与 `ctest -C Release`。测试可执行文件就是普通的可执行文件，也可以直接运行；**快照写出使用相对路径**，因此直接运行时请把构建目录作为工作目录（CTest 已把每个测试的工作目录设为 `${CMAKE_BINARY_DIR}`）。

### 18.3 回归测试清单

测试编译并运行**真实的框架源码**，只把显示驱动替换为写入内存数组的模拟驱动——在 `Devices/st7789.c` 与 `src/render/cgl_display.c` 之间只有一个 `cgl_display_drv_t` 虚表，因此主机上的逐帧断言与目标板行为一致。

| 测试 | 验证内容 |
|---|---|
| `alpha_test` | 组不透明度的逐像素规格：重叠处不双重混合、跨 10 条带正确、还原后逐位一致、超出 `CGL_CFG_ALPHA_LEVELS` 的嵌套被计数 |
| `page_test` | 页面 + 时钟 + 补间 + 子树平移：定步执行、卡顿后不追补、按类型与按页面的转场表、Genie 变形几何（`p = 0` 恒等、逐行步长不超过 1、漏斗无空洞） |
| `widget_test` | 截图无法裁决的规则：`CGL_CFG_WIDGET_MAX_CHILDREN` 被强制执行并计数、默认标志恰为 `VISIBLE\|CLIP`、命中测试遵守 `CGL_WF_CLIP`、变形子树不被命中 |
| `bar_test` | 条形控件逐像素：值到填充的映射、四舍五入、自清除绘制、边框与分段、损坏纪律、补间。写出 `build/bar_test.ppm` / `.bmp` |
| `arc_test` | 径向图元与抗锯齿：全部 65536 个角度对比 libm、`isqrt` 对比 `sqrt`、圆/线/弧几何、条带一致性与 pan。写出 `build/arc_test.ppm` / `.bmp` |
| `watch_test` | `cgl_arc` 与 `cgl_watch` **作为模块**使用：默认配置就是一只可用的表、日历进位、角度约定、烘焙表与出货烘焙器一致、"增量损坏的一帧 == 全量重绘" |
| `logo_test` | 20 秒（1250 步）物理长跑：周期、点数、包围盒、无 `int16` 溢出、`z` 在范围内、重球把点撞飞 |
| `bounce_test` | 弹跳球与逐页定步，走真实的 `cgl_screen_frame(17 ms)` 路径：步数与控件数量无关、被覆盖后不追补、不穿墙 |
| `bullet_test` | 粒子引擎与页面：引擎存储与页面世界是同一段字节、正弦折叠/倒数表/定点乘法、绘制顺序随相机变化、时间真的停止 |
| `bullet_frames` | 帧转储：驱动真实的子弹时间页面走完一整轮 |
| `clock_frames` | 帧转储：表盘入场、若干时刻、分钟跳变、彗尾 |
| `clock_test` | 表盘页面实测：时基（含 32 位毫秒回绕）、跨午夜与闰年的日历、角度约定、入场组装、彗尾、数字滚动、被覆盖页面仍走时但收不到帧、无残影 |
| `clock_tick_test` | 同一页面在"机械跳动"秒针走法下的全部断言 |
| `clock_tick_frames` | 上述走法的同一组快照 |
| `cgl_iso2d_test` | 投影公式；深度键必须是 `x+y` 而不是 `y`；200 个刚体的排序是真排列且单调；每步只排一次；三种相机风格都留在视口内 |
| `ext_smoke_test` | 模块"作为模块"可用：只包含 `<ext/...>` 并链接 `cytgl_ext`；一步使刚体位移恰为速度×步数；同一步内重复排序被戳记短路 |
| `cgl_bounce_test` | 重力球契约：`vy += g` 恰在积分**之前**、900 步内能量衰减、按身份自愈、非法场地得到惰性球、两个世界逐位独立 |
| `user_storage_test` | 调用者存储形态：`CGL_PHYS2D_STORAGE_BYTES(n)` 与结构体逐字节一致、超容量时 `add()` 返回 -1、被拒绝的存储使世界惰性。该测试**不能**链接 `cytgl_ext`（宏改变结构体布局） |
| `ext_example` | 扩展模块的完整调用序列（9 个刚体、20 步、一次相机拟合、深度排序、绘制到 96×48 字符屏幕），并向 stdout 打印 ASCII 图 |
| `render_test` | 真实场景的逐像素断言：投影后徽标仍可辨认、`COVER_UP` 的新旧页面区域、`GENIE_OUT/IN` 的漏斗轮廓、RESET/栈/暂停。写出 `frame00..frame25` 的 `.ppm` 与 `.bmp` |

**警告不是失败**：只有编译或运行的非零退出码算 FAIL。运行完整套件时若未打开 `CYTGL_BUILD_EXT`，扩展模块的测试不会被注册（构建系统会打印明确的跳过提示）。

### 18.4 快照与检查

渲染类测试会写出可直接打开的图像。写出接口只有两个函数：

```c
void img_dump_ppm(const char *path_fmt, int index, const uint16_t *buf, int width, int height);
void img_dump_bmp(const char *path_fmt, int index, const uint16_t *buf, int width, int height);
```

输入为 **RGB565 行优先**缓冲。PPM 为 `P6` 二进制格式；BMP 为 24 位未压缩、自下而上、BGR、行按 4 字节对齐，宽度超过 1024 时拒绝写出。RGB565 到 8 位通道按高位对齐扩展，因此导出的图像比面板的真实精度略亮——用于检查布局与动画足够。

检查方式：

- `.bmp` 可双击打开（画图、照片查看器）；
- `.ppm`（P6）用 GIMP、IrfanView 或相应的编辑器扩展打开；
- 终端/CI 环境使用 `tools/ppm_view.ps1` 把 PPM 降采样成**字符画**（亮度阶梯 `' .:-=+*#%@'`，亮度取 `0.299R + 0.587G + 0.114B`）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1 -File build/frame02.ppm
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1 -Threshold 200   # 只显示亮点（单像素点阵）
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ppm_view.ps1 -Sample 3 -Width 200
```

参数：`-File`（留空则扫描 `<仓库>/build` 下的全部 `frame*.ppm`）、`-Sample`（每字符对应几个像素，默认 2）、`-Width`（最大字符宽度，默认 170）、`-Threshold`（0..255；大于 0 时只显示块内**最亮**像素达到阈值的块，使单像素点不会被平均掉）。注意 CTest 运行的快照落在 `<构建目录>/build/`，与脚本默认扫描的 `<仓库>/build` 不同，需要 `-File` 指定或先复制。

### 18.5 在硬件上观测

显示设备、场景与导航器是移植层文件内静态变量 `s_board` 的成员；页面及其世界在应用的文件内静态变量 `s_app` 中。调试器可以直接按名字找到两者。

| 观测点 | 含义 |
|---|---|
| `s_board.ctx.screen.clock.now_ms` / `frame` / `dt_ms` | 单调时间 / 帧号 / 本帧 dt（已钳位到 100 ms） |
| `...clock.steps` | 本帧的逻辑步数（正常为 1，卡顿时为 2..4） |
| `...clock.step_total` | 累计逻辑步（不因换页清零） |
| `...clock.accum_ms` | 定步累加器余数（正常 0..15） |
| `...clock.fps` | 最近统计窗口的帧率（前 500 ms 为 0） |
| `s_board.ctx.display.frame_id` / `bands` | 已推送的帧数 / 累计条带数（**整屏一次是 43 条带**） |
| `s_board.ctx.screen.q_dropped` | 因队列满而丢弃的输入事件数（非零 = 事件过于密集） |
| `s_board.nav.depth` / `trans.active` / `trans.trans` | 栈深度 / 转场是否进行中 / 当前转场类型 |
| `s_board.nav.warp.progress_q8` | 变形进度（0 = 原样，256 = 吸成一点） |
| `s_board.nav.transitions` / `req_dropped` | 已完成转场数 / 被拒绝的导航请求数 |
| 页面的 `steps` / `frames` / `paused` | 该页收到的逻辑步数 / 帧数 / 是否被覆盖 |
| 各计数器的读取函数 | `cgl_screen_fps()`、`cgl_screen_dropped_events()`、`cgl_page_nav_depth/transitions/dropped()`、`cgl_widget_alpha_dropped()`、`cgl_widget_children_dropped()`、`cgl_bar_range_rejected()`、`cgl_arc_range_rejected()`、`cgl_bounce_bounces()`、`cgl_bounce_area_rejected()`、`cgl_debris_dot_count()`、`cgl_debris_active_dots()`、`cgl_particles_active()`、`cgl_particles_max_active()`、`cgl_particles_tick()`、`cgl_watch_frames()` |

### 18.6 资源生成工具

**字形表**（TTF → `cgl_font_data.c`）

```bash
python tools/mkfont.py --sizes 8x16 12x24 --threshold 45 \
    --out src/core/cgl_font_data.c --license-out licenses/LICENSE_DEJAVU.txt
python tools/mkfont.py --dry-run          # 只预览，不写文件
```

参数：`--sizes`（`宽x高` 列表，默认 `8x16 12x24`）、`--font`（输入 TTF，缺省自动查找 DejaVu Sans Mono）、`--threshold`（覆盖率百分比，越低笔画越粗，默认 45）、`--out`（默认 `src/core/cgl_font_data.c`）、`--license-out`（默认 `licenses/LICENSE_DEJAVU.txt`）、`--dry-run`。默认值已经指向本仓库中的真实位置，因此上面两条参数可以省略，写出来只是为了明确写入目标。生成 95 个 ASCII 字形（`0x20..0x7E`），4 倍超采样后盒式降采样，位序为 **bit0 = 最左像素**；写完后会重新读回并校验尺寸、字节数与置位数，不一致时以退出码 1 结束。

**点阵徽标**（文字或图片 → 点阵头文件）

```bash
python tools/mklogo.py --text "CytStudio" --font tools/DelaGothicOne-Regular.ttf \
    --size 160 --tracking 2 --align left --dot-threshold 40 --pad-x 12 --out App/logo_bitmap.h
python tools/mklogo.py --image logo.png --invert
python tools/mklogo.py --text "SMASH" --dry-run
```

输出 `logo_bitmap.h`，包含 `LOGO_GRID_W`、`LOGO_GRID_H`、`LOGO_ROW_BYTES`、`LOGO_DOT_COUNT`、`logo_bitmap[]` 与 `logo_bitmap_get()`。位序为 **MSB 优先**，与 `cgl_font_data.c` **相反**，不要混用。工具会读取固件源码中的约束（面板尺寸、`CGL_PHYS2D_MAX_BODIES` 等），超出预算时按 0.94 的步长收缩网格，并做读回校验。

**粒子表**（生成正弦表与倒数表）

```bash
python tools/mkparticletables.py --out include/ext/cgl_particles_tables.h \
                                --src src/ext/cgl_particles_tables.c
```

`--out` 必须显式给出；`--src` 缺省按镜像树推导（`include/ext/x.h` → `src/ext/x.c`）。生成 `cgl_particles__sin_q8[64]` 与 `cgl_particles__recip[384]`（合计 578 字节 Flash），两者都做范围检查。

**表盘默认表**（C 程序，生成后手工粘贴）

```bash
cc -std=c11 -DCGL_CFG_WATCH_DIAL_LUT=0 -Iinclude tools/mkwatchlut.c \
   src/core/cgl_arc.c src/core/cgl_watch.c src/core/cgl_math.c \
   src/core/cgl_canvas.c src/core/cgl_widget.c src/core/cgl_anim.c \
   src/core/cgl_font.c src/core/cgl_font_data.c \
   -o build/mkwatchlut && build/mkwatchlut
```

无参数，向 stdout 打印 `cgl_watch_face_lut_default[CGL_ARC_FACE_LUT_N]` 与 `cgl_watch_well_lut_default[CGL_ARC_RING_LUT_N]` 两个数组，粘贴覆盖 `src/core/cgl_watch_dial.c` 中的对应数组即可。`-DCGL_CFG_WATCH_DIAL_LUT=0` 是必需的（否则默认配置会指向正在生成的这两个数组）。它只是框架自身 `cgl_arc_surface_bake()` 的一层包装，`watch_test` 在运行时用同一个烘焙器逐项复核，因此"表与生成器漂移"不可能发生。

### 18.7 构建系统陷阱

1. **包含路径规则**：不要同时把 `src/core` 与 `include/` 加入全局包含路径（公共头与私有头同名）。框架源码以纯文件名包含自己的头文件，因此只需 `include/` 一个根，它同时服务 `<cytgl/...>` 与 `<ext/...>`。也不要单独加 `-I include/ext` 或 `-I include/cytgl`。
2. **按目录收集源文件是非递归的，移动文件会静默失败**。以目录列表驱动的构建（IDE 工程文件、固件构建脚本）中，一个 `.c` 若移出已列出的目录，构建**完全没有错误**，只是链接不再看到它。必须一起修改的入口有：顶层 `CMakeLists.txt`、IDE 工程文件的源路径与包含路径、固件构建脚本、`tests/CMakeLists.txt`、徽标生成工具。头文件同理（模块源码通过 `<ext/...>` 依赖 `include/ext/`）。**检测手段是逐字节的 `size` 对比，而不是"构建成功"**。
3. **必须跨编译单元一致的宏**：`CGL_CFG_PARTICLES` / `CGL_CFG_DEBRIS` / `CGL_CFG_BOUNCE`、`CGL_PHYS2D_USER_STORAGE`（改变结构体布局）、`CGL_CFG_ALPHA*`、`CGL_CFG_SCREEN_W/H`。把它们放进构建系统，而不是某个文件里的局部定义。
4. **编译期断言**：`cytgl_config.h` 校验不透明度层数与预算、表盘与径向工具箱的依赖关系；`cgl_phys2d.h` 校验参考几何锚点；`cgl_iso2d.h` 与桥校验排序表容量；粒子与重力球各自校验尺寸与参数（例如恢复系数超过 256 会 `#error`）。这些断言的作用是把"参数被设成不可能的值"变成一句构建错误，而不是"效果看起来有点怪"。
5. **数值一致性**：`CGL_PHYS2D_MAX_BODIES` 与 `CGL_ISO2D_MAX_POINTS` 必须匹配；`CGL_CFG_SCREEN_W/H` 必须与面板驱动的几何表、`cgl_screen_init()` 的校验以及位图生成工具一致；`CGL_CFG_CLOCK_STEP_MS` 决定物理速度；字号与条高会影响页面布局与徽标视口推导。
6. `text` 段大小**依赖路径**（HAL 源码里嵌有 `__FILE__`），因此跨目录比较固件体积没有意义，只能在同一目录内比较不同配置。

### 18.8 固件构建、烧录与调试

**参考固件的构建设置**

| 项目 | 取值 |
|---|---|
| 编译器 | `arm-none-eabi-gcc` 14.3（Arm GNU Toolchain，硬浮点，`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16`） |
| 优化 | `-O2` |
| 警告 | `-Wall -Wextra -Wpedantic -Wcast-align`，目标是 **0 错误 / 0 警告** |
| 其他 | `-std=c11`、`-fno-common`、`-fno-strict-aliasing`、`-ffunction-sections`、`-fdata-sections` |
| 宏 | `USE_HAL_DRIVER`、`STM32G431xx` |
| 产物 | `build/CytGL_Demo.elf` / `.bin` / `.hex` / `.srec` / `.map` |

源文件与包含路径都是**目录列表**：生成代码（`Core/Startup`、`Core/Src`、HAL 驱动源码）、本工程目录与 `BSP`、两个示例共用的 `../common/{App,BSP,Devices}`，以及框架的 `src/core`、`src/render`、`src/port` 与可选模块的 `src/ext`。包含路径中只有一个 `include/` 根，它同时服务 `<cytgl/...>` 与 `<ext/...>`。`tests/` 与 `tools/` **不参与固件构建**，它们是宿主侧资源。

**三种构建模式**

| 模式 | 作用 |
|---|---|
| `translate` | 单文件语法检查，最快；改一行代码后的第一道门 |
| `build` | 增量构建 |
| `rebuild` | 全量构建；改动包含路径、宏或链接脚本之后使用 |

命令行等价形式：

```bash
arm-none-eabi-gcc ... -c <每个源文件>              # 文件清单来自工程的源路径设置
arm-none-eabi-gcc ... -T <链接脚本> -o build/CytGL_Demo.elf
arm-none-eabi-size build/CytGL_Demo.elf            # 查看 Flash / RAM 占用
```

**烧录与调试流程**

```
1. 构建                          -> 产出 build/CytGL_Demo.elf
2. 通过 SWD 烧写 .elf            -> flash
3. 连接调试会话（复位并停在入口）  -> attach / start session
4. 运行                          -> run
```

典型调试配置项（以参考工程为例）：调试接口 `SWD`，调试驱动 `DAPLink`（J-Link 亦可），速率 40000 kHz，启动时运行到 `main`，并加载芯片的 SVD 文件以获得外设寄存器视图。

**在硬件上定位问题**时，先在可疑位置下断点看四个数：`frame_id` 是否在增长（总线是否真的在推送）、`bands` 的增量是否为 43（整屏）、`dt_ms` 是否被钳位、`steps` 是否为 1（大于 1 说明本帧发生了卡顿并补步）。第 18.5 节列出了完整的观测点清单。

---

## 19. 完整示例：一个可运行的应用骨架

本节给出一个"从零到可用"的完整骨架：板级端口 + 页面 + 菜单 + 主循环。它省略了芯片外设的初始化细节（那些属于 CubeMX 生成的代码），其余部分可以直接照抄。

### 19.1 板级端口（`cgl_port.c`）

```c
#include "cgl_port.h"                 /* 本板工程自己的头文件；**不要**与本框架的 cgl_port.h 同名头文件冲突 */
#include <cytgl/cytgl.h>
#include <cytgl/cgl_port.h>

/* ---- 1. 面板驱动：实现 cgl_display_drv_t ---- */
static int  panel__init(void *ctx);
static void panel__set_window(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
static void panel__write_pixels(void *ctx, const cgl_color_t *px, uint32_t count);
static void panel__wait_idle(void *ctx);
static void panel__set_backlight(void *ctx, uint8_t percent);

static const cgl_display_drv_t s_panel_drv = {
    .name = "my-panel",
    .init = panel__init,
    .set_window = panel__set_window,
    .write_pixels = panel__write_pixels,   /* 必须异步：启动传输后立即返回 */
    .wait_idle = panel__wait_idle,         /* 阻塞到传输完成，然后释放 CS   */
    .set_backlight = panel__set_backlight, /* 可以为 NULL                  */
    .width = CGL_CFG_SCREEN_W,             /* 必须与编译期配置一致           */
    .height = CGL_CFG_SCREEN_H,
};

/* ---- 2. 时钟：单调毫秒 ---- */
static uint32_t port__tick_ms(void) { return (uint32_t)HAL_GetTick(); }

/* ---- 3. 输入：扫描按键并只投递事件 ---- */
static void port__poll_input(void *user, uint32_t dt_ms);

/* ---- 4. 上电钩子：引脚、总线时钟、背光 PWM ---- */
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

/* ---- 5. 板级对外接口 ---- */
static cgl_port_ctx_t s_ctx;
static cgl_page_nav_t s_nav;

int cgl_port_board_init(void)
{
    /* 应用自己持有导航器：页面必须注册到一个已知的导航器上 */
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

**绝对不要**把 `cgl_port_ctx_t` 里的 `display` / `screen` 拷贝出来：那会复制约 5 KB 的条带缓冲并制造第二个损坏汇。始终通过 `cgl_port_display()` / `cgl_port_screen()` 取用。

工程自己的 `cgl_port.h` **必须使用自己的包含保护宏**（例如 `CGL_BOARD_PORT_H`），不要用 `CGL_PORT_H`——后者属于框架，若同名会使 `<cytgl/cgl_port.h>` 展开为空，`cgl_port_t` / `cgl_port_ctx_t` 随之消失。

### 19.2 输入：一次长按变成两个事件

```c
static void port__poll_input(void *user, uint32_t dt_ms)
{
    (void)user;
    board_key_event_t k = board_key_poll(dt_ms);   /* 去抖与长按计时用传入的 dt */
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

    /* 长按再补一个"返回"：消费 KEY_LONG 的页面把它当作确认，
       不消费的页面让随后的 BACK 到达导航器并弹栈 */
    if (k == BOARD_KEY_LONG)
    {
        ev.kind = (uint8_t)CGL_EV_BACK;
        (void)cgl_port_post_event(&s_ctx, &ev);
    }
}
```

按键状态机完全由传入的 `dt_ms` 驱动（参考实现使用"时长"而非采样计数）：原始电平需稳定 `BOARD_KEY_DEBOUNCE_MS`（20 ms）才被采信；按住时累计时长，达到 `BOARD_KEY_LONG_MS`（600 ms）发出一次长按，此后每 `BOARD_KEY_REPEAT_MS`（200 ms）发出一次重复。因此短按要等到 `UP` 才被判定。

### 19.3 页面与页面表

```c
/* ---------- 页面存储：全部静态；页面持有自己的控件 ---------- */
typedef struct
{
    cgl_page_t  base;
    cgl_panel_t root;
    cgl_label_t title;
    cgl_label_t hint;
} app_page_home_t;

/* 第二个页面同构：首成员为 cgl_page_t，其余为本页控件 */
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

/* ---------- 一个页面 ---------- */
static void home__create(cgl_page_t *page)
{
    app_page_home_t *p = (app_page_home_t *)page;
    const cgl_page_ctx_t *ctx = cgl_page_ctx(page);

    (void)ctx;

    cgl_panel_init(&p->root, "home.root",
                   cgl_rect_wh(0, 0, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H),
                   CGL_CFG_BG_COLOR, CGL_CFG_BG_COLOR);
    (void)cgl_widget_add_child(&p->root.base, &p->title.base);   /* 顺序即绘制顺序 */
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
        /* 只登记请求；真正生效在帧边界的 cgl_page_nav_frame() 中 */
        (void)cgl_page_nav_request(p->base.nav, CGL_NAV_PUSH,
                                   (cgl_page_t *)&s_app.second, CGL_TRANS_AUTO, NULL);
        return true;
    }
    return false;   /* BACK 交给导航器处理 */
}

static const cgl_page_vt_t s_home_vt = {
    .type = "home",
    .on_create = home__create,
    .on_event = home__event,
};
```

```c
/* ---------- 页面表：新增页面 = 一行 ---------- */
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
    s_ctx.app = NULL;                 /* 页面互链表（如有） */

    (void)cgl_page_register(s_pages,
                            (uint8_t)(sizeof(s_pages) / sizeof(s_pages[0])), &s_ctx);

    /* 首页：RESET 没有转场，因此首帧就是稳定的主界面 */
    (void)cgl_page_nav_reset(cgl_port_board_nav(), (cgl_page_t *)&s_app.home);

    (void)cgl_port_board_frame();     /* 一帧：提交 RESET 并画满整屏 */
    cgl_port_board_set_backlight(100u);
    return 0;
}

void app_run(void)
{
    uint32_t last = cgl_port_board_tick_ms();

    for (;;)
    {
        uint32_t now = cgl_port_board_tick_ms();

        if (now == last) { continue; }     /* 每毫秒推进一帧 */
        last = now;
        cgl_port_board_frame();
    }
}
```

`app_run()` 只做节拍控制：它不计算 dt、不做钳位（卡顿保护是时钟的职责，因为时钟还要保护定步累加器），也不包含任何 HAL 头文件——它向**端口**要时间。

### 19.4 上电顺序

移植层与框架的上电顺序只有一条正确路线：

1. 芯片与外设初始化（时钟、引脚、SPI、DMA、可选 PWM），由生成代码提供；
2. `app_init()` → `cgl_port_board_init()`：
   a. 用整屏矩形初始化导航器（应用自持导航器时）；
   b. `cgl_port_bind()` → 依次执行 `board_init` 钩子、绑定显示驱动（几何不符即失败）、创建场景并安装损坏汇；
   c. 复位按键状态机；
3. `cgl_page_register()` 按表创建页面（此时不压栈，也不运行 `on_create`）；
4. `cgl_page_nav_reset()` 安装首页（这一步运行第一次 `on_create`）；
5. **推进一帧**，让 RESET 与首屏绘制生效；
6. 打开背光——放在这一步可以避免用户看到复位与初始化序列。

### 19.5 页面导航的常见形态

- **导航只能由事件触发**：没有空闲定时器、没有脚本。页面在自己的 `on_event` 中登记请求，用 `CGL_TRANS_AUTO` 把转场交给目标页面自己声明。
- **菜单页**通常维护一个名称表与一个目标解析函数，把索引映射到页面指针；目标从 `ctx->app`（互链表）解析，因此"本构建没有该页面"是合法状态（菜单项显示但什么也不做）。
- **返回** 不需要每个页面实现：`CGL_EV_BACK` 在栈深度大于 1 时由导航器直接弹栈。需要"长按返回"的板子只需在移植层把长按翻译成一对 `KEY_LONG` + `BACK`。
- **覆盖层**（半透明卡片、信息页）两步都要做：`cgl_page_set_show_below(page, true)` 加 `cgl_widget_set_alpha(&root.base, a)`。

---

## 20. 故障排查速查表

### 20.1 显示与移植

| 现象 | 可能原因 |
|---|---|
| `cgl_port_bind()` 立即失败 | 驱动报告的 `width/height` 与 `CGL_CFG_SCREEN_W/H` 不一致；或 `board_init` / 驱动 `init` 返回非零；或导航器区域不是整屏 |
| 图像整体固定偏移 | 面板可见区位于更大的 GRAM 内，需要在 `set_window` 中补偏移 |
| 图像撕裂、有移动的水平接缝 | `write_pixels` 实际是同步的，或 CS 释放过早 |
| 颜色错误/通道互换 | 字节序：RGB565 是本机序，检查是否做了多余的字节交换，以及 `COLMOD` 是否为 16 位 |
| 颜色反相 | IPS 面板通常需要 `INVON` |
| 全屏单色（通常是背景色） | 损坏区为空：没有控件调用 `cgl_widget_invalidate()`。调用 `cgl_screen_redraw_all()` |
| 只画了顶部若干行 | 几何不一致：驱动按竖向报告而配置为横向（或相反） |
| 帧率约为总线能力的一半 | `write_pixels` 同步执行，CPU 与总线不再重叠 |
| 缺少约一条带，或图像跳动 | 交给 `write_pixels` 的缓冲在 `wait_idle()` 之前被复用：DMA 未真正结束 |
| 首帧卡住、`frame_id` 停在 1 | SPI 的 DMA 请求/通道配置错误 |
| 随机横向条纹 | 某次推送的条带没有被完整重绘（推给面板的条带必须是完整的） |
| 背光不亮 | 初始背光等级、`set_backlight` 是否为 `NULL`、或 PWM 通道未启动 |
| 按键需要按特别久 | 20 ms 去抖 + 600 ms 长按；短按要等到 `UP` 才判定 |
| 编译期 `_Static_assert` 报 `CGL_CFG_ALPHA_BUDGET` | 分组不透明度所需 RAM 超过预算：降低 `CGL_CFG_ALPHA_LEVELS` / `CGL_CFG_BAND_H`，或有意提高预算 |
| 链接失败并提示某个 `cgl_arc_*` / `cgl_watch_*` 符号缺失 | `CGL_CFG_WATCH=1` 而 `CGL_CFG_ARC=0`（正常应被断言拦下）；或源码列表漏掉了 `src/core/cgl_arc.c` / `cgl_watch.c` / `cgl_watch_dial.c` |

### 20.2 绘制

| 现象 | 可能原因 |
|---|---|
| 文字乱码 | 字形位序读反：框架要求 bit0 = 最左像素，而点阵徽标头文件是 MSB 优先，两者相反 |
| 在平移的子树里矩形跟着动、文字不动 | 使用了 `cgl_canvas_row()` 的快速路径却忘记自行加 `pan` |
| 半透明没有生效 | 没有暂存区（`CGL_CFG_ALPHA=0`）或嵌套层数用尽。查 `cgl_widget_alpha_dropped()` |
| 半透明层透出的是背景而不是下面的页面 | 覆盖层没有 `cgl_page_set_show_below(page, true)` |
| 半透明区域不刷新，残留旧内容 | 该控件清除了 `CGL_WF_CLIP` 并画到 frame 之外，改 alpha 后需要自行再标记一次损坏 |
| 某个控件没显示 | 父控件子数达到 `CGL_CFG_WIDGET_MAX_CHILDREN`，`cgl_widget_add_child()` 返回了 false。查 `cgl_widget_children_dropped()` |
| 残留上一帧内容 | 控件没有填满自己的 frame（框架不清背景） |
| 抗锯齿边缘有硬边或"没有完全覆盖的像素" | 1 像素宽的抗锯齿笔画不存在完全覆盖的像素；需要看起来实心时用 2 像素索取 |
| 笔画两端超出盒子 | 圆头端帽使笔画在两端各多出 `thickness / 2` 像素，需要自行缩短几何 |

### 20.3 页面、转场与时间

| 现象 | 可能原因 |
|---|---|
| 页面切换后画面缺一块 | 控件树在 `cgl_page_nav_frame()` 之外被改动 |
| Genie 吸入时页面只剩上半截 | 剔除用了源 frame 而不是 `cgl_canvas_map_rect()` 的保守包围盒 |
| 漏斗轮廓之外有零散像素 | 裁剪必须落在源空间（`src_clip`） |
| 变形首帧有抖动 | 取整方式错误：行向下取整、列四舍五入 |
| 吸入转场只有几帧/极慢 | 自行逐行扫描的绘制器没有把行走范围绑定到条带的原像（用 `cgl_canvas_src_window()`） |
| 转场期间按键无效 | 输入被锁定，事件被保留到转场结束后投递，并未丢失 |
| 同一帧多次导航只生效一次 | 这是设计：同帧请求只有最后一次生效 |
| 被覆盖的页面仍在步进 | 检查它是否真的是栈顶；只有 `cgl_page_nav_top()` 会收到 `on_step` |
| 返回后物理突然跳几百步 | 出现了追补：确认世界只在一处被步进，且时钟没有被绕过 |
| 页面切换完全没有动画 | 调用者显式传了 `CGL_TRANS_NONE`（它会覆盖页面声明），或该类型的时长为 0 |
| 页面上声明的转场没有生效 | `dur_ms` 为 0 表示"未声明"；要表达瞬间切换必须声明 `trans = CGL_TRANS_NONE` |
| 给吸入配了 `OUT_BACK` 后画面镜像 | `OUT_BACK` 会让进度超过 256，而 Genie 走几何变形路径；滑动类才能用它 |
| 表盘不走时 | 表盘的虚表 `update` 为 `NULL`，页面必须在 `on_frame` 中调用 `cgl_watch_frame(face, clk)` |
| 表盘被覆盖后时间停了 | 不应发生：时间来自 `clk->now_ms`。若确实停了，检查页面是否仍在栈顶（不在栈顶时收不到 `on_frame`） |
| `set_time()` 之后指针没有立刻变化 | 时间设置被暂存，在下一次 `cgl_watch_frame()` 开头生效 |

### 20.4 扩展模块

| 现象 | 可能原因 |
|---|---|
| 徽标模糊/糊成一片 | `CGL_DEBRIS_DOT_PX` 大于投影后的最小点距（相机风格与点径必须成对调整） |
| 点阵碎裂后消失 | `CGL_DEBRIS_DOT_R_Q4` 大于 `CGL_DEBRIS_STEP_Q4 / 2` |
| 伪 3D 无法辨认 | 相机风格与点径不匹配（`ISO_UNIFORM` 下最小点距约 2.16 像素） |
| 球穿过点阵 | 粗筛网格与世界尺寸被改动而未重新推导一致性闸门 |
| 刚体静默缺失 | 池容量不足：`cgl_phys2d_add()` 返回 -1。或 `CGL_ISO2D_MAX_POINTS` 小于真实元素数 |
| 首帧遮挡顺序错误 | 世界重建后没有调用 `cgl_iso2d_sorter_reset()` |
| 位置变了但绘制顺序没变 | 位置变化没有换戳记（`cgl_iso2d_sort()` 在戳记未变时是空操作） |
| 球会盖住它背后的东西 | 有序场景里出现了第二个绘制者：一次 `cgl_debris_view_draw()` 必须画出世界的全部刚体 |
| 粒子飞向屏幕外 | 相机过宽：`CGL_PARTICLES_CAM_EXTENT_Q8` 的上下闸门；或相机推拉振幅过大让深度钳位吃掉远端 |
| 粒子出现"环绕飞回" | 世界坐标超出 Q8.8 的 ±128 像素窗口 |
| `cgl_particles_init()` 返回 `NULL` | 存储小于 `CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX)`，或对齐不足 4 字节 |
| 球粘在地上不再弹 | `CGL_BOUNCE_RESTITUTION_Q8` 过低（每次弹跳损失 `1 - e²`） |
| 球在角落抖动 | 恢复系数大于 256，每次弹跳都在加能量 |
| 球在弧顶越来越高 | `vy += g` 加在了积分之后（应为半隐式欧拉） |
| 球的绘制位置与碰撞位置不一致 | 球被 `cgl_phys2d_reset()` 抹掉后由自愈重建；绘制必须使用不自愈的 const 版本 `cgl_bounce_draw_position_px()` |
| 只链接了模块却没有效果 | 只链接不引用是零开销；要让效果出现必须真正调用它，并注意引擎与页面的存储是否真的一致 |

### 20.5 构建与测试

| 现象 | 可能原因 |
|---|---|
| 编译报 `cgl_screen.h` 之类的符号歧义 | 同时把 `src/core` 与 `include/` 加进了全局包含路径。只保留 `include/` |
| `<cytgl/cgl_port.h>` 展开为空、`cgl_port_t` 未定义 | 工程自己的 `cgl_port.h` 使用了 `CGL_PORT_H` 作为包含保护宏 |
| 移动源文件后固件"构建成功"但行为变了 | 按目录收集源文件的构建是非递归的；移动文件不会报错。用逐字节的 `size` 对比检测 |
| 多个编译单元对某个结构体布局理解不一致 | `CGL_PHYS2D_USER_STORAGE` / `CGL_CFG_PARTICLES` 等构建级宏在不同单元取值不同 |
| CTest 一个测试都没跑 | 多配置生成器（MSVC）需要 `--config` 与 `ctest -C` |
| 找不到快照文件 | 快照写在 `<构建目录>/build/`，而字符画脚本默认扫描 `<仓库>/build`。用 `-File` 指定或复制 |
| 改了字库但界面没变 | 生成的文件不是构建实际编译的那一份：确认 `mkfont.py` 的 `--out` 指向 `src/core/cgl_font_data.c`（默认即是），并确认构建真的重新编译了该文件 |

---

## 附录 A 公共头文件与职责索引

| 头文件 | 职责 | 主要标识符 |
|---|---|---|
| `cytgl/cytgl.h` | 单一入口：一次包含引入全部公共 API；同时定义版本宏 | `CGL_VERSION_MAJOR/MINOR/PATCH`、`CGL_VERSION_STRING` |
| `cytgl/cytgl_config.h` | 全部编译期开关 | `CGL_CFG_*` |
| `cytgl/cgl_types.h` | 颜色、点、矩形、输入事件、唯一混合公式、强制内联宏 | `cgl_color_t`、`cgl_point_t`、`cgl_rect_t`、`cgl_event_t`、`cgl_color_blend_q8()`、`CGL_AINLINE` |
| `cytgl/cgl_math.h` | 整数三角、极坐标映射、范围比例、整数平方根 | `cgl_angle_t`、`cgl_sin_q8()`、`cgl_math_polar()`、`cgl_math_polar_q6()`、`cgl_math_frac_q8()`、`cgl_math_isqrt()` |
| `cytgl/cgl_canvas.h` | 条带画布、图元、变形描述符、覆盖率混合、分层暂存 | `cgl_canvas_t`、`cgl_warp_t`、`cgl_canvas_*()` |
| `cytgl/cgl_font.h` | 点阵字体与文本绘制 | `cgl_font_t`、`cgl_font_8x16`、`cgl_font_12x24`、`cgl_font_draw_text*()` |
| `cytgl/cgl_widget.h` | 控件树、标志、平移/变形/组不透明度、损坏汇、命中测试、事件分发 | `cgl_widget_t`、`cgl_widget_vt_t`、`cgl_widget_*()` |
| `cytgl/cgl_panel.h` | 内置控件：背景/边框容器 | `cgl_panel_t`、`cgl_panel_init()` |
| `cytgl/cgl_label.h` | 内置控件：文本标签（定长缓冲、自制数字格式化） | `cgl_label_t`、`cgl_label_set_text/int/fixed1()` |
| `cytgl/cgl_bar.h` | 内置控件：进度/电平条（任意有符号范围、两种取向、边框、分段、补间） | `cgl_bar_t`、`cgl_bar_*()` |
| `cytgl/cgl_arc.h` | 径向工具箱：覆盖率、渐细指针、着色表面、圆环仪表、不透明度包装 | `cgl_arc_cov_q6()`、`cgl_arc_stroke_*()`、`cgl_arc_needle*()`、`cgl_arc_surface_*()`、`cgl_arc_gauge_*()`、`cgl_arc_draw_*()` |
| `cytgl/cgl_watch.h` | 内置控件：模拟表盘（时基与日历、分度环、三针、轴心、彗尾、玻璃罩） | `cgl_watch_t`、`cgl_watch_config_t`、`cgl_watch_*()` |
| `cytgl/cgl_display.h` | 显示驱动虚表、条带呈现、损坏累积 | `cgl_display_drv_t`、`cgl_display_t`、`cgl_display_present()` |
| `cytgl/cgl_time.h` | 唯一的时钟：帧时间、定步调度、帧率 | `cgl_clock_t`、`cgl_clock_begin/next_step()` |
| `cytgl/cgl_anim.h` | 整数补间与 Q8 缓动曲线 | `cgl_anim_t`、`cgl_ease_t`、`cgl_anim_*()` |
| `cytgl/cgl_page.h` | 页面对象、生命周期、导航栈、转场、页面表 | `cgl_page_t`、`cgl_page_vt_t`、`cgl_page_nav_*()`、`cgl_page_register()` |
| `cytgl/cgl_screen.h` | 显示 + 时钟 + 导航器 + 输入队列 = 一帧 | `cgl_screen_t`、`cgl_screen_frame()` |
| `cytgl/cgl_port.h` | 移植契约：端口描述符、框架侧实例、帧入口 | `cgl_port_t`、`cgl_port_ctx_t`、`cgl_port_bind/frame()` |
| `ext/cgl_ext_basis.h` | 扩展模块共用：Q12.4 约定、强制内联宏 | `CGL_EXT_Q4_SHIFT`、`CGL_EXT_Q4_ONE`、`CGL_EXT_INLINE` |
| `ext/cgl_phys2d.h` | 定点圆盘刚体世界（粗筛 + 求解器 + 2.5D 竖轴） | `cgl_phys2d_t`、`cgl_phys2d_body_t`、`cgl_phys2d_*()` |
| `ext/cgl_iso2d.h` | 等轴测相机与计数排序深度排序器（不认识刚体） | `cgl_iso2d_cam_t`、`cgl_iso2d_sorter_t`、`cgl_iso2d_view_t`、`cgl_iso2d_*()` |
| `ext/cgl_iso2d_phys2d.h` | 两层之间唯一的桥（全内联） | `cgl_iso2d_phys2d_read/src()`、`cgl_iso2d_sort_bodies()` |
| `ext/cgl_particles.h` | Q8.8 粒子云、环绕相机、透视除法表 | `cgl_particles_t`、`cgl_particles_cam_t`、`cgl_particles_*()` |
| `ext/cgl_particles_view.h` | 粒子云的条带绘制器 | `cgl_particles_view_draw_band()` |
| `ext/cgl_particles_tables.h` | 生成的正弦表与倒数表（请勿手工编辑） | `cgl_particles__sin_q8`、`cgl_particles__recip` |
| `ext/cgl_debris.h` | 碎裂场景：位图 → 刚体云、相机拟合、SMASH/SETTLE 循环 | `cgl_debris_t`、`cgl_debris_*()` |
| `ext/cgl_debris_view.h` | 地面、网格、深度排序刚体的绘制器 | `cgl_debris_view_draw()`、`cgl_debris_palette_t` |
| `ext/cgl_bounce.h` | 重力球：重力、四壁、恢复系数、按身份自愈 | `cgl_bounce_t`、`cgl_bounce_*()` |
| `ext/cgl_bounce_view.h` | 重力球绘制器与独立小球控件 | `cgl_bounce_view_draw()`、`cgl_bounce_widget_t` |
| `ext/*_config.h` | 各层/各效果的编译期开关（按层拆分） | `CGL_PHYS2D_*`、`CGL_ISO2D_*`、`CGL_PARTICLES_*`、`CGL_DEBRIS_*`、`CGL_BOUNCE_*`、`CGL_CFG_PARTICLES/DEBRIS/BOUNCE` |

## 附录 B 编译期配置宏索引

**面板与内存**

`CGL_CFG_SCREEN_W`、`CGL_CFG_SCREEN_H`、`CGL_CFG_BAND_H`、`CGL_CFG_ALPHA`、`CGL_CFG_ALPHA_LEVELS`、`CGL_CFG_ALPHA_LEVEL_STRIDE`、`CGL_CFG_ALPHA_BUDGET`、`CGL_CFG_ALPHA_BYTES`（派生）、`CGL_CFG_LABEL_MAX_TEXT`、`CGL_CFG_WIDGET_MAX_CHILDREN`、`CGL_CFG_PAGE_STACK_MAX`、`CGL_CFG_INPUT_QUEUE`。

**径向模块**

`CGL_CFG_ARC`、`CGL_CFG_WATCH`、`CGL_CFG_WATCH_DIAL_LUT`、`CGL_CFG_WATCH_TRAIL`、`CGL_CFG_WATCH_GLASS`、`CGL_CFG_WATCH_CX/CY`、`CGL_CFG_WATCH_R_FACE`、`CGL_CFG_WATCH_R_RING`、`CGL_CFG_WATCH_RING_W`、`CGL_CFG_WATCH_R_WELL_IN/OUT`、`CGL_CFG_WATCH_R_TICK_OUT`、`CGL_CFG_WATCH_R_TICK_HOUR_IN/MIN_IN`、`CGL_CFG_WATCH_R_HOUR/MINUTE/SECOND`、`CGL_CFG_WATCH_R_TAIL`、`CGL_CFG_WATCH_R_HUB`、`CGL_CFG_WATCH_R_HOUR_BASE/MINUTE_BASE`、`CGL_CFG_WATCH_HOUR_W_BASE/TIP`、`CGL_CFG_WATCH_MINUTE_W_BASE/TIP`、`CGL_CFG_WATCH_SECOND_W`、`CGL_CFG_WATCH_SHADOW_PX/SHADOW_ALPHA`、`CGL_CFG_WATCH_TICK_MS`、`CGL_CFG_WATCH_REVEAL_MS`、`CGL_CFG_WATCH_PULSE_MS`、`CGL_CFG_WATCH_TRAIL_MS`、`CGL_CFG_WATCH_SECOND_MODE`。结构常量：`CGL_ARC_FACE_LUT_N`、`CGL_ARC_RING_LUT_N`、`CGL_ARC_COV_N`、`CGL_WATCH_TRAIL_MARKS`、`CGL_CANVAS_INK_OPAQUE`、`CGL_ALPHA_OPAQUE`。

**时间**

`CGL_CFG_CLOCK_STEP_MS`、`CGL_CFG_CLOCK_MAX_STEPS`、`CGL_CLOCK_DEFAULT_MAX_DT_MS`、`CGL_CLOCK_MS_PER_S`。

**页面与转场**

`CGL_CFG_TRANSITION_MS`、`CGL_CFG_GENIE_MS`、`CGL_CFG_TRANSITION_EASE`、`CGL_CFG_GENIE_OUT_EASE`、`CGL_CFG_GENIE_IN_EASE`、`CGL_CFG_S_OUT_EASE`、`CGL_CFG_S_IN_EASE`、`CGL_CFG_GENIE_NARROW_Q8`、`CGL_CFG_GENIE_DROP_PX`、`CGL_CFG_S_AMP_MAX_Q8`、`CGL_CFG_S_DROP_PX`、`CGL_PAGE_FX_KEEP_EASE`、`CGL_PAGE_FX_KEEP_MS`。

**调色板**

`CGL_CFG_BG_COLOR`、`CGL_CFG_FG_COLOR`、`CGL_CFG_ACCENT_COLOR`。

**扩展模块**

物理：`CGL_PHYS2D_SCREEN_W/H`、`CGL_PHYS2D_WORLD_W_PX/H_PX`、`CGL_PHYS2D_WORLD_X0_PX/Y0_PX`、`CGL_PHYS2D_CELL_SHIFT`、`CGL_PHYS2D_MAX_BODIES`、`CGL_PHYS2D_MAX_VELOCITY_PX`、`CGL_PHYS2D_GRAVITY_Z_PX`、`CGL_PHYS2D_LAUNCH_MIN_Q4`、`CGL_PHYS2D_LAUNCH_GAIN_Q8`、`CGL_PHYS2D_LAND_REST_Q8`、`CGL_PHYS2D_LAND_MIN_Q4`、`CGL_PHYS2D_USER_STORAGE`。
投影：`CGL_ISO2D_DEPTH_BUCKETS`、`CGL_ISO2D_MAX_POINTS`、`CGL_ISO2D_COS30_Q8`、`CGL_ISO2D_SIN30_Q8`、`CGL_ISO2D_SQRT3_Q8`、`CGL_ISO2D_LIFT_Q8`。
粒子：`CGL_CFG_PARTICLES`、`CGL_PARTICLES_SCREEN_W/H`、`CGL_PARTICLES_MAX`、`CGL_PARTICLES_BUCKETS`，以及引擎常量 `CGL_PARTICLES_GRID_W/H`、`CGL_PARTICLES_CELL_PITCH_Q8`、`CGL_PARTICLES_WORLD_HALF_SPAN_Q8`、`CGL_PARTICLES_NEAR_Q8/FAR_Q8`、`CGL_PARTICLES_TRIGGER_TICKS`、`CGL_PARTICLES_CAM_PITCH_A/DIST_PX/FOCAL_PX/DOLLY_Q8`、`CGL_PARTICLES_DOT_SPAN_Q8/DOT_BASE_Q8`、`CGL_PARTICLES_STORAGE_BYTES(n)`。
碎裂：`CGL_CFG_DEBRIS`、`CGL_DEBRIS_SCREEN_W/H`、`CGL_DEBRIS_STEP_Q4`、`CGL_DEBRIS_DOT_R_Q4`、`CGL_DEBRIS_DOT_PX`、`CGL_DEBRIS_BALL_R_Q4`、`CGL_DEBRIS_BALL_SPEED_Q4`、`CGL_DEBRIS_BALL_PX`、`CGL_DEBRIS_BALL_START_X_Q4`、`CGL_DEBRIS_BALL_START_Y_OFFSET_Q4`、`CGL_DEBRIS_SETTLE_MS`、`CGL_DEBRIS_EXIT_MARGIN_PX`、`CGL_DEBRIS_SCENE_STYLE`、`CGL_DEBRIS_SCENE_MARGIN_PX`、`CGL_DEBRIS_FLOOR_STEP_PX`、`CGL_DEBRIS_SHADOW_MIN_Z_Q4`。
重力球：`CGL_CFG_BOUNCE`、`CGL_BOUNCE_RADIUS_PX`、`CGL_BOUNCE_GRAVITY_Q4`、`CGL_BOUNCE_RESTITUTION_Q8`、`CGL_BOUNCE_SPAWN_INSET_PX`、`CGL_BOUNCE_SPAWN_VX_Q4`、`CGL_BOUNCE_SPAWN_VY_Q4`、`CGL_BOUNCE_DRAW_R_PX`。

**定点格式**

`CGL_EXT_Q4_SHIFT`（4）、`CGL_EXT_Q4_ONE`（16）、`CGL_PHYS2D_SHIFT` / `CGL_PHYS2D_ONE`（同义）、`CGL_PARTICLES_SHIFT`（8）、`CGL_PARTICLES_ONE`（256）、`CGL_PARTICLES_PX(v)`、`CGL_PARTICLES_MUL(a,b)`、`CGL_PARTICLES_ANGLE_FULL`、`CGL_ANGLE_TURN`、`CGL_ANGLE_QUARTER/HALF/UP`、`CGL_ANGLE_DEG()`、`CGL_ANGLE_FROM_U8()` / `CGL_ANGLE_TO_U8()`、`CGL_ANIM_ONE_Q8`。

## 附录 C 可观测计数器索引

| 读取函数 | 含义 |
|---|---|
| `cgl_screen_fps(s)` | 最近一个统计窗口的帧率（前 500 ms 为 0） |
| `cgl_screen_dropped_events(s)` | 因输入队列满而丢弃的事件数 |
| `cgl_page_nav_depth(n)` | 页面栈深度（1 = 只有首页） |
| `cgl_page_nav_busy(n)` | 是否有转场在播放（输入被锁） |
| `cgl_page_nav_pending(n)` | 是否有尚未提交的导航请求 |
| `cgl_page_nav_transitions(n)` | 已完成的转场数（一次按键应 +1） |
| `cgl_page_nav_dropped(n)` | 因目标非法而被拒绝的导航请求数（非零 = 调用者缺陷） |
| `cgl_widget_alpha_dropped()` | "设置了 alpha 但未生效"的累计次数（单位：每条带 × 每次遍历），正常为 0 |
| `cgl_widget_children_dropped()` | 因父控件子数超限而被拒绝的 `add_child()` 次数，正常为 0 |
| `cgl_bar_range_rejected()` | 被拒绝的条形范围设置次数，正常为 0 |
| `cgl_arc_range_rejected()` | 被拒绝的仪表范围设置次数，正常为 0 |
| `cgl_display_t::frame_id` | 已推送到面板的帧数（**不是**帧率） |
| `cgl_display_t::bands` | 累计推送的条带数（整屏一次为 `CGL_CFG_SCREEN_H / CGL_CFG_BAND_H` 向上取整） |
| `cgl_clock_t::steps` / `step_total` / `accum_ms` | 本帧逻辑步数 / 累计逻辑步（不因换页清零）/ 定步余数 |
| `cgl_particles_active(s)` / `max_active(s)` / `tick(s)` | 当前存活粒子数 / 本轮峰值 / 本轮已跑逻辑步数 |
| `cgl_debris_dot_count(d)` / `active_dots(d)` / `ball_index(d)` | 上次构建放置的点数 / 仍在仿真域内的点数 / 池中重球索引 |
| `cgl_bounce_bounces(b)` / `area_rejected(b)` / `max_speed_q4(b)` | 累计撞墙次数 / 被拒绝的场地次数 / 历史最大速度 |
| `cgl_watch_frames(w)` | 表盘收到的帧调用次数 |
| `cgl_watch_reveal_value(w)` / `pulse_value(w)` | 入场进度 Q8 / 分钟闪光强度 Q8 |
| `cgl_arc_gauge_tip(g)` / `sweep(g)` | 当前扫掠终点 / 当前扫掠长度（角度单位） |

---

## 附录 D 改动定位速查

"我想改 X 应该去哪里"的一览表。路径为仓库内路径；`examples/common/App/` 是示例应用（应用层的写法示范，不是框架的一部分）。

| 我想改 | 去哪里 |
|---|---|
| 默认前景/背景/强调色 | `include/cytgl/cytgl_config.h`（`CGL_CFG_BG_COLOR` 等），以及各页面 `*_create()` 中的配色宏 |
| 屏幕尺寸 / 条带高度 / 栈深度 / 转场时长与曲线 / 时钟步长 | `include/cytgl/cytgl_config.h` |
| **某一个页面**的转场（类型 / 曲线 / 时长） | 该页面自己的 `*_attach()`：`cgl_page_set_trans_enter/leave()`（`RESET` 例外，框架强制瞬间切换） |
| 某个页面的布局与文字 | 该页面自己的 `*_create()` / `*_frame()` |
| 导航图与转场类型 | 页面的 `on_event` 中调用 `cgl_page_nav_request()`；菜单的"索引 → 页面"映射在 `examples/common/App/app_page_menu.c`，显式入口在 `App/app_ui.c` |
| 新增一个控件 | 见第 16.1 节 |
| 新增一个页面 | 见第 13.7 节：一个 `*_attach()` 函数、一个对象成员、页面表里一行 |
| 物理参数 / 点阵几何 | `include/ext/cgl_phys2d_config.h`（可覆盖的 `#ifndef`）与 `include/ext/cgl_debris_config.h` 顶部的几何旋钮 |
| 相机与伪 3D | `include/ext/cgl_iso2d_config.h`（`CGL_ISO2D_*`）与 `CGL_DEBRIS_SCENE_STYLE` |
| 粒子效果：池大小、排序桶、颜色 | `include/ext/cgl_particles_config.h`（`CGL_PARTICLES_MAX`、`CGL_PARTICLES_BUCKETS`）与引擎头文件中的相机/仿真旋钮；颜色是 `cgl_particles_view_draw_band()` 的参数 |
| 碎裂效果：网格、重球、静止时间、相机拟合、调色板 | `include/ext/cgl_debris_config.h`（`CGL_DEBRIS_*`）；调色板是 `cgl_debris_view_draw()` 的参数 |
| 重力球：半径、重力、恢复系数、出生点 | `include/ext/cgl_bounce_config.h`（`CGL_BOUNCE_*`）。**球可以去哪里**属于应用而不是模块（示例中的 `smash__ball_area()`） |
| **不编译**某个效果（省 Flash） | `-DCGL_CFG_PARTICLES=0` / `-DCGL_CFG_DEBRIS=0` / `-DCGL_CFG_BOUNCE=0` 清空该效果的编译单元；正常链接模块的构建只要不引用它就已经是 0 字节 |
| **不声明**某个效果的存储（省 RAM） | 应用侧的效果开关（示例中的 `examples/common/App/app_fx_config.h`）：`APP_FX_PARTICLES=0` 去掉"子弹时间"页；`APP_FX_SMASH=0` 去掉碎裂页**以及它拥有的共享世界**——后者才是大项（该页同时是重力球的唯一使用者，因此还需 `CGL_CFG_BOUNCE=0`） |
| 在没有物理世界的情况下使用 `cgl_iso2d` | 本来就可以：`cgl_iso2d_sort()` 接受一个 `cgl_iso2d_src_t`（一个访问器 + 一个计数），而 `include/ext/cgl_iso2d_phys2d.h` 是"刚体池 → 排序器"唯一的桥 |
| 把物理池放到自己的存储上 / 运行时决定池大小 | `-DCGL_PHYS2D_USER_STORAGE=1` 加 `cgl_phys2d_init_storage()`；默认形态把池放在 `cgl_phys2d_t` 内部，不额外花任何字节 |
| 把可选模块构建/链接到本仓库之外 | 模块是自包含的：`add_subdirectory(.../src/ext)` 加 `cytgl::ext`，或安装产物（头文件落在 `include/ext/`） |
| **链接**可选模块到另一个工程 | `-DCYTGL_BUILD_EXT=ON` 产出 `cytgl_ext` / `cytgl::ext`（`libcytgl_ext.a`），其公共头文件属于模块自身；核心从不列出它，因此不要它就是 0 字节 |
| 徽标 artwork | `python tools/mklogo.py ...`，见第 18.6 节 |
| 引脚 / SPI / DMA / 按键 | 改 `.ioc` 并重新生成，见第 18.8 节与第 4 章 |

---

## 附录 E 提交前自检清单与数值一致性

### E.1 自测四问

1. 宿主渲染测试通过了吗？打开 `frame00.ppm` 之类的新快照，看过新布局了吗？
2. 物理相关测试通过了吗（`logo_test` / `bounce_test` / `cgl_bounce_test` / `cgl_iso2d_test`）？
3. 新控件在 `draw` 中**填满了自己的 frame** 吗？（否则会残留上一帧内容）
4. setter 做到"仅在值真的变化时才 `invalidate`"了吗？（否则每帧多画一整条带）

### E.2 分层四问

1. `src/` 之下是否出现了 `#include "stm32g4xx_hal.h"`？——不允许。
2. `src/` 之下是否出现了具体颜色或具体字号？——不允许（由调用者传入）。
3. 面板驱动是否直接触碰寄存器？——不允许（走总线虚表）。
4. 应用层是否用 `HAL_GetTick()` / `HAL_Delay()` 计算时间？——不允许；时间只经移植层的 `tick_ms` 进入，输入只经 `poll_input` 并携带当帧 `dt` 进入。

### E.3 数值一致性

改动下列任何一项时，必须回头检查右侧对应位置。

| 改动 | 同时需要更新 |
|---|---|
| `CGL_CFG_SCREEN_W/H` | 面板驱动的模式表（可见区偏移与 MADCTL）；`cgl_screen_init()` 的几何校验；位图生成工具的网格上限 |
| `CGL_CFG_CLOCK_STEP_MS` | 物理速度（`CGL_DEBRIS_SETTLE_MS` 不受影响，它按毫秒累加） |
| `CGL_DEBRIS_STEP_Q4` / 点距 / 点径 | `include/ext/cgl_debris_config.h` 顶部的几何注释；位图生成工具推导出的网格上限；三者必须成对调整 |
| `CGL_BOUNCE_GRAVITY_Q4` / `CGL_BOUNCE_RESTITUTION_Q8` / `CGL_BOUNCE_RADIUS_PX` | 示例应用的调参表；`cgl_bounce_test` 打印的弹跳次数与能量（默认值就是为它们选的） |
| `CGL_PHYS2D_MAX_BODIES` | 位图生成工具的亮点预算；RAM 占用（16 B + 2 B 每刚体）；**`CGL_ISO2D_MAX_POINTS`**——它必须不小于池容量，否则桥中的 `_Static_assert` 直接失败（留大只是浪费每个空槽 2 B） |
| `CGL_CFG_LABEL_MAX_TEXT` | RAM 占用（示例中约 25 个标签各自持有一份缓冲） |
| 条高 `BAR_H` / 字号 | 示例应用点数阵卡的布局；位图生成工具的视口推导 |
| 徽标 artwork | 生成的 `logo_bitmap.h` 头部注释（自动生成）；亮点总数（示例中为 480） |
| 新增一个页面 | 应用的页面存储结构与页面表；要让菜单能到达它，还要扩展菜单项数量与"索引 → 页面"映射 |

**经验**：实测数字胜过注释。本工程历史上出现过注释与代码脱节（周期、球速、默认相机风格）。有疑问时先跑测试，然后**把注释一起改掉**。

---

*本手册适用于本仓库当前版本的 CytGL（`CGL_VERSION_STRING` = `0.1.0`）。*
