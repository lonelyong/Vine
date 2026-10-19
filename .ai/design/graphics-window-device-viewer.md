# 窗口 · 设备 · 引擎 · 视图 · 离屏 · 多窗口 —— 目标设计（提案，2026-10-11）

> **2026-10-19 K1 改名**：本文中的 `Pipeline` / `PipelineOptions` 是当时的名字；现行 API 是
> `RenderPipeline` / `RenderPipelineOptions`（见 `graphics-followups-2026-10-19.md` §1.1）。

> 状态：**S8、S9 已落地（2026-10-11），S10–S11 仍为提案**。本文是 S7（配方归 `Pipeline`、视图只采纳）落地之后，用户逐条追问的设计收敛结果：
> "`RenderControl` 职责是不是复杂了？要不要 `ViewerBase/AAAViewer`？" → "Viewer 管理多个 View" → "引擎不该接受
> `WindowHandle`，应该接受 Device？一个 Viewer 一个 Engine 还是一个 View 一个 Engine？" → "无窗口怎么初始化
> Device？新增窗口怎么绑到窗口 surface？"。
>
> **读法**：§1 是骨架，§2 是**现状逐条证据**（file:line），§3 是六个问题的**裁决与理由**（要争论就争这里），
> §4–§6 是目标形状、生命周期与契约变化，§7 是**落地切片**（每片动作/判据/变异/依赖），§8 是否决清单。
>
> 前置与相关：`graphics-redesign.md`（S1–S7 已落地，本文是它后面的下一轴）、`appfw-render-view.md`（宿主侧现状 +
> §12 多视图/多窗口立场）、`graphics-layering.md`（分层与依赖方向）、`graphics-pipeline.md` 与
> `graphics-pipeline-builder.md`（链与 pass 调度）、`vsg-reimplementation.md`（后端实现记录）。
>
> **一句话**：窗口是宿主的；"能呈现"是**目标（交换链）**的；"一份 GPU 与资源"是 **Device** 的；"一帧"是
> **Engine** 的；"一台相机"是 **View** 的；"一个渲染区里有哪几个 View、各自怎么画"是 **Viewer** 的。
> **离屏不是模式**，只是"这一帧没有目标"。

---

## 0. 一页纸

```
   宿主(Qt)                 应用层（组合/策略：哪几格、各自哪个矩形 —— T4 归应用）
   QWindow ──句柄──┐                         │
                   ▼                         ▼
             WindowPresentTarget       Viewer ──1:N── SceneView ──采纳── Pipeline
             （交换链，可生可灭）             │                                  │
                   ▲ 1:1（同刻）               └── 矩形 ── RenderSurface（区的表：谁画/清屏/顺序/尺寸）
                   │ createSurfaceTarget(handle)                               │
             RenderDevice ──1:N── RenderEngine（区的帧：compile → 帧的值）
             GPU/队列/资源/交换链工厂                    │
                   ▲                                    ▼
                   │ 实现                        RenderFrameExecutor
             插件 gfx_backend_vsg                 executeFrame(frame, target?) ── 帧尾 present
                                                        │
                                                   RenderTarget（离屏，可无窗口）
```

**四条不变量**（改设计先改它们，否则会被后来的需求拽回去）：

1. **引擎不认识窗口**：没有句柄、没有尺寸、没有 present；它只认识 Device 与"这一帧"。
2. **窗口在 SDK 里只出现在一处**：`RenderSurface`（渲染区）绑定的那个 `WindowPresentTarget`。
3. **一帧只有一个目标**：一个渲染区一条帧（N 个视图共用），多窗口 = 多渲染区 = 多帧。
4. **SDK 无进程唯一状态**：Device 可多实例、可换、可无呈现能力；谁建谁持，注册表留在应用层。

---

## 1. 现状（逐条证据，全部来自代码）

| # | 事实 | 证据 |
|---|---|---|
| 1 | **1 控件 = 1 窗口 = 1 引擎 = 1 后端实例 = 1 会话 = 1 设备 + 1 交换链** | `Session.hpp` 的类注释："A render session: one window, one device, one swapchain"，`VsgExecutor.hpp`："a frame has one swapchain to present"；`SurfaceWindow` 构造里 `new RenderEngine()` + `new RenderSurface(engine)` |
| 2 | **引擎公开面收裸窗口句柄**（**S9 之前**；S9 起句柄与尺寸归区：`RenderSurface::bindTarget` / `resize`，引擎两个入口都删了） | `RenderEngine.hpp` 的 `setWindowHandle(void*)` 与 `resize(w,h)` <!-- drift-ok --> |
| 3 | 窗口**已经不在帧里**（S3 把平坦路径删了），它现在是后端契约的**可选半片** | `RenderBackend.hpp`：`surface()` 默认 `nullptr`（"没有窗口可绑、没有东西可呈现"） |
| 4 | 窗口**已经是"可移动的句柄"**：Qt 重建平台窗口时**不拆会话**，只重公告句柄，设备与已编译管线保留 | `SurfaceWindow.cpp` 的 `handleDestroyed()` 注释与 `init()` 顺序（**S9 起是 `surface->bindTarget(handle)` → `engine->initialize()`**；S9 之前是 `setWindowHandle(handle)` → `engine->initialize()`，"re-announce so the backend can move"）|
| 5 | **呈现表已经在**：哪个 pass/视图画在哪块矩形、谁清屏、顺序、尺寸、呈现条目 | `RenderSurface.hpp`（S6/S7 收口） |
| 6 | **多视图的缝已经在**：归一化视口、表面矩形即路由表、焦点、每视图自己的相机与场景 | `appfw-render-view.md` §12.2；`SceneView::setViewport`；`RenderSurface::viewAt/focusedView` |
| 7 | **无窗口渲染已经在**：离屏目标 + 手搭 pass（无视图、无窗口） | `SdkPixelTest` 的夹具那条"没有视图也能渲染"；S4b 的可选 `surface()` |
| 8 | 插件的资源存储**归会话**：`BlockStorage` / `MaterialImages` 每会话一份 | 插件 `Device.hpp` / `SessionContent.hpp` 的注释与字段 |

⇒ **结论**：这套目标设计不是"从零发明"，而是把已经存在的三条事实**推到底**：窗口不在帧里（#3）、窗口可移动（#4）、
呈现表已经是区的记录（#5）；再加上一件今天没有的能力：**设备跨窗口/跨区共享**（#1 的直接反面）。

