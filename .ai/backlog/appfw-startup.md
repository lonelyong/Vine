# Startup, next steps: startup event / showing the main window late / rendering ready first (backlog, 2026-09-26)

> **Naming evolution (later the same day)**: `beginStartup` / `finishStartup` / `runStartup(work)` in this page are what
> later became `startupStart(next)` / `startupEnd()` / `run(work)`; items 1-4 have landed, and the actual shape of all
> three is in `../design/appfw-startup-phases.md`
> (`run(work)` was later deleted: the host rewrites that beat itself and posts it to the thread pool itself).

> The direction the user set on 2026-09-26 (their words, translated): "`Application` and `GuiApplication` need a unified
> initialisation, is `init()` needed; run `exec()` first, some method pushes an event that calls `beginStartup`, or the
> startup calls a virtual `beginStartup`/`finishStartup`; the main window `show()`s only after initialisation is done,
> and for rendering the resources must be initialised before the window is shown."
>
> The previous round (`0ad0a10` + `e39373d`) landed "**creation and on-screen split apart** (`init()` only creates, never
> shows)" + the framework ordering + the host handing over work only, see the "unified startup flow" section of
> `../design/appfw-startup-splash.md`.
> This round landed item 1 (`init()` folded into the constructor, below); the remaining three go one step further on top
> of it.

## The target shape (what it looks like when 1-4 all hold)

```
main: app->run()                         the host declares configuration only, the rest is the framework's (logging is main's own initDefault)
run():  exec()                           get the loop running first (nothing is on screen before that)
        └─(startup step, posted) framework: create the report sink + stage("正在启动") + beginStartup()
                     (first-frame gate)  GUI waits for the startup frame to paint its first frame, only then may work run
                     loadAll()           plugins are loaded by the framework (AppConfig::load_plugins is on by default)
                     work()              the host's own phase (if any; runs after the plugins)
                     finishStartup()     close the frame + end the report sink (after 3: the main window shows here for the first time)
```

- **During startup the only thing on screen is the startup frame** (after 3); the main window first appears in
  `finishStartup()` ⇒ the user never sees "a window pops up, then changes shape".
- **Without a startup frame**: nothing is on screen during startup (the main window has not shown either), and the main
  window appears only in `finishStartup()`.
- **The first-frame gate keeps exactly one job**: work may not run before the startup frame has painted its first frame
  (on X11, calling only `repaint()` without dispatching just leaves it blank).
  Today that gate still means "both windows have painted" (startup frame + main window); once the main window shows
  late it degrades to "the startup frame has painted" ⇒ the two-window logic of `bootWindows()` / `allPainted()` can be
  deleted.

Order suggested: **item 1 (done) → 2 (the startup event has landed, the virtual-function part is left) → 4 → 3** (4
blocks 3).

## Item 1: is `init()` still needed → fold it into the constructor - **landed (2026-09-26)**

There is no `init()` any more: the **constructors of `Application` / `GuiApplication` do the whole initialisation**,
`setSplashConfig()` is deleted, and each builder is left with one line `make_unique<...>(config, argc, argv)`.

The shape:

- `Application(const AppConfig&, int, char**)`: set the identity (`config.name` / `config.organization`, with a fallback
  organisation) → create the managers → create the Qt application object (headless `QCoreApplication`) →
  `initialize(config)` (create the UserIO + install the config file).
  `Application(int, char**)` delegates to it with a default `AppConfig` (the entry for headless tools).
- `GuiApplication(const AppConfig&, int, char**)`: go through the base first (identity + managers), then create its own
  `QApplication` (`selectX11UnderWslg()` for WSLg runs here) → `initialize(config)` → `createWindows(config.splash)`
  (theme, startup frame, main window; **still no show**). `GuiApplication(int, char**)` delegates in the same way.
- **Why it is split in two**: a process can have only one Qt application object, and while the base constructor runs it
  does not yet know whether the leaf wants a `QCoreApplication` or a `QApplication` (a virtual call during base
  construction does not dispatch to the leaf) ⇒ the leaf creates it, stores it in the data, and then calls
  `initialize()` to finish.
- **No Qt type in the SDK**: `initialize(const AppConfig&)` takes no Qt parameter; the leaf writes the Qt object into the
  private `ApplicationData::app` (`dptr()->app = std::make_unique<QCoreApplication/QApplication>(dptr()->argc,
  dptr()->argv)`).
  An intermediate version did put `QCoreApplication*` into a protected SDK signature (and forward-declared Qt types in
  `Application.hpp`) - **withdrawn**: the core SDK (`Application.hpp`/`AppConfig.hpp`/`AppBuilder.hpp`) contains no Qt
  type at all, only the documentation mentions Qt.
