# gfx_backend_vsg 问题描述

> ## ⚠️ 状态：本文件的问题已全部解决（2026-09-11 复核）
>
> 本文件是历史上的一次问题记录（2026-09-08 提出）。**下面列出的 5 项均已修复**，
> 正文保留作为问题描述的历史来源，**不要按正文“期望”重复实现**：
>
> | 问题 | 现状 | 位置 |
> |---|---|---|
> | 一 索引未校验 | **已修**：越界索引拒绝建节点并在 CPU 侧防御（`unpackXyz` / `makeIndexedNormals` 越界跳过）；索引保真不再按三角形截断，count == 源索引数 | `SceneBridge::buildGeometryData` |
> | 二 分量步长错误 | **已修**：`unpackXyz` 以 `components` 为 stride，仅接受 3/4 分量并取 xyz，不可整除/非 3-4 分量则拒绝并诊断 | `SceneBridge.cpp` |
> | 三 自定义属性被丢弃 | **已修**：loc>=3 全量转发（`vine_Attribute{L}` + components→VkFormat + 按布局缓存 ShaderSet）；loc2 在 custom 程序路径按 authored 颜色绑定 | `SceneBridge.cpp`、`tests/test_vsg/CustomAttributeTest.cpp` |
> | 四 clear 未遵守 clearDepth/离屏语义 | **已修 + 语义收窄**：离屏 target 持久化 clear 色/深度策略（含 depth-LOAD pass）；window 由 vsg 拥有交换链 pass，深度恒清，已在 `RenderBackend::clear` / `RenderPass::setShouldClearDepth` / `VsgRenderer::clear` 三处文档化收窄 | `VsgRenderer::clear/buildOffscreenTarget` |
> | 五 插件/Registry 生命周期导致退出崩溃 | **已修**：插件库进程期存活（`DynamicLibraryLoader` 不再 dlclose）；测试退出码 0 | `fw/.../DynamicLibraryLoader.hpp` |
>
> 当前权威契约见 `.ai/design/vsg-pass-lifecycle.md`（pass 生命周期）、
> `.ai/design/vsg-custom-attributes.md`（自定义通道）、
> `RenderBackend.hpp`（各接口文档）。验证：`test_graphics` 135 / `test_vsg` 53 /
> `scripts/gfx_lavapipe_check.sh` RESULT: PASS。

## 背景

`gfx_backend_vsg` 负责将 Vine 的 `Geometry`、渲染状态、RenderTarget 与
ShaderProgram 转换为 VulkanSceneGraph（VSG）对象。当前实现已具备 retained
scene、离屏目标、MRT 和运行时 GLSL 编译等能力，但部分路径与 graphics SDK 的
公开约定不一致，且存在数据越界和退出崩溃风险。

## 复现结果

在仓库根目录执行：

```bash
ctest --test-dir build -R 'vsg|gfx' --output-on-failure
```

`test_vsg` 的 29 个断言全部通过，但测试进程会在全局/静态对象析构阶段以
`SIGSEGV`（退出码 139）结束。因此 CTest 将该测试标记为失败。

## 问题一：索引数据未校验，可能越界访问

**位置**：`src/SceneBridge.cpp`

`makeIndexedNormals()` 直接使用索引访问 `positions`，而
`buildGeometryData()` 会把全部原始索引上传并交给 `DrawIndexed`。两处均未验证
索引是否落在 `[0, positionCount)` 范围内。

### 影响

- 非法索引可造成 CPU 侧越界读取和未定义行为；
- 即使未在法线计算处触发，非法索引仍会进入 Vulkan 绘制，可能触发 validation
  error、错误画面或设备异常；
- 该风险来自公开 `Geometry::setIndices()` 输入，属于后端必须防御的数据边界。

### 期望

- 在解包/上传前验证每个索引；
- 空索引、非法索引以及不完整图元应安全拒绝或按明确规则处理；
- 被拒绝的 geometry 不得进入 GPU 绘制。

## 问题二：AttributeBuffer 的分量步长处理错误

