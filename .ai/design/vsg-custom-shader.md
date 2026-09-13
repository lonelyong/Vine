# vsg 后端自定义着色器设计（自写 shading ABI）

> 状态：设计稿 v1（2026-09-03）
> 2026-09-03 已落地：语义着色预置 `ShaderPreset` + 到 vsg 内建 set 的过渡映射，并完成像素验证（见 §8）。
> **2026-09-13 变更（推翻 §8 的枚举）**：`ShaderPreset` **已删除**，着色只能**显式指定 program**，且**没有兜底**。
> - 会话级入口：`RenderEngine::setContentProgram(intrusive_ptr<const ShaderProgram>)` / `contentProgram()`
>   （引擎默认就是命名的 `forwardProgram()`，构造函数里定好，`initialize()` 前转发给后端；运行中设置立即转发）。
> - 内建程序工厂（`BuiltinShaders.hpp`）：`forwardProgram()`（`vine_forward.*`）与 `flatForwardProgram()`
>   （同一对 stage，片元源注入 `#define VINE_FLAT 1`）。原来的 `builtinProgram(preset)` 与
>   `Pbr` / `ShadowedPhong` 两个"保留项"一起消失——**没落地的着色不再有名字**。
> - 后端**没有默认**：`makeContentShaderSet(program)` 在 `program == nullptr` 或它没有可编译的 stage 时
>   返回 null，调用方报一条 diagnostic 并**不画**（"declined, not substituted"，见 §8.3）。
> - `flat` 不等于 unlit：它仍然是光照着色，只是法线换成屏幕空间导数的**面法线**（§8 的老注释是错的）。
> - 证据：`vsg_selftest_evidence.sh` 基线 53 行逐字节不变（除三条改词），两条像素门禁 42 / 765 / 255,255,255 数字不变。
> 关联：`graphics-lighting.md`、`graphics-shadow.md`、`vsg-design.md`、`render-pipeline-builder.md`
>
> **一句话**：材质 / 光照 / 阴影 / 着色语义全部归 `graphics`（我们），vsg 只保留
> "窗口 / 交换链 / 资源 / 命令 / 管线构建 / 录制" 工程层。Shader/UBO/描述符契约是
> **后端无关的 ABI**，未来平移到 Diligent / 手写 Vulkan 后端时原样复用。

## 1. 动机与目标

- vendored vsg 的 phong 是**序列化 blob**（`phong_ShaderSet.cpp` 仅二进制流），
  不可编辑：材质数据被绑死在 `PhongMaterialValue`，光照依赖 `vsg::Light` 节点 +
  `ViewDependentState` 的 `lightData`，阴影依赖 vsg 内建黑盒。
- 实测结论（详见 `graphics-shadow.md` §10）：vsg 内建阴影在**独立 viewer**（vsg_probe）
  可用，但在我们引擎（多 view + 每帧重建光节点 + 自定义宿主）里集成不可靠、且黑盒。
- 目标：
  1. 我们拥有材质/光照/阴影的**完整数据布局与着色逻辑**，跨驱动确定、可扩展多光/PCF；
  2. 摆脱对 vendored blob 与 vsg::Light / PhongMaterialValue 的耦合；
  3. 这套 ABI 后端无关，为未来自研后端（Diligent / Volk+VMA）留好接缝。

## 2. vsg 边界

### 弃用（语义层，回归我们）
- `createPhongShaderSet()` / vendored phong / `PhongMaterialValue`
- `vsg::Light` 节点（`AmbientLight/DirectionalLight/...`）+ VDS 的 `lightData`
  （我们改用每帧 `LightsUBO`，直接由 `Scene.lights()` 打包）
- vsg 内建阴影（`HardShadows`/`ShadowSettings`/shadow 预渲染）——阴影改走我们自己的
  engine depth pass + 自写采样

### 保留（工程层，仍由 vsg 提供）
- `vsg::Window`（表面/交换链/多缓冲/acquire-present）、`vsg::Device`
- `CommandGraph` / `RenderGraph` / `View` / `RecordTraversal`（多视口、per-viewID 管线缓存）
- `RenderPass` / `Framebuffer`（主窗 + 离屏可采样 + depth-only）
- `GraphicsPipelineConfigurator` + `ShaderSet`（装配**我们自己的** stages/bindings）
- `ShaderCompiler`（GLSL→SPIR-V）、Buffer/Image/ImageView/Descriptor 等资源
- 顶点数组映射不变：保留 `vsg_Vertex / vsg_Normal / vsg_Color` 属性名
  → `SceneBridge::assignArray` 无需改动

## 3. 关键机制约束（来自 vendored vsg 源码核对）

- vsg 录制只在 ShaderSet **声明了 view-dependent binding**（名字如 `lightData`、
  `viewportData`、`shadowMaps`，经 `ViewDependentStateBinding`）时，才经
  `BindViewDescriptorSets` 在 set0 自动绑 VDS（`GraphicsPipelineConfigurator::_assignInheritedSets`）。
- 因此**自写 ShaderSet 只要不声明这些名字，VDS 就完全不介入**，set 布局归我们。
  现有 `makeScreenTextureNode` 已实证：不含 VDS 的自定义 ShaderSet 录制/渲染正常。
- 干净做法：把 `view->features` 置 `0`（或仅保留 `INHERIT_VIEWPOINT`），使 VDS 连
  lightData/阴影资源都不分配，避免无谓开销。

## 4. Shader ABI 草案

### 4.1 管线构建
`buildVineShaderSet(program, extent, depth_test)` 取代 `buildShaderSet()`：
- **程序是着色轴的唯一选择键**（2026-09-13 起：没有 `ShaderPreset` 枚举）：每个 program 产出自己的
  stages / bindings（内建 `forwardProgram()` / `flatForwardProgram()`，宿主也可以自己写）；
  用不了的 program（null / 无可用 stage）产出 null ⇒ 调用方报一条 diagnostic 并**不画**。
- 用 `ShaderCompiler` 编译自写 GLSL 450 VS/FS（运行时）；
- 装配与现状相同的 default states（depth/raster/cull-none/blend/inputAssembly/multisample/viewport）；
- 自写 binding 命名避免与 vsg view-dependent 名冲突（`vine_*` 前缀）。

### 4.2 描述符 set 布局

> **已按 §11 落地**：前向路径的 push 范围被矩阵占满（Vulkan 只保证 128 B），所以光走
> set0/binding2 的 UBO，材质仍在 set0/binding0；下面这段是原始草案，保留作对照。
```
set0  每帧
  b0  FrameUBO    { mat4 viewProj; mat4 view; mat4 proj; vec3 cam_pos; ... }
  b1  LightsUBO   { uint count; VineLight lights[N]; }   // 由 Scene.lights() 打包
  b2  sampler2DShadow shadow_map    // 该视图有 castShadow 光时才绑（我们自己的 depth RT）
set1  材质 / 每 drawable
  b0  MaterialUBO { vec4 base_color; vec4 emissive; float shininess; uint flags; ... }
      （或 dynamic 大缓冲 + dynamic offset，见 4.4）
set2  贴图（未来扩展）
```
`VineLight` 结构（先支持方向光）：
```
vec4  dir_and_type;    // xyz=方向, w=类型(0 ambient/1 dir/2 point/3 spot)
vec4  color_and_intensity;
vec4  params;          // spot 内外角 / point 衰减等
mat4  shadow_matrix;   // 仅 castShadow 有效
```
- shadow 采样：receiver 世界坐标 × `shadow_matrix` → [0,1] 深度比较
  （matrix = lightProj × lightView × model⁻¹ 约定，与 engine 光相机一致）。
- 先用 Hard（PCF 后续）；bias 取自 `Light::ShadowSettings.bias` 默认 0.002。

### 4.3 VS/FS
- VS：读 `vsg_Vertex/vsg_Normal/vsg_Color`；输出世界法线/位置（+ 未来 uv）；
- FS：材质 × 累加（ambient + N·L 方向光 + specular），有阴影则乘 shadow 项；
- **interface 变量名必须与顶点/片元精确一致**（glslang 链接要求），沿用
  `makeScreenTextureNode` 的教训。

