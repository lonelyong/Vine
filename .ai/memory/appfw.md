# appfw notes (`src/fw/appfw`)

> **Last verified**: 2026-11-19.
> The current design is split by module across the appfw pages under `.ai/design/`; this file holds only
> the cross-module rules a working session must recall immediately.
> History, rejected options and round-by-round review records are in git; open items are in
> `.ai/backlog/appfw-startup.md`.

## Startup

- Three steps: `startupStart()` → `startup()` → `startupEnd()`, all `vn::async::Task<void>`, protected,
  declared at the end of the class.
- `run()` calls `exec()` first, then posts one startup step; `startupSequence()` flattens the three steps and
  pads each one with a `MainThreadDispatcher::resumeOnMainThread()` (after a `co_await` the code runs on the
  waking thread).
- The progress reporter is created and closed by the driver: `beginStartupProgress()` before the first step,
  `endStartupProgress()` after the last one; the `startupEnd()` step can still report.
- A startup exception is fatal: `failStartup()` = log + close the progress reporter + `exit(1)`, and **does
  not** run `startupEnd()`. A plugin `loadAll()` returning false only means "one plugin did not load"; it
  logs a warning and the application starts as usual.
- The framework does not own threads for the host: the host overrides `startup()`, first
  `co_await Application::startup()` (plugins load first), then `co_await vn::async::run(...)` to put the
  heavy work on the pool; an exception thrown from that step is equally fatal.
- Startup is cancellable: `StartupProgress::stopToken()` / `requestCancel()`; `cancelStartup()` unloads the
  installed plugins + closes the progress reporter + `exit(0)`; a cancel is not a failure and the main window
  never goes on screen.
- A cancellation that arrives as an exception is the same cancellation: `vn::async::CancellationToken` *is*
  `std::stop_token`, so token-aware work (`sleepFor(d, token)`, `whenAll(…, token)`, `withCancellation`) throws
  `TaskCancelledException` (`Exception::CANCELLED`). The driver maps it to `cancelStartup()` (exit 0); a plain
  exception is still `failStartup()` (exit 1). `CommandManager` and `PluginManager::loadAllAsync()` classify the
  same exception the same way.
- The loop can stop while a phase is waiting: `shutdown()` closes a reporting door it still finds open, sets the
  abandoned flag, and every suspension point of `startupSequence()` (plus the end of the plugin load) returns on
  `bootAbandoned()` - otherwise `isBusy()` would stay true forever and the main window would go up in a torn-down
  application. `run()` is one per Application (it ends with `shutdown()`).
- One `Application` per process: a second construction throws `vn::Exception(INVALID_OPERATION, "only one
  Application may exist per process")`, and the process-wide slot is withdrawn again if the constructor itself
  throws.
- `Application::isBusy()` reads `ProgressHost::currentSnapshot()`, never the borrowed `current()`: the question is
  asked by an observer (command admission, on whichever thread) that may outlive the operation it looks at.
- `Application::exit()` is safe from any thread; from a worker it is queued, and when no loop is running the request
  is **dropped with a warning** instead of being armed for the loop `run()` is about to start (`loop_running` is
  atomic for exactly that read).
- `GuiApplicationData` owns its widgets through `unique_ptr`, declared main window → view registry → document host →
  startup frame, so the frame dies first and the host hands its slot back before the window goes; `~GuiApplication`
  only reports a frame left up by a boot that never reached its last phase.
- Progress updates per phase; there is no continuous animation or heartbeat. The splash has only a status
  line and a spinner, never a percentage or a progress bar.
- During startup the splash is the only thing on screen; the main window `show()`s for the first time in
  `startupEnd()`, and the splash is closed at the same time with `raise()`/`activate()`.
- The Windows question "an embedded surface already has a valid HWND before the main window is shown" is
  still unmeasured; see `.ai/backlog/appfw-startup.md`.

## Plugins

- Three lifecycle passes: `preLoad()` for everyone → `load()` for everyone → `postLoad()` for everyone; a
  declared dependency only orders the `load` pass.
- `loadAllAsync()` is the startup gate (a coroutine; the manager does not pump the loop itself); `loadAll()`
  is the synchronous gate for tools, tests and self-driven hosts that do not run a loop.
- The synchronous gate = `MainThreadDispatcher::runToCompletion()` (the pump only dispatches
  `deliverPostedCalls()`); **do not use `Task::result()`**, which blocks without dispatching, so the hook's
  "back to the application thread" step deadlocks.
- A hook returns `Task<void>` and **every exit needs `co_return;`**; without it the function is not a
  coroutine, returns an empty `Task`, and `co_await` hits `ud2`.
- `VN_APPFW_PLUGIN_ABI_VERSION` = **10u**.
- `uuid` is the identity: same name with a different uuid (first position wins), different names with the
  same uuid (both stay), and a registration file that disagrees with the library (the library wins) are all
  only warnings.
- `Application::shutdown()` is protected: only `run()`'s teardown path calls it; the host, commands and
  plugins stop the process with `exit()`. Calling `shutdown()` while the loop still runs or from a
  non-application thread only warns; it is not refused.

## Document model

