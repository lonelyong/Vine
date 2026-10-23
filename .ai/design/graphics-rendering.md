# graphics 渲染：目标、MSAA、图源、颜色、着色、阴影

> 单一来源：一条画面用到的资源与着色。头文件 `RenderTarget.hpp` / `ImageSource.hpp` / `Sampler.hpp` /
> `ShaderProgram.hpp` / `ShaderAbi.hpp` / `Light.hpp` 是 API 细节的权威。待办见 `graphics-followups.md`。

## 1. 目标

- `RenderTarget` 是**描述**：颜色附件（可多个 / MRT）、可选深度、尺寸、采样数。后端按描述造图。
- 有效性：至少一个自己的附件 + 正尺寸；`shareDepth` 借来的深度不算自己的附件。
- 深度：`attachDepth(format)`；`setDepthPromotion(false)` 让深度停在 depth 布局，供同一帧的另一条 pass 借用；
  `shareDepth(DepthView)` 让本目标复用别人的深度（借出方先画，借入方 LOAD）。
- 读回：`RenderDevice::readColorBuffer` / `readDepthBuffer`；读的是**解析后的画面**，不是多重采样附件。

## 2. MSAA

- `RenderTarget::setSamples(n)` 是唯一入口（0 视作 1）；采样数进 `TargetShape` ⇒
  `RenderPassCompatibility::samples` ⇒ 管线与图像。
- 多重采样目标自建 MS 图 + **颜色与深度各一张单采样 resolve 图**：
  - 颜色：MS 图只作 attachment，resolve 图 `COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC`；
  - 深度：MS 图只作 `DEPTH_STENCIL_ATTACHMENT`，resolve 图 `DEPTH_STENCIL_ATTACHMENT|TRANSFER_SRC[|SAMPLED]`，
    解析模式 `sample0`（不平均）。
- `picture()` / `depthPicture()` 是“画面”的唯一定义：单采样时是绘制图，多重采样时是 resolve 图；
  读回 / 采样 / 借出 / 提升都走它。
- 显式拒绝：`samples > max_samples`；多重采样目标**借入**深度（借出方可以是 MS，提升也可以是 MS）。
  默认窗口 / 默认帧缓冲仍单采样；窗口抗锯齿 = 画进 MS 离屏目标，再把 resolve 图当屏幕 pass 的输入。
- `Capabilities::max_samples` 报设备限制。

## 3. 图源

- `ImageInput` 是一条 pass 的输入声明，保持 `ImageRef`（produced）或 `Texture`（imported）的拥有关系；
  `ImageSource` 是解析后的值。
- produced：`ImageRef` = target + attachment（+ depth）；一帧内一张图最多一个 producer；内容 pass 只绑定
  声明的那个 attachment，`ProducedTarget` 保留“整张 target”的旧拼法（全屏 ABI / G-buffer 预览）。
- imported：`Texture` + revision + 可选 `Sampler`；上传键 `(Texture*, revision)`，sampler 状态按值缓存；
  `InputSetCache` 的键是 `(view, sampler)`。
- `Sampler` 是值：min/mag filter、mipmap mode、三个 address mode、border color、anisotropy、lod bias、
  LOD range。**没有 compare sampler**：硬件深度比较不在这一版。
- 全屏 ABI：source 的颜色附件占 0..C-1，depth 跟随，imported 图按声明序填程序声明的其余 sampler binding；
  数量对不上由后端拒绝并报一次。

## 4. 颜色

- `Colorf` = 线性光浮点；`Color` = sRGB 编码字节。两者只由一对有名转换桥接：
  `Color::toLinear()` / `Colorf::toColor()`（IEC 61966-2-1 分段曲线，alpha 不编码）。
- SDK 的作者颜色只有 `Colorf` 一种：清屏、pass 颜色、默认色都是线性值；后端不再 `/255`。
- `Color` 提供 CSS hex 文本（`#rgb[a]` / `#rrggbb[aa]`）与带序的 `fromArgb` / `fromBgr`；
  没有通用移位入口。`Colorf::lerp` 在线性光下插值。

## 5. 着色

- `ShaderProgram` = 一组 `ShaderStage`（Vertex / Fragment）+ 入口点；`setStages` / `setStage` 推进内容
  revision，后端按 revision 重编变体。
- 内置程序（`BuiltinShaders.hpp`）覆盖 forward / flat / gbuffer / deferred lighting / shadowed 等；
  文件命名 `builtin_<角色>.<阶段>`，构建期嵌入。
- `ShaderAbi.hpp` 是 C++ 与 GLSL 的唯一契约：顶点 location、`VineViewBlock` / `VineDrawBlock` /
  `VineMaterialBlock` / `VineShadowBlock` 的布局、128 B push constant 预算。两侧改一个必须同时改另一个。
- 透明度：引擎唯一透明通道是 `Node::opacity`；内置前向着色把它写进片元 alpha；材质不贡献 alpha。
  混合恒开（单颜色附件内容），`BlendState::enabled` 只选因子。
- pass 可覆盖 program（`PassDesc::program_override`），覆盖在**记录时**解析，不改共享命令表。

## 6. 阴影

- 阴影由**灯**声明：`Light::setCastShadow(true)` + `ShadowSettings{resolution, bias}`；
  `PassChain` 为投影光建一条 depth-only pass（自建光相机 + 只深度 `RenderTarget` + 负 order + 内容绑定）。
- `directionalShadowMatrix(light, content_box, ...)` 是光相机的唯一推导；目标通过 `shadowOf` 声明自己是谁的
  阴影图，采样侧按 `VineShadowBlock` 投影比较。
- 阴影 pass 是普通 pass：没有引擎级阴影子系统 / 自动调度；没有投影光就不建目标、不跑 pass。
- 不做：点/聚光阴影、CSM、PCSS、同场景多个投影方向光的合成。

## 7. 判据

`RenderTargetTest` / `MsaaTargetTest` / `SdkPixelTest` / `VsgBackendTest`（颜色、采样、阴影、图源）、
`ScreenDrawTest` / `ImageSourceTest` / `SamplerTest` / `ColorTest` / `ColorfTest` / `ShaderAbiTest`。
