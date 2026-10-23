# appfw 启动阶段三拍与启动工作线程

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：三拍、线程契约、失败/取消/退出；启动框呈现见 `appfw-startup-splash.md`。
> **权威来源**：`Application.hpp`、`PluginManager.hpp`、`tests/test_appfw`。
> **待办**：`../backlog/appfw-startup.md`

## 1. 概要设计

### 1.1 问题与背景

启动期主线程被一段长工作占住时：UI 不刷新、窗口不响应、取消点不到。实测把“冷启动到主窗可画”劈开：
启动框上屏 → 首帧 7 ms；插件库加载/实例化 1 ms；demo 内容加载 1693 ms；`render_control->init()`
（attach + warm-up）413 ms。约八成是内容加载（I/O + 解码），不是渲染的一次性开销。

### 1.2 目标

- 启动阶段拆成三拍，顺序只写在一处。
- 可等待的拍用协程表达；宿主的重活由宿主自己丢到池上。
- 启动异常致命、取消不是失败；进度按相位更新。
- 应用线程最长连续占用有门禁（默认 150 ms）。

### 1.3 非目标

- 框架不替宿主拥有线程；没有 `run(work)` 重载。
- 不把启动驱动搬到新线程；不做启动期动画/进度条。
- 不把 `render_control->init()` 搬到池上；它是同步 attach。

### 1.4 一句话结论

`run()` 先 `exec()`，再推一个启动步；`startupSequence()` 平铺 `startupStart()` → `startup()` →
`startupEnd()`，每拍之后垫一次 `resumeOnMainThread()`；异常走 `failStartup()`，取消走 `cancelStartup()`，
两者都不走 `startupEnd()`。

## 2. 设计要点

### 2.1 三拍

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

`startupSequence()` 是成员协程（`DetachedTask`，eager），三拍平铺：

```cpp
beginStartupProgress()->stage("正在启动");
bool ok = true;
try {
    co_await startupStart();
    co_await MainThreadDispatcher::resumeOnMainThread();
    co_await startup();
    co_await MainThreadDispatcher::resumeOnMainThread();
    co_await startupEnd();
    co_await MainThreadDispatcher::resumeOnMainThread();
} catch (...) { ok = false; }
if (!ok) { co_await MainThreadDispatcher::resumeOnMainThread(); failStartup(); co_return; }
endStartupProgress();
```

### 2.2 线程契约

- 每拍都在主线程结束：框架在 `startup()` 里替宿主保证；叶子自己 `resumeOnMainThread()`。
- 插件 `preLoad/load/postLoad` 在应用线程（`load()` 建控件、视图、原生窗口）。
- 宿主的纯计算/IO/解码可以丢到池上；通过 `StartupProgress` 上报是跨线程安全的。
- 主线程永不阻塞等 worker；否则循环停了，本设计就白做了。

### 2.3 生命周期与退出

- 异常：任一拍抛异常 → 记日志 → `failStartup()`（收上报口 + `exit(1)`），不走 `startupEnd()`。
- 插件 `loadAll()` 返回 false 只是“某个插件没装上”，记告警，应用照常启动。
- 取消：`StartupProgress::stopToken()` / `requestCancel()`；`cancelStartup()` 卸已装插件 + 收上报口 +
  `exit(0)`，不是失败。
- 框架没有启动收尾等待；`shutdown()` 不管宿主自己的后台活。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 三拍驱动 | `startupSequence()` 平铺三拍，每拍后回主线程 | 已落地 |
| F2 | 宿主启动工作 | 宿主重写 `startup()`，重活 `co_await vn::async::run(...)` | 已落地 |
| F3 | 插件加载 | `loadAllAsync()` 启动门；`loadAll()` 同步门 | 已落地 |
| F4 | 取消 | stop token；`cancelStartup()` 走 `exit(0)` | 已落地 |
| F5 | 占用门禁 | `VINE_GATE_BOOT_OCCUPANCY_MS` 默认 150 ms | 已落地 |
| F6 | 同步 init | `RenderControl::init()` 在应用线程一次付掉 | 已落地 |

### 3.1.1 F1 三拍驱动

