# appfw 启动阶段三拍与启动工作线程（2026-09-26）

本文回答两个问题：启动阶段怎么摆才读得懂（`startupStart` / `startup` / `startupEnd`），
以及"启动期主线程被占住、消息不更新"这条现象怎么根治（宿主的启动工作丢到线程池上）。

## 现象与实测拆分

启动期主线程被一段长工作占住时：UI 不刷新（消息循环不跑），窗口不响应，取消点不到。
本机（WSLg/Weston + Xwayland，2026-09-26 探针，已撤）把"冷启动到主窗可画"这段劈开：

| 段 | 耗时 |
|---|---|
| 启动框上屏 → 首帧 | 7 ms |
| 插件库加载 + 实例化（`Plugin '…' loaded` 指的是这一步，三阶段生命周期在其后） | 1 ms |
| `app_shell::load()` 里的 **demo 内容**：两张 cube map（6×256² + 6×512²）读盘 + 解码 + 场景装配 | **1693 ms** |
| `render_control->init()`：attach + 设备/管线 warm-up（`prewarmFrame()`） | **413 ms** |

结论：那"两秒冻结"里约八成是**内容加载（I/O + 解码）**，不是渲染的一次性开销；两者都在
插件 `load()` 里、都在主线程。数字是**热缓存**（当天第二次跑，PNG 在 page cache）；冷启动这一笔只会更大。

顺带确认（回答"能否先初始化好"）：**代码本来就是这么做的**。`SurfaceWindow::initializeBackend()`
在 attach 时就用平台窗口当时的尺寸建引擎（实测 160×160，布局还没跑），控件不在屏上时走
`prewarmFrame()` 把设备与管线的一次性开销付掉，不发布、不上屏；窗口上屏后的第一帧只是就地改尺寸
（`AppShellUi.cpp` 的注释："the size the surface has right now does not matter"）。所以 warm-up
不是"在等实际大小"，把它再挪上工作线程收益仅 0.4 s，而代价是动 vsg 单设备那条无门禁的路径 ⇒ 先不动。

## 三拍

```cpp
class Application {
  public:
    virtual int run();

  protected:
    virtual vn::async::Task<void> startupStart();   // 拍 1（要挂起：等它摆的东西真的上屏）
    virtual vn::async::Task<void> startup();        // 拍 2（框架：插件加载；宿主重写它加自己的活）
    virtual vn::async::Task<void> startupEnd();     // 拍 3（收尾，全同步）
};
```

**框架不替宿主拥有线程**（2026-09-26 定稿）：曾经有个 `run(std::function<void()> work)` 重载，框架把宿主的活丢到线程池
上、`shutdown()` 再有界收尾。它被删了——宿主自己重写 `startup()` 那一拍、自己把同步的重活交给别的线程即可：

```cpp
vn::async::Task<void> MyApp::startup()
{
    co_await Application::startup();          // 框架的部分：插件加载
    co_await vn::async::run(theHeavyWork);    // 挂起这一拍，主循环照转（同库的"把同步函数丢到别的线程"）
}
```

原重载的实体只有测试在用，而"上有界收尾 + 提前退出的契约"（旗子、`2 s` 上限、放弃日志）比那条便利门贵得多。

驱动：`run()` 推进循环的那一步调 `startupSequence()`，顺序只写在这一处，而且**三拍平铺**：

```cpp
vn::async::DetachedTask Application::startupSequence()
{
    beginStartupProgress()->stage("正在启动");
    bool ok = true;
    try {
        co_await startupStart();
        co_await dptr()->main_dispatcher->resumeOnMainThread();   // 每拍之后：下一拍与收尾都在应用线程上
        co_await startup();
        co_await dptr()->main_dispatcher->resumeOnMainThread();
        co_await startupEnd();
        co_await dptr()->main_dispatcher->resumeOnMainThread();
    }
    catch (const std::exception& error) { VN_LOGE("the startup phase failed: {}", error.what()); ok = false; }
    catch (...)                         { VN_LOGE("the startup phase failed with an unknown exception"); ok = false; }

    if (!ok) {
        co_await dptr()->main_dispatcher->resumeOnMainThread();   // catch 里不能 co_await，所以两条路在这里合并
        failStartup();
        co_return;
    }
    endStartupProgress();                // 上报口最后收：startupEnd() 那一拍仍属于这次启动
}
```

它是**成员协程**而不是一个立即调用的 lambda：`DetachedTask` 是急的（`initial_suspend` = `suspend_never`），
所以"调它"就是"开跑"，而协程体长在成员函数里、能直接取那几个受保护的钩子（叶子重写的版本照常被调到）。

