# Graphics HUD / Top Pass（叠加绘制）设计

状态：已随“overlay 类删除 + 引擎单列表统一”重构落地（2026-09），编译/单测全绿。

> **2026-09-16 状态更新（替下面正文里的“已落地”措辞纠偏）**：本文写于叠加层**按相机键保留窗口层**的那一轮，其中两条**已不成立**：
> ① `RenderBackend::releaseWindowLayer`（以及更早的 `releaseOverlay`）**已删除** —— P17 之后“作用域是唯一驱动方式”，后端不再为窗口层保留视图，保留身份是 `SlotKey::ownerPass(pass)`（见 `.ai/design/vsg-pass-lifecycle.md`）；下文 §“配套改动”“已实现文件”“vsg 后端落地”里凡出现 `releaseWindowLayer` / `window_layers[camera]` 的行，读作**当时**的设计，不再是可调用的接口。
> ② “按 `Camera*` 键的 `window_layers` 表”**已不存在**：现在是每个输出目标一张槽表、每个 pass 一个内容槽（`VsgRenderTargetEntry` + `SlotKey`）。
> **仍然成立**：顶部 pass = 普通 pass（`RenderPass` + 引擎单列表）、`CameraMirror` 组件、`AxisGizmo : RenderPass`、`hasWindowPass()`，以及“不要为叠加层单独建第二个 window render pass（CLEAR 会显灰底）”这条结论 —— 这些仍是当前模型的依据。

## 目标

在 graphics 核心提供**通用、后端无关**的“叠加在画面之上”的能力，覆盖不止坐标轴的一种场景：
左下角坐标轴 / 准星 / 小地图 / 屏幕标注 / 拾取高亮 / 水印 等。
约束：多后端兼容（当前 vsg，未来手写 Vulkan/OpenGL）——**叠加内容仍是普通
Scene/Node/Drawable/Material**，后端不需要为叠加层写专属代码。

## 核心抽象：顶部 pass = 普通 pass

**没有独立的 `Overlay` 类**。一个 HUD / 顶部层就是一个普通 `RenderPass`，用
`RenderEngine::addPass(pass, content, order)` 注册到**高于主视图**的 order，
与主场景同处引擎的**单一有序列表**（`slots_`）。原 Overlay 的 4 个正交属性全部落在既有或
新增的最小语义上：

| Overlay 属性 | 现在的落点 |
|---|---|
| 内容 Scene | `addPass(pass, content, order)` 的内容绑定（引擎 slot 持有） |
| 相机 | `RenderPass::setCamera()`；跟随主相机用独立组件 `applyCameraMirror`（见下） |
| 区域（子视口） | `RenderPass::setViewport(x,y,w,h)`，表面变化由 `RenderPass::onSurfaceResized(w,h)` 重排 |
| 合成 | `RenderPass::setClearEnabled(false)`（顶部层不清屏）+ order 决定叠在最后 |
| 显隐 | `RenderPass::setEnabled(bool)`（引擎跳过硬关的 pass） |

配套改动：
- `RenderPass` 新增 `enabled()`/`setEnabled()`、`virtual onSurfaceResized(int,int)`（引擎 resize 时
  对每个注册 pass 调用，默认 no-op）。
- `RenderBackend`：`releaseOverlay` 改名 **`releaseWindowLayer(raw_ptr<const Camera>)`**
  （后端按相机键保留“窗口层”，见下）。
- `RenderEngine`：`addOverlay/removeOverlay/clearOverlays` **删除**；移除即 `removePass/clearPasses`，
  只在“该 pass 真的被移除”后调用后端 `releaseWindowLayer(pass->camera())` +
  `releaseRenderTarget(pass->renderTarget())`（均做非空判断）。新增 `hasWindowPass()`：
  判断是否有“enabled + camera==masterCamera + target==null”的 pass（RenderControl 据此决定是否
  自动补一条默认窗口 pass——只加 HUD pass 不会再把主视图挤掉）。

```cpp
auto gizmo = new AxisGizmo();               // AxisGizmo 现在是 RenderPass 派生
gizmo->setSourceCamera(engine->masterCamera());
engine->addPass(gizmo, 10);                 // 画在 order-0 窗口 pass 之上
gizmo->setEnabled(false);                   // 需要时隐藏
```

## 相机跟随 = 独立镜像组件（CameraMirror）

“谁跟随谁”不再属于叠加层：`sdk/vine/graphics/CameraMirror.hpp` 提供命名空间级
`enum class MirrorMode { None, Orientation, FullView }` 与自由函数
`applyCameraMirror(dst, src, mode)`，只操作两个 `Camera`，与 pass/引擎无关，可随处驱动：
- `Orientation`：只跟随源相机朝向、保持目标自身取景距离（坐标轴用）。
- `FullView`：完全采用源 eye/target/up（画中画 / VR 眼，预留）。
- `None`：不动。

