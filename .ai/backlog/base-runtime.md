# base/runtime backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.

## 1. Actionable Now (No Trigger)

Empty.

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| T1 | Persistent cache backends | `Cache` and `KeyValueStore` are the seams and `SerializedCache` is the mechanism-free facade; only the backends are missing. Implement a file store in a module that links `vn::IOBase`, a database store in a module carrying its driver, and a network store in the module owning the transport - each is a `KeyValueStore` (String keys, bytes, absolute expiry) in its own target, never in `runtime`. | The first consumer that needs a cache surviving the process | `../design/base-runtime.md` §2.6, §2.8, §9.2; `../decisions/ADR-2026-11-19-cache-interface-and-backends.md` |
| T2 | Real plugin unload | `Retention::UnloadOnDestruction` exists, but no host can use it for plugins until the command and backend registries are cleared before the unload. | The first host that must unload a plugin at run time | `../design/base-runtime.md` §2.2, §9.2 |
| T3 | Windows identity coverage | `GetModuleFileNameW` and the Win32 `lastError` text are not exercised on the Linux run. | The Windows CI lane starts running `test_runtime` | `../design/base-runtime.md` §9.2 |
