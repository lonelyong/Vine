# .ai/ —— AI 辅助目录

面向 AI 助手与团队的仓库内知识，随 git 版本化。`docs/guides/` 是使用者文档；设计文档与笔记放这里。
已关闭、已明确不做、已被取代的内容不在树内保留（需要时看 git），只保留现行设计、
决策记录、开放待办与模块要点。

**索引由机器钉住**（`scripts/check_ai_docs.py`）：`.ai/` 下每一份 `.md` 必须在这里被点名，点到的名字必须存在。
**写法**：结论在上、历史在下；现行段落提到已不存在的东西时加 `<!-- drift-ok -->`。

**分层**：`design/` 只写**现在是什么**（现行、无日期）；`decisions/` 写**为什么这么定**
（ADR，日期不可变）；`backlog/` 只写**还没做、有触发条件**的事；`memory/` 写短要点；`bugs/`
一 bug 一文件。历史归 git，不放进 `design/`。

**目录分工**：`src/<层>` 放**库**；`src/apps/<产品>` 放**要安装的应用**（`src/apps/vine` → `Vine.exe`）；
`tools/<名字>` 放**不安装的开发工具**（`tools/urdf2vine`）；`tests/` 放用例。可执行文件不等于产品：
一个模块自己的进程半边（如 `src/base/crash/handler` 的 `CrashHandler.exe`）留在模块里，不搬进 `src/apps/`。

## design/ —— 现行设计文档（54）

每份设计文档的统一结构：**一句话 → 设计要点（1.x）→ 功能点（F#）→ 公开接口 → 注意项（N#）→
否决项（D#，一行 + ADR 链接）→ 判据（T#）→ 相关文档**。

**graphics / vsg（11）**
- `graphics-architecture.md` —— 分层、设备/窗口/引擎/链/视图、一帧与调度。
- `graphics-scene.md` —— 节点、状态、收集、层、视图上下文、拾取。
- `graphics-geometry.md` —— 通道、`Primitive`、实例化、revision、包围盒、存储与共享；网格三角形身份（`triangleCount()` / `triangleCorners()`）与三角形树缓存（F32）。
- `graphics-rendering.md` —— 目标、MSAA、图源、颜色、着色、阴影。
- `graphics-content-lifecycle.md` —— 内容释放与缓存策略。
- `graphics-frame-pipeline.md` —— 帧管线（**草案**）：graphics-next 采纳后的目标形状 —— 内容 → 快照 → 派生 → 执行 四层单向、8 个名词 / 3 个动词、`C1–C12` / `R1–R39` / `I1–I7` / `N1–N10` / `P1–P11` / `D1–D11` / `T1–T34`，逐功能标落地状态，迁移表（步 5 拆成 5a / 5b-1 / 5b-2a / 5b-2b / 5b-2c）在 §9.3。
- `vsg-backend.md` —— vsg 后端架构 / 类图 / 契约 / 键与状态 / 坑索引 / 否决项 / 门禁。
- `vsg-pitfalls.md` —— vsg 后端 P1–P82 坑表，按九个风险域分组，每条带症状与判据。
- `graphics-shader-params.md` —— 着色参数 ABI：五个块每份声明一个 owner，GLSL 去重、名字只在接线期。
- `graphics-material.md` —— 材质：metallic-roughness PBR、纹理集、资产/实例、两条路径一致（草案）。
- `graphics-threading.md` —— 线程模型（**现行**）：内容侧 `publish()` 出一份不可变快照，引擎半段只读它；
  两条线程各有一个写者，跨边界的载荷只有「发布的值」与「完成报告」，字节内容靠两个原子 revision 轮询；
  表面归内容线程、交换链归引擎线程，并行只在派生层的纯值上。

**appfw（17）**
- `appfw-startup-phases.md` —— 启动阶段三拍与启动工作线程。
- `appfw-startup-splash.md` —— 启动框与启动进度。
- `appfw-render-surface.md` —— 渲染表面生命周期与平台回退。
- `appfw-render-view.md` —— 渲染视图设计（四层 API、配方归链、输入路由、着色模式）。
- `appfw-command-manager.md` —— CommandManager。
- `appfw-command-arguments.md` —— 命令参数：一份分词、随执行携带、命令自解、历史/事件可回放。
- `appfw-eventbus.md` —— EventBus。
- `appfw-plugin-system.md` —— 插件系统（含禁用与反初始化）。
- `appfw-config.md` —— 配置子系统。
- `appfw-progress.md` —— 环境进度（`ProgressHost`）。
- `appfw-userio.md` —— UserIO。
- `appfw-document-model.md` —— 文档模型（身份 + 生命周期 + 类型注册）。
- `appfw-ribbon.md` —— Ribbon 身份/角色/放置：id、role、placement、终止区与顺序解析。
- `appfw-command-binding.md` —— 命令条目的显示状态：enabled/目标要求/描述 tooltip/勾选 provider。
- `appfw-backstage.md` —— 应用按钮事件、菜单/Backstage 二选一、顶层覆盖窗口与页面导航。
- `appfw-app-info.md` —— 应用身份与应用信息：`AppInfo`/`AppConfig::info`、`Application::appInfo()`、关于面板与对话框。
- `appfw-translation.md` —— 文案翻译：只翻 appfw 及以上层的界面文案，源语言英文、译文只有中文，Qt 目录随模块走并内嵌（可被文件覆盖），启动时装载、缺译文回退英文。