---

## 2. 名词与职责（每个概念只答一类问题）

| 概念 | 答什么问题 | 生命周期 | 今天叫什么 |
|---|---|---|---|
| 原生窗口 | 宿主的窗口 | 宿主说了算，可重建 | Qt 的 `QWindow` |
| `WindowPresentTarget` | "这块窗口怎么被呈现"：句柄重公告 / 尺寸 / present（2026-10-12 起前两条在基类 `PresentTarget` 上） | ≤ 窗口占用期，可生可灭 | `RenderSurfaceBinding`（S4b 已建，S10a 换成它） |
| `RenderDevice` | "一份 GPU 有什么"：队列、资源、能力；**兼交换链工厂** | 最长命，可跨窗口与区 | 后端实例 / 会话 |
| `RenderFrameExecutor` | "这一帧怎么画出来" | 随 Device | `RenderBackend::executeFrame` 那一半 |
| `RenderEngine` | "这一帧画什么、按什么顺序" | = 一个渲染区 | `RenderEngine`（**S9 起不再收句柄与尺寸**；窗口与尺寸归 `RenderSurface`） |
| `RenderSurface` | "这个区由谁画、画在哪、谁清屏、多大" | = 一个渲染区 | `RenderSurface`（表）✓ |
| `Pipeline` | "一个视图怎么画"（一组 pass 与目标） | 随视图 | `Pipeline` ✓ S7 |
| `SceneView` | "一台相机看什么、画在哪个矩形" | 随宿主（多视图共享引擎） | `SceneView` ✓ S7 |
| `Viewer` | "这个区里有哪几个视图、各自怎么画、谁拿输入" | = 一个渲染区 | 今天散在控件与 `SurfaceWindow` |
| `RenderTarget` | 离屏目标（G-buffer / composite / 读回） | 随链 | ✓ 已有 |
| `RenderControl` | "一块窗口渲染区 widget" | 随宿主 | ✓（本文把它的职责收窄） |

---

## 3. 六个问题的裁决（要争论就争这一节）

### Q1 设备该不该认识窗口？——**不"初始化窗口"，但"要不要上屏"必须是创建时的输入**

- **Vulkan 的硬约束**：`VkSurfaceKHR` 必须先于 `vkCreateDevice` 存在（`vkGetPhysicalDeviceSurfaceSupportKHR` 要
  surface），带 present 的队列族在创建设备那一刻定死、**不可逆**。D3D12 没有这个约束，GL 则相反（上下文
  与窗口绑得更死）。
- **因此三种策略**：

  | 策略 | 创建时的输入 | Vulkan | D3D12 | GL | 裁决 |
  | --- | --- | --- | --- | --- | --- |
  | A 严格 | `present = 句柄或探针`；不传 ⇒ `capabilities().presentation == false`，此后永不上屏 | ✔ | ✔ | ✔ | **采用为默认** |
  | B 宽容 | 不传也允许"以后再说"：headless 探针（`VK_EXT_headless_surface`）或猜一个 present 队列族 | ⚠ 猜错则上屏才失败 | ✔ 免费 | ✗ | **可选便利**（T6：删掉它机制路径仍成立），不做默认 |
  | C 两段式 | `initialize()` 之后再 `initializePresentation(target)` | ✗ 不可逆 | ✔ | ✗ | **否决**：契约在 Vulkan 上兑现不了 |

- **推论**：设备创建需要一次"呈现意图"，之后**窗口与设备再无关系**。
- **S9 的半步（已落地 2026-10-11）**：`capabilities().presentation` 已经在了 —— 在设备成为一等公民之前，它的
  答案是"这个后端有没有窗口那一半"（`RenderBackend::surface()`），由引擎答（它同时驱动那一半，宿主读一个值
  而不是问两个对象）；`DeviceDesc{presentation: None|Handle|Probe}` 与"不传就永不上屏"的**创建输入**留给 S10
  （`RenderSurface` 的窗口公告在当前形态下仍可后到：`bindTarget` 在无后端时收下、会话起来时再交出去）。
  **2026-10-20 收口**：S10b 的 `DeviceDesc` 已落地，见 §5 规则 1。

### Q2 一个窗口只能有一个设备吗？——**不是；硬约束在"窗口 : 在场交换链 = 1:1"**

| 关系 | 基数 | 性质 |
| --- | --- | --- |
| 原生窗口 : **在场** `WindowPresentTarget` | **1 : 1** | **平台硬约束**（Vulkan `NATIVE_WINDOW_IN_USE`；DXGI 同理） |
| `RenderDevice` : 窗口 | **1 : N** | 平台允许，且是我们要的（共享显存） |
| 窗口 : `RenderDevice` | N : 1 允许 | 允许但**无收益**（显存/队列各一份，还要抢窗口独占）⇒ 本仓不做 |
| `RenderDevice` : `WindowPresentTarget` | 1 : N | 目标属于设备，可生可灭 |

⇒ 要共享就共享**设备**；要一格一画面就加**视图**；不要"给一个窗口配多个设备"。

### Q3 一个 Viewer 一个 Engine，还是一个 View 一个 Engine？——**一个渲染区一个 Engine，View : Engine = N : 1**

1. **帧的单位是引擎**：`compileFrame()` 产出"这一帧的值"，执行器吃一次。多格的顺序、共享清屏、共享深度、
   跨格插叠层**都靠同一条帧**；`RenderSurface` 的呈现顺序正是按 `RenderEngine::passOrder` 排的。
2. **设备今天跟着引擎走**（#1）：一个 View 一个 Engine = 一个 View 一个设备 = 四格四份显存，与 §12.1 的
   目标态与"四分屏不要用四个控件"直接冲突。
3. **一视图一幅画**：视图采纳的链注册在某个引擎的帧里；表也拒"一个视图呈现另一块表面"（`RenderSurface`）。

### Q4 离屏算什么？——**不是模式，只是"这一帧没有目标"**

同一台引擎、同一份帧的值：带 `RenderTarget` 的行画进离屏目标，名字是"默认帧缓冲"的那一行解析为**本区的
目标**；没有目标就没有解析、没有取像、没有 present。S4b 已经让"窗口"在后端契约里可选（#3、#7），本文只是
把这件事在**设备**这一层也说清（`presentation == false` 是一等形态，不是错误）。

