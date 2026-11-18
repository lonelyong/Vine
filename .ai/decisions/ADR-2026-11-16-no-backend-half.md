# ADR-2026-11-16: the "backend half" is deleted - shared mechanisms return to the SDK mainline and a plugin's own types go back to the plugin

- Status: **Accepted and implemented** (2026-11-16). Supersedes the half of [ADR-2026-11-16-redesign-seam-and-vocabulary](ADR-2026-11-16-redesign-seam-and-vocabulary.md)
  §2 and batch 3 that made a directory the seam; that ADR's vocabulary, phases, refusal shape and remaining batches are unchanged.
- Date: 2026-11-16
- Scope: `src/viz/graphics/sdk/vine/graphics/backend/**` (deleted), `src/viz/graphics/src/backend/**` (deleted),
  `scripts/check_include_hygiene.py`, `scripts/check_export_annotations.py`, `scripts/check_vk_exposure.py`,
  `cmake/VineLibraryHelper.cmake`, `src/viz/graphics/CMakeLists.txt`, `gfx_backend_{vk,vsg}/CMakeLists.txt`,
  `tests/*/CMakeLists.txt`, `graphics_global.hpp`, the `vn::backend_vsg` namespace, `graphics-architecture.md` §2.9.
- Related: `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`, `ADR-2026-11-13-one-engine-one-surface.md`.

## Background

§2 of `ADR-2026-11-16` made `sdk/vine/graphics/backend/` the "backend seam", holding only the mechanisms both backends share, and gave that boundary four things:

1. `BackendHalfGuard.hpp`: every header included it first, and a host that had not defined `VN_GRAPHICS_BACKEND_CONSUMER` **failed to compile**; <!-- drift-ok -->
2. `BACKEND_SDK_SUBDIR`: the default include tree excluded that subtree, which was installed separately as the `VineGraphicsBackendSdk` component;
3. `BACKEND_HALF_AUDIENCE` in `check_include_hygiene.py`: a host may not include that prefix;
4. `SEAM_HEADERS` / `SEAM_SOURCES`: pinning the directory's file set.

Once batch 3 landed, the "the seam does not live up to its name" problem (18 of the 25 headers were read only by vsg) was solved: the remaining 8 mechanisms really are used by both backends.
But the four things still maintained **one and the same boundary**, and the only question that boundary answers is one sentence: "these headers are for the backend, a host must not include them."
One property expressed in four places: a rename, a moved file or a new header in any one of them meant syncing the other three.

## Decision

**Delete the concept of a "backend half"** (`G8`: no new consumer, and the boundary only blocks choices; `G7`: do not express one property with four layers of mechanism).

- The 8 mechanisms go flat into `sdk/vine/graphics/`, and their implementations flat into `src/viz/graphics/src/`:
  `FrameGate`, `DeviceClock`, `FrameTimeline`, `RetirementQueue`, `Readback`, `DrawState` (formerly `Keys`),
  `TargetPlan`, `DiagnosticDraft`.
- The namespace merges into `vn::graphics`; `VN_GRAPHICSBACKEND_NS_BEGIN/END` and `BackendHalfGuard.hpp` are deleted. <!-- drift-ok -->
- The install split is deleted: `vn_add_library`'s `BACKEND_SDK_SUBDIR` parameter and the matching branch in `VineLibraryHelper.cmake` go with it.
- The gates become **about the property**, not the directory: the graphics module (`sdk/` + `src/`) may not include a rendering API or any concrete backend; the exported symbols of the shared
  mechanisms are still checked against a file list (`check_export_annotations.py`). All three keep the guard "zero matching files is a finding".
- **`gfx_backend_vsg`'s own types move to `vn::backend_vsg::core`**. They used to live in `vn::graphics::backend` (the plugin treated the seam namespace as its own layer), which is
  the counter-example to `G13`: a backend's private types must not appear in the SDK's namespace. `core` keeps existing spellings such as `core::DeviceClock` with one
  `using namespace ::VN_ROOT_NS::graphics`, so the 800-odd references do not change.
