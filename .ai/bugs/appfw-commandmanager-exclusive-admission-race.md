# CommandManager: an ordinary top-level command is still admitted while Exclusive runs

- **Status**: Open; the design page records the gap, and existing tests do not cover ordinary admission while Exclusive runs.
- **Found**: 2026-11-02
- **Module**: `src/fw/appfw`
- **Files**: `CommandManager::Impl::takeOverForeground()`, `admit()`, `exclusive_busy`

## Symptom and impact

A top-level Exclusive command sets `exclusive_busy` only after it has cancelled the registered live chains and waited for them. During that wait and while the Exclusive command runs, the admission branch for ordinary top-level commands never checks the state, so such a command can still start and run concurrently with the Exclusive command.
That contradicts the intuitive meaning of Exclusive "taking over the foreground", and it can let new concurrent work appear right after the taken-over commands finish.

## Root cause

Exclusive currently implements "take a snapshot of the live chains and cancel them", not an admission barrier that lasts from the start of the takeover to the end of the command. No reservation covers the whole flow between cancelling the snapshot, waiting for the drain and admitting the Exclusive command.

## Suggested fix

1. Set a "takeover in progress" state atomically inside the manager's mutex when the takeover starts, and have ordinary top-level admission check and refuse it.
2. Keep that reservation for the whole execution lifetime of the Exclusive command once the takeover succeeds; clear it through RAII on every path - timeout, abort by `cancelAll()`, exception and normal completion.
3. Decide how two Exclusive requests submitted at the same time are handled; one should hold the takeover right and the other be refused explicitly, rather than each taking a different live-chain snapshot.
4. Check the precedence between the LongRunning busy gate and the Exclusive reservation so that interleaved release does not open admission unexpectedly.

## Verification plan

- Start an Exclusive request while a controllable old command runs; submit an ordinary short command during the takeover wait and verify it does not start.
- Submit an ordinary top-level command while the Exclusive body runs and verify it is still refused.
- Cover drain timeout, abort by `cancelAll()`, two Exclusive requests submitted together and normal completion, and verify the admission state recovers correctly in each.

## Design constraints

The finding touches **G3** (one readable execution flow) and **G10** (concurrent state and lifetime).