### Q5 谁管多视图？——**先按"一块区 N 个 View"落地（S8），当晚裁决：多格下线，一块区只画一幅画**

- **为什么需要一个"管多个视图"的对象**（S8 的原始动机）：预设今天挂在**控件**上 ⇒ "四分屏每格各自预设"只能变成四控件 = 四设备
  （与 §12.1 冲突）。多格预设今天**无处可放**。
- **为什么它不能变成第二个 owner**：视图集合与矩形的事实已经在 `RenderSurface`（#5）⇒ `Viewer` **读表**拿
  视图集合与矩形，自己不存第二份（S6 的教训）；它只持"**每格的请求**"（`ViewerOptions`）+ 路由策略 + 比例来源。
- **为什么不做布局树**：T4 已定"布局树/比例/join/格子生死归应用层"。`Viewer` 收**归一化矩形**这一句
  （"这四块"由应用说），不提供分割/权重/响应式 ⇒ 它是"面板集合"，不是"布局引擎"。
- **为什么不做基类**：`ViewerBase → AAAViewer/BBBViewer` 没有可覆写的行为（viewer 之间的差别是**值**）；
  "哪种 viewer"的真实扩展点是 `DocumentView` + 插件（`ModelRenderView` / `DemoView` 已经在做）。
- **窗口相关的能力归谁**：`Viewer` 借（引擎、视图、区），**不持**设备/会话/原生面；attach 与 DPR 归控件。
- **落地形态（S8）**：`vn::appfw::gui::Viewer`（`sdk/vine/appfw/gui/Viewer.hpp`）：`addView`（建链 + 采纳 + 在表上
  声明）/ `removeView` = `release` / `views()`（读表）/ `setPaneRect`（转发 `RenderSurface::setViewport`）/
  每格 `setShadingMode` / `viewerOptions(view)` / `setViewerOptions(view, options)` / `isActive()`（表里有宿主自建的
  pass ⇒ false）/ `takeArea()`（attach 时给没有链的格子建链再声明；有宿主 pass 就让位，什么都不建）。
  `RenderControl::viewer()` 把它交出去，老三条（`viewerOptions`/`setViewerOptions`/`setShadingMode`）保留为"作用于
  第一格"的转发。判据与变异见 `.ai/memory/appfw.md` / `.ai/memory/graphics.md` 顶部 S8 条。
- **后续裁决（2026-10-11 晚，推翻上面一半）：多格下线，一块区只画一幅画。** 理由与取舍：
  - **一帧里两条都画默认帧缓冲的 pass 会互相擦**；"谁第一、谁清屏"的合并只有**静默**的默认行为（后端只认第一条公告），
    而"点归谁、谁有焦点"这两件事在真实需求里还没出现（当前只有一块窗口、一幅画面）。
  - 留什么：**"哪条 pass 画这块窗口"仍是显式声明**（`present(SceneView&)` / `present(RenderPass*)`）—— 它防的正是
    §1 那条"全局猜、第二个共用相机的视图被静默丢掉"；**尺寸的唯一 owner**（`resize`）；**目标（窗口）的持有**
    （`bindTarget/target`）。删什么：`present(view, rect)`、`setViewport`、`presentations()`、`Presentation::rect`、
    `viewAt`、`focus`/`focusedView`、`RenderControl::setInputRouter`/`focusedView`、`Viewer` 的多格入口
    （`addView(rect,...)`/`removeView`/`views()`/`setPaneRect`/带 `view` 参数的三个入口）、`SceneView` 自己的矩形
    （`viewport()/setViewport`：区就是它的画面，一个矩形说得清）。
  - 新增的两条硬规则：**一块区只画一幅画**（第二条呈现被拒 + 报一次）；**接管要显式**（先 `viewer()->release(view)`
    再声明宿主自己的 pass，或声明先落再 attach —— 见 `takeArea`）。
  - 要多幅画面的应用**自己说清接线**（自己算矩形、自己给 pass 子视口、自己决定哪条清屏）：见
    `docs/guides/render-views.md` §6。判据与坑见 `.ai/memory/graphics.md` / `.ai/memory/appfw.md` 顶部"多格下线"条。


### Q6 设备共享与多 GPU 谁决定？——**应用层策略；SDK 不注册表**

- 同 adapter 的第二个窗口 ⇒ 复用同一个 Device（这才是"几何/贴图只上传一次"的意义）。
- 窗口被拖到另一块 GPU（present 支持是"设备 × surface"的属性）⇒ `createSurfaceTarget` 如实拒绝 ⇒ 应用为
  那个 adapter 建第二个 Device，把那块区的 Target 与 Engine 重新绑上去。
- **SDK 不提供全局设备表**：那会引入进程唯一状态（§12.1 不变量 1）。`Device` 可多实例、可换、可重建。

---

## 4. 契约变化（相对今天）

| 今天 | 目标 | 备注 |
| --- | --- | --- |
| `RenderEngine::setWindowHandle(void*)` + `resize(w,h)` | **退场**；引擎 `setDevice(RenderDevice&)` | 句柄与尺寸归目标与区（**S9 落前半**：两个入口都删了，句柄与尺寸归 `RenderSurface`；**S10a 收 `setDevice(intrusive_ptr<RenderBackend>)`** —— 今天唯一的一等设备就是那个聚合对象，设备独立成对象后（S10b）改成借用 `RenderDevice&`） |
| `RenderBackend`（设备 + 执行器 + 目标，同一个对象） | 拆成 `RenderDevice` / `RenderFrameExecutor` / `WindowPresentTarget` 三张契约 | **S10a 已落**：三张契约各自一个头文件，`RenderBackend` 退成"三面一体的聚合"（工厂返回它、引擎收它、测试双实现它）；插件今天仍然一体 |
| `executeFrame(frame)`（隐含"画到那一个窗口"） | `executeFrame(frame, WindowPresentTarget* present_to)` | **S10a 已落**（默认 nullptr＝这一帧不指名宿主的目标）：帧里"默认帧缓冲"那一行解析为它，引擎交的是**区**那把目标；`nullptr` ⇒ 不解析、不取像、不 present（离屏是一等形态）。present 仍由帧的驱动者在帧尾经目标调用（`WindowPresentTarget::present`），**2026-10-19 收口：帧尾 present 由执行器调**（驱动者不再调，见 §5 规则 8 / §9 待决 5） |
| `Capabilities`（今天只有 `render_targets`） | 加 `presentation` | **S9 落了字段，S10a 换了答题人**：由**设备**自己答（`capabilities()` 直通），不再是引擎替它算"后端有没有窗口那一半" |
| `RenderSurface`（表）+ `RenderSurfaceBinding`（后端半片） | `RenderSurface`（区）+ `WindowPresentTarget`（可呈现的半片，**由设备创建**；2026-10-12 起帧那半收在基类 `PresentTarget` 上） | **S10a 已落**：改名 `WindowPresentTarget`，`surface()` 退场，改 `RenderDevice::createSurfaceTarget(handle, why)`（设备工厂，§9 待决 1 的裁决）；区持有的是**目标对象**（`RenderSurface::bindTarget`/`target`），句柄只是造它时的输入；三因拒绝 `SurfaceTargetResult`（无呈现 / 这设备服务不了这块 surface / 窗口已被占用），"无呈现"静默答，"占用"报一次 |
| 每控件一个后端实例（= 一个设备） | 每控件一个引擎；**设备可由多个区共享** | **S10a 落了契约面**（Device : Target = 1 : N、Device : Engine = 1 : N 上无第二份副本；无头结构判据已测）；插件把会话提升成设备（缓存上移、窗口 1:N）是 **S10b，已落地（2026-10-18/19，见 §7）** |
| 预设挂在控件 | 预设挂在 `Viewer` 的每一格 | §7 的 S8（已落地） |

