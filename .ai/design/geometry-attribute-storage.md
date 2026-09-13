# geometry 属性存储：一份分配，两侧共用

> 相关：`.ai/memory/loaders.md`（`meshio` 拆分）、`src/plugins/gfx_backend_vsg/docs/data-flow.md`（前端 → 后端的字段映射）。

## 问题

`Mesh` 用 `std::vector<T>` 存 positions / normals / texcoords，而 `Geometry` 又用
`std::shared_ptr<std::vector<float>>` 存一份 **packing 过的副本**（`packVec3` / `packVec2`）。
同一个顶点在内存里存在几份。

关键在于：对 `Vec3f` / `Vec2f` 这类元素，packing 产出的字节与源数据**逐字节相同**
（3 个 `float` 挨着，正是 `packVec3` 的输出布局）。所以那份副本不是"转换"，是纯开销 ——
把它消除不需要任何转换代码，只需要让两侧**指向同一块分配**。

## 关键一步：元素类型钉死为 float / uint32

**这里中途改过方向，记下来是因为教训比结论重要。**

- 第一版让 `Buffer<T>` 保持泛型、`Mesh` 存 `Buffer<Vec3f>` / `Buffer<Vec2f>`。于是 `Geometry`
  侧要“持住某种 buffer”就必须**类型擦除** —— 而 `Buffer<T>` 是模板、`RefCounted` 是 CRTP
  **没有公共基类**，`intrusive_ptr` 擦除不了（core 里唯一的非模板多态基类是 `Object`，
  而 `Buffer` 刻意不是 `Object`）。当时的绕法：`shared_ptr<const void>` + 空 deleter 关住一个
  `intrusive_ptr`，再配一份**快照**（裸指针 + 长度）。
- 那个快照留下了真实的**悬空尖角**：源 buffer 之后增长（`push_back` 重新分配）会让通道指向已释放
  内存。之前 `Geometry` 自己 repack 时并没有这个风险 —— 为省一半内存引入 use-after-free，
  这笔账不该默认接受。
- **正解是把元素类型钉死**：属性就是 float（位置/法线 3 个、UV 2 个），索引就是 uint32。
  不用泛型之后，通道**直接存 `intrusive_ptr<const Buffer<float>>`**，每次访问现取 ——
  悬空尖角、长度陈旧、类型擦除、deleter 把戏**一起消失**，而且没有新增 core 类型、没有 vptr。

`Mesh` 因此也存 `Buffer<float>` / `Buffer<uint32_t>`。`Vec3f` 那份类型化 API 保留，由同一批字节
reinterpret 得到：`Vector3` 是 `{T x, y, z}` 与 `T data[3]` 的 union，布局由 `Mesh.cpp` 的
`static_assert`（`sizeof` 与 `alignof`）钉住 —— 这条假设是整套方案的地基。

## 结果

| | 组件 |
| --- | --- |
| 存储 | `core::Buffer<T>`：`RefCounted`，**内部组合** `std::vector<T>`（派生会被 `vector&` 传递切片掉引用计数） |
| 通道 | `AttributeChannel{ intrusive_ptr<const Buffer<float>> values; uint32_t components; size_t offset; size_t scalarCount; }`（`offset`/`scalarCount` 按**标量**计，`0` 长度 = 到缓冲末尾 ⇒ 可做 arena 切片） |
| 建模侧 | `Mesh` 存 `Buffer<float>`；`positions()` 等返回 `span<const Vec3f>`（同一批字节 reinterpret） |
| 桥 | `geometryFromShape()` 传 `mesh->positionsBuffer()` —— 两侧读**同一块分配** |
| 公告 | **一律手动**：`Buffer` 自己不 bump（它看不见每种写入，也不知道一次编辑何时结束）⇒ 写的人改完显式 `setRevision(revision()+1)`；`Mesh` 的 builder 在 `addVertex`/`addTriangle`/`clear` 里各公告一次（`Mesh::announceChange()`） |
| 打包 | `packAttribute(span<const Vec3f\|Vec2f>)` —— 给“有类型化顶点、没有 buffer”的调用方 |