- The framework defines **document identity and lifetime only**: `Document` + `DocumentManager`; one per
  process, reachable as `Application::documentManager()`, created in `initialize()` before plugins load, so a
  plugin can register types in `load()`.
- Open = `open(payload)`: the opener is registered by the payload's C++ type; the framework does not
  interpret the payload, does not copy it and does not hold it. Refresh is the type's `reload()` action
  slot, not "replay `open(payload)`".
- A location is **`Vfs` + a virtual path**, not a bare `path`; core does not know `Vfs` (appfw does not link
  iobase).
- `DocumentSource` is a value type and is not derived; "no source" = an empty value; parameterless `save()`
  writes back to the source and the base class refuses by default.
- Three contracts: the `Vfs` is borrowed and must outlive the document; the `Vfs` is lock-free (one instance
  must not be touched by two threads); the payload is valid only during `open()`.
- The view layer exists only in `GuiApplication`: `DocumentView` + `DocumentViewRegistry` +
  `CentralDocumentHost`.
- A view is per **document instance**; `content()` is "the thing placed into the central area". A 2D view
  returns itself and a 3D view returns itself too ⇒ the host has one rule only: one page per instance.
- The central area is a single slot, and `setCentralWidget()` reparents (Qt recreates the native window) ⇒
  put one container there, set it once, and switch pages inside it with a `QStackedWidget`.
- Three displays have one source only (2026-10-28): `DocumentManager::current()` ⇒ the tab selection, the
  central-area content and the view `activate()` is called on, all derived in one place,
  `CentralDocumentHost::syncToCurrentDocument()`. Activating a floating window is only a `setCurrent()`
  request.
- While the current document floats, the central area shows an explanation page (`shownContent()`) and
  `currentView()` is still that document; the tab's context menu offers float / dock / close.
- Closing the current document: the manager emits `currentChanged(nullptr)` and then `closed`; in `closed`
  the host asks "the document that took over that slot" to become current, because the tab bar cannot
  express "nothing selected" (measured on Qt 6.10: `setCurrentIndex(-1)` is a no-op while tabs exist). With
  one or zero documents the bar is hidden.
- Each 3D document has its own `RenderControl` (its own surface / session / device / pipeline); only the
  DockPanel/Ribbon shell is shared at application level.
- `VSG_MAX_DEVICES` = **4** (set explicitly in `gfx_backend_vsg/CMakeLists.txt`). A fifth block is refused
  by the backend: the `vsg::Device` constructor throws, `deviceFailureMessage()` turns that into a reason
  containing "at most 4 simultaneous render devices", and `RenderControl::init()` returns false and enters
  `Failed`.
- The multi-pane / shared-session machinery is gone: `RenderControl::present()`, a public
  `SceneView::removeWindowPass()` and the host "borrowing a page" no longer exist.
- Input reaches a view through one channel only: `SceneView`'s manipulator. Picking lives in the SDK;
  selection / highlight / rubber-band selection is business logic, and the framework has no selection state.

## Render control and viewer

- Construction only assembles; attach happens through `init()` / `initAsync()` or as a fallback on the first
  step. `init()` is synchronous and idempotent; it returns false while the surface is not laid out yet, and
  calling it again is the retry.
- `initAsync()` is just the coroutine spelling of `co_return init()`; it **does not suspend and does not go
  to the pool**. Device / session / pipeline and the warm-up frame are all paid once on the application
  thread (the longest measured stretch ~184 ms).
- Attach reads the scene graph (pipelines are built by passes and the warm-up has to record) ⇒ the host must
  not modify the scene from another thread while attach runs; there is no `isAttaching()` flag today, attach
  and content assembly on the application thread are naturally serial.
- `Viewer` is the **requesting half** of one render area: `setViewerOptions()` / `setShadingMode()` /
  `release(view)` / `view()` / `isActive()` / `takeArea()`. One area draws one picture; multi-pane is gone.
- If the host declares its own main pass on the area ⇒ `isActive()` is false and `takeArea()` does nothing;
  to take over, declare first or `release(view)` first.
- A central-area `setCentralWidget()` reparent recreates the native window ⇒ the `RenderControl` goes
  through `Pending -> Presenting` once.

## Ribbon

- Tab/group identity, roles and order: see `.ai/design/appfw-ribbon.md`; the standard catalogue lives in
  app_shell and a plugin tab anchors with a role.
- The state of a `setCommand()` item (button, menu item, Backstage) is **derived**: enabled follows
  `isCommandEnabled()` plus the `NeedsDocument` / `NeedsView` requirement on the dispatch target, the tooltip
  follows the description and the checked state comes from `setCheckedProvider()`; the implementation exists
  in one place, `src/gui/CommandBinding.hpp`. `setEnabled()` on a bound item is only a temporary effect.
- `CommandManager::commandFlags()` / `commandDescription()` read the cache made at registration; aliases
  resolve the same way as the execution entry.
- Do not leave row height to the style sheet: `min-height` constrains the content box only, so a 1 px bottom
  border makes the row taller (a Backstage item row 44 px ≠ a navigation row 40 px); use `setFixedHeight()`
  for the size and let the style sheet own colour and padding only.
