# Vine 自写 shader 设计（custom-shader P0/P1 落地稿）

> 状态：设计稿 v1.1（2026-09-03）
> **方向确认（SDK 第一准则）**：用户必须能写 GLSL；SDK 着色契约先于后端，vsg（乃至手写
> Vulkan）只是可替换实现。**内置 program 与用户 Program 是同一个模型**（2026-09-13：枚举
> `ShaderPreset` 已删除，详见 `vsg-custom-shader.md` §11.10）；多 pass 的意义依赖用户
> 可编程着色（pass 级 Program + 命名产出槽）。契约草案见 **§11**。本稿 §4 push-constant 矩阵机制
> 受其影响，已标注"待契约化修订"。
> 上游：`vsg-custom-shader.md`（总纲 + 着色语义 §8 / 显式 program §11.10）；
> `graphics-lighting.md`（v4a：Scene 级 Light + `RenderBackend::setLights`）；
> `graphics-shadow.md`（v4b-1：depth-only RT 已落地；v4b-2 采样=本设计的 ShadowedPhong 切片）；
> `graphics-render-pipeline.md`（v3：命名产出槽 publish/resolve）。
>
> **一句话**：把 `vsg-custom-shader.md` §4 的草案变成**可落地的自写 VS/FS + UBO 契约 + 集成点**——
> push-constant 矩阵 + 我们自己的 set0(帧/光)/set1(材质) 布局替换 vendored phong/flat blob；
> GLSL 源码仓库化，SPIR-V **离线预编译嵌入**（无运行时 glslang 依赖，工程已验证该路线渲染正确）。

## 0. 现状核对（以代码为准，2026-09-03）

| 层 | 现状 | 本设计改向 |
|---|---|---|
| SDK 语义 | `ShaderProgram`（`forwardProgram()` / `flatForwardProgram()` + 用户自己写的），RenderEngine 持有并转发 `RenderBackend::setDefaultContentProgram`；**枚举 `ShaderPreset` 已删除** | 不变 |
| vsg 着色 | `VsgRenderer.cpp::buildShaderSet()` → `createPhongShaderSet()/createFlatShadedShaderSet()`（Pbr/Shadowed 回落 Phong） | 换 `buildVineShaderSet()`（自写 SPIR-V） |
| 几何桥 | `SceneBridge::buildGeometry()`：每几何 `GraphicsPipelineConfigurator`；属性 `vine_Vertex/Normal/Color`；描述符 `"material"`=`PhongMaterialValue`（`VsgMaterialManager` 缓存）；blend 常开；opacity 走 per-vertex alpha | 属性不变；描述符换 `"vine_material"`（我们的 UBO） |
| 光源 | v4a：`RenderPass::execute→setLights`；vsg 每视图转 `vsg::Light` 节点 → VDS lightData | 改为每帧打包 `LightsUBO`（world space），不再建 vsg::Light 节点 / 不再依赖 VDS |
| 编译路径 | vendored phong = 内嵌 SPIR-V blob；无 glslang → 运行时 ShaderCompiler 不可用 | 自写 GLSL 离线预编译 `.spv`，生成 `VineShaders.cpp` 内嵌（同 vendored phong 模式） |
| 已有痕迹 | `shaders/flat.vert/.frag` + 提交的 `.spv`（push-constant 矩阵机制的验证起点） | 收编进正式 shader 目录与 CMake |

## 1. 目标 / 非目标

**P0 目标（本稿范围）**
1. 自写 `StandardPhong` VS/FS，替换 vendored phong，主场景几何 A/B 像素一致；
2. `FlatShaded` 走**同一 shader 族的 unlit 开关**（FrameUBO flag），不再需要第二套 shader / 第二类管线；
3. 光照数据上游全归我们：FrameUBO + LightsUBO 每帧打包（内容 scene 有光用内容光；无光回退默认
   headlight+ambient，零观感回归）；
4. 布局/契约是**后端无关 ABI**（`vsg-custom-shader.md` §4 同款），为 Diligent / 自研后端留缝。