**属性 setter 每个通道只留一个**：`setPositions` / `setNormals` / `setTexcoords2`（2 分量）/ `setTexcoords3`（3 分量）各收一个
`intrusive_ptr<const Buffer<float>>`，不重载。原先并存的 `setPositionsBuffer` 是同一件事的第二个名字
（还有为花括号调用保留的数组重载），都已删除；借用形式的入口改为 `packAttribute(...)` 工厂。
于是“这次是复制还是共享”由**传什么**决定，而不是由**调哪个名字**决定。

## 契约（借用与共享）

- `span` 是**借用**：不得比 mesh 活得久；任何**增长**都会使既有 view 失效。
- 共享句柄指向的是 mesh 的**活存储，不是快照**：后面对 mesh 的编辑在句柄侧可见。
  这正是共享要承担的义务 —— **写者必须让编辑被公告**，而且公告**一律是手动的**（`Buffer` 与
  `Geometry` 同一条规矩：被共享者自己不推断“内容变了”）：
  · `Buffer` 侧：`push_back`/`append`/`clear` 都不动 `revision_`，写的人改完自己 `setRevision(revision()+1)`；
    `Mesh` 是那个写的人，所以它的每个 `addVertex`/`addTriangle`/`clear` 都经 `announceChange()` 公告一次。
    理由：buffer 看不见 `data()`/`operator[]` 这类写入，也不知道一次编辑何时结束 —— 自 bump 只能是半真话
    （一条写路径自己公告、下一条静默），而消费者分不出这两者。
  · `Geometry` 侧：重建闸门是 `Geometry::revision()`，它**只由 `Geometry::setRevision()` 推进**
    （`++revision_` 已从所有 setter 删掉）。理由：geometry 借的就是那些字节，它自己分不出“还没读过的新字节”
    与“上次读过的旧字节”（它并不复制）。
  允许的写路径只有**模型自己的 API**（`addVertex` / `clear` / setter；绕过它原地改 buffer 要自己补公告）。
  忘记公告不会报错：读者静默沿用旧字节（首次构建不受影响 —— 那是从零建，不看 revision）。
- 因为通道**持的是 buffer 而不是快照**，即使源 buffer 增长，通道也不会悬空：长度与地址都现取。
- **通道可以只是缓冲的一段（arena）**：`AttributeChannel::slice(values, components, first_vertex, vertex_count)`
  用顶点说话，`shared(..., offset, scalar_count)` 用标量说话；`floatCount()/vertexCount()/scalars()/xyz()/vec3View()`
  全部只认**这一段**（越过末尾就是空，不会读到邻居）。索引侧用 `Geometry::setIndices(buffer, first_index, index_count)`
  表达切片，但**绑定的是整段缓冲**（切片在 draw 命令里：`firstIndex/indexCount`）⇒ 一个索引 arena 共享一次索引上传。
  段的身份进两层缓存 key：共享绑定缓存用 `缓冲地址 + revision + offset + 长度`，派生法线缓存另加索引的 `first/count`。

## 分期

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 1 | `core::Buffer<T>` + 单测（9 条） | ✅ `1adcd3b` / `72ee9cc` |
| 2 | `Mesh` / `IndexedTriangleMesh` 改存 `Buffer`；访问器改 `span` | ✅ `eb9b5ea` |
| 3a | `AttributeChannel` 改为「视图 + keepalive」（当时是类型擦除版），行为不变 | ✅ `55204a1` |
| 3b | `geometryFromShape()` 走共享 ⇒ **内存不再翻倍** | ✅ `6e9acff` |
| 3c | 元素钉死 float/uint32、`AttributeChannel` 直接持 buffer（删掉擦除与快照）、setter 合一名 | ✅ 本批 |
| 3d | 索引同样改成 `intrusive_ptr<const Buffer<uint32_t>>` + `packIndices()`；`Geometry::indices()` 改返回 `std::span` | ✅ 本批 |
| 4 | 后端从 `bytes()` 上传；`SceneBridgeGeometry` 不再逐顶点拷进 vsg typed array | ⬜ |

## 教训（第一版被推翻的过程）

**“优雅的类型擦除”不如“把类型钉死”。**
第一版的 keepalive（`shared_ptr<const void>` + 空 deleter，再配一份裸指针 + 长度的快照）能编译、
能过测试、能过证据比对 —— 因为它在**正确的输入**上没有可观测差异。它唯一的问题是：它把一个真实的
use-after-free 引进了原本安全的路径，而换来的只是“省一半内存”。根因是我把“通道的元素类型是开放的”
当成了前提；它不是。

