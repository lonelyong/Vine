# 后端实现里能不能暴露 Vulkan？—— 边界设计论证

> **状态**：设计论证（**非现行设计**；结论先在 vk 上执行，落地并入 `design/vsg-backend.md` / `design/graphics-architecture.md` 后再删本文）
> **日期**：2026-11-13
> **问题**：写第二个后端（`vk`，手撸 Vulkan）时，**Vulkan 类型到底可以暴露到哪一层**？哪些暴露是自由的，哪些一旦发生就把 SDK 契约弄坏了？
> **结论（一句话）**：**插件内部随便暴露；SDK 与工厂入口不许暴露；需要给学习/工具用的那一扇门是一个显式、可选、后端特有的"逃生门"，不是 `RenderDevice` 动词表里的一个方法。**

## 1. 概要

### 1.1 为什么这问题现在才值得写

`gfx_backend_vsg` 是唯一实现，没人问过这个问题：它内部大量使用 Vk 类型，而它没有第二个消费者。`vk` 是第一个
"同 API 家族的第二个实现"，也是**学习用途**——这意味着它比 vsg 更需要 Vk 访问（抓帧、experiment、validation
回调、拿 `VkDevice` 做实验）。所以"能不能暴露"从学术问题变成要拍板的事。

### 1.2 本文回答的三问

1. 今天 Vk 出现在哪、谁 include、装不装（事实）；
2. 别的引擎怎么做（参考）；
3. 因此本仓库该在哪一层允许、哪一层禁止、用什么形状、怎么检查（论证与结论）。

## 2. 事实盘点（代码证据）

### 2.1 今天 Vk 出现在哪里

统计口径：`grep -rn "Vk[A-Z]\|vulkan"`，`src/` 与 `tests/`，2026-11-13。

| 位置 | 命中 | 性质 | 结论 |
|---|---|---|---|
| SDK 宿主面 `sdk/vine/graphics/*.hpp` | **3** | 全部是**注释里的文字**（`Sampler.hpp` 解释"Vulkan 把采样器放在 image 之外"、`RenderBackendRegistry.hpp` 的 `"vulkan \| opengl3"` 示例串） | **零 Vk 类型**，零 `<vulkan>` include |
| SDK 后端面 `sdk/vine/graphics/backend/*.hpp` | **2** | 全部是 `Keys.hpp` 注释里的 `VkFormat` / `VkPipeline`（解释"设备格式码"与一次真实 VUID 事故） | **零 Vk 类型**，零 `<vulkan>` / `<vsg>` include |
| 插件 `include/vine/backend_vsg/internal/` + `support/` | 18 / 57 个头含 `Vk*` | 真实类型（`VkDevice`、`VkPipeline`、`VkCommandBuffer`…） | 允许，且是今天的主力 |
| 插件 `include/vine/backend_vsg/shell/` | **0** | `GfxBackendVsgPlugin.hpp` / `VsgRenderBackendFactory.hpp` | 干净 |

**读法**：本仓库的边界**不是"后端不许提 Vulkan"**，而是"**SDK 的两个面都不出现 Vk 类型，插件内部随便用**"。
`backend/` 的规则是 C1 的另一种说法：它可以用 Vulkan 的**语义**（`device_color_formats` 装 `VkFormat` 值），
但不 include Vulkan（`C6`：触发前不再新增这类拼写）。

### 2.2 谁 include 谁（消费者决定严格程度）

| 头 | 谁 include | 后果 |
|---|---|---|
| `sdk/vine/graphics/*.hpp` | 宿主、appfw、应用、`test_graphics` | 出现 Vk 类型 ⇒ **所有应用**被迫依赖 Vulkan |
| `sdk/vine/graphics/backend/*.hpp` | `libGraphics`、`gfx_backend_*`、`test_vsg` | 出现 Vk 类型 ⇒ **所有后端**被迫依赖 Vulkan（含未来的 DX12/Metal） |
| 插件 `shell/` | 只有插件自己的两个 `.cpp` | 出现 Vk 类型 ⇒ 今天**无人受害**；将来若被宿主 include 才成问题 |
| 插件 `internal/` `support/` | 插件自己 + `test_vsg`（直接编译源码） | 出现 Vk 类型 ⇒ 只有测试受影响，而测试本来就要 Vk |

