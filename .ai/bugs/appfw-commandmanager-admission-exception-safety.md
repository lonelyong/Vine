# CommandManager: chain admission and activation do not roll back state on an exception

- **Status**: Closed (2026-11-18) as **not modelled**: the only exception these paths can raise is `std::bad_alloc`, and this manager does not turn an allocation failure into a command outcome. The part that was a real defect under any model - a run released by an object that never took it - is fixed by ordering.
- **Found**: 2026-11-02
- **Module**: `src/fw/appfw`
- **Files**: `CommandManager::Impl::admit()`, `executeCommandAsyncImpl()`, `ChainGuard`

## Symptom and impact

If an allocation or a container operation throws while a chain is admitted or a command is activated, a busy state can be left behind or the chain's run count can be broken:

- `admit()` sets `exclusive_busy` before it creates the chain and registers it in the live chain set. A later allocation failure can leave Exclusive occupied forever.
- `ChainGuard` is constructed before the progress host is created and before `chain->enter()`; if an earlier step throws it still calls `leaveChain()` while `runs` has not been incremented. Debug builds may trip the assertion, other builds may underflow the count.
- `chain->enter()` itself can fail when the stack container grows; that path must also avoid calling `leaveChain()` for a run that was never registered.

These are exception-safety path risks, not a claim that the normal successful path has the same problem.

## Root cause

The admission flag, the chain registration, the stack registration and the RAII cleanup have no single commit point; the guard assumes every earlier step succeeded, but its scope covers invariants that are not established yet.

## Suggested fix

1. Design chain creation and live-registry registration as an admission transaction with the strong exception-safety guarantee; commit shared state such as `foreground` and `exclusive_busy` only after those steps succeed.
2. Make the cleanup guard record explicitly which resources and counts it actually acquired, and release only those; do not rely on the destruction order of local variables to imply "already in the chain".
3. Make the pairing between a successful `chain->enter()` and `leaveChain()` explicit; a failed stack registration must not decrement the run count.
4. Avoid work that can allocate while holding the admission lock; where that is impossible, make the exception path roll back every state change.

## Decision (2026-11-18)

- What can throw here, exhaustively: `std::make_shared<Chain>()`, the live-chain `push_back`, the command-stack `push_back`, and two allocations inside the `ProgressHost` constructor (`ProgressIndicator`'s `new`, the registry `push_back`). There is no validation, no user callback and no virtual call between the gate claim and the entry, so nothing but `std::bad_alloc` exists on this path.
- The manager does not model allocation failure, and it never did: `admit()` had no rollback, and `runGuarded` converts exceptions from *command bodies*, not the manager's own allocations. Defending one of these four allocations and not the others would leave a reader unable to tell which throws are handled - `G6` asks for one stance, and the stance is "not modelled".
- Kept: `chain->enter()` runs **before** the guard exists, and the guard releases the stack entry, the run and the gate claims. That deletes the separate `StackGuard`, and it also covers everything after the entry - including the `changed()` handlers the progress host fires - so those are released normally. This is what removes the finding's (b) and (c): the run can no longer be released by an object that did not take it.
- Reverted: the `entered` flag and the `admit()` commit-after-allocate reorder. Both existed only for `bad_alloc`, and the flag in particular made the guard's scope depend on a boolean instead of on where it was constructed.
- Consequence, accepted: if an allocation on that path does fail, a gate claimed just before it stays claimed and the manager refuses further top-level commands. That is the behaviour of not modelling the failure, and it is stated here instead of being defended in one spot.

## Verification plan

- Add controllable failure injection at chain allocation, at live-chain container growth, at command-stack growth and at progress-host creation.
- After each failure point verify that `runningCount()`, `currentCommand()`, the busy / Exclusive admission state and the admission of later commands are all back to normal.
- Run the regression cases in a Debug build and confirm the `runs > 0` assertion does not fire.

## Design constraints

The finding touches **G10** (exception safety, allocation and lifetime) and **G11** (state should be observable and testable on its own).
