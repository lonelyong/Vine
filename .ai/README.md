# .ai/ —— AI 辅助目录

面向 AI 助手与团队的仓库内知识，随 git 版本化。`docs/guides/` 是使用者文档；设计文档与笔记放这里。
已关闭、已明确不做、已被取代的内容不在树内保留（需要时看 git），只保留现行设计、
决策记录、开放待办与模块要点。

**索引由机器钉住**（`scripts/check_ai_docs.py`）：`.ai/` 下每一份 `.md` 必须在这里被点名，点到的名字必须存在。
**写法**：结论在上、历史在下；现行段落提到已不存在的东西时加 `<!-- drift-ok -->`。

**分层**：`design/` 只写**现在是什么**（现行、无日期、≤300 行）；`decisions/` 写**为什么这么定**
（ADR，日期不可变）；`backlog/` 只写**还没做、有触发条件**的事；`memory/` 写短要点；`bugs/`
一 bug 一文件。历史归 git，不放进 `design/`。

**目录分工**：`src/<层>` 放**库**；`src/apps/<产品>` 放**要安装的应用**（`src/apps/vine` → `Vine.exe`）；
`tools/<名字>` 放**不安装的开发工具**（`tools/urdf2vine`）；`tests/` 放用例。可执行文件不等于产品：
一个模块自己的进程半边（如 `src/base/crash/handler` 的 `CrashHandler.exe`）留在模块里，不搬进 `src/apps/`。

## design/ —— 现行设计文档（46）

每份设计文档的统一结构：**一句话 → 设计要点（1.x）→ 功能点（F#）→ 公开接口 → 注意项（N#）→
否决项（D#，一行 + ADR 链接）→ 判据（T#）→ 相关文档**。

**graphics / vsg（9）**
- `graphics-architecture.md` —— 分层、设备/窗口/引擎/链/视图、一帧与调度。
- `graphics-scene.md` —— 节点、状态、收集、层、视图上下文、拾取。
- `graphics-geometry.md` —— 通道、`Primitive`、实例化、revision、包围盒、存储与共享。
- `graphics-rendering.md` —— 目标、MSAA、图源、颜色、着色、阴影。
- `graphics-content-lifecycle.md` —— 内容释放与缓存策略。
- `vsg-backend.md` —— vsg 后端架构 / 类图 / 契约 / 键与状态 / 坑索引 / 否决项 / 门禁。
- `vsg-pitfalls.md` —— vsg 后端 P1–P82 坑表，按九个风险域分组，每条带症状与判据。
- `graphics-shader-params.md` —— 着色参数 ABI：五个块每份声明一个 owner，GLSL 去重、名字只在接线期。
- `graphics-material.md` —— 材质：metallic-roughness PBR、纹理集、资产/实例、两条路径一致（草案）。

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

**base 与其他（20）**
- `async-design.md` —— `src/base/async` 设计（三条必守规则）。
- `base-crash.md` —— 崩溃报告：`vn::Crash` 装配/报告格式/handler 符号化与崩溃窗口。
- `brep-operations.md` —— 运行时 B-rep 运算：模块归属（`brep` / `brepio` / `geometry`）、三角化与由基本体造实体的契约、失败表示。
- `geometry-axis-surfaces.md` —— 轴对称解析曲面（圆柱 / 圆锥 / 圆环）的坐标系值 `Placement`、共用的“平面内轴”规则（`Frame.hpp`）、矩形裁剪曲面与样条曲面的现状。
- `geometry-kinds.md` —— 几何族：`Curve` / `Surface` 都是求值接口（`Curve<T>` 十一项、`Surface<T>` 十四项，含 `isValid()`），曲面多一层 `ElementarySurface` / `BoundedSurface` 分类标签；曲线 / 曲面族已无占位类型。
- `geometry-parametric.md` —— 参数化几何是建模类型（`Primitive` 的五个子类同居 `Primitive.hpp`）；canonical 局部空间与 OCCT 的职责收缩。
- `geometry-primitives.md` —— geometry 的空间值类型：`Aabb`/`Obb`/`LineCurve`/`Plane` 的归属、命名与契约。
- `geometry-ray.md` —— 射线与图元相交：`Ray` 归 `geometry`、相交挂在被交对象上、场景走查留 `graphics`。
- `geometry-intersection.md` —— 相交配对与结果形态：动词、结果类型由交集维数决定、`false` 时出参语义、入口配对表与缺口；曲线 × 解析曲面的带符号距离求根（`intersectCurveWithSurface` + `signedValue`）与曲线 × 任意曲面的投影求根（`intersectCurveWithSurfaceByProjection`）。
- `geometry-surface-queries.md` —— 族级曲面查询：点到曲面的最近点 / 距离（高斯-牛顿）与射线 × 曲面（三元牛顿），参数窗口规则、重合分类与局部解的代价。
- `geometry-curve-queries.md` —— 点到曲线的最近点 / 距离（平方距离驻点求根）、窗口规则与局部极小值的限制。
- `geometry-solid.md` —— 解析实体的族 `Solid<T>`（五个 kind 是值、`SolidShape` 是 variant）：最近点与射线求交，模型按值持有。
- `geometry-tessellation.md` —— 解析实体的三角化：`Solid<float>` 到 `TriangleMesh` 的自由函数、质量参数、精确法向与顶点复制、闭合与外向绕序。
- `imaging-design.md` —— imaging 模块（CPU 像素数据）。
- `imageio-design.md` —— imageio 的公开面（容器能力表、`Result`/`IoError`、无状态自由函数）。
- `iobase-vfs-design.md` —— IOBase（`vn::io`）VFS 与 Stream。
- `meshio-brepio-design.md` —— meshio / brepio 的公开面（草案：格式枚举、`Result`/`IoError`、无单例）。
- `robotics-io-design.md` —— Robotics IO（XML、VFS 打包、材质库）。
- `robotics-proximity-design.md` —— Robotics proximity（接口、VMR 对照、FCL 接入点）。
- `window-design.md` —— window 模块的跨平台窗口上下文。

