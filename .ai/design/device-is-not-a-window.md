# 设备不是窗口：`RenderBackend` 的最终形状（2026-10-18，S10b 收口）

> 触发：S10b 落地"一设备 N 窗口"之后，`VsgBackend` 仍然 derive `WindowPresentTarget`，且**第一块窗口的目标就是
> `this`**（S10a 单窗口时代的"三面一体"）。于是窗口 0 是设备、窗口 1..N 是对象——一个只有历史原因的不对称。
> 本文把它收口：**设备持有并发放可呈递目标，但设备不是窗口，也不认识窗口**。

## 0. 结论

| 概念 | 归谁 | 设备的关系 |
| --- | --- | --- |
| 原生窗口（句柄、尺寸、生命周期） | **宿主**（Qt / 测试） | **不认识**。句柄只是目标的创建输入，之后可被 `setWindowHandle` 搬走 |
| 可呈递目标（表面、交换链、present） | **设备造、设备销毁**；区借用 | 设备的**产物**，`Device : Target = 1 : N` |
| 帧 | 只认**目标**（`executeFrame(frame, present_to)`） | 已经是这样；补上 `nullptr` 的真正含义 |

行业同形：Filament `Engine` 持有 `SwapChain`、Qt `QRhi` 持有 `QRhiSwapChain`、OGRE `RenderSystem` 持有
`RenderWindow`（都**不**把窗口概念放进设备）；wgpu 连 Surface 都不放在 Device 上。共同的只有一句：
**设备里放的是"能呈递的东西"，不是窗口**。

## 1. 现状的三处伤害

1. **身份不对称**：`createSurfaceTarget` 有两种返回（`this` / `WindowFacade`），调用方要理解"第一块特殊"。
2. **类型不诚实**：`VsgBackend` 是 `WindowPresentTarget`，于是"设备可以当窗户用"这件事在类型系统里成立，
   而它对第 2..N 块窗口不成立。
3. **`nullptr` 语义含糊**：契约说 `executeFrame(frame, nullptr)` = 不解析、不取像、不 present（离屏是一等形态），
   实现却拿窗口 0 `beginFrame()` **取了交换链图像**，宿主不 present 就永远不还。

## 2. 目标形状

**SDK**

* `RenderBackend` = `RenderDevice` + `RenderFrameExecutor`，**不再**兼 `WindowPresentTarget`。
  契约注释改为："窗口那一半是设备的**产物**（`createSurfaceTarget`），不是设备自己；一个跑帧的对象不需要是窗口。"
* 任何 in-repo 实现与双实现都按这条写（`VsgBackend`、`HeadlessBackendTest::WindowedBackend`；`MockBackend` /
  `StubBackend` 已经是"独立的 target 对象"，不用动）。
* `executeFrame(frame, nullptr)` 的含义写死在契约里：**这一帧没有窗口**——不开帧（不取像）、不 present，
  只按 pass 自己的目标记录。

**插件**

* `VsgBackend`：窗口表 + 设备 + 执行器 + 设备级 clock/queue。**每块窗口（含第一块）都有一个 `WindowFacade`**，
  它转发 `setWindowHandle / nativeHandle / resize / present` 到设备上按索引的入口。
* `createSurfaceTarget(handle)`：只返回 facade（或 `nullptr` + 原因）；同一块窗口第二次 ⇒ `WindowOccupied` 报一次。
* 后端内部只有一条时钟：`d->clock.timeline()`（每块会话都被注入同一个 clock），执行器 / 回收 / 扫除都读它，
  不再经 `session.timeline()` 转一手。
* 帧路径：
  - 有窗口：`session.beginFrame()`（取像）；`facts` 含该窗口的默认帧缓冲行；`presentWindow(i)` 提交并 present。
  - 无窗口（`nullptr`）：`d->clock.begin()`（只取帧号）；`facts` 只有宿主目标行；不 present。
* 设备上**没有** `setWindowHandle / nativeHandle / resize / present`：它们是目标的方法。

**宿主侧**（`SurfaceWindow` 已按 S10a 写好）：不变——它从设备要目标、把句柄/dpr 交给目标。
`RenderEngine::initialize()` 的"无区兜底"仍然向设备要一把目标（`createSurfaceTarget(nullptr)`），语义不变。

