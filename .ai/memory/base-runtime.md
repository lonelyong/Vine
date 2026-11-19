# base/runtime facts

- `DynamicLibraryLoader::instance()` is `Retention::KeepMapped`; a directly constructed loader unloads
  at destruction. The `d.release()` comment lives in the KeepMapped branch only.
- Library identity is the canonical path the platform reports (`dlinfo(RTLD_DI_LINKMAP)` on Linux,
  `GetModuleFileNameW` on Windows), falling back to the canonical candidate; `fileName()` keeps the
  spelling that loaded.
- `load()` records the platform reason in `lastError()` (cleared on success). POSIX reasons already name
  the library; Windows reasons are prefixed with the candidate.
- `Cache<TKey, TValue>` is the typed seam (`Cache.hpp`); `InMemoryCache` is its in-memory
  implementation (values by value, **no serialisation**). Every operation returns `CacheOutcome<T>`
  (`Ok`/`Miss`/`Failure`) or `CacheResult`; a `Miss` is not a failure.
- `KeyValueStore` (`KeyValueStore.hpp`) is the persistent seam: String keys, bytes, **absolute
  `system_clock` expiry** (a record must expire across a restart), optional enumeration
  (`CacheError::Unsupported` otherwise). `SerializedCache<TValue, TSerializer>`
  (`SerializedCache.hpp`) is the mechanism-free facade: encode on `set`, decode on `get`, decode
  failure = `Corrupt`, store failure = passthrough. The serializer is a `ValueSerializer` concept,
  injected, never in `Cache`.
- Backends are **not** in `runtime`: a file store (links `vn::IOBase`), a database store (carries its
  driver) and a network store live in their own modules and implement `KeyValueStore` (backlog T1).
  `runtime` keeps its Global+Core dependency rule.
- `set` takes `std::optional<std::chrono::seconds>`: empty/non-positive means keep. The out-parameter
  `get` and `isEmpty` are gone; use `get(...).ok()` / `count()`.
- `InMemoryCache`'s sweeper is a leaked, per-instantiation singleton with a **detached** worker; it
  exits when the registry stays empty and is started again by the next `start()`. Never put a joinable
  `std::thread` in static storage here: `std::thread`'s destructor calls `std::terminate` at exit.
- A sweep period below `kMinSweepPeriod` (1 ms) is raised to it; the period is shared by every instance
  of a `<TKey, TVal>`.
- `InMemoryCache::set` erases then emplaces (`move_if_noexcept`), so `TVal` needs copy-construction, not
  assignment; a throwing copy loses the previous entry.
