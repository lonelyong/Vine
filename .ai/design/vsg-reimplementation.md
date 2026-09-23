# vsg 后端"从零重新实现"设计（提案）

> 状态：**设计提案 v2（2026-09-21）**，核心层已开始落地（见 §11），**不改动**现有 `gfx_backend_vsg`。
>
> **实施进度（截至 2026-09-22）**：§11 是逐片的实施记录，每片都带自己的证据面。当前已完成的最后一片是
> **M8i：一帧收成两次调用**——`api/ContentAssembly`：`beginFrame(计划)` 开块预算 + 由 store 产出表，
> `record(pass, …)` 把半片（`ContentHalves`）、声明集合（`ContentSets`）、**每 pass 一个 `StateRegistry`**
> （注册表的契约就是"这个 pass 的状态记忆"）与输入集合串成一次 `ContentPass::record`；构造时自己从设备取
> **动态状态入口点**（集成测试证明：漏了就是 12 条 VUID——动态调用被静默跳过）。真设备证据：整帧只用这两个
> 调用画出（两纹理各自采自己的图）＋**第二帧每个生产者的计数都不动**（`frames()` 确认块预算按帧开）；
> 4/4 变异反证红；`test_vsg` 636 用例 / 97 套件全绿；门禁 **0 VUID / 0 SYNC-HAZARD**、skipped=0、
> hygiene 全清（现 847 文件）。更早一片是
> **M8h：声明集合的生产侧**——`api/ContentSets`：把"每套声明集合自己建"的那段宿主代码收成一处——每个被声明
> 的采样器按**名字**取图（`diffuseMap` ⇒ 材质的纹理（缺/不可用 ⇒ 白，cube 槽给白 cube）、`shadow_map` ⇒
> pass 解析出的那张图（没有 ⇒ 白）、其余名字 ⇒ pass 的输入图，按输入集合的绑定序），每套集合按
> **(program, revision, variant, set, 图来源)** 作键复用（图来源 = M8g 的 `ImageSource`；采样器全是 pass 级的
> 集合打**空来源**，同变体的所有 drawable 共用），键离开表 ⇒ 同一窗口停靠，被拒的集合记住不重试；真设备证据：
> 与 M8g 同一幅画（两纹理、一同形状的两套集合）但**一套都不手建**，并且第二趟调用 `builds()` 不涨；
> `test_vsg` 635 用例 / 97 套件全绿；门禁 **0 VUID / 0 SYNC-HAZARD**、skipped=0、hygiene 全清（现 845 文件）。
> 更早一片是
> **M8g：一个 drawable 的图，一个集合**——同一变体的两个 drawable 的**声明集合形状完全相同**（同样的块绑定、
> 同样的采样器绑定号），所以"按形状挑第一套"会把一个 drawable 的贴图交给另一个（**静默**：形状对、图错）。
> `BlockDescriptors::ImageSource`（纹理 + 修订——正是 `MaterialImages` 的键）给集合打上"它的图来自哪版纹理"，
> pass 由 drawable 的材质事实算出**它要的**那对（纹理指针 + **实时**读到的 `revision()`），先找精确匹配、
> 再退到"图里没有纹理"的集合（采样器全是 pass 级的），都没有才按名拒绝（拒绝语分清"没人建过这种形状"与
> "建过的那些装的是别的图"）。真设备证据：**同一半片、两张纯色贴图、两套同形状集合** ⇒ 左半洋红、右半绿；
> 5/5 变异反证（M1 就是旧匹配 = 复现）。`test_vsg` 634 用例 / 97 套件全绿；门禁 **0 VUID / 0 SYNC-HAZARD**、
> skipped=0、hygiene 全清（现 843 文件）。更早一片是
> **M8f：半片的生产侧**——`api/ContentHalves`：按计划走一遍 pass，用**与录制器相同的查找**（几何按计划的修订、
> 材质按身份、变体由两者决定、程序条目按变体取）为每个 (kind, program, revision, layout, variant、
> **pass 的颜色附件数**) 产出一片**已编译的半片**（层 + 录制器，`Scope::Entry` 直接可用）；布局的绑定/格式拼写
> 收进 api（`ContentPipeline::create(abi, GeometryFacts, shaders, settings)`，夹具也改用它——一份拼写）；键的
> 表不再回答（修订被表退役）的半片按同一窗口停靠；**被拒的层记住、不重试**。真设备证据：整帧 = store 的表 +
> 生产者的半片 ⇒ 左半洋红、右半材质色、0 VUID；`test_vsg` 633 用例 / 97 套件全绿；门禁 **0 VUID /
> 0 SYNC-HAZARD**、skipped=0、hygiene 全清（现 843 文件）。更早一片是
> **M8e：三张表的生产侧**——`api/ContentStore`：宿主 `track()` 活对象（条目持键，地址不回收），
> `tablesFor(计划)` 只建**计划点到名**的东西（稳态帧 0 重建），修订是重建的闸（几何/程序读 SDK 自己的
> `revision()`，材质只有 `updateMaterial()` 报告的那一个——SDK 类型没有 revision）；被顶替的修订**留在表里**
> 直到时间线过了还可能记录它的槽（同一个 `RetirementQueue`，弱引用捕获，队列先死也不会悬空），材质例外：
> 它按身份查（计划不能点修订），所以**就地替换**、只把旧值停靠；`releaseAbandoned()` 丢"只剩存储自己"的对象；
> 没有停靠窗口时**留下并计数**（`retained()`），绝不提前释放。顺手把**程序表按变体作键**（`ProgramFacts.variant`
> + `findProgram(…, variant)`，pass 先算变体再取条目）并让两个查找**扫全表**（表里可能有被顶替的修订）；真设备
> 证据：整帧的表由 store 生产、同一程序两个变体（`VINE_DIFFUSE_MAP` 连 push 声明都门控）⇒ 左半洋红、右半材质色；
> `test_vsg` 624 用例 / 96 套件全绿；门禁 **0 VUID / 0 SYNC-HAZARD**、skipped=0、hygiene 全清（现 840 文件）。
> 更早一片是
> **M8d-2：程序的文本是好几份程序**——同一份文本按 `#pragma import_defines` 编译出不同的 ABI 与分支（引擎的
> `forwardProgram` 没有 `VINE_DIFFUSE_MAP` 时 (0,1) 处**没有**采样器、texcoord 的种类由 define 决定），
> 于是**变体的 define 进管线身份**：`api/ProgramVariant`（规则唯一拼写：恒有且只有一个 texcoord kind、
> `VINE_DIFFUSE_MAP` iff 材质有贴图、`VINE_VERTEX_COLOR` iff 几何**自己写下**颜色通道）、`PipelineKey += variant`、
> `Shaders.defines`（扫描与编译**同一份**清单，布局与模块不可能对不上）、pass 的 half 按 (program, revision,
> layout, **variant**) 匹配；像素证据：**一趟、两画、同一程序的两个 half** ⇒ 左边采到贴图的洋红、右边是材质
> 自己的 0.25/0.5/0.75（只按 (program, revision, layout) 挑 half 的实现会把贴图涂到两边）；顺手修**每趟只
> serve 第一个 content half** 的老洞（多 half 的趟会把第一个 half 的集合绑到别的 half 的画上——真设备上是
> VUID 00358+08600）。`test_vsg` 615 用例 / 95 套件全绿；门禁 **0 VUID / 0 SYNC-HAZARD**、skipped=0、
> hygiene 全清（现 837 文件）。更早一片是
> **M8d-1：材质贴图的 GPU 侧**——`api/MaterialImages`（一张 texture 的 image/view/sampler，按**地址 + 修订**缓存、
> 条目持住键、白 fallback 按**种类**给 2D/cube、超限淘汰、放弃即释放）；像素证据：四色 2x2 贴图 → 画面四象限、
> 六色 cube → 五个方向上的五个面（层序错是**静默**的，只有像素能抓）、重填后第二趟是新颜色。更早一片是
> **M8c-4：全屏集合就是程序自己的声明**——引擎的 `shadowedDeferredLightProgram` 把 `shadow_map` 写在
> binding 5、`VineShadowBlock` 写在 6（源的四张颜色占 0..3、源无可采样深度），全屏路径按**声明号**装集合；
> 引擎带影/无影两个变体在引擎自己格式的 G-buffer 上端到端出像素；顺手修了**动态命令一直开着混合**（多附件
> pass 的每个附件都被自己的 alpha 缩放——L2 量过的同一件事在新路径上重演）；`test_vsg` 598 用例 / 93 套件全绿；
> 门禁 **0 VUID / 0 SYNC-HAZARD**、skipped=0、hygiene 全清。更早一片是
> **M8c-3：`shadow_map` 按名字到达程序声明的 binding**：`api/ContentImages`（“哪个名字的图从哪来”的策略唯一拼写：
> `diffuseMap` → 材料/白、`skyMap` → 环境（未落地 ⇒ 建层时拒）、`shadow_map` → pass 解析出的那张 map、
> 其余名字 → pass 的输入）+ 内容路径的真设备像素（同一程序三种输入：有 map ⇒ 读到 0.25、无 map ⇒
> 白色替身 ⇒ 材料的颜色、程序不声明 map 而 pass 声明了 ⇒ 画照画、报一次）；顺手修了 **material HIT 返回 offset 0**
> 的稳态帧缺陷（`BlockStorage::writeMaterial`）；`test_vsg` 595 用例 / 93 套件全绿；门禁
> （`scripts/vsg_rewrite_gate.sh`）：**0 VUID / 0 SYNC-HAZARD**、hygiene 全清（0 / 831 文件）、相位 9 行 / 2 次运行。
> M7 完整（身份半边 §11.16ac、读数字半边 §11.16ad）；**场景桥已开工**：M8a 读事实（§11.16ae）、M8b 布局跟着
> 声明（§11.16af）、M8c-1 相机 push（§11.16ag）、M8c-2a 混合集合（§11.16ah）、M8c-2b 白色 fallback（§11.16ai）、
> M8c-3 名字即来源（§11.16aj）、M8c-4 全屏集合即声明（§11.16ak）、M8d-1 材质贴图的 GPU 侧（§11.16al）、
> M8d-2 变体的 define 进管线身份（§11.16am）、M8e 三张表的生产侧（§11.16an）、M8f 半片的生产侧（§11.16ao）、
> M8g 一个 drawable 的图一个集合（§11.16ap）、M8h 声明集合的生产侧（§11.16aq）、M8i 一帧两次调用（§11.16ar）、
> M8j 丢掉的提交下一帧修一次（§11.16as）、M8k 提交这一步自己说它失败了（§11.16at）、M8l 形状变了就是真的重建（§11.16au）、
> M8m 帧驱动应用计划的答案（§11.16av）、M8n 会话的提交也自己说（§11.16aw）、M8o 窗口路径一次调用（§11.16ax）、
> M8p 每帧换图不再拆机器（§11.16ay）、M8q 丢帧的会话自己活下来（§11.16az）、
> M8r 计划说的形状连格式一起核对（§11.16ba）、M8s 窗口的答案也有执行者（§11.16bb）、
> M8t `skyMap` 是 drawable 自己的图（§11.16bc）、M9a 门面立起来（§11.16bd）、M9b pass 协议接到内容层（§11.16be）、
> M9c 离屏那一半（§11.16bf）、M9d 读回（§11.16bg）、M9e 活着的 resize（§11.16bh）、
> M9f 工厂切到门面（§11.16bi）。
> 下一步：
> **收尾**：旧实现退场（名字已换，`VsgRenderer` 一路的源与测试是树里的死代码，删它是独立一步），
> 或挑一条 §11.17 登记的口子做。
> 场景桥的登记项（`skyMap`）至此清空；其余遗留口子见下。
> 其余遗留口子：集合与半片停靠窗口各自独立；窗口 `facts()` 的 live 采样与 `refresh()` 的成功臂今天没有可驱动的触发（登记）；
> 设备半边在"某一侧没说"时跳过（登记）。
>
> v2 修订：按一份外部评审（20 条）重钉了 10 个 P0 定义（见 §2.5），改了架构图（§2.1 两个流 +
> §2.2 六个对象 + §2.3 物理边界），并按评审重写了键的拆分（D3）、资源寿命（D4/D5）、
> 以及循环依赖与 normalize 的语义。评审逐条处理见 §6.2。
>
> 定位：本文回答"如果把 `gfx_backend_vsg` 重做一遍会怎么设计"。**功能面与现有实现等价**
> （§4 是逐条对照表），**内部组织刻意不同**（§6 列出差异与代价）。读"今天怎么做"请看
> `src/plugins/gfx_backend_vsg/docs/backend.md` 与 `docs/data-flow.md`；本文只描述"我会怎么做"。
>
> 前置阅读：`src/viz/graphics/sdk/vine/graphics/RenderBackend.hpp`（契约原文）、
> `ShaderAbi.hpp`（着色 ABI）、`.ai/design/vsg-pass-lifecycle.md`（现有生命周期缺陷表）、
> `vsg-target-resize-in-place.md`（原地改尺寸）、`vsg-pipeline-sharing.md`（管线/动态状态归位）、
> `vsg-upstream-alignment.md`（vsg 上游约束）。
>
> 上游依据：`build/_deps/vsg-src`（**仓库实际编译的那份 vsg，1.1.16**；§9.1 的证据来源）、
> `/opt/opensrc/vsgExamples`（上游用法参考，1.1.15——差一小版，只当idiom参考，不作行为性结论依据）。
>
> **引用约定（便于后续修改）**——本文引用分三类，按此定位；带符号的引用优先于行号：
> 1. **本仓设计/记忆**：`.ai/design/*.md`、`.ai/memory/*.md`（相对仓库根，稳定）；
> 2. **本仓实现/脚本**：写路径 + **符号名或脚本名**（如
>    `src/plugins/gfx_backend_vsg/include/vine/vsg/VsgRetireRing.hpp`、`scripts/gfx_lavapipe_check.sh`）；
> 3. **上游源码**：写 `vsg <tag>` + 仓库内路径 + **符号名**（如
>    `vsg 1.1.16 — src/vsg/app/RecordAndSubmitTask.cpp — RecordAndSubmitTask::start()`）。
>    本地路径 `build/_deps/vsg-src` 只是**便利入口**（FetchContent 产物，清了 `build/` 就没了）；
>    **版本号才是指针**。括号里的行号按 2026-09-21 的检出核对，仅供定位，改动后以符号为准。
>
> 每条事实都要写**复核状态**：`已逐行复核` / `来自既有结论` / `来自脚本断言`——三者不能混用。
>
> 验收口径：§7 的不变量清单 + 现有 `vsg_backend_selftest` 的像素相位 + 证据基线逐字节比对。

## 0. 一句话

后端不是"把场景翻成 Vulkan 调用"，而是**一台把帧意图编译成 GPU 状态的编译器**：
API 是声明意图；`FrameDescription` 是这一帧的事实；`CompiledFrame` 是依赖求值的结果；
`ResourceManager` 管资源生命周期事实；`VsgExecutor` 只是把已经决定好的事实机械地变成 GPU 操作。

这条边界是本设计唯一真正的价值所在：**只要 Protocol / Compiler / ResourceManager 不偷偷操作 vsg 对象、
不做 GPU 决策**，`core/` 四件（Protocol / Compiler / ResourceManager / Timeline）即使把 vsg 换成裸 Vulkan
（甚至换 API）也大体可留；反过来它们一开始越界，"从零重做"的收益就会一点点消失。

**No-VUID is necessary but not sufficient**：验证层干净只说明 API 调用合法，不说明画出了任何东西。
每个相位必须同时满足"验证干净 ∧ 计数器符合预期 ∧ 像素符合预期"（§7）。

## 1. 输入：不可协商的边界

### 1.1 契约（`RenderBackend.hpp`）

| 契约点 | 内容 | 设计后果 |
| --- | --- | --- |
| 帧配对 | `beginFrame()` 可获取交换链图像；**只有** `swapBuffers()` 呈现，且是一帧最后一次调用 | 需要一个"帧令牌"（D5）；空帧也必须提交 |
| pass scope | scope 属性（target / order / depthMode / clear）属于该 pass 并在 `endPass()` 丢弃；每次绘制属性（viewport / lights）只服务下一个绘制调用；无 scope 的绘制**必须拒绝并上报** | 协议是一台状态机，不是一个函数（D1） |
| 借用一切 | 相机 / 命令 / 灯 / target / 程序只在调用期间借用；pass 只在 scope 内借用 | 后端必须"抄下来"而非持有（D3 的 data 层） |
| 增量 | `render()` 必须增量对账；稳态帧不重传几何、不重编管线、不增长 | 三层键 + 键审计（D3） |
| 失败语义 | 不抛异常跨接口；`initialize()` 返回 false 时**后端自己**清理半成品（引擎不会调 `shutdown()`） | 会话必须"要么完整要么不存在"（D4） |
| 保留态 | 可以由 `release*` 释放，但**不得要求**宿主必须调它才正确；不得随帧数增长 | 账本 + park 环（D4） |
| 诊断 | 同步 sink、只记录不回调；无论是否装 sink 都要计数 | 单一出口 + 类型化的"每 episode 报一次"（D8） |
| 线程 | 单线程、串行、不可重入；但不得假设 `initialize()`/`shutdown()` 同一线程 | 全设计不加锁，但会话状态必须整体替换而非逐成员改 |
| 借用必须**当场快照** | 引擎自己的 `resolved_inputs_` 是**复用成员**：每帧 `clear()` + `reserve()` + `push_back()`，`setPassInputs(resolved_inputs_)` 传进来后**下一个 pass 就会改**（`RenderEngine.cpp:515-532`）；`RenderCommand` 同理只活到 `render()` 返回 | **帧内一切声明在到达时就复制进 `FrameArena`**，计划里不得出现指向宿主容器的 `span`（P0-1） |
| 循环依赖无人拦截 | `graphics/src` 里**没有**任何环检测（已核：无 cycle/topological 相关代码），`validateWiring()` 只管声明冲突（谁填谁、谁声明谁的输出） | 后端是唯一能看见环的地方 ⇒ 环必须有**显式失败语义 + 计数**，不能静默按当前顺序执行（P0-7） |

### 1.2 平台事实（不是"现有实现的选择"）

| 事实 | 影响 |
| --- | --- |
| 设备地板 `VK_API_VERSION_1_4`（`VsgBackendUtility.hpp`：`kRequiredVulkanVersion`；注释说明这是**策略**，状态层真正依赖的是 core 1.3 的四个动态状态 + EDS2/3） | 启动即拒绝；版本号要连设备号和原因一起上报 |
| 该 vsg 版本**没有** dynamic rendering、也不传 `VkPipelineCache` | 必须走 render pass + load-op 变体；重建设备等于重编全部管线 ⇒ "移动会话"比"重建会话"值钱 |
| render pass 兼容性**不含** load-op 与 `initialLayout`，但**含**附件格式/采样/深度格式 | 同一附件集合可以有多份"变体"（兼容、可换），形状变化无法用变体绕过 |
| vsg 的 `GraphicsPipeline` 按 **viewID** 编译，且复用循环只比 pipeline states、不比 render pass | 每个 pass 一份状态注册表是**硬约束**，不是优化 |
| push constant 保证预算 128B | 光照 + 视图必须挤进一个块（`LightPushBlock`），前向着色只能走 UBO |
| 顶点 ABI：位置 0 / 法线 1 / 颜色 2 / UV **8** / 自定义 ≥3；四个块 288-80-64-80 字节（std140，静态断言钉死） | 通道→binding 映射是固定正典序，缺的通道要喂零值载体 |
| reverse-Z（远平面 0）+ 比较算子反转；世界 CCW 为正面 ⇒ framebuffer 空间 CW | 投影、清屏值、比较算子、正面声明必须是**同一处**的四个侧面 |
| X11(XCB)/Win32 双实现，Wayland 无（vsg 该版本没有） | 句柄要按平台**精确类型**转换；Win32 采用宿主窗口时要撤掉自己的 OLE 注册 |

每条事实的上游出处与复核状态见 §9.1：其中"无 dynamic rendering""无 EDS2/3 入口""不使用 `VkPipelineCache`"
三条已在 `build/_deps/vsg-src` 里逐行复核（grep 零命中 / 只出现在拉进来的 Vulkan 头文件里）；
其余各条来自仓库既有结论与脚本断言（`scripts/check_vsg_upstream_capabilities.py`、`vsg-pipeline-sharing.md` 等）。

### 1.3 被迫同形的地方（我会照做，不"创新"）

1. render pass + load-op 变体（上游没有 dynamic rendering）。
2. 每个 pass（= 每个 viewID）一份状态注册表；上传/几何流按内容共享、状态不共享。
3. 128B push block 承载全屏路径的光照+视图。
4. 深度提升 / 借用 / LOAD 保留三件套（SDK 的 `RenderTarget` 就是这么建模的）。

## 2. 架构

### 2.1 两个流，不是一条链

本系统里跑的是**两条互不相同的流**，把它们画在一条链上是 v1 最大的表达缺陷：
帧数据流每帧清零重建，资源寿命流跨帧存在。

```
                         FRAME FLOW（每帧重建）

  API  ──→ FrameRecorder ──→ FrameDescription ──→ FrameCompiler ──→ CompiledFrame ──→ VsgExecutor
            │ 合法性            （自包含快照）        │ normalize/resolve          （可执行计划）      │
            │ （Protocol）                            │ graph/schedule                              │
            └───────────── 意图 ────────────────────┴──────────── 引用（key/handle） ─────────────┘
                                                                         │
                                                                         ▼
  ┌──────────────────────────────────────────────────────────────────────────────┐
  │ ResourceManager  active ──replace──→ retired ──completed frame──→ destroyed  │
  │  （几何/管线/描述符/目标/世代/退役队列；owning）                              │
  └──────────────────────────────────────────────────────────────────────────────┘
                         LIFETIME FLOW（跨帧）

            Observe（Diagnostics / Counters / RetentionStats / Trace）
            ↑ 横切：被上面每一个环节调用，不是一层，也不参与决策
```

### 2.2 六个核心对象 + 一个横切面

原则：**对象数尽量少，但每个对象只回答一个问题**。评审担心"v1 的 L1–L6 六层最后变成
边界复杂的大状态机"，这个担心是对的；下面每个对象都明写"不做什么"。

| 对象 | 只负责 | **不做** |
| --- | --- | --- |
| `VsgBackend : RenderBackend` | SDK 的 31 个方法**逐字**实现 + 转发；持有 `FrameTimeline` | 不知道管线/渲染图怎么建，不碰 vsg |
| `FrameRecorder`（内含 `Protocol`） | 把调用收成 `FrameDescription`；**只判"这次调用是否合法"** | 不决定 rebuild / resize / borrow / 顺序 / 缓存 |
| `FrameCompiler`（内含 `FrameGraph`） | normalize → resolve target/depth → 建边 → schedule → `CompiledFrame` | 不碰 GPU，不持有资源，不知道 vsg 类型 |
| `ResourceManager`（含 `RetirementQueue`、`Validator`） | 资源的创建/查找/替换/退役；世代号 | 不决定"该不该重建"（那是 Compiler 的输出），不录制 |
| `VsgExecutor` | 把 `CompiledFrame` 机械地变成 vsg 图/提交/呈现 | 不判断 rebuild / resize / reorder / borrow / 选管线；只保留**平台强制**的步骤（取图/重建交换链/变体回切） |
| `Observe`（横切） | 诊断、计数器、retentionStats、trace | 不参与任何决策（这就是它不能算一层的原因） |

`SceneMirror` 退化成 `ResourceManager` 的输入适配（scene 对象 → 资源身份），**不再拥有 GPU 资源**：
所有权只有一处。

### 2.3 物理边界：`core/` 不许 include vsg

"只有 Executor 碰 vsg"如果只写在文档里，最后一定退化成口号。因此目录本身就是边界：

```
backend/
  core/            ← 不许出现任何 vsg::*（含 ref_ptr / RenderGraph / GraphicsPipeline / DescriptorSet）
    Protocol.hpp  FrameDescription.hpp  FrameCompiler.hpp  FrameGraph.hpp
    ResourceManager.hpp  RetirementQueue.hpp  FrameTimeline.hpp
    Keys.hpp      Handles.hpp           Observe.hpp
  api/             ← 可以 include core；这里是唯一的 API 世界（落地时叫 api/，不是 vsg/：
    Session.hpp      插件目录已经叫 vine/vsg/，再套一层 vsg/ 只会让人读成命名空间）
    DeviceProbe.hpp  VsgExecutor.hpp  VsgResource.hpp  VsgDynamicState.hpp
```

规则：`core → vsg` 禁止，`vsg → core` 允许；`core` 里只用自己定义的 opaque handle
（`PipelineHandle{index, generation}` / `ImageHandle` / `TargetHandle` / `BufferHandle`）。
落地手段（**已实现 2026-09-21**）：`scripts/check_include_hygiene.py` 新增 `core_layer_findings()`——
`core/` 目录下禁止 `vsg/*`（渲染 API 自己的头）与除 `vine/vsg/core/*`、`vine/vsg/vsg_global.hpp`
（命名空间/导出宏，编码规范要求它第一个包含）之外的 `vine/vsg/*`（插件自己的上层）。

这一步的回报是可验证的：换 API（裸 Vulkan / D3D12）时，`core/` 理论上可原样复用。

### 2.4 一帧的生命周期

```
beginFrame          → FrameTimeline 铸令牌；取交换链图像；FrameArena 重置（bump，不释放）
  每个启用的 pass（升序）
      beginPass     → Recorder 开 scope；记下"本帧活跃 pass"
      setPassOrder  → 写进 FrameDescription（堆叠位，不是身份）
      setPassInputs → **复制**解析结果进 FrameArena（绝不持有引擎的 span）
      pass->execute  → setRenderTarget / setViewport / announceClear / setDepthMode /
                       setLights / render（或 drawScreenProgram）—— 只写 FrameDescription
      endPass       → 关 scope；未消费的属性丢弃
endFrame            → FrameCompiler：normalize → 目标/深度求解 → FrameGraph → Schedule
                    → ResourceManager：份额快照 → 退役结算 → 废弃清扫
                    → VsgExecutor：按 Schedule 录制（记录顺序 ≠ 到达顺序）
swapBuffers         → 消费令牌；submit + present 各一次；FrameTimeline.commit()（退役推进只在这里之后）
```

**与现有实现的关键差别**：`render()` / `drawScreenProgram()` 的**录制时机推迟到 `endFrame()`**，
且计划里的每一段数据都活在 `FrameArena` 里而不是宿主的容器里（P0-1）。

### 2.5 P0 定义表（实现契约）

评审要求"先钉死定义再开工"。以下 10 条就是开工契约，每条都给出**结论**：

| # | P0 问题 | 结论 |
| --- | --- | --- |
| 1 | `FramePlan` 的 backing storage 归谁 | `FrameArena`（帧内 bump 分配器）**独占**：所有 span 指向 arena，**禁止**指向宿主 API 参数背后的容器；`render()` 收到 `RenderCommand` 就复制所需字段 |
| 2 | Intent → Plan 的 normalize 语义 | 见 D2 的两列表：`FrameDescription` 允许保序/重复/后设覆盖/非法调用/缺省；`CompiledFrame` 必须是"每 pass 一个终态、每 draw 一个终态、借用已快照、默认已解析、合法性已判定" |
| 3 | `Protocol` 的职责上限 | **只答"这次调用是否合法"**，不答"应该怎么办"：rebuild/resize/borrow/顺序/缓存一律不属于它 |
| 4 | 依赖图与退役是否同一个东西 | 不是：`FrameGraph`（帧内、纯数据、求值后即弃）与 `RetirementQueue`（跨帧、按帧时间轴）分开；`ResourceManager` 只是它们的宿主 |
| 5 | 目标形状 / 兼容性 / load-op 变体的关系 | 拆成 `PipelineCompatibility`（格式/采样/子 pass/深度格式）与 `LoadOpVariant`（load/store + initialLayout）；**管线键只含前者**（见 D3） |
| 6 | Schedule 的节点与边 | 节点 = **pass scope**（不是 draw）；边 = 生产者 pass → 消费者 pass（采样边 + 深度借用边）；draw 顺序只属于 pass 内部 |
| 7 | 环的失败语义 | 环 = **声明本身不合法且引擎不查**（已核）⇒ 上报 Error + **跳过整个强连通分量**（其余 pass 照旧、帧照旧提交呈现）+ `invalid_schedules` 计数；自检断言该计数为 0。禁止"按当前顺序偷偷执行" |
| 8 | `revision` 的来源与稳定性 | 只能来自上游资源（`Geometry/Buffer/Texture/ShaderProgram::revision()`）；**后端绝不自行递增**；键是纯身份 |
| 9 | `InstanceSlot` 是 CPU 逻辑还是 GPU 存储 | **CPU 逻辑记录**（矩阵/透明度/材质引用+revision）；GPU 存储属于 arena。两者靠"pack"连接 |
| 10 | Evidence 是不是一层 | 不是：横切 instrument（图见 §2.1）；相位表、计数器、像素断言围绕它组织（D8） |

## 3. 八个设计决定

### D1 · 合法性收在一个类型里，但只判合法性

**做法**：`FrameRecorder` 内部持有 `Protocol`——一个显式的 `{ Idle, InFrame, InPass }` 状态 + 声明字段
（当前 pass、当前 target、该 target 是否已被 `releaseRenderTarget` 宣告可销毁）。所有入口第一步过它，
违规返回 `ProtocolVerdict{ Allow | Drop | Refuse }`；`ReportOnce` 的重臂边界写在状态机里
（"无 scope 绘制"按帧重臂、"已释放 target"按 scope 重臂）。

**硬边界**（评审 P0-3）：Protocol 只答"这次调用是否合法"，**绝不答"应该怎么办"**——
不判 rebuild、不判 resize、不判 borrow、不排顺序、不碰缓存。它一旦开始回答后者，就变成
评审担心的那个大状态机（`Protocol → {scope, target, pipeline, depth, cache, lifetime}`）。

**为什么**：契约里 8 条协议规则都有"报告 / 拒绝 / 丢弃"三选一；分散实现最容易漏的是
"该拒绝的却被静默重定向"（把已释放 target 的绘制降级到窗口）。集中后规则可穷举，
且"新加一个入口"必然经过它。

**判据**：无设备的状态转移表测试：每个 (状态 × 入口) 断言 `{允许 / 丢弃 / 拒绝并上报}`；
`diagnosticCount(PassProtocolViolation)` 逐格钉住。

### D2 · 三段式（收集 → 编译 → 执行），且 normalize 有明确语义

**做法**：`FrameRecorder` 收 `FrameDescription`（自包含快照）→ `FrameCompiler` 产出 `CompiledFrame`
→ `VsgExecutor` 按 Schedule 机械执行。三段之间只有数据，没有回调。

normalize 的两侧契约（评审 P0-2）**必须分别成立**：

| `FrameDescription`（收集侧）**允许** | `CompiledFrame`（编译侧）**必须** |
| --- | --- |
| 保留调用顺序（顺序本身有意义：`setPassOrder` 是堆叠位） | 每 pass 一个终态：后设覆盖先设 |
| 同一属性被设多次 | 每 draw 一个终态（viewport / lights 已绑定到具体绘制） |
| 非法调用（无 scope 绘制 / 嵌套 scope / 已释放 target） | 合法性已判定（非法的在收集侧就被拒绝或丢弃，不进编译） |
| 缺省值**未解析**（没设 viewport = 全目标，没设 clear = 不清） | 缺省值已解析成显式值（全目标矩形、无 clear） |
| 借用的指针（相机 / 命令 / 灯 / target） | 借用已快照（数据在 `FrameArena`，见 §2.5 P0-1） |

**为什么**：如果 `FrameDescription` 直接等于 `CompiledFrame`（v1 的隐含假设），Emitter 里会重新
冒出 `if (!target) / if (!depth) / if (!lights)`，v1 的 L5 就变回第二个 L1——评审这条是对的，
所以两侧契约写在同一张表上。

**判据**：`GraphOrderTest` 式的纯函数测试（环的另一半见 D2.1）；`AllocationGate`（core，进程内读堆用量）：
稳态帧增长 = 0。

#### D2.1 环的失败语义（评审 P0-7，重新定义）

**事实核对**：`graphics/src` 里**没有**环检测（`Group.cpp` 的 cycle 检查是场景图父子关系，不是渲染依赖），
`RenderEngine::validateWiring()` 只管"谁填谁 / 谁声明谁的输出 / 声明冲突"。⇒ **后端是唯一能看见环的地方**。
（现有后端在 `VsgRecordOrder.cpp:161` 记下了"实践上无环；有环就保持当前顺序"——那正是评审担心的
"验证层干净、画面却少了一个 pass"。）

**结论**：环 = 调用方声明本身不合法，按**不变式违规**处理：

1. 上报 `Error`（"依赖环：A ↔ B，无法确定执行顺序"），每 episode 一次；
2. **跳过整个强连通分量**：分量内每个 pass 本帧**不绘制**（不是"换个顺序偷偷画"，而是不画）；
3. 分量外的 pass 照旧执行，帧照旧提交与呈现（契约要求一帧必须以 `swapBuffers()` 收尾）；
4. `invalid_schedules` 计数 +1，自检断言其恒为 0。

**为什么不是"丢弃整帧"**：环是调用方 bug，通常只涉及两个辅助 pass；把主画面一起丢掉是
把局部错误放大成黑屏。**为什么不是"按当前顺序继续"**：那会让消费方采样到尚未写入的附件，
画面错得无从归因。⇒ "跳过 + 计数 + 响亮"，像素上不假装。

### D3 · 三层状态模型 + 键审计 + 兼容性/变体拆分

**做法**：每个绘制项的状态分三层，并规定**上层的东西绝不进下层的键**：

| 层 | 内容 | 允许的变化代价 |
| --- | --- | --- |
| Identity → 管线键 | 程序 + `revision()`、顶点布局、**兼容性键**（下方拆开）、源深度是否可采样、shadow 是否绑定、采样附件数 | 重编管线（唯一允许） |
| Dynamic state | `DepthMode`、cull、front face、polygon、topology、blend enable/equation（9 项 + 逐附件） | 一条 `vkCmdSet*` |
| Data | 视图块、光照块、`VineDrawBlock`（矩阵 + opacity 在 `params.x`）、材质 64B | 就地字节写 / 描述符 re-point |

**目标属性的两分（评审 P0-5）**——v1 含糊的地方在这里钉死：

```
TargetShape
 ├── PipelineCompatibility      ← 进管线键（Vulkan 的 render-pass 兼容性就是这些）
 │    ├── color formats（每个附件）
 │    ├── depth format（或“无”）
 │    ├── sample count
 │    └── subpass structure
 └── LoadOpVariant              ← 不进管线键（兼容性明确不含 load-op / initialLayout）
      ├── color load / store op
      └── depth load / store op
```

三条推论，直接回答"为什么 resize / CLEAR↔LOAD 不重编"：
* **extent 谁都不进**：像素尺寸是运行时资源（图像 / 视图 / 帧缓冲 / 动态视口），不是身份；
* **CLEAR ↔ LOAD 只是换变体**：同一管线在多个兼容变体上都能用（这也是 bootstrap 变体合法的原因）；
* **形状变化才 rebuild**：附件数 / 格式 / 采样 / 子 pass / 深度格式变了，兼容性才真的变了。

因此 v1 那句"尺寸不进 ShadingKey"要改成更精确的措辞：**像素尺寸不得进入 shader/pipeline identity；
只有影响 attachment 兼容性的属性才能进入兼容性键**（评审 P0-5）。

**为什么**：现有缺陷族（pass-lifecycle §1、pipeline-sharing §2）都是**键污染**：把可动态化的东西
放进管线键（StateNode 一改就重编），或把要换的视图留在节点里（resize 要重建整批程序槽）。

**判据**：**键审计测试**——每个 key 声明"允许的输入集合"，测试断言集合未扩大；再加行为断言：
`StateNode` 改 cull/polygon/blend ⇒ `pipelineVariantCount()` 不变；材质改参数 ⇒ 节点重建 0 次、
只发生一次 64B 写；目标改尺寸 ⇒ `offscreen_builds` / `program_slot_builds` 不变。

### D4 · 资源寿命：owning 的 ResourceManager + 退役队列 + 只读校验器

**做法**（评审 P0-4，把 v1 的 `Ledger` 拆成三件事）：

```
ResourceManager            ← owning：几何 / 管线 / 描述符 / 目标，按 key 查找与创建
 ├── GeometryCache / PipelineCache / DescriptorCache / TargetCache
 ├── Generations           ← 每个会话一个 generation；所有 key/handle 携带它
 └── SceneMirror           ← 只是“scene 对象 → 资源身份”的适配（不拥有 GPU 资源）
RetirementQueue            ← 跨帧寿命：active → replaced → retired → destroyed，按帧时间轴
Validator                  ← 只读：检查 ResourceManager + RetirementQueue 的不变量（Debug 每帧）
```

销毁只有两条合法路径：① **整会话替换**（前面一次计数过的 device idle）；② **退役队列**出队。
Debug 校验器每帧断言："没有 handle 指向已退役资源"、"稳态帧退役队列不增长"、"帧路径 device wait = 0"。

**为什么拆**：依赖关系（descriptor → view、graph → pass）是**帧内**的、求值完就丢；资源寿命是**跨帧**的。
v1 让一个 `Ledger` 同时当依赖图 + 垃圾回收 + 世代管理 + 校验器，正是评审说的"God Object"风险。

**判据**：`DeferredReleaseTest` / `GeometrySafetyTest` 式用例；Debug 校验器在全部自检相位下零报告；
`retentionStats().parked_nodes` 帧末回落。

### D5 · FrameTimeline：submitted 与 completed 必须分开

**做法**（评审 P0-5 修正了 v1 的"帧时钟"语义）：`FrameTimeline` 暴露两术数而不是一个：

```cpp
FrameToken begin();                 // 铸令牌
void       submitted(FrameToken);   // swapBuffers 里，提交完成后
uInt64_t   submittedFrame() const;  // 已提交帧数
uInt64_t   completedFrame() const;  // 有完成证据的帧数（见下）
```

**完成证据从哪来（这条是整个机制的关键）**：本后端**没有**独立的时间线信号量查询，安全性的来源是
**命令缓冲槽位的 fence 复用**。已在仓库实际编译的那份 vsg（`build/_deps/vsg-src`，1.1.16）里逐行复核：

* 槽数 = 帧在飞数：`Viewer::assignRecordAndSubmitTaskAndPresentation()` 里的 `uint32_t numBuffers = 3;`
  （`vsg 1.1.16 — src/vsg/app/Viewer.cpp — :501`）→ `RecordAndSubmitTask::RecordAndSubmitTask(Device*, uint32_t numBuffers = 3)`
  （`include/vsg/app/RecordAndSubmitTask.h`）→ 构造体里的 `_fences.resize(numBuffers)`；
* **槽只有在它自己的 fence 被等过之后才会被重新录制**：`RecordAndSubmitTask::start()` 里
  `if (current_fence->hasDependencies()) { current_fence->wait(timeout); current_fence->resetFenceAndDependencies(); }`
  ——即"一槽一个 fence，重用前先等它"，**这就是完成证据**（详见 §9.1）；
* 槽索引每帧轮转：`RecordAndSubmitTask::advance()`，每帧由 `Viewer::advanceToNextFrame()` 调。

⇒

* `kRetireDepth = slots + 1` **是被推导出来的**，不是魔数：槽数来自 `numBuffers`（本 vsg 是 3）
  ⇒ 深度 4（与现有 `VsgRetireRing::kRetireRingDepth` 一致）；
* 退役点写成 `retire_at = submittedFrame() + kRetireDepth`，释放条件是 `completedFrame() >= retire_at`，
  其中 `completedFrame()` 由**槽位回收**推进；
* 槽数**不在公开 API 上**（`Viewer.cpp:501` 是局部硬编码），而 `fence(i)` 越界返回 nullptr
  （`RecordAndSubmitTask.cpp:79-83`）⇒ **启动期探测或断言一次**（探测值与推导值不一致就上报并按探测值走）；
* **推进点必须是帧的最后一步**（现有实现踩过：park 落在该调用之后会早一帧释放，
  于是销毁了已提交命令缓冲仍在引用的对象）；
* 若执行器将来改用"不等待槽位 fence"的提交方式，`completedFrame()` 的证据就消失 ⇒
  **必须退回计数过的 device wait**，以 `retireWaitCount()` 为判据（不得静默假设）。

同一时间轴同时服务：资源退役、pass 退役（本帧未被 `beginPass` 声明的 slot）、bootstrap 状态、
GPU profile 的滞后判定、诊断里的 `frame=N`。

**为什么**：三条独立的"延迟"各自维护节拍必然错拍。评审的 `submitted`/`completed` 区分是对的：
"提交"不等于"完成"，把两者混为一个计数就是最后一类静默错误的来源。

**判据**：`FrameCommitTest` 式用例；帧路径 `device_waits == 0`；`runPolicyChurnStressPhase`
下像素与计数器同时稳定。

### D6 · 目标三件套 + `planTarget` / `depthPlan` 纯函数

**做法**（沿用 v1 的纯函数，按评审补上命名与 generation）：

```cpp
struct TargetDesc {                   // 逻辑描述（= SDK 的 RenderTarget 投影）
    Extent2D extent; std::span<const Format> color_formats; std::optional<Format> depth_format;
};
struct TargetInstance {               // 当前这一代 GPU 对象
    TargetDesc desc; ImageHandle color[]; ImageHandle depth; uInt64_t generation;
};

enum class TargetAction { None, Repair, ResizeInPlace, Rebuild };
TargetAction planTarget(const TargetInstance&, const TargetWanted&);
//  Rebuild       ⟸ 兼容性键变了（附件数 / 颜色格式 / 深度格式 / 有无深度 / 子 pass）
//  ResizeInPlace ⟸ extent 变：换图像/视图/帧缓冲 + 描述符 re-point + barrier + 借用源视图重指
//  Repair        ⟸ 尺寸未知，或 bootstrap（首次进新 target 必须清一次）
//  None          ⟸ 其余

struct DepthPlan { bool sampleable; bool borrowed; const RenderTarget* source; bool preserve; };
DepthPlan depthPlan(const RenderTarget&, const PassClearSet&);
//  sampleable ⟺ 提升 ∧ 本 target 无“保留深度”的 pass（提升被任一 LOAD pass 撤销）
//  borrowed   ⟹ 策略来自出借方；出借方被释放 ⇒ 恰好重建一次（记忆“死深度源”）
```

**为什么**：三件事（提升 / 借用 / LOAD 保留）都改 `initialLayout`，都与 load-op 变体交互；
分开实现必然出现"某一帧多清一次"或"采样到 attachment layout"。合成一个函数的输出后可表驱动穷举。

**判据**：`TargetBookkeepingTest` 式的表 + `runTargetResizePhase`（`offscreen_resizes` +2、
`offscreen_builds` 不变、`program_slot_builds` 不变、`device_waits` 不变、`parked_nodes` 先升后落）
+ 像素读回确认采样到的是**新**图像。

### D7 · 两类存储：持久 arena（材质）与每帧 ring（视图 / draw）；三类作用域永不混

**做法**（评审 P0-9 修正了 v1 把材质也塞进"每帧 ring"的含糊）：

| 存储 | 内容 | 写入时机 |
| --- | --- | --- |
| **持久 arena** | 材质块（每材质一个 slot，按 in-flight 副本数轮转） | **只在 `revision()` 变化时**写；稳态零流量 |
| **每帧 ring** | 视图块（每 pass）、`VineDrawBlock`（每 draw：矩阵 + opacity） | 每帧写（这些本来就每帧变） |
| 描述符 | 绑定以上两类（dynamic offset） | 视图被替换时**只换 set 元素**（复用 layout 与 sampler），旧 set 进退役队列 |

这样"材质改参数不产生每帧流量"这条稳态性质被保住，同时仍然只有一套描述符布局 + dynamic offset，
材质管理器不再需要"比别的 bridge 活得久"。

其余三条照旧：
* 作用域三分且**永不混**：**会话级**（纹理、网格流）、**进程级**（GLSL 编译阶段、引擎默认程序）、
  **pass 级**（状态注册表、变体池）；
* 废弃判定 = "entry 持有 key 对象 + 按份额计数"（`useCount() <= shares`）；
* 份额在帧开始时快照、帧结束后清扫（现有 P11 缺陷——互相等待——的直接教训）。

**判据**：材质/纹理/网格缓存用例（含 65 个程序的 FIFO 淘汰与"淘汰真的归还共享对象"）；
"材质稳态零字节写"用计数器断言；"两个 pass 画同一几何只上传一次、但各有自己的状态注册表"。

### D8 · 证据是横切的，而且是三重验证

**做法**：

```cpp
struct Phase { const char* name; SetupFn setup; AssertFn assert; CounterDelta expect; };
```

* 相位表：一行 = 装配 → 断言（**像素读回** + 计数器）→ 期望的计数器增量（含"必须不变"的项）；
* **三重验证**（评审 §14，已提升到 §0）：验证层干净 ∧ 计数器符合预期 ∧ 像素符合预期，
  "No-VUID is necessary but not sufficient"；
* 分配门禁：稳态帧堆增长 = 0（`core/AllocationGate`；`scripts/alloc_trace.c` 仍是“内存去哪了”的排查
  工具，它的文件头自己写着“not a gate”，两者职责不混）；
* 诊断带 `frame=N / pass / target / generation / schedule_index`（评审 §18 的 Plan ID），
  "旧世代 / 新世代 / 退役节点"一类问题可以完整追踪；
* 证据脚本逐字节比对 `[selftest]` 行；后端 trace 行不进基线（spdlog 带 file:line、行数随帧数变）。

**为什么**：能力清单与测试清单同源，"新加能力但没相位"会立刻暴露；`CounterDelta` 让
"悄悄多了一次重建"变成显式失败。三重验证是针"验证层干净但什么都没画"这类问题的唯一有效门禁。

**判据**：`scripts/vsg_selftest_evidence.sh` 基线逐字节一致；`scripts/gfx_lavapipe_check.sh` 全绿。

## 4. 能力对照表（同功能证明）

| # | 能力（现有实现已钉住的行为） | 交付者 | 判据 |
| --- | --- | --- | --- |
| 1 | 设备/会话上线、失败自清、幂等、**移动而非重建**、空帧也提交、令牌语义 | `ResourceManager` + 世代号 + `FrameTimeline` + `planTarget`（格式兼容校验） | `DeviceRequirementsTest`、`runHostSurfaceMovePhase`（`window_builds` 不变、移动前后像素相同、两个宿主窗口都还活着） |
| 2 | 表面尺寸权威、就地改尺寸、隐藏/0 尺寸、"无可用尺寸"每 episode 一次 **（"无可用尺寸"在重写版归 `planTarget` 的 `Repair(SizeUnknown)`；读回那张表**没有** `Empty` 这一类——`create` 拒绝零尺寸，见 §11.16y）** | D6 `planTarget` | `runTargetResizePhase`、`ReadbackRefusal::Empty` |
| 3 | 离屏 target 懒建、MRT、attachment 0 才清、额外附件透明黑、深度清到 reverse-Z 远平面、LOAD/STORE 变体、提升/借用/退役 | D6 `TargetInstance` / `planTarget` / `depthPlan` + D4 退役队列 | `PassClearValuesTest`、`PassRenderPassPlanTest`、`runColorBootstrapPhase`、`runSharedDepthPhase` |
| 4 | pass scope、order 只是堆叠位、release 语义、协议违规、预热（成本归 "pre-frame"）、停用/移除 pass | D1 + D2 | `PassProtocolTest`、`FrameCommitTest`、`runPassProtocolPhase`（`detached_slots` 升后落、`offscreen_builds` 不变） |
| 5 | 内容绘制、增量刷新、稳态（不重传/不重编/不增长）、缓存有界、废弃判定、共享规则 | D3 三层键 + D7 缓存与份额计数 | `SceneBridgeDataRebuildTest`、`runDataRefreshPhase`（build 1 / refresh 1 / 节点数平）、churn 相位 |
| 6 | 全屏/屏幕 pass、PiP 子矩形、deferred 合成、源附件 i→binding i、深度真可采样才绑 | D3 键（draw kind = fullscreen，不另建系统）+ Executor | `runCompositingPixelPhase`、`runDepthSamplingProgramPhase`、`runTargetResizePhase` 的 `program_slot_builds` 不变 |
| 7 | 光照 world→view、1 ambient + 3 directional、默认头光/环境光、丢弃项每 episode 一次 | 编译侧打包（前向 UBO + 全屏 128B push） | `LightsTest`、`LightDropReportTest`、`ForwardShaderSetTest` |
| 8 | HUD / window layer：就是更高 order 的普通 pass + 子视口 + 不清屏（LOAD） | 无需专门代码 | `ContentSlotViewportTest`、HUD 相位、`.ai/bugs/vsg-maximize-black-band.md` 的复现 |
| 9 | readback：RGBA8 紧凑打包、D32 原样 / D16÷65535 / D24S8 诚实拒绝、借用方读到源 **（已关，§11.16y：`core::readbackOf` 单表 + 两张格式表 + `decodeDepth`；借用方读到源由共享深度夹具钉住）** | `core/` 里的 `ReadbackRefusal → ReadbackResult` 单表 | `ReadbackTest`、`runPixelReadbackPhase`、`runSharedDepthPixelPhase` |
| 10 | 缓存与所有权：entry 持有 key、份额计数、会话/进程/pass 三类作用域 | D7 | `SceneBridgeCacheOwnershipTest`、`GeometrySafetyTest`（未画 1000 次仍保留） |
| 11 | 诊断：单一出口、9 类、`ReportOnce`、告警不静默降级 | `Observe`（横切）+ D2.1 的环语义 | `DiagnosticsTest`、`ReportOnceTest`、`LightDropReportTest`、`scripts/check_diagnostic_formats.py` |
| 12 | 平台：reverse-Z 与算子反转、正面声明、动态状态 9+ 项（3 个走函数指针）、无 VUID、WSI 细节 | `vsg/` 目录（唯一设备层） | `RenderStateMapperTest`、`DynamicStateTest`、`runHostSurfaceMovePhase`、`check_vsg_upstream_capabilities.py` |
| 13 | 自检与证据：相位表、像素断言、逐字节基线、性能档（构建档案 / GPU profile） | D8 | `scripts/vsg_selftest_evidence.sh`、`runGpuProfilePhase`、`GpuProfileTest` |

## 5. 类型草图

```cpp
// —— 收集侧：自包含快照（所有 span 指向 FrameArena）——
struct FrameDescription {
    FrameToken token;
    std::span<const CollectedPass> passes;   // 保序；终值可能尚未规范化
};
struct CollectedPass {
    PassId pass; int order; TargetHandle target;
    bool rect_announced; Viewport rect;              // 未设 = 全目标（未解析）
    std::optional<ClearPolicy> clear; DepthMode depth;
    std::span<const InputRef> inputs;                // 从引擎的复用 vector 复制而来
    std::span<const LightRef> lights; std::span<const CollectedDraw> draws;
};

// —— 编译侧：可执行计划（缺省已解析、借用已快照、合法性已判）——
struct CompiledFrame {
    FrameToken token;
    std::span<const CompiledPass> passes;            // 已按 Schedule 排序
    std::span<const TargetPlan> targets;
    std::span<const ResourceRequest> resources;
};
struct CompiledPass {
    PassId pass; TargetHandle target;
    RenderPassCompatibility compatibility;           // 管线兼容性身份
    LoadOpVariantKey variant;                        // 可换的 load-op / initialLayout
    Viewport rect; DepthState depth; std::uint32_t schedule_index;
    std::span<const CompiledDraw> draws;
};
struct CompiledDraw {
    DrawKind kind;                                   // Geometry | Fullscreen（不另建系统）
    PipelineHandle pipeline; DescriptorHandle descriptors;
    DynamicState dynamic; InstanceSlot instance;     // 三层：身份 / 动态 / 数据
    std::span<const GeometryBatch> batches;
};

// —— 键（D3）：只放“改了就必须重编/重建”的东西 ——
struct DataKey { const vine::Buffer<float>* buffer; std::uint64_t revision;   // revision 只来自上游
                 std::uint32_t components, offset, count; };
struct VertexLayoutKey { std::uint32_t canonical_mask; std::span<const std::uint32_t> custom_locations; };
struct RenderPassCompatibility { std::span<const Format> color_formats;
                                 std::optional<Format> depth_format; std::uint32_t samples, subpass; };
struct LoadOpVariantKey { LoadOp color_load, depth_load; ImageLayout initial, final; };
struct PipelineKey { const ShaderProgram* program; std::uint64_t revision;
                     VertexLayoutKey vertex_layout; RenderPassCompatibility compatibility;
                     bool depth_sampleable, shadow_bound; std::uint32_t sampled_color_count; };

// —— 三层里的另两层 ——
struct DynamicState { DepthMode depth; CullMode cull; FrontFace front_face;
                      PolygonMode polygon; BlendState blend; Topology topology; };  // vkCmdSet*
struct InstanceSlot { Mat4d matrix; float opacity; MaterialRef material; };          // CPU 逻辑记录

// —— 目标与寿命 ——
struct TargetInstance { TargetDesc desc; ImageHandle color[]; ImageHandle depth; std::uint64_t generation; };
enum class TargetAction { None, Repair, ResizeInPlace, Rebuild };
struct DepthPlan { bool sampleable, borrowed, preserve; const RenderTarget* source; };

class FrameTimeline { public: FrameToken begin(); void submitted(FrameToken);
                             std::uint64_t submittedFrame() const; std::uint64_t completedFrame() const; };
class RetirementQueue { public: void retire(ResourceHandle, FrameToken);
                               void advance(FrameTimeline&);   // 帧的最后一步
                               std::size_t waitCount() const; };
```

四条**结构性规则**（比类型更重要）：

1. **arena 独占**：`FrameDescription` / `CompiledFrame` 里的每个 `span` 都指向本帧 `FrameArena`；
   不得指向宿主 API 参数背后的容器（P0-1）。借用的 `RenderCommand` / light / target 一律复制所需字段；
2. **键里不放会被就地改写的值**：矩阵、透明度、材质字节属于 `InstanceSlot`；
3. **extent 不进身份，兼容性属性才进**（P0-5）：像素尺寸 → 运行时资源；格式/采样/子 pass/深度格式 →
   `RenderPassCompatibility`；load-op / initialLayout → `LoadOpVariantKey`（不进管线键）；
4. **缓存 entry 必须持有它的 key 对象**（`OwnedCache`），否则地址回收后无从察觉。

## 6. 与现有实现的差异（含代价）

### 6.1 内部组织差异

| # | 现有做法 | 我的做法 | 代价 / 收益 |
| --- | --- | --- | --- |
| 1 | 材质管理器跨会话、必须比 bridge 活得久 | 会话内 ring arena + dynamic offset | 一次描述符布局改动；换来生命周期耦合消失、每帧只有字节写 |
| 2 | 协议规则分散在若干 `refuse*` / `reportPassMisuse` 站点 | 一台 `Protocol` 状态机 + 转移表 | 多一层间接；换来规则可穷举、拒绝不能被绕过 |
| 3 | 键隐含在重建判定里（散在各 `needsRebuild`） | 键是显式结构体 + 审计测试 | 加字段要过测试（刻意的摩擦） |
| 4 | 记录顺序在遍历中顺带决定 | `Schedule` 纯函数 + 推迟到 `endFrame()` 录制 | plan 必须在 arena 里复用；换来顺序可测、录制层只做机械落地 |
| 5 | 逐成员维护会话态与清理顺序 | 整会话替换 + 世代号 + 账本校验器 | Debug 多一次 O(slot) 校验；换来"换屏后旧键失效"是自动的 |
| 6 | 稳态靠用例断言 | 稳态还有**分配门禁**（`core/AllocationGate`，进程内读 glibc 堆用量） | 需要纪律：不能随手 `std::vector` |

**不变的部分**（§1.3）：render pass + load-op 变体、每 pass 一份状态注册表、128B push block、
深度提升/借用/LOAD 三件套、顶点与四个块的 ABI、reverse-Z 与正面声明、诊断类别词表。

### 6.2 外部评审逐条处理（20 条）

结论口径：**采纳**（改设计） / **部分**（方向对但有边界要补） / **不采纳**（附理由）。

| # | 评审点 | 结论 | 处理 |
| --- | --- | --- | --- |
| 1 | `FramePlan` 生命周期未定义，span 可能悬空 | **采纳** | 新增 §2.5 P0-1：`FrameArena` 独占全部 backing storage，禁止指向宿主容器 |
| 2 | `FrameIntent → FramePlan` 缺 normalize 定义 | **采纳** | D2 的两列表（允许 vs 必须） |
| 3 | Protocol 不要变成万能状态机 | **采纳** | D1 硬边界：只答合法性；并写进 §2.2 的"不做"列 |
| 4 | Ledger 职责过重，应拆 | **采纳** | D4：`ResourceManager` / `RetirementQueue` / `Validator` 三分 |
| 5 | 兼容性 / load-op 变体 / 形状要拆清 | **采纳** | D3 的 `TargetShape` 两分 + §2.5 P0-5 |
| 6 | "尺寸不进 key"应更精确 | **采纳** | 改为"extent 不进身份，兼容性属性才进"（D3、§5 规则 3） |
| 7 | `revision` 的来源要钉死 | **采纳** | §2.5 P0-8：只能来自上游；后端不得自行递增 |
| 8 | `InstanceSlot` 是 CPU 逻辑还是 GPU 存储 | **采纳（取 CPU 逻辑）** | §2.5 P0-9 + D7 的两类存储；并补上"材质稳态零流量"这条被 v1 弄含糊的性质 |
| 9 | `park` 的 N+1 不能是魔数 | **部分** | 采纳"写成退役点"；但完成证据不是时间线信号量，而是**槽位 fence 复用**（D5 给出了 `kRetireDepth = slots + 1` 的推导）；拿不到证据时必须退回计数过的 device wait |
| 10 | Schedule 的节点应是 pass 而非 draw | **采纳** | §2.5 P0-6；边 = pass → pass，draw 顺序只在 pass 内部 |
| 11 | 环不能"上报 + 跳过"了事 | **采纳（比评审更严）** | D2.1：跳过整个强连通分量 + `invalid_schedules` 计数 + 自检断言为 0；帧仍提交 |
| 12 | 三重验证应提升为核心原则 | **采纳** | 写进 §0 与 D8（"No-VUID is necessary but not sufficient"） |
| 13 | `FrameClock` 命名不如 `FrameTimeline` | **采纳** | D5 改名；并把 `submittedFrame()` / `completedFrame()` 分开 |
| 14 | Evidence 是横切，不是层 | **采纳** | §2.1 的两个流图；`Observe` 不算一层（§2.2） |
| 15 | M7 不应"最后才做证据" | **采纳** | §7 新增 **M0 证据地基**；M7 改为"证据加固" |
| 16 | L1–L4 不得出现 `vsg::*` | **采纳（升为物理边界）** | §2.3：`core/` / `vsg/` 目录 + include 规则 + 扩 `check_include_hygiene.py` |
| 17 | 应有 Plan ID / frame id 便于追踪 | **采纳** | D8：诊断带 `frame / pass / target / generation / schedule_index` |
| 18 | 把"数据流"与"所有权流"分开画 | **采纳** | §2.1 |
| 19 | 核心对象数不宜多，建议 6 个 | **部分** | 采纳"六个 + 一个横切"（§2.2）；但 `VsgBackend` 必须是 SDK 的 **31 个方法逐字实现**，评审草案里的签名（`initialize(createInfo)` / `beginPass(RenderPass&)` / `render(const RenderCommand&)` 等）与真实契约不符，不能照抄 |
| 20 | Executor "越笨越好" | **部分** | 采纳"不做决策"；但平台强制的步骤（取图/重建交换链/变体回切）留在它里面——否则会把"平台事实"藏进上游层 |

## 7. 里程碑与验收

| 里程碑 | 内容 | 验收 |
| --- | --- | --- |
| **M0 证据地基**（已完成 2026-09-21） | 相位表驱动、计数器骨架、像素探针夹具、分配门禁、诊断出口 | 空相位能跑通并逐字节产生基线；后续每个 M 落地时**同时**加相位（不留给 M7） |
| M1 会话 | 设备地板检查、格式、交换链、submit/present、空帧也提交、令牌 | `DeviceRequirementsTest`+`FrameCommitTest` 式用例；`runHostSurfaceMovePhase` 移动不重建 |
| M2 内容 | 三层键、几何别名、通道切片、材质 arena、稳态 | `runDataRefreshPhase` 的 (build 1 / refresh 1 / 节点数平)；`pipelineVariantCount` 在 churn 下不涨 |
| M3 目标 | `planTarget` / `depthPlan`、MRT、清屏规则、bootstrap、borrow | `PassClearValuesTest`、`runTargetResizePhase`、`runSharedDepthPhase` |
| M4 全屏 | 全屏 slot、PiP 子矩形、deferred 合成、深度采样绑定 | `runCompositingPixelPhase`、`runTargetResizePhase` 的 `program_slot_builds` 不变 |
| M5 阴影/深度 | LOAD 变体、提升撤销、shadow 输入解析、light slot 索引 | `runDeferredShadowPixelPhase`、`runForwardShadowPixelPhase`、`runShadowedLitFacePhase` |
| M6 readback | 分类拒绝先于等待、格式诚实、借用读源 | `ReadbackTest`、`runPixelReadbackPhase` |
| M7 证据加固 | 基线冻结、性能档、无 VUID 门禁、诊断字段完整性 | `vsg_selftest_evidence.sh` 逐字节一致；`gfx_lavapipe_check.sh` 全绿 |

**全局不变量（每个里程碑都要成立）**

| 不变量 | 含义 |
| --- | --- |
| 帧路径 `device_waits == 0` | 只有 readback 与会话替换会 idle 设备 |
| 改尺寸不重建 | `offscreen_builds` / `program_slot_builds` 在 resize 相位不变 |
| 稳态帧零分配 | arena 复用；分配门禁 |
| 稳态帧不涨 | `pipelineVariantCount`、`parked_nodes`、`retained` 均回落 |
| 与计数聚合一致 | `counters()` 与 `retentionStats()` 重叠项必须相等（同一份数据两个视图） |
| 声称与画面一致 | 每个相位既有计数器断言也有像素断言：验证层干净但没画出东西 = 失败 |

## 8. 风险与不做的事

**最难的三处**

1. **记录顺序 × in-flight 复用**：退役深度与 render pass 兼容性的组合决定"能不能原地换"，
   而完成证据来自槽位 fence 复用。`FrameTimeline`（D5）是唯一能把它讲清楚的机制；不做的话，
   "移动/重建/原地"会变成三种互不知道的延迟。
2. **提升 / 借用 / LOAD 三件套**：都改 `initialLayout`，都必须与 load-op 变体回切对齐。
   合成 `depthPlan()` 是唯一能穷举的方式。
3. **vsg 按 viewID 编译**：强制"状态按 pass 分家、上传按内容共享"。任何"共享一份注册表"的简化
   都会在第二个 pass 上炸——这是硬约束，不尝试绕过。

**明确不做**（与现有实现一致）：dynamic rendering（上游没有）、compute、光追、遮挡查询、MSAA、
宽线、实例化绘制、Wayland、窗口事件泵、后台线程、`VkPipelineCache` 持久化。

## 9. 上游依据与惯用法映射

本节把设计里对外部世界的假设落到**可核对的上游文件**上。引用的是仓库实际编译的那份 vsg
（`build/_deps/vsg-src`，**1.1.16**）；惯用法参考 `/opt/opensrc/vsgExamples`（1.1.15，差一小版，
只当"上游怎么用"的参考，不作为行为性结论的依据）。

### 9.1 已复核的事实

| 事实 | 上游出处（vsg **1.1.16**，以**符号**为准；行号为 2026-09-21 检出的定位提示） | 影响的设计 |
| --- | --- | --- |
| 帧在飞 = 3 个命令缓冲槽，每槽一条 fence | `Viewer::assignRecordAndSubmitTaskAndPresentation()`（`src/vsg/app/Viewer.cpp`，`:420`；其中 `numBuffers = 3` 在 `:501`）→ `RecordAndSubmitTask::RecordAndSubmitTask(Device*, uint32_t numBuffers = 3)`（`include/vsg/app/RecordAndSubmitTask.h`）+ 构造体内的 `_fences.resize(numBuffers)`（`src/vsg/app/RecordAndSubmitTask.cpp`） | D5 的槽数来源；**退役深度是推导值** |
| 槽只有在它自己的 fence 被等过之后才被重新录制 | `RecordAndSubmitTask::start()`（`src/vsg/app/RecordAndSubmitTask.cpp`，`:116`）：`if (current_fence->hasDependencies()) { current_fence->wait(...); current_fence->resetFenceAndDependencies(); }` | D5 的**完成证据**；"park 满槽数 + 1 即可释放"成立 |
| 槽索引每帧轮转 | `RecordAndSubmitTask::advance()` + `Viewer::advanceToNextFrame()` 里的每帧 `task->advance()`（`src/vsg/app/Viewer.cpp`，`:197-199`） | 退役点 = `submittedFrame() + slots + 1` 的语义 |
| **槽表每帧只填一个** ⇒ 槽数要“逐帧学到不再增长” | `RecordAndSubmitTask::index()` / `fence()`（`src/vsg/app/RecordAndSubmitTask.cpp`）：`_indices[i]` 初值为哨兵 `numBuffers`，`advance()` 每帧填一格；`index()` 越界返 `_indices.size()` ⇒ `fence(i)` 为 nullptr | 落地时被实测推翻——见 §11.3（第一帧答案是 1，绝不能当窗口） |
| 帧起点取图；槽数不在公开 API 上 | `Viewer::acquireNextFrame()`（`src/vsg/app/Viewer.cpp`，`:208`，其中 `:220` 调 `window->acquireNextImage()`）；`RecordAndSubmitTask::fence(size_t)` 越界返回 nullptr（`src/vsg/app/RecordAndSubmitTask.cpp`） | 启动期**探测/断言**槽数，不写死 4（P0-5） |
| 上游不做 dynamic rendering | 在 `src/` + `include/`（排除 `include/vsg/vk/vulkan.h`）grep `vkCmdBeginRendering` / `VK_KHR_dynamic_rendering` **零命中** | §1.2：必须走 render pass + load-op 变体 |
| 上游不碰 EDS2/3 的三个扩展入口 | 同上 grep `vkCmdSetPolygonModeEXT` / `VkPhysicalDeviceExtendedDynamicState3FeaturesEXT` **零命中** | §1.2：三个入口要后端自己取（volk / 函数指针） |
| 上游不使用 `VkPipelineCache` | `VkPipelineCache` 仅出现在 `include/vsg/vk/vulkan.h`（拉进来的头） | §1.2：重建设备 = 重编全部管线 ⇒ "移动会话"比"重建"值钱 |

**复核状态**：上表七条均为"已逐行复核"（grep / 读码，2026-09-21 于 vsg 1.1.16 检出）；
§1.2 其余几条来自仓库既有结论（`vsg-pipeline-sharing.md`、`graphics-*.md`）与脚本断言
（`scripts/check_vsg_upstream_capabilities.py`），**未逐行复核**。

### 9.2 惯用法映射（`/opt/opensrc/vsgExamples`）

给 `VsgExecutor` 一个"上游认可的写法"参照，降低自创装配方式的风险：

| Executor 要做的事 | 上游参考 |
| --- | --- |
| 无宿主窗口会话（headless 路径） | `examples/app/vsgheadless` |
| 颜色读回（readback 相位的写法参照） | `examples/app/vsgscreenshot`、`vsgoffscreenshot` |
| 渲染到离屏 target / target 数组（MRT 语义） | `examples/app/vsgrendertotexture`、`vsgrendertotexturearray` |
| 多视图 / 子 pass（子视口、PiP） | `examples/app/vsgmultiviews`、`vsgsubpass` |
| HUD / overlay 叠层 | `examples/app/vsgoverlay` |
| 窗口 / 平台 / 设备选择 / validation 开关 | `examples/app/vsgwindows`、`vsgdeviceselection`、`vsgvalidate` |
| 命令图与提交结构 | `examples/commands` |
| 动态状态用法（逐 draw 交付的那 9 项） | `examples/state` |
| 计算 / mesh shader / 光追（"明确不做"那一节的对照） | `examples/vk/vsgcompute`、`meshshaders`、`raytracing` |
| 帧节拍与线程 | `examples/threading`、`examples/core` |

## 10. 术语

* **scope**：`beginPass()` 与 `endPass()` 之间；scope 属性属于该 pass，每次绘制属性只服务一个绘制调用。
* **键污染**：把"可以用动态状态或就地数据表达的东西"放进了管线/几何身份键，导致本可零成本的变化
  触发重建或重编。
* **`FrameDescription` / `CompiledFrame`**：前者是收集侧的**自包含快照**（允许保序、重复、缺省未解析），
  后者是编译侧的**可执行计划**（终态、缺省已解析、合法性已判、借用已快照）。
* **`FrameArena`**：帧内 bump 分配器；计划里所有 `span` 的 backing storage，稳态帧零分配。
* **兼容性键 vs load-op 变体**：前者进管线键（格式/采样/子 pass/深度格式），后者不进
  （load/store op + initialLayout）；这正是"CLEAR↔LOAD 不重编"的原因。
* **退役点**：`retire_at = submittedFrame() + kRetireDepth`，其中 `kRetireDepth = 命令缓冲槽数 + 1`；
  释放条件是 `completedFrame() >= retire_at`，完成证据来自槽位 fence 复用（D5）。
* **强连通分量跳过**：声明出的依赖成环时的唯一合法处理——分量内不画、分量外照旧、计数 + 响亮（D2.1）。
* **bootstrap**：全新 target 的颜色图像 `UNDEFINED` 无法 LOAD，所以首次进入必须清一次（用一次性的
  load-op 变体，帧末换回稳态变体）。
* **移动 vs 重建**：宿主平台窗口被销毁重建时，用新句柄重建 surface+swapchain 而保留设备与全部管线
  （移动），无法服务时才走整会话重建。

## 11. 实施记录

### 11.1 M0（2026-09-21）：`core/` 的无设备骨架 + 证据夹具

落地位置就是 §2.3 划的那两个目录：

| 文件 | 是什么 | 把哪条 P0 变成可测的 |
| --- | --- | --- |
| `include/vine/vsg/core/Protocol.hpp` + `src/core/Protocol.cpp` | scope 状态机（`Allow` / `Drop` / `Refuse` + 每 episode 上报） | P0-3：只答合法性 |
| `include/vine/vsg/core/FrameArena.hpp` + `src/core/FrameArena.cpp` | 分块 bump 分配器（**增长加块、从不搬块**） | P0-1：计划独占存储 |
| `include/vine/vsg/core/FrameTimeline.hpp` + `src/core/FrameTimeline.cpp` | `submitted` / `completed` 两条水位 + 退役点推导 | D5、P0-9 |
| `include/vine/vsg/core/RetirementQueue.hpp` + `src/core/RetirementQueue.cpp` | 按帧时间轴的延迟销毁 + 计数过的 device wait | D4 |
| `include/vine/vsg/core/TargetPlan.hpp` + `src/core/TargetPlan.cpp` | `planTarget()` / `depthPlan()` 纯函数 | P0-5、D6 |
| `include/vine/vsg/core/Keys.hpp` + `src/core/Keys.cpp` | 三层键 + 键审计表 | D3 |
| `include/vine/vsg/core/Observe.hpp` + `src/core/Observe.cpp` | 计数聚合 + 与退役队列的交叉校验 | D8 |
| `include/vine/vsg/core/PhaseTable.hpp` + `src/core/PhaseTable.cpp` | 相位表驱动（复用 `[selftest]` 行格式） | D8 |
| `include/vine/vsg/core/PixelProbe.hpp` + `src/core/PixelProbe.cpp` | 读回图像的断言夹具（非黑占比、子矩形计数、包围盒钳位） | D8：像素断言有了唯一的写法 |
| `include/vine/vsg/core/AllocationGate.hpp` + `src/core/AllocationGate.cpp` | 进程内堆增长门禁（glibc 堆用量，无插桩） | D2/D8：稳态帧零增长成为可失败的门禁 |
| `include/vine/vsg/core/Diagnostics.hpp` + `src/core/Diagnostics.cpp` | 单一诊断路由（分类计数 + sink 转发）+ `ReportOnce` | D8：报告与计数同源 |

用例：`tests/test_vsg/BackendCoreTest.cpp`（22 例）+ `tests/test_vsg/BackendEvidenceTest.cpp`（11 例），
全部零 GPU、零 `vsg::`：协议转移表；目标/深度计划表；时间轴与退役（“完成证据未到不得释放”、
“提交 ≠ 完成”）；arena（“增长不搬块”、reset 是唯一失效点、稳态零增长）；键审计（“extent 不进身份”）；
计数交叉校验；相位表输出格式；像素探针（行主序、子矩形、钳位、“什么都没画”就叫失败）；
诊断（分类计数、无 sink 也计数、episode 只报一次）；分配门禁（**稳态窗口一次也不分配**，
且“故意分配必须被抓到”这条用例保证了门禁能失败）。

**落地时被代码推翻的三处设计措辞**（本文已同步修正）：

1. v2 写“arena 增长时复制已发放的字节，所以只有 reset 会失效”——**不可能**：单一可增长缓冲一旦
   重分配，此前发出的每个 span 都指向已释放内存。用例当场抓到（读出垃圾值，并伴随堆损坏）。
   改为**分块**：增长加块、不搬块，“只有 reset 会失效”才成立。
2. v2 写 `kRetireDepth = slots + 1` 是推导值——落地时补上另一半：槽数**不在公开 API 上**
   （`Viewer::assignRecordAndSubmitTaskAndPresentation()` 里是局部硬编码），只能启动期探测/断言
   （`RecordAndSubmitTask::fence(i)` 越界返回 nullptr），不得写死 4。
3. v2 把分配门禁挂在 `scripts/alloc_trace.c`（LD_PRELOAD）上——两个理由换成进程内实现：
   ①那个文件自己写着 “an investigation instrument, **not a gate**”，而且只插桩 `mmap`（看不到帧内
   那些小分配）；②插桩分配器要在它正在包装的分配器里解析自己的真实函数。glibc 已经能在进程内回答
   “堆是否增长”，门禁只需要这一位。`alloc_trace.c` 保留原职（“内存去哪了”）。

### 11.2 M1a（2026-09-21）：设备地板 + 探测

M1 的第一半，也是新实现**第一次碰图形 API**——所以它正好把 §2.3 的边界试了一遍：

| 文件 | 是什么 | 在哪一侧 |
| --- | --- | --- |
| `include/vine/vsg/core/DeviceRequirements.hpp` + `src/core/DeviceRequirements.cpp` | 地板版本（major/minor 比较，patch 有意忽略）、必需特性表、`missingFeatureCount()`、`satisfiesRequirements()` | `core/`：无 API 头、可无设备穷举 |
| `include/vine/vsg/api/DeviceProbe.hpp` + `src/api/DeviceProbe.cpp` | 开一个即抛的 instance、枚举物理设备、把驱动答案翻成 `DeviceFacts`、关 instance | `api/`：唯一 include `volk.h` 的地方；不建设备、不建窗口 |

用例：`BackendCoreTest.cpp` 里的 `CoreDeviceRequirementsTest`（3 例：地板忽略 patch、七个特性各自有名有数、
缺一个就拒）+ `DeviceProbeTest.cpp`（无 loader / 无设备 ⇒ **SKIP** 而不是失败，“没装 Vulkan”与“装了但没设备”
是两回事）。

**实测（本机 lavapipe，2026-09-21）**：

```
[device_probe] name=llvmpipe (LLVM 21.1.8, 256 bits) api=1.4.335 features=7/7 usable=1
[device_probe] devices=1 usable=1 loader=1.4
```

⇒ 地板（1.4）与七个必需特性在仓库门禁用的软件光栅器上全部满足；探测与策略的判决一致（用例本身就在断言
“探测的 usable == 策略的 satisfiesRequirements”，不允许第二份意见）。

顺带记一个坑：`vine::String` 包的是 `std::u8string`（`value_type = char8_t`）——字面量必须写 `u8"…"`，
而驱动给的 `char[256]`（如 `deviceName`）与 printf/gtest 转发都要走仓库既有的
`reinterpret_cast<const char*>(s.data()), s.size()` 写法。

### 11.3 M1b（2026-09-21）：会话（自有窗口）+ 空帧提交 + 槽数的**逐帧学习**

| 文件 | 是什么 |
| --- | --- |
| `include/vine/vsg/api/Session.hpp` + `src/api/Session.cpp` | 会话：自有窗口 → 设备 → 交换链 → 帧配对（`beginFrame` / `commitFrame`）；API 全部藏在 PImpl 后（头文件里没有 `vsg::`） |
| `include/vine/vsg/core/SlotProbe.hpp` + `src/core/SlotProbe.cpp` | `probeSlotCount()`（问“现在能答多少个槽”）+ `SlotTracker`（逐帧折叠，直到数字不再增长） |

实测（lavapipe + X11，`SessionTest.EmptyFramesAreCommittedAndAParkedObjectWaitsForTheCompletionEvidence`）：
会话建立 → 学会槽数 → 空帧连续提交与呈现 → 在第 S+1 帧 park 一个对象 → 它在 `retire_at + slots` 帧
才被释放（早一帧的断言会红）→ 全程 `deviceWaits() == 0`。

**本轮最重要的发现（推翻了 v2 在 §9.1 的那条推断）**：**槽数不是一次探测就能得到的**。
上游的 `RecordAndSubmitTask::fence(i)` 走的是 `index(relative)` → `_indices[relative]`，而 `_indices` 每个
槽位在第一次 `advance()` 之前持哨兵值（`numBuffers`），**每帧只填一个** ⇒

* 第一帧探测得到 **1**，第二帧 2，直到填满才停在真值（本机 3）；
* “探测到 1 就当成窗口” = 早一帧销毁一个仍被已提交命令缓冲引用的对象（正是这个环要避开的事故）。

所以拆成两个类型：`probeSlotCount()` 只答“现在能答多少”，`SlotTracker` 负责“数字不再增长才算学会”
（两帧连续相等即饱和）。**学会之前不 park**：`RetirementQueue` 的 `retire()` 直接返回 false，调用方必须
退回计数过的 device wait（`parkingAvailable()` 是这条状态的判据）。学会后若与假设值（3）不一致，报一次
Info；一致则一句话也不说。

另一个落地才看清的时序细节：`RecordAndSubmitTask` 是在 `Viewer::assignRecordAndSubmitTaskAndPresentation()`
里建的（需要设备已存在），而它的槽表要等第一帧 `advance()` 才被填 ⇒ 探测的**最早可能时刻**是
“第一帧开帧之后”，不是 `initialize()` 里。

### 11.4 M1c（2026-09-21）：宿主句柄采纳 + 会话移动（不重建）

| 文件 | 是什么 |
| --- | --- |
| `include/vine/vsg/core/SessionMove.hpp` + `src/core/SessionMove.cpp` | 决策表：`MoveAction{Keep, Move, Rebuild}` × `MoveFacts{has_live_session, on_host_window, handle_announced, handle_differs}` → `MoveDecision{action, reason}` |
| `src/api/Session.cpp` | `SessionOptions::native_handle`；`initialize()` 先决策再动手；`moveTo()`；`moves()/rebuilds()/keeps()/generation()` |
| `tests/test_vsg/SessionMoveTest.cpp` | 真实 X11 窗口：同一句柄 → keep；第二个句柄 → move（`moves()==1`、`rebuilds()==0`、`generation()==2`、`deviceWaits()==1`、两个宿主窗口在 `shutdown()` 后都还活着） |

**决策表**（`planSessionMove()`，四条事实彼此独立，所以是表而不是嵌套 if）：

| 事实 | 动作 | 理由 |
| --- | --- | --- |
| 没有活会话 | `Rebuild` | 移动是给已有会话用的；首次建立只是“建在哪个句柄上” |
| 有活会话、不在宿主窗口上 | `Rebuild` | 自建窗口没有可转换的平台句柄；留在旧窗口上等于控件迁移后画面不动 |
| 有活会话、在宿主窗口上、句柄未变 | `Keep` | 同一个句柄：动它就只是白停一次设备（这是每帧重入 `initialize()` 的常态） |
| 有活会话、在宿主窗口上、句柄已变 | `Move` | 平台转换（xcb/Wayland X11 / HWND），设备与管线保留 |

**复用的是设施，不是策略**：`vine/vsg/detail` 里的 `VsgHostWindow`（`hostHandle()` / `moveToHostSurface()` /
“绝不销毁宿主的窗口”）与 `hostHandleFromVoid()` 保留原样，重写版只负责“什么时候让它动”。
移动失败（例如交换链格式与旧句柄不兼容）→ 报 Warning 并退回**重建**：失败路径就是决策表里的 `Rebuild` 分支，
不是第三套逻辑。

两次实测（lavapipe + X11）：

```text
[VsgHostWindow] attached to the host window 0x600000 (320x240, mapped=true)
[VsgHostWindow] moved to the host's new window 0x600001 (320x240); the device and its pipelines were kept
[       OK ] SessionMoveTest.ASecondHostWindowMovesTheSessionAndTheSameOneKeepsIt (43 ms)
```

**落地才看清的两件事（都是计数器/设计措辞导致的）**：

1. `vsg::Viewer` **没有** `device` 成员；停设备要走 `viewer->deviceWaitIdle()`
   （既有实现的 `VsgRetireRing::waitForIdle(viewer)` 用的就是它，见 §9 的引用约定）。
2. “学会槽数”时**不能**重建 `RetirementQueue`（`= RetirementQueue(slots)`），因为队列自己也数着“被计数的
   device idle”；重建会把那些数悄悄清零（测试里表现为 `deviceWaits()==0`，而移动明明发生了）。
   现在改成 `setSlots()` 就地改窗口 ⇒ 计数器与窗口互不干扰。

### 11.5 M2a（2026-09-21）：内容身份与存储策略（core）

M2（内容）落地时按前几个 M 的做法拆成三段：**M2a 身份与存储（纯 core，本节）**、M2b 变体池与每 pass 状态
注册表、M2c 设备侧（上传 / 描述符 re-point / 绘制录制 + 相位）。

| 文件 | 是什么 |
| --- | --- |
| `core/Streams` | `StreamKey`（**切片 + revision 就是身份**）、`GeometryStreams` / `GeometrySnapshot`、纯函数 `planGeometry`（`None` / `Refresh` / `Rebuild`）、`SharedStreams`（几何别名的登记；**不持有字节**） |
| `core/MaterialArena` | 材质块的持久存储：slot × in-flight 副本轮转；revision 未变 ⇒ 零写，变了 ⇒ 每帧恰好一次 64B 写 |
| `core/FrameRing` | 每帧 ring（视图块 / 绘制块）：容量固定，超预算 ⇒ 拒绝 + 计数（**不增长**） |
| `tests/test_vsg/ContentCoreTest.cpp` | 19 个无设备用例（Streams 7、SharedStreams 4、MaterialArena 5、FrameRing 3） |

**`planGeometry` 的判据表**（纯函数，输入只有两个快照）：

| 输入事实 | 动作 | 理由 |
| --- | --- | --- |
| 还没建过（快照为空） | `Rebuild`（允许共享上传） | 每个流都是新的 |
| 通道形状 / 通道集合 / 有无索引流变了 | `Rebuild` | 装配方式变了，不是字节变了 |
| 通道的字节动了（revision 变） | `Refresh`（列出动过的 location） | 逐流重指：重传的粒度就是一条通道 |
| 索引 **buffer** 换了、span 没变 | `Refresh`（`index_refreshed`） | bind 别名整个 buffer，draw 才声明 span |
| 索引 **span**（offset/count）变了 | `Rebuild` | span 写进了装配好的节点里 |
| 流全同、只有 geometry revision 动了 | `Rebuild` + **拒绝共享上传** | 这是“没有流能解释的声明”（应用透过裸指针改了字节），共享上传持有的是**插入当时**的字节，不能替它背书 |

**三条落地时钉住的边界**：

1. `SharedStreams` **不拥有字节**：容量到顶时最老的 entry 只是离开**查找表**（`evictions()` 数它），已经绑上它的
   读者照旧读自己那份 —— “entry 离开 map” 与 “字节可以释放” 是两件事，后者只能由持有 GPU 对象的层决定；
2. 派生通道（白透明度载体 / 零 UV / 算出来的法线）`buffer == nullptr`，**永远不能共享**：它的字节属于某一个几何；
3. `MaterialArena` 的写 **每材质每帧至多一次**：应用一帧内改两次得到的是 2 个 note、1 次 64B 写（GPU 只可能看到
   一个版本），稳态帧的 `writes()` 保持不变 —— 这就是 “材质稳态零流量” 的可断言形式。

### 11.6 M2b（2026-09-21）：管线变体池 + 每 pass 状态注册表（core）

| 文件 | 是什么 |
| --- | --- |
| `core/VariantPool` | 按 `PipelineKey` 索引的变体池：`acquire()` 答 `Created / Reused` + 一个**永不复用**的 id；FIFO 上界（65，与现有程序缓存同值）；`keyOf(id)` 让条目自己持有键对象 |
| `core/StateRegistry` | 每个 pass 一份：解析出「绑哪个变体」与「动态块要不要重发」两笔**分开的**代价（`variant_switched` / `dynamic_issued`） |
| `Keys` 新增 `pipelineKeyFingerprint`（`PipelineKeyHash`） | 键的哈希与 `operator==` 写在**同一处**：只有 `==` 决定是不是同一个变体，哈希只为查找 |
| `tests/test_vsg/VariantCoreTest.cpp` | 11 个无设备用例（池 5、注册表 6） |

**验收对应**（§7 里 M2 那条 “churn 下 `pipelineVariantCount` 不涨”）：

| 行为 | 断言 |
| --- | --- |
| 同一身份两次查找 | `created()==1`、`reused()==1`、同一个 id |
| 动态状态 churn（cull / polygon / topology / depth / blend 各 20 步） | `pool.created()==1`、`variants()==1`、`variant_switches()==1`、`dynamic_issued()==21` —— **动态层只花 set 命令** |
| 程序 / 布局 / 兼容性 / 深可采样 / shadow 任一改变 | `Created`（6 个键 ⇒ 6 个变体，同键再来仍是 `Reused`） |
| 两个 pass 用同一身份 | 各自 `variant_switched==true`（各自要绑），`pool.created()==1`（编译只有一次） |
| 值没变 | `dynamic_issued==false`（不重发）；换变体但值没变也**不**重发 |
| 容量到顶 | 最老的查找离开（`evictions()==1`），持有者仍持有自己的对象；被淘汰的键再 `acquire` ⇒ `Created`（id 不复用 ⇒ 陈旧 id 无法冒充新变体） |

**这条边界是刻意钉死的**：池只接受 `PipelineKey`（身份层）；`DynamicState` 永不进池 —— 测试通过 20 步 churn 后
`created()` 仍为 1 来证明，而不是靠注释保证（这正是 §6.1 第 3 条“键是显式结构体 + 审计测试”的行为半边）。

### 11.7 M2c-1（2026-09-21）：几何上传与共享（api）

M2c 拆成两段：**M2c-1 上传与共享（本节，已完）**、M2c-2 环/材质映射缓冲 + 描述符 re-point + 绘制录制 + 相位。

| 文件 | 是什么 |
| --- | --- |
| `api/StreamUploads` | 对象层：`StreamKey` → 共享 bind（`acquireVertex` / `acquireIndex` / `release`）；决策仍然由 `core::SharedStreams` 做，这里只持有对象并与之同步 |
| `core/Streams` | `Decision` 新增 `evicted`：容量到顶时把“离开 map 的键”告诉调用方 —— 对象层由此可与登记表对齐，而 core 里**没有回调** |
| `tests/test_vsg/StreamUploadsTest.cpp` | 10 个无设备用例 |

**可观测含义**：一个 bind 命令拥有自己的 `BufferInfo`，而 `BufferInfo` 决定“一个设备缓冲 + 一次上传”。所以
“两个 drawable 读同一流”有一个不需要 GPU 的判据 —— **它们拿到同一个 bind 对象**（测试断言指针相等）。
同理：切片不同 / revision 不同 / buffer 不同 ⇒ 不同 bind（那些是不同的字节）。

| 规则 | 结论 |
| --- | --- |
| 可共享 | 四个 canonical 顶点通道（且字节是模型自己的）+ 索引流 |
| 拒绝（loud + 计数） | 派生通道（白透明度载体等，属于某一个几何）、自定义通道（location ≥ 3：它们的 bind 身份是**整个自定义布局**的一条命令） |
| 索引键 | **归一化**为整个 buffer（无 offset/count）：bind 别名整个 buffer，draw 才声明 span ⇒ 同一 index arena 的两个切片共享一次上传；`release` 用同一个归一化 ⇒ 不需要调用方再给 span |
| 淘汰 | `Decision.evicted` ⇒ 对象层同步丢弃该 bind（已经绑上它的读者持有自己的 ref，读到的字节不受影响） |
| 两账一致 | `agreesWithRegistry()`（`live() == objects()`）是断言，不是假设 —— 与 `Observe` 的“同一份数据两个视图必须相等”同一条纪律 |

### 11.8 M2c-2a（2026-09-21）：每帧块存储（api，真设备）

M2c-2 再拆一次：**M2c-2a 块存储（本节，已完）**、M2c-2b 描述符 re-point + 绘制录制 + 相位。理由是两者的证据面不同：
存储的正确性（区域、旋转、稳态零写、拒绝不增长）可以在真设备上直接读回证明，而录制要等管线/描述符齐了才有像素。

| 文件 | 是什么 |
| --- | --- |
| `api/BlockStorage` | **一个**映射缓冲（HOST_VISIBLE｜HOST_COHERENT）装三类块：视图块 / 绘制块（`core::FrameRing` 决定本帧可写哪个 slab）+ 材质块（`core::MaterialArena` 决定 slot 与 in-flight 副本）；`writeView` / `writeDraw` / `writeMaterial` 直接把调用方的块字节 memcpy 进去 |
| `tests/test_vsg/BlockStorageTest.cpp` | 8 个用例，7 个在**真设备**上跑（X11 + lavapipe，否则 SKIP），1 个无设备（空设备 ⇒ 无存储） |

**为什么是映射内存**：这些块在帧在飞时还要被 GPU 读，`vsg::Data` 的 DYNAMIC 语义会在一个值变化时整块转移；
映射内存让帧自己的线程就地写、可见性由 COHERENT 保证、没有 staging 拷贝 —— 这正是“稳态帧零流量”的机制面。

| 规则 | 结论 |
| --- | --- |
| 布局 | 三段区域（视图 / 绘制 / 材质）在一个 buffer 里，区域起点按**设备的** `minUniformBufferOffsetAlignment` 对齐（调用方只给块的**大小**，从不给 offset 或 stride） |
| 旋转 | 视图/绘制各一个 ring：本帧写自己的 slab（3 帧在飞 ⇒ 3 个 slab，第 4 帧回到第一个）；材质按 in-flight 副本轮转 |
| 稳态 | 同一 revision 的材质**零写**（映射里的字节不变，测试直接读回证明）；真编辑每帧恰好一块 |
| 拒绝 | 超过区域 stride 的块 ⇒ `oversized()` 计数、**不消耗**本帧预算；超过 blocks_per_frame ⇒ `overflows()` 计数，buffer 不迁移 |
| 一帧内两次编辑 | 第二次是 `Unchanged`（GPU 只可能看到一个版本），映射里仍是draw 会用到的那个版本 |

真设备读数（lavapipe + X11，`BlockStorageTest`）：8/8 通过；region 不重叠、所有写入落在自己的区域内、读回等于写入。

### 11.9 M2c-2b-1（2026-09-21）：块描述符（api，真设备）

| 文件 | 是什么 |
| --- | --- |
| `api/BlockDescriptors` | 一个 set 布局（3 个 **dynamic uniform** 绑定：0 = 视图块、1 = 绘制块、2 = 材质块）+ 一个 set（每个绑定 `range` = 一块、descriptor 自身 offset = 0）+ `bind()`（按**绑定序**给 3 个 dynamic offset）+ `repoint()`（换存储、留布局） |
| `api/BlockStorage` | 新增 `strides()`：每区域两块之间的距离（含设备对齐补齐），即每个绑定的 `range` |
| `tests/test_vsg/BlockDescriptorsTest.cpp` | 5 个真设备用例（lavapipe + X11，否则 SKIP） |

| 规则 | 结论 |
| --- | --- |
| 一个 set 服务所有 draw | “哪一块”由 dynamic offset 决定；测试断言两次不同 offset 的 `bind()` 拿到**同一个** `descriptorSet` 对象 —— 这就是“编辑是一次写、新帧是一次偏移”的机制面 |
| 对齐即拒绝 | dynamic offset 必须是设备 `minUniformBufferOffsetAlignment` 的倍数（VUID-vkCmdBindDescriptorSets-pDynamicOffsets-01971）：不合规 ⇒ **不给命令** + `refusals()` 计数，而不是把验证错误交给驱动 |
| re-point | 换存储只换 set 元素、保留 layout；旧 set 不在这里死——绑定它的命令持有引用，调用方照常 park |
| 落地抓到的真 bug | `vsg::DescriptorBuffer::create` 的参数序是（bufferInfo、**目标绑定**、数组元素、类型）：把绑定写进数组元素槽会让三个 descriptor 全落在 binding 0 的元素 0/1/2 上（布局每绑定只有 1 个元素 ⇒ 越界），第 2 个用例当场抓住 |

### 11.10 下一步

### 11.10 M2c-2b-2a（2026-09-21）：内容管线与着色器（api，无设备）

| 文件 | 是什么 |
| --- | --- |
| `api/ContentPipeline` | GLSL 两段（vertex / fragment）→ `vsg::ShaderStage`（进程内 glslang）→ **一身份一个** `vsg::GraphicsPipeline`；布局 = 块集（set 0）+ 128B push；状态表 = 6 个 baked 状态 + 动态声明 |
| `tests/test_vsg/ContentPipelineTest.cpp` | 7 个**无设备**用例（`vsg::GraphicsPipeline` 在 `vsg::Context` 编译之前只是 create-info，GLSL→SPIR-V 不需要设备） |

| 规则 | 结论 |
| --- | --- |
| 一身份一对象 | 同一 `PipelineKey` 第二次 acquire 拿到**同一个** pipeline 指针（池决定，层只建造）；`pool.created()==1` |
| 动态半声明 | 既有实现的 9 项（depth test/write/compare、cull、front face、polygon、topology、blend enable/equation）**加 viewport 与 scissor** |
| 为什么多两项 | 这是重写版对既有实现的**有意改进**：既有实现把 extent 传进了 `makeContentShaderSet` ⇒ resize 会重建内容管线；声明 viewport/scissor 动态后，resize 只是换一条 set 命令，extent 真的不进身份（D3 的“像素尺寸是运行时资源”由此闭环） |
| keep-alive 不是所有者 | 池淘汰身份 ⇒ 层同步丢对象（`agreesWithPool()` 断言）；绑定它的命令持有自己的引用 |
| 说不了就不说 | 着色器编译失败 ⇒ `create()` 返回 null，而不是造一个永远画不出东西的层 |
| 三层键在真对象上闭环 | 20 步动态 churn：`pool.created()==1`、`layer.compiles()==1`、`registry.dynamic_issued()==21`；而 `revision` 变一次 ⇒ 恰好新 pipeline 一个 |

### 11.11 M2c-2b-2b-1（2026-09-21）：绘制录制（api，无设备）

| 文件 | 是什么 |
| --- | --- |
| `api/StateCommands` | 引擎状态 → API 命令的映射：`makeDynamicStateCommand`（复用既有的 `detail::SetDynamicState`，携带三个扩展入口点）、`makeViewportCommand` / `makeScissorCommand` |
| `api/ContentDraw` | 一次解析好的绘制 → `vsg::StateGroup`：`stateCommands` = [管线绑定（**仅换变体时**）、动态块（**仅值变化时**）、块描述符绑定]；children = `Commands`[viewport、scissor、顶点绑定、索引绑定、`DrawIndexed`] |
| `tests/test_vsg/ContentDrawTest.cpp` | 6 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| 两个“仅当” | 同一变体 + 同一状态连续两次绘制 ⇒ 第二次的 `stateCommands` 只剩块描述符绑定（`pipeline_binds==1`、`dynamic_commands==1`）——pass 的命令缓冲与**内容**成正比，而不是与绘制次数 |
| 动态块的槽 | `kDynamicStateSlot == 15`：槽是状态栈的**身份**不是优先级，取默认槽 0 会顶掉管线绑定（既有实现从驱动崩溃里学到的那条，这里断言钉住） |
| 换身份 ≠ 重发动态块 | 换变体要重绑管线，但动态值没变 ⇒ **不**重发（写这条用例时预期写错成 3 条命令，测试当场纠正为 2 条） |
| 两条引擎约定 | reverse-Z ⇒ `VK_COMPARE_OP_GREATER`；**front face = CLOCKWISE**（vsg 的投影翻转 Y，声明成直觉的 CCW 会让每个 cull 模式作用在错误的面——静默）；混合**恒开**，`blend.enabled` 只选因子 |
| 视口 / 裁剪 | 矩形是**命令**（这正是 extent 不进身份的机制）；scissor 的负原点与零面积按 API 要求钳制，而不是交给驱动 |
| 顺带修掉的偏差 | `ContentPipeline` 的 baked 常量原写成 `LESS_OR_EQUAL` + `COUNTER_CLOCKWISE`（照抄时的直觉值），已按引擎约定改为 `GREATER` + `CLOCKWISE` |

### 11.12 M2c-2b-2b-2a（2026-09-21）：离屏目标与读回（api，真设备、无窗口）

| 文件 | 是什么 |
| --- | --- |
| `api/Device` | **无窗口**的设备缝：instance（版本从 1.4 往下试，取 loader 接受的那个）→ 按 `core::DeviceRequirements` 逐设备挑选 → `vsg::Device` + graphics queue family |
| `api/DeviceFeatures` | 设备特性策略的**唯一拼写**（`static_assert(core::kDeviceFeatureCount == 7)`）；`Session` 改为调用它——同一个策略不能有两个拼写，否则"窗口进来的设备"与"无窗口的设备"能力不同 |
| `api/OffscreenTarget` | 一张图 + 一个 render pass（CLEAR、`initialLayout = UNDEFINED`、`finalLayout = TRANSFER_SRC`【**彩色部分已由 §11.16n 改为 SHADER_READ_ONLY**】）+ framebuffer + RenderGraph（`clearValues` + **`renderArea`**）+ capture 节点（`CopyImageToBuffer` + buffer barrier）→ `probe()` 给出紧凑 RGBA8 的 `core::PixelProbe` |
| `tests/test_vsg/OffscreenTargetTest.cpp` | 3 个真设备用例，**不需要窗口也不需要显示服务器**（lavapipe 头部路径） |

| 规则 | 结论 |
| --- | --- |
| 证据通道 | 读回是命令图的一部分（`capture()` 由调用方挂在 render graph **之后**）：同一命令缓冲、同一队列、顺序有保证；帧提交 + device idle 之后才 `probe()` |
| 为什么不需要 barrier | `initialLayout = UNDEFINED` + 每帧 CLEAR：上一帧内容被丢弃是**有意**的，这正是"读回不用 barrier 舞蹈"的代价与收益（该目标不支持 LOAD 上一帧——相位本来就每帧清）。**已修订（M3d-3b-8，§11.16n）**：读回的那一次拷贝此后自带一对 layout 转移（彩色附件改为按 SHADER_READ_ONLY 收尾，因为它是下一趟 pass 的纹理） |
| 落地抓到的坑 | vsg 的 `RenderGraph` 默认 `renderArea` 是**零面积**：pass 记录成功却一个像素都没清，读回全 0，看起来像"拷贝坏了"。设上 `renderArea` 后立刻通过——这类"沉默的空 pass"正是像素相位存在的理由 |
| DeviceProbe 重写 | `probePhysicalDevices()` 从 volk 版改为 vsg `Instance` + `PhysicalDevice::getFeatures<>` 版：**特性读取只剩一处实现**（`describePhysicalDevice`），并消掉新 api 层的 volk 依赖（include 顺序陷阱随之消失）；既有探测用例仍绿 |

### 11.13 M2c-2b-2b-2b（2026-09-21）：端到端像素证据（整条栈画一个三角形）

| 文件 | 是什么 |
| --- | --- |
| `tests/test_vsg/ContentStackTest.cpp` | 2 个真设备用例，**无窗口**：整条重写栈（`StreamUploads` → `BlockStorage` → `BlockDescriptors` → `ContentPipeline` → `ContentDraw` → `OffscreenTarget`）画三角形并读回像素 |

| 规则 | 结论 |
| --- | --- |
| **三重验证齐了** | ① 验证层干净（`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` 强制开启后零 VUID）② 计数器（`uploads==2`、`pool.created()==1`、`compiles==1`、`pipeline_binds==1`、`dynamic_commands==1`、`materialWrites==1`）③ **像素**（中心是片元色、两角是 clear 色） |
| dynamic offset 的端到端证明 | 一帧两个 draw、两个绘制块：左三角与右三角各自落位，中间**仍是 clear 色**——若偏移、描述符或录制任何一处错，画面会是"两个三角重叠 / 只有一个 / 什么都没有"，而这三个都是计数器看不出来的 |
| 动态入口点 | 测试用 `detail::fetchDynamicStateEntryPoints(device->vk(), instance->vk())` 取真实的三个函数指针（会话将来同样取）：不取则多边形模式与混合调用被跳过，画面变成 create-info 的值 |
| 落地抓到的坑 | 断言助手把 clear 色写成"深蓝 `b > 150`"，而 0.25 蓝量化后是 **64**：像素相位的第一原则是**读回的值说了算**——期望值必须来自同一量化，这次是把 `(0, 0, 64, 255)` 打出来才看清的 |
| 里程碑状态 | **内容绘制（M2）到此完整**：身份/计划/别名/材质/ring（M2a）、变体池 + 状态注册表（M2b）、共享上传（M2c-1）、块存储与描述符（M2c-2a/2b-1）、管线与录制（M2c-2b-2a/2b-1）、端到端像素（2b-2b） |

### 11.14 M2c-3（2026-09-21）：会话侧内容挂载（内容经 `Session` 进帧）

| 文件 | 是什么 |
| --- | --- |
| `api/SessionContent.hpp` | api 内部的三件套：`root(session)`（会话每帧渲染的组）、`device(session)`（内容必须建在**会话的设备**上）、`recompile(session)`（把后来挂上的内容编译出来）；`Session` 只加了一行 friend，公开头文件仍无 `vsg::` |
| `src/api/Session.cpp` | `initialize()` 创建内容根并挂到 render graph（**在 compile 之前**）；`shutdown()` 随会话丢弃；三个 accessor 的定义 |
| `tests/test_vsg/SessionContentTest.cpp` | 真设备 + X11 用例：整条栈的内容挂进会话 → 提交 3 帧 → 用 `xcb_get_image` 读**窗口自己的像素**断言 |

| 规则 | 结论 |
| --- | --- |
| 为什么需要 recompile | vsg 编译一次并保留已建对象；后挂的内容没有实现、根本录制不出来。再跑一次 compile 只补新对象（管线实现存在即跳过），不必重建会话、不重编管线 |
| 证据 | 窗口中心 = 片元色、角落 ≠ 片元色（"整窗一个颜色"过不了这一关）；`framesPresented()>=3`、`deviceWaits()==0`、`draws==1 / binds==1 / dynamic==1`；**强制验证层下 0 VUID** |
| 内容必须在会话的设备上 | accessor 给出会话自己的设备（内容建在别的设备上，会话的命令缓冲引用不了） |
| **落地抓到的坑（只有像素能报的）** | 同一份内容在离屏目标（**无深度附件**）里画得出，在窗口里**画不出**：窗口的 pass 有深度附件、按 reverse-Z 清到 **0.0**，而深度比较是 `GREATER` ⇒ 三角形若躺在 z = 0 就被深度测试丢弃。离屏相位永远发现不了这个差异——这正是"两个入口都要像素证据"的理由 |

### 11.15 M3a（2026-09-21）：清屏/加载策略（无设备可判）

| 文件 | 是什么 |
| --- | --- |
| `core/ClearPlan.hpp` | `ClearPolicy`（这一趟 pass 想清什么：`color` / `color_value` / `depth` / `depth_value`）、`AttachmentClear`、`DepthClear`、`PassClearPlan`、常量 `kReverseZFarDepth = 0.0F`，以及纯函数 `planClearValues(shape, policy, bootstrap, depth_preserved)` |
| `src/core/ClearPlan.cpp` | 四条规则的实现（下表）；四个类型的相等比较 |
| `tests/test_vsg/ClearPlanTest.cpp` | 规则表的 6 个用例，无设备 |

| 规则 | 内容 | 为什么是它 |
| --- | --- | --- |
| 1 | `bootstrap` ⇒ **每个**附件都清：附件 0 取 policy 的颜色、**额外附件保持透明黑** `{0,0,0,0}`、深度取 policy 深度 | 新建（或已失效）的镜像初始布局是 `UNDEFINED`，**不能 LOAD**；清屏是唯一合法的第一笔。额外附件涂成 pass 颜色是"看起来很合理"的错误画面——单附件用例永远发现不了 |
| 2 | 非 bootstrap 且没要求清 ⇒ 该附件 `Load` | 默认是"保留"，不是"清掉"：清屏是开销，也是内容的破坏 |
| 3 | `depth_preserved` ⇒ 深度**永不清**，**且压过 bootstrap** | 深度被后一趟 pass 依赖时（延迟渲染的前置 pass），清掉它会让后一趟读到远平面、把每个片元当成可见 |
| 4 | 要求清颜色 ⇒ **所有**颜色附件都清，但**只有附件 0 拿到这一趟的颜色** | 与规则 1 同一件事的不同入口：`LoadOp` 是逐附件的，颜色值是"这一趟的画面" |

| 结论 | 内容 |
| --- | --- |
| 反转 Z 的远平面是 `0.0` | `kReverseZFarDepth = 0.0F`。深度清到 `1.0` 配 `GREATER` 比较 ⇒ 每个片元都被丢弃（空画面），而这个常量是唯一能把这个约定写成可断言事实的地方 |
| 为什么是纯函数 | 三个输入（形状、policy、bootstrap/depth_preserved）决定整张表，表可以无设备钉死；设备侧只负责把 `Load`/`Clear` 翻译成 API 枚举 |
| 与 `planTarget` 的分工 | `planTarget` 回答"这个目标要不要重建/换镜像"（`RepairReason::Bootstrap` 就是规则 1 的触发点），`planClearValues` 回答"这一趟 pass 的附件各自怎么开始、怎么结束" |

### 11.16 M3b（2026-09-21）：多附件目标（MRT + 深度）与逐附件读回

| 文件 | 是什么 |
| --- | --- |
| `api/OffscreenTarget.hpp` | 新增 `TargetLayout`（引擎语汇的形状：`color_formats` 多个 + 可选 `depth_format` + `core::ClearPolicy`）；旧 `Layout` 保留为等价的单附件写法。新增 `capture(i)` / `probe(i)` / `colorAttachmentCount()` / `hasDepth()` |
| `src/api/OffscreenTarget.cpp` | 每附件一张镜像 + 视图；pass 的附件描述与 clear value 全部由 `planClearValues`（bootstrap 恒开）驱动；**清屏值按附件顺序**排布；每个颜色附件各有一块 host-visible 目的缓冲与一个 copy 节点 |
| `tests/test_vsg/MrtTargetTest.cpp` | 真设备用例 2 个：清屏分量（附件 0 = policy 颜色、附件 1 = 透明黑）与双输出着色器（附件 0 绿、附件 1 红） |

| 规则 | 结论 |
| --- | --- |
| 形状用引擎语汇 | `TargetLayout` 收 `RenderTarget::ColorFormat` / `DepthFormat`，转 API 枚举的是 api 层（与 `VsgPipelineFactory` 同一张映射表），core 不需要认识 `VkFormat` |
| bootstrap 恒开 | 目标的镜像每帧以 `UNDEFINED` 起始、pass 每帧清屏，所以"这趟是 bootstrap"在这个目标上是永远为真的事实；与其让调用方猜，不如由目标自己声明，并在计划里出现 `Load` 时**拒绝创建**（LOAD 一张 `UNDEFINED` 镜像不是策略选择，是 bug） |
| 深度附件的依赖 | 有深度时子依赖要覆盖 `EARLY/LATE_FRAGMENT_TESTS`，否则上一帧的深度读取会和这一帧的深度写入重叠 |
| **落地抓到的坑** | MRT 的"第二个附件等于第一个"很难被察觉：只要 `capture(i)` 落到同一块缓冲，`probe(1)` 就会返回附件 0 的像素，画面看起来"MRT 成功了"。所以证据必须是**两个不同的片元输出**（绿 / 红），而不是同一个颜色的两次复制。另外：片元阶段**没写**的附件在 MRT 下内容是 `UNDEFINED`（不是清屏值），所以"不清也能看到清屏色"这件事本身不是证据 |
| 三重验证 | 6 + 2 = 8 个新用例；`test_vsg` 全量 **444 用例 / 70 套件全绿**（含既有 `OffscreenTargetTest` 走旧 `Layout` 入口，证明兼容）；`check_include_hygiene.py` 0 findings / 769 文件 |

### 11.16b M3c（2026-09-21）：借用的深度，以及装上验证层后当场抓到的两个真 bug

| 文件 | 是什么 |
| --- | --- |
| `api/OffscreenTarget` | `create(device, layout, depth_source)`：借用方不建镜像、用出借方的视图、`initialLayout = DEPTH_STENCIL_ATTACHMENT_OPTIMAL`、按 `depth_preserved` LOAD；`depth()` 把事实交给 `core::depthPlan`（`borrowed` 恒 `sampleable=false`；出借方被借用 ⇒ 提升被撤销，借用方析构即恢复）；`captureDepth()` / `depthProbe()` |
| `core/DepthProbe`（新） | 深度读回的数据面：`valid()` / `depthAt()` / `countNear(value, tolerance)`；越界答 0 而不是越界读 |
| `core/DeviceRequirements` | 设备地板补上**扩展**那一半：`DeviceExtension`（EDS2 / EDS3）+ `missingExtensionCount` + `extensionName`；`satisfiesRequirements` 同时要求版本 ∧ 特征 ∧ 扩展 |
| `api/DeviceFeatures` | `requiredDeviceExtensions()`：名字与上面 7 个特征位是**同一条策略的两半**，`static_assert` 钉住数量 |
| `api/Device` / `api/Session` | 两处建设备的地方都启用这两个扩展名；`createDevice` 现在把 `vsg::Instance::create` 的**异常**当成"换下一个版本"而不是直接终止；验证层不可用时**降级并如实报告**（`DeviceResult::validation`） |
| `tests/test_vsg/SharedDepthTest.cpp` | 真设备用例 2 个：事实/提升撤销/拒绝（无深度、格式不匹配）；像素 + 深度双重证据 |

| 规则 | 结论 |
| --- | --- |
| **借用的深度不是 pass 级的细节** | `VkSubpassDependency` 的 stage/access mask **属于 render-pass 兼容性**（验证层点名 `VUID-vkCmdDrawIndexed-renderPass-02684`）：借用方为了读别人的深度而多声明一个 `DEPTH_STENCIL_ATTACHMENT_READ`，就会和出借方**不兼容**——同一个管线、同一个形状、两个不能共用的 pass。所以依赖按**形状**决定（有深度就把 early/late fragment tests 与 depth 读写全写上，取超集），而 loadOp 与 layout 才允许逐 pass 不同 |
| **扩展没启用，动态状态就是非法的** | `VK_DYNAMIC_STATE_POLYGON_MODE_EXT` / `COLOR_BLEND_ENABLE_EXT` / `COLOR_BLEND_EQUATION_EXT` 属于 `VK_EXT_extended_dynamic_state3`（1.4 也没吸收）。之前的设备缝只请求了**特征位**、没有启用**扩展名** ⇒ 管线 create-info 非法、`vkCmdSet...EXT` 通过函数指针调用到未启用的能力上。**只有验证层能看见这一类错误**：像素和计数器都可以全绿 |
| **验证层是仪器，不是地板** | 它是单独的软件包，很多机器没有。缺了要**降级并报告**（`DeviceResult::validation`、测试里打印 warning），不能让"验证干净 ∧ 计数器 ∧ 像素"悄悄变成两重。仓库自己的 lavapipe gate 也是这个态度（warn and proceed） |
| **Vulkan 的 NDC z 就是深度** | 顶点写 `gl_Position.z = z` ⇒ 默认视口下深度就是 `z`，**没有** OpenGL 的 `z*0.5+0.5`。写错的后果极像"被遮挡"：把"远处"三角形写在 z = -0.5 就是落到了远平面之外，**到处**被 `GREATER` 拒绝，画面与"共享深度生效"一模一样。最后是**深度读回**把这件事一句话讲清楚（0.5 / 0.1 / 0.0 三个值），颜色图永远说不清 |
| 深度读回的诚实 | D32 → 原样 float，D16 → ÷65535；**D24S8 拒绝**（组合格式没有普通深度拷贝语义，按 float 读出来的数字"像深度但不是"）。镜像要带 `TRANSFER_SRC`，否则拷贝是验证错误；拷贝前后各一次 image barrier，把 layout 还原成附件布局——不然共享给后一趟 pass 的深度就废了 |
| 证据 | 颜色：出借方红、借用方在被遮挡处**清屏色**（不是绿）、借用方自己的三角形绿。深度：出借方三角形处 `≈0.5`、清屏处 `0.0`、借用方自己的三角形处 `0.1`，且**每个 texel 恰好是这三个值之一**（第四种值就意味着有别的 pass 写过深度）。计数器：`draws=3 / refusals=0`；`test_vsg` 全量 **448 用例 / 72 套件全绿**，全量输出 `Validation Error` 计数 **0**；hygiene 0 findings / 772 文件 |

### 11.16c M3d-1（2026-09-21）：帧的收集段（`core/FrameRecorder`，无设备）

M3d（“执行器按 `Schedule` 机械落地”）按**证据面**拆成三段：**M3d-1 收集（本节，已完）**、
M3d-2 编译（`FrameCompiler`：normalize → `FrameGraph` → `Schedule` → `CompiledFrame`，含环的跳过语义）、
M3d-3 执行（`api/VsgExecutor`：把已经决定好的计划机械地变成 vsg 图 / 提交 / 呈现）。拆法与 M2c 同一把尺子：
收集段的行为能在无设备下逐条钉死（arena 归属、消费规则、三种裁决），而执行段要有像素才谈得上证据。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameRecorder.hpp` | 收集段的类型与入口：`PassId`、`ProgramRef`、`InputRef`、`LightRef`、`CameraSnapshot`、`CollectedCommand`、`DrawKind`、`CollectedDraw`（= **一次绘制调用**）、`CollectedPass`、`FrameDescription`、`FrameRecorder` |
| `src/core/FrameRecorder.cpp` | 规则实现：协议先行、arena 快照、scope 属性 vs 逐绘制消费、三种裁决的落地与上报 |
| `core/Protocol` | 新增 `droppedCount()` 与 `frameOpen()`；`Drop` 的两个出口收敛到一处计数（Drop 是唯一静默的裁决，只有计数能把它从“什么都没发生”里分出来） |
| `tests/test_vsg/FrameRecorderTest.cpp` | 14 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **arena 独占（P0-1）** | 计划里没有一个 span 指向宿主容器：`CollectedCommand` 在 `render()` 里复制，`CollectedDraw`/`CollectedPass` 在 `endPass()`/`endFrame()` 复制。用例按引擎自己的模式驱动（一个 vector 清空重填）后断言**第一趟 pass 的记录仍按字节等于它当时拿到的值** |
| **一次绘制调用，不是一条命令** | 粒度就是调用，因为契约恰在这个粒度上消费公告，视口/深度也只在调用级有意义；相机按调用记一次而不是按命令（1000 条命令不该抄 1000 份矩阵）。与契约同一句话：`render()` 里的 1000 条命令共享这一次调用的全部公告 |
| **逐绘制消费 vs pass 属性** | 视口 / 光照被“下一次绘制调用”消费（SDK 原文：one announcement serves ONE drawing call）；**输入属于 pass**（引擎自己只宣布一次并写明 “every draw call in it sees the same list”）；target / order / clear / depth 属于 scope，`endPass()` 丢弃。§5 草图把 `lights` 画在 pass 级：那是草图与契约的分歧，落地按契约走 |
| **没画东西的 scope 不进计划，除非它宣布了 clear** | 清屏走的是该 pass 自己的 load-op，丢掉一个“只清屏”的 pass 的代价是画面底色没了；只宣布 target/order/viewport 却什么都没画的 scope 丢掉是**对的**（“未消费的 scope 属性在 endPass 丢弃”的可观测形式） |
| **三种裁决各自落地** | `Allow`→记录；`Drop`→不记录、不报（无 scope 的 setter 是合法的惰性），但计数；`Refuse`→不记录、上报一次/每 episode。episode 的边界由 `Protocol` 拥有，录制器不留第二份“报过没有”的标志 |
| **上报消息带数字** | `frame 3: a drawing call arrives with no pass scope open: it is refused and nothing is drawn...`；“死 scope”与“无 scope”是两句不同的话（前者说“这是那个被释放的 target”，后者说“下一个 pass 从空请求开始”） |
| **借用的东西只抄不留** | 灯→`LightRef` 数字（用例在 endFrame 之后 `reset()` 掉 `Light` 再断言快照）；相机→`CameraSnapshot`（VIEW/PROJ + eye/target/up）；程序→identity+revision；几何→identity+revision。材质只记身份：SDK 的 `Material` **没有** revision 访问器，材质 revision 的来源在 api 层材质管理器（那是它的账） |
| **default program 不走状态机** | 它在所有状态下都合法，问协议只会得到一个恒为 `Allow` 的 `CallKind`；录制器记身份+revision，但**不替换**命令里为 null 的 program——“缺省未解析”是收集侧允许的形状，解析是编译器的活（P0-2） |
| **死 scope 不会被新公告救活** | 被释放的 target 让该 scope 余下的生命失效；后续 `setRenderTarget` 按 `Drop` 落地（上报一次），之后的绘制仍被拒。旧实现注释里的“新的 `setRenderTarget` 会清掉标记”是**有意不保留**的行为：协议已经钉死，录制器不绕过裁决——分歧写在这里，而不是埋在实现对注释的偏离里 |

| 变异反证（两条都实测跑过） | 结果 |
| --- | --- |
| 计划别名收集器的 scratch 而不是 arena（把 `endPass` 的 arena 复制换回 `span(open_draws_.data(), size())`） | `TheHostsReusedCommandListIsCopiedAtTheCallNotHeld` 与 `OneViewportAndLightAnnouncementServesOneDrawingCall` 变红，其余 12 绿 |
| 公告不被绘制调用消费（去掉 `render()` 里的三行消费） | `OneViewportAndLightAnnouncementServesOneDrawingCall` 变红，其余 13 绿 |

| 结论 | 内容 |
| --- | --- |
| 计数只有一处 | 进入计划的 pass 与收集到的绘制调用记在 `Observe`（相位门禁读的那一份）；被拒/被丢弃的次数记在 `Protocol`。录制器**不留**自己的账，同一件事两个计数器就是同一件事两个谎言 |
| 与计数聚合一致 | 被拒的绘制**不**计入 `draws`：用例断言“两次无 scope 绘制 ⇒ `refusalCount()==2` 而 `draws==0`” |
| 边界没被偷偷扩大 | 录制器只答“这次调用合法吗、它记录成什么数据”：不判 rebuild / resize / borrow / 顺序 / 缓存。编译器需要的一切都是描述里的**事实**（这条是 M3d-2 的入口条件） |
| 证据 | `test_vsg` 全量 **462 用例 / 73 套件全绿**（+14）；插件目标 `gfx_backend_vsg` 同样编过（GLOB 收进新 core 文件，插件与测试编的是同一份源）；hygiene **0 findings / 775 文件** |

### 11.16d M3d-2（2026-09-21）：帧的编译段（`core/FrameGraph` + `core/FrameCompiler`，无设备）

| 文件 | 是什么 |
| --- | --- |
| `core/FrameGraph.hpp` / `src/core/FrameGraph.cpp` | 依赖图 + **稳定**拓扑序（`(order, 调用序)` 最小者先）+ Tarjan（**迭代版**：pass 数是宿主的数字，不该上 C++ 调用栈）找强连通分量 |
| `core/FrameCompiler.hpp` / `src/core/FrameCompiler.cpp` | normalize（视口/程序/动态层/清屏+bootstrap）→ `planTarget` / `depthPlan` → 建边 → `Schedule` → `CompiledFrame`；输入是描述 + `FrameFacts`（api 层自己的 target 账） |
| `core/Keys` | 新增 `resolveDynamicState`：**深度意图的唯一裁决点**（显式 StateNode 赢、否则跟 pass），其余动态项只有一个来源 |
| `core/Observe` | 新增 `invalid_schedules` |
| `tests/test_vsg/FrameGraphTest.cpp` / `FrameCompilerTest.cpp` | 10 + 12 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **顺序是依赖图的答案，不是调用顺序** | 稳定 = 在所有可跑者里取 `(公告 order, 调用序)` 最小者。公告顺序是画面叠放的决定（契约原文 "ascending pass order"、同序保持注册序），一个忽略它的拓扑排序会静默把画面重新叠一遍 |
| **环 = 跳过整个强连通分量** | 三条失败语义各自落地：① 上报 Error（列出分量里的 pass，**每分量一次/每帧**）；② 分量成员不进计划（不是"按当前顺序偷偷画"，是不画）；③ 分量外的 pass 照旧，帧照旧提交呈现；计数器 `invalid_schedules` **按分量 +1**（两个独立环是两个问题），自检断言恒 0 |
| **只读一个被跳过 pass 的 pass 仍然要跑** | 它的输入本帧没人生产——这正是契约里本来就存在的状态（`setPassInputs` 的 null）。把消费者一起停掉会让一个两 pass 的环吞掉整帧 |
| **边只有两种** | 采样边（输入目标的生产者 = 本帧写入它的 pass）+ 深度借用边（出借目标的生产者）。**自我边不产生**：pass 采样自己画的目标是 feedback 模式（执行器判定，见 M2c 的 source==destination 拒绝），调度器把它当环会静默跳过每一个这样的 pass |
| **bootstrap = 该目标的第一个写者** | 写 = 有绘制或宣布了 clear；且目标的 attachments 本帧新建/换镜像（`Rebuild` / `ResizeInPlace` / `Repair(Bootstrap)`）。后续写者 LOAD——在那里再来一次清屏会把第一个写者画的东西擦掉 |
| **0×0 目标不是错误** | 没有任何东西能画进去 ⇒ 它的 pass 不进计划，**也不上报**（这是状态，不是错；目标层的构建失败另有报告）。用例断言 `diagnostics.clean()` |
| **未知目标不静默** | 后端从没听说过的 target ⇒ 不进计划 + `ContentSkipped`(Error)。用一个空的 compatibility 或未建的 framebuffer 顶过去就是静默跳过 |
| **缺省全部解析** | 视口（没公告 = 整目标，逐 draw 落定）、程序（命令没有自己的 = 帧的 default，**在编译期替换**）、动态层（`resolveDynamicState`）、清屏（有效 policy + `bootstrap` / `depth_preserved` 两个**执行器推不出来**的事实） |

| 有意偏差（写在这里，不埋在实现里） | 内容 |
| --- | --- |
| `CompiledPass::compatibility` 没有 materialize | §5 草图里它是一份 `RenderPassCompatibility`，而该类型带 `std::vector` ⇒ 逐 pass 复制等于每帧每 pass 一次堆分配，而执行器本就按 target 身份持有同一份 shape。计划改为 `target_index` 指向 `CompiledFrame::targets`，兼容性由执行器按身份取。**回报**：整个计划里**零 vector**（§7 的“稳态帧零分配”从 M3d-3 起就成立） |
| 逐 draw 的 compare op 仍未进动态层 | `core::DynamicState` 没有 compare 字段；M2c 的 `ContentPipeline` 按 §11.11 把 `GREATER` 烘进管线（引擎 reverse-Z 约定），而**旧实现**用 `RenderStateMapper::mapCompareOp` 按 `DepthState.compare` 逐 draw 映射。⇒ 内容自定比较算子的画面，新旧后端的答案不同。**登记为待办**（把 compare 加进 `DynamicState` 要动 `Keys.hpp` 的审计表，是独立小批次） |

| 落地抓到的坑 | 内容 |
| --- | --- |
| 用例当场抓到真 bug | 不可服务的 pass（未知目标 / 0×0）只标了 `servable_=0`，**没有** `graph_.exclude()` ⇒ 它仍被排进 schedule，解析阶段用 `kNoTarget` 去索引 target 表（`stl_vector.h` 的越界断言当场炸）。修法：两处都 exclude，并在解析循环里留一条"不可达但把前置条件放在本地"的守卫 |
| 变异反证 ×2（都实测） | ① 不认环（`cyclic = false`）⇒ **5 个用例红**（图 3 + 编译 2），其余 17 绿；② `freshAttachments` 恒 false ⇒ bootstrap 那条红 |

| 结论 | 内容 |
| --- | --- |
| 与计数聚合一致 | `invalid_schedules` 是被跳过**分量**的数；被跳过的 pass 不进 `CompiledFrame::passes`，而 `counters().passes` 记的是收集到的 scope——两个数的差就是本帧丢掉的 pass，相位可断言 |
| 边界没被扩大 | 编译器不碰任何 API 类型（事实是纯值、计划是纯值），不建资源、不选 GPU 对象；`planTarget` / `depthPlan` 的答案照读，不自造策略 |
| 证据 | `test_vsg` 全量 **484 用例 / 75 套件全绿**（+22）；插件目标 `gfx_backend_vsg` 编过；hygiene **0 findings / 781 文件** |

### 11.16e M3d-3a（2026-09-21）：执行段的第一片（`api/VsgExecutor`，真设备、无窗口）

M3d-3 按“**能证明什么**”再拆：**M3d-3a 记录顺序 + 一个 pass scope = 一个 render pass**（本节，已完）、
M3d-3b 内容绘制（几何/块/描述符经计划进 render graph，`ContentDraw` 接进来）、M3d-3c 会话侧（窗口 pass、
`present`、帧计数与退役推进）。理由：**记录顺序是整个三段式拆分存在的理由**，而它恰好可以用清屏色证明——
不需要先有内容路径。

| 文件 | 是什么 |
| --- | --- |
| `api/VsgExecutor.hpp` / `src/api/VsgExecutor.cpp` | 逐 pass 走 `CompiledFrame`：按 `target_index` 解析 target → 取该 pass 的 render graph（带自己的清屏值）→ 按计划顺序挂进 command graph；所有 target 的 capture 节点**最后**加；不服务的 pass 报告而不是画到别处 |
| `api/OffscreenTarget` | 新增 `passGraph(policy)`：在**同一个** render pass + framebuffer 上造一张带本 pass 清屏值的 `RenderGraph`；清屏值填充抽成与构造函数共用的 `fillClearValues`（一处规则，两处使用） |
| `tests/test_vsg/ExecutorTest.cpp` | 2 个真设备用例（lavapipe 无窗口；否则 SKIP） |

| 规则 | 结论 |
| --- | --- |
| **一个 pass scope = 一个 render pass 实例** | vsg 的一 `RenderGraph` 就是一次 begin/end render pass。清屏**值**属于 pass，render pass / framebuffer / load op 属于 target ⇒ 同一 target 上的两个 pass 是两张图，各清各的颜色，后录的那张胜出 |
| **记录顺序 = 计划顺序（像素可查）** | 用例故意让两个 target 的“公告序”与“调度序”相反：A=红(10)/ 绿(0) ⇒ 期望**红**；B=绿(10)/ 红(0) ⇒ 期望**绿**。(A=红, B=绿) 这对组合只有走 schedule 才可能，“按调用序录”只会得到 (绿, 绿) |
| **记录序列也是证据** | `recorded()` 断言 {2,4,1,3}（调度序），而不只是“四个都录了”：失败时能看出是哪一级动了 |
| **capture 最后加** | 探针要读“最后一个写该 target 的 pass”留下的画面；夹在两个 pass 之间的拷贝读到的不是本帧的终态 |
| **不服务的 pass 必须响** | 默认 framebuffer（窗口 pass 未接）、未注册的 target、无附件的 target ⇒ `Warning`/`ContentSkipped` + `skipped()` 计数 + `record()` 返回 false（这是部分帧，调用方要知道）。默认 framebuffer 那句还会点名“这一层还没接”，而不是把内容画到别的 target 上 |

| 落地抓到的 | 内容 |
| --- | --- |
| **变异反证（端到端）** | 让调度器忽略公告 order（`ready.emplace(0, node)`）⇒ `FrameGraphTest` 的排序用例与 **`ExecutorTest` 的像素用例**同时变红，其余 22 绿。⇒ 那条像素断言测的是“**画面跟着 schedule 走**”，不是“图能提交” |
| 工具 | `GTEST_SKIP() << vine::String` 编不过（u8 串进不了 gtest 的消息流）：要 `.as_std_str()` |

| 结论 | 内容 |
| --- | --- |
| 边界 | 执行器只把计划变成 API 对象：不判 rebuild / resize / 顺序 / 缓存，也不挑管线或描述符（那两样在 M3d-3b 接内容时进来，但也仍是“按计划里的身份取对象”） |
| 与旧实现解耦 | 执行器不认识 `SceneBridge`：它按 **identity** 解析 target，与 api 层其余部分（`Session` / `OffscreenTarget`）用同一张表 |
| 清屏值这一半已到位 | M3a 的 load-op 变体有两半：**清屏值**（本节已能逐 pass 变化）与 **LOAD/CLEAR 操作**（仍归 target：镜像每帧 UNDEFINED ⇒ 必须清）。后者要等“带历史的 target”（跨帧保留 / 深度保留的 LOAD 路径） |
| 证据 | `test_vsg` 全量 **486 用例 / 76 套件全绿**（+2，含真设备）；插件目标 `gfx_backend_vsg` 编过；hygiene **0 findings / 784 文件** |

### 11.16f M3d-3b-0（2026-09-21）：计划带上“管线身份要的 pass 侧事实”，执行器核对计划与资源世界

M3d-3b（内容绘制进 render graph）开工前先把它的**两处前提**钉住：

1. 一个 `PipelineKey` 里有两项**只能从 pass/target 得出**：写了几个颜色附件（`color_attachments`）与目标是否提供可采样深度
   （`depth_sampleable`）；它们现在由编译器从**已经查到的同一份 target 事实**解析进 `CompiledPass`，调用方不必再问一次。
2. M3d-2 记下的偏差（“不 materialize `RenderPassCompatibility`，因为带 vector”）需要一个**按身份取兼容性的口子**：
   `OffscreenTarget::shape()` 返回它建 render pass 用的那份形状（返回副本，无借用），执行器由此命名 key 的兼容性半边。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameCompiler` | `CompiledPass::color_attachments` / `depth_sampleable`；按 slot 缓存附件数，避免逐 pass 重算 |
| `api/OffscreenTarget` | `shape()`（返回 `core::TargetShape`） |
| `api/VsgExecutor` | 记录一个 pass 前**核对**计划与 target：附件数或深度可采样不一致 ⇒ `Warning`/`ContentSkipped` + 跳过 + `record()` 返回 false |
| `tests/test_vsg` | 编译器 +1（无设备，含“保留的深度撤销提升”那一面）；执行器 +1（真设备：故意让事实漂移到两个附件） |

| 规则 | 结论 |
| --- | --- |
| **同一个事实只答一次** | 两项都从编译器已经拿来建 target 表的事实里解析，所以“计划说的”与“执行器要的”不可能给出两个答案 |
| **计划与资源世界必须对得上** | 计划描述的形状与它解析到的 target 不一致 = 上游事实漂了，而**按错形状建的管线与宿主要的画面没有任何关系**。这个不一致只在执行器这一处能被发现，所以在那里报，而不是交给驱动 |
| 证据 | `test_vsg` 全量 **488 用例 / 76 套件全绿**（+2）；插件目标编过；hygiene 0 / 784 文件 |

### 11.16g M3d-3b-1（2026-09-21）：内容“按 pass 放置”，状态全部来自计划

内容世界（管线 / 流 / 块 / 材质）属于拥有它的那一层，不属于执行器。所以这一片的接缝是**数据而不是接口**：
调用方把已录制好的内容按 pass 交给执行器（`PassContent{ pass, content }`），执行器只回答一个问题——
**它进的是不是计划说的那个 pass**。这样既不需要在 api 里提前发明一套“内容工厂”抽象，又让“记录顺序 / 清屏值 /
视口由计划决定”这些主张保持可验证。

| 文件 | 是什么 |
| --- | --- |
| `api/VsgExecutor` | `PassContent` + `record(frame, graph, content)`：匹配同一个 `PassId` 的内容作为该 pass render graph 的子节点；**计划里没有的 pass 的内容会被报**（它的 pass 可能因为环被跳过、或 target 不可服务） |
| `tests/test_vsg/ExecutorTest.cpp` | 新增真设备用例：一个 pass + 一个 render command，内容层用 `ContentDraw`／`StreamUploads`／`BlockStorage`／`BlockDescriptors` 录一个三角形，**动态状态与视口都取自 `CompiledPass`** |

| 规则 | 结论 |
| --- | --- |
| **内容只需进对 pass** | 执行器不看内容内部（里面有哪些 draw、绑了什么，是内容层的事），只保证它在计划的那个 pass 里、在 plan 的 clear 之后 |
| **状态来自计划，而不是内容层重新决定** | 用例把 `pass.draws[0].commands[0].dynamic` 与 `pass.viewport` 直接交给 `ContentDraw`——这就是 M3d-2 把 normalize 做在编译段的意义：内容层不再需要知道"这个 pass 的深度策略是什么" |
| **内容静默丢掉是不行的** | 给一个计划里不存在的 pass 的内容 ⇒ `Warning`/`ContentSkipped` + `skipped()` 计数。那一 pass 缺席是有原因的（环被跳过、target 不可服务），而原因已经在别处报过；这里只报“你录的东西没地方放” |
| **变异反证** | 把 `graph->addChild(packet.content)` 拿掉 ⇒ 像素用例红，而且失败消息直接给出真相：中心读到 `(64, 128, 191)` = **计划那个 clear 色**——即“背景是计划的清屏，三角形没进去” |
| 证据 | `test_vsg` 全量 **489 用例 / 76 套件全绿**（+1，真设备）；插件目标编过；hygiene 0 / 784 文件 |

| 本片**没有**做（下一步的前提） | 内容层今天在测试里手录几何。生产侧需要三张事实表（程序身份 → GLSL、几何身份 + revision → 通道流/索引、材质身份 → 块字节），它们现在只存在于旧实现的 `SceneBridge`／`VsgMaterialManager` 里 |

### 11.16h M3d-3b-2（2026-09-21）：内容层的三张事实表（`api/ContentFacts`，无设备）

计划只能按**身份**命名程序/几何/材质（每帧每个命令只复制得起这个，也只应该复制这个）；
“这个身份是哪些顶点流”“这个材质是哪些字节”是**内容**，不是帧意图，所以由拥有它们的层从自己的表里回答。
这一片把这三张表与它们的**查找语义**钉住（数据源还在旧实现的 `SceneBridge`／`VsgMaterialManager` 里，那是下一片）。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentFacts.hpp` / `src/api/ContentFacts.cpp` | `ChannelFacts` / `GeometryFacts` / `ProgramFacts` / `MaterialFacts` + `ContentFacts`（三张表，借用一帧）+ 三个查找 + 两条规则函数 |
| `tests/test_vsg/ContentFactsTest.cpp` | 5 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **身份 + revision 是键** | 三个查找都按这两个值回答。revision 不同**不是**“旧的也能用”：计划描述的是内容已经往前走之后的画面，拿新字节去录就是一张与帧无关的图 |
| **miss 有三种，都是“不能录”但不是同一件事** | `Unknown`（这张表不认识这个身份）/ `Revision`（身份在、修订不同）/ `Malformed`（条目在但不能画）。三者决定相同（跳过 + 报），但消息与排查路径不同——这正是“一次报清”的前提 |
| **几何的 Malformed 是“通道与布局对不上”** | `popcount(canonical_mask) + custom_locations.size() == channels.size()`，且索引流必须是 `Index`。规则放在**查找里**，内容层就绕不过去：声明了没人喂的属性是驱动拒编或未绑定内存读，多出来的流是数据没人消费 |
| **材质的 Malformed 是“块不是 ABI 的大小”** | `sizeof(VineMaterialBlock)`。字节数错了会让着色器读到别的块（与 M2c-2b-1 “对齐即拒绝”同一条纪律） |
| **空表答 Unknown，不猜** | 表里没有就是没有：一个“缺省材质”或“缺省程序”的静默代替品会让内容错得无从归因（与旧实现“没有有效 shader 就不画”同一条口径） |

| 变异反证 | 把三个 revision 检查改成“照样回答”⇒ 三条 revision 用例变红、其余 2 条绿 |
| --- | --- |
| 证据 | `test_vsg` 全量 **494 用例 / 77 套件全绿**（+5）；插件目标编过；hygiene 0 / 787 文件 |

### 11.16i M3d-3b-3（2026-09-21）：每绘制 ABI 块的打包（`api/DrawBlock`，无设备）

内容层把计划变成 GPU 对象时，第一处“算术”就是每绘制的块（`VineDrawBlock`）。三件事会静默错：矩阵的列主序、
平移在平铺数组的 12..14、不透明度在 `params.x`。所以打包只允许有一处，并用一条**非对称矩阵**的用例钉住它——
对称矩阵（单位阵、缩放、大多数测试内容）的转置打包与正确打包逐字节相同，只会在以后表现为“模型转错方向”。

| 文件 | 是什么 |
| --- | --- |
| `api/DrawBlock.hpp` / `src/api/DrawBlock.cpp` | `packDrawBlock(const CompiledCommand&, VineDrawBlock&)`：从访问器逐元素写（不抄内存），把列主序写成**函数自己的注释与用例**；`params = {opacity, 0, 0, 0}` |
| `tests/test_vsg/DrawBlockTest.cpp` | 2 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **列主序写成 `column * 4 + row`** | 元素 (row, column) 落到平铺数组的哪个位置，是这份 ABI 与着色器之间的契约；从访问器逐元素写而不是复制存储，让“用的是哪种约定”在读过的地方直接可见 |
| **平移在 12..14** | 同一个事实从字节侧再看一遍（第四列），也是“没有转置”的第二个证据 |
| **`params.yzw` 是保留槽，打包为零** | “看起来应该有地方放”的值就是每绘制参数悄悄失效的入口 |
| **值真的从计划来** | 第二个用例跑完整条路（recorder → compiler → packer），从平铺数组里读出平移：这是“屏幕上的数来自宿主那个模型矩阵”的声明 |
| **视图块暂时不做** | `VineViewBlock` 的 ABI 已钉（`view` / `inv_view` / `proj` / `view_proj` / `cam_pos` / `frame`），但 `frame`（时间 / 视口尺寸）与 `cam_pos.w` 需要**会话侧**的约定，而那里才是这两个值得出处；在此发明就是给同一个问题第二个答案 |

| 变异反证 | 把索引改成行主序 ⇒ **两条用例都红**（包括专门为它准备的非对称矩阵那条） |
| --- | --- |
| 证据 | `test_vsg` 全量 **496 用例 / 78 套件全绿**（+2）；插件目标编过；hygiene 0 / 790 文件 |

### 11.16j M3d-3b-4（2026-09-21）：几何的通道走查（`api/GeometryFacts`，无设备）

三张表的第一张真实数据源：把 SDK 的 `Geometry`（宿主 authored 的对象）变成 `GeometryFacts`。规则都是
“错了也看不出来”的那种，所以逐条钉：

| 文件 | 是什么 |
| --- | --- |
| `api/GeometryFacts.hpp` / `src/api/GeometryFacts.cpp` | `buildGeometryFacts(geometry, out, storage)`：按固定顺序走 `bufferLocations()`，逐通道建 `StreamKey` + `vsg::floatArray`，再建索引流 |
| `tests/test_vsg/GeometryFactsTest.cpp` | 5 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **通道顺序就是顶点绑定顺序** | canonical 角色（位置 0 / 法线 1 / 颜色 2 / 保留 UV 8）在前，自定义 location 升序在后。管线属性声明从同一份 entry 建，所以“哪个缓冲喂哪个属性”不可能两边不一致 |
| **索引流归一化为整个 buffer** | 几何声明的是一段**切片**，但流的身份是（buffer, revision）——切片随 draw 走（`index_count` / `first_index`）。这就是同一 index arena 上的两个几何只上传一次的原因；按切片建键会把同一段字节按几何数上传 |
| **切片通道上传自己的段** | 通道读共享缓冲的一段，上传的就是那一段；索引相对几何自己的顶点（SDK 自己这么说）⇒ `vertex_offset = 0` |
| **同一起点的规则** | 一个几何的所有通道必须从**同一个顶点**起算（`offset / components` 一致），否则“索引 0”对每个通道指的是不同顶点，索引就没有单一含义 ⇒ Malformed |
| **不能画就是不能画** | 没有任何顶点缓冲 ⇒ `Unknown`；没有位置（唯一必需的属性）或没有索引（本后端的绘制是 indexed）⇒ `Malformed`。**自定义通道（≥ 3 且 ≠ 8）不被拒绝**：照原样描述，能力上限由上传层说出——在这里丢掉它会让宿主 authored 的几何看起来像另一个它没写过的几何 |

| 落地抓到的 | 内容 |
| --- | --- |
| 用例当场抓到真 bug | 建好的 `storage` **没有发布成 `out.channels`**（entry 的 span 全空）⇒ 三条用例当场红。这正是“表是借用的、发布是显式一步”的价值 |
| 变异反证 | ① 通道顺序改回“几何自己报的顺序” ⇒ 顺序用例红；② 索引键改成按切片（`count = indexCount()`）⇒ 两条分享/索引用例红。其余用例保持绿 |
| 证据 | `test_vsg` 全量 **501 用例 / 79 套件全绿**（+5）；插件目标编过；hygiene 0 / 793 文件 |

### 11.16k M3d-3b-5（2026-09-21）：程序与材质两张数据源（`api/ContentSources`，无设备）

| 文件 | 是什么 |
| --- | --- |
| `api/ContentSources.hpp` / `src/api/ContentSources.cpp` | `buildProgramFacts`（`ShaderProgram` → 两段 GLSL + 入口点）与 `buildMaterialFacts`（`Material` → ABI 块字节；`nullptr` → 默认材质） |
| `api/ContentFacts` | `findMaterial` **不再特判 nullptr**：默认材质是一张表里的普通条目，而不是查找失败——决定“拒绝还是回退”留在内容层 |
| `tests/test_vsg/ContentSourcesTest.cpp` | 4 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **一个内容管线 = 恰好两段图形阶段 + 一个入口点** | 多余的同种阶段 / compute 阶段 / 空源 / 两段入口点不一致 ⇒ `Malformed`。“取每种的第一个”会编出宿主没写过的程序，而画面上什么也不会说 |
| **没有材质不等于查找失败** | SDK 的材质是可选的，而 ABI 自己的默认值**就是**默认材质；所以 `nullptr` 是一条**身份为空的普通条目**。表里有它就答得上，没有就答 `Unknown`——是否回退由内容层定，不由查找定 |
| **材质的修订由调用方给** | `Material` 没有 revision 访问器（M3d-1 已记）；知道材质何时被改的是材质管理器，所以修订作为参数传入（与几何同一条规则：修订是计划要报的事实，只有它的拥有者能报） |
| **块的成员才是载荷** | `VineMaterialBlock` 是聚合体，`{}` 只初始化**成员**（这是 ABI 的默认材质：灰、只有一个非零 shininess）而**不碰尾部填充**（shininess 在 48..51，块 64 字节）。填充不被任何着色器读；比较要用 ABI 自己的逐成员 `operator==`，字节级比较不是有意义的运算（它会把“没改过”报成“改过了”） |

| 落地抓到的 | 内容 |
| --- | --- |
| 一个值得记住的字节级事实 | 先写了“`{}` 会把对象（含填充）清零”的注释，又被诊断打脸：诊断打印出**差异字节正好是 52..63**（= `shininess` 之后的尾部填充），而逐成员 `operator==` 为真。注释与用例都已改成陈述真正的事实（这一条也已记进仓库记忆） |
| 变异反证 | ① 接受任意阶段组合 + 入口点不一致 ⇒ 程序规则用例红；② `findMaterial` 恢复 nullptr 特判 ⇒ 默认材质用例红；其余 7 绿 |
| 证据 | `test_vsg` 全量 **505 用例 / 80 套件全绿**（+4）；插件目标编过；hygiene 0 / 796 文件 |

### 11.16l M3d-3b-6（2026-09-21）：把三张表接进内容层（`api/ContentPass`，真设备）

内容层就是“身份变成字节”的那一层：几何的通道变成流、程序的阶段变成管线、材质的块字节变成描述符的载荷。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentPass.hpp` / `src/api/ContentPass.cpp` | `Scope`（管线/录制器/注册表/块存储/描述符集/上传）+ `record(pass, facts, compatibility, view_block, out)`：逐命令 `findProgram/findGeometry/findMaterial` → 写块（`packDrawBlock` + 材质的 ABI 块）→ `acquireVertex/acquireIndex` → `ContentDraw::Draw` → 挂到组上 |
| `api/ContentFacts` | **纠正**：`findMaterial` 只按身份查（签名去掉 revision 参数）——计划根本报不出材质的修订（SDK 的 `Material` 没有访问器），所以“表里的条目”就是它此刻的字节；条目仍留 revision，因为块存储按它管理 in-flight 副本 |
| `tests/test_vsg/ContentPassTest.cpp` | 1 个真设备用例（含“第二帧复用”与“拒绝”两半） |

| 规则 | 结论 |
| --- | --- |
| **逐命令拒绝，不是逐 pass** | 程序/几何/材质任一查不到 ⇒ 该命令不画、按查找到的理由上报（未知 / 修订不同 / 畸形），**其余照画**。一个没法着色的 drawable 是画面上的一个洞，不是丢掉整帧的理由 |
| **一个 scope 一种顶点布局（已声明的限制，§11.16m 已取代）** | 本层驱动一套 `ContentPipeline` + `ContentDraw`，而管线是按顶点布局编译的；几何布局不同 ⇒ 拒绝并说出原因。“静默按错布局画”比拒绝更糟；多布局 scope（一布局一管线、共享变体池）是下一片 |
| **输入与全屏绘制调用未接** | 计划还没携带 pass 的输入（所以键里的 `sampled_color_count` 为 0），全屏绘制属于 program-slot 路径 ⇒ 两者都**拒绝**而不是近似 |
| **缓存的证据是计数，不是“画出来了”** | 同一内容第二帧：`uploads()` 不涨而 `aliases()` +2（顶点 + 索引两条流）、`materialWrites()` 不涨而 `materialHits()` +1——这正是“身份带修订而不是带字节”的价值 |

| 落地抓到的 | 内容 |
| --- | --- |
| 测试自己踩的坑（已修） | 用栈对象构造 `intrusive_ptr<Geometry>(&geometry)` ⇒ 析构时 `free(): invalid pointer`。SDK 对象必须堆上由 `intrusive_ptr` 拥有（与计划里的身份一致） |
| 变异反证 | ① 每次 acquire 用**移动的身份**（revision + 计数器）⇒ 缓存断言红（`uploads` 3≠2、`aliases` 1≠2）；② 忽略几何 miss ⇒ 进程崩在空指针（那道检查正是它在防的事）。头两次尝试的变异都是无效变异（`+bound` 恒为 0；`+1` 两帧一致），**变异本身也要验证**——这一条已记进仓库记忆 |
| 证据 | `test_vsg` 全量 **506 用例 / 81 套件全绿**（+1，真设备）；插件目标编过；hygiene 0 / 799 文件 |

### 11.16m M3d-3b-7（2026-09-21）：多布局 scope（`api/ContentPass`，真设备）

一片管线层 = 一个程序的阶段 × 一种顶点布局，所以一个 pass 要画两种布局的网格就需要两片。scope 现在是一**组**“编译好的半片”，逐命令按 (program, revision, layout) 选；所有半片共享池与 pass 的注册表。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentPass` | `Scope::Entry`（program / revision / layout / pipelines / draws）+ `Scope::entries`（span）；ctor 不再收布局；逐命令选半片，选不中时按“哪一项没对上”给两条不同消息 |
| `tests/test_vsg/ContentPassTest.cpp` | 新增真设备用例（两半片 + 两次拒绝），并把既有用例迁到新 scope 形状 |

| 规则 | 结论 |
| --- | --- |
| **半片的身份 = (程序, 修订, 布局)** | 管线层按一份阶段文本 + 一种布局建；借别人的阶段画就是画出没人写过的画面，而池会把那份对象记在计划点名的键上（静默）⇒ 三者都得对上 |
| **选不中要说清是哪一项** | 两种 miss 的修法不同（去编译那个程序 / 去编译那种布局）⇒ 消息各说自己那一项；用例用 sink 收消息逐字钉住 |
| **池与注册表共享是结构性的** | `Scope` 只有一个 registry 指针 ⇒ “半片一个注册表”这种错误安排**表达不出来**；共享的语义价值 = 换过半片后回到前一片要**重发管线绑定**，证据 = 左半片 `pipeline_binds()==2`（三次绘制：左、右、左） |

| 落地抓到的 | 内容 |
| --- | --- |
| 变异反证 | ① 选半片忽略布局 ⇒ 未服务的布局被画（`record` 真、零消息）红；② 忽略程序身份 ⇒ 未编译的程序被画，红；③ 把注册表改成“每个录制器一份”（临时给 `ContentDraw` 加成员）⇒ 第三次绘制跳过绑定（1≠2）**且左网格被蓝管线圈画成 (0,0,255)** —— 计数与像素同时抓到 |
| 测试自己踩的坑 | 第二帧的 `beginFrame` 被协议拒绝：**关帧的是 `swapBuffers()`**（`endFrame()` 只离开 pass，回 `Idle` 靠 swap）⇒ 症状是“第二帧的计划还是第一帧的三条命令”，靠打印 token 抓到 |
| 证据 | `test_vsg` 全量 **507 用例 / 81 套件全绿**（+1）；插件目标编过；hygiene 0 / 799 文件 |

### 11.16n M3d-3b-8（2026-09-21）：pass 输入的采样绑定（计划携带输入表 + 内容层绑定，真设备）

§11.16l 留下的第一个缺口：`setPassInputs` 声明的输入到不了着色——计划不携带它们，键里的
`sampled_color_count` 恒为 0，描述符里也就没有采样的纹理。这一片把那条线接起来，分成三件事：

1. **计划携带输入**（`core/FrameCompiler`）：`CompiledPass::inputs` = `CompiledInput{ target, color_attachments }`
   的有序表，**从与 pass 目标同一份 target 事实**里解析（同一个事实只答一次：调用方重新从 target 对象推一遍
   就可能与计划给出两个答案）；空输入（本帧无人产出）留在表里、报 0；**非空但事实答不上来的输入上报一次**
   （此时什么也绑不了，静默按"没有这个输入"着色更糟）——注意 pass 本身照跑：目标查不到是"没地方画"，
   输入查不到只是"这一项绑不上"。
2. **采样集的布局进管线身份**（`api/ContentPipeline`）：键里的 `sampled_color_count` 是**身份**而不是运行时
   状态（管线是按一份描述符集布局编译的），所以布局层按计数建 **set 1**（每项一个 combined image sampler，
   顶点/片元两阶段都可读）**并按计数缓存**；`layoutFor(count)` 给出与之一体的管线布局，`sampledSetLayout(count)`
   给内容层用来建**同一个**布局对象的 set；`inputSampler()` 一层一个采样器（默认线性）。
3. **内容层绑定**（`api/ContentPass`）：调用方按计划顺序逐个输入交出图像（`InputImages`，
   `OffscreenTarget::colorView(i)` 就是答案），内容层核对**每一项的彩色数**与计划一致后才建 set；
   不一致 ⇒ **整趟 pass 拒绝**并说出是哪一项动了（输入是 pass 的**一个**事实，不存在"逐命令近似"）；
   计数进键（`sampled_color_count`），set 进每一次绘制的状态组。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameCompiler` | `CompiledInput` + `CompiledPass::inputs`（声明序、解析出彩色数）+ 答不上来的输入上报；文件头把"输入是什么"列进它决定的事 |
| `core/StateRegistry` | 第三个"仅当"：采样集按**集合身份**去重（`resolve(key, state, inputs)` + `inputs_issued/skipped`），因为输入是 pass 的属性 ⇒ 一趟 pass 一条绑定命令，不是一绘制一条 |
| `api/ContentPipeline` | 按 `sampled_color_count` 现建并保留的 set 1 布局 + 配套管线布局（`layoutFor/sampledSetLayout/inputSampler`）；`acquire()` 用键的计数选布局 |
| `api/ContentDraw` | `Draw::inputs`（采样集绑定）+ `input_binds()` 计数；绑定排在块集之后（set 0 → set 1） |
| `api/ContentPass` | `InputImages` + 计划/资源世界核对 + `makeInputSet`（一次 pass 一个 set）+ 键的计数 |
| `api/OffscreenTarget` | 彩色附件改为**按可采样收尾**（`SAMPLED` usage + render pass 的 `finalLayout = SHADER_READ_ONLY`），`capture()` 自带一对 layout 转移，新增 `colorView(i)` |
| `tests/test_vsg/SampledInputTest.cpp` | 1 个真设备用例（像素 + 拒绝两半）；`FrameCompilerTest` +2、`VariantCoreTest` +1、`ContentPipelineTest` +1 全无设备 |

| 规则 | 结论 |
| --- | --- |
| **输入的粒度是 pass，不是绘制** | "pass 声明的输入"在契约里就是 pass 的属性（`setPassInputs` 每次绘制前重申同一份解析结果）⇒ 集合只建一次、只绑一次（`input_binds()==1` 对两个绘制调用），身份与绑定不可能各自漂移 |
| **计数是身份不是状态** | 管线编译绑定在一份描述符集布局上，所以"这趟 pass 绑几个采样纹理"必须进键；两个计数 ⇒ 两个布局 ⇒ 两个管线对象（同键再来仍是 `Reused`）。这也是键审计表里"采样附件数"那一项真正落地的位置 |
| **计划与资源世界必须对得上** | 计划说几项、每项几张，调用方给的图必须逐项相等；不等就**整趟拒绝**（与 M3d-3b-0 执行器核对 target 形状同一条纪律）。拒绝消息分两条：项数不对 / 第 i 项彩色数不对 |
| **彩色附件按可采样收尾** | 输入的图像必须真的处在 `SHADER_READ_ONLY` 且建了 `SAMPLED` usage，否则描述符在撒谎。旧实现是"彩色附件永远按 SHADER_READ_ONLY 收尾"；重写版原先为了免 barrier 读回改成 TRANSFER_SRC，这一片把彩色改回**采样收尾**（深度仍留在附件布局：它是下一趟 pass 的附件、也是阴影的纹理，另有机制），**代价是读回的拷贝自带一对转移**（拷贝仍是一条命令，只是前后各一个 image barrier） |
| **"没人产出"与"后端不认识"是两件事** | 空白输入（引擎解析成 null）留在表里、报 0 张、**不上报**——那一层的问题归引擎（它自己会报）；非空但 target 事实答不上来才由编译器报一次（后端自己的账缺了一页）。两种情形的共同答案都是"这一项绑不了东西"，所以内容层按 0 张核对，照画其余 |

### 11.16o M3d-3c（2026-09-21）：会话侧（窗口目标、一次 present、视图块，真设备）

执行段的第三片：把"窗口"变成执行器认识的目标，把"视图块"的四处约定钉在一处，把会话的帧时钟与
帧图接缝接上。三件事：

1. **窗口是目标，不是特例**（`api/WindowTarget`）：`prepare(clear)` 刷新 renderArea（跟随窗口**活的**
   尺寸）并按计划写清屏值，`graph()` 给出唯一那张 `RenderGraph`，`shape()/facts()` 用与离屏目标同一套
   词回答"你是几张彩色、深度是什么"。`facts()` 的 `target` 是 **nullptr** —— 这就是默认帧缓冲的身份，
   执行器用它把 pass 分派到窗口路径。
2. **一张图、一次清、一个稳定视图**（`api/WindowTarget` + `api/VsgExecutor::recordWindow`）：
   窗口的 framebuffer 是**记录时**按 `window->imageIndex()` 解的，而窗口的 render pass 的 load op 是
   vsg 的（平台定），所以**一帧里只能清一次**：计划顺序里第一个窗口 pass 拥有这次清（它自己的
   `clear` 策略），后面的窗口 pass 叠在它上面（`window_recorded_` 正是这条规则的开关）。
   窗口的内容挂在**一个** `vsg::View` 下，且这视图跨帧稳定：vsg 的 `GraphicsPipeline` 是**按 viewID**
   编译的（`GraphicsPipeline.cpp` 的 `_implementation[viewID]`），没有视图时所有 pass 共用 viewID 0，
   窗口的 pass 就可能复用到"按别人 render pass 编译"的管线；逐帧换视图则会让编译出的管线**泄漏**
   （`View::~View` 只放号，不回收号上的实现）。
3. **视图块**（`api/ViewBlock`）：`buildViewBlock(camera, time, width, height)` 把计划里的相机快照、
   会话的帧时钟与**目标**（不是 pass 子矩形）的尺寸装配成 ABI 块；`api/Session` 新增 `frameSeconds()`
   （会话自己开机的时刻为零点，**一帧内不变**），并给内容层两条帧图接缝（`makeFrameGraph` /
   `assignFrameGraphs`）。

| 文件 | 是什么 |
| --- | --- |
| `api/WindowTarget`（新） | 窗口作为目标：shape/facts/`prepare`/`addContent`/`graph` + `colorAttachmentCount/width/height/depthSampleable`；文件头写明"一张图、一次清、一个稳定视图"三条理由 |
| `api/ViewBlock`（新） | 视图块唯一装配处：列主序、`view_proj = proj * view`、`inv_view`、`cam_pos.w`/`frame.w` 保留为 0、**裁剪矩阵折进设备约定**、`frame = {时间, 目标宽, 目标高, 0}`、无相机 = 零矩阵 + 帧事实仍在 |
| `api/VsgExecutor` | `setWindow(WindowTarget*)` + `recordWindow`（计划/world 形状核对、首次 `prepare` + 挂图、逐包挂内容；不匹配就整趟跳过并上报）；离屏路径移进 `recordOffscreen` |
| `api/Session` | `Impl` 里窗口目标取代临时 render graph；帧时钟（`started_at`/`frame_seconds`）在 `beginFrame()` 采样；`frameSeconds()`；`makeFrameGraph`/`assignFrameGraphs`（后者换掉 viewer 的录/提交任务并紧接着编译） |
| `tests/test_vsg/ViewBlockTest.cpp`（新） | 4 个无设备用例（列主序 + 组合、**裁剪折叠**的算术、帧事实与保留位、无相机） |
| `tests/test_vsg/SessionContentTest.cpp` | +1 个真设备用例：计划驱动的整帧进窗口（清屏像素、视图块进片元着色、一次 present、timeline 推进、零设备等待） |

| 规则 | 结论 |
| --- | --- |
| **视图块的裁剪矩阵是设备的，不是 SDK 的** | SDK 的矩阵是 x 右、y 上、z∈[-1,1] 近端 -1；本后端的设备是 reverse-Z + y 向下 NDC（近→1、远→0、世界上=屏幕上）。**宿主程序写 `gl_Position = view_proj * model * pos` 时不该知道任何一条**，所以折叠（x/w 不变、y 取反、z 记作 `0.5 - 0.5*z`）发生在块装配处。`view`/`inv_view` 是视图空间矩阵，**不折**（视图空间的关照计算与裁剪约定无关） |
| **不折的失败形态（实测）** | 没折的矩阵把测试三角形放在裁剪 z = **-0.714**；窗口深度清 0.0、比较是 `GREATER` ⇒ 每个片元都被拒绝。画面只剩清屏色，而**绘制仍被记录、计划仍正确、0 VUID、0 诊断**——这正是 `SceneBridgePipeline` 文件头为手写 `gl_Position` 记下的陷阱，只是上了一层（矩阵而不是程序） |
| **镜像的深度映射画面看不出来** | 变异 M2（z 折反：近→0、远→1）让三角形落在深度 0.143，仍 `GREATER` 过测 ⇒ **像素断言全绿**，只有算术用例红。这就是"算术用例与像素用例各管一段"的实证：画面负责"这条约定能用"，算术负责"这条约定是对的" |
| **时间与尺寸的出处** | 时间来自 `Session::frameSeconds()`（会话开机为零点、一帧内所有 pass 同值）；尺寸是**目标**的（着色重建屏幕空间数据时用的那张图），不是 pass 的子矩形——子矩形是 viewport 命令的事 |
| **一帧一次 present** | `commitFrame()` 里 `recordAndSubmit()` → `present()` → 帧计数 +1、`timeline.submitted(token)`、`retirement.advance(timeline)` 的顺序就是"帧的账"。像素用例在**第一帧**之后立刻断言 `framesPresented()==1`、`submittedFrame()==1`、`deviceWaits()==0`，再空转两帧只为**让呈现落地**（显示服务器的拷贝是异步的，第一帧刚 present 就读到的可能还是窗口的旧后备存储——另一个用例一直呈现三帧就是这个原因） |
| **变异反证（五条已验，一条登记）** | M1 折掉 ⇒ 3 用例红（2 算术 + 像素的 4 条断言）；M2 z 折反 ⇒ 2 算术红、像素**绿**（见上）；M3 y 不折 ⇒ 2 算术 + 朝向像素红；M4 `view` 也折 ⇒ 2 算术红（像素读不到 `view`）；M6 不调 `prepare` ⇒ 清屏像素红且实测露出 vsg 的默认清屏色 **(102,51,51)**（"画面里的清屏色确实是计划的"由此有了反证）。M1–M4 的容差特意收紧到 ±6：0.25 的 sRGB 像（137）与 0.5 的线性像（128）只差 9，容差一松，**没画出来的像素**就能替着色答"cam_pos 到了"。M5（`prepare` 不刷 renderArea）**在本片的夹具里不可观测**：窗口尺寸没变过，创建时的矩形与活的尺寸相同——它的可观测形态要一个**活改尺寸**的窗口用例（登记到下面的口子） |

**本片留下的口子（登记，不假装解决）**：

* **多次窗口 pass 的"一次清"没有像素证据**（**已关，§11.16r**：M4c 的夹具里第④趟窗口 pass 公告了另一种清屏色，
  背景仍是第③趟的颜色、公告的那个色**不出现**）；本片用例只有一趟窗口 pass，`window_recorded_` 那条规则
  要等 M4（全屏/screen pass 会往同一张图上叠第二趟）才有可观测的形态；
* **稳定视图的反证（与离屏 pass 共用管线键）也没落地**：要看到"窗口管线被按别人 render pass 编译"的
  后果，得让同一帧里有一趟离屏 pass 与窗口 pass **同键**（同程序/几何/兼容性）；这是 M4 的用例形态
  （共享键 + 采样输入正是全屏 pass 的日常）。**已关，且结论是修正**（§11.16r）：同键的后果实测到了
  （`VUID-vkCmdDrawIndexed-renderPass-02684`），但稳定视图**不是**解法——vsg 的复用循环只比 pipeline states，
  两个视图的状态集合相同时照样共用一份实现；解法是键带上**设备格式**（两个变体）；
* **逐帧新建 `CommandGraph`/内容节点**：`makeFrameGraph` 现在每帧可以给一张新图，内容节点也每帧重建
  （保留 + 停放的优化留给后面）；
* **采样输入的描述符集逐帧重建**（M3d-3b-8 的既有形态）依旧成立；
* **第二个渲染通道家族出现时可能需要逐 pass 视图**（现在是"一个视图管整个窗口"）——**已实测给出答案**
  （§11.16r）：家族之分靠**键**（设备格式），不靠视图；视图只在需要**逐视图状态覆盖**时才有用，本后端不覆盖
  任何状态，所以"一个视图管整个窗口"留着；
* **活改尺寸的窗口用例**：`prepare` 里"renderArea 跟随活的尺寸"那行现在只有代码与理由，没有可观测证据（M5 不可观测的原因）；它需要把宿主窗口在帧间改尺寸、再读新区域的像素。

### 11.16p M4a（2026-09-21）：全屏绘制调用（身份 + 层 + 事实 + 录制，真设备像素）

**为什么先做这一片。** 引擎只有两种绘制调用：内容绘制（`render()`）与全屏绘制（`drawScreenProgram()`）。
M3d-3b-8 把采样线（"源附件 i → binding i"）建在内容路径上，而**全屏路径是这条线的另一个消费者**：它的
源就是它的采样集。但两者**不是同一套描述符 ABI**：内容层是"块在 set 0、采样输入在 set 1"，而引擎自己
的全屏程序（`BuiltinShaders::screenCopyProgram` / `deferredLightProgram`，随 SDK 一起发布的 GLSL）声明的是
`layout(binding = i) uniform sampler2D`（**没有 set 限定词 ⇒ set 0**）与 `layout(push_constant)`。§1.3 的
"被迫同形"清单早就写了这一条（"128B push block 承载全屏路径的光照+视图"），所以这一片**照做**：

| | 内容 ABI | 全屏 ABI |
| --- | --- | --- |
| set 0 | 四个块（view/draw/material，动态偏移） | 源彩色附件（binding i = 附件 i） |
| set 1 | 采样输入（同一条采样线） | 无 |
| 顶点 | 几何流 + 索引流 | 无：`gl_VertexIndex` 生成的三角形 |
| push | 128B（顶点阶段） | 128B（**片元**阶段：引擎的全屏顶点阶段不声明常量） |

落地四件事：

1. **身份**（`core/Keys`）：`DrawKind` 从计划搬到键的旁边（一处拼写），`PipelineKey.kind` 进
   `operator==`/哈希/**键审计表**——两套 ABI 编译出的管线互不可绑，键不携带它就可能把一份错 ABI 的管线
   发给绘制。
2. **层**（`api/ContentPipeline`）：`createScreen` 建一个全屏层（无块集、无顶点流、set 0 按采样数现建、
   push 是片元阶段的 128B）；`kind()` 自报；`acquire` **拒绝另一种 kind 的键**（计数 `failures`，不进池）；
   烘焙状态用旧实现 overlay 的形状（cull NONE、深度测试/写关闭）——动态命令仍然赢。
3. **事实**（`api/ContentSources`）：`buildScreenProgramFacts` 把**引擎的正典全屏顶点阶段**与宿主的片元
   阶段组成一对（顶点阶段是**读**SDK 的文本，不是抄一份）；宿主自带的顶点阶段**按契约忽略**；无片元阶段
   / 多片元 / compute / 空源 / 入口点不是正典的那个 ⇒ `Malformed`（"程序什么都没写" 才是 `Unknown`）。
4. **录制**（`api/ContentDraw`）：`recordScreen` = 管线 + set 0 采样集 + 动态块 + viewport/scissor +
   `Draw(3, 1, 0, 0)`；计数 `screen_draws()` 与 `input_binds()`（一趟 pass 一次绑定，与内容路径同一条
   "仅当"）。

| 文件 | 是什么 |
| --- | --- |
| `core/Keys`（+.cpp） | `DrawKind` 的家 + `PipelineKey.kind`（相等/哈希/审计表一行） |
| `core/FrameRecorder` | 改用 `Keys.hpp` 的 `DrawKind`（一处拼写） |
| `api/ContentPipeline` | `createScreen` + 按 kind 的 set/布局（set 0 采样集、push 片元 128B）+ `kind()` + 异 kind 拒绝 |
| `api/ContentSources` | `buildScreenProgramFacts`（引擎顶点阶段 + 宿主片元阶段；契约说宿主顶点阶段被忽略） |
| `api/ContentDraw` | `ScreenDraw` + `recordScreen` + `screen_draws()`；`vsg::Draw`（非索引） |
| `tests/test_vsg/ScreenDrawTest.cpp`（新） | 2 个真设备用例：**随 SDK 发布的** `screenCopyProgram(0)` 的拷贝只落在 PiP 矩形内、`screenCopyProgram(1)` 读的是附件 1（由内容 MRT pass 画的绿色）而不是附件 0 |
| `tests/test_vsg/ContentSourcesTest.cpp` | +2（全屏事实的组成与被忽略的宿主顶点阶段；六种拒绝） |
| `tests/test_vsg/ContentPipelineTest.cpp` | +2（set 0 采样集/无块集/异 kind 拒绝；旧 overlay 的烘焙状态） |
| `tests/test_vsg/ContentDrawTest.cpp` | +1（`Draw(3)` + set 0 + 空状态组的第二趟） |

| 规则 | 结论 |
| --- | --- |
| **两套 ABI 是身份，不是风格** | 同一个程序身份 + 不同 kind = 两个变体（同键仍是 `Reused`）。这条不是洁癖：把"全屏层编译的管线"发给内容绘制（或反过来）是**描述符集布局层面的不匹配**，驱动不报、画面也未必看得出 |
| **采样线只用一次** | 全屏路径没有第二套采样机制：`sampledSetLayout`/`inputSampler` 原样复用，差别只有**集合的索引**（kind 决定是 0 还是 1）。M3d-3b-8 的"计划携带输入表"因此一行没改就服务了第二种调用 |
| **全屏三角形是引擎的** | 顶点阶段从 `BuiltinShaders::fullscreenVertexProgram()` **读**出来（与旧实现的工厂同一个做法）；宿主带了顶点阶段也忽略——契约原文如此，而"尊重"它的后果是编译出一个引擎片元阶段没写过的三角形 |
| **push 块的**内容**还没落地** | 布局声明了 128B（SDK 的 `deferredLightProgram` 就是这么声明的），但光照半边（world→view 的三盏方向光 + 环境光）与深度重建半边（near/far）分别是光照相位与深度采样相位的活。**这一片的程序（拷贝）不读它**；读它的内置程序在这之前看到零值——登记，不假装 |
| **门禁抓到的是夹具不是产品** | 设备用例第一次跑出 **12 条 VUID**（`vkCmdSetPolygonModeEXT`/`vkCmdSetColorBlendEnableEXT`/`vkCmdSetColorBlendEquationEXT` 从未调用）：管线声明了这些动态状态，而夹具建 `ContentDraw` 时没取扩展入口点 ⇒ 动态命令静默跳过它们。同一族陷阱在 M3d-3b 的夹具注记里已经写过一次（"没有它们，动态命令跳过 polygon-mode 与 blend 调用"） |
| **变异反证（四条已验）** | N1 集合索引不再跟 kind ⇒ 算术用例红 + 设备侧**段错误**（布局里带着空块集）；N3 三个顶点改成六个 ⇒ 算术红、**像素绿**（PiP 的 viewport 把多出来的三角形裁掉，而全屏拷贝的第二枚三角形画的又是同一张画）；N5 宿主顶点阶段获胜 ⇒ 事实用例红；N6 去掉 kind 检查 ⇒ 层与录制两处一起红。N3 的"像素看不见"是这一片的正当结论：**顶点数是 ABI 的声明，算术用例是它的证据** |

**本片留下的口子（登记）**：**计划侧**（`api/ContentPass` 仍在拒绝 `DrawKind::Screen`）——源 = pass 声明的
第一个输入（采样集已就位）、push 块的内容、PiP 子矩形经计划上屏、以及窗口合成（M4b）；顺带把 §11.16o 登记的
两条口子（多趟窗口 pass 的"一次清"、窗口/离屏同键变体）一起变成可观测用例。

### 11.16q M4b（2026-09-22）：全屏绘制的计划侧（计划解析它的状态，内容层按 kind 录它，真设备像素）

M4a 把全屏调用接到了内容栈；这一片把它接到**计划**上：`api/ContentPass` 不再拒绝 `DrawKind::Screen`，源就是
pass 声明的第一个输入（M3d-3b-8 那条采样线的另一个消费者），PiP 子矩形与"一趟 pass 一条绑定"照旧。三件事：

1. **计划解析全屏调用的状态**（`core/FrameCompiler`）：全屏调用**没有命令**，所以没有"逐命令的状态"可解——
   `CompiledDraw::dynamic` 是它的，取的默认值是屏幕程序不编辑的那些（无剔除、fill、三角形、引擎的混合因子），
   而**深度策略取 `Disabled`**：引擎的正典三角形正好落在 **reverse-Z 的远平面**（z = 0.0 —— 窗口深度清屏的
   那个值），任何深度测试都会把整块覆盖层拒掉。旧实现的 overlay 管线就是为此把深度烘成关闭的，SDK 也把这次
   调用描述成"opaque over it"；继承 pass 的 `TestAndWrite` 会让同一块覆盖层在窗口上消失、在无深度附件的小目
   标上却看着正常。
2. **内容层按 kind 找半片**（`api/ContentPass`）：`Scope::Entry` 多一个 `kind`；全屏半片按**计划里的程序身份 +
   修订**找（没有几何那一半：这次调用不画几何，所以不需要事实表），miss 的消息区分"没编过这个程序"与"修订
   对不上"。采样集**每 kind 一份**：内容在 set 1（块集之后），全屏在 **set 0**（它自己的 ABI），两个 kind 各
   自用自己那一层的布局对象去建 set，而"一趟 pass 一次绑定"的"仅当"逻辑是**同一个注册表**说了算。
3. **push 块**（`api/ContentPass::recordScreenDraw`）：128 字节、**片元阶段**、内容**全零**。布局是 SDK 的
   （`deferredLightProgram` 就这么声明），填它的两半（world→view 的光照、near/far）分属光照相位与深度采样
   相位。**必须真的推**：声明了 push 范围却从不推，着色读到的是**未定义**字节，"那一相位还没落地"得是确定的
   零而不是驱动恰好留下的东西。本片证据用的程序（引擎的 screen copy）一个字节都不读它。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameCompiler`（+.hpp） | `CompiledDraw::dynamic`（全屏调用的状态）+ 解析规则（默认值 + `Disabled` 深度）与理由 |
| `api/ContentPass`（+.hpp） | `Scope::Entry::kind`；全屏半片查找（程序 + 修订，两种 miss 消息）；`makeInputSet` 改为按层与集合索引参数化；`recordScreenDraw`（管线 + set 0 + 动态块 + **push** + 矩形 + `Draw(3)`） |
| `api/ContentDraw`（+.hpp） | `ScreenDraw::push`（在管线绑定之后录） |
| `tests/test_vsg/FrameCompilerTest.cpp` | +1 无设备用例：全屏调用的 kind/source/program/矩形进计划，`dynamic` 是 `Disabled` + 屏幕默认值；**同一趟 pass 的内容命令仍拿 pass 的深度** |
| `tests/test_vsg/SampledInputTest.cpp` | +1 真设备用例：计划驱动的合成——pass 1 清源、pass 2 用**随 SDK 发布的** `screenCopyProgram(0)` 在 PiP 矩形里拷贝源；矩形内是源的颜色、外是目标自己的清屏色；`input_binds() == 1` |
| `tests/test_vsg/ContentDrawTest.cpp` | 屏幕用例 +push 节点的形状（片元阶段、128 字节） |

| 规则 | 结论 |
| --- | --- |
| **全屏调用不测深度是它的定义，不是优化** | 正典三角形在 z = 0.0；窗口的深度清 0.0 且比较是 `GREATER` ⇒ 测了就整块消失。变异 P2（改成继承 pass 的策略）在**有深度附件的目标上**让"矩形内是源的颜色"直接变红（实测得到目标清屏色 (0,0,191)） |
| **集合的索引是 ABI，绑错一层就崩** | 变异 P1（全屏采样集绑到 set 1）：4 条 VUID（`vkCmdBindDescriptorSets-firstSet-00360`、`vkCmdDraw-None-08600`）+ **段错误**——全屏管线布局只有 set 0，绑到 1 的集合既越界又不被绘制看见 |
| **一趟 pass、两个 kind、一份输入** | 采样集按 kind 各建一份，但"输入是 pass 的"这条没变：注册表还是同一个，`input_binds()` 仍是每 pass 一次。两种 kind 混在一趟 pass 里在计划与本层都合法（内容命令走自己的三张表，全屏调用走半片） |
| **push 的"形"有证据、"内容"没有** | 形：设备无关用例钉住节点（片元阶段 + 128 字节）与它排在管线绑定之后；内容：**登记**为光照相位的活（本片的程序不读它）。想用"读到零值"做反证不可靠（未定义的 push 内存常常也读成零），所以不假装有反证。**已关，§11.16w**（M5d）：内容落地（`LightPushBlock` + 每次调用推），并且**正向**断言了"三带各自的颜色 + 全零变异三条带全黑" |
| **相机不再被本层使用，但契约仍然要它** | 全屏 ABI 的采样器在 set 0、块一个不绑，所以重写版的全屏路径**不读视图块**；SDK 的 `ScreenPass` 在没有相机时**根本不会问后端**（wiring 期就报），所以"没有视图就画不出来"这条不在这里重造 |

**本片留下的口子（登记）**：**窗口合成**（一趟窗口 pass 里叠一块全屏覆盖层）与 §11.16o 登记的两条口子
（多趟窗口 pass 的"一次清"、窗口/离屏同键变体）——它们要一个"离屏 pass + 两趟窗口 pass"的夹具，与 M4a/b 的
离屏夹具不同源，单独做一片（M4c）。

### 11.16r M4c（2026-09-22）：窗口合成（一趟窗口 pass 里叠全屏覆盖层），顺带把两条登记的口子做成用例

M4a/M4b 的全屏绘制只在离屏目标上验过；这一片把它放进**窗口自己的 render pass**（vsg 造的，表面格式 + 它自己
的依赖），与一趟离屏 pass 同帧。夹具 = 四趟 pass 一帧：①「picture」离屏（只清屏，红）②「shared」离屏（清屏 +
同一段场景三角形，深度格式与窗口 traits 相同 ⇒ 引擎看来与窗口同形）③窗口场景（**第一趟窗口 pass，拥有那一次
清**）④窗口覆盖层（全屏绘制：采 picture，画在 PiP 子矩形里，自己公告**绿色**清屏——必须不生效）。计数器与像素
各管一段：像素说"叠对了"，计数器说"录对了"（黑窗也能让"没画"看起来像"画对了"）。

三条结论，两条来自登记的口子，第三条是这一片**新发现**：

1. **多趟窗口 pass 的"一次清"有了像素证据**：窗口背景是第③趟的清屏色（蓝），第④趟公告的绿色**不出现**
   （`isColourByte(background[1], 0.25)` 为假），三角形仍是场景色 ⇒ `window_recorded_` 那条规则
   （一帧里第一趟窗口 pass 拥有那次清）从"只有理由"变成"有反证"。执行器还断言了它是**按计划顺序**放置四趟
   （`recorded() == {1,2,3,4}`，不是公告顺序——本夹具里两者恰好相同，所以这条是"接缝存在"的证据而不是"顺序会
   被改"的证据）。
2. **"窗口/离屏同键变体"这个口子是错的**：真正的答案不是"共享一个变体、让 vsg 按 viewID 分开编译"，而是
   **两个变体**。理由两条，都是实测：
   * 引擎的兼容性词汇表是**投影**（`RenderTarget::ColorFormat::RGBA8` 同时覆盖 `B8G8R8A8_SRGB` 与
     `R8G8B8A8_UNORM`），而这两个 render pass **不兼容**（通道序 + 色彩空间 + 子 pass 依赖数都不同）；
   * vsg 的 `GraphicsPipeline` 虽然按 viewID 编译，但**复用循环只比 pipeline states，不比 render pass**
     （`vsg 1.1.16 — src/vsg/state/GraphicsPipeline.cpp — GraphicsPipeline::compile()`）：本后端的管线把
     viewport/scissor 声明成动态状态、状态集合逐位相同 ⇒ "两个视图"照样复用同一份实现。
   实测形态：`VUID-vkCmdDrawIndexed-renderPass-02684`（`B8G8R8A8_SRGB` vs `R8G8B8A8_UNORM`、
   `dependencyCount 2 != 1`）——**画面竟然是对的**（未定义行为，不是"能跑就行"的证据）。
   修法（本片落地）：**把设备格式放进键的兼容性半边**——`core::TargetShape` 多两个字段
   （`device_color_formats` / `device_depth_format`，装载设备自己的格式码，0 = 无该附件/未知），
   `core::RenderPassCompatibility` 同样带上，`TargetShape::compatibility()` 一处装配（调用方不再手搓），
   两个目标各自把真实格式报出来（窗口从 surface，离屏从自己建镜像用的那个映射）。这是**引擎词汇表表达不了
   的事实由知道它的那一层交出来**，不是把 extent 之类的运行时量塞进身份：格式是 render pass 身份的一部分，
   Vulkan 的兼容性规则里写着。
3. **顺带纠正 §11.16o 的一条理由**：那里说"窗口内容挂在自己的稳定 `vsg::View` 下 ⇒ vsg 就按窗口的 render pass
   编译窗口的管线"。视图**没有**这个作用（见 2 的第二条：状态相同就复用实现），稳定视图本身还在（记录期
   `viewID` 的出处），但"分族"是**键**的事。视图的真正用途是"逐视图状态覆盖"的挂点，本后端不覆盖任何状态。

**顺带修掉的既有缺陷（本片夹具第一个踩到）**：**capture 的跨帧写-写**。每个目标的 `capture()` 每帧被录一次，
写的是**同一块** host-visible 目的缓冲；会话可以有好几帧在飞，两份拷贝之间没有任何依赖 ⇒ 同步验证报
`SYNC-HAZARD-WRITE-AFTER-WRITE`（4 条）。修法是 capture 命令表开头加一条**声明顺序**的缓冲屏障
（`TRANSFER → TRANSFER`、`TRANSFER_WRITE → TRANSFER_WRITE`）——两份拷贝写的是同样的字节，缺的只是"谁先谁后"
的声明。

| 文件 | 是什么 |
| --- | --- |
| `tests/test_vsg/WindowCompositionTest.cpp`（新） | 四趟 pass 一帧的窗口合成夹具：像素三条（三角形 = 自己的变体、背景 = 第一趟窗口 pass 的清、PiP = 覆盖层采到的那张图）+ 计数器五条 + 执行器顺序；窗口读回按**表面字节序**归一（见下） |
| `tests/test_vsg/TestHostWindow.hpp`（新） | 测试自己的宿主窗口（XCB）：`handle()` / `alive()` / `pixel()`——把 `SessionContentTest` / `SessionMoveTest` 里两份雷同的局部 `HostWindow` 合成一处（-116 行），本片的用例是第三个使用者 |
| `core/Keys.hpp` / `Keys.cpp` | `RenderPassCompatibility` 带 `device_color_formats` / `device_depth_format`（+ `operator==` + 哈希 + 审计表那一行的词）与理由（含实测 VUID） |
| `core/TargetPlan.hpp` / `TargetPlan.cpp` | `TargetShape` 同两个字段 + `compatibility()`（键的兼容性半边就此只有一处装配）；`operator==` 带上它们 |
| `api/WindowTarget.cpp` / `api/OffscreenTarget.cpp` | 各自把真实设备格式报进 shape（窗口 = surface format，离屏 = 自己镜像用的那个）；窗口文件头纠正"稳定视图"那条理由 |
| `api/OffscreenTarget.cpp` | capture 命令表加声明顺序的缓冲屏障（跨帧写-写） |
| `tests/test_vsg/{ContentPassTest,ExecutorTest,SampledInputTest,SessionContentTest,WindowCompositionTest}.cpp` | 五份本地 `compatibilityOf(shape)` 助手删掉，改调 `shape.compatibility()`（一处规则一处装配） |

| 规则 | 结论 |
| --- | --- |
| **"两个视图"不等于"两份编译"** | vsg 的复用循环看的是状态集合；两个视图的状态集合相同时它**故意**复用（对同一 render pass 的 load-op 变体来说这正是想要的：`LoadOpVariantKey` 不进键就是为了让它们共享一份编译）。所以"哪些东西算兼容"必须由**键**回答完整，视图救不了键 |
| **引擎的格式枚举是投影，不是身份** | `RGBA8` 不能作为 render pass 身份的替身：sRGB/线性、BGR/RGB 都是兼容性的一部分。设备格式因此**必须**从设备层交上来（`core/` 不认识这些码，只比较它们）；"没报"与"报了别的"是两个不同的兼容性，未知**不会**静默并族 |
| **未定义行为可能"看着对"** | 变异 Q1（键丢掉设备格式）复现 VUID 的同时**像素照过**：管线当初是按另一个 render pass 编译的，驱动只是恰好把这次画对了。所以这一条的判据是验证层，不是画面——"画面对了"证明不了兼容性 |
| **窗口读回要按表面字节序** | `xcb_get_image` 回来的字节是**服务器/表面**的顺序：本机交换链是 `B8G8R8A8_*` ⇒ `[B,G,R]`。第一版夹具直接在 `[0]` 上比红色，把红蓝读反了——而三角形选的洋红在红蓝上对称，这个错误**看不出来**。夹具现在按 `device_color_formats` 把像素归一成 (r,g,b) 一次，并把颜色选成"红蓝不对称"（背景蓝、覆盖层源红） |
| **记录一帧两次会碰到 probe 的跨帧序** | capture 写的是同一块目的缓冲；会话有好几帧在飞时这就是写-写。修法是声明顺序的屏障（不是加环：probe 的语义正是"读这帧的最后一张图"，而宿主读之前要等设备） |
| **`Session` 中途换帧图不安全** | 试过"settle 帧只挂窗口图"（避开离屏 capture 的重复录制）：`assignFrameGraphs` 会释放**在飞**的命令缓冲 ⇒ `VUID-vkFreeCommandBuffers-pCommandBuffers-00047`。在飞的帧没有公开的等待点（`shutdown()` 才有），所以这条只能整片地做（登记到下面的口子） |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| Q1：键里丢掉设备格式（`TargetShape::compatibility()` 不搬那两个字段） | `pipelines()` 从 2 变 1（计数器断言红）+ `VUID-vkCmdDrawIndexed-renderPass-02684` 复现（验证层红，4 条）+ **像素全绿**（未定义行为，见上） |
| Q2：capture 不声明顺序（去掉那条屏障） | 同步验证从 0 变 **4** 条 `SYNC-HAZARD-WRITE-AFTER-WRITE`（写的是同一块缓冲） |
| Q3：不跑 settle 帧 | 窗口读到 **(0,0,0)**：呈现是异步的，`commitFrame()` 之后立刻读会拿到旧后备存储（登记在 §11.16o 的既有事实） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **527 用例 / 85 套件全绿**（+1 用例 / +1 套件） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / **808** 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过；`check_vsg_upstream_capabilities.py` 7 条全部仍成立 |

**本片留下的口子（登记，不假装解决）**：

* **离屏目标的多趟写入**：`OffscreenTarget::passGraph()` 里 bootstrap 恒为 `true`（每趟都清），且离屏内容挂在
  **pass 图**里而不是目标的稳定视图下 ⇒ 同一目标一帧两趟时第二趟会擦掉第一趟、内容只画在自己那趟里。计划侧
  （`CompiledPass::bootstrap` = "该目标的第一个写者"）已经能表达，执行侧还没接上——留给"多写者离屏目标"那片；
  **已关，§11.16s**（M5a）：执行侧接上了——`passGraph` 按计划解析出的 load op 选/建**渲染通道变体**，
  第二趟 LOAD 第一趟写的东西；"内容只画在自己那趟里"正是对的（LOAD 保住了第一趟的画面）。
* **`Session` 在飞的帧没有等待点**：中途 `assignFrameGraphs` 会释放仍在待处理的命令缓冲（VUID 00047）。要用
  "换图"表达"这一帧只画窗口"的用例（本片的 settle 帧本可以更省）得先有那个等待点（或 vsg 侧的回收协议）；
* **capture 的宿主读序**：屏障声明的是**设备侧**的顺序；宿主 `probe()` 前仍要靠 `deviceWaitIdle`（真设备用例
  都这么做）。多帧在飞时，"读第 N 帧的图"与"第 N+2 帧正在写同一块缓冲"之间的宿主侧顺序没有机制，只有约定；
* **`prepare` 的 renderArea 仍无活改尺寸证据**（§11.16o 登记，M5 之前不动）；
* **窗口内容视图仍是"一个管整个窗口"**：与 pass 的对应关系不存在（本后端不覆盖逐视图状态），若将来某个 pass
  需要逐视图状态（例如按视图不同的深度范围），那才是"逐 pass 视图"的时机。


| 有意不做（写在这里，不埋在实现里） | 内容 |
| --- | --- |
| 输入的**深度**半边 | "深度真可采样才绑"与 shadow 的专用绑定（`shadow_bound` + `VineShadowBlock`）一起留给 M5：键里另一个 `depth_sampleable` 字段现在的含义是"pass 自己的目标深度可采样"（M3d-3b-0 的事实 + 执行器核对），在 M5 重钉之前不在这里造第二条语义 |
| 全屏绘制调用（`DrawKind::Screen`） | **已接（§11.16p 内容侧 / §11.16q 计划侧）**：键带 kind、`createScreen` 建全屏层（set 0 = 采样集）、`recordScreen` 录 `Draw(3)`；计划解析其状态（深度 `Disabled`），`api/ContentPass` 按 kind 找半片并把 pass 的输入绑在 set 0 + 推 128B push（内容全零，光照/深度相位各管一半）。采样线没有第二套机制——"源附件 i→binding i"就是 M3d-3b-8 那条线的另一个消费者 |
| 着色器绑定声明的核对（"声明了却供给不了"） | 旧实现靠扫源码 `layout(binding=…)`（`MissingDescriptorBinding`）。这一片不做：驱动/验证层会报，而"谁声明了什么"要动 `ProgramFacts`，留给需要它的那片（M4 的拒绝理由与旧实现逐字对齐时） |

| 落地抓到的 | 内容 |
| --- | --- |
| 顺带修掉的既有偏差（验证层抓的） | 整仓跑验证层时 `MrtTargetTest` 报 `VUID-vkCmdDrawIndexed-firstAttachment-07476`：那两个附件用例的绘制**没告诉录制器**自己写几个附件（默认 1）⇒ 动态混合命令只覆盖附件 0。修法是那一行 `draw.color_attachments = 2U`；修完全套 0 VUID（此前该套件只在无层的情况下跑过，所以这条一直没露） |
| **变异反证 ×4（全部实测）** | A：计划丢掉输入表（`pass.inputs = {}`）⇒ `FrameCompilerTest` 与真设备用例同时红；B：键里计数写回 0 ⇒ 着色器读一个布局里不存在的 set 1，**段错误**（139）；C：注册表不回答"要发采样绑定"（`inputs_issued` 恒 false）⇒ 注册表 6 条断言红 + 真设备用例在"绑一次"那句红、随后段错误；D：彩色收尾改回 `TRANSFER_SRC` ⇒ **像素仍然通过**，但验证层报 6 条（`vkCmdDrawIndexed-imageLayout-00344` + `vkBarrier-oldLayout-01197`）——布局这类主张只有仪器看得见，这条已记进记忆 |
| 工具事实 | 本机 `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` 对**测试二进制**同样有效（不只是 selftest）；`VK_LAYER_ENABLES=…SYNCHRONIZATION_VALIDATION_EXT` 也能整仓跑（512 用例约 3.5 s，完全可做常规门禁） |
| 证据 | `test_vsg` 全量 **512 用例 / 82 套件全绿**（+5）；插件目标与全仓 `ninja` 零 error / 零 warning；强制验证层整仓 **0 VUID**，再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / **800** 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

### 11.16s M5a（2026-09-22）：离屏目标的多写者（LOAD 变体，真设备像素）

M4c 登记的口子：`OffscreenTarget` 只建**一张**渲染通道、load op 恒 CLEAR，所以同一目标一帧两趟时第二趟会擦
掉第一趟。计划侧早就能表达（`CompiledPass::bootstrap` = "该目标的第一个写者"、`planClearValues` 的第 1/2/3
条规则），这一片把执行侧接上。三件事：

1. **计划的三个输入决定 load op**（`api/VsgExecutor::recordOffscreen`）：`pass.clear` / `pass.bootstrap` /
   `pass.depth_preserved` 交给 `passGraph`，目标由此解析出 `PassClearPlan` 并选中要用的渲染通道变体。执行器
   不再自己判断"要不要清"——它只搬事实。
2. **目标按 load op 建渲染通道变体**（`api/OffscreenTarget` + `core::LoadOpVariantKey`）：`LoadOpVariantKey`
   从"粗粒度一份"改成能表达变体的形状（颜色集合一份 + 深度独立一份：load/store + 起始/收尾布局 + `has_depth`），
   `core::loadOpVariantOf(plan, color_final, depth_final)` 是唯一的装配处；目标按该键缓存渲染通道对象，
   framebuffer 与附件在所有变体间共享（`passVariantCount()` 是可读的计数）。
   **关键不变量：所有变体的子 pass 结构与依赖列表逐位相同**——依赖掩码是渲染通道**兼容性**的一部分
   （验证层点名 `VUID-vkCmdDrawIndexed-renderPass-02684` 会比较 `pDependencies`），只有 load/store 与布局可
   以差。因此依赖取"该形状任何一趟都可能需要"的超集，其中颜色的目的域补上了
   `COLOR_ATTACHMENT_READ`（LOAD 的一趟要**读**附件）。
3. **"没人写过"是计划的事实，必须由目标报出来**（`api/OffscreenTarget::written()`）：投影出来的新问题——
   一个刚建好但还没画过的目标，其镜像处于 UNDEFINED；`planTarget` 的
   `RepairReason::Bootstrap`（"nothing usable yet - the first pass in has to clear"）本来就有这条规则，但事实
   得有人报。`written()` 在**建 pass 图时**置位（"有人往里录过东西"），夹具与将来的 target 账用
   `TargetFacts::current.built = target->written()`。

| 文件 | 是什么 |
| --- | --- |
| `core/Keys.hpp` / `Keys.cpp` | `LoadOpVariantKey` 改成变体的名字（颜色集合 load/store + 起始/收尾布局；深度独立一份 + `has_depth`）+ `operator==` + 审计表那一行 |
| `core/ClearPlan.hpp` / `ClearPlan.cpp` | `loadOpVariantOf(plan, color_final, depth_final)`：CLEAR 从 UNDEFINED 起步、LOAD 命名上一趟留下的布局；**清屏值不进键**（值属于 pass 实例） |
| `api/OffscreenTarget`（+.hpp） | 变体表（键 → 渲染通道对象）+ `renderPassFor()`；`makeOffscreenRenderPass` 改为按变体建造（依赖列表取超集，与 load op 无关）；`passGraph(policy, bootstrap, depth_preserved)`；`passVariantCount()`、`written()` |
| `api/VsgExecutor` | `recordOffscreen` 把计划的三个事实交给目标 |
| `tests/test_vsg/ClearPlanTest.cpp` | +1 无设备用例：变体命名"做什么"而不命名"清成什么"（CLEAR ⇒ 起始 UNDEFINED、LOAD ⇒ 起始 = 收尾布局；值不同的两份计划是**同一个**变体；保留的深度走 LOAD；纯色目标深度位为空） |
| `tests/test_vsg/ContentPassTest.cpp` | +1 真设备用例：一帧两趟同一目标（第一趟 bootstrap 清屏 + 左网格，第二趟 LOAD + 右网格），像素三条（背景是第一趟的清、左右两个网格都在）+ 三个计数（`pool.created() == 1`、`pipeline_binds() == 2`、`passVariantCount() == 2`）；夹具的 `built` 改成 `target->written()` |

| 规则 | 结论 |
| --- | --- |
| **load op 是"交换半边"，不是身份** | Vulkan 的兼容性规则里没有 load/store 与布局，所以一个变体对象的管线在另一个变体里**合法**——这正是"两趟 pass 共用一个变体"能成立的原因；反过来说，**依赖掩码绝不能随变体变**（变异 Q3 实测：4 条验证层报错，含 `VkRenderPassBeginInfo-renderPass-00904`——变体连自己的 framebuffer 都不兼容了） |
| **"没人写过"不能 LOAD** | 新目标的第一趟必须清（颜色与深度都是）；报这条事实的只能是目标自己（`written()`），硬写 `built = true` 的症状是 `VUID-vkCmdDraw-None-09600`（"期望 DEPTH_STENCIL_ATTACHMENT_OPTIMAL，当前 UNDEFINED"）——只有同步验证看得见 |
| **值不属于变体** | 清屏值是 pass 实例的事，所以两趟清成不同颜色的 pass 共用一份渲染通道对象、一份管线；变体键里只放"操作 + 布局" |
| **bootstrap 是"第一个写者"的** | 同一目标一帧两趟：第一趟 bootstrap（清），第二趟不是（LOAD）；变异 Q1（执行器写死 bootstrap）⇒ 第二趟清屏 ⇒ 背景与左网格一起消失（像素读回 (0,0,0)、(0,0,0)） |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| Q1：执行器忽略计划（`passGraph(clear, /*bootstrap*/ true, false)`，即 M4c 之前的形态） | 新用例红 4 条：`passVariantCount` 2→1、背景 (0,0,0)、左网格 (0,0,0)（第一趟的画面被第二趟擦掉） |
| Q2：`loadOpVariantOf` 忽略 LOAD（恒 Clear） | 无设备用例 3 条红（`color_load`、`color_initial`、`color_initial=ShaderReadOnly` 那句）+ 设备用例红（变体数 1、背景黑） |
| Q3：依赖掩码随变体变（LOAD 时多一个 stage 位、颜色目的域去掉 READ） | 用例本身**全绿**，验证层 4 条：`VkRenderPassBeginInfo-renderPass-00904`（变体与 framebuffer 不兼容）+ `vkCmdDrawIndexed-renderPass-02684`（共享管线跨族）——"依赖是兼容性的一部分"由此有反证 |
| Q4：`written()` 恒 true | 整仓同步验证 0 → **4** 条 `VUID-vkCmdDraw-None-09600`（深度附件期望 DEPTH_STENCIL_ATTACHMENT_OPTIMAL、实际 UNDEFINED） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **529 用例 / 85 套件全绿**（+2 用例） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / 808 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **`passGraph` 的默认参数是"bootstrap"**（`bootstrap = true, depth_preserved = false`）：手驱路径（`renderGraph()`
  与旧夹具）仍按"清"来建图，因为它的调用方不经过计划；等手驱路径退役再收掉这两个默认值。
* **退役/重用变体**：变体表只增不减（一个目标最多几种 load-op 组合，且随目标一起销毁）；若将来出现"每帧换
  load-op"的形态，这里要按 `RetirementQueue` 处理。
* **`written()` 在"录了但没提交"时也为真**：事实是"有人往里录过东西"，不是"GPU 写过"；对 bootstrap 判断够用
  （下一帧的 pass 会写），但"提交失败后重来"的语义要靠 `attachments_invalidated` 那条路（M6/M7 再说）。
* **深度侧的 promote/borrow 布局仍只走"附件布局"**（M3c 的既有形态）：`depth_final` 目前恒为
  `DepthAttachment`，阴影那片的"提升成纹理"会带来第三种收尾布局——那时变体键的深度半边才真正被用满。
  **已关，§11.16t**（M5b）：`core::depthFinalLayout` 给了第三种收尾布局（纯深度可采样 ⇒ `ShaderReadOnly`），
  LOAD 变体的 `depth_initial` 改成命名 `depth_steady`（借用者命名的就是出借者的布局）。
* 前一版（§11.16r）留下的 `Session` 等待点、capture 宿主读序两条口子不变。

### 11.16t M5b（2026-09-22）：输入表带上了深度半边（"阴影脊柱"，真设备像素）

M5a 登记的口子：变体键的**深度半边**还只是"附件布局"一种取值，"提升成纹理"没接
上；而 SDK 的输入契约原文
就是"该来源的**每一张颜色附件**（外加**那张深度**，只要它可被采样）"——上一片只实
现了前半句。这一片补后半
句，并且用"一趟 pass 采样另一趟写进**纯深度目标**的值"来证明它：

1. **计划为输入解析出深度事实**（`core/FrameCompiler::resolveInputs`）：`Compiled
Input` 加 `depth_sampleable`，
   值与目标自己那份计划**同源**（`core::depthPlan(found->depth).sampleable`）——
"这个深度能不能被采样"
   一帧只有一个答案，采样者与被采样者不会各说各话。
2. **深度-only + 可采样 = 深度收尾在采样布局**（`core::depthFinalLayout(shape, sam
pleable)`）：只有"purely
   深度 + 宿主要求可采样"的形状（阴影贴图的形状）把深度留在 `ShaderReadOnly`——
   它的深度**就是**那幅画，下一趟采样它不需要自己的屏障；任何带颜色附件的目标
   仍留 `DepthAttachment`（下一趟还要拿它做深度测试）。这条规则只依赖形状与宿主要
   求，所以**无设备可钉**（`CoreClearPlanTest` 两条用例）。
   `OffscreenTarget::depthSteadyLayout()` 用同一个函数，并且**借用者问出借者**：
   借来的深度处在出借者留下的布局里，所以借用者的 LOAD 变体声明的是出借者的收尾
   布局。
3. **内容层绑定深度**（`api/ContentPipeline` + `api/ContentPass`）：采样集合的布局
从"颜色数"扩成（颜色数, 深度数）
   的**对**（`SampledKey`），绑定顺序**固定为：输入逐个来，一个输入内部先颜色附件
（按附件序）后深度**——所以
   深度绑定号 = 颜色数（阴影那片的程序里就是 `binding 0`，因为它没有任何颜色输
入）；深度用**NEAREST** 采样器
   （`ContentPipeline::depthSampler()`）：着色器比对的是精确深度，插值出来的值
等于一个谁也没栅格化过的深度。
   `PipelineKey` 因此也加了 `sampled_depth_count`（"这个管线的集合里有几个深度采
样器"是身份，与颜色同理）。
4. **给消费者的报价也校验深度半边**（`api/ContentPass::record`）：调用方与计划在
**两个半边**上都要一致，不一致
   就整趟不画，报文点名是哪一个半边动了（"计划说不可采样/计划说可采样而你没
给"）。
5. **深度捕获的出入布局跟着形状走**（`api/OffscreenTarget::create`）：原来写死
```
DepthAttachment → TRANSFER_SRC → DepthAttachment
```
   的一对屏障，在"深度收尾在 `ShaderReadOnly`"的形状上直接是**谎报**
   （实测 `VUID-VkImageMemoryBarrier-oldLayout-01197`：镜像实际在 `SHADER_READ_O
NLY`，屏障说它是
   附件布局），并且把镜像**放回错的布局**，于是消费者采样到全 0（整幅画面黑，而
深度探针却正常——像素与探针
   说的是两件事，这条差异就是线索）。改成"从 `depthSteadyLayout()` 出去、回
`depthSteadyLayout()`"，前向
   屏障的源作用域补上 `FRAGMENT_SHADER`（采样者已经读过它），后向补上
   `EARLY|LATE_FRAGMENT_TESTS | FRAGMENT_SHADER`（下一趟既当附件又当纹理）。
6. **纯深度形状被 `create` 接受**：`TargetLayout::color_formats` 为空 + 有深度
格式 = 合法的阴影贴图形状（原来"空形状"一律拒绝；
   注意 `Layout` 那个老重载会默默塞一张 RGBA8——夹具里必须显式 `color_formats.c
lear()`，这一条实测踩过）。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameCompiler.hpp` / `.cpp` | `CompiledInput::depth_sampleable`（输入表的
深度半边）+ `resolveInputs` 用 `core::depthPlan` 解析它 |
| `core/ClearPlan.hpp` / `.cpp` | `depthFinalLayout(shape, depth_sampleable)`（阴
影贴图形状 ⇒ `ShaderReadOnly`，其余 ⇒ `DepthAttachment`）+ `loadOpVariantOf(pla
n, color_final, **depth_steady**, depth_final)`：LOAD 必须命名**它保留的像素真正
在的**布局——借用者读的是出借者的布局，而不是自己写完之后的布局 |
| `core/Keys.hpp` / `.cpp` | `PipelineKey::sampled_depth_count`（+ `operator==`
、哈希、审计表那一行） |
| `api/OffscreenTarget`（+.hpp） | `depthSteadyLayout()`（借用问出借者）；`makeOf
fscreenRenderPass` 里的布局转换提到文件作用域的 `toVkLayout()`（捕获屏障与渲染通道
描述必须用同一个转换）；深度捕获屏障按 `depthSteadyLayout()` 进/出 |
| `api/ContentPipeline`（+.hpp） | `SampledKey{colors, depths}` + 哈希；`sampledSe
tLayout(colors, depths)`、`layoutFor(colors, depths)`；`depthSampler()`（NEAREST
） |
| `api/ContentPass.cpp`（+.hpp） | `sampledDepthCount(pass)`；报价两半边校验（不一致 ⇒ 整趟不画 + 报文点名）；`makeInputSet` 绑定顺序=先颜色后深度、深度用 NEAREST |
| `api/VsgExecutor.cpp` | 帧尾读回循环里 `readback()` 的空安全（一个目标可以只有
深度、没有颜色捕获） |
| `tests/test_vsg/SampledInputTest.cpp` | +1 真设备用例（"阴影脊柱"）：①纯深度可
采样目标（`D32`、清 0.0）+ ②消费者颜色目标（清蓝）；生产者三角形的片段着色器为
空（`settings.color_attachments = 0`），消费者采样深度并写出灰值。判据：计划的两
条输入事实、深度探针（三角内 0.5 / 外 0.0）、消费者像素（左半 0.5 灰 / 右半黑 /
三角形外保留清屏色） |
| `tests/test_vsg/ClearPlanTest.cpp` | +2 无设备用例：`depthFinalLayout` 四种形状
（纯深度可采样 ⇒ `ShaderReadOnly`；纯深度不可采样 / 带颜色 + 可采样 / 无深度 ⇒
`DepthAttachment`），以及阴影贴图形状的变体（CLEAR 起 `Undefined`、LOAD 起 `Shad
erReadOnly`，两者收尾都是 `ShaderReadOnly`，且是**两张**渲染通道对象） |

| 规则 | 结论 |
| --- | --- |
| **"可采样"决定收尾布局，而不是决定"要不要转"** | 纯深度可采样（阴影贴图）⇒
深度收尾 `ShaderReadOnly`，采样者零屏障；带颜色附件的目标⇒ `DepthAttachment`（下
一趟的深度测试要用）。规则是形状的函数（§11.16t-2 的两条无设备用例），所以相位/
目标/捕获三处不会各算各的 |
| **捕获屏障必须命名真正的当前布局** | 写死 `DepthAttachment` 的捕获入口屏障对阴
影贴图是谎报（`VUID-VkImageMemoryBarrier-oldLayout-01197`），而且它还把镜像放回错
布局 ⇒ 采样全 0。**探针绿 / 像素黑**这种撕裂，正是"探针读的是捕获副本、消费者读
的是原图布局"的指纹 |
| **借用的深度：LOAD 命名出借者的布局** | `depth_steady` 是"这趟不清它时它现在在
哪"，借用者是**出借者**的收尾布局；收尾仍是本层决定的 `depth_final`（借用者原样
还回去） |
| **绑定顺序是 ABI** | 输入逐个来，一个输入内部先颜色后深度 ⇒ 深度绑定号 = 颜色
数；阴影片（无颜色输入）里它就是 `binding 0`。`sampled_depth_count` 进 `Pipeline
Key`，因为"集合里有没有深度采样器"是管线身份的 ABI 半边 |
| **深度用 NEAREST** | 比对精确深度：线性插值出来的值是一个谁也没栅格化过的深度 |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| Q1：`depthFinalLayout` 恒 `DepthAttachment`（阴影贴图也收在附件布局） | 红：2
条 VUID（`vkCmdDrawIndexed-imageLayout-00344`：着色器访问时镜像布局与描述符不符）
+ 6 条断言（4 条无设备 + 2 条像素） |
| Q2：`makeInputSet` 不绑深度（`if (false && input.depth != nullptr)`） | 红：2
条 VUID（`vkCmdDrawIndexed-None-08114`：集合里的描述符无效）+ 进程段错误（lavapi
pe 读未绑定描述符）——"没绑"不是"少画点东西"，是未定义行为 |
| Q3：`resolveInputs` 把 `depth_sampleable` 恒 false（计划忘了深度半边） | 红：用
例 2 条（计划的输入事实 + 内容层**拒绝整趟**——报价与计划不一致）且进程不再崩溃
（拒绝发生在录制前） |
| Q4（过程记录）：夹具只设 `Layout::clear_color`、没给第二趟 `setClearPolicy` |
红：整幅清成黑（第二条）——补上策略后清屏色才生效；顺带证明"清屏是宿主决策、计划
只决定 bootstrap 何时override" |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **532 用例 / 85 套件全绿**（+3 用例） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验
证仍 **0 SYNC-HAZARD**；hygiene 0 / 808 文件；`check_diagnostic_formats.py` 0 /
39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **灯光与 `VineShadowBlock` 还没接**：这一片证明的是"深度能被采样"，还不是"阴影
算得对"——`shadow_bound` 这个键位、灯槽索引、
  `VineShadowBlock`（旧路径：颜色数 +2 处的块）与 `Light::castShadow` 的挑选逻辑
都留到 M5c。**灯光那半已关，§11.16u**（M5c-1）：前向光照块落地（`shadow_bound` / 阴影块 / 槽号仍是 M5c-2）。
* **采样集合的深度数仍是"每输入 0/1"**：计划里一个输入最多贡献一个深度；若将来
出现"一趟采样两个目标的深度"，`SampledKey` 的
  `depths` 计数已经够用，但绑定顺序的推导（=颜色数）要保持"先颜色后深度"的前缀
规则。
* **深度捕获的 `steady_layout` 对借用者的语义**：借用者捕获的是**共享**深度（出
借者的镜像），布局也按出借者算——如果将来允许"借用者独立提升"，
  这条要再想；目前借用深度恒不可采样（`core::depthPlan`），所以还到不了这里。
* 前一版（§11.16s）留下的 `passGraph` 默认参数、变体退役、`written()` 语义三条口
子不变。

### 11.16u M5c-1（2026-09-22）：前向光照块（灯到了着色器里，真设备像素）

M5b 登记的"灯光与 `VineShadowBlock` 还没接"拆成两片，这是**灯光**那半。要解决的问题只有一句：引擎
把灯光宣布给**一次绘制调用**，而这份数据此前没有任何一条路到达着色器。前向路径**不能走 push**——
128B 的保证预算被视图矩阵占满（引擎的前向顶点阶段就在那里读），所以灯光必须走**UBO**；全屏路径的
那半（128B push 里装灯与深度重建）留给它自己的相位。四件事：

1. **打包是一个规则，不是一个 memcpy**（`api/LightBlock` + `api/LightBlock.cpp`）：一个 ambient 槽
   （rgb + 强度，后宣布的覆盖前一个）+ 最多三盏方向光（按宣布顺序占 0..2 槽），世界坐标方向**乘相机
   的三个轴**换成视图空间（视图矩阵的前三行就是 r / u / -f），方向**归一化**（着色器拿它和法线做点
   积，未归一化等于把灯调亮），跳过 disabled 与块装不下的类型（Point/Spot 保留但无槽）。块是 ABI
   （`VineLightsBlock`，112B，`builtin_forward.frag` 声明的那份文本），静态断言钉死。
2. **"空灯单"仍然可见，但补光不算数**：没有可用的环境光时块里填 0.15 的 ambient
   （否则着色把 albedo 乘成零），但**计数只算宿主宣布的灯**——补光被算进去，调用方就会报一个宿主从
   未宣布过的"丢弃"。返回值 = "块代表了几盏宣布的灯"，调用方拿去和宣布数比。
3. **没有相机 = 空块**：没有视图空间可换算时块保持全零（不是补光）。这是参考实现的行为，而且**看得
   见**：没宣布相机的 pass 是"不亮"，不是"被猜出来的环境光照亮"。
4. **每绘制调用写一块、四个动态偏移绑定**（`api/BlockStorage` 的第 4 个区域 + `api/BlockDescriptors`
   的 `kLightsBinding = 3` + `api/ContentPass` 在每个内容 draw 的命令循环**之外**写块）：契约宣布灯光的
   粒度是"一次调用"（该调用的每条命令共享），所以块也按调用写一次；`BlockDescriptors::Offsets` 从三个
   变四个（view / draw / material / **lights**），布局多一个 `UNIFORM_BUFFER_DYNAMIC` 绑定。
5. **丢弃报告是"每 episode 一次"**（`ContentPass::reportLightsDropped` + `Scope::lights_dropped`）：
   规则照旧实现搬——`announced == 0 || represented >= announced` ⇒ **re-arm**（episode 结束），否则
   只在 `shouldReport()` 为真时报一次；报文分三支，且每支都说清楚**替代物**是什么（没相机 ⇒ 不亮；
   全 unusable ⇒ 补光；部分 ⇒ 缺几盏）。`ReportOnce` 放在 caller 提供的 `Scope` 里：episode 的结束由
   调用方决定（一帧一个 scope ⇒ 每帧一次；一个 session 一个 scope ⇒ 全程一次）。

| 文件 | 是什么 |
| --- | --- |
| `api/LightBlock.hpp` / `.cpp`（新） | `VineLightsBlock`（112B：ambient + `dirs[3]` + `cols[3]`，std140 静态断言）+ `packLightBlock(lights, camera, out)` 的完整规则与"没有相机 ⇒ 空块" |
| `api/BlockStorage`（+.hpp） | 第 4 个区域 `lights{112, 1, 3, 1024}`（每调用一块）+ `writeLights` + `Regions` / `Strides` / `overflows()` 的相应半边 |
| `api/BlockDescriptors`（+.hpp） | `kLightsBinding = 3` + 第 4 个 `UNIFORM_BUFFER_DYNAMIC` 绑定 + `Offsets::lights`（动态偏移数组按**绑定顺序**四个） |
| `api/ContentPass`（+.hpp） | 每个内容 draw 写一块、把 offset 传进 `recordCommand`；`Scope::lights_dropped`（`core::ReportOnce`）+ `reportLightsDropped`（re-arm 规则 + 三支报文，`ChannelIgnored`）；文件注记里"深度半边还没接"的过期句子一并更正（M5b 已接） |
| `tests/test_vsg/LightBlockTest.cpp`（新） | 6 条无设备用例（视图空间换算、**旋转相机**才是判据、disabled/无槽类型跳过、空灯单 = 补光且不计数、四盏方向光只装三盏、无相机 ⇒ 空块）+ 1 条真设备像素用例（**一帧一趟四带**：带 0 灯朝观察者 = ambient + 太阳；带 1 同色反方向 = 只有 ambient；带 2/3 宣布装不下的灯 = 补光，且两条之间**只有一条**报告；带外是 pass 自己的清屏色） |
| `tests/test_vsg/BlockDescriptorsTest.cpp` | 三条断言从"三个绑定"改成"四个"（布局、集合的描述符范围、动态偏移数组按绑定顺序四个） |

| 规则 | 结论 |
| --- | --- |
| **光照块是宿主决策的载体，不是身份** | 与材质块同理：块在描述符集里、按动态偏移选，不进 `PipelineKey`——"灯换了"是写一块新字节，不是编译一份新管线 |
| **方向必须换成视图空间** | 而**轴对齐的相机会让缺失的旋转变不可见**：变异 M1（方向原样留在世界空间）只有算术用例红，真设备像素用例仍然全绿（那台的相机在 +Z 且目不斜视）。这正是"判据要选对"的又一例：约定用算术用例钉，像素管"能用" |
| **补光不算灯** | 变异 M2（把补光算进返回值）⇒ 5 条红：4 条算术 + 设备用例的报告断言（"全装得下"⇒ 不报告 ⇒ 计数 1 变 0） |
| **块是每次调用的** | 变异 M3（整趟 pass 只打包第一调用的灯）⇒ 设备用例三条像素红：带 1..3 全变成带 0 的颜色 `(128,26,26)` |
| **布局与声明必须一致** | 变异 M4（布局里去掉 lights 绑定，着色器仍声明 binding 3）⇒ 无设备用例红（绑定数 3≠4），设备用例**在驱动里段错误**（未定义行为，崩溃前 0 VUID）——"没绑"不是"少画一点" |
| **清屏仍是宿主的** | 夹具不把 `setClearPolicy` 交给第二趟以后的 pass，看到的就是黑屏（M5b 已记一次，这一片在四带图里又踩一次：带外的"背景"必须是 pass 自己的清屏色，否则"背景"断言什么都没有 |
| **报告要有替代物** | 三条报文都说清了替代物（不亮 / 补光 / 缺几盏）：只说"丢弃"会让读者猜是黑屏还是补光 |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| M1：世界的方向不换视图空间 | 1 条红（`TheDirectionsAreRotatedIntoViewSpace`）；设备用例**全绿**（轴对齐相机看不出）——判据选对的实测 |
| M2：补光算进"代表了几盏" | 5 条红（4 条算术 + 设备用例的报告计数 1→0） |
| M3：整趟 pass 只打包第一调用的灯 | 设备用例红 4 条（报告计数 + 带 1/2/3 像素全变成带 0 的 `(128,26,26)`） |
| M4：布局去掉 lights 绑定 | 无设备用例红（3≠4）+ 设备用例段错误（崩溃前 0 VUID） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **539 用例 / 86 套件全绿**（+7 用例、+1 套件） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / 811 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **阴影块（`VineShadowBlock`）与 `shadow_bound` 键位还没接**（M5c-2）：灯的"槽号"（`params.w` 指向
  块里第几盏方向光）、`Light::castShadow` 的挑选、以及把 map 与块绑到内容 ABI 里，都是那一半的事。
  **已关，§11.16v**（M5c-2）：阴影块落地（`packShadowBlock` + `kShadowBinding = 4` + 槽号），而
  `shadow_bound` 键位按实测**删掉**（本后端的 ABI 里它是冗余的，见该节）。
* **全屏路径的 128B push 内容仍全零**：`deferredLightProgram` 读的 ambient/sun/projparms 是光照相位
  的另一半（全屏不需要矩阵，所以它走 push），本片只做了前向 UBO 那半。
* **引擎自带的前向程序文本与本后端的 set 0 布局不同**（登记为场景桥的债）：SDK 的
  `builtin_forward.frag` 声明 material 0 / diffuse 1 / lights 2（外加 shadow 3/4），draw 块还在 **set 1
  binding 0**；而本重写版的 set 0 是 view 0 / draw 1 / material 2 / lights 3。两者都能工作，但**同一个
  管线的着色器文本与集合布局必须匹配**——场景桥把那批程序接进来时，要么按 SDK 的绑定建集合，要么
  在文本层做转换，不能两套 ABI 混配。
  **已决（§11.16ae，M8a）**：按**程序文本自己的声明**服务——布局跟着声明走（M8b）、每个声明按角色取值
  （M8c）。理由是这句债的反面：文本是宿主的，后端选不了它的绑定号；重写版自己的 set 0 布局因此只是
  “另一种声明”（它的程序照 `VineViewBlock` 这类 L1 名声明即可）。M8a 已把这批文本的声明逐条读成事实。
* 前一版（§11.16t）留下的"灯光槽索引/阴影块""采样集合深度前缀规则""借用者捕获布局"三条口子，
  除前两条外不变。

### 11.16v M5c-2（2026-09-22）：阴影块（一张 map 只缩放它属于的那盏灯，真设备像素）

M5c 的后半片。M5b 把 map 当**采样输入**绑上了（深度半边），但着色器拿不到"一个片元落在 map 的哪里"——
map 是**那盏灯的相机**栅格化的，消费 pass 用的是自己的相机，所以块里必须带
`light_vp * inverse(view)` 与比较用的标量（`VineShadowBlock`：一个 mat4 + `params` = 开关 / bias / 强度 / 灯槽
号）。四件事：

1. **map 的"身份"是目标说出来的**（`core::ShadowFacts` + `TargetFacts::shadow`）：`RenderTarget::shadowOf`
   说这张 map 属于哪盏灯、`setProducerViewProjection` 说怎么读它；事实由调用方（会话/执行器）从 SDK 目标
   读进来，计划原样复制。参考实现踩过的坑就在这里：**"第一个深度可采样的输入就是太阳的 map"会把 G-buffer 的
   深度当成阴影贴图**（整个 deferred 分支"影子是世界坐标的函数"，`.ai/design/graphics-shadow.md` 有记录）。
2. **计划解析"这趟 pass 的 map"**（`core/FrameCompiler::resolveShadow`）：输入里**第一个**
   "目标表明它属于某盏灯 + 深度可采样 + 生产者公布了矩阵"的输入；三个事实缺一个 ⇒ `light == nullptr`
   ⇒ 着色开关保持关闭（不假装有一张能读的 map）。
3. **打包与开关**（`api/ShadowBlock`）：按身份找到 map 的那盏灯，要求它**这趟调用宣布了、启用了、还在投影
   （`castShadow`）、类型是方向光、且在灯块的三个槽内**；满足才写矩阵（列主序）与
   `params = {1, 该灯自己的 ShadowSettings::bias, 1, 槽号}`，否则整块**零字节**（开关关）。
   `api/LightBlock` 新增 `directionalSlotOf(lights, identity)`——"这盏灯落在第几槽"必须和灯块打包**同一次
   遍历**（旧实现的注释原话："the same walk collectLight…"），否则 `params.w` 会指向另一盏灯。
4. **每绘制调用写一块、第 5 个动态偏移绑定**（`BlockStorage` 第 5 区域 + `BlockDescriptors::kShadowBinding = 4`）：
   块的内容与调用有关（相机 + 灯单），所以粒度仍是"一次调用"；开关关的调用也**照写**（全零），因为同一个
   着色器文本要同时服务有影/无影两种 pass——**绝不能留下未绑定的块**。

**顺带的键修正（实测推论，已改设计）**：`PipelineKey::shadow_bound` **删掉**。它在 v1 里是从旧实现抄来的
（旧路径的前向 shader set 有"插入阴影 ABI"的变体 ⇒ 集布局不同 ⇒ 管线不同），而在本后端的 ABI 里：
map 走**采样输入**（`sampled_depth_count` 已经是键的一半）、阴影块**恒在块集里**（shader 可以少声明，集合布局
不变）、有影/无影是**运行期开关**（同一个文本）——留着一个"改了 GPU 对象却不变"的键字段，正是键审计表要防止
的事：它只会让本该共用一份管线的两个 pass 分家。§D3 的那一行按此更正。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameRecorder.hpp` / `.cpp` | `LightRef` += `identity`（`Light*`，阴影靠它认领）/ `cast_shadow` / `shadow_bias`（`ShadowSettings::bias`）；`snapshotLights` 逐项填 |
| `core/FrameCompiler.hpp` / `.cpp` | `ShadowFacts`（谁的 map + 是否公布矩阵 + 生产者矩阵）+ `TargetFacts::shadow` + `CompiledInput::shadow` + `CompiledPass::shadow`；`resolveShadow`（三个事实、先来先用） |
| `core/Keys.hpp` / `.cpp` | **删** `PipelineKey::shadow_bound`（等值 / 哈希 / 审计表行同步） |
| `api/ShadowBlock.hpp` / `.cpp`（新） | `packShadowBlock(shadow, draw, out)`：身份匹配 + 启用/投影/类型/槽检查 + `light_vp * inverse(view)`（列主序）+ `params`；不可用 ⇒ 全零 + false |
| `api/LightBlock`（+.hpp/.cpp） | `directionalSlotOf(lights, identity)`（与打包同一次遍历） |
| `api/BlockStorage` / `api/BlockDescriptors` | 第 5 区域 `shadows{80,1,3,1024}` + `writeShadows`；`kShadowBinding = 4` + 5 个动态偏移 |
| `api/ContentPass`（+.hpp） | 每绘制调用打包/写阴影块（开关关也写）+ `shadow_offset` 传入 `recordCommand` |
| `tests/test_vsg/ShadowBlockTest.cpp`（新） | 4 条无设备用例（视图→灯 clip 的组合**用两张矩阵语义验证**且相机非恒等、七种缺一事实的拒绝 + 块必须全零、第四盏方向光的槽号、`directionalSlotOf` 与打包**互相钉住**）+ 1 条真设备像素用例（**一帧四带**，见下） |
| `tests/test_vsg/FrameCompilerTest.cpp` | +1 无设备用例：**四输入**（G-buffer 深度带矩阵且排第一 / 深度不可采样的 map / 没公布矩阵的 map / 可用的 map 排最后）⇒ 计划必须选最后一个；每条被跳过的输入都要说得出原因 |
| `tests/test_vsg/BlockDescriptorsTest.cpp` / `VariantCoreTest.cpp` | 绑定数 4→5（含范围与动态偏移）；把 `shadow_bound` 的开关换成 `sampled_depth_count` |

| 规则 | 结论 |
| --- | --- |
| **只能由目标"认领"** | 变异 M4（去掉"目标说了属于哪盏灯"这个条件，任何"可采样深度 + 有矩阵"都算）⇒ 计划把 G-buffer 的深度当成 map（`pass.shadow.light == NULL`）——这就是参考实现的实测缺陷 |
| **槽号决定缩哪盏灯** | 变异 M2（`params.w` 恒 0）⇒ 设备用例带 2 从 `(26,128,26)` 变成 `(128,26,26)`：本该被缩放的那盏灯没被动，另一盏被缩没了——"整张 map 缩放所有灯"的经典错误 |
| **开关是灯的运行期事实** | 变异 M3（忽略 `castShadow`）⇒ 无设备用例 + 设备用例带 1 红（同一盏灯在两趟调用之间**被翻开开关**⇒ 带 1 从全亮变成只剩 ambient）；相机夹具第一版用**另一个 Light 对象**表达"不投影"，M3 只在无设备用例红——那测的是**身份**不是开关，现已改成同一对象翻开关 |
| **组合必须走世界** | 变异 M1（丢掉 `inverse(view)`，直接用生产者的矩阵）⇒ **只有算术用例红**（设备用例的相机只有平移，而测试选的生产者矩阵不读 z ⇒ 平移不可见）。又是一次"判据选对"：这类约定归算术用例 |
| **矩阵是列主序** | 变异 M5（按行写）⇒ 算术用例红（测试的矩阵有非对称项 (0,1)=0.25 / (1,0)=-0.125，对角矩阵看不出来） |
| **拒绝必须写零** | 变异 M6（拒绝时把 `params.x` 留成 1）⇒ **只有字节级用例红**：设备像素看不出（零矩阵 ⇒ `light_clip.w = 0` ⇒ NaN ⇒ 着色器的范围判断全 false ⇒ 恰好落回"亮"，那是巧合而不是检查） |
| **块是每次调用的** | 变异 M7（整趟 pass 只打包第一调用的阴影块）⇒ 设备用例三条像素红（带 1/2/3 全按带 0 的灯与槽算） |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| M1：组合丢掉 `inverse(view)` | 1 条红（算术用例；设备用例绿——轴对齐/纯平移相机看不出） |
| M2：`params.w` 恒 0 | 设备用例红（带 2 的颜色变成"另一盏灯被缩放"） |
| M3：忽略 `castShadow` | 2 条红（无设备 + 设备带 1） |
| M4：不要求目标认领（任何可采样深度 + 矩阵都算） | 1 条红（计划选错输入：G-buffer 的深度变成 map） |
| M5：矩阵按行写 | 1 条红（算术用例） |
| M6：拒绝时留下开关 1 | 1 条红（字节级用例；像素因 NaN 恰好正确） |
| M7：阴影块按 pass 打包 | 设备用例 3 条红（带 1/2/3 全用带 0 的灯） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **545 用例 / 87 套件全绿**（+6 用例、+1 套件） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / 814 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **`TargetFacts::shadow` 的生产侧还没接**：本片的夹具从 SDK 目标自己读（`shadowOf()` /
  `producerViewProjection()`）再填进事实表；会话/执行器那层要把它变成常规动作（和 `depth` 事实同一处）。
* **`params.y/z` 的语义只钉了"来源"**：bias 来自投影灯的 `ShadowSettings`（实测值随灯走），强度恒 1；
  `ShadowSettings::filter`（PCF）是 SDK 自己标了 RESERVED 的，本片不假装支持。
* **全屏路径的 128B push 光照/深度重建半边**仍未填（M5c-1 登记过），它读的是同一份灯数据的另一种表示。
* **场景桥的 ABI 债仍在**（§11.16u 登记）：SDK 自带前向程序的绑定与本后端 set 0 布局不同，接进来时要统一。
* 前一版（§11.16u）留下的 `TargetFacts::shadow` 生产侧、`params.y/z` 语义两条口子中，前一条已在上方登记。

### 11.16w M5d（2026-09-22）：全屏路径的 128B push（光照的第二种表示，真设备像素）

M4b 登记的"push 的形有证据、内容没有"到此关闭。全屏路径**不需要视图矩阵**（顶点阶段是
`gl_VertexIndex` 生成的正典三角形），所以它把整个 128B push 预算花在光照上——同一个场景的两条路径
因此有两种表示（前向 UBO / 全屏 push），值必须一模一样。三件事：

1. **push 是同一份打包的另一种布局**（`api/LightBlock` 的 `LightPushBlock` + `packLightPushBlock`）：
   实现**调用** `packLightBlock`（同一个遍历、同一套规则），再把三个字段搬进 push 的布局——
   两次遍历就是"同一个场景两条路径照出不同亮度"的入口。128B 的四个槽位：`ambient` / **`projparms`** /
   `dirs[3]` / `cols[3]`，静态断言钉死（含三个偏移）。
2. **`projparms` 是保留位，且保持全零**：它是"从深度缓冲重建视图位置"的程序要的
   near / far / proj[0][0] / proj[1][1]，而**随 SDK 发布的程序都不读它**（引擎自己的光照程序采样
   G-buffer 的视图位置附件，`builtin_deferred_lighting.frag` 在声明处就写明"this program does not
   read it"）。参考实现为透视相机填了它、没人读——"承诺了效果却没有效果"正是本项目一直在点名的
   失败家族；这一片**不发明读者**，把保留位留成可见的保留位。
3. **按"每次调用"推**（`api/ContentPass::recordScreenDraw`）：`packLightPushBlock(draw.lights, draw.camera, …)`
   的 `draw` 是**这一趟全屏调用**（相机与灯都是它宣布的），空灯单 ⇒ 补光（同前向路径的规则），
   丢弃**不上报**（丢报是内容路径的，参考实现亦然）。

**顺带修掉一个真洞（本片夹具当场撞到）**：`ContentPipeline::createScreen` 从不建
"没有输入时的 pipeline layout"，于是 `layoutFor(0,0)` 返回空 ⇒ **任何"只读 push、不声明输入"的全屏 pass
都被拒**（"its pipeline could not be built"——那是一条从未被请求的管线，而不是失败）。修法：屏幕层也建一份
"只有 push range、没有集合"的布局，`layoutFor(0,0)` 返回它；M4a 那条"全屏层没有布局"的断言按此更正。
"全屏程序只读 push"是合法调用（本片的设备用例就是这样），不是边角。

| 文件 | 是什么 |
| --- | --- |
| `api/LightBlock.hpp` / `.cpp` | `LightPushBlock`（128B：ambient / **projparms 保留** / dirs[3] / cols[3]，静态断言 + 偏移）+ `packLightPushBlock`（**复用** `packLightBlock` 的遍历，只换布局） |
| `api/ContentPass.cpp` | `recordScreenDraw` 的 push 从"128 个零字节"改成打包后的灯；`kFullscreenPushBytes = sizeof(LightPushBlock)`（+ 静态断言 128） |
| `api/ContentPipeline.cpp` | `createScreen` 建"push-only"布局（无集合）；`layoutFor(0,0)` 不再返回空 |
| `tests/test_vsg/LightBlockTest.cpp` | +2 用例：无设备（push 与 UBO 逐字段一致、`projparms` 全零、三个偏移、无相机 ⇒ 空 push、空灯单 ⇒ 补光）+ 真设备像素（**一趟三个全屏调用、三个视口**：带 0 灯朝观察者 = 0.5/0.1/0.1；带 1 同灯反向 = 只有 ambient；带 2 不宣布 = 补光 0.15；且 `ChannelIgnored` 计数为 0——全屏路径不上报丢弃） |
| `tests/test_vsg/ContentPipelineTest.cpp` | M4a 那条"全屏层 `layoutFor(0,0)` 为空"的断言更正为"返回 push-only 布局、集合为空" |

| 规则 | 结论 |
| --- | --- |
| **两种表示，一份打包** | 变异 P1（把 UBO 的字节直接塞进 push，两个布局混用）⇒ 无设备用例红 + 设备用例带 0 从 `(128,26,26)` 变 `(26,26,26)`：方向/颜色整体错位，太阳的项落进了保留位 |
| **内容真的有到** | 变异 P2（push 保持全零，即 M4b 的形态）⇒ 三条带全 `(0,0,0)`（未定义 push 内存"常常也读成零"，所以**反向**断言才是有用的方向——本条正是"M4b 的形有证据、内容没有"的实证） |
| **按调用推** | 变异 P3（整趟 pass 只推第一调用的灯）⇒ 带 1/2 变成带 0 的颜色 `(128,26,26)` |
| **阶段是 ABI 的一半** | 变异 P4（push 发到顶点阶段）⇒ **0 VUID**、带 1/2 仍显示带 0 的颜色：片元阶段从没读到新 push，静默失败——正是"阶段写错看不出"的实测 |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| P1：push 用 UBO 的布局 | 2 条红（无设备 + 设备带 0 错位） |
| P2：push 全零 | 1 条红（三条带全黑） |
| P3：push 按 pass 推 | 1 条红（带 1/2 用带 0 的灯） |
| P4：push 发到顶点阶段 | 1 条红（带 1/2 仍是带 0 的颜色；**0 VUID**，静默） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **547 用例 / 87 套件全绿**（+2 用例） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / 814 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **`projparms` 没有读者**：保留位按 SDK 的声明保持零；将来若出现"从深度重建视图位置"的程序，
  它的 near/far/proj00/proj11 必须**来自相机自己的投影矩阵**（`CameraSnapshot::projection` 的两个对角 +
  near/far），而不是在这里二次发明一套公式。
* **全屏路径的丢弃报告**：按参考实现，全屏调用不报（只有内容路径报）；若将来全屏侧也要报，
  `Scope::lights_dropped` 是现成的 episode 状态，但语义要先钉（"谁宣布的灯"在两个路径里是同一条契约）。
* **`shadowedDeferredLightProgram` 的 map/块（binding 5/6）仍未接**：那属于场景桥的 ABI 债（§11.16u），
  本片只填了 push。
* 前一版（§11.16v）留下的三条口子（`TargetFacts::shadow` 生产侧、`params.y/z` 语义、场景桥 ABI 债）不变。

### 11.16x M5e（2026-09-22）：目标生命周期（计划驱动的换尺寸、租约两向拒绝、停车 vs 计数空闲）

M5a–M5d 把"一个目标能画出什么"填完了，这一片填的是"**目标的尺寸和寿命**"，也就是
`core::planTarget`（M0 就落地、至今只有 `BackendCoreTest` 的表在照它）终于在真设备
上有了执行者。四件事：

1. **`OffscreenTarget` 的字段拆成"形状的"和"尺寸的"两块**（`api/OffscreenTarget`）
   ：`Data` 里新增 `struct Attachments`（颜色附件 + 视图 + 回读缓冲/映射、深度图
   像/视图/回读、帧缓冲、`render_graph`），并且**只由参数化函数构建**
   （`buildAttachments(width, height, out)`）——纯"按给定尺寸造对象"的一步，**不写
   本对象任何字段**。`create` 先建渲染通道与其首个变体（形状的），再调它，成功了才
   `std::move` 进 `attachments`，**借用计数也改成成功之后才 +1**（原来在 `return
   nullptr` 之前就加，创建失败会漏一个借用者）。
   尺寸之所以是**参数**而不是 `width()/height()`：换尺寸必须**先造好替代品**才能碰
   旧的，而造替代品的那一瞬间，目标仍在服务旧尺寸。
2. **`resize()` 让计划说话**（`ResizeInPlace` / `Rebuild` 都走同一条替换臂；`None`
   与 `Repair` 直接返回、一个 GPU 对象都不碰）：`TargetInstance` 由目标自己的事实填
   （`desc`、`generation`、`built = render_graph != nullptr`），`wanted` 的**形状就
   是目标自己的形状**——格式在 `create` 时定死，换形状是另一个目标而不是"改尺寸"。
   **替换的都是"描述里含尺寸"的对象**（图像/视图/回读缓冲与节点/帧缓冲/它周围的
   `render_graph`），**保留渲染通道、它的 LOAD/CLEAR 变体与所有按它们编译的管线**
   ——尺寸不进渲染通道兼容性（`core::TargetShape` 里没有 extent，§11.16 的键表早已
   钉死）。
3. **旧的一批对象不是"扔掉"，是"停车"**：`resize` 把它们交给 `retirement`
   （`RetirementQueue::retire(timeline, …)`），到退役点才释放。闸门关着（调用方从
   来没学到在飞槽数）时 `retire()` **返回 false 而不是猜窗口**——那时释放必须退回
   **计数过的 device idle**（`noteDeviceWait()` + `vkDeviceWaitIdle`），计数就是"帧
   路径不停设备"这条不变量的判据（§11.17 的 D5）。
   实现细节上有一处必须做对：`retire()` 的 `ReleaseFn` 是**按值**收的，拒绝时它直
   接把回调丢掉——**回调若按值持有那批对象，就会在 `retire()` 内部、没有 idle 的情
   况下把它们析构掉**（而提交过的命令缓冲可能还命名着它们）。所以这里用一个
   `shared_ptr` "托管"捕获：队列丢掉回调什么也不会析构，本地还有一份，退到 idle 那
   条路才真正释放。
4. **租约两向都拒绝换尺寸**（一个被借的深度图只有一个所有者，而**借用者的帧缓冲命
   名的是出借者的图像**）：
   * 出借方（`borrowers > 0`）：借用者的帧缓冲命名着本片要换掉的那张图，而本对象
     不知道它们是谁 ⇒ 调用方先重建借用者（参考实现读作 "the borrow points at
     another image"）；
   * 借用方（`depth_source != nullptr`）：新帧缓冲的深度附件会是**出借者的图、出借
     者的尺寸**，而帧缓冲附件必须**不小于**帧缓冲本身
     （`VUID-VkFramebufferCreateInfo-pAttachments-00861`）⇒ 尺寸不是借用方能挪的，
     调用方先换出借方、再按新尺寸重建这个目标。
   `refused` 两向都置位，**不替换、不停车**；借用者析构（计数递减）后同一条调用立
   刻替换成功。
5. **换尺寸后目标回到"从未被写"**：`written = false` + `generation += 1`。前者是执
   行者用 `bootstrap = !written()` 推"谁清屏"的那个事实（新图像是 UNDEFINED，LOAD
   没有意义）；后者是调用方"我编的那一帧命名的图像没了"的判据。

| 文件 | 是什么 |
| --- | --- |
| `api/OffscreenTarget.hpp` | `Attachments`（含 `Color`）+ `buildAttachments(width, height, out)`（文档写明"为什么尺寸是参数"）+ `generation()` + `Resized`/`resize()`（"换什么/留什么/拒绝什么/之后算作什么"四段契约）；`resize` 的 `@return` 写明"想要但没做到 = `replaced == false`：`refused` 是租约，`ResizeInPlace` 而没 `refused` 是构建失败" |
| `api/OffscreenTarget.cpp` | `Data` 拆字段（+`attachments`/`clear_policy`/`generation`）；`create` 改成"形状先、尺寸后、成功才接手、计数后加"；`resize` 实现（计划 → 租约检查 → 先造后换 → 停车/计数 idle → 换代） |
| `tests/test_vsg/OffscreenTargetTest.cpp` | +5 真设备用例：换尺寸后旧集**仍在**（引用计数 + `pending()`/`released()` 两条证据）且新尺寸**真渲染**（像素 + `probe` 的宽高）；同尺寸/0 尺寸 ⇒ 什么都不换且仍渲染；无停车窗口 ⇒ `deviceWaits()` +1 且对象真的释放；带深度目标换尺寸后**深度回读按新尺寸重建**（`depthProbe` 的宽高 + 清屏值不变）；租约两向拒绝 + 借用者死后同一调用成功 + 出借方还能再借出新图 |

| 规则 | 结论 |
| --- | --- |
| **租约是唯一的拒绝原因** | 变异"租约不再拒绝"⇒ 租约用例红（`refused` 为假、尺寸被换掉） |
| **停车 ≠ 丢引用** | 变异"报告 parked 却直接丢掉这批对象"⇒ `pending()` 与引用计数两条断言红（对象真的没了，而报告说它停着） |
| **`written` 必须复位** | 变异"不复位"⇒ 用例红，并且**验证层与像素同时报**（第二趟不宣布清屏 ⇒ 被当作 LOAD 而不是首写者） |
| **换代要真的换** | 变异"不 bump generation"⇒ 红；变异"换了对象却不采纳新尺寸"⇒ `width()/height()` 与探针红 |
| **退到 idle 要计数** | 变异"不计数"⇒ `deviceWaits()` 断言红（这条判据就是计数，不是"跑起来没崩"） |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| P1：报告 parked、实际直接丢 | 2 条红（`pending()` + 旧图引用计数） |
| P2：`written` 不复位 | 1 条红（`written()` 断言 + 随后的验证层报错 + 像素不符） |
| P3：不 bump `generation` | 2 条红（`Resized::generation` 与 `generation()`） |
| P4：忽略租约 | 2 条红（两向 `refused`/`replaced`） |
| P5：不采纳新尺寸 | 2 条红（`width()/height()`） |
| P6：退回 idle 不计数 | 1 条红（`deviceWaits()`） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **552 用例 / 87 套件全绿**（+5 用例） |
| 门禁 | 插件目标 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / 814 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **`resize` 现在没有执行者**：调用它的是将来的帧执行者/宿主窗口尺寸权威（`planTarget`
  的生产侧在 `FrameCompiler`，`TargetFacts::current` 目前由用例自己填）。本片给全了
  "换尺寸"这一半的语义与证据，但"谁在什么时候请求换尺寸"仍属于执行者那片；`Rebuild`
  臂（形状真的变了）在 `resize` 里与 `ResizeInPlace` 同路处理，真正需要重建内容/全屏
  槽与管线的那条路还没接。
* **`attachments_invalidated` 没有生产者**：`TargetInstance` 有这个事实、`planTarget`
  也照它答 `Repair(Bootstrap)`，但本后端还没有"失效但还在"的状态；留给会话级重建那片。
* **租约的"重建借用者"没有 API**：`resize` 说"调用方先重建借用者"，但"把借用者按新
  出借方重建"目前只能靠调用方先析构再 `create`（本片用例正是这么做的）。若将来借用成
  为常规用法，值得给一条显式的"重新出借"路径而不是让调用方拼。
* 前一版（§11.16w）留下的四条口子（`projparms` 无读者、全屏丢弃报告、`binding 5/6`
  的 ABI 债、`TargetFacts::shadow` 生产侧等）不变。

### 11.16y M6（2026-09-22）：读回（一张分类表、格式诚实、借用方读到源，真设备）

设计表（§4 第 9 行）的落地。三件事，外加两处顺手补掉的真洞：

1. **一张表**（`core/Readback.hpp` / `.cpp`）：`ReadbackState`（目标的事实：颜色附件数、被请求附件的颜色
   格式、深度格式、各自有没有交出过拷贝节点）+ `ReadbackRequest{kind, attachment}` →
   `ReadbackResult{ok, refusal}`。**优先级是写下来的**（无设备用例逐条钉）：请求存在性
   （`UnknownAttachment`）→ 格式能不能读（`UnreadableFormat`，**永久**原因）→ 有没有交出拷贝
   （`NotCaptured`，**下一帧可以再试**）。"永远不行"和"还没行"必须是不同答案：方向报错一次，读者就去错的
   地方找 bug。
2. **没有 `Empty` 这一类**（设计表第 2 行借用的 `ReadbackRefusal::Empty` 在重写版**换了家**）：活着的目标永远
   有可用尺寸——`create` 从 M5a 起就拒绝零尺寸（本片把这条规则写进 `@return`），而"还没尺寸 / 窗口隐藏"是
   **生命周期**的问题（`planTarget` 的 `Repair(SizeUnknown)`，执行者那半），不是读回能回答的。一个**走不到**的
   拒绝臂读起来像"覆盖了这个用例"，实际没覆盖，所以这张表里没有它。
3. **捕获簿记**：`Attachments::Color::captured` + `Attachments::depth_captured`，在 `capture(i)` /
   `captureDepth()` **交出节点**时置位——和 `written` 是同一条约定（记的是**录制**侧，"录了但丢帧"仍是同一个
   目标）。**簿记属于附件集**：换尺寸整体换集，新集的缓冲里什么都没有 ⇒ 标志天然为假。没有它，一次"还没跑过
   帧"的读回会返回**分配内存里碰巧有的东西**（旧实现的 `probe()` 就是这样），而头文件早就承诺过"没捕获 ⇒ 无效
   探针"——这一片把文档兑现了，并且让"为什么"可查（`readbackResult`）。
4. **格式诚实搬进 core**（`colorReadbackOf` / `depthReadbackOf` / `decodeDepth`）：RGBA8（4B/texel）、
   D16（2B，解码 ÷65535）、D32/D32F（4B，原样 float）、D24 组合格式（**拒绝**：没有"普通深度拷贝"这回事）。
   颜色侧**只打包 RGBA8** ⇒ float 附件（16F/32F）**不再建回读缓冲、不再录拷贝**，读回答 `UnreadableFormat`；
   这是**顺手关掉的真洞**——原来 `buildAttachments` 无论什么格式都按 4B/texel 建缓冲并录
   `CopyImageToBuffer`，一个 16F 目标第一次读回就是
   `VUID-vkCmdCopyImageToBuffer-pRegions-00183`（拷贝写到缓冲末尾之外），而此前**没有任何用例建过 float 目标**
   （所以它一直没被撞到）。**pass 照常渲染**：它的深度附件（可读格式）照样读回，本片就用这个当"这一趟真的跑
   了"的证据。
5. **借用方读到源**：借用方的深度拷贝从**共享图像**读、写进**它自己的**目标缓冲；它的 `readbackResult` 只认
   **自己**的交出记录（出借方捕获过 ≠ 借用方能读——借用方的缓冲里那时还什么都没有）。设备用例把两半都钉住：
   ①共享深度夹具里，**借用方**的 `depthProbe()` 在被拒绝的三角形处读到**出借方**的近值 0.5、在自己可见的
   三角形处读到自己的远值 0.1、别处是清屏值 0.0（一张图、两趟 pass 的写）；②只有出借方交出过拷贝时，借用方
   答 `NotCaptured`。

| 文件 | 是什么 |
| --- | --- |
| `core/Readback.hpp` / `.cpp`（新） | 分类表（`readbackOf` + `refusalName`）、两张格式表（`colorReadbackOf` / `depthReadbackOf`）、解码（`decodeDepth`）；文件注记写明"为什么是一张表"、"为什么没有 Empty"、优先级 |
| `api/OffscreenTarget.hpp` / `.cpp` | `Attachments` 加 `captured` / `depth_captured`（`capture(i)` / `captureDepth()` 置位）；`readbackResult(request)`（新，分类口径）；`probe(i)` / `depthProbe()` 先过表、再按格式表读；float 颜色格式**不建**回读缓冲；`core::decodeDepth` 取代 `api` 里的 switch；`create` 的零尺寸规则写进文档 |
| `tests/test_vsg/BackendCoreTest.cpp` | +4 无设备用例（`CoreReadbackTest`）：分类与优先级（含"未捕获"可重试 vs 格式永久）、格式表、解码（D16 ÷65535 的**非精确** 0.5 = 0.5000076、D32 原样、D24/半 texel ⇒ 空） |
| `tests/test_vsg/OffscreenTargetTest.cpp` | `recordOneFrame` 帮手可带深度回读 + 空安全挂节点；+3 真设备用例：任何帧之前 `probe()`/`depthProbe()` 答 `NotCaptured`（而不是返回分配内存）、D16 按 65535 缩放（0.5 清屏 ⇒ 探针 0.5）/D24 拒绝而颜色半边照常、16F 颜色拒绝而 pass 照跑（深度探针 0.8 是证据） |
| `tests/test_vsg/SharedDepthTest.cpp` | 夹具把**借用方**的深度拷贝也录进帧；+断言：借用方读到 0.5（出借方的）/0.1（自己的）/0.0（清屏），以及"两个目标各读各的缓冲"（出借方捕获 ⇒ 借用方仍 `NotCaptured`） |
| `tests/test_vsg/CMakeLists.txt` | 新 core 源文件登记（`GLOB` 只管插件本身） |

| 规则 | 结论 |
| --- | --- |
| **分类先于任何工作** | 读回被拒时**不看设备、不拷贝**：`readbackResult` 是纯事实函数（无设备用例 0 ms 跑完） |
| **永久 vs 暂时** | 变异 P5（把 `NotCaptured` 排在格式之前）⇒ 无设备用例红：一个跑多少帧都读不出来的目标会被告成"再试一次" |
| **簿记是"交出录制"** | 变异 P1（忽略 `captured`）⇒ 无设备用例 + `AProbeBeforeAnyCapture…` 红（未捕获的探针会变成"有效"） |
| **簿记属于集** | 换尺寸复用同一标志：新集里 `captured=false`（M5e 的用例已证明 `written` 同理） |
| **格式诚实是 core 的事** | 变异 P3（D16 按 4B 读）⇒ 设备用例红（解码把 2 字节数据当 float）；P4（D16 不除 65535）⇒ 无设备 + 设备用例都红（0.5 变成 ~32768） |
| **拒绝格式 ≠ 不渲染** | 变异 P2（忽略"可读"检查）⇒ `Buffer::create(0)` ⇒ `VUID-VkBufferCreateInfo-size-00912` + **段错误**（不是静默） |
| **借用方读的是共享图像** | 变异 P8（拷贝的 `srcImage` 换成"自己的图像"，借用方为 null）⇒ **段错误**（不是静默）；变异 P7（借用方盗用出借方的捕获标志）⇒ 断言红 |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| P1：忽略 `captured` | 3 条红（2 无设备 + 1 真设备） |
| P2：忽略颜色可读性 | 1 条红（16F 用例：0 字节缓冲的 VUID + 段错误） |
| P3：D16 按 4 字节/texel | 2 条红（格式表 + 设备探针） |
| P4：D16 不解码 | 3 条红（解码 + 设备探针） |
| P5：优先级颠倒 | 1 条红（无设备） |
| P6：忽略颜色格式 | 2 条红（无设备） |
| P7：借用方盗用出借方的捕获标志 | 2 条红（真设备） |
| P8：深度拷贝换源图像 | 1 条红（真设备段错误） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **559 用例 / 88 套件全绿**（+7 用例 / +1 套件） |
| 门禁 | 插件与全仓 `ninja` 零 error；强制验证层 **0 VUID**；加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / 816 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **float 颜色的读回只是"诚实拒绝"**：真要读 16F/32F，需要一个 float 探针类型（`PixelProbe` 的契约就是紧凑
  RGBA8）与它的解码/断言，那是新类型，而不是把这张表放宽——放宽只会让"看起来像图片"的东西替真实数据回答。
* **`ReadbackResult` 只做分类**：像素仍在 `probe()` / `depthProbe()`；目标**没有诊断出口**（sink 在会话/执行者
  手里），要报文由拿到分类的那一层报。
* **窗口读回不走这张表**：宿主表面的字节（字节序归一、xcb 取图）是平台层的事（M4b/M4c 一带），这张表管的是
  离屏附件。
* **相位形式**：`runPixelReadbackPhase` 是旧 selftest 的名字；重写版的证据是这套 gtest 对（core 表 + 真设备），
  相位表形态留给 M7。
* 前一版（§11.16x）留下的三条口子不变（`resize` 的执行者、`attachments_invalidated` 的生产者、租约的"重建
  借用方"）。

### 11.16z M7（2026-09-22）：证据加固（重写版的第一张真相位表 + 门禁脚本 + 稳态帧的门）

M7 的设计口径是"基线冻结、性能档、无 VUID 门禁、诊断字段完整性"。这一片落地能在重写版上**诚实成立**的那两样
（诊断字段完整性从 M0 起就有 `check_diagnostic_formats.py` 在跑，覆盖 39 个文件），并把"手跑的仪式"变成机器。

1. **重写版的第一张真相位表**（`BackendEvidenceTest`）：`PhaseTable` 的行第一次真的带上 `sample` / `expect`
   ——那张表存在的理由就是"这一相位**不许**抬高某个计数器"，而此前只有格式与空表的用例。三行：
   * `two steady frames allocate nothing`：**两个连续的稳态帧各开一次 `AllocationGate` 窗口**，plan 路径
     （recorder + compiler）在暖机后不再向堆要一字节，arena 也不加块。判据是"两帧都 0"而不是"一帧 0"：
     一帧不分配是那一帧的事实，两帧连着不分配才是规则。
   * `counters move by the frame's own shape`：`draws` 恰好 +2（一个 pass、两次绘制调用）。
   * `a cycle is skipped and counted`：两个互相读对方目标的 pass ⇒ 整个分量被跳过（`passes.empty()`）**且**
     `invalid_schedules` 恰好 +1。
   写这条用例时撞到的两件实测（都写进了注释，因为它们都能让相位**假绿**）：
   ① `beginFrame` 由协议状态机把关，`endFrame` 之后必须 `swapBuffers()` 才走完一帧的契约——忘了它，下一个
   `beginFrame` 直接被拒（相位红得很吵，比静默好）；
   ② **既没有绘制也没有清屏的 pass 根本不是 pass**（plan 会把它丢掉），第一版的环形 pass 因此"确实被跳过了、
   可计数是 0"——`passes.empty()` 为真，行看起来通过了，实际什么都没测；把两个 pass 都改成真 pass（各带一次
   绘制）之后，这一行才真正钉住 `invalid_schedules`。
   行文本本身冻结成基线（`[selftest] <name>` + 收尾 `[selftest] done`，与旧自检同一格式）：**相位名单就是能力
   名单**，改名或消失是一次 diff，而不是一条沉默的缺口。
2. **门禁脚本**（`scripts/vsg_rewrite_gate.sh`）：把此前每一片都要手跑的那套仪式变成一条命令与一个退出码——
   build → 套件（含真设备用例，**跳过即失败**，除非显式 `VINE_GATE_ALLOW_SKIPS=1`）→ 强制验证层
   （`VUID` / `Validation Error` 计数必须为 0）→ 同步验证（`SYNC-HAZARD` 必须为 0，`--quick` 可跳过）→ 三个
   hygiene 脚本 → `[selftest]` 行必须以 `[selftest] done` 收尾，最后打一张阶段表。
   **为什么必须是脚本**：套件全绿的同时验证层可以一直在报 VUID——"测试过了"根本不是这个后端要下的结论；而手跑
   的仪式里，最后一个阶段总是最容易先被跳过。
   门禁**自己也能失败**（三条实测，见下表）：伪造的 VUID、伪造的 `[  SKIPPED  ]`、没有 `[selftest] done` 的
   相位输出，分别让对应的阶段红。"不允许它失败"的门禁等于没有门禁——这条规矩在 M0 的分配门禁上已经立过一次。
3. **实测记录**（写相位时用二分窗口量出来的）：plan 路径的存储在**头两帧**里有界地长一次（第二次 `compile`
   分配 32 字节），第三帧起连续多帧都是 0。所以用例先暖三帧、再门两帧——"稳态"的定义正是"暖机之后"；若哪天真
   出现**每帧**分配，这条门会红。

| 文件 | 是什么 |
| --- | --- |
| `tests/test_vsg/BackendEvidenceTest.cpp` | +1 用例：重写版第一张真相位表（三行、带 `sample`/`expect`）+ `[selftest]` 基线冻结；文件注记里的"later phases add ASSERTIONS"有了第一份实例 |
| `scripts/vsg_rewrite_gate.sh`（新） | 一条命令的完整门禁：build / 套件（跳过即失败）/ 验证层 0 VUID / 同步验证 0 SYNC-HAZARD / 三个 hygiene 脚本 / `[selftest]` 收尾；输出一张阶段表，退出码即结论 |

| 规则 | 结论 |
| --- | --- |
| **"测试过了"不是结论** | 套件与验证层是两件事：脚本读输出里的 `VUID`/`Validation Error`/`SYNC-HAZARD` 计数，才让"0 VUID"成为可失败的断言 |
| **跳过不是证据** | 没有设备时所有真设备用例都会 SKIP；脚本把"有跳过"当失败（除非显式放行），否则"全绿"可以只是"什么都没跑" |
| **相位名单 = 能力名单** | 行文本冻结成基线：相位改名/消失 ⇒ 用例红（`report.lines` 与基线不等），不是沉默缺口 |
| **计数器要真的动** | 第 2/3 行分别把 `draws`、`invalid_schedules` 的增量写成期望；表在 `run` 前后各读一次计数器，行本身不自己断言数字 |
| **假绿要能被自己抓到** | 实测的两处（忘了 `swapBuffers()`、空 pass 不是 pass）都是"行看起来通过了却什么都没测"的形态，注释里点名 |

| 门禁自证（实测） | 结果 |
| --- | --- |
| 伪造一个打印 VUID 的测试二进制 | `suite (validation)` 红：`vuid=1` 并打印那条 VUID；退出码 1 |
| 伪造一个含 `[  SKIPPED  ]` 的输出 | 红：`skipped=1`，点名"set VINE_GATE_ALLOW_SKIPS=1 to accept skips" |
| 伪造一个没有 `[selftest] done` 的相位输出 | 红：`phase lines ... the phase run did not close with [selftest] done` |
| 真跑（lavapipe + 两个验证层） | 全阶段绿：`cases=560 failed=0 vuid=0 hazard=0 skipped=0`，4 行相位以 `[selftest] done` 收尾 |

| 相位表的变异反证（全部实测） | 结果 |
| --- | --- |
| P1 被门的帧故意分配 4 KiB | `two steady frames allocate nothing FAILED`（堆门禁真的会响） |
| P2 帧里画三次而期望是 +2 | `counters move by the frame's own shape FAILED: counter expectation not met` |
| P3 环形 pass 又变回空 pass（本片踩过的"假绿"形态） | `a cycle is skipped and counted FAILED`（`run` 会通过，计数器期望把它拦下） |
| P4 把一行相位改名 | 基线断言红：`report.lines != baseline`（"相位名单 = 能力名单"） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **560 用例 / 88 套件全绿**（+1 用例）；相位用例 `--gtest_repeat=20` 连跑 20 次全绿（堆窗口测量不是抽奖） |
| 门禁 | `scripts/vsg_rewrite_gate.sh` 一条命令全绿（build + 两个验证层 + 三个脚本 + 相位收尾） |

**本片留下的口子（登记，不假装解决）**：

* **相位表目前只覆盖 plan 半边**（recorder / compiler / counters / arena）。真设备侧的相位仍以 gtest 用例的形态
  存在：把设备用例也搬进 `PhaseTable` 需要"相位运行器"那一层（相位 = 装配 + 断言 + 计数器增量 + 像素读回），
  而装配逻辑现在分散在各用例里。本片先落地**格式、基线与门禁**，形态统一留给相位运行器那一片。
* **性能档（构建档案 / GPU profile）没做**：旧实现有 `VsgGpuProfile` + `GpuProfileTest`，重写版只有
  `Session::frameSeconds()` 这一格；profile 出口要等执行者把 pass 命名接上去（`WindowTarget` 的图形命名那套
  已经在旧实现里，重写版还没有）。
* 之前的口子不变（`resize` 的执行者、`attachments_invalidated` 的生产者、租约的"重建借用方"、float 颜色读回、
  相位运行器）。

### 11.16aa 执行者收口（2026-09-22）：目标自己说事实、丢帧有生产者、执行者循环落成用例

§11.16x 登记的三条口子里，有两条是"语义已经写好、但没有生产者"：`attachments_invalidated` 没有生产者、
`resize` 没有执行者。这一片把**事实的产出与消费**接成一条可跑的循环，并把"谁报告丢帧"留给会话那条路（见口子）。

1. **`OffscreenTarget::instance()`**（新）：目标自己产出 `core::TargetInstance` —— `desc` 来自它自己的尺寸与形状
   （含设备格式，兼容性那一半照旧在 `shape()` 里）、`generation`、`built`、`attachments_invalidated`。这里
   有一条必须说清的语义：**`built` 不是"图像存在"，而是"能 LOAD"** —— 一个刚建好、从没写过的目标如果报
   `built = true`，计划就会答 `None`，第一趟 pass 于是去 LOAD 一张 UNDEFINED 的图；所以 `built = written`
   （M5a 起 `written` 就是"有人往里录过东西"的那个事实）。设计文里 `TargetInstance::built` 的旧措辞
   （"Attachments exist for desc"）按这份实现更正为"**loadable**"。
   收益立刻可见：**15 处手拼的 facts 换成 `instance()`**（`ContentPassTest` 3、`SampledInputTest` 6、
   `ShadowBlockTest` 2、`LightBlockTest` 2、`WindowCompositionTest` 2），而这些用例里有 8 条真设备像素断言
   ——如果 `instance()` 与它们原来的手拼说法不一致，那些像素会当场红。一处拼写、一处事实。
2. **`invalidateAttachments()`**（新）：一份"提交失败 / 设备丢失 ⇒ 里面现在是什么没人知道"的事实。它**不是拆
   除**：图像还在、还是合法的 LOAD 对象，变的只是"内容可信吗"这个事实。计划的反应是
   `Repair(Bootstrap)`（`planTarget` 第 2 条），于是**下一帧的第一个写者清屏**而不是 LOAD。
   谁是"生产者"：**报告丢帧的调用方**（目标不观察提交），也就是会话/执行者的那条路（本片只提供缝，见口子）。
3. **修复这个事实的，恰好是"清屏的那一趟"**：`passGraph(..., bootstrap = true, ...)` 在交出图的同时把标志清掉。
   语义上必须如此——**只有清屏能把"内容未知"变回"内容已知"**，一次 LOAD 不能声称它修好了什么；所以
   `bootstrap = false` 的调用**不许**清标志（用例钉住：忽略计划的调用者清不掉它）。
   同一条约定也用在 `resize` 的替换臂上：新图像换掉了被丢的那批，标志随旧集合一起消失（`written = false`
   已经让计划说 `ResizeInPlace` ⇒ 首写者清屏）。
4. **执行者的循环落成一条用例**（`ALostSubmissionIsRepairedByTheNextFrameAndOnlyOnce`）：facts 从
   `instance()` 拿 → `FrameCompiler` 给决定与 `pass.bootstrap` → 用**计划给的那个 flag**建图 → 提交 → 读像素。
   像素是判据：帧 1（目标从没写过）计划必须答 `Repair(Bootstrap)`，画面是清屏色；丢帧后帧 2 **不宣布清屏**
   而计划仍要求 bootstrap ⇒ 画面是**这一帧自己给的颜色**（说明真的清了）；帧 3 不再清（画面停在帧 2 的颜色，
   "只修一次"由此可判）；最后 `invalidate` + `resize` 后 `attachments_invalidated` 为假、`generation` 前进。

| 文件 | 是什么 |
| --- | --- |
| `api/OffscreenTarget.hpp` / `.cpp` | `instance()`（文档写明 `built` = loadable 的理由）、`invalidateAttachments()`（文档写明它不是拆除、谁是生产者、以及为什么只有 bootstrap 能修）；`passGraph` 在 bootstrap 时清标志（**只**在 bootstrap）；`resize` 替换集合时清标志 |
| `tests/test_vsg/OffscreenTargetTest.cpp` | +1 真设备用例：丢帧 → 下一帧清屏 → 只修一次 → resize 换集合；中间还钉住"忽略计划的 LOAD 不许修事实" |
| 5 个既有设备测试文件 | 15 处手拼 `current` 换成 `target->instance()`（像素断言是这次替换的判据） |

| 规则 | 结论 |
| --- | --- |
| **`built` = "能 LOAD"** | 变异 P3（改成"图像存在"）⇒ 帧 1 的计划变 `None` ⇒ 首写者不再清屏 ⇒ 断言与像素都红 |
| **丢帧必须被记录** | 变异 P1（`invalidateAttachments()` 空实现）⇒ 帧 2 的计划变 `None` ⇒ 画面停在红（旧内容）而不是新颜色 |
| **只有清屏能修** | 变异 P5（任何 pass 都清标志）⇒ "忽略计划的 LOAD 不许修事实"那条断言红；变异 P2（bootstrap 也不清）⇒ 帧 3 又清了一次 ⇒"只修一次"红 |
| **换集合就换事实** | 变异 P4（resize 保留标志）⇒ `attachments_invalidated` 断言红 |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| P1 `invalidateAttachments()` 不记录 | 2 条红（事实断言 + 像素） |
| P2 bootstrap 不清标志 | 2 条红（"只修一次"的两条） |
| P3 `built` = 图像存在 | 2 条红（帧 1 的决定 + 像素） |
| P4 resize 保留标志 | 1 条红 |
| P5 任何 pass 都修（不只清屏） | 2 条红（"只有清屏能修"及其后续） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **561 用例 / 88 套件全绿**（+1 用例；15 处 facts 改口径零测试改动） |
| 门禁 | `scripts/vsg_rewrite_gate.sh` 一条命令全绿：`cases=561 failed=0 vuid=0 hazard=0 skipped=0` + hygiene 三个脚本 + 相位收尾 |

**本片留下的口子（登记，不假装解决）**：

* **"谁报告丢帧"还没接**：`invalidateAttachments()` 有缝、计划与用例有完整语义，但**会话的提交路径**还不知道
  自己这一帧都碰过哪些目标（`Session::commitFrame` 只提交 viewer 的图，目标表在将来的适配层里）。接法已经明确：
  提交失败时（`vsg` 抛错 / 丢失设备）由持有目标表的那一层对本帧的目标调用 `invalidateAttachments()`。
* **`Rebuild` 臂仍没有执行者**：形状真的变了（换格式/换附件数）时 `resize` 不做——那是"销毁并重建这个目标"，
  归会话的目标表；本片只把 `ResizeInPlace`/`Repair` 两条路走通。
* **租约的"重建借用者"仍没有 API**（§11.16x 口子不变）：调用方现在只能先析构再 `create`。
* M7 剩下的两样（**设备侧相位运行器**、**GPU profile 性能档**）不变。

### 11.16ab M7 第二半（2026-09-22）：设备侧相位运行器（能力只有一个写法、两个读出口）

§11.16z 留下的第一张口子就是"设备侧相位还是散用例"。这一片把**设备能力**搬进 `PhaseTable` 的形态，但
不新增第二份断言：**相位体就是用例体**。

1. **身体是函数，读出口有两个**（`tests/test_vsg/DevicePhases.hpp`）：`runOffscreenReadbackPhase` /
   `runSharedDepthPhase` / `runTargetResizePhase` / `runLostSubmissionPhase` 各自是一个函数，**既**被细节
   用例调用（`TEST(...) { run…Phase(device, counters); }` 这样的薄壳），**也**被相位表的一行调用。一个能力
   长出一条相位没跑的断言、或一条相位行没有身体，在这种形态下**不可能**发生——这正是"两处各写一遍"会烂掉
   的地方。断言仍是 gtest 的（红的时候给文件与行号），而**计数器**是行真正门住的东西：帧数、建了几个目标、
   换了几次尺寸、停车几个、idle 几次——`DevicePhaseCounters` 记的是"这一相位**驱动**了什么"。
2. **四行各自门一个数**（`DevicePhaseTest`）：`targets_built +1`（离屏读回）、`+2`（共享深度：出借方与借用
   方）、`resizes_replaced +1`（换尺寸）、`frames +3`（丢帧修复：新帧 / 修复帧 / 稳态帧）。只写"它跑过了"
   的行会在能力**什么都没做**时照样通过——计数列就是为此存在的（变异 P2 实测：相位体直接 return ⇒ 行以
   "counter expectation not met" 红）。
3. **一次运行一个设备**：四行在同一个设备上依次跑（`Stack::build(device)` 现在收调用方的设备，不再自建），
   于是这次运行只花一个设备而不是四个；细节用例照旧各自带一个（共享深度那条还要**验证层开着**，它测的是布局）。
4. **`test_vsg` 现在是两次相位运行**：plan 半边（`BackendEvidenceTest`，3 行）+ 设备半边（`DevicePhaseTest`，
   4 行），门禁输出 9 行 `[selftest]`、2 个 `done`。**门禁的判据随之改对**：原来要求"最后一行是
   `[selftest] done`"——两张表之后，**前面一张表红、后面一张表干净收尾**就会骗过它（实测：伪造输出
   `FAILED` 行 + 随后的干净表，旧判据 PASS）。现在判据是"**没有任何 `FAILED` 行** + 每次开始运行都收尾"，
   伪造输入实测红。

| 文件 | 是什么 |
| --- | --- |
| `tests/test_vsg/DevicePhases.hpp`（新） | 四个相位体的声明 + `DevicePhaseCounters`（帧 / 目标 / 换尺寸 / 停车 / idle）+ 文件注记（为什么是函数、记的是什么、跳过是谁的事） |
| `tests/test_vsg/OffscreenTargetTest.cpp` | 三条用例（离屏读回、换尺寸、丢帧修复）改成薄壳 + 相位体；12 条用例仍全绿（像素与断言一字未改） |
| `tests/test_vsg/SharedDepthTest.cpp` | `Stack::build(device)` 收设备；共享深度用例同上；2 条用例仍全绿 |
| `tests/test_vsg/DevicePhaseTest.cpp`（新） | 四行相位表（每行一个计数器期望）+ 行文本基线 + 总数断言（7 帧 / 5 目标 / 1 次换尺寸 / 1 次停车） |
| `scripts/vsg_rewrite_gate.sh` | 相位行判据改为"无 FAILED 行 + 有收尾"，并报告"几次运行" |

| 规则 | 结论 |
| --- | --- |
| **能力只有一个写法** | 变异 P1（把离屏读回的期望颜色改掉）⇒ **细节用例与相位行同时红**（同一个身体的两个读出口） |
| **"跑过了"不是证据** | 变异 P2（共享深度相位体直接 return）⇒ 用例壳的计数器断言红 + 行以 "counter expectation not met" 红 |
| **两张表都要能被判** | 伪造"非末表 FAILED + 末表干净收尾"的测试输出 ⇒ 门禁红（旧判据会放行） |

| 变异反证 / 门禁自证（全部实测） | 结果 |
| --- | --- |
| P1 相位体的期望颜色改掉 | 2 处红（细节用例 + 相位行） |
| P2 相位体空转 | 2 处红（用例计数器 + 相位行计数器） |
| 伪造非末表 FAILED 的输出 | 门禁 `phase lines` 红：`1 phase(s) reported FAILED` |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **562 用例 / 88 套件全绿**（+1 用例：相位运行；四条被接管的用例行为不变） |
| 门禁 | `scripts/vsg_rewrite_gate.sh` 全绿：`cases=562 failed=0 vuid=0 hazard=0 skipped=0`、hygiene 0 / 818、相位 **9 行 / 2 次运行全收尾** |

**本片留下的口子（登记，不假装解决）**：

* **设备能力只搬了四个**：离屏读回、共享深度、目标换尺寸、丢帧修复。其余（MRT、全屏合成、前后向阴影像素、
  窗口合成、内容稳态……）仍是散用例；形态已经就位（`DevicePhases.hpp` + 一行 `Phase`），搬过去是机械工作，
  但每搬一个都要**保留它自己的细节用例**（薄壳化），不能只留相位行。
* **性能档（构建档案 / GPU profile）仍未做——这是 M7 的最后一样**。旧实现（`vine/vsg/VsgGpuProfile`）的规则
  要照抄语义而不是代码：①每个 render pass 一个采样（离屏 pass + 窗口图，窗口图给的是"呈现路径"整体）；
  ②采样要有**身份**——上游 `RenderGraph::record` 的时间戳是空对象，所以旧实现给每个 pass 的图包一层
  `vsg::InstrumentationNode` 并把图自己当对象传；③读结果**不带 `VK_QUERY_RESULT_WAIT_BIT`**，读到的总是
  几帧前的数据，用 `age_frames` 说清"这是哪一帧的"，因为"读到 0 帧延迟"意味着阻塞读，而阻塞读会抬高计数过的
  device wait。重写版要做的是：执行者按开关包那层 + 名字从编译好的帧里取（目标/顺序/"window"）+ 会话暴露
  `age_frames`。这是独立一片（要动执行者与会话，且需要自己的像素/计数器判据）。
* 之前的口子不变（调用 `invalidateAttachments()` 的提交失败缝、`Rebuild` 臂、租约"重建借用者"、float 颜色读回）。

### 11.16ac M7 第三半（2026-09-22）：性能档的"身份"半边（每个 pass 都记成可归属的区间）

M7 的最后一样是性能档（GPU profile）。它有两个半边：**采样有没有身份**（执行侧的包装）与**读数字**（会话侧的
profiler 安装 + 不阻塞的读取）。这一片把第一半做完，并把第二半的确切语义写进下面的口子。

1. **开关默认关，关就是"什么都没有"**（`VsgExecutor::setProfiling`）：关着时命令图里就是各 pass 的
   `RenderGraph` 本身——没有包装节点、没有名字、没有条目。这是可被变异证明的断言（变异"永远包装"⇒
   "关着不加东西"那条用例红）。
2. **开着时每个 pass 经过一个带名字的 `vsg::InstrumentationNode` 记录，而归属靠 GRAPH 而不是名字**：
   上游 `RenderGraph::record` 自己写的每图时间戳带的是**空对象**，捕获工具拿到的区间无法归属到任何 pass；
   包装节点把**图自己**当作对象传给 instrumentation（`InstrumentationNode::traverse(RecordTraversal&)`
   传的是 `child.get()`），于是执行器手里"图 → 是哪个 pass"的账本（`ProfileEntry` + `profileOf(graph)`）就是
   读数字那一半需要的全部映射——不需要再建第二张目标表。名字（`"target<i>@<schedule>"`、窗口是
   `"window@<schedule>"`）只给人看（捕获里、vsg 的报告里）；**读者按图匹配**，这正是旧实现文件里写下的同一条
   约定。
3. **窗口图只包装一次**（它被加进命令图的那一刻，即本帧第一个窗口 pass）：它的区间覆盖整张图的记录遍历，
   也就是**本帧所有窗口 pass 的内容** ⇒ 窗口的采样是"呈现路径整体"，不是某一路视口——与旧实现的说明一致。
4. **条目属于帧**：`record()` 开头清空，条目里记的是**这一帧**的图；留着上一帧的会把这一帧的区间归到上一帧的
   pass 上（变异 P4 实测红）。名字里的序号用 `target_index`（计划给的）与 `schedule_index`——**索引式**，等
   适配层能给 SDK 句柄起名时再换成人名；这不是损失，因为归属不靠名字。

| 文件 | 是什么 |
| --- | --- |
| `api/VsgExecutor.hpp` / `.cpp` | `setProfiling` / `profiling`；`recordedChild()`（包装 + 记条目）；`ProfileEntry{graph, pass, schedule, window, name}`；`profileEntries()` / `profileOf(graph)`；窗口图在加入处包装 |
| `tests/test_vsg/ExecutorTest.cpp` | +2 真设备用例：**关**着时命令图里没有任何包装节点、条目为空（且帧照常渲染）；**开**着时每个 pass 都被包装（`wrapper->child == 图`）、名字合规则、`profileOf` 往返、像素不变、**第二帧替换条目**（不是两帧的并集） |

| 规则 | 结论 |
| --- | --- |
| **关着不加东西** | 变异"永远包装"（守卫恒假）⇒ "关"用例红（找到 1 个包装节点） |
| **开着必须包装并可归属** | 变异"从不包装"（守卫恒真）⇒ "开"用例红（没有包装、没有条目） |
| **身份是图，不是名字** | 变异 P5（包装一个空 `Group` 而不是那张图）⇒ 归属断言红 + 像素红（pass 根本没记录） |
| **名字带序号** | 变异 P3（名字用 pass 号代替 schedule）⇒ 名字断言红 |
| **条目属于本帧** | 变异 P4（不清空条目）⇒ 第二帧的条目数断言红 |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| 守卫恒假（永远包装） | `ProfilingOffAdds…` 红（1 个包装节点） |
| 守卫恒真（从不包装） | `ProfilingWraps…` 红 |
| 名字丢序号 | `ProfilingWraps…` 红 |
| 条目不清空 | `ProfilingWraps…` 红（第二帧 2 条） |
| 包装空 Group（丢掉图） | `ProfilingWraps…` 红（归属 + 像素） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **564 用例 / 88 套件全绿**（+2 用例） |
| 门禁 | `scripts/vsg_rewrite_gate.sh` 全绿：0 VUID / 0 SYNC-HAZARD、hygiene 全清、相位 9 行 / 2 次运行全收尾 |

**本片留下的口子（登记，不假装解决）——M7 的最后一块**：

* **读数字那一半还没接**（会话侧，独立一片）：①按 `VINE_VSG_PROFILE`（沿用旧实现的环境变量名，另加
  `VINE_VSG_PROFILE_CPU` / `_GPU` 设定 `vsg::Profiler::Settings` 的两个 level）在会话的 viewer 上安装
  `vsg::Profiler`；②读取**不带 `VK_QUERY_RESULT_WAIT_BIT`**（vsg 自己就是这么读的：没就绪的查询下次再读），
  因此读到的总是几帧前的数据 ⇒ 必须给出 `age_frames`（这一帧与数据所属帧的差），并保证**读取本身不抬高
  `deviceWaits()`**（这是本后端"帧路径不停设备"的判据，profile 不许例外）；③归属用本片的
  `profileOf(interval.object)`，**不需要**新的目标表；④数字只做"非负 + 相位有采样"这类断言，时间不是像素，
  不假装能断言具体数值。
* **名字仍是索引式**（`target<i>@<schedule>`）：等适配层持有 SDK 句柄时，把 `RenderTarget::name()`（或等价物）
  接进 `CompiledTarget` 即可换成人名；读者不受影响（按图归属）。
* 之前的口子不变（提交失败缝、`Rebuild` 臂、租约"重建借用者"、float 颜色读回等）。

### 11.17 下一步

| 项 | 内容 |
| --- | --- |
| ~~M0~~ | **已完成（2026-09-21）**：core 骨架 + 证据夹具 + 分配门禁 + 诊断出口 |
| ~~M1a~~ | **已完成（2026-09-21）**：设备地板策略（core）+ 物理设备探测（api） |
| ~~M1b~~ | **已完成（2026-09-21）**：自有窗口会话、空帧提交与呈现、槽数逐帧学习、按证据退役 |
| ~~M1c~~ | **已完成（2026-09-21）**：宿主句柄采纳、会话移动（`Keep` / `Move` / 失败退回 `Rebuild`）、句柄代次 |
| ~~M2a~~ | **已完成（2026-09-21）**：流身份与编辑计划（`planGeometry`）、几何别名登记、材质 arena、每帧 ring |
| ~~M2b~~ | **已完成（2026-09-21）**：变体池（按身份、有界、id 不复用）+ 每 pass 状态注册表（动态层只花 set 命令） |
| ~~M2c-1~~ | **已完成（2026-09-21）**：共享上传（`api/StreamUploads`：canonical 通道 + 索引键归一化；派生/自定义通道响亮拒绝） |
| ~~M2c-2a~~ | **已完成（2026-09-21）**：每帧块存储（`api/BlockStorage`：一缓冲三区域、slab 旋转、材质稳态零写、拒绝不增长），真设备用例 |
| ~~M2c-2b-1~~ | **已完成（2026-09-21）**：块描述符（`api/BlockDescriptors`：一个 set + 三个 dynamic offset、对齐即拒绝、repoint 留布局），真设备用例 |
| ~~M2c-2b-2a~~ | **已完成（2026-09-21）**：内容管线与着色器（一身份一对象、动态半声明含 viewport/scissor、编译失败即拒绝），无设备用例 |
| ~~M2c-2b-2b-1~~ | **已完成（2026-09-21）**：绘制录制（`api/ContentDraw` + `api/StateCommands`：两个“仅当”、动态槽、引擎约定），无设备用例 |
| ~~M2c-2b-2b-2a~~ | **已完成（2026-09-21）**：无窗口设备缝（`api/Device` + `api/DeviceFeatures`）+ 离屏目标与读回（`api/OffscreenTarget`：CLEAR、TRANSFER_SRC【彩色收尾见 §11.16n】、capture 节点、`probe()`），真设备无窗口用例 |
| ~~M2c-2b-2b-2b~~ | **已完成（2026-09-21）**：整条栈画三角形到离屏目标并断言像素（三重验证齐） |
| ~~M2c-3~~ | **已完成（2026-09-21）**：内容经 `Session` 进帧（内容根 + 设备 + recompile；窗口像素证据） |
| ~~M3a~~ | **已完成（2026-09-21）**：清屏/加载策略（`core/ClearPlan`：bootstrap 全清、额外附件透明黑、保留的深度永不清、反转 Z 远平面 0.0），无设备用例 |
| ~~M3b~~ | **已完成（2026-09-21）**：多附件目标与逐附件读回（`api/OffscreenTarget::TargetLayout`：MRT + 深度、`capture(i)`/`probe(i)`），真设备像素用例（双输出着色器分色） |
| ~~M3c~~ | **已完成（2026-09-21）**：借用深度落到目标层（不建镜像 / LOAD / 提升撤销 + 事实即计划），真设备用例（颜色 + 深度双重证据）；顺带补上设备地板的**扩展**一半与深度读回（`core/DepthProbe`） |
| ~~M3d-1~~ | **已完成（2026-09-21）**：帧的收集段（`core/FrameRecorder`：协议先行、arena 独占快照、scope 属性 vs 逐绘制消费、三种裁决 + 计数），14 个无设备用例 |
| ~~M3d-2~~ | **已完成（2026-09-21）**：帧的编译段（`core/FrameGraph`：稳定拓扑序 + 环的 SCC 跳过；`core/FrameCompiler`：normalize + target/depth 求解 + `CompiledFrame`），10 + 12 个无设备用例 |
| ~~M3d-3a~~ | **已完成（2026-09-21）**：执行段第一片（`api/VsgExecutor`：一个 pass scope = 一个 render pass，逐 pass 清屏值，记录顺序 = 计划顺序有像素证据），真设备用例 2 个 |
| ~~M3d-3b-0~~ | **已完成（2026-09-21）**：计划带 pass 侧管线事实（`color_attachments` / `depth_sampleable`）+ `OffscreenTarget::shape()` + 执行器核对计划与 target（§11.16f） |
| ~~M3d-3b-1~~ | **已完成（2026-09-21）**：内容按 pass 放置（`PassContent` 接缝 + 非法内容上报），内容层用 `ContentDraw` 等录的三角形经计划上屏，真设备像素证据（§11.16g） |
| ~~M3d-3b-2~~ | **已完成（2026-09-21）**：三张事实表的定义与查找语义（`api/ContentFacts`：身份 + revision 是键、三种 miss、通道↔布局与块↔ABI 两条规则），无设备用例（§11.16h） |
| ~~M3d-3b-3~~ | **已完成（2026-09-21）**：每绘制 ABI 块的打包（`api/DrawBlock`：列主序 + 平移在 12..14 + `params.x` = opacity），非对称矩阵用例 + 变异反证（§11.16i） |
| ~~M3d-3b-4~~ | **已完成（2026-09-21）**：几何的通道走查（`api/GeometryFacts`：通道顺序 = 绑定顺序、索引键归一化为整 buffer、切片上传自己的段、起点必须一致），无设备用例（§11.16j） |
| ~~M3d-3b-5~~ | **已完成（2026-09-21）**：程序与材质两张数据源（`api/ContentSources`：两段 + 一个入口点才算内容管线；无材质 = 身份为空的默认条目；块的成员才是载荷），无设备用例（§11.16k） |
| ~~M3d-3b-6~~ | **已完成（2026-09-21）**：内容层（`api/ContentPass`：逐命令按表录取、查不到就拒绝并上报；块 + 流 + 绘制；`findMaterial` 改为只按身份查），真设备用例（像素 + 缓存计数）（§11.16l） |
| ~~M3d-3b-7~~ | **已完成（2026-09-21）**：多布局 scope（`api/ContentPass`：半片 = 程序 × 修订 × 布局，池与注册表共享，选不中报“是哪一项”），真设备用例（双色像素 + 绑定计数 + 两条拒绝消息）（§11.16m） |
| ~~M3d-3b-8~~ | **已完成（2026-09-21）**：pass 输入的采样绑定（`core/FrameCompiler` 的输入表 + `api/ContentPipeline` 的采样集布局 + `api/ContentPass` 的绑定 + `api/OffscreenTarget` 的“彩色附件按可采样收尾”），真设备像素用例 + 四条变异反证（§11.16n） |
| ~~M3d-3c~~ | **已完成（2026-09-21）**：执行段第三片（`api/WindowTarget` 把窗口变成执行器认识的目标：一张图、一次清、一个稳定视图；`api/ViewBlock` 定下视图块的四处约定并把 SDK 裁剪空间折进设备约定；`api/Session` 的帧时钟 + `frameSeconds()` + 帧图接缝），真设备像素用例（计划清屏 + 视图块进着色 + 一次 present）+ 五条变异反证（一条登记为不可观测）（§11.16o） |
| ~~M4a~~ | **已完成（2026-09-21）**：全屏绘制调用的内容侧（键的 `kind` + `api/ContentPipeline::createScreen`（set 0 = 采样集、push 片元 128B）+ `api/ContentSources::buildScreenProgramFacts`（引擎顶点阶段 + 宿主片元阶段）+ `api/ContentDraw::recordScreen`（`Draw(3)`）），真设备像素用例（随 SDK 发布的 screen copy 的 PiP 拷贝 + binding i = 附件 i）+ 四条变异反证（§11.16p） |
| ~~M4b~~ | **已完成（2026-09-22）**：全屏绘制的计划侧（`CompiledDraw::dynamic` + 全屏调用的深度策略 = `Disabled`（正典三角形在 reverse-Z 远平面）；`api/ContentPass` 按 `kind` 找半片、采样集在 set 0、push 128B 片元（内容全零）；真设备像素用例（计划驱动的 PiP 拷贝）+ 两条变异反证（§11.16q） |
| ~~M4c~~ | **已完成（2026-09-22）**：窗口合成（`tests/test_vsg/WindowCompositionTest.cpp`：四趟 pass 一帧——离屏清屏 / 同形离屏场景 / 窗口场景（拥有那一次清）/ 窗口全屏覆盖层，真设备像素三条 + 计数器五条 + 执行器按计划顺序放置）；顺带把 §11.16o 的两条口子做成用例，其中"窗口/离屏同键变体"被证明是错的 ⇒ **设备格式进键的兼容性半边**（`core::TargetShape` / `core::RenderPassCompatibility` + `TargetShape::compatibility()` + 两个目标各自上报），并纠正 §11.16o 里"稳定视图分族"那条理由；修掉 capture 的跨帧写-写（声明顺序的屏障）+ 三条变异反证 + 测试宿主窗口去重（§11.16r） |
| ~~M5a~~ | **已完成（2026-09-22）**：多写者离屏目标（LOAD 变体）——`LoadOpVariantKey` 改成变体的名字 + `core::loadOpVariantOf` 装配；`OffscreenTarget` 按变体建/缓存渲染通道（依赖列表逐位相同、framebuffer 共享）+ `passVariantCount()` + `written()`；执行器把计划的 `clear`/`bootstrap`/`depth_preserved` 交下去；真设备像素用例（一帧两趟：清 + LOAD，两个网格都在）+ 四条变异反证（§11.16s） |
| ~~M5b~~ | **已完成（2026-09-22）**：输入表带上深度半边（"阴影脊柱"）——`CompiledInput::depth_sampleable` 由 `core::depthPlan` 解析（采样者与被采样者同一个答案）；`core::depthFinalLayout` 给"纯深度 + 可采样"第三种收尾布局（`ShaderReadOnly`），`loadOpVariantOf` 的深度起始布局改成 `depth_steady`（借用者命名的是出借者的布局）；`PipelineKey::sampled_depth_count` + `SampledKey{colors, depths}` + NEAREST 深度采样器；深度捕获屏障按 `depthSteadyLayout()` 进/出（原来写死的布局是 `VUID-VkImageMemoryBarrier-oldLayout-01197` 的谎报，还会让采样读到全 0）；真设备用例（探针 0.5/0.0 + 像素灰/黑/清屏色）+ 无设备用例两条 + 四条变异反证（§11.16t） |
| ~~M5c-1~~ | **已完成（2026-09-22）**：前向光照块——`api/LightBlock`（`VineLightsBlock` 112B + 世界→视图换算 + 归一化 + 三槽规则 + 补光不计数 + 无相机 ⇒ 空块）；`BlockStorage` 第 4 区域 + `writeLights`；`BlockDescriptors::kLightsBinding = 3` + 四个动态偏移；`ContentPass` 每绘制调用写一块 + "每 episode 一次"的丢弃报告（`ReportOnce`）；6 条无设备用例 + 1 条真设备四带像素用例（带 0 朝向观察者的太阳 / 带 1 反向 / 带 2-3 装不下的灯 = 补光 + 一条报告）+ 四条变异反证（其中 M1 实测"轴对齐相机看不出缺旋转"⇒ 判据是算术用例）（§11.16u） |
| ~~M5c-2~~ | **已完成（2026-09-22）**：阴影块——`ShadowFacts`（谁的 map + 生产者矩阵）进 `TargetFacts`/`CompiledInput`/`CompiledPass`，`FrameCompiler::resolveShadow` 三个事实先来先用；`LightRef` += 身份/投影开关/bias；`api/ShadowBlock::packShadowBlock`（身份匹配、启用/投影/类型/槽检查、`light_vp * inverse(view)` 列主序、`params` 四元组）；`BlockStorage` 第 5 区域 + `kShadowBinding = 4`；`directionalSlotOf`（与灯块打包同一次遍历）；**按实测删除 `PipelineKey::shadow_bound`**（本后端 ABI 里 map 走采样输入、块恒在块集、开关是运行期值 ⇒ 它只会白拆管线）；4 条无设备 + 1 条真设备四带像素用例 + 7 条变异反证（§11.16v） |
| ~~M5d~~ | **已完成（2026-09-22）**：全屏路径的 128B push——`LightPushBlock`（128B，`projparms` 保留为零）+ `packLightPushBlock`（复用 `packLightBlock` 的遍历、只换布局）；`recordScreenDraw` 按每次调用推；`createScreen` 建 push-only 布局（"只读 push"的全屏 pass 不再被拒）；2 条用例（无设备 + 真设备三视口像素）+ 4 条变异反证（其中"push 全零"与"发错阶段"分别是内容缺失与静默失败的实证）（§11.16w） |
| ~~M5e~~ | **已完成（2026-09-22）**：目标生命周期（计划驱动的换尺寸）——`OffscreenTarget::Attachments`（尺寸相关的一整批对象）+ `buildAttachments(width, height, out)`（纯构建、不写自身）；`create` 成功后才接手并计数借用者；`resize(w, h, timeline, retirement)` 按 `core::planTarget` 决定、**保留渲染通道与管线**、旧集经 `RetirementQueue` 停车（闸门关着时退回**计数过的** device idle）；租约两向拒绝；换后 `written=false` + `generation+1`；5 条真设备用例（含深度回读按新尺寸重建、引用计数证明"停着而不是扔了"）+ 6 条变异反证（§11.16x） |
| ~~M6~~ | **已完成（2026-09-22）**：读回——`core/Readback`（`readbackOf` 单表 + 两类格式表 + `decodeDepth`），优先级"存在性 → 格式（永久）→ 捕获（可重试）"，不设走不到的 `Empty`（零尺寸归 `planTarget` / `create` 拒绝）；`OffscreenTarget` 的 `captured` / `depth_captured` 簿记（属于附件集，换尺寸天然复位）+ `readbackResult()`；float 颜色附件不再建回读缓冲（原来必撞 `VUID-vkCmdCopyImageToBuffer-pRegions-00183`）；借用方的深度回读 = 共享图像 + 自己的缓冲；4 条无设备 + 4 条真设备用例 + 8 条变异反证（§11.16y） |
| ~~M7~~ | **已完成（2026-09-22，第一半：证据加固）**：重写版第一张**真**相位表（`PhaseTable` 的 `sample`/`expect` 第一次派上用场：稳态帧零分配 ×2 + `draws` +2 + 环跳过 `invalid_schedules` +1），`[selftest]` 行冻结成基线；`scripts/vsg_rewrite_gate.sh` 把整套仪式变成一条命令（跳过即失败、0 VUID / 0 SYNC-HAZARD 成为可失败断言、相位必须以 `[selftest] done` 收尾），门禁自己也被三条伪造输入证明能红；实测记录 plan 路径头两帧的有界增长（第二次 compile 32B，之后 0）（§11.16z）。| ~~M7c（设备侧相位运行器）~~ | **已完成（2026-09-22）**：`DevicePhases.hpp` 让四个设备能力**只有一个写法**（身体是函数：细节用例与相位行都调它）；`DevicePhaseTest` 用计数器门住每一行（目标 +1/+2、换尺寸 +1、帧 +3），行文本冻结；`Stack::build(device)` 改收调用方的设备（一次运行一个设备）；门禁的相位判据改成"无 FAILED 行 + 每次运行收尾"（两张表之后，"最后一行是 done"会被"前面红、后面干净"骗过，实测）；2 条变异 + 1 条伪造输入全红（§11.16ab）。| ~~M7d（性能档的身份半边）~~ | **已完成（2026-09-22）**：`VsgExecutor::setProfiling`（默认关，**关着不加任何东西**）+ `recordedChild()` 把每个 pass 记成**可归属的区间**（带名字的 `InstrumentationNode`，对象是**图自己** ⇒ `ProfileEntry` + `profileOf(graph)` 就是读数字那一半要的全部映射）；窗口图只包一次（区间 = 呈现路径整体）；条目属于帧；2 条真设备用例 + 5 条变异反证（§11.16ac）。~~M7e（会话侧读数字半边）~~ | **已完成（2026-09-22）**：`VINE_VSG_PROFILE` 初始化时读一次并
装 `vsg::Profiler`（关着不装）；`Session::gpuProfile()` 倒着走日志取最新有结果的一帧（`readable` /
`age_frames` / `frame_gpu_ms` / 按地址归属的 `passes`），读一次不等待、不让设备停（`deviceWaits()`
前后相等钉住）；1 条会话用例 + 5 条变异反证（§11.16ad）。|
| ~~M7b（执行者收口）~~ | **已完成（2026-09-22）**：目标自己说事实——`OffscreenTarget::instance()`（`built` = **能 LOAD** = `written`；15 处手拼 facts 统一到它，像素断言是判据）+ `invalidateAttachments()`（丢帧 ⇒ 计划 `Repair(Bootstrap)` ⇒ 下一帧首写者清屏；**只有 bootstrap 能修事实**，`resize` 换集合同时换事实）；+1 真设备用例把执行者循环（facts → 计划 → `pass.bootstrap` → 图 → 像素）跑通，含"只修一次"与 resize 两条判据；5 条变异反证（§11.16aa） |
| **M8 场景桥** | **开工**：把引擎的场景内容（程序 / 几何 / 材质 / 目标）接到重写版的内容层。已决的接法 = **按程序文本自己的声明服务**（§11.16ae）：文本是宿主的，后端选不了它的绑定号。 |
| ~~M8a（声明成为事实）~~ | **已完成（2026-09-22）**：`api/ProgramAbi`——`scanProgramAbi(vertex, fragment, defines, out)` 把文本的绑定声明读成事实（set/binding、种类、阶段、按**L1 类型名**认领的角色、std140 尺寸、push 范围），条件按变体求值（taken 分支的 `#error` ⇒ Malformed），引擎自己的八个程序逐条钉住（前向 5 绑定 + push 128B 顶点、带影延迟光照的 5/6、屏幕拷贝的 binding=N…）；11 条无设备用例 + 5 条变异反证（§11.16ae）。下一步 M8b：布局跟着声明装配。 |
| ~~M8b（布局就是声明）~~ | **已完成（2026-09-22）**：`ContentPipeline::create` 收 `ProgramAbi`——`describeAbi` 把声明读成布局（每个被声明碰到的 set 一条布局、空隙填真布局、块 → dynamic UBO 在**它自己写的** binding 上、push 照声明），填不了的按名拒绝（外来块 / 非 std140 / 超尺寸 / `count != 1` / 无视图的采样器种类 / 内容路径 set≠1 的采样器 / 块与输入挤一个 set），`acquire` 另拒“采样器超出本 pass 输入数”；`BlockDescriptors` 形状驱动（`canonicalShape` / `layoutOfShape` / `forAbi` / `blockShapeOf`，`bind()` 一个声明绑定一个偏移）；`ContentPass::Scope::block_sets` 一组块集 + `serveHalf` 按“set 序号 + 形状逐位相同”认领、按名拒绝、push 未填即拒；`Draw::blocks` 变 span。4 条新用例（真设备像素 1、真设备描述符 1、无设备 2）+ 6 条变异反证全红；门禁还揪出三处仍无条件绑块的旧夹具（程序一个块都没声明 ⇒ 布局 0 个 set），修夹具而不是加容忍（§11.16af）。 |
| ~~M8c-1（push 由 pass 自己填）~~ | **已完成（2026-09-22）**：`AbiPushRange` 带上成员表（名字/偏移/尺寸，扫描时就记）；新 `api/ContentPush`：`contentPushMemberOf` 认 `projection` / `modelView`，`packContentPush` 按**名字**填——`projection` = `foldToDeviceClip(camera.projection)`（与 `VineViewBlock.proj` 同一个值、同一处折叠），`modelView` = `view * model`（顺序就是契约：模型矩阵在右）；认不出的名字或尺寸不对（`vec4 projection`）在 `ContentPipeline::create` 就拒（前缀可以），无相机写零（同 `buildViewBlock`）；`ContentDraw::Draw` += `pushes`，每个可绘制对象一遍（`modelView` 带的是它自己的模型矩阵）。**实测的坑**：`vsg::PushConstants` 放进 `StateGroup` 的 stateCommands 会被 vsg 按 slot 记录、但**到不了 shader**（画出来是恒等矩阵）——必须放进 `Commands` 节点、按插入序紧贴绘制（全屏路径 M5d 一直就是这么做的）；变异 N1 把位置改回去 ⇒ 像素用例红。另：计划里有相机时，pass 的**深度清屏值要显式给**（reverse-Z 远 = 0.0），否则片元被拒（画面=清屏色、0 VUID、无拒绝）。5 条新用例（真设备像素 ×1、无设备 ×4）+ 5 条变异反证全红（§11.16ag）。 |
| ~~M8c-2a（声明的集合装块与图）~~ | **已完成（2026-09-22）**：`describeAbi` 的 set 形状带出采样器表（引擎的 set 0 同时装 material 块与 `diffuseMap`）；`ContentPass::Scope` 的块集与采样集按 **set 序号 + 形状逐位相同**认领；全屏与内容的采样集各按自己的 kind 建（§11.16ah）。 |
| ~~M8c-2b（没有贴图采样白）~~ | **已完成（2026-09-22）**：`api/WhiteImage`（值而不是错误：材料没有纹理时 `diffuseMap` 绑白图）+ `ContentPipeline` 的 `sampler_shapes` 只服务声明过的绑定；真设备像素用例（白 × 材质色）（§11.16ai）。 |
| ~~M8c-3（名字即来源）~~ | **已完成（2026-09-22）**：`api/ContentImages`（`ImageOrigin` + `imageOriginOf` + `samplesShadowMap` + `shadowImageOf`）；`shadow_map` 按名字到达程序声明在**自己集合里**的那个 binding；`skyMap` 在建层时按名字拒；"pass 解析出 map 而程序读不到"每半片上报一次；顺手修 `BlockStorage::writeMaterial` 的 HIT 返回 offset 0（§11.16aj）。 |
| ~~M8c-4（全屏集合即声明）~~ | **已完成（2026-09-22）**：`createScreen` 收 `ProgramAbi` + set≠0 / 非阴影块两条拒绝；`sampledSetLayout` 的全屏分支（声明号就是绑定号、深度槽跳过已点名的号、块 = 静态 UBO）；`recordScreenDraw` 按身份找源、按计划找 map、按名拒绝、每调用一个阴影块；引擎带影/无影延迟光照七趟真设备像素；顺手修**动态命令一直开着混合**（多附件 = 不混合，单附件恒开）；5 条变异反证全红（§11.16ak）。 |
| ~~M8d-1（材质贴图的 GPU 侧）~~ | **已完成（2026-09-22）**：`api/MaterialImages`（按地址 + 修订缓存；条目持住键；mip-major 交错 staging；元素 = 一个 texel、数组维数 = 层数；view type 写在 DATA 上；白 fallback 2D（复用 `WhiteImage`）+ cube；`releaseAbandoned`/`clear`/超限淘汰；`setMaxAnisotropy`）；`ContentPipeline` 放开 `samplerCube` 的拒绝（立方体视图落地了）；上传走 vsg 的 TransferTask（数据背书的 `vsg::Image`），像素证：四色 2x2 → 四象限、六色 cube → 五方向、重填 → 第二趟新颜色；5 条变异反证全红（§11.16al）。 |
| ~~M8d-2（变体的 define 进管线身份）~~ | **已完成（2026-09-22）**：`api/ProgramVariant`（规则 + `bits()` + `defines()` + `describe()` + `variantOf(材质, 几何)`）；`PipelineKey += variant`（相等 + 散列 + 审计表行数不变）；`Shaders.defines` 一份清单喂**扫描与编译**两侧；`compileStage` 交给 vsg 的 `compiler.compile(stage, defines)`；`Scope::Entry += variant`（放在最后，旧聚合初始化照旧编）；`recordCommand` 由**事实**算变体并按四元组配 half、三种拒绝各自说清；顺手修**每趟只 serve 第一个 content half**（多 half 的趟绑错集合 = VUID 00358+08600）；5 条变异反证全红（§11.16am）。 |
| ~~M8e（三张表的生产侧）~~ | **已完成（2026-09-22）**：`api/ContentStore`（track 活对象 + 条目持键；`tablesFor(计划)` 只建点名者、稳态 0 重建；几何/程序按 SDK 修订重建、材质按 `updateMaterial` 计数；被顶替的修订留在表里直到停靠到期、材质就地替换只停旧值；`releaseAbandoned`；无窗口 = 留下并计数）。顺手：程序表按变体作键（`ProgramFacts.variant` + `findProgram(…, variant)` + `recordCommand` 先算变体），两个查找改为扫全表（表里可能有被顶替的修订）；真设备用例 = store 生产的表画的整帧（push 声明的有无由变体决定）；6 条变异反证全红（§11.16an）。 |
| ~~M8f（半片的生产侧）~~ | **已完成（2026-09-22）**：`api/ContentHalves`（走 pass、与录制器同查找 ⇒ 每个 (kind, program, revision, layout, variant, 附件数) 一片；层 + 录制器 + `Scope::Entry`；键离开表 ⇒ 停靠；被拒的层记住不重试）；`ContentPipeline::create(abi, GeometryFacts, shaders, settings)`（绑定 = 规范编号、格式 = 分量数，自定义通道拒）且夹具改用它；真设备用例 = store 的表 + 生产者的半片画的整帧；6 条变异反证全红（§11.16ao）。 |
| ~~M8g（一个 drawable 的图，一个集合）~~ | **已完成（2026-09-22）**：`BlockDescriptors::ImageSource`（纹理 + 修订，= MaterialImages 的键）+ `forAbi/create` 收它 + `source()`；`serveHalf` 收"drawable 要的图"，先精确、再退到"没有纹理"的集合、否则按名拒绝（两种失败分说）；`recordCommand` 从材质事实算（纹理 + 实时 `revision()`）。真设备用例 = 同一半片两张贴图两套同形状集合 ⇒ 左洋红右绿；5 条变异反证全红（§11.16ap）。 |
| ~~M8h（声明集合的生产侧）~~ | **已完成（2026-09-22）**：`api/ContentSets`（按名取图：材质/白 + cube 白、影子+白、输入按序；键 = (program, revision, variant, set, 图来源)，空来源共享；键离开表 ⇒ 停靠；被拒记住；`sets()/builds()/fallbacks()/refused()`）；真设备用例 = 不手建任何集合的两纹理画；5 条变异反证全红（§11.16aq）。 |
| ~~M8i（一帧收成两次调用）~~ | **已完成（2026-09-22）**：`api/ContentAssembly`（beginFrame = 块预算 + 表；record = 半片 + 集合 + 每 pass 注册表 + 输入集合 + 一次 `ContentPass::record`；构造自带动态状态入口点；`facts()/halves()/sets()`）；真设备用例 = 只用两次调用的整帧 + 第二帧计数全不动（`frames()` 证块预算按帧开）；4/4 变异反证红（§11.16ar）。 |
| ~~M8j（丢掉的提交，下一帧修一次）~~ | **已完成（2026-09-22）**：`VsgExecutor::noteLostSubmission(frame)`（只标该帧 pass 真正点到的离屏目标；下一份计划答 `Repair(Bootstrap)`、第一个 bootstrap pass 清标志）；真设备用例（记录 → 标 1 个 → 下帧 `bootstrap` ✓ → 记录后清 ✓ → 别的目标不动 ✓）；变异反证红（§11.16as）。 |
| ~~M8k（提交这一步自己说）~~ | **已完成（2026-09-22）**：`VsgExecutor::submit(frame, viewer)`（记录 → 提交 → **失败即标记**：`noteLostSubmission` + 报 `SubmissionFailed` + 答 false；vsg 的异常词表两种都接——`vsg::Exception` 不是 std::exception）＋ SDK 分类表按它自己写明的规则在 `Count` 前追加 `SubmissionFailed`；真设备用例（成功的提交 ⇒ 清屏色真的在像素里、无标记；失败 ⇒ **只有它写过的**目标被标 + 拒绝计数 +1；下帧计划 `bootstrap` ✓）；4/4 变异反证红 + 一次"只留 std::exception 分支"的变异也红（§11.16at）。 |
| ~~M8l（形状变了就是真的重建）~~ | **已完成（2026-09-22）**：`OffscreenTarget::rebuild(wanted, timeline, retirement)`（渲染通道 + 全部 load-op 变体 + 附件一起重建，旧的**连 pass 一起停靠**（记录过的 `vkCmdBeginRenderPass` 直接点名它）；lease 两向拒；构建失败一个字段都不动）+ `core::planTarget` 次序改为**形状变化先于 load-op 修复**；设备相位（1 色 8×4 ⇒ 2 色 + D32F 16×12；附件 0 / 附件 1 / 深度三条读回；compatibility 真的变了；`pending()==1`、`deviceWaits()==0`）+ lease 两向拒绝用例 + 2 条次序用例；变异 5/5 红（次序复原 / 新形状不装 / 事实不重置 / 旧变体留着 / 直接销毁不停靠）+ 一次**用例抓不住、门禁 VUID 抓住**（18 条）；门禁 639 用例 / 98 套件、0 VUID（§11.16au）。 |
| ~~M8m（帧驱动应用计划的答案）~~ | **已完成（2026-09-22）**：`VsgExecutor::applyTargetPlans(frame, facts, timeline, retirement)`（只走**计划点名的**目标；`ResizeInPlace` ⇒ `resize`、`Rebuild` ⇒ `rebuild`；新形状取自 facts 的 wanted，目标的 clear 策略与深度提升取自新访问器 `OffscreenTarget::layout()`；计分 `resized/rebuilt/refused/failed`）+ 设备相位（三帧：Repair 不应用、ResizeInPlace ⇒ 16×12 像素、Rebuild ⇒ 2 色 + 深度；旁观者目标不动；相位 11 行）+ 真设备用例（"计划与目标不符 ⇒ 拒录"那帧在应用之后**录得进去**）；变异 4/4 红（§11.16av）。 |
| ~~M8n（会话的提交也自己说）~~ | **已完成（2026-09-22）**：`Session::commitFrame()` 自己驱动 viewer 的任务（`Viewer::recordAndSubmit` 返回 void、**吞掉队列 `VkResult`**）：任何一个任务提交失败（VkResult 非成功，或 vsg 抛异常）⇒ 报 `SubmissionFailed`、答 false、`lostFrames+1`；**不呈现、不计已呈现、不声称完成**；帧本身照样结束（`FrameTimeline::abandoned(token)`：令牌被消费、**submitted 水位不动**——两个水位因此分别是"帧"与"提交"）；真设备用例（丢帧的提交答 false + 报告 + 计数 + 水位 + 无开帧；重建会话后正常提交呈现）+ 1 条无设备时间线用例；变异 6/6 红 + 一次"照样呈现"挂住并带 2 条 VUID（§11.16aw）。 |
| ~~M8o（窗口路径一次调用）~~ | **已完成（2026-09-22）**：`VsgExecutor::commit(frame, session)`（`commitFrame()` 答 false ⇒ `noteLostSubmission(frame)`；报告留会话、标记归执行器）+ 真设备用例（两个会话：A 上 `commit` 成功、离屏目标像素 + "没被标"；B 上把记录步做成抛异常 ⇒ 答 false + `SubmissionFailed` + 标记 + 下一份计划里**写它的那一趟** `bootstrap` + 录进去清掉；两次运行各 **0 VUID**）；3/3 变异红；**顺手撞出新口子**：上一次提交还在飞时再 `assignFrameGraphs` 会销毁 viewer 的 task ⇒ 12 条 VUID + 验证层里段错误（§11.16ax）。 |
| ~~M8p（每帧换图不再拆机器）~~ | **已完成（2026-09-22）**：`SessionContentAccess::assignFrameGraphs` 改成把新图**交给自己已有的 task**（`task->commandGraphs = graphs`），只在 viewer 一个 task 都没有时才让 viewer 重建——不再销毁在飞的 fence / semaphore / 命令缓冲；真设备用例（一帧一图、帧帧在飞时换图，六个提交零 VUID、`deviceWaits()==0`、像素指出**最后换的那张图**真的被提交）+ 3/3 变异红（旧行为 ⇒ 30 条 VUID；不换图 ⇒ 像素错；不编译 ⇒ 红）（§11.16ay）。 |
| ~~M8q（丢帧的会话自己活下来）~~ | **已完成（2026-09-22）**：`Session::commitFrame()` 的失败分支重建 swapchain（`window->resize()`：vsg 的 `buildSwapchain()` 先 `vkDeviceWaitIdle` 再换链）+ **把这次 idle 计进 `deviceWaits()`**；真设备用例（同一会话丢帧后**下一帧就位**：`commit` 答 true、被标的目标在它的 bootstrap 趟里清掉、像素 = 清屏色、`framesPresented`+5、`lostFrames==1`、`deviceWaits==1`、报告恰好一条；两次运行各 0 VUID）+ 3/3 变异红（不重建 ⇒ 10 条 VUID；不计数 ⇒ 红；每帧都重建 ⇒ 红，还连带 8 个会话用例红）（§11.16az）。 |
| ~~M8r（计划说的形状，连格式一起核对）~~ | **已完成（2026-09-22）**：`core::CompiledShape`（形状的兼容性半边，编译器用 `arena.copy` 抄进帧的 arena ⇒ 计划里没有指向宿主 facts 的 span、也不每帧分配）+ `CompiledTarget::shape` + `core::statedShapeAgrees`（引擎半边每份计划都说了 ⇒ 直接比；**设备半边只在两边都说了时比**）；`recordOffscreen` / `recordWindow` 都接上（窗口那趟现在拿到 `CompiledTarget`）；无设备用例（计划的副本是它自己的 + 判定表逐行）+ 两条真设备用例（离屏：数目相同的引擎格式漂移、引擎相同的设备格式漂移各拒一次、说真话即录；窗口：同样的事 + 说真话后**呈递**，`deviceWaits()==0`）；当场修掉两处旧夹具的谎话；变异 **5/5 红**（不抄形状 ⇒ 75 行红、无条件下比设备半边 ⇒ 51 行红）（§11.16ba）。 |
| ~~M8s（窗口的答案也有执行者）~~ | **已完成（2026-09-22）**：`WindowTarget::facts()` 把 "想要" 与 "已有" 分开（wanted = swapchain 现在的 live 采样、current = 记录被建时的形状 ⇒ 平台换了格式就是 `Rebuild`，不再被静默记进旧兼容性的通道）+ `WindowTarget::refresh()`（重新采样，变了才替换；**唯一能写这份形状的是平台**）+ `applyTargetPlans` 的窗口臂（`Rebuild` ⇒ 问平台：变了 ⇒ `rebuilt`，没变 ⇒ `failed`；`ResizeInPlace` 无事可做）；真设备用例（稳定帧四计数全 0 且录得进去；说谎的帧 ⇒ `failed==1`、主张未被采纳、`record` 拒录；再说真话 ⇒ 录进去并呈递、`deviceWaits()==0`）+ 3/3 变异红（§11.16bb）。 |
| ~~M8t（`skyMap` 是 drawable 自己的图）~~ | **已完成（2026-09-23）**：名字表改正——`skyMap` 与 `diffuseMap` 同一行（`Material`，引擎自带天空程序的文本自己写着它是 material 的纹理、在同 ABI 的 diffuse 槽），`Environment` 行与那一整套拒绝删除；材质图的**种类必须对得上声明的采样器**（`ContentSets` 按声明的种类给 fallback、不一致 ⇒ 声明的白 + 计数；`createScreen` 拒 cube 声明）；顺手抓到并修掉**两处未初始化的 `ImageSource`**（没贴图的材质被按上一个 drawable 的纹理归档）；真设备用例 = 引擎自带 `skyboxProgram` 四个方向条带（+Z 洋红 / +X 红 / 无图白 / 2D 图配 cube 声明 ⇒ 白）+ 计数器；变异 **5/5 红**（其中两条带 6 / 2 条 VUID）（§11.16bc）。 |
| ~~M9a（门面立起来：会话生命周期 + 空帧驱动）~~ | **已完成（2026-09-23）**：`api/VsgBackend`（`VsgBackend : vine::graphics::RenderBackend`，PImpl）——引擎与重写版之间的缝：`initialize()` 由公告（宿主句柄、尺寸、默认程序）建会话并交给执行器；`beginFrame/endFrame/swapBuffers` 走会话的帧协议（**空帧照样编译、录制、呈递**）；`setWindowHandle/nativeHandle`、`resize` 是公告（下次起会话时生效、活着时报一次）；诊断走核心的**一条**路（会话自己的报告由 SDK 的 sink 与 `diagnosticCount()` 看到，门面不重复报）；画的一半（pass/离屏/内容/全屏/读回）**每处报一次"还没服务"**（`core::ReportOnce`），`supportsRenderTargets() == false`（引擎在摆离屏工作**之前**就知道）。真设备用例两条：①无会话开帧 ⇒ 报一次；三次空帧 ⇒ `framesPresented()==3`、`deviceWaits()==0`、健康会话零报告；三趟 pass 回路 ⇒ **恰好 4 条**报告（`endPass` 与会话内的 `beginPass` 同一条情节）；活着的 `resize` 报一次；再 `initialize()` ⇒ 计数归零；双 `shutdown()` 后再起来照样呈递；②宿主面被采纳、**换另一个宿主面不重建会话**（帧号 2→3 连续），两次关闭后两个宿主窗口都还活着。**本片撞出的真问题**：空帧若提交**空命令图**，被 acquire 的图像停在 `UNDEFINED` ⇒ 呈递 **VUID 01430**（实测 16 行，gtest 全绿）——把图像搬出 `UNDEFINED` 的是**窗口那棵 render graph**（会话自己那张初始化期的图里就有它）；修法 = 计划为空时不换图、直接 `commit` 会话自己的图。变异 **4/4 红**（不呈递 ⇒ 5 行 + 6 VUID；又交空图 ⇒ 16 VUID；不报告 ⇒ 3 行；`supportsRenderTargets` 说谎 ⇒ 3 行）（§11.16bd）。 |
| ~~M9b（pass 协议接到内容层）~~ | **已完成（2026-09-23）**：`VsgBackend` 把 SDK 的 pass 协议翻进计划录制器——`beginPass` 经新的 `api/PassRegistry`（SDK 的 pass 对象 → `core::PassId`，**号永不复用**、`releasePass()` 只忘身份）交给 `core::FrameRecorder`，`endPass/setPassOrder/setRenderTarget(nullptr)/setViewport/setClearPolicy/setDepthMode/setLights/render` 全部照收；**帧外的调用**（引擎的 pre-frame warm-up：每个 enabled 非清屏 pass 在**任何帧之前**执行一次）按 SDK 契约**惰性**——只有 pass 身份留下；`render()` 顺带把命令点名的 geometry/material/program 追踪进 `ContentStore`（默认程序在 `initialize` 时追）；`swapBuffers` 现在**真的组装内容**（`ContentAssembly`：开块预算 + 表 → 每趟 pass 的半片/集合/输入/视图块 → `executor.record(frame, graph, packets)`），视图块按**该趟的相机 + 帧时钟 + 窗口尺寸**建，兼容性取窗口自己的形状。**本片撞出的真缺陷**：执行器的窗口臂把每帧内容**累加**进保留视图（`addContent`）——两帧之间移动相机就能看见"上一帧的画还在"且组无边增长；修法 = `WindowTarget::beginFrame()`（帧首清掉**本帧内容组**，宿主的会话根不动）+ `addFrameContent()`，执行器在**本帧第一趟窗口 pass** 调它（空帧不调：上一幅画照旧被呈递）。真设备用例（warm-up 惰性 + 身份跨帧、像素里读到相机 x / 窗口尺寸 / 清屏色、第二帧"稳态零构建"、第三帧换相机 ⇒ 左半旧画**消失**、右半新画就位、`deviceWaits()==0`、pass 释放即重发得新号）+ 无设备 `PassRegistryTest`；变异 **7/7 红**（不换帧内容/不组装内容/不开范围/丢清屏色/不追踪对象/不释放身份/warm-up 进录制器）。门禁 **655 用例 / 100 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 856、相位 11 行 / 2 次（§11.16be）。 |
| ~~M9c（离屏那一半：目标、输入、全屏、重建）~~ | **已完成（2026-09-23）**：`supportsRenderTargets()` 转真；新 `api/HostTargets`——宿主 `RenderTarget` 的**描述是快照**（每次点名比字段更新，稳态零分配；绝不留宿主指针）、对象**懒建**（正值尺寸 + 至少一个彩色附件才建；建不成与"还缺描述"分开作答：`NotBuilt`/`DepthSourceMissing`/`BuildFailed`）、借来的深度持**lender 的 share**（`shared_ptr`，lender 被释放而 borrower 还在也不悬空）；`facts()` 是计划解析**目标与输入**的那张表（引擎半边永远是宿主的话；**设备半边只在引擎形状没变时**才说——M8r 的"不知道不是没有"；深度的事实：建成后按目标自己的 `layout()` 报 promotion、borrowed 时永不承诺；shadow 声明（谁的图 + 产出者的矩阵）原样搬）；门面：`setRenderTarget` 建/注册/交身份（只报 `DepthSourceMissing`/`BuildFailed` 一次、`Ready` 后重置情节；`NotBuilt` 静默——宿主先配后画）、`setPassInputs` 观察 + 转身份、`drawScreenProgram` 追踪片元程序 + 交身份、`releaseRenderTarget` 忘条目 + 注销执行器 + 让 recorder 丢掉挂起公告；内容驱动按**每趟自己的目标**取兼容性与尺寸（窗口或宿主目标），每个**声明输入**给一条 `InputImages`（该目标的彩色视图 + 计划说可采样时的深度视图，scratch 复用不每帧分配）。真设备用例：离屏目标画三角 → SDK 自带 `screenCopyProgram(0)` 复制到窗口（像素：红三角 + **目标自己的清屏绿**，不是窗口的清屏蓝）；建成目标的 facts（设备拼写非空、`built` 真）；`setSize` ⇒ 计划答 ResizeInPlace、**目标真的变 32×48**、画还在；`attachColor` 第二个附件 ⇒ 形状变 ⇒ 计划答 Rebuild、目标 2 附件、屏幕仍采附件 0；释放后 `live()==0`；借一个**不存在的 lender** ⇒ 报一次（三次公告一条情节）。无设备 `HostTargetsTest`（快照 vs 活对象、借来的深度不是 borrower 的承诺、shadow 声明原样、释放只答一次）。变异 **7/7 红**（不建/不进事实表/丢目标身份/离屏不录内容/输入不给图/释放不清/**形状变了还说旧的设备格式**——后三条各带 12 / 2 / 2 条 VUID）。门禁 **658 用例 / 101 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 859、相位 11 行 / 2 次（§11.16bf）。 |
| ~~M9d（宿主目标的读回）~~ | **已完成（2026-09-23）**：`readColorBuffer`/`readDepthBuffer` 转真；新 `api/HostReadback`——**两个来源一张图**：执行器每帧本来就在**全部 pass 之后**给每个目标接上自己的拷贝节点（彩色目标=附件 0，只有深度的目标=深度），所以读"帧已经拷过的"**不用再提交任何东西**（停一次设备 + 读映射缓冲）；帧**没拷过的**（彩色目标的深度、第二个彩色附件）在这一层用目标自己的拷贝命令**提交一次**（自己的 command buffer + fence，100 s 上限）——被画过的目标处在确定布局，拷贝合法；**没画过的目标答 `NotRecorded`**（拷贝 UNDEFINED 内存再把垃圾叫"图片"是不行的）。顺序即契约：①**先分类**（不需要设备：未知附件、读不了的格式、没录过的目标——不能服务的请求**一分钱不花**）；②**调用者停设备**（写缓冲的那一帧可能还在飞）——这次停**被计数**（`SessionContentAccess::waitDeviceIdle`，与 `deviceWaits()` 同一个计数器：读回是唯一允许停设备的路径，"恰好停一次"保持可查）；③只有帧没拷过才在这里拷（在停之后，两次提交不会重叠）；④探针读映射缓冲，把字节/浮点交给宿主（`PixelProbe::pixels()`/`DepthProbe::values()` 交出原缓冲）。门面：拒绝**两个频道一起说**（`why` 给机器答案、诊断路由给一句话，**每个情节一次**——目标自己的情节挂在条目上，目标解析不了就共用一个每入口点情节；成功即 rearm）；**还没停设备就先拒绝**；借来的深度走 `BorrowedDepth`（SDK 的规矩：源目标才是读的地方）；`readbackResultOf` 一张表（未知目标/没建好/没录过 ⇒ NotReady、空目标/未知附件 ⇒ Invalid、读不了的格式/借来的深度/没设备 ⇒ Unsupported、搬运失败 ⇒ Failed；**未映射的落到 Failed 永不落到 Ok**）；诊断类别沿用旧实现的 `ContentSkipped`。真设备用例（`VsgBackendTest.TheSdkReadsBackItsOwnTargetsPixelsAndDepths`，64×64 的 RGBA8+D32F 目标一帧）：彩色读回 `Ok`、`64*64*4` 字节、三角内 (16,40) 是 `(255,0,0,255)`、外面 (48,8) 是目标的清屏绿、alpha 255；深度 4096 个 float、三角形处 ∈ (0.05, 0.95)、清屏处 `==0.0`、全在 [0,1]；`deviceWaits` 每次读回 **+1**；拒绝面（**全部不加等待**）：附件 5 ⇒ Invalid、未知目标 ⇒ NotReady、RGBA16F ⇒ Unsupported（持有且建成、不需要帧）、borrower 读深度 ⇒ Unsupported、**lender 建成但没画过 ⇒ NotReady**、释放过的目标 ⇒ NotReady。**本片撞出的真问题**：只有离屏 pass 的帧**从没跑过窗口那棵图** ⇒ 被 acquire 的图像停在 `UNDEFINED` ⇒ 呈递 **VUID 01430**（M9a 的同一课，换了张脸）；修法 = `WindowTarget::prepareWithoutClear()`（只重开渲染区、清屏值原样）+ 执行器在**没有任何窗口 pass** 的帧尾把窗口图**不加壳**挂进命令图（没有 pass 可以归因，注释里写明）。变异 **6/6 红**：①没画过的目标读起来像能服务（3 行）；②只有离屏的帧不跑窗口图（**2 条 VUID**）；③**两道"没录过"的闸一起拆**（3 行；只拆一道是绿的——分类那一道先答，两道闸在**不同来源**上各管一半）；④借来的深度从 borrower 读（3 行）；⑤读回的停不计（3 行）；⑥帧不把自己的目标拷回来（**63 行**，执行器与相位一起红）。门禁 **659 用例 / 101 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 861、相位 11 行 / 2 次（§11.16bg）。 |
| ~~M9e（活着的 resize）~~ | **已完成（2026-09-23）**：`resize()` 转真；`SessionContentAccess::followResizedSurface`——**表面拥有尺寸**（SDK 授权顺序 surface > announcement > default），跟随 = `window->resize()`（重读表面几何 + 重建交换链），**停一次设备且被计数**（与 M8q 丢帧修复同一笔开销）；挂在尺寸上的东西下一帧现读（`WindowTarget::prepare(WithoutClear)` 的 renderArea、目标形状、每趟视图块），没有第二份尺寸要同步；非正值不跟随；公告仍是**下次 `initialize()`** 的建窗尺寸。`kUnservedResize` 槽删除 ⇒ **门面不再有未服务入口点**（剩下两个"没地方可去"的调用照旧各报一次：无会话的帧、内容世界未起来的 `render()`）；`BackendContentAccess::windowTarget()` 作为测试视图。**本片量到的机理**：表面变了却不公告/不跟随时，vsg 的 Viewer 会在 **acquire** 发现 `_extent2D` 与交换链不符（`Window::acquireNextImage` 直接答 `OUT_OF_DATE`），在**提交里**自己 `window->resize()`——而那一帧**已按旧矩形录完** ⇒ `VUID-VkRenderPassBeginInfo-pNext-02852/02853` + 段错误（M1 变异实测）。真设备用例：画面自身编码尺寸（`frame.y/160`、`frame.z/192`，两个尺寸下都在 [0,1]）——宿主窗口 128×96 → 改 96×64 + 公告 ⇒ 恰好 **1 次**被计数的停、`WindowTarget` 报 96×64、零报告、新尺寸下三角与清屏都在（`resize(0,0)` 一分钱不花）；第一个用例同步改写（活着 resize 不再报、`deviceWaits()==1`、重生日窗口 = 公告的 320×180）。变异 **5/5 红**（①不重读表面 ⇒ **4 条 VUID + 段错误**；②重建停不计数；③活得公告不记（回默认 640×360）；④ `0×0` 闸拆掉；⑤渲染区冻在第一个尺寸 ⇒ **4 条 VUID + 段错误**）。门禁 **660 用例 / 101 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 861、相位 11 行 / 2 次（§11.16bh）。 |
| ~~M9f（工厂切到门面）~~ | **已完成（2026-09-23）**：`VsgRenderBackendFactory::create()` 造 `VsgBackend` 而不是 `VsgRenderer`——**注册名 "vsg" 就是重写版**（插件 load → 注册表 → create 这条生产路径）；旧实现仍在树里、仍由自己的测试驱动，但没有任何名字创建它。**两条注册路径**（`load()` 里显式注册的 `s_factory` 与 `VsgRenderBackendFactory.cpp` 里的静态 `Registrar`）都在，所以变异 M3 要把**两处**一起关才红。**切换逼出的半片**：老实现每帧刷新它命令到的每个材质（`SceneBridge` → `VsgMaterialManager::updateMaterial`，而那方法自己就是 compare-and-write），重写版 `ContentStore` 只认 `updateMaterial()` 推的修订、**而没人调它**（§11.16be 的登记）⇒ 不补则切完工厂**材质编辑不再进画**。所以门面 `render()` 对每条命令的材质调一次 `ContentStore::updateMaterial()`，并把该方法从"盲增修订"改成 **compare-and-write**：读材质现在的字段、与表里那行的块**按 ABI 逐成员比较**（`materialBlockAgreesWith`——**不能比字节**，块的尾部填充故意不写；块映射收成一处 `blockOfMaterial`），只有不同才推修订 ⇒ 下一帧换行 + 停靠旧值照旧，稳态帧只比不建、不分配。证据：真设备①`VsgBackendPluginTest.TheRegisteredBackendComesUpOnTheHostsSurfaceAndDraws`（**通过注册表**拿门面、采纳宿主窗口、走 SDK 协议画一帧、把宿主窗口像素读回）；②`VsgBackendTest.AMaterialEditLandsOnTheNextFrameAndASteadyFrameRebuildsNothing`（材质 diffuse 着色：编辑帧 `builds()` 恰好 +1 且像素质变，稳态帧不涨、`deviceWaits()==0`）；无设备 `ContentStoreTest.ATouchWithNoEditChangesNothing`。`CreateBackendByName` 用 `dynamic_cast` 钉住"造出来的是门面"；`BackendContentAccess::store()` 是测试视图。**夹具教训**：块 canonical 绑定 **View=0 / Draw=1 / Material=2**——把材质块写在 binding 0 会被 ABI 扫描判成"两阶段同绑定不一致" ⇒ 内容层整趟拒绝（只剩清屏色）。变异 **6/6 红**（旧实现 / 答空 / **两处注册全关** / 名字不是 vsg / 触碰盲增 / 门面不触碰）。门禁 **663 用例 / 101 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 861、相位 11 行 / 2 次（§11.16bi）。 |
| **下一步** | 旧实现退场（删 `VsgRenderer` 一路的源与测试——名字已换，树里那份是死代码），或挑一条登记的口子做。 |

M1 起每条相位都要同时给出：像素/计数器断言（`PhaseTable` + `PixelProbe`）、不得移动的计数器
（`expect` 为“不变”的那些）、以及需要时的一段 `AllocationGate` 窗口。

代码落地前的约定：新增 `core/` 文件会被 `v_add_plugin` 的 `GLOB_RECURSE` 自动收进插件，
`tests/test_vsg` 需要显式加源文件（两份 CMakeLists 各一处）。

### 11.16ad（M7e）会话侧读数字半边

身份半边（§11.16ac）把每个 pass 记成可归属的区间；这一半把区间读成数字，规则只有三条：

1. **开关是环境的**：`VINE_VSG_PROFILE` 在 `initialize()` 里读一次（`"0"` / 空 = 关），关着时一个
   `Profiler` 都不装——`profiling()` 为假、`gpuProfile().enabled` 为假、`passes` 为空。
2. **读是走查，不是采样**：日志里最后几帧还没有结果（读取不带 `VK_QUERY_RESULT_WAIT_BIT`），所以从后
   往前找第一帧有结果的：`age_frames = frames.size() - 1 - index`。一帧只要有命令缓冲区区间**或** pass
   样本就算读到了——只画窗口图、没有执行层包 pass 的会话照样能报 `frame_gpu_ms`；什么都没读到就报
   `readable == false`，绝不报 0 假冒“这帧不耗时”。
3. **读数字不许让设备停**：整条路径不碰设备（不提交、不等待、不做阻塞式的查询结果读取）。用例把这条钉成
   断言：读之前 / 之后的 `deviceWaits()` 必须相等——设备侧等待正是 `RetirementQueue` 存在的理由，
   读性能数字不该是它的第一个反例。

归属键是**地址**，只许比较、不许解引用（图可能在帧之间被释放——旧实现实测过这条崩溃）；归属映射的另一半在
`VsgExecutor::profileOf(graph)`。时间戳是否可用是**设备事实**（`timestampComputeAndGraphics`），不可用时
回答“开着、但没有可读的”，而不是一个数字。

实测（lavapipe）：8 帧后 `readable == true`、`age_frames == 2`、`frame_gpu_ms ≈ 1.86 ms`、1 个带非空键的
样本——样本数是日志里带对象的 GPU 区间数（会话自己不包 pass，所以这是上游/执行层写下的那些），用例只断言
“键非空、毫秒非负”，不断言条数（那是驱动时序的函数）。

证据：`SessionTest.TheProfileIsTheEnvironmentsSwitchAndReadingItNeverStopsTheDevice`（关：`profiling()`
假 + 4 帧后不可读 + 空 `passes` + `deviceWaits()==0`；开：8 帧后 `readable` 必须为真、`age_frames < 8`、
逐样本键非空且毫秒非负、读前后 `deviceWaits()` 相等；无时间戳能力则 `GTEST_SKIP`）；变异反证 5/5 红——
(a) `readable` 永不为真、(b) `age_frames` 取成 `frames.size()`、(c) 关着也报可读、(d) 读里加一次
`noteDeviceWait()`、(e) 空键样本放行。门禁：565 用例 / 88 套件、0 VUID、0 SYNC-HAZARD、hygiene 全清、
相位 9 行 / 2 次运行全收尾。

### 11.16ae M8a（2026-09-22）：场景桥的第一半——程序文本的声明成为事实（`api/ProgramAbi`，无设备）

§11.16u 把“引擎自带的前向程序文本与本后端的 set 0 布局不同”登记为场景桥的债。这一片不动管线，只把债
**变成事实**。理由是那句债的反面：**一个后端选不了 shader 的块住在哪**——`layout(set = …, binding = …)`
写在宿主给的文本里，而“文本与集合布局必须匹配”不是风格问题（不匹配时 shader 读的是没人写过的地方，
而管线等的是一个 shader 从不点名的绑定）。所以场景桥的接法是**按程序文本自己的声明服务**：布局跟着
声明装配（M8b），每个声明按**角色**取值（M8c）。重写版自己的 set 0 布局因此不是“两套 ABI”里的另一套，
而是**另一种声明**——它的程序照 `VineViewBlock` 这类 L1 名声明，角色就认出来了；这正是
`graphics-shader.md` §11.3 早就写下的“GLSL 块类型名与 L1 名逐字相同”。

**事实与政策分开**。`scanProgramAbi(vertex, fragment, defines, out)` 只回答文本声明了什么：
`(set, binding)`、种类（uniform 块 / `sampler2D` / `samplerCube` / 其它 sampler）、哪些阶段读它、
它是不是五个 L1 块之一（**按类型名，不按位置**）、它的 std140 大小（按成员算出来的）、以及 push 范围
（std430，认 `layout(offset = …)`）。它**不**决定哪段字节、哪张图去哪个绑定——那是服务层的政策，全屏
路径与内容路径在同一份事实上做不同的决定。认不出的块类型是 `Foreign` 这一**事实**而不是拒绝：文本合法，
是后端填不了它，拒绝属于绑定那一层。同理，一个没有绑定的 `uniform sampler2D` 不是事实（文本什么都没说，
编译器自己分配）——不许替它编一个槽。

**变体才是事实的主体**。引擎的程序用 `#ifdef` 门住声明（`VINE_DIFFUSE_MAP`、`VINE_TEXCOORD_CUBE`…），
所以“这个程序的绑定”要等 defines 才知道：扫描**按变体回答**，条件只读地求值，文本自己的 `#define`
算数（平面前向的 `VINE_FLAT` 就是这么来的），而 **taken 分支里的 `#error` 让变体 Malformed**——那是驱动
要到很晚才说、而读文本的人现在就能说的事（SDK 的 texcoord 种类就是这么写的：采了槽却没说 kind ⇒ 编译
不过）。扫描的语法是一个小而固定的子集（`#ifdef` / `#ifndef` / `#if defined(X)`（可带 `!`）、同形的
`#elif`、`#else` / `#endif`、`#define` / `#undef`、`#error`、`#pragma import_defines(…)`），**读不出来
的报，不猜**：一个绑定猜错的后果是着色器读没人写过的内存。`import_defines` 也进事实——编译器会把源里
没要过的 define 静默丢掉，那是只有读过文本的人能说出的第二件事。

| 文件 | 是什么 |
| --- | --- |
| `api/ProgramAbi.hpp` / `src/api/ProgramAbi.cpp`（新） | `AbiBinding` / `AbiPushRange` / `ProgramAbi` + `scanProgramAbi`（条件求值 → 声明扫描 → 跨阶段合并）+ `blockRoleOf`（五个 L1 名 → 角色）+ `abiBlockRoleName`；成员按 std140 规则定尺寸（标量/vec/ivec/uvec/bvec/mat，数组按 stride 补齐） |
| `tests/test_vsg/ProgramAbiTest.cpp`（新） | 11 条无设备用例：**引擎自己的程序**逐条读出来并与既有后端服务的那一份对照；三张合成文本（本重写版的形状、坐在 diffuse 槽上的外来块、成员漂移的 material）；拒绝面（跨阶段矛盾、读不出的 binding 值、读不出的条件、未收尾的条件、taken 的 `#error`） |

**引擎程序的实测 ABI**（扫描结果，逐条等于既有后端 `buildVineShaderSet` 声明的那一份）：

| 程序 | 声明 |
| --- | --- |
| `builtin_forward` | material `(0,0)` 64B、diffuse `(0,1)` **只在 `VINE_DIFFUSE_MAP`**（种类跟 `VINE_TEXCOORD_CUBE`/`_UV` 走）、lights `(0,2)` 112B、`shadow_map` `(0,3)`、`VineShadowBlock` `(0,4)` 80B、draw `(1,0)` 80B、push 128B **顶点** |
| `builtin_forward_flat` | 与前向逐条相同；`VINE_FLAT` 是**文本自己的 define**，不在 `import_defines` 里（那四个名字是 `VINE_DIFFUSE_MAP` / `VINE_TEXCOORD_CUBE` / `VINE_TEXCOORD_UV` / `VINE_VERTEX_COLOR`） |
| `builtin_gbuffer` | material `(0,0)` + push 128B **顶点**（**没有**灯与阴影块），diffuse 同样按变体出现 |
| `builtin_skybox` | 只有 `skyMap` `(0,1)`（cube / pair 两支都由 kind define 选），push 128B 顶点，**没有** material/lights |
| `builtin_deferred_lighting[_shadowed]` | 四个 G-buffer 采样 `0..3` + push 128B **片元**；带影变体多 `shadow_map` `(0,5)` 与 `VineShadowBlock` `(0,6)` |
| `builtin_screen_copy_<N>` / `builtin_fullscreen` | 一个 sampler 在 binding **N**（没有 set 限定词 ⇒ set 0）；全屏顶点阶段一个绑定都没有、一个 push 都没有 |

| 规则 | 结论（变异反证全部实测） |
| --- | --- |
| **角色是块的类型名** | 变异 N2（角色按 binding 序号给：0=view、1=draw…）⇒ 4 条红。引擎的 material 在 0、lights 在 2，按位置读会读成 view/draw——这条正是“位置不是角色”的实测 |
| **条件决定事实** | 变异 N1（不管 defines，所有分支都算 taken）⇒ 4 条红：同一个 `(0,1)` 上出现两种 sampler 种类 ⇒ Malformed |
| **`#error` 是拒绝** | 变异 N5（taken 分支里的 `#error` 当作能编译）⇒ “只给 `VINE_DIFFUSE_MAP` 的变体”那条红 |
| **尺寸要按 std140 算** | 变异 N3（成员字节相加，不做对齐/补齐）⇒ 4 条红（52 → 64 这类补齐没了；`sizeof` 对照是判据） |
| **一个 `(set,binding)` 不能是两个东西** | 变异 N4（把同一 `(set,binding)` 的两个声明当成同一个绑定）⇒ 拒绝面那条红 |
| **漂移可见** | material 块多一个 `vec4` ⇒ 事实里是 80，而 `sizeof(VineMaterialBlock)` 是 64：绑定那一层能看见不一致，而不是假设它不存在 |

**这一片留下的口子（登记，不假装解决）**：

* **布局还没跟着声明装配**（M8b）：`ContentPipeline` 现在仍收 `BlockDescriptors` 的固定 5 绑定块集；
  下一步是让块角色 → dynamic UBO、sampler → 静态绑定、push 范围照声明来。
* **“哪段数据去哪个角色”还没有政策**（M8c）：material / lights / shadow 块在 arena 里的偏移、draw 块在
  set 1、`pc` 的 `projection` / `modelView`（`view` × `model`）、`diffuseMap` 的贴图（含白色 fallback）
  与 `shadow_map` 的图，都是服务层要按声明里的绑定号填的活。
* **变体从哪来仍没定**：`VINE_DIFFUSE_MAP` / kind 取决于几何的纹理坐标与材质有没有贴图，而管线身份现在
  是 `(program, revision, layout)`；把“变体的 define 进身份”与几何/材质的纹理支持一起做是独立的一片。
* **重写版自己的测试文本要改用 L1 块名**：`ProgramAbiTest` 里已按 `VineViewBlock` 等声明了本重写版的形状
  （角色按名认领）；散在设备用例里的 `ViewBlock` 一类名字在 M8b 接上布局时一并改名。
* **带影延迟光照的 5/6 仍是文本里写死的数**：它们随 G-buffer 的彩色数（4）而定；“按 pass 的形状算绑定号”
  属于全屏路径的后续（§11.16v 登记过）。

证据：`test_vsg` 全量 **576 用例 / 90 套件全绿**（+11 用例，`ProgramAbiTest` 一套）；门禁一条命令
`scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、hygiene 0 / 821 文件、
`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、相位 9 行 / 2 次运行全收尾。变异反证
**5/5 红**（N1 条件不分、N2 角色按位置、N3 不按 std140 补齐、N4 矛盾相消、N5 `#error` 忽略）。

### 11.16af M8b（2026-09-22）：布局就是声明——块服务到文本自己写的那个 `(set, binding)` 去

§11.16ae 把“按程序文本自己的声明服务”定成政策，这一片让**装配真的按它走**：`ContentPipeline::create`
收的是 `ProgramAbi`（不再是“一个块集 + 一个布局”），`describeAbi` 把声明读成**布局本身**——每个被声明碰到
的 set 序号一条 `DescriptorSetLayout`（块 → dynamic UBO，绑在**它自己写的** binding 上），**空隙是真实的
空布局**而不是空指针（`PipelineLayout` 的集合列表是从 0 起的连续区间，空指针是无效句柄不是“没有这个
set”）；push 范围同样照声明来（内容程序声明了 push，范围就在——只是**这一片还不填**）。于是重写版自己的
“view 0 / draw 1 / material 2”不再是一条后端政策，而只是**另一种声明**：引擎的“material 0 + draw 1”、
第三种程序、任何一种，走的是同一条装配路径。

**填不了的按名字拒绝，绝不静默改派**（拒绝策略集中在 `describeAbi` 与 `acquire`）：外来块（不是五个 L1
名）、非 std140（尺寸是编译器的，绑进去的字节是 std140 的）、**声明比 L1 结构体更大**（多出来的字节 ABI
里没有）、`count != 1`、`sampler3D` 一类没有视图的种类、**内容路径里不在 set 1 的采样器**、以及“块和采样
输入挤在同一个 set”（一个 set 不能是两样东西——纹理支持来了才有那种排布）；`acquire` 另外拒“采样器的
binding 号超出这个 pass 的输入数”（那张图不存在，绑上去是另一次绘制的图）。

**pass 侧按声明的 set 逐条服务**：`Scope::descriptors` 换成 `Scope::block_sets`（**一组**块集），
`serveHalf` 用“set 序号 + 形状逐位相同”去认领（认不出就按名拒绝、每个条目只报一次），`recordCommand` 把
每条声明集的 `bind` 放进 `Draw::blocks`（span，逐条录制）；每个声明绑定一个动态偏移、按**形状的顺序**给
（不是按它们被写进代码的顺序）。还在 `serveHalf` 里立了 M8c 之前必须立的一道闸：**声明了 push 却没有填**
⇒ 拒绝这一半（“黑屏是因为矩阵是零”不算诊断，见 §11.16x 的教训）。

| 文件 | 是什么 |
| --- | --- |
| `api/ProgramAbi.hpp` / `src/api/ProgramAbi.cpp` | 不变（事实仍是 M8a 那份） |
| `api/FactResult.hpp`（新） | `FactMiss` + `FactResult` 搬出来，解开 `ProgramAbi` 进 `ProgramFacts` 的包含环 |
| `api/ContentFacts.hpp` / `api/ContentSources.cpp/.hpp` | `ProgramFacts` += `ProgramAbi abi{}`：两张数据源在 scan 时就带上事实（扫描失败 = Malformed） |
| `api/BlockDescriptors.hpp` / `src/api/BlockDescriptors.cpp` | 形状驱动：`Binding{binding, role}`、`canonicalShape()`（本重写版的声明 = 五种之一）、`layoutOfShape()`（形状→集合布局**唯一的写法**）、`create(..., shape, set_index)`、`forAbi(abi, set, …)`、`blockShapeOf(abi, set)`（自由函数）；`bind()` = 形状顺序、每绑定一个动态偏移 |
| `api/ContentPipeline.hpp` / `src/api/ContentPipeline.cpp` | `create(abi, bindings, attributes, shaders[, settings])`（旧的“块集”重载删掉）；`describeAbi` 拒绝政策 + 空隙填充；`abi()` / `blockShape(set)` / `blockSets()` 三个读出口；采样变体按声明集 + 输入集（set 1）装 |
| `api/ContentPass.hpp` / `src/api/ContentPass.cpp` | `Scope::block_sets`（一组）+ `serveHalf`（形状认领、按名拒绝、push 未填即拒）+ 逐声明集 `bind` |
| `api/ContentDraw.hpp` / `src/api/ContentDraw.cpp` | `Draw::blocks` 改成 span（每个声明集一条） |
| `tests/test_vsg/BlockDescriptorsTest.cpp` | +1 真设备用例：**按声明形状**（view 0 + material 3、set 1）建的集合 ⇒ 两个动态偏移 `{0, 64}`（正典五个在这里是错的） |
| `tests/test_vsg/ContentPipelineTest.cpp` | +2 无设备用例：声明在别处的布局（material 3、draw 在 set 1、只声明 set 2 时的**空隙是空布局**）；拒绝面一整批（外来块 / 超尺寸 / 非 std140 / 采样器在块集 / `sampler3D` / 前缀块可以 / 键盖不住采样器时 `acquire` 拒） |
| `tests/test_vsg/ContentPassTest.cpp` | +1 真设备像素用例：**SDK 排布的程序**（material 在 `(0,0)`、draw 在 `(1,0)`、无 push）——binding 0 上是全零的 view 块，所以“按键号猜角色”会画成黑 |

| 规则 | 结论（变异反证全部实测） |
| --- | --- |
| **角色是类型名，不是位置** | 变异 N1（角色按 binding 序号给）⇒ **7 条红**，含新的像素用例（`(0,0)` 上的 material 会被当成 view ⇒ 黑）与 `shape()[0].role == Material` 断言 |
| **声明写在哪个 set 就是哪个 set** | 变异 N2（所有块都进 set 0）⇒ 无设备用例 `sets.size() == 2` 失败 + 像素用例 SIGSEGV（shader 要的 set 1 布局不存在） |
| **每个声明绑定一个动态偏移** | 变异 N3（只写一个偏移、其余复用）⇒ 2 条红（正典五绑定那条 + 新的声明形状 `{0,64}` 那条） |
| **空隙必须是真布局** | 变异 N4（空格填成空指针）⇒ 像素/布局用例 SIGSEGV（`PipelineLayout` 里出现无效句柄）——**这条实测还暴露了一处死代码**：`create` 里那段“补空”防御循环从来没被走到过，已删（空隙只在 `describeAbi` 一处填） |
| **填不了的拒绝** | 变异 N5（不看 std140 / 不看尺寸）⇒ 拒绝面那条红（超尺寸与非 std140 的层被建了出来） |
| **逐声明集绑定** | 变异 N6（第一个块集复用给所有声明集）⇒ 像素用例 SIGSEGV（set 1 上绑了 set 0 的集合） |
| **“什么都不声明”就是“什么都不绑”** | 门禁实测：三处**测试夹具**（`ExecutorTest` / `MrtTargetTest` / `ScreenDrawTest`）仍无条件绑一个块集，而它们的程序文本一个块都没声明 ⇒ 布局 0 个 set、绑定无效句柄：`VUID-vkCmdBindDescriptorSets-firstSet-00360` ×6。修的是夹具（没有声明就没有集合可绑），不是给后端加“容忍” |

**这一片留下的口子（登记，不假装解决）**：

* **角色取值是 M8c**：material / lights / shadow 块在 arena 里的偏移按角色拿；draw 块搬进 set 1；`pc` 的
  `projection` / `modelView`（`view` × `model`）填进声明出来的 push 范围；`diffuseMap` 的白色 fallback 与
  `shadow_map` 的图。在这一片里，声明了 push 的内容半片会被 `serveHalf` **拒绝**（宁可拒绝，不用上一个
  draw 推过的字节画）。
* **变体 define 还没进管线身份**：`import_defines` 已经在事实里，但“哪些 define 生效”与几何/材质的纹理
  支持一起做（§11.16ae 登记过）。
* **采样输入的 set 序号仍是内容路径的 1**：`kContentInputSet` 写死；纹理支持来了才有“采样与块混在一个
  set”的排布需要（那种排布现在被明确拒绝）。
* **三张表的生产侧**还是 §11.17 那一行：活的 SDK 对象 + 修订 + 退役（现在的用例都是自己填事实）。

证据：`test_vsg` 全量 **580 用例 / 90 套件全绿**（+4 用例：真设备像素 1、真设备描述符 1、无设备 2）；门禁
一条命令 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、hygiene 0 / 822 文件、
`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、相位 9 行 / 2 次运行全收尾。变异反证
**6/6 红**（N1 角色按位置、N2 忽略声明 set、N3 偏移复用、N4 空隙空指针、N5 放弃拒绝、N6 复用第一个块集），
其中 N2 / N4 / N6 的失败形态是崩溃（无效句柄进 `PipelineLayout` / 绑错集合），N1 / N3 / N5 是断言。

### 11.16ag M8c-1（2026-09-22）：声明出来的 push 由 pass 自己填（相机矩阵按名字装配）

§11.16af 让布局跟着声明走，于是“一个声明了 push 的程序”不再是 M8b 那样被 `serveHalf` 一拒了事：范围是
它自己声明的（128B、顶点阶段、`offset = 0`），里面要装什么也是它自己写下的——`projection` 与
`modelView` 两个 `mat4`。而这两份值不是新约定，是**同一对 L1 值的另一个居住地**：本后端把
`VineViewBlock` / `VineDrawBlock` 直接绑成块，而那 128B 的 push 是同一对矩阵的 L2 实现（既有后端的
`VsgPipelineFactory.cpp` 注释写得很清楚：`pc.projection == VineViewBlock.proj`、
`pc.modelView == VineViewBlock.view * VineDrawBlock.model`——同一段话，改写一遍）。所以这一片做的事是
把它**真的填上**：`describeAbi` 在 `create` 就拒“填不了的成员”，pass 在每个绘制命令前把声明的每个范围
按名字填好、按声明的阶段与偏移发给 `PushConstants`。

**按名字，不按位置**。push 是唯一一个“字节要装配”的范围（块从 L1 结构体整块拷），所以“哪个成员是什么”
只能来自文本写的名字：

* `projection` → `foldToDeviceClip(camera.projection)`——**与 `VineViewBlock.proj` 逐位同一个值**，折叠
  只写在一处（`api/ViewBlock` 的 `foldToDeviceClip`，M8c-1 从它里面提出来共用）：读 push 的程序与读块的
  程序不能对“画面在哪”有不同看法；
* `modelView` → `camera.view * model`，**顺序就是契约**（模型矩阵在右手）：写反了不是“偏一点”，是每个可
  绘制对象被摆到没人写过的变换上；
* 认不出的名字（`pc.tint`）与尺寸不对的名字（`vec4 projection`）⇒ 在 **`create`** 就拒（一个没人能填的
  声明是一条永远画不出来的管线，拒绝属于布局那一层）；声明了**一部分**是可以的（`mat4 projection;`
  单独出现），与“块只读 L1 结构体的前缀”同一条规则；
* 没有相机 ⇒ 写零（与 `buildViewBlock` 同一个 `present` 事实）： “没有视图”是一个值，不是错误。

**每个可绘制对象一遍**。`modelView` 带着**这个** drawable 的模型矩阵，所以 push 是逐命令写的（不是逐 pass）
——一个 pass 里十个对象就是十次 `vkCmdPushConstants`；范围的偏移、尺寸与阶段全部来自声明，pass 只是把
字节装配进去（`api/ContentPush`），两边不可能对不上。

| 文件 | 是什么 |
| --- | --- |
| `api/ProgramAbi.hpp` / `src/api/ProgramAbi.cpp` | `AbiPushRange` += `members`（`AbiPushMember{name, offset, size}`）：块尺寸的那次遍历本来就知道每个成员在哪，顺手记下来；两个阶段声明同一个范围时，名字/偏移/尺寸也要逐位一致（否则矛盾 ⇒ Malformed） |
| `api/ContentPush.hpp` / `src/api/ContentPush.cpp`（新） | `ContentPushMember{Unknown, Projection, ModelView}` + `contentPushMemberOf` / `contentPushMemberName` / `canFillPushMember` + `packContentPush`（按名字装配，列主序写矩阵）+ `pushStagesOf`（声明阶段 → API flags 的唯一拼写，`ContentPipeline` 也用它） |
| `api/ViewBlock.hpp` / `src/api/ViewBlock.cpp` | 折叠提出来共用：`foldToDeviceClip(sdk_projection)`（`buildViewBlock` 改用它，**同一处**折叠） |
| `api/ContentPipeline.hpp` / `src/api/ContentPipeline.cpp` | `create` 在声明 push 范围时逐成员检查 `canFillPushMember`（填不了 ⇒ 整个程序不建） |
| `api/ContentPass.hpp` / `src/api/ContentPass.cpp` | `serveHalf` 不再拒“声明了 push 的半片”；`recordCommand` 把声明的每个范围填好、按声明发给 `PushConstants`（复用成员缓冲：稳态帧不分配） |
| `api/ContentDraw.hpp` / `src/api/ContentDraw.cpp` | `Draw::pushes`（span，逐声明范围）+ `push_commands()` 计数器；**push 记在 `Commands` 节点的插入序里**（见下） |
| `tests/test_vsg/ContentPushTest.cpp`（新） | 5 条无设备用例：名字→值、与 `VineViewBlock.proj` 逐位相同、`view * model`（并证明另一个顺序是另一条矩阵）、拒绝面（名字/尺寸/零尺寸）、无相机写零 |
| `tests/test_vsg/ProgramAbiTest.cpp` | 引擎前向程序的 push 成员逐条钉住（`projection@0` / `modelView@64`，各 64B，顶点阶段） |
| `tests/test_vsg/ContentPipelineTest.cpp` | +1：填不了的成员在 `create` 就拒；只有 `projection` 的范围可以（尺寸 64） |
| `tests/test_vsg/ContentPassTest.cpp` | +1 真设备像素：**引擎形状的 push-only 程序**（`gl_Position = pc.projection * pc.modelView * pos`），相机 + 非恒等模型矩阵 ⇒ 三角形被缩小并移右（右像素是网格色、左像素是计划清屏色） |

| 规则 | 结论（变异反证全部实测） |
| --- | --- |
| **push 必须紧贴绘制、按插入序记** | 变异 N1（把 push 放回 `StateGroup` 的 stateCommands——vsg 按 slot 记录）⇒ 像素用例红：**命令确实发出去了（vsg 里打印得到我的字节），但 shader 读到的是恒等矩阵**，画面“看着正常”。全屏路径（M5d）一直把 push 加进 `Commands` 节点，这就是原因 |
| **按名字装配** | 变异 N2（按声明顺序：offset 0 给 modelView）⇒ 6 条红（4 条无设备 + 2 条设备） |
| **投影要折** | 变异 N3（直接用 SDK 投影）⇒ 4 条红：几何被裁掉（设备 z 出 [0,1]）、画面=清屏色——§11.16o 同一个坑，这次在 push 上 |
| **`modelView` 的顺序** | 变异 N4（`model * view`）⇒ 4 条红 |
| **尺寸不对的名字也要拒** | 变异 N5（只看名字不看尺寸）⇒ 4 条红（层会把 `vec4 projection` 当成投影矩阵建出管线） |

**这一片留下的口子（登记，不假装解决）**：

* **采样器与块同集合的排布**（M8c-2）：引擎的前向程序把 material/lights/阴影块与 `diffuseMap`/`shadow_map`
  放在**同一个 set 0**，而现在“块与采样输入挤一个 set”被明确拒绝；`diffuseMap` 的白色 fallback 与
  `shadow_map` 的图都还没有生产侧。
* **push 的其它成员**：本片只填 L1 的那两个（`projection` / `modelView`）；全屏光照程序的 128B push
  （`LightPushBlock`）走的是**另一条**路径（M5d，按调用打包），两条路径的成员名不同、互不干扰。
* **相机缺失时的零矩阵**只是“没有视图”的值：pass 照画（几何落在原点）。要不要在上层把“无相机的 pass”
  整个拒掉，属于场景桥再上一层的政策（引擎给每个绘制的 pass 都带相机）。

证据：`test_vsg` 全量 **587 用例 / 91 套件全绿**（+5 用例：`ContentPushTest` 5 条、`ContentPipelineTest` +1、
`ContentPassTest` +1 —— 其中 `ContentPushTest` 是第 91 套）；门禁
一条命令 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、hygiene 0 / 825 文件、
`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、相位 9 行 / 2 次运行全收尾。变异反证
**5/5 红**（N1 push 放回 stateCommands、N2 不按名字、N3 不折投影、N4 顺序写反、N5 不查尺寸）；N1 的失败
形态最有价值：它在“命令发出去了”的情况下仍然红——判据是像素，不是“我发了命令”。

### 11.16ah M8c-2a（2026-09-22）：一个声明的集合可以同时装块与采样图（引擎的 set 0）

§11.16af 让布局跟着声明走，但那时“采样器”仍然只允许住在输入集（set 1）里——那是**重写版自己的**排布的
习惯（一个 pass 采样上一个 pass 的产物），不是文本的规则。引擎的程序不是这样写的：`builtin_forward`
把 material 块与 `diffuseMap` 都放在 **set 0**（binding 0 与 1），`builtin_gbuffer` 同理。这一片把那条
“采样器只能在 set 1”的限制拆掉：**一个集合可以同时声明块（dynamic UBO）与采样图（combined image
sampler）**，谁在哪一个 binding 由文本说了算；只要不把两样东西挤在同一个 binding 上（那是矛盾，事实层就
拒），一个集合是什么就是什么。

**调用方建集，建立的就是声明本身**。`BlockDescriptors` 从“一组块角色”扩成“一个声明的集合”：
`SampledBinding{binding, view, sampler}` 是图的半边（块那半边照旧走 arena + 动态偏移），
`layoutOfShape(shape, sampler_bindings)` 是**这一个集合布局的唯一拼写**，`samplers()` 把图读回来。
层这一边：`declaredSets()`（调用方要建的集合）与 `samplerBindings(set)`（那个集合里必须有哪些图）——
把 `blockSets()`（只有块）换成它们的理由很直接：引擎的 `skybox` 程序**一个块都没有**，只有一个 `skyMap`，
它的 set 0 照样是“一个要建的集合”。**输入集仍是 pass 自己的**：`declaredSets()` 里排掉
`kInputSet`（除非那个 set 声明了块），因为它的图不是一个调用方能有的东西——它们是 pass 的输入，按声明的
顺序绑（`Data::sampled` 那套照旧）。

**“集合对得上”现在有两半**：`serveHalf` 除了比块形状（逐位），还比**采样 binding 列表**——一个“缺了那张
图的集合”会被拒并点名，而不是绑上去让 shader 读某个未写过的 binding。`acquire` 的输入数规则也收窄到
**输入集**：set 0 的 `diffuseMap` 不需要 pass 的输入图（它是材料/fallback 的事），以前的规则会把它误拒。

| 文件 | 是什么 |
| --- | --- |
| `api/BlockDescriptors.hpp` / `src/api/BlockDescriptors.cpp` | `SampledBinding`（图 + 采样器）；`layoutOfShape(shape, sampler_bindings)`；`create` / `forAbi` 收一列图；`makeSet` 把图按**声明的 binding** 放进同一个 set；`samplers()` 访问器 |
| `api/ContentPipeline.hpp` / `src/api/ContentPipeline.cpp` | `kInputSet` 常量（“输入集”这个概念现在有了名字）；`describeAbi` 允许采样器在任何 set（拒 `samplerCube` / `OtherSampler` / 数组）、禁 block+sampler 同 binding；每个 set 的布局带上图；`declaredSets()`（= 有块的 set + 非输入集的图 set）、`samplerBindings(set)`；`acquire` 的输入数规则只对输入集 |
| `api/ContentPass.cpp` | `serveHalf` 多比一半年（`sameSamplers`），并把“输入集里的采样器数 ≤ 输入数”说清楚 |
| `tests/test_vsg/BlockDescriptorsTest.cpp` | +1：一个 set = 块 @0 + 图 @1（布局两种绑定、`samplers()`、**set 的两个描述符各自的 binding**、bind 仍只给块一个动态偏移） |
| `tests/test_vsg/ContentPipelineTest.cpp` | 采样器在块集里现在**合法**（并读回 `samplerBindings`）；`samplerCube` 仍拒 |
| `tests/test_vsg/ContentPassTest.cpp` | +1 真设备像素：引擎形状的程序（material @(0,0) + `map_tex` @(0,1) + push），图来自**另一个目标**的清屏色——fragment 把两者**相乘**，所以“图没绑”或“材料没到”各自会给出另一个颜色；另含“缺图的集合被拒且上报” |

| 规则 | 结论（变异反证） |
| --- | --- |
| **布局要声明两种绑定** | 变异 M1（`layoutOfShape` 忘掉采样 binding）⇒ 纯套件**绿**、验证层 **2 条 VUID**：布局类主张的判据是验证层（与 §11.16r 同一条经验） |
| **描述符要落在声明的 binding** | 变异 M4（图放进 binding 0）⇒ 纯套件红（用例检查 set 的描述符 binding）+ 验证层 2 条 |
| **“集合对得上”含图那半** | 变异 M3（只比块形状）⇒ 像素用例红：缺图的集合被绑上去 |
| **混合是合法的** | 变异 M5（恢复“块集里不许有采样器”）⇒ 像素用例红（层直接把程序拒了） |

**这一片留下的口子（登记，不假装解决）**：

* **`diffuseMap` 的白色 fallback**（M8c-2b）：材料还没有纹理（`Material::texture()` 有了，GPU 那半边没
  有），所以“没有图 ⇒ 采样白”还需要一张 1×1 的图（上传机制：数据背书的 `vsg::Image` 在本版 vsg 里
  `requiresDataCopy` 无人消费，要自己做一次 staged 上传），这一片先用**真实存在的附件**当图证明机制。
* **输入图仍只按 set 1 的规则绑**：引擎的 `shadow_map` 在 set 0（binding 3），G-buffer 采样在 set 0 的
  0..3——按名字把它们指到 pass 的输入图是 M8c-2b 的活（policy：`diffuseMap` → 材料/白、`skyMap` → 环境、
  其它名字 → pass 的输入，按声明顺序）。
* **`samplerCube` 仍拒**：立方体视图（天空盒/立方体漫反射）随纹理那一并做。

证据：`test_vsg` 全量 **589 用例 / 91 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID /
0 SYNC-HAZARD**、hygiene 0 / 825 文件、`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、
相位 9 行 / 2 次运行全收尾。变异 **M3 / M5 纯套件红**，**M1 / M4 由验证层抓住**（各 2 条 VUID；干净树上
这三条用例的 VUID 基线是 0，实测核对过）。

### 11.16ai M8c-2b（2026-09-22）：没有贴图的材料采样"白"——fallback 是值，不是错误

§11.16ah 让"一个集合同时装块与图"成立，并用**真实存在的附件**证明了机制；但引擎的程序会**无条件**采样
`diffuseMap`（`texture(diffuseMap, uv)`），而引擎的 ABI 对"材料没有贴图"给的是**值**：采样结果是白色，
于是着色就是材料自己的颜色。所以那个 binding 不能空着——**没写过的描述符不是"没有贴图"，是未定义数据**
（开着验证就是一条 VUID）。这一片补上这个 fallback，顺手把"给它找内容"这件事做干净。

**为什么不走上传**。内容是常量、图像是一个纹素，没什么可暂存的：`fill()` 在两道屏障之间录一条
`vkCmdClearColorImage`（vsg 的 `vsg::ClearColorImage`），图像就此变白并停在
`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`——**描述符声明的那一个布局**（和"写过内容的图像"最后待的
布局相同）。清的源布局是 `UNDEFINED`：图像的全部内容都由这条命令写，之前是什么都该丢掉。它不是渲染通道
命令（不需要挂载点、不需要 pass 处于活动状态），所以调用方把它加进**帧的命令图**、放在内容之前；每帧重录
是幂等的，代价是一次 1×1 的清。

**工具与政策分开**。`api/WhiteImage` 只是"那个值"（图像 + 视图 + 采样器 + `fill()` 节点，且**不需要设备**：
`vsg::Image` / `ImageView` / `Sampler` 到 `Context` 编译前都只是 create-info，和 `ContentPipeline` 同一个
理由）。**谁把哪个 binding 指向它**是政策：`diffuseMap` → 材料自己的贴图，没有贴图就是白（本片）；材料
真正带贴图、以及按名字把 pass 的输入图指到声明的 binding（`shadow_map` / G-buffer 采样）是"三张表的生产
侧"和贴图那一并做的活（引擎还没有可用的 GPU 贴图路径——`Material::texture()` 有了，缓存与上传没有）。

| 文件 | 是什么 |
| --- | --- |
| `api/WhiteImage.hpp` / `src/api/WhiteImage.cpp`（新） | `WhiteImage::create()`（无设备）→ `view()` / `sampler()` / `fill()`；`fill()` = 屏障（`UNDEFINED → TRANSFER_DST`）+ `ClearColorImage`(白) + 屏障（`TRANSFER_DST → SHADER_READ_ONLY`）；每次 `create()` 给一套**独立**的对象（一个集合绑的东西不与他人共享） |
| `tests/test_vsg/WhiteImageTest.cpp`（新，无设备） | fallback 就是声明集合需要的那三样；`fill()` 的结构（屏障 / 清 / 屏障）、清的是白、`TRANSFER_DST`、覆盖整张图；两次 `create()` 不共享视图 |
| `tests/test_vsg/ContentPassTest.cpp` | +1 真设备像素：引擎形状的程序（material 块 @(0,0) + `diffuseMap` @(0,1) + push），材料**没有**贴图 ⇒ 图那一半绑 fallback，`fill()` 先于内容录进帧 ⇒ 画面 = 材料的 diffuse（白 × 材质） |

| 规则 | 结论（变异反证） |
| --- | --- |
| **fallback 必须是白的** | 变异 N1（清成黑）⇒ 纯套件红 2 条（无设备用例查了清的四个通道 + 像素用例变黑） |
| **清完必须停在描述符声明的布局** | 变异 N2（第二道屏障留在 `TRANSFER_DST`）⇒ 纯套件绿、验证层 **4 条 VUID**：像素在 lavapipe 上照样"看着对"，判据是验证层（§11.16r 的老经验） |

**这一片留下的口子（登记，不假装解决）**：

* **材料真正带贴图**：`Material::texture()` 是有的，GPU 侧没有（缓存 + 上传 + 采样器选择）——贴图那一片。
* **按名字指派输入图**：`shadow_map` / G-buffer 采样按声明的 binding 指到 pass 的输入（policy：`diffuseMap`
  → 材料/白、`skyMap` → 环境（仍拒）、其余名字 → pass 的输入，按声明顺序）；随"三张表的生产侧"一起做，
  因为**谁把哪个输入图给哪条声明**是那一层的事实。
* **`samplerCube`**（`diffuseMap` 的立方体变体、`skyMap`）仍拒：立方体视图随贴图那一并做。

证据：`test_vsg` 全量 **591 用例 / 92 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID /
0 SYNC-HAZARD**、hygiene 0 / 828 文件、`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、
相位 9 行 / 2 次运行全收尾。变异 **N1 纯套件红**（2 条）、**N2 由验证层抓住**（4 条 VUID；干净树上这两条
用例的 VUID 基线为 0，实测核对过）。

### 11.16aj M8c-3（2026-09-22）：名字即来源——`shadow_map` 到达它声明的 binding

§11.16ai 给"没有贴图"定了值；这一片给"没有阴影"定值，并把**哪个名字的图从哪来**变成一条可断言的策略。

**问题**：引擎的内容程序把 `shadow_map` 声明在**自己的集合里**（前向：set 0 / binding 3，挨着 material@0 与
`diffuseMap`@1），而那张图是 **pass 的输入**——计划里由目标自己的声明解析出来（`resolveShadow`：目标是 map +
深度可采样 + 生产者公布了怎么读）。caller 建那个集合（块 + 图），但它凭什么知道 binding 3 该放哪张图？
"第一个可采样的深度"是这条路上已经量过的缺陷（一个 G-buffer 也有深度——参考实现就是把 G-buffer 的深度当成太阳的
map）。**光靠位置不行，得靠名字**。

**策略（`api/ContentImages`，唯一拼写）**：

| 名字 | 来源 | 今天的生产者 |
| --- | --- | --- |
| `diffuseMap` | 材料自己的贴图，没有就是那张白图 | caller（§11.16ai 的 `api/WhiteImage`） |
| `skyMap` | 帧的环境图 | **环境没落地 ⇒ `ContentPipeline::create` 按名字拒**（比编一个替身诚实） |
| `shadow_map` | pass 解析出的那张 map | `shadowImageOf`（本片）+ "程序读不到"的上报 |
| 其余名字（`albedo_tex`、`screen_tex`……） | pass 声明的输入，按声明顺序 | caller（场景桥落地时用它走输入表） |

**`shadowImageOf` 不问位置，问计划**：它按 `resolveShadow` 的同样三个事实（同一个 light 身份 + 深度可采样 +
生产者公布过矩阵）走输入表，取**caller 真的提供过的**那个深度视图，配 `ContentPipeline::depthSampler()`（NEAREST：
深度比较读的是光栅器写下的精确值）。pass 没解析出 map ⇒ 返回 false，caller 绑**白色替身**——引擎的程序只在
`VineShadowBlock::params.x` 打开时读它，而白色的值是那次乘法的单位元。

**pass 侧能断言的只有它自己知道的事实**：程序声明了 `shadow_map` 而这条 pass 没解析出 map ⇒ 不拒（白替身兜住），
但**反过来的情形必须说话**：pass 解析出了 map、程序的名字表里没有它 ⇒ 画照画（"一个 drawable 不该因为阴影而消失"），
并**每个半片报一次**（`UnsupportedRequest`："the pass declared a shadow, but the program shading it declares no
`shadow_map` sampler…"）。这是旧实现已经认下的判据（§SceneBridgePipeline 的那条），搬到新层后不再依赖
"set 0 / binding 3"这种写死的坐标——**名字**才是契约。

**顺手抓到并修掉的稳态帧缺陷**：`BlockStorage::writeMaterial` 在 HIT（同一 material + 同一 revision，稳态帧的
常态）时返回 `{Unchanged, offset 0, 0}`，而 `ContentPass::recordCommand` 会照用那个 offset ⇒ 一个帧里**第二次**
绘制同一材料时绑到 offset 0——那是存储缓冲的第一块（view 区），着色读到的 material 全是 0（实测：(0,0,0)）。
之前没有用例把"第二帧的记录"提交上去过（§11.16p 的第一个用例只记录、不断言第二帧的画面），本片的像素帧里同一
材料被画三次，第二次就现形。修法：HIT 也报出**上一次写留下的那块**（`offsetOf(slot, copy)`——arena 的 slot
与 copy 本来就是给读的一侧准备的），并把"hit 也指名"钉进 `BlockStorageTest`。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentImages.hpp` / `src/api/ContentImages.cpp`（新） | `InputImages`（从 `ContentPass.hpp` 搬来，图的事归图）、`ImageOrigin` + `imageOriginOf`（名字表）、`samplesShadowMap(abi)`、`shadowImageOf(pass, inputs, depth_sampler, out)`（**无设备**：图是 create-info，计划是值） |
| `ContentPipeline` | 名字为 `skyMap` 的采样器在 describeAbi 里被拒（"环境没落地"这件事有一个按名字的拼写） |
| `ContentPass` | `reportShadowNotSampled`（每半片一次）：pass 解析出 map 而程序读不到 ⇒ 上报，不拒 |
| `BlockStorage` | HIT 的 offset = 上一次写留下的那块（稳态帧的第二笔绘制读的就是它） |
| `tests/test_vsg/ContentImagesTest.cpp`（新，无设备，3 条） | 名字表（含"名字不是模式"：`diffuseMap2`/`shadowMap`/`SkyMap` 都是 Input）；`samplesShadowMap` 不看 set/binding；`shadowImageOf` **按身份挑**（两个可采样深度、map 在后/在前/换别的 light/没公布矩阵/没提供视图） |
| `tests/test_vsg/ContentPassTest.cpp`（+1 真设备） | 一帧五趟：两个 depth-only 可采样目标（一个有 light 声明、一个没有）+ 同程序三趟（[source,map] ⇒ 读 0.25；[source] ⇒ 白替身；不给 map 的程序 + [source,map] ⇒ 上报一次且照画） |
| `tests/test_vsg/ContentPipelineTest.cpp` | 2D `skyMap` 与 cube 一样被拒 |
| `tests/test_vsg/BlockStorageTest.cpp` | HIT 的 offset 与 Allocated/Rewritten 的相同（改法被变异住的那条） |

**变异反证（5/5 红，都在纯套件里，不带设备）**：

| 变异 | 结果 |
| --- | --- |
| M1 挑选器改成"第一个提供过深度的输入"（就是那个已量过的缺陷） | 无设备用例红（挑到 source 的 0.0）+ 像素红（画面变黑） |
| M2 建层时不再拒 `skyMap` | 无设备用例红（`ADeclarationThisBackendCannotFillIsRefused`） |
| M3 "程序读不到阴影"不上报 | 像素红（上报计数断言）——纯套件抓不到（这就是它必须上报的原因） |
| M4 HIT 返回 offset 0（复现原缺陷） | `BlockStorageTest` 红 + 像素红 |
| M5 `samplesShadowMap` 变成"声明了任何采样器就算" | 无设备用例红 + 像素红（报告该不响的时候响了） |

证据：`test_vsg` 全量 **595 用例 / 93 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、
hygiene 0 / 831 文件、`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、相位 9 行 / 2 次运行全收尾。

**这一片留下的口子（登记，不假装解决）**：全屏路径的输入集仍是"按计数依次绑定"，引擎的 shadow ABI 给全屏程序
声明的槽位是 `color_count+1`（map）与 `color_count+2`（块）——`layout` 跟着全屏程序的声明走（含那个块用动态偏移
绑）是下一片；材质贴图的 GPU 侧（缓存 + 上传 + 立方体视图）随之；`Input` 那一行要等场景桥来消费。

### 11.16ak M8c-4（2026-09-22）：全屏集合就是程序自己的声明——引擎的带影延迟光照端到端

M8c-3 把"哪个名字的图从哪来"变成策略；这一片把**全屏程序声明在哪个号上**变成策略。
引擎的 `shadowedDeferredLightProgram` 把 `shadow_map` 声明在 **binding 5**、
`VineShadowBlock` 在 **6**（源的四张颜色占 0..3，源自己的深度本应占 4），而它的
G-buffer 没有可采样深度——"按计数依次绑定"（M5d 起的行为）会把 map 放到 4，着色器
在自己写死的 5 上读到未定义数据。判据一句话：**全屏集合的每个 binding 都能从程序的
声明反查出来**。

**建层（`ContentPipeline::createScreen`）**：收 `ProgramAbi`（`describeAbi` 共用，
外来块 / 非 std140 / 超尺寸 / `count != 1` / `skyMap` 都按名字拒），另拒两件事：set
≠ 0 的声明（全屏只有 set 0 可绑）与不是阴影的块（全屏程序的光走 push）。
`sampledSetLayout` 的全屏分支：颜色 0..c-1 → 源自己的深度（从 c 起**跳过文本已经
点名的号**）→ 文本声明的采样器**按它写的号**→ 声明的块按它写的号、类型是**静态
UBO**（一次全屏调用只有一个块，pass 把它的 offset 烧进描述符，绑定不带动态偏移），
排序 + 去重后逐个 addBinding。`declared_sets` 保持空：全屏的集合是 **pass 的**。

**录制（`ContentPass::recordScreenDraw`）**：源按**身份**（`draw.source`）而不是位置
找；map 用 `shadowInputIndexOf`（计划解析出的那张）；拒绝全部按名字：没有源、有既非
源也非 map 的输入、map 那个输入带颜色、程序声明 `shadow_map` 而 pass 没有可读的
map。调用自己的阴影块每调用写一次（`packShadowBlock` + `writeShadows`），在它声明的
槽位上静态绑；集合按布局的 binding 逐个填：块 → 存储缓冲的静态 UBO、map → 计划的
那张图、`binding < colours` → 源的附件 i、否则 → 源的深度、再否则按名字拒。键的计数
只用**源的**颜色与它自己的深度（map 与块是文本的声明，不随 pass 变）。

**证据（真设备）**：一帧七趟——四附件 G-buffer（引擎自己的格式：RGBA8 / RGBA16F /
RGBA8 / RGBA16F）+ 两张 depth-only 可采样 map（各自一趟只清屏的 pass，**必须录进
帧**：不录就对着镜像里的旧内容比深度）+ 引擎两个变体共四趟全屏光照：map 清到远平面
⇒ 阳光到达（64,32,26）、清到 1.0 ⇒ 只剩环境（13,6,26）、无 map 的变体 ⇒ 与前者同值、
给了 map 而程序不读 ⇒ 照画 + 一次上报。写入者的法线附件 alpha = 材料的
shininess/256（引擎的 G-buffer 约定；本夹具的材料 shininess = 0）——这是下一条的探针。

**顺手抓到并修掉的真缺陷：动态命令一直开着混合**。`makeDynamicStateCommand`（M7）
写死 `blendEnable = VK_TRUE`（"这个引擎混合恒开"——L2 每顶点 opacity 路径的规则），
而混合的**使能**是这条命令投递的、bake 的常量不再决定 ⇒ 多附件 pass 的每个附件都被
自己的 alpha 缩放。L2 早就在自己的 G-buffer 上量过同一件事并写下了规则
（`applyOpaqueBlendForAttachments`："法线附件带 shininess/256（≈0.125），混合把存下
的法线缩到 12.5%"），新路径的命令把这个缺陷重新造了一遍。修法：`color_attachments >
1` ⇒ 全部 `blendEnable = VK_FALSE`、因子 ONE/ZERO（与 L2 那条规则逐位一致）；单附件
仍恒开（opacity 路径）。无设备用例
`DynamicStateTest.SeveralColourAttachmentsAreDeliveredUnblended` 钉住两侧，
`ContentDrawTest` 里"混合恒开"的旧断言改为单附件规则。判据的强度：本夹具 shininess
= 0 ⇒ 混合会把法线**整体抹掉**（透明黑底 × 0），阳光消失——变异 M5 把规则退回 `> 4`
时像素直接回到 (13,6,26)。

**另一个坑（夹具侧，写进 memory）**：`vine::math::Mat4d` 默认构造 = **单位阵**（不是
零）——生产者矩阵只写了 (0,0)/(1,1)/(2,3)/(3,3) 时 (2,2) 仍是 1，光的 z 变成
`1*(-2)+5.5 = 3.5` ⇒ `frag = 1-(3.5*0.5+0.5) = -1.25`（RGBA8 夹到 0）⇒ 所有"看着像
被阴影"的结果都从这一项来。要点：**先读默认值再写矩阵**。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentPipeline`（`.hpp`/`.cpp`） | `createScreen(abi, shaders[, settings])`；set ≠ 0 / 非阴影块两条拒绝；`sampledSetLayout` 的全屏分支（声明号就是绑定号；深度槽跳过已点名的号；块 = 静态 UBO）；`declaredSets` 保持空（全屏集合是 pass 的） |
| `api/ContentImages` | `shadowBindingOf` / `shadowInputIndexOf`（`shadowImageOf` 改为经它们表达） |
| `api/ContentPass` | `recordScreenDraw` 重写：源按身份、map 按计划、按名拒绝、每调用一个阴影块、集合按布局的 binding 逐个填 |
| `api/StateCommands.cpp`（+ 头注） | 多附件 = 不混合（L2 规则的第二次落地）；单附件仍恒开 |
| `tests/test_vsg/ContentPassTest.cpp` | 引擎带影/无影延迟光照七趟真设备像素 + 一次上报 |
| `tests/test_vsg/ContentPipelineTest.cpp` | 引擎带影光照声明的槽位（无论源深度是否可采样，map 都在 5、块都在 6）+ set ≠ 0 与外来块两条拒绝 |
| `tests/test_vsg/DynamicStateTest.cpp` | 多附件不混合 / 单附件恒开（无设备） |
| `tests/test_vsg/ContentDrawTest.cpp` | 旧"混合恒开"断言改为单附件规则 |

**变异反证（5/5 红）**：

| 变异 | 结果 |
| --- | --- |
| M1 声明的采样器放到"下一个空位"（就是那片已量过的缺陷） | 无设备用例红 + 像素红（按名拒绝 ⇒ 用例的断言） |
| M2 建层不再拒 set ≠ 0 | 无设备用例红 |
| M3 建层接受非阴影的块 | 无设备用例红 |
| M4 块描述符只认动态 UBO（静态布局下没人填） | 像素红（按名拒绝 ⇒ 用例的断言） |
| M5 多附件规则退回 `> 4` | 无设备用例红 + 像素红（法线被抹掉 ⇒ 阳光消失） |

证据：`test_vsg` 全量 **598 用例 / 93 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：
**0 VUID / 0 SYNC-HAZARD**、skipped=0、hygiene 0 / 831 文件、`check_diagnostic_formats.py`
0 / 39、`check_doc_symbols.py` 通过、相位 9 行 / 2 次运行全收尾。

**这一片留下的口子**：材质贴图的 GPU 侧（缓存 + 上传 + 立方体视图）→ 变体的 define 进
管线身份 → 三张表的生产侧（活的 SDK 对象 + 修订 + 退役）。

### 11.16al M8d-1（2026-09-22）：材质贴图的 GPU 侧——缓存、上传、立方体视图

§11.16ai 给"没有贴图"定了值（白），这一片把"有贴图"接上：**一张 texture 的 image / view / sampler**
（`api/MaterialImages`），以及它怎么到达设备。

**为什么必须在这一层**：引擎的 `Texture` 是**逻辑描述**（种类、尺寸、格式、mip 数、逐面的 `imaging::Image`
源图、`revision`），它按设计**不持有任何 GPU 资源**——"能建纹理的后端物化这个描述"是它写下的分工。于是
`MaterialImages` 就是那个物化：`acquire(texture, reason)` 回答**这套图与这个采样器**，caller 把它绑到程序
声明的 `diffuseMap` 槽上（引擎的 set 0 = material 块 + 图，M8c-2a 已经能让一个声明的集合同时装两者）。

**上传：不自己搬运，靠数据背书的 image + viewer 的传输步**。`makeImage` 把每个面的每一级 mip 拷进一条
staging 字节链（**mip-major 交错**：vsg 的拷贝区域按"一级里的所有层连续"读，引擎的存储是"一层里所有级连续"，
多层纹理必须在这里交织——错了是**静默**的，字节总数一样、每个拷贝区域都合法，只是每个面采到别人的数据），
用**一个 texel 宽**的元素类型把它包成 `vsg::Data`（元素宽 = stride，vsg 的步进就是一片 face 的大小），
数组的**维数 = 层数**（`TransferTask` 对 cube 从 DATA 的 depth 取 arrayLayers），cube 的
`properties.imageViewType = CUBE`（ImageView 的构造从 DATA 读它，事后赋 `view->viewType` 会被覆盖），
`flags |= CUBE_COMPATIBLE`。数据背书的 image 由 viewer 的传输步上传（`RecordAndSubmitTask::submit` 里
`transferData(TRANSFER_BEFORE_RECORD_TRAVERSAL)`）——**测试的手动驱动也走这条**，所以不需要额外的上传节点
（§11.16ai 记的"`requiresDataCopy` 无人消费"指的是那个遗留标志；真正生效的是 `Data::dirty` 那套）。

**缓存的三条规则**（L2 的 `VsgTextureCache` 是参考，这里重写为自己的拼写）：
1. **键是地址 + 修订**：条目记住构建时的 `Texture::revision()`，重填过的纹理下次 `acquire` 重建（没有它就会
   一直采旧像素）；
2. **条目持住键**：地址是键而缓存看不见销毁，条目持一个 owning 引用，新纹理复用同一地址时不会拿到死纹理的图；
   `releaseAbandoned()` 释放"只剩缓存自己持有"的条目；
3. **有界**（256 条，超限淘汰最旧的）+ **fallback 是值**：不可用的纹理给白图而不是空视图，**按种类**给
   （cube 槽不能绑 2D 视图——那不是"没贴图"，是非法描述符）；2D 白复用 `api/WhiteImage`（清屏、不是上传），
   白 cube 走与真纹理同一条路（六面一个 texel）。

**顺手放开的拒绝**：`ContentPipeline` 原先按"没有视图的采样器种类"拒掉 `samplerCube`；立方体视图落地后这条
只剩 `OtherSampler`（`sampler3D` 一类）与 `count != 1`。

| 文件 | 是什么 |
| --- | --- |
| `api/MaterialImages.hpp` / `src/api/MaterialImages.cpp`（新） | `SamplerImage acquire(texture, reason)`、`white()` / `whiteCube()`、`has` / `count` / `releaseAbandoned` / `clear` / `setMaxAnisotropy`；设备无关（create-info），决策复用 `VsgSceneRules` 的那一份（`classifyTexture` / `TextureReject` / `vkFormatFor` / `levelExtent` / `textureDataMatchesExtent` / `anisotropyFor`——**一个决策只有一处拼写**） |
| `api/ContentPipeline.cpp` | 放开 `samplerCube` 的拒绝（视图有了），拒绝面收敛为 `OtherSampler` / 非 1 个 / `skyMap` |
| `tests/test_vsg/MaterialImagesTest.cpp`（新，无设备，10 条） | 缺纹理 → 共享白（不入缓存）；未填完不入缓存；**fallback 按种类**（cube → 白 cube，view type 可断言）；无 Vulkan 对应格式 → 拒；同一纹理只建一次；重填重建；条目持住纹理直到放弃；clear 清空（含 fallback，重建是新对象）；超限淘汰最旧；cube = 六层 depth + CUBE 视图 + 视图类型写在 DATA 上 |
| `tests/test_vsg/ContentPassTest.cpp`（+3 真设备） | 四色 2x2 贴图 × 四边形 UV ⇒ 画面四象限 = 四个 texel；六色 cube ⇒ 五个方向读五个面；同纹理重填 ⇒ 第二趟画新颜色 |
| `tests/test_vsg/ContentPassTest.cpp`（夹具） | `pipelineFor` 把通道的绑定号改成**后端的规范编号**（`StreamUploads::bindingOfCanonical`：位置 0、法线 1、纹理坐标 **2**、颜色 3）——不是通道在表里的次序，也不是 shader 的 location（纹理坐标槽是 8）。按次序绑会让 location 8 的 `vec2` 永远读 (0,0) |

**实测踩到/量到的四件事**：

* **绑定号不是 location 也不是次序**：L1 的纹理坐标槽是 location 8，而后端的绑定号是 2（`bindingOfCanonical`）。
  夹具原先按"第几个通道就绑几号"，位置/法线恰好一致、纹理坐标就错位——表现是"贴图上传了但画面是四个 texel 的
  平均"（uv 恒为 (0,0)，重复寻址 + 线性过滤在角上正好平均四格）。
* **1x1 的 cube 面用线性过滤会在接缝处互渗**（1x1 面在面内任何点都不是 texel 中心）⇒ 面的颜色只在面内部纯；
  夹具用 4x4 纯色面。
* **换行的注释会被编译器当真**：写长中文注释时行被折在检查之外，续行没有 `//` ⇒ "unknown type name"。写完先 build。
* **变异脚本的恢复断言会误伤**：M4 的还原串 `return white();` 在文件里出现 4 次 ⇒ 断言失败、脚本中止、文件留在
  **变异态**（本片的 M4 就是这样，手动还原后才继续）。规则：还原串必须带**唯一上下文**，或者先存原文再整段写回。

**变异反证（5/5 红）**：

| 变异 | 结果 |
| --- | --- |
| M1 staging 不再按层交错（每层都写同一个槽 ⇒ 只剩最后一层） | 像素红（cube 五个方向变成同一个面） |
| M2 cube 不写 view type（六层 2D 数组视图） | 无设备用例红（视图类型）+ 像素红 |
| M3 缓存不看修订 | 无设备用例红（重填重建）+ 像素红（第二趟还是旧颜色） |
| M4 被拒的 cube 退回 2D 白 | 无设备用例红（fallback 按种类） |
| M5 条目永不放弃 | 无设备用例红（放弃即释放） |

证据：`test_vsg` 全量 **611 用例 / 94 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID /
0 SYNC-HAZARD**、skipped=0、hygiene 0 / 834 文件、`check_diagnostic_formats.py` 0 / 39、
`check_doc_symbols.py` 通过、相位 9 行 / 2 次运行全收尾。

**这一片留下的口子**：变体的 define 进管线身份（`VINE_DIFFUSE_MAP` 与 texcoord kind 取决于几何与材质——
管线身份现在是 `(program, revision, layout)`，还差 define）；三张表的生产侧（活的 SDK 对象 + 修订 + 退役）。

### 11.16am M8d-2（2026-09-22）：程序的文本是好几份程序——变体的 define 进管线身份

§11.16al 让"有贴图"到达设备；这一片处理它引出的**身份**问题：同一份程序文本，材质有贴图与没有贴图时
被编译成**两种文本**——若管线身份还是 `(program, revision, layout)`，一趟里两种 drawable 就会共用一层，
其中一种必然被画成另一种。

**问题在引擎的文本里就写着**：`builtin_forward.vert` 把 texcoord 输入与采样器放在 `#ifdef VINE_DIFFUSE_MAP`
里，槽的种类还用 `#error` 把"没有 kind"钉死，并在 `#pragma import_defines (VINE_VERTEX_COLOR,
VINE_DIFFUSE_MAP, VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE)` 里**点名**它可能收到的开关。vsg 的预处理器只对
**源里列出**的名字发 `#define`——所以"这几种文本"不是调用方的自由选择，而是程序自己声明的：什么开关有效，
由文本写死；开关取什么值，由 drawable 的**事实**决定。

**规则只有一处拼写**（`api/ProgramVariant`，`variantOf(材质事实, 几何事实)`）：

1. **恒有且只有一个 texcoord kind**：几何的 `TexCoord0` 通道 3 个分量 ⇒ `VINE_TEXCOORD_CUBE`，否则
   `VINE_TEXCOORD_UV`——因为任何声明 texcoord 槽的文本都得能在"没有贴图"时编译通过（引擎的 `#error` 就是
   为这件事写的），kind 不是"有贴图才有"的开关；
2. `VINE_DIFFUSE_MAP` ⇔ 材质有贴图（`MaterialFacts::texture != nullptr`）；
3. `VINE_VERTEX_COLOR` ⇔ 几何**自己写下**了颜色通道——派生的"白色载体"不算（那是后端的替身，不是作者的数据）。

`bits()` 是它的身份号（进管线键），`describe()` 是它的诊断文字（拒绝时读它，而不是让读者去猜三个布尔）。

**身份进三处**：

* `core::PipelineKey += std::uint32_t variant`（相等、散列、审计表都带上；键**类型**数不变，仍是 8 行）；
* `api/ContentPipeline::Shaders += std::vector<std::string> defines`——**一份清单喂两侧**：ABI 扫描用它决定
  "文本这次真的声明了什么"，`compileStage` 把它交给 vsg 的 `compiler.compile(stage, defines)`。两者读同一份
  `out.shaders.defines`，所以布局与模块不可能对不上（M5 变异证明扫描那一侧也吃 define：没有它，带贴图的
  程序扫不出 (0,1) 的采样器）；
* `api/ContentPass::Scope::Entry += ProgramVariant variant`（放在**最后**，旧聚合初始化照旧编译）。

**pass 侧的判断**：`recordCommand` 先查材质（变体的输入之一），由**事实**算出
`variantOf(材质, 几何)`，再按 (program, revision, layout, **variant**) 找 half——拒绝时分三种说清：
程序不在/pass 未编译、布局不合、以及"这趟没有服务于该变体的 half"（三条路径的修法不同，合成一句会把读者
引到错的半边）。命中后 `record.key.variant = variant.bits()`，状态组与池都按它分开。

**顺手修掉的老洞**：`serveHalf` 原先**每趟只对第一个 content half 调一次**（"the pass' content half"），
`recordCommand` 随后把它的集合绑给**每一条** draw——单 half 的趟看不出来，M8d-2 一上来就现形：贴图 half 的
声明集合有 2 个描述符（块 + 图），无贴图 half 的布局只有 1 个，后者绑前者 ⇒ 真设备上
**VUID 00358 + 08600**（画面却是对的——lavapipe 不管校验，门禁才管）。修法：**每条命令 serve 它自己穿过
的那个 half**，集合只来自它；这也一并堵上"一趟里两个不同 (program, revision, layout) 的 half"的同一个洞。

**再记一个静默的**：`MaterialFacts.texture` 原先写在 `out = MaterialFacts{}` 重置**之前**，被清零——
`variantOf` 于是永远说"没贴图"，无设备用例全绿、只有真设备用例的打印看得见
（`textured.diffuse_map=0`）。事实结构体在函数尾部整体赋值时，任何"提前赋值"都是死代码。

| 文件 | 是什么 |
| --- | --- |
| `api/ProgramVariant.hpp` / `src/api/ProgramVariant.cpp`（新） | `diffuse_map` / `vertex_color` / `cube_texcoord`、`bits()` / `defines()` / `describe()`、`==`/`!=`、`variantOf(材质, 几何)` |
| `core/Keys.hpp` / `src/core/Keys.cpp` | `PipelineKey += variant`（相等 + 散列 `mix(key.variant)` + 审计行） |
| `api/ContentSources.hpp` / `.cpp` | `MaterialFacts += texture`（`raw_ptr<const Texture>`）；`buildProgramFacts(program, variant, facts)` 重载（`out.shaders.defines = variant.defines()`，扫描按它进行）；顺带修 texture 赋值次序 |
| `api/ContentPipeline.hpp` / `.cpp` | `Shaders += defines`；`compileStage` 把它交给 `compiler.compile` |
| `api/ContentPass.hpp` / `.cpp` | `Scope::Entry += variant`（最后一位）；`recordCommand` 算变体、按四元组配 half、三种拒绝、`record.key.variant`；**每条命令 serve 自己的 half** |
| `tests/test_vsg/ProgramVariantTest.cpp`（新，无设备，3 条） | define 名单 + 8 个 `bits()` 互异；规则（裸 / UV / cube / 自己写下的颜色 vs 派生）；**引擎自己的 `forwardProgram()` 逐变体**：无 define ⇒ (0,1) 处没有采样器、UV kind ⇒ `Sampler2D`、cube kind ⇒ `SamplerCube`，而**片元文本在两变体间逐字节相同** |
| `tests/test_vsg/ContentPassTest.cpp`（+1 真设备） | 一趟、两画、同一程序的两个 half（只差 variant）⇒ 左半 = 贴图洋红、右半 = 材质自己的 0.25/0.5/0.75；M8d-1 的三条有贴图用例各自声明自己 half 服务的变体 |

**变异反证（5/5 红）**：

| 变异 | 结果 |
| --- | --- |
| M1 `variantOf` 不看材质贴图（恒 false） | 规则用例红 + 真设备用例红 |
| M2 配 half 时不比变体（恒 true） | 真设备用例红（无贴图半边被涂上贴图） |
| M3 键不带变体（恒 0） | **段错误**（状态组/池被两变体共用，拿回错层的管线与集合） |
| M4 define 不进编译（`compile(stage, {})`） | 真设备用例红（带贴图的半边编进了 `#else` 分支） |
| M5 事实扫描丢 define（`shaders.defines.clear()`） | 引擎逐变体用例红 + 真设备用例红（ABI 里没有采样器） |

证据：`test_vsg` 全量 **615 用例 / 95 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID /
0 SYNC-HAZARD**、skipped=0、hygiene 0 / 837 文件、`check_diagnostic_formats.py` 0 / 39、
`check_doc_symbols.py` 通过、相位 9 行 / 2 次运行全收尾。

**这一片留下的口子**：pass 的**输入集合（set 1）仍按第一个 content half 的层构建**——设计上写在
"同一种 kind 的 halves 用同一份 recipe 建采样布局，所以彼此兼容"（`makeInputSet` 的注释）；引擎今天的
三个变体都不碰输入集合，但**用户程序**若按变体改输入声明，就需要按 half 建输入集合（或明确拒绝）；
三张表的生产侧（活的 SDK 对象 + 修订 + 退役）仍是下一片。

### 11.16an M8e（2026-09-22）：三张表的生产侧——计划说要什么，修订说何时重建，时间线说何时退役

§11.16am 把"变体"接进身份，这一片回答它引出的后勤问题：**这些表在生产里由谁产出**。之前的每一片里，表都是
测试手搭的；宿主编辑场景时不该做这件事——ABI 扫描是工作，按帧、按 drawable、或在录制时做，等于把内容决策
塞进帧循环。

**`api/ContentStore` 的四条规则**：

1. **计划就是清单**：`tablesFor(计划)` 走一遍编译好的帧（内容命令的 geometry/material/program、全屏调用的
   program），只建**点到名**的东西，点两次只建一次。没有比它更可信的清单——宿主手维护一份就是帧的第二份拷贝，
   两份不一致的方向恰好是"什么都不画"。
2. **修订是闸，且不猜**：重建读对象**现在**的 `Geometry::revision()` / `ShaderProgram::revision()`；材质
   SDK 类型没有 revision，只有 `updateMaterial()`（SDK `MaterialManager` 的契约，这里拼成一个成员）报告的
   那个。计划点的是旧修订，表**不给**旧字节——它已经不在了；那一刻的查找就是 miss，pass 按设计上报（"miss 是
   报告，不是回落"）。
3. **被顶替的修订留在表里**：点过 3 的帧可能还被重录（重建臂），所以 3 必须一直能答——直到**还可能记录它的
   槽**过去为止（与 GPU 对象同一窗口、同一个 `RetirementQueue`）。做法是**追加**而不是替换：表里短暂地同时
   有 3 和 4，3 在停靠到期时连同它的存储一起离场。**材质是例外**：它按身份查（计划不能点材质修订），表必须
   回答"现在的材质"，所以编辑**就地替换**那一行，只把旧值（facts + 它的块存储）停靠起来。
4. **条目持有键对象**：表按**地址**作键而观察不到销毁，只引用的条目会活过对象——地址被复用时会拿到死对象的
   事实（画面错，且是静默的）。所以每个被 track 的对象由 store 持一份份额；`releaseAbandoned()` 丢"只剩
   存储自己"的那些，把它们的行按同一窗口停靠。没有停靠窗口（会话没探到在飞槽数）时**留下并计数**
   （`retained()`）：内存有代价、正确性没有。停靠的闭包持 `weak_ptr<Data>`——store 先死则释放无事发生，
   队列先死则行按时离场，两个方向都不悬空。

**顺手补的两处（生产暴露的）**：

* **程序表的键要装得下生产**：一条 (身份, 修订) 现在可能对应**几份文本**（不同变体），所以 `ProgramFacts`
  带 `variant`，`findProgram(…, variant)` 按三者匹配，`recordCommand` 先由材料+几何算变体、再取条目——
  这样命令拿到的 `abi`（含 **push 范围**）就是它自己那份文本声明的。变异 M1/M5 证明：忽略变体时，要么
  真设备用例的左半只剩清屏色（贴图命令拿到"没有 push"的 ABI，矩阵从没写过），要么半片条目断言红。
* **查找必须扫全表**：表里现在可能有被顶替的修订，`findGeometry`/`findProgram` 原先"第一个同身份条目修订
  不符就报 Revision"会在多修订表上**把在表里的修订报成缺失**（追加序 [3,4] 查 4 ⇒ 先在 3 上停住）。改成
  扫全表、精确命中优先；M6 证明。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentStore.hpp` / `src/api/ContentStore.cpp`（新） | `track(几何/程序/材质)`、`updateMaterial`、`tablesFor(计划, 时间线, 停靠队列)`、`releaseAbandoned`、`clear`、`geometryEntries/programEntries/materialEntries/builds/retained`；内部三张表 + 每行配套存储（行的 span 指向自己的堆内存） |
| `api/ContentFacts.hpp` / `.cpp` | `ProgramFacts += variant`；`findProgram(…, variant)`；`findGeometry`/`findProgram` 改为全表扫描（被顶替的修订可能在场） |
| `api/ContentSources.cpp` | `buildProgramFacts(program, variant, out)` 顺手写上 `out.variant`（生成与查找一处拼写） |
| `api/ContentPass.cpp` | `recordCommand` 先查几何+材质 → 算变体 → 再按变体取程序条目 |
| `api/ProgramVariant.hpp` | 不再包含两张表头（前向声明 `GeometryFacts`/`MaterialFacts`）——表按变体作键 ⇒ 反过来包含它，写定义会成环 |
| `tests/test_vsg/ContentStoreTest.cpp`（新，无设备，8 条） | 计划决定建什么；稳态帧 0 重建；修订抖动 ⇒ 旧修订留到停靠到期（早一帧不放）；材质编辑 ⇒ 就地替换 + 停旧值；一个程序两个变体两条目；全屏程序拿引擎的顶点阶段；未 track 就什么都不答；`releaseAbandoned` 只丢没人持有的 |
| `tests/test_vsg/ContentPassTest.cpp`（+1 真设备） | 整帧的表由 store 生产：同一程序两个变体（`VINE_DIFFUSE_MAP` 连 push 声明都门控），**无贴图命令先收**（先建的一版被错查就会让贴图半边读不到 push）⇒ 左半洋红、右半材质色；M8d-1 的三条有贴图用例改为按各自变体建表条目 |

**变异反证（6/6 红）**：

| 变异 | 结果 |
| --- | --- |
| M1 `findProgram` 不比变体 | 变体用例红 + 真设备用例红 |
| M2 store 不看重修订（建过就不建） | 修订抖动用例红 |
| M3 被顶替的修订立刻删行 | 修订抖动用例红（窗口内就不再作答） |
| M4 材质编辑不推进修订 | 材质编辑用例红（还是旧块字节） |
| M5 store 忽略材质的贴图（恒用空变体） | 变体用例红 + 真设备用例红 |
| M6 几何查找在别的修订上停住 | 修订抖动用例红（在表里的修订被报缺失） |

证据：`test_vsg` 全量 **624 用例 / 96 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、
skipped=0、hygiene 0 / 840 文件、`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、
相位 9 行 / 2 次运行全收尾。

**这一片留下的口子**：①**半片的生产侧**——表有了，管线层与 `Scope::Entry` 仍由宿主自己建（用例里就是
"由表建层"那段循环），那才是下一步；②屏幕路径的程序条目**不带变体**（全屏 ABI 是引擎的、今天的全屏程序
也门控不了什么，但用户片元文本若按 define 变脸，这里要补）；③M8d-2 记的输入集合仍按第一个 content half 建。

### 11.16ao M8f（2026-09-22）：半片的生产侧——表与几何产出可录制的半片

§11.16an 产出了三张表，这一片把**半片**也接上：`Scope::Entry`（一个 (program, revision, layout, **variant**)
的已编译层 + 录制器）过去由宿主自己按 pass 手搭——那段循环每个宿主都要写一遍，而它写错的每个地方恰好都是身份
拼写的地方（变体、布局的绑定号、pass 的附件数）。

**`api/ContentHalves` 的规则**：

1. **走 pass，做与录制器相同的查找**：几何按计划点名的修订、材质按身份、变体由这两条事实决定（engine 的规则）、
   程序条目按变体取（§11.16an 的键）。因此**半片恰好存在于 pass 会问的那些元组上**：表答不出的东西不产出半片
   （录制器先因为几何或材质拒绝，两种说法一致），产出半片也绝不早于表能回答它的时刻。
2. **半片的键 = 编译产物依赖的一切**：kind、program、revision、layout、variant、**pass 的颜色附件数**。最后一项
   不是细节：层的混合状态是建层时按附件数写死的，1 附件的层用在 4 附件的 pass 上会**静默**地只画进三张。
3. **布局的拼写收进 api**：`ContentPipeline::create(abi, const GeometryFacts&, shaders, settings)`——绑定号 =
   `StreamUploads::bindingOfCanonical`（绘制侧绑的就是它，**不是**通道在表里的次序、也不是 shader 的 location：
   保留的纹理坐标槽是 8 而绑定号是 2），格式 = 分量数 × 32 位浮点，自定义通道（没有规范绑定）直接拒。测试夹具
   的 `pipelineFor` 改用它 ⇒ 夹具与生产路径**一份拼写**（既有的全部真设备内容用例顺带覆盖新重载）。
4. **键离开表 ⇒ 停靠**：表自己会把被顶替的修订退役；一个半片的键一旦表不再回答，pass 也再不会问它（pass 的
   元组正是从同一张表来的），所以它按**同一个** `RetirementQueue` 窗口停靠（闭包持 `weak_ptr`，两边都不悬空）。
   `halves()` 只数活着的；被停靠的活在窗口里、离开实时列表。**被拒的层记住**：编译不过的程序每帧重试一次
   等于把 shader 编译放进帧循环，而且每帧说同一句话。

**它不做什么（有意）**：**不**建 pass 的**声明集合**——那里面装的是帧的描述符号与材料的图（`BlockDescriptors`
是 scope 的事，本片的真设备用例仍自己按半片的 `abi` 建它）；也**不**清扫"再也没被画到"的程序留下的半片
（它的键还答得出，就留着——见下面的口子）。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentHalves.hpp` / `src/api/ContentHalves.cpp`（新） | 构造收 `VariantPool&` + 动态状态入口点（无设备调用方给空集）；`halvesFor(pass, facts, timeline, retirement)` 返回本 pass 的 `Scope::Entry` span（首次出现序、去重）；`halves()` / `builds()` / `refused()` / `clear()` |
| `api/ContentPipeline.hpp` / `.cpp` | 新重载 `create(abi, const GeometryFacts&, shaders, settings)`：绑定 = 规范编号、格式 = 分量数、自定义通道拒；头里前向声明 `GeometryFacts`（表反向包含本头 ⇒ 定义会成环） |
| `tests/test_vsg/ContentHalvesTest.cpp`（新，无设备，9 条） | 一条元组一片；稳态 0 重建；一个程序两个变体两片；附件数是键的一部分；布局是键的一部分；全屏拿引擎顶点阶段；表答不出就不产出；键离开表 ⇒ 停靠（先停靠、后释放、释放后不再重建）；被拒的层不重试 |
| `tests/test_vsg/ContentPassTest.cpp`（真设备用例重写 + 夹具） | 整帧 = store 的表 + 生产者的半片（`halvesFor` 的 span 直接进 scope）；断言两片各自带**自己文本的 push 声明**；夹具的 `pipelineFor` 改用 api 重载（删掉夹具里的 `channelFormat`） |

**实测踩到的两件事**：

* **两个条目不能共用一段存储**：`buildGeometryFacts` / `buildMaterialFacts` 会先清空传入的存储向量；测试里
  两个几何（或两个材质）共用一条 `std::vector` 时，第一个条目的 span 会**指向第二份数据**（span 指向的是
  向量的堆缓冲，向量的移动不搬它 ⇒ 存放它们的**外层向量的每个内层向量才是稳定住所**）。症状是"第一个几何
  的通道数对不上布局" ⇒ `findGeometry` 判 Malformed ⇒ 少一片半片。
* **替换文本的搜索必须限定起点**：`s.index(marker)` 从 0 找起，而同一段文本在文件更早的用例里也有
  （`scope.entries = halves;`）⇒ 起止反向、切片把整段文件复制坏。恢复靠 `git checkout -- <file>`（本文件
  当时只有本轮的改动，且已备份到 /tmp）。规则：**先 `index(start)`，再 `index(marker, start)`**。

**变异反证（6/6 红）**：

| 变异 | 结果 |
| --- | --- |
| M1 生产者的变体查找恒用空变体 | 真设备用例红（贴图半边拿不到带 push 的半片） |
| M2 键不比附件数 | 附件数用例红 |
| M3 键不比布局 | 布局用例红 |
| M4 永不清扫 | "键离开表 ⇒ 停靠"用例红 |
| M5 被拒的层不记住 | "被拒不重试"用例红 |
| M6 表还答得出也照扫 | 7 条用例红（半片每帧重建） |

证据：`test_vsg` 全量 **633 用例 / 97 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、
skipped=0、hygiene 0 / 843 文件、`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、
相位 9 行 / 2 次运行全收尾。

**这一片留下的口子**：①**逐 drawable 的声明集合**——同一变体、不同贴图的两个 drawable 今天会撞同一套
（`serveHalf` 认领集合只比 set 序号 + 形状 + 采样器绑定号，不比**装的是哪张图**）⇒ 下一步；②"再也没被画到"
的程序留下的半片没有清扫（键还答得出就留着，容量上界只能靠表的退役）；③M8d-2 记的输入集合仍按第一个 content
half 建。

### 11.16ap M8g（2026-09-22）：一个 drawable 的图，一个集合

§11.16ao 让同一变体的两个 drawable 共用**一个半片**；这一片处理共用暴露出的下一个问题：它们的**声明集合**。

**缺陷的形状**：引擎的 set 0 同时装 material 块与 `diffuseMap`——两个**带不同贴图**的材质走同一变体时，调用方
为它们各建一套集合，而这两套的**形状完全相同**（同样的块绑定、同样的采样器绑定号），差别只在**装的是哪张图**。
`serveHalf` 认领集合时只看（set 序号 + 块形状 + 采样器绑定号），于是"第一套"会同时服务两个 drawable——**形状
对、图错**，而且**静默**：画面是一张合法的贴图，只是别人的。这是"布局对了、字节错了"这一类里最晚被抓住的一个，
因为直到 M8f 之前，一个 pass 里的集合往往只有一个带图。

**修法：集合带上"它的图来自哪版纹理"**（`BlockDescriptors::ImageSource` = 纹理 + 修订）。

1. **调用方给每套自己建的集合打标**（`forAbi(..., samplers, source)`）：标的就是它取图时用的那对——
   `MaterialImages` 正是按 (纹理, 修订) 建缓存条目，所以"这套集合的图"与"那次 acquire 的图"是同一件事
   （一次拼写）。
2. **pass 由 drawable 的事实算它要的那对**：材质的 `texture` 指针 + **实时**读到的 `Texture::revision()`
   （材质编辑只改字节、不改图 ⇒ 不换集合；重填纹理改的是修订 ⇒ 换集合——键选的正是**图**变没变）。
3. **选取顺序**：先精确匹配的集合；没有就退到"source 为空"的集合（它的采样器全是 pass 级的——影子图、输入图，
   没有 drawable 的图 ⇒ 服务任何 drawable）。**这条回落是既有调用的活路**：M8d-1/M8f 的用例建的正是这种
   集合（M5 变异把它删掉 ⇒ 那些用例红）。
4. **拒绝分两种**：没有任何形状匹配 ⇒ "没人建过这种形状"；有形状匹配但都装着别的图 ⇒ "它的材质采样的纹理不是
   调用方建集合时用的那版"（修法不同，说法就得不同——§11.16am 的同一条纪律）。

**顺手记一个 C++ 坑**：`ImageSource` 是 `BlockDescriptors` 的**嵌套类型**，而它的默认参数写作 `= {}`——嵌套类型
的**默认成员初始化器**在"外层类定义内使用"时会被诊断
（`default member initializer for 'texture' needed within definition of enclosing class`）。所以它的成员故意
**不写初始化器**（`{}` 值初始化即为"没有纹理"），而不是靠 NSDMI。

| 文件 | 是什么 |
| --- | --- |
| `api/BlockDescriptors.hpp` / `.cpp` | `ImageSource{texture, revision}`（+ `empty()`、`==`）、`forAbi`/`create` 收尾参、`source()` 访问器 |
| `api/ContentPass.hpp` / `.cpp` | `serveHalf(entry, input_count, demanded)`（`demanded == nullptr` = 无 drawable 的 pass 级校验，接受任何匹配）；`recordCommand` 从材质事实算 demanded；拒绝语分清两种失败；`Scope::block_sets` 的文档改成"每个 set 序号 **× 每个图来源**" |
| `tests/test_vsg/ContentPassTest.cpp`（+1 真设备） | 同一半片、两张 1×1 贴图（洋红/绿）、两套**同形状**集合（各自打标）⇒ 左半洋红、右半绿；并断言两套形状/采样器数完全相同（证明只靠形状分不出来） |

**变异反证（5/5 红）**：

| 变异 | 结果 |
| --- | --- |
| M1 匹配只看形状（= 旧行为，复现缺陷） | 新真设备用例红（两半同色） |
| M2 pass 不要图（demand 恒空） | 新用例红（打标的集合一套都匹配不上 ⇒ 拒绝 ⇒ 清屏色） |
| M3 集合忘了自己的标（`d->source` 不存） | 新用例红（退化成形状优先、第一套通吃） |
| M4 demand 的修订恒 0 | 新用例红（精确匹配对不上、回落也没有 ⇒ 拒绝） |
| M5 删掉"没有纹理"的回落 | M8f 真设备用例红（既有调用方建的未打标集合不再被接受） |

证据：`test_vsg` 全量 **634 用例 / 97 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、
skipped=0、hygiene 0 / 843 文件、`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、
相位 9 行 / 2 次运行全收尾。

**这一片留下的口子**：①**集合的生产侧**——谁按 (程序变体 × 材质纹理修订) 建集合、并在纹理重填后把旧集合停靠，
今天还是调用方手写（下一步）；②"图来源"只覆盖 `diffuseMap` 一类**材质图**：`shadow_map`/输入图是 pass 级的，
两次 pass 用不同影子图时集合各建各的（调用方 per-pass ✓），但没有一条规则**强制**它；③M8d-2 记的输入集合仍按
第一个 content half 建。

### 11.16aq M8h（2026-09-22）：声明集合的生产侧

§11.16ap 让 pass 按"图来源"挑集合，这一片把**建集合**的那段宿主代码收成一处（`api/ContentSets`）。它回答的
三个问题每个宿主都会问：**图从哪来**、**这是哪版纹理**、**什么时候这套集合不再是它对的那套**。

**每个被声明的采样器按名字取图**（`api/ContentImages` 的策略，`imageOriginOf` 的三种来源各有出处）：

* `diffuseMap`（Material）⇒ 这个 drawable 自己的纹理，经 `MaterialImages::acquire`——材质没有纹理、或纹理
  不可用 ⇒ **白**（"没有图"是白色，不是没写过的绑定）；cube 槽（文本声明 `samplerCube`）落在白 **cube** 上
  （2D 视图配 cube 声明是非法描述符，不是"没有图"）。
* `shadow_map`（Shadow）⇒ pass 计划解析出的那张图（`shadowImageOf`），没有 ⇒ 白（按声明种类给 2D/cube）。
* 其余名字（Input）⇒ pass 的输入图，**按 pass 自己输入集合的绑定序**（逐输入、每个输入的彩色附件序、再深度）。

**集合的键 = (program, revision, variant, set, 图来源)**：前三个说它服务哪份声明（新修订是新 ABI、另一个变体
是另一份文本），图来源说它装的是谁的图（= M8g 的 `ImageSource`，`MaterialImages` 的键）。**采样器全是
pass 级的集合打空来源**——同变体的所有 drawable 共用一套（引擎的 set 0 装 material 块 + `diffuseMap` +
`shadow_map`，后者是 pass 级的，但整套仍可能因 `diffuseMap` 而按 drawable 分）。键的表不再回答（修订被表退役）
⇒ 同一窗口停靠；**被拒的集合记住**、不重试。

**它不做什么**：不建 pass 的**输入集合**（`ContentPass` 按 key 的计数自己建）；不建全屏调用的集合（屏幕 ABI 的
`declaredSets` 为空，整套都是 pass 的）；不报告（只计数：`fallbacks()` / `refused()`，诊断流是调用方的）。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentSets.hpp` / `src/api/ContentSets.cpp`（新） | 构造收 (`Device`, `BlockStorage&`, `MaterialImages&`)；`setsFor(pass, facts, halves, inputs, timeline, retirement)` 返回 `Scope::block_sets` 的候选 span（首次出现序、去重）；`sets()/builds()/fallbacks()/refused()/clear()` |
| `tests/test_vsg/ContentPassTest.cpp`（+1 真设备） | 与 M8g 同一幅画（**一个半片、两张 1×1 贴图、两套同形状集合**）但**一套都不手建**：`setsFor` 产出两套、`builds()` 第二趟不涨、两套来源不同；像素仍是左洋红、右绿 |

**变异反证（5/5 红）**：

| 变异 | 结果 |
| --- | --- |
| M1 产出的集合全不标来源 | 新用例红（两 drawable 都落到第一套 ⇒ 同色） |
| M2 材质图恒用白 fallback | 新用例红（两边都变白） |
| M3 键不比图来源 | 新用例红（一套通吃） |
| M4 永不复用（find 恒空） | 新用例红（`builds()` 第二趟仍涨、候选重复） |
| M5 图来源只比修订号 | 新用例 + M8g 用例红（两张纹理的修订号相同 ⇒ 分不开） |

证据：`test_vsg` 全量 **635 用例 / 97 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、
skipped=0、hygiene 0 / 845 文件、`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、
相位 9 行 / 2 次运行全收尾。

**这一片留下的口子**：①**帧装配的收口**——表（`ContentStore`）、半片（`ContentHalves`）、集合（`ContentSets`）、
输入集合（`ContentPass`）现在各自可用，但把它们按一次 record 串起来仍是调用方（真设备用例）在写；②集合与半片
的停靠窗口各自独立（同一修订的集合与半片会在两帧里先后离场——安全，但不整齐）；③`Environment`（skyMap）仍
无生产者（建层时就拒）。

### 11.16ar M8i（2026-09-22）：一帧收成两次调用

至此每一块都有自己的所有者：`ContentStore` 回答计划点到名的事实、`ContentHalves` 编译 pass 会问的半片、
`ContentSets` 建它们的声明集合、`ContentPass` 录制——而**把它们串起来的那段循环**（走 pass、把上一块的答案交给
下一块、开帧的块预算、每个 pass 一个注册表）每个调用方都要写一遍。`api/ContentAssembly` 就是那段循环：

* **`beginFrame(计划, 时间线, 停靠队列)`**：开块预算（`BlockStorage::beginFrame`——它是"一帧的第一个动作"）+
  由 store 产出表 ✓；
* **`record(pass, 兼容性, 输入图, 视图块, out)`**：半片 → 集合（后者拿前者的 entries）→ **每 pass 一个
  `StateRegistry`** → 组装 scope → 一次 `ContentPass::record`。注册表是每 pass 的：它自己的契约就是"这个 pass
  的状态记忆"，而一次录制发出的状态（含 pass 的视图块偏移）不该被第二个 pass 继承（逐 pass 的用例一直这么建）。
* 构造时**自己取动态状态入口点**（它拿着设备）——见下。

**集成测试揪出的真问题**：assembly 最初把 `ContentHalves(pool)` 用**默认空入口点**建起来，于是全部动态调用被
静默跳过 ⇒ 门禁 12 条 VUID（`07621`/`07627`/`10862`）。这正是 M4a 记过的坑，三个月后在新的调用链上原地复现——
说明"入口点"这条依赖最好由**知道设备的层**（assembly，而不是每个宿主）满足：现在它的构造签名就把这件事钉死了。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentAssembly.hpp` / `src/api/ContentAssembly.cpp`（新） | 构造收 (`ContentStore&`, `Device`, `VariantPool&`, `BlockStorage&`, `MaterialImages&`, `Diagnostics&`)；`beginFrame` / `record`；`facts()/halves()/sets()` 暴露证据计数；record 在 beginFrame 之前 ⇒ 空节点 + false（调用方 bug，不上报） |
| `tests/test_vsg/ContentPassTest.cpp`（+1 真设备） | 整帧只用两次调用：两纹理各自采自己的图；`storage->frames()` 证块预算按帧开；第二帧 store/halves/sets 的 `builds()` 全不动 |

**变异反证（4/4 红）**：

| 变异 | 结果 |
| --- | --- |
| M1 beginFrame 不开块预算 | 用例红（`frames()` 不动 ⇒ 第二帧没开） |
| M2 scope 不带声明集合 | 用例红（拒绝 ⇒ 录制失败） |
| M3 scope 不带半片 | 用例红（同上） |
| M4 beginFrame 忘了表 | 用例红（record 见空表 ⇒ false） |

证据：`test_vsg` 全量 **636 用例 / 97 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、
skipped=0、hygiene 0 / 847 文件、`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、
相位 9 行 / 2 次运行全收尾。

**这一片留下的口子**：①**提交失败接缝 / 重建臂**——"重录一帧"还不是一条真实路径（它一落地，集合/半片/表的停靠
窗口就有了真实读者，今天靠"记录是同步的"省掉的时序问题会回来）；②`Environment`（skyMap）仍无生产者；
③集合与半片的停靠窗口各自独立。

### 11.16as M8j（2026-09-22）：丢掉的提交，下一帧修一次

`OffscreenTarget` 的"内容不可信"事实（`attachments_invalidated`）与它的修复路径（计划答 Repair(Bootstrap)、
第一个 bootstrap pass 清标志）早已就位，但**只有测试手调 `invalidateAttachments()`**。这一片补上那道缝：
`VsgExecutor::noteLostSubmission(frame)` —— 把"这一帧的提交没发生"变成"它写过的那些离屏目标的内容不可信"：
只标**该帧 pass 真正点到的**离屏目标（没写的目标不动、默认帧缓冲没有我们的附件 ✓），下一份计划的对应 pass
`bootstrap == true`（用例断言），第一个 bootstrap pass 清标志（用例断言），第二帧起回到普通计划。
证据：真设备用例（记录一帧 → noteLostSubmission == 1 → 下一帧的计划 bootstrap ✓ → 记录后标志清 ✓ →
另一注册目标始终未被标 ✓）、0 VUID；变异 2/2（不标 / 清成 true ⇒ 红；另一次尝试的变异是空操作，不计）。
门禁 637 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 全清。

### 11.16at M8k（2026-09-22）：提交这一步自己说它失败了

M8j 把"提交没发生"变成了目标上的事实，但**说这句话的人还是宿主**：`noteLostSubmission(frame)` 谁来调、调在哪个时刻，是宿主自己的事。这一片把那句话搬进执行器：
`VsgExecutor::submit(frame, viewer)` —— **记录 → 提交 → 失败即标记**。任何一个从 `viewer.recordAndSubmit()` 抛出来的异常都意味着这一步没走完：
vsg 自己的词表是异常（命令缓冲建不出来就抛 `vsg::Exception`，而 `Viewer::recordAndSubmit` 返回 void、什么都不告诉调用方），而"这一步没走完"
⇒ 该帧写过的离屏目标内容不可信 ⇒ `noteLostSubmission` 标记它们、报一条 `SubmissionFailed`、答 false。宿主从此不需要解释任何失败。
**不在这层重试**：要不要再来一帧是宿主/会话的决定，而标记让重试变正确——下一份**编译出的**计划答 Repair(Bootstrap)（用例断言钉住）。

顺手两件：①`vsg::Exception` **不是** `std::exception`（纯结构体 message + VkResult），只 catch `std::exception` 会让故障直接穿过去——
变异 M3（只留 std::exception，去掉 vsg 分支与 `catch (...)`）红，且 gtest 打出的是 `Unknown C++ exception thrown in the test body.`（这条教训自己会说话）；
②SDK 的分类表按它自己写明的规则（"Append a new value before Count"）在 `Count` 前追加 `SubmissionFailed`——宿主得能按类别分辨"这一帧没提交"与"后端没起来"。

证据：真设备用例（两个目标；帧 1 经 `submit` 提交成功 ⇒ 清屏色真的在像素里（alpha=255）、没有标记；帧 2 的提交在记录步抛 `vsg::Exception`
⇒ `submit` 答 false、**只有它写过的**目标被标、另一目标不动、`SubmissionFailed` 计数 +1；帧 3 的计划 `bootstrap == true`）、0 VUID；
变异 4/4 红（不标 / 答 true / 标到别的目标上 / 不报）+ M3 也红。门禁 638 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 848 文件。

**这一片留下的口子**：①`Viewer::recordAndSubmit()` 吞掉的最后一层——队列提交返回的 `VkResult`（真·设备丢失正是这样回来的）宿主与会话都看不见；
等自己的帧驱动（会话的 commit 或插件侧）落地时，这条结果要接到 `submit` 同一道缝上；②Rebuild 臂；③`skyMap` 仍无生产者；④集合与半片的停靠窗口各自独立。

### 11.16au M8l（2026-09-22）：形状变了就是真的重建（Rebuild 臂）

`planTarget` 的 `Rebuild` 一直是"计划说得出、没人做得到"的一臂：换尺寸有 `resize`（ResizeInPlace），而**形状**
（附件数 / 格式 / 深度 / 样本 / 子通道）变了要重建渲染通道、帧缓冲**和按它编译的管线**——没有第二条路径。

这一片补上它：

* **`OffscreenTarget::rebuild(wanted, timeline, retirement)`**：被替换的是渲染通道**和它的每个 load-op 变体**
  （那些 pass 是按旧格式建的、`vkCmdBeginRenderPass` 直接点名）、镜像 / 视图 / 回读缓冲 / 帧缓冲 / 图；被保留的是
  lease（两向都拒，理由与 `resize` 相同：借用方的帧缓冲点名出借方的镜像）；被**停靠**的是旧附件**和旧渲染通道**
  （这是 `resize` 不需要、重建必须做的一半）。新附件是刚建的 ⇒ `written=false`、`generation+1`、下一份计划答
  `Repair(Bootstrap)`。构建失败（含驱动拒绝帧缓冲时 vsg 抛的 `vsg::Exception`）⇒ 一个字段都不动，目标继续服务旧形状。
* **`core::planTarget` 的次序改了**：形状变化现在**先于** load-op 修复判定。Repair 的定义是"没有 GPU 对象要改，
  第一趟进来清屏"——一旦渲染通道要重建，这句话就是假的；而重建后的新附件下一份计划照样答 Bootstrap，先答 Rebuild
  不丢任何信息。经典情形：宿主在第一帧之前就把格式改了（`built=false` + 形状变化），旧次序会把兼容性变化吞进
  "Bootstrap"。

证据：新设备相位（真设备 + 验证层）——先按旧形状（1 色、8×4）真画一帧，再 `rebuild` 成 **2 色 + D32F、16×12**：
计划答 `Rebuild`、`generation=1`、`written=false`、`passVariantCount()==1`（旧变体随旧 pass 一起走）、
`shape()` / `instance()` 都变成新形状而且 **compatibility 真的不一样**（管线键的判据）、旧图被停靠
（引用计数不变 + `pending()==1` + `deviceWaits()==0`）；第二帧只用新形状的图渲染：附件 0 = 新清屏色、
**附件 1 = 透明黑**（plan 的额外附件规则）、深度回读 = reverse-Z 远平面——三条读回证明新 pass / 帧缓冲真的有两个颜色
和一个深度；相位行按 `rebuilds_replaced` 计数门禁（门禁相位 9 → 10 行 / 2 次运行）。另加 lease 两向拒绝的真设备用例
（borrower 消失后同一调用重建，且重建后的 lender 能再次出借）+ 次序表的 2 条无设备用例。
变异 5/5 红（次序复原 / 新形状不装 / 事实不重置 / 旧变体留着 / 直接销毁不停靠）+ 一次"不装新 pass"的变异
**用例抓不住、门禁抓住**：18 条 VUID（`VkFramebufferCreateInfo-attachmentCount-00876`、
`VkRenderPassBeginInfo-renderPass-00904`），像素照过——"兼容性主张像素抓不住"的老教训在新臂上重演。
门禁 639 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 848。

**这一片留下的口子**：①**谁在每帧应用计划的答案**（`resize` / `rebuild` 今天仍由宿主或测试调用；帧驱动落地时接上，
到时"编译用的是新形状、目标还是旧形状"要在录制前挡住——executor 现有的 shape 核对只看**颜色附件数与深度可采样性**，
格式变化它看不见）；②`Viewer::recordAndSubmit` 吞掉的队列 `VkResult`；③`skyMap` 仍无生产者；④集合与半片的停靠窗口各自独立。

### 11.16av M8m（2026-09-22）：帧驱动把计划的答案应用到自己持有的目标上

M5e 的 `resize` 与 M8l 的 `rebuild` 在此之前**只有测试在调**：计划每帧都算得出 `ResizeInPlace` / `Rebuild`，
但"谁把答案变成动作"一直悬着。这一片把它收进执行器：`VsgExecutor::applyTargetPlans(frame, facts, timeline, retirement)` ——
`compile → applyTargetPlans → record → submit` 至此是一条完整的帧驱动。

* 只走**计划点名的**目标（`frame.targets`），每个按身份在 `facts` 里找它的描述——**想要什么活在 facts 里**
  （计划只带答案）；执行器注册表里有、但这一帧没点名的目标**不动**（相位里的"旁观者"就是这条的判据）。
* `ResizeInPlace` ⇒ `resize`，`Rebuild` ⇒ `rebuild`；新形状从 `wanted.shape` 来，而目标的 **clear 策略**与
  **深度采样提升**从目标自己来——新增的 `OffscreenTarget::layout()` 就是为此（计划描述里没有这两样，猜一个
  会静默丢掉一次性设置）。
* 计分 `resized` / `rebuilt` / `refused` / `failed`；`None` 与两个 Repair 臂不计（后者是录制的事：bootstrap 清屏）。
  默认帧缓冲跳过（尺寸属于 surface）；没注册的目标跳过（`record` 会照旧上报那条 pass）。
* 已知的下一层：形状里**格式**变了而驱动没跑（或 refused/failed）时，`record` 的核对只看颜色附件数与深度可采样性，
  **格式变化它看不见**（继续记为口子）。

证据：新设备相位（真设备 + 验证层）——同一执行器持有两个目标：帧 1 = 目标的第一个写者（计划答 `Repair(Bootstrap)`，
驱动**不**应用、录制清屏 ⇒ 像素）；帧 2 = 计划答 `ResizeInPlace` ⇒ `applied.resized == 1`、16×12 的像素；
帧 3 = 计划答 `Rebuild`（2 色 + D32F）⇒ `applied.rebuilt == 1`、附件 0 是清屏色、附件 1 透明黑；三帧都经
`executor.submit` 提交；旁观者目标的 `generation`/`width`/`height` 全不动。相位行按 `plan_applied` 门禁（+2）。
另加真设备用例，与既有的"计划与目标不符 ⇒ 拒录"配对：同一份事实（这次说真话、帧点名**第二个**目标）下
`applyTargetPlans` 之后同一帧**录得进去**（`applied.rebuilt == 1`、像素是帧自己的清屏色），计划没点名的目标不动。
变异 4/4 红（Rebuild 不应用 / ResizeInPlace 不应用 / 拿"第一个事实"当答案 / 新形状取 `current` 而不是 `wanted`；
第一次 M4 尝试只改了 extent、在"形状变化"下是空操作，不计）。
门禁 640 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 848、相位 11 行 / 2 次运行。

**这一片留下的口子**：①`record` 的 shape 核对看不见格式变化；②`Viewer::recordAndSubmit` 吞掉的队列 `VkResult`；
③`skyMap` 仍无生产者；④集合与半片的停靠窗口各自独立。

### 11.16aw M8n（2026-09-22）：会话的提交也自己说它失败了

M8k 让执行器的提交步自己说失败，但**窗口路径的提交在会话里**：`Session::commitFrame()` 调的是
`Viewer::recordAndSubmit()`，而它返回 void、把每个任务的 `VkResult` **丢掉**——队列提交被驱动拒绝（设备丢失 / 内存不够）
会和"这一帧发生了"长得一模一样，宿主连"要不要标这个帧写过的目标"都无从判断。这一片把这一半接上：

* 会话**自己驱动** viewer 的任务（把它自己的两步照做一遍：reset 每个命令图，然后逐任务在调用线程上提交——会话从不
  打开 viewer 的多线程），把第一个非成功的 `VkResult` 留下；vsg 抛的异常（`vsg::Exception`，命令缓冲建不出来）同样算
  "提交没发生"。
* 失败的后果是**两半**：报告 `SubmissionFailed` + `lostFrames()+1` + 答 false，**不呈现、不计已呈现、不声称完成**
  （没有槽被回收，完成证据不存在）；而帧**照样结束**——`FrameTimeline::abandoned(token)`：令牌被消费（swapchain 图像
  已取、图已录，重试不是一个可选项），但 **submitted 水位不动**（它是"提交数"不是"开帧数"）。这让两个水位各归各位，
  也让丢帧期间停靠的对象有正确的日期（没有任何 in-flight 命令缓冲点名它们）。
* **不修的**：丢帧时 acquire 到的那张 swapchain 图像再也不会被呈递，WSI 因此缺一张可用的图像——下一次 acquire 在
  这个环境里就开始报错（写用例时实测：`vkAcquireNextImageKHR` 的 forward-progress 警告 + 呈现未渲染图像的错误）。
  这一层不修 WSI：持续提交失败的会话就是设备没了，宿主要么重建会话（`initialize()`，与移动 surface 同一条路），
  要么结束。用例走的是前者。

证据：真设备用例（会自己开窗口）——会话的帧图里放一个"被武装就在记录步抛异常"的节点：`commitFrame()` 答 false、
`SubmissionFailed` +1、`lostFrames()==1`、`framesPresented()` 不动、`submittedFrame()==0`、没有开着的帧；随后
`initialize()` 把会话建回来，一帧正常提交并呈现（`framesPresented()==1`、`submittedFrame()==1`），**0 VUID**。
另加一条无设备时间线用例（`abandoned`：帧结束、水位不动、帧号是提交时钟所以下一个帧接它的号、停靠日期）。
变异：M1 不接 vsg 异常（异常穿出提交）、M2 不结束帧、M3 把丢掉的帧算成提交、M4 不计数、M5 不报告、M6 `abandoned` 也推水位
——**6/6 红**（各 0 VUID；"帧没结束"那一条的后果就是断言红，不是挂住）；另一次"失败也照样呈现"的变异让**用例挂住**
（呈现一张没渲染过的图像，阻塞在 WSI 上），并在阻塞前先吐出 2 条 VUID（`VkPresentInfoKHR-pImageIndices-01430` +
`vkAcquireNextImageKHR-surface-07783`）——两种读法都抓得住它，也是"丢帧后 WSI 状态"那条口子的现场。
门禁 642 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 849。

**这一片留下的口子**：①把 M8m/M8n 连起来（窗口路径上"提交失败 ⇒ 标这一帧写过的目标"由帧驱动一次完成）；②`record` 的
shape 核对看不见**格式**变化；③丢帧后的 WSI 状态（上面那条）；④`skyMap` 仍无生产者；⑤集合与半片的停靠窗口各自独立。

### 11.16ax M8o（2026-09-22）：窗口路径上一次调用把"提交失败"变成事实

M8k 让执行器的 `submit(frame, viewer)` 在自己驱动的 viewer 上说失败，M8n 让会话的 `commitFrame()` 说它；但窗口路径上
"提交失败 ⇒ 标这一帧写过的目标"仍要宿主把两者拼起来。这一片收成一步：`VsgExecutor::commit(frame, session)` ——
`session.commitFrame()`，答 false 就 `noteLostSubmission(frame)`；宿主两个答案都不用解释。分工写在明处：**报告**留在会话
一侧（`SubmissionFailed` 带原因），执行器只加会话**不可能知道**的那半——哪些目标现在的内容没人能担保。答 false 的两种
情况（提交失败、以及"没有开着的帧"）都标：两者都意味着这一帧的写入没进过队列。

证据：真设备用例（开真窗口）——**两个会话**：会话 A 上经 `commit` 提交一帧（窗口 pass + 离屏 pass）⇒ 答 true、离屏目标
像素 = 清屏色、没有被标；会话 B（丢失的帧会把 swapchain 钉住一张已取未呈的图，所以第二半本来就该跑在重建出来的会话上，
见 §11.16aw）上把记录步做成抛异常 ⇒ `commit` 答 false、`SubmissionFailed` +1、离屏目标被标、`framesPresented` 不动、
`lostFrames()==1`；下一份计划里**写它的那一趟** `bootstrap == true`（窗口那趟不动），把那一趟录进去 ⇒ 标记清掉。
两次运行各 **0 VUID**、0 崩。变异 3/3 红（不标 / 成功也标 / 永远答 true）。

**写这一片时撞出来的新口子（记在案）**：`assignFrameGraphs` 换的是 viewer 的 record-and-submit task——**上一次提交还在飞**
时再 assign（本片用例最初的写法：每帧 assign 一次）会销毁在飞的 fence / semaphore / 命令缓冲：实测 12 条 VUID
（`vkDestroyFence/Semaphore/Buffer` 在飞、`vkFreeCommandBuffers` pending）+ 验证层里段错误（不带层时静默通过）。本片用例
因此**每个会话只 assign 一次**；正确做法（assign 前等设备、或复用 task 而不是重建）留给帧驱动收口那片。

### 11.16ay M8p（2026-09-22）：每帧换命令图不再拆掉在飞的机器

M8o 收尾时撞出来的口子：`SessionContentAccess::assignFrameGraphs` 走的是 viewer 自己的
`assignRecordAndSubmitTaskAndPresentation`，而它**清空并重建** record-and-submit task——task 手里握着在飞提交还点名的
fence / semaphore / 命令缓冲，于是"每帧换图"（这本来就是 `makeFrameGraph` 的用法：一帧一张图）在上一次提交还没回来时
会拆掉它们：实测 12 条 VUID（`vkDestroyFence/Semaphore/Buffer` in use、`vkFreeCommandBuffers` pending）+ 验证层里段错误
（不带层时静默通过）。

这一片把换图改成**换图不换机器**：新图交给 session 已经有的 task（`task->commandGraphs = graphs`），只有 viewer 一个
task 都没有时（这个 session 不会：它起来时就带着自己的帧图）才让 viewer 重建；task 的 fence、WSI 信号量与窗口原样保留。
编译照旧跟在后面（新图里的对象要那一趟才有实现）。

证据：真设备用例——一帧一张图，**帧帧都在上一帧的提交还在飞时换图**：六个提交（前三个各写一个离屏目标，后三个只写窗口；
后三个是判据的一半：帧 k 会等它进入的那个槽的 fence，所以"后面三帧"正是"前三帧已经做完"的证据）⇒ `framesPresented()==6`、
`lostFrames()==0`、`deviceWaits()==0`、`diagnostics.clean()`、两次运行各 **0 VUID**，且两个离屏目标的像素是**最后换的那张图**
的清屏色（0.4 / 0.6，而不是第一帧的 0.2 / 0.6）。变异 3/3 红：①回到旧行为（viewer 重建 task）⇒ **30 条 VUID**；
②换图不装机（新图不交给 task）⇒ 像素错；③不编译 ⇒ 红。
门禁 644 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 849、相位 11 行 / 2 次运行。

### 11.16az M8q（2026-09-22）：丢帧的会话自己活下来

M8n/M8o 的现场留了一个"宿主必须重建会话"的尾巴：丢帧时 acquire 到的那张 swapchain 图像再也不会被呈递，WSI 因此缺一张图，
下一次 acquire 就报 forward-progress 警告、present 报"呈了一张没 acquire 的图"（实测两类 VUID）。这一片把这一层修进会话自己：

* `commitFrame()` 的失败分支在结束帧之后**重建 swapchain**（`window->resize()` → vsg 的 `buildSwapchain()`：先
  `vkDeviceWaitIdle`，再换新链、重建 depth/multisample 与 frames），并把这次 idle **计进 `deviceWaits()`**——"帧路径不停
  设备"这条不变量因此仍然可查：丢帧是例外，计数器就是例外留下的痕迹。
* "没有开着的帧"那条 false 不走这里（没有 acquire 过，没有 WSI 损伤要修）。
* 不做的仍然是"重试这一帧"：图像已经取走、图已经录过，重试不是一个选项（见 §11.16aw 的 `abandoned`）。

证据：真设备用例（M8o 的会话 B 扩写）——丢帧（`commit` 答 false、目标被标、`SubmissionFailed` +1）之后**同一个会话**继续驱动：
下一帧（窗口 pass + 离屏 pass）`commit` 答 true，计划里写该目标的那趟 `bootstrap == true`，录进去后标记清掉，随后四个只写窗口的
帧（比槽数多一：帧会等它进入的那个槽的 fence，所以它们是"那一帧已经做完"的证据）⇒ `framesPresented()==5`、`lostFrames()==1`、
`deviceWaits()==1`、`diagnostics.total()` 恰好一条；目标像素 = 这一趟的清屏色。两次运行各 **0 VUID**。
变异 3/3 红：①不重建 ⇒ **10 条 VUID**（acquire 的 forward-progress + present 未 acquire 的图）；②重建但不计数 ⇒ 红；
③每帧都重建 ⇒ 红（本片用例的 `deviceWaits()==0`），且连带给**另外 8 个会话用例**留下失败——"这条路径不该停设备"的判别力
原来就在那儿。门禁 644 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 849、相位 11 行 / 2 次运行。

### 11.16ba M8r（2026-09-22）：计划说的形状，连格式一起核对

`record` 那道"计划与世界必须一致"的检查此前只问两件事：**颜色附件数**与**深度可采样**（§11.16f）。于是同一数目、同一深度、
只有**格式**不同的漂移完全不可见——而格式正是引擎自己的词汇无法分辨的那一半（RGBA8 同时是线性图和 sRGB 面，见
`RenderPassCompatibility` 的实测）。计划本身也**没有携带**形状，连比较的材料都不在。

这一片把"计划被告诉的形状"变成计划的一部分，并拿它核对：

* `core::CompiledShape`（`FrameCompiler.hpp`）：形状的兼容性半边——引擎拼写、深度格式、**设备拼写**、samples、subpass。
  `CompiledTarget += shape`；编译器用 `arena.copy` 把 facts 说的两份格式表抄进**帧自己的 arena**：计划里不许出现指向宿主
  facts 的 span（P0-1），而值形式的 `TargetShape` 会每帧给每个 target 分配两个 `vector`（分配门禁就是为这种事准备的）。
* `core::statedShapeAgrees(stated, actual)`：**计划说了什么就核对什么**——引擎半边每份计划都说了（空 `color_formats` 就是
  "没有颜色附件"，如深度-only 的 shadow map），所以直接比；**设备半边只在两边都说了时才比**：没学会设备拼写的计划"什么也没说"，
  "不知道"不等于"没有"（与 `RenderPassCompatibility` 同一条规矩），两个"不知道"也不当作一致证据。
* `VsgExecutor::recordOffscreen` / `recordWindow` 都接上它；窗口那一趟此前拿不到 `CompiledTarget`，现在传进去（窗口的 facts
  来自它自己，`WindowTarget::facts()` 连设备拼写一起说）。

证据分三层：

* **无设备（2 条）**：①计划真的带着那份形状，而且是**它自己的副本**——编译后改 facts（引擎格式、设备格式、深度一起改），计划里
  的值不动；②判定表逐行：引擎格式漂移（数目相同）⇒ 不一致；设备格式漂移（引擎相同）⇒ 不一致；深度格式、samples、subpass 各一行
  ⇒ 不一致；**设备半边留空 ⇒ 一致**（"没说"不比）。
* **真设备（`ExecutorTest.APlanThatGotOnlyTheFormatsWrongIsRefusedToo`）**：同一个 target 上两半各来一次——引擎格式换了、
  数目一样（`color_attachments == 1` 是"旧检查看不见"的证据）⇒ `record` 答 false、`skipped == 1`、什么都没录、报告 +1；设备
  格式换成**另一个合法的拼写**（引擎半边完全一致）⇒ 再拒一次、报告 +2；把它告诉成真话（`target->shape()`）⇒ 同一帧录得进去、
  `skipped == 0`。
* **真设备（`SessionTest.AWindowPlanThatGotTheFormatWrongIsNotRecorded`）**：会话自己的窗口上同样的事——facts 说的格式与窗口
  的差一个而数目相同 ⇒ 拒录（报告 +1、`recorded()` 空）；说真话后录进去**并且呈递**（`framesPresented() == 1`、
  `deviceWaits() == 0`——核对帧的形状不该让设备停下来）。两次运行各 **0 VUID**。

**它当场揪出两处旧夹具的谎话**（修夹具，不加容忍）：`runPlanDrivenTargetPhase` 的帧 3 先把旧形状抄下来、只往引擎半边 push 一个
格式（设备表因此少一项）⇒ 改成"在引擎的词汇里**声明**想要的东西"（设备拼写是目标层对这次请求的回答：不声明是诚实的，半声明
不是）；`SampledInputTest` 的全屏用例把有深度附件的目标说成"只有颜色"（此前这让计划一直答 Rebuild）⇒ 改成目标自己的 `shape()`。
两处都是这条检查存在的理由本身。

变异 5/5 红：①离屏那半回到只比数目 ⇒ 红（本片用例 + 2）；②窗口那半回到只比数目 ⇒ 红（窗口用例 + 2）；③编译器不抄形状 ⇒
**75 行红**（每一条真设备记录都开始拒）；④设备半边**无条件下比**（计划没说也比）⇒ **51 行红**（"只在说了时比"是承重的）；
⑤不比深度格式 ⇒ 红（无设备判定表 + 全屏用例）。门禁 **648 用例 / 98 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 849、
相位 11 行 / 2 次运行。

**本片留下的口子（登记）**：①窗口路径仍没接 `applyTargetPlans`（计划的换尺寸/重建答案对窗口还没有执行者）；②`skyMap` 仍无
生产者；③集合与半片停靠窗口各自独立（旧口子不变）；④设备半边在"某一侧没说"时跳过——那是一处**已知的不检查**，不是等价性声明。

### 11.16bb M8s（2026-09-22）：窗口的答案也有执行者——问平台，不采纳计划的主张

M8m 的 `applyTargetPlans` 把默认帧缓冲整支跳过（"尺寸属于 surface"），于是**计划对窗口的任何答案都没有执行者**。M8r 之后这件事有了后果：
窗口的答案一旦与目标不符会被拒录，而一个被忽略的答案只会让那个不符永远存在。这一片把窗口当成一个正常目标处理，并把它的本质差别写进代码：

* `WindowTarget::facts()` 把"想要"与"已有"分开：**wanted = swapchain 现在服务的形状**（`sampledShape()`，一次实时采样），**current = 这份目标的
  记录被建时的形状**（`shape()`——那个进了管线键的兼容性）。稳定会话上两者相同 ⇒ 计划答 `None`；平台把 surface 换成别的格式时，计划看到的就是
  形状变化 ⇒ `Rebuild`，而不是被记进一个计划没有描述过的渲染通道。
* `WindowTarget::refresh()`：重新采样 swapchain，**变了才替换**并答 true。**唯一能写这份形状的是平台**——执行器不采纳计划的主张。
* `VsgExecutor::applyTargetPlans` 的窗口臂：`Rebuild` ⇒ 问平台（`refresh()`）：真变了 ⇒ `rebuilt`，没变 ⇒ `failed`；`ResizeInPlace` 无事可做
  （extent 属于 surface，平台与 vsg 自己跟随）；`None` 与 Repair 臂照旧是录制的事。口径与离屏目标一致，`record` 的核对把它兜住。

证据（真设备用例 `SessionTest.AWindowRebuildIsAnsweredByAskingThePlatformNotByBelievingThePlan`，会话自己的窗口）：

* 稳定帧：`facts()` 的两半相同 ⇒ 计划 `None`、`applyTargetPlans` 四个计数全 0、同一帧录得进去（`skipped == 0`）；`refresh()` 答 false
  （"没有宿主路径能在活会话下换掉 swapchain 的形状"——换格式被 `VsgHostWindow::moveToHostSurface` 拒绝，宿主重建走 `Session::initialize` 的
  Rebuild 臂，两者都不留一个陈旧的缓存）。
* 说谎的帧：计划被告知 swapchain 换了另一个（真实存在的）设备格式（格式码取自另一个离屏目标——这一层只比格式码，从不臆造）⇒ 计划答 `Rebuild`；
  执行器问平台 ⇒ 平台说没变 ⇒ `rebuilt == 0`、`failed == 1`、**目标报告的形状没被改**（主张没被采纳）、再问一次 `refresh()` 仍 false；
  随后 `record` 拒录（`skipped == 1`、`ContentSkipped` +1）：没被应用的 Rebuild 不会被记录。
* 再说真话：同一帧录得进去并**呈递**（`framesPresented() == 1`、`deviceWaits() == 0`）。两次运行各 **0 VUID**。

变异 3/3 红：①把 `refresh()` 的结果取反（把"没变"记成 `rebuilt`）；②`refresh()` 删掉比较、永远答"变了"；③去掉 `action != Rebuild` 的守卫
（对每个窗口答案都跑一次）⇒ 稳定帧的"四计数全 0"红。

**本片留下的口子（登记）**：①窗口 `facts()` 的 live 采样与 `refresh()` 的**成功臂**今天没有可驱动的触发（宿主换格式被拒、宿主重建走整会话
Rebuild）——它的可观测形态需要一条"平台真的换了形状"的宿主路径（或一个能重建 surface 的测试缝），那时两者会同时被观测；②`skyMap` 仍无
生产者；③集合与半片停靠窗口各自独立；④设备半边在"某一侧没说"时跳过（M8r 的登记不变）。

### 11.16bc M8t（2026-09-23）：`skyMap` 是 drawable 自己的图——引擎自带天空程序落地

M8c-3 把 `skyMap` 读成"帧级环境"并在建层处拒掉，理由是"环境图没落地"。这一片纠正这个名字：**`skyMap` 就是 drawable 自己的图**，
证据在引擎自己的文本里——`builtin_skybox.frag` 写着 "The material's texture, at the binding every content program samples
it from (set 0 / binding 1, the ABI's `diffuseMap` slot)"，`AppShellDemo` 的天空盒也是把自己的 cube 放进 material 再挂
`skyboxProgram()`。所以 `imageOriginOf("skyMap") == Material`（与 `diffuseMap` 同行），`ImageOrigin::Environment` 这一行、
`describeAbi` 的拒绝、`ContentSets` 的那条分支一起删除：**引擎自带的天空程序从"建层即拒"变成能画**。

**种类必须对得上声明的采样器**，这是这一片新立的规矩（demo 的注释里提到的那件事）：

* `ContentSets` 的 Material 行按**声明的种类**给 fallback（cube 声明 ⇒ `whiteCube()`，2D 声明 ⇒ `white()`），并且当图是**另一种
  种类**时同样给声明的白 + 计一次 fallback（"不可用的图"是值；把 2D 视图写进 cube 声明是**非法描述符**，不是"没有贴图"）。
* `createScreen` 收紧：全屏 ABI 绑的是源的附件与深度（2D 视图），文字声明 `samplerCube` ⇒ 拒（cube 图是内容侧 drawable 自己的）。

**顺手抓到一个真缺陷**（就是本片用例抓的）：`BlockDescriptors::ImageSource` 故意没有成员初始化器，而两处 `ImageSource x;`
忘了 `{}`——`ContentSets` 里"这次 drawable 要哪张图"的局部量与 `ContentPass` 里的 `demanded`。结果：**没贴图的材质会被按上一个
drawable 的纹理归档**（栈上的残留值）。实测形态：天空盒用例里"无图"那条的集合根本没建成（键撞上前一个 drawable 的图），pass 随后
按"材质要的图不在调用方的集合里"拒绘。两处都改成 `{}` 并写明理由。

证据（真设备，`ContentPassTest.TheEngineSkyProgramDrawsTheSkyBoxsOwnCubeMap`）：四个方向条带、同一趟、只经过 `ContentAssembly`
的两条直路（不手建任何集合）——

* 方向 (0,0,1) ⇒ cube 的 +Z 面**洋红**；(1,0,0) ⇒ +X 面**红**（插值通道就是方向，全角同方向 ⇒ 整条只采一个面）；
* 无图的材质 ⇒ **白**（声明的 cube 种类的白 cube，不是 2D 白）；2D 图配 cube 声明 ⇒ 也**白**，且 `sets().fallbacks() == 2`；
* 计数器：`halves() == 2`（有图/无图两个变体）、`sets() == 3`（cube 一张、无图一张、2D 一张）、稳态帧全不动；两次运行各 **0 VUID**。
无设备行同步更新：名字表（`skyMap` ⇒ Material）、cube/2D 的 `skyMap` 层建得起来、全屏 cube 声明被拒。变异 5/5 红：①名字表不认
`skyMap`；②fallback 不看声明种类（**6 条 VUID**）；③不一致不检查（**2 条 VUID**）；④`ImageSource` 又未初始化（复现上面的缺陷）；
⑤全屏 cube 声明放行。门禁 **650 用例 / 98 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 849、相位 11 行 / 2 次运行。

**本片留下的口子（登记）**：①天空"跟随相机/当作无穷远"是宿主的事（demo 的盒子是静态的，内容 push ABI 里没有视图旋转）；②集合与
半片停靠窗口各自独立、窗口 live 采样与 `refresh()` 成功臂不可驱动、设备半边"某一侧没说就跳过"（皆是旧口子）。

### 11.16bd M9a（2026-09-23）：门面立起来——SDK 的方法落到会话与帧驱动上

前面每一片的证据都是测试**直接**调用 `api::` 的那些件（会话、执行器、内容层、编译器）。这一片第一次把
`vine::graphics::RenderBackend` 这条**引擎看得到的缝**接上：`api/VsgBackend`（`VsgBackend : RenderBackend`，
PImpl）不自己决定任何事——它把 SDK 的调用翻成对已有件的驱动，翻不过去的**报出来**。

**它今天服务的东西**（就是 SDK 契约里"后端该有的骨架"）：

* **生命周期**：`initialize()` 由公告（宿主句柄、最后一次 `resize` 的尺寸、默认内容程序）建会话、把窗口目标注册给执行
  器；`shutdown()` 幂等、可以在任何 `initialize()` 之前调、之后还能再起来——宿主重建表面的路径就是"下来、上去"。
* **帧协议**：`beginFrame()` 开帧（快照 + 会话开帧）、`endFrame()` 收帧（计划冻结、按这一帧画进哪里编译）、
  `swapBuffers()` 把计划交给执行器（应用目标计划、录制、提交、呈递）。**空帧也走全套**——SDK 自己的规则：开帧
  可能 acquire 了图，把它还回去的唯一调用是呈递。
* **表面事实**：`setWindowHandle()` 在 `initialize()` 前公告，采纳后 `nativeHandle()` 答的是宿主那个；`resize()` 是
  **公告**——下次起会话时用它建窗口，会话活着时报一次（活着改尺寸是后面的片）。
* **诊断**：这一层**一条都不自己报**——核心的 `Diagnostics` 转发进 SDK 的 `reportDiagnostic`，所以宿主装的 sink 与
  `diagnosticCount()` 看到的就是会话/执行器自己的那些报告；门面只补"这一步我还没有"。
* **画的那一半一律报"还没服务"**：`beginPass/endPass/setPassOrder/setViewport/setLights/setPassInputs/setDepthMode/
  setClearPolicy/render/drawScreenProgram/setRenderTarget/readColorBuffer/readDepthBuffer` 每处用 `core::ReportOnce`
  报**一次**（`UnsupportedRequest`；读回另答 false + `ReadbackResult::Unsupported`），`supportsRenderTargets()` 答
  **false**：引擎在**摆**离屏工作之前就会问一次，"拒绝"是一个被告知的状态，而不是一个黑屏。

证据（真设备，`VsgBackendTest`，两条用例）：

* `TheSdkFacingBackendComesUpPresentsEmptyFramesAndSaysWhatItCannotServe`：没有会话时开帧 ⇒ 报一次、不开帧；起来
  之后三帧空帧 ⇒ `framesPresented() == 3`、`deviceWaits() == 0`、**健康会话零报告**；三趟 pass 回路
  （`beginPass/setViewport/setClearPolicy/render/endPass`）**恰好 4 条**报告（`endPass` 与会话内部的 `beginPass` 是
  同一条情节，它没有自己的槽）；`supportsRenderTargets() == false`；活着的 `resize` 报一次；再 `initialize()` ⇒
  `framesPresented()` 归零；双 `shutdown()` 之后再起来照样呈递。sink 与 `diagnosticCount()` 逐条对齐。
* `AHostSurfaceIsAdoptedAndMovingToTheNextOneKeepsTheSession`：采纳 `TestHostWindow`、走两帧，再公告**另一个**宿主面
  ⇒ 移动**保住会话**（`framesPresented() == 3`：帧号接着数，而不是从头——重建会话才会从头）；两次关闭之后两个宿主
  窗口都还活着（窗口是宿主的）。

**本片撞出的真问题（它自己的用例抓的）**：门面第一版的 `swapBuffers()` 给空帧交了 `makeFrameGraph()` 的**空命令
图**——gtest 全绿，验证层却给每条呈递一条 **VUID 01430**（presented image is in `VK_IMAGE_LAYOUT_UNDEFINED`，实测
16 行）。原因不在 acquire：临时打进 `Session::beginFrame/commitFrame` 的打印证明 imageIndex 在 0/1/2 之间轮转、
尺寸公告（640x360 → 320x180）真的生效。**把图像搬出 `UNDEFINED` 的是窗口那棵 render graph**——会话自己那张初始
化期的图里就有它（所以会话自己的空帧路径从来干净），空命令图里什么都没有。修法：**计划为空时不换图**——直接
`commit` 会话自己的那张（`Session::commitFrame`）；有 pass 的帧再照 M8q/M8o 的驱动形状自建图（窗口那趟由
`recordWindow` 把窗口图放进新图里）。这不是"测试口味"，是一条不变量：**acquire 过的图必须有一趟 render pass 走
过它，才轮得到呈递**。

变异 4/4 红：①空帧不呈递（不 commit）⇒ 5 行红 + 6 VUID；②又交空图（复现上面的问题）⇒ **16 VUID**（gtest 0 行）；
③"未服务"的报一声不响 ⇒ 3 行红；④`supportsRenderTargets()` 说谎（答 true）⇒ 3 行红。门禁 **652 用例 / 99 套件**、
0 VUID / 0 SYNC-HAZARD、hygiene 0 / 852、相位 11 行 / 2 次运行。

**本片留下的口子（登记）**：①门面今天的计划永远是空的（画的一半全部拒绝），所以"有 pass 的帧"那条驱动形状要等
门面第二片才真被用过；②活着的 `resize` 只报不改（会话 swapchain 的形状只有平台能换采样）；③集合与半片停靠窗口
各自独立、窗口 `facts()` 的 live 采样与 `refresh()` 成功臂不可驱动、设备半边"某一侧没说就跳过"（皆是旧口子）。

### 11.16be M9b（2026-09-23）：pass 协议接到内容层——宿主经 SDK 真的画出内容

M9a 立起来的是"骨架"：会话起来、空帧呈递、画的一半一律报"还没服务"。这一片把**画的那一半**接上：SDK 的
pass 协议（`beginPass/endPass/setPassOrder/setViewport/setClearPolicy/setDepthMode/setLights/render`）翻进计划
的**收集段**（`core::FrameRecorder`），`swapBuffers()` 再把编译好的计划交给**内容世界**（`ContentAssembly`）组装
并录制。至此一条真实的宿主路径成立：`beginPass` → … → `render(commands, camera)` → `endFrame` → `swapBuffers`
画出内容并被会话呈递。

**pass 的身份：`api/PassRegistry`（新件）**。SDK 的 pass 身份是**对象**（它的 `beginPass()` 注释写着"retain 了
per-pass GPU 状态的后端按这个对象作键"），计划的身份是**小整数**（`core::PassId`：计划、执行器内容包、录制日志
都按它索引）。注册表就是这两者之间的那一层：同一个对象**终生同一个号**（warm-up 里第一次公告与第一帧里公告
拿到的是同一个号）；**号永不复用**——死 pass 的号可能还留在某处（正在录制的计划、执行器的日志），发给新 pass
会让那些记忆描述错的对象；`releasePass()` 只**忘身份**（今天没有按键保留的 GPU 状态，"释放"就是这一件事）。

**帧外的调用 = 惰性，但身份留下**。引擎在 `initialize()` 之后、**任何帧之前**跑一次 warm-up：每个 enabled、
非清屏的 pass 完整执行一遍（`beginPass` → per-pass 状态 → `render` → `endPass`），好让后端把保留状态准备好。
这个语义下这些调用**不是协议错误**：本层只把 pass 身份注册下来（跨帧有效），收集段一律不碰；有帧在开时才对
录。判罚留给 recorder 自己的协议（比如"帧里不带范围就画"⇒ 它拒并报 `PassProtocolViolation`，每帧一次）。

**内容世界随会话生灭**。`initialize()` 在会话起来之后按它的设备建 `BlockStorage`/`MaterialImages`/
`ContentStore`/`ContentAssembly`（以及跨帧复用的 `core::VariantPool`），`shutdown()` **先拆它们**再拆会话
（GPU 对象属于那台设备）。`render()` 在收到调用时把命令点名的对象（geometry / material / program）**追踪**进
store——对象对调用是借用的，而表要在计划录制时回答它们（store 自己持一份所有权，宿主随后释放自己的句柄也不
悬空）；`setDefaultContentProgram()` 同时告诉 recorder（没有自己程序的命令按它画）和 store（表要回答它；SDK 给
的是 `intrusive_ptr<const>`，store 要的是可变句柄——白名单式 `const_cast`，store 只读）。

**清屏的翻译**：SDK 的策略是**字节 + 深度标志**，计划的是**浮点 + 深度值**（reverse-Z 远 = 0）。颜色按字节
原样折算（不套传递函数——测试读出窗口时的约定），深度只带标志（SDK 没有深度值可给，给一个就是第二处会搞错
裁剪约定的地方）。

**每帧的内容驱动**（`swapBuffers()`，都在 `recordContent()` 一处）：计划里每个**画进窗口**（`target == nullptr`）
且有绘制调用的 pass 产出一个内容包——按**声明的输入**数给等长的空 `InputImages`（内容录制器按声明逐项读，
短一截会读过界；"没人提供"是它自己会报的事实），视图块按该趟第一个绘制调用的相机 + `Session::frameSeconds()`
+ 窗口的实时尺寸建，兼容性取窗口的形状；离屏 pass 不产包（执行器会报它，内容给了也没人录）。报告之后**内容
替换**由 M9b 的缺陷修复负责（见下）。

**本片撞出的真缺陷：窗口内容会在帧之间累加**。执行器的窗口臂把每趟 pass 的内容 `addContent` 进**保留**的视图
（那是"稳定视图 id / 管线复用"要留的东西），但没有任何东西清它：第二帧会把第一帧的节点再画一遍、并且无限增长
（既画错了画，又是内存漏）。以前的设备用例只驱动**一帧有内容的帧 + 若干无 pass 的帧**，所以看不见；M9b 的用例
在两帧之间**移动相机**就一眼可见（左半的旧三角还在）。修法：`WindowTarget` 的视图下面添一个**本帧内容组**——
`beginFrame()` 清它、`addFrameContent()` 往里加；执行器在每帧**第一趟窗口 pass**（就是它已经记着的那一处）调
`beginFrame()`。**宿主的会话根不受影响**（`addContent()` 仍旧挂在视图上、活到会话结束），而**没有窗口 pass 的
帧**（M9a 的空帧路径）不调它——所以空帧照旧把上一幅画**再呈递一次**（M8o 的用例依赖这个）。

证据（真设备 `VsgBackendTest.TheSdkPassProtocolDrawsContentIntoTheWindow`，宿主窗口 128×96）：

* **warm-up**：整段范围带 `render` 在帧外执行 ⇒ 零报告、零呈递，且注册表里已经有这个 pass（`live()==1`）；
* **第一帧**：同一段范围在帧内执行 ⇒ 零报告；执行器 `recorded()` 里恰好一条、其号**就是** warm-up 那一个；
  两帧 settle 后读像素：左半三角形（片元把 `cam_pos.x = 0.5`、窗口宽 128、高 96 编进三个色字节）＋右半清屏
  （0, 0.25, 0）——视图块、清屏翻译、兼容性、追踪，一条链全被像素证明；
* **第二帧**（同内容同相机）：`halves().builds()` 与 `sets().sets()` 都不动（稳态零构建）；
* **第三帧**（相机移到 -0.5）：settle 后**左半变回清屏**（上一帧的画真的不在了）、右半出现新三角 —— 直指上面的
  累积缺陷；`deviceWaits()==0`；
* **身份生命周期**：`releasePass()` 后 `contains()==false`、`live()==0`；重新公告得到的是**新号**。
* 无设备侧：`PassRegistryTest`（同对象同号、两对象不同号、释放只答一次、同一地址的新对象拿新号、null 不计）。

`VsgBackendTest` 的第一个用例同步改写：pass 协议与内容不再报"未服务"（协议回路改由**帧内无范围**
`render()` ⇒ recorder 报一次 `PassProtocolViolation` 来证），未服务清单缩到五种（离屏目标、pass 输入、全屏
程序、两条读回、活着的 resize），每种重复调用**不再**重复报。

变异 **7/7 红**：①帧首不换窗口内容（复现累积缺陷）；②不组装内容（只剩清屏）；③`beginPass` 不开范围（计划
里没有 pass）；④清屏色不翻译；⑤命令对象不追踪（表回答不了 ⇒ 报告 + 拒录）；⑥`releasePass` 不留身份；⑦
warm-up 的调用进了录制器（帧外调用变成一串拒判）。门禁 **655 用例 / 100 套件**、0 VUID / 0 SYNC-HAZARD、
hygiene 0 / 856、相位 11 行 / 2 次运行。

**本片留下的口子（登记）**：①离屏目标 / pass 输入 / 全屏程序 / 读回仍报"未服务"（下一片）；②门面仍不接
`MaterialManager` 风格的材质编辑通知——`ContentStore::updateMaterial()` 存在但 SDK 的 `RenderBackend` 面里没有
入口，等**工厂切换**时由宿主侧接线（登记）；③活着的 `resize` 只报不改；④集合与半片停靠窗口各自独立、窗口
`facts()` 的 live 采样与 `refresh()` 成功臂不可驱动、设备半边"某一侧没说就跳过"（皆是旧口子）。

### 11.16bf M9c（2026-09-23）：离屏那一半——目标、输入、全屏、重建

M9b 让宿主经 SDK 画出**窗口**的内容；这一片把**离屏**那一半接上：`supportsRenderTargets()` 转真，宿主的
`RenderTarget` 被持有、被画、被当输入采样，并且**形状变化真的触发重建**。至此门面的 SDK 面只剩读回与"活着的
resize"。

**`api/HostTargets`（新件）：宿主目标的描述快照、对象与事实**。三件事只有这一层能同时拿住：

* **描述是快照，不是宿主指针**。SDK 的契约是"指针只在调用期间有效，宿主在 `releaseRenderTarget()` 之后就可
  以销毁对象"，所以这里存的是**拷下来的描述**（每次点名按字段比对更新——同目标每帧公告是零分配），之后不再
  解引用任何宿主对象。`RenderTarget stays a logical description` 是 SDK 自己的话。
* **对象懒建**。描述够了才建（正值尺寸 + 至少一个彩色附件）；建不成与"还缺描述"**分开作答**：
  `NotBuilt`（配置未完成的正常状态——宿主先配后画）、`DepthSourceMissing`（借的深度来自本后端没持有的
  lender）、`BuildFailed`（完整却建不出来）。建成后的尺寸/形状变化是**计划的答案**（`planTarget`），由执行器
  应用——这一层从不自己重建。
* **`facts()` 是计划解析目标与输入的那张表**。引擎半边永远是宿主的原话；**设备半边只在附件正是为这套引擎形状
  建的时候**才说（形状变了就没有"设备的拼写"可说——"不知道"不能读成"没有"，M8r 的规矩）；深度的事实：建成后
  promotion 按**目标自己的 `layout()`** 报（重建保留的是它自己那份），借来的深度 `borrowed` + `source`，且
  **永不是 borrower 的承诺**；shadow 声明（`shadowOf()` 的灯 + `hasProducerViewProjection` + 矩阵）原样搬进
  `ShadowFacts`，从不推导。

**借来的深度**（`shareDepth`）：borrower 的条目持**lender 的 `shared_ptr<OffscreenTarget>`**——SDK 那边
`shareDepth` 自己保活源对象，这边是配套的另一半：lender 被 `releaseRenderTarget()` 释放时，注册表丢掉它的条目，
但 borrower 还把它撑着，图像不会在飞行中被销毁。lender 必须**先被公告**（SDK 的调用序契约就是"源在同一帧更早
渲染"），所以 lender 的解析发生在**公告的那一刻**，不是事后去找。

**门面侧**：`setRenderTarget()` 建/注册（`executor.addTarget(identity, target)`）/把**身份**交给 recorder；
只报 `DepthSourceMissing`/`BuildFailed` 一次（`Ready` 后 `rearm()`——同一个毛病再犯还报），`NotBuilt` **静默**
（宿主先配后画；真需要的 pass 由执行器报"没告诉过我这个目标"）。`endFrame()` 的事实表在窗口之后**逐条目补
行**（建没建都算——计划要能说"这个还没有对象"，而且编译器解析 **pass 的输入**用的就是同一张表）。`setPassInputs()`
把每个非空输入 `observe()`（刷新描述——对象是它被画进去时建的，要新的是**事实**）后整表转交。`drawScreenProgram()`
追踪它的**片元程序**（表要回答它的两份文本）后转交；源是**身份**，哪张图在 `setPassInputs` 那一步说过。
`releaseRenderTarget()` 忘条目 + 注销执行器（那是借来的指针，绝不能悬）+ 让 recorder 丢掉挂起的公告。

**内容驱动**按**每趟自己的目标**取兼容性与尺寸：窗口那趟用窗口的形状，宿主目标那趟用**该目标**的形状——所以
离屏 pass 的管线与它自己的 render pass 对齐；每个**声明输入**给一条 `InputImages`：该目标的彩色视图（逐附件）
＋ 计划说可采样时的深度视图（三个 scratch 容器复用，稳态不分配；每趟按声明数给等长条目，短一截会让录制器读过
界）。

**证据**（真设备 `VsgBackendTest.TheSdkOffscreenTargetIsDrawnIntoSampledAndRebuilt`，宿主窗口 128×96，
宿主目标 64×64）：

* 一帧两趟：内容画进目标，SDK **自带的 `screenCopyProgram(0)`** 把它复制到窗口——窗口像素是**红三角 ＋ 目标
  自己的清屏绿**（不是窗口的清屏蓝），左/右/角落三处都对（外围两字节按**集合**读：这个服务器发的是 BGRA 序，
  红在最后一个字节——测试自己的坑，不是后端的）；
* 建成目标的 facts：`wanted` 的引擎半边=宿主的话、**设备拼写非空**、`current.built == true`（第一趟录过就算）；
* `setSize(32, 48)`：计划答 ResizeInPlace ⇒ 执行器应用 ⇒ **目标真的是 32×48**，画还在；
* `attachColor` 加第二个附件：形状变 ⇒ 计划答 Rebuild ⇒ 目标 2 个附件、屏幕仍采附件 0；
* 借一个刚被释放的 lender：报一次（三次公告一条情节）、条目的 `target == nullptr`。

无设备侧 `HostTargetsTest`：描述是快照（对象改了、事实不动，直到再次公告）、`NotBuilt`、借来的深度不是
borrower 的承诺、shadow 声明原样旅行、释放只答一次。

第一个用例的"未服务"清单缩到**两条读回 + 活着的 resize + 无会话 + 内容世界不在**；`supportsRenderTargets()`
翻身，所以**引擎现在会把离屏 pass 真的交给这个后端**。

变异 **7/7 红**：①宿主目标永不建；②离屏目标不进事实表（编译器解析不了输入）；③`setRenderTarget` 丢身份
（**12 条 VUID**——同一帧两次把窗口当目标）；④离屏趟不录内容（**2 条 VUID**：屏上复制一张没写过的图）；
⑤输入不给图；⑥释放不清（用例数一下 `live()`）；⑦形状变了还说旧的设备格式（**2 条 VUID**：管线与 render pass
不再相容）。门禁 **658 用例 / 101 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 859、相位 11 行 / 2 次运行。

**本片留下的口子（登记）**：①**读回**仍是"未服务"（下一片：capture 的录制策略 + 一次被计数的设备等待 + 字节
取出）；②**借了深度的目标**（以及有 borrower 的 lender）会被 `OffscreenTarget::rebuild`/`resize` 的租约拒绝——
宿主的 `shareDepth` 组合要保持同尺寸，形状变了得先释放再重建（写在这里，免得当成"重建坏了"）；③活着的目标上
翻转 `depthPromotion` 不会被应用（目标的 `layout()` 说了算，事实跟着目标走——要变就得释放重建）；④活着的
`resize` 只报不改；⑤集合与半片停靠窗口各自独立、窗口 live 采样与 `refresh()` 成功臂不可驱动、设备半边"某一侧
没说就跳过"（皆是旧口子）。

### 11.16bg M9d（2026-09-23）：宿主的读回——两个来源一张图

M9c 之后门面的 SDK 面只剩读回与"活着的 `resize`"。这一片把**读回**接上：`readColorBuffer` /
`readDepthBuffer` 转真，宿主的离屏目标第一次能被**同步**读出字节与深度；"未服务"清单再缩一格，只剩
**活着的 resize + 无会话 + 内容世界不在**。

**两个来源，一张图（`api/HostReadback`，新件）**。执行器**已经**在每帧全部 pass 之后给每个目标接上它
自己的拷贝节点（彩色目标拷附件 0、只有深度的目标拷深度）——"探针读的是帧的最终画面"（M8s 的规矩）。
所以：

* 帧**已经拷过**的附件：**不用再提交任何东西**——停一次设备、读映射缓冲即可；
* 帧**没拷过**的（彩色目标的深度、第二个彩色附件）：在这一层用目标**自己的拷贝命令**提交一次（自己的
  command buffer + fence，100 秒上限）。这是合法的，因为**被画过的目标处在确定布局**；而**没画过的
  目标**——拷贝它的图像就是读 `UNDEFINED` 内存、再把垃圾叫"图片"——答 `NotRecorded`（SDK 的 `NotReady`）。

**顺序即契约**：

1. **先分类**（`classifyReadback`，不需要设备）：未知附件、读不了的格式、没录过的目标——**不能服务的
   请求一分钱不花**（连下面那次设备等待都不花）；
2. **调用者停设备**：写缓冲的那一帧可能还在飞；这次停**被计数**（`SessionContentAccess::waitDeviceIdle`
   → `deviceWaitIdle()` + `retirement.noteDeviceWait()`，与 `Session::deviceWaits()` 同一个计数器）——
   读回是**唯一允许停设备的路径**，"恰好停一次"保持可查；
3. 只有帧没拷过才在这里拷（**在停之后**，两次提交不可能重叠）；
4. 探针读映射缓冲，把字节/浮点交给宿主（`PixelProbe::pixels()` / `DepthProbe::values()` 交出原缓冲，
   门面不再多拷一次）。

**门面侧**：拒绝**两个频道一起说**（`why` 给机器答案、诊断路由给一句话），**每个情节一次**——目标自己的
情节挂在它的条目上（`HostTargets::Entry::readback_report`），解析不了的目标共用一个每入口点的情节
（`readback_reports[kReadbackColour/kReadbackDepth]`）；一次成功的读回**结束情节**（`rearm()`）。
借来的深度：**从 borrower 读是 `Unsupported`，要去源目标读**（SDK 自己的规矩；映射属于 lender，这里
从不承诺）。`readbackResultOf` 是一张表：未知目标/没建好/没录过 ⇒ `NotReady`（"以后可能可以"）、空目标/
未知附件 ⇒ `Invalid`（"这个请求做不出来"）、读不了的格式/借来的深度/没设备 ⇒ `Unsupported`（"这个后端
永远不会"）、搬运失败 ⇒ `Failed`；**未映射的落到 `Failed`，永不落到 `Ok`**。诊断类别沿用旧实现的
`ContentSkipped`。

**本片撞出的真问题：只有离屏 pass 的帧从没跑过窗口那棵图**。与 M9a 同一课、换了张脸：计划里**一个窗口
pass 都没有**时，执行器只录了宿主目标的命令——被 acquire 的窗口图像从头到尾没跑过 render pass，停在
`UNDEFINED`，呈递就是 **VUID 01430**（实测 **2 行**，gtest 全绿）。修法两半：
`WindowTarget::prepareWithoutClear()`（**只重开渲染区**，清屏值保持原样——它补的是"图像被写过"这一步，
不是画的新一帧）+ 执行器在**没有任何窗口 pass** 的帧尾把窗口图**不加壳**挂进命令图（**没有 pass 可以给
它归因**，注释里写明这是呈递需要的，不是内容）。

**真设备用例**（`VsgBackendTest.TheSdkReadsBackItsOwnTargetsPixelsAndDepths`）：64×64 的 RGBA8+D32F
目标画一帧三角，然后

* `readColorBuffer` ⇒ `Ok`、`64*64*4` 字节、三角内 (16,40) 是 `(255,0,0,255)`、外面 (48,8) 是目标的
  清屏绿 `(0,64,0,255)`、alpha 255（**离屏读回是 RGBA 序**——窗口面那个 BGRA 是测试服务器的坑，见 M9c）；
* `readDepthBuffer` ⇒ 4096 个 float、`depths[40*64+16] ∈ (0.05, 0.95)`（三角处）、
  `depths[8*64+48] == 0.0F`（清屏处）、全部在 [0,1]；
* `deviceWaits()`：读彩色 `+1`、再读深度 `+2`——**每次读回恰好停一次**；
* 拒绝面（全部**不加等待**）：附件 5 ⇒ `Invalid`、未知目标 ⇒ `NotReady`、RGBA16F 的目标 ⇒ `Unsupported`
  （持有且建成，连帧都不需要）、从 borrower 读借来的深度 ⇒ `Unsupported`、**lender 建成但没画过 ⇒
  `NotReady`**、释放过的目标 ⇒ `NotReady`。

变异 **6/6 红**：①没画过的目标读起来像能服务（3 行）；②只有离屏的帧不跑窗口图（**2 条 VUID**——用例
抓不住、门禁抓住）；③**两道"没录过"的闸一起拆**（3 行；只拆一道是**绿**的——分类那一道先答，说明两道闸
在**不同来源**上各管一半：帧拷过的附件本来就不需要"录过"，要靠一提交路径上的那道闸拦）；④借来的深度从
borrower 读（3 行）；⑤读回的停不计（3 行）；⑥帧不把自己的目标拷回来（**63 行**：执行器与相位一起红）。
门禁 **659 用例 / 101 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 861、相位 11 行 / 2 次运行。

**本片留下的口子（登记）**：①活着的 `resize` 只报不改（下一片可选）；②借了深度的目标、以及有 borrower
的 lender 会被 `OffscreenTarget::rebuild`/`resize` 的租约拒绝（M9c 的登记，不变）；③一个**帧没拷过**的
附件上的读回会走一次提交路径——真要"零额外提交"的宿主自己保证先画过（这正是 `NotRecorded` 的意义）；
④旧口子（集合/半片独立、live 采样与 `refresh()` 成功臂不可驱动、设备半边"某一侧没说就跳过"）不变。

### 11.16bh M9e（2026-09-23）：活着的 resize——跟随表面

门面的 SDK 面到 M9d 只剩"活着的 `resize`"一条"未服务"。这一片把它接上，**门面不再有任何未服务的入口
点**：那两个仍会报一次的调用（无会话的帧、内容世界未起来的 `render()`）不是"这个后端不会"，而是"没地方
可去"——句子也跟着改了（"先要有一个会话"）。

**语义来自 SDK 的授权顺序**：`RenderBackend::resize` 写着 surface > announcement > default——**表面自己拥有
尺寸**，所以活着的一条尺寸公告不是命令，而是"跟随"：`SessionContentAccess::followResizedSurface` 让窗口
做它自己的那件事（`window->resize()`：重读所在表面的几何 + 重建交换链），挂在尺寸上的东西**下一帧从窗口
现读**（`WindowTarget::prepare` / `prepareWithoutClear` 每帧写 `renderArea`、目标形状、每趟 pass 的视图
块），所以没有第二份尺寸要同步。重建**停一次设备**（vsg 的 `buildSwapchain()` 先 `vkDeviceWaitIdle` 再销毁
旧交换链——与 M8q 丢帧修复同一笔开销），这次停**被计数**：活着 resize 是宿主的例外，计数器让它保持可见。
公告的数字仍是**下次 `initialize()` 建窗的尺寸**；自开窗口那一半"应用公告"没做（它没有窗口系统事件、也
没有别人能改它——旧实现同一口径；登记）。

**本片量到的真机理（M1 变异顺带抓到）**：表面变了却不公告（或后端不跟随）时，vsg 的 **Viewer 自己**会在
acquire 发现 `_extent2D` 与交换链不符（`Window::acquireNextImage` 直接答 `OUT_OF_DATE`），于是在**提交里**
调 `window->resize()` 重建——可那一帧**已经按旧矩形录完了**：`VUID-VkRenderPassBeginInfo-pNext-02852/02853`
（render area 128 > framebuffer 96，实测 4 行）+ 段错误。跟随发生在**录制之前**，这正是这一片存在的理由。

**门面侧**：`resize()` 非正值直接返回（不是表面能有的尺寸）；活着 ⇒ 跟随（零报告）；未起来 ⇒ 只记公告。
`kUnservedResize` 槽整条删除；`BackendContentAccess::windowTarget()` 作为测试视图（"表面被跟上了"与
"旧尺寸还在"可由它区分——`WindowTarget::width()/height()` 是**现读**）。

**真设备用例**（`VsgBackendTest.TheWindowFollowsItsHostsSurfaceThroughALiveResize`，宿主窗口 128×96）：
片元把**表面尺寸写进画面**（`frame.y/160`、`frame.z/192`，两个尺寸下都落在 [0,1]）——"跟没跟上"于是由
**像素**说，不只是形状说：

* 前：三角在左（相机看它右边半个单位），外围两字节 = 128 与 96 的编码，右四分之一是清屏绿；
* `backend->resize(0, 0)` ⇒ **一分钱不花**（不跟随、不计数）；
* `host.resize(96, 64)` + `backend->resize(96, 64)` ⇒ 恰好 **1 次**被计数的停；`WindowTarget` 报 96×64；
  零报告；
* 后：同一段内容在新尺寸下重画，外围两字节 = 96 与 64 的编码、清屏与三角都在**新**位置；宿主窗口活着。

第一个用例（`TheSdkFacingBackend...`）的相应步骤改写：活着 resize **不再报**（诊断数不变）、
`deviceWaits()==1`（跟随的代价）；重生日后窗口确实是公告的 320×180。

变异 **5/5 红**：①**不重读表面**（不重建交换链）⇒ **4 条 VUID（02852/02853）+ 段错误**（上面那条机理）；
②重建发生了但**停不计数**（3 行红）；③**活得公告不记**（下次 `initialize()` 回到默认 640×360，3 行红）；
④ `0×0` 闸拆掉（3 行红）；⑤**渲染区冻在第一个尺寸**（4 条 VUID + 段错误，红）。门禁 **660 用例 / 101 套件**、
0 VUID / 0 SYNC-HAZARD、hygiene 0 / 861、相位 11 行 / 2 次运行。

**观察（不是本片引入，登记以免误判）**：本片验证期间全量套件**偶发**在 `SessionTest` 的两个用例上红：
`vkCreateSwapchainKHR` 收到**未初始化的表面能力**（`preTransform` 垃圾位、`imageExtent` 宽 1891005984、
`VkSwapchainCreateInfoKHR-*` 共 10 条 VUID）随后抛异常——那是 `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`
失败的症状；**同一棵树**再跑即绿（`SessionTest` 单独跑 5/5、去掉本片用例全量跑 4/4 绿），而且 `SessionTest`
在二进制里**跑在本片用例之前**，与本片无因果。登记为环境级偶发（X/lavapipe），下一次见到先重跑再查。

**本片留下的口子（登记）**：①**自开窗口不"应用"公告**（没有窗口系统事件能改它，也没有它的主人：公告仍
在下次 `initialize()` 生效——SDK 那句 "owns its surface ⇒ applies the announcement" 在这一半未实现）；
②宿主**不公告**表面变化时不会被跟（vsg 会在提交里补，代价是那一帧错 + VUID/崩）——不能报尺寸的宿主要自己
保证公告；③旧口子（借来的深度租约、活目标翻 `depthPromotion`、集合/半片独立、live 采样与 `refresh()` 成功
臂、设备半边"某一侧没说就跳过"）不变。

### 11.16bi M9f（2026-09-23）：工厂切到门面——"vsg" 就是重写版

最后一步：`VsgRenderBackendFactory::create()` 不再造 `VsgRenderer`，而是造 `vine::vsg::VsgBackend`（门面）。
从此**注册名 "vsg" 的含义就是重写版**——插件 load → 注册表 → `create()` 这条生产路径上跑的是这一路 M0–M9e
建起来的东西；被替换的实现仍在树里、仍由自己的测试驱动，但**没有任何名字会创建它**（第二个名字就是"哪个是
vsg"的第二个答案，而这正是重写要消灭的东西）。注册实际有**两条路**：`GfxBackendVsgPlugin::load()` 显式注册
一个 `s_factory`，而 `VsgRenderBackendFactory.cpp` 里的静态 `Registrar` 在模块被载入时就自己注册了同一个名
字（后注册的赢，两者造出的东西一样）——变异 M3 就是把**两处一起**关掉，只关一处处仍然绿。

**切换逼出来的半片（必须一起交）**：老实现**每帧**刷新它命令到的每个材质（`SceneBridge` 对每个 distinct
material 调 `VsgMaterialManager::updateMaterial`，而那个方法**自己就是 compare-and-write**——它的测试原话
是 "refreshes in place, and only when something changed"）。重写版是**拉**模型：`ensureMaterial` 只在
`described_revision == revision` 时跳过，而 `revision` 只有 `updateMaterial()` 会推——**而重写版里没有任何人
调它**（§11.16be 登记的就是这一条：SDK 的 `RenderBackend` 面里没有"材质变了"的入口，引擎也不持有
`MaterialManager`）。不补的话，切完工厂**宿主的材质编辑就不进画**（老实现的核心行为之一，静默丢）。所以本片
把两半一起交：

* 门面 `render()` 对**每条命令的材质**调一次 `ContentStore::updateMaterial()`——"在哪被点名，就在哪被注意
  到"，与老实现同一处、同一频率；
* `updateMaterial()` 从"盲增修订"改成 **compare-and-write**：读材质**现在**的字段，与表里那一行携带的块
  **按 ABI 的逐成员比较**（`materialBlockAgreesWith`；**不能比字节**——块的尾部填充是故意不写的，比字节会把
  "没变"报成"变了"，§11.16k 的老坑），只有**不同**才推修订，于是"下一帧 `tablesFor` 换掉那一行、把旧值停
  靠"照旧；稳态帧只比不建、**不分配**，首帧之后的每帧开销就是几个浮点比较。块的字段映射收成一处
  （`blockOfMaterial`），`buildMaterialFacts` 与比较共用同一份拼写。

**证据**：

* `VsgBackendPluginTest.CreateBackendByName` 用 `dynamic_cast<vine::vsg::VsgBackend*>` 把"造出来的是门面"
  钉住（旧工厂会一模一样地通过 `!= nullptr`）；
* `VsgBackendPluginTest.TheRegisteredBackendComesUpOnTheHostsSurfaceAndDraws`：**通过注册表**拿对象（不是直接
  `new`）、采纳宿主窗口、初始化、走 SDK 协议画一帧（pass + 一个三角）、呈递、把**宿主窗口的像素**读回来
  （左半红三角、右半清屏绿）——生产路径端到端，切换只有它的终点能证；
* `ContentStoreTest.ATouchWithNoEditChangesNothing`（无设备）：触碰没编辑 ⇒ 零构建、零停靠；编辑后再触碰
  ⇒ 恰好一行、旧值被停靠；
* `VsgBackendTest.AMaterialEditLandsOnTheNextFrameAndASteadyFrameRebuildsNothing`：内容用**材质自己的
diffuse** 着色（顶点阶段只把位置透传，三角形因此落在窗口中间）——帧 1 读到 (0.25, 0.5, 0.75)；宿主只改材质、
  **什么都不通告** ⇒ 帧 2 的 `builds()` 恰好 +1 且像素质变成 (0.75, 0.25, 0.5)；再来一帧什么都不改 ⇒
  `builds()` **不涨**、画面不变、`deviceWaits()==0`。

`BackendContentAccess::store()` 作为测试视图（`builds()` 是"这一帧建了东西"的读数，与上一片的
`windowTarget()` 同一个理由）。

**夹具教训（当场抓到的）**：块的 canonical 绑定是 **View=0 / Draw=1 / Material=2**；把 `VineMaterialBlock`
写在 binding 0（视图块的位置）会被 ABI 扫描判成"两阶段对同一绑定的说法不一致" ⇒ 内容层整趟拒绝（日志里
"no compiled content half was built for this pass"，画面上只剩清屏色）——不是后端的缺陷，是夹具的拼写错。

**变异 6/6 红**：①名字又造旧实现（`CreateBackendByName` 的 cast 先红）；②`create()` 答空；③**两条注册
路径一起关**（`PluginRegistersVsgBackend` 红）；④注册的名字不是 "vsg"；⑤触碰改成盲增（
`ATouchWithNoEditChangesNothing` 红：稳态帧每帧重建、还停靠）；⑥门面**不再触碰**材质（材质编辑用例红：
像素停在旧颜色）。门禁 **663 用例 / 101 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 861、相位 11 行 / 2 次运行。

**变异夹具的一课**：插件**独有**的源（工厂、插件入口、旧实现）**不在 `test_vsg` 目标里**，所以
`ninja -C build test_vsg` 不会重编它们——第一次跑这六个变异**全绿**，就是因为被测的仍是**旧的插件 .so**。
电池必须 `ninja -C build test_vsg gfx_backend_vsg`（插件的 api/core 源同时编进测试目标，所以 M9e 的电池没
踩到这一条）。已写进仓库记忆。

**本片留下的口子（登记）**：①旧实现仍在树里（测试驱动）——删它是独立一步（要连它的源文件、公共头与测试
一起处理）；②引擎侧真的跑起来（真窗口、真场景）没有在本片里做过：证据是"注册表 → 门面 → 宿主窗口一帧"这
条路径，**宿主侧的接线**（怎么把场景/材质编辑接到这个后端）属于引擎那边；③旧口子（自开窗口不"应用"公告、
宿主不公告表面变化不会被跟、借来的深度租约、活目标翻 `depthPromotion`、集合/半片独立、live 采样与
`refresh()` 成功臂、设备半边"某一侧没说就跳过"）不变。
