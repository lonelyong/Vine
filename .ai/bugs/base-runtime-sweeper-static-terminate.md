# Bug: a started `InMemoryCache` with static storage duration terminates the process at exit

- **Module**: `src/base/runtime`
- **Status**: Fixed
- **Found**: 2026-11-19, by the `base/runtime` review
- **Files**: `sdk/vine/runtime/InMemoryCache.hpp`, `tests/test_runtime/static_cache_exit/main.cpp`

## Symptom

A process that holds a started cache as a static or global object aborts at exit:

```
main returning with a started static cache...
terminate called without an active exception
Aborted (core dumped)      # exit status 134
```

Nothing in the cache is used after `main` returns; the abort happens during static destruction.

## Root cause

The shared reclaimer was stored as `inline static std::thread s_thread_`, constructed on the first
`start()` and therefore **after** the cache object that registered with it. Static destruction runs
in reverse construction order, so `s_thread_`'s destructor ran while the cache still existed and the
thread was still joinable. `std::thread`'s destructor calls `std::terminate` for a joinable thread.

The same design had a second failure: `shutdown()` moved the thread object out, set a stop flag,
released the lock, and joined. A `start()` that ran between the unlock and the wake-up cleared the
stop flag and spawned a second worker, so the first worker never exited and the join waited for the
next stop of a different thread — a destructor that blocks indefinitely. `shutdown()` could also
construct a thread (and throw) from a destructor.

## Impact and verification

- Deterministic reproduction with a static cache, a started sweeper, and `return 0` from `main`
  (SIGABRT, status 134). The abort is a `std::terminate` with no active exception.
- The join race is reasoned from the code; a two-thread stress probe (40 s) did not hit its
  unlock-to-wake window on this machine, so it was fixed with the same change rather than left as a
  latent defect.
- `InMemoryManager` starts its sweeper in the constructor, so any static `InMemoryManager` reaches
  the same abort.

## Fix

- The per-instantiation sweeper state (`mutex`, condition variable, registry, period, flags) is a
  heap-allocated `Sweeper` that is **never destroyed**; the OS reclaims it with the process.
- The worker is **detached** and never joined. It wakes on registration, on a stop request, or on
  its period, sweeps every registered instance under the registry lock, and exits when the registry
  has stayed empty. `start()` clears the stop request under the same lock, so a start/stop race
  cannot lose a stop or leave a second worker.
- `shutdown()` only unregisters and wakes the worker: no thread creation, no join, nothing that can
  block or throw from a destructor.
- A sweep period below `kMinSweepPeriod` (1 ms) is raised to it, so a zero period cannot spin the
  worker.

## Verification

- `runtime_static_cache_exit` (CTest): the same static-cache program exits 0.
- `InMemoryCacheTest.StartStopChurnFromSeveralThreadsLeavesAWorkingWorker`: four threads create,
  start, stop and destroy caches 50 times each; a new cache is still swept afterwards.
- `InMemoryCacheTest.OneWorkerSweepsEveryStartedInstance`: two started caches both lose their
  expired entry with no explicit `removeExpired()`.
- `test_runtime` 35 cases, `runtime_static_cache_exit` 1/1, all green.
