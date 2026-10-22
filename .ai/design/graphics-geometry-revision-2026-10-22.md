# 几何 / buffer 的 revision（2026-10-22）：形状归几何，字节归 buffer，**没有兜底**

> B6 的定稿记录（含三次方案的取舍与实测）。一句话：**每个 stream 用自己 buffer 的 revision 决定要不要重传**；
> 几何的 revision 只描述"这个几何长什么样"（喂哪些通道、每条读哪段），**不**替任何 buffer 说话。

## 0. 规则

| 谁 | 说什么 | 谁跟 |
| --- | --- | --- |
| `Buffer::setRevision()` | "这块字节变了" | 上传缓存：键 = `(buffer, 该 buffer 的 revision, 切片, kind/location/components)` ⇒ 只有读它的 stream 换键、重传（共享 arena 的每个读者都跟着，因为字节是共享的） |
| `Geometry::setRevision()` / `bumpRevision()` | "这个几何的形状变了"（加/删/换通道、换切片、换索引范围） | 后端的事实行：按 `(geometry, geometry.revision())` 重建 layout / 通道集 / 计数。**它不参与上传键** |
| 谁都没喊 | **字节视为没变** | 渲染器从不比较字节，也没有兜底：没公告就是旧的 |

## 1. 为什么没有兜底

曾经把几何 revision 也放进上传键，作为"我没说是哪条"的兜底。它的代价是：**改一条通道 ⇒ 整个几何的所有流全部重传**
（B6 的形状：5 条流 / 16.98 MiB），而收益只是让一个"忘了公告"的宿主看起来正常。这等于让守规矩的宿主为不守规矩的宿主付费。
现在的分工是：**字节的事只能在字节那一层说**；形状的事在几何那一层说。两个公告各自可查、各自有判据。

## 2. 三条实现要点

1. **键在录制时拼**：`ChannelFacts` 不存 revision，`streamKey()` 在录制这一刻读 `values->revision()`。
   于是"buffer 改了但几何没动"立刻生效，且**事实行不需要因为字节而重建**。
2. **字节在上传时读**：`ChannelFacts` 只借 buffer 句柄 + 切片，`scalars()` 返回 live 视图；`StreamUploads`
   在 sharing miss 时才建 `vsg` 数组并拷贝。命中时既不解引用也不读内存。
3. **结构变化靠身份**：换 buffer（指针变）、换切片（offset/count 变）都是新键 ⇒ 新上传，**不需要任何 revision**；
   几何公告只负责让后端重建"描述"（layout/计数）。

## 3. 实测（`VsgBackendTest.MeasureWhatAMultiChannelMeshCostsWhenOneChannelChanges`，512×512、4 通道 + 索引，整份 16.98 MiB）

| 场景 | 每帧 uploads | 每帧 builds |
| --- | --- | --- |
| 稳态（没编辑） | 0 | 0 |
| 一条通道就地编辑 + `buffer->setRevision` | **1** | 0 |
| 一条通道换 buffer + `geometry->setRevision` | **1** | 1 |
| 四条通道就地编辑 | **4**（索引流不重传） | 0 |

旧契约下"任意一次公告"都是 **5 条流**（4 通道 + 索引）。也就是说：单通道编辑的带宽从 16.98 MiB 降到一条流。

## 4. 契约变更（宿主需要知道的唯一一件事）

`Geometry::setRevision()` **不再表示"字节变了"**。改字节要 `buffer->setRevision()`；换 buffer / 换切片 / 加删通道才用
`Geometry::setRevision()`。仓库自己的宿主只需要一次这样的对照：`FpsOverlay` 每次更新都装一个**新** buffer（身份变化，
天然是新 stream），它的 `bumpRevision()` 仍然正确（形状里那个通道换了来源）。

曾经的三次尝试（折叠哈希当 `contentRevision`、私有探测器 + 单调计数器、把几何 revision 当兜底）以及它们为什么复杂，
留在 git 与同日 memory 条目里，不再作为方案。

## 5. 判据

`tests/test_vsg/StreamRevisionTest.cpp` 五条（设备无关）：

1. buffer 公告只让读它的那条键变；
2. **几何公告不改变任何 stream 键**（钉住"没有兜底"），只改变描述版本；
3. 结构变化（换 buffer / 换切片）本身就是新键；
4. 字节来自 buffer 而不是事实表的快照（改了立刻可见）；
5. 共享上传表：稳态 0 上传、buffer 公告 1 上传 1 alias、几何公告 0 上传。

变异实测：`streamKey()` 不读 live revision ⇒ 第 1、5 条红；事实表存快照 ⇒ 第 4 条红（并会让第 1 条同红）。
设备级实测见 §3 的那条用例（数字就是判据）。
