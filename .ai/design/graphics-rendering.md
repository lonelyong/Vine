# graphics 渲染：目标、MSAA、图源、颜色、着色、阴影

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：目标、MSAA、图源、颜色空间、着色与阴影。
> **权威来源**：`RenderTarget.hpp` / `ImageSource.hpp` / `Sampler.hpp` / `ShaderProgram.hpp` /
> `ShaderAbi.hpp` / `Light.hpp`。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

本文件定义 graphics 渲染：目标、MSAA、图源、颜色、着色、阴影 的现行设计：先给对象与边界，再给功能、接口、约束与验收。

### 1.2 目标

- 把 §2 的设计要点落成 §3 的功能点、§4 的接口与 §8 的验收。
- 让“现在是什么”与“为什么这么定”分开：理由在 §2/§7，历史看 git。

### 1.3 非目标

- 不在本文件重复头文件里的完整签名；接口只给契约与指针。
- 不在本文件保存已关闭的历史或逐轮审查记录。

### 1.4 一句话结论

> `RenderTarget` 是描述；采样数是目标属性；produced 与 imported 两种图源不合并； 颜色分线性光与 sRGB 编码；着色走 `ShaderProgram` + `ShaderAbi`；阴影由灯声明。

## 2. 设计要点

2.1 **描述与 GPU 资源分离**：`RenderTarget` 只是描述，后端按描述造图。
2.2 **采样数是目标属性**：`setSamples` → `TargetShape` → 管线键 → 通道与图像。
2.3 **图源两种生命周期**：produced（`ImageRef`）与 imported（`Texture`）不合并，只用 `ImageSource` 统一解析形状。
2.4 **颜色进类型**：`Colorf` 线性光、`Color` sRGB 编码，一对有名转换。
2.5 **着色薄接口**：GLSL 源 + 类型化入口；`ShaderAbi` 是 C++ 与 GLSL 的唯一契约。
2.6 **阴影是普通 pass**：灯声明，链建 depth-only pass，没有引擎级阴影子系统。
2.7 **阴影的四个归属**：请求在灯（`Light::setCastShadow` + `ShadowSettings`）、身份在图（`RenderTarget::setShadowOf` 与 `setProducerViewProjection`，plan 只抄成 `CompiledPass::shadow`）、约定在内置 ABI（`VineShadowBlock` 与阴影项，可选变体）、组合在预设（`ChainBuilder` 的 recipe）。引擎核心的 `PassDesc` 没有 shadow 字段（见 D5）。

  **内置阴影管线的能力边界**（`PassChain::build` 造的那条，不是 pass 模型的上限）：
  - **一张图**：每链只建一张阴影图，服务**一盏灯** —— 内容里第一个 enabled、`isCastingShadow` 且种类可建的方向光或聚光。第二盏投射灯没有图。
  - **两种光相机**：方向光走正交拟合（`directionalShadowMatrix`），聚光走透视拟合（眼在灯位、锥角=外锥角×1.02，`spotShadowMatrix`）；两者出口都是同一份 `VineShadowBlock`（一张矩阵），着色不分流。
  - **点光不支持**：点光的阴影是同一张图的六个面，今天的 ABI 没有多矩阵 / cube，所以点光 `setCastShadow` 在这条管线里一定不建 pass（backlog S6）。
  - **三种不建的理由分开报**（每 build 一次，`UnsupportedRequest`）：第二个投射灯 / 点光不可表示 / 这条路本身不产生光照（宿主自己的 lighting program、或不需要着色的 mode）。分着数是为了让“N 盏灯没投影”能被定位，而不是靠猜。

## 3. 功能点

### 3.1 目标

| # | 功能 | 说明 |
|---|---|---|
| F1 | 颜色附件 | `attachColor(format)`；可多个（MRT / G-buffer）。 |
| F2 | 深度附件 | `attachDepth(D16/D24/D32/D32F)`。 |
| F3 | 共享深度 | `setDepthPromotion(false)` + `shareDepth(DepthView)`；借出方先画，借入方 LOAD。 |
| F4 | 有效性 | 至少一个自己的附件 + 正尺寸；借来的深度不算自己的。 |
| F5 | 读回 | `readColorBuffer` / `readDepthBuffer`；读解析后的画面。 |

### 3.2 MSAA

| # | 功能 | 说明 |
|---|---|---|
| F6 | 采样数 | `RenderTarget::setSamples(n)`；0 视作 1。 |
| F7 | 颜色解析 | MS 图只作 attachment；resolve 图单采样、可采样、可拷贝。 |
| F8 | 深度解析 | MS 图只作 depth attachment；resolve 图单采样，`sample0` 不平均。 |
| F9 | 画面定义 | `picture()` / `depthPicture()`：读回、采样、借出、提升都走它。 |
| F10 | 上限 | `Capabilities::max_samples`；超限报 `SamplesUnsupported`。 |

### 3.3 图源