- `Readback`'s structure `ReadbackResult` is renamed `ReadbackAnswer`: once merged into `vn::graphics` it clashed with the public `ReadbackResult` enum of `RenderDevice.hpp`,
  and the two mean different things (one answers "can this step be done", the other "what did this read-back produce").

## Rejected roads

| Option | Why not |
|---|---|
| Flat files but keep `BackendHalfGuard.hpp` | The guard can only enumerate file names, while "a host may include it" is already the new rule; the guard would stop **legitimate** uses from compiling (a `G8` counter-example: it blocks real usage, not errors) | <!-- drift-ok -->
| Keep the `VineGraphicsBackendSdk` component (CMake excludes by file list) | It needs an 8-file list maintained in both CMake and the gates, exactly the kind of syncing this change removes (`G7`) |
| Keep the `vn::graphics::backend` namespace and only flatten the files | Name and reality separate: `#include <vine/graphics/FrameGate.hpp>` but write `graphics::backend::FrameGate` (`G1` / `G6`) |
| Split the 8 mechanisms into one copy per backend | Two sources of truth, and residency and frame discipline drift (`G1`); their definition is "both backends use it" |
| Leave the vsg private types in `vn::graphics` | One backend's internal types enter the SDK namespace (`G13`), and it sets a bad example for a future backend |

## Consequences

**Good**

- One property ("the SDK does not depend on any concrete backend") is expressed by one dependency rule, no longer by path + guard + install component + file set in four places.
- A plugin's own types finally live in its own namespace (`G13`); `Observe` / `VariantPool` / `PhaseTable` no longer appear in `vn::graphics`.
- One fewer compile-time guard header, one fewer install component, two fewer CMake parameters.

**Cost (stated plainly)**

- A host can now include these mechanism headers, and they are visible in the SDK's namespace - the residue of `G7`: the complexity went from "four mechanisms kept in sync" to
  "a reader has to know these types are for the backend". That fact rests on documentation and review alone.
- The three gates changed from "directory" to "file list / module rule", and must keep the guard "matching 0 files is red", or a rename silently disables the check
  (which has happened twice in this tree).
- Code outside the repository that consumes `VineGraphicsBackendSdk` has to change its includes and namespace (a source-level break).

**Trigger conditions (when to reopen)**

1. A third backend appears and some mechanism is read only by it ⇒ move it into that backend per `G13`, do not leave it in the SDK.
2. A consumer appears where "the host really does read some mechanism" ⇒ name and document it as proper SDK vocabulary instead of restoring a "half".
3. Host misuse (including a header only the backend should use and depending on its behaviour) really happens outside review ⇒ add documentation and an example first, then a gate,
   and do not restore a directory.

## Implementation status

- **Landed (2026-11-16)**: 9 headers → 8 headers flat plus 7 implementations flat; the includes and namespaces rewritten throughout;
  `check_include_hygiene.py`, `check_export_annotations.py` and `check_vk_exposure.py` rescoped;
  `graphics_global.hpp` left with a single set of namespace macros. Evidence: `Graphics` / `gfx_backend_vk` / `gfx_backend_vsg` compile with zero warnings,
  `test_graphics` / `test_vk` / `test_vsg` all green, and the three gates report zero findings.
- **Correction (2026-11-17, audit A3-04)**: the fourth thing in the background, `SEAM_HEADERS` / `SEAM_SOURCES`, **never existed** -
  it was already absent from the code when this document decided to delete it. The gap it left ("which 8 headers are the seam" has no gate assertion) was once
  misdescribed by `docs/guides/graphics-backends.md` as "a gate pins the file set", and that page has been corrected. Adding such a gate is a **new decision**
  (the same family as trigger condition 3), not restoring the "half"; the tracking item is backlog `graphics.md` T39.
