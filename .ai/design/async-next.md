# async 下一步设计（P2：取消环境 / 结构化作用域 / 帧分配）

> 母文档：`.ai/design/async-design.md`（§8 指向本文）。模块笔记：`.ai/memory/async.md`。
> 本文只出设计，不改代码；每节都写"为什么值得/不值得"和"怎么证明做对了"。

## 0. 结论表（依据都在下面各节）

| 项 | 结论 | 依据（实测/全仓 grep） |
| --- | --- | --- |
| 取消的**环境**（stop token 随任务走） | **建议排期**，形状已定（§1 A + C） | appfw 已经手搓了环境（`PluginLoadContext::stopToken()`、`CommandExecutionContext::stopToken()`、`StartupProgress::stopToken()`），而 async 看不见它；子任务要拿令牌只能靠作者手写参数 |
| 取消的**结果通道**（`set_stopped` 取代异常） | **不做**，只补文档 | cppcoro/folly 也用异常；上层已按 `TaskCancelledException` 分类；代价是 `Task` 结果型别 + 全部组合子 + 145 条用例 |
| 结构化作用域的**强制** | **小改**（析构 = 请求停止 + 断言，等待永远显式；加 `detach()`） | `Scope` 在生产代码 **0 使用者**（全仓 grep）；stdexec 的 `async_scope`/`counting_scope` 也是"析构断言、同步点显式 await" |
| **帧分配**（promise `operator new` / 分配器） | **不做**，挂账并写清判据 | 协程体只出现在 appfw(15 文件)/app_shell(10)/test_plugin(6)/vsg 插件(**1 处**)，`src/viz` 渲染路径 **0** 个 ⇒ 帧分配不在任何热路径 |

---

## 1. 取消的环境（建议排期）

### 1.1 现状：环境已经有了，只是没人认领

`vn::async` 的取消是**显式参数**（`whenAll(tasks, token)`、`sleepFor(d, token)`、`Scope::join(token)`），
与 cppcoro 一致。但上层早就需要一个"不靠参数传递"的令牌：

- `PluginLoadContext::stopToken()`（文档原话：the context carries the same token as `StartupProgress::stopToken()`）；
- `CommandExecutionContext::stopToken()` / `isCancelled()`；
- `StartupProgress::stopToken()`。

也就是说 **appfw 已经把令牌装进了"上下文对象"**，而 `vn::async` 的组合子看不见：`Plugin::load()` 里
`co_await whenAll(...)`、`co_await sleepFor(...)` 都拿不到启动取消，除非作者逐个把 token 传下去。
P2300 把这件事做成了**环境**（`get_stop_token(env)`），本文建议按同一思路给 async 一个最小的
"环境式令牌"，而不是引入整个 sender/receiver。

### 1.2 形状（A：环境式令牌）

```cpp
// 新增（sdk/vine/async/StopToken.hpp）
namespace vn::async {

/// 当前协程的停止令牌：没被注入过就是默认构造的空令牌。
[[nodiscard]] std::stop_token stopToken() noexcept;

/// 把令牌注入一个孩子（组合子用）：此协程 body 里 stopToken() 读到它。
[[nodiscard]] Task<void> withStopToken(std::stop_token token, Task<void> body);   // 形状示意

} // namespace vn::async
```

实现要点（决定成败的三条）：

1. **令牌存在 promise 里**：`Task` 的 `promise_type` 增加 `std::stop_token token{};`，`stopToken()` 通过
   *当前协程的 promise* 读它。取"当前协程的 promise"需要一次性把 handle 传进 body——用现成的对称转移
   awaiter（`await_suspend(handle)` 里把 `handle.promise()` 存到一个线程局部槽，见下）。
2. **注入点是"启动孩子之前"**：`WhenChild::start(state)` / `Scope::add()` / `runShared()` / `scopeChild` 都已有
   "贴元数据"的那一行（`handle_.promise().state = std::move(state);`），注入紧随其后 ⇒ **零新机制**。
3. **继承而不是替换**：组合子拿到显式 token 就用它，没有就沿用当前环境（`stopToken()`）；`withStopToken`
   显式覆盖。`Task` 的 promise 默认空令牌 ⇒ 与今天的行为完全一致（`stopToken()` 返回空 ⇒ 谁都不会因为
   "以为是取消状态"而提前返回）。

