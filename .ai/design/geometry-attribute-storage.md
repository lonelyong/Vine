# geometry 属性存储：一份分配，两侧共用

> 相关：`.ai/memory/loaders.md`（`meshio` 拆分）、`src/plugins/gfx_backend_vsg/vine-to-vsg-data-flow.md`（前端 → 后端的字段映射）。

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
| 通道 | `AttributeBuffer{ intrusive_ptr<const Buffer<float>> values; uint32_t components; }` |
| 建模侧 | `Mesh` 存 `Buffer<float>`；`positions()` 等返回 `span<const Vec3f>`（同一批字节 reinterpret） |
| 桥 | `geometryFromShape()` 传 `mesh->positionsBuffer()` —— 两侧读**同一块分配** |
| 公告 | 任何变更 `++revision_`；`data()` / `operator[]` 这类 buffer 看不见的写入用 `setRevision()` |
| 打包 | `packAttribute(span<const Vec3f\|Vec2f>)` —— 给“有类型化顶点、没有 buffer”的调用方 |

**属性 setter 每个通道只留一个**：`setPositions` / `setNormals` / `setTexcoords` 各收一个
`intrusive_ptr<const Buffer<float>>`，不重载。原先并存的 `setPositionsBuffer` 是同一件事的第二个名字
（还有为花括号调用保留的数组重载），都已删除；借用形式的入口改为 `packAttribute(...)` 工厂。
于是“这次是复制还是共享”由**传什么**决定，而不是由**调哪个名字**决定。

## 契约（借用与共享）

- `span` 是**借用**：不得比 mesh 活得久；任何**增长**都会使既有 view 失效。
- 共享句柄指向的是 mesh 的**活存储，不是快照**：后面对 mesh 的编辑在句柄侧可见。
  这正是共享要承担的义务 —— **写者必须让编辑被公告**（走 mesh 的 API 会自动 bump，绕过它就必须
  `setRevision()`），**读者必须比较 `revision()` 决定是否重读**。写者不公告，读者就静默沿用旧字节。
- 因为通道**持的是 buffer 而不是快照**，即使源 buffer 增长，通道也不会悬空：长度与地址都现取。

## 分期

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 1 | `core::Buffer<T>` + 单测（9 条） | ✅ `1adcd3b` / `72ee9cc` |
| 2 | `Mesh` / `IndexedTriangleMesh` 改存 `Buffer`；访问器改 `span` | ✅ `eb9b5ea` |
| 3a | `AttributeBuffer` 改为「视图 + keepalive」（当时是类型擦除版），行为不变 | ✅ `55204a1` |
| 3b | `geometryFromShape()` 走共享 ⇒ **内存不再翻倍** | ✅ `6e9acff` |
| 3c | 元素钉死 float/uint32、`AttributeBuffer` 直接持 buffer（删掉擦除与快照）、setter 合一名 | ✅ 本批 |
| 3d | 索引共享（`Geometry::indices()` 仍是 `const UInt32Array*`，带索引的 mesh 仍有一份副本） | ⬜ |
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
| 给 `AttributeBuffer` 加回快照（裸指针 + 缓存长度） | 恰好 3 条增长断言失败：`floatCount()` 6≠9、`vertexCount()` 2≠3、`scalars().data()` 地址不同 |
| `AttributeBuffer::shared()` 的 keepalive 换成空 lambda | 恰好 `useCount()` 断言失败（1≠2） |

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

## 注意

- `Mesh` 的 buffer **永不为空指针**（构造时分配），所以访问器不必判空；代价是每个 mesh 3~4 次
  小额分配（顶点数据本身远大于此）。
- 通道的 `size()` 是**标量数**，不是顶点数：顶点数用 `vertexCount()` 或 `positions().size()`。
- `Vec3f` ↔ float 的 reinterpret 现在主要在 `Mesh` 内部（存取器），由 `static_assert` + 单测钉住。
- 新增 `src/` 文件后必须 `cmake -S . -B build`（`v_add_library` 的 glob 无 `CONFIGURE_DEPENDS`）。
