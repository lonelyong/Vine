# RenderPipeline 设计（管子线的形状、效果与生命周期）

> 状态：**设计稿 v1（2026-09-13）**；S1（本设计的“形状与生命周期”部分）落地中，见 §7。
> 前置：`render-pipeline-builder.md`（Builder 作为薄配方层的定位仍然有效；其 Design C 的
> `PipelinePreset` 枚举由本文 §2 取代）、`graphics-render-pipeline.md`（pass 调度、命名产出槽、
> `ImageRef` §14）、`graphics-shadow.md`（阴影的语义模型）、`graphics-shader.md` §11（L1/L2/L3
> 后端无关 ABI）。
>
> **一句话**：`Pipeline` 是**一棵小 pass 图 + 它注册过的东西的句柄**，不是继承体系；改变它的只有
> 两件事 —— **路径**（几何怎么被着色）与**效果**（在哪些 pass 之间插什么），二者正交，所以用
> **选项 + 阶段（stage）**表达，不用子类。

## 0. 结论

| 问题 | 结论 |
| --- | --- |
| 派生 `ForwardPipeline` / `DeferredPipeline`？ | **不**。两个 recipe 函数 + `ShadingPath` 选项（§1） |
| 派生 `ShadowPipeline` / `SsaoPipeline`？ | **不**。它们是**效果**：往既有的 pass 图里插 pass、给既有 pass 加输入（§2、§5） |
| PBR 是不是一种 pipeline？ | **不是**。它是**着色模型** = program + 材质 ABI（§2） |
| 效果怎么插？ | **阶段（`PipelineStage`）+ 插入顺序**，取代魔数 order（§3） |
| 谁负责注销 pass？ | **`Pipeline` 自己**（RAII）——今天它只持引用不持注册，是个真缺陷（§4） |

## 1. 为什么不派生

1. **轴是正交的**：路径（forward / deferred）× 阴影（N 个投影光）× SSAO（开/关）× 泛后处理 ×
   HUD。单继承只能表达一个维度，`Deferred + SSAO + 阴影` 需要 2ᵏ 个子类或菱形继承。
   正交的东西**组合**，不做类层级。
2. **`Pipeline` 是数据不是行为**：它持有的是“pass 列表 + target + 句柄”。行为在
   `RenderPass::execute` 与后端里；派生一个不覆写任何虚函数的类，只是把数据换成类型名
   （`copilot-instructions.md`：avoid unnecessary abstractions）。
3. **本仓已经就同类问题拍过一次板**：着色从 `ShaderPreset` 枚举改成**命名 program**
   （`6d9063a`）。**PBR 是着色模型**，必须走同一条先例 —— 那不是 pipeline 的维度。

**规则**：只有当"取值集合封闭、且每种取值携带不同的**行为/状态**"时才派生。这里唯一封闭的集合是
**着色路径**（2 值），而它携带的是**拓扑**（一串 pass）= 数据。

## 2. 四个轴各自归哪一层

| 轴 | 归谁 | 形态 | 现状 |
| --- | --- | --- | --- |
| **着色路径**（forward / deferred） | pipeline 结构 | `ShadingPath` 选项；两个 recipe 函数 | ✅ 有，但被塞进 `PipelinePreset` 枚举（本设计删掉枚举） |
| **阴影** | **光的数据**（`Light::castShadow()` + `ShadowSettings`）+ pipeline 认领 | 按内容里 `castShadow && enabled && Directional` 的光生成 depth-only pass，并在着色 pass 上声明输入 | ⚠️ 数据在 SDK 里**已存在但无消费者**（Design B 删掉了自动调度） |
| **PBR / 着色模型** | **program + 材质 ABI** | `ShaderAbi.hpp` 扩 `VineMaterialBlock`（或第二块）+ `pbrProgram()` | ❌ 不是 pipeline 的事 |
| **SSAO / 泛后处理** | **效果** | 消费几何产出的深度/法线，产出一张图，供着色 pass 读 | ❌ 需要"结构跟着效果变"的能力（本设计给的就是这个） |
| **HUD**（gizmo / fps） | 效果（已如此） | `Overlay` 阶段 | ✅ |

