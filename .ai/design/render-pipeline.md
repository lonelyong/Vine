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
| **S2** | 阴影：消费 `Light::castShadow`/`ShadowSettings`（已在 SDK 里躺着）；每个投影方向光一个 depth-only pass；forward / deferred 都接采样 | 新像素相位（立方体投到地面 + PCF）+ 无投影光时证据不变 |
| **S3** | SSAO：证明"效果槽"这个抽象本身；deferred 直接用 G-buffer 深度，forward 走深度 prepass | 新像素相位（角落变暗）+ 默认关 ⇒ 证据不变 |
| **S4** | PBR：**不是 pipeline 步**，是 program + 材质 ABI（`VineMaterialBlock` 扩 metallic/roughness） | 材质块 `static_assert` + 图像对照 |

**不做（附触发条件）**：

- ❌ 自动帧图定序（生产者/消费者推 order）：等 §14 的图像语义走完，且出现"人工排 order 排不对"
  的实例；今天 `PipelineStage` 已经让效果不必抢数字。
- ❌ `PassContext` pull 重构（`graphics-render-pipeline.md` §11）：仍等"多 pass 完全成熟"。
- ❌ `Pipeline::applyOptions()` 的 diff：等真有"运行中改结构"的消费者（今天是重建换手）。
- ❌ `PipelineManager` / 全局管线注册表：§4.3。

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

### 8.2 S2 进度（2026-09-13）：地基已落，绑定与像素门禁待做

已完成（各自独立提交、门禁绿）：

| 步 | 内容 | 判据 |
| --- | --- | --- |
| S2-1 | L1 阴影 ABI：`VineShadowBlock`（`ShaderAbi.hpp`）+ `RenderTarget::setProducerViewProjection()`（矩阵**只由产出者写一次**，消费者读） | `test_graphics` +1（size/align/offset）；证据不变 |
| S2-2 | **pass 输入通道**：`resolvePassInputs()` 交回解析结果，引擎在 `beginPass()`/`execute()` 之间 `RenderBackend::setPassInputs()`（新虚函数，默认空） | `test_graphics` +1（声明两项 → 按序到达、未产出为 null） |
| S2-3 | 延迟光照的**带阴影变体**：程序文本由 `deferred_light.frag` 的**两行标记**插出（不是 `#ifdef` —— 全屏 program 声明的绑定就是后端必须提供的），无阴影变体是"源去掉标记"，两者都不带脚手架 | `test_graphics` +1（标记 + binding **5/6**）；`vine_shader_check` 7 PASS |

**下一步的精确清单**（机械执行，不需要再决策）：

1. ✅ **后端绑定**（`gfx_backend_vsg`）：已落地。`FullscreenShadowInput`（图 + 块）+ `makeFullscreenProgramNode` 的新参数；可填槽位变成**集合**（源彩色 / 可采样源深度 / 阴影两槽），集合外声明在建任何东西之前就被拒；矩阵 = `target->producerViewProjection() × camera->viewMatrix().inverted()`；bias 取自投影那盏灯的 `ShadowSettings`。
2. ✅ **builder**：已落地。`makeDirectionalShadowMatrix()`（**唯一一份**光相机推导）+ `PipelineStage::Depth` 的 depth-only 图；光照 pass 用 `deferredLightProgram(castShadow != nullptr)` 并声明该图；宿主自带 lighting program ⇒ 不建阴影 pass 并报一条。
3. ⬜ **像素门禁（下一步，唯一剩下的）**：`vsg_selftest` 新相位，照 `runProgramShadingPixelPhase`（`main.cpp:5112`）的骨架 —— `FrameScope`/`PassScope` + `renderer.render(commands, camera)` + `readTarget(renderer, target, &image)` + `PixelImage::at(x,y,ch)`；场景 = 地面大四边形 + 悬空立方体 + 斜射的 `castShadow` 太阳；断言**影内的地面像素明显暗于影外**。**变异验证**：把 `sun->setCastShadow(false)` ⇒ 该相位必须红。证据基线行数 +1~2（`vsg_selftest_evidence.txt` 用 `--update` 重生成，并逐条核对只有新行是新增的）。

**注意**：相位用 builder（需要 `RenderEngine`）还是手工搭（要用到只存在于 builder 里的光相机推导，会**复制**那唯一一份推导）——**必须选前者**，否则门禁本身就违反了 §9 的决定。若 `RenderEngine` 与该相位的直驱 `VsgRenderer` 混用有冲突（引擎与直驱各有一份 slot 账本），就在该相位里单独建一个 `RenderEngine` 包住同一个 renderer，并在相位结束时 `engine.shutdown()`。

