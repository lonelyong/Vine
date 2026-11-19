# base/runtime

> **Status**: living
> **Last verified**: 2026-11-19
> **Scope**: `src/base/runtime` — dynamic library loading and the in-memory TTL cache. It does not
> cover the plugin policy built on top (`appfw-plugin-system.md`) or the loaders that memoise
> through the cache (`meshio-brepio-design.md`).
> **Sources**: `sdk/vine/runtime/*.hpp`, `src/*.cpp`, `tests/test_runtime`, `tests/test_runtime/static_cache_exit`.
> **Backlog**: `../backlog/base-runtime.md`

## 1. Overview

### 1.1 Problem and Background

Two process-level facilities sit under every layer above and under no layer below:

- A host that loads code at run time needs one place that performs the load, keeps one object per
  file, and states what happens to the code when that object dies.
- A loader that memoises expensive work needs a cache whose lifetime is cheap, whose values are
  typed at the call site, and whose expired entries are reclaimed without the caller polling.

Both had one contract that the code did not keep: the loader said "the same file is loaded once"
while keying by the spelling it was given, and the cache's background sweeper lived in static
storage as a joinable `std::thread`, which aborts the process at exit when a cache is a static
object. The module is small; the failure modes were not.

### 1.2 Goals

- One loaded object per **file**, whatever spelling or search path produced the file.
- Retention chosen by the owner, not implied by the class: an isolated loader unloads, the
  process-wide plugin loader keeps its code mapped.
- A failed load is explainable to the caller without a debugger.
- A started cache is safe as a static object, contributes no teardown race, and does not spin.
- The cache abstraction has a stated swap point and an honest reason to exist.

### 1.3 Non-goals

- Plugin discovery, ABI handshakes and plugin policy — those belong to `appfw`.
- Unloading plugin code while registries still reference it. The module offers the policy
  (`Retention`); the ordering problem is the host's.
- A distributed cache implementation. `CacheManager` is the seam, not an implementation.
- File watching, hash-based invalidation or persistence.

### 1.4 One-paragraph Conclusion

`base/runtime` is two small value-shaped facilities. The loader's identity is the file the platform
actually opened, its retention is an explicit constructor choice, and its failures carry the
platform's reason. The cache's sweeper is process-lifetime state with a detached worker, so a static
cache cannot abort at exit and a concurrent `start()`/`stop()` cannot block a destructor; the
worker is shared per stored type and sleeps while no cache is registered.

## 2. Design Points

### 2.1 **Identity is the loaded file, not the caller's string**

- What it is: the cache key is the canonical path of the file the platform reports for the loaded
  module (`dlinfo(RTLD_DI_LINKMAP)` on Linux, `GetModuleFileNameW` on Windows), falling back to the
  canonical candidate path when the platform cannot answer. `fileName()` keeps the spelling that
  successfully loaded.
- Why: a symlink, a `dir/./name` spelling and a search-path name are one file; keying by text made
  two objects own one platform handle, made `count()` lie, and made a future unload decrement one
  reference twice.
- Boundary: the platform query is the loader's business; the caller keeps the spelling it used.
  Satisfies `G1` (one definition of "the loaded file") and `G4` (a name that means what it says).

### 2.2 **Retention belongs to the owner**

- What it is: `DynamicLibraryLoader::Retention` is chosen at construction —
  `UnloadOnDestruction` for the default constructor, `KeepMapped` for `instance()`.
- Why: "never unload" was one fw consumer's requirement (plugin code must outlive the registries
  that hold its function pointers) hard-coded into a base destructor, which made `DynamicLibrary`'s
  own destructor unreachable and leaked even test loaders.
- Boundary: `DynamicLibrary` always unloads when destroyed; whether it is ever destroyed is the
  loader's stated policy. Serves `G2` (the policy is not pushed into a lower layer), `G7` (the
  caller does not have to know a hidden leak), and `G10` (the lifetime is costed).

### 2.3 **A failed load is explainable**

- What it is: `load()` records the platform's reason for the last failed candidate in `lastError()`
  (`dlerror` on POSIX, the Win32 error text on Windows) and clears it on success.
- Why: a plugin that exists but misses a dependency was indistinguishable from "not a plugin at
  all", so the host skipped it silently — the one case where a sentence is the whole diagnosis.
