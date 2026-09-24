> 2026-09-17 **P18：剔除的盒成本 —— 这一轮唯一量出"大收益"的项（其余都在 0.0001% 量级）**
> 形状：`Scene.cpp:239` 的早退只省**下降**，而容器的盒是**后代盒的并集** ⇒ 每次收集给所有可达节点算世界盒，**与可见量无关**。
> · **实测（Release -O2，同 N、同"可见 95 条命令"）**：全部可见 72.8 ms ／ 99% 散落兄弟 **24.7 ms** ／ 99% 挂**一个视锥外 Group**
> **25.7 ms**（⇒ 把下降与逐节点测试**全省掉**只换来 ~4%，其余全是盒）／ 理想持久化代理 **0.19 ms** ⇒ 上限 **~99%**。
> 单位：**≈250 ns 每"存在但不可见"节点、每次收集**（Debug 3.4 µs）。
> · **修正我在同一轮里先说出的话**：我说过"收益被 0.00007% 限制"——那只对**我们手上的小场景**成立；这条是**规模化**问题
> （100k 节点 ⇒ 24.7 ms/帧全花在看不见的东西上，60 fps 下 1.5 个帧预算）。
> · 已写入 `.ai/design/graphics-scene-graph.md` **§9**（成本结构 + 可复现测量配方）与 **§10**（持久化方向、10.1 零风险前置、
> 契约决定、顺序建议），登记为 backlog **P18**。配方**不进 CI**（25–500 ms 级、依赖机器）。
> · 关键测量技巧：用"**只存在那 1% 可见节点**"的同类场景当**理想持久化代理**（今天的代码就能跑），
> 于是"上限收益"是被**测**出来的，不是估的。

> 2026-09-17 **V3/V4 收口：把"等上游"从假设变成有门禁的事实（附一个表格形状的坑）**
> 停在"被上游阻塞"上的条目，危险的不是它没做，而是**前提悄悄失效后没人再看它**。
> · **事实（现抓，不是回忆）**：pinned v1.1.16 与**上游 master** 都是 `vkCreateGraphicsPipelines(*device, **VK_NULL_HANDLE**, …)`
> ——`src/vsg/state/GraphicsPipeline.cpp:260`，compute `ComputePipeline.cpp:110`、rt `RayTracingPipeline.cpp:168` 同样；
> vsg 自己的代码里除 Vulkan 头文件（`include/vsg/vk/vulkan.h` 的 typedef）外**不出现** `VkPipelineCache`，也没有
> `PipelineCache` 类型 ⇒ **升到 master 也拿不到**。V4 同理：`src/` 全树无 `vkCmdBeginRendering`，只有
> `vkCmdBeginRenderPass`（`src/vsg/app/RenderGraph.cpp`）。
> · **门禁**：新增 `scripts/check_vsg_upstream_capabilities.py`（7 条断言；无 vsg 树则 SKIP 退 0，照 `check_vsg_window_surface_state.py`
> 的样子）。**变异 4 条全红**：让 vsg 传一个真 cache 句柄 / 在 vsg 头里加 `VkPipelineCache` / 往 `RenderGraph.cpp` 塞
> `vkCmdBeginRendering` / 造一个 `PipelineCache.h`。变异后 vsg 树**逐字节复原**（`diff -q` 验过）。
> · **推上游的最小形状**照 vsg 自己的先例：`Context` 已有 `getOrCreateShaderCompiler()`（`include/vsg/vk/Context.h:88`）
> ⇒ 要一个 `getOrCreatePipelineCache()`（默认 `VK_NULL_HANDLE`，零破坏），`Implementation` 里把那第三实参换成它。
> · **坑**：backlog 里 V3/V4 两行**原本就多带一个空的尾单元格**（9 parts），我按"6 列 = 8 parts"断言，替换后才发现行变成 8 个 `|`。
> ⇒ **表格行替换要在替换后复查 pipe 数**（或者按"目标形状"重建整行），不要只断言替换前。

> 2026-09-17 **P10 结案：缺陷已修且已钉，剩下的 B2 触发条件**未到**（顺手纠正本行一句陈旧话 + 给设计提个醒）**
> 先查"剩余"是什么：本行说`内建路径仍用载体（vsg phong 读 `vine_Color.a`）`，查下去发现这句话**两重陈旧** ——
> vsg 内建 set 这条路径 **2026-09-13 已删**（现在只有引擎自己的 set）；剩下的"白载体"是**静态兜底**，给没写 loc2 色的
> 几何一个合法的 `vine_Color` 属性（乘 albedo 等于没乘），**不承载 opacity**。opacity 今天的唯一来源是每 drawable 的
> `draw.params.x`（渐变片 `alpha = draw.params.x`，顶点色只出 `.rgb`），并被三条**具名**测试钉住：
> `OpacityIsNotPartOfTheVariantIdentity`、`OpacityDoesNotRideTheVertexColour`、`OpacityEditRebuildsNothing`。
> · **B2 按自己的触发条件暂缓**（§12.6："要等第二个标量（材质值或用户参数）出现，否则会把一个 float 包装成一整套 API"），
> 实测条件**未到**：`VineDrawBlock.params` 只有一个活标量（`.x`），`.yzw` 在渐变片里明写 reserved；阴影块的
> `VineShadowBlock` 属于**另一个块**（它自己的 `params.w` 自 2026-09-18 起 = 所属灯的槽位），不算第二个标量。
> · **给将来的 B2 留一句**：§12.6 把"材质值进 `VineDrawBlock.params`"当首个候选消费者，但 `params` 是**每 drawable** 的，
> 材质是**每材质**的（按地址键、跨 drawable 共享、已有 `set0/b0` 材质块）—— 搬进去等于按 drawable 复制材质值，还和材质身份
> 键打架。已在 `.ai/design/graphics-shader.md` §12.6 加一条日期注记。
> · 本次**无代码改动**（缺陷已修、API 按设计暂缓），只改了 backlog 一行 + 设计文档一条注记。

> 2026-09-17 **R4 落地：八个诊断计数折成一个 `VsgRendererCounters` + `counters()`（别名式，零调用点改动）**
> 它那行的触发条件自己说了算 ——「等真要加下一个计数时再动」：V7 加了 `streamsRefreshed`/`dataNodesBuilt`，6 → 8，
> 每个都要一条公开方法 + 一段 Doxygen。做法照**仓库里已有的先例**（`VsgRetentionStats` + `retentionStats()`）：新头文件
> `VsgRendererCounters.hpp`（八个字段，各写"哪个方向可疑"，其中两条是**不变量**：`device_waits` 恒 0、`detached_slots`
> 在全部 pass 都画时恒 0），`VsgRenderer::counters()` 一处汇总；值仍来自三处（`persistent.window_build_count`、
> `state.*_build_count`、退役环）**加**三个"问槽本身"的求和（同 `retentionStats()` 问池要 `compile_contexts` 的手法）。
> **八个访问器全部保留为一行的别名**（约 110 处调用点：`offscreenBuildCount` 31 / `windowBuildCount` 19 /
> `deviceWaitCount` 18 …）⇒ 公开 API 零破坏、零迁移，计数从此只有**一处**。
> · **门禁的设计比门禁本身重要**：别名让"访问器 == 字段"成了定义（永远绿），所以要比的是**两个独立聚合在重叠处
> 是否说同一句话** —— 两者都读退役环。policy-churn 相位断言 `counters().{device_waits,released_objects}` ==
> `retentionStats().{device_waits,released_nodes}`，打印 `[counters]` 行（不带 `[selftest]` 前缀 ⇒ 55 行基线不动）。
> · **变异**：把两个字段的来源互换 ⇒ 新断言立刻红："counters() says 301 wait(s) / 148 release(s), retentionStats()
> says 148 / 301"（外加两条既有断言同时红）。
> · **顺手记住的硬要求**：**新增公开头文件 ⇒ `docs/backend.md` 单元表必须加一行**，否则 `check_doc_symbols.py` 直接点名
> `VsgRendererCounters.hpp: no living document names this unit`。

> 2026-09-17 **P1 结案：上界早就在，"容量 LRU"没有触发面（附一个会把"0 次"说成证据的坑）**
> 先查"封顶"缺什么：**不缺**。三个 program 缓存（64/64/256）、纹理 256、mesh 256、材质 `kMaxEntries` 都在插入点
> `trimToCapacity(...) != 0u` 裁剪，且驱逐**真的归还对象**（`noteEviction()` → `shared_objects_->prune()`，即 D40）；边界由单测
> 钉住（65 程序 ⇒ 必 prune；64 个同变体仍须塌成 **1** 条管线；停掉 prune ⇒ 首条断言红）。缺席窗口那半 2026-09-14 已随机制删除，
> 语义被反向钉住（未画 1000 次 sync 仍保留 / 放手当帧回收 / 隐藏 601 帧回来不重建）。
> · **剩下的只有 FIFO→LRU 的顺序**。探针 `SceneBridge::noteEviction`（只在裁剪**真的删掉条目**时才 +1）：**正对照 `test_vsg` 28 次**
> （那条 65 程序的测试）⇒ 探针确实看得见裁剪；**自检 0 次**（churn 相位 `stage_cache=1` 对上限 64）；**app 0 次**。
> · **坑（差点把 0 次当结论）**：app 的后端是 **dlopen 进来的插件**，gdb 启动时没有那个符号 ⇒ 脚本第 4 行
> `Function "vn::vsg::SceneBridge::noteEviction" not defined` 直接中止，**`run` 根本没跑**。必须 `set breakpoint pending on`，
> **并且**在同一次运行里用一个每帧必中的符号（`VsgRenderer::beginFrame`，命中 **4**）当"断点真的绑上了"的凭据 —— 否则"0 次"与
> "探针没绑"长得一模一样。自检/test_vsg 是**静态链接**、不需要 pending：**同一个探针，两条装载路径，行为不同**。
> · **另一个教训**：app 是**事件驱动**的（gdb 下 120 秒只有 **4** 帧），所以"帧数多"不能当"负载跑过"的凭据；缓存压力的属性是
> **场景内容**（活程序/变体数），不是帧数。
> · 结论：**不改代码**；触发条件（活程序/变体数越过 64/256）与价钱（被逐出程序重建 ≈ **50 ms**，见 H7）写进 backlog 的 P1 行。

> 2026-09-17 **H6 全清（第三条拒答路径）+ P13 结案（那次"帧级一次"的遍历早就在代码里）**
> **H6**：`moveSessionToHostSurface` 三条拒答里，第三条（新窗口的 swapchain 格式不能服务本会话的
> render pass）要两个视觉映射到不同格式的窗口 —— 那是驱动属性，不是调用方行为，所以相位改为由测试档
> `VINE_HOST_MOVE_FORMAT_MISMATCH` 令那次比较失败（**走的是同一条代码路径**，只伪造触发条件）。新账
> 是：该步**恰好一条** `Warning`/`UnsupportedRequest` **且恰好一次**窗口重建（都从该步之前起算，前两
> 幕因此不必重数）。**两半各变异一次**：静音上报 ⇒ 重建照旧（4→5 build）而诊断不增，只诊断那半红；
> 令窗口不再拒答 ⇒ 4→4、诊断也不增 ⇒ 两半都被证明是承重的。相位成功行：`refusals: 3 reported …`。
> **P13**：登记的是"剩余：与 P2 合并成一次帧级遍历"。先查再动 —— `collectFrameShares()` 是全仓唯一
> 收集入口，调用点只有两个且都帧级。**实测**：772 = 2 × 386（`beginFrame`），与槽数无关；每槽
> `SceneBridge::collectOwnedShares` 2827/772 ≈ 3.66 次/收集 ⇒ 每槽恰好一次且省不掉。**不能再合成**：
> 释放要判"整会话都松手"，第一个槽做决定前那张表就得含所有槽 + manager（P11 本身），塞进逐槽 sweep
> 就是那一类悬垂。不改代码。
> 判据（H6 有代码改动 / P13 无）：build 0/0、`test_vsg` 292 / `test_graphics` 272 / `test_core` 82、
> **证据 55 行逐字不变**、lavapipe PASS 0 VUID、四个静态检查 0（窗口表面那个仍报 8 dropped / 5
> refreshed / 9 kept）。

> 2026-09-16 **H5 完成：那笔 +0.43 s 在 release 里的真相（附带一个方法论教训）**
> 做法：`git worktree` 各建一个 revision，**各自独立**在 Release 下建 `vsg_backend_selftest`（依赖源码用 `-DFETCHCONTENT_SOURCE_DIR_<NAME>=主树/build/_deps/<name>-src` 复用 ⇒ 不需要网络；**各 1m29s**），交错 5 轮 × 30 帧、lavapipe。
> · **数字**：pre-T16（`d6a182f`）**2.76 s** 对 T16（`fb6894f`）**3.37 s** ⇒ **+0.61 s（+22%）** —— 比 -O0 的 +0.43 s **更大** ⇒ **交付物确实带这笔钱**。
> · **但它不是"启动成本 + 每帧收益"，也没有"每帧回本"（2026-09-16 深夜复核，初稿错了）**：把帧数拉开到 **5 / 200 帧、各交错 5 轮** ⇒ 差 **+669 ±13 ms（5 帧）**、**+627 ms（200 帧）**，**与帧数无关** ⇒ 那三个单点拟合出来的"每帧省 2.6 ms、约 290 帧回本"是假象，已删（两端点斜率 A ≈17.0、B ≈16.8 ms/帧）。
> · **钱花在 9 个渲染目标 attach/release 事件上**（各 +60–176 ms；帧循环那 161 步合计只差 55 ms）—— 日志自带毫秒时间戳，逐行相减即可定位。
> · **新证据（把"光栅器同样工作却变慢"这条判词推翻了）**：差额 **+0.54 s 全在进程主线程**（llvmpipe 的 16 条光栅线程逐条相同）、**JIT 量相同**（`mmap/mprotect(PROT_EXEC)` 219 对 219）、**不是阻塞**（user +0.55 对 wall +0.60）、**峰值 RSS 不变**；但 **B 多让驱动分配 39 块 16 MB 设备内存**（`/memfd:allocation fd`，这字符串就在 `libvulkan_lvp.so` 里）⇒ **日志逐行相同 ≠ 工作相同**。**控制实验**：同一 revision 独立建两份 ⇒ 产物 **md5 完全相同** ⇒ 与构建噪声无关，差只可能来自源码。机制未定，登记为 backlog 的 **H7**（本机没有 perf / strace / ltrace / valgrind / gdb）。
> · **H7 结案（2026-09-16 深夜，装好 valgrind/gdb 之后）：那 0.6 s 是 glslang 在重编内建符号表，不在驱动里。** `callgrind` 两边各跑 6 帧：A **16.60 G** 条指令 / B **19.51 G**（+2.92 G，+17.6%），多出来的几乎全是 glslang 在解析（`yyparse` 1.61 G → 2.60 G、`getch` +54% 输入字符、`TSymbolTable::insert` 199k → 339k）。**着色器源码与编译份数两边完全相同**（182/21200/279/208/421 全部一致，日志逐行一致）⇒ 差在**内建符号表被重建的次数**。
> · **原因**：vsg 用引用计数管 glslang 的进程生命周期（`ShaderCompiler.cpp:51/59`），**归零时 `glslang::FinalizeProcess()` 会丢掉内建符号表缓存**，下次编译必须从头解析内建符号源（每次约 50 ms）。**A 17 次、B 29 次** `InitializeProcess`/`FinalizeProcess` ⇒ +12 个 epoch ≈ +2 G 指令 ≈ 那 0.6 s。glslang 是纯 CPU 的 ⇒ **真机同样要付**。为什么是 29 对 17（Context/CompileTraversal/ShaderCompiler 构造数两边完全一致）登记为 **H8**。
> · **H8 结案（2026-09-16 深夜）：受益面是零，所以不修。** 同一探针量两处（gdb 断在 `glslang::InitializeProcess/FinalizeProcess`）：**自检（6 帧）31 个 epoch，GUI 应用跑 25 秒只有 `init=2 finalize=1`** ⇒ **交付的应用根本不付这笔钱**（只建一次会话，计数不归零）。那 +0.6 s 是**自检反复建/拆会话**这个工作负载的产物（A 17 / B 29 / HEAD 31），不是改动带给产品的回归 ⇒ 不做“常驻 `ShaderCompiler`”的改动。另：`forget()`（T16 新增，93 次）与 epoch 变多的因果**未证实**——前 7 次 finalize 之前一次 forget 都没有。
> · **教训（比数字值钱）**：第一次我拿 pre-T16 对 **HEAD** 算出 +0.82 s，**那个比较是错的** —— HEAD 多了 C1/A6 的 host-surface 相位（每轮多两次整会话重建）。`nm -C` 身份只证明了“符号对”，**没证明“工作负载相同”**：要再验 `grep -c host-surface`（pre-T16/T16 = 0，HEAD = 4）。⇒ **二进制 A/B 检查单：符号身份 + 工作负载身份**。
> · 副产品：`build-release/` 现成（gitignore `/build-*`），以后做 Release 对比直接复用；两个 worktree 用完已删。

> 2026-09-16 **R5 结案：appfw 插件注册表的 LSan 报告 —— 抑制，但把“为什么是有意的”一并写进去**
> 先查证它到底是不是缺陷：`~DynamicLibraryLoader` 的 `d.release()` **是有意的**（注释写明：若按静态析构序在退出时 dlclose 掉插件代码，而 `CommandManager` / `RenderBackendRegistry` 还持着指进插件的 callable / 工厂指针 ⇒ SIGSEGV）。所以这是**保留决定**，不是忘记释放 —— 而 `asan_leaks.supp` 原来的规则（“框架自己的分配一律不许抑制”）恰好把这种情况也堵死了。
> · 处置：给那份文件加一条**带条件的例外**（“有意保留 **且** 决定写在代码里，可以入表；‘只有几个字节’不是理由”），并新增 `leak:vn::runtime::DynamicLibrary` + 实测数字。
> · 收益：后端两条跑法（`test_vsg` **292** 全绿 + 设备自检 55 行证据全跑完）现在都用**全或无**泄漏判据 PASS —— 比 R2 当时的 `VINE_ASAN_LEAK_SCOPE` **更强**（scope 会把同一轮里别的泄漏一起放过）。scope 开关保留，但降级为“还没 excuse 的泄漏”的备用手段。
> · **证据**：`LSAN_OPTIONS=…:print_suppressions=1` ⇒ `Suppressions used: count 16 bytes 808 template vn::runtime::DynamicLibrary`（**只有**这一类被匹配）。

> 2026-09-16 **H4 落地：宿主真的开始“跟着走”了（C1 的另一半），且这件事终于可判**
> 原计划是“把 `initializeBackend()` 里的 `shutdown()` 换成重新公告句柄”。查下去发现**宿主有两处在拆会话**：`initializeBackend()` 的“句柄变了”分支，**以及** `onSurfaceDestroyed()`（Qt 的 `SurfaceAboutToBeDestroyed`）。只要后者还在，前者改得再对也没用 —— 真实路径（Qt 重建窗口）先经过它。两处都改：`onSurfaceDestroyed()` 只标记 `surface_ok=false`（渲染由 `renderFrame()` 既有的“句柄/可见性”守卫拦住），会话留着等新句柄；`initializeBackend()` 在新句柄到来时**重新公告**（`setWindowHandle` + `initialize`），由后端的 `initialize()` 自己决定搬还是重建；`init()` 的幂等返回改成“句柄匹配才算已绑定”。
> · **可判性是这次的重点**：“平台窗口被重建”（换屏 / reparent / 把 dock 拖出去）本来**无法按需触发**，所以加了一个测试钩子：`VINE_RECREATE_SURFACE_MS` → `RenderControl::recreateSurface()`（`QWindow::destroy() + create() + show()`）。app 阶段默认 `VINE_APP_RECREATE_MS=1200`，并断言两条：`moved to the host's new window ≥ 1` **且** `attached to the host window == 1`（重建就是 2/0）。
> · **实测**：`attached to the host window 0x60004a` → 钩子 → `[RenderControl] the render surface was recreated: re-announcing …` → `[VsgHostWindow] moved to the host's new window 0x600051 (378x247); the device and its pipelines were kept`；渲染区**采的就是新窗口**、84.90% 非黑、0 VUID。**变异**（把 shutdown 放回 `onSurfaceDestroyed()`）⇒ `follow it (0 moved)` + `rebuilt the session (2 attached)` + 像素阶段去读**已销毁的旧窗口**失败，**三条红**；回滚后全绿。
> · **像素阶段顺手修的一个真坑**：它原来在“看到第一行 attach”时就定住句柄，钩子把窗口换掉后它采的是**已销毁的窗口** ⇒ 抽出 `latest_window_id()`（attach 与 move 两行都算、取最后一行），并且在钩子开启时**先等 move、采样前再取一次**。
> · 判据：build 0/0、`test_vsg` 292 / `test_graphics` 272 / `test_core` 82、include 卫生 0、63 单元 agree、**证据 55 行逐字不变**、lavapipe PASS 0 VUID。

> 2026-09-17 **H1 落地：Win32 分支从“只能审读”变成“有门禁”**
> · 原来的样子是自检相位开头写着 `#if defined(_WIN32) … return true; // 本机不跑，靠 Windows 的 app 门禁` —— 也就是**在 Windows 上什么都不验**。现在两个平台共用一个相位体，平台差异只剩两处：`HostWindow`（X11：自己的 connection + `xcb_create_window`；Win32：自己的窗口类 + `AdjustWindowRect` + `ShowWindow(SW_SHOWNOACTIVATE)`，句柄就是指针）与 `setEnvironmentFlag()`（`setenv/unsetenv` 对 `_putenv_s`）。断言（像素、计数、窗口存活、三条拒绕路径）两平台一模一样地跑。
> · **实测（Windows 11 + RTX 4060，Vulkan 1.4.351）**：自检 exit 0，`mapped=true`、`windows built 2 before, 2 after; 1 counted device stop(s); host windows intact; centre 34,6,2 before and after; a repeated handle kept the session: yes`、`refusals: 3 reported`；app 门禁（`VINE_RECREATE_SURFACE_MS=1200`）`attached to the host window 0xe083c (752x480, mapped=true)` → `moved to the host's new window 0xf083c (752x480); the device and its pipelines were kept`，`attached=1 / moved=1`、0 VUID。
> · **变异**（`~VsgHostWindow()` 里的 `_window = {};` 注掉）⇒ 非 0 退出（后续 rebuild 拿着无效句柄：`GetClientRect(..) failed : 无效的窗口句柄` → `initialize FAILED` → `FAILED — the host surface move did not hold`）。
> · **这一跑顺手挖出一个真缺陷（Win32 专有）**：vsg 的 `Win32_Window` **采纳**宿主窗口时也会走 `_initDrop()`，把 OLE 拖放目标注册到**宿主窗口**上；撤销只在基类析构里做，而那时本类已把 `_window` 置空（不置空就会被 `DestroyWindow`），**搬移后句柄更不是注册时那个** ⇒ 宿主窗口上留下一个指向已 `Release()` 的 `DropTarget` 的注册（拖上去就是 UAF），且**该窗口此后注册不上拖放**。判据很好抓：修复前同一窗口第二次被采纳时 `Warning: Win32_Window::_initDrop() RegisterDragDrop failed`，一轮 2 次；修复后 0 次。
> · 处置：新增 `VsgHostWindow::withdrawHostWindowState()`（头文件里的平台小节，Win32 = `_shutdownDrop()`，X11 为空实现），在**构造末尾**调用 —— 构造时就撤，而不是等析构：搬移会让“句柄”和“注册时的窗口”失去对应，只有刚采纳的那一刻两者一定一致。副作用：本后端不再在宿主窗口上提供文件拖放，但那条路本来就死了（`EmbeddedViewer::pollEvents()` 不泵窗口，drop 事件永远不会被发出）。
> · **仍缺**：Windows 上没有 `xwin2ppm.py` 的等价物 ⇒ app 阶段在 Windows 只能断言结构性证据（attach/move 计数、0 VUID、`mapped=true`），**“画出来没有”仍无判据**；“拉伸窗口看画面跟随”仍需人工拖一次。

> 2026-09-17 **宿主表面续：放大窗口后「新露出的区域先黑、然后被拉伸填满、再恢复正常」**
> · 判据来源：本机 Windows 11 + RTX 4060 跑 `Vine.exe`（deferred 默认管线），`scripts/win_maximize_probe.ps1`（`ShowWindow(SW_MAXIMIZE)`）触发，靠**临时**插桩（`beginFrame`/`endFrame`/`resize` 打窗口 extent、窗口 RenderGraph 的 `renderArea`、每个 slot 的 viewport 矩形，以及各 target/slot 的重建时间戳）读时间线。**修前**一条：`resize announced=1176x444 live=2352x888 graphRenderArea=752x480+0,0` → 该帧结束仍 `renderArea=752x480`（旧矩形），而**这一帧里**重建完 gbuffer/composite + 5 个全屏程序槽（各 ~21 ms，共 ~250 ms）→ 下一帧才 `renderArea=2352x888`。完整记录（现象/根因/修复/验证）见 `.ai/bugs/vsg-maximize-black-band.md`。
> · **根因一**：窗口共享 `RenderGraph` 的 `renderArea` 只由 vsg 在**录制期**（`RenderGraph::accept` 发现 extent 变化 → `resized()`）缩放 ⇒ 中间那些帧仍按旧矩形清/画，新露出的部分既没清也没画（刚重建的 swapchain 图像本来就是黑的）。**修**：`VsgRenderer::resize()` 里**当场**写 `renderArea = {{0,0}, extent}` / `viewportState->set(...)`，并把 `previous_extent` 同步成同值。
> · **顺带修掉一个更隐蔽的**：同步 `previous_extent` 等于**关掉 vsg 的缩放路径**——它会把 Vine 已经正确的矩形再按 new/old 缩一次。实测 HUD overlay 矩形 `16,368 96x96` → `50,1436 300x177`（跑到 888 高的窗口外）、fps overlay → `7003,1561`（x > 2352）；修后 `16,776 96x96` / `2239,844 105x36`，都在窗口内。
> · **根因二**：「第一帧就是最贵的那帧」：`RenderControl::handleSurfaceUpdate()` 是先 `view->onSurfaceResized()`（= 重建整条离屏链）再 `renderFrame()`，那 ~250 ms 里**没有任何一帧以新尺寸 present 过** ⇒ 新露出的区域一直黑。**试过**：拆成 `engine->resize()` → `renderFrame()`（离屏链还是旧尺寸 ⇒ 上一帧画面被**拉伸**填满新窗口，实测第一帧 26 ms 就提交）→ `view->onSurfaceResized()` → `renderFrame()`（重建帧）→ settle frames。**结论：这个中间帧撤掉了**（2026-09-17 实机看过后按需求决定）——它把画面**拉伸变形**（旧画面按新 aspect 拉伸一下再弹回），比“新区域晚 ~250 ms 才填上”更难接受。**现在的约定：画面任何时刻都不变形**；窗口新长出来的部分等重建帧落地时填（该帧覆盖整个新窗口，因为 renderArea 已当场写对）。
> · **根因三**：程序槽的 rebuild identity 里含**目标表面尺寸**（`ProgramSlot::dest_w/dest_h`），于是 resize 帧要把窗口里的 5 个全屏程序全部重编译（~105 ms）。**修**：去掉它——节点的几何是全屏三角形、矩形是**动态状态**（每帧从 pass 的 viewport 写进 `slot.camera->viewportState`），每个全屏片段阶段都按 `vine_uv` 采样（与尺寸无关）；「矩形是动态而不是烤死的」由自检的 PiP 相位**反证**（PiP 在小矩形里采到**整个**源，而它的管道是按**表面**尺寸烤的）。
> · **实测（最终形态）**：resize 后**只提交一帧**（重建帧，~240 ms：gbuffer + composite 重建，6 个全屏程序节点重建），该帧覆盖整个新窗口（renderArea 当场写对），0 VUID、无崩溃。中间帧版本实测过（26 ms 提交），因为拉伸变形已撤。
> · 判据：全量构建绿；`test_vsg` / `test_graphics` / `test_gui` 通过；`vsg_backend_selftest` 绿（`[host-surface] move:` 行 + 0 VUID + `[selftest] done`）。
> · **仍剩（登记在 backlog H9）**：重建帧本身 ~240 ms（6 个全屏程序节点 ~180 ms + 2 个 target ~40 ms；**glslang 只占 ~50 ms**，其余是 vsg 每节点建管线/描述符）。这段期间旧画面在屏（不变形），窗口新长出来的部分到重建帧落地时才填上。要缩短只能改槽的重建策略（原地改描述符 + 动态 viewport 状态），因为 resize 后**源的图像视图换了**，节点必须重建，而每节点 ~20–40 ms 是驱动建管线的成本。**未做**。

> 2026-09-16 **R3 落地：「一个 episode 只报一次」从 10 份约定收成一个类型**
> 新增 `include/vine/vsg/VsgReportOnce.hpp`（`shouldReport()` / `reported()` / `rearm()`），把散在 `VsgRendererState`（5）、`VsgRenderTargetEntry`（3）、`SceneBridge`（1）的 bool，以及 `detail::beginLightsDroppedEpisode` / `beginTargetSizeMissingEpisode` 两个 `bool&` 自由函数全部换掉。**重武装仍由调用者决定**（各站点边界不同：新帧 / 新作用域 / 可用的尺寸 / 每盏灯都亮回来 / 换了源），类型只承载规则本身 —— 这是本次抽取唯一的风险点，所以写进了类注。
> · 两个自由函数因此各短三行：`if (条件结束) { reported.rearm(); return false; } return reported.shouldReport();`。
> · 语义中性由既有测试守着（`LightDropReportTest` / `TargetBookkeepingTest` / `DiagnosticsTest` / `PassProtocolTest` / `FrameCommitTest` 都断言上报次数）；新增 `tests/test_vsg/ReportOnceTest.cpp`（3 条）直接钉类型契约（含“被拒的上报不得重武装”与“对未上报的 episode 重武装是 no-op”）。
> · **变异**：`shouldReport()` 改恒 `true` ⇒ **恰好 6 条红**（新套件 1 + LightDrop 3 + TargetBookkeeping 2）。
> · 判据：build 0/0、`test_vsg` **289 → 292**、`test_graphics` 272 / `test_core` 82、三脚本 0（**63** 单元 —— 新头必须先补进 `gfx_backend_vsg.md` 的单元表，否则 `check_doc_symbols.py` 会红，这正是它该有的行为）、**证据 55 行逐字不变**、lavapipe PASS 0 VUID。

> 2026-09-16 **R1 + R2 落地（审查轮次 4 的头两条）**
> **R1 平台重复收成一份**：`VsgHostWindow.cpp` **315 → 110 行**、`.cpp` 里**零** `#if`。做法：新增 `VsgHostHandle`（`xcb_window_t` / `HWND`）与 `hostHandleFromVoid(void*)`（头文件里的 4 行 `#if`），类体只写一遍；`makeWindowTraits` 的两处句柄转换也改用它。**两次编译拒绝（标准限制，不是风格问题）**：① `reinterpret_cast<uint32_t>(void*)` ⇒ "cast from pointer to smaller type loses information"；② `reinterpret_cast<uint32_t>(uintptr_t)` ⇒ "reinterpret_cast from integer to integer is not allowed" ⇒ 那个 4 行 `#if`（X11 用 `static_cast` 收窄 / Win32 用 `reinterpret_cast` 取指针）**不可消除**。顺带删掉 `.cpp` 里 A6 派生重构后的残留 include（`vulkan/vulkan_{xcb,win32}.h`、`xcb/xcb.h` —— surface 现在建在基类里），空句柄判定从 `_window == 0` 改成 `hostHandle() == nullptr`（两平台同一写法，也不怕 `-Wzero-as-null-pointer-constant`）。判据：build 0/0、289/272/82、include 卫生 0、**证据 55 行逐字不变**、lavapipe PASS 0 VUID（自检 host-surface 相位就是这条转换的端到端门禁）。
> **R2 后端进 ASan 门禁**：脚本本来就为 `test_vsg` 加建 `gfx_backend_vsg`（今天才知道 —— 手工 `ninja` 少了这步会让 `VsgBackendPluginTest` 两条假红），真正的卡点是**泄漏判据全或无**：`test_vsg` 链 appfw 插件管理器，`DynamicLibraryLoader` 单例必报 ⇒ 后端结论被遮住。新增 `VINE_ASAN_LEAK_SCOPE`（grep -E 模式）：只有“栈里没有任何一句匹配它”的泄漏才**报出但不判**；内存错误与测试失败永不豁免。两条跑法写进脚本头。
> · 实测：`VINE_ASAN_TARGET=test_vsg VINE_ASAN_FILTER='*' VINE_ASAN_LEAKS=1 VINE_ASAN_LEAK_SCOPE='vn::vsg' scripts/asan_check.sh` ⇒ 289 全绿 + `RESULT: PASS (scope 'vn::vsg' clean; …)`，唯一报告（appfw loader，808 B / 16 次）**显式打出来**；`VINE_ASAN_TARGET=vsg_backend_selftest … VINE_ASAN_LEAK_SCOPE='vn::vsg|vn::graphics|selftest::'` ⇒ 自检 **55 行证据 + `[selftest] done` 全跑完、零报告**（设备路径：会话建立 / 搬移 / shutdown + 目标物化）。
> · **顺手修的门禁缺陷**：`asan_check.sh` 里 `sed -E 's|(^|/)clang\+\+|\1clang|'` 的**分隔符与模式里的 `|` 撞车** ⇒ sed 报 “unknown option to `s'”、编译器名推导**从未生效**（一直回退到 PATH 扫描）。改成 `s#…#…#`。属于“门禁自己撒谎”那一类：**`sed` 用 `|` 当分隔符时，模式里不能再出现 `|`**。

> 2026-09-16 **审查轮次 4：后端 + SDK 复看（只读，无代码改动；候选登记为 R1–R4）**
> **泄漏这条的判据是跑出来的，不是看出来的**：`build-asan` 里 vsg 目标**一个都没建**（脚本默认 `VINE_ASAN_TARGET=test_gui`），今天手工补建（`ninja -C build-asan test_vsg vsg_backend_selftest`，1m55s，0 error）后跑 LSan：
> · `ASAN_OPTIONS=detect_leaks=1 ./build-asan/bin/test_vsg` ⇒ **289 全绿**，泄漏报告**唯一一条在 appfw**（`vn::appfw::PluginManager::loadAll` → `DynamicLibraryLoader` 单例，808 B / 16 次分配，栈里**零 vsg 帧**）；
> · `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ASAN_OPTIONS=detect_leaks=1 ./build-asan/bin/vsg_backend_selftest` ⇒ **exit 0、0 条 sanitizer 报告**（会话建立/搬移/退役环/缓存/编译租约全过）。
> · **坑**：ASan 树里不先建插件 ⇒ `VsgBackendPluginTest` 两条假红（"gfx_backend_vsg plugin should have registered the 'vsg' backend"），`ninja -C build-asan gfx_backend_vsg` 之后 4/4 绿。所以 **R2** = 把后端纳入 ASan 门禁 + 决定 appfw 那个单例抑制还是修。
> **R1（平台重复）怎么量出来的**：把 `VsgHostWindow.cpp` 两个 `#if` 分支各自抽出（各 ~93 行）、归一化平台词汇后 `diff` ⇒ **真正因平台不同的代码只有 3 处**（`hostHandle()` 的两种 cast、空句柄判定 `== nullptr`/`== 0`、`next` 的两种转换），其余**逐字相同**，而两份注释已经开始漂移（`xcb_destroy_window` vs `DestroyWindow` + `UnregisterClass`）。收成一份约 **−85 ~ −90 行**，`#if` 只留在头文件的 typedef/include 上。
> **R3（episode 规则散布）**：`grep -rn "bool .*_reported" include/vine/vsg/*.hpp` ⇒ 10 处；另有 2 个 `begin*Episode(..., bool&)` 自由函数；重武装点 4 处（`beginFrame` / `setRenderTarget` / `VsgTargetBookkeeping` ×2）。
> **R4**：`VsgRenderer.hpp:391-460` 6 个 `*Count()` 各配一段 Doxygen，值分散在 `persistent` / `state` / `retireRing`+池 stats 三个属主 ⇒ 可并成一个 `VsgRendererCounters`，但测试里这些名字出现几十次，低优先。
> **两条"看着像、查完不是"的**（记下来免得下一轮再查）：`VsgBufferView` 的 `dataAvailable`/`dataRelease`/`dimensions`/`elements`/`valueSize` 看着没人调用，实际是 `vsg::Data` 的虚覆写；`VsgDrawBlockPool` 用 `shared_ptr` 看着能收成 `unique_ptr`，实际 `Lease` 持 `shared_ptr<VsgDrawBlockPool>` ⇒ 收不了（而这正是"池比租约活得久"的类型保证，旧笔记里"析构绝不碰池"那条现在只是保险）。
> **一条真漂移**：`.ai/design/graphics-overlay.md` 仍以**现役**口吻写 `RenderBackend::releaseWindowLayer` 与"按相机键的 `window_layers` 表"（line 30/33/65/96），而两者**已从 SDK 与后端删除**（P17 之后身份是 `SlotKey::ownerPass`、作用域是唯一驱动），`hasWindowPass()` 仍在役 ⇒ 该文件需要一条 dated banner 指到 `.ai/design/vsg-pass-lifecycle.md`。
> **不做**：按体积拆 `SceneBridge.hpp` / `SceneBridge.cpp`（1377 / 1209 行）。绝大部分是 Doxygen，且规则已按 `VsgSceneRules` / `SceneBridgeGeometry` / `SceneBridgePipeline` 分好家 —— 仓库的规矩是"抽概念，不抽文件"。

