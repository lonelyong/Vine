# graphics 的两半：契约与后端工具层（分层与依赖）

> 状态：**已落地**（2026-09-27）。这份文档回答两个开发者问题——"要渲染，我依赖哪个模块？"、
> "要写一个后端，我依赖哪个模块？"——并记录这一层从插件里摘出来、最终并入 graphics 的原因。
> 相关：`graphics-design.md` §6（分层）、`vsg-reimplementation.md` §2.3（物理边界）。

## 0. 结论先写

```
vn::graphics                          一个模块、一个 target、一个依赖名
  sdk/vine/graphics/*.hpp             HOST 半：场景、相机、RenderEngine/RenderPass/RenderTarget、
                                      RenderBackend 契约（宿主唯一需要的东西）
  sdk/vine/graphics/backend/…           BACKEND 半：把那份契约变成计划的共享机器
                                      （**一个扁平目录，内部不再分组**，见 §1.5）
  src/…                               实现；私有头与 cpp 同目录（"..." 引）
  libGraphics (SHARED)                两半编在同一个库里
```

**放置判据只有一条：是否跨越 DLL 边界（= 是否导出）。**

| 判据 | 目录 | 安装 | 导出宏 | 外部能否 include |
|---|---|---|---|---|
| **导出**（宿主用 **或** 后端用） | `sdk/vine/graphics/**` | ✅ | **要** | 能（**许不许**由规则管） |
| 只在本 DLL 内（引擎内部） | `src/**`，私有头与 cpp 同目录 | ❌ | **不要** | **物理上不能**（`src/` 不在任何 include 根上） |

**"给谁用"不是放置判据，是规则判据**（见 §3）：`backend/` 是导出的，所以它必须在 `sdk/`——即使宿主不该 include 它；
只在本 DLL 内用的东西放 `src/`，连 include 路径都写不出来。

约定来源：`vn_add_library` 只 `install(DIRECTORY sdk/)`（导出面 = 安装面）；私有头与 cpp 同目录是 appfw 的
做法（`src/fw/appfw/src/gui/{SurfaceWindow,RenderControl}.hpp`）；`include/` 是**插件的**目录（插件整体不安装头，
所以它的 `include/` 天然只在构建树里），库不用它（`base/core` 只有 `sdk/` + `src/`）。

* **宿主**：依赖 `vn::Graphics`，只 include `vine/graphics/*.hpp`（**不含** `backend/`）。
* **后端**（`gfx_backend_*`）：依赖 `vn::Graphics`，另可 include `vine/graphics/backend/**`。
* **没有任何人是"只依赖 backend 半"的**——这正是它不是一个独立模块的判据（见 §2）。

## 1. 两半各自是什么

| | HOST 半 | BACKEND 半（`backend/`） |
|---|---|---|
| 内容 | `Scene` `Node` `Geometry` `Material` `Camera` `CameraManipulator` `RenderEngine` `RenderPass` `RenderTarget` `Pipeline` `ShaderProgram` `RenderBackend`（契约）、后端注册表、`Diagnostics`（唯一诊断路线）`ReportOnce`（每 episode 报一次） | `Protocol`（合法性状态机）`FrameRecorder` `FrameCompiler` `FrameGraph`（依赖序 + 环）`Keys` `TargetPlan`/`ClearPlan` `Streams`/`FrameArena`/`FrameRing`/`MaterialArena` `VariantPool` `RetirementQueue`/`FrameTimeline` `Observe`/`PhaseTable`/`PixelProbe`/`SlotProbe`/`AllocationGate`/`DeviceRequirements`/`SessionMove` |
| 读者 | 宿主应用、appfw、应用插件 | 后端作者 |
| 稳定性 | 宿主 API：不为某个后端改动 | 随后端一起演进（`vsg-reimplementation.md` §4 的标题就是"当前实现的名字"） |
| 规模 | ~7.5k 行 src + 40 头 | ~3.5k 行 src + 24 头 |

## 1.5 `backend/` 内部：分类而**不分目录**，以及四个"粘连"单元

`backend/` 里只放**只有后端需要**的那一半；25 个单元**平铺**在一个目录里。按"它回答什么问题"可以分成四类，
但这只是**分类**，不落成目录（也不提为同级）：