三条跟着这次收尾一起定的规则（2026-09-26，用户提问后定稿）：

- **异常不吞，启动失败 = 进程退出**：一个 `try` 罩住三拍，任一拍（含宿主启动工作带上来的异常）的异常记日志后
  返回 false，驱动调 `failStartup()`：收起上报口 + `exit(1)`。`run()` 带着非零码返回，`shutdown()` 照常跑完
  （命令、插件、配置都干净收尾）。**不走 `startupEnd()`**：启动都没成，把半成品的主窗端上来更糟。
  插件的 `loadAll()` 返回 false（"某个插件没装上"）**不在此列**：应用照样启动，只记一条告警。
- **上报口最后收**：它在三拍之后才销毁，所以 `startupEnd()` 那一拍也能往启动进度里报，呈现者到最后一刻才回到
  "跟下一步干什么"。
- **`co_await` 之后的代码跑在唤醒它的那个线程上**：叶子完全可能在定时器线程/IO 线程上结束自己那一拍，所以
  驱动在**每一拍之后**都垫一次 `resumeOnMainThread()`（失败路径也一样）：下一拍与启动收尾都必须在应用线程上。
  已经在主线程时它是空操作，快路径不付代价。（`co_await` 不能写在协程的 catch 处理块里，所以那个垫片只有一处、
  两条路共用。）


**为什么必须用协程才平铺得起来**（2026-09-26 用户提问后定稿的口径）：裸的
`startupStart(); startup(); startupEnd();` 只在"每拍返回即完成"时才对，而第一拍不是——GUI 返回时启动框可能一个像素
都没画，必须等它画出首帧（X11 上只能由循环派发；实测不等的形状就是"整个启动期全透明"的启动框）。而普通函数里
"下一条语句"就是"上一条返回之后"，所以**平铺 + 异步的拍不可兼得**：等待不能用"返回"表达（阻塞会停住正在画框的
循环，抽队列会把别的启动中对象的定时器一并带跑）。协程把"等下一拍"拉直成 `co_await`，三行就是三拍。

把"可等待"这件事放进钩子本身（**钩子返回 `vn::async::Task<void>`**）而不是外部适配器，是这一步的收尾：早先那版
用一个 `AwaitPhase` 适配器（成员指针 + 延续），钩子还是回调形状；现在钩子就是协程，概念只剩一个。

- **上报口由驱动管**（建在拍 1 之前、收在拍 3 之前）：钩子因此纯粹是"这一拍做什么"，叶子忘了调基类也不会把上报口
  漏在栈上。
- **叶子契约**：叶子可以任意线程结束自己那一拍（定时器、IO），框架每拍之后会拉回应用线程 - 但叶子在**自己那一拍
  内部**碰 UI 前仍得先 `MainThreadDispatcher::resumeOnMainThread()`。宿主丢到自己线程上的活也一样，"回到主线程"是它
  自己在那一拍里的责任（`co_await` 那个工作之后，框架的垫片照旧把下一拍拉回应用线程）。
- `DetachedTask`：启动只跑一次，没人要它的返回值；帧在最后一行之后自毁。极端情况（宿主让循环先退出、启动被放弃）
  下没人唤醒它，那一帧跟着进程结束（见"生命周期与退出"）。
- 一拍抛异常：异常存在 `Task` 的 promise 里、在驱动的 `co_await` 处重抛，驱动接住它 → 记日志 →
  `failStartup()`（收上报口 + `exit(1)`），不把应用卡在"上报口还开着、主窗永远不上屏"，也不留下一个半启动的应用。
- **懒任务**：`Task` 挂起在第一行之前 ⇒ "调钩子"≠"跑钩子"，必须 `co_await`（`[[nodiscard]]` 会把漏写变成警告）。
  不跑循环的叶子（测试、工具）用 `Task::result()` 驱动它——只在那几拍**不会真挂起**时安全
  （`GuiApplication::startupEnd()` 就是全同步的那一拍）。

**访问控制**：三拍都是 **protected**（`startupEnd()` 原来是 public）：收尾是框架的动作，宿主不从外面插手到一次
正在跑的启动里去；叶子重写它，需要自己驱动启动的叶子从自己的类里调它。`test_gui` 的共享应用就是
`TestGuiApplication`（`tests/test_gui/fixtures/`），用 `using` 把最后一拍提上来并用 `Task::result()` 驱动。
**槽位**：三拍声明为**一组、放在类末尾**（新虚函数追加在末尾的既有规矩），签名同时变了 ⇒
`VN_APPFW_PLUGIN_ABI_VERSION` 抬到 **5u**。