## decisions/ —— 决策记录（ADR）（33）

一份 ADR 只记一个决定：背景、选项、决定、后果、证据。状态为 `已采纳` / `已取代` / `已否决`，
日期不可变；被取代时在旧文件标 `已取代` 并链接新 ADR。

- `ADR-2026-11-04-curve-evaluation-interface.md` —— 曲线族改为求值接口（`Curve<T>` 虚函数 + `TrimmedCurve`），`Ray` 退出曲线族。
- `ADR-2026-11-04-surface-family-queries.md` —— 族级曲面查询用自由函数 + 参数窗口 + 牛顿迭代，不扩 `Surface` 接口、不逐类型写特例。
- `ADR-2026-11-04-surface-evaluation-interface.md` —— 曲面族改为求值接口（`Surface<T>` 十三项虚函数 + 两个分类标签），取代曲线 ADR 的第 6 条。
- `ADR-2026-11-04-geometry-precision-and-instantiation.md` —— 几何类型保留精度模板，`float` / `double` 两种精度都显式实例化并导出。
- `ADR-2026-11-04-geometry-kinds.md` —— 几何族用空标签分类，不用虚基类。
- `ADR-2026-11-04-primitive-is-a-shape.md` —— 参数化几何是建模类型（`Primitive` 子类），算法属于曲面；OCCT 收缩为 STEP 解析与三角化。
- `ADR-2026-11-05-solid-family-and-variant.md` —— 解析实体是族接口 `Solid<T>` + 封闭值集 `SolidShape`，模型按值持有；指针持有与“纯 variant 无接口”均否决。
- `ADR-2026-11-04-curve-third-derivative.md` —— 偏置曲线的二阶导数不实现，也不给 `Curve<T>` 加三阶导数；`OffsetCurve` 保持抽象。
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

## research/ —— 调研与提案（0）

非权威、带日期；落地后并入 `design/` 或 `decisions/`，并删除这里的文件。当前没有在途提案：曲线 × 曲面求交的提案已并入 `design/geometry-intersection.md`（2.7 / F11–F12）并删除。

## backlog/ —— 开放待办（10）

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
- `loaders.md` —— loaders 层的开放项（点云 I/O 的归属与入口）。

## memory/ —— 模块要点（10）

- `graphics.md` —— graphics / vsg 现役要点：机制、判据、坑与“当时为什么这么定”。
- `core.md` —— core 要点：类型描述符只经 `Type::obtain()` 注册、内联与出实参两种声明形式、接口的导出约定。
- `geometry.md` —— geometry 要点：SDK 类导出约定、五个基本体的局部空间。
- `appfw.md` —— appfw 横切规则（线程 / 锁内不跑用户代码 / 生命周期）。
- `async.md` —— `src/base/async` 要点。
- `imaging.md` —— imaging 要点。
- `imageio.md` —— imageio 要点。
- `loaders.md` —— loaders 层索引。
- `robotics-kinematics.md` —— Robotics kinematics 要点。
- `robotics-proximity.md` —— Robotics proximity 要点。

## bugs/ —— Bug 记录（28）

现象 / 根因 / 状态 / 涉及文件 / 验证，一 bug 一文件；已修复记录保留历史标记，未修复项标明状态。

- `vsg-embedded-init-crash.md`
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