> 2026-09-16 **H6 收尾（宿主表面那四项）：app 门禁开始看画面，拒答不再静默**
> **④ app 门禁读像素（四项里最值钱的一条）**：此前 app 阶段只看 stderr 证据 + "0 VUID"，**一个黑屏会话把两条都满足**（当天 0.84% 那次就是这么过的）。现在：
> ① 后端把**它渲染的那个窗口句柄**写进日志（`[VsgHostWindow] attached to the host window 0x60004a (378x247, mapped=true)`，搬移那行同理）—— Qt 的渲染区是**具名顶层窗口的子窗**，按名字读会读到 Qt 自己的界面，而那块**即使渲染区全黑也是亮的**（0.84% 被藏住就是这个原因：实测按名字读顶层是 97.06%，按句柄读渲染区才是 84.90%）；
> ② `scripts/xwd2ppm.py` → **`scripts/xwin2ppm.py`**：抓取改走 libX11 `XGetImage`（自己解 `XImage` 的掩码/步长；32 位真彩走切片快路径），**不再需要 xwd/xwininfo** —— 本机根本没装 x11-apps 且 `sudo` 要密码装不上，旧版会在**最需要它的地方**静默跳过；
> ③ app 阶段把 app 放到**后台**跑（`cd $BUILD && exec timeout $SECONDS_V ./bin/Vine &`，`exec` 让 `$!` 就是那个进程、退出码语义仍是 124）⇒ 趁它活着采样 ⇒ 再 `wait`；开关 `VINE_APP_PIXELS=0`、`VINE_APP_MIN_CONTENT`(30)、`VINE_APP_PIXEL_WAIT`(8)、`VINE_APP_PIXEL_SETTLE`(2)。
> **判据**：门禁绿时打印 `84.90% of the render area is not near-black (threshold 30%)`（渲染区 378x247，与 C1 当天的 83.81% / 均值 (96,105,112) 同量级）；**变异**（`VsgHostWindow::visible() → false`，就是当天黑屏的根因）⇒ `0.00%` + `[FAIL]` + `RESULT: FAIL`。分支各自验过：`VINE_APP_PIXELS=0`、无 `DISPLAY`（都跳过且不计失败）、app 一直不报窗口、句柄读不到、阈值 0/30/99 两侧。
> **顺带修掉一个报告缺陷**：阶段判 PASS 原用 `stage_before=$FAILED` 比全局布尔，**前面阶段已经失败时就分不出"又多一条失败"** —— 变异那次它就打出了 `[PASS] Vine (default demo)` 压在刚报的 `[FAIL]` 上。现在每阶段自己的计数 `STAGE_FAILURES`（`mark_stage_failure`），PASS 只在它为 0 时打印。
> **② 拒答路径**：`moveSessionToHostSurface` 的"公告 null 句柄"与"会话不在本后端宿主窗口"两条**原本静默**（`docs/backend.md` 却说它们会报），现在各自报 `Warning` + `UnsupportedRequest`；自检相位加两条断言（被拒 ⇒ 上报 + 重建，`windowBuildCount()` +1 / +2，最后搬回宿主窗口以保持"采纳的窗口要活过我们"那条断言仍指向 B）；**两条都变异过**（各自静音 ⇒ 恰好对应那条红）。**第三条**（新窗口 swapchain 格式不可用）仍**无断言**：它要两个视觉映射到不同格式的窗口，是驱动属性不是调用方行为，已登记遗留。
> **① 相位尺寸**：`kPixelsWidth/kPixelsHeight` 只命名一次，采样点由它派生（原 `256/144/128/72` 散在三处）；**③ `resize`**：覆写参数改 `announced_width/announced_height` 并写明"公告是 advisory"（SDK 契约本就是 surface > announcement > default）。
> **门禁**：build 0/0；`test_vsg` **289** / `test_graphics` **272** / `test_core` **82**（**未增删单测**：拒答是私有的，驱动它要走 `setWindowHandle` + `initialize`，那是**带设备**的自检相位的活——先在 `tests/test_vsg` 起了个 GPU-free 的套件，编译期就报 `private member`，拆了）；三脚本 0（62 单元 / 663 文件 / 33 文件）；证据 **55 行逐字节不变**；lavapipe **PASS**、0 VUID；自检 3.8 s（多了两次重建，仍在门禁 12 s 预算内）。

> 2026-09-16 **C3（本轮 brief）：编译上下文的池由"每帧对账"决定，而不是靠三处记得释放（T16 的收尾）**
> **目标**：让"池里有哪些 context"成为**存活内容槽的函数**，而不是一条需要三处拆除点记住的约定。
> 现状的四个负担：`ContentSlot::compile_context_registered`（bool）、`VsgRendererState::compile_context_registrations`
> （与池内容必须同步的影子计数器，+1 在 `VsgViewCompiler.cpp`、-1 在 `forgetCompileContext`）、
> **三处必须记得调的 `forgetCompileContext`**（`unhookTargetPasses` / `resetContentShaderSlots` / `erasePassFromTarget`）、
> 以及为解释这条约定写下的整段文档（`VsgViewCompiler.hpp` 类注、`VsgTargetBookkeeping.hpp` 的"三处即三处"段、
> `VsgRendererState.hpp` / `VsgRetentionStats.hpp` / `docs/backend.md` 5.3.1 各一段）。
> **做法（五步）**：① `VsgCompileManager` 的 `forget(view)` 换成 `prune(span<const View*> live)`，另加 `holds(view)`（查池）与
> `contextCount()`（问池）；三者共用一个私有的 `visitPool()`，把"借出 traversal 再归还"这条契约只写一遍。
> ② 新增 `syncCompileContexts(state)`：由会话的槽表算出 live 视图表，交给 `prune`；`compilePendingViews()` 在**队列判空之前**
> 调用它 ⇒ 每个提交帧结束时池 = 存活内容槽（帧内槽的丢弃都发生在 `retireInactivePassSlots()` 之前，`VsgRenderer.cpp:760/763`）。
> ③ 注册分支改问池：`if (!manager->holds(view))` ⇒ 删 bool。④ `retentionStats()` 的 `compile_contexts` 改成问池
> （`detail::compileContextCount`）⇒ 删影子计数器。⑤ 删三处 `forgetCompileContext` 调用及其注释。
> **判据**：① 池里一条都不多（`VINE_PROBE_RETENTION=1` 指纹：`churn START 4/4`、`END 7/7`，与 T16 终态**逐字相同**）；
> ② 该相位末尾既有断言 `compile_contexts <= content_slots` **不变**、仍然通过；③ 55 行证据逐字节不变；④ `test_vsg` 289 /
> `test_graphics` 272 / `test_core` 82；⑤ 三脚本 0；⑥ lavapipe PASS、0 VUID；⑦ build 0 warning、include 卫生 0 命中。
> **变异（必须红）**：把 `prune` 改成空转 ⇒ 60 对 4 的指纹回来、断言 FAIL；把 `holds` 恒返回 false ⇒ 同一视图被反复注册 ⇒ 断言 FAIL。
> **已知代价（写进注释，不藏）**：① 释放从"立即"变成"**至多晚一帧**"（下一次 `compilePendingViews` 对账时释放），
> 与停放/退役环同级，而不是 T16 之前那种"活一整个会话"；② `retentionStats()` 从读一个字段变成**借一次池**
> （无并发编译时即时返回；它是 `noexcept`，取还只在队列上 push/pop）。
> **不做**：RAII 句柄挂在槽上（`state = VsgRendererState{}` 是**按声明序**赋值成员，`window`/`viewer` 先于 `targets` 被释放
> ⇒ 句柄会对着已析构的 manager 调 forget；把 T16 刚去掉的隐性耦合请回来）。
> **本条也回答"派生到底带来什么"**：T16 派生掉的是一条 **API 缺口**，本轮去掉的是一条**义务**——池自己算得出来的事实，不该由人记住。
>
> **2026-09-16 收尾（实际落地的是"租约"，不是 brief 里的"每帧对账"）**：先按 brief 把"每帧对账"实现完了（`prune`/`holds`/`contextCount` + `syncCompileContexts`），**自检 3/3 次段错误**（都在 churn 的 `churn-rebuild`
> 阶段），gdb 下因 ASLR 关闭又不复现。改用**租约**后全绿。判词：**释放必须绑在槽的析构上**（新类型 `detail::VsgCompileRegistration`，声明在 `view` 之后，成员反序析构
> ⇒ 注册先走而 view 还活着）——延迟释放会让池里留着一个 `context->view` 已析构的 context，而 vsg 的 `CompileTraversal::apply(View&)`
> 对每个 context 做 `context->view.ref_ptr()`（`observer_ptr` → `ref_ptr` **就是加引用计数**），于是悬垂 observer 不是只读比较而是**写已释放内存**。
> **落地结果**：删掉槽上的 bool、会话上的影子计数器（`compile_contexts` 改成问 manager 要池真值）、三处 `forgetCompileContext` 与解释它们的整段文档；
> 新增 `VsgCompileRegistration.hpp`（~90 行含注释）；`shutdown()` 在整体赋值前多一行 `state.targets.clear()`（成员赋值按**声明序**释放，`viewer` 先于 `targets`
> ⇒ 槽要在 viewer 还在时释放注册）。
> **判据（全绿）**：55 行证据逐字节不变、`test_vsg` 289 / `test_graphics` 272 / `test_core` 82、三脚本 0、lavapipe PASS + 0 VUID、build 0 warning；指纹
> `churn START 3/3`、`END 6/6`（与**基线**逐字相同，同日 A/B：stash 前后各跑一次）；**变异**：`release()` 短路 ⇒ 立刻回到修前 `4/60 → 63` 且断言红。
> **顺带的记录更正**：旧文里"修后 START 4/4、END 7/7"在记下它的那份代码上今天也复现不出来（基线二进制读到 3/6，`retired=115 builds=2` 两者相同），
> 且那句"`observer_ptr<View>` 只是弱引用 ⇒ 不会悬垂"是**错的**。
> **那笔 +0.43 s 仍未查明**：租约版复测 3.12–3.27 s（3 次），与上一版同区间 ⇒ "释放调用"与"影子计数器"也可排除。

> 2026-09-16 **C1 brief：后端自己拥有宿主表面（宿主重建窗口 = 迁移，而不是整会话重建）**
> **现状（三条已核实的事实）**：① `RenderControl::initializeBackend()` 在 Qt 重建平台窗口时走 `engine->shutdown()` + `initialize()`
> ——整会话重建：设备、**全部 PSO**、全部目标与缓存；② SDK 契约（`RenderBackend::nativeHandle()` 的注释）本来就写着"重新公告
> `setWindowHandle()` **就是让后端迁到新表面**的方式"，也就是说**契约允许迁移，而我们做不到**；③ 为了迁就现状，
> `CMakeLists.txt` 把 vsg 的并发设备上限抬到 4（`VSG_MAX_DEVICES=4`，注释写明是 Qt 表面重建），`shutdown()` 里还得
> `window->releaseWindow()`——否则 vsg 的 XCB 窗口析构会 `xcb_destroy_window` **宿主的**窗口。
> **另外，上一轮我判"C1 在本环境没有门禁覆盖"是错的**：这里 `DISPLAY=:0` 且 `/tmp/.X11-unix/X0` 可连（`xcb_connect` 成功），
> 门禁的 app demo 本来就是跑在窗口上的，所以宿主表面这条路**既能回归也能新增用例**。
> **做法**：新增 `detail::VsgHostWindow : vsg::Inherit<vsg::Window, VsgHostWindow>`，**只替换 `_initSurface()`**（在自己开的 X 连接上用
> `vkCreateXcbSurfaceKHR` 建宿主窗口的 surface；Win32 分支同理），其余全部复用 vsg 基类的受保护机器
> （`_initFormats` / `_initPhysicalDevice` / `_initDevice` / `_initRenderPass` / `_initSwapchain` / `buildSwapchain`，惰性 `getOrCreate*` 照旧）。
> 析构只 `clear()`，**绝不**销毁宿主的窗口/连接。新增 `bool moveToHostSurface(void* handle)`：采纳新 id → 丢 surface/swapchain →
> 在**同一个 `VkInstance`** 上重建 surface → 重算格式；**格式变了就返回 false**（调用方回落整会话重建），否则 `buildSwapchain()`
> 后返回 true。插件侧：`initialize()` 用它；`setWindowHandle()` 在活会话上改为"移动"；`shutdown()` 删 `releaseWindow()`；
> `CMakeLists.txt` 把 `VSG_MAX_DEVICES` 恢复默认。宿主侧：`RenderControl` 在句柄变化时改为"重新公告"，不再 shutdown+initialize。
> **为什么这样比"换个窗口重建"划算**：`resize()` 这条路**今天已经在跑**——`Xcb_Window::resize()` 就是"重查几何 + `buildSwapchain()`"，
> 而 `buildSwapchain()` 不碰 `_renderPass` ⇒ "重建 swapchain 而设备与管线不动"是**已验证的机制**；D28 没有管线缓存，
> 所以少一次设备重建省下的是**全部 PSO**。
> **判据**：① 新自检相位 `host surface move`（自建 X 窗口 → 后端挂上 → 销毁并重建另一个窗口 → 重新公告）断言：会话未被重建
> （阶段缓存与管线计数不变）、移动后回读像素仍正确、**旧宿主窗口仍然存活**（`xcb_get_geometry` 有回包）、0 VUID；
> ② 55 行证据逐字节不变（默认参数必须与 vsg 的 XCB 窗口一致——render pass/swapchain 仍是 vsg 的代码，这是"只换 `_initSurface`"的回报）；
> ③ 设备上限恢复默认（任何路径想要第二个并发设备都会**抛异常**，门禁会红）；④ `test_vsg` 289 / `test_graphics` 272 / `test_core` 82 /
> 三脚本 0 / app 门禁 PASS。
> **变异（必须红）**：把 `moveToHostSurface()` 改成回落整会话重建 ⇒ 断言①红；让析构照 vsg 那样 `xcb_destroy_window` ⇒ 断言（旧窗口存活）红。
> **风险**：与 vsg XCB 窗口在默认参数（present 模式/格式/深度格式/图像数）上漂移——证据门禁会抓；格式真变时回落整会话重建（今天的行为）；
> Windows 路径本机只能编译不能验。**不做**：Wayland（vsg 1.1.16 无 Wayland 窗口实现）。


> 2026-09-16 **继续：候选清单的裁决 + 那笔账的调查（无代码改动，只动文档）**
> **stash 已删**：`derived+instr`（上一轮的 WIP + 自检探针，已被提交的工作覆盖）；删前把它的 `--stat` 记进了提交信息。当时 `git stash list` 还有过一条历史遗留——这正是"二进制 A/B 前先验身份"那条教训的实物。
> **C4（一帧瞬态变体 → 派生 `RenderGraph`）：不做。** 查细后的理由两条：① `settleSubmittedFrame()` 的换回**必须发生在提交之后**——瞬时变体一旦被**记录**，深度图像就已经回到稳态变体所期望的布局，而"开了帧但没提交"的那一帧什么都没记录，瞬态必须**继续挂**；改成"每帧开始时无条件下调回稳态"会在那种帧上记录一个声明了错误 initialLayout 的变体（比现状更差）。② 正确的版本是"图自己在被记录时消耗掉这一枪"（派生 `accept(RecordTraversal&) const` + `mutable`/`const_cast` 改 `renderPass`），能删掉 `PassObjects::transient` 与两处调平，但代价是新增一个类 + 一处"const 录制里改状态"，对一个**没有实测缺陷**的小簿记不划算。
> **C1（宿主表面窗口）：登录路线，现在不做。** 前提**已证实**：`RenderControl::initializeBackend()`（`src/fw/appfw/src/gui/RenderControl.cpp`）在 Qt 重建平台窗口时确实走 `engine->shutdown()` + `initialize()` 整会话重建，而 SDK 契约（`RenderBackend::nativeHandle()` 的注释）本来就写着"重新公告 setWindowHandle 就是让后端**迁到**新表面的方式"——两边合起来就是**当前后端不能迁**。改成派生 `vsg::Window` 接管宿主表面的好处很硬：删掉 `VSG_MAX_DEVICES=4`（抬高上游上限）、`releaseWindow()`（否则析构会 Destroy 宿主的 HWND）、以及表面重建不再丢设备与全部 PSO（D28 无管线缓存时这笔最贵）。**但现在不做**：这条路在本环境**没有任何门禁覆盖**（门禁跑的是无窗口的 app demo；D21 仍标着"Qt 子窗口主路径待定"），而它要新增 Xcb/Wayland/Win32 的 surface 代码——无门禁可验的平台码与本仓库的规矩相背；等有人能在真窗口环境验的那一轮再动。（**同日更正**：这条里的"没有任何门禁覆盖"**是错的**——本机 `DISPLAY=:0` 可连、app 门禁本来就跑在窗口上；C1 随即开工，见上面的 C1 brief。）
> **C2（管线归属下沉到 `Command`）**：维持登记，等实测需要（D28 启动耗时 / §9.4 tile GPU）。
> **那笔 +0.43 s（现已收敛为 +0.47 s 的"一次性"账）**：`perf` 要提权、本机无 valgrind，于是树内临时装 SIGPROF 采样器 + 时间戳对齐 + 三个扰动实验（详见 `docs/backend.md` 5.3.1）。结论：**工作逐行相同、计数相同、与帧数无关、无热点，差在软件光栅器的一次性开销**；"代码布局"这条被三个扰动实验否掉（旧猜测已删）。**未做**：release 复测（全量 reconfigure+build）。

> 2026-09-16 **审查轮次 3：接口 / 命名 / 文档收尾（任务表 T1–T16）**
> 判据（整批）：build 0 error/0 warning；`test_graphics` 269→**272**、`test_vsg` **289**、`test_core` **82**；
> 三脚本 0（文档 58 单元、include 659 文件、诊断格式 31 文件）；lavapipe **RESULT: PASS** 且 **55 行证据逐字节不变**。
> - **接口瘦身 29→27 虚函数**：删 `RenderBackend::materialManager()`（返回后端内部缓存，连后端自己都不经接口拿；
>   `SceneBridge::materialManager()` 是**另一个**函数）；`isPassScopeOpen()` **移出接口**（测试钩子不该长在公开契约上，
>   每个后端都要为它写实现）→ 成为 `VsgRenderer` 的方法。`nativeHandle()` **保留**：它是宿主面能力（检测窗口被系统重建），
>   只把"宿主会拿它比较"这个没人做的承诺改写清楚——按先例，**"背后什么都没有"的动作才删**（如 `releaseWindowLayer`）。
> - **`supportsRenderTargets()` 从"文档里的协商"变成真协商**：引擎每帧问一次，按 **episode** 只报一次
>   （`TargetBuildFailed`，点名 pass）。删掉它才是错的：引擎遇到不支持离屏目标的后端会**静默把内容画进窗口**。
>   `MockBackend` 现在答 `true`（带 `supports_targets` 开关）⇒ 既有管线用例零影响；用例
>   `ABackendThatCannotDrawTargetsIsToldOncePerEpisode` 钉住"只报一次 + 条件消失后重新武装"。
> - **清屏三 setter 不合并（撤回原提案）**：`isClearEnabled` / `clearColor` / `shouldClearDepth` 是**三个正交轴**
>   （8 处生产调用点；"清色但保深度"是真实状态）；开关关闭时另两轴失效是**任何可选属性**的通性（`viewport` 同理）。
>   但顺手挖出真东西：**清屏色默认是不透明 `(51,51,51,255)`（窗口灰），而 `RenderPipelineBuilder` 从不设颜色**
>   ⇒ 每个离屏 pass 的附件 0 也清成不透明灰（附件 ≥1 仍是透明黑，那是后端契约）。改成把来历写清，**不动默认值**（那会改画面）。
> - **`setOcclusionEnabled`/`occlusionEnabled` 删除**：三值枚举压成两布尔，`p.setOcclusionEnabled(p.occlusionEnabled())`
>   会把 `TestOnly` 静默升级成 `TestAndWrite`；getter 只有测试读。4 处生产调用点全是"关深度" → `setDepthMode(Disabled)`；
>   测试改名 `DepthStyleIsExplicitAndIndependentOfClear` 并新增"**TestOnly 能存活**"这条断言。
> - **命名**：`RenderEngine::diagnosticCount()` → `backendDiagnosticCount()`（它的 3 个读者读的是**后端**计数）；
>   布尔谓词 `valid→isValid`、`bound→isBound`、`complete→isComplete`、`clearEnabled→isClearEnabled`、`enabled→isEnabled`
>   （最后一个是同类内一致性：改了兄弟就一起改）。
> - **`bumpRevision()` 新增**（`Buffer` 与 `Geometry`——唯一两个有 `setRevision` 的）：`setRevision(revision() + 1u)`
>   这个咒语此前在测试里 20+ 处重复，连 `Mesh.hpp:219` 的**生产代码**都在写它；新方法让"前进一格、不会倒退"成为易写且写不错的路。
> - **T7（已记待办，未做）**：专门写了扫描器量 Doxygen 覆盖——**公开函数 `@param` 缺口 = 0**（约定这半边本来就守住了）；
>   **缺 `@return` 的 103 处**，绝大多数是 `/** @brief Gets X. */ X x() const;`，`@brief` 本身就是返回值说明。
>   补 103 条 "@return The X" 属文档戏法，**建议把规则收窄为"`@brief` 已说明返回值时可省 `@return`"**；决定前不动
>   （`scripts/check_doc_symbols.py` 不查每参数标签，门禁不会因此变红）。
> - **T6 遗留（已决定不改）**：`Light::castShadow()`、`RenderTarget::depthPromotion()`、`OrbitCameraManipulator::zoomToCursor()`
>   三处不合 `is/has` 前缀，但其 `is/has` 形式读起来更差——与 `RenderPass::shouldClearDepth()`（**请求**而非状态，头文件已写明理由）同理。
> - **T10（重新定性，只改注释）**：`setLights` 的 `reserve` 与 `setPassInputs` 的"assign 保住 buffer"注释**陈述了代码做不到的事**
>   ——`resetPassRequest()` 是 `state.request = VsgPassRequest{}`，**每个 pass 整体赋值两次**（beginPass + endPass），容量照样丢。
>   修法要么字段化重置（丢掉"新增字段永不忘重置"这个**刻意**保证，见该函数注释），要么把两个 buffer 移出 request（~8 处改名）；
>   **收益（~2 次分配/pass/帧）不值这个保证**，故只把注释改成事实，并写下将来真要动时该怎么做。
> - **T15（已量，待做）**：`VsgRendererState&` 的实际使用面量出来了——真正收它的**定义**是 **38 个**（先前口径 81 含头文件声明），
>   **24 个只碰 ≤2 个字段**，均值 **2.6 字段 / ~30**；只有 3 个碰 ≥8（`setupContentSlot` 12、`drawScreenProgram` 12、
>   `renderContentSlot` 8 —— 正是已标记过长的那些）。⇒ **收窄参数是机械可做的（24 处）且真能换来隔离**，
>   而重耦合只在 3 个长函数上，属于 T14/拆函数的同一味药。测量脚本：`/tmp/statefields.py`（按 `state.<field>` 读点计数）。
> - **T11 已完成**：`passes_active_this_frame` 从每帧 `std::set` 换成复用 vector（`AnnouncedPasses`：`mark/contains/drop/clear`
>   把"去重"这条规则收进类型），每 pass 每帧的节点分配归零——与 `RenderEngine::WiringState::outputs_` 同一拼法。
> - **T9 已完成**：`addOffscreenToScreen` 的 4 个连续 `int` 换成 `Viewport`（demo 原来传 `px/py/pip_w/pip_h`，测试传 `8,8,320,180`）；
>   `deferredLightProgram(bool with_shadow)` 拆成**两个具名工厂** `deferredLightProgram()` / `shadowedDeferredLightProgram()`
>   （调用点原来自己在写 `/*with_shadow*/` 注释解释实参——布尔不可读的自证），共享实现留在 .cpp 的匿名命名空间里。
> - **T13 第一步已完成（前向声明）**：新增 `VsgFwd.hpp`（**只在指针/ref_ptr 后面出现的 vsg 类型的前向声明单一家**，规则写在头里：
>   一旦某个头要调方法/取大小/按值持有，该类型就回到**那个头**的真实 include，而不是加到这个文件）。
>   实测：`#include <vine/vsg/VsgRenderer.hpp>` **1.21 s / 203 MB → 0.55 s / 152 MB**；`VsgRetireRing.hpp` 降到 0.18 s；
>   `VsgReadback.hpp` 1.01 → 0.74（**还没到底**：链上还剩 `VsgRenderTargetEntry.hpp`（View.h/RenderGraph.h/ShaderSet.h/
>   PipelineBarrier.h）与 `SceneBridge.hpp`（ShaderSet.h + 47 处 vsg 用法）。`VsgBackendUtility.hpp` 的 `vsg/app/Viewer.h`
>   是**纯多余** include（0 处使用）。
> - **T13 的两个 C++ 陷阱（必须记住）**：
>   ① **在头里声明析构函数会让编译器在这里实例化每个成员的析构**——为了算隐含的异常规格。`ref_ptr<T>` 的析构要
>   `T` 完整 ⇒ `member access into incomplete type` 出现在**每个**含该头的 TU。解法：析构声明**显式写 `noexcept`**，
>   外加 out-of-line `= default`。**默认构造同理**（其异常规格也要实例化成员析构），所以三个特殊成员（ctor/dtor/move-assign）
>   全部外移，并在 .cpp 里 `= default`。
>   ② **用户声明析构或移动赋值会抑制隐式移动构造与默认构造**，而 `VsgRendererState` 是**按值构造并返回**的（测试夹具）
>   ⇒ 必须把**移动构造和默认构造都显式声明**（否则 `return state;` 落到被删除的拷贝构造上）。
>   ③ 顺带：源文件现在**必须自己 include 它用到的 vsg 头**（`VsgContentSlot.cpp` / `VsgRetireRing.cpp` / `VsgViewCompiler.cpp`
>   各补了 `vsg/app/Viewer.h`）——这正是 `VsgFwd.hpp` 规则的另一半。
>   **改动前的三个拉取者**（`VsgBackendUtility.hpp` / `VsgRenderer.hpp` / `VsgRendererState.hpp` / `VsgRetireRing.hpp`）里，
>   `VsgRenderer.hpp` 的 14 个 vsg include **一个都不需要**（vsg 类型只出现在注释里，成员全是插件自己的类型，且它的析构早已 out-of-line）。
> - **T13 第二步：卡在"签名词汇"而不是 include 卫生（已核实，未做）**。`VsgRenderTargetEntry.hpp` 本身可清（所有 vsg 用法都在
>   `ref_ptr`/指针后面，唯一按值的是 `::vsg::vec4`，那个头很便宜），但它 **include 了 `SceneBridge.hpp`**，而后者把 vsg 的**类型词汇
>   写进了自己的签名**：`::vsg::DataList`、`::vsg::ShaderStages`（**按值成员**）、`::vsg::uintArray&`、`::vsg::dmat4&`、
>   `::vsg::GraphicsPipelineConfigurator&` —— 前向声明救不了，这些是 typedef / 按值类型。而链路余下的成本**正好等于**
>   `#include <vsg/utils/ShaderSet.h>`（实测 0.73 s；当前 `VsgReadback.hpp` 0.74 s）⇒ **只清 `VsgRenderTargetEntry.hpp` 收益为零**。
>   三条路：①把带 vsg 类型词汇的入口从 `SceneBridge.hpp` 移到只被 .cpp 包含的 `detail` 头（调用者全是内部的
>   `SceneBridgeGeometry.cpp`/`SceneBridgePipeline.cpp`）——这是真正的修法，保住边界头；②把词汇换成插件自己的类型；
>   ③就此停在"边界头 1.21→0.55"，让内部 TU 继续付 `ShaderSet.h`。**建议 ①，当专批做**（纯头重组，行为不变）。
>   另外：**PImpl 在这里不划算**（已量）——`VsgRenderer.hpp` 10 个消费者里只有 4 个"只需接口"（PImpl 只帮这 4 个 ≈1.6 s），
>   6 个本来就要内部；而第二步做完后 `VsgRenderer.hpp` 自己就会掉到 ~0.2 s，不需要 PImpl。PImpl 用在 `VsgRendererState`
>   上则是明确错的（该类型自己写明"没有 d-pointer"的理由 + 38 个 detail 函数收它 + 6 个测试直接读字段）。
>   **PImpl 的触发条件**：插件头若要作为给宿主的契约发出去（要 ABI 稳定），那时它买的是边界声明而非编译时间。
> - **T13 第二步：诊断修正 + 尝试与回滚（2026-09-16，下一批按此执行）**。真正的阻塞**不是"签名词汇"而是成员数据**：
>   `SceneBridge.hpp` 的 `RetainedBinds`（`ref_ptr<Commands>`、`array<ref_ptr<BindVertexBuffers>>`、`ref_ptr<BindIndexBuffer>`）
>   与每 draw 的 `white_colors`/`zero_texcoords`/`derived_normals` 都是 vsg 节点，前向声明救不了；而状态的内容槽
>   **按值**持有 `SceneBridge bridge;` ⇒ 状态头必然要它完整 ⇒ 整条链一直付 `ShaderSet.h` 的 **0.79 s**（链本身 0.83 s）。
>   **两条路**：**A** 给 `SceneBridge` 上 PImpl——但 22 个测试文件正是**读它内部**的设备无关单测（`RetainedBinds`、每 draw 数组都被断言）
>   ⇒ 用可测性换编译时间，不推荐；**B（推荐）把重的成员躲在指针后面**：`ContentSlot::bridge` 改 `std::unique_ptr<SceneBridge>`，
>   于是 `VsgRenderTargetEntry.hpp` 可前向声明 `SceneBridge`，**`SceneBridge` 类与 22 个测试一行不改**，状态链摘掉 `ShaderSet.h`。
>   这是 PImpl 的同一条原理，只用在真正贵的那个成员上。外部调用者只有 `uintArray`（`ChannelSliceTest`/`SceneRulesTest`）⇒ 边界不是问题。
>   **已尝试的第一步 + 它的代价（已回滚，树保持绿）**：把 `VsgRenderTargetEntry.hpp` 的 vsg 头换成 `VsgFwd.hpp`（扩到 18 个类型）
>   + 只留 `ref_ptr.h`/`vec4.h`，结果 **99 个错误、全是预期两类**：`VsgReadback.cpp` **38** 个（源文件必须自己 include 它用到的 vsg 头）
>   + **61** 个 `ref_ptr` 析构实例化（说明 entry 里**按值**持有的 vsg 类型在多处被销毁 ⇒ entry 的 ctor/dtor/move 必须像 `VsgRendererState`
>   那样**显式 `noexcept` + 定义外移**）。⇒ **这一批要留出 N 个构建循环**，顺序：① entry 的特殊成员外移；② 逐 TU 补 include；
>   ③ 路 B（`unique_ptr` 化 + 建槽处的分配）；④ 量 `VsgReadback.hpp` **0.83 s → ~0.3 s**。判据沿用：构建 0/0、`test_vsg` 289、
>   `test_graphics` 272、三脚本 0、lavapipe **55 行证据逐字节不变**。
> - **T13 第三步（PCH）已试、已否证、已回滚；T13 就此停在第一步（2026-09-16 定论）**。
>   ① 我先前报的"33 TU × 0.5 s ≈ **16 s**"是 **CPU 秒而不是墙钟**：这台机 **24 核**，触碰链根头
>   （`VsgRendererState.hpp`）后重建 `gfx_backend_vsg + vsg_backend_selftest + test_vsg` 的**墙钟只有 1.64–1.95 s**。
>   ② 试了三行 `target_precompile_headers`（覆盖上述三个目标、16 个 vsg app/state 头）⇒ **同一负载反而慢 4–6 倍**：
>   `1.66 s → 14.52 / 6.69 / 10.56 s`，三个 PCH 各 **~40 MB**；已 `git checkout` 回滚，基线恢复 1.95 s。
>   ③ ⇒ **结论：T13 停在第一步**（已提交：`VsgRenderer.hpp` **1.21 → 0.55 s / 203 → 152 MB**）。剩余的头批次
>   （路 B + pipeline-factory 前向声明）与 PCH 在**这个负载 + 这台机**上都不划算——判据是墙钟，不是 CPU 秒。
>   ④ 何时该重新量：CI 变单核、或 TU 数大幅增加、或链上出现更贵的头（届时应先量墙钟再决定，不要照抄"16 s"）。
>   ⑤ 教训（通用）：**优化编译时间必须报墙钟并注明核数**；CPU 秒乘以 TU 数会高估一个数量级。
> - **T16 已完成：编译上下文泄漏已修（2026-09-16），最终形态是"派生 manager"，不是"换 manager"**。
>   - **机制（最终）**：会话的 manager 是本后端的 `detail::VsgCompileManager`
>     （`vsg::Inherit<vsg::CompileManager, …>` + `create()`；`~CompileManager()` 是 protected，派生类把析构声明 public 即可）。
>     能这么做是因为 `CompileManager` 的池（`compileTraversals` / `numCompileTraversals` / `takeCompileTraversals`）是
>     **protected**、`CompileTraversal::contexts` 是 **public**：于是 `forget(view)` 做出了上游 `remove(view)` 的效果，
>     **不用打 vsg 补丁**（先前记的"①上游加 remove"因此不需要了）。池被换成"一条无上下文的 traversal"，本后端自己注册每条
>     上下文；槽死时三处拆除点（`unhookTargetPasses` / `resetContentShaderSlots` / `erasePassFromTarget`）调
>     `detail::forgetCompileContext`。槽上回到一个 **bool**，会话上没有 generation，也没有替换规则——比 renewal 方案更简单。
>   - **实测（lavapipe，二进制已按指纹+符号验明）**：`churn START content_slots=4 compile_contexts=60` → **4/4**；
>     `churn END` 63 → **7/7**（**恰好一槽一条**）；单次编译要过一遍的上下文数均值 53.5（8962/167）→ 与存活槽同量级；
>     `stage_cache` 恒 1；释放代价 **118 次调用、摘掉 114 条、13 ms**（实测）。门禁全绿（build 0/0、289/272/82、三脚本、
>     `gfx_lavapipe_check.sh` PASS + 55 行证据逐字节不变）。
>   - **仍未查明的一笔账（重要，别再重复我这次的误判）**：整份改动相对修前的墙钟 **2.79–3.03 s → 3.23–3.40 s**（+0.43 s，≈+15%）、
>     CPU **+0.42 s**，两份各自独立构建的修前二进制都复现。但它**不是**这次的逻辑：① 不是编译上下文（修后池内容与修前相同，
>     一槽一条）；② 不是释放（13 ms）；③ 不是 manager 安装（每会话一次）；④ 不是自检新增的两次 `retentionStats()` 与断言。
>     ⇒ 按排除法更像**代码布局/代码生成效应**；下一步得用 profiler（本机 `perf` 受 `perf_event_paranoid` 限制）。
>     **先前那条"编译穿过派生上下文慢 1.6×"的归因已被推翻**（派生上下文在最终方案里根本不存在，慢还在）。
>   - **方法论教训（这次绕了远路的根因）**：`git stash push -q` **静默失败**过一次，于是有几个"修前二进制"其实是我自己的代码，
>     一度得出"没有差别"的错结论；还发生过把 renewal 版当成 derived 版比较。⇒ **二进制级 A/B 之前先验身份**：
>     `nm -C <bin> | grep <symbol>` + `VINE_PROBE_RETENTION=1` 指纹（修前 60 / renewal 0–3 / derived 恰好等于存活槽数）。
>   - **被取代的设计**：`renewCompileContexts`（整只换 manager + generation + "至少一半是废的"规则）——它能批量带走废注册，
>     但会让活槽注册一起失效（重新注册 + 重新编译）且更复杂；已从树里移除（在 `f483245` 的历史里）。

> 2026-09-15 **审查轮次：graphics + vsg 后端逐条修复（见 `.ai/design/graphics-vsg-audit.md`）**
> 12 条缺陷全部修完，每条都带门禁（单测/像素证据/变异验证）。**判据**：build 0 error；`test_graphics` 260→**269**、
> `test_vsg` 276→**288**；`scripts/vsg_selftest_evidence.sh` **55 行逐字节不变**。
> - **透明度只有一条通道**：前向着色改成 `alpha = draw.params.x`（不再乘 `material.diffuse.a` / `texel.a`）。
>   原因：材质是**共享**的，乘进去会让所有用它的 drawable 一起半透明，而排序用的是每 drawable 的 opacity，
>   且延迟路径把 alpha 丢掉 —— 同一资产在 forward / deferred 会不一致。纹理只贡献颜色。
> - **材质 ABI 瘦身**：`VineMaterialBlock` 删掉 `emissive`/`alphaMask`/`alphaMaskCutoff`（无人填无人读），
>   **80B → 64B**，`static_assert` 与 `ShaderAbiTest` 同步。`LightPushBlock::projparms` 保留但在两侧都写明
>   "给自建深度重建程序预留"，引擎自己的点亮程序不读它。
> - **正交相机**：`Camera` 现在保留**完整窗口**（`orthographicLeft/Right/Bottom/Top`），后端按四个边界建
>   `vsg::Orthographic`。以前只留 height ⇒ 非对称窗口下 CPU 剔除与 GPU 渲染是**两个视锥**。拾取射线也走同一窗口。
> - **拾取**：`screenToWorldRay` 的基改成**读视图矩阵的行**（退化 up 不再产生 NaN，与 `lookAt` 的兜底一致）；
>   奇异世界矩阵（某轴缩放为 0）改用 `invert()` 检测后**跳过**（`inverted()` 会静默返回原矩阵 → 假命中，已变异验证）。
> - **收集路径（Scene）**：①`Geometry` 缓存**局部**包围盒（键 = positions 缓冲指针 + 缓冲 revision + 段 + geometry
>   revision；可观测 `localBoundsComputationCount()`）——以前每帧全场景**扫描顶点**，剔除一分钱都没省；
>   ②状态折叠改成**自顶向下**（`InheritedState`），不再每叶 3 次上行 + 一次 vector 分配，`StateNode.hpp` 的上行版保留，
>   两者由 `SceneTest.TheFoldedStateAgreesWithTheUpWalkingHelpers` 钉住一致；③新增
>   `Scene::collectRenderCommandsShared()`（不可变共享表），pass 路径不再每 pass 复制整张命令表，
>   只有设了 program override 的 pass 才 fork。
> - **引擎侧每帧分配**：`validateWiring()` 改为**声明驱动**（`RenderPass::wiringRevision()` + `publish/unpublish`
>   标记；可观测 `wiringValidationCount()`）；内容槽的 `ViewportState` 改成**一个、原位更新**（不再每槽每帧 new）。
> - **每 drawable 槽池**：`SlotAllocator`（无设备可单测）拒绝**重复归还**并计数（`Stats::refused`），且拒绝发生在写字节
>   之前 —— 否则会清掉现在拥有该槽的 drawable 的 opacity。`Stats` 新增 `bytes`，`VsgRetentionStats` 新增
>   `slot_bytes`/`mesh_streams`/`textures`。
> - **踩坑（必须记住）**：改了 `libviGraphics` 里类的布局后**只 build 目标**会留下陈旧二进制
>   （`vsg_backend_selftest` 旧布局 + 新库 ⇒ 结尾 `malloc(): largebin double linked list corrupted`）。
>   **证据门禁前必须整包 build**。
> - **本环境的 `http(s)_proxy` 是坏的**（`127.0.0.1:7890` 在 TLS 握手中断连），**直连正常**：FetchContent 的
>   `git fetch spdlog`（reconfigure）与 `git push` 都会报 `GnuTLS, handshake failed`。绕过：
>   `env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY git push ...`；构建目录已设
>   `FETCHCONTENT_FULLY_DISCONNECTED=ON`（`build/CMakeCache.txt`，gitignore，依赖已就位）。
>   `vine_shader_check.sh` 需要 Linux `glslangValidator`（本机只有 Windows exe）⇒ 无法运行，着色器编译由
>   `test_vsg` 的 `GlslCompileTest` 覆盖。