**唯一的时间轴契约：每一拍都在主线程结束**（框架的在 `startup()` 里替宿主保证，叶子的靠它自己
`resumeOnMainThread()`），于是下一拍也在主线程开始。"执行可以换线程、顺序由主线程控"就是这个意思。

| 拍 | 框架的部分 | 叶子（`GuiApplication`）的部分 |
|---|---|---|
| `startupStart` | 无（上报口由驱动建） | 报"正在显示启动画面"，上启动框，`co_await` 它画出首帧（`AwaitStartupFrame`：`first_paint` + 300 ms 兜底，唤醒排一拍） |
| `startup` | 插件 `preLoad/load/postLoad`（应用线程；`loadAllAsync()` 逐相位上报） | 不重写；要加自己的启动工作就重写它（先 `co_await Application::startup()`，重活再 `co_await vn::async::run(...)`） |
| `startupEnd` | 无（上报口由驱动收） | 报"正在准备主窗口"，主窗第一次上屏、收启动框、`raise()`/`activate()`（全同步，无挂起点） |

读法：

```cpp
vn::async::Task<void> GuiApplication::startupStart()
{
    frame->show();
    co_await AwaitStartupFrame{ qt_app, frame };       // 首帧或 300 ms 兜底（唤醒排一拍）
    /* 证据日志：startup work starting N ms ... the startup frame is on screen */
}

vn::async::Task<void> GuiApplication::startupEnd()
{
    /* 主窗上屏、收框、bring to front */
    co_return;
}
```

三拍**不是对称的 pre/post 对**：拍 1 必须挂起（"等框上屏"无法用返回表达：阻塞会停住那个正在画框的循环，而抽队列
（`processEvents()`）会连带跑起别的启动中对象的定时器），拍 3 没有可等的点（全同步）。这条在 `Application.hpp`
的注释里同时保留。

**`PluginManager` 的两道门与"管理器不转循环"**（2026-09-26 定稿，`VN_APPFW_PLUGIN_ABI_VERSION` 6u）：早先那版
"`loadAll()` 保持同步"被撤了。钩子 `preLoad()/load()`/`postLoad()` 现在是 `Task<void>`，于是插件自己就能在钩子里
**跳池**（`co_await vn::async::run(...)`）→ 用 `resumeOnMainThread()` 回来，界面那一半仍在应用线程上。
管理器因此有两道门，做的是**同一件事**（同一份扫描、同一个依赖闭包、同一个生命周期顺序），差别只有一个：
**等的时候线程在谁手里**。实测（同一条二进制，只换启动走哪道门，`VINE_BOOT_TIMING=1`）：

| 启动走哪道门 | 应用线程最长连续占用 | Attached | 首个 Presenting |
|---|---|---|---|
| `loadAllAsync()`（启动用的） | **73 ms** | 149 ms | 363 ms |
| `loadAll()`（同步门） | **450 ms** | 153 ms | 410 ms |

同步门**能跑通**（它轮询时派发已投递的调用，钩子"回应用线程"那一步因此不死锁——见下），但它"等"的方式就是
**阻塞**：即便插件把活丢到池上，应用线程照样被按住整段（450 ms 里大部分是池上的时间换了个地方被阻塞）。
所以两道门的分工是：**`loadAllAsync()` = 启动（有循环）；`loadAll()` = 没有循环的调用方**（测试、工具、自驱宿主）。
门禁守着这条：应用阶段要求 <= 150 ms（见"门上守着它"），把启动改回同步门会直接红。

- `loadAllAsync()`：启动用的那道，是协程。管理器**自己不转循环**（不抽队列、不放 0 ms 定时器）：那会把别人的定时器
  一并带跑——内嵌渲染面自带 attach 退避定时器，让它在本机窗口还没布局好之前跑起来，就会拿"没有原生句柄的窗口"去建
  交换链。
- `loadAll()`（同步门）：给不跑循环的工具/测试/宿主用。它是 `loadAllAsync()` 外面包的一层中继 + 轮询
  （`relayToSyncDoor()` + `waitForSyncDoor()`），等的时候**只派发已投递的调用**
  （`deliverPostedCalls()` → 只跑 `QEvent::MetaCall`）。**不能用 `Task::result()`**：它只阻塞、不派发，
  而钩子"回到应用线程"那一步正是一条已投递的调用 ⇒ 死锁（用例
  `TheSynchronousDoorLoadsAPluginThatComesBackToTheApplicationThread` 就是钉这件事的：改用 `Task::result()` 会挂死）。

钩子必须**每条出口都写 `co_return;`**（哪怕它一件事都不做）：缺了它函数不是协程，返回的是个空 `Task`（无帧），
`co_await` 它必然 `ud2`。这条写进了 `Plugin.hpp` 的钩子文档。