- **行为**：`run()` 先 `exec()`，再推启动步；`startupSequence()` 依次 `co_await` 三拍。
- **约束**（C1）：每拍之后必须 `resumeOnMainThread()`；下一拍与收尾都在应用线程。
- **要求**（R1）：三拍声明为类末尾一组；`startupStart`/`startup`/`startupEnd` 都是 protected。
- **验收**（T1）：`test_appfw` 的启动步/主线程断言。
- **注意**（N1）：`co_await` 不能写在 catch 块里，所以失败路径单独垫一次 `resumeOnMainThread()`。

### 3.1.2 F2 宿主启动工作

- **行为**：宿主重写 `startup()`，先 `co_await Application::startup()`（插件先加载），
  重活再 `co_await vn::async::run(...)` 丢到池上。
- **约束**（C2）：框架不替宿主拥有线程；没有 `std::thread`/join/detach/握手/旗子。
- **要求**（R2）：宿主在工作线程碰 UI 前必须 `resumeOnMainThread()`。
- **验收**（T2）：`test_appfw` 的线程 id/主线程定时器断言。
- **注意**（N2）：宿主丢出去的后台活活得比循环久，是宿主自己的契约。

### 3.1.3 F3 插件加载

- **行为**：`loadAllAsync()` 是启动门（协程；管理器不转循环）；`loadAll()` 是同步门
  （`runToCompletion(task, pump)`，pump 只派发 `deliverPostedCalls()`）。
- **约束**（C3）：同步门不能用 `Task::result()`，它只阻塞不派发，钩子回主线程会死锁。
- **要求**（R3）：插件钩子每条出口都写 `co_return;`；ABI 当前 8u。
- **验收**（T3）：`TheSynchronousDoorLoadsAPluginThatComesBackToTheApplicationThread`。
- **注意**（N3）：管理器自己不抽队列/不放 0 ms 定时器，否则会带跑别人的定时器。

### 3.1.4 F4 取消

- **行为**：`StartupProgress::stopToken()` / `requestCancel()`；框架每拍之后、每个插件之前看一眼。
- **约束**（C4）：取消不是失败；`cancelStartup()` 走 `exit(0)`，主窗不上屏。
- **要求**（R4）：插件从 `PluginLoadContext::stopToken()` 拿同一个 token。
- **验收**（T4）：`test_appfw` 的取消用例。
- **注意**（N4）：不在启动里的插件拿到永不停的 token。

### 3.1.5 F5 占用门禁

- **行为**：应用线程最长一次连续占用打到 stderr（10 ms 定时器量间隔）；
  `scripts/vsg_rewrite_gate.sh` 的应用阶段要求 `<= VINE_GATE_BOOT_OCCUPANCY_MS`（默认 150 ms）。
- **约束**（C5）：把某段搬回应用线程 ⇒ 门禁直接红；日志里没有这一行也算红。
- **要求**（R5）：反证用 `VINE_GATE_BOOT_OCCUPANCY_MS=1` 跑同一道门。
- **验收**（T5）：`[FAIL] app ... the boot held the application thread for N ms`。
- **注意**（N5）：默认值是实测的两倍，免得机器一忙就假红。

### 3.1.6 F6 同步 init

- **行为**：`RenderControl::init()` 同步 attach：设备/会话/管线 + warm-up 帧都在应用线程一次付掉。
- **约束**（C6）：`initAsync()` 只是 `co_return init()`，不挂起、不下池。
- **要求**（R6）：构造只组装；宿主在自己的顺序里调 `init()`/`initAsync()`。
- **验收**（T6）：`RenderControlTest.InitAsyncCompletesWithoutSuspending`。
- **注意**（N6）：attach 读场景图，宿主不要在 attach 进行中从别的线程改场景。

## 4. 公开接口

### 4.1 `Application`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `run()` | 启动循环 + 三拍 | 阻塞到退出 |
| `startupStart()` / `startup()` / `startupEnd()` | protected 异步钩子 | 声明在类末尾 |
| `beginStartupProgress()` / `endStartupProgress()` | 上报口建/收 | 驱动调用 |
| `failStartup()` / `cancelStartup()` | 失败/取消 | `exit(1)` / `exit(0)` |