- `border-*-color: palette(...)` does not necessarily resolve to the grey you want (`palette(midlight)` is
  white on this machine, so the line is drawn but invisible); use a neutral translucent grey
  `rgba(127,127,127,90)` for separators, which works on both light and dark backgrounds.
- In the default light palette `palette(midlight)` is `#ffffff` (the same colour as `Base`), so using it as a
  hover background gives no feedback; use `rgba(127,127,127,40)` / `rgba(127,127,127,70)` for hover / pressed.
- `QScrollArea::setWidget()` turns on `autoFillBackground` for the scrolled widget (it paints a block in the
  `Window` colour); call `setAutoFillBackground(false)` after setWidget, or a grey block appears on a
  transparent page.
- The top-level overlay window (Backstage) follows the main window: while dragging, synchronise the overlay
  geometry in the **same call**; relying on the main window's `Move` event depends on whether the platform
  dispatches it synchronously, and when queued the two windows are one frame apart.

## Commands

- Arguments are only **carried**: `executeCommandLineAsync(line)` tokenises (`tokenizeCommandLine`, Windows C
  runtime quoting rules), the remaining tokens go into `CommandArguments` and the command reads them from
  `context.arguments()`; the meaning (positional, `--name`, missing, type) belongs to the command.
- Arguments are **not written into the command object**: the instance is default-constructed by the factory
  and destroyed after the run; the exception is binding them into a local struct at the top of `execute()`.
- History and `executing` / `executed` record `args_record = toString()`, which round-trips identically with
  `tokenizeCommandLine` ⇒ replayable.
- The instance entry `executeCommandAsync(Command*, target)` takes no arguments (the fields are the
  arguments); a blank line is rejected before dispatch.
- Argument variants use an **explicit mode token** (`size` / `corners`, plus a unique unambiguous
  abbreviation), never inference from the argument count; the mode goes into `args_record` first, so replay
  does not drift.

## Public surface

- SDK text is always `vn::String` (literals are written `u8"…"`); base's progress module uses `std::string`
  and the appfw boundary converts once with `as_std_str()`.
- Advancing a phase is called `setDone(double)`; whether it is countable is answered only by
  `std::optional<double> fraction()`.
- The cross-thread helpers live on `MainThreadDispatcher`'s static surface: `isMainThread` / `hasEventLoop` /
  `postToMainThread` / `invokeOnMainThread` / `resumeOnMainThread`; the instance carries only
  `deliverPostedCalls()`.
- `Application::d` is private now; leaves go through `dptr()`; `argv()` returns `char* const*`.

## Known gaps

- `MeshLoader::load()` / `loadMerged()` take only a path or a stream and have no `Vfs` entry ⇒ the document
  model's "`Vfs` + virtual path" cannot read a model file today.
- There is no panel registry ⇒ a panel can only be built and attached by the plugin itself; the view half
  has landed.
- At most 4 3D documents at once; more requires patching vsg (this tree keeps vsg upstream + gates).

## Translation

- Source strings in the code are **English**; Chinese lives in `<module>/res/translations/<id>_zh_CN.ts`;
  there is no `en` directory, and a missing translation falls back to English.
- A whole sentence with parameters uses `formatQt(translate("appfw", "… %1 …"), value…)`
  (`src/Format.hpp`, the same semantics as `QString::arg`); never concatenate fragments. The standard
  dialect is core's `vn::format` (`vine/Format.hpp`, `{}`, plus a String-list overload).
- `system` = the system language: on Windows it is the OS UI language (do not trust `LANG`, MSYS / Git Bash /
  a VS Code terminal carries one), on Unix it is `QLocale::system()`.
- Changing a string ⇒ run `lupdate -locations none -no-obsolete <module>/src <module>/sdk -ts <file>` in the
  same commit and add the Chinese.


## EventBus

- Admission and the stop handshake live in one type, `StopGate` (`EventBus.cpp`): `Ticket` admits a call,
  `request(Deadline)` answers `Perform` / `Completed` / `Reentrant` / `TimedOut`, and `finish(clean)` publishes the
  outcome. `shutdown()`, `shutdownGracefully()` and `~EventBus()` differ only in what they do after `Perform`.
- `Deadline::max()` means "no bound", so one `request()` and one `awaitCallers()` cover the bounded and the unbounded
  wait instead of two functions each.
- The delivery thread mode is a `SubscriptionState` property, not a channel entry's: a parked `PendingDelivery` reads
  it from the state it already holds, and the queued lambda captures only a `DeliveryTask`.
- Two locks, two facts: `Impl::channels_mutex` decides which channels exist (exclusive only for `subscribe()`), a
  channel's own mutex decides its list and is always taken while the map lock is held.
- TSan over `test_gui` (2026-11-18): 12 reports, all Qt's DBus / `QWaitCondition` teardown inside the fixture's
  `QCoreApplication` destruction, none with a frame in `EventBus.cpp`; the repo's TSan gate is graphics-scoped, so a
  clean `test_gui` TSan run is not something to expect. `scripts/asan_check.sh` is the EventBus gate.
