# vsg 后端：用户端可变 / 可配置 / 可组合情形的处理策略

> 模块：`src/plugins/gfx_backend_vsg`
> 日期：2026-09-08
> 关联：`.ai/design/vsg-pipeline-sharing.md`（管线/数据/状态共享与增量编译 D22）、
> `.ai/design/vsg-selection-highlight.md`（STEP 元素选择/高亮）、
> `src/plugins/gfx_backend_vsg/gfx_backend_vsg.md`、`vine-to-vsg-data-flow.md`（缺陷表 D8/D10/D13/D14/D16/D19/D22）。
>
> 一句话：**后端遵循"增量变更模型"** —— 每个用户级变化先映射到
> `检测键 → 受影响层(数据/状态/DS/编译) → 动作`；能**就地写**的绝不重建，
> 能**只重建数据或只重建状态**的绝不整树重建，确实要 GPU 编译的走 **D22 增量**，
> 稳态帧零成本。本文枚举用户可改/可配/可组合的情形与对应策略。

## 1. 变更分层模型（谁变了 → 动哪层）

`SceneBridge` 保留 Item 子树为
`MatrixTransform(root) → state_node(StateGroup: 管线+DS) → data_node(Commands: 顶点/索引/绘制)`。

| 输入层 | 后端"检测键" | 主要受影响层 |
|---|---|---|
| `Material` **值**（diffuse/specular/ambient/shininess） | 每帧读值 vs 缓存值比较 | 共享 UBO（就地写 + `dirty()`） |
| `Material` **对象**（换绑/新建） | `Material*` 指针 | state_node（新 DS；管线按内容去重共享） |
| 透明度（scene×node×leaf opacity） | opacity 值 | 颜色数组 alpha（就地写 + `dirty()`） |
| 可见性（隐藏/剔除） | 命令流缺失 | 摘 root 子节点（Item 保留复用） |
| 变换（`MatrixTransform`/worldMatrix） | 矩阵比较 | `transform.matrix`（就地写） |
| `StateNode` 折叠（深度/剔除/多边形/拓扑/blend） | `ResolvedRenderState` | state_node（变体模板） |
| `Geometry` 顶点/索引（revision） | `geometry->revision()` | data_node |
| 增/删 drawable | 命令流增删 | data(+state) / 600 帧逐出 |
| `ShaderProgram` GLSL 源 | **无（D10）** | —（缺口） |
| `Light` 增删改 | 每帧 light_group 替换 | 无 GPU 编译（record 期 uniform） |
| `Camera`/视角/视口 | 每帧 apply | 无（动态 viewport） |
| RenderTarget 尺寸/重建 | extent | 全槽 teardown→重建 |
| 移除 pass/target | `release*` | 摘图→deviceWaitIdle→clearCache→erase |

## 2. 各情形处理策略表

| # | 用户操作 | 动作 | 重建 | GPU 编译 | 生效 | 备注 |
|---|---|---|---|---|---|---|
| 1 | 改 `Material` 某属性值（如 `setDiffuse`） | 每帧去重后比较；变则 UBO 就地写 + `value->dirty()` | 无 | 无 | 次帧 TransferTask | **已修(2026-09-08)**：Phong 值标 `DYNAMIC_DATA`（见 VsgMaterialManager）；未变零拷贝 |
| 2 | 换绑到另一个 `Material` 对象 | state_dirty → 重建 state（新 DS；管线共享） | state_node | D22 增量（新 DS/变体） | 次帧 | data_node **复用，不重传网格** |
| 3 | `Material` 不再被引用 | 命令缺失 → 600 帧后逐出；teardown 时 `clearCache()` | — | — | 延迟 | 裸指针键依赖场景保活（D14） |
| 4 | 透明度（scene×node×leaf） | 颜色数组 alpha 就地写 + `dirty()` | 无 | 无 | 次帧 | 颜色数组 `DYNAMIC`；程序路径 opacity 无效（D8） |
| 5 | 隐藏 / 视锥剔除 | 该几何不在命令流 → 摘子节点、Item 保留 | 无 | 无 | 当帧 | absent<600 帧复用，不重编 |
| 6 | 移动 / 旋转 / 缩放 | `item->last_matrix != world` 时写 `transform.matrix` | 无 | 无 | 当帧 | 懒比较，稳态零写 |
| 7 | 改 StateNode（深度/剔除/线框/blend/拓扑） | `ResolvedRenderState` 变 → state-only（L2 变体模板命中则跳过 configurator） | state_node | 仅新变体 D22 增量 | 次帧 | 拓扑变化=新管线变体（Vulkan 属性） |
| 8 | 改顶点/索引数据（revision） | data-only：重建 data_node（物化+上传） | data_node | D22 增量（上传） | 次帧 | state_node 原样复用；仍整份重物化（见 §4） |
| 9 | 新增 drawable / 删 drawable | 新建 Item / 600 帧逐出 | 按需 | D22 增量 | 次帧 | 新几何加入即编译 |
| 10 | 改 `ShaderProgram` GLSL（同对象，源变） | revision 变 → state-only（L1/L2 含 revision）；**全屏程序槽同样按 revision 重建节点**（2026-09-11 补，此前只比指针，热重载不生效） | state_node / 全屏槽 node | D22 增量（新变体） | 次帧 | **已修(2026-09-08)**：`ShaderProgram::revision()`；数据节点复用；几何路径与全屏路径现同口径 |
| 11 | 灯光的增/删/改 | 每帧替换 light_group 子节点；**公告的灯全部不可用（禁用 / 未翻译类型）时不动光根，默认光保留**（2026-09-11 补） | 无 | 无（record 期收进 lightData uniform） | 当帧 | 无需重编译；零光源会把整个 pass 照黑 |
| 12 | Camera 变换 / 视口 / 窗口尺寸变化 | 每帧 apply；动态 viewport | 无（几何管线与尺寸解耦） | 无 | 当帧 | vsg `DYNAMIC_VIEWPORTSTATE` 默认开 |
| 13 | RenderTarget 尺寸变化/重建 | 摘图→`deviceWaitIdle`→各槽 `clearCache`→按新尺寸重建→全图编一次 | 全槽 | 全图（重建点） | 全停 | 一次性代价（D18） |
| 14 | 移除 pass / target / 槽 | `releaseWindowLayer/releaseRenderTarget`：摘图→`deviceWaitIdle`→clearCache→erase | — | — | 全停 | 清理顺序统一（先摘图再释放 GPU 对象） |