**判据里最有信息量的一条是“证据逐字节相同”。** 阶段 2（换存储）、3b（改走共享）、3c（钉死类型 +
合并 API）之后，self-test 证据都是 **47 行逐字节相同**：渲染输出一个字节没变，读的却已经是另一套
存储。它同时也说明这些改动**不能**靠证据验证正确性 —— 所以每个阶段都另配了共享/同一性断言。

## 变异验证

| 变异 | 结果 |
| --- | --- |
| `IndexedTriangleMesh::addVertex()` 改成每次重建存储（“复制而非共享”） | 恰好 3 条共享断言失败：`size()` 2≠3、两侧 `data()` 地址不同、`revision()` 0 vs 0 |
| `geometryFromShape()` 改回 repack | 恰好 3 条指针同一性断言失败（positions/normals/texcoords），分量/坐标/计数断言全过 |
| 给 `AttributeChannel` 加回快照（裸指针 + 缓存长度） | 恰好 3 条增长断言失败：`floatCount()` 6≠9、`vertexCount()` 2≠3、`scalars().data()` 地址不同 |
| `AttributeChannel::shared()` 的 keepalive 换成空 lambda | 恰好 `useCount()` 断言失败（1≠2） |
| `geometryFromShape()` 的索引改回 `packIndices(indexed->indices())`（复制而非共享） | 恰好 3 条索引断言失败：两侧地址不同、增长后计数 3≠6、增长后仍不同 |
| `Geometry::setRevision()` 改成空操作 | 恰好 2 条红：`ManuallyReportedRevisionRebuildsTheDataNode`（顶点数据没被刷新，变换节点与状态包装都在）与设备无关的 `GeometryTest.RevisionCanBeReportedByHand`（41 读成 1）—— 证明“公告”确实是唯一能触发重建的东西 |

每条变异都只打中对应的那组断言、其余全过 —— 即那些断言卡的是它们声称的不变量，而不是碰巧因为别的
原因一起失败。

## 阶段 2 的实测结论（预测 vs 实际）

事前预测：破坏点只应出现在**构造方**，且限定为"把返回值绑到 `const Vec3fArray&`"的地方
（`MeshLoader.cpp`、`Geometry.cpp`、`XmlIOBase` 调用参数）；只读消费者零改动。

实测：**方向对，集合不全，解法也不同**。

- 漏了两处同类（都是"把视图绑到数组"这一族，只是没列进清单）：
  `MeshExporter.cpp` 把 `&itm.positions()` 存成 `const Vec3fArray*`；
  `GraphicsTest` 两处 `Geometry::setPositions(mesh->positions())`。
- 解法不是原计划的"就地造临时数组"，而是**让消费者接受视图**：`Geometry` 增加
  `std::span<const T>` 重载（原有 `const Vec3fArray&` 重载保留并委托，既有的
  `setTexcoords({...})` 花括号调用因此继续可用）。少一次物化、少改两处测试。
- `XmlIOBase.cpp` 的**调用点一行没改**：把 `IoUtils.hpp` 的 `*ArrayToBytes` 形参从
  `const vector&` 放宽到 `span` 即可。
- 只读消费者（`size()` / `operator[]` / 范围 for / `front()`）**确实零改动** —— 与预测一致。

**变异验证**：把 `IndexedTriangleMesh::addVertex()` 改成"每次重建存储"（即"复制而非共享"
这一真实回归形态），恰好 **3 条**共享断言失败：`shared->size()` 2≠3（持有者仍看着旧存储）、
两侧 `data()` 地址不同（两份分配）、`revision()` 0 vs 0（增长没有在持有者的 buffer 上公告）。
其余断言全过 —— 证明这几条断言精确地卡住"共享"这个不变量，而不是碰巧因为别的原因失败。

## 阶段 4 的实测结论：裸 `Data` 绑不上，真 `vsg::Array` 别名才成立（2026-09-12）

目标：后端不再逐顶点拷进 `vsg::vec3Array`（`SceneBridgeGeometry` 的 `unpackXyz` + `makeTypedVertexData`），渲染侧直接读模型内存。

