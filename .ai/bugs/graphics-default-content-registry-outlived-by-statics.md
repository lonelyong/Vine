# graphics: the default `ContentRegistry` was destroyed before the statics that release into it

- **Status**: Closed (2026-11-26) - the process default registry is immortal.
- **Found**: 2026-11-26, by the full-suite ThreadSanitizer run of `test_graphics` (review 12)
- **Module**: `src/viz/graphics` (`ContentRegistry`), witnessed by `src/viz/graphics/src/AxesMarker.cpp`
- **Files**: `ContentRegistry::instance()` (`src/viz/graphics/src/ContentRegistry.cpp`), `AxesMarker.cpp`'s
  `s_prototypes`

## Symptom and impact

`test_graphics` ran all 659 cases green and then **aborted at process exit**:

```
SUMMARY: ThreadSanitizer: SEGV ... in vn::graphics::ContentRegistry::release(vn::graphics::Handle)
  #1 MaterialValues::~MaterialValues()        MaterialValues.cpp:15
  #2 Material::~Material()                    Material.hpp:46
  #8 Geometry::~Geometry()                    Geometry.cpp:60
  #14 vn::graphics::(anonymous namespace)::Prototype::~Prototype()  AxesMarker.cpp:35
  #19 std::vector<Prototype>::~vector()
  #20 cxa_at_exit_callback_installed_at
  #22 __do_global_dtors_aux                   crtstuff.c
```

The crash is in a destructor, at library finalisation, reading the registry's `slots_` through a registry object that no
longer exists. Impact in a normal process: any host that unloads the graphics library (a plugin leaving, an embedded
engine torn down at exit) while a content object is still held by a static gets a use-after-destruction instead of a
clean exit. Nothing in the test suite failed; only the exit path did - which is why the plain (non-TSan) run had been
green and this had gone unnoticed.

## Root cause

`ContentRegistry::instance()` returned a **function-local `static ContentRegistry s_default`**, whose destructor is
registered with `__cxa_atexit` at first use, inside the same finalisation pass as the library's own global destructors.
Finalisation runs those handlers in reverse registration order, so the registry (registered later, on first use) is
destroyed **first**, and a file-scope cache registered earlier (`AxesMarker`'s `s_prototypes`, a `std::vector<Prototype>`
holding `Geometry`/`MaterialValues`/`Material`) destroys its objects **afterwards** - releasing handles into a destroyed
registry.

The header promised "it lives as long as the process". The implementation lived as long as the local static's turn in the
finalisation order, which is not the same thing (`G4`: the contract said more than the code did).

## Decision

`instance()` now holds the registry behind a `static ContentRegistry* const`, i.e. constructs it once and **never
destroys it**. That is the only spelling that keeps the promise for every static that might hold a content object - the
alternative fixes (`no_destroy`, a static byte buffer) are the same intentional leak without the allocator's
bookkeeping. The pointer is a static root, so LeakSanitizer does not report the object.

The reasoning is written at the definition, and the header states the lifetime as a fact rather than a wish.

## What would have prevented it

- A content object's destructor reaching a process-wide registry during static destruction is an ordering the registry
  must own, not one each static must discover. A process-wide facility that content destructors call must either be
  immortal or be documented as "must not be reachable from a static destructor" - and the codebase had already chosen
  "immortal" once, for `DynamicLibraryLoader` (`scripts/asan_leaks.supp` records that decision).
- The evidence that caught it was the **full-suite** TSan run, not the filtered one. A filter over the new cases is not
  the same check as `scripts/tsan_check.sh` over a suite, because the exit path belongs to no case.

## Verification

- `scripts/tsan_check.sh` (whole `test_graphics`): 659 cases pass, **0 ThreadSanitizer warnings**, exit 0.
- `scripts/tsan_check.sh` with `VN_TSAN_TARGET=test_vk`: 223 cases, 0 warnings, exit 0.
- Plain `build/bin/test_graphics` / `test_vk`: unchanged, green.
- Leak judging unchanged: an ASan + LeakSanitizer run of the registry / axes cases
  (`build-asan/bin/test_graphics`, `detect_odr_violation=0` to get past a separate, pre-existing
  `detail::SnapshotSource` ODR warning between the test binary and the shared library) ends with **no leak report** -
  the registry is reachable from the static pointer that holds it.
