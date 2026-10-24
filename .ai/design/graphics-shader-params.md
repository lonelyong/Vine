# graphics 着色参数 ABI：Schema、名字与后端契约

> **状态**：提案（已定，未实现；实现进度见 `../backlog/graphics.md`）
> **最后核对**：2026-10-24
> **范围**：view / draw / material / shadow 四个参数块的布局来源、名字解析、后端写值契约。
> **权威来源**：`ShaderAbi.hpp`（现行块布局）/ 本文件（目标形态）/ 实现后的 params 清单文件。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

四个参数块已经在 SDK 里，偏移由 `static_assert` 钉死（`VineViewBlock` 288 B、`VineDrawBlock` 80 B、
`VineMaterialBlock` 64 B、`VineShadowBlock` 80 B）。缺的是四件事：

- **名字只存在于散文**：`VineDrawBlock::params.x` 是 opacity，这条约定写在注释里，代码里没有名字；
- **GLSL 侧是手抄的**：块声明写在内置 shader 里，与 C++ 结构体一致靠人；
- **后端直接写自己的存储**：没有“写字节”的契约，第二个后端要把打包逻辑重写一遍；
- **用户参数槽无名**：`params.yzw` 是保留槽，宿主无法按名字使用。

一个后端时这些都不是问题；第二个后端到来时，“有哪些参数、在哪个偏移”会各自长一份。

### 1.2 目标

- 一条真相生成三份产物：C++ 结构体、GLSL 声明、名字/偏移表。
- 名字只在接线期存在，一次解析成 `{buffer, offset, type}`；热路径只有偏移。
- 后端只回答“怎么写字节”。
- 未知名字在三处报错，而不是静默填零。

### 1.3 非目标

- 不做材质 DSL（自有语言 + 编译器）。
- 不做运行时反射 / 按名自动填充。
- 不做松散 uniform（Vulkan 不允许，见 P1）。
- 不做逐 draw 参数惰性上传、specialization constant 优化。

### 1.4 一句话结论

> Schema 是真相，生成是手段，偏移是运行时，后端只是出口。

## 2. 设计要点

### 2.1 唯一真相

参数清单只在一处手写（`params/vine_params.txt`），由生成器产出 C++ 头、GLSL 文本与偏移表。
**手抄的 GLSL 块声明和手写的名字常量都不再存在。**

### 2.2 频率定缓冲

| 缓冲 | 内容 | 上传时机 |
|---|---|---|
| view | 相机矩阵、视口尺寸、时间 | 每 view |
| draw | 模型矩阵、不透明度、点大小、用户槽 | 每 draw（ring + dynamic offset） |
| material | 环境 / 漫反射 / 高光 / 光泽 | 每材质（去重后） |
| shadow | 光空间矩阵、阴影参数 | 每 pass |

逐顶点与逐实例数据**不是参数**：位置 / 法线 / 颜色 / UV 走 canonical attribute，自定义通道保留源
location（≥ 3，避开 8 与 12–15，见 `ShaderAbi.hpp`）。

### 2.3 名字只在接线期

`find("Draw.Opacity")` 返回 `{Buffer::Draw, 64, Type::Float}`；`Material` 与 `StateNode` 缓存解析结果，
之后的每次写入都是按偏移写字节。名字是 `块.字段`，跨块唯一。

### 2.4 GLSL 由生成器产出并注入

生成的声明片段在编译前插到 `#version` 之后、用户代码之前。内置与用户 shader 看到同一套 `vn_*` 名字：
拼错 → 未声明标识符（编译失败）；重复声明同名块 → 重定义（编译失败）。两个失败都是可见的。

### 2.5 后端只写字节

后端契约只有两个动作：按缓冲写一段字节、按缓冲刷一段范围。值怎么算、什么时候算不在后端，
在 `packDrawBlock` 这类引擎侧函数里。

### 2.6 P0 逐字节不变

第一阶段验收：生成的 C++ 结构与今天的 `ShaderAbi.hpp` 在尺寸、对齐、成员偏移上完全一致，
既有测试一行不改就能过。**先证明等价，再谈新能力。**

### 2.7 散文约定变成具名成员

`params.x` / `cam_pos.w` / `frame.w` 这类“vec4 里第几个分量是什么”的约定，在 GLSL 侧变成具名成员
（`vn_opacity` / `vn_pointSize`）；C++ 侧字节布局不变，名字与偏移由生成器保证一致。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 |
|---|---|---|
| F1 | Schema 文件 | 一行一个字段：缓冲、名字、类型、可选分量位置 |
| F2 | 生成的 C++ | 块结构体 + `static_assert` + 名字表 |
| F3 | 生成的 GLSL | 具名成员块声明，编译前注入 |
| F4 | 名字解析 | 一次解析、缓存、错误可见 |
| F5 | 后端契约 | `ParameterSink` 的写与刷 |
| F6 | `Material` 按名设值 | 类型化重载 + 既有字段糖保留 |
| F7 | 用户扩展点 | draw 块的具名用户槽 + 保留 binding 段 |
| F8 | 门禁 | 生成物一致、未知 `vn_` 名字、布局单测 |
| F9 | P0 等价性 | 与现行 `ShaderAbi.hpp` 逐字节一致 |

