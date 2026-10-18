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

* `VsgBackend` **不再 derive `WindowPresentTarget`**：设备在类型上不再是窗口。
* **每块窗口（含第一块）都有自己的 target 对象**（`detail::WindowFacade`）；`createSurfaceTarget` 只有一种返回；
  "同一块窗口两次 ⇒ `WindowOccupied`、不同窗口 ⇒ 服务它"不变，`windowCount()` 是它的观测面。
* **设备上不再有窗口入口**：`setWindowHandle / nativeHandle / resize / present` 已从 `VsgBackend` 删除，
  只剩按索引的机制入口（`presentWindow / resizeWindow / windowHandleAt / setWindowHandleAt`），那几个只由
  facade 调用。直驱的测试用 `DirectDriveBackend`（测试本地适配器）表达"这一帧是哪块窗口"——它们正是宿主的形状。
* 判据：全量构建 0 错 0 警；`ctest` **24/24**；`test_vsg` 计数不变；五个门禁 0 finding。

**待落地：`executeFrame(frame, nullptr)` 的真正含义**

写这一步时发现它**不能单独翻**，原因是 `api::Session` 现在同时是两件事：**一个窗口**（表面 / 交换链）和
**一次提交的载体**（viewer、命令缓冲、fence 表、in-flight 槽数）。没有任何窗口，就没有人把这一帧的命令缓冲区
交给队列——离屏帧也就录不下来。

所以顺序必须是：

1. **先拆会话**：把"提交载体"从窗口里拿出来（设备级 viewer / 命令缓冲池），`api::Session` 只剩窗口那一半；
2. **再翻语义**：`nullptr` ⇒ 用设备的提交载体开一帧（`clock.begin()` 取帧号、不取像、`facts` 只有宿主目标行），
   录完提交，不 present；
3. **再删**`d->driving` 的"无目标时默认窗口 0"这条分支，以及 `VsgBackendTest` 里 `DirectDriveBackend` 的一参
   `executeFrame`（那时测试要显式说明"这一帧没有窗口"）。

在这之前，`nullptr` 仍然驱动窗口 0——**这是已知且写在契约注释里的约定**，不是偶然：一块窗口的设备上
"不呈现"与"没有窗口"今天无法区分，因为提交本身就是靠那条会话做的。

**已落地（2026-10-18，第二片）**：上面的三步按顺序做完了。落地形状与判据：

* `api::OffscreenSession`（`include/vine/vsg/internal/OffscreenSession.hpp` + `src/plugins/gfx_backend_vsg/src/internal/OffscreenSession.cpp`）：
  借用设备与图形队列族，`vsg::Viewer`（零窗口）+ `CommandGraph(device, family)` + 一个 task；`beginFrame()` 只
  `advanceToNextFrame()`（无窗口 ⇒ 不取像）与 `clock.begin()`，`commitFrame()` 提交、**不 present**，并按
  `frame - slots` 向 `DeviceClock::windowCompleted` 报证据、`retirement().advance()` 收尾。槽数学习与窗口会话同源
  （`core::probeSlotCount` / `SlotTracker`）。两个会话共用的两件（不泵事件的 viewer、保留 VkResult 的提交）
  抽到 `SessionDrive.hpp`，避免第二份实现漂移。
* `VsgBackend`：`Data::driving` 用 `kNoWindow` 表示"这一帧没有窗口"；`nullptr` ⇒ 懒建离屏会话、`executor.setWindow(nullptr)`、
  `facts` 不含默认帧缓冲行、录完立即 `commit`（**帧在 `executeFrame` 返回前完成**，因为没有人会替它 present）。
  窗口路径的"记录 + 提交"与离屏路径合并成 `commitFrameThrough(window, offscreen)`，两者只差会话、默认帧缓冲行与
  present。
* `recordContent(...)`：`WindowTarget&` 变成指针；没有窗口时跳过"命名默认帧缓冲"的 pass（这样的 pass 在编译期
  就已因缺少 facts 行被排除），命名了自己目标的 pass 照常录。
* 判据：全量构建 0 错 0 警；`ctest` **24/24**；`test_vsg` 434 例（431 过 + 3 skip）；五个门禁 0 finding。
  新用例 `VsgBackendTest.ANullTargetRunsTheFrameWithoutAnyWindow` 覆盖：不 present、不新增窗口、无诊断、帧确实
  进队列（`progress->submitted`）、离屏帧进设备水位（`progress->completed > 0`），随后窗口帧仍 present 一次。
  变异实测：① `nullptr` 回到窗口 0 ⇒ 该用例红（且离屏电池连夹具都起不来）；② 离屏不报 `windowCompleted` ⇒
  水位断言红。