**进度按相位更新**（2026-09-26 定稿，用户："算了不管卡顿，按相位更新吧"）：启动期的进度**不做**连续动画（不做心跳、
不按时间插值、不为它切 223 ms 的会话 `init()`）。做法是**每个相位开始时报一次**，而帧每收到一次上报就同步重画一次：

| 相位 | 谁报的 | 文字 |
|---|---|---|
| 拍 0（驱动） | `startupSequence()` | "正在启动" |
| `startupStart` | `GuiApplication` | "正在显示启动画面"（上框之后、等首帧之前） |
| `startup` | `PluginManager` | "正在查找插件" → "正在加载插件"（计数，总量 = 插件数）→ "正在收尾插件" |
| ↳ 单个插件内部 | 插件自己（demo：`app_shell`） | "正在准备功能栏" / "正在准备面板" / "正在建立渲染会话" / "正在接通控制台" |
| `startupEnd` | `GuiApplication` | "正在准备主窗口"（主窗上屏、撤框之前） |

- **相位边界一定看得见**：每个相位名都跟着一次 `ProgressHost` 通知，`BootSplash::onStartupChanged()` 在应用线程上
  立刻 `refresh() + root->repaint()`（该函数的注释写明**有意不抽队列**：抽队列会把别的启动中对象的定时器带跑）。
- **相位内部画面不动，这是有意接受的**：应用线程上最长一段连续占用实测 **414 ms**（其中"建立渲染会话" `init()` 304 ms、
  面板 49 ms、功能栏 3 ms）；要给这段时间加上动画，只能把 304 ms 的 `init()` 切开（attach 要原生窗口，动不得）或让启动期
  回转循环（本文件与 `PluginManager::loadAllAsync()` 都写明拒绝）——两条都比"相位边界可见"贵，收益只是相位内的帧数。
- 钉子：`test_gui/BootSplashTest` 里 `splash.statusText()` 断言框**画出来的字**跟着 `stage()/setLabel()` 变
  （机制被钉住）；相位**名字**不属于契约，改文案不会红。


## 线程契约

| 允许在非主线程 | 必须在主线程 |
|---|---|
| 宿主的启动工作：纯计算、文件/数据库/网络 IO、解码、预处理 | 插件 `preLoad/load/postLoad`（`load()` 建控件、视图、原生窗口） |
| 通过 `StartupProgress` 上报（跨线程路径已具备，见下） | 原生窗口与渲染表面的创建/挂接、`startupStart`/`startupEnd` 的全部内容 |
| `MainThreadDispatcher::postToMainThread()` 回主线程 | `QCoreApplication::exit()`（`Application::exit()` 会自己改道） |

- **宿主的工作怎么上线程**：由宿主自己决定——重写 `startup()` 那一拍，`co_await vn::async::run(work)` 就是库为
  "一个同步函数该在别的线程上跑"提供的那一件（`ThreadPoolScheduler::schedule()` 跳到 `ThreadPool::defaultPool()` 的
  工作线程上再调 `work`）。框架因此**自己不拥有线程**：没有 `std::thread`、没有 join/detach、没有握手/旗子，异常也不
  需要手工搬运（`Task` 的 promise 存下来、在 `co_await` 处重抛给那一拍）。宿主的手工线程（`std::jthread`、自管池）
  随时仍然可以写在自己的 `startup()` 里。

- **回主线程**：`MainThreadDispatcher::postToMainThread(task)` 是新的静态入口——工作线程不能用
  实例方法，因为它属于应用、随应用销毁（悬垂）。循环已停时它返回 false，任务被丢弃，这正是
  迟到回调的天然护栏。
- **上报跨线程已经通了**：`ProgressHost` 的更新有互斥保护，`current()` 读的是**全局前台栈**
  （所以工作线程上报时主线程的 `isBusy()` 照样为真、命令照样被挡，与旧形状同义）；
  `BootSplash::onStartupChanged` 本来就判线程，跨线程时 `QMetaObject::invokeMethod` 排队投递。
- **取消**：`ProgressHost` 带 `std::stop_source`，宿主的工作可在工作线程里自行观察取消请求。
- **工作线程要主线程的结果**：`postToMainThread` + 等 future（主线程是自由的，不会死锁）。
  反向**禁止**：主线程永不阻塞等 worker，否则循环又停了，本设计就白做了。

## 生命周期与退出

- **异常**：宿主自己那一拍里抛的异常（包括它从池上重抛回来的）由驱动那个 `try` 接住 → 记日志 → `failStartup()`：
  启动期的异常是致命的，得由启动序列统一处理，而不是让一个半启动的应用继续跑。
