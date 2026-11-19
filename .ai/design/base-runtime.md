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
- The persistent cache backends themselves (file, database). `Cache` and its outcome contract are
  defined, and a backend lands with the consumer that needs it (`D8`, backlog `T1`).
- File watching, hash-based invalidation or persistence of the in-memory store.

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

### 2.6 **One typed interface, mechanisms behind it**

- What it is: `Cache<TKey, TValue>` is the seam every backend implements; `InMemoryCache` is the
  in-memory implementation and stores values by value, so a hit copies and never encodes.
- Why: memory, file and database backends must be interchangeable at the call site, while the
  in-memory hot path must not pay for serialisation it can never use. A typed interface keeps the
  value type known to both the caller and the store that encodes it; serialisation lives inside the
  persistent implementation (a serializer injected there), never in the interface.
- Boundary: a persistent implementation is a `KeyValueStore` (String keys, bytes, absolute expiry)
  plus a serializer; a new backend implements the store only (`G9`). The interface is typed on
  purpose — an erased String/`std::any` seam was tried and rejected (`D8`): it cast at every call
  site and hid the value type from the store (`G1`, `G4`).

### 2.7 **An overwrite must not demand more of the value than a copy**

- What it is: `set()` erases an existing entry and emplaces the new one with
  `std::move_if_noexcept`, so `TVal` needs to be copy-constructible, not assignable.
- Why: `insert_or_assign` silently required copy-assignability, which the class documentation did
  not state and which a cache of immutable values does not have.
- Boundary: the trade is the strong exception guarantee — a throwing copy leaves no entry rather than
  the previous value; a cache is allowed to lose an entry, and the caveat is stated. Serves `G4`
  (stated requirements) and `G10` (moves when moving is safe).

### 2.8 **A backend lives with the mechanism it needs**

- What it is: `runtime` defines `Cache`, `KeyValueStore` and the mechanism-free implementations
  (`InMemoryCache`, `SerializedCache`). A file, database or network store is implemented in the
  module that owns that mechanism (an I/O module, a database plugin), depending on `runtime`, never
  the other way round.
- Why: a file store needs `vn::io`, a database store needs its driver, a network store needs a
  socket; pulling any of them into `runtime` would force every plugin user to link it and would break
  the module's Global+Core-only dependency rule (`C4`).
- Boundary: the criterion is the mechanism, not the abstract/concrete split — a memory store needs no
  external mechanism and belongs here; a concrete backend that needs one belongs to that module
  (`G2`, `G5`, `G9`).

## 3. Features

### 3.1 Feature Index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Load and reuse a library | One object per file; search paths and the system search are tried in order | landed |
| F2 | Retention policy | Unload at loader destruction, or keep mapped for the process | landed |
| F3 | Failure reporting | The platform's reason for the last failed candidate | landed |
| F4 | Typed TTL cache | Values by value, lazy expiry on access, no serialisation | landed |
| F5 | Background reclamation | One detached worker per stored type, shared by every started instance | landed |
| F6 | Cache interface | One typed seam; results distinguish a miss from a failure | landed |
| F7 | Persistent backend seam | `KeyValueStore` (bytes + absolute expiry) and `SerializedCache` (values encoded by an injected serializer) | landed; backend implementations trigger-gated |

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

- **Behaviour**: `set(key, value, ttl)` stores a copy; an empty or non-positive `ttl` keeps the
  entry. `get()` answers `Ok` with the value, `Miss` when absent or expired, and evicts an expired
  entry it touches; `contains`, `count` and `keys` answer their value in an outcome.
- **Constraints** (C3): `TVal` must be copy-constructible.
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

- **Behaviour**: `Cache<TKey, TValue>` declares `get`, `set`, `contains`, `remove`, `clear`, `count`
  and `keys`; every one answers `CacheOutcome<T>` or `CacheResult`, so an absent key (`Miss`) and a
  failed store (`Failure`) are different answers. `InMemoryCache` is the in-memory implementation.
- **Constraints** (C5): implementations are safe for concurrent use and may evict lazily, so methods
  are non-const.
- **Requirements** (R6): a lookup distinguishes absent (`Miss`) from failed (`Failure`); the
  in-memory implementation never reports `Failure`.
- **Acceptance** (T6): `InMemoryCacheTest.ImplementsTheCacheInterface`,
  `InMemoryCacheTest.MissingReportsMiss`, `InMemoryCacheTest.NonPositiveLifetimeKeepsTheEntry`.
