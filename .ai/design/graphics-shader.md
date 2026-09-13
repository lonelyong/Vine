# Graphics 可编程着色设计（用户写 GLSL / ShaderProgram）

> 状态：设计稿 v1（2026-09-03），评审对象。
> 上游/前身：`vine-shader.md`（后端 vsg 自写内置 shader 的 P0 落地稿；其 §11 为本文前身，以本文为准）。
> 关联：`graphics-scene-graph.md`（program 挂点）、`graphics-state.md`（状态参与变体键）、
> `graphics-render-pipeline.md`（pass 级 program + 命名产出槽）、`vsg-custom-shader.md`。
>
> **一句话**：SDK 第一准则——**用户必须能写 GLSL**；SDK 只给一个**薄着色接口**（源 + 类型化参数
> + 纹理/输入槽），不搞重型"契约"；内置 `ShaderPreset` 与用户 Program 是**同一模型**；vsg（乃至
> 自写 Vulkan/GL）只是把 Program 编译并装配成管线的可替换实现。
>
> ⚠ **编译能力（更新 2026-09-03）**：vsg 已**集成 glslang**——gfx_backend_vsg 在 VINE_USE_FETCHCONTENT
> 分支**源码构建 vsg** 并链系统 glslang-dev（`VSG_SUPPORTS_ShaderCompiler 1`，`ShaderCompiler.cpp` 已
> 编译），**运行期 GLSL→SPIR-V 可用**。因此 SDK 支持两条编译路径：
> - (a) **运行期**：后端 `vsg::ShaderCompiler` 直接把用户 GLSL 编成 SPIR-V（无外部工具依赖）；
> - (b) **离线/作者时**：`glslangValidator` 或 SDK 薄辅助 `compileGlslToSpirv()` 预编译（测试/工具链）。
> `ShaderProgram` 也可直接携带 SPIR-V（字节或 `.spv` 路径）。后端以 `ShaderSet.stages /
> attributeBindings / descriptorBindings / defaultGraphicsPipelineStates` **自描述装配**用户
> Program 并经 `GraphicsPipelineConfigurator` 成管线；`program()==nullptr` → 内置默认
> （ShaderPreset / vendored SPIR-V），零回归。依赖注记：无 glslang 的本地 vsg 安装仅在
> VINE_USE_FETCHCONTENT=OFF 分支使用（无运行期编译）。详见 §6/§7/§10。
>
> 📋 评审（2026-09-03）：P1 SDK 侧已落地——`ShaderProgram/ShaderStage`、`Geometry::setProgram`、
> `StateNode::setProgram`、`effectiveProgram`、`RenderCommand.program`（GraphicsTest 全绿）；
> 后端"编译半环"已验（`vsg::ShaderCompiler`，GlslCompileTest）且"自定义 ShaderSet 装配"经
> `vsg_color_probe custom` 探针在 lavapipe 验证（已入回归脚本）。**未接**：SceneBridge 遇
> `cmd.program` 建 ShaderSet 并喂视图/模型矩阵——卡点：vsg 内置 phong ShaderSet 是**序列化 blob**
> （`shaders/phong_ShaderSet.cpp` = io.read_cast 数据，无文本可镜像），且 vsg 内核/ViewDependentState
> 未暴露"模型矩阵/视图矩阵"的标准注入名；接线前需先逆向出 phong 的 push/descriptor 契约（用
> vsg_shader_dump 打印其 stages/attributeBindings/descriptorBindings/pushConstantRanges），勿盲改。

## 0. 为什么必须（多 pass 的意义）

多 pass 的价值 = 每个 pass 的着色逻辑可编程。若用户不能写 shader，ScreenPass 只能跑引擎写死的
全屏拷贝，`publish/resolve` 退化为"拷来拷去"，deferred/SSAO/泛光/自定义合成全做不了。
因此"用户写 GLSL"是**管线架构需求**，不是附加功能。

## 1. 薄接口（刻意不做成重型契约）