### 4.4 每 drawable 的 model 矩阵与材质数据
推荐方案：**一块 dynamic UBO（矩阵 + 材质数组），按 drawable dynamic offset 绑定**，
每帧从命令流填充（复用 `SceneBridge` 的逐帧 reconcile 思路），管线数量收敛（少切换）。
备选：每 drawable 独立小 UBO（实现简单、切换多）。P0 可先走独立小 UBO，P2 再收敛为 dynamic。

### 4.5 数据上游（不变 & 新增）
- Frame/Lights：每帧从引擎相机 + 该 pass 内容场景的 `Scene.lights()` 打包
  （引擎 `RenderPass::execute → setLights` 已按 pass 转发内容光，天然可用）。
- Material：`Vine Material` 仍是**唯一真相源**；后端把其参数打包进 `MaterialUBO`
  （替代 `PhongMaterialValue`），缓存键仍是 `Material*`（闭环见资源生命周期管理文档）。
- 阴影 depth pass：使用 `RenderEngine` 已算好的**光相机（正交）**与 engine depth RT
  （1024² D24）——不再需要 vsg 内建 shadow 预渲染。

## 5. Node / 录制层不变
- `SceneBridge` 仍产出"壳"：`MatrixTransform → StateGroup → (Bind*/Draw*)`；只换
  StateGroup 内管线/描述符内容为自定义。
- **Node 是 draw 载体壳，语义全在 shader**——不依赖 vsg 场景图语义（无 Light/LOD/Bin）。

## 6. 分阶段落地

- **P0 垂直切片**：`buildVineShaderSet` 平替 `buildShaderSet`，只用于主场景几何；
  先复刻当前 phong 光照（ambient+方向光）使画面与现有输出一致（主视图/PiP 对照 A/B）。
  - **P0.1 shader + set（2026-09-13 已落地，见 §11）**：`vine_forward.*` + ABI + 门禁；默认路径未接线。
  - **P0.2 接线（2026-09-13 已落地，见 §11.2/§11.3）**：槽级 lights UBO + 描述符集、`makeContentShaderSet` 单一入口、`VINE_VSG_FORWARD` 开关、forward 独立证据基线 + lavapipe 阶段 3d/4。
  - **P0.3 转正（2026-09-13，见 §11.5）**：默认走自写 set（`VINE_VSG_BUILTIN=1` 退回内建）、自检相位改名、两条基线重生成；**未完**：去掉 vsg `Light`/VDS 的 content 用法（`view->features` 收敛）、无作者色/UV 时不喂白载体/零 UV。
- **P1 自写阴影**：绑定我们自己的 depth RT + `shadow_map` 采样，替换 vsg 内建。
- **P2**：材质 dynamic 化 / 多光 / overlay 迁移（overlay 可暂留 vsg phong 作内部特例）。
- 每阶段 GraphicsTest 不依赖后端，保持不变；用 lavapipe 截图 A/B + 真机 validation 验证。

## 7. 决策记录

- (2026-09-03) 选"自定义着色器"路线替代 vsg vendored phong；弃用
  `vsg::Light` / `PhongMaterialValue` / vsg 内建阴影；vsg 保留为工程层。
- (2026-09-13) 着色器源码改为"真文件 + 构建期嵌入"（§10）：GLSL 不再写在 C++ 字符串里，
  删除死文件 `src/plugins/gfx_backend_vsg/shaders/flat.*`（含两个 `.spv`）。
- 依据：vsg 内建阴影在独立 viewer 可用但引擎内集成不可靠（`vsg_probe`，见
  `graphics-shadow.md` §10）；blob phong 不可改。
- ABI 后端无关，作为未来 Diligent / 自研 Vulkan 后端的接缝。

## 8. 着色语义与过渡映射（2026-09-03 落地；**2026-09-13 枚举已被删除**）

> **读这一节前先看第 4 行起的变更说明**：8.1 的 `ShaderPreset` 枚举已经不存在。下面保留原文，
> 因为它记录了“为什么当初这么设计”和一条**今天仍然成立**的事实：**slot 的 shader set 是建 slot
> 时烘进去的**（程序 + 要喂哪个光源 + 读哪些 View features），所以运行中换着色必须重建着色侧。
> 今天这个动作由 `VsgRenderer::setContentProgram(program)` 执行（原来是 `setShaderPreset`）。

### 8.1 语义枚举（graphics SDK，后端无关）—— 【已删除，见头部变更】
`sdk/vine/graphics/ShaderPreset.hpp`：

```cpp
enum class ShaderPreset { StandardPhong, FlatShaded, Pbr, ShadowedPhong };   // 现已删除
```

- **StandardPhong**：lit Phong（diffuse/specular/ambient）——当前默认。
- **FlatShaded**：lit + **面法线**（屏幕空间导数），**不是 unlit**（老注释写错了）。
- **Pbr** / **ShadowedPhong**：**预留**，暂无后端实现（见下）。
- 归属：**渲染配置**（`RenderEngine` 持有，`setShaderPreset/shaderPreset`），初始化前转发后端
  （`RenderBackend::setShaderPreset` 默认 no-op）。**不放进 RenderPipelineBuilder**——preset 是
  "几何怎么着色"的着色轴，与 pass 拓扑（builder）正交；builder 仍是纯配方层。
  → **2026-09-13**：同一句话里把“preset”换成 **program**，入口是 `setContentProgram/contentProgram`，
  后端 `RenderBackend::setContentProgram` 默认 no-op，且**后端没有默认值**（null ⇒ 报 + 不画）。
- **会话中途切换（2026-09-13 落地）**：preset 不再只是"initialize 前的一次性决定"。一个 slot 的
  shader set 是**建 slot 时烘进去的**，而 set 带的不只是"哪个程序"：还有"这个 slot 要喂哪个光源"
  （`SceneBridge::hasOwnLightsBlock`）和该程序读哪些 View features。三者都无法事后打补丁，所以
  `VsgRenderer::setContentProgram` 在已初始化的会话上做的是**重建着色侧**：重建 window 三套 set
  （on/testonly/off）+ 丢掉每个 target 的 `content_slots`（经 `detail::resetContentShaderSlots`，
  走 `detachSlotView` + `clearCache()`，带计数设备等待）并清掉 target 自己烘的 `depth_*_shader_set`。
  下一帧各 pass 的懒建 slot 就用新 program 重建。**attachments / pass graph / 深度历史不动**——
  宿主看到的是"同一张图换了着色"，不是"会话重开"。像素门禁 `runLiveContentProgramSwitchPixelPhase`：
  同一个 target+slot 连画三次（forward 42 → 切 `flatForwardProgram()` 765 → 切回 forward 42），第三段把
  "只往前不回头"的实现钉死；关掉重建（变异验证）两段都报错。

### 8.2 过渡映射（vsg 内建 set，待 P0 自写替换）

- `StandardPhong` → `vsg::createPhongShaderSet()`
- `FlatShaded`   → `vsg::createFlatShadedShaderSet()`
- `Pbr` / `ShadowedPhong` → **预留**：vsg 1.1.16 虽有 `createPhysicsBasedRenderingShaderSet()`，
  但其 `material` 描述符是 `PbrMaterialValue`（非 PhongMaterialValue），SceneBridge/VsgMaterialManager
  的材质路径不兼容 → 需自定义材质路径，随 P0/P1；ShadowedPhong 随 shadow 路线（最后）。
- **兼容性事实（vsg_shader_dump 核对）**：flat 与 phong 的 `material` 描述符**都是**
  `vsg::PhongMaterialValue`（set1/binding10）→ 共享的 SceneBridge（`assignDescriptor("material",
  phong)`）+ VsgMaterialManager 材质管线**无需改动**即可切 FlatShaded。
- 工具：`vsg_shader_dump` 现 dump flat/phong/pbr 三套 attribute + descriptor + data 类型。

### 8.3 验证记录（lavapipe/Weston，设备像素 378x234）

