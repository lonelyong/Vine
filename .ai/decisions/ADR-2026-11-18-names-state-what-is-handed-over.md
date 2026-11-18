# ADR-2026-11-18: a public name says what the caller is handed

- Status: **Accepted and implemented** (2026-11-18). Answers `T41` of
  [graphics-2026-11-18-review-5](../research/graphics-2026-11-18-review-5.md) §2.2 and applies the rule that
  page §2.2 and `.ai/design/graphics-scene.md` 2.12 record.
- Date: 2026-11-18
- Scope: `DiagnosticsHandle.hpp` / `PassChain.hpp` / `RenderEngine.hpp` / `Diagnostics.hpp` (type rename),
  `Geometry.hpp` (one alias deleted), `StateResolution.hpp` / `StateNode.hpp` (one function renamed), and the
  in-tree call sites.
- Related: [ADR-2026-11-18-public-vocabulary-unification](ADR-2026-11-18-public-vocabulary-unification.md)
  (the previous vocabulary batch, `BC-17`-`BC-21`),
  [ADR-2026-11-16-device-is-a-pool-sessions-own-frames](ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md)
  (why the chain holds a lease rather than a reference).

## Background

The fifth review found three public spellings that describe a *neighbouring* thing rather than the thing in the
caller's hand. None of them is a behaviour defect; all three cost a reader the same thing - they have to open the
header to learn what they were given.

1. **`Diagnostics` / `DiagnosticsRoute`.** The engine owns one diagnostics route: it counts, and it forwards to
   the single sink the host installed. A **device** is handed that object by reference
   (`RenderDevice::setDiagnosticsRoute(Diagnostics&)`) - so "route" is the right word there. But the engine hands
   *out* something else: a `Lease`-backed handle onto its route, for `PassChain` and for a host that wants
   `count()` / `isClean()`. That type was called `DiagnosticsRoute` too, so two different things - the route, and
   a borrowed view of it - differed by nothing a reader could see.
2. **`MaterialPtr`, declared twice.** `Material.hpp` declares it (the owner); `Geometry.hpp` declared it again,
   forward-declaring `Material` first, and then never used it. The duplicate is not a compile error - identical
   alias declarations are legal - which is exactly why it survived: a name with two homes teaches the wrong owner
   (`G1`).
3. **Two `resolveRenderState` overloads answering different shapes.** The three-argument form takes an authored
   value plus a pass context and answers the final state *and* the origin of every item
   (`ResolvedCommandState`). The one-argument form takes a folded block, fills what nobody stated, and answers
   `ResolvedRenderState`. They were told apart by arity alone, so a reader could not tell whether the call had
   already walked the tree without opening the header.

## Decision

**A public name states what the caller is handed** (`G1`, `G4`, `G12`): the noun names the thing, and two things
that differ in kind get two spellings.

- `DiagnosticsRoute` → **`DiagnosticsHandle`** (`BC-22`). `Diagnostics` stays the route. The handle's own
  documentation already began "the engine's ONE diagnostic route **as a narrow handle**"; only the spelling lagged
  behind the sentence. `RenderDevice::setDiagnosticsRoute` / `removeDiagnosticsRoute` keep their names: they take
  the route itself, which is what those calls mean.
- The `MaterialPtr` alias in `Geometry.hpp` is **deleted** (`BC-23`). `Material.hpp` is its one home. A caller that
  used the alias through `Geometry.hpp` adds the include it always needed to construct or name a material.
- The one-argument `resolveRenderState(state)` → **`applyStateDefaults(state)`** (`BC-24`). It takes a value and no
  context, so under the derivation rule of `.ai/design/graphics-scene.md` 2.12 it is not a `resolve*`; the name now
  says what it does (it applies defaults to a block whose items may be unset), and `resolve*` is free to mean one
  thing.

## Options rejected

- **Rename the route instead** (`Diagnostics` → `DiagnosticRoute`, leaving `DiagnosticsRoute` as the handle): two
  renames for the same clarity, and it would have left "route" naming the borrowed view - the original confusion
  with more churn.
- **Delete the handle and let the chain hold `Diagnostics&`**: the `Lease` exists because a chain may outlive its
  engine (`PassChain` N1, ADR-2026-11-15), and a reference would dangle in exactly the shutdown order the lease
  was introduced for. Keeping the type and fixing its name is the cheap half of the same fix.
- **Keep both `resolveRenderState` overloads and document the difference**: done for two releases already; the
  arity rule is what failed. A comment cannot be read at the call site, a name can.
- **Keep the `MaterialPtr` re-export for source compatibility**: it is not a re-export of a *different* spelling,
  it is the same alias twice, and nothing in the tree used the copy. Removing it costs a host one include and
  removes one way to learn the wrong owner.
- **Add deprecated aliases** (`using DiagnosticsRoute = DiagnosticsHandle;`): rejected for the same reason as in
  the previous vocabulary batch - a migrated *word* has no callers left after the sweep, and an alias restores the
  double spelling the batch exists to remove.

## Consequences

- **Source-breaking, ABI-neutral**: no signature changed shape and no default changed; a host recompiles. The
  in-tree call sites (43 occurrences in 11 files, plus four design pages and one test) are the whole migration.
- **Two names now say different things**: "the route" (`Diagnostics`) and "a handle to the route"
  (`DiagnosticsHandle`), and `resolve*` means one thing in the derivation vocabulary.
- **`check_public_docs.py`'s baseline is keyed by declaration name**, so the rename shows as new-plus-stale until
  `--update` moves it - the same trap the previous batch recorded.
- **A file rename under `src/` needs one `cmake -B build`**: the source glob is not `CONFIGURE_DEPENDS`, so the old
  `.cpp` keeps a ninja rule that names a file that is gone.
