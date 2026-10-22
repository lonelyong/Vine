# A3 内容释放设计（2026-10-22）：一个 `releaseContent()`，释放的是 GPU 驻留，不是对象

> **状态：方案，未实现。** 触发条件仍是“宿主报告内存压力，且需要在同帧内/数帧内让后端放手”。
> 现行行为（宿主丢掉引用，帧级扫尾回收）不改；本文件定的是触发后加的那一个入口，以及它必须遵守的
> 语义。实现切片见 §7，判据见 §6。

## 0. 结论

- **一个入口**：`RenderDevice::releaseContent(raw_ptr<const Object> content)`，`RenderEngine` 转发同名方法。
  参数类型是 `Object`，不是四个 `releaseGeometry/Material/Program/Texture`；`Object` 已经在公开面
  （`RenderEngine::removePassesOwnedBy(const Object*)`），后端用一个显式类型分派识别四种内容。
- **释放的是驻留，不是对象**：四种内容对象仍由宿主持有、仍然有效；下一次画到它时重新上传/编译/建行。
  `releaseContent()` 不修改 `revision()`，也不从场景里移除任何东西。
- **不 idle 设备**：内容行的移除走已有的 `RetirementQueue`；纹理缓存条目立即丢，但已记录的描述符集
  自己持有引用。帧路径“不停设备”的判据不变。
- **可取消**：`ContentStore` 的 live entry 带一个 release token；宿主下一次真正使用它（`track` /
  `ensure` / `updateMaterial`）就把请求取消，已排队的移除回调看到 token 不匹配就什么也不做。
  所以“释放后马上又画”不会把新行误删，也不会出现 identity 相同的两行。
- **停车不可用时拒绝**：会话还没学到 in-flight 数时，`RetirementQueue::retire()` 会拒绝猜；此时
  `releaseContent()` 返回 false，状态不变。宁可这次不放，也不在仍被记录帧命名时提前释放。
- **不做的**：全局 `trimContent()` / 内容预算 / LRU 淘汰。那是 A6 的淘汰策略问题，触发条件不同；
  本设计只解决“宿主点名的对象，后端把手上的副本放掉”。

## 1. 现状与约束

| 事实 | 位置 | 对设计的影响 |
| --- | --- | --- |
| 内容对象是引用计数，宿主放手即 `useCount` 变化 | `Geometry` / `Material` / `ShaderProgram` / `Texture` | 现有 `releaseAbandoned()` 只在“后端是最后一个持有者”时回收；宿主仍持有时没有入口 |
| 后端为对象保留 share，按对象地址做键 | `ContentStore` 的 `LiveGeometry` / `LiveProgram` / `LiveMaterial` | 释放必须显式丢掉这个 share，不能等 `useCount` |
| 纹理缓存条目持有 `owner` share | `MaterialImages::CacheEntry` | 释放纹理可以立即从 cache 删；描述符集仍持有图像 |
| 行被计划命名时不能提前删 | `ContentStore` 的 `ContentFacts` + `RetirementQueue` | 行的移除必须 park，不能立即 erase |
| 会话可能还没学到 in-flight 数 | `SlotProbe` / `RetirementQueue::parkingAvailable()` | 无法安全 park 时必须拒绝，而不是猜 |
| 材质块槽是 arena，只经驱逐离场 | `BlockStorage` 的 material arena | 释放材质行不会立即归还那 64 字节槽；这是已知残量，不是新泄漏 |
| 流上传按“帧命名 + grace”回收 | `StreamUploads::releaseUnseen()` | 几何的 stream 不随 `releaseContent` 立即消失；下一帧扫尾接手 |
| 编译半片按 table 可答性扫尾 | `ContentHalves::halvesFor()` 的 sweep | 程序/几何行消失后，下一趟收集把半片 park；释放本身不强制一次收集 |

A3 的触发面已经量化（`vsg-reimplementation.md` §11.16cz）：12 轮创建 + 丢弃后表行/纹理/流都回基线，唯一残量是
材料 arena 的 12/12 槽。也就是说“丢掉引用”这条路是通的；本设计补的是“宿主不丢引用、但要让 GPU 副本先走”的那条路。

## 2. 公开面

### 2.1 `RenderDevice`

放在 `RenderDevice.hpp` 的 `releasePass` / `releaseRenderTarget` 旁边，语义也按同一族写：对象本身仍是宿主的逻辑
描述，设备只放自己保留的 GPU 状态。

