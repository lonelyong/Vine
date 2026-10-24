# graphics 材质：一个 PBR 模型、纹理集、实例

> **状态**：草案（设计已定，未实现；落地后改“现行”）
> **最后核对**：2026-10-24
> **范围**：材质的数据模型、纹理集与采样、实例覆盖、上传去重、forward 与 deferred 的一致性；
> 不含材质 DSL、运行时名字反射、bindless 与 IBL。
> **权威来源**：`Material.hpp` / `ShaderAbi.hpp` / `shaders/vine_params.glsl`（材质块已是 PBR 版；着色仍是迁移期桥）；
> 落地后新增实例类型的头与 `vine_brdf.glsl` BRDF 片段。
> **待办**：`../backlog/graphics.md`（M1）

## 1. 概要设计

### 1.1 问题与背景

- 今天 `vn::graphics::Material` 是“平铺 Phong + 一张贴图”：ambient / diffuse / specular / shininess，
  材质 alpha 只运输不被读；导入的 PBR 资源在 `ModelScene.cpp` 的 `materialFrom` 里被近似成 Phong。
- 没有实例：逐对象高亮 / tint 只能把整个材质换掉（`ModelRenderView::selectAt` 克隆一份），
  共享材质上的改动会同时改掉所有使用者。
- backlog T16 的两个触发条件都已出现：真实 PBR 需求（导入资源带 metallic/roughness），
  逐对象参数出现第二个调用点（model_viewer 的高亮、test_plugin 的动画色）。

### 1.2 目标

- 一个着色模型：metallic-roughness PBR；Phong 只存在于导入边界。
- 资产 / 实例两级：纹理与因子只有一份，实例只覆盖它改的那几项。
- 纹理集：五个语义槽，每槽带采样状态与色彩空间契约。
- forward 与 deferred 读同一份材质字节、跑同一份 BRDF。
- 透明度只有一个模型：模式在材质、幅度在 drawable，排序与着色读同一对输入。

### 1.3 非目标

- 不做材质 DSL、运行时名字反射（`graphics-shader-params.md` 的 S2/S3 仍是独立触发项）。
- 不做 bindless / 纹理数组 / 描述符索引。
- 不做逐材质 program：程序解析仍是“图元 > 几何默认 > 最近 StateNode > pass”。
- 不做第二套 UV，不做切线顶点通道（法线贴图用屏幕空间导数建切线基）。
- 不做 IBL / 环境探针：输出仍是解析光 + ambient。

### 1.4 一句话结论

> 材质是一份 PBR 因子 + 一个纹理集；实例是它上面的稀疏覆盖；两条路径读同一份字节、跑同一份 BRDF。

## 2. 设计要点

2.1 **一个着色模型**（`G1` `G6`）：metallic-roughness（glTF 2.0 核心词汇：base color / metallic / roughness /
emissive / occlusion / normal）。Phong 不再是引擎模型；`vn::geometry::PhongMaterial` 在导入边界按固定表转成
PBR，引擎内部只有一个语义。两条路径、导入器与测试只需对齐一套定义，代价是旧 setter 消失（见 §9.3）。

2.2 **资产与实例是两种类型**（`G1` `G4`）：`Material` 是资产——可共享、可编辑，改动传播给所有未覆盖它的
实例；`MaterialInstance` 引用一个资产并只覆盖自己改的项。二者共同基类 `MaterialBase`，
`RenderCommand` / `Geometry` / `Primitive` / `StateNode` 持有 `MaterialBasePtr`，所以“一次 draw 能被什么
着色”只有一个类型。否决“单类 + 可选父”（见 D2）：那让“改资产会/不会影响谁”成为运行期数据而不是类型，
且实例无法回答“我覆盖了哪几项”。

2.3 **覆盖是稀疏且显式的**（`G4` `G8`）：实例的每一项是 optional，`clearOverride(x)` 恢复继承；解析结果
缓存在实例里，键是（父 revision，自身 revision）。一次解析、多次读取，未覆盖的项不复制存储。

