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
  contentstore  ContentStore.cpp lines and lifetime-token occurrences (six tokens, and
                the same set plus `parked`).
  engine        RenderEngine.hpp lines, public-signature count and the number of files
                that include it.
  sdk           SDK header count (top-level vs. including `backend/`) and `bool name(`
                occurrences in the same two scopes.
  tests         `--gtest_list_tests` case counts and `ctest -N` suite count, when the
                binaries/build tree are present.

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
LIFETIME_TOKENS = re.compile(r"\b(?:markUsed|release_token|release_requested|retired|kept|retirement)\b")
LIFETIME_WITH_PARKED = re.compile(
    r"\b(?:markUsed|release_token|release_requested|retired|kept|retirement|parked)\b"
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


def content_store_counts() -> tuple[int, int, int]:
    """Returns (lines, six-token count, seven-token count)."""
    path = ROOT / "src" / "plugins" / "gfx_backend_vsg" / "src" / "internal" / "ContentStore.cpp"
    text = path.read_text(encoding="utf-8", errors="replace")
    return len(text.splitlines()), len(LIFETIME_TOKENS.findall(text)), len(LIFETIME_WITH_PARKED.findall(text))


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


def gtest_count(binary: Path) -> int | None:
    """Returns the number of gtest cases, or None when the binary is absent/unusable."""
    if not binary.exists():
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
    return sum(1 for line in listed.stdout.splitlines() if line.startswith("  "))


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
    store_lines, lifetime, lifetime_parked = content_store_counts()
    engine_lines, engine_public, engine_includes = engine_counts()
    top_headers, all_headers, top_bools, all_bools = sdk_counts()
    graphics_tests = gtest_count(ROOT / "build" / "bin" / "test_graphics")
    vsg_tests = gtest_count(ROOT / "build" / "bin" / "test_vsg")
    suites = ctest_count()

    print("graphics claims (scope is part of the number)")
    print(f"bridge header+cpp: {paired}")
    print(f"bridge cpp only:   {cpp_only}")
    print(f"ContentStore.cpp:  {store_lines} lines; lifetime tokens {lifetime}; with parked {lifetime_parked}")
    print(f"RenderEngine.hpp:  {engine_lines} lines; public signatures {engine_public}; included by {engine_includes} files")
    print(f"SDK headers:       {top_headers} top-level; {all_headers} including backend/")
    print(f"SDK bool returns:  {top_bools} top-level; {all_bools} including backend/")
    print(f"test_graphics:     {graphics_tests if graphics_tests is not None else 'n/a'}")
    print(f"test_vsg:          {vsg_tests if vsg_tests is not None else 'n/a'}")
    print(f"ctest suites:      {suites if suites is not None else 'n/a'}")
    print()
    print("collection cost (run where the number matters):")
    print("  ./build/bin/test_graphics --gtest_filter='*MeasureWhatOneCollectionCosts*'")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