> 2026-09-14 **着色器内的插值变量前缀 `vn_` → `vine_`**（补上 2026-09-13「着色器内标识符前缀统一 `vine_`」的尾巴）
> - `src/viz/graphics/shaders/` 下 9 个源文件全改：`vn_uv` → `vine_uv`、`vn_view_pos` → `vine_view_pos`、
>   `vn_view_normal` → `vine_view_normal`、`vn_color` → `vine_color`、`vn_texcoord` → `vine_texcoord`、`vn_dir` → `vine_dir`。
>   顶点输入本来就是 `vine_Vertex` / `vine_Normal` / `vine_Color` / `vine_TexCoord0`，现在着色器里的标识符
>   不分输入 / 输出 / 插值，一律 `vine_`。
> - 同步改：**ABI 文档**（`BuiltinShaders::fullscreenVertexProgram`、`ScreenPass::setProgram`、
>   `VsgPipelineFactory` 的用户 program 段）与**钉住成品文本的门禁**（`EmbeddedShadersTest`、
>   `ForwardShaderSetTest`、`OverlayStagesTest`，以及 `GraphicsTest` 里照全屏 ABI 写的宿主 program 样例）。
> - **行为中性**：只换标识符，location / 绑定 / 取值范围 / Y 方向一个不动 ⇒ 画面不变（任何 shading 的 ABI 都没变）；
>   `vine_shader_check.sh` 9 shader × 各 define 组合照旧全编。
> - 教训：**插值名虽然只在顶点/片元两段之间可见，仍是 ABI 的一部分**——文档与单测都会钉住它，改名要一起改；
>   漏改一处不是编译错，而是某个门禁按旧名 find 不到（前者喧哗，后者难查）。

> 2026-09-14 **texcoord 的 kind 显式化：`VINE_TEXCOORD_CUBE`（缺省 = UV）→ `VINE_TEXCOORD_UV` / `VINE_TEXCOORD_CUBE`**（用户口径：“不要隐式，要显式”）
> - **两个轴，别再混**：`VINE_DIFFUSE_MAP` 是**门**（有没有贴图要采）——它由 vsg 的赋值门控置位（`ArrayConfigurator::assignArray`：一赋数组/描述符就置位），
>   并且决定 pipeline layout 里那个属性/描述符**存不存在**（`ShaderSet::createDescriptorSetLayout` 按 `defines.count(binding.define)` 过滤）。**kind** 是“槽是哪一种”，
>   由后端按**数据宽度**手插（三标量 ⇒ CUBE，否则 UV）。一个 binding 只能挂一个 define、`getAttributeBinding` 是首个匹配 ⇒ 门不可能被 kind 名取代。
> - 四个内容 shader（forward / gbuffer 两对）的 kind 链改成 `#if CUBE / #elif UV / #else #error`：**采样了槽却没说 kind 的变体现在编译不过**，
>   不再静默当 UV。
> - 后端 `SceneBridgePipeline`：**每个 variant 一定插恰好一个 kind**；自定义 program 的 opt-in 判据改成“问它被给的那个 kind”（SDK 的 gbuffer 两个名字都声明）。
>   原来只问 `VINE_TEXCOORD_CUBE`，于是“声明 CUBE”的程序在 2-wide 槽上也套用了槽规则——现在按 kind 分别生效。
> - **天空盒例外**：`builtin_skybox.*` 没有门，而 `VsgPipelineFactory` 的**程序级编译是零 define**（`compiledProgramStages`；variant 由 `ShaderSet::getShaderStages(scs)` 按 defines 重编）
>   ⇒ 对它“没有 kind”是真实构建状态，`#error` 会把整个 skybox program 变成 declined。所以它保留“配对”分支，并在注释里点名 `VINE_TEXCOORD_UV`。
> - 为什么不反过来（采样器 → 坐标）：顶点侧宽度只能由**数据**决定（vsg 取数组自己的 format 建顶点输入，Vulkan 要求 `in` 类型与之兼容），
>   不一致时只能让**纹理**让步（白 fallback + 报告）；让数据让步就得更凭空造分量。反方向还要两个 define（采样器 kind + 通道宽度），名字更多。
> - 门禁：`vine_shader_check.sh` 矩阵 = on/off define 组合 × **恰好一个 kind**（每 shader 16 组）；`ForwardShaderSetTest.ASampledTexcoordSlotMustStateItsKind`
>   用 glslang 直接钉（带门无 kind ⇒ 必须失败；UV / CUBE / 零 define ⇒ 必须过）；`ProgramSamplingTest` 钉 per-kind opt-in。
> - 判据：build 0 error（仅 `OverlayStagesTest` 两条**既有**的 `const const` 警告）；`vine_shader_check.sh` PASS（9 shader × 16 组合 + 嵌入字节一致）；
>   test_vsg **250**、test_graphics 259 全过；`gfx_lavapipe_check.sh` PASS —— selftest 证据基线 **55 行逐字节不变** + app 阶段 validation clean（只换分支名，画面没动）。

> 2026-09-13 **shader 文件命名规则：目录里每个文件都是 `builtin_<角色>.<阶段>`**（本条覆盖此前两次取名）
> - 现名：`src/viz/graphics/shaders/` 下 `builtin_forward.{vert,frag}`（原 `std_forward.*`，更早 `vine_forward.*`）、
>   `builtin_gbuffer.{vert,frag}`（原 `gbuffer_geometry.*`）、`builtin_deferred_lighting.frag`（原 `deferred_light.frag`）、
>   `builtin_skybox.{vert,frag}`、`builtin_screen_copy.frag`、`builtin_fullscreen.vert`。
> - 规则：`builtin_` 划分归属（引擎文本 vs 宿主 shader）；角色是**渲染器**的词，不重复阶段或目标（`gbuffer` 不必写 `_geometry`）
>   且不带产品名或 C++ 的词（`std_forward` 读起来是 `std::forward`，弃用）；常量按文件名推导
>   （`kBuiltinForwardVert`）；**program 名 = 文件名去后缀**（`builtin_forward` / `builtin_gbuffer` / `builtin_deferred_lighting`，
>   多 program 加后缀 `_flat` / `_shadowed` / `_<N>`）；**pass 名不带前缀**（`gbuffer` / `deferred_lighting`，
>   因为 pass 画的是宿主可以替换的 program，见 `RenderPipelineBuilder` 的 `gbuffer_program` / `lighting_program`）。
> - 门禁：`test_graphics::EmbeddedShadersTest.EveryShaderNameFollowsTheInventorysRule`（规则 + 反例都测）。
> - 行为中性：证据基线 55 行逐字节不变，`vine_shader_check.sh` 9 shader PASS，test_graphics 258 → **259**。
> - 历史章节沿用当时的名字。

> 2026-09-13 **收尾：vsg 内建着色在后端与测试里都不再出现**
> - **材质**：`VineMaterialBlock` 进 SDK（`ShaderAbi.hpp`，与 `VineViewBlock/DrawBlock` 并列，带 sizeof/offsetof assert），`VsgMaterialManager` 的 payload 从 `vsg::PhongMaterialValue` 换成 `vsg::ubyteArray(sizeof(VineMaterialBlock))`；两侧 ShaderSet 的 `material` 声明同步。以前靠“字段次序恰好一致”在工作。
> - **光照**：删 `buildLightNode` / `setGroupLights` / `makeAmbientLight` / 槽的 `light_group`·`headlight_seed`·`vsg_lights` / `seedSlotLight` / `SceneBridge::hasOwnLightsBlock`。灯**只有一个来源**：每槽的 `vine_lights` block。丢灯报告改由 `fillVineLightsBlock` 的**返回值**驱动（它知道 block 装下了几盏：禁用 / 非 ambient·directional / 第二盏 ambient / 第 4 盏 directional 都算没装下）。
> - **属性名**：桥只按 `vine_*` 查（删掉 `vsg_*` 回退）；一组都不声明 ⇒ 报 Warning/ContentSkipped（“不是我们的 set”）。
> - **不透明度载体**：顶点色 alpha 载体删除（`buildGeometryData` 不再有 `opacity_carrier`，桥的 `colors`/`last_opacity` 与逐帧改写循环删除）；loc2 作者色**原样绑定**，没作者色绑静态白。不透明度只走 `vine_draw` block。
> - **测试**：89 处 `vsg::createPhongShaderSet()` → `tests/test_vsg/TestContentSet.hpp::testContentSet()`（我们的 forward set）。
> - **工具**：`vsg_probe` / `vsg_shader_dump` 删除，`gfx_lavapipe_check.sh` 从 4 段降到 2 段（selftest + 证据 + app）。
> - 判据：**证据基线 53 行逐字节不变**；test_graphics 248；test_vsg 251 → **247**；lavapipe PASS。

> 2026-09-13 **命名：`vine_forward.*` → `std_forward.*`，着色器内不再用 `vsg_` 前缀**
> - 前向着色的两段改名（同一个 program）：`src/viz/graphics/shaders/std_forward.{vert,frag}`，常量 `kStdForwardVert/frag`；program 名字 `std_forward` / `std_forward_flat`（原 `vine_forward` / `vine_flat`）。
> - **着色器内标识符前缀统一 `vine_`**：`vine_Vertex` / `vine_Normal` / `vine_Color` / `vine_TexCoord0`（后端 `addAttributeBinding` / `assign_array` 的名字同步；测试按名字查绑定的一处也同步）。这条当时还留了个尾：`vsg_probe`（已删）和“描述 vsg 内建 set”的段落。
> - 判据：**行为中性** —— 证据基线 53 行逐字节不变（着色器文本只换标识符/文件名，位置与 ABI 不变）；`vine_shader_check` PASS（7）；test_graphics / test_vsg 不变；lavapipe PASS。

> 2026-09-13 **全屏着色也归 SDK：`ScreenPass` 必须命名 program，后端不再有 shader**
> - **删掉的东西**：`RenderBackend::drawScreenTexture`（两个重载）、`VsgRenderer::drawScreenTexture`、`detail::drawScreenTexture`、`makeScreenTextureNode`、`ScreenPass::{setSourceAttachment,sourceAttachment,attachmentToSample}`、后端槽表 `ScreenSlot`/`screen_slots`/`SlotKind::Screen`、以及**整个 `src/plugins/gfx_backend_vsg/shaders/`**。清单 `cmake/VineShaders.cmake` 从两个 owner 变成一个（7 个源全在 `vine/graphics/EmbeddedShaders.hpp`）。
> - **新增**：`BuiltinShaders::fullscreenVertexProgram()`（顶点段 + vn_uv 的 ABI 写成文档）与 `screenCopyProgram(int attachment = 0)`（片元段；**binding 就是附件**，N≠0 时替换 `layout(binding = 0)` 那一行，靠被单测钉住的 marker）。全屏只剩**一条**路径：`drawScreenProgram`。
> - **规则**：`ScreenPass` 没有 program ⇒ 不画 + 接线期报一次（"no program"）；接线检查按修复顺序 `continue`：没有 program → 没有输入 → 没有相机。**phase 3b 删除**（"无 program 只能采一张彩色附件"）：原来被它报的"只声明深度"现在合法。
> - **判据**：**证据基线 53 行逐字节不变**（PiP/合成/深度共享全不动 ⇒ 换绑定路径不改画面）；test_graphics 248；test_vsg 252 → **251**（`EmbeddedShadersTest` 5 条 → `OverlayStagesTest` 4 条）；`vine_shader_check` PASS（7，全 SDK）；lavapipe PASS。
> - **坑 1（lavapipe 抓到，证据行却是绿的）**：给直驱屏幕画补 pass scope 时把 `RenderPassPtr` 建在帧循环里 ⇒ 每帧新身份 ⇒ 旧槽管线/图像在飞行中被释放 ⇒ `VUID-vkDestroyImage/Pipeline-*`。后端按 **pass 指针**认槽，pass 对象必须比帧活得久（引擎就是这么做的）。
> - **坑 2（脚本自身的 bug）**：`vine_shader_check.sh` 用 glob 找生成头，删掉 owner 后磁盘上的旧头让脚本去检查一个不存在的 owner。现在它**从 manifest 读 OUTPUT 行**。
> - **性能注意**：后端按 **program 对象**缓存编译结果，所以程序要建一次（相位/宿主持有），别每次 draw 新建。

> 2026-09-13 **着色只能显式指定 program：删掉 `ShaderPreset`，不兜底**（用户口径："不要兜底，必须显示指定着色器"）
> - **枚举删除**：`ShaderPreset` / `setShaderPreset` / `builtinProgram(preset)` 全没了；会话级入口是
>   `RenderEngine::setDefaultContentProgram(intrusive_ptr<const ShaderProgram>)` / `defaultContentProgram()`，后端是
>   `RenderBackend::setDefaultContentProgram`（默认 no-op）。内建工厂：`forwardProgram()`（`std_forward`）、
>   `flatForwardProgram()`（`std_forward_flat`，与 forward **同一对 stage**，片元源 `withDefine("#define VINE_FLAT 1")`）。
> - **引擎有默认，后端没有**：`RenderEngine` 构造时就把 `default_content_program_` 定为 `forwardProgram()`，`initialize()` 前转发，
>   运行中设置**立即转发**（旧实现 initialize 之后再设是静默 no-op，这个洞顺手补了）。后端 `persistent.default_content_program` **没有默认值**：
>   null ⇒ `makeContentShaderSet` 返回 null ⇒ 每会话报一条 Error 且**什么都不画**（"declined, not substituted"）。
> - **缓存与 key**：`VsgPipelineFactory::compiledStages(program)` 的缓存按 `(program 指针, 变体 hash)` 建 map，**value 拥有那个 program**（否则指针 key 会被宿主的临时 program 悬空）。
> - **顺手补的同类谎**：`PipelinePreset::{Forward,Deferred}Shadowed` 今天装配的就是无阴影版本，现在 `RenderPipelineBuilder::build` 会报一条
>   `DiagnosticCategory::UnsupportedRequest`（新枚举值）。为让 builder 走引擎的 sink，`RenderEngine::reportEngineProblem` 由 private 改 public（唯一的 API 扩大）。
> - **selftest 启动顺序**：后端不再有默认 ⇒ 必须**先** `backend->setDefaultContentProgram(forwardProgram())` **再** `initialize()`。
>   晚设的代价是实测出来的：起来后的 30 帧没有程序 ⇒ 多报 9 条诊断（1 条会话级 + 8 条每桥），`diagnostics` 相位
>   `diagnosticCount() == rejected + fallbacks` 当场变红（10 != 2）。
> - **判据（行为中性）**：证据基线 53 行**只改 3 行的词**（`preset shading:`/`live preset switch:` → `program shading:`/`live program switch:`），
>   **数字全不变**：42 / 765 / (255,255,255) / (10,20,30) ⇒ 画面一个像素都没动。test_graphics 247 → **248**（+1 `ShadowedPresetsReportThatTheyArePlaceholders`，**变异验证**：关掉 `reportEngineProblem` 必红）；test_vsg **252 不变**（四处结构性改写，不是新增）；shader check PASS；`check_diagnostic_formats.py` 0 suspicious；lavapipe PASS；ctest 仅 3 个既有失败。
> - **命名（同批）**：这一版最初叫 `setContentProgram` / `contentProgram()`，改名 `setDefaultContentProgram` / `defaultContentProgram()`：它设的是**默认值**（drawable 自己的 program、pass 自己的 program 都仍然优先），`setContentProgram` 读起来像"把内容全换成这个"。后端内部字段跟着改成 `default_content_program`（含 `no_default_content_program_reported` 与诊断文本）。行为中性：证据基线 53 行逐字节不变。
> - **文档/词表**：`preset` 这个词只剩 **PipelinePreset** 用；"引擎自己的 preset" 全部改成 "引擎自己的 program"（后端注释 + 诊断文本一起扫）。

﻿> 2026-09-13 **canonical 属性 location 只有 ABI 一处定义**
> - 删掉所有硬编码：`Geometry::setPositions/setNormals/setTexcoords2/hasPositions/…/localBounds`、`RayIntersection`、vsg 后端的几何构建（canonical 通道、派生通道、自定义通道过滤、loc→binding 映射）现在都问 `attributeLocation(VertexAttribute)`；`Geometry::kTexCoordLocation` 直接等于 `attributeLocation(TexCoord0)`。
> - 新增 `isCanonicalAttributeLocation(location)`（ABI 拥有）取代后端里那句 `location <= 2u || location == kTexCoordLocation`——“这个 location 是引擎的还是转发的”只该有一个回答。
> - **索引侧对齐（同日）**：`setIndices(buffer)` / `setIndices(buffer, first_index, index_count)` 两个重载（去掉默认参），1 参版在 `.cpp` 里一行转调 3 参版 ⇒ 四个角色都是"两种拼写、一条实现路径"；`Geometry::IndexStream = BufferSlice<uint32_t>` 的四个视图（`indices/firstIndex/indexCount/indicesBuffer`）读同一个段。守卫 `TheIndexStreamsTwoSpellingsShareOneSegment`（两拼写同段、整块跟随增长而段不跟随、空几何各视图一致）。
> - **段的重载（同日）**：canonical 角色现在两种拼写都行 —— `setPositions(buffer)` 整块（跟随增长）与 `setPositions(buffer, first_vertex, vertex_count)` 段（`count == 0` = 到末尾，**不是空**）；`setNormals` / `setTexcoords2` 同。三个新重载都是**一行委托**给 `addBuffer + AttributeChannel::slice`（一条实现路径，防漂移）；索引侧继续用默认参表达同样两种情形。守卫 `TheWholeBufferAndTheSegmentSpellingsAgree`（同 buffer/offset/components、覆盖相同、整块跟随增长而段不跟随、texcoord 段 3 顶点 = 6 scalar ⇒ 复制粘贴用错 stride 会红）。
> - 测试：`ShaderAbiTest.TheCanonicalPredicateMatchesTheLocations` + `GeometryAttachesCanonicalChannelsWhereTheAbiSays`（setter 落在 ABI 的 location 上、`positionCount()` 读的就是同一个通道）；`SceneBridgePipelineSharingTest` 里两处读通道也改用 ABI；GLSL 那半原本就由 `EmbeddedShadersTest` 钉着。
> - 判据：行为中性（值今天相同）——证据基线 51 行逐字节不变；test_graphics 240 → **242**；test_vsg 250；shader check PASS；lavapipe PASS。

> 2026-09-13 **完全不使用 vsg 内建 shader set**
> - `makeContentShaderSet` **只**调 `buildVineShaderSet`；删除 `buildShaderSet()` 与 `vineForwardShaderEnabled()`（`VINE_VSG_BUILTIN` 开关、两条基线的第二份、自检 `--builtin` 模式一起删）。
> - **没有有效 shader 就不画**（2026-09-13 口径）：没有自己 program 的 preset ⇒ `makeContentShaderSet` 返回 **null**（无替补），建槽时每会话一条 **Error**；槽没被注入 set ⇒ `buildStateGroup` 每桥一条 Error + 该 drawable 不入图；用户 program 编译失败 ⇒ 报告后**不再回落**到槽的 set。`SceneBridge::baseShaderSet()` 也不再兜底造 set；`setShaderSet()` 会 invalidate 保留 state（旧 set 的管线/描述符）。
> - `SceneBridge::baseShaderSet()` 无注入时建**我们的** forward set（原来 `createPhongShaderSet()`）；桥仍接受**外来** set（SDK 语义）。当时留了 `hasOwnLightsBlock()`/`vsg_lights` 服务外来 set——**同日收尾已删**（外来 set 不是受支持的配置，现在会报一条 Warning）。
> - 自检 preset 相位的 Pbr 段改成“与 StandardPhong 像素相同（±4）”——替补是引擎自己的模型，不是另一套着色。
> - 判据：两条基线 → **一条 51 行**（只 rewrite 那一行）；test_vsg 249（`EveryContentSetIsTheEnginesOwn` 取代 `TheForwardSwitchIsOnByDefault`）；test_graphics 240；lavapipe PASS。

> 2026-09-13 **preset 会话中途生效（重建着色侧，不重建 target）**
> - 症状：`RenderEngine::setShaderPreset` 以前只写 `persistent.shader_preset`，而 set 是**建 slot 时烘的**（程序 + "喂哪个光源" + View features 三者都在里面），所以运行中切换画不出来。
> - 做法：`VsgRenderer::setShaderPreset` 在已初始化会话上重建 window 三套 set + 走新 `detail::resetContentShaderSlots(state)`：每个 target 的 content slot 逐个 `detachSlotView`（**不 detach 就还在画旧 set**）→ 一次计数设备等待（`clearCache` 会释放共享对象注册表）→ 清 `content_slots` → 清 `depth_*_shader_set`（target 自烘的也要跟着忘）。**attachments / pass graph / 深度历史不清**——下一帧懒建 slot 用新 preset 重建。
> - 门禁 `runLivePresetSwitchPixelPhase`：**同一个** target+pass+slot 连画三段（背向光源的四边形）smooth 42 → 切 FlatShaded 765 → 切回 smooth 42；第三段专治"只往前不回头"。**变异验证过**：把重建短接掉（只写 preset）→ 两段都 FAIL。
> - 判据：两条基线 49 → **51 行**（新增 live-switch 行；`preset shading: FlatShaded` 行是 B 带来的）；build 0/0；test_vsg 244；test_graphics 240；shader check PASS；lavapipe PASS。
> - 坑：编辑时把 `runPresetShadingPixelPhase` 末尾的 `if (ok) fprintf(...)` 一起替掉了，于是那条证据行**静默消失**、基线行数不变（50 而不是 51）——基线行数不变而新增相位多打一行，就是"我删掉了别的行"的信号，要对 `git diff` 看增删两侧。

> 2026-09-13 **FlatShaded 进 SDK（P1 第一步）+ 像素级钉住"平直"**
> - `builtinProgram(FlatShaded)` 不再是 null：**同一对 stage**，片元源里注入 `#define VINE_FLAT 1`（`withDefine()`：**必须插在 `#version` 之后**——放前面是 GLSL 语法错，而编译失败会被"回落内建集"静默吃掉：第一版就这么画出了 vsg flat 的无光照材质色）。
> - `std_forward.frag` 增 `VINE_FLAT` 分支：法线用 `cross(dFdy(vn_view_pos), dFdx(vn_view_pos))`（面法线）。**叉乘顺序必须是这个**：Vulkan 帧缓冲行向下生长，`dFdx × dFdy` 得到的是背向相机的法线（实测：朝向相机的面一直停在大气项，换序后才是受光的）。
> - 新相位 `runPresetShadingPixelPhase`（取代原 preset-fallback 相位）两半都断言：①Pbr 回落内建 phong 集 → 必须受光（按 set 决定灯源）；②同一块四边形用**背向光源的作者法线**：平滑 preset 只剩大气项（42），flat 必须明显更亮（765）——**这才真的钉住"平直"**（否则平面四边形上两者同值）。
> - 着色门禁的 `VARIANT_DEFINES` 加 `VINE_FLAT`（7 shader × 8 组合全编）。
> - 判据：两条基线 49 → **50 行**（第 48 行因相位改用平行光而变值，其余逐字节不变）；build 0/0；test_vsg 243 → **244**；test_graphics 240；`vine_shader_check` PASS。

> 2026-09-13 **修缺陷：灯源必须按 SET 决定，不是按会话**
> - `VsgContentSlot` 的 `vsg_lights = !vineForwardShaderEnabled()` 是**会话级**判断，而“哪个灯源”是 **set 的属性**：`buildVineShaderSet` 对没有 Vine program 的 preset 返回 null ⇒ 回落内建集，而内建 **phong** 集从 vsg 的 view-dependent lightData 取光 —— 可 forward 开关开着 ⇒ 不建 vsg 灯节点 ⇒ **该 slot 全黑**（实测 Pbr 回落画 (0,0,0)，修后 (46,8,3)，与内建基线同值）。
> - 修法：`SceneBridge::hasOwnLightsBlock()`（“这个 set 读不读 `vine_lights`”）+ `ContentSlot::vsg_lights`（建槽时定一次，逐帧路径复用）。两份现有模式行为**逐字节不变**（都取同一条分枝）。副作用：用户 program 的 set 以前也拿不到灯，现在也有灯了。
> - 新相位 `runPresetFallbackPixelPhase`：切到 **Pbr**（引擎文档明确写“回落 StandardPhong”）、画进新离屏目标、断言中心既非清屏色也非黑。**注意 FlatShaded 测不出来**：vsg 的 flat shader 本来就不读光（不黑）。
> - **同日收尾后本条的两半都消失了**：没有“回落内建集”这回事（declined, not substituted），也就没有“一个 set 一个灯源”的问题——灯只有一个来源。（本条保留为“为什么当时要这么修”的记录。）
> - mutation：把赋值改回会话级 ⇒ 相位红（(0,0,0)）✓ 证明门禁真咬。
> - 判据：两条基线 **48 → 49 行**（只多这一行）；build 0/0；test_vsg 242 → **243**（+1 `TheLightSourceFollowsTheSlotSetNotTheSession`）；test_graphics 240；lavapipe PASS。

> 2026-09-13 **P10 收尾：`vine_draw` 改 per-drawable 槽 + dynamic offset（②`model` 不写）**
> - 新 `VsgDrawBlockPool`（session 级，随其他设备缓存创建/注入）：块 = `stride`(块大小按 `minUniformBufferOffsetAlignment` 向上取整) 的槽，一块(chunk)默认 64 槽；缓冲 + `DeviceMemory`(HOST_VISIBLE|HOST_COHERENT) + `MappedData<ubyteArray>`，**直接写映射内存**（每改一次不透明度 = 4 B，无 transfer task、不落后一帧）；`reserve/release` 带自由表，`descriptorSet(slot, layout)` 每 (chunk, layout) 一个 set。
> - set 从 s0/b3 移到 **s1/b0** 并用 `CustomDescriptorSetBinding`：它只给 **layout**，bind 由 `SceneBridge::appendDrawBlockBind` 按 drawable 追加（带 dynamic offset），模板命令仍共享。set1 的“范围”必须在 `descriptorBindings` 里另声明一行（vsg 的 `descriptorSetRange()` 只扫那里）。
> - 槽生命周期 = drawable：Item 保留时 `reserve()`，淘汰/`clearCache()` 时经 `advanceRetireRing` 延后释放（飞行中的帧可能还绑着那个偏移，release 时会清零该槽 params）。
> - **三个坑**（都真踩了）：① range 没声明 ⇒ pipeline layout 少一个 set、SPIR-V 引用不存在的 set ⇒ **lavapipe 段错误**（不是 VUID）；② 内建 phong 集自带 set1（材质）⇒ 往它绑我们的动态 set 也段错误，所以按 **layout 形状**（唯一 DYNAMIC binding@0）判定“是不是我们的 set”，并且只有 `forward_draw_block_` 的桥才领槽；③ 池必须**比桥活得久**：`shutdown()` 是整体赋值 state，池（声明靠前）会先死，桥的析构碰它就是 UAF ⇒ 析构**不碰池**（`flushDrawSlots` 只由 `advanceRetireRing` 调）。
> - 判据：两条证据基线 **48 行逐字节不变**（含 opacity 门禁那一行！）；build 0/0；test_vsg 241 → **242**（新增 `ThePerDrawBlockIsSetOneWithItsOwnBinding`）；test_graphics 240；`vine_shader_check` PASS（7）；**lavapipe PASS**。

> 2026-09-13 **P10：每 drawable 不透明度改走 `vine_draw` 块（修一个真缺口）**
> - SDK L1 块的 L2 落点：set0/binding3 `vine_draw` = `VineDrawBlock`(80B: `mat4 model` + `vec4 params`)；SceneBridge 每 drawable 一个 `floatArray`(20 float, `DYNAMIC_DATA`)，写 `params.x` 后 `dirty()`；`std_forward.frag` 改 `alpha = material.diffuse.a * draw.params.x`。
> - **修缺口**：P10 前半的"顶点载体 alpha = opacity"在 forward 路径**从未到达帧缓冲**（新像素门禁 `runOpacityBlendPixelPhase` 抓到：opacity 0.5 与 1.0 的像素完全相同）。诊断靠对照：同阶段换 **material 对象**像素会变（描述符路径 ✓），换载体字节不变（顶点路径 ✗），内建路径变（它的 shader 读载体）。
> - **踩坑（已修）**：`draw_block` 必须**在 buildStateGroup 之前**创建 —— 否则 wrapper 会绑 ShaderSet 的**样本** uniform（全 0），且之后再也不会重绑（state 不再 dirty），表现为"整场 content 全不可见"（alpha=0）。
> - 想要的副作用：opacity 不再进 variant 身份（删 `opacity_changes_state`/`wrapper_opacity_opaque`）；forward 路径**不再维护动态顶点载体**（`opacity_carrier = program==nullptr && !forward_draw_block_`）⇒ 作者着色 geometry 不再为透明度付 O(V)/帧，透明 drawable 也享受"丢派生属性"的精简变体。
> - 新门禁 `runOpacityBlendPixelPhase`：同一 quad 画 1.0 与 0.5，断言 `colour(0.5) == 0.5*colour(1.0) + 0.5*clear`（±4）且存储 alpha = 191（用来区分 255 = opacity 丢掉 / 128 = 混合没开）。两条证据基线 **47 → 48 行**：新增就是这一行，其余 47 行逐字节不变（含 forward/内建那 6 个着色数字）。
> - 判据：build 0/0；`vine_shader_check` PASS（7）；test_vsg 240 → **241**；test_graphics 240；两条基线 PASS；lavapipe PASS。

> 2026-09-13 **P10 前半：forward 路径接通透明度（已被上面取代，保留作教训）**
> - 做法：`std_forward.frag` 在 `VINE_VERTEX_COLOR` 下 `alpha *= vn_color.a`，载体 alpha 由 SceneBridge 按 `cmd.opacity` 维护；`drop_color` 仅在 `opacity >= 1` 时成立；opacity 跳 1 计入 state 身份。
> - **为什么被取代**：这条路在 forward 上根本没通（见上）。教训：**"CPU 侧写了正确的字节"不等于"着色器读到它"** —— 一个只看结构（绑了哪些属性、变体文本）的断言会全绿而画面纹丝不动；像素级差分才是判据。
> - 单测 +1（透明 → 4 条顶点绑定）已被换成 `OpacityIsNotPartOfTheVariantIdentity`（透明 = 不透明，2 条绑定）。
> - **同一条教训的第二次实证（同日，cube 方向槽）**：`std_forward.*` 用了 `#ifdef VINE_DIFFUSE_MAP` / `VINE_VERTEX_COLOR`，但源码缺 `#pragma import_defines`，而 vsg 只对 pragma 列出的名字发 `#define` ⇒ 两个分支**从未编译过**（内建 forward 路径一直不采样、不读顶点色），而全部结构性门禁（断言 define 名字出现在 stage 里）**全绿**。修法：加 pragma（必须在 `#version` 之后）；新门禁两条 —— `ForwardShaderSetTest::TheForwardStagesAskForEveryDefineTheBackendCanSet`（后端会设的每个 define 必须在 pragma 列表里）+ selftest 的 `built-in sampling` 相（**不设 program**，由引擎自己的 shader 采样一张双色 2D 贴图和一个六色 cube；变异验证：删 pragma 两行都 FAIL，只删 `VINE_TEXCOORD_CUBE` 则只有 cube 行 FAIL）。
> - **cube 方向 = texcoord 槽的第二种形状**：同一 location 8，2 分量 = UV，3 分量 = 方向；SDK 拼写 `Geometry::setTexcoords3()`，后端 `detail::texCoordArray()` 按 `components` 建阵列**并在阵列上陈述 `properties.format`**（vsg 的 `Array::assign` 只设 stride，format 留 UNDEFINED，pipeline 顶点格式默认取 binding 声明 —— 一个 ShaderSet 服务两种形状时必须由阵列陈述）。变体身份 `layout` 加一位 cube；采样器种类由槽形状决定，只对**引擎自己的 set** 生效（用户 program 可能拿 UV 通道自己算方向 —— 第一版无差别应用时被现有 `cube map` 相当场抓住）。判据：证据基线 51 → 53 行（其它数字不变）、`vine_shader_check` 变体矩阵 4 个 define、test_vsg 252、test_graphics 247、lavapipe PASS。

> 2026-09-13 **P0.S1：GLSL 块名对齐 L1**：`MaterialBlock`→`VineMaterialBlock`、`LightsBlock`→`VineLightsBlock`（gbuffer_geometry.frag / std_forward.frag）；`ShaderAbiTest` +1 钉"契约名 == GLSL 块名"。行为中性；test_graphics 239 → **240**。

> 2026-09-13 **P0.C2：push 标注为 L1 的实现**：vsg `buildVineShaderSet` 的 push `pc` 在代码注释与头文档里写明
> `pc.projection ≡ VineViewBlock.proj`、`pc.modelView ≡ VineViewBlock.view * VineDrawBlock.model`（L2 实现，不是契约本身：`VineViewBlock` 288B > 128B push）。
> `ForwardShaderSetTest` +1 钉住 shader 文本（`PushConstants` / `projection` / `modelView` / `} pc;`）与 `sizeof(VineViewBlock) > 128`。
> 口径：行为中性；test_vsg 238 → **239**，两条证据基线不变。

> 2026-09-13 **P0.C1：L1 数据块布局（SDK）+ 命名定案**：L1 块统一命名 **`Vine<Role>Block`**（与既有 `VineLightsBlock` 一致；
> `Block` 明示内存布局、与 `FrameContext`/`RenderCommand` 区分），草稿的 `VineFrame` 改成 **`VineViewBlock`**（per-view 语义，避开 `FrameContext`）。
> `ShaderAbi.hpp` 新增 `VineViewBlock`(288B) / `VineDrawBlock`(80B)（16B 对齐、全 mat4/vec4 ⇒ std140 与 D3D cbuffer 同布局）+ `static_assert`；
> `tests/test_graphics/ShaderAbiTest.cpp` 钉 sizeof/offsetof。口径：行为中性；test_graphics 236 → **239**。下一步 C2（vsg 标注 push ≡ 子集）。

> 2026-09-13 **P0.B1：SDK 显式属性 location 表**：新增 `sdk/vine/graphics/ShaderAbi.hpp`（`VertexAttribute{Position,Normal,Color,TexCoord0}`
> + `attributeLocation()`，值 **0/1/2/8**）；vsg 的 `buildVineShaderSet` / `assembleProgramShaderSet` 用它替代字面量。
> 契约与 DX 映射写在 `.ai/design/graphics-shader.md` §11（L1/L2/L3 + B1..B4 分期）。
> 口径：两条证据基线 47 行不变；test_graphics 235→**236**（+1：表值 ↔ shader 文本声明的 location 一致）；lavapipe PASS。
> 下一步 B2：`ShaderProgram` 参数表 + 命名槽声明。**口径决策见 `graphics-shader.md` §12**：B3 采用选项 C
> （L1 声明式块 + vsg push 作内部优化；C1 SDK 块布局 / C2 标注等价），L2 shim 等第二个后端，**B2 暂缓**（无消费者前不加 `addParam`/`addInputSlot`）。

> 2026-09-13 **P0.A：内建前向着色归 SDK（行为中性）**：`std_forward.{vert,frag}` 从 `gfx_backend_vsg/shaders/` 搬到
> `src/viz/graphics/shaders/`，清单随之移动（嵌入数 graphics 3→5、vsg 4→2）。新增 SDK `BuiltinShaders.hpp/.cpp`：
> `builtinProgram(ShaderPreset)`（**已被 `forwardProgram()` / `flatForwardProgram()` 取代**）+ `gbufferGeometryProgram`/`deferredLightProgram`
> （从 `RenderPipelineBuilder` 搬来，builder 的两个静态工厂改转发，公开 API 不变）。
> 后端 `VsgPipelineFactory::compiledStages(preset)` 取 SDK program 编译（每 preset 缓存一次），`buildVineShaderSet` 不再自带 GLSL。
> 口径：两条证据基线 47 行逐字节不变；`vine_shader_check` PASS（7 shader）；test_graphics 234→**235**、test_vsg 237→**238**；lavapipe PASS。
> 边界：SDK 拥有**着色文本**（L3），后端拥有**编译 + ABI + 管线**（L2）；`ShaderSet` 仍只属 vsg 后端。下一步 P0.B（ABI 契约移入 SDK）。