**必须承认的结构后果**：SSAO 需要"可采样的深度"。deferred 免费（G-buffer 有）；**forward 没有**
（窗口 pass 的深度不可采样），所以 `forward + SSAO` 必须变成"深度 prepass → AO pass → 消费 AO 的
前向 pass"。同理 `forward + 阴影` 要在前向 pass 里采 shadow map。**这正是"效果改拓扑"的实证**，
也是为什么需要阶段而不是几个 if。

## 3. 形状：选项 + 阶段

```cpp
enum class ShadingPath { Forward, Deferred };   // 内容怎么被着色（唯一封闭的结构轴）

struct PipelineOptions {
    ShadingPath path = ShadingPath::Forward;
    // deferred 路径的两张图与两段 program（forward 不读）
    int offscreen_width = 0, offscreen_height = 0;
    intrusive_ptr<ShaderProgram> gbuffer_program, lighting_program;
    AxisGizmoOptions gizmo; FpsOverlayOptions fps;
};
```

**阶段取代魔数**。今天 builder 用 `-3 / 0 / 1 / 2 / 10 / 30`，效果一多就没人能保证自洽（阴影要在
所有消费者之前、SSAO 要在几何之后光照之前）。所以顺序由**意图**声明：

```cpp
enum class PipelineStage {
    Depth,        // 只产出深度供后续采样：阴影 depth map、深度 prepass
    Geometry,     // 不透明内容（deferred 的 G-buffer pass）
    Effect,       // 读几何产物的屏幕空间效果（SSAO）
    Shading,      // 受光结果：forward 的内容 pass，或 deferred 的光照 pass
    Transparent,  // 只走 forward 的内容，叠在受光结果之上
    Present,      // 把烤好的结果呈现到窗口（deferred+透明的那条链）
    Overlay,      // HUD
    Preview,      // 宿主/调试用的预览屏（PiP、逐附件预览）：在所有东西之上，含 HUD
};
constexpr int pipelineStageOrder(PipelineStage) noexcept;
```

- **同一阶段内**：按规划顺序（`addPass` 插入序；引擎对相同 order 是稳定排序）。
- **跨阶段**：由 `pipelineStageOrder()` 的档位决定。档位之间留空隙，新 pass 不会挤掉别人。
- **宿主手写 pass** 仍可用裸 order（app_shell 的手搓 demo 就是这样），但**推荐**用阶段值；
  裸数字与档位的关系在本文 §6 的表里写清。

## 4. 生命周期与管理

### 4.1 一个真缺陷：`Pipeline` 只持有引用，不持有注册

`Pipeline::~Pipeline() = default`，而 `RenderEngine::addPass` 另持一份 `intrusive_ptr`。后果：
**丢掉 Pipeline 不会注销任何 pass，它们继续每帧执行**。今天 `SceneView` 必须手写
`removeWindowPass()`，而且它**只删 window pass** —— 转 deferred 后 G-buffer / 光照 / 合成 / HUD
全成孤儿（今天只建 forward 所以没炸）。

**修法（S1）**：`Pipeline` 记下自己注册过的每一个 pass，析构时 `removePass`。它同时**强引用
engine**（`intrusive_ptr<RenderEngine>`）—— 这不是循环（engine 不认识 pipeline），而是把
"passes 注册在那个 engine 里"这个依赖说清楚：**pipeline 活着，它注册用的 engine 就必须活着**。

### 4.2 改选项的两档

| 档 | 例子 | 机制 |
| --- | --- | --- |
| **活的** | pass 开关、target 尺寸、program 热换 | `RenderPass::setEnabled` / `Pipeline::resize` / `ScreenPass::setProgram`（已有） |
| **结构性的** | 加/减一个效果（阴影、SSAO） | 重建 pipeline 并换手；组装只是接线，GPU 资源本来就是懒建（`Pipeline::applyOptions()` 的 diff 化留到真有第二个消费者时再做，见 §6 不做清单） |

### 4.3 谁持有

保持"**创建者持有**，一个 view 一条 pipeline"（`SceneView` 已经如此），**不做**全局
`PipelineManager`：管线是配置，不是资源池；需要共享的是**资源**（网格/纹理/材质），那已经由会话级
缓存负责（`graphics-perf-backlog.md` P8/P9）。

## 5. 效果怎么接（三个例子）

