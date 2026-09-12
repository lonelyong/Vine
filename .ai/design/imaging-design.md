# imaging 模块设计（CPU 像素数据）

> 状态：设计稿 v1（2026-09-12）
>
> **定位**：`imaging` 是 base 层的一个**CPU 像素数据**模块。它定义"一张图在内存里长什么样"
> （`PixelFormat` = 每像素字节布局、`Image` = 一张 2D 图 + 可选 mip 链），
> **不知道纹理、采样器、设备或渲染器的存在**。
> 因此它可以被**加载器**（解码文件）、**渲染层**（读回结果）、**无渲染进程**（离屏工具）同时使用。

## 1. 为什么需要独立模块

在此之前，SDK 里**像素数据没有对象**：

| 事实 | 位置 |
| --- | --- |
| 材质贴图只是一个**路径字符串** | `graphics::Material::textureFile() -> String texture_file_` |
| 没有任何像素解码 | `loaders/` 里零 `stb_image`/`QImage` |
| 没有任何读回/截图路径 | 全库 `readPixels`/`capture` 零命中 |
| `ImageRef` 是**绑定句柄**（target + attachment 索引），不是数据 | `graphics/ImageRef.hpp` |

`ImageRef.hpp` 的注释自己承认了这个洞：

> *"What an `ImageRef` does NOT yet cover is an image that is no pass' output at all
> (a material's texture, an imported image, a cube map): those have no identity in this SDK
> (a material texture is a file PATH), so they cannot be wired yet."*

`imaging` 就是补这个洞的**数据侧**。

## 2. 架构定位

```
                ┌──────────────────────────┐
                │ imaging（叶子，CPU 像素） │
                │  PixelFormat / Image     │
                │  只依赖 Core + Global    │
                └────────────┬─────────────┘
                             │
        ┌────────────────────┼────────────────────┐
        │                    │                    │
┌───────▼────────┐  ┌────────▼────────┐  ┌────────▼────────┐
│ graphics       │  │ meshio         │  │ 离屏工具         │
│ Texture 由它建  │  │ 解码文件→Image  │  │ urdf2vine 等     │
│ 读回产出 Image  │  │                 │  │ （不链渲染器）    │
└────────────────┘  └─────────────────┘  └─────────────────┘
```

**关键：依赖方向只向上。** `imaging` 在 `geometry` 同级，谁都能依赖它，它不依赖任何人。
这正是它不能放进 `graphics` 的原因。

## 3. 与 graphics 的分工铁律

> **CPU 像素 / 图像数据 → `imaging`**
> **GPU 资源（format / mip / sampler / usage）→ `graphics`**

判定问句：

> **"这个东西在 headless、没有渲染器、没有 Qt 的环境里有意义吗？"**
> 有 → `imaging`；没有 → `graphics`。

### 3.1 为什么 `Image` 不放 `graphics`（被否决的方案）

把 `Image` 放进 `graphics` 是"今天最省、三个月后最痛"：

1. **`meshio → graphics` 的层级倒置。** assimp 的 `aiTexture::pcData` 天然产出**解码后的像素**。
   要让材质贴图从路径字符串变成真纹理，链路是 `meshio`（解码）→ `graphics`（上传）。
   `Image` 在 `graphics` 就等于让**加载器依赖渲染器**。
2. **无渲染工具用不了。** `urdf2vine` 现在是
   `PRIVATE RoboticsIO RoboticsCore Geometry IOBase MeshIO tinyxml2` —— **刻意不链 graphics/Qt**。
   将来机器人管线做缩略图/纹理烘焙时，`Image` 在 `graphics` 里它碰不到。
3. **层级逻辑倒置。** `graphics → PUBLIC vi::Geometry`：顶点数据 `Geometry` 在 `graphics` **下面**。
   像素数据是同一类东西（buffer + 格式），没道理放在消费者**上面**。

补充：`Image` 也**不能**是 `QImage` —— `graphics` 是 Qt-free 的，`QImage` 在 `Qt6::Gui`。
Qt 侧的转换桥留在 `appfw`（它本来就链 `Qt6::Gui`）。

### 3.2 为什么 `Texture` 留 `graphics`

纹理是 GPU 资源：消费者只有 `Material`（采样）、`RenderTarget`（作为附件）、`ImageRef`（身份）、
后端（创建）。`graphics` 以下的模块没法有意义地谈论 GPU 纹理。

**当前范围（拍定）：只做 2D 与 Cube。** 不做 1D / 3D / array，避免过度设计。

### 3.3 被否决的第三个方案：改名 `window`

曾考虑把 `window` 模块改名为 `display` 并让 `Image` 住进去。否决理由：

- 名字要能自圆其说，"显示相关叶子资源" 是硬凑；
- 改名是**牵一发动全身**的动作：短名 / 目录名 / `*_global.hpp` 宏名 / 别名 `vi::X` **四处必须同步**
  （`v_add_library` 用短名同时生成别名和 `V_<SHORT>_LIB` 宏），漏一处就是 Windows 上的 `dllimport` 自炸；
- 新建模块没有这个风险，且名字更诚实。

## 4. 模块结构

```
src/base/imaging/                      # 短名 Imaging → 别名 vi::Imaging，宏 V_IMAGING_API
  CMakeLists.txt                       # 只链 vi::Core vi::Global
  sdk/vine/imaging/
    imaging_global.hpp                 # API export 宏 + V_IMAGING_NS 命名空间
    PixelFormat.hpp                    # 每像素字节布局枚举 + 4 个查询函数
    Image.hpp                          # 一张 2D 图 + mip 链
  src/
    PixelFormat.cpp                    # 枚举 → 事实的**唯一**映射点
    Image.cpp                          # 布局计算与总覆盖式访问
```

## 5. 核心类型设计

### 5.1 `PixelFormat`

`enum class PixelFormat : std::uint8_t`，18 个真格式 + `Unknown`。

**设计要点：查询走自由函数，不走枚举分支。**

```cpp
int         channelCount(PixelFormat) noexcept;    // D24UnormS8Uint 报 2（深度+模板都算通道）
std::size_t bytesPerPixel(PixelFormat) noexcept;   // 打包格式报整个 texel 大小
bool        isDepthFormat(PixelFormat) noexcept;
bool        isSrgbFormat(PixelFormat) noexcept;
const char* formatName(PixelFormat) noexcept;      // 静态字面量，永不返回 null
```

理由：调用方真正要问的是"几个通道 / 每像素几字节 / 是不是深度 / 是不是 sRGB"，
而不是"它等于哪个枚举值"。**加一个格式 = 只教一处，不是教所有调用点。**

`Unknown` 是唯一"不是格式"的值，存在的意义是**默认构造的值可被检测为错**，而不是悄悄退化成第一个真格式。
越界强转（如 `static_cast<PixelFormat>(200)`）在 `switch` 走不到任何 case 后统一按 `Unknown` 处理。

实现用 `switch` 覆盖**全部**枚举值（不用查表、不用 `default:`）：
**编译器 `-Wswitch` 会强制新格式被处理**，这是查表法没有的保障。

### 5.2 `Image`

`Object` + `RefCounted<Image>`（与 `Material`/`ImageRef` 同族），用 `intrusive_ptr` 共享。

```cpp
Image(int width, int height, PixelFormat format, int mip_count = 1);
static int mipCapacity(int width, int height) noexcept;
```

**布局**：一张图 = 每个 mip 一层**紧凑 2D 网格**（行间无 padding），整条链**一次分配**。

- 基层 `width() x height()`；
- mip 层 N = `max(1, width >> N) x max(1, height >> N)`；
- `mip_offsets_` 存 N+1 个偏移（最后一个是哨兵 = 总大小），所以"某层多大"就是两个偏移相减。

一次分配 ⇒ 对象是**一次拷贝、一次释放**。

**为什么 cube map 是六个 `Image` 而不是一个带 face 轴的 `Image`**：一个 face **就是**一张图。
给 `Image` 加 face 轴会让每个持有普通 2D 图的调用方都背上 `faces = 1` 的特例。
需要六个 face 一起用的渲染器自己把六个 `Image` 编组 —— 编组是渲染器的事，不是像素数据的事。

**访问是全覆盖（total）的**：mip 访问器把越界索引**夹取**到最后一层，不失败。
理由：它们是 `noexcept`，而**尺寸查询不是发现调用方 bug 的地方**。
越界请求在**构造函数**里被拒绝，并且**抛异常**而不是悄悄分配一张比要求更小的图。

**构造函数拒绝**：非正尺寸、像素大小为 0 的格式（含 `Unknown` 与越界值）、
mip 数不在 `[1, mipCapacity]` —— 全部 `std::invalid_argument`。
像素**零初始化**，所以只填了一部分的图不会把未初始化内存泄进渲染或测试。

## 6. 未来成长清单（这就是"通用名字"的价值，都不用改模块名）

- `ImageCodec` 接口 + 注册表（PNG/JPG 解码**接口**放这，实现另议）
- `Volume` / `ImageStack`（深度图、多帧堆栈、体数据 —— 机器视觉都会碰到）
- 图像元数据（色彩空间、位深、stride 策略）

## 7. 未做（明确推迟）

- `graphics::Texture`（2D + Cube）**已实现**、`Material::textureFile()` → `Texture*` 的迁移**已完成**
  （那个路径字符串在 `src/` 里零调用，只有一个测试引用，是个死 API）；
- 图像解码/编码**已实现** → 见 `loaders/imageio`（`vi::ImageIO`，stb 支持）；
- **mip 生成没有**：解码只产出 1 个 mip，所以现有素材只能填 `mip_count == 1` 的 `Texture`；
- **读回路径**（渲染目标 → `Image`）**尚未有**；
- **后端仍不消费 `Texture`**：face / mip 链不上传、不建 sampler、不进描述符集；
- `RenderTarget` 自己的 `ColorFormat`/`DepthFormat` 枚举与 `imaging::PixelFormat` **重复**，
  将来应并入后者 —— 那是**改公开 API**，需单独一批。

## 8. 验证判据

**第一批（模块建立）**

- `test_imaging`：**19 个测试 / 3 个套件**全过（新建）；
- 全量 `ninja`：**零 error / 零 warning**；
- `test_graphics`、`test_vsg`：**无回归**（纯新增叶子模块，未改任何既有源文件）。

**第二批（`graphics::Texture` + `Material` 迁移）**

- `test_graphics`：**191 → 200**（+9 = 8 个 `TextureTest` + 1 个 `MaterialTest`）；
- 全量 `ninja`：**零 error / 零 warning**；
- `scripts/vsg_selftest_evidence.sh` → `RESULT: PASS`（45 行与基线**逐字节相同**）；
- `scripts/gfx_lavapipe_check.sh` → `RESULT: PASS`（0 VUID）；
- `scripts/check_diagnostic_formats.py` → 0 suspicious / 22 files。
