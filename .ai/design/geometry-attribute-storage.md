# geometry 属性存储：一份分配，两侧共用

> 相关：`.ai/memory/loaders.md`（`meshio` 拆分）、`src/plugins/gfx_backend_vsg/vine-to-vsg-data-flow.md`（前端 → 后端的字段映射）。

## 问题

`Mesh` 用 `std::vector<T>` 存 positions / normals / texcoords，而 `Geometry` 又用
`std::shared_ptr<std::vector<float>>` 存一份 **packing 过的副本**（`packVec3` / `packVec2`）。
同一个顶点在内存里存在几份。

关键在于：对 `Vec3f` / `Vec2f` 这类元素，packing 产出的字节与源数据**逐字节相同**
（3 个 `float` 挨着，正是 `packVec3` 的输出布局）。所以那份副本不是"转换"，是纯开销 ——
把它消除不需要任何转换代码，只需要让两侧**指向同一块分配**。

## 方案

| 概念 | 做法 | 理由 |
| --- | --- | --- |
| 存储 | `core::Buffer<T>`（`RefCounted`，**内部组合** `std::vector<T>`） | 组合而非派生：`std::vector` 没有虚析构，派生对象经 vector 引用删除是 UB，且任何按 `vector&` 传递的地方都会把引用计数**切片**掉 |
| 类型化访问 | `view()` → `std::span<T>` | 建模 / 碰撞 / 拾取 / IO 要的就是 `Vec3f` 级访问 |
| 类型擦除访问 | `bytes()` → `std::span<const std::byte>` | 设备要的字节**就是**那些元素本身；消费者只需要再被告知布局 |
| 增长公告 | 任何变更 `++revision_` | 消费者比较 revision，区分"同一 buffer、内容已换"与"同一 buffer、仍然新鲜" |
| 手动公告 | `setRevision()` | `data()` / `operator[]` 交出的是**可写引用**，从那里写入 buffer 永远看不见 —— 必须有入口让调用者自己声明 |

`Mesh` 存 `intrusive_ptr<Buffer<T>>`：

- 读访问 `positions()` / `normals()` / `texcoords()` 返回 `std::span<const T>`；
- 共享句柄 `positionsBuffer()` 等返回 `intrusive_ptr<const Buffer<T>>`。

**为什么读访问器返回 `span` 而不是 `const vector&`**：`span` 把存储类型挡在接口之外，
且保留了调用方已有的 `size()` / `operator[]` / 范围 for 写法。
**为什么另给共享句柄**：只有持有引用计数才能把同一块分配交给第二个消费者；
`span` 表达不了这件事。两者分工明确：`span` 是借用，句柄是所有权。

## 契约（借用与共享）

- `span` 是**借用**：不得比 mesh 活得久；任何**增长**都会使既有 view / 字节指针失效。
- 共享句柄指向的是 mesh 的**活存储，不是快照**：后面对 mesh 的编辑在句柄侧可见。
  这正是共享要承担的义务 —— **写者必须让编辑被公告**（走 mesh 的 API 会自动 bump，
  绕过它就必须 `setRevision()`），**读者必须比较 `revision()` 决定是否重读**。
  写者不公告，读者就会静默沿用旧字节。

## 分期

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 1 | `core::Buffer<T>` + 单测（9 条） | ✅ `1adcd3b` / `72ee9cc` |
| 2 | `Mesh` / `IndexedTriangleMesh` 改存 `Buffer`；访问器改 `span`；`Geometry` 增加 `span` 重载（仍 repack） | ✅ `eb9b5ea` |
| 3a | `AttributeBuffer` 改为「视图 + 类型擦除 keepalive」，行为不变（证据逐字节相同） | ✅ `55204a1` |
| 3b | `Geometry::setPositionsBuffer()` 等 + `geometryFromShape()` 走共享 ⇒ **内存不再翻倍** | ✅ `6e9acff` |
| 3c | 索引共享（`Geometry::indices()` 仍是 `const UInt32Array*`，带索引的 mesh 仍有一份副本） | ⬜ |
| 4 | 后端从 `bytes()` 上传；`SceneBridgeGeometry` 读字节视图 + 布局，不再依赖 `shared_ptr<vector<float>>` | ⬜ |

## 阶段 3 的关键取舍

**keepalive 用 `shared_ptr<const void>` + 空 deleter 关住 `intrusive_ptr`。**
`Buffer<T>` 是 CRTP 引用计数，**没有公共基类**，所以 `intrusive_ptr` 无法类型擦除；而通道需要能
持住「某种 buffer」。用 `shared_ptr` 的 no-op deleter 在那个类型擦除句柄里关住一个 `intrusive_ptr`
即可 —— 若只存裸指针加空 deleter，最后一个 handle 一放就会把 buffer 释放掉。

**共享引入的尖角（已知且已记录）**：通道持有的是指向 buffer 元素的裸指针 + 当时的长度，
**不是快照**。若源 buffer 之后增长（`push_back` 导致重新分配），通道持有的指针会悬空。
之前 `Geometry` 自己 repack 时不存在这个风险 —— 所以这是一次真实的安全性权衡，换来的是内存减半。

- 缓解：契约写在 `AttributeBuffer::shared()` 与 `Geometry::setPositionsBuffer()` 的注释里
  （"还在构建中的 mesh 必须构建完再转换"）。
- 同上：这个尖角在**索引**上早已存在（`Geometry::setIndices(shared_ptr<UInt32Array>)` 一直是共享指针），
  所以它不是新引入的一类风险，而是把既有模式扩展到顶点属性。

**若将来真的需要在共享之后再编辑 mesh**，可选的加固方案（尚未做，因为现在没有消费者）：
给 `AttributeBuffer` 存一个「由 owner 重新导出 scalars」的函数指针
（`std::span<const float> (*)(const void*)`），这样地址与长度都每次实时导出，
既不怕增长导致的重新分配，也不需要虚函数。

## 阶段 3 的实测结论

- **3a 的正确性检查就是「证据逐字节不变」**：纯表示变更不应当改变任何一个渲染字节，
  self-test 47 行确实逐字节相同。
- 一次编译就暴露的 API 问题：`AttributeBuffer::shared()` 最初写成
  `template<typename T> shared(intrusive_ptr<const Buffer<T>>)`，而 `intrusive_ptr` 的
  **转换构造函数不参与模板推导**，所以传非 const 句柄推导失败。改为按 buffer 类型推导
  （`using Element = std::remove_const_t<BufferT>::value_type`），const / 非 const 都接受。
- Self-test 证据在 3b 之后**仍然逐字节相同**：读的是 mesh 自己的存储，而渲染输出一个字节没变。
- 变异验证（3b）：把 `geometryFromShape()` 改回 repack → 恰好 3 条指针同一性断言失败
  （positions / normals / texcoords），而分量数、坐标值、顶点计数断言全过 ——
  即该断言只卡「是否共享」，不靠别的性质连带失败。

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

- `Mesh` 的 buffer **永不为空指针**（构造时分配），所以访问器不必判空；
  代价是每个 mesh 3~4 次小额分配（顶点数据本身远大于此）。
- 阶段 2 结束时内存**尚未减少**：`Geometry` 仍 repack。减少发生在阶段 3。
- 新增 `src/` 文件后必须 `cmake -S . -B build`（`v_add_library` 的 glob 无 `CONFIGURE_DEPENDS`）。