2.4 **纹理集是五个语义槽**（`G1` `G9`）：base_color / metallic_roughness / normal / occlusion / emissive，
每槽 =（`Texture` + `Sampler` + 是否存在）。槽的身份决定绑定与色彩空间，不允许宿主自定义名字。只有
base_color 允许 Cube（沿用“采样器种类跟几何 texcoord 宽度”的规则）；其余四槽只收 D2。UV 集固定为 0，
第二个 UV 是几何格式变更，不是材质变更。

2.5 **变体是“有没有纹理集”，不是“有哪几个槽”**（`G10`）：`VINE_TEXTURED` 一个布尔位。有任意一槽时程序
声明全部五个采样器，缺的槽绑各自类型的常量回退（base 白、MR 白、normal 平直、AO 白、emissive 黑），块里的
flags 逐槽短路。逐槽位会得到 2^5 组合 × 两条路径 × 阴影变体，撑爆管线缓存与 `vine_shader_check.sh` 的
组合门禁；二元位保住今天“无纹理内容零采样器”的快路径。

2.6 **一个 BRDF，两条路径**（`G1` `G5`）：`shaders/vine_brdf.glsl` 是 GGX / Smith / Schlick 的唯一实现：
forward 直接调用；deferred 先把 G-buffer 还原成同样的输入再调用。`vineEvaluateLight` 继续是灯的唯一实现。

2.7 **透明度：模式在材质，幅度在 drawable**（`G1` `G4`）：`AlphaMode::Opaque | Mask | Blend`；有效 alpha =
`draw.params.x`（drawable opacity）× `material.base_color.a` × 可选 base 贴图 alpha。
`RenderCommand::isTransparent()` 改为“opacity < 1 **或** 材质是 Blend”；Mask 在着色阶段按 cutoff discard，
不参与排序，留在不透明 pass。这取代旧的“材质不贡献 alpha”规则——旧规则是为了避免“排序用 opacity、
着色用材质 alpha”的分裂；新规则让排序与着色读同一对输入，并在文档里写死。Blend 内容走 forward overlay，
deferred 的 G-buffer 只收不透明与 Mask。

2.8 **G-buffer 是材质的投影**（`G10`）：四张附件恰好装下逐像素量——A0 albedo.rgb（a 写 1）、
A1 八面体 normal.rg + roughness + metallic、A2 emissive.rgb + occlusion、A3 view position.xyz + 写入掩码。
八面体把 3 分量法线压进 2 个通道；A2 从 RGBA8 升到 RGBA16F，因为 emissive 是 HDR。

2.9 **色彩空间在槽上**（`G4`）：base_color / emissive 必须是 sRGB 格式（硬件解码），
metallic_roughness / normal / occlusion 必须是线性格式；槽与格式不符按 episode 报一次，不做隐式 gamma。
沿用 `Colorf` 线性光的全局约定。

2.10 **材质不带程序**（`G5`）：程序解析保持现状；材质只给参数与纹理集。用户程序读同一份
`VineMaterialBlock`，不引入第二套材质接口。