| 效果 | 插在哪 | 读什么 | 写什么 |
| --- | --- | --- | --- |
| **阴影** | `Depth`（每个投影方向光一个 depth-only pass） | —— | 每光一张深度图 |
| | 着色 pass 加输入 | 那些深度图 + 光相机 VP | —— |
| **SSAO** | `Effect` | `Geometry`/`Depth` 产出的深度（+ 法线，deferred 有） | 一张 AO 图 |
| | 着色 pass 加输入 | AO 图 | —— |
| **HUD** | `Overlay` | —— | 窗口（或目标）子视口 |

- 效果**不写死 order**：它声明"我在 `Shading` 之前"（`Effect`）与"我读谁的输出"（`ImageRef` /
  `addInputTarget`，§14 已落地第一步）。
- 效果**不猜输入**：基路径把"我产出了什么"声明出来（deferred 的 G-buffer pass 已经
  `setOutputTarget(gbuffer)`），效果读那份声明；读不到就说（引擎的 wiring 校验 + 每帧
  "未产出"上报）。
- 效果**不静默降级**：要了但现在建不出来（例如阴影还没实现）⇒ 报一条，并说清"你拿到的画面是
  没影的"。

## 6. 分期

| 步 | 内容 | 判据 |
| --- | --- | --- |
| **S1** | §2/§3/§4 落地：`ShadingPath` 取代 `PipelinePreset`；`PipelineStage` 取代魔数；效果函数化（`buildPath` / `applyOverlays` / 阴影的"请求-报告"）；`Pipeline` RAII 注销 | **行为中性**：证据基线 **53 行逐字节不变**；`test_graphics` / `test_vsg` 计数；lavapipe PASS |
| **S2** ✅ | 阴影：消费 `Light::castShadow`/`ShadowSettings`（已在 SDK 里躺着）；每个投影方向光一个 depth-only pass；forward / deferred 都接采样 | 落地记录见 §8.2（延迟）与 §8.3（前向）：两条路径各一个像素相位 + 无投影光时证据逐字节不变 |
| **S3** | SSAO：证明"效果槽"这个抽象本身；deferred 直接用 G-buffer 深度，forward 走深度 prepass | 新像素相位（角落变暗）+ 默认关 ⇒ 证据不变 |
| **S4** | PBR：**不是 pipeline 步**，是 program + 材质 ABI（`VineMaterialBlock` 扩 metallic/roughness） | 材质块 `static_assert` + 图像对照 |

**不做（附触发条件）**：

- ❌ 自动帧图定序（生产者/消费者推 order）：等 §14 的图像语义走完，且出现"人工排 order 排不对"
  的实例；今天 `PipelineStage` 已经让效果不必抢数字。
- ❌ `PassContext` pull 重构（`graphics-render-pipeline.md` §11）：仍等"多 pass 完全成熟"。
- ❌ `Pipeline::applyOptions()` 的 diff：等真有"运行中改结构"的消费者（今天是重建换手）。
- ❌ `PipelineManager` / 全局管线注册表：§4.3。
- ❌ **宿主自选离屏 target 的前向管线**：前向路径只 presenting 到窗口，而窗口读不回来
  （`readColorBuffer` 拒绝 null target）——S2b 的像素门禁因此**重定向** window pass 才量到画面
  （§8.3）。触发条件：出现"画面要渲进纹理"的消费者（预览、反射、录屏），那时给 `PipelineOptions`
  或 `Pipeline` 一个离屏落点，并把门禁改回它自己声明的那种用法。

## 7. 决策记录

1. **不派生**（§1）；pipeline = 数据 + 句柄。
2. **两个正交的入口**：`ShadingPath`（结构）与效果（附加），其余（阴影分辨率/偏差、PBR 材质）
   属于**光**与**材质/program**，不进 pipeline 选项。
3. **阶段取代魔数**：顺序是**意图**（"阴影在所有消费者之前"），不是数字。
4. **`Pipeline` 持有注册**（RAII）+ 强引用 engine：谁注册谁注销。
5. **效果不静默降级**：建不出来就报，并说清画面与请求的差距（延续 `UnsupportedRequest` 的口径）。
6. 本文取代 `render-pipeline-builder.md` 的 Design C 中 `PipelinePreset` 那部分；Builder 作为
   "薄配方层"的定位不变。

## 8. 落地记录

### 8.3 S2b（前向阴影）（2026-09-13）：同一个 pass，另一条 hand-off

**这一步要解决的是 S2a 留下的那个选择**（§9 的"分期"里写死的两难）：内容 set 是按
`(target, 深度档)` 会话级共享的，所以"带阴影 / 不带阴影"不能是一套 set 的两种变体。

