# appfw's EventBus

> **Status**: current
> **Last verified**: 2026-11-18
> **Scope**: the event bus, subscription and cancellation, delivery and shutdown; not the internals of `Signal` itself.
> **Sources**: `EventBus.hpp`, `EventBus.cpp`, `tests/test_gui/EventBusTest.cpp`.
> **Backlog**: `../backlog/appfw-eventbus.md`

## 1. Overview

### 1.1 Problem and background

appfw needs a process-level event bus: a subscriber may outlive the bus, a delivery may cross a thread or wait in a queue, and shutdown has to be able to prove that no thread is still inside the bus.
That last requirement is what shapes the design — admission counting and the close state machine have to agree on the same moment, and everything the bus hands out has to stay valid after the bus is gone; otherwise a graceful shutdown reports work it never drained.

### 1.2 Goals

- An object that has left the bus (`Subscription`, a queued task) holds no pointer to, and no strong reference to, the bus.
- Cancellation means the same thing everywhere: `unsubscribe()`, a handle's destructor, `shutdown()` and the bus destructor all cancel the handlers that have **not started**.
- `shutdown()` and the destructor release queued events immediately; an exception never escapes `publish()`, a queue callback or the Qt event loop.
- No user code runs under a lock.

### 1.3 Non-goals

- Not killing a handler that is already running at shutdown; a call that has started runs to its end.
- `shutdown()` does not wait for in-flight calls (only `shutdownGracefully()` and the destructor wait).
- Not pumping the event loop's timers, painting or input (only `QEvent::MetaCall` is dispatched).

### 1.4 One-paragraph conclusion

Every path out of `EventBus` holds `shared_ptr` state or delivery objects, never a pointer to the bus; admission counting, the close state machine and the delivery registry together make "graceful returned true ⇒ no thread is still inside the bus" true.

## 2. Design Points

### 2.1 Structure

```
EventBus ── Impl ─┬─ map<TypeId, EventChannel>     (channels, std::shared_mutex)
                  ├─ shared_ptr<DeliveryRegistry> (parked deliveries, decoupled from the bus)
                  └─ StopGate                     (admission + the stop handshake)
StopGate ────────── mutex + cv + active_calls + State + stopper + atomic stopping_ + per-thread depth table
  └─ StopGate::Ticket ── one admitted call; releases the count on destruction
EventChannel ────── vector<shared_ptr<SubscriptionState>> + std::mutex
SubscriptionState ── const id / const Handler / const mode / const tag / atomic<bool> active
Subscription ────── shared_ptr<Control>  (Control holds SubscriptionState; its destructor deactivates)
PendingDelivery ─── weak_ptr<SubscriptionState> + shared_ptr<const Object> + error-hook snapshot
DeliveryTask ────── shared_ptr<DeliveryRegistry> + weak_ptr<PendingDelivery>; unregisters when destroyed
queue lambda ────── captures only shared_ptr<DeliveryTask> (no bus / channel / this)
```

`MainThreadDispatcher` is created by `Application` and injected into `EventBus`; in `ApplicationData` the `main_dispatcher` member is declared before `event_bus` and is therefore destroyed after it.

`Impl` owns no admission state of its own: the whole protocol lives in `StopGate`, which is the only place that knows what "the bus is stopping" means.

### 2.2 Admission and the stop handshake