- **没有框架级的收尾等待**：重载删掉后，`shutdown()` 不再等任何启动工作（它只管命令、插件、事件总线、配置）。宿主自己
  丢出去的后台活活得比循环久，是宿主自己的契约：`shutdown()` 之后不要再碰框架对象。
- **不悬垂（性质与叶子同一条）**：没有循环在抽队列时，那一拍唤醒后的第一件事是 `resumeOnMainThread()`，它只投递、不执行
  ⇒ 帧挂在 `co_await` 上，启动停在那里（不进 `startupEnd()`，不会从别的线程去摸窗口）。**窄窗口**：若某一拍（叶子的定时器
  那一拍、或宿主自己丢出去、比循环活得久的活）是在 `ApplicationData` 析构之后才结束的，唤醒它的线程会去读那已销毁的对象
  （`dptr()`）。这是这条设计的固有性质：框架不拥有那些线程，也就无法阻止它们回家。
- **`exit()` 加固**：非主线程调用时改为 `postToMainThread`，工作线程里的宿主不必知道这条规矩。

## 门禁与钉子（都能红）

1. 宿主自己那一拍里交给池的工作的线程 id ≠ 主线程 id（用例 `WorkingHostApplication` 就是宿主写法本身；变异：内联在主线程跑 ⇒ 红）。
2. **工作期间循环真的在转**：主线程上每 10 ms 一跳的定时器，工作里睡 120 ms，断言到点前 ≥5 跳
   （变异：主线程阻塞等 worker ⇒ 红）。这条直接对应"消息不更新"。
3. 顺序：插件先加载完 → 工作线程开跑；启动收尾在工作完成之后。
4. 工作线程里的 `stage()/advance()` 到得了上报口与呈现者。
5. 启动工作抛异常：**进程以非零码退出**（`run()` 返回 1），上报口已收、没有半启动状态（用例
   `AFailingStartupWorkEndsTheProcess`）。第 1–5 条现在量的是**宿主那一拍的写法**（用例的夹具就是范例），不再是框架的机制。
6. **没有任何一拍可以占住循环**（2026-09-26 加，比第 2 条更强）：测试叶子在自己的第一拍里
   `co_await sleepFor(150 ms)`，主线程定时器记"最大间隔"，断言 `max_gap < 100 ms`；变异（那一拍改成
   忙等 150 ms）⇒ `max_gap = 150` 红。这只叶子**故意在定时器线程上结束、不自己回主线程**，所以它同时钉住：
   框架每拍之后的那次 `resumeOnMainThread()` 真的生效（第二拍进来时断言 `isMainThread()`）。
7. **同步委托（`MainThreadDispatcher::invokeOnMainThread()`）的两条性质**（2026-09-26 加，在用例的宿主工作里量）：
   ①被委托的代码在**应用线程**上跑；②**调用返回时它已经跑完**——委托那一段故意睡 60 ms，调用方量自己身上花的时间并断言
   `>= 50 ms`（变异：只排队、不等 ⇒ 立刻返回 ⇒ 红）。另有"已在应用线程时内联执行"一条（排队等自己会死锁）。

## 异步装配：重活换线程、界面留在主线程（2026-09-26 落地）

**实测把 1768 ms 的 `AppShellDemo::install()` 劈开**：`loadDemoCubeMap(256)` 885 ms + `loadDemoCubeMap(512)` 880 ms
（十二张 2048² JPEG 的读盘 + 解码 + `boxFilterRgba` 降采样），而**其余全部合计只有 3 ms**（场景图、诊断 pass、管线）；
`render_control->init()`（会话 attach + 设备/管线）另计 223 ms。结论：能上别的线程的只有**纯数据那一大块**，要同步装配的
几乎为零。

形状（**插件自己包**，框架不管插件的线程；`deferStartup` 那一版做过又删了，见"落地顺序"第 2 条）：

```cpp
// AppShellDemo：资产 = 纯数据；骨架 = 图形对象
vn::async::Task<DemoCubeImages> loadDemoCubeImagesAsync();      // 十二个面各一个池任务（whenAll 并行，本机 16 核）
void AppShellDemo::install();                                   // 骨架：场景根/相机/pass/管线（不读资产）
void AppShellDemo::installContent(const DemoCubeImages&);       // 建 CubeMap + 挂 env_box / sky_box（微秒级）
void assembleDemoContentLater(std::shared_ptr<AppShellDemo>);   // 自己包的协程（急启动的 DetachedTask）
```

