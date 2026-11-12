# graphics / vsg 后端 —— 现役要点

> 只记**现役**：机制、判据、坑，以及“当时为什么这么定”。逐片历史与重写前那一代在 git；
> 设计文档在 `.ai/design/`，**唯一待办清单**是 `.ai/backlog/graphics.md`。
> 更细的后端契约见 `.ai/design/vsg-backend.md`，坑表见 `.ai/design/vsg-pitfalls.md`。

## 1. 分层与一帧

- SDK：`src/viz/graphics/sdk/vine/graphics`；vsg 后端：`src/plugins/gfx_backend_vsg`（`core/` 纯策略 + `api/` vsg 适配 + `support/`）。
- `RenderEngine`：生命周期 / `frame()` / 设备借用 / 尺寸 / 能力与报告；三个门面是 `PassRegistry`、`OutputRegistry`、`DiagnosticsRoute`。
- `frame()` 返回 `FrameOutcome`（Ran / NotInitialized / NoDevice）；pass 的内容住在 `PassDesc::content`，`bindPassContent` 返回是否命中。
- 节点 / pass / 几何的 `Object` 派生类型判断用 `obj_cast<T>`（core 元类型链），不要用 `dynamic_cast`；`CameraManipulator` 与第三方 vsg 类型例外。
- `PassChain`：一张画面的配方 + 注册归属。`PassChain::build(engine, ChainDesc)` 建链并注册 pass；析构把手里的 pass 交还引擎；`SceneView::setChain` 采纳它。
- 一帧：`beginFrame` → 每个 pass（`beginPass` / `setPassOrder` / `setPassInputs` / `execute` / `endPass`）→ `endFrame` → present。
- 调度只在引擎（`detail::FrameScheduler`：迭代 Tarjan + 堆就绪队列，稳态零分配）；后端按交来的 `FrameDescription.passes` 物化，不再重排、不报环。
- 后端契约：`RenderBackend.hpp` + `ShaderAbi.hpp`；`backend/` 半的类型在 `core/`、`api/`。
- **一帧两个半段**：内容半段（钩子 / publish / describe）在调用线程，引擎半段（drain / bind / expand / execute /
  present）在引擎线程；两块帧值轮换、帧号随帧走（`detail::EngineFrame`）。**宿主默认线程化**
  （`SurfaceWindow` 开局就 `setFrameThreading(EngineThread)`），`RenderControl::setFrameThreading(Inline)` 是回退口。
- 线程化后的可感知代价（别当 bug 报）：画面比内容**晚一帧**（N15）；`frame()` 每次最多等**一个引擎半段**（含
  acquire/present），所以 UI 被渲染节拍节流——"不阻塞"指的是不付展开与整帧，不是不被节流。
- **编辑批次**：`Scene::beginEdit()` / `endEdit()`（或 `Scene::Edit` 作用域）把镜像比较的版本按住 —— 批内帧不发布、
  画批次开始时的状态，整批在之后那一帧一次落地（C18/R17，机器人关节状态的硬要求）。查询（拾取、`boundingBox`、
  直接采集）在批内仍看当前内容；`version()` 返回的是 `published_epoch_/published_boxes_`，不是 `bake_epoch_`。
- **指针查询按"用户看到的那一帧"算**（N17）：`RenderEngine::presentedCamera()` 发布被答完那一帧的相机快照，
  `SceneView::setPresentedCamera` 交给 manipulator，`OrbitCameraManipulator::makeScreenRay` 优先用它；没有发布值就退回
  当前相机（静止画面 / inline）。一个像素 → 世界射线是 `CameraSnapshot::screenRay`（两端反投影，透视正交同一套）。
- 呈现结局是值：`PresentOutcome{Presented, Skipped, Lost}` + `RenderEngine::presentRecord()`（帧号+结局，一个原子字）。
  `Lost` 由**引擎**修（忘掉已告知的尺寸 → 下一帧重报 → 后端重建交换链），宿主只记一条日志、不拆会话、不藏区域
  （藏区域会停掉修复帧）。设备动词（`createSurfaceTarget` / `setWindowHandle` / `initialize`）是**帧间**操作。
- 采集备忘录：`detail::DrawListMemo` 被 `Scene` 与 `RenderWorldSource` 共用（键 = 视图投影/眼点/视口/层掩码，状态 =
  表 revision，令牌 = 帧号，一帧宽限；令牌 0 = 不记）。见证在引擎侧：`RenderEngine::contentCollectCount/ReuseCount`。
- 竞争/寿命门禁：`scripts/tsan_check.sh`（`build-tsan`，`-fsanitize=thread`，**0 条警告**才算过）。它顺手抓到过
  `removePass` 的 use-after-free 与"`*collectRenderCommands(...)` 直接进 range-for"（临时 `shared_ptr` 在 range
  初始化结束就销毁）。

## 2. 内容模型

- `Geometry` = 通道集；`Primitive` = 一次 draw（topology + index span + material / program + instance stream）。
- 索引与实例流只写在 `Primitive`；`Geometry::material` / `program` 是**几何级默认**（图元自己的值优先，再否则最近 `StateNode`）。
- 通道 = `BufferSlice<float>` + components。`setPositions` / `setNormals` / `setTexcoords2` / `setTexcoords3` 共享 buffer；arena 用 slice。
- 实例流：每实例 16 个 float（列主序 mat4）；`visible_instances` 是 draw 参数，不重传；`VN_INSTANCED` 决定 binding 4 与顶点布局。
- revision：**形状归几何**（`Geometry::setRevision` / `bumpRevision`），**字节归 buffer**（`Buffer::setRevision`）。上传缓存只认 buffer revision；几何 revision 只驱动事实行重建。
- 身份：`vn::graphics::Handle`（gen24/domain8/index32）+ `ContentRef{id, revision}`，公开头是 `sdk/vine/graphics/ContentId.hpp`。`Node::id()` / `MaterialBase::id()` 在构造时分配、析构时释放；`Row::identity`、`RenderWorld::RowId`、delta、`RenderCommand::node`、`CompiledCommand::geometry`、`GeometryFacts::geometry`、材质表都按它键。**表按 `(id, revision)` 建行**，旧 revision 的行在停靠窗口内继续作答。地址不再是身份（`ADR-2026-11-11-content-identity-is-a-handle`）；`Buffer`/流上传仍是地址 + `ADR-2026-11-09` 的“持有即身份”（内容对象、计划、编译半边都已改 `ContentRef`）。
- 拾取树：`Geometry::prepareBvh(draw, builder)` 默认 `pickTreeBuilder()`（`kPickTreeLeafSize = 64`），建出的树**已封存**（`Bvh::seal()`）——一把自叶容量 4 的 14M 三角形树从 1.36 GB 降到 88 MB。封存树的查询回调第二参数是叶盒，拾取本来就不用它。
- 收集：`Scene::collectRenderCommands` 对每个 primitive 发一条命令，快照 world / state / material / program / instance；按 `drawOrder` 后不透明前向、透明后向排序。
- 包围盒：`Geometry::localBounds` 缓存键 = positions buffer + 其 revision + 段 + 几何 revision；`subtreeBounds` 在节点自身坐标系；`Scene::boundingBox` 只算可见且命中层掩码的内容。
- 盒/线/面的类型在 `vn::geometry`：`Aabb<T>`（`Aabbf/Aabbd/Aabbi`）、`Obb<T>`、`Line<T>`、`Plane<T>`；`math` 只有数值与变换，没有 `Rect3`，也没有 `math::Aabb*`。见 `.ai/design/geometry-primitives.md`。
- OBB 拟合：`Obb::computePca` / `computeFixedOrientation`；特征分解是 `math::symmetricEigen`（升序谱、只读上三角），矩阵→四元数是 `math::toQuaternion`。最小体积 OBB 与 `merge` 没有做。

## 3. 目标、层、颜色、图源

- `RenderTarget` 是**描述**，后端按描述造图；`RenderTarget::setSamples` 是 MSAA 唯一入口，采样数进 `TargetShape` ⇒ 管线键 ⇒ 通道/图像。颜色与深度各解析一张单采样图（深度 `sample0`，不平均）。
- 层：`Node::setLayers` + `PassDesc::layer_mask`；最近声明获胜；`Node::subtreeLayerUnion` 是 layer epoch 驱动的懒缓存，三条 walk 在可见性之后先判 `(union & mask) == 0`。
- 颜色：`Colorf` 是线性光，`Color` 是 sRGB 编码字节；`toLinear()` / `toColor()` 一对转换；清屏与 pass 颜色用线性值。
- 图源：`ImageInput` / `ImageSource`；produced（`ImageRef`：target + attachment）与 imported（`Texture`）两种生命周期不合并。`MaterialImages` 上传键 `(Texture*, revision)`，sampler 按值缓存。

