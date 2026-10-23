# graphics 内容生命周期：释放与缓存

> **一句话**：内容对象归宿主，GPU 驻留归后端；释放走显式入口；缓存不设容量上限，用多少拿多少。
>
> **权威来源**：`RenderDevice.hpp` / `RenderEngine.hpp` / `MaterialImages.hpp` /
> `core/MaterialArena.hpp` / `core/Streams.hpp` / `core/BlockStorage.hpp`。
> **待办**：`../backlog/graphics.md`。

## 1. 设计要点

1.1 **两层寿命**：`Geometry` / `Material` / `ShaderProgram` / `Texture` 是宿主对象；上传、块、描述符、
    编译变体是后端驻留。
1.2 **释放 GPU 驻留，不释放对象**：`releaseContent()` 之后对象仍可用，下一次使用重建驻留。
1.3 **显式管理**：没有 LRU、没有字节上限、没有压力淘汰；释放由宿主说。
1.4 **不 idle 设备**：释放走 token + park；只有破坏路径用计数 device idle。
1.5 **流按 buffer 身份共享**：`StreamKey` = buffer + revision + 段；稳态零重传。

## 2. 功能点

### 2.1 显式释放

| # | 功能 | 说明 |
|---|---|---|
| F1 | `releaseContent(Object)` | 释放 Geometry / Material / ShaderProgram / Texture 的 GPU 驻留。 |
| F2 | release token | 释放后、回收前宿主再次使用 ⇒ 取消释放，不误删新驻留。 |
| F3 | 纹理立即删 | 纹理 cache 立即删条目；行 / 编译产物 park。 |
| F4 | 重建 | 下一次使用重建驻留；不是泄漏。 |
| F5 | 弃用扫尾 | 宿主丢掉引用后由 `ContentSweep` 回收。 |

### 2.2 缓存

| # | 功能 | 说明 |
|---|---|---|
| F6 | 纹理缓存 | `MaterialImages` 条目跟宿主持有量走；无 LRU / 上限。 |
| F7 | 材质 arena | 每槽 `copies` 份（对应在飞帧）；槽满 `Refused`，不淘汰活材质。 |
| F8 | 按需扩容 | 记录前按 `ContentStore::materialCount()` 换更大 storage；无硬上限。 |
| F9 | 流共享 | `StreamUploads` 按 `StreamKey` 共享；稳态帧 0 上传。 |
| F10 | 每帧块 | `BlockStorage` 每帧 ring，slab 数 = 在飞帧 + 1；稳态帧不写。 |
| F11 | 对齐 | 块偏移必须是设备 `minUniformBufferOffsetAlignment` 的倍数。 |
| F12 | 预算 | oversized / overflow / material refusal 在消耗预算前拒绝并计数。 |

### 2.3 观测

| # | 功能 | 说明 |
|---|---|---|
| F13 | 计数器 | `writes` / `bytesWritten` / `materialWrites` / `uploads` / `aliases` / `refusals`。 |
| F14 | 诊断 episode | 被拒绝的块 / 材质各报一次；缓存压力不再发 `ResourcePressure`。 |
| F15 | 释放后可观测 | 释放 → 下一帧重建；用例断言驻留消失再出现。 |

## 3. 公开接口

### 3.1 释放

| 接口 | 作用 |
|---|---|
| `RenderDevice::releaseContent(raw_ptr<const Object>)` | 释放 GPU 驻留 |
| `RenderEngine::releaseContent(...)` | 引擎转发 |

### 3.2 缓存与存储

| 接口 | 作用 |
|---|---|
| `MaterialImages::acquire()` / `releaseTexture()` / `releaseContent()` / `releaseAbandoned()` / `bytes()` | 纹理 cache |
| `core::MaterialArena::write()` / `capacityNeed()` / `live()` | 材质块 |
| `BlockStorage::beginFrame()` / `writeView()` / `writeDraw()` / `writeMaterial()` / `releaseMaterial()` | 每帧块 |
| `BlockStorage::strides()` / `regions()` / `bytes()` | 布局与观测 |
| `StreamUploads::acquireVertex()` / `acquireIndex()` / `release()` / `agreesWithRegistry()` | 流上传 |

## 4. 注意项

| # | 注意项 |
|---|---|
| N1 | 不要为了“省内存”重新引入 LRU / 字节上限；方向已裁决为显式释放。 |
| N2 | 槽立即复用安全：每槽 `copies` 份，新材质写当前 copy。 |
| N3 | 拒绝不淘汰活材质：同帧复用已记录的槽可能画错。 |
| N4 | 释放路径不 idle 设备；破坏路径才用计数 idle。 |
| N5 | 块存储增长只加 chunk，不搬旧块（旧 span 必须保持有效）。 |
| N6 | 对齐用设备限制，不要硬写 16 / 256。 |

## 5. 否决项

| # | 否决 | 理由 |
|---|---|---|
| D1 | LRU / 容量上限 / 压力淘汰 | 300 张同序循环下 LRU 与 FIFO 一样差；上限是替宿主做决定。详见 [ADR](../decisions/ADR-2026-10-22-cache-unbounded-explicit-release.md)。 |
| D2 | 材质 FIFO 淘汰 | 淘汰活材质会画错；拒绝只兜底。 |
| D3 | 释放时 idle 设备 | 破坏路径才需要；释放走 token + park。 |
| D4 | 全局预算 / trim / 收缩 | 没有消费者；宿主显式管理。 |
| D5 | 旧帧读回接缝 | F−N 是观测缺口，不是生产竞态；不做。 |

## 6. 判据

| # | 判据 |
|---|---|
| T1 | `ContentStoreTest.*Release*` / `MaterialImagesTest.Release*`。 |
| T2 | `VsgBackendTest.ReleasingContentDropsItsResidencyAndTheNextFrameRebuildsIt`。 |
| T3 | `HeadlessBackendTest.TheEngineForwardsAContentReleaseToItsDevice`。 |
| T4 | `BlockStorageTest.*` / `MaterialArenaTest.*` / `StreamUploadsTest.*`。 |

## 7. 相关文档

`graphics-architecture.md` · `graphics-geometry.md` · `vsg-backend.md` · `../backlog/graphics.md`
