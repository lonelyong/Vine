#!/usr/bin/env python3
"""Checks that the vsg backend's docs only name units that exist, and name every unit.

The module keeps three living documents (``gfx_backend_vsg.md``, ``docs/backend.md``,
``docs/data-flow.md``) beside the code they describe, and they rot in one direction: a unit is
renamed or deleted and the sentence that named it survives. That is not compile-checked, and it
is not visible in review, so it accumulates until a reader follows a pointer that leads nowhere
(``drawScreenTexture`` outlived the function by two weeks; ``renderOffscreenTarget`` and
``window_layers`` described a model that had been replaced).

Only the two claims a script can decide EXACTLY are gated:

  1. A ``Foo.hpp`` / ``Foo.cpp`` the doc names must exist. Prose can be wrong in ways a regex
     cannot judge (a sentence may name a symbol that is deliberately absent, or a foreign type),
     so only UNIT files are checked — a renamed or deleted unit is exactly the drift that hurts a
     reader following the module map.
  2. Every unit under ``src/`` and ``include/vine/vsg/`` must be named somewhere in the three
     docs. The map is part of what those documents are for; a unit nobody documented is a hole.

What is skipped, on purpose:
  * fenced code blocks (``\\`\\`\\``` ...) — a snippet is not a claim about the module's files;
  * sections whose heading is marked as history (``历史登记`` / ``不再更新``) — they record what
    the code USED to be, and deleted units are their point;
  * a line carrying ``<!-- drift-ok -->`` — the escape hatch for a sentence that must name
    something absent.

Exit code 1 with a list of findings, 0 when the docs agree with the tree.

Usage:
    python3 scripts/check_doc_symbols.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "src" / "plugins" / "gfx_backend_vsg"
DOCS = [
    PLUGIN / "gfx_backend_vsg.md",
    PLUGIN / "docs" / "backend.md",
    PLUGIN / "docs" / "data-flow.md",
]
# Directories whose units the docs are expected to map.
MAPPED_DIRS = [PLUGIN / "src", PLUGIN / "include" / "vine" / "vsg"]
# A unit mention is checked only when it names THIS plugin: the docs legitimately name vsg's own
# units (vsg::Builder) and the SDK's (RenderBackend.hpp), which live outside this tree.
PLUGIN_UNIT_PREFIXES = ("Vsg", "SceneBridge", "CameraBridge", "OwnedCache", "GfxBackendVsg", "vsg_global")
FROZEN_SECTION_MARKERS = ("历史登记", "不再更新", "历史）")
# An HTML comment, so that a sentence DESCRIBING the escape hatch (and therefore mentioning its
# name in backticks) cannot exempt itself the way a bare marker word would.
DRIFT_OK_MARKER = "<!-- drift-ok -->"

UNIT_RE = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*\.(?:hpp|cpp))`")
FENCE_RE = re.compile(r"^\s*(?:```|~~~)")


def units_of(directory: Path) -> set[str]:
    """Names of the unit files directly under @p directory."""
    if not directory.is_dir():
        return set()
    return {entry.name for entry in directory.iterdir() if entry.suffix in (".hpp", ".cpp")}


def check_doc(path: Path, unit_names: set[str], text: str) -> list[str]:
    """Return one finding per unit the doc names but the tree does not have."""
    findings: list[str] = []
    # A frozen section stays frozen for its SUB-sections too: the history tables carry their own
    # headings (### 13.5 …), and those must not un-freeze the section they belong to.
    frozen_level = 0
    in_fence = False
    for number, line in enumerate(text.splitlines(), start=1):
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        heading = re.match(r"^(#{1,6})\s", line)
        if heading:
            level = len(heading.group(1))
            if level <= frozen_level:
                frozen_level = 0
            if any(marker in line for marker in FROZEN_SECTION_MARKERS):
                frozen_level = level
            continue
        if frozen_level != 0 or DRIFT_OK_MARKER in line:
            continue
        for mentioned in UNIT_RE.findall(line):
            if not mentioned.startswith(PLUGIN_UNIT_PREFIXES):
                continue  # a foreign unit (vsg's, the SDK's): not this tree's business
            if mentioned not in unit_names:
                findings.append(f"{path.name}:{number}: no such unit: `{mentioned}`")
    return findings


def print_doc_texts() -> str:
    """Concatenation of the living docs, for the coverage rule."""
    return "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in DOCS)


def main() -> int:
    findings: list[str] = []
    missing_docs = [path for path in DOCS if not path.is_file()]
    if missing_docs:
        for path in missing_docs:
            print(f"doc drift: missing document: {path.relative_to(ROOT)}")
        return 1

    unit_names: set[str] = set()
    for directory in MAPPED_DIRS:
        unit_names |= units_of(directory)

    for doc in DOCS:
        findings.extend(check_doc(doc, unit_names, doc.read_text(encoding="utf-8", errors="replace")))

    # Rule 2: the map has to cover the tree — a unit nobody documented is a hole in it.
    all_text = print_doc_texts()
    for unit in sorted(unit_names):
        if re.search(rf"\b{re.escape(Path(unit).stem)}\b", all_text) is None:
            findings.append(f"{unit}: no living document names this unit")

    if findings:
        print("doc drift: the living documents and the tree disagree:")
        for finding in findings:
            print(f"  {finding}")
        print(f"\n{len(findings)} finding(s); fix the doc (or add the escape-hatch comment if the line"
              " must name something absent).")
        return 1
    print(f"OK — {len(DOCS)} living document(s) and {len(unit_names)} unit(s) agree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