```cpp
// 插件 load()（应用线程）：Ribbon → 停靠布局 → 骨架 → **把资产丢出去** → 会话 init()（223 ms，与它们重叠）
// assembleDemoContentLater() 里：
//     auto images = co_await loadDemoCubeImagesAsync();          // 池上并行；
//     co_await app->mainThreadDispatcher()->resumeOnMainThread();// 让**应用线程唤醒这次 await**
//     demo->installContent(images);                              // 于是这一行就在应用线程上跑
```

实测（本机、热缓存）：启动步上屏 7 ms → 会话 attach 90 ms → **主窗上屏 339 ms**（改前 2027 ms，≈6×），
门禁 app 阶段的像素**逐位不变**（`87.04%` / `85.14%`，preview 244）。

三条把这件事做对的规则，其中两条是实测撞出来的：

- **换线程用 `co_await resumeOnMainThread()`（让 UI 线程唤醒 await）**：`co_await` 之后的代码天然跑在应用线程上
  （CubeMap、场景节点都建在那里），而挂起期间池的 worker 是**自由的**。`MainThreadDispatcher::invokeOnMainThread()`
  （同步委托：worker 等它跑完）留给"任务要把自己的线程留着、只借 UI 线程做一段"的情形——两者语义不同，别混用：
  前者把**尾巴**搬过去，后者只**借一段**。
- **渲染会话（attach/init）必须贴着主窗上屏建**：把它提前到插件 `load()` 开头（早 ~465 ms）时，渲染面的原生窗口
  **一直不被 map**（主窗 viewable、面自己 unmapped）⇒ 门禁读到 `not viewable`。旧序里 attach 与上屏只差 50 ms；
  现在资产放在骨架之后、`init()` 之前，两者重叠，既快又保持贴近。
- **插件 `.so` 的符号可见性**：声明在头里、被别的 TU 调用的函数**不能定义在匿名 namespace 里**（内部链接，同一个
  `.so` 跨 TU 也找不到 ⇒ 运行期 `undefined symbol`，编译期完全看不出来）。
- 界面本身（Ribbon、停靠面板、控件、图形对象）自始至终在应用线程上：换线程的只有纯数据那一块。

## 取消（token）与启动框上的关闭按钮（2026-09-26 落地）

取消的东西早就在了：每个 `ProgressHost` 自带 `std::stop_source`（状态栏那个 Cancel 按钮就是 `request_stop()`）——
**缺的是有人看它**。现在这条链路接通了：

- `StartupProgress::stopToken()` / `requestCancel()`：把这次启动的 token 摆到台面上（哨声来自哪里都一样：启动框上的
  按钮、状态栏的 Cancel、宿主自己调）。
- **谁看它**：`Application::startupSequence()` 在**每拍之后**看一眼；`PluginManager::loadAllAsync()` 在**每个插件开始前**
  看一眼（看到就卸掉本批已装的、提前收工）。插件自己也可以看：**`PluginLoadContext::stopToken()`**（2026-09-26 加，
  插件不必去摸全局上报口），它把长活丢到池上时可以传给那一头；没在启动里装的插件（测试、工具）拿到的是**永不停**的 token。
  钉子：`test_appfw` 的 `ACancelledBootEndsCleanlyWithoutFinishingTheBoot` 里，夹具在 `load()` 里请求取消并**记下**
  "从 context 看不看得到"（落到配置项），用例读它——⚠️ **在钩子里抛异常是看不见的**（管理器会把它收成
  `loadAllAsync()` 返回 false，而取消本身也让它返回 false ⇒ 观测不到差别）。
- **收场语义：取消不是失败**——`Application::cancelStartup()`：记日志 → `unloadAll()`（插件在 `load()` 里建的东西不留在
  进程里）→ 收起上报口 → `exit(0)`。**不走 `startupEnd()`**（主窗不上屏），也**不走 `failStartup()`**（不是非零退出）。
  取消时 `startup()` 里那句"Some plugins failed to load"也被拦下来了（管理器提前收工也返回 false，别吓人）。
- **启动框上不提供关闭**（2026-09-26 决定，用户："去掉加载时关闭，暂时不允许关闭"）：框上没有取消按钮，
  `SplashWindow::closeEvent()` 也拒掉窗口系统送来的关闭请求（启动的脸是框架撤的——`startupEnd()` 析构它；
  用户提前把它关掉只会得到一段空屏幕）。**取消的机器保留**（`StartupProgress::requestCancel()` → 宿主的 stop
  token，宿主/工具/无头场景照用），只是启动期不把它接到界面上。钉子：`test_gui` 的
  `TheFrameRefusesToCloseWhileTheBootRuns`（框上无按钮 + 关闭请求被拒）。