---

## 5. 生命周期与事件（规则，10 条）

1. **建设备**：`DeviceDesc{ presentation: None | Handle | Probe }`；`None` ⇒ `capabilities().presentation == false`。
   **2026-10-20 已落**：描述是**创建输入**（`RenderBackendFactory::create(const DeviceDesc&)` → 注册表 →
   `VsgBackend(desc)`），`Probe` 是默认；`None` 由无窗口缝 `createDevice()` 建设备（`windowCount()` 恒 0，
   `createSurfaceTarget` 静默答 `NoPresentation`，帧就是“不命名目标”那种）；`Handle` 没给句柄 ⇒ `initialize()`
   答 false + 一条 `InitFailed`（不静默改成为自己造一块窗口）。
2. **第一个窗口通常先于设备**（UI 应用）：句柄作为创建输入（今天的顺序已经如此，只是入口该从引擎搬到设备）。
3. **新增窗口**：`device.createSurfaceTarget(handle)` → 成功则绑给区（`surface.bindTarget(target)`）；失败分三类
   原因如实报出：本设备无呈现能力 / 本设备不支持这块 surface / 窗口已被占用。**S10a 已落**（`SurfaceTargetResult`；
   "无呈现"静默答，"占用"由设备报一次）。
4. **同刻一个窗口一个 Target**；第二次创建**拒绝 + 报一次**，不做排队抢占。**S10a 已落**（插件仍单窗口：第二把
   一律 `WindowOccupied`；S10b 起按窗口记账）。
5. **窗口重建**：`target.setWindowHandle(new_handle)` ⇒ 重建交换链，**设备与已编译管线保留**（今天的规则）。
   **S10a 已落**（区把新句柄公告在**旧目标**上；引擎不再回放句柄，因为它已经不是句柄的持有者）。
6. **窗口关闭**：区解绑、Target 销毁；设备与资源留着（可给下一个窗口用）。
7. **窗口尺寸/比例变化**：`target.resize(w,h,dpr)` → 区扇出 → 视图重算几何（已有）。
8. **present** 归执行器的帧尾；目标只是机制（取像/呈现）。**S10a 落在"驱动者"这一步**：帧尾 present 经**区那把目标**
   调用（引擎或直接驱动一格的宿主）；**2026-10-19 收口：执行器自持**——`executeFrame(frame, present_to)` 在返回前
   经目标 present（`nullptr` 则只提交），引擎与直接驱动一格的宿主都不再 present（见 §9 待决 5）。
9. **跨 GPU**：拒绝是**正确行为**，应用换设备（新 Device + 新 Target + 重建该区的引擎与链）。
10. **设备丢失/重建设备**：自下而上重建（Device → Target → Engine → 链），Target 与 Engine 都是可重建的。

---

## 6. 目标形状（基数总表）

| 关系 | 基数 | 性质 |
| --- | --- | --- |
| 窗口 : 在场 Target | 1 : 1 | 平台硬约束 |
| 区（`RenderSurface`）: Target | 1 : 1 | 我们的选择（一区一画面） |
| Device : Target | 1 : N | 平台允许，我们要 |
| Device : Engine | 1 : N | 共享显存/编译（契约已落；共享下的**会话归属未定**，见 §9.7） |
| Engine : 区（`RenderSurface` + `Viewer`） | 1 : 1 | 帧的单位 |
| View : Engine | **N : 1** | 多格必须同帧 |
| Viewer : View | **1 : 1**（2026-10-11 晚：多格下线，早先的 1:N 已推翻，见 Q5） | 区的那一条请求 |
| View : Pipeline | N : 1 允许，默认 1 : 1 | 一条链给两格 = 两格同一幅画（显式） |
| View : Target（经链） | N : M | 离屏目标属于链 |

---

## 7. 落地切片（每片：动作 / 判据 / 变异 / 依赖）

**贯穿判据**（照 S1–S7 的规矩）：每片结束时全量 Release 0 error / 0 warning；六套件**计数不掉**（**2026-10-12 起的一套基线**：
`test_graphics` 357 / `test_gui` 257 / `test_vsg` **429 = 426 + 3 skip**（两条"已知红"已修，见 §下一条与 memory 顶条）/ appfw 32 /
meshio 34 / iobase 94，并按片更新记账）；
五个 gate 0 finding（`--strict` 仍 13 条，同一列表）；**变异 N/N 红**（各自单独实测）；中英文档同步（本文件 + memory + usage）。