## 4. vsg 平台事实（硬约束）

- Vulkan 1.4 地板；EDS2 / EDS3 + 3 个 fn-pointer 入口；**没有 dynamic rendering** ⇒ render pass + load-op variants。
- vsg 按 viewID 编管线 ⇒ 每 pass 的状态注册表必须私有。
- push constant 128 B；reverse-Z（far = 0）；世界 CCW vs framebuffer CW ⇒ `frontFace = CLOCKWISE`。
- X11 / Win32；**无 Wayland**。
- 在飞槽数是 vsg `Viewer` 的硬编码 3：启动期逐帧学习（`probeSlotCount` / `SlotTracker`），学会前不 park。
- 退役窗口 = `slots + 1`；`RetirementQueue::setSlots()` 原地更新，不要重新赋值（会清零计数）。

## 5. 坑（不可再踩）

- `vn::String` 包的是 `std::u8string`；字面量写 `u8"..."`，printf/gtest 用 `reinterpret_cast<const char*>(s.data()), s.size()`。
- `PassDesc::camera` 是**拥有式** `intrusive_ptr<Camera>`；传栈相机地址会 double-free。
- `vsg::Viewer` 没有 `device` 成员；停设备用 `viewer->deviceWaitIdle()`。
- `vsg::Instance::create` **抛异常**（不返回 null）；版本回落循环要 catch。
- `vsg::RenderGraph` 默认 `renderArea` 是零面积；必须显式设置，否则 pass 记录了但什么都不清。
- `vsg::DescriptorBuffer::create` 参数顺序是 `(bufferInfoList, DESTINATION BINDING, array element, type)`。
- 动态状态块槽位必须是 `detail::kDynamicStateSlot`（15），否则会替换 pipeline bind。
- `VkSubpassDependency` 的 stage/access mask 是 render-pass 兼容性的一部分；借深度时 dependency 必须按形状取超集。
- 设备扩展必须真正 enable（EDS3 的 polygon mode / color blend）；只开 feature bit 不够，validation layer 才看得见。
- Vulkan NDC z 就是 depth（没有 OpenGL 的 `z*0.5+0.5`）；reverse-Z far = 0.0，清深度不能清 1.0。
- 动态 `vkCmdSetPrimitiveTopology` 只能在 pipeline 的 topology class 内；`Topology` 进 `PipelineKey`。
- `POINT_LIST` 要求顶点阶段静态写 `gl_PointSize`；内置程序写 1.0。
- 顶点/索引上传是**别名，不是复制**：`StreamUploads` 用 `VsgBufferView` + 真 `vsg::floatArray` / `vsg::uintArray` 读模型字节
  （顶点通道别名它的段、索引别名**整段 buffer**——跨度由 draw 说）。模型先死也没事：数组→视图→buffer 是一条持有链。
  把 `vsg::Data` 子类**直接**当数组绑上去**不会生效**（验证层干净但画不出），必须先套成真数组。
  没有 buffer 句柄或 span 不属于该 buffer 时才退回复制（拷贝路径仍要保留，测试就是这么调用的）。
- vsg 上传后**进程里会多一份与数据同尺寸的 host 显存分配**（14M 三角形的模型：模型 1.16 GB 之外还有 1.23 GB
  `SharedUsage`，另有同尺寸 `DedicatedUsage`）。**归属已定**：`TransferTask::transferData()` 里
  `staging = vsg::createBufferAndMemory(device, totalSize, TRANSFER_SRC, EXCLUSIVE, HOST_VISIBLE|HOST_COHERENT)`
  —— 整份传输一个 host-visible 缓冲，存在 `TransferBlock::staging` 里，`staging->size < totalSize` 时只增不减、
  传完不还，**不经过** `stagingMemoryBufferPools`，所以 `releaseUnusedPools()` 碰不到它（应用实测数值一字不变）。
  它随会话（viewport/文档）销毁而释放；vsg 没有任何 getenv/配置能把这条路关掉（源码里只有 FileSystem 读 getenv）。
  **已解决（F16.2，2026-11-10 改定）**：**暂存不保留**。`detail::StagingTask`（派生自 `vsg::TransferTask`，会话起来时由
  `installStagingTask()` 装好）在**每个帧边界**（`VsgBackend::executeFrame()` 开头）把 `fence->status() == VK_SUCCESS`
  的块的 `staging` + `buffer_data` 置空 —— 正是 vsg 自己“不够大就重建”的起点，下一次上传到那块时重新分配。
  证据是那块自己的 fence，所以**零等待、零 park、零 `deviceWaitIdle`**；没完成的拷贝留给下一帧。
  **不要加额外条件**：曾经在 sweep 里先问 `to_copy->containsDataToTransfer()`（“还有待拷数据就跳过”），
  而 vsg 的 `_transferData()` 在 `requiresCopy()` 为假时提前返回、**不清理** `dataMap` → 那个条件可以永久为真，sweep 永久跳过，
  1.2 GB 又常驻回去（用例抓到的就是这个偶发）。
  **每块独立**：`TransferTask` 有 `2 × numBuffers` 个块，上传按 `frameIndex` 轮转，所以两块可能各压一个模型
  （实测 704 MB 车身 → `SharedUsage` 1,228 MB = `2 × 614 MB`）。
  **可观测**：`VsgBackend::uploadStagingBytes()` / `api::Session::stagedBytes()` / `api::OffscreenSession::stagedBytes()`；
  稳态恒 0，`RetentionStats::device_waits` 在这条线上永远不涨。
  **代价**：每帧都上传的宿主会每帧重新分配一块（最小 16 MB）—— 当前仓库无此宿主，触发条件在 backlog。
  **换任务**（`installStagingTask`）现在只用于“安装可清块的任务”，四个交接仍缺一不可（见 P42.1）。
  **判读方法**：`Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory` 的实例名是 `pid_<id>_luid_...`
  （按 `*Vine*` 过滤会得到 0 行）；`DedicatedUsage` = 显存（计入 private、**不计入 ws**）、`SharedUsage` = 系统内存
  （计入 ws）。要降它只能减少上传字节数（减面 / 换带索引格式）或让暂存被回收。
- Shader 命名：文件保持 `builtin_*` / `vine_*` 前缀；函数是 `vnXxx`，变量、块实例、stage 输出、采样器一律 `vn_` 前缀。
- 采样器名**就是** ABI 键（`ContentImages::imageOriginOf` / `MaterialSlotCheck`）：改 shader 里的名字必须同步改表，
  改一边不报错、只把 drawable 的图换成 pass 输入（`vn_shadow_map`、材质五槽、`vn_skyMap`）。
- 透明度：引擎唯一透明通道是 `Node::opacity`；材质不贡献 alpha；blend 恒开，`enabled` 只选因子。
- 设备读回像素是 BGR 字节序；单通道断言用 `isRed()` 等，不要读 `pixel[0]`。
- 改公开签名或改名后必须全量构建；旧插件 `.so` 会 `undefined symbol`。
- **隐藏 / 被裁剪的上传不丢，离树才丢**：寿命是两个 O(1) 事实——内容层发布的声明索引
  （`ContentFacts::declarations`，由 `ContentStore` 的几何行变更同步维护的地址→声明计数）说“还有没有活行
  声明这块 buffer”；条目自己的句柄说“revision 还是不是它上传时的那个”。所以裁剪 100 帧也不重传，
  buffer `edit()` 换代也自己离场；对象离树 ⇒ 行在 `slots + 1` 帧后删掉 ⇒ 声明消失 ⇒ 上传随之离场。
  每帧只遍历驻留条目，不扫内容行（扫行会让“什么都没画”的帧最贵）。见 `.ai/design/graphics-content-lifecycle.md` §2.5 / §2.9。
- **非 Debug 配置的两个构建坑（2026-11-10 实测）**：
  - vcpkg 的 glslang 只导出 `DEBUG` / `RELEASE` 两个配置，RelWithDebInfo / MinSizeRel 没有精确匹配，CMake 会回落到**第一个**（DEBUG）
    ⇒ 插件把 `/MDd` 的 glslang 目标链进 `/MD` 的 DLL：`LNK2038`（`_ITERATOR_DEBUG_LEVEL`、`RuntimeLibrary`）+ 未解析的 `__imp__CrtDbgReport`。
    `gfx_backend_vsg/CMakeLists.txt` 对 `glslang::glslang` / `::SPIRV` / `::glslang-default-resource-limits` 设了
    `MAP_IMPORTED_CONFIG_RELWITHDEBINFO|MINSIZEREL = RELEASE`；以后新增只在两个配置出库的 vcpkg 包，消费方要同样映射。
  - `tests/test_graphics/GraphicsTest.cpp` 约 650 KB，优化配置下段数超 65535（`C1128`）⇒ `tests/CMakeLists.txt` 的 MSVC 选项统一带 `/bigobj`。