**原第 1 条的细节（已实现，保留为记录）**：

   - `makeFullscreenProgramNode`（`VsgPipelineFactory.cpp:~780`）加一个额外输入参数：`std::vector<std::pair<::vsg::ref_ptr<::vsg::ImageView>, bool>>`（视图 + 是否深度），**绑定号 = `color_count + 1 + i`**；`VineShadowBlock` 的 UBO 绑在 **`color_count + 2`**（规范 4 彩色 ⇒ 5 / 6）。`provided` 的白名单检查同步放宽，`declaredBindings()` 反射已经在做"声明即要求"。
   - `drawScreenProgram`（`VsgOverlay.cpp:~470`）取 `state.request.inputs` 里**第一个非空**的 target，用与 `src` 相同的方式拿它的深度视图（要求 `depth_sampleable`），并填 `VineShadowBlock`：`viewToLight = target->producerViewProjection() * inverse(pass camera viewMatrix)`，`params = {1, bias(来自该光源 ShadowSettings), 1, 0}`。**矩阵一律从 target 读，不重算光相机**（§9 的决定）。
2. **builder**（`RenderPipelineBuilder::buildDeferredPath`）：内容里有 `enabled && castShadow && Directional` 的灯时 —— 建光相机（正交、`Scene::boundingBox()` 取景）+ `resolution²` 的 **depth-only** target（`setDepthPromotion(true)` + `setProducerViewProjection(lightVP)`）+ `PipelineStage::Depth` 的 pass 画内容（`content_`）；光照 pass 用 `deferredLightProgram(true)` 并 `addInputTarget(shadow)`。**没有投影光时一字不改**（今天的程序与 pass 列表）。
3. **像素门禁**：`vsg_selftest` 新增相位 —— 地面 + 立方体 + 投影的太阳，读回后断言**影内的地面像素明显暗于影外**；变异验证：把太阳的 `castShadow` 关掉 ⇒ 该相位必须红。证据基线行数 +1~2。

**为什么停在这里**：1 与 2 必须与 3 一起提交 —— 只做 2 会让 `makeFullscreenProgramNode` 拒绝那个程序（声明了没人提供的绑定），于是"光照 pass 不画" + 一条 Error，比不做更糟；只做 1 则没有消费者可验。三件一起做完才是一个可判定的切片。

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

**光相机**：正交；eye 沿 `-direction` 退到内容包围盒（`Scene::boundingBox()`）之外，center 取盒心，
正交窗口 = 盒在该光空间的范围 + 余量；由 `ShadowSettings::resolution` 定方图尺寸的 depth-only target
（`setDepthPromotion(true)`，后端已有 depth-only render pass 与深度采样路径）。

**分期**：

| 步 | 内容 | 判据 |
| --- | --- | --- |
| **S2a（延迟）** | 全屏 program ABI 扩一条规则：**源自己的绑定之后**，接该 pass 声明的额外输入（纹理绑定）与 `VineShadowBlock`（UBO）；`deferred_light.frag` 加阴影项（**程序带 define 的变体**，见下）。SSAO 将来走同一跳 | 新像素相位（立方体在地面上的投影：影内的地面像素明显暗于影外）+ 无投影光时证据逐字节不变 |
| **S2b（前向）** | 内容 set 声明 `vine_shadow`(UBO) + `shadow_map`(sampler2D)；槽从 pass 输入 target 填块 + 绑深度；`std_forward.frag` 同样的变体 | 同上（前向路径的像素相位）|

**为什么先做延迟**（2026-09-13 修正：起初的判断反了）：**全屏 program 的 ShaderSet 是每个 pass 现建的**
（`makeFullscreenProgramNode` 为这个程序建一套绑定，程序文本带 define 就能决定要不要声明阴影绑定），
所以延迟路径**不需要动共享的 set**；而**内容 set 是按 (target, 深度档) 会话级共享的**
（`state.depth_on_shader_set`），一个带阴影的 pass 和一个不带阴影的 pass 会要两套 set —— 前向那条要么多 6 套
缓存 set（3 深度档 × 有/无阴影，窗口 + 每离屏目标），要么永远声明绑定并绑一张 1×1 深度占位。两条都做完，
§2 的"阴影是效果"才算真的兑现。