### 1.3 组合子怎么用（建议的默认策略）

| 设施 | 取消策略（A 落地后） |
| --- | --- |
| `sleepFor(d, token={})` | **不做环境回退**（见下面的更正）：被调用者读不到调用者的环境 |
| `whenAll` / `whenAny` | 显式 token 优先；**默认是否注入孩子**属于第二阶（会改行为，需重排用例） |
| `Scope::add(task)` | 注入 scope 自己的令牌（已落地：`Scope` 持有 `stop_source` ✓） |
| `withTimeout(task, d, token)` | 现状是"超时即销毁孩子"；要做三段式需单独定（见 §1.5） |

**更正（2026-09-26 实测发现，原设计这一格是错的）**：`sleepFor()` 曾经写成"没有显式 token 就用
`co_await currentStopToken()`"，但那个读的是**它自己的**环境——而它是**被调用者**，调用者的令牌不可能被它看见。
`await_transform` 只能让一个协程读**自己** promise 上的令牌，所以：

- **环境只能由创建者注入**（`withStopToken(token, child())`），**不能由被调用者回查**；
- P2300 那边能做到"操作看见调用者的环境"，是因为环境随 receiver 一路**往下传**，不是靠环境变量式的回查；
  C++20 这套形状没有 receiver，就只能由创建者显式交接（一阶即如此，见下）。

于是正确的用法是两个方向各一句：

```cpp
// 创建者（框架/组合子）把令牌交给它创建的任务：
co_await withStopToken(context->stopToken(), plugin->load(context));

// 任务体读自己的环境，并把它交给它 await 的东西：
const std::stop_token mine = co_await currentStopToken();
co_await sleepFor(std::chrono::seconds(2), mine);
```

### 1.4 不改的东西（兼容性边界）

- 所有显式参数入口**保留**（172 处 `x.result()` 与各组合子的 token 参数不动）。
- 错误通道**不变**：取消仍抛 `TaskCancelledException`（类型化，`vn::Exception::Code::CANCELLED`）。
- `Task<T>` 的型别、`await_resume` 语义、`result()` 的"只读一次"都不变 ⇒ **不改 ABI、不改 145 条用例**。

### 1.5 为什么"结果通道（`set_stopped`）"不做

`set_stopped` 的价值是"取消不是错误"：组合子不必把取消当失败、`whenAll` 不会因为一个孩子被取消而
整体报错。代价是 `Task<T>` 的结果变成三态（value / error / stopped），`await_resume`、全部组合子、
`result()`、`unhandled_exception` 路径与全部用例都要改，而**收益在这套代码里很薄**：上层（命令链、插件、
启动）已经把 `TaskCancelledException` 当作"取消"单独 catch（见 `CommandManager`、`AppLifecycle` 相关用例）。
若将来要收口 `withTimeout` 的"超时即销毁孩子"（§4.2 的已知风险），届时再评估"先请求停止、等孩子收尾、
超时才销毁"的三段式——那是**局部**改动，不必先动全局结果通道。

### 1.6 验证配方（做 A 时照此钉）

- 钉子 1（注入生效）：一个宿主给 `PluginLoadContext` 一个令牌，插件 body 里 `sleepFor(500ms)`；宿主
  `request_stop()` ⇒ 20 ms 内抛 `TaskCancelledException`（今天必须显式传参才可能）。
- 钉子 2（默认空）：不注入时 `stopToken().stop_possible() == false`，且 `sleepFor(20ms)` 正常到点。
- 钉子 3（继承不替换）：`whenAll(withStopToken(t, ...), ...)` 的孩子两两看到同一个令牌（比较 `stop_token` 的
  `stop_possible()` 与 `can_stop()`，或用一个记录型 task）。
- 变异：把注入那一行删掉 ⇒ 钉子 1 红（钉 2/3 不动）；把"继承"改成"总是空" ⇒ 钉子 3 红。
- 线程约定（必须写进头文件）：令牌只在**创建/启动孩子的那次注入**时读取，不参与竞态；`stopToken()` 是
  "当前协程的环境"，跨线程读的是同一个 `stop_token` 值语义副本（`std::stop_token` 本身线程安全）。