> 2026-09-13 **P0.3 默认转正：自写前向着色成为 shipped 默认**：`vineForwardShaderEnabled()` 改返回 `getenv("VINE_VSG_BUILTIN") == nullptr`
> ⇒ 默认走自写 set，`VINE_VSG_BUILTIN=1` 退回内建（无 Vine stages 的 preset 仍自动回退）。自检 variant 探针的
> `'built-in Phong + …'` 改名 `'default shading + …'`（它跑的是内容 set，不是固定路径）。
> **两条证据基线语义对调 + 重命名**：`scripts/vsg_selftest_evidence.txt` = 默认（自写 set，centre 34,6,2）；
> `scripts/vsg_selftest_builtin_evidence.txt` = 内建退回（centre 46,8,3）。`vsg_selftest_evidence.sh [--builtin]`；
> lavapipe 3c 跑默认、3d 跑内建并把两条基线都比一遍。两基线差异仍只有 **6 个着色数字**（覆盖/深度/清屏/诊断计数全同）。
> 口径：build 0 error 0 warning；test_vsg **235**、test_graphics **234**；两条证据基线 PASS；lavapipe 整体 PASS（test_cppstd/test_runtime/test_system 为环境相关旧红，与本次无关）。
> 收尾（同日）：vsg `Light`/VDS 的 content 用法已去掉（forward 时槽不建灯节点、view `features=0`、不跑每帧 `setGroupLights`）；
> `SceneBridge::buildStateGroup` 新增 `derived` 参数，在几何无作者色（且无 UV、材质无纹理）时**不 assign** vine_Color/vine_TexCoord0
> （define 关、少两条顶点绑定 + 一次采样；单独丢 texcoord 会把 vine_Color 绑定号前移，故只在颜色也丢时一起丢），
> 属性在位与否并入 L2 variant 的 layout 哈希。两条证据基线 47 行不变；test_vsg **235 → 237**；lavapipe PASS。仍未做：opacity（P10）。

> 2026-09-13 **P0.2：自写前向着色接线（`VINE_VSG_FORWARD=1`）**：`makeContentShaderSet` 做唯一入口（窗口三档深度 + 每个离屏目标都走它），
> 槽级 `ContentSlot::lights_data`（112B）+ `SceneBridge::setLightsData` 注入 + 每帧 `fillVineLightsBlock`+`dirty()`，`buildStateGroup` 里
> “有块 **且** set 声明了 `vine_lights`”才挂描述符 ⇒ 内建/自定义 program 路径零影响（默认 47 行基线逐字节不变）。
> **两个证据基线**：`vsg_selftest_evidence.sh [--forward]`；两者差异只有 **6 个着色数字**（46,8,3→34,6,2 等），覆盖数/深度/清屏/诊断计数全同
> ⇒ “同一份几何、换了一套着色”。lavapipe 加 3d/4 阶段跑 forward 自检（0 VUID + 证据比自己的基线；帧数由证据脚本统一，否则 15 帧跑 vs 30 帧基线假红）。
> mutation 两条：跳过每帧光块 ⇒ forward 基线红；开关默认改 true ⇒ 内建基线红。test_vsg 233 → **235**。

> 2026-09-13 **P0 第一步：自写前向 shader + ShaderSet（`std_forward.*` + `buildVineShaderSet`）**：§4 的 ABI 草案在实现时撞上硬约束 ——
> Vulkan 只保证 **128 字节 push**，而 vsg 的矩阵栈已占满 0..128（全屏延迟路径能把 112 B 光块塞 push，正因它不需要矩阵）⇒ 前向的光必须走 **UBO**（set0/binding2，
> `VineLightsBlock`）。因此每 drawable 的唯一数据仍是 vsg 自动推的 `modelView`，§4.4 的 dynamic UBO 不是前置条件。另两条查证：`assignArray` 的绑定号是
> `base + arrays.size()`（按成功顺序，未声明就跳过并前移 ⇒ 声明顺序必须与数据节点的绑定顺序一致），而带 `define` 的绑定被赋值时会自动打开那个 define（= “喂了数据就开变体”）。
> 实现：两个 stage（`VINE_VERTEX_COLOR` / `VINE_DIFFUSE_MAP` 门控）+ `makeScenePipelineStates`（与内建 set 逐项相同）+ `VineLightsBlock`/`fillVineLightsBlock`（与延迟路径共用
> `collectViewSpaceLights` 一份实现）；**默认路径未接线**（证据 47 行不变）。门禁：`ForwardShaderSetTest` 6 条（含 stage 门控一致性）+ `OverlayLightingTest` +3 条；
> 四条 mutation（改 define 名 / 挪 `vine_lights` 绑定 / 写死 depthWrite / 去掉默认环境光）各自咬住目标测试。test_vsg 220 → **233**。设计与后续（P0.2 接线）见 §11。

> 2026-09-13 **着色器文件化 + 构建期嵌入（P12）**：产品 shader 从 C++ 字符串搬进真文件，构建期嵌进二进制；死文件
> `flat.*`（含两个提交进仓库的 `.spv`）删除。清单在**顶层** `cmake/VineShaders.cmake`（生成规则必须在顶层：`tests/test_vsg`
> 直接编译插件源码，要能依赖同一个生成头文件）→ 机制 `cmake/VineShaderHelper.cmake`（`vn_declare_embedded_shaders` /
> `vn_use_embedded_shaders`）→ 生成器 `cmake/VineEmbedShaders.cmake`（`cmake -P`，写 `inline constexpr std::u8string_view`
> + `Entry{name,hash,bytes}` 表）。为什么不用 `file(READ)` + `CMAKE_CONFIGURE_DEPENDS`：那样每次改 shader 都整包
> reconfigure（实测 ~15s），而 `-P` + `add_custom_command` 拿的是 ninja 原生依赖追踪（改 `.glsl` 只重编依赖它的 TU，
> 改生成器本身也会重新生成），且内容没变就不重写头文件（否则 touch 一下 `.glsl` 引发一串重编）。
> 类型口径：`ShaderStage::source` 是 `vn::String`（内部 `std::u8string`）⇒ `String(kX)`；vsg 侧要 `std::string`
> ⇒ `asShaderSource(kX)`（`vine/vsg/VsgUtils.hpp`，GLSL 是 ASCII 的逐字节视图）。
> **新门禁** `scripts/vine_shader_check.sh`：每个 shader × 4 种 define 变体（`VINE_VERTEX_COLOR` / `VINE_DIFFUSE_MAP`）
> 过 glslangValidator + 嵌入副本的 SHA-256 前缀与字节数必须与磁盘一致 + 每个 `*/shaders/*` 文件必须在清单里。
> 生成器两条**构建期**守卫（都做过 mutation）：源里出现 CR ⇒ 报错（`file(READ)` 会**静默**把 CRLF 归一化成 LF，所以要
> 用 `HEX` 读原始字节判）；源里出现 `)VINE_GLSL"` ⇒ 报错（否则 raw string 提前结束）。环境事实：本机 CMake 4.2.3 的
> `string(SHA256 …)` 恒返回空串 ⇒ 必须用 `file(SHA256 <path>)`；`glslangValidator -V` 不带 `-o` 会把 `vert.spv`/`frag.spv`
> 写进**当前目录**（门禁曾自己污染工作区，已加 `-o <tmp>`）。
> 判据：迁移**行为中性** —— selftest 证据 47 行逐字节相同、lavapipe 0 VUID、test_graphics 230 → **234**、test_vsg 220 → **224**、
> 诊断格式 0 suspicious；四条 mutation（非法 GLSL / 只改不重建 / 未入清单 / hash 截断）各自恰好目标门禁红。
> 设计与"加一个 shader 的步骤"见 `.ai/design/vsg-custom-shader.md` §10。

> 2026-09-12 **§14 步 1 已实施（第四十七批）：`ImageRef` + 接线期两条校验**：D37/D55/D56 的共同**结构根因** =
> "pass 之间用字符串当端点身份，且把三件不同的事（接线=静态 / '本帧产出了吗'=动态 / 谁是生产者=身份）塞进同一张
> **每帧重建**的 map"。`graphics-render-pipeline.md` §14（并在 §5 加"已修订"指针）给出方案：端点 = 对象
> `ImageRef`（**target + attachment 索引 + `Kind::Color/Depth`**，**强持** target ⇒ 链自描述），生产者写、宿主可
> 手动 `bind`，名字**降级为标签 + 语法糖**（`name→port` 只在**接线期**解析一次）。**接线期**校验四条结构规则
> （一个 port 至多一个生产者 = D55；消费者的 port 必须有生产者 = D37；`ScreenPass` 输入数不得为 0 = D56；生产者
> order < 消费者 order = 顺序首次**可判**），运行期只留"本帧未产出"一条（分集上报；**绝不递上一帧的图** = D38
> 教训）。后端**零改动**（插件/门禁/selftest 不动）。
>
> **步 1 落地（本次）**：`ImageRef.hpp/.cpp`（`label`/`kind`/`bind`/`unbind`/`target`/`attachment`/`bound`）+
> `RenderPass::setOutput/output/addInput/inputs/clearInputs`（与名字 API **并列**，互不清空）+ 新增
> `RenderEngine::validateWiring()`（`frame()` 首部、**只读**、无设备）。**本批先做两条**规则：① 一张图被两个
> **不同** pass 声明为产出（D55 的结构孪生；与名字版 D55 检测**互不重复报**）；② `ScreenPass` 无图像也无名字输入
> （D56 的解）。**当场推迟、次日补上**的另两条见下（"消费者的图无生产者"与"order 倒置"）——当时担心与 D37 的零周
> 报告重复上报，实测不会（那条只看名字、这条只看声明的图像，互不重叠）。上报沿用既有语义：**分集**（一次）+ 每帧
> 把集合重建为"本帧仍在的问题" ⇒ **重新武装**。
> **糖的回收规则（拍定）**：`name→ImageRef` 表条目在**其生产/消费者全部消失**时丢弃、需要时重建（"条目活着的
> 唯一理由是有人引用它"，与仓库既有的"表的条目必须自持其键"同源）。**判据**：`test_graphics` 160 → **166**（`ImageRef`
> 2 + 图像 API 1 + 两条校验 3）、`test_vsg` 162；全量 `ninja` 零 error/零 warning；`[selftest]` **45 行**、
> `released 115` 与上一轮相同（新校验只读，真实 app **零触发**）；门禁 `RESULT: PASS`；诊断格式 0 suspicious。
>
> **命名修订（同日）**：初稿 `RenderPort` → **`ImageRef`**。理由：`port` 说**角色**，对象装**身份**（哪张图），
> 而引擎规则全是关于身份的（"一张图至多一个生产者"）；角色由用它的位置表达（`output()` 写侧 / `inputs()` 读侧）。
> **问到"port 就是附件吧，可能是纹理吗"的结论**：一张图两种角色——写时是**附件**（`ATTACHMENT_OPTIMAL`）、
> 采时是**纹理**（`SHADER_READ_ONLY`），后端为此有整套布局切换（深度提升/借用/撤销），所以身份必须与角色无关。
> **不覆盖**的只有一类：**不是任何 pass 产物的图**（材质贴图、导入图、cube map）——SDK 里它们**没有身份**
> （材质贴图 = `Material::textureFile()` 一个路径）。
> **已知洞已修（第四十九批）**：冲突检测的键从 `ImageRef` 对象身份改成**地址**（`RenderEngine::OutputIdentity` =
> 已绑定时 `(target, attachment)`、未绑定时"声明的对象"）⇒ "两个 `ImageRef` 指向同一张图的同一附件"也报（两种冲突用
> **各自的消息分支**，符合同一批的"一条分支一个格式串"约定）。**不能只按 target**：同 target 多 pass 是**合法形态**
> （MRT 一次交多张图；`RenderPipelineBuilder` 的 `light` 与 `transparent` 都写 `composite`）。**两条反证已跑**：
> ①退回对象身份 ⇒ `TwoImageRefsOfOneAttachmentAreReportedOnce` 红；②退成"只看 target" ⇒
> `TwoImagesOfOneTargetWithDifferentAttachmentsAreNotACollision` 红。
>
> **§14.4 推迟的两条也补上了（第五十批）**：`validateWiring()` 分**三阶段**（消费者可能先注册 ⇒ 先收齐生产者再判
> 消费者）：①产出冲突（只看**启用**的 pass：禁用者不会"后跑覆盖"）；②**声明的输入图没人产出 / 生产者注册在消费者
> 之后**（只看**声明**，禁用也算"声明了这根线"——开关 pass 不是接线错误）；③`ScreenPass` 完全没输入。②的两条分支
> 各用自己的格式串，报告键为 `(消费者, 图身份)`，集合同样每帧重建 ⇒ 修好再断会重新武装。**有意不算错的两种形态**：
> 同 target **不同附件**（MRT；`light`+`transparent` 共写 `composite`）与一个 pass **读自己写的图**（反馈环，后端有意
> 支持）——各有单测钉住（`PassReadingItsOwnOutputImageIsNotReported`）。判据：`test_graphics` 169 → **172**
> （+3：无生产者报一次+重新武装 / 生产者排在后面报一次+重新武装 / 反馈环不报），其余同前（证据 45 行逐字节不变、
> 门禁 PASS、格式检查 0）。**真 app 零触发**（`RenderPipelineBuilder` 只声明名字，不用 `addInput(image)`）。
> **两层声明（2026-09-12，第五十二批，仍不动执行路径）**：`RenderPass` 加 `setOutputTarget`/`outputTarget`、
> `addInputTarget`/`inputTargets`（`clearInputs` 清两层）；**细（`ImageRef` = 一张图）与粗（`RenderTarget` = 整捆）
> 在同一**地址**身份上汇合** ⇒ 粗的承诺能被细的读满足，两个各自建的 `ImageRef` 也认成同一张图；`OutputIdentity` 带
> **kind**（**深度与颜色附件 0 是两张图**）。依据是后端事实：PiP 读一张图，全屏 program 读**整捆**（`makeFullscreenProgramNode`
> 收到 `src.color_views` 全部）。判定：**生产者 = 写这张 target 的 pass（顺序最早）或声明了这张图的 pass**（承诺不是
> 满足读的必要条件，它只是"谁拥有这次交接"的声明）；冲突**逐图**报。取舍：某 target 的**唯一写者**承诺整捆（一行、
> 对附件数免疫）；多 pass **累加**写同一 target 时**不承诺**。**迁移**：`RenderPipelineBuilder`（gbuffer 承诺整捆 /
> light、present 读整捆 / transparent 作为 composite 最后写者承诺 / `addOffscreenToScreen` 两侧）+ `AppShellUi` 四个
> demo（G-buffer 预览改**细颗粒**：`ImageRef` 绑 (target,k) 后 `addInput`；multislot 累加 ⇒ 不承诺）。判据：门禁跑真 app
> （含 `VINE_VSG_DEFERRED=1` + `VINE_VSG_OFFSCREEN_MULTISLOT=1`）⇒ **新校验零触发**、0 VUID ⇒ 真产线声明与新模型一致；
> `test_graphics` 174 → **179**、`test_vsg` 162、证据 45 行逐字节相同、格式检查 0。
> **未决**：①是否暴露"按名字取 `ImageRef`"的查询 API；②可寻址粒度何时扩到 mip / array / cube 层。

> 2026-09-12 **§57 跟做：D55 已修（第四十六批）**：`RenderEngine` 的命名输出表是**按名字平铺的 map**，第二个生产者
> 直接覆盖第一个——消费者静默拿到"记录顺序上靠后"的那个，而两个 pass 各自都合法，除了引擎没人看得见这个冲突。现在
> `publish` 检测"同名 + **不同对象**"：`duplicate_outputs_seen_this_frame_`（每帧重建）+ `duplicate_outputs_reported_`
> （帧末只保留仍在冲突的名字 ⇒ 冲突消失即重新武装），消息带输出名与两个目标名。**同一对象**同名两次是合法的
> （`AppShellUi` 就这么用）⇒ 不报。**判据**：`test_graphics` 新增 2 例 ⇒ 158 → **160**。**如实记录**：新上报在
> **默认 demo 与 `VINE_VSG_GBUFFER=1` 下都未触发**（实测 0 次）——三种 demo 互斥，所以这是**潜在**洞（单测钉住），
> 不是当前会冒烟的行为。

> 2026-09-12 **§57 跟做：D54 已修（第四十五批）**："部分灯不可用"以前**完全静默**——`beginLightsDroppedEpisode`
> 只在 `attached == 0` 时报，而典型场景恰恰是"一个场景带了 2 盏可用灯 + 1 盏后端翻译不了的灯"。改法：判定
> 改 `attached < announced`（空公告不算一段、全亮即重新武装），消息按"全丢 / 部分丢"**两条分支各用自己的格式串**
> （§54 教训）；规则从 `VsgContentSlot.cpp` 的匿名命名空间提进 `detail`（声明+文档 → `VsgContentSlot.hpp`），
> `light_fallback_reported` 的字段注释同步。**新测 4 例全设备无关**（`LightGroupTest.cpp`）⇒ `test_vsg` 158 → **162**；
> `[selftest]` 45 行逐字节不变、无新警告（自检相位没有丢灯场景）。

> 2026-09-12 **§58 跟做：D60 已修（第四十四批）**：回读的 `false` 现在**分类**了：prologue 返回 `ReadbackRefusal`
> （`NoTarget` / `NoSession` / `NotRendered` / `NotBuilt` / `Empty` / `NoDevice`），两个入口在**停下设备之前**上报
> （`readbackRefusalMessage` 每分支各用自己的格式串 —— §54 教训）；会话判定收紧为 `state.initialized`（它就是
> "窗口/设备存在"的权威状态，且**不需要设备就能测**）。**判据**：`ReadbackTest.cpp` 4 → **6 例**全设备无关
> （逐状态分类 + 两条入口的静默路径都上报 + 拒绝**不触发设备等待** + 每条消息各带自己措辞且不含别分支措辞）⇒
> `test_vsg` 156 → **158**。自检 `[selftest]` 45 行**逐字节不变**（自检回读都打到已建目标，新分类不触发）；
> stderr 那三条回读报告是**既有**的（RGBA16F / 打包 D24 / 附件越界）。

> 2026-09-12 **§58 跟做：D59 已修（第四十三批）**：`releasePass()` / `retargetPass()` 现在会把该 pass 的 `PassObjects`
> 一起丢掉：`erasePassSlotsFromTarget` → **`erasePassFromTarget`**（名字兑现新职责），GPU 对象 `retireRing.park`
> （**不停设备**，与 load-op 重建同一策略），表项删除后由调用者的 `reconcileOffscreenOrder()` 把图从录制序列摘掉。
> **判据全设备无关**（表 / 键 / 停放 / 环都不需要设备）：新增 `tests/test_vsg/PassObjectReleaseTest.cpp` 5 例 ——
> 释放后表为空 + 停放 1 个 + `waits == 0`；32 次"建一个再释放"表**不增长**；`retargetPass` 只从离开的 target 删；
> null / 未知 pass no-op；环 `kRetireRingDepth` 次 `advance()` 后交出 ⇒ `test_vsg` 151 → **156**。真 GPU 对象的停放
> 安全性由门禁（VUID 0）覆盖。**证据行的变化（有意，已 A/B 归因）**：`policy churn:` 从 `released 107` → **115**；
> 临时开关跳过停放可复现 107，且增量是**固定 +8**（30 帧与 44 帧下都恰好 +8）⇒ 它是**窗口之前**那批
> `releasePass` 的停放（`released` 是环的*释放*计数，滞后停放 `kRetireRingDepth` 帧），**不是**窗口内逐帧
> 删除/重建（那会随帧数增长）；`0 device wait(s)` / `built 2` 不变。

> 2026-09-12 **§58 第五轮审查：`RenderBackend.hpp` 的承诺逐条对账（D59–D60，第四十二批）**：§57 修掉的 D52 只是
> "文档承诺"类的一个实例，这轮把类级契约（CALL ORDER / BORROWED ARGUMENTS / WHAT MAY BE RETAINED / THREADING /
> FAILURE）与各方法文档**逐条**对实现。**兑现的**：帧序（`endFrame` 不 present）、借用参数自持（D13/D14/D32/
> D34/D42 各补过）、线程与同步 sink、`PassProtocolViolation` 两处、`grep throw` = 0、MRT 透明黑规则、`endPass`
> 清请求。**新发现两条**：**D59（最重）`releasePass()` 不释放该 pass 的 `PassObjects`——`Target::passes` 的
> 唯一移除点是目标重建的 `t.passes.clear()`（`VsgTargetBookkeeping.cpp:79`），`erasePassSlotsFromTarget` 只删**槽**。
> 同时违反两条契约：①留存状态"不得随帧数增长"（编辑器每帧 addPass + removePass，`RenderEngine::removePass`
> 确实调 `releasePass`，就按帧累积 RenderPass+Framebuffer+RenderGraph 到会话结束）；②键是裸 `RenderPass*` 且
> `PassObjects` **不自持**它——D32/D34 给 target/material/geometry/program 都补过这条所有权规则，**pass 漏了**。
> **不是错画**：`applyRecordPlan` 每帧 `children.clear()` 重建录制序（`VsgRecordOrder.cpp:164-187`），陈旧图不会
> 继续被录制 ⇒ 是内存 + 契约缺陷。**为什么不当场修**：释放的是真 GPU 对象（render pass / framebuffer），
> "计数等待 vs 停放进退役环"本身就是一次要带判据的改动（§28 第 9/10 批教训），而"没有判据不许改语义"是硬规矩
> ⇒ 登记 + 附修法与判据计划。**D60** readback 的 `false` 无法区分"不支持/还没建好/失败"——契约明写 caller 总能
> 区分，而 D20 已经把"float 附件不支持"报出来了 ⇒ 报告策略不一致，只看返回值的宿主无法决定"重试还是放弃"。

> 2026-09-12 **§57 第四轮审查：后端 ↔ graphics 的协同（D52–D58，第四十一批）**：两条线并行审 ——①**边界静默
> 失败**（每个 caller 可见的"什么都没发生"是否带诊断）；②**前端调用序列 ↔ 后端消费规则**（`setViewport` /
> `setLights` 一次性消费、作用域属性到 `endPass`、screen draw 写当前 target）。**先记好消息**：前端现有**全部**
> 调用点都遵守一次性消费（每个公告后恰好一次绘制）、`beginPass/endPass` 全成对、`ScreenPass` 两条 draw 前都设
> target ⇒ 契约脆弱处不在现有调用点，而在"契约允许一个作用域画多次、viewport/lights 却只能消费一次"（自定义
> `RenderPass` 覆写画第二次就静默丢 viewport/灯，登记不修）。**已修 2 条**：**D52** `initialize()` 的 null-window
> 失败绕过诊断通道（只 `VN_LOGE`，而契约写的是"false 也要在诊断通道报原因"）⇒ 补 `InitFailed` 上报；**D53**
> `addDeferredDemo` 的 `ScreenPass` 没绑内容场景 ⇒ `setLights` 不执行 ⇒ 延迟预览只有 0.15 平坦环境光、零方向光
> （而该函数文档写着"灯是内容场景自己的"；builder 的同一 pass 绑了 `content_`）⇒ `addPass(light, scene, 130)`。
> **已登记 5 条**：D54 部分灯不可用静默丢（只有全不可用才报，`attached<announced` 可判）· D55 `outputs_` 同名
> 输出静默后者覆盖（同对象重复发布合法，不能误报）· D56 `ScreenPass` 未声明 input 时静默不画（引擎只覆盖
> "声明的 input 全落空"）· D57 overlay/PiP 的源 0×0 与窗口图未建静默不画 · D58 `resize(w,h)` 忽略尺寸参数。
> **验收**：全量 ninja 零警告 + 证据 45 行逐字节相同 + VUID 0/FAIL 0 + 151/158 + 门禁 PASS + 格式检查 0；
> **针对性验证**：门禁环境加 `VINE_VSG_DEFERRED=1` 复跑（延迟光照 pass 平时不在门禁里）⇒ 同样 PASS、0 VUID。

> 2026-09-12 **§56 通道物化（components → typed array）也变成可测契约（第四十批）**：`SceneBridgeGeometry.cpp`
> 匿名命名空间最后剩下的 `makeTypedVertexData` 是 §54 `channelShape` `@pre` 的**兑现者**：假定"分量 1..4、每顶点恰好
> 一个值"，按 `components` 当 stride 取分量（1/2/3/4 → `floatArray`/`vec2Array`/`vec3Array`/`vec4Array`）。
> **判词**：错了**不会失败** —— 分量数选错数组类型时 configurator 照样接受，只在绘制时"属性读错/缺失"；stride
> 取错就是 D1 的交错读错换位复发；它还是唯一把**通道数据**与 §53 的 `formatForComponents`/`sampleVertexData`
> 对齐的环节。提进 `detail`（声明+文档 → `VsgSceneRules.hpp`，定义 → `VsgSceneRules.cpp`）；**副产物**：几何 TU 的
> 匿名命名空间因此清空 ⇒ 删掉 `namespace { }` 整块（该 TU 现在只剩 `SceneBridge` 的方法）。**新测 3 例全设备无关**
> （数组类型按分量数 + 与 `sampleVertexData` 的 `className()` 逐一一致 + 按 stride 逐分量拷贝/vec4 的 w 是数据/
> 0 顶点是空数组）⇒ `test_vsg` 148 → **151**。**踩坑**：返回 `ref_ptr<::vsg::Data>` ⇒ 断言 size 要 `valueCount()`
> （`Data` 没有 `size()`）。**验收**：全量 ninja 零警告 + 证据 45 行逐字节相同 + VUID 0/FAIL 0 + 151/158 +
> 门禁 PASS + 格式检查 0（22 文件）。**下一步候选**：`SceneBridgeGeometry.cpp` 的文件局部 helper 已全部归位；
> 几何侧的下一步该转向真正需要设备的那部分（或用户指定的新方向）。

> 2026-09-12 **§55 推导法线（零覆盖的那条路）也变成可测契约（第三十九批）**：先量覆盖 —— 自检里**每一处**几何
> 都 `setNormals(...)`（7 处）⇒ "网格没给法线 ⇒ CPU 推导"这条分支**从未被执行过**，而它的失败模式最安静：退化
> 三角形（共线/重复顶点）叉积为零，直接归一化就是**除以零 ⇒ NaN 写进顶点法线**（无验证层报 NaN 属性），绕序反了
> 则是"从内部被照亮"。这两条规则在 `makeNormals` 与 `makeIndexedNormals` 里**各写一遍**（同一叉积 + 同一
> `len_sq > 0` 守卫）⇒ 同一语义两份实现，该统一。做法：`faceNormal` / `normalIsUsable` 两个规则 + 三个构造器
> （`makeWhiteColors` / `makeNormals` / `makeIndexedNormals`）从 `SceneBridgeGeometry.cpp` 匿名命名空间提进
> `detail`（声明+文档 → `VsgSceneRules.hpp`，只多一个 `<vsg/core/Array.h>`；定义 → `VsgSceneRules.cpp`）。
> **两条归一化算术有意保持不同**（倒数乘 vs `::vsg::normalize` 除法，合并会改末位比特；§39 已因同一理由拒绝过）
> —— 本次只把判定与叉积各收一处。**新测 9 例全设备无关**（绕序/翻转/退化精确为零 · 只拒零长度 · 非索引单位化 ·
> 退化保持零且 `!isnan` · 给定法线逐字拷贝 · 索引累加/越界跳过/无引用顶点保持零 · 白色 0 顶点为空数组）⇒
> `test_vsg` 139 → **148**。**验收**：全量 ninja 零警告 + 证据 45 行逐字节相同 + VUID 0/FAIL 0 + 148/158 +
> 门禁 PASS + 格式检查 0（22 文件）。**下一步候选**：`SceneBridgeGeometry.cpp` 剩下的 `makeTypedVertexData`
> （channelShape 的 `@pre` 兑现者：components → typed array，零直接测试）可按同一判词抽；`sdk/` 用户说先不动。

> 2026-09-12 **§54 几何侧 xyz 解包规则也变成可测契约（第三十八批）**：`SceneBridgeGeometry.cpp` 匿名命名
> 空间剩的三件（`XyzUnpack` 三判定 / `unpackXyz` 按 `components` 当 stride 取前三个标量 / 
> `ignoredNormalChannelMessage` 两条分支各带自己的数字）提进 `detail`：声明 + 文档 → `VsgSceneRules.hpp`
> （只多一个 `<vine/geometry/Array.hpp>`），定义 → `VsgSceneRules.cpp`，该 TU 三行显式 `using`。**判词**：
> 决定几何是否正确解包却只在整帧路径里执行过，错了**不会失败** —— stride 取错（写死每顶点 3 float）会把顶点
> **交错读错**（D1 的缺陷，画面只是"看着不对"，无法定位）；诊断消息曾把两条分支合成一个格式串、参数按其中一
> 条排 ⇒ 打印**互换的数字**（消息撒谎，编译/运行/验证全干净）。**新测 5 例全设备无关**：vec3/vec4 按 stride
> 取 xyz 且 vec4 的 w 被跳过 + 合法空通道 Ok + 1/2/0/5 分量 ⇒ `NotXyzStride` + 3 分量 4 float、4 分量 5
> float ⇒ `NotDivisible` + 被拒**不动**输出数组、成功**替换而非追加** + 两条消息各带自己分支的数字（互相
> 不含对方的措辞）⇒ `test_vsg` 134 → **139**。**验收**：全量 ninja 零警告 + 证据 45 行逐字节相同 +
> VUID 0 / FAIL 0 + 139 / 158 + 门禁 PASS + 格式检查 0（22 文件）。**下一步候选**：`SceneBridgeGeometry.cpp`
> 里仅剩的非纯规则（`makeNormals` / `makeIndexedNormals` 的退化法线处理、`makeWhiteColors`）需要 to-vsg
> 对象，是否值得抽按"错了会不会失败"逐个判；`sdk/` 先不动。

> 2026-09-12 **§53 顶点绑定/stage 映射也变成可测契约（第三十七批）**：`SceneBridgePipeline.cpp` 匿名命名空间
> 里剩的四个纯映射（`formatForComponents` / `sampleVertexData` / `customAttributeName` / `stageFlag`）提进
> `detail`：声明 + 文档 → `VsgSceneRules.hpp`，定义 → `VsgSceneRules.cpp`（一个概念的头 ↔ 一个概念的 TU），
> 该 TU 用四行显式 `using detail::xxx;` 接入（延续 §51b 面最小的做法）。**判词**：它们**决定绑定是否正确**却
> 只在整帧路径里被执行过，而且错了**多半不会失败** —— `customAttributeName` 的两个使用者
> （`addAttributeBinding` / `configurator.assignArray`）都调它，改名只改一处 ⇒ attribute **静默不绑定**
> （几何照画、少一个属性、零报告）；`formatForComponents` 与 `sampleVertexData` 必须**互相一致**（格式的每分量
> 4 字节 = 样例数组元素类型），不一致时 configurator 会接受，只在绘制时表现为属性错/缺失。头文件只加
> `<string>` + `<vsg/core/Data.h>`（后者同时给 `Data` 与 Vulkan 类型）。**新测 4 例全设备无关**（1–4 的格式
> 映射 + 0/>4 四分量兜底 + 样例数组**具体类型**/`valueCount()==1`/`valueSize()==components*4` + 命名稳定、
> 区分、不与 `vine_Vertex`/`vine_Normal`/`vine_Color`/`material` 撞名 + 三个 stage 各占一个**不同**的 Vulkan 位）
> ⇒ `test_vsg` 130 → **134**。**验收**：全量 ninja 零警告 + 证据 45 行逐字节相同 + VUID 0 / FAIL 0 +
> 134 / 158 + 门禁 PASS + 格式检查 0（22 文件）。**下一步候选**：几何侧同类纯规则（`SceneBridgeGeometry.cpp`
> 的 `unpackXyz` / `ignoredNormalChannelMessage`）；`sdk/` 仍空。

> 2026-09-12 **类里剩的七个概念（设计 §50，已落地 1–7））**：批次顺序 = **依赖方向**。**已做**：①诊断
> 2026-09-12 **§52 overlay 的两个纯 helper 也变成可测契约（第三十六批）**：`viewRotation`（look-at 基）与
> `fillLightPushBlock`（128 字节 push 常量块：投影参数 / world→view 光方向 / 三个方向光上限 / 无 ambient 补
> 0.15）从 `VsgOverlay.cpp` 的匿名命名空间提进 `detail`，声明+文档进 `VsgOverlay.hpp`，定义留 TU。**判词**：
> 它们决定画面却只在整帧里被执行过 —— 光方向错是「光照看着不对」，`projparms` 错是「深度重建位置偏」，退化
> 相机（eye==target、up 平行视线）错是 **NaN 进 push 常量**。**踩坑**：`LightPushBlock` 在 `detail` 里，第一版
> 前向声明写在 `vn::vsg` ⇒ 测试拿到的是那个**永远不完整**的外层声明（16 个 incomplete type）；**名字的真身
> 在哪个 namespace，声明就得写在哪**。新测 11 例（右手基 + 正交性 + 两个退化兜底 + 清零 + 透视参数 + 正交相机 +
> ambient 默认 + view space 光方向 + 禁用/null 不占槽 + 第四个方向光被丢弃）⇒ `test_vsg` 119 → **130**。

> 2026-09-12 **§51 两处"整体"收尾（第三十五批）**：①**一个泊车环**：`SceneBridge` 手写的
> `retire_ring_[kRetireRingDepth]` + `retire_head_` 与 `VsgRetireRing` 是同一件事的两份实现，而且
> `VsgRetireRing.hpp` 反过来 include `SceneBridge.hpp` 只为借常数 ⇒ **依赖反了**。做法：深度常数搬进
> `VsgRetireRing`，`SceneBridge` 改持 `VsgRetireRing retire_ring_`，`retireNode` / `advanceRetireRing`
> 变 1 行委派，删 `SceneBridge::kRetireRingDepth`（3 处用点改指 ring 的）。**判据**：同一语义的第二份
> 实现 + 反向依赖 = 该统一。②**设备无关规则出匿名命名空间 → `VsgSceneRules`(169/130)**：`channelShape` /
> `ignoredChannelMessage` / `hashCombine` / `vertexLayoutHash` / `hashStateVariant` /
> `colourAttachmentCount` / `applyOpaqueBlendForAttachments` —— 它们错了**不会失败**（通道形状错 ⇒ 坏
> 缓冲；哈希漏字段 ⇒ 只丢缓存共享、每帧重建、零报告）。两个 TU 用 `using detail::xxx;` **显式**接入
> （不用 `using namespace detail;`：桥 TU 拉整个 detail 风险大于收益，且原名字本就文件局部）。**新测 11 例
> 设备无关**（通道 4 判定 + 诊断文本 + 布局哈希对 location / components / **顺序**敏感 + 变体哈希对
> **每个**折叠字段敏感 + `hashCombine` 顺序 + 附件数（null / 无 blend 态 / 0 附件都 = 1）+ opaque 写是
> 替换不是追加）⇒ `test_vsg` 108 → **119**。**踩坑**：测试 CMake 的 `SRC_FILE_LIST` 用 **tab**（第三次踩，
> 终端把 tab 显示成 8 列）；单行 doc `/** @brief ... */` 的锚点算法要允许"命中行自己就以 `/**` 开头"。

> 2026-09-12 **类里剩的七个概念：批次 6 采样与摆放（第三十四批，设计 §50 收尾）**：`VsgOverlay`(119/744)
> = `VsgOverlayDestination`（原类私有嵌套 `OverlayDestination` 出到命名空间作用域 —— 和 `VsgContentSlotRequest`
> 同一条理由：helper 必须能命名它）+ `resolveOverlayDestination` / `placeOverlayView` / `installOverlayView`
> （**模板且只在本 TU 用 ⇒ 定义留在 TU，头里不声明**）+ `drawScreenTexture` / `drawScreenProgram`（公开覆写在
> 类上只留 3 行委派）；三个文件局部 helper（`makeCompiledOverlayView` / `viewRotation` / `fillLightPushBlock`）
> 留在匿名命名空间。**关键细节**：draw 里原先调类的私有 `takeRequestViewport()`（pass 请求状态机的一部分，
> **不该出类**），而它其实就是 `state.request.takeViewport()` ⇒ **状态机一行没动**。教训：搬不动的东西先问
> "它是类的行为，还是状态的取用？"。**踩坑**：①文档挂在 `template <class Slot>` 之上 ⇒ 向上找文档被 template
> 行挡住，文档没搬、类头留下孤立 doc（靠**备份**取回）；②断言写得太糙（`OverlayDestination` 是
> `resolveOverlayDestination` 的子串）⇒ 改"计数配平"；③**又一次吃掉最后一个函数的 `}`**（收尾替换
> `\n}\n\nVN_VSG_NS_END`）⇒ 护栏补一条：**收尾替换必须保证函数右括号与命名空间右括号都在**。
> **§50 收尾**：七个概念全部落地 ⇒ `VsgRenderer` 从 2096 行类体 + 四个上千行 TU 收敛到 **579 行类头** +
> **两个自己的 TU**（`VsgRenderer.cpp` 830 帧泵+委派 / `VsgRendererPasses.cpp` 150 pass 协议）。**必须留下**的
> 判定标准：`RenderBackend` 覆写、pass 请求状态机（§28 调用顺序契约）、帧泵、`state = VsgRendererState{};`
> 的整体重置。批次 6/7 连续两次被依赖拽动顺序 ⇒ **依赖方向 > 概念漂亮**已是被验证的结论。

