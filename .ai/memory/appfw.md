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
> （`open_test_model` / `open_mesh_file`；后者写一个最小 STL 再读回来，用来看真加载那段）。
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

- `tests/test_gui/test_gui.cpp`：`CommandManager_*` 38 例、`UserIOTest.*` 7 例（含工作线程读、历史去载荷、
  `NotApplicable` 的两条：会话臂与日志级别都断言到）。
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
