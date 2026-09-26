# appfw 环境进度（`ProgressHost`）设计（2026-09-18：搬迁 + 推送模型）

## 归属与分工

- **base / `vn::Progress`**：`ProgressIndicator`（[0,1] 标尺、原子位置、外部取消 token）、
  `ProgressRange` / `ProgressScope`（把标尺切成区间）。零外部依赖，插件和测试可以直接用来算进度。
- **appfw / `vn::Appfw`**：`ProgressHost`（`sdk/vine/appfw/ProgressHost.hpp`）——进程级注册表、
  前台栈、label、取消源共享、变更通知。"应用此刻在干什么"是应用状态，而且通知要用 Core 的 `Signal`
  （base 不该依赖 Core），所以宿主归 appfw。
- 2026-09-18 之前宿主在 base：那里用不了 `Signal`，只好自带回调且只能挂**一个**观察者。
  这是层约束逼出来的降级设计，搬迁后 GUI 呈现器与控制台消费者可以各自订阅。
- 依赖方向：appfw → `vn::Progress`。`ProgressHost.hpp` include indicator/range/scope 且按值返回
  range/scope，所以 appfw 里 `vn::Progress` 是 **PUBLIC** 链接。
- `VN_APPFW_PLUGIN_ABI_VERSION` 2u → **3u**：公共 SDK 头变了（宿主换了 include 路径与命名空间），
  插件必须重编。

## 推送模型（原来是拉）

- `ProgressHost::changed()` 返回进程级 `Signal<>`，在以下时刻发火：宿主注册/注销、前台栈压入/弹出、
  label 变化（设成相同值不发）、指示器位置跨过整百分点（含到终点、含被重置）。
- 信号**不带载荷**：注销通知是在宿主析构体内发出的（成员还活着），带指针等于交给观察者一个正在死的对象；
  观察者重采样 `current()` / `activeHosts()`。
- 发火线程 = 改状态的线程（可能是工作线程）：呈现层自己编组。
  发火在锁外进行，绝不与注册表锁嵌套（观察者是用户代码，会读注册表）。
- **源头合流**在最底层：`ProgressIndicator::setPositionCallback(std::function<void()>)` 只在
  "跨整百分点 / 到终点（哪怕最后一步远小于 1%）/ 位置被重置"时回调一次，而 `increment()` 是每条目一次
  的热路径。实测（-O3，2000 万条目）：`next(1)` 由 10.0 → 9.9 ns/item（噪声内，无可测代价），
  2000 万条目只发 **101** 次通知。

## 两个消费者

- `gui::ProgressPresenter`：订阅 changed() 后**不再轮询**；空闲时不持有任何定时器，只为两个截止时间
  （"出现延迟" 400ms、"隐藏延迟" 300ms）臂一个单发 `QTimer`。跨线程通知用
  `QMetaObject::invokeMethod(widget, …, Qt::QueuedConnection)`：投给**原生 widget**，Qt 会丢弃接收者
  已销毁的排队调用，而 presenter 随 widget 一起销毁，所以排队调用既不会越过 widget 生命周期，
  也不会在 `self` 之后运行。
- `ConsoleProgressReporter`（无头，`sdk/vine/appfw/ConsoleProgressReporter.hpp`）：订阅 changed()。
  唯一需要时钟的是 show_delay——跑到一段时间而没有任何进度报告也该出一行——用 async 共享定时服务的
  **一次性**唤醒（generation 计数器把 stop() 之后到点的唤醒变成空操作，`sleeping_` 保证同时只有一个）。
  `poll()` 保留给宿主自驱循环与测试，且**不需要**先 `start()`。

## 契约与不变量

1. label 跨线程安全：`label()` **按值**返回（2026-09-18 修正——原来是 `const std::string&`，
   与工作线程里的 `setLabel` 构成真实数据竞争），写入侧互斥锁保护，值没变则不发通知。
2. `stop()` 返回后不再有任何输出：互斥锁栅栏 + generation；注销通知在宿主析构体内发出，
   所以观察者重采样时看到的是"已经恢复的前台"（父宿主或 nullptr）。
3. 没有前台宿主时 `current()` 为 nullptr；`LongRunning` 命令用 `setForeground(true)` 提升为前台，
   嵌套子命令压栈、析构自动恢复父宿主。**启动阶段自己也是一个前台宿主**（`StartupProgress`，
   由启动驱动 `startupSequence()` 在第一拍之前建、启动收完后由 `endStartupProgress()` 销毁）⇒ 无头宿主通过控制台消费者
   看到 `[进度] …`，启动框只是同一份状态的另一个呈现者；构造时不建，否则不跑启动的进程会一直被判成"忙"
   （`isBusy()` = 有前台宿主）。
4. **空闲零唤醒**：presenter 无定时器，控制台消费者没有待发唤醒；只有真正有待办截止时间时才各臂一个。
5. `ConsoleProgressOptions::interval` 现在的语义是**最小行距**（原来是轮询周期）；测试手动步进时设 0。

## 测试映射

| 用例 | 覆盖 |
| --- | --- |
| `tests/test_progress/ProgressIndicatorTest.cpp`（9 例，只链 `vn::Progress vn::Core`） | 位置算术、range 一次性、嵌套 scope、回调合流、到终点必通知、重置再通知、跨线程读、无回调也无害 |
| `tests/test_gui/ProgressHostTest.cpp`（15 例） | 注册表/前台栈/label/取消，以及 changed() 的注册、label（值相同不发）、前台压栈/弹出、注销、合流（1 万条目约 100 次）、句柄析构即静音 |
| `ConsoleProgressReporterTest.*`（3 例，`test_gui`） | 手动步进的节流与收尾、通知驱动 + `stop()` 后静音、`ConsoleUserIO` 端到端打印 |
| `ProgressPresenterTest.*`（`test_gui`） | 取消按钮、`isBusy()` |

## 已评估但**未采纳**（留档）

- **保留 base 里的自查回调 + 单观察者**：层约束逼出来的降级（写一个更差的 Signal），搬迁后作废。
- **把整个 progress 模块搬进 appfw**：`ProgressIndicator`/Range/Scope 是零依赖原语，插件与 base 测试
  直接可用；整搬会让 `test_progress` 被迫链 `vn::Appfw`（Qt/Graphics/SARibbon）。
- **给控制台消费者保留常驻轮询循环**：事件驱动 + 一次性唤醒已足够，且空闲零唤醒；
  轮询只在需要"证明它还在跑"时才有价值，而那正是 show_delay 那一次唤醒。
- **让宿主自己抱住 indicator 的钩子以外的方式**（例如宿主轮询自己的 indicator）：那就退回了拉模型。
