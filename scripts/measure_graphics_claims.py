#!/usr/bin/env python3
"""measure_graphics_claims.py — reproduce the numbers quoted by the graphics-next proposals.

WHY THIS EXISTS. The six proposal documents and their audit quote line counts, header
counts, token counts and test counts. Those numbers decide `K1`/`K5`/`K11`, so a reader
must be able to re-derive them instead of trusting a number that drifted since the
paragraph was written. This script is the shared measuring stick: it prints the count,
the exact scope and the command it used.

WHAT IT MEASURES

  bridge        ContentMirror / DeltaChannel / DrawListMemo / PublicationStore /
                FrameExpander / FrameRunner / SceneRows / EngineFrame / DrawListBake:
                .hpp + .cpp, and cpp-only.
  engine        RenderEngine.hpp lines, public-signature count and the number of files
                that include it.
  sdk           SDK header count (top-level vs. including `backend/`) and `bool name(`
                occurrences in the same two scopes.
  tests         `--gtest_list_tests` case and suite counts (`test_graphics`, `test_vk`)
                and the `ctest -N` suite count, when the binaries/build tree are present;
                a single-config and a multi-config tree are both searched.

Usage:
    python3 scripts/measure_graphics_claims.py

The script never fails the build: it is a measuring tool, not a gate. A missing test
binary is reported as `n/a`, not as an error.
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GRAPHICS = ROOT / "src" / "viz" / "graphics"
SDK = GRAPHICS / "sdk" / "vine" / "graphics"
BRIDGE = (
    "ContentMirror",
    "DeltaChannel",
    "DrawListMemo",
    "PublicationStore",
    "FrameExpander",
    "FrameRunner",
    "SceneRows",
    "EngineFrame",
    "DrawListBake",
)
BOOL_RETURN = re.compile(r"\bbool\s+[A-Za-z_][A-Za-z0-9_]*\s*\(")
PUBLIC_SIGNATURE = re.compile(r"^    [^ /].*\(", re.MULTILINE)
INCLUDE_ENGINE = re.compile(r"^\s*#\s*include\s*[<\"].*RenderEngine\.hpp[>\"]", re.MULTILINE)


def line_count(path: Path) -> int:
    """Returns the line count, or 0 when the file is absent."""
    try:
        return len(path.read_text(encoding="utf-8", errors="replace").splitlines())
    except FileNotFoundError:
        return 0


def bridge_counts() -> tuple[int, int]:
    """Returns (header+cpp, cpp-only) lines across the bridge files."""
    paired = 0
    cpp_only = 0
    for stem in BRIDGE:
        header = GRAPHICS / "src" / f"{stem}.hpp"
        source = GRAPHICS / "src" / f"{stem}.cpp"
        paired += line_count(header) + line_count(source)
        cpp_only += line_count(source)
    return paired, cpp_only


def public_signature_count(text: str) -> int:
    """Counts signatures in RenderEngine's public sections (class body only)."""
    in_class = False
    public = False
    count = 0
    for line in text.splitlines():
        if "class VN_GRAPHICS_API RenderEngine" in line:
            in_class = True
            continue
        if not in_class:
            continue
        stripped = line.strip()
        if line.startswith("};"):
            break
        if stripped in {"public:", "private:", "protected:"}:
            public = stripped == "public:"
            continue
        if public and PUBLIC_SIGNATURE.match(line):
            count += 1
    return count


def engine_counts() -> tuple[int, int, int]:
    """Returns (lines, public signatures, files including the header)."""
    path = SDK / "RenderEngine.hpp"
    text = path.read_text(encoding="utf-8", errors="replace")
    includes = 0
    for candidate in list((ROOT / "src").rglob("*")) + list((ROOT / "tests").rglob("*")):
        if candidate.is_file() and candidate.suffix in {".hpp", ".h", ".cpp"}:
            try:
                body = candidate.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if INCLUDE_ENGINE.search(body):
                includes += 1
    return len(text.splitlines()), public_signature_count(text), includes


def sdk_counts() -> tuple[int, int, int, int]:
    """Returns (top-level headers, all headers, top-level bool returns, all bool returns)."""
    top = sorted(SDK.glob("*.hpp"))
    all_headers = sorted(SDK.rglob("*.hpp"))

    def bools(paths: list[Path]) -> int:
        return sum(len(BOOL_RETURN.findall(path.read_text(encoding="utf-8", errors="replace"))) for path in paths)

    return len(top), len(all_headers), bools(top), bools(all_headers)


def test_binary(name: str) -> Path | None:
    """Returns the NEWEST test binary across a single-config and a multi-config build tree."""
    bin_dir = ROOT / "build" / "bin"
    candidates = [bin_dir / name, bin_dir / f"{name}.exe"]
    for config in ("Debug", "RelWithDebInfo", "Release", "MinSizeRel"):
        candidates += [bin_dir / config / name, bin_dir / config / f"{name}.exe"]
    existing = [candidate for candidate in candidates if candidate.exists()]
    return max(existing, key=lambda path: path.stat().st_mtime) if existing else None


def gtest_counts(binary: Path | None) -> tuple[int, int] | None:
    """Returns (cases, suites) from `--gtest_list_tests`, or None when the binary is absent/unusable."""
    if binary is None:
        return None
    try:
        listed = subprocess.run(
            [str(binary), "--gtest_list_tests"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return None
    cases = 0
    suites = 0
    for line in listed.stdout.splitlines():
        if line.startswith("  ") and line.strip():
            cases += 1
        elif line.strip() and not line.startswith(("Running main", "Note:", "[", "WARNING")):
            suites += 1
    return cases, suites


def suite_line(raw: tuple[int, int] | None) -> str:
    """Renders a count pair the way the documents quote it."""
    return f"{raw[0]} cases / {raw[1]} suites" if raw is not None else "n/a"


def ctest_count() -> int | None:
    """Returns the number of CTest suites, or None when the build tree is absent."""
    build = ROOT / "build"
    if not (build / "CTestTestfile.cmake").exists():
        return None
    try:
        listed = subprocess.run(
            ["ctest", "--test-dir", str(build), "-N"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return None
    match = re.search(r"Total Tests:\s*(\d+)", listed.stdout)
    return int(match.group(1)) if match else None


def main() -> int:
    """Prints every claim with its scope; always returns 0."""
    paired, cpp_only = bridge_counts()
    engine_lines, engine_public, engine_includes = engine_counts()
    top_headers, all_headers, top_bools, all_bools = sdk_counts()
    suites = ctest_count()

    print("graphics claims (scope is part of the number)")
    print(f"bridge header+cpp: {paired}")
    print(f"bridge cpp only:   {cpp_only}")
    print(f"RenderEngine.hpp:  {engine_lines} lines; public signatures {engine_public}; included by {engine_includes} files")
    print(f"SDK headers:       {top_headers} top-level; {all_headers} including backend/")
    print(f"SDK bool returns:  {top_bools} top-level; {all_bools} including backend/")
    print()
    print("test suites (the newest binary per target - a count belongs to its build)")
    for name in ("test_graphics", "test_vk"):
        binary = test_binary(name)
        print(f"  {name + ':':<15} {suite_line(gtest_counts(binary)):<24} {binary if binary is not None else ''}")
    print(f"  {'ctest suites:':<15} {suites if suites is not None else 'n/a'}")
    print()
    print("collection cost (run where the number matters):")
    print(f"  {test_binary('test_graphics') or './build/bin/test_graphics'} --gtest_filter='*MeasureWhatOneCollectionCosts*'")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
