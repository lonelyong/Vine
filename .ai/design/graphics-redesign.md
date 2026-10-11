# graphics 的重新设计（2026-10-11 定，提案）

**状态：提案，未落地。** 用户 2026-10-11 定"重构，重新设计"，边界划到**含后端契约**
（`RenderBackend.hpp` + `sdk/vine/graphics/backend/` 半 + `gfx_backend_vsg` 插件）。

**这一稿作废了两条已登记的决定（记账，谁在什么时候改的主意）：**

1. `graphics-render-next.md` §7 的 *"否决：现在就整块重写（图 + 设备 + 视图一起换）。理由：四条摩擦里三条能用窄口消掉；
   整块重写会把'已经对的'一起推倒，代价与风险不成比例。"* —— **由用户 2026-10-11 显式推翻**。本稿不再引用那条理由，
   但**保留它的技术内核**（"配方 / 几何 / 开关三分"等被点名要保的东西，见 §0）。
2. `graphics-render-next.md` §6 不变量 1 *"公开 API 不乱动……新增入口优先（老入口标'便捷/兼容'，不删）"* ——
   在本稿范围内改为**逐片评估**：契约的形状**要**变，但每一片仍必须是"新增窄口 + 老路保留"（§4 的 S1–S2），
   只有在同一片的两棵树逐字节相同之后才删老路。

**读法**：§1 全是**代码证据**（file:line），§2 是目标形态，§4 是能独立验收的迁移片。
落地一片就把该片移进现状文档并从本文删除（沿用 `graphics-render-next.md` 的规矩）。

## 0. 一页纸

**一句话**：**帧在引擎里已经是一个值，但在边界上又被打散成一串有顺序的可变调用，后端再把它拼回一个值。**

`RenderEngine::frame()` 先编译出这一帧（`compileFrame()`，M7 落地），然后**把编译结果拆开**，用
`beginFrame` → 每行 `beginPass`/`setPassOrder`/`setPassInputs`/`endPass` → `endFrame` → `swapBuffers`
驱动后端（`RenderEngine.cpp:192,273-288`）；每条 pass 自己的 `execute()` 再补上
`setClearPolicy`/`setRenderTarget`/`setViewport`/`setPassState`/`setLights`/`render`（`RenderPass.cpp:248-301`）。
后端那一半收下这些调用、**再拼回一个值**：`FrameRecorder` → `CollectedPass` + `CollectedDraw` →
`FrameDescription`（`backend/FrameRecorder.hpp:152,172,185`），然后 `FrameCompiler` 把它编译成可执行计划。

**重新设计 = 让引擎直接产出后端已经在拼的那个值，把中间那层"平坦协议"删掉。** 连带三件：

- **窗口从帧协议里拿出去**（今天 `beginFrame` 可以取像、`swapBuffers` 才呈现、`nullptr` 目标 = "窗口帧缓冲"：
  8 个纯虚里 3 个只有带窗口的后端才有意义 —— `viewAspect()` 已于 A 删除）。
- **能力与观测各收成一个值**（今天是 4 个"我有没有这东西"的 bool 出参 + 三套互不相干的数）。
- **宿主侧**：`SceneView` 的六个角色收成"视图 + 值"，尺寸/矩形/呈现各自单一 owner。

| 保留（重新设计也不动） | 为什么 |
| --- | --- |
| 分层 API + 第 0 层逃生口（`engine->addPass`、自写 shader、自建 builder） | "三五行出画面"与"完全自己接"同时存在的前提 |
| **配方 / 几何 / 开关** 三分 | 换设置才重建、开关永不重建、几何只改取值 |
| 一帧的真相在编译期定下来（M7 的 `compileFrame`） | **这一条正是本稿能成立的原因**：值已经存在，只是没送到边界 |
| 一个事实一个 owner（矩形/清屏归表面、**画面形状归相机**） | 重划时不能丢 |

## 1. 为什么要重切（证据全部来自代码）

### 1.1 帧被描述了两遍，形状不同

| 层 | 类型 | 形状 |
| --- | --- | --- |
| 宿主引擎（导出） | `FramePlan` / `PassPlan`（`FramePlan.hpp:31,83`，`RenderEngine::framePlan()`） | 每 pass 的 order/name/content/camera/target/enabled/clear/viewport/inputs/output + 上一帧耗时 |
| 后端半（导出） | `CollectedPass` / `CollectedDraw` / `FrameDescription`（`FrameRecorder.hpp:152,172,185`） | scope 属性 + 每条绘制调用（viewport/lights/inputs/commands） |
| 边界上的实际通道 | `RenderBackend` 的 34 个方法（`RenderBackend.hpp`） | **顺序 + 调用次数**才是语义 |

同一个事实被表达三次，其中**通道那一次最弱**（它是时间序列，不是数据）。

### 1.2 顺序是协议，不是类型

`RenderBackend.hpp` 的类注释要花 20 行讲 "CALL ORDER"，再花一大段区分
**scope 属性**（`setRenderTarget`/`setPassOrder`/`setPassState`/`setClearPolicy`，属于整条 pass）与
**每条绘制调用的属性**（`setViewport`/`setLights`/`setPassInputs`，只服务紧随其后那一次绘制），
以及"没有 scope 时设置是 inert、没有 scope 时绘制是 Refuse"。

这套规则无法写进签名，所以它被实现成一个**运行时状态机**：`backend/Protocol.hpp`
（*"`RenderBackend`'s contract has eight rules about call sequences"*，三种判决 Allow/Drop/Refuse，
每个 episode 的报一次还要在 `beginFrame`/`endPass`/新公告处**重新武装**）。
一个接口需要状态机来保证自己的调用序列合法，就是"形状不对"的定义 —— 改成值以后这 8 条规则**无法被违反**，
因为它们无法被表达。

**同一件事的一个具体实例（落地 S1 时撞上的，也正是 A 的起因）**：`RenderBackend::viewAspect()` 的答案
**取决于之前公告了什么** —— 它读"开着的那个 scope 的 target 与 viewport"。于是"把一整帧的描述**在派发之前**拼好"
这件事，在**形状不再是后端的一个查询**之前根本写不出来。一条**返回值依赖调用历史**的查询，就是顺序协议最硬的形态：
它让值化不能只做一半（当时记为 S4a，**已由 A 取代**：形状回到相机）。

### 1.3 窗口烧进了帧

