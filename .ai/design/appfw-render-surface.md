# appfw's render-surface lifetime

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: the native surface, attach, state and platform fallback of `RenderControl` / `SurfaceWindow`; the graphics layer's surface / target / device contracts are in `graphics-architecture.md`.
> **Sources**: `RenderControl.hpp`, `src/gui/SurfaceWindow.{hpp,cpp}`, `tests/test_gui/RenderControlTest.cpp`.
> **Backlog**: `../backlog/appfw-startup.md`

## 1. Overview

### 1.1 Problem and background

A 3D area has to answer four questions at once: when does the native window have a handle, when can it be attached, when can it be shown, and what happens after the window is recreated.
Each of them needs a stated rule rather than a timer — a moment guessed by a retry ladder shows up as an empty window, a hole in the layout, or a first frame that never arrives.

### 1.2 Goals

- Construction only assembles; attaching has an explicit entry, so the host can place it in its own order.
- Attaching does not require the surface to be visible; the attach renders the first frame itself, and the area stays hidden until then.
- After the native window is recreated the session survives and the host does nothing.
- A failure has a state and a reason and the host may retry; the control arms no timers.

### 1.3 Non-goals

- No layout policy inside the control; the size comes from the container or a resize event.
- No retrying on the host's behalf; there is no 100 ms × N attach ladder.
- No promise that every platform can attach to an unshown window (Wayland falls back to "show first, then attach").

### 1.4 One-paragraph conclusion

`RenderControl`'s constructor only assembles; `init()` / `initAsync()` / the first-turn fallback performs one synchronous attach; the state is only `Pending → Presenting` (a failure is `Failed`), the container is hidden until the first frame, and the control follows a recreated window itself.

## 2. Design Points

### 2.1 File split

| File | Responsibility |
| --- | --- |
| `sdk/vine/appfw/gui/RenderControl.hpp` / `src/gui/RenderControl.cpp` | the public control: forwards `init()/initAsync()`, `state()`, `failureReason()`, size / ratio, `viewer()`, `view()`, `engine()`, `renderFrame()`, `fitToScreen()`, `setContinuousRendering()` |
| `src/gui/SurfaceWindow.{hpp,cpp}` | the private implementation: the native `QWindow`, the `RenderEngine`, the `SceneView`, the `Viewer`, the attach rules, deferred resize, Qt events → view input |
| the container of `SurfaceWindow` | `QWidget::createWindowContainer` wraps the native `QWindow`; the container is hidden until the first frame |

### 2.2 Construction only assembles