### 2.3 安装与导出

- `backend/` 单独装成 `VineGraphicsBackendSdk` 组件（`install_manifest_VineGraphicsBackendSdk.txt`）——
  它是**建立**的公共面，所以它必须不含 Vulkan 头，否则安装包把 Vulkan 变成依赖。
- 插件头**不安装**（`VinePluginHelper.cmake` 的 `install(TARGETS ...)` 只装二进制）。
  ⇒ 插件头的"公开"是**名义上的**，今天没有安装面这个消费者。

**这条事实很重要**：它意味着"插件头不许有 Vk"今天**没有受害者**，所以它不该是一道门（见 §6.3）。

## 3. 外部参考（多引擎对照）

### 3.1 对照表

| 项目 | 做法 | 借什么 | 不借什么 |
|---|---|---|---|
| **wgpu / wgpu-hal** | 公共类型 API 无关；原生访问是**显式、可选、unsafe 的 cast**（`as_hal` 一类），不是核心类型上的方法 | 逃生门要"有名字、要主动要" | 把 unsafe 语义引入本仓库（`G10` 的异常/安全成本） |
| **Diligent Engine** | Vulkan 对象经**后端特有接口**给出（`IRenderDeviceVk` / `ITextureVk::GetVkTexture()`），不在 `IRenderDevice` 上 | "后端特有接口 + 显式转换"这一形状 | 为每个资源建一套 `I*Vk` 接口（本仓库资源是值，无对象可挂） |
| **Qt QRhi** | `QRhiTexture::nativeTexture()` 返回后端特有的 `NativeTexture{object, layout}`；初始化收 `QRhiVulkanNativeHandles` | 原生句柄**成对带语义**（对象 + layout） | 让 SDK 头 include `vulkan.h` |
| **Godot RenderingDevice** | 资源用不透明 `RID`；原生句柄经 `get_driver_resource()` 这一**命名枚举**取得 | 逃生门是"一个入口 + 一个枚举"，不是一堆 getter | 把 RID 式句柄引入（本仓库用 typed id + 值） |
| **bgfx** | 资源无原生句柄；只有 `PlatformData` 在初始化时带平台/context 句柄 | "init 时给平台句柄"是合理的最小暴露 | 不做资源级原生访问 |
| **Filament** | `backend::` 是内部抽象；公共 `Engine` / `Texture` API 无关 | 分层与本文结论一致 | — |
| **Unreal RHI** | `FRHIResource::GetNativeResource()` + 平台子类（`FVulkanTexture`） | 逃生门在**具体后端类型**上，不在抽象基类 | — |
| **Dawn** | `dawn_native` 对象有 `GetNativeTexture()` / 后端 cast，供 Chromium 互操作 | 互操作是**真实需求**，所以逃生门有存在价值 | — |
| **VulkanSceneGraph** | 它**就是** Vulkan；类型即 API | 反例：单一 API 引擎不需要边界 | 本仓库要保留第二个后端的可能，所以不学 |
| **VMA** | Vulkan-only 的下一层（分配器） | 不适用（层级不同） | — |

### 3.2 归纳

三条几乎无例外的规律：

1. **没有一个多 API 引擎把原生类型放进核心接口。**（wgpu / Diligent / QRhi / Godot / Filament / UE 全部如此）
2. **只要存在真实互操作需求，就有一扇"逃生门"**——但它**可选**（单独的头 / 接口 / cast / 枚举）、
   **有名字**，并且**不是抽象类型上的方法**。
3. **纯单 API 引擎（vsg、VMA）不做边界**，因为边界没有第二个实现要保护。

本仓库同时满足 1 与 2 的条件：有多后端的意图（`RenderBackendRegistry` / `RenderApi`），也有真实的工具需求
（学习、抓帧、实验）。所以正确答案不是"禁止"也不是"随便"，而是**第 2 条的形状**。

## 4. 论证

### 4.1 论点一：暴露的代价是**契约**，不是编译

把 `VkDevice activeDevice()` 加到 `RenderDevice` 上，代价不是"多一个 include"，而是：

- DX12/Metal 后端**无法实现**这个动词（没有对等物）⇒ 只能返回 null 或 `void*`，契约立刻多一个空洞（`G4`：
  失败方式要明确，而"这个后端没有原生设备"不是失败，是**不适用**）；