2.11 **上传按身份 + 复合 revision**（`G10`）：材质 arena 仍按对象地址 + revision 去重（`block_bytes` 仍是
64 B，见 F4）；实例的 revision = 父 revision 与自身 revision 的组合，父资产改动自动让所有实例重写一次。
实例不复制纹理，也不复制块以外的字节。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | `Material` 资产 | PBR 因子 + 纹理集 + alpha 模式；setter 只移动 revision | 已做 |
| F2 | `MaterialInstance` | 引用一个资产，稀疏覆盖因子 / 纹理 / alpha | 未做 |
| F3 | 纹理集 | 五个语义槽 + 每槽 `Sampler` + kind/格式校验 | 已做（格式校验随 F11） |
| F4 | 材质块 v2 | 仍 64 B：base_color / emissive / params / flags | 已做 |
| F5 | forward PBR | GGX 直接光 + ambient，含 normal/ORM/emissive 槽 | 未做 |
| F6 | deferred PBR | G-buffer 还原 + 同一 BRDF + emissive/occlusion | 未做 |
| F7 | alpha 模式 | Opaque / Mask / Blend 的排序、discard 与输出 alpha | 未做 |
| F8 | 导入转换 | asset `Color/Phong/Pbr` → 引擎 PBR，去掉近似 | 已做 |
| F9 | 变体 | `VINE_TEXTURED` 二元位 + 常量回退 | 已做 |
| F10 | 上传去重 | 实例复合 revision；改资产使实例重写 | 未做 |
| F11 | 诊断 | 缺槽 / 格式不符 / Cube 进非 base 槽 / 无效覆盖 | 未做 |

### 3.2 F1–F2 资产与实例

- **行为**：`Material` 的字段 = base_color（线性 RGBA）、emissive（RGB + 强度）、metallic、roughness、
  occlusion、alpha_mode、alpha_cutoff、五个纹理槽。`MaterialInstance` 的字段 = 一个 `MaterialPtr` +
  同样字段的 optional 版本 + 纹理槽 optional 版本。
- **约束**（C1）：实例只有一个父，父不能是实例（不链式）；`G1` 一个定义。
- **要求**（R1）：解析结果 = 父值逐项被覆盖替换，得到一份与 `Material` 同形的
  `MaterialParameters`；解析缓存随父 revision 失效。
- **验收**（T1）：父改基色、实例覆盖粗糙度——实例只剩两者的组合，未覆盖项跟随父。
- **注意**（N1）：覆盖项不能持有第二份纹理引用而不进 revision；`setTexture(slot, ...)` 与
  `clearTexture(slot)` 都动 revision。

### 3.3 F3 纹理集

- **行为**：`enum class TextureSlot`（五槽）+ `TextureSlotBinding { TexturePtr texture; Sampler sampler{}; }`；绑定固定
  （content set 0：material 0，五槽 1–5，lights 6，shadow 7/8）。
- **行为**：`metallic_roughness` 按 glTF 打包（G = roughness，B = metallic），`occlusion` 取 R。
- **约束**（C2）：槽的 kind 与格式按 §2.9；不合法时按 F11 报一次并**不**静默替换成回退图（回退只用于
  “槽不存在”，不用于“槽错了”）。
- **要求**（R2）：每个槽的 `Sampler` 进 `MaterialImages::acquire(texture, sampler, why)` 的按值缓存，
  相同状态共享采样器对象。
- **验收**（T2）：同一贴图两个不同 wrap 的槽各绑一个 sampler 但只上传一次。
- **注意**（N2）：`Sampler` 含 NaN 会每帧 miss 缓存（`Sampler.hpp` 已写），材质不做数值清洗。

### 3.4 F4–F6 块与两条路径

- **行为**：块仍是 64 B，`static_assert` 钉偏移：

  | 偏移 | 成员 | 内容 |
  |---|---|---|
  | 0 | `base_color` | rgb 线性 + a = 材质 alpha |
  | 16 | `emissive` | rgb + a = 强度 |
  | 32 | `params` | x metallic, y roughness, z occlusion, w alpha_cutoff |
  | 48 | `flags` | `uvec4`：bit0–4 = 槽存在；bit8–9 = alpha 模式 |

- **约束**（C3）：块由 `blockOfMaterial` 一处打包；实例在打包前解析（R1）。
- **要求**（R3）：forward 与 deferred 对同一份块得出同一画面；BRDF 与灯求值各只有一份 GLSL。
- **验收**（T3）：同材质、同灯、同相机下 forward 与 deferred 的同像素差 ≤ 量化误差（读回比较）。
- **注意**（N3）：G-buffer 是 HDR，A2 的 8-bit 会把 emissive 截断（§2.8）。

