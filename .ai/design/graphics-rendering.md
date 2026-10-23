# graphics 渲染：目标、MSAA、图源、颜色、着色、阴影

> **一句话**：`RenderTarget` 是描述；采样数是目标属性；produced 与 imported 两种图源不合并；
> 颜色分线性光与 sRGB 编码；着色走 `ShaderProgram` + `ShaderAbi`；阴影由灯声明。
>
> **权威来源**：`RenderTarget.hpp` / `ImageSource.hpp` / `Sampler.hpp` / `ShaderProgram.hpp` /
> `ShaderAbi.hpp` / `Light.hpp`。**待办**：`graphics-followups.md`。

## 1. 设计要点

1.1 **描述与 GPU 资源分离**：`RenderTarget` 只是描述，后端按描述造图。
1.2 **采样数是目标属性**：`setSamples` → `TargetShape` → 管线键 → 通道与图像。
1.3 **图源两种生命周期**：produced（`ImageRef`）与 imported（`Texture`）不合并，只用 `ImageSource` 统一解析形状。
1.4 **颜色进类型**：`Colorf` 线性光、`Color` sRGB 编码，一对有名转换。
1.5 **着色薄接口**：GLSL 源 + 类型化入口；`ShaderAbi` 是 C++ 与 GLSL 的唯一契约。
1.6 **阴影是普通 pass**：灯声明，链建 depth-only pass，没有引擎级阴影子系统。

## 2. 功能点

### 2.1 目标

| # | 功能 | 说明 |
|---|---|---|
| F1 | 颜色附件 | `attachColor(format)`；可多个（MRT / G-buffer）。 |
| F2 | 深度附件 | `attachDepth(D16/D24/D32/D32F)`。 |
| F3 | 共享深度 | `setDepthPromotion(false)` + `shareDepth(DepthView)`；借出方先画，借入方 LOAD。 |
| F4 | 有效性 | 至少一个自己的附件 + 正尺寸；借来的深度不算自己的。 |
| F5 | 读回 | `readColorBuffer` / `readDepthBuffer`；读解析后的画面。 |

### 2.2 MSAA

| # | 功能 | 说明 |
|---|---|---|
| F6 | 采样数 | `RenderTarget::setSamples(n)`；0 视作 1。 |
| F7 | 颜色解析 | MS 图只作 attachment；resolve 图单采样、可采样、可拷贝。 |
| F8 | 深度解析 | MS 图只作 depth attachment；resolve 图单采样，`sample0` 不平均。 |
| F9 | 画面定义 | `picture()` / `depthPicture()`：读回、采样、借出、提升都走它。 |
| F10 | 上限 | `Capabilities::max_samples`；超限报 `SamplesUnsupported`。 |

### 2.3 图源

| # | 功能 | 说明 |
|---|---|---|
| F11 | produced | `ImageRef` = target + attachment（+ depth）；一帧一个 producer。 |
| F12 | imported | `Texture` + revision + 可选 `Sampler`；上传键 `(Texture*, revision)`。 |
| F13 | 窄绑定 | `ProducedImage` 只绑声明的那个 attachment；`ProducedTarget` 保留整张。 |
| F14 | 全屏 ABI | source 颜色附件占 0..C-1，depth 跟随，imported 按声明序填空余 binding。 |
| F15 | Sampler | min/mag、mipmap、address、border、anisotropy、lod bias、LOD range；按值缓存。 |

### 2.4 颜色

| # | 功能 | 说明 |
|---|---|---|
| F16 | `Colorf` | 线性光浮点；清屏 / pass 颜色 / 默认色用它。 |
| F17 | `Color` | sRGB 编码字节；`toLinear()` / `toColor()` 一对转换。 |
| F18 | 文本与字节序 | CSS hex 读写；`fromArgb` / `fromBgr` 带序构造。 |
| F19 | 插值 | `Colorf::lerp` 在线性光下插值。 |

### 2.5 着色与阴影

| # | 功能 | 说明 |
|---|---|---|
| F20 | `ShaderProgram` | Vertex / Fragment 阶段 + 入口点；`setStage` / `setStages` 推进 revision。 |
| F21 | 内置程序 | forward / flat / gbuffer / deferred lighting / shadowed；`builtin_<角色>.<阶段>`。 |
| F22 | ABI | 顶点 location、`VineViewBlock` / `VineDrawBlock` / `VineMaterialBlock` / `VineShadowBlock`、128 B push。 |
| F23 | 透明度 | 唯一通道是 `Node::opacity`；材质不贡献 alpha；blend 恒开。 |
| F24 | 阴影 | `Light::setCastShadow` + `ShadowSettings{resolution, bias}`；`directionalShadowMatrix` 唯一推导。 |
| F25 | 阴影目标 | depth-only `RenderTarget` + `shadowOf` 声明；采样走 `VineShadowBlock`。 |