- 反过来，一旦宿主用上它，它就**再也不能删**（`G9` 的演进被锁死）；
- 而它服务的场景（拿 VkDevice 做实验）**不需要**经过 SDK：插件自己的头就能给。

⇒ **SDK 动词表里加原生访问，是花掉一个永久契约去换一次便利**（`G10` 算账：收益一次性，成本永久）。

### 4.2 论点二：插件内部的暴露成本是**零**

证据（§2.1）：插件 57 个头里 18 个已经含 `Vk*`，`shell/` 是 0。也就是说：

- 插件内部暴露 Vk **今天就在发生**，且**没有产生任何维护问题**；
- 它只影响"插件 + 它的测试"，而测试本来就要建管线、读像素。

⇒ 对 `internal/` `support/` 加限制，收益是 0，成本是"为了绕开限制而造影子类型"（下一节）。

### 4.3 论点三：危险的只有"会被别人 include 的那一半"

消费者判据（§2.2）给出严格程度的排序：

```
宿主面   > SDK 后端面 > 插件 shell > 插件 internal/support
（最严）                                      （最松）
```

- **宿主面**：一条 Vk 类型 = 所有应用依赖 Vulkan。必须门。
- **SDK 后端面**：一条 Vk 类型 = 所有后端依赖 Vulkan，且**安装包**把 Vulkan 变成依赖。必须门（已有 C1/C6）。
- **插件 shell**：今天只有插件自己 include、不安装 ⇒ **没有受害者**（`G8`：没有消费者的约束不做）。
  它作为**形状**保留（将来一旦被宿主 include，它就会变成第二严的一层），但**不该是门**。
- **插件 internal/support**：本来就该有 Vk。禁止它只会逼出影子类型。

### 4.4 论点四（反方最强的一击）：学习后端**确实需要** Vk

诚实面对：`vk` 的**目的就是学习 Vulkan**。想做的事包括

- 用 RenderDoc / Nsight 抓帧（通常不需要你的 `VkDevice`，但需要 `VK_LAYER` 可注入）；
- 挂 `VkDebugUtilsMessengerEXT` 自己的回调、跑校验层实验；
- 写"同一帧用两种 barrier 方案"的对照实验；
- 拿 `VkPhysicalDevice` 去枚举扩展、做能力探测脚本。

这些需求**不能靠"再多一个中性 API"满足**——中性化恰恰会把它变成没东西可学。

⇒ 结论不是"收紧"，而是**给它一扇形状正确的门**。

### 4.5 论点五：门的形状 = 显式、可选、后端特有

对照 §3.1 的规律 2，本仓库里最合适的位置是 **`support/`**（vsg 已经用 `support/` 放"测试与工具用、不安装"的头）：

```cpp
// include/vine/backend_vk/support/Handles.hpp   ← 含 Vk 类型；不安装；不进 shell；不进 SDK
struct vn::backend_vk::Handles {
    VkInstance       instance{ VK_NULL_HANDLE };
    VkPhysicalDevice physical{ VK_NULL_HANDLE };
    VkDevice         device{ VK_NULL_HANDLE };
    VkQueue          graphics{ VK_NULL_HANDLE };
    std::uint32_t    graphics_family{ 0 };
};
[[nodiscard]] vn::backend_vk::Handles vkHandles(const RenderDevice& device);  // 非 vk ⇒ 空结构
```

三条性质让它不是"暴露契约"：

1. **它不是 `RenderDevice` 的方法**——是自由函数，参数是 `const RenderDevice&`，所以 SDK 动词表不变（`G2`）；
2. **它的返回值不假装通用**——没有 `native_handle()` 这种中性名，名字里就写着 `vk`（`G4`：语义明确）；
3. **它在 `support/`**——测试与工具能 include，宿主与 SDK 不能（`BackendHalfGuard` 之外再加一条同形的守卫，
   或用 `check_include_hygiene.py` 约束）。

这与 Diligent 的 `ITextureVk`、QRhi 的 `nativeTexture()`、Godot 的 `get_driver_resource()` 是同一个形状，
只是本仓库没有 GPU 资源对象可挂，所以改为**一个 handles 值**（`G1`：值的形状与本仓库一致）。