真正不可省只有一句：**shader 要有一个确定的输入接口**（矩阵、顶点属性、参数、pass 纹理怎么进）。
这跟后端无关，任何可编程渲染都有；但**不必**形式化为 location 表 + 布局推导 + 声明式块抽象——
那是"跨任意后端一次编写"才需要的。未来后端只有 vsg 与手写 Vulkan（同为 SPIR-V），用户 GLSL 两边
都能跑，因此接口可以极薄：

```cpp
// SDK（后端无关）：ShaderStage + 参数 + 槽
class ShaderStage { ShaderStageType type; String source; String entry; };  // VS/FS；compute 留缝
class ShaderProgram : public Object, public RefCounted<ShaderProgram> {
  String name();
  void addStage(ShaderStage);                 // 或 setVertexSource/setFragmentSource
  void addParam(String name, ParamType type); // float/vec2..4/mat4/int（布局后端推）
  void setParam(String name, const ParamValue&);
  void addTextureSlot(String name);           // pass 命名产出 或 纹理资产（P1）
  // 需要哪些顶点属性（按 loc）也声明在这里
};
```

**默认好用的关键**：`program()==nullptr` → 走引擎默认（内置程序由 ShaderPreset+材质+几何数据决定），
一行 GLSL 不碰、零回归。

## 2. 挂点与解析链

- `Geometry::setProgram(...)`（null=默认）——per-object 覆盖（graphics-scene-graph.md）；
- `StateNode::setProgram(...)`——给子树统一设（如"整块无光照"），可选；叶子优先；
- **pass 级**：`ScreenPass`/通用 pass 可挂 Program + 声明输入槽（见 §5）。

解析链：`Geometry.program` 非空用它；否则向上找最近 `StateNode.program`；否则引擎默认内置。

## 3. 最小绑定约定（后端保证，用户对着写）

- **顶点属性**：`loc 0 = position`（唯一定死）；其余 loc 由用户按需声明消费（默认内置程序按
  约定 loc0/1/2 = pos/normal/color，自定义程序读自己绑的 loc）。
- **每视图/每 draw 数据**（对外契约形态，避免 vsg 专属 push 泄漏）：
  - per-view `VineViewBlock{ view; inv_view; proj; view_proj; cam_pos; frame(time/viewport) }`
  - per-draw `VineDrawBlock{ model; …用户参数… }`
  用户据此写 `gl_Position = view_proj * model * pos`。push constant 只允许作**后端内部优化**
  （须与上述契约可证等价），不是用户要写的接口。
- 这些约定由后端用 `ShaderSet`（attributeBindings/descriptorBindings）**自描述**实现，
  SDK 只传"源 + 槽声明"，不复制一份契约文档。

## 4. 参数机制

- 类型化参数表 → 后端推导 std140 布局 → per-drawable UBO（set1）；
  每帧仅当参数变时原地打包（复用 vsg Data/DYNAMIC 机制，MaterialUBO 是它的固定特例）。
- 不提供"按名字运行时 poking"（Vulkan 无此模型，且贵）。

## 5. pass 级 Program + 命名产出槽（多 pass 意义落点）

- 复用现有命名产出 `publish/resolve`：Program 声明消费的槽名（`SceneColor/Depth/ShadowMap/…`），
  engine resolve → 后端绑成 sampler → 输出再 publish。
- `ScreenPass::execute` 由固定全屏拷贝改为"跑用户 Program 的全屏三角形"（v1 先只支持全屏 +
  少量输入槽，不做透视变换）。

## 6. 编译 / 排错 / 热更（能力更新 2026-09-03，见顶部 ⚠）

- **交付形态**：`ShaderProgram` 作者用 **GLSL 源**；后端可**运行期编译**（vsg ShaderCompiler，glslang
  已集成）或**载入预编译 SPIR-V**。
- **编译路径**：
  - (a) 运行期：后端 `vsg::ShaderCompiler` 把 GLSL 直接编 SPIR-V（`VSG_SUPPORTS_ShaderCompiler 1`）；
  - (b) 离线：SDK 薄辅助 `compileGlslToSpirv(source, stage, entry)`——调 `glslangValidator` 子进程
    （工具缺失报清晰错误）；或直接给 SPIR-V（字节/`.spv`）→ `ShaderStage::read` / SPIRV 构造。