- **Caveats** (P6): a caller that ignores the result cannot tell a miss from a failure; the loaders
  treat a non-hit as a miss because they hold the in-memory implementation.

### 3.1.7 F7 Persistent backend seam

- **Behaviour**: `KeyValueStore` stores bytes under String keys with an absolute `system_clock`
  expiry; `SerializedCache<TValue, TSerializer>` implements `Cache<String, TValue>` by encoding on
  `set` and decoding on `get`. A store reports a record it cannot decode as no value, which the
  cache turns into `CacheError::Corrupt`; a store error is passed through. `count()`/`keys()` are
  optional and answer `CacheError::Unsupported` when the store cannot enumerate.
- **Constraints** (C6): a store that needs an external mechanism (file system, database, network)
  is implemented outside `runtime`, in the module that owns the mechanism.
- **Requirements** (R7): the cache always hands the store an absolute wall-clock expiry, never a
  duration, so a stored record expires across a restart.
- **Acceptance** (T8): `SerializedCacheTest.*` (round trip, corrupt bytes, store failure, absolute
  expiry, enumeration unsupported).
- **Caveats** (P7): `contains()` reads the record through the store (it does not decode it), so it
  costs one store read; a backend that can answer presence more cheaply can implement `Cache`
  directly instead of using `SerializedCache`.

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
| `Cache` | Typed seam: `get`/`set`/`contains`/`remove`/`clear`/`count`/`keys` | Every operation returns an outcome; implementations are concurrent-safe |
| `CacheOutcome` | `Ok` with a value, `Miss`, or `Failure` with a reason | A miss is an answer, not an error |
| `CacheResult` | `Ok` or `Failure` for operations with no value | `set`/`clear` |
| `InMemoryCache` | Typed, thread-safe, lazily expiring store; one shared worker per instantiation | Header-only template; no serialisation |
| `KeyValueStore` | Bytes under String keys with an absolute expiry; optional enumeration | Implemented by a backend module, never by `runtime` |
| `ValueSerializer` | `encode(TValue) -> bytes`, `decode(bytes) -> optional<TValue>` | A concept; injected into `SerializedCache` |
| `SerializedCache` | `Cache<String, TValue>` over a `KeyValueStore` + serializer | Encodes on `set`, decodes on `get` |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints

- C1: the loader is single-threaded. `load`, `find`, `lastError` and the search-path mutators must
  not run concurrently; the shared instance is driven by the plugin manager on one thread.
- C2: `Retention::KeepMapped` leaks on purpose. Changing it requires clearing the registries that
  reference plugin code first.
- C3: `InMemoryCache` values are stored by value; `TVal` must be copy-constructible.
- C4: `base/runtime` depends on `vn::Global` and `vn::Core` only. No other `base` module includes
  it, so it cannot become a back edge.
- C5: a `Cache` implementation is safe for concurrent use and may evict expired entries lazily, so
  every method is non-const.
- C6: a backend that needs an external mechanism (file system, database, network) lives in the
  module that owns that mechanism and depends on `runtime`; `runtime` never depends on it.

### 5.2 Requirements

- R1: one `DynamicLibrary` per file per loader, whatever spelling loaded it.
- R2: a destroyed `DynamicLibrary` unloads exactly once.
- R3: a failed `load()` is always explainable through `lastError()`.
- R4: expiry is enforced on access even when no worker is running.
- R5: a started cache with static storage duration does not abort at exit.
- R6: a lookup distinguishes absent (`Miss`) from failed (`Failure`); the in-memory implementation
  never reports `Failure`.
- R7: the in-memory implementation never serialises a value, and a persistent one serialises only
  where it stores; a stored record carries an absolute wall-clock expiry.