- **preset 确实生效**：同相机同场景实拍 phong vs flat 全帧像素差 mad≈11.6；最亮孤立黄 cube
  区域 mad≈36.5（flat 无高光/渐变 → 纯色）。两 preset 均 exit=124 稳定、无管线/校验错误。
- **offscreen + PiP 两 preset 均正常**：`VINE_VSG_OFFSCREEN=1` → offscreen 640x360 挂载，
  PiP 189x106@(181,120) 右下挂载；PiP 区放大 = 整帧缩小版 mini-frame（灰菱形地面 + 彩块堆栈），
  证明 离屏渲染 → 发布 SceneColor → ScreenPass 采样上屏链路在跑。
- 环境开关：`VINE_SHADER_PRESET`（存在→FlatShaded）；`VINE_VSG_OFFSCREEN` 开离屏验证。
- 验证入口：`./build/bin/Vine`（非 install 副本），xwd 抓子窗口 + 自写 XWD→PNG 解码。

### 8.4 与 §4/§6 的关系
- P0 自写 `buildVineShaderSet(program, ...)` 时，`forwardProgram()` 须先复刻当前 phong 输出
  （§6 P0 A/B 对照）；届时 flat / 其他独立 stages/bindings 各自产出，过渡映射（8.2）退役。

## 9. vsg 内建 ShaderSet 输入约定（参考档案）

> 2026-09-04 核对（vsg 1.1.16，`build/_deps/vsg-src`）：`vsg_shader_dump` 反序列化
> flat/phong/pbr 三套 blob + `src/vsg/utils/GraphicsPipelineConfigurator.cpp` 源码 +
> blob 内嵌 GLSL 明文（`#version 450`…）。结论：**flat/phong/pbr 共用同一张契约表**，
> 仅光照算法与启用的贴图不同——这就是"内建 shader 的输入都一样"的原因。
> 用途：过渡期（仍映射内建 set）与 P0 自写 ShaderSet 的对照基准。

### 9.1 顶点属性（attributeBindings，三 preset 一致）

| 名字 | loc | format(枚举值) | GLSL | define（激活条件） |
|---|---|---|---|---|
| `vsg_Vertex` | 0 | 106 `R32G32B32_SFLOAT` | `vec3` | 恒开 |
| `vsg_Normal` | 1 | 106 | `vec3` | 恒开 |
| `vsg_TexCoord0..3` | 2..5 | 103 `R32G32_SFLOAT` | `vec2` | `VSG_TEXTURECOORD_{0..3}` |
| `vsg_Color` | 6 | 109 `R32G32B32A32_SFLOAT` | `vec4` | 恒开 |
| `vsg_Translation_scaleDistance` | 7 | 109 | `vec4` | `VSG_BILLBOARD` |
| `vsg_Translation` | 7 | 106 | `vec3` | `VSG_INSTANCE_TRANSLATION` |
| `vsg_Rotation` | 8 | 109 | `vec4`(四元数) | `VSG_INSTANCE_ROTATION` |
| `vsg_Scale` | 9 | 106 | `vec3` | `VSG_INSTANCE_SCALE` |
| `vsg_JointIndices` | 10 | 108 `R32G32B32A32_UINT` | `uvec4` | `VSG_SKINNING` |
| `vsg_JointWeights` | 11 | 109 | `vec4` | `VSG_SKINNING` |

- loc 7 上 `Translation_scaleDistance` 与 `Translation` 互斥（billboard vs instance）。
- **format 写死**：位置 vec3、颜色 vec4、uv vec2——喂错类型会拿错 location/stride。
- `vsg_Vertex/Normal/Color` 无 define → 变体恒含（phong VS 明文：
  `layout(location=0) in vec3 vsg_Vertex; layout(location=1) in vec3 vsg_Normal;`）。
- **per-vertex 透明度通道 = `vsg_Color` 的 alpha（loc 6 vec4）**——**内建退回路径**下
  SceneBridge 每帧重写 `color.a = cmd.opacity` 走的正是这个恒开槽位。**我们自己的 forward
  set 不再用它到这一步**：它把不透明度放进 `vine_draw` 块的 `params.x`（见 §11），
  `vsg_Color` 在那里只承载**作者写的颜色**（且其 alpha 不参与 opacity）。

### 9.2 描述符（descriptorBindings，phong 为例）

**set0 = 视图/每 view 全局**（RecordTraversal 经 ViewDependentState 自动填）：
- `lightData` b0 uniform `vec4Array`（场景 vsg Light 节点转出）、`viewportData` b1、
  shadowMaps/sampler b2..4。

**set1 = 每 drawable 材质/纹理**：
- 贴图（define-gated）：`diffuseMap` b0(`VSG_DIFFUSE_MAP`)、`detailMap` b1、
  `normalMap` b2、`aoMap` b3、`emissiveMap` b4、`displacementMap` b7(+`Scale` b8)
- **`material` b10 uniform `PhongMaterialValue`**（flat 同，pbr 为 `PbrMaterialValue`）
- `texCoordIndices` b11、`jointMatrices` b12(`VSG_SKINNING`)

Shader 侧：`#define VIEW_DESCRIPTOR_SET 0` / `MATERIAL_DESCRIPTOR_SET 1` +
`#pragma import_defines(VSG_TEXTURECOORD_0, VSG_DISPLACEMENT_MAP, VSG_SKINNING, …)`
导入为宏，`#ifdef` 包裹可选块。

`PhongMaterialValue` 布局（include/vsg/state/material.h）：`vec4 ambient/diffuse/
specular/emissive` + `float shininess/alphaMask/alphaMaskCutoff`——extra 字段我们
不用（已强置 `diffuse.a=1`）。

### 9.3 Push constant

```
pc: 全 stage, offset 0, size 128  →  layout(push_constant) uniform PushConstants
    { mat4 projection; mat4 modelView; } pc;
```
RecordTraversal 每个 drawable 绘制前自动填 → 自定义 program 路径的
`addPushConstantRange("pc","",VERTEX,0,128)` 正是复刻它。

### 9.4 对接机制：按名字查找 + define 变体（非硬编码 location）

`GraphicsPipelineConfigurator.cpp`：
- `assignArray(arrays,"vsg_Color",rate,data)` → `shaderSet->getAttributeBinding(name)`
  取 location/format；非空 `define` 塞进 shaderHints->defines。
- `assignDescriptor("material",value)` → 按名字查 set/binding，建描述符。
- ShaderSet 按 define 组合把 GLSL 编译成对应**变体**（`variants`）——没喂的
  define-gated 输入不进变体（省带宽/寄存器）。

### 9.5 与 Vine 的关系
- 默认路径只用了这张表的一小条：`vsg_Vertex`+`vsg_Normal`+`vsg_Color`(白)+
  `material`(b10)。§4 的 `vine_*` 自写 set 是"不用内建"时对这张表的等价重写。

## 10. 着色器文件与嵌入方式（2026-09-13 落地）

自写 shader 从"代码里的字符串字面量"改成"真文件 + 构建期嵌入"：文件有语法高亮、可 diff、可离线
编译校验，内容随二进制走（**不**往 DLL 旁边拷资源，**不**提交预编译 `.spv`）。§4 的 ABI 草案不变，
本节只回答四件事：文件放哪、怎么进二进制、怎么加一个、怎么被门禁挡住。

### 10.1 文件位置与归属

| 归属 | 目录 | 生成的头文件 | 命名空间 | 谁在用 |
| --- | --- | --- | --- | --- |
| graphics SDK（内建 program） | `src/viz/graphics/shaders/` | `vine/graphics/EmbeddedShaders.hpp` | `vine::graphics::shaders` | `BuiltinShaders`（`forwardProgram()` / `flatForwardProgram()` 前向着色 + gbuffer 几何 / 全屏光照）；`RenderPipelineBuilder` 是它的别名 |
| vsg 后端（自有阶段） | `src/plugins/gfx_backend_vsg/shaders/` | `vine/vsg/EmbeddedShaders.hpp` | `vine::vsg::shaders` | `VsgPipelineFactory`（overlay / PiP 全屏三角形）。**前向着色已不在这里**（2026-09-13 起归 SDK，见 §11.7） |