**base 与其他（24）**
- `async-design.md` —— `src/base/async` 设计（三条必守规则）。
- `base-crash.md` —— 崩溃报告：`vn::Crash` 装配/报告格式/handler 符号化与崩溃窗口。
- `brep-operations.md` —— 运行时 B-rep 运算：模块归属（`brep` / `brepio` / `geometry`）、三角化与由基本体造实体的契约、失败表示。
- `crypto.md` —— crypto 模块：只做算法不做策略、一个对象一个方向、增量与一次性同义、后端初始化只有一处（`detail::ensureLibraryInitialized`）；今天的实现（摘要 / HMAC / HKDF / 口令哈希 / AES-GCM·CBC·CTR / RSA / ECC + DER、PEM 与口令保护的 PKCS#8 密钥文件）与缺口（证书解析）。
- `geometry-axis-surfaces.md` —— 轴对称解析曲面（圆柱 / 圆锥 / 圆环）的坐标系值 `Placement`、共用的“平面内轴”规则（`Frame.hpp`）、矩形裁剪曲面与样条曲面的现状。
- `geometry-kinds.md` —— 几何族：`Curve` / `Surface` 都是求值接口（`Curve<T>` 十一项、`Surface<T>` 十四项，含 `isValid()`），曲面多一层 `ElementarySurface` / `BoundedSurface` 分类标签；曲线 / 曲面族已无占位类型。
- `geometry-planar.md` —— 平面几何：`geom::planar::Curve<T>` 是与 3D 契约同义的第二份契约（不是泛型特化、不继承），今天有直线、圆、椭圆、双曲线、抛物线、圆弧、Bezier、BSpline、偏置；三条非圆圆锥曲线共享 `ConicCurve`（中心 + 起始轴 + `CN`），圆不进这层；圆弧是“圆按值 + 角度区间”；族级算法有最近点、三值求交、两曲线最近距离、弧长与反解、带符号曲率；有界类型各有 `bounds()`（`math::Rect2`，椭圆用支撑函数，样条是极点盒）；模块 header-only 模板、与空间侧同一个库（`vn::Geom`）与同一个 global，名字不带维度标记。
- `geometry-parametric.md` —— 参数化几何是建模类型（`Primitive` 的五个子类同居 `Primitive.hpp`）；canonical 局部空间与 OCCT 的职责收缩。
- `geometry-primitives.md` —— geometry 的空间值类型：`Aabb`/`Obb`/`LineCurve`/`Plane` 的归属、命名与契约。
- `geometry-module.md` —— 几何模块的形态与命名：一个库 `vn::Geom`、一个 global、四类位置（两棵维度树 / `Continuity` / `geom/detail`）、“什么放哪里”的三步判定、五条隔离门禁（含 `detail` 不得命名维度与 include 域）、命名规则（类型不带维度标记）、以及模块级的已否决项。
- `geometry-ray.md` —— 射线与图元相交：`Ray` 归 `geometry`、相交挂在被交对象上、场景走查留 `graphics`。
- `geometry-bvh.md` —— 空间包围体层次（BVH）：`Bvh` 是纯结构（包围体、32 字节扁平节点、`parent + 1` 布局、两种零分配遍历、`refit`、`validate`），`BvhBuilder` 是纯构建（两档质量、可注入外来算法），两者之间只有 `detail::BuildJob`；`Obb` 体积经实测否决（叶子表面积约 2 倍、包含性要容差）。
- `geometry-intersection.md` —— 相交配对与结果形态：动词、结果类型由交集维数决定、`false` 时出参语义、入口配对表与缺口；曲线 × 解析曲面的带符号距离求根（`intersectCurveWithSurface` + `signedValue`）与曲线 × 任意曲面的投影求根（`intersectCurveWithSurfaceByProjection`）。
- `geometry-surface-queries.md` —— 族级曲面查询：点到曲面的最近点 / 距离（高斯-牛顿）与射线 × 曲面（三元牛顿），参数窗口规则、重合分类与局部解的代价。
- `geometry-curve-queries.md` —— 曲线的最近点 / 距离（平方距离驻点求根）、两曲线最近距离、弧长与反解、曲率（3D 无符号）、窗口规则与局部极小值的限制。
- `geometry-curve-discretization.md` —— 曲线折线化：`CurveSampler<T>` 基类 + 参数等分 / 弧长等距 / 弦高偏离三个子类，`curve(c)` / `range()` / `compute()` 无参 / `polyline()` 的结果式对象壳与结果复位规则，`CurvePolyline<T>{points, parameters}`，2D 与 3D 共用的 `detail` 内核，播种与点数预算，折角不需要间断点表，进度钩子只留位。
- `geometry-solid.md` —— 解析实体的族 `Solid<T>`（五个 kind 是值）：最近点与射线求交，模型只存尺寸并按需构造实体。
- `geometry-tessellation.md` —— 解析实体的三角化：五个 kind 到 `Triangulation<T>`（位置 / 法向 / 索引）的自由函数重载、质量参数、精确法向与顶点复制、闭合与外向绕序，以及模型侧的 `meshFromTriangulation()` 装配。
- `imaging-design.md` —— imaging 模块（CPU 像素数据）。
- `imageio-design.md` —— imageio 的公开面（容器能力表、`Result`/`IoError`、无状态自由函数）。
- `iobase-vfs-design.md` —— IOBase（`vn::io`）VFS 与 Stream。
- `math-base-layer.md` —— `math` 是底座层且只有数学词汇：值类型 / 常量 / 概念 / 3×3 特征分解，18 个头无子目录、无过程；求根、积分、Bernstein 求值与样条表示约定都在 `geom`（两条切分判据 + 代价与反转路径）。
- `meshio-brepio-design.md` —— meshio / brepio 的公开面（草案：格式枚举、`Result`/`IoError`、无单例）。
- `robotics-io-design.md` —— Robotics IO（XML、VFS 打包、材质库）。
- `robotics-proximity-design.md` —— Robotics proximity（接口、VMR 对照、FCL 接入点）。
- `window-design.md` —— window 模块的跨平台窗口上下文。