| 决定 | 内容 | 理由 |
| --- | --- | --- |
| 内容 set **永远声明**阴影两槽（set 0 / binding 3 = `shadow_map`，4 = `vine_shadow`），程序文本也永远声明 | 一个 set 一份，不翻倍；宿主程序不需要"阴影孪生版"就能拿到这两个绑定 | 变体方案要给每个 target 再翻一倍缓存，而且**宿主自己的内容程序没有阴影变体可挑**——那条路只能靠"永远声明"兜住 |
| 关闭时绑 **会话的白回退**（不是 1×1 深度图）+ 块 `params.x = 0` | 声明了的绑定必须**写进描述符**（未写入的 set 是非法的），而"白回退 + 关闭的块"是一次性资源、且 shader 永不采样它（`params.x` 就是那个开关，`ShaderAbi.hpp` 写着它存在的理由） | 少一件设备资源、少一条"占位图"约定 |
| 前向的 hand-off = **内容 pass 自己声明输入** | 前向没有 G-buffer、没有全屏 program，map 只能作为**内容 pass 的输入**交给后端 | 引擎的 `resolvePassInputs` / `setPassInputs` 通路（S2a 建的）原样复用 |
| 一个**解析规则**给两个消费者 | `detail::resolveShadowInput(state, camera, lights)`：`VsgOverlay`（延迟光照）与 `VsgContentSlot`（前向内容）都调它 | 两个消费者各推一遍"哪个输入是图、矩阵怎么乘"就是两份只在被改之前一致的约定 |

**门禁（`runForwardShadowPixelPhase`）**：同一个场景、同两个采样点、同三级亮度（背景 15 /
影内 31 / 阳光下 196），只把管线换成前向 —— 两条路径一比，差的就是**路径**。证据 **54 → 55 行**
（只多这一行）。

- **读回**：前向路径presenting 到窗口，而窗口读不回来（`readColorBuffer` 拒绝 null target），
  所以相位**把 window pass 重定向**到自己的离屏 target。pass 列表、它依赖的阴影 pass、声明的输入、
  着色全是 builder 的，只有落点变了。（宿主想自己渲到离屏目前**没有**支持的口子，已记进 §6 不做清单。）
- **变异**：删掉 `buildForwardPath` 里那行 `addInputTarget(shadow_map)` ⇒ 影内读到 196（红）。
  加上 S2a 的四个变异（关 `castShadow`、去掉 v 翻转、去掉 z 反转、相机改回裸指针），
  这十数行着色文本与两处 hand-off 都各有一条会红的路径。
- **结构门禁**：`ARequestedShadowIsBuiltOnTheForwardPathToo`（阴影 pass 先画、内容 pass 落到窗口、
  矩阵 == 唯一推导、正交且非视图相机、输入被解析）；`TheForwardProgramDeclaresTheShadowAbiWhereTheContentSetBindsIt`
  钉前向文本的 binding **0/3/4**（内容路径的号，不是全屏路径的 5/6）。
- **新增的一条上报**（否则是静默无影）：宿主自己的**内容程序**若没有声明 `shadow_map`，
  它就拿着一张自己不读的图 ⇒ 每 build 一次变体报一条 `UnsupportedRequest`（`programDeclaresBinding`
  从全屏路径反射那套搬进共享工具，两个消费者同一条规则）。

**顺带修的两个"说法与事实不符"**：

1. `reportRequestedShadows()` 在 S2a 改成"只报没兑现的"之后，前向路径**也建了**阴影 pass ⇒ 那句
   "本管线不建阴影 pass" 的对象现在只剩"宿主自带 lighting program"。测试随之改名
   （`ARequestedShadowIsReportedAsUnbuilt` → `AShadowThisPipelineCannotShadeIsReported`）并改断言。
2. `forwardProgram()` 与 `flatForwardProgram()` 现在都从**同一份**带 ABI 的片元文本派生
   （flat 只是多一个 define）——原来是 flat 走**原始**文本，那会让 flat 预设静默丢掉阴影。
   测试同时钉住这两件事（"flat 去掉 define 行 == forward 文本"）。

