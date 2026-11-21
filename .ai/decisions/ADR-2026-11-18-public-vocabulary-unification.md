# ADR-2026-11-18: the public vocabulary is unified on what each thing is

- Status: **Accepted and implemented** (2026-11-18). Closes audit-4 `T-12` / `B12`, which the third round had registered and left
  "not doing by default".
- Date: 2026-11-18
- Scope: the graphics SDK's public names - `RenderCommand`, `DrawDescription`, `MaterialBase` (+ `MaterialBasePtr`),
  `EngineNotReady`, `RenderDevice::createSession` / `releaseSession` - and every page, test and plug-in that names them.
- Related: [ADR-2026-11-16-redesign-seam-and-vocabulary](ADR-2026-11-16-redesign-seam-and-vocabulary.md) (the S2 rename that
  stopped at the noun), [ADR-2026-11-16-authoring-view-and-target-vocabulary](ADR-2026-11-16-authoring-view-and-target-vocabulary.md)
  (the values/objects split these names now express).

## Background

The third round registered five words whose spelling does not say what the thing is (audit `C-A3-02` / `C-A3-03` / `C-A3-08`,
`A3-17`):

1. `RenderCommand` names the engine's artifact, but the value is **one drawable the scene contributed to this frame**;
2. `DrawDescription` names a "description", but the value is **one drawing call** - and a pass may issue several;
3. `MaterialBase` is a base-class name on an interface that is the **read face of a material's values**;
4. `EngineNotReady` is an enum **of reasons** (its only use is `FrameOutcome::reason`), and read as a state;
5. `createSession` / `releaseSession` still use "session", a noun S2 retired in favour of *frame driver*.

The fourth round corrected the register before acting: two of the words the third round listed no longer existed
(`releaseResidency` → `releaseContent`, `SessionId` → `DeviceClock::FrameDriverId`). What was left was five renames with
~1,200 in-tree references and no behavioural change, so the batch could be taken in one go instead of word by word.

## Decision

**Rename all five, keep no alias** (`G1`, `G4`, `G12`).

| Before | Now |
|---|---|
| `RenderCommand` (and `collectRenderCommands`) | `DrawItem` (and `collectDrawItems`); `RenderCommand.hpp` / `.cpp` -> `DrawItem.*` <!-- drift-ok --> |
| `DrawDescription` | `DrawCall` |
| `MaterialBase` / `MaterialBasePtr` (`MaterialBase.hpp` / `.cpp`) | `MaterialValues` / `MaterialValuesPtr` (`MaterialValues.*`) <!-- drift-ok --> |
| `EngineNotReady` (`EngineNotReady.hpp`) | `EngineNotReadyReason` (`EngineNotReadyReason.hpp`) <!-- drift-ok --> |
| `RenderDevice::createSession` / `releaseSession` | `createFrameDriver` / `releaseFrameDriver` |

- The new names are the industry's where one exists (O3DE `DrawItem` / `DrawPacket`, UE `FMeshBatch` / `FMeshDrawCommand`,
  Filament `Material` / `MaterialInstance`), and they keep this module's own order: **item, then call**.
- `RenderFrameExecutor` keeps its name: it is what the device hands out, and the rename is about the *verb* that asks for it.
- Five migration notes (`BC-17`…`BC-21`) live in `graphics-rendering.md` 2.10, the one page a reader consults about the model's
  vocabulary; the guide's 5.1 / 5.2 tables and `.ai/memory/glossary.md` carry the new spellings and the retired ones.

## Rejected roads

| Option | Why not |
|---|---|
| Keep the old spellings as `[[deprecated]]` aliases for one release (B4's shape) | B4 migrated a *shape* whose in-tree callers still existed; a migrated *word* has no callers left after the sweep, and the alias would leave two names in every search, guide table and reader's head - the double spelling the guide's 5.2 table warns about (`G1`) |
| Rename only the cheap words, defer `RenderCommand` / `DrawDescription` | A half-renamed pair (`DrawItem` beside `DrawDescription`) is harder to read than either end state, and the register is read as one vocabulary (`G6`) |
| Keep the words and document the mismatch (what B11 did for the false friends) | Valid for words that are *right but borrowed* (`RenderPass`, `target`); these five state the wrong thing, and the audit's own rule is that a false friend is documented while a wrong name is renamed (`G12`) |
| Rename the member functions on `RenderDevice` only, keep `createSession` as a forwarder | Keeps the retired noun alive in the one place a host actually writes (`G1`), and the plugins' overrides would have to answer two spellings |

## Consequences

**Good**

- One spelling per concept, and the spelling says the kind of thing: an item is drawn, a call is issued, values are read,
  a reason explains, a driver is borrowed.
- The guide's translation table now lines up with Filament / O3DE / UE in the industry's order, so a reader arriving from
  either maps in one step.
- `MaterialValues` completes the trio the design already had (`Material` asset / `MaterialInstance` / the values' read face).

**Cost (stated plainly)**

- **Source break for any out-of-tree host**: `#include <vine/graphics/RenderCommand.hpp>` and the four other spellings are gone.
  There is no in-tree host left to update, and the module promises neither source nor ABI stability; the migration notes are the
  whole of the compensation.
- One commit touches ~1,200 references (≈650 of them test references), so it is a poor base for a parallel branch; it was taken
  when the tree was otherwise quiet.
- The design pages' prose about "the command list" still reads naturally; where a sentence defined the *name* rather than the
  concept it was rewritten rather than sed-replaced.

## Amendment (2026-11-20)

The `DrawDescription` -> `DrawCall` row above is superseded: `DrawCall` is renamed `DrawPacket` by
[ADR-2026-11-20-draw-packet-and-fullscreen-vocabulary](ADR-2026-11-20-draw-packet-and-fullscreen-vocabulary.md),
because the value is one view's whole PACKET of draw items and "draw call" is the industry's word for one submitted
item. The other four decisions above stand unchanged; `BC-18` keeps its number and its note gains a successor line
in `graphics-rendering.md` 2.10.