## decisions/ —— 决策记录（ADR）（77）

一份 ADR 只记一个决定：背景、选项、决定、后果、证据。状态为 `已采纳` / `已取代` / `已否决`，
日期不可变；被取代时在旧文件标 `已取代` 并链接新 ADR。

- `ADR-2026-11-13-one-engine-one-surface.md` —— **一个引擎驱动一个表面**：否决 graphics-next 的 F8（一引擎多 Surface / `Target{SurfaceId}`），多窗口 = 一个设备服务多个引擎（每窗一引擎 / 一区 / 一个 `Viewer`）；`FrameResult.present` / `PresentRecord` 保持单值；触发条件（真实-host 双视口 / 第二后端）写在 ADR 里。
- `ADR-2026-11-12-adopt-graphics-next.md` —— **采纳 graphics-next 作为下一版帧管线设计**（四层单向：内容 → 快照 → 派生 → 执行）；目标形状的规范文档是 `design/graphics-frame-pipeline.md`（草案），`K1–K11` 从“否决提案”改成“每一步的验收阈值与回退条件”，backlog 的 `T32` 从触发条件型移出；否决保持现状、只留已落地的身份/寿命层、以及大爆炸重写。
- `ADR-2026-11-12-snapshot-takes-over-collection.md` —— **快照接管生产收集拆成 5a 影子 / 5b-1 翻源 / 5b-2a 公开入口 / 5b-2b 删除 / 5b-2c pass 收口**：5a 只跑新旧两路并逐项 diff、旧路径仍是渲染真相；5b-1 把 Published 模式绑到 `SnapshotSource`；5b-2a 公开 `Scene::publish()` 三态并让 `FrameWorker` 直接持有 `SnapshotSource`；5b-2b 删 `ContentMirror` / `DeltaChannel` / `RenderWorld` / `PublishOutcome` 与 23 条 LEGACY 用例；5b-2c 才删 `addPass` 家族与运行时拒绝；否决一步完成、拆三步、保留 mirror 与 debug-only 双跑。
- `ADR-2026-11-11-content-identity-is-a-handle.md` —— 内容身份是**句柄**（`Handle` gen24/domain8/index32 + `ContentRef{id,revision}`），不是地址：公开 `ContentId.hpp`、`Node::id()`、行身份/镜像键/`RenderCommand::node`/计划行/后端几何表全部改为 `NodeId`；分三段落地（公开身份 → `ContentRef` 唯一键 → epoch 回收）；部分取代 `ADR-2026-11-09-address-keys-hold-the-object`。
- `ADR-2026-11-10-render-world-mirrors-content.md` —— 渲染世界**镜像**内容世界：内容留在 UI 线程（编辑 API 不变），镜像（世界矩阵/世界盒/折叠状态/代理）在引擎线程，靠 delta 增量更新（payload 必填 ⇒ 漏发编译不过）、字节靠原子 revision 轮询；表面归内容线程、交换链归引擎线程；否决把内容移交给引擎线程（第一稿）、每帧 extract、双缓冲世界、加锁、每帧起线程、按 pass 并行记录、vsg 的 threading path。
- `ADR-2026-11-10-upload-staging-is-not-retained.md` —— 上传暂存**不保留**：一次上传的 host 可见缓冲是**一个** `vkCmdCopyBuffer` 的源，拷贝的 fence 一完成就清（`detail::StagingTask` 在每个帧边界扫，零等待、零 idle），SDK **没有**对应动词（`RenderDevice::releaseUploadStaging()` 与模型查看器的 `release_render_memory` 一并删除）；否决显式入口 + park/drain、每帧无脑清、换任务释放、以及“还有待拷数据就跳过”的便宜条件（那是会让 sweep 永久冻结的坑）。
- `ADR-2026-11-10-gpu-residency-pinned-by-content.md` —— GPU 驻留按**内容对象（身份 + revision）**钉住，回收只问“现在谁活着”（事实表枚举 pin 集合），删掉时间窗与容量上限：一套判据替掉现在的五种（流的时间窗、纹理/块的引用计数、半片与描述符集的反事实扫描）；显存不再自动让出是它的明码代价。第一步（`ResidencyLedger` 词汇 + 流/图像/材质块三行接入）已落地。
- `ADR-2026-11-10-upload-staging-release-is-explicit.md` ——（已取代）上传暂存的回收是**显式入口**，时机归宿主/用户命令。
- `ADR-2026-11-10-curve-sampling-is-an-object.md` —— 曲线离散化做成对象：基类 + 三个规则子类、setter 配置 + 无参 `compute()`；否决构造即算与“三个自由函数 + 返回值”。
- `ADR-2026-11-10-bvh-seal-and-coarse-picking-tree.md` —— BVH 可封存（`seal()` 放掉构建期的图元盒，只留节点 / 节点盒 / 顺序）、拾取树粗叶 64 且建成即封存、vsg 与索引上传改成别名模型 buffer（14M 三角形的拾取树 1.36 GB → 88 MB，后端再省 1.18 GB）；否决独立封存类型、删 `update`/`refit`、改库的默认叶容量。
- `ADR-2026-11-09-address-keys-hold-the-object.md` —— 按地址做键的缓存必须持有键名所指的对象：地址在行仍在时被回收，键就指向了新对象。
- `ADR-2026-11-09-buffer-edit-scope.md` —— `Buffer` 的写入口收成一个作用域（共享可变字节只有一个写入口）。
- `ADR-2026-11-09-scene-collection-two-halves.md` —— 场景收集拆成「烘焙」与「选取」两半（第一半已落地，第二半待测量）。
- `ADR-2026-11-09-box-transform-lives-in-geom.md` —— 盒的变换算子回到 `geom`（`Aabb::transformed()`，只接受仿射，非仿射答空盒），删除 `graphics::transformBox()` 与 `RayIntersection.cpp` 的副本；`Obb` 暂不提供（表示装不下一般仿射，零消费者）。
- `ADR-2026-11-09-solid-tessellation-in-geom.md` —— 三角化回到 `geom`：`tessellateSolid()` 返回精度模板 `Triangulation<T>`（位置 / 法向 / 索引），`meshFromTriangulation()` 在模型侧装配成 `TriangleMesh`；否决返回 `TriangleMesh`（回边）与 `Buffer`（多一次拷贝）。
- `ADR-2026-11-09-solid-closed-set-without-variant.md` —— 删掉 `SolidShape` 变体与 `solidShapeOf()`：封闭集合由五个 kind + `visitPrimitive()` + 一 kind 一个重载表达；`Solid<T>` 族接口与五个 kind 值不变。
- `ADR-2026-11-09-bvh-shape-is-the-tree-storage.md` —— BVH 的形状就是它自己的存储（`BvhTopology` 三条数组 + 平行的节点体积），`BvhTopology.hpp` 只留词汇；恢复有视图（`RestoreTopology`）与移动（`Bvh::restoreTopology()`）两个入口、一份检查。否决内部类、AoS 转录与访问器化。
- `ADR-2026-11-04-curve-evaluation-interface.md` —— 曲线族改为求值接口（`Curve<T>` 虚函数 + `TrimmedCurve`），`Ray` 退出曲线族。
- `ADR-2026-11-04-surface-family-queries.md` —— 族级曲面查询用自由函数 + 参数窗口 + 牛顿迭代，不扩 `Surface` 接口、不逐类型写特例。
- `ADR-2026-11-04-surface-evaluation-interface.md` —— 曲面族改为求值接口（`Surface<T>` 十三项虚函数 + 两个分类标签），取代曲线 ADR 的第 6 条。
- `ADR-2026-11-04-geometry-precision-and-instantiation.md` —— 几何类型保留精度模板，`float` / `double` 两种精度都显式实例化并导出。
- `ADR-2026-11-04-geometry-kinds.md` —— 几何族用空标签分类，不用虚基类。
- `ADR-2026-11-04-primitive-is-a-shape.md` —— 参数化几何是建模类型（`Primitive` 子类），算法属于曲面；OCCT 收缩为 STEP 解析与三角化。
- `ADR-2026-11-05-solid-family-and-variant.md` —— 解析实体是族接口 `Solid<T>` + 封闭值集 `SolidShape`（族与五个 kind 现行；“模型按值持有”与 `SolidShape` 变体本身已分别被后续 ADR 取代 / 删除）。
- `ADR-2026-11-05-primitive-is-a-modelling-base.md` —— `Primitive` 是建模基类：不持有实体，五个子类各存尺寸并 `solid()` 按需构造；几何 kind 改名 `*Solid`。
- `ADR-2026-11-05-planar-curve-contract.md` —— 平面曲线是第二份契约（与 3D 同名同义、互不继承），共享词汇留在 `math`；否决泛型 `Curve<PointT>`、继承 3D 契约与嵌入 `z = 0`。
- `ADR-2026-11-05-planar-arc-is-a-value-type.md` —— 平面圆弧是值类型（`ArcCurve` = 圆按值 + 区间），通用 `TrimmedCurve<T>` 不与之并存。（已被 `ADR-2026-11-05-planar-trimmed-curve.md` 取代）
- `ADR-2026-11-05-planar-trimmed-curve.md` —— 通用裁剪视图 `TrimmedCurve<T>`（`shared_ptr` 基曲线 + 区间）取代 `ArcCurve`；一段圆的紧盒回到 `CircleCurve::bounds(first, last)`。
- `ADR-2026-11-05-planar-offset-curve.md` —— 平面偏置是“具体基曲线类型”的模板 + 具体类型上的非虚三阶导，契约保持十一项；否决给 `Curve<T>` 加第 12 项与差分近似的二阶导。（已被 `ADR-2026-11-05-planar-offset-holds-handle.md` 取代）
- `ADR-2026-11-05-planar-offset-holds-handle.md` —— 平面偏置持有 `shared_ptr<const Curve<T>>` 基曲线，阶数 = 基曲线能力 − 1（封顶 2）；基曲线给不出三阶导时 `isValid()` 为假。
- `ADR-2026-11-05-curve-derivative-capability.md` —— 导数按能力暴露：`Curve<T>` 增加非纯虚 `maxDerivativeOrder()` / `derivativeAt(u, order)`，越界答 `std::nullopt`；否决把某一阶写成纯虚与 OCCT 式任意阶 + 异常。
- `ADR-2026-11-04-curve-third-derivative.md` —— 偏置曲线的二阶导数不实现，也不给 `Curve<T>` 加三阶导数；`OffsetCurve` 保持抽象。（已被 `ADR-2026-11-06-space-third-derivative-and-offset.md` 取代）
- `ADR-2026-11-06-space-third-derivative-and-offset.md` —— 空间曲线族新增非虚 `thirdDerivative()` 与能力覆写（报 3），`OffsetCurve` 用基曲线三阶导答二阶导并成为可构造值类型，阶数 = 基曲线能力 − 1。
- `ADR-2026-11-06-shared-numeric-kernels-in-math.md` —— 与维度无关的数值内核（自适应 Simpson、单调安全 Newton）住进 `math`，2D 与 3D 的弧长 / 反解共用一份实现。
- `ADR-2026-11-06-math-is-the-base-layer.md` —— `math` 是底座层而非纯数学库：数学词汇进根层、数学过程进 `numeric`、NURBS 表示约定（重数 / 夹持 / 连续度阶梯）进 `spline`；建 `vn::Nurbs` 的触发条件一并写下。
- `ADR-2026-11-06-merged-geom-module.md` —— `geom/planar` 与 `geom/spatial` 合并为 `src/base/geom` + 单库 `vn::Geom`，两棵 SDK 树共用一个 `geom/geom_global.hpp`；2D/3D 隔离从编译器保证改为 `check_geom_layering.py`。
- `ADR-2026-11-06-geom-detail-subtree.md` —— 模块内新增 `geom/detail/` 子树，收与维度无关的共享实现（曲线标量、窗口/容差规则、括号收窄、弧长与反解、两曲线细化）；两棵树都可 include 它，它不得 include 任一方；公开名字不动。
- `ADR-2026-11-06-spline-model-moves-to-geom.md` —— B 样条的节点模型与基函数移入 `geom/detail`、`Continuity` 移入 `geom` 根，判据是“签名里有没有我们自己的数据模型”；`math` 只剩数学词汇与数值过程（取代 `ADR-2026-11-06-math-is-the-base-layer.md` 的表示约定那一半）。
- `ADR-2026-11-06-math-keeps-only-vocabulary.md` —— 求根、积分与 Bernstein 求值也移入 `geom/detail`，`math` 只剩词汇（取代 `ADR-2026-11-06-shared-numeric-kernels-in-math.md`）；代价（非几何模块要用积分得依赖 `vn::Geom`）与反转路径（第二个消费者出现时升独立模块）写明。
- `ADR-2026-11-06-geom-tree-layout-and-naming.md` —— 两棵树的目录位置与维度命名维持现状：共享已由 `geom/detail` 与门禁解决，重排只剩名字好看；三个方案（嵌套 `planar`/`spatial`、平铺+`2` 后缀、两棵树都加后缀）及否决理由。
- `ADR-2026-11-06-geom-contracts-nested-under-module.md` —— 两个契约下沉为 `geom/{planar,spatial}` 目录与 `vn::geom::{planar,spatial}` 子命名空间（取代上一条的“维持兄弟目录”）；重导出归零、目录与命名空间一致；并记录一项预期收益的落空（嵌套不能缩短 `geom::detail::`，因为契约自己有同名 `detail`）。
- `ADR-2026-11-07-crypto-api-shape.md` —— crypto 的 API 形状：加解密用两个工厂而不是 `mode` 标志、增量 GCM 先缓冲后一次算完（失败会放行未认证数据，代价与退路写明）、后端初始化收在模块内一处。
- `ADR-2026-11-07-certificates-and-legacy-keys.md` —— 证书只取公钥不做信任判断（名为 F15 的边界，不是缺口）、旧式 `Proc-Type` PEM 只读不写、`WOLFSSL_MD5` 改为显式打开（枚举里有 `kMd5` 就不能靠预设的副作用）。
- `ADR-2026-11-07-encrypted-private-keys.md` —— 口令保护的私钥用 PKCS#8 + PBES2（AES-256-CBC + PBKDF2），不是自造容器：私钥文件的全部价值是别的工具也能读；否决 `toPrivatePem(password)` 重载（名字要说出格式变化）、把迭代数写死、用 PBES1 写出。
- `ADR-2026-11-07-bvh-as-structure-plus-builder.md` —— BVH 拆成结构与构建器（`BuildJob` 是唯一契约），`Obb` 体积经实测否决，取代 `geometry-ray.md` 的 `D5`。
- `ADR-2026-11-07-digest-vocabulary.md` —— 摘要收敛成一套词汇：`Digest` 枚举 + `digest()` + `DigestCalculator`，删掉 `Hash` / `HashCalculator` / `Hmac::Algorithm` 三套并行面；否决“每种算法一个类”。（命名部分已被下一条取代）
- `ADR-2026-11-07-mesh-triangle-tree-cache.md` —— 网格自带三角形身份（`triangleCount()` / `triangleCorners()` 上移到 `Mesh`）并把三角形树当缓存（`Bvh<double>`，一三角形一 primitive，id 即序号）。
- `ADR-2026-11-07-incremental-api-shape.md` —— 增量 API 的形态与名字：一次性用自由函数（`digest()` / `hmac()`），增量用东西本身命名（`Hasher` / `Hmac`），方法统一 `update()` / `finish()`；取消自造的 `*Calculator`。
- `ADR-2026-11-07-aesgcm-streaming.md` —— AES-GCM 改为真流式（去缓冲）：wolfSSL 5.9.4 的 `Init/Update/Final` 入口 + `WARN_UNUSED_RESULT`，代价是“解密输出在 `finish()` 前不可信”这一契约必须显式写明。
- `ADR-2026-11-04-axis-surface-frame.md` —— 旋转面的框架放进新的 `AxisSurface` 层，“平面内轴”的规则抽成 `Frame.hpp` 的一份实现；球面不迁移。（已被 `ADR-2026-11-04-surface-placement-value-type.md` 取代）
- `ADR-2026-11-04-surface-placement-value-type.md` —— 曲面的框架是值类型 `Placement`（原点 + 主方向 + X 方向），不是继承层；“平面内轴”仍只在 `Frame.hpp` 一处。
- `ADR-2026-11-04-isvalid-on-family-interfaces.md` —— `isValid()` 是 `Curve<T>` / `Surface<T>` 的纯虚函数：`const Curve<T>&` 也能问；`Plane` 因此第一次有了零法向判据。
- `ADR-2026-11-04-bspline-without-knot-breaks.md` —— 样条内部重数不超过度数：内部重数 = 度 + 1 是缺口，`isValid()` 拒绝它，`C0` 因此始终是“各段交于一点”。
- `ADR-2026-11-03-geometry-primitives-ownership.md` —— 空间值类型归 `geometry`，轴对齐盒改名为 `Aabb`；`Rect2` 留在 `math`，不留兼容别名。
- `ADR-2026-11-03-brep-operations-module.md` —— 运行时 B-rep 运算独立成 `brep` 模块；`geometry` 不碰 OpenCASCADE，探测集中在 `VineOpenCASCADE.cmake`。
- `ADR-2026-10-24-command-target.md` —— 命令的目标是视图（`DocumentView`），层可空、需求按命令声明；`RenderControl` 不做目标。
- `ADR-2026-11-01-translation-scope.md` —— 只有界面层（appfw 及以上）可翻译，源语言中文，目录用 Qt Linguist，v1 不做运行期切换（源语言一条已被下一条取代）。
- `ADR-2026-11-02-english-source-language.md` —— 源语言改为英文、中文进目录；缺译文回退英文，非中文系统落到 `en`。
- `ADR-2026-11-01-translation-catalog-embedding.md` —— 翻译目录随模块走、内嵌进模块，模块旁的同名 `.qm` 可覆盖；语言集合只有中文与英文，因此只有一本中文目录。
- `ADR-2026-10-28-ribbon-roles-and-placement.md` —— Ribbon 用 role + id + 锚点放置，终止区是解析器不变量；不做全局序号与 item id。
- `ADR-2026-10-28-backstage-overlay-window.md` —— Backstage 用顶层 Tool 窗口，不用 SARibbon 的子控件覆盖层（原生表面遮挡）。
- `ADR-2026-10-30-crash-handler-x11-on-linux.md` —— Linux 的崩溃窗口用 Xlib + Xft 手画（系统库，不链 UI 工具包；取代同日的 Qt 版本）。
- `ADR-2026-10-30-crash-handler-qt-on-linux.md` ——（已取代）Linux 的崩溃窗口用 Qt Widgets。
- `ADR-2026-10-28-document-tabs-and-floating.md` —— 文档的 tab 在中央区，浮动是自已的顶层窗口（不进停靠树）。
- `ADR-2026-10-28-central-area-shows-current-document.md` —— 中央区只显示当前文档，浮动的当前文档用说明页占位（一个 current、三处一致）。
- `ADR-2026-10-24-command-target-no-window-query.md` —— 目标只走一条路：不留窗口级的"当前 3D 区"查询，视图的访问器叫 `renderControl()`（部分取代上一条的决定 4）。
- `ADR-2026-10-24-tabs-not-mdi.md` —— 一个窗口同一时刻只显示一个视图（tab）；并排看几块 3D 区由视图自己的窗格表达，不做 MDI/分屏。
- `ADR-2026-10-24-material-model.md` —— 材质改为单一 metallic-roughness PBR + 稀疏实例；alpha 模式进材质，G-buffer 重排。
- `ADR-2026-10-24-light-block-typed-lights.md` —— 灯块改为带类型的灯数组（位置/范围/锥角），两条路径共用 UBO。
- `ADR-2026-10-24-shader-parameter-abi.md` —— 着色参数手写 + 去重，不做反射与材质 DSL。
- `ADR-2026-10-23-no-multi-parent-dag.md` —— 不引入多父，严格树 + 实例化。
- `ADR-2026-10-23-instancing-per-primitive.md` —— 实例化挂在 `Primitive`，不做作用域。
- `ADR-2026-10-23-l2a-reverted.md` —— L2a 回撤，分层目标等 L2b 一起做。
- `ADR-2026-10-22-engine-schedules-passes.md` —— pass 调度只在引擎。
- `ADR-2026-10-22-cache-unbounded-explicit-release.md` —— 缓存不设上限，释放显式管理。
- `ADR-2026-10-22-msaa-target-property.md` —— MSAA 是目标属性，颜色/深度各解析一张图。
- `ADR-2026-10-21-colour-space-in-types.md` —— 颜色空间与数值域放进类型。
- `ADR-2026-10-19-produced-vs-imported-image-sources.md` —— produced 与 imported 图源不合并。

