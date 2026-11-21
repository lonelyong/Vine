# ADR-2026-11-20: a draw packet is not a draw call, and a full-screen pass is not a screen

- Status: **Accepted and implemented** (2026-11-20). Supersedes the `DrawCall` row of
  [ADR-2026-11-18-public-vocabulary-unification](ADR-2026-11-18-public-vocabulary-unification.md) and closes that
  ADR's remaining alias exceptions.
- Date: 2026-11-20
- Scope: the frame model's public names - `DrawCall` / `OwnedDraw` / `isDrawingCall` / `drawingCallItems`, the
  `Screen` enumerator of `PassKind` / `DrawCall::Kind` / `DrawKind` / `FrameHead::Kind`, `ScreenPass`,
  `FrameOutcome`, the `Capabilities` forwarders, `DeviceClock::WindowId`, the `FrameStats.hpp` header name (renamed below) - and <!-- drift-ok -->
  the three count fields `draws` / `draw_items` / `draw_packets`.
- Related: [ADR-2026-11-18-public-vocabulary-unification](ADR-2026-11-18-public-vocabulary-unification.md) (the
  `DrawItem` / `DrawCall` split this ADR completes), [ADR-2026-11-16-authoring-view-and-target-vocabulary](ADR-2026-11-16-authoring-view-and-target-vocabulary.md)
  (`Capabilities` = `Features` + `Limits`), `.ai/design/graphics-rendering.md` 2.10, `.ai/memory/glossary.md`.

## Background

The 2026-11-20 vocabulary batch (P1/P2) corrected the *sentences* and the *counts*: `FrameCounts` was introduced,
`draw_items` was added beside `draws`, and the public headers stopped claiming that a pass "issues one draw call"
for every drawable it draws. What it did not correct is the name of the value, because a type rename breaks source
and had to be argued first. The measurements below were taken on the tree that batch left behind.