- **错误分层**：运行期编译错由 ShaderCompiler 上报；离线工具错由 stderr/exit；装配期由后端上报。
- 热更 = 重编译 / 重载 SPIR-V（P1 后可选）。

## 7. 与内置预设同构

- `ShaderPreset{StandardPhong, FlatShaded, Pbr, ShadowedPhong}` 退化为**内置 Program 的别名**
  （默认快路径，走同一 ShaderProgram 模型）；
- 不搞"内置一套机制、用户一套机制"。

## 8. 后端映射

| SDK | vsg | GL(假想) |
|---|---|---|
| ShaderProgram | ShaderSet(stages/attributeBindings/descriptorBindings) + GraphicsPipelineConfigurator | program + glVertexAttribPointer(uniform 由参数表下发) |
| 参数表 | `vsg::Value<…>`(DYNAMIC) → DescriptorBuffer(set1) | glUniform*（编译期布局） |
| 输入槽 | 采样命名产出的 ImageView → sampler | 纹理单元 |

## 9. 分期

- **P0（后端，vine-shader.md）**：自写内置 StandardPhong/FlatShaded 替换 vendored blob——先用
  vsg 内建/近端路径跑通，**内置 shader 从第一版就按 ShaderProgram 形状组织**，为同构铺路。
- **P1**：SDK `ShaderStage/ShaderProgram/Param` 类型（GLSL 源 + 可选 SPIR-V）+ `compileGlslToSpirv`
  离线辅助 + Geometry/StateNode 挂点（null=默认）；先把 pass 级全屏 shader 打通（SPIR-V 交付）。
- **P2（可选）**：per-drawable 自定义顶点数据 shader（点云等）深化、热更、GL 后端验证。

## 10. 决策记录（2026-09-03）

1. SDK 第一准则：用户必须能写 GLSL；着色契约先于后端（vsg 可弃）。
2. **薄接口**：不做重型契约/布局推导；接口最小化到"源 + 类型化参数 + 槽"。
3. loc0=position 为唯一固定约定；per-view/per-draw 用 `VineViewBlock/VineDrawBlock` 声明式块，push 仅内部。
4. 内置 ShaderPreset 与用户 Program 同一模型；默认 null=内置，用户可覆盖（Geometry/StateNode/pass）。
5. 多 pass 意义 = pass 级 Program + 命名产出槽（复用现有 publish/resolve）。

6. **编译能力（2026-09-03，初稿→更新）**：初判"无 glslang → 仅离线"；随后**集成 glslang**
   （vsg 源码构建 + 系统 glslang-dev → 运行期 `ShaderCompiler` 可用，`VSG_SUPPORTS_ShaderCompiler 1`）。
   最终：**运行期编译可用（默认）**，离线 `glslangValidator` / 直接 SPIR-V 为兜底；作者形态 = GLSL 源。
7. **绑定契约自描述**：用户 Program 声明所需顶点属性（loc0=position 固定）与 uniform/纹理槽；
   后端按 `ShaderSet.attributeBindings/descriptorBindings` 自描述绑定数组与参数，SDK 不复制契约文档。
8. **默认不变**：`program()==nullptr` → 内置（ShaderPreset / vendored SPIR-V），零回归。

## 11. 后端无关着色 ABI：L1/L2/L3（2026-09-13 设计）

> 动机：`ShaderProgram` 统一之后，**换后端（vsg → DX/GL）应只换/生成 shader 文本**，ABI 与着色语义不改。
> 这要求把"角色/块/槽"与"别名的具体编号"分开。P0.A（内建前向着色归 SDK）已落地，见
> `vsg-custom-shader.md` §11.7。

### 11.1 能复用 / 不能复用