* 与 §4 判据 2 的一处口径修正：`initialize()` 仍会为"从未要过窗口"的设备起一块自己的窗口（`RenderEngine` 无区
  兜底依赖它），所以"离屏帧后 `windowCount()`"是**不变**而不是 0——用例断言的是"离屏帧不新增窗口"。
  **2026-10-20 补**：这一条只是"要过窗口的设备"的口径。设备**创建时**说了 `DeviceDesc::None`（永不上屏）之后，
  `initialize()` 走无窗口缝 `createDevice()`、一块窗口都不建，`windowCount()` 在整个生命周期里**真的是 0**，
  `createSurfaceTarget` 静默答 `NoPresentation`（`VsgBackendTest.ADeviceCreatedWithNoPresentationRunsWholeFramesWithoutAWindow`）。

## 6. 拆会话：提交载体可以没有窗口（下一片，2026-10-18 设计）

**vsg 的事实（已核对）**：`vsg::CommandGraph` 有三种构造——
`CommandGraph()`、`CommandGraph(ref_ptr<Device>, int family)`、`CommandGraph(ref_ptr<Window>)`。
**中间那种就是"没有窗口的一次提交"**：它不取像、不 present，只把命令缓冲交给设备的队列。

**要拆的不是"一个共享 viewer"，而是"窗口"这一项依赖**。一条进程里每块窗口各有自己的 fence 表（槽数逐窗学习），
硬把它们合成一个 viewer 会让 `viewer->present()` 一次呈现所有窗口，而一帧只该呈现它命名的那一块。所以：

| 对象 | 拥有 | 是否有窗口 |
| --- | --- | --- |
| `api::Session`（现有） | 窗口、交换链、`WindowTarget`、viewer、命令图、槽数学习 | 有 |
| `api::OffscreenSession`（**新增**） | 设备、viewer（零窗口）、`CommandGraph(device, family)`、槽数学习 | **无** |

**`VsgBackend` 的接线**

* 有目标：走该窗口的 `Session`（今天的行为）。
* `nullptr`：**懒建**一个 `OffscreenSession`，用 `d->clock.begin()` 取帧号（不取像），
  `executor.setWindow(nullptr)`，`facts` 只有宿主目标行，录完 `commit`，**不 present**。
* 两条路径共用设备级 `DeviceClock`：`OffscreenSession` 也是一个"窗口"（`addWindow()` 一个 id），
  它自己的 fence 证据照同样规则汇进 min——没有窗口的那一帧，其 GPU 工作同样要被算进水位。

**要动的代码点（估）**

1. `api::OffscreenSession`（新头 + 实现，~150 行，复用 `Session` 的槽数学习与 viewer 装配）；
2. `recordContent(...)` 增加"没有默认帧缓冲目标"的分支（只录命名了自身目标的 pass）；
3. `VsgBackend::executeFrame` 的 `nullptr` 分支 + `d->driving` 改成"窗口索引 or 离屏"；
4. 测试：`DirectDriveBackend` 的一参 `executeFrame` 改成"离屏"语义，新用例
   `ANullTargetRunsTheFrameWithoutAnyWindow`（`framesPresented()==0`、`windowCount()==0`、目标上的像素仍可读回）。

**判据**：全量构建 0 错 0 警；`ctest` 24/24；离屏用例上设备电池可跑；变异：① 离屏帧仍 present ⇒ 红；
② 离屏帧不进 `DeviceClock` ⇒ 水位用例/离屏用例红。

## 7. 执行手册（给下一个会话说"开干"用）

**起点**：`dev` 分支，工作树干净，最后一次提交是本文档的 §6（`57e24cea`）。**先跑一遍**下面第三节的验证，
确认起点是绿的（`ctest` 24/24）。

### 7.1 按顺序改四处

