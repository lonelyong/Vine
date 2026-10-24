# graphics 着色参数 ABI：块定义、去重与名字

> **状态**：现行（P0 已实现：GLSL 声明去重 + 构建期 inline；P1 未做，见 §9.2）
> **最后核对**：2026-10-24
> **范围**：view / draw / material / shadow / lights 五个参数块的声明归属、名字解析、后端写值契约。
> **权威来源**：`ShaderAbi.hpp` / `shaders/vine_params.glsl` / `LightBlock.hpp`（现行块布局）。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

五个参数块撑起引擎与 shader 之间的全部数据，容量已经确定（`VineViewBlock` 288 B、`VineDrawBlock` 80 B、
`VineMaterialBlock` 64 B、`VineShadowBlock` 80 B、`VineLightsBlock` 528 B：一盏环境光 + 8 个带类型的灯
槽，见 [ADR](../decisions/ADR-2026-10-24-light-block-typed-lights.md)），C++ 侧偏移由 `static_assert`
钉死。问题不是“布局对不对”，而是“每份声明有几个 owner”：

- **GLSL 侧只能有一份声明**：参数块写在 `src/viz/graphics/shaders/vine_params.glsl` 一处，shader 用
  `#include` 引入、构建期展开。抄成两份时，改一处忘另一处，只有一半画面读错字节，而画面照样有东西；
- **影子块按 program 定 binding**：全屏 program 的低 binding 已被自己的 source 占用，所以 `VineShadowBlock`
  的唯一声明是 `BuiltinShaders::shadowBindings()`（它写调用方要的编号），不在片段里；
- **C++ 侧跨了两个模块**：四个 L1 块在 SDK 的 `ShaderAbi.hpp`，`VineLightsBlock` 在后端的 `LightBlock.hpp`
  （唯一消费者是 vsg 后端，上移是触发条件型，见 §3.3）；
- **名字只存在于散文**（P1）：`VineDrawBlock::params.x` 是 opacity，这条约定写在注释里，代码里没有名字；
- **后端直接写自己的存储**（P1）：没有“写字节”的契约，第二个后端要把打包逻辑重写一遍。

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

- **GLSL**：五个块的声明在 `shaders/vine_params.glsl`；shader 用 `#define VINE_PARAM_<块>` 声明自己要哪些，
  再 `#include "vine_params.glsl"`，构建期把片段 inline 进嵌入字符串。影子块是唯一例外：它的 set/binding
  属于声明它的 program，唯一声明在 `BuiltinShaders::shadowBindings()`。
- **C++**：四个 L1 块在 `ShaderAbi.hpp` 一处；`VineLightsBlock` 与打包同住后端 `LightBlock.hpp`（上移见 §3.3）。
- **名字表**（P1）：手写在结构体旁边，条目用 `offsetof` 而不是手填数字，编译器保证与 C++ 一致。

去重不是纪律问题：重复的声明不在维护者眼前，改一处忘一处是**结构**造成的，只能用结构的办法解决。

### 2.2 频率定缓冲

| 缓冲 | 内容 | 上传时机 |
|---|---|---|
| view | 相机矩阵、视口尺寸、时间 | 每 view |
| draw | 模型矩阵、不透明度、点大小、用户槽 | 每 draw（ring + dynamic offset） |
| material | 环境 / 漫反射 / 高光 / 光泽 | 每材质（去重后） |
| shadow | 光空间矩阵、阴影参数 | 每 pass |
| lights | 一盏环境光 + `kMaxLights` 个带类型的灯（方向光 / 点光 / 聚光；位置与方向都在视图空间） | 每 view |

表里两处要点：`params` 的 `x` 是有效不透明度（唯一透明通道）、`y` 是点大小（framebuffer 像素，顶点阶段写进 `gl_PointSize`；由 `StateNode::setPointSize` 或 pass 覆盖决定、解析后**进 draw 块而不是 `PipelineKey`**，所以两个尺寸共用一条 pipeline）、`z`/`w` 是用户槽。`kMaxLights`（`ShaderAbi.hpp`）是灯容量的唯一定义（GLSL 数组长度、后端结构体尺寸、掉落文案都由它派生，`EmbeddedShadersTest` 读回数组长度），它**不是**内容上限（`Scene::addLight` 不设限，见 `graphics-scene.md` §2.7）。

逐顶点与逐实例数据**不是参数**：位置 / 法线 / 颜色 / UV 走 canonical attribute，自定义通道保留源
location（≥ 3，避开 8 与 12–15，见 `ShaderAbi.hpp`）。

### 2.3 名字只在接线期

