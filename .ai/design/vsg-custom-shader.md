# vsg 后端自定义着色器设计（自写 shading ABI）

> 状态：设计稿 v1（2026-09-03）
> 2026-09-03 已落地：语义着色预置 `ShaderPreset` + 到 vsg 内建 set 的过渡映射，并完成像素验证（见 §8）。
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
`buildVineShaderSet(ShaderPreset preset, extent, depth_test)` 取代 `buildShaderSet()`：
- `ShaderPreset`（graphics 语义枚举，见 §8）是**着色轴的选择键**；未来每个 preset 产出自己的
  stages / bindings（Phong / Flat / PBR / Shadowed 变体）。
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

## 8. ShaderPreset 语义与过渡映射（2026-09-03 已落地 + 像素验证）

### 8.1 语义枚举（graphics SDK，后端无关）
`sdk/vine/graphics/ShaderPreset.hpp`：

```cpp
enum class ShaderPreset { StandardPhong, FlatShaded, Pbr, ShadowedPhong };
```

- **StandardPhong**：lit Phong（diffuse/specular/ambient）——当前默认。
- **FlatShaded**：unlit 平面着色（常量色）。
- **Pbr** / **ShadowedPhong**：**预留**，暂无后端实现（见下）。
- 归属：**渲染配置**（`RenderEngine` 持有，`setShaderPreset/shaderPreset`），初始化前转发后端
  （`RenderBackend::setShaderPreset` 默认 no-op）。**不放进 RenderPipelineBuilder**——preset 是
  "几何怎么着色"的着色轴，与 pass 拓扑（builder）正交；builder 仍是纯配方层。

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
- P0 自写 `buildVineShaderSet(ShaderPreset, ...)` 时，StandardPhong 须先复刻当前 phong 输出
  （§6 P0 A/B 对照）；届时 FlatShaded/Pbr/ShadowedPhong 各自产出自写 stages/bindings，
  过渡映射（8.2）退役。

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
- **per-vertex 透明度通道 = `vsg_Color` 的 alpha（loc 6 vec4）**——我们 SceneBridge
  每帧重写 `color.a = cmd.opacity` 走的正是这个恒开槽位。

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
| graphics SDK（内建 program） | `src/viz/graphics/shaders/` | `vine/graphics/EmbeddedShaders.hpp` | `vine::graphics::shaders` | `BuiltinShaders`（`builtinProgram(preset)` 前向着色 + gbuffer 几何 / 全屏光照）；`RenderPipelineBuilder` 是它的别名 |
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
  `builtinProgram(preset)` 取源并编译，见 §11.7；走 §10 的文件 + 嵌入机制；
  `VINE_VERTEX_COLOR` / `VINE_DIFFUSE_MAP` 两个门控）。
- 组装：`detail::buildVineShaderSet(preset, extent, depth_test, depth_write, color_count)`
  （`VsgPipelineFactory.cpp`），只对 `StandardPhong` 返回非空 —— 其它 preset 宁可用内建 set，
  也不要“被当成 phong 静默着色错”。
- ABI（**取代 §4.2 草案的 set 布局**）：

| 位置 | 内容 | 谁填 |
| --- | --- | --- |
| attribute 0 / 1 | `vsg_Vertex` / `vsg_Normal` | SceneBridge 的数据节点 |
| attribute 2 | `vsg_Color`（define `VINE_VERTEX_COLOR`） | 同上（**作者颜色，调制而非不透明载体**） |
| attribute 8 | `vsg_TexCoord0`（define `VINE_DIFFUSE_MAP`） | 同上 |
| set0 / binding0 | `material`（std140，`PhongMaterialValue` 形状） | `VsgMaterialManager`（与内建/延迟路径同一个值） |
| set0 / binding1 | `diffuseMap`（define `VINE_DIFFUSE_MAP`） | 纹理缓存（含白色回退） |
| set0 / binding2 | `vine_lights`（`VineLightsBlock`，112 B） | **pass 的槽**，每视图一次 |
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
| opacity | **forward 已接（2026-09-13）**：`VINE_VERTEX_COLOR` 打开时 `alpha *= v_color.a`，载体 alpha 由 SceneBridge 按 `cmd.opacity` 维护（与内建路径同机制）；**完全不透明的 drawable 仍不绑该属性**（`drop_color` 仅在 `opacity >= 1` 时成立，opacity 跳 1 是 state 变化）。**剩余**：per-drawable 值改走 dynamic-offset UBO，届时连 per-vertex 重写也省掉 |
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