| 类 | 单元 | 判据 |
|---|---|---|
| 设备/WSI | `DeviceRequirements` `SessionMove` `SlotProbe` `Readback` `DepthProbe` | 只有"有设备/交换链的 API"才有 |
| 存储 | `FrameArena` `FrameRing` `MaterialArena` `Streams` | "这些字节放哪、这个流是不是新的" |
| 证据 | `Observe` `PhaseTable` `PixelProbe` `AllocationGate` | 不在渲染路径上，只被"要证据的人"用 |
| 一帧怎么走 + 后端的身份与寿命 | `Protocol` `FrameRecorder` `FrameCompiler` `FrameGraph` `Keys` `VariantPool` `StateRegistry` `ClearPlan` `TargetPlan` `RetirementQueue` `FrameTimeline` | 每帧必过，或后端跳帧缓存的身份/寿命 |

**为什么不拆目录、也不提为 `backend/` 的同级**——四条判据逐条否决：

1. **放置看"是否导出"**（§0）：25 个都导出 ⇒ 都必须在 `sdk/vine/graphics/**` 下；在 `backend/` 里还是 `device/` 里
   是**组织**问题，与放置规则无关。
2. **同级不改变任何权限**：要让宿主/引擎用某个单元，只能提到 **`graphics/*.hpp`（A 段）**；
   放到 `backend/` 旁边会被同一条规则照旧归入后端侧。
3. **同级不降低深度**：`vine/graphics/backend/X.hpp` 与 `vine/graphics/device/X.hpp` **同深度**（都是两级）；
   真要变浅只有 A 段。
4. **同级不区分读者**：`backend`/`device`/`storage`/`evidence` 的读者**相同**（都是后端）——拆顶层目录的收益本应
   来自"读者不同 / 依赖方向不同"，这里没有。

（**名字**，2026-09-27 定）：目录叫 `backend/`，不叫 `frame/`——25 个单元里只有 6 个名字带 `Frame`（24%），
`frame` 只描述了少数内容，还让 `frame::FrameArena` 这类限定名重复；`backend` 点的是**读者**（写后端的人），
与本文一直用的措辞（"只有后端需要的那一半"）一致。被否决的名字：`detail`/`internal`/`impl`——这半边是
**明确支持的扩展面**，把它标成"内部"是**假契约**，比名字不精确更糟；`core`——会把"哪半是核心"说反
（核心是宿主半），且与 `base/core` 撞名；`support`/`kit`——说了等于没说。
所以**扁平放在 `backend/` 下，不再细分、也不再改名**。

**四个"粘连"单元**（一个头里同时装了两种东西）——正确动作是**拆**，而不是整体搬到某一边：

| 单元 | 该拆成 | 去哪 |
|---|---|---|
| `ClearPlan` | 策略（该不该清、谁 bootstrap）/ 应用（load-op 变体、layout） | 策略 → SDK；应用 → `backend/`（家族形状） |
| `TargetPlan` | 策略（resize/rebuild/借深度，纯函数）/ 应用（真的去建、搬附件） | 策略 + `TargetShape`（数据）→ SDK；应用 → `backend/` |
| `FrameTimeline` | 查询（在飞行几帧、某帧完成了吗）/ 账本 | **已做（2026-09-27）**：查询面上 A 段成 `FrameProgress`（`FrameStats.hpp`，与 `FrameCounters`/`RetentionStats` 同形：`submitted`/`completed` + `inFlight()`）＋ `RenderBackend::frameProgress()`（默认 false =“不记账”）＋ `RenderEngine::frameProgress()`；**账本**（`begin/submitted/abandoned/completeUpTo`）与 `FrameToken` 留 `backend/`——**宿主今天拿不到 token**（`Session::beginFrame()` 是插件内部的），提一个拿不到的值类型等于凭空开一个面 |
| `Diagnostics` | `ReportOnce`（通用去重工具）/ 收集器 | **已做（2026-09-27）**：两个类型都上 A 段（`ReportOnce.hpp` 自此是独立的头，规则与收集器本来就不是一件事），并**合并路线**——引擎持有唯一的 `Diagnostics`，后端经 `RenderBackend::setDiagnosticsRoute()` 拿同一实例 |