> `VsgDiagnostics`（SDK 已有 sink+计数 ⇒ 概念只命名"报告往哪走"；踩坑：`reportDiagnostic` protected ⇒
> 需 `deliverToSdkChannel` 蹦床）；②录制顺序 `VsgRecordOrder`；③读回 `VsgReadback`（公开覆写留类上只做
> 3 行委派；收非 const 状态——服务读回要停设备且被计数）；④增量编译 `VsgViewCompiler`（编译队列留状态里，
> 它引用会话拥有的 view）；⑤内容槽 `VsgContentSlot`（**`VsgContentSlotRequest` 从类私有区解放** —— §37
> "传 8 个松散字段"的根因；连带 `passGraph`+`dropDepthSamplingProgramSlots` → 物料化、`placeViewByOrder`
> → 内容槽、`installDiagnosticRoute` → 诊断）。**顺序教训**：按**依赖**排批次，不按"概念漂亮"排：内容槽
> 表面只依赖状态，实际依赖 `passGraph`，后者又依赖 `dropDepthSamplingProgramSlots` ⇒ 改签名→编译→按报错
> 逐个归位，比事前猜准。**已做 ⑦ 目标装配 `VsgTargetBookkeeping`(275/560)**：`VsgRendererState.cpp`
> 整个消失 —— 它那 241 行里**没有一行是"状态自己的行为"**（七个方法全是目标装配）⇒ 状态只剩数据 +
> 一张表。装配（附件、借用深度、附件 USAGE 位一处决定）、判定（`borrowNeedsRebuild` 的 WAITING vs
> STALE、`dropConsumersSampling`）、注销（`unhookTargetPasses` / `detachSlotView` /
> `erasePassSlotsFromTarget` / `retargetPass`）、公开入口（`releaseRenderTarget` / `releaseWindowLayer`
> 只留 3 行委派）全在一处；只有 `resetTargetAttachments` 不收 `state`（它只清一个条目）—— **收不收
> `state` 由函数真的碰不碰会话决定**。**顺序教训（又一次）**：7 先于 6，因为 **6 依赖 7**（
> `resolveOverlayDestination` 里就调 `buildOffscreenTarget` / `retargetPass`）⇒ **批次顺序 = 依赖方向**。
> **脚本教训**：函数体必须包进 `namespace detail { ... }`（只加 `using namespace detail;` 会定义出一批
> **新**函数 ⇒ ambiguous / no member named in namespace detail）；**"内存里删过"不等于"文件里删过"**
> （后面 `read()` 又读了回来）；新 TU 拿不到 `VN_LOGI`（以前靠 `VsgRenderer.hpp` 传递包含）⇒ 显式 include，
> **"一个头一个 TU"继续逼出自足**；备份救过一次内存里丢掉的文档。**顺带清旧债**：批次 5 留下的僵尸声明
> `dropDepthSamplingProgramSlots`；类私有区里被遗忘的 `SlotKey` 身份说明（搬到 `struct SlotKey` 头上）。
> **⑥ 采样/摆放 `VsgOverlay`(119/744)** —— 见下一条。**⚠ 事故**：搬运脚本把 `\n` 写成字面量 ⇒> `VsgRenderer.cpp` 变 1 行；靠 VS Code 本地历史 + 去空白 token 级 diff **精确**恢复。**护栏**：`chr(10)`
> join / 写前备份 / 写后行数断言 / 精确文本改写要报未命中；长字符串别用 heredoc（会截断成语法错）。

> 2026-09-12 **类里剩的七个概念（设计 §50，已落地 1–3）**：用户问"`VsgRenderer` 还能不能拆概念"。实测
> 类 = 帧协议 + 七概念（诊断 / 录制顺序 / 读回 / 增量编译 / 内容槽 / 采样与摆放 / 目标装配），批次顺序 = 
> **依赖方向**（从没人依赖到所有人依赖）。**已做**：①诊断 `VsgDiagnostics`（**SDK 已有 sink+计数** ⇒ 概念
> 只该是"stderr 追踪 → 下游 channel"；`route()` 给模块接同一路；**踩坑**：`reportDiagnostic` 是 protected,
> lambda 闭包不是本类成员 ⇒ 需要 `deliverToSdkChannel` 蹦床）；②录制顺序 `VsgRecordOrder`（3 步 + 驱动器
> 出类，`RecordPlan` 从 VsgFramePlan 迁入）；③读回 `VsgReadback`（公开覆写留类上只做 **3 行委派**；函数收
> **非 const** 状态——服务读回要停设备且被计数；guard 契约变可设备无关测试）。**未做**：4 增量编译 → 
> 5 内容槽 → 6 采样/摆放 → 7 目标装配。**必须留类里**：`RenderBackend` 覆写集、pass 请求状态机、帧泵。
> 验收同前（`test_vsg` 108 = 100 + 4 诊断 + 4 读回）。**脚本教训**：同文件多切割段必须合并后一次写回；
> 精确文本改写必须**报告未命中项**（`hostVisibleMemory` 因对齐空格静默漏改）；测试 CMake 列表用 tab。

> 2026-09-12 **实现跟着头走（设计 §49）**：用户要求"`VsgRendererTargets.cpp` 也要拆，把对应的实现移动到对应的
> 头文件的 cpp 里"。**问题**：§48 只让概念有了头，实现仍留在 1426 行的 `VsgRendererTargets.cpp` 里与
> `VsgRenderer` 方法交错 ⇒ 半张地图。**做法（纯搬运）**：新建 `VsgRetireRing.cpp`(40) /
> `VsgPassMaterialiser.cpp`(234，8 个 detail 函数) / `VsgRendererState.cpp`(454，**全部** `VsgRendererState::`
> 方法，从 Targets **和** Passes 两个 TU 收拢)；剩 `VsgRendererTargets.cpp` 1426→**829**（目标账本/命令图序/
> 读回）、`VsgRendererPasses.cpp` 571→**535**（pass 协议 + 内容槽）。**规则**：一个概念的头 ↔ 一个概念的 TU
> （`VsgRenderTargetEntry.hpp`/`VsgFramePlan.hpp` 不需要 TU：前者只有内联访问器、后者纯数据）。**三处源列表**：
> 插件是 `file(GLOB_RECURSE)`（**无 CONFIGURE_DEPENDS**）⇒ 新增 TU 必须重新 configure；selftest + test_vsg
> 是显式列表，各加三行（test_vsg 用 **tab** 缩进）。**附带收益**：新 TU 第一个 include 是自己的头 ⇒ 头第一次被
> **单独**编译，立刻暴露两个被传递包含掩盖的缺口（状态头缺 `<vine/graphics/RenderPass.hpp>`、entry 头缺
> `RenderPass.hpp` + `<vsg/commands/PipelineBarrier.h>`）——"一个头一个 TU"迫使头自足。**脚本坑**：同一文件的
> 多个切割段各自"读原文→写回"会互相覆盖，必须按文件合并后一次写回。验收同前（扫 17 文件）。

> 2026-09-12 **概念出类（设计 §48）**：用户问"概念性的提出来取一个合适的名字，是否合适，不然 state 成了神仙类"。
> 判词：**改名只解决一半**——§47 只做了命名层，1253 行的 `VsgRendererState` 仍是四组职责 / 21 个方法，
> 而且**嵌套会助长它**（状态里没有"给某个概念写行为"的位置 ⇒ 每加一个行为就再加一个方法）。**先量耦合**
> （每个方法实测碰了哪些 state 级成员：泊车环只碰环 + viewer；pass 组只碰 window/targets/request/
> passes_active_this_frame；录制组只碰 targets + command_graph；回读组碰 window/viewer/targets）⇒ 真能拆。
> 三批：**A 命名层**（`Target`→`VsgRenderTargetEntry`、`PassRequest`→`VsgPassRequest`、
> `PassAttachments/PassPlan/RecordPlan`→`detail::*`；新头 VsgRenderTargetEntry.hpp 装 SlotKey+三种槽；
> 改名用**上下文正则**：`Target` 后跟 `& * :: , ) > ; {`/行尾才算类型引用，否则会改烂散文比如
> "Target whose depth attachment to read"；1253→674）；**B 泊车环**（`VsgRetireRing`=4 字段+`park`/
> `advance`/`waitForIdle(viewer)`，先做它因为对 target 零知识；诊断读 `retireRing.waits/.released`）；
> **C pass 物料化**（8 方法→`detail` 自由函数，新头 VsgPassMaterialiser.hpp；6 个收 `state`，`makePassGraph`/
> `publishPass` 纯函数；674→**438**，方法 21→13）。**顺序理由**：命名空间作用域且有名字的类型才有资格
> 当自由函数的第一参数（类私有区不行）——命名是前提不是装饰。**留下的 13 个方法**正好是目标表自己的操作
> （teardown/build/readback/录制排序/借用判定）。顺带清三处旧债：`*/    struct ContentSlot {` 合并行（HEAD
> 里就有）、`entryFor` 的文档被遗在上一函数上方、状态头 14 个随概念搬走的 include。验收同前（三批各自
> 全量门禁：证据 45 行逐字节相同 / VUID 0 / 100 / 158 / 门禁 PASS / 格式检查 0）。

> 2026-09-12 **状态搬出类体（设计 §47）**：用户问"为啥 `Impl` 还存在；不想要 PIMPL"。老实答：§44 去掉的是
> PImpl **机制**（指针/无实体类型/编译防火墙），`Persistent`/`Impl` 两个结构是**按值留着**的，其中一个还叫
> `Impl` ⇒ 名字仍在暗示一个已不存在的东西。**做法（抽出来，不拍平）**：新头
> `include/vine/vsg/VsgRendererState.hpp`（插件私有）装三件：`SlotKey`（搬到命名空间作用域：状态用它对槽表
> 做键）、`VsgRendererPersistent`（跨会话，材质管理器契约）、`VsgRendererState`（原 `Impl`：单窗口会话）。
> 类头 **2096 → 911 行**，只剩公开 API + 两个参数类型（`OverlayDestination`/`ContentSlotRequest`）+ 私有
> helper 声明 + 两个按值成员；288 处重命名（`Impl::`→`VsgRendererState::`、`impl.`→`state.`）全在 4 个渲染器
> TU。**为什么不拍平**：`shutdown()` 靠 `state = VsgRendererState{};` **一句整体替换**保证会话期资源不会被
> 手写拆卸清单漏掉；拍平=逐个重置 ~250 行成员 + 换成"记得改这里"。**抽出的前提**：命名空间作用域结构不能
> 命名类的私有嵌套类型 ⇒ 先实测状态体内 `ContentSlotRequest`/`OverlayDestination` 引用为 0，只有 `SlotKey`
> 要跟着搬。顺带把 §44 合并脚本留在类头 include 区的乱序/重复归位。验收同前（纯结构，行为零变化）。

> 2026-09-12 **头文件目录约定（设计 §46）**：**`include/` = 插件私有头（全部）**，`sdk/` = 将来"真要对外"
> 的头才放（像库那样），目前为空/不存在；`src/` 只留 `.cpp`。做法：`git mv` 五个头（`VsgPipelineFactory` /
> `VsgBackendUtility` / `VsgUtils` / `SceneBridgeInternals` / `GfxBackendVsgPlugin`）进 `include/vine/vsg/`，
> 26 处 include 改 `<vine/vsg/X.hpp>`，**清掉 `tests/test_vsg` 与 `vsg_backend_selftest` 里手工加的插件 `src/`
> 包含路径**（它们本来就是为了这些头）。**为什么这不等于发布私有头**：`vn_add_plugin` 的 PUBLIC 只是"本构建
> 可见"，**没有任何 install 规则安装插件头**，宿主只经 `RenderBackend` SDK 接口拿渲染器。验收同前。

> 2026-09-12 **去掉 `VsgRenderer` 的 PImpl（设计 §44）**：事实 —— `vine/vsg/VsgRenderer.hpp` 只被插件自己的
> 6 个 TU + selftest + `PassProtocolTest.cpp` include；插件是 MODULE DLL，宿主走 `RenderBackend` SDK 接口
> ⇒ 该头**不在任何部署边界上**，PImpl 的 ABI 理由落空，而"编译防火墙"本来就半破（公开头已 include
> `<vsg/app/Viewer.h>`）。代价是真的：需要状态类型的 helper 不能声明在公开头 ⇒ "`Impl` 成员 vs 渲染器私有
> 成员"那条规矩（§37/§36/§40b 都被它逼过）+ 204 处 `impl->` + §42 的实现理由写进"公开"头。**做法**：类定义
> 并入 `src/VsgRenderer.hpp`（公开头删除；§45 后为 `include/vine/vsg/VsgRenderer.hpp`，§47 后其中的状态在
> `VsgRendererState.hpp`），状态改**按值**成员，`Persistent`/`Impl` 的拆分**保留**（生命
> 周期语义：跨会话 vs 一个窗口会话），217 处 `impl->`→`impl.`、17 处 `persistent->`→`persistent.`、ctor
> `= default`、`impl = Impl{};`。**暴露的真问题**：`unique_ptr::operator->` 在 const 方法里也返回非 const
> 指针 ⇒ 去掉后 `detachedSlotCount() const` 等只读访问器立刻编译失败（它们原来在非 const 走槽表）；补 const
> `forEachSlot` 重载后才是真 const —— PImpl 一直在掩盖 const 正确性。**过程教训**：不要对"脚本刚生成、缩进
> 层级变过"的文件手写补丁（锚点按旧缩进会**静默吃掉相邻声明**，本次吃掉了 `visitSlot`，报 300 错）；正确做
> 法是先把预期增量打在合并前的干净源上，再跑合并脚本。

> 2026-09-12 **`render()` 只取一处（设计 §43）**：先判断 —— 115 行里值得抽的只有**一处**（两段深度借用判定），
> 其余（`ContentSlotRequest` 填充、`retargetPass` 前后）是机械搬运。①`Impl::borrowNeedsRebuild(t, target_key)
> const` 把两条互异的"借用失效"合成一个纯函数：**PENDING**（请求的借用还没兑现，源当时没有深度图像；永久不可用
> 的源记成 `unusable_depth_source` ⇒ 只在"仅仅在等"时重试，禁用/未构建的产出方每帧只花一次查表）+ **STALE**
> （已兑现的借用绑源的深度 *view*，源被重建会换图像 ⇒ 借用方继续测没人写的旧图像、**借来的深度静默冻结**；
> 用"源当前 view vs 烘入时记下的 view"检测）。②**删掉重复注释**：`render()` 里 20 行 scope 属性说明在
> `setPassOrder`/`setDepthMode`/`setRenderTarget` 各自的注释里**已有** ⇒ 压到 8 行（README 式一句 + 指向 setter，
> 保留 render() 特有的"每目标共用一条槽路径、附件在此确保"）。`render()` 115→**81**，TU 960→926。**踩坑（第二次
> 同族）**：`targets` 表的键是非 const 指针 ⇒ `wanted_source` 不能声明成 `const RenderTarget*`（`map::find` lose
> const qualifier），用普通指针（`depthSource() const` 本就返回普通指针）。验收同前（证据逐字节相同 / VUID 0 /
> FAIL 0 / test_vsg 100 / test_graphics 158 / 门禁 PASS）。

> 2026-09-12 **"单调用点 helper"审计 + 提交后一步合并（设计 §42b，承 §42）**：审计实测 —— `submitFrame`
> 拆出的五个方法**各 1 个调用点**（既有的 `retireInactivePassSlots` 也一样），本批无死代码；但分三类：
> ①`compilePendingViews()` **有真实复用潜力**（它碰的队列被 `renderContentSlot` push、detach/teardown erase，
> 将来"提前编译"入口或第二条提交路径就该共享它）；②"帧已提交"事件类 2 个（任何未来提交者都必须调）；
> ③契约上就该单调用点 2 个（`reportSessionDevice` 已有标志守卫 ⇒ 多调 no-op；`releaseAbandonedTargets` 挂在
> 帧边界上，而边界只有一处）。**处理 = 不改结构、改可发现性**：五个方法各写一条前置/幂等契约（顺序类 `@pre`：
> 编译早于本帧记录、结算晚于 `recordAndSubmit()+present()`；幂等类写明多调是 no-op）—— 比写"只被调用一次"
> 更稳（事实型断言会腐烂）。**并合并提交后那一对** `settleTransientPassVariants()` + `releaseParkedObjects()`
> → **`settleSubmittedFrame()`**（同一事件触发，合并后无法只做一半：早结算毁掉正在记录的帧、一帧推两次环会
> 早一帧释放）；名字 5→4，`submitFrame` 33 行。**先例警示**：`Impl::parkTargetObjects`（单调用点 helper 方案被
> 否决、函数体删了，声明+相反结论的注释又活好几批，§38 才清）⇒ 单调用点模式的真实代价是**静默腐烂**。
> **反向观察**：长得像但**保证不同**的规则不该合并（`releaseRenderTarget` 丢消费者槽**每槽一次计数等待** vs
> 重建路径**不能等**）—— 这种"看似可复用"比单调用点 helper 更值得警惕。验收同前（证据逐字节相同 / VUID 0 /
> FAIL 0 / test_vsg 100 / test_graphics 158 / 门禁 PASS）。

> 2026-09-12 **`submitFrame` 拆成帧协议（设计 §42）**：130 → **35** 行。五个具名步骤（`VsgRenderer` 私有、无
> 参数）：`releaseAbandonedTargets()`（宿主没打招呼丢掉的目标，表持有所有权因此再也查不到它）、
> `reportSessionDevice()`（首次提交记录驱动）、`compilePendingViews()`（D22 增量 + `VINE_VSG_DISABLE_INCREMENTAL_COMPILE`
> 逃逸 + 失败回落全图）、`settleTransientPassVariants()`（一次性变体**提交之后**才换回稳态）、
> `releaseParkedObjects()`（`kRetireRingDepth` 帧前停放对象：各内容槽桥环 + 渲染器自己的环）。`submitFrame`
> 现在就是协议本身，顺序一眼可见。**行数账**：函数 130→35，但 TU 981→960、头文件 +55（五段理由搬进声明处
> Doxygen，与该私有区既有风格一致）；收益是"不被理由淹没"而非总行数。**踩坑**：第一次编译 clang **崩溃**
> （frontend exit 135，栈顶指向一个注释里的 "annotation token"）—— 重跑同一条命令即通过 ⇒ 编译器瞬时崩溃
> （并行编译内存压力），不是代码问题；但**崩溃重跑干净后仍要跑完整判据**才认通过。验收同前（证据逐字节相同 /
> VUID 0 / FAIL 0 / test_vsg 100 / test_graphics 158 / 门禁 PASS）。

> 2026-09-12 **`buildOffscreenTarget` 拆分（设计 §41）**：范式不是"计划/施工"（其决定 `resolveDepthBorrow()` 早
> 已抽出且会报告），而是**按"一个 builder 拥有什么"命名**：①`Impl::resetTargetAttachments(t)` —— **列清一次
> build 拥有的全部东西**（三张槽表 / 颜色+深度图像与视图 / `passes` / `attachments_built`、`depth_seeded`、
> `any_load_pass`、`color_seeded`、`depth_sampleable`、`depth_borrow_pending_reported` / 借用源+视图+barrier /
> `graph` / 三套 depth 策略 shader set / 宽高 / `build_key`）；这份清单就是**所有权边界**，`Target` 加字段忘了
> 重置会变成残留图像/残留 built 标志而无人报错。②`Impl::dropConsumersSampling(target)` —— "重建方换了新视图，
> 而消费者的过期检查只看源**尺寸**" ⇒ 同尺寸重建后消费者仍采样旧视图；消费者靠槽属性 `source_target` 找，摘除
> 走 `detachSlotView()`。③`Impl::createTargetAttachments(...)` —— 图像+视图（含 usage 标志规则：深度没有
> `TRANSFER_SRC` 连 `TRANSFER_SRC_OPTIMAL` 都到不了；必须走 `createImageView()` 否则 framebuffer 带脏句柄只在
> `vkCmdBeginRenderPass` 崩）。`buildOffscreenTarget` 202 → **89** 行，TU 1410→1399。**踩坑**：`targets` 表以
> 非 const `RenderTarget*` 为键 ⇒ helper 的 `depth_src` 形参不能是 `const`（`map::find` 报 lose const
> qualifier）；中途误加的 `if (targets.empty()) return;` 会提前返回整个函数、静默跳过 barrier/build key/log —— **加守卫前先想清楚它会不会跳过后续步骤**。验收同前 + 全量重建。

> 2026-09-12 **`passGraph` 决定/施工分家（设计 §40b，承 §40）**：新增 `Impl::PassPlan`（值）+ `Impl::planPass(t,
> key, target) const`（纯决策无副作用），两段决策理由（load-op 策略、提升状态）搬进它的文档；`passGraph` 只剩
> 取材 → 施工，且 `has_color`/`has_depth` 中间变量删掉（统一读 `plan.*`）。**这一步的全部风险是取值顺序**，已写成
> `planPass` 的契约：`current` 指向目标的 pass 表 ⇒ 必须在 `publishPass` 之前取；`planPassVariant` 读的
> `any_load_pass`/`depth_seeded`/`color_seeded` 正是发布新对象会改的 ⇒ 计划必须先于撤销提升与发布。**踩坑**：
> `PassObjects` 是 `Target` 嵌套类型 ⇒ `PassPlan` 里写 `const Target::PassObjects*`（否则 `unknown type name`）；
> `reconcileOffscreenOrder()` 是 `VsgRenderer` 成员，`Impl` 调不到 ⇒ 重排仍留在 `passGraph`。结果 `passGraph`
> **270 → 164** 行（两批合计 −39%），TU 1425→1410；`planPass` 58 / `makePassGraph` 30 / `reuseSteadyPass` 18 /
> `publishPass` 22 行。验收同前（证据逐字节相同 / VUID 0 / FAIL 0 / test_vsg 100 / test_graphics 158 / 门禁 PASS）。

> 2026-09-12 **`passGraph` 拆分（设计 §40）**：先数职责：`passGraph` 270 行 = **9 个职责**，其中约 110 行
> 是决策理由注释。拆出三步（各步独立验证）：①`Impl::makePassGraph(t, has_depth, clear_color)` —— 一个 pass
> 一个 RenderGraph，**清屏值按附件顺序**（颜色在前、深度最后；attachment 0 是本 pass 清屏色，额外 MRT 保持
> 透明黑，深度项是目标的深度清屏值）；②`Impl::reuseSteadyPass(...)` —— 稳态帧全部开销（`passVariantIsStale`
> + 清屏值更新），null = 清屏策略变了要重建，"clear value 0 只在真有颜色附件时是颜色项"（否则是 union 里的
> 深度值）随之成为其文档；③`Impl::publishPass(t, key, objects, has_color)` —— 记录入库 + 目标级不变量
> （`color_seeded`/`depth_seeded`/`any_load_pass`/`depth_sampleable`）。**踩坑**：`reconcileOffscreenOrder()`
> 是 `VsgRenderer` 成员（命令图只有它有），`Impl` 没有外层 `this` ⇒ **`Impl` helper 不能调它**，"顺序变更→
> 重排""新图入图→重排"留在 `passGraph`。结果 270 → **210** 行。**下一步（未做，风险更高）**：把"决定"整体
> 打成 `Impl::PassPlan`（`planPass(t,key,target)`）并把两段决策理由搬进其文档，预计再降到 ~135 行；风险点是
> `current` 指针与 `planPassVariant` 读的目标级标志必须在"发布"改写它们之前取值，顺序错会静默改掉 load-op。

> 2026-09-12 **可维护性整理（设计 §39；一条规则一处）**：①`SceneBridgePipeline.cpp` 里 `hashCombine`
> 的混合式抄了三份、顶点布局哈希抄了两份 ⇒ 合为一个 `hashCombine()` + `vertexLayoutHash()`（**模板**：
> `VertexChannel` 是 `SceneBridge` 私有嵌套类型，文件内自由函数不能命名它）+ 两个具名种子；**三份哈希不同步
> 不会报错，只会让缓存不再命中**（每帧重编译，无诊断）。同批给 MRT 两条规则命名：
> `colourAttachmentCount(shader_set)` 与 `applyOpaqueBlendForAttachments(states, n)`（G-buffer 必须不混合
> 写入：法线附件 alpha≈shininess/256，混合会把它缩到 12.5%）。`buildStateGroup` 216→**179**。
> ②`SceneBridgeGeometry.cpp`：通道形状检查（1..4 分量 / 整除 / 顶点数）原先写两遍（调用处 report 一遍、
> `makeTypedVertexData` 内再守一遍）⇒ `channelShape()` 判一次 + `ignoredChannelMessage()` 一处出消息 +
> 前置条件声明；`buildGeometryData` 225→**202**。③**缺陷实测**：loc1 法线被拒的报告用三元选格式，却按
> 第一条分支的顺序传参 ⇒ 第二条分支打印互换的数字（`%zu` 读 32 位值）——**编译通过、运行通过、验证层干净，
> 只是消息在撒谎**。修法：两分支各自出消息。**排查手段**：写了参数计数检查器（三元格式按**分支**核对，
> 否则这种"顺序错"看不见），插件 14 文件 **0 命中** ⇒ 该缺陷类只剩这一处；检查器入库
> `scripts/check_diagnostic_formats.py`（有怀疑退出码 1）。④`VsgRenderer::initialize` 的 38 行窗口 traits
> 构造搬成 `makeWindowTraits(host_handle)`，137→**104**。**有意不做**：两处"退化法线保持零"的写法没合并
> （倒数乘 vs `vsg::normalize` 除法，合并会改最后一位比特，而像素在证据行里）。验收同前 + 全量重建。
> 三个 TU：641→700 / 473→539 / 965→981（Doxygen 比省下的代码长，收益是单点定义）。

> 2026-09-12 **结构整理七（设计 §38；只改结构）**：`buildOffscreenTarget`（重建分支）与
> `releaseRenderTarget`（释放分支）各自写了一遍同一件**破坏性拆解**（摘 pass 图 → 等设备 →
> 逐个 content slot `bridge.clearCache()` + 摘出编译队列）⇒ 合成 `Impl::unhookTargetPasses(Target&)`，
> 两处各调一次；"为什么这里必须等待"（clearCache 释放共享对象注册表，停放实测报
> `vkDestroyPipeline-00765`）的推理也随之下沉到这一处。另外：`buildOffscreenTarget` 里**第四份**
> "槽的 view 记在哪个图"（局部 lambda + 只有 program 槽用的 `forget_view`）删除，改走既有
> `Impl::detachSlotView()`（顺带消掉一处死 `erase`：只有 content 槽排队编译）；深度共享 barrier
> 抽成 `Impl::makeDepthShareBarrier(source)`（含 combined depth/stencil 必须覆盖两个 aspect 的
> `VUID-VkImageMemoryBarrier-image-03320` 规则），字段 `Target::depth_share_barrier` 由
> `ref_ptr<Node>` 收紧为 `ref_ptr<PipelineBarrier>`；删掉死声明 `Impl::parkTargetObjects`（第十批
> 否决"停放"后残留，注释还与实测结论相反）。行数：`buildOffscreenTarget` 272→**234**、
> `releaseRenderTarget` 266→**253**、TU 1460→1423（另一 TU +21）。等价性有断言兜底：`policy churn:`
> 第二段每帧翻附件形态 ⇒ 每帧走重建 teardown，并断言"等待数 = 重建数"。验收同前 + **全量重建**
> （发现并修掉 `tests/test_vsg/ProgramSamplingTest.cpp` 第 46 行的外来残留 `}-10/2=`，编译错误）。

> 2026-09-12 **结构整理六（设计 §37；只改结构）**：`renderContentSlot`（内容槽的每帧热路径）
> 223 → **120** 行。四个文件内 helper：`updateSlotViewport`（公告的矩形 / 未公告则整目标 / clamp 进
> 目标；2026-09-13 起不再看 presenting）、`seedSlotLight`（presenting 角色翻转时重置默认光 —— 方向光会把
> gizmo 从斜角照黑）、`beginLightsDroppedEpisode`（“宣告的灯全被丢掉”是**场景**属性而非帧属性
> ⇒ 每段只报一次，一旦有可用灯或本帧无灯立即重新武装；helper 只回答“现在要不要报”，真正的
> `reportFailure` 留在调用方 —— §31 那条“helper 不能持有 renderer 状态”的延伸）、
> `logContentSlotDiagnostics`（env 门控的 TEMP 诊断 `VINE_VSG_DIAG_MRT` 移出热路径；**它被两份
> 设计文档引用 ⇒ 不删，只搬**）。**踩坑**：helper 不能收 `ContentSlotRequest`（`Impl` 的嵌套
> 类型，文件内自由函数里不可命名）⇒ 改为传字段
> `(target, depth_mode, order, commands, created, root_children, variants)`。TU 547→550
> （含新 helper 的 Doxygen）。主流程现在读作“守卫 → 建槽/取图 → 重挂 → 重放属性 → 视口 →
> 相机/灯 → 同步命令 + 入队编译”七步。验收同前（`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 +
> test_vsg 100 / test_graphics 158 + 门禁 PASS）。

> 2026-09-12 **结构整理五（设计 §36；只改结构）**：两个 overlay 函数“节点造好之后”的部分也逐字重复
> （建视图 → 记入槽 camera/view/ready → 按 order 摆放 → 离屏则 `reconcileOffscreenOrder()`），
> 以及“slot 被 retire 过 → 重新挂上”分支；另发现 `makeCompiledOverlayView()` 的 `what` 形参
> **从无使用者**（死参数，§35 同类）。抽出：`placeOverlayView(dest, view, order)`
> （摆放 + “离屏目标就要 reconcile”这条规则写一处）+ `template <class Slot> installOverlayView(...)`
> （建视图/编译→失败报一次并返回 false，调用方丢自己的槽；成功则记入槽并摆放）；删死参数。
> 模板而非重载：两种槽只用共同字段 camera/view/order/ready；声明放私有区（形参无 `Impl` 类型
> ⇒ 公开头可写），定义在本 TU（两个实例化点都在此）。行数：`drawScreenTexture` 194→**169**、
> `drawScreenProgram` 222→**199**（相对 §32 之前 226/247 降了 57/48），TU 724→705。
> 有意保留的差异：源校验（PiP attachment 钳位 / program 深度提升报告）、key 构造、矩形策略
> （PiP 自动贴右下 / program 只钳位）、节点工厂与 `VN_LOGI` 文案 —— 再合并只会把差异藏进参数。
> 验收同前：`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 + test_vsg 100 / test_graphics 158 + 门禁 PASS。

> 2026-09-12 **重构残留检查（设计 §35）**：§33 把一次性提交收进 `Impl::submitOneShot()` 后留下两处
> **死代码**（`readColorBuffer` 的 `queue_family`、`readDepthBuffer` 的 `physical`）—— 无编译错，但已无用户。
> 更值得记的是**守卫的位置**：原来两个函数各自判 `device/physical` 空才提交；提交搬进 `submitOneShot()`
> 后，这个判空就与**真正解引用设备的地方**分家了 ⇒ 守卫应该跟着使用者走：`submitOneShot()` 改成
> `[[nodiscard]] bool`（自判 `window`/`getDevice`/`getPhysicalDevice`），调用方只在不可用时
> `return false`（colour 仍需 `device` 查格式属性、`source` 判空；depth 只需 `device` 建 staging buffer）。
> 顺带修回同批里改错的一处：抽走提交语句时误删了紧邻的 `VkImageSubresource sub_resource{...}`
> （编译器只在真用到时报）。**流程结论**：每个“抽走一段逻辑”的批次收尾都要专门查三类残留 ——
> ①死局部量/死参数；②守卫与被守卫的使用是否还在一起；③被搬走语句**紧邻**的声明是否被带走。
> 验收：`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 + test_vsg 100 / test_graphics 158 + 门禁 PASS
> （且 `-Wunused-result` 证明两个调用点都消费了返回值）。

> 2026-09-12 **结构整理四（设计 §34；只改结构）**：`makeScreenTextureNode`（PiP）与
> `makeFullscreenProgramNode`（用户全屏程序）在设备眼里是**同一件东西**（全屏三角形 + 深度关 +
> 不混合 + overlay 视口 + set0 采样纹理 + 运行时编译的 shader）—— 配方里有三段逐字重复：
> ①`ShaderCompiler` 可用性→stage 创建→编译→`ShaderSet{vs,fs}`+`makeOverlayPipelineStates`；
> ②`config->init()`→`copyTo(StateGroup, SharedObjects{})`→push constant（仅 program）→`Draw(3,1,0,0)`。
> 抽出 `makeOverlayShaderSet(vs, fs, entry, extent, failure)` + `makeOverlayStateGroup(config, push_data)`
> （push_data 为 null 即 PiP 那种），两工厂只剩差异（描述符/纹理/采样器/push 范围）。
> 行数：`makeScreenTextureNode` 73→**51**、`makeFullscreenProgramNode` 182→**159**。
> **诚实说明**：本批**没有**减少总行数（新 helper 的 Doxygen 比省下的代码长，TU 758→780）——
> 收益是**单点定义**：“overlay drawable 的配方”只有一份，两个 pass 不会漂移（同理 §32）。
> 验收同前：`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 + test_vsg 100 / test_graphics 158 + 门禁 PASS。

> 2026-09-12 **结构整理三（设计 §33；只改结构）**：①**两个回读函数的公共前奏去重**
> （`readColorBuffer` 134→122、`readDepthBuffer` 123→113）：抽 `Impl::readbackTarget()`
> （会话/`attachments_built`/尺寸，**故意不等设备** —— 调用方先格式检查再付等待）、
> `Impl::hostVisibleMemory()`（“回读落地内存必须 HOST_VISIBLE|HOST_COHERENT”写一处；colour 落
> LINEAR 图像 / depth 落 staging buffer）、`Impl::submitOneShot()`（队列+fence+具名超时常量
> `kReadbackTimeoutNs`，原先两处各写 `100000000000` 且无解释）。
> ②**设备等待一律可数**：`shutdown()` / `readColorBuffer` / `readDepthBuffer` 原先直接
> `viewer->deviceWaitIdle()`，绕过了第十批引入的计数 ⇒ 全部改走 `Impl::waitForIdle()`；
> 现在 `grep deviceWaitIdle` 在插件里只剩该函数体本身，`policy churn:` 的“0 次设备等待”
> 断言覆盖了全部等待入口。验收同前：`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 +
> test_vsg 100 / test_graphics 158 + 门禁 PASS。
> **仍剩**：`vsg_selftest/main.cpp`（~4600 行）拆 TU 需要**整段重写**数千行（工具链下代价高），
> 已记录待专门一轮；两个 overlay 函数尾部可模板化收敛。

> 2026-09-12 **结构整理续（设计 §32；只改结构，等价性判据同 §31：`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 + test_vsg 100 / test_graphics 158 + 门禁 PASS）**：①两个 overlay 函数头部的 55 行重复
> （viewport → 源校验 → **目的目标解析** → surf 尺寸 → retargetPass → passGraph）抽成
> `VsgRenderer::OverlayDestination` + `resolveOverlayDestination(source, key, what)`；
> 报错消息用 `formatDiagnostic(u8"%s: ...", what)` 保持逐字节不变。收益不只是行数：
> **“overlay 画到哪”的规则（反馈环 / 无附件 / 离屏重建）现在只有一份**。
> `drawScreenTexture` 226→**194**、`drawScreenProgram` 247→**222**。
> ②`reconcileOffscreenOrder` 215→**21** 行：一个值 `Impl::RecordPlan`（window_graph / graphs_of /
> present / order）+ 三个阶段 `fillRecordPlan`（收集，跳过 retired pass；同目标按显式 pass order）/`orderRecordPlan`（采样边 + 深度借用边 + `stableTopologicalOrder`）/`applyRecordPlan`
> （重挂 children + 借用方 barrier 在其源最后一图后 + 窗口图最后）。
> **过程教训**：大函数重构时**只替换头部会留下旧函数体**（编译期暴露）；拆函数要整段替换，
> 且 oldString 锚点要选在**两版真正不同的行**上（新旧 `pass_records` 注释几乎相同，只差折行与
> `Impl::Target&`/`Target&`）。**下一步候选**：`vsg_selftest/main.cpp` 4603 行按主题拆 TU +
> 共享 helper 头（判据现成：输出逐字节相同）；两个 overlay 函数尾部（建视图 → 记 camera/view/
> ready → placeViewByOrder → reconcile → 日志）可模板化收敛。

> 2026-09-12 **结构整理（设计 §31，只改结构、行为契约不动）**：判据是**机械重构等价性** ——
> 前后各跑一次独立 selftest，`diff` 全部 `[selftest]` 行**逐字节相同**（实测相同）+ VUID 0 / FAIL 0 +
> test_vsg 100 / test_graphics 158 + 门禁 PASS。①**三张槽表（content/screen/program）的遍历收敛成
> 访问器**：`Target::forEachSlot/visitSlot(key)/hasSlot(key)/eraseSlot(kind,key)` + `SlotKind`，
> 4 处双/三循环各变一个循环；kind 差异用 `if constexpr (requires { slot.bridge; })` 就地表达；
> `Impl::slotGraph()` 取代三份"槽的记录图在哪"的 lambda，并把 `retireInactivePassSlots` 的
> "预扫描 + 真扫描"合成一次遍历。②**`passGraph` 387 → 271 行**：抽出 `passAttachments()`（含
> `PassAttachments`）、`makePassObjects()`、`depthStillPromoted()`、`revokeDepthPromotion()`
> （均在 `Impl`）+ `VsgRenderer::dropDepthSamplingProgramSlots()`。③**`buildOffscreenTarget`
> 317 → 257 行**：借用校验抽成 `VsgRenderer::resolveDepthBorrow()`。④删掉重构后失效的重复：
> `releaseRenderTarget` 的局部 `graph_of_slot`、`VsgBackendUtility` 的自由 `waitForIdle`。
> **结构约束（记住）**：公开头只前置声明 `struct Impl;` ⇒ 内部 helper **不能**在公开头写
> `Impl::Target&` 形参（incomplete type）；要么做成 `Impl` 成员（内部头声明），要么做成
> `VsgRenderer` 私有方法且形参不出现 `Impl` 类型；`Impl` 内声明要晚于 `Target` 定义。
> 下一步候选：`drawScreenTexture`(226) 与 `drawScreenProgram`(247) 共享骨架（dest/矩形/stale/建视图/
> 摆放），`reconcileOffscreenOrder`(209) 三事混一。

> 2026-09-12 **方向登记：合并每 pass 的 `beginRenderPass`（dynamic rendering）被上游卡住（设计 §9.4）**：
> 一个 render pass 对象只能烧死一种 load-op 组合 —— 这就是"每 pass 一个 render pass"的唯一理由，
> 也是变体工厂 / "变体必须兼容"的子通道依赖约束 / 一次性 transient 变体 + 帧末换回 / D49 重建
> 这整批机制存在的原因。若能走 dynamic rendering（`vkCmdBeginRendering`：attachments 内联、load-op
> 变逐 pass 取值、布局转换改显式 barrier），这些**大部分可删**（`planPassVariant()` 的**决策**留下，
> selftest 相位作为行为契约不动）。**当前走不通**（vsg 1.1.16 实测）：①全树无
> `vkCmdBeginRendering`/`VkRenderingInfo`（记录路径只有 `RenderGraph.cpp:150/170` 的 begin/end
> render pass）；②无任意命令的记录钩子（无 `CustomCommand` 一类）；③
> `GraphicsPipeline.cpp:233` 硬绑 `pipelineCreateInfo.renderPass`，`pNext = nullptr`
> ⇒ 产不出 dynamic-rendering 形态的管线。触发条件：上游支持（或推 PR）/ 记录开销或 tile GPU 成瓶颈；
> 第一步是"一个 target 一个作用域"的 spike，判据复用 `clear flip:` + `policy churn:` + VUID 0。
> 收益性质是**简化**而非桌面性能（begin/end 在桌面/离屏开销很小）。

