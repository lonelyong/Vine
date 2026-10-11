# 窗口 · 设备 · 引擎 · 视图 · 离屏 · 多窗口 —— 目标设计（提案，2026-10-11）

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
             RenderSurfaceTarget       Viewer ──1:N── SceneView ──采纳── Pipeline
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
2. **窗口在 SDK 里只出现在一处**：`RenderSurface`（渲染区）绑定的那个 `RenderSurfaceTarget`。
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
| `RenderSurfaceTarget` | "这块窗口怎么被呈现"：句柄重公告 / 尺寸 / present | ≤ 窗口占用期，可生可灭 | `RenderSurfaceBinding`（S4b 已建） |
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

### Q2 一个窗口只能有一个设备吗？——**不是；硬约束在"窗口 : 在场交换链 = 1:1"**

| 关系 | 基数 | 性质 |
| --- | --- | --- |
| 原生窗口 : **在场** `RenderSurfaceTarget` | **1 : 1** | **平台硬约束**（Vulkan `NATIVE_WINDOW_IN_USE`；DXGI 同理） |
| `RenderDevice` : 窗口 | **1 : N** | 平台允许，且是我们要的（共享显存） |
| 窗口 : `RenderDevice` | N : 1 允许 | 允许但**无收益**（显存/队列各一份，还要抢窗口独占）⇒ 本仓不做 |
| `RenderDevice` : `RenderSurfaceTarget` | 1 : N | 目标属于设备，可生可灭 |

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

### Q5 谁管多视图？——**appfw 的 `Viewer` 管 N 个 `View`；事实留在 `RenderSurface`**（**已落地 2026-10-11：S8**）

- **为什么需要一个"管多个视图"的对象**：预设今天挂在**控件**上 ⇒ "四分屏每格各自预设"只能变成四控件 = 四设备
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

### Q6 设备共享与多 GPU 谁决定？——**应用层策略；SDK 不注册表**

- 同 adapter 的第二个窗口 ⇒ 复用同一个 Device（这才是"几何/贴图只上传一次"的意义）。
- 窗口被拖到另一块 GPU（present 支持是"设备 × surface"的属性）⇒ `createSurfaceTarget` 如实拒绝 ⇒ 应用为
  那个 adapter 建第二个 Device，把那块区的 Target 与 Engine 重新绑上去。
- **SDK 不提供全局设备表**：那会引入进程唯一状态（§12.1 不变量 1）。`Device` 可多实例、可换、可重建。

---

## 4. 契约变化（相对今天）

| 今天 | 目标 | 备注 |
| --- | --- | --- |
| `RenderEngine::setWindowHandle(void*)` + `resize(w,h)` | **退场**；引擎 `setDevice(RenderDevice&)` | 句柄与尺寸归目标与区（**S9 已落前半：两个入口都删了，句柄与尺寸归 `RenderSurface`；`setDevice` 留给 S10**） |
| `RenderBackend`（设备 + 执行器 + 目标，同一个对象） | 拆成 `RenderDevice` / `RenderFrameExecutor` / `RenderSurfaceTarget` 三张契约 | 本文 §2 的三行；`RenderBackend` 今天同时是这三者 |
| `executeFrame(frame)`（隐含"画到那一个窗口"） | `executeFrame(frame, RenderSurfaceTarget* present_to)` | 帧里"默认帧缓冲"那一行解析为它 |
| `Capabilities`（今天只有 `render_targets`） | 加 `presentation` | 设备创建意图的可观察答案 |
| `RenderSurface`（表）+ `RenderSurfaceBinding`（后端半片） | `RenderSurface`（区）+ `RenderSurfaceTarget`（可呈现的半片，**由设备创建**） | 命名对齐；"窗口"在 SDK 只有这一处 |
| 每控件一个后端实例（= 一个设备） | 每控件一个引擎；**设备可由多个区共享** | §7 的 S10 |
| 预设挂在控件 | 预设挂在 `Viewer` 的每一格 | §7 的 S8 |

---

## 5. 生命周期与事件（规则，10 条）

1. **建设备**：`DeviceDesc{ presentation: None | Handle | Probe }`；`None` ⇒ `capabilities().presentation == false`。
2. **第一个窗口通常先于设备**（UI 应用）：句柄作为创建输入（今天的顺序已经如此，只是入口该从引擎搬到设备）。
3. **新增窗口**：`device.createSurfaceTarget(handle)` → 成功则绑给区（`surface.bindTarget(target)`）；失败分三类
   原因如实报出：本设备无呈现能力 / 本设备不支持这块 surface / 窗口已被占用。
