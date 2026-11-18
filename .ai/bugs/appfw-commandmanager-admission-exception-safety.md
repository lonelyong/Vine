# CommandManager: chain admission and activation do not roll back state on an exception

- **Status**: Open; found by a control-flow review, there is no allocation-failure injection test yet.
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

## Verification plan

- Add controllable failure injection at chain allocation, at live-chain container growth, at command-stack growth and at progress-host creation.
- After each failure point verify that `runningCount()`, `currentCommand()`, the busy / Exclusive admission state and the admission of later commands are all back to normal.
- Run the regression cases in a Debug build and confirm the `runs > 0` assertion does not fire.

## Design constraints

The finding touches **G10** (exception safety, allocation and lifetime) and **G11** (state should be observable and testable on its own).
