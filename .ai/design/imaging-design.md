# imaging 模块设计（CPU 像素数据）

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：CPU 像素格式、`Image` 与 mip 链；不涉及纹理/采样器/设备。
> **权威来源**：`src/base/imaging/sdk/vine/imaging/**`、`tests/test_imaging`。
> **待办**：`../backlog/`

## 1. 概要设计

### 1.1 问题与背景

SDK 里曾经**像素数据没有对象**：材质贴图只是路径字符串，没有解码，没有读回，`ImageRef` 只是绑定句柄。
`imaging` 补的是这块**数据侧**：一张图在内存里长什么样。

### 1.2 目标

- `PixelFormat`：每像素字节布局 + 查询函数。
- `Image`：一张 2D 图 + 可选 mip 链，一次分配、一次拷贝、一次释放。
- 可被加载器（解码文件）、渲染层（读回结果）、无渲染工具同时使用。

### 1.3 非目标

- 不认识纹理、采样器、设备或渲染器。
- 不做 `QImage` 桥（Qt 侧转换留在 appfw）。
- 不做 1D / 3D / array；当前只做 2D 与 Cube（Cube = 六个 `Image`）。
- 不做 mip 生成器（解码只产出第 0 级；生成另议）。

### 1.4 一句话结论

`imaging` 是 base 层的 **CPU 像素叶子模块**：`PixelFormat` 是格式事实的唯一映射点，`Image` 是 2D 图 + mip 链；
GPU 资源（纹理/采样器/设备）留在 `graphics`，两边只靠 `IntrusivePtr<Image>` 交接。

## 2. 设计要点

### 2.1 架构定位

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

### 2.2 与 graphics 的分工铁律

> **CPU 像素 / 图像数据 → `imaging`**
> **GPU 资源（format / mip / sampler / usage）→ `graphics`**

判定问句：**“这个东西在 headless、没有渲染器、没有 Qt 的环境里有意义吗？”** 有 → `imaging`；没有 → `graphics`。

### 2.3 为什么 `Image` 不放 `graphics`

把 `Image` 放进 `graphics` 是“今天最省、三个月后最痛”：

1. **`meshio → graphics` 的层级倒置**：assimp 的 `aiTexture::pcData` 天然产出解码后的像素。
2. **无渲染工具用不了**：`urdf2vine` 刻意不链 graphics/Qt。
3. **层级逻辑倒置**：顶点数据 `Geometry` 在 `graphics` 下面，像素数据是同一类东西。

补充：`Image` 也**不能**是 `QImage` —— `graphics` 是 Qt-free 的；Qt 侧转换桥留在 appfw。

### 2.4 为什么 `Texture` 留 `graphics`

纹理是 GPU 资源：消费者只有 `Material`（采样）、`RenderTarget`（附件）、`ImageRef`（身份）、后端（创建）。
`graphics` 以下的模块没法有意义地谈论 GPU 纹理。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | `PixelFormat` | 18 个真格式 + `Unknown`；查询走自由函数 | 已落地 |
| F2 | `Image` | 2D 图 + mip 链，一次分配 | 已落地 |
| F3 | mip 布局 | 每层紧凑 2D 网格，偏移表 + 哨兵 | 已落地 |
| F4 | 格式查询 | `channelCount` / `bytesPerPixel` / `isDepthFormat` / `isSrgbFormat` / `formatName` | 已落地 |

### 3.1.1 F1 `PixelFormat`

- **行为**：`enum class PixelFormat : std::uint8_t`，18 个真格式 + `Unknown`；越界强转按 `Unknown` 处理。
- **约束**（C1）：查询走自由函数，不走枚举分支；`switch` 覆盖全部枚举值，不用 `default:`。
- **要求**（R1）：`Unknown` 是唯一“不是格式”的值，默认构造可被检测为错，不悄悄退化成第一个真格式。
- **验收**（T1）：`test_imaging` 的 PixelFormat 用例。
- **注意**（N1）：`D24UnormS8Uint` 的 `channelCount` 报 2（深度+模板都算通道）。

### 3.1.2 F2 `Image`

- **行为**：`Object` + `RefCounted<Image>`；`Image(width, height, format, mip_count = 1)`；
  `mipCapacity(width, height)`。
- **约束**（C2）：非正尺寸、像素大小为 0 的格式、mip 数不在 `[1, mipCapacity]` 全部
  `std::invalid_argument`；像素零初始化。
- **要求**（R2）：一次分配、一次拷贝、一次释放；mip 访问器越界夹取到最后一层，不失败。
- **验收**（T2）：`test_imaging` 的 Image/布局用例。
- **注意**（N2）：越界请求在构造函数里拒绝；尺寸查询不是发现调用方 bug 的地方。

### 3.1.3 F3 mip 布局

- **行为**：基层 `width x height`；mip N = `max(1, width >> N) x max(1, height >> N)`；
  `mip_offsets_` 存 N+1 个偏移（最后一个是哨兵 = 总大小）。
- **约束**（C3）：每层紧凑 2D 网格，行间无 padding；整条链一次分配。
- **要求**（R3）：某层大小 = 两个偏移相减；不需要额外尺寸表。
- **验收**（T3）：`test_imaging` 的 mip 用例。
- **注意**（N3）：上传侧已按级数工作（`makeMipmapLayout`），缺的只是生成第 0 级之外的层。

### 3.1.4 F4 格式查询

