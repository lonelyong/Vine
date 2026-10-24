# ADR-2026-10-24：着色参数手写声明 + 去重，不做反射与材质 DSL

- 状态：已采纳
- 日期：2026-10-24
- 相关：`../design/graphics-shader-params.md`

## 背景

五个参数块（view / draw / material / shadow / lights）撑起引擎与 shader 之间的全部数据。容量与偏移已经
确定并被 `static_assert` 钉死，但“每份声明有几个 owner”没有答案：`VineMaterialBlock` 在内置 shader 里
有两份，`VineLightsBlock` 的定义在后端，名字仍然只是注释里的散文（`params.x` 是 opacity）。

需要决定的是：**声明放几处，以及参数用什么机制送到 shader。**

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 运行时反射，按名字自动填充 | 用户只声明自己用到的；新增参数不动既有 shader | 名字成为协议：拼错 / 类型错 / 布局错全部静默；每个后端一个匹配器 |
| B. 松散 uniform（osg `StateSet` 风格） | 最接近 osg 的写法，单值即可生效 | Vulkan GLSL 不允许；要支持必须自己做源码包装或另建默认块 |
| C. 材质 DSL（自有语言 + 编译器） | 自动变体、跨后端源码、参数清单可静态校验 | 一个编译器 + 一套工具链；当前没有需求 |
| D. Schema 生成（C++ / GLSL / 名字表由一份清单产出） | 三者由构造保证一致 | 生成器 + 构建步骤 + 门禁；内部 ABI 一年改几次，成本大于收益 |
| E. 手写 + 去重（选定） | 每份声明一个 owner；无生成器；改动面最小 | 共享片段与 C++ 结构体仍是两份，靠相邻 review |

## 决定

选 **E**：

- 五个块的 GLSL 声明收拢成一个共享片段，内置 shader `#include`，构建期 inline 进嵌入字符串；
- C++ 块定义收拢到一处（`VineLightsBlock` 是否上移单独判断）；
- 名字表（P1）写在结构体旁边，条目用 `offsetof`，由编译器保证与 C++ 一致；
- Schema 生成（D）推迟到第二种 shader 语言出现时再评估。

理由：

- **A 的失败模式是这个仓库最贵的一类**：未声明的绑定是静默 no-op、`#ifdef` 分支条件决定事实、
  写死 binding 读到未定义数据——三次都是“名字对不上，然后什么都不说”。
- **B 在 Vulkan 里不合法**：`glslangValidator -V` 对块外非不透明 uniform 直接报错；osg 的 Vulkan 后继
  vsg 也已没有 `Uniform` 类。
- **C 是超前设计**：没有自动变体与跨后端源码的需求。
- **D 的成本落在一个只有开发者维护的内部 ABI 上**：真正的缺陷不是“没人检查”，而是“声明有两份”。
  去掉第二份之后，剩下的两处（片段 ↔ 结构体）相邻、可并排读，一个人能负责。

## 后果

- 好：重复的 GLSL 声明消失（`VineMaterialBlock` 只剩一处）；C++ 定义收拢；新后端只写一个
  `ParameterSink` 与一份片段。
- 代价：共享片段与 C++ 结构体仍是两份手写；靠相邻 review，必要时加一条布局测试。
- 重开条件：出现第二种 shader 语言（HLSL / Metal），或参数项超过约 20 个且“只想要其中两个”成为真实负担。

## 证据

- `glslangValidator -V` 对一个块外 `uniform float` 的实测：`'non-opaque uniforms outside a block' : not
  allowed when using GLSL for Vulkan`；同一声明放进 `std140` 块内即通过。
- `build/_deps/vsg-src` 中不存在 `Uniform` 类，`state/` 目录只有描述符与 push constant。
- 现状：`VineMaterialBlock` 在 `builtin_forward.frag` 与 `builtin_gbuffer.frag` 各声明一次；
  `VineLightsBlock` 定义在 `LightBlock.hpp`，不在 SDK 的 `ShaderAbi.hpp`。
