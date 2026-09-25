# 纹理上传：Vine 的 `Texture` → vsg 的 GPU 图像

本篇记录 **CPU 侧图像如何变成可被采样的 Vulkan 图像**，重点是**与 vsg 的接缝**。

SDK 侧的类型设计（`Texture` / `Texture2D` / `CubeMap` / `Kind`、`layerCount()`/`layer()`）见
`.ai/design/graphics-design.md` §3.3。链路的另一端（几何与材质如何物化成 vsg 数组）见
`docs/data-flow.md`。

> **为什么单独成篇。** 下面每一条约定都是 vsg 源码里的隐含行为，且**违反它们全部是静默的**：
> 没有 VUID、没有异常、返回值不可查，画面上只是"少了点什么"或"采到了别的东西"。不写下来就只能
> 再花一轮把它们重新推一遍。

> **2026-09-25 注**：本接缝事实对 vsg 1.1.16 仍然成立，本文保留为活文档；角色名已换成
> 重写后的单元（`MaterialImages` 等），旧 `VsgTextureCache` / `SceneBridgePipeline` 随老渲染器删除。

## 1. 分工

| 一侧 | 负责 |
|---|---|
| `graphics::Texture` 及其派生 | 逻辑描述（种类 / 尺寸 / 格式 / 级数）+ 每层一个 `imaging::Image`；**无 GPU 对象、无设备句柄** |
| `MaterialImages` | 按 `Texture*` 缓存，把描述物化成 `vsg::ImageInfo`（图像 + 视图 + 采样器） |
| `BlockDescriptors` / `ContentDraw` | 把该 `ImageInfo` 绑到程序声明的采样器上（`diffuseMap` / `skyMap`） |

`MaterialImages` 构建的全部是 **CPU 侧描述** —— `vsg::Image` / `ImageView` / `Sampler` 在编译前不碰设备 ——
所以它的决策可以**无设备单测**（`tests/test_vsg/MaterialImagesTest.cpp`）。

## 2. 四条不成文的 vsg 约定

### 2.1 层数经 **Data 的 `depth`** 传递，不是 `image->arrayLayers`

`TransferTask::transferImageData` 按 image view 类型决定层数：

```cpp
uint32_t faceWidth = width, faceHeight = height, faceDepth = depth;
uint32_t arrayLayers = 1;
switch (imageView->viewType) {
case (VK_IMAGE_VIEW_TYPE_CUBE):     arrayLayers = faceDepth; faceDepth = 1; break;
case (VK_IMAGE_VIEW_TYPE_2D_ARRAY): arrayLayers = faceDepth; faceDepth = 1; break;
...
}
```

`faceDepth` 来自 **Data 的 `depth`**。所以**多层纹理必须用 3D 数组**（`uintArray3D` 等）并以
`depth = 层数` 构造。用 `uintArray2D` 承载 6 层 ⇒ `arrayLayers = 1` ⇒ **只生成一个拷贝区域、只写第 0 层**，
其余层保留显存残值。**没有任何 VUID**：那一个区域完全合法，拷贝也确实成功了。

### 2.2 `MipmapLayout` 是「每级一条」，而且它本是为块压缩格式准备的

- vsg 的注释写明：*"only required when the data contains mipmaps that use block compressed formats"*。
  **分层不靠它表达**（靠 2.1 的 `depth`）。
- vsg **每级只推进一次** `mipmapItr++`，每级内其余层由它自己按 `faceSize` 步进。所以给成
  每（级, 层）一条，级别 1 会读到级别 0 的第二条 —— **extent 与偏移全错**。

### 2.3 `properties.stride` 会被 `assign()` 覆写，而 vsg 把它当 `valueSize`

```cpp
// Array2D / Array3D 的 assign
properties.stride = stride;   // 调用方先前设的值被覆盖
```

```cpp
// TransferTask
const auto   valueSize = properties.stride;                 // 这个值 = 行 stride
const size_t faceSize  = valueSize * level_width * level_height;
```

两处相叠 ⇒ 一层占用的字节 = **`row_stride × level_w × level_h`**，而不是该层 texel 的真实字节数。
因此 **层与层之间要按这个步长留空**，`MipmapLayout` 每级的基址按
`层数 × row_stride × level_w × level_h` 前进。

### 2.4 `ImageView::viewType` 取自 **Data**，不是 `ImageView`

```cpp
// ImageView 的构造
if (image->data && image->data->properties.imageViewType >= 0)
    viewType = static_cast<VkImageViewType>(image->data->properties.imageViewType);
else
    viewType = <由 image->imageType 推导>;
```

`imageViewType` 未设置时是 `-1`（哨兵）。**写 `imageView->viewType` 会被构造过程直接覆盖**，结果是
一个 6 层的 `2D_ARRAY` 视图 —— 不报错、不崩溃，只是 cube 采不到东西。cube 还需要
`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` 置在 `Image::flags` 上（默认 `0`）。

## 3. 两条自检（改动上传路径时先验这两个）

1. **`total == data->dataSize()`。** 按 2.3 的公式算出的打包总量，应当与 vsg 自己算出的暂存量一致
   （实测：二维 `2720 == 2720`、cube `16128 == 16128`）。两边对上说明 padding 公式与它的预期吻合；
   对不上就说明还有东西在猜。
2. **`L == 1` 时证据逐字节不变。** 通用路径在单层时**必须**退化为原来的连续打包（padding 项为 0）。
   这是"通用化没有偷改单层上传"的独立证据，不必等多层断言来告诉你。

## 4. 验收的三层，各自能看见什么

| 手段 | 管什么 | **看不见**什么 |
|---|---|---|
| 门禁的 VUID 计数（`scripts/vsg_rewrite_gate.sh`） | 请求是否**合法** | 层数错、次层未写入 —— 这些都合法 |
| 像素用例 / 门禁应用阶段 | 结果**对不对** | 缓存决策、性能 |
| `MaterialImagesTest` | 缓存**决策**（命中 / 重建 / 兜底 / 回收 / 淘汰） | 像素对错 |

六面断言（六面各一色，读回 6 个 band）是唯一能抓住 2.1 的手段：**层序错位会让两个 band 颜色互换**。

> **一条方法论。** 若对上传路径的改动让输出**逐字节不变**，那不是"改动无害"，而是"没碰到决定它的输入"
> —— 此时应立即去读分派代码（如 2.1 的 `switch`），而不是继续调够得着的参数。层数错这一类缺陷，
> 编译、VUID、二维证据**都**看不见。

## 5. 尚未做的

- **`Texture3D` / `2DArray` 无法表达**：`imaging::Image` 是严格二维的（无 depth/slice）。基类的
  `layer()` / `layerCount()` 已为此备好，`imaging` 出现维度变体后，加形状是**纯增量**。
- 各向异性过滤**已接入**：`detail::anisotropyFor()` 把设备上限夹到 `[1, 16]`，`makeSampler` 按级数决定
  是否开启。注意 `samplerAnisotropy` 必须在创建设备时**显式申请**（设备特性由 `DeviceFeatures` 显式申请）—— 漏掉它不是"退化为各向同性"，而是
  `VUID-VkSamplerCreateInfo-anisotropyEnable-01070` 报错。
