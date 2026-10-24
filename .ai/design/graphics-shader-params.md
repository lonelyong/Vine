# graphics 着色参数 ABI：块定义、去重与名字

> **状态**：提案（已定，未实现；实现进度见 `../backlog/graphics.md`）
> **最后核对**：2026-10-24
> **范围**：view / draw / material / shadow / lights 五个参数块的声明归属、名字解析、后端写值契约。
> **权威来源**：`ShaderAbi.hpp` / `LightBlock.hpp`（现行块布局）/ 本文件（目标形态）。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

五个参数块撑起引擎与 shader 之间的全部数据，容量已经确定（`VineViewBlock` 288 B、`VineDrawBlock` 80 B、
`VineMaterialBlock` 64 B、`VineShadowBlock` 80 B、`VineLightsBlock` 112 B），C++ 侧偏移由 `static_assert`
钉死。问题不是“布局对不对”，而是“每份声明有几个 owner”：

- **GLSL 侧有两份 `VineMaterialBlock`**：`builtin_forward.frag` 与 `builtin_gbuffer.frag` 各声明一次；改一处
  忘另一处，只有一半画面读错字节，而画面照样有东西；
- **C++ 侧跨了两个模块**：四个块在 SDK，`VineLightsBlock` 在后端的 `LightBlock.hpp`；
- **名字只存在于散文**：`VineDrawBlock::params.x` 是 opacity，这条约定写在注释里，代码里没有名字；
- **后端直接写自己的存储**：没有“写字节”的契约，第二个后端要把打包逻辑重写一遍。

一个后端、一个人维护时都能忍；第二个后端到来时，这些会被各复制一遍。

### 1.2 目标

- **每份声明只有一个 owner**：GLSL 一份共享片段，C++ 一处定义。
- 名字只在接线期存在，一次解析成 `{buffer, offset, type}`；热路径只有偏移。
- 后端只回答“怎么写字节”。
- 未知名字报错，而不是静默填零。

### 1.3 非目标

- 不引入 Schema 生成器：内部 ABI、一个模块、一年改几次，生成的成本大于收益；等第二种 shader 语言再议。
- 不做材质 DSL（自有语言 + 编译器）。
- 不做运行时反射 / 按名自动填充。
- 不做松散 uniform（Vulkan 不允许，见 P1）。
- 不做逐 draw 参数惰性上传、specialization constant 优化。

### 1.4 一句话结论

> 一份声明只写一次；名字只在接线期存在；后端只写字节。

## 2. 设计要点

### 2.1 一份声明，一处

- **GLSL**：五个块的声明抽成一个共享片段，内置 shader `#include` 它；构建期 inline 进嵌入字符串。
- **C++**：块定义收拢到一处；`VineLightsBlock` 是否上移 SDK 单独判断（今天只有 forward 与 `BlockStorage` 用得到）。
- **名字表**（P1）：手写在结构体旁边，条目用 `offsetof` 而不是手填数字，编译器保证与 C++ 一致。

去重不是纪律问题：重复的声明不在维护者眼前，改一处忘一处是**结构**造成的，只能用结构的办法解决。

### 2.2 频率定缓冲

| 缓冲 | 内容 | 上传时机 |
|---|---|---|
| view | 相机矩阵、视口尺寸、时间 | 每 view |
| draw | 模型矩阵、不透明度、点大小、用户槽 | 每 draw（ring + dynamic offset） |
| material | 环境 / 漫反射 / 高光 / 光泽 | 每材质（去重后） |
| shadow | 光空间矩阵、阴影参数 | 每 pass |
| lights | 一盏环境光 + 至多三盏方向光（视图空间） | 每 view |

逐顶点与逐实例数据**不是参数**：位置 / 法线 / 颜色 / UV 走 canonical attribute，自定义通道保留源
location（≥ 3，避开 8 与 12–15，见 `ShaderAbi.hpp`）。

### 2.3 名字只在接线期

`find("Draw.Opacity")` 返回 `{Buffer::Draw, 64, Type::Float}`；`Material` 与 `StateNode` 缓存解析结果，
之后的每次写入都是按偏移写字节。名字是 `块.字段`，跨块唯一。

### 2.4 GLSL 只有一份

