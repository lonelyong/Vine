# graphics / vsg 后端性能待办（2026-09-13 立项）

本文记录"大场景 + 相机常动 + 多 pass"下的性能结论与待办项。
来源：一次代码走查（`Scene::collectRenderCommands` / `SceneBridge` / vsg 1.1.16），
**未实测**的数字都标了「估算」。

## 0. 已核实的机制（避免重复调研）

| 事实 | 依据 | 后果 |
| --- | --- | --- |
| 收集的 memo 键 = `(projection×view, eye, 场景内容版本)`，每帧边界失效 | `src/viz/graphics/src/Scene.cpp:398-420` | **相机一动每帧必 miss** ⇒ 每帧一次全树走 + 剔除 + 排序 |
| 走查实测（debug、2000 节点、20 次均值） | `src/plugins/gfx_backend_vsg/docs/data-flow.md` D27 | 全走 **17.1 ms** vs memo 复用 **0.10 ms**（170×） |
| 每个未剪掉的 geometry 算局部 AABB = **扫全部顶点**，且无缓存 | `src/viz/graphics/src/Geometry.cpp:238`（`transformBox(localBounds(this), worldMatrix())`） | 大网格场景里"收集"的成本是 O(可见顶点数)/帧，不是 O(节点数) |
| 剔除是 CPU 侧节点级 AABB（p-vertex 测试），无 GPU 剔除/无遮挡剔除 | `Scene.cpp:59-113`、`Scene.cpp:207-213` | 根容器罩全场 ⇒ 所有子节点都要先算盒再判掉 |
| 剔除掉的几何体不在命令里；后端无法区分"被剔除"与"已移除"，但 `abandoned()`（App 释放引用）免费覆盖后者 | `SceneBridge.cpp:474-508`、`OwnedCache.hpp:29` | 缺席 = 保留节点从 root 摘下，不销毁、不重传；缺席 > 600 次同步才淘汰 |
| 缺席计数器是**每内容槽**（每 bridge）一份，不是每帧全局 | `VsgContentSlot.cpp:325`、`SceneBridge.cpp:179` | 600 帧 = 6 s @100 fps = 0.6 s @1000 fps；与渲染帧率耦合 |
| `evictAbsentItems()` **每帧无条件扫整个 cache**，每条目一次原子 `useCount()` | `SceneBridge.cpp:474-508` | 每帧 O(曾经出现过的 geometry 数)；漫游会把 cache 撑到全场景 ⇒ 正反馈 |
| 声明数据/状态**解耦**：`data_dirty` 只看 geometry revision / topology / loc2 路径；`state_dirty` 只看 material/texture/state/program —— **两者都没有相机项** | `SceneBridge.cpp:347-359` | 相机移动**不触发**任何重建 |
| 每个内容槽一个 `SceneBridge`，`shared_objects_` 也是每 bridge 一份 | `SceneBridge.cpp:78`、`VsgContentSlot.cpp:325` | 多槽 ⇒ 保留节点/VkBuffer/上传各一套，管线与描述符也不共享 |
| 重建时**派生通道每次重算**：无法线 ⇒ `makeNormals/makeIndexedNormals`（O(V+F)）；无 loc2 颜色 ⇒ `makeWhiteColors(V)`（16 B/顶点）；无 UV ⇒ `makeZeroTexcoords(V)`（8 B/顶点） | `SceneBridgeGeometry.cpp:186-271`、`VsgSceneRules.cpp` | 改一个通道 = 整个几何体的账（全通道重传 + 派生重算） |
| vsg 的重传粒度是**一条 `BindVertexBuffers` 命令**：任一 array 脏 ⇒ 该命令全部数组一起重新预留+重传 | `build/_deps/vsg-src/src/vsg/commands/BindVertexBuffers.cpp:84-108` | 想要"只传变了的通道"必须**每通道一条 bind 命令** |
| 设备内存池化：一条命令的数组进**同一块池 buffer**、按 offset 分片 | `build/_deps/vsg-src/src/vsg/vk/Context.cpp`（`createBufferAndTransferData` → `deviceMemoryBufferPools->reserve`） | 每命令的分配很便宜；重建的真实成本 ≈ 拷贝字节数 |
| 每帧动态数据检查 = 已注册的动态 BufferInfo 逐个比 `modifiedCount` | `build/_deps/vsg-src/include/vsg/state/BufferInfo.h:69`、`vsg/app/Viewer.cpp:402` | 全量 DYNAMIC 化 = 每帧 O(#动态数组) 次比较 |
| 上传批量：一帧一个 staging buffer，按 `(VkBuffer, offset)` 去重，多区域一条 copy | `build/_deps/vsg-src/src/vsg/app/TransferTask.cpp:110-177` | 稳态帧零传输；新数据的成本是 Σ字节 |
| `AttributeBuffer = {values, components}`，**无 offset/子区间**；`aliasArray` 一律 offset 0 | `src/viz/graphics/sdk/vine/graphics/Geometry.hpp:52-…`、`VsgSceneRules.hpp`（`aliasArray`） | "一个 arena buffer + 每 geometry 一段"**目前表达不了** |
| vsg 的 **BufferInfo 是 per-command 的**：新建数据节点就建新 BufferInfo/Buffer，即使里面装的是同一个 Data 对象 → 仍然从头 reserve + 重拷 | `vsg/commands/BindVertexBuffers.cpp:45`（构造时 per-array 建 BufferInfo）、`vsg/vk/Context.cpp`（`createBufferAndTransferData` → 池 reserve）、`vsg/state/BufferInfo.h:69`（`requiresCopy` 比 modifiedCount） | 想只重传一个通道，**必须保留承载它的命令对象**、只把那个通道的 `data` 换掉（见 P6） |
| 同一 buffer 被多个 geometry 别名 ⇒ 每个 geometry 各自的 `vsg::Array`/`BindVertexBuffers`/`BufferInfo`（内容不去重） | vsg `BindVertexBuffers::assignArrays` per-command 建 `BufferInfo` | CPU 侧零拷贝共享，GPU 侧各自上传（**估算**，需实测确认）；**只共享 `vsg::Array` 对象还不够** —— 每条命令各自建 BufferInfo、各自 reserve ⇒ 必须共享**承载 BufferInfo 的命令对象**。**已修（P9，2026-09-13）**：别名自模型缓冲的流（位置/作者法线/作者 UV/四分量 loc2 颜色/索引）改为经会话级 `VsgMeshResourceCache` 取同一条 bind |
| 作用域不对称：**材质全局，纹理原本每 bridge 一份** | `VsgContentSlot.cpp:165`（`setMaterialManager(&persistent.materialManager)`）、`VsgRenderer.cpp:388-391`（shutdown 时 `clear()` 掉死设备引用） | 原本：同一张纹理被 N 个槽采样 = N 份 image + N 次上传。**已修（P8，2026-09-13）**：`VsgRendererState::texture_cache` 会话级 + `SceneBridge::setTextureCache()` 注入每个槽 |
| `VsgTextureCache::releaseAbandoned()` **原本没有生产调用点**；`SceneBridge::clearCache()` 也不清纹理缓存 | `VsgTextureCache.cpp:400`（实现）、`tests/test_vsg/TextureCacheTest.cpp:127`（单测）、`SceneBridge.cpp:228-245`（clearCache） | 原本：不再使用的纹理只等 256 容量 FIFO 淘汰或拆槽才释放。**已接线（P8）**：帧级 `releaseAbandonedCaches()` → `textureCache().releaseAbandoned()` |
| **跨槽共享 `shared_objects_` 不可行（实测）**：vsg 的 `GraphicsPipeline::compile` 复用已有实现时**只比 `_pipelineStates`，不比 render pass**（`build/_deps/vsg-src/src/vsg/state/GraphicsPipeline.cpp:177`，实现用 `context.renderPass` 创建于 `:218`），而后端刻意按 pass 变体建不同的 `VkRenderPass`（§5.4） | 实验：把会话级 `SharedObjects` 注入每个槽的 bridge 后，`scripts/vsg_selftest_evidence.sh` 的 policy-churn 相位失败（“depth target's centre holds 0.0000, expected ~0.0249”）；回退后 `RESULT: PASS`（47 行一致） | 第二个 view 会拿到用不兼容 render pass 编译的 pipeline ⇒ **槽的管线注册表必须私有**（已把原因写进 `VsgContentSlot.cpp` / `VsgContentSlot.hpp` / `VsgRenderTargetEntry.hpp` 的注释） |

## 1. 待办项（按建议顺序）

> 执行顺序（2026-09-13 拍定）：**P1 / P2 / P3 排到最后**；先做共享与局部更新一类。**P4 已否决**（见下）。

| 编号 | 项目 | 现状 | 目标 | 代价/风险 | 状态 |
| --- | --- | --- | --- | --- | --- |
| **P1** | 保留策略：**容量 LRU** + 缺席窗口可配置（帧或秒） | 淘汰只看"缺席 600 次同步"，与帧率耦合，且不是内存上限 | 内存真正封顶；"仍在场景但长期不可见"的条目有归宿；与帧率解耦 | 需选 LRU 键（条目数/字节）；窗口单位要兼顾确定性测试 | 待办 |
| **P2** | sweep 改**候选表**并提到**帧级一次**（本帧所有 pass 的并集都没收集到才计缺席） | 每槽每帧无条件扫整个 cache | 每帧 O(缺席数)；语义变成"这一帧没有任何 pass 画它" | 需要 `VsgRendererState` 级别的帧级 seen 登记（各 bridge 汇报）；要定义"某槽本帧没画"的语义 | 待办 |
| **P3** | **局部 AABB 缓存**（键 = positions buffer 地址 + revision） | 每个 geometry 每帧重扫全部顶点算局部盒 | 收集侧 O(顶点)/帧 → O(1)/帧 | 缓存失效依赖"改数据就 bump revision"的既有契约 | 待办 |
| **P4** | ~~`shared_objects_` 提到 session 级~~ | 每槽一套管线/描述符 | — | — | **已否决（有实测证据，见下）** |
| **P5** | **派生通道缓存**：法线 / 白 / 零 UV 跨重建保留 | 每次数据重建都重算重分配（36 B/顶点） | 重建时省 O(V) 计算与分配 | 键：白/零 UV = 顶点数；派生法线 = positions 与 indices 的**缓冲区指针 + revision** | **已完成（2026-09-13）** |
| **P6** | **per-location 变更检测 + 拆绑定**实现局部上传 | 一个 `revision_`，一变全量重建 | 只重建/只重传脏通道（改位置省 ~75% 字节，改索引省 ~93%） | 语义分工：`Geometry::revision()` = "变了"，逐流快照（`Buffer::revision()` + 指针 + 形状）= "变了哪一路"；刷新必须被快照**解释**，否则回退重建 | **已完成（2026-09-13）** |
| **P7** | `AttributeBuffer::offset`（scalar 计）→ 打通 `scalars()/xyz()/vertexCount()`、后端 aliasing、`DrawIndexed(firstIndex)` | arena 切片不可表达 | "一个大 buffer、每 geometry 一段"可行 | 新公开 API；要贯通绑定/裁剪/包围盒 | 待办 |
| **P8** | **纹理会话级全局化** + 接上 `releaseAbandoned()` 清扫 | 纹理缓存每 bridge 一份（同纹理 N 份 image + N 次上传）；`releaseAbandoned()` 无生产调用点，`clearCache()` 也不清它 | 会话级一份纹理资源；帧级 sweep 接上；注入点照 `materialManager` 的样子加 | 需给 `SceneBridge` 加注入点；设备重建要处理（建议**会话级**而非跨会话）；描述符集/管线仍在 per-bridge 表里 ⇒ 收益只一半（需 P4，而 P4 已否决） | **已完成（2026-09-13）** |
| **P11** | 材质释放被**互持**卡住：材质管理器条目与桥的 variant 模板条目各自持有同一个 Material，而两边都用 `useCount() <= 1` 判定"只有我还持有它"⇒ 两边都不放手 | 二者都用 `OwnedCacheEntry` / `OwnedPairCacheEntry`（`OwnedCache.hpp:29,156`），实测：App 丢掉材质后 `useCount == 2`（两个缓存各一），`VsgMaterialManager::releaseAbandoned()` 与桥的 sweep 都返回 0 | 让"App 放手"能被可靠观察到（候选：variant 条目对 Material 持**弱**引用 + 显式失效事件；或让材质管理器成为唯一权威，桥订阅其释放） | 改成弱引用会动摇"条目拥有它的键"这条防悬空规则，必须连地址复用风险一起设计；且影响 `VsgMaterialManager` 的公有接口语义 | 待办 |
| **P9** | 会话级 **`MeshResourceCache`**（网格资源共享，**承接 P4 的目标**） | 每几何体各自别名数组 + `BindVertexBuffers` + `BufferInfo` + 池区间 + 上传；同一 mesh 的 k 个实例 = k 份 + k 次上传 | 键 =(每通道 buffer 地址+components+数量, 索引 buffer+数量, **layout hash**) → 共享 `{arrays, BindVertexBuffers, BindIndexBuffer, DrawIndexed}`；条目**拥有** `vine::Buffer`；先只对"无 opacity 载体"（自定义 program 路径）开放 | 必须共享**承载 BufferInfo 的命令对象**，否则 vsg 每条命令各自 reserve；共享后 `clearCache()` 不再释放本槽资源 ⇒ 需全局 LRU/sweep（与 P1/P2 合并）；内建路径不可用（见 P10） | **已完成（2026-09-13）**：键 = `binding + components + Buffer 地址 + Buffer::revision() + 元素数`（**没带 layout hash**：键就是流，同一流在不同布局下也应是同一条 bind）；共享范围比原计划大（**内建路径也共享**，只有白载体/零 UV/派生法线/三分量色不共享） |
| **P10** | 把 opacity 从顶点色移到 push constant / instance 属性 | 内建路径 binding 2 是**每 drawable 的白 DYNAMIC 载体** ⇒ data 节点无法共享 | 内建路径也能共享网格资源（解锁 P9） | 改变 opacity 承载方式，影响 shader 契约、既有测试与 selftest 证据行 | 待办 |

### 已否决

| 想法 | 否决理由 |
| --- | --- |
| **P4：把 `shared_objects_`（管线状态注册表）提到会话级** | 会让两个槽共享 `vsg::GraphicsPipeline` 对象，而 vsg 的 per-view 实现复选**不比 render pass**（`GraphicsPipeline.cpp:177`），后端又刻意按 pass 变体建不同的 `VkRenderPass` ⇒ 第二个 view 拿到用不兼容 render pass 编译的 pipeline。**实测**：注入会话级表后 selftest 的 policy-churn 相位失败（深度读到 0.0000，期望 ~0.0249）；回退后门禁 PASS。正确的跨槽去重范围是**不携带 render pass 的对象**（缓冲区/网格资源），即 P9 |
| 收集时**连视锥外的也一起枚举**（为了区分"被剔除"与"已移除"） | 视锥早退正是剔除省钱之处；全量枚举要**不剪枝走完整棵树** ⇒ 每帧 O(全部节点)，与目标相反。"已移除"已由 `abandoned()` 免费覆盖；如需更强信号，可只对**缺席候选**做 O(depth) 父链/可见性检查 |

## 2. 需要实测的数字（还没有）

| 问题 | 怎么测 |
| --- | --- |
| 单个几何体重建 / 全场景重建 / 稳态帧各多少 ms | headless 合成基准：`N` 几何体 × 每几何体 `M` 顶点，量①首次 sync ②全部 bump `revision()` 那一帧 ③稳态帧；用 `VINE_VSG_DIAG_MRT=1`（per-slot `commands/created/rootChildren/variants`）+ 自计时 |
| 拆绑定（P6）后的帧率影响 | 同上基准，比较"1 条 bind"与"每通道 1 条 bind"的录制耗时 |
| 全量 DYNAMIC 化的每帧检查成本 | 量 `requiresCopy` 检查数与帧时间 |
| 同名/同内容 buffer 是否在 GPU 侧各占一份 | 统计 VkBuffer 数或池 `totalReservedSize()`（当前只有代码链推断） |
| 命中 P5 缓存 vs 每次重算的**实测耗时**差 | 同一基准下比较那两种重建帧（预期差在 CPU 推导，不在上传） |
| 纹理/mesh 共享后省下的设备内存与上传次数 | 基准：同一纹理被 N 个槽采样、同一 mesh 被 k 个实例绘制，统计池 `totalReservedSize()` 与上传字节 |
| 全局缓存的 sweep / 淘汰成本 | 每帧 sweep 的条目数与耗时（P2 候选表方案前后对比） |

## 3. 顺带发现（非性能，但值得记住）

- `collectSceneCommandsNoCull` 在代码里**不存在**（文档漂移已在 `gfx_backend_vsg.md` §7.1 与
  `.ai/design/vsg-target-unification.md` 修掉）。现在 `initialize()` 只建窗口 target 与空图，
  内容槽按 pass 惰性创建，几何子树走增量编译（`compilePendingViews`）。
- 首帧就被剔除的几何体**不会预建**：第一次进入视锥那一帧才建 + 编译（会有一帧抖动）。
- 缺席期间 bump 的 `Geometry::revision()` 不会被消费（`Item::revision` 不更新），回来那一帧才重建。
- **缺口已修（P8，2026-09-13）**：`VsgTextureCache::releaseAbandoned()` 原本在生产路径无调用者 —— 现在由 `SceneBridge::releaseAbandonedCaches()`（帧级）调用；新增公有诊断 `SceneBridge::textureCount()` 使其可无设备断言。
- **P8 验证**：单测 `ADroppedTextureIsReleasedByTheFrameSweep`（扫描接线）、`TwoBridgesShareTheInjectedTextureCache` / `TwoBridgesWithoutInjectionUploadSeparately`（共享与对照，按**被采样的 ImageInfo 指针**断言）；mutation 各一次（去掉 sweep 行 / 让注入变成 no-op）→ 恰好目标测试红。运行时门禁：selftest 证据 47 行一致、lavapipe 校验层 0 VUID。
- **P6 索引流已完成（2026-09-13）**：`SceneBridge::ChannelKey`（每通道的 `位置/分量/缓冲指针/revision/元素数`）作为"这份数据是从哪些流建出来的"快照，随 `Item` 保留；`indexKeyOf()` / `channelKeysOf()` 取当前快照。当一次 revision 变动里**顶点通道快照完全一致**、而索引流换了缓冲（**索引数不变**）时，走快路径：`BindIndexBuffer::assignIndices(aliasArray(...))` **原地替换**该流 —— vsg 只重建/重拷这个 BufferInfo（索引字节），不碰任何顶点通道。数据节点与 `MatrixTransform` 都保持原对象（只重新进一次编译遍）。
  两个闸门（缺一不可）：① 布局必须能被别名路径读（location 0 恰好三分量），否则顶点数算不对；② 新索引必须**逐个在范围内** —— 否则必须交给重建路径，由它报 `GeometryRejected` 并拒绝绘制。闸门②是被既有测试抓出来的：`DiagnosticsTest.RejectedGeometryIsReportedOncePerRevision` 一开始变红，因为快路径把越界索引直接换了进去。
  验证：`tests/test_vsg/SceneBridgeDataRebuildTest.cpp` 新增 4 条（索引专用编辑只换索引、两者同改必须重建、索引数变必须重建、越界必须拒绝）；mutation 四条（关掉快路径 / 去掉索引数闸门 / 去掉顶点快照闸门 / 去掉越界闸门）各自恰好目标测试变红。
  剩余（P6 的另一半）**已完成（2026-09-13）**：数据节点改成**每个 canonical 通道一条 `BindVertexBuffers`**（绑定号仍
  是 0/1/2/3，自定义通道共用第 5 条，绑定号 4+），并把手里的 bind 存进 `RetainedBinds`。于是一次只改了某几路字节的
  revision 可以**原地刷新**：对变化的通道 `assignArrays({新数组})`（新 BufferInfo ⇒ vsg 只重建/重拷这一路），索引流同
  理。数据节点与 `MatrixTransform` 都保持原对象，只重新进一次本帧编译遍。
  · 通道→名字的配对也一并改成**按绑定号**（`boundArraysOf` 由 firstBinding 索引），所以拆命令不影响 shader 契约。
  · 派生法线是**耦合**的：位置变了且几何体不作者法线时，必须重新推导（刷新路径里做了，并同步 P5 缓存）。
  · 两个必须的闸门：① 形状必须不变（通道集合/分量/元素数；位置 0 必须仍是可别名布局）；② revision 必须**被快照解释**
    （至少一路变了），否则回退全量重建 —— 闸门②是被既有测试抓出来的（`ManuallyReportedRevisionRebuildsTheDataNode`
    与 `DiagnosticsTest.RejectedGeometryIsReportedOncePerRevision` 先变红，因为"改了但哪路都没变"被当成了 no-op）。
  · 验证：`SceneBridgeDataRebuildTest` 9 条（索引单独刷新、通道单独刷新、顶点+索引同时刷新、作者通道单独刷新、派生法线
    跟随位置、形状变必须重建、未解释的 revision 必须重建、越界必须拒绝、派生通道复用）；mutation 四条（关掉刷新 /
    去掉"未解释就重建" / 派生法线不失效 / 忽略形状比对）各自恰好目标测试红。
- **P9 已完成（2026-09-13）**：别名自模型缓冲的流收敛成**一条 bind**（= 一份设备缓冲 + 一次上传）。
  · 范围：位置 / 作者法线 / 作者 UV / **四分量** loc2 颜色 / 索引流；白载体（alpha 是 per-drawable 的）、零 UV、
    派生法线、三分量（要打包）颜色都**不共享**；自定义通道**暂不共享**（它们共用一条命令，身份是布局）。
  · 键 = `binding + components + Buffer 地址 + Buffer::revision() + 元素数`；条目持有 bind → 数组 → 模型缓冲（字节被留住）。
  · **第二个闸门（被既有测试抓出来的）**：共享条目里的字节副本是插入那一刻拷的，而 `Geometry::revision()` 允许
    "借用的模型缓冲被改于渲染器背后"（`ManuallyReportedRevisionRebuildsTheDataNode` 正是这种情形：只 bump geometry，
    没有任何流变）。所以一次重建先问"这条 revision 有没有哪条流能解释"（`streamsMatch()` 逐键比身份）：**能解释**才走
    共享表，**不能解释**就建自己的 bind、重新读一遍模型字节。少了这条闸门 = 静默画上一帧的网格（该测试先变红）。
  · 刷新**绝不原地改共享 bind**（那会把 peer 的流一起换掉）：走 `getOrCreate*(新 key, 新数组)` 得到新条目，同一帧里
    其它刷新到同一 revision 的 drawable 命中同一条目（一起只拷一次），命令列表里只换那个**槽位**。
  · 未对共享条目做"同 key 重新指向调用者数组"：那会退化成"每次建节点都拷一次"（增量加载场景每帧一条 drawable 就
    每帧拷一次），所以选择"不能担保就别共享"。
  · 验证：新套件 `tests/test_vsg/MeshResourceCacheTest.cpp` 11 条（两 drawable 同 bind / 不同流不同 bind / BUILT 通道
    绝不共享 / 索引共享 / 重新填充不拿旧 bind / 无人读的条目被释放 / peer 的 bind 不被刷新改动 / 作者四分量色共享而三分量
    不共享 / 未解释的 revision 自己建 bind / 两个未注入的 bridge 互不共享 / 容量恰好封顶在 512 且裁剪后仍能共享）；
    mutation 七条各自恰好目标测试红；运行时门禁：test_vsg 213 全绿、test_graphics 223 全绿、selftest 证据 47 行一致、
    lavapipe 0 VUID。
  · 仍未做：自定义通道共享（要先把它们拆成每通道一条命令）、内建路径的颜色槽（P10）、arena 切片（P7）。
- 与 P4 的区别值得记住：**纹理能跨槽共享，管线不能** —— vsg 的 image/imageview/sampler 是设备级，而 `GraphicsPipeline` 的每个 viewID 实现携带着当时 view 的 render pass。
- 反例可以参考：材质管理器是**跨会话**全局（`persistent.materialManager`），shutdown 时显式 `clear()` 掉对死设备的引用 —— 全局缓存必须正面处理"设备身份"这件事。
- **已接线（2026-09-13）**：`SceneBridge::releaseAbandonedCaches()` 现在会调 `textureCache().releaseAbandoned()`（在此之前它没有生产调用点），并新增公有诊断 `SceneBridge::textureCount()`；单测 `SceneBridgeCacheOwnershipTest.ADroppedTextureIsReleasedByTheFrameSweep`；mutation 验证：去掉那一行后恰好该测试红。
- 同一个测试暴露了 P11："App 丢掉材质"在当前引用链下**无法被观察**（材质管理器条目与 variant 模板互持），所以那个测试走的是 SDK 的显式 `releaseMaterial()` 路径。
- **P5 已完成（2026-09-13）**：`SceneBridge::DerivedChannels`（header 内嵌结构，随 `Item` 保留）缓存三个 "BUILT 通道"：白载体 / 零 UV 按顶点数，派生法线按 `(positions 指针, positions revision, indices 指针, indices revision, 顶点数)`。
  注意：**复用数组对象并不省上传**（新数据节点会建新的 `BufferInfo`，vsg 按命令重拷），省的是 O(V) 计算与分配 —— 上传字节要靠 P6/P9。
  验证：新套件 `tests/test_vsg/SceneBridgeDataRebuildTest.cpp`（3 条：无关重建复用、换 positions 必须重算、顶点数变必须重建），mutation 三条各自恰好目标测试变红。