- **`Application` owns the Qt application object (added 2026-09-26)**: `ApplicationData::app` used to be a raw pointer
  (assigned, never released), so "one process finishes one host and builds the next" died outright on an **asserting
  Qt** in `Q_ASSERT_X(!self, "QCoreApplication", ...)`. Qt allows **one** application object at a time, and `test_appfw`
  builds one host per case ⇒ only the first case could run (a release Qt compiles the assertion out, so the author's
  machine never showed it).
  It is a `std::unique_ptr` now, and **declared first** in `ApplicationData` ⇒ destroyed **last**: the UserIO's windows
  and the managers go first, the application object last; its destructor lets Qt drop the singleton, and the next
  `Application` can be built.
  **The price that comes with it (added the same day, 2026-09-26)**: the host **must not have static storage duration**.
  `test_vsg` kept the host in a static `unique_ptr` (created in `SetUpTestSuite()`, never released), so the Qt
  application object was destroyed during **static destruction** - by then Qt's own globals and plugin loader have gone,
  and `~QCoreApplication()` calls an address that is **no longer mapped**: `ctest` reports `test_vsg (SEGFAULT)` while
  the binary itself prints "452 tests ... [ PASSED ] 452" (it crashes after the last case); reproduced 3/3 (exit 139),
  with the stack `__run_exit_handlers → unique_ptr<Application>(s_app) → ~Application → ~ApplicationData →
  ~QCoreApplication`.
  The fix is to give that suite a `TearDownTestSuite()` that releases the host explicitly (it dies inside `main()`, while
  Qt is still whole), and the contract is written into the documentation of `Application`'s public constructor;
  `test_appfw` is unaffected (it builds the host inside the case body, so the host dies inside `main()`).
- **ABI (the conclusion of trap 1)**: `init()` was a public virtual function in the middle of the vtable ⇒ deleting it
  shifts every later slot ⇒ `VN_APPFW_PLUGIN_ABI_VERSION` **3u → 4u** (no deprecated empty slot kept).
  `beginStartup()` is declared after every other virtual function, so later additions do not need another version bump.
- **`setSplashConfig()` is deleted together with that title patch**: `BootSplash` resolves an empty title to the
  application name itself, and the identity is applied **before the windows are created** (the constructor order), so the
  builder no longer has to fill in `splash.title = config.name`.