片段在构建期并入每个 embedded shader 的字符串，运行时不需要文件、不需要 include 解析；
用户 shader 想用同一套名字时，把同一个片段交给他们（P1 决定是安装文件还是编译前拼接）。

### 2.5 后端只写字节

后端契约只有两个动作：按缓冲写一段字节、按缓冲刷一段范围。值怎么算、什么时候算不在后端，
在 `packDrawBlock` 这类引擎侧函数里。

### 2.6 P0 不改字节

第一阶段验收：尺寸、对齐、成员偏移、公开接口、既有测试全部不变。**先证明等价，再谈新能力。**

### 2.7 散文约定变成具名成员

`params.x` / `cam_pos.w` / `frame.w` 这类“vec4 里第几个分量是什么”的约定，在 GLSL 侧变成具名成员
（`vn_opacity` / `vn_pointSize`）；C++ 侧字节布局不变。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 |
|---|---|---|
| F1 | 共享 GLSL 片段 | 五个块声明一处，内置 shader include |
| F2 | C++ 定义收敛 | 块定义收拢；`VineLightsBlock` 归属单独判断 |
| F3 | 构建期 inline | 片段并入嵌入字符串，运行时不落盘 |
| F4 | 名字解析（P1） | 手写表 + `offsetof`；一次解析、缓存 |
| F5 | 后端契约 | `ParameterSink` 的写与刷 |
| F6 | `Material` 按名设值（P1） | 类型化重载 + 既有字段糖保留 |
| F7 | 用户扩展点 | draw 块的具名用户槽 + 保留 binding 段 |
| F8 | 门禁（可选） | 嵌入副本一致、未知 `vn_` 名字 |
| F9 | P0 等价性 | 字节、接口、测试全不变 |

### 3.2 F1 共享片段

```glsl
// vine_params.glsl —— 五个参数块，五个 owner
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock { ... };
layout(set = 1, binding = 0, std140) uniform VineDrawBlock { ... };
```

每个内置 shader 只保留一行 `#include "vine_params.glsl"`；`VineMaterialBlock` 的两份副本随之消失。

### 3.3 F2–F3 C++ 收敛与构建期 inline

C++ 侧保持手写与 `static_assert`，只是把五个块放在一处；`VineLightsBlock` 上移的条件写进待办——
第二个后端也需要同一份灯布局时才动。

嵌入前由构建脚本把片段展开进每个 shader 的字符串：运行时不解析 include、不落盘，打包形态不变；
离线门禁 `vine_shader_check.sh` 用同一份片段编译每个变体。

### 3.4 F4 名字解析（P1）

```cpp
struct ParameterDesc { const char* name; Buffer buffer; std::size_t offset; Type type; };
inline constexpr ParameterDesc kParameters[]{
    { "Draw.Opacity", Buffer::Draw, offsetof(VineDrawBlock, opacity), Type::Float },
};
```

表与结构体同在 C++ 头里，`offsetof` 让编译器保证两者一致；`find()` 在接线期查一次，结果缓存。

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

### 3.6 F6–F7 `Material` 与用户扩展点

`Material::set("Material.Diffuse", color)` 走名字表，既有 `setDiffuse()` 等字段糖保留、内部走同一入口；
未知名字与类型不符都在 `set()` 返回错误。用户扩展点是 draw 块的两个具名槽（`Draw.UserSlot0/1`）与一段
保留 set/binding；用户自定义块只能落在保留段之外，占用即错误。

### 3.7 F8–F9 门禁与等价性

- 嵌入副本与源文件一致：`vine_shader_check.sh` 的 SHA 校验已覆盖；
- 未知 `vn_` 名字：按需加（可选，不是 P0 的前置）；
- 等价性：P0 不改任何字节与接口，`BlockStorageTest` / `StateNodeTest` 不改。

## 4. 公开接口

### 4.1 共享片段的契约

片段决定：块名、set/binding、成员名（`vn_` 前缀）、类型与顺序。它是 C++ 结构体之外**唯一**的
shader 侧声明；任何 shader 再抄一份就是新的 owner。

### 4.2 C++ 类型

五个块结构体（保持原名；`VineLightsBlock` 是否上移见 §3.3）、`ParameterBuffer` 枚举、
`ParameterRef{buffer, offset, type}`、`find()`。

### 4.3 运行时 API

