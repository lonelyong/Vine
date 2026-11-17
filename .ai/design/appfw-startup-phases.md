# appfw's three startup beats and the startup work thread

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: the three beats, the thread contract, failure / cancellation / exit; the splash is in `appfw-startup-splash.md`.
> **Sources**: `Application.hpp`, `PluginManager.hpp`, `tests/test_appfw`.
> **Backlog**: `../backlog/appfw-startup.md`

## 1. Overview

### 1.1 Problem and background

While one long piece of work holds the main thread during startup, the UI does not repaint, the window does not answer, and the cancel button cannot be reached.
Measured, "cold start to a drawable main window" splits into: splash on screen → first frame 7 ms; plugin libraries loaded / instantiated 1 ms; demo content loaded
1693 ms; `render_control->init()` (attach + warm-up) 413 ms. About 80% is content loading (I/O + decoding), not a one-off cost of rendering.

### 1.2 Goals

- Startup splits into three beats, with the order written in one place.
- A beat that can be awaited is expressed as a coroutine; the host's heavy work is the host's to put on the pool.
- A startup exception is fatal and a cancellation is not a failure; progress updates per phase.
- The application thread's longest continuous occupancy has a gate (150 ms by default).

### 1.3 Non-goals

- The framework does not own threads for the host; there is no `run(work)` overload.
- The startup driver does not move to a new thread; there is no startup animation or progress bar.
- `render_control->init()` does not move to the pool; it is a synchronous attach.

### 1.4 One-paragraph conclusion

`run()` calls `exec()` first and then pushes one startup step; `startupSequence()` lays out `startupStart()` → `startup()` → `startupEnd()` flat, with one
`resumeOnMainThread()` after each beat; an exception goes through `failStartup()`, a cancellation through `cancelStartup()`, and neither goes through `startupEnd()`.

## 2. Design Points

### 2.1 The three beats

```cpp
class Application {
  public:
    virtual int run();

  protected:
    virtual vn::async::Task<void> startupStart();   // beat 1 (must suspend: wait until what it put up is really on screen)
    virtual vn::async::Task<void> startup();        // beat 2 (framework: plugin loading; the host overrides it to add its own work)
    virtual vn::async::Task<void> startupEnd();     // beat 3 (the wrap-up, fully synchronous)
};
```

`startupSequence()` is a member coroutine (`DetachedTask`, eager) that lays the three beats out flat:

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

### 2.2 The thread contract

- Every beat ends on the main thread: the framework guarantees it for the host inside `startup()`, and a leaf calls `resumeOnMainThread()` itself.
- A plugin's `preLoad/load/postLoad` run on the application thread (`load()` builds controls, views and native windows).
- The host's pure computation / I/O / decoding may go to the pool; reporting through `StartupProgress` is safe across threads.
- The main thread never blocks waiting for a worker; if it did, the loop would stop and this design would be pointless.

### 2.3 Lifetime and exit

- An exception: any beat throwing → log it → `failStartup()` (closes the reporting door + `exit(1)`), without `startupEnd()`.
- A plugin `loadAll()` returning false only means "one plugin did not load": log a warning and start normally.
- Cancellation: `StartupProgress::stopToken()` / `requestCancel()`; `cancelStartup()` unloads the loaded plugins + closes the reporting door + `exit(0)`, and is not a failure.
- The framework has no startup wrap-up wait; `shutdown()` does not manage the host's own background work.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Driving the three beats | `startupSequence()` lays out the three beats, back to the main thread after each | landed |
| F2 | The host's startup work | the host overrides `startup()` and puts heavy work in `co_await vn::async::run(...)` | landed |
| F3 | Plugin loading | `loadAllAsync()` is the startup door; `loadAll()` the synchronous one | landed |
| F4 | Cancellation | a stop token; `cancelStartup()` goes through `exit(0)` | landed |
| F5 | The occupancy gate | `VN_GATE_BOOT_OCCUPANCY_MS`, 150 ms by default | landed |
| F6 | Synchronous init | `RenderControl::init()` is paid once on the application thread | landed |

### 3.1.1 F1 Driving the three beats

- **Behaviour**: `run()` calls `exec()` first and then pushes the startup step; `startupSequence()` `co_await`s the three beats in order.
- **Constraints** (C1): `resumeOnMainThread()` after every beat; the next beat and the wrap-up run on the application thread.
- **Requirements** (R1): the three beats are declared as one group at the end of the class; `startupStart` / `startup` / `startupEnd` are all protected.
- **Acceptance** (T1): `test_appfw`'s startup-step / main-thread assertions.
- **Caveat** (N1): `co_await` cannot appear inside a catch block, so the failure path gets its own `resumeOnMainThread()`.