- **材质改动的三条落地点（2026-11-11 实测，别再踩）**：材质是"以值过帧"的最后一个内容事实（C19）。改
  `Material::setXxx` 之后值要落地，靠的是**两处取值点**：`SceneRows::publish`（发布边界，镜像/引擎侧看到的
  版本）和 `Scene::collectRenderCommands` 开头的 `detail::refreshMaterials`（版本动了才清 memo、抬
  `Version::materials`）。**只改一处就有画面滞后**：只改 publish ⇒ 直连/镜像采集命中旧 memo；只改 collect ⇒
  镜像永不重发布。批内（`edit_depth_ != 0`）两处都不取，这才是 C18 对材质生效的方式。
  另：`RenderCommand::materialDescription()` 对手搓命令退回 `material->description()`（与
  `geometryDescription()` 同一形态），所以手搓命令仍"下一帧生效"，但那条路是宿主的（别在录制中改它）。

## 6. 证据与门禁

- `test_graphics`（模型无关）、`test_vsg`（后端，含真实设备用例）、`test_gui`、`test_core`。
- `scripts/vsg_rewrite_gate.sh`：suite 两跑 0 VUID / 0 SYNC-HAZARD、相位行闭合、应用画面百分比。
- 静态门禁：`check_ai_docs.py`、`check_doc_symbols.py`、`check_include_hygiene.py`、`check_export_annotations.py`、`check_diagnostic_formats.py`、`check_public_docs.py`、
  `check_api_style.py`（API 风格：值里不许有 setter/回调、禁 `getXxx` 访问器、`bool`+出参不并存、`XxxBuilder` 必须有 `build()`；
  4 条既有发现登记在脚本的 `BASELINE` 里，修完即删）。
- `scripts/asan_check.sh` / `VN_ASAN_*`；`skipped == 0` 才算跑过设备电池。
- 量后端每帧成本：`test_vsg --gtest_filter='*MeasureWhatEachKind*'`（打印 µs/帧 + `builds`/`uploads`/`created`，需真实窗口，headless 会 skip）；成本模型与分配上限见 `.ai/design/vsg-backend.md` §2.10 / N4 / T5。

## 2026-11-11 —— graphics-next 提案组（未采纳；下次从这里继续）

- 六份文档，入口 `.ai/research/graphics-2026-11-11-next.md`（总纲），展开：`-requirements`（`R1–R30`、
  `N1–N18`、`C1–C12`、`T1–T16`）、`-design`（类型草案 + hot/cold 分块 + 七个算法 + 7 条 open question）、
  `-benefits`（收益/代价 `P1–P7`/三批迁移）、`-api`（`P1–P32` + `M1–M10` + 风格三档 + 49 方法去向）、
  `-hot-paths`（性能预算、动画 pose、后期依赖图、扩展、`N11–N18`）。对应 backlog `T32`；**现行设计不变**。
- 讨论中定下、且值得下次直接沿用的三条判断：
  1. **三档风格**：跨线程/进计划/被比较 ⇒ 值（无 setter，`withXxx`）；有身份/单线程/增量编辑 ⇒ 对象（Qt 访问器）；
     只在创建时存在 ⇒ 构建器（`build()` 产出值）。`RenderPass` / `PassChain` / `ShaderProgram` / `RenderControl`
     今天**越档**（分别是值语义、构建器、值、适配器）。
  2. **动画不走发布**：`Instance::setMatrix` 是编辑（hot 块 CoW + 发布）；每帧动画走帧内 `PoseSet`
     （快照不动、每行一次矩阵乘）。这样"半个配置上屏"在动画路径上不会出现，也顺带消掉 backlog `T6`。
  3. **hot/cold 分块**是"折叠留在快照"与"发布 O(变更)"能同时成立的前提：动画只复制 hot 块（~10 KB/块），
     cold 块（身份/状态/引用）共享零拷贝。
- 新门禁：`scripts/check_api_style.py`（四条规则，今天 0 新增 / 4 基线）。
- 本 session 唯一的**生产代码**改动：`af2de738` 把 `sessionChangeAllowed` 收回 private（原为我误放 public）。
- 下次的第一条命令：`-hot-paths` §8 第 1 步 —— 把 `Measure*` 用例从打印改成阈值断言（不改架构、让性能回归可见）。

## 2026-11-11 —— graphics-next 第二轮审计（新增 `graphics-2026-11-11-next-audit.md`；动作顺序被改写）

- 新增 `.ai/research/graphics-2026-11-11-next-audit.md`：`X1–X10` 跨文档矛盾、`A1–A13` 设计缺口、
  `R31–R39` / `T17–T31` / `K7–K11` 补充。**提案仍是“未采纳”；现行设计不变。**
- 最关键的 A1：新设计的 `DrawItem` 只带 `id`，丢了 revision；内容一改，在飞旧帧会按 id 查到新值，
  直接违反 `R9`/`I1`。今天的 `RenderCommand` 反而带 `GeometryDescriptionPtr`/`MaterialDescriptionPtr`
  （`ContentStore` 也为旧 revision 保留行到帧走完）。修法：`ContentRef{id,rev}` 或冷块存值指针；
  引擎表按 `(id, generation, revision)`。**先修这条再谈快照原型。**
- 其余必须先修的：A2 拾取身份是 `InstanceId` + `FrameResult` 缺 View/pose；A3 共享原型要“一个写者域 +
  发布时比对原型 revision”；A4 `Instance` 要持有原型存活；A5 原型实例化“10 次矩阵乘”不成立（5000×10 是 5 万次，
  内存按实例倍增）；A6 `DrawItem ≤64 B` 按字段已超且 `sort_key` 少 32 位；A7 发布元数据整体拷贝不是 O(变更)；
  A8 `chain[0]` 块剔除不成立且 `local_box_union` 字段不存在。
- 证据口径：2519 = 提案基线（8 个文件 .hpp+.cpp，仅 cpp 1212；当前原型新增 select/facts 后 2702，K5 仍用 2519 分母）；ContentStore 1016 行、寿命 token 75（六 token）/100（含 parked）；
  RenderEngine 1309 行；test_graphics 548（提案时；当前 559）/ test_vsg 537 / ctest 30 套；2000 drawable 重测约 2074–2128 µs（200 为 204 µs）；
  “47 处 include”当前只有 23 个文件，“126 个 bool”按 scope 是 110/152——数字要带命令，否则会漂。
- 动作顺序（取代上面“下次第一条命令”）：① 修 X/A1–A4 文档 + 补 K7–K11；② `-hot-paths` §8 第 1 步（Measure 阈值）
  + 证据脚本；③ `detail::` 快照 + 纯 select（**不带原型实例化**，内容引用带 revision）；④ 寿命/注册表；⑤ 计划值 +
  `prepare` + 单 Session 多 Surface；⑥ 原型实例化单独实验（先定 A5 的共享 local rows + per-instance transform）。
- 结论一句话：跨层不可变值必须落到引用上——**只带 id 的引用不是不可变值**，只是把版本问题推到引擎表里。

### 落地（同一 session，2026-11-11）

- **步骤 0 已完成**：六份提案里的 `X1–X10` / `A1–A13` 已按 audit 修订——`ContentRef{id,rev}`、
  `DrawItem` 24 B + 64 位 `sort_key`、`world_bound` 块剔除、`PoseEntry` 子树传播、`FrameQuery`/`PickResult`、
  `prepare`、单 Session 多 Surface、`Result<void>` 特化、`C2b` 写者域、`R31–R39`/`T17–T31`/`K7–K11`。
  提案仍是“未采纳”；`.ai/design` 未动。
- **步骤 1 已完成**：`SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera` 现在有阈值
  `drawables*3+1000` µs（sanitizer 只打印）；实测 200→204 µs / 2000→2128 µs，均通过。新增
  `scripts/measure_graphics_claims.py`：桥 2519（提案时；当前 2700）、ContentStore 1016/75/100、
  RenderEngine 1309/50 public/22 include、SDK 75/99、bool 110/152、tests 548/537/30（提案时；当前 559/537/30）。
- 下次从步骤 2 开始：`detail::Snapshot` + 纯 `select`，**内容引用带 revision、暂不做原型实例化**；
  先跑 `K1–K3` + `K7`。不要跳过 `ContentRef`——那是本轮审计的核心修正。

### 步骤 2：chunked snapshot + 增量发布（2026-11-11，Debug 结果）

- `SceneSnapshot` 现在是 32 行 chunk 的不可变快照：`makeSceneSnapshot` 复用上一版未变 chunk；
  `SceneRows::snapshot` 的缓存挂在内部 `SceneBake` 上（整表重建时把上一版缓存搬过去），`Scene` 的公开布局不变。