---

## 2. 结构化作用域（小改，不重写）

### 2.1 现状与证据

- `Scope` 的契约：孩子**自持**（`DetachedTask` + 共享状态），`~Scope` 不等待也不取消；孩子可以活得比
  作用域长，只有一句散文警告（`async-design.md` §4.4）。
- **全仓没有生产使用者**：`grep -rn 'async::Scope' src/` 只命中无关的 `PluginScope`/`ChainScope`/`ReadScope`。
  也就是说今天的"不结构化"还没有伤到任何人——但 API 一旦被用就会踩。
- 标准形状（stdexec 的 `async_scope`/`counting_scope`）：**析构断言、同步点显式 `co_await`**，不是
  "析构里等"（析构不是协程，无法挂起；同步等要么阻塞线程、要么泵事件循环，那属于宿主的策略）。

### 2.2 建议的形状

```cpp
class Scope
{
  public:
    /// 请求停止（不等待）：析构与显式调用都只做这一件事。
    void requestStop() noexcept;

    /// 不再对当前批次请求停止：明说"这批孩子我自己等，别去打断它们"（不影响 join() 仍然等待）。
    void detach() noexcept;

    ~Scope();   // debug: pending != 0 且未 detach() ⇒ 断言；release: 一条 warning
};
```

- **析构动作 = `request_stop()`**（不等待、也不断言；`pendingChildren()` 交给宿主决定怎么报——async 只依赖
  `vn::Global`+`vn::Core`，不为一条诊断引入 logging 依赖）。`requestStop()` 需要有 `stop_source`；这正是 §1 的环境
  令牌的落点：`Scope` 持有 `std::stop_source`，`add()` 把令牌注入孩子（§1.2 第 2 条），于是"析构请求停止"
  对合作式的孩子真的有效果，而 Assert/Detach 只是把"逃逸"变成显式选择。
- `join(token)` 的语义要写准：**取消只停等待**（现状），**不取消孩子**；要取消孩子就 `requestStop()`。
  两者可以并存：`join()` 内部其实可以照 `token` 先 `request_stop`（若 token 是孩子继承的那一个）——这条
  留待实现时用用例钉住（"取消 join 后孩子收尾、`join()` 返回"）。

### 2.3 验证配方

- 钉子：`ScopeDestructorRequestsStop`（孩子 `co_await sleepFor(1s)` + 观察 `TaskCancelledException`）；
  `ScopeDetachSilencesTheAssert`（`detach()` 后析构不再诊断）；`CancelJoinStillLetsChildrenFinish`（已有，
  上一轮加的 `ScopeTest.JoinCancellationWakesOffTheCancellersStackAndLeavesTheChildRunning` 里已覆盖）。
- 变异：析构里删掉 `request_stop()` ⇒ 钉子 1 红（孩子跑满 1 s 而不是立刻结束）；把断言删掉 ⇒ 诊断钉子红。
- 注意 debug 断言与 release warning 都要能被测试看到（仓库已有 `WarningCapture` 夹具，见 test_appfw 第 29 轮）。

---

## 3. 帧分配（建议不做，写清判据）

- 现状：全模块 0 个 `promise_type::operator new/delete`、0 个 `allocator_arg_t` 构造、0 个
  `get_return_object_on_allocation_failure` ⇒ 每个协程帧走全局 `operator new`。
- **全仓实测**：协程体只出现在 appfw(15 文件)、app_shell(10)、test_plugin(6)、`gfx_backend_vsg`(1 处，
  插件自己的钩子)，`src/viz` 渲染路径 **0 个**。
  ⇒ **没有任何热路径在分配协程帧**（启动期几十个、命令执行每次几个、渲染每帧 0 个），池化的收益上限就是
  "启动期少几十次 malloc"，量不出来。
- 判据（将来出现下列任一条再做）：① 出现"每帧/每事件创建协程"的用法；② profile 里协程帧分配占比可见
  （`valgrind --tool=massif` 或给 `Task` 的 promise 临时加计数 `operator new`）；③ 引入只读/裸机目标。
  做的时候第一步是 P2014 形状（`promise_type(std::allocator_arg_t, const Alloc&, ...)` + `operator new`），
  并保留"分配失败返回空"的口子（`get_return_object_on_allocation_failure`）。