> 2026-09-12 **停放的边界：非破坏性停、破坏性等（第十批）**：（1）`SceneBridge::invalidateState()`
> **本来就**把状态包装停放（`retireNode`）⇒ **depth 模式变更**那处调用方等待是多余的，去掉（反证：
> 改回 → `policy churn:` 报 **14 次**等待 / 15 帧，因为该相位每帧翻一次 depth 模式）。相位现在同时翻
> colour clear / depth clear / **depth 模式** / pass 是否公告，四种都是 **0 等待**。
> （2）同批尝试把 **target 重建 / 槽 teardown** 的等待也换成"停放 view" ⇒ **被实测否决**：那两条路径会
> `clearCache()`，清空共享对象注册表，那里的管线/采样器没有存活节点兜底 ⇒ lavapipe 报 **12 条**
> `VUID-vkDestroyPipeline-00765` / `vkDestroySampler-01082`（`VkPipeline ... in use by VkCommandBuffer`）。
> 于是改回计数等待并写明原因。**结论：非破坏性路径停放 (0 等待)，破坏性路径（碰 clearCache / 丢图像）
> 保留计数等待** —— 这就是界。
> （3）相位加第二段：每帧翻 target 附件形态（depth promotion 属 build key）→ 每帧重建，断言
> "重建 = frames-1、等待 = 重建" ⇒ 破坏性路径每帧恰好一次 teardown 等待。
> （4）顺带量到语义不对称：**pass 请求**（清屏策略）当帧生效，**target 描述**变更在**下一帧 start**
> 才被采纳（相位计数按实测写成 `frames - 1`）。
> 验收：test_vsg 100 / test_graphics 158 / 独立 selftest VUID 0 + FAIL 0 / 门禁 `RESULT: PASS`。

> 2026-09-12 **策略变化帧不再停设备（渲染器自己的退役环）**：变体重建、提升撤销级联、被丢弃的
> program 节点、"某个 pass 本帧不再公告"的视图摘除，原来都在帧装配期 `waitForIdle()`。现在被换下的
> render pass / framebuffer / 节点停放进渲染器自己的退役环（`Impl::retireObject()` /
> `advanceRetireRing()`，环深与推进点跟 §8.2 的节点环一致：`kRetireRingDepth = 4`，`submitFrame()`
> 提交之后推进）。判定标准仍是 §3：**破坏性销毁**（槽 teardown / target 重建 / `bridge.clearCache()` /
> depth 模式变更的状态重建）继续显式等待。**视图摘除那处等待本来就是多余的** —— 那条路径只把 view
> 从图上摘下、槽（view/节点/管线）全部保留，没有任何对象被销毁。
> 判据 `[selftest] policy churn:`（`runPolicyChurnStressPhase`）：15 帧里翻颜色清屏、深度清屏、
> pass 是否公告，断言 `deviceWaitCount()` 增量 **0**、退役环**确实释放**（`retiredObjectCount()` > 0）、
> target 构建数**恰好 2**、末帧像素/深度仍符合末帧策略。反证（实测）：改回等待 → 同 15 帧 **29 次**
> 设备等待 + 环零释放；只把视图摘除那处改回 → **7 次**。为可数，所有刻意等待都走 `Impl::waitForIdle()`。
> 仍未覆盖：depth 模式变更（`SceneBridge::invalidateState()` 丢状态包装）与 `clearCache()` 系列 ——
> 第一版相位翻了 depth 模式，每帧都撞上这处等待（实测确认），故相位固定 depth 模式并写明边界。
> 验收：test_vsg 100 / test_graphics 158 / 独立 selftest VUID 0 + FAIL 0 / 门禁 `RESULT: PASS`
> （新增 `require_evidence "^\[selftest\] policy churn:"`）。

> 2026-09-12 **D47 同帧残留窗口（撤销提升必须同时丢弃"真正绑定深度"的 program 槽）**：撤销提升发生在
> 正在组帧的那一帧里，而该帧更早建好的全屏 program 槽若**绑定了**源深度，其描述符声明的是"提升后"的
> `SHADER_READ_ONLY` —— 撤销（重建提升型 pass + 本 pass）却把图像留在附件布局 ⇒ 本帧记录过期描述符
> （`VUID-vkCmdDraw-imageLayout-00344`，每条 draw 一条，宿主无感）。判据先落地：`depth sample:` 相位
> 第 3 段 —— 采样深度的 program pass（order 2）**先建**，保留型 pass（order 1）**后到**，且 P(order 0)
> 已先跑 ⇒ 问题被孤立在"槽"上而不是 pass 自己的布局；修复前实测 1 条 VUID + 2 条 FAIL（无丢弃报告 /
> 目标被写成采到的 (6,6,6)）。
> 修法：撤销级联里 `removeGraphChild` + 删槽 + 一条 `dropped for this frame` 报告；宿主下一次
> `drawScreenProgram` 按新的可采样性重建（不需要深度照常画，需要深度走既有的
> `MissingDescriptorBinding` 拒绝）。**只丢真正绑定了深度的槽**：`programSamplesDepth()`（纯函数，
> ABI 的深度绑定号 = 颜色附件数；`ProgramSamplingTest` 4 例钉住）判定 —— 纯颜色 program 的管线布局里
> 没有深度采样器，本帧照常绘制、下一帧也不需要重建。
> 有意**没有**走"撤销延后一帧"（要同时改 `depth_still_promoted` 判据、install 时机与
> `depth_sampleable` 语义，牵动 D47 诊断 / 借用校验 / `readDepthBuffer` 的 barrier 推导；而且撤销
> pass 自己就把深度交回附件布局，槽照样会过期 ⇒ 丢弃这一步无论如何都要做）。
> 验收：test_vsg **100**（+4）/ test_graphics **158** / 独立 selftest VUID 0 + FAIL 0 / 门禁
> `RESULT: PASS`（新增 `require_evidence "dropped for this frame"`）。

> 2026-09-11 **颜色 bootstrap 必须只清一次，且它有判据（设计 §30 "D49 补"）**：新目标的颜色图是
> UNDEFINED，第一个 pass 必须清一次才能被后续 LOAD；这一次清必须是**一次性变体**（帧末换回稳态
> LOAD），否则它就变成"这个 pass 永远清颜色"、每帧抹掉同目标早先 pass 的东西。反证两半都实测：
> 拿掉一次性变体 → `color bootstrap:` 相位报"填充被抹掉"；首帧直接记稳态变体 → 6 行 VUID。
> 另：一个目标的 pass 按它们记录 order 依次 build 只是个**假设**（`depth_read_only` 依赖它），
> 反序渲染实测为干净（VUID 0 / FAIL 0，原因：先建的是保留型 pass，走 seed 分支），该反序场景已
> 永久留在 `preserved depth:` 相位里。
>
> 2026-09-11 **depth-only 目标的布局制度 + 描述符绑定必须拒不可用者（设计 §30）**：（1）depth-only
> 目标（shadow-map 形态）的深度**永远**收在 `SHADER_READ_ONLY`（它存在的意义就是这个），所以
> ①`clearDepth=false` 必须真的 LOAD（`makeDepthOnlyRenderPass()` 也收 `depth_initial`）；
> ②它的 `depth_sampleable` **不得**被 LOAD pass 撤销 —— `readDepthBuffer()` 就是拿这个标志当
> "图像当前布局"用，谎报就得到 oldLayout 不符的 barrier（反证实测 8 行 VUID）。稳态布局改用
> `steady_depth_layout` 表达（depth-only ⇒ SHADER_READ_ONLY，否则 ATTACHMENT）。判据：
> `depth only preserve:` 证据行。
> （2）**fragment 声明的描述符绑定若本 pass 提供不了，必须在建节点时拒掉并上报**
> （`ProgramNodeFailure::MissingDescriptorBinding`）：全屏 program 只能提供"颜色附件 0..n-1 + 深度（仅当
> 真可采样）"，否则建的管线 layout 缺该 bind，错的是每帧一条 VUID、宿主却一无所知。vsg 1.1.16 无
> shader 反射 ⇒ 绑定从源码扫（`declaredBindings()`）。这条也把 D47 变成**可观测**：真去采样
> `binding = N`（深度）的 program 相位 `depth sample:`（正面：采到深度；反面：被拒 + 目标不被写）。
>
> 2026-09-11 **pass 粒度的 render pass 变体可以运行期互换，但有三个硬条件（D49/D51，设计 §30）**：
> （1）**子通道依赖必须逐字段相同** —— 渲染通道兼容性只豁免 initial/final layout 与 load/store op，
> 依赖不同就 `VUID-vkCmdDrawIndexed-renderPass-02684`；两个工厂的 `ext_to_sub.srcAccessMask` 因此
> 统一成与 load-op 无关的常量。
> （2）**LOAD depth 的 pass 必须声明图像"真实所在"的布局**：UNDEFINED（要先 CLEAR seed）/
> ATTACHMENT / **SHADER_READ_ONLY**（上一帧的提升型 pass 留在可采样布局）—— 第三种漏了就是
> `VUID-vkCmdDraw-None-09600`。`makeDepthLoadRenderPass()` 的 `initial_clear` 因此升级为
> `VkImageLayout depth_initial`，三种变体互相兼容、可随时换；"提升还没撤销、本帧又没别的 pass 先跑"
> 的那一帧记**一次性变体**，帧末 `submitFrame()` 换回常驻变体（同 seed 机制，字段更名
> `render_pass_transient` / `transient`）。
> （3）**重建判据比"宿主请求"（want_color_clear / want_depth_clear），不能比 load-op** —— 同一帧一个
> pass 会被建两次（`setupContentSlot()` + `render()`），颜色 bootstrap 会让第二次算出不同 load-op
> ⇒ 一帧内重建成"对 UNDEFINED 图像做 LOAD"。颜色 bootstrap 也是一次性变体，否则它变成"这个 pass
> 永远清颜色"。判据：selftest `depth only:` / `clear flip:` 证据行（各自反证后必红）。
>
> 2026-09-11 **`SceneView::setScene` 必须把新场景送到两个消费者（D50，设计 §30）**：默认 window pass 的内容是**绑在 pass 上**的（`addPass(pass, content, order)`），而惰性创建的默认 `OrbitCameraManipulator` 持场景的 **raw_ptr**（拾取 / `fitToScreen`）⇒ 只换 `scene_` 会让 viewer 继续画旧场景、并留下悬垂指针。修法：`bindPassContent(window, scene_)` + `dynamic_cast<OrbitCameraManipulator*>` 后 `setScene()`（不新增成员）。判据：帧内 `Scene::contentCollectCount()`（替换后走 1 遍 / 被替换 0 遍）+ manipulator 的 `scene()`。

> 2026-09-11 **共享对象表必须跟着缓存驱逐走（D40，设计 §27）**：`config->copyTo(stateGroup, shared_objects_)` 是**登记即持有** ⇒ 变体条目被 FIFO 逐出/abandoned 后，去重表**仍然**抓着那些 pipeline，只增不减（直到槽 teardown）⇒ D16 修好的缓存上限形同虚设。修法：`releaseAbandonedCaches()` 在**有驱逐的那一帧**调 `shared_objects_->prune()`（vsg 的 `prune()` 就是本项目自己的规则：`referenceCount()==1` = 除了表没人要 ⇒ 删，在用的变体经缓存 bind 命令继续持有而被保留）；触发面覆盖**两条**路径：abandoned 清扫 + 插入点 FIFO 裁剪（`noteEviction()` 记账）⇒ 只在驱逐帧走表，摊销 O(1)/驱逐。判据：test_vsg `SharedObjectsTableIsPrunedOnEvictionFramesOnly`（65 程序 ⇒ 裁剪 ⇒ 必须 prune；64 个相同变体仍须塌成 1 个 pipeline；再画必定命中的程序 ⇒ 不得 prune）；反证：停 prune → 首条红。**诚实记录**："重建被逐出变体会重新计数"依赖条目所有者何时放手（retire 环），单个 sync 内不可确定性断言 ⇒ 未写。另：把 render pass 下沉到 pass 粒度的**分阶段计划**已写入设计 §28（含 6 条必须同时成立的不变量），未实施。

> 2026-09-11 **目标描述中途改变必须重建：构建指纹（D39，设计 §26）**：`buildOffscreenTarget` 把附件数量/格式、深度格式、深度提升标志烧进图像 + render pass + framebuffer，但重建谓词只逐项列了尺寸/深度策略/借用项 ⇒ **没列到的属性静默失效**：中途 `attachColor()` → 新附件永远不存在（`readColorBuffer(t,1)` 报 "out of range"，听着像不支持、其实是丢了请求）；中途 `setDepthPromotion(true)` → **变更帧重建 0 次**（反证实测），且 §23 借用校验读的是构建时烧下的 `depth_sampleable` ⇒ 继续放行借用 → 借用方把一个已按采样布局收尾的深度当附件挂上（§23 拦的错误从后门回来）。修法：`Target::BuildKey`（附件数 + 各格式 + 深度有无/格式 + 提升标志）构建末尾记录、重建重置块清空、重建谓词整把比较；尺寸/深度策略仍是独立项（借用校验要读已建尺寸、`releaseRenderTarget` 靠清零逼重建）。判据：selftest `runTargetDescriptionChangePhase` 两段（加附件 → 附件 1 必须读回透明黑 + 恰好 1 次重建；开提升 → 源 + 借用方恰好 2 次重建、借用被拒报 1 次、远面又能画出来）；反证：停掉指纹项 → 4 条断言同时红。

> 2026-09-11 **深度借用的两个隐性缺陷（D38，设计 §25）**：借来的深度是直接烧进帧缓冲的，所以源一重建（**同尺寸**！混合深度策略收敛、尺寸变化都会）就换掉它的深度图像，而借用方只在**自己**重建时才重新校验 → 帧缓冲继续测**没人再写的旧图**（深度静默冻结）。修法：`Target::depth_source_view` 记住烧进帧缓冲的那张源视图，`render()` 比较"源当前的 `depth_view` != 记录值"即重建（重跑 §23 借用校验，带墓碑守卫防诊断/重建循环）。**另一半**：`reconcileOffscreenOrder` 的依赖边只来自采样（PiP / 全屏 program），深度借用**不是边**，所以源重建后被追加到末尾 → 借用方记录在源**之前** → 整帧用上一帧的深度（静默 1 帧滞后）；现在 `depth_source` 也是一条边。判据：selftest `runDepthShareOrderPhase`（借用方用 `TestOnly` 只测不写，否则写入会污染共享图），源第 2 帧混合重建、第 3 帧画更近的面 → 从第 3 帧起必须帧帧被拒（实测 `AAA---`）；反证：停 `borrow_stale` → `AAAAAA` 报红，停依赖边**在当前实现下不可观测**（借用方的重建会重新追加到末尾，等价修好顺序）→ 该边作为不变量保留并记录在案。

> 2026-09-11 **`DepthMode::TestOnly` 的语义断言（设计 §24）**：两条内置半透明 pass（forward /
> deferred 的 `forward_transparent`）都用 TestOnly，但以前**只驱动、从没量过**。新阶段
> `runDepthTestOnlyPixelPhase`：不透明 pass 先写近面深度；半透明 pass（不 clear、TestOnly）按序
> 画 近/中/远 三个四边形 → 断言**中心必须是中间那个（蓝）** + **深度读回仍是不透明 pass 的值**。
> 这一条中心断言同时盖住两半错误：写深度 → 更近的绿色（先画）会赢；不测试 → 最后画的灰色
> （在不透明面之后）会赢（反证实测：TestOnly→蓝 (5,10,46)、改 TestAndWrite→绿 (5,41,10)、改
> Disabled→灰 (43,43,43)）。harness 要求 `depth testonly:` ≥1 行。

> 2026-09-11 **前端也有诊断通道了（D37，设计 graphics-render-pipeline §13）**：`RenderEngine` 以前只
> 转发宿主的 sink，自己不能上报，于是"`ScreenPass` 声明的输入一个都没解析到 → 什么都不画"**完全静默**
> （生产者被禁用/移除/改名/排在后面都触发）。现在 `reportEngineProblem()` 送同一宿主 sink +
> `engineDiagnosticCount()`，`resolvePassInputs` 在**全部落空**时 `ContentSkipped` 上报（含 pass 名与
> 输入名）；每 pass 只报一次、解析成功即**重新武装**、`removePass`/`clearPasses` 清理记录（否则新 pass
> 复用同地址会被旧记录噤声）。契约：声明的输入必须由本帧**更早**运行的 pass 发布（注册表每帧清空），
> 多名字=备选链，只要一个命中就不报。

> 2026-09-11 **深度借用的可用性校验（D36，设计 §23）**：`shareDepth` 只在源深度图"原样可用"时才能
> 成立。三种不可用情形分**两类命运**：**瞬时**（源本帧还没深度图，如预热顺序）→ 本帧用自己的深度渲染
> + **源一出现就重试借**（`render()` 重建谓词新增一项）+ 每段只报一次；**持久**（尺寸不同 / 源把深度
> `setDepthPromotion(true)` 成可采样）→ 墓碑报一次 + 回落自有深度。三类过去都静默且都画错：尺寸不同 →
> 非法帧缓冲（llvmpipe 上还"看起来正常"）；源提升过 → 每帧 `VUID-VkImageMemoryBarrier-oldLayout-01197`
> 且什么都没画。**坑**：第一版把三类都当持久 → app 预热阶段把借永久关掉（表面能跑，半透明内容再也测不到
> 不透明深度）；selftest 的"借生效后远面必须被拒"用例当场报红。契约：源**同尺寸** + `setDepthPromotion(false)`
> + 源先于借方建好（否则下一帧重试）。harness 要求 `depth borrow:` ≥1 行。

> 2026-09-11 **内容收集的帧内复用（D27，设计 graphics-render-pipeline §12）**：
> `RenderEngine::frame` 开局给每个场景 `Scene::setContentFrame(token)`（幂等），同一帧内**同视图**的
> 多个 pass 共享一次全树遍历；memo 键是 `(projection*view, eye, 内容版本)`——**不是相机地址**
> （同视图的另一相机共享、相机就地编辑 miss、**堆栈相机安全**：用 `intrusive_ptr` 持键会
> `free(): invalid pointer`）；失效 = 帧边界 + 场景自身变更（同值 setter 不失效）+ `invalidateContent()`；
> `token==0`（自己调 `collectRenderCommands`）时**完全不缓存** → 零行为变更；返回的是**副本**
> （pass 的 `programOverride` 不泄漏）。实测 debug/2000 节点：遍历 17.1 ms vs 复用 0.10 ms（**170×**）。
> 残留：**同一帧两个 pass 之间**直接改节点要下一帧生效。可观测：`Scene::contentCollectCount()` /
> `contentCollectReuseCount()`（test_graphics 151 → 156）。**部署教训**：改 SDK 类布局（给 `Scene`/
> `RenderEngine` 加成员）后必须整体刷新 `dist/lib/*.so*` + `dist/plugins/vine/*.so` + `dist/bin/Vine`，
> 只换两个文件会让其它插件按旧布局分配对象 → `free(): invalid pointer`。

> 2026-09-11 **同目标多 pass 的深度策略："最后一次请求赢"是缺陷，已修（设计 §22，D35）**。
> 不透明 pass 每帧清深度 + 半透明 pass 保留深度是真实多 pass 管线的标准写法，而 `clearDepth`
> 曾是**目标**属性 → 目标只烧一个 depth load-op → 第二个 pass 的 `clearDepth=false` **吞掉**了
> 第一个 pass 的每帧清深度 → 旧帧深度留着 → 移开的物体继续遮挡（ghosting，静默错画）。
> 修法：`clearDepth` 同时是 **pass 作用域属性**（`PassRequest::clear_depth`）；同一目标出现不同
> 请求 → `Target::depth_policy_mixed`（sticky）→ `Target::wantsDepthLoad()` 改用 LOAD pass，
> **要清的人自己在 view 里、自己绘制之前插 `ClearAttachments`**（值 = `Target::depth_clear_value`，
> 与 render pass 同源）；该命令必须在**独立于 bridge root 的 group** 里（bridge 会重建 root 子节点）
> 且位于 view 内（`vkCmdClearAttachments` 只能写在 render pass 实例里）。已知残留：冲突到**第二个
> 请求到达时**才发现，**首帧仍按旧策略**，次帧起两者都满足（粘性标志不震荡）；借用深度的目标
> （`depth_source != nullptr`）既不用 LOAD pass 也不自清。

> 2026-09-11 **深度 LOAD / 共享深度的语义断言（设计 §21）**：两条此前只靠"无 VUID"覆盖的语义
> 现在有回读断言。**`runDepthLoadPixelPhase`**：`clearDepth` 是**目标**的 pass 属性（一个目标
> 一个 depth load op，"最后一次 clear 请求"就是该目标的策略），所以"同一目标两个 pass、一个
> CLEAR 一个 LOAD"是**模型外用法**（实测必败）；支持的用法是单 pass + 深度**跨帧** LOAD：
> 阶段 1 画近面（0.0249）播种，阶段 2 换成远面（0.0166）→ 必须 0 蓝像素、中心保持该 pass
> 清屏色、深度**一字不变**、目标只构建 1 次。**`runSharedDepthPixelPhase`**：出借方画近面，
> 借用者（`shareDepth`，只清颜色）画远面必须**一个像素都不变**（拒绝），再画更近面必须赢
> （接受）—— 两半都要，否则"全拒绝"也能通过。判据力做过反证（出借方改 `Disabled` → 立刻报
> 256 蓝像素）。harness 现在要求 `depth load:` ≥1 与 `shared depth pixels:` ≥1 行证据。

> 2026-09-11 **缓存收口：一套骨架、四种缓存（设计 §20，D16 / D34）**：`SceneBridge` 的四个
> 缓存不再各写一套语义 —— 几何缓存（自持 + 外侧放手即逐出，**无容量上限**；2026-09-14 起无时间窗）、`program_stages_`
> （64 FIFO）、`program_shader_sets_`（64 FIFO）、`variant_cache_`（256 FIFO，**两个键都自持**）
> 都走 `OwnedCache.hpp`（`OwnedCacheEntry` / 新增 `OwnedPairCacheEntry` + `keyReleased()` 唯一
> 定义处）；"超限整表清空"删除（D16），改为插入时 FIFO 修剪 + 每帧 `releaseAbandonedCaches()`。
> **D34（身份靠地址但不持地址）已修**：`Item::material/program` 改为自持；两个哈希键缓存自持
> 键对象（variant 同时持 program + material）。**注意**：多缓存共享同一对象时 `abandoned()`
> 只在其它缓存也放手后才成立（`eraseAbandoned` 回收的是链尾，链长由 FIFO 上限界定）。
> `OwnedCache.hpp` 已从 `src/` 挪到插件 `include/vine/vsg/`（成员类型要出现在 `SceneBridge.hpp`
> 里）。新增 `SceneBridgeCacheOwnershipTest`（4 个测试，含"模板在阶段条目被逐出后仍持 program"
> 这个判别性用例）。

> 2026-09-11 **SceneBridge 拆分（设计 §19）**：1571 行 → 四单元：`SceneBridge.cpp` 512（会话态 +
> `syncRenderCommands` + 保留态 `Item`）、`SceneBridgeGeometry.cpp` 473（`buildGeometryData` +
> 顶点打包 helper）、`SceneBridgePipeline.cpp` 620（`getProgramShaderSet` / `buildStateGroup` +
> 编译/变体 helper）、`SceneBridgeInternals.hpp` 44（两 TU 唯一共享的 `VariantEntry`，内部头不
> 安装）。规则：**匿名 helper 只留在唯一使用者的 TU**（22 个 helper 里只有 `VariantEntry` 共享）
> → 不升级成公开接口。纯搬运：非空行多重集只差 include / 命名空间 / 内部头前言，零代码改写。
> include 按"符号驱动 + 编译验证"收窄（同步净减 1、几何净减 20、管线净减 12），判据是**插件 +
> selftest + test_vsg 三份编译命令同时通过**（三目标 include 上下文不同，只验一份会漏）。
> `test_vsg` / `vsg_backend_selftest` 的源列表已同步（MODULE 不能链接插件）。验收：67 + 151 全绿、
> 门禁 `RESULT: PASS`（0 VUID，六组证据齐全）。