| # | 功能 | 说明 |
|---|---|---|
| F11 | produced | `ImageRef` = target + attachment（+ depth）；一帧一个 producer。 |
| F12 | imported | `Texture` + revision + 可选 `Sampler`；上传键 `(Texture*, revision)`。 |
| F13 | 窄绑定 | `ProducedImage` 只绑声明的那个 attachment；`ProducedTarget` 保留整张。 |
| F14 | 全屏 ABI | source 颜色附件占 0..C-1，depth 跟随，imported 按声明序填空余 binding。 |
| F15 | Sampler | min/mag、mipmap、address、border、anisotropy、lod bias、LOD range；按值缓存。 |

### 3.4 颜色

| # | 功能 | 说明 |
|---|---|---|
| F16 | `Colorf` | 线性光浮点；清屏 / pass 颜色 / 默认色用它。 |
| F17 | `Color` | sRGB 编码字节；`toLinear()` / `toColor()` 一对转换。 |
| F18 | 文本与字节序 | CSS hex 读写；`fromArgb` / `fromBgr` 带序构造。 |
| F19 | 插值 | `Colorf::lerp` 在线性光下插值。 |

### 3.5 着色与阴影

| # | 功能 | 说明 |
|---|---|---|
| F20 | `ShaderProgram` | Vertex / Fragment 阶段 + 入口点；`setStage` / `setStages` 推进 revision。 |
| F21 | 内置程序 | forward / flat / gbuffer / deferred lighting / shadowed；`builtin_<角色>.<阶段>`。 |
| F22 | ABI | 顶点 location、`VineViewBlock` / `VineDrawBlock` / `VineMaterialBlock` / `VineShadowBlock`、128 B push。 |
| F23 | 透明度 | 唯一通道是 `Node::opacity`；材质不贡献 alpha；blend 恒开。 |
| F24 | 阴影 | `Light::setCastShadow` + `ShadowSettings{resolution, bias}`；光相机由 `directionalShadowMatrix`（正交）/ `spotShadowMatrix`（透视）推导。**每链一张图、一盏灯（第一个可建的方向光或聚光）**，点光不支持（见 2.7）。 |
| F26 | 线宽 | `StateNode::setLineWidth` / `RenderPass::setLineWidthOverride`：**线图元**（`Topology::Lines`）的宽度，framebuffer 像素，动态状态（`vkCmdSetLineWidth`）所以不重编；`wideLines` 是设备必需特性，上限是设备 `lineWidthRange`（`Capabilities::max_line_width`），下限 1.0。它**不加宽** `PolygonMode::Line` 的线框边。 |


## 4. 公开接口

### 4.1 目标

| 接口 | 作用 |
|---|---|
| `attachColor()` / `attachDepth()` | 附件 |
| `setSamples()` / `samples()` | 采样数 |
| `setDepthPromotion()` / `shareDepth()` / `sharedDepth()` | 深度共享 |
| `hasColor()` / `hasDepth()` / `colorCount()` / `colorFormat()` / `depthFormat()` | 查询 |
| `setSize()` / `width()` / `height()` | 尺寸 |

### 4.2 图源与采样

| 接口 | 作用 |
|---|---|
| `ImageInput(ImageRef)` / `ImageInput(Texture[, Sampler])` | 声明 |
| `ImageSource::producedImage()` / `producedTarget()` / `imported()` | 解析 |
| `Sampler` 字段 | filter / mipmap / address / border / anisotropy / lod |
| `RenderPass::addInput(Texture[, Sampler])` / `PassDesc::inputs` | pass 输入 |

### 4.3 着色与阴影

| 接口 | 作用 |
|---|---|
| `ShaderProgram::setStages()` / `setStage()` / `stages()` / `revision()` | 程序 |
| `ShaderStage{type, source, entryPoint}` | 阶段 |
| `attributeLocation()` / `Vine*Block` | ABI |
| `Light::setCastShadow()` / `isCastingShadow()` / `shadowSettings()` / `setShadowResolution()` / `setShadowBias()` | 灯 |
| `directionalShadowMatrix(light, box, ...)` | 光相机（方向光，正交） |
| `spotShadowMatrix(light, box, ...)` | 光相机（聚光，透视，眼在灯位，锥角=外锥角×1.02） |
| `RenderTarget::shadowOf()` / `setShadowOf()` | 阴影目标声明 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 采样数是目标属性；图像/通道/管线三者由 `TargetShape` 统一驱动。 |
| C2 | produced 与 imported 图源不合并；生命周期不同。 |
| C3 | 颜色分线性光 `Colorf` 与 sRGB 编码 `Color`。 |
| C4 | 引擎核心的 pass 描述没有 shadow 字段；一张图“是阴影”由目标自己声明，消费者不靠“第一个带深度的输入”猜。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | 阴影是普通 pass：灯声明，链建 pass，不做引擎内置阴影子系统。 |
| R2 | 多重采样目标的深度解析取 `sample0`，不做平均。 |
| R3 | 目标格式不再加第二套颜色语义。 |
| R4 | 阴影的约定只在内置着色里：不用内置程序的 pass 与 `VineShadowBlock` 无关，把 map 当普通 produced 图采样即可。 |

