# appfw's splash and startup progress (`BootSplash` / `StartupProgress`)

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: the splash, startup progress and its presentation; the three beats, the thread contract and the failure / cancellation paths are in `appfw-startup-phases.md`.
> **Sources**: `StartupProgress.hpp`, `gui/BootSplash.hpp`, `AppConfig.hpp`, `tests/test_gui/BootSplashTest`.
> **Backlog**: `../backlog/appfw-startup.md`

## 1. Overview

### 1.1 Problem and background

Startup had no progress to look at: `GuiApplication`'s constructor builds the main window (Ribbon, docked panels), plugin loading runs on the application thread, and the user
faces "a window that is not finished yet", with no way to see how fast loading is going. The splash must show the application's identity and the current phase without
pretending to know a total.

### 1.2 Goals

- The application configures appearance only (`AppConfig::splash`) and writes no presentation code.
- During startup only the splash is shown; the main window first appears in `startupEnd()`, so the user never sees "a window pops up and then changes shape".
- Progress updates per phase, with no percentage and no progress bar; a status line + an 18 px spinner.
- The reporting door is application-level state, shared by the status bar, the console and the splash.

### 1.3 Non-goals

- No overall ETA, progress bar or percentage.
- No animation or heartbeat inside a phase; the session `init()` is not cut apart and the startup does not turn the loop.
- The framework does not own a startup thread for the host; the host puts its heavy work on the pool itself, in the `startup()` beat.

### 1.4 One-paragraph conclusion

The splash is framework-level presentation: `Application::startupSequence()` opens and closes the reporting door, `GuiApplication::startupStart()` puts the splash up and
waits for its first frame, and `startupEnd()` takes it down and shows the main window for the first time; progress is pushed per phase, and the splash shows a status line
and a spinner only.

## 2. Design Points

### 2.1 The pieces

| Piece | Location | Responsibility |
| --- | --- | --- |
| `SplashConfig` | `AppConfig::splash` (`AppConfig.hpp`) | the switch + title / subtitle / logo |
| `StartupProgress` | `sdk/vine/appfw/StartupProgress.hpp` | the startup reporting door: phase, status text, fraction; at most one per process |
| `gui::BootSplash` | `sdk/vine/appfw/gui/BootSplash.hpp` | a borderless, self-drawn rounded box + status line + spinner |
| `Application` | `startupSequence()` drives the three beats; opens / closes the reporting door | lifetime |
| `GuiApplication` | the constructor builds the splash; `startupStart()` / `startupEnd()` override to show / take it down | presentation |
| `PluginManager::loadAllAsync()` | reports "finding / loading / finishing plugin x (i/n)" | the longest and least predictable part of startup |

