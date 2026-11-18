# CommandManager: concurrent updates of the disabled preference can overwrite each other

- **Status**: Open; the design page records that this operation assumes it is called from the UI thread, and there is no test of that thread constraint.
- **Found**: 2026-11-02
- **Module**: `src/fw/appfw`
- **Files**: `CommandManager::setCommandEnabled()`, `Impl::setDisabledPreference()`, `ConfigManager`

## Symptom and impact

Two concurrent `setCommandEnabled()` calls can read the same disabled-command list, each modify it, and write it back. The later writer can overwrite the other's preference, so a user setting is lost.
Even when the configuration manager protects the single read and the single write, the read-modify-write across two calls is not atomic.

## Root cause

Updating the disabled preference depends on several steps - read the list, modify a copy, write the list back - while the public API neither declares that it must be called from a single thread nor offers an atomic update.

## Suggested fix

1. Decide the contract first: if the operation may only be called from the application thread, state the thread affinity in the public documentation and add a thread assertion where that is feasible.
2. If concurrent calls are supported, offer an atomic update of the configuration array, or use a synchronisation mechanism that covers the whole update and does not fire re-entrant callbacks while the lock is held.
3. Keep the registry state and the persisted preference consistent; state which result the runtime `enabled` flag takes when the configuration write fails.
4. Do not just lock the single `ConfigManager` getter and setter - that does not fix a lost update between two calls.

## Verification plan

- Enable and disable two different commands concurrently, using a barrier so both write back after reading the old list, and verify both preferences survive.
- If application-thread affinity is chosen, verify that a call from the wrong thread is rejected or diagnosed explicitly instead of racing silently.
- Cover the expected behaviour of the registry enable state and of the persisted state when the configuration write fails.

## Design constraints

The finding touches **G4** (thread and failure contract stated), **G6** (consistent error handling) and **G10** (concurrent state).
