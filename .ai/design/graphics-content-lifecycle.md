# graphics 内容生命周期：释放与缓存

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：内容对象的宿主所有权、GPU 驻留释放与缓存策略。
> **权威来源**：`RenderDevice.hpp` / `RenderEngine.hpp` / `MaterialImages.hpp` /
> `core/MaterialArena.hpp` / `core/Streams.hpp` / `core/BlockStorage.hpp`。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

本文件定义 graphics 内容生命周期：释放与缓存 的现行设计：先给对象与边界，再给功能、接口、约束与验收。

### 1.2 目标

- 把 §2 的设计要点落成 §3 的功能点、§4 的接口与 §8 的验收。
- 让“现在是什么”与“为什么这么定”分开：理由在 §2/§7，历史看 git。

### 1.3 非目标

- 不在本文件重复头文件里的完整签名；接口只给契约与指针。
- 不在本文件保存已关闭的历史或逐轮审查记录。

### 1.4 一句话结论

> 内容对象归宿主，GPU 驻留归后端；释放走显式入口；缓存不设容量上限，用多少拿多少。

## 2. 设计要点

2.1 **两层寿命**：`Geometry` / `Material` / `ShaderProgram` / `Texture` 是宿主对象；上传、块、描述符、
    编译变体是后端驻留。
2.2 **释放 GPU 驻留，不释放对象**：`releaseContent()` 之后对象仍可用，下一次使用重建驻留。
2.3 **显式管理**：没有 LRU、没有字节上限、没有压力淘汰；释放由宿主说。
2.4 **不 idle 设备**：释放走 token + park；只有破坏路径用计数 device idle。
2.5 **流按 buffer 身份共享**：`StreamKey` = buffer + revision + 段；稳态零重传。

## 3. 功能点

### 3.1 显式释放

| # | 功能 | 说明 |
|---|---|---|
| F1 | `releaseContent(Object)` | 释放 Geometry / Material / ShaderProgram / Texture 的 GPU 驻留。 |
| F2 | release token | 释放后、回收前宿主再次使用 ⇒ 取消释放，不误删新驻留。 |
| F3 | 纹理立即删 | 纹理 cache 立即删条目；行 / 编译产物 park。 |
| F4 | 重建 | 下一次使用重建驻留；不是泄漏。 |
| F5 | 弃用扫尾 | 宿主丢掉引用后由 `ContentSweep` 回收。 |

### 3.2 缓存

| # | 功能 | 说明 |
|---|---|---|
| F6 | 纹理缓存 | `MaterialImages` 条目跟宿主持有量走；无 LRU / 上限。 |
| F7 | 材质 arena | 每槽 `copies` 份（对应在飞帧）；槽满 `Refused`，不淘汰活材质。 |
| F8 | 按需扩容 | 记录前按 `ContentStore::materialCount()` 换更大 storage；无硬上限。 |
| F9 | 流共享 | `StreamUploads` 按 `StreamKey` 共享；稳态帧 0 上传。 |
| F10 | 每帧块 | `BlockStorage` 每帧 ring，slab 数 = 在飞帧 + 1；稳态帧不写。 |
| F11 | 对齐 | 块偏移必须是设备 `minUniformBufferOffsetAlignment` 的倍数。 |
| F12 | 预算 | oversized / overflow / material refusal 在消耗预算前拒绝并计数。 |

### 3.3 观测

| # | 功能 | 说明 |
|---|---|---|
| F13 | 计数器 | `writes` / `bytesWritten` / `materialWrites` / `uploads` / `aliases` / `refusals`。 |
| F14 | 诊断 episode | 被拒绝的块 / 材质各报一次；缓存压力不再发 `ResourcePressure`。 |
| F15 | 释放后可观测 | 释放 → 下一帧重建；用例断言驻留消失再出现。 |

## 4. 公开接口

### 4.1 释放

| 接口 | 作用 |
|---|---|
| `RenderDevice::releaseContent(raw_ptr<const Object>)` | 释放 GPU 驻留 |
| `RenderEngine::releaseContent(...)` | 引擎转发 |

