#!/usr/bin/env python3
"""Fails when a Vulkan name crosses the exposure boundary the design argument drew.

`.ai/research/graphics-2026-11-13-vk-exposure-boundary.md` §5 splits the tree into four layers, and only
three of them can be broken by accident, because only they have a consumer that would inherit the damage.
This gate is that split made executable:

  1. V2 - the SDK BACKEND face names no backend. `sdk/vine/graphics/backend/*.hpp` is installed as
     `VineGraphicsBackendSdk` and is included by every backend, including the ones that are not Vulkan, so
     one `#include <vulkan/...>` there turns Vulkan into a dependency of the whole backend seam and of the
     install package (§2.3). The face may use Vulkan SEMANTICS - it stores `VkFormat` values in
     `device_color_formats` - but it may not spell the headers.
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

What this gate deliberately does NOT check (§6.2): `internal/` and `support/` may name Vulkan freely, and no
rule here constrains them. There is no consumer to protect - those headers are included only by the plugin
and by tests that already build pipelines and read pixels - and a gate would not remove the Vulkan, it
would force a shadow type (`VkImage` wrapped as `vk::Image`) to get around the check. That is repetition
bought with the appearance of cleanliness, and it would also contradict the vsg plugin, whose `internal/`
has always named Vk types (G6). A rule there would be a cost with no beneficiary (G8).

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

SDK_BACKEND_FACE = "src/viz/graphics/sdk/vine/graphics/backend"
PLUGIN_SHELLS = "src/plugins/*/include/vine/*/shell"
# E1 covers both halves of the plugin's internals: the sources that run a frame, and the headers that describe how.
VK_INTERNALS = (
    "src/plugins/gfx_backend_vk/src/internal",
    "src/plugins/gfx_backend_vk/include/vine/backend_vk/internal",
)

# V2: the three headers a backend face may not include. `<vulkan` also covers `<vulkan.hpp>`; `<volk` and
# `<vsg` are the other two ways the same dependency arrives, and the design names all three.
VULKAN_INCLUDE_RE = re.compile(r"#\s*include\s*<(?:vulkan|volk|vsg)")
# V4: a `Vk`-prefixed type token. `\b` keeps `myVkFormat` and `VK_NULL_HANDLE` out of the result.
VK_TYPE_RE = re.compile(r"\bVk[A-Z]")
# E1: the escape door, whether it is included with angle brackets or quotes.
ESCAPE_DOOR_RE = re.compile(r'#\s*include\s*[<"][^>"]*support/Handles\.hpp')


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
        ".ai/research/graphics-2026-11-13-vk-exposure-boundary.md (V2, V4, E1)."
    )
    parser.add_argument(
        "--root",
        default=str(DEFAULT_ROOT),
        help="Repository root (default: derived from this script's location).",
    )
    args = parser.parse_args()
    root = resolve_root(args.root)

    # Each check is (label, repo-relative glob, pattern, why a hit is wrong).
    checks = (
        (
            "V2 sdk-backend-face",
            SDK_BACKEND_FACE,
            VULKAN_INCLUDE_RE,
            "the SDK backend face must not include a backend: every backend and the install package inherit it",
        ),
        (
            "V4 plugin-shell",
            PLUGIN_SHELLS,
            VK_TYPE_RE,
            "a plugin shell must name only RenderBackend: its shape is the only part a host may see",
        ),
        (
            "E1 vk-frame-path",
            VK_INTERNALS,
            ESCAPE_DOOR_RE,
            "the vk frame path must not use the escape door it offers; support/Handles.hpp is for tests and tools",
        ),
    )

    total = 0
    counts: list[str] = []
    for label, relative_glob, pattern, reason in checks:
        patterns = relative_glob if isinstance(relative_glob, tuple) else (relative_glob,)
        targets = sorted(path for one in patterns for path in root.glob(one) if path.is_dir())
        if not targets:
            print(f"{display(root, root / relative_glob)}: {reason} - the directory is gone")
            print("  update this check with it: a rename must not silence the gate")
            total += 1
            counts.append(f"{label}=0")
            continue

        files: list[Path] = []
        for target in targets:
            files.extend(files_under(target))
        findings = scan(files, pattern)
        for path, number, line in findings:
            print(f"{display(root, path)}:{number}: {line}")
            print(f"  {reason}")
        total += len(findings)
        counts.append(f"{label}={len(files)}")

    print(f"vk_exposure: {total} finding(s) - scanned {', '.join(counts)} file(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