### 4.6 反论点与回应

| 反论点 | 回应 |
|---|---|
| "有逃生门，抽象就漏了" | 逃生门是**单向**的：SDK 从不依赖它，DX12 后端不需要它，宿主不 include 它。漏的是**用户选择逃出去**，不是抽象泄漏（`G8`） |
| "那不如干脆别通用化" | 通用化的收益是**规则复用**（pin / 退役 / 驻留 / 目标决策 / 管线键），与 Vk 访问是两件事；逃生门不削弱它们 |
| "学习用途干脆别做成插件" | 做成插件正是学习目标之一（`RenderDevice` / `RenderFrameExecutor` 的真实契约）；独立程序学不到接缝 |
| "`support/` 的头将来会被装" | 那就触发升级（§8）：届时按 §6.1 给它加守卫 |

## 5. 结论

### 5.1 三层边界（一句话版）

| 层 | Vk 类型 | 规则 | 今天的状态 |
|---|---|---|---|
| SDK 宿主面 `vine/graphics/*.hpp` | **禁止** | 出现即破坏所有宿主的可移植性 | **0 类型**（只有注释里的文字） |
| SDK 后端面 `vine/graphics/backend/*.hpp` | **禁止**（可用 Vulkan **语义**） | C1 / C6：不 include，不新增拼写 | **0 类型**、0 include |
| 插件 `shell/` | **形状上禁止**（不是门） | 工厂/入口只露 `RenderBackend` | **0** |
| 插件 `internal/` `support/` | **允许** | 随便用；`support/` 是工具的逃生门 | 18/57 个头 |

### 5.2 逃生门的纪律

- 生产路径**不许**走逃生门：`vk` 自己的帧路径只用插件的 `internal/`，不 include `support/Handles.hpp`；
- 使用方是**测试、工具、实验**，且一次使用要在注释里说明"为什么中性 API 不够"；
- 逃生门**不进** SDK、**不进** `shell/`、**不安装**。

### 5.3 一句话判据

> 问"这个暴露会不会让**不写 Vulkan 的人**多一个依赖"——会 ⇒ 只能进 `support/` 或 `internal/`；不会 ⇒ 随便。

## 6. 可检查性

### 6.1 能成为门的（有受害者）

| # | 门 | 检查方式 |
|---|---|---|
| V1 | SDK 宿主面零 Vk 类型 | `grep -rn "Vk[A-Z][A-Za-z]* [a-z_]*[;{]" src/viz/graphics/sdk/vine/graphics/*.hpp` 为空（今天的 3 处命中全是注释与字符串，已被 `check_doc_symbols.py` / 人工区分） |
| V2 | SDK 后端面零 Vulkan include | `grep -rn "#include <vulkan\|#include <vsg" .../backend/` 为空（**今天已是 0**） |
| V3 | 安装组件不含 Vulkan 头 | `install_manifest_VineGraphicsBackendSdk.txt` 的 include 列表不含 `vulkan/` |
| V4 | 插件 `shell/` 零 Vk | `grep -rl "Vk[A-Z]" src/plugins/*/include/vine/*/shell/` 为空（**今天已是 0**） |

V2 / V4 今天已经是事实，可以直接写成脚本断言；V1 需要"只数类型不数注释"，成本略高，可先不做。

### 6.2 只能是形状建议的

`internal/` 与 `support/`：**不加门**。理由（`G8`）：

- 没有受害者（§2.2）；
- 加门会逼出影子类型（把 `VkImage` 包成 `vk::Image`），那是**用重复换取"看起来干净"**；
- 与 vsg 的现状不一致（vsg 的 `internal/` 就有 Vk），破坏 `G6`。

### 6.3 为什么把"D2"降级

上一轮我给 vk 立的 "D2：`Vk*` 只允许 `src/internal/`" 有两个问题：

1. **与仓库分层不符**：vsg 的 `internal/` 头在 `include/vine/backend_vsg/internal/`，不在 `src/`；照 D2 写会与周边不一致（`G6`）；
2. **没有受害者**：插件头不安装、`shell/` 无人 include ⇒ 它是一道只增加维护成本的检查（`G8`）。