约定：

| 规则 | 原因 |
| --- | --- |
| 后缀即阶段：`.vert` / `.frag` / `.comp` / `.geom` / `.tesc` / `.tese` | 校验器据此判阶段，不需要额外清单 |
| 每行 LF 结尾 | CR 会被读写两端各自归一化，嵌入文本就与文件不一致；生成器直接报错 |
| 小 fixture（`app_shell` / `vsg_probe` / `vsg_selftest` / 测试探针）保持内联 | 它们不是产品 shader，放进清单反而多一层间接 |
| 常量名 = 文件名转大驼峰 + 阶段（`gbuffer_geometry.vert` → `kGbufferGeometryVert`） | 从文件名就能猜出常量名，拼错是编译错误而不是运行时回落 |

### 10.2 嵌入机制（构建期，不拷资源）

| 环节 | 位置 | 说明 |
| --- | --- | --- |
| 清单（shader → 头文件） | `cmake/VineShaders.cmake`（root `include(VineShaders)`） | 在**顶层**声明：生成规则对 `src/` 与 `tests/` 同时可见 |
| 机制 | `cmake/VineShaderHelper.cmake` | `v_declare_embedded_shaders(...)` + `v_use_embedded_shaders(<target> ...)` |
| 生成器 | `cmake/v_embed_shaders.cmake`（`cmake -P`） | 读文件 → 写 `inline constexpr std::u8string_view` + `Entry{name,hash,bytes}` 表 |
| 消费 | 各 CMakeLists | 加生成目录到 include、加生成顺序依赖（`test_vsg` 直接编译插件源码，所以也要挂） |

为什么是 `-P` 脚本 + `add_custom_command`，而不是 `file(READ)` + reconfigure：

| 方案 | 依赖追踪 | 代价 |
| --- | --- | --- |
| `-P` 脚本 + `add_custom_command`（**采用**） | ninja 原生：改 `.glsl` 只重编依赖它的 TU | 生成器自身改动也进依赖（实测改生成器会重新生成） |
| `file(READ)` + `CMAKE_CONFIGURE_DEPENDS` | 只能整包 reconfigure | 实测每次约 15s，改一个字也要全量 |

另两条实现细节：

| 细节 | 做法 | 为什么 |
| --- | --- | --- |
| 内容没变不重写头文件 | 生成器先比较再写 | 否则 touch 一下 `.glsl` 会引发一串无谓重编 |
| 生成器自带两条守卫 | 源里出现 CR、或出现 `)VINE_GLSL"` → `FATAL_ERROR` | 前者让嵌入文本≠文件；后者会提前结束 raw string 字面量 |

### 10.3 用法（C++ 侧）

```cpp
// SDK 侧（RenderPipelineBuilder.cpp）：ShaderStage::source 是 vine::String
vs.source = String(shaders::kGbufferGeometryVert);

// vsg 侧（VsgPipelineFactory.cpp）：vsg::ShaderStage::source 是 std::string
const std::string source(asShaderSource(shaders::kFullscreenVert));  // VsgUtils.hpp
```

| 类型 | 值 | 转换 |
| --- | --- | --- |
| 生成常量 | `std::u8string_view` | —— |
| `vine::String` | 内部 `std::u8string` | `String(kX)`（构造函数 explicit） |
| vsg `std::string` | —— | `asShaderSource(kX)`（`vine/vsg/VsgUtils.hpp`，GLSL 是 ASCII，逐字节视图） |

### 10.4 门禁

| 门禁 | 查什么 | 红了意味着 |
| --- | --- | --- |
| `scripts/vine_shader_check.sh` | (a) 每个 shader × 变体 define 组合（`VINE_VERTEX_COLOR` / `VINE_DIFFUSE_MAP`）跑 glslangValidator；(b) 每个嵌入副本的 SHA-256 前缀与字节数与磁盘文件一致；(c) 每个 `*/shaders/*` 文件都在清单里 | 语法错 / 变体分支编译不过 / 嵌入副本过期 / 有孤儿 shader 文件 |
| `test_graphics` / `test_vsg` 的 `EmbeddedShadersTest` | 每个条目是完整 GLSL（`#version 450\n` 开头、有 `main`、以 `}\n` 结尾）、`bytes == size()`、hash 是 16 位小写十六进制、名字唯一；工厂确实在用嵌入文本 | 生成器截断/转义出错，或常量与使用者脱钩 |
| `scripts/vsg_selftest_evidence.sh` | 47 行像素/计数证据与基线逐字节比对 | 移植造成渲染行为变化（本次迁移的等价性证明） |

变体策略（§4 的 define 变体）在本机制里的落法：**一份源文件 + `#ifdef`**，门禁把每个 define 组合都
编译一遍，所以"用了 define 却没测过另一支"在提交前就会被抓住。

### 10.5 加一个 shader 的步骤

1. 在归属目录放 `xxx.vert` / `xxx.frag`（LF 结尾）。
2. 在 `cmake/VineShaders.cmake` 对应 `SOURCES` 里加一行。
3. 在 C++ 里 `#include <vine/.../EmbeddedShaders.hpp>`，用 `kXxxVert`。
4. `cmake -S . -B build`（新文件要重新 configure），然后 `scripts/vine_shader_check.sh`。

> 与 §4 的关系：§4 是 ABI（set / binding / push constant 长什么样），本节是**这些 ABI 的载体**。
> P0 的 `vine_forward.*` 是第一个走新机制的**新** shader：顶点色用 `VINE_VERTEX_COLOR` 门控，
> 材质 UBO 从一开始就用 dynamic offset（§4.4）。

## 11. P0 第一步：`vine_forward` + `buildVineShaderSet`（2026-09-13 落地）

§4 的 ABI 草案在实现时撞上一条硬约束，据此定型（**结论写在 §4.2 之前先读本节**）：

| 约束 | 事实（`build/_deps/vsg-src` 核对 + 实测） | 后果 |
| --- | --- | --- |
| push constant 总量 | Vulkan 只保证 **128 字节**；vsg 的矩阵栈（`vsg::State::projectionMatrixStack/modelviewMatrixStack`）**已占满** 0..128（`{mat4 projection; mat4 modelView;}`） | 前向路径**没有** push 空间放光。全屏延迟路径能把 112 字节光块塞进 push，正是因为它不需要矩阵 ⇒ **前向的光必须走 UBO** |
| 顶点绑定号从哪来 | `GraphicsPipelineConfigurator::assignArray` 里 `bindingIndex = baseAttributeBinding + arrays.size()`：**按成功赋值的顺序**编号；名字未声明 ⇒ 跳过、后面全部前移 | ShaderSet 的**属性声明顺序**必须与数据节点的绑定顺序一致（位置 / 法线 / uv / 颜色 / 自定义），**location 可以不同** |
| define 变体怎么生效 | `assignArray` / `assignTexture` / `enableDescriptor` 在命中带 `define` 的绑定时 `shaderHints->defines.insert(define)` | “喂了数据 = 打开那个 define” ⇒ 不喂作者的顶点色，就自然得到**不含该属性**的变体（不需要白载体） |

### 11.1 落地形态

- 着色器：**SDK** `src/viz/graphics/shaders/vine_forward.{vert,frag}`（P0.A 起；后端经 `BuiltinShaders.hpp` 的
  `forwardProgram()` / `flatForwardProgram()` 取源并编译，见 §11.7；走 §10 的文件 + 嵌入机制；
  `VINE_VERTEX_COLOR` / `VINE_DIFFUSE_MAP` / `VINE_TEXCOORD_CUBE` / `VINE_FLAT` 四个门控）。
- 组装：`detail::buildVineShaderSet(program, extent, depth_test, depth_write, color_count)`
  （`VsgPipelineFactory.cpp`）：**有可用 stage 才返回非空**，否则 null ⇒ 调用方报一条 diagnostic
  且**不画**（2026-09-13 起没有“回落到内建 set”这一说了）。