## research/ —— 调研与提案（9 文件 / 1 组提案）

非权威、带日期；落地后并入 `design/` 或 `decisions/`，并删除这里的文件。

- `graphics-2026-11-12-audit-3.md` —— graphics 的**第三轮审计**（只读）：线程载荷与契约相反（R3-1）、三条新公共入口零生产消费者（R3-2）、`Scene` 三概念同居（R3-3）、`backend/` 公共安装与 Vulkan 拼写（R3-4）、内容半段调引擎半段（R3-5）、结果形状七套（R3-6）、扩展轴与计划互斥（R3-7）、`RenderEngine` 组合根（R3-8）；含修复任务表 W1–W9 与“现在不做”的触发条件。
- `graphics-2026-11-12-instances.md` —— 原型与实例（A6）的 `T23` A/B 测量口径：5000 行原型 × 10 实例、E0–E3 四类编辑、`snapshot_rows` / `draw_rows` / 放置乘法 / publish / select / 常驻六项；B 的判定线（O(R+N)、常驻 ≤ 2× 1× 原型、`K11` 绿）与“数字不好就停在原型 DAG 共享”的回退。**未采纳**。
- `graphics-2026-11-11-next.md` —— graphics 整体骨架的重设计：**总纲**（问题来源、骨架摘要、`I1–I7`、
  判死条件 `K1–K6`、验证计划、否决项、`G1–G13` 对照）。对应 backlog `T32`；采纳则改写 `design/`，否决则留 ADR。