| 片 | 内容 | 判据 | 依赖 |
| --- | --- | --- | --- |
| **S8（appfw）** | **已落地 2026-10-11；当晚按 Q5 的后续裁决收窄（多格下线，一块区只画一幅画）**：`Viewer` 管这块区的**一条请求**（`ViewerOptions`）、建链/采纳/声明/重建搬运行期选择、`isActive()` / `view()` / `release(view)` / `takeArea()`；`RenderControl` 收窄成"原生面 + 会话 + 尺寸/DPR + 帧驱动 + `viewer()`"，老三条保留为转发；`SurfaceWindow` 把 viewer 那一段与状态搬出去 | **S8 当时的实测**：`test_gui` 259（254 + 5 条新用例：同表面两格各自预设〔设备/引擎数不变〕/ 宿主自建 pass 声明后 `isActive() == false` / `release(view)` 后表里没有它 / 重建搬运行期选择 / 两格的路由与焦点）；graphics 360 / vsg 426 = 421+3 skip+2 已知红；**变异 5/5 红**（重建不搬运行期选择 / 两格共用一份请求 / `release` 不撤声明 / `views()` 不读表 / `takeArea` 不让位）。**收窄后的实测（取代上面这一行）**：`test_gui` **257**（261 − 4：删 3 条路由/分屏用例、两条输入用例合成 1 条）、`test_graphics` **356**（364 − 8：删分屏/矩形/`viewAt`/焦点/清屏合并，新增"一块区只画一幅画"与"区不碰呈现者的清屏策略"）、`test_vsg` **429** 未动（424 + 3 skip + 2 已知红）、appfw 32 / meshio 34 / iobase 94 不掉；全量 Release 0 error / 0 warning；五 gate 0 finding（`--strict` 仍 13 条既有路径漂移）。当时那 5 条变异里仍成立的两条（重建不搬运行期选择 / `release` 不撤声明）在收窄后照旧红，另两条（两格共用一份请求 / `views()` 不读表）随面一起删 | 无 |
| **S9（graphics 半步，可逆）** | **已落地 2026-10-11**：引擎公开面拿掉裸句柄（`setWindowHandle`/`resize` 删），改由区绑目标（`RenderSurface::bindTarget` / `targetHandle` / `hasTarget`；`RenderSurface::resize` 成为**唯一**的尺寸公告：帧上下文 + 窗口那一半）；契约注释写入 §5 的规则（区域:窗口 1:1、重公告=搬移、解绑、帧尾 present）；`Capabilities` 加 `presentation`（本片由引擎按"后端有没有窗口那一半"答出：插件不动） | **实测**：`test_graphics` **362**（360 + 2：`AReAnnouncedHandleMovesTheWindowAndTheDeviceStays` / `TheAttachHandsOverTheAreasCurrentWindow`；S4b 的三条改形为按区公告，两条尺寸用例改形到区）；无呈现意图的设备上绑目标**被拒 + 报一次**（连报也只一条）；离屏设备上 `executeFrame` 仍成立；其余套件计数不掉（gui 259 / vsg 426 = 421+3 skip+2 已知红 / appfw 32 / meshio 34 / iobase 94）；全量 Release 0 error / 0 warning；五 gate 0 finding（`--strict` 22）。**变异 4/4 红**：① 不拒（无窗口那一半也收下）⇒ `BindingATargetOnAWindowlessBackendIsRefusedOnce` 红；② 不报（拒得静默）⇒ 同条红；③ 引擎仍读自己那份句柄 ⇒ `AReAnnouncedHandleMovesTheWindowAndTheDeviceStays` 红；④ 尺寸不走区 ⇒ 4 条红 | 无（插件不动） |
| **S10a（graphics + appfw + 插件半步，已落地 2026-10-11）** | 拆 `RenderDevice` / `RenderFrameExecutor` / `WindowPresentTarget` 三张契约（各自一个头文件；`RenderBackend` 退成"三面一体的聚合"，仍然是工厂返回、引擎收取、测试双实现的类型）；目标改由**设备**造（`RenderDevice::createSurfaceTarget(handle, why)`，`SurfaceTargetResult` 三因：`NoPresentation` 静默答 / `UnsupportedSurface` / `WindowOccupied` **报一次**），区收**对象**（`RenderSurface::bindTarget`/`target`/`hasTarget`，句柄只是创建输入；重建窗口是**在目标上重公告** `setWindowHandle`，不是第二把目标）；`executeFrame(frame, present_to)` 落地并默认 `nullptr`＝不指名宿主目标（帧的"默认帧缓冲"解析为它；离屏＝没有目标）；引擎改 `setDevice`，`capabilities().presentation` 由设备自己答，尺寸的窗口那一半改由**区→目标**直推（引擎只记帧上下文）；插件仍单窗口、缓存不动（会话→设备的提升是 S10b），但 `createSurfaceTarget` 如实拒第二把并报一次、帧命名外来目标也报一次 | **实测**：全量 Release 0 error / 0 warning；`test_graphics` **364**（362 + 2：`ADeviceThatPresentsNothingAnswersNoTargetSilently` / `AWindowAlreadyServedIsRefusedWithItsReasonOnce` / `OneDeviceServesSeveralAreasWithoutCrossingThem`＋两条 S9 用例改形为设备工厂语义，共 7 条在 `HeadlessBackendTest`）；`test_gui` **261**（259 + 2：控件向设备要窗口的目标〔`target_calls == 1`、句柄是创建输入、帧的 `present_to` 就是区那把目标、帧尾 present 走它〕/ 设备"不呈现"⇒ attach 以理由失败）；`test_vsg` **428** = 423 + 3 skip + 2 已知红（+2：`TheOneWindowADeviceServesIsHandedOutOnceAndTheSecondAskSaysWhy`〔第二次问 ⇒ `WindowOccupied` + 一条 Warning，再问仍一条〕/ `AFrameThatNamesATargetTheDeviceDidNotHandOutIsRefusedOnce`〔外来目标：不记录、报一次、自己的目标照常 present〕）；appfw 32 / meshio 34 / iobase 94 不掉；五 gate 0 finding（`--strict` 仍 22）。**变异 4/4 红**（各自单独实测）：① 引擎不认区那把目标（只交自己的）⇒ 3 条红；② 区不把尺寸推给目标 ⇒ `OneDeviceServesSeveralAreasWithoutCrossingThem` 红；③ 插件"窗口已被占用"不报 ⇒ `TheOneWindowADeviceServesIsHandedOutOnceAndTheSecondAskSaysWhy` 红；④ 区已有目标时引擎仍去要一把 ⇒ 4 条红。**坑**：`raw_ptr<T>` 就是裸 `T*`（`RenderSurface::target_` 少了 `= nullptr` ⇒ 非确定性 0xc0000005，与 S9 的 `RenderEngine::area_` 同一课）；`intrusive_ptr<WindowPresentTarget>` 需要该静态类型的 ADL 计数钩子，而目标由各自的设备拥有 ⇒ **借用指针**（设备拥有、区借用），与 `executor()` 一致 | S9；触发条件命中一条（视图数 >4 / 同内容 2+ 窗口 / 启动开销 × 窗口数 / 跨 GPU 拖动） |
| **S10b（插件：会话→设备）—— 已推迟（2026-10-11，登记为后期优化项，等触发）** | **推迟理由**：当前应用只有一块窗口 —— 触发条件（视图数 >4 / 同内容 2+ 窗口 / 启动开销 × 窗口数 / 跨 GPU 拖动）**一条都没命中**，命中再落。**2026-10-18 已落地（1:N）**：`SessionOptions::device` + `detail::adoptDevice`（`VsgHostWindow` 与新增的 `VsgOwnedWindow`）让第二个会话在**同一个 `vsg::Device`** 上渲染；`VsgBackend` 从此是**设备 + 窗口表**——每窗口一个 `api::Session`（表面、交换链、默认帧缓冲目标）与一把目标对象（**每块窗口各一个** `detail::WindowFacade`，设备不是其中任何一块；2026-10-18 收口，见 `device-is-not-a-window.md` §5），`executeFrame` 按 `present_to` 认窗口、`present()` 走该窗口；**`present_to == nullptr` 不再是窗口 0**：它走 `api::OffscreenSession`——无窗口的提交载体（不取像、不 present，帧在 `executeFrame` 返回前完成，槽数学习与完成证据照窗口的规则，见 `device-is-not-a-window.md` §7）。完成证据升到设备级 `core::DeviceClock`（各窗口自己的证据取 **min**，回收队列也只有一处），见 §11 第 3 条。**2026-10-19 补**：执行器自持帧尾 present
已落（§5 规则 8）——`executeFrame` 返回时帧已提交并呈递（没有目标则只提交）。**2026-10-20 补（登记项清空）**：`DeviceDesc{presentation: None|Handle|Probe}` 作为创建输入已落 ——
`RenderBackendFactory::create(const DeviceDesc&)` → `RenderBackendRegistry::create(name, desc)` → `VsgBackend(desc)`；`Probe` 是默认；
`None` 的设备由无窗口缝 `createDevice()` 建（`windowCount()` 恒 0、`capabilities().presentation == false`、
`createSurfaceTarget` 静默答 `NoPresentation`、帧就是“不命名目标”那种），`Handle` 没给句柄 ⇒ `initialize()` 答 false + 一条 `InitFailed`。
实测：`test_vsg` **436 = 433 + 3 skip**（新 2 条）、`test_graphics` **377**（新 1 条）、`ctest` 24/24、六 gate 0 finding；
变异：拿掉 `None` 的 `createSurfaceTarget` 早退 ⇒ 5 条断言红。内容：`VsgBackend` 把 `Session` 的**设备**抽出来成一等设备：窗口与交换链 1:N、`BlockStorage` / `MaterialImages` 上移到设备、时间线/回收随之（现在 `Session` 是"一个窗口、一个设备、一条交换链"，内容世界挂在它那个设备上）；`DeviceDesc{presentation: None\|Handle\|Probe}` 作为创建输入（`None` ⇒ `capabilities().presentation == false`，且 `createSurfaceTarget` 答 `NoPresentation`）；执行器自持帧尾 present（§5 规则 8） | 真设备电池（本机只能跑部分：无头判据先上，设备电池在有 lavapipe 的地方跑并如实记账）；结构判据：两个区共享一个 Device 时只编译一次程序、只上传一份块 | S10a |
| **S11（登记不做）** | 一帧跨多窗口（pass 带目标 + `setSurface(i)`） | — | 只有"一场画面铺满两块屏"这类需求才做 |

