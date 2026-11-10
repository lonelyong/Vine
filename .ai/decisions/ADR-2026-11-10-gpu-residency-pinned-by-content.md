# ADR-2026-11-10：GPU 驻留按内容对象钉住（pin），按"谁活着"回收

- 状态：已采纳（第一步落地，其余按 §迁移 推进）
- 日期：2026-11-10
- 相关：`../design/graphics-content-lifecycle.md`、`../design/vsg-backend.md` §2.10、`../backlog/graphics.md`
  `src/viz/graphics/src/Scene.cpp`、`src/plugins/gfx_backend_vsg/src/internal/ContentStore.cpp`、
  `src/viz/graphics/sdk/vine/graphics/backend/Streams.hpp`、`src/viz/graphics/sdk/vine/graphics/Residency.hpp`

## 背景

同一个问题——"这份 GPU 驻留什么时候可以放掉"——今天有五种答案：

| 驻留 | 判据 | 机制 |
|---|---|---|
| 顶点/索引/实例流 | 最近被命名 + 窗口（`grace = slots + 1`）| `SharedStreams::releaseUnseen` |
| 纹理图像 | 引用计数 `owner->useCount() <= 1` | `MaterialImages::releaseAbandoned` |
| 材质块槽 | 引用计数 + 行扫尾 | `ContentStore::releaseAbandoned` → `BlockStorage::releaseMaterial` |
| 编译半片（管线）| 反事实查询（表还答 program@rev 吗 ∧ 还有几何问这个 layout 吗）| `ContentHalves::answerable` |
| 描述符集 | 同上 + 每帧 `ensure` | `ContentSets::sweep` |

