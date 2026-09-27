# graphics / vsg 后端性能待办（2026-09-13 立项）
> ⚠️ **历史与对账（2026-09-25）**：本文立项于**老渲染器**（SceneBridge/VsgRenderer）时代；那套实现已被重写版
> （`core/`+`api/` 分层，见 `.ai/design/vsg-reimplementation.md`）替换，所以 §0 的机制表与 §1 各表里
> **针对老后端的实现结论（P1–P17、R1–R5、V1–V7 的行）不要再按它们去改代码**（作为方法与教训仍可读）。
> **仍然活跃的登记**：H1 的两条残留（Windows 上没有读像素等价物；“拉伸窗口看画面跟随”仍靠人工）、
> H2 的真实桌面目视（机制已定案，见行注）、V2/V3/V4（升级 vsg 时逐条复核；能力断言在
> `scripts/check_vsg_upstream_capabilities.py`）。**后端活跃登记以
> `.ai/design/vsg-reimplementation.md` §6 为准**；图形侧当前状态见 `.ai/memory/graphics.md`。
> **已结**：P18（§10.1+§10.2 均于 2026-09-25 落地，见其行注）；**H3**（2026-09-25 实测无害，见行注与实测块）。

本文记录"大场景 + 相机常动 + 多 pass"下的性能结论与待办项。
来源：一次代码走查（`Scene::collectRenderCommands` / `SceneBridge` / vsg 1.1.16），
**未实测**的数字都标了「估算」。

## 0. 已核实的机制（避免重复调研）