### 4.2 `MainThreadDispatcher`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `isMainThread()` / `hasEventLoop()` | 查询 | 应用线程判定 |
| `postToMainThread()` / `invokeOnMainThread()` | 投递/同步借用 | 循环停了返回 false |
| `resumeOnMainThread()` | 协程恢复 | 空操作 if 已在主线程 |
| `deliverPostedCalls()` | 同步门 pump | 只派发 `QEvent::MetaCall` |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 每拍之后 `resumeOnMainThread()`；下一拍与收尾在应用线程。 |
| C2 | 框架不拥有线程；宿主自己把重活丢到池上。 |
| C3 | 同步门不能用 `Task::result()`；pump 只派发已投递调用。 |
| C4 | 取消不是失败；`cancelStartup()` 走 `exit(0)`。 |
| C5 | 占用门禁默认 150 ms；搬回应用线程直接红。 |
| C6 | `initAsync()` 不挂起、不下池。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 三拍 protected、声明在类末尾。 |
| R2 | 宿主在工作线程碰 UI 前 `resumeOnMainThread()`。 |
| R3 | 插件钩子每条出口 `co_return;`；ABI 8u。 |
| R4 | 插件拿同一个 stop token；不在启动里的拿永不停 token。 |
| R5 | 门禁日志缺失也算红。 |
| R6 | attach 期间宿主不得从别的线程改场景。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 启动阶段整个跑在循环里。 |
| N2 | 上报口建在第一拍前、收在最后一拍后。 |
| N3 | 启动异常/取消都不走 `startupEnd()`。 |
| N4 | 主线程永不阻塞等 worker。 |

## 6. 注意事项与陷阱

### 6.1 把宿主工作内联回主线程

- 会怎么错：主线程被占住，窗口不刷新、取消点不到；占用门禁红。
- 怎么判：`VINE_GATE_BOOT_OCCUPANCY_MS` 超限；10 ms 定时器不跳。
- 怎么防：宿主重写 `startup()`，`co_await vn::async::run(work)`。

### 6.2 同步门用 `Task::result()`

- 会怎么错：钩子“回到应用线程”那一步等不到派发 → 死锁。
- 怎么判：`loadAll()` 挂死。
- 怎么防：用 `MainThreadDispatcher::runToCompletion()`（pump = `deliverPostedCalls()`）。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 框架提供 `run(work)` 重载 | 只有测试在用；“旗子 + 有界等待”比便利门贵。 | — |
| D2 | 启动驱动搬到新线程 + 动画 | 应用线程占用里大部分是必须在应用线程的插件块；买不到动画。 | — |
| D3 | 把 `render_control->init()` 搬到池上 | attach 要句柄/会话，且与内容装配并发复杂；暂不做。 | — |
| D4 | 同步门用 `Task::result()` | 只阻塞不派发，钩子回主线程死锁。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 启动步在循环里、每拍回主线程 | `test_appfw` 启动断言 |
| T2 | 宿主 work 在别的线程、主线程仍派发 | `test_appfw` 线程 id/定时器断言 |
| T3 | 同步门能加载“回主线程”的插件 | `TheSynchronousDoorLoadsAPluginThatComesBackToTheApplicationThread` |
| T4 | 取消/失败路径 | `test_appfw` 取消/失败用例 |
| T5 | 占用门禁 | `scripts/vsg_rewrite_gate.sh` app 阶段 |
| T6 | `initAsync()` 不挂起 | `RenderControlTest.InitAsyncCompletesWithoutSuspending` |

## 9. 状态与演进

### 9.1 当前状态

三拍、异步插件门、取消、占用门禁、同步 init 均已落地。

### 9.2 已知缺口 / 待办

- 启动驱动搬线程/动画不做（见 D2）。
- `render_control->init()` 搬池不做（见 D3）。
- Windows 的未显示窗口 attach 仍未实测；见 `../backlog/appfw-startup.md`。

### 9.3 兼容性 / 迁移

旧名 `beginStartup` / `finishStartup` / `runStartup` 与 `run(work)` 重载只在 git 历史里；
新代码按 `startupStart` / `startup` / `startupEnd` 三拍写。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/Application.hpp`
- `src/fw/appfw/sdk/vine/appfw/PluginManager.hpp`
- `src/fw/appfw/sdk/vine/appfw/MainThreadDispatcher.hpp`
- `tests/test_appfw`

### 10.2 文档与 ADR

- `appfw-startup-splash.md`
- `appfw-plugin-system.md`
- `appfw-render-surface.md`
- `../backlog/appfw-startup.md`

### 10.3 外部资料

- Qt 事件循环与 `QEvent::MetaCall` 语义。