**位置**：`src/SceneBridge.cpp`

`AttributeBuffer::components` 的接口允许 1–4 个 float 分量，但位置和法线的
解包逻辑始终以 3 个 float 为步长读取。

### 影响

- `components == 4` 的位置/法线数据会错位读取；
- 顶点数、坐标和自动法线均可能错误；
- `data.size()` 不能整除 `components` 时没有明确的安全处理。

### 期望

- 用 `components` 作为输入 stride；
- 位置和法线要求至少 3 个分量，读取 `xyz` 并忽略可选的第 4 分量；
- 对不完整 buffer 返回可诊断的失败结果，不创建绘制节点。

## 问题三：自定义顶点属性被静默丢弃

**位置**：`src/SceneBridge.cpp`

graphics SDK 将 Geometry 定义为开放的 attribute-location 容器，但 VSG bridge
仅处理 location 0（position）和 1（normal），并自行生成 location 2（color）。
自定义 ShaderProgram 也只声明了这三个输入。

### 影响

- location >= 3 的用户 attribute 不会传入 VSG；
- 点云颜色、尺寸、实例数据及其他自定义 shader 输入无法正常使用；
- 行为为静默降级，调用方难以定位问题。

### 期望

- 保持内建 0/1/2 的兼容规则；
- 为自定义程序遍历 Geometry 全部 attribute buffer；
- 根据 `components` 映射 Vulkan float format：1/2/3/4 分量分别对应
  `R32_SFLOAT`、`R32G32_SFLOAT`、`R32G32B32_SFLOAT`、`R32G32B32A32_SFLOAT`；
- 为自定义 location 使用稳定、可文档化的 shader binding 命名。

## 问题四：clear() 未遵守 clearDepth 与离屏目标语义

**位置**：`src/VsgRenderer.cpp`

`VsgRenderer::clear(const Color&, bool clearDepth)` 当前忽略 `clearDepth`，并固定
将 window 深度清为 `0.0`。离屏目标的 clear value 在创建 graph 时写死；后续
`clear()` 仅更新 window graph。

### 影响

- 调用 `clear(color, false)` 仍会清深度，破坏多 pass 深度复用；
- 离屏 RenderTarget 无法使用调用方设置的 clear color/depth；
- `RenderBackend::clear()` 的公开契约在不同 target 上行为不一致。

### 期望

- `clearDepth == false` 时保留深度附件内容；
- clear color/depth 应应用于当前 target，包括 window 和 off-screen target；
- 保留现有“clear 后为主场景 depth-on”的层级判定，但该判定不能替代实际的
  attachment clear 语义；
- 离屏 target 重建后应保留最近一次有效 clear 配置。

## 问题五：插件/Registry 生命周期导致测试退出崩溃

**位置**：`src/GfxBackendVsgPlugin.cpp`、`RenderBackendRegistry`

VSG 插件将 DLL 内的静态 `VsgRenderBackendFactory` 以裸指针注册到进程级
`RenderBackendRegistry`，而 plugin 的 `unload()` 没有注销。registry 也没有公开
注销机制。测试退出时已观察到 SIGSEGV，表明跨模块静态对象析构顺序或已失效 factory
引用存在风险。

### 期望

- 明确 factory 的所有权与生命周期；
- 提供 `unregisterFactory()` 或等价的安全机制；
- plugin unload 时清理注册项，且不会移除其他同名/不同实例的有效 factory；
- 不依赖跨 DLL 静态析构顺序；
- 修复后测试必须以退出码 0 正常退出。

## 建议验收测试

1. 非法索引、空索引及不完整图元不会崩溃或提交非法绘制。
2. vec4 position/normal 能按 xyz 正确解包；不完整 attribute buffer 被拒绝。
3. 自定义 location 的 attribute 能被自定义 ShaderProgram 接收。
4. `clearDepth=false` 保留深度；window 和离屏目标均采用请求的 clear 值。
5. `ctest --test-dir build -R 'vsg|gfx' --output-on-failure` 全部通过且进程退出码为 0。
