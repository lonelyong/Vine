# 图源统一：`ImageRef` / `Texture` 与 pass 输入（2026-10-19）

> **状态：提案，未落地。** 本文只定形状与切片，不改公开头。
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

- `RenderPass` 增加 `addInput(intrusive_ptr<Texture>)`（名字可以是 `addTextureInput`，但 `addInput` 重载更贴近
  “一张输入图”的语义；两个重载在调用点可区分）。
- `PassDesc` 增加 `std::vector<intrusive_ptr<Texture>> texture_inputs`，或者把输入声明收成一个
  `std::vector<PassInputDecl>`（`std::variant<intrusive_ptr<ImageRef>, intrusive_ptr<Texture>>`）。
  **推荐后者**：一个序列才能保留“produced / imported 混排”的声明顺序；两个 vector 只能表达“所有 produced
  在前、所有 imported 在后”。
- `addInputName` 仍只解析 produced（名字注册表是 pass 产出的表）；imported 图必须对象声明。

### 3.2 解析侧

- `RenderEngine::resolveRowInputs` 产出 `std::vector<ImageSource>`。
- `FrameDescription::Pass::inputs` 从 `std::vector<raw_ptr<RenderTarget>>` 改为
  `std::vector<ImageSource>`；`RenderFrameExecutor` 的借用规则不变（`ImageSource` 里的指针仍是借用的）。
- `RenderPass::resolveInputTextures` 的签名改为 `std::span<const ImageSource>`（或保持 vector 引用）。
  这是一次**契约改动**，所有 pass 子类与测试替身一起改。
- `CompiledInput` 增加 `kind` 与 `texture`/`revision`；produced 的字段保持原样。`sampled` 绑定层按 kind
  选择 `target` 的 attachment view 或 `Texture` 的上传结果。

### 3.3 后端侧

- vsg 后端已有 `TextureCache`（材质贴图）与 `SampledInput`（pass 输入）两条路径；统一后只需让
  `SampledInput` 接受 imported 图，复用 `TextureCache` 的上传与 revision 失效。
- `ScreenPass` 的 `source` 仍是一个 target：它是“全屏三角形的主输入”，不是任意贴图。额外的 imported 图
  走输入列表，由程序自己声明 sampler slot；`ScreenPass` 不猜 sampler 类型（D2 vs Cube）。
- 第一切片只支持 `Texture::Kind::D2`；Cube 作为输入需要程序声明 cube sampler，等真实需求再加。

## 4. 备选方案与否决理由

| 方案 | 形状 | 否决/保留理由 |
|---|---|---|
| A. 两个平行列表 | `inputs` + `texture_inputs` | 不能保留 produced/imported 的混排顺序；消费方要遍历两次，容易把顺序弄丢。**只作为过渡切片**。 |
| B. `ImageRef` 增加 `bind(Texture)` | `ImageRef` 变成两种生命周期的联合体 | `ImageRef` 的 wiring 规则（单 producer、绑定 target、取消绑定）对 Texture 不成立；会逼出大量 `if (texture)` 分支。**否**。 |
| C. `ImageSource` 值（本提案） | 声明对象各自独立，解析后统一 | 保留两种生命周期规则，只统一“输入图”的已解析形状。**推荐**。 |
| D. 继承根 `ImageSource` | `Texture : ImageSource`, `RenderTargetImage : ImageSource` | 公开面多一层继承、多一堆虚函数；生命周期仍不统一，收益只是语法。**否**。 |
| E. `std::variant` 直接进 `FrameDescription` | 后端 `std::visit` | 变体在 ABI 边界上不可 dump、不可稳定比较；`ImageSource` 值是更窄的词汇。**否**。 |

## 5. 迁移切片（每片独立验收）

1. **声明侧（非破坏）**：`RenderPass::addInput(Texture)` + `PassDesc` 的输入声明收成 `PassInputDecl` 序列；
   未解析前仍走现有 `inputs` 路径。判据：declaration-only 测试，现有 pass 行为不变。
2. **解析侧（契约改动）**：`FrameDescription::Pass::inputs` 改为 `ImageSource`；engine / `RenderPass` /
   `ScreenPass` / 测试替身一起改。判据：现有 produced 输入的像素用例全绿。
3. **后端绑定**：`CompiledInput` 加 imported 分支，复用 `TextureCache`；新增“static texture 作为 screen pass
   输入”的像素用例；`(texture, revision)` 改动后下一帧重上传。
4. **便利层**：`ScreenPass` / `PipelineOptions` 暴露一个 `texture_inputs` 便捷入口；材质贴图与 pass 输入共用
   同一个上传缓存。

## 6. 判据与风险

- **判据**：结构测试（声明顺序、produced/imported 混排、未绑定/未上传的拒绝）+ 像素测试（一张 D2 贴图被
  screen pass 采样到目标上）+ revision 测试（`Texture::setSource` 后下一帧重上传）。
- **风险**：`FrameDescription::Pass::inputs` 是公开 ABI；这次改动会动 vsg 后端与 `test_vsg` 的测试替身。
  切片 2 单独提交，避免和“声明侧”混在一起。
- **不做**：`Sampler` 抽象、Cube/array 输入、把 `Texture` 纳入 producer 规则、把 `RenderTarget` 改名。