**非目标（本稿不落地）**
- PBR（`Pbr` 预留）、阴影采样（`ShadowedPhong` = P1，消费 v4b-1 depth RT，§6 给 ABI 接缝）；
- 纹理贴图**已落地（L0–L3）**：`Material::texture()` 的 face 0 上传为 vsg 图像、建 sampler、进描述符集（set0/binding1），
  `Geometry::setTexcoords2()` 经 location 8 喂 `vine_TexCoord0`；cube map 也已落地：六面作为 6 层上传（层数经 Data 的 `depth` 传给 vsg），按 `CubeMap::Face` 顺序采样有像素断言；未做的只剩各向异性过滤。多光（LightsUBO 数组已按 4 预留，但只验
  ambient + 单方向光）、材质 dynamic UBO（P2）。
- 用户可编程 Program / pass 级自定义着色：**方向已确认**（§11，SDK 第一准则），P0 后实现；
  P0 内置 shader 须与它同契约，避免两套机制。

## 2. 必须先钉死的 vsg 机制事实（设计前提）

1. **矩阵走 push constants**：vsg RecordTraversal 对声明了
   `layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; }` 的顶点 shader
   自动按 draw 写入 `projection`（视图相机）与 `modelView`（= view × 逐节点累积 model）。
   `shaders/flat.vert` 尖刺已证明该语法可行。**前提**：pipeline layout 的 push 范围来自
   `ShaderSet.pushConstantRanges`（已核对 vsg 头：字段 + `addPushConstantRange` 存在）→ 自写
   ShaderSet **必须显式 `addPushConstantRange("PushConstants",...,VERTEX,0,128)`**，且与 GLSL block
   名/offset/size 逐字节一致，否则矩阵不会推入（画面不渲）。内置 phong/flat 能渲 = 该机制对
   ShaderSet 管线成立；**P0 spike 1 须在 lavapipe 确认我们自建 ShaderSet 也能吃到这两个矩阵**
   （本设计最大单点风险）。
2. **VDS 完全不介入**：只要 ShaderSet 不声明 `lightData/viewportData/shadowMaps` 名字，vsg 就不会
   BindViewDescriptorSets set0（`makeScreenTextureNode` 先例已验证）。我们因此独占 set0/set1 布局。
3. **顶点属性名不变**：`vine_Vertex(0,vec3) / vine_Normal(1,vec3) / vine_Color(2,vec4)` →
   `SceneBridge::assignArray` 与顶点缓冲映射**零改动**。
4. **无 glslang**：运行时 ShaderCompiler 不可依赖 → GLSL 离线编译成 `.spv` 提交 + CMake 内嵌
   （vendored phong 同款，工程已实测正确）。运行时若 `compiler->supported()` 为真可作调试加速，但
   不作为主路径。
5. **材质 opacity 现状**：diffuse.a 恒为 1，有效透明度乘在 per-vertex `vine_Color.a`（SceneBridge
   逐帧只在该值变化时重写 O(V)）。因此自写 FS **必须保留 `final *= vine_Color` 语义**，且 alpha 通道
   取自 `vine_Color.a × diffuse.a`，否则透明度/混合与 A/B 全崩。

## 3. 光照坐标系决策

**采用世界空间光照**（而非 vsg phong 的视图空间）：
- Vine `Light::direction()`、`Camera`（eye/center/up）本来就是世界坐标，Scene 语义直通，**无需在
  CPU 每帧把光方向转视图空间**；
- P1 阴影的 `shadow_matrix`（lightProj×lightView，世界→光裁剪）与世界空间 receiver 天然一致
  （`vsg-custom-shader.md` §4.2 约定），不用在视图/世界两套坐标系间来回倒；
- VS 里用 FrameUBO 的 `inv_view` 由 `modelView` 重建世界坐标/法线（每顶点两次 mat3 乘，代价可忽略）。

> 与现状观感一致性的保障：光照对旋转不变，只要 N/L/V 三向量同空间、公式同款，视图/世界空间结果
> 数学等价；A/B 像素对照只用于校准具体公式常量（见 §8 spike 2）。

## 4. Shader ABI（最终布局）

### 4.1 描述符 / push-constant 布局

```
push     每 draw    { mat4 projection; mat4 modelView; }          // vsg 按 draw 写入
set0
  b0  FrameUBO     (view/inv_view/proj/view_proj/cam_pos/frame)  // per-bridge 共享，每帧打包
  b1  LightsUBO    (count + VineLight[4])                         // per-bridge 共享，打包自 Scene.lights()
  b2  shadow_map   sampler2DShadow                                // P1 才绑（自身 depth RT）
set1
  b0  MaterialUBO  (base_color/specular/ambient/emissive/shininess/flags)  // P0 独立小 UBO
```

