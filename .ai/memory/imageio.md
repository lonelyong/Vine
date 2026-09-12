# imageio 模块笔记

> 定位依据见 `.ai/design/imaging-design.md`（§3「与 graphics 的分工铁律」那套推理同样适用于这里）。
> 这里只记**结论 + 判据**。

## 事实速查

- 目录 `src/loaders/imageio/`，短名 **`ImageIO`** → 别名 `vi::ImageIO`，宏 `V_IMAGEIO_API` / `V_IMAGEIO_LIB`，
  命名空间 `vine::imageio`，头 `<vine/imageio/ImageCodec.hpp>`。
- 依赖：**PUBLIC `vi::Imaging`** + **PRIVATE `tp::stb`**（静态烘进 DLL，消费者看不到 stb）。
- 注册点：`src/loaders/CMakeLists.txt`、`tests/CMakeLists.txt`、`third_party/CMakeLists.txt`
  （新增 `third_party/stb/CMakeLists.txt`，INTERFACE target `stb` / 别名 **`tp::stb`**）。
- **改完新文件必须 `cmake -S . -B build`**（`v_add_library` 的 glob 没有 `CONFIGURE_DEPENDS`）。

## 为什么在 loaders 而不是 imaging

- **仓库已经回答过同一个问题一次**：`base/geometry` 装数据（`Mesh`/`Shape`），`loaders/meshio` 装用 assimp
  把它们读出来。图像同构：`base/imaging` 是数据，`loaders/imageio` 是**某一种**填充它的通道。命名对齐 `meshio`。
- **"base 层不能碰三方库"是假命题**（`base/crypto` 就 `PRIVATE` 链了 wolfssl），但真正的分界更锋利：
  **crypto 依赖 wolfssl 是因为"哈希就是这个库"；而 `Image` 不是解码器** —— 同一份数据要服务于
  probe / 读回 / 程序生成 / 文件四种来源，stb 只是其中一条通道。
- 代价具体：`imaging` 现在是只有 Core+Global 的叶子、编译秒级；塞进 stb 后**每个**链 `vi::Imaging` 的目标
  （将来的 `meshio`、headless 的 `urdf2vine`）都背上它。

## stb 引入方式（2026-09-12）

- **vendor** 进 `third_party/stb/`：`stb_image.h`（v2.30，sha256 `594c2fe3…`）、
  `stb_image_write.h`（v1.16，sha256 `cbd5f0ad…`）。不 FetchContent：上游无 CMakeLists、无 release，
  FetchContent 会拉整个仓库只为两个文件，还得手工建 target。
- **编译配置集中在 `src/StbConfig.hpp`**，被两个 TU 包含（`StbImageImpl.cpp` 带 `*_IMPLEMENTATION`，
  `ImageCodec.cpp` 不带）。两处配置必须一致，所以只写一份。
- `STBI_ONLY_{PNG,JPEG,BMP,TGA}` 裁剪 + `STBI_NO_STDIO`/`STBI_WRITE_NO_STDIO`：
  编解码走 **memory API**，文件 I/O 由我们自己用 `std::filesystem` + fstream 做
  （避开 Windows 窄字符路径，也让编解码可纯内存测试）。
- **stb 在项目的严格 flag 下零警告** —— 不需要 `-w` 抑制，也就没加。

## 通道语义（**坑，用 stb 前必须知道**）

stb 的通道数含义**不是** R/G/B/A 的顺序展开：

| 通道 | stb 的含义 |
| --- | --- |
| 1 | **Rec.601 亮度**（`(r*77+g*150+b*29)>>8`），不是红通道 |
| 2 | **灰度 + alpha**（`convert_format` 里 `(3,2){dest[0]=compute_y(...); dest[1]=255;}`） |
| 3 | RGB |
| 4 | RGBA |

→ 所以 `imageio` **拒绝 2 通道**：文件里的 2 通道是"灰度+alpha"，而 `Rg8Unorm` 是"红+绿"，
回答它就是撒谎。宁可不给。