**顺序（别把搬迁做两遍）**：

1. 判据与"粘连清单"定稿（本节）；
2. 目录**扁平**放在 `backend/` 下，并把 `frame` 改名 `backend` —— **已做**（曾试过"分组在 `frame/` 内"与"提为 `frame/` 的同级"，
   按上面四条判据否决；名字判据见本节末）；
3. 把“要给宿主看的数据”提到模块公共 + 引擎开口 —— **部分已做（2026-09-27）**：
   `FrameCounters`/`RetentionStats` 已在 `vine/graphics/FrameStats.hpp`（模块公共、随 SDK 安装）；
   通路是 `RenderBackend::frameCounters()/retentionStats()`（默认返回 **false ＝“不保留计数”**，别拿零当空帧）
   ＋ `RenderEngine::frameCounters()/retentionStats()`；vsg 后端从自己的 `Observe` 回答同一批数字。
   钉子：引擎转发/不回答各一条（`test_graphics`），后端自答一条（`test_vsg`，含变异反证：把计数来源改成 `{}` ⇒ 红）。
   **同日追加**：帧时钟的查询面 `FrameProgress`（`submitted`/`completed`/`inFlight()`）走同一条通路
   （`RenderBackend::frameProgress()` 默认 false ＋ `RenderEngine::frameProgress()`，vsg 从 `Session::timeline()` 回答）。
   变异反证做了两处：引擎不转发 ⇒ `test_graphics` 红；后端改回 false ⇒ `test_vsg` 红。
   **未做**：引擎自己那 6 组手写"seen / reported"集合改用 `ReportOnce`（行为中性，单独一轮）；
   `TargetShape` / 计划数据（与 S3/S4/S5 的通路一起）；
4. 诊断路线**合并** —— **已做（2026-09-27）**：`ReportOnce` 与 `Diagnostics` 一起上 A 段
   （`vine/graphics/ReportOnce.hpp`、`vine/graphics/Diagnostics.hpp`），引擎持有唯一的路线，
   `RenderBackend::setDiagnosticsRoute()` 把它交给被驱动的后端 ⇒ 宿主读**一个**数
   （`RenderEngine::diagnosticCount()` / `diagnosticCount(cat)` / `diagnosticsClean()`），
   旧的 `backendDiagnosticCount()` + `engineDiagnosticCount()` 连同"要总数就自己加"一起删掉。
   判据：机器规则本来就会挡住折中方案——引擎要用共享路线，就只能把它放在 A 段（R2 不许宿主半 include `backend/`）。
   没被引擎驱动的后端（测试、工具）仍用自己那条私有路线，所以 `test_vsg` 直接驱动 `VsgBackend` 的用法一字不变；
5. `Protocol`/`FrameGraph`/`FrameCompiler`/`FrameRecorder` 下沉 `src/`（S1/S2，**行为契约改动**，单独一轮）。

## 2. 为什么是"一层"，而不是"第二个模块 "

**判据：脱离父层就不存在的组件，是这一层的一部分。** `vn::Math`/`vn::Global`/`vn::Core` 都能独立成立；
backend 半**离开 `graphics` 就不能编译**（它整个建立在该模块的类型上）。

三条备选路径与它们的代价：

| | 独立模块 `src/viz/rendergraph`（曾实现） | 同一棵树、两个 target（曾实现） | **并入 graphics（现行）** | <!-- drift-ok -->
|---|---|---|---|
| 开发者要记的名字 | 2 个 | 2 个（还要解释为什么两个） | **1 个** |
| 后端的链接行 | `vn::Graphics` + `vn::RenderGraph` | `vn::Graphics` + `vn::GraphicsFrame` | **`vn::Graphics`** |
| 导出宏 | 不需要（STATIC） | 不需要（STATIC） | 需要（37 处，已加，**由 `check_export_annotations.py` 钉住**） |
| 宿主是否加载 backend 半的代码 | 否 | 否 | 是（~3.7k 行，可忽略） |
| 形态 | 模块 | 既不是层也不是模块 | 层 |

最后"导出宏"这条曾经是我反对并入的主要理由，现在不成立了：**漏标的失败模式是 Windows 上的
`unresolved external symbol`（响的链接错，不是静默错误）**，而且它是**可静态检查**的——见 §3 的 R3。