| 层 | 归属 | 换后端复用? |
| --- | --- | --- |
| 着色语义（GLSL/HLSL 文本里的**算法**） | **SDK** | ✅ 语义照搬，文本换方言 |
| ABI 契约（属性角色、数据块、参数、槽） | **SDK** | ✅ 契约照搬 |
| 编译（SPIR-V / DXIL） | 后端 | ❌ |
| 绑定编号（set·binding / register·space）、管线/PSO、root signature | 后端 | ❌ |
| 渲染图 / pass / MRT / 深度策略 / 资源绑定 | 后端 | ❌ |

**铁律**：产品 shader 文本里**不出现** `set=` / `binding=` / `register` / `vsg_*`。那些属于每后端的 L2 shim。

### 11.2 三层

| 层 | 内容 | 位置 |
| --- | --- | --- |
| **L1 语义 ABI** | 属性角色表、数据块（`VineViewBlock`/`VineDrawBlock`/`VineMaterialBlock`/`VineLightsBlock`）、参数表、命名槽 | SDK（本文档 + `ShaderAbi.hpp`） |
| **L2 现实化** | 角色 → 该 API 的绑定/语义；块 → push/cbuffer/root constants；块布局 packing 规则；矩阵序、Y 方向、深度约定 | 每后端一份薄 shim（vsg / DX / GL） |
| **L3 着色体** | 同一份算法，GLSL 或 HLSL（写两份或从单源生成） | SDK（`src/viz/graphics/shaders/`）+ 用户 Program |

### 11.3 L1 契约（当前值 = 既有事实，暂不改编号）

**属性角色 → shader location**（`ShaderAbi.hpp`；`vsg_*` 只是 vsg 侧的绑定别名）：

| 角色 | location | 说明 |
| --- | --- | --- |
| Position | 0 | 唯一必需 |
| Normal | 1 | 着色必需（可派生） |
| Color | 2 | 可选（门控） |
| TexCoord0 | 8 | 可选（门控）；8 是保留槽，避开自定义通道 |
| 自定义通道 | = 其**源 location**（>= 3，≠ 8） | 调用者给的通道直接复用为 shader location |

**数据块（语义，机制由后端定）**：per-view `VineViewBlock`（view/inv_view/proj/view_proj/cam_pos/frame）；per-draw `VineDrawBlock`（model + 参数表）；`VineMaterialBlock`；`VineLightsBlock`。
命名约定 **`Vine<Role>Block`**（与既有 `VineLightsBlock`/`LightPushBlock` 一致；`Block` 明示"这是内存布局，不是引擎对象"，与 `FrameContext`/`RenderCommand`/`Camera` 区分）。**GLSL 块类型名与 L1 名逐字相同**（2026-09-13 起：`VineMaterialBlock`/`VineLightsBlock`），从契约到源码无需对照表。
**布局规则：全部 16 字节对齐、成员为 `mat4`/`vec4`** —— 这样 std140 与 D3D cbuffer packing 同时成立（`vec3` 紧跟 `float` 是唯一要避免的坑）。现有 `LightPushBlock`(128B)/`VineLightsBlock`(112B)/`VineMaterialBlock` 已满足；C1 落地的 `VineViewBlock`(288B)/`VineDrawBlock`(80B) 亦然。

**参数表 / 槽表**：Program 声明类型化参数与消费的命名产出槽（`in_SceneColor`…）；布局由后端推导，用户不碰字节。

### 11.4 L2 对照（同一 L1，三种现实化）

| L1 | vsg (Vulkan) | D3D12 | D3D11 / GL |
| --- | --- | --- | --- |
| 属性 location | `addAttributeBinding(name, define, location, …)` + `assignArray` 编号 | InputLayout + HLSL 语义（`POSITION`/`NORMAL`/`COLOR`/`TEXCOORD0`） | 同 D3D12 / `glVertexAttribPointer` |
| 数据块 | push constant / UBO（`VineLightsBlock` 走 s0b2 即此） | root constants / root CBV / cbuffer | cbuffer / uniform |
| 采样槽 | `layout(set,binding)` | `register(tN, spaceN)` | `register(tN)` / 纹理单元 |
| 编译 | glslang → SPIR-V | DXC → DXIL | FXC→DXBC / GLSL |
| 管线 | `ShaderSet` + `GraphicsPipelineConfigurator` | PSO + root signature | PSO / GL program |
| 管线变体 | `ShaderSet` 的 define 变体 | 多份 PSO / 动态状态 | 多 program / `#define` |