- Boundary: the loader reports; the host decides whether the failure is a rejection, a retry or
  silence. Satisfies `G4` (failure mode is part of the interface) and `G11` (the failure can be
  located).

### 2.4 **The sweeper is process-lifetime state with a detached worker**

- What it is: each `<TKey, TVal>` instantiation owns a heap-allocated, never-destroyed `Sweeper`
  (mutex, condition variable, registry, period, two flags). `start()` registers and spawns a
  detached worker if none is alive; `stop()` unregisters and wakes it; the worker exits once the
  registry stays empty.
- Why: a joinable `std::thread` in static storage is destroyed before the caches that registered
  with it, and `std::thread`'s destructor calls `std::terminate`; a join in `shutdown()` additionally
  let a concurrent `start()` clear the stop request and block the join. Both are gone when the
  state is immortal and the worker is never joined.
- Boundary: the worker is per instantiation, not per cache; the state is reclaimed by the OS, not
  by a destructor. Serves `G10` (the lifetime is deliberate and costed) and `G11` (no hidden
  teardown ordering).

### 2.5 **One worker, every instance**

- What it is: every started instance of the same `<TKey, TVal>` registers in the sweeper's set; the
  worker sweeps them all under the registry lock.
- Why: one worker per cache would multiply threads by cache count; per-instantiation sharing keeps
  the count at "one per stored type that ever started", and an idle worker sleeps without a periodic
  wake-up.
- Boundary: the registry lock is the only lock the worker takes before an instance's own lock, and
  instances unregister through the same lock, so no entry is destroyed mid-sweep. Serves `G9` (a new
  key/value type needs no new machinery) and `G1` (one owner of "which caches are swept").

### 2.6 **The cache interface is the swap point, kept without a consumer of its own**

- What it is: `CacheManager` is the String/any interface; `InMemoryManager` is its in-memory
  adapter, kept although its only caller today is `InMemoryManagerTest`.
- Why: a host may register one cache service in `vn::di` and later change the backing store without
  touching call sites; the typed `InMemoryCache` is used directly by `meshio` and `brepio`, so the
  interface is not on their hot path.
- Boundary: this is the module's one knowingly speculative surface. `G8` requires a trigger rather
  than a defence: the backlog records what reopens it (a real service asking for the interface, or
  its removal). The adapter must nevertheless be a correct, documented implementation.

### 2.7 **An overwrite must not demand more of the value than a copy**

- What it is: `set()` erases an existing entry and emplaces the new one with
  `std::move_if_noexcept`, so `TVal` needs to be copy-constructible, not assignable. The
  out-parameter `get()` still assigns and says so.
- Why: `insert_or_assign` silently required copy-assignability, which the class documentation did
  not state and which a cache of immutable values does not have.
- Boundary: the trade is the strong exception guarantee — a throwing copy leaves no entry rather than
  the previous value; a cache is allowed to lose an entry, and the caveat is stated. Serves `G4`
  (stated requirements) and `G10` (moves when moving is safe).

## 3. Features

### 3.1 Feature Index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Load and reuse a library | One object per file; search paths and the system search are tried in order | landed |
| F2 | Retention policy | Unload at loader destruction, or keep mapped for the process | landed |
| F3 | Failure reporting | The platform's reason for the last failed candidate | landed |
| F4 | Typed TTL cache | Values by value, lazy expiry on access, no type erasure | landed |
| F5 | Background reclamation | One detached worker per stored type, shared by every started instance | landed |
| F6 | Cache interface | `CacheManager` + `InMemoryManager`; no production consumer yet | kept, trigger-gated |

### 3.1.1 F1 Load and reuse a library

- **Behaviour**: `load()` resolves a bare name through the search paths (each joined with the name,
  then the bare name itself), loads the first candidate that succeeds, and caches it under the
  canonical path of the file the platform reports. `find()` answers the same lookup without loading.
- **Constraints** (C1): the loader is not thread-safe.
- **Requirements** (R1): one `DynamicLibrary` per file per loader.
- **Acceptance** (T1): `SameFileThroughASymlinkIsOneLibrary`, `SameFileThroughADotSegmentIsOneLibrary`.
- **Caveats** (P1): `find()` cannot match a bare name that only the system loader could resolve,
  because the file's location is unknown until it is loaded.