- `Scene::ensureRows` 增加增量路径：按每行 `Node::contentEpoch` 找编辑行；结构/可见性/父子顺序
  全检；只允许 local / opacity / layers / draw order / view placement 这些热字段原地刷新，
  其余（state / geometry / material / primitive / box）回退整表 `buildRows`。
- `SnapshotTest` 五条通过：纯 select 等价、旧 revision、transform 增量、2000 行计时、常驻内存；`test_graphics` 559 条通过（另加 `ContentRegistryTest` / `ResidencyTableTest` 6 条）。
- **2000 行改 1 行**：Debug publish ≈ 2.2 ms / select ≈ 1.3 ms；**RelWithDebInfo publish ≈ 154–203 µs /
  select ≈ 293–307 µs**，K1（≤500 µs）与 K3（≤×1.2）都通过；shared 62/63 chunks。
- **K2/N3 通过**：`MeasureWhatASnapshotKeepsResident` 测得 snapshot 常驻约 **1.03× 行表**（K2 ≤3×，N3 ≤2×）。
- **N2 已调整为 ≤250 µs**（2026-11-11）：50 µs 需要内容侧 dirty notification，与“Node 无 Scene 反向指针”
  冲突；203 µs 在 250 µs 内，RelWithDebInfo 的计时测试已加 N2 断言。下一步进步骤 3
  （`ContentRegistry` + `ContentRef{id,rev}` 的引擎表）。不要拿 Debug 的 2.2 ms 当最终数字。

### 步骤 3a（身份/驻留原型；2026-11-11）

- 新增内部 `ContentRegistry.{hpp,cpp}`：`Handle`（gen24|domain8|index32）、typed `Id<Tag>`、
  `allocate<Tag>()`、generation/domain 校验、24 位回绕退休槽；`ContentRef{id,rev}`；
  `ResidencyTable`（`last_epoch < oldest_live` 的 collectable/reclaim，revision 是 key 的一部分）。
- 新增 `ContentRegistryTest` / `ResidencyTableTest` 6 条（stale handle、跨 domain、重复 release、
  collectable 可枚举、revision 键、touch 延长寿命）；`test_graphics` 559 条通过。
- **3b 已按设计拍板走 B（公开身份 = `Handle`），第一段落地**（2026-11-11）：新增公开
  `sdk/vine/graphics/ContentId.hpp`（`Handle`/`Id<Tag>`/`ContentRef`/`ContentRegistry::instance()` + `std::hash`），
  `Node` 持 `NodeId`（ctor 分配 / dtor 释放，`Node::id()`）。行身份全面改 `NodeId`：`Row::identity`、
  `RenderWorld::RowId`、`Delta`、镜像 delta、`SceneSnapshot::identities`；公开 `RenderCommand::row`
  （`const void*`）→ `RenderCommand::node`（`NodeId`），`Collected/CompiledCommand`、`GeometryFacts`、
  `ContentStore` 几何表随之按 `NodeId` 键。手写命令无 node 时用 `geometry->id()`。
  证据：`test_graphics` 559 / `test_vsg` 537 / ctest 30/30。改动后**清旧对象再编**（`Node`/`RenderCommand` 布局变）。
  仍未做：①`Geometry`/`MaterialBase`/`ShaderProgram` 自己的 id，让 `ContentRef` 成为唯一键（几何表现在是
  `(NodeId, rev)`、材质表仍按地址单键 = A1 在后端的残余）；②计划行合并成 `ContentRef geometry`；
  ③`markUsed`/parking/`kept` 换 `ResidencyTable` epoch 回收。三步之间不要半迁移 `ContentStore`。

### 步骤 4（材质身份；2026-11-11）

- `MaterialBase::id()`（ctor 分配 / dtor 释放）。`MaterialFacts::material` 改成 `ContentRef`（revision 进键，
  独立 `revision` 字段删掉）；`CompiledCommand`/`CollectedCommand::material` 也是 `ContentRef`。
- `ContentStore::ensureMaterial` 现在**每 revision 追加一行**、旧行停靠后删（不再就地替换 + `RetiredMaterial`/`kept`）；
  `materialEntries()` 是“每 revision 一行”。`MaterialArena`/`BlockStorage`/`ContentSweep`/`released_materials` 按 `MaterialId`。
- `FrameRecorder::snapshotCommands` 的材质 revision 规则：已发布的值取值 revision，否则取对象当前 revision ——
  必须与 `ensureMaterial` 取值规则一致，否则手写命令与表永远错过（实测踩过）。
- **`ContentRef::revision` 是 `std::uint64_t`**（原设计稿写 uint32）：SDK 每个 `revision()` 都是 64 位。
- 测试 helper：`tests/test_vsg/TestMaterialFacts.hpp`（`materialId()`/`materialRef()`/对象→`ContentRef`）。
- 下一步：`ShaderProgram::id()` + `ProgramRef` → `ContentRef`（程序表也按 `(id,rev,variant,kind)`）；
  计划行 `geometry`+`geometry_revision` 合并为 `ContentRef geometry`；然后 epoch 回收。
- 这一步只改了内部 `SceneBake` / `SceneSnapshot` / `Scene::ensureRows` / `DrawListBake`；公开 API 未变。
  `Scene` 的缓存/epochs 全放在内部 `SceneBake`，避免改公开类布局（曾因临时加字段导致 test_vsg ABI 崩溃，已改回）。

### 步骤 5（程序身份；2026-11-12）

- `ShaderProgram::id()`（ctor 分配 / dtor 释放）。`ProgramRef` 类型删除：计划/记录（`CollectedCommand`、
  `CollectedDraw`、`CollectedFrame::default_program`、`CompiledCommand`、`CompiledDraw`、`CompiledFrame`）、
  `PipelineKey::program`、`Scope::Entry::program`、`ContentHalves::Key::program`、`ContentSets::Key::program`
  全部是 `ContentRef`。
- `ProgramFacts::program` 是 `ContentRef`（独立的 `revision` 与 `generation` 两个字段删除）；程序表按
  `(ProgramId, revision, variant, kind)` 建行。`ContentStore` 的 live 表按 `ProgramId` 键；`ensureProgram`
  从被 track 的对象取 revision（计划里的 revision 不参与建行，规则仍是“建行描述对象现在是什么”）。
- 诊断：`ContentFacts::program_names`（每 identity 一条 `ProgramNameFacts`）+ `programName()`。原因：计划只带
  句柄，而“文本被拒绝”的程序没有 `ProgramFacts` 行，`describeMissingContentHalf` 仍要能说出是哪个程序。
- 测试：`test_graphics` 559 / `test_vsg` 538 全绿；`ContentHalvesTest.AnotherProgramIdentityIsAnotherHalf`
  钉住“同文本、同 revision 的两个程序对象不共半边”（原来靠 `generation`，现在靠句柄）。
- 下一步：计划行 `geometry` + `geometry_revision` 合并为 `ContentRef geometry`；然后 `markUsed`/parking 换
  `ResidencyTable` epoch 回收。`ProgramFacts::generation` 删除后，`ADR-2026-11-09` 的“持有即身份”只剩上传表
  （`Buffer`）一条。

### 几何行合并为 `ContentRef`（2026-11-12 已落地）

- 计划行：`CollectedCommand` / `CompiledCommand` 的 `geometry`（`NodeId`）+ `geometry_revision` 合成一个
  `vn::graphics::ContentRef geometry`。产生点仍只有 `FrameRecorder::snapshotCommands`（identity 取
  `RenderCommand::node`，无 node 时退回 `geometry->id()`；revision 取 `description->revision()`），
  `FrameCompiler::resolveCommands` 只复制。
- 事实与表：`GeometryFacts` 的 `geometry` + `revision` 合并成同一个 ref；`GeometryKey` / `compareGeometry` /
  `orderGeometryRows` 与 `findGeometry(facts, ContentRef)` 按一个键；`ContentStore` 的 `eraseGeometryRows`、
  `liveGeometry`、`ensureGeometry` 与几何表都按一个 ref。`ensureGeometry` 仍从 `live.shape->revision()` 建行
  （计划里的 revision 不参与建行，规则与程序/材质相同）。
- **类型**：几何的身份就是 `Geometry` 继承自 `Node` 的 `NodeId`（`Geometry::id()` = `Node::id()`），
  `ContentRef::revision` 才是几何 revision。**不要为此造 `GeometryId`**（`ContentId.hpp` 有这个 tag，但今天
  没有分配者）；要 typed 视图用 `ref.as<vn::graphics::NodeTag>()`。
