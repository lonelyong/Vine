# graphics / vsg 后端性能待办（2026-09-13 立项）

> **2026-09-15 更新（审查轮次，逐条见 `.ai/design/graphics-vsg-audit.md`）**：
> - **P3 已完成**：`Geometry` 现在缓存局部包围盒（键 = positions 缓冲指针 + 缓冲 revision + 段 + geometry revision）。
>   此前 `BoundsCache` 只能保证"每个叶子算一次盒"，而**求 root 的盒必须先求所有叶子的盒** ⇒ 每次收集 = O(全部节点)
>   + **O(全部顶点)**，与视锥无关；相机一动 memo 必 miss ⇒ 每帧全场景扫顶点。可观测：
>   `Geometry::localBoundsComputationCount()`；门禁 `SceneTest.TheLocalDataBoxIsComputedOnceAndRecomputedWhenTheDataChanges`。
> - **每叶 3 次祖先上行 + 一次 vector 分配已去掉**：状态折叠改**自顶向下**（`InheritedState`）。
> - **命令表不再每 pass 复制**：`Scene::collectRenderCommandsShared()`（不可变共享表）；只有设了 program override
>   的 pass 才 fork（`SceneTest.TheSharedCollectionIsOneListAndTheOwningSpellingCopiesIt`）。
> - **每帧分配又少两处**：`RenderEngine::validateWiring()` 改声明驱动（`wiringValidationCount()`）；
>   内容槽的 `Vsg::ViewportState` 改"一个、原位更新"（`ContentSlotViewportTest`）。
> - **内存可观测**：`VsgDrawBlockPool::Stats::bytes`、`VsgRetentionStats::{slot_bytes,mesh_streams,textures}`；
>   mesh/纹理缓存的**字节**统计与可配上限仍未做（需要 §2 的实测数字）。
> - **工程坑**：改了 `libviGraphics` 里类的布局后**只 build 目标**会留下陈旧二进制（`vsg_backend_selftest`
>   旧布局 + 新库 ⇒ `malloc(): largebin double linked list corrupted`）——**证据门禁前必须整包 build**。
> - **本环境的 `http(s)_proxy` 是坏的**（TLS 握手中断连），**直连正常**：reconfigure 时 FetchContent 去
>   `git fetch` spdlog 会失败 ⇒ 已在构建目录设 `FETCHCONTENT_FULLY_DISCONNECTED=ON`
>   （`build/CMakeCache.txt`，gitignore）；`git push` 用 `env -u http_proxy -u https_proxy ...` 绕。

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
| 剔除掉的几何体不在命令里；后端无法区分"被剔除"与"已移除"，但 `abandoned()`（外侧释放引用）免费覆盖后者 | `SceneBridge.cpp`（`releaseAbandonedGeometries`）、`OwnedCache.hpp:29` | 未画 = 保留节点从 root 摘下，不销毁、不重传；只有 `abandoned()`（外侧无人持有）才淘汰 —— **2026-09-14 删掉 600 次同步阈值**（移动中的节点同样不在帧里，会被误删） |
| ~~缺席计数器是**每内容槽**（每 bridge）一份~~ **已删（2026-09-14）**：候选表按槽维护，但释放判据读会话级份额 | `SceneBridge.cpp`（`updateUndrawnCandidates`） | 与渲染帧率无关了 |
| `evictAbsentItems()` **每帧无条件扫整个 cache**，每条目一次原子 `useCount()` | `SceneBridge.cpp` | 每帧 O(曾经出现过的 geometry 数)；漫游会把 cache 撑到全场景 ⇒ 正反馈。**已改（2026-09-13）**：改走候选表 `undrawn_`（见 P2），正反馈消除 |
| 声明数据/状态**解耦**：`data_dirty` 只看 geometry revision / topology / loc2 路径；`state_dirty` 只看 material/texture/state/program —— **两者都没有相机项** | `SceneBridge.cpp:347-359` | 相机移动**不触发**任何重建 |
| 每个内容槽一个 `SceneBridge`，`shared_objects_` 也是每 bridge 一份 | `SceneBridge.cpp:78`、`VsgContentSlot.cpp:325` | 多槽 ⇒ 保留节点/VkBuffer/上传各一套，管线与描述符也不共享 |
| 重建时**派生通道每次重算**：无法线 ⇒ `makeNormals/makeIndexedNormals`（O(V+F)）；无 loc2 颜色 ⇒ `makeWhiteColors(V)`（16 B/顶点）；无 UV ⇒ `makeZeroTexcoords(V)`（8 B/顶点） | `SceneBridgeGeometry.cpp:186-271`、`VsgSceneRules.cpp` | 改一个通道 = 整个几何体的账（全通道重传 + 派生重算） |
| vsg 的重传粒度是**一条 `BindVertexBuffers` 命令**：任一 array 脏 ⇒ 该命令全部数组一起重新预留+重传 | `build/_deps/vsg-src/src/vsg/commands/BindVertexBuffers.cpp:84-108` | 想要"只传变了的通道"必须**每通道一条 bind 命令** |
| 设备内存池化：一条命令的数组进**同一块池 buffer**、按 offset 分片 | `build/_deps/vsg-src/src/vsg/vk/Context.cpp`（`createBufferAndTransferData` → `deviceMemoryBufferPools->reserve`） | 每命令的分配很便宜；重建的真实成本 ≈ 拷贝字节数 |
| 每帧动态数据检查 = 已注册的动态 BufferInfo 逐个比 `modifiedCount` | `build/_deps/vsg-src/include/vsg/state/BufferInfo.h:69`、`vsg/app/Viewer.cpp:402` | 全量 DYNAMIC 化 = 每帧 O(#动态数组) 次比较 |
| 上传批量：一帧一个 staging buffer，按 `(VkBuffer, offset)` 去重，多区域一条 copy | `build/_deps/vsg-src/src/vsg/app/TransferTask.cpp:110-177` | 稳态帧零传输；新数据的成本是 Σ字节 |
| `AttributeChannel = {values, components}`，**无 offset/子区间**；`aliasArray` 一律 offset 0 | `src/viz/graphics/sdk/vine/graphics/Geometry.hpp:52-…`、`VsgSceneRules.hpp`（`aliasArray`） | "一个 arena buffer + 每 geometry 一段"**原本表达不了**。**已修（P7，2026-09-13）**：`offset`/`scalarCount`（标量计）+ `slice()`；别名按 offset 换算字节起点；索引侧用 `DrawIndexed(firstIndex)` 而绑定仍是整段缓冲 |
| vsg 的 **BufferInfo 是 per-command 的**：新建数据节点就建新 BufferInfo/Buffer，即使里面装的是同一个 Data 对象 → 仍然从头 reserve + 重拷 | `vsg/commands/BindVertexBuffers.cpp:45`（构造时 per-array 建 BufferInfo）、`vsg/vk/Context.cpp`（`createBufferAndTransferData` → 池 reserve）、`vsg/state/BufferInfo.h:69`（`requiresCopy` 比 modifiedCount） | 想只重传一个通道，**必须保留承载它的命令对象**、只把那个通道的 `data` 换掉（见 P6） |
| 同一 buffer 被多个 geometry 别名 ⇒ 每个 geometry 各自的 `vsg::Array`/`BindVertexBuffers`/`BufferInfo`（内容不去重） | vsg `BindVertexBuffers::assignArrays` per-command 建 `BufferInfo` | CPU 侧零拷贝共享，GPU 侧各自上传（**估算**，需实测确认）；**只共享 `vsg::Array` 对象还不够** —— 每条命令各自建 BufferInfo、各自 reserve ⇒ 必须共享**承载 BufferInfo 的命令对象**。**已修（P9，2026-09-13）**：别名自模型缓冲的流（位置/作者法线/作者 UV/四分量 loc2 颜色/索引）改为经会话级 `VsgMeshResourceCache` 取同一条 bind |
| `string(SHA256 "…" var)` 在本机 CMake 4.2.3 上**恒返回空串**（MD5/SHA1/SHA3 同样空）；`file(SHA256 <path> var)` 正常 | 实测（`cmake -P`）：`string(SHA256 "abc" s)` ⇒ `s` 为空；`file(SHA256 <shader> f)` ⇒ 64 位十六进制 | 生成器算哈希**必须走 `file(<algo> <path>)`**；`string(<algo> <string>)` 在本环境不可用 |
| `file(READ <path> var)` **会把 CRLF 归一化成 LF**（要原始字节得用 `HEX`） | 实测：CRLF 写回的 shader 读进来没有 CR；`file(READ … HEX)` 能看到 `0d` | 想守"嵌入文本 == 磁盘字节"必须用 HEX 检查 CR；否则 CRLF 文件会静默变成 LF 版本（与磁盘文件不再逐字节相同） |
| `glslangValidator -V <shader>` **不带 `-o` 会把 `vert.spv`/`frag.spv` 写进当前目录** | 实测：在仓库根跑校验后 `git status` 多出 `vert.spv`/`frag.spv` | 校验脚本必须给 `-o <tmpdir>/x.spv`，否则门禁自己污染工作区 |
| 作用域不对称：**材质全局，纹理原本每 bridge 一份** | `VsgContentSlot.cpp:165`（`setMaterialManager(&persistent.materialManager)`）、`VsgRenderer.cpp:388-391`（shutdown 时 `clear()` 掉死设备引用） | 原本：同一张纹理被 N 个槽采样 = N 份 image + N 次上传。**已修（P8，2026-09-13）**：`VsgRendererState::texture_cache` 会话级 + `SceneBridge::setTextureCache()` 注入每个槽 |
| `VsgTextureCache::releaseAbandoned()` **原本没有生产调用点**；`SceneBridge::clearCache()` 也不清纹理缓存 | `VsgTextureCache.cpp:400`（实现）、`tests/test_vsg/TextureCacheTest.cpp:127`（单测）、`SceneBridge.cpp:228-245`（clearCache） | 原本：不再使用的纹理只等 256 容量 FIFO 淘汰或拆槽才释放。**已接线（P8）**：帧级 `releaseAbandonedCaches()` → `textureCache().releaseAbandoned()` |
| **跨槽共享 `shared_objects_` 不可行（实测）**：vsg 的 `GraphicsPipeline::compile` 复用已有实现时**只比 `_pipelineStates`，不比 render pass**（`build/_deps/vsg-src/src/vsg/state/GraphicsPipeline.cpp:177`，实现用 `context.renderPass` 创建于 `:218`），而后端刻意按 pass 变体建不同的 `VkRenderPass`（§5.4） | 实验：把会话级 `SharedObjects` 注入每个槽的 bridge 后，`scripts/vsg_selftest_evidence.sh` 的 policy-churn 相位失败（“depth target's centre holds 0.0000, expected ~0.0249”）；回退后 `RESULT: PASS`（47 行一致） | 第二个 view 会拿到用不兼容 render pass 编译的 pipeline ⇒ **槽的管线注册表必须私有**（已把原因写进 `VsgContentSlot.cpp` / `VsgContentSlot.hpp` / `VsgRenderTargetEntry.hpp` 的注释） |

## 1. 待办项（按建议顺序）

> 执行顺序（2026-09-13 拍定）：**P1 / P2 / P3 排到最后**；先做共享与局部更新一类。**P4 已否决**（见下）。

| 编号 | 项目 | 现状 | 目标 | 代价/风险 | 状态 |
| --- | --- | --- | --- | --- | --- |
| **P1** | 保留策略：**容量 LRU** + 缺席窗口可配置（帧或秒） | 淘汰只看"缺席 600 次同步" → **机制已删（2026-09-14）**，见 P2 更正 | 内存真正封顶；"仍在场景但长期不可见"的条目有归宿 | 需选 LRU 键（条目数/字节） | 待办（窗口部分作废：释放只看外侧持有） |
| **P2** | sweep 改**候选表**并提到**帧级一次**（本帧所有 pass 的并集都没收集到才计缺席） | 每槽每帧无条件扫整个 cache | 每帧 O(缺席数)；语义变成"这一帧没有任何 pass 画它" | 需要 `VsgRendererState` 级别的帧级 seen 登记（各 bridge 汇报） | **已完成（2026-09-13）**：①候选表——每槽 `absent_` + `last_seen_`（并集就是 `cache_` 的键集，份额收集也读它们），`ageAbsentItems()` 只走候选表 ⇒ 每帧 O(drawn + absent)；②**帧级一次 + 并集语义**——每槽 sync 把自己画的几何报进 `VsgRendererState::geometry_drawn_this_frame`，`submitFrame()` 在所有槽 sync 完后用**并集**给每个槽老化一次 ⇒"本帧没有任何 pass 画它"才算缺席；**副作用（想要的）**：某槽的 pass 本帧根本没跑（被禁用）也会老化，不再把内容钉到会话结束。单桥直驱（测试）没有并集指针 ⇒ 退化为"按本槽自己画的"在 sync 内老化（同一函数，只是 drawn 的来源不同）。窗口语义 = **连续**无 pass 画它 600 帧。守卫：`TheAbsenceWindowAgesTheGeometriesTheFrameStoppedDrawing`（窗口内/超窗口/400→画→400 不淘汰）、`TheAbsenceWindowCountsFramesNoPassDrewTheGeometry`（别的 pass 一直画 ⇒ 永不计缺席；且那些帧会重置窗口）；**三条变异验证过**（不加新缺席到候选 / 回来不重置 / 老化用本槽而非并集）。**实现中踩的坑**：拆函数时把 `cache_.erase(it)` 弄丢了 ⇒ 现有测试（`RetainedCacheOwnsTheGeometryItIsKeyedBy` 等 4 个）立刻红——保留缓存的自持/释放不变量是有测试的。**2026-09-14 更正**：本行里的“缺席窗口 / 帧级并集”已整个删除（见 `.ai/design/vsg-design.md`）——`ageAbsentItems()` → `releaseAbandonedGeometries()`（不再计数、也不再看“本帧有没有 pass 画过”），`VsgRendererState::geometry_drawn_this_frame` / `setFrameGeometrySet()` 随之删除（释放判据是对象级的：缓存之外是否还有人持有）；候选表（`undrawn_` / `drawn_`）保留，只剩成本过滤的作用。上述两个窗口守卫换成：未画 1000 次 sync 仍保留 / 放手当帧回收 / 隐藏 601 帧回来不重建。 |
| **P13** | P11 修好后的**每帧份额收集**（`VsgRenderer::refreshRetainedShares` 走 manager + 每槽的三个缓存） | 每帧 O(所有条目)；**几何那部分**已随 P2 的候选表去掉（份额改读 `last_seen_ ∪ absent_`，不再走 cache），剩下的是 manager 的材质条目 + 每槽有界的 program 缓存（64/64/256） | 先做到**稳定态零分配**（`OwnedShareCounts` 保留哈希节点、只清值；表涨到远超本帧用量才整表丢弃），把代价从"分配 + 哈希"降到"纯哈希"；真正的候选表与帧级一次留给 P2 | 份额必须覆盖**所有**槽（跨槽互持就是 P11 本身），所以不能只收集本槽可见的；新槽在帧中途建立时它的份额不在本帧计数里 ⇒ 该槽退化成本地份额（保守：晚一帧释放，不会早放） | **已完成（2026-09-13，第一步）**：节点复用 + 冷表整表丢弃 + `trackedCount()` 可断言；test_vsg `TheShareCountsForgetTheirValuesButKeepTheirKeys`；两条证据基线 51 行不变。**剩余**：与 P2 合并成一次帧级遍历 |
| **P3** | **局部 AABB 缓存**（键 = positions buffer 地址 + revision） | 每个 geometry 每帧重扫全部顶点算局部盒 | 收集侧 O(顶点)/帧 → O(1)/帧 | 缓存失效依赖"调用者改数据就 bump revision"的既有契约（`Geometry` 与 `Buffer` 都是公告一律手动） | **已完成（2026-09-15）**：键 = positions 缓冲指针 + **缓冲 revision** + 段（offset/scalarCount）+ **geometry revision**（枚举后两项：流变了/几何自己宣布变了都要重算）；世界盒仍每次派生。守卫 `SceneTest.TheLocalDataBoxIsComputedOnceAndRecomputedWhenTheDataChanges`（变异：去掉缓冲 revision 键 ⇒ 红） |
| **P4** | ~~`shared_objects_` 提到 session 级~~ | 每槽一套管线/描述符 | — | — | **已否决（有实测证据，见下）** |
| **P5** | **派生通道缓存**：法线 / 白 / 零 UV 跨重建保留 | 每次数据重建都重算重分配（36 B/顶点） | 重建时省 O(V) 计算与分配 | 键：白/零 UV = 顶点数；派生法线 = positions 与 indices 的**缓冲区指针 + revision** | **已完成（2026-09-13）** |
| **P6** | **per-location 变更检测 + 拆绑定**实现局部上传 | 一个 `revision_`，一变全量重建 | 只重建/只重传脏通道（改位置省 ~75% 字节，改索引省 ~93%） | 语义分工：`Geometry::revision()` = "变了"，逐流快照（`Buffer::revision()` + 指针 + 形状）= "变了哪一路"；刷新必须被快照**解释**，否则回退重建 | **已完成（2026-09-13）** |
| **P7** | `AttributeChannel::offset`（scalar 计）→ 打通 `scalars()/xyz()/vertexCount()`、后端 aliasing、`DrawIndexed(firstIndex)` | arena 切片不可表达 | "一个大 buffer、每 geometry 一段"可行 | 新公开 API；要贯通绑定/裁剪/包围盒 | **已完成（2026-09-13）**：`offset` + `scalarCount`（标量计，0 = 到末尾）+ `slice()`；索引侧 `setIndices(buffer, first_index, index_count)`；切片的身份进共享绑定 key 与派生法线 key；索引 span 变了必须**重建**（span 在 draw 命令里） |
| **P8** | **纹理会话级全局化** + 接上 `releaseAbandoned()` 清扫 | 纹理缓存每 bridge 一份（同纹理 N 份 image + N 次上传）；`releaseAbandoned()` 无生产调用点，`clearCache()` 也不清它 | 会话级一份纹理资源；帧级 sweep 接上；注入点照 `materialManager` 的样子加 | 需给 `SceneBridge` 加注入点；设备重建要处理（建议**会话级**而非跨会话）；描述符集/管线仍在 per-bridge 表里 ⇒ 收益只一半（需 P4，而 P4 已否决） | **已完成（2026-09-13）** |
| **P11** | 材质释放被**互持**卡住：材质管理器条目与桥的 variant 模板条目各自持有同一个 Material，而两边都用 `useCount() <= 1` 判定"只有我还持有它"⇒ 两边都不放手 | 二者都用 `OwnedCacheEntry` / `OwnedPairCacheEntry`（`OwnedCache.hpp:29,156`），实测：App 丢掉材质后 `useCount == 2`（两个缓存各一），`VsgMaterialManager::releaseAbandoned()` 与桥的 sweep 都返回 0 | 让"App 放手"能被可靠观察到 | 改成弱引用会动摇"条目拥有它的键"这条防悬空规则 | **已完成（2026-09-13）**：判据改成"对象上**只剩保留条目**在持有"⇒ `useCount() <= 保留份额数`（`OwnedShareCounts`）。份额是**数据相关**的（一个材质被两个槽画 = 管理器 1 + 各槽模板 1），所以是**数出来的**不是假设的：`collectOwnedShares()` 走遍所有一起清扫的缓存；会话每帧数一次（`VsgRenderer::refreshRetainedShares`）并交给各槽（`SceneBridge::setRetainedShares`），单桥自己驱动时退化为"本桥可见份额 = 自己的缓存 + 材质管理器"。回归测试：App 丢掉材质后一帧内材质真的析构（`TrackedMaterial::alive == 0`）；会话份额 > 单桥份额直接钉住"为什么必须会话级"。**变异验证过**（换回 `<= 1` 即红）。**2026-09-14 后续**：份额改为**每次 sync 的入参**（`SceneBridge::syncRenderCommands(..., const OwnedShareCounts*)`），不再有 `setRetainedShares` 那套指针协议；帧尾清扫前重数一次（`VsgRenderer::releaseAbandonedContent`） |
| **P9** | 会话级 **`MeshResourceCache`**（网格资源共享，**承接 P4 的目标**） | 每几何体各自别名数组 + `BindVertexBuffers` + `BufferInfo` + 池区间 + 上传；同一 mesh 的 k 个实例 = k 份 + k 次上传 | 键 =(每通道 buffer 地址+components+数量, 索引 buffer+数量, **layout hash**) → 共享 `{arrays, BindVertexBuffers, BindIndexBuffer, DrawIndexed}`；条目**拥有** `vine::Buffer`；先只对"无 opacity 载体"（自定义 program 路径）开放 | 必须共享**承载 BufferInfo 的命令对象**，否则 vsg 每条命令各自 reserve；共享后 `clearCache()` 不再释放本槽资源 ⇒ 需全局 LRU/sweep（与 P1/P2 合并）；内建路径不可用（见 P10） | **已完成（2026-09-13）**：键 = `binding + components + Buffer 地址 + Buffer::revision() + 元素数`（**没带 layout hash**：键就是流，同一流在不同布局下也应是同一条 bind）；共享范围比原计划大（**内建路径也共享**，只有白载体/零 UV/派生法线/三分量色不共享） |
| **P10** | 把 opacity 从顶点色移到**每 drawable 的值** | 内建路径 binding 2 是**每 drawable 的白 DYNAMIC 载体** ⇒ data 节点无法共享 | 内建路径也能共享网格资源（解锁 P9） | 改变 opacity 承载方式，影响 shader 契约、既有测试与 selftest 证据行 | **forward 侧已完成（2026-09-13）**：set1/b0 的 `vine_draw` + `VsgDrawBlockPool`（共享缓冲 + **dynamic offset**，每 drawable 一个 80 B 槽，直接写映射内存 ⇒ 改一次 opacity = 4 B、无 transfer）；`model` 不写（走 push）。**内建路径仍用载体**（vsg phong 读 `vine_Color.a`）。剩余：`params` 承载材质值（B2） |
| **P12** | 着色中途切换要**重建着色侧**（不是重开会话） | `setShaderPreset` 以前只写 `persistent.shader_preset`；而 set 是建 slot 时烘的（程序 + 喂哪个光源 + View features） | 宿主给个着色开关，画面当场变 | 丢 slot 必须逐个 `detachSlotView`（否则旧 view 还在画旧 set）；`clearCache()` 前要一次计数设备等待；target 自烘的 `depth_*_shader_set` 也要清 | **已完成（2026-09-13）**：`detail::resetContentShaderSlots` + window 三套 set 重建；门禁 `runLiveDefaultContentProgramSwitchPixelPhase`（同一 slot 三段 42 → 765 → 42，**变异验证过**）；attachments / pass graph / 深度历史不动。**同日**：入口由 `setShaderPreset(preset)` 改为 `setDefaultContentProgram(program)`（枚举删除，见 `graphics.md` 顶端条目） |

| **P14** | 内容着色**双路径**（我们的 set + vsg 内建 set 回落） | `makeContentShaderSet` 两条腿 + `VINE_VSG_BUILTIN` 开关；两套 ABI（属性位置/灯源/VDS/ViewFeatures）要对齐；两条证据基线 | 一套 ABI、一条路径、一条基线 | 删内建基线等于少一个回归面（用自检相位“Pbr == StandardPhong 像素”补上语义） | **已完成（2026-09-13）**：删除 `buildShaderSet()` / `vineForwardShaderEnabled()` / `VINE_VSG_BUILTIN` / `vsg_selftest_builtin_evidence.txt` / `--builtin`；口径随后收紧为**没有有效 shader 就不画**（preset 无 program ⇒ null + 每会话一条 Error；槽无 set ⇒ 每桥一条 Error + drawable 不入图；program 编译失败不再回落）；见 `.ai/design/vsg-custom-shader.md` §11.7 |
| **P16** | canonical 角色陈述"段"只能绕道 `addBuffer` + `AttributeChannel::slice`（加载器要先知道 `attributeLocation`） | 易用性/可发现性差；顶点与索引的 setter 不对称 | 三个重载（`setPositions/setNormals/setTexcoords2` 的 `(buffer, first_vertex, vertex_count)`） | 接口面 +3；必须纯委托否则两边会漂 | **已完成（2026-09-13）**：三个重载纯委托给通用门；守卫 `TheWholeBufferAndTheSegmentSpellingsAgree`；见 `.ai/design/geometry-attribute-storage.md` |
| **P15** | 流（stream）的段描述各写一遍：索引流用散的 `indices_/indices_first_/indices_count_`，属性通道用自己的 `offset/scalarCount` + 各自的长度算术 | 同一条规则（越界钳位 / 0 = 到末尾 / 跟随 buffer 增长）有三份实现，出错都是静默的（少读一个元素照样画） | 一个核心结构 + 一个名字 | 改名要动 ~90 处（机械），并在文档里说清"组合而非基类" | **已完成（2026-09-13）**：核心新增 `BufferSlice<Element>`（唯一的 `resolvedLength`）；`AttributeBuffer` → **`AttributeChannel`**（= `BufferSlice<float>` + stride，`scalarSlice()` / `fromSlice()`）；`Geometry::IndexStream = BufferSlice<std::uint32_t>`；见 `.ai/design/geometry-attribute-storage.md` |
| **P17** | **pass 流程还有第二条路**：同一个 `state.request` 既服务 `beginPass` 作用域，也服务"不开作用域、队列跨帧存活"的直驱（只有 `vsg_selftest/main.cpp` 17 个绘制点用它） | 直驱专属机制共 ~150 行 / 5 个概念都属于这一条路：`SlotKey::cameraOrder`+`sampledTarget` 两个兜底身份、dead-announcement（`target_released`/`target_release_reported`/`takeDeadTargetAnnouncement`/`refuseDeadTargetAnnouncement`+3 个拒画点）、粘性 `pass_protocol_used`、"排队请求跨帧存活"这条规则、直驱专用释放入口 `releaseWindowLayer`（39 行） | 一条路：作用域是唯一驱动方式（SDK 契约收窄）；直驱专属符号全删 | 动 **SDK 契约文本**（`RenderBackend.hpp` 四处措辞，无签名变更）+ selftest 17 个点改用已有 `PassScope`（**必须把 pass 对象提到循环外**：每帧新建 pass = 每帧新槽 ⇒ 退役环账目炸） | **已完成（2026-09-15，设计 §67）**：插件删干净（死符号 grep **0** 命中）、SDK 四处文本收窄、引擎零改动、harness 17 点转作用域（`PassScope`/`FrameScope` 改成 `RenderBackend&`）、`PassProtocolTest` 改写 + 新增"作用域外绘制被拒"；插件新增唯一守卫 `refuseNoPassAnnounced`（每帧只报一次）。**判据**：证据 **55 行逐字节不变**（harness 迁移后与插件删直驱后各验一次）、`test_vsg` 271→**272**、260/82 不变、构建 0 error、lavapipe **PASS**（0 VUID）、反证三组（守卫短路 ⇒ 测试红；`SlotKey::cameraOrder` ⇒ 编译期 `no member named 'cameraOrder'`；提交令牌见 §66.3） |
### 已否决

| 想法 | 否决理由 |
| --- | --- |
| **P4：把 `shared_objects_`（管线状态注册表）提到会话级** | 会让两个槽共享 `vsg::GraphicsPipeline` 对象，而 vsg 的 per-view 实现复选**不比 render pass**（`GraphicsPipeline.cpp:177`），后端又刻意按 pass 变体建不同的 `VkRenderPass` ⇒ 第二个 view 拿到用不兼容 render pass 编译的 pipeline。**实测**：注入会话级表后 selftest 的 policy-churn 相位失败（深度读到 0.0000，期望 ~0.0249）；回退后门禁 PASS。正确的跨槽去重范围是**不携带 render pass 的对象**（缓冲区/网格资源），即 P9 |
| 收集时**连视锥外的也一起枚举**（为了区分"被剔除"与"已移除"） | 视锥早退正是剔除省钱之处；全量枚举要**不剪枝走完整棵树** ⇒ 每帧 O(全部节点)，与目标相反。"已移除"已由 `abandoned()` 免费覆盖；如需更强信号，可只对**缺席候选**做 O(depth) 父链/可见性检查 |

### 宿主表面（C1/A6）未完成项（2026-09-16 登记，编号 H#；过程与判据见 `.ai/memory/graphics.md` 的“交接”节）

> 背景：2026-09-16 把宿主表面归属做成后端自己的事（`VsgHostWindow` 派生 vsg 平台窗口），撤回 `VSG_MAX_DEVICES`／`releaseWindow()`，修好“窗口是黑的”（`valid()/visible()` 漏覆写 ⇒ 整帧不录）。代码已提交（`424e15e`/`143c9f4`/`dce6946`/`44e6ad9`），下列是**还没做完的**。

| 编号 | 项目 | 现状 | 目标 | 代价/风险 | 状态 |
| --- | --- | --- | --- | --- | --- |
| **H1** | **Win32 分支的编译与行为验证**（`VsgHostWindow` 派生 `vsgWin32::Win32_Window`） | 本机 `_WIN32` 不成立 ⇒ **编译器都没跑过**，只做了源码审读（`VSG_DECLSPEC`、采纳分支设 `_windowMapped = true`、析构会 `DestroyWindow` **和** `UnregisterClass(GetClassName(hwnd))` ⇒ 我们“析构先置空 `_window`”是对的） | Windows：build 0/0 + app 门禁 + 自检相位（`mapped=true`、窗口构建数不变、恰好 1 次计数 device stop、两宿主窗口存活）+ 拉伸窗口看画面跟随 | 无法在本环境验证；错误会以“窗口黑/崩溃”形式出现，而不是编译错误 | 待办（最高优先） |
| **H2** | 继承来的 `pollEvents()` 会不会偷 Qt 的事件 | 源码级已确认：事件掩码只在 `createWindow` 分支的 `xcb_create_window` 里设，采纳路径我们这条连接收不到 X 事件 | 真机上边缩放/拖拽边点菜单，确认 Qt 事件不丢 | 只在真实交互下暴露 | 待办（真机复验） |
| **H3** | 新副作用：vsg 平台窗口构造会调 `_initXdnd()`，在**宿主窗口**上写 `XdndAware` 属性 | 幂等，且 Qt 在 X11 本来也用 XDND | 往窗口拖一个文件验证无害；若有害则构造后清属性（或拒绝 vsg 构造） | 可能影响 Qt 的拖放行为 | 待办（低成本验证） |
| **H4** | 宿主侧 `RenderControl::initializeBackend()` 仍做 `engine->shutdown()` + `initialize()` | C1/A6 只给了后端**搬**的能力，宿主还没用上 | 改成句柄变化时只 `setWindowHandle()` + `initialize()`（**不** shutdown），再 resize/渲染；用 `windowBuildCount()` 断言没重建 | 落在 `src/fw/appfw/src/gui/RenderControl.cpp`；本环境除 app 演示外无自动门禁覆盖 | 待办 |
| **H5** | **`+0.43 s` 的 release(-O2) 复测** | 只有 debug(-O0) 的交错 A/B（已排除代码布局/堆起点等猜测，无热点、帧数无关的一次性开销） | 新开 `build-release/`（`-DCMAKE_BUILD_TYPE=Release`）全量构建，三条已提交二进制交错比墙钟，**先验身份**（`nm -C` + 指纹）再下结论 | 一次全量构建；FetchContent 需 `FETCHCONTENT_FULLY_DISCONNECTED=ON`（见本文件 §0 末条） | 待办 |
| **H6** | 宿主表面这条线的**小收尾（四项）** | ① 自检相位里 `at(128u, 72u, …)`／`256u`/`144u` 与 `pixels_target` 尺寸重复；② 三条**拒答路径零覆盖**（null 句柄 / 非本后端窗口 / 搬移被拒）连同 `Warning` + `UnsupportedRequest` 诊断无断言；③ `VsgRenderer::resize(int,int)` 忽略参数（设计如此，但签名易误读）；④ **app 门禁仍只看 stderr** ⇒ 当天的黑屏被完整放过 | ①从 target 取尺寸或提常量；②各加一条断言（诊断用 `setDiagnosticSink` 收）；③标 `[[maybe_unused]]` 或改注释；④把“渲染区非黑比例 ≥ 阈值”接进 app 阶段（用已入库的 `scripts/xwd2ppm.py`，跨 X 环境可能不稳 ⇒ 做成可选档） | 都是小改动；④有一个阈值选取问题 | **已完成（2026-09-16，四条）**，过程见 `.ai/memory/graphics.md` 顶部：①尺寸提成 `kPixelsWidth/kPixelsHeight`，采样点由它派生；②两条**可达**拒答路径（公告 null 句柄 / 会话不在本后端宿主窗口）原本**静默**，现在各自上报 `Warning` + `UnsupportedRequest`，自检相位各加一条断言（**两条都做过变异**：各自静音 ⇒ 恰好对应那条红）；**第三条**（新窗口的 swapchain 格式不可用）仍**无断言**——它要两个视觉映射到不同格式的窗口，那是驱动属性而非调用方行为，登记为遗留；③覆写改成 `announced_width/announced_height` + 文档写明“公告是 advisory”（SDK 契约本就是 surface > announcement > default）；④app 阶段现在读**渲染区像素**（后端日志带上它渲染的那个窗口句柄；`scripts/xwin2ppm.py` 改为**只依赖 libX11**；app 改**后台**跑以趁活着采样）。判据：`84.90% ≥ 30%`；**变异**（`visible() → false`，即当天黑屏根因）⇒ `0.00%` + `RESULT: FAIL` |

两条当日踩到的坑（非性能，值钱）：① `cmake --build . --target Vine` **不重建插件** `build/plugins/vine/gfx_backend_vsgd.so`（app 运行时 dlopen 它）⇒ 二进制级结论要 `nm -DC <产物> | grep <symbol>` + 时间戳，只验 `bin/Vine` 会得到假阳性；② **抓图工具原来依赖 `xwd`/`xwininfo`（x11-apps），本机没有、`sudo` 又要密码装不上** ⇒ 门禁会在最需要它的地方静默跳过。已改：`scripts/xwin2ppm.py` 经 libX11 `XGetImage` 自己抓（名字从 `xwd2ppm.py` 改过来就是因为它不再用 xwd），`xwd -root` 在 XWayland 下 BadMatch、`-out -` 不支持这些坑随之消失。

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
- **公告一律手动（2026-09-13）**：`Buffer::push_back/append/clear` 里的 `++revision_` 已删除 —— 与
  `Geometry`（更早一批）、`Texture`、`ShaderProgram` 同一条规矩：**被共享的对象自己不推断“内容变了”**。
  理由：buffer 看不见 `data()`/`operator[]`/跨线程这类写入，也不知道一次编辑何时结束，所以自 bump 只能是
  半真话（一条写路径自己公告、下一条静默），而消费者分不出这两者；改成“写的人在编辑结束时 `setRevision(
  revision()+1)`”后，规则是绝对的，粒度也对（**每编辑一次**，不是**每元素一次** —— 以前 append 一个顶点会
  bump 3 次）。
  · 承接方：`Mesh` 是那个写的人，新增 `Mesh::announceChange()`，`addVertex`/`addTriangle`/`clear` 各公告一次
    ⇒ “共享句柄能得知编辑”这条契约不变（`MeshTest.AttributeStorageIsSharedNotCopied` 仍绿）。替换存储
    （`setPositions`）本来就不是“编辑这个 buffer”，句柄持有者要重新取一次句柄。
  · 与 P9 的关系：P9 的第二个闸门（“未解释的 revision ⇒ 不共享”）正好覆盖“写者忘了公告”的旧世界；而
    只 bump `Geometry::revision()`、不 bump buffer 的调用者依旧**正确**（全量重建 + 私有 bind），只是不共享。
  · 验证：`tests/test_core/BufferTest.cpp`（无变异自公告 / reserve 不算内容变 / setRevision 是唯一入口）+
    `tests/test_graphics/GraphicsTest.cpp` 新增 `MeshTest.EveryEditAnnouncesItselfOncePerEdit`；mutation 三条
    （`announceChange` 变空 / 改成每元素公告一次 / `clearAttributes` 不公告）各自恰好目标测试红。门禁：
    test_core 82、test_graphics 224、test_vsg 213、selftest 证据 47 行一致、lavapipe 0 VUID。
    （`test_cppstd` 2 条与 `test_system.MotherboardInfoIsFilled` 在本环境本来红：前者比栈地址的
    `reinterpret_cast`，后者读 WSL 没有的 SMBIOS；两者都不引用 `Buffer`/`Mesh`。）
- 与 P4 的区别值得记住：**纹理能跨槽共享，管线不能** —— vsg 的 image/imageview/sampler 是设备级，而 `GraphicsPipeline` 的每个 viewID 实现携带着当时 view 的 render pass。
- 反例可以参考：材质管理器是**跨会话**全局（`persistent.materialManager`），shutdown 时显式 `clear()` 掉对死设备的引用 —— 全局缓存必须正面处理"设备身份"这件事。
- **已接线（2026-09-13）**：`SceneBridge::releaseAbandonedCaches()` 现在会调 `textureCache().releaseAbandoned()`（在此之前它没有生产调用点），并新增公有诊断 `SceneBridge::textureCount()`；单测 `SceneBridgeCacheOwnershipTest.ADroppedTextureIsReleasedByTheFrameSweep`；mutation 验证：去掉那一行后恰好该测试红。
- 同一个测试暴露了 P11："App 丢掉材质"在当前引用链下**无法被观察**（材质管理器条目与 variant 模板互持），所以那个测试走的是 SDK 的显式 `releaseMaterial()` 路径。
- **P5 已完成（2026-09-13）**：`SceneBridge::DerivedChannels`（header 内嵌结构，随 `Item` 保留）缓存三个 "BUILT 通道"：白载体 / 零 UV 按顶点数，派生法线按 `(positions 指针, positions revision, indices 指针, indices revision, 顶点数)`。
  注意：**复用数组对象并不省上传**（新数据节点会建新的 `BufferInfo`，vsg 按命令重拷），省的是 O(V) 计算与分配 —— 上传字节要靠 P6/P9。
  验证：新套件 `tests/test_vsg/SceneBridgeDataRebuildTest.cpp`（3 条：无关重建复用、换 positions 必须重算、顶点数变必须重建），mutation 三条各自恰好目标测试变红。
- **P7 已完成（2026-09-13）**：通道可以是缓冲里的一段 ⇒ "一个大 buffer、每 geometry 一段"可行（顶点只存在一份，不重打包）。
  · SDK：`AttributeChannel` 加 `offset` / `scalarCount`（都按**标量**计，`0` 长度 = 到缓冲末尾 ⇒ 无固定长度的通道仍然跟着缓冲增长）；
    `slice(values, components, first_vertex, vertex_count)` 用**顶点**说；`floatCount()/vertexCount()/scalars()/xyz()/vec3View()` 只认这一段，
    `offset` 越过末尾 ⇒ 一律当空（不会读邻居）。索引侧 `Geometry::setIndices(buffer, first_index = 0, index_count = 0)` +
    `firstIndex()/indexCount()/indices()`（`indices()` 返回**画的那一段**）。
  · 后端：`aliasArray(buffer, count, offset_scalars)` 把 offset 换算成 vsg `Array` 的**字节**起点；`channelShape/unpackXyz/packColor4/aliasTypedVertexData`
    全走通道访问器（判的是这一段）；`channelKeysOf` 快照带 offset、`indexKeyOf` 带 `first/count`。
  · 索引的设计取舍（值得记）：**绑定别名整段索引缓冲，切片写在 draw 命令里**（`DrawIndexed(firstIndex, indexCount)`）——
    这样同一个索引 arena 的所有 geometry 解析到**同一个共享 key** ⇒ 共享一次索引上传（顶点侧做不到：顶点数组别名到切片，key 必须带 offset）。
    代价：span 变了（同缓冲换一段）**必须重建**（span 在 draw 命令里，原地换 bind 表达不了），闸门 `index_span_changed` 只管这一条，
    "换缓冲 + 换 span" 同时发生才是它真正挡住的情形 —— 第一版测试只覆盖了"换 span"，mutation 直接活下来（SURVIVED），补了第二种情形才咬住。
  · 派生法线缓存 key 加 positions 的 `offset` 与索引的 `first/count`（同缓冲另一段是另一组输入）。
  · 验证：`tests/test_graphics/ChannelSliceTest.cpp` 7 条（切片访问器 / 无固定长度跟着长 / `slice()` 用顶点说话 / 包围盒只覆盖这一段 /
    索引 span 暴露 / 拾取只命中这一段）与 `tests/test_vsg/ChannelSliceTest.cpp` 7 条（绑定从 offset 起且仍在模型内存里 / 同缓冲两段不共享 bind /
    段变了走刷新 / 索引切片用 firstIndex+count 且共享一个 bind / span 变了必须重建（两种情形）/ 越界索引被拒 / 派生法线跟着段走）；
    mutation 六条各自恰好目标测试红（别名 offset / key 带 offset / span 闸门 / draw 带 span / `floatCount()` / `xyz()` 的 offset）。
  · 口径：索引是**段内相对**的（索引 0 = 这段第一个顶点），所以越界检查按这一段的顶点数判；arena 用户要把索引写成段内的。
  · 门禁：ninja 0 error 0 warning；test_core 82、test_graphics 230、test_vsg 220；selftest 证据 47 行一致；lavapipe 0 VUID；诊断格式 0 suspicious。
- **着色器文件化 + 构建期嵌入（P12，2026-09-13）**：GLSL 不再写在 C++ 字符串里 —— 5 个产品 shader 变成真文件
  （SDK 侧 `src/viz/graphics/shaders/{builtin_gbuffer.vert,builtin_gbuffer.frag,builtin_deferred_lighting.frag}`
  —— 当时叫 `gbuffer_geometry.*` / `deferred_light.frag`，见 `.ai/design/vsg-custom-shader.md` 顶部 2026-09-13 命名规则；
  后端侧 `src/plugins/gfx_backend_vsg/shaders/{fullscreen.vert,screen_texture.frag}` —— **该目录已于 2026-09-13 删除**，两段都成了 SDK program：`BuiltinShaders::fullscreenVertexProgram` / `screenCopyProgram()`），构建期生成
  `vine/graphics/EmbeddedShaders.hpp` / `vine/vsg/EmbeddedShaders.hpp`；同时删掉死文件 `flat.*`（含两个提交进仓库的 `.spv`）。
  · 机制：清单在**顶层** `cmake/VineShaders.cmake`（`include(VineShaders)`；生成规则必须在顶层，`tests/test_vsg` 直接编译插件源码，
    要能依赖同一个生成头文件）→ `cmake/VineShaderHelper.cmake`（`v_declare_embedded_shaders` / `v_use_embedded_shaders`）→
    `cmake/v_embed_shaders.cmake`（`cmake -P`，写 `inline constexpr std::u8string_view` + `Entry{name,hash,bytes}` 表）。
  · 为什么用 `-P` + `add_custom_command`：ninja 原生依赖追踪（改 `.glsl` 只重编依赖它的 TU；实测改**生成器**本身也会重新生成），
    避开 `file(READ)` + `CONFIGURE_DEPENDS` 的整包 reconfigure（实测约 15s）。内容没变则不重写头文件（否则 touch 一下 `.glsl` 引发一串重编）。
  · 迁移是**行为中性**的：`scripts/vsg_selftest_evidence.sh` 47 行与基线逐字节相同；lavapipe 0 VUID；test_graphics 230→**234**（+4）、test_vsg 220→**224**（+4）。
  · 新门禁 `scripts/vine_shader_check.sh`：每个 shader × define 变体组合（`VINE_VERTEX_COLOR` / `VINE_DIFFUSE_MAP`，共 4 种）过 glslangValidator +
  嵌入副本的 SHA-256 前缀与字节数必须与磁盘一致 + 每个 `*/shaders/*` 文件必须在清单里。
    mutation 验证：① 加一行非法 GLSL ⇒ 4 个变体全红（恢复后 PASS）；② 只改 shader 不重建 ⇒ "embedded copy is stale"（hash+字节数都报）；
    ③ 加一个未入清单的 shader ⇒ "not listed"；④ 生成器把 hash 截成 8 位 ⇒ 门禁"stale" **且** `EmbeddedShadersTest.BookkeepingAgreesWithTheText` 变红（恢复后 4/4 绿）。
  · 生成器自带两条构建期守卫（都做过 mutation）：源里有 CR ⇒ `FATAL_ERROR`（CRLF 写回的 shader 会红，恢复后绿）；源里出现 `)VINE_GLSL\"` ⇒ `FATAL_ERROR`。
  · `ShaderStage::source` 是 `vine::String`（内部 `std::u8string`）⇒ `String(kX)`；vsg 侧需要 `std::string` ⇒ `asShaderSource(kX)`（`vine/vsg/VsgUtils.hpp`，GLSL 是 ASCII 的逐字节视图）。
  · 后续（P0，§4/§6）：`std_forward.*` 是第一个走新机制的**新** shader；设计见 `.ai/design/vsg-custom-shader.md` §4 / §10。
- **P0.1a 自写前向 shader + ShaderSet（2026-09-13）**：`std_forward.{vert,frag}` + `detail::buildVineShaderSet`（只对 `StandardPhong` 返回非空）。
  · **关键约束（决定了 ABI）**：Vulkan 只保证 **128 字节 push**，而 vsg 的矩阵栈已占满 0..128 ⇒ 延迟全屏路径能把 112 字节光块塞 push（它不需要矩阵）而**前向不能**，前向的光必须走 UBO（set0/binding2，`VineLightsBlock` 112 B）。因此**每 drawable 的唯一数据仍是 vsg 自动推的 modelView**，§4.4 的 dynamic UBO 不是前置条件。
  · **绑定号规则**：`GraphicsPipelineConfigurator::assignArray` 用 `bindingIndex = base + arrays.size()`（按**成功赋值的顺序**），名字未声明就跳过、后面全部前移 ⇒ ShaderSet 的属性**声明顺序**必须与数据节点的绑定顺序一致（位置/法线/uv/颜色/自定义），location 可以不同（我们用自定义契约的 2=色、8=uv）。
  · **define 变体怎么来**：`assignArray`/`assignTexture` 命中带 `define` 的绑定时会 `shaderHints->defines.insert(define)` ⇒ **喂了数据 = 打开 define**，于是“不喂作者色”自然得到不含该属性的变体（将来替掉白载体靠的就是这条）。
  · 门禁：`ForwardShaderSetTest` 6 条（含**两个 stage 门控必须一致** —— 不一致就是未定义输入，Vulkan 不报错）+ `OverlayLightingTest` +3 条（与 push 块光部分逐字段相同 / 无相机全零 / 无光种默认环境光）；四条 mutation 各自咬住目标测试。
  · 口径：本步**不改默认路径**（selftest 证据 47 行逐字节相同、lavapipe 0 VUID）；test_vsg 220 → **233**。
  · 下一步 P0.2：槽级 lights UBO + 描述符集 + opt-in 开关 + selftest 相位（像素级端到端）。
- **P0.2 接线已完成（2026-09-13）**：`VINE_VSG_FORWARD=1` 下整套内容渲染走自写 set，默认仍走内建。
  · 入口唯一：`makeContentShaderSet(...)`（窗口三档深度 + 每个离屏目标都走它）⇒ 不会出现“一半换了新 shader”。
  · 槽级 `ContentSlot::lights_data`（112 B ubyteArray）+ `SceneBridge::setLightsData` 注入 + 每帧 `fillVineLightsBlock` + `dirty()`；
    `buildStateGroup` 里“有块 **且** set 声明了 `vine_lights`”才挂描述符 ⇒ 内建/自定义 program 路径的 set 不受影响。
  · **两个证据基线**：`vsg_selftest_evidence.sh [--forward]`。两条基线的差异**只有 6 个着色数字**（centre 46,8,3 → 34,6,2；
    共享深度相位 5,41,10 → 4,31,8），**覆盖数/深度值/清屏色/诊断计数全同** ⇒ 证明“同一份几何、换了一套着色”而不是画错。
  · lavapipe 新阶段 3d/4 跑 forward 自检（0 VUID + 无 `[selftest] FAIL` + 证据比自己的基线）；帧数由证据脚本统一（15 帧跑 vs 30 帧基线会假红）。
  · mutation 两条：跳过每帧光块 ⇒ forward 基线红（画面变黑）；开关默认改 true ⇒ 内建基线红。
  · 口径：test_vsg 233 → **235**（+2：`makeContentShaderSet` 对全部 preset/深度/色彩数永不为空；开关关闭时拿到内建 set）；lavapipe 整体 PASS。
  · 未做（P0.3 剩余）：去掉 vsg Light/VDS 的 content 用法；几何无作者色/UV 时不喂那两个数组（拿掉白载体、省一条绑定与一次采样）。
- **P0.3 默认转正（2026-09-13）**：`vineForwardShaderEnabled()` 默认返回 true，`VINE_VSG_BUILTIN=1` 退回内建。
  · 自检 `variant 'built-in Phong + …'` → `'default shading + …'`；基线文件语义对调 + 重命名：`vsg_selftest_evidence.txt` = 默认（自写 set），`vsg_selftest_builtin_evidence.txt` = 内建。
  · 脚本：`vsg_selftest_evidence.sh [--builtin]`；lavapipe 3c 跑默认、3d 跑内建 + 两条基线各比一遍。
  · 口径：build 0 error/0 warning；test_vsg 235、test_graphics 234；两条证据基线 PASS；lavapipe 整体 PASS。
  · 收尾（同日）：content 不走 vsg 灯/VDS（forward 时 view `features=0`、不建灯节点、不跑 `setGroupLights`）；`buildStateGroup` 在几何无作者色（且无 UV/无纹理）时不 assign vine_Color/vine_TexCoord0（define 关），属性是否在位并入 L2 variant 的 layout 哈希。
  · 口径：test_vsg 235 → **237**（+2 断言丢属性）；两条证据基线 47 行不变；lavapipe PASS。
- **P0.A 内建前向着色归 SDK（2026-09-13，行为中性）**：`std_forward.*` 移到 `src/viz/graphics/shaders/`；新增 SDK
  `BuiltinShaders`（`builtinProgram(ShaderPreset)` + 两个延迟 program 工厂，`RenderPipelineBuilder` 改转发；**同日再改**：`builtinProgram(preset)` → `forwardProgram()` / `flatForwardProgram()`，枚举删除）；后端
  `compiledStages(preset)` 编译 SDK program（每 preset 缓存）。SDK 拥有**着色文本**，后端拥有编译+ABI+管线。
  · 口径：两条证据基线 47 行不变；`vine_shader_check` PASS（7）；test_graphics 235、test_vsg 238；lavapipe PASS。
- **P0.B1 SDK 显式属性 location 表（2026-09-13，行为中性）**：新增 `sdk/vine/graphics/ShaderAbi.hpp`
  （`VertexAttribute{Position,Normal,Color,TexCoord0}` + `attributeLocation()`，值 0/1/2/8）；vsg 两个 set 装配
  用它替代字面量。契约/分期（L1/L2/L3 + DX 映射 + B1..B4）见 `.ai/design/graphics-shader.md` §11。
  · 口径：两条证据基线 47 行不变；test_graphics 235→**236**；lavapipe PASS。下一步 B2：`ShaderProgram` 参数表 + 命名槽声明。
- **P0.B3 口径 + C1 落地（2026-09-13）**：L1 块命名定案 **`Vine<Role>Block`**（`VineViewBlock`/`VineDrawBlock`/`VineMaterialBlock`/`VineLightsBlock`；
  `Block` 明示内存布局，`VineFrame`→`VineViewBlock`）；B3 采用**选项 C**（L1 声明式块 + vsg push 作内部优化）。
  · `ShaderAbi.hpp` 落 `VineViewBlock`(288B)/`VineDrawBlock`(80B) + `static_assert`；`ShaderAbiTest` 钉 sizeof/offsetof。
  · 口径：行为中性（纯新增）；test_graphics 236→**239**。下一步 C2（vsg 标注 push ≡ 子集）。
  · **C2 落地（2026-09-13）**：vsg push `pc` 注释/头文档写明 `pc.projection ≡ VineViewBlock.proj`、`pc.modelView ≡ VineViewBlock.view * VineDrawBlock.model`；`ForwardShaderSetTest` +1 钉 shader 文本 + `sizeof(VineViewBlock) > 128`。行为中性；test_vsg 238→**239**。
- **P0.S1 + P10（2026-09-13）**：①GLSL 块名对齐 L1（`VineMaterialBlock`/`VineLightsBlock`，`ShaderAbiTest` 钉契约名==块名，test_graphics 240）；
  ②**每 drawable 不透明度改走 `vine_draw` 块**：`alpha = material.diffuse.a * draw.params.x`；每 drawable 一个 `floatArray`(20 float, DYNAMIC) 原地改 4 个 float（O(1)/帧，取代 O(V)/帧的顶点载体重写）；opacity **不再进 variant 身份**（删 `opacity_changes_state`）；forward 路径不再维护动态载体（`opacity_carrier=false`）⇒ 透明 drawable 也走"丢派生属性"精简变体。
  · **修真缺口**：P10 前半的顶点载体方案在 forward 上**从未到达帧缓冲**（新像素门禁抓到）；教训 = 结构断言（绑了哪些属性/变体文本）会全绿而画面不动，**像素差分才是判据**。
  · 口径：两条证据基线 **47 → 48 行**（只多这一行，其余逐字节不变）；test_vsg 240→**241**；`vine_shader_check` PASS（7）；lavapipe PASS。未做：dynamic-offset 打包（一 drawable 一 UBO）。