### 3.1.2 F2 The host's startup work

- **Behaviour**: the host overrides `startup()`, first `co_await Application::startup()` (plugins load first), then puts the heavy work on the pool with
  `co_await vn::async::run(...)`.
- **Constraints** (C2): the framework does not own threads for the host; there is no `std::thread` / join / detach / handshake / flag.
- **Requirements** (R2): the host must `resumeOnMainThread()` before touching the UI from a worker thread.
- **Acceptance** (T2): `test_appfw`'s thread-id / main-thread-timer assertions.
- **Caveat** (N2): background work the host launches and lets outlive the loop is the host's own contract.

### 3.1.3 F3 Plugin loading

- **Behaviour**: `loadAllAsync()` is the startup door (a coroutine; the manager does not turn the loop); `loadAll()` is the synchronous door
  (`runToCompletion(task, pump)`, where the pump only dispatches `deliverPostedCalls()`).
- **Constraints** (C3): the synchronous door must not use `Task::result()` — it blocks without dispatching, so a hook coming back to the main thread deadlocks.
- **Requirements** (R3): every exit of a plugin hook writes `co_return;`; the ABI is currently 8u.
- **Acceptance** (T3): `TheSynchronousDoorLoadsAPluginThatComesBackToTheApplicationThread`.
- **Caveat** (N3): the manager does not pump the queue or post a 0 ms timer itself, or it would run other people's timers.

### 3.1.4 F4 Cancellation

- **Behaviour**: `StartupProgress::stopToken()` / `requestCancel()`; the framework looks at it after every beat and before every plugin.
- **Constraints** (C4): cancellation is not a failure; `cancelStartup()` goes through `exit(0)` and the main window never appears.
- **Requirements** (R4): a plugin takes the same token from `PluginLoadContext::stopToken()`.
- **Acceptance** (T4): `test_appfw`'s cancellation cases.
- **Caveat** (N4): a plugin outside startup gets a token that never stops.

### 3.1.5 F5 The occupancy gate

- **Behaviour**: the application thread's longest continuous occupancy is written to stderr (measured by a 10 ms timer);
  `scripts/vsg_rewrite_gate.sh`'s application stage requires `<= VN_GATE_BOOT_OCCUPANCY_MS` (150 ms by default).
- **Constraints** (C5): moving a piece back onto the application thread makes the gate red outright; a missing log line also counts as red.
- **Requirements** (R5): prove it with `VN_GATE_BOOT_OCCUPANCY_MS=1` on the same gate.
- **Acceptance** (T5): `[FAIL] app ... the boot held the application thread for N ms`.
- **Caveat** (N5): the default is twice the measured value, so a busy machine does not go red for nothing.

### 3.1.6 F6 Synchronous init

- **Behaviour**: `RenderControl::init()` attaches synchronously: device / session / pipelines + the warm-up frame are all paid once on the application thread.
- **Constraints** (C6): `initAsync()` is only `co_return init()`; it neither suspends nor goes to the pool.
- **Requirements** (R6): the constructor only assembles; the host calls `init()` / `initAsync()` in its own order.
- **Acceptance** (T6): `RenderControlTest.InitAsyncCompletesWithoutSuspending`.
- **Caveat** (N6): attach reads the scene graph, so the host must not change the scene from another thread while attach is running.

## 4. Public Interface

### 4.1 `Application`

| Interface | Contract | Notes |
| --- | --- | --- |
| `run()` | the startup loop + the three beats | blocks until exit |
| `startupStart()` / `startup()` / `startupEnd()` | protected asynchronous hooks | declared at the end of the class |
| `beginStartupProgress()` / `endStartupProgress()` | open / close the reporting door | called by the driver |
| `failStartup()` / `cancelStartup()` | failure / cancellation | `exit(1)` / `exit(0)` |

### 4.2 `MainThreadDispatcher`