- 消费点只改了参数：`ContentHalves::halvesFor`、`ContentPass`（两处 `findGeometry`）、`ContentSets::setsFor`。
  `ContentHalves::Key` / `ContentSets::Key` 里没有几何身份（只有 layout / set / source），键没动；
  `RenderWorld` / `SceneSnapshot` / `Scene` 的行身份也没动（它们本来就是 `NodeId` 键）。
- 测试 helper（`TestGeometryFacts.hpp`、`ContentStoreTest` / `ContentSweepTest` / `ContentHalvesTest` 的手搓
  计划）保留“对象 + revision”入口，但内部只造一个 ref —— 计划行的 API 只有一个 ref。
- 验证：`test_graphics` / `test_vsg` / ctest 30/30 全绿。
- 下一步（已落地，见下节）：`ResidencyTable` 的 epoch 回收替换 `ContentStore` 的 `markUsed`/parking/`kept`。

### 行寿命改为 epoch 回收（2026-11-12 已落地）

- `ContentStore` 的 `markUsed` / `release_token` / `release_requested` / `retained` / park 全部删除。每条
  几何 / 程序 / 材质行带 `last_epoch`（最后一次被计划命中的帧，`tablesFor` 逐条 stamp）；
  `Data::reclaim()` 在每帧 `tablesFor` 开头跑一次：`last_epoch < completedFrame() + 1` **且不再是条目的
  当前答案**（`isCurrent`）的行离开。`RetirementQueue` 仍然只给 GPU 对象用（上传、半片、描述符集）。
- **“当前答案”那一半不能省**：裁剪 / 隐藏的内容本帧不被计划命中，但它仍是活条目的当前答案，所以行（和它
  声明的上传）留着；否则 `ACulledGeometryKeepsItsUploadsForAHundredFrames` 与 design F27/F28 都会破。
  已释放条目与 superseded revision 不再是当前答案，按 epoch 走（无时间窗）。
- 条目（`Live*`）多活一步：`released` 标记（`release*` / `releaseAbandoned`）+ 无行 ⇒ `dropReleased()`
  丢条目；“没有行”由表的 per-identity 行计数回答（`Table::rowsOf`）。材质 arena 槽在条目丢时进
  `material_slots_pending`，由下一次 `releaseAbandoned` 交给 `BlockStorage::releaseMaterial`（比旧 park 晚一帧）。
- `Table<Row, Storage, MakeOrder, KeyOf>`：`append(row, storage, epoch)` / `stamp(row, epoch)` /
  `reclaimBefore(oldest_live, keep, onErase)` / `rowsOf(key)`；`epochs_` 与 `rows_`/`storage_` 同步移动。
- API：`tablesFor(frame, timeline)`、`releaseGeometry/Program/Material` 不再收 timeline/retirement，
  `releaseAbandoned(released_materials)`，`releaseAbandonedContent(store, images, released_materials)`；
  `ContentStore::retained()` 删除。
- 测试：`Frames` helper（`ContentStoreTest`）驱动 begin/submit/`completeUpTo(frame - slots)`；旧 park 用例改名
  为 “...OnceNoPlanNamesThem”，`AReleaseThatCannotBeParked...` 删除，新增
  `AReleaseWithoutCompletionEvidenceKeepsTheRows` 与 `AnAbandonedEntryKeepsItsObjectUntilItsRowsLeave`。
- 证据：`measure_graphics_claims.py` 的寿命 token 75 → **1**；`test_vsg` 537 / `test_graphics` 559 / ctest 30/30。
- 陷阱：`tablesFor` 返回 store 的 `d->facts` 引用，多次 record 会改写它；断言要在每次 record 后立刻读。

## 2026-11-12 —— `releaseContent` 入口补齐（MaterialBase + 一条流）

- **`Material` → `MaterialBase`**：`RenderCommand::material` 是 `MaterialBasePtr`，`MaterialInstance : MaterialBase`
  且**不是** `Material`，旧的具体类 `obj_cast` 让每个 instance 落到分派末尾、返回 false（行与 arena 槽永不释放）。
  `VsgBackendTest.ReleasingAMaterialInstanceDropsItsResidency` 反向验证：改回 `Material` 即红。
- **buffer 分派**：`Buffer<T>` 不是 `Object`（design 2.6 故意把它留在内容对象体系之外），所以
  `releaseContent(raw_ptr<const Object>)` 根本收不到它——补的是**两个引用参数重载**
  （`const vn::Buffer<float>&` / `const vn::Buffer<std::uint32_t>&`），`RenderDevice` → `RenderEngine` →
  `VsgBackend` 各一层。用**引用**而不是指针：`releaseContent(nullptr)` 仍解析到 Object 那条（三个指针重载会让
  `HeadlessBackendTest` 里那句 null 断言直接 ambiguous），而且“不存在的 buffer 没有驻留可放”本来就不是一种入参。
- 实现：`backend/ResourceTable::retireIf(drop, timeline, retirement)`（`retireUnpinned` 现在只是它的取反）+
  `StreamUploads::releaseBuffer(address, timeline, retirement)`（两张表各按 `key.buffer` 找，停靠不销毁；
  停车窗口没学到就保留并计入 retained）。`VsgBackend::releaseStreamBuffer` 用**设备**的 clock/queue，不是某个窗口的。
- 语义：只放这一条流的 GPU 副本，geometry 的行与表不动；下一帧画到它重传。想放一个 geometry 的全部流 → 放 geometry。
- 证据：`VsgBackendTest.ReleasingAStreamBufferDropsTheUploadButNotTheGeometry`（`uploads().live()` 2 → 1 →
  2、`geometryEntries()` 保持 1、`uploads()` 回涨）；`test_vsg` 539 全绿，`test_graphics` / 各项门禁 0。

## 2026-11-12 —— ADR-2026-11-10 步骤 3：纹理迁入 pin 模型

- **声明索引有两个了**：`ContentDeclarations` 同时借 buffers / textures 两张表（`declaresBuffer` /
  `declaresTexture`）；`ContentStore` 里两者共用同一个 `Declarations` 类（地址 → 引用计数，append 加、erase 减、
  归零删），`GeometryTable` 与新的 `MaterialTable` 各持一个。材质行在 append / reclaim 时声明 / 撤回
  `slots[].texture`。
- **`MaterialImages::beginFrame(declarations, imported)`**：声明 = 材质行 slot **或该帧 pass 的导入输入**
  （`CompiledPass::inputs` 里 `ImportedTexture` 的地址；`ContentAssembly::beginFrame` 收集到一个复用 vector），
  另一半是条目自己的 `owner->revision()`。**不 park**：条目只持有查找份额，描述符集自己持有 view / sampler。
  `releaseAbandoned()` 删除，新增累计计数 `released()`；`VsgBackend::releasedTextures()` 读它。
- **`ContentSweep.{hpp,cpp}` 与 `ContentSweepTest.cpp` 删除**：只剩 store 一半的包装，名字与行为不符。 <!-- drift-ok -->
  `VsgBackend::sweepAbandonedContent()` 直接调 `store->releaseAbandoned(...)`，文档搬进那里的注释；
  默认材质的断言搬成 `ContentStoreTest.TheDefaultMaterialIsNeverAbandoned`。
- **陷阱（真实缺陷，用例发现）**：`passes` 为空的帧走 `commitFrameThrough` 的早退分支，原来不跑
  `recordContent` ⇒ 图像扫描不跑 ⇒ “宿主不再画任何东西”时贴图永不释放。早退分支现在也调
  `assembly->beginFrame(...)`。
- **陷阱**：`std::span<const Texture*>` 不能从 `const std::vector<const Texture*>` 构造（libstdc++ 的 range
  约束），局部 vector 必须可写。
- **语义变化（ADR 的代价，T27）**：贴图只由活内容钉住——宿主手里但没有材质/导入引用的贴图会在下一帧离场。
  `VsgBackendTest.AWorkingSetLargerThanTheOldTextureBoundStaysResident` 的前提正是旧的持有量规则，删除；
  “无容量上限”由 `MaterialImagesTest.EveryLiveTextureKeepsItsImages`（300 条、不跑帧）继续钉住。
- 变异验证：把 `declared` 改成恒真 ⇒ 3 个新用例全红。`test_vsg` / 全 ctest 30/30，门禁 0。

## 2026-11-12 —— ADR-2026-11-10 步骤 4：材质块迁入 pin 模型

- **pin = 材质身份**（不是 `(身份, revision)`）：arena 一个身份一个槽，而“还活着”这一半就是材质表的
  per-identity 行计数——所以 `ContentDeclarations` 直接借用 `Table::rows_of_`
  （`declaresMaterial(id)` = `find(bits)`），**没有第二份记账**。行还答得出（包括被取代的 revision）就钉着槽。