9 个纯虚函数：`initialize` `shutdown` `beginFrame` `endFrame` `setRenderTarget` `render`
`drawScreenProgram` `swapBuffers`（**A 之后是 8 个**：`viewAspect()` 已删）。其中
`beginFrame`（"may acquire the next presentable image"）、`swapBuffers`（"the only call that presents"）、
`setRenderTarget(nullptr)`（= 默认窗口帧缓冲）
**三个都只对带窗口的后端有意义**（`viewAspect()` 曾是第四个 —— 只有知道表面尺寸的那一方能答，A 把它删了）。
一个只画离屏的后端（测试夹具、离线烘焙、CI 无窗口跑）必须实现 3 个空壳。

`viewAspect()` 当时的存在理由写在它自己的注释里（"a second derivation would be the place 'what you see' 和
'what was culled' drift apart"）—— **这个理由是对的**，但它推出的是"aspect 只能有一个来源"，不是
"这个来源必须是后端"。**A 的结论**：那个来源就是**相机**（一台相机一张画面），读者（投影 / 剔除 / view block /
拾取）读同一份状态；而"相机与它画进的矩形不一致"由引擎在编译期报出来 —— 一个来源 **加** 一条诊断，
比"问后端"少了一整条依赖链。

### 1.4 两个同名不同义的东西

`FramePlan`（引擎，只读报告）与 `FrameCompiler`（后端，"what the host asked for becomes an executable plan"）
是**两个不同的东西共用一个词**。同样地 `frame()` 有两条路（`RenderEngine::frame()` 干活、`SceneView::frame()` 转发）。
这不是命名洁癖：它让"这一帧谁说了算"在读者脑子里没有唯一答案。

### 1.5 宿主侧（与上一轮诊断一致，此处只记结论）

- `SceneView` 一个类六个角色（引擎绑定 / 配方 / 相机+内容 / 导航 / 几何+尺寸 / 输入转发 / 呈现胶水），
  约 30 个公开方法。
- **尺寸有两个存储**：`RenderSurface::width_/height_/pixel_ratio_` 与
  `SceneView::surface_width_/surface_height_/pixel_ratio_`（T1 之后表面是 owner，视图那份是缓存 + 一条扇出同步路径）。
- **window pass 的知识跨三个类**：`SceneView::ensureWindowPass()` 造它、`RenderSurface::passOf()` 找它、
  `RenderControl` 声明它。
- `SurfaceWindow::handleUpdate()` 同一个窗口尺寸说两遍：`engine->resize(w,h)` 与 `surface->resize(w,h,ratio)`。

### 1.6 规模实测（这是"整块重写"的真实代价）

| 部件 | 规模 |
| --- | --- |
| 宿主半（`sdk/vine/graphics/*.hpp` + `src/viz/graphics/src`） | ~40 头 / ~7.5k 行 src（见 `graphics-layering.md` §1 的自述） |
| 后端半（`sdk/vine/graphics/backend/`） | 25 个单元 / ~3.5k 行 src / 24 头 |
| `gfx_backend_vsg` 插件 | **20 836 行 / 95 文件**（最大两块：`src/plugins/gfx_backend_vsg/src/internal` 11 981 行 / 36 文件，`src/plugins/gfx_backend_vsg/include/vine/vsg/internal` 6 025 行 / 40 文件） |
| 验收面 | `test_graphics` 346 / `test_gui` 253 / `test_vsg` 433 + 后端自检 + 五个 gate + 两条 demo 冒烟 |

**结论**：这不是"一次提交"，是**六片、每片独立可逆**的迁移。§4 的顺序就是"任一片停下都不留半成品"的答案。

## 2. 目标形态

### 2.1 三张契约（今天的 1 张 → 3 张，纯虚 9 → 3）

```cpp
// 一帧：不可变的值，后端一次收下。形状就是后端已经在拼的那个（见 §1.1 第二行）。
struct FrameDescription
{
    struct Draw
    {
        std::optional<Viewport>          viewport;   // 本来 setViewport（每条绘制一次）
        std::span<const LightRef>        lights;     // 本来 setLights
        std::span<const InputRef>        inputs;     // 本来 setPassInputs
        raw_ptr<const ShaderProgram>     program;    // 本来 drawScreenProgram 的 program（场景档为空）
        raw_ptr<const Camera>            camera;
        std::span<const RenderCommand>   commands;
    };
    struct Pass
    {
        raw_ptr<const RenderPass>        pass;       // 身份：后端按它 keyed 保留的 GPU 状态
        raw_ptr<const RenderTarget>      target;     // nullptr = 呈现表面
        int                              order;      // 本来 setPassOrder
        PassRenderState                  state;      // 本来 setPassState
        ClearPolicy                      clear;      // 本来 setClearPolicy
        // 没有形状字段：画面形状是**相机**自己的状态（一台相机 = 一张画面，2026-10-11 A 定），
        // 于是剔除、投影、view block、拾取射线读的是同一个数（本来 viewAspect()）
        std::span<const Draw>            draws;
        std::span<const Draw>            draws;
    };
    std::span<const Pass> passes;
};

// 1) 设备与会话：建/拆、资源公告、读回、能力。不认窗口。
class VN_GRAPHICS_API RenderDevice
{
  public:
    virtual bool         initialize() = 0;
    virtual void         shutdown() = 0;
    virtual Capabilities capabilities() const;              // 替代 supportsRenderTargets() 等 4 个 bool
    virtual bool         readColorBuffer(const RenderTarget*, int attachment,
                                         std::vector<std::uint8_t>& out, ReadbackResult* why);
    virtual bool         readDepthBuffer(const RenderTarget*, std::vector<float>& out, ReadbackResult* why);
    virtual void         releasePass(raw_ptr<const RenderPass>);      // 资源公告（保留）
    virtual void         releaseRenderTarget(RenderTarget*);          // 资源公告（保留）
    virtual void         setDefaultContentProgram(intrusive_ptr<const ShaderProgram>);
    virtual FrameReport  frameReport() const;               // 替代 frameCounters/retentionStats/frameProgress
};

// 2) 帧：唯一的入口。没有 scope、没有顺序、没有 inert setter。
class VN_GRAPHICS_API RenderFrameExecutor
{
  public:
    virtual bool executeFrame(const FrameDescription& frame) = 0;
};

// 3) 呈现（可选）：没有窗口的后端 surface() 返回 nullptr —— 这是能力，不是空壳。
class VN_GRAPHICS_API RenderSurfaceBinding
{
  public:
    virtual void* nativeHandle() const = 0;
    virtual void  setWindowHandle(void* native_handle) = 0;
    virtual void  resize(int width, int height) = 0;
    virtual void  present() = 0;                            // 本来 swapBuffers
    virtual PresentFormat format() const = 0;               // 本来隐含在 setRenderTarget(nullptr) 里
};
```

