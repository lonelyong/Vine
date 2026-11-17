# ADR-2026-11-16：删除“后端半边” —— 共享机制回到 SDK 主线，插件的类型回到插件

- 状态：**已采纳并实施**（2026-11-16）。取代 [ADR-2026-11-16-redesign-seam-and-vocabulary](ADR-2026-11-16-redesign-seam-and-vocabulary.md)
  §2 与批次 3 中“目录即接缝”的那一半决定；该 ADR 的词汇、相位、拒绝形状与其余批次不变。
- 日期：2026-11-16
- 影响范围：`src/viz/graphics/sdk/vine/graphics/backend/**`（删除）、`src/viz/graphics/src/backend/**`（删除）、
  `scripts/check_include_hygiene.py`、`scripts/check_export_annotations.py`、`scripts/check_vk_exposure.py`、
  `cmake/VineLibraryHelper.cmake`、`src/viz/graphics/CMakeLists.txt`、`gfx_backend_{vk,vsg}/CMakeLists.txt`、
  `tests/*/CMakeLists.txt`、`graphics_global.hpp`、`gfx_backend_vsg` 的命名空间、`graphics-architecture.md` §2.9。
- 相关：`ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`、`ADR-2026-11-13-one-engine-one-surface.md`。

## 背景

`ADR-2026-11-16` §2 把 `sdk/vine/graphics/backend/` 定为“后端接缝”，只留两个后端共用的机制，并给这条边界配了四件东西：

1. `BackendHalfGuard.hpp`：每个头都先包含它，宿主没有定义 `VN_GRAPHICS_BACKEND_CONSUMER` 就**编译失败**； <!-- drift-ok -->
2. `BACKEND_SDK_SUBDIR`：默认 include 树排除该子树，单独安装成 `VineGraphicsBackendSdk` 组件；
3. `check_include_hygiene.py` 的 `BACKEND_HALF_AUDIENCE`：宿主不得 include 该前缀；
4. `SEAM_HEADERS` / `SEAM_SOURCES`：钉住目录的文件集。

批次 3 落地后，“接缝名不副实”的问题（25 个头里 18 个只有 vsg 读）已经解决：剩下的 8 个机制确实两个后端都在用。
但四件东西仍然维护**同一条边界**，而这条边界要回答的问题只有一句：“这些头是后端用的，宿主不该包含。”
一句性质由四处表达，任何一处改了名、搬了文件、加了头，另外三处都要跟着同步。

## 决定

**删除“后端半边”这个概念**（`G8`：没有新消费者，边界只挡选择；`G7`：不要用四层机制表达一句性质）。

- 8 个机制平铺进 `sdk/vine/graphics/`，实现平铺进 `src/viz/graphics/src/`：
  `FrameGate`、`DeviceClock`、`FrameTimeline`、`RetirementQueue`、`Readback`、`DrawState`（原 `Keys`）、
  `TargetPlan`、`DiagnosticDraft`。
- 命名空间并入 `vn::graphics`；`VN_GRAPHICSBACKEND_NS_BEGIN/END` 与 `BackendHalfGuard.hpp` 删除。 <!-- drift-ok -->
- 安装分流删除：`vn_add_library` 的 `BACKEND_SDK_SUBDIR` 参数与 `VineLibraryHelper.cmake` 的对应分支一并删除。
- 门禁改成**按性质**，不按目录：图形模块（`sdk/` + `src/`）不得 include 渲染 API 或任何具体后端；共享机制的导出符号
  仍由文件清单检查（`check_export_annotations.py`）。三者都保留“命中的文件数为 0 就是 finding”的守卫。
- **`gfx_backend_vsg` 自己的类型搬到 `vn::backend_vsg::core`**。它们原本住在 `vn::graphics::backend`（插件把接缝
  命名空间当自己的层用），那是 `G13` 的反例：一个后端的私有类型不该出现在 SDK 的命名空间里。`core` 用一个
  `using namespace ::VN_ROOT_NS::graphics` 保留 `core::DeviceClock` 这类既有拼写，因此 800 余处引用不用改。