- **builder / destroyer**：写还是 `MaterialArena::note`（记录时照旧 memo：稳态 0 写、一次编辑一块、同帧第二次
  `Unchanged`）；放槽变成 `MaterialArena::retireUnpinned(is_live)`，由 `BlockStorage::retireUnpinnedMaterials(
  declarations)` 在 `ContentAssembly::beginFrame`（表出版之后）每帧问一次：O(槽数)，不随内容走。
- **不 park，立即放**：arena 的副本轮转就是那个窗口——“放掉可以复用槽的权利”，不是在飞帧读的字节。
  停靠反而有洞：窗口内内容回来重建了条目，停靠到期仍会把槽还掉，条目里的偏移随后会被别的材质写脏。
- **`ContentStore` 三个释放循环 → 一个 `retireOwner`**：标记（`Live::isAbandoned()`：geometry 问 object 或
  shape，program/material 问 object）与丢弃（`released && rowsOf == 0`）同一遍完成；`dropReleased()` 只是
  `retire(false)`。`material_slots_pending`、`releaseAbandoned(released_materials)` 的出口、
  `BlockStorage::releaseMaterial`（生产侧最后一个调用点）、`VsgBackend::abandoned_materials` 全部删除。
- **头文件坑**：`BlockStorage.hpp` 不能 include `ContentFacts.hpp`——`ContentFacts.hpp` 又 include
  `ContentPipeline.hpp`，后者要求 `BlockDescriptors` 先被包含，而 `BlockDescriptors.hpp` 正是 include
  本文件 ⇒ 环。用前置声明 `class ContentDeclarations;`（只收引用），实现放 `.cpp`。
- **计数器**：`ResidencyKind::MaterialBlocks` 的 `retired` 从“缺席”变成 arena 的 `retired()`（a3 以前那行
  “no release path - eviction only” 的残余现在实测为 0）；`BlockStorage::materialResidency()` 一并填上。
- 用例：`CoreMaterialArenaTest.ThePinSweepReturnsTheSlotsNoLiveContentNames`、
  `BlockStorageTest.AMaterialSlotIsReturnedOnceNoLiveRowDeclaresIt`（真设备）、
  `ContentStoreTest.AMaterialDeclaresItsIdentityForAsLongAsARowAnswersIt`；a3 的残差断言改成
  `liveMaterials() <= base_slots`（实测每周期回 0）。`test_vsg` 全绿。

## 2026-11-12 —— graphics-next 第 1 步：计划是值（`Plan` + 按名字差分 + 稳定 code）

- **新公开值**：`Plan`（有序 `PassDesc` 列表，pass 的**名字**是身份）、`PlanProblem { code, pass, detail }`、
  `PlanProblemCode`（`EmptyPassName` / `DuplicatePassName` / `UnknownInput` / `DependencyCycle` /
  `TooManyPasses` / `SessionBusy`）、`kMaxPlanPasses = 64`、纯函数 `validatePlan(plan, external_outputs)`、
  `planCodeText(code)`。头文件 `Plan.hpp`，实现 `src/Plan.cpp`。
- **引擎入口**：`RenderEngine::applyPlan(plan) -> vector<PlanProblem>`（空 = 已应用）与 `RenderEngine::plan()`。
  差分按名字：新名字 `makePassNode` + `insertSlot`；同名同 kind ⇒ `setDesc` **原地**（节点与后端状态存活，
  P7）；同名但 kind 变了 ⇒ 删后重建（保留 owner/role）；计划没命名的 pass ⇒ `removePass`。整份校验通过
  才应用，**全有或全无**。
- **`plan()` 会把有效 order 写进 `desc.order`**，所以 `applyPlan(engine.plan())` 是幂等的（只声明 stage
  的 pass 不会因为读回而被重排）。
- **命名**：`RenderEngine` 原来的私有 `PlanProblem`（句子身份）改名 `CompileProblem`——公开的 `PlanProblem`
  是“形状缺陷 + 稳定 code”，两者不是一回事；`FramePlan` 仍是**编译后**的视图，`Plan` 是声明。
- **坑**：`validatePlan` 必须用**两个** map——pass 名字（差分身份，要唯一）与 `output_name`（消费者解析的
  生产者）。第一版混成一个 map，环与 feedback 两个用例当场红。
- **测试**：`PlanTest`（12 个）——T7（无 add 调用即有 pass 并真的跑）、读回幂等、改名、原地更新、四类 code +
  feedback 不是环、拒绝后形状不变、code 拼写、`describe`。`test_graphics` 571 / `test_vsg` 540（一个
  既有窗口用例首跑红、单跑绿：宿主窗口报 `4294967295x4294967295` 的已知偶发）/ 全 ctest 30/30。
- **文档**：`graphics-frame-pipeline.md` §3.1/§3.6/§8/§9.1/§9.3/§10 更新；`K1–K11` 落进 §8.1 作为仓库内
  权威副本（`K4` 分母改成“该步开始时的用例数”）；§9.3 修正为“`addPass` 家族在第 5 步删”（原表与 `C11`
  自相矛盾）。`-audit` **未删**：它是 `R31–R39` / `K7–K11` 的出处，且被两份 ADR 引为证据。

## 2026-11-12 —— graphics-next 第 2 步：`publish` 返回四态（内部半边）

- **为什么**：`ContentMirror::publish` / `FrameWorker::publish` 返回 `bool`，而同一个 `false` 表示三件不同的事
  ——“没变”“批内被冻住”“通道满、下次重试”。调用方无法据此决定重试还是报告，`T31` 的“批内显式未发布”也无处表达。
- **新值**：`src/viz/graphics/src/PublishOutcome.hpp`（5b-2b 已删）<!-- drift-ok --> —— `PublishStatus{Posted, Unchanged, InsideBatch, Refused}`
  + `PublishOutcome{status, held}`（`held` = 调用之后镜像持有的那一版，首次发布前为空）+ `explicit operator bool`
  （保留旧的“是否发布了”读法，所以既有 `EXPECT_TRUE(publish(...))` 一处未改）+ `posted()`。
- **怎么分辨批内**：`SceneRows::editing(scene)`（friend，读 `edit_depth_ > 0`）。**先问它**再比版本：批内版本是被
  故意冻住的，比较不出来；先问还让一个长批次不再每帧付一次行构建。
- **没做的**：公开 `Scene::publish()`。今天没有公开的快照类型承载“上一份快照”，先公开只能答一个没人能用的
  版本号，还会和镜像的记账形成两个真相。它与“帧内隐式发布消失”一起属于第 5 步（§9.3 第 2 行已改写）。
- **断言**：三处既有用例把 `EXPECT_FALSE/TRUE` 升级为状态断言（`Unchanged` / `InsideBatch` / `Refused` / `Posted`），
  没有新增重复用例。`test_graphics` 571。

## 2026-11-12 —— graphics-next 第 3 步先定缝：预热不能是"跑一帧不提交"

- **查代码否掉的那个显而易见的实现**：`VsgBackend::executeFrame` 的循环
  （`openFrameBooks` → 每 pass → `closeFrameBooks` → 提交）看起来可以"跑完不提交"，但
  `Session::beginFrame()` 里 `viewer->advanceToNextFrame()` **acquire** 一张交换链图像：
  不提交 ⇒ 下一帧没图像可 acquire（饥饿）；提交 ⇒ 已经不是预热。而画到窗口的 pass 只能画进那张图像，
  所以这个做法连它都覆盖不到。**已记入 §7 否决项 D11 / §6.11 P11。**
- **选定的缝**：预热只做**值**这一侧 —— 半片 / 描述符集 / 上传。键由
  `drawNeeds(compiled pass, draw)` 算，`prepare` 用它建、`ContentPass::record` 用它取（一个构造者两个消费者，
  `G1`）。窗口目标的格式由 `WindowTarget::facts()` 回答（const，不 acquire）。
- **入口**：`RenderEngine::prepare()` 先编成 `FrameDescription`，后端拿到的是与 `executeFrame()` **同一个值**
  （一条线、两个动词，不引入新名词）；快照进这个输入是第 5 步。
- **§9.3 第 3 步因此拆成 3a / 3b**：3a = 答案（`T32`：报告的键集 = 这一帧实际构建的键集），
  3b = 预建（`T7`/`T10`：预热后 `builds` 增量 0）。每行一个可回退提交。
- **下一步**：3a —— 在 `ContentPass::record` 里定位键构造点（约 1235 行附近，含 `PipelineKey::variant`
  与 topology 的注释），抽成纯函数，再让 `prepare` 走同一函数。

## 2026-11-12 —— graphics-next 第 3a 步落地：`drawNeeds` 是唯一的键构造者

