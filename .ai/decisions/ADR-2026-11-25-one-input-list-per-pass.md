# ADR-2026-11-25: one list of declared inputs per pass — one identity per entry, one order for every reader

- Status: **accepted** (2026-11-25).
- Date: 2026-11-25
- Scope: the SDK's pass declaration (`PassDesc::inputs`, `ImageInput`), the plan validator and description
  (`Plan.hpp` / `Plan.cpp`), the frame compile (`EngineCompile.cpp`), the host-facing frame table (`FrameTable`),
  the owned frame (`OwnedFrame.hpp`), and the authoring recipes (`ChainBuilder`, `ChainPresets`, the guides).
- Related: `graphics-rendering.md` §2.3, §2.13, `C7`, `N13`, §6.5, `D11`-`D16`, `T8`; `F27`; backlog item `T56`
  (deleted by this batch). `ImageRef.hpp` ("why an object and not a name"), `T30` (plan validation), `D5`
  (no shadow field on the pass).

## Background

`PassDesc` stated what a pass reads three times:

- `inputs` — `std::vector<ImageInput>` (produced image attachment, or imported texture);
- `input_targets` — `std::vector<intrusive_ptr<RenderTarget>>` (a whole target);
- `input_names` — `std::vector<String>` (a published name).

Four readers each read a different subset, and they disagreed about what had been declared:

| Reader | Read | Consequence |
|---|---|---|
| Frame resolution (`resolveRowInputs`) | objects, else names | a name stated beside an object was **silently discarded** ("the names are not consulted at all") |
| `validatePlan` | names only | a plan could not check an object input at all, so recipes stated the same wire by name *and* by object to be validated |
| Scheduler edges | objects + names | one wire produced two edges (harmless, but two answers for one fact) |
| `FrameTable` | names only | an input stated by object — a shadow map, an MRT attachment — **never appeared** in the plan a host reads |

Three concrete defects came from that split, all in-tree:

1. the built-in chain stated the G-buffer twice (`input_names = {"GBuffer"}` + `input_targets = {gbuffer}`) in five
   places, and the demo plugin in four more;
2. `main`'s shadow map (`input_targets = { shadow_map }`) existed in no frame-table row;
3. the `engine.screen.no_input` diagnostic told the host to call `addInput` / `addInputName` — methods that no longer
   exist (three headers' Doxygen referenced them too).

`PassDesc` had already solved "one field, several forms" twice: `Target` (window / explicit / transient) and `Output`
(name / one image / whole target), each with one `isValid()`. The input list was the third statement that had been
left in the old shape.

## Decision

1. **`PassDesc::inputs` is the one list.** `std::vector<ImageInput>`, in binding order; `input_targets` and
   `input_names` are removed, and so are `RenderPass::inputNames()` / `inputTargets()`.
2. **`ImageInput` names exactly one identity**, through four named factories and `Kind`:
   `byImage(ImageRef)` (one attachment a producer binds), `byTarget(RenderTarget)` (a whole target),
   `byName(String)` (a late-bound published name), `byTexture(Texture, Sampler?)` (an imported texture). Fields are
   private, the factories normalise a null handle / empty name to `Kind::None`, and there are **no implicit
   constructors**: the conversion is visible at the call site.
3. **Resolution is a switch, not a precedence rule.** `SessionShape::resolveRowInputs` maps each entry to one
   `ImageSource` in declaration order (`byName` resolves to `ProducedTarget`, since a name publishes a target); an
   entry that did not resolve keeps its slot. The "several names are a fallback list" rule is kept and now stated.
4. **Every other reader walks the same list.** Scheduling edges (`Image::target`, `Target`, `Name` → writer's output
   name), the wiring checks (`Kind::Target` / `Kind::Image` in one loop), the "does it write" predicate,
   `Plan::describe` and `FrameTable::PassRow::inputs` all read `PassDesc::inputs` in order.
5. **Which rule judges which identity is a property of the identity.** `validatePlan` keeps judging only
   `Kind::Name` entries — a plan has no frame, so "was this target produced" is unanswerable there — and the compile
   keeps judging object identities, as before. Both now read the same list.
6. **The frame table spells the declaration.** `PassRow::inputs` stays `std::vector<String>` but carries
   `ImageInput::description()` per entry (`name:X`, `target:X`, `image:label#N`, `texture:D2 WxH`, `(empty)`), which
   is how an input stated by object becomes visible. The text is for reading, never a lookup key.
7. **`OwnedFrame::borrowed()` resolves the three object identities** and answers a `Kind::Name` entry empty, because
   a name lives in the engine's registry; `OwnedPass` says so.

## Rejected options

- **Keep three vectors, document the precedence.** `G4` / `G1`: a field that can be stated and silently ignored is
  not a contract, and one input concept with three storages is the defect being removed.
- **Resolve names at authoring time, keep objects only.** `G9` / `G4`: a transient target is created by the engine
  while it compiles, and a host binding is keyed by name — the author cannot hold the object when it writes the
  declaration.
- **`std::variant<...>` for the four identities.** `G6`: `ImageSource` is a tagged struct (`Kind` + fields); the
  declaration mirroring it makes the two sides one shape to learn. Private fields plus factories already make an
  illegal entry unrepresentable, and the saving is a few dozen bytes per declaration (`G8`).
- **A separate `InputTarget` type beside `ImageInput`.** `G1`: one input, two types; the binding order would need
  two lists again.
- **Have `validatePlan` check object identities too.** `G7`: it would need the host publication set and the transient
  identities inside the plan layer, duplicating the compile's wiring rules (`N13`).
- **Rename the pair to `PassInput` / `ResolvedInput`.** `G1`: the existing pair already says declaration/resolution;
  the rename buys no distinction and costs every search, guide table and reader.

## Consequences

- One wire is stated once, resolved once, ordered once and reported once; "objects win, names are ignored" is gone
  rather than documented.
- A recipe that used to state a name only to satisfy `validatePlan` now states the object it holds, which is also
  the identity the frame resolves — and a recipe that must be validated by name still says `byName`.
- `FrameTable::PassRow::inputs` changed meaning: it is the whole declaration list, spelled, not the name list.
  Hosts that matched a name string must read the `name:` prefix (or walk `PassDesc::inputs` for identity).
- No bootstrap (`BC-`) is recorded: the module promises no source stability and every in-tree consumer moved in the
  same commit.
- Cost: `ImageInput` is a fat value (four owning handles, one used) and authoring is one function call longer per
  entry. Both are paid once per declaration and buy a vocabulary that says what it means.