### 11.5 现状差距（诚实）

- 已一致：属性 location 表（隐式）、块布局（vec16 对齐）。
- **未做**：SDK shader 文本仍用 `layout(set=…, binding=…)` 与 push `pc`（vsg 形状）；块类型名已对齐（§11.3）；
  `ShaderProgram` 只有 stages，**没有参数表/槽声明**（`addParam`/`addTextureSlot` 不存在）。
- 因此今天换 DX **还不能**"只换文本"：要先把 L1 显式化、把编号从产品 shader 里拿掉。

### 11.6 分期（每步保持两条证据基线 + lavapipe 绿）

| 步 | 内容 | 风险 |
| --- | --- | --- |
| **B1（本次）** | SDK 显式属性 location 表 `ShaderAbi.hpp`；vsg 后端用它替代字面量；测试钉住 L1 value ↔ shader 文本 | 低（行为中性） |
| B2 | `ShaderProgram` 参数表 + 命名槽声明；后端绑成 UBO/sampler | 中 |
| B3 | `VineViewBlock`/`VineDrawBlock` 声明式块（替换 `pc` + 块绑定）；先定"per-draw model 走 UBO vs vsg push 作内部优化"的口径 | 高（动 ABI/预算） |
| B4（可选） | 第二个后端（DX/GL）验证"只换文本 + L2 shim" | 视需求 |

> B2/B3 的口径见 **§12**：B2 暂缓（无消费者），B3 采用**选项 C**（L1 声明式块 + push 作后端内部优化）。

### 11.7 B1：SDK 显式属性 location 表（2026-09-13 落地）

- `sdk/vine/graphics/ShaderAbi.hpp`：`enum class VertexAttribute { Position, Normal, Color, TexCoord0 }` +
  `constexpr std::uint32_t attributeLocation(VertexAttribute)`（值 = 11.3 表）。
- vsg 侧 `buildVineShaderSet` / `assembleProgramShaderSet` 的 `addAttributeBinding(..., location, ...)`
  改用 `attributeLocation(...)`，字面量 0/1/2/8 从后端消失。
- 测试：`test_graphics` 钉 `attributeLocation` 的值，且内置前向着色的嵌入文本确实以这些 location 声明
  （L1 ↔ L3 一致性；改表不改文本会红）。
- 判据：行为中性 —— 两条证据基线 47 行不变；`vine_shader_check` PASS；相关单测 +1；lavapipe PASS。


## 12. B3 决策：per-view / per-draw 数据怎么给（2026-09-13）

> 目的：在动 `VineViewBlock`/`VineDrawBlock` 之前把口径钉死，否则会返工（动 push 预算、动两条证据基线）。

### 12.1 现状（= vsg 形状泄漏在 SDK shader 里的地方）

| 数据 | 现在怎么给 | 谁决定 |
| --- | --- | --- |
| 相机矩阵（per-draw） | vsg 矩阵栈写 push 0..128：`pc{ mat4 projection; mat4 modelView; }` | 后端（SDK shader 只是"知道有 pc"） |
| 光照（per-view） | 前向：`VineLightsBlock` UBO `set0/binding2`；延迟：`LightPushBlock` **push**（全屏不需要矩阵） | 后端 |
| 材质 | `VineMaterialBlock` UBO `set0/binding0` | 后端 |

SDK shader 文本因此写死了 `layout(push_constant)` / `layout(set = 0, binding = N, std140)` —— 这就是 DX 的拦路石。

### 12.2 硬约束