三种失败形状，实测都在：**裁剪丢上传**（第一条的时间性：离开视野 `slots + 1` 帧就让出上传，回来重传）；
**`useCount` 阈值随持有者变化而静默失效**（第二、三条：任何一个新持有者都让 1 变成 2）；
**每 pass 的 O(#halves × #geometries) 反事实扫描**（第四、五条，随资源种类数增长）。

## 其它库怎么做的（借什么、不借什么）

- **wgpu-core**：资源分 **user refs** 与 **submission refs** 两套引用，销毁走 triage（`ResourceTracker`）——
  只在没有在飞提交引用时真正销毁。**借**：两套引用是两件事（对应本仓库的"引用计数"与"退役队列"）。
  **不借**：它把两套引用都挂在资源上；本仓库的退役队列已经是独立对象，不需要资源知道在飞状态。
- **bgfx**：句柄 + 延迟销毁队列，按帧延迟（`BGFX_CONFIG_MAX_FRAME_LATENCY`）。**借**：延迟销毁的窗口就是
  `RetirementQueue`；**不借**：延迟窗口不能替代"谁还在用"的判断。
- **Diligent Engine**：显式 `ReleaseStaleResources()` + fence。**借**：释放是显式的、由宿主驱动（与
  `ADR-2026-10-22-cache-unbounded-explicit-release` 同向）。
- **Godot RenderingDevice**：RID + `free_rid(rid, dependencies)`，依赖决定真正释放的时机。**借**：
  "释放时点名依赖"就是本 ADR 的 pin。
- **QRhi（Qt）**：`QRhiResource` 基类 + `create()` / `destroy()`。**不借**：它成立的前提是 QRhi **自己**
  创建 Vulkan 对象；本仓库把创建交给 vsg，基类会变成一层转发（`G7`）。
- **Dawn（WebGPU）**：对象内部引用 + 命令缓冲的"内部用途"引用。**借**：内部引用与用户引用分开；
  **不借**：需要改 `RefCounted` 的布局（本仓库的 `intrusive_ptr` 刻意零分配）。
- **VMA**：suballocation（材质块/每帧块就是它的形状）。**借**：块不是独立资源，账与释放分开记。
- **VulkanSceneGraph**：侵入式引用 + 每帧 record/compile 出新的命令节点，`CompileTraversal` 缓存
  实现。**事实**：对象释放只能靠"丢引用"，所以"什么时候丢"必须由本仓库自己回答。

## 选项

| 选项 | 好处 | 代价 / 违反 |
|---|---|---|
| A 保持现状 | 零改动 | 五种判据；裁剪丢上传；反事实扫描（`G1` / `G6` / `G10`）|
| B 窗口调大（`grace = N`）| 抖动变少 | 仍是时间猜测；"多久算久"是策略而不是事实（`G8`）|
| C 两级淘汰（释放驻留但留 key）| 早还内存 | 回来仍要重传；多一个状态（`G1`）|
| D `GpuResource` 基类 + 虚释放 | 统一动词 | 覆盖不到流/描述符集/管线（无内容对象可继承）；需 `RenderDevice&` 参数（`G2` 反向边）；把 GPU 概念压给内容类型（`G13` 同类问题）|
| E 生命周期状态机挂在对象上 | 直观 | 状态本是三时钟的函数；在飞事实会进对象，`RetirementQueue` 已精确表达（`G1`）|
| F **pin =（内容身份, revision），按"谁活着"回收** | 一条规则、一个动词、零时间窗、反事实扫描消失 | 显存不再自动让出（见"代价"）|

## 决定

选 **F**：

1. 每个驻留条目记 **pin =（内容对象的身份, revision）**；`revision` 是内容对象自己的"这一次的内容"公告
   （`Geometry::revision` / `Material::revision` / `ProgramFacts::generation`），所以"换了一版"与"换了一个对象"
   用同一个键表达。
2. 回收不问"它死了吗"，只问"**现在谁活着**"：每帧（或内容变更时）由事实表枚举活的 pin 集合，不在集合里的
   驻留统一 retire（`RetirementQueue` 停车，不 free）。
3. 删除全部时间窗与容量上限：`Streams.hpp` 的 `last_frame` / `grace` / `releaseUnseen` / `evictions` 退役；
   `ContentHalves::answerable` 与 `ContentSets::sweep` 由 pin 存活替代（半片的 pin = program@rev，见"开放项"）。
4. 显式释放仍是宿主动词（`releaseContent(Object)`，补 `Buffer`），它等价于"立刻把这些 pin 判死"。
5. 重建是天然的：`acquire` miss 调 builder；同一 key 允许**多代共存**（`generation`），旧的靠停车窗口消失。

**为什么这比"基类"好**：pin 是**值**，不动对象模型，所以内容类型不承担 GPU 概念（`G13`），
不需要 `RenderDevice&` 参数（`G2`），而且覆盖到**没有内容对象可继承**的那三类（流、描述符集、管线）。

## 代价

- **显存**：驻留 = 活内容所需要的全部。移除场景但宿主仍持有的 600 MB 模型会常驻，直到 `releaseContent()`。
  这是显式管理（`ADR-2026-10-22`）的必然结果，也是本 ADR 唯一的真实代价，必须写进
  `graphics-content-lifecycle.md` 的现役要点。
- **半片累积**：pin 只看 program@rev 时，layout/变体更替会让半片累积（每个是几十 KB 的管线）。
  这一条要按 `G10` 量一次再定（见开放项 1）。

## 迁移

每一步独立可验证、可回退：

1. **词汇 + 账本**（已落地）：`vn::graphics::ResidencyLedger` / `ResidencyRow` / `ResidencyKind`（`graphics/` 半，
   Vulkan-free），`RetentionStats::residency` 暴露；`gfx_backend_vsg` 把流 / 图像 / 材质块三张表接进去。
   无行为变化。
2. **流**迁入 pin 模型：`StreamKey` 表泛化到 `backend/ResourceTable<K, V>`，pin = geometry@rev，删窗口。
   验收：裁剪 100 帧零重传；离树后 `slots + 1` 帧内离场。
3. **纹理**迁入：删 `MaterialImages::releaseAbandoned`，pin = texture@rev。
4. **材质块**迁入：builder 写 arena、destroyer 放槽；删 `ContentStore` 三个释放循环 → 一个 `retireOwner`。
5. **半片 + 描述符集**迁入：删 `answerable` 与 `ContentSets::sweep`，pin = program@rev（+ 描述符集另加
   5 个贴图 pin）。删掉的是两份 O(n × m) 扫描。

每步之后跑：`check_ai_docs.py --strict`、`check_design_docs.py`、`check_doc_symbols.py`、
`check_include_hygiene.py`、`check_export_annotations.py`、`check_public_docs.py`、
`check_vsg_content_seam.cmake`、`test_vsg`（含 `StreamUploadsTest` / `BlockStorageTest` / `ContentPassTest` /
`VsgBackendTest` 的编辑成本配方）。

## 开放项

1. 半片的 pin 是否只看 program@rev（会累积）还是另加"layout 仍被活几何需要"（回到一份扫描）。**倾向后者只
   在实测显示累积不可忽略时才加**。
2. 是否给 `RefCounted` 加 weak 支持：**不做**。pin 集合由事实表枚举，不需要"对象已死"的通知。
3. `Buffer` 不在事实表里：流的 pin 用**几何@rev**（几何声明了它的字节），共享 buffer 的自然由多个几何共同钉住。

## 后果

- `G1` / `G4` / `G6`：驻留寿命一个定义、一个动词、一套判据。
- `G7`：宿主的接口不变（`releaseContent` + 现有报告），复杂度收进后端。
- `G10`：删掉每 pass 的反事实扫描；代价是显存，写清楚并计量。
- `G13`：内容类型不新增 GPU 概念；pin 由后端从身份派生。
- 被取代：`Streams.hpp` 的读者计数论证仍成立（判的是**持有量/身份**，不是"谁读过"），但时间窗部分退役。
