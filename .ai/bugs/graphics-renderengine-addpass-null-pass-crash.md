# RenderEngine::addPass dereferences an empty pass before it checks it

- **Status**: Open; the existing tests cover only "empty pass with empty content" and "a valid pass", not an empty pass with content.
- **Found**: 2026-11-02
- **Module**: `src/viz/graphics`
- **Files**: `RenderEngine::addPass(intrusive_ptr<RenderPass>, intrusive_ptr<Scene>, int)`, `RenderEngine::insertSlot`

## Symptom and impact

When `content != nullptr`, `addPass(pass, content, order)` calls `slot.pass->setContent(...)` first
(`src/viz/graphics/src/RenderEngine.cpp:384`), and only the later registration checks for the null pointer
(`insertSlot()`, `src/viz/graphics/src/EngineCompile.cpp:569`). Given an empty pass with content, `setContent` is an
ordinary member function (`RenderPass.hpp:133`, it writes `desc_.content`), so calling it on a null pointer dereferences
null: undefined behaviour, usually a crash right there.

The same defect has a second entry point in the opposite direction: `bindPassContent()` writes content only after it has
found a matching slot, so an empty pass is safe there - which shows that "an empty pass is refused" was always the intent
of this API, and only the registration path puts the check too late.

## Root cause

The check and the use live in two different layers: `insertSlot()` is responsible for refusing an empty pass, but
`addPass()` has already bound the content to the pass before it runs. The order is wrong, so the protection does not cover
the use.

## Suggested fix

1. Refuse an empty pass at the very start of `addPass()` (both overloads), return a null pointer and bind **no** content.
2. Whether an empty pass reports a diagnostic is up to consistency with the existing convention: a failed registration is
   a wiring error in the caller, so one `UnsupportedRequest` per episode is fine and so is returning nullptr silently -
   either works, but write it into the header contract so the next reader does not have to guess.
3. Check in the same pass whether the entry points of `addPassOwnedBy()` / `PassRegistry::add()` all go through one
   "check before use" gate, so a third hole does not remain.

## Verification plan

- `RenderEngineTest`: `addPass(nullptr, scene, 0)` returns `nullptr`, `passCount()` does not change and the process does not
  crash.
- Cover both overloads, and add one "empty pass with empty content" case to confirm that behaviour is unchanged.

## Design constraints

The finding touches **G4** (the failure semantics of a null argument must be stated and must match the behaviour) and
**G11** (an obvious wiring mistake should not become a crash).
