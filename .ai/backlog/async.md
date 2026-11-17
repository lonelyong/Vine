# async next-step design (P2: cancellation environment / structured scope / frame allocation)

> Parent document: `.ai/design/async-design.md` (§8 points here). Module notes: `.ai/memory/async.md`.
> This page designs only, it changes no code; every section says "why it is worth it / not worth it" and "how to prove it
> was done right".

## 0. Conclusion table (the evidence is in the sections below)

| Item | Conclusion | Evidence (measurement / repository-wide grep) |
| --- | --- | --- |
| A cancellation **environment** (the stop token travels with the task) | **recommended for scheduling**, the shape is settled (§1 A + C) | appfw already hand-built an environment (`PluginLoadContext::stopToken()`, `CommandExecutionContext::stopToken()`, `StartupProgress::stopToken()`), and async cannot see it; a child task can only get the token by having its author pass it by hand |
| A cancellation **result channel** (`set_stopped` instead of an exception) | **not doing it**, documentation only | cppcoro/folly use exceptions too; the layers above already classify by `TaskCancelledException`; the price is a `Task` result type + every combinator + 145 cases |
| **Enforcing** the structured scope | **a small change** (destructor = request stop + assert, waiting stays explicit; add `detach()`) | `Scope` has **0 users** in production code (repository-wide grep); stdexec's `async_scope`/`counting_scope` are also "assert in the destructor, await the synchronisation point explicitly" |
| **Frame allocation** (promise `operator new` / an allocator) | **not doing it**, booked with the criteria written down | coroutine bodies appear only in appfw (15 files) / app_shell (11) / demo_plugin (3) / test_plugin (6) / the vsg plugin (**1 place**), and the `src/viz` render path has **0** ⇒ frame allocation is on no hot path |

---

## 1. The cancellation environment (recommended for scheduling)

### 1.1 Today: the environment exists, nobody claims it

Cancellation in `vn::async` is an **explicit parameter** (`whenAll(tasks, token)`, `sleepFor(d, token)`,
`Scope::join(token)`), the same as cppcoro. But the layers above have long needed a token that does not travel as a
parameter:

- `PluginLoadContext::stopToken()` (the documentation's own words: the context carries the same token as
  `StartupProgress::stopToken()`);
- `CommandExecutionContext::stopToken()` / `isCancelled()`;
- `StartupProgress::stopToken()`.

In other words **appfw has already put the token inside a "context object"**, and `vn::async`'s combinators cannot see it:
`co_await whenAll(...)` and `co_await sleepFor(...)` inside `Plugin::load()` cannot reach the startup cancellation unless
the author passes the token down one by one.
P2300 turned this into an **environment** (`get_stop_token(env)`); this page proposes giving async a minimal
"environment-style token" along the same lines, rather than importing all of sender/receiver.

### 1.2 The shape (A: an environment-style token)

```cpp
// new (sdk/vine/async/StopToken.hpp)
namespace vn::async {

/// The current coroutine's stop token: a default-constructed empty token when none was injected.
[[nodiscard]] std::stop_token stopToken() noexcept;

/// Hand a token to a child (used by combinators): stopToken() inside this coroutine body reads it.
[[nodiscard]] Task<void> withStopToken(std::stop_token token, Task<void> body);   // shape only

} // namespace vn::async
```

Implementation points (the three that decide success or failure):

1. **The token lives in the promise**: `Task`'s `promise_type` gains `std::stop_token token{};`, and `stopToken()` reads it
   through *the current coroutine's promise*. Getting "the current coroutine's promise" needs the handle to reach the body
   once - use the existing symmetric-transfer awaiter (store `handle.promise()` in a thread-local slot from
   `await_suspend(handle)`, see below).
2. **The injection point is "before starting the child"**: `WhenChild::start(state)` / `Scope::add()` / `runShared()` /
   `scopeChild` all already have the "attach metadata" line (`handle_.promise().state = std::move(state);`), and the
   injection follows it ⇒ **zero new mechanism**.
