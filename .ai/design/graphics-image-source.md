# 图源统一：`ImageRef` / `Texture` 与 pass 输入（2026-10-19）

> **状态：切片 1–3 的内容路径已落地（2026-10-19）；全屏 ABI 的 imported 绑定仍登记。** 公开头已改：
> `ImageInput` / `ImageSource`、`RenderPass::addInput(Texture)`、`PassDesc::inputs`、
> `FrameDescription::Pass::inputs`。Content（Scene）pass 按声明序绑定 imported 图；full-screen ABI 目前只绑
> produced source + shadow map，引擎会报告声明 imported 图的 ScreenPass 并跳过它（见 §5 末尾）。
> 触发条件：出现“把一张静态/导入贴图当某条 pass 的输入”的真实需求（离线烘焙的输入图、预览磁盘图、
> 编辑器把选中纹理喂进屏幕 pass）。今天的两套图源模型见 `graphics-object-model-vs-3d-frameworks.md` §3 第 7 条。

## 0. 结论

**不把 `Texture` 硬塞进 `RenderTarget`，也不让 `ImageRef` 同时承担两种生命周期；把“pass 的一张输入图”提升成一个
值类型 `ImageSource`（produced | imported）。**

- `ImageRef` 继续表示**帧内产出图**的身份（target + attachment），保留“一张图最多一个 producer”的规则；
- `Texture` 继续表示**导入/静态图**的身份（对象 + revision），不参与产出/覆盖规则；
- `RenderPass` 的输入声明与 `FrameDescription` 的已解析输入改成一个 `ImageSource` 序列；
- 后端只问一件事：“这张输入图是 target 附件还是 texture？”——绑定路径不同，采样语义相同。

这样统一的是**描述**，不是**生命周期**：produced 图和 imported 图本来就有不同的来源、owner 和失效规则，
把它们塞进同一个继承根只会把两种失效规则混在一起。

## 1. 今天的两套模型

| | `ImageRef` | `Texture` |
|---|---|---|
| 表示 | target + attachment | 一张采样图（D2 / Cube）+ 源图 |
| 生命周期 | 帧内产出；由 target 的 owner 保活 | 宿主/资产持有；可跨帧 |
| 身份 | `(RenderTarget*, attachment, depth)` | `Texture*` + `revision()` |
| 是否参与 wiring | 是（producer / consumer、单 producer 规则） | 否 |
| 能否作 pass 输入 | 是（经 `RenderPass::addInput`） | **否** |
| 现有消费者 | `ScreenPass` / deferred lighting / 阴影 | `Material::setTexture`（仅材质） |

`RenderPass::resolveInputTextures` 收到的是 `std::vector<raw_ptr<RenderTarget>>`，
`FrameDescription::Pass::inputs` 也是 target 指针；后端的 `CompiledInput` 只认识 target 身份。
所以“贴图作为 pass 输入”不是少一个 setter，而是**已解析输入的表示里没有第二种图**。

## 2. 要求与不变量

1. **声明可以是两种图**：produced（`ImageRef`）或 imported（`Texture`）。
2. **身份不混**：produced 的身份仍是 `(target, attachment)`；imported 的身份仍是 `(Texture*, revision)`。
3. **产出规则只作用于 produced**：imported 图没有 producer，也不参与“一张图最多一个 producer”的检查。
4. **已解析输入有序**：一个 pass 的输入顺序就是声明顺序；produced / imported 可以混排，后端按同一序绑定。
5. **借用规则不变**：`FrameDescription` 里的图仍只在 `executeFrame()` 期间有效；imported 图的 `Texture`
   由声明持有，后端要留存就按 `(Texture*, revision)` 缓存。
6. **采样器不新增抽象**：`Texture` 目前没有 sampler state，pass 输入先用后端默认采样器（与
   `Material::setTexture` 同一条路径）；等 anisotropic/wrap 成为真实需求再加 `Sampler`。
7. **现有声明不回归**：`addInput(ImageRef)` / `addInputTarget` / `addInputName` 的行为与顺序保持不变。

## 3. 形状：`ImageSource` 值，不是继承根