- （历史）当时只对 `StandardPhong` 返回非空 —— 其它 preset 宁可用内建 set，也不要“被当成 phong 静默着色错”；
  2026-09-13 把“宁可用内建 set”改成“宁可什么都不画并报出来”。
- ABI（**取代 §4.2 草案的 set 布局**）：

| 位置 | 内容 | 谁填 |
| --- | --- | --- |
| attribute 0 / 1 | `vsg_Vertex` / `vsg_Normal` | SceneBridge 的数据节点 |
| attribute 2 | `vsg_Color`（define `VINE_VERTEX_COLOR`） | 同上（**作者颜色，调制而非不透明载体**） |
| attribute 8 | `vsg_TexCoord0`（define `VINE_DIFFUSE_MAP`） | 同上 |
| set0 / binding0 | `material`（std140，`PhongMaterialValue` 形状） | `VsgMaterialManager`（与内建/延迟路径同一个值） |
| set0 / binding1 | `diffuseMap`（define `VINE_DIFFUSE_MAP`） | 纹理缓存（含白色回退） |
| set0 / binding2 | `vine_lights`（`VineLightsBlock`，112 B） | **pass 的槽**，每视图一次 |
| **set1 / binding0** | `vine_draw`（`VineDrawBlock` = `mat4 model` + `vec4 params`，80 B，**UNIFORM_BUFFER_DYNAMIC**） | **SceneBridge**，每 drawable 一个槽（`VsgDrawBlockPool`）；绑定带 dynamic offset |
| push 0..128 | `{ mat4 projection; mat4 modelView; }` | vsg 矩阵栈（每 drawable） |

> **属性 location 现由 SDK 定义**（`ShaderAbi.hpp` 的 `attributeLocation`，0/1/2/8；契约见
> `graphics-shader.md` §11）：本节表格是 vsg 后端把角色映射成绑定别名的落点。

- 光照在**视图空间**做（与延迟路径同一约定），所以前向 shader 不需要 world 矩阵，
  **每 drawable 的唯一数据仍是 vsg 自动推的 modelView** ⇒ §4.4 的 dynamic UBO 不是 P0 的前置条件。
- 光的打包复用延迟路径那份实现（`collectViewSpaceLights`，`fillLightPushBlock` /
  `fillVineLightsBlock` 两个出口）⇒ 两条路径同一套“view-space 光 + 无光时种一个默认环境光”。
- 管线状态与内建 set **逐项相同**（同一个 `makeScenePipelineStates`），否则“两条路径同 pass 不同画面”。

### 11.2 P0.2 接线（2026-09-13 已落地）

| 环节 | 落点 |
| --- | --- |
| 开关 | `detail::vineForwardShaderEnabled()`：P0.2 用 `VINE_VSG_FORWARD` 存在即开；**P0.3 起默认开**，`VINE_VSG_BUILTIN` 存在即退回内建。**进程内只读一次**（这是会话级决策，不是每帧问题） |
| 选 set | `detail::makeContentShaderSet(preset, extent, depth_test, depth_write, color_count)`：开关开且该 preset 有 Vine stages 就用我们的 set，否则回内建；**所有** content set（窗口三档深度 + 各离屏目标）都走这一个入口，避免“一半换成新 shader” |
| 槽级光块 | `ContentSlot::lights_data`（`ubyteArray(sizeof(VineLightsBlock))`），`setupContentSlot` 建、注入桥（`SceneBridge::setLightsData`，照 `setTextureCache` 的样子） |
| 每帧填 | `renderContentSlot`：`fillVineLightsBlock(request.camera, *request.lights, block)` + memcpy + `dirty()`（方向是视图空间的，相机一动就得刷；112 B/槽/帧） |
| 挂描述符 | `SceneBridge::buildStateGroup`：`lights_data_ != nullptr && shaderSet->getDescriptorBinding("vine_lights")` 两条同时成立才 `assignDescriptor("vine_lights", …)` —— 内建/自定义 program 路径的 set 没声明它，于是完全不受影响 |

**默认路径零变化**：开关默认关闭 ⇒ 内建基线 47 行逐字节相同（mutation 验证：把开关默认改成 `true` ⇒ 基线立刻红）。

### 11.3 P0.2 的验证

| 门禁 | 覆盖 |
| --- | --- |
| `vsg_selftest_evidence.sh` | 跑同一份自检。**默认模式 = 自写前向 set**，比 `scripts/vsg_selftest_evidence.txt`；`--builtin` 用 `VINE_VSG_BUILTIN=1` 跑内建 set，比 `scripts/vsg_selftest_builtin_evidence.txt`。两条基线的差异**只有 6 个着色数字**（如 centre 34,6,2 vs 46,8,3；共享深度相位 4,31,8 vs 5,41,10），**覆盖数、深度值、清屏色、诊断计数一律相同** ⇒ 证明“同一份几何、换了一套着色”，而不是“画错了/少画了” |
| `gfx_lavapipe_check.sh` 阶段 3c/3d | 3c 跑默认（自写 set）自检：0 VUID、无 `[selftest] FAIL`、并报告像素；3d 跑 `VINE_VSG_BUILTIN=1` 的内建自检，再把**两条**证据基线逐字节各比一遍（帧数由证据脚本统一，避免“15 帧跑 vs 30 帧基线”的假红） |
| mutation | ① 跳过每帧光块填充 ⇒ forward 基线红（画面变黑）；② 开关默认改 `true` ⇒ 内建基线红 |
| 单测 | `ForwardShaderSetTest` +2：`makeContentShaderSet` 对**四个 preset × 深度组合 × 色彩数**永不为空（没有任何一个 pass 会没管线）；开关关闭时 content set 是内建 set（其布局里没有 `vine_lights`） |
| 口径 | 两边 47 行都不丢相位；test_vsg 233 → **235**；ninja 0 error 0 warning；lavapipe 整体 PASS |

### 11.4 还没做的（P0 之后）

| 项 | 说明 |
| --- | --- |
| 转正（P0.3） | **已做（2026-09-13，§11.5 + §11.6）**：默认走自写 set，vsg `Light`/VDS 的 content 用法已去掉（`view->features` 收敛到 0），无作者色/UV 时不再喂白载体/零 UV |
| 顶点色/贴图门控的收益 | **已做（2026-09-13，§11.6）**：forward set 在几何无作者色（且无作者 UV、材质无纹理）时**不 assign** 那两个数组 ⇒ define 关、少两条顶点绑定、少一次采样；白载体/零 UV 仍为内建/自定义 program 路径保留 |
| opacity | **已接（forward，2026-09-13）**：`alpha = material.diffuse.a * draw.params.x`。不透明度是**每 drawable 的值**，走 `vine_draw` 块：每 drawable 在 `VsgDrawBlockPool` 里一个槽，块值写进 **HOST_VISIBLE|HOST_COHERENT 映射内存**（每帧 O(1)，无 transfer、无 staging、也不落后一帧），槽由 **dynamic offset** 在 set1 的**共享**描述符集上选中。不进 variant 身份（透明与不透明共用一条管线）。**剩余**：`params` 承载材质值（要 B2 槽表）；内建退回路径仍用顶点载体（vsg phong 读 `vsg_Color.a`） |
| 阴影 / PBR / Flat | 仍走内建映射；§6 的 P1/P2 |
| 自检相位命名 | **已做（P0.3）**：探针那两条改名 `variant 'default shading + …'`（它跑的是内容 set，不是某条固定路径），两条基线一起重生成 |



### 11.3 本步门禁

