# ADR-2026-11-12: adopt graphics-next as the next frame-pipeline design

- Status: Accepted
- Date: 2026-11-12
- Related: `../design/graphics-frame-pipeline.md` (the normative description of the target shape), `../design/graphics-threading.md`
  (threading and the two worlds before the migration), `../design/vsg-backend.md`, `../backlog/graphics.md`,
  `scripts/measure_graphics_claims.py`

## Background

Today "when does that cross-layer copy go stale" has five answers (address + version, mirror + delta, memo + epoch, `markUsed`'s
reference count, residency's parking window), and the bridge and lifetime layers total 2519 lines. Both real incidents (a mirror born inside a frame without a frame token,
`refreshMaterials` changing only publish and lagging behind) sat in this layer; `ContentStore`'s lifetime layer produced another UAF in 2026-11
where "an address was recycled before its row was read" (`.ai/bugs/vsg-content-address-recycled-before-rows.md`).

`graphics-2026-11-11-next` and its six expansion documents propose another shape (four layers, 8 nouns, 3 verbs, 7 invariants),
and they wrote "what evidence would reject it" first as `K1–K11`. After two audit rounds, the proposal's **first four steps had landed** (document fixes, the `Measure*`
thresholds, the chunked first-write copy-on-write snapshot, public identity `ContentId.hpp` + `ContentRef` + row-level epoch recycling), measured at
`K1 = 203 µs ≤ 500`, `K2 = 1.03x`, `K3` passed, and `K7` guaranteed by the `(id, revision)` key.

Its status in the repository before this was "a research proposal / not adopted", with the trigger written as "any one of `K1–K11` is hit". **That hands the "should we do it"
question to measurement, and the measurement first requires the proposal to be built.** The owner stated on 2026-11-12: graphics-next is the next design,
not a candidate. So this ADR adopts it.

## Options

| Option | Benefit | Cost / violation |
|---|---|---|
| A Keep the status quo, the proposal stays a prototype | No risk; the identity/lifetime layer already landed stays usable | The five staleness answers and the mirror/delta/memo stay in orbit; `K4`/`K5` (equivalence, code size) can never be assessed (`G1`/`G7`) |
| B **Adopt the whole proposal, replacing it step by step as §9.3 of `graphics-frame-pipeline.md` says** | One rule, one verb; misuse cannot be expressed; each step can be reverted on its own | Two shapes coexist during the migration; the public API changes shape (plan, publish, session); the last hot-path step (production collection moving to the snapshot) is the riskiest |
| C Rewrite in one bang and merge afterwards | Clean in one go | No way to judge by step; against `C12` and the repository's rhythm of "one revertible commit per step" |
| D Adopt only the identity/lifetime layer that landed, leave the rest in the proposal | Does not touch the public API | Keeps the mirror/delta/memo that need replacing; "publish = one pointer swap" does not hold, so `R9` stays permanently partial |

## Decision

Chose **B**, and three things change with it:

1. **The target shape enters `.ai/design/`**: a new `graphics-frame-pipeline.md` (status: draft) carries the four layers, the 8 nouns,
   the 3 verbs and `C1–C12` / `R1–R39` / `I1–I7` / `N1–N10` / `P#` / `D#` / `T#`. It is the norm for "what the next version is";
   `graphics-threading.md` / `graphics-scene.md` remain "what is now", and where the two conflict the **status field** decides.
2. **`K1–K11` change role**: from "reject the proposal" into **the acceptance threshold and rollback condition of each step**. If a step measures through one of them, revert that step
   and record the conclusion in this ADR (without reopening "should we adopt").
3. **Backlog `T32` leaves the trigger-condition family**: it becomes a concrete step list that can be done now, with the order maintained by §9.3 of
   `graphics-frame-pipeline.md`.

The research drafts are not deleted at once: the `-design` type drafts and seven algorithms, `-api`'s `P1–P32`/`M1–M10`, and `-hot-paths`' pose and
late-dependency details are absorbed into design/ADR step by step, and **each absorbed document is deleted when its step lands** (`.github/skills/design-docs` §1).

## Consequences

- Good: one lifetime chain (`publish → derive → execute`) replaces five staleness rules; stale misuse becomes inexpressible
  (a session shape, a stale handle, a query against a live camera); every step has a threshold already written, so a verdict no longer needs an argument.
- Bad / cost:
  - **Two shapes coexist during the migration**: production collection still goes through `RenderWorld` + `drawListBake` until the last step, and `selectSnapshot`
    has test consumers only. During that time "the design is new, the implementation is old" is a **known state**, not an omission (§9.1 marks every item's status).
  - **Video memory**: the cost of pinning the model (ADR-2026-11-10 §cost) still holds under `I4` - a host-owned snapshot does not pin resources,
    but everything the live snapshot needs resident is kept.
  - **Public API shape**: the plan becomes a value, `publish` returns a `Result`, `prepare` becomes a first-class entry point, and the `addPass` family disappears.
    All of these are listed one by one in §4 of `graphics-frame-pipeline.md`, and land step by step under `C11` (the migration only adds).
- Trigger conditions (when to reopen this ADR): a step measures through its own `K#` and cannot be fixed without violating a `C#`; or
  a second backend appears and changes the premises of `C3`/`C8`.

## Evidence

- The target shape itself: `../design/graphics-frame-pipeline.md` (`C1–C12` / `R1–R39` / `I1–I7` / `N1–N10` / `T1–T31`,
  and §9.3 for the steps). The dated proposal and its two audit rounds - from which `R31–R39` and `K7–K11` came - are
  deleted: their results are what that page states.
- The measurements already landed: `SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` (RelWithDebInfo 203 µs / 293 µs,
  1.03x resident row table); `scripts/measure_graphics_claims.py`; `test_graphics` 559 / `test_vsg` 540 / ctest 30/30.
- The mechanisms already landed: `sdk/vine/graphics/ContentId.hpp` (`Handle` / `Id<Tag>` / `ContentRef` / `ContentRegistry`),
  `Node::id()`, `ContentStore`'s row-level epoch recycling, and `detail::SceneSnapshot` + `selectSnapshot` (prototype).
