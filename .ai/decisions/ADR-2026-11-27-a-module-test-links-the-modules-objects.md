# ADR-2026-11-27: a module's test links the module's objects

- Status: accepted
- Date: 2026-11-27
- Related: `../design/graphics-architecture.md` 2.19 / C6 / D9, `../research/graphics-2026-11-27-review-19.md` `Z9`,
  `cmake/VineLibraryHelper.cmake`, `tests/test_graphics/CMakeLists.txt`

## Background

`test_graphics` linked `vn::Graphics` and also compiled twelve of that module's private translation units into the
same binary (`tests/test_graphics/CMakeLists.txt`'s `target_sources`). The reason the comment gave was a Windows one -
"a DLL exports only what the SDK declares" - and no `if(WIN32)` guarded the list, so on Linux both the executable and
`libvnGraphics` defined `vn::graphics::detail::SnapshotSource` and its neighbours. The suite therefore could not run
under AddressSanitizer:

```
ERROR: AddressSanitizer: odr-violation (0x...):
  [1] size=38 'typeinfo name for vn::graphics::detail::SnapshotSource' ... in build-asan/bin/test_graphics
  [2] size=38 'typeinfo name for vn::graphics::detail::SnapshotSource' ... in build-asan/lib/libvnGraphicsd.so
```

The one sanitizer that finds use-after-free and leaks therefore did not run over the code that owns the scene graph,
the publication channel and the frame pump - which is exactly the code those findings live in.

The cause is not the test's file list. It is that the module had no way for its own test to reach the private half:
Windows and Linux answered that absence differently - a duplicate definition in one, accidental default symbol
visibility in the other - and both answers put two definitions of one class in one process.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Guard the private `target_sources` with `if(WIN32)` | Two lines; the Linux suite links the library's own symbols | The duplicate definition stays on Windows - the platform the guard exists for - so the cause survives and is only invisible there, not absent |
| B. Export the private half with a second export macro and drop the copies everywhere | The test drives the shipped DLL; no build change | Puts `src/` types on the export table, contradicting "`sdk/` is everything exported" and the gate rule that a declaration crossing the DLL boundary lives in the installed API root; exported classes with STL members pull in `dllexport` mechanics (`C4251`) and a cascade of data types; only compilable on Windows, where this tree cannot verify it |
| C. Build a second, duplicate object set for tests (`EXCLUDE_FROM_ALL`) | No product change at all | Every test build recompiles the module (~90 translation units), and the test would exercise a copy whose statics are not the library's |
| D. The module compiles into one object set; the shared library and the test are two images of it | One definition per process on every platform; no extra compile, no export-macro, no ABI change; identical on Linux and Windows | The module's compile-time configuration must reach the object target too (one loop in its CMakeLists); the test binary embeds the module and no longer exercises the installed library's symbol resolution |

## Decision

Option D. `vn_add_library(<var> <ShortName> OBJECT_TARGET <Objects>)` compiles the module's sources once as an object
library, makes the shared library a shell that links it PRIVATE, and publishes `vn::<ShortName>Objects` for the
module's own tests. `tests/test_graphics` links that object set instead of the library plus twelve sources.

## Consequences

- Good: `test_graphics` runs under ASan (687/687, `RESULT: PASS`) and under TSan (687/687, 0 warnings) with no ODR
  report; `ldd bin/test_graphics` names no graphics library; no file list in `tests/`, so a new private unit needs no
  CMake edit; no public header, export macro or ABI changed.
- Bad / cost: the module's CMakeLists applies its public definitions and link libraries to both images, because the
  objects are what compile and the shell is what consumers read; the test binary embeds the module, so it grows by the
  module's size and no longer proves the installed library's export resolution - that surface stays covered by
  `check_export_annotations.py`, `test_vk` and `test_gui`.
- Trigger (when to reopen): a test that must prove the exported surface itself (a plugin-style load of the installed
  library), or a module whose object set would have to be linked by an image that also loads that module's DLL - the
  second is this defect returning, and the answer is to load the module as a plugin instead.

## Evidence

- `tests/test_graphics/CMakeLists.txt` - the link line; the `target_sources` list is gone.
- `cmake/VineLibraryHelper.cmake` - `OBJECT_TARGET`, and `POSITION_INDEPENDENT_CODE` on the object set: an object set
  that feeds a shared object needs PIC, or a `thread_local` reference fails the link with
  `relocation R_X86_64_TPOFF32 against ... s_building_ can not be used when making a shared object: local-exec is
  incompatible with -shared` (measured here, 2026-11-27).
- `src/viz/graphics/CMakeLists.txt` - the one loop over both images, and the generated shader header wired to the
  object set (that is the target that compiles the includers).
- `VN_ASAN_TARGET=test_graphics VN_ASAN_FILTER='*' scripts/asan_check.sh` → `RESULT: PASS`, 687 cases.
- `scripts/tsan_check.sh` → 687/687, 0 ThreadSanitizer warnings.