- `graphics-2026-11-11-next-requirements.md` —— 同上的**目标与需求**：`O1–O7` / `R1–R30` / `N1–N10` /
  `C1–C12` / `T1–T16`，并给出现状 `C1–C20` 的去向。
- `graphics-2026-11-11-next-design.md` —— 同上的**初步设计**：分层、类型草案、hot/cold 分块布局、
  publish / fold / select / sort / record / reclaim / plan-diff 算法、线程与内存模型、诊断、后端契约。
- `graphics-2026-11-11-next-benefits.md` —— 同上的**收益与代价**：按维度量化收益与测量方式、`P1–P7`、
  三批迁移与每批的判死条件、反方论点回应。
- `graphics-2026-11-11-next-hot-paths.md` —— 同上的**性能/动画/后期/扩展**：实测成本模型与 60 fps 预算、
  select/fold/sort/record 的手段、值的热路径三条规则、动画走"帧内 pose"（不经发布）、后期＝计划即依赖图、
  受控逃逸口、工程效率、`N11–N18`，以及"先给 `Measure*` 加阈值"的优先级。
- `graphics-2026-11-11-next-api.md` —— 同上的**现有 API 集审计**：`P1–P32`（一概念两表示、声明里的回调、
  目标四种说法、顺序四套、49 个公开方法、七个别名…）、`M1–M10` 缺失能力、逐方法去向、命名对照、A–E 分批。