### 3.5 F7 alpha 模式

- **行为**：Opaque 忽略 alpha；Mask 在 base 采样后 `discard`；Blend 输出 a 并进透明队列。排序键
  = opacity < 1 或模式为 Blend（§2.7）。
- **约束**（C4）：Mask 不能进透明队列（否则排序把本该不透明的写进 overlay，失去自遮挡）。
- **要求**（R4）：`draw.params.x` 仍是唯一逐 drawable 幅度；材质不复制它。
- **验收**（T4）：Mask 叶子在 cutoff 两侧正确 discard；Blend 叶子按 opacity × material alpha 混合。
- **注意**（N4）：deferred 的 G-buffer 不写 Blend 内容；它只能出现在 forward overlay。

### 3.6 F8 导入转换

- **行为**：`ColorMaterial` → base=color、metallic 0、roughness 1；`PhongMaterial` → base=diffuse、
  `roughness = sqrt(2 / (shininess + 2))`（Blinn-Phong 到 GGX 的标准映射）、specular 折算进 metallic；
  `PbrMaterial` → 逐字段，不再近似。
- **规格**：`metallic = clamp(max(specular.rgb) × clamp(specular.a, 0, 1), 0, 1)`——metallic-roughness 没有单独反射率旋钮，强或带色的 specular 正是“金属”含义（最强通道 = 反射峰值，alpha = 强度）；代价见 D7。
- **约束**（C5）：转换只发生在 `ModelScene.cpp` 一处；引擎侧不出现“源是 Phong”的分支。
- **验收**（T5）：导入一个 glTF PBR 资产，base/metallic/roughness/emissive 与源文件逐字段一致。

### 3.7 F9–F11 变体、上传与诊断

- **行为**：`ProgramVariant` 增加 `textured`（任一槽存在）；`diffuse_map` 位被 `textured` 取代。
  实例的复合 revision 让 arena 重写；诊断走 `DiagnosticsRoute` 的 episode 规则。
- **约束**（C6）：`vine_shader_check.sh` 的组合数 = 现有位 × 1，不随槽数增长；
  `ProgramAbi` 扫描到的五个采样器声明随 `VINE_TEXTURED` 出现。
- **验收**（T6）：`EmbeddedShadersTest` 的变体矩阵与 `ProgramAbiTest` 的绑定计数同步更新。

## 4. 公开接口

### 4.1 `MaterialBase` / `Material` / `MaterialInstance`

| 接口 | 契约 |
|---|---|
| `Material::setBaseColor` / `setEmissive` / `setMetallic` / `setRoughness` / `setOcclusion` | 写入并移动 revision；值相等不动 |
| `Material::setAlphaMode` / `setAlphaCutoff` | 模式与 cutoff；只影响 Mask/Blend |
| `Material::setTexture(slot, texture, sampler)` / `clearTexture(slot)` / `texture(slot)` | 纹理集 |
| `MaterialInstance::parent()` / `overrideMask()` / `clearOverride(item)` | 稀疏覆盖 |
| `MaterialBase::revision()` / `name()` / `resolved()` | 资产的自身 revision；实例的复合 revision 与解析值 |
| `MaterialBasePtr` | `RenderCommand` / `Geometry` / `Primitive` / `StateNode` 持有的统一类型 |

### 4.2 着色侧

