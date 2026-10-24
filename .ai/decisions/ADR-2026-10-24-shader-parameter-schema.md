# ADR-2026-10-24：着色参数走 Schema 生成，不走运行时反射与材质 DSL

- 状态：已采纳
- 日期：2026-10-24
- 相关：`../design/graphics-shader-params.md`

## 背景

四个参数块（view / draw / material / shadow）已经在 SDK 里，偏移由 `static_assert` 钉死，
但“有哪些参数、叫什么名字、在哪个偏移”只存在于注释和手抄的 GLSL 里。宿主想按名字设一个参数
（点大小、自定义槽）没有入口，第二个后端会把打包逻辑与名字再写一遍。

需要决定的是：**参数的契约放在哪、如何送到 shader。**

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 运行时反射，按名字自动填充 | 用户只声明自己用到的；新增参数不动既有 shader | 名字成为协议：拼错 / 类型错 / 布局错全部静默；每个后端一个匹配器 |
| B. 松散 uniform（osg `StateSet` 风格） | 最接近 osg 的写法，单值即可生效 | Vulkan GLSL 不允许；要支持必须自己做源码包装或另建默认块 |
| C. 材质 DSL（自有语言 + 编译器） | 自动变体、跨后端源码、参数清单可静态校验 | 一个编译器 + 一套工具链；当前没有需求 |
| D. Schema + 生成（选定） | 一条真相生成 C++ / GLSL / 偏移表；约定编译期可校验 | 多一个生成步骤与一道门禁 |

## 决定

选 **D**：参数清单只在一处手写，生成 C++ 结构体、GLSL 声明片段与名字/偏移表；后端只实现
“按缓冲写一段字节”。第一阶段（P0）的输出必须与现行 `ShaderAbi.hpp` 逐字节一致。

理由：

- **A 的失败模式是这个仓库最贵的一类**：未声明的绑定是静默 no-op、`#ifdef` 分支条件决定事实、
  写死 binding 读到未定义数据——三次都是“名字对不上，然后什么都不说”。按名字反射把这条路径再开一次。
- **B 在 Vulkan 里不合法**：`glslangValidator -V` 对块外非不透明 uniform 直接报错；osg 的 Vulkan 后继
  vsg 也已没有 `Uniform` 类（`state/` 里只有 `DescriptorBuffer` / `DescriptorSet` / `PushConstants`）。
- **C 是超前设计**：没有自动变体与跨后端源码的需求；引入 DSL 等于多养一个编译器。
- **D 让“约定”变成“生成物的属性”**：拼错名字在编译期失败，块布局在生成期确定，后端之间只差一个
  写字节的实现。

## 后果

- 好：名字、类型、偏移三者由同一份清单派生，不可能漂移；新后端只写一个 `ParameterSink` 与一个模板；
  用户 shader 与内置 shader 共享同一套 `vn_*` 名字。
- 代价：多一个 Python 生成器与一道“重新生成无差异”的门禁；Schema 的说明文字要承担原注释的一部分职责。
- 重开条件：参数项超过约 20 个且“只想要其中两个”成为真实负担；或出现宿主自定义 uniform 的真实需求
  （届时的正确形态是“一个用户块”，不是按名反射）。

## 证据

- `glslangValidator -V` 对一个块外 `uniform float` 的实测：`'non-opaque uniforms outside a block' : not
  allowed when using GLSL for Vulkan`；同一声明放进 `std140` 块内即通过。
- `build/_deps/vsg-src` 中不存在 `Uniform` 类，`state/` 目录只有描述符与 push constant。
- 现行 `ShaderAbi.hpp` 已用 `static_assert(sizeof/offsetof)` 钉住四个块，P0 以此作为等价性基准。