> 2026-09-11 **深度回读 + 直接深度断言（设计 §18，顺带两个真缺陷）**：`VsgRenderer::readDepthBuffer`
> 落地 —— 只读无歧义格式（D32_SFLOAT / D16_UNORM），**打包 D24 诚实返回 false**；做法
> `vkCmdCopyImageToBuffer` → 宿主可见 buffer（深度不能 blit），拷完转回原布局。断言直接读深度值：
> 同一四边形 4 单位 vs 6 单位 → `near=0.0249 > far=0.0166 > 清屏 0`（比值 == 距离比，反 Z 的
> `z ≈ near/d`），`Disabled` 时中心仍 0（写入侧也关）。
> 顺带抓到的缺陷：**(A)** 离屏目标表条目**不自持** `RenderTarget` → 宿主销毁目标后同地址新目标
> **继承死目标的附件**（断言第一次跑就撞上：第二个 D32 目标读回打包 D24）；修法照搬缓存骨架
> （`Target::owner` + `Impl::entryFor()` + `submitFrame()` 回收 `useCount()<=1`）。
> **(B)** 深度图缺 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`（连转到 TRANSFER_SRC 都非法，
> `VUID-vkCmdCopyImageToBuffer-srcImage-00186`）——**只在开了 `VINE_VSG_DEBUG_LAYER=1` 的门禁里
> 才暴露**，本地跑的时候没开：本地验证必须与门禁同环境。
> harness 分别要求像素 ≥4 / 深度 ≥1 / MRT ≥2 行证据。

> 2026-09-11 **像素断言铺开到其余路径（设计 §17）**：`vsg_backend_selftest` 现在断言 PiP blit
> （子矩形中心=生产者内容、内缘=生产者清屏色、矩形外=消费者清屏色、**变化像素数恰好等于矩形
> 面积**）、deferred 全屏程序（整张目标填满程序输出）、**深度顺序**（近红远蓝按"画家算法会画错
> 的顺序"提交：开深度测试时蓝色像素必须为 0）、**深度模式权威**（`DepthMode::Disabled` 时中心
> 必须变蓝）、MRT 附件 0 必须收到几何。门禁打印逐条证据并**要求 ≥5 行 `[selftest] pixels:`**
> （断言被删掉不能读作通过）。
> 量到一条此前只写在注释里的规则：**MRT 附件 ≥1 一律清成透明黑**（附件 0 才拿 `clear()` 颜色）
> ——对采样额外附件的消费者（deferred 读 G-buffer 法线）可见，已登记 D31 待决策。
> 辅助设施：`PixelImage` / `readTarget(…, attachment)` / `driveContentPass` / `makeVisibleQuad(half, z)`。

> 2026-09-11 **反 Z 陷阱（归因实验结论，设计 §16）**：本后端 reverse-Z（近→NDC 1、远→0、
> 深度清 0、`COMPARE_OP_GREATER`）。**用户程序自己写 `gl_Position` 时若写 `z = 0`（非反 Z
> 直觉的"近平面"）实际落在远平面，与清屏深度相等 → 严格 greater 拒绝全部片元 → 绘制对象
> 完全消失，零 VUID、零诊断**。selftest 的变体探针（同四边形单变量）常驻打印
> `covered=0`（z=0）vs `covered=5916`（z=0.5）；`runPixelReadbackPhase` 用 z=0.5 的用户
> 程序断言该路径确实光栅化（`covered ≥ 1000`、中心 == (255,51,51)）。顺带补口：
> `assignArray` 未命中且着色器声明过该绑定时上报；`bindGraphicsPipeline == nullptr` 不再
> 静默记录。

> 2026-09-11 **像素回读 + 像素断言（D20 正面修补）**：`VsgRenderer::readColorBuffer` 落地
> （离屏 RGBA8：blit 到线性宿主可见图 + 按 rowPitch 收成紧凑 RGBA8；float 附件诚实返回 false
> + ContentSkipped；同步语义先 `deviceWaitIdle`，源图 SHADER_READ_ONLY↔TRANSFER_SRC 双向屏障）。
> selftest 新增 `runPixelReadbackPhase`（中心像素=被光照红四边形、角像素=清屏色、alpha=255、
> RGBA16F 返回 false），harness 把 `[selftest] FAIL` 当硬失败。**旧盲点**：原有辅助几何占 0 像素
> （裁剪空间 x 全相同 / Phong 通路下与世界视线共面），所以过去"无 VUID"从未证明光栅化。
> 后端在**首次 submit** 打印 `[VsgRenderer] device: ...`（vsg 懒创建 device，initialize 期间
> 拿不到），harness 显示为 `[info] Vulkan device:`。本机只有 llvmpipe（无 GPU 驱动），真机冒烟
> 仍未做。设计 §15。

## 后端契约（规范性在 `RenderBackend.hpp` 类注释；预算见设计文档 §14）
- **调用序**：`beginFrame` → 每启用 pass（order 升序）`beginPass`→`setPassOrder`→可选逐
  pass 状态→绘制→`endPass` → `endFrame` → `swapBuffers`（**唯一 present 点**）。首帧前有
  warm-up（启用且非清屏的 pass 先各跑一遍），所以"首帧前创建的东西"也要能摆对位置。
- **借用**：camera / commands / lights / target / program 只在当次调用内有效（commands 是本
  帧临时量），`beginPass` 的 pass 只在作用域内有效；**不得保留**，需要就拷贝/上传。
  * `releasePass()` / `releaseRenderTarget()` **同时宣布宿主可以立刻销毁该对象** ⇒ 队列里的“宣布”（
    `request.pass` / `request.target`）必须一起作废：直接驱动路径的请求**跨帧存活**，留着就是悬垂指针；
    接下来那一次画不了 ⇒ **拒画 + 分集报一次**（`PassProtocolViolation`，消息带哪个调用点被跳过），
    **不得静默改画到窗口**。同理：0 尺寸的 target 建不了 ⇒ `TargetBuildFailed` 分集报一次，
    不静默什么都不画。
- **保留**：保留 GPU 状态必须按被服务对象寿命定键、由对应 `release*` 释放、**不随帧数增长**；
  宿主不必为正确性调 `release*`。
- **线程**：串行、不可重入、无需加锁；不得假设跨 `initialize`/`shutdown` 同线程；**诊断 sink
  是同步回调**，只能记录返回，不得回调后端。
- **声明**：输出的三种声明（`setRenderTarget` 画到哪 / `setOutput`·`setOutputTarget` 对象声明（校验 + 对象侧解析）/ `setOutputName` 查表键）**互不覆盖**，一个错误只报一次；**发不出去的必须报** —— 名字表只能交付可采样 target，所以"渲染进窗口却声明发布名"和 `publish(name, nullptr)` 都**分集上报**（过去是静默丢弃，消费者随后被告知"本帧没人产出它"，指向错的一方）。
- **pass 的前置条件（接线期一次性检查）**：`ScreenPass` 必须有输入（否则永远不画）、只声明深度的 texture 路径会被**静默**换成彩色 0、**有 program 就必须有相机**（program 路径要先建 view）——这三条都在 `validateWiring()` 报一次（分集），因为 host 侧看到的是"这个 pass 从来不出现"。
- **声明的 `ImageRef` 必须 `bind` 到一张 target**：未绑定的身份**没有地址** ⇒ 消费者解析不到、承诺校验也无从比较（旧注释写着"接线期已报"其实没报）⇒ 整根线静默。现按**每张图**报一次（分集），且**已被冲突报过的不再报**（一个错误一条消息）。
- **失败**：接口内不抛异常；`false` ≠ 部分生效；**`initialize()` 返回 false 必须自己收拾残局**
  （引擎只在 initialize 成功后才调 `shutdown()`，见 `RenderEngine::shutdown`）。
- **保留预算**（本后端数字）：退役环 4（提交帧）、几何条目按“外侧是否仍持有”回收（无时间窗，2026-09-14 删）、材质 256 条、
  变体/ShaderSet 超限整表清空、pass/target 槽靠 `releasePass`/`releaseRenderTarget`。

> 2026-09-11 **后端模块拆分（结构，行为零变更）**：`VsgRenderer.cpp` 3603 -> 901 行，
> 按职责拆成 `VsgRendererPasses/Targets/Overlay.cpp` + `VsgRenderer.hpp`（会话态；
> §47 后会话态在 `include/vine/vsg/VsgRendererState.hpp`）
> + `VsgPipelineFactory.{hpp,cpp}`（纯工厂，`vn::vsg::detail`）+ `VsgBackendUtility.*`
> （图手术/设备同步/策略）。置放规则：纯工厂只依赖显式参数、只返回失败原因；会话态改
> `VsgRenderer.hpp`；跨 TU 自由函数进 `detail`（各 TU `using namespace detail;`）。
> 顺手修正漂移的文档注释与 `LightPushBlock` 的编译期断言位置。设计 §13。
> 注意：`test_vsg` 与 `vsg_backend_selftest` 直接编译插件源码，加/删 .cpp 必须同步其
> 源列表。

> 2026-09-11 **后端缓存骨架
 + 材质缓存修复（D13/D19）**：新增
> `src/plugins/gfx_backend_vsg/src/OwnedCache.hpp` —— 保留型缓存统一"条目自持键对象
> （地址不可能在存活期内被复用）+ `eraseAbandoned()`（`useCount()<=1` ⇒ 除缓存无人能再查到
> ⇒ 立即回收）+ `trimToCapacity()`（FIFO，永不动 null 键默认条目）"两半不变量。
> `VsgMaterialManager` 切到该骨架：条目自持 `Material`（修掉同地址复用旧 Phong 值/descriptor）、
> `releaseAbandoned()` 由 `VsgRenderer::submitFrame()` 每提交帧调、`kMaxEntries = 256` FIFO 兜底；
> 刷新路径收敛为 `updateMaterial()` 唯一入口（`Entry` 记上次 `PhongParameters`，相等即不写/不 dirty/
> 不传输 —— 此前该接口全仓零调用点，SceneBridge 自己又写了一遍 = D19）。设计 §12、register D13/D19、
> 测试 `tests/test_vsg/MaterialManagerTest.cpp`（6 例，含"地址唯一性"确定性断言）。

> 2026-09-11 **pass 协议显式化**：7 个 `pending_*` 字段收敛为单一 `PassRequest`（`VsgRenderer::Impl`）
> + 显式作用域（`pass_open`）；**作用域属性**（pass/target/order/depth_mode/presenting）在作用域内
> 每次绘制都有效、`endPass()` 丢弃；**每次绘制属性**（viewport/lights）由紧随的绘制调用消费；
> `resetPassRequest()` 只有一句 `request = PassRequest{}`（新字段不会漏清）。违反协议（嵌套
> `beginPass` / 不配对 `endPass`）上报为 `DiagnosticCategory::PassProtocolViolation`（Warning）；
> 直连驱动（无作用域，自检/legacy 键）行为不变，`isPassScopeOpen()` 可断言状态。
> 改这条协议前先读设计文档 §11；测试见 `tests/test_vsg/PassProtocolTest.cpp`（**无需设备**：
> 测试目标已编入 `VsgRenderer.cpp`/`CameraBridge.cpp`）。

> 2026-09-11 **后端诊断通道（失败不再静默）**：新增 `vine/graphics/RenderDiagnostic.hpp`
> （`DiagnosticSeverity` / `DiagnosticCategory`（枚举、按后果分类）/ `RenderDiagnostic` /
> `DiagnosticSink`）+ `RenderBackend::setDiagnosticSink/diagnosticSink/diagnosticCount(category)`
> 与 `protected reportDiagnostic`（默认实现齐全 → 现有后端零改动）；`RenderEngine::setDiagnosticSink`
> 是宿主入口（引擎保存并应用到当前与之后的后端）。**vsg 后端：`VsgRenderer::reportFailure` 是唯一上报
> 权威**（stderr 追踪 + 计数器 + 宿主 sink），槽内 `SceneBridge` 经 `installDiagnosticRoute` 把发现
> 转发给它（保证 `diagnosticCount()` 诚实、无双份追踪）；自由函数 helper 改为**返回失败原因**由调用方
> 上报。已接线：几何拒绝 / 通道丢弃 / 着色回退（原 D9 全静默）/ 编译失败 / 离屏 target 失败 / 内容跳过 /
> 初始化失败；上报频次按 **revision**（不刷屏）。宿主侧 `RenderControl` 把诊断写进 `vine/logging`。
> 验证：test_graphics 151 / test_vsg 58 / `vsg_backend_selftest::runDiagnosticsPhase()`（真实设备上
> 1 条 GeometryRejected + 1 条 ShaderFallback 到达 sink）/ lavapipe 门禁 PASS。详见设计文档 §10。

> 2026-09-11 **状态一致性 / 资源生命周期终检**（详见 `.ai/design/vsg-pass-lifecycle.md` §8）：
> (1) **保留缓存的键必须指向活对象**：`SceneBridge` 的 `cache_`/`rejected_`/`program_stages_`
> 原按裸指针索引且不自持 → 对象销毁后地址复用会把死条目的保留状态（旧网格 / 旧 SPIR-V /
> 旧拒绝记录）喂给新对象（静默错误）。现条目**自持**所索引的几何/program，并把拒绝记录
> 合并进 `Item`；几何**缓存之外无人持有时**（`abandoned(shares)`）当帧回收，仍被引用则一直保留
> （2026-09-14 起无 600 帧窗）。(2) **退役环**：活路径上被替换的保留节点（数据/状态包装/条目驱逐）先进
> `SceneBridge::retireNode()`，由 `advanceRetireRing()` 在每个**已提交**帧后推进，环深 4
> （=命令槽 3+1）→ 可能的槽已重新录制（其 fence 已等）后才能销毁，避免
> `VUID-vkDestroyPipeline-00765`/`vkDestroyBuffer-*` 类的在飞销毁。(3) **`Group::addChild`
> 拒绝成环**（祖先链检查，静默拒绝），否则递归遍历栈溢出并破坏包围盒缓存前提。
> 复查通过：单线程（无 thread/锁）、`shutdown()` 先 `deviceWaitIdle` 再整体重建、
> `clearCache()` 5 处调用点均先 `waitForIdle`。仍未做：D13（材质缓存无逐出 + 同地址
> 复用风险）、跨 pass 命令缓存、`VkPipelineCache`（三项逐项设计登记见该文档 §9）。验证：test_graphics 150 /
> test_vsg 55 / lavapipe 门禁 RESULT: PASS（含新 churn phase）。

> 2026-09-11 **遍历热路径 + 顶点属性 stride**：`Scene::collectRenderCommands` 每 pass 每帧全树走，
> 原实现有四处算法级重复：(1) `Node::worldMatrix()` 递归 O(depth²)；(2) 遍历中每节点再调一次
> `worldMatrix()`（O(n·depth)）；(3) 每节点调 `boundingBox()`，容器又要 union 子树（O(n·depth)）；
> (4) 排序比较器内做两次 `modelMatrix * Point3d` + 开方（O(n log n) 次矩阵乘）。
> 现已改为：`worldMatrix()` 单次折叠 O(depth)（`localTransformMatrix()` 提为 public 供自顶向下累积）、
> 遍历把父矩阵当参数下传、`BoundsCache` 每趟每节点 bbox 只算一次（叶子调虚函数，容器 union 子节点
> —— 场景图是树所以成立）、排序前算一次平方距离并排指针（保持 stable 语义）。另加
> `Group::childrenRef()`（热路径 5 处不再拷贝 NodePtr 向量）。实测同一 debug 构建：3 层/1080 命令
> 23.9→7.3 ms，5 层/9720 命令 375.7→83.9 ms（3.3~4.5x）。
> **stride 缺陷（正确性）**：`AttributeChannel::components` 就是 stride，但 `Geometry::localBounds/
> positionCount/normalCount` 与 `RayIntersection::meshOfGeometry` 都按每顶点 3 float 读 → vec4 位置通道
> 的 AABB 错误（错剔除 / 错 fitToScreen）、计数错误、拾取失效；新增 `stride()/vertexCount()/xyz(i)`
> 并全部改用它。回归测试：GraphicsTest 新增 12 个（含 `CountingGeometry/CountingGroup` 证明每叶子
> bbox 只问一次），共 148 个。详见 `.ai/design/vsg-pass-lifecycle.md` §6.1。

> 2026-09-11 **pass 生命周期（架构级）**：新增 `RenderBackend::beginPass(pass)/endPass()/releasePass(pass)`
> （默认空实现，向后兼容）；**后端保留状态的槽身份从 `(camera, order)` 改为 pass 指针**
> （`VsgRenderer::SlotKey`；直接驱动后端、不调 beginPass 的调用方仍走历史键）。引擎在每个 enabled pass
> 前 beginPass、后 endPass，removePass/clearPasses 额外调 releasePass。修复：禁用 pass 仍绘制（幽灵）、
> 运行期换 camera/RT 残留旧槽、槽 depth/presenting 首帧冻结、同 (camera,order) 两 pass 互相覆盖、
> pending 泄漏到下一 pass。另修：`shareDepth`+`clearDepth=false` 每帧重建（H4）、释放被借深度者留悬垂屏障（H5）、
> **本帧必 present**（beginFrame 已 acquire 交换帧图像，跳过 submit → `VUID-vkAcquireNextImageKHR-07783`）。
> **DepthMode 权威化**：`RenderCommand::depthExplicit` + `SceneBridge::setContentDepthMode` → TestOnly/Disabled
> 真正进管线（此前被 `applyRenderStateObjects` 覆盖=失效）。**铁律：先 `deviceWaitIdle` 再删被提交命令缓冲引用的
> 对象**（否则 `VUID-vkDestroyPipeline-00765`/`vkDestroySampler-01082`，validation 实测抓到）。
> 新诊断 `VsgRenderer::offscreenBuildCount()`（稳态不增长）。详见 `.ai/design/vsg-pass-lifecycle.md`。
> 验证：test_graphics 135 / test_vsg 53 / `scripts/gfx_lavapipe_check.sh` RESULT: PASS；
> （历史：那两份评审记录 `src/plugins/gfx_backend_vsg/{ISSUES,REVIEW_FINDINGS}.md` 已于 2026-09-12 从仓库移除 ——
> 它们列的缺陷全部已解决，结论已并入本文件与 `.ai/design/`；原文留在 git 历史里可查。）

> **世界坐标系约定：Z-up**（robotics：X 前、Y 左、Z 上）。OrbitCameraManipulator 与全部
> app_shell demo 已按此转换（内容映射 Y-up `(x,y_h,z)` → Z-up `(x,z,y_h)`；demo 相机 up=`(0,0,1)`）。
> vsg/glTF 生态原生 Y-up，将来在加载边界转换；URDF 原生 Z-up。

> 2026-09-07 **Design C（RenderEngine 瘦身 + SceneView）**：相机/导航/内容都不再放 engine，引擎
> 纯调度器（零内容零相机）。新增 graphics 概念 `SceneView`（宿主无关，组合**借用** engine）：owns
> Camera + 内容 Scene（`scene()` 返回 owning `intrusive_ptr<Scene>`）+ 默认 OrbitCameraManipulator
> （懒创建）+ 尺寸/aspect 维护；内容一律 per-pass 显式绑定（`addPass(pass, content, order)`）。
> `ensureWindowPass()` 注册 order-0 窗口 pass（3 参显式绑 view scene）除非 engine 已有「携带该
> camera 且 RT==null」的 pass。RenderEngine 删除 `setMasterCamera/masterCamera`、
> `setCameraManipulator/cameraManipulator`、mouse/scroll/key 三路 `pushEvent`、Resize 里的相机
> aspect 维护，**以及 `scene_/setScene/scene()`**（默认内容）；`hasWindowPass()` 无参
> →`hasWindowPass(raw_ptr<Camera>)`（结构化查询）。RenderControl 持 engine+view，输入/尺寸走 view；
> AppShell/test_plugin 改用 `render_control->view()->camera()/scene()`，内容 pass 显式绑
> `view->scene()`（gbuf/deferred/multislot/builder.setContent）；RenderPipelineBuilder 需显式
> setCamera/setContent。CameraManipulator 基类新增虚 fitToScreen()/home()（Orbit override）。
> test_graphics 121/121（SceneViewTest×4），全量构建绿。详见 /memories/repo/vine-sceneview-engine.md
> 与本文件下方旧 Design B 条目（已过时）。**新 .cpp 进构建需重跑 cmake configure**。
> 2026-09-07 布局最终态：engine `resize(w,h)`（由 `pushEvent(ResizeEvent)` 改名，唯一 resize
> 入口；RenderControl 一行 `engine->resize(w,sh)` 紧接 `view->onSurfaceResized(w,sh)`）只做
> swapchain+frame_ctx；RenderTarget 只有 setSize；新增 `Viewport` 值对象由 RenderPass 持有（未设=
> 全幅）；resize 布局走 SceneView::addSurfaceLayout 回调注册（创建方显式，每 effect 自己
> setSize/setViewport）。test_graphics 124/124。
>
> 2026-09-08 **统一主窗管线 preset**：`RenderPipelineBuilder::build(PipelinePreset, PipelineOptions)`
> → `intrusive_ptr<Pipeline>`（新 `RenderPipeline.hpp/.cpp`，RefCounted）。`Forward`=order0 窗口场景 pass；
> `Deferred`=order-3 gbuffer(MRT albedo/normal+shininess/spec/view-pos+D24, 发布"GBuffer")+order0 全屏延迟
> 光照 ScreenPass（带 camera+绑 content 转发灯光）为窗口 pass；阴影变体 ForwardShadowed/DeferredShadowed
> =占位（暂同无阴影）。Deferred **默认自带临时 shader**（builder 公共静态 `defaultGbufferGeometryProgram()`/
> `defaultDeferredLightProgram()`）与 canonical G-buffer（`defaultGbufferTarget(w,h)`），调用方零 GLSL；options 的
> gbuffer/lighting program 为可选覆盖；缺 camera/content 才返回 null。**SceneView::ensureWindowPass
> 默认 viewer 也改走 build(Forward)**（同一套 recipe），不再手搓 pass；行为不变。test_graphics 128/128。
>
> 2026-09-07 **Scene 收敛为单根**：`Scene` 只持一个根 `Node`（空场景 `root()==nullptr`，不渲染）。
> 删除 `addNode/removeNode/nodes()`；新增 `setRoot(intrusive_ptr<Node>)/root()`；`clear()` 释根
> （灯不受影响，仍用 `clearLights()`）。`findNode/boundingBox/collectRenderCommands` 自单根遍历。
> 消费迁移：`addBox`/demo 改收 `Group*`（单恒等根 Group + `addChild`，删 removeNode 再包 StateNode
> 的写法改为直接 state 包 box 后 addChild 根）；AxisGizmo 3 根并入一个根 Group；RayIntersection
> 两遍历器改单根；test_graphics 增 `setIdentityRoot(Scene&)` helper、SceneTest 改 RootSetClearAndFind/
> RootlessSceneIsEmpty。世界矩阵/bbox/剔除/透明度/状态折叠语义不变（根 Group 恒等）——纯 API 形态
> 收敛，与 osg/vsg "一个 scene = 一棵根子树" 对齐（graphics-scene-graph.md §5）。
> 旧多 root 表述仍留在 graphics-design.md §3.4/§7（已标注过时）。
>
> 2026-09-04 **已知缺陷已登记**（26 项 D1–D26，分级 🔴/🟡/🟢）：存
> `src/plugins/gfx_backend_vsg/docs/data-flow.md` §13（内存审计：两侧引用
> 计数、无环、真泄漏风险低；主要风险 = 只增不减留存 + 行为缺陷。🔴：D13 材质缓存
> 无逐出且 `releaseMaterial` 全仓零调用点；D10 `ShaderProgram` 无 revision → 改
> shader 不重编；D9 编译失败静默回退内建；D3 用户 loc6 顶点色被白覆盖；D1
> components 未当 stride）。
>
> 2026-09-04 **Material 透明度已移除 + 内建 ShaderSet 契约已归档**：透明只属
> scene/node(叶)/geometry 的 opacity（per-vertex alpha 通道 = vine_Color.a@loc6），
> **Material 纯颜色**。删除 `Material::opacity()/setOpacity()`+`opacity_`（doc 注明
> diffuse alpha 恒 1 忽略）；`RenderCommand` ctor 不再用材质 opacity 播种（Scene
> 收集器重算）；Scene 有效透明度 = clamp(node_opacity)（叶 Geometry 自身即 node，
> 材质项移除）；vsg `VsgRenderer` no-cull 收集器去 `material_opacity` 项；材质默认
> 灰 Phong 在 `VsgMaterialManager` 里 force `diffuse.a=1`。测试改用叶/节点透明度：
> `CollectCommandsSortsTransparentBackToFrontAfterOpaque` 用 MatrixTransform
> setOpacity(0.5)，`CollectCommandsOpacityIncludesMaterial`→改名 `…LeafAndNode`
> (叶 0.5×祖先 0.5=0.25)；MaterialTest 删 opacity 断言。**test_graphics 113/113、
> test_vsg 10/10、全量构建、lavapipe 回归全绿**。材质颜色类型保持 `Colorf`(vec4)
> 不改 vec3（全 SDK 统一 + vsg PhongMaterial/UBO 本就 vec4；opaque 约定放渲染映射
> 层 force alpha=1）。内建 ShaderSet 输入契约（attribute loc/format/define +
> descriptor set/binding + pc 128B + "按名查找+define 变体"机制，flat/phong/pbr 共用
> 一张表）已核对进 `.ai/design/vsg-custom-shader.md` §9（供过渡期与 P0 自写对照）。
>
> 2026-09-03 **lavapipe 像素级渲染验证通过（抓帧）**：`vsg_color_probe` 增 `VINE_PROBE_CAPTURE=out.ppm`
> 抓帧（照 vsgExamples/app/vsgscreenshot 读回：blit 上一帧 swapchain → 线性 R8G8B8A8 → map）。
> custom 模式抓帧像素**正确**：中心 (255,108,89)=sRGB(线性 1.0,0.15,0.1 珊瑚色三角形)、角落
> (149,149,149)=sRGB(clear 0.3)——即**运行期编译的用户 program 真实光栅化**。PPM→PNG 用
> `scripts/ppm2png.py`（纯 stdlib），图已人工确认（灰底珊瑚色三角）。注：抓帧 barrier 曾报
> swapchain 无 TRANSFER_SRC 的 VUID（lavapipe 仍出正确像素）；抓帧目前仅在 vsg_color_probe 用。
> 用法：`VINE_PROBE_MODE=custom VINE_PROBE_CAPTURE=out.ppm …/vsg_color_probe`。
>
> 2026-09-03 **SceneBridge program 接线已落地**：`buildGeometry` 增 program 参数——`cmd.program` 非空时
> `buildProgramShaderSet()`（ShaderCompiler 运行期编译 stages→SPIR-V，手搭 ShaderSet：vine_Vertex loc0 +
> addPushConstantRange("pc",0,128)+继承默认管线状态），只喂位置数组、跳过 material 描述符/per-vertex
> opacity；失败自动回退内置（坏 program 不伤场景）。`Item` 重建键含 program。验证：临时 VINE_DEMO_PROGRAM
> 钩子（box_side 挂用户 VS/FS）lavapipe 首帧 `created=1`（独立管线）、无验证错误（钩子已移除）；
> test_vsg 10/10、test_graphics 113/113、lavapipe 默认回归 PASS。遗留：真机像素级验证、把 program demo
> 固化为可复现用例。
>
> 2026-09-03 **SceneBridge program 接线卡点已解**（vsgExamples 官方证据）：
> - vsg 每 drawable 由 `RecordTraversal.cpp` 自动 push **push constant "pc" = { mat4 projection;
>   mat4 modelView }（0..128B）**；自定义 ShaderSet 声明同构 GLSL 块即拿到 view/model（modelView 含
>   场景 MatrixTransform 累计矩阵）。官方模板：`/opt/opensrc/vsgExamples/examples/utils/
>   vsgcustomshaderset/custom_pbr.cpp`（addAttributeBinding(vine_Vertex,loc0) + addPushConstantRange
>   ("pc",0,128) + 可选 ViewDependentStateBinding(VIEW set1)/lightData）。
> - `vsg_color_probe custom` 已升级为官方契约（VS 用 pc.projection*pc.modelView*vine_Vertex），
>   lavapipe 20 帧无错误。**SceneBridge 接线 = 直接镜像**：program!=null → ShaderCompiler 编译 stages
>   → ShaderSet(vine_Vertex loc0 + pc range + 复用默认管线状态) → assign 位置数组 → mapper 状态 →
>   config.init；Item 重建键加 program。缺 v1 最小 demo/真机像素验证。
>
> 2026-09-03 三稿评审（graphics-scene-graph/state/shader + 旧 graphics-design.md）：补📋评审核对/
> 过时横幅——正文=写作时设计（MatrixNode/Drawable/primitive/"Scene 只持 root"均历史表述，以顶部
> ⚠/📋为准）；Scene 实现为多 root（有意）；program 槽 SDK 侧全落地、后端接线进行中。
> **SceneBridge program 接线卡点（关键）**：vsg 内置 phong ShaderSet 是**序列化 blob**（无文本），
> 且 vsg 内核/ViewDependentState 无"model/视图矩阵"标准注入名 ⇒ 无法安全镜像"自定义着色器如何拿到
> view/model"。下一步应先 `vsg_shader_dump` 打印 phong 的 stages/attributeBindings/descriptorBindings/
> pushConstantRanges 以逆向契约，勿盲改 SceneBridge。
>
> 2026-09-03 program 槽 P1 · 自定义 ShaderSet 装配探针（vsg_color_probe `VINE_PROBE_MODE=custom`）：
> 运行期 `vsg::ShaderCompiler`(glslang) 编译用户 VS/FS GLSL→SPIR-V → **手搭 `ShaderSet`**（1 个
> vine_Vertex attributeBinding loc0、无 descriptor/push）→ `GraphicsPipelineConfigurator` 成管线 →
> BindVertexBuffers+Draw，直接 clip 空间输出（z=0.5、关深度）。lavapipe+验证层 20 帧**无错误**；
> 已纳入 `scripts/gfx_lavapipe_check.sh`（4 段含 custom）回归 PASS。这证明"装配半环"的最小契约可行。
> 剩余：把该机制接进 `SceneBridge::buildGeometry`（遇 `cmd.program` 建 ShaderSet 并泛化数组/描述符
> 绑定；视图矩阵需经 vsg ViewDependentState 绑定，需真机像素验证——勿盲改）；Item 重建键纳入 program。
>
> 2026-09-03 program 槽 P1 · glslang 运行期编译验证：`tests/test_vsg/GlslCompileTest.cpp`
> （+2）——`vsg::ShaderCompiler::supported()==true`，SDK `ShaderProgram` 的 VS/FS GLSL 逐 stage 经
> ShaderCompiler 成功编出 SPIR-V（module->code 非空，72ms，纯 CPU 无需 device）。test_vsg 10/10。
> 这验证了 program 槽后端装配的"编译半环"；"装配半环"（按用户 Program 建 ShaderSet（stages/
> attributeBindings/descriptorBindings）+ SceneBridge buildGeometry 泛化 + Item 重建键含 program +
> lavapipe/真机像素验证）仍待做——注意 vsg 的 phong ShaderSet 是序列化 blob（shaders/phong_ShaderSet.cpp
> 为 io.read_cast 数据），自定义需手搭 addDescriptorBinding(…, coordinateSpace) 等，视图矩阵经
> ViewDependentState 绑定，需 GPU/像素验证，勿盲改。
>
> 2026-09-03 program 槽 P1 第二步（解析链 + 命令携带）落地：`StateNode` 增
> `setProgram/clearProgram/program()`（子树级着色覆盖，独立于 render-state）；新增自由函数
> `effectiveProgram(node)`（叶子 Geometry program 优先 → 最近祖先 StateNode program → null=默认）；
> `Scene::collectRenderCommands` 每叶把 `cmd.program` 折好；`RenderCommand` 增 `ShaderProgramPtr
> program`。test_graphics **113 tests 全绿**（+4），全工程构建过。遗留：vsg 后端按 cmd.program
> 建 ShaderSet 装配（ShaderCompiler 可用）+ Item 重建键含 program。
>
> 2026-09-03 program 槽 P1 第一步（SDK 类型 + 挂点）落地：`ShaderProgram.hpp/.cpp` 新增
> `ShaderStageType{Vertex/Fragment/Compute}` + `ShaderStage{type,source,entryPoint="main"}`
> + `ShaderProgram : Object+RefCounted`（name/addStage/stageCount/stage/stages）；`Geometry` 增
> `program()/setProgram()`（null=引擎默认，零回归）。test_graphics **109 tests 全绿**（+2），
> 全工程构建过。遗留：StateNode 级 program + 解析链、RenderCommand 携带、vsg 后端按用户 Program
> 建 ShaderSet 装配（ShaderCompiler 已可用）。
>
> 2026-09-03 **glslang 已集成**（vsg 运行期 GLSL→SPIR-V 可用）：
> - gfx_backend_vsg 在 VINE_USE_FETCHCONTENT 分支**改源码构建 vsg**（FetchContent v1.1.16，链系统
>   glslang-dev 16.2）→ `VSG_SUPPORTS_ShaderCompiler 1`（build/_deps/vsg-build/include/.../Version.h），
>   ShaderCompiler.cpp 已编译；无 glslang 的本地 `/opt/opensrc/VSG` 仅 VINE_USE_FETCHCONTENT=OFF 用。
> - 依赖注记：本 build 的 `FETCHCONTENT_FULLY_DISCONNECTED` 曾缓存 ON（网络禁用、依赖预取）——
>   vsg 首次拉取须 `-DFETCHCONTENT_FULLY_DISCONNECTED=OFF -DFETCHCONTENT_UPDATES_DISCONNECTED=ON`；
>   vsg 拉取后与其余依赖一样进 _deps。graphics-shader.md 顶部 ⚠ 已把"仅离线"决策更新为
>   "运行期可用（默认）+ 离线/SPIR-V 兜底"。
> - 验证：test_vsg 8/8、test_graphics 107/107、lavapipe 回归脚本 PASS（新 vsg）。
>
> 2026-09-03 program 槽 · 编译约束决策（graphics-shader.md 修订）：本 vsg **无 glslang**（运行期
> GLSL 编译不可用、不引入运行期依赖）→ 定 **"作者 GLSL、运行 SPIR-V、离线编译"**：`ShaderProgram`
> 作者用 GLSL 源，运行交付 SPIR-V（字节/.spv）；SDK 提供 `compileGlslToSpirv` 薄辅助（调开发机
> `/usr/bin/glslangValidator`，缺工具报清晰错误）；后端按 `ShaderSet.stages/attributeBindings/
> descriptorBindings/defaultGraphicsPipelineStates` 自描述装配 + GraphicsPipelineConfigurator；
> `program()==nullptr`→内置默认零回归。下一步 P1：SDK ShaderStage/ShaderProgram/Param 类型 +
> Geometry.setProgram + 后端装配。
>
> 2026-09-03 lavapipe 真机验证（Mesa 软件 Vulkan，llvmpipe CPU，Vulkan 1.4.335 + Khronos validation）：
> ①`vsg_color_probe` raw/box/flat 各 10-20 帧——无验证错误；②Vine 主程序默认 demo 端到端（VsgRenderer+
> SceneBridge）：每帧 sync 稳定 rootChildren=5、changed=0，无验证错误/VUID——**默认映射==现状零回归**成立；
> ③临时给 box_side 套 StateNode(PolygonMode::Line + 自定义 blend SrcColor/OneMinusSrcColor) 后重跑：
> 首帧 `created=1`（状态变→重建独立管线），后续稳定，**无验证错误**——非默认 RenderStateMapper 路径经
> driver 验证通过（验证后钩子已移除，demo 复原）。**可复用回归脚本：`scripts/gfx_lavapipe_check.sh`**
> （跑 raw/box probe + Vine 默认 demo，抓验证层错误；env 覆盖帧数/秒数，`VINE_SKIP_APP=1` 跳过 app）。
> 运行方式：`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./bin/Vine`。遗留：像素级（winding/
> blend 视觉效果）仍需人工真机确认。
>
> 2026-09-03 后端 renderState 消费（vsg）落地：`include/vine/vsg/RenderStateMapper.hpp`（纯、device-free）
> `makeRenderStateObjects(resolved)->{DepthStencil,Rasterization,ColorBlend,InputAssembly}` +
> `applyRenderStateObjects(config,…)`；SceneBridge::buildGeometry 按 cmd.renderState 装配、Item 增
> render_state（状态变→重建）；tests/test_vsg/RenderStateMapperTest 6 用例全绿（直跑 8/8，全工程构建过）。
> **两个历史决策落地**：①深度——vsg 投影是 reverse-Z(1→0)，SDK CompareOp 为"closer/farther"距离语义
> ⇒ 边界反转映射（Less→VK GREATER…），默认=vsg GREATER=现状零回归；②Blend——per-vertex-alpha 使 alpha
> blend 必须常开，blend.enabled 只是"是否用自定义因子"，vsg 后端无法经 StateNode 关闭 alpha blend。
> 映射：cull→cullMode(CCW front,默认 NONE)、polygon→polygonMode、topology→InputAssembly.topology。
> 遗留：cull winding/blend 因子真机视觉验证（无 GPU）。test_vsg 现 8/8（+6 映射）。
>
> 2026-09-03 场景图 R1 补强：新增 `MatrixTransformTest`（5）与 `GroupTest`（1）+ Scene 嵌套 world
> modelMatrix + GeometryTest 拓扑分离用例，**test_graphics 107 tests 全绿**（新增 8）。
> `vertexCount` = 纯数据统计，不随 StateNode Topology 变化（有测试钉住）；`triangleCount` 已于
> 2026-09-04 移除（Geometry 数据面不保证是三角网格；geometry 模块 `Mesh::triangleCount()` 保留）。
>
> 2026-09-03 场景图 R1 核心落地（test_graphics 99 绿、全工程构建过、test_vsg 直跑 2/2 绿）：
> **`Node` 拆成基类**（name/visible/opacity/parent()/虚 boundingBox()/worldMatrix()；无 children/
> 无变换）；**`Group`** 承接 addChild/removeChild/children()/boundingBox=children 并集；
> **`MatrixTransform : Group`** 为唯一变换源（matrix/setMatrix，经 protected 虚 localTransformMatrix()
> 参与 Node::worldMatrix() 父链累积）；`StateNode : Group` 不变。**`Geometry : Node` 叶子**：
> material 并入（name/visible/opacity 继承自 Node），boundingBox = loc0 本地盒 × worldMatrix()。
> **`Drawable.hpp/.cpp` 已删**；`RenderCommand.drawable`→`RenderCommand.geometry`(GeometryPtr)。
> Scene::collect/findNode、RayIntersection 三遍历器、AxisGizmo、SceneBridge、VsgRenderer no-cull
> walker、app_shell::addBox、TestRenderLiveCommand、GraphicsTest 全部迁移；`makeTriangleNode` 现返
> MatrixTransform(子=Geometry)。bbox 语义=**世界空间**（叶子世界盒；Group/MT=子世界盒并集）。
> 关键易错：`Mat4d()` 默认=identity（无 ::identity()）；Mat4d×Point3d 需 include math/Transform3.hpp；
> intrusive_ptr 析构需完整类型→RenderCommand.hpp 直接 include Geometry.hpp；加/删 .cpp 要重跑 cmake。
> 基线无关失败：test_cppstd/test_runtime/test_system 与本重构无关；test_vsg 的 ctest SegFault 为既有
> 退出时序问题（直跑干净）。
> 遗留：program 槽、renderState 后端消费、MatrixTransform 测试用例补强（现靠 Scene/Node 用例覆盖）。
>
> 2026-09-03 场景图 R1（进行中，当前绿点）：`MatrixNode` 更名/落成 **`MatrixTransform`**（真实变换节点：
> matrix()/setMatrix/worldMatrix（父链累积）+ boundingBox override）；`Node::boundingBox()` 已虚化。
> 下一步（R1 核心）：`Node` 拆成基类（name/visible/opacity/parent + 虚 boundingBox，去掉 children/
> transform/drawables）+ `Group`(children) + `MatrixTransform`(matrix) 语义到位 + `Geometry : Node`
> 叶子上树（material 收进来）→ 删 `Drawable` 与 `Node::drawables` → `RenderCommand.drawable` 改
> GeometryPtr → `Scene::collect`/`RayIntersection` 改树遍历。消费者/测试迁移量大，逐步保绿。
>
> 2026-09-03 场景图 S1（类型体系就位）：新增 `Group`（children 容器基类）与 `MatrixNode`（变换节点），
> `StateNode` 从 `Node` 重挂到 `Group`；Node→Group→{MatrixNode,StateNode} 骨架建立（vsg/文档对齐），
> 行为零回归（99 用例绿）。⚠ 过渡：Node 仍自带 children/transform/drawables（旧 API 保留）；后续 S2
> 把变换收敛到 MatrixNode、Geometry 上树为叶子、顺势删 Drawable。新 .cpp 加入需重跑 cmake 配置
> （glob 才拾取）。
>
> 2026-09-03 Geometry 重构为**开放属性列表**：单一 `location→AttributeChannel`（打包 float +
> components），`addBuffer(loc)` 为唯一写入口，数量不限（仅受后端 max-attribute）；不再有
> positions_/normals_ 固定成员；`setPositions/setNormals` 降级为便捷（写 loc0/loc1），`positions()/
> normals()` typed 访问器已移除 → `positionCount()/normalCount()`；约定 loc0=position（bbox/计数/
> 拾取）。SceneBridge（物化 loc0/loc1）与 RayIntersection（GeometryMesh 物化 loc0）已适配；
> `GeometryTest` 7/7、全仓 99 用例绿。索引仍独立一条（index buffer）。AttributeChannel.data 与索引
> 均为 shared_ptr（可共享/按身份缓存）。
>
> 2026-09-03 Geometry 通用属性缓冲已落地：`AttributeChannel{data(floats),components}` + `setBuffer(loc)`
> /clearBuffer/hasBuffer/buffer/bufferLocations（loc≥2 自定义通道，0/1 保留给类型化 positions/normals）；
> 供点云色/尺寸等自定义 shader 通道的数据模型，CPU 可测（GeometryTest 7/7）。渲染消费仍后置。
>
> 2026-09-03 拓扑归位修正：`Geometry` 移除 PrimitiveType（曾误放），改为 **渲染状态项 `Topology`**
> （默认 Triangles；StateNode set/clear；与 PolygonMode 分清：点云=Topology::Points，线框=
> PolygonMode::Line）——同一数据可换状态变三角/点/线框，不换几何（vsg/Vulkan 拓扑属管线）。
>
> 2026-09-03 Geometry 纯数据化完成：移除 `shape_`/`shape()`——Geometry 只存 buffers
> （positions/normals/indices + revision）；新增转换器 `geometryFromShape()`（**2026-09-11：**
> `setShape(Shape)` 已删除——与转换器重复、非 Mesh 形状（Sphere/BRep）静默清空几何且连带清掉
> 自定义 loc 通道、签名以 `intrusive_ptr` 暗示持有却不持有；就地（重）填用
> `setPositions/setNormals/setIndices`）；SceneBridge（缓存键改 revision、建几何读 buffers）与
> RayIntersection（meshOfGeometry）已切 buffers；bbox/计数全从 buffers。⚠ 语义变化：不再借用
> Shape 的 Aabb 缓存（测试改名 BoundingBoxComputedFromBuffers）。未做：通用 setBuffer(loc)/Buffer
> 容器（留点云/Geometry 叶子切片）。注：test_vsg 的 ctest 在进程退出期 SegFault（用例全 PASS 后、
> 直跑正常）——疑为插件卸载既有问题，与本改动无关，待另查。
> 2026-09-03 Geometry 数据面（additive）已落地：raw `positions`(loc0)/`indices`；无 Shape 时
> vertexCount/bbox 回退 positions；Shape 主路不变。⚠ 与 graphics-scene-graph.md 草图偏差：暂用
> 类型化 `setPositions`（loc0），未引入通用 `setBuffer(loc)`/Buffer 容器——留到 Geometry 变叶子/点云
> 切片再定。renderer 仍只消费 Shape 路（raw 数据面待后端切片）。
>
> 2026-09-03 State 切片已落地：`StateNode` + 状态项（Depth/Cull/Blend/PolygonMode/**Topology**）
> + 折叠函数（collect/resolve/effectiveRenderState）；`RenderCommand.renderState` 已由 collect 折叠
> 填入（Scene 集成 3 用例全绿）。未接后端变体键（下一步）。
>
> 2026-09-03 场景图重构设计（评审稿）：`graphics-scene-graph.md`（Node→Group/MatrixNode/StateNode，
> Geometry 叶子 Node，去 Drawable/Shape，loc0=position）、`graphics-state.md`（StateNode 子树状态/
> 继承）、`graphics-shader.md`（用户写 GLSL 薄接口，取代 vine-shader.md §11）。分阶段落地。
>
> 2026-09-03 方向确认（SDK 第一准则）：**用户必须能写 GLSL**；SDK 着色契约先于后端，vsg（乃至自写
> Vulkan）只是可替换实现；内置 ShaderPreset 与用户 Program 同一契约；多 pass 意义 = pass 级 Program +
> 命名产出槽。契约草案（声明式 VineFrame/VineDraw 块、属性 location 表、pass 槽）见
> `.ai/design/vine-shader.md` §11（push 矩阵机制标注待契约化修订）。
>
> 2026-09-03 shader 设计稿：自写 VS/FS + UBO ABI（push 矩阵 + set0 帧/光 + set1 材质；世界空间光照；
> FlatShaded=unlit flag；离线 SPIR-V 内嵌，无 glslang）。设计见 `.ai/design/vine-shader.md`（待 P0 落地）。
>
> 2026-09-03 更新：渲染后端(vsg)走向"自定义着色器路线"——语义层(材质/光照/阴影/着色)
> 全部归 graphics，vsg 仅保留工程层(窗口/交换链/命令/管线构建/录制)。设计见
> `.ai/design/vsg-custom-shader.md`。同时补齐 vsg 资源生命周期闭环：
> `RenderBackend::releaseOverlay/releaseRenderTarget`(默认空实现) +
> `RenderEngine` 删除点接线(removeOverlay/clearOverlays/removePass/clearPasses/shadow 剪枝) +
> `VsgRenderer` 摘除 overlay View / offscreen graph / PiP slot 并 deviceWaitIdle。
> 遗留：Material 缓存释放未接线、Scene 几何曾经靠 600 帧懒驱逐（**2026-09-14 已改为“外侧放手即回收”**）、Object 销毁钩子(自动兜底)未做。
> 测试：GraphicsTest 82 全绿（17 套件）。
>
> 2026-09-03 二更（Design B）：RenderEngine **不再有 main pass**，也不再自动建 scene/camera/pass。
> 引擎空启动：`scene()` 为可选默认内容(未绑 content 的 pass 用它，可 null)；`masterCamera()` 为
> 可选的交互主相机（manipulator 驱动它，不设则无效）。pipeline 完全显式——所有 pass 进统一
> `passes_` 注册表按 order 升序执行 + overlays 最后；"窗口 pass"= camera==masterCamera 且
> RT==null 的注册 pass（约定 order 0）。旧 `setCamera/camera`→`setMasterCamera/masterCamera`，
> `setMainPass/mainPass` 已删。RenderControl(fw) 作为"普通 viewer 引导层"：ctor 注入默认
> Scene+masterCamera，init() 在 passCount()==0 时注册默认窗口 pass。
> **阴影子系统已整体移出 RenderEngine**（addShadowPass/runShadowPasses/ShadowSlot/auto castShadow
> 调度全删，82 测试）：阴影 depth pass 现在就是普通注册 pass（光相机 + depth-only RT + order<0 +
> content）；Light::castShadow/ShadowSettings 保留为语义标志。
> ⚠ **路线决策**：shadow **排到最后**实现——前置 = 自定义 shader(buildVineShaderSet P0/P1) + 多 pass
> 完全成熟；届时才消费 castShadow/ShadowSettings。此前不跑任何半成品阴影：已剥除 vsg 内建
> HardShadows 探针(buildLightNode) + shadow-state 诊断 + VINE_VSG_SEED_LIGHTS_ONCE，demo sun 不再
> castShadow(VINE_VSG_NO_SHADOW 已删)。App 冒烟 exit=124。
>
> 2026-09-03 三更（着色预置）：graphics 加语义枚举 `ShaderPreset{StandardPhong, FlatShaded, Pbr,
> ShadowedPhong}`（`ShaderPreset.hpp`），由 RenderEngine（渲染配置）持有并在 initialize 前转发
> 后端（`RenderBackend::setShaderPreset` 默认 no-op）。vsg 后端映射：StandardPhong→phong ShaderSet、
> FlatShaded→flat ShaderSet（二者 "material" 描述符都是 PhongMaterialValue，SceneBridge 材质路径
> 通用，已验证）；Pbr/ShadowedPhong **预留**（Pbr 需 PbrMaterialValue，shadow 排最后）→ 暂回落
> Phong。builder 不掺和（preset 是着色轴，非 pass 拓扑轴）。GraphicsTest 84 全绿（+1 转发测试）。
>
> 2026-09-04 **Overlay 类删除 + 单列表统一**：`Overlay`（sdk + addOverlay/removeOverlay/clearOverlays）
> 整体删除；`RenderEngine` 只留统一 `slots_` 有序 pass 列表，`Slot{pass,content,order}`。顶部/HUD 层 =
> 高 order 普通 pass；内容经 addPass 绑定；显隐=RenderPass::setEnabled；子视口重排=新
> RenderPass::onSurfaceResized（引擎 resize 对每个 pass 调）；相机跟随=新独立 `CameraMirror.hpp`
> （`MirrorMode` + `applyCameraMirror(dst,src,mode)`）。`AxisGizmo` 改 `: public RenderPass` 自包含
> HUD pass（owned camera_/content_，execute 先镜像再画自己内容）。`RenderBackend::releaseOverlay`
> →`releaseWindowLayer(Camera*)`；`RenderEngine` 新增 `hasWindowPass()`（RenderControl 自动主 pass
> 条件由 passCount()==0 改为 !hasWindowPass()，只加 HUD pass 不再挤掉主视图）。后端 Checkpoint1 已把
> 主/叠加视图统一为 `window_layers`(Camera* 键)；释放主层时清空别名 vsg_camera/vsg_scene。
> 测试：GraphicsTest 113 全绿（HudPassTest/CameraMirrorTest/AxisGizmoTest 新语义）；test_vsg 10 绿。
> 设计稿见 .ai/design/graphics-overlay.md。
>
> 2026-09-04 C6（vsg 后端 Target 统一 + 去绑定，见 .ai/design/vsg-target-unification.md）：
> C6.1 三桶(window_layers/offscreen/screen_slots)→单表 `targets[RenderTarget*]`（nullptr=窗口，行为等价）；
> C6.2 `create()` 无参（不再绑 Vine Scene/Camera），主层惰性创建，主/顶(HUD) 由 `clear()` 标记判定
> （清屏→depth-on 主层；否则 depth-off+ambient 顶部层），窗口 RenderGraph init 空建、层随 render 加入；
> C6.3a `RenderPass::setProgramOverride`（逐 pass 整帧换 program）；C6.3b 窗口层键 (camera, content slot)
> `WindowKey` + `RenderPass::contentSlot`/`setContentSlot` + `releaseWindowLayer(camera, slot)`，
> 同相机非零槽=各自保留层顺序叠画（复用窗口图多 View），槽0 行为不变。
> 单测：GraphicsTest 114 全绿；test_vsg 10 绿；全量构建 0。App 冒烟（C6.2b/C6.3b）用户已确认正常。
> 遗留：逐槽 depth 策略(≤/write-off)、slot>0 运行期 demo、gfx_backend_vsg.md §7/11/12 旧文改写、C6.4 离屏多槽。

**模块职责**：场景图管理、可视对象、相机视图、渲染抽象层

## 核心类关系

```
Object
  ├─ Drawable (可绘制对象基类)
  │   └─ Geometry (网格/BRep/基本体)
  ├─ Material (材质: 颜色、纹理、光泽)
  ├─ Scene (场景容器: 树形结构)
  └─ View (相机: 投影、变换)