- **行为**：`channelCount` / `bytesPerPixel` / `isDepthFormat` / `isSrgbFormat` / `formatName`。
- **约束**（C4）：`formatName` 返回静态字面量，永不返回 null；打包格式的 `bytesPerPixel` 报整个 texel 大小。
- **要求**（R4）：加一个格式 = 只教一处（`PixelFormat.cpp`），不是教所有调用点。
- **验收**（T4）：`test_imaging` 的格式查询用例。
- **注意**（N4）：`-Wswitch` 会强制新格式被处理，这是查表法没有的保障。

## 4. 公开接口

### 4.1 `PixelFormat.hpp`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `enum class PixelFormat` | 18 个真格式 + `Unknown` | 越界强转按 `Unknown` |
| `channelCount(PixelFormat)` | 通道数 | 深度+模板算 2 |
| `bytesPerPixel(PixelFormat)` | 每像素字节 | 打包格式报 texel 大小 |
| `isDepthFormat` / `isSrgbFormat` | 格式分类 | `noexcept` |
| `formatName` | 静态名字 | 永不 null |

### 4.2 `Image.hpp`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `Image(width, height, format, mip_count)` | 构造并零初始化 | 非法参数抛 `invalid_argument` |
| `mipCapacity(width, height)` | 最大 mip 数 | 静态 |
| `width()` / `height()` / `format()` / `mipCount()` | 查询 | — |
| `mipData(level)` / `mipSize(level)` | 访问 | 越界夹取 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 查询走自由函数；`switch` 覆盖全部枚举值，不用 `default:`。 |
| C2 | 非法尺寸/格式/mip 数在构造函数拒绝；像素零初始化。 |
| C3 | 每层紧凑 2D 网格，行间无 padding；整条链一次分配。 |
| C4 | `formatName` 返回静态字面量；`bytesPerPixel` 报 texel 大小。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | `Unknown` 默认构造可检测，不悄悄退化。 |
| R2 | mip 访问器越界夹取，不失败；尺寸查询不发现 bug。 |
| R3 | 层级只向上：`imaging` 不依赖 `graphics`/Qt。 |
| R4 | 加格式只改 `PixelFormat.cpp` 一处。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | `Image` 是 2D；Cube = 六个 `Image`，不加 face 轴。 |
| N2 | `mip_offsets_` 的最后一个值是总大小哨兵。 |
| N3 | 像素数据在构造时全零，未填部分不泄内存。 |

## 6. 注意事项与陷阱

### 6.1 `Unknown` 不是第一个真格式

- 会怎么错：默认构造的值被当成 `R8G8B8A8`，悄悄画出黑/错色。
- 怎么判：`formatName(Unknown)` 返回 `"Unknown"`；构造函数拒绝像素大小为 0 的格式。
- 怎么防：`switch` 覆盖全部枚举值，不用 `default:`。

### 6.2 mip 偏移的哨兵

- 会怎么错：少存一个偏移，最后一层的 `mipSize()` 越界。
- 怎么判：`mip_offsets_.size() == mip_count + 1`。
- 怎么防：只通过两个偏移相减求大小，不另存尺寸表。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | `Image` 放进 `graphics` | `meshio → graphics` 层级倒置；无渲染工具用不了。 | — |
| D2 | `Image` = `QImage` | `graphics` 是 Qt-free；Qt 桥留 appfw。 | — |
| D3 | 把 `window` 改名 `display` 收 `Image` | 名字硬凑；改名牵动短名/目录/宏/别名四处。 | — |
| D4 | 1D / 3D / array | 过度设计；当前只做 2D 与 Cube。 | — |
| D5 | `Image` 带 face 轴 | 每个 2D 调用方都背 `faces=1` 特例；Cube 编组是渲染器的事。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | PixelFormat 查询/Unknown | `test_imaging` |
| T2 | Image 构造/零初始化/非法参数 | `test_imaging` |
| T3 | mip 布局/偏移/夹取 | `test_imaging` |
| T4 | Texture/Material 迁移无回归 | `test_graphics`（200）、`test_vsg` |

## 9. 状态与演进

### 9.1 当前状态

`PixelFormat` / `Image` 已落地；`graphics::Texture`（2D + Cube）与 `Material::textureFile()` → `Texture*`
迁移已完成；`ImageIO` 解码已实现（公开面见 `imageio-design.md`）。

### 9.2 已知缺口 / 待办

- **mip 生成没有**：解码只产出 1 个 mip；上传侧已按级数工作，缺“把第 0 级扩成一条链”的一步。
- **`imaging::Image` 级读回包装**没有；后端已有 `readColorBuffer()/readDepthBuffer()`，窗口读回走 X11 脚本。
- `RenderTarget` 的 `ColorFormat`/`DepthFormat` 与 `PixelFormat` 重复，将来应并入后者（改公开 API，单独一批）。
- `Texture3D`/`2DArray` 不能表达（`Image` 严格二维）。

### 9.3 兼容性 / 迁移

`Material::textureFile()` 已迁移到 `Texture*`；旧路径字符串在 `src/` 里零调用。新代码用
`IntrusivePtr<Image>` 交接 CPU 像素，GPU 侧只用 `Texture`。

## 10. 参考

### 10.1 代码

- `src/base/imaging/sdk/vine/imaging/PixelFormat.hpp`
- `src/base/imaging/sdk/vine/imaging/Image.hpp`
- `src/base/imaging/src/PixelFormat.cpp`
- `tests/test_imaging`

### 10.2 文档与 ADR

- `.ai/memory/imaging.md`
- `.ai/design/graphics-rendering.md`
- `imageio-design.md`
- `../backlog/`

### 10.3 外部资料

- `loaders/imageio`（`vn::ImageIO`，stb 支持）。
- `graphics::Texture` / `Material` 的 GPU 侧契约。
