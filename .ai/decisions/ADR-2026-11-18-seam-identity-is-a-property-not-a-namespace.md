# ADR-2026-11-18: the seam is a property and a checked list, not a namespace

- Status: **Accepted and implemented** (2026-11-18). Extends [ADR-2026-11-16-no-backend-half](ADR-2026-11-16-no-backend-half.md)
  (which deleted the seam's *directory*) to the *name* question the third-round audit left open (C-A3-10 / T-14).
- Date: 2026-11-18
- Scope: `src/viz/graphics/sdk/vine/graphics/` (nothing moves), `scripts/check_include_hygiene.py` (one added rule),
  `.ai/design/graphics-architecture.md` 2.9, `docs/guides/graphics-backends.md` 5.
- Related: [ADR-2026-11-16-redesign-seam-and-vocabulary](ADR-2026-11-16-redesign-seam-and-vocabulary.md),
  [ADR-2026-11-15-device-is-a-pool-sessions-own-frames](ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md).

## Background

The third-round audit recorded that the seam "has no name" (C-A3-10, audit-4 `T-14`): the eight mechanisms both
backends use live in `vn::graphics` beside the host's model types, so no spelling says "this is the seam". The
comparison it drew was O3DE, which separates `RHI` (device) from `RPI` (rendering), and wgpu, which has one API plus
internal modules. It offered two ways out - a namespace alias group `vn::graphics::backend`, or the file-set gate plus
the pages that describe the seam (landed as B3) - and left the choice open.

Two measured facts decided it.

1. **The mechanism list is not the seam.** `check_include_hygiene.py` measures both sets today: 8 mechanism headers are
   reachable from both backends (`SEAM_HEADERS`), and 25 SDK headers are included **directly** by both
   (`SEAM_SHARED_HEADERS`). An alias group over the eight would assert a boundary at 8 of 25 and leave the other 17
   with no identity at all - a name that understates the thing it names (`G1`).
2. **A second spelling is not an identity.** `namespace backend { using graphics::DrawState; }` gives one class two
   names, and the nested name `vn::graphics::backend` reads beside `vn::backend_vk` / `vn::backend_vsg` as a *plugin*
   namespace - the opposite of the distinction the alias was meant to draw (`G1` / `G4`). ADR-2026-11-16 already
   rejected "keep the namespace, flatten the files" for the same reason: name and reality separate.

## Decision

**The seam is a measured property with one recorded list, not a namespace** (`G1`, `G11`, `G12`).

- **The property**: a mechanism is on the seam when *both* backends reach it - directly, or one hop through another
  seam header. The gate measures it, so a mechanism that stops being shared and a header that starts being shared by
  both are both findings (`SEAM_HEADERS` / `SEAM_SHARED_HEADERS`; the rule landed in B3).
- **The list is the decision point**: changing it is a deliberate edit in the same commit, and the comment above it
  says what the change means (a new mechanism joins, or a backend starts using the model API).
- **The two pages that state the seam must name every mechanism the list holds** - a new rule in the same gate
  (`SEAM_DOCS`). The list is the fact; the pages are its two audiences (the argument in
  `graphics-architecture.md` 2.9, the reader's table in `docs/guides/graphics-backends.md` 5). A page that stops
  naming a mechanism is drift; a mechanism added to the list without a page naming it is an undocumented decision.
- **No namespace, no alias group, no aggregate header.** A plugin's own types stay in `vn::backend_{vk,vsg}`
  (ADR-2026-11-16), and the SDK keeps one spelling per concept.

## Rejected roads

| Option | Why not |
|---|---|
| `namespace vn::graphics::backend { using ...; }` (the audit's option) | Two spellings of one class (`G1`); covers 8 of the 25 SDK headers both backends include, so the name would claim a boundary that does not exist; `graphics::backend::X` reads as a plugin namespace beside `vn::backend_vk` / `vn::backend_vsg` (`G4`) |
| An aggregate header that includes the eight mechanisms | No consumer: a backend TU needs two or three mechanisms, so a TU including all eight pays for eight unused headers and would trip the include-hygiene rule it exists to express (`G8`) |
| Move the mechanisms back into a `backend/` directory | Deleted in ADR-2026-11-16: a path cannot express "both backends need it", and it was maintained from four places |
| Give VSG's `core` namespace the seam names from the SDK side | `core` already sees them through its one `using namespace`; a second bridge would put plugin vocabulary into the SDK (`G13`) |
| Derive the list from the measurement and stop recording it | The set is a *design judgment* (a mechanism or the model API?), and a derived set turns that judgment into whatever the tree happens to do (`G12`) |

## Consequences

**Good**

- One fact, one home: the eight-name list exists in exactly one machine-checked place, and the two pages that restate
  it can no longer drift from it.
- Nothing moved: no ABI change, no include change, no compile-time cost.
- A third backend has a stated test ("do both backends reach it?") instead of guessing from a namespace.

**Cost (stated plainly)**

- A reader cannot tell a mechanism from a model type by its name alone; they read the list, or the guide's 5 table.
  That is the price of one namespace, and the alternative was a name that lies over 17 of 25 shared files.
- The gate now also reads two markdown pages. Renaming a mechanism therefore requires editing the page that names it -
  which is the point, and the failure message says so.
- The check is *coverage*, not staleness: both pages name mechanisms in other sections too, so "a page says a
  mechanism the list does not hold" cannot be decided from prose. A retired mechanism is caught by the unit-name gates
  instead (`check_doc_symbols.py`, `check_ai_docs.py --strict`).