**另一个坑**：JPEG 解码器在 `stb_image.h:3991/4001` 用**钳位前的**中间 RGB 算 1 通道亮度，
而 `stbi__convert_format` 用钳位后的字节 —— 所以"从 RGB8 结果反算亮度"和"1 通道解码结果"可能差一两步。
**测亮度语义要在无损容器（PNG）上测**，别在 JPEG 上测。

## API（`sdk/vine/imageio/ImageCodec.hpp`）

```cpp
enum class ImageFileFormat { Unknown, Png, Jpeg, Bmp, Tga };
const char*     formatName(ImageFileFormat) noexcept;
ImageFileFormat formatFromPath(const std::filesystem::path&);   // 按扩展名，仅作提示
bool canRead(ImageFileFormat) noexcept;    // 4 个都行
bool canWrite(ImageFileFormat) noexcept;   // PNG/BMP/TGA；JPEG 不行（有损且需质量参数）

intrusive_ptr<imaging::Image> decodeImage(std::span<const std::byte>, imaging::PixelFormat);
intrusive_ptr<imaging::Image> loadImage(const std::filesystem::path&, imaging::PixelFormat);
std::vector<std::byte>        encodeImage(const imaging::Image&, ImageFileFormat);
void                          saveImage(const std::filesystem::path&, const imaging::Image&, ImageFileFormat);
```

- **容器由字节嗅探，不由扩展名**（解码从不看扩展名 —— 文件名撒谎也解得对）。
- **调用方指定要哪种 `PixelFormat`**（1/3/4 通道 + sRGB/BGRA 变体），这解决了"解码天然 RGB8 但 GPU 没有
  24-bit 纹理格式"。BGRA 用解码后交换 R/B 实现（stb 只产 RGBA）。
- 解码结果**恒为 1 个 mip**（容器存一张图；链是另一件事）。
- 编码**只写 base mip**（PNG/BMP/TGA 装不下链）。
- 失败：`std::invalid_argument`（布局/容器不支持、空缓冲）+ `std::runtime_error`（解不了、文件读不了）。

## BMP 的能力边界（**坑**）

stb 的 BMP writer 只有两条路（`stb_image_write.h:494`）：`comp != 4` → 24-bit RGB，`comp == 4` → 32-bit V4。
`comp == 1/2` 走 `expand_mono` 把灰度复制成 RGB。
所以 **BMP 能存 1 / 3 / 4 通道，不能存 2 通道**（2 通道时 alpha 被静默丢掉）。
实测：`BMP/Rg8Unorm` 往返失败（第二个通道回来恒为 `0xFF`）。PNG 和 TGA 支持 1/2/3/4。
注意 `stbiw__write_pixels` 是按 `comp` 正确跨步的（`data + (j*x+i)*comp`），所以**没有越界读**。

## 2026-09-12 第一批：`loaders/imageio` 建立

**判据**：`test_imageio` **26 tests / 3 suites** 全过（新建）；全量 `ninja` **零 error / 零 warning**；
`test_imaging` / `test_graphics`（200）/ `test_vsg` / `test_meshio` / `test_brepio` 无回归；
`vsg_selftest_evidence.sh` → PASS（45 行逐字节相同）；`check_diagnostic_formats.py` → 0 suspicious。

**测试用到了真实素材**：`test_data/images/` 的 12 张 2048² JPEG 全部解过。
`tests/test_imageio/CMakeLists.txt` 用 `target_compile_definitions(... VINE_TEST_DATA_DIR=...)`
指向**二进制旁边的副本**（`<build>/bin/test_data`）——**这是仓库里第一个读磁盘素材的测试，也就顺手把路径约定定下来了**。
约定后来（2026-09-13）改为：素材目录自带搬运规则（`test_data/CMakeLists.txt`，目标 `stage_test_data`：构建期
`<build>/bin/test_data`、安装期 `<prefix>/test_data` 与 `bin/`/`lib/` 同级），**不再把源码树路径编进去** ——
Windows 上不安装也能直接跑，且不再依赖绝对路径。