### 3.2 F1 Schema 文件

```
# 缓冲    名字        类型    位置        说明
view     view        mat4
view     cam_pos     vec4                xyz = 世界坐标，w 保留
view     frame       vec4                x = 时间，y/z = 视口尺寸，w = flags
draw     model       mat4
draw     opacity     float   params.x
draw     point_size  float   params.y
draw     user_slot0  float   params.z
draw     user_slot1  float   params.w
material ambient     vec4
material diffuse     vec4
material specular    vec4
material shininess   float
shadow   view_to_light mat4
shadow   params      vec4                x = enabled，y = bias，z = strength，w = light
```

`位置` 列只在“一个 vec4 里放多个标量”时出现；它是为了保留今天的字节布局，不是新的自由度。

### 3.3 F2–F3 生成的产物

```cpp
struct alignas(16) VineDrawBlock
{
    std::array<float, 16> model{};
    float opacity{1.0F};
    float point_size{1.0F};
    float user_slot0{0.0F};
    float user_slot1{0.0F};
};
static_assert(sizeof(VineDrawBlock) == 80u);

inline constexpr ParameterDesc kParameters[]{
    { "Draw.Opacity",   Buffer::Draw, offsetof(VineDrawBlock, opacity),    Type::Float },
    { "Draw.PointSize", Buffer::Draw, offsetof(VineDrawBlock, point_size), Type::Float },
};
```

```glsl
layout(set = 2, binding = 1, std140) uniform VineDrawBlock
{
    mat4  vn_model;
    float vn_opacity;
    float vn_pointSize;
    float vn_userSlot0;
    float vn_userSlot1;
};
```

字节布局两边相同，名字两边各自可读——这是“生成”相对“手抄”最直接的收益。

### 3.4 F4 名字解析

`ParameterTable::find(name)` 是唯一入口：在生成的名字表里查一次，返回缓冲、偏移与类型；
`set()` 同时校验类型，类型不符即错误。解析结果挂在 `Material` / 命令上，不进入每帧循环。

### 3.5 F5 后端契约

```cpp
class ParameterSink
{
  public:
    virtual void write(ParameterBuffer buffer, std::span<const std::byte> bytes) = 0;
    virtual void flush(ParameterBuffer buffer) = 0;
};
```

Vulkan 后端实现为映射 ring + dynamic offset；别的后端实现成 cbuffer / `glUniform*`。框架层不出现任何
后端词汇。

### 3.6 F6 `Material` 按名设值

`Material::set("Material.Diffuse", color)` 走名字表；`setDiffuse()` / `setSpecular()` 等既有字段糖保留，
内部走同一个入口。未知名字与类型不符都在 `set()` 返回错误，不进入渲染。

### 3.7 F7 用户扩展点

- **具名用户槽**：`Draw.UserSlot0` / `Draw.UserSlot1`，宿主可为每个 drawable 设两个 float；
- **保留 binding 段**：框架占用一段 set/binding，用户自定义块只能落在保留段之外；占用即错误。

不做“用户随便写个 `vn_` 名字框架自动填”——见 D1。

### 3.8 F8–F9 门禁与等价性

- 生成物一致：重新运行生成器不得产生差异；
- 未知 `vn_` 名字：内置与用户 shader 里出现表外的 `vn_*` 标识符即失败；
- 布局单测：一 Matrix 化的断言表覆盖每个字段的偏移与类型；
- 等价性：P0 生成的头与现行结构体逐字节相同，`BlockStorageTest` / `StateNodeTest` 不改。

## 4. 公开接口

### 4.1 Schema 语法

空白分隔的列：缓冲、名字、类型、可选位置、可选说明；`#` 起注释。类型集合固定为 `float` / `vec2` / `vec3` / `vec4` / `mat4` / `uint`，每一种都有确定的 std140 对齐。

### 4.2 生成的 C++ 类型

四个块结构体（`VineViewBlock` / `VineDrawBlock` / `VineMaterialBlock` / `VineShadowBlock` 保持原名）、
`ParameterBuffer` 枚举、`ParameterRef{buffer, offset, type}`、`find()`。

### 4.3 运行时 API

| 接口 | 契约 |
|---|---|
| `ParameterTable::find(name)` | 名字 → 引用；未知返回空 |
| `Material::set(name, value)` | 类型校验后写入；未知名字返回错误 |
| `ParameterSink::write / flush` | 后端实现；框架不解释值 |

### 4.4 GLSL 契约