- `publish()` / `subscribe()` enter the bus through `StopGate::Ticket`; `state` and `active_calls` live under the same gate mutex, so "the bus has begun stopping" and "a caller got in" cannot interleave.
- `Running → Stopping → Stopped`; entering `Stopping` also sets the `stopping_` flag that is visible outside the lock.
- Every stop entry point asks the gate once and then does only what the answer says: `request()` returns `Perform` (this call owns the stop and must call `finish()`), `Completed` (another thread's stop is over and `result()` is its answer), `Reentrant` (this thread is running that stop) or `TimedOut`. `shutdown()`, `shutdownGracefully()` and `~EventBus()` therefore differ only in what they do *after* `Perform`.
- One wait bound: `StopGate::Deadline`, where `Deadline::max()` means "no bound", so "wait forever" and "wait until" are one function with two arguments instead of two functions.

### 2.3 Dispatch rules

- A subscription receives exactly the types for which `obj_cast<TEvent>(event)` succeeds: the event's own type, its derived classes and the interfaces it implements.
- Planned order: the most-derived class → the interfaces that class declares (recursively, in declaration order) → the base classes and their interfaces; a type is planned once.
- The interface walk is an explicit-stack DFS with `visited` maintained at the walk level, which removes the recursion depth and the repeated visits of a diamond.
- `publish()` collects a subscription snapshot under the map's shared lock and dispatches after releasing it; a subscribe or unsubscribe performed during dispatch affects later `publish()` calls only.

### 2.4 One owner per fact

The bus reads as five small types because every fact lives with the object that owns it, and no fact is restated in a second place:

| Fact | Owner | Why it lives there |
| --- | --- | --- |
| Whether a call may enter, who performs a stop, what that stop reported | `StopGate` | The three stop entry points sequenced the same handshake by hand; one `request()` answer makes the rule exist once (D6). |
| The delivery thread a subscription asked for | `SubscriptionState` | It is a property of the subscription, not of a channel entry; a parked delivery therefore reads it from the object it already holds, and the entry type disappears (D7). |
| Which channels exist, versus what one channel holds | `Impl::channels_mutex` versus `EventChannel::mutex` | Two concurrent publications both reap; one shared map lock would let two readers write the same list, so the two facts keep two locks with a fixed order (D5). |
| The parked work, versus the queued task's lifetime | `PendingDelivery` (held by the registry) versus `DeliveryTask` (held by the event loop) | Both must outlive the bus, and only the task may unregister on death; collapsing them would put the unregistration in a destructor that the registry itself keeps from running. |
| A failure's context | `EventBusError`, built once in the delivery path | The hook receives a value; no second copy of the event type, tag or mode is threaded through the arguments. |

The public class keeps no state beyond the pimpl and the injected marshaller: `EventBus` is a facade over `Impl`, so the whole protocol can be read in one file without asking which entry point is running.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Subscribe / unsubscribe | `subscribe()` returns an RAII handle; the destructor / `unsubscribe()` cancels calls that have not started | landed |
| F2 | Dispatch / polymorphism | `publish()` dispatches by the `obj_cast` rules; interface subscriptions | landed |
| F3 | Queued delivery | Main / Auto modes; `DeliveryRegistry` is decoupled from the bus | landed |
| F4 | Shutdown | `shutdown()` / `shutdownGracefully(timeout)` / the destructor | landed |
| F5 | Exception observation | `setErrorHandler()`; a handler exception does not escape | landed |

### 3.1.1 F1 Subscribe / unsubscribe

- **Behaviour**: `subscribe(handler, mode, tag)` returns a `Subscription`; the destructor / `unsubscribe()` sets `active` to false.
- **Constraints** (C1): `active` is checked before every handler call; that check is the cancellation linearisation point.
- **Requirements** (R1): a call that fails the check never starts; a call that has started runs to its end; `unsubscribe()` does not wait for in-flight calls.
- **Acceptance** (T1): `SubscriptionUnsubscribesOnDestruction`, `UnsubscribeInsideHandlerCancelsNotYetVisited`.
- **Caveat** (N1): a `Subscription` is held and destroyed by one thread only.

### 3.1.2 F2 Dispatch / polymorphism

- **Behaviour**: dispatch follows the `obj_cast` rules; an interface subscription requires the event's metadata to declare that interface.
- **Constraints** (C2): the planned order is fixed; a type is planned once.
- **Requirements** (R2): `publish()` with a null event, or on a stopped bus, is a no-op.
- **Acceptance** (T2): `PolymorphicDispatch`, `InterfaceSubscriptionReceivesImplementingEvents`, `MultiInterfaceVisitOrderAndDedup`.
- **Caveat** (N2): declaring an interface without really inheriting it makes `obj_cast` / `dynamic_cast` fail (and is logged).

### 3.1.3 F3 Queued delivery

- **Behaviour**: Main / Auto modes; the `DeliveryRegistry` holds the `PendingDelivery` strongly and the queued `DeliveryTask` holds a weak reference.
- **Constraints** (C3): `shutdown()` clears the registry → the event is released immediately and the queued callback degenerates to a no-op.
- **Requirements** (R3): whether a task runs, is cancelled or is dropped, `~DeliveryTask` is what unregisters it.
- **Acceptance** (T3): `MainModeQueuesDelivery`, `ShutdownReleasesPendingEvent`, `BusDestroyedBeforeQueuedDeliveryIsDropped`.
- **Caveat** (N3): with no dispatcher, or no `QCoreApplication`, Main / Auto run inline on the publishing thread (documented).

### 3.1.4 F4 Shutdown

- **Behaviour**: `shutdown()` stops admission, cancels subscriptions, drops deliveries and does not wait; `shutdownGracefully(timeout)` waits for in-flight calls and pumps the queue; the destructor refuses admission and then waits indefinitely for admitted calls to leave.
- **Constraints** (C4): `shutdown_result` defaults to false; only a graceful shutdown that really waited and really pumped the queue writes true.
- **Requirements** (R4): a re-entrant call on the stopping thread does not wait for itself; `shutdownGracefully()` from a non-application thread returns false.
- **Acceptance** (T4): `GracefulShutdownWaitsForPublishingThread`, `GracefulResultAfterPlainShutdownDoesNotClaimDrainedWork`, `ReentrantShutdownFromReleasedHandlerDoesNotWaitForItself`.
- **Caveat** (N4): `true` means exactly one thing — this shutdown waited for the in-flight calls and also ran the queued deliveries to their end.

### 3.1.5 F5 Exception observation

- **Behaviour**: `setErrorHandler(EventBusErrorHandler)` is optional; the context carries the event type, the subscription id, the tag, the `exception_ptr`, the thread mode and `deferred`. The `exception_ptr` rethrows whatever was thrown - a non-standard exception rethrows as its own type - and the log line names the event type next to the subscription id, because the id is only unique within one event type.
- **Constraints** (C5): the hook is always called outside every lock; an exception thrown by the hook itself is discarded (`noexcept`).
- **Requirements** (R5): a queued delivery snapshots the hook and the tag when it is posted, so replacing or removing the hook later does not affect deliveries already queued.
- **Acceptance** (T5): `SubscriberExceptionIsContained`, `ErrorHandlerReceivesDeferredFailure`, `ErrorHandlerReportsSubscriptionTag`.
- **Caveat** (N5): with no hook installed the only cost is one extra `has_error_handler` load on the error path.

## 4. Public Interface

### 4.1 `EventBus`

| Interface | Contract | Note |
| --- | --- | --- |
| `subscribe<TEvent>(handler, mode, tag)` | returns an RAII `Subscription` | `TEvent` must be `TypeDescribed` |
| `publish(event)` | dispatches by the `obj_cast` rules | a null event is a no-op |
| `unsubscribe(subscription)` | cancels calls that have not started | does not wait for in-flight calls |
| `shutdown()` | stops admission, drops queued deliveries | does not wait |
| `shutdownGracefully(timeout)` | waits for in-flight calls + pumps the queue | returns whether draining completed |
| `isShutDown()` / `pendingDeliveryCount()` | queries | — |
| `setErrorHandler(...)` | exception observation | called outside locks |

### 4.2 `Subscription` / `EventBusError`

| Interface | Contract | Note |
| --- | --- | --- |
| `Subscription::~Subscription()` | unsubscribes | movable |
| `EventBusError` | event type / id / tag / exception / thread mode / deferred | a value copy |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | `active` is checked before every handler call; that check is the cancellation linearisation point. |
| C2 | The planned order is fixed; a type is planned once. |
| C3 | `shutdown()` clears the registry → the event is released immediately. |
| C4 | `shutdown_result` defaults to false; only a graceful shutdown that really drained writes true. |
| C5 | The exception hook is called outside locks; an exception from the hook is discarded. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | A call that fails the cancellation point never starts; a call that has started runs to its end. |
| R2 | `publish()` with a null event, or on a stopped bus, is a no-op; `subscribe()` after the stop returns an inert handle. |
| R3 | Whether a queued task runs, is cancelled or is dropped, `~DeliveryTask` unregisters it. |
| R4 | A re-entrant call on the stopping thread does not wait for itself; graceful from a non-application thread returns false. |
| R5 | A queued delivery snapshots the exception hook and the tag when it is posted. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | An object that has left the bus holds no pointer or strong reference to it. |
| N2 | No user code runs under a lock; handler closures and event destruction happen outside it. |
| N3 | `shutdownGracefully()` returning true ⇒ no thread is still inside the bus. |
| N4 | No concurrent call during destruction; a violation turns from a use-after-free into a delay. |

## 6. Caveats and Traps

### 6.1 Destroying the bus inside a handler

- How it fails: `~EventBus()` waits for admitted calls to leave, and the current handler is one of them → deadlock or a delay.
- How to detect it: `delete bus` or a destruction call inside a handler.
- How to prevent it: never destroy the bus inside a handler; to stop the process use `Application::exit()`.
- The same shape one step later: a stop reaps the handler closures outside every lock, so a closure that owns (and deletes) the bus destroys it from the stopping thread. `~EventBus()` asks the gate, gets `Reentrant`, and cannot wait for a stop it is itself holding up - the object dies under the stopper. `shutdown()` and `shutdownGracefully()` answer `Reentrant` by returning, the destructor cannot, so this stays a contract violation to avoid rather than one the bus can absorb.

### 6.2 Graceful shutdown from a non-application thread

- How it fails: assuming `shutdownGracefully()` drains from any thread.
- How to detect it: it returns false and `deliverPostedCalls()` never ran.
- How to prevent it: call it on the application thread only; other threads use `shutdown()` or let the application thread finish.

### 6.3 A shutdown re-entered from a subscription closure's destructor

- How it fails: the re-entrant call on the stopping thread waits for itself → deadlock.
- How to detect it: `shutdown()` / `shutdownGracefully()` while `Stopping` with `stopper == this_thread`.
- How to prevent it: the `Impl::stopper` guard returns directly (`shutdown()` no-op / graceful false).

## 7. Rejected Options

| # | Option | Reason | ADR |
| --- | --- | --- | --- |
| D1 | `DeliveryRegistry` as an `unordered_map` + `extract` | Pending deliveries are usually 0–2, `push_back` is already O(1), and a vector is cache-friendly. | — |
| D2 | `visited` as an `unordered_set` | The set is small, pointer comparison is faster and it allocates nothing. | — |
| D3 | A `thread_local` depth covering detached async tasks | An async task calling `publish()` is its own admission call and does not rely on a depth escaping the thread. | — |
| D4 | Making `shutdown()` wait for in-flight calls | Its meaning is "stop admitting"; waiting belongs to `shutdownGracefully()` / the destructor. | — |
| D5 | One lock for the map and the channels | Two concurrent publications both reap cancelled entries, so a single shared map lock would let two readers write the same list. The channel mutex keeps "which channels exist" and "what one channel holds" as two facts, with the lock order fixed. | — |
| D6 | Three separate stop arbitrations | `beginStop()` / `stoppingOnThisThread()` / `awaitStopped*()` had to be sequenced identically by three entry points. One `request()` answer (`Perform` / `Completed` / `Reentrant` / `TimedOut`) makes the rule one rule. | — |
| D7 | A separate entry type carrying the thread mode | The mode is a subscription property, not an entry property: keeping it in `SubscriptionState` removes the entry type and lets a parked delivery read the mode from the object it already holds. | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | subscribe / cancel / RAII / move / concurrent churn | `SubscriptionUnsubscribesOnDestruction`, `MoveTransfersOwnership`, `PublishingWhileSubscriptionsChurn` |
| T2 | polymorphism / interface planned order | `PolymorphicDispatch`, `MultiInterfaceVisitOrderAndDedup` |
| T3 | queueing / event lifetime / the queued mode | `MainModeQueuesDelivery`, `ShutdownReleasesPendingEvent`, `BusDestroyedBeforeQueuedDeliveryIsDropped`, `AutoModeOffMainKeepsItsQueuedMode` |
| T4 | shutdown / admission | `GracefulShutdownWaitsForPublishingThread`, `GracefulResultAfterPlainShutdownDoesNotClaimDrainedWork`, `IsShutDownTurnsTrueWhileAGracefulStopDrains`, `SecondShutdownWaitsForTheStopInProgress` |
| T5 | exception observation (standard and non-standard) | `SubscriberExceptionIsContained`, `ErrorHandlerReportsSubscriptionTag`, `ErrorHandlerReceivesNonStandardException` |
| T6 | the ASan gate | `scripts/asan_check.sh` (`EventBusTest.*`) |

## 9. Status and Evolution

### 9.1 Current status

Subscription, dispatch, queueing, shutdown and exception observation are all in place; the ASan gate passes.

### 9.2 Known gaps / backlog

- If the event loop stalls for a long time with tens of thousands of Main deliveries pending at once, `remove` on the drain path is O(N²); the registry changes if that happens (see D1 and `../backlog/appfw-eventbus.md` T1).
- `SubscriptionThreadMode` states no underlying type although it appears in the public `EventBusError` value; declaring it `: std::uint8_t` changes that struct's layout, so it rides the next plugin-ABI batch instead of a stand-alone edit (`../backlog/appfw-eventbus.md` T2).
- LeakSanitizer's 15 reports / 5864 bytes all pass through `GuiTest::SetUp`; they are held by the test fixture / `QApplication`, not by the bus.

### 9.3 Compatibility / migration

New code uses the interface of §4 only.
`shutdown_result` defaults to false, so a caller must not read it as "drained" without a graceful shutdown having run.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/EventBus.hpp`
- `src/fw/appfw/src/EventBus.cpp`
- `src/fw/appfw/sdk/vine/appfw/MainThreadDispatcher.hpp`
- `src/base/core/sdk/vine/Type.hpp`

### 10.2 Documents and ADRs

- `appfw-command-manager.md`
- `appfw-progress.md`
- `../backlog/`

### 10.3 External material

- Qt's `QMetaObject::invokeMethod` / `QEvent::MetaCall` semantics.
- The ASan build conventions of `scripts/asan_check.sh`.