**诚实的边界**：`buildVineShaderSet` 的声明是无条件的，所以**每一个**内容管线都多带
一个采样器 + 一个 UBO（即使场景里没有投影光）；不投影时它们绑的是白回退 + 关闭的块，永远不会被采样，
但布局确实变大了。这是"一份 set"换来的代价，写在 §9 的决定里。


### 8.2 S2 进度（2026-09-13）：S2a（延迟阴影）落地（S2b 见 §8.3）

已完成（各自独立提交、门禁绿）：

| 步 | 内容 | 判据 |
| --- | --- | --- |
| S2-1 | L1 阴影 ABI：`VineShadowBlock`（`ShaderAbi.hpp`）+ `RenderTarget::setProducerViewProjection()`（矩阵**只由产出者写一次**，消费者读） | `test_graphics` +1（size/align/offset）；证据不变 |
| S2-2 | **pass 输入通道**：`resolvePassInputs()` 交回解析结果，引擎在 `beginPass()`/`execute()` 之间 `RenderBackend::setPassInputs()`（新虚函数，默认空） | `test_graphics` +1（声明两项 → 按序到达、未产出为 null） |
| S2-3 | 延迟光照的**带阴影变体**：程序文本由 `deferred_light.frag` 的**两行标记**插出（不是 `#ifdef` —— 全屏 program 声明的绑定就是后端必须提供的），无阴影变体是"源去掉标记"，两者都不带脚手架 | `test_graphics` +1（标记 + binding **5/6**）；`vine_shader_check` 7 PASS |
| S2-4 | 后端绑定 + builder：`FullscreenShadowInput`（图 + 块）；**可填槽位成为集合**（源彩色 / 可采样源深度 / 阴影两槽），集合外声明在建任何东西之前就被拒；`PipelineStage::Depth` 的 depth-only 图；光照 pass 声明该图。矩阵 = `target->producerViewProjection() × camera->viewMatrix().inverted()`，**不重算光相机** | 证据逐字节不变（无投影光时一字不改）；`test_graphics` +1（输入按序到达） |
| S2-5 | **光相机的唯一推导成为 SDK 公开静态**（`directionalShadowMatrix`）：pass 用它、target 记它、着色用它，**相位也必须用它**——复制一份就是"两份只在被改之前一致"的推导 | `test_graphics` +1（盒角全落在正交窗口内 + eye 在光源一侧；两次变异皆红） |
| S2-6 | **像素门禁**：`vsg_selftest` 的新相位 `runDeferredShadowPixelPhase`（引擎驱动 builder 的 deferred 配方：阴影 pass → G-buffer → 光照 → 合成），读回 composite 的像素 | 证据 **53 → 54 行**（只有新行是新增的）；变异 4/4 全红 |
| S2-7 | **结构门禁**：`ARequestedShadowIsBuiltOnTheDeferredPath` 钉阴影 pass 的形状（depth-only、灯要的分辨率、`depthPromotion`、正交光相机、stated 矩阵 == 唯一推导、光照 pass 的输入被解析） | `test_graphics` +1 |

**像素门禁怎么做的**（`runDeferredShadowPixelPhase`）：

- **必须走 builder**，所以这一个相位（也只有这一个）把 renderer 交给一个 `RenderEngine`：它验的是
  builder 的**配方**（`castShadow` 的灯 → `PipelineStage::Depth` 的 depth-only pass → target 记下矩阵 →
  光照 pass 声明该图 → 引擎 `resolvePassInputs`/`setPassInputs` → 后端的绑定），手工搭一份只能证明
  "后端会绑一张图"，证不了配方本身。
- 相位跑在原直驱 teardown **之后**（`backend->shutdown()` 之后）：引擎的 `initialize()` 建自己的一份 session，
  结束时 `engine->shutdown()`，两份 slot 账本从不同时活着。
- 场景：地面大四边形（y=0，法线 +y，**镜面黑**：影子只乘漫反射项）+ 立在它上面的墙（y∈[0,2]，z=0）+
  斜射的 `castShadow` 太阳（沿 (0.6,-1,0.4)）+ 一台俯视的相机（自检的平视相机看见的地面是一条线）。
- **读回 composite**：窗口读不回来（`readColorBuffer` 拒绝 null target），而 deferred 路径只有在**有前向内容
  要合成**时才把受光结果烘到离屏 target。所以相位给一个**空的** transparent 场景：那条前向 pass 什么都不画，
  composite 因此正好是受光的不透明明面。窗口之外的第二个好处：合成路径的两个分支都被走到。