**Why the reporting door is in appfw and not in base**: it reports into a `ProgressHost` (Core's `Signal`), and `ProgressHost` is application state. The door holds a
foreground `ProgressHost` of its own, so startup progress is naturally shared by the status bar and the console.

### 2.2 The two kinds of phase

- **A countable phase**, `stage(name, total)`: `setDone()` advances by `done/total` (plugin loading works this way, one share per plugin coming back).
- **An indeterminate phase**, `stage(name)`: only what is being done is known.
- **Every countable phase starts at zero** (it is that phase's own fraction), not what is left of the whole startup. A reporter cannot know how long phases it has not
  reached will take; forcing a global percentage either jumps back to 0 at every phase change or makes the application declare a table of weights up front.
- The splash does not turn a countable phase into a number; the status-bar presenter and the console show them as before.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Splash presentation | application identity + status line + spinner; no progress bar, no percentage | landed |
| F2 | Phase progress | one report at the start of each phase, repainting on every report | landed |
| F3 | The first-frame door | startup work runs only after the splash has painted its first frame; a 300 ms fallback | landed |
| F4 | The main window appears | `startupEnd()` takes the splash down, the main window `show()`s for the first time, `raise()` / `activate()` | landed |
| F5 | Cancellation | the splash' `✕` / `startupStart`'s token; cancellation is not a failure | landed |
| F6 | The reporting door's lifetime | opened before the first beat, closed after the last; every call is a no-op without it | landed |

### 3.1.1 F1 Splash presentation

- **Behaviour**: `SplashConfig` sets the switch / title / subtitle / logo; `BootSplash` draws a rounded panel itself, with a status line and an 18 px spinner.
- **Constraints** (C1): no percentage or progress bar; an empty title falls back to the application name; an unreadable logo counts as "no logo".
- **Requirements** (R1): follow the light and dark themes; centred on the primary screen; no change to appfw's existing widget styles.
- **Acceptance** (T1): `BootSplashTest`'s `statusText()` assertions.
- **Caveat** (N1): the status text is `elidedText` to the box's width, so a long plugin name cannot widen the window.

### 3.1.2 F2 Phase progress

- **Behaviour**: each phase reports once at its start ("starting" → "showing the splash" → "finding / loading / finishing plugins" → "preparing the main window");
  the splash `repaint()`s on every report.
- **Constraints** (C2): nothing animates inside a phase; no heartbeat and no interpolation over time.
- **Requirements** (R2): a phase boundary must always be visible; the status bar, the console and the splash read the same `StartupProgress`.
- **Acceptance** (T2): `BootSplashTest`'s `statusText()` follows `stage()` / `setLabel()`.
- **Caveat** (N2): animating inside a phase would need cutting the session `init()` apart or turning the loop during startup, and both cost more than "phase boundaries are
  visible".

### 3.1.3 F3 The first-frame door

- **Behaviour**: `startupStart()` puts the splash up and `co_await`s its first frame (`Window::hasPainted()` + a 300 ms fallback) before going on; on X11 a
  `repaint()` / `show()` without dispatching leaves an empty window.
- **Constraints** (C3): startup work (including a plugin's `load()`) must come after the splash has painted its first frame; the main window has not been shown yet.
- **Requirements** (R3): without a splash, continue immediately; the first-frame door must not become a fixed sleep.
- **Acceptance** (T3): `test_appfw`'s startup-work case; `startup work starting N ms ... the startup frame is on screen`.
- **Caveat** (N3): during startup do not `processEvents()` on the renderer's behalf; only let the splash repaint itself.

### 3.1.4 F4 The main window appears

- **Behaviour**: `startupEnd()` takes the splash down and `show()`s the main window for the first time; with a splash, `raise()` / `activate()` first.
- **Constraints** (C4): the main window is not shown before `startupEnd()`; during startup the screen holds the splash only.
- **Requirements** (R4): the main window's first appearance is already finished; the render area's container is hidden until `Presenting`, showing the window background.
- **Acceptance** (T4): on X11, `scripts/xwinmap.py` reads the main window as `IsUnmapped` during startup and `IsViewable` after it.
- **Caveat** (N4): `Qt::SplashScreen` stays on top and does not activate the process; Windows only gives foreground activation to a new process, so `raise()` /
  `activate()` is required.

### 3.1.5 F5 Cancellation

- **Behaviour**: the `✕` in the splash' top-right corner writes to `StartupProgress::stopToken()`; the framework looks at it after every beat and before every plugin.
- **Constraints** (C5): cancellation is not a failure; `cancelStartup()` unloads the loaded plugins + closes the reporting door + `exit(0)`, and the main window never appears.
- **Requirements** (R5): a plugin takes the same token from `PluginLoadContext::stopToken()`; one outside startup gets a token that never stops.
- **Acceptance** (T5): `test_appfw`'s cancellation cases.
- **Caveat** (N5): a cancellation goes through neither `startupEnd()` nor `failStartup()`.

### 3.1.6 F6 The reporting door's lifetime

- **Behaviour**: `beginStartupProgress()` before the first beat, `endStartupProgress()` after the last; the `startupEnd()` beat can still report.
- **Constraints** (C6): not built in the constructor — a process that never runs a startup would keep a live host, `isBusy()` would always be true and every top-level command
  would be refused.
- **Requirements** (R6): without the door every call is a no-op; destroying it is deliberate (not just `complete()`).
- **Acceptance** (T6): `test_appfw`'s headless startup-progress case.
- **Caveat** (N6): a live host stays on the foreground stack and blocks the progress of every command after it.

## 4. Public Interface

### 4.1 `StartupProgress`

| Interface | Contract | Notes |
| --- | --- | --- |
| `stage(name)` / `stage(name, total)` | start an indeterminate / countable phase | every countable phase starts at zero |
| `setDone(double)` | advance the current phase | the value is absolute, not a delta |
| `setLabel(String)` | change the status text | used for the per-plugin reports |
| `fraction()` | `optional<double>` | whether a phase is countable is read from it alone |
| `stopToken()` / `requestCancel()` | cancel the startup | cancellation is not a failure |

### 4.2 `gui::BootSplash`

| Interface | Contract | Notes |
| --- | --- | --- |
| `isBusyIndicatorRunning()` | whether the spinner is turning | no percentage, no progress bar |
| `statusText()` | the status line currently drawn | for test assertions |

### 4.3 `SplashConfig`

`enabled` / `title` / `subtitle` / `logo`; the application configures appearance only and writes no presentation code.

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | The splash shows no percentage and no progress bar; a status line + a spinner. |
| C2 | Nothing animates inside a phase; no heartbeat and no interpolation over time. |
| C3 | Startup work comes after the splash' first frame; the main window is not shown before `startupEnd()`. |
| C4 | The main window is first shown in `startupEnd()`; during startup the screen holds the splash only. |
| C5 | Cancellation is not a failure; it goes through neither `startupEnd()` nor `failStartup()`. |
| C6 | The reporting door is not built in the constructor; `startupSequence()` opens and closes it. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | Follow the light and dark themes; an unreadable logo does not affect startup. |
| R2 | Phase boundaries are visible; the status bar, the console and the splash share one progress. |
| R3 | The first-frame door has a 300 ms fallback; without a splash, continue immediately. |
| R4 | The main window's first appearance is finished, with no hole in it. |
| R5 | A plugin takes the same stop token; one outside startup gets a token that never stops. |
| R6 | Without the reporting door every call is a no-op. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | During startup the screen holds the splash only. |
| N2 | The main window's first `show()` happens in `startupEnd()`. |
| N3 | The reporting door's lifetime = the startup phase's lifetime. |
| N4 | The splash never turns a countable phase into a number. |

## 6. Caveats and Traps

### 6.1 The price of animating inside a phase

- How it fails: the assumption that a timer would make the spinner smoother; in practice it needs cutting the session `init()` apart or turning the loop during startup.
- How to detect it: the application thread's longest continuous occupancy measures ~184 ms (the session init).
- How to prevent it: update at phase boundaries only and stay still inside a phase.

### 6.2 Pumping events during startup

- How it fails: `processEvents()` wakes the render surface's resize / settle timers early, so no valid handle is available.
- How to detect it: attach happens before the layout settles; the splash is fully transparent or blank.
- How to prevent it: the splash only `repaint()`s; the first-frame door uses `Window::hasPainted()`.

### 6.3 Windows is unverified

- How it fails: with the main window not shown, `VsgHostWindow` cannot get a valid HWND and attach fails.
- How to detect it: the log says `GetClientRect(..) failed` / `surface -> Failed`.
- How to prevent it: fall back to "show the main window early and let the door wait for both windows" and record "not adopted on Windows"; the criteria are in the backlog.

## 7. Rejected Options

| # | Option | Reason | ADR |
| --- | --- | --- | --- |
| D1 | a progress bar / percentage / overall ETA | startup has no known total, so a bar could only lie. | — |
| D2 | animation / heartbeat / interpolation inside a phase | it needs cutting the session `init()` apart or turning the loop, and the cost outweighs the gain. | — |
| D3 | showing the main window and the splash together, early | the user would see "a window pops up and then changes shape". | — |
| D4 | building the reporting door in the constructor | a process that never runs a startup would be `isBusy()` for ever and every top-level command would be refused. | — |
| D5 | letting the host order the startup itself (finish startup, then run the loop) | one missing step returns to the old failure; the startup step belongs to `run()`. | — |
| D6 | `processEvents()` pumping during startup | it would run other components' timers and events too. | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | off by default; `startupEnd()` is idempotent when off | `BootSplashTest.DisabledByDefaultInTheTestApplication` |
| T2 | the text drawn by the box follows `stage()` / `setLabel()` | `BootSplashTest`'s `statusText()` |
| T3 | the startup step runs inside the loop, the host's work on another thread, and the main thread keeps dispatching | `test_appfw`'s startup-work cases |
| T4 | attach succeeds while the main window is unshown, and `Presenting` follows `startupEnd()` | `scripts/xwinmap.py` + the gate's app stage |

## 9. Status and Evolution

### 9.1 Current status

F1–F6 are landed; X11 / WSLg verified that "during startup only the splash is on screen"; Windows is unverified.

### 9.2 Known gaps / backlog

- The Windows HWND + client-rect probe; see `../backlog/appfw-startup.md`.
- Animation / a progress bar inside a phase is not done; doing it would first need the session `init()`'s thread and loop-turning problem solved.

### 9.3 Compatibility / migration

The old names `beginStartup` / `finishStartup` / `runStartup` and the removed `run(work)` overload appear in git history only; code is written against the three beats
`startupStart` / `startup` / `startupEnd`.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/StartupProgress.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/BootSplash.hpp`
- `src/fw/appfw/sdk/vine/appfw/AppConfig.hpp`
- `tests/test_gui/BootSplashTest.cpp`

### 10.2 Documents and ADRs

- `appfw-startup-phases.md`
- `appfw-render-surface.md`
- `appfw-progress.md`
- `../backlog/appfw-startup.md`

### 10.3 External material

- Qt's `Qt::SplashScreen` / `WA_TranslucentBackground` semantics.
- X11 `XQueryTree` / `XGetWindowAttributes` (`scripts/xwinmap.py`).