### 3.1.2 F2 Retention policy

- **Behaviour**: the default constructor unloads its libraries at destruction; `instance()` and an
  explicit `Retention::KeepMapped` loader keep them mapped.
- **Constraints** (C2): `KeepMapped` leaks the loader's state and its libraries deliberately.
- **Requirements** (R2): a `DynamicLibrary` always unloads when it is destroyed; retention controls
  whether that happens.
- **Acceptance** (T2): `KeepMappedLoaderOutlivesItsOwnDestruction`.
- **Caveats** (P2): probing a library after an unloading loader is gone is undefined behaviour; only
  `KeepMapped` promises the code stays mapped.

### 3.1.3 F3 Failure reporting

- **Behaviour**: every failed `load()` leaves the platform's reason in `lastError()`, prefixed with
  the candidate on Windows (the Win32 text does not name the file) and unprefixed on POSIX
  (`dlerror` already does). A successful load clears it.
- **Constraints** (C1): the same single-threaded rule as the rest of the loader.
- **Requirements** (R3): a non-empty `lastError()` always belongs to the call that just returned
  `nullptr`.
- **Acceptance** (T3): `LastErrorExplainsAFailedLoad`.
- **Caveats** (P3): with several candidates, only the last failure is kept — the system-search
  attempt, which is the one that matters after the search paths are exhausted.

### 3.1.4 F4 Typed TTL cache

- **Behaviour**: `set(key, value, ttl)` stores a copy; `ttl <= 0` means forever. `get()` returns
  `std::optional<TVal>` and evicts an expired entry it touches; `contains`, `count`, `isEmpty` and
  `keys` do the same sweep-on-access.
- **Constraints** (C3): `TVal` must be copy-constructible; the out-parameter `get()` also requires
  assignability.
- **Requirements** (R4): expired entries are never returned, even without a background worker.
- **Acceptance** (T4): `InMemoryCacheTest.*` (round trip, TTL, keys, overwrite).
- **Caveats** (P4): an overwrite is erase-then-insert; a throwing copy loses the previous value.

### 3.1.5 F5 Background reclamation

- **Behaviour**: `start(period)` registers the instance and creates the shared worker if needed; the
  worker sweeps every registered instance each period and sleeps while none is registered;
  `stop()` or destruction unregisters. A period below `kMinSweepPeriod` is raised to it.
- **Constraints** (C4): the state is immortal and the worker detached; nothing is joined.
- **Requirements** (R5): a started cache may have static storage duration; process exit must not
  abort because of it.
- **Acceptance** (T5): `OneWorkerSweepsEveryStartedInstance`,
  `StartStopChurnFromSeveralThreadsLeavesAWorkingWorker`, `runtime_static_cache_exit`.
- **Caveats** (P5): a `TVal` destructor that calls back into a cache of the same instantiation
  deadlocks against the registry lock the worker holds during a sweep.

### 3.1.6 F6 Cache interface

- **Behaviour**: `InMemoryManager` implements `CacheManager` over `InMemoryCache<String, std::any>`;
  `set` clamps a lifetime beyond `int`'s range instead of wrapping to "forever".
- **Constraints** (C1): the adapter is as thread-safe as the cache it wraps.
- **Requirements** (R6): the interface's documented "0 keeps forever" is true after the clamp.
- **Acceptance** (T6): `InMemoryManagerTest.*`.
- **Caveats** (P6): a consumer-less surface stays on the checklist in the backlog; it is not
  evidence that a distributed cache is planned.

## 4. Public Interface

### 4.1 DynamicLibraryLoader

| Interface | Contract | Notes |
|---|---|---|
| `DynamicLibraryLoader` | Loads and caches one object per file; not thread-safe | Constructors, destructor, `Retention` in the header |
| `load` / `find` | Borrowed library, owned by the loader; `nullptr` when absent or failed | `lastError()` explains a failed `load` |
| `lastError` | Borrowed platform reason for the last failed load | Cleared by success |
| `instance` | Process-wide loader with `Retention::KeepMapped` | Static; valid for the process lifetime |
| `Retention` | Unload at destruction, or keep mapped | Chosen at construction |
| `count` | Number of distinct files loaded | Not the number of spellings tried |

