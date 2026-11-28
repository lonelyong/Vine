#!/usr/bin/env python3
"""Fails when a Vulkan layout is spelled outside the one table that owns the spelling.

`.ai/design/vk-backend.md` 2.7 states the rule this gate executes: in this backend a layout has ONE answer, and
every command that has to be told where an image is reads it from the barrier table (`BarrierPlan::stateOf` /
`transferReadState()` / `transferWriteState()`, all `constexpr`). Before the sweep of 2026-11-28 the same fact was
also spelled as a raw `VK_IMAGE_LAYOUT_*` constant at every consumer - the rendering instance's `imageLayout` and
`resolveImageLayout`, a descriptor's `imageLayout`, a copy's layout and an image's creation `initialLayout` - so a
row and its consumer could disagree, and the barrier before a command would leave the image where that command did
not expect it (a VUID at submit time at best, a half-defined pass at worst).

Why a textual check rather than a type: the consumers are plain Vulkan structures the driver reads, and a shadow
type around `VkImageLayout` would not remove the value, only hide the constant behind an accessor - repetition
bought with the appearance of cleanliness (`G8`). What protects the rule is that ONE file may name the constants:
a new consumer has to go through the table, which is where the row gets decided.

Scope: `src/plugins/gfx_backend_vk/**` (recursive). The table's own two files are the exception by definition:
`BarrierPlan.cpp` states the rows and `BarrierPlan.hpp` states the `ImageState` default. Tests are outside the scan
on purpose - `tests/test_vk/BarrierPlanTest.cpp` asserts the table's answers, and a test that could not spell the
expected value could only repeat the implementation.

Usage: python3 scripts/check_vk_layout_literals.py [--root <repository root>]
Exit status: 1 on findings, 0 when clean.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The root translation and the file scan are the gate corpus' own (`check_vk_exposure.py` documents why a drive path
# needs translating for a Cygwin/MSYS2 interpreter); one spelling of each, imported rather than forked.
from check_vk_exposure import DEFAULT_ROOT, display, files_under, resolve_root

PLUGIN = "src/plugins/gfx_backend_vk"
# The two files that own the spelling: the rows (`stateOf` / the transfer states) and the layout record's default.
TABLE_FILES = {
    f"{PLUGIN}/include/vine/backend_vk/internal/BarrierPlan.hpp",
    f"{PLUGIN}/src/internal/BarrierPlan.cpp",
}
LAYOUT_RE = re.compile(r"\bVK_IMAGE_LAYOUT_[A-Z0-9_]+")

# Every layout the table must still state. A missing one means the scan stopped reading the table (renamed, emptied),
# and the gate would then pass while checking nothing that matters - so the set is checked, not assumed.
EXPECTED_LAYOUTS = {
    "VK_IMAGE_LAYOUT_UNDEFINED",
    "VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL",
    "VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL",
    "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL",
    "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR",
    "VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL",
    "VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL",
}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Checks that src/plugins/gfx_backend_vk names VK_IMAGE_LAYOUT_* only in the BarrierPlan table "
        "(.ai/design/vk-backend.md 2.7)."
    )
    parser.add_argument(
        "--root",
        default=str(DEFAULT_ROOT),
        help="Repository root (default: derived from this script's location).",
    )
    arguments = parser.parse_args()
    root = resolve_root(arguments.root)

    plugin = root / PLUGIN
    files = [path for path in files_under(plugin) if path.suffix in {".hpp", ".cpp", ".h"}] if plugin.is_dir() else []
    if not files:
        print(f"{PLUGIN}: nothing to scan (renamed or deleted)")
        print("  update this check with it: a rename must not silence the gate")
        return 1

    total = 0
    table_layouts: set[str] = set()
    for path in files:
        relative = display(root, path)
        text = path.read_text(encoding="utf-8", errors="replace")
        names = set(LAYOUT_RE.findall(text))
        if relative in TABLE_FILES:
            table_layouts |= names
            continue
        for number, line in enumerate(text.splitlines(), start=1):
            if LAYOUT_RE.search(line):
                print(f"{relative}:{number}: {line.strip()}")
                print(
                    "  a layout is the barrier table's answer, not a second spelling: read the row "
                    "(stateOf(...).layout, transferReadState(), transferWriteState())"
                )
                total += 1

    missing = sorted(EXPECTED_LAYOUTS - table_layouts)
    if missing:
        print(f"{', '.join(sorted(TABLE_FILES))}: the table no longer states {', '.join(missing)}")
        print("  update this check with the table: the rows moving must not silence the gate")
        total += len(missing)

    print(f"vk_layout_literals: {total} finding(s) - scanned {len(files)} file(s), {len(table_layouts)} table layout(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