| 接口 | 契约 |
|---|---|
| `VineMaterialBlock`（64 B） | 见 F4 的偏移表；`operator==` 保持默认 |
| `vine_brdf.glsl` | `vineEvaluateBRDF(base, metallic, roughness, N, V, L, radiance)` 一处实现 |
| `VINE_TEXTURED` | 声明五槽采样器；缺槽绑常量回退 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 实例只有一个父且父不能是实例；解析是一份 `MaterialParameters`。 |
| C2 | 纹理槽的 kind / 格式契约（§2.9）由材质校验，错误不回落成回退图。 |
| C3 | 块只由 `blockOfMaterial` 打包，实例先解析。 |
| C4 | Mask 不进透明队列；Blend 只走 forward。 |
| C5 | 导入转换只在 `ModelScene.cpp` 一处。 |
| C6 | 变体数量不随纹理槽数量增长。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | 实例解析缓存以（父 revision，自身 revision）为键，父改动自动失效。 |
| R2 | 采样器按 `Sampler` 值缓存；同状态共享一个 sampler 对象。 |
| R3 | forward 与 deferred 共用一份 BRDF 与一份灯求值。 |
| R4 | 逐 drawable 的透明度幅度只有 `draw.params.x` 一个来源。 |

### 5.3 不变量（N#）

| # | 注意项 |
|---|---|
| N1 | 覆盖项与纹理槽都必须进 revision，否则实例改动不上传。 |
| N2 | `Sampler` 含 NaN 会破坏按值缓存；材质不做数值清洗。 |
| N3 | G-buffer 的颜色附件是 HDR；8-bit 会截断 emissive。 |
| N4 | deferred 只收 Opaque / Mask；Blend 只能出现在 forward overlay。 |
| N5 | 材质 alpha 与 drawable opacity 的分工是“模式 / 幅度”，不要互相替代。 |
| N6 | base_color 可以是 Cube，其余四槽只能是 D2；这是 kind 契约，不是可选项。 |

## 6. 注意事项与陷阱

| # | 陷阱 | 会怎么错 | 怎么判 | 怎么防 |
|---|---|---|---|---|
| P1 | 材质 alpha 与 drawable opacity 双写 | 宿主把材质 alpha 当不透明度，排序与着色各读一个 | forward 里透、deferred 里不透，或透明队列里出现 alpha = 1 的对象 | §2.7 的公式是唯一入口，`isTransparent()` 同时读两个输入 |
| P2 | 逐槽变体 | 五个槽各一个 define，组合数 32×，管线缓存与门禁矩阵同时膨胀 | `ProgramVariant::bits()` 的有效组合数与槽数同阶增长 | C6；只有 `VINE_TEXTURED` 一个位 |
| P3 | 实例 revision 不跟随父 | 改资产后实例仍画旧值，且没有日志 | `resolved()` 的缓存键漏掉父 revision | R1；测试钉住“父改 → 实例重写一次” |
| P4 | 法线贴图没有切线 | 切线基错，法线贴图方向整体旋转，光照看起来“歪” | 平面法线贴图应当不改变外观；改变即基错 | 屏幕空间导数构基（位置与 UV 的 dFdx/dFdy），GLSL 一处实现 |

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 保留 Phong、只加实例 | 真实 PBR 需求已经出现，Phong 无法表达 metallic/roughness；见 [ADR](../decisions/ADR-2026-10-24-material-model.md)。 |
| D2 | 单类 + 可选父节点（不做 `MaterialInstance` 类型） | 覆盖语义成为运行期数据，宿主无法回答“我改了哪几项”；`G1` `G4`。 |
| D3 | 逐槽变体位 | 2^5 组合 × 路径 × 阴影；`G10`。 |
| D4 | 只用 draw block 的用户槽做 tint，不做实例 | 没有纹理/alpha 覆盖能力，第二个调用点之后仍会克隆材质；`G8` 反向证据。 |
| D5 | bindless / 描述符索引 | 今天没有消费者；`G8`。 |
| D6 | 名字表作为材质核心（`set("BaseColor", ...)`） | 运行期查找、无类型安全；S2/S3 保持独立；`G4` `G8`。 |
| D7 | 把 Phong 的 specular 当 F0 保留（Blender / Godot 的做法） | 需要给材质与块再加一个反射率输入，而引擎今天只有一条反射路径；折进 metallic 的代价（典型 `Ks 0.5` 会变半金属）由 F8 的固定表承担；`G8`。 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 实例解析：覆盖项与继承项正确组合 | `MaterialInstanceTest.*`（新增） |
| T2 | 采样器按值共享、贴图只上传一次 | `MaterialImagesTest.SamplerByValue*` |
| T3 | forward 与 deferred 同像素一致 | `VsgBackendTest.ForwardAndDeferredAgreeOnOneMaterial` |
| T4 | Mask discard 与 Blend 排序 | `GraphicsTest.*AlphaMode*` |
| T5 | 导入逐字段一致 | `ModelSceneTest.PbrMaterialIsNotApproximated` |
| T6 | 变体矩阵与绑定计数同步 | `EmbeddedShadersTest` / `ProgramAbiTest` |