```cpp
/// One pass input: the image, and where it comes from.
struct ImageSource
{
    enum class Kind : std::uint8_t
    {
        Produced,  ///< An image a pass wrote this frame (target + attachment).
        Imported,  ///< A texture the host owns (Texture + revision).
    };

    Kind                         kind{ Kind::Produced };
    raw_ptr<const RenderTarget>  target{ nullptr };   // Produced
    int                          attachment{ 0 };     // Produced (colour index; depth is a flag)
    bool                         depth{ false };      // Produced
    raw_ptr<const Texture>       texture{ nullptr };  // Imported
    std::uint64_t                revision{ 0 };       // Imported (Texture::revision())
};
```

- 它是**已解析输入**的形状，不是新的对象。`ImageRef` 与 `Texture` 仍是各自的声明对象。
- `ImageSource` 可以值传递、可比较、可 dump；后端不需要知道 `ImageRef` 的 wiring 规则。
- `revision` 是 import 侧的缓存键：`Texture` 已经有一个 revision（见 `Texture.hpp`），
  后端按 `(texture, revision)` 上传；produced 侧仍按 target 的 build/plan 状态。

### 3.1 声明侧

- `RenderPass` 增加 `addInput(intrusive_ptr<Texture>)` 与 `addInput(ImageInput)`；`ImageInput` 是**声明**
  值，持有 `intrusive_ptr<ImageRef>` 或 `intrusive_ptr<Texture>`，不是已解析形状。
- `PassDesc::inputs` 是 `std::vector<ImageInput>`（一个序列保留 produced / imported 混排的声明顺序）；
  `input_targets` / `input_names` 的旧路径不变。
- `addInputName` 仍只解析 produced（名字注册表是 pass 产出的表）；imported 图必须对象声明。

### 3.2 解析侧

- `RenderEngine::resolveRowInputs` 产出 `std::vector<ImageSource>`：`ImageInput::image()` 经
  `resolveDeclaredImage` 变成 `ProducedImage` / `ProducedTarget`，`ImageInput::texture()` 变成
  `ImportedTexture`（带 `Texture::revision()`）。
- `FrameDescription::Pass::inputs` 从 `std::vector<raw_ptr<RenderTarget>>` 改为
  `std::vector<ImageSource>`；`RenderFrameExecutor` 的借用规则不变（`ImageSource` 里的指针仍是借用的）。
- `RenderPass::resolveInputTextures` 的签名改为 `std::span<const ImageSource>`；`ScreenPass` 只取第一个
  `isProduced()` 的 source。这是一次**契约改动**，所有 pass 子类与测试替身一起改。
- `CompiledInput` 增加 `kind` / `texture` / `texture_revision`；produced 的字段保持原样。后端按 kind
  选择 `target` 的 attachment view 或 `Texture` 的上传结果；`FrameCompiler` 只为 produced 输入加依赖边。

以上形状对应实现里的 `ImageSource::Kind::{None, ProducedImage, ProducedTarget, ImportedTexture}`；
`None` 是“本帧没产出”的条目，保留在列表里占位。

### 3.3 后端侧

- vsg 后端用 `MaterialImages`（材质贴图缓存，键为 `(Texture*, revision)`）上传 imported 图；
  `ContentPass` 的 sampled input set 按声明序绑定 `InputImages::colors`，所以 imported 图就是下一个
  colour sampler。
- `recordContent` 为 `ImportedTexture` 条目调用 `MaterialImages::acquire`，把 view 填入该条目的
  `InputImages`；`color_attachments = 1`、`depth_sampleable = false`。上传失败时绑白图并报告原因。
- 采样器暂用输入 set 的 `inputSampler()`（与现有 pass 输入一致）；`Texture` 没有 sampler state，等
  sampler 成为真实需求再加。
- **全屏 ABI 的 imported 绑定（2026-10-19 落地）**：source 的颜色附件占 0..C-1，depth 跟随（跳过文本自己
  声明的 binding）；imported 图按输入声明序填程序**已经声明的其余 sampler binding**（升序）。数量对不上由
  backend 报 refusal，不静默绑错图。第一切片只支持 `Texture::Kind::D2`；Cube 作为输入需要程序声明 cube
  sampler，等真实需求再加。
- **produced 输入的 attachment 仍是选择符**：vsg 绑定层今天给 produced 输入整个 target（binding N =
  attachment N，全屏 ABI 的规则），`ImageSource::attachment` / `depth` 还没有收窄绑定集合；真正按
  attachment 绑定是后端能力项，等程序需要时再加。

## 4. 备选方案与否决理由