## 3. 公开接口

### 3.1 目标

| 接口 | 作用 |
|---|---|
| `attachColor()` / `attachDepth()` | 附件 |
| `setSamples()` / `samples()` | 采样数 |
| `setDepthPromotion()` / `shareDepth()` / `sharedDepth()` | 深度共享 |
| `hasColor()` / `hasDepth()` / `colorCount()` / `colorFormat()` / `depthFormat()` | 查询 |
| `setSize()` / `width()` / `height()` | 尺寸 |

### 3.2 图源与采样

| 接口 | 作用 |
|---|---|
| `ImageInput(ImageRef)` / `ImageInput(Texture[, Sampler])` | 声明 |
| `ImageSource::producedImage()` / `producedTarget()` / `imported()` | 解析 |
| `Sampler` 字段 | filter / mipmap / address / border / anisotropy / lod |
| `RenderPass::addInput(Texture[, Sampler])` / `PassDesc::inputs` | pass 输入 |

### 3.3 着色与阴影

| 接口 | 作用 |
|---|---|
| `ShaderProgram::setStages()` / `setStage()` / `stages()` / `revision()` | 程序 |
| `ShaderStage{type, source, entryPoint}` | 阶段 |
| `attributeLocation()` / `Vine*Block` | ABI |
| `Light::setCastShadow()` / `isCastingShadow()` / `shadowSettings()` / `setShadowResolution()` / `setShadowBias()` | 灯 |
| `directionalShadowMatrix(light, box, ...)` | 光相机 |
| `RenderTarget::shadowOf()` / `setShadowOf()` | 阴影目标声明 |

## 4. 注意项

| # | 注意项 |
|---|---|
| N1 | 多重采样目标不能**借入**深度；借出方 / 提升可以是 MS。 |
| N2 | 深度解析用 `sample0`；平均会让“打开 MSAA”改变深度值。 |
| N3 | 默认窗口 / 默认帧缓冲仍单采样；抗锯齿画进 MS 离屏目标再送屏幕。 |
| N4 | produced 输入在内容 pass 只绑声明的 attachment；不要假设整张 target。 |
| N5 | `Sampler` 是缓存键：字段必须是有限数；NaN 会让缓存每帧 miss。 |
| N6 | 材质不贡献 alpha；透明度只来自 `Node::opacity`。 |
| N7 | `ShaderAbi` 的块布局 / location 改一个必须同时改 C++ 与 GLSL。 |
| N8 | 阴影只支持方向光；点/聚光、CSM、PCSS 不做。 |

## 5. 否决项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 硬件 compare sampler | 这一版 pass 深度路径读精确深度；compare sampler 需要独立 ABI。 |
| D2 | 数组 / 3D 纹理 | `Texture::Kind` 只有 D2 / Cube；分层目标是触发条件型设计。 |
| D3 | 多重采样目标借入深度 | 借到的是单采样解析图，而一个帧缓冲附件共用一个样本数。 |
| D4 | 目标格式的 sRGB 变体 | 颜色空间已经在类型层解决；不再给目标加第二套语义。 |
| D5 | 引擎自动阴影子系统 | 阴影是普通 pass；灯声明，链建 pass。 |
| D6 | 点/聚光阴影 / CSM / PCSS | 没有消费者；等真实需求。 |

## 6. 判据

| # | 判据 |
|---|---|
| T1 | `RenderTargetTest` / `MsaaTargetTest` / `SdkPixelTest` / `VsgBackendTest`。 |
| T2 | 1 样本三角形边缘 0 个混合像素；4 样本 > 0；内部仍是纯色。 |
| T3 | 4 样本深度可读且无中间值；解析后与 1 样本逐点一致。 |
| T4 | `ScreenDrawTest` / `ImageSourceTest` / `SamplerTest` / `ColorTest` / `ColorfTest` / `ShaderAbiTest`。 |
| T5 | 阴影：有投影光时地面变暗；无投影光时证据逐字节不变。 |

## 7. 相关文档

`graphics-architecture.md` · `graphics-scene.md` · `graphics-geometry.md` · `vsg-backend.md` ·
`graphics-followups.md`