When `new RenderControl()` returns: the engine, view, viewer and diagnostic routing exist; **no backend is chosen and nothing is attached**; `state()==Pending`, `engine()->device()==nullptr`, and there is no pass or chain.
That way `engine()->setDevice()`, `viewer()->setViewerOptions()` (or the host's own `view()->setChain()`) and the content assembly all run synchronously before the first attach, with **zero rebuilds**.

### 2.3 The two attach entries

- `init()`: synchronous and idempotent; if the session is already bound to the current handle it returns `true`, otherwise it performs the full attach.
- `initAsync()`: `co_return init()`, which **does not suspend** and pushes no part onto a pool; it is only a shape for coroutine callers.
  Criterion: `RenderControlTest.InitAsyncCompletesWithoutSuspending`.

When neither is called, the first-turn `QTimer::singleShot(0)` fallback attaches; the fallback uses the backend the host just installed, not the registry default.

### 2.4 The attach sequence

1. **Make sure the surface has something to present first**: if the view has no chain, build a default chain from `ViewerOptions` and adopt it, then declare it on the `RenderArea`; if the host already declared its own pass, this step does nothing.
2. **Fill in the default backend**: `useDefaultBackend()` asks `chooseBackend` (`BackendChoice.hpp`) which backend to bring up, and does so only when the engine has no backend, so a backend registered later is still picked up by the next attach. The rule is: the name the application stated, otherwise the ONLY registered backend, otherwise a refusal that names the candidates. It never takes the first entry - `entries()` is ordered by name, so that was a choice made by the alphabet (F6).
3. **No backend** ⇒ `failAttach("no render backend is registered")` and the state becomes `Failed`.
4. **Empty handle or degenerate size** ⇒ one log line, stay `Pending`, and wait for show / resize / the next `activate()`; the control arms no timer.
5. **Wait for a frame first**: `engine->waitForFrame()` — the verbs below change the session the engine thread is recording into (target, swapchain, device), so the attach has to happen between frames (`.ai/design/graphics-threading.md` §2.12). In inline mode this is a no-op; when threaded it usually waits for the engine's own previous frame, at the cost of one "already done" check.
6. **Attach**: `setWindowHandle` / `createSurfaceTarget` → device / session / pipeline `initialize()` → `resize(w,h)` plus handing the size to the surface → `renderFrame()` (this frame pays for the pass graph, program slots and compilation, and the state becomes `Presenting`).

Attaching does not require the surface to be visible; only presenting requires the window to be on screen.
When the platform disagrees, the attach fails and the control falls back to "show the surface, then attach".

**Losing the surface mid-frame (`Lost`) neither tears down the session nor hides the area**: a frame that did not reach the surface means "this surface wants a rebuild", not "the session is being replaced".
The fixing side is the engine — it forgets the size it announced, re-announces the same size on the next frame, and the backend rebuilds the swapchain from it (counting one device wait); the host side only does two things: record the answer once and say it once (the frame number in `presentRecord()` guarantees one answer is handled once), and go through attach when **the handle really changed or became null** (`renderFrame()` already does that at its head).
Leaving `state()` alone is deliberate: `Presenting` means "there is a frame in the surface", and **the last image is still on screen**; hiding it would also stop the very frames that fix it (a hidden window requests no frames), turning a recoverable swapchain into a dead one.
When threaded, the answer arrives one frame late, so it reads "the last frame that was answered".

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Construction only assembles; attaching has an entry | `init()` / `initAsync()` / the first-turn fallback | landed |
| F2 | State and visibility | `Pending` / `Presenting` / `Failed`; the container is hidden until the first frame | landed |
| F3 | Native window recreation | the new handle is re-announced and the session survives | landed |
| F4 | Failure and retry | a refusal ⇒ false + `Pending`; no backend ⇒ `Failed`; the host may retry | landed |
| F5 | Platform fallback | X11/WSLg attach while unshown; Wayland shows first, then attaches | landed (unverified on Windows) |
| F6 | The application names its backend | `RenderControl::setDefaultBackendName`; the attach creates that name, or refuses and names the candidates | landed |

### 3.1.1 F1 Construction only assembles; attaching has an entry

- **Behaviour**: the constructor builds the widget + engine + view + container; `init()` attaches synchronously, `initAsync()` is the coroutine shape; with no explicit call the first turn attaches.
- **Constraints** (C1): attaching needs a handle and a size; with an empty handle or a degenerate size the state stays `Pending` and no delay is guessed.
- **Requirements** (R1): configuration done before the attach means zero rebuilds; the control arms no retry timer.
- **Acceptance** (T1): `ConstructionOnlyComposesAndTheFirstTurnAttaches`.
- **Caveat** (N1): `initAsync()` does not go to a pool; the device, session, pipeline and warm-up all run on the application thread.

### 3.1.2 F2 State and visibility

- **Behaviour**: `Pending` (no usable surface, or nothing rendered yet) / `Presenting` (a frame is in the surface) / `Failed` (the backend will not start).
  The container stays hidden until `state_changed` reports `Presenting`.
  **`Presenting` means "a frame really reached the surface"**: inline, a queued frame is answered at once, so it holds at the moment of submission; when threaded, the first one `waitForFrame()`s for that first frame (whether the area may be revealed depends on it), and every later turn only reads the previous frame's `presentRecord()` without waiting.
  A `Lost` frame **does not change the state** (see §2.4): what fixes it is the engine re-announcing the size, and hiding the area would stop the repair.
- **Constraints** (C2): no session means no frame; `failureReason()` is non-empty ⟺ `Failed`; `Presenting` only after an answer that says `Presented`.
- **Requirements** (R2): what shows before `Presenting` is the main window's background, not a hole; `renderFrame()` / `fitToScreen()` must not push the state before the attach.
- **Acceptance** (T2): `TheFrameTheAttachRendersPutsTheSurfaceOnScreen`, `AFrameAskedForBeforeAnySessionDoesNotPutTheSurfaceOnScreen`.
- **Caveat** (N2): an unshown native window is a hole in Qt (the window container punches its own rectangle transparent), so hiding it is correctness.

### 3.1.3 F3 Native window recreation

- **Behaviour**: when Qt destroys and recreates the platform window (a dock drag, a screen change, a `setCentralWidget()` reparent), the new handle is picked up and re-announced to the backend; the session, device, caches and compiled pipelines stay, and the state goes through `Pending → Presenting` again.
- **Constraints** (C3): a rebuild replaces the surface, not the session; the host does not call `init()` again.
- **Requirements** (R3): during the rebuild the state drops to `Pending` and the area is hidden; it is shown again once the first frame returns.
- **Acceptance** (T3): `FollowsARecreatedSurfaceWithoutTheHost`.
- **Caveat** (N3): reparenting recreates the native window, so the central area holds one container installed once and switches pages inside it.

### 3.1.4 F4 Failure and retry

- **Behaviour**: the backend refuses ⇒ `init()` returns false and the state stays `Pending`; no backend is registered ⇒ `Failed` plus a reason; the host calling `init()` again is the retry.
- **Constraints** (C4): the control retries nothing in the background; the failure reason is only valid in `Failed`, and a successful attach clears the old verdict.
- **Requirements** (R4): both a refusal and a failure must be observable (return value / state / reason) and never silent.
- **Acceptance** (T4): `RefusedAttachReportsFalseAndWaitsForTheHost`, `HostCanRetryAfterTheBackendRefused`, `ReportsFailedWhenNoRenderBackendIsRegistered`.
- **Caveat** (N4): a backend registered later is picked up by the next attach, so do not declare it dead in the constructor.

### 3.1.5 F5 Platform fallback

- **Behaviour**: on X11/WSLg the standalone `QWindow` inside the container already has a valid handle while the main window is unshown, so the attach succeeds; on Wayland an unmapped surface has no extent, so `init()` returns false and the host calls it again after showing the surface.
- **Constraints** (C5): attaching needs only a handle and a size; presenting needs the window on screen.
- **Requirements** (R5): when the platform disagrees the fallback is "show first, then attach", which is never worse.
- **Acceptance** (T5): `scripts/xwinmap.py` reads `map_state` at startup; the Windows criterion is in the backlog.
- **Caveat** (N5): the `map_state` field offset is 92, not 100; the Windows HWND + client rect has still not been measured.

### 3.1.6 F6 The application names its backend

- **Behaviour**: a host states one name (`RenderControl::setDefaultBackendName`) and every attach that finds its engine without a device creates THAT backend by name (`RenderBackendRegistry::create`). No name stated: the attach takes the only registered backend. Several registered and none named: the attach fails and the reason lists them. A stated name that is not registered: the attach fails and the reason names it and what IS registered.
- **Constraints** (C6): the engine's own device always wins; the registry's name order is never a decision; a stated name is never replaced by another registered backend.
- **Requirements** (R6): the choice is process-wide (a view creates its own control, so a per-control setting could not serve "this application runs on X") and made before the first attach; every refusal is actionable from its message alone.
- **Acceptance** (T6): `BackendChoiceTest` (the rule, with neither a registry nor a window) and `AStatedBackendThatIsNotRegisteredFailsTheAttachAndNamesIt` (the reason reaches the host).
- **Caveat** (N6): `VN_RENDER_BACKEND` is the app shell's override, not appfw's - the framework reads no environment variable of its own.

## 4. Public Interface

### 4.1 `RenderControl`

| Interface | Contract | Note |
| --- | --- | --- |
| `init()` | synchronous, idempotent attach | session already bound to the current handle ⇒ true at once |
| `initAsync()` | `co_return init()`, never suspends | the coroutine caller's shape |
| `state()` | `Pending` / `Presenting` / `Failed` | see F2 |
| `failureReason()` | non-empty ⟺ `Failed` | cleared by a successful attach |
| `engine()` / `view()` / `viewer()` / `surface()` | borrowed, the control owns them | `view()` is null after `release` |
| `renderFrame()` / `fitToScreen()` | renders one frame synchronously / frames the content | pushes no `Presenting` before the attach |
| `requestFrame()` | requests a frame: draws in the same turn when shown, coalesced, nothing when not on screen | the default shape after a state change |
| `setContinuousRendering(on, interval_ms)` | continuous frames | on demand by default |
| `defaultBackendName()` / `setDefaultBackendName(name)` | static, process-wide; read by every attach whose engine has no device | empty = "no name stated"; unsynchronized, call it at startup (F6) |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | Attaching needs a handle and a size; otherwise the state stays `Pending`. |
| C2 | No session means no frame; `failureReason()` is non-empty ⟺ `Failed`. |
| C3 | A rebuild replaces the surface, not the session; the host does not call `init()` again. |
| C4 | The control arms no retry timer; a failure is observable. |
| C5 | Attaching does not require visibility; presenting requires the window to be on screen. |
| C6 | The engine's device always wins; the registry's name order never decides which backend attaches; a stated name is never replaced by another backend. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | Configured before the attach = zero rebuilds. |
| R2 | The area stays hidden before `Presenting`, showing the main window's background. |
| R3 | During a rebuild the state drops to `Pending` and returns to `Presenting` after the first frame. |
| R4 | A refusal or failure must be observable; a backend registered later is picked up by the next attach. |
| R5 | When the platform disagrees, fall back to "show first, then attach". |
| R6 | The backend choice is process-wide and stated before the first attach; a refusal says what is registered and how to choose. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | When the constructor returns, `state()==Pending`, `device()==nullptr` and there is no chain. |
| N2 | `Presenting` ⟺ a frame is already in the surface. |
| N3 | Recreating the native window destroys no session, device or compiled pipeline. |
| N4 | The control owns nothing beyond the window container's visibility policy; the size comes from a resize event. |

## 6. Caveats and Traps

### 6.1 An unpresented native window is a hole

- How it fails: the container is visible with no frame yet, and the desktop shows through.
- How to detect it: while `state()!=Presenting` the container must be hidden.
- How to prevent it: `setVisible(true)` only once `state_changed` reports `Presenting`.

### 6.2 Do not pump events for the render component during startup

- How it fails: `processEvents()` also runs other components' timers and events, and the embedded surface is woken early.
- How to detect it: the attach happens before the layout settles, so no valid size or handle is available.
- How to prevent it: the startup box only calls `repaint()`; the first-frame door uses `Window::hasPainted()` plus a 300 ms fallback.

### 6.3 Reparenting recreates the native window

- How it fails: one central widget per document, so switching pages or dragging rebuilds the session and the image flickers.
- How to detect it: the state repeatedly goes `Pending → Presenting` and the device count climbs.
- How to prevent it: the central area holds one container installed once and switches pages inside it with a `QStackedWidget`.

## 7. Rejected Options

| # | Option | Reason | ADR |
| --- | --- | --- | --- |
| D1 | Creating or inserting the `RenderControl` later, once "ready" | Self-locking: a control that is not in a shown window's layout has no usable surface. | — |
| D2 | Setting the size first, then attaching, then embedding | A degenerate size (1x1) is already enough to attach, and embedding first then calling `init()` is simpler. | — |
| D3 | Showing the `QWindow` later, once the engine is ready | The causality is reversed: a ready engine needs the window to exist, and presenting needs it mapped. | — |
| D4 | Painting a background behind the `QWindow` as a placeholder | What an unpresented native window reveals depends on the platform and the compositor. | — |
| D5 | A 100 ms × N attach retry ladder, or settle frames | The first attach is the host's `init()`; after that, window events drive it. | — |
| D6 | A separate `Attached` state | It never persists through a synchronous attach, and no reader distinguishes it from `Pending`. | — |
| D7 | Taking the registry's first entry as the default backend | `entries()` is ordered by NAME, so the renderer was chosen by the alphabet and a plugin changed it silently (`G4`). | — |
| D8 | A per-control backend name | A view creates its own control, so "this application runs on X" is one process-wide statement - a static pair on `RenderControl`, not a property each view would have to repeat (F6). | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | construction only assembles; the first turn attaches to the backend the host just installed | `ConstructionOnlyComposesAndTheFirstTurnAttaches` |
| T2 | the frame the attach renders reaches `Presenting`; no state push before the attach | `TheFrameTheAttachRendersPutsTheSurfaceOnScreen`, `AFrameAskedForBeforeAnySessionDoesNotPutTheSurfaceOnScreen` |
| T3 | the session survives a native window rebuild | `FollowsARecreatedSurfaceWithoutTheHost` |
| T4 | a refusal ⇒ false + `Pending`; no backend ⇒ `Failed`; retryable | `RefusedAttachReportsFalseAndWaitsForTheHost`, `HostCanRetryAfterTheBackendRefused`, `ReportsFailedWhenNoRenderBackendIsRegistered` |
| T5 | `init()` right after embedding, without waiting for the layout | `HostCanAttachRightAfterEmbeddingBeforeTheLayoutSettles` |
| T6 | the stated backend is the one created; several registered without a name is a refusal, not a guess | `BackendChoiceTest`, `AStatedBackendThatIsNotRegisteredFailsTheAttachAndNamesIt` |

## 9. Status and Evolution

### 9.1 Current status

F1–F6 are in place (attaching to an unshown window on Windows is still unmeasured).
The attach is synchronous, the state machine has three states, and the container is hidden until the first frame.
The backend is the one the application names (`RenderControl::setDefaultBackendName`); the registry only answers what is installed.

### 9.2 Known gaps / backlog

- Windows: whether the host's window gets a valid HWND while the main window is unshown; see `../backlog/appfw-startup.md`.
- The session init still runs on the application thread (~184 ms); moving it to a pool needs the concurrency of attach and content assembly redesigned, which is not being done now.

### 9.3 Compatibility / migration

There is no `SurfaceState::Attached`: a caller looks at `Pending` / `Presenting` / `Failed` only.
There is no `isAttaching()`, no `AttachScope` and no settle frame.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/gui/RenderControl.hpp`
- `src/fw/appfw/src/gui/SurfaceWindow.{hpp,cpp}`
- `src/fw/appfw/src/gui/BackendChoice.hpp`
- `tests/test_gui/RenderControlTest.cpp`
- `tests/test_gui/BackendChoiceTest.cpp`

### 10.2 Documents and ADRs

- `appfw-render-view.md`
- `appfw-startup-splash.md`
- `graphics-architecture.md`
- `../backlog/appfw-startup.md`

### 10.3 External material

- The native-window semantics of Qt's `QWidget::createWindowContainer`.
- X11 `XQueryTree` / `XGetWindowAttributes` (`scripts/xwinmap.py`).