---

## 8. 否决清单（免得反复争）

| 否决 | 理由 |
| --- | --- |
| `ViewerBase → AAAViewer/BBBViewer` | 没有可覆写的行为（差别是值）；扩展点已经在 `DocumentView` + 插件；多一层就把"结构 vs 运行期开关"混回虚函数 |
| 一个 View 一个 Engine | = 一个 View 一个设备；四格四份显存；破坏"多格同帧" |
| 一个窗口多个 Device | 无收益（显存/队列各一份）且要抢窗口独占（平台 1:1） |
| 设备两段式初始化（先建后"加上"呈现） | Vulkan 队列族不可逆 |
| 布局树进 SDK | T4：布局归应用层；`Viewer` 只收归一化矩形 |
| `Viewer` 存第二份视图集合/矩形 | 表已经是事实的唯一 owner（S6 的教训）；**2026-10-11 晚起这条更强**：多格下线，区只呈现一个视图，`Viewer` 只有一条请求可存 |
| 把 `ViewerOptions` 并进 `PipelineOptions` | 意图（"我要坐标轴"）与接线（program 对象、源相机原始指针、像素比）是两个问题；两个值 + 一处单向映射 |
| SDK 提供全局设备注册表 | 进程唯一状态（§12.1 不变量 1）；设备表是应用策略 |
| 一对改名 `RenderTargetWindow` / `RenderTargetOffscreen`（2026-10-11 问） | 这两者**不同族**，同前缀会承诺接口没有的对称：`RenderTarget` 是一份**描述**（格式 / 尺寸 / 附件 / depthSource / readback，`HostTargets` 那句"stays a logical description"），`RenderSurfaceTarget` 是一个**活着的可呈递对象**（句柄 / 尺寸 / present，设备造、区借用、可被搬到新窗口）；没有共同基类也不该有。另外 `Offscreen` 在本 SDK 是冗余信息（**所有** `RenderTarget` 都离屏，屏幕那半从来不是它），却要动 944 处（src+tests，72 文件）+ 文档 113 处；`*Window` 又已经是 vsg 后端内部那个会话侧对象的名字。**2026-10-12 按后半句落地**：同族命名只在**呈递端内部**成立 —— `PresentTarget`（帧的契约：`resize` + `present`）是基类，`WindowPresentTarget`（窗口那两条动词）是它的窗口实现，两个名字都不与离屏描述同族 |
| 把 `RenderSurface` 通用化成"离屏 / 云渲染的入口" | 云渲染要的三件东西各有落点，都不是区：① 离屏画面 + 取回像素 = `RenderTarget`（描述）+ pass 的目标 + `RenderDevice::readColorBuffer` / `readDepthBuffer`（**不要窗口、不要区**也能跑完整帧：`executeFrame(frame, nullptr)` + `Capabilities::presentation == false` 是一等状态）；② 远端鼠标 = `SceneView::pushEvent(MouseEvent/ScrollEvent/KeyEvent)`（公开、事件是纯值结构），接网络是**业务层**的事，区已不管输入；③ 分区 = 应用层组合（一块离屏目标上多 pass 各给 `pass->setViewport()`，或一帧多次 copy 分块编码）——"一块区只画一幅画"约束的只是**默认帧缓冲 / 交换链**，离屏目标上多 pass 互不相干。区的定位是"**画面变得可见的那一端**"（窗口 / 嵌入式宿主 / 将来的流出缓冲），不是"渲染的入口"；云端真正缺的是**第二个呈递端**（编码 / 网络）与 `present()` 平级，见 §9 第 6 条 |

