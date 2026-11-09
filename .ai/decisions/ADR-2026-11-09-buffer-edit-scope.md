# ADR-2026-11-09：`Buffer` 的写入口收成一个作用域

- 状态：已采纳
- 日期：2026-11-09
- 相关：`../design/graphics-geometry.md`（F18 / F25）、`../../src/base/core/sdk/vine/Buffer.hpp`、`../../docs/guides/graphics.md`

## 背景

`core::Buffer<T>` 是引擎里唯一"共享可变字节"的类型：模型侧要按 `T` 读，渲染侧要按字节上传。整条 GPU 侧的缓存链
（`StreamKey` 的 `revision`、后端事实表的 `described_revision`、`Scene` 的内容 epoch）**都只认它的 `revision()`**。
而 `revision()` 原来只有一条手工路径：`setRevision()` / `bumpRevision()`，写者**自己记得**在写完的时候调一次。

`Buffer.hpp` 的长注释把这个选择论证过，结论是"**任何**写都不自动公告，所以规则是绝对的"：

> 它看不见每一次写（非 const 的 `data()`、元素引用、另一个线程的写），也不知道一次编辑从哪开始到哪结束，
> 所以自我 bump 只能是半真话：一条写路径公告，下一条悄悄不公告。

这个论证是对的——**针对"每次写各自 bump"**。但它漏了第三种选项：**一次编辑一个作用域**。对象缺的那个事实正是
"编辑到哪儿结束"，把指针/引用/span 都从**有结束的作用域**里发出去，这个事实就有了。

代价是可数的：`setRevision(` 18 处、`bumpRevision(` 13 处是分离的第二步；`docs/guides/graphics.md` 原来直接
教这个两步舞（"`buffer->setRevision(buffer->revision() + 1)` + `geometry->setRevision(...)`"）；漏了公告不报错，
只表现为"画面停在上一次上传的字节"，与本节开头修过的那一类 bug 同源，只是发生在生产端。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 现状：四个静默写入口 + 手工公告 | 零改动；写代码最省事 | 正确性靠记性；漏公告不可检测；三份文档各讲一遍同一件事 |
| B. 每个写自动 bump（`data()`/`operator[]`/`view()` 保留） | 简单写法的确会自动了 | 指针一旦发出就再也看不见——**正是原文论证的漏洞**：一半写公告，一半不公告，与 A 同样不可推理 |
| C. **作用域唯一写入口**：`auto edit = buffer->edit();`，析构时公告一次 | 规则绝对且可检查（没有静默路径）；一次编辑一次公告，正是消费者要的粒度；文档不再教两步舞 | 写点从 `b->x()` 变成作用域内 `edit.x()`；需要一次机械迁移 |

## 决定

选 **C**。`Buffer::Edit` 是唯一能改元素的句柄（`data()` / `view()` / `operator[]` / `push_back` / `append` /
`clear` / `reserve`），`Buffer` 只留 **const** 的 `data()` / `view()` / `operator[]` / `bytes()` / `size()` /
`revision()`。作用域结束时 `revision` 前进一次；同时活的第二个写者会 `assert`（两次"编辑结束"会让消费者听不清）。
`setRevision()` / `bumpRevision()` 保留，语义收窄为**对象之外的写**（映射文件、设备写、自带版本号的生产者）。

## 后果

- 好：`G10`——正确性不再押在调用方记性上；`G11`——不存在"写了但没公告"的路径，所以这一类故障不会再出现；
  `G4`——`data()` 不再既说读又允许写。消费者侧一行代码没变（它们只读）。
- 好：实测**没有一行生产代码需要改**——只有测试里 15 处写点要进作用域。这反过来说明生产代码本来就"按编辑块"
  在写（新建一个 `Buffer(std::vector)` 交给几何体），欠的只是 API 形状。
- 代价：一条批量构建要套一层 `{}`；`reserve()` 只在一个作用域里调用也会公告一次（保守，读者多读一次，无正确性
  影响）。
- 代价：`Buffer` 多 4 字节（`editing_`），对一个顶点级共享对象可忽略。
- 重开条件：出现"必须在不持有 `Buffer` 的情况下写它的字节"的合法场景（映射内存/外部 DLL 直接写）——那类场景
  现在走 `setRevision()`，已经是显式路径，不需要重开。

## 证据

- `BufferTest.AnEditAnnouncesItselfExactlyOnce`（一次编辑一次公告，作用域内不公告）、
  `BufferTest.ASecondWriterCannotOpenWhileOneIsLive`（第二个写者断言）、
  `BufferTest.SetRevisionStaysForBytesWrittenOutsideTheBuffer`（逃生口）。
- 迁移点全部在测试里：`ChannelSliceTest`、`GraphicsTest`（实例流 / BVH 失效 / 通道增长）、`AdaptersTest`
  （`BufferSliceSource` 的"交给了别人还改"用例）、`ContentPassTest`、`StreamRevisionTest`。
- `test_core` / `test_graphics` / `test_vsg` 全量。
