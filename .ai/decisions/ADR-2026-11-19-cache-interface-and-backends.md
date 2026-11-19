# ADR-2026-11-19: one typed cache interface, mechanisms behind it

- Status: accepted
- Date: 2026-11-19
- Related: `../design/base-runtime.md` §2.6, §7 `D8`; `src/base/runtime/sdk/vine/runtime/Cache.hpp`,
  `InMemoryCache.hpp`; `../backlog/base-runtime.md` `T1`

## Background

The module must serve memory, file and database backends through one interface, keep the typed
in-memory store on the hot path, and avoid paying serialisation for values that never leave memory.
Earlier attempts failed in opposite directions: a String/`std::any` interface plus a non-template
implementation over the typed template made one concept wear two names and put a cast at every call
site; deleting the interface left no seam for the planned backends.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Typed `Cache<TKey, TValue>` interface, three-state results; `InMemoryCache` implements it (chosen) | One seam for every mechanism; memory never serialises; a type error is a compile error; a persistent implementation serialises behind the interface | A memory lookup returns a three-state outcome (see Consequences); one vtable per `<TKey, TValue>` |
| B. Erased String/`std::any` interface + implementation | One seam; no template | `any_cast`/copy at every call; the interface names a value type no store knows; two names for one concept (`G1`, `G4`) |
| C. Byte-level interface (`String -> bytes`) + typed facade | The industry default; memory and persistent stores share one representation | A memory cache behind the facade serialises too, or the facade needs a capability query; contradicts "memory does not serialise" |
| D. Dual-form value (`std::any` + bytes) with a capability query | No serialisation in memory, one interface | Every store is two-mode, every caller branches; the interface stops saying what it is (`G4`) |
| E. No interface, one template | Zero speculation (`G8`) | No way to swap in a persistent backend; the planned backends have no entry point |

## Decision

- `Cache<TKey, TValue>` is the seam: `get`, `set`, `contains`, `remove`, `clear`, `count`, `keys`.
- Results are explicit: `CacheOutcome<T>` (`Ok` / `Miss` / `Failure` + `error` + value) and
  `CacheResult` (`Ok` / `Failure` + `error`). `Miss` is an answer, not an error.
- Lifetime is `std::optional<std::chrono::seconds>`; empty or non-positive keeps the entry. A
  persistent store records an absolute expiry, so a restart still expires.
- `InMemoryCache<TKey, TValue>` implements the interface by storing values by value: no serialisation,
  and its background sweeper and `removeExpired()` stay on the concrete type (a persistent store
  enforces expiry itself and has nothing to sweep).
- A persistent implementation is `SerializedCache<TKey, TValue, Serializer>` over a `KeyValueStore`
  (`String` keys, bytes, absolute expiry): the serializer is injected where the store is constructed,
  never part of the interface. A new backend implements `KeyValueStore` only (`G9`).
- The serializer/persistent layer is **not** written yet: it has no consumer. The interface exists
  because its failure channel and lifetime are contracts that would be expensive to change later
  (`G4`); a backend lands with its first consumer (backlog `T1`).
- Layering: `runtime` defines the interface, the memory implementation and (later) the serialized
  facade; a file store links `vn::io` in its own target and a database store carries its own driver,
  so `runtime` keeps its Global+Core dependencies (`G2`).

## Consequences

- Good: one name per role (`Cache`, `InMemoryCache`, `SerializedCache`, `KeyValueStore`,
  `Serializer`); memory pays no encoding; a persistent backend cannot silently turn a failure into a
  miss; the hot path can still call the concrete type, and `InMemoryCache` is `final`, so a
  polymorphic call only happens where a caller chose to hold `Cache&`.
- Bad / cost: a memory lookup returns a three-state outcome, so the loaders translate it (a failure is
  treated as a miss); one vtable per instantiation exists even when it is never used polymorphically.
- Trigger (when to reopen): a second in-memory representation (e.g. a lock-free store) that should
  also implement `Cache`, or a backend whose failure model does not fit `CacheError` (e.g. a remote
  service needing retry semantics) - in that case extend the error vocabulary, not the interface.

## Evidence

- `InMemoryCacheTest.ImplementsTheCacheInterface` drives the memory store through `Cache&`
  (`set`/`get`/`contains`/`remove`).
- `InMemoryCacheTest.MissingReportsMiss` pins `Miss` (and not `Failure`) as the absent answer.
- `InMemoryCacheTest.NonPositiveLifetimeKeepsTheEntry` pins the lifetime rule through the interface.
- `meshio` and `brepio` use `InMemoryCache` directly; their call sites treat an outcome's non-hit as a
  miss.
