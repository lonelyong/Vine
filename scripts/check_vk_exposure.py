#!/usr/bin/env python3
"""Fails when a Vulkan name crosses the exposure boundary the design argument drew.

`.ai/research/graphics-2026-11-13-vk-exposure-boundary.md` §5 splits the tree into four layers, and only
three of them can be broken by accident, because only they have a consumer that would inherit the damage.
This gate is that split made executable:

  1. V2 - the SDK BACKEND face names no backend. `sdk/vine/graphics/*.hpp` is installed as
     `VineGraphicsBackendSdk` and is included by every backend, including the ones that are not Vulkan, so
     one `#include <vulkan/...>` there turns Vulkan into a dependency of the whole backend seam and of the
     install package (§2.3). The face may use no Vulkan SEMANTICS either, as of 2026-11-23: `ImageLayout`
     and `LoadOpVariantKey` left the SDK for the backends that build pass objects (backlog T15,
     `.ai/research/graphics-2026-11-23-review-8.md` 4.7/4.8), a device format code is whatever the backend
     says it is (B8 made it the opaque `DeviceFormat`), and the pass structure is the backend-computed
     `RenderPassKey`. What the face carries is the neutral vocabulary only (`LoadOp` / `StoreOp`,
     `DrawKind`, ...) - and it may not spell the headers.
  2. V4 - the plugin shells name no Vulkan type. `shell/` is the one part of a plugin a host is meant to
     see, and its shape is "a factory that returns `RenderBackend`, and nothing else". Today nobody outside
     the plugin includes it, so this is a SHAPE rule rather than a gate with a victim (§6.3); it is checked
     because the shape is what keeps the layer safe the day it gains a second consumer, and a shape nobody
     checks is a shape that drifts. Comments count: a shell may not even discuss a `Vk*` name, because the
     next edit copies what the comment says.
  3. E1 - the frame path never uses the escape door. `support/Handles.hpp` is the plugin's explicit,
     opt-in way out (a free function returning a `Handles` value, §4.5). Production code that included it
     would make the door load-bearing, and the design's own landing criterion (§8) is that the vk frame
     path does not include it. The plugin offers the door; it must not walk through it.
  4. E2 - the LOW-LEVEL units stay free of the render seam. `MemoryAllocator`, `PipelineCacheFile`,
     `ReadbackStaging`, `WindowSystem` and `Handles` include nothing from `vine/graphics`, which is what
     makes them "a tool could use these beside the plugin" rather than a claim
     (`.ai/design/vk-backend.md` C10). They are not extracted into a library today because no consumer
     outside the plugin exists (G8) - and that option stays a move instead of an archaeology project only
     as long as one convenience include cannot quietly re-tie them to the seam.
  5. V5 - the SDK face states the CONTRACT, not the survey. A public header's prose names no concrete backend
     (nor its plugin directory): a sentence that defines a rule by comparing implementations is the survey, and
     after the reference retired it also points at a tree that no longer exists. The one exception is a
     diagnostic code's prefix (`vk.<category>.<cause>`), which is an identity a host matches
     (`.ai/design/graphics-architecture.md` C4).
  6. V6 - the SDK face names no PLUGIN-INTERNAL unit. V5 catches a backend's NAME; this catches the next step of the
     same leak - a public header explaining the contract by pointing at a type only one plugin keeps in `internal/`
     (`ResidencyClaims`, `TargetTable`), which is the SDK shipping that plugin's vocabulary. The set is derived from
     the tree: every `src/plugins/*/include/**/internal/*.hpp` stem that the SDK does not own (naming the SDK's own
     type is what a contract sentence does: `RetirementQueue` is both) and that has at least two capitalised words
     (`Device` and `Handles` are ordinary words a sentence may use). The scan set is the SDK's headers AND the shipped
     shader sources, because a host reads both. Derivation is what makes the rule serve the NEXT backend: the day a
     second backend brings its own residency table, the SDK may still name neither (backlog T30).

What this gate deliberately does NOT check (§6.2): `internal/` and `support/` may name Vulkan freely, and no
rule here constrains them. There is no consumer to protect - those headers are included only by the plugin
and by tests that already build pipelines and read pixels - and a gate would not remove the Vulkan, it
would force a shadow type (`VkImage` wrapped as `vk::Image`) to get around the check. That is repetition
bought with the appearance of cleanliness, and it would also contradict the plugin's own `internal/`, which
names Vk types by design (G6). A rule there would be a cost with no beneficiary (G8).

The checks are textual on purpose: each asks a question about a line, not about a program, so the answer
cannot be argued with and does not need a compiler.

Usage: python3 scripts/check_vk_exposure.py [--root <repository root>]
Exit status: 1 on findings, 0 when clean.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parent.parent

SDK_FACE = "src/viz/graphics/sdk/vine/graphics"
PLUGIN_SHELLS = "src/plugins/*/include/vine/*/shell"
# E1 covers both halves of the plugin's internals: the sources that run a frame, and the headers that describe how.
VK_INTERNALS = (
    "src/plugins/gfx_backend_vk/src/internal",
    "src/plugins/gfx_backend_vk/include/vine/backend_vk/internal",
)

# E2: the vk units that depend on NOTHING in the render seam (no `vine/graphics` include at all). Today they are the
# low-level half a tool could use beside the plugin, and that property only survives if it is gated: one convenience
# include of `RenderTarget` or `String`-shaped engine type would silently tie them to the seam again. Their being
# graphics-free is what makes "extract a small library later" a move instead of an archaeology project.
VK_GRAPHICS_FREE_UNITS = (
    "src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/MemoryAllocator.hpp",
    "src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/PipelineCacheFile.hpp",
    "src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/ReadbackStaging.hpp",
    "src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/WindowSystem.hpp",
    "src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/Handles.hpp",
    "src/plugins/gfx_backend_vk/src/internal/MemoryAllocator.cpp",
    "src/plugins/gfx_backend_vk/src/internal/PipelineCacheFile.cpp",
    "src/plugins/gfx_backend_vk/src/internal/ReadbackStaging.cpp",
    "src/plugins/gfx_backend_vk/src/internal/WindowSystem.cpp",
)

# V6: where every plugin keeps the units only it may name. The NAMES are derived at run time (see main), because a list
# written here would go stale the day a backend lands its own table - which is exactly the event this rule exists for.
PLUGIN_INTERNAL_DIRS = "src/plugins/*/include/**/internal"

# V2: the graphics SDK's headers must name no render API. `<vulkan` also covers `<vulkan.hpp>`; `<volk` and
# `<vsg` are the other two ways the same dependency arrives, and the design names all three. The scope is the
# WHOLE SDK because every backend and every host includes it - one such include ties them all to that API.
VULKAN_INCLUDE_RE = re.compile(r"#\s*include\s*<(?:vulkan|volk|vsg)")
# V4: a `Vk`-prefixed type token. `\b` keeps `myVkFormat` and `VK_NULL_HANDLE` out of the result.
VK_TYPE_RE = re.compile(r"\bVk[A-Z]")
# V5: a concrete backend's name in the SDK's prose. `vsg` is always a finding (the reference backend and the
# library it was built on left the tree on 2026-11-23, and its codes are never reused), and so are a `Vsg`-prefixed
# identifier and a plugin-directory spelling (`gfx_backend_<name>` / `backend_<name>`). `vk` as a WORD is allowed
# only as a diagnostic CODE's prefix (`vk.<category>.<cause>`, wildcards included), because a code is an identity a
# host matches (graphics-architecture.md C4).
BACKEND_NAME_RE = re.compile(
    r"\bvsg\b|\bVsg|gfx_backend_(?:vk|vsg)|backend_(?:vk|vsg)\b|\bvk\b(?!\.[a-z_]+\.[a-z_*]+)"
)
# E1: the escape door, whether it is included with angle brackets or quotes.
ESCAPE_DOOR_RE = re.compile(r'#\s*include\s*[<"][^>"]*support/Handles\.hpp')
# E2: any include that reaches the render seam, whichever spelling (`vine/graphics/...` today).
SEAM_INCLUDE_RE = re.compile(r'#\s*include\s*[<"]vine/graphics/')


def resolve_root(raw: str) -> Path:
    """Turns `--root` into a directory, translating a drive path for a POSIX-emulation interpreter.

    CMake passes `${CMAKE_SOURCE_DIR}` in its own notation, which is a drive path (`I:/projs/Vine`) even when the
    interpreter it found belongs to a Cygwin or MSYS2 installation. A native interpreter resolves that string as-is;
    a Cygwin or MSYS2 one would read it as a relative path and find nothing, which would look like every guarded
    directory had been deleted. The drive form is translated instead of reported as a false finding.
    """
    candidate = Path(raw)
    resolved = candidate.resolve()
    if resolved.is_dir():
        return resolved

    drive = re.match(r"^([A-Za-z]):[\\/](.+)$", raw)
    if drive:
        letter = drive.group(1).lower()
        rest = drive.group(2).replace("\\", "/")
        for translated in (Path(f"/cygdrive/{letter}/{rest}"), Path(f"/{letter}/{rest}")):
            if translated.is_dir():
                return translated.resolve()

    return candidate if candidate.is_dir() else resolved


def files_under(directory: Path) -> list[Path]:
    """Returns every regular file below `directory`, in a stable order."""
    return sorted(path for path in directory.rglob("*") if path.is_file())


def scan(files: list[Path], pattern: re.Pattern[str]) -> list[tuple[Path, int, str]]:
    """Returns the `(path, line number, matching line)` triples of one textual check."""
    findings: list[tuple[Path, int, str]] = []
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), start=1):
            if pattern.search(line):
                findings.append((path, number, line.strip()))
    return findings


def display(root: Path, path: Path) -> str:
    """Returns `path` relative to the repository root, with forward slashes on every platform."""
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Checks the Vulkan exposure boundary of "
        ".ai/research/graphics-2026-11-13-vk-exposure-boundary.md (V2, V4, E1, V5, V6) and the graphics-free "
        "unit boundary of .ai/design/vk-backend.md (E2)."
    )
    parser.add_argument(
        "--root",
        default=str(DEFAULT_ROOT),
        help="Repository root (default: derived from this script's location).",
    )
    args = parser.parse_args()
    root = resolve_root(args.root)

    # V6's pattern is built from the tree: a plugin unit's stem, unless the SDK owns the same stem or the stem is one
    # ordinary word. Sorted and joined once, so the scan stays one regex per file rather than one per unit. The SDK's
    # stems include its subdirectories: `api/ProgramVariant` is the SDK's own spelling even though the plugin keeps a
    # `ProgramVariant` of its own.
    sdk_stems = {path.stem for path in (root / SDK_FACE).rglob("*.hpp")}
    unit_stems = {
        path.stem
        for directory in root.glob(PLUGIN_INTERNAL_DIRS)
        for path in directory.rglob("*.hpp")
        if path.stem not in sdk_stems and len(re.findall(r"[A-Z][a-z0-9]*", path.stem)) >= 2
    }
    plugin_unit_re = re.compile(r"\b(?:" + "|".join(sorted(unit_stems)) + r")\b") if unit_stems else re.compile(r"(?!)")

    # Each check is (label, repo-relative glob, pattern, why a hit is wrong).
    checks = (
        (
            "V2 graphics-sdk-face",
            SDK_FACE,
            VULKAN_INCLUDE_RE,
            "the graphics SDK must not include a render API: every backend and every host inherits it",
        ),
        (
            "V4 plugin-shell",
            PLUGIN_SHELLS,
            VK_TYPE_RE,
            "a plugin shell must name only RenderBackend: its shape is the only part a host may see",
        ),
        (
            "V5 graphics-sdk-prose",
            SDK_FACE,
            BACKEND_NAME_RE,
            "the public SDK states the contract, not the survey (graphics-architecture.md C4): a header names no "
            "concrete backend, and only a diagnostic code's prefix may quote one",
        ),
        (
            "E1 vk-frame-path",
            VK_INTERNALS,
            ESCAPE_DOOR_RE,
            "the vk frame path must not use the escape door it offers; support/Handles.hpp is for tests and tools",
        ),
        (
            "E2 vk-graphics-free",
            VK_GRAPHICS_FREE_UNITS,
            SEAM_INCLUDE_RE,
            "this unit is graphics-free by contract (see .ai/design/vk-backend.md C10): a low-level piece a tool can "
            "use beside the plugin, which one render-seam include would end",
        ),
        (
            "V6 graphics-sdk-plugin-units",
            (SDK_FACE, "src/viz/graphics/shaders"),
            plugin_unit_re,
            "the public SDK states the contract, not a backend's internals (graphics-architecture.md C4): a header names "
            "no unit a plugin keeps in `internal/` - the id's meaning is the contract, the table that answers it is "
            "that plugin's (backlog T30)",
        ),
    )

    total = 0
    counts: list[str] = []
    for label, relative_glob, pattern, reason in checks:
        patterns = relative_glob if isinstance(relative_glob, tuple) else (relative_glob,)
        matched = sorted(path for one in patterns for path in root.glob(one))
        # A check names DIRECTORIES (scanned recursively) or FILES (scanned as they are): E2 is the second kind,
        # because "these units are graphics-free" is a statement about named files, not about a directory.
        files: list[Path] = []
        for target in matched:
            if target.is_dir():
                files.extend(files_under(target))
            elif target.is_file():
                files.append(target)
        if not files:
            first = patterns[0]
            print(f"{display(root, root / first)}: {reason} - nothing to scan (renamed or deleted)")
            print("  update this check with it: a rename must not silence the gate")
            total += 1
            counts.append(f"{label}=0")
            continue
        findings = scan(files, pattern)
        for path, number, line in findings:
            print(f"{display(root, path)}:{number}: {line}")
            print(f"  {reason}")
        total += len(findings)
        counts.append(f"{label}={len(files)}")

    if not unit_stems:
        print(f"{PLUGIN_INTERNAL_DIRS}: no plugin-internal header to derive a unit name from")
        print("  update this check with the tree: a rename must not silence the gate")
        total += 1

    print(f"vk_exposure: {total} finding(s) - scanned {', '.join(counts)} file(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