### 5.3 不变量（N#）

| # | 注意项 |
|---|---|
| N1 | 多重采样目标不能**借入**深度；借出方 / 提升可以是 MS。 |
| N2 | 深度解析用 `sample0`；平均会让“打开 MSAA”改变深度值。 |
| N3 | 默认窗口 / 默认帧缓冲仍单采样；抗锯齿画进 MS 离屏目标再送屏幕。 |
| N4 | produced 输入在内容 pass 只绑声明的 attachment；不要假设整张 target。 |
| N5 | `Sampler` 是缓存键：字段必须是有限数；NaN 会让缓存每帧 miss。 |
| N6 | 材质不贡献 alpha；透明度只来自 `Node::opacity`。 |
| N7 | `ShaderAbi` 的块布局 / location 改一个必须同时改 C++ 与 GLSL。 |
| N8 | 阴影投射灯是方向光与聚光（正交 / 透视光相机，同一张 2D 深度图、同一份 `VineShadowBlock`）；**每链只有一张图** —— 第二个投射灯没有图；点光需要六面 cube，`setCastShadow` 在点光上不建 pass。三种不建的理由（第二个投射灯 / 点光 / 这条路不产生光照）由 `ChainBuilder::reportRequestedShadows` 分别报出，见 2.7 与 backlog S6。CSM / PCSS 不做。 |

## 6. 注意事项与陷阱

### 6.1 一事实一家

- 会怎么错：同一份状态被两处保存，改一处另一处过期。
- 怎么判：出现两份可写的值/两个可写的注册表。
- 怎么防：回到 §5 的 N#；只留一个 owner，其余只读。

### 6.2 后端/平台的边界

- 会怎么错：把平台差异写进引擎/应用层，换平台时到处分叉。
- 怎么判：出现“如果 Windows/X11 就……”的分支。
- 怎么防：平台事实留在后端，引擎只消费编译好的帧值。

### 6.3 阴影图的身份靠猜

- 会怎么错：消费者把“第一个带可采样深度的输入”当阴影图——G-buffer 也有深度，于是太阳的阴影读到 G-buffer 的深度，
  只有一半画面对（这是参考后端测过的缺陷）。
- 怎么判：喂进 shader 的图和 `CompiledPass::shadow` 声明的不是同一张；或 map 的 owner 不在本次 call 的灯表里。
- 怎么防：身份只来自目标（`setShadowOf` + `setProducerViewProjection`），plan 只抄不推；`ShadowFacts` 是唯一来源。

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 硬件 compare sampler | 这一版 pass 深度路径读精确深度；compare sampler 需要独立 ABI。 |
| D2 | 数组 / 3D 纹理 | `Texture::Kind` 只有 D2 / Cube；分层目标是触发条件型设计。 |
| D3 | 多重采样目标借入深度 | 借到的是单采样解析图，而一个帧缓冲附件共用一个样本数。详见 [ADR](../decisions/ADR-2026-10-22-msaa-target-property.md)。 |
| D4 | 目标格式的 sRGB 变体 | 颜色空间已经在类型层解决；不再给目标加第二套语义。详见 [ADR](../decisions/ADR-2026-10-21-colour-space-in-types.md)。 |
| D5 | 引擎自动阴影子系统，或给 `PassDesc` 加 shadow 字段 / `PassStage::Shadow` | 阴影是一个或多个普通 pass 的组合；引擎只提供通用的 `Depth` 档位、图身份与解析事实。加字段会把“谁投影、怎么读”复制进 pass 模型（见 §2.7）。 |
| D6 | 点/聚光阴影 / CSM / PCSS | 没有消费者；点/聚光的 `setCastShadow` 今日不建 pass 且被报告（backlog S6 等真实需求）。 |

## 8. 测试与验收

| # | 判据 |
|---|---|
| T1 | `RenderTargetTest` / `MsaaTargetTest` / `SdkPixelTest` / `VsgBackendTest`。 |
| T2 | 1 样本三角形边缘 0 个混合像素；4 样本 > 0；内部仍是纯色。 |
| T3 | 4 样本深度可读且无中间值；解析后与 1 样本逐点一致。 |
| T4 | `ScreenDrawTest` / `ImageSourceTest` / `SamplerTest` / `ColorTest` / `ColorfTest` / `ShaderAbiTest`。 |
| T5 | 阴影：有投影光时地面变暗；无投影光时证据逐字节不变。 |

## 9. 状态与演进

### 9.1 当前状态

现行设计；功能点与接口以上表为准。

### 9.2 已知缺口 / 待办

见 `../backlog/graphics.md`；开放项按触发条件保留。

### 9.3 兼容性 / 迁移

旧名/旧路径只在 §7 已否决选项或 git 历史里出现；新代码按 §4 的接口写。

## 10. 参考

`graphics-architecture.md` · `graphics-scene.md` · `graphics-geometry.md` · `vsg-backend.md` ·
`../backlog/graphics.md`
