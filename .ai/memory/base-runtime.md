# base/runtime facts

- `DynamicLibraryLoader::instance()` is `Retention::KeepMapped`; a directly constructed loader unloads
  at destruction. The `d.release()` comment lives in the KeepMapped branch only.
- Library identity is the canonical path the platform reports (`dlinfo(RTLD_DI_LINKMAP)` on Linux,
  `GetModuleFileNameW` on Windows), falling back to the canonical candidate; `fileName()` keeps the
  spelling that loaded.
- `load()` records the platform reason in `lastError()` (cleared on success). POSIX reasons already name
  the library; Windows reasons are prefixed with the candidate.
- `InMemoryCache`'s sweeper is a leaked, per-instantiation singleton with a **detached** worker; it
  exits when the registry stays empty and is started again by the next `start()`. Never put a joinable
  `std::thread` in static storage here: `std::thread`'s destructor calls `std::terminate` at exit.
- A sweep period below `kMinSweepPeriod` (1 ms) is raised to it; the period is shared by every instance
  of a `<TKey, TVal>`.
- `InMemoryCache::set` erases then emplaces (`move_if_noexcept`), so `TVal` needs copy-construction, not
  assignment; a throwing copy loses the previous entry.