`AxisGizmo::execute()` 每帧先 `applyCameraMirror(camera_, source_camera_, Orientation)`
再画自己的内容，因此“跟随”在绘制时生效，源相机取 `masterCamera()` 裸指针（非拥有）。

## 已实现文件（新形态）

- `sdk/vine/graphics/CameraMirror.hpp` / `src/CameraMirror.cpp`（镜像组件 + 单测）
- `sdk/vine/graphics/AxisGizmo.hpp` / `src/AxisGizmo.cpp`——**`AxisGizmo : public RenderPass`**
  自包含 HUD pass：owned framing `camera_` + owned 内容 `content_`（三根红X/绿Y/蓝Z 方柱，复用
  IndexedTriangleMesh）+ 左下角子视口；`execute()` 忽略引擎传入 scene，先镜像再画自己的内容。
  不再有 `Overlay::MirrorMode`（用命名空间 `MirrorMode`）与 `Overlay::pass()`（自身即 pass）。
- `RenderPass.hpp/.cpp`：viewport / clearEnabled / enabled / onSurfaceResized
- `RenderBackend.hpp`：`setViewport` no-op、`releaseWindowLayer`
- `RenderEngine.hpp/.cpp`：单列表 + hasWindowPass + 非空释放守卫
- `RenderControl.cpp`：默认窗口 pass 自动补足条件 `passCount()==0` → `!hasWindowPass()`
- `AppShellUi::addAxisGizmo()`：`setSourceCamera(masterCamera())` + `addPass(gizmo, 10)`
- 测试 `tests/test_graphics/GraphicsTest.cpp`：HudPassTest(2)（顺序/隐藏、子视口+清屏策略）、
  CameraMirrorTest(2)、AxisGizmoTest(3)，与释放闭环测试改写为 pass 语义。

## vsg 后端落地（保留的关键结论，措辞已随 window_layers 统一）

`VsgRenderer` 用 **`window_layers`（键 `Camera*`）** 一张表替代“主视图 vs overlay 特判”：
每个窗口层 = `vsg::Camera + root + light_group + view + SceneBridge + on_top`。
- `render()` 只做“同步 + 编译”，**提交推迟到 `swapBuffers()`**（submitFrame：record+submit+present
  一次）→ 各窗口层一帧只 present 一次。
- `setViewport(x,y,w,h)` override：记录 pending 子视口，由下一次 `render()` 消费；无子视口 = 全屏。
- **不要为叠加层单独建第二个 window `RenderGraph`/render pass**——CLEAR 会显示灰底，且 View 内容
  从不光栅化。正确做法 = **主 RenderGraph 里的额外 `vsg::View`**（官方多视口范式，同一 render pass）；
  顶部层相机 `viewportState` 每帧设为子矩形 → 内容被裁剪/映射到该子区并画在主场景之上。
- 每窗口层用**独立 `SceneBridge`** + 顶部层用关深度测试/写入的 `on_top_shader_set`
  （`makeContentShaderSet(..., depth_test=false)`）→ 轴永远在最上层，不被场景几何遮挡。
- 顶部层视图只放一个 `AmbientLight`（intensity 1），**不要**用定向头灯——定向光方向固定，
  镜像相机转到对角线（如 (1,1,1)）时面法线·光为负 → 轴发黑。环境光下 phong
  `ambientColor = diffuse*ambient*ambient.a`，与面朝向无关 → 恒纯色。
- `AxisGizmo` 必须**持有**自己的 framing 相机（owned `camera_`），不能传局部 `intrusive_ptr`
  裸指针给 pass（构造后即析构 → 悬空 → mirror 每帧写已释放内存 → eye=NaN/inf 相机）。
- `RenderEngine::initialize()` 在 `backend->initialize()` 成功后**预执行一次 enabled 且不清屏的 pass**
  （warm-up），让顶部层内容在首帧前于“与主内容一致的可靠上下文”编译（帧内运行时编译几何曾
  “proven unreliable”；保留 warm-up 作为保险）。
- 观感：轴默认 framing 距离 3.3（长度 1 的柱约占子框半宽 73%）、半厚 0.09（约 3px）、
  材质 diffuse=颜色 + ambient=白 + specular=黑（平板纯色、无白色高光）。
- 代价：单 pass 多视图无法给顶部层做独立清屏 → **无灰色不透明底块**（轴直接叠在 3D 场景上），
  用户已接受。残影/深度穿帮由深度关闭解决。
