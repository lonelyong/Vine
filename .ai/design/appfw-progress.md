# appfw's ambient progress (`ProgressHost`)

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: `ProgressHost`'s ownership, its push model, its two consumers and its contract.
> **Sources**: `ProgressHost.hpp`, `tests/test_gui`.
> **Backlog**: none (open items are in `../backlog/`)

## 1. Overview

### 1.1 Problem and background

"what is the application doing right now" is application state: it must be reported across threads, subscribed by several presenters (status bar, console, splash), and at run
time there may be no foreground host. The host belongs to appfw because it needs Core's `Signal`; base's `vn::Progress` offers the dependency-free progress primitives only.

### 1.2 Goals

- `ProgressHost`: a process-level registry, a foreground stack, labels, a shared cancel source and change notification.
- Two consumers: `gui::ProgressPresenter` (the status bar) and `ConsoleProgressReporter` (headless).
- Zero wake-ups when idle: no timer is armed when there is no pending deadline.

### 1.3 Non-goals

- No polling model; notification is driven by state changes.
- The whole progress module does not move into appfw (`ProgressIndicator` / Range / Scope stay dependency-free).
- No foreground host is built in a constructor; a process that never runs a startup must not be judged "busy".

### 1.4 One-paragraph conclusion

`ProgressHost` is appfw's process-level foreground stack plus a `Signal<>` change notification; presenters subscribe and marshal on their own, and base's
`ProgressIndicator` / Range / Scope keep doing the dependency-free progress arithmetic.

## 2. Design Points

### 2.1 The three layers

- **base / `vn::Progress`**: `ProgressIndicator` (a [0,1] scale, an atomic position, an external cancel token) and `ProgressRange` / `ProgressScope` (cutting the scale into
  intervals). No external dependency.
- **appfw / `vn::Appfw`**: `ProgressHost` — the process-level registry, the foreground stack, labels, the shared cancel source and change notification.
- **The presentation layer**: `gui::ProgressPresenter`, `ConsoleProgressReporter`.

The dependency direction: appfw → `vn::Progress`; `ProgressHost.hpp` returns ranges and scopes by value, so `vn::Progress` is a **PUBLIC** link dependency of appfw.
`VN_APPFW_PLUGIN_ABI_VERSION` is currently **10u**.

### 2.2 The push model

- `ProgressHost::changed()` returns a process-level `Signal<>` that fires when a host registers or unregisters, when the foreground stack is pushed or popped, when a label
  changes (setting the same value does not fire), and when an indicator's position crosses a whole percentage point (including reaching the end and being reset).
- The signal **carries no payload**: the unregister notification is emitted from the host's destructor, and carrying a pointer would hand the observer an object that is
  dying; observers re-sample `current()` / `activeHosts()`.
- The firing thread is the thread that changed the state (possibly a worker): the presentation layer marshals on its own; firing happens outside the lock and never nests
  with the registry lock.

### 2.3 Where the sources merge

`ProgressIndicator::setPositionCallback(std::function<void()>)` calls back once only when "a whole percentage point is crossed / the end is reached / the position is
reset", while `increment()` is a per-item hot path. Measured (-O3, 20 million items): `next(1)` went from 10.0 to 9.9 ns/item and the 20 million items fired **101**
notifications.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | The foreground stack | host registration / unregistration, `setForeground`, nested push / restore | landed |
| F2 | Change notification | `changed()` fires on a state change or a whole percentage point | landed |
| F3 | Status-bar presentation | `ProgressPresenter` subscribes + 400 / 300 ms single-shot timers | landed |
| F4 | Headless presentation | `ConsoleProgressReporter` subscribes + one-shot wake-up | landed |
| F5 | Cancellation | `ProgressIndicator`'s external cancel token, shared | landed |

### 3.1.1 F1 The foreground stack

- **Behaviour**: `ProgressHost` holds the process-level registry and the foreground stack; `setForeground(true)` promotes, a nested sub-command pushes, and the destructor
  restores the parent host automatically.
- **Constraints** (C1): with no foreground host `current()` is nullptr; the startup phase is a foreground host itself.
- **Requirements** (R1): a constructor builds no foreground host; a process that never runs a startup must have `isBusy()` false.
- **Acceptance** (T1): `ProgressHostTest`'s registry / foreground-stack cases.
- **Caveat** (N1): `isBusy()` = there is a foreground host.

### 3.1.2 F2 Change notification

- **Behaviour**: `changed()` fires on register / unregister, push / pop, a label change, a whole percentage point / the end / a reset.
- **Constraints** (C2): the signal carries no payload; firing happens outside the lock; the unregister notification is emitted inside the host's destructor.
- **Requirements** (R2): setting the same label does not fire; observers re-sample `current()` / `activeHosts()`.
- **Acceptance** (T2): `ProgressHostTest`'s `changed()` merging case (about 100 fires for 10,000 items).
- **Caveat** (N2): the firing thread may be a worker; the presentation layer marshals with its own `QMetaObject::invokeMethod`.

### 3.1.3 F3 Status-bar presentation

- **Behaviour**: `gui::ProgressPresenter` subscribes to `changed()` and then stops polling; it arms one single-shot `QTimer` each for "400 ms show delay" and "300 ms hide
  delay".
- **Constraints** (C3): a cross-thread notification is posted to the native widget (`Qt::QueuedConnection`); the presenter dies with the widget.
- **Requirements** (R3): it holds no timer while idle.
- **Acceptance** (T3): `ProgressPresenterTest.*`.
- **Caveat** (N3): a queued call never outlives the widget.