## 3. 改动清单

* `sdk/vine/graphics/RenderBackend.hpp`：类注释（两面 + 产物）；不动 C++ 形状（`RenderDevice`/`RenderFrameExecutor`）。
* `sdk/vine/graphics/RenderDevice.hpp`：`createSurfaceTarget` 的注补一句"设备不是它所交出的目标的身份"；
  `executeFrame` 的 `nullptr` 含义补"没有窗口 ⇒ 不开帧"。
* `plugins/gfx_backend_vsg/include/vine/vsg/internal/VsgBackend.hpp`：去 `WindowPresentTarget` 基类；
  删四个兼容入口；`createSurfaceTarget` 只交 facade；文档改。
* `plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`：facade 对**每块**窗口；`executeFrame` 认目标、
  无目标走无窗口路径；`openFrameBooks/closeFrameBooks/present` 按"有没有窗口"分支；时钟统一。
* `tests/test_vsg/TestFrame.hpp` + `VsgBackendTest.cpp`：宿主形状改成"先要目标、再驱动"（机械改写）。
* `tests/test_graphics/HeadlessBackendTest.cpp`：`WindowedBackend` 改成独立目标对象（与 `FleetBackend` 同形）。
* `.ai/memory/graphics.md`、`.ai/design/graphics-window-device-viewer.md`（S10b 行 + §11）：回填。

## 4. 判据与变异

**判据**

1. 全量构建 0 error / 0 warning；`ctest` **24/24**，`test_vsg` 计数不掉（432 = 429 + 3 skip）。
2. 新用例 `VsgBackendTest.ANullTargetRunsTheFrameWithoutAnyWindow`：`executeFrame(frame)` + 不 present ⇒
   `framesPresented()==0`、`windowCount()==0`、无诊断；随后要一把目标并驱动帧，`framesPresented()==1`。
3. 改写后的 `TheSameWindowIsNeverServedTwiceAndADifferentOneGetsItsOwnTarget`：两块窗口拿到**两个不同**的
   facade，且两者都不等于设备指针（类型上也不再是）。
4. 五个 doc/hygiene gate 0 finding（`--strict` 0）。

**变异**

| # | 变异 | 期望 |
| --- | --- | --- |
| M1 | `createSurfaceTarget` 对第一块窗口仍返回 `this` | 3 红（类型/身份不对称回来） |
| M2 | `nullptr` 仍走窗口 0 的 `beginFrame()` | 2 红（无窗口帧取了像） |
| M3 | 无窗口路径仍 present | 2 红 |

## 5. 落地状态（2026-10-18）

**已落地**

* `VsgBackend` **不再 derive `WindowPresentTarget`**：设备在类型上不再是窗口，"设备可以当窗户用"这件事不再成立。
* **每块窗口（含第一块）都有自己的 target 对象**（`detail::WindowFacade`），`createSurfaceTarget` 只有一种返回；
  "同一块窗口两次 ⇒ `WindowOccupied`、不同窗口 ⇒ 服务它"不变。`windowCount()` 是这条的观测面。
* 设备上的 `setWindowHandle / nativeHandle / resize / present` 降级为**非虚便利入口**（窗口 0），文档写明
  "它们本来就是目标的方法，持有目标的宿主在目标上调"。
* 判据：全量构建 0 错 0 警；`ctest` **24/24**；`test_vsg` 计数不变。

**待落地（下一步，改动面已知）**

* `executeFrame(frame, nullptr)` 改成**真的没有窗口**：不 `beginFrame()`（不取像）、不 present、`facts` 只有宿主
  目标行。今天它仍驱动窗口 0（因此仍会取一张交换链图像）。
* 之后删掉那四个便利入口，`RenderBackend` 就只剩"设备 + 帧"。
* 这两步都要改写 `tests/test_vsg/VsgBackendTest.cpp` 的 **85 处**（`setWindowHandle` → `createSurfaceTarget`、
  `present()`/`executeFrame(f)` → 目标上的调用），`TestFrame.hpp` 的示例注释 2 处，以及
  `HeadlessBackendTest::WindowedBackend`（把"返回 `this`"改成独立目标对象，与 `FleetBackend` 同形）。