## 3. 机器规则（脚本在 `scripts/`，门禁第 5 段跑）

| 规则 | 内容 | 实现 |
|---|---|---|
| **R1** | backend 半**不许** include 任何后端（`vsg/`、`vine/vsg/`） | `check_include_hygiene.py` 的 `backend_half_findings()` |
| **R2** | 除 `gfx_backend_*`、`test_vsg` 与 backend 半自己外，**任何地方不许** include `vine/graphics/backend/` | 同脚本的 `host_audience_findings()` |
| **R3** | backend 半里凡是有 **out-of-line 定义**的类型 / 自由函数，其声明必须带 `VN_GRAPHICS_API` | `check_export_annotations.py`（37/37） |

R2 是关键的一条：没有它，"两半"只是目录约定；有了它，宿主在编译期就被挡住。

## 4. 谁能复用到什么（三桶）

| 桶 | 单元 | 手撸 Vulkan（零 vsg） | D3D12 / Metal | OpenGL / GLES |
|---|---|---|---|---|
| **A 任何后端**（API 无关帧纪律） | `Protocol` `FrameRecorder` `FrameCompiler` `FrameGraph` `Observe` `PhaseTable` `AllocationGate` `FrameArena` `FrameRing` `Streams` `RetirementQueue` `PixelProbe` `DepthProbe` `StateRegistry` | ✅ | ✅ | ✅（时间线/槽位概念退化） |
| **B 显式 API 家族** | `Keys` `ClearPlan` `TargetPlan` `VariantPool` `Readback` `SlotProbe` `FrameTimeline` | ✅ | ⚠️ 可映射 | ❌（无管线对象/load-op/描述符集） |
| **C Vulkan 家族** | `DeviceRequirements` `SessionMove`、以及键里命名 API 版本/交换链/兼容性的字段 | ✅ | ⚠️ 换掉能力表 | ❌ |

支持这三桶的两个事实（可复核）：

* 这一层的**全部 include** 只有标准库 + `vine/graphics/*` + `vine/math/*`（**没有任何图形 API 头**）；
* `Vk*` 只出现在**注释**里，格式/layout/load-op 用的是 Vine 自己的枚举。

所以"它像 Vulkan"是"**契约**像 Vulkan（`RenderBackend::ClearPolicy` 的 load-op/附件模型、`RenderTarget`
的格式/采样）"的必然结果；要舒服地支持 GL 家族，要动的是契约，而不是这一层放在哪。

**待办**：等第二个**家族**（GL）真的立项时，把 B/C 桶整体下沉到"家族工具层"或具体后端——今天只有一个
后端 + 一个"手撸 Vulkan"计划，切第三层就是猜。

## 5. 决策记录

* **2026-09-27** 起因：帧机器（协议/编译/键/寿命/证据）原本住在 `gfx_backend_vsg` 插件里，虽然已经
  `vsg`-free，但**插件不能依赖插件**，第二个后端（`graphics-design.md:825` 的 `gfx_backend_vulkan` 手撸）
  要么依赖 vsg 插件、要么复制 7.8k 行。
* 曾实现为独立模块 `src/viz/rendergraph`（`vn::rendergraph`、`vn::RenderGraph` STATIC），随后并入 <!-- drift-ok -->
  graphics（`vine/graphics/backend/`、`vn::graphics::backend`、`vn::Graphics`）。**include 路径与命名空间在
  第二次迁移中没有再变**——两次迁移共同的成果（从插件里摘出来、去掉 `vsg::core` 内层命名空间、别名桥、
  卫生规则）都保留了下来。
* 命名空间的拼写遵循 `appfw_global.hpp` 的约定：**每个模块一个 `<module>_global.hpp`**，嵌套子命名空间的宏由
  父宏组合（`VN_GRAPHICSBACKEND_NS_BEGIN = VN_GRAPHICS_NS_BEGIN + namespace backend {`）。
* 别名 `namespace core = ::vn::graphics::backend;`（在 `vsg_global.hpp` 里）是**迁移桥**：插件里 1800 余处
  `core::X` / `vsg::core::X` 拼写保持不变。清理它属于独立的一步（把调用点写成 `rendergraph::X` 后删掉别名）。