1. **`src/plugins/gfx_backend_vsg/include/vine/vsg/internal/OffscreenSession.hpp`（新）** <!-- drift-ok -->
   - 照 `Session.hpp` 的形状写一个**不带窗口**的会话：`initialize(options, diagnostics)`、`shutdown()`、
     `initialized()`、`beginFrame()`（只 `clock.begin()`，**不取像**）、`commitFrame()`（提交，**不 present**）、
     `slots()` / `framesPresented()` / `deviceWaits()`。
   - 内部：`vsg::Viewer`（同 `EmbeddedViewer`，不泵事件）+ `assignRecordAndSubmitTaskAndPresentation({CommandGraph(device, family)})`
     + 复用 `Session` 的两个既有件：**槽数学习**（`core::SlotTracker` / `probeSlotCount`，从 `task->fence(i)`）
     与**设备级 clock 的证据上报**（`clock->addWindow()` / `setWindowSlots` / `windowCompleted`）。
   - 设备的 `queue_family`：从 `vsg::Device`/`PhysicalDevice` 取（`Session` 里是 `window->getOrCreateDevice()` 之后才有的信息；
     离屏会话拿的是**别人建好的 device**，family 由调用方传入）。
2. **`src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`**
   - `Data` 加 `std::unique_ptr<api::OffscreenSession> offscreen;`（懒建）。
   - `executeFrame`：`present_to == nullptr` ⇒ 不再落到窗口 0，走离屏：`d->driving = kOffscreen`；
     `d->executor.setWindow(nullptr)`；`openFrameBooks()` 用 `offscreen->beginFrame()`；
     `closeFrameBooks()` 的 `facts` 不压窗口行；`presentWindow` 不会被调用（没有人持目标）。
   - `recordContent(...)`（同文件匿名命名空间里那个）：`WindowTarget&` 参数变成指针，`nullptr` 时只录
     **命名了自己目标**的 pass（`recordsWindow` 那类分支保持 false），并跳过窗口目标事实。
   - `growBlockStorageIfNeeded` / `sweepAbandonedContent` / `applyTargetPlans` 已经读设备级 clock，不用动。
3. **`src/plugins/gfx_backend_vsg/src/internal/Session.cpp`**：`d->driving` 的类型从 `std::size_t` 改成
   "窗口索引 | 离屏"（建议 `std::size_t` + `kOffscreen = size_t(-1)`），或干脆在 `VsgBackend` 里存一个
   `api::Session* driving_window` + `api::OffscreenSession* driving_offscreen`——后者更好读。
4. **`tests/test_vsg/`**
   - `VsgBackendTest.cpp` 的 `DirectDriveBackend::executeFrame(frame)`（一参）改成**离屏**语义？**不要**：
     那些用例要的是"这块窗口的一帧"，保持现在（带 target）。
   - 新增 `ANullTargetRunsTheFrameWithoutAnyWindow`：真 backend，`executeFrame(FrameDescription{})`（不带目标）⇒
     `framesPresented()==0`、`windowCount()==0`、诊断干净；再要一把目标驱动一帧 ⇒ `framesPresented()==1`。
   - 设备电池里的离屏像素用例（`SdkPixelTest`）必须仍然绿——它是 `recordContent` 空窗口分支的判据。

### 7.2 验证（一条一条跑，别跳）

```powershell
cmake --build build --config Debug                     # 0 error / 0 warning
ctest -C Debug                                          # 24/24；test_vsg 432 = 429 + 3 skip
python scripts/check_ai_docs.py --strict                # 0
python scripts/check_public_docs.py                     # 0 new / 0 stale（新头要有 @brief/@param/@return）
python scripts/check_include_hygiene.py                 # 0
python scripts/check_export_annotations.py              # 0
```

变异（各单独实测，必须红）：① 离屏帧仍 `present()`；② 离屏会话不上报 `windowCompleted`（水位不动）。

### 7.3 已知坑

* **新源文件要重跑 `cmake -S . -B build`**：`vn_add_library` 的 glob 没有 `CONFIGURE_DEPENDS`。
* **`VsgOwnedWindow` / `adoptDevice` 只对"本后端造的窗口"有效**；离屏会话根本不建窗口，别去碰它们。
* **不要用 `viewer->present()` 做离屏提交**：它按窗口呈现；离屏的提交只走 task 的 submit。
* **水位不动的症状**：`BlockStorage` 的替换永远停不下来（`kept_storage` 增长），或断言"淘汰迟迟不释放"。
  离屏会话必须像窗口一样 `addWindow()` 并上报证据，否则设备的 min 会把水位钉死。
* **`Session::wouldKeep` 的表**不适用于离屏会话（它没有可"保持"的表面）：离屏的 `initialize()` 幂等即可。



