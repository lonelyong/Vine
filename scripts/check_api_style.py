#!/usr/bin/env python3
"""check_api_style.py — the graphics/appfw public API style gate.

WHY THIS EXISTS. `.ai/research/graphics-2026-11-11-next-api.md` audits the API
set and finds the same concept said twice (P1 `PassDesc` + `RenderPass`, P3 four
spellings of a pass' target, P25 six suffixes for one kind of thing, P27 three
error vocabularies). Those are not one-off mistakes: without a machine check they
come back one header at a time, and every review re-argues them from taste.

THE FOUR RULES (all pure text, all cheap):

  A. A VALUE must not be mutable: a type whose name ends with Desc / Options /
     Settings / Info / Config / Params is a description crossing a boundary, so
     it may have no `set*` method and no `std::function` member (a value that
     cannot be copied, compared or diffed is not a value).
  B. `getXxx` is not a property accessor: Qt-style accessors are spelled
     `xxx()`, and an imperative "ask the user for a value" is a verb, not a
     getter. New `get[A-Z]` names in the SDK are refused.
  C. One error vocabulary: `bool` AND an out-parameter result enum in one
     declaration is two answers to one question.
  D. A `XxxBuilder` produces a value: it must have `build()` returning by value.

BASELINE. Today's tree has known findings (they are the audit's P2 / P27 / P30).
They are listed here so the gate is GREEN today and RED on the next violation —
which is the whole point: the batch that fixes them removes its line.

Usage:
    python3 scripts/check_api_style.py            # check (exit 1 on a new finding)
    python3 scripts/check_api_style.py --list     # check and list every finding
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The audit covers the graphics SDK. appfw's UserIO / ConfigManager carry the same
# `getXxx` habit but they are out of this proposal's scope: widening the gate
# there would fail the build for findings nobody is about to fix.
DIRS = [ROOT / "src" / "viz" / "graphics" / "sdk" / "vine" / "graphics"]

VALUE_SUFFIXES = ("Desc", "Options", "Settings", "Info", "Config", "Params")

TYPE_RE = re.compile(r"\b(?:struct|class)\s+(?:VN_\w+_API\s+)?(\w+)")
SETTER_RE = re.compile(r"\bvoid\s+(set[A-Z]\w*)\s*\(")
FUNCTION_RE = re.compile(r"std::function\s*<")
GETTER_RE = re.compile(r"\b(get[A-Z]\w*)\s*\(")
BOOL_OUT_RE = re.compile(r"\bbool\s+\w+\s*\([^;{}]*\b(\w+Result)\s*\*\s*\w+")
BUILD_RE = re.compile(r"\b(\w+)\s*\(")

# Known findings today (file name, symbol, rule). Each line belongs to a batch of
# the audit's §10; the batch removes it.
BASELINE = {
    ("PassDesc.hpp", "std::function", "A"),        # P2: `before_draw` inside a value; batch A deletes it
    ("PassChain.hpp", "ChainBuilder", "D"),        # P1: a forward declaration of the chain builder (see P1 / batch C)
    ("VisualUserIO.hpp", "getStringAsync", "B"),   # P30: commands, not accessors; rename in batch B
    ("VisualUserIO.hpp", "getIntAsync", "B"),
    ("VisualUserIO.hpp", "getDoubleAsync", "B"),
    ("VisualUserIO.hpp", "getPoint3dAsync", "B"),
    ("VisualUserIO.hpp", "getFileAsync", "B"),
    ("VisualUserIO.hpp", "getFilesAsync", "B"),
    ("RenderDevice.hpp", "ReadbackResult", "C"),   # P27: bool + out-param; becomes Result in P27's batch
}


def body_of(text: str, start: int) -> str:
    """Returns the brace-matched body of the type declaration starting at `start`."""
    open_at = text.find("{", start)
    if open_at < 0:
        return ""
    depth = 0
    for index in range(open_at, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[open_at : index + 1]
    return text[open_at:]


def scan(path: Path) -> list[tuple[str, str, int, str]]:
    """Returns (file, symbol, line, rule) findings for one header."""
    text = path.read_text(encoding="utf-8")
    findings: list[tuple[str, str, int, str]] = []

    for match in TYPE_RE.finditer(text):
        name = match.group(1)
        body = body_of(text, match.end())
        if not body:
            continue
        line = text.count("\n", 0, match.start()) + 1
        if name.endswith(VALUE_SUFFIXES):
            for setter in SETTER_RE.finditer(body):
                findings.append((path.name, setter.group(1), line, "A"))
            if FUNCTION_RE.search(body):
                findings.append((path.name, "std::function", line, "A"))
        if name.endswith("Builder"):
            if not re.search(r"\bbuild\s*\(", body):
                findings.append((path.name, name, line, "D"))

    for match in GETTER_RE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        findings.append((path.name, match.group(1), line, "B"))

    for match in BOOL_OUT_RE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        findings.append((path.name, match.group(1), line, "C"))

    return findings


def main() -> int:
    show_all = "--list" in sys.argv[1:]
    headers = sorted(p for directory in DIRS if directory.exists() for p in directory.glob("*.hpp"))

    new: list[tuple[str, str, int, str]] = []
    known = 0
    for header in headers:
        for finding in scan(header):
            file, symbol, line, rule = finding
            if (file, symbol, rule) in BASELINE:
                known += 1
                if show_all:
                    print(f"  baseline  {file}:{line}  {symbol}  (rule {rule})")
            else:
                new.append(finding)
                print(f"  NEW       {file}:{line}  {symbol}  (rule {rule})")

    print(f"--- {len(new)} new, {known} baseline, {len(headers)} header(s)")
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
