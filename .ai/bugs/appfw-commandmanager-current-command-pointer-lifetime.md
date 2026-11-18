# CommandManager::currentCommand cannot guarantee the lifetime of the value it returns across threads

- **Status**: Open; found by a review of the API concurrency contract, there is no dedicated concurrency test.
- **Found**: 2026-11-02
- **Module**: `src/fw/appfw`
- **Files**: `CommandManager::currentCommand()`, `Chain::innermost()`

## Symptom and impact

`currentCommand()` reads the top `Command*` under the chain lock, and the lock is released before it returns. Another thread can end the command and destroy the instance afterwards; if the caller keeps calling member functions through that pointer it can reach a destroyed object.
The documentation says the pointer "is valid only while the command runs", but a concurrent caller cannot tell from that return value when the command ends.

## Root cause

The mutex protects the pointer read only; it does not extend the lifetime of the command instance. `raw_ptr` is an ordinary non-owning pointer and provides neither concurrency safety nor lifetime management.

## Suggested fix

1. Prefer a query with value semantics - for example a snapshot of the current command name or type identity - and have the caller use `cancelCurrent()` for the control operation.
2. If the full command object really has to be reached, introduce a shared owner or execution handle that can be held, and state its relationship to destruction and to plugin unload; do not just turn the raw pointer into a thinner wrapper.
3. If the raw-pointer API stays for now, restrict it explicitly to diagnostic use where external synchronisation is guaranteed for the duration of the call, and do not offer it as a thread-safe query.

## Verification plan

- Add a test that queries concurrently with a command ending, and verify the snapshot API is still safe to read after the command has ended.
- Check every `currentCommand()` call site in the repository and confirm none of them reaches a command through a raw pointer without a lifetime guarantee.

## Design constraints

The finding touches **G4** (return value and lifetime semantics stated) and **G10** (concurrency and object ownership).