| 门禁 | 覆盖 |
| --- | --- |
| `scripts/vine_shader_check.sh` | `vine_forward.*` 已进清单：4 种 define 组合全部过 glslangValidator + 嵌入副本与磁盘逐字节一致（7 个 shader） |
| `tests/test_vsg/ForwardShaderSetTest.cpp` | 6 条：只给有 stage 的 preset 建 set / 四个属性的 location 与 define / material+diffuseMap+vine_lights 的 set·binding·类型·块大小 / push 范围 / 状态与内建 set 逐项同类 / **两个 stage 的门控必须一致**（否则链接出未定义输入，Vulkan 不报错）/ 两个 stage 真的编出 SPIR-V |
| `tests/test_vsg/OverlayLightingTest.cpp` | +3 条：`fillVineLightsBlock` 与 push 块的光部分逐字段相同 / 无相机时全零 / 无光时种默认环境光 |
| mutation | 四条各自咬住目标测试：改一个 stage 的 define 名（门控一致性红）、把 `vine_lights` 从 b2 挪到 b3（ABI 红）、把 depthWrite 写死 false（状态一致性红）、去掉默认环境光种（两条可见性测试红） |
| 回归 | test_graphics 234、test_vsg 220 → **233**、test_core 82；selftest 证据 47 行逐字节相同（默认路径未接线）；lavapipe 0 VUID |


### 11.5 P0.3 默认转正（2026-09-13）

| 环节 | 落点 |
| --- | --- |
| 默认值 | `detail::vineForwardShaderEnabled()` 返回 `std::getenv("VINE_VSG_BUILTIN") == nullptr` ⇒ **自写 set 是 shipped 默认**；`VINE_VSG_BUILTIN=1` 退回内建（仍是进程内只读一次） |
| 相位改名 | 自检 variant 探针的 `'built-in Phong + …'` → `'default shading + …'`（探针跑的是内容 set，不再是某条固定路径） |
| 基线 | `scripts/vsg_selftest_evidence.txt` = **默认（自写 set）**；`scripts/vsg_selftest_builtin_evidence.txt` = 内建退回（原 `vsg_selftest_forward_evidence.txt` 与 `vsg_selftest_evidence.txt` 的语义对调 + 重命名）。两条都是 47 行，差异只有 6 个着色数字 |
| 门禁 | `vsg_selftest_evidence.sh`（默认）/ `--builtin`；`gfx_lavapipe_check.sh` 阶段 3c 跑默认、3d 跑内建并把两条基线都比一遍 |
| 单测 | `ForwardShaderSetTest.TheForwardSwitchIsOnByDefault`（原 `…IsOffByDefault`）：无 `VINE_VSG_BUILTIN` 时开关为真、content set 声明 `vine_lights` |

口径：两条基线的差异仍只有 6 个着色数字（centre 34,6,2 vs 46,8,3；共享深度 4,31,8 vs 5,41,10；clear-flip/testonly/MRT 同源），覆盖数/深度/清屏/诊断计数全同 ⇒ 差异是**光照公式**差异，可接受。

**已补齐（§11.6）**：vsg `Light`/VDS 的 content 用法已去掉；无作者色/UV 时不再喂白载体/零 UV。


### 11.7 完全不使用 vsg 内建 ShaderSet（2026-09-13）

拍板：**内容着色只有一条路径 —— 引擎自己的 set**。`vsg::createPhongShaderSet()` /
`createFlatShadedShaderSet()` 不再出现在任何渲染路径里（`vsg_shader_dump` / `vsg_probe`
这两个**对比工具**除外，它们存在的意义就是拿 vsg 的 set 来对照）。

| 环节 | 落点 |
| --- | --- |
| 选 set | `detail::makeContentShaderSet(preset, …)` **只**调 `buildVineShaderSet`，而且**没有替补**：没有自己 program 的 preset（Pbr / ShadowedPhong）返回 **null**，调用方上报并**什么都不画**（见下一行） |
| 删除 | `detail::buildShaderSet()`、`detail::vineForwardShaderEnabled()`、`VINE_VSG_BUILTIN` 开关 |
| 桥的兜底 | `SceneBridge::baseShaderSet()` 无注入时建**我们的** forward set（原为 `createPhongShaderSet()`）；桥本身仍接受**任何** set（SDK 允许后端被塞入外来的 set，测试就用 vsg 的 set 当这种“外来者”） |
| 没有有效 shader ⇒ 不画（2026-09-13 口径） | 三条路都**报错并跳过 drawable/槽**，绝不用别的着色顶替：① preset 没有自己的 program ⇒ 建槽时每会话一条 Error（`ShaderFallback`，“本会话内容不会绘制”）；② 槽没有被注入 set（`SceneBridge::baseShaderSet()` 不再兜底造 set）⇒ `buildStateGroup` 每桥一条 Error + 返回空（该 drawable 不入图）；③ 用户 program 编译/装配失败 ⇒ 沿用既有的 per-(program,layout,revision) 报告，但**不再回落**到槽的 set。**判据**：`ABridgeWithNoShaderSetReportsAndDrawsNothing`（`root->children` 为空 + 恰好一条 Error + 注入 set 后重新武装）＋自检预设相位“Pbr 画的中心仍是清屏色，而 StandardPhong 画 (255,255,255)”。附带修正：`setShaderSet()` 现在会 invalidate 保留的 state wrapper（那些管线/描述符是旧 set 的）|
| 门禁 | `vsg_selftest_evidence.sh --builtin` 与 `scripts/vsg_selftest_builtin_evidence.txt` **删除**（那条路径已不可能产生）；`gfx_lavapipe_check.sh` 的 3d 从“两条基线各比一遍”并成“一条基线比一遍” |
| 单测 | `ForwardShaderSetTest.TheForwardSwitchIsOnByDefault` → **`EveryContentSetIsTheEnginesOwn`**：四个 preset 的 content set 都非空、都声明 `vine_lights`、stages 数一致；`TheLightSourceFollowsTheSlotSetNotTheSession` 改成“四个 preset 都是我们的” + 用 **vsg 的 set 当外来 set** 钉住 `vsg_lights` 那条老路径；管线状态奇偶校验不再拿 vsg 的 set 当参照，改成同程序的另一档深度变体 |
| 自检相位 | preset 相位的 Pbr 那一段：从“内建回落画出了一点亮色”改成“**与 StandardPhong 同一四边形像素相同**（±4）”——替补必须是引擎自己的前向模型，而不是另一套库的着色（后者也会画出亮色，但值不同） |
| 判据 | 证据基线 **51 行**，只有 preset 相位那一行改写；test_vsg **249**；test_graphics 240；`vine_shader_check` PASS；lavapipe PASS |

为什么这么做：一套 ABI（我们声明属性位置 / 描述符 / 推常量范围），不再同时维护“我们的”和
“vsg 的”两套；灯源（`vine_lights` vs vsg 的 view-dependent lightData）、属性位置、`ViewFeatures`
的差异面随之消失；宿主拿到的着色一定可解释。

**未完**：PBR / shadowed 的**真程序**（PbrMaterialValue + IBL / shadow map）仍未落地；2026-09-13 起它们
**不再有名字**（枚举删除），宿主需要就在自己的 `ShaderProgram` 里写。vsg 的 `Light` / `ViewDependentState`
现在只在**外来 set** 被注入时才需要（`SceneBridge::hasOwnLightsBlock()` 仍是每 set 的判断）。

> 注意：§11.3 / §11.5 / §11.6 里提到的 `--builtin`、内建基线、`vineForwardShaderEnabled()`、
> `vsg_lights = !vineForwardShaderEnabled()` **都已删除/改写**（那几节记录的是当时的状态）。

### 11.6 P0.3 收尾：content 不再走 vsg 灯/VDS + 门控变体真正生效（2026-09-13）