### 2.2 删掉什么（每一条都是一个可验收的减法）

| 删掉 | 因为 |
| --- | --- |
| `backend/Protocol.hpp` 的 **8 条顺序规则** 与 `Decision{Allow,Drop,Refuse}` | 值里没有"调用序列"可违规 |
| **"inert setter"概念** | 没有 scope 就没有"scope 之外的 setter" |
| `beginFrame`/`endFrame` 作为**帧的括号** | 帧是参数，不是区间；"这帧做完了"由返回值说 |
| `viewAspect()` | **删掉，不搬家**：形状回到相机（一台相机一张画面，取代片 A 已落地），一个数、四个读者（投影 / 剔除 / view block / 拾取射线）；"同相机不同形状"由引擎编译期报出来 |
| `swapBuffers`/`setWindowHandle`/`nativeHandle`/`resize` 在帧契约里 | 移进 `RenderSurfaceBinding` |
| 4 个 bool 出参的能力/观测方法 | 收成 `Capabilities` / `FrameReport` 两个值 |
| 引擎的**预热那一拍**（`RenderEngine.cpp:155-158`，跑一遍 `beginPass/setPassOrder/endPass`） | 那一拍存在的唯一理由是"后端要在呈现前看一遍 pass 集合"；描述里本来就有全集 |

### 2.3 值从哪来（不是新增一层，而是把已有的搬过来）

- `FrameDescription` 的形状**已经在 `backend/FrameRecorder.hpp` 里存在**（T2 直接把它提到宿主半并改名，
  或保留在 `backend/` 而由宿主包含——这取决于放置判据，见 `graphics-layering.md` §0）。
- **引擎侧要新做的事只有一件**：让 pass**描述**它的绘制而不是**执行**它 ——
  `RenderPass::execute(backend)` → `RenderPass::describe(scene, PassDescription&)`（信息它今天全有：
  `render_target_`/`viewport_`/`stateOverrides()`/`render_commands`/`camera_`，见 `RenderPass.cpp:248-301`）。
  这是 S1 的主体工作量，也是唯一一处"真改结构"（**已落地**，见 §4"已落地：S1"）。
- `ScreenPass` 同理（`ScreenPass.cpp:56-78` → 一条带 `program` 的 `Draw`）。

### 2.4 宿主侧（独立成片，可与契约片并行）

- **尺寸唯一 owner**：`SceneView` 不再存 `surface_width_/height_`（挂表面时向表面问，不挂表面时才用自己的值）。
- **window pass 的知识收进一个地方**：呈现表（`RenderSurface`）已经是它的 owner，`SceneView::ensureWindowPass()`
  降为"给表面调的一步"，`RenderControl` 只声明。
- **`SceneView` 的六段公开面按角色分组并命名**（不改行为、不动 ABI）：recipe / camera+content / navigation /
  geometry / input / presentation，并标出哪三条是"给 `RenderSurface`/`RenderControl` 调，不是宿主 API"。

## 3. 行业对照

| 本设计的取向 | 行业同款 | 差别 |
| --- | --- | --- |
| **一帧是一个值** | `VkRenderingInfo`（一次渲染实例的 area/attachments/loadOp/clearValue 全在一个 struct 里）、WebGPU `GPURenderPassDescriptor`、Metal `MTLRenderPassDescriptor` | 那三家都是"**pass 是值**"；本稿把粒度提到**整帧**（因为 M7 已经把整帧编译出来了）。它们也没有保留"没有 scope 时设置是 inert"这种状态 |
| **窗口是可选依赖** | Vulkan（swapchain 是 `VK_KHR_swapchain` 扩展，核心渲染不认它）、WebGPU（`GPUCanvasContext` 与 device 分离） | 我们的 `beginFrame/swapBuffers` 把 WSI 焊进了帧协议；它们的分层正好相反 |
| **能力是一个值** | Vulkan `VkPhysicalDeviceFeatures`/`Properties`（一次查询）、wgpu `Features`/`Limits` | 我们是 4 个"我有没有这东西"的 bool 出参 |
| **观测是一条路** | Unreal `RDG` 的 `FRDGBuilder` + `stat` 通道、Filament 的 `FrameGraph` + `FrameInfo` | 我们有引擎 `FramePlan`、后端 `FrameCounters/Retention/Progress`、`Diagnostics` **三套** |
| **顺序不可表达** | 反面教材正是底层图形 API 的 command buffer（`vkCmd*` 全是顺序，必须靠 layer/validation 兜） | 我们比 API 更抽象一层，却把 API 那道"顺序"问题带了下来 |

## 4. 迁移切片（每片：动作 / 判据 / 变异 / 依赖）

**贯穿所有片的总判据**：`scripts/vsg_rewrite_gate.sh` 的各阶段证据（含 `[selftest]` 相位行）在两棵树上
**逐行相同** + 三个套件计数不掉（`test_graphics` 346 / `test_gui` 253 / `test_vsg` 428 + 2 已知红）+ 五个 gate 0 finding。
**两棵树**（老路 / 新路）是这一整套能做的前提 —— 见 §5 对 big bang 的否决。