- **两个采样点**：同 world z=0.4、x 镜像（+0.3 在影内 / −1.0 在阳光下）——同一行（相机无 roll，两点 y、z 相同），
  像元列号由这台相机的投影**算一次**写死（`343/240`），不在运行期"找最暗的像素"（那正是错位的影子也满足的判据）。
- **三级亮度让判据无歧义**：背景（程序写死的 0.06 灰 ≈ 15）、影内地面（只有环境填充：0.8 反照率 × 0.15 ≈ **31**）、
  阳光下地面（≈ **196**）。断言：阳光下 > 300 总和；影内**每通道落在 [20,45]**（既是"画了"，又是"只有环境项"）；
  影内比影外至少暗 200。最后一条把"两边都没画"和"画了但没光照"都挡在外面。

**门禁抓到了四个缺陷**（都是先复现、再修、再用同一条门禁证明修好的；每个都单独变异验证过）：

| # | 症状 | 根因 | 修法 |
| --- | --- | --- | --- |
| 1 | 宿主被告知"本管线不建阴影 pass"——**而延迟路径已经建了** | `reportRequestedShadows()` 只看"有没有请求"，不看"建没建" | builder 记 `shadows_built_`，只报**没兑现**的那部分（前向路径、宿主自带 lighting program） |
| 2 | 阴影深度图**全空**（着色器读到 `caster = 0`） | pass **不持有**相机（`RenderPass::camera_` 是 `raw_ptr`），而 builder 的光相机是 `buildDeferredPath` 的局部变量——build 一结束就释放，阴影 pass 从此透过**已释放的内存**绘制 | pass 强持相机（与它强持 render target 一致）；`setCamera()` 的签名不变 |
| 3 | 整幅画面只剩环境光：**太阳"不见了"** | 块里的矩阵是 SDK 的裁剪约定（近 0 / 远 1），图里存的是后端的**反向 Z**（近 1 / 远 0）：空图（clear = 远平面 = 0）对任何片元都判"更深" | 取样前换算：`frag = 1 - (z*0.5+0.5)`，比较方向与 bias 同向翻转（ABI 说明写进 `ShaderAbi.hpp`） |
| 4 | 太阳回来了，但影子落在**别处**（采到了镜像的 texel） | SDK 的裁剪空间 y 向上，而后端的图 `v = 0` 在世界**上**方（同一个把 G-buffer 摆正的事实） | `map_uv = light_uv.xy * vec2(0.5,-0.5) + 0.5`（同上，写进 `ShaderAbi.hpp`） |

**变异验证（4/4 全红，每次都单独 build + 跑）**：`sun->setCastShadow(false)` ⇒
"影内的地面读到 (196,196,196)"；去掉 v 翻转 ⇒ "影外的地面读到 (31,31,31)"（影外的像素被判成影内，正是镜像
texel 的样子）；去掉 z 反转 ⇒ 影内 196；把 pass 的相机改回裸指针 ⇒ 影内 196（深度图又空了）。

**诚实的边界**：

- 门禁量的是**画面**，不是图：`readDepthBuffer` 拒绝 `D24_UNORM_S8_UINT`（既有能力边界，见自检的
  "packed D24 honestly unsupported"），而 builder 的阴影图正是 D24。要直接看图就得上 D32——那是另一件事。
- 投影物是一张**没有厚度**的墙：它证明的是"光相机取景 + 反向 Z + v 轴 + 绑定 + 比较"这条链，不是体积阴影；
  自阴影（地面自己投在自己身上）由 `ShadowSettings::bias` 挡住，不在这条判据里。
- 采样像元是这台相机投影的**手算结果**；换场景/换相机就要重算（相位里写着这几行是怎么来的）。

### 8.1 S1（2026-09-13）：形状与生命周期

