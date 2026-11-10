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

## 2. 内容模型

- `Geometry` = 通道集；`Primitive` = 一次 draw（topology + index span + material / program + instance stream）。
- 索引与实例流只写在 `Primitive`；`Geometry::material` / `program` 是**几何级默认**（图元自己的值优先，再否则最近 `StateNode`）。
- 通道 = `BufferSlice<float>` + components。`setPositions` / `setNormals` / `setTexcoords2` / `setTexcoords3` 共享 buffer；arena 用 slice。
- 实例流：每实例 16 个 float（列主序 mat4）；`visible_instances` 是 draw 参数，不重传；`VN_INSTANCED` 决定 binding 4 与顶点布局。
- revision：**形状归几何**（`Geometry::setRevision` / `bumpRevision`），**字节归 buffer**（`Buffer::setRevision`）。上传缓存只认 buffer revision；几何 revision 只驱动事实行重建。
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

## 6. 证据与门禁

- `test_graphics`（模型无关）、`test_vsg`（后端，含真实设备用例）、`test_gui`、`test_core`。
- `scripts/vsg_rewrite_gate.sh`：suite 两跑 0 VUID / 0 SYNC-HAZARD、相位行闭合、应用画面百分比。
- 静态门禁：`check_ai_docs.py`、`check_doc_symbols.py`、`check_include_hygiene.py`、`check_export_annotations.py`、`check_diagnostic_formats.py`、`check_public_docs.py`。
- `scripts/asan_check.sh` / `VN_ASAN_*`；`skipped == 0` 才算跑过设备电池。
- 量后端每帧成本：`test_vsg --gtest_filter='*MeasureWhatEachKind*'`（打印 µs/帧 + `builds`/`uploads`/`created`，需真实窗口，headless 会 skip）；成本模型与分配上限见 `.ai/design/vsg-backend.md` §2.10 / N4 / T5。
