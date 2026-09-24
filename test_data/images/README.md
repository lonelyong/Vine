# test_data/images

测试用图像素材。**`tests/test_imageio` 会真实解码全部 12 张**（`robots/` 下的 `.vdev` 目前没有引用方）。

## 内容

两套**各自独立**的 6 面 cube map，每张均为 `2048x2048`、8-bit baseline JPEG、RGB
（**无 alpha**，3 组件）。

| 一套 | 文件 | 命名风格 |
| --- | --- | --- |
| 北欧夏日风景（红房子 / 白桦林 / 多云） | `posx, negx, posy, negy, posz, negz` | 名字与立方体面语义**一一对应**（+X, -X, +Y, -Y, +Z, -Z，即 Vulkan/D3D 的面顺序） |
| 月夜海面与群山 | `front, back, left, right, top, bottom` | 引擎风格命名，**需要显式映射表**才能对到面下标 |

合计 12 个文件、约 6.4 MB。

## 用途注意

- **两套命名都不能直接按目录序用。** `posx..negz` 虽然名字对应面无歧义，但**按字母序排列**是
  `negx, negy, negz, posx, posy, posz` —— 与面顺序（+X, -X, +Y, -Y, +Z, -Z）**不同**。
  `front..bottom` 那套更需要一张显式映射表。两者都必须**显式写映射**，别依赖排序。
- **解码后的内存不小**：一套 cube map 按 RGB8 解码 = 6 × 2048² × 3 ≈ **75.5 MB**；
  转 RGBA8 则 ≈ **100.7 MB**。测试里别无脑整套解码，优先只解 1~2 个面，或改用缩略尺寸。
- **RGB8 不是 GPU 纹理格式**：JPEG 解出来天然是 3 字节 RGB，而主流 GPU 没有 24-bit 纹理格式
  （见 `imaging::PixelFormat::Rgb8Unorm`）。**直接用 `loadImage(path, PixelFormat::Rgba8Unorm)` 让解码器
  一步产出 RGBA8**，不必先解成 RGB8 再自己转。
- **来源**：从 `XGraph` 仓库 `src/xg/igl/core/res/images` 拷贝，作为后期测试素材。

## 接线现状（2026-09-12 更新）

- ✅ **解码已可用**：`loaders/imageio`（`vn::ImageIO`，stb 支持）能解 JPEG / PNG / BMP / TGA。
  `tests/test_imageio` 已经在解这套素材（**12 张全部解过**）。
- ✅ `graphics::Texture` 能**描述** cube map（`Shape::Cube`，6 个 face），并能把 `imaging::Image` 作为源图。
- ❌ 后端**仍不消费** `Texture`：face / mip 链不上传、不建 sampler、不进描述符集。
- ❌ **没有 mip 生成**：解码出来只有 1 个 mip，所以这些 2048² 图目前只能填 `mip_count == 1` 的 `Texture`。
- ❌ **2 通道被拒**：文件里的 2 通道是"灰度 + alpha"，而 `Rg8Unorm` 是"红 + 绿"，`imageio` 拒绝这个不匹配（宁可不给，也不把亮度塞进你叫"红"的通道）。

所以：**文件 → `imaging::Image` 已经通了；`Image` → 屏幕还没通。**