3. **Inherit, do not replace**: a combinator uses an explicit token when given one and otherwise keeps the current
   environment (`stopToken()`); `withStopToken` overrides explicitly. A `Task` promise defaults to an empty token ⇒
   behaviour is exactly as today (`stopToken()` returns empty ⇒ nothing returns early because it "thinks it is
   cancelled").

### 1.3 How the combinators use it (the default policy suggested)

| Facility | Cancellation policy (once A lands) |
| --- | --- |
| `sleepFor(d, token={})` | **no environment fallback** (see the correction below): a callee cannot read its caller's environment |
| `whenAll` / `whenAny` | an explicit token wins; **whether children are injected by default** is a second stage (it changes behaviour and needs the cases rearranged) |
| `Scope::add(task)` | injects the scope's own token (already in place: `Scope` holds a `stop_source` ✓) |
| `withTimeout(task, d, token)` | today "a timeout destroys the child"; a three-stage form has to be decided separately (see §1.5) |

**Correction (found by measurement on 2026-09-26; this cell of the original design was wrong)**: `sleepFor()` used to be
written as "with no explicit token, use `co_await currentStopToken()`", but that reads **its own** environment - and it is
the **callee**, so its caller's token can never be visible to it.
`await_transform` only lets a coroutine read the token on **its own** promise, therefore:

- **the environment can only be injected by the creator** (`withStopToken(token, child())`), **it cannot be looked up by
  the callee**;
- P2300 can do "an operation sees its caller's environment" because the environment travels **down** with the receiver
  rather than being looked up like a variable; the C++20 shape here has no receiver, so the creator has to hand it over
  explicitly (that is the first stage, see below).

The correct usage is therefore one line in each direction:

```cpp
// the creator (framework/combinator) hands the token to the task it creates:
co_await withStopToken(context->stopToken(), plugin->load(context));

// the task body reads its own environment and hands it to what it awaits:
const std::stop_token mine = co_await currentStopToken();
co_await sleepFor(std::chrono::seconds(2), mine);
```

### 1.4 What does not change (the compatibility boundary)

- Every explicit-parameter entry point **stays** (172 `x.result()` calls and the token parameters of every combinator are
  untouched).
- The error channel **does not change**: cancellation still throws `TaskCancelledException` (typed,
  `vn::Exception::Code::CANCELLED`).
- `Task<T>`'s type, the `await_resume` semantics and `result()`'s "read once" all stay ⇒ **no ABI change, no change to
  the 145 cases**.

### 1.5 Why "a result channel (`set_stopped`)" is not done

The value of `set_stopped` is "cancellation is not an error": a combinator need not treat cancellation as failure, and
`whenAll` does not fail as a whole because one child was cancelled. The price is that `Task<T>`'s result becomes
three-state (value / error / stopped), and `await_resume`, every combinator, `result()`, the `unhandled_exception` path
and every case have to change, while **the benefit in this code is thin**: the layers above (command chain, plugins,
startup) already catch `TaskCancelledException` separately as "cancelled" (see `CommandManager` and the `AppLifecycle`
cases).
If the "a timeout destroys the child" behaviour of `withTimeout` has to be tightened one day (§4.2 of the risks), then
evaluate the three-stage "request stop, wait for the child to wind down, destroy on timeout" - that is a **local**
change, and the global result channel does not have to move first.

### 1.6 The verification recipe (nail it this way when doing A)

- Nail 1 (the injection works): a host gives `PluginLoadContext` a token and the plugin body does `sleepFor(500ms)`; the
  host calls `request_stop()` ⇒ `TaskCancelledException` within 20 ms (today that is only possible by passing the
  parameter explicitly).
- Nail 2 (empty by default): without injection `stopToken().stop_possible() == false`, and `sleepFor(20ms)` finishes on
  time.
- Nail 3 (inherit, do not replace): the children of `whenAll(withStopToken(t, ...), ...)` both see the same token (compare
  `stop_possible()` and `can_stop()` of the `stop_token`s, or use a recording task).
- Mutation: delete the injection line ⇒ nail 1 goes red (nails 2/3 unchanged); change "inherit" to "always empty" ⇒
  nail 3 goes red.
- The threading contract (must go into the header): the token is read only at **the injection that creates/starts the
  child** and takes part in no race; `stopToken()` is "the current coroutine's environment", and reading it across threads
  reads a value-semantics copy of the same `stop_token` (`std::stop_token` itself is thread-safe).

---

## 2. The structured scope (a small change, not a rewrite)

### 2.1 Today and the evidence

- `Scope`'s contract: children **own themselves** (`DetachedTask` + shared state), `~Scope` neither waits nor cancels; a
  child may outlive the scope, and there is one prose warning (`../design/async-design.md` §4.4).
- **No production user anywhere in the repository**: `grep -rn 'async::Scope' src/` only hits the unrelated
  `PluginScope`/`ChainScope`/`ReadScope`.
  In other words today's "not structured" has not hurt anyone yet - but an API that is used will trip over it at once.
- The standard shape (stdexec's `async_scope`/`counting_scope`): **assert in the destructor, await the synchronisation
  point explicitly**, not "wait inside the destructor" (a destructor is not a coroutine and cannot suspend; a synchronous
  wait would either block the thread or pump the event loop, which is the host's policy).

### 2.2 The shape suggested

```cpp
class Scope
{
  public:
    /// Request a stop (without waiting): the destructor and an explicit call both do only this.
    void requestStop() noexcept;

    /// Stop requesting a stop for the current batch: say plainly "I wait for this batch myself, do not interrupt them"
    /// (it does not affect join(), which still waits).
    void detach() noexcept;

    ~Scope();   // debug: pending != 0 and not detached() ⇒ assert; release: one warning
};
```

- **The destructor action = `request_stop()`** (no waiting and no assertion; `pendingChildren()` is left to the host to
  report - async depends only on `vn::Global`+`vn::Core` and does not pull in a logging dependency for one diagnostic).
  `requestStop()` needs a `stop_source`, which is exactly where §1's environment token lands: `Scope` holds a
  `std::stop_source`, `add()` injects the token into the child (§1.2 point 2), so "the destructor requests a stop" really
  has an effect on a cooperative child, while assert/detach merely turn "escaping" into an explicit choice.
- `join(token)`'s semantics must be stated precisely: **cancellation only stops the wait** (as today), **it does not
  cancel the children**; cancelling the children means `requestStop()`.
  Both can coexist: `join()` can actually `request_stop` per `token` first (when the token is the one the children
  inherited) - that is left to be nailed with a case at implementation time ("after cancelling the join the children wind
  down and `join()` returns").

### 2.3 The verification recipe

- Nails: `ScopeDestructorRequestsStop` (a child `co_await sleepFor(1s)` + observe `TaskCancelledException`);
  `ScopeDetachSilencesTheAssert` (after `detach()` the destructor no longer diagnoses); `CancelJoinStillLetsChildrenFinish`
  (already covered by `ScopeTest.JoinCancellationWakesOffTheCancellersStackAndLeavesTheChildRunning`, added the previous
  round).
- Mutation: delete `request_stop()` from the destructor ⇒ nail 1 goes red (the child runs the full 1 s instead of ending at
  once); delete the assertion ⇒ the diagnostic nail goes red.
- Note that both the debug assertion and the release warning have to be visible to a test (the repository already has the
  `WarningCapture` fixture, see round 29 of test_appfw).

---

## 3. Frame allocation (recommended not doing; the criteria written down)

- Today: 0 `promise_type::operator new/delete` in the whole module, 0 `allocator_arg_t` constructors, 0
  `get_return_object_on_allocation_failure` ⇒ every coroutine frame goes through the global `operator new`.
- **Measured across the repository**: coroutine bodies appear only in appfw (15 files), app_shell (11), demo_plugin (3),
  model_viewer (3), test_plugin (6) and `gfx_backend_vsg` (1 place, the plugin's own hook), with **0** on the `src/viz`
  render path.
  ⇒ **no hot path allocates a coroutine frame** (some dozens during startup, a few per command execution, none per
  rendered frame), so the upper bound of pooling is "a few dozen fewer mallocs during startup", which cannot be measured.
- Criteria (do it when any of these appears): 1) a use appears that "creates a coroutine per frame / per event"; 2) the
  frame allocation share is visible in a profile (`valgrind --tool=massif`, or a temporary counting `operator new` on
  `Task`'s promise); 3) a read-only or bare-metal target is introduced.
  The first step then is the P2014 shape (`promise_type(std::allocator_arg_t, const Alloc&, ...)` + `operator new`), and
  the "allocation failure returns empty" door stays open (`get_return_object_on_allocation_failure`).

---

## 4. Suggested order

1. **§1 A + C** (the environment token + `Scope` holding a `stop_source`): one design, two small changes, and appfw's
   `*::stopToken()` contexts "connect" to async's combinators directly;
2. **§2** (in the same batch as §1, since both touch `Scope`; the destructor only requests a stop + assertion + `detach()`);
3. **§3 booked** (move when there is data);
4. §1.5's "request a stop first, then wait" (the `withTimeout` three-stage form) waits for a real need and gets decided
   separately;
5. **The second stage (§6, 2026-09-27)**: `CancelPolicy{Destroy, StopAndWait}` - **landed**, but the default differs by
   applicability: `whenAll` (including the typed tuple/vector and the variadic form) = `StopAndWait` (request a stop
   first, then wait for the wind-down), `whenAny`/`withTimeout`/`Scope::join` = `Destroy` (unchanged). See §6.6.

---

## 5. First stage landed (2026-09-26): the optional environment token + the scope's stop request

**What landed** (`src/base/async/sdk/vine/async/`):

- `Task.hpp`: the promise gains `std::stop_token token{}`; `await_transform(CurrentStopTokenRequest)` answers through the
  already-ready `detail::ValueAwaiter<std::stop_token>`, and the other `await_transform(A&&)` passes **every other
  co_await through unchanged** (once a promise defines await_transform it intercepts every co_await, and that forwarding
  is the precondition for adding the hook).
  `detail::TaskEnvironment::setToken(task, token)` writes the promise of a **not yet started** lazy task (`friend` access).
- `StopToken.hpp` (new): `co_await currentStopToken()` reads its own environment; `withStopToken(token, task)` is injected
  by the creator.
- `Scope.hpp`: holds its own `std::stop_source`; `add()` injects the token **before starting the child**; `~Scope()` only
  calls `request_stop()`; new `pendingChildren()` and `detach()` (the latter = no more stop requests for this batch,
  `join()` still waits).
- `Sleep.hpp`: **no** environment fallback (the reason is the correction in §1.3).

**Deliberately not done**: `whenAll`/`whenAny` bringing children into the environment by default - that would turn "a child
of a cancelled combinator" from "destroyed" into "winds itself down", which is a behaviour change that has to be
rearranged together with the cases (second stage).

**Nails (5, `test_async` 145 → 150)**: the environment is empty by default
(`ATaskNobodyHandedATokenToSeesAnEmptyEnvironment`), the injection is visible (`WithStopTokenBecomesTheTasksOwnEnvironment`),
a handed-over token really cancels deep work (`AHandedOverTokenReachesWorkDeepInsideTheBody`, woken in < 300 ms after
another thread calls request_stop), a `Scope` gives its children a token and requests a stop on leaving
(`ChildrenRunInTheScopesEnvironmentAndLeavingAsksThemToStop`, which also pins `pendingChildren()`), and `detach()` leaves
the children alone afterwards (`ADetachedScopeLeavesItsChildrenAlone`).

**Mutations (measured)**: making `TaskEnvironment::setToken` a no-op ⇒ the two injection nails go red; dropping
`request_stop()` from `~Scope()` ⇒ the Scope nail goes red while the `detach` one stays green. After restoring, 150/150
green.

**The wiring landed (2026-09-26, same round)**: `PluginManager` no longer waits on the hook bare; it hands the startup
token in as the environment - three beats each for the synchronous gate
(`runToCompletion(async::withStopToken(context.stopToken(), plugin->hook(&context)))`) and the startup gate
(`co_await async::withStopToken(context.stopToken(), lp.plugin->hook(&context))`), 6 places in all.
So `co_await currentStopToken()` inside a plugin body is this startup's cancellation token.
The nail: the fixture plugin's `load()` records two more keys in the existing "request a cancel at this beat" branch
(`saw_environment` / `environment_sees_stop`), and `HeadlessBootTest.ACancelledBootEndsCleanlyWithoutFinishingTheBoot`
asserts both are true - an empty token's `stop_possible()` is false, so "the wiring is broken" is distinguishable from
"the token was not cancelled". Mutation: drop the `withStopToken` at that `load()` ⇒ the case goes red (both assertions
fail); restore ⇒ green.

---

## 6. Second stage: the combinators' cancellation policy (set by industry practice, 2026-09-27)

### 6.1 What five industry systems state in their own words

| System | When one child completes/fails, the others | Does it wait for them | The nature of cancellation |
| --- | --- | --- | --- |
| **Trio** (nursery) | "If any task inside the nursery finishes with an unhandled exception, then the nursery **immediately cancels all the tasks inside the nursery**"; cancellation is level-triggered and raises at a checkpoint | **waits** ("the block does not exit until *all* tasks have completed") | cooperative (no forced kill: a child that does not respond simply keeps it waiting) + `shield=True` carves out an exception for cleanup |
| **Kotlin** `coroutineScope` | "If block or any child coroutine in this scope fails with an exception, the scope fails, **cancelling all the other children**" | **waits** ("completes when both the block and all the coroutines launched in the scope complete") | cooperative; `supervisorScope` is the exit for "each fails on its own" |
| **Rust** `tokio::select!` | "returning when the first branch completes, **cancelling the remaining branches**" - the implementation is **drop** | does not wait (dropping is dropping) | **destruction is cancellation**; the price is written down: "cancellation safety: being dropped must be a no-op" |
| **Rust** `tokio::join!` | cancels nothing, waits for all | waits | - |
| **C#** `Task.WhenAll` | **cancels no sibling**; a faulted child ⇒ the result is Faulted and **all exceptions are aggregated**; no fault but a cancellation ⇒ the result is **Canceled** | waits for all | cancellation is an **independent state** (Canceled ≠ Faulted), initiated by the caller through its own CTS |
| **P2300 / asio** | a combinator **requests a stop** of the remaining operations but waits for them to report completion themselves (`set_stopped`); asio's `parallel_group` turns "wait for all / wait for the first / wait for the first error" into a **completion-condition parameter** paired with per-operation cancellation | waits | cooperative; cancellation is an independent channel |

**Two legitimate extremes** (it is not "who is right"):

- **(a) request a stop + wait for all** = Trio / Kotlin / P2300. It requires the children to "**respond to the stop**",
  otherwise the combinator is dragged along by one of them (Trio/Kotlin accept that, since they have no "forced kill"
  route at all).
- **(b) destroy immediately** = Rust `select!`. It requires the children to "**be a no-op when dropped**"
  (cancellation-safe).

The two camps **demand different contracts from the children, but both demand the contract be written down** - which is
exactly the piece we lack today (there is one prose sentence, "the child gets destroyed"). C#/Trio additionally settle
"do not drop failures" (aggregation) and "cancellation is an independent state".

### 6.2 The decision: put the policy in a parameter, keep the default unchanged

```cpp
enum class CancelPolicy
{
    Destroy,      // destroy the unfinished children as soon as the event fires (today's semantics; children must be cancellation-safe)
    StopAndWait,  // request a stop first, then wait for them to wind down (Trio/Kotlin/P2300 semantics; children must respond to the stop)
};
```

- **The triggering event does not change**: All = the first failure or an external cancellation; Any = the first
  completion or an external cancellation.
- `Destroy`: after the event, **call `request_stop()` first and then destroy** (one line of cost; a cooperative child
  still sees it if it happens to be winding down).
- `StopAndWait`: after `request_stop()`, **wait until `remaining == 0` before completing**; the children's results are
  still discarded and the first exception is still rethrown (**deliberately no ExceptionGroup**).
- Why not "replace the semantics": a batch of cases today (`whenAny(never(), sleepFor(...))` and the like) relies on
  "never hang", while `StopAndWait`'s specified semantics **is** to wait for an unresponsive child - making it the default
  would change the library's determinism promise. Separating the names (Rust separates `join!`/`select!`, Kotlin
  separates `coroutineScope`/`supervisorScope`, asio separates by a completion-condition parameter) is how the industry
  offers both.
- Why not follow C#'s aggregation and the independent Canceled state: that needs a three-state `Task` result plus a
  rewrite of every combinator (see §1.5), and the benefit in this repository is thin; we keep "cancellation = a typed
  exception, the first failure wins" and **state in the documentation that this is a deliberate deviation**.

### 6.3 The landing shape (do it this way when implementing)

- `detail::WhenState` gains `std::stop_source source`; `WhenChild::start()` / `WhenAnyChild::start()` inject
  `source.get_token()` through **the same** `detail::TaskEnvironment::setToken()` that `Scope::add()` uses.
- `whenRace(WhenMode, tasks, token, CancelPolicy)`, `whenAllImpl` and the typed `whenAny` each gain a policy parameter
  (default `Destroy`).
- `Destroy`: call `source.request_stop()` before throwing/returning.
- `StopAndWait`: after waking on `state->done`, if `remaining != 0`, **re-arm the event and keep waiting** (`AsyncEvent` is
  manually reset, and `Scope::join` already writes it that way - copy it); once done, throw/return by the original rules.
- The same on an external cancellation: `cancelled && policy == StopAndWait` ⇒ request a stop → wait for the wind-down →
  throw `TaskCancelledException`.
- `Scope::join` takes the same policy: today "cancellation only stops the wait", `StopAndWait` changes it to
  "`requestStop()` first, then wait" (which settles the semantics left open in §2).

### 6.4 The verification recipe

- Nail A (`StopAndWait` waits for the wind-down): child 1 throws immediately, child 2 sleeps 50 ms and then **observes the
  stop and returns normally** ⇒ the combinator must throw only after child 2 finishes, and child 2 must have seen
  `stop_requested()`.
- Nail B (`Destroy` does not wait): the same scene under the default policy ⇒ the combinator throws at once.
- Nail C (pin the policy's price as well): a child that never returns + `StopAndWait` ⇒ the combinator has **not** returned
  within 200 ms (that is the contract; the same scene under `Destroy` returns at once).
- Mutation: change `StopAndWait` to "destroy right after the request" ⇒ nail A goes red; drop `request_stop()` ⇒ nail A's
  "saw the stop" assertion goes red.

### 6.5 Not doing (recorded so the discussion is not repeated)

- `ExceptionGroup`-style aggregation (Trio/Kotlin/C#): it needs a new error type + a change to every combinator;
- a `set_stopped` independent channel / an independent `Canceled` state (P2300/C#): see §1.5;
- **waiting unconditionally**: it would change this library's "never hangs" promise - Trio/Kotlin dare to because they have
  **no** destruction route to offer.

### 6.6 Status: landed (2026-09-27): `whenAll` now defaults to "request a stop + wait for the wind-down"

**What triggered this round**: the user asked explicitly for the "wait" semantics (their words, translated: "it has to wait - C# waits for all").
C#'s `Task.WhenAll` never cancels by itself and always waits for all, **handing the token to the tasks** - putting those
two sentences together gives Trio/Kotlin/P2300's "request a stop + wait for the wind-down" (§6.1).

**The shape that actually landed** (`src/base/async/sdk/vine/async/`):

- `Cancellation.hpp`: new `enum class CancelPolicy { Destroy, StopAndWait }` (with the two camps' wording in its comment).
- `When.hpp`: `detail::WhenState`/`WhenAnyState` each hold a `std::stop_source source` and the `policy`; before starting,
  a child gets `source.get_token()` through `detail::TaskEnvironment::setToken()` (the same mechanism as `Scope::add()`);
  `detail::drainChildren(state)` = `request_stop()` + re-arm the manually-reset event and wait for `remaining == 0`; the
  four drivers (`whenRace`/`whenAllImpl`/the typed `whenAll`/the typed `whenAny`) drain by policy after waking.
- `WithTimeout.hpp`: a `policy` parameter (default `Destroy`, forwarded to `whenAny`); `Scope.hpp`: `join(token, policy)`
  (default `Destroy`; under `StopAndWait` it becomes "`requestStop()` first, then wait for pending to reach zero, then
  throw `TaskCancelledException`").
- **The defaults**: every form of `whenAll` = `StopAndWait`; `whenAny`/`withTimeout`/`Scope::join` = `Destroy`.
  The variadic form gains the "policy first" overloads (`whenAll(policy, token, tasks...)` and 7 more), because otherwise
  a caller of the pack form has nowhere to choose a policy.

**Two deviations from §6.3 (recorded honestly)**:

1. `Destroy` does **not** send a stop request (the §6.2 plan was "`request_stop()` first, then destroy"). The reason: the
   request would only fire a batch of its own `stop_callback`s a few microseconds before the frame is destroyed, which
   means nothing to a child that is already destroyed, and it stirs the teardown together with the cross-thread wakeups.
   The benefit is that `Destroy` behaves **word for word** as today, so "the default is unchanged" is verifiable.
2. **The wake-up rules differ by mode**: All mode used to `done.set()` only when the last child finished, so under
   `StopAndWait` "the first failure" could not wake the combinator at all (and there was no way to send the stop request);
   Any mode **did not decrement** `remaining` and woke only on the first completion. Both were changed by policy: All
   wakes early on "the first failure + StopAndWait"; Any now decrements per child and wakes again on "the last loser
   reporting + StopAndWait".

**The verification recipe (already run)**: `test_async` 150 → **156**; the new nails are
`WhenAllAsksTheSurvivorsToStopAndWaitsForThem`, `WhenAllDestroyPolicyNeverAsksTheSurvivorsToStop`,
`WhenAllStopAndWaitOnCancellationAsksAndWaits`, `WhenAllStopAndWaitIsHeldByAChildThatIgnoresTheToken`,
`WhenAnyStopAndWaitIsHeldByALoserItCannotAskToStop`,
`TypedWhenAnyTest.StopAndWaitWaitsForTheLoserItAskedToStop`,
`TypedWhenAnyTest.StopAndWaitIsHeldByALoserItCannotAskToStop` and
`ScopeTest.JoinStopAndWaitAsksTheChildrenToStopAndWaitsForThem` (the old cancellation probe now states `Destroy`
explicitly; it pins the waking thread, not the policy).

**Mutations (8/8 hit, 156/156 green after restoring)**:

| Mutation | Red |
| --- | --- |
| `drainChildren` sends no `request_stop()` | 3 |
| `drainChildren` just `co_return`s (no waiting) | 4 |
| `composeChild` injects no token | 2 |
| `composeAnyChild` injects no token | 1 |
| the erased path's Any branch drops the "last loser" wake-up | 1 |
| the typed `whenAny` drops the same wake-up | 2 |
| the All branch drops the "first failure" early wake-up | 1 |
| `Scope::join`'s StopAndWait sends no `requestStop()` | 1 |

**Two traps hit during development (written down to save a round)**:

1. `async::discard(...)` is a **wrapper coroutine** (`co_await inner`), so the token injection lands on the wrapper ⇒ the
   inner body reads an empty token. That is not a defect but a direct consequence of "the environment is not passed
   inwards automatically" (§1.3 already disproved "a callee reads its caller's environment"): a wrapper that does not
   forward cuts the channel, just as C# passes the token explicitly. The cases therefore put the child straight into the
   container and state the reason in a comment.
2. The typed `whenAny` and `WhenAnyChild` are **indented differently**: a mutation that dropped the last-loser wake-up hit
   only the erased path, and the typed path only went red when run on its own (and exposed that the erased `whenAny` path
   had **no** nail at all - now added). Mutations have to be aimed per path; "there is one place in the file" is not
   enough.

**The old promise still holds**: `whenAll`'s default now **waits for a child that does not read the token** (C# does the
same, see nail C in §6.4); `whenAny`/`withTimeout`/`Scope::join` defaults are unchanged to the letter, and their callers
have to ask for `CancelPolicy::StopAndWait` to enter the waiting semantics.