```

## 主要 API

### Scene（场景）
- `addDrawable()` / `removeDrawable()` — 树形管理
- `findDrawable()` — 名称查询
- `boundingBox()` — 递归计算边界
- `collectRenderCommands()` — 收集渲染指令

### View（相机）
- 参数：eye, target, up, near/far, FOV, 宽高比
- `viewMatrix()` / `projectionMatrix()` — 矩阵计算
- `screenToWorldRay()` — 拾取射线

### Drawable（可绘制对象）
- `localTransform()` / `worldTransform()` — 层级变换
- `isVisible()` — 可见性
- `material()` — 材质绑定
- `boundingBox()` — 局部 AABB

### Geometry（几何体）
- buffers：`setPositions/setNormals/setIndices`（loc 0/1 + 索引）、`addBuffer(loc, ...)` 开放通道
- `geometryFromShape()` — `Shape` → Vertex data 的唯一转换入口（非 Mesh 返回 null）

### Material（材质）
- RGB 颜色：diffuse, specular, ambient
- 参数：shininess, opacity (透明度)
- 可选：纹理文件路径

## 边界框类型（Aabbd）

- `Scene/Node/Drawable/Geometry::boundingBox()` / `computeBoundingBox()` 返回
  `vn::math::Aabbd`（`Rect3<double>` 别名，见 `vine/math/Rect3.hpp`）。
  原来的 `vn::graphics::BoundingBox` 已移除。
- **语义**：`Rect3` 默认构造为零点在原点的合法零盒；累积式构建必须用
  `Aabbd::empty()`（反转哨兵）起步，再用 `expandBy(Point3/Vector3/Rect3)`。
  空盒 `isEmpty()==true`、`isValid()==false`。
- **注意**：`Rect3.hpp` 只前向声明 `Point3/Vector3`，`min()/max()/center()/size()`
  的调用方需自行 include `vine/math/Point3.hpp`/`Vector3.hpp`；
  `vn::math::Point3d` 别名仅定义于 `Point3.hpp`。

## 设计特点

✓ **引用计数**：所有核心类（含 `CameraManipulator`）继承 `RefCounted<T>`，用
  `intrusive_ptr` 管理
✓ **借用/所有权分界**：getter 返回与“不 retain”的借用入参用 `vn::raw_ptr<T>`；
  会 retain（存入 owning 字段/容器）的 setter/add 入参用 `intrusive_ptr<T>`
  （by value + std::move，如 `setMaterial`、`Node::addChild`、`setScene`）；
  引擎持有操纵器经 `setCameraManipulator(intrusive_ptr<...>)`，`cameraManipulator()` 返 `raw_ptr`
✓ **Pimpl**：数据隐藏，二进制兼容性
✓ **无环依赖**：只依赖 Core、Global、Geometry；不反向依赖
✓ **后端抽象**：`RenderBackend` 接口支持多个实现（OpenGL/Vulkan）
✓ **命名规范**：无 `get` 前缀，`is`/`has` 布尔前缀，`set` setter

## RenderEngine 有序场景通道管线（2026-09 落地）

- 每帧执行：`pre passes(order<0)` → `main pass(order 0)` → `post passes(order>0)`
  → `overlays(升序 zOrder)`。
- `addPass(intrusive_ptr<RenderPass>, int order)` / `removePass(raw_ptr)` /
  `clearPasses()` / `passCount()`；同 order 稳定按插入序。
- **内容关联由 Engine 管理**：`addPass(pass, content, order)` 绑定显式场景，
  `bindPassContent` 重绑、`contentOf` 查询；执行按 `slot.content ?: engine.scene_`
  解析 → `setScene()` 对未绑定 pass 是单点更新。`RenderPass` 不携带 Scene。
- `RenderPass`：view(Camera)=借用(raw_ptr)；**输出 RenderTarget=持有(intrusive_ptr)**
  （析构在 .cpp 出外联）；null target=backbuffer；clear/viewport 在 pass 上。
- Overlay（HUD）始终最后；不 addPass 时行为与旧版一致。
- 未来 light：光源挂 Scene；shadow map = 以光源相机渲染的 order<0 通道。
- 衔接/数据传递（设计）：pass 输出=RenderTarget 纹理；将来后处理用“命名产出槽”
  (publish/resolve) 由 Engine 连接；当前阶段只需顺序 + 各自输出 target。
- v2a 平台层(2026-09-03)：RenderTarget 增 hasColor/hasDepth/colorFormat/depthFormat/valid()；
  RenderBackend 增 supportsRenderTargets()+离屏契约。vsg 离屏 scaffold 已实现（color±depth +
  createRenderPass/Framebuffer/RenderGraph + SceneBridge 同步，编译通过，GPU 未验证；默认路径不变），
  采样/合成属 v3。
- v2b(2026-09-03)：FrameContext 骨架(dt/尺寸, engine.frame/Resize 填充, frameContext() 暴露)；
  vsg 离屏 resize 时 deviceWaitIdle+从 CommandGraph.children 摘旧图重建；app_shell 提供
  VINE_VSG_OFFSCREEN=1 离屏验证入口（默认关）。
- v3(2026-09-03, lavapipe 实测)：命名产出槽 publish/resolve 落地 —— RenderPass
  setOutputName/addInputName + resolveInputTextures + execute 改 virtual；新 ScreenPass(默认不清屏)；
  RenderBackend.drawScreenTexture(source)；Engine 每帧帧首清 outputs_ 注册表、逐 pass
  执行前 resolve 输入/执行后 publish 输出（public publish/resolve/unpublish）。vsg：离屏
  renderpass 用 color finalLayout=SHADER_READ_ONLY+external 读依赖（makeSampleableRenderPass），
  离屏图插 command_graph 队首先录；drawScreenTexture 内嵌 GLSL(ShaderCompiler) 全屏纹理三角作主
  render_graph 第二 View 画 PiP（超面自动缩锚右下）。教训：ShaderCompiler 链接要求 VS/FS 接口变量
  同名；新增 View compile 失败须先从 render_graph 摘下再弃。验证：VINE_VSG_OFFSCREEN=1 右下 PiP
  正确显示离屏四色方块(偏暗=仅环境光)；GraphicsTest 68 全过(新增 5 用例)。
- v4a(2026-09-03, lavapipe 实测)：光源归属定案 **Scene 级**(不与 pass/node 绑定；node 级留 v5 升级,
  scene->lights() 换实现即可)。新增 Light(Ambient/Directional, Colorf+intensity+castShadow 预留)/
  Scene 光槽；RenderBackend.setLights(no-op 默认) 由 RenderPass::execute 在 render() 前从内容 scene
  下发(空=保留后端默认 headlight/ambient, 有光=替换)；vsg 主视图手工化(RenderGraph::create(window,
  main_view)+main_light_group(默认 createHeadlight)+vsg_scene)以拿 View 句柄挂/换灯, setGroupLights 每帧
  reconcile 各视图灯(灯节点无 GPU 资源, record 时收进 lightData, 无需重编译)。demo(env 门控) 给 engine
  scene 配 ambient0.25+sun 方向光 → 主/离屏同源、PiP 颜色与主一致(不再偏暗)。教训：vine String 不能赋给
  vsg std::string name。GraphicsTest 73 全过(新增 Light/Scene/RenderPass 传光 5 用例)。
- v4b-1(2026-09-03, CPU+lavapipe)：阴影=Scene 级光的属性+按需。Light::ShadowSettings(res/bias/filter)。
  Engine runShadowPasses()(帧首扫 castShadow 方向光→光正交相机(AABB 取景)+仅深度 RT+内部 pass, 先于主
  管线; 空 AABB/无消费跳过; 帧末释放) + 手动 addShadowPass(light,content)(显式注册, 自动让位, 不必
  castShadow)。vsg renderOffscreenTarget 支持仅深度目标(makeDepthOnlyRenderPass: storeOp=STORE+
  finalLayout SHADER_READ_ONLY+external→fragment 读依赖); clearValues 逐附件手填(勿用 setClearValues,
  其按 finalLayout==DEPTH_STENCIL 判型会误判 SHADER_READ depth)。实测日志见 1024x1024 shadow target
  先于颜色离屏, 无崩溃; GraphicsTest 77 全过(+4)。v4b-2 采样(自建 shadowed Phong)待做。

## 实现计划

1. **框架** → 头文件定义 + 空实现
2. **场景管理** → 树形结构 + 变换层级
3. **视图管理** → 相机矩阵 + 投影
4. **集成几何体** → Geometry 包装 Shape
5. **渲染后端** → OpenGL/Vulkan 实现


## 验证口径：self-test 的判据是 45 行 `[selftest]`（2026-09-12）

- **判据**：`scripts/vsg_selftest_evidence.sh`（配 `scripts/vsg_selftest_evidence.txt` 基线，`--update` 刷新）
  —— 只比 `[selftest]` 证据行、**逐字节**。**不要**拿整份 stderr trace 比：trace 行带 `file.cpp:line`（任何行数变动都会重写）
  且按帧数计（`retired (detached)` / `EXPERIMENTAL ... attached`）。
- **实测**：`retired (detached)` 行数在**一致构建**下 **HEAD 与当前工作树都是 25**（45 行证据完全相同）；一次记录的基线里是
  26 —— 那是**构建产物不一致**（plugin `.so` 与 `libviGraphics` 版本不匹配）的那次运行留下的，不是行为变化。
- **教训**：①数字比较用**整文件 + 程序统计**，别读终端里被换行/渲染过的行内数字；②回退实验必须**重建到齐**
  （`ninja <target>` 不会重建运行时 `dlopen` 的 plugin `.so` 与 SDK 动态库 ⇒ 假结果）；③`publish()` 是**宿主常驻绑定**
  （`host_outputs_`，跨帧有效），pass 输出是**本帧**账（`outputs_`）——两账分开，见设计文档 §14.7。

## 纹理对象化：`graphics::Texture`（2D + Cube）+ `Material` 迁移（2026-09-12）

**动机**：`Material::textureFile()` 那个路径字符串在 `src/` 里**零调用**（只有一个测试引用）—— 是个死 API，
也正是 `ImageRef.hpp` 注释里承认的洞（*"a material texture is a file PATH"*）。
VSG 侧 `docs/data-flow.md` 早已记着"纹理/uv 均未接线"。

**分工（拍定）**：CPU 像素 / 图像数据 → **新模块 `imaging`**（`Image` + `PixelFormat`，叶子，只依赖 Core/Global）；
GPU 资源（format/mip/sampler/usage）→ `graphics`。
自问："这东西在 headless、无渲染器、无 Qt 的环境里有意义吗？" 有 → `imaging`。
**否掉的方案**：① `Image` 放 `graphics`（会让 `meshio` **依赖渲染器**；且 `graphics → Geometry` 说明顶点数据在其
**下面**，像素数据不该在上面）；② 改名 `window`→`display` 再放 `Image`（名字硬凑；改名要同步短名/目录名/宏名/别名
**四处**，漏一处 Windows 上 `dllimport` 自炸）。详见 `.ai/design/imaging-design.md`。

**落地**：`graphics::Texture` = **逻辑描述**（与 `RenderTarget` 同一个模式，**不持 GPU 对象**）
+ 每 face 一张 `imaging::Image` 源图；`Shape{D2,Cube}`（**只做这两个**，不做 1D/3D/array）；
源图必须与描述在 format/size/mipCount **三项都一致**（不一致在**调用处**就拒绝，不让后端到上传时才发现）；
mip 上限**复用** `imaging::Image::mipCapacity`；写越界 face 抛 `std::out_of_range`、读越界 face 返回 null
（查询不是错误）。`graphics` 现在 **PUBLIC 依赖 `vn::Imaging`**。
`Material`：删 `textureFile()/setTextureFile()`（死 API），改 `texture()/setTexture()`。

**判据**：`test_graphics` **191 → 200**（+9 = 8 个 `TextureTest` + 1 个 `MaterialTest`）；
全量 `ninja` 零 error/零 warning；`vsg_selftest_evidence.sh` → PASS（**45 行逐字节相同**，后端零改动）；
`gfx_lavapipe_check.sh` → PASS（0 VUID）；`check_diagnostic_formats.py` → 0 suspicious。

**仍未做**：后端**不消费** `Texture`（face/mip 链不上传、不建 sampler、不进描述符集）；
图像解码 / 读回路径未有；`RenderTarget` 自己的 `ColorFormat`/`DepthFormat` 与 `imaging::PixelFormat` **重复**，
应并入后者（改公开 API，需单独一批）。

## 顶点存储去重：`core::Buffer<T>` + 元素类型钉死 float/uint32（阶段 1–3，2026-09-12）

**动机**：`Mesh` 用 `std::vector<T>` 存顶点，`Geometry` 又用 `shared_ptr<vector<float>>` 存一份 packing
副本（`packVec3` / `packVec2`）。对 `Vec3f` 这类元素，packing 的字节与源数据**逐字节相同** ——
那份副本不是转换，是纯开销。让两侧指向同一块分配不需要任何转换代码。

**关键一步（方案中途改过方向）**：第一版保持 `Buffer<T>` 泛型，于是通道要持住“某种 buffer”
就必须类型擦除（`Buffer<T>` 是模板、`RefCounted` 是 CRTP 无公共基类）；绕法是
`shared_ptr<const void>` + 空 deleter **再配一份裸指针 + 长度的快照** —— 而那个快照留下了真实的
悬空尖角（源 buffer 增长会让通道指向已释放内存）。正解是**把元素类型钉死**：属性就是 float
（位置/法线 3 个、UV 2 个）、索引就是 uint32。不用泛型之后，`AttributeChannel` 直接存
`intrusive_ptr<const Buffer<float>> values + uint32_t components`，**每次访问现取** ——
擦除、快照、悬空尖角一起消失，没有新增 core 类型、没有 vptr。

**其余做法**：`core::Buffer<T>`（`RefCounted`，**组合**而非派生 `std::vector` —— 派生会被
`vector&` 传递切片掉引用计数）；两个面 `view()` / `bytes()`。**变更一律手动公告**（2026-09-13
把 `push_back`/`append`/`clear` 里的 `++revision_` 也删了）：buffer 看不见 `data()`/`operator[]`
这类可写引用，也不知道一次编辑何时结束，自 bump 只能是半真话；写的人改完调 `setRevision(revision()+1)`。
`Mesh` 就是那个写的人：`addVertex`/`addTriangle`/`clear` 各经 `announceChange()` 公告一次，所以
“共享句柄能得知编辑”这条契约不变。`Mesh` 存
`Buffer<float>`，`positions()` 等仍返回 `span<const Vec3f>`（同一批字节 reinterpret，布局由
`Mesh.cpp` 的 `static_assert` 钉住），另给 `positionsBuffer()` 等共享句柄。
**属性 setter 每个通道只留一个**：`setPositions/setNormals/setTexcoords2` 各收一个 buffer 句柄，
不重载（原先的 `setPositionsBuffer` 与数组重载已删）；借用形式改走 `packAttribute(span)` 工厂，
于是“复制还是共享”由**传什么**决定。`geometryFromShape()` 走共享，
**所以 mesh 与 Geometry 读同一块分配，顶点不再翻倍**。

**判据**：`test_graphics` **219 → 221 → 223**、`test_core` **82**、`test_vsg` **185 → 190**；
`ninja` 零 error/零 warning；`vsg_selftest_evidence.sh` → PASS（**47 行逐字节相同**）；
`gfx_lavapipe_check.sh` → PASS（0 VUID）；`check_diagnostic_formats.py` → 0 suspicious。
**判据里最有信息量的一条**：阶段 2（换存储）、3b（改走共享）、3c（钉死类型 + 合并 API）之后
证据**都是**逐字节相同 —— 渲染输出一个字节没变，读的却已经是另一套存储。
阶段 4 更进一步：后端**真的改了代码**（位置通道从拷贝改成别名），证据**仍然**逐字节相同 ——
这才是“渲染侧读的是模型内存、而渲染结果零变化”的正证据。
**变异验证**：`addVertex` 每次重建存储 → 3 条共享断言失败；`geometryFromShape()` 改回 repack →
3 条指针同一性断言失败（分量/坐标/计数全过）；`AttributeChannel` 加回快照 → 3 条增长断言失败；
keepalive 换成空 lambda → `useCount()` 断言失败；阶段 4：别名 offset 跳一个顶点 → **证据 FAIL**
（证明别名真被读，不是悄悄走回退路径）；关掉别名分支改回拷贝 → 指针同一性断言失败
（拷贝渲染得逐字节相同，**只有地址能区分**共享与复制）—— 改成“复制进自己的一份 buffer”的全局变异
⇒ **恰好 5 条地址/生命周期断言红、证据仍 PASS**（渲染关口对拷贝 vs 共享完全无感）。

**阶段 4（顶点别名）已落地**：位置通道不再逐顶点拷进 `vsg::vec3Array`，而是用
`vsg::Array(storage, 0, sizeof(vsg::vec3), n)` **别名**一个持有模型 buffer 的 `Data`
（`detail::VsgBufferView<float>`，只做存储那一半）。**踩过的坑：自己写 `vsg::Data` 子类当顶点数组会静默
不画** —— 所有 properties 报得与同形状 `vec3Array` 一模一样、validation 干净，就是不出图；被绑定的**必须**
是真 `vsg::Array` 类型，元素类型仍是数组的，所以 format/stride 仍自动推断（没有手写）。
其余通道（法线 / texcoords / 4 分量颜色 / 自定义 / 索引）**已用同一机制落地**：`detail::aliasArray<Array, Element>` 是唯一入口，
`geometry->indicesBuffer()` 让索引也能别名；拷贝版 `makeTypedVertexData` 已删。
推导量（白色 opacity 载体、零填充 texcoords、`makeNormals` / `makeIndexedNormals`）仍自己分配真数组。
**关键坑**：别名数组的 stride 是**数组自己的元素大小**（vec3 → 12），不是 buffer 的（float → 4）；vsg 用 `properties.stride`
同时索引 CPU 侧与 GPU 绑定，取错会让 GPU 交错读 —— 证据关口与新单测同时抓到（判据已钉 `properties.stride == sizeof(vec3)`）。

**手动 revision（同一批）**：`Geometry` 加 `setRevision(uint64)`（像 `Buffer` 那样由调用者给值），并且**把 setter 里的
`++revision_` 全删掉** —— 公告一律手动。理由是共享之后 geometry **借**模型的 buffer：它分不出“没读过的新字节”与
“上次读过的旧字节”（并不复制），而渲染侧重建闸门就是 `geometry->revision()`（`SceneBridge.cpp:350`）⇒ 只有知道
数据变了的人能说这句话（重建模型的一方、换 buffer 的一方）。忘记公告不报错，只是静默沿用旧字节；首次构建不受影响。
判据：`ManuallyReportedRevisionRebuildsTheDataNode`（公告后顶点数据刷新、变换节点与状态包装不变）
+ 设备无关的 `RevisionCanBeReportedByHand`；变异（`setRevision` 改空操作）⇒ 恰好这两条红。
**连带改动（也是完备性检查）**：写 setter 不再触发重建 ⇒ 6 处“改数据后期望重建”的测试要改成显式 `setRevision`
（`DataOnlyRebuildLeavesStateUntouched`、“everything changes at once”、`FixedDataRevisionRebuildsRejectedGeometry`、
`ReplacedDataNodeIsParkedUntilTheRingAdvances`、`LiveChannelAddForcesStateRebuild` ×2），第一次跑正好挂出这 3 个
没被 grep 到的漏网（拒绝重试那两处也是靠 `rejected_revision` 才生效的）。
索引已一并收掉（阶段 3d）：`Geometry::indices()` 返回 `std::span<const uint32_t>`，`setIndices` 亦只收
buffer 句柄 + `packIndices()` 工厂，`geometryFromShape()` 共享索引 ⇒ **索引也不再复制**。
第一版被推翻的过程、setter 合名的理由、以及预测与实际破坏点清单的差异，
详见 `.ai/design/geometry-attribute-storage.md`。

## C1 落地：宿主表面归宿主（附加 / 搬移 / 绝不销毁，2026-09-16）

- **前提确认**：宿主的 `RenderControl::initializeBackend()` 在句柄变化时 `shutdown()` + `initialize()`，而 `setWindowHandle()` 的语义本就是"搬"；`VSG_MAX_DEVICES=4` 与 `releaseWindow()` 都只是这个症状的补丁 ⇒ 前者**撤回**、后者**不再需要**（调用点保留但已成空操作）。
- **新窗口类** `detail::VsgHostWindow`（`VsgHostWindow.hpp/.cpp`）：`vsg::Inherit<vsg::Window, ...>`，实现 `_initSurface()` + `instanceExtensionSurfaceName()`（X11 `VK_KHR_XCB_SURFACE_EXTENSION_NAME`／Win32 `VK_KHR_WIN32_SURFACE_EXTENSION_NAME`，Win32 分支本机只编译验证）。自持 `xcb_connect`，**采纳** `traits->nativeWindow`，`_initSurface()` 用 `new vsgXcb::Xcb_Surface(...)`（无 `create()`）；`moveToHostSurface()` 丢弃 swapchain/frames/indices/depth/multisample/surface → 同一 instance 上重建 surface → `_initFormats()` 复核（格式变了就拒绝）→ `buildSwapchain()`；析构只 `clear()` + `xcb_disconnect`，**永不** `xcb_destroy_window`。
- **入口**：`VsgRenderer::moveSessionToHostSurface(void*)`（先 `retireRing.waitForIdle` → 计数；拒绝 null/同句柄/非本后端窗口，并报 `Warning` + `UnsupportedRequest`）；`initialize()` 的活会话分支 = **先搬、搬不动才重建**。可观测量 `windowBuildCount()`：**没新建窗口 ⇒ 没新 instance/device ⇒ 管线留着**。
- **自检相位** `selftest_hostsurface.cpp`：两个自建 X11 宿主窗口 A→B，断言 ①窗口构建数不变 ②恰好 1 次计数 device stop ③两窗口都活着 ④`shutdown()` 后宿主 B 仍活。打印 `[host-surface] ...`（**不带 `[selftest]` 前缀** ⇒ 55 行证据基线不动）。实测：`windows built 2 before, 2 after; 1 counted device stop(s); host windows intact; the session still presented it`。**变异**（`moveSessionToHostSurface()` → `return false`）⇒ 3 builds + 0 stops，相位红，符合预期。
- **门禁**：build 0/0、`test_vsg` 289、证据 55 行逐字相同、app 演示 PASS、`RESULT: PASS — lavapipe validation clean`。
- **宿主窗口必须回答 `valid()` / `visible()`（2026-09-16 修正，重要）**：vsg 的 `Window::valid()` 默认 **false**、`visible()` 默认 `valid()`；而 `CommandGraph::record()` / `SecondaryCommandGraph::record()` / `Viewer::advance()` / `Presentation::present()` 全先问 `visible()` ⇒ 自建窗口类不覆写就**整帧不录**：窗口黑、**离屏 target 也一个像素都不写**，而且 **0 条 validation error**（"validation clean" 与 "什么都没画" 同形）。`vsgXcb::Xcb_Window` 覆写 `visible() = _windowMapped`；`VsgHostWindow` 现在覆写为"采纳窗口的 map 状态"（X11 `xcb_get_window_attributes().map_state == XCB_MAP_STATE_VIEWABLE`，Win32 `IsWindow`+`IsWindowVisible`），在附加/`resize()`/搬移处刷新、未映射时惰性重问。**实测**：Qt 渲染区 0.84% → **83.81%** 非黑（与 C1 前 `Xcb_Window` 逐位相同），窗口拉到 1498×828 后 97.10%；日志多出 `mapped=true`。
- **先前记的"窗口会话 + 离屏 target 不写入 / InvalidImageLayout 是独立未解 bug"是错的**：它就是上面这条（整帧被跳过），不是布局缺陷；自检相位因此恢复像素判据（`centre 34,6,2` vs 角点清屏色 `10,20,30`，搬移前后逐位相同；帧被跳过时两点皆透明黑）。
- **验证纪律（新踩的坑）**：`cmake --build . --target Vine` **不重建插件** `build/plugins/vine/gfx_backend_vsgd.so`（app 运行时 dlopen），只验 `bin/Vine` 会导致假阳性 ⇒ 二进制级结论要按**构建产物**验身份（`nm -DC <plugin.so> | grep <symbol>` + 时间戳）。
- **看画面本身的办法（2026-09-16 晚已改工具，见顶部 H6 条目）**：`scripts/xwin2ppm.py <窗口句柄|窗口名> [out.ppm]` —— 句柄从后端那行 `[VsgHostWindow] attached to the host window 0x…` 里取（Qt 渲染区是具名顶层窗口的**子窗**，按名字读到的是 Qt 自己的界面）；它经 libX11 `XGetImage` 自己抓、自己解掩码，**不需要 xwd/xwininfo**，再用 `scripts/ppm2png.py` 转 PNG。（旧写法是 `xwd -id … -out f.xwd` + 自解 XWD 头：`-root` 在 XWayland 下 BadMatch、`-out -` 不支持，`bpp@44 / bytes_per_line@48 / mask@56,60,64`。）
- **小经验**：`VsgRenderer::deviceWaitCount()` 是**会话级**计数，`shutdown()` 后读回 0 ⇒ 相位要在放手之前取样（打印的就是量到的值，别写死散文数字）。
- **宿主侧下一步**：`src/fw/appfw/src/gui/RenderControl.cpp::initializeBackend` 仍先 `shutdown()`；改成"只重新公告句柄 + `resize()`/渲染"，让后端的 `initialize()` 去搬（本环境除 app 演示外无门禁覆盖）。**已做（2026-09-16 晚，H4）**：宿主**两处** shutdown 都删了（`initializeBackend()` 与 `onSurfaceDestroyed()`），并加了 `VINE_RECREATE_SURFACE_MS` 钩子把它接进门禁 —— 见本文件顶部 H4 条目。

## A6 落地：`VsgHostWindow` 改成派生平台窗口（2026-09-16 完成；**Win32 分支 2026-09-17 已在 Windows 上实测，见本文件 H1 落地条目**）

- **做法**：`using VsgHostWindowBase = vsgXcb::Xcb_Window | vsgWin32::Win32_Window`（平台 typedef），`class VsgHostWindow : public ::vsg::Inherit<VsgHostWindowBase, VsgHostWindow>` **只改两件事**：① 析构 `clear()` 后把 `_window` 置空（基类析构因此不 `xcb_destroy_window`／不 `DestroyWindow`，Win32 更不会 `UnregisterClass(GetClassName(hwnd))` 去注销 Qt 的窗口类）；② `moveToHostSurface()`（丢 surface/swapchain → **继承的** `_initSurface()` 重建 surface → `_initFormats()` → 格式变了就拒 → **继承的** `resize()` 重查几何 + 重建 swapchain）。连接/屏幕/几何/surface/`valid()`/`visible()`/`pollEvents()`/XDND/`systemConnection` 全部白拿。
- **根因与收益**：黑屏 bug 的根因就是"手写平台窗口，漏了 `valid()/visible()`"（vsg 的两个平台窗口**在采纳分支里就设 `_windowMapped = true`**，所以它们对宿主的窗口本来就答对）。派生后这一整类"漏覆写虚函数"的风险消失，`VsgHostWindow.*` 从 **567 行降到 315 行**（−252，约 −44%，含两平台分支），手写的 `hostWindowFromTraits`/`hostWindowExtent`/`valid`/`visible`/`refreshHostWindowState`/`_initSurface`/`resize`/`<atomic>` 全删。
- **顺带**：`makeWindowTraits` 的句柄类型从 `unsigned int` 改成 `xcb_window_t`（vsg 用 `std::any_cast<xcb_window_t>`，std::any 要求类型**完全一致**）；头文件按平台 include `vsg/platform/.../Xxx_Window.h`，并在 `__APPLE__` 上 `#error`（CMake 只在 `UNIX AND NOT APPLE` 给 xcb）。公开 SDK 头不受影响：`VsgRenderer.hpp` 不包含这个头（只在 .cpp 与 `tests/test_vsg` 里）。
- **实测**：build 0/0；自检相位绿、0 VUID，`[host-surface] attached … (320x180, mapped=true)`、搬移 `windows built 2 before, 2 after; 1 counted device stop; centre 34,6,2 / corner 10,20,30 与手写版逐位相同`；app 日志 `attached to the host window (378x234, mapped=true)`，`xwd` 读渲染区 **85.75% 非黑**（同一动画场景，比例随帧变化）；`gfx_lavapipe_check.sh` PASS（55 行证据逐字相同、app demo PASS、0 VUID）；`test_vsg` 289 / `test_graphics` 272。
- **仍未验证/待确认**：Win32 分支本机**编译不了**（只能审读：`vsgWin32::Win32_Window` 是 `VSG_DECLSPEC`，采纳分支同样设 `_windowMapped`，析构会 `DestroyWindow` + `UnregisterClass` ⇒ 我们置空句柄是对的）；采纳路径下我们这条连接不选事件掩码 ⇒ `pollEvents()` 收不到 X 事件、不会偷 Qt 事件（已按源码确认，真机再复验一次更稳）；vsg 平台窗口构造会调 `_initXdnd()`，会在**宿主窗口**上写 XdndAware 属性（幂等、Qt 在 X11 本来也用 XDND）。

## 交接：未做的事、待确认的事（2026-09-16 收工存档，回家接着干）

> **唯一登记处**：下面这些项的正式登记在 `.ai/memory/graphics-perf-backlog.md` §1 的「宿主表面（C1/A6）未完成项」（编号 **H1–H6**，见 `gfx_backend_vsg.md` §15 的“只指向唯一登记”约定）；本节保留当天的过程、判据与命令，不另开一份登记。

> 今天已完成并提交：`424e15e` C1（宿主表面归属）→ `143c9f4`（`valid()/visible()` 修复，黑屏根因）→ `dce6946`（复核五项：删 `releaseWindow()`、同句柄保持会话、atomic、拆文档、`__APPLE__` 守卫）→ `44e6ad9` A6（改为派生平台窗口，567→315 行）。下面只列**没做完的**，每条写清"为什么没做 / 怎么判成功 / 动哪里"。

### 1. 代码写了但本机证明不了的（最该先补）
| # | 事项 | 现状 | 判据 |
| --- | --- | --- | --- |
| ~~V1~~ | **Win32 分支**（`VsgHostWindow` 派生 `vsgWin32::Win32_Window`） | **已做（2026-09-17，Windows 11 + RTX 4060）**；原先：本机 `_WIN32` 不成立 ⇒ **编译器都没跑过**，只做了源码审读（`Win32_Window` 是 `VSG_DECLSPEC`；采纳分支设 `_windowMapped = true`；其析构会 `DestroyWindow` **和** `UnregisterClass(GetClassName(hwnd))` ⇒ 我们"析构先置空 `_window`"是对的） | Windows 上：build 0/0 + app 门禁 + 自检相位（`mapped=true`、窗口构建数不变、恰好 1 次计数 device stop、两宿主窗口存活）+ 拉伸窗口看画面跟随（走 `resize()`） |
| V2 | 采纳路径下 `pollEvents()` 不会偷 Qt 事件 | 源码级已确认：事件掩码只在 `createWindow` 分支的 `xcb_create_window` 里设置，我们这条连接收不到 X 事件 | 真机上边缩放/拖拽边点菜单，确认 Qt 事件不丢 |
| V3 | 新副作用：vsg 平台窗口构造会调 `_initXdnd()`，在**宿主窗口**上写 `XdndAware` 属性 | 幂等，且 Qt 在 X11 本来也用 XDND | 往窗口拖一个文件试；若真有害，对策是"不接受 vsg 构造"或构造后清属性 |
| ~~V4~~ | ~~宿主侧 `RenderControl::initializeBackend()` 仍是 `engine->shutdown()` + `initialize()`~~ | **已做（2026-09-16 晚，H4）**：宿主**两处** shutdown 删除（`initializeBackend()` + `onSurfaceDestroyed()`），新增 `VINE_RECREATE_SURFACE_MS` 钩子把“平台窗口被重建”变成可按需触发，app 阶段断言 `moved ≥ 1` 且 `attached == 1` | 实测 `0x60004a → 0x600051`、渲染区 84.90% 非黑；**变异**（shutdown 放回 `onSurfaceDestroyed()`）⇒ 三条红。见本文件顶部 H4 条目 |
| V5 | `+0.43 s` 的 **release(-O2) 复测** | 只有 debug(-O0) 的交错 A/B；当时已排除"代码布局/堆起点"等猜测，但"交付物是否带这笔钱"仍未测 | 新开 `build-release/`（`-DCMAKE_BUILD_TYPE=Release`）全量构建，三条已提交二进制交错比墙钟；**先验身份**（`nm -C` + 指纹）再下结论 |

### 2. 已定位、未做的代码收尾（都小）
- **自检相位的魔法尺寸**：`vsg_selftest/selftest_hostsurface.cpp` 里 `at(128u, 72u, …)` 与 `256u`/`144u` 同 `pixels_target` 的尺寸是同一个事实写两遍；从 target 取尺寸或提成常量。
- **拒答路径零覆盖**：`VsgRenderer::initialize()` 收 null 句柄、公告的窗口"不是本后端的"、以及"搬移被拒（新表面 swapchain 格式不同）"这三条分支，连同 `Warning` + `DiagnosticCategory::UnsupportedRequest` 诊断，**一条断言都没有**。建议各加一条（诊断可用 `renderer.setDiagnosticSink(...)` 收）。
- **`VsgRenderer::resize(int,int)` 忽略参数**（尺寸权威在窗口，这是设计），但签名会让读者误以为值生效；可标 `[[maybe_unused]]` 或改注释。
- **app 门禁仍不看像素**：`gfx_lavapipe_check.sh` 的 app 阶段只查 stderr 证据 ⇒ **今天的黑屏被它完整放过**。工具已入库：`scripts/xwin2ppm.py`（原名 `xwd2ppm.py`；见下）。**已接进门禁（2026-09-16 晚，见顶部 H6 条目）**：app 阶段现在趁 app 活着读它的渲染区，要求非黑比例 ≥ `VINE_APP_MIN_CONTENT`（默认 30%），`VINE_APP_PIXELS=0` 可关。
- 早前会话留下的：上游 vsg 加 `CompileManager::remove(view)`（已有派生方案，**不需要了**，见 §5.3.1）；`docs/` 与 `.ai/design/` 里若要给 `VsgHostWindow` 的"派生 vs 重写"留设计记录，可在 `graphics-vsg-audit.md` 补一节。

### 3. 今天的判据 / 命令速查（回家直接抄）
```bash
# 全门禁（PASS = 55 行证据逐字相同 + content-shading 匹配 + app demo PASS + 0 VUID）
timeout 900 bash scripts/gfx_lavapipe_check.sh

# 自检 + 校验层（相位含窗口构建数 / 计数 device stop / 宿主窗口存活 / 同句柄 / 像素）
VINE_VSG_DEBUG_LAYER=1 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./bin/vsg_backend_selftest

# 单测 / 脚本门禁
./bin/test_vsg            # 289 ; ./bin/test_graphics  # 272
python3 scripts/check_doc_symbols.py       # 3 living document(s) and 62 unit(s) agree
python3 scripts/check_include_hygiene.py   # 0 finding(s) in 663 file(s)
python3 scripts/check_diagnostic_formats.py

# 看 GUI 到底画没画（今天的黑屏就是这么抓到的）
./bin/Vine &                       # 默认 demo（VINE_VSG_OWN_WINDOW=1 可看后端自建窗口那条对照）
python3 scripts/xwin2ppm.py Vine /tmp/area.ppm     # 打印非黑比例/均值色 + 写 PPM
python3 scripts/ppm2png.py /tmp/area.ppm /tmp/area.png
```

> **身份铁律（今天就栽过一次）**：`cmake --build . --target Vine` **不会重建插件** `build/plugins/vine/gfx_backend_vsgd.so`（app 运行时 dlopen 它）。任何二进制级结论都要 `nm -DC <构建产物> | grep <symbol>` + 时间戳，只验 `bin/Vine` 会得到假阳性。
> **另（2026-09-16 晚起已不适用）**：抓图工具不再用 `xwd`（`xwd -root` 在 XWayland 下 BadMatch、`-out -` 不支持、XWD 头偏移 bpp@44 / bytes_per_line@48 / RGB mask@56,60,64 都是它当年自己封的坑）；现在只依赖 libX11 的 `XGetImage`（`scripts/xwin2ppm.py`）。

## 模块文档（面向使用者）

`src/viz/graphics/docs/usage.md`（仿 `src/base/math/docs/Eigen.md` 的"模块自带 docs 目录"约定）：分层架构、属性沿树折叠的四条规则、每帧数据流与"顶点被别名而非复制"、生命周期与变更契约（**数据变更一律 `Geometry::setRevision()` 公告**）、最小宿主用法，以及现成例子的**构建与运行方式**（`VINE_PIPELINE=forward|deferred|forward_shadowed|deferred_shadowed`（默认 deferred）、`VINE_VSG_GBUFFER` / `VINE_VSG_DEFERRED` / `VINE_VSG_OFFSCREEN_MULTISLOT` / `VINE_VSG_SLOT_DEMO` / `VINE_SHADER_PRESET`、无头门禁）。
文档如实标注 **forward_shadowed / deferred_shadowed 目前是占位（等同无阴影预设）**：阴影切片（order<0 深度 pass + 阴影光照）未实现，只有 `Light::castShadow()/shadow()`、`ShadowSettings`、`ShadowFilter` 已就位，计划见 `.ai/design/graphics-shadow.md`。
**2026-09-13**：占位不再默不作声——`RenderPipelineBuilder::build` 会报一条 `DiagnosticCategory::UnsupportedRequest`；`ShaderPreset`（含 `ShadowedPhong` 那个“保留项”）已删除，内容着色只能显式命名 program（见本文件顶端条目）。