| 约束 | 事实 |
| --- | --- |
| Vulkan push 保证 | **128 B**（本项目实测就是上限） |
| vsg 矩阵栈 | **已占满 0..128**（前向每 drawable 都要投影/模型矩阵） |
| L1 `VineViewBlock`（view / inv_view / proj / view_proj / cam_pos / frame） | 4×mat4 + 2×vec4 = **288 B** ⇒ **塞不进 push** |
| D3D12 root constants | 256 B，且 D3D11 **没有** push 等价物（只能 cbuffer） |

⇒ **只要 L1 承认 `VineViewBlock` 是一个"块"，per-view 数据就必须落 UBO/cbuffer；push 只能是后端内部优化。**
这与 §11.2.3 的原话一致（"push 仅内部优化，须与声明式块可证等价"）。

### 12.3 三个选项

| 选项 | 做法 | 代价 | 换后端 |
| --- | --- | --- | --- |
| **A** 维持 push（现状） | SDK 不声明块，只承诺"有 view/proj/model"，后端自选载体 | 最省 | ❌ DX11 无 push；L1 无法表达完整 `VineViewBlock` |
| **B** 全面 UBO | `VineViewBlock` per-view UBO + `VineDrawBlock` per-draw（dynamic offset）UBO | 每 drawable 多一次 UBO/offset 绑定；model 从 push 移出 | ✅ 干净 |
| **C** 混合（**推荐/采纳**） | L1 **声明** `VineViewBlock`/`VineDrawBlock`；vsg 前向仍可把 L1 子集塞进 push 作**内部优化**（`VineViewBlock.proj` + `VineDrawBlock.model` = 现在的 `pc`），并在后端注释/测试里记明这层等价；新后端用真 UBO/cbuffer | 需维护"push 实现 ≡ L1 子集"的对应；per-draw 参数（P10 opacity / 用户参数）一来仍需 per-draw UBO | ✅ 且不牺牲当下性能 |

**采用 C**：对外声明式块，push 是 vsg 的实现细节。

### 12.4 落地顺序（C）

| 步 | 内容 | 风险 |
| --- | --- | --- |
| **C1（2026-09-13 落地）** | SDK `ShaderAbi.hpp` 定义 `VineViewBlock`(288B) / `VineDrawBlock`(80B)（16 B 对齐、成员 `mat4`/`vec4`，与 `LightPushBlock`/`VineLightsBlock`/`MaterialBlock` 同一"全 vec4 对齐"纪律）+ `static_assert`；`ShaderAbiTest` 钉住 sizeof/offsetof | 低（纯新增） |
| **C2（2026-09-13 落地）** | vsg 后端把 push 标注为 C 的实现：`pc.projection ≡ VineViewBlock.proj`、`pc.modelView ≡ VineViewBlock.view * VineDrawBlock.model`（代码注释 + 头文档），并加测试钉住 shader 文本 / 范围 / `sizeof(VineViewBlock) > 128` | 低（行为中性） |
| C3 | 新后端（有第二个时）直接实现 UBO/cbuffer；`ShaderProgram` 参数表随首个消费者（P10 材质值/用户参数）一起落 | 中 |

### 12.5 L2 shim 何时做（结论：等第二个后端）

把 SDK shader 里的 `layout(set = 0, binding = N)` 换成角色宏（L2 preamble）**只在有第二个后端时才有收益**：
GLSL 与 HLSL 连**声明结构**都不同（`layout(binding = N)` vs `register(tN, spaceN)`），单后端下宏化只是把数字挪个位置。
因此：**先钉 L1（§11）+ 保持块 vec4 对齐（§12.4 C1）；L2 shim 与第二个后端一起做。**

### 12.6 B2 的前置（暂缓 `addParam`/`addInputSlot`）

`ShaderProgram` 的参数表与命名槽**需要消费者**才落地，否则是死 API：
- 命名槽（per-attachment）要先有引擎侧的**按附件命名**（现在槽模型是"整目标 publish，附件顺序 = binding 顺序"）；
- 参数表的首个真实消费者是 P10 的材质/每 drawable 值（`Material` dynamic offset）与用户参数。

⇒ **在这两个消费者出现之前，不引入 `addParam`/`addInputSlot`。**