`find("Draw.Opacity")` 返回 `{Buffer::Draw, 64, Type::Float}`；`Material` 与 `StateNode` 缓存解析结果，
之后的每次写入都是按偏移写字节。名字是 `块.字段`，跨块唯一。

### 2.4 GLSL 只有一份

片段在构建期并入每个 embedded shader 的字符串（`cmake/VineEmbedShaders.cmake`），运行时不需要文件、不需要
include 解析，也不需要编译器扩展；用户 shader 想用同一套块时，把同一个片段交给他们（P1 决定是安装文件
还是编译前拼接）。

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

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 共享 GLSL 片段 | 五个块声明一处；shader 用守卫 + `#include` 选块 | 已实现 |
| F2 | C++ 定义收敛 | 四个 L1 块在 `ShaderAbi.hpp`；`VineLightsBlock` 留在后端 | 已实现（灯块上移待触发） |
| F3 | 构建期 inline | 片段并入嵌入字符串，运行时不落盘 | 已实现 |
| F4 | 名字解析（P1） | 手写表 + `offsetof`；一次解析、缓存 | 未做 |
| F5 | 后端契约 | `ParameterSink` 的写与刷 | 未做 |
| F6 | `Material` 按名设值（P1） | 类型化重载 + 既有字段糖保留 | 未做 |
| F7 | 用户扩展点 | draw 块的具名用户槽 + 保留 binding 段 | 未做 |
| F8 | 门禁 | 片段清单、展开一致（未知 `vn_` 名字可选） | 部分实现 |
| F9 | P0 等价性 | 字节、接口、测试全不变 | 已实现 |

### 3.2 F1 共享片段

参数块只写在 `src/viz/graphics/shaders/vine_params.glsl`；一个 shader 只声明它真正读的块，因为程序文本就是
它（variant 的）descriptor 接口——后端按声明服务绑定，一个没人读的块仍然是一个有人要填的 binding：

```glsl
#define VINE_PARAM_MATERIAL
#define VINE_PARAM_DRAW
#define VINE_PARAM_LIGHTS
#include "vine_params.glsl"
```

守卫是 `VINE_PARAM_MATERIAL` / `VINE_PARAM_DRAW` / `VINE_PARAM_LIGHTS`；`#include` 必须独占一行行首（P5）。
影子块（§1.1）与 view 块不在片段里：view 块没有内置 shader 声明它——content 路径把视图实现成 128 B push
range（见 `ContentPush.hpp`），想要这个块的 program 按自己的布局声明它。

灯块的 set/binding 是两条路径**共用**的 set 0 / binding 5（content 集：material 0、diffuse 1、shadow 对 3/4，
2 因此空出；全屏集：source 颜色 0..N-1、深度 4、灯 5、shadow 对 6/7），所以片段里写的是同一个数字，不需要
可参数化的 binding（ABI 扫描器也不做宏求值）。

### 3.3 F2–F3 C++ 收敛与构建期 inline

C++ 侧保持手写与 `static_assert`：四个 L1 块在 `ShaderAbi.hpp` 一处。`VineLightsBlock` 与它的打包同住
`LightBlock.hpp`——今天唯一的消费者是 vsg 后端，上移 SDK 的条件（第二个后端需要同一份灯布局）记在待办。
片段与结构体仍是两份手写，靠相邻 review 与 `ProgramAbiTest` 的绑定数/块大小断言守住。

嵌入前由 `cmake/VineEmbedShaders.cmake` 把片段展开进每个 shader 的字符串：运行时不解析 include、不落盘，
打包形态不变。片段文件在 `cmake/VineShaders.cmake` 的 `INCLUDES` 里声明，否则改片段不会重跑生成（P7）。
离线门禁 `vine_shader_check.sh` 用同一个展开脚本编译并校验每个变体，所以“门禁过” = “嵌入的就是编译的”。

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

