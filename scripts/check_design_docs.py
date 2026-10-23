#!/usr/bin/env python3
"""Check `.ai/design/*.md` against the repository's design-doc template.

The template is defined in `.github/skills/design-docs/SKILL.md` and summarised in
`.github/copilot-instructions.md`. This script checks the parts a machine can decide:

  R1  The status / last-verified / scope / source block is at the top.
  R2  The ten chapters are present, in order, with numbered headings.
  R3  Subsections are numbered (`1.1`, `3.2.1`).
  R4  The document is at most 300 lines.
  R5  Chapter titles carry no dates or review-round labels.
  R6  A document with a 功能点 chapter has at least one `F#`; each of C/R/T/N/D
      appears somewhere in the document (the template uses stable IDs even when a
      chapter is short).

Usage:
    python3 scripts/check_design_docs.py [--strict]

Without `--strict`, R4/R6 are warnings; with it, every finding fails.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DESIGN = ROOT / ".ai" / "design"

# The chapter order, with the words that identify each chapter. Keeping the
# accepted spellings narrow is deliberate: a different name is a different
# structure, and the point is that every design doc reads the same way.
CHAPTERS = [
    ("1", ("概要设计", "概要")),
    ("2", ("设计要点",)),
    ("3", ("功能点",)),
    ("4", ("公开接口",)),
    ("5", ("约束", "要求", "不变量")),
    ("6", ("注意事项", "陷阱")),
    ("7", ("已否决", "否决项")),
    ("8", ("测试", "验收")),
    ("9", ("状态", "演进")),
    ("10", ("参考",)),
]

META = ("状态", "最后核对", "范围", "权威来源")
H2 = re.compile(r"^##\s+(\d+)\.\s+(.*)$")
H3 = re.compile(r"^###\s+(\d+)\.(\d+)(?:\.(\d+))?\s+(.*)$")
DATE_OR_ROUND = re.compile(r"20\d\d-\d\d-\d\d|第[一二三四五六七八九十]+轮|本轮")


def design_docs() -> list[pathlib.Path]:
    """Every design document, sorted by name."""
    return sorted(DESIGN.glob("*.md"))


def check(path: pathlib.Path) -> list[str]:
    """Return the findings for one document; the strings start with ERROR/WARN."""
    text = path.read_text(encoding="utf-8", errors="ignore")
    lines = text.splitlines()
    findings: list[str] = []

    head = "\n".join(lines[:30])
    for token in META:
        if token not in head:
            findings.append(f"ERROR {path.name}: missing `{token}` in the top block")

    h2 = []
    in_fence = False
    for number, line in enumerate(lines, start=1):
        if line.strip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = H2.match(line)
        if match:
            h2.append((number, match.group(1), match.group(2).strip()))
    numbers = [item[1] for item in h2]
    expected = [item[0] for item in CHAPTERS]
    if numbers != expected:
        findings.append(
            f"ERROR {path.name}: chapters are {numbers}, expected {expected} (see the 10-chapter template)"
        )
    else:
        for (line_no, _, title), (_, words) in zip(h2, CHAPTERS):
            if not any(word in title for word in words):
                findings.append(
                    f"ERROR {path.name}:{line_no}: chapter `{title}` does not read as one of {words}"
                )

    current_chapter = ""
    in_fence = False
    for number, line in enumerate(lines, start=1):
        if line.strip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        h2_match = H2.match(line)
        if h2_match:
            current_chapter = h2_match.group(1)
            if DATE_OR_ROUND.search(h2_match.group(2)):
                findings.append(f"ERROR {path.name}:{number}: dated/review-round chapter title")
            continue
        if line.startswith("### "):
            match = H3.match(line)
            if not match:
                findings.append(f"ERROR {path.name}:{number}: subsection is not numbered")
            elif match.group(1) != current_chapter:
                findings.append(
                    f"ERROR {path.name}:{number}: subsection {match.group(1)}.{match.group(2)} "
                    f"is under chapter {current_chapter}"
                )

    if len(lines) > 300:
        findings.append(f"WARN  {path.name}: {len(lines)} lines, over the 300-line aim")

    for token, label in (("F", "feature"), ("C", "constraint"), ("R", "requirement"),
                         ("T", "test"), ("N", "note/caveat"), ("D", "rejected option")):
        if not re.search(rf"\b{token}\d+\b", text):
            findings.append(f"WARN  {path.name}: no `{token}#` {label} id")
    return findings


def main() -> int:
    """Print every finding, then a summary. `--strict` turns warnings into failures."""
    strict = "--strict" in sys.argv[1:]
    findings: list[str] = []
    docs = design_docs()
    for doc in docs:
        findings.extend(check(doc))
    errors = [item for item in findings if item.startswith("ERROR")]
    warnings = [item for item in findings if item.startswith("WARN")]
    for item in findings:
        print(item)
    total = len(errors) + (len(warnings) if strict else 0)
    print(f"--- {len(errors)} error(s), {len(warnings)} warning(s), {len(docs)} design doc(s)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