所有 binding 名 `vine_*` 前缀，规避 vsg view-dependent 名。

**绑定机制（P0，走 proven 路径）**：不做"view 根 StateGroup + 跨几何共享 set0"——那是 vsg 仅在
声明了 view-dependent 名时自动干的事，自己绑 set0 需跨所有几何管线 DescriptorSetLayout 兼容、未验证。
改为**逐几何自足**（与现状 material 完全同构）：每个几何的 `GraphicsPipelineConfigurator` 把
`vine_frame`/`vine_lights`/`vine_material` 都 `assignDescriptor` 进**它自己的 descriptor set**
（set0: frame/lights；set1: material），其中 frame/lights 指向 **per-bridge 共享**的 Data-backed
UBO（main/overlay/offscreen 各一份，因各自相机/光不同）；每帧在
render()/renderOffscreenTarget()/renderOverlayPass() 内用该 pass 的 camera + pending_lights 原地
改写共享 UBO → 复用 vsg Data 上传机制（与 SceneBridge 每帧刷 material 同款，仓库已验证 live edit
生效）。管线 layout 由各自 ShaderSet 派生，无跨管线兼容问题。

> ⚠ **契约化修订（§11，SDK 第一准则）**：上面"push 矩阵"是 vsg 实现细节，不是跨后端契约。用户
> 可编程后，契约应提供**声明式 per-view `VineFrame(view/inv_view/proj/view_proj/cam_pos)` 与
> per-draw `VineDraw(model + 用户参数)` 块**，用户据此算 `gl_Position = view_proj × model × pos`；
> push 仅作后端内部优化（须与契约可证等价）。P0 内置 shader 与用户 Program 走同一契约，避免
> 两套机制。

// set0 b0 —— 每帧
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;        // 备用（CPU 打包与调试）
    mat4 inv_view;    // VS 用：世界坐标/法线重建
    mat4 proj;        // 备用
    mat4 view_proj;   // 备用
    vec4 cam_pos;     // 世界相机位置（specular / 将来 point light）
    vec4 frame;       // x=time(s, 取 vsg FrameStamp), y=viewport_w, z=viewport_h, w=flags(bit0 unlit)
} vine_frame;         // 320 B

// 每帧一光
struct VineLight {                       // std140 偏移
    vec4 dir_and_type;    // 0..16   xyz=世界方向, w=0 ambient / 1 directional（2/3 point/spot 预留）
    vec4 color_intensity; // 16..32  rgb 颜色, a=intensity 倍数
    vec4 shadow_params;   // 32..48  x=1 投影使能; y=bias, z=filter, w=分辨率（P1）
    mat4 shadow_matrix;   // 48..112 P1: lightProj×lightView（世界→光裁剪）；零矩阵=无
};                        // 112 B（对齐 16；数组 stride = 112）

// set0 b1 —— 每帧
layout(set = 0, binding = 1) uniform LightsUBO {
    uint count;           // 有效光数 [0..4]（offset 0，占满一个 vec4 slot → lights 从 16 起）
    VineLight lights[4];
} vine_lights;            // 16 + 4×112 = 464 B（C++ 打包结构须逐字段镜像上表，避免 std140 错位）

// set1 b0 —— 每 drawable（P0 独立小 UBO，P2 收敛 dynamic）
layout(set = 1, binding = 0) uniform MaterialUBO {
    vec4 base_color;      // diffuse RGBA；a 恒 1（opacity 走 per-vertex alpha）
    vec4 specular;        // rgb specular 色, a 备用
    vec4 ambient;         // rgb ambient
    vec4 emissive;        // rgb（预留）
    float shininess;      // Phong 指数
    uint  flags;          // 预留：将来 per-material unlit/emissive 等（P0 FlatShaded 全局开关走 frame.w bit0）
} vine_material;
```

### 4.3 顶点 shader（vine_phong.vert）

```glsl
#version 450
layout(location = 0) in vec3 vine_Vertex;
layout(location = 1) in vec3 vine_Normal;
layout(location = 2) in vec4 vine_Color;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_normal_world;
layout(location = 2) out vec3 v_pos_world;

layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;
layout(set = 0, binding = 0) uniform FrameUBO { /* 见 4.2 */ } vine_frame;