---

## 9. 待决问题（落地前需要定的）

1. **`WindowPresentTarget` 的创建者**：本文选"设备工厂"（`device.createSurfaceTarget(handle)`）。备选是"区自己
   从句柄建"（那要求区能问设备要交换链，等于绕一层）。落地时若插件侧更顺，可改成 `device` 的静态工厂 + 区只收
   结果，语义不变。**S10a 按"设备工厂"落地**：`RenderDevice::createSurfaceTarget(void* handle, SurfaceTargetResult*)`
   答三因（无呈现 / 服务不了这块 surface / 窗口已被占用），区收的是一把**借用**的目标对象——所以目标不需要自己的
   引用计数。**2026-10-12 复核收紧寿命口径**（原稿写"窗口关了设备放掉"，与"区里面的借用指针"放一起就是悬垂）：
   目标**活到设备销毁或区解绑**，窗口关闭只是设备自己的 **detach**（句柄没了，此后的 `resize` / `present` 是
   no-op 并如实回答）；`RenderEngine::shutdown()` 在拆设备**之前**把区解绑（`bindTarget(nullptr)`），
   "设备丢失 → 自下而上重建"因此有据可依。判据见 `.ai/memory/graphics.md` 顶部 2026-10-12 条。
2. **`executeFrame` 的目标参数**：本文选"帧名默认帧缓冲 ⇒ 解析为本区的目标"（一帧一个目标）。若将来要"一帧
   多目标"（S11），参数会从"一个目标"变成"帧内的目标表"，届时再改。**S10a 落地并补一条**：参数默认 `nullptr`
   ＝"这一帧不指名宿主的目标"（离屏，不解析/不取像/不 present）；执行器自己拥有的窗口（插件今天那条）不受它影响。
3. **`RenderControl` 的老三条（`viewerOptions`/`setViewerOptions`/`setShadingMode`）**：本文选"保留为作用于第一
   格的转发"（不变量 1：公开面不删入口）。**S8 已按这条落地**（转发到 `Viewer` 上"第一格" = 控件自己的视图那组
   入口）—— 库外有调用方（`model_viewer` 的读-改-写），所以三个入口都留着。
4. **`Viewer` 是否导出**（SDK 可见）：本文按"导出"（appfw 的宿主都要用它），若最终只有一个宿主用，可退成内部
   类型。**S8 按这条落地：导出**（`vn::appfw::gui::Viewer`，`RenderControl::viewer()` 把它交出去）。
5. **`present` 归谁**（S9 遗留 → S10a 的裁决）：`PresentTarget::present()` 是**机制**，帧尾调用它的是**帧的
   驱动者**（引擎，或一个自己驱动一格的宿主）。§5 规则 8 的"执行器自持帧尾 present"与 S10b 的 N 目标一起落，
   否则今天 `executeFrame` 里 present 会让直接驱动者（test_vsg 那 50 处）present 两次。
   **2026-10-19 落地**：N 目标已落（S10b），执行器自持帧尾 present 随之收口——`RenderEngine::frame()` /
   `RenderPass::execute()` 不再 present，test_vsg 的 drive 直接调 `executeFrame`；测试双实现（`test_graphics` /
   `test_gui` 的帧执行器）在 `executeFrame` 的帧尾调 `present_to->present()`，计数因此不变。
6. **第二个呈递端（云渲染 / 远端呈递）**：今天只有一个呈递端 —— 窗口。**2026-10-12 已把它拆成两层**（见 §8 末行）：
   `PresentTarget`（帧的契约：`resize` + `present`）与 `WindowPresentTarget`（窗口那一面：`setWindowHandle` /
   `nativeHandle`），`executeFrame` / `RenderPass::execute` / 引擎的 `own_target_` 只认基类。所以"第二个呈递端"今天
   已经是一个**纯实现问题**：一个只实现 `resize` + `present` 的对象（编码 / 网络出口）就能被帧命名；缺的只是那条实现
   与它的宿主装配。
   **触发条件**（命中一条再落）：① 出现"没有窗口但要把帧交出去"的目标宿主（编码器 / 网络 / 共享内存缓冲），并且它也
   要 handle + resize + present 这四条语义 —— 那时 `handle` 那两条本该落在**它自己那层**（窗口的实现已经这么分了）；
   ② 该宿主需要**多客户端分区**（同一帧切成 N 块各自编码）—— 分区仍在**应用层**（自己算矩形、给 pass 子视口、
   自己决定哪条清屏），SDK 只提供"帧 + 目标 + 呈递端"这三样机制；③ 一帧要同时交给多个呈递端 —— 那要改的是
   `executeFrame` 的参数形态（帧命名一组），不是呈递端契约。
   在那之前不动：今天的离屏路径（`RenderTarget` + readback + `executeFrame(frame, nullptr)`）已经能跑完"渲染但不显示"。