## 9. 状态与演进

### 9.1 当前状态

**M1（模型 + 块 + 导入）与 M2（纹理集 + 二元变体）已落地**（2026-10-24）：`Material` 是 PBR 因子 + 五个纹理槽
（每槽 = 贴图 + `Sampler`，槽身份定绑定与色彩空间），块是 64 B 的 `base_color / emissive / params / flags`
（flags 的 bit0–4 逐槽存在），`materialFrom` 的固定表就位、`PbrMaterial` 逐字段直传；内容集绑定固定为
material 0 / 槽 1–5 / lights 6 / shadow 7–8，`VINE_TEXTURED` 一个位声明全部五槽。
着色仍是迁移期桥：两路径的 Blinn-Phong 项由 `vineMaterialShininess` / `vineMaterialF0` 导出，M2 只让 base 槽参与
画面；MR / normal / occlusion / emissive 与 alpha 模式只进块与绑定，缺槽统一绑白 —— M3 读它们时按槽给常量（emissive
黑、normal 平直）并删除桥。

### 9.2 已知缺口 / 待办

- 落地分四步：M1 模型与块（F1/F4/F8）与 M2 纹理集与变体（F3/F9）已做；M3 两条路径的 BRDF 与 G-buffer（F5/F6）；M4 实例与 alpha 模式（F2/F7/F10/F11）。
- 镜像 UV 下的导数切线基是已知近似，不是 bug；需要时再引入切线通道。
- IBL、SSAO、透射（KHR_materials_transmission）不在本设计内。

### 9.3 兼容性 / 迁移

- `setDiffuse` → `setBaseColor`，`setAmbient` / `setSpecular` / `setShininess` 删除；`setTexture(t)` → `setTexture(TextureSlot::BaseColor, t, {})`，`texture()` → `texture(slot)`。
- `MaterialPtr` 在持有处改为 `MaterialBasePtr`；`vn::geometry` 侧不变。落地时同步修改 `graphics-scene.md` F7、`graphics-rendering.md` F23/N6、`graphics-shader-params.md` 的 material 块行与 §2.2、`vsg-backend.md` 的变体与上传小节。

## 10. 参考

### 10.1 代码

- `src/viz/graphics/sdk/vine/graphics/Material.hpp` · `ShaderAbi.hpp` · `Sampler.hpp` · `Texture.hpp`
- `shaders/vine_params.glsl` · `builtin_forward.frag` · `builtin_gbuffer.frag` · `builtin_deferred_lighting.frag`
- `ContentSources.cpp`（`blockOfMaterial`）· `ProgramVariant.hpp` · `MaterialImages.hpp` · `ModelScene.cpp`（`materialFrom`）

### 10.2 文档与 ADR

- `graphics-shader-params.md` · `graphics-rendering.md` · `graphics-scene.md` · `graphics-content-lifecycle.md`
- [ADR-2026-10-24-material-model](../decisions/ADR-2026-10-24-material-model.md)

### 10.3 外部资料

- glTF 2.0 的 material（metallic-roughness、textureInfo 的 texCoord/sampler）；Filament 的材质/实例分层、采样器参数与 Cook-Torrance GGX 的 Smith/Schlick 形式。