---

## 4. 建议顺序

1. **§1 A + C**（环境令牌 + `Scope` 持有 `stop_source`）：一项设计、两处小改动，直接让 appfw 那些
   `*::stopToken()` 上下文"接上"async 的组合子；
2. **§2**（与 §1 同批做，因为都碰 `Scope`；析构只请求停止 + 断言 + `detach()`）；
3. **§3 挂账**（有数据再动）；
4. §1.5 的"先请求停止再等"（`withTimeout` 的三段式）留到有真实需求时单独定。

---

## 5. 第一阶已落地（2026-09-26）：可选式环境令牌 + Scope 的停止请求

**落地内容**（`src/base/async/sdk/vine/async/`）：

- `Task.hpp`：promise 新增 `std::stop_token token{}`；`await_transform(CurrentStopTokenRequest)` 用已就绪的
  `detail::ValueAwaiter<std::stop_token>` 作答，另一个 `await_transform(A&&)` 把**其余所有 co_await 原样透传**
  （promise 一旦定义 await_transform 就会拦截全部 co_await，这条透传是加钩子的前提）。
  `detail::TaskEnvironment::setToken(task, token)` 写一个**尚未启动**的惰性任务的 promise（`friend` 访问）。
- `StopToken.hpp`（新）：`co_await currentStopToken()` 读自己的环境；`withStopToken(token, task)` 由创建者注入。
- `Scope.hpp`：自持 `std::stop_source`；`add()` 在**启动孩子之前**注入令牌；`~Scope()` 只 `request_stop()`；
  新增 `pendingChildren()` 与 `detach()`（后者 = 本批次不再请求停止，`join()` 照旧等待）。
- `Sleep.hpp`：**没有**环境回退（原因见 §1.3 的更正）。

**刻意不做**：`whenAll`/`whenAny` 默认把孩子带进环境——那会让"被取消的组合子里的孩子"从"被销毁"变成"自己收尾"，
是行为变化，要连着用例一起重排（第二阶）。

**钉子（5 条，`test_async` 145 → 150）**：环境默认空（`ATaskNobodyHandedATokenToSeesAnEmptyEnvironment`）、
注入可见（`WithStopTokenBecomesTheTasksOwnEnvironment`）、被交接的令牌真能取消深层工作
（`AHandedOverTokenReachesWorkDeepInsideTheBody`，另一线程 request_stop 后 <300 ms 唤醒）、
Scope 给孩子令牌且离开时请求停止（`ChildrenRunInTheScopesEnvironmentAndLeavingAsksThemToStop`，
顺带钉 `pendingChildren()`）、`detach()` 后不再打断孩子（`ADetachedScopeLeavesItsChildrenAlone`）。

**变异（实测）**：`TaskEnvironment::setToken` 变空操作 ⇒ 注入那两条红；`~Scope()` 去掉 `request_stop()` ⇒
Scope 那条红、`detach` 那条仍绿。恢复后 150/150 绿。

**接线已落地（2026-09-26，同轮）**：`PluginManager` 不再裸等钩子，而是把启动令牌当环境交进去 —— 同步门
（`runToCompletion(async::withStopToken(context.stopToken(), plugin->hook(&context)))`）与启动门
（`co_await async::withStopToken(context.stopToken(), lp.plugin->hook(&context))`）各三拍，共 6 处。
于是插件 body 里的 `co_await currentStopToken()` 就是这次启动的取消令牌。
钉子：夹具插件的 `load()` 在既有的"这一拍请求取消"分支里多记两个键（`saw_environment` / `environment_sees_stop`），
`HeadlessBootTest.ACancelledBootEndsCleanlyWithoutFinishingTheBoot` 断言两者为真 —— 空令牌的 `stop_possible()`
是 false，所以"接线断了"与"令牌没被取消"分得开。变异：把 `load()` 那一处的 `withStopToken` 去掉 ⇒ 该用例红
（两条断言同时失败），恢复 ⇒ 绿。