| 方案 | 形状 | 否决/保留理由 |
|---|---|---|
| A. 两个平行列表 | `inputs` + `texture_inputs` | 不能保留 produced/imported 的混排顺序；消费方要遍历两次，容易把顺序弄丢。**只作为过渡切片**。 |
| B. `ImageRef` 增加 `bind(Texture)` | `ImageRef` 变成两种生命周期的联合体 | `ImageRef` 的 wiring 规则（单 producer、绑定 target、取消绑定）对 Texture 不成立；会逼出大量 `if (texture)` 分支。**否**。 |
| C. `ImageSource` 值（本提案） | 声明对象各自独立，解析后统一 | 保留两种生命周期规则，只统一“输入图”的已解析形状。**推荐**。 |
| D. 继承根 `ImageSource` | `Texture : ImageSource`, `RenderTargetImage : ImageSource` | 公开面多一层继承、多一堆虚函数；生命周期仍不统一，收益只是语法。**否**。 |
| E. `std::variant` 直接进 `FrameDescription` | 后端 `std::visit` | 变体在 ABI 边界上不可 dump、不可稳定比较；`ImageSource` 值是更窄的词汇。**否**。 |

## 5. 迁移切片（每片独立验收）

1. **声明侧（已落地）**：`RenderPass::addInput(Texture)` + `ImageInput` + `PassDesc::inputs` 序列；
   未解析前仍走现有 `inputs` 路径。判据：declaration-only 测试，现有 pass 行为不变。
2. **解析侧（已落地，契约改动）**：`FrameDescription::Pass::inputs` 改为 `ImageSource`；engine / `RenderPass` /
   `ScreenPass` / 测试替身一起改。判据：现有 produced 输入的像素用例全绿。
3. **后端绑定（内容与全屏路径均已落地）**：`CompiledInput` 加 imported 分支，复用 `MaterialImages`；
   内容 pass 按声明序绑定，全屏 pass 在 source 绑定之后的 declared sampler binding 上绑定；两条路径都有像素用例。
4. **便利层（未做）**：`PassDesc` 之外的便捷入口；材质贴图与 pass 输入已经共用 `MaterialImages`。

## 6. 判据与风险

- **判据**：结构测试（声明顺序、produced/imported 混排、未绑定/未上传的拒绝）+ 像素测试（一张 D2 贴图被
  content pass 采样到目标上）+ revision 缓存键（`(Texture*, revision)` 已进 `CompiledInput`，下一帧重上传由
  缓存负责）。
- **风险**：`FrameDescription::Pass::inputs` 是公开 ABI；这次改动会动 vsg 后端与 `test_vsg` 的测试替身。
- **不做**：`Sampler` 抽象、Cube/array 输入、把 `Texture` 纳入 producer 规则、把 `RenderTarget` 改名。

## 7. 落地记录（2026-10-19）

- 公开头：`ImageSource.hpp`（`ImageInput` 声明值 + `ImageSource` 已解析值）、`RenderPass::addInput(Texture)`
  / `addInput(ImageInput)`、`inputs()` 返回 `std::vector<ImageInput>`、`resolveInputTextures(std::span<const
  ImageSource>)`；`PassDesc::inputs`、`FrameDescription::Pass::inputs`、`singlePassFrame`、
  `RenderEngine::PlanRow::inputs` 同步。
- 引擎：`resolveRowInputs` 产出 produced / imported 两种 source；`collectPlanProblems` 只对 produced 输入
  跑 wiring 规则；ScreenPass 的 imported 输入不再被引擎拒绝。
- 后端：`FrameRecorder::InputRef` / `FrameCompiler::CompiledInput` 携带 `ImageSource` 的 kind / texture /
  revision；依赖边只为 produced 输入建；`VsgBackend::recordContent` 对 imported 输入调用
  `MaterialImages::acquire`，把 view 填进该条目的 `InputImages`。全屏路径在
  `ContentPass::recordScreenDraw` 里把 imported 输入按程序声明的 sampler binding 填充。
- 判据：`FrameCompilerTest.AnImportedTextureInputIsAFirstClassInputWithNoProducerEdge`、
  `RenderPipelineBuilderTest.AnImportedTextureIsAnInputInDeclarationOrder`、
  `RenderPipelineBuilderTest.AScreenPassKeepsImportedTextureInputsForItsProgram`、
  `VsgBackendTest.AnImportedTextureInputReachesAContentProgram` 与
  `VsgBackendTest.AnImportedTextureInputReachesAScreenProgram`（两条像素：内容与全屏程序各采样导入贴图）。