4. **同刻一个窗口一个 Target**；第二次创建**拒绝 + 报一次**，不做排队抢占。
5. **窗口重建**：`target.setWindowHandle(new_handle)` ⇒ 重建交换链，**设备与已编译管线保留**（今天的规则）。
6. **窗口关闭**：区解绑、Target 销毁；设备与资源留着（可给下一个窗口用）。
7. **窗口尺寸/比例变化**：`target.resize(w,h,dpr)` → 区扇出 → 视图重算几何（已有）。
8. **present** 归执行器的帧尾；目标只是机制（取像/呈现）。
9. **跨 GPU**：拒绝是**正确行为**，应用换设备（新 Device + 新 Target + 重建该区的引擎与链）。
10. **设备丢失/重建设备**：自下而上重建（Device → Target → Engine → 链），Target 与 Engine 都是可重建的。

---

## 6. 目标形状（基数总表）

| 关系 | 基数 | 性质 |
| --- | --- | --- |
| 窗口 : 在场 Target | 1 : 1 | 平台硬约束 |
| 区（`RenderSurface`）: Target | 1 : 1 | 我们的选择（一区一画面） |
| Device : Target | 1 : N | 平台允许，我们要 |
| Device : Engine | 1 : N | 共享显存/编译 |
| Engine : 区（`RenderSurface` + `Viewer`） | 1 : 1 | 帧的单位 |
| View : Engine | **N : 1** | 多格必须同帧 |
| Viewer : View | 1 : N | 区的视图集合 |
| View : Pipeline | N : 1 允许，默认 1 : 1 | 一条链给两格 = 两格同一幅画（显式） |
| View : Target（经链） | N : M | 离屏目标属于链 |

---

## 7. 落地切片（每片：动作 / 判据 / 变异 / 依赖）

**贯穿判据**（照 S1–S7 的规矩）：每片结束时全量 Release 0 error / 0 warning；六套件**计数不掉**（`test_graphics`
360 / `test_gui` 254 / `test_vsg` 426 = 421 + 3 skip + 2 已知红，外加 appfw 32 / meshio 34 / iobase 94，并按片更新记账）；
五个 gate 0 finding（`--strict` 仍 22 条，同一列表）；**变异 N/N 红**（各自单独实测）；中英文档同步（本文件 + memory + usage）。