| # | 片 | 动作 | 判据 / 变异 | 依赖 |
| --- | --- | --- | --- | --- |
| **S0** | 记账与冻结（**0 代码**） | 本文件；`graphics-render-next.md` §7 的否决标"已作废（2026-10-11，用户定）"；冻结"给 `RenderBackend` 加方法"（新需求一律只用"改形状"两片解决） | 文档门禁（`check_ai_docs` 0） | 无 |
| **S1** | **pass 描述它的绘制**（唯一的结构改动）— **已落地 2026-10-11，见下** | … | … | done |
| **A** | **画面形状回到相机**（**已落地 2026-10-11，见下**；取代 S4a，也把 S1 的那道缝合掉） | `Camera` 持 `aspect`（`setAspect`/`aspectRatio`/`projectionMatrix()`/`screenToWorldRay(pos)`）；`RenderBackend::viewAspect()` 从契约里**删除**；`collectRenderCommands*(camera)`；`SceneView` 把窗格的形状 push 给相机；HUD 子视口相机自己声明形状；引擎在编译期报"pass 的矩形 ≠ 相机的画面形状" | 见下"已落地：A" | S1 |
| **S2** | **引擎产出整帧的值**（窄口，不破宿主 API）— **前半已落地 2026-10-11，见下**（值 + 观测面） | `compileFrame()` 之后组装 `FrameDescription`；`executeFrame(desc)` 作为**新增**入口；能力位 `Capabilities::frameAsValue`；老路保留 | **两棵树逐字节相同**（selftest + demo 冒烟 + 三套件）；变异：值里漏一条 pass / 漏一条 draw ⇒ 像素判据红 | S1 + A |
| **S3** | **删平坦路径与 `Protocol` 的顺序规则** — **已落地 2026-10-11：帧由值驱动 + 整帧交给后端 `executeFrame`；剩下的 10 个逐条入口与 `Protocol` 的 8 条规则现在是纯删** | 删 `beginPass`/`endPass`/`setPassOrder`/`setPassState`/`setClearPolicy`/`setViewport`/`setLights`/`setPassInputs`/`beginFrame`/`endFrame`；`Protocol` 只留 `ReleaseRenderTarget` 一条（"目标被释放时丢掉公告"）或整体删；预热那一拍删 | **先回答"覆写 `execute()` 的 pass 怎么办"**（见 S2 落地记录第 3 条：仓库里就有这样一条见证）；`Protocol` 的 7 条 `CoreProtocolTest` 用例改写或删除并记账；三套件计数**只许减该删的**；两棵树逐字节相同（此时只剩一棵） | S2 |
| **S4b** | **拆出呈现**（窗口成为可选依赖） | `RenderSurfaceBinding`（`nativeHandle`/`setWindowHandle`/`resize`/`present`/`format`） | **新增能力用例**：一个**只离屏**的后端（`surface()==nullptr`）用一条 pass 画进 `RenderTarget` 并读回 —— 今天它必须实现 4 个空壳。 | S2 |
| **S5** | **能力与观测各收成一个值** | `Capabilities`、`FrameReport`；删 4 个 bool 出参；引擎/后端/诊断三套数收成"一条路 + 一个值" | `test_vsg` 的 counters/progress/retention 用例改写（语义等价）；gate 里的数字逐项对照 | S3 |
| **S6** | **宿主侧**（可与 S1–S5 并行） | 尺寸唯一 owner；`SceneView` 六角色分组命名 + 三条"内部入口"标注；presentation 知识收进呈现表 | `test_gui` 253 不变；`SceneViewViewportTest` 增一条"视图不持有第二份尺寸" | 无 |

**建议的第一刀就是 S1**：它是唯一一处真改结构的地方，无宿主 API 变动、无两棵树需求、判据是
"描述 == 今天的调用序列"这种可以逐项对照的强判据 —— 而且它是 S2–S5 的前置，做完之后剩下的都是搬运。

### 已落地（2026-10-11）：S1

**动作**：新增导出的 `sdk/vine/graphics/FrameDescription.hpp`（`DrawDescription` + `PassDescription`），
`ClearPolicy` **从 `RenderBackend.hpp` 移到这里**（它是"一条 pass 描述自己"的东西，不是后端接口的东西 ——
`VkRenderingInfo` 的 loadOp/clearValue 也在渲染信息里，不在设备接口上；`RenderBackend.hpp` 反过来 include 它）。
`RenderPass` 拆出两个虚拟扩展点 `describePass(scene, out)` / `describeDraws(scene, out)`，
`execute()` 变成"描述 → 在**两半之间**问画面形状 → 描述绘制 → 按契约的顺序宣告"的**薄壳**；
`ScreenPass` 不再覆写 `execute`，只覆写两个 describe。

