#!/usr/bin/env python3
"""Fails when the geometry module's subtrees break their layering.

`geom2d` and `geom3d` live in one module and one CMake target, so the compiler no longer refuses a cross include the way
two separate targets did. This gate is that refusal, and it also keeps the shared `detail` subtree dimension-free
(`../.ai/design/geometry-module.md` 2.5 / C6):

  1. `geom2d` may not include `geom3d`, and `geom3d` may not include `geom2d`;
  2. both may include `geom/detail`, which is where a helper that is identical in both dimensions has one copy;
  3. `geom/detail` may include neither tree;
  4. `geom/detail` may not *name* `Point2` / `Point3` / `Vector2` / `Vector3` - a helper that does is not shared;
  5. a geometry header may only include `vine/geom*`, `vine/math*` or the standard library, and a quoted include must
     stay in its own directory (R2: the only edges are geometry -> geometry, geometry -> math).

Run: python scripts/check_geom_layering.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEOM = ROOT / "src" / "base" / "geom" / "sdk" / "vine" / "geom"

CONTRACTS = ("planar", "spatial")
SHARED_TREE = GEOM / "detail"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*(?P<bracket>[<"])(?P<target>[^>"]+)[>"]')
ALLOWED_ANGLE_ROOTS = ("vine/geom/", "vine/math/")
DIMENSION_NAMES = re.compile(r"\b(Point2|Point3|Vector2|Vector3)\b")

findings: list[str] = []
checked = 0


def includes_of(path: Path) -> list[tuple[int, str, str]]:
    """Returns the `(line number, bracket, normalised include target)` triples of one header."""
    found: list[tuple[int, str, str]] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        match = INCLUDE_RE.match(line)
        if match:
            found.append((number, match.group("bracket"), match.group("target").replace("\\", "/")))
    return found


def geometry_headers() -> list[Path]:
    """Returns every header of the two contracts plus the shared subtree."""
    headers: list[Path] = []
    for contract in CONTRACTS:
        headers.extend(sorted((GEOM / contract).rglob("*.hpp")))
    headers.extend(sorted(SHARED_TREE.rglob("*.hpp")))
    return headers


for path in geometry_headers():
    checked += 1
    tree = next((name for name in CONTRACTS if (GEOM / name) in path.parents), None)

    # Rules 1 and 3: no edge between the two contracts, and no edge out of `detail` into either.
    if tree is not None:
        forbidden = [other for other in CONTRACTS if other != tree]
        culprit = f"geom/{tree}"
    else:
        forbidden = list(CONTRACTS)
        culprit = "geom/detail"

    for number, _bracket, target in includes_of(path):
        for other in forbidden:
            if f"vine/geom/{other}/" in target or target.startswith(f"../{other}/"):
                reason = "a shared helper must not name a dimension" if tree is None else "the contracts must not mix"
                findings.append(f"{path.relative_to(ROOT)}:{number}: {culprit} includes {other}: {target} ({reason})")

        # Rule 5: the include domain. A standard header has no path separator; anything with one must be geometry or
        # math, because those are the only two edges this module is allowed to have.
        if _bracket == "<" and "/" in target and not target.startswith(ALLOWED_ANGLE_ROOTS):
            findings.append(
                f"{path.relative_to(ROOT)}:{number}: include outside geom/ and math/: {target} "
                f"(the only edges are geometry -> geometry and geometry -> math)"
            )
        if _bracket == '"' and ("/" in target or "\\" in target):
            findings.append(
                f"{path.relative_to(ROOT)}:{number}: quoted include leaves its directory: {target} "
                f"(a same-directory include carries no path)"
            )

    # Rule 4: `detail` stays dimension-free by name, not only by include.
    if tree is None:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = DIMENSION_NAMES.search(line)
            if match:
                findings.append(
                    f"{path.relative_to(ROOT)}:{number}: geom/detail names {match.group(1)} "
                    f"(a shared helper that names a dimension is not shared any more)"
                )

for finding in findings:
    print(finding)
print(f"--- {len(findings)} geom-layering finding(s) in {checked} file(s)")
sys.exit(1 if findings else 0)