- `Readback` 的结构体 `ReadbackResult` 改名 `ReadbackAnswer`：并入 `vn::graphics` 后它与 `RenderDevice.hpp` 的
  公开枚举 `ReadbackResult` 同名，两者含义不同（一个答“这一步能不能做”，一个答“这次读回的结果”）。

## 被否决的路

| 选项 | 为什么不做 |
|---|---|
| 平铺文件但保留 `BackendHalfGuard.hpp` | 守卫只能按文件名列举，而“宿主可包含”已经是新规则；守卫会让**合法**的使用编不过（`G8` 反例：挡住的是真实用法，不是错误） | <!-- drift-ok -->
| 保留 `VineGraphicsBackendSdk` 组件（CMake 按文件清单排除） | 需要在 CMake 与门禁各维护一份 8 文件名单，正是本次要拆掉的那类同步（`G7`） |
| 保留 `vn::graphics::backend` 命名空间、只平铺文件 | 名实分离：`#include <vine/graphics/FrameGate.hpp>` 却写 `graphics::backend::FrameGate`（`G1` / `G6`） |
| 8 个机制拆给两个后端各一份 | 两份真源，驻留与帧纪律会漂（`G1`）；它们的定义就是“两个后端都用” |
| 让 vsg 私有类型留在 `vn::graphics` | 一个后端的内部类型进入 SDK 命名空间（`G13`），且给未来的后端立了坏样子 |

## 后果

**好的**

- 一条性质（“SDK 不依赖任何具体后端”）由一条依赖规则表达，不再由路径 + 守卫 + 安装组件 + 文件集四处表达。
- 插件自己的类型第一次住在自己的命名空间里（`G13`）；`vn::graphics` 不再出现 `Observe` / `VariantPool` / `PhaseTable`。
- 少一个编译期守卫头、一个安装组件、两个 CMake 参数。

**代价（如实）**

- 宿主现在可以 include 这些机制头，SDK 的命名空间里也看得见它们——`G7` 的残余：复杂度从“四件同步的机制”变成
  “读者需要知道这些类型是给后端用的”。这条事实只由文档与评审承载。
- 三处门禁从“目录”改成“文件清单/模块规则”，必须保留“匹配 0 个文件即红”的守卫，否则重命名会让检查静默失效
  （本树已发生过两次）。
- 仓库外消费 `VineGraphicsBackendSdk` 的代码要改 include 与命名空间（源码级破坏）。

**触发条件（什么时候重开）**

1. 第三个后端出现且某个机制只有它读 ⇒ 按 `G13` 搬进那个后端，不留在 SDK。
2. 出现“宿主确实要读某个机制”的消费者 ⇒ 把它当成 SDK 的正式词汇命名与文档化，而不是恢复“半边”。
3. 宿主误用（include 了只有后端该用的头并依赖其行为）在评审外真的发生 ⇒ 先加文档与样例，再加门禁，不恢复目录。

## 实施状态

- **已落地（2026-11-16）**：9 个头 → 8 个头平铺 + 7 个实现平铺；全量 include / 命名空间改写；
  `check_include_hygiene.py`、`check_export_annotations.py`、`check_vk_exposure.py` 重新定范围；
  `graphics_global.hpp` 只剩一组命名空间宏。证据：`Graphics` / `gfx_backend_vk` / `gfx_backend_vsg` 编译零警告，
  `test_graphics` / `test_vk` / `test_vsg` 全绿，三道门禁零 finding。
- **更正（2026-11-17，审计 A3-04）**：背景里第 4 件东西 `SEAM_HEADERS` / `SEAM_SOURCES` **从未存在过**——
  本文决定删掉它时它就不在代码里。它留下的空缺（"接缝是哪 8 个头"没有门禁断言）曾被
  `docs/guides/graphics-backends.md` 误写成"门禁已钉住文件集"，那一页已改正。是否补这条门禁是一次**新决定**
  （与"触发条件"第 3 条同族），不是恢复"半边"；跟踪项见审计报告的 `T-04`。