**判据（实测）**：`test_graphics` 346 → **349**（新增 `PassDescriptionTest` 三条：场景档 / 全屏档 /
"什么都不发" —— 每条都把**描述的值**与记录型后端**收到的调用**逐项对照）；`test_gui` **253**、
`test_vsg` **428**（+3 skip +2 已知红，与落地前逐项相同）、`test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94
全绿；全量 Release 构建 0 error / 0 warning；五个 gate 0 finding。
**变异 1/1 红**：`describeDraws` 少填 `lights` ⇒ 只有场景档那条红（实测）。

**落地时发现两件事（都进设计，不只是记账）**：

1. **顺序错了一片：S4a 是 S2 的前置，不是后续 —— 而 A 把 S4a 整个取代了。** `RenderBackend::viewAspect()` 的答案
   取决于之前公告了什么
   （它读开着的那个 scope 的 target 与 viewport），所以"把整帧的描述**在派发之前**拼好"（S2）在这条依赖解除
   之前**根本写不出来** —— 拼描述时问不到正确的形状。于是 S4 拆成 **S4a（形状变字段，提前到 S2 之前）** 与
   S4b（窗口/呈现拆出去）。这正是 §1.2 那类"顺序是协议"的病灶，`describePass`/`describeDraws` 之间的那道缝
   就是它的样子（缝写在头文件注释里，标明是 S4a 要消掉的东西）。
   **后来（同日的 A）缝从根上没了**：形状回到相机之后，`describe()` 就是**一个**虚函数、不需要向后端问任何东西，
   S4a 也随之取消（见下"已落地：A"）。
2. **空场景的 pass 一个调用都不许发。** 一条没有内容的 `RenderPass` 今天在**第一次公告之前**就返回了；
   "描述的值"如果只在"绘制"那一层表达"没东西可发"，就会静默地把 `setRenderTarget`/`setPassState` 发出去
   —— 那是**行为变化**（多宣告一条 target、多一次清屏归属）。所以门禁放在 `describePass` 里（虚拟，因为
   "什么算没东西可发"是每种 pass 自己的事：场景档看有没有场景，全屏档看源解析出来没有），
   并且有独立用例钉住"一个调用都不许发，连目标都不许"。

### 已落地（2026-10-11）：S3 前半 —— 帧由值驱动（`execute()` 退成宿主直呼）

**动作**：`frame()` 的运行段不再调 `pass->execute()`，而是**宣告编译期建好的那条 entry**
（`beginPass` / `setPassOrder(entry.order)` / `setPassInputs(entry.inputs)` / `announcePassTo(*entry.description)` /
`announceDrawsTo(...)` / `endPass`；`entry.description == nullopt` ⇒ 一个属性都不发）。`drawScenePass` 删除。
**每条 pass 每帧只被描述一次**（编译里），于是 S2 那条过渡代价消失 —— 两条 memo 见证回到紧值（1 / 2、1）。
**每条 pass 的耗时改在编译里量**（`buildFrameValue` 包住"钩子 + 输入 + 描述"），因为帧的值化把工作搬到了那里：
`record_micros` 仍然是"这条 pass 上一帧的占比"，只是量的位置从"运行"变成"记录"（运行那半的耗时是后端的，
见 `FrameCounters`）。

**契约变化（这一片的实质）**：`RenderPass::execute()` **不再是引擎调用 pass 的方式**，它是"宿主自己驱动一条 pass"
（预热、测试、宿主自绘）时才用的拼写；想让引擎跑的活必须放进 `describe()` 或声明的 `before_draw` 钩子。
这条写进了 `RenderPass::execute()` 的文档、`RenderEngine::frame()` 的文档与 `usage.md`。

**判据（实测）**：`test_graphics` 356 → **357**，新增 `TheEngineRunsTheFrameValueAndNeverCallsExecute`
（覆写 `describe()` 与 `execute()` 各计一次数：两帧后 describes = 2〔每帧一次〕、**executes = 0**，
同时断言这条 pass 确实到了线上：两条 scope、两次 `render`、相机与命令数都对）。**变异 1/1 红**：把运行段改回
`pass->execute(...)` ⇒ 3 条红（新用例 + 两条 memo 见证）。
两条见证按"一帧描述一次"复原（`PassesSharingSceneAndCameraCollectOncePerCameraPerFrame` 1/2、
`OneCameraCullsWithItsOwnShapeInEveryRectangle` 1）；`TheFramePlanCarriesWhatEachPassLastCost` 的 `SlowPass`
从"覆写 `execute()` 睡觉"改成"覆写 `describe()` 睡觉"（**这就是 S2 记录的那条发现**：不改写它，这个 pass 的工作会静默消失）。
其余套件全绿（gui 253 / vsg 428 + 3 skip + 2 已知红 —— **含 SDK 像素夹具，即真设备上像素不变** / appfw 32 /
meshio 34 / iobase 94）；全量 Release 0 error / 0 warning；五个 gate 0 finding。

**还剩什么（S3 后半）**：平坦路径本身（`beginPass`/`endPass`/`setPassOrder`/`setPassState`/`setClearPolicy`/
`setViewport`/`setLights`/`setPassInputs`/`beginFrame`/`endFrame`）+ `backend/Protocol.hpp` 的 8 条顺序规则 +
`CoreProtocolTest` 的改写记账 —— **注意**：`executeFrame` 落地之后，这些逐条入口已经不再有"引擎的运行"这个读者，
它们的读者只剩"默认实现"、"宿主自己驱动一条 pass"与后端自己，所以删除它们是一次**纯删**。
**预热那一拍已删（见下）**，它不在剩下的清单里。

### S3 后半（**下一步**）：删那 10 个逐条入口 + `Protocol` 的 8 条规则 —— 执行计划

写清楚，因为这一片**要求两侧同时到位**（改一半就是一棵跑不起来的树，正是 §5 禁止的形态）。三步，每步都能独立验收：

| 步 | 动作 | 判据 | 风险 |
| --- | --- | --- | --- |
| **3b**（**前半已落地 2026-10-11**：`VsgBackend::executeFrame` 覆写，见下） | **插件吃值**：`VsgBackend::executeFrame` 覆写 —— 逐条方法的**体**搬进值驱动的循环：每 pass `d->passes.adopt(pass.pass)` 取身份 → recorder 的 per-pass/per-draw 入口（`beginPass(id)`/`setPassOrder`/`setPassInputs`/pass 事实/每 draw 的 `setLights`/`render`/`drawScreenProgram`/`endPass`）+ **每 draw 的内容副作用**（`d->store->updateMaterial(command.material.get())` 那类，今天是 `VsgBackend::render()` 里做的）；逐条方法退成**薄转发**到同一批私有 helper，供 3c 删掉 | `test_vsg` 全量逐项不变（**含真设备像素夹具**）；`VsgBackendTest` 里"逐条驱动后端"的用例改为按值驱动，或明确标注为"宿主直驱单条 pass" | 中（每 draw 的副作用是唯一真风险：漏一个 `updateMaterial` 就是画面/性能问题） |
| **3c** | **删契约**：`RenderBackend` 删那 10 个方法，`executeFrame` 变**纯虚**；`backend/Protocol.hpp` 的 8 条顺序规则退场（`Protocol` 整体删或只留"目标释放时丢掉公告"）；`CoreProtocolTest` 的用例按设计记账（改写为"值不可能乱序"的用例或删除并记数） | 三套件计数**只许减该删的**；`test_gui` 253 不变；五个 gate 0 finding；`check_ai_docs --strict` 不增 | 中（宿主侧只剩"宿主自己驱动一条 pass"这一条路：`RenderPass::execute` + `announcePassTo`/`announceDrawsTo` 必须留在 SDK 里，它们是那条路的地基） |

**3b 前半（已落地 2026-10-11）**：`VsgBackend::executeFrame` 覆写 —— 插件现在**自己吃值**：走值里的每条 pass，
调它**自己**的入口（`beginPass`〔含 `d->passes.adopt` 的身份〕/ `setPassOrder` / `setPassInputs` / pass 事实与绘制
〔走 SDK 那一份共享映射 `announcePassTo`/`announceDrawsTo`〕/ `endPass`），于是每 draw 的内容副作用
（recorder 采集、`updateMaterial` 那类）都还从**同一条路**过。
**判据（实测）**：`test_graphics` **359**、**`test_vsg` 433 = 428 + 3 skip + 2 已知红（含真设备像素夹具）——
与覆写前逐项相同**；插件与 SDK 全量 Release 0 error / 0 warning。**这正是"冻结再重构"的兑现**：改的是"值从哪来"，
线上序列与画面都没动。剩下 3b 的收尾 = 把这些入口的**体**抽成私有 helper（供 3c 删契约方法），再进 3c。

**为什么按这个顺序**：3b 把插件搬到新路上，行为仍由全量 + 真设备像素夹具兜住；3c 才是删除，而删除的那一刻
**引擎、插件、测试三边都已经在新路上**。任何一步停下，树都是绿的。

**3b 的判据先冻结好了（已落地 2026-10-11）**：`TheFrameValueBecomesExactlyThisCallSequence`
（`test_graphics` 359 绿）把"值 → 线上调用"的**序列**逐项钉死（帧括号；每 pass 的 scope〔身份/order/inputs〕；
它声明的 target/viewport/clear/state〔只发它说了的〕；每 draw 的 lights + 绘制调用；scope 关闭；
什么都不发的 pass 只开合 scope、一个属性都不发；被禁用的 pass 根本不在帧里）。`MockBackend` 为此加了一条
`wire` 调用日志。**插件自己的 `executeFrame` 必须产出同一序列** —— 这就是 3b 的验收线，改了顺序会当场红，
而不是变成一张不一样的画面。

**原来计划里的"3a：给 `FrameRecorder` 加按值采集入口"已取消（本次调查的结论）**：recorder 的入口本来就是
per-pass / per-draw 的（`beginPass(PassId)` / `setPassOrder` / `setPassInputs` / 各 pass 属性 / `render` /
`drawScreenProgram` / `endPass`），它们**不是契约**、而是插件内部的采集层 —— 值按构造就是有序的，所以
"值 → 这些入口"只是换个调用者，不需要一层翻译；而 `PassId` 的所有权在插件（`d->passes.adopt`），
"按值采集"反而要往 recorder 里塞一个 key 函数。**结论：把值喂给 recorder 就是 3b 的插件循环本身。**

### 已落地（2026-10-11）：S3 后半之二 —— 预热那一拍删除


**动作**：`initialize()` 里那段"每个启用且不清屏的 pass 先执行一遍"的预热**整段删除**，只留下注释说明为什么它
曾经存在、以及为什么现在不需要。它当年的理由是"让后端在呈现第一帧之前看到整个 pass 集合、并把内容编译好"
（当时帧是一串调用、内容是在绘制调用里惰性构建的）。现在**一帧就是一个值**（`executeFrame`）：后端在动手之前
就拿到了整个 pass 集合，引擎自己的路径与第一帧的路径是同一条 —— 该后端自己预热的部分（主内容）本来就在
`initialize()` 里做。

**判据（实测）**：`test_graphics` **358**（与删之前逐项相同）；**`test_vsg` 433 = 428 + 3 skip + 2 已知红，
与删之前逐项相同** —— 其中含 **SDK 像素夹具（真设备上渲染并读回像素）**，即"没有预热的第一帧画面不变"的实测；
`test_gui` 253 / `test_appfw` 32；其余门禁同前。**若将来在第一帧上出现"内容构建"类回归，这条注释就是它的来历。**

### 已落地（2026-10-11）：S3 后半之一 —— 整帧交给后端（`executeFrame`）

**动作**：契约新增 `virtual void RenderBackend::executeFrame(const FrameDescription&)`，**默认实现就是原先那条
逐条宣告序列**（每 pass：`beginPass` / `setPassOrder` / `setPassInputs` / pass 自己的事实 /
`announceDrawsTo` / `endPass`；没有 description 的 entry 一个属性都不发）。引擎的 `frame()` 里那段逐条循环
**整体换成一次调用** `backend_->executeFrame(frame_value_)` —— 于是**边界上是值**，而不是"一串必须按顺序发的调用"：
后端可以覆写它直接从值构建（保留式渲染器建图、离线渲染器遍历），那时逐条调用它一份都收不到；不覆写的后端
（含仓库里的 vsg 插件）**行为逐调用不变**，因为它们拿到的是同一个序列、只是从**一个**输入来。

**判据（实测）**：`test_graphics` 357 → **358**：`ABackendIsHandedTheWholeFrameAsOneValue`（覆写 `executeFrame`
的后端：一帧收到**一次**调用，passes 按运行序、带 order 与绘制数；被禁用的 pass 不在里面；并且
`beginPass`/`setPassOrder`/`render`/`drawScreenProgram` **一次都没被调**）。**变异 1/1 红**：引擎改回自己逐条发 ⇒
该用例红。其余套件全绿（gui 253 / **vsg 428 + 3 skip + 2 已知红，含 SDK 像素夹具 ⇒ 真设备像素不变** / appfw 32 /
meshio 34 / iobase 94）；全量 Release 0 error / 0 warning。`MockBackend` 顺带补了 `setPassOrder` 的记录
（`order_sets` / `orders`），让"一条 pass 一条 scope"这类见证更硬。

**这一步之后，S3 后半的删除变成纯删**：那 10 个逐条方法已经没有"引擎的运行"这个读者了。

### 已落地（2026-10-11）：S2 前半 —— 帧的值（引擎产出，可观测）

**这一步写完之后的状态**（S3 前半把它接了上）：值在编译期建好、可直接观测，而帧当时**仍由 `execute()` 驱动** ——
S2 与 S3 前半是同一件事的两半，合起来就是"引擎产出整帧的值，并且跑的就是它"。

**动作**：`FrameDescription` 补上帧那一层（`vn::graphics::FrameDescription::Pass` = 引擎知道的三个事实
〔pass 身份 / order / 解析后的 inputs〕+ 该 pass 自己的 `PassDescription`〔nullopt = 这帧什么都不发〕）。
`RenderEngine::compileFrame()` 末尾调 `buildFrameValue()`：按**运行序**对每条启用的 pass 跑它的 `before_draw`
钩子、把解析好的 inputs 告诉它、请它描述自己 —— 于是**整帧的值在派发之前就存在**（这正是 A 解除的那条依赖）。
`RenderEngine::frameDescription()` 把它读出来（只读观测，像 `framePlan()` 一样）。
`announcePassTo` / `announceDrawsTo` 从 `RenderPass.cpp` 的匿名命名空间**搬到 `RenderBackend.cpp` 并导出**
（声明在 `FrameDescription.hpp`）：描述→平坦调用的映射只能有一份，`RenderPass::execute()`（宿主直呼的兼容线）
与引擎的运行都走它。

**这一步**（写清楚，免得读者以为帧已经由值驱动）：**帧仍然由 `execute()` 驱动**（`drawScenePass` 原样保留），
值目前是**编译的产物 + 观测面**。设计与判据都要求 S2 两棵树并存（老路 `execute()`、新路 `executeFrame(desc)`），
所以"S2 = 值 + 新入口"的后半片与 S3 一起做。

**判据（实测）**：`test_graphics` 354 → **356**，两条新用例：
`TheFrameValueIsEveryPassAndEveryDrawTheFrameRan`（5 条 pass：场景档带 target/viewport/program 覆盖、
全屏档带解析出来的 input、无相机的档〔有 entry、没有 draw〕、无内容的档〔有 entry、`description == nullopt`〕、
被禁用的档〔根本没有 entry〕—— 值里的 pass 与后端被 `beginPass` 的**逐条对齐**，值声称的绘制调用数
= 后端收到的 `render` + `drawScreenProgram` 数）、
`TheFramesValueIsBuiltAfterThePerFrameHooks`（钩子改内容 ⇒ 值描述的是**这一帧**的版本，不是上一帧的）。
**变异 2/2 红**：① 值里不填 description ⇒ 4 条红（两条新用例 + 两条 memo 见证）；② 钩子不跑 ⇒ 4 条红。
其余套件：`test_gui` 253 / `test_vsg` 428(+3 skip +2 已知红) / `test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94；
全量 Release 0 error / 0 warning；五个 gate 0 finding。**两条 memo 见证按新事实改写**（见下"过渡代价"）。