| 片 | 内容 | 判据 | 依赖 |
| --- | --- | --- | --- |
| **S8（appfw）** | **已落地 2026-10-11**：`Viewer` 管 N 个 `View`：每格预设（`ViewerOptions`）、建链/采纳/重建搬运行期选择、路由与焦点、`isActive()` / `release(view)`；`RenderControl` 收窄成"原生面 + 会话 + 尺寸/DPR + 帧驱动 + `viewer()`"，老三条保留为"作用于第一格"的转发；`SurfaceWindow` 把 viewer 那一段与状态搬出去 | **实测**：`test_gui` 259（254 + 5 条新用例：同表面两格各自预设〔设备/引擎数不变〕/ 宿主自建 pass 声明后 `isActive() == false`〔声明先落 + 上线后接管两种次序〕/ `release(view)` 后表里没有它 / 重建搬运行期选择 / 两格的路由与焦点）；其余套件计数不掉（graphics 360 / vsg 426 = 421+3 skip+2 已知红 / appfw 32 / meshio 34 / iobase 94）；全量 Release 0 error / 0 warning；五 gate 0 finding。**变异 5/5 红**：重建不搬运行期选择 / 两格共用一份请求 / `release` 不撤声明 / `views()` 不读表 / `takeArea` 不让位 ⇒ 各自单独实测，只有目标用例红 | 无 |
| **S9（graphics 半步，可逆）** | **已落地 2026-10-11**：引擎公开面拿掉裸句柄（`setWindowHandle`/`resize` 删），改由区绑目标（`RenderSurface::bindTarget` / `targetHandle` / `hasTarget`；`RenderSurface::resize` 成为**唯一**的尺寸公告：帧上下文 + 窗口那一半）；契约注释写入 §5 的规则（区域:窗口 1:1、重公告=搬移、解绑、帧尾 present）；`Capabilities` 加 `presentation`（本片由引擎按"后端有没有窗口那一半"答出：插件不动） | **实测**：`test_graphics` **362**（360 + 2：`AReAnnouncedHandleMovesTheWindowAndTheDeviceStays` / `TheAttachHandsOverTheAreasCurrentWindow`；S4b 的三条改形为按区公告，两条尺寸用例改形到区）；无呈现意图的设备上绑目标**被拒 + 报一次**（连报也只一条）；离屏设备上 `executeFrame` 仍成立；其余套件计数不掉（gui 259 / vsg 426 = 421+3 skip+2 已知红 / appfw 32 / meshio 34 / iobase 94）；全量 Release 0 error / 0 warning；五 gate 0 finding（`--strict` 22）。**变异 4/4 红**：① 不拒（无窗口那一半也收下）⇒ `BindingATargetOnAWindowlessBackendIsRefusedOnce` 红；② 不报（拒得静默）⇒ 同条红；③ 引擎仍读自己那份句柄 ⇒ `AReAnnouncedHandleMovesTheWindowAndTheDeviceStays` 红；④ 尺寸不走区 ⇒ 4 条红 | 无（插件不动） |
| **S10（M6 整步）** | 拆 `RenderDevice` / `RenderFrameExecutor` / `RenderSurfaceTarget`；插件把会话提升成设备（窗口与交换链 1:N，`BlockStorage`/`MaterialImages` 上移到设备）；多窗口共享一个设备 | 真设备电池（本机只能跑部分：无头判据先上，设备电池在有 lavapipe 的地方跑并如实记账）；结构判据：两个区共享一个 Device 时只编译一次程序、只上传一份块 | S9；触发条件命中一条（视图数 >4 / 同内容 2+ 窗口 / 启动开销 × 窗口数 / 跨 GPU 拖动） |
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
| `Viewer` 存第二份视图集合/矩形 | 表已经是事实的唯一 owner（S6 的教训） |
| 把 `ViewerOptions` 并进 `PipelineOptions` | 意图（"我要坐标轴"）与接线（program 对象、源相机原始指针、像素比）是两个问题；两个值 + 一处单向映射 |
| SDK 提供全局设备注册表 | 进程唯一状态（§12.1 不变量 1）；设备表是应用策略 |

---

## 9. 待决问题（落地前需要定的）

1. **`RenderSurfaceTarget` 的创建者**：本文选"设备工厂"（`device.createSurfaceTarget(handle)`）。备选是"区自己
   从句柄建"（那要求区能问设备要交换链，等于绕一层）。落地时若插件侧更顺，可改成 `device` 的静态工厂 + 区只收
   结果，语义不变。
2. **`executeFrame` 的目标参数**：本文选"帧名默认帧缓冲 ⇒ 解析为本区的目标"（一帧一个目标）。若将来要"一帧
   多目标"（S11），参数会从"一个目标"变成"帧内的目标表"，届时再改。
3. **`RenderControl` 的老三条（`viewerOptions`/`setViewerOptions`/`setShadingMode`）**：本文选"保留为作用于第一
   格的转发"（不变量 1：公开面不删入口）。**S8 已按这条落地**（转发到 `Viewer` 上"第一格" = 控件自己的视图那组
   入口）—— 库外有调用方（`model_viewer` 的读-改-写），所以三个入口都留着。
4. **`Viewer` 是否导出**（SDK 可见）：本文按"导出"（appfw 的宿主都要用它），若最终只有一个宿主用，可退成内部
   类型。**S8 按这条落地：导出**（`vn::appfw::gui::Viewer`，`RenderControl::viewer()` 把它交出去）。

---

## 10. 与既有文档的关系

| 想知道 | 看 |
| --- | --- |
| 六片重设计（帧的值 / 契约 / 窗口成为可选 / 能力观测 / 宿主侧 / 配方归链） | `graphics-redesign.md` |
| 宿主侧现状（四层 API、四条缝、初始化时机、多视图与多窗口立场） | `appfw-render-view.md`（§12 与本文 §3 Q3/Q5 是同一问题的两份记录） |
| 分层与依赖方向、放置判据 | `graphics-layering.md` |
| 后端实现（会话、交换链、执行器） | `vsg-reimplementation.md`、插件自己的三份 living 文档 |
| **本文（下一轴：窗口/设备/多区/多视图）** | 就是这里；落地一片就把该片移进 `.ai/memory/graphics.md` 与 `.ai/memory/appfw.md` 并回填本表 |