| 环节 | 落点 |
| --- | --- |
| 路径 | `PipelinePreset`（4 值，含 2 个占位）→ `ShadingPath { Forward, Deferred }`，进 `PipelineOptions::path`；`build(const PipelineOptions&)` 取代 `build(preset, options)` |
| 阴影 | 占位枚举**删除**，不是改名：请求改由**灯**提出（`Light::castShadow` + `ShadowSettings`），builder 扫 content / transparent 两个场景里 `enabled && castShadow` 的灯，有就报一条 `UnsupportedRequest`（"画面无影"）。**比原来覆盖更广**：以前只有点名 `*Shadowed` 预设才报，宿主直接给灯设标志是**静默**的（app_shell 的 demo 就是这样） |
| 顺序 | 新增 `PipelineStage` + `pipelineStageOrder()`：`Depth -100 / Geometry -40 / Effect -20 / Shading 0 / Transparent 20 / Present 40 / Overlay 60 / Preview 100`；builder 的每个 pass（含 HUD、含 PiP 配方）都按阶段落位，HUD 的 `order` 降级为**阶段内**堆叠偏移 |
| 生命周期 | `Pipeline` 改 RAII：`Pipeline(intrusive_ptr<RenderEngine>)`（引擎是构造不变量）+ `addPass()` 注册并记账 + 析构**逐个 `removePass`**；成员 `intrusive_ptr<RenderEngine>` **强持**引擎（无环：引擎不认识 pipeline） |
| SceneView | 手写的 `removeWindowPass()` 只剩"放开句柄"——那正是 RAII 修掉的那个缺陷：旧版只删 window pass，转 deferred 后 G-buffer / 光照 / 合成 / HUD 全成孤儿 |
| AppShell | `VINE_PIPELINE=*_shadowed` 不再选另一条管线，改成给场景里的方向光设 `castShadow`，于是走上面那条"请求-报告"通路 |

**判据（全部绿）**：自检证据 **53 行逐字节不变**（自检手搓自己的 pass，但 app demo 跑的是
deferred 默认路径 —— 档位重排后 lavapipe 无 VUID/validation 错误）；`test_graphics` 248 → **250**
（+`StagesOrderThePasses` 钉档位单调、+`DroppingThePipelineUnregistersItsPasses` 钉 RAII，
`ShadowedPresetsReportThatTheyArePlaceholders` 改写为 `ARequestedShadowIsReportedAsUnbuilt`）；
`test_vsg` 247；`vine_shader_check` PASS；lavapipe PASS。

**诚实的遗留**：`addOffscreenToScreen`（PiP 配方）仍直接向 engine 注册并返回裸 `ScreenPass*`，
那两条 pass 没有句柄负责注销 —— 与 S1 修的是同一个缺陷。它改成返回 `Pipeline` 需要同时给宿主
一个"稍后锚定这个 ScreenPass"的路径，等第二个消费者出现时一起做（触发条件已写进 §6 不做清单）。
## 9. S2 阴影：三个 ABI 决定（2026-09-13）

阴影要跨过三样东西：**深度图**、**光空间的矩阵**、**每 pass 的参数**。每一件都要先决定走哪条路，
否则实现会退化成"某个后端的私有约定"。

| 问题 | 决定 | 理由 / 先例 |
| --- | --- | --- |
| 光空间矩阵怎么到着色器 | **`VineShadowBlock`**（`ShaderAbi.hpp`，view→light clip 的 mat4 + `params`），**走 UBO 不走 push** | push 只保证 128 B，`LightPushBlock` 已经正好占满 128（先例：灯走 UBO 就是这个原因）。矩阵用 view→light 而不是 world→light，是因为两条路径手上都有**片元的 view 位置**（前向是 varying，延迟是 G-buffer 附件） |
| 矩阵由谁算 | **产出者算一次**：建光相机的 pipeline `RenderTarget::setProducerViewProjection(lightVP)`；消费者（前向的槽、延迟的着色）从**输入 target** 读 | 否则"建相机的地方"和"填块的地方"各推一遍同样的正交取景，两者一旦有差就是画面错而 validation 干净。放在 target 上也说得通：投影过的图**本来就**带着它的投影 |
| 深度图怎么到着色器 | pass **声明输入**（已有 `RenderPass::addInput` / `ImageRef`），后端在两个消费者处绑定 | 这是 §5 承诺的"效果声明输入"，不是新机制；缺的是"解析后的输入怎么告诉后端"这一跳 |
| 那一跳 | **`RenderBackend::setPassInputs(const std::vector<RenderTarget*>&)`**，默认 no-op，引擎在 `execute()` 前调用 | 照 `setLights` / `setDefaultContentProgram` 的先例：新增虚函数带默认空实现，既有实现者不受影响 |