- `graphics-2026-11-11-next-audit.md` —— 同上的**第二轮审计**：证据口径复核、跨文档矛盾 `X1–X10`、
  设计缺口 `A1–A13`、补充要求 `R31–R39` 与判死条件 `K7–K11`、成本再评估与修订后的推进顺序。

（此前的三角形树两帧统一与 `.vbtp` 持久化已并入 `design/graphics-geometry.md` 2.10 / F33、
`design/geometry-bvh.md` 2.13 / F8 与 `design/robotics-io-design.md` F7。）

## backlog/ —— 开放待办（13）

只留**还没做、有触发条件**的条目；做完就删，决定不做也删（理由进 ADR）。

- `graphics.md` —— graphics / vsg 的开放待办（内容、后端资源、平台、对象模型）。
- `appfw-startup.md` —— 启动流程剩余项（Windows 半边）。
- `appfw-ribbon.md` —— Ribbon 的开放项（order、item id、自定义布局、上下文 tab、manifest）。
- `appfw-command-arguments.md` —— 命令参数的剩余项（声明式参数、帮助/补全、缺参提问、对象参数、长度上界）。
- `appfw-app-info.md` —— 应用信息的开放项（构建期版本、清单与第三方致谢）。
- `appfw-translation.md` —— 翻译的开放项（运行期切换、第一本英文目录、插件元数据、lupdate 门禁、覆盖位部署规则）。
- `async.md` —— async 下一步（取消环境、作用域、帧分配的 go/no-go）。
- `base-crash.md` —— 崩溃报告的开放项（上次崩溃提示、terminate、栈保证、只读目录、Linux、上报触发条件）。
- `brep.md` —— 运行时 B-rep 运算的开放项（OCCT 后端验证、布尔与缝合）。
- `crypto.md` —— crypto 的开放项（错误码分类、Argon2 线程模式、写出方案可配置、第二个 AEAD、泄漏检测）。
- `geom-planar.md` —— 平面几何的开放项（椭圆 / 圆弧 / `ConicCurve` 层、平面查询、`CircleCurve::pointAt`）。
- `geometry.md` —— 空间几何的开放项（实体求交、三角面类型、四阶导数与偏置阶数、样条求值的表算法与临时分配、缺的相交配对）。
- `loaders.md` —— loaders 层的开放项（点云 I/O 的归属与入口）。