- 钉子（都验过会红）：`test_gui` 的 `TheCloseButtonAsksTheBootToStop`（点一下 ⇒ token 置位）、
  `TheCloseButtonDoesNothingWithoutABoot`；`test_appfw` 的 `ACancelledBootEndsCleanlyWithoutFinishingTheBoot`（夹具在
  `load()` 里请求取消 ⇒ `run()` 返回 **0**、最后一拍没跑、插件已卸、上报口已收）。变异（把两处 `startupCancelled()`
  检查改成 `false`）⇒ 那条用例**挂死**（没人停循环）⇒ 它自带 3 s 兜底，回归时以失败收场而不是卡死。

## 门上守着它：应用线程最长连续占用（2026-09-26 加）

那个“414 → 74 ms”的数字以前只在提交信息里。现在它是**门禁的一行**：demo 在 `VINE_BOOT_TIMING=1` 下把启动期
应用线程**最长一次连续占用**打到 stderr（10 ms 定时器量间隔），`scripts/vsg_rewrite_gate.sh` 的应用阶段读它并要求
`<= VINE_GATE_BOOT_OCCUPANCY_MS`（默认 150 ms，约是实测的两倍，免得机器一忙就假红）。
把某一段搬回应用线程 ⇒ 这里直接红（反证：用 `VINE_GATE_BOOT_OCCUPANCY_MS=1` 跑同一道门 ⇒
`[FAIL] app (deferred demo) … the boot held the application thread for 69 ms, over the 1 ms allowed`）。
日志里没有这一行也算红（探针被删掉不能被静默放过）。

## 渲染会话 init 的一半搬到池上（2026-09-26 落地，**A/B 量过**）

结论先给：**会话 init 只是"要句柄"，不是"要应用线程"**——设备/会话/管线建在池上完全成立，像素逐位不变。

- 形状：`SurfaceWindow::initializeBackend()` 劈成 `prepareBackendAttach()`（应用线程：还句柄、`setWindowHandle`）→
  `engine->initialize()`（**任意线程**）→ `finishBackendAttach()`（应用线程：状态、尺寸、`onSurfaceResized`、warm-up、
  settle）。新增 `RenderControl::initAsync()`（`vn::async::Task<bool>`）把中间那一段走 `vn::async::run()` +
  `resumeOnMainThread()`；同步的 `init()` 一字不改（不跑循环的宿主照用）。demo 插件改成 `co_await …initAsync()`。
- 先做过**可行性**实验（`engine->initialize()` 整段丢到 `std::thread` + join）：`ok=1`，`Pending -> Attached` 正常，
  渲染区 `87.04%`、无 `vuid`/validation —— 所以后续的搬移不是赌博。
- A/B（同一条二进制，`VINE_BOOT_SYNC_INIT=1` 走老路；10 ms 定时器量应用线程最长连续占用）：

| | 应用线程最长连续占用 | 启动期空档 | attach | 首个 Presenting | 渲染区像素 |
|---|---|---|---|---|---|
| 旧（全同步） | **364 ms** | 一整段 364 | 111 ms | 326 ms | 87.04% |
| 新（initAsync） | **189–204 ms** | 68 / 189 / 68 | 136 ms | 368 ms | 87.04%（逐位同） |

- 归属（临时探针，已撤）：**池上 115 ms**（设备/会话/管线）+ 应用线程尾 **184 ms**（= warm-up 那一帧）。

### 第二步：warm-up 那一帧也搬到池上（2026-09-26 同日）

`prewarmFrame()` 就是一句 `engine->frame()`，而它要的也只是句柄与已建好的会话 ⇒ `initAsync()` 里它同样走
`vn::async::run()`；不是 `initAsync()` 的调用方（同步 `init()`、控制台已经在屏上时）照旧在应用线程上付这笔钱
（`finishBackendAttach(bool warm_up_here)`）。

| 应用线程最长连续占用 | |
|---|---|
| 原始（全部同步） | **414 ms**（一整段） |
| 设备/会话/管线搬到池上 | **189–204 ms**（68 / 189 / 68） |
| warm-up 也搬到池上 | **74 ms**（74 / 22 / 64，四段短账） |

渲染区像素逐位不变（`87.04%`，两棵树门禁同上），`Attached -> Presenting` ~370–385 ms，无 `vuid`/validation。

