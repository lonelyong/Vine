# appfw 模块要点（`src/fw/appfw`）

> **启动阶段三拍（2026-09-26）**：`startupStart()` / `startup()` / `startupEnd()` 是**异步方法**
> （`vn::async::Task<void>`，protected，声明为类末尾一组）；`run()` 推的 `startupSequence()` 协程里三行
> `co_await` 平铺（每拍之后垫一次 `resumeOnMainThread()`，因为 `co_await` 之后跑在唤醒它的线程上；三拍罩在
> 一个 `try` 里，失败路径先垫一次再 `failStartup()`——`co_await` 不能写在 catch 块里）。上报口建在第一拍前、
> **收在最后一拍之后**。启动期异常**致命**：记日志 + 收上报口 +
> `exit(1)`（`failStartup()`），不走 `startupEnd()`；插件 `loadAll()` 返回 false 则不算失败。
> 宿主的启动工作：**框架不替宿主拥有线程**（`run(work)` 重载已删）——宿主重写 `startup()` 那一拍，先
> `co_await Application::startup()`（插件先加载），自己的重活再 `co_await vn::async::run(...)` 丢到进程线程池上；
> 它抛的异常从那一拍抛出 ⇒ 与叶子异常同样致命。
> `Task` 是懒的 ⇒ 调用点必须 `co_await`（不跑循环的叶子用 `Task::result()`）。`VN_APPFW_PLUGIN_ABI_VERSION` = **7u**
> （钩子 `preLoad/load/postLoad` 变成 `Task<void>`；**每条出口都要写 `co_return;`**，否则返回空 `Task` / `ud2`）。
> 早期笔记里的 `beginStartup` / `finishStartup` / `runStartup` 就是这三个的新名。见 `.ai/design/appfw-startup-phases.md`。
> **插件加载两道门（2026-09-26）**：`loadAllAsync()` 是启动用的协程（插件自己能在钩子里跳池 + `resumeOnMainThread()`
> 回来）；`loadAll()` 是给不跑循环的工具/测试用的同步门（`MainThreadDispatcher::runToCompletion()` = async 的 `runToCompletion(task, pump)`，pump 就是
> `deliverPostedCalls()`，等的时候只派发它——
> **不能用 `Task::result()`**：它只阻塞不派发，钩子"回到应用线程"那一步会死锁）。管理器**自己不转循环**。
> **三趟生命周期，`postLoad()` 是"全员 load 完"那一趟（2026-09-27 说准）**：`preLoad()` 全员 → `load()` 全员 →
> `postLoad()` 全员；声明依赖只决定 `load` 那一趟里的先后（被依赖的先），而 `postLoad` 在整趟 `load` 结束之后 ⇒
> "内容插件先挂骨架、外壳后建会话"就落在这里：`app_shell` 只搭外壳（窗口/停靠布局/控制台）并在 `load()` 里
> `setPrimaryRenderControl()` 发布 3D 视图，`demo_plugin` 在**自己的** `load()` 里把 `DemoScene` 骨架挂上去，
> 会话 attach 在 `app_shell::postLoad()`（管线由内容插件注册的 pass 建，早一拍就会漏；attach 又要贴着主窗上屏）。
> 顺序也由这条保证：`demo_plugin` 声明依赖 `app_shell` ⇒ 它的 `load()` 在外壳之后。
> **文档模型（2026-09-27，core + 打开/保存 + 元数据/面板 + 视图层已落）**：框架只定义**文档身份与生命周期**
> （`Document`：`typeId/title/isDirty/canClose` + `source()`/`save()`；`DocumentManager`：类型注册（带元数据）、
> **按载荷类型注册的打开器**、`create`/`open`/`close`/`current`、`opened/closed/currentChanged`），进程一份挂在
> `Application::documentManager()`（建在 `initialize()` 里，**早于插件加载** ⇒ 插件能在 `load()` 里注册类型）。
> **打开 = `open(payload)`**：载荷是调用方与类型之间的私有协议（`Vfs+path`/剪贴板图/设备句柄/自定义 struct 都行），
> 登记项是 **`DocumentOpenerRegistration<Payload>` struct**（同 `DocumentTypeRegistration` 的理由：可选字段具名不互串；没有 `open`
> 的登记会被拒 —— 这条检查必须在包装 lambda **之前**做），载荷类型是模板参数所以回调是强类型的；
> 框架**只按载荷的 C++ 类型匹配、不解释内容**：载荷派生 `Object`，键就是 `Payload::desc()`（`registerOpener<Payload>` 的
> `DocumentPayload` 还要求 `&T::desc != &Object::desc`，忘了 meta 就是**编译失败**），而 `open` 只收
> `raw_ptr<const Object>`（**没有模板重载**：模板参数只是把入参静态类型重说一遍，键其实来自运行时类型；代价是载荷得是左值）；
> `open` 的文档还写得明白：**框架不处理拷贝逻辑**（“自己拷”归派生类型、“提前拷”归业务；`open()` 路上零拷贝，用例
> `TheFrameworkDoesNotCopyAPayload` 用带拷贝计数的载荷钉着），**框架不持有载荷对象，但文档可以把它需要的字段拷进自己**
> （要重读就得自己留下 `Vfs*`+path —— `DocumentSource` 只有两个字符串，装不下引用）⇒ **刷新是类型的 `reload()` 动作位，
> 不是“重放 open(payload)”**（看 §5.8）。
> 它与 `EventBus::subscribe<TEvent>` 同一种擦除形状，所以 appfw 只有一套 RTTI。代价：
> **载荷不能 header-only**（要一处 `VN_OBJECT_META_IMPL`）、**不再是聚合体**（指定初始化器没了）、标量得包 struct。
> 挑选顺序 = **具体者优先**（登记类型在 `parent()` 链上离载荷自己的类型最近者先 —— 用 `isKindOf` 匹配，所以**登记基类 = 接受
> 派生，且打开器拿到的是登记的那个类型**，`obj_cast` 在这里才真在干活） → `refine` 分数 > `priority` > 登记顺序 ⇒
> 登记基类就是“兜底”，分数抢不走具体的那个；一次 `open()` 只看到它开始时的登记表（用户代码会在 `refine`/`open`
> 里登记新打开器 ⇒ 候选只有下标、callable 先拷一份）；没人受理就明确返回 nullptr（这是“为什么打不开”的答案）。**位置不是 path 而是 `Vfs + 虚拟路径`**（iobase 已有
> `DirectoryVfs`/`ZipArchive`/`MountVfs`；`robotics::io` 就是 `loadVfs(vfs, path)` 这个形状）。
> **来源（`DocumentSource`）是值类型、不派生**：变化放 Vfs/打开器/类型三处；“没有来源”= 空值；`save()` 无参数写回自己的
> 来源、基类默认拒绝 ⇒ **core 不认识 `Vfs`（appfw 不链 iobase）**。三条契约：`Vfs` 借用且必须比文档活得久、`Vfs` 无锁
> （同一实例不可两线程同碰）、载荷只在 `open()` 期间有效。`create()`/`open()` 发完 `opened` 后要回头确认文档
> 还在集合里（处理函数可以当场关掉它）⇒ 不在就返回空，不返回已销毁的指针。
> **元数据/面板**：`DocumentTypeInfo`（含 owner/icon/can_create/payload_types/source_schemes）+
> `gui::DocumentManagerDialog` + `app_shell` 的 `show_document_types` 命令（**只读**，类型没有启用/禁用开关）；owner 来自
> `Application::registrationOwner()` 这一个标签（`CommandManager::setRegistrationOwner()` 也写它）。
> **视图层（2026-09-27 落地，全在 GUI 里）**：`gui::DocumentView`（`Control` 的薄包装 + `document()` + `content()` +
> `activate()`/`deactivate()`）/ `DocumentViewRegistry`（`type_id → 工厂`，一类型一个，重复/空 id/空工厂都拒）/ `gui::CentralDocumentHost`。
> **视图按文档实例**（同类型两份文档各一个实例，各自的相机/选择；切回来复用同一个，文档关闭才销毁；没显示过的不建），
> **`content()` 是“放进中央区的东西”**（2D 视图返回自己、3D 视图返回那块**共享**渲染控件 ⇒ 接真 3D 时视图层/宿主/注册表一行都不改）。
> 宿主六条契约写在 `CentralDocumentHost.hpp` 类注释里，其中第 6 条是这次唯一的真缺陷：**宿主拥有视图对象、容器拥有页控件**，
> 交给宿主的视图必须 `owns=false` 包控件 —— 否则控件随容器析构会顺着 `UIElement` 的自毁路径把视图对象再删一次
> （double free，`gdb` 里同一个 `UIElement::~UIElement` 两帧；`build-asan` 一跑就抓）。接管中央区时把外壳原来的控件
> **藏起来**（不 delete，退出时还回去 ⇒ 为此给 `DockPanelManager` 加了 `centralWidget()` 读数）。
> **3D 视图的那一页是"借"的**（2026-09-27）：`content()` 是别人的控件时宿主既不接管所有权也不删它，只在该页没人用时
> 从容器里摘下来；几条 3D 文档共用同一页（同一页 `setCurrentWidget` 不重父化），切文档只是换呈现的 view。
> **3D 视图自带渲染面（2026-09-27 用户拍板：一个文档 = 一个视图 = 它自己那份资源）**：视图自己 `new
> RenderControl`（自己的面/会话/设备/管线）铺在自己的控件里，`content()` 返回**视图自己** ⇒ 宿主只有一条页规则（一实例一页）。
> 应用级共享的只有 DockPanel/Ribbon 那套外壳。`VSG_MAX_DEVICES` 取 **4**（vsg 里唯一正确的那一档；`>4` 的动态分支在
> 1.1.16 是坏的：`vk_buffer<std::vector<T>>::size()` 只在写时增长、读会越界，`test_vsg` 当场 abort）—— 所以"同时两个
> device"不再是错误，而上限是 4；**`<=4` 分支不查边界 ⇒ 第 5 块面必须由我们拒（未做）**。
> 原来那套"共享会话"的东西（`RenderControl::present()`、`SurfaceWindow` 的 own_view/view 二分、`SceneView::removeWindowPass()`
> 公开、宿主的"借来的页"规则）**已删**：模型变了就不留无用 API。
> **视图层只在 `GuiApplication`**（视图是 `UIElement`，无头 `Application` 连控件都没有）：注册表与宿主在建主窗时建好，
> 插件在 `load()` 里登记（拿不到 `GuiApplication` 就跳过）。三档状态：**文档**（数据/来源/脏）/ **视图**（按**实例**：相机、选择）/ **宿主·面板·ribbon**（按**事件**或按**类型**：同类型切换不重建结构）。
> **框架不定**：区域数量、标签、单/多文档、展示形态 —— 那些归具体 app（外壳）派生；**一根硬线**：运行时只有一块渲染区
> （`VSG_MAX_DEVICES=1` 是有意绊线，一个后端实例 = 一个 session = 一个原生窗口 + 一个 device）⇒ 3D 文档只能共享一个会话、
> 各带一个 `SceneView`。`DockPanelManager` 的中央区是**单槽**且 `setCentralWidget()` 会**重父化**（Qt 会重建原生窗口 ⇒
> `RenderControl` 走一遍 `Pending -> Attached`）⇒ 中央区放一个容器、只装一次、内部切页（`QStackedWidget`）。
> 见 `.ai/design/appfw-document-model.md`、`tests/test_appfw/DocumentTest.cpp`（23 条钉子）、
> `tests/test_gui/test_gui.cpp` 的 `DocumentManagerDialogTest`。
> **参考实现（2026-09-27 落地）**：`src/plugins/model_viewer/`（**测试插件**，但按真插件的形状写）—— 文档类型 `"model"`
> （没有 create 工厂）+ **两个打开器**（内存网格 `MeshPayload` / 磁盘网格文件 `MeshFilePayload`，后者在打开器里调 `MeshLoader`）
> + 右侧 dock 的**模型信息面板**（类型/标题/**顶点数/三角形数**；header-only 的 `gui::Control`，**自己**听 `currentChanged`，不靠宿主编排）
> + **中央区的模型视图** `ModelRenderView`（**真渲染**：自带的 `RenderControl` + 自己的 `SceneView`；`content()` 返回自己;
> 几何信息归右侧停靠面板 `ModelInfoPanel`）+ 两条命令
> （`open_test_model` / `open_mesh_file`；后者**问用户要一个网格文件**（`UserIO::getFileAsync`，GUI 可以点按钮挑）
> 再让打开器用真加载器读回来）。
> 钉子：`tests/test_gui/ModelViewerTest.cpp`（4 条：空状态、两个数跟着**当前**文档换、非模型文档如实说不适用、
> 插件登记的那个视图确实被宿主放进中央区并显示这份文档的数据）；视图层本身见 `tests/test_gui/DocumentViewTest.cpp`（5 条）；
> 插件在真应用里加载/卸载干净（门禁 app 阶段 `Plugin 'model_viewer' loaded`，warnings=0）。
> **它撞出的两个缺口**：①`MeshLoader::load()` 只吃 `std::filesystem::path`（没有字节/`DataStream`/`Vfs` 重载）
> ⇒ §5 的"位置是 `Vfs + 虚拟路径`"今天**读不进模型**；②**面板注册表不存在** ⇒ 面板只能插件自己建自己挂
> （视图那一半已经落地，面板这一半等注册表出来才变成一条声明）。
> **`Application::shutdown()` 是 protected（2026-09-26 说准）**：只有 `run()` 那条收尾路径调它（派生宿主
> 自己写 `run()` 用同一段）；宿主/命令/插件要停进程用 `exit()`（`cancelStartup()`/`failStartup()` 也走它）。
> 循环还在跑时调、或从非应用线程调 ⇒ 各留一条**只警告不拒绝**的日志（`run()` 的 `loop_running` 位是判据）。
> **`uuid` 是身份（2026-09-26 补齐）**：扫描时三条告警，都只 warning 不拒绝——同名不同 `uuid`（第一个位置胜）/
> 不同名同 `uuid`（两个都留下）/ 注册文件的 `name`·`uuid` 与库不符（**库为准**，只比"指向单个库文件"的注册）。
> **启动可以被取消**（2026-09-26）：`StartupProgress::stopToken()/requestCancel()`；插件从 `PluginLoadContext::stopToken()`
> 拿同一个 token（不用摸全局上报口；不在启动里的插件拿到的是永不停的 token）。框架在每拍之后、每个插件之前
> 看一眼；取消**不是失败**——`Application::cancelStartup()` 卸掉已装插件 + 收上报口 + `exit(0)`，不走 `startupEnd()`
> （主窗不上屏）也不走 `failStartup()`。启动框右上角有个 `✕` 往这个 token 上写。

> **渲染会话可以异步建**（2026-09-26）：`RenderControl::initAsync()`（`Task<bool>`）把会话的 `engine->initialize()`
> （设备/会话/管线）**和 warm-up 那一帧**（`engine->frame()`）都放到池上跑，应用线程回到循环；取句柄、状态、尺寸、
> settle 帧仍留在应用线程。同步 `init()` 行为不变（不跑循环的宿主用它）。
> A/B：应用线程最长连续占用 **414 ms → 189–204 ms → 74 ms**（warm-up 也搬走之后），渲染区像素逐位不变（87.04%）。
> ⚠️ **契约**：attach 读场景图（管线由 pass 建、warm-up 要 record）⇒ 宿主不得在 attach 进行中改场景。已做成结构性保证：
> `RenderControl::isAttaching()`（`AttachScope` 罩住整段 attach）；demo 的装配**在应用线程上再确认一次**再装
> （attach 也是在应用线程上开的 ⇒ "检查+装"之间无挂起点，相对它的开启原子）。
> **进度按相位更新**（用户 2026-09-26 定稿："算了不管卡顿，按相位更新吧"）：不做连续动画/心跳，每个相位开始报一次
> （"正在启动" → "正在显示启动画面" → "正在查找/加载/收尾插件" → "正在准备主窗口"；插件内部相位由插件自己报），
> 框每收到一次上报就同步 `repaint()` 一次。相位**内部**不动画是有意的：应用线程最长连续占用实测 414 ms
> （会话 `init()` 304 ms），要插帧只能切 `init()` 或启动期回转循环，两条都被拒。
> 详细设计按模块拆在 `.ai/design/`：`appfw-command-manager.md`（含 Command/执行链/历史/禁用）、
> `appfw-userio.md`（UserIO/ConsoleUserIO/VisualUserIO）、`appfw-progress.md`（ProgressHost 与两个呈现者）、
> `appfw-render-surface.md`（RenderControl 的表面生命周期）、`appfw-startup-splash.md`（启动框与启动进度）、
> `appfw-eventbus.md`、`appfw-plugin-system.md`、`appfw-config.md`。
> 本文件只放"跨这几篇、干活时必须立刻想起的规则"。

> **SDK 面统一（2026-09-26，ABI 7u）**：插件/宿主交给框架的**文本一律 `vn::String`**
> （`StartupProgress::stage/setLabel/label`、`ProgressHost::setLabel/label/scope`、`AppConfig`/`SplashConfig` 的文本字段）
> ⇒ 字面量写 `u8"…"`（与 `PluginInfo`/`VN_DECLARE_PLUGIN` 一致）；只有 base 的 progress 模块（Qt 无关）用 `std::string`，
> 在 appfw 边界转换一次（`as_std_str()`）。**阶段推进叫 `setDone(double)`**（值一直是绝对值，旧名 `advance` 骗人）；
> **可计数与否只由 `std::optional<double> fraction()` 回答**（`isCounted()` 已删，完成后不再有“残留的 1.0”）。
> **跨线程助手全在 `MainThreadDispatcher` 的静态面上**（`isMainThread/hasEventLoop/postToMainThread/invokeOnMainThread/
> resumeOnMainThread`；实例上只剩 `deliverPostedCalls()` 这个泵——它是 EventBus 手里那个 marshaller 的用法）；
> 实例方法 `postToMain()` 与 `Application::startupProgress()` 都删了（前者是同义词，后者与 `StartupProgress::current()` 重复）。
> **`Application::d` 已私有**（叶子只走 `dptr()`）；`argv()` 返回 `char* const*`。

## 三条横切规则

1. **线程**：命令在它**恢复时所在的线程**上继续（定时器/IO/线程池），所以
   - `UserIO::putString`/`clear`/`cancelPendingInput` 可以被任意线程调用（GUI 实现自己编组，`VisualUserIO`
     用 `onConsolePanel` + `QPointer` 守卫，面板没了就是空操作）；
   - 命令可能在任意线程结束 ⇒ 事件 `executing`/`executed` 的**处理函数**必须自己编组（它们跑在结束线程上）；
     要回到应用线程：命令体里 `co_await MainThreadDispatcher::resumeOnMainThread()`（协程式，无事件循环时不挂起），
     纯回调场景用 `postToMainThread()`；
   - `Signal` **本身已经是线程安全的**（2026-09-17：不可变快照 `vector<Entry>` + 原子发布，`trigger` **不取任何锁**、
     不分配，与 Qt 连接表同构），
     所以"订阅必须放在启动期"这条老限制**已作废**——任何线程都可以随时 `connect`/`disconnect`。
     实测：一边发火一边增删，TSan 从 12 条竞争降到 0；20 个 handler 时发火 104.6 → 25.8 ns；
     订阅+注销 147 ns/对（订阅要复制整表，仍属于装配期动作）。
   - 订阅成员推荐用 **`Connection` 句柄**：`theme_handler_ = app->theme_changed.connect(...)`，析构/赋值自动取消；
     不需要管理时显式 `.detach()`（`connect` 是 `[[nodiscard]]`，丢掉返回值 = 订阅完立刻取消）。
     句柄只持 `weak_ptr`，Signal 先死也安全，所以拆除路径里不再需要 `removeHandler` + `Application::current()` 查找。
     2026-09-18：句柄从 `Signal::Subscription` 外提为独立**非模板**类 `vn::Connection`；入口改名
     `subscribe/unsubscribe/release/unsubscribeAll` → `connect/disconnect/detach/disconnectAll`（对齐 Qt），
     `Signal::Slot` 改为继承 `Connection::State`。
2. **锁内不跑用户代码**：`CommandManager` 的 `mutex`/`registry_mutex`、`Chain::mutex` 里只做容器操作与值拷贝；
   命令虚函数、工厂、快照回调、事件处理函数、`ProgressHost::current()`（progress 全局锁）都在锁外。
   `registry_mutex` 是叶子锁；`admit()` 在临界区**外**采样 progress 宿主。
3. **生命周期是宿主契约**：`~CommandManager` 只告警不阻塞（析构里等协程 = 死锁风险），所以宿主必须先
   `UserIO::cancelPendingInput()`（唤醒停在用户输入上的命令）再 `CommandManager::cancelAllAndWait()`；
   活着的命令帧里存着 `Impl*` 与 `CommandManager*`。`Application::shutdown()` 已经这么做。

## 容易记错的 API 语义

- `isRegistered(name)` = **存在性**（只查注册表：不解析别名、不看 enabled）；`isCommandEnabled(name)` =
  **能不能执行**（解析别名链 + enabled）。禁用是标记而不是移除（`names()`/`commandInfos()` 仍列出）。
- 嵌套必须走 `CommandExecutionContext::executeChild()`：它共享父链的栈与取消源、绕过串联门；
  在命令里调顶层入口 = 新开一条链、会抢前台、`maxChainDepth` 拦不住。
  子命令要么给名字（走注册表建实例），要么**直接传实例** `executeChild(std::unique_ptr<Command>)`
  ——参数由父命令给、无法预注册的那类子命令用后者；实例所有权转到执行帧，子命令结束即析构，
  拒绝规则与顶层按实例入口一致（null / 名称已注册且被禁用 ⇒ Failed）。
- `CommandFlags` 现在支持位运算（`Undoable | LongRunning`），判位用 `vn::testFlag()`。
- **历史不留结果载荷**（2026-09-17）：`CommandHistoryEntry::result` 是 `CommandResult(status, message)`，
  `data()` 恒为空——条目数有上界（1024）时字节数才有上界，否则"最近 1024 次运行的载荷之和"可以到 GB 级；
  载荷属于发起那次执行的调用方。用例 `CommandManager_HistoryDoesNotRetainTheResultPayload` 钉住。
- **用户可见消息用中文，编程错误用英文**（2026-09-17 统一）：门拒绝是
  `另一个操作正在进行中，请稍候。` / `另一个操作仍在收尾，请稍后再试。`，禁用与未注册也是中文；
  `Command is null` 这类保持英文。
- `UserIO::parseInt()` 现在委托 `String::toInt()`（范围规则只有一处实现），它比 `toInt` 多出的唯一契约是
  **失败时不写 `value`**（重提示要保留用户已输入的值）。
- **路径读（2026-10-10）**：`UserIO` 多了四个读 —— `getFileAsync`/`getFilesAsync`/`getDirAsync`/`getDirsAsync`
  （文件与文件夹分开，单个与多个分开；选项是 `UserIO::FileReadOptions`（后缀表 / `require_existing` / `suggested_path`）
  与 `UserIO::DirReadOptions`，三个类型都嵌在 `UserIO` 里 —— 它们只服务这个类的四个读）。
  **字段为什么是这几个**：`extensions` 是**规则**（无头侧没有选择器也照样拒错后缀），标题不属于调用方（框架定，
  所以没有 filter/label 字段）；`require_existing = false` 读的是"另存为"，比 `must_exist = false` 少一层
  "必须不存在"的误读；建议值给的是**路径**而不是文本。不做"只留一个 filter 字段自动解析"：那要 Qt-free 的基类
  去解析 `标题 (模式);;…`（详见 `.ai/design/appfw-userio.md` §1/§6）。
  三个容易记错的点：
  ① **多个路径写在一行里、用 `;` 分隔**（Windows 文件名里 `;` 非法，所以无歧义）—— 不是"逐行输入到空行"，
  于是槽位/取消/行缓冲一行都没改；POSIX 上含 `;` 的路径在多值读里表达不了（单值读整行不切分，可用）。
  ② **无头侧一次读从不重提示**：非法输入打一行原因（`文件不存在: …`/`不是目录: …`/`后缀不支持（仅 .stl）: …`）
  然后 `nullopt`；GUI 侧原地重提示并**把原文留在输入行**里。
  ③ 校验只有一份（`UserIO::checkPath`/`checkPathLine`，protected static）：存在性、类型、后缀（大小写不敏感、
  比文件名尾部 ⇒ 多段后缀也认）、成对引号剥壳；**绝对化/TOCTOU 刻意不做**（检查是建议性的）。
  提示文本由实现拼（`promptWithRequirements`，无头与 GUI 同一串），所以"要什么格式"看得见，不用靠悬浮文案。
- **控制台的选择器（2026-10-10）**：路径读挂起时输入行**内部**长出一个按钮
  （`QLineEdit::addAction(…, TrailingPosition)` + `style()` 标准图标，appfw 零图标资源；`Ctrl+O` 是它的键盘等价物），
  下面一条 2px 下划线（取 `ConsoleTheme::prompt`，**没给 `ConsoleTheme` 加字段**：那是公开结构体，加字段要 bump ABI）。
  **点按钮只是"更快地填那一行"**：路径写进输入行、**不自动提交**（回车才走 `lineEntered` 那条唯一路径），
  多值读两次点选追加成 `a;b`，对话框在 Windows 上给的 `C:/a/b` 会先过 `QDir::toNativeSeparators`。
  对话框是**模态嵌套事件循环**，期间投递的回调照跑 ⇒ 返回后先比"当下还是我开对话框时那个 kind 吗"，
  不是就丢掉结果；读结束时实现下发 `setInputAction({})`，按钮与高亮一起撤（撤下用 `deleteLater`，
  因为点击回调可能正在栈上）。`ConsolePanel` 的**新增状态全在 `Impl`**、一个 `Signal` 都没加 ⇒ 不用动
  `VN_APPFW_PLUGIN_ABI_VERSION`。**拖放没做**（设计留在 `appfw-userio.md` §5.4）。
- **嵌入渲染表面：宿主给时机，控件自维护**（2026-09-19 改）：`RenderControl` 不自己 attach 了 —— 首次
  attach 只有 `init()`（幂等；表面还没布局好就返回 `false`，不猜延时），宿主（app_shell）在
  `new` + `setCentralWidget()` + `demo.install()` 之后立刻调一次。**已建立的会话自己维护**：Qt 重建
  平台窗口（换屏/reparent/拖 dock）后新句柄由控件自己重新公告、自己重新显示，宿主零调用；重建期间
  状态退回 `Pending` 再走 `Attached → Presenting`（`Presenting` 只在真的往可见表面出过帧时成立）。
  表面**由控件持有的容器控制可见性**（容器藏着直到首帧 present，句柄换了也重新藏，`handleDestroyed()`
  只把状态打回 `Pending`）。**2026-09-19 拆分**：会话逻辑全在私有 `SurfaceWindow`（`src/fw/appfw/src/gui/SurfaceWindow.hpp` 与 `.cpp`），
  `RenderControl` 只剩封装（嵌 surface + 转发公开 API，`state_changed` 用 `on_state_changed` 回调中继；
  2026-09-21 信号由 `stateChanged` 改名而来，对齐 `theme_changed`/`name_changed`），
  日志前缀仍是 `[RenderControl]`；公开 API 与用例不变，详见 `.ai/design/appfw-render-surface.md` 的“文件划分”。
  `setAutoInitialize`/重试阶梯/构造里的首触发已删；`Failed` 现在只有“没注册后端插件”一种来源。
  要知道“什么时候才能出画面”就订阅 `state_changed`（`Pending/Attached/Presenting/Failed`），别用
  `QTimer::singleShot` 猜。
- **`SceneView` ↔ `RenderEngine` 的持有关系（2026-10-11 查）**：视图**借用**引擎（`raw_ptr<RenderEngine> engine_`，
  "the host owns it and keeps it alive"），但视图的 `default_pipeline_`（`intrusive_ptr<Pipeline>`）**强引用**引擎
  ⇒ **"默认管线活着 ⇒ 引擎活着"**，所以 `~SceneView()` 里那次 `removeWindowPass()` 即使宿主先释放了引擎也安全
  （`SceneView.cpp` 的注释原话："even if the host released it first"）。引擎**反向没有边**：不认识视图、不认识管线
  （无环）；它只在 `slots_` 里强引用 pass + 内容场景，而 pass 又强引用相机（`setCamera(intrusive_ptr<Camera>)`）
  —— 那是 pass 的属性，不是"引擎知道视图"。`SurfaceWindow::Impl` 同时强引用两者，且 `engine` 声明在 `view` 之前
  ⇒ 析构序 view → engine。**实现约束**：`setOptions()` 重建默认管线必须"**先建新的、再丢旧的**"（`Pipeline::engine_`
  私有且无访问器，视图无法从旧管线借引擎；顺序反了会让 `engine_` 悬空）。详见 `.ai/design/appfw-render-view.md` §13。
- **框架级复核的三条硬结论（2026-10-11，`appfw-render-view.md` §14）**：①**几何只有一个输入** —— `viewport`（归一化）
  × 表面尺寸 × 比例才是矩形真相，`onSurfaceResized` 永远只给**表面**尺寸，宿主不必自己算"一半"；②**HUD 可见性的唯一
  真相是 pass 的 `isEnabled()`**（`Options` 只回答"有没有这个档"），而档属于**默认管线** ⇒ 宿主自建主 pass 时 HUD 开关
  无档可切（记诊断、不静默）；③**多视图的额外视图要显式 `ensureWindowPass()`**（它没有 attach 事件），而"恰好一个视图
  清屏"是后端 **loadOp 整张清**（`OffscreenTarget.cpp:107`）逼出来的约定、且清屏那条 pass 要排最前。**ABI 边界**：
  appfw 公开类加数据成员/`Signal` = bump `VN_APPFW_PLUGIN_ABI_VERSION`（`setInputRouter` 因此放 `Impl`）；graphics 没有
  版本宏，`SceneView` 加成员可行但要写明"插件需重建"。
- **连续渲染（2026-10-11 已落地）**：`RenderControl::setContinuousRendering(bool on, int interval_ms = 16)`
  —— 按需渲染仍是**默认**（attach / resize / 输入 / `renderFrame()` 才出帧），连续是宿主可选的一档（动画、仿真、
  或一个值得读的帧率读数）。它是**帧驱动不是帧率保证**（滴答"要一帧"，一帧该多久多久）。两个"滴答免费"的条件
  **写明**：还没 attach（没有设备可画，首帧归 attach），或原生表面**未 exposed**（藏起来的 dock 页/最小化窗口不该
  继续占 GPU）。计时器挂在 `SurfaceWindow`（`QTimer`，首次使用才建、关掉只是 stop）。用例：
  `RenderControlTest.ContinuousRenderingDrivesFramesWithoutInput`（按需时没人输入就不出帧 → 开着自己出帧 →
  关掉一帧都不再出 → 再开）。
- **输入路由（2026-10-11 已落地）**：`RenderControl::setInputRouter(std::function<SceneView*(int x, int y)>)`
  —— **一个**"这一点归谁"的回调（放 `Impl`，公开面零新数据成员 ⇒ ABI 不动）。语义：**指针按下时路由、松开前捕获**
  （拖拽越界不断线；捕获期间控件持一个强引用 ⇒ 宿主中途丢掉自己的引用也不崩）；滚轮按点路由（为此
  `vn::window::ScrollEvent` 补了 `x`/`y`，逻辑像素、与鼠标同空间）；键盘给"捕获中的 / 自己的"视图；返回 `nullptr`
  = 回退自己的视图；返回**别的引擎**的视图 = 记一条诊断（`UnsupportedRequest`）并回退。未设回调时一切照旧给
  `view()`（单视图逐字不变）。用例：`RenderControlTest.TheInputRouterSendsEachDragToTheViewUnderTheCursor`、
  `ACapturedViewSurvivesTheHostLettingGoOfIt`。
  补丁（2026-10-11）：**捕获是按按钮记的**（`Impl::captured_views[]` + `held_buttons` 位掩码），不是"一个按钮布尔"
  —— 左键拖一个视图、右键同时拖另一个视图是两个拖拽，松开其中一个**不能**结束另一个（被抽掉 release 的那个视图，
  操纵器会卡在"正在拖"，之后**纯悬停**也继续转相机）。释放事件按"它自己那个按钮的捕获"投递（指针漂到别处也还给
  发起者），带按钮的位移给"最近一次按下且还在飞的那个拖拽"，滚轮/无按钮悬停按点路由。钉子：
  `RenderControlTest.ReleasingOneButtonDoesNotEndTheOtherButtonsDrag`（变异验证：把释放改回"清空全部捕获" ⇒ 该用例失败）。
- **渲染视图 v2 已落地（S0–S4，2026-10-11）**：`SceneView::Options{path,gizmo,stats}` + `setOptions/options` + `pipeline()`；
  **几何** `setViewport(NormalizedViewport)`（视图内一处把 {viewport × 表面尺寸 × 比例} 换算成相机宽高比 / 窗口 pass 视口 /
  离屏与 HUD 尺寸 / layout 步尺寸）+ `onSurfaceResized(w,h,ratio)` 重载 + 三参数 `addSurfaceLayout`；**开关**
  `setStatsVisible/setGizmoVisible`（`pass->setEnabled`，零重建；档不在配方里时补建一次并记诊断）；`AxisGizmo/FpsOverlay`
  加 `setAreaOrigin`（多视图里 HUD 落在自己那块）；`Pipeline::resize` 连比例一起推给 HUD；`LightRig::studio()/headlight()`
  + `Scene::addLighting/setLighting`；`SurfaceWindow`：**构造只组装 + 第一拍兜底 attach**、尺寸与比例一起推、
  `ensureWindowPass()` 提到后端检查之前。四个套件全绿（graphics 304 / gui 246 / appfw 32 / meshio 34）。
  细节与两条实现教训见 `.ai/design/appfw-render-view.md` 头部"落地记录"。
- **HUD 的显示 / 隐藏是运行时开关（2026-10-11，已落地）**：`SceneView::Options::gizmo/stats` 只回答"这个视图
  **有没有**这个档"（配方，变更 = 一次重建）；`setStatsVisible()/setGizmoVisible()`（+ `isStatsVisible/isGizmoVisible`）
  回答"现在**看得见吗**"——**零重建**（就是 `RenderPass::setEnabled`；引擎在六处跳过 disabled 的 pass）。档还不存在时
  第一次开启会**补建一次**（每个视图每种档最多一次）⇒ 要频繁切的宿主在配置窗口里把两个档都设 `true`（R1 零重建），
  之后随便切。可见性归**视图**，不进 `RenderControl`，也没有全局开关（多视图各不同）。详见
  `.ai/design/appfw-render-view.md` §4.1。
  补丁（2026-10-11）：**"使用者最后一次说的可见性"必须跟着重建走** —— 重建是从配方建的，而**新 pass 天生
  `enabled`**，所以"关掉读数 → 之后补建 gizmo 档 / 换路径"本来会让关掉的读数自己亮回来。现在 `setGizmoVisible/
  setStatsVisible` 记下请求，`ensureWindowPass()` 末尾用 `applyOverlayIntent()` 落回新建的那几个档上（`isXxxVisible()`
  仍然只读 pass，不存第二份"可见"）。钉子：`test_graphics` 的
  `SceneViewViewportTest.AHiddenOverlayStaysHiddenAcrossARebuild` / `AnOverlayRemovedFromTheRecipeAndAddedBackKeepsTheRequestedVisibility`。
- **多视图 / 多窗口的边界（2026-10-11 查，未落地）**：**多窗口/多 widget = 多个 `RenderControl`，今天就能用**
  —— 一控件一面一引擎一视图，**后端会话每个控件 `factory->create()` 一个 ⇒ 多控件 = 多设备**（同一模型开两窗口
  就上传两遍；`BlockStorage`/`MaterialImages` 归会话）。这是刻意选 Qt/three.js 那条路（隔离、生命周期干净），
  要保的不变量是"公开面里没有进程唯一的假设 + 设备资源归后端会话"，共享设备（一设备多交换链）落在后端 +
  `RenderBackendRegistry`，不碰控件/视图 API。**一个表面里多视图（分屏）底层已经够**：`SceneView` 类注释写明
  "several views may share one engine"，给每个 pass `setViewport`（**设备像素**）+ 恰好一个 `setClearEnabled`，
  自己建的 pass 一呈现该相机，`ensureWindowPass()` 就不插一脚；视口随表面尺寸更新走 `addSurfaceLayout()`。
  （2026-10-11 补的设计，见 `.ai/design/appfw-render-view.md` §12）**缺件变成两件已设计 + 一件是策略**：
  ①输入路由 = `RenderControl::setInputRouter`（**按下时路由、松开前捕获**；默认 = 自己的 `view()`，单视图行为不变；
  放 `Impl` 里的 `std::function` ⇒ ABI 不动）②视口 = `SceneView::setViewport`（**归一化 [0,1]**，与 Unity `Camera.rect` /
  VTK `SetViewport` 同口径；**几何，改它不重建**，S1 落地）③清屏策略仍是宿主的（恰好一个视图清，不进 `Options`）。
  多窗口的**目标态**是"一进程一设备、N 交换链"（Filament/VTK/OSG/Unreal/Unity 口径），现状"一控件一设备"是刻意的
  过渡（命中 ①视图数 > 4、②同一内容多窗口、③设备启动开销被乘上窗口数 之一才做）。
- **初始化时机：构造只组装（2026-10-11 已落地）**：原状是 `SurfaceWindow` 构造末尾 `if (engine->backend() != nullptr) init();`
  （"构造即试一次"），而后端插件（`gfx_backend_vsg`）在启动期就 `registerFactory` ⇒ 真实应用里**构造栈上已经 attach 过**，
  于是 `engine()->setBackend()` 的"Call before initialize()"（`RenderEngine.hpp:64`）不可能再遵守，
  `demo_plugin` 写下的"`install()` 在 attach 之前"（`DemoScene.hpp:124`）也被绕过。现在的契约
  （`.ai/design/appfw-render-view.md` §2.3、`.ai/design/appfw-render-surface.md` 2026-10-11 节）：
  **构造只组装；attach = 宿主显式 `init()`（同步、幂等、优先）或事件循环第一拍兜底（`QTimer::singleShot(0,…)`）**；
  `useDefaultBackend()` 的守卫是"引擎没有后端时才补第一个注册的后端"（晚注册的后端下一次 attach 还能被捡起，宿主自己设的永不被覆盖）；
  `ensureWindowPass()` 提到后端检查**之前**（视图呈现什么是它的配方，不是设备事实）；不显式 `init()` 的宿主要把设备/管线
  那几百毫秒挪到第一拍 —— 仓库内两个宿主都在 `activate()` 里显式 attach（`DemoView` 的 `initAsync()`、`ModelRenderView` 的
  `init()`；`initAsync()` 是 `co_return init()`，**不挂起**，所以两者都在那一拍、在应用线程上跑完，见
  `RenderControlTest.InitAsyncCompletesWithoutSuspending` 与 `DemoView::activate()` 的注释）。
- **没有会话就没有帧（2026-10-11 落地）**：`Presenting` 的含义就是"帧在面里"，也是控件把渲染区域显示出来的依据 ——
  所以 `SurfaceWindow::renderFrame()` 在 `backend_live` 为假（还没 attach，或上一次失败）时直接返回，**不推状态**；
  attach 那一帧才是第一张画面。attach 之前调 `fitToScreen()`（`ModelRenderView::activate()` 里就有这一句）不再能把表面
  推到 `Presenting`（否则宿主要到一个"有画面"的状态，屏上却是一个空洞）。同一条规则连续渲染那边早就写着
  （`setContinuousRendering()` 的注释：没有会话的 tick 是免费的）。钉子：
  `RenderControlTest.AFrameAskedForBeforeAnySessionDoesNotPutTheSurfaceOnScreen`。
- **`failureReason()` 只在 `Failed` 那一格上是答案（2026-10-11 落地）**：attach 成功后清掉旧判决 —— 否则"失败一次、
  后端晚到、attach 成功"之后，宿主还在显示一个早就不成立的失败原因。钉子：
  `RenderControlTest.ReportsFailedWhenNoRenderBackendIsRegistered`（后半段：晚到的后端 attach 成功 ⇒ 原因清空）。
- **渲染后端的初始化可以挪进 `load()`**（2026-09-18）：控件先丢进窗口、再立刻 `init()` ——
  attach 只要求“句柄 + 尺寸 > 0”，而新 QWindow 的退化尺寸（实测 1x1，不是 0x0）就够，真实尺寸随布局
  由 resize/settle 路径补齐。否则设备/管线构建会掉到事件循环第一拍（启动框关掉后那 ~1 s 空屏）。
  框架不该为这事加尺寸策略（曾加的 `RenderControl::setInitialSurfaceSize()` 已删）。
- **主窗口的启动尺寸是显式写下的**（2026-09-25）：`MainWindow` 构造里 `resize(800×600)`（与
  `setMinimumSize` 同值）。不写它的话 Qt 首 show 会**按布局 sizeHint 定尺寸**，而 hint 跟着
  ribbon/dock/日志内容走——实测六次启动六个渲染区（最大 3418×1110，见 §11.16cs/dc）；门禁因此一直
  **自己**先把窗口 resize 到 800×600 再判图（`scripts/vsg_rewrite_gate.sh`），写上之后那一步从“补偿”
  变“确认”。钉子 `MainWindowTest.TheOpeningSizeIsStatedInsteadOfInheritedFromTheLayout`（去掉 resize 行 ⇒ 红）。
- **无头模式已经有进度显示了**（2026-09-18）：`ConsoleUserIO` 构造时挂一个 `ConsoleProgressReporter`，
  订阅 `ProgressHost::changed()` 后按"500ms 后首次出字、最小行距 200ms、百分比变 5% 才重画"出**一行一条**的
  `[进度] 42% 阶段名`，宿主结束后补一行 `[进度] 已结束`；要推自己的节奏就调 `poll()`（不需先 `start()`）。
- **`ProgressHost` 现在属于 appfw**（2026-09-18 从 `src/base/progress` 搬来）：注册表/前台栈/label/变更信号是
  应用状态；base 只留零依赖的 `ProgressIndicator`/`ProgressRange`/`ProgressScope`（`vn::Progress`）。
  插件写 `<vine/appfw/ProgressHost.hpp>` + `vn::appfw::ProgressHost`，`VN_APPFW_PLUGIN_ABI_VERSION` **3u**。
- **进度是推送而不是轮询**：`ProgressHost::changed()` 是进程级 `Signal<>`，在注册/注销/前台栈/label/
  整百分点时发火；位置合流在 `ProgressIndicator::setPositionCallback`（每条目一次的热路径上只多一次比较，
  -O3 实测无代价，2000 万条目 101 次通知）。GUI 呈现器与控制台消费者各自订阅（不再是单观察者回调）。
- **启动框（2026-09-18；统一流程 2026-09-26）**：`AppConfig::splash` 开（`enabled`/`title`/`subtitle`/`logo`），
  框架自己上报 "正在启动 + 逐个插件"，应用插入自己的阶段用 `StartupProgress::current()->stage(u8"正在初始化日志")`（无上报口时是空操作）。
  **宿主只做一件事**：调 `app->run()`（要加自己的启动工作就重写 `startup()` 那一拍，见三拍一节）；框架把活干完就
  做最后一拍 `startupEnd()`（上主窗 + 撤启动框），上报口由驱动在那一拍**之后**收（`endStartupProgress()`）。
  启动期**只有启动框在屏**（`startupStart()` 只上框），
  主窗在 `startupEnd()` 第一次 `show()`；首帧门只等启动框（没框 ⇒ 没什么可等）。
  `Application::run()` 的步骤（两边共用一份）：**`exec()` 先跑** → 队列里的启动步（`startupSequence()`：建上报口 +
  `stage("正在启动")` → `startupStart()`（上框 + 等首帧）→ `startup()`（框架加载插件 + 宿主的活）→ `startupEnd()`）。
  ⚠️ **上报口属于启动阶段，不属于窗口**（2026-09-26 修）：它由 `startupSequence()` 在第一拍之前建、启动收完后销毁
  （取消 `cancelStartup()` / 失败 `failStartup()` 也收）；
  启动框/状态栏只是呈现者 ⇒ **无头宿主也报告自己的启动**（控制台 `[进度] …`）。构造就建是不行的：
  不跑启动的进程会多一个活的前台宿主 ⇒ `isBusy()` 恒真 ⇒ 顶层命令全被拒。
  阶段语义是**阶段内比例**（可计数 `stage(name,total)`+`setDone`；不确定 `stage(name)`），不是全局 ETA。
- 📋 **下一步待办（2026-09-26 用户定的方向，独立文档 `.ai/design/appfw-startup-next.md`）**：
  ✅**①已落地**：`init()` 折进构造函数（`Application(const AppConfig&, argc, argv)`：身份 → managers → Qt 应用对象
  → `initialize()`（UserIO + 配置文件）；GUI 多一步建窗口）⇒ `init()`/`setSplashConfig()` 删、ABI **3u→4u**、
  两个 builder 各剩一句 `make_unique`；**Qt 类型不进 core SDK**（Qt 对象走私有 `ApplicationData::app`，
  `unique_ptr` 持有且声明在最前 ⇒ 最后析构，一个进程因此能接着建下一个宿主）；
  **宿主不能有静态存储期**——静态析构里销毁 Qt 应用对象会调到已不再映射的地址（退出时段错误，用例全绿也一样），
  套件要显式 `TearDownTestSuite()` 释放（见 `.ai/design/appfw-startup-next.md`）；
  ✅**②机制已落地**：`run()` 先 `exec()`，再推 posted 启动步（`startupSequence()`：建上报口 + `stage("正在启动")`
  → `startupStart()` → `startup()` → `startupEnd()`）；插件加载也成了框架内置动作（`AppConfig::load_plugins`）。
  ✅**③已落地（X11 已验）**：启动期只有启动框在屏，主窗在 `startupEnd()` 第一次 show；
  ✅**④X11 半边已验**：句柄来自容器里独立 `QWindow` 的 `winId()`，顶层不必 show（Windows 待验）。
  细节与实测见 `.ai/design/appfw-startup-next.md`。
- ⚠️ ~~窗口一定在宿主的启动工作之前上屏~~（2026-09-26 **已推翻**，X11 实测）：启动期**只有启动框在屏**，
  主窗由 `finishStartup()` 第一次 `show()`。旧结论（“嵌入式渲染表面要用顶层窗口的原生句柄 ⇒ 主窗必须先 show”）
  错在把句柄的归属搞错了：`SurfaceWindow::nativeHandle()` 用的是容器里那个独立 `QWindow` 自己的 `winId()`，
  与顶层是否 show 无关（WSLg/X11：主窗未 show 时 `[VsgHostWindow] attached …` + `Pending -> Attached` 都正常，
  `Attached -> Presenting` 紧随主窗上屏）。历史那次 `GetClientRect(..) failed : 无效的窗口句柄` 来自旧渲染器拿顶层句柄。
  ⚠️ **Windows 侧未验**（判据：启动期日志里有没有 `attached to the host window 0x…` / `surface Pending -> Attached`）；
  万一那边拿不到 HWND，回退是“主窗早 show + 门等两窗”。
- ⚠️ **启动期不要 `processEvents()`**：会顺手跑别的组件的定时器/事件（渲染表面的 resize/settle 更新
  就是这样被提前唤醒的）。启动框只 `repaint()` 自己那一帧。
- ⚠️ **启动期只有启动框要“show 之后派发到它画出第一帧”**（2026-09-26 调整；主窗现在启动期不上屏，见上一条）。
  X11 上 Qt 要先收到服务端的 expose 才把 backing store flush 进窗口，而 expose 只能由事件队列派发送来 ⇒
  不派发时 `repaint()` 是空转，启动框整个启动期全透明（用户实测报的就是这条）。机制：第一拍
  `GuiApplication::startupStart()` 先 `show()` 启动框，再 `co_await AwaitStartupFrame{…}`
  （`Window::first_paint` + 300 ms `QTimer` 兜底；唤醒先 `postToMainThread` 排一拍再 `resume()`，否则就在那个
  窗口的 paint 派发里跑后面的加载 —— 实测 `QWidget::repaint: Recursive repaint detected` + 段错误）。
  契约都在 SDK 里：`Window::hasPainted()`（状态）+ `Window::first_paint`（状态转移，只报一次），实现是
  `WindowData` 里的 `PaintWatcher`（窗口自己 + 已有子控件 + 后加的子控件靠 `ChildAdded`）。
  实测：`startup work starting N ms after the application was asked to run: the startup frame is on screen`。
  测试：`tests/test_gui/WindowPaintTest.cpp` 4 例（含"首帧信号只报一次"）；Qt 自己的 `QSplashScreen::repaint()`
  也是转 `processEvents()`（文档："even when there is no event loop present"）——这是 Qt 级行为，不是 WSL 缺陷。
- ⚠️ **启动框关掉时要把主窗口 `raise()` + `activate()`**：`Qt::SplashScreen` 置顶且不激活进程地显示，
  Windows 的前台激活名额被它占掉，随后 show() 的主窗口就压在终端/IDE 后面（看起来像“没显示出来”）。
  `finishStartup()` 只“确实有框”时做，那一刻会打一行 `main window visible=…, active=…` 供区分。
- ⚠️ **“框关掉时窗口必须已经能画”这条责任 2026-09-20 挪回渲染视图**：框架那套等待
  （`windowCanBeSeen()` / `deferStartupFrameClose()` / `closeStartupFrame()` + 2000 ms 定时器）**已整个删除**
  （`GuiApplicationData` 的两个字段、常量也一并删），`finishStartup()` 无条件关框。取代它的是 `RenderControl`
  的规则：**窗口容器藏着，直到 `state_changed` 报到 `Presenting`** —— 可见的容器会被 Qt 用
  `CompositionMode_Source` + `Qt::TRANSPARENT` 抹成洞（嵌入窗口的洞），隐藏的容器不被 paint，所以那一格是
  主窗口自己的背景；表面自己不再管可见性（`surface_shown`/`setSurfaceShown()`/`handleShown()`/`showEvent()`
  全删，容器 `setAutoFillBackground(true)` 是错的机制、已删）。首帧提前：`initializeBackend()` 里控件不在屏上
  就 `prewarmFrame()`（按当时尺寸渲一帧，付掉设备/管线开销，不发布），上屏后的首帧走就地改尺寸。
  同时删掉 `init()` 里 `singleShot(150/400/900)` 的重试梯子：“表面现在能画了”由容器上屏
  （`eventFilter` 的 `QEvent::Show` 补 `scheduleUpdate()`）/ 容器 resize / SurfaceCreated 三个事件上报。
  实测（Windows + RTX 4060，2026-09-20）：`Pending -> Attached` 在 `load()` 里（2642 ms）→ 预热帧
  `extent 320x320, targets 3, program slots 3, total 142.3 ms` → 69 ms 后关框（2711 ms）→ 392 ms 后
  `Attached -> Presenting`，这一帧 `total 3.8 ms`（旧版 183.6 ms）→ 稳定帧 `extent 752x480, program slots 2,
  total 43.8 ms`。**框到 Presenting：759 ms → 392 ms**。像素证据：品红窗口垫在主窗口背后 + `PrintWindow`
  （与 z 序无关）—— 那一格是主题背景色，`Presenting` 后是画面，品红没露过。契约不变：
  **插件从 `load()` 返回即表示其子系统可用**，`startupEnd()` 仍是“宿主 + 插件的活都干完了”。
- ⚠️ **`ConsoleUserIO` 现带 `VN_APPFW_API`**（类仍私有，头在 `src/`）：`test_gui` 直接构造它抽 stdout，
  不导出就 LNK2019（`6ec0e4d` 起 `test_gui` 一直链不上，2026-09-18 修）。
- 命令不能在自身上 `cancelAllAndWait()`（必定失败，用例钉住）；要退出应用用 `Application::quit()`。

## 命令（`CommandManager.cpp`）

- 入口点/前台链/串联门/Exclusive 接管/取消语义/历史与深度的上界：见设计文档「入口点语义」「关键机制」「不变量」。
- `LongRunning` = 占串联门 + 建 ambient `ProgressHost`；`Exclusive` = 停**所有**活链并等收尾（超时则
  `Failed("另一个操作仍在收尾，请稍后再试。")`），并且两个 Exclusive 之间靠 `exclusive_busy` 串行。
- 等待链收尾一律 5ms 切片轮询 `runs`，不用 `AsyncEvent`（有界等待销毁等待者会踩 async 生命周期契约）。
- **`CommandStatus::NotApplicable`（2026-09-27 新增）**：命令跑到底但“当下没有作用对象”＝**不是失败**。
  `succeeded()` 仍是 `status == Success`（要报错就必须自己看状态）；三处用户可见路径都要分开：`logOutcome()` 记
  **info**（`Failed` 仍 `VN_LOGE`）、`executeDetached()`（ribbon）与 `VisualUserIO` 的控制台回写把解释当**提示**转达，
  **空解释就沉默**（绝不补一句“命令执行失败”）——控制台用 `ConsoleMessageType::Warning`（黄）不是 `Error`（红）。
  不要用 `setCommandEnabled()` 表达“不适用”（那是**用户偏好**，会持久化）。见 `appfw-command-manager.md`「结果状态」。
- **“不适用”由命令自己判（2026-09-27 用户拍板）**：`execute()` 里自己取上下文（如 `documentManager()->current()`
  再看 `typeId()`），不适用就返回 `NotApplicable` 并写清缺了什么。**框架不带“适用文档类型”元数据**
  （不做 `Command::documentTypes()` / `CommandInfo::document_types`，也不按类型过滤 ribbon）⇒ `Command` 虚表不动，
  插件 ABI 不必 7u → 8u。

## 测试

- `tests/test_gui/test_gui.cpp`：`CommandManager_*` 38 例、`UserIOTest.*` 12 例（含工作线程读、
  "点按钮真开对话框"的端到端路径读）、`ConsoleUserIOReadTest.*` 7 例（行/EOF、解析、单飞、跨线程取消、
  三条路径读）；`NotApplicable` 的两条：会话臂与日志级别都断言到。
  这个目标现在链 `vn::Logging`：命令用例要拿 `LogSink` 抓日志行（断言某个状态没被记成 error 级）。
- `tests/test_appfw/HeadlessBootTest.cpp`：8 例（**无头宿主**：只链 `vn::Appfw` + `Qt6::Core`；构造不建上报口、
  启动阶段里有且能用、启动收完后销毁；三拍顺序、取消、失败退出、同步门、uuid 身份告警、shutdown 误用告警）。
  这是 appfw 无头宿主路径唯一的用例集（`test_gui`/`test_vsg` 都只经 builder 造应用、从不跑循环）。
- `tests/test_core/SignalTest.cpp`：16 例（含"发火期间注销/清空"与"并发订阅+发火"）。
- `tests/test_asyncqt/QtAsyncTest.cpp`：2 例（`async::Scheduler` 与 `MainThreadDispatcher::resumeOnMainThread`，
  都用到 appfw 私有头）；同目录原来的 QTimer 版 `async::Sleep` 已删除（与 `vn::async::sleepFor()` 重复且无调用者）。
- `tests/test_progress/ProgressIndicatorTest.cpp`：9 例，只链 `vn::Progress vn::Core`（无 Qt）；
  `ProgressHost` 的 15 例搬到了 `tests/test_gui/ProgressHostTest.cpp`（宿主属于 appfw，测试跟着走）。
- `ConsoleProgressReporter` 的 3 例在 `test_gui`（含一例直接构造私有 `ConsoleUserIO` 抽 stdout 的端到端）。
- `tests/test_gui/RenderControlTest.cpp`：8 例，用假后端（无需 GPU）钉住渲染表面的“宿主给时机 + 已建会话
  自己维护 + 隐藏到绑上为止”状态机（含平台窗口重建后自己跟过去、状态退回 `Pending`）。
- GUI 用例需要 `QT_QPA_PLATFORM=offscreen`；全量 GUI 套件约 8 s。

## 视图把内容交给渲染只有一个接口（2026-09-29，model_viewer 的"打开了却看不到"）

- **不要自己造 `SceneView`**：`RenderControl` 构造时就把 `SurfaceWindow` 建好了，那个面在构造里造出**它唯一的**
  `SceneView` 并 `setEngine`（`SurfaceWindow.cpp` 里的 `d->view`），后端只绑这一个。挂到自己另造的 `SceneView` 上的
  场景，屏幕上什么都不会有（`model_viewer` 原来就是这样：内存载荷与文件载荷都不显示）。
  正确形状：`render_control->view()->scene()->setRoot(root)`（`demo_plugin/src/DemoScene.cpp` 就是这写法）。
- **"一份文档一份相机"不需要自己造 view**：一个文档一个视图 ⇒ 一个视图一个控件 ⇒ 一个控件一个面 ⇒ 一个 view，
  相机与轨道操纵器各是各的。
- `view()` / `engine()` 在控件构造后即可用，不必等 attach；没注册后端时面只是停在 `Failed`（`init()` 返回 false，
  下次 `activate()` 再试），内容场景照旧可读 —— 所以无 GPU 的 `test_gui` 也能把"载荷 → 打开器 → 文档 → 视图 →
  场景 → 几何"整条链走完并断言顶点坐标。
- 与 Qt 同 TU 的坑：`QtCore/qforeach.h` 把 `forever` 定义成宏（`for (;;)`），所以**任何名为 `forever` 的成员/局部
  变量都会让"先包含 Qt 头、再包含它"的 TU 编不过**（`InMemoryCache::Entry::forever` 因此改名 `never_expires`；
  `MeshLoader.hpp` 会把这个 cache 带进来，`test_gui` 里一个 `<QLabel>` 就够触发）。

## 帧率读数由**视图自己**挂（2026-10-10，model_viewer）

- `FpsOverlay` 是自带 HUD pass，但它是 **opt-in**：`SceneView::ensureWindowPass()` 建的默认窗口管线用的是
  `PipelineOptions{}`（`fps.enabled = false`），而 `default_pipeline_` 私有、plugin 够不着 ⇒ **谁想要读数谁自己挂**。
- 查看器的三步接线（`ModelRenderView::attachFpsOverlay()`，与 `FpsOverlay` 文档示例一致）：
  `setPixelRatio(control->devicePixelRatio())` → `view->addSurfaceLayout([fps](w,h){ fps->onSurfaceResized(w,h); })`
  → `engine->addPass(fps, kFpsOverlayOrder)`（`kFpsOverlayOrder` = `pipelineStageOrder(Overlay) + 30`，
  与 `RenderPipelineBuilder::applyOverlays()` 的 `overlay_base + options.fps.order` 同一个算式 ⇒ 与 demo 的读数同层）。
- **尺寸归创建者维护，而且 `addSurfaceLayout()` 只登记回调、不重放**：控件构造后可能还没布局（0×0，`onSurfaceResized`
  只认正数），所以挂的时候先报一次当前尺寸，`activate()` 里再补一次（那一刻控件才真的有了大小）。
- **`activate()` 会重试**（控件没布局好时建场景提前返回、`fpsOverlay()` 可能是空）⇒ `attachFpsOverlay()` 必须先查
  `fps_ != nullptr` 早退，否则每次上屏叠一个读数。
- 宿主想关掉：`engine()->removePass(...)`，或者 `setEnabled(false)`（引擎跳过被禁用的 pass）。
- 钉子：`tests/test_gui/ModelViewerTest.cpp` 的 `TheSceneCarriesItsOwnFrameRateReadout` —— 用"摘得掉"验注册
  （`removePass` 对没注册的 pass 是空操作，所以 pass 数减一才算真的在里面），并钉住 `activate()` 可重复。

## 灯是场景的属性，不是渲染面的（2026-10-10，model_viewer 的"有画面但没光照"）

- 同一条"一份文档一份场景"的直接后果：**灯也必须每份场景自己装**（`Scene::addLight`）。零灯的场景不会黑，而是
  **一片平色** —— 后端在 `!has_ambient` 时只补 `ambient = 0.15`（`gfx_backend_vsg/src/internal/LightBlock.cpp`），
  0 盏方向光 ⇒ 明暗项全为 0、只剩下 albedo×0.15。看起来就是"打开了却没有光照效果"（比"全黑"更像"没装灯"）。
- 正确形状：`view->scene()` 拿到场景后自己装三盏（`ModelRenderView::addLighting`）：**环境光 0.35 + 世界固定的
  主方向光 + 背面弱补光**；`demo_plugin` 的 `addDemoLighting()` 是同一套（它还额外开了阴影）。
- 方向光取**世界固定**方向，不按相机算：否则"明暗"成了视角的函数，同一个模型换个角度就换一套阴影，
  "这里画得对不对"没有可对照的答案（`DemoScene.cpp` 里那段注释的结论，viewer 沿用）。
- 装灯的入口要防重复：`activate()` 会重试建场景（控件还没布局好时 `view()` 为空 ⇒ 建场景提前返回），
  所以按 `scene->hasLights()` 早退，别每次加一遍。
- 钉子：`tests/test_gui/ModelViewerTest.cpp` 的 `TheSceneCarriesItsOwnLights`（恰好一盏环境光 + ≥2 盏方向光，
  且方向不是零向量）。

## 视图设置怎么接到命令 / 菜单上（2026-10-11，model_viewer 的"显示模式"）

- **通道是"宿主发布的那个当前 3D 视图"**：`MainWindow::setPrimaryRenderControl()` / `primaryRenderControl()`。
  谁上屏谁发布 —— 插件在 `activate()` 里发布自己控件的指针，在 `deactivate()` 里**只收回自己的那一个**
  （`ModelRenderView` 与 `DemoView` 同一形状；这是 `DocumentView::deactivate()` 注释里那条"3D 视图要解开它绑的东西"
  的一半，发布而不收回会让窗口留着一个已经下屏的控件）。命令层因此不必认识任何具体视图类。
- **作用是"读-改-写"整份配方**：`view->options()` 取出当前 `SceneView::Options`，只改自己那一项再 `setOptions()`。
  直接送一个只带目标字段的值会把别的设置（帧率读数、坐标轴）关掉 —— 值语义的代价，选项里的每一项都得自己保住。
- **模式归视图、不归文档**：一份文档一个视图 ⇒ 换文档时它以默认档出现；要让"显示模式"跟着文档走，就得把模式存进
  文档（编辑器那一档的事）。
- **命令一道守卫两道**：当前文档不是本插件的类型 ⇒ `NotApplicable`（演示文档的视图不该被查看器的命令改掉）；
  文档开着但视图还没发布（没显示过）⇒ 同样 `NotApplicable`，而且**要说清原因**（不静默）。
- **一条命令一个模式**（`set_shading_lit` … `set_shading_wireframe`），不做"循环切换"：菜单上每条名字就是它的结果。
  按钮不做勾选态 —— 框架今天没有单选组的概念，做了就得自己维护"谁被选中"这份第二真相。
- **登记是显式的**（插件自己在 `postLoad()` 里按名字 `registerCommand<T>()`），不用 `VN_DECLARE_COMMAND` 的自动
  登记：那会把 `moduleCommandQueue()` 的依赖带进每个 include 它的 TU，而可执行文件里没有那个符号（链接错误）。
  名字是头文件里的 `kShadingMode*Command` 常量，插件挂 ribbon 按钮时用的是同一批名字。
- **挂 UI 放 `postLoad()`**：那是"所有插件都 load 完了"的第一个时刻（`Plugin` 的说明），后加载的外壳可能还要往
  ribbon 上加自己的 tab。主窗口更早就有（`GuiApplication::startupStart()` → `createWindows()`）。同名 tab 已存在就
  不再加第二个（插件重载会再走一遍 `postLoad()`）。
- 钉子：`tests/test_gui/ModelViewerTest.cpp` 的 `TheShadingModeCommandsDriveTheCurrentModelView`（五档各跑一遍，
  断言 `options().mode` 变了、`stats` 没被顺手关掉、光栅化覆盖只在线框档留下；下屏后 `primaryRenderControl()`
  回到空、命令如实说 `NotApplicable`）。