## memory/ —— 模块要点（11）

- `graphics.md` —— graphics / vsg 现役要点：机制、判据、坑与“当时为什么这么定”。
- `core.md` —— core 要点：类型描述符只经 `Type::obtain()` 注册、内联与出实参两种声明形式、接口的导出约定。
- `crypto.md` —— crypto 要点：后端初始化只一处、wolfSSL 的 `Md5`/`Sha256` 宏与 `::Hmac` 同名坑、增量 GCM 缓冲、测试向量与命令。
- `geometry.md` —— geometry 要点：SDK 类导出约定、五个基本体的局部空间。
- `appfw.md` —— appfw 横切规则（线程 / 锁内不跑用户代码 / 生命周期）。
- `async.md` —— `src/base/async` 要点。
- `imaging.md` —— imaging 要点。
- `imageio.md` —— imageio 要点。
- `loaders.md` —— loaders 层索引。
- `robotics-kinematics.md` —— Robotics kinematics 要点。
- `robotics-proximity.md` —— Robotics proximity 要点。

## bugs/ —— Bug 记录（29）

现象 / 根因 / 状态 / 涉及文件 / 验证，一 bug 一文件；已修复记录保留历史标记，未修复项标明状态。

- `vsg-embedded-init-crash.md`
- `vsg-content-address-recycled-before-rows.md`
- `spline-knotspan-hang.md` —— `geom::detail::knotSpan()` 在“最后两跨”括号上不前进，带内部节点的 B 样条求值挂死。
- `vsg-embedded-blank-render.md`
- `vsg-resize-distortion.md`
- `vsg-maximize-black-band.md`
- `shared-task-batch-resume-uaf.md`
- `docking-flyout-under-render.md`
- `docking-layout-transitions.md`
- `docking-tab-dangling-client.md`
- `docking-title-click-hides-pane.md`
- `model-viewer-fit-not-framing.md`
- `async-scope-reuse-after-stop.md`
- `async-runon-move-only-arguments.md`
- `async-queue-reentrant-pop-uaf.md`
- `async-cv-wait-cancel-lock.md`
- `async-tcs-multiple-value-consumers.md`
- `async-queue-nondefault-values.md`
- `async-withcancellation-token-propagation.md`
- `async-queue-noexcept-failures.md`
- `appfw-commandmanager-shutdown-drain-timeout.md`
- `appfw-commandmanager-exclusive-admission-race.md`
- `appfw-commandmanager-admission-exception-safety.md`
- `appfw-commandmanager-current-command-pointer-lifetime.md`
- `appfw-commandmanager-async-instance-lifetime.md`
- `appfw-commandmanager-disabled-preference-lost-update.md`
- `vsg-depth-bias-not-declared-dynamic.md`
- `graphics-renderengine-addpass-null-pass-crash.md`
- `graphics-scene-memo-key-missing-view-basis.md`
- `graphics-transparent-sort-uses-origin.md`

约定：`memory/` 保持简短要点式；详细设计放 `design/`；bug 一条一文件。
