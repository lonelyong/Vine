# imaging 模块笔记

> 模块定位、设计依据、被否决的方案：见 `.ai/design/imaging-design.md`。
> 这里只记**结论 + 判据**，方便下次直接接着干。

## 事实速查

- 目录 `src/base/imaging/`，短名 **`Imaging`** → 别名 `vn::Imaging`，宏 `VN_IMAGING_API` / `VN_IMAGING_LIB`，
  命名空间 `vn::imaging`，头 `<vine/imaging/...>`。
- 依赖 **只有 `vn::Core vn::Global`**（叶子，与 `geometry` 同级）。
- 注册点：`src/base/CMakeLists.txt` 的 `add_subdirectory(imaging)`（放在 `geometry` 之后）、
  `tests/CMakeLists.txt` 的 `add_subdirectory(test_imaging)`。
- **改完新文件必须 `cmake -S . -B build`**：`vn_add_library` 用 `file(GLOB_RECURSE ...)` 且**没有**
  `CONFIGURE_DEPENDS`。

## 分工铁律

> **CPU 像素 / 图像数据 → `imaging`；GPU 资源（format / mip / sampler / usage）→ `graphics`。**
> 自问：*"这东西在 headless、无渲染器、无 Qt 的环境里有意义吗？"* 有 → `imaging`。

## 2026-09-12 第一批：模块建立 + `PixelFormat` + `Image`

**背景**：SDK 里像素数据此前**没有对象** —— 材质贴图只是一个路径字符串
（`Material::textureFile() -> String`），`loaders/` 零像素解码，全库零读回路径。
`ImageRef.hpp` 的注释自己承认这个洞（"a material texture is a file PATH"）。

**拍定的方案**：
- `Image`（CPU 像素）**不放** `graphics`（会让 `meshio` **依赖渲染器**；`graphics → Geometry` 说明顶点数据在
  其**下面**，像素数据不该在上面）；**不放** `window`（名字硬凑；改名要同步短名/目录名/宏名/别名四处，漏一处
  Windows 上 `dllimport` 自炸）；**新建叶子模块** `imaging`。
- `Texture` 留 `graphics`，**当前只做 2D + Cube**，不做 1D/3D/array。
- `Image` 也**不能**是 `QImage`（graphics 是 Qt-free；QImage 在 `Qt6::Gui`），Qt 桥留 `appfw`。

**落地**：
- `PixelFormat`：18 个真格式 + `Unknown`；查询走**自由函数** `channelCount`/`bytesPerPixel`/
  `isDepthFormat`/`isSrgbFormat`/`formatName`（加格式只教一处）；
  实现用 `switch` **覆盖全部枚举值**（不用 `default:`），靠 `-Wswitch` 强制新格式被处理；
  越界强转统一读作 `Unknown`。
- `Image`：`Object` + `RefCounted`；一次分配存整条 mip 链（`mip_offsets_` N+1 项，末项是哨兵）；
  mip 层 N = `max(1, w>>N) x max(1, h>>N)`；像素**零初始化**；
  访问器**夹取**越界 mip（`noexcept`，尺寸查询不是发现 bug 的地方）；
  构造函数对非正尺寸 / 像素大小为 0 的格式 / 越界 mip 数**抛 `std::invalid_argument`**。
- **cube map = 六个 `Image`**，`Image` 不带 face 轴（加 face 轴会让所有普通 2D 图调用方背 `faces = 1` 特例）。

**判据**：`test_imaging` **19 tests / 3 suites** 全过（新建）；全量 `ninja` **零 error / 零 warning**；
`test_graphics`、`test_vsg` **无回归**（纯新增叶子模块，未改既有源文件）。

**未做（明确推迟）**：**后端仍不消费 `Texture`**（face / mip 链不上传、不建 sampler、不进描述符集）；
图像解码 / 读回路径未有；`RenderTarget` 自己的 `ColorFormat`/`DepthFormat` 与 `imaging::PixelFormat` **重复**，
应并入后者（改公开 API，需单独一批）。

## 2026-09-12 第二批：`graphics::Texture`（2D + Cube）+ `Material` 迁移

**前置事实**：`Material::textureFile()` 那个路径字符串在 `src/` 里**零调用**，只有一个测试引用 —— 是个死 API，
正是 `ImageRef.hpp` 注释里说的那个洞（*"a material texture is a file PATH"*）。

**落地**：
- 新增 `graphics::Texture`（`Object` + `RefCounted`，与 `Material`/`ImageRef` 同族）：
  `Shape{D2,Cube}`、`faceCount()`（D2=1 / Cube=6）、`width/height/format/mipCount`、
  `setSource(face, intrusive_ptr<const imaging::Image>)` / `source(face)` / `hasSource(face)` / `complete()`。
- **跟 `RenderTarget` 同一个模式**：SDK 类型是**逻辑描述**，不持 GPU 对象；后端物化它。
- 源图必须与描述在 **format / size / mipCount 三项都一致**（不一致在**调用处**就拒绝，
  不让后端到上传时才发现）。
- 读越界 face 是**查询**（答"没有这个 face"，返回 null）；写越界 face 抛 `std::out_of_range`。
- mip 上限**复用** `imaging::Image::mipCapacity`，而不是重述规则。
- `graphics` 现在 **PUBLIC 依赖 `vn::Imaging`**。
- `Material`：删 `textureFile()`/`setTextureFile()`（死 API），改
  `texture()`/`setTexture(intrusive_ptr<Texture>)`。

**判据**：`test_graphics` **191 → 200**（+9 = 8 个 `TextureTest` + 1 个 `MaterialTest`）；
全量 `ninja` 零 error / 零 warning；`vsg_selftest_evidence.sh` → PASS（**45 行逐字节相同**，后端零改动）；
`gfx_lavapipe_check.sh` → PASS（0 VUID）；`check_diagnostic_formats.py` → 0 suspicious。