### 4.2 DynamicLibrary

| Interface | Contract | Notes |
|---|---|---|
| `DynamicLibrary` | Unloads when destroyed; constructed only by the loader | Non-copyable |
| `fileName` | Borrowed spelling that loaded the library | Not the canonical identity |
| `handle` | Borrowed platform handle | Valid while the owner keeps it mapped |
| `resolveSymbol` | Borrowed function pointer, or `nullptr` | `T` must be a function pointer |

### 4.3 Cache

| Interface | Contract | Notes |
|---|---|---|
| `InMemoryCache` | Typed, thread-safe, lazily expiring store; one shared worker per instantiation | Header-only template |
| `CacheManager` | String/any cache interface, safe for concurrent use | The swap point |
| `InMemoryManager` | `CacheManager` over `InMemoryCache<String, std::any>` | Kept without a consumer; see the backlog |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints

- C1: the loader is single-threaded. `load`, `find`, `lastError` and the search-path mutators must
  not run concurrently; the shared instance is driven by the plugin manager on one thread.
- C2: `Retention::KeepMapped` leaks on purpose. Changing it requires clearing the registries that
  reference plugin code first.
- C3: `InMemoryCache` values are stored by value; `TVal` must be copy-constructible.
- C4: `base/runtime` depends on `vn::Global` and `vn::Core` only. No other `base` module includes
  it, so it cannot become a back edge.

### 5.2 Requirements

- R1: one `DynamicLibrary` per file per loader, whatever spelling loaded it.
- R2: a destroyed `DynamicLibrary` unloads exactly once.
- R3: a failed `load()` is always explainable through `lastError()`.
- R4: expiry is enforced on access even when no worker is running.
- R5: a started cache with static storage duration does not abort at exit.
- R6: the `CacheManager` TTL rule ("0 keeps forever") survives the size-to-int conversion.

### 5.3 Invariants

- N1: every cache entry returned by `get()`/`contains()` is unexpired at the time it is returned.
- N2: no joinable `std::thread` exists in static storage anywhere in this module.
- N3: the worker only touches instances that are registered under the registry lock.
- N4: `fileName()` never changes after a library is loaded.

## 6. Caveats and Traps

### 6.1 Feed a cache a type with an expensive destructor

- How it fails: the worker destroys expired values while holding the registry lock; a destructor
  that calls back into a cache of the same instantiation (`stop`, `start`, `clear`) deadlocks.
- How to detect it: a hang whose stack is inside `removeExpired` on the sweeper thread.
- How to prevent it: keep cached values passive; release external resources outside the destructor
  or through a value that only owns memory.

### 6.2 A period set from another instance

- How to fail: `start(period)` updates the shared period for **every** instance of that stored type
  (last caller wins), so a cache asking for one minute can be swept every millisecond by another.
- How to detect it: a sweep frequency that does not match the constructor argument.
- How to prevent it: treat the period as a per-instantiation setting, not per-cache; the default is
  one second.

### 6.3 A bare name only the system loader can resolve

- How it fails: `find("libx.so")` returns `nullptr` although `libx.so` is loaded, because the
  loader keys by the file the platform opened and the caller supplied no path.
- How to detect it: `find` by the same spelling that `load` used returns a library; a bare name that
  needed the system search does not.
- How to prevent it: keep the `DynamicLibrary*` that `load` returned, or load by a path.

### 6.4 Testing retention

- How it fails: a test that calls a symbol after an `UnloadOnDestruction` loader is destroyed
  crashes or silently runs unmapped code.
- How to detect it: the crash happens inside the plugin fixture, not in the loader.
- How to prevent it: only `Retention::KeepMapped` promises the code stays mapped; probe retention
  through that mode.

## 7. Rejected Options