块名与 binding 号由后端决定并记录在 `Keys` / `BlockDescriptors`；框架只约定：**块成员带 `vn_` 前缀、
名字来自 Schema、声明由生成器注入**。用户块必须在保留段之外。

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 参数清单只有一处手写；C++ / GLSL / 偏移表全部由它生成。 |
| C2 | 名字是 `块.字段` 形式，跨块唯一。 |
| C3 | 未知名字与类型不符都必须报错：`set()` 时、编译时、生成时三处。 |
| C4 | 框架占用一段 set/binding；用户块必须在保留段之外。 |
| C5 | 逐顶点 / 逐实例数据走 attribute，不进参数块。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | P0 的生成结果与今天逐字节一致，既有测试不改。 |
| R2 | 生成物进构建目录，门禁校验“重新生成无差异”。 |
| R3 | 后端只写字节，不解释值、不解析名字。 |

### 5.3 不变量（N#）

| # | 不变量 |
|---|---|
| N1 | 名字只在接线期存在；每帧循环里没有字符串。 |
| N2 | 每个块 16 字节对齐、按 std140 排列，D3D cbuffer 与 push constant 都能复用同一份布局。 |
| N3 | 同一字节只能有一个名字；一个名字只能指向一个字节。 |

## 6. 注意事项与陷阱

| # | 坑 | 症状 / 判据 |
|---|---|---|
| P1 | 松散 uniform 在 Vulkan 不合法 | `glslangValidator -V` 直接报 `non-opaque uniforms outside a block`；不要为“像 osg 一样”去包装它 |
| P2 | `vec3` 后面接 `float` 会跨 16 字节边界 | 只用 mat4/vec4/标量组合；生成器按 base alignment 排布并 `static_assert` |
| P3 | GLSL 手抄与 C++ 结构体漂移 | 现状：只有结构体测试；目标：GLSL 由生成器产出，不可能漂移 |
| P4 | 生成物与 Schema 不同步 | 门禁重新生成并比对；构建目录里的副本不带人改 |
| P5 | 注入位置错了 | 片段必须在 `#version` 之后；插在之前是编译错误，不是静默 |
| P6 | 用户重复声明同名块 | 重定义 → 编译失败；这是特性，不是限制 |

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 运行时按名反射自动填充 | 名字成为协议：拼错、类型错、布局错全部静默；多后端要写多个匹配器。详见 [ADR](../decisions/ADR-2026-10-24-shader-parameter-schema.md)。 |
| D2 | 松散 uniform（osg `StateSet` 风格） | Vulkan GLSL 不允许（P1）；osg 的 Vulkan 后继 vsg 也已没有 `Uniform` 类。 |
| D3 | 材质 DSL（自有语言 + 编译器） | 收益是自动变体与跨后端源码，成本是一个编译器；当前没有需求。详见 ADR。 |
| D4 | 以 C++ 头为真相、脚本解析它生成 GLSL | 要解析 C++；结构体里的注释与宏会让解析器变成第二套语义。 |
| D5 | 逐 draw 参数惰性上传 / specialization constant | 块已经一次写多帧，省不出东西。 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | P0 逐字节等价 | 生成头与现行 `ShaderAbi.hpp` 的尺寸 / 偏移断言全部通过 |
| T2 | 名字表覆盖每个字段 | 布局单测遍历 Schema，逐字段比对偏移 |
| T3 | 未知名字被拒 | `Material::set("Nope", 1.0F)` 返回错误；shader 里拼错 `vn_` 编译失败 |
| T4 | 生成物一致 | `scripts/vine_shader_check.sh` + 生成器差异检查 |
| T5 | 既有行为不变 | `test_graphics` / `test_vsg` 全绿，`vsg_rewrite_gate.sh` 0 VUID |

## 9. 状态与演进

### 9.1 当前状态

提案已定，未实现。现行形态是手写 `ShaderAbi.hpp` + 手抄 GLSL + 后端直接写存储。

### 9.2 已知缺口 / 待办

分三段：P0（Schema + 生成器，逐字节不变）、P1（名字表 + `Material::set` + 注入）、P2（第二个后端 +
用户扩展点）。触发条件见 `../backlog/graphics.md`。

### 9.3 兼容性 / 迁移

P0 不改任何字节与接口，可独立合入；P1 只加接口（`set()` 与生成的名字表），既有字段糖保持；P2 等第二个后端。

## 10. 参考

- `ShaderAbi.hpp` —— 现行四个块的布局与 `static_assert`。
- `DrawBlock.hpp` —— 现行打包规则与三条静默失败事实。
- `../decisions/ADR-2026-10-24-shader-parameter-schema.md` —— 为什么是 Schema 生成。
- `../design/graphics-rendering.md` —— 着色与目标；`../design/vsg-backend.md` §4.2 —— 键与状态。
- glslang `-V` 的 `non-opaque uniforms outside a block` 实测（P1）。