| 环节 | 落点 |
| --- | --- |
| vsg 灯只在退回路径 | `VsgContentSlot`：`vsg_lights = !vineForwardShaderEnabled()`。forward 时槽不建/不种 vsg 灯节点、view 不挂 `light_group`、每帧不跑 `setGroupLights`（也没有“灯全不可用”的诊断） |
| VDS 收敛 | forward 时 content view 用 `View::create(camera, {}, static_cast<ViewFeatures>(0))` ⇒ `ViewDependentState` 不收集灯、`lightData` 缓冲停在 1 vec4 最小尺寸（内建退回路径仍是默认 `RECORD_ALL`） |
| 不喂派生数组 | `SceneBridge::buildStateGroup(...)` 新增 `derived` 参数：forward set 且 `arrays[3] == derived->white_colors` 时把 `vsg_Color` 置空（define 关）；**仅当颜色也被丢**且 `arrays[2] == derived->zero_texcoords` 且解析到的纹理是白色回退时才把 `vsg_TexCoord0` 也置空、并跳过 `assignTexture("diffuseMap")`（两者共用 `VINE_DIFFUSE_MAP`）。理由：数据节点把颜色绑在固定 canonical index 3，单独丢 `vsg_TexCoord0` 会让 `vsg_Color` 的绑定号前移到 2 而与数据节点不符 |
| 变体身份 | 是否丢属性会改变管线，故把 `(color_bound<<0)|(uv_bound<<1)` 并入 L2 variant 的 `layout` 哈希，避免同材质/状态但属性不同的几何复用同一条管线 |
| 判据 | 两条证据基线 47 行**逐字节不变**（丢属性只省绑定/采样，画面等价：无作者色=白调制、白纹理=乘 1）；test_vsg **237**（+2：`ForwardSetDropsDerivedColourAndUvs` 断言 2 条顶点绑定 + 无采样器；`BuiltInSetKeepsTheFullCanonicalPrefix` 断言内建仍 4 条）；lavapipe 整体 PASS |

> **更正（见 §11.9）**：这一节把「define 关掉」当作变体机制在生效，但当时 `vine_forward.*` 的源码里**没有** `#pragma import_defines`，而 vsg 只对 pragma 列出的名字发 `#define` ⇒ `VINE_DIFFUSE_MAP` / `VINE_VERTEX_COLOR` **两个分支从未真正编译过**（丢属性确实省了绑定/采样，但采样分支本身也一直是关的）。本节其余结论（绑定前缀、variant 身份、基线与 47 行不变）仍然成立；「门控变体真正生效」这句在 §11.9 才成立。

**仍未做**：`params` 承载材质值与用户参数（要 B2 槽表，即第二个标量消费者）；槽池的分块调优（当前 64 槽/块）；阴影 / PBR / Flat（§6 的 P1/P2）。

### 11.7 每 drawable 块：为什么是 set1 + dynamic offset（2026-09-13）

- **一个共享缓冲 + dynamic offset**，不是“每 drawable 一个 UBO”：描述符集是池稀缺资源，也是每新增
drawable 最贵的一操作；共享后一块（chunk）里的 drawable 只共用一个 set，选中靠 16 位偏移。
- **为什么要独立 set（1）**：块必须**按 drawable** 绑（偏移每 drawable 不同），而 vsg 为普通
binding 建的是**每 variant 一条**共享命令 ⇒ 表达不了。所以 set1 用 `CustomDescriptorSetBinding`
（vsg 官方钩子）：它只提供 **layout**，`createStateCommand` 返回空，bind 由 SceneBridge 追加到
每个 drawable 的 state wrapper 末尾（模板命令在后、偏移命令在前）。
- **为什么不用 push**：Vulkan 只保证 128 B push，已被两个 mat4 用满；D3D11 根本没有 push。
- **为什么映射内存**：`DYNAMIC_DATA` 的 Data 路径在 dirty() 时重传**整个**缓冲；映射后一次改 4 个
float，且值在**记录本帧时**已在缓冲里（transfer 路径最早也是下一帧）。
- **`model` 不写**：vsg 前向的矩阵走 push（见 §12.4 C2 等价标注），故这个后端写 `model` 是 64 B/
drawable 白花；块**声明**仍属 L1 契约，读它的后端（无 push 的 DX/GL）在同一槽里填就是。
- **三个坑（都踩过，见 `.ai/memory/graphics.md`）**：① set 范围从 `descriptorBindings` 推导，
自定义 set 必须在 `descriptorBindings` 里也声明一行，否则 pipeline layout 少一个 set、SPIR-V 引用
不存在的 set ⇒ 驱动段错误；② 内建 phong 集自带 set1（材质），往它绑我们的动态 set 会 layout 不匹配
⇒ 也必须段错误，所以按 **layout 形状**判定“这是不是我们的 set”；③ 池必须**比桥活得久**（与
texture/mesh cache 同一契约），桥的析构**不得**碰池。


### 11.7 P0.A：内建前向着色归 SDK（2026-09-13）

> **2026-09-13 后续变更**：这一节里的 `builtinProgram(ShaderPreset)` 已被
> `forwardProgram()` / `flatForwardProgram()` 取代（枚举删除），`VsgPipelineFactory::compiledStages(program)`
> 现在按 **program 指针** 缓存（map 的 value **拥有** 那个 program）。下表保留当时的落点记录。

| 环节 | 落点 |
| --- | --- |
| 文件搬家 | `vine_forward.{vert,frag}`：`src/plugins/gfx_backend_vsg/shaders/` → **`src/viz/graphics/shaders/`**；清单 `cmake/VineShaders.cmake` 两条随之移到 `vine/graphics/EmbeddedShaders.hpp`（嵌入数 3 → **5**，vsg 4 → **2**） |
| SDK 入口 | 新增 `BuiltinShaders.hpp/.cpp`：`builtinProgram(ShaderPreset)`（preset → 内建 program，未实现的 preset 返回 null）→ **现为** `forwardProgram()` / `flatForwardProgram()`；另有 `gbufferGeometryProgram()` / `deferredLightProgram()`（从 `RenderPipelineBuilder` 搬来；builder 的两个静态工厂改为**转发**，公开 API 不变） |
| 后端 | `VsgPipelineFactory::compiledStages(preset)`：取 `builtinProgram(preset)` 的 stages 编译（**每 preset 缓存一次**，空则 decline）；`buildVineShaderSet` 不再自带 GLSL、不再用 `preset != StandardPhong` 硬判 → **现为** `compiledStages(program)`，缓存键 `(program 指针, 变体 hash)` |
| 判据 | **行为中性**：两条证据基线 47 行逐字节不变；`vine_shader_check.sh` PASS（7 shader）；test_graphics 234 → **235**（+1：`builtinProgram(StandardPhong)` 用嵌入源、Pbr/ShadowedPhong 返回 null）、test_vsg 237 → **238**（+1：vsg 表**不含** `vine_forward.*`，钉住归属边界）；lavapipe PASS |

**边界**：SDK 拥有**着色文本**（L3）；后端拥有**编译 + ABI 绑定 + 管线**（L2）。`ShaderSet` 仍是 vsg 后端内部机制，不进 SDK。下一步（P0.B）：把 ABI 契约（属性角色 / `VineFrame`・`VineDraw` / 参数・槽表）也移到 SDK，为换后端铺路。

### 11.9 门控变体真正生效 + cube 方向槽（2026-09-13）

**发现（这是本轮最重要的一条）**：`vine_forward.{vert,frag}` 用了 `#ifdef VINE_DIFFUSE_MAP`
/ `VINE_VERTEX_COLOR`，但源码里没有 `#pragma import_defines`。vsg 的
`ShaderCompiler::combineSourceAndDefines` **只对 pragma 列出的名字**发 `#define`，其余被**静默丢弃**：
没有编译错误、没有 validation、没有诊断 —— 分支只是永远不编译。也就是说
`buildVineShaderSet` 里那两个 define（attribute binding + descriptor binding 都声明了）自落地以来
**从未产生过任何效果**：内建 forward 路径其实不采样纹理、不读顶点色。

为什么没被发现：门禁全是**结构性**的（`ForwardShaderSetTest` 断言「define 名字出现在两个 stage 里」
—— 名字确实在，pragma 里在不在它没问），唯二的像素门禁（texture / cube map）走的是**用户 program**
路径（自带 GLSL、不需要 define）。这正是 `.ai/memory/graphics.md` 里那条教训的第二个实证：
「结构性断言可以全绿而画面没动」。