| 接口 | 契约 |
|---|---|
| `ParameterTable::find(name)` | 名字 → 引用；未知返回空 |
| `Material::set(name, value)` | 类型校验后写入；未知名字返回错误 |
| `ParameterSink::write / flush` | 后端实现；框架不解释值 |

### 4.4 用户 shader 怎么用

内置与用户 shader 共用同一份片段：拼错 → 未声明标识符（编译失败），重复声明同名块 → 重定义
（编译失败）。两个失败都可见。

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 每个块在 GLSL 侧只能声明一次；再抄一份即错误。 |
| C2 | 名字是 `块.字段` 形式，跨块唯一。 |
| C3 | 未知名字与类型不符都必须报错：`set()` 时、编译时。 |
| C4 | 框架占用一段 set/binding；用户块必须在保留段之外。 |
| C5 | 逐顶点 / 逐实例数据走 attribute，不进参数块。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | P0 不改字节、不改接口、不改测试。 |
| R2 | 嵌入副本与源文件一致由现有门禁保证。 |
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
| P2 | `vec3` 后面接 `float` 会跨 16 字节边界 | 只用 mat4/vec4/标量组合；结构体的 `static_assert` 钉住偏移 |
| P3 | GLSL 声明抄了第二份 | 现状：`VineMaterialBlock` 在 forward 与 gbuffer 各一份；只有一半画面错，最难查 |
| P4 | 片段与 C++ 结构体漂移 | 现在只剩这一对；靠相邻 review，必要时加一条布局测试 |
| P5 | 片段没进嵌入字符串 | 构建期 inline 漏了 = 编译期未声明标识符，不会静默 |
| P6 | 用户重复声明同名块 | 重定义 → 编译失败；这是特性，不是限制 |

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 运行时按名反射自动填充 | 名字成为协议：拼错、类型错、布局错全部静默；多后端要写多个匹配器。详见 [ADR](../decisions/ADR-2026-10-24-shader-parameter-abi.md)。 |
| D2 | 松散 uniform（osg `StateSet` 风格） | Vulkan GLSL 不允许（P1）；osg 的 Vulkan 后继 vsg 也已没有 `Uniform` 类。 |
| D3 | 材质 DSL（自有语言 + 编译器） | 收益是自动变体与跨后端源码，成本是一个编译器；当前没有需求。 |
| D4 | 现在引入 Schema 生成器 | 内部 ABI、一个模块、一年改几次；去重后只剩“片段 ↔ C++”一对，生成的成本大于收益。第二种 shader 语言出现时重开。详见 ADR。 |
| D5 | 逐 draw 参数惰性上传 / specialization constant | 块已经一次写多帧，省不出东西。 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 字节与接口不变 | `test_graphics` / `test_vsg` 全绿，`vsg_rewrite_gate.sh` 0 VUID |
| T2 | 每个块只有一处声明 | 搜索 `uniform Vine*Block`，只能命中片段文件 |
| T3 | 嵌入副本与源一致 | `scripts/vine_shader_check.sh` |
| T4 | 未知名字被拒（P1） | `Material::set("Nope", 1.0F)` 返回错误 |
| T5 | 片段被每个 shader 用上 | 缺失声明时编译器报未声明标识符，构建即失败 |

## 9. 状态与演进

### 9.1 当前状态

提案已定，未实现。现行形态：五个块的手写定义跨两个模块，`VineMaterialBlock` 的 GLSL 声明有两份。

### 9.2 已知缺口 / 待办

P0（片段去重 + C++ 收敛，字节不变）见 `../backlog/graphics.md` §1；名字表与 `Material::set`、用户扩展点、
`VineLightsBlock` 上移都是触发条件型。

### 9.3 兼容性 / 迁移

P0 不改任何字节与接口，可独立合入；P1 只加接口，既有字段糖保持；生成器只在第二种 shader 语言出现时评估。

## 10. 参考

- `ShaderAbi.hpp` —— 四个块的布局与 `static_assert`。
- `LightBlock.hpp` —— 灯块与它的 GLSL 契约。
- `DrawBlock.hpp` —— 打包规则与三条静默失败事实。
- `../decisions/ADR-2026-10-24-shader-parameter-abi.md` —— 为什么手写、为什么不反射、为什么暂不生成。
- glslang `-V` 的 `non-opaque uniforms outside a block` 实测（P1）。