**不改的**：`getPassInputs` 不做（pass 输入不是查询对象）；**超 128 B 的 push 不做**（保证之外）；
**多投影光源不做**（`graphics-shadow.md` §8 已拍板单方向光/内容）；**级联/PCSS 不做**（
`ShadowSettings::filter` 先只兑现 `Hard` 与 `PCF`）。

第五条决定是**像素门禁逼出来的**（§8.2 的缺陷 3 / 4），因为它不属于"矩阵怎么来"，而属于"矩阵
和图各自活在什么坐标系里"：

| 问题 | 决定 | 理由 |
| --- | --- | --- |
| 块里的矩阵是什么约定 | **SDK 的裁剪约定**：x 向右、y 向上、z 近 0 远 1（`Camera::projectionMatrix()` 本来就产出这个，于是矩阵在哪个后端都同义） | 让"产出者算一次"名副其实：矩阵是**数学**，后端只管它自己的光栅化约定 |
| 图里存的是什么 | **后端的约定**：vsg 后端是反向 Z（近 1 远 0）+ 自上而下的图（v=0 在世界**上**方） | 这是后端的事实，SDK 改不了也不该假装没有；`ShaderAbi.hpp` 里把这**两个轴都要换算**写成了 ABI 说明（`xy * vec2(0.5,-0.5) + 0.5`、`1 - (z*0.5+0.5)`），因为两个符号写错的后果都不是报错而是画面（一个丢太阳、一个采到镜像 texel），只有像素门禁看得见 |

**光相机**：正交；eye 沿 `-direction` 退到内容包围盒（`Scene::boundingBox()`）之外，center 取盒心，
正交窗口 = 盒在该光空间的范围 + 余量；由 `ShadowSettings::resolution` 定方图尺寸的 depth-only target
（`setDepthPromotion(true)`，后端已有 depth-only render pass 与深度采样路径）。

**分期**：

| 步 | 内容 | 判据 |
| --- | --- | --- |
| **S2a（延迟）** ✅ | 全屏 program ABI 扩一条规则：**源自己的绑定之后**，接该 pass 声明的额外输入（纹理绑定）与 `VineShadowBlock`（UBO）；`deferred_light.frag` 加阴影项（**程序带标记插出的变体**，见下）。SSAO 将来走同一跳 | 新像素相位（墙在地面上的投影：影内的地面 (31,31,31) vs 阳光下 (196,196,196)）+ 无投影光时证据逐字节不变 → **证据 53 → 54 行**，`test_graphics` 250 → 255，lavapipe PASS（§8.2 有落地记录与它抓到的四个缺陷） |
| **S2b（前向）** ✅ | 内容 set **永远声明** `shadow_map`(set 0/3) + `vine_shadow`(set 0/4)；槽从 pass 输入 target 解析图与块（关掉时绑白回退 + `params.x = 0`）；`std_forward.frag` 用**同一段**标记插出的项，`forwardProgram()` 与 `flatForwardProgram()` 从同一份文本派生 | 前向像素相位（同场景同采样点：影内 31 / 阳光下 196）→ **证据 54 → 55 行**；`test_graphics` 255 → 257、`test_vsg` 247 → 248；变异：删掉输入声明 ⇒ 影内 196（红）。决定与代价见 §8.3 |

**为什么先做延迟**（2026-09-13 修正：起初的判断反了）：**全屏 program 的 ShaderSet 是每个 pass 现建的**
（`makeFullscreenProgramNode` 为这个程序建一套绑定，程序文本带 define 就能决定要不要声明阴影绑定），
所以延迟路径**不需要动共享的 set**；而**内容 set 是按 (target, 深度档) 会话级共享的**
（`state.depth_on_shader_set`），一个带阴影的 pass 和一个不带阴影的 pass 会要两套 set —— 前向那条要么多 6 套
缓存 set（3 深度档 × 有/无阴影，窗口 + 每离屏目标），要么永远声明绑定并绑一张 1×1 深度占位。两条都做完，
§2 的"阴影是效果"才算真的兑现。**两条路径都做完了**：前向那条选了"永远声明 + 运行时开关"
（§8.3 记了为什么，以及它的代价：每个内容管线多带一个采样器 + 一个 UBO），门禁按 S2a 的形状
做（同一个场景、同两个采样点、只换管线），变异也照做。

阴影之后 S2 还剩一件事没做：**性能 / 画质那半**（分辨率与 PCF 的实际效果、`ShadowSettings::filter`），
它们不改变 ABI，等有真实的画面需求再动。