## 3. 多条件组合（叠加）规则

- **四个脏标记独立判定**：`data_dirty`(revision) 与 `state_dirty`(material/state/program) 在 `syncRenderCommands`
  内分别计算；`changed` 只置一次，`created` 只入一次 → 同帧多次/多层变化**合并为一次 D22 增量**。
- 组合结果（示例）：
  - 改材质**值** + 改矩阵 → 仅 UBO dirty + matrix 写，**零重建**；
  - 换材质对象 + 改 StateNode 线框 → state-only + 可能一个新管线变体 → 一次增量；
  - 顶点数据 + 材质同变（少见）→ data+state 一起，一次增量；
  - 透明度 + 移动（常见 hover/拖拽）→ alpha dirty + matrix 写，零重建。
- **顺序保证**：数据先建后包装 state；材质每帧**去重一次**比较（O(distinct materials)，D19 缓解）。
- 组合后仍守恒的不变量：**管线数 ≈ 状态变体×program×槽位**（与几何/材质数无关）；
  顶点上传只发生在 revision 变化；动态小数据（alpha/UBO）只在 dirty 时回传。

## 4. 已知缺口与对策（不只是材质）

| 缺口 | 现状 | 对策 |
|---|---|---|
| **材质属性热改** | 已修（值 DYNAMIC+dirty，2026-09-08） | 回归测试 `MaterialPhongValueIsDynamic` |
| **D8**：program 路径 opacity | 颜色数组静态、program 拥有 opacity | 定版 ABI：由 program 的 per-draw uniform 提供；或回退内置路径 |
| **D10**：改 `ShaderProgram` GLSL | **已修(2026-09-08)**：`ShaderProgram::revision()`（内容版本）+ `clearStages/replaceStages/setStage`；后端 L1(ProgramEntry)、Item(`program_revision`)、L2 哈希均纳入 revision → 改源次帧重建 state（数据复用）并出新变体 |
| **D13**：`updateMaterial` 会换对象（使已绑 DS 失效） | **已修(2026-09-08)**：改为**就地刷新同一缓存对象 + `dirty()`**（抽出 `applyPhongMaterial`）；回归 `UpdateMaterialRefreshesInPlace` |
| 顶点数据热改整份重物化/重传 | 数据/状态解耦已避免“状态变即重传”，但 revision 变仍整份 | 后续：按 `AttributeBuffer` 身份共享 GPU 顶点缓冲 + 局部子集上传（STEP 元素级改动的底座） |
| **D16**：槽内缓存只增不减 | **已修(2026-09-08)**：`variant_cache_`(L2) 上限 256、`program_shader_sets_`(L1) 上限 64，超限即清（只失快路径，不失正确性）+ teardown `clearCache()` |
| 静默失败（坏 program/坏网格） | 回退内置/null，无诊断 | 错误上报 + 降级可见 |
| 真机行为未在 CI 验证 | headless 单测只覆盖 CPU 路径 | 真实 demo 复验 + env 逃生口（见 §5） |

## 5. 工程策略与逃生口

- **优先级原则**：就地写 > 最小层重建 > D22 增量编译 > 全图编译 > deviceWaitIdle 全停。
- **同步延迟分层**：当帧（矩阵/透明度/灯光/视角）→ 次帧（UBO/dirty、新增几何增量编译）→ 全停（RT 重建/移除）。
- **env 逃生口**：`VINE_VSG_INCREMENTAL_COMPILE=1`（实验性增量；当前 vsg context 池未填充会静默空转/崩，默认全图）、
  `VINE_VSG_DIAG_MRT`（variants/commands 诊断）、`VINE_VSG_OWN_WINDOW`（独立窗口调试）。
- **并发模型**：当前单线程（collect→sync→compile→record 均主线程），无数据竞争；多线程编译/上传是后续方向。
- **测试矩阵（device-free）**：管线共享 1k 不变量、DYNAMIC 顶点颜色、材质 UBO DYNAMIC、数据/状态解耦、
  program 共享等 —— 见 `tests/test_vsg/SceneBridgePipelineSharingTest.cpp`。