**落地时发现三件事（都进设计）**：

1. **`before_draw` 钩子必须搬进编译期。** 它原来在运行序里、pass 之前跑（HUD 自更新：FPS 读数、镜像 gizmo）。
   值的意义是"派发之前就拼好"，而钩子写的是**内容** —— 描述必须发生在钩子之后，否则值描述的是**上一帧的画面**。
   所以钩子在 `buildFrameValue()` 里按运行序跑，`frame()` 里那处删除。实测两条钩子都是"只写内容、不碰后端"，
   且都不依赖任何 pass 已经跑过（镜像是抄**源相机**的朝向、读数是取墙钟），所以行为不变 ——
   唯一动的是 `record_micros` 不再包含钩子的耗时（它现在在编译里）。
2. **过渡代价：每帧每条 pass 被描述两次**（编译一次建值、运行一次 `execute()`），于是
   `contentCollectReuseCount()` 的见证值变大（树**走**的次数不变：仍然每 view 每帧一次，memo 命中不算走）。
   两条用例（`PassesSharingSceneAndCameraCollectOncePerCameraPerFrame` / `OneCameraCullsWithItsOwnShapeInEveryRectangle`）
   已按新事实改写并在注释里写明这一条会在 **S3**（帧由值驱动）复原。
3. **S3 必须先回答"覆写 `execute()` 的 pass 怎么办"。** 仓库里就有这样一个见证：`TheFramePlanCarriesWhatEachPassLastCost`
   的 `SlowPass` 覆写 `execute()` 睡 2 ms，靠"引擎调用它"来当耗时。一旦帧由值驱动，引擎不再调 `execute()`，
   这个 pass 的工作**静默消失**（实测：这正是把 `frame()` 改成由值驱动时当场红的那条，见下）。
   所以 S3 不能只是"删平坦路径"：它要么把 `execute()` 明确降级为"宿主直呼才生效"（并改写这条用例 + 记帐），
   要么给覆写者留一条显式的路（"我自己发公告"的声明）。**这条是 S3 的第一件事，不是收尾。**