7. **共享设备（`Device : Engine = 1 : N`）下 `initialize` / `shutdown` 归谁**（2026-10-16 补登；**S10b 的前置**；
   **2026-10-18 裁决并落地：形状②**）。
   - **问题（登记时的三条）**：`RenderEngine::initialize()` 调 `device_->initialize()`、`RenderEngine::shutdown()`
     调 `device_->shutdown()`，而设备契约说“已初始化的设备先拆掉上一个会话” ⇒ 一设备两引擎时 **B 的 `initialize()`
     拆掉 A 的会话**、**A 的 `shutdown()` 拆掉 B 还在用的设备**（B 的 `initialized_` 仍为 true，此后每帧静默无事）；
     诊断路线被第二个引擎覆盖（第一个的 `diagnosticCount()` 不再是全量）；无区引擎的“设备自带窗口”兜底只够一个引擎。
   - **裁决**：**会话归创建者，引擎只借用设备**。`RenderEngine::initialize()` 不再 bring up、`shutdown()` 不再
     tear down；设备由建它的那一方驱动（`SurfaceWindow` 在目标的创建输入与尺寸公告之后调 `device->initialize()`，
     平台重建窗口后再调一次就是“跟随新句柄”）。否掉①（引用计数式会话：`initialize()` 的语义在“再次附着”和“第二个
     租户”之间二义，且要加一对没有任何一方知道自己是不是“最后一个”的计数）；否掉③（收紧成 1:1 = 放弃 S10b）。
     ②的代价是宿主显式驱动：115 处 `initialize()` 里生产代码只有 `SurfaceWindow` 一处，其余是测试夹具。
   - **落地**：`RenderDevice` 加 `initialized()`（引擎附着前问“借用来的设备起来了吗”，没起来 ⇒ **一次**报告 + 附着
     失败）；`RenderEngine::setDevice` 顺手把默认内容程序公告给设备（设备由创建者 bring up，程序必须早于那一刻
     到），并在换设备 / 析构时 `removeDiagnosticsRoute`；**诊断路线改成扇出**——设备把每条报告发给**所有**借它的
     路线（`setDiagnosticsRoute` 追加 / `removeDiagnosticsRoute` 撤销），设备自己的 `diagnosticSink()` /
     `diagnosticCount()` 读它自己那条（同样收到全部报告）；插件侧 `Session::wouldKeep(options)` 让
     `VsgBackend::initialize()` 在“同一块窗口、已经起来”时**不丢内容世界**（原来无条件 `releaseContentWorld()`
     ⇒ 每次附着重建全部管线）。
   - **仍未收口**：无区引擎的“设备自带窗口”兜底（`RenderEngine` 的 own-target 分支）在一设备两引擎下只有第一个
     拿得到（第二个 `WindowOccupied`）——那是“设备自己的窗口”只够一个消费者，真要多窗口是 S10b 的 1:N 交换链
     （插件侧），不是这里。判据与变异见 `.ai/memory/graphics.md` 顶部 2026-10-18 条。

---

## 10. 与既有文档的关系

| 想知道 | 看 |
| --- | --- |
| 六片重设计（帧的值 / 契约 / 窗口成为可选 / 能力观测 / 宿主侧 / 配方归链） | `graphics-redesign.md` |
| 宿主侧现状（四层 API、四条缝、初始化时机、多视图与多窗口立场） | `appfw-render-view.md`（§12 与本文 §3 Q3/Q5 是同一问题的两份记录） |
| 分层与依赖方向、放置判据 | `graphics-layering.md` |
| 后端实现（会话、交换链、执行器） | `vsg-reimplementation.md`、插件自己的三份 living 文档 |
| **本文（下一轴：窗口/设备/多区/多视图）** | 就是这里；落地一片就把该片移进 `.ai/memory/graphics.md` 与 `.ai/memory/appfw.md` 并回填本表 |

---

## 11. 行业对照：设备 / 窗口 / 帧驱动 / 删除延迟（2026-10-18）

> 为什么有这一节：§9.7 与 S10b 落地时有"是不是搞复杂了"的质疑。把主流引擎摆在一起看，本仓的形状是**主流那半**；
> 真正多出来的只有"完成证据的粒度"一条，而那是 vsg 的产物，不是模型必然。

| 引擎 | 设备（谁创建 / 谁拥有） | 窗口侧对象 | 帧驱动 | 对象销毁延迟 |
| --- | --- | --- | --- | --- |
| **Filament** | `Engine`（无窗口，应用持 `shared_ptr`） | `createSwapChain(nativeWindow)`，1:N | `Renderer` 借用 Engine，`beginFrame(swapChain)` **点名目标** | driver `destroyDeferred`，**设备级** |
| **Qt RHI** | `QRhi`（应用创建 / 持有） | `QRhiSwapChain`，每窗口一个 | 应用自己，`beginFrame(swapChain)` | QRhi 内部帧，**设备级** |
| **OGRE-Next** | `RenderSystem`（单例） | N 个 `RenderWindow` | `Root::renderOneFrame` 遍历窗口 | 资源管理器，**RenderSystem 级** |
| **Unity / UE** | 进程全局 RHI（模块单例） | 每 `Display` / `FViewport` 一条交换链 | 引擎运行时 | RHI 延迟删除队列，**设备级** |
| **wgpu / Dawn** | `Instance → Surface(每窗) → Adapter → Device` | `Surface::configure` 绑到某个 Device | 应用，`queue.submit` | submission index，**设备级** |
| **Godot 4** | `RenderingDevice`（全局） | DisplayServer 每窗一条交换链 | `RenderingServer` | RD 队列，**设备级** |
| three.js / bgfx | 一个 context / 一个 canvas | — | — | 没有"多窗口共享"这个能力（不算反例） |

**三条读法**：

1. **帧驱动借用设备、不拥有它，是行业默认**（Filament `Renderer`、UE `FSceneRenderer`、Qt 的 `QRhi` 使用者）。所以 §9.7 的 ② 不是新增复杂度，是把一处反常规（引擎驱动设备生命周期，行业里没人这么做）改回主流。
2. **"第一块窗口是设备创建输入"不是本仓的怪癖**：Vulkan 要求 `VkSurfaceKHR` 先于 `vkCreateDevice`、present 队列族在创建设备那一刻定死；Qt RHI 的 Vulkan init params **直接要求一个 `QWindow`**，Filament 的 Vulkan 平台也是先建 surface —— Q1 的裁决就是这条。
3. **唯一多出来的复杂度是"完成证据的粒度"**：上面的引擎把延迟删除做成**设备级**一条（driver `destroyDeferred` / RHI deferred delete / submission index），而 vsg 的 `Viewer` 自己持有 fence 表，所以本仓原先把它做成了**每会话一条 timeline + 一条回收队列**。**2026-10-18 已对齐主流**：`core::DeviceClock` 让帧号归设备、各窗口只报自己的证据、设备水位取 **min**，回收队列也只有一处（见 `.ai/memory/graphics.md` 顶部 2026-10-18 条）。
