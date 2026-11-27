# ADR-2026-11-27: a graphics pipeline states two stages, and the builder is where that is required

- Status: accepted
- Date: 2026-11-27
- Related: `../design/vk-backend.md` 2.22 / C15 / P20 / T12 / 9.3,
  `../design/graphics-rendering.md` F20,
  `../research/graphics-2026-11-27-review-19.md` `Z16`,
  `src/plugins/gfx_backend_vk/{include/vine/backend_vk/internal/ContentCaches.hpp,src/internal/ContentCaches.cpp,src/internal/ContentRefusal.cpp}`

## Background

Review 19 `Z16` reported that a program with no vertex and no fragment module "is cached as successfully compiled, and
the pipeline build hands `VK_NULL_HANDLE` to `vkCreateGraphicsPipelines`". The second half does not hold in the tree, and
has not since before the review: `ensureContentPipeline` and `ensureFullscreenPipeline` each refused a pair with a null
module (`git log -S "a content program needs a vertex and a fragment stage"` finds a commit older than the review), so the
driver was never called with an invalid handle. The finding's own remedy - "a cached entry with a null module is
`failed` with a refusal code" - is wrong, and this ADR records why.

What the tree DID do wrong, and what this batch fixes:

1. **The requirement was stated twice, at the wrong level.** Two `ensure*Pipeline` functions each tested their modules
   with their own sentence, and `buildPipeline` - the one place a module becomes a `VkPipelineShaderStageCreateInfo` -
   stated them unconditionally. A third pipeline path would have had to remember the check, and neither sentence named
   the stage that was missing ("needs a vertex and a fragment stage" asks the reader which one is absent).
2. **The refusal was reported as the wrong thing.** A program with no vertex stage reached the planner as a *pipeline*
   failure: `vk.content.pipeline_refused`, whose sentence is "vkCreateGraphicsPipelines refused the state the draw
   resolves to". The driver was never called, and a host that reads the code goes looking at blend and depth state for a
   defect in the shader's stage list - exactly what the code vocabulary (§2.14) exists to prevent.
3. **A stage was dropped in silence.** `ensureProgram` did `continue` on a `Compute` stage ("M2 has no compute path"), so
   a compute-only program was cached as *compiled* with no modules and then reported as a missing graphics stage. A
   capability the backend lacks has to be reported as that capability, not as a symptom of it (the module's own rule,
   §2.8: "absent is not zero").

The concept behind the rule: **a program is a value, not a verdict.** A `ShaderProgram` is editable to any stage list
(`clearStages` then `addStage`, a fragment-only file, a compute stage), and the same description can be complete for one
use and incomplete for another - a fragment-only program IS a whole full-screen program, because that path states the
engine's canonical vertex stage itself.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. The builder owns the requirement: `buildPipeline` refuses a pair with a hole, names the stage, and answers a `PipelineFailure`; both `ensure*Pipeline` guards go; a compute stage is refused at the program entry; a new cause `ProgramStageMissing` reports it | One statement, where the driver is called, covering every present and future path; the report names the program instead of the state; no stage disappears silently | An out-parameter on three internal signatures, one new cause and code, and a second `test_vk` fixture set |
| B. The review's shape: fail the *program entry* when it resolves no vertex module | The draw is refused earlier (before its geometry uploads) | Refuses a LEGAL use for the session: a fragment-only program is a complete full-screen program (`planFullscreen` compiles exactly that). A verdict per (program, need) would be two answers under one key |
| C. Keep the guard where it is and report the hole under `pipeline_refused` | Smallest diff | The code's sentence is false ("the driver refused the state") and sends a host to the state for a defect in the program - the finding's own complaint, one layer up |
| D. Add the guard to `buildPipeline` and keep the two `ensure*` ones | No signature change | Three statements of one requirement; the next editor still cannot see which one is authoritative (`G1`) |
| E. Validate stage completeness in the engine's frame compile (an `engine.*` code) | Reported before any backend work, and portable | A layer that does not state a pipeline's stages answers for the API's requirement - a second answer, and a per-frame scan for a fact that changes only with the program's revision |
| F. Make `ShaderProgram` refuse a description without both stages | Impossible to express | Forbids a legal editing state (`clearStages()` then two `addStage()`s), and refuses every fragment-only full-screen program |
| G. Add `ShaderProgramDescription::declaresStage()` and let each caller check | A named query | The rule becomes each caller's to remember, which is precisely what option A removes |

## Decision

Option A.

**The requirement lives at the API call.** `buildPipeline` - the only site that hands modules to
`vkCreateGraphicsPipelines` - refuses a stage pair with a null module, naming the stage in its sentence, and answers
`PipelineFailure::StageMissing`. The two guards in `ensureContentPipeline` / `ensureFullscreenPipeline` are deleted, so
there is one statement and it cannot be forgotten by a new path.

**Two failures, two codes.** The builder answers `PipelineFailure` (`StageMissing` / `DriverRefused`); the planners map
it to `vk.content.program_stage_missing` (a new cause: the repair is the program's stage list) or to their path's
pipeline code (`vk.content.pipeline_refused` / `vk.content.screen_pipeline_refused`: the repair is the state, and the
loader's report says what).

**No stage is dropped in silence.** `ensureProgram` refuses a `Compute` stage with `vk.content.program_refused` and a
sentence that says this backend stages no compute program. An entry that resolved nothing is never cached as usable, and
the draw is reported for what is actually wrong.

**A program stays a value.** Nothing about stage completeness is cached on `ProgramEntry`; the question is asked per use,
which is what keeps a fragment-only full-screen program working.

## Consequences

- One requirement, one sentence, one place: a null module can no longer reach the driver from any path, and the
  diagnostic names the program.
- A fragment-only program keeps working for the full-screen path and is refused for a content draw - each use states its
  own stages, and neither poisons the other.
- Internal-only API change (three signatures inside `ContentCaches`, one new `ContentRefusalCause` member); the plugin ABI
  the host sees is unchanged. One new diagnostic code, `vk.content.program_stage_missing`; no code was renamed or reused.
- Residuals. The refusal lands after the geometry upload (the pipeline is resolved last), so the first frame that names
  such a program uploads its geometry before being refused - one upload, once, and the alternative is a second statement
  of the rule. A `Compute` stage is refused, not implemented: `M2` is where a compute pipeline would state its own
  requirement instead. The sentence for a driver refusal is now the pipeline's ("the graphics pipeline could not be
  built"), where the full-screen path used to forward the content path's wording.

## Evidence

- `VkContentDrawTest.AProgramWithNoVertexStageIsRefusedAsAProgramFault`: the code is `vk.content.program_stage_missing`
  (never `pipeline_refused`), the category is `CompileFailed`, the sentence names the vertex stage, and the target keeps
  its clear colour.
- `VkContentDrawTest.AFragmentOnlyProgramIsAWholeFullScreenProgram`: the same kind of program drives a full-screen pass
  and the picture is the source's colour - the case that fails under option B.
- `VkContentDrawTest.AComputeStageIsRefusedRatherThanDropped`: a compute-only program is refused with `program_refused`
  and a sentence naming compute.
- Mutations, each restored afterwards: reporting every pipeline failure as `pipeline_refused` fails the first case at
  "a program's missing stage was reported as a state the driver refused"; failing the entry on a missing vertex module
  (option B) fails the second; removing the builder's guard lets the null handle reach the driver and the draw is
  reported as a pipeline refusal, which the first case also catches.