- R8: an undecodable stored value is `Failure` with `CacheError::Corrupt`, never a `Miss`.

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
| D8 | An erased String/`std::any` cache interface (with its implementation) beside the typed store | `any_cast` and a copy at every call site; the interface names a value type no store knows; two names for one concept (`G1`, `G4`). The typed interface was chosen instead; also rejected: no interface at all (no seam for the planned backends) and a byte-level interface with a capability query (`G4`) | `../decisions/ADR-2026-11-19-cache-interface-and-backends.md` |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
|---|---|---|
| T1 | One object per file across spellings | `DynamicLibraryLoaderTest.SameFileThroughASymlinkIsOneLibrary`, `...SameFileThroughADotSegmentIsOneLibrary` |
| T2 | Retention keeps code mapped | `DynamicLibraryLoaderTest.KeepMappedLoaderOutlivesItsOwnDestruction` |
| T3 | A failed load is explained | `DynamicLibraryLoaderTest.LastErrorExplainsAFailedLoad` |
| T4 | Typed cache behaviour | `test_runtime --gtest_filter='InMemoryCacheTest.*'` |
| T5 | Worker serves every instance, survives churn, static exit is clean | `OneWorkerSweepsEveryStartedInstance`, `StartStopChurnFromSeveralThreadsLeavesAWorkingWorker`, ctest `runtime_static_cache_exit` |
| T6 | The typed interface and its answers | `InMemoryCacheTest.ImplementsTheCacheInterface`, `MissingReportsMiss`, `NonPositiveLifetimeKeepsTheEntry` |
| T8 | Persistent seam: bytes, errors, expiry, optional enumeration | `SerializedCacheTest.*` (10 cases) |
| T9 | Every public declaration is documented | `python3 scripts/check_public_docs.py` |

## 9. Status and Evolution

### 9.1 Current Status

Living. Both facilities are used: `meshio` and `brepio` cache through `InMemoryCache`; `appfw`'s
plugin manager loads through `DynamicLibraryLoader::instance()`. The module's tests are 40 cases
plus the static-exit program.

### 9.2 Known Gaps and Backlog

- No backend implementation exists yet (file, database, network). The contracts (`Cache`,
  `KeyValueStore`) and the mechanism-free `SerializedCache` are in place; each backend is a
  `KeyValueStore` implemented in its own module and lands with its first consumer (backlog `T1`).
- Plugin unloading is not supported. `KeepMapped` is the only safe policy while registries hold
  plugin function pointers; ordering a real unload is a future host responsibility.
- The Windows identity path (`GetModuleFileNameW`) and the Win32 `lastError` text are not exercised
  by the Linux test run.

### 9.3 Compatibility and Migration

- Public API additions: `Cache` with `CacheOutcome` / `CacheResult` / `CacheStatus` / `CacheError`,
  `Retention`, the `explicit DynamicLibraryLoader(Retention)` overload, `lastError()`,
  `InMemoryCache::kDefaultSweepPeriod` / `kMinSweepPeriod`.
- Cache API change: `set` takes an `std::optional<std::chrono::seconds>` lifetime (empty means
  keep), and `get`/`contains`/`remove`/`count`/`keys` return an outcome; the out-parameter `get` and
  `isEmpty` are gone (use `count()`). A caller that held the old `std::optional` result must check
  `ok()`/`missed()` instead.
- Behaviour change: a directly constructed `DynamicLibraryLoader` now unloads its libraries at
  destruction (it leaked before). The process-wide `instance()` is unchanged.
- Behaviour change: cache identity is the canonical file rather than the candidate spelling; a
  caller that relied on two spellings producing two objects must not.
- Cache overwrite no longer requires assignment; a throwing copy now loses the entry.

## 10. References

### 10.1 Code

- `src/base/runtime/sdk/vine/runtime/DynamicLibrary.hpp`
- `src/base/runtime/sdk/vine/runtime/DynamicLibraryLoader.hpp`
- `src/base/runtime/sdk/vine/runtime/Cache.hpp`
- `src/base/runtime/sdk/vine/runtime/InMemoryCache.hpp`
- `src/base/runtime/sdk/vine/runtime/KeyValueStore.hpp`
- `src/base/runtime/sdk/vine/runtime/SerializedCache.hpp`
- `tests/test_runtime/`

### 10.2 Documents and ADRs

- `appfw-plugin-system.md` — the plugin policy that needs `Retention::KeepMapped`.
- `meshio-brepio-design.md` — the loaders that memoise through `InMemoryCache`.
- `../backlog/base-runtime.md` — open, trigger-gated work for the module.
- `../decisions/ADR-2026-11-19-cache-interface-and-backends.md` — why the seam is typed, why
  outcomes carry a miss and a failure separately, where the serializer belongs, and why a backend
  lives outside `runtime`.

### 10.3 External Sources

- `dlopen(3)` / `dlinfo(3)` (glibc), `LoadLibraryEx` / `GetModuleFileName` (Win32).
- `std::thread` / `std::filesystem::weakly_canonical` semantics (C++20).