1. **`DrawCall` names the wrong quantity, and has to explain itself.** The type holds ONE VIEW'S WHOLE LIST of
   `DrawItem`s (one pass may issue several packets); a "draw call" in every engine, profiler and API is ONE
   submission - one item, one instanced group. The header has to say so in capitals ("ONE DRAWING CALL IS A REQUEST,
   NOT A SUBMISSION") and the reports carry two numbers because one word means two things: `draws` counted packets,
   `draw_items` counted what a profiler counts. A name whose definition needs a warning is the defect the warning
   describes (`G12`).
2. **`Screen` names the wrong object.** `PassKind::Screen`, `DrawCall::Kind::Screen`, `DrawKind::Screen` and the vk
   `FrameHead::Kind::Screen` all mean "a pass that fills its view with a program", not "a pass that draws to the
   window" - the presenting pass of the ordinary chain is itself one of them. In this module "screen" is the
   window/surface, so the enumerator reads as the thing it is not, and the class `ScreenPass` repeats it in the one
   place a host writes (`G1` / `G4`).
3. **`FrameOutcome` and `FrameResult` are near-synonyms for two different subjects.** `RenderEngine::frame()`
   answers `FrameOutcome` (Ran / Submitted / NotReady): what the CALL did, where `NotReady` means no frame happened
   at all. `RenderEngine::frameResult()` answers `FrameResult`: what the last ANSWERED frame was. Two English
   synonyms, two subjects, one reader who has to check the header to tell them apart.
4. **Two compatibility aliases outlived their migration.** `.ai/memory/glossary.md` 2 already registered
   `Capabilities::Feature` / `supports` / `bit` and `DeviceClock::WindowId` as exceptions "removed by the next batch
   that touches it". `Features::Feature` / `features.supports` / `Features::bit` and `FrameDriverId` are the
   canonical spellings; the forwarders are the double spelling the batch rule forbids.
5. **`FrameStats.hpp` (renamed below) names a type that does not exist.** The header declares `FrameCounters`, `RetentionStats`, <!-- drift-ok -->
   `FrameProgress`, `FrameUploads`, `FrameTiming`, `FrameLocking` and `FrameReport` - there is no `FrameStats` in the
   tree. An include line is vocabulary too, and it currently teaches a name the module does not have.

## Options

| # | Option | Benefit | Cost |
|---|---|---|---|
| 1A | Rename `DrawCall` -> `DrawPacket`, `OwnedDraw` -> `OwnedDrawPacket` | The industry's name for exactly this value (O3DE `DrawPacket`: one packet per view, holding draw items); "draw call" is then free to mean one `DrawItem`, which is what `draws` will count | Source break; one sweep over ~250 references |
| 1B | Keep `DrawCall`, keep the warning in the header | No break | Every reader, guide row and profiler comparison keeps needing the warning; `draws` can never mean the industry's number (`G1`) |
| 1C | Rename `DrawCall` -> `DrawList` | "list" is honest about the contents | A draw list is what a collection PRODUCES (`DrawListSource`, `DrawListBake`); naming the frame value the same would collide with the collection side (`G1`) |
| 2A | `Screen` -> `Fullscreen` in all four enums, `ScreenPass` -> `FullscreenPass` | One spelling for "fills its view with a program", indistinguishable from the window/surface only if the reader wants it to be | Source break; `screenCopyProgram` / `builtin_screen_copy.frag` keep the old word (see Consequences) |
| 2B | Only rename the class, keep the enumerator | Smaller break | The enumerator is the value a host actually writes in a declaration (`kind = PassKind::Screen`), so the ambiguity would survive where it is read most |
| 3A | `FrameOutcome` -> `FrameCallOutcome` | Names its subject (the CALL) the way `PresentOutcome` already does; `FrameResult` and `FrameReport` keep the frame's own names | Source break; the name is longer |
| 3B | Rename `FrameResult` -> `FrameAnswer` instead | Keeps the shorter return-value name on `frame()` | "Answer" is this module's internal metaphor, not the industry's word for the value a host queries; `FrameResult` is the name a reader arrives with |
| 4A | Delete the forwarders and `WindowId` | One spelling per concept; the aliases have zero callers of their own kind | Source break for an out-of-tree host that used the old spelling; the migration notes are the compensation |
| 5A | Rename the header to `FrameReport.hpp` | The include line names the type it declares | A file rename: 17 includes plus one entry in `scripts/check_include_hygiene.py` |

## Decision

**Take 1A, 2A, 3A, 4A and 5A, in one batch, keeping no alias** (`G1`, `G4`, `G12`).

| Before | Now | Note |
|---|---|---|
| `DrawCall` / `OwnedDraw` | `DrawPacket` / `OwnedDrawPacket` | `BC-28` |
| `isDrawingCall()` / `drawingCallItems()` | `packetDraws()` / `drawItemsOf()` | `BC-28` |
| `FrameCounts::draws` (packets) / `draw_items` | `FrameCounts::draw_packets` / `draws` (items) | `BC-29` |
| `FrameCounters` / `PrepareReport` / `FrameResult` `draws` / `draw_items` | the same split: `draw_packets` / `draws` | `BC-29` |
| `PassKind::Screen` / `DrawPacket::Kind::Screen` / `DrawKind::Screen` / `FrameHead::Kind::Screen` | `Fullscreen` | `BC-30` |
| `ScreenPass` / `ScreenPassPtr`; `ScreenDraw` / `recordScreen` / `screen_draw` | `FullscreenPass` / `FullscreenPassPtr` (files `ScreenPass.hpp` / `.cpp` -> `FullscreenPass.*`); `FullscreenDraw` / `recordFullscreen` / `fullscreen_draw` | `BC-30` <!-- drift-ok --> |
| `FrameOutcome` | `FrameCallOutcome` | `BC-31` |
| `Capabilities::Feature` / `supports` / `bit`; `DeviceClock::WindowId` | `Features::Feature` / `features.supports` / `Features::bit`; `FrameDriverId` | `BC-32` |
| `FrameStats.hpp` | `FrameReport.hpp` | `BC-32` <!-- drift-ok --> |

- **`draws` is realigned to the item count.** A field called `draws` in a frame report is read against RenderDoc,
  Nsight and every engine's stat overlay; after this batch it counts `DrawItem`s, the packets get their own name,
  and "a steady frame asks N packets and M draws" is a sentence with no definition attached (`G4`).
- **`Fullscreen` describes the DRAW, not the target.** A full-screen packet may land in a picture-in-picture
  rectangle or the window; what it fills is the pass' view (see `View`), so the enumerator is correct in both
  chains.
- **`PassKind::Scene` is not renamed.** It names the module's own content owner (`Scene`, `SceneSnapshot`) and a
  scene pass may draw lines, points and impostors - `Geometry` would be narrower than the truth and `Content` would
  be ambiguous (a full-screen program is engine content too). The `DrawKind` / `FrameHead::Kind` pair keeps
  `Content` for the same reason it always had: there the alternative to a full-screen payload is a content payload.
- The migration notes (`BC-28`…`BC-32`) live beside the previous vocabulary batch's, in
  `.ai/design/graphics-rendering.md` 2.10.

## Consequences

**Good**

- One vocabulary at every level of the frame model: a `Scene`/`Fullscreen` pass describes a `Scene`/`Fullscreen`
  `DrawPacket`, whose `draws` are `DrawItem`s; the guide's Filament/O3DE/UE mapping table becomes one step per row
  (`DrawPacket` <-> O3DE `DrawPacket`, `DrawItem` <-> O3DE `DrawItem` / UE `FMeshDrawCommand` / Filament
  `RenderPass::Command`).
- The bench log and the HUD speak the same numbers as an external profiler, and `.ai/memory/glossary.md` no longer
  needs the "not the items" warning on its draw-call row.
- The header's apparatus (a capitalised warning and a second count field) is replaced by the name it was
  compensating for (`G7`: the complexity is absorbed in the vocabulary, not pushed into every reader).

**Bad / cost**

- **Source break, again, for any out-of-tree host.** Like the 2026-11-18 batch, the module promises neither source
  nor ABI stability; every in-tree consumer moved in this batch and no alias was kept - a migrated *word* has no
  callers left after the sweep, and an alias would restore the double spelling the batch removes (`G1`).
- One commit touches ~550 references (the `DrawCall` family ~250, `Screen`/`ScreenPass` ~225, `FrameOutcome` 37,
  the count fields ~30, the alias call sites ~25). It is a poor base for a parallel branch.
- **The screen-PROGRAM vocabulary keeps the old word in this batch**: `drawScreenProgram`, `screenCopyProgram`,
  `buildScreenProgramFacts`, `kScreenFragmentSource`, `kBuiltinScreenCopyFrag` and the vsg plugin's program layer. A
  screen-space program - a fragment stage run over a full-screen triangle - is the industry's term, and the last one is a
  build input (the embed list names the generated constant after `builtin_screen_copy.frag`). The DRAW path moved
  (`FullscreenDraw`, `recordFullscreen`, `collectFullscreenDraw`); renaming the program path would mix a shader-embed
  change into a vocabulary sweep. Registered in `.ai/backlog/graphics.md` as `T49` with its trigger.

**Trigger (when to reopen)**

- A consumer needs `PassKind` to express a third execution shape (a compute or a mesh-shading pass): then the kind
  set is redesigned as a whole rather than renamed. Today the closed set has exactly two members and no consumer
  asks for a third (`G8`).

## Evidence

- Measured before the batch: `git grep -w` counted `DrawCall` 246, `OwnedDraw` 5, `ScreenPass` 126,
  `PassKind::Screen` 51, `DrawKind::Screen` 48, `FrameOutcome` 37, `FrameStats.hpp` 22 include/reference lines, <!-- drift-ok -->
  and `Capabilities::Feature`/`supports`/`bit` call sites 25 (plugins and tests).
- The defect that forced the decision is in the tree today: `FrameDescription.hpp`'s `DrawCall` note says "ONE
  DRAWING CALL IS A REQUEST, NOT A SUBMISSION", and `PrepareReport` / `FrameCounters` / `FrameResult` each carry
  `draws` + `draw_items` for that reason (P1/P2, 2026-11-20).
- The industry mapping is not an opinion: O3DE's `DrawPacket` is the per-view bundle of `DrawItem`s, UE's
  `FMeshDrawCommand` and Filament's `RenderPass::Command` are the leaf submission. The old `DrawCall` sat between
  the two and matched neither. ADR-2026-11-18 already cited O3DE's `DrawItem` / `DrawPacket` pair while choosing
  `DrawCall` for the second value: the target vocabulary was recorded then, and this ADR takes the last step.
- The batch's own acceptance is the parity test
  `BackendParityTest.ADrawPacketCarriesItsDrawsAndBothBackendsCountThem` (one packet, two draws, asked and
  recorded, both backends) plus the doc gates listed in `.github/copilot-instructions.md`.
