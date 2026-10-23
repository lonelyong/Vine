# graphics 内容生命周期：释放与缓存

> 单一来源：内容对象与 GPU 驻留的寿命、缓存策略。头文件 `RenderDevice.hpp` / `MaterialImages.hpp` 等
> 是 API 细节的权威。待办见 `graphics-followups.md`。

## 1. 两层寿命

- **内容对象**：`Geometry` / `Material` / `ShaderProgram` / `Texture` 都是 `RefCounted`，由宿主持有。
  宿主丢掉引用后，帧级扫尾（`ContentSweep`）回收后端为它保留的驻留。
- **GPU 驻留**：后端按内容身份建的上传、块、描述符、编译变体。它不是对象的字段，而是后端的一张表。

## 2. 显式释放

```cpp
bool RenderDevice::releaseContent(raw_ptr<const Object> content); // RenderEngine 转发
```

- 释放的是 **GPU 驻留**，不是对象本身；对象仍可继续被宿主使用。
- 支持四类：`Geometry` / `Material` / `ShaderProgram` / `Texture`（其它类型返回 false）。
- `ContentStore` 的 live entry 带 release token：释放后、真正回收前宿主再次使用 ⇒ 取消释放，不误删新驻留。
- 纹理 cache 立即删条目；行 / 编译产物走 park（按帧退役）；**不 idle 设备**。
- 下一次使用会重建驻留；这是显式内存管理的正常代价，不是泄漏。
- 破坏路径（引擎 shutdown / 区销毁）仍用计数过的 device idle；替换路径用 `RetirementQueue`，
  两条路分工不变。

## 3. 缓存策略

**方向：不设容量上限，有多少内存就用多少，释放显式管理。**

- `MaterialImages`（纹理上传缓存）：条目跟宿主持有量走；没有 LRU / 字节上限 / 淘汰。
  只有 `releaseAbandoned()`（宿主丢掉）与 `releaseTexture()` / `releaseContent(texture)`（显式）删条目。
  `bytes()` 报 mip 链暂存字节。
- `core::MaterialArena`（材质块）：每槽 `copies` 份（对应在飞帧）；没有 FIFO 淘汰。
  槽满返回 `WriteKind::Refused`，不淘汰活材质（同帧复用已记录的槽可能画错）。
  `capacityNeed() = live + 本帧不同被拒材质`；`VsgBackend::growMaterialArenaIfNeeded()` 在录制前按
  `ContentStore::materialCount()` 换更大 storage（无硬上限；失败保留旧 storage 并报）。
- `StreamUploads`：按 `StreamKey`（buffer + revision + 段）共享；稳态零重传。
- `BlockStorage`：每帧 ring（slab 数 = 在飞帧 + 1），稳态帧不写；扩大只加 chunk，不搬旧块。
  区域对齐用设备的 `minUniformBufferOffsetAlignment`；超预算的块在消耗预算前拒绝。

## 4. 诊断

- 缓存不再因“压力”发 `ResourcePressure` 警告；被拒绝的 oversized / overflow / material refusal 各有计数与
  一次 episode。
- 显式释放、重建、缓存命中都有计数器（`writes` / `bytesWritten` / `materialWrites` / `uploads` / `aliases` /
  `evictions`（已删）/ `refusals`）——它们是测试与门禁的判据。

## 5. 判据

`ContentStoreTest.*Release*` / `MaterialImagesTest.Release*` /
`VsgBackendTest.ReleasingContentDropsItsResidencyAndTheNextFrameRebuildsIt` /
`HeadlessBackendTest.TheEngineForwardsAContentReleaseToItsDevice` /
`BlockStorageTest.*` / `MaterialArenaTest.*` / `StreamUploadsTest.*`。