| # | Option | Why not | ADR |
|---|---|---|---|
| D1 | Keep "never unload" in the loader destructor | Made `DynamicLibrary`'s unload path unreachable and leaked isolated loaders; the policy belongs to the owner (`G2`, `G7`) | — |
| D2 | Key the cache by the candidate spelling | Two objects for one file, `count()` lied, one handle owned twice (`G1`, `G4`) | — |
| D3 | Static `std::thread` with a join in `shutdown()` | `std::terminate` at exit for a static cache; a concurrent `start()` could block the join (`G10`, `G11`) | — |
| D4 | One worker per cache instance | Multiplies threads by cache count for no gain (`G10`) | — |
| D5 | `insert_or_assign` and a documented assignability requirement | An undocumented compile-time requirement is worse than losing the strong guarantee a cache does not need (`G4`) | — |
| D6 | Throw from `load()` | Hosts probe optional plugin directories; a refusal is an answer, not an exceptional event (`G3`) | — |
| D7 | Rename `fileName()` to `path()` | The stored value may be a bare name; the rename is churn without a truth gain (`G8`) | — |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
|---|---|---|
| T1 | One object per file across spellings | `DynamicLibraryLoaderTest.SameFileThroughASymlinkIsOneLibrary`, `...SameFileThroughADotSegmentIsOneLibrary` |
| T2 | Retention keeps code mapped | `DynamicLibraryLoaderTest.KeepMappedLoaderOutlivesItsOwnDestruction` |
| T3 | A failed load is explained | `DynamicLibraryLoaderTest.LastErrorExplainsAFailedLoad` |
| T4 | Typed cache behaviour | `test_runtime --gtest_filter='InMemoryCacheTest.*'` |
| T5 | Worker serves every instance, survives churn, static exit is clean | `OneWorkerSweepsEveryStartedInstance`, `StartStopChurnFromSeveralThreadsLeavesAWorkingWorker`, ctest `runtime_static_cache_exit` |
| T6 | Adapter through the interface | `test_runtime --gtest_filter='InMemoryManagerTest.*'` |
| T7 | Every public declaration is documented | `python3 scripts/check_public_docs.py` |

## 9. Status and Evolution

### 9.1 Current Status

Living. Both facilities are used: `meshio` and `brepio` cache through `InMemoryCache`; `appfw`'s
plugin manager loads through `DynamicLibraryLoader::instance()`. The module's tests are 35 cases
plus the static-exit program.

### 9.2 Known Gaps and Backlog

- `CacheManager` / `InMemoryManager` have no production consumer (F6, `G8`). Kept as the stated swap
  point; the backlog records the trigger that either consumes or deletes them.
- Plugin unloading is not supported. `KeepMapped` is the only safe policy while registries hold
  plugin function pointers; ordering a real unload is a future host responsibility.
- The Windows identity path (`GetModuleFileNameW`) and the Win32 `lastError` text are not exercised
  by the Linux test run.

### 9.3 Compatibility and Migration

- Public API additions: `Retention`, the `explicit DynamicLibraryLoader(Retention)` overload,
  `lastError()`, `InMemoryCache::kDefaultSweepPeriod` / `kMinSweepPeriod`.
- Behaviour change: a directly constructed `DynamicLibraryLoader` now unloads its libraries at
  destruction (it leaked before). The process-wide `instance()` is unchanged.
- Behaviour change: cache identity is the canonical file rather than the candidate spelling; a
  caller that relied on two spellings producing two objects must not.
- Cache overwrite no longer requires assignment; a throwing copy now loses the entry.

## 10. References

### 10.1 Code

- `src/base/runtime/sdk/vine/runtime/DynamicLibrary.hpp`
- `src/base/runtime/sdk/vine/runtime/DynamicLibraryLoader.hpp`
- `src/base/runtime/sdk/vine/runtime/InMemoryCache.hpp`
- `src/base/runtime/sdk/vine/runtime/CacheManager.hpp`
- `src/base/runtime/sdk/vine/runtime/InMemoryManager.hpp`
- `tests/test_runtime/`

### 10.2 Documents and ADRs

- `appfw-plugin-system.md` — the plugin policy that needs `Retention::KeepMapped`.
- `meshio-brepio-design.md` — the loaders that memoise through `InMemoryCache`.
- `../backlog/base-runtime.md` — the trigger for the cache interface.

### 10.3 External Sources

- `dlopen(3)` / `dlinfo(3)` (glibc), `LoadLibraryEx` / `GetModuleFileName` (Win32).
- `std::thread` / `std::filesystem::weakly_canonical` semantics (C++20).