```cpp
/**
 * @brief Releases the device's GPU residency of a content object the host still owns.
 *
 * @details
 * - Recognized content: Geometry, Material, ShaderProgram, Texture. Any other Object is not content and is
 *   refused (false), not silently ignored.
 * - The object stays valid and keeps its revision. The next frame that names it uploads / compiles / builds
 *   its rows again.
 * - The release is SCHEDULED, not synchronous: rows and compiled objects are parked through the session's
 *   RetirementQueue so a command buffer that already names them is past before they leave. Textures leave
 *   the image cache immediately; retained descriptor sets keep their own shares.
 * - Call between frames, on the frame thread. Like releasePass / releaseRenderTarget, this is a resource
 *   announcement, not a frame call.
 *
 * @param content Content object to evict; null is refused.
 * @return true when @p content is recognized and the release was accepted (scheduled, or nothing was
 *         resident); false when it is not content, the device is not up, or the release cannot be parked
 *         safely (the session has not learned its in-flight count).
 */
virtual bool releaseContent(raw_ptr<const Object> content)
{
    (void)content;
    return false;
}
```

默认实现返回 false，和 `releasePass` / `releaseRenderTarget` 的默认 no-op 一样：不保留内容的设备（以及现有
测试替身）不需要为这个入口实现任何东西；只有真的把内容留在 GPU 上的后端才覆盖它。

为什么参数是 `Object` 而不是四个重载：

- A3 的登记原话是“先加**一个**入口，而不是四个”；四个重载在调用面仍是四个名字参与重载解析的函数。
- `Object` 是四种内容共同的基类，`RenderEngine::removePassesOwnedBy(const Object*)` 已经有同样的公开先例。
- 后端必须显式分派（`obj_cast<const Geometry>` 等），不认识的 `Object` 返回 false；不会把
  `releaseContent(renderTarget)` 悄悄当成成功。
- 如果将来出现“异构容器里存一批内容”的消费者，再引入 `ContentRef { Object*, Kind }`；现在没有消费者，
  不先造身份层。

### 2.2 `RenderEngine`

引擎转发，宿主不必自己拿 `device()`：

```cpp
bool RenderEngine::releaseContent(raw_ptr<const Object> content);
```

实现只做两件事：`device() == nullptr` 时返回 false；否则转给 `RenderBackend::releaseContent()`。
不缓存、不计数、不改变 pass / scene / frame 的任何状态。

## 3. 后端语义

### 3.1 `VsgBackend::releaseContent()`

```
if (content == nullptr || !initialized) return false;
if (auto* geometry = obj_cast<const Geometry>(content))
    return d->store->releaseGeometry(geometry, timeline, retirement);
if (auto* material = obj_cast<const Material>(content))
    return d->store->releaseMaterial(material, timeline, retirement);
if (auto* program = obj_cast<const ShaderProgram>(content))
    return d->store->releaseProgram(program, timeline, retirement);
if (auto* texture = obj_cast<const Texture>(content))
    return d->images->releaseTexture(texture);
return false;
```

`obj_cast` 的顺序按 `Geometry` → `Material` → `ShaderProgram` → `Texture`；四者没有继承重叠，
`Texture2D` / `CubeMap` 会匹配到 `Texture`。

### 3.2 四类内容各自发生什么

| 内容 | 释放时立即做的 | GPU 对象何时离开 | 再次使用 |
| --- | --- | --- | --- |
| `Geometry` | 在 live entry 上打 release token，park 一个“删掉该 identity 全部几何行”的回调 | 行在 in-flight 窗口之后删；stream 由下一帧 `releaseUnseen` 按 grace 收 | 下一帧 `track` 取消请求并重建行；stream 重新上传 |
| `Material` | 同上；材质行按 identity 删 | 行在窗口之后删；material arena 槽仍由 arena 驱逐 | `track` / `updateMaterial` 取消请求并重建材质行 |
| `ShaderProgram` | 同上；park 删除该 program 的全部 revision/variant 行 | 行删后，下一趟 `halvesFor` 的 sweep 把半片 park | `track` 取消请求；下一次收集重新编译 |
| `Texture` | 立即从 `MaterialImages` 的 cache 删条目 | 已记录的描述符集持有自己的 share；它们退休后图像才真正销毁 | 下一次 acquire 重新上传；sampler 缓存不动 |