- 释放：`releaseWindowLayer(camera)` 摘除 `window_layers[camera]` 的 View、deviceWaitIdle、
  清该层 bridge 缓存后 erase；若移除的是主层（camera==impl->camera）同时清空公开别名
  `vsg_camera/vsg_scene`（下帧 render 会经 `setupWindowLayer` 按需重建并重设别名）。

## 为什么 Camera 不引入 master/slave

`Camera` 保持叶子矩阵对象（eye/target/up + 投影），不加父子关系。
“谁跟随谁”由独立 `applyCameraMirror` 运行时装配表达，避免把视图关系耦合进 Camera
（aspect/manipulator/resize 均不受影响）。真正的“同视图多投影”（VR 双眼 / 环绕多屏）
留作高层的 `View` 概念，届时再引入，`MirrorMode::FullView` 已为其预留扩展点。

## 环境备注

- `vn_add_library` 用 `file(GLOB_RECURSE)`：新增/删除 .cpp/.hpp 后需 `cmake.configure` 重新配置
  才会纳入（删除文件不重配会让 ninja 报“No rule to make target”）。
- 若 configure 因残留 `vsg_FOUND:INTERNAL=TRUE` 走错 "installed vsg" 分支：
  删除该行后 `cmake.configure`（强制）即回到 FetchContent vsg（`_deps/vsg-src`）。

## 一个 pass 画进哪块矩形（2026-09-13）

- 事实：内容槽与程序 pass **各有一份** clamp/回退的算术，而且对同一个问题给出不同答案——内容路径忽略"presenting pass"的 viewport，程序路径永远尊重它。
- 收口：几何与"角色"合并进一个函数 `detail::passDrawRect(viewport, fills_target, w, h)`（`VsgBackendUtility`），两条路径共用；`RenderPass::setViewport` 的文档只留一条规则：**公告的矩形，未公告则整目标，clamp 进目标；device 像素、左上原点**。**清屏是另一件事**，故意不并进这条规则：clear 覆盖整个目标，draw 只在矩形内——这正是 PiP 模式（铺满目标 + 角落画预览）能成立的原因。
- 为什么"填满"这个角色不能靠"clear 过"判定：把程序路径改成 `state.request.presenting` 之后，既有的 PiP 相位立刻变红——`the PiP changed 36864 pixel(s), expected exactly 5184 (the sub-rectangle)`。一个**清了屏的离屏** pass 什么也没 presenting。
- **当时的折中**：先加了一个显式 `fills_target` 参数，把"两个同名标志不同义"写在签名上（内容槽的角色来自槽，程序 pass 只有请求里"清过屏"的标记）。
- **收口（同日，那个参数彻底删掉）**：证据指出这条规则**没有生产者**——`AxisGizmo` / `FpsOverlay` 都是**内容** pass + 子矩形，但 `setClearEnabled(false)`（本来就不受这条规则影响）；`RenderPipelineBuilder` 里唯一的 `setViewport` 是 PiP 的 `ScreenPass`；`addOffscreenToScreen` 的离屏内容 pass 没有矩形；窗口主 pass / G-buffer 都没有。唯一的"清屏的内容 pass + 矩形"只存在于它自己的单测里。⇒ 规则统一成一条：**公告的矩形，未公告则整目标，clamp 进目标**，两条路径共用一个 `passDrawRect(viewport, w, h)`，`RenderPass` 与 `ScreenPass` 对同一个 `setViewport` 终于同义。
- 顺带解开一个真缺口：**内容 pass 原先根本画不进子矩形**（split-screen / 把第二个视图画进角上做不到），而 `ScreenPass` 可以。`SceneView::addSurfaceLayout` 的文档一直承诺"布局回调可以更新某个 pass 的 viewport"——对清屏的内容 pass 那句话是假的。
- `presenting` 退回它真正的事实（"这个 pass 公告了清屏" = 该目标的基底层：depth-on 与窗口默认光），不再回答 viewport 问题（见 `PassAttributes::presenting`）。槽记住的是**公告**（`ContentSlot::announced_viewport`）而不是推导出的矩形，`resize()` 因此重新推导，而不是留着按旧表面算的矩形。
- 门禁（**变异验证**）：新像素相位——内容 pass 清屏 + 只画进 96x54 的矩形，断言矩形中心是那支四边形、而**目标中心**是清屏色（旧的"填满"规则会把四边形画在目标中心 ⇒ 相位红，实测：`the content pass painted (34,6,2) at the target centre`）；`ContentSlotViewportTest` 里那条角色单测换成"公告的矩形（含 clamp）对每个 pass 都算数"；证据基线 55 → **74 行**（原基线自阴影那两笔起就没重基，18 行 lit-face 断言一直没进基线，本轮一并补上），其余 55 行逐字节不变。
- 门禁：`vsg_backend_selftest`（6 个 lit-face vantage + PiP 相位）、`ctest` 23/23、`check_include_hygiene` / `check_doc_symbols` / `check_diagnostic_formats`。


