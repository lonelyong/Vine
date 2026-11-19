# base/runtime backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.

## 1. Actionable Now (No Trigger)

Empty.

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| T1 | Consume or retire `CacheManager` / `InMemoryManager` | The interface and its adapter have no production consumer; they are kept as the stated swap point for a host that wants one cache service (e.g. in `vn::di`). Either register such a service or delete both and keep only the typed cache. | A service asks for a swappable cache, or the second review round confirms no consumer after the plugin work | `../design/base-runtime.md` §2.6, §9.2 |
| T2 | Real plugin unload | `Retention::UnloadOnDestruction` exists, but no host can use it for plugins until the command and backend registries are cleared before the unload. | The first host that must unload a plugin at run time | `../design/base-runtime.md` §2.2, §9.2 |
| T3 | Windows identity coverage | `GetModuleFileNameW` and the Win32 `lastError` text are not exercised on the Linux run. | The Windows CI lane starts running `test_runtime` | `../design/base-runtime.md` §9.2 |