### 已落地（2026-10-11）：A —— 画面形状回到相机（取代 S4a）


**决定**：用户给出的两条口径是"一台相机 = 一张形状；引擎把'同相机不同形状'报出来"。这条把 M2c
（上一轮的"aspect 出相机"）**反转**，并且把 S1 留下的那道缝（`describePass` / `describeDraws` 之间要问后端
一个数）**缝掉** —— 缝没了，形状是相机自己的状态。

**为什么（评估过"必要性"）**：M2c 换来的是"相机可跨矩形复用"，代价是**形状成了只有后端能答、且答案取决于
之前公告了什么**的问题（§1.2 那类病灶的又一个实例），于是 S2 在这条依赖解除前写不出来。行业是**分裂**的
（VTK / Filament / Unreal 从矩形推；three.js / Babylon / Godot / Unity 把形状放在相机上），而本仓**文档**选了
VTK 那一派、**用法**却是 three.js 那一派（每个视图一台相机）。选 A 之后"同相机不同形状"从**被支持**变成
**被报告**：这一类静默错没有消失，只是第一次变得可见。

**动作**：`Camera` 持 `aspect_`（`setAspect` 忽略非正数 / `aspectRatio` / `projectionMatrix()` /
`screenToWorldRay(pos)`）；`Scene::collectRenderCommands*(camera)` 少一个入参（memo 仍以 view-projection 为键 ——
形状在矩阵里，所以两种形状天然是两条 memo 项）；`RenderBackend::viewAspect()` 从契约里**删除**（纯虚 9 → 8）；
`PassDescription` 不再有形状字段；`FrameRecorder` 删 `setViewAspect`，快照投影读相机；`SceneView`
把窗格的形状 push 给相机（`applySurfaceGeometry` 里的一行，与 `manip->onResize` 同一个矩形）；HUD 子视口相机
自己声明形状（`AxisGizmo` 1:1、`FpsOverlay` = 自己那块 box 的形状）；`OrbitCameraManipulator::viewportAspect()`
读相机（无相机时才回退到自己的矩形 —— 否则交互与**拾取射线**会读两个数）；**新增**引擎编译期检查
`RenderEngine::collectPictureShapeMismatches()`（矩形 = viewport 否则目标/表面，画面 = 相机的 aspect，
正交相机则为它窗口的形状；一条 pass 的错**每 episode 报一次**，因为拖窗格会让句子里的数字每帧都变）。