“park 一个回调”是**释放请求**，不是“立刻 erase”。这是与现有 `releaseAbandoned()` 最大的区别：后者处理的是
宿主已经放手、不可能复用的对象；本设计处理的是宿主仍持有、可能复用的对象，所以必须有取消。

### 3.3 释放 token：为什么不能直接 erase

朴素实现（release 时立即从 `d->geometries` 删 live entry、park `eraseGeometryOf(identity)`）有一个竞态：

1. 释放，删 live entry，park 删除回调；
2. 宿主又画这个几何，`track` 新建 live entry，`ensure` 建新行；
3. 回调到期，`eraseGeometryOf(identity)` 把**新行**也删了。

材质更糟：材质按 identity 查表，新旧两行会同时存在，可能读到旧行。token 是把这个竞态变成可判定的：

```
LiveGeometry {
    intrusive_ptr<Geometry> object;
    bool described;
    uint64_t described_revision;
    uint64_t release_token{0};     // 每次 release / cancel 都 ++
    bool release_requested{false};
};
```

- `releaseGeometry()`：若 live entry 不存在，返回 true（本来就没有驻留）；若已 requested，返回 true；
  否则 `++release_token`、`release_requested = true`，park 一个捕获 `(identity, token)` 的回调。
- 回调到期：lock store；找 live entry；只有 `release_requested && release_token == token` 才删行并删 entry；
  否则什么都不做（宿主已经复用，请求已被取消）。
- `track()` / `ensure*()` / `updateMaterial()`：只要对象被真正使用，就 `release_requested = false`、
  `++release_token`。旧回调因此失效；新释放会拿新 token。
- `retire()` 返回 false（停车不可用）：把 `release_requested` / token 回滚，返回 false；不删任何东西。

这样：

- 释放后马上又画：`track` 取消请求，行保持，画面不受影响；GPU 内存没有白放，但这是宿主自己的选择。
- 释放后不再画：窗口到期，行删，entry 删，内存归还。
- 释放后先画一次又停：第一次 `track` 取消；之后再 `releaseContent` 会排一个新的回调。
- 连续两次 `release`：第二次看到 `release_requested`，直接返回 true；不会排两个回调。

### 3.4 纹理

`MaterialImages` 没有“live entry”，它的 cache 条目本身就是后端保留的 share，所以纹理释放不需要 token：

- `releaseTexture(texture)`：在 cache 里找到该指针的条目就 erase；没找到返回 true（本来就没驻留）。
- 已记录的 descriptor set / `SamplerImage` 持有 `ref_ptr`，cache 条目消失不会让在飞帧读到已销毁的 image。
- 下一次使用重新 `acquire()`，重新上传并建 sampler；sampler 缓存（按 `Sampler` 值）保留。
- 纹理 release 不 park，因为 cache 条目不是“被计划命名”的对象；真正被计划命名的是描述符集，它自己有退休窗口。

## 4. 不做什么

| 方案 | 为什么不做 |
| --- | --- |
| `releaseGeometry()` / `releaseMaterial()` / `releaseProgram()` / `releaseTexture()` 四个方法 | A3 登记明确要一个入口；四个方法把同一个生命周期问题拆成四份文档和四个调用点 |
| 新增 `ContentObject` 基类 | 四种内容已经共享 `Object`；为了一个 API 改继承树，收益只是少一次 `obj_cast` |
| `ContentHandle` / `ContentRef` 身份值 | 没有异构容器的消费者；多一层身份就要多一层与对象地址同步的状态 |
| 立即 `erase` 行 / 立即销毁 GPU 对象 | 已记录的帧可能还命名它们；正确做法是 `RetirementQueue`，不是 `deviceWaitIdle` |
| 释放时 `deviceWaitIdle` | 违反“帧路径不停设备”的不变量；释放是资源公告，不是帧步骤 |
| 全局 `trimContent()` / 内容预算 / LRU | 这是 A6 的淘汰策略，需要“最近使用帧号 / 字节上界”；A3 只解决宿主点名的对象 |
| 释放即 `revision++` | 释放不改变内容数据；下一次使用按 live revision 重建即可，不能让宿主看见一次假编辑 |

## 5. 与现有扫尾的关系