> **R6 已落地（2026-09-19）：设备侧逐 pass GPU 时间。** 开关 `VINE_VSG_PROFILE=1`（会话建立时读），
> 值出口 `VsgRenderer::gpuProfile()`，机制与判据见 `src/plugins/gfx_backend_vsg/docs/backend.md` §5.7。
> **做任何 GPU 侧结论前先用它**。两条实测前提：①样本**永远落后几帧**（回读不等待设备，`age_frames ≥ 1`）；
> ②**软件设备上每个 render pass 自身就有 ≈2 ms 固定开销** —— 1 个全屏 quad ≈2.1 ms、1024 个全屏 quad
> ≈7.7–8.6 ms（×3.6–4.0），而 64 个落在噪声里（×0.8）：**测 GPU 工作量差异要拉开一个数量级**，
> 否则量到的是 render pass 的地板而不是内容。
| 收集的 memo 键 = `(projection×view, eye, 场景内容版本)`，每帧边界失效 | `src/viz/graphics/src/Scene.cpp:398-420` | **相机一动每帧必 miss** ⇒ 每帧一次全树走 + 剔除 + 排序 |
| 每个未剪掉的 geometry 算局部 AABB = **扫全部顶点**，且无缓存 | `src/viz/graphics/src/Geometry.cpp:238`（`transformBox(localBounds(this), worldMatrix())`） | 大网格场景里"收集"的成本是 O(可见顶点数)/帧，不是 O(节点数) |
| 剔除是 CPU 侧节点级 AABB（p-vertex 测试），无 GPU 剔除/无遮挡剔除 | `Scene.cpp:59-113`、`Scene.cpp:207-213` | 根容器罩全场 ⇒ 所有子节点都要先算盒再判掉 |
| vsg 的重传粒度是**一条 `BindVertexBuffers` 命令**：任一 array 脏 ⇒ 该命令全部数组一起重新预留+重传 | `build/_deps/vsg-src/src/vsg/commands/BindVertexBuffers.cpp:84-108` | 想要"只传变了的通道"必须**每通道一条 bind 命令** |
| 设备内存池化：一条命令的数组进**同一块池 buffer**、按 offset 分片 | `build/_deps/vsg-src/src/vsg/vk/Context.cpp`（`createBufferAndTransferData` → `deviceMemoryBufferPools->reserve`） | 每命令的分配很便宜；重建的真实成本 ≈ 拷贝字节数 |
| 每帧动态数据检查 = 已注册的动态 BufferInfo 逐个比 `modifiedCount` | `build/_deps/vsg-src/include/vsg/state/BufferInfo.h:69`、`vsg/app/Viewer.cpp:402` | 全量 DYNAMIC 化 = 每帧 O(#动态数组) 次比较 |
| 上传批量：一帧一个 staging buffer，按 `(VkBuffer, offset)` 去重，多区域一条 copy | `build/_deps/vsg-src/src/vsg/app/TransferTask.cpp:110-177` | 稳态帧零传输；新数据的成本是 Σ字节 |
| `AttributeChannel = {values, components}`，**无 offset/子区间**；`aliasArray` 一律 offset 0 | `src/viz/graphics/sdk/vine/graphics/Geometry.hpp:52-…`、`VsgSceneRules.hpp`（`aliasArray`） | "一个 arena buffer + 每 geometry 一段"**原本表达不了**。**已修（P7，2026-09-13）**：`offset`/`scalarCount`（标量计）+ `slice()`；别名按 offset 换算字节起点；索引侧用 `DrawIndexed(firstIndex)` 而绑定仍是整段缓冲 |
| vsg 的 **BufferInfo 是 per-command 的**：新建数据节点就建新 BufferInfo/Buffer，即使里面装的是同一个 Data 对象 → 仍然从头 reserve + 重拷 | `vsg/commands/BindVertexBuffers.cpp:45`（构造时 per-array 建 BufferInfo）、`vsg/vk/Context.cpp`（`createBufferAndTransferData` → 池 reserve）、`vsg/state/BufferInfo.h:69`（`requiresCopy` 比 modifiedCount） | 想只重传一个通道，**必须保留承载它的命令对象**、只把那个通道的 `data` 换掉（见 P6） |
| 同一 buffer 被多个 geometry 别名 ⇒ 每个 geometry 各自的 `vsg::Array`/`BindVertexBuffers`/`BufferInfo`（内容不去重） | vsg `BindVertexBuffers::assignArrays` per-command 建 `BufferInfo` | CPU 侧零拷贝共享，GPU 侧各自上传（**估算**，需实测确认）；**只共享 `vsg::Array` 对象还不够** —— 每条命令各自建 BufferInfo、各自 reserve ⇒ 必须共享**承载 BufferInfo 的命令对象**。**已修（P9，2026-09-13）**：别名自模型缓冲的流（位置/作者法线/作者 UV/四分量 loc2 颜色/索引）改为经会话级 `VsgMeshResourceCache` 取同一条 bind |
| `string(SHA256 "…" var)` 在本机 CMake 4.2.3 上**恒返回空串**（MD5/SHA1/SHA3 同样空）；`file(SHA256 <path> var)` 正常 | 实测（`cmake -P`）：`string(SHA256 "abc" s)` ⇒ `s` 为空；`file(SHA256 <shader> f)` ⇒ 64 位十六进制 | 生成器算哈希**必须走 `file(<algo> <path>)`**；`string(<algo> <string>)` 在本环境不可用 |
| `file(READ <path> var)` **会把 CRLF 归一化成 LF**（要原始字节得用 `HEX`） | 实测：CRLF 写回的 shader 读进来没有 CR；`file(READ … HEX)` 能看到 `0d` | 想守"嵌入文本 == 磁盘字节"必须用 HEX 检查 CR；否则 CRLF 文件会静默变成 LF 版本（与磁盘文件不再逐字节相同） |
| `glslangValidator -V <shader>` **不带 `-o` 会把 `vert.spv`/`frag.spv` 写进当前目录** | 实测：在仓库根跑校验后 `git status` 多出 `vert.spv`/`frag.spv` | 校验脚本必须给 `-o <tmpdir>/x.spv`，否则门禁自己污染工作区 |
| **跨槽共享 `shared_objects_` 不可行（实测）**：vsg 的 `GraphicsPipeline::compile` 复用已有实现时**只比 `_pipelineStates`，不比 render pass**（`build/_deps/vsg-src/src/vsg/state/GraphicsPipeline.cpp:177`，实现用 `context.renderPass` 创建于 `:218`），而后端刻意按 pass 变体建不同的 `VkRenderPass`（§5.4） | 实验：把会话级 `SharedObjects` 注入每个槽的 bridge 后，门禁的 policy-churn 相位失败（“depth target's centre holds 0.0000, expected ~0.0249”）；回退后相位全绿 | 第二个 view 会拿到用不兼容 render pass 编译的 pipeline ⇒ **槽的管线注册表必须私有**（原因已写进后端半存储层的注释） |
| ✅ **已修（2026-09-19）：`frontFace` 与 vsg 的 Y 翻转反了（D6）** | SDK 合约：世界空间 CCW = 正面（`StateNode.hpp:32-41`）；vsg 投影反 Y（`vsg/maths/transform.h:140` 的 `-f` 项 + “Y NDC coordinates are inverted in Vulkan”）⇒ 帧缓冲里 SDK 的正面是 CW | `RenderStateMapper.hpp:181` 原来写死 `COUNTER_CLOCKWISE` ⇒ `CullMode::Back` 剔掉 SDK 的正面（demo 的 `culled_box` 画的是内壁，零报告）。**修法**：改为 `VK_FRONT_FACE_CLOCKWISE`；门禁 `RenderStateMapperTest.*`（注释钉住“掩码与 front face 是一个决定”） |
| **上游对齐审查（2026-09-19）**：矩形/视口、阶段缓存、编译模型、管线静态 viewport 四条**与上游同构**；跨槽共享只差"按 render pass 分表"这个安全版本（收益有上限，触发条件未到） | `.ai/design/vsg-upstream-alignment.md` §1（逐条 file:line，含 vsg 侧 `RenderGraph.h:71` 默认 `DYNAMIC_VIEWPORTSTATE`、`State.cpp:68` pushView、`Context.cpp:134`、`CompileTraversal.cpp:95/171`、`GraphicsPipeline.cpp:167-179`） | 这一族别再当成"可疑用法"重新调研；要动只有 §4 那一个安全版本 |
## 1. 待办项（按建议顺序）
| **H2** | 继承来的 `pollEvents()` 会不会偷 Qt 的事件 | **已定案（2026-09-25）：机制上不可能，三层独立**——① `Session.cpp:69` 的 `EmbeddedViewer` 覆写**什么都不泵**（“宿主拥有消息循环”）；② 全仓 `grep pollEvents`：`Xcb_Window::pollEvents` **无调用方**；③ 即便被调，采纳路径**从未对宿主窗选过事件掩码**（vsg 源码：掩码只在 `createWindow` 分支的 value list 里）⇒ 我们这条连接收不到 Qt 窗口的任何事件 | 真实桌面**边缩放边点菜单**的目视确认降级为可选（剩余风险≈0）：本机无法指针交互（headless Weston 把窗口放 `(-32768,-32768)`，`XMoveWindow` 被立即还原，日志 `visible=false`） | 只在真实交互下暴露（现由构造排除） | **机制收口（2026-09-25）**；可选目视留真实桌面 |
> **H2/H3 实测（2026-09-25，本机 X11 = Weston 的 headless 后端，root 3840×2160）**
> · **窗口树事实**：渲染子窗（挂接目标，Qt 创建）出生 `XdndAware=None`，**挂接瞬间**变 `[5]`（20 ms 采样 `t=1.81 None` → `t=1.83 [5]`，应用日志 `22:55:42.388 attached to the host window 0x800027`；源码侧 `Xcb_Window::_initXdnd()` 注释原文 “Advertise support for version 5”）；Qt 顶层 `0x80000f` 出生即 `[5]`（vsg 未触碰）。
> · **模拟 XDND 拖放**（假源：libX11 ctypes，Enter v5 + Position + Drop，`text/uri-list`/copy）：打给渲染子窗 ⇒ `XdndStatus accept=0` + `XdndFinished 未接受`；打给 Qt 顶层 ⇒ **同样**。应答者 = Qt（窗口的创建者；Xdnd 消息以 `event_mask=0` 发送 ⇒ 投给创建者，vsg 的连接收不到）。全程应用存活、vsg 零日志（无文件名抓取、无 3 s 超时告警）。
> · **交互上限（H2 目视项降级的根据）**：本显示上窗口被放在 `(-32768,-32768)`（Weston 无输入后端），`XMoveWindow` 立即被还原 ⇒ XTEST 指针事件到不了窗口；合成 `XSendEvent` 点击可**部分**被 Qt 处理（唤起了一个惰性窗口，但从未映射）。⇒ “边缩放边点菜单”留真实桌面一次目视；缩放后的画面跟随本身已由 app 门禁覆盖（resize + 判图）。
> · 复现：ctypes 三件套（窗口树/`XdndAware` 读取、假 XDND 源、`XSendEvent`/XTEST 点击，探针跑完即弃）+ `scripts/xwinresize.py` / `scripts/xwin2ppm.py`。
| **R5** | appfw 的插件注册表在 LSan 下**每次都报**（后端门禁里唯一的报告） | `vn::runtime::DynamicLibraryLoader` 单例（`.cpp:76` 构造、`:190` load）→ `PluginManager::loadAll`：跑完 `test_vsg` 报 808 B / 16 次分配，栈里**零 vsg 帧** | 二选一：让 loader 在退出时释放，或把它声明为“进程生命周期保留”并进 `scripts/asan_leaks.supp` —— **但那份文件自己的规则写着“框架自己的分配不许抑制”** ⇒ 更可能要改代码 | 属 appfw，不在后端范围 | **已结（2026-09-16）：抑制，但把“为什么是有意的”写进允许清单**。查证：`~DynamicLibraryLoader` 的 `d.release()` **是有意的**（注释写明：在静态析构期 dlclose 掉插件代码，而 CommandManager / RenderBackendRegistry 还持着指进插件的 callable/工厂指针 ⇒ exit 时 SIGSEGV）⇒ 这是**保留决定**，不是忘记释放。处置：`asan_leaks.supp` 顶部规则加一条例外（“有意保留 + 决定写在代码里”可入表），新增 `leak:vn::runtime::DynamicLibrary` 并附实测数字。**证据**：`print_suppressions=1` ⇒ `count 16 bytes 808 template vn::runtime::DynamicLibrary`（**只**这一类被匹配）；此后后端两条跑法（套件 + 设备自检）都用**全或无**泄漏判据 PASS，`VINE_ASAN_LEAK_SCOPE` 降级为“还没excuse 的泄漏”的备用手段 |
| **V2** | 靠 `protected` 的池做出上游没有的 `remove(view)` | `detail::VsgCompileManager : vsg::CompileManager`，用 `protected` 的池实现 `forget(view)`（`VsgViewCompiler.hpp:26`、`docs/backend.md:54`）；HEAD 实测 `forget` 93 次 | 低—中：依赖 vsg 的 protected 布局，但比打补丁干净 | 不改；把"升级 vsg 时同时复核 V1 + V2"写进升级清单 | 待办（仅登记） |
| **V3** | 拿不到 `VkPipelineCache`（D28） | vsg 的 `GraphicsPipeline::compile` 不接受 `VkPipelineCache` ⇒ 跨会话/磁盘的 PSO 复用做不到；两条路都堵（等上游 / 自建管线，后者不推荐） | 中 | **优先级应上调**：H7/H8 刚量出"编译"是启动成本的大头（但**应用只编一次** ⇒ 受益集中在冷启动与换会话）。把那些测量数字挂到这条上，作为推上游的理由 | **2026-09-17 收口：前提从"假设"升级为"有门禁的事实"，测量挂上（不改代码）**。① **"拿不到"现在是有出处的三处**：pinned v1.1.16 与**上游 master（2026-09-17 现抓）**都是 `vkCreateGraphicsPipelines(*device, `**`VK_NULL_HANDLE`**`, …)`（`src/vsg/state/GraphicsPipeline.cpp:260`；compute `ComputePipeline.cpp:110`、ray tracing `RayTracingPipeline.cpp:168` 同样）；vsg 自己的代码里除 Vulkan 头文件外**根本不出现 `VkPipelineCache`**，也不存在 `PipelineCache` 类型 ⇒ **升到 master 也拿不到**（"等上游"变成核实过的结论）。② **门禁**：新增 `scripts/check_vsg_upstream_capabilities.py`（7 条断言；无 vsg 树则 SKIP 退 0），把上面的话变成可红的检查，**变异 4 条全红**：让 vsg 传一个真 cache 句柄 / 在 vsg 头里加 `VkPipelineCache` / 往 `RenderGraph.cpp` 塞 `vkCmdBeginRendering` / 造一个 `PipelineCache.h`。③ **要挂的测量（H7/H8）**：编译的钱几乎全在 glslang 重解内建符号表（每次 ≈ **50 ms**；会话重建 ⇒ 17 / 29 / 31 个 epoch），而 PSO 只在首次为 (program, material, state) 变体建一次；**交付的应用只编一次**（25 s 里 `init=2 finalize=1`）⇒ 收益集中在**冷启动**与**换会话/换窗口重建**（自检那种负载），不在稳定帧率上。④ **推上游的最小形状**（照 vsg 自己的先例）：`Context` 已有 `getOrCreateShaderCompiler()`（`include/vsg/vk/Context.h:88`），要的等价物就是一个 `getOrCreatePipelineCache()`，`Implementation` 里把那第三个实参从字面量换成它、默认仍是 `VK_NULL_HANDLE` ⇒ 零破坏，且**跨会话/落盘复用**随之可用（D28 的"`pipelineCacheUUID` 不匹配必须安全丢弃"跟着生效）。⑤ 拿到注入点之前**不改**：路线 B（自建管线）仍不推荐（会与 vsg 的 state 装配/记录路径分叉） |
| **V4** | 合并每 pass 的 `beginRenderPass`（dynamic rendering，§9.4） | vsg 的记录路径只有 `vkCmdBeginRenderPass`（全树无 `vkCmdBeginRendering`）⇒ 无法在它的遍历里插一个 | — | 保持"被上游阻塞"，不动 | **2026-09-17 收口：前提同样变成有门禁的事实**。pinned 与**上游 master** 的 vsg 记录路径都**没有** `vkCmdBeginRendering`（`src/` 全树），只有 `vkCmdBeginRenderPass`（`src/vsg/app/RenderGraph.cpp`）；这两条由 `scripts/check_vsg_upstream_capabilities.py` 盯着（变异：塞一个 `vkCmdBeginRendering` ⇒ 红）。⇒ 结论不变（被上游阻塞、不动），但"为什么"不再只活在话里 |
### 2026-09-19 登记：启动/改尺寸的"现造的东西"值多少（一条已做、一条已否决）

> 量具：`VsgBuildProfile` + `reportBuildProfile()`（`VsgRendererState::build_profile`；行格式与用法见
> `src/plugins/gfx_backend_vsg/docs/backend.md` 顶部）。数字是本机 Debug + RTX 4060 + `Vine.exe`（默认 deferred + shadowed 管道），
> 只在该帧真造了东西或总耗时 > 5 ms 时打一行，稳态帧不打。
- **已做（2026-09-19）**：全屏 program 的 SPIR-V 按 (fragment 源码, entry point) **进程级缓存**
- **顺带把数字挂给 V3**：一次改尺寸的 6 个槽 = 6 次 `vkCreateGraphicsPipelines` + 6 套 pipeline/descriptor 布局 +
## 2. 需要实测的数字（还没有）

| 问题 | 怎么测 |
| --- | --- |
| 单个几何体重建 / 全场景重建 / 稳态帧各多少 ms | headless 合成基准：`N` 几何体 × 每几何体 `M` 顶点，量①首次 sync ②全部 bump `revision()` 那一帧 ③稳态帧；用 `VINE_VSG_DIAG_MRT=1`（per-slot `commands/created/rootChildren/variants`）+ 自计时 |
| 拆绑定（P6）后的帧率影响 | 同上基准，比较"1 条 bind"与"每通道 1 条 bind"的录制耗时 |
| 全量 DYNAMIC 化的每帧检查成本 | 量 `requiresCopy` 检查数与帧时间 |
| 同名/同内容 buffer 是否在 GPU 侧各占一份 | 统计 VkBuffer 数或池 `totalReservedSize()`（当前只有代码链推断） |
| 命中 P5 缓存 vs 每次重算的**实测耗时**差 | 同一基准下比较那两种重建帧（预期差在 CPU 推导，不在上传） |
| 纹理/mesh 共享后省下的设备内存与上传次数 | 基准：同一纹理被 N 个槽采样、同一 mesh 被 k 个实例绘制，统计池 `totalReservedSize()` 与上传字节 |
| 全局缓存的 sweep / 淘汰成本 | 每帧 sweep 的条目数与耗时（P2 候选表方案前后对比） |
