# async backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.
> What is in the code is `../design/async-design.md`. The P2 proposal that shaped the task environment
> token and `Scope`'s stop request has landed; its rationale is in git, and the one decision it took that
> is not visible in the code (no `set_stopped` result channel) is `../design/async-design.md` D7.

## 1. Actionable Now (No Trigger)

(none)

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| A1 | Frame allocation for coroutine frames | give `Task`'s promise an `operator new` / an `allocator_arg_t` constructor over a pool, so a frame stops going through the global `operator new` (today: 0 promise `operator new` in the module) | any of: a use appears that creates a coroutine per frame or per event; the frame allocation share is visible in a profile (`valgrind --tool=massif`, or a counting `operator new` on the promise); a read-only or bare-metal target is introduced | `../design/async-design.md` 9.2 |
| A2 | A composition propagates its token into its children | `whenAll` / `whenAny` inject the environment token into the child they start, instead of the creator having to hand one over from outside (today only `withStopToken` injects) | a combinator site hand-passes one token into two or more children, or a child is found running with an empty environment while its creator held a live one | `src/base/async/sdk/vine/async/StopToken.hpp` |