**判据（实测）**：`test_graphics` 349 → **354**（新增：`ACameraWhoseShapeIsNotThePassRectangleIsReportedOnce`
（报告 + 每 episode 一次 + 修好又坏是新 episode）、`TheRectangleIsThePassViewportAndAScreenPassIsNotMeasured`
（视口才是矩形；全屏 pass 不参与 —— 它的三角形在 clip space，读不到投影）、
`SceneViewTest.ResizingTellsTheCameraTheShapeOfThePictureItDraws`、
`FpsOverlayTest.TheReadoutCameraIsShapedForTheBoxItDrawsInto`、
`CameraManipulatorTest.TheInteractionShapeIsTheCameraPictureNotThePane`；M2c 那三条判据**反转**：
`SceneTest.OneCameraInTwoRectanglesIsTwoPicturesAndTwoWalks` → 同一相机两次 setAspect 两次走树，
`RenderEngineTest.OneCameraCullsWithItsOwnShapeInEveryRectangle`（原 `EachPassCullsWithTheShapeOfItsOwnRectangle`）
→ 宽矩形不再把视锥变宽、一个相机一次走树，`FrameRecorderTest.TheViewBlockIsProjectedForTheCamerasOwnShape`
（原 `...TheShapeTheScopeAnnounced`）→ 快照投影是相机自己的矩阵）；`test_gui` **253**、`test_vsg` **428**（+3 skip
+2 已知红）、`test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94 全绿；全量 Release 构建 0 error / 0 warning；
五个 gate 0 finding（`check_ai_docs --strict` 仍是 20 条历史引用，不增）。
**变异 4/4 红**：① 检查里永不报 ⇒ 两条新用例红；② 矩形不读 viewport ⇒ 视口那条红；
③ `SceneView` 不 push 形状 ⇒ `ResizingTellsTheCamera...` 红；④ `FpsOverlay` 不 push box 形状 ⇒ HUD 那条红（各自实测）。

**留下的记账**：S4a 整片**取消**（A 就是它要的东西，只是把形状放在相机而不是 `PassDescription`）；
`RenderBackend.hpp` 的 include 与 `ClearPolicy` 的位置不变（S1 已搬）；S2 的前置依赖现在是 **A（已落地）**。

## 5. 否决与否决理由

- **否决：big bang（一次换完，不保留老路）。** 理由：本稿最强的一条判据是"**两棵树逐字节相同**"
  （selftest 证据 47 行 + demo 画面 + 四套件计数）。一次换完就没有第二棵树，回归面（§1.6：20.8k + 7.5k + 3.5k 行）
  只剩"跑起来看起来没坏"。S1–S2 保留老路正是为了让最强的判据一直可用。
- **否决：把 `Protocol` 留成新值的"校验器"。** 理由：它校验的 8 条规则在新形状里**不可表达**
  （没有 scope、没有顺序、没有 inert setter）。留一个"校验不可表达之物"的状态机，是把 §1.2 的病灶留着。
- **否决：只写文档不动码**（上一轮给出的"一页地图"）。理由：用户 2026-10-11 明确要"重构，重新设计"；
  地图降级为 S6 的一半（分组命名）。
- **否决：先把宿主侧 `SceneView` 拆干净再动契约。** 理由：`SceneView` 的六个角色大部分**证据不足**
  （"多视图/多窗口"还不是常态），而契约那三处（值 / 窗口 / 观测）有**逐字节可验的判据**。先做有证据的。
- **保留观察**：`FramePlan` 与 `FrameDescription` 合并之后，`FramePlan` 是否还该作为**导出**类型存在
  （它是"读者看的一帧"），还是降为 `FrameDescription` 的一个视图 —— 等 S2 落地时看读者有几家。

## 6. 风险登记

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 契约是**导出**面，改名/改形状会同时打到 appfw 与插件 | 插件需重建、appfw 一处（`SurfaceWindow`）要跟 | S1–S2 只"新增"；真正删除集中在 S3/S4b，且**同一片内**改完两侧 |
| 20.8k 行后端 + 3.5k 行后端半的迁移 | 大范围回归 | 每片两棵树逐字节；S3 之前老路一直在 |
| ~~`viewAspect()` 移进值之后，剔除与投影可能各算一次~~（**已由 A 消掉**：形状是相机的一个字段，没有"各算一次"的余地） | 静默的画面/剔除不一致（本设计最怕的那类） | A 的判据钉这一条：**一个** 数（`Camera::aspectRatio()`）被投影 / 剔除 / view block / 拾取射线读；矩形与它不一致时引擎**报**出来（`ACameraWhoseShapeIsNotThePassRectangleIsReportedOnce`）；变异：检查里永不报 / 矩形不读 viewport ⇒ 各自红（实测） |
| 无头/无窗口后端是**新能力**，历史上没跑过 | 可能暴露既有假设（今天 8 个纯虚里的窗口假设同样没被验证） | S4 用夹具那条"没有视图也能渲染"的最小用例改写而来（`SdkPixelTest.TheFixtureAlsoDrawsContentWithNoViewAtAll`） |
| 这是本仓自己否决过的"整块重写" | 半成品比不动更糟 | §4 的顺序就是为了"任一片停下都不留半成品"；S0 的冻结 + S1 的自洽 + S2 的两棵树是三道闸 |
| **改了契约半（`RenderBackend.hpp`）就必须重建插件**（A 已实测：`viewAspect()` 一删，老 `gfx_backend_vsg.dll` 与新头文件不匹配，`VsgBackendPluginTest` 两条直接红 —— 而且**测试目标不会自动构建插件**，得显式 `--target gfx_backend_vsg`） | 看起来像"插件注册丢了"，浪费一轮排查 | 任何动 `sdk/vine/graphics/*.hpp` 的片，验收命令里显式带上 `gfx_backend_vsg`；把这条写进切片给出的命令里 |
| 真设备电池（ASan/LSan、lavapipe 门禁）需要环境 | S2/S4 的判据在无设备机器上只能跑一半 | 无头判据（`VINE_VSG_*`）先上；设备电池只在有 lavapipe 的地方跑，如实记账 |

## 7. 与既有文档的关系

| 想知道 | 看 |
| --- | --- |
| 今天的宿主半是什么样（配方/几何/开关、初始化时机、所有权） | `appfw-render-view.md` |
| 今天的后端契约与 `backend/` 半（放置判据、四个"粘连"单元） | `graphics-layering.md`、`vsg-reimplementation.md` §2/§3 |
| 本轮之前那套提案（M1–M11 与 T1–T7）的现状 | `graphics-render-next.md`（其中 §7 的"整块重写否决"**已被本稿作废**） |
| **本稿** | 就是这里；落地一片就移进上面的现状文档 |
