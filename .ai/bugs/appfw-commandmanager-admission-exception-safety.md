# CommandManager: chain admission and activation do not roll back state on an exception

- **Status**: Chain admission and activation fixed (2026-11-18); no allocation-failure injection test exists yet.
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

## Fix (2026-11-18)

- `admit()` checks the gate first, then performs both fallible steps (chain allocation, live-registry push), and only then commits: `exclusive_busy`, `foreground_busy`, `chain` and `foreground`. Everything after the last fallible call is `noexcept`, so the commit is all-or-nothing and a throw leaves no gate claimed and no foreground naming a chain the registry does not know.
- `ChainGuard` records `entered`, set immediately after `chain->enter()` returns. Its destructor always releases the gate claims of `admit()` and releases the stack entry and the run only when the entry happened, so a throw from the `ProgressHost` allocation or from the stack push cannot decrement a run that was never added.
- `leaveStack()` and `leaveChain()` now belong to that one guard (the separate `StackGuard` is gone), so the stack entry and the run count can no longer be released by different objects on different paths.

## Remaining gap

No failure-injection test: exercising these paths needs a controllable allocation failure or a counter that throws, which the test build does not provide. The invariants are documented in `../design/appfw-command-manager.md` (C4 and §2.2) and the normal paths are covered by the `CommandManager_*` cases.

## Verification plan

- Add controllable failure injection at chain allocation, at live-chain container growth, at command-stack growth and at progress-host creation.
- After each failure point verify that `runningCount()`, `currentCommand()`, the busy / Exclusive admission state and the admission of later commands are all back to normal.
- Run the regression cases in a Debug build and confirm the `runs > 0` assertion does not fire.

## Design constraints

The finding touches **G10** (exception safety, allocation and lifetime) and **G11** (state should be observable and testable on its own).