- **新文件**：`src/plugins/gfx_backend_vsg/include/vine/vsg/internal/DrawNeeds.hpp` + `src/internal/DrawNeeds.cpp`。
  `DrawNeeds` 值 + `Miss{None,Geometry,Material,Program}` + `keyFor(compatibility)`；两个 `drawNeeds` 重载：
  `(pass, CompiledCommand, facts)` = 内容绘制，`(pass, CompiledDraw, inputs)` = 全屏调用（**不需要 facts**：
  它的身份全在计划 + 调用方 offer 里）。`sampledColorCount` / `sampledDepthCount` 从 `ContentPass.cpp` 的
  匿名命名空间搬到头文件（`record` 的 set 构建与 `serveHalf` 也用同一拼写）。
- **两组计数，一条规则**：`set_colors/set_depths` = 这一帧的输入集绑多少（pass 的事实）；`key_colors/key_depths`
  = 管线键写多少（program 不在 set 1 采样 ⇒ 0，`programSamplesInputSet` 留在 .cpp 匿名命名空间）。两对名字
  写在同一处，不会各算各的。
- **删掉的重复**：`ContentHalves::halvesFor` 的 variant/layout/program/Key 构造；`recordCommand` 的
  geometry/material/variant/findProgram/半片四路匹配（→ `halfFor(needs)`）；`recordScreenDraw` 的 source
  搜索与六个月手填的 key 字段；`describeMissingContentHalf` 的第三次 lookup。`recordCommand` 少了两个参数
  （`sampled_color_count/depth_count`），改读 `needs.set_*`。
- **T32（设备无关）**：`ContentHalvesTest.TheReportedNeedsAreWhatTheFrameBuilds` —— 身份由用例**手写**
  （不用 `drawNeeds` 喂期望），再比 (a) 答案、(b) `halvesFor` 的半片、(c) 录制后 pool 的键集（`keyOf(1..N)`，
  id 从 1 单调且不复用）。另在 `ContentPassTest.AMultiLayoutScopeServesEveryHalfItWasBuiltFor` 加了同一个
  键集断言（录制侧）。
- **变异验证 3/3 红**：`keyFor` 丢 topology / 丢 variant、`recordCommand` 里把 `record.key.compatibility`
  再覆盖掉。
- **陷阱**：`ContentPassTest.cpp` / `ContentHalvesTest.cpp` 这类大文件**不要**整体 clang-format（会重排既有
  `Scope::Entry{...}` 与 `using` 顺序）；只格式化新文件与 `ContentPass.*` / `ContentHalves.cpp`。
- `test_vsg` = 541 cases / 78 suites 全绿。下一步 = 3b：`RenderEngine::prepare()` 入口 +
  按答案预建（判据：预热后录制 `builds` 增量 0）。

## 2026-11-12 —— graphics-next 5a：影子快照接缝（`T34`）

- **新文件**：`SnapshotSource`（`SceneSnapshot` → `DrawListSource`，按 `SceneSnapshot::revision` 记忆化）、
  `DrawListDiff`（字段级比较）。
- **开关**：`ContentMirror::setSnapshotShadow` / `FrameWorker::setSnapshotShadow`，默认关闭；影子只比较、不替换。
  `publish` 在**成功 post 之后**用 `SceneRows::snapshot(scene)` 取影子；帧协议保证 content half 完成后 engine half
  才开始，所以不会出现“行新影子旧”。
- **坑**：不要从 `SceneRows::publish` 的拷贝建快照——它清掉 `nodes` / `node_epochs`，`makeSceneSnapshot` 会失去
  chunk 共享；用 live `SceneBake` 的 `snapshot_cache`。
- **比较口径**：`DrawListDiff` 比 row 身份 / geometry + description / material + program / world / opacity / order /
  authored_state / instance slice；顺序即 `sortCollected` 的发出顺序（命令没有存 sort key）。
- **测试**：`SnapshotShadowTest` 600 帧（transform / opacity / material / placement 编辑）+ 批内旧图 + 旧快照
  材质/几何/程序 + 空快照变异；`FrameWorkerTest.TheSnapshotShadow...` 帧级接缝。`test_graphics` 577 / `test_vsg` 542 /
  ctest 30/30 / TSan 0。

## 2026-11-12 —— graphics-next 5b-1：Published 模式绑定快照

- **翻源**：`FrameWorker::runPass(Published)` 现在把每个 draw 绑到 `ContentMirror::snapshotSource()`；旧
  `RenderWorldSource` 只经 `legacySource()` 作为影子，`setLegacyShadow` 默认关闭。`FrameMode::Mirrored` 改名
  `Published`。
- **发布**：`ContentMirror::publish` 总是取 `SceneRows::snapshot`；只有 LEGACY 开启时才 `SceneRows::publish` + post。
  snapshot 先建、legacy 后 post；legacy 被拒时两者都不前进（旧测试的 Refused 语义）。
- **坑（test 抓到）**：`SnapshotSource::beginFrame` 必须像 `RenderWorldSource`/`Scene` 一样用**上一帧**的 token
  调 `DrawListMemo::beginFrame`，否则每帧都把上一帧的 memo 全删；`PassesSharingSceneAndCameraCollectOnce...` 当场红。
- **测试**：`test_graphics` 577 / `test_vsg` 542 / ctest 30/30 / TSan 0。

## 2026-11-12 —— graphics-next 5b-2a：公开 `Scene::publish()`，`FrameWorker` 直接持有 `SnapshotSource`

- **公开入口**：`ScenePublishStatus` / `ScenePublishResult`（`Posted` / `Unchanged` / `InsideBatch`，不再有
  `Refused`）；`Scene::publish()` 用 `SceneRows::snapshot` + `published_snapshot_revision_` 判 `Unchanged`，批内
  不前进。`ScenePublishResult` 有 `posted()` 与 explicit `operator bool`。
- **源归位**：`FrameWorker` 不再经过 `ContentMirror`；每个内容一个 `{content, unique_ptr<SnapshotSource>}`，
  `publish(Scene&)` 调 `scene.publish()` 后把快照交给源；`drain`/`applied`/LEGACY shadow 全部删除；`run` 改 void。
- **坑**：第二个 `FrameWorker` 对同一 scene 调 `publish` 得到 `Unchanged`（不是 `Posted`），旧测试的
  `ASSERT_TRUE(publish(...))` 要改成接受 `Posted|Unchanged`。
- **测试**：`FrameWorkerTest.PublishAnswersPostedUnchangedAndInsideBatch`；`test_graphics` 577 / `test_vsg` 542 /
  ctest 30/30 / TSan 0。

## 2026-11-12 —— graphics-next 5b-2b：删除旧镜像 / delta / 版本

- **删除**：`ContentMirror` / `DeltaChannel` / `RenderWorld` / `RenderWorldSource` / `PublishOutcome` 全部删；
  `SceneRows` 只剩 `snapshot`（`version` / `editing` / `publish` 一并删）。`DrawListMemo` 保留为 `SnapshotSource`
  的纯 per-view 缓存；`Scene::refreshBoxes/refreshMaterials` 保留（快照输入）。
- **测试**：`ContentMirrorTest` 11 + `RenderWorldTest` 7 + `DeltaChannelTest` 5 = 23 条 LEGACY 用例删除；
  `RenderWorldTest.cpp` 只留 `SnapshotTest` 与 `DrawListBakeTest`；`SnapshotShadowTest.cpp` 改成
  `SnapshotSourceTest`（发布源 vs Scene 直接收集，600 帧 + 批内 + T17 + 空快照变异）。
- **计数**：`test_graphics` 554 / `test_vsg` 542 / ctest 30/30 / TSan 0。5b-2c（`addPass` 家族 /
  `PassRegistry` 运行时拒绝）未做。
- **坑**：`RenderEngine` 的 contentCollectCount/ReuseCount 读 `FrameWorker::memoStores/Hits`；删掉 LEGACY 后
  只剩 `SnapshotSource` 的 memo，`PassesSharingSceneAndCameraCollectOnce...` 仍应绿（`beginFrame` 的上一帧
  grace 是前提）。

## 2026-11-12 —— GPU 驻留 pin 迁移第 5 步：半片 / 描述符集

- **同一张表**：`ContentHalves` 与 `ContentSets` 改走 `core::ResourceTable`；`answerable` 与 `ContentSets`
  的 sweep 删除，换成一个 `retireUnpinned` 谓词。查找从线性扫描变哈希；清扫从 O(entries × geometries)
  变 O(entries)。
- **半片 pin**：`(program identity, revision, variant, kind)`。layout 不参与——问它要扫全部 geometry facts，
  正是本步删掉的成本；代价是 layout / 变体更替会累积几十 KB 的管线，由 backlog `T28` 观察，未命中不加回。
- **描述符集 pin**：program@rev + 键里每个非空 slot 的 `declaresTexture` + 自己的 `texture.revision()`
  比较。空 source（全是 pass 级采样器）只由 program 钉住。