| Interface | Contract | Notes |
| --- | --- | --- |
| `isMainThread()` / `hasEventLoop()` | queries | application-thread tests |
| `postToMainThread()` / `invokeOnMainThread()` | post / borrow synchronously | returns false once the loop has stopped |
| `resumeOnMainThread()` | resume a coroutine | a no-op when already on the main thread |
| `deliverPostedCalls()` | the synchronous door's pump | dispatches `QEvent::MetaCall` only |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | `resumeOnMainThread()` after every beat; the next beat and the wrap-up run on the application thread. |
| C2 | The framework owns no threads; the host puts its heavy work on the pool itself. |
| C3 | The synchronous door must not use `Task::result()`; the pump dispatches posted calls only. |
| C4 | Cancellation is not a failure; `cancelStartup()` goes through `exit(0)`. |
| C5 | The occupancy gate defaults to 150 ms; moving work back onto the application thread goes red outright. |
| C6 | `initAsync()` neither suspends nor goes to the pool. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | The three beats are protected and declared at the end of the class. |
| R2 | The host calls `resumeOnMainThread()` before touching the UI from a worker thread. |
| R3 | Every exit of a plugin hook writes `co_return;`; the ABI is 8u. |
| R4 | A plugin takes the same stop token; one outside startup gets a token that never stops. |
| R5 | A missing gate log line counts as red too. |
| R6 | During attach the host must not change the scene from another thread. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | Startup runs entirely inside the loop. |
| N2 | The reporting door opens before the first beat and closes after the last. |
| N3 | Neither a startup exception nor a cancellation goes through `startupEnd()`. |
| N4 | The main thread never blocks waiting for a worker. |

## 6. Caveats and Traps

### 6.1 Inlining the host's work back onto the main thread

- How it fails: the main thread is held, the window does not repaint, the cancel point cannot be reached, and the occupancy gate goes red.
- How to detect it: `VN_GATE_BOOT_OCCUPANCY_MS` is exceeded; the 10 ms timer does not tick.
- How to prevent it: the host overrides `startup()` and does `co_await vn::async::run(work)`.

### 6.2 Using `Task::result()` on the synchronous door

- How it fails: the step where a hook "comes back to the application thread" never gets dispatched → deadlock.
- How to detect it: `loadAll()` hangs.
- How to prevent it: use `MainThreadDispatcher::runToCompletion()` (the pump being `deliverPostedCalls()`).

## 7. Rejected Options

| # | Option | Reason | ADR |
| --- | --- | --- | --- |
| D1 | a framework `run(work)` overload | only the tests would use it; "a flag + a bounded wait" costs more than the convenience door. | — |
| D2 | moving the startup driver to a new thread + animation | most of the application thread's occupancy is the plugin block, which must be on the application thread; no animation can be bought that way. | — |
| D3 | putting `render_control->init()` on the pool | attach needs handles and sessions, and running it concurrently with content assembly is complicated; not now. | — |
| D4 | `Task::result()` on the synchronous door | it blocks without dispatching, so a hook coming back to the main thread deadlocks. | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | the startup step runs inside the loop and every beat returns to the main thread | `test_appfw` startup assertions |
| T2 | the host's work runs on another thread while the main thread keeps dispatching | `test_appfw` thread-id / timer assertions |
| T3 | the synchronous door loads a plugin that comes back to the main thread | `TheSynchronousDoorLoadsAPluginThatComesBackToTheApplicationThread` |
| T4 | the cancellation / failure paths | `test_appfw` cancellation / failure cases |
| T5 | the occupancy gate | `scripts/vsg_rewrite_gate.sh`'s app stage |
| T6 | `initAsync()` does not suspend | `RenderControlTest.InitAsyncCompletesWithoutSuspending` |

## 9. Status and Evolution

### 9.1 Current status

The three beats, the asynchronous plugin door, cancellation, the occupancy gate and the synchronous init are all landed.

### 9.2 Known gaps / backlog

- Moving the startup driver to a thread / animation is not done (see D2).
- Moving `render_control->init()` to the pool is not done (see D3).
- Attaching an unmapped window on Windows is still unmeasured; see `../backlog/appfw-startup.md`.

### 9.3 Compatibility / migration

The old names `beginStartup` / `finishStartup` / `runStartup` and the `run(work)` overload exist only in git history; new code is written against the three beats
`startupStart` / `startup` / `startupEnd`.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/Application.hpp`
- `src/fw/appfw/sdk/vine/appfw/PluginManager.hpp`
- `src/fw/appfw/sdk/vine/appfw/MainThreadDispatcher.hpp`
- `tests/test_appfw`

### 10.2 Documents and ADRs

- `appfw-startup-splash.md`
- `appfw-plugin-system.md`
- `appfw-render-surface.md`
- `../backlog/appfw-startup.md`

### 10.3 External material

- Qt's event loop and the semantics of `QEvent::MetaCall`.
