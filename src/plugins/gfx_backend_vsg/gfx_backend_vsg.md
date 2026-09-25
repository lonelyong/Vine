# gfx_backend_vsg：模块说明（重写版）

> **一句话**：注册名 `vsg` 创建的是**重写版门面** `VsgBackend`（由 `VsgRenderBackendFactory` 注册），它按
> [`.ai/design/vsg-reimplementation.md`](../../../.ai/design/vsg-reimplementation.md) 的设计实现
> `vn::graphics::RenderBackend`。那份设计文档是本模块**契约、坑与实施记录**的唯一权威。
>
> **本文只回答**：这个插件是什么、文件在哪、怎么构建、怎么跑、证据在哪、哪件事该看哪份文档。
> 行为细节（服务/拒绝、调用次数、更新策略）见 [`docs/backend.md`](./docs/backend.md)；逐帧数据流与
> L0/L1 映射见 [`docs/data-flow.md`](./docs/data-flow.md)。
>
> **已退场的旧渲染器**（`VsgRenderer` 一路：场景桥、相机桥、材质管理器、槽模型）连同它的文档叙述一起删掉了；
> 考古用 git 历史（`git log -- src/plugins/gfx_backend_vsg`），设计与删除记录见设计文档 §11.16bt。

## 1. 定位与插件模型

`gfx_backend_vsg` 是 `vn::graphics` 渲染抽象（`RenderBackend`）的一个真后端，以 appfw **MODULE 插件**
（动态库）形式交付：把 graphics 层的场景图 / 相机 / 渲染命令翻译成 **VulkanSceneGraph（vsg）** 调用。

```
┌────────────────────────────────────────────┐
│ graphics（抽象层，无三方依赖）                │
│  RenderBackend / RenderBackendRegistry      │
│  RenderEngine / RenderPass / ScreenPass     │
│  Scene / Node / Geometry / Material / Camera│
└───────────────────┬────────────────────────┘
                    │ registry.create(u8"vsg")（运行时插件）
┌───────────────────▼────────────────────────┐
│ gfx_backend_vsg（MODULE 插件）               │
│  GfxBackendVsgPlugin → VsgRenderBackendFactory│
│  VsgBackend : RenderBackend（SDK 门面）      │
│  └─ Session / WindowTarget（呈递与表面）     │
│     FrameRecorder → FrameCompiler → VsgExecutor（三段式）
│     ContentAssembly（内容世界：表 / 半片 / 集合 / 流 / 块）
└────────────────────────────────────────────┘
```

* 插件 `load()` 在 `RenderBackendRegistry` 里登记工厂；`create(u8"vsg")` 拿到的就是重写版门面。
  宿主不需要包含本模块或 vsg 的头文件。
* 命名空间：实现都在 `vn::vsg`（`vsg_global` 的宏）；graphics 类型在 `vn::graphics`；vsg 类型是全局 `::vsg`。
* 分层纪律：`core/` **不许出现 `vsg::`**（`scripts/check_include_hygiene.py` 机器校验）——这是"哪些规则能
  无设备测试"的物理保证；只有 `api/` 与几个扁平单元碰 vsg 对象。

## 2. 文件地图（谁负责什么）

**包含树** `include/vine/vsg/`：`vsg_global`（导出宏与命名空间宏）与下述单元的公开头；其中
`RenderStateMapper`、`VsgUtils`、`VsgBufferView`、`VsgFwd` 是 header-only/前向声明单一家。

**扁平共享单元**（重写版与测试都用的 6 个 + 2 个）：

| 单元 | 职责 |
| --- | --- |
| `VsgHostWindow` | 宿主窗口的采纳与跟随（表面格式、viewable、resize） |
| `VsgSceneRules` | 设备无关规则：像素格式→`VkFormat`、通道形状、颜色附件写 |
| `VsgDynamicState` | 动态状态命令的声明与比较（cull/polygon/topology/blend） |
| `VsgVulkanEntryPoints` | 三个 loader 不导出的扩展入口点（volk 是唯一包含点） |
| `VsgBackendUtility` | 图手术、设备同步、会话策略查询 |
| `VsgUtils` / `VsgFwd` / `VsgBufferView` | `Mat4d`→`dmat4`；只在指针后出现的 vsg 类型；缓冲视图 |

**`api/`**（`include/vine/vsg/api` + `src/api`）——门面、会话、目标与内容世界：

| 组 | 单元 |
| --- | --- |
| 门面与会话 | `VsgBackend` `Session` `SessionContent` `SessionMove` `WindowTarget` `Device` `DeviceFeatures` `DeviceProbe` `VsgExecutor` |
| 内容层 | `ContentAssembly` `ContentPass` `ContentHalves` `ContentDraw` `ContentFacts` `ContentSources` `ContentStore` `ContentSweep` `ContentImages` `ContentPush` `ContentSets` `ContentPipeline` |
| 数据与表 | `GeometryFacts` `DrawBlock` `LightBlock` `ShadowBlock` `ViewBlock` `ProgramAbi` `ProgramVariant` `MaterialImages` `StreamUploads` `BlockStorage` `BlockDescriptors` `VariantPool` `PassRegistry` `StateCommands` |
| 目标与读回 | `OffscreenTarget` `HostTargets` `HostReadback` `WhiteImage` |
| 内部视图（仅测试） | `BackendContent` |

