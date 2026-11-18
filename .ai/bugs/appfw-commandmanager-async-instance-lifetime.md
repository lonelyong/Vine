# CommandManager: the per-instance async entry point does not state how long it borrows the instance

- **Status**: Open; found by a review of the public API lifetime contract, there is no dedicated regression case yet.
- **Found**: 2026-11-02
- **Module**: `src/fw/appfw`
- **Files**: `CommandManager::executeCommandAsync(Command*)`

## Symptom and impact

The per-instance call returns a lazy Task, and the command is reached through the raw pointer once the Task is driven. If the caller destroys the instance before the Task completes, execution can dereference a dangling pointer.
The current comment says the argument must not be null, but not that the caller must keep the instance alive until the task completes.

## Root cause

The interface takes the command as a non-owning raw pointer, while the returned async task may start or finish later than the call returns; the interface documentation does not connect the two lifetimes.

## Suggested fix

1. State an explicit contract on the existing overload: the instance must stay valid until the returned Task completes or is safely destroyed, and the caller must not destroy it concurrently.
2. If the manager is meant to take over the lifetime, add an async overload taking `std::unique_ptr<Command>` and hold the instance in the coroutine frame until execution ends.
3. Keep the raw-pointer overload for existing callers but position it explicitly as a borrowing API; state separately for the synchronous entry point that its borrow only has to cover the synchronous call.
4. Add an ownership test that can be verified, so "destroy the Task" is not mistaken for a guarantee that the command ran or that a running command is cancelled.

## Verification plan

- For the owning overload, verify the command lives during execution and is destroyed afterwards.
- For the borrowing overload, verify the documentation/API test states that the caller holds the instance until the task completes.
- Check `executeCommandAndWait(Command*)` and the other call sites against the borrow scope that applies to them.

## Design constraints

The finding touches **G4** (ownership, lifetime and failure semantics) and **G10** (object lifetime).