**第一版做法（错）**：自己写一个 `vsg::Data` 子类，报 `dataPointer()` / `properties`，直接当顶点数组绑。结果 **画出空白，且 validation 一条不报** —— 静默失败。
debug 打印证明 `format` / `stride` / `valueSize` / `valueCount` / `dataSize` 与同形状 `vec3Array` **完全一致**也无效；强行把 `properties.format` 改成 `VK_FORMAT_UNDEFINED` 同样无变化。
结论：vsg 的绑定路径是围绕它自己的 `Array` 类型建的，"只是把属性报对"的 `Data` 不会被当数组用。

**第二版（正解）**：用 `vsg::Array(ref_ptr<Data> storage, offset, stride, numElements, ...)` 的**别名构造** —— `assign()` 把 `storage` 存成 `ref_ptr`，并令 `_data = storage->dataPointer() + offset`。
所以**被绑定的对象必须是真 `vsg::vec3Array`**，它别名一个持有 Vine buffer 的 `Data`（`detail::VsgBufferView<float>`，只做存储那一半）。

**为什么这个版本更好**：元素类型仍然是**数组的**类型 ⇒ Vulkan format / stride 仍由 `vsg::vec3Array` 推断，**没有任何手写** —— 绕开了 `VsgSceneRules.hpp` 记的"格式不匹配会被 configurator 静默接受"的坑。
生命周期也自然成立：数组持 storage、storage 持 buffer ⇒ 模型可以先死，渲染侧恰好活到读它的那一刻为止。

**判据（都是实测）**：

- `vsg_selftest_evidence.sh` → PASS，**47 行逐字节相同** —— 别名生效且渲染输出零变化。
- 变异：把别名的 `offset` 从 `0` 改成 `sizeof(::vsg::vec3)`（跳一个顶点）→ 证据 **FAIL**（证明别名真被读，不是悄悄走了回退路径）。
- 新断言 `SceneBridgePipelineSharingTest.PositionBindingAliasesTheModelBuffer`：绑定数组的 `dataPointer()` 必须**等于**模型 buffer 的 `scalars().data()`，且必须是真 `vsg::vec3Array`（`test_vsg` 185 → **186**）。
- 变异：关掉别名分支改回拷贝 → 该断言失败。渲染关口做不到这一点：拷贝渲染得逐字节相同，**只有指针同一性能区分**。

**其余通道（法线 / texcoords / 4 分量颜色 / 自定义通道 / 索引）已按同一机制落地**：`detail::aliasArray<Array, Element>(buffer, count)` 是所有通道的**唯一**入口（`aliasTypedVertexData` 只是按分量数选数组类型的 switch）；`geometry->indicesBuffer()` 补上以让索引也能别名。

**推导量仍然自己分配真数组**：白色 opacity 载体（alpha 会被逐 drawable 就地改写，别名会写穿到模型）、零填充 texcoords、以及 `makeNormals` / `makeIndexedNormals` 的推导分支。`makeTypedVertexData`（拷贝版）**已删除** —— 自定义通道的布局由 `channelShape` 保证匹配，拷贝是纯开销。
vec4 位置（stride 4）**有意不进别名**：它要的是 R32G32B32 绑定，别名会改成 R32G32B32A32，属行为变化；仍走 `unpackXyz`。

**踩到的坑（值得记）**：stride 必须是**数组自己的元素大小**，不是 buffer 元素的大小。vsg 用 `properties.stride` **同时**索引 CPU 侧与 GPU 绑定 —— 给一个别名 float buffer 的 `vec3Array` 传 `sizeof(float)`（4）会让 GPU 按 4 字节跨步交错读，而 CPU 侧的值看着还挺像。**两个关口同时抓住了它**：新单测（`(*arr)[1].x` 读到 2 而不是 4）与渲染证据（47 行不再相同）。现在单测直接钉住 `properties.stride == sizeof(::vsg::vec3)`。

**变异验证（本轮最有价值的一条）**：把 `aliasArray` 改成“复制同样的字节到自己的一份 buffer”（渲染输出逐字节相同、只是地址不同）⇒ **恰好 5 条地址/生命周期断言失败**（2 条共享断言 + 3 条单测），而 `vsg_selftest_evidence.sh` **仍然 PASS**。也就是说：**渲染关口对“拷贝 vs 共享”完全无感**，只有地址断言能区分 —— 这正是这条不变量必须有专门断言的理由。