- `sweepAbandonedContent()` 继续负责“宿主已经放手”的路径；它按 `useCount` 判断，不动。
- `releaseContent()` 负责“宿主仍持有但点名要放”的路径；它按 token + park 判断，不动 `useCount` 语义。
- 两条路最后都落到同一组 `eraseGeometryOf` / `eraseProgramOf` / `eraseMaterialOf` 回调；不需要第二套删除机制。
- 材质 arena 的 FIFO 淘汰（A6）不变：释放只删行，槽仍由 arena 的正常替换回收；A6 的容量/键策略是另一件事。

## 6. 判据

### 6.1 设备无关（`ContentStoreTest`，新增）

1. `ReleaseMarksAndCancels`：释放后 `track` 同一对象，推进 timeline 到停车点，行仍在；请求被取消。
2. `ReleaseWithoutReuseDropsTheRows`：释放后不 `track`，推进到停车点，行与 live entry 都消失。
3. `ReleaseTwiceIsOneRequest`：第二次释放不排第二个回调，计数不变。
4. `ReleaseWithoutAParkingWindowIsRefused`：`RetirementQueue(0)` 下返回 false，行与 live entry 不变。
5. `ReleaseThenEditThenTrackKeepsTheNewRevision`：释放后改 revision 再 track，旧行到期只删旧 revision，新行在。

### 6.2 纹理（`MaterialImagesTest`）

6. `ReleaseDropsTheCacheEntryAndAcquireUploadsAgain`：释放后 cache 计数下降；再 acquire 产生一次上传/新条目。
7. `ReleaseOfANeverCachedTextureIsAccepted`：返回 true，计数不变。

### 6.3 设备级（`VsgBackendTest`）

8. `ReleasingContentKeepsTheObjectUsable`：画带纹理的几何，读像素；`releaseContent`；再画；像素相同，
   `builds()` / `uploads()` 上升，`deviceWaits()` 不变。
9. `ReleasingContentWhileAFrameMayNameItParksTheRows`：记录一帧后释放，下一帧仍能画（或按文档报告一次 miss），
   不出现 stale facts / VUID。
10. `ReleasingANonContentObjectIsRefused`：传一个非内容 `Object`，返回 false，无副作用。
11. `ReleasingAnObjectTheDeviceNeverSawIsAccepted`：返回 true，无副作用。

### 6.4 门禁

- `test_graphics` / `test_vsg` / ctest 全绿；验证层 0 VUID；六道静态门禁 0。
- 关键变异：去掉 token 检查 ⇒ 6.1.1 / 6.3.9 红；release 时直接 `erase` 行 ⇒ 6.3.9 红；
  纹理 release 不删 cache ⇒ 6.2.6 红。

## 7. 落地切片

| 片 | 内容 | 判据 |
| --- | --- | --- |
| A3-1 | `RenderDevice::releaseContent` + `RenderEngine::releaseContent`（默认 false / 转发），Doxygen 与指南 | 编译、API 文档门禁 |
| A3-2 | `ContentStore` 的 live release token + `releaseGeometry/Material/Program`；`MaterialImages::releaseTexture` | 6.1 + 6.2（设备无关） |
| A3-3 | `VsgBackend::releaseContent` 分派与设备级用例 | 6.3；`deviceWaits()` 不变 |
| A3-4 | 指南与台账：A3 从“无入口”改为“入口已落地，触发=宿主实际使用” | 全量套件 + 六道门禁 |

## 8. 未决 / 已知边界

- **不从场景移除**：`releaseContent` 只动后端驻留；场景仍持有对象，下一帧会重新上传。宿主若想彻底释放，
  应同时从场景移除并丢引用。
- **不强制一次收集**：程序的编译半片在下一趟 `halvesFor` 的 sweep 里离开；宿主停止收集时，会话销毁是上界。
  若将来需要“释放后立刻扫半片”，再加一个显式的 `ContentAssembly` 扫尾入口，不在本设计里混。
- **材质 arena 槽**：释放材质行不会立即归还 arena 槽；这是 A3 量化里已知的 12/12 残量，属于 arena 的
  驱逐策略（A6），不是内容释放的漏洞。
- **纹理的临时双份**：纹理释放后立即又被画到，会重新上传一张；旧图可能还被在飞描述符集持有，直到它们退休，
  所以这一小段窗口里显存会短暂出现两份。释放是给“冷内容”用的；热纹理一释放就复用，省不下内存还会多一次上传。
- **返回值粒度**：先只有 bool（识别并接受 / 拒绝）。如果宿主需要“释放了多少字节”做预算，再在 A6 的
  预算接口里回答，不把统计塞进这个入口。