- ⚠️ **它带一条契约**（写进了 `RenderControl::initAsync()` 的文档）：attach 会**读场景图**（管线由已注册的 pass 建，
  warm-up 帧还要 record 它）⇒ **宿主不得在 attach 进行中改场景**；内容装配要等它返回再动手。
  **已做成结构性保证（2026-09-26 同日）**：`SurfaceWindow` 用 `AttachScope` 把整段 attach（含 warm-up）标在
  `std::atomic<bool> attaching` 上，`RenderControl::isAttaching()` 公开；demo 的装配协程**两处**等它——
  池上先等一次，**应用线程上再确认一次**再 `installContent()`（attach 也是在应用线程上开的 ⇒ "检查 + 装"之间没有挂起点，
  相对 attach 的开启是原子的；只靠池上那次不够，attach 可能在那之后开起来）。
  钉子：`test_gui/RenderControlTest.InitAsyncAttachesOffTheApplicationThreadAndPublishesWhenAttaching`。
- 顺带一条：`prewarmFrame()` 自己的注释早就写着这笔开销（“measured 183.6 ms for the demo's 5 slots”），
  实测与它相符（166–184 ms）。

- 代价：首个 Presenting 晚 ~40 ms（多一次池往返 + 回主线程的投递），换来启动期多了两个 68 ms 的可响应窗口。
- 门禁：两棵树 `ninja` 0 warning；`ctest` 24/24 ×2；两棵树门禁 8/8 `[ok]`（app 像素 `87.04%` / `85.14%` 逐位不变）；
  `cmake --install` + 真机跑无错。

## 本设计不解决

- **warm-up 那一帧仍在应用线程**（实测 184 ms）：设备/会话/管线已经搬到池上（见上一节），但 `prewarmFrame()` 那一帧
  （建 pass 图、program slot、编译管线）还是应用线程上最长的一段。要动它必须先解决它与 `installContent()`（内容装配
  回来时在应用线程建 CubeMap、挂场景节点）的并发——不是"丢线程"三个字能解决的，单独立项。
- **"启动驱动搬到新线程 + 取消令牌 + 启动框关闭按钮 + 动画"**（2026-09-26 评估过，**用户当场决定不做**：
  "算了先不管那么多，卡顿就卡顿吧"）。评估结论留档，免得下次重新推导：
  - 驱动单独搬线程**买不到动画**：应用线程占用 414 ms 里 **356 ms 是插件里必须在应用线程的块**（`init()` 304），
    驱动自身的账（扫描/dlopen/实例化）只有 **12–16 ms**；而且 `BootSplash` **没有重画源**（它的注释写明"启动期不回
    到循环，所以上报时当场 `repaint()`"），所以没有循环回转 + 定时器，动画只能停在上报那一帧。
  - 真要做，四块缺一不可：①`MainThreadDispatcher` 要新加一个**借用 awaitable**（现有的 `invokeOnMainThread()` 是
    同步借用：functor 一返回就算完，驱动不了会挂起的协程）；②驱动搬到 boot 线程（`shutdown()` 有界收尾）⇒ 块与块
    之间应用线程才真的空着；③取消链路：token 已经在（`ProgressHost::cancelSource()`，状态栏 Cancel 按钮已经在
    `request_stop()`），缺的是**有人看它**（驱动/`loadAllAsync()`/`PluginLoadContext::stopToken()`）+ 取消语义 +
    回滚；④呈现：框上加关闭按钮 + 动画重画源，并且 **G1 切 `init()`** 才是动画连续的那一半（⚠️ attach 必须贴着主窗
    上屏，提前 465 ms 实测渲染面不被 map）。

- **`BootSplash` 的同步 `repaint()`**：循环现在一直转，那段"宿主正忙、不会回到循环"的理由消失了
  （可改回随循环刷新）；本次先只改注释，不动渲染行为，避免把两件事混在一个改动里。
- Wayland 原生会话下"不跑循环就画不出窗口"这条依旧成立（`repaint()` 在 Wayland 上本就做不到），
  本设计只是让启动期不再出现"没有循环在转"的时段。

## 落地顺序

1. ~~三拍重命名与虚函数（同步形状，行为不变）~~（2026-09-26 完成）。
2. ~~宿主的启动工作上工作线程（握手 + 有界收尾 + `exit()` 加固）~~（完成，**后又撤掉**：`run(work)` 重载直接删了——
   只有测试在用，而"旗子 + 有界等待"比那条便利门贵；宿主的活现在由它自己在 `startup()` 那一拍丢到池上。`exit()` 加固
   留下了：非主线程调 `exit()` 会被改道到主线程，宿主不必知道这条规矩）。
3. ~~钩子改成异步方法（`Task<void>`）+ 驱动 `co_await` 三拍 + 三拍新钉子（第 6 条）~~（完成；
   `VN_APPFW_PLUGIN_ABI_VERSION` 5u）。
4. ~~demo 内容加载搬上工作线程（1693 ms 那笔）~~（**完成 2026-09-26**，见下节）。