### 11.6 P0.3 收尾：content 不再走 vsg 灯/VDS + 门控变体真正生效（2026-09-13）

| 环节 | 落点 |
| --- | --- |
| vsg 灯只在退回路径 | `VsgContentSlot`：`vsg_lights = !vineForwardShaderEnabled()`。forward 时槽不建/不种 vsg 灯节点、view 不挂 `light_group`、每帧不跑 `setGroupLights`（也没有“灯全不可用”的诊断） |
| VDS 收敛 | forward 时 content view 用 `View::create(camera, {}, static_cast<ViewFeatures>(0))` ⇒ `ViewDependentState` 不收集灯、`lightData` 缓冲停在 1 vec4 最小尺寸（内建退回路径仍是默认 `RECORD_ALL`） |
| 不喂派生数组 | `SceneBridge::buildStateGroup(...)` 新增 `derived` 参数：forward set 且 `arrays[3] == derived->white_colors` 时把 `vsg_Color` 置空（define 关）；**仅当颜色也被丢**且 `arrays[2] == derived->zero_texcoords` 且解析到的纹理是白色回退时才把 `vsg_TexCoord0` 也置空、并跳过 `assignTexture("diffuseMap")`（两者共用 `VINE_DIFFUSE_MAP`）。理由：数据节点把颜色绑在固定 canonical index 3，单独丢 `vsg_TexCoord0` 会让 `vsg_Color` 的绑定号前移到 2 而与数据节点不符 |
| 变体身份 | 是否丢属性会改变管线，故把 `(color_bound<<0)|(uv_bound<<1)` 并入 L2 variant 的 `layout` 哈希，避免同材质/状态但属性不同的几何复用同一条管线 |
| 判据 | 两条证据基线 47 行**逐字节不变**（丢属性只省绑定/采样，画面等价：无作者色=白调制、白纹理=乘 1）；test_vsg **237**（+2：`ForwardSetDropsDerivedColourAndUvs` 断言 2 条顶点绑定 + 无采样器；`BuiltInSetKeepsTheFullCanonicalPrefix` 断言内建仍 4 条）；lavapipe 整体 PASS |

**仍未做**：per-drawable 值改走 dynamic-offset UBO（P10 后半；opacity 已能工作，只是仍走 per-vertex 载体）；阴影 / PBR / Flat（§6 的 P1/P2）。


### 11.7 P0.A：内建前向着色归 SDK（2026-09-13）

| 环节 | 落点 |
| --- | --- |
| 文件搬家 | `vine_forward.{vert,frag}`：`src/plugins/gfx_backend_vsg/shaders/` → **`src/viz/graphics/shaders/`**；清单 `cmake/VineShaders.cmake` 两条随之移到 `vine/graphics/EmbeddedShaders.hpp`（嵌入数 3 → **5**，vsg 4 → **2**） |
| SDK 入口 | 新增 `BuiltinShaders.hpp/.cpp`：`builtinProgram(ShaderPreset)`（preset → 内建 program，未实现的 preset 返回 null）+ `gbufferGeometryProgram()` / `deferredLightProgram()`（从 `RenderPipelineBuilder` 搬来；builder 的两个静态工厂改为**转发**，公开 API 不变） |
| 后端 | `VsgPipelineFactory::compiledStages(preset)`：取 `builtinProgram(preset)` 的 stages 编译（**每 preset 缓存一次**，空则 decline）；`buildVineShaderSet` 不再自带 GLSL、不再用 `preset != StandardPhong` 硬判 |
| 判据 | **行为中性**：两条证据基线 47 行逐字节不变；`vine_shader_check.sh` PASS（7 shader）；test_graphics 234 → **235**（+1：`builtinProgram(StandardPhong)` 用嵌入源、Pbr/ShadowedPhong 返回 null）、test_vsg 237 → **238**（+1：vsg 表**不含** `vine_forward.*`，钉住归属边界）；lavapipe PASS |

**边界**：SDK 拥有**着色文本**（L3）；后端拥有**编译 + ABI 绑定 + 管线**（L2）。`ShaderSet` 仍是 vsg 后端内部机制，不进 SDK。下一步（P0.B）：把 ABI 契约（属性角色 / `VineFrame`・`VineDraw` / 参数・槽表）也移到 SDK，为换后端铺路。