**`core/`**（`include/vine/vsg/core` + `src/core`）——无 vsg 的规则与账本：

| 组 | 单元 |
| --- | --- |
| 协议与帧 | `Protocol` `FrameRecorder` `FrameCompiler` `FrameGraph` `FrameArena` `FrameRing` `FrameTimeline` `RetirementQueue` |
| 键与状态 | `Keys` `StateRegistry` `VariantPool`（声明在 core） `ClearPlan` `TargetPlan` `DepthProbe` |
| 内容身份与存储 | `Streams` `MaterialArena` `FactResult` |
| 设备约束 | `DeviceRequirements` `AllocationGate` |
| 证据与诊断 | `Observe` `Diagnostics` `PhaseTable` `PixelProbe` `SlotProbe` `Readback` `OneShot` |

**插件外壳**：`GfxBackendVsgPlugin`（`VN_DECLARE_PLUGIN`）与 `VsgRenderBackendFactory`（按名注册/创建）。

**测试与工具**：`tests/test_vsg/`（本模块的集成测试；MODULE 库不能链，测试**直接编实现源文件**）、
`scripts/vsg_rewrite_gate.sh`（门禁）、`scripts/xwin2ppm.py` / `ppmprobe.py` / `xwinresize.py`（读窗口与断言）。

**文档**：本文件（导航）、[`docs/backend.md`](./docs/backend.md)（运行时行为）、
[`docs/data-flow.md`](./docs/data-flow.md)（数据流与映射）、设计文档（契约 / 坑 / 实施记录）。

## 3. 构建与依赖

* **运行期下限 Vulkan 1.4**（`DeviceRequirements`）：设备 `apiVersion` 低于它就**拒绝会话**并在诊断通道报两个
  版本号；后端有权依赖 1.4 的核心行为，把低版本设备"跑子集"当成可接受会让宿主从驱动深处才知道。
  判据：`tests/test_vsg/DeviceRequirementsTest.cpp`。
* `vn_add_plugin(... gfx_backend_vsg)`：MODULE 库；部署到 `<exe>/plugins/vine/gfx_backend_vsgd.*`。
* `VINE_USE_FETCHCONTENT=ON`（推荐）：静态 glslang + vsg **v1.1.16** 打进插件，保留运行期 `ShaderCompiler`
  （自定义 program 需要）；`OFF`：走 vcpkg/本机 vsg——**没有 glslang 的预装 vsg 会让 program 路径静默失效**。
* 链接 `vsg::vsg` + appfw（插件基类）+ graphics（接口与注册表）。
* **插件源列表是 GLOB**：`src/` 下新增 `.cpp` 要重新 configure；`tests/test_vsg/CMakeLists.txt` 要显式加两处
  （`SRC_FILE_LIST` 与 `target_sources`）。

## 4. 跑起来与证据

```bash
ninja -C build test_vsg && ./build/bin/test_vsg          # 无设备用例秒级；设备用例要 lavapipe + X11
ninja -C build gfx_backend_vsg                           # 插件（app 运行时 dlopen 它）
bash scripts/vsg_rewrite_gate.sh build                   # 完整门禁（Debug），build-release 同理
```

门禁一条命令给结论：构建 → 套件（**跳过即失败**，读 `VUID` / `SYNC-HAZARD` 计数）→ 三个 hygiene 脚本 →
相位行（`[selftest]` 不许有 `FAILED`）→ 应用阶段（读 demo 窗口两次，判 `content ≥30%`、预览条 `max ≥64`、
平背景占比 `≤5%`）。**"测试过了"不是结论**——细节见设计文档 §5.8。

## 5. 文档分工（同一件事只写一处）

| 主题 | 唯一权威 |
| --- | --- |
| 契约、设计决定（D1–D8）、**坑**、实施记录与登记 | `.ai/design/vsg-reimplementation.md` |
| 运行时行为：服务/拒绝、每帧调用次数、更新策略、诊断、验证 | 本文 §4 + [`docs/backend.md`](./docs/backend.md) |
| 逐帧数据流、L0/L1 与 ShaderSet 映射、支持矩阵 | [`docs/data-flow.md`](./docs/data-flow.md) |
| 自定义着色：作者视角 / 后端 ABI / 名字地图 / 引擎契约 | `src/viz/graphics/docs/usage.md`、`.ai/design/vsg-reimplementation.md` §5.4、`docs/data-flow.md` §6、`.ai/design/graphics-shader.md` |
| 引擎侧使用文档（宿主视角） | `src/viz/graphics/docs/usage.md` |
| 构建树 / 工具约定 | 仓库根 `CMakeLists.txt` 与 `cmake/`、`scripts/vsg_rewrite_gate.sh`、`.ai/memory/graphics.md` |

> 早期设计历史（vsg-design、vsg-pass-lifecycle、vsg-pipeline-sharing、vsg-target-unification、
> vsg-custom-attributes、vsg-user-mutation-strategy、vsg-selection-highlight、vsg-upstream-alignment、
> graphics-vsg-audit、vsg-custom-shader 等）已在 2026-09-25 压缩为**历史登记**存根：它们描述的都是
> 已退场的 SceneBridge 时代，全文在 git 历史（存根顶部写了 `git show <hash>:<path>`）；仍生效的规则
> 与教训已并入设计文档 §5。