- **A latent defect fixed on the way**: `GuiApplicationData` used to **shadow** `ApplicationData::app` with
  `QApplication* app`, so `d->app->exec()` in `Application::run()` always went through a null pointer (it only failed to
  crash because `QCoreApplication::exec()` is a static member).
  With the two merged into one (the base's `QCoreApplication* app`), the GUI path really stores it too.
- `AppConfig`/`SplashConfig` move to `sdk/vine/appfw/AppConfig.hpp` (`AppBuilder.hpp` includes it instead, the old include
  path still works); `AppBuilderSupport.hpp` and `applyAppConfig()` are deleted, their logic goes into `Application::initialize()`. <!-- drift-ok -->
- The nails (they go red): `GuiApplicationConstructionTest.TheBuilderReturnsAFinishedApplication` and
  `BootSplashTest.AnEmptyTitleShowsTheApplicationName` in `test_gui`, and
  `VsgBackendPluginTest.TheConstructorBuildsTheApplicationFromItsConfig` in `test_vsg` (`persist_config=false` ⇒
  `configFile()` is empty).
- Verified this round: `ninja` green in both the Debug and Release trees; `test_gui` 219 (217+2), `test_vsg` 452 (451+1)
  and `test_asyncqt` 2 all green; the startup log of the real `bin/Vine` identical to before the change
  (`startup work starting 10 ms …`, `Pending -> Attached -> Presenting`).

## Item 2: startup phases advanced by "a startup event + virtual functions" - **landed (2026-09-26)**

The shape (see "lifecycle" in `../design/appfw-startup-splash.md`): `run()` calls `exec()` first and then pushes a
startup step over `Qt::QueuedConnection` (the two lines in `run()`: create the report sink + `stage("正在启动")`, then hand
the screen to `beginStartup(work)`'s callback), so the startup phases of both the headless and the windowed application
run inside the loop (headless startup work used to run before `exec()`). Plugin loading came in as well:
`AppConfig::load_plugins` (on by default) ⇒ the host's `main` no longer writes `loadAll()`, and it runs before the host's
work.
The nail: in `test_appfw`, "a frame callback posted before startup, and work takes a snapshot showing it has already
happened" ⇒ the mutation (calling the startup step directly before `exec()`) goes red.

**The two extension points were renamed and there are still two** (the original plan was one `beginStartup()`):
`showUserInterface()` → `beginStartup()` (it only puts the startup frame on screen, while "the user interface" - the main
window - belongs to `finishStartup()`: the old name lied), and `whenUserInterfaceIsUp()` → `beginStartup()` (it waits for
the startup frame's first frame, not for "the interface is ready").
Why they cannot merge into one `beginStartup()`: **only the windowed layer can answer "is it really on screen"** - the
base `Application` lives in the core SDK and cannot name `gui::Window` (not even a Qt type may appear in
`Application.hpp`), so "wait for the first frame" has to be a virtual function implemented by the gui layer. The division
is therefore: `beginStartup()` lays out the screen (headless: empty), `beginStartup(then)` reports whether it really made
it on screen (headless: `then()` right away), and the framework only owns the order and `finishStartup()`.
(Removing or renaming these two protected virtual functions does **not** need a bump of
`VN_APPFW_PLUGIN_ABI_VERSION`: they sit at the **end** of the vtable and are unreachable for a plugin (protected), while
the slots a plugin can touch, `finishStartup()`/`run()`, are unchanged.)

- Three ways to trigger it, none adding a dependency: `QTimer::singleShot(0, d->app, …)` (what the GUI uses today, the
  least fuss) / `QMetaObject::invokeMethod(d->app, …, Qt::QueuedConnection)` / a custom `QEvent` (which needs a private
  QObject to receive it, in exchange for "a named event that `removePostedEvents` can revoke").
- ⚠️ **The key trap**: a posted user event and the repaint request Qt posts itself (`QEvent::UpdateRequest`) are
  **dispatched FIFO in the same pass** ⇒ the event precedes the paint ⇒ "push the startup step first" **cannot** replace
  "wait for the first frame". That gate must be satisfied **before the event is posted** (on the GUI side: post the
  startup event from the first-frame signal or from a deadline).
- Virtual functions: `finishStartup()` is already virtual ✓. Add `virtual void beginStartup()` (empty by default; GUI =
  show the startup frame).
  **Do not add** `startup()` just for symmetry: the host's work is better served by the existing `runStartup(work)`
  callback (`main.cpp` is not an `Application` subclass).
  If it ever really has to become OO (`class VineApp : public GuiApplication { void onStartup() override; }`), make the
  default implementation of `beginStartup()` "call the registered callback"; there is no need to do that now.
- Where it lives (changed on 2026-09-26): **the startup report sink is created by `Application::run()`** (`stage("正在启动")`,
  see "the report sink belongs to the startup phase" in `../design/appfw-startup-splash.md`), and the "create the report
  sink + report 'initialising the interface'" lines in the `GuiApplication` constructor are gone. So when
  `beginStartup()` lands, those two lines ("create the report sink + report the framework's first phase") move from
  `run()` into `beginStartup()` (the GUI also shows the startup frame there), and `GuiApplication`'s window creation
  needs no further change.

## Item 3: the main window `show()`s after initialisation completes - **landed (2026-09-26, verified on X11)**

The shape: `beginStartup()` **only puts the startup frame on screen** (with no frame nothing is put on screen); the
first-frame gate waits for the startup frame only (the two-window logic of `bootWindows()`/`allPainted()` is deleted, and
with no frame it simply continues); `finishStartup()` `show()`s the main window for the first time (with a frame, after
`raise()`/`activate()` first).

- The precondition is that **the X11 half of item 4 holds** (below): "a hidden embedded-surface window has a usable
  handle" has been measured on WSLg/X11.
- Hard evidence (read back from the X server, `map_state`): during startup only the startup frame is `IsViewable` while
  the main window's `800x600` is still **IsUnmapped**; after `finishStartup()` the startup frame disappears and the main
  window (frame `864x664`) is `IsViewable`. On the log side: `surface Pending -> Attached` happens while the main window
  is not shown, and `Attached -> Presenting` immediately follows the main window appearing. The details and the full log
  are in the "only the startup frame during startup" section of `../design/appfw-startup-splash.md`.
- ⚠️ **Windows is unverified** (the other half of item 4): if `VsgHostWindow` cannot get a valid HWND there ⇒
  `GetClientRect(..) failed` + `surface -> Failed`, then today's shape has to come back (show the main window early,
  gate on both windows) and "not adopted on Windows" gets written down.
  Even if the early attach fails, showing the main window triggers a layout/resize and the control retries the attach
  itself (`handleUpdate()`).

## Item 4 (a prerequisite, blocks 3): rendering resources ready first, the window shown later - **the X11 half is done (2026-09-26)**

The current state (`.ai/design/appfw-render-surface.md`):

- **attach needs only "a handle + a size"** (the surface does not have to be visible); **present** is what requires the
  window to be on screen.
- **The visibility of the container control IS the "reveal switch"**: `RenderControl` keeps the window container hidden
  until `state_changed` reports `Presenting` and only then calls `setVisible(true)` ⇒ "showing the main window late" does
  not expose a hole either (that cell is the main window's own background).

Probe result (WSLg/X11): **"a top-level window that has been created but not shown" does not affect attach**, because
`SurfaceWindow::nativeHandle()` uses the **`winId()` of the separate `QWindow` inside the container**, not the
top-level's handle; while the top level is not shown it still has a valid handle
(measured `[VsgHostWindow] attached to the host window 0x60002a (160x160, mapped=true)`). The historical failures came
from the old renderer taking the top-level handle plus nobody creating a native window, two things stacking up; they are
not a property of the current shape.

Still to do: **the same probe on the Windows side** (HWND + client rect). The criterion: `attached to the host window
0x…` and `surface Pending -> Attached` appear in the startup log (rather than `GetClientRect(..) failed` / `-> Failed`).
- ~~`VSG_MAX_DEVICES=1` in the Release tree is a deliberate tripwire (nothing in the repository sets it) ⇒ both trees
  have to run.~~
  **Void as of 2026-09-27**: `gfx_backend_vsg/CMakeLists.txt` now sets it to 4 explicitly (one render surface per
  document view), so that difference between the two trees is gone; the practice of "run the gate in both trees" stays.

Deliverables:

1. a case that pins "with the main window not shown, `RenderControl::init()` can create the device/pipeline (returning
   true, or a clearly retryable false), and the first frame arrives after the show";
2. change the attach precondition of `SurfaceWindow` from "the host control is visible" to "a handle + a size > 0" (if
   that is already the case, add the case and align the documentation);
3. a visual confirmation on real hardware (a Windows desktop): during startup **only the startup frame is on screen**, and
   the main window appears with its content when it ends.

## Measurements and hard constraints not to forget

1. A window only counts as "on screen" after it has been **dispatched to its first frame** (on X11 a bare `repaint()` is
   a no-op) ⇒ the only window on screen during startup (the startup frame) still has to pass the first-frame gate.
2. ~~"on screen before the startup work" is not an ordering preference: **a render surface's attach needs the window's
   handle**, and today that exists only after a show~~
   **Overturned (2026-09-26, X11)**: the handle comes from the `winId()` of the separate `QWindow` inside the container,
   and the top level need not be shown.
   What still holds is: **plugin loading and the host's work must wait until "the startup frame has painted its first
   frame"** (on X11, showing without dispatching leaves an empty window).
3. User events and repaint requests share one FIFO pass ⇒ "push the startup step first" ≠ "wait for the first frame".
4. ~~`init()` sits in the middle of the vtable ⇒ deleting it either keeps the slot or bumps the ABI~~ **Decided**: delete
   it and bump the ABI **3u → 4u** (item 1).
5. The test application's windows (which never run the loop) get on screen through the idempotent `finishStartup()`; once
   `finishStartup()` means "close the frame + show the main window for the first time", that path has to be re-checked
   with it (`BootSplashTest.DisabledByDefaultInTheTestApplication` in `test_gui`).
6. The probe: **`scripts/xwinmap.py` (committed)** - lists the name/size/`map_state` of the root's top-level windows, and
   "only the startup frame is on screen during startup" is read with it (once during startup, once after it ends). It
   replaces the earlier `/tmp` copies of `xstack.py`/`xfirst.py` (which get cleaned up). ⚠️ In a hand-written
   `XWindowAttributes` layout `map_state` sits at offset **92**, not 100 (getting it wrong makes every window look like
   IsUnmapped).

## Acceptance (the criteria once all four are done)

- **Behaviour**: an independent X read-back during startup shows **only the startup frame** (the main window is not in
  `XQueryTree`, or `isVisible()==false`); the log shows `startup work starting … : the startup frame is on screen`; after
  startup the main window appears with `painted ≈ 83.5%`.
- **"Black first, then painted" is gone for good**: the first time the main window appears on screen it already has
  content (the render area at least shows the window background left by the hidden container, not a hole).
- `test_gui` (now 219, item 1 added 2 cases) / `test_vsg` (now 452, item 1 added 1) green;
  `vsg_rewrite_gate.sh` in both trees → `cases=452 failed=0 vuid=0 hazard=0`
  + the application-phase pixels matching the baseline; `check_doc_symbols.py` / `check_diagnostic_formats.py` /
  `check_include_hygiene.py` green; one real-hardware run after `cmake --install build --prefix dist`.
- **The counter-proof**: removing the new gate/event has to reproduce a failure such as "the main window is visible
  during startup but `painted=0.0%`"
  (proving the new mechanism carries the load rather than coincidence; this round proved the unified gate still does
  exactly that way).