- **被拒的构建**：没有 GPU 对象，不进表；单独 `unordered_set<Key>` 记“别重试”，随 program revision 遗忘。
  所以 `halves()` / `sets()` 的计数与旧语义一致（被拒不算活）。
- **`ResourceTable::forEach`**：`ContentSets::repoint` 需要遍历所有驻留值；visitor 不得 acquire/retire/clear
  （会移动 `entries_`），但可以改 value 的 body。
- **坑**：sets 的 texture pin 必须先 `declaresTexture` 再解引用——地址可能被回收，只有“还有活行声明”才
  保证那个指针现在是 `Texture`；顺序反了就是 use-after-free。
- **测试**：`ContentHalvesTest.AHalfStaysPinnedByItsProgramWhenNoGeometryFeedsItsLayout`、
  `ContentPassTest.TheProducedSetsGiveEachTexturedDrawableItsOwnMap` 尾部的 pin 断言、
  `ResourceTableTest.TheVisitSeesEveryResidentValueAndCanChangeItsBody`。`test_vsg` 544 / `test_graphics` 554。

## 2026-11-12 —— 同日收口：半片 pin 加 layout 声明索引（取代上一条的 layout 部分）

- **新增第 4 个声明索引**：`ContentDeclarations` 多了 `layouts_`（`declaresLayout` / `hasLayoutIndex`）；
  `GeometryTable` 在 append / eraseIf / reclaimBefore / clear 的同一个方法里维护 `layout → 活行计数`
  （`Declarations<Key, Hash>` 现在按 key 泛化，buffers / layouts / textures / materials 共用一条规则）。
- **半片 pin**：`program@rev ∧ declaresLayout(layout)`；screen 半片只按 program。这样“最后一条使用该 layout
  的几何行离树 ⇒ 半片 park”是 O(1)，不再有 layout 累积，也不再需要每帧扫 geometry facts。
- **可选索引**：手建的 `ContentFacts` 没发布 layout 索引时，`ContentHalves` 退回扫描它的 `geometries`
  （与 `program_order` 空则扫描的规则一致）；生产 `ContentStore` 永远发布，帧路径不走回退。
- **公开头**：`VertexLayoutKeyHash`（`backend/Keys.hpp`，与 `operator==` 同字段）；`ContentDeclarations`
  构造参数把 layouts 放最后，旧的 2/3 参数调用不变。
- **测试**：`ContentStoreTest.TheDeclarationsFollowTheRowsAndOutliveThemForTheParkingWindow` 增加了 layout
  的 refcount / 最后一行离树断言；`ContentHalvesTest.AHalfIsPinnedByTheLayoutDeclarationAndLeavesWithIt`
  钉“索引说有就留、索引说没有就 park”。backlog `T28` 已删（收口项，不再是触发项）。
- **计数**：`test_vsg` 544 / `test_graphics` 554 / ctest 30/30 / TSan 554 用例 0 warning。

## 2026-11-13 —— 第三轮审计 W3：`FrameWorker` 拆成 `PublicationStore` + `FrameExpander`

- **对象级回边消除**（`G2` / `G12`）：`detail::PublicationStore`（`src/viz/graphics/src/PublicationStore.hpp`）
  住发布与绑定（`publish` / `bindSnapshot` / `hasExplicitSnapshot` / `beginFrame` / `sourceOf` + 表锁）；
  `detail::FrameExpander`（`src/viz/graphics/src/FrameExpander.hpp`）只读它（`runPass` / `run` / `unbound`）。
  内容半段不再需要引擎半段的对象；旧 `FrameWorker` 两个文件删除，`SceneSnapshotRef` 的 friend 改为
  `detail::PublicationStore`。
- **发布粒度**：`buildFrameValue` 先跑完本帧所有 `before_draw`（各自记钩子耗时），再按“每帧每场景一次”发布，
  最后 describe。旧形状每 pass 发一次（后 N-1 次 `Unchanged` 仍付锁 + epoch 比较）；先发布再跑后面的钩子会把
  钩子写的内容漏出本帧（`RenderEngineTest.AFrameAnswersOneStateSoEveryPassSeesWhatAnyHookWrote` 钉住这一点）。
- **`record_micros`**：钩子阶段先记钩子耗时，describe 阶段把发布（首个命名该场景的 pass）+ resolve + describe
  累加进去，总口径仍是“describe + collect”。
- **测试**：旧的 `FrameWorkerTest`（已删除）拆成 `PublicationStoreTest.cpp` 与 `FrameExpanderTest.cpp`；
  `FrameRunnerTest` / `ValueChannelTest` 改成两个对象。`T3` / `T31` 由
  `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch` 钉住；`T34` 由
  `FrameExpanderTest.AFrameExpandsTheSamePictureTheSceneWould`、`T35` 由
  `FrameExpanderTest.ABoundPublicationIsThePictureTheFrameDraws` 钉住。
- **计数**：`test_graphics` 556 / `test_vsg` 542 / TSan 556 用例 0 warning。全量并行 ctest 在这台机上
  偶发 `test_vsg` 的 `backend->initialize()` false（4 例）与移动相机测量超预算；单套件重跑即绿，先重跑再怀疑回归。

## 2026-11-13 —— 第三轮审计 W4：计划只表达 `PassKind` 的形状

- **规则**：计划只能说 `Scene` / `Screen`；子类 pass 是 `describe()` 的逃生口，不在计划契约内。同名同 kind
  的声明原地更新会保留它（节点与 `describe()` 都活着）；kind 变更会被重建为内置基类，所以 `applyPlan` 拒绝。
- **新 code**：`PlanProblemCode::SubclassPassWouldBeRebuilt`（`subclass-pass-would-be-rebuilt`），由
  `RenderEngine::collectRebuildProblems` 在 `validatePlan` 之后判定——纯计划检查看不见"这个名字下已注册的
  是哪个对象"。判定用 C++ 动态类型（`typeid`），不是 metatype：没写 `VN_OBJECT_META_DECL` 的快速子类也要被拦住。
- **测试**：`PlanTest.AKindChangeOnACustomPassIsRefusedWithItsCode`（原地更新保留节点 → kind 变更被拒 →
  内置 pass 仍可换 kind）；`TheCodeSpellingsAreStable` 增加新 token。`test_graphics` 557。

## 2026-11-13 —— 第三轮审计 W6a：`Scene` 的派生侧搬进 `detail::ScenePublisher`

- **搬走的东西**：`SceneBake`、per-view `DrawListMemo`、`content_revision_` / `memo_epoch_` / `content_frame_`、
  编辑批 `edit_depth_`、发布版本对与 `published_snapshot_`、三个计数，全部落进 `src/ScenePublisher.{hpp,cpp}`。
  `Scene` 只剩作者树（root / lights / visible / opacity）、查询（boundingBox / findNode）与委托
  （collect / publish / beginEdit / endEdit / setContentFrame / invalidateContent / 计数）。依赖方向只有
  Scene → publisher（publisher 以 `Scene&` 读作者的树；`Scene` 是它的 friend）。
- **门没变**：`detail::SceneRows::snapshot(scene)` 仍是一行门，转给 `ScenePublisher::snapshot()`；`SnapshotShadowTest` /
  `RenderWorldTest` 对它的引用不用改。空的共享命令表由一个 `detail::noCommands()` 统一（两个 TU 各存一份会破坏
  `TheSharedCollectionIsOneListAndTheOwningSpellingCopiesIt` 的"空列表是同一个"断言）。
- **新文件要 reconfigure**：库源码是 `file(GLOB_RECURSE)`（无 `CONFIGURE_DEPENDS`），加 .cpp 后必须
  `cmake -S . -B build`（`build-tsan` 同）再构建，否则链接期只看到未定义符号。
- **测试**：`test_graphics` 557 / `test_vsg`（gtest_list 546 例）全绿 / TSan 557 用例 0 warning；公开 API 与测试语义未动。

## 2026-11-13 —— 第三轮审计 W2：迁移 API 的状态标记

- 四个入口 `applyPlan` / `plan` / `bindSnapshot` / `prepare` 标为“迁移 API，尚未成为宿主契约”：设计文档
  （`graphics-frame-pipeline.md` §4、`graphics-threading.md` §4.2、`graphics-architecture.md` §1.3 / §2.10）与
  公开头文件的 `@note` 同时写明；5b-2c 前不再新增公共入口。
- **决定**：`FramePlan` 在 5b-2c 改名 `FrameTable`（它读的是编译后的帧表，不是计划）；`SessionBusy` 随 `W7`
  移出 `PlanProblemCode`（它描述运行时前置条件，不是形状缺陷）。两项都在 W9 窗口一次改完，之前不动公开签名（`C11`）。
- 计数：0 代码改动；生产路径仍是 `addPass` 家族 + 帧首隐式发布。