### 3.1.4 F4 Headless presentation

- **Behaviour**: `ConsoleProgressReporter` subscribes to `changed()`; the show delay uses a one-shot wake-up from the async shared timer service.
- **Constraints** (C4): a generation counter turns a wake-up that comes due after `stop()` into a no-op; `sleeping_` guarantees only one is outstanding.
- **Requirements** (R4): `poll()` stays for a host-driven loop and for tests, and does not need a prior `start()`.
- **Acceptance** (T4): `ConsoleProgressReporterTest.*`.
- **Caveat** (N4): `ConsoleProgressOptions::interval` is the minimum line spacing, not a polling period.

### 3.1.5 F5 Cancellation

- **Behaviour**: `ProgressIndicator` carries an external cancel token, and `ProgressHost` shares the cancel source.
- **Constraints** (C5): a cancellation is a request; the host does not decide for the consumer whether to cancel.
- **Requirements** (R5): after `stop()` returns there is no further output (a mutex fence + the generation).
- **Acceptance** (T5): `ProgressHostTest`'s cancellation cases, `ProgressPresenterTest`'s cancel button.
- **Caveat** (N5): after the unregister notification an observer sees the **already restored** foreground (the parent host or nullptr).

## 4. Public Interface

### 4.1 `ProgressHost`

| Interface | Contract | Notes |
| --- | --- | --- |
| `current()` / `activeHosts()` | the process-level foreground stack / registry | for observers to re-sample |
| `changed()` | `Signal<>` | no payload |
| `setForeground(bool)` / `isForeground()` | foreground promotion | nested push / restore |
| `label()` / `setLabel()` | returns by value / writes | an unchanged value fires nothing |
| `stop()` | stops reporting | the fence + the generation |

### 4.2 The presenters

| Interface | Contract | Notes |
| --- | --- | --- |
| `gui::ProgressPresenter` | subscribe + single-shot timers | zero timers when idle |
| `ConsoleProgressReporter` | subscribe + one-shot wake-up | `poll()` for a host-driven loop / tests |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | With no foreground host `current()` is nullptr; the startup phase is a foreground host itself. |
| C2 | `changed()` carries no payload; firing happens outside the lock; the unregister notification is emitted inside the host's destructor. |
| C3 | A cross-thread notification is posted to the native widget; the presenter dies with the widget. |
| C4 | A wake-up coming due after `stop()` is a no-op; only one is outstanding at a time. |
| C5 | A cancellation is a request; the host does not decide for the consumer. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | A constructor builds no foreground host; `isBusy()` = there is a foreground host. |
| R2 | Setting the same label fires nothing; observers re-sample. |
| R3 | A presenter holds no timer while idle. |
| R4 | `poll()` does not need a prior `start()`. |
| R5 | After `stop()` returns there is no further output. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | Firing the signal holds no registry lock. |
| N2 | During the unregister notification the host object is still alive, so an observer can re-sample safely. |
| N3 | The startup phase shares the same foreground-stack semantics as an ordinary command. |

## 6. Caveats and Traps

### 6.1 A pointer inside the unregister notification

- How it fails: the signal carries a host pointer and an observer dereferences it inside the host's destructor.
- How to detect it: ASan / UAF; `changed()` with a parameter.
- How to prevent it: the signal carries no payload and observers re-sample `current()` / `activeHosts()`.

### 6.2 A presenter's timers

- How it fails: polling or a resident timer while idle, so the CPU spins for nothing.
- How to detect it: a timer exists with no pending deadline.
- How to prevent it: arm single-shot timers for the 400 / 300 ms deadlines only; the console uses a one-shot wake-up.

## 7. Rejected Options

| # | Option | Reason | ADR |
| --- | --- | --- | --- |
| D1 | keeping base's self-check callback + single observer | a degradation forced by the layer constraint; it lapses once the move is done. | — |
| D2 | moving the whole progress module into appfw | `ProgressIndicator` / Range / Scope are dependency-free primitives. | — |
| D3 | a resident console polling loop | event-driven plus a one-shot wake-up is enough, and it wakes nothing when idle. | — |
| D4 | a host polling its own indicator | that falls back to a pull model. | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | position arithmetic / range / scope / callback merging | `tests/test_progress/ProgressIndicatorTest.cpp` (9 cases) |
| T2 | registry / foreground stack / label / cancellation / `changed()` | `tests/test_gui/ProgressHostTest.cpp` (15 cases) |
| T3 | console throttling / silence / end to end | `ConsoleProgressReporterTest.*` (3 cases) |
| T4 | status-bar cancellation / `isBusy()` | `ProgressPresenterTest.*` |

## 9. Status and Evolution

### 9.1 Current status

`ProgressHost`, both presenters and the shared cancel source are landed; an idle system wakes nothing.

### 9.2 Known gaps / backlog

None; a new requirement is judged by "does it really need polling or a resident timer".

### 9.3 Compatibility / migration

`label()` returns by value now; `ConsoleProgressOptions::interval` means the minimum line spacing now.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/ProgressHost.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/ProgressPresenter.hpp`
- `src/fw/appfw/sdk/vine/appfw/ConsoleProgressReporter.hpp`
- `src/base/progress/**`

### 10.2 Documents and ADRs

- `appfw-startup-splash.md`
- `appfw-startup-phases.md`
- `../backlog/`

### 10.3 External material

- Core `Signal`'s thread-safety semantics.