片段决定：块名、set/binding、成员名、类型与顺序；`#ifdef VINE_PARAM_<块>` 决定一个 shader 声明哪些块。它是
C++ 结构体之外**唯一**的 shader 侧声明；任何 shader 再抄一份就是新的 owner。唯一的例外是影子块：它声明在
`BuiltinShaders::shadowBindings()`，因为它的 set/binding 由声明它的 program 决定（§1.1）。

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
| C1 | 每个块在 GLSL 侧只能声明一次：参数块在 `vine_params.glsl`，影子块在 `shadowBindings()`。 |
| C2 | 名字是 `块.字段` 形式，跨块唯一。 |
| C3 | 未知名字与类型不符都必须报错：`set()` 时、编译时。 |
| C4 | 框架占用一段 set/binding；用户块必须在保留段之外。 |
| C5 | 逐顶点 / 逐实例数据走 attribute，不进参数块。 |
| C6 | 片段只能用行首独占一行的 `#include` 引入，由构建期展开；嵌入字符串里不出现 include。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | P0 不改字节、不改接口、不改测试。 |
| R2 | 嵌入副本与展开后的源一致由 `vine_shader_check.sh` 保证。 |
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
| P3 | GLSL 声明抄了第二份 | 只有一半画面读错字节，最难查；判据是 T2 的搜索，修法是删掉副本改用片段 |
| P4 | 片段与 C++ 结构体漂移 | 现在只剩这一对；靠相邻 review，`ProgramAbiTest` 的块大小断言钉住字节总数 |
| P5 | 片段没进嵌入字符串 | 只有始于行首的 `#include` 会被展开；缩进的会原样留下，运行时 glslang 报“include 需要扩展”——可见，不静默 |
| P6 | 用户重复声明同名块 | 重定义 → 编译失败；这是特性，不是限制 |
| P7 | 片段改了而嵌入没重跑 | 片段必须在 `VineShaders.cmake` 的 `INCLUDES` 里：否则生成规则不依赖它，嵌入副本静默变旧；T3 用展开后的哈希兜底 |
| P8 | 守卫写错（多声明或漏声明块） | 多声明 → program 多一个必须填的 binding；漏声明 → 未声明标识符。两者都在 `ProgramAbiTest` 的绑定数断言或编译时暴露 |
| P9 | 把点大小（或任何顶点阶段要读的值）当成 pipeline 状态 | Vulkan 里点大小是 `gl_PointSize` 输出，不是光栅状态；放进 `PipelineKey` = 一个尺寸一个 pipeline，而且动它还要重编。它的终点是 draw 块的 `params.y`（下限 1.0，`largePoints` 是设备要求），`PrimitiveDrawTest` 用像素数钉住整条路 |

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
| T2 | 每个块只有一处声明 | `grep -rn 'uniform Vine\w*Block' src/`：只命中 `shaders/vine_params.glsl` 与 `BuiltinShaders.cpp` 的 `shadowBindings()` |
| T3 | 嵌入副本与展开后的源一致 | `scripts/vine_shader_check.sh`（展开后的哈希与字节数） |
| T4 | 未知名字被拒（P1） | `Material::set("Nope", 1.0F)` 返回错误 |
| T5 | 片段真的进了程序文本 | 删掉声明 → 未声明标识符，构建期门禁失败（与 T3 同一条命令） |
| T6 | 每个 program 只声明自己的块 | `ProgramAbiTest`：forward 5 个绑定、gbuffer 1 个 |

## 9. 状态与演进

### 9.1 当前状态

P0 已实现：参数块只有一处 GLSL 声明（`shaders/vine_params.glsl`，构建期展开），四个 L1 块的 C++ 定义在
`ShaderAbi.hpp`，`VineLightsBlock` 与打包同住 `LightBlock.hpp`。字节、公开接口与既有测试未变。

灯块自 2026-10-24 起是「带类型的灯数组」（位置 / 方向 / 范围 / 锥角，两条路径共用 UBO），
见 [ADR](../decisions/ADR-2026-10-24-light-block-typed-lights.md)。

### 9.2 已知缺口 / 待办

P1（名字表与 `Material::set`、用户扩展点、`ParameterSink`）与 `VineLightsBlock` 上移都是触发条件型，见
`../backlog/graphics.md` §2.5。材质块仍是 Phong：改 PBR 见 `graphics-material.md` F4 / backlog M1。

### 9.3 兼容性 / 迁移

P0 不改任何字节与接口，可独立合入；P1 只加接口，既有字段糖保持；生成器只在第二种 shader 语言出现时评估。

## 10. 参考

- `shaders/vine_params.glsl` —— 参数块的唯一 GLSL 声明（material / draw / lights，按守卫选块）。
- `cmake/VineEmbedShaders.cmake` —— 构建期展开与嵌入；`cmake/VineShaders.cmake` 的 `INCLUDES` 声明依赖。
- `ShaderAbi.hpp` —— 四个 L1 块的 C++ 布局与 `static_assert`。
- `LightBlock.hpp` —— 灯块、它的打包与三条静默失败事实。
- `DrawBlock.hpp` —— 打包规则与三条静默失败事实。
- `../decisions/ADR-2026-10-24-shader-parameter-abi.md` —— 为什么手写、为什么不反射、为什么暂不生成。
- glslang `-V` 的 `non-opaque uniforms outside a block` 实测（P1）。
