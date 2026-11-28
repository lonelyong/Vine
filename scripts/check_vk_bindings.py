#!/usr/bin/env python3
"""Fails when the GLSL texts and the engine's shadow generator disagree with the ContentAbi binding table.

`.ai/design/vk-backend.md` 2.26 states the rule this gate executes: the engine's shader-ABI binding numbers have ONE
C++ home - `ContentAbi.hpp` - and the GLSL side must agree with it. A mismatch is not a compile error on either side:
the pipeline is created (or, with the validation layer installed, refused at create time), and a wrong binding reads
another resource's descriptor or nothing at all - which on a software rasteriser can still produce a plausible picture.

The four parties that must agree (see ContentAbi's class note): the set layouts and the descriptor writes are built
FROM the constants, the dynamic-offset order is drawn from the same bindings - and the SHADER TEXTS cannot include a
C++ header, so this gate reads them against the table:
  1. `vine_params.glsl` declares the material block, the draw block and the light block at (set, binding) pairs.
  2. the texture-slot samplers (`vn_<slot>Map`) sit at consecutive bindings from the table's first slot, in the
     `TextureSlot` enum's order.
  3. `BuiltinShaders.cpp` generates the shadow pair through `shadowBindings(<set>, <map binding>, ...)`; the engine's
     generator states the block at the map's binding + 1, which the table's own `static_assert` holds it to.

The numbers are COMPARED, never restated here: the table is parsed from `ContentAbi.hpp`, the slot order from
`MaterialValues.hpp`. A table constant that disappears, a block or slot that stops being declared, and a generator call
that vanishes are findings too - a rename must not silently shrink what the gate covers.

Usage: python3 scripts/check_vk_bindings.py [--root <repository root>]
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

TABLE = "src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/ContentAbi.hpp"
SHADERS = "src/viz/graphics/shaders"
MATERIAL_VALUES = "src/viz/graphics/sdk/vine/graphics/MaterialValues.hpp"
BUILTINS = "src/viz/graphics/src/BuiltinShaders.cpp"

CONST_RE = re.compile(r"inline constexpr\s+std::uint32_t\s+(k\w+)\s*=\s*(\d+)U?;")
PARAMS_BLOCK_RE = re.compile(r"layout\(set = (\d+), binding = (\d+), std140\)\s*uniform\s+(Vine\w+)")
SAMPLER_RE = re.compile(r"layout\(set = (\d+), binding = (\d+)\)\s*uniform\s+sampler(?:2D|Cube)\s+(vn_\w+);")
SLOT_ENUM_RE = re.compile(r"enum class TextureSlot\s*\{(.*?)\};", re.S)
SLOT_NAME_RE = re.compile(r"^\s*(\w+),", re.M)
SHADOW_CALL_RE = re.compile(r"shadowBindings\(\s*(\d+)u\s*,\s*(\d+)u")

# Which table pair each block `vine_params.glsl` declares: the material and the lights live in set 0, the draw in set 1.
PARAMS_BLOCKS = {
    "VineMaterialBlock": ("kContentSet", "kMaterialBinding"),
    "VineDrawBlock": ("kDrawSet", "kDrawBinding"),
    "VineLightsBlock": ("kContentSet", "kLightsBinding"),
}

REQUIRED_CONSTANTS = (
    "kContentSet",
    "kMaterialBinding",
    "kTextureBindingBase",
    "kLightsBinding",
    "kShadowMapBinding",
    "kDrawSet",
    "kDrawBinding",
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Checks the GLSL side of the content ABI against ContentAbi.hpp's binding table "
        "(.ai/design/vk-backend.md 2.26)."
    )
    parser.add_argument(
        "--root",
        default=str(DEFAULT_ROOT),
        help="Repository root (default: derived from this script's location).",
    )
    arguments = parser.parse_args()
    root = resolve_root(arguments.root)

    table_path = root / TABLE
    if not table_path.is_file():
        print(f"{TABLE}: the binding table is missing (renamed or deleted)")
        print("  update this check with it: a rename must not silence the gate")
        return 1
    constants = {name: int(value) for name, value in CONST_RE.findall(table_path.read_text(encoding="utf-8"))}
    missing = [name for name in REQUIRED_CONSTANTS if name not in constants]
    for name in missing:
        print(f"{TABLE}: the table no longer states {name}")
        print("  update this check with the table: a renamed constant must not silence the gate")
    if missing:
        return 1  # the checks below would compare against nothing

    findings = 0

    # 1. vine_params.glsl: the three parameter blocks at the table's (set, binding) pairs.
    params_path = root / SHADERS / "vine_params.glsl"
    if not params_path.is_file():
        print(f"{SHADERS}/vine_params.glsl: the parameter blocks' one declaration is missing")
        return 1
    seen_blocks: set[str] = set()
    for number, line in enumerate(params_path.read_text(encoding="utf-8").splitlines(), start=1):
        match = PARAMS_BLOCK_RE.search(line)
        if not match:
            continue
        set_number, binding, block = int(match.group(1)), int(match.group(2)), match.group(3)
        seen_blocks.add(block)
        if block not in PARAMS_BLOCKS:
            continue
        set_name, binding_name = PARAMS_BLOCKS[block]
        if (set_number, binding) != (constants[set_name], constants[binding_name]):
            print(f"{display(root, params_path)}:{number}: {block} is declared at (set {set_number}, binding {binding})")
            print(f"  the table says ({set_name} = {constants[set_name]}, {binding_name} = {constants[binding_name]})")
            findings += 1
    for block in PARAMS_BLOCKS:
        if block not in seen_blocks:
            print(f"{display(root, params_path)}: {block} is no longer declared (renamed or deleted)")
            findings += 1

    # 2. The texture slots: slot S of `TextureSlot` sits at kTextureBindingBase + S, in the enum's order.
    values_path = root / MATERIAL_VALUES
    enum_match = SLOT_ENUM_RE.search(values_path.read_text(encoding="utf-8")) if values_path.is_file() else None
    slots: list[str] = []
    if enum_match is None:
        print(f"{MATERIAL_VALUES}: the TextureSlot enum is missing (renamed or deleted)")
        findings += 1
    else:
        slots = SLOT_NAME_RE.findall(enum_match.group(1))
    seen_slots: set[str] = set()
    shader_dir = root / SHADERS
    shader_files = [path for path in files_under(shader_dir) if path.suffix in {".glsl", ".frag", ".vert"}] if shader_dir.is_dir() else []
    if not shader_files:
        print(f"{SHADERS}: nothing to scan (renamed or deleted)")
        findings += 1
    for path in shader_files:
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
            match = SAMPLER_RE.search(line)
            if not match:
                continue
            set_number, binding, sampler = int(match.group(1)), int(match.group(2)), match.group(3)
            for index, slot in enumerate(slots):
                if sampler != "vn_" + slot[0].lower() + slot[1:] + "Map":
                    continue
                seen_slots.add(slot)
                expected_set = constants["kContentSet"]
                expected_binding = constants["kTextureBindingBase"] + index
                if (set_number, binding) != (expected_set, expected_binding):
                    print(f"{display(root, path)}:{number}: {sampler} is declared at (set {set_number}, binding {binding})")
                    print(
                        f"  the table says slot {index} ({slot}) at "
                        f"(kContentSet = {expected_set}, kTextureBindingBase + {index} = {expected_binding})"
                    )
                    findings += 1
    for slot in slots:
        if slot not in seen_slots:
            print(f"{SHADERS}: no sampler declaration matches slot {slot} (vn_<slot>Map)")
            findings += 1

    # 3. The engine's shadow generator: the map at the table's binding; the block follows at + 1.
    builtins_path = root / BUILTINS
    if not builtins_path.is_file():
        print(f"{BUILTINS}: the shadow generator is missing (renamed or deleted)")
        findings += 1
    else:
        calls = 0
        for number, line in enumerate(builtins_path.read_text(encoding="utf-8").splitlines(), start=1):
            match = SHADOW_CALL_RE.search(line)
            if not match:
                continue
            calls += 1
            set_number, binding = int(match.group(1)), int(match.group(2))
            if (set_number, binding) != (constants["kContentSet"], constants["kShadowMapBinding"]):
                print(f"{display(root, builtins_path)}:{number}: shadowBindings({set_number}u, {binding}u, ...)")
                print(
                    f"  the table says (kContentSet = {constants['kContentSet']}, "
                    f"kShadowMapBinding = {constants['kShadowMapBinding']})"
                )
                findings += 1
        if calls == 0:
            print(f"{BUILTINS}: no shadowBindings(...) call found (renamed or deleted)")
            findings += 1

    print(f"vk_bindings: {findings} finding(s) - checked {len(constants)} table constant(s), {len(slots)} texture slot(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
