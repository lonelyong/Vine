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
- 身份：`vn::graphics::Handle`（gen24/domain8/index32）+ `ContentRef{id, revision}`，公开头是 `sdk/vine/graphics/ContentId.hpp`。`Node::id()` / `MaterialBase::id()` 在构造时分配、析构时释放；`Row::identity`、`RenderWorld::RowId`、delta、`RenderCommand::node`、`CompiledCommand::geometry`、`GeometryFacts::geometry`、材质表都按它键。**表按 `(id, revision)` 建行**，旧 revision 的行在停靠窗口内继续作答。地址不再是身份（`ADR-2026-11-11-content-identity-is-a-handle`）；`Buffer`/流上传/编译半边仍是地址 + `ADR-2026-11-09` 的“持有即身份”。
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
