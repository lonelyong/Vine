# 着色器文件、嵌入与命名（`builtin_*` 机制）

> **状态（2026-09-25 收口）**：本文原为「vsg 后端自定义着色设计」（设计稿 v1，2026-09-03）+ 旧后端
> 落地记录，共 686 行。现在**只有 §10 是活文档** —— 着色器文件、嵌入机制、命名规则、门禁、加一个
> shader 的步骤（`builtin_<角色>.<阶段>` 这套机制属于 SDK，重写后不变）；其余章节（§1–§9 的 ABI 草案
> 与 vsg 内建 ShaderSet 参考档案、§11 的旧落地记录、2026-09-18 校注）属于已删除的旧后端，全文在
> git 历史：`git show 0b2f58c:.ai/design/vsg-custom-shader.md`。
>
> **自定义着色的活契约（四份）**：作者视角 = `src/viz/graphics/docs/usage.md`；后端侧 ABI
> （声明即绑定、push、变体、`(set, binding)` 规则）= `.ai/design/vsg-reimplementation.md` §5.4；
> 名字地图 = `docs/data-flow.md` §6；引擎侧着色契约 = `.ai/design/graphics-shader.md`。

## 10. 着色器文件与嵌入方式（2026-09-13 落地）

自写 shader 从"代码里的字符串字面量"改成"真文件 + 构建期嵌入"：文件有语法高亮、可 diff、可离线
编译校验，内容随二进制走（**不**往 DLL 旁边拷资源，**不**提交预编译 `.spv`）。本节只回答四件事：文件放哪、怎么进二进制、怎么加一个、怎么被门禁挡住。

### 10.1 文件位置与归属

| 归属 | 目录 | 生成的头文件 | 命名空间 | 谁在用 |
| --- | --- | --- | --- | --- |
| graphics SDK（内建 program） | `src/viz/graphics/shaders/` | `vine/graphics/EmbeddedShaders.hpp` | `vn::graphics::shaders` | `BuiltinShaders`（`forwardProgram()` / `flatForwardProgram()` 前向着色 + gbuffer 几何 / 全屏光照）；`RenderPipelineBuilder` 是它的别名 |
| vsg 后端（自有阶段）—— **已取消（2026-09-13）** | ~~`src/plugins/gfx_backend_vsg/shaders/`~~ 目录已删除 | ~~`vine/vsg/EmbeddedShaders.hpp`~~ | ~~`vn::vsg::shaders`~~ | 全屏三角形与屏幕拷贝现在都是 SDK program（`BuiltinShaders::fullscreenVertexProgram` / `screenCopyProgram`）。**清单只剩一个 owner**：`vine/graphics/EmbeddedShaders.hpp` |

约定：

| 规则 | 原因 |
| --- | --- |
| 后缀即阶段：`.vert` / `.frag` / `.comp` / `.geom` / `.tesc` / `.tese` | 校验器据此判阶段，不需要额外清单 |
| 每行 LF 结尾 | CR 会被读写两端各自归一化，嵌入文本就与文件不一致；生成器直接报错 |
| 小 fixture（`app_shell` / `vsg_selftest` / 测试探针）保持内联 | 它们不是产品 shader，放进清单反而多一层间接 |
| 文件名 = `builtin_<角色>.<阶段>`（角色用渲染器的词，不重复阶段或目标，不带产品名或 C++ 的词） | 见文件顶部 2026-09-13 命名规则：`builtin_` 划分归属，角色与默认管线的 pass 名同一个词 |
| 常量名 = 文件名转大驼峰 + 阶段（`builtin_forward.vert` → `kBuiltinForwardVert`） | 从文件名就能猜出常量名，拼错是编译错误而不是运行时回落 |
| program 名 = 文件名去后缀（同一份文件出多个 program 时加后缀） | diagnostic 里报出的 program 名直接指明该读哪个文件 |

### 10.2 嵌入机制（构建期，不拷资源）

| 环节 | 位置 | 说明 |
| --- | --- | --- |
| 清单（shader → 头文件） | `cmake/VineShaders.cmake`（root `include(VineShaders)`） | 在**顶层**声明：生成规则对 `src/` 与 `tests/` 同时可见 |
| 机制 | `cmake/VineShaderHelper.cmake` | `vn_declare_embedded_shaders(...)` + `vn_use_embedded_shaders(<target> ...)` |
| 生成器 | `cmake/VineEmbedShaders.cmake`（`cmake -P`） | 读文件 → 写 `inline constexpr std::u8string_view` + `Entry{name,hash,bytes}` 表 |
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
// SDK 侧（RenderPipelineBuilder / BuiltinShaders）：文本是嵌入常量
vs.source = String(shaders::kBuiltinGbufferVert);
```

（vsg 侧不再直接引用嵌入常量：program 的 GLSL 文本由 SDK / 宿主的声明交上来，后端只解析 ——
见 `.ai/design/vsg-reimplementation.md` §5.4 的 `ProgramAbi`。）

| 类型 | 值 | 转换 |
| --- | --- | --- |
| 生成常量 | `std::u8string_view` | —— |
| `vn::String` | 内部 `std::u8string` | `String(kX)`（构造函数 explicit） |

### 10.4 门禁

| 门禁 | 查什么 | 红了意味着 |
| --- | --- | --- |
| `scripts/vine_shader_check.sh` | (a) 每个 shader × 变体 define 组合（`VINE_VERTEX_COLOR` / `VINE_DIFFUSE_MAP`）跑 glslangValidator；(b) 每个嵌入副本的 SHA-256 前缀与字节数与磁盘文件一致；(c) 每个 `*/shaders/*` 文件都在清单里 | 语法错 / 变体分支编译不过 / 嵌入副本过期 / 有孤儿 shader 文件 |
| `test_graphics` / `test_vsg` 的 `EmbeddedShadersTest` | 每个条目是完整 GLSL（`#version 450\n` 开头、有 `main`、以 `}\n` 结尾）、`bytes == size()`、hash 是 16 位小写十六进制、名字唯一；工厂确实在用嵌入文本 | 生成器截断/转义出错，或常量与使用者脱钩 |
| 门禁应用阶段（`scripts/vsg_rewrite_gate.sh`） | 画面判据（内容占比 / 预览亮度 / 平背景） | 渲染行为变化 |

变体策略（§4 的 define 变体）在本机制里的落法：**一份源文件 + `#ifdef`**，门禁把每个 define 组合都
编译一遍，所以"用了 define 却没测过另一支"在提交前就会被抓住。

### 10.5 加一个 shader 的步骤

1. 在归属目录放 `xxx.vert` / `xxx.frag`（LF 结尾）。
2. 在 `cmake/VineShaders.cmake` 对应 `SOURCES` 里加一行。
3. 在 C++ 里 `#include <vine/.../EmbeddedShaders.hpp>`，用 `kXxxVert`。
4. `cmake -S . -B build`（新文件要重新 configure），然后 `scripts/vine_shader_check.sh`。

> 自定义着色的活契约：`src/viz/graphics/docs/usage.md`（作者视角）、
> `.ai/design/vsg-reimplementation.md` §5.4（后端 ABI）、`docs/data-flow.md` §6（名字即绑定）。