### 4.2 缓存与存储

| 接口 | 作用 |
|---|---|
| `MaterialImages::acquire()` / `releaseTexture()` / `releaseContent()` / `releaseAbandoned()` / `bytes()` | 纹理 cache |
| `core::MaterialArena::write()` / `capacityNeed()` / `live()` | 材质块 |
| `BlockStorage::beginFrame()` / `writeView()` / `writeDraw()` / `writeMaterial()` / `releaseMaterial()` | 每帧块 |
| `BlockStorage::strides()` / `regions()` / `bytes()` | 布局与观测 |
| `StreamUploads::acquireVertex()` / `acquireIndex()` / `release()` / `agreesWithRegistry()` | 流上传 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 内容对象归宿主，GPU 驻留归后端；释放走显式入口。 |
| C2 | 缓存不设容量上限；宿主用多少拿多少。 |
| C3 | 材质槽满时拒绝，不淘汰活材质。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | `releaseContent()` 释放 GPU 驻留；不替宿主决定寿命。 |
| R2 | 不 idle 设备；释放走 token + park。 |
| R3 | 不引入全局预算/trim/收缩。 |

### 5.3 不变量（N#）

| # | 注意项 |
|---|---|
| N1 | 不要为了“省内存”重新引入 LRU / 字节上限；方向已裁决为显式释放。 |
| N2 | 槽立即复用安全：每槽 `copies` 份，新材质写当前 copy。 |
| N3 | 拒绝不淘汰活材质：同帧复用已记录的槽可能画错。 |
| N4 | 释放路径不 idle 设备；破坏路径才用计数 idle。 |
| N5 | 块存储增长只加 chunk，不搬旧块（旧 span 必须保持有效）。 |
| N6 | 对齐用设备限制，不要硬写 16 / 256。 |

## 6. 注意事项与陷阱

### 6.1 一事实一家

- 会怎么错：同一份状态被两处保存，改一处另一处过期。
- 怎么判：出现两份可写的值/两个可写的注册表。
- 怎么防：回到 §5 的 N#；只留一个 owner，其余只读。

### 6.2 后端/平台的边界

- 会怎么错：把平台差异写进引擎/应用层，换平台时到处分叉。
- 怎么判：出现“如果 Windows/X11 就……”的分支。
- 怎么防：平台事实留在后端，引擎只消费编译好的帧值。

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | LRU / 容量上限 / 压力淘汰 | 300 张同序循环下 LRU 与 FIFO 一样差；上限是替宿主做决定。详见 [ADR](../decisions/ADR-2026-10-22-cache-unbounded-explicit-release.md)。 |
| D2 | 材质 FIFO 淘汰 | 淘汰活材质会画错；拒绝只兜底。 |
| D3 | 释放时 idle 设备 | 破坏路径才需要；释放走 token + park。 |
| D4 | 全局预算 / trim / 收缩 | 没有消费者；宿主显式管理。 |
| D5 | 旧帧读回接缝 | F−N 是观测缺口，不是生产竞态；不做。 |

## 8. 测试与验收

| # | 判据 |
|---|---|
| T1 | `ContentStoreTest.*Release*` / `MaterialImagesTest.Release*`。 |
| T2 | `VsgBackendTest.ReleasingContentDropsItsResidencyAndTheNextFrameRebuildsIt`。 |
| T3 | `HeadlessBackendTest.TheEngineForwardsAContentReleaseToItsDevice`。 |
| T4 | `BlockStorageTest.*` / `MaterialArenaTest.*` / `StreamUploadsTest.*`。 |

## 9. 状态与演进

### 9.1 当前状态

现行设计；功能点与接口以上表为准。

### 9.2 已知缺口 / 待办

见 `../backlog/graphics.md`；开放项按触发条件保留。

### 9.3 兼容性 / 迁移

旧名/旧路径只在 §7 已否决选项或 git 历史里出现；新代码按 §4 的接口写。

## 10. 参考

`graphics-architecture.md` · `graphics-geometry.md` · `vsg-backend.md` · `../backlog/graphics.md`