所以本节把它换成 **V4（`shell/` 零 Vk）**——同一个意图，**只对今天真有意义的那一半**，并保留升级条件。

## 7. 已否决选项

| # | 选项 | 理由 |
|---|---|---|
| D1 | 在 `RenderDevice` 上加 `nativeDevice()` / `nativeInstance()` | DX12/Metal 无法实现 ⇒ 契约出现空洞（`G4`）；一旦被用就删不掉（`G9`）；需求可用 `support/` 满足 |
| D2 | 让 `backend/` 改成完全不提 Vulkan（连注释与语义都不提） | 现有拼写已被 C6 冻结；改名成本 53（`ImageLayout`）+ 62（`device_color_formats`）+ 36（`subpass`）处引用，收益为零（`G8`）。真正的解是"不加新的"，不是"擦掉旧的" |
| D3 | 插件头全部 Vk-free（含 `internal/`） | 逼出影子类型；与 vsg 现状不一致（`G6`）；没有受害者（`G8`） |
| D4 | 统一 `GpuResource` 基类 / 多态总表 | 见 `ADR-2026-11-10` 选项 D 与本文档 §3 的评估：没有跨 API 消费者，且覆盖不到流/描述集/管线 |
| D5 | 把逃生门做成 `virtual void* nativeHandle() const` | 名字中性而语义不中性，正是 `G4` 说的"靠隐含约定"；改成带后端名的自由函数后，语义写在名字里 |

## 8. 触发与验收

**升级为门**（把 §6.2 的形状建议变成检查）：

- 插件头开始被安装（`install(DIRECTORY include/...)`），或
- 出现第二个 include 插件 `shell/` 的模块，或
- `support/` 被生产路径 include。

**推翻本文**：

- 宿主需要**直接给插件喂** Vulkan 对象（例如把外部 `VkImage` 交给引擎画）——那时需要的是**导入通道**
  （像 `ImageSource` 导入纹理那样，作为值/描述进 SDK），不是 `nativeDevice()`；
- 第二个 API 家族落地且逃生门成为**唯一**可用的互操作方式（那就说明 `FrameDescription` 的值不够，问题在值层）。

**vk 的落地判据**（与评估 §9.6 的 D1–D5 一致）：

- `backend/` 公开面零 diff；
- `shell/` 零 Vk（V4）；
- `support/Handles.hpp` 存在且**不被** vk 自己的帧路径 include；
- 同一 `FrameDescription` 与 vsg 的对拍通过（M7）。

## 9. 参考

**本仓库**

- `../design/vsg-backend.md` C1（SDK `backend/` 不 include vsg）与 C6（新增 Vulkan 拼写冻结）
- `../design/graphics-architecture.md` 2.9（`backend/` 是后端接缝、单独安装）
- `../design/graphics-threading.md` §2（跨侧只传值，不传活对象）
- `../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md`（选项 D 否决资源基类；"其它库怎么做的"一节的写法是本文的体例来源）
- `../decisions/ADR-2026-11-13-content-seam-stays-in-the-plugin.md`（内容缝留在插件内）
- `graphics-2026-11-13-gpu-resource-abstraction.md` §8（vk 的形态与里程碑）、§9（通用化边界 J1–J3）
- 代码：`src/viz/graphics/sdk/vine/graphics/backend/BackendHalfGuard.hpp`、`scripts/check_include_hygiene.py`、`cmake/VinePluginHelper.cmake`

**外部**

- wgpu：`wgpu-hal` 作为后端 trait 层，原生访问是显式、可选的 cast
- Diligent Engine：`IRenderDeviceVk` / `ITextureVk::GetVkTexture()`（后端特有接口）
- Qt QRhi：`QRhiTexture::NativeTexture`、`QRhiVulkanNativeHandles`
- Godot：`RenderingDevice::get_driver_resource()`（命名枚举的逃生门）
- bgfx：`bgfx::PlatformData`（只在 init 暴露平台句柄）
- Filament：`backend::` 内部抽象 + API 无关的公共 `Engine`
- Unreal RHI：`FRHIResource::GetNativeResource()` + 平台子类
- Dawn：`dawn_native` 的原生访问用于 Chromium 互操作
- VulkanSceneGraph / VMA：单 API 层，作为"不需要边界"的反例