## 注意

- `Mesh` 的 buffer **永不为空指针**（构造时分配），所以访问器不必判空；代价是每个 mesh 3~4 次
  小额分配（顶点数据本身远大于此）。
- 通道的 `size()` 是**标量数**，不是顶点数：顶点数用 `vertexCount()` 或 `positions().size()`。
- `Vec3f` ↔ float 的 reinterpret 现在主要在 `Mesh` 内部（存取器），由 `static_assert` + 单测钉住。
- 新增 `src/` 文件后必须 `cmake -S . -B build`（`v_add_library` 的 glob 无 `CONFIGURE_DEPENDS`）。

## 流（stream）的统一表示（2026-09-13）

一条**流** = 「哪个 buffer + 从哪开始 + 覆盖多少」，这个形状由核心的 `BufferSlice<Element>`
（`src/base/core/sdk/vine/Buffer.hpp`）定义，三条规则只写一次：偏移越界**钳到末尾**、count == 0 表示
**"到 buffer 末尾为止"**、长度按**当前** buffer 长度解析（buffer 会长大 —— arena 的尾段就靠这条）。

- **属性通道**：`AttributeChannel`（原 `AttributeBuffer`，改名以对齐代码与文档里一直用的 "channel" 说法）
  = **一个 `BufferSlice<float>`（以 scalar 计）+ 顶点 stride（components）**。`scalarSlice()` 把它的段
  交出去，`fromSlice(slice, components)` 从段 + stride 建回来。
- **索引流**：`Geometry::IndexStream = BufferSlice<std::uint32_t>`，元素计、无 stride；`setIndices(buffer,
  first, count)` 就是建这个段。
- **自定义流**（未来）：同一个 `BufferSlice<T>`，不需要第三种表示。

**谁陈述切片**：**四个角色、两种拼写**，都只有一条实现路径。整块 = `count` 0 ⇒ **跟随增长**；段 = 固定 count（`count == 0` 是"到末尾"，**不是空**；要空就传 null）。
- 顶点侧：`setPositions/setNormals/setTexcoords2/setTexcoords3(buffer)` 与 `...(buffer, first_vertex, vertex_count)`，实现是 `addBuffer(location, AttributeChannel::slice(...))`；
- 索引侧：`setIndices(buffer)` 与 `setIndices(buffer, first_index, index_count)`，实现是 `IndexStream::slice(...)`；1 参重载就是"整段"，`Geometry.cpp` 里只是一行转调，所以两种拼写不会漂。

**索引流不是 channel**：channel = 着色器**输入**（per-vertex、有 stride、按 location 绑定），索引流 = **拓扑**（顶点装配按它取属性，着色器看不到，元素是 `uint32`、无 stride）。两者共享的只是**表示**（`BufferSlice`）。`Geometry::IndexStream` 就是 `BufferSlice<std::uint32_t>` 的别名；`indices()/firstIndex()/indexCount()/indicesBuffer()` 是它同一个段的四个视图（没有第二份状态）。单位要分清：顶点侧是**顶点**（texcoord 段 3 顶点 = 6 scalar），索引侧是**索引元素**。

`addBuffer(location, channel)` 仍是**非 canonical**（别的 location、vec4 位置、别的 stride）的门，也仍然是"段"的实现路径：三个新重载都是**一行委托**给它。守卫：`TheWholeBufferAndTheSegmentSpellingsAgree`（同一 buffer/offset/components、覆盖相同、整块侧跟随增长而段不跟随、以及每个角色各自的 stride —— 复制粘贴重载用错 stride 会在 texcoord 那条上红）。

**为什么是组合而不是基类**：通道不是段，通道是"段 + 顶点 stride"的解释。若让 `AttributeChannel` 继承
`BufferSlice<float>`，把通道按值传给一个要段的接口会**静默丢掉 stride**（也就丢掉"每个顶点从哪开始"），
而这类错误不会在任何地方报出来。段是通道**拥有并能交出去**的东西（见两个成员函数），不是它"是"的东西。