void main()
{
    vec4 mv_pos = pc.modelView * vec4(vine_Vertex, 1.0);
    gl_Position = pc.projection * mv_pos;
    vec4 w = vine_frame.inv_view * mv_pos;
    v_pos_world = w.xyz / w.w;
    v_normal_world = normalize(mat3(vine_frame.inv_view) * mat3(pc.modelView) * vine_Normal);
    v_color = vine_Color;
}
```

> 法线 = mat3(inv_view)×mat3(modelView)×n，即 model 的旋转/等比缩放部分——与 vendored phong 的
> 视图空间近似同级；非等比缩放会歪（现状亦如此；Vine 不支持非等比 drawable 缩放，不阻塞）。

### 4.4 片元 shader（vine_phong.frag，世界空间 Blinn-Phong）

```glsl
#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_normal_world;
layout(location = 2) in vec3 v_pos_world;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform FrameUBO   { /* 见 4.2 */ } vine_frame;
layout(set = 0, binding = 1) uniform LightsUBO  { /* 见 4.2 */ } vine_lights;
layout(set = 1, binding = 0) uniform MaterialUBO{ /* 见 4.2 */ } vine_material;

void main()
{
    vec3 base = vine_material.base_color.rgb * v_color.rgb;   // 保 vine_Color 乘法语义
    if ((uint(vine_frame.frame.w) & 1u) != 0u) {               // unlit(flat)，全局开关走 FrameUBO
        out_color = vec4(base, vine_material.base_color.a * v_color.a);
        return;
    }
    vec3 N = normalize(v_normal_world);
    vec3 V = normalize(vine_frame.cam_pos.xyz - v_pos_world);
    vec3 color = vec3(0.0);
    for (uint i = 0u; i < vine_lights.count; ++i) {
        VineLight L = vine_lights.lights[i];
        if (int(L.dir_and_type.w) == 0) {                       // ambient
            color += L.color_intensity.rgb * L.color_intensity.a
                   * vine_material.ambient.rgb;
        } else if (int(L.dir_and_type.w) == 1) {                // directional
            vec3 dir = normalize(L.dir_and_type.xyz);
            float ndl = max(dot(N, dir), 0.0);
            color += L.color_intensity.rgb * L.color_intensity.a
                   * (vine_material.base_color.rgb * ndl);
            // specular（Blinn-Phong；与 vendored phong 对照后校准，见 §8 spike 2）
            vec3 H = normalize(dir + V);
            color += L.color_intensity.rgb * L.color_intensity.a
                   * vine_material.specular.rgb
                   * pow(max(dot(N, H), 0.0), vine_material.shininess);
        }
    }
    out_color = vec4(color, vine_material.base_color.a * v_color.a);
}
```

> 上述公式是**目标型默认**。P0 spike 2 须对着 vendored phong FS（vsg 1.1.16 自带、可读）逐行
> 校准：ambient 是否乘 material.ambient、specular 的 half-vector vs 反射向量、vertex color 乘在
> 哪一级——以 A/B 像素一致为准冻结常量，不留"看似对"。

## 5. 集成点（逐处点名）

| 文件/符号 | 改动 |
|---|---|
| `shaders/`（新目录，收编 flat.vert/.frag 实验） | 放 `vine_phong.vert/.frag` 源 + 提交 `.spv`；CMake 有 `glslangValidator` 时重编译 |
| `VsgRenderer.cpp::buildShaderSet()` | → `buildVineShaderSet(preset, extent, depth_test)`：内嵌 SPIR-V 建 `ShaderSet`（stages + attributeBindings 与 SceneBridge 一致 + set0/set1 descriptorBindings + 与今天相同的 default pipeline states）。按 `(preset, depth_test)` 缓存复用 |
| `VsgRenderer.cpp` 三处调用（主 784 / overlay 792 / 离屏 1178） | 换用 `buildVineShaderSet` |
| `VsgRenderer` per-bridge 帧/光 UBO | 每个 bridge（main/overlay/offscreen）持共享 `FrameUBO`/`LightsUBO` Data（注入 SceneBridge、随几何 assignDescriptor，见 §4.1）；在 render()/renderOffscreenTarget()/renderOverlayPass() 入口用该 pass 的 `camera` + `pending_lights` **原地改写**（不能笼统"帧首"——相机/光到这些入口才确定）。time 取 vsg FrameStamp；unlit 位由 `d->shader_preset` 打包。**不再建 vsg::Light 节点**，`setLights` 仅排队（现状已如此）。无内容光 → 填默认 headlight 等价方向光（CPU 用 view 逆旋转 (0,0,-1) 得世界方向）+ ambient，零观感回归 |
| `VsgMaterialManager` | `getOrCreate()` 返回值类型 `PhongMaterialValue → VineMaterialValue`（UBO 描述符缓存）；`updateMaterial/releaseMaterial/clear/forEach` 等接口名不变 |
| `SceneBridge::buildGeometry` | 描述符改名：`"material"` → `"vine_material"`（set1 b0），并新增把共享 `"vine_frame"`(set0 b0)/`"vine_lights"`(set0 b1) assignDescriptor 进同一几何 descriptor set（引用注入的 per-bridge 共享 Data，见 §4.1）；`shader_set_` 缺省回退 `buildVineShaderSet(StandardPhong,...)` |
| `SceneBridge` 材质逐帧刷新循环 | 从 `m.diffuse/...` 写 `PhongMaterialValue` → 写 VineMaterial 结构（base_color a=1，opacity 仍走 per-vertex alpha，机制不动） |
| `RenderPass::execute / RenderEngine` | 不变（内容光已在 `render()` 前经 `setLights` 到达后端） |
| `RenderPipelineBuilder` | **不碰**：preset 是着色轴，与 pass 拓扑正交（既定决策） |

## 6. ShadowedPhong 接缝（P1，只留 ABI 不实现）

- 采样用 set0 b2 `sampler2DShadow`（v4b-1 的 depth-only RT 已可读），bias/filter 取
  `Light::ShadowSettings`（已存在）；`VineLight.shadow_matrix` 装 lightProj×lightView；
- `StandardPhong` 与 `ShadowedPhong` 共享同一 shader 族：FS 以 `shadow_params.x` 判断是否有影，
  有则 world 坐标过 `shadow_matrix` → [0,1] 深度比较（Hard/PCF）；
- 前置 = P0 完整跑绿 + 多 pass 成熟（既定路线，shadow 排最后）。

## 7. GLSL 仓库化与编译

> ⚠ 本节已被 `vsg-custom-shader.md` §10 取代（真文件 + 构建期嵌入，**不**提交 `.spv`；SDK 侧
> 2026-09-13 起含内建前向着色 `builtin_forward.*`——同日先后叫过 `std_forward.*` / `vine_forward.*`，
> 改名理由见 `.ai/design/vsg-custom-shader.md` 顶部 2026-09-13 命名规则）。以下保留作历史记录。

- 源：`src/plugins/gfx_backend_vsg/shaders/*.vert|.frag`（仓库提交；**该目录已于 2026-09-13 删除** —— 全屏三角形 / 屏幕拷贝归 SDK，见 `vsg-custom-shader.md` §11.11）；
- 产物：同目录 `*.spv`（提交）；生成 `VineShaders.cpp`（内嵌字节数组，方式同 vendored phong 的
  `phong_ShaderSet.cpp`），运行时 `vsg::ShaderStage::create(stage,"main",code,size)`；
- CMake：`find_program(glslangValidator)`，命中则对源重编译 `.spv` 并重新生成内嵌 `.cpp`
  （开发期改 shader 无需手工跑命令）；未命中则直接用提交的 `.spv` → **任何机器都保证能编能跑**；
- 顶点/片元接口变量名必须逐字一致（glslang 链接要求，`makeScreenTextureNode` 已踩过）。

## 8. 分期与验收

- **P0.0 spike（单点风险先破）**：用自写 VS/FS（flat.vert/.frag 已备）+ 离线 `.spv` 在 lavapipe
  渲出纯色三角 → 确认 (a) push-constant 矩阵被 vsg 自动写入、(b) 内嵌 SPIR-V 加载路径通。
- **P0.1 主场景平替 + A/B**：`buildVineShaderSet` 只接主场景几何；临时 env（如 `VINE_VSG_USE_BUILTIN`）
  在自写/内建间 A/B；同相机同场景截图（沿用 xwd 抓图 + XWD→PNG 自解码）像素一致 → 冻结公式。
- **P0.2 光路切换**：FrameUBO/LightsUBO 每帧打包；有内容光用内容光、无光回退默认；主/离屏 PiP 同源
  （沿用 `VINE_VSG_OFFSCREEN=1`）；`VINE_SHADER_PRESET` 切 FlatShaded = 仅 unlit flag，无管线重编。
- **P0.3**：overlay/axis 迁移可选项；`raw_layout.txt` 之类临时诊断移除。
- **P1**：ShadowedPhong 采样（v4b-2），消费 v4b-1 depth RT + §6 ABI。
- GraphicsTest 不依赖后端 → 保持不变；SDK 侧新增只测数据契约（打包函数/空光回退规则），不碰 vsg。

## 9. 决策记录（2026-09-03）

1. **世界空间光照**：N/V/L 三向量与 Scene/Camera/Light 语义、P1 阴影矩阵同一空间，免视图/世界互转。
2. **单 shader 族 + unlit flag 覆盖 FlatShaded**：`FlatShaded` 不再需要第二套 shader/管线；unlit
   走 **FrameUBO.frame.w bit0**（per-bridge 每帧打包时由 preset 置位）→ preset 切换 = 一次 uniform
   写，无重编、无材质缓存失效。MaterialUBO.flags 留作将来 per-material 特性。Pbr/Shadowed 后续各自
   加 FS 变体或扩展字段。
3. **离线预编译 SPIR-V 内嵌**：无 glslang 也可编可跑（vendored phong 同款，已实测）；运行时
   ShaderCompiler 仅作调试加速。
4. **矩阵仍走 vsg push constants**（不加每 draw model UBO）：自写 ShaderSet 显式
   `addPushConstantRange("PushConstants",VERTEX,0,128)`（与 GLSL block 名/offset/size 一致）；P0
   最小化状态切换；P2 再评估材质 dynamic UBO（§4.4 遗留路线）。
   ⚠ **待契约化修订（§11，SDK 第一准则）**：契约 = 声明式 `VineFrame.view_proj`/`VineDraw.model`
   块；push 降级为后端内部优化。此决定随用户 Program 切片重估（per-draw 需一块 model UBO，P2
   用 dynamic UBO 收敛）。
8. **set0 逐几何自足绑定（P0）**：frame/lights 与 material 一样逐几何 assignDescriptor（引用
   per-bridge 共享 Data），复用仓库已验证的 Data 上传 / live-edit 路径；不做 view 根 StateGroup 跨
   管线 set0（P2 再评估）。
5. **保留 `final *= vine_Color` 与 per-vertex alpha opacity 机制**：动它 = 透明度/混合全崩。
6. **材质描述符名 `vine_material`**：避免与 `"material"`（PhongMaterialValue 绑定）混淆导致
   `assignDescriptor` 名字不匹配。
7. **FrameUBO 带 `inv_view` 而非每 draw model 矩阵**：模型矩阵仍由 vsg 经 modelView 提供，世界坐标
   用 inv_view×modelView 重建。

## 10. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 自定义 ShaderSet 的 push-constant 不被填充（未显式声明范围） | ShaderSet 显式 `addPushConstantRange("PushConstants",VERTEX,0,128)` 且与 GLSL 一致；内置 phong 能渲 = 机制成立；P0.0 尖刺先破 |
| set0 跨几何绑定的 DescriptorSetLayout 兼容坑 | P0 逐几何自足：frame/lights 像 material 一样 assignDescriptor（per-bridge 共享 Data），无跨几何共享 set |
| FrameUBO 数据源错位（"帧首"更新拿不到相机） | 更新点收敛到 render()/renderOffscreenTarget()/renderOverlayPass()（该 pass 相机+光已确定）；time 取 vsg FrameStamp，不新增 SDK 传时通道 |
| A/B 观感差异（ambient 公式/vertex color 乘位/specular 模型） | P0.1 用 vendored phong FS 逐行对照 + 像素 A/B 校准，不靠目测 |
| 无 glslang 导致改 shader 需外部工具 | CMake 检测 `glslangValidator` 自动重编译；未命中用提交 `.spv`（改完必须提交新 `.spv`） |
| `LightsUBO` 打包与默认光回退破坏现状观感 | P0.2 沿用 demo 环境门控 A/B（同 v4a 做法） |

## 11. 用户可编程着色契约（方向已确认，SDK 第一准则）

> ⚠ 本节为过渡草案；正式设计稿见 `graphics-shader.md`（薄接口版，取代本节的"契约六要素"表述）。
> 状态：2026-09-03 方向定稿。本节是**契约草案（评审对象）**；P0 只落地"内置 Program 走同一
> 契约"，用户 Program / pass 级自定义着色在 P0 完成后实现。SDK 第一准则：语义/契约先于后端，
> vsg（乃至自写 Vulkan）只是把契约翻译成管线的可替换实现——即使弃用 vsg，本节契约不变。

### 11.1 为什么是必需品

多 pass 的价值 = 每个 pass 的着色逻辑可编程。若用户不能写 shader，ScreenPass 只能跑引擎写死的
全屏拷贝，publish/resolve 退化为"拷来拷去"，deferred/SSAO/泛光/自定义合成全做不了。因此"用户
写 GLSL"是管线架构需求，不是附加功能。

### 11.2 契约六要素（SDK 提供、后端保证、用户对着写）

1. **着色单元 `ShaderStage`**：`source + entry`（先 VS/FS；compute/geometry 留缝）。SDK 只承载
   不解析；后端编译（GLSL→SPIR-V，离线/运行时）。
2. **属性 location 表**：固定 `0=position 1=normal 2=color 3=uv0 …` + 语义说明；用户 GLSL 按
   location 声明、变量名随意；自定义数据（点云等）走新增通道，不挤占内置槽位。
3. **声明式数据块**（跨后端契约，取代 push 作为对外接口）：
   - per-view `VineFrame{ view; inv_view; proj; view_proj; cam_pos; frame(time/viewport/flags) }`
   - per-draw `VineDraw{ model; …用户参数表… }`（参数表布局由 SDK 推导，用户不碰字节）
   用户据此写 `gl_Position = view_proj × model × pos`；push 只允许作后端内部优化。
4. **类型化参数表**：Program `add/set/get` typed 参数（float/vec/mat/int/纹理槽…）；
   MaterialUBO 是它的固定特例。
5. **pass 级 Program + 命名产出槽**：Program 声明消费的 sampler 槽（名字 = 引擎 publish 的输出
   `SceneColor/Depth/ShadowMap/…`），engine resolve → 后端绑 sampler；输出再 publish。← 多 pass
   意义所在。
6. **编译/排错/热更**：SDK 定义编译接口与错误上报；支持"改 shader → 重载"迭代闭环。

### 11.3 与内置预设同构

`ShaderPreset` 当时被规划为"内置 Program 的别名"；**2026-09-13 实际落地时直接删掉了这个别名**
（内置着色就是两个 Program 工厂：`forwardProgram()` / `flatForwardProgram()`）。用户 Program 是同一模型
的一等公民：可覆盖几何（Drawable）或挂在 pass 上，也可作为会话的内容 program。不搞两套着色机制。

### 11.4 示例（供评审）

挂 Drawable 的自定义材质 VS（读声明式块）：

```glsl
#version 450
layout(location = 0) in vec3 position;
layout(set = 0, binding = 0) uniform VineFrame { mat4 view; mat4 inv_view; mat4 proj;
                                                mat4 view_proj; vec4 cam_pos; vec4 frame; } vine_frame;
layout(set = 1, binding = 0) uniform VineDraw  { mat4 model; /* 用户参数表 */ } vine_draw;
void main() { gl_Position = vine_frame.view_proj * vine_draw.model * vec4(position, 1.0); }
```

挂 pass 的全屏用户 shader（多 pass 意义所在）：

```glsl
#version 450
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D in_SceneColor;   // 槽名=引擎命名产出
layout(set = 0, binding = 1) uniform sampler2D in_Depth;
layout(set = 1, binding = 0) uniform VinePost { float exposure; vec4 tint; } params;
layout(location = 0) out vec4 out_color;
void main() { /* 读前序 pass 输出，写本 pass 结果 */ }
```

### 11.5 对 P0 本稿的影响（标注）

- §4.1 / §4.3 / §9.4 的 push-constant 矩阵机制 → **契约化修订**（见 11.2.3）：对外改声明式块。
- P0 内置 shader 也走声明式块 → VS 读 `VineDraw.model` + `VineFrame.view_proj`；代价 = per-draw
  多一块 model UBO（原用 push 省状态切换）——按 SDK 第一准则接受，P2 以 dynamic UBO 收敛。
- 属性以 location 表对齐；vsg 侧 `vsg_*` 名字仅作绑定别名，在 ShaderSet attributeBindings 层归一。
- 上述修订随用户 Program 切片实施时一并进行；届时更新 §4/§9/§10 相应条目。