| 环节 | 落点 |
| --- | --- |
| 修复 | 两个 forward stage 源码加 `#pragma import_defines (VINE_VERTEX_COLOR, VINE_DIFFUSE_MAP, VINE_TEXCOORD_CUBE)`（必须在 `#version` 之后那一行；vsg 把这两行搬进 header） |
| 结构化门禁 | `ForwardShaderSetTest::TheForwardStagesAskForEveryDefineTheBackendCanSet`：**后端会设的每个 define 名字必须出现在 pragma 的括号列表里**（这条门禁本来就能挡住上面那个 bug） |
| 像素门禁 | selftest 新增 `built-in sampling` 相：同一张双色纹理 + 同一个六色 cube，**不设 program**，即由引擎自己的 shader 采样。变异验证：去掉 pragma ⇒ 两行都 FAIL；只去掉 `VINE_TEXCOORD_CUBE` ⇒ 2D 行仍 PASS、cube 行 FAIL |
| cube 方向槽 | 同一 location 8 的**第二种宽度**：3 分量。SDK 加 `Geometry::setTexcoords3()`（整块 + 段两种拼写，与其它角色同形）；命名定为**宽度制**（`setTexcoords2`/`setTexcoords3` + `texcoordComponents()`）而不是 `setCubeDirections`：SDK 只陈述数据宽度，用途属于采样器（同一个 3 分量通道配自定义 program 可以是 volume 坐标）；后端 `detail::texCoordArray()` 按 `components` 建 `vec2Array`/`vec3Array` **并在阵列上陈述顶点格式**（`properties.format`），`SceneBridgePipeline` 由绑定的阵列读回**宽度**（`isThreeScalarTexcoord`）：3 分量 ⇒ 给该 drawable 的编译设置插入 `VINE_TEXCOORD_CUBE`，shader 编译出 `samplerCube` + `vec3` 属性 |
| 采样器种类 | 由**槽的形状**决定，两种不可混：`samplerCube` 绑 2D 视图（或反之）不是"白贴图"而是**非法描述符**。纹理种类不匹配时**报一次 + 绑该种类的白色回退**（`VsgTextureCache::whiteCubeFallback()`，1×1×6 面）。**只对引擎自己的 set 生效**：用户 program 自带 sampler 与坐标（cube 相就是拿 UV 通道自己算方向），替它的纹理是把画面换成它没要的那个 |
| 变体身份 | 形状选择管线 ⇒ `(color_bound<<0)|(uv_bound<<1)|(cube<<2)` 并入 L2 variant 的 `layout` |
| 判据 | 证据基线 51 → **53** 行（仅新增 2 行，其它数字逐字节不变）；`vine_shader_check.sh` 变体矩阵 3 → **4** 个 define（7 shader × 16 组合）PASS；test_vsg 250 → **252**；test_graphics 246 → **247**；lavapipe PASS（无 validation 错误，说明 cube 视图 ↔ `samplerCube`、`R32G32B32` 属性都合法） |

**踩过的坑**：① 我第一版把「槽形状 ↔ 采样器种类」规则无差别应用到 program 路径，直接被现有
`cube map` 相抓住（六条带全白）—— 用户 program 的 artifact 不是引擎可以替换的；② `Geometry::setTexcoords2()`
硬编码 2 分量，所以 3 分量的方向**没有 SDK 拼写**，必须新开一种拼写（落地为 `setTexcoords3()`，不是用
`addBuffer(kTexCoordLocation, …)` 蒙过去）；③ `aliasArray()` 的注释声称格式由元素类型推断，
实际 **vsg 的 `Array::assign` 只设 stride，format 保持 UNDEFINED**，pipeline 的顶点格式来自
binding 声明 —— 一个 ShaderSet 服务两种形状时，必须由阵列陈述 `properties.format`。

### 11.10 着色只能显式指定 program：删除 `ShaderPreset`，且不兜底（2026-09-13）

**决定**："不要兜底，必须显示指定着色器"。`ShaderPreset` 枚举**删除**，`RenderEngine::setContentProgram`
成为唯一的会话级着色入口；后端**没有默认值**，没有可用 set 的 drawable **不画**并报出来。

**为什么删掉枚举**：枚举和 program 本来就是同一个模型的两套入口，而枚举里
`Pbr` / `ShadowedPhong` 两个"保留项"其实**什么都不是**——后端对它们的回应是"没有程序"。
留着这两个名字等于对宿主承诺了一种兜底语义（"没有实现的预设会回落到 StandardPhong"），
而那条兜底恰恰是我们要禁掉的东西：宿主拿到一张自己没要、也认不出的画面，比拿到一张空的、
带原因的画面更糟。宿主自己的着色用枚举根本表达不了，所以最终形态只剩一种：**命名一个 program**。

| 环节 | 落点 |
| --- | --- |
| SDK 工厂 | `BuiltinShaders.hpp/.cpp`：`builtinProgram(ShaderPreset)` → **`forwardProgram()`**（名字 `vine_forward`）与 **`flatForwardProgram()`**（名字 `vine_flat`，与 forward **同一对 stage**，片元源 `withDefine(frag, "#define VINE_FLAT 1")`）。文档同时写明：**程序是选择着色的唯一方式**，以及 **flat 不是 unlit**（老注释写错了，实测同一 quad 765 vs 42） |
| 引擎 | `RenderEngine::setContentProgram(intrusive_ptr<const ShaderProgram>)` / `contentProgram()`；字段 `content_program_`，**构造函数里定成 `forwardProgram()`**（默认不是"没有"），`initialize()` 前转发，运行中设置**立即转发**（旧实现只在 initialize 前生效，之后再设是静默 no-op） |
| 后端接口 | `RenderBackend::setContentProgram(...)` 默认 no-op（能加载 ≠ 必须实现着色）；`VsgRenderer::setContentProgram` 在活会话上做**重建着色侧**（window 三套 set + `detail::resetContentShaderSlots`），与 §8.1 里 `setShaderPreset` 的动作逐字相同 |
| 后端（vsg） | `VsgPipelineFactory`：`buildVineShaderSet(program, …)` / `makeContentShaderSet(program, …)` / `compiledStages(program)`，缓存 map 的 key 是 `(program 指针, 变体 hash)`，value **拥有** 那个 program（`nullptr` 或"没有可用 stage"⇒ 返回 null）；`VsgContentSlot` 报一条会话级诊断（**每会话一次**）并且**不画**；`SceneBridgePipeline` 对没 set 的 slot 同样报 + 丢 |
| 管线的占位预设 | 顺手补了同一类谎：`PipelinePreset::ForwardShadowed/DeferredShadowed` 今天装配的就是无阴影版本。现在 `RenderPipelineBuilder::build` 会报一条 `DiagnosticCategory::UnsupportedRequest`（新枚举值；为了让 builder 能走引擎的 sink，`RenderEngine::reportEngineProblem` 从 private 移到 public） |
| selftest | 两个相位改名（`runProgramShadingPixelPhase` / `runLiveContentProgramSwitchPixelPhase`）；**启动时先 `backend->setContentProgram(forwardProgram())` 再 `initialize()`**（后端不再有默认；晚设会让起来后的那 30 帧无程序可画，而且会多报一串诊断） |
| 单元门禁 | `ForwardShaderSetTest` 四处结构性改写（`OnlyProgramsWithUsableStagesGetASet`、`AProgramWithNoUsableStagesIsDeclinedNotSubstituted`、`EveryContentSetIsTheEnginesOwn`、`TheLightSourceFollowsTheSlotSetNotTheSession`、`TheFlatProgramReusesTheForwardStagesWithItsDefine`）；`GraphicsTest::ContentProgramForwardedToBackend`（含"运行中也要被告知"与"null 是合法答案"）；新增 `RenderPipelineBuilderTest::ShadowedPresetsReportThatTheyArePlaceholders` |
| 判据 | 证据基线 53 行**逐字节不变，只改了 3 行的词**（`preset shading:` / `live preset switch:` → `program shading:` / `live program switch:`），**所有数字原样**：`42` / `765` / `(255,255,255)` / `(10,20,30)`——即画面完全没动；test_graphics 247 → **248**（+1，变异验证：关掉 `reportEngineProblem` 该测试必失败）；test_vsg **252 不变**（改写而非新增）；`vine_shader_check.sh` PASS；`check_diagnostic_formats.py` 0 suspicious；lavapipe PASS；ctest 仅 3 个既有失败（test_cppstd / test_runtime / test_system） |