## FPS 读数的采样口径（2026-09-26）：每次发布都是它那段时间的**真平均**

- **触发**：用户报告“鼠标拖动时读数上下跳动较大”。查代码：读数是**每帧瞬时 1/dt 的 EMA（权重 0.2）**、每
  **0.15 s**（≈6.7 Hz）刷一次。
- **为什么这是缺陷（不是“太跳”这么简单）**：它报的**量**本身就不存在——1/dt 是一帧的**样本**而非速率，EMA 的时间
  常数是 `1/(0.2·fps)`（60 fps 时 ≈83 ms、200 fps 时 ≈25 ms），**依赖它正在显示的那个数**；而且它印出的值**任何区间
  都没有过**。实测（阶跃流：60 fps 3 s → 30 fps 3 s）：旧读数印 `60, 60, 40, 33, 31, 30`；拖动形状流（60 fps 名义、
  1/4 的帧慢 1.5–2.5×）上 10 s 内改值 **75** 次、单步最大 **17 fps**。
- **新口径**：**窗口内数帧 / 窗口墙钟时间**，窗口关了就发布，**不做任何混合**——每个数字都是它那段区间的精确平均。
  同一条阶跃流印 `60, 60, 60, 60, 60, 54, 30, 30…`：唯一混的那一格是**真的前 60 后 30 的半秒**的真实均值，之后立即
  精确 30；50 ms 卡顿在 0.5 s 窗里如实显示为 **56.25**（30 帧 / 0.533 s），既不抹平也不推后。
- **两个旋钮**：`Sampler::window_seconds`（采样区间，默认 **0.5 s**）与发布节奏（= 窗口关闭，即 **2 Hz**）；
  整数显示本身是四舍五入的**死区**（值不变不重画）= 最便宜的防抖，且不引入假值。**不做跨窗 EMA**：要更稳就加长
  窗口（每个数字仍然真实），不“混合出”没发生过的值。
- **窗口多长的取舍（实测；拖动形状流：真值 51.5 fps、sd(dt) = 5.9 ms）**：

| T | 窗内帧数 | 估计 ±1σ | 相邻两窗最大跳 | 反映真实变化 | 单次 50 ms 卡顿显示为 |
|---|---|---|---|---|---|
| 0.1 s | ~5 | ±6.5 | 30 | 0.1 s | 46.7（−22%） |
| 0.2 s | ~10 | ±4.8 | 20 | 0.2 s | 52.0（−13%） |
| **0.5 s** | ~25 | **±3.0** | 13 | 0.5 s | 56.4（−6%） |
| 1 s | ~51 | ±2.2 | 7.3 | 1 s | 58.1（−3%） |
| 2 s | ~103 | ±1.5 | 4.8 | 2 s | 59.0（−1.7%） |

  取 0.5 s：±3 fps 的估计精度、半秒内如实反映真实变化；>2 s 会把真实的短事件摊平（这也说明“stutter 可见性”
  不该靠加长窗解决）。
- **行业口径（一手：PresentMon 的 README / 采集应用文档）**：主量是**帧时间 ms**（`FrameTime`/`MsBetweenPresents`/
  `MsBetweenDisplayChange`…），FPS 是**派生**量；每次 capture 出**逐帧 CSV** + 统计摘要（duration、总帧数、
  average、min、max、**90/95/99th FPS 分位**）；实时 overlay 是独立进程、抢 Z-band 的叠层，画的是**图**。
  生态同构（OCAT / CapFrameX / RTSS / NVIDIA FrameView / PIX）：**可读的滚动平均 + 暴露卡顿的分位数/帧时间图**
  是一对。⇒ 一个数字做不到“既可读又不撒谎”；三位七段选“可读的那一半”，并把它做成**不撒谎的**（窗口真平均）。
- **门禁**：`FpsOverlayTest` 5 例（结构 / 稳态节拍 / 保真=逐窗复核窗口平均 / 阶跃“只有一格混、其余精确” / 卡顿如实）；
  **变异 2/2 红**（M1 每帧瞬时发布=旧形状；M2 跨窗混合=被删掉的 EMA），恢复基线 5/5 绿。`test_graphics` 287→**291**；
  两棵树门禁 `cases=451`、应用阶段判图逐字不变